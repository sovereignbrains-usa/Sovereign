#include "pipe_server.h"

#include <sddl.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <array>

#include "pipe_access.h"
#include "protocol.h"

namespace sovereign::service {

namespace {

// Служба создаёт пайп под SYSTEM, а будущий трей подключается из обычной
// (неэлевированной) пользовательской сессии — CreateNamedPipeW с
// nullptr-дескриптором безопасности берёт DACL по умолчанию из токена
// создающего процесса, который не включает SID сессии другого контекста
// того же пользователя (split-token). Без явного ACL клиент получает
// "Access to the path is denied" на ConnectNamedPipe/CreateFile — не
// баг клиента, а отсутствие ACL на сервере. SY/BA/IU = SYSTEM/Built-in
// Administrators/Interactive (those signed in at this computer - the tray's
// users; not network logons, service accounts and scheduled tasks, which
// "Authenticated Users" let in too). Read and write, not "all": a client
// has no business changing the pipe's own DACL. Who of those may actually
// give commands is IsAllowedPipeClient's call (pipe_access.h).
wil::unique_hlocal_ptr<SECURITY_DESCRIPTOR> CreatePipeSecurityDescriptor() {
  PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
  THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GRGW;;;SY)(A;;GRGW;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &rawDescriptor,
      nullptr));
  return wil::unique_hlocal_ptr<SECURITY_DESCRIPTOR>(
      static_cast<SECURITY_DESCRIPTOR*>(rawDescriptor));
}

wil::unique_hfile CreatePipeInstance(const std::wstring& pipeName) {
  const auto securityDescriptor = CreatePipeSecurityDescriptor();
  SECURITY_ATTRIBUTES securityAttributes{sizeof(SECURITY_ATTRIBUTES),
                                          securityDescriptor.get(), FALSE};

  wil::unique_hfile pipe(CreateNamedPipeW(
      pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      // REJECT_REMOTE_CLIENTS: without it \\computer\pipe\sovereign-control
      // opens over SMB for anyone the DACL names.
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1,  // nMaxInstances: один инстанс, обрабатываем клиентов по очереди
      sovereign::ipc::kPipeBufferSize, sovereign::ipc::kPipeBufferSize, 0,
      &securityAttributes));
  THROW_LAST_ERROR_IF(!pipe);
  return pipe;
}

// Ждёт подключения клиента; возвращает false, если сервер должен остановиться.
bool WaitForClient(HANDLE pipe, HANDLE stopEvent) {
  OVERLAPPED overlapped{};
  wil::unique_event_nothrow connectEvent;
  THROW_IF_FAILED(connectEvent.create(wil::EventOptions::ManualReset));
  overlapped.hEvent = connectEvent.get();

  if (ConnectNamedPipe(pipe, &overlapped)) {
    return true;  // клиент успел подключиться синхронно
  }

  const DWORD error = GetLastError();
  if (error == ERROR_PIPE_CONNECTED) {
    return true;
  }
  THROW_LAST_ERROR_IF(error != ERROR_IO_PENDING);

  const std::array<HANDLE, 2> waitHandles{stopEvent, connectEvent.get()};
  const DWORD waitResult =
      WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()),
                              waitHandles.data(), FALSE, INFINITE);
  if (waitResult == WAIT_OBJECT_0) {
    CancelIoEx(pipe, &overlapped);
    return false;
  }

  DWORD bytesTransferred = 0;
  THROW_LAST_ERROR_IF(
      !GetOverlappedResult(pipe, &overlapped, &bytesTransferred, FALSE));
  return true;
}

// Who is on the other end: its executable and whether its token is elevated.
// What can't be read counts as "no": an image that isn't there is no tray, a
// token that can't be opened isn't proof of an administrator.
PipeClient QueryPipeClient(HANDLE pipe, ULONG* processId) {
  PipeClient client;
  *processId = 0;
  if (!GetNamedPipeClientProcessId(pipe, processId)) {
    return client;
  }
  // The handle keeps the process object, and so its id, from being reused
  // while we look at it.
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *processId));
  if (!process) {
    return client;
  }
  std::wstring image(32768, L'\0');
  DWORD size = static_cast<DWORD>(image.size());
  if (QueryFullProcessImageNameW(process.get(), 0, image.data(), &size)) {
    image.resize(size);
    client.image = std::move(image);
  }
  wil::unique_handle token;
  TOKEN_ELEVATION elevation{};
  DWORD returned = 0;
  if (OpenProcessToken(process.get(), TOKEN_QUERY, &token) &&
      GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof elevation, &returned)) {
    client.elevated = elevation.TokenIsElevated != 0;
  }
  return client;
}

// Reads one whole pipe message. In message mode a message longer than the
// read buffer does not arrive in one ReadFile: the read completes with
// ERROR_MORE_DATA and the rest has to be read on — without this loop any
// request over kPipeBufferSize (a real sing-box config easily is) failed and
// the client just saw the connection close.
std::string ReadMessage(HANDLE pipe) {
  std::array<char, sovereign::ipc::kPipeBufferSize> buffer{};
  std::string message;
  for (;;) {
    OVERLAPPED overlapped{};
    wil::unique_event_nothrow ioEvent;
    THROW_IF_FAILED(ioEvent.create(wil::EventOptions::ManualReset));
    overlapped.hEvent = ioEvent.get();

    if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped)) {
      const DWORD error = GetLastError();
      THROW_LAST_ERROR_IF(error != ERROR_IO_PENDING && error != ERROR_MORE_DATA);
    }
    DWORD bytesRead = 0;
    const BOOL complete = GetOverlappedResult(pipe, &overlapped, &bytesRead, TRUE);
    const DWORD resultError = complete ? ERROR_SUCCESS : GetLastError();
    message.append(buffer.data(), bytesRead);
    if (complete) {
      return message;
    }
    THROW_WIN32_IF(resultError, resultError != ERROR_MORE_DATA);
    THROW_WIN32_IF(ERROR_MESSAGE_EXCEEDS_MAX_SIZE,
                   message.size() > sovereign::ipc::kMaxMessageBytes);
  }
}

void WriteMessage(HANDLE pipe, const std::string& message) {
  OVERLAPPED overlapped{};
  wil::unique_event_nothrow ioEvent;
  THROW_IF_FAILED(ioEvent.create(wil::EventOptions::ManualReset));
  overlapped.hEvent = ioEvent.get();

  const BOOL immediate =
      WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()),
                nullptr, &overlapped);
  DWORD bytesWritten = 0;
  if (!immediate) {
    THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
  }
  THROW_LAST_ERROR_IF(
      !GetOverlappedResult(pipe, &overlapped, &bytesWritten, TRUE));
}

}  // namespace

PipeServer::PipeServer(std::wstring pipeName, RequestHandler handler, std::optional<std::wstring> trustedClientImage)
    : pipeName_(std::move(pipeName)),
      handler_(std::move(handler)),
      trustedClientImage_(std::move(trustedClientImage)) {}

void PipeServer::Run(const std::stop_token& stopToken) {
  wil::unique_event_nothrow stopEvent;
  THROW_IF_FAILED(stopEvent.create(wil::EventOptions::ManualReset));

  std::stop_callback signalStop(stopToken,
                                 [&] { SetEvent(stopEvent.get()); });

  wil::unique_hfile pipe = CreatePipeInstance(pipeName_);

  while (!stopToken.stop_requested()) {
    if (!WaitForClient(pipe.get(), stopEvent.get())) {
      break;
    }

    // Before reading a byte: a client that isn't allowed doesn't get to hold
    // the single instance with a half-sent message either.
    ULONG clientProcess = 0;
    if (!IsAllowedPipeClient(QueryPipeClient(pipe.get(), &clientProcess), trustedClientImage_)) {
      LOG_WIN32_MSG(ERROR_ACCESS_DENIED, "pipe client %lu refused: not the tray, not elevated", clientProcess);
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    try {
      const std::string request = ReadMessage(pipe.get());
      const std::string response = handler_(request);
      WriteMessage(pipe.get(), response);
      // WriteFile-overlapped завершается, как только данные попали в буфер
      // пайпа, а не когда клиент их забрал. Без этого FlushFileBuffers
      // DisconnectNamedPipe ниже может оборвать соединение раньше, чем
      // клиент успеет прочитать ответ (клиент увидит EOF/0 байт).
      FlushFileBuffers(pipe.get());
    } catch (...) {
      // One bad client must not take the server down; the instance is reset
      // below by DisconnectNamedPipe. WIL reports it to ETW (trace.h, keyword
      // kFailures) with the HRESULT - there is no console under SCM.
      LOG_CAUGHT_EXCEPTION_MSG("pipe client request failed");
    }

    DisconnectNamedPipe(pipe.get());
  }
}

}  // namespace sovereign::service

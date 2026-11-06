#include "ssh_run.h"

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "ssh_probe.h"

namespace sovereign::tray {

namespace {

constexpr std::size_t kMaxOutput = 64 * 1024;

// Reads a pipe to its end on a thread of its own: two full pipes would stop ssh.
void Drain(HANDLE pipe, std::string& into) {
  char buffer[4096];
  DWORD got = 0;
  while (ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) && got > 0) {
    if (into.size() < kMaxOutput) {
      into.append(buffer, std::min<std::size_t>(got, kMaxOutput - into.size()));
    }
  }
}

std::wstring SshPath() {
  wchar_t system[MAX_PATH];
  const UINT n = GetSystemDirectoryW(system, MAX_PATH);
  return n > 0 && n < MAX_PATH ? std::wstring(system, n) + L"\\OpenSSH\\ssh.exe" : std::wstring(L"ssh.exe");
}

}  // namespace

SshResult RunSsh(const OwnServer& server, std::string_view identityFile, std::string_view remote,
                 int timeoutSeconds) {
  SshResult result;
  const std::vector<std::string> args = SshArguments(server, identityFile, remote);
  if (args.empty()) {
    return result;
  }
  SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
  wil::unique_handle outRead;
  wil::unique_handle outWrite;
  wil::unique_handle errRead;
  wil::unique_handle errWrite;
  wil::unique_handle nul;
  if (!CreatePipe(outRead.put(), outWrite.put(), &inherit, 0) || !CreatePipe(errRead.put(), errWrite.put(), &inherit, 0)) {
    return result;
  }
  SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0);
  // stdin on NUL: nothing for ssh to ask, and it can't wait on one.
  nul.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nul.get();
  startup.hStdOutput = outWrite.get();
  startup.hStdError = errWrite.get();
  const std::wstring exe = SshPath();
  std::wstring line = BuildCommandLine(exe, args);
  wil::unique_process_information process;
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    return result;
  }
  result.started = true;
  outWrite.reset();  // ours closed: the reads end when ssh's do
  errWrite.reset();
  std::thread out([&] { Drain(outRead.get(), result.output); });
  std::thread err([&] { Drain(errRead.get(), result.error); });
  if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(std::max(1, timeoutSeconds)) * 1000) == WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess, 255);
    WaitForSingleObject(process.hProcess, 5000);
    result.timedOut = true;
  }
  out.join();
  err.join();
  DWORD code = 0;
  if (GetExitCodeProcess(process.hProcess, &code)) {
    result.exitCode = static_cast<int>(code);
  }
  return result;
}

std::string SshProblem(const SshResult& result) {
  if (!result.started) {
    return "Не удалось запустить ssh.exe (в Windows он ставится как «Клиент OpenSSH») или адрес не подходит";
  }
  if (result.exitCode == 0 && !result.timedOut) {
    return {};
  }
  return SshFailure(result.error, result.exitCode, result.timedOut);
}

}  // namespace sovereign::tray

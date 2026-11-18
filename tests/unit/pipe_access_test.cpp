// Who the service takes requests from on its control pipe (src/service/pipe_access.h):
// the decision as such, and then a real PipeServer on a pipe of its own - a client
// of the trusted image is served, a stranger is cut off before it is read (as long
// as this test doesn't run elevated: an elevated client is served by design).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wil/resource.h>

#include <atomic>
#include <exception>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "check.h"
#include "pipe_access.h"
#include "pipe_server.h"

namespace {

using sovereign::service::IsAllowedPipeClient;
using sovereign::service::PipeClient;
using sovereign::service::PipeServer;

constexpr wchar_t kTray[] = LR"(C:\Program Files\Sovereign\sovereign-tray.exe)";

void TestDecision() {
  const std::wstring tray = kTray;
  const PipeClient trayClient{tray, false};
  const PipeClient trayInOtherCase{LR"(c:\program files\sovereign\SOVEREIGN-TRAY.EXE)", false};
  const PipeClient stranger{LR"(C:\Users\user\Downloads\evil.exe)", false};
  const PipeClient elevated{LR"(C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe)", true};
  const PipeClient unknown{};  // image and token unreadable

  CHECK(IsAllowedPipeClient(trayClient, tray));
  CHECK(IsAllowedPipeClient(trayInOtherCase, tray));
  CHECK(!IsAllowedPipeClient(stranger, tray));
  CHECK(!IsAllowedPipeClient(unknown, tray));
  CHECK(IsAllowedPipeClient(elevated, tray));
  // The same folder under another name, a different drive, a longer path: not the tray.
  CHECK(!IsAllowedPipeClient({LR"(C:\Program Files\Sovereign\sovereign-tray.exe.bak)", false}, tray));
  CHECK(!IsAllowedPipeClient({LR"(D:\Program Files\Sovereign\sovereign-tray.exe)", false}, tray));
  CHECK(!IsAllowedPipeClient({LR"(C:\Program Files\Sovereign\sovereign-core.exe)", false}, tray));

  // A service that can't tell its own path trusts no program, only elevated ones.
  const std::optional<std::wstring> noPath = std::wstring();
  CHECK(!IsAllowedPipeClient(unknown, noPath));
  CHECK(!IsAllowedPipeClient(stranger, noPath));
  CHECK(IsAllowedPipeClient(elevated, noPath));

  // Console mode for development: no restriction beyond the DACL.
  CHECK(IsAllowedPipeClient(stranger, std::nullopt));
  CHECK(IsAllowedPipeClient(unknown, std::nullopt));
}

std::wstring OwnImage() {
  std::wstring path(32768, L'\0');
  path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
  return path;
}

bool OwnTokenElevated() {
  wil::unique_handle token;
  TOKEN_ELEVATION elevation{};
  DWORD returned = 0;
  return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
         GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof elevation, &returned) &&
         elevation.TokenIsElevated != 0;
}

// One request the way the tray makes it. nullopt: the server cut us off.
std::optional<std::string> Request(const std::wstring& pipeName, const std::string& request) {
  wil::unique_hfile pipe;
  for (int attempt = 0; attempt < 100 && !pipe; ++attempt) {
    pipe.reset(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!pipe) {
      Sleep(50);  // the server hasn't made its instance yet, or is on the previous client
    }
  }
  if (!pipe) {
    return std::nullopt;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr);
  DWORD written = 0;
  if (!WriteFile(pipe.get(), request.data(), static_cast<DWORD>(request.size()), &written, nullptr)) {
    return std::nullopt;
  }
  char buffer[256]{};
  DWORD read = 0;
  if (!ReadFile(pipe.get(), buffer, sizeof buffer, &read, nullptr) || read == 0) {
    return std::nullopt;
  }
  return std::string(buffer, read);
}

// Runs a PipeServer that answers "served" to anything and counts its calls.
struct Served {
  std::atomic<int> calls{0};
  std::jthread thread;
  Served(const std::wstring& pipeName, std::optional<std::wstring> trusted) {
    thread = std::jthread([this, pipeName, trusted](const std::stop_token& stop) {
      PipeServer server(
          pipeName,
          [this](const std::string&) {
            ++calls;
            return std::string("served");
          },
          trusted);
      server.Run(stop);
    });
  }
};

void TestServer() {
  const std::wstring base = L"\\\\.\\pipe\\sovereign-pipe-access-test-" + std::to_wstring(GetCurrentProcessId());
  const bool elevated = OwnTokenElevated();

  {  // No restriction: console mode.
    Served served(base + L"-open", std::nullopt);
    CHECK(Request(base + L"-open", "{}") == "served");
    CHECK(served.calls == 1);
  }
  {  // The trusted image is this very program.
    Served served(base + L"-self", OwnImage());
    CHECK(Request(base + L"-self", "{}") == "served");
    CHECK(Request(base + L"-self", "{}") == "served");  // and again: the instance is reused
    CHECK(served.calls == 2);
  }
  {  // Another program is trusted; this one is a stranger - unless it is elevated.
    Served served(base + L"-other", std::wstring(kTray));
    const auto answer = Request(base + L"-other", "{}");
    if (elevated) {
      CHECK(answer == "served");
      CHECK(served.calls == 1);
    } else {
      CHECK(!answer);
      CHECK(served.calls == 0);
    }
    // Either way the server lives on for the next client.
    const auto again = Request(base + L"-other", "{}");
    CHECK(static_cast<bool>(again) == elevated);
  }
  {  // A service with no path of its own: strangers are cut off the same way.
    Served served(base + L"-nopath", std::wstring());
    const auto answer = Request(base + L"-nopath", "{}");
    CHECK(static_cast<bool>(answer) == elevated);
    CHECK(served.calls == (elevated ? 1 : 0));
  }
  std::cout << "pipe server tested " << (elevated ? "elevated" : "not elevated") << "\n";
}

}  // namespace

int main() {
  try {
    TestDecision();
    TestServer();
  } catch (const std::exception& e) {
    std::cerr << "exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

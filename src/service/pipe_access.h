#pragma once

#include <optional>
#include <string>

namespace sovereign::service {

// What the service learns about the process on the other end of its control
// pipe before it reads a byte from it.
struct PipeClient {
  std::wstring image;      // full Win32 path of its executable; empty if it could not be read
  bool elevated = false;   // its token is an elevated one (an administrator's, SYSTEM)
};

// Whether the service takes requests from `client`. The pipe's DACL only keeps
// out other accounts; every process of a signed-in user passes it, and the
// service runs what it is told as SYSTEM (a config, a kill switch off, an
// installer). So on top of the DACL it is the tray - `trustedImage`, the
// sovereign-tray.exe beside the service in a folder users can't write to - or
// an elevated process (an administrator can stop the service anyway: the smoke
// tests and tools/ scripts run that way). No `trustedImage` at all is the
// console mode for development, where the service is the developer's own
// process and the DACL is all there is; an empty one - a service that can't
// tell where it lives - trusts no tray, only elevated processes.
[[nodiscard]] bool IsAllowedPipeClient(const PipeClient& client, const std::optional<std::wstring>& trustedImage);

}  // namespace sovereign::service

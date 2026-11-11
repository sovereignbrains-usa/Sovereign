#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Before wil: its wrappers for these come only with them.
#include <userenv.h>
#include <wtsapi32.h>

#include <cstdint>
#include <string>

#include "self_update.h"
#include "update.h"

namespace sovereign::service {

// The real IUpdateRunner: checks the tray's installer against the signed
// release it sent, or, from a tray older than that, against the release's
// checksum on GitHub. Copies it into `directory` (UpdateDirectory: under
// Program Files, where users can't write) and runs it silently as SYSTEM.
class SelfUpdate final : public IUpdateRunner {
 public:
  // Clears the installers an update before left in `directory`.
  SelfUpdate(tray::Version current, std::wstring directory, std::wstring userAgent);
  [[nodiscard]] std::string Install(const std::string& version, const std::string& installer, std::uint32_t session,
                                    const std::string& signedRelease) override;
  // A file `held` in `directory` (the installer leaves the folder alone):
  // the time in seconds since 1970.
  std::optional<std::chrono::system_clock::time_point> HeldSince() override;
  void Hold(bool on) override;

 private:
  const tray::Version current_;
  const std::wstring directory_;
  const std::wstring userAgent_;
};

// <the service's folder>\Update.
std::wstring UpdateDirectory();

// sovereign-core.exe --launch-tray <session>, run by the installer the
// service started: the new tray in the user's session, as the user, without
// its window. False if nobody is signed in there.
[[nodiscard]] bool LaunchTray(std::uint32_t session);

}  // namespace sovereign::service

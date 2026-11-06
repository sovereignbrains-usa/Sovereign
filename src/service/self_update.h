#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace sovereign::service {

// update_install's side (control.h): Sovereign updated by the service, as
// SYSTEM - no UAC, so the tray can update while nobody is at the computer.
// A fake in tests/unit/control_test.cpp; the real one is self_update_win.cpp.
class IUpdateRunner {
 public:
  IUpdateRunner() = default;
  virtual ~IUpdateRunner() = default;
  IUpdateRunner(const IUpdateRunner&) = delete;
  IUpdateRunner& operator=(const IUpdateRunner&) = delete;
  IUpdateRunner(IUpdateRunner&&) = delete;
  IUpdateRunner& operator=(IUpdateRunner&&) = delete;

  // Runs the installer of release `version` the tray downloaded to
  // `installer` (UTF-8), if it is that release's: newer than this one and
  // matching the checksum the release itself carries on GitHub - the tray's
  // word is taken for nothing, any user may ask. Afterwards the tray comes
  // back in `session`. "" once the installer runs (it stops this service
  // next), or why not.
  [[nodiscard]] virtual std::string Install(const std::string& version, const std::string& installer, std::uint32_t session) = 0;

  // The kill switch held on for an update (control.h's update_install): a
  // mark that outlives this service - the next one, the installer's, finds
  // it and keeps the filters till its own box is up. Since when, if set.
  virtual std::optional<std::chrono::system_clock::time_point> HeldSince() = 0;
  // Set (now) or cleared.
  virtual void Hold(bool on) = 0;
};

}  // namespace sovereign::service

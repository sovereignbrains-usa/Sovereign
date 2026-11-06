#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "profiles.h"

// Step 3 of docs/design/self-hosted.md: putting the VPN on a server of the user's own.
// The script (tools/selfhost/hy2-setup.sh and lab-setup.sh made one, kept here) goes
// to the server on ssh's stdin (`sh -s`) and says how it goes in "@@sov" lines:
//   @@sov step <n> <of> <name>   a step begins
//   @@sov why <text>             why it stops (UTF-8, for the page)
//   @@sov fail <n>               it stopped at step n; what it changed is put back
//   @@sov rolled-back            ... and was
//   @@sov config / @@sov end     the client's outbounds (JSON) between them - the secrets
//   @@sov done                   all of it went
// The pure part, unit-tested in tests/unit/own_install_test.cpp; main.cpp runs it.

namespace sovereign::tray {

inline constexpr int kInstallSteps = 7;
inline constexpr int kInstallTimeoutSeconds = 900;  // apt on a slow VPS takes minutes

// The script itself.
std::string_view InstallScript();

// The line ssh runs there: the address the client will dial, then the shell reading the script.
// Empty when the host isn't one a command line can hold (IsOwnHost).
std::string InstallCommand(const OwnServer& server);

// What's on the server after it went, for OwnServer::protocols ("script-N" is the script's version).
std::vector<std::string> InstalledProtocols();

struct InstallEvent {
  enum class Kind { None, Step, Why, Failed, RolledBack, ConfigBegin, ConfigEnd, Done };
  Kind kind = Kind::None;
  int step = 0;      // Step, Failed
  std::string text;  // Why
};

// One line of the script's output; anything else is Kind::None (a line for the log).
InstallEvent ParseInstallLine(std::string_view line);

// A step's name for the page: "ставлю sing-box".
std::string InstallStepName(int step);

// "Шаг 3 из 7 · ставлю sing-box".
std::string InstallProgressText(int step);

struct InstallOutcome {
  bool done = false;
  int lastStep = 0;      // the last step that began
  int failedStep = 0;    // 0: didn't say it failed
  bool rolledBack = false;
  std::string why;       // the script's own reason
  std::string outbounds; // the client's {"outbounds": [...]}; only when done
  std::string log;       // the output without the config block: no secrets in it
};

// The whole of what the script printed.
InstallOutcome ReadInstallOutput(std::string_view output);

// One line for the page from the outcome and what ssh said when it couldn't run it
// (SshProblem; empty when ssh ran).
std::string InstallSummary(const InstallOutcome& outcome, std::string_view sshProblem, std::string_view sshError);

}  // namespace sovereign::tray

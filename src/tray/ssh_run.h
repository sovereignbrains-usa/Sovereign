#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "profiles.h"

// Runs the system's ssh.exe against a server of the user's own and returns what it
// printed (ssh_probe.h makes the arguments and reads the answer). Blocks: for a
// worker thread, never the UI's.

namespace sovereign::tray {

struct SshResult {
  bool started = false;    // ssh.exe could be run at all
  bool timedOut = false;   // killed after `timeoutSeconds`
  int exitCode = -1;       // ssh's own: 255 = it couldn't connect
  std::string output;      // stdout, at most 256 KB
  std::string error;       // stderr, at most 256 KB
};

// `remote` is the shell line to run there; `identityFile` empty = ssh's defaults.
// `input` goes to its stdin, which is then closed (none: stdin on NUL); `onLine` gets each
// line of its stdout as it comes, on a thread of RunSsh's own.
SshResult RunSsh(const OwnServer& server, std::string_view identityFile, std::string_view remote,
                 int timeoutSeconds, std::string_view input = {},
                 const std::function<void(std::string_view)>& onLine = {});

// The words for a result that isn't an answer: "" when it is one (exit 0).
std::string SshProblem(const SshResult& result);

}  // namespace sovereign::tray

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "profiles.h"

// The first step of a server of the user's own (docs/design/self-hosted.md, step 2):
// getting in by the system's ssh.exe and asking what is on the other end.
// The pure part, apart from the process - unit-tested in tests/unit/ssh_probe_test.cpp;
// ssh_run.h starts the process.
//
// The login is by key only (BatchMode: ssh never waits for a password on a
// console the user can't see) - the key of ssh-agent or of ~\.ssh, or the file
// the user names (OwnServer::keyRef is the secret store's name, resolved by the caller).

namespace sovereign::tray {

// ssh's arguments for `remote` on `server`; `identityFile` empty = the defaults.
// The host and the user are checked (IsOwnHost) - false and an empty list if they aren't:
// a name starting with '-' would be an option.
std::vector<std::string> SshArguments(const OwnServer& server, std::string_view identityFile,
                                      std::string_view remote);

// One command line for CreateProcessW from an executable and its arguments, quoted
// the way CommandLineToArgvW reads them back (backslashes before quotes doubled).
std::wstring BuildCommandLine(std::wstring_view exe, const std::vector<std::string>& args);

// What the server is asked: a shell line without a quote character that the
// remote shell would have to read twice; it prints "key=value" lines.
std::string ProbeCommand();

struct ServerFacts {
  std::string os;    // "ubuntu-24.04", "debian-12"; "-" if /etc/os-release is missing
  std::string arch;  // "x86_64", "aarch64"
  int uid = -1;      // 0 = root; -1: not said
  std::vector<int> ports;  // the TCP ports something listens on
  bool Valid() const { return uid >= 0 && !arch.empty(); }
};

// The answer of ProbeCommand; anything that isn't a "key=value" line is skipped.
ServerFacts ParseProbe(std::string_view output);

struct ProbeVerdict {
  bool ok = false;               // the installer can go on
  std::string text;              // one line for the page, UTF-8
};

// Can the installer go on: root, a system it knows (Debian or Ubuntu), a processor
// it has builds for; which of 80 and 443 are taken is said, not forbidden.
ProbeVerdict JudgeProbe(const ServerFacts& facts);

// What ssh said when it couldn't connect (its stderr, `exitCode` 255): in words, UTF-8.
std::string SshFailure(std::string_view error, int exitCode, bool timedOut);

}  // namespace sovereign::tray

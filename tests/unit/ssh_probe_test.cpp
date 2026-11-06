// What the tray says to a server over ssh and how it reads the answer (ssh_probe.h):
// the arguments, the command line, the probe's output and the verdict on it.

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "check.h"
#include "ssh_probe.h"

namespace {

using namespace sovereign::tray;

OwnServer Server() {
  OwnServer s;
  s.host = "203.0.113.7";
  return s;
}

void TestArguments() {
  const auto args = SshArguments(Server(), {}, "id -u");
  CHECK(!args.empty() && args.back() == "id -u");
  CHECK(args.size() >= 4 && args[0] == "-p" && args[1] == "22");
  // The host comes after "--": nothing after it is an option.
  const auto dashes = std::find(args.begin(), args.end(), "--");
  CHECK(dashes != args.end() && dashes + 1 != args.end() && *(dashes + 1) == "203.0.113.7");
  CHECK(std::find(args.begin(), args.end(), "BatchMode=yes") != args.end());
  CHECK(std::find(args.begin(), args.end(), "-i") == args.end());

  const auto keyed = SshArguments(Server(), "C:\\keys\\home", {});
  CHECK(std::find(keyed.begin(), keyed.end(), "-i") != keyed.end());
  CHECK(keyed.back() == "203.0.113.7");  // no remote command: the host is last

  OwnServer bad = Server();
  bad.host = "-oProxyCommand=calc";
  CHECK(SshArguments(bad, {}, "id").empty());
  bad = Server();
  bad.sshUser = "a b";
  CHECK(SshArguments(bad, {}, "id").empty());
  bad = Server();
  bad.sshPort = 0;
  CHECK(SshArguments(bad, {}, "id").empty());
}

void TestCommandLine() {
  CHECK(BuildCommandLine(L"C:\\Windows\\ssh.exe", {"-p", "22"}) == L"C:\\Windows\\ssh.exe -p 22");
  CHECK(BuildCommandLine(L"C:\\Program Files\\ssh.exe", {"a b"}) == L"\"C:\\Program Files\\ssh.exe\" \"a b\"");
  CHECK(BuildCommandLine(L"x", {"say \"hi\""}) == L"x \"say \\\"hi\\\"\"");
  CHECK(BuildCommandLine(L"x", {"C:\\dir with space\\"}) == L"x \"C:\\dir with space\\\\\"");
  CHECK(BuildCommandLine(L"x", {""}) == L"x \"\"");
  // UTF-8 comes out as the same text in UTF-16.
  CHECK(BuildCommandLine(L"x", {"\xD1\x82\xD0\xB5\xD1\x81\xD1\x82"}) == L"x \x0442\x0435\x0441\x0442");
}

void TestProbe() {
  const std::string command = ProbeCommand();
  CHECK(command.find('"') == std::string::npos);  // nothing the remote shell would read in two layers

  const auto facts = ParseProbe("os=ubuntu-24.04\r\narch=x86_64\nuid=0\nports=22,443,22,bad,99999,\nnoise\n");
  CHECK(facts.Valid() && facts.os == "ubuntu-24.04" && facts.arch == "x86_64" && facts.uid == 0);
  CHECK(facts.ports == std::vector<int>({22, 443}));
  CHECK(!ParseProbe("").Valid());
  CHECK(!ParseProbe("Welcome to a router\nlogin:").Valid());
  CHECK(ParseProbe("uid=-5\narch=x86_64").uid == -1);
}

void TestVerdict() {
  const auto judge = [](const char* output) { return JudgeProbe(ParseProbe(output)); };
  CHECK(judge("os=debian-12\narch=x86_64\nuid=0\nports=22,").ok);
  CHECK(judge("os=ubuntu-22.04\narch=aarch64\nuid=0\nports=22,443,").ok);
  CHECK(judge("os=ubuntu-22.04\narch=aarch64\nuid=0\nports=22,443,").text.find("443") != std::string::npos);
  CHECK(!judge("os=debian-12\narch=x86_64\nuid=1000\nports=22,").ok);  // not root
  CHECK(!judge("os=debian-12\narch=mips\nuid=0\nports=").ok);
  CHECK(!judge("os=alpine-3.20\narch=x86_64\nuid=0\nports=").ok);  // unknown system: said, not run
  CHECK(!judge("").ok);
}

void TestFailure() {
  CHECK(SshFailure("root@h: Permission denied (publickey).", 255, false).find("ключ") != std::string::npos);
  CHECK(SshFailure("ssh: connect to host h port 22: Connection timed out", 255, false).find("не ответил") !=
        std::string::npos);
  CHECK(SshFailure("", 255, true).find("не ответил") != std::string::npos);
  CHECK(SshFailure("ssh: connect to host h port 22: Connection refused", 255, false).find("отказал") !=
        std::string::npos);
  CHECK(SshFailure("something new\nmore", 7, false) == "ssh завершился с кодом 7: something new");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestArguments();
    TestCommandLine();
    TestProbe();
    TestVerdict();
    TestFailure();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

// The own server's installer (own_install.h): the script's lines read back, the client's
// config kept out of the log, and what the page says for each way it can end.

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "own_install.h"

namespace {

using namespace sovereign::tray;

void TestScript() {
  const std::string_view script = InstallScript();
  CHECK(script.starts_with("#!/bin/sh"));
  CHECK(script.find("trap rollback EXIT") != std::string_view::npos);
  CHECK(script.find("main </dev/null") != std::string_view::npos);  // apt can't eat the script
  CHECK(script.find("flush ruleset") == std::string_view::npos);    // the user's own firewall stays
  CHECK(script.find("api.ipify.org") == std::string_view::npos);    // the address comes from the client
  CHECK(script.find("printf 'R_SNI=%s\\n'") != std::string_view::npos);  // REALITY keeps its site across runs
  for (int step = 1; step <= kInstallSteps; ++step) {
    CHECK(script.find("go " + std::to_string(step) + " ") != std::string_view::npos);
  }
  CHECK(script.find("go " + std::to_string(kInstallSteps + 1) + " ") == std::string_view::npos);
  OwnServer server;
  server.host = "203.0.113.7";
  CHECK(InstallCommand(server) == "ADDR=203.0.113.7 sh -s");
  server.host = "a;reboot";
  CHECK(InstallCommand(server).empty());
}

void TestLines() {
  CHECK(ParseInstallLine("@@sov step 3 7 sing-box").kind == InstallEvent::Kind::Step);
  CHECK(ParseInstallLine("@@sov step 3 7 sing-box\r").step == 3);
  CHECK(ParseInstallLine("@@sov step 9 7 x").kind == InstallEvent::Kind::None);
  CHECK(ParseInstallLine("@@sov why Порт 443/TCP занят").text == "Порт 443/TCP занят");
  CHECK(ParseInstallLine("@@sov fail 2").step == 2);
  CHECK(ParseInstallLine("@@sov done").kind == InstallEvent::Kind::Done);
  CHECK(ParseInstallLine("echo @@sov done").kind == InstallEvent::Kind::None);
  CHECK(InstallProgressText(3) == "Шаг 3 из 7 · ставлю sing-box");
  CHECK(InstallProgressText(0) == "Подключаюсь к серверу…");
}

void TestDone() {
  const InstallOutcome got = ReadInstallOutput(
      "@@sov step 1 7 check\n@@sov step 7 7 start\n@@sov config\n{\"outbounds\": [{\"password\": \"s3cret\"}]}\n"
      "@@sov end\n@@sov done\n");
  CHECK(got.done);
  CHECK(got.outbounds.find("s3cret") != std::string::npos);
  CHECK(got.log.find("s3cret") == std::string::npos);  // the keys stay out of install.log
  CHECK(got.log.find("@@sov done") != std::string::npos);
  CHECK(InstallSummary(got, {}, {}).starts_with("Готово"));

  // done without the config's end: not done
  CHECK(!ReadInstallOutput("@@sov config\n{\n@@sov done\n").done);
}

void TestFailed() {
  InstallOutcome got = ReadInstallOutput(
      "@@sov step 1 7 check\n@@sov step 2 7 packages\nE: Unable to locate package nftables\n@@sov fail 2\n"
      "@@sov rolled-back\n");
  CHECK(!got.done && got.failedStep == 2 && got.rolledBack);
  CHECK(got.why == "E: Unable to locate package nftables");  // no reason given: the last line before
  CHECK(InstallSummary(got, {}, {}) ==
        "Не вышло на шаге 2 из 7 (ставлю пакеты): E: Unable to locate package nftables. Сервер вернул как было");

  got = ReadInstallOutput("@@sov step 1 7 check\n@@sov why Порт 443/TCP занят программой nginx: освободи его\n@@sov fail 1\n");
  CHECK(InstallSummary(got, {}, {}) ==
        "Не вышло на шаге 1 из 7 (смотрю, что на сервере): Порт 443/TCP занят программой nginx: освободи его. "
        "На сервере ничего не менял");

  got = ReadInstallOutput("@@sov step 1 7 check\n@@sov step 3 7 sing-box\n");
  CHECK(InstallSummary(got, {}, {}).starts_with("Связь оборвалась на шаге 3 из 7"));

  got = ReadInstallOutput("");
  CHECK(InstallSummary(got, "Сервер не принял ключ", {}) == "Сервер не принял ключ");
  CHECK(InstallSummary(got, {}, "sh: 1: ADDR: parameter not set\n") ==
        "Сервер не начал установку: sh: 1: ADDR: parameter not set");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestScript();
    TestLines();
    TestDone();
    TestFailed();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

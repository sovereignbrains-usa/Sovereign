// The own server's installer (own_install.h): the script's lines read back, the client's
// config kept out of the log, and what the page says for each way it can end.

#include <cctype>
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
  CHECK(script.find('\r') == std::string_view::npos);  // sh on the server: LF only, whatever the checkout
  CHECK(script.find("trap finish EXIT") != std::string_view::npos);
  CHECK(script.find("main </dev/null") != std::string_view::npos);  // apt can't eat the script
  CHECK(script.find("flush ruleset") == std::string_view::npos);    // the user's own firewall stays
  // packetlab from one commit, and its modules from the same one
  const std::size_t ref = script.find("\nREF=");
  CHECK(ref != std::string_view::npos);
  if (ref != std::string_view::npos) {
    const std::string_view hash = script.substr(ref + 5, script.find('\n', ref + 1) - ref - 5);
    CHECK(hash.size() == 40);
    for (const char c : hash) {
      CHECK(std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)));
    }
  }
  CHECK(script.find("PL_REF=\"$REF\" PL_NO_DOMAIN=1 PL_YES=1 bash") != std::string_view::npos);
  CHECK(script.find("packetlab install all </dev/null") != std::string_view::npos);
  CHECK(script.find("packetlab outbounds") != std::string_view::npos);
  CHECK(script.find("!= \"salamander\"") != std::string_view::npos);  // Hysteria2 only with salamander
  CHECK(script.find("o[\"server\"] = addr") != std::string_view::npos);  // the address comes from the client
  for (int step = 1; step <= kInstallSteps; ++step) {
    CHECK(script.find("go " + std::to_string(step) + " ") != std::string_view::npos);
  }
  CHECK(script.find("go " + std::to_string(kInstallSteps + 1) + " ") == std::string_view::npos);
  CHECK(script.find("say step \"$1\" " + std::to_string(kInstallSteps) + " ") != std::string_view::npos);
  CHECK(InstalledProtocols().back() == "script-2");
  OwnServer server;
  server.host = "203.0.113.7";
  CHECK(InstallCommand(server) == "ADDR=203.0.113.7 sh -s");
  server.host = "a;reboot";
  CHECK(InstallCommand(server).empty());
}

void TestLines() {
  CHECK(ParseInstallLine("@@sov step 3 6 packetlab").kind == InstallEvent::Kind::Step);
  CHECK(ParseInstallLine("@@sov step 3 6 packetlab\r").step == 3);
  CHECK(ParseInstallLine("@@sov step 7 6 x").kind == InstallEvent::Kind::None);
  CHECK(ParseInstallLine("@@sov why Порт 443/TCP занят").text == "Порт 443/TCP занят");
  CHECK(ParseInstallLine("@@sov fail 2").step == 2);
  CHECK(ParseInstallLine("@@sov done").kind == InstallEvent::Kind::Done);
  CHECK(ParseInstallLine("echo @@sov done").kind == InstallEvent::Kind::None);
  CHECK(InstallProgressText(3) == "Шаг 3 из 6 · ставлю packetlab");
  CHECK(InstallProgressText(0) == "Подключаюсь к серверу…");
}

void TestDone() {
  const InstallOutcome got = ReadInstallOutput(
      "@@sov step 1 6 check\n@@sov step 6 6 config\n@@sov config\n"
      "{\"outbounds\": [{\"type\": \"hysteria2\", \"tag\": \"Hysteria2\", \"password\": \"s3cret\"},\n"
      "{\"type\": \"vless\", \"tag\": \"REALITY\"}]}\n"
      "@@sov end\n@@sov done\n");
  CHECK(got.done);
  CHECK(got.outbounds.find("s3cret") != std::string::npos);
  CHECK(got.log.find("s3cret") == std::string::npos);  // the keys stay out of install.log
  CHECK(got.log.find("@@sov done") != std::string::npos);
  CHECK(InstallSummary(got, {}, {}) == "Готово: Hysteria2, REALITY — конфигурация обновлена");

  // done without the config's end: not done
  CHECK(!ReadInstallOutput("@@sov config\n{\n@@sov done\n").done);
  // a config whose names can't be read: still done, said plainly
  CHECK(InstallSummary(ReadInstallOutput("@@sov config\n{}\n@@sov end\n@@sov done\n"), {}, {}) ==
        "Готово: VPN на сервере стоит, конфигурация обновлена");
}

void TestFailed() {
  InstallOutcome got = ReadInstallOutput(
      "@@sov step 1 6 check\n@@sov step 2 6 fetch\n@@sov step 3 6 packetlab\n"
      "  ✗ не смог поставить базовые пакеты\n@@sov fail 3\n");
  CHECK(!got.done && got.failedStep == 3 && !got.rolledBack);
  CHECK(got.why == "✗ не смог поставить базовые пакеты");  // no reason given: the last line before
  CHECK(InstallSummary(got, {}, {}) ==
        "Не вышло на шаге 3 из 6 (ставлю packetlab): ✗ не смог поставить базовые пакеты. "
        "Что успело встать, осталось: «Поставить VPN» ещё раз продолжит; лог — install.log");

  got = ReadInstallOutput("@@sov step 1 6 check\n@@sov why Порт 443/TCP занят программой nginx: освободи его\n@@sov fail 1\n");
  CHECK(InstallSummary(got, {}, {}) ==
        "Не вышло на шаге 1 из 6 (смотрю, что на сервере): Порт 443/TCP занят программой nginx: освободи его. "
        "На сервере ничего не менял");

  got = ReadInstallOutput("@@sov step 1 6 check\n@@sov step 4 6 protocols\n");
  CHECK(InstallSummary(got, {}, {}).starts_with("Связь оборвалась на шаге 4 из 6 (ставлю протоколы)"));

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

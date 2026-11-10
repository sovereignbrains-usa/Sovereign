#include "own_install.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace sovereign::tray {

namespace {

// Kept in step with the script below: a change to what it puts on the server is a new number.
// 1 was sing-box of its own (Hysteria2 and AnyTLS + REALITY); 2 is packetlab.
constexpr int kScriptVersion = 2;

// packetlab (github.com/sovereignbrains-usa/packetlab) on the server, from one commit: install.sh with
// PL_REF so its modules come from the same one, no domain, no questions (there is no terminal over ssh),
// then every protocol it can raise there, then its own `packetlab outbounds` - the client's side,
// the server field made the address the client dials. What the client keeps: Hysteria2 with salamander
// (and its port-hopping twin), REALITY and AnyTLS + REALITY - one UDP way and TCP ones beside it for
// a network that filters UDP. ShadowTLS goes as two outbounds (a detour the server list can't hold),
// Mieru and MTProxy aren't sing-box's: they stay on the server, for packetlab's own links.
// No rollback: packetlab's installer is for a clean server (it sets ufw afresh, ssh's ports kept open)
// and runs again where it stopped. A packetlab with a domain already there is left as it is and only
// asked for its outbounds. The whole of it is one function read before it runs, its stdin on
// /dev/null: apt or dpkg reading stdin would eat the script's own text.
constexpr std::string_view kScript = R"sh(#!/bin/sh
set -u
exec 2>&1
REF=b96ea83f64a95d946b21432e09184790c5a651a3
ADDR="${ADDR:?}"
RAW="https://raw.githubusercontent.com/sovereignbrains-usa/packetlab/$REF"
step=0
said=0
work=

say() { echo "@@sov $*"; }
go() { step=$1; say step "$1" 6 "$2"; }
stop() { said=1; say fail "$step"; exit 1; }
fail() { say why "$1"; stop; }

finish() {
  code=$?
  [ -z "$work" ] || rm -rf "$work"
  if [ "$code" != 0 ] && [ "$said" = 0 ]; then say fail "$step"; fi
  exit "$code"
}

# Who holds 443 on tcp or udp, the one it may be (packetlab's own) left out.
holder() {
  ss -H "-l$1np" "sport = :443" 2>/dev/null | sed -n 's/.*users:(("\([^"]*\)".*/\1/p' | grep -vx "$2" | head -n1
}

main() {
  go 1 check
  [ "$(id -u)" = 0 ] || fail "Нужен вход под root"
  [ -r /etc/os-release ] || fail "Не понять, что за система: нет /etc/os-release"
  . /etc/os-release
  [ "${ID:-}" = debian ] || fail "Система ${PRETTY_NAME:-неизвестная}: packetlab ставится только на Debian (лучше 13)"
  [ "${VERSION_ID:-0}" -ge 12 ] 2>/dev/null || fail "Debian ${VERSION_ID:-без номера}: нужен 12 или новее, лучше 13"
  [ "$(uname -m)" = x86_64 ] || fail "Процессор $(uname -m): packetlab ставится только на x86_64"
  command -v systemctl >/dev/null 2>&1 || fail "Нет systemd"
  command -v ss >/dev/null 2>&1 || fail "Нет ss (пакет iproute2)"
  t="$(holder t haproxy)"
  [ -z "$t" ] || fail "Порт 443/TCP занят программой $t: на нём будет haproxy packetlab — освободи порт"
  u="$(holder u sing-box)"
  [ -z "$u" ] || fail "Порт 443/UDP занят программой $u: на нём будет Hysteria2 — освободи порт"
  mode=fresh
  domain=
  if [ -f /etc/packetlab/meta.json ]; then
    mode=again
    domain="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("domain") or "")' /etc/packetlab/meta.json 2>/dev/null)"
    [ -z "$domain" ] || mode=keep
  fi
  echo "packetlab on the server: $mode"

  go 2 fetch
  work="$(mktemp -d)"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 -o "$work/install.sh" "$RAW/install.sh"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$work/install.sh" "$RAW/install.sh"
  else
    fail "Нет ни curl, ни wget: нечем скачать packetlab"
  fi || fail "packetlab не скачался: сервер не достаёт до raw.githubusercontent.com"
  head -n1 "$work/install.sh" | grep -q '^#!/usr/bin/env bash' || fail "Вместо установщика packetlab пришло что-то другое"
  command -v bash >/dev/null 2>&1 || fail "Нет bash"

  go 3 packetlab
  if [ "$mode" = keep ]; then
    echo "packetlab is set up with the domain $domain: left as it is"
  else
    # Its own last line ("✗ ...") is the reason the page shows.
    PL_REF="$REF" PL_NO_DOMAIN=1 PL_YES=1 bash "$work/install.sh" </dev/null || stop
  fi
  pl="$(command -v packetlab)" || fail "packetlab не встал: команды packetlab нет"
  grep -q 'outbounds)' "$(readlink -f "$pl")" ||
    fail "packetlab на сервере старый, без команды outbounds: обнови его (install.sh) и повтори"

  go 4 protocols
  packetlab install all </dev/null || echo "packetlab: not every protocol went up, see above"

  go 5 verify
  systemctl is-active -q sing-box ||
    fail "sing-box не запущен: $(journalctl -u sing-box -n 3 --no-pager -o cat 2>/dev/null | tr '\n' ' ' | cut -c1-300)"
  packetlab outbounds >"$work/all.json" </dev/null || fail "packetlab не выдал ни одного протокола для клиента"
  got="$(python3 - "$ADDR" "$work/all.json" "$work/client.json" <<'PY'
import json, sys
addr, src, dst = sys.argv[1:4]
keep = []
with open(src, encoding="utf-8") as f:
    every = json.load(f).get("outbounds", [])
for o in every:
    kind = o.get("type")
    if kind not in ("hysteria2", "vless", "anytls") or o.get("detour"):
        continue
    if kind == "hysteria2" and (o.get("obfs") or {}).get("type") != "salamander":
        print("left out %s: its obfs is not salamander" % o.get("tag"), file=sys.stderr)
        continue
    o["server"] = addr
    keep.append(o)
with open(dst, "w", encoding="utf-8") as f:
    json.dump({"outbounds": keep}, f, ensure_ascii=False, indent=2)
udp = [o["server_port"] for o in keep if o["type"] == "hysteria2" and o.get("server_port")]
print(udp[0] if udp else 0, sum(o["type"] != "hysteria2" for o in keep), len(keep))
PY
)" || fail "Ответ packetlab не разобрать"
  set -- $got
  [ "$#" = 3 ] || fail "Ответ packetlab не разобрать"
  [ "$3" -gt 0 ] || fail "На сервере не встал ни один протокол, который понимает клиент"
  if [ "$1" != 0 ]; then
    ss -H -lunp "sport = :$1" | grep -q '"sing-box"' || fail "Hysteria2 в конфиге, но sing-box не слушает $1/UDP"
  fi
  if [ "$2" != 0 ]; then
    ss -H -ltnp "sport = :443" | grep -q '"haproxy"' || fail "haproxy не слушает 443/TCP: REALITY и AnyTLS не дойдут"
  fi

  go 6 config
  say config
  cat "$work/client.json"
  echo
  say end
  say done
}

trap finish EXIT
main </dev/null
)sh";

constexpr std::array<std::string_view, kInstallSteps> kStepNames = {
    "смотрю, что на сервере", "скачиваю packetlab", "ставлю packetlab",
    "ставлю протоколы",       "проверяю",           "собираю конфигурацию"};

bool ToInt(std::string_view text, int& out) {
  const auto [at, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc() && at == text.data() + text.size();
}

std::string_view Trimmed(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
    line.remove_suffix(1);
  }
  return line;
}

std::string_view NextWord(std::string_view& rest) {
  const std::size_t space = rest.find(' ');
  const std::string_view word = rest.substr(0, space);
  rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
  return word;
}

// "Hysteria2, REALITY, AnyTLS + REALITY": the tags of the client's outbounds; empty when none are read.
std::string ServerNames(std::string_view outbounds) {
  const nlohmann::json json = nlohmann::json::parse(outbounds, nullptr, /*allow_exceptions=*/false);
  std::string names;
  if (!json.is_object() || !json.contains("outbounds") || !json["outbounds"].is_array()) {
    return names;
  }
  for (const auto& outbound : json["outbounds"]) {
    if (outbound.is_object() && outbound.contains("tag") && outbound["tag"].is_string()) {
      names += (names.empty() ? "" : ", ") + outbound["tag"].get<std::string>();
    }
  }
  return names;
}

}  // namespace

// A checkout with CRLF (git's autocrlf on Windows) would put a CR at the end of every line of the
// raw string - and sh reads "fi\r" as a word, not "fi". The script goes out with LF whatever the checkout.
std::string_view InstallScript() {
  static const std::string script = [] {
    std::string text(kScript);
    std::erase(text, '\r');
    return text;
  }();
  return script;
}

std::string InstallCommand(const OwnServer& server) {
  if (!IsOwnHost(server.host)) {
    return {};
  }
  return "ADDR=" + server.host + " sh -s";
}

std::vector<std::string> InstalledProtocols() {
  return {"packetlab", std::format("script-{}", kScriptVersion)};
}

InstallEvent ParseInstallLine(std::string_view line) {
  line = Trimmed(line);
  constexpr std::string_view kMark = "@@sov ";
  if (!line.starts_with(kMark)) {
    return {};
  }
  std::string_view rest = line.substr(kMark.size());
  const std::string_view word = NextWord(rest);
  InstallEvent event;
  if (word == "step") {
    int step = 0;
    if (ToInt(NextWord(rest), step) && step >= 1 && step <= kInstallSteps) {
      event.kind = InstallEvent::Kind::Step;
      event.step = step;
    }
  } else if (word == "why") {
    event.kind = InstallEvent::Kind::Why;
    event.text = std::string(rest.substr(0, 400));
  } else if (word == "fail") {
    int step = 0;
    if (ToInt(rest, step) && step >= 0 && step <= kInstallSteps) {
      event.kind = InstallEvent::Kind::Failed;
      event.step = step;
    }
  } else if (word == "rolled-back" && rest.empty()) {
    event.kind = InstallEvent::Kind::RolledBack;
  } else if (word == "config" && rest.empty()) {
    event.kind = InstallEvent::Kind::ConfigBegin;
  } else if (word == "end" && rest.empty()) {
    event.kind = InstallEvent::Kind::ConfigEnd;
  } else if (word == "done" && rest.empty()) {
    event.kind = InstallEvent::Kind::Done;
  }
  return event;
}

std::string InstallStepName(int step) {
  return step >= 1 && step <= kInstallSteps ? std::string(kStepNames[static_cast<std::size_t>(step - 1)])
                                            : std::string("подключаюсь");
}

std::string InstallProgressText(int step) {
  return step >= 1 && step <= kInstallSteps ? std::format("Шаг {} из {} · {}", step, kInstallSteps, InstallStepName(step))
                                            : std::string("Подключаюсь к серверу…");
}

InstallOutcome ReadInstallOutput(std::string_view output) {
  InstallOutcome outcome;
  bool inConfig = false;
  bool configEnded = false;
  std::string config;
  std::string lastNoise;
  while (!output.empty()) {
    const std::size_t end = output.find('\n');
    const std::string_view line = Trimmed(output.substr(0, end));
    output = end == std::string_view::npos ? std::string_view() : output.substr(end + 1);
    const InstallEvent event = ParseInstallLine(line);
    if (inConfig) {
      if (event.kind == InstallEvent::Kind::ConfigEnd) {
        inConfig = false;
        configEnded = true;
        outcome.log += "@@sov end\n";
      } else {
        config.append(line).push_back('\n');
      }
      continue;
    }
    outcome.log.append(line).push_back('\n');
    switch (event.kind) {
      case InstallEvent::Kind::Step: outcome.lastStep = event.step; break;
      case InstallEvent::Kind::Why: outcome.why = event.text; break;
      case InstallEvent::Kind::Failed: outcome.failedStep = std::max(event.step, 1); break;
      case InstallEvent::Kind::RolledBack: outcome.rolledBack = true; break;
      case InstallEvent::Kind::ConfigBegin:
        inConfig = true;
        config.clear();
        outcome.log += "(the client's config: not kept here, it holds the keys)\n";
        break;
      case InstallEvent::Kind::Done: outcome.done = true; break;
      case InstallEvent::Kind::ConfigEnd: break;
      case InstallEvent::Kind::None:
        // packetlab's own lines are indented ("  ✗ ..."): the page gets them without it
        if (const std::size_t text = line.find_first_not_of(" \t"); text != std::string_view::npos) {
          lastNoise = std::string(line.substr(text, 200));
        }
        break;
    }
  }
  if (outcome.failedStep > 0) {
    outcome.done = false;
    if (outcome.why.empty()) {
      outcome.why = lastNoise;
    }
  }
  if (outcome.done && configEnded) {
    outcome.outbounds = std::move(config);
  } else {
    outcome.done = false;
  }
  return outcome;
}

std::string InstallSummary(const InstallOutcome& outcome, std::string_view sshProblem, std::string_view sshError) {
  if (outcome.done) {
    const std::string names = ServerNames(outcome.outbounds);
    return names.empty() ? std::string("Готово: VPN на сервере стоит, конфигурация обновлена")
                         : "Готово: " + names + " — конфигурация обновлена";
  }
  if (outcome.failedStep > 0) {
    std::string text = std::format("Не вышло на шаге {} из {} ({})", outcome.failedStep, kInstallSteps,
                                   InstallStepName(outcome.failedStep));
    if (!outcome.why.empty()) {
      text += ": " + outcome.why;
    }
    if (outcome.failedStep == 1) {
      return text + ". На сервере ничего не менял";
    }
    if (outcome.rolledBack) {  // the script of version 1 put things back
      return text + ". Сервер вернул как было";
    }
    return text + ". Что успело встать, осталось: «Поставить VPN» ещё раз продолжит; лог — install.log";
  }
  if (outcome.lastStep == 0) {  // the script never began: ssh couldn't get in
    if (!sshProblem.empty()) {
      return std::string(sshProblem);
    }
    std::string_view line = Trimmed(sshError.substr(0, sshError.find('\n')));
    return line.empty() ? std::string("Сервер не начал установку") : "Сервер не начал установку: " + std::string(line.substr(0, 160));
  }
  return std::format("Связь оборвалась на шаге {} из {} ({}): что успело встать, осталось; «Поставить VPN» ещё раз "
                     "продолжит; лог — install.log",
                     outcome.lastStep, kInstallSteps, InstallStepName(outcome.lastStep));
}

}  // namespace sovereign::tray

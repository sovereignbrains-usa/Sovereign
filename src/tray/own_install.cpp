#include "own_install.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace sovereign::tray {

namespace {

// Kept in step with the script below: a change to what it puts on the server is a new number.
constexpr int kScriptVersion = 1;

// hy2-setup.sh and lab-setup.sh (tools/selfhost) made one, for a server the user may have other things on:
// no `flush ruleset` (a table of its own, by a unit of its own), the address from the client, not
// from a web service, ufw opened if it is on, port 443 checked before anything is touched, and every
// change put back if a step fails. The secrets it made before are taken again: clients already set up
// keep working after a reinstall. The whole of it is one function read before it runs, its stdin on
// /dev/null: apt or dpkg reading stdin would eat the script's own text.
constexpr std::string_view kScript = R"sh(#!/bin/sh
set -eu
exec 2>&1
VERSION=1.14.4
PORT=443
HOP_LO=20000
HOP_HI=20100
SNI=www.example.com
REALITY_SNI="${REALITY_SNI:-}"
ADDR="${ADDR:?}"
dir=/etc/sing-box
step=0
tmp=
had_box=0
put_box=0
was_active=0
ufw_added=
hop_ok=1

say() { echo "@@sov $*"; }
go() { step=$1; say step "$1" 7 "$2"; }
fail() { say why "$1"; exit 1; }
put_back() {
  if [ -f "$tmp/$(basename "$1")" ]; then cp -p "$tmp/$(basename "$1")" "$1"; else rm -f "$1"; fi
}

rollback() {
  code=$?
  set +e
  if [ "$code" = 0 ]; then
    [ -z "$tmp" ] || rm -rf "$tmp"
    return 0
  fi
  say fail "$step"
  if [ -n "$tmp" ]; then
    put_back "$dir/config.json"
    put_back /etc/sysctl.d/99-sovereign.conf
    put_back /etc/sovereign/hop.nft
    if [ -f /etc/sovereign/hop.nft ]; then
      nft -f /etc/sovereign/hop.nft
    else
      nft delete table inet sovhop 2>/dev/null
      systemctl disable -q sov-hop 2>/dev/null
      rm -f /etc/systemd/system/sov-hop.service
      systemctl daemon-reload
    fi
    for r in $ufw_added; do ufw delete allow "$r" >/dev/null; done
    if [ "$put_box" = 1 ] && [ "$had_box" = 0 ]; then
      systemctl disable --now -q sing-box 2>/dev/null
      dpkg -r sing-box >/dev/null
    elif [ "$was_active" = 1 ]; then
      systemctl restart sing-box
    else
      systemctl stop sing-box 2>/dev/null
    fi
    rm -rf "$tmp"
    say rolled-back
  fi
  exit "$code"
}

taken() {
  ss -H "-l$1np" "sport = :$PORT" 2>/dev/null | grep -v '"sing-box"' | head -n1 |
    sed -n 's/.*users:(("\([^"]*\)".*/\1/p'
}

main() {
  go 1 check
  [ "$(id -u)" = 0 ] || fail "Нужен вход под root"
  case "$(uname -m)" in
    x86_64) ARCH=amd64 ;;
    aarch64) ARCH=arm64 ;;
    *) fail "Процессор $(uname -m): сборок sing-box для него нет" ;;
  esac
  command -v apt-get >/dev/null 2>&1 || fail "Нет apt-get: ставлю только на Debian и Ubuntu"
  command -v systemctl >/dev/null 2>&1 || fail "Нет systemd: ставлю только на Debian и Ubuntu с ним"
  t="$(taken t)"
  [ -z "$t" ] || fail "Порт $PORT/TCP занят программой $t: освободи его"
  u="$(taken u)"
  [ -z "$u" ] || fail "Порт $PORT/UDP занят программой $u: освободи его"
  command -v sing-box >/dev/null 2>&1 && had_box=1
  systemctl is-active -q sing-box 2>/dev/null && was_active=1
  tmp="$(mktemp -d)"
  for f in "$dir/config.json" /etc/sovereign/hop.nft /etc/sysctl.d/99-sovereign.conf; do
    if [ -f "$f" ]; then cp -p "$f" "$tmp/"; fi
  done

  go 2 packages
  export DEBIAN_FRONTEND=noninteractive
  apt-get -o DPkg::Lock::Timeout=180 update -qq >/dev/null || fail "apt-get update не прошёл: сеть сервера или его зеркала"
  apt-get -o DPkg::Lock::Timeout=180 install -y -qq curl ca-certificates openssl nftables >/dev/null ||
    fail "Не поставились curl, openssl и nftables"

  go 3 sing-box
  if [ "$(sing-box version 2>/dev/null | head -n1 | awk '{print $3}')" != "$VERSION" ]; then
    curl -fsSL --retry 3 -o "$tmp/sing-box.deb" \
      "https://github.com/SagerNet/sing-box/releases/download/v$VERSION/sing-box_${VERSION}_linux_$ARCH.deb" ||
      fail "sing-box не скачался с GitHub: сервер не достаёт до github.com"
    put_box=1
    dpkg -i "$tmp/sing-box.deb" >/dev/null || fail "dpkg не поставил sing-box"
  fi

  go 4 keys
  mkdir -p "$dir"
  umask 077
  if [ ! -s "$dir/hy2.key" ]; then
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
      -subj "/CN=$SNI" -addext "subjectAltName=DNS:$SNI" \
      -keyout "$dir/hy2.key" -out "$dir/hy2.crt" 2>/dev/null || fail "openssl не сделал сертификат"
  fi
  pin="$(openssl x509 -in "$dir/hy2.crt" -pubkey -noout | openssl pkey -pubin -outform DER | openssl dgst -sha256 -binary | base64)"
  if [ ! -s "$dir/hy2.secrets" ]; then
    printf 'PASS=%s\nOBFS=%s\n' "$(openssl rand -hex 16)" "$(openssl rand -hex 16)" > "$dir/hy2.secrets"
  fi
  if [ ! -s "$dir/sovereign.secrets" ]; then
    if [ -s "$dir/lab.secrets" ] && grep -q '^R_PRIV=' "$dir/lab.secrets"; then
      grep -E '^(ANY_PASS|R_PRIV|R_PUB|R_SID)=' "$dir/lab.secrets" > "$dir/sovereign.secrets"
    else
      keys="$(sing-box generate reality-keypair)"
      {
        printf 'ANY_PASS=%s\n' "$(openssl rand -hex 16)"
        printf 'R_PRIV=%s\n' "$(echo "$keys" | awk '/PrivateKey/ {print $2}')"
        printf 'R_PUB=%s\n' "$(echo "$keys" | awk '/PublicKey/ {print $2}')"
        printf 'R_SID=%s\n' "$(openssl rand -hex 8)"
      } > "$dir/sovereign.secrets"
    fi
  fi
  umask 022
  . "$dir/hy2.secrets"
  . "$dir/sovereign.secrets"
  # REALITY borrows a site's handshake: the one the server already had (a lab, an earlier setup) stays,
  # or a client of it breaks on the certificate; kept beside the keys for the next run.
  if [ -z "${R_SNI:-}" ]; then
    R_SNI="$REALITY_SNI"
    [ -n "$R_SNI" ] || R_SNI="$(sed -n 's/.*"handshake": *{ *"server": *"\([A-Za-z0-9.-]*\)".*/\1/p' "$dir/config.json" 2>/dev/null | head -n1)"
    [ -n "$R_SNI" ] || R_SNI=www.microsoft.com
    printf 'R_SNI=%s\n' "$R_SNI" >> "$dir/sovereign.secrets"
  fi
  [ -n "${PASS:-}" ] && [ -n "${OBFS:-}" ] && [ -n "${ANY_PASS:-}" ] && [ -n "${R_PRIV:-}" ] && [ -n "${R_PUB:-}" ] &&
    [ -n "$pin" ] || fail "Ключи не получились"

  go 5 config
  if [ -f "$dir/config.json" ] && [ ! -f "$dir/config.json.before-sovereign" ]; then
    cp -p "$dir/config.json" "$dir/config.json.before-sovereign"
  fi
  cat > "$dir/config.json.new" <<EOF
{
  "log": { "level": "warn" },
  "inbounds": [
    {
      "type": "hysteria2", "tag": "hy2-in", "listen": "::", "listen_port": $PORT,
      "users": [ { "name": "owner", "password": "$PASS" } ],
      "ignore_client_bandwidth": true,
      "obfs": { "type": "salamander", "password": "$OBFS" },
      "masquerade": { "type": "string", "status_code": 200, "headers": { "content-type": "text/html; charset=utf-8" }, "content": "<!doctype html><title>Welcome</title><p>It works." },
      "tls": { "enabled": true, "alpn": [ "h3" ], "certificate_path": "$dir/hy2.crt", "key_path": "$dir/hy2.key" }
    },
    {
      "type": "anytls", "tag": "anytls-in", "listen": "::", "listen_port": $PORT,
      "users": [ { "name": "owner", "password": "$ANY_PASS" } ],
      "tls": {
        "enabled": true, "server_name": "$R_SNI",
        "reality": { "enabled": true, "handshake": { "server": "$R_SNI", "server_port": 443 },
                     "private_key": "$R_PRIV", "short_id": [ "$R_SID" ] }
      }
    }
  ],
  "outbounds": [ { "type": "direct", "tag": "direct" } ]
}
EOF
  chmod 600 "$dir/config.json.new"
  sing-box check -c "$dir/config.json.new" || fail "sing-box не принял конфиг"
  mv "$dir/config.json.new" "$dir/config.json"

  go 6 network
  mkdir -p /etc/sovereign
  cat > /etc/sovereign/hop.nft <<EOF
add table inet sovhop
delete table inet sovhop
table inet sovhop {
  chain prerouting {
    type nat hook prerouting priority dstnat; policy accept;
    udp dport $HOP_LO-$HOP_HI redirect to :$PORT
  }
}
EOF
  cat > /etc/systemd/system/sov-hop.service <<EOF
[Unit]
Description=Sovereign: hysteria2 port hopping onto $PORT
After=network.target nftables.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/sbin/nft -f /etc/sovereign/hop.nft
ExecStop=/usr/sbin/nft delete table inet sovhop

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  if ! { systemctl enable -q sov-hop && systemctl restart sov-hop; }; then
    echo "port hopping is off: nftables refused the rule (a container?)"
    hop_ok=0
  fi
  printf 'net.core.rmem_max = 41943040\nnet.core.wmem_max = 41943040\nnet.core.default_qdisc = fq\nnet.ipv4.tcp_congestion_control = bbr\n' \
    > /etc/sysctl.d/99-sovereign.conf
  sysctl -q -p /etc/sysctl.d/99-sovereign.conf >/dev/null 2>&1 || echo "bbr is not taken by this kernel"
  if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active'; then
    for r in "$PORT/tcp" "$PORT/udp" "$HOP_LO:$HOP_HI/udp"; do
      if ! ufw status | grep -q "^$r "; then
        ufw allow "$r" >/dev/null || fail "ufw не открыл $r"
        ufw_added="$ufw_added $r"
      fi
    done
  fi

  go 7 start
  systemctl enable -q sing-box
  systemctl restart sing-box || fail "sing-box не перезапустился"
  i=0
  while [ "$i" -lt 10 ] && ! ss -H -ltnp "sport = :$PORT" | grep -q '"sing-box"'; do
    sleep 1
    i=$((i + 1))
  done
  systemctl is-active -q sing-box ||
    fail "sing-box не запустился: $(journalctl -u sing-box -n 3 --no-pager -o cat 2>/dev/null | tr '\n' ' ' | cut -c1-300)"
  ss -H -ltnp "sport = :$PORT" | grep -q '"sing-box"' || fail "sing-box запущен, но не слушает $PORT/TCP"

  tls="\"enabled\": true, \"server_name\": \"$SNI\", \"alpn\": [ \"h3\" ], \"certificate_public_key_sha256\": [ \"$pin\" ]"
  hop=
  if [ "$hop_ok" = 1 ]; then
    hop="{
      \"type\": \"hysteria2\", \"tag\": \"Hysteria2 · порты $HOP_LO-$HOP_HI\", \"server\": \"$ADDR\",
      \"server_ports\": [ \"$HOP_LO:$HOP_HI\" ], \"hop_interval\": \"20s\", \"hop_interval_max\": \"60s\",
      \"password\": \"$PASS\", \"obfs\": { \"type\": \"salamander\", \"password\": \"$OBFS\" }, \"tls\": { $tls }
    },"
  fi
  say config
  cat <<EOF
{
  "outbounds": [
    {
      "type": "hysteria2", "tag": "Hysteria2", "server": "$ADDR", "server_port": $PORT,
      "password": "$PASS", "obfs": { "type": "salamander", "password": "$OBFS" }, "tls": { $tls }
    },
    $hop
    {
      "type": "anytls", "tag": "AnyTLS + REALITY", "server": "$ADDR", "server_port": $PORT, "password": "$ANY_PASS",
      "tls": {
        "enabled": true, "server_name": "$R_SNI",
        "utls": { "enabled": true, "fingerprint": "chrome" },
        "reality": { "enabled": true, "public_key": "$R_PUB", "short_id": "$R_SID" }
      }
    }
  ]
}
EOF
  say end
  say done
}

trap rollback EXIT
main </dev/null
)sh";

constexpr std::array<std::string_view, kInstallSteps> kStepNames = {
    "смотрю, что на сервере", "ставлю пакеты", "ставлю sing-box", "делаю ключи",
    "пишу конфиг",            "настраиваю сеть", "запускаю"};

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
  return {"hysteria2", "anytls-reality", std::format("script-{}", kScriptVersion)};
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
        if (!line.empty()) {
          lastNoise = std::string(line.substr(0, 200));
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
    return "Готово: на сервере Hysteria2 (UDP 443) и AnyTLS + REALITY (TCP 443), конфигурация обновлена";
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
    return text + (outcome.rolledBack ? ". Сервер вернул как было" : ". Вернуть как было не удалось — смотри install.log");
  }
  if (outcome.lastStep == 0) {  // the script never began: ssh couldn't get in
    if (!sshProblem.empty()) {
      return std::string(sshProblem);
    }
    std::string_view line = Trimmed(sshError.substr(0, sshError.find('\n')));
    return line.empty() ? std::string("Сервер не начал установку") : "Сервер не начал установку: " + std::string(line.substr(0, 160));
  }
  return std::format("Связь оборвалась на шаге {} из {} ({}): что успело поставиться, осталось; лог — install.log",
                     outcome.lastStep, kInstallSteps, InstallStepName(outcome.lastStep));
}

}  // namespace sovereign::tray

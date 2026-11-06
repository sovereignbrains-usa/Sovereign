#include "ssh_probe.h"

#include <algorithm>
#include <charconv>

namespace sovereign::tray {

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
  return out;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

bool ToInt(std::string_view text, int& out) {
  const auto* end = text.data() + text.size();
  const auto [at, error] = std::from_chars(text.data(), end, out);
  return error == std::errc() && at == end;
}

}  // namespace

std::vector<std::string> SshArguments(const OwnServer& server, std::string_view identityFile,
                                      std::string_view remote) {
  if (!IsOwnHost(server.host) || !IsOwnHost(server.sshUser) || server.sshPort < 1 || server.sshPort > 65535) {
    return {};
  }
  std::vector<std::string> args = {"-p", std::to_string(server.sshPort)};
  if (!identityFile.empty()) {
    args.insert(args.end(), {"-i", std::string(identityFile), "-o", "IdentitiesOnly=yes"});
  }
  // BatchMode: no question the user can't answer. accept-new: a first meeting is taken, a changed key refused.
  args.insert(args.end(), {"-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=10",
                           "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3", "-l", server.sshUser, "--",
                           server.host});
  if (!remote.empty()) {
    args.emplace_back(remote);
  }
  return args;
}

std::wstring BuildCommandLine(std::wstring_view exe, const std::vector<std::string>& args) {
  const auto widen = [](const std::string& s) {
    std::wstring out;
    std::size_t i = 0;
    while (i < s.size()) {  // UTF-8 to UTF-16, by hand: this file stays free of Win32
      const auto c = static_cast<unsigned char>(s[i]);
      unsigned cp = c;
      std::size_t more = 0;
      if (c >= 0xF0) {
        cp = c & 0x07;
        more = 3;
      } else if (c >= 0xE0) {
        cp = c & 0x0F;
        more = 2;
      } else if (c >= 0xC0) {
        cp = c & 0x1F;
        more = 1;
      }
      ++i;
      for (; more > 0 && i < s.size(); --more, ++i) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
      }
      if (cp >= 0x10000) {
        cp -= 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
      } else {
        out.push_back(static_cast<wchar_t>(cp));
      }
    }
    return out;
  };
  const auto quote = [](const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
      return arg;
    }
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : arg) {
      if (c == L'\\') {
        ++slashes;
        continue;
      }
      if (c == L'"') {
        out.append(slashes * 2 + 1, L'\\');
        out.push_back(L'"');
      } else {
        out.append(slashes, L'\\');
        out.push_back(c);
      }
      slashes = 0;
    }
    out.append(slashes * 2, L'\\');  // before the closing quote
    out.push_back(L'"');
    return out;
  };
  std::wstring line = quote(std::wstring(exe));
  for (const std::string& arg : args) {
    line.push_back(L' ');
    line += quote(widen(arg));
  }
  return line;
}

std::string ProbeCommand() {
  return ". /etc/os-release 2>/dev/null; echo os=$ID-$VERSION_ID; echo arch=$(uname -m); echo uid=$(id -u); "
         "echo ports=$(ss -tln 2>/dev/null | tr -s ' ' | cut -d' ' -f4 | rev | cut -d: -f1 | rev | tr '\\n' ,)";
}

ServerFacts ParseProbe(std::string_view output) {
  ServerFacts facts;
  while (!output.empty()) {
    const std::size_t end = output.find('\n');
    std::string_view line = output.substr(0, end);
    output = end == std::string_view::npos ? std::string_view() : output.substr(end + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.remove_suffix(1);
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view key = line.substr(0, eq);
    const std::string_view value = line.substr(eq + 1);
    if (key == "os" && value.size() <= 64) {
      facts.os = std::string(value);
    } else if (key == "arch" && value.size() <= 32) {
      facts.arch = std::string(value);
    } else if (key == "uid") {
      int uid = -1;
      if (ToInt(value, uid) && uid >= 0) {
        facts.uid = uid;
      }
    } else if (key == "ports") {
      std::string_view rest = value;
      while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        int port = 0;
        if (ToInt(rest.substr(0, comma), port) && port > 0 && port < 65536 &&
            std::find(facts.ports.begin(), facts.ports.end(), port) == facts.ports.end()) {
          facts.ports.push_back(port);
        }
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
      }
    }
  }
  return facts;
}

ProbeVerdict JudgeProbe(const ServerFacts& facts) {
  if (!facts.Valid()) {
    return {false, "Сервер ответил не так, как отвечает Linux: ставить нечего"};
  }
  const std::string os = Lower(facts.os);
  const std::string arch = Lower(facts.arch);
  if (facts.uid != 0) {
    return {false, "Вход не под root: для установки нужен root (или пользователь с полным sudo — пока не умею)"};
  }
  if (arch != "x86_64" && arch != "aarch64") {
    return {false, "Процессор " + facts.arch + ": сборок для него нет"};
  }
  const bool known = os.starts_with("ubuntu-") || os.starts_with("debian-");
  std::string text = "Сервер подходит: " + facts.os + ", " + facts.arch;
  const bool p443 = std::find(facts.ports.begin(), facts.ports.end(), 443) != facts.ports.end();
  const bool p80 = std::find(facts.ports.begin(), facts.ports.end(), 80) != facts.ports.end();
  if (!known) {
    text = "Система " + facts.os + " не из проверенных (Debian, Ubuntu): может не встать";
  }
  if (p443 || p80) {
    text += std::string(", занят порт ") + (p443 && p80 ? "80 и 443" : p443 ? "443" : "80");
  }
  return {known, text};
}

std::string SshFailure(std::string_view error, int exitCode, bool timedOut) {
  const std::string low = Lower(error);
  if (timedOut || Contains(low, "timed out")) {
    return "Сервер не ответил за отведённое время: адрес, порт SSH или файрвол";
  }
  if (Contains(low, "permission denied")) {
    return "Сервер не принял ключ: нужен вход по ключу (ssh-agent или ~\\.ssh), пароль клиент не спрашивает";
  }
  if (Contains(low, "host key verification failed") || Contains(low, "remote host identification has changed")) {
    return "Ключ сервера изменился с прошлого раза: или сервер переустановили, или это не он. Проверь и убери старую запись из ~\\.ssh\\known_hosts";
  }
  if (Contains(low, "connection refused")) {
    return "Сервер отказал в соединении: SSH не слушает на этом порту";
  }
  if (Contains(low, "could not resolve") || Contains(low, "name or service not known")) {
    return "Адрес не находится: проверь, как он записан";
  }
  if (Contains(low, "no route to host") || Contains(low, "network is unreachable")) {
    return "До сервера нет пути: сеть или адрес";
  }
  std::string text = "ssh завершился с кодом " + std::to_string(exitCode);
  std::string_view line = error.substr(0, error.find('\n'));
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
    line.remove_suffix(1);
  }
  if (!line.empty()) {
    text += ": " + std::string(line.substr(0, 160));
  }
  return text;
}

}  // namespace sovereign::tray

#include "log_events.h"

#include <cstddef>
#include <initializer_list>

namespace sovereign::tray {

namespace {

constexpr std::size_t kNone = std::wstring_view::npos;

bool Has(std::wstring_view text, std::wstring_view what) { return text.find(what) != kNone; }

bool Starts(std::wstring_view text, std::wstring_view what) { return text.substr(0, what.size()) == what; }

// Go's errors are ASCII: lower-cased to look for words in them.
std::wstring Lower(std::wstring_view text) {
  std::wstring out(text);
  for (wchar_t& c : out) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return out;
}

std::wstring_view Trim(std::wstring_view text) {
  while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\r' ||
                           text.back() == L'\n')) {
    text.remove_suffix(1);
  }
  return text;
}

// "[3712645 1ms] dns: ..." -> "dns: ...": the connection's id and age the
// core puts before its info lines.
std::wstring_view StripContext(std::wstring_view message) {
  if (message.size() < 3 || message[0] != L'[' || message[1] < L'0' || message[1] > L'9') {
    return message;
  }
  const std::size_t close = message.find(L"] ");
  if (close == kNone || close > 32) {
    return message;
  }
  return message.substr(close + 2);
}

// Who said it: "outbound/naive[Naive]: rest" (kind, type, tag) or "dns: rest" (kind).
struct Who {
  std::wstring_view kind;
  std::wstring_view type;
  std::wstring_view tag;
  std::wstring_view rest;
};

// "naive[Naive]: rest" after "outbound/": type, tag and what follows.
bool TypeTag(std::wstring_view text, Who& who) {
  const std::size_t open = text.find(L'[');
  const std::size_t space = text.find(L' ');
  if (open == kNone || open == 0 || (space != kNone && space < open)) {
    return false;
  }
  const std::size_t close = text.find(L']', open);
  if (close == kNone) {
    return false;
  }
  who.type = text.substr(0, open);
  who.tag = text.substr(open + 1, close - open - 1);
  std::wstring_view rest = text.substr(close + 1);
  if (Starts(rest, L":")) {
    rest.remove_prefix(1);
  }
  who.rest = Trim(rest);
  return true;
}

Who WhoOf(std::wstring_view message) {
  Who who;
  const std::size_t slash = message.find(L'/');
  const std::size_t space = message.find(L' ');
  if (slash != kNone && slash > 0 && (space == kNone || slash < space)) {
    Who typed;
    typed.kind = message.substr(0, slash);
    if (TypeTag(message.substr(slash + 1), typed)) {
      return typed;
    }
  }
  const std::size_t colon = message.find(L": ");
  if (colon != kNone && colon > 0 && colon <= 24 && !Has(message.substr(0, colon), L" ")) {
    who.kind = message.substr(0, colon);
    who.rest = Trim(message.substr(colon + 2));
    return who;
  }
  who.rest = message;
  return who;
}

// The way named inside a line: "... using outbound/naive[Naive]: ...".
Who OutboundIn(std::wstring_view text) {
  Who who;
  for (const std::wstring_view said : {L"outbound/", L"endpoint/"}) {
    if (const std::size_t at = text.find(said); at != kNone) {
      who.kind = said.substr(0, said.size() - 1);
      if (TypeTag(text.substr(at + said.size()), who)) {
        return who;
      }
      who = {};
    }
  }
  return who;
}

// "youtube.com:443", "[2001:db8::1]:443", "1.2.3.4:443" -> the host without the port.
std::wstring HostOf(std::wstring_view text) {
  text = Trim(text);
  if (Starts(text, L"[")) {
    const std::size_t close = text.find(L']');
    return close == kNone ? std::wstring(text) : std::wstring(text.substr(1, close - 1));
  }
  std::size_t end = 0;
  while (end < text.size() && text[end] != L' ' && text[end] != L',' && text[end] != L'/') {
    ++end;
  }
  text = text.substr(0, end);
  while (!text.empty() && text.back() == L':') {  // "youtube.com:443: dial tcp ..."
    text.remove_suffix(1);
  }
  if (const std::size_t colon = text.rfind(L':'); colon != kNone && text.find(L':') == colon) {
    text = text.substr(0, colon);
  }
  while (!text.empty() && text.back() == L'.') {  // "youtube.com." as DNS writes it
    text.remove_suffix(1);
  }
  return std::wstring(text);
}

// The host a line is about: the site it went to, else (dialled) the address dialled.
std::wstring TargetOf(std::wstring_view text, bool dialled) {
  for (const std::wstring_view said :
       {L"connection to ", L"exchange failed for ", L"lookup failed for ", L"lookup ", L"dial tcp ", L"dial udp "}) {
    if (!dialled && Starts(said, L"dial")) {
      continue;
    }
    if (const std::size_t at = text.find(said); at != kNone) {
      std::wstring host = HostOf(text.substr(at + said.size()));
      if (!host.empty()) {
        return host;
      }
    }
  }
  return {};
}

struct Reason {
  std::wstring_view text;
  LogTone tone = LogTone::Bad;
};

// What went wrong, in a word or two; empty text: not known.
Reason ReasonOf(std::wstring_view message) {
  const std::wstring lower = Lower(message);
  const auto any = [&lower](std::initializer_list<std::wstring_view> words) {
    for (const std::wstring_view word : words) {
      if (Has(lower, word)) {
        return true;
      }
    }
    return false;
  };
  if (any({L"invalid argument", L"network is unreachable", L"no route to host"}) && Has(lower, L"]:") &&
      Has(lower, L"::")) {
    return {L"IPv6 недоступен", LogTone::Warn};
  }
  if (any({L"context canceled", L"operation was canceled"})) {
    return {L"соединение отменено", LogTone::Neutral};
  }
  if (any({L"i/o timeout", L"timed out", L"timeout", L"deadline exceeded", L"failed to send handshake"})) {
    return {L"не ответил вовремя"};
  }
  if (any({L"refused"})) {
    return {L"отказал в подключении"};
  }
  if (any({L"x509", L"certificate"})) {
    return {L"сертификат не подошёл"};
  }
  if (any({L"no such host", L"server misbehaving", L"nxdomain"})) {
    return {L"не нашёл адрес"};
  }
  if (any({L"authentication", L"auth failed", L"unauthorized", L"invalid user", L"bad password", L"wrong password",
           L"forbidden"})) {
    return {L"не принял ключ"};
  }
  if (any({L"connection reset", L"forcibly closed", L"closed network connection", L"broken pipe", L"eof",
           L"connection aborted"})) {
    return {L"закрыл соединение"};
  }
  if (any({L"no route", L"unreachable", L"network is down"})) {
    return {L"сеть недоступна"};
  }
  if (any({L"handshake", L"tls:", L"failed to create session"})) {
    return {L"не договорились о шифровании"};
  }
  return {};
}

// The end of a Go error chain, "a: b: c" -> "c": what really happened.
std::wstring_view Tail(std::wstring_view text) {
  const std::size_t colon = text.rfind(L": ");
  return colon == kNone ? text : text.substr(colon + 2);
}

struct WayName {
  std::wstring title;
  std::wstring where;
  bool proxy = true;   // a server's way, not direct / block / dns
  bool direct = false;
};

WayName NameOf(std::wstring_view type, std::wstring_view tag, const std::vector<LogWay>& ways) {
  WayName name;
  for (const LogWay& way : ways) {
    if (way.tag == tag) {
      name.title = way.title.empty() ? way.tag : way.title;
      name.where = way.where;
      return name;
    }
  }
  if (type == L"direct" || tag == L"direct") {
    name.proxy = false;
    name.direct = true;
    name.title = L"напрямую";
    return name;
  }
  if (type == L"block" || type == L"dns" || tag == L"block" || tag == L"dns-out") {
    name.proxy = false;
    name.title = std::wstring(tag);
    return name;
  }
  name.title = tag == L"warp" ? std::wstring(L"WARP") : std::wstring(tag);
  return name;
}

std::wstring Join(std::wstring_view a, std::wstring_view b) {
  if (a.empty()) {
    return std::wstring(b);
  }
  if (b.empty()) {
    return std::wstring(a);
  }
  std::wstring out(a);
  out += L" · ";
  out += b;
  return out;
}

std::wstring Concat(std::initializer_list<std::wstring_view> parts) {
  std::wstring out;
  for (const std::wstring_view part : parts) {
    out += part;
  }
  return out;
}

LogTone ToneOf(std::wstring_view level) {
  if (level == L"ERROR" || level == L"FATAL" || level == L"PANIC") {
    return LogTone::Bad;
  }
  if (level == L"WARN") {
    return LogTone::Warn;
  }
  return LogTone::Neutral;
}

// An error or a warning about a connection: a server's way that failed, or a site.
bool Trouble(LogEvent& event, std::wstring_view message, const Who& who, const std::vector<LogWay>& ways) {
  Who way = who;
  if (way.kind != L"outbound" && way.kind != L"endpoint") {
    way = OutboundIn(message);
  }
  const Reason reason = ReasonOf(message);
  const std::wstring host = TargetOf(message, way.tag.empty() || !NameOf(way.type, way.tag, ways).proxy);
  const std::wstring_view said = reason.text.empty() ? std::wstring_view(L"ошибка") : reason.text;
  if (!reason.text.empty()) {
    event.tone = reason.tone == LogTone::Bad ? event.tone : reason.tone;
  }
  const std::wstring_view tail = reason.text.empty() ? Trim(Tail(message)) : std::wstring_view();
  if (!way.tag.empty()) {
    WayName name = NameOf(way.type, way.tag, ways);
    event.way = std::wstring(way.tag);
    if (name.proxy) {
      event.topic = LogTopic::Server;
      event.title = Concat({name.title, L": ", said});
      event.detail = Join(name.where, host.empty() ? std::wstring() : Concat({L"к ", host}));
      event.detail = Join(event.detail, tail);
      event.key = Concat({L"server|", way.tag, L"|", said, L"|", tail});
      return true;
    }
    if (!host.empty()) {
      event.topic = LogTopic::Site;
      event.title = Concat({host, L": ", said});
      event.detail = Join(name.direct ? std::wstring_view(L"напрямую, мимо VPN") : std::wstring_view(), tail);
      event.key = Concat({L"site|", host, L"|", said, L"|", tail});
      return true;
    }
  }
  if (!host.empty() && !reason.text.empty()) {
    event.topic = LogTopic::Site;
    event.title = Concat({host, L": ", reason.text});
    event.key = Concat({L"site|", host, L"|", reason.text});
    return true;
  }
  return false;
}

}  // namespace

LogEvent EventOf(std::wstring_view line, const std::vector<LogWay>& ways) {
  LogEvent event;
  std::wstring_view level;
  std::wstring_view message = line;
  if (line.size() >= 17 && line[2] == L':' && line[8] == L' ' && line[9] == L' ') {
    if (line[0] != L'-') {
      event.time = std::wstring(line.substr(0, 5));
    }
    level = Trim(line.substr(10, 5));
    message = line.substr(17);
  }
  message = Trim(message);
  event.tone = ToneOf(level);
  const bool trouble = event.tone != LogTone::Neutral;

  // The tray's own: already in Russian.
  if (Starts(message, L"трей: ")) {
    event.tone = LogTone::Bad;
    event.title = std::wstring(message.substr(6));
    event.key = std::wstring(message);
    return event;
  }
  message = StripContext(message);
  if (Starts(message, L"sing-box started")) {
    event.tone = LogTone::Ok;
    event.title = L"VPN включён";
    event.key = L"started";
    return event;
  }
  const Who who = WhoOf(message);
  const std::wstring_view rest = who.rest;

  if (who.kind == L"ads" && Starts(rest, L"blocked ")) {  // gocore/blocked.go
    const std::wstring name = HostOf(rest.substr(8));
    event.tone = LogTone::Neutral;
    event.topic = LogTopic::Site;
    event.title = name;
    event.detail = L"заблокирован: реклама или слежка";
    event.key = Concat({L"blocked|", name});
    event.route = LogRoute::Block;
    return event;
  }
  if (Has(message, L"missing default interface")) {
    event.tone = LogTone::Warn;
    event.title = L"Нет сети";
    event.detail = L"Windows не видит подключения к интернету";
    event.key = L"no network";
    return event;
  }
  if (Has(message, L"no route to internet")) {
    event.tone = LogTone::Warn;
    event.title = L"Нет сети";
    event.detail = L"интернет ещё не появился";
    event.key = L"no network";
    return event;
  }
  if (who.kind == L"network" && Starts(rest, L"updated default interface ")) {
    std::wstring_view name = rest.substr(26);
    if (const std::size_t comma = name.find(L", index"); comma != kNone) {
      name = name.substr(0, comma);
    }
    event.tone = LogTone::Neutral;
    event.title = Concat({L"Сеть: ", name});
    event.key = Concat({L"network|", name});
    return event;
  }
  if (who.kind == L"inbound" && !trouble) {
    if (Has(rest, L"started at")) {
      event.tone = LogTone::Ok;
      event.title = L"Виртуальный адаптер VPN поднят";
      event.key = L"tun";
      return event;
    }
    if (Has(rest, L"inbound connection") || Has(rest, L"inbound packet connection")) {
      event.hidden = true;
      return event;
    }
  }
  if (!trouble && (who.kind == L"router" || who.kind == L"dns")) {
    // Which rule a name or connection met, what DNS answered, whose it is:
    // the connection's other lines, nothing of their own.
    if (Has(rest, L"match[") || Has(rest, L"found process") || Has(rest, L"found package") ||
        Has(rest, L"exchanged ") || Has(rest, L"cached ") || Has(rest, L"lookup succeed") ||
        Has(rest, L"strategy") || Has(rest, L"sniffed") || Has(rest, L"exchange ") || Has(rest, L"resolved") ||
        Has(rest, L"lookup domain")) {
      event.hidden = true;
      return event;
    }
  }
  if (who.kind == L"dns" && trouble) {
    const Reason reason = ReasonOf(rest);
    const std::wstring host = TargetOf(rest, false);
    const std::wstring_view said = reason.text.empty() ? Trim(Tail(rest)) : reason.text;
    event.topic = LogTopic::Site;
    event.title = reason.text == L"не ответил вовремя" ? std::wstring(L"DNS не ответил вовремя")
                                                         : Concat({L"DNS: ", said});
    event.detail = host;
    event.key = Concat({L"dns|", said});
    return event;
  }
  if ((who.kind == L"outbound" || who.kind == L"endpoint") && !trouble) {
    for (const std::wstring_view said : {L"outbound connection to ", L"outbound packet connection to "}) {
      if (Starts(rest, said)) {
        const WayName name = NameOf(who.type, who.tag, ways);
        if (who.type == L"dns" || who.tag == L"dns-out") {
          event.hidden = true;
          return event;
        }
        const std::wstring host = HostOf(rest.substr(said.size()));
        event.topic = LogTopic::Site;
        event.title = host;
        event.way = std::wstring(who.tag);
        // WARP's endpoint: "warp", or "warp 2" by a config's own "warp" (routing.cpp).
        const bool warp = who.tag == L"warp" || (who.type == L"wireguard" && Starts(who.tag, L"warp"));
        if (name.direct) {
          event.detail = L"напрямую, мимо VPN";
          event.route = LogRoute::Direct;
        } else if (!name.proxy) {
          event.detail = L"заблокирован";
          event.route = LogRoute::Block;
        } else {
          event.detail = Concat({L"через ", name.title});
          event.route = warp ? LogRoute::Warp : LogRoute::Proxy;
        }
        event.key = Concat({L"site|", host, L"|", who.tag});
        return event;
      }
    }
  }
  if (who.kind == L"connection" && (Has(rest, L"upload closed") || Has(rest, L"download closed") ||
                                    Has(rest, L"connection closed"))) {
    const Reason reason = ReasonOf(rest);
    event.tone = LogTone::Neutral;
    event.title = L"Соединение оборвалось";
    event.detail = reason.text.empty() ? std::wstring(Trim(Tail(rest))) : std::wstring(reason.text);
    event.key = Concat({L"closed|", event.detail});
    return event;
  }
  if (trouble && Trouble(event, message, who, ways)) {
    return event;
  }
  event.title = std::wstring(message);
  event.key = std::wstring(message);
  return event;
}

}  // namespace sovereign::tray

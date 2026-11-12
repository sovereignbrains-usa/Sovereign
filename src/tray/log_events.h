#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sovereign::tray {

// A server's way as the log's events name it (UiContent's protocols): the
// tag the core writes, what it is, where it is.
struct LogWay {
  std::wstring tag;    // "AnyTLS-ECH"
  std::wstring title;  // "AnyTLS + ECH"; empty: the tag
  std::wstring where;  // "5.83.147.210 · Германия"; may be empty

  bool operator==(const LogWay&) const = default;
};

// How it went: a dot of its colour by the event (green, amber, red; grey: just so).
enum class LogTone : std::uint8_t { Neutral, Ok, Warn, Bad };

// What it is about, as the log page's filter sorts them.
enum class LogTopic : std::uint8_t { Other, Server, Site };

// A line of the core's log as the log page shows it (logs-concept.html,
// variant A, 12.11.2026): what happened, in Russian, and with what - a
// server's way, a site, a program. A line it can't read is shown as it is.
struct LogEvent {
  LogTone tone = LogTone::Neutral;
  LogTopic topic = LogTopic::Other;
  std::wstring time;    // "19:22"; empty: not known
  std::wstring title;   // "AnyTLS + ECH: не ответил вовремя"
  std::wstring detail;  // under it: "5.83.147.210 · Германия · к youtube.com"; may be empty
  std::wstring key;     // the same key, the same thing again: one row with a count
  std::wstring way;     // the way's tag it is about; empty: none
  bool hidden = false;  // nothing of its own to say (a connection's other lines): only in the lines as they are
};

// `line` as FormatLogLine (main.cpp) writes it: "19:22:40  ERROR  outbound/...".
LogEvent EventOf(std::wstring_view line, const std::vector<LogWay>& ways);

}  // namespace sovereign::tray

#pragma once

#include <string>
#include <string_view>

namespace sovereign::tray {

// The Run value's command line (autostart.h), apart from the registry - so
// tests/unit/autostart_test.cpp can check it.

// What SetAutostart writes: the exe quoted, then `argument`.
std::wstring AutostartCommand(std::wstring_view exe, std::wstring_view argument);

// The exe a Run value starts: quoted - up to the closing quote; not quoted
// (written by hand) - the whole value less a trailing argument starting with
// " --". Empty for an empty value or an unclosed quote.
std::wstring AutostartExe(std::wstring_view command);

// Whether RefreshAutostart rewrites the value for the tray running from
// `exe`: written by an older tray (no `argument`), or for another exe - the
// folder moved, or a copy unpacked elsewhere and the old one deleted
// (11.11.2026, review: a stale path started the old tray, or nothing at all,
// at every sign-in). Paths compare as Windows does, case aside.
bool AutostartNeedsRewrite(std::wstring_view command, std::wstring_view exe, std::wstring_view argument);

}  // namespace sovereign::tray

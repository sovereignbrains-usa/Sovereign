#include "autostart_command.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sovereign::tray {

namespace {

std::wstring_view Trim(std::wstring_view text) {
  while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

std::wstring AutostartCommand(std::wstring_view exe, std::wstring_view argument) {
  std::wstring command = L"\"";
  command.append(exe);
  command.append(L"\" ");
  command.append(argument);
  return command;
}

std::wstring AutostartExe(std::wstring_view command) {
  command = Trim(command);
  if (command.starts_with(L'"')) {
    const std::size_t close = command.find(L'"', 1);
    return close == std::wstring_view::npos ? std::wstring() : std::wstring(command.substr(1, close - 1));
  }
  if (const std::size_t argument = command.find(L" --"); argument != std::wstring_view::npos) {
    command = Trim(command.substr(0, argument));
  }
  return std::wstring(command);
}

bool AutostartNeedsRewrite(std::wstring_view command, std::wstring_view exe, std::wstring_view argument) {
  if (command.find(argument) == std::wstring_view::npos) {
    return true;
  }
  const std::wstring written = AutostartExe(command);
  return CompareStringOrdinal(written.data(), static_cast<int>(written.size()), exe.data(),
                              static_cast<int>(exe.size()), TRUE) != CSTR_EQUAL;
}

}  // namespace sovereign::tray

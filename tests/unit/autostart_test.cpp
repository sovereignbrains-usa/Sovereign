// The Run value's command line (src/tray/autostart_command.h): what is
// written, what exe it starts, and when the tray rewrites it.

#include <exception>
#include <iostream>
#include <string>

#include "autostart_command.h"
#include "check.h"

namespace {

using namespace sovereign::tray;

constexpr wchar_t kArg[] = L"--background";
constexpr wchar_t kExe[] = L"C:\\Program Files\\Sovereign\\sovereign-tray.exe";

void TestCommand() {
  CHECK(AutostartCommand(kExe, kArg) == L"\"C:\\Program Files\\Sovereign\\sovereign-tray.exe\" --background");
  // What is written reads back as the same exe, and needs nothing.
  CHECK(AutostartExe(AutostartCommand(kExe, kArg)) == kExe);
  CHECK(!AutostartNeedsRewrite(AutostartCommand(kExe, kArg), kExe, kArg));
}

void TestExe() {
  CHECK(AutostartExe(L"\"C:\\A B\\t.exe\" --background") == L"C:\\A B\\t.exe");
  CHECK(AutostartExe(L"  \"C:\\A B\\t.exe\"") == L"C:\\A B\\t.exe");
  // Not quoted (by hand): spaces belong to the path, " --" starts an argument.
  CHECK(AutostartExe(L"C:\\Program Files\\Sovereign\\sovereign-tray.exe --background") == kExe);
  CHECK(AutostartExe(L"C:\\Program Files\\Sovereign\\sovereign-tray.exe") == kExe);
  CHECK(AutostartExe(L"").empty());
  CHECK(AutostartExe(L"\"C:\\unclosed.exe --background").empty());
}

void TestRewrite() {
  // An older tray's value: no argument.
  CHECK(AutostartNeedsRewrite(L"\"C:\\Program Files\\Sovereign\\sovereign-tray.exe\"", kExe, kArg));
  // Another folder: the old copy, moved or deleted.
  CHECK(AutostartNeedsRewrite(L"\"D:\\Old\\Sovereign\\sovereign-tray.exe\" --background", kExe, kArg));
  CHECK(AutostartNeedsRewrite(L"\"C:\\Program Files\\Sovereign\\sovereign-tray-old.exe\" --background", kExe, kArg));
  CHECK(AutostartNeedsRewrite(L"\"C:\\unclosed.exe --background", kExe, kArg));
  // The same path in another case - Windows' paths ignore it, Cyrillic too.
  CHECK(!AutostartNeedsRewrite(L"\"c:\\program files\\SOVEREIGN\\Sovereign-Tray.EXE\" --background", kExe, kArg));
  CHECK(!AutostartNeedsRewrite(L"\"C:\\Users\\\u0418\u0432\u0430\u043d\\S\\t.exe\" --background",
                               L"C:\\Users\\\u0438\u0432\u0410\u041d\\S\\t.exe", kArg));
  // Not quoted, with the argument, the right exe: left as it is.
  CHECK(!AutostartNeedsRewrite(L"C:\\Program Files\\Sovereign\\sovereign-tray.exe --background", kExe, kArg));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestCommand();
    TestExe();
    TestRewrite();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

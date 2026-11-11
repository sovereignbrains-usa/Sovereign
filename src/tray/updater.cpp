#include "updater.h"

#include <shellapi.h>

#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string_view>
#include <utility>

#include "fetch.h"
#include "sha256.h"

namespace sovereign::tray {

namespace {

constexpr std::chrono::minutes kFirstCheck{1};
constexpr std::chrono::hours kCheckEvery{12};
constexpr std::size_t kMaxInstallerBytes = 512ULL * 1024 * 1024;
constexpr std::size_t kMaxSmallBytes = std::size_t{1024} * 1024;
// The places of update.json asked at most: the defaults and what the newest release names.
constexpr std::size_t kMaxSources = 8;

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
  return wide;
}

// %TEMP%\Sovereign-Update\<name>, the directory created; empty on failure.
std::wstring TempPath(const std::string& name) {
  wchar_t temp[MAX_PATH + 1]{};
  const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
  if (length == 0 || length > MAX_PATH) {
    return {};
  }
  std::wstring dir = std::wstring(temp, length) + L"Sovereign-Update";
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    return {};
  }
  return dir + L"\\" + Widen(name);
}

bool WriteFileAll(const std::wstring& path, const std::string& data) {
  const wil::unique_hfile file(
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1U << 20));
    DWORD written = 0;
    if (!WriteFile(file.get(), data.data() + offset, chunk, &written, nullptr) || written == 0) {
      return false;
    }
    offset += written;
  }
  return true;
}

// A plain file of at most kMaxSmallBytes, whole.
std::optional<std::string> ReadSmallFile(const std::wstring& path) {
  const wil::unique_hfile file(
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  LARGE_INTEGER size{};
  if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > kMaxSmallBytes) {
    return std::nullopt;
  }
  std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  if (!ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()) {
    return std::nullopt;
  }
  return data;
}

}  // namespace

Updater::Updater(Version current, std::wstring userAgent, bool automatic, std::wstring cache, std::function<void()> changed)
    : current_(current),
      userAgent_(std::move(userAgent)),
      cache_(std::move(cache)),
      changed_(std::move(changed)),
      automatic_(automatic),
      thread_([this](const std::stop_token& stop) { Run(stop); }) {}

Updater::~Updater() {
  thread_.request_stop();
  wake_.notify_all();
}

void Updater::Check() {
  {
    const std::scoped_lock lock(mutex_);
    checkRequested_ = true;
  }
  wake_.notify_all();
}

void Updater::SetAutomatic(bool automatic) {
  {
    const std::scoped_lock lock(mutex_);
    if (automatic_ == automatic) {
      return;
    }
    automatic_ = automatic;
    automaticChanged_ = true;
  }
  wake_.notify_all();
}

void Updater::Install() {
  {
    const std::scoped_lock lock(mutex_);
    installRequested_ = true;
  }
  wake_.notify_all();
}

void Updater::LaunchFailed(const std::string& why) {
  Set([&](State& s) {
    s.status = Status::Available;
    s.error = why;
    s.installer.clear();
    s.signedRelease.clear();
  });
}

Updater::State Updater::Get() const {
  const std::scoped_lock lock(mutex_);
  return state_;
}

void Updater::Set(const std::function<void(State&)>& change) {
  {
    const std::scoped_lock lock(mutex_);
    change(state_);
  }
  changed_();
}

void Updater::Run(const std::stop_token& stop) {
  if (IsDevelopmentBuild(current_)) {
    Set([](State& s) { s.status = Status::Off; });
    std::unique_lock lock(mutex_);
    wake_.wait(lock, stop, [] { return false; });  // asked or not: nothing to do till the end
    return;
  }
  LoadCache();
  auto next = std::chrono::steady_clock::now() + kFirstCheck;
  while (!stop.stop_requested()) {
    bool check = false;
    bool install = false;
    {
      // Not on its own: no time to wake at, only a request (or being put
      // back on its own, when the time it would have checked may be past).
      std::unique_lock lock(mutex_);
      const auto woken = [&] { return checkRequested_ || installRequested_ || automaticChanged_; };
      if (automatic_) {
        wake_.wait_until(lock, stop, next, woken);
      } else {
        wake_.wait(lock, stop, woken);
      }
      if (stop.stop_requested()) {
        return;
      }
      automaticChanged_ = false;
      check = std::exchange(checkRequested_, false) || (automatic_ && std::chrono::steady_clock::now() >= next);
      install = std::exchange(installRequested_, false);
    }
    if (check) {
      next = std::chrono::steady_clock::now() + kCheckEvery;
      DoCheck();
    }
    if (install) {
      DoInstall();
    }
  }
}

void Updater::LoadCache() {
  if (const auto text = cache_.empty() ? std::nullopt : ReadSmallFile(cache_)) {
    known_ = ParseSignedRelease(*text);
  }
}

std::vector<std::string> Updater::Sources() const {
  std::vector<std::string> sources;
  const auto add = [&sources](std::string_view url) {
    if (sources.size() < kMaxSources && IsUpdateUrl(url) && std::ranges::find(sources, url) == sources.end()) {
      sources.emplace_back(url);
    }
  };
  std::ranges::for_each(kUpdateSources, add);
  if (known_) {
    std::ranges::for_each(known_->sources, add);
  }
  return sources;
}

void Updater::DoCheck() {
  // A version found stays found while it's asked about again: its key in the
  // window doesn't blink, and an ask that fails doesn't lose it.
  Set([](State& s) {
    if (s.status != Status::Ready && s.status != Status::Downloading && s.status != Status::Available) {
      s.status = Status::Checking;
    }
  });
  // sing-box's newest, by the way: an ask that fails keeps what was known.
  if (const auto box = Download(Widen(std::string(kSingBoxReleasesApi)), userAgent_, kMaxSmallBytes)) {
    if (const auto version = ParseUpstreamRelease(*box)) {
      Set([&](State& s) { s.boxLatest = ToString(*version); });
    }
  }
  // Every place, not the first that answers: one that is behind (or made to be) can't hide a newer
  // release another one has. Whatever they serve, only what our keys signed counts. A release that
  // names a place we didn't know is asked in this same pass (up to kMaxSources).
  std::vector<std::string> sources = Sources();
  std::optional<Release> release;
  std::string networkError;
  bool badSignature = false;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto body = Download(Widen(sources[i]), userAgent_, kMaxSmallBytes);
    if (!body) {
      if (networkError.empty()) {
        networkError = body.error();
      }
      continue;
    }
    auto found = ParseSignedRelease(*body);
    if (!found) {
      badSignature = true;
      continue;
    }
    for (const std::string& extra : found->sources) {
      if (sources.size() >= kMaxSources) {
        break;
      }
      if (IsUpdateUrl(extra) && std::ranges::find(sources, extra) == sources.end()) {
        sources.push_back(extra);
      }
    }
    if (!release || found->version > release->version) {
      release = std::move(found);
    }
  }
  // The newest verified one is remembered, the places it names with it: a release can move the
  // updates somewhere new, and a restart doesn't forget it.
  if (release && (!known_ || release->version > known_->version ||
                  (release->version == known_->version && release->signedText != known_->signedText))) {
    known_ = release;
    if (!cache_.empty()) {
      const std::wstring next = cache_ + L".new";
      if (WriteFileAll(next, release->signedText)) {
        MoveFileExW(next.c_str(), cache_.c_str(), MOVEFILE_REPLACE_EXISTING);
      }
    }
  } else if (release && known_ && known_->version > release->version) {
    release = known_;  // the places that answered are behind one that said more before
  }
  if (!release) {
    Set([&](State& s) {
      if (s.status == Status::Checking) {
        s.status = Status::Failed;
        s.error = badSignature             ? std::string("описание релиза не прошло проверку подписи")
                  : networkError.empty() ? std::string("негде узнать о новой версии")
                                         : networkError;
      }
    });
    return;
  }
  const bool newer = release->version > current_;
  {
    const std::scoped_lock lock(mutex_);
    release_ = newer ? release : std::nullopt;
  }
  Set([&](State& s) {
    // A newer one than the one found: what failed with that one is over.
    const std::string latest = ToString(release->version);
    if (s.status == Status::Checking || (s.status == Status::Available && (!newer || s.latest != latest))) {
      s.status = newer ? Status::Available : Status::UpToDate;
      s.error.clear();
    }
    s.latest = latest;
    // "обновлений нет" alone read as now, while it was said hours ago (11.11.2026).
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t clock[16] = {};
    swprintf_s(clock, L"%u:%02u", static_cast<unsigned>(local.wHour), static_cast<unsigned>(local.wMinute));
    s.checkedAt = clock;
  });
}

void Updater::DoInstall() {
  // The release found by the last check may be hours old, and a newer one may
  // have come out since: ask again, so the click installs the latest, not the
  // one that was latest back then.
  DoCheck();
  std::optional<Release> release;
  {
    const std::scoped_lock lock(mutex_);
    release = release_;
  }
  if (!release) {
    return;
  }
  Set([&release](State& s) {
    s.status = Status::Downloading;
    s.error.clear();
    s.downloaded = 0;
    s.total = release->installerSize;
    s.bytesPerSecond = 0;
  });
  const auto fail = [this](const std::string& why) {
    Set([&](State& s) {
      s.status = Status::Available;
      s.error = why;
    });
  };
  const std::string name = InstallerName(release->version);
  // The places in order, the next one if a place fails or gives something else than what was signed.
  std::optional<std::string> installer;
  std::string why = "в описании релиза нет установщика";
  bool tried = false;
  for (const std::string& url : release->urls) {
    if (tried) {
      Set([](State& s) {
        s.downloaded = 0;
        s.bytesPerSecond = 0;
      });
    }
    tried = true;
    // The page shows it as it comes: told about a step of a percent (or of a second's quarter), not
    // about every piece of 64 KB.
    const auto began = std::chrono::steady_clock::now();
    auto lastTold = began;
    std::uint64_t lastPercent = 101;
    auto got = Download(Widen(url), userAgent_, kMaxInstallerBytes, [&](std::size_t bytes) {
      const auto now = std::chrono::steady_clock::now();
      const std::uint64_t percent = release->installerSize > 0 ? bytes * 100 / release->installerSize : 0;
      if (percent == lastPercent && now - lastTold < std::chrono::milliseconds(500)) {
        return;
      }
      lastPercent = percent;
      lastTold = now;
      const double seconds = std::chrono::duration<double>(now - began).count();
      Set([&](State& s) {
        s.downloaded = bytes;
        s.bytesPerSecond = seconds > 0.2 ? static_cast<double>(bytes) / seconds : 0;
      });
    });
    if (!got) {
      why = "установщик не скачался: " + got.error();
      continue;
    }
    bool matches = got->size() == release->installerSize;
    try {
      matches = matches && Sha256Hex(*got) == release->sha256;
    } catch (...) {
      LOG_CAUGHT_EXCEPTION_MSG("hashing the installer failed");
      matches = false;
    }
    if (matches) {
      installer = std::move(*got);
      break;
    }
    why = "установщик не совпал с подписанной контрольной суммой — не запускаю";
  }
  if (!installer) {
    return fail(why);
  }
  const std::wstring path = TempPath(name);
  if (path.empty() || !WriteFileAll(path, *installer)) {
    return fail("не удалось сохранить установщик во временную папку");
  }
  Set([&](State& s) {
    s.status = Status::Ready;
    s.installer = path;
    s.signedRelease = release->signedText;
  });
}

std::string LaunchInstaller(HWND owner, const std::wstring& path) {
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof info;
  info.fMask = SEE_MASK_NOASYNC;
  info.hwnd = owner;
  info.lpVerb = L"runas";
  info.lpFile = path.c_str();
  info.lpParameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
  info.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&info)) {
    return {};
  }
  return GetLastError() == ERROR_CANCELLED ? std::string("установка отменена") : std::string("установщик не запустился");
}

}  // namespace sovereign::tray

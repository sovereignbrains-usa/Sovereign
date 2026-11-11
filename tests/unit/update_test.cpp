// The updater's pure rules (src/tray/update.h).

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "check.h"
#include "update.h"

namespace {

using namespace sovereign::tray;

void TestVersions() {
  CHECK((ParseVersion("1.2.3") == Version{1, 2, 3}));
  CHECK((ParseVersion("v0.10.0") == Version{0, 10, 0}));
  CHECK(!ParseVersion("1.2"));
  CHECK(!ParseVersion("1.2.3-rc1"));  // pre-releases aren't followed
  CHECK(!ParseVersion("1.2.3.4"));
  CHECK(!ParseVersion("01.2.3"));
  CHECK(!ParseVersion("1..3"));
  CHECK(!ParseVersion("v"));
  CHECK(!ParseVersion(""));
  CHECK(!ParseVersion("1.2.999999999999"));
  CHECK((Version{0, 10, 0} > Version{0, 9, 9}));
  CHECK((Version{1, 0, 0} > Version{0, 99, 99}));
  CHECK((!(Version{0, 2, 0} > Version{0, 2, 0})));
  CHECK(ToString({1, 20, 3}) == "1.20.3");
  CHECK(InstallerName({0, 2, 0}) == "Sovereign-Setup-0.2.0.exe");
}

void TestModes() {
  for (const UpdateMode mode : {UpdateMode::Off, UpdateMode::Check, UpdateMode::Install}) {
    CHECK(ParseUpdateMode(UpdateModeName(mode)) == mode);
  }
  CHECK(UpdateModeName(UpdateMode::Install) == "install");
  // A tray.json from before the setting, or a word it doesn't know: as it always was.
  CHECK(ParseUpdateMode("") == UpdateMode::Check);
  CHECK(ParseUpdateMode("Install") == UpdateMode::Check);
}

// update.json for the 9-byte sample, as tools/release/sign-update.ps1 writes it, including the
// trailing newline. The primary and the spare are kUpdateKeys; the test key is not.
constexpr std::string_view kSignedByPrimary =
    R"upd({"signed":"{\"version\":\"0.2.0\",\"size\":9,\"sha256\":\"9c0d294c05fc1d88d698034609bb81c0c69196327594e4c69d2915c80fd9850c\",\"page\":\"https://github.com/sovereignbrains-usa/Sovereign/releases/tag/v0.2.0\",\"urls\":[\"https://github.com/sovereignbrains-usa/Sovereign/releases/download/v0.2.0/Sovereign-Setup-0.2.0.exe\",\"https://updates.packetlab.tech/v0.2.0/Sovereign-Setup-0.2.0.exe\"],\"sources\":[\"https://github.com/sovereignbrains-usa/Sovereign/releases/latest/download/update.json\",\"https://updates.packetlab.tech/update.json\"]}","signature":"0694e708cad2535d815f59951e53a86d280957b88c5fc4d41b7ad308508d5a61ab92039c9ac05e67e44152ae1f4a40c18f29ff16f3a5febb4e595014da01a042"})upd"
    "\n";
constexpr std::string_view kSignedBySpare =
    R"upd({"signed":"{\"version\":\"0.2.0\",\"size\":9,\"sha256\":\"9c0d294c05fc1d88d698034609bb81c0c69196327594e4c69d2915c80fd9850c\",\"page\":\"https://github.com/sovereignbrains-usa/Sovereign/releases/tag/v0.2.0\",\"urls\":[\"https://github.com/sovereignbrains-usa/Sovereign/releases/download/v0.2.0/Sovereign-Setup-0.2.0.exe\",\"https://updates.packetlab.tech/v0.2.0/Sovereign-Setup-0.2.0.exe\"],\"sources\":[\"https://github.com/sovereignbrains-usa/Sovereign/releases/latest/download/update.json\",\"https://updates.packetlab.tech/update.json\"]}","signature":"70e0cfca745c06925a58b234ef9d29603161a4d8c52222f8481c0e4cf3d5dbf1c27ebe918f426323eed85b7ace0eb58c5aa2af0ff2c5f725d11163822dae54c6"})upd"
    "\n";
constexpr std::string_view kSignedByTest =
    R"upd({"signed":"{\"version\":\"0.2.0\",\"size\":9,\"sha256\":\"9c0d294c05fc1d88d698034609bb81c0c69196327594e4c69d2915c80fd9850c\",\"page\":\"https://github.com/sovereignbrains-usa/Sovereign/releases/tag/v0.2.0\",\"urls\":[\"https://github.com/sovereignbrains-usa/Sovereign/releases/download/v0.2.0/Sovereign-Setup-0.2.0.exe\",\"https://updates.packetlab.tech/v0.2.0/Sovereign-Setup-0.2.0.exe\"],\"sources\":[\"https://github.com/sovereignbrains-usa/Sovereign/releases/latest/download/update.json\",\"https://updates.packetlab.tech/update.json\"]}","signature":"835e5eae1055cf11a31be044af229461cf366b4d50143a461f9293b45bd401bf4dfe82bbb691d7f7f1e1c775fb4a456941eb18cceac41844a2aaf649548fb588"})upd"
    "\n";
constexpr std::string_view kTestKey =
    "8b6adc5c59b9e753354ce8322d9dd570b6cad43d19d05ffd4de682a4836381bf"
    "03be33cbf2024da6f762b30c7f446f32d5c9f16b411728211837fc921edf3c22";

void CheckSample(const std::optional<Release>& release, std::string_view file) {
  CHECK(release.has_value());
  if (!release) {
    return;
  }
  CHECK((release->version == Version{0, 2, 0}));
  CHECK(release->installerSize == 9);
  CHECK(release->sha256 == "9c0d294c05fc1d88d698034609bb81c0c69196327594e4c69d2915c80fd9850c");
  CHECK(release->urls.size() == 2);
  CHECK(release->urls.front() ==
        "https://github.com/sovereignbrains-usa/Sovereign/releases/download/v0.2.0/Sovereign-Setup-0.2.0.exe");
  CHECK(release->urls.back() == "https://updates.packetlab.tech/v0.2.0/Sovereign-Setup-0.2.0.exe");
  CHECK(release->sources.size() == 2);
  CHECK(release->pageUrl == "https://github.com/sovereignbrains-usa/Sovereign/releases/tag/v0.2.0");
  CHECK(release->signedText == file);
  CHECK(!file.empty() && file.back() == '\n');
}

// A description with the fields filled in as text, so a size can be a number or not one.
std::string Description(const std::string& version, const std::string& size, const std::string& sha256,
                        const std::string& urls) {
  return std::string(R"({"version":")") + version + R"(","size":)" + size + R"(,"sha256":")" + sha256 +
         R"(","urls":)" + urls + "}";
}

void TestSignedRelease() {
  const std::string sha = "9c0d294c05fc1d88d698034609bb81c0c69196327594e4c69d2915c80fd9850c";
  const std::string urls = R"(["https://updates.packetlab.tech/v0.2.0/Sovereign-Setup-0.2.0.exe"])";
  CheckSample(ParseSignedRelease(kSignedByPrimary), kSignedByPrimary);
  CheckSample(ParseSignedRelease(kSignedBySpare), kSignedBySpare);

  std::string tampered(kSignedByPrimary);
  const auto at = tampered.rfind("0694e708");
  CHECK(at != std::string::npos);
  if (at != std::string::npos) {
    tampered[at] = '1';
  }
  CHECK(!ParseSignedRelease(tampered));
  CHECK(!ParseSignedRelease(kSignedByTest));
  const std::array<std::string_view, 1> testKeys{kTestKey};
  CheckSample(ParseSignedRelease(kSignedByTest, testKeys), kSignedByTest);

  const auto plain = ParseReleaseDescription(Description("0.2.0", "9", sha, urls));
  CHECK(plain.has_value());
  if (plain) {
    CHECK(plain->urls.size() == 1 && plain->sha256 == sha);
  }
  CHECK(!ParseReleaseDescription(Description("v0.2.0", "9", sha, urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "9", sha, "[]")));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "9", sha.substr(1), urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "9", "zz" + sha.substr(2), urls)));
  CHECK(!ParseReleaseDescription("not json"));
  CHECK(!ParseReleaseDescription(R"({"version":"0.2.0"})"));

  // An address that is not plain https is left out; a field the client doesn't know is ignored.
  const auto mixed = ParseReleaseDescription(Description(
      "0.2.0", "9", sha,
      R"(["http://updates.packetlab.tech/a.exe","https://updates.packetlab.tech/a.exe","https://updates.packetlab.tech/a.exe"])"));
  CHECK(mixed.has_value());
  if (mixed) {
    CHECK(mixed->urls.size() == 1);
  }
  CHECK(!ParseReleaseDescription(Description("0.2.0", "9", sha, R"(["http://updates.packetlab.tech/a.exe"])")));
  std::string noted = Description("0.2.0", "9", sha, urls);
  noted.insert(noted.size() - 1, R"(,"note":"later")");
  CHECK(ParseReleaseDescription(noted).has_value());
  const auto upper = ParseReleaseDescription(Description("0.2.0", "9", "9C0D294C05FC1D88D698034609BB81C0C69196327594E4C69D2915C80FD9850C", urls));
  CHECK(upper.has_value());
  if (upper) {
    CHECK(upper->sha256 == sha);
  }

  const auto capped = ParseReleaseDescription(Description(
      "0.2.0", "9", sha,
      R"(["https://updates.packetlab.tech/0.exe","https://updates.packetlab.tech/1.exe","https://updates.packetlab.tech/2.exe","https://updates.packetlab.tech/3.exe","https://updates.packetlab.tech/4.exe","https://updates.packetlab.tech/5.exe","https://updates.packetlab.tech/6.exe","https://updates.packetlab.tech/7.exe","https://updates.packetlab.tech/8.exe"])"));
  CHECK(capped.has_value());
  if (capped) {
    CHECK(capped->urls.size() == 8 && capped->urls.back() == "https://updates.packetlab.tech/7.exe");
  }

  // A size that is not a byte count is refused, and none of these may throw.
  CHECK(ParseReleaseDescription(Description("0.2.0", "536870912", sha, urls)).has_value());
  CHECK(!ParseReleaseDescription(Description("0.2.0", "536870913", sha, urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "0", sha, urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "-1", sha, urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "\"9\"", sha, urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "9.5", sha, urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "9.0", sha, urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "1e30", sha, urls)));
  CHECK(!ParseReleaseDescription(Description("0.2.0", "18446744073709551615", sha, urls)));

  CHECK(IsUpdateUrl("https://evil.example/Sovereign-Setup-0.2.0.exe"));
  CHECK(!IsUpdateUrl("http://updates.packetlab.tech/update.json"));
  CHECK(!IsUpdateUrl("https://"));
  CHECK(!IsUpdateUrl("https:///x"));
  CHECK(!IsUpdateUrl("https://updates.packetlab.tech/a b.exe"));
  CHECK(!IsUpdateUrl("https://updates.packetlab.tech/a\n.exe"));
  CHECK(!IsUpdateUrl("https://updates.packetlab.tech/a\r.exe"));
  const std::string longUrl = std::string("https://") + std::string(2040, 'a');
  CHECK(longUrl.size() == 2048 && IsUpdateUrl(longUrl));
  CHECK(!IsUpdateUrl(longUrl + "a"));
}

void TestChecksum() {
  const std::string hash = "9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08";
  const std::string lower = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
  CHECK(ParseChecksum(hash + "\n", "x.exe") == lower);
  CHECK(ParseChecksum(hash + "  Sovereign-Setup-0.2.0.exe\r\n", "Sovereign-Setup-0.2.0.exe") == lower);
  CHECK(ParseChecksum(hash + " *Sovereign-Setup-0.2.0.exe\n", "Sovereign-Setup-0.2.0.exe") == lower);
  CHECK(ParseChecksum("other  a.exe\n" + hash + "  b.exe\n", "b.exe") == lower);
  CHECK(!ParseChecksum(hash + "  other.exe\n", "Sovereign-Setup-0.2.0.exe"));
  CHECK(!ParseChecksum("zz" + hash.substr(2) + "  a.exe", "a.exe"));
  CHECK(!ParseChecksum("", "a.exe"));
  // The service fetches it itself: the release's own word, not the tray's.
  CHECK(ChecksumUrl({0, 5, 46}) ==
        "https://github.com/sovereignbrains-usa/Sovereign/releases/download/v0.5.46/Sovereign-Setup-0.5.46.exe.sha256");
}

void TestUpstream() {
  CHECK((ParseUpstreamRelease(R"({"tag_name":"v1.14.5","draft":false,"prerelease":false})") == Version{1, 14, 5}));
  CHECK(!ParseUpstreamRelease(R"({"tag_name":"v1.15.0-alpha.3","prerelease":true})"));
  CHECK(!ParseUpstreamRelease(R"({"tag_name":"v1.14.5","draft":true})"));
  CHECK(!ParseUpstreamRelease(R"({"message":"API rate limit exceeded"})"));
  CHECK(!ParseUpstreamRelease("not json"));
}

void TestQuiet() {
  using std::chrono::minutes;
  using std::chrono::seconds;
  CHECK(QuietForUpdate(minutes(2), false, minutes(10)));
  CHECK(QuietForUpdate(minutes(60), false, minutes(30)));
  CHECK(!QuietForUpdate(seconds(119), false, minutes(60)));  // at the computer
  CHECK(!QuietForUpdate(minutes(60), true, minutes(60)));   // a game in full screen, no input
  CHECK(!QuietForUpdate(minutes(60), false, seconds(30)));  // downloading
  CHECK(QuietForUpdate(minutes(5), false, minutes(10), minutes(5)));  // the user's own wait
  CHECK(!QuietForUpdate(minutes(4), false, minutes(10), minutes(5)));
  CHECK(ClampQuietMinutes(10) == 10 && ClampQuietMinutes(7) == 2 && ClampQuietMinutes(-1) == 2 && ClampQuietMinutes(30) == 30);
}

void TestDownloadProgress() {
  CHECK(DownloadPercent(0, 0) == -1);
  CHECK(DownloadPercent(50, 200) == 25);
  CHECK(DownloadPercent(300, 200) == 100);
  CHECK(DownloadSecondsLeft(50, 200, 0) == -1);   // no speed yet
  CHECK(DownloadSecondsLeft(50, 0, 10) == -1);    // no total
  CHECK(DownloadSecondsLeft(50, 200, 10) == 15);  // 150 bytes at 10 a second
  CHECK(DownloadSecondsLeft(200, 200, 10) == 0);
  CHECK(DownloadSecondsLeft(51, 200, 10) == 15);  // rounded up: never "0 s" while bytes are left
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestVersions();
    TestModes();
    TestSignedRelease();
    TestChecksum();
    TestQuiet();
    TestUpstream();
    TestDownloadProgress();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

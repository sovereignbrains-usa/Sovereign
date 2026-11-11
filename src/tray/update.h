#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The updater's pure rules, apart from WinHTTP and the installer launch
// (updater.h) so they can be unit-tested (tests/unit/update_test.cpp): which
// release is newer, whether its description is ours (signed), what its installer must be.

namespace sovereign::tray {

// Where a release is described: update.json (tools/release/sign-update.ps1), the same file in every
// place - this repository's GitHub releases and our own mirror (tools/mirror). Whoever serves it, it
// is taken only signed by one of kUpdateKeys, so a place lost (an account, a domain) loses no client:
// the others still serve it, and each release names the places of the next one (Release::sources),
// which the updater remembers.
inline constexpr std::array<std::string_view, 2> kUpdateSources{
    "https://github.com/sovereignbrains-usa/Sovereign/releases/latest/download/update.json",
    "https://updates.packetlab.tech/update.json"};
// The release keys' public halves: ECDSA P-256, X then Y, hex. The first signs in the release
// workflow (the secret UPDATE_SIGNING_KEY); the second is kept offline, for when the first is lost or
// leaked - a release signed by it can carry a client with new keys.
inline constexpr std::array<std::string_view, 2> kUpdateKeys{
    "8e2407a56afcc8f33edd87411a0c2d637541163c583a97ae112276ad5ddbda0f38dbcf24a83f24eb944261adccd7c0911a6cb1dfe0934e3147e3b5d3d89fc7a5",
    "c470fd7636c973f205a8701b9a7a2b7097765f8b0741c9928e7d46f2e25529cde8c917507523bf0515eeb997e1f6c1093fcba35a649e25c631452b6d5c938034"};
// Where GitHub keeps a release's files: the .sha256 the service checks a tray older than update.json
// against (ChecksumUrl).
inline constexpr std::string_view kReleaseDownloads = "https://github.com/sovereignbrains-usa/Sovereign/releases/download/";

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
  auto operator<=>(const Version&) const = default;
};

// 0.0.0: a build for development (CMakeLists.txt) - no updates for it.
inline bool IsDevelopmentBuild(const Version& v) { return v == Version{}; }

// What the updater does unasked (tray.json "updates", the settings' row):
// Check looks a minute after start and every 12 hours and says what it
// found; Install also downloads it and has the service install it once the
// computer is quiet (QuietForUpdate) - no UAC; Off looks only when the user
// asks.
enum class UpdateMode : std::uint8_t { Off, Check, Install };
std::string_view UpdateModeName(UpdateMode mode);  // "off" / "check" / "install"
UpdateMode ParseUpdateMode(std::string_view name);  // unknown -> Check

// "1.2.3" or "v1.2.3"; nullopt for anything else (pre-release suffixes
// included - the updater only follows plain releases).
std::optional<Version> ParseVersion(std::string_view text);
std::string ToString(const Version& version);

// The installer a release carries: Sovereign-Setup-1.2.3.exe, and next to it
// its checksum, Sovereign-Setup-1.2.3.exe.sha256.
std::string InstallerName(const Version& version);

// Where the release v1.2.3 keeps that checksum on GitHub - what the service, asked to install 1.2.3
// (update_install) by a tray that sends no signed description, trusts rather than the tray's word.
std::string ChecksumUrl(const Version& version);

// Whether nobody would miss the connection for the half a minute an update
// takes it away (the service stops, the box with it): nobody at the mouse or
// the keyboard for kQuietInput, nothing full screen (a game, a film, a
// presentation), and next to no traffic for kQuietFor (a download left
// running overnight).
// Nobody at the keyboard: short - someone who is at the computer all day would never see 10 quiet
// minutes - but traffic (a film, a download) is a person watching or a job running: longer.
inline constexpr std::chrono::minutes kQuietInput{2};
inline constexpr std::chrono::minutes kQuietFor{10};
inline constexpr double kBusyBytesPerSecond = 64.0 * 1024;
// The minutes without input the user can choose (the settings' row): kQuietInput is the
// default; anything else a tray.json says is taken for it.
inline constexpr std::array<int, 5> kQuietChoices{1, 2, 5, 10, 30};
int ClampQuietMinutes(int minutes);

// A download's percent (0-100; -1 when the total is unknown) and the seconds left at the speed so far
// (-1 when there is no speed yet or no total).
int DownloadPercent(std::uint64_t downloaded, std::uint64_t total);
int DownloadSecondsLeft(std::uint64_t downloaded, std::uint64_t total, double bytesPerSecond);
bool QuietForUpdate(std::chrono::milliseconds sinceInput, bool fullScreen, std::chrono::seconds sinceTraffic,
                    std::chrono::minutes quietInput = kQuietInput);

struct Release {
  Version version;
  std::vector<std::string> urls;     // where the installer is, tried in order
  std::string sha256;                // the installer's, lowercase hex
  std::uint64_t installerSize = 0;   // its size in bytes
  std::string pageUrl;               // the release's page, for "what's new"; may be empty
  std::vector<std::string> sources;  // where the next update.json is (kUpdateSources)
  std::string signedText;            // the whole update.json it came in, for the service to check again
};

// A release's description, the text update.json signs: {"version","size","sha256","page","urls",
// "sources"}. nullopt unless it has a plain version, a size, a sha256 and at least one https
// address of the installer; addresses that aren't plain https ones are left out, fields it doesn't
// know are ignored (a newer release may say more). Takes it on its word: ParseSignedRelease checks it.
std::optional<Release> ParseReleaseDescription(std::string_view json);

// update.json, {"signed":"<the description>","signature":"<hex: ECDSA P-256 over SHA-256, r then s>"}:
// the release, if the signature is one of keys' over exactly that text. nullopt otherwise.
std::optional<Release> ParseSignedRelease(std::string_view text, std::span<const std::string_view> keys = kUpdateKeys);

// Whether signatureHex (r then s, 128 hex) is keyHex's (X then Y, 128 hex) over SHA-256 of message.
bool VerifyEcdsaP256(std::string_view message, std::string_view signatureHex, std::string_view keyHex);

// An address the updater may fetch: https, no spaces or control characters, not absurdly long.
bool IsUpdateUrl(std::string_view url);

// sing-box's own latest stable release (/latest skips prereleases): whether
// the sing-box inside is the newest - the mark by the version line.
inline constexpr std::string_view kSingBoxReleasesApi = "https://api.github.com/repos/SagerNet/sing-box/releases/latest";

// The version of that API's answer; nullopt for a draft, a prerelease or no
// plain vX.Y.Z tag.
std::optional<Version> ParseUpstreamRelease(std::string_view json);

// A sha256sum-style checksum file: "<64 hex>  <name>" lines, or a bare hash.
// The lowercase hash for `fileName`; nullopt if there is none.
std::optional<std::string> ParseChecksum(std::string_view text, std::string_view fileName);

}  // namespace sovereign::tray

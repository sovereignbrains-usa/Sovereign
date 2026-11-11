#include "update.h"

#include <cmath>

// windows.h's min/max macros break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <limits>

#pragma comment(lib, "bcrypt.lib")
#include "json_field.h"

namespace sovereign::tray {

namespace {

bool IsHex(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
  return out;
}

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// Exactly size bytes of hex, either case; nullopt for anything else.
std::optional<std::vector<unsigned char>> FromHex(std::string_view hex, std::size_t size) {
  if (hex.size() != size * 2 || !IsHex(hex)) {
    return std::nullopt;
  }
  std::vector<unsigned char> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    unsigned value = 0;
    if (std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, value, 16).ec != std::errc{}) {
      return std::nullopt;
    }
    bytes[i] = static_cast<unsigned char>(value);
  }
  return bytes;
}

// How many addresses of each kind a description may give: more is a mistake, not a plan.
constexpr std::size_t kMaxAddresses = 8;
constexpr std::int64_t kMaxInstallerSize = 512LL * 1024 * 1024;

// The array key of object, its plain https addresses (IsUpdateUrl) and only those, at most kMaxAddresses.
std::vector<std::string> Addresses(const nlohmann::json& object, const char* key) {
  std::vector<std::string> out;
  const auto it = object.find(key);
  if (it == object.end() || !it->is_array()) {
    return out;
  }
  for (const auto& item : *it) {
    if (item.is_string() && IsUpdateUrl(item.get_ref<const std::string&>()) && out.size() < kMaxAddresses &&
        std::ranges::find(out, item.get_ref<const std::string&>()) == out.end()) {
      out.push_back(item.get<std::string>());
    }
  }
  return out;
}

// "size" as a byte count. A JSON number past int64, or a float, is not one: nlohmann's
// get<int64_t>() throws on an unsigned value that doesn't fit, and this text is untrusted
// (the updater's thread has nowhere to catch it).
std::optional<std::int64_t> DescriptionSize(const nlohmann::json& object) {
  const auto it = object.find("size");
  if (it == object.end() || it->is_number_float() || !it->is_number_integer()) {
    return std::nullopt;
  }
  if (it->is_number_unsigned()) {
    const std::uint64_t value = it->get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
  }
  return it->get<std::int64_t>();
}

}  // namespace
std::optional<Version> ParseVersion(std::string_view text) {
  if (text.starts_with('v')) {
    text.remove_prefix(1);
  }
  Version version;
  int* const parts[] = {&version.major, &version.minor, &version.patch};
  for (std::size_t i = 0; i < 3; ++i) {
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text.front())) ||
        (text.size() > 1 && text.front() == '0' && std::isdigit(static_cast<unsigned char>(text[1])))) {
      return std::nullopt;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *parts[i]);
    if (error != std::errc{} || *parts[i] > 99999) {
      return std::nullopt;
    }
    text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    if (i < 2) {
      if (!text.starts_with('.')) {
        return std::nullopt;
      }
      text.remove_prefix(1);
    }
  }
  return text.empty() ? std::optional(version) : std::nullopt;
}

std::string ToString(const Version& version) {
  return std::format("{}.{}.{}", version.major, version.minor, version.patch);
}

std::string_view UpdateModeName(UpdateMode mode) {
  switch (mode) {
    case UpdateMode::Off: return "off";
    case UpdateMode::Install: return "install";
    case UpdateMode::Check: break;
  }
  return "check";
}

UpdateMode ParseUpdateMode(std::string_view name) {
  return name == "off" ? UpdateMode::Off : name == "install" ? UpdateMode::Install : UpdateMode::Check;
}

std::string InstallerName(const Version& version) { return "Sovereign-Setup-" + ToString(version) + ".exe"; }

std::string ChecksumUrl(const Version& version) {
  return std::string(kReleaseDownloads) + "v" + ToString(version) + "/" + InstallerName(version) + ".sha256";
}

int ClampQuietMinutes(int minutes) {
  return std::ranges::find(kQuietChoices, minutes) != kQuietChoices.end() ? minutes
                                                                          : static_cast<int>(kQuietInput.count());
}

int DownloadPercent(std::uint64_t downloaded, std::uint64_t total) {
  if (total == 0) {
    return -1;
  }
  return static_cast<int>(downloaded >= total ? 100 : downloaded * 100 / total);
}

int DownloadSecondsLeft(std::uint64_t downloaded, std::uint64_t total, double bytesPerSecond) {
  if (total == 0 || bytesPerSecond < 1) {
    return -1;
  }
  const std::uint64_t left = downloaded >= total ? 0 : total - downloaded;
  return static_cast<int>(std::ceil(static_cast<double>(left) / bytesPerSecond));
}

bool QuietForUpdate(std::chrono::milliseconds sinceInput, bool fullScreen, std::chrono::seconds sinceTraffic,
                    std::chrono::minutes quietInput) {
  return !fullScreen && sinceInput >= quietInput && sinceTraffic >= kQuietFor;
}

bool IsUpdateUrl(std::string_view url) {
  constexpr std::string_view kHttps = "https://";
  return url.starts_with(kHttps) && url.size() > kHttps.size() && url.size() <= 2048 && url[kHttps.size()] != '/' &&
         std::none_of(url.begin(), url.end(), [](char c) { return static_cast<unsigned char>(c) <= ' ' || c == 0x7F; });
}

std::optional<Release> ParseReleaseDescription(std::string_view json) {
  const auto root = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (!root.is_object()) {
    return std::nullopt;
  }
  // A plain version, as a tag would be ("v1.2.3" isn't one here: the description says 1.2.3).
  const std::string versionText = Field<std::string>(root, "version", {});
  const auto version = versionText.starts_with('v') ? std::nullopt : ParseVersion(versionText);
  const auto size = DescriptionSize(root);
  const std::string sha256 = Field<std::string>(root, "sha256", {});
  if (!version || !size || *size <= 0 || *size > kMaxInstallerSize || sha256.size() != 64 || !IsHex(sha256)) {
    return std::nullopt;
  }
  Release release;
  release.version = *version;
  release.installerSize = static_cast<std::uint64_t>(*size);
  release.sha256 = Lower(sha256);
  release.urls = Addresses(root, "urls");
  release.sources = Addresses(root, "sources");
  if (std::string page = Field<std::string>(root, "page", {}); IsUpdateUrl(page)) {
    release.pageUrl = std::move(page);
  }
  if (release.urls.empty()) {
    return std::nullopt;
  }
  return release;
}

std::optional<Release> ParseSignedRelease(std::string_view text, std::span<const std::string_view> keys) {
  const auto root = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!root.is_object()) {
    return std::nullopt;
  }
  const std::string description = Field<std::string>(root, "signed", {});
  const std::string signature = Field<std::string>(root, "signature", {});
  // Parsed first: the signature is checked only on what could be taken anyway.
  auto release = ParseReleaseDescription(description);
  if (!release || std::ranges::none_of(keys, [&](std::string_view key) { return VerifyEcdsaP256(description, signature, key); })) {
    return std::nullopt;
  }
  release->signedText = std::string(text);
  return release;
}

bool VerifyEcdsaP256(std::string_view message, std::string_view signatureHex, std::string_view keyHex) {
  auto signature = FromHex(signatureHex, 64);
  const auto key = FromHex(keyHex, 64);
  if (!signature || !key || message.size() > 0xFFFFFFFFU) {
    return false;
  }
  std::array<unsigned char, 32> digest{};
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())),
                 static_cast<ULONG>(message.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0) {
    return false;
  }
  // BCRYPT_ECCPUBLIC_BLOB: the header, then X and Y, 32 bytes each.
  const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
  std::vector<unsigned char> blob(sizeof header + key->size());
  std::memcpy(blob.data(), &header, sizeof header);
  std::memcpy(blob.data() + sizeof header, key->data(), key->size());
  wil::unique_bcrypt_key handle;
  if (BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, handle.put(), blob.data(),
                          static_cast<ULONG>(blob.size()), 0) < 0) {
    return false;
  }
  return BCryptVerifySignature(handle.get(), nullptr, digest.data(), static_cast<ULONG>(digest.size()),
                               signature->data(), static_cast<ULONG>(signature->size()), 0) >= 0;
}
std::optional<Version> ParseUpstreamRelease(std::string_view json) {
  const auto root = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (!root.is_object() || Field<bool>(root, "draft", false) || Field<bool>(root, "prerelease", false)) {
    return std::nullopt;
  }
  return ParseVersion(Field<std::string>(root, "tag_name", {}));
}

std::optional<std::string> ParseChecksum(std::string_view text, std::string_view fileName) {
  const std::string_view whole = Trim(text);
  if (whole.size() == 64 && IsHex(whole)) {
    return Lower(whole);
  }
  while (!text.empty()) {
    const auto end = text.find('\n');
    const std::string_view line = Trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (line.size() < 66 || !IsHex(line.substr(0, 64)) || (line[64] != ' ' && line[64] != '\t')) {
      continue;
    }
    std::string_view name = Trim(line.substr(64));
    if (name.starts_with('*')) {  // sha256sum's binary-mode marker
      name.remove_prefix(1);
    }
    if (name == fileName) {
      return Lower(line.substr(0, 64));
    }
  }
  return std::nullopt;
}

}  // namespace sovereign::tray

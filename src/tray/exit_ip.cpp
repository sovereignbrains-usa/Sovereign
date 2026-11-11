#include "exit_ip.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "json_field.h"

namespace sovereign::tray {

namespace {

constexpr std::size_t kMaxAddress = 45;  // the longest IPv6 text form
constexpr std::size_t kMaxError = 300;
constexpr std::size_t kMaxIsp = 256;  // bytes: gocore cuts at 64 characters

bool LooksLikeAddress(const std::string& ip) {
  return !ip.empty() && ip.size() <= kMaxAddress && std::all_of(ip.begin(), ip.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
  });
}

constexpr std::size_t kMaxHost = 253;

// A host name as gocore leaves it: lower-case letters, digits, '-' and
// '.', two labels at least.
bool LooksLikeHost(std::string_view host) {
  return !host.empty() && host.size() <= kMaxHost && host.front() != '.' && host.back() != '.' &&
         host.find("..") == std::string_view::npos && host.find('.') != std::string_view::npos &&
         std::all_of(host.begin(), host.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'; });
}

std::vector<std::string_view> Split(std::string_view s, char by) {
  std::vector<std::string_view> parts;
  for (std::size_t i = 0;;) {
    const std::size_t end = std::min(s.find(by, i), s.size());
    parts.push_back(s.substr(i, end - i));
    if (end == s.size()) {
      return parts;
    }
    i = end + 1;
  }
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// The numbers of up to three digits in `s`: "ip-5-83-147-210" -> 5, 83, 147,
// 210 ("005" is 5); longer runs of digits are none of an address's bytes.
std::vector<int> SmallNumbers(std::string_view s) {
  std::vector<int> numbers;
  for (std::size_t i = 0; i < s.size();) {
    if (!IsDigit(s[i])) {
      ++i;
      continue;
    }
    std::size_t end = i;
    int value = 0;
    while (end < s.size() && IsDigit(s[end])) {
      value = end - i < 3 ? value * 10 + (s[end] - '0') : value;
      ++end;
    }
    if (end - i <= 3) {
      numbers.push_back(value);
    }
    i = end;
  }
  return numbers;
}

bool LooksLikeCountry(const std::string& code) {
  return code.size() == 2 && code[0] >= 'A' && code[0] <= 'Z' && code[1] >= 'A' && code[1] <= 'Z';
}

// One line of text, not too long: it's drawn in the window.
bool LooksLikeIsp(const std::string& isp) {
  return !isp.empty() && isp.size() <= kMaxIsp &&
         std::none_of(isp.begin(), isp.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
}

std::string Upper(std::string_view word) {
  std::string upper(word);
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
  return upper;
}

// A company's legal form, as a word of its name: "GmbH", "LLC", "Ltd.", "OU".
bool IsLegalForm(std::string_view word) {
  while (!word.empty() && (word.back() == '.' || word.back() == ',')) {
    word.remove_suffix(1);
  }
  static constexpr std::array<std::string_view, 40> kForms = {
      "LLC",  "LTD",  "LIMITED", "GMBH", "INC",  "OU",    "O\xC3\x9C", "AB",   "BV",    "B.V",
      "SA",   "S.A",  "SRL",     "S.R.L", "SAS", "SARL",  "OY",        "AG",   "KFT",   "CO",
      "CORP", "PLC",  "LLP",     "PTE",  "PTY",  "SPA",   "S.P.A",     "UAB",  "SIA",   "D.O.O",
      "S.R.O", "KG",  "NV",      "N.V",  "AS",   "A.S",   "OOO",       "\xD0\x9E\xD0\x9E\xD0\x9E", "JSC", "LTDA"};
  const std::string upper = Upper(word);
  return std::find(kForms.begin(), kForms.end(), upper) != kForms.end();
}

}  // namespace

std::string ShortIsp(std::string_view isp) {
  // "Philip Fjaera trading as PFWeb Solutions": the business, not the owner.
  if (const std::size_t at = Upper(isp).find(" TRADING AS "); at != std::string::npos && at + 12 < isp.size()) {
    isp.remove_prefix(at + 12);
  }
  std::vector<std::string_view> words;
  for (std::size_t i = 0; i < isp.size();) {
    const std::size_t end = std::min(isp.find(' ', i), isp.size());
    if (end > i) {
      words.push_back(isp.substr(i, end - i));
    }
    i = end + 1;
  }
  std::size_t first = 0;
  std::size_t last = words.size();
  while (last > first + 1 && IsLegalForm(words[last - 1])) {
    --last;
  }
  while (last > first + 1 && IsLegalForm(words[first])) {
    ++first;
  }
  std::string name;
  for (std::size_t i = first; i < last; ++i) {
    name += (name.empty() ? "" : " ") + std::string(words[i]);
  }
  while (!name.empty() && name.back() == ',') {  // "Example, Inc."
    name.pop_back();
  }
  return name.empty() ? std::string(isp) : name;
}

std::string HosterOf(std::string_view host, std::string_view ip) {
  const std::vector<std::string_view> bytes = Split(ip, '.');
  if (!LooksLikeHost(host) || bytes.size() != 4 ||
      std::any_of(bytes.begin(), bytes.end(), [](std::string_view b) {
        return b.empty() || b.size() > 3 || !std::all_of(b.begin(), b.end(), IsDigit);
      })) {
    return {};
  }
  const std::vector<std::string_view> labels = Split(host, '.');
  // The domain: its last two labels, three under a country's own second
  // level ("example.co.uk", "example.com.br").
  static constexpr std::array<std::string_view, 10> kSecondLevels = {"co", "com", "net", "org", "ac",
                                                                     "or", "ne",  "go",  "gov", "edu"};
  std::size_t domainLabels = 2;
  if (labels.back().size() == 2 && labels.size() >= 4 &&
      std::find(kSecondLevels.begin(), kSecondLevels.end(), labels[labels.size() - 2]) != kSecondLevels.end()) {
    domainLabels = 3;
  }
  if (labels.size() <= domainLabels) {
    return {};  // the name is the domain: nothing in it says which address
  }
  const std::size_t first = labels.size() - domainLabels;
  const std::string_view name = labels[first];
  if (std::all_of(name.begin(), name.end(), [](char c) { return IsDigit(c) || c == '-'; })) {
    return {};  // "5.83.147.210.in-addr..." or a number: not a hoster's name
  }
  // The rest spells the address: all four of its bytes, in any order.
  const std::size_t domainAt = static_cast<std::size_t>(labels[first].data() - host.data());
  std::vector<int> numbers = SmallNumbers(host.substr(0, domainAt));
  for (const std::string_view b : bytes) {
    int value = 0;
    for (const char c : b) {
      value = value * 10 + (c - '0');
    }
    const auto it = std::find(numbers.begin(), numbers.end(), value);
    if (it == numbers.end()) {
      return {};
    }
    numbers.erase(it);
  }
  return std::string(host.substr(domainAt));
}

std::optional<ExitIp> ParseExitIpResponse(std::string_view response) {
  const auto json = nlohmann::json::parse(response, nullptr, /*allow_exceptions=*/false);
  if (Field<std::string>(json, "cmd", {}) != "box_exitip") {
    return std::nullopt;
  }
  ExitIp exit;
  if (std::string ip = Field<std::string>(json, "ip", {}); LooksLikeAddress(ip)) {
    exit.ip = std::move(ip);
  }
  if (std::string country = Field<std::string>(json, "country", {}); LooksLikeCountry(country)) {
    exit.country = std::move(country);
  }
  if (std::string isp = Field<std::string>(json, "isp", {}); LooksLikeIsp(isp)) {
    exit.isp = std::move(isp);
  }
  if (std::string city = Field<std::string>(json, "city", {}); LooksLikeIsp(city)) {
    exit.city = std::move(city);
  }
  if (std::string host = Field<std::string>(json, "host", {}); LooksLikeHost(host)) {
    exit.host = std::move(host);
  }
  exit.pending = Field<bool>(json, "pending", false);
  exit.error = Field<std::string>(json, "error", {}).substr(0, kMaxError);
  return exit;
}

}  // namespace sovereign::tray

#pragma once

#include <optional>
#include <string>
#include <string_view>

// The service's box_exitip answer (gocore/exitip.go): the address the
// internet sees through the server in use, and its country. Apart from the
// worker so it's unit-tested (tests/unit/exit_ip_test.cpp) and fuzzed.

namespace sovereign::tray {

struct ExitIp {
  std::string ip;       // "185.12.34.56" / "2a01:4f8::1"; empty until known
  std::string country;  // "NL": two uppercase letters, or empty
  std::string isp;      // the address's network owner ("Hetzner Online GmbH"): one line, or empty
  std::string city;     // "Frankfurt am Main": one line, or empty
  std::string host;     // the address's reverse name, lower case: "210.147.83.5.hostbrr.com", or empty
  bool pending = false; // a lookup runs
  std::string error;    // why the last one failed
};

// An ISP's name to show next to a server: legal forms cut off -
// "Hetzner Online GmbH" -> "Hetzner Online", "AEZA INTERNATIONAL LTD" ->
// "AEZA INTERNATIONAL", "OOO Selectel" -> "Selectel". The whole name if that
// would leave nothing.
std::string ShortIsp(std::string_view isp);

// Who hosts the server, by its address's reverse name: the name's domain -
// "210.147.83.5.hostbrr.com" -> "hostbrr.com", "static.4.3.2.1.clients.
// your-server.de" -> "your-server.de", "1-2-3-4.host.example.co.uk" ->
// "example.co.uk". Only a hoster's own name counts, one that spells the
// address (`ip`, IPv4): a name its owner chose ("vpn.example.com") says
// nothing about the hoster, and gives "".
std::string HosterOf(std::string_view host, std::string_view ip);

// nullopt unless it's a box_exitip answer. Fields that don't look like what
// they should (an address of other characters, a country that isn't two
// letters) are left empty: they're drawn in the window.
std::optional<ExitIp> ParseExitIpResponse(std::string_view response);

}  // namespace sovereign::tray

// The service's box_exitip answer (src/tray/exit_ip.h).

#include <exception>
#include <iostream>
#include <string>

#include "check.h"
#include "exit_ip.h"

namespace {

using sovereign::tray::ParseExitIpResponse;

void TestParses() {
  const auto v4 = ParseExitIpResponse(R"({"cmd":"box_exitip","tag":"nl","ip":"185.12.34.56","country":"NL"})");
  CHECK(v4 && v4->ip == "185.12.34.56" && v4->country == "NL" && !v4->pending && v4->error.empty());
  const auto v6 = ParseExitIpResponse(R"({"cmd":"box_exitip","tag":"nl","ip":"2a01:4f8::1"})");
  CHECK(v6 && v6->ip == "2a01:4f8::1" && v6->country.empty());
  const auto pending = ParseExitIpResponse(R"({"cmd":"box_exitip","tag":"nl","pending":true})");
  CHECK(pending && pending->pending && pending->ip.empty());
  const auto failed = ParseExitIpResponse(R"({"cmd":"box_exitip","tag":"nl","error":"i/o timeout"})");
  CHECK(failed && failed->error == "i/o timeout");
}

void TestDropsWhatDoesNotFit() {
  const auto odd = ParseExitIpResponse(
      R"({"cmd":"box_exitip","ip":"<script>","country":"nl","pending":"yes","error":5})");
  CHECK(odd && odd->ip.empty() && odd->country.empty() && !odd->pending && odd->error.empty());
  const auto longIp = ParseExitIpResponse(R"({"cmd":"box_exitip","ip":")" + std::string(60, '1') + R"("})");
  CHECK(longIp && longIp->ip.empty());
  const auto longCountry = ParseExitIpResponse(R"({"cmd":"box_exitip","country":"NLD"})");
  CHECK(longCountry && longCountry->country.empty());
  const auto longError = ParseExitIpResponse(R"({"cmd":"box_exitip","error":")" + std::string(1000, 'e') + R"("})");
  CHECK(longError && longError->error.size() == 300);
}

void TestIsp() {
  const auto full = ParseExitIpResponse(
      R"({"cmd":"box_exitip","tag":"nl","ip":"185.12.34.56","country":"NL","isp":"Hetzner Online GmbH"})");
  CHECK(full && full->isp == "Hetzner Online GmbH");
  const auto twoLines = ParseExitIpResponse(R"({"cmd":"box_exitip","isp":"a\nb"})");
  CHECK(twoLines && twoLines->isp.empty());
  const auto number = ParseExitIpResponse(R"({"cmd":"box_exitip","isp":5})");
  CHECK(number && number->isp.empty());
  const auto tooLong = ParseExitIpResponse(R"({"cmd":"box_exitip","isp":")" + std::string(300, 'x') + R"("})");
  CHECK(tooLong && tooLong->isp.empty());

  using sovereign::tray::ShortIsp;
  CHECK(ShortIsp("Hetzner Online GmbH") == "Hetzner Online");
  CHECK(ShortIsp("AEZA INTERNATIONAL LTD") == "AEZA INTERNATIONAL");
  CHECK(ShortIsp("Brainoza OU") == "Brainoza");
  CHECK(ShortIsp("DigitalOcean, LLC") == "DigitalOcean");
  CHECK(ShortIsp("Example, Inc.") == "Example");
  CHECK(ShortIsp("OVH SAS") == "OVH");
  CHECK(ShortIsp("OOO Selectel") == "Selectel");
  CHECK(ShortIsp("Stark Industries Solutions Ltd") == "Stark Industries Solutions");
  CHECK(ShortIsp("Cloudflare") == "Cloudflare");
  CHECK(ShortIsp("Philip Fjaera trading as PFWeb Solutions") == "PFWeb Solutions");
  CHECK(ShortIsp("AEZA GROUP LLC") == "AEZA GROUP");
  CHECK(ShortIsp("LLC") == "LLC");  // nothing else: the whole name
  CHECK(ShortIsp("Ltd. Co.") == "Ltd.");
  CHECK(ShortIsp("") == "");
}

void TestCityAndHost() {
  const auto full = ParseExitIpResponse(
      R"({"cmd":"box_exitip","tag":"de","ip":"5.83.147.210","country":"DE","city":"Frankfurt am Main",)"
      R"("host":"210.147.83.5.hostbrr.com"})");
  CHECK(full && full->city == "Frankfurt am Main" && full->host == "210.147.83.5.hostbrr.com");
  const auto odd = ParseExitIpResponse(R"({"cmd":"box_exitip","city":"a\r\nb","host":"Evil Host.com"})");
  CHECK(odd && odd->city.empty() && odd->host.empty());
  const auto dots = ParseExitIpResponse(R"({"cmd":"box_exitip","host":"a..com"})");
  CHECK(dots && dots->host.empty());
  const auto oneLabel = ParseExitIpResponse(R"({"cmd":"box_exitip","host":"localhost"})");
  CHECK(oneLabel && oneLabel->host.empty());

  using sovereign::tray::HosterOf;
  CHECK(HosterOf("210.147.83.5.hostbrr.com", "5.83.147.210") == "hostbrr.com");
  CHECK(HosterOf("17.171.175.5.hostbrr.com", "5.175.171.17") == "hostbrr.com");
  CHECK(HosterOf("static.56.34.12.185.clients.your-server.de", "185.12.34.56") == "your-server.de");
  CHECK(HosterOf("ip-005-083-147-210.vps.example.co.uk", "5.83.147.210") == "example.co.uk");
  CHECK(HosterOf("vm4-3-2-1.example.com.br", "1.2.3.4") == "example.com.br");
  CHECK(HosterOf("vpn.example.com", "5.83.147.210").empty());          // its owner's name
  CHECK(HosterOf("210.147.83.6.hostbrr.com", "5.83.147.210").empty());  // another address's
  CHECK(HosterOf("hostbrr.com", "5.83.147.210").empty());
  CHECK(HosterOf("4.3.2.1.123.com", "1.2.3.4").empty());  // a number for a name
  CHECK(HosterOf("210.147.83.5.hostbrr.com", "2a01:4f8::1").empty());
  CHECK(HosterOf("210.147.83.5.hostbrr.com", "").empty());
  CHECK(HosterOf("", "5.83.147.210").empty());
  CHECK(HosterOf("1.1.1.1.example.com", "1.1.1.1") == "example.com");
  CHECK(HosterOf("1.1.1.example.com", "1.1.1.1").empty());  // one "1" short
}

void TestRejectsOtherAnswers() {
  CHECK(!ParseExitIpResponse(R"({"cmd":"error","message":"gocore not loaded"})"));
  CHECK(!ParseExitIpResponse("not json"));
  CHECK(!ParseExitIpResponse("[1]"));
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestParses();
    TestDropsWhatDoesNotFit();
    TestIsp();
    TestCityAndHost();
    TestRejectsOtherAnswers();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

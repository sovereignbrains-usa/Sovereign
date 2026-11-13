// The log page's events: the core's lines in Russian (src/tray/log_events.h).

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "check.h"
#include "log_events.h"

namespace {

using namespace sovereign::tray;

const std::vector<LogWay>& Ways() {
  static const std::vector<LogWay> ways = {
      {L"AnyTLS-ECH", L"AnyTLS + ECH", L"5.83.147.210 · Германия"},
      {L"Naive", L"NaiveProxy", L""},
  };
  return ways;
}

LogEvent Of(const wchar_t* line) { return EventOf(line, Ways()); }

void TestAServersWayThatTimedOut() {
  const LogEvent event = Of(
      L"19:22:40  ERROR  outbound/anytls[AnyTLS-ECH]: open connection to youtube.com:443: dial tcp "
      L"5.83.147.210:443: i/o timeout");
  CHECK(event.time == L"19:22");
  CHECK(event.tone == LogTone::Bad);
  CHECK(event.topic == LogTopic::Server);
  CHECK(event.title == L"AnyTLS + ECH: не ответил вовремя");
  CHECK(event.detail == L"5.83.147.210 · Германия · к youtube.com");
  CHECK(event.way == L"AnyTLS-ECH");
  CHECK(!event.hidden);
  // The same again: the same key, one row with a count.
  const LogEvent again = Of(
      L"19:22:41  ERROR  outbound/anytls[AnyTLS-ECH]: open connection to youtube.com:443: dial tcp "
      L"5.83.147.210:443: i/o timeout");
  CHECK(again.key == event.key);
}

void TestIpv6ThatIsNotThere() {
  const LogEvent event = Of(
      L"19:22:40  ERROR  outbound/naive[Naive]: open UDP connection to [2001:4860:4860::8888]:443: dial udp "
      L"[2001:4860:4860::8888]:443: An invalid argument was supplied.");
  CHECK(event.tone == LogTone::Warn);
  CHECK(event.title == L"NaiveProxy: IPv6 недоступен");
  CHECK(event.detail == L"к 2001:4860:4860::8888");
}

void TestTheWayNamedInside() {
  const LogEvent event = Of(
      L"19:22:40  ERROR  [3843 30s] connection: open connection to discord.com:443 using outbound/naive[Naive]: "
      L"dial tcp 1.2.3.4:443: connectex: No connection could be made because the target machine actively "
      L"refused it.");
  CHECK(event.topic == LogTopic::Server);
  CHECK(event.title == L"NaiveProxy: отказал в подключении");
  CHECK(event.detail == L"к discord.com");
}

void TestADirectSite() {
  const LogEvent event =
      Of(L"19:22:40  INFO   [7 0ms] outbound/direct[direct]: outbound connection to sberbank.ru:443");
  CHECK(event.topic == LogTopic::Site);
  CHECK(event.title == L"sberbank.ru");
  CHECK(event.detail == L"напрямую, мимо VPN");
  const LogEvent proxied =
      Of(L"19:22:40  INFO   [8 0ms] outbound/anytls[AnyTLS-ECH]: outbound connection to youtube.com:443");
  CHECK(proxied.detail == L"через AnyTLS + ECH");
  CHECK(proxied.key != event.key);
  // Which branch of the routing page's each went by: its "сейчас:" line.
  CHECK(event.route == LogRoute::Direct);
  CHECK(proxied.route == LogRoute::Proxy);
  const LogEvent warp =
      Of(L"19:22:40  INFO   [9 0ms] endpoint/wireguard[warp]: outbound connection to gemini.google.com:443");
  CHECK(warp.title == L"gemini.google.com");
  CHECK(warp.route == LogRoute::Warp);
  CHECK(Of(L"19:20:01  INFO   ads: blocked mc.yandex.ru").route == LogRoute::Block);
  CHECK(Of(L"19:20:01  INFO   sing-box started (0.31s)").route == LogRoute::None);
}

void TestWhatSaysNothingOfItsOwn() {
  CHECK(Of(L"19:22:40  INFO   [3712645 1ms] dns: match[9] rule_set=[sov-ads sov-ads-adguard] => "
           L"predefined(rcode=NOERROR)")
            .hidden);
  CHECK(Of(L"19:22:40  INFO   [7 0ms] router: match[4] rule_set=sov-ads => reject").hidden);
  CHECK(Of(L"19:22:40  INFO   [7 0ms] inbound/tun[tun-in]: inbound connection to 1.1.1.1:443").hidden);
  CHECK(Of(L"19:22:40  INFO   [7 0ms] outbound/dns[dns-out]: outbound packet connection to 172.19.0.2:53").hidden);
}

void TestPlainEvents() {
  const LogEvent started = Of(L"19:20:01  INFO   sing-box started (0.31s)");
  CHECK(started.tone == LogTone::Ok && started.title == L"VPN включён");
  const LogEvent nonet = Of(L"19:20:01  ERROR  network: missing default interface");
  CHECK(nonet.tone == LogTone::Warn && nonet.title == L"Нет сети");
  const LogEvent blocked = Of(L"19:20:01  INFO   ads: blocked mc.yandex.ru");
  CHECK(blocked.topic == LogTopic::Site && blocked.title == L"mc.yandex.ru");
  const LogEvent tray = Of(L"19:20:01  ERROR  трей: служба не отвечает");
  CHECK(tray.tone == LogTone::Bad && tray.title == L"служба не отвечает");
  const LogEvent dns = Of(L"19:20:01  ERROR  dns: exchange failed for example.org. IN A: context deadline exceeded");
  CHECK(dns.title == L"DNS не ответил вовремя" && dns.detail == L"example.org");
}

void TestWhatItCannotRead() {
  const LogEvent event = Of(L"--:--:--  WARN   something new happened");
  CHECK(event.time.empty());
  CHECK(event.tone == LogTone::Warn);
  CHECK(event.title == L"something new happened");
  CHECK(!event.hidden);
  CHECK(EventOf(L"", {}).title.empty());
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  try {
    TestAServersWayThatTimedOut();
    TestIpv6ThatIsNotThere();
    TestTheWayNamedInside();
    TestADirectSite();
    TestWhatSaysNothingOfItsOwn();
    TestPlainEvents();
    TestWhatItCannotRead();
  } catch (const std::exception& e) {
    std::cerr << "exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

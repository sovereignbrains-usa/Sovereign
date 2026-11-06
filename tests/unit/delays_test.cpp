// Reading the service's box_delays answers (src/tray/delays.h).

#include <exception>
#include <iostream>

#include "check.h"
#include "delays.h"
#include "ui_content.h"

namespace {

using namespace sovereign::tray;

void TestParsesResults() {
  const auto delays = ParseDelaysResponse(
      R"({"cmd":"box_delays","results":[{"tag":"nl","delay":48,"jitter":3,"loss":250,"samples":9,"connect":412},)"
      R"({"tag":"fi","error":"i/o timeout"},)"
      R"({"tag":"de","pending":true},{"delay":5},"junk",{"tag":"big","delay":1e9},{"tag":7,"delay":1}]})");
  CHECK(delays.has_value());
  if (!delays) {
    return;
  }
  CHECK(delays->size() == 3);
  CHECK(delays->contains("nl") && delays->at("nl").state == Delay::State::Ok && delays->at("nl").ms == 48);
  CHECK(delays->contains("nl") && delays->at("nl").jitter == 3 && delays->at("nl").loss == 100 &&
        delays->at("nl").samples == 9 && delays->at("nl").connect == 412);
  CHECK(delays->contains("fi") && delays->at("fi").state == Delay::State::Failed &&
        delays->at("fi").error == "i/o timeout");
  CHECK(delays->contains("de") && delays->at("de").state == Delay::State::Pending);
  CHECK(!delays->contains("big"));  // not an integer: skipped, not thrown
}

void TestRejectsWhatIsNotAnAnswer() {
  CHECK(!ParseDelaysResponse("not json"));
  CHECK(!ParseDelaysResponse(R"({"cmd":"error","message":"box not running"})"));
  CHECK(!ParseDelaysResponse(R"({"cmd":"box_delays"})"));
  CHECK(!ParseDelaysResponse(R"({"cmd":"box_delays","results":{}})"));
  CHECK(ParseDelaysResponse(R"({"cmd":"box_delays","results":[]})").has_value());
}

Delay Ok(int ms, int jitter = 0, int loss = 0) {
  return {.state = Delay::State::Ok, .ms = ms, .jitter = jitter, .loss = loss, .samples = 9};
}

// Auto picks the best at once when it has nothing (or a dead server), and
// moves off a live one only for a clear win, twice in a row.
void TestJudgeAuto() {
  const std::vector<std::string> members = {"nl", "fi", "de"};
  std::map<std::string, Delay> delays = {{"nl", Ok(80)}, {"fi", Ok(60, 15)}, {"de", Ok(70)}};
  // The spread counts: fi's 60 ±15 is worse than de's steady 70.
  AutoPick pick = JudgeAuto({}, members, delays);
  CHECK(pick.server == "de" && pick.candidate.empty());

  // A few milliseconds better is noise: no switch, no candidate.
  delays = {{"nl", Ok(64)}, {"fi", Ok(90)}, {"de", Ok(70)}};
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "de" && pick.candidate.empty());

  // A clear win once makes a candidate; twice, the pick.
  delays = {{"nl", Ok(40)}, {"fi", Ok(90)}, {"de", Ok(70)}};
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "de" && pick.candidate == "nl" && pick.wins == 1);
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "nl" && pick.candidate.empty());

  // A win that doesn't repeat is forgotten.
  delays = {{"nl", Ok(80)}, {"fi", Ok(40)}, {"de", Ok(90)}};
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "nl" && pick.candidate == "fi" && pick.wins == 1);
  delays = {{"nl", Ok(80)}, {"fi", Ok(78)}, {"de", Ok(90)}};
  pick = JudgeAuto(pick, members, delays);
  CHECK(pick.server == "nl" && pick.candidate.empty());

  // Losses weigh: 5% lost is worse than 40 ms more.
  delays = {{"nl", Ok(50, 0, 5)}, {"fi", Ok(90)}, {"de", Ok(95)}};
  CHECK(JudgeAuto({}, members, delays).server == "fi");

  // Protocols of one host: the host is judged by its best, the protocol on it
  // is the best of its own at once (ten ms), the other host needs a clear win twice.
  const std::map<std::string, std::string> hosts = {{"de-naive", "de"}, {"de-vless", "de"}, {"nl-naive", "nl"}};
  const std::vector<std::string> servers = {"de-naive", "de-vless", "nl-naive"};
  std::map<std::string, Delay> hostDelays = {{"de-naive", Ok(66, 4)}, {"de-vless", Ok(51, 2)}, {"nl-naive", Ok(58, 3)}};
  AutoPick onHost = JudgeAuto({.server = "de-naive"}, servers, hostDelays, hosts);
  CHECK(onHost.server == "de-vless" && onHost.candidate.empty());
  // Close protocols on one host don't swap.
  hostDelays = {{"de-naive", Ok(55)}, {"de-vless", Ok(51)}, {"nl-naive", Ok(80)}};
  CHECK(JudgeAuto({.server = "de-naive"}, servers, hostDelays, hosts).server == "de-naive");
  // The other host wins clearly: a candidate first, the host's own protocol on the second.
  hostDelays = {{"de-naive", Ok(120)}, {"de-vless", Ok(110)}, {"nl-naive", Ok(60)}};
  onHost = JudgeAuto({.server = "de-naive"}, servers, hostDelays, hosts);
  CHECK(onHost.server == "de-vless" && onHost.candidate == "nl-naive" && onHost.wins == 1);
  onHost = JudgeAuto(onHost, servers, hostDelays, hosts);
  CHECK(onHost.server == "nl-naive");

  // The best by score is what the page compares auto's server with; none answered: empty.
  CHECK(BestServer(members, delays) == "fi");
  CHECK(BestServer({"x"}, delays).empty());

  // The one in use failed: the best at once.
  delays = {{"nl", {.state = Delay::State::Failed, .error = "timeout"}}, {"fi", Ok(90)}, {"de", Ok(95)}};
  CHECK(JudgeAuto({.server = "nl"}, members, delays).server == "fi");
  // Not measured this time: kept.
  delays = {{"nl", Delay{}}, {"fi", Ok(20)}};
  CHECK(JudgeAuto({.server = "nl"}, members, delays).server == "nl");
  // Gone from the group: replaced.
  CHECK(JudgeAuto({.server = "se"}, members, delays).server == "fi");
  // Nothing answered: kept as it is.
  delays = {{"fi", {.state = Delay::State::Failed}}};
  CHECK(JudgeAuto({.server = "nl", .candidate = "de", .wins = 1}, members, delays).candidate == "de");
}

// WARP's next server: untried, answered first and best first, failed last.
void TestNextWarpServer() {
  const std::vector<std::string> servers = {"nl", "fi", "de", "se"};
  std::map<std::string, Delay> delays = {
      {"nl", Ok(80)}, {"fi", {.state = Delay::State::Failed}}, {"de", Ok(40)}, {"se", Delay{}}};
  CHECK(NextWarpServer(servers, {"nl"}, delays) == "de");
  CHECK(NextWarpServer(servers, {"nl", "de"}, delays) == "se");
  CHECK(NextWarpServer(servers, {"nl", "de", "se"}, delays) == "fi");
  CHECK(NextWarpServer(servers, servers, delays).empty());
  CHECK(NextWarpServer(servers, {}, {}) == "nl");  // nothing measured: in their order
}

}  // namespace

// The service's box_sites answers.
void TestParsesSites() {
  using State = SiteCheck::State;
  const auto got = ParseSitesResponse(
      R"({"cmd":"box_sites","tag":"nl","pending":true,"results":[{"site":"youtube","state":"ok"},)"
      R"({"site":"chatgpt","state":"blocked"},{"site":"instagram","state":"fail","error":"i/o timeout"},)"
      R"({"site":"Bad Site!","state":"ok"},{"site":"telegram","state":"???"},{"state":"ok"},7]})");
  CHECK(got && got->pending && got->error.empty() && got->sites.size() == 4);
  if (!got || got->sites.size() < 4) {
    return;
  }
  CHECK(got->sites[0].site == "youtube" && got->sites[0].state == State::Ok);
  CHECK(got->sites[1].site == "chatgpt" && got->sites[1].state == State::Blocked);
  CHECK(got->sites[2].site == "instagram" && got->sites[2].state == State::Fail);
  CHECK(got->sites[3].site == "telegram" && got->sites[3].state == State::Fail);  // unknown: a failure
  const auto old = ParseSitesResponse(R"({"cmd":"box_sites","tag":"nl","error":"not supported by this core","results":[]})");
  CHECK(old && old->sites.empty() && old->error == "not supported by this core");
  CHECK(!ParseSitesResponse(R"({"cmd":"box_delays","results":[]})"));
  CHECK(!ParseSitesResponse("not json"));
}

// UDP through a server and what it carried for real (gocore/udpprobe.go, seen.go).
void TestParsesUdpAndSeen() {
  const auto delays = ParseDelaysResponse(
      R"({"cmd":"box_delays","results":[)"
      R"({"tag":"hy2","delay":48,"udp_delay":50,"udp_jitter":4,"udp_loss":300,"udp_error":"ignored","seen_down":90000,"seen_up":2000},)"
      R"({"tag":"naive","delay":60,"udp_error":"no udp"},)"
      R"({"tag":"tuic","error":"timeout","udp_delay":9,"seen_down":5000},)"
      R"({"tag":"de","pending":true,"seen_down":-5}]})");
  CHECK(delays && delays->size() == 4);
  if (!delays || delays->size() < 4) {
    return;
  }
  const Delay& hy2 = delays->at("hy2");
  CHECK(hy2.udpMs == 50 && hy2.udpJitter == 4 && hy2.udpLoss == 100 && hy2.udpError.empty());
  CHECK(hy2.seenDownKbps == 90000 && hy2.seenUpKbps == 2000);
  CHECK(delays->at("naive").udpMs == 0 && delays->at("naive").udpError == "no udp");
  const Delay& tuic = delays->at("tuic");
  CHECK(tuic.state == Delay::State::Failed && tuic.udpMs == 0 && tuic.seenDownKbps == 5000);
  CHECK(delays->at("de").seenDownKbps == 0);
}

// What the core's sov-auto reports (gocore/auto.go): a way that failed, and the UDP way.
void TestParsesAutoState() {
  const auto delays = ParseDelaysResponse(
      R"({"cmd":"box_delays","results":[)"
      R"({"tag":"a","delay":40,"failed_at":1762400000,"failed_why":"connection refused","udp_way":true},)"
      R"({"tag":"b","delay":50,"failed_at":-3,"failed_why":"x"}]})");
  CHECK(delays && delays->size() == 2);
  if (!delays || delays->size() < 2) {
    return;
  }
  CHECK(delays->at("a").failedAt == 1762400000 && delays->at("a").failedWhy == "connection refused" && delays->at("a").udpWay);
  CHECK(delays->at("b").failedAt == 0 && delays->at("b").failedWhy.empty() && !delays->at("b").udpWay);
}

// What the servers page says of UDP and of the rate seen.
void TestUdpDetail() {
  CHECK(MbitText(2'460) == L"2,5 Мбит/с");
  CHECK(MbitText(90'400) == L"90 Мбит/с");
  UiDelay delay{.state = UiDelay::State::Ok, .ms = 48, .udpMs = 50, .udpJitter = 4};
  CHECK(UdpDetail(delay, false).empty());  // fine: nothing to read past
  CHECK(UdpDetail(delay, true) == L"UDP 50 мс ±4");
  delay.udpLoss = 5;
  CHECK(UdpDetail(delay, false).empty() && UdpDetail(delay, true) == L"UDP 50 мс ±4, теряет 5%");
  delay.udpLoss = 20;
  CHECK(UdpDetail(delay, false) == L"UDP теряет 20%");
  const UiDelay naive{.state = UiDelay::State::Ok, .ms = 60, .udpError = L"no udp"};
  CHECK(UdpDetail(naive, false) == L"без UDP" && DelayDetail(naive, false) == L"без UDP");
  UiDelay broken{.state = UiDelay::State::Ok,
                 .ms = 60,
                 .udpError = L"dial udp [2001:db8::1]:19302: connect: network is unreachable"};
  CHECK(UdpDetail(broken, false) == L"UDP не отвечает");
  CHECK(UdpDetail(broken, true) == L"UDP: network is unreachable");
  broken.udpError = L"no answer";
  CHECK(UdpDetail(broken, true) == L"UDP не отвечает");
  const UiDelay failed{.state = UiDelay::State::Failed, .error = L"i/o timeout", .udpError = L"no udp"};
  CHECK(UdpDetail(failed, false).empty());
  UiDelay seen{.state = UiDelay::State::Ok, .ms = 48, .udpMs = 50, .udpJitter = 4, .seenDownKbps = 90'000};
  CHECK(DelayDetail(seen, false).empty());
  CHECK(DelayDetail(seen, true) == L"UDP 50 мс ±4 · качал до 90 Мбит/с");
}

// The answer to "замерить".
void TestParsesSpeed() {
  const auto done = ParseSpeedResponse(R"({"cmd":"box_speed","tag":"nl","kbps":91234})");
  CHECK(done && !done->pending && done->kbps == 91234 && done->error.empty());
  const auto going = ParseSpeedResponse(R"({"cmd":"box_speed","tag":"nl","pending":true})");
  CHECK(going && going->pending && going->kbps == 0);
  const auto busy = ParseSpeedResponse(R"({"cmd":"box_speed","tag":"fi","error":"another run is going","kbps":-5})");
  CHECK(busy && busy->error == "another run is going" && busy->kbps == 0);
  CHECK(!ParseSpeedResponse(R"({"cmd":"box_sites"})") && !ParseSpeedResponse("junk"));
}

// The servers page's columns and its folded servers' line.
void TestWays() {
  UiDelay way{.state = UiDelay::State::Ok, .ms = 180, .connect = 194, .udpMs = 182, .udpJitter = 3};
  CHECK(WayToneOf(way) == WayTone::Ok && WayNote(way).empty());
  CHECK(OpenText(way) == L"0,19 с" && GamesText(way) == L"±3 мс" && !GamesWarn(way));
  CHECK(SpeedText(way).empty());  // nothing seen: the page offers "замерить"
  way.seenDownKbps = 60'000;
  CHECK(SpeedText(way) == L"60 Мбит/с");
  way.measuredKbps = 140'000;
  CHECK(SpeedText(way) == L"140 Мбит/с");  // the run's figure over the seen one
  way.measuring = true;
  CHECK(SpeedText(way) == L"меряю…");
  way.udpJitter = 24;
  CHECK(GamesWarn(way));
  way.failedRecently = true;
  CHECK(WayToneOf(way) == WayTone::Warn && WayNote(way) == L"не открыл соединение — взял другой");
  const UiDelay naive{.state = UiDelay::State::Ok, .ms = 190, .connect = 1'204, .udpError = L"no udp"};
  CHECK(GamesText(naive) == L"нет UDP" && OpenText(naive) == L"1,20 с");
  const UiDelay dead{.state = UiDelay::State::Failed, .error = L"connection refused"};
  CHECK(WayToneOf(dead) == WayTone::Bad && WayNote(dead) == L"сервер отклонил соединение" && OpenText(dead).empty());
  CHECK(WayToneOf(UiDelay{}) == WayTone::Idle);
  CHECK(std::wstring(WaysWord(1)) == L"способ" && std::wstring(WaysWord(3)) == L"способа" &&
        std::wstring(WaysWord(5)) == L"способов" && std::wstring(WaysWord(12)) == L"способов");
  UiContent c;
  c.delays = {UiDelay{.state = UiDelay::State::Ok, .ms = 185, .connect = 412},
              UiDelay{.state = UiDelay::State::Ok, .ms = 186, .connect = 350}, dead};
  CHECK(ServerSummary(c, {0, 1}) == L"2 способа, оба работают · открытие от 0,35 с");
  CHECK(ServerSummary(c, {0, 1, 2}) == L"3 способа, работают 2 · открытие от 0,35 с");
  CHECK(ServerSummary(c, {2}) == L"сервер отклонил соединение");
  CHECK(ServerSummary(c, {0}) == L"открытие 0,41 с");
}

// The user's own network, judged by the way around the proxy.
void TestLocalNetwork() {
  Delay direct{.state = Delay::State::Ok, .ms = 12};
  CHECK(JudgeLocalNetwork(direct, 0, 12, false).state == LocalNetwork::State::Fine);
  direct.ms = 190;
  const LocalNetwork slow = JudgeLocalNetwork(direct, 0, 15, false);
  CHECK(slow.state == LocalNetwork::State::Slow && slow.forUser.find("190 мс (обычно 15)") != std::string::npos);
  CHECK(!slow.forSupport.empty());
  CHECK(JudgeLocalNetwork(direct, 0, std::nullopt, false).state == LocalNetwork::State::Slow);  // no best seen yet
  // A far ISP: 110 where it's usually 90 - not slow for it.
  direct.ms = 110;
  CHECK(JudgeLocalNetwork(direct, 0, 90, false).state == LocalNetwork::State::Fine);
  direct.ms = 20;
  direct.loss = 40;
  CHECK(JudgeLocalNetwork(direct, 0, 15, false).state == LocalNetwork::State::Lossy);
  const Delay failed{.state = Delay::State::Failed, .error = "timeout"};
  CHECK(JudgeLocalNetwork(failed, 1, 15, true).state == LocalNetwork::State::Down);
  // Directly not, the servers yes (a whitelist network): nothing said.
  CHECK(JudgeLocalNetwork(failed, 3, 15, false).state == LocalNetwork::State::Fine);
}

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestParsesResults();
    TestRejectsWhatIsNotAnAnswer();
    TestJudgeAuto();
    TestNextWarpServer();
    TestLocalNetwork();
    TestParsesSites();
    TestParsesUdpAndSeen();
    TestParsesAutoState();
    TestUdpDetail();
    TestWays();
    TestParsesSpeed();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 2;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

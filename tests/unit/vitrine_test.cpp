// The connect case's motion (src/tray/vitrine.h): the glass breaking into
// pieces that fill the hood, the box opening after it, the moves between
// states and where each ends.

// windows.h's min/max macros break std::min/max (clang-tidy in CI doesn't see
// the global NOMINMAX for every file); vitrine.h brings in d2d1.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>

#include "check.h"
#include "vitrine.h"

namespace {

using namespace sovereign::tray;

bool Close(float a, float b, float within = 0.01f) { return std::abs(a - b) < within; }

float HoodArea() {
  const auto hood = vitrine::Hood();
  float s = 0;
  for (std::size_t i = 0; i < hood.size(); ++i) {
    const vitrine::Point a = hood[i];
    const vitrine::Point b = hood[(i + 1) % hood.size()];
    s += a.x * b.y - b.x * a.y;
  }
  return std::abs(s) / 2;
}

void TestBreak() {
  const vitrine::Point at = vitrine::DefaultImpact();
  const vitrine::Shatter s = vitrine::BreakGlass(at, 42);
  CHECK(s.shards.size() > 40);  // 78 sites, the far ones mostly outside the hood
  // The pieces are the glass: together they cover it, less only the specks too small to draw.
  float area = 0;
  for (const vitrine::Shard& p : s.shards) {
    area += p.area;
    CHECK(p.local.size() >= 3 && p.area >= 4);
    CHECK(p.left <= 0 && p.right >= 0 && p.top <= 0 && p.bottom >= 0);  // the centre is inside its bounds
  }
  CHECK(area > HoodArea() * 0.99f && area < HoodArea() * 1.001f);
  // The cracks run out from the blow.
  CHECK(!s.cracks.empty());
  for (std::size_t i = 1; i < s.cracks.size(); ++i) {
    CHECK(s.cracks[i - 1].distance <= s.cracks[i].distance);
  }
  CHECK(Close(s.impact.x, at.x) && Close(s.impact.y, at.y));
  CHECK(!s.dust.empty());
  // The same pieces for the same seed; others for another.
  const vitrine::Shatter again = vitrine::BreakGlass(at, 42);
  CHECK(again.shards.size() == s.shards.size() && again.dust.size() == s.dust.size());
  CHECK(Close(again.shards[3].center.x, s.shards[3].center.x) && Close(again.shards[3].velocity.y, s.shards[3].velocity.y));
  const vitrine::Shatter other = vitrine::BreakGlass(at, 43);
  CHECK(other.shards.size() != s.shards.size() || !Close(other.shards[3].center.x, s.shards[3].center.x));
  // The pieces near the blow fly faster than the far ones, on the whole.
  float nearSpeed = 0;
  float farSpeed = 0;
  int nearCount = 0;
  int farCount = 0;
  for (const vitrine::Shard& p : s.shards) {
    const float d = std::hypot(p.center.x - at.x, p.center.y - at.y);
    const float v = std::hypot(p.velocity.x, p.velocity.y + 70);
    if (d < 80) {
      nearSpeed += v;
      ++nearCount;
    } else if (d > 200) {
      farSpeed += v;
      ++farCount;
    }
  }
  CHECK(nearCount > 0 && farCount > 0 && nearSpeed / static_cast<float>(nearCount) > farSpeed / static_cast<float>(farCount));
}

void TestAfterBreak() {
  const vitrine::Opened start = vitrine::AfterBreak(0, 1);
  CHECK(Close(start.lift, 0) && Close(start.open, 0) && Close(start.stream, 0) && Close(start.plinth, 1));
  const vitrine::Opened late = vitrine::AfterBreak(30, 1);
  CHECK(late.plinth < 0.01f && late.open > 0.98f && late.open < 1.02f && Close(late.stream, 1));
  CHECK(late.lift > 9 && late.lift < 15);  // up, bobbing a little
  CHECK(late.cone > 0.7f && late.pool > 0.45f);
  // Closed again (k 0): down, shut, the plinth back.
  const vitrine::Opened shut = vitrine::AfterBreak(30, 0);
  CHECK(Close(shut.lift, 0) && Close(shut.open, 0) && Close(shut.stream, 0) && Close(shut.plinth, 1) && Close(shut.pool, 0));
  // The flaps overshoot a little and settle.
  float most = 0;
  for (int step = 0; step < 200; ++step) {
    const double u = static_cast<double>(step) * 0.01;
    most = std::max(most, vitrine::AfterBreak(u, 1).open);
  }
  CHECK(most > 1 && most < 1.4f);
}

void TestStream() {
  CHECK(Close(vitrine::StreamFade(-25), 0) && vitrine::StreamFade(100) > 0.9f && vitrine::StreamFade(232) < 0.01f);
  // It widens as it rises; the near side is z 1.
  const vitrine::StreamPoint low = vitrine::StreamAt(0, 0);
  const vitrine::StreamPoint high = vitrine::StreamAt(150, 0);
  CHECK(high.x > low.x && high.y < low.y);
  CHECK(Close(vitrine::StreamAt(50, 1.5707964f).z, 1));
}

void TestRest() {
  const VitrineFrame on = VitrineRest(Display::On);
  CHECK(on.glass == 0 && on.shatter < 0 && on.opened.open > 0.9f && on.opened.plinth < 0.05f);
  const VitrineFrame off = VitrineRest(Display::Off);
  CHECK(off.glass == 1 && off.crack == 0 && off.opened.open == 0 && off.opened.plinth == 1);
  const VitrineFrame starting = VitrineRest(Display::Starting);
  CHECK(starting.glass == 1 && starting.crack > 140 && starting.crackAlpha > 0 && starting.shards);
  CHECK(VitrineRest(Display::Error).danger == 1 && VitrineRest(Display::ServiceDown).dim == 1);
  CHECK(VitrineRest(Display::Off).danger == 0 && VitrineRest(Display::Off).dim == 0);
}

void TestNoMotion() {
  // Animations off: each state at once, nothing moving.
  Vitrine v;
  v.To(Display::Off, 100, std::nullopt, false);
  CHECK(v.Known() && v.Shown() == Display::Off && !v.Moving(100));
  v.To(Display::On, 101, vitrine::Point{200, 300}, false);
  const VitrineFrame f = v.At(101);
  CHECK(f.glass == 0 && f.shatter < 0 && f.flash < 0 && f.opened.open > 0.9f && !v.Moving(101));
  v.To(Display::Off, 102, std::nullopt, false);
  CHECK(v.At(102).glass == 1 && !v.Moving(102));
}

void TestConnectAndBack() {
  Vitrine v;
  v.To(Display::Off, 10, std::nullopt, true);  // the window learns the state: no motion for it
  CHECK(!v.Moving(10) && v.At(10).glass == 1 && v.At(10).crack == 0);
  // A click on the glass: it cracks there.
  const vitrine::Point hit{220, 260};
  v.To(Display::Starting, 11, hit, true);
  CHECK(v.Moving(11.05));
  const VitrineFrame cracking = v.At(11.05);
  CHECK(cracking.glass == 1 && cracking.crack > 0 && cracking.crack < 140 && cracking.flash >= 0);
  CHECK(cracking.shards && Close(cracking.shards->impact.x, hit.x) && Close(cracking.shards->impact.y, hit.y));
  CHECK(v.At(14).crack > 250);  // creeping on while it connects
  // Connected: it shatters and the box opens.
  v.To(Display::On, 14, std::nullopt, true);
  const VitrineFrame breaking = v.At(14.3);
  CHECK(breaking.glass == 0 && breaking.shatter > 0 && breaking.flash >= 0 && !breaking.shardsBack);
  CHECK(breaking.shards && Close(breaking.shards->impact.x, hit.x));  // the same glass that cracked
  const VitrineFrame open = v.At(30);
  CHECK(open.shatter < 0 && open.flash < 0 && open.opened.open > 0.95f && open.opened.plinth < 0.01f &&
        open.opened.stream > 0.99f && Close(open.flow, 1));
  CHECK(!v.Moving(30));  // the spiral turns by its idle time; no quick frames are its motion's
  // Off: the shards fly back, the glass mends, the box closes on its plinth.
  v.To(Display::Off, 30, std::nullopt, true);
  const VitrineFrame mending = v.At(30.5);
  CHECK(mending.glass == 0 && mending.shardsBack && mending.shatter > 0);
  CHECK(v.Moving(30.5));
  const VitrineFrame mended = v.At(40);
  CHECK(mended.glass == 1 && mended.shatter < 0 && mended.crackAlpha <= 0 && mended.mendFlash < 0);
  CHECK(Close(mended.opened.open, 0) && Close(mended.opened.lift, 0) && Close(mended.opened.plinth, 1));
  CHECK(!v.Moving(40));
}

// Off answers on the click's next frames - the spiral going in and the light
// dimming at once, not after a slow start (19.11.2026: it stood a moment) -
// then the parts in turn: the spiral, the flaps, the box down, the glass. The
// shards settle into their places, they don't slam into them.
void TestClosingAnswersAtOnce() {
  using vitrine::kCrackTime;
  using vitrine::kHold;
  Vitrine v;
  v.To(Display::On, 0, std::nullopt, true);
  const VitrineFrame open = v.At(10);
  v.To(Display::Off, 10, std::nullopt, true);
  const VitrineFrame first = v.At(10);
  CHECK(Close(first.opened.stream, open.opened.stream) && Close(first.opened.open, open.opened.open, 0.02f) &&
        Close(first.opened.lift, open.opened.lift, 0.05f));  // from where it was: no jump
  const VitrineFrame soon = v.At(10.033);                    // two frames on
  CHECK(soon.opened.stream < open.opened.stream - 0.08f && soon.opened.cone < open.opened.cone - 0.03f);
  // The dust not standing in the air before the shards start back; in with them.
  CHECK(soon.shardsBack && Close(soon.dustIn, 0) && v.At(10.35).dustIn > 0.2f && Close(v.At(10.6).dustIn, 1));

  double streamIn = -1;
  double flapsShut = -1;
  double boxDown = -1;
  double landed = -1;
  for (int step = 0; step <= 400; ++step) {
    const double t = 10 + step * 0.005;
    const VitrineFrame f = v.At(t);
    if (streamIn < 0 && f.opened.stream < 0.01f) {
      streamIn = t;
    }
    if (flapsShut < 0 && std::abs(f.opened.open) < 0.01f) {
      flapsShut = t;
    }
    if (boxDown < 0 && f.opened.lift < 0.1f) {
      boxDown = t;
    }
    if (landed < 0 && f.glass > 0) {
      landed = t;
    }
  }
  CHECK(streamIn > 10 && streamIn < flapsShut && flapsShut < boxDown && boxDown < landed && landed < 11.1);
  // Flying back, slower near their places than on the way.
  const auto tau = [&](double t) { return v.At(t).shatter - kCrackTime - kHold; };
  CHECK(v.At(landed - 0.1).shardsBack && tau(landed - 0.1) > 0 && tau(landed - 0.1) < 0.05f);
  CHECK(tau(10.3) - tau(10.4) > 3 * (tau(landed - 0.15) - tau(landed - 0.05)));
  CHECK(v.At(landed + 0.5).glass == 1 && !v.Moving(12));
}

void TestTurnedOnWhileMending() {
  Vitrine v;
  v.To(Display::On, 0, std::nullopt, true);  // learnt on: broken long ago
  CHECK(v.At(1).glass == 0 && v.At(1).shatter < 0);
  v.To(Display::Off, 10, std::nullopt, true);
  CHECK(v.At(10.3).opened.open > 0.2f && v.At(10.3).opened.open < 0.95f);  // closing
  const VitrineFrame before = v.At(10.4);
  CHECK(before.shatter > 0 && before.shardsBack);
  // On again with the shards in the air: they turn round from where they
  // are, and the box opens again from where it had got to - no jump.
  v.To(Display::On, 10.4, std::nullopt, true);
  const VitrineFrame turned = v.At(10.4);
  CHECK(!turned.shardsBack && Close(turned.shatter, before.shatter, 0.02f) && turned.glass == 0);
  CHECK(Close(turned.opened.open, before.opened.open, 0.02f) && Close(turned.opened.lift, before.opened.lift, 0.05f));
  CHECK(v.At(10.6).shatter > turned.shatter && v.At(10.6).opened.open > turned.opened.open);
  CHECK(v.At(12).opened.open > 0.95f);
  // And off once more, mid-reopening: it closes from there too.
  Vitrine w;
  w.To(Display::On, 0, std::nullopt, true);
  w.To(Display::Off, 10, std::nullopt, true);
  w.To(Display::On, 10.4, std::nullopt, true);
  const VitrineFrame midway = w.At(10.6);
  w.To(Display::Off, 10.6, std::nullopt, true);
  CHECK(Close(w.At(10.6).opened.open, midway.opened.open, 0.02f));
  CHECK(w.At(20).glass == 1 && Close(w.At(20).opened.open, 0));
}

void TestGaveUpBeforeBreaking() {
  Vitrine v;
  v.To(Display::Off, 0, std::nullopt, true);
  v.To(Display::Starting, 1, std::nullopt, true);  // the tray's connect: the blow in the middle
  const VitrineFrame f = v.At(1.5);
  CHECK(f.crack > 0 && f.shards && Close(f.shards->impact.x, vitrine::DefaultImpact().x));
  // It failed: the cracks fade out of a whole glass, no shards.
  v.To(Display::Error, 2, std::nullopt, true);
  const VitrineFrame fading = v.At(2.1);
  CHECK(fading.glass == 1 && fading.crack > 0 && fading.crackAlpha > 0 && fading.crackAlpha < 1 && fading.shatter < 0);
  CHECK(v.At(2.1).danger > 0 && v.At(2.1).danger < 1);  // the red coming in
  const VitrineFrame failed = v.At(5);
  CHECK(failed.crackAlpha <= 0 && Close(failed.danger, 1) && !v.Moving(5));
  // The client retries by itself: no blow, the red stays.
  v.To(Display::Starting, 6, std::nullopt, true);
  CHECK(v.At(6.5).crack == 0 && Close(v.At(8).danger, 1));
  // He clicks: a blow where he did, the red goes.
  v.To(Display::Error, 9, std::nullopt, true);
  v.To(Display::Starting, 10, vitrine::Point{300, 200}, true);
  CHECK(v.At(10.5).crack > 0 && Close(v.At(12).danger, 0) && Close(v.At(10.5).shards->impact.x, 300));
}

void TestMissedTheGlass() {
  // A click the hood's bounds took but not its glass (a corner of the rect): the blow at the default spot.
  Vitrine v;
  v.To(Display::Off, 0, std::nullopt, true);
  v.To(Display::Starting, 1, vitrine::Point{2, 2}, true);
  const VitrineFrame f = v.At(1.2);
  CHECK(f.shards && Close(f.shards->impact.x, vitrine::DefaultImpact().x) &&
        Close(f.shards->impact.y, vitrine::DefaultImpact().y));
}

void TestTints() {
  Vitrine v;
  v.To(Display::Off, 0, std::nullopt, true);
  v.To(Display::ServiceDown, 1, std::nullopt, true);
  CHECK(v.At(1.2).dim > 0 && v.At(1.2).dim < 1 && Close(v.At(3).dim, 1));
  v.To(Display::Off, 4, std::nullopt, true);
  CHECK(Close(v.At(6).dim, 0) && !v.Moving(6));
  // A reconnect while connected (Starting with the glass broken): the spiral weakens, the glass stays broken.
  Vitrine w;
  w.To(Display::On, 0, std::nullopt, true);
  w.To(Display::Starting, 5, std::nullopt, true);
  const VitrineFrame weak = w.At(7);
  CHECK(weak.glass == 0 && weak.flow < 0.5f && weak.crack == 0);
  w.To(Display::On, 8, std::nullopt, true);
  CHECK(Close(w.At(10).flow, 1));
}

void TestTraffic() {
  // The VPN's rate as the spiral's 0..1: nothing for none, ~0.35 for 64 KB/s, full by 4 MB/s.
  CHECK(vitrine::TrafficLevel(0) == 0 && vitrine::TrafficLevel(-5) == 0 && vitrine::TrafficLevel(std::nan("")) == 0);
  CHECK(Close(vitrine::TrafficLevel(64.0 * 1024), 0.352f) && Close(vitrine::TrafficLevel(4e6), 1, 0.02f));
  float last = 0;
  for (int step = 1; step < 40; ++step) {
    const float level = vitrine::TrafficLevel(std::pow(2.0, step * 0.5));
    CHECK(level >= last && level <= 1);
    last = level;
  }
  // Busy, the spiral runs faster; no jump where the traffic comes or goes.
  Vitrine calm;
  calm.To(Display::On, 0, std::nullopt, true);
  Vitrine busy;
  busy.To(Display::On, 0, std::nullopt, true);
  busy.Traffic(1, 10);
  CHECK(std::abs(busy.At(10).flowClock - calm.At(10).flowClock) < 1e-6);
  CHECK(busy.Moving(10.5) && !busy.Moving(20));
  double was = busy.At(10).flowClock;
  bool smooth = true;
  for (int step = 1; step < 200; ++step) {
    const double now = 10 + step * 0.015;
    const double clock = busy.At(now).flowClock;
    smooth = smooth && clock > was && clock - was < 0.015 * 2.3;
    was = clock;
  }
  CHECK(smooth);
  const double calmRun = calm.At(21).flowClock - calm.At(20).flowClock;
  const double busyRun = busy.At(21).flowClock - busy.At(20).flowClock;
  CHECK(Close(static_cast<float>(calmRun), 1) && busyRun > calmRun * 2);
  CHECK(Close(busy.At(20).traffic, 1) && Close(calm.At(20).traffic, 0));
  busy.Traffic(0, 30);
  CHECK(busy.At(30.6).traffic > 0 && busy.At(30.6).traffic < 1);
  const double after = busy.At(41).flowClock - busy.At(40).flowClock;
  CHECK(Close(busy.At(40).traffic, 0) && std::abs(after - calmRun) < 1e-6);
  // The same level again changes nothing.
  busy.Traffic(0, 50);
  CHECK(!busy.Moving(50.1));
}

void TestUpload() {
  // The upload's share: what goes up of all; next to nothing either way is half and half.
  CHECK(Close(vitrine::UpShare(0, 0), 0.5f) && Close(vitrine::UpShare(-3, std::nan("")), 0.5f));
  CHECK(Close(vitrine::UpShare(1e6, 0), 0) && Close(vitrine::UpShare(0, 1e6), 1));
  CHECK(Close(vitrine::UpShare(3e5, 1e5), 0.25f) && Close(vitrine::UpShare(100, 0), 0.5f, 0.03f));
  CHECK(Close(vitrine::UpShare(1, std::numeric_limits<double>::infinity()), 0.5f));
  // Eased like the level: the packets turn round over a second, not at once.
  Vitrine v;
  v.To(Display::On, 0, std::nullopt, true);
  CHECK(Close(v.At(10).upShare, 0.5f) && !v.Moving(10));
  v.Upload(0, 10);
  const float midway = v.At(10.6).upShare;
  CHECK(midway > 0 && midway < 0.5f && v.Moving(10.6));
  CHECK(Close(v.At(12).upShare, 0) && !v.Moving(12));
  v.Upload(0, 20);
  CHECK(!v.Moving(20.1));
  CHECK(Close(VitrineRest(Display::On).upShare, 0.3f));
}

std::size_t Burning(const VitrineFrame& f) {
  return static_cast<std::size_t>(std::count_if(f.sparks.begin(), f.sparks.end(), [](const vitrine::Spark& s) { return s.age >= 0; }));
}

void TestSparks() {
  Vitrine v;
  v.To(Display::On, 0, std::nullopt, true);
  CHECK(Burning(v.At(10)) == 0);
  // One ad blocked: one red packet up the spiral and out, burning out in kSparkLife.
  v.Blocked(1, 10);
  const VitrineFrame f = v.At(10.1);
  CHECK(Burning(f) == 1 && Close(f.sparks[0].age, 0.1f) && f.sparks[0].lane >= 0.12f && f.sparks[0].lane <= 0.88f);
  CHECK(v.Moving(10.6) && Burning(v.At(11.25)) == 0 && !v.Moving(11.3));
  // A burst: in turn, kSparkGap apart, no more queued than burn out in kSparkLife.
  Vitrine w;
  w.To(Display::On, 0, std::nullopt, true);
  w.Blocked(50, 10);  // at 10, 10.2 ... 11.0
  CHECK(Burning(w.At(10)) == 1 && Burning(w.At(10.5)) == 3 && Burning(w.At(11.1)) == 6 && Burning(w.At(12.25)) == 0);
  // More while they burn: the new ones wait their turn; none put out early.
  w.Blocked(3, 10.3);  // two fit (11.2, 11.4), the third would wait too long
  CHECK(Burning(w.At(11.3)) == 6 && Burning(w.At(11.5)) == 6);
  CHECK(Burning(w.At(12.5)) == 1 && Burning(w.At(12.7)) == 0);
  // Each one on another strand or at another height.
  const VitrineFrame six = w.At(11.1);
  for (std::size_t i = 0; i < six.sparks.size(); ++i) {
    for (std::size_t j = i + 1; j < six.sparks.size(); ++j) {
      CHECK(std::abs(six.sparks.at(i).lane - six.sparks.at(j).lane) > 0.05f);
    }
  }
}

void TestStreamTop() {
  // Half the ceiling in the quiet, all of it at full, higher with more.
  CHECK(Close(vitrine::StreamTop(0), vitrine::kStreamTop / 2) && Close(vitrine::StreamTop(1), vitrine::kStreamTop));
  CHECK(Close(vitrine::StreamTop(-1), vitrine::StreamTop(0)) && Close(vitrine::StreamTop(3), vitrine::StreamTop(1)));
  CHECK(vitrine::StreamTop(0.3f) < vitrine::StreamTop(0.6f));
  // A low one fades out at its own top, whole below it.
  const float low = vitrine::StreamTop(0);
  CHECK(vitrine::StreamFade(low, low) < 0.01f && vitrine::StreamFade(low / 3, low) > 0.85f);
  CHECK(Close(vitrine::StreamFade(100), vitrine::StreamFade(100, vitrine::kStreamTop)));
}

void TestLinkTrouble() {
  // A good link is calm; losses from a few percent, all of it by 15.
  CHECK(Close(vitrine::LinkTrouble(0, 5, 60, false), 0));
  CHECK(vitrine::LinkTrouble(10, 0, 60, false) > 0.5f && Close(vitrine::LinkTrouble(20, 0, 60, false), 1));
  float was = 0;
  for (int loss = 0; loss <= 30; ++loss) {
    const float now = vitrine::LinkTrouble(loss, 0, 60, false);
    CHECK(now >= was);
    was = now;
  }
  // The latency jumping: 40 ms of 80 is bad, 40 of 400 a far server.
  CHECK(vitrine::LinkTrouble(0, 40, 80, false) > 0.9f && Close(vitrine::LinkTrouble(0, 40, 400, false), 0));
  // A connection it couldn't open lately: a good part.
  CHECK(vitrine::LinkTrouble(0, 0, 60, true) >= 0.7f && vitrine::LinkTrouble(-5, -5, 0, false) == 0);
}

void TestShaky() {
  // Eased in and out, still once it's there.
  Vitrine v;
  v.To(Display::On, 0, std::nullopt, true);
  CHECK(Close(v.At(10).shaky, 0) && !v.Moving(10));
  v.Shaky(1, 10);
  const float midway = v.At(10.4).shaky;
  CHECK(midway > 0 && midway < 1 && v.Moving(10.4));
  CHECK(Close(v.At(11).shaky, 1) && !v.Moving(11));
  v.Shaky(1, 12);
  CHECK(!v.Moving(12.1));
  v.Shaky(5, 13);  // no more than all
  CHECK(Close(v.At(14).shaky, 1));
  v.Shaky(0, 20);
  CHECK(v.At(20.4).shaky > 0 && Close(v.At(21).shaky, 0));
  CHECK(Close(VitrineRest(Display::On).shaky, 0));
}

void TestPlace() {
  VitrinePlace place;
  place.scale = 0.4f;
  place.origin = {30, -12};
  const D2D1_RECT_F button = FromScene(place, vitrine::HoodBounds());
  const VitrinePlace back = VitrinePlaceFor(button);
  CHECK(Close(back.scale, place.scale, 1e-4f) && Close(back.origin.x, place.origin.x, 1e-3f) &&
        Close(back.origin.y, place.origin.y, 1e-3f));
  const vitrine::Point c = vitrine::HoodCenter();
  const vitrine::Point there = ToScene(place, {place.origin.x + c.x * place.scale, place.origin.y + c.y * place.scale});
  CHECK(Close(there.x, c.x, 1e-3f) && Close(there.y, c.y, 1e-3f));
  // The box stands inside the hood.
  const D2D1_RECT_F box = vitrine::BoxBounds();
  const D2D1_RECT_F hood = vitrine::HoodBounds();
  CHECK(box.left > hood.left && box.right < hood.right && box.top > hood.top && box.bottom < hood.bottom);
  CHECK(hood.top > vitrine::kDrawnTop && hood.bottom < vitrine::kDrawnBottom);
  CHECK(Close(EaseOut(0), 0) && Close(EaseOut(1), 1) && EaseOut(0.5f) > 0.5f);  // fast first, settling
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) - see the catch below
  try {
    TestBreak();
    TestAfterBreak();
    TestStream();
    TestRest();
    TestNoMotion();
    TestConnectAndBack();
    TestClosingAnswersAtOnce();
    TestTurnedOnWhileMending();
    TestGaveUpBeforeBreaking();
    TestMissedTheGlass();
    TestTints();
    TestTraffic();
    TestUpload();
    TestSparks();
  TestStreamTop();
  TestLinkTrouble();
  TestShaky();
    TestPlace();
  } catch (const std::exception& e) {
    std::cerr << "unexpected exception: " << e.what() << "\n";
    return 1;
  }
  return sovereign::test::Failures() == 0 ? 0 : 1;
}

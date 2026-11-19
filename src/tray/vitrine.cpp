// windows.h's min/max macros break std::min/max (clang-tidy in CI doesn't see
// the global NOMINMAX for every file); vitrine.h brings in d2d1.h, so before it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "vitrine.h"

#include <d2d1_1.h>
#include <d2d1effects.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <tuple>
#include <utility>

namespace sovereign::tray {

float EaseOut(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const float rest = 1 - t;
  return 1 - rest * rest * rest;
}

namespace vitrine {
namespace {

// The box's picture was drawn 1408 pixels across; the scene is 520 units.
constexpr float kPx = kScene / 1408.0f;
constexpr float kBoxLeft = 198 * kPx;
constexpr float kBoxRight = 1216 * kPx;
constexpr float kBoxTop = 160 * kPx;
constexpr float kBoxBottom = 1250 * kPx;
constexpr float kCx = (kBoxLeft + kBoxRight) / 2;
constexpr float kBoxH = kBoxBottom - kBoxTop;
constexpr float kOy = kBoxTop + kBoxH * 0.75f;  // the box's lower side corners: where it stands

constexpr Point Add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
constexpr Point Sub(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
constexpr Point Times(Point a, float s) { return {a.x * s, a.y * s}; }
constexpr Point Mid(Point a, Point b) { return {(a.x + b.x) / 2, (a.y + b.y) / 2}; }
constexpr Point Lerp(Point a, Point b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
constexpr Point Down(Point p, float d) { return {p.x, p.y + d}; }
// Pushed out by `s` round the box's foot.
constexpr Point Out(Point p, float s) { return {kCx + s * (p.x - kCx), kOy + s * (p.y - kOy)}; }

// The box's outline: top, upper right, the near top corner, upper left,
// lower right, lower left, bottom.
constexpr Point kBoxT{kCx, kBoxTop};
constexpr Point kBoxUR{kBoxRight, kBoxTop + kBoxH * 0.25f};
constexpr Point kBoxC{kCx, kBoxTop + kBoxH * 0.5f};
constexpr Point kBoxUL{kBoxLeft, kBoxTop + kBoxH * 0.25f};
constexpr Point kBoxLR{kBoxRight, kOy};
constexpr Point kBoxLL{kBoxLeft, kOy};
constexpr Point kBoxB{kCx, kBoxBottom};

// The glass hood: the box's outline a little bigger.
constexpr float kGlass = 1.13f;
constexpr Point kGT = Out(kBoxT, kGlass);
constexpr Point kGUR = Out(kBoxUR, kGlass);
constexpr Point kGC = Out(kBoxC, kGlass);
constexpr Point kGUL = Out(kBoxUL, kGlass);
constexpr Point kGLR = Out(kBoxLR, kGlass);
constexpr Point kGLL = Out(kBoxLL, kGlass);
constexpr Point kGB = Out(kBoxB, kGlass);

// The plinth: a flat slab under it, its top a rhombus, `kThick` deep.
constexpr float kPlinth = 1.22f;
constexpr float kThick = 24;
constexpr float kHalfW = kBoxRight - kCx;
constexpr float kHalfH = kBoxBottom - kOy;
constexpr Point kPL{kCx - kHalfW * kPlinth, kOy};
constexpr Point kPR{kCx + kHalfW * kPlinth, kOy};
constexpr Point kPF{kCx, kOy + kHalfH * kPlinth};
constexpr Point kPK{kCx, kOy - kHalfH * kPlinth};

// The lid on the picture (measured on it): back, right, front, left; its
// two sides from the back corner; how far up a flap's free edge goes.
constexpr Point kLidT{261, 60};
constexpr Point kLidR{449, 148};
constexpr Point kLidF{265, 248};
constexpr Point kLidL{75, 148};
constexpr Point kLidU = Sub(kLidR, kLidT);
constexpr Point kLidW = Sub(kLidL, kLidT);
constexpr float kFlapRise = 106;
constexpr float kOpenBack = 108;   // degrees the back flaps open
constexpr float kOpenFront = 118;  // the front ones
constexpr float kPictureGlow = 0.28f;  // the flow's warmth over the opened picture's own

// The spiral over the box's mouth.
constexpr float kStreamX = 263;
constexpr float kStreamY = 158;
constexpr float kStreamTop = 232;
constexpr float kPitch = 120;
constexpr float kTurn = 1.15f;
// Its three ribbons, one inside another: where round the turn, how wide
// their cone (of StreamAt's), the ribbon and how bright; the inner ones
// coil tighter and turn faster, a vortex's core - one pitch for all read as
// a spring.
struct Strand {
  float phase;
  float radius;
  float width;
  float light;
  float pitch;  // of kPitch
  float spin;   // of kTurn
};
constexpr std::array<Strand, 3> kStrands{{
    {0, 1, 1, 1, 1, 1},
    {2.0944f, 0.8f, 0.7f, 0.75f, 0.8f, 1.3f},
    {4.18879f, 0.6f, 0.5f, 0.55f, 0.64f, 1.7f},
}};
constexpr float kRibbon = 9;  // the outer ribbon facing the eye, units

constexpr float kGravity = 650;
constexpr float kDriftX = 110;
constexpr float kDriftY = -80;
constexpr float kTwoPi = 2 * std::numbers::pi_v<float>;
constexpr float kDegrees = std::numbers::pi_v<float> / 180;

// mulberry32: the prototype's generator - small, and the same pieces for a seed.
class Rng {
 public:
  explicit Rng(std::uint32_t seed) : state_(seed) {}
  float operator()() {
    state_ += 0x6D2B79F5u;
    std::uint32_t t = state_;
    t = (t ^ (t >> 15)) * (1u | t);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return static_cast<float>((t ^ (t >> 14)) >> 8) * (1.0f / 16777216.0f);
  }

 private:
  std::uint32_t state_;
};

float Area(const std::vector<Point>& p) {
  float s = 0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const Point a = p[i];
    const Point b = p[(i + 1) % p.size()];
    s += a.x * b.y - b.x * a.y;
  }
  return s / 2;
}

Point Centroid(const std::vector<Point>& p) {
  Point c;
  for (const Point q : p) {
    c.x += q.x;
    c.y += q.y;
  }
  const auto n = static_cast<float>(p.size());
  return {c.x / n, c.y / n};
}

template <typename Outline>
bool InsidePolygon(const Outline& poly, Point pt) {
  bool in = false;
  const std::size_t n = std::size(poly);
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Point a = poly[i];
    const Point b = poly[j];
    if ((a.y > pt.y) != (b.y > pt.y) && pt.x < (b.x - a.x) * (pt.y - a.y) / (b.y - a.y) + a.x) {
      in = !in;
    }
  }
  return in;
}

// The part of `poly` nearer `a` than `b`.
std::vector<Point> Clip(const std::vector<Point>& poly, Point a, Point b) {
  const float nx = b.x - a.x;
  const float ny = b.y - a.y;
  const Point m = Mid(a, b);
  const auto side = [&](Point p) { return (p.x - m.x) * nx + (p.y - m.y) * ny; };
  std::vector<Point> out;
  out.reserve(poly.size() + 1);
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const Point p = poly[i];
    const Point q = poly[(i + 1) % poly.size()];
    const float fp = side(p);
    const float fq = side(q);
    if (fp <= 0) {
      out.push_back(p);
    }
    if ((fp < 0 && fq > 0) || (fp > 0 && fq < 0)) {
      const float t = fp / (fp - fq);
      out.push_back(Lerp(p, q, t));
    }
  }
  return out;
}

bool Near(Point a, Point b) { return std::abs(a.x - b.x) < 0.05f && std::abs(a.y - b.y) < 0.05f; }

float Smooth(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3 - 2 * x);
}

}  // namespace

std::array<Point, 6> Hood() { return {kGT, kGUR, kGLR, kGB, kGLL, kGUL}; }

Point HoodCenter() { return kGC; }

D2D1_RECT_F HoodBounds() { return {kGUL.x, kGT.y, kGUR.x, kGB.y}; }

D2D1_RECT_F BoxBounds() { return {kBoxLeft, kBoxTop, kBoxRight, kBoxBottom}; }

Point DefaultImpact() { return {kCx + 30, 290}; }

Shatter BreakGlass(Point impact, std::uint32_t seed) {
  Rng r(seed);
  constexpr int kSites = 78;
  std::vector<Point> sites;
  sites.reserve(kSites);
  sites.push_back(impact);
  for (int i = 1; i < kSites; ++i) {
    const float a = r() * kTwoPi;
    const float d = 350 * std::pow(r(), 1.9f) + 8;
    sites.push_back({impact.x + std::cos(a) * d, impact.y + std::sin(a) * d * 0.9f});
  }
  Shatter out;
  out.impact = impact;
  out.shards.reserve(sites.size());
  const std::array<Point, 6> hood = Hood();
  for (std::size_t i = 0; i < sites.size(); ++i) {
    std::vector<Point> poly(hood.begin(), hood.end());
    for (std::size_t j = 0; j < sites.size() && poly.size() >= 3; ++j) {
      if (j != i) {
        poly = Clip(poly, sites[i], sites[j]);
      }
    }
    if (poly.size() < 3) {
      continue;
    }
    const Point c = Centroid(poly);
    const float area = std::abs(Area(poly));
    if (area < 4) {
      continue;
    }
    // Its edges are the cracks (each once: a neighbour has the same edge).
    for (std::size_t q = 0; q < poly.size(); ++q) {
      const Point a = poly[q];
      const Point b = poly[(q + 1) % poly.size()];
      if (Near(a, b)) {
        continue;
      }
      const bool seen = std::any_of(out.cracks.begin(), out.cracks.end(), [&](const Crack& k) {
        return (Near(k.a, a) && Near(k.b, b)) || (Near(k.a, b) && Near(k.b, a));
      });
      if (!seen) {
        const Point m = Mid(a, b);
        out.cracks.push_back({a, b, std::hypot(m.x - impact.x, m.y - impact.y)});
      }
    }
    Shard s;
    s.center = c;
    s.area = area;
    s.local.reserve(poly.size());
    s.left = s.top = 1e9f;
    s.right = s.bottom = -1e9f;
    for (const Point p : poly) {
      const Point l = Sub(p, c);
      s.local.push_back(l);
      s.left = std::min(s.left, l.x);
      s.right = std::max(s.right, l.x);
      s.top = std::min(s.top, l.y);
      s.bottom = std::max(s.bottom, l.y);
    }
    const float dx = c.x - impact.x;
    const float dy = c.y - impact.y;
    const float d = std::hypot(dx, dy) + 1e-3f;
    const float reach = std::min(1.0f, d / 350);  // 0 at the blow .. 1 far from it
    const float angle = std::atan2(dy, dx) + (r() - 0.5f) * 0.6f;
    const float speed = (540 - 400 * reach) * (0.7f + 0.6f * r());
    s.velocity = {std::cos(angle) * speed, std::sin(angle) * speed - 70};
    s.spin = (r() - 0.5f) * 7;
    s.grow = -0.2f + r() * 1.1f;
    s.flip = 4 + r() * 9;
    s.spark = r() < 0.22f;
    s.vanish = 0.10f + reach * 0.6f + r() * 0.18f;
    // Its dust: specks from inside it, leaving as it fades.
    const int specks = std::min(60, static_cast<int>(std::lround(area / 70)));
    for (int q = 0; q < specks; ++q) {
      Point lp;
      for (int tries = 0; tries < 10; ++tries) {
        const Point t{s.left + (s.right - s.left) * r(), s.top + (s.bottom - s.top) * r()};
        if (InsidePolygon(s.local, t)) {
          lp = t;
          break;
        }
      }
      Speck p;
      p.start = s.vanish + r() * 0.35f;
      const float zs = 1 + s.grow * p.start;
      p.life = 0.8f + r() * 1.3f;
      p.x = c.x + lp.x * zs + s.velocity.x * p.start;
      p.y = c.y + lp.y * zs + s.velocity.y * p.start + 0.5f * kGravity * p.start * p.start;
      p.vx = s.velocity.x * 0.4f + (r() - 0.5f) * 110;
      p.vy = s.velocity.y * 0.4f + (r() - 0.5f) * 110;
      p.wobbleRate = 4 + r() * 9;
      p.wobblePhase = r() * 6.28f;
      p.wobble = 5 + r() * 20;
      p.size = 0.9f + r() * 1.9f;
      (r() < 0.07f ? out.glints : out.dust).push_back(p);
    }
    out.shards.push_back(std::move(s));
  }
  std::sort(out.cracks.begin(), out.cracks.end(),
            [](const Crack& a, const Crack& b) { return a.distance < b.distance; });
  return out;
}

Opened AfterBreak(double u, float k) {
  // Closing (k going down), the parts go in turn - the spiral back into the
  // box, the flaps, then the box down onto its plinth; opening again, the
  // other way round. Each takes its own stretch of k.
  const double kk = k;
  const double kLift = Smooth(k / 0.6f);
  const double kOpen = Smooth((k - 0.2f) / 0.6f);
  // The spiral answers at once - no slow start at the top of its stretch, the
  // click's first frames move it - and settles into the box softly.
  const double sx = std::clamp((kk - 0.45) / 0.55, 0.0, 1.0);
  const double kStream = sx * sx * (2 - sx);
  // The light comes at once; the box has weight: it starts up from rest
  // (critically damped), then floats, bobbing.
  const double e = (1 - std::exp(-u * 3.2)) * kk;
  const double w = 4.2 * u;
  const double rise = 1 - (1 + w) * std::exp(-w);
  const double bob = u > 0.9 ? std::sin((u - 0.9) * 2.3) * std::min(1.0, u - 0.9) : 0.0;
  const double burst = std::exp(-u * 4) * std::min(1.0, u * 14) * kk;
  // The flaps: a spring let go from rest - slow to start, a little past open, back.
  constexpr double kZeta = 0.55;
  constexpr double kOmega = 8.5;
  const double damped = kOmega * std::sqrt(1 - kZeta * kZeta);
  const double ou = std::max(0.0, u - 0.06);
  const double swing = 1 - std::exp(-kZeta * kOmega * ou) *
                               (std::cos(damped * ou) + kZeta * kOmega / damped * std::sin(damped * ou));
  const double s0 = std::clamp((u - 0.3) / 0.8, 0.0, 1.0);
  Opened o;
  o.lift = static_cast<float>((12 * rise + 2.2 * bob) * kLift);
  o.pool = static_cast<float>((0.28 + 0.22 * e) * kk);
  o.cone = static_cast<float>((e + 0.9 * burst) * 0.8);
  o.motes = static_cast<float>(e);
  o.rim = static_cast<float>(0.25 * e);
  o.plinth = static_cast<float>(1 - e);
  o.open = static_cast<float>(kOpen * swing);
  o.stream = static_cast<float>(kStream * s0 * s0 * (3 - 2 * s0));
  return o;
}

StreamPoint StreamAt(float h, float phi) {
  const float r = std::min(100.0f, 22 + 50 * (1 - std::exp(-h / 60)) + 0.16f * std::max(0.0f, h));
  return {kStreamX + r * std::cos(phi), kStreamY - h + r * 0.5f * std::sin(phi), std::sin(phi)};
}

float StreamFade(float h) {
  return Smooth((h + 25) / 55) * Smooth((kStreamTop - h) / 95);
}

// On a log scale from 8 KB/s: 64 KB/s about a third, 512 KB/s two thirds, 4 MB/s all.
float TrafficLevel(double bytesPerSecond) {
  if (!(bytesPerSecond > 0)) {  // NaN too
    return 0;
  }
  return std::clamp(static_cast<float>(std::log2(1 + bytesPerSecond / 8192) / 9), 0.0f, 1.0f);
}

// Under 2 KB/s both ways the share means nothing: it leans to half and half.
float UpShare(double downBytesPerSecond, double upBytesPerSecond) {
  const double down = downBytesPerSecond > 0 ? downBytesPerSecond : 0;  // NaN too
  const double up = upBytesPerSecond > 0 ? upBytesPerSecond : 0;
  const double all = down + up;
  if (!(all > 0) || !std::isfinite(all)) {
    return 0.5f;
  }
  constexpr double kQuiet = 2048;
  const double weight = std::min(1.0, all / kQuiet);
  return std::clamp(static_cast<float>(0.5 + (up / all - 0.5) * weight), 0.0f, 1.0f);
}

}  // namespace vitrine

// ---------------------------------------------------------------------------
// The motion.

namespace {

using vitrine::kCrackTime;
using vitrine::kHold;
using vitrine::kSparkLife;

constexpr float kCrackFirst = 140;  // units the cracks run at the blow
constexpr float kCrackMore = 220;   // and creep on while it connects
constexpr float kHealFade = 0.45f;  // cracks fading when it gave up before breaking
constexpr float kMendClose = 0.8f;  // the box closing and sinking (AfterBreak's parts in turn)
constexpr float kMendDelay = 0.2f;  // before the shards start back
constexpr float kMendRate = 1.6f;   // their flight's seconds per second, backwards, on average
constexpr float kMendCap = 1.16f;   // from no further than this (the last faded by then: vanish 0.88 + 0.28)
constexpr float kMendDustIn = 0.3f; // their dust fading in as they start
constexpr float kMendFade = 0.3f;   // the whole glass fading in over them
constexpr float kMendGlow = 0.5f;   // its soft flash
constexpr float kTint = 0.45f;      // the error's red, the dimming, the flow easing
constexpr float kFlowWeak = 0.3f;   // the spiral while it reconnects
constexpr float kFlowPace = 0.35f;  // its speed at no flow (all of it at 1): it falters, not stops
constexpr float kHoverIn = 0.2f;    // the pointer coming onto the case, leaving it
constexpr float kHoverOut = 0.35f;
constexpr float kPressIn = 0.07f;   // the press taking, letting go
constexpr float kPressOut = 0.3f;
constexpr float kTrafficEase = 1.2f; // the traffic's level easing: the rates come once a second
constexpr float kBusyPace = 1.2f;    // the spiral's speed added at full traffic (its own is 1)
constexpr float kSparkGap = 0.15f;   // blocked ads at once: their sparks this far apart
constexpr float kFlightEnd = 3.6f;  // the last dust gone
constexpr double kNever = -1e9;
// A flight at rest on: the box up, open, between two bobs.
constexpr double kRestFlight = 0.9 + 2 * std::numbers::pi / 2.3;
constexpr std::uint32_t kRestSeed = 7;

float CrackReach(float t) {
  if (t <= 0) {
    return 0;
  }
  if (t < kCrackTime) {
    return std::pow(t / kCrackTime, 0.55f) * kCrackFirst;
  }
  return kCrackFirst + kCrackMore * (1 - std::exp(-(t - kCrackTime) / 2.5f));
}

float Smooth01(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3 - 2 * x);
}

float Eased(float from, float to, double at, double now, float span = kTint) {
  return from + (to - from) * Smooth01(static_cast<float>(now - at) / span);
}

// Eased's integral from `at` to `now`: how far a clock running at its speed goes.
double EasedSum(float from, float to, double at, double now, float span = kTint) {
  const double d = std::max(0.0, now - at);
  const double x = std::min(1.0, d / span);
  const double eased = span * (x * x * x - x * x * x * x / 2) + std::max(0.0, d - span);  // Smooth01's integral
  return from * d + (to - from) * eased;
}

std::shared_ptr<const vitrine::Shatter> MakeShatter(vitrine::Point impact, std::uint32_t seed) {
  return std::make_shared<const vitrine::Shatter>(vitrine::BreakGlass(impact, seed));
}

vitrine::Point ImpactFor(std::optional<vitrine::Point> hit) {
  if (hit && vitrine::InsidePolygon(vitrine::Hood(), *hit)) {
    return *hit;
  }
  return vitrine::DefaultImpact();
}

}  // namespace

VitrineFrame VitrineRest(Display display) {
  static const std::shared_ptr<const vitrine::Shatter> rest = MakeShatter(vitrine::DefaultImpact(), kRestSeed);
  VitrineFrame f;
  f.display = display;
  f.idle = 1.3;
  f.flowClock = f.idle;
  f.traffic = 0.3f;  // a page or two loading
  f.upShare = 0.3f;  // most of it coming in
  f.shards = rest;
  switch (display) {
    case Display::On:
      f.glass = 0;
      f.opened = vitrine::AfterBreak(kRestFlight, 1);
      break;
    case Display::Starting:
      f.crack = CrackReach(3);
      f.crackAlpha = 0.9f;
      f.opened.pool = 0.12f;
      break;
    case Display::Error:
      f.danger = 1;
      break;
    case Display::ServiceDown:
      f.dim = 1;
      break;
    case Display::Off:
      break;
  }
  return f;
}

float Vitrine::DangerAt(double now) const { return Eased(dangerFrom_, dangerTo_, dangerAt_, now); }
float Vitrine::FlowAt(double now) const { return Eased(flowFrom_, flowTo_, flowAt_, now); }
float Vitrine::DimAt(double now) const { return Eased(dimFrom_, dimTo_, dimAt_, now); }

// The spiral's clock runs at its speed, kFlowPace at no flow and whole at
// full: the flow's easing integrated, so a change of speed never jumps it.
double Vitrine::FlowClock(double now) const {
  const double d = std::max(0.0, now - flowAt_);
  return flowClock_ + kFlowPace * d + (1 - kFlowPace) * EasedSum(flowFrom_, flowTo_, flowAt_, now);
}

float Vitrine::TrafficAt(double now) const { return Eased(trafficFrom_, trafficTo_, trafficAt_, now, kTrafficEase); }

double Vitrine::TrafficSum(double now) const {
  return trafficSum_ + EasedSum(trafficFrom_, trafficTo_, trafficAt_, now, kTrafficEase);
}

void Vitrine::Traffic(float level, double now) {
  level = std::clamp(level, 0.0f, 1.0f);
  if (level == trafficTo_) {
    return;
  }
  trafficSum_ = TrafficSum(now);
  trafficFrom_ = TrafficAt(now);
  trafficTo_ = level;
  trafficAt_ = now;
}

float Vitrine::UpAt(double now) const { return Eased(upFrom_, upTo_, upAt_, now, kTrafficEase); }

void Vitrine::Upload(float share, double now) {
  share = std::clamp(share, 0.0f, 1.0f);
  if (share == upTo_) {
    return;
  }
  upFrom_ = UpAt(now);
  upTo_ = share;
  upAt_ = now;
}

void Vitrine::Blocked(std::uint64_t count, double now) {
  for (std::uint64_t i = 0; i < count; ++i) {
    // After the ones still to come; none if they'd queue up past a spark's life.
    const double at = std::max(now, lastStrike_ + kSparkGap);
    if (at - now >= kSparkLife) {
      return;
    }
    // Round the rim in golden steps: never twice in one spot, never bunched.
    const double step = 0.381966 * struck_ + 0.3;
    const auto where = static_cast<float>(0.12 + 0.76 * (step - std::floor(step)));
    strikes_.at(struck_ % strikes_.size()) = {.at = at, .where = where};
    lastStrike_ = at;
    ++struck_;
  }
}

float Vitrine::Ease::At(double now) const {
  return span <= 0 ? to : from + (to - from) * EaseOut(static_cast<float>(now - at) / span);
}

bool Vitrine::Ease::Set(float value, double now, float in, float out) {
  if (value == to) {
    return false;
  }
  from = At(now);
  to = value;
  at = now;
  span = value > from ? in : out;
  return true;
}

bool Vitrine::Touch(bool hovered, bool pressed, double now, bool motion) {
  const bool hoverMoved = hover_.Set(hovered ? 1.0f : 0.0f, now, motion ? kHoverIn : 0, motion ? kHoverOut : 0);
  const bool pressMoved = press_.Set(pressed ? 1.0f : 0.0f, now, motion ? kPressIn : 0, motion ? kPressOut : 0);
  return hoverMoved || pressMoved;
}

float Vitrine::MendTau(double now) const {
  // Back fast, slowing as they reach their places: they settle, not slam.
  const float span = mendTau_ / kMendRate;
  const float back = static_cast<float>(now - mendAt_) - kMendDelay;
  if (back <= 0) {
    return mendTau_;
  }
  if (back >= span) {
    return 0;
  }
  const float left = 1 - back / span;
  return mendTau_ * left * left;
}

double Vitrine::MendedAt() const { return mendAt_ + kMendDelay + mendTau_ / kMendRate; }

Vitrine::Box Vitrine::BoxAt(double now) const {
  switch (glass_) {
    case Glass::Broken:
      if (reopenAt_ >= 0) {
        const double since = now - reopenAt_;
        return {reopenU_ + std::max(0.0, since),
                reopenK_ + (1 - reopenK_) * Smooth01(static_cast<float>(since) / kMendClose)};
      }
      return {std::max(0.0, now - breakAt_ - kHold), 1};
    case Glass::Mending: {
      // The click's first frames move it: the closing starts at speed, not
      // from rest as a smoothstep did (it stood a moment, 19.11.2026), and
      // lands softly - 1.5x - 0.5x^3, slope 1.5 at the start, 0 at the end.
      const float x = std::clamp(static_cast<float>(now - mendAt_) / kMendClose, 0.0f, 1.0f);
      return {mendU_, mendK_ * (1 - (1.5f * x - 0.5f * x * x * x))};
    }
    case Glass::Whole:
    case Glass::Cracking:
      break;
  }
  return {};
}

void Vitrine::To(Display display, double now, std::optional<vitrine::Point> hit, bool motion) {
  if (known_ && display == display_ && motion == motion_) {
    return;
  }
  const Display from = display_;
  const bool settle = !known_ || !motion;
  known_ = true;
  motion_ = motion;
  display_ = display;
  const vitrine::Point impact = ImpactFor(hit);
  const bool struck = hit.has_value();
  const auto next = [this] {
    seed_ = seed_ * 1664525u + 1013904223u;
    return seed_;
  };

  // The tints: red for the error (kept through a retry it started itself), dim for the service down.
  float danger = display == Display::Error ? 1.0f : 0.0f;
  if (display == Display::Starting && from == Display::Error && !struck) {
    danger = dangerTo_;
  }
  const float dim = display == Display::ServiceDown ? 1.0f : 0.0f;

  if (settle) {
    // Straight into the state's pose: the window just learnt it, or there's no motion.
    dangerFrom_ = dangerTo_ = danger;
    dimFrom_ = dimTo_ = dim;
    flowFrom_ = flowTo_ = 1;
    dangerAt_ = dimAt_ = flowAt_ = now - 100;
    flowClock_ = flowAt_;  // the spiral's clock on the window's
    flashAt_ = kNever;
    healAt_ = -1;
    reopenAt_ = -1;
    switch (display) {
      case Display::On:
        shatter_ = MakeShatter(impact, next());
        glass_ = Glass::Broken;
        breakAt_ = now - 100;
        crackAt_ = breakAt_ - kCrackTime;
        break;
      case Display::Starting:
        shatter_ = MakeShatter(impact, next());
        glass_ = Glass::Cracking;
        crackAt_ = now - 10;
        break;
      default:
        glass_ = Glass::Whole;
        break;
    }
    return;
  }

  if (danger != dangerTo_) {
    dangerFrom_ = DangerAt(now);
    dangerTo_ = danger;
    dangerAt_ = now;
  }
  if (dim != dimTo_) {
    dimFrom_ = DimAt(now);
    dimTo_ = dim;
    dimAt_ = now;
  }

  // Mid-mend and turned on again: the shards still flying back turn round,
  // the box opens again from where it had closed to.
  const bool backInFlight = glass_ == Glass::Mending && MendTau(now) > 0;
  const bool whole = glass_ == Glass::Whole || (glass_ == Glass::Mending && !backInFlight);
  const auto turnRound = [&] {
    const Box box = BoxAt(now);
    reopenAt_ = now;
    reopenU_ = box.u;
    reopenK_ = box.k;
    breakAt_ = now - kHold - MendTau(now);
    glass_ = Glass::Broken;
  };
  switch (display) {
    case Display::On:
      if (backInFlight) {
        turnRound();
      } else if (glass_ == Glass::Cracking) {
        breakAt_ = now;
        flashAt_ = now - kCrackTime;  // the break flashes at its height
        reopenAt_ = -1;
        glass_ = Glass::Broken;
      } else if (whole) {
        const bool healing = healAt_ >= 0 && now - healAt_ < kHealFade && shatter_;
        shatter_ = MakeShatter(struck || !healing ? impact : shatter_->impact, next());
        crackAt_ = now - kCrackTime;
        flashAt_ = crackAt_;
        breakAt_ = now;
        healAt_ = -1;
        reopenAt_ = -1;
        glass_ = Glass::Broken;
      }
      break;
    case Display::Starting:
      if (backInFlight) {
        turnRound();
      } else if (whole && (struck || from == Display::Off)) {
        // A blow: the user's click (or the tray's, the keyboard's - in the middle).
        shatter_ = MakeShatter(impact, next());
        crackAt_ = now;
        flashAt_ = now;
        healAt_ = -1;
        glass_ = Glass::Cracking;
      }
      break;
    default:
      if (glass_ == Glass::Cracking) {
        healCrack_ = CrackReach(static_cast<float>(now - crackAt_));
        healAt_ = now;
        glass_ = Glass::Whole;
      } else if (glass_ == Glass::Broken) {
        const double flight = std::max(0.0, now - breakAt_ - kHold);
        const Box box = BoxAt(now);
        mendAt_ = now;
        mendU_ = box.u;
        mendK_ = box.k;
        mendTau_ = std::min(kMendCap, static_cast<float>(flight));
        glass_ = Glass::Mending;
      }
      break;
  }

  const float flow = display == Display::Starting && glass_ == Glass::Broken ? kFlowWeak : 1.0f;
  if (flow != flowTo_) {
    flowClock_ = FlowClock(now);
    flowFrom_ = FlowAt(now);
    flowTo_ = flow;
    flowAt_ = now;
  }
}

VitrineFrame Vitrine::At(double now) const {
  if (!known_ || !motion_) {
    return VitrineRest(display_);
  }
  VitrineFrame f;
  f.display = display_;
  f.idle = now;
  f.danger = DangerAt(now);
  f.flow = FlowAt(now);
  f.flowClock = FlowClock(now) + kBusyPace * TrafficSum(now);
  f.traffic = TrafficAt(now);
  f.upShare = UpAt(now);
  std::size_t shown = 0;  // at most kSparks burn at once: they are kSparkGap apart
  for (const Strike& s : strikes_) {
    const double age = now - s.at;
    if (age >= 0 && age < kSparkLife && shown < f.sparks.size()) {
      f.sparks.at(shown++) = {.age = static_cast<float>(age), .at = s.where};
    }
  }
  f.hover = hover_.At(now);
  f.press = press_.At(now);
  f.dim = DimAt(now);
  f.shards = shatter_;
  const double sinceFlash = now - flashAt_;
  f.flash = sinceFlash >= 0 && sinceFlash < 1 ? static_cast<float>(sinceFlash) : -1;
  switch (glass_) {
    case Glass::Whole:
      if (healAt_ >= 0) {
        const float a = 1 - static_cast<float>(now - healAt_) / kHealFade;
        if (a > 0) {
          f.crack = healCrack_;
          f.crackAlpha = a;
        }
      }
      break;
    case Glass::Cracking: {
      const auto t = static_cast<float>(now - crackAt_);
      f.crack = CrackReach(t);
      f.crackAlpha = 0.84f + 0.16f * std::sin(t * 4.5f);
      f.opened.pool = 0.12f * Smooth01(t / 0.4f);
      if (t < 1) {
        const float amp = 2.5f * std::exp(-t * 9);
        f.shake = {std::sin(t * 85) * amp, std::cos(t * 70) * amp * 0.6f};
      }
      break;
    }
    case Glass::Broken: {
      const double flight = now - breakAt_;
      f.glass = 0;
      if (flight < kFlightEnd) {
        f.shatter = kCrackTime + static_cast<float>(flight);
      }
      const Box box = BoxAt(now);
      f.opened = vitrine::AfterBreak(box.u, box.k);
      if (flight >= 0 && flight < 1 && reopenAt_ < 0) {
        const auto u = static_cast<float>(flight);
        const float amp = 5 * std::exp(-u * 7);
        f.shake = {std::sin(u * 85) * amp, std::cos(u * 70) * amp * 0.6f};
      }
      break;
    }
    case Glass::Mending: {
      const Box box = BoxAt(now);
      f.opened = vitrine::AfterBreak(box.u, box.k);
      const float tau = MendTau(now);
      if (tau > 0) {
        f.glass = 0;
        f.shatter = kCrackTime + kHold + tau;
        f.shardsBack = true;
        // Not all at once in mid-air, standing through the delay (19.11.2026): in as it moves.
        f.dustIn = Smooth01((static_cast<float>(now - mendAt_) - kMendDelay) / kMendDustIn);
      } else {
        const auto whole = static_cast<float>(std::max(0.0, now - MendedAt()));
        f.glass = Smooth01(whole / kMendFade);
        if (whole < kMendFade) {
          f.crack = 1000;
          const float seam = 1 - whole / kMendFade;  // the seams, gone as the glass sets
          f.crackAlpha = 0.75f * seam * seam;
          f.shatter = kCrackTime + kHold;
          f.shardsBack = true;
        }
        if (whole < kMendGlow) {
          f.mendFlash = whole;
        }
      }
      break;
    }
  }
  return f;
}

bool Vitrine::Moving(double now) const {
  if (!known_ || !motion_) {
    return false;
  }
  const auto since = [now](double at) { return now - at; };
  if (since(dangerAt_) < kTint || since(flowAt_) < kTint || since(dimAt_) < kTint || since(flashAt_) < 0.5 ||
      since(trafficAt_) < kTrafficEase || since(upAt_) < kTrafficEase || since(lastStrike_) < kSparkLife || hover_.Moving(now) || press_.Moving(now)) {
    return true;
  }
  switch (glass_) {
    case Glass::Whole:
      return healAt_ >= 0 && since(healAt_) < kHealFade;
    case Glass::Cracking:
      return since(crackAt_) < 1.2;
    case Glass::Broken:
      return since(breakAt_) < kFlightEnd;
    case Glass::Mending:
      return since(mendAt_) < kMendClose || since(MendedAt()) < kMendGlow;
  }
  return false;
}

VitrinePlace VitrinePlaceFor(D2D1_RECT_F button) {
  const D2D1_RECT_F hood = vitrine::HoodBounds();
  VitrinePlace place;
  place.scale = (button.right - button.left) / (hood.right - hood.left);
  place.origin = {button.left - hood.left * place.scale, button.top - hood.top * place.scale};
  return place;
}

vitrine::Point ToScene(const VitrinePlace& place, D2D1_POINT_2F at) {
  return {(at.x - place.origin.x) / place.scale, (at.y - place.origin.y) / place.scale};
}

D2D1_RECT_F FromScene(const VitrinePlace& place, D2D1_RECT_F scene) {
  return {place.origin.x + scene.left * place.scale, place.origin.y + scene.top * place.scale,
          place.origin.x + scene.right * place.scale, place.origin.y + scene.bottom * place.scale};
}

// ---------------------------------------------------------------------------
// The drawing.

namespace {

using namespace vitrine;  // NOLINT(google-build-using-namespace): the scene's constants, this file's own

D2D1_POINT_2F P(Point p) { return {p.x, p.y}; }

D2D1_COLOR_F Rgba(int r, int g, int b, float a = 1) {
  return D2D1::ColorF(static_cast<float>(r) / 255, static_cast<float>(g) / 255, static_cast<float>(b) / 255,
                      std::clamp(a, 0.0f, 1.0f));
}

D2D1_COLOR_F Hex(std::uint32_t rgb, float a = 1) {
  return Rgba(static_cast<int>((rgb >> 16) & 0xff), static_cast<int>((rgb >> 8) & 0xff), static_cast<int>(rgb & 0xff),
              a);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
  return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a);
}

float Wave(double x) { return static_cast<float>(std::sin(x)); }

float Phase(double time, double rate) { return static_cast<float>(std::fmod(time * rate, 1.0)); }

// The specks of dust in the spotlight's beam: three depths, each spread evenly
// (a low-discrepancy sequence: no clumps, no holes), all on the same slow air.
constexpr int kMoteDepths = 3;
constexpr int kMotesPerDepth = 22;
constexpr std::array<float, kMoteDepths> kMoteFall{2.6f, 3.8f, 5.2f};  // units a second, far to near
constexpr std::array<float, kMoteDepths> kMoteSize{0.9f, 1.3f, 1.9f};
constexpr std::array<float, kMoteDepths> kMoteGain{0.45f, 0.7f, 0.9f};
constexpr float kMoteTop = 20;
constexpr float kMoteRange = 380;

struct Mote {
  float across = 0, down = 0, phase = 0;  // 0..1 over the beam's width and its height; the glint's
  std::size_t depth = 0;
};

float Fraction(double x) { return static_cast<float>(x - std::floor(x)); }

const std::vector<Mote>& Motes() {
  static const std::vector<Mote> motes = [] {
    std::vector<Mote> all;
    all.reserve(static_cast<std::size_t>(kMoteDepths) * static_cast<std::size_t>(kMotesPerDepth));
    for (int n = 0; n < kMoteDepths * kMotesPerDepth; ++n) {
      const double k = n + 1;
      Mote m;
      m.across = Fraction(0.5 + 0.7548776662 * k);  // R2: the plastic number's
      m.down = Fraction(0.5 + 0.5698402910 * k);
      m.phase = Fraction(0.5 + 0.6180339887 * k) * kTwoPi;
      m.depth = static_cast<std::size_t>(n / kMotesPerDepth);
      all.push_back(m);
    }
    return all;
  }();
  return motes;
}

wil::com_ptr<ID2D1PathGeometry> Path(ID2D1Factory* factory, std::span<const Point> points, bool closed = true) {
  wil::com_ptr<ID2D1PathGeometry> path;
  wil::com_ptr<ID2D1GeometrySink> sink;
  if (points.size() < 2 || FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) {
    return nullptr;
  }
  sink->BeginFigure(P(points[0]), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
  for (std::size_t i = 1; i < points.size(); ++i) {
    sink->AddLine(P(points[i]));
  }
  sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
  if (FAILED(sink->Close())) {
    return nullptr;
  }
  return path;
}

wil::com_ptr<ID2D1PathGeometry> Path(ID2D1Factory* factory, std::initializer_list<Point> points, bool closed = true) {
  return Path(factory, std::span<const Point>(points.begin(), points.size()), closed);
}

wil::com_ptr<ID2D1GradientStopCollection> Stops(ID2D1RenderTarget* t, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
  wil::com_ptr<ID2D1GradientStopCollection> collection;
  if (FAILED(t->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), collection.put()))) {
    return nullptr;
  }
  return collection;
}

wil::com_ptr<ID2D1LinearGradientBrush> Linear(ID2D1RenderTarget* t, std::initializer_list<D2D1_GRADIENT_STOP> stops,
                                              Point from = {0, 0}, Point to = {1, 1}) {
  wil::com_ptr<ID2D1LinearGradientBrush> brush;
  const auto collection = Stops(t, stops);
  if (collection) {
    t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(P(from), P(to)), collection.get(), brush.put());
  }
  return brush;
}

wil::com_ptr<ID2D1RadialGradientBrush> Radial(ID2D1RenderTarget* t, std::initializer_list<D2D1_GRADIENT_STOP> stops,
                                              Point center = {0, 0}, float rx = 1, float ry = 1) {
  wil::com_ptr<ID2D1RadialGradientBrush> brush;
  const auto collection = Stops(t, stops);
  if (collection) {
    t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(P(center), {0, 0}, rx, ry), collection.get(),
                                 brush.put());
  }
  return brush;
}

// A quad: the hinge `a`-`b` and the free edge `o` from it.
std::array<Point, 4> Quad(Point a, Point b, Point o) { return {a, b, Add(b, o), Add(a, o)}; }

// Where a flap's free edge goes from its hinge, `v` its side on the lid, `th` its angle.
Point FlapOff(Point v, float th) {
  return {v.x * std::cos(th) * 0.5f, v.y * std::cos(th) * 0.5f - kFlapRise * std::sin(th)};
}

std::vector<std::uint8_t> Resample(IWICImagingFactory* wic, std::vector<std::uint8_t>& source, UINT side, UINT n) {
  if (n == side) {
    return source;
  }
  wil::com_ptr<IWICBitmap> bitmap;
  wil::com_ptr<IWICBitmapScaler> scaler;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(n) * n * 4);
  if (FAILED(wic->CreateBitmapFromMemory(side, side, GUID_WICPixelFormat32bppPBGRA, side * 4,
                                         static_cast<UINT>(source.size()), source.data(), bitmap.put())) ||
      FAILED(wic->CreateBitmapScaler(scaler.put())) ||
      FAILED(scaler->Initialize(bitmap.get(), n, n, WICBitmapInterpolationModeFant)) ||
      FAILED(scaler->CopyPixels(nullptr, n * 4, static_cast<UINT>(out.size()), out.data()))) {
    return {};
  }
  return out;
}

std::uint8_t Byte(float v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

// The box without its lid: the rhombus on top cleared, the front walls' rim kept.
void CutLid(std::vector<std::uint8_t>& pixels, UINT n) {
  const std::array<Point, 6> cut{Add(kLidT, {0, -3}), Add(kLidR, {3, -2}), kLidR, kLidF, kLidL, Add(kLidL, {-3, -2})};
  const float unit = kScene / static_cast<float>(n);
  const auto first = [&](float v) { return static_cast<std::size_t>(std::clamp(std::floor(v / unit), 0.0f, static_cast<float>(n))); };
  const auto last = [&](float v) { return static_cast<std::size_t>(std::clamp(std::ceil(v / unit), 0.0f, static_cast<float>(n))); };
  for (std::size_t j = first(kLidT.y - 4); j < last(kLidF.y + 1); ++j) {
    for (std::size_t i = first(kLidL.x - 4); i < last(kLidR.x + 4); ++i) {
      int in = 0;
      for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
          const Point at{(static_cast<float>(i) + (static_cast<float>(sx) + 0.5f) / 4) * unit,
                         (static_cast<float>(j) + (static_cast<float>(sy) + 0.5f) / 4) * unit};
          in += InsidePolygon(cut, at) ? 1 : 0;
        }
      }
      if (in == 0) {
        continue;
      }
      const float keep = 1 - static_cast<float>(in) / 16;
      const std::size_t o = (j * n + i) * 4;
      for (std::size_t c = 0; c < 4; ++c) {
        pixels[o + c] = Byte(static_cast<float>(pixels[o + c]) * keep);
      }
    }
  }
}

// The spotlight on the box: strongest on its top, fading down, round its middle - only on the box itself.
// The picture is `n` pixels square, `unit` scene units each, its corner at (`origin`, `origin`).
std::vector<std::uint8_t> LightOnBox(const std::vector<std::uint8_t>& shape, UINT n, float unit, float origin = 0) {
  std::vector<std::uint8_t> out(shape.size());
  const float spotY = kBoxTop + kBoxH * 0.18f;
  for (std::size_t j = 0; j < n; ++j) {
    const float y = origin + (static_cast<float>(j) + 0.5f) * unit;
    const float f = (y - kBoxTop) / (kBoxH * 0.62f);
    float a = 0;
    float g = 232;
    float b = 195;
    if (f <= 0) {
      a = 0.58f;
      g = 240;
      b = 215;
    } else if (f < 0.45f) {
      const float t = f / 0.45f;
      a = 0.58f + (0.2f - 0.58f) * t;
      g = 240 + (232 - 240) * t;
      b = 215 + (195 - 215) * t;
    } else if (f < 1) {
      a = 0.2f * (1 - (f - 0.45f) / 0.55f);
    }
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t o = (j * n + i) * 4;
      const float x = origin + (static_cast<float>(i) + 0.5f) * unit;
      const float d = std::hypot(x - kCx, y - spotY) / 270;
      const float m = d < 0.6f ? 1 - 0.3f * d / 0.6f : (d < 1 ? 0.7f * (1 - (d - 0.6f) / 0.4f) : 0.0f);
      const float alpha = a * m * static_cast<float>(shape[o + 3]) / 255;
      out[o] = Byte(b * alpha);
      out[o + 1] = Byte(g * alpha);
      out[o + 2] = Byte(255 * alpha);
      out[o + 3] = Byte(255 * alpha);
    }
  }
  return out;
}

// The spotlight's beam, 640 x 471 units from above the scene to under the box:
// widening down, soft across with no edge, coming out of nothing at the top
// and going back into it under the box (a cut there showed as a band under the
// open box, its plinth gone).
constexpr float kBeamWidth = 640;
constexpr float kBeamTop = -70;
constexpr float kBeamHeight = 471;

std::vector<std::uint8_t> Beam(UINT w, UINT h) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
  std::uint32_t noise = 0x9E3779B9u;
  for (std::size_t j = 0; j < h; ++j) {
    const float f = (static_cast<float>(j) + 0.5f) / static_cast<float>(h);
    const float half = 125 + 165 * f;
    const float s = std::min(1.0f, f / 0.3125f);
    const float e = std::min(1.0f, (1 - f) / 0.22f);
    const float fade = s * s * (3 - 2 * s) * e * e * (3 - 2 * e) * (0.8f + 0.2f * (1 - f));
    for (std::size_t i = 0; i < w; ++i) {
      const float x = ((static_cast<float>(i) + 0.5f) / static_cast<float>(w) - 0.5f) * kBeamWidth;
      const float dx = std::abs(x) / half;
      float a = 0;
      if (dx < 1) {
        const float p1 = 1 - dx * dx;
        const float p2 = std::max(0.0f, 1 - (dx / 0.55f) * (dx / 0.55f));
        a = (0.17f * p1 * p1 + 0.21f * p2 * p2) * fade;
      }
      noise = noise * 1664525u + 1013904223u;
      const float dither = static_cast<float>(noise >> 8) / 16777216.0f - 0.5f;
      const float v = std::clamp(a * 255 + dither, 0.0f, 255.0f);
      const std::size_t o = (j * w + i) * 4;
      out[o] = Byte(v * 208 / 255);
      out[o + 1] = Byte(v * 238 / 255);
      out[o + 2] = Byte(v);
      out[o + 3] = Byte(v);
    }
  }
  return out;
}

// A shaft of the light scattered in the air over the open box, kShaftWidth x
// kShaftHeight units: narrow at its foot in the box, wider and gone further up.
constexpr float kShaftWidth = 110;
constexpr float kShaftHeight = 260;

// The light's scatter (Impl::Scatter): what glows drawn again on a bitmap of
// half the pixels over this part of the scene (units: the spiral's top to
// under the box's mouth), blurred twice - a tight halo round each line, a
// wide haze in the air - and added on top.
constexpr float kGlowLeft = 20;
constexpr float kGlowTop = -175;
constexpr float kGlowWidth = 490;
constexpr float kGlowHeight = 540;
constexpr float kHaloSigma = 3.5f;  // units
constexpr float kHazeSigma = 16;
constexpr float kHaloGain = 0.5f;
constexpr float kHazeGain = 0.5f;
constexpr float kScatterInner = 0.18f;  // the source in the box, of its full light

std::vector<std::uint8_t> Shaft(UINT w, UINT h) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
  std::uint32_t noise = 0x2545F491u;
  for (std::size_t j = 0; j < h; ++j) {
    const float up = 1 - (static_cast<float>(j) + 0.5f) / static_cast<float>(h);  // 0 at its foot
    const float half = 0.38f + 0.62f * up;
    const float along = Smooth01(up / 0.14f) * std::pow(1 - up, 1.6f);
    for (std::size_t i = 0; i < w; ++i) {
      const float dx = std::abs((static_cast<float>(i) + 0.5f) / static_cast<float>(w) * 2 - 1) / half;
      float a = 0;
      if (dx < 1) {
        const float p = 1 - dx * dx;
        a = 0.2f * p * p * along;
      }
      noise = noise * 1664525u + 1013904223u;
      const float dither = static_cast<float>(noise >> 8) / 16777216.0f - 0.5f;
      const float v = std::clamp(a * 255 + dither, 0.0f, 255.0f);
      const std::size_t o = (j * w + i) * 4;
      out[o] = Byte(v * 160 / 255);
      out[o + 1] = Byte(v * 216 / 255);
      out[o + 2] = Byte(v);
      out[o + 3] = Byte(v);
    }
  }
  return out;
}

// The light in the box breathing, slowly and hardly: two waves that don't repeat together.
float Breath(double idle) { return 0.93f + 0.04f * Wave(idle * 1.21) + 0.03f * Wave(idle * 0.67 + 1.3); }

wil::com_ptr<ID2D1Bitmap> Bitmap(ID2D1RenderTarget* t, const std::vector<std::uint8_t>& pixels, UINT w, UINT h) {
  wil::com_ptr<ID2D1Bitmap> bitmap;
  if (pixels.size() < static_cast<std::size_t>(w) * h * 4) {
    return bitmap;
  }
  t->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4,
                  D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                         96, 96),
                  bitmap.put());
  return bitmap;
}

// Adds light instead of covering (the canvas' "lighter"); where the target can't, it covers.
class Lighter {
 public:
  explicit Lighter(ID2D1DeviceContext* dc) : dc_(dc) {
    if (dc_ != nullptr) {
      dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
    }
  }
  ~Lighter() {
    if (dc_ != nullptr) {
      dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
  }
  Lighter(const Lighter&) = delete;
  Lighter& operator=(const Lighter&) = delete;

 private:
  ID2D1DeviceContext* dc_;
};

constexpr std::size_t kShardLooks = 11;  // the shards' glint, in steps
constexpr std::size_t kShardHeats = 5;   // their heat at the blow

std::size_t Step(float v, std::size_t steps) {
  const auto last = static_cast<long>(steps - 1);
  return static_cast<std::size_t>(std::clamp(std::lround(v * static_cast<float>(last)), 0L, last));
}

}  // namespace

struct VitrineArt::Impl {
  wil::com_ptr<IWICImagingFactory> wic;
  std::vector<std::uint8_t> source;
  UINT side = 0;
  std::vector<std::uint8_t> openSource;  // the opened box, the scene and a sixteenth round it
  UINT openSide = 0;

  wil::com_ptr<ID2D1RenderTarget> owner;
  wil::com_ptr<ID2D1DeviceContext> dc;
  wil::com_ptr<ID2D1Factory> factory;
  wil::com_ptr<ID2D1StrokeStyle> round;
  wil::com_ptr<ID2D1Layer> layer;
  wil::com_ptr<ID2D1SolidColorBrush> solid;

  wil::com_ptr<ID2D1PathGeometry> hood, hoodTop, hoodLeft, hoodRight;
  wil::com_ptr<ID2D1PathGeometry> plinthLeft, plinthRight, plinthTop, plinthEdge, plaque;
  wil::com_ptr<ID2D1PathGeometry> rim, rimLight, mouth, insideLeft, insideRight;

  wil::com_ptr<ID2D1LinearGradientBrush> plinthShade, plaqueBrass, rimBrass, gloss, insideLeftShade, insideRightShade,
      warm, flapShadow;
  std::array<wil::com_ptr<ID2D1LinearGradientBrush>, 4> flaps;
  wil::com_ptr<ID2D1RadialGradientBrush> poolWarm, poolRed, shadow, bloom, insideGlow, insideCore, mouthBloom,
      flash, ember;
  std::array<wil::com_ptr<ID2D1LinearGradientBrush>, kShardLooks * kShardHeats> shardFill;

  UINT pixels = 0;
  wil::com_ptr<ID2D1Bitmap> box, body, lightClosed, lightOpen, beam, shaft;
  wil::com_ptr<ID2D1BitmapBrush> lid;
  UINT pad = 0;  // the opened picture's pixels past the scene on each side
  wil::com_ptr<ID2D1Bitmap> openBox, lightOpenBox;
  wil::com_ptr<ID2D1Layer> flapsLayer;  // the drawn flaps giving way to the picture
  // The scatter (Scatter): its bitmap, drawn on in turn, and the blurs with their warm tints.
  wil::com_ptr<ID2D1BitmapRenderTarget> glowTarget;
  wil::com_ptr<ID2D1DeviceContext> glowDc;
  wil::com_ptr<ID2D1Bitmap> glowBitmap;
  wil::com_ptr<ID2D1Effect> halo, haze, haloTint, hazeTint;
  float glowK = 0;  // its pixels a unit

  std::shared_ptr<const Shatter> shapesFor;
  std::vector<wil::com_ptr<ID2D1PathGeometry>> shapes;

  ID2D1RenderTarget* t = nullptr;  // while drawing
  float zk = 1;                    // units a line is at least: a pixel on screen
  float busy = 0;                  // the frame's traffic (VitrineFrame::traffic): the spiral's packets
  float up = 0.5f;                 // and how many of them rise (VitrineFrame::upShare)
  std::array<Spark, kSparks> sparks{};  // the frame's blocked ads

  ID2D1SolidColorBrush* Ink(D2D1_COLOR_F c) const {
    solid->SetColor(c);
    return solid.get();
  }

  bool MakeStatic(ID2D1RenderTarget* target) {
    Impl fresh;  // all the old target's things dropped; the pictures kept
    fresh.wic = std::move(wic);
    fresh.source = std::move(source);
    fresh.side = side;
    fresh.openSource = std::move(openSource);
    fresh.openSide = openSide;
    *this = std::move(fresh);
    owner = target;
    dc = owner.try_query<ID2D1DeviceContext>();
    target->GetFactory(factory.put());
    if (!factory || FAILED(target->CreateSolidColorBrush(Rgba(255, 255, 255), solid.put()))) {
      return false;
    }
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                               nullptr, 0, round.put());
    target->CreateLayer(layer.put());
    target->CreateLayer(flapsLayer.put());
    if (dc) {
      const auto blur = [this](wil::com_ptr<ID2D1Effect>& blurred, wil::com_ptr<ID2D1Effect>& tinted) {
        if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1GaussianBlur, blurred.put())) &&
            SUCCEEDED(dc->CreateEffect(CLSID_D2D1ColorMatrix, tinted.put()))) {
          tinted->SetInputEffect(0, blurred.get());
        } else {
          blurred.reset();
          tinted.reset();
        }
      };
      blur(halo, haloTint);
      blur(haze, hazeTint);
    }
    ID2D1Factory* f = factory.get();
    const std::array<Point, 6> outline = Hood();
    hood = Path(f, outline);
    hoodTop = Path(f, {kGT, kGUR, kGC, kGUL});
    hoodLeft = Path(f, {kGUL, kGC, kGB, kGLL});
    hoodRight = Path(f, {kGC, kGUR, kGLR, kGB});
    plinthLeft = Path(f, {kPL, kPF, Down(kPF, kThick), Down(kPL, kThick)});
    plinthRight = Path(f, {kPF, kPR, Down(kPR, kThick), Down(kPF, kThick)});
    plinthTop = Path(f, {kPK, kPR, kPF, kPL});
    plinthEdge = Path(f, {kPL, kPF, kPR});
    const auto q = [](float u, float v) { return Down(Lerp(kPL, kPF, u), kThick * v); };
    plaque = Path(f, {q(0.17f, 0.2f), q(0.47f, 0.2f), q(0.47f, 0.8f), q(0.17f, 0.8f)});
    rim = Path(f, {kGLL, kGB, kGLR}, false);
    rimLight = Path(f, {Down(kGLL, -1.3f), Down(kGB, -1.3f), Down(kGLR, -1.3f)}, false);
    constexpr float kLip = 2.5f;
    mouth = Path(f, {kLidT, kLidR, Down(kLidR, kLip), Down(kLidF, kLip), Down(kLidL, kLip), kLidL});
    insideLeft = Path(f, {kLidT, kLidL, kLidF});
    insideRight = Path(f, {kLidT, kLidF, kLidR});

    plinthShade = Linear(target, {{0, Hex(0x2a313c)}, {1, Hex(0x1c222b)}}, {0, kPK.y}, {0, kPF.y});
    plaqueBrass = Linear(target, {{0, Hex(0x9a7838)}, {0.5f, Hex(0xe3c47f)}, {1, Hex(0xa07f3f)}},
                         {q(0.17f, 0).x, 0}, {q(0.47f, 0).x, 0});
    rimBrass = Linear(target, {{0, Hex(0x7d5e2c)}, {0.5f, Hex(0xecc983)}, {1, Hex(0x8d6c34)}}, {kGLL.x, 0},
                      {kGLR.x, 0});
    gloss = Linear(target, {{0, Rgba(255, 255, 255, 0)}, {0.5f, Rgba(255, 255, 255, 0.17f)}, {1, Rgba(255, 255, 255, 0)}});
    insideLeftShade = Linear(target, {{0, Hex(0x323c4b)}, {1, Hex(0x0e1319)}}, {0, kLidT.y}, {0, kLidF.y});
    insideRightShade = Linear(target, {{0, Hex(0x28303c)}, {1, Hex(0x0a0e13)}}, {0, kLidT.y}, {0, kLidF.y});
    warm = Linear(target, {{0, Rgba(255, 196, 120, 0.55f)}, {0.7f, Rgba(255, 196, 120, 0.08f)}, {1, Rgba(255, 196, 120, 0)}});
    flapShadow = Linear(target, {{0, Rgba(0, 0, 0, 0.34f)}, {1, Rgba(0, 0, 0, 0)}});
    flaps[0] = Linear(target, {{0, Hex(0x242c38)}, {1, Hex(0x5a687e)}});  // the small back one, its underside
    flaps[1] = Linear(target, {{0, Hex(0x58677d)}, {1, Hex(0x46546a)}});  // the small back one, its top
    flaps[2] = Linear(target, {{0, Hex(0x232b37)}, {1, Hex(0x5d6b81)}});  // the big back one, its underside
    flaps[3] = Linear(target, {{0, Hex(0x3a4659)}, {1, Hex(0x54627a)}});  // the left front one, outside

    poolWarm = Radial(target, {{0, Rgba(255, 240, 218, 0.8f)}, {0.5f, Rgba(210, 170, 110, 0.34f)}, {1, Rgba(210, 170, 110, 0)}},
                      {kCx, kOy}, 250, 140);
    poolRed = Radial(target, {{0, Rgba(255, 150, 140, 0.8f)}, {0.5f, Rgba(220, 70, 70, 0.34f)}, {1, Rgba(220, 70, 70, 0)}},
                     {kCx, kOy}, 250, 140);
    shadow = Radial(target, {{0, Rgba(0, 0, 0, 0.55f)}, {0.06f, Rgba(0, 0, 0, 0.55f)}, {1, Rgba(0, 0, 0, 0)}},
                    {kCx, kOy + 4}, 170, 85);
    bloom = Radial(target, {{0, Rgba(255, 238, 205, 0.34f)}, {0.4f, Rgba(255, 222, 170, 0.15f)}, {1, Rgba(255, 210, 150, 0)}},
                   {kCx, kBoxTop + kBoxH * 0.5f}, 330, 280.5f);
    insideGlow = Radial(target, {{0, Rgba(255, 214, 150, 0.95f)}, {0.4f, Rgba(255, 178, 92, 0.42f)}, {1, Rgba(255, 160, 70, 0)}},
                        {263, 196}, 172.5f, 120);
    // Its source deep in the box, under the front lip: the walls lit most low down.
    insideCore = Radial(target, {{0, Rgba(255, 244, 222, 0.75f)}, {0.4f, Rgba(255, 212, 150, 0.3f)}, {1, Rgba(255, 180, 100, 0)}},
                        {263, 214}, 96, 54);
    mouthBloom = Radial(target, {{0, Rgba(255, 226, 170, 0.30f)}, {0.45f, Rgba(255, 200, 130, 0.12f)}, {1, Rgba(255, 190, 110, 0)}},
                        {263, 140}, 230, 287.5f);
    flash = Radial(target, {{0, Rgba(255, 250, 240, 1)}, {0.35f, Rgba(255, 238, 210, 0.38f)}, {1, Rgba(255, 230, 190, 0)}});
    // A blocked ad's burn: a deep warm red, its heart lighter - an ember, not a neon.
    ember = Radial(target, {{0, Rgba(255, 168, 132, 0.85f)}, {0.3f, Rgba(236, 84, 60, 0.36f)}, {1, Rgba(214, 56, 44, 0)}});
    return true;
  }

  // The box's bitmaps at `n` pixels across the scene: drawn pixel for pixel.
  bool MakeBitmaps(UINT n) {
    std::vector<std::uint8_t> picture = Resample(wic.get(), source, side, n);
    if (picture.empty()) {
      return false;
    }
    std::vector<std::uint8_t> opened = picture;
    CutLid(opened, n);
    const float unit = kScene / static_cast<float>(n);
    box = Bitmap(owner.get(), picture, n, n);
    body = Bitmap(owner.get(), opened, n, n);
    lightClosed = Bitmap(owner.get(), LightOnBox(picture, n, unit), n, n);
    lightOpen = Bitmap(owner.get(), LightOnBox(opened, n, unit), n, n);
    // The opened one on the same pixels: whole ones past the scene, so its walls stay on the closed one's.
    openBox.reset();
    lightOpenBox.reset();
    pad = (n + 8) / 16;
    const UINT wide = n + 2 * pad;
    if (!openSource.empty()) {
      const std::vector<std::uint8_t> open = Resample(wic.get(), openSource, openSide, wide);
      if (!open.empty()) {
        openBox = Bitmap(owner.get(), open, wide, wide);
        lightOpenBox = Bitmap(owner.get(), LightOnBox(open, wide, unit, -static_cast<float>(pad) * unit), wide, wide);
      }
    }
    const float perUnit = static_cast<float>(n) / kScene / 2;  // the beam is soft: half the pixels do
    const auto bw = static_cast<UINT>(std::max(2L, std::lround(kBeamWidth * perUnit)));
    const auto bh = static_cast<UINT>(std::max(2L, std::lround(kBeamHeight * perUnit)));
    beam = Bitmap(owner.get(), Beam(bw, bh), bw, bh);
    const auto sw = static_cast<UINT>(std::max(2L, std::lround(kShaftWidth * perUnit)));
    const auto sh = static_cast<UINT>(std::max(2L, std::lround(kShaftHeight * perUnit)));
    shaft = Bitmap(owner.get(), Shaft(sw, sh), sw, sh);
    lid.reset();
    if (box) {
      owner->CreateBitmapBrush(box.get(),
                               D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                               lid.put());
    }
    MakeGlow(n);
    pixels = n;
    return box && body && lid;
  }

  // The scatter's bitmap for `n` pixels across the scene; none where the target can't blur.
  void MakeGlow(UINT n) {
    glowTarget.reset();
    glowDc.reset();
    glowBitmap.reset();
    if (!halo || !haze) {
      return;
    }
    glowK = static_cast<float>(n) / kScene / 2;
    const auto w = static_cast<UINT>(std::max(2L, std::lround(kGlowWidth * glowK)));
    const auto h = static_cast<UINT>(std::max(2L, std::lround(kGlowHeight * glowK)));
    if (FAILED(owner->CreateCompatibleRenderTarget(
            D2D1::SizeF(static_cast<float>(w), static_cast<float>(h)), D2D1::SizeU(w, h),
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, glowTarget.put())) ||
        FAILED(glowTarget->GetBitmap(glowBitmap.put()))) {
      glowTarget.reset();
      glowBitmap.reset();
      return;
    }
    glowDc = glowTarget.try_query<ID2D1DeviceContext>();
    halo->SetInput(0, glowBitmap.get());
    haze->SetInput(0, glowBitmap.get());
    halo->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kHaloSigma * glowK);
    haze->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kHazeSigma * glowK);
  }

  bool Ensure(ID2D1RenderTarget* target, float span) {
    if (source.empty()) {
      return false;
    }
    if (owner.get() != target && !MakeStatic(target)) {
      return false;
    }
    const auto n = static_cast<UINT>(std::clamp(std::lround(span), 64L, 1600L));
    if (n != pixels && !MakeBitmaps(n)) {
      pixels = 0;
      return false;
    }
    return true;
  }

  ID2D1LinearGradientBrush* ShardFill(float glint, float heat) {
    const std::size_t g = Step(glint, kShardLooks);
    const std::size_t h = Step(heat, kShardHeats);
    auto& brush = shardFill[g * kShardHeats + h];
    if (!brush) {
      const float e = static_cast<float>(g) / static_cast<float>(kShardLooks - 1);
      const float hot = static_cast<float>(h) / static_cast<float>(kShardHeats - 1);
      brush = Linear(owner.get(), {{0, Rgba(235, 246, 255, 0.08f + 0.34f * e + 0.4f * hot)},
                                   {0.5f, Rgba(140, 185, 240, 0.07f + 0.10f * (1 - e) + 0.3f * hot)},
                                   {1, Rgba(245, 250, 255, 0.06f + 0.26f * e + 0.4f * hot)}});
    }
    return brush.get();
  }

  void Fill(ID2D1Geometry* g, ID2D1Brush* b) const {
    if (g != nullptr && b != nullptr) {
      t->FillGeometry(g, b);
    }
  }

  void Stroke(ID2D1Geometry* g, ID2D1Brush* b, float w, ID2D1StrokeStyle* style = nullptr) const {
    if (g != nullptr && b != nullptr) {
      t->DrawGeometry(g, b, w, style);
    }
  }

  void Ellipse(Point c, float rx, float ry, ID2D1Brush* b) const {
    if (b != nullptr) {
      t->FillEllipse(D2D1::Ellipse(P(c), rx, ry), b);
    }
  }

  void Line(Point a, Point b, D2D1_COLOR_F c, float w, ID2D1StrokeStyle* style = nullptr) const {
    t->DrawLine(P(a), P(b), Ink(c), w, style);
  }

  // --- the parts, back to front ---

  void Plinth(float pa) const {
    Fill(plinthLeft.get(), Ink(Hex(0x12161c, pa)));
    Fill(plinthRight.get(), Ink(Hex(0x1a2029, pa)));
    if (plinthShade) {
      plinthShade->SetOpacity(pa);
    }
    Fill(plinthTop.get(), plinthShade.get());
    Stroke(plinthEdge.get(), Ink(Rgba(255, 255, 255, 0.22f * pa)), 1.2f);
    Line(Down(kPF, kThick), kPF, Rgba(255, 255, 255, 0.07f * pa), 1.2f);
    if (plaqueBrass) {
      plaqueBrass->SetOpacity(pa);
    }
    Fill(plaque.get(), plaqueBrass.get());
    Stroke(plaque.get(), Ink(Rgba(20, 12, 0, 0.55f * pa)), 0.7f);
    const auto q = [](float u, float v) { return Down(Lerp(kPL, kPF, u), kThick * v); };
    for (const float v : {0.42f, 0.58f}) {
      Line(q(0.21f, v), q(0.43f, v), Rgba(70, 45, 10, 0.65f * pa), 0.8f);
    }
  }

  void Pool(float amount, float danger, double idle, float pa) const {
    if (amount <= 0 || pa <= 0) {
      return;
    }
    const float b = std::min(1.4f, amount) * (0.94f + 0.06f * Wave(idle * 2.2)) * pa;
    if (poolWarm && danger < 1) {
      poolWarm->SetOpacity(std::min(1.0f, b * (1 - danger)));
      Fill(plinthTop.get(), poolWarm.get());
    }
    if (poolRed && danger > 0) {
      poolRed->SetOpacity(std::min(1.0f, b * danger));
      Fill(plinthTop.get(), poolRed.get());
    }
  }

  void Shadow(float a) const {
    if (a <= 0 || !shadow) {
      return;
    }
    shadow->SetOpacity(std::min(1.0f, a));
    Fill(plinthTop.get(), shadow.get());
  }

  void Back(float glass, float pa) const {
    if (pa <= 0) {
      return;
    }
    Line(kGC, kGLL, Rgba(150, 115, 60, 0.5f * pa), 2.2f);
    Line(kGC, kGLR, Rgba(150, 115, 60, 0.5f * pa), 2.2f);
    if (glass > 0) {
      Line(kGC, kGT, Rgba(200, 220, 255, 0.22f * glass * pa), 1);
    }
  }

  void Bloom(float a) const {
    if (a <= 0 || !bloom) {
      return;
    }
    const Lighter add(dc.get());
    bloom->SetOpacity(std::min(1.0f, a));
    Ellipse({kCx, kBoxTop + kBoxH * 0.5f}, 330, 280.5f, bloom.get());
  }

  void Cone(float a) const {
    if (a <= 0 || !beam) {
      return;
    }
    const Lighter add(dc.get());
    const D2D1_RECT_F at{kCx - kBeamWidth / 2, kBeamTop, kCx + kBeamWidth / 2, kBeamTop + kBeamHeight};
    t->DrawBitmap(beam.get(), at, std::min(1.0f, a), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (a > 1) {
      t->DrawBitmap(beam.get(), at, std::min(1.0f, a - 1), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
  }

  // The far depths behind the box, the near one in front. They settle slowly
  // on one air, fade by the beam's own profile (none pops at its edge), and
  // over the open box take its warmth (`warmth`, the flow's).
  void DrawMotes(float a, float warmth, double time, bool front) const {
    if (a <= 0.003f) {
      return;
    }
    const Lighter add(dc.get());
    const D2D1_COLOR_F dust = Rgba(255, 240, 210);
    const D2D1_COLOR_F lit = Rgba(255, 206, 140);
    for (const Mote& m : Motes()) {
      const std::size_t d = m.depth;
      if ((d == kMoteDepths - 1) != front) {
        continue;
      }
      const float y = kMoteTop + Fraction(m.down + time * kMoteFall[d] / kMoteRange) * kMoteRange;
      const float half = (125 + 165 * (y - kBeamTop) / kBeamHeight) * 0.9f;
      const float x0 = kCx + (m.across * 2 - 1) * half;
      const auto dd = static_cast<float>(d);
      const float drift =
          (7 * Wave(time * 0.21 + y * 0.011 + dd * 0.7) + 3 * Wave(time * 0.13 + x0 * 0.017)) * (0.6f + 0.2f * dd);
      const float x = x0 + drift;
      const float across = std::abs(x - kCx) / half;
      if (across >= 1) {
        continue;
      }
      const float profile = (1 - across * across) * (1 - across * across);
      const float ends = Smooth((y - kMoteTop) / 60) * Smooth((kOy + 10 - y) / 50);
      const float glint = 0.8f + 0.2f * Wave(time * 0.35 + m.phase);
      const float alpha = std::min(1.0f, a * kMoteGain[d] * profile * ends * glint);
      if (alpha < 0.01f) {
        continue;
      }
      const float aside = (x - 263) / 120;  // from the column over the box
      const float heat = std::clamp(warmth * std::exp(-aside * aside) * Smooth((210 - y) / 80), 0.0f, 1.0f);
      D2D1_COLOR_F ink = Mix(dust, lit, heat);
      const float r = kMoteSize[d] / 2;
      ink.a = alpha * 0.18f;
      t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r * 2, r * 2), Ink(ink));
      ink.a = alpha;
      t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), Ink(ink));
    }
  }

  // `pic`: the opened box's share that is its picture (Share).
  void TopLight(float a, bool opened, float pic) const {
    if (a <= 0) {
      return;
    }
    const Lighter add(dc.get());
    const float k = std::min(1.0f, a);
    ID2D1Bitmap* light = opened ? lightOpen.get() : lightClosed.get();
    if (light != nullptr && pic < 1) {
      t->DrawBitmap(light, D2D1::RectF(0, 0, kScene, kScene), k * (1 - pic), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    if (lightOpenBox && pic > 0) {
      t->DrawBitmap(lightOpenBox.get(), OpenRect(), k * pic, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
  }

  void RimFront(float glow, float pa) const {
    if (pa <= 0) {
      return;
    }
    if (rimBrass) {
      rimBrass->SetOpacity(pa);
    }
    Stroke(rim.get(), rimBrass.get(), 3.6f, round.get());
    Stroke(rimLight.get(), Ink(Rgba(255, 243, 205, 0.65f * pa)), 0.9f, round.get());
    for (const Point p : {kGLL, kGB, kGLR}) {
      t->FillEllipse(D2D1::Ellipse(P(p), 3.2f, 3.2f), Ink(Hex(0xd9b873, pa)));
      t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x - 0.8f, p.y - 0.9f), 1, 1), Ink(Rgba(255, 250, 230, 0.8f * pa)));
    }
    if (glow > 0) {
      const Lighter add(dc.get());
      const float g = std::min(0.8f, glow);
      Stroke(rim.get(), Ink(Rgba(255, 196, 100, 0.07f * g)), 14, round.get());
      Stroke(rim.get(), Ink(Rgba(255, 196, 100, 0.14f * g)), 7, round.get());
      Stroke(rim.get(), Ink(Rgba(255, 226, 160, g)), 2, round.get());
    }
  }

  // `hover`, `press`: 0..1, eased.
  void Glass(float a, float danger, double idle, float hover, float press) const {
    if (a <= 0) {
      return;
    }
    const D2D1_COLOR_F red = Rgba(240, 96, 96);
    const float tint = danger * 0.75f;
    Fill(hoodTop.get(), Ink(Mix(Rgba(215, 232, 255, 0.10f * a), red, tint)));
    Fill(hoodLeft.get(), Ink(Mix(Rgba(170, 200, 240, 0.045f * a), red, tint)));
    Fill(hoodRight.get(), Ink(Mix(Rgba(170, 200, 240, 0.075f * a), red, tint)));
    if (gloss) {
      const float ph = Phase(idle, 1 / 4.2);
      const float x = kCx + (ph * 2 - 1) * 330;
      const Point dir{std::cos(-0.35f) * 40, std::sin(-0.35f) * 40};
      gloss->SetStartPoint(D2D1::Point2F(x - dir.x, kOy - dir.y));
      gloss->SetEndPoint(D2D1::Point2F(x + dir.x, kOy + dir.y));
      gloss->SetOpacity(std::min(1.0f, a * (1 + 0.6f * hover) * (1 - 0.4f * press)));
      Fill(hood.get(), gloss.get());
    }
    const float edge = std::clamp(0.8f + 0.2f * hover - 0.25f * press, 0.0f, 1.0f) * a;
    const D2D1_COLOR_F ink = Mix(Rgba(235, 245, 255, edge), red, tint);
    Stroke(hood.get(), Ink(ink), 1.6f * zk, round.get());
    Line(kGC, kGUL, ink, 1.6f * zk, round.get());
    Line(kGC, kGUR, ink, 1.6f * zk, round.get());
    Line(kGC, kGB, ink, 1.6f * zk, round.get());
  }

  void Cracks(const Shatter& s, float reach, float a) const {
    if (a <= 0 || reach <= 0) {
      return;
    }
    // Bright and bold round the blow, thinner and fainter the further they ran.
    for (const Crack& c : s.cracks) {
      if (c.distance >= reach) {
        break;
      }
      const float fall = std::exp(-c.distance / 120);
      Line(c.a, c.b, Rgba(255, 255, 255, 0.95f * a * (0.22f + 0.78f * fall)), (0.65f + 0.75f * fall) * zk);
    }
  }

  void EnsureShapes(const std::shared_ptr<const Shatter>& s) {
    if (shapesFor == s) {
      return;
    }
    shapesFor = s;
    shapes.clear();
    shapes.reserve(s->shards.size());
    for (const Shard& shard : s->shards) {
      shapes.push_back(Path(factory.get(), shard.local));
    }
  }

  void Shards(const std::shared_ptr<const Shatter>& s, float time, float opacity, const D2D1::Matrix3x2F& scene) {
    if (time < kCrackTime || opacity <= 0) {
      return;
    }
    EnsureShapes(s);
    const float tau = std::max(0.0f, time - kCrackTime - kHold);
    const float hot = std::max(0.0f, 1 - (time - kCrackTime) / (kHold + 0.07f));
    struct Placed {
      std::size_t index;
      D2D1::Matrix3x2F at;
      float alpha;
    };
    std::vector<Placed> placed;
    placed.reserve(s->shards.size());
    for (std::size_t i = 0; i < s->shards.size(); ++i) {
      const Shard& shard = s->shards[i];
      const float fade = tau < shard.vanish ? 1 : std::max(0.0f, 1 - (tau - shard.vanish) / 0.28f);
      const float alpha = fade * opacity;
      if (alpha <= 0.004f || !shapes[i]) {
        continue;
      }
      const float zs = 1 + shard.grow * tau;
      const float af = std::abs(std::cos(shard.flip * tau));
      const Point c{shard.center.x + shard.velocity.x * tau,
                    shard.center.y + shard.velocity.y * tau + 0.5f * kGravity * tau * tau};
      const D2D1::Matrix3x2F at = D2D1::Matrix3x2F::Scale(zs * (0.35f + 0.65f * af), zs) *
                                  D2D1::Matrix3x2F::Rotation(shard.spin * tau / kDegrees) *
                                  D2D1::Matrix3x2F::Translation(c.x, c.y) * scene;
      t->SetTransform(at);
      ID2D1LinearGradientBrush* fill = ShardFill(1 - af, hot);
      if (fill != nullptr) {
        fill->SetStartPoint(D2D1::Point2F(shard.left, shard.top));
        fill->SetEndPoint(D2D1::Point2F(shard.right, shard.bottom));
        fill->SetOpacity(alpha);
        t->FillGeometry(shapes[i].get(), fill);
      }
      // Its edge catches the light as it turns: bright edge-on, a thin line face on.
      t->DrawGeometry(shapes[i].get(), Ink(Rgba(250, 253, 255, (0.5f + 0.45f * (1 - af)) * alpha)),
                      (0.9f + 0.6f * (1 - af)) * zk);
      if (shard.spark && af < 0.2f && fade > 0.3f) {
        t->SetTransform(scene);
        const float r = 12 + 8 * (1 - af / 0.2f);
        Line({c.x - r, c.y}, {c.x + r, c.y}, Rgba(255, 255, 255, alpha), 1.1f * zk);
        Line({c.x, c.y - r}, {c.x, c.y + r}, Rgba(255, 255, 255, alpha), 1.1f * zk);
      }
      placed.push_back({i, at, alpha});
    }
    // The colours parting on the edges, like light through a prism.
    const Lighter add(dc.get());
    for (const Placed& p : placed) {
      t->SetTransform(D2D1::Matrix3x2F::Translation(zk, 0) * p.at);
      t->DrawGeometry(shapes[p.index].get(), Ink(Rgba(70, 190, 255, 0.40f * p.alpha)), 1.3f * zk);
      t->SetTransform(D2D1::Matrix3x2F::Translation(-zk, 0) * p.at);
      t->DrawGeometry(shapes[p.index].get(), Ink(Rgba(255, 90, 120, 0.26f * p.alpha)), 1.3f * zk);
    }
    t->SetTransform(scene);
  }

  static Point Fly(const Speck& p, float u) {
    const float e = std::min(1.0f, u * 2);
    return {p.x + p.vx * u + 0.5f * kDriftX * u * u + std::sin(u * p.wobbleRate + p.wobblePhase) * p.wobble * e,
            p.y + p.vy * u + 0.5f * kDriftY * u * u + std::cos(u * p.wobbleRate * 0.8f + p.wobblePhase) * p.wobble * e};
  }

  void Dust(const Shatter& s, float time, float opacity) const {
    const float tau0 = time - kCrackTime - kHold;
    if (tau0 < 0 || opacity <= 0) {
      return;
    }
    const float grain = std::pow(zk, 0.8f);
    for (const Speck& p : s.dust) {
      const float u = tau0 - p.start;
      if (u <= 0 || u > p.life) {
        continue;
      }
      const float a = std::pow(1 - u / p.life, 1.35f) * (0.55f + 0.45f * std::sin(u * 18 + p.wobblePhase));
      if (a <= 0.03f) {
        continue;
      }
      const Point q = Fly(p, u);
      const float z = p.size * grain / 2;
      t->FillRectangle(D2D1::RectF(q.x - z, q.y - z, q.x + z, q.y + z),
                       Ink(Rgba(218, 234, 255, std::min(1.0f, a * 1.15f) * opacity)));
    }
    const Lighter add(dc.get());
    for (const Speck& p : s.glints) {
      const float u = tau0 - p.start;
      if (u <= 0 || u > p.life) {
        continue;
      }
      const float a = std::pow(1 - u / p.life, 1.1f) * (0.5f + 0.5f * std::sin(u * 22 + p.wobblePhase));
      if (a <= 0.05f) {
        continue;
      }
      const Point q = Fly(p, u);
      const float z = p.size * 2.1f * grain;
      t->FillRectangle(D2D1::RectF(q.x - z / 2, q.y - z / 2, q.x + z / 2, q.y + z / 2),
                       Ink(Rgba(255, 255, 255, std::min(1.0f, a * 1.3f) * opacity)));
      if (a > 0.6f) {
        const float w = 0.8f * zk;
        const D2D1_COLOR_F ink = Rgba(255, 255, 255, a * 0.5f * opacity);
        t->FillRectangle(D2D1::RectF(q.x - z * 3, q.y - w / 2, q.x + z * 3, q.y + w / 2), Ink(ink));
        t->FillRectangle(D2D1::RectF(q.x - w / 2, q.y - z * 3, q.x + w / 2, q.y + z * 3), Ink(ink));
      }
    }
  }

  void Flash(Point at, float r, float a) const {
    if (a <= 0 || !flash) {
      return;
    }
    flash->SetCenter(P(at));
    flash->SetRadiusX(r);
    flash->SetRadiusY(r);
    flash->SetOpacity(std::min(1.0f, a));
    Ellipse(at, r, r, flash.get());
  }

  void Impact(const VitrineFrame& f, Point at) const {
    const Lighter add(dc.get());
    if (f.flash >= 0 && f.flash < 0.34f) {
      const float a = (1 - f.flash / 0.34f) * (f.flash < kCrackTime ? 0.45f + 0.55f * f.flash / kCrackTime : 1.0f);
      Flash(at, 90 + 700 * f.flash, a);
    }
    if (f.shatter >= kCrackTime && !f.shardsBack) {
      const float u = f.shatter - kCrackTime;
      if (u <= 0.7f) {
        const float k = 1 - u / 0.7f;
        const float r = 20 + 1000 * std::pow(u, 0.8f);
        t->DrawEllipse(D2D1::Ellipse(P(at), r, r), Ink(Rgba(235, 244, 255, 0.75f * k * k)), (1 + 4 * k) * zk);
        t->DrawEllipse(D2D1::Ellipse(P(at), r * 0.82f, r * 0.82f), Ink(Rgba(255, 255, 255, 0.3f * k * k)), 1);
      }
    }
    if (f.mendFlash >= 0 && f.mendFlash < 0.5f) {
      const float k = 1 - f.mendFlash / 0.5f;
      Flash(at, 120 + 260 * f.mendFlash, 0.5f * k * k);
    }
  }

  // --- the open box ---

  wil::com_ptr<ID2D1PathGeometry> Flap(Point a, Point b, Point o) const {
    const std::array<Point, 4> q = Quad(a, b, o);
    return Path(factory.get(), q);
  }

  void PlainFlap(ID2D1Geometry* g, Point hinge, Point free, ID2D1LinearGradientBrush* b, float gl, float heat) const {
    if (b != nullptr) {
      b->SetStartPoint(P(hinge));
      b->SetEndPoint(P(free));
      Fill(g, b);
    }
    if (heat > 0 && warm) {
      const Lighter add(dc.get());
      warm->SetStartPoint(P(hinge));
      warm->SetEndPoint(P(free));
      warm->SetOpacity(std::min(1.0f, heat * gl));
      Fill(g, warm.get());
    }
  }

  // The lid's picture laid on a flap: the lid's sides (from its back corner)
  // go to `u` and `w`, its back corner to `at`.
  void TexFlap(ID2D1Geometry* g, Point u, Point w, Point at) const {
    if (!lid) {
      return;
    }
    const float det = kLidU.x * kLidW.y - kLidU.y * kLidW.x;
    const float m00 = kLidW.y / det;
    const float m01 = -kLidW.x / det;
    const float m10 = -kLidU.y / det;
    const float m11 = kLidU.x / det;
    const float a = u.x * m00 + w.x * m10;
    const float c = u.x * m01 + w.x * m11;
    const float b = u.y * m00 + w.y * m10;
    const float d = u.y * m01 + w.y * m11;
    const float e = at.x - a * kLidT.x - c * kLidT.y;
    const float f = at.y - b * kLidT.x - d * kLidT.y;
    const float unit = kScene / static_cast<float>(pixels);
    lid->SetTransform(D2D1::Matrix3x2F::Scale(unit, unit) * D2D1::Matrix3x2F(a, b, c, d, e, f));
    Fill(g, lid.get());
  }

  void FlapEdges(ID2D1Geometry* g, Point a, Point b, Point o, float lit) const {
    Stroke(g, Ink(Rgba(12, 16, 22, 0.7f)), zk, round.get());
    Line(Add(a, o), Add(b, o), Rgba(236, 222, 196, 0.28f + 0.4f * lit), 1.2f * zk);
  }

  // The light inside: a soft halo over the walls and its hot source deep in
  // the box, both breathing slowly.
  void Inner(float gl, double idle) const {
    if (gl <= 0) {
      return;
    }
    const Lighter add(dc.get());
    const float b = std::min(1.0f, gl * Breath(idle));
    if (insideGlow) {
      insideGlow->SetOpacity(b);
      Fill(mouth.get(), insideGlow.get());
    }
    if (insideCore) {
      insideCore->SetOpacity(b);
      Fill(mouth.get(), insideCore.get());
    }
  }

  void Interior(float gl, double idle) const {
    Fill(insideLeft.get(), insideLeftShade.get());
    Fill(insideRight.get(), insideRightShade.get());
    Inner(gl, idle);
    const Lighter add(dc.get());
    Line(kLidT, kLidF, Rgba(255, 222, 170, 0.10f + 0.30f * gl), zk);
  }

  // The light scattered in the air over the open box: a few faint shafts out
  // of its source, each breathing on its own slow wave.
  void Shafts(float a, double idle) const {
    if (a <= 0.003f || !shaft) {
      return;
    }
    constexpr Point kSource{263, 214};
    constexpr float kHaze = 0.6f;
    constexpr std::array<float, 5> kAngles{-24, -11, 1, 12, 23};  // degrees from upright
    constexpr std::array<double, 5> kRates{0.41, 0.53, 0.37, 0.61, 0.47};
    const Lighter add(dc.get());
    D2D1::Matrix3x2F was = D2D1::Matrix3x2F::Identity();
    t->GetTransform(&was);
    const D2D1_RECT_F at{kSource.x - kShaftWidth / 2, kSource.y - kShaftHeight, kSource.x + kShaftWidth / 2, kSource.y};
    for (std::size_t i = 0; i < kAngles.size(); ++i) {
      const float breath = 0.6f + 0.4f * (0.5f + 0.5f * Wave(idle * kRates[i] + static_cast<double>(i) * 2.39996));
      t->SetTransform(D2D1::Matrix3x2F::Rotation(kAngles[i], P(kSource)) * was);
      t->DrawBitmap(shaft.get(), at, std::min(1.0f, a * breath * kHaze), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    t->SetTransform(was);
  }

  // The flow's spiral: three ribbons of light, one inside another, each
  // twisting as it goes up - wide where it faces the eye, its two edges one
  // bright line where it turns edge-on, thin and faint on the far side - the
  // top swaying and fraying into dust. On them ride the packets, a comet
  // each with its tail behind it: the upload's rising out of the box, the
  // download's falling into it (`up`: the share that rises). Before, three
  // threads of one cone read as a spring, and the marks between the packets
  // (chevrons, cells) were two or three pixels in the window - nothing
  // (19.11.2026). `grow`: how far it has come up out of the box (it goes back
  // in the same way), a soft bead at its front; `a`: how bright; `clock`: its
  // own (it slows as the flow weakens); `glow`: for the scatter - the light
  // alone, no edges, no dust.
  void Stream(float grow, float a, double clock, bool glow = false) const {
    if (grow <= 0.003f || a <= 0.003f) {
      return;
    }
    const Lighter add(dc.get());
    const float z = zk;
    constexpr float kSpan = kStreamTop + 25;
    constexpr float kFeather = 50;
    constexpr float kStep = 4;                                // a ribbon's piece, units of height
    constexpr int kPieces = static_cast<int>(kSpan / kStep);  // up to just under the top
    constexpr int kSlots = 8;                                 // places for packets on a strand, each way
    // Taken in this order, those holding one spread evenly at any count.
    constexpr std::array<int, kSlots> kOrder{0, 4, 2, 6, 1, 5, 3, 7};
    constexpr int kTail = 6;  // a comet's tail, in pieces
    // Packets on a strand by the traffic: one and a half in the quiet, six at
    // full, the next one fading in; so many rising, the rest falling.
    const float carried = 1.5f + 4.5f * busy;
    const float rising = carried * up;
    const float falling = carried - rising;
    const float front = -25 + (kSpan + kFeather) * std::min(1.0f, grow);
    const auto shown = [front](float h) { return StreamFade(h) * Smooth01((front - h) / kFeather); };
    const auto air = [](double x) { return static_cast<float>(std::sin(x)); };
    const float train = Phase(clock, 0.17);
    const float bead = a * std::sin(std::numbers::pi_v<float> * std::min(1.0f, grow)) * 0.8f;
    struct Vertex {
      Point p;
      Point n;     // across the ribbon, a unit
      float w = 0; // its width
      float e = 0; // its light
      float f = 0; // 1 on the near side, 0 the far
    };
    std::array<Vertex, kPieces + 1> ribbon{};
    for (std::size_t j = 0; j < kStrands.size(); ++j) {
      const Strand& s = kStrands.at(j);
      const auto jf = static_cast<float>(j);
      const float spin = static_cast<float>(std::fmod(clock * kTurn * s.spin, static_cast<double>(kTwoPi)));
      const auto phiAt = [&](float h) { return h * kTwoPi / (kPitch * s.pitch) + s.phase - spin; };
      // On its cone, breathing a little in and out; the axis swaying, the more the higher.
      const auto at = [&](float h) {
        const StreamPoint p = StreamAt(h, phiAt(h));
        const float cy = kStreamY - h;
        const float k = s.radius * (1 + 0.06f * air(h * 0.05 - clock * 0.9 + jf * 1.7));
        const float sway = 7 * std::pow(std::max(0.0f, h) / kStreamTop, 1.5f) * air(clock * 0.5 + h * 0.012);
        return StreamPoint{kStreamX + sway + (p.x - kStreamX) * k, cy + (p.y - cy) * k, p.z};
      };
      // Up top it breaks into pieces, more of them gone the higher.
      const auto fray = [&](float h) {
        const float top = Smooth01((h - 0.5f * kStreamTop) / (0.45f * kStreamTop));
        const float gate = Smooth01((0.5f + 0.5f * air(h * 0.23 + jf * 2.1 + clock * 1.1) - 0.35f) / 0.3f);
        return 1 + (gate - 1) * top;
      };

      for (int i = 0; i <= kPieces; ++i) {
        const float h = -25 + kStep * static_cast<float>(i);
        const StreamPoint p = at(h);
        const StreamPoint before = at(h - 1);
        const StreamPoint after = at(h + 1);
        float dx = after.x - before.x;
        float dy = after.y - before.y;
        const float len = std::hypot(dx, dy);
        if (len > 0) {
          dx /= len;
          dy /= len;
        }
        const float f = 0.5f + 0.5f * p.z;
        const float turn = std::abs(std::cos(0.37f * phiAt(h) + jf * 1.3f));  // 0: edge-on
        const float out = 0.45f + 0.55f * Smooth01((h + 25) / 90);           // narrow out of the mouth
        Vertex& v = ribbon.at(static_cast<std::size_t>(i));
        v.p = {p.x, p.y};
        v.n = {-dy, dx};
        v.w = kRibbon * s.width * (0.3f + 0.7f * f) * (0.15f + 0.85f * turn) * out;
        v.e = shown(h) * a * s.light * fray(h);
        v.f = f;
      }
      for (std::size_t i = 0; i + 1 < ribbon.size(); ++i) {
        const Vertex& p = ribbon.at(i);
        const Vertex& q = ribbon.at(i + 1);
        const float e = (p.e + q.e) / 2;
        if (e < 0.01f) {
          continue;
        }
        const float f = (p.f + q.f) / 2;
        const float core = e * (0.06f + 0.14f * f) * (glow ? 1.4f : 1.0f);
        t->DrawLine(P(p.p), P(q.p), Ink(Rgba(255, 204, 136, core)), std::max((p.w + q.w) / 2, z));
        if (glow) {
          continue;
        }
        const D2D1_COLOR_F edge = Rgba(255, 234, 200, e * (0.15f + 0.33f * f));
        for (const float across : {-0.5f, 0.5f}) {
          t->DrawLine(P(Add(p.p, Times(p.n, across * p.w))), P(Add(q.p, Times(q.n, across * q.w))), Ink(edge), 0.9f * z);
        }
      }

      // The dust it sheds up top, drifting out from the axis.
      if (!glow) {
        for (int k = 0; k < 5; ++k) {
          const auto kf = static_cast<float>(k);
          const float age = Fraction(clock * 0.3 + kf / 5 + jf * 0.37);
          const float h0 = kStreamTop * (0.5f + 0.3f * Fraction(kf * 0.618 + jf * 0.29)) + 35 * age;
          const StreamPoint p = at(h0);
          const float cy = kStreamY - h0;
          const float spread = 1 + 0.45f * age;
          const Point d{kStreamX + (p.x - kStreamX) * spread, cy + (p.y - cy) * spread - 10 * age};
          const float e = a * s.light * Smooth01((front - h0) / kFeather) * std::sin(std::numbers::pi_v<float> * age);
          if (e > 0.02f) {
            const float r = 1.3f * z;
            t->FillEllipse(D2D1::Ellipse(P(d), r, r),
                           Ink(Rgba(255, 226, 180, 0.5f * e * (0.4f + 0.6f * (0.5f + 0.5f * p.z)))));
          }
        }
      }

      // The packets in even steps, a strand a third of a step after the one
      // before; the falling ones half a step off the rising.
      const auto comets = [&](float count, bool rises) {
        for (int n = 0; n < kSlots; ++n) {
          const float share = std::clamp(count - static_cast<float>(n), 0.0f, 1.0f);
          if (share <= 0) {
            break;
          }
          const int i = kOrder.at(static_cast<std::size_t>(n));
          const float slot = (static_cast<float>(i) + jf / 3 + (rises ? 0.0f : 0.5f)) / kSlots;
          const float hp = Fraction(rises ? slot + train : slot - train) * kSpan - 25;
          const float behind = rises ? -1.0f : 1.0f;
          const float tail = 16 + 5 * static_cast<float>((i * 3 + static_cast<int>(j)) % 5);  // 16..36 units
          const float ka = a * share;
          for (int piece = 0; piece < kTail; ++piece) {
            const auto pf = static_cast<float>(piece);
            const float h0 = hp + behind * tail * pf / kTail;
            const float h1 = hp + behind * tail * (pf + 1) / kTail;
            const float e = shown(h0) * ka * std::pow(1 - pf / kTail, 1.4f);
            if (e < 0.01f) {
              continue;
            }
            const StreamPoint p = at(h0);
            const StreamPoint q = at(h1);
            const float f = 0.5f + 0.5f * p.z;
            // Going out: near white; coming in: amber - and each tail behind its head.
            const D2D1_COLOR_F ink = rises ? Rgba(255, 250 - piece * 6, 236 - piece * 14, e * (0.7f + 0.3f * f))
                                           : Rgba(255, 200 - piece * 10, 116 - piece * 12, e * (0.7f + 0.3f * f));
            t->DrawLine({p.x, p.y}, {q.x, q.y}, Ink(ink), (3.2f - pf * 0.4f) * z * (0.8f + 0.25f * f), round.get());
          }
          const StreamPoint head = at(hp);
          const float e = shown(hp) * ka;
          if (e > 0.02f) {
            const float f = 0.5f + 0.5f * head.z;
            const float r = 3 * z * (0.75f + 0.3f * f);
            const D2D1_POINT_2F c = D2D1::Point2F(head.x, head.y);
            t->FillEllipse(D2D1::Ellipse(c, 2.2f * r, 2.2f * r),
                           Ink(rises ? Rgba(255, 244, 222, 0.2f * e) : Rgba(255, 190, 110, 0.22f * e)));
            const D2D1_COLOR_F ink = rises ? Rgba(255, 253, 246, e * (0.75f + 0.25f * f)) : Rgba(255, 220, 160, e * (0.75f + 0.25f * f));
            t->FillEllipse(D2D1::Ellipse(c, r, r), Ink(ink));
          }
        }
      };
      comets(rising, true);
      comets(falling, false);

      // its front while it comes up or goes back in
      if (bead > 0.02f) {
        const float hb = front - kFeather / 2;
        const float e = StreamFade(hb) * bead * s.light;
        if (e > 0.02f) {
          const StreamPoint p = at(hb);
          const float f = 0.5f + 0.5f * p.z;
          t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y), 7 * z, 7 * z), Ink(Rgba(255, 214, 150, 0.15f * e)));
          t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y), 2.2f * z, 2.2f * z),
                         Ink(Rgba(255, 246, 228, e * (0.6f + 0.4f * f))));
        }
      }
    }
  }

  // The ads blocked: each a red spark burning out on the front rim of the
  // box's mouth - a short burn, a few embers thrown up off it that cool and
  // fall back. Instead of the red dots that flew at the box from below.
  void Sparks() const {
    if (!ember) {
      return;
    }
    const Lighter add(dc.get());
    const float z = zk;
    for (const Spark& s : sparks) {
      if (s.age < 0 || s.age >= kSparkLife) {
        continue;
      }
      const float u = s.age / kSparkLife;
      const float fade = (1 - u) * (1 - u);
      const Point p = s.at < 0.5f ? Lerp(kLidL, kLidF, s.at * 2) : Lerp(kLidF, kLidR, s.at * 2 - 1);
      const float burn = std::min(1.0f, s.age / 0.04f) * fade;
      const float r = 15 + 9 * u;
      ember->SetCenter(P(p));
      ember->SetRadiusX(r);
      ember->SetRadiusY(r * 0.8f);
      ember->SetOpacity(burn);
      Ellipse(p, r, r * 0.8f, ember.get());
      const float core = 2.4f * z * (1 - 0.5f * u);
      t->FillEllipse(D2D1::Ellipse(P(p), core, core), Ink(Rgba(255, 226, 204, 0.9f * burn)));
      // White-hot as they leave, red as they cool.
      const D2D1_COLOR_F ink = Mix(Rgba(255, 216, 176), Rgba(232, 78, 56), std::min(1.0f, u * 2.5f));
      for (int k = 0; k < 5; ++k) {
        const auto kf = static_cast<float>(k);
        const float wobble = std::sin(s.at * 37 + kf * 2.3f);
        const float angle = -std::numbers::pi_v<float> / 2 + (kf - 2) * 0.5f + 0.15f * wobble;
        const float speed = 60 + 22 * wobble;
        const auto where = [&](float age) {
          return Point{p.x + std::cos(angle) * speed * age, p.y + std::sin(angle) * speed * age + 140 * age * age};
        };
        D2D1_COLOR_F c = ink;
        c.a = 0.85f * std::pow(1 - u, 1.5f);
        Line(where(std::max(0.0f, s.age - 0.05f)), where(s.age), c, 1.3f * z, round.get());
      }
    }
  }

  // How much of the opened box is its picture: none while the flaps swing up,
  // all from nearly open; the same way back as they close.
  float Share(float op) const { return openBox ? Smooth01((op - 0.5f) / 0.45f) : 0.0f; }

  D2D1_RECT_F OpenRect() const {
    const float m = static_cast<float>(pad) * kScene / static_cast<float>(std::max(1U, pixels));
    return {-m, -m, kScene + m, kScene + m};
  }

  // `grow`, `bright`: the spiral's (Stream); `clock`: its own; `idle`: the window's.
  void OpenBox(float op, float grow, float bright, double clock, double idle) const {
    const float flow = std::min(1.0f, grow) * bright;
    const float gl = std::min(1.0f, 0.3f * std::min(1.0f, op) + 0.7f * flow);
    const float pic = Share(op);
    if (pic < 1) {
      // The drawn flaps give way to the picture over its second half.
      const float drawn = std::min(1.0f, 2 * (1 - pic));
      const bool faded = drawn < 1 && flapsLayer;
      if (faded) {
        t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), drawn),
                     flapsLayer.get());
      }
      DrawnBox(op, gl, pic > 0 ? 0.0f : grow, bright, clock, idle);
      if (faded) {
        t->PopLayer();
      }
    }
    if (pic > 0) {
      t->DrawBitmap(openBox.get(), OpenRect(), pic, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      Inner(kPictureGlow * gl * pic, idle);
      Stream(grow, bright, clock);
    }
    if (gl > 0 && mouthBloom) {
      const Lighter add(dc.get());
      mouthBloom->SetOpacity(std::min(1.0f, 0.5f * gl * Breath(idle)));  // the rest is the scatter's
      Ellipse({263, 140}, 230, 287.5f, mouthBloom.get());
    }
    Shafts(flow, idle);
  }

  // The flaps drawn swinging on the lid's edges, the lid's picture on the big ones.
  void DrawnBox(float op, float gl, float grow, float bright, double clock, double idle) const {
    const float thB = op * kOpenBack * kDegrees;
    const float thF = op * kOpenFront * kDegrees;
    const Point oBL = FlapOff(kLidU, thB);
    const Point oBR = FlapOff(kLidW, thB);
    const Point oFR = FlapOff(Times(kLidU, -1), thF);
    const Point oFL = FlapOff(Times(kLidW, -1), thF);
    const auto under = [](float th) { return th > 0.69f; };  // edge-on by ~40 degrees: its underside shows
    // The back flaps: the small one (T-R), over it the big one (the lid's picture while it lies flat).
    const auto backSmall = Flap(kLidT, kLidR, oBR);
    if (under(thB)) {
      PlainFlap(backSmall.get(), Mid(kLidT, kLidR), Mid(Add(kLidT, oBR), Add(kLidR, oBR)), flaps[0].get(), gl, 1);
    } else {
      PlainFlap(backSmall.get(), kLidT, Add(kLidT, oBR), flaps[1].get(), gl, 0);
    }
    FlapEdges(backSmall.get(), kLidT, kLidR, oBR, gl);
    const auto backBig = Flap(kLidT, kLidL, oBL);
    if (under(thB)) {
      PlainFlap(backBig.get(), Mid(kLidT, kLidL), Mid(Add(kLidT, oBL), Add(kLidL, oBL)), flaps[2].get(), gl, 1);
    } else {
      TexFlap(backBig.get(), Times(oBL, 2), kLidW, kLidT);
    }
    FlapEdges(backBig.get(), kLidT, kLidL, oBL, gl);
    Interior(gl, idle);
    if (body) {
      t->DrawBitmap(body.get(), D2D1::RectF(0, 0, kScene, kScene), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    Stream(grow, bright, clock);
    // The front flaps: their outsides (the big one with the tape).
    const auto frontLeft = Flap(kLidL, kLidF, oFL);
    PlainFlap(frontLeft.get(), Mid(kLidL, kLidF), Mid(Add(kLidL, oFL), Add(kLidF, oFL)), flaps[3].get(), gl, 0);
    FlapEdges(frontLeft.get(), kLidL, kLidF, oFL, 0.5f);
    const auto frontRight = Flap(kLidR, kLidF, oFR);
    TexFlap(frontRight.get(), Times(oFR, -2), kLidW, Add(kLidR, Times(oFR, 2)));
    if (flapShadow) {
      flapShadow->SetStartPoint(P(Mid(kLidR, kLidF)));
      flapShadow->SetEndPoint(P(Mid(Add(kLidR, oFR), Add(kLidF, oFR))));
      Fill(frontRight.get(), flapShadow.get());
    }
    FlapEdges(frontRight.get(), kLidR, kLidF, oFR, 0.6f);
  }

  // The light's scatter: the source in the box and the spiral drawn again
  // small and blurred twice - a tight halo round each line, a wide haze in
  // the air that spills over the flaps - and added on top, warmer than the
  // light itself, as in dusty air. Without it the spiral was lines drawn on
  // the box, not light (he, 19.11.2026). Nothing where the target can't blur.
  void Scatter(float op, float grow, float bright, double clock, double idle) {
    const float flow = std::min(1.0f, grow) * bright;
    const float gl = std::min(1.0f, 0.3f * std::min(1.0f, op) + 0.7f * flow);
    if (!glowTarget || !glowDc || !dc || !haloTint || !hazeTint || gl <= 0.003f) {
      return;
    }
    ID2D1RenderTarget* const main = t;
    glowTarget->BeginDraw();
    glowTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
    glowTarget->SetTransform(D2D1::Matrix3x2F::Translation(-kGlowLeft, -kGlowTop) *
                             D2D1::Matrix3x2F::Scale(glowK, glowK));
    t = glowTarget.get();
    std::swap(dc, glowDc);
    Inner(kScatterInner * gl, idle);
    Stream(grow, bright, clock, true);
    Sparks();
    std::swap(dc, glowDc);
    t = main;
    if (FAILED(glowTarget->EndDraw())) {
      return;
    }
    const auto tint = [](ID2D1Effect* e, float gain, float green, float blue) {
      e->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                  D2D1::Matrix5x4F(1, 0, 0, 0, 0, green, 0, 0, 0, 0, blue, 0, 0, 0, 0, std::min(1.0f, gain), 0, 0, 0, 0));
    };
    tint(haloTint.get(), kHaloGain, 0.92f, 0.8f);
    tint(hazeTint.get(), kHazeGain * Breath(idle), 0.8f, 0.58f);
    D2D1::Matrix3x2F was = D2D1::Matrix3x2F::Identity();
    t->GetTransform(&was);
    t->SetTransform(D2D1::Matrix3x2F::Scale(1 / glowK, 1 / glowK) * D2D1::Matrix3x2F::Translation(kGlowLeft, kGlowTop) *
                    was);
    dc->DrawImage(hazeTint.get(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
    dc->DrawImage(haloTint.get(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
    t->SetTransform(was);
  }

  void Draw(ID2D1RenderTarget* target, const VitrineFrame& f, const VitrinePlace& place, bool hovered, bool pressed) {
    float dpiX = 96;
    float dpiY = 96;
    target->GetDpi(&dpiX, &dpiY);
    const float px = std::max(dpiX, 1.0f) / 96;
    if (place.scale <= 0 || !Ensure(target, kScene * place.scale * px)) {
      return;
    }
    t = target;
    // A scale that draws the bitmaps pixel for pixel, the box's foot where the place puts it.
    const float scale = static_cast<float>(pixels) / (kScene * px);
    const float footX = place.origin.x + kCx * place.scale;
    const float footY = place.origin.y + kOy * place.scale;
    D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Identity();
    t->GetTransform(&base);
    D2D1::Matrix3x2F at = D2D1::Matrix3x2F::Scale(scale, scale) *
                          D2D1::Matrix3x2F::Translation(footX - kCx * scale, footY - kOy * scale) * base;
    if (base._11 == 1 && base._22 == 1 && base._12 == 0 && base._21 == 0) {
      at._31 = std::round(at._31 * px) / px;  // on whole pixels: the box's picture stays sharp
      at._32 = std::round(at._32 * px) / px;
    }
    // The pointer on the case and the press, eased (the window's at once when the frame has none).
    const float hv = f.hover >= 0 ? f.hover : (hovered ? 1.0f : 0.0f);
    const float pr = f.press >= 0 ? f.press : (pressed ? 1.0f : 0.0f);
    // Pressed, the whole case gives a little, from its foot.
    const float give = 1 - 0.016f * pr;
    const D2D1::Matrix3x2F scene = D2D1::Matrix3x2F::Scale(give, give, P(Down(kPF, kThick))) *
                                   D2D1::Matrix3x2F::Translation(f.shake.x, f.shake.y) * at;
    zk = std::max(1.0f, 0.8f / (scale * px));

    const bool dimmed = f.dim > 0.01f && layer;
    if (dimmed) {
      t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), 1 - 0.55f * f.dim),
                   layer.get());
    }
    t->SetTransform(scene);

    const Opened& o = f.opened;
    const float pa = std::clamp(o.plinth, 0.0f, 1.0f);
    // Open, the box rises a touch to the pointer and gives under the press.
    const float lift = o.lift + (1 - pa) * (1.5f * hv - 2.2f * pr);
    const float touch = (1 + 0.1f * hv) * (1 - 0.14f * pr);
    const float cone = o.cone * (0.4f + 0.6f * f.flow) * touch;
    const float bright = f.flow * touch;
    const float motes = o.motes * f.flow;
    const float warmth = o.stream * f.flow;
    const float pool = o.pool + 0.2f * f.danger + f.glass * (0.08f * hv - 0.06f * pr);

    if (pa > 0) {
      Plinth(pa);
      Pool(pool, f.danger, f.idle, pa);
      Shadow(std::min(1.0f, lift / 10) * pa);
    }
    Back(f.glass, pa);
    Bloom(std::min(1.0f, cone));
    Cone(cone);
    DrawMotes(motes, warmth, f.idle, false);

    const bool opened = o.open > 0.003f && body;
    t->SetTransform(D2D1::Matrix3x2F::Translation(0, -lift) * scene);
    busy = std::clamp(f.traffic, 0.0f, 1.0f);
    up = std::clamp(f.upShare, 0.0f, 1.0f);
    sparks = f.sparks;
    if (opened) {
      OpenBox(o.open, o.stream, bright, f.flowClock, f.idle);
      Scatter(o.open, o.stream, bright, f.flowClock, f.idle);
    } else if (box) {
      t->DrawBitmap(box.get(), D2D1::RectF(0, 0, kScene, kScene), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    Sparks();
    TopLight(cone, opened, opened ? Share(o.open) : 0.0f);
    t->SetTransform(scene);

    RimFront((o.rim + 0.12f * hv) * pa, pa);
    DrawMotes(motes, warmth, f.idle, true);
    if (f.glass > 0) {
      Glass(f.glass, f.danger, f.idle, hv, pr);
    }
    if (f.shards) {
      Cracks(*f.shards, f.crack, f.crackAlpha);
      if (f.shatter >= 0) {
        // Back in place, the pieces melt into the whole glass quickly: no seams left standing.
        const float shardAlpha = f.shardsBack ? (1 - f.glass) * (1 - f.glass) : 1.0f;
        Shards(f.shards, f.shatter, shardAlpha, scene);
        if (!f.shardsBack || f.glass <= 0) {
          Dust(*f.shards, f.shatter, shardAlpha * f.dustIn);
        }
      }
      Impact(f, f.shards->impact);
    }

    t->SetTransform(base);
    if (dimmed) {
      t->PopLayer();
    }
    t = nullptr;
  }
};

namespace {

// A square PNG's pixels, premultiplied BGRA, and its side; nothing if it isn't one.
std::pair<std::vector<std::uint8_t>, UINT> Decode(IWICImagingFactory* wic, std::span<const std::byte> png) {
  if (wic == nullptr || png.empty()) {
    return {};
  }
  wil::com_ptr<IWICStream> stream;
  wil::com_ptr<IWICBitmapDecoder> decoder;
  wil::com_ptr<IWICBitmapFrameDecode> frame;
  wil::com_ptr<IWICFormatConverter> converter;
  UINT w = 0;
  UINT h = 0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): WIC reads the bytes, never writes them
  auto* bytes = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(png.data()));
  if (FAILED(wic->CreateStream(stream.put())) ||
      FAILED(stream->InitializeFromMemory(bytes, static_cast<DWORD>(png.size()))) ||
      FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) ||
      FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(converter.put())) ||
      FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)) ||
      FAILED(converter->GetSize(&w, &h)) || w != h || w == 0) {
    return {};
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
  if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()))) {
    return {};
  }
  return {std::move(pixels), w};
}

}  // namespace

VitrineArt::VitrineArt(IWICImagingFactory* wic, std::span<const std::byte> png, std::span<const std::byte> openPng)
    : impl_(std::make_unique<Impl>()) {
  impl_->wic = wic;
  std::tie(impl_->source, impl_->side) = Decode(wic, png);
  std::tie(impl_->openSource, impl_->openSide) = Decode(wic, openPng);
}

VitrineArt::~VitrineArt() = default;

void VitrineArt::Draw(ID2D1RenderTarget* target, const VitrineFrame& frame, const VitrinePlace& place, bool hovered,
                      bool pressed) {
  if (target != nullptr) {
    impl_->Draw(target, frame, place, hovered, pressed);
  }
}

}  // namespace sovereign::tray

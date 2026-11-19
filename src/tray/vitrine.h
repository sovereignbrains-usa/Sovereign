#pragma once

#include <d2d1.h>
#include <wincodec.h>
#include <wil/com.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "tray_model.h"

// The connect button as a relic under glass (main screen; the plan of
// 18.11, his "the glass idea"): the sing-box box on a plinth under a museum
// glass hood. Off, the glass is whole and a gloss runs over it now and then.
// Turned on, it cracks where it was struck; connected, it shatters - shards,
// dust, a flash - the plinth goes, the box rises into a spotlight, its flaps
// open and a golden spiral of data flows out of it. Turned off, the shards
// fly back, the glass mends and the box sinks closed onto its plinth again.
// An error tints the glass red; the service down, the case goes dim.
//
// Drawn with Direct2D in the prototype's units: a 520-unit square scene
// (A:\work\singbox-cube\engine.js, its variant B), placed and scaled into the
// window. The motion is a pure function of the state and the time (Vitrine,
// VitrineFrame), tested in tests/unit/vitrine_test.cpp; the drawing
// (VitrineArt) holds the render target's bitmaps and brushes.

namespace sovereign::tray {

// Fast at first, settling at the end: t in 0..1.
float EaseOut(float t);

namespace vitrine {

// The scene's side, in its own units.
inline constexpr float kScene = 520;

// Seconds from the blow to the shards' release, and their moment of stillness after.
inline constexpr float kCrackTime = 0.14f;
inline constexpr float kHold = 0.07f;

struct Point {
  float x = 0;
  float y = 0;
};

// The glass hood's outline (six corners, clockwise from the top) and its
// three front edges' inner corner.
std::array<Point, 6> Hood();
Point HoodCenter();

// The part of the scene that is the button (the hood's bounds), the box's
// bounds, and the scene's whole height drawn (the spiral's top to the
// plinth's foot) - for the window's layout.
D2D1_RECT_F HoodBounds();
D2D1_RECT_F BoxBounds();
inline constexpr float kDrawnTop = -100;
inline constexpr float kDrawnBottom = 510;

// A blow the glass takes where no click says: right of its middle, a third up.
Point DefaultImpact();

// One piece of the broken glass: its outline round its centre, where it flies.
struct Shard {
  std::vector<Point> local;
  Point center;
  Point velocity;
  float left = 0, top = 0, right = 0, bottom = 0;  // the outline's bounds round the centre
  float spin = 0;        // radians a second
  float grow = 0;        // how fast it comes at the viewer (scale a second)
  float flip = 0;        // how fast it turns edge-on and back
  float vanish = 0;      // seconds after the release it starts to fade
  bool spark = false;    // catches the light edge-on
  float area = 0;
};

// A speck of glass dust or a glint, flying from its shard.
struct Speck {
  float start = 0;  // seconds after the release it leaves the shard
  float life = 0;
  float x = 0, y = 0, vx = 0, vy = 0;
  float wobbleRate = 0, wobblePhase = 0, wobble = 0;
  float size = 0;
};

// A crack: an edge between two shards, by how far it is from the blow.
struct Crack {
  Point a;
  Point b;
  float distance = 0;
};

struct Shatter {
  Point impact;
  std::vector<Shard> shards;
  std::vector<Speck> dust;
  std::vector<Speck> glints;
  std::vector<Crack> cracks;  // each edge once, nearest the blow first
};

// The glass broken at `impact`: Voronoi cells round it clipped to the hood,
// each with its flight and its dust. The same for the same seed.
Shatter BreakGlass(Point impact, std::uint32_t seed);

// The case after the glass broke, `u` seconds into the flight; `k` how far
// it is open (1; less while it closes again).
struct Opened {
  float lift = 0;    // units the box floats up
  float pool = 0;    // the light on the plinth
  float cone = 0;    // the spotlight from above
  float motes = 0;   // dust in its beam
  float rim = 0;     // the brass catching the light
  float plinth = 1;  // 1: the plinth and the hood's brass are there; 0: gone
  float open = 0;    // the flaps, 0 closed .. 1 open (overshoots a little)
  float stream = 0;  // the spiral of data
};
Opened AfterBreak(double u, float k);

// Where the spiral's strand passes at height `h` and angle `phi`; z its depth (-1 far .. 1 near).
struct StreamPoint {
  float x = 0;
  float y = 0;
  float z = 0;
};
StreamPoint StreamAt(float h, float phi);
// How much of the spiral shows at height `h`: it rises out of the box and fades at the top.
float StreamFade(float h);

// The VPN's traffic, bytes a second, as the spiral's 0..1: a trickle a
// little, a video or two most of it, 4 MB/s and more all of it.
float TrafficLevel(double bytesPerSecond);

// A blocked ad burning on the rim of the box's mouth: for so long, so many at once at most.
inline constexpr float kSparkLife = 0.6f;
inline constexpr std::size_t kSparks = 4;
// The open box's warm light on the window behind the case: the vitrine's own
// colour, not a flow's (ui_style.h).
inline constexpr D2D1_COLOR_F kRoomLight{1.0f, 196.0f / 255, 136.0f / 255, 0.10f};
struct Spark {
  float age = -1;  // seconds since it struck; <0 none
  float at = 0;    // where on the mouth's front rim, 0 its left corner .. 1 its right
};

}  // namespace vitrine

// One frame of the case, all the drawing needs.
struct VitrineFrame {
  Display display = Display::Off;
  double idle = 0;         // seconds: the gloss, the motes, the spiral's turn
  float glass = 1;         // the whole hood's opacity
  float crack = 0;         // units from the blow the cracks reach
  float crackAlpha = 0;
  float shatter = -1;      // seconds since the blow for the shards, the dust, the ring; <0 none
  bool shardsBack = false; // the shards fly back (shatter counts down)
  float dustIn = 1;        // 0..1: their dust, fading in as they start back - not standing in the air before
  float flash = -1;        // seconds since the blow for its flash; <0 none
  float mendFlash = -1;    // seconds since the glass mended, for its soft flash; <0 none
  vitrine::Opened opened;
  float flow = 1;          // the spiral's and the spotlight's strength (weak while it reconnects)
  double flowClock = 0;    // seconds on the spiral's own clock: slow while it reconnects, fast with traffic
  float traffic = 0;       // 0..1: the VPN's traffic (TrafficLevel), eased - the spiral's packets
  std::array<vitrine::Spark, vitrine::kSparks> sparks{};  // blocked ads burning on the box's rim
  float hover = -1;        // 0..1: the pointer on the case, eased; <0: the window's, at once
  float press = -1;        // 0..1: the case pressed, eased; <0: the window's, at once
  float danger = 0;        // 0..1: the error's red
  float dim = 0;           // 0..1: the service down
  vitrine::Point shake;
  std::shared_ptr<const vitrine::Shatter> shards;
};

// The case at rest in `display`'s pose, the same each call: for the window
// before it knows the state's history, and for snapshots.
VitrineFrame VitrineRest(Display display);

// The case's motion from state to state. Times are seconds on one clock.
class Vitrine {
 public:
  // A new state at `now`; `hit` where a click struck the glass (scene
  // units), if one did. With `motion` false every change is at once.
  void To(Display display, double now, std::optional<vitrine::Point> hit, bool motion);
  VitrineFrame At(double now) const;
  // Whether it needs quick frames at `now` (beyond the slow idle life).
  bool Moving(double now) const;
  bool Known() const { return known_; }
  Display Shown() const { return display_; }
  // The pointer on the case and its press at `now`, eased in and out (with
  // `motion` false at once); true if either changed.
  bool Touch(bool hovered, bool pressed, double now, bool motion);
  // The VPN's traffic at `now` (TrafficLevel): the more, the faster the
  // spiral turns and the more packets it carries - eased, the rates come
  // once a second.
  void Traffic(float level, double now);
  // `count` ads blocked at `now`: a red spark each on the box's rim, in turn,
  // a few at most - more at once would only be a smear.
  void Blocked(std::uint64_t count, double now);

 private:
  enum class Glass : std::uint8_t { Whole, Cracking, Broken, Mending };
  // A value eased from one end to the other over its own time.
  struct Ease {
    float from = 0, to = 0;
    double at = -100;
    float span = 0;
    float At(double now) const;
    bool Moving(double now) const { return now - at < span; }
    bool Set(float value, double now, float in, float out);
  };

  float DangerAt(double now) const;
  float FlowAt(double now) const;
  double FlowClock(double now) const;
  float DimAt(double now) const;
  float TrafficAt(double now) const;
  double TrafficSum(double now) const;  // the eased traffic's integral: the spiral's extra clock
  float MendTau(double now) const;  // the shards' time flying back, easing to 0: whole again
  double MendedAt() const;
  // Where the box is: its flight (AfterBreak's u) and how open (its k).
  struct Box {
    double u = 0;
    float k = 0;
  };
  Box BoxAt(double now) const;

  bool known_ = false;
  bool motion_ = true;
  Display display_ = Display::Off;
  Glass glass_ = Glass::Whole;
  std::shared_ptr<const vitrine::Shatter> shatter_;
  std::uint32_t seed_ = 1;
  double crackAt_ = 0;      // the blow
  double flashAt_ = -1e9;   // the last flash (the blow, or the break at its height)
  double healAt_ = -1;      // cracks fading on a whole glass since; <0 not
  float healCrack_ = 0;     // how far they reached
  double breakAt_ = 0;      // the shatter
  double mendAt_ = 0;       // the closing began
  double mendU_ = 0;        // the box's flight frozen at it
  float mendK_ = 1;         // and how open it was
  float mendTau_ = 0;       // the shards' flight then
  double reopenAt_ = -1;    // turned on again mid-mend: the box opens from where it was; <0 not
  double reopenU_ = 0;
  float reopenK_ = 1;
  float dangerFrom_ = 0, dangerTo_ = 0;
  double dangerAt_ = -100;
  float flowFrom_ = 1, flowTo_ = 1;
  double flowAt_ = -100;
  double flowClock_ = -100;  // FlowClock at flowAt_
  Ease hover_, press_;
  float dimFrom_ = 0, dimTo_ = 0;
  double dimAt_ = -100;
  float trafficFrom_ = 0, trafficTo_ = 0;
  double trafficAt_ = -100;
  double trafficSum_ = 0;  // TrafficSum at trafficAt_
  struct Strike {
    double at = -1e9;
    float where = 0;
  };
  // Twice the sparks a frame shows: the ones queued ahead never take the slot
  // of one still burning.
  std::array<Strike, 2 * vitrine::kSparks> strikes_{};
  double lastStrike_ = -1e9;
  std::uint32_t struck_ = 0;  // sparks so far: the next one's slot and place
};

// Where the scene goes in the window: a scale and the place its origin lands (DIPs).
struct VitrinePlace {
  float scale = 0.5f;
  D2D1_POINT_2F origin{};
};
// The place that puts the hood's bounds at `button` (the Power item's rect).
VitrinePlace VitrinePlaceFor(D2D1_RECT_F button);
// A point in the window (DIPs) in the scene's units.
vitrine::Point ToScene(const VitrinePlace& place, D2D1_POINT_2F at);
D2D1_RECT_F FromScene(const VitrinePlace& place, D2D1_RECT_F scene);

// The case's bitmaps and brushes for one render target, made from the box's
// pictures (PNGs): closed, and opened - its walls on the closed one's, the
// scene widened by a sixteenth each side for the flaps. Without the opened
// one the flaps are drawn. Made again whenever the target is.
class VitrineArt {
 public:
  VitrineArt(IWICImagingFactory* wic, std::span<const std::byte> png, std::span<const std::byte> openPng = {});
  ~VitrineArt();
  VitrineArt(const VitrineArt&) = delete;
  VitrineArt& operator=(const VitrineArt&) = delete;

  void Draw(ID2D1RenderTarget* target, const VitrineFrame& frame, const VitrinePlace& place, bool hovered,
            bool pressed);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sovereign::tray

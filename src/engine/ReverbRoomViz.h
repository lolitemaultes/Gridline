// Gridline — Q-5: the reverb editor's room visual and decay view as pure,
// testable geometry (no ImGui here; the editor only strokes what this builds).
//
// The picture follows the reference's idea (a wireframe prism "room" whose
// side count and twist come from the room size and whose proportions come
// from the diffusion) and adds one distinct, readable cue per control, all
// taken from graphmodel::ReverbView, i.e. from the network that actually runs:
//
//   room size      side count (3 .. 28, continuous morph) + twist between rings
//   diffusion      radius vs height (slender column .. wide drum)
//   decay (T60)    floor ripples at fixed times after the pre-delay; ripple k
//                  shows the tail level at its time (10^(-3 t / T60))
//   reflections    sparks on the floor at the engine's tap times / gains
//   damping/colour top ring brightness (T60 at 6 kHz over mid T60, colour tilt)
//   bass           base ring weight (T60 below the crossover over mid T60)
//   width          horizontal spread of the whole figure
//   pre-delay      the room floats above its floor (gap = pre-delay)
//   wet level      glow strength;  modulation  ripple waviness
//
// Every value is eased towards its target with a frame-rate independent
// exponential (no popping on knob moves or preset loads); geometry is rebuilt
// only while easing or when the box changes. Fixed-size storage throughout:
// no allocation per frame.
#pragma once
#include <array>
#include <cstdint>
#include "GraphModels.h"

namespace gl::roomviz {

// ---------------------------------------------------------------- the eased visual state
enum Field : int {
  kSides,     // 3 .. 28, continuous
  kTwist,     // radians between the rings
  kRadius,    // 0..1 (diffusion)
  kRoom,      // 0..1 (room size; overall scale)
  kStretch,   // horizontal spread factor
  kBright,    // 0..1 top ring brightness (highs)
  kBass,      // 0..1 base ring weight (lows), 0.5 = flat
  kWet,       // 0..1 glow
  kLift,      // 0..1 pre-delay gap
  kMod,       // 0..1 ripple waviness
  kFreeze,    // 0..1
  kLogT60,    // log(T60 s), mid band (display clamp 0.05 .. 60 s)
  kLogT60Low,
  kLogT60High,
  kPre,       // pre-delay seconds
  kErSec0,    // 16 reflection tap times (s after the pre-delay)
  kErLvl0 = kErSec0 + 16,  // 16 tap levels 0..1 (0 = no tap)
  kFieldCount = kErLvl0 + 16
};
constexpr int kMaxSides = 28, kMaxTaps = 16;
constexpr double kMinT60 = 0.05, kMaxT60 = 60.0;

struct Look {
  std::array<float, kFieldCount> v{};
  float operator[](int i) const { return v[static_cast<size_t>(i)]; }
  float& operator[](int i) { return v[static_cast<size_t>(i)]; }
  double t60() const;
  double t60Low() const;
  double t60High() const;
};

// Target look for a reverb view (pure; monotonic in each control).
Look lookFor(const graphmodel::ReverbView& rv);

// Side count law (reference): round(e^(size*3.26) - 1) + 3, clamped to 28;
// continuous here so it can morph (integers at the reference's steps).
double sidesFor(double size01);

// Frame-rate independent exponential easing of every field.
struct Ease {
  static constexpr double kTau = 0.09;  // seconds to ~63 %
  Look cur;
  bool primed = false;
  // Moves `cur` towards `target` by dt seconds; returns true while still moving.
  // The first call snaps (an editor opening shows the state, not a fly-in).
  bool step(const Look& target, double dt);
};

// ---------------------------------------------------------------- room geometry (screen pixels)
struct Pt {
  float x = 0, y = 0;
  float depth = 0;  // 0 nearest .. 1 farthest (shading)
};
struct Seg {
  Pt a, b;
  float alpha = 1;     // fade (partial side morph)
  uint8_t kind = 0;    // 0 top ring, 1 base ring, 2 upright
};
constexpr int kRipples = 6, kRipplePts = 64;
struct Geometry {
  // Prism edges, sorted far to near (draw in order).
  int segCount = 0;
  std::array<Seg, 3 * kMaxSides> segs{};
  // The two rings in order (closed loops; the top ring is also the faint cap,
  // a convex polygon) and the floor shadow under the base.
  int capCount = 0;
  std::array<Pt, kMaxSides> cap{}, base{};
  std::array<Pt, kRipplePts> shadow{};
  // Floor ripples (closed loops) and their levels.
  std::array<std::array<Pt, kRipplePts>, kRipples> ripple{};
  std::array<float, kRipples> rippleLevel{};
  std::array<float, kRipples> rippleSec{};
  // Reflection sparks on the floor.
  std::array<Pt, kMaxTaps> spark{};
  std::array<float, kMaxTaps> sparkLevel{};
  // Box the figure is fitted in and the pixel unit for line weights.
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0, unit = 1;
};
// Builds the figure into the pixel rectangle (x0,y0)-(x1,y1). The framing is
// fixed (fitted to the extremes of every control), so every control change
// reads as a change of the figure and nothing ever leaves the box.
void build(const Look& l, float x0, float y0, float x1, float y1, Geometry& g);
// Ripple k's time after the pre-delay (s) and level for a T60.
double rippleSec(int k);
double rippleLevel(int k, double t60, bool freeze);

// ---------------------------------------------------------------- decay view (dB envelope)
constexpr int kDecayPts = 64;
struct Decay {
  float span = 1;  // seconds shown
  std::array<float, kDecayPts + 1> lo{}, mid{}, hi{};  // level 0..1 (1 = 0 dB, 0 = -60 dB) at t = span * i / N
  float pre = 0;   // pre-delay (s)
};
// Straight dB lines from 0 dB at the pre-delay to -60 dB at pre-delay + T60
// (low / mid / high bands); the span follows the longest band.
void buildDecay(const Look& l, Decay& d);
double decaySpan(const Look& l);
float decayLevel(double secAfterPre, double t60, bool freeze);

}  // namespace gl::roomviz

// Gridline — "synths" pack: Matrix FM drawn data (PluginState::data).
//
// Text, one record per line; anything not listed takes its default:
//   mfm 1
//   h <op> <harmonic>:<level>:<phase> …        harmonic levels (0..1) / phases (0..1)
//   e <unit> <art> <sustain> <loopStart> <loopEnd> <dt>,<y>,<tension>;… [dec=<i>] [tempo=1]   envelope
//   l <unit> <art> <dt>,<y>,<tension>;… [ph=<0..1>] [tn=<-1..1>] [pw=<-1..1>]   LFO cycle shape (dt in cycles)
//   m <unit> <art> <map> <dx>,<y>,<tension>;…  mapping curve (x 0..1)
//   w <filter> <dx>,<y>,<tension>;…            waveshaper transfer curve (x 0..1)
// Envelope dt are seconds; y 0..1; tension -1..1 (0 = straight).
// Default harmonics: a sine (harmonic 1 at full level). Default envelope:
// short attack / decay to 0.7 / release. Default LFO: sine-like. Default
// maps: neutral (no effect). Default waveshaper: straight line.
#pragma once
#include <array>
#include <string>
#include <vector>
#include "matrixfm.h"

namespace gl::packs::synths::mfm {

struct Pt {
  float dt = 0, y = 0, tension = 0;
};
struct Curve {
  std::vector<Pt> pts;
  int sustain = -1, loopStart = -1, loopEnd = -1;
  // Envelopes: the decay point ends the attack part (segments up to it use the
  // attack time scale, the ones after it up to the sustain point the decay
  // scale); -1 = none. tempo: dt are beats instead of seconds.
  int decay = -1;
  bool tempo = false;
  bool empty() const { return pts.empty(); }
};
struct ArtData {
  Curve env, lfo;
  std::array<Curve, kMaps> maps;  // empty = neutral
  float lfoPhase = 0;    // LFO start phase 0..1
  float lfoTension = 0;  // LFO shape tension -1..1 (bends the whole cycle)
  float lfoPw = 0;       // LFO pulse width -1..1 (0 = as drawn)
};
struct PatchData {
  std::array<std::array<float, kHarmonics>, kOps> level{}, phase{};
  std::array<std::array<ArtData, kFltArts>, kUnits> arts;  // [unit][target]
  std::array<Curve, kFilters> ws;                        // empty = straight
  PatchData();
};

// Default curves.
Curve defaultEnvelope();
Curve defaultLfo();
// Parses data text (unknown lines are ignored); false when it is not Matrix FM data.
bool parsePatch(const std::string& text, PatchData& out);
// Serializes only what differs from the defaults.
std::string serializePatch(const PatchData& d);

// Curve evaluation helpers (shared by the runtime and the editors).
// Value of a curve whose points advance by dt (x cumulative) at position x.
double curveAt(const Curve& c, double x);
// Total length (sum of dt).
double curveLength(const Curve& c);
// Segment shape: progress 0..1 bent by tension (-1..1).
double tensionShape(double t, double tension);

}  // namespace gl::packs::synths::mfm

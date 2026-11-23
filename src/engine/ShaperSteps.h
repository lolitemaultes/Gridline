// Gridline — the Shaper's step bank (K-257 phase 2): 8 patterns of three lanes (volume, cutoff,
// pan) of up to 64 steps, each step with a level, a chance (percent) and repeats (1..4 hits), and
// the pattern chain. Shared by the DSP, the editor and the tests.
//
// Pattern 1's first 16 levels per lane stay the plugin's own parameters (ShVol, ShCut, ShPan: they
// keep their automation and macros); everything else lives in PluginState::data as text:
//   SH1
//   v <pattern 0..7> <lane 0..2> <first step> <level x 10000> ...
//   q <pattern> <lane> <first step> <chance 0..100> ...
//   r <pattern> <lane> <first step> <repeats 1..4> ...
//   c <slot pattern 1..8, 0 empty> ...            (the chain, up to 16 slots)
// Steps without a value keep the lane's default (volume and cutoff 1, pan 0; chance 100, one hit).
// Empty data: nothing extra, the plugin plays exactly as before. parse() does not allocate.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"

namespace gl::shsteps {

inline constexpr int kPatterns = 8, kLanes = 3, kSteps = pp::kShaperMaxSteps, kChain = 16;

struct Bank {
  float val[kPatterns][kLanes][kSteps];
  uint8_t chance[kPatterns][kLanes][kSteps];  // percent
  uint8_t hits[kPatterns][kLanes][kSteps];    // 1..4
  int8_t chain[kChain];                       // -1 empty, else pattern 0..7
  bool anyChance[kPatterns][kLanes], anyHits[kPatterns][kLanes];
  int chainLen;  // slots up to the last filled one (0: no chain)
};

inline int laneBase(int lane) { return lane == 0 ? pp::ShVol : lane == 1 ? pp::ShCut : pp::ShPan; }
// A lane's default level, and the level a step plays when its chance skips it (or between repeats):
// silence for volume, the open filter for cutoff, the centre for pan.
inline float laneDefault(int lane) { return lane == 2 ? 0.f : 1.f; }
inline float laneRest(int lane) { return lane == 0 ? 0.f : lane == 1 ? 1.f : 0.f; }

void clear(Bank& b);
void parse(const char* text, Bank& b);  // clear() first, then the text's values (no allocation)
std::string write(const Bank& b);       // "" when everything is at its default
void finish(Bank& b);                   // recomputes the flags and the chain length

// The level of step k of a lane in pattern p (pattern 1's first 16: the plugin parameters).
inline float level(const PluginState& s, const Bank& b, int p, int lane, int k) {
  if (p == 0 && k < pp::kShaperPoints) {
    const size_t i = static_cast<size_t>(laneBase(lane) + k);
    return i < s.params.size() ? s.params[i] : laneDefault(lane);
  }
  return b.val[p][lane][k];
}

// The dice for one step: deterministic from the step's position in the song (its absolute step
// number on the lane's grid), the lane and the pattern, uniform in [0, 1). An export repeats exactly.
inline double dice(int64_t absStep, int lane, int pattern) {
  uint64_t z = static_cast<uint64_t>(absStep) * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(lane + 1) << 56) ^ (static_cast<uint64_t>(pattern + 1) << 48);
  z ^= z >> 30, z *= 0xBF58476D1CE4E5B9ull;
  z ^= z >> 27, z *= 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

// Swing: in each pair of steps the second starts late by swing x half a step (100%: at 3/4 of the pair).
// Returns the absolute step number and the fraction through it at `pos` steps from the song start.
inline int64_t swingStep(double pos, double swing, double& frac) {
  if (swing <= 0) {
    const double f = std::floor(pos);
    frac = pos - f;
    return static_cast<int64_t>(f);
  }
  const double pair = std::floor(pos * 0.5), x = pos - pair * 2, m = 1 + 0.5 * swing;
  if (x < m) {
    frac = x / m;
    return static_cast<int64_t>(pair) * 2;
  }
  frac = (x - m) / (2 - m);
  return static_cast<int64_t>(pair) * 2 + 1;
}

// K-257c: the lane engine shared by the Shaper and Terrace (one lane of one pattern). Which step
// plays at `beat` (beats from the song start) on a grid of `stepBeats` with `swing`, the lane
// looping every `steps` steps: returns the step's absolute number on the grid (the dice's seed);
// k is the step in the lane, fr the fraction through it.
inline int64_t laneStep(double beat, double stepBeats, double swing, int steps, int& k, double& fr) {
  const int64_t abs = swingStep(beat / stepBeats, swing, fr);
  k = static_cast<int>(((abs % steps) + steps) % steps);
  return abs;
}

// One lane's step data: chance and repeats per step (kSteps each), and whether any step uses them.
struct LaneRef {
  const uint8_t* chance;
  const uint8_t* hits;
  bool anyChance, anyHits;
  int seed;     // the dice's lane number (the Shaper: 0..2; Terrace: band * 3 + lane)
  int pattern;  // the dice's pattern number
  float rest;   // the level a skipped step (or the gap between repeats) plays
};

// The lane's level at step k (fraction fr, absolute step abs): the step's chance rolls its dice,
// repeats split it into hits that each hold the level for their first half, and `tension` (the
// curve, 0: hard steps) bends the line to the next step. level(k) is the step's drawn level.
template <class Level>
inline double laneValue(const LaneRef& r, int k, double fr, int64_t abs, int steps, double tension, Level level) {
  auto at = [&](int kk, int64_t ab) -> double {
    if (r.anyChance) {
      const int pc = r.chance[kk];
      if (pc < 100 && dice(ab, r.seed, r.pattern) * 100.0 >= pc) return r.rest;
    }
    return level(kk);
  };
  double a = at(k, abs);
  if (r.anyHits && r.hits[k] > 1) {  // repeats: each hit holds the level for its first half
    const double sub = fr * r.hits[k];
    return sub - std::floor(sub) < 0.5 ? a : static_cast<double>(r.rest);
  }
  if (std::fabs(tension) < 1e-4) return a;  // steps
  double b = at((k + 1) % steps, abs + 1);
  double kk = std::pow(2.0, -tension * 3.0);
  return a + (b - a) * std::pow(fr, kk);
}

}  // namespace gl::shsteps

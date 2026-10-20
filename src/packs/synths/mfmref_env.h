// Gridline — "synths" pack: Matrix FM reference model, articulator curves.
//
// The point-curve engine of the reference model: float point data, double
// segment phase, per-point curvature constants and sustain-offset weights,
// attack / decay / release time scales, loops; the same engine plays LFO
// envelopes (a 32-bit phase oscillator shaped by tension / skew / pulse
// width) and evaluates mapping curves. Internal to mfmref.
#pragma once
#include <cstdint>
#include <vector>
#include "mfmref.h"

namespace gl::packs::synths::mfmref::env {

struct Pt {
  float time = 0, value = 0, tension = 0;
  uint8_t shape = 0, wait = 0;
  int8_t sign = 0;  // 0, +1, -1, or 2 = straight
  float p10 = 0, p14 = 0, p18 = 0, weight = 0;
};

struct Env {
  std::vector<Pt> pts;
  int decay = -1, loop = -1, sus = -1, end = -1;  // end: sustain, or the last point when only a loop is set
  bool enabled = false, clamp = true, loopZero = false;
  int32_t flags = 0;
  double centre = 0, speed = 0;   // speed: time units per control tick
  float scale[4] = {1, 1, 0, 1};  // attack, decay, sustain offset, release
  // LFO oscillator
  float oscQinv = 0, oscRinv = 0, oscC = 0, oscCinv = 0, oscK = 0;
  uint32_t oscP = 0, oscQ = 0, oscT = 0;
  bool oscBipolar = false;
  uint32_t phaseOfs = 0;
  int32_t speedRaw = 0;           // LFO speed word (tempo changes rebuild inc)
  double inc = 0, incRaw = 0;     // phase increment per tick (u32 units); incRaw ignores freezing
  bool frozen = false, global = false;
  bool hasWait = false;  // wait points up to the sustain point (step envelopes)
};

struct State {
  int st = 9;  // 1 attack, 3 decay, 4 loop, 6 released, 9 ended
  int k = 0;
  int phase = 0;
  double phi = 0, rate = 0;
  float v0 = 0, delta = 0, ofs = 0, out = 0, lfoOut = 0;
  float susOfs = 0;
  float scale[4] = {1, 1, 0, 1};
  bool loopHold = false;
  uint32_t lfoPhase = 0;
  // Step envelopes: the current step, this voice's step (its key among the
  // held keys), the number of held keys; an inactive voice holds its value.
  int waitStep = -1, myIdx = -1, count = -1;
  bool frozen = false;
  float frozenV = 0;
};

// kind: 1 envelope, 2 LFO, 3 mapping curve. centre: 0.5 (bipolar / LFO) or 0.
Env prepare(const Record& c, int kind, double centre, double speed, bool clamp);
// LFO increment for a speed word and a tick length (time units per tick).
void setLfoSpeed(Env& e, double speed);
double pointValue(const Env& e, float ofs, int k, bool clamp);
inline double pointValue(const Env& e, float ofs, int k) { return pointValue(e, ofs, k, e.clamp); }
// shared: the state of the first held voice (shared envelopes), or null.
void start(const Env& e, State& s, const State* shared = nullptr);
void setStep(const Env& e, State& s, int idx, int count);
void tick(const Env& e, State& s, double dt);
void release(const Env& e, State& s);
void lfoStart(const Env& e, State& s, double globalTicks);
void lfoTick(const Env& e, State& s, double dt);
float oscValue(const Env& e, uint32_t phase);
float mapAt(const Env& m, double x);
bool mapFlat(const Env& m, double neutral);
float mapIterStart(const Env& m, State& s);
float mapIterStep(const Env& m, State& s, double dt);

// The family's exponential bend (also used by the wave shaper and tables).
struct Bend {
  float c = 0, k = 0, l = 0;
};
Bend makeBend(float c);
double bendAt(const Bend& b, double x);

}  // namespace gl::packs::synths::mfmref::env

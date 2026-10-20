// Gridline — FL-style articulator engine (J-41): point-list envelopes with
// decay / loop / sustain markers and time / level scalers, LFOs with an
// amplitude envelope, and mapping curves. Shared by the Envelope Controller
// (gl.envctl, articulator model) and the Slicex region engine (gl.slicer,
// region model).
//
// Behavioural reimplementation from analysis/rea/agents/j33/envc_spec.md
// §1–§4 (records, segment playback, LFO oscillator and speed law, maps).
// The engine runs per control step: one FL tick for the Envelope Controller,
// one control block for Slicex; dt is the step length in ticks.
//
// Records are parsed / written in FL's binary layout here (so the FLP
// decoders and encoders of both plugins share them) and kept in Gridline
// states as text (`writeText` / `readText`).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gl::flart {

enum Kind : int { KindEnv = 1, KindLfo = 2, KindMap = 3, KindDeck = 6 };

struct Point {
  double time = 0;   // duration of the segment that ends here (point 0: pre-delay)
  double value = 0;  // 0..1 (NaN → previous point's value)
  float tension = 0;
  uint8_t shape = 0, wait = 0, extra = 0;
  int8_t sign = 0;  // cached by FL (kept for byte-exact writing)
  bool operator==(const Point& o) const;
};

// One envelope / LFO / map / deck envelope record.
struct Record {
  int kind = KindEnv;
  bool hasKind = true;  // the kind word is stored (version-only in old files)
  int version = 3;
  std::vector<Point> pts;
  int32_t flags = 0, enabled = 0, decay = -1, loop = -1, sustain = -1;
  // Envelope extra (kind 1): attack / decay scale, sustain offset, release
  // scale (128 = neutral). LFO extra (kind 2): speed, tension, skew, pulse
  // width, then the start phase (version >= 3).
  int32_t extra[4] = {128, 128, 0, 128};
  uint32_t phase = 0;
  bool operator==(const Record& o) const;
  bool on() const { return enabled != 0; }
};

// Flag bits.
inline constexpr int32_t kTempoTime = 1;    // point times in beats (else 1.0 = 0.5 s)
inline constexpr int32_t kShared = 2;       // env: shared / LFO: global retriggered phase
inline constexpr int32_t kBipolarTen = 4;   // LFO: bipolar tension
inline constexpr int32_t kFrozen = 8;       // LFO: frozen phase

// Default records written by a fresh FL articulator (spec §1.6).
Record defaultEnvelope();
Record defaultLfo();
Record defaultMap(double level);

// ---- binary (FL state) -------------------------------------------------------
// Reads a record of `kind` at `at` (advanced past it, extra included).
// False on truncated / implausible data.
bool readRecord(const uint8_t* p, size_t n, size_t& at, int kind, Record& out);
void writeRecord(std::vector<uint8_t>& out, const Record& r);
// Size of the extra block that follows a record of this kind / version.
size_t extraSize(int kind, int version);

// ---- text (Gridline state) ---------------------------------------------------
// One line: "<kind> <hasKind> <version> <flags> <enabled> <decay> <loop>
// <sustain> <e0> <e1> <e2> <e3> <phase> <n> (time value tension shape wait
// extra sign)×n" — doubles round-trip exactly.
std::string writeText(const Record& r);
bool readText(const std::string& line, Record& out);

// ---- engine ------------------------------------------------------------------
// A record prepared for playback (UI thread): values with NaNs resolved,
// marker indices validated, per-point sustain-offset weights.
struct Prepared {
  int n = 0;
  std::vector<double> time, value, weight;
  std::vector<float> tension;
  std::vector<uint8_t> shape;
  int decay = -1, loop = -1, sus = -1;  // sus: effective sustain (loop without sustain → last point)
  bool enabled = false, tempo = false, loopZero = false;
  int32_t flags = 0;
  double loopTime = 0;
  // Maps: cumulative positions and the flat / constant shortcuts.
  std::vector<double> pos;
  bool flat = true, constant = true;
};
Prepared prepare(const Record& r, double neutral = 0.5);

// Time / level scalers of an envelope (128 = ×1).
struct Scales {
  double attack = 1, decay = 1, sustain = 0, release = 1;
};

// Per-voice envelope state.
struct EnvState {
  int k = 0;
  double phi = 0, rate = 0;
  double v0 = 0, delta = 0, o = 0, out = 0;
  double ps = 0, pe = 0;  // smooth tangents of the running segment
  double carry = 0;       // overshoot carried through instant segments (time units)
  enum : uint8_t { Off, Attack, Decay } state = Off;
  bool released = false, holding = false, ended = true, started = false;
  bool predelay() const { return k == 0 && !ended; }
};

// Segment speed in time units per tick: 1/PPQ for tempo-based records,
// (1/PPQ)·(120/tempo) otherwise (1.0 = 0.5 s).
double unitsPerTick(bool tempoBased, double tempo, int ppq);

// Envelope value of point k (clamped, minus `centre`).
double pointValue(const Prepared& e, int k, double o, double centre);
void envStart(EnvState& s, const Prepared& e, const Scales& sc, double centre);
// Advances by `dt` time units (unitsPerTick × ticks).
void envTick(EnvState& s, const Prepared& e, const Scales& sc, double centre, double dt);
void envRelease(EnvState& s, const Prepared& e, const Scales& sc, double centre);

// LFO oscillator (u32 phase, spec §3.2): params as stored (tension ±128,
// skew ±128 (/256), pulse width 0..128).
struct LfoShape {
  double T = 0, P = 0, Q = 0;  // in phase units (2^32 per cycle)
  double tensionC = 0;         // curvature (0 = linear)
  bool bipolarTension = false;
  void set(int tension, int skew, int pw, bool bipolar);
  double value(uint32_t phase) const;  // −1..1
  uint32_t startPhase(uint32_t offset) const;
};
// Phase increment per tick (u32 units) for speed 0..65536 at `unitsPerTick`.
uint32_t lfoIncrement(int speed, double unitsPerTick);
// LFO speed in Hz (free) or cycles per beat (tempo-based).
double lfoRate(int speed);

// Map value at x (0..1) minus `centre`.
double mapValue(const Prepared& m, double x, double centre);

}  // namespace gl::flart

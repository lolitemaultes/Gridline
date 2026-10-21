// Gridline — channel instrument envelopes (J-33, J-72): the per-note envelope /
// LFO for panning, volume, mod X (filter cutoff), mod Y (resonance) and pitch
// plus the channel filter that FL-style channels apply to sample voices and to
// "hybrid" native instruments. Settings are kept as the reference's raw
// integers (PluginState::ins text) so imports round-trip exactly.
//
// One envelope system, two timing behaviours (InsSettings::timing):
//   Smooth  — Gridline's own: envelopes, LFOs and the filter run per sample
//             (times in seconds / beats, no tick quantisation), gain and pan
//             are smooth per sample, filter coefficients follow a smoothed
//             control every few samples. The default for envelopes switched
//             on in Gridline.
//   Classic — FL timing ("FL timing" switch; set by every FL import): the
//             reference's results exactly — envelopes / LFOs step once per
//             song tick (PPQ), single-precision state, gains and the fast
//             low-pass coefficient ramp across each tick with a 0.001 / sample
//             slope limit, biquad / SVF coefficients change per tick.
// Shared laws (both behaviours, measured against the reference):
//   time knob v → u = 1001^(v/65536) − 1 (0 at v ≤ 100); 0.064·u s, or
//   0.128·u beats when tempo-synced; tension t → curve constant
//   c = sign(t)(e^(|t|/128 · 8.7557) − 1)/10, segment level y = 1 − (e^(L(1−x)) − 1)/|c|
//   (c > 0) or (e^(Lx) − 1)/|c| (c < 0), L = ln(1 + |c|);
//   volume gain (1 + lfo·amt)·env (env 0 → 1 → sustain → 0; amount unused),
//   pan offset 2·lfo·amt (the pan envelope does not move the voice), mod X / Y
//   add env·amt + lfo·amt to knob/256 (+ the note's mod X / Y), pitch adds
//   env·amount + lfo·amount cents; LFO period from the speed knob (larger =
//   slower, ≥ 3 ticks), sine / triangle / pulse, delay + attack ramp, global
//   (song-locked) phase option;
//   filter: fast LP (two-state resonant one-pole), RBJ LP / BP / HP / band
//   stop (×2 in series for LP x2 with a gain correction), Chamberlin SVF LP
//   (×2 with a softer second stage).
#pragma once
#include <cfloat>
#include <cstdint>
#include <string>

namespace gl {

// The reference's filters keep their sums (and the SVF / fast low-pass state
// within a tick) at x87 extended precision: classic timing uses it where the
// platform has it natively, double elsewhere (software quad would be too slow).
#if LDBL_MANT_DIG == 64
using InsExt = long double;
#else
using InsExt = double;
#endif

struct InsTarget {
  int flags = 0;     // bit0 env tempo sync, bit1 LFO tempo sync, bit2 full level when off, bit5 global LFO
  bool on = false;   // envelope enabled
  int predelay = 100, attack = 20000, hold = 20000, decay = 30000, sustain = 50, release = 20000, amount = 0;
  int lfoDelay = 100, lfoAttack = 20000, lfoAmount = 0, lfoSpeed = 32950, lfoShape = 0;
  int tensionA = 0, tensionD = 0, tensionR = 0;
  bool operator==(const InsTarget& o) const;
};

enum InsTargetIndex { InsPan, InsVol, InsModX, InsModY, InsPitch, InsTargets };

// Timing behaviour of the channel envelopes (see the top of this file).
enum class InsTiming : uint8_t { Smooth = 0, Classic = 1 };

struct InsSettings {
  InsTarget t[InsTargets];
  int cutoff = 256, reso = 0, type = 0;  // 0 fast LP, 1 LP, 2 BP, 3 HP, 4 band stop, 5 LP x2, 6 SVF LP, 7 SVF LP x2
  // Velocity / key tracking (FL's channel "tracking" records): [0] velocity,
  // [1] key; each {middle value, → panning, → mod X, → mod Y}. A note's
  // velocity / key away from the middle moves its pan and filter base.
  int track[2][4] = {{100, 0, 0, 0}, {60, 0, 0, 0}};
  // Text without a timing line is Classic (every envelope written before
  // J-72 came from an FL import); Smooth is written as "M 0".
  InsTiming timing = InsTiming::Classic;
  InsSettings();  // the reference's new-channel defaults
  bool operator==(const InsSettings& o) const;
  // True when anything differs from a neutral channel (no envelope, LFO or filter in use).
  bool active() const;
  bool volumeEnvelope() const { return t[InsVol].on; }
  bool tracking() const;
  bool classic() const { return timing == InsTiming::Classic; }
};

std::string formatIns(const InsSettings& s);
bool parseIns(const std::string& text, InsSettings& s);

// Seconds (or beats when `beats`) of a time knob value.
double insKnobTime(int v, bool beats);
// Level of a segment with tension `tension` (−128..128) at position x (0..1),
// x = 1 being full level (the shared curve law; used for drawing too).
double insCurve(int tension, double x);
// Instruments whose voices run the channel envelopes (J-33 / J-72).
bool insSupported(const std::string& pluginType);

// Per-voice runtime. Not thread-shared; no allocation.
class InsVoice {
 public:
  // `modX` / `modY`: note mod X / Y offsets (0 = neutral).
  // `songTick`: the song position (ticks of `ppq`) at the note start; it
  // places the voice on the song's tick grid and gives global LFOs their
  // (song-locked) phase.
  void start(const InsSettings* s, double sampleRate, double bpm, int ppq, double modX = 0, double modY = 0, double songTick = 0);
  // The note's key (MIDI, 60 = C5) and velocity (0..1 = 0..128) for the
  // velocity / key tracking; call right after start() (and setHostGains).
  void setNote(double key, float velocity);
  // Host gain law (instruments that take the reference host's note level and
  // panning from the channel, J-72): the voice then applies the constant-power
  // pan law to (note pan + pan modulation, note volume × volume envelope)
  // instead of the plain balance. Call right after start().
  void setHostGains(float notePan, float noteVolume);
  void release();
  bool active() const { return s_ != nullptr; }
  // The volume envelope finished (the voice can end).
  bool done() const;
  // Pitch offset for the next samples (classic: changes at ticks).
  double pitchSemis() const { return pitchCents_ / 100.0; }
  float pitchCents() const { return static_cast<float>(pitchCents_); }
  // Samples until the pitch can change (≥ 1): classic — the next tick;
  // smooth — the next control update.
  int pitchHold() const;
  // Runs a pending tick / control update (call before reading the pitch for the next samples).
  void sync();
  // Processes one stereo voice sample in place (filter, volume, pan); call once per sample.
  void process(float& l, float& r) { processWith(l, r, 1.0f); }
  // Same, with `pre` (e.g. a release declick) applied between the filter and the gains.
  void processWith(float& l, float& r, float pre);

 private:
  enum Stage { kPredelay = 0, kAttack = 1, kHold = 2, kDecay = 3, kSustain = 4, kRelease = 6, kDone = 9 };
  struct Curve {
    float c = 0, k = 0, L = 0;
  };
  // Values derived from one target's knobs (per voice start).
  struct Derived {
    bool on = false, global = false;
    int predelayT = 0, holdT = 0, lfoDelayT = 0;
    float attackInc = 1, decayInc = -1, releaseInc = -1, sustain = 0, amt = 0, lfoAmt = 0, lfoAttackInc = 1;
    float amtRaw = 0, lfoAmtRaw = 0;
    uint32_t lfoInc = 0;
    int shape = 0;
    Curve cv[3];  // attack, decay, release
    // Smooth timing: seconds per stage, LFO cycles per second.
    double predelayS = 0, attackS = 0, holdS = 0, decayS = 0, releaseS = 0, lfoDelayS = 0, lfoAttackS = 0, lfoHz = 0;
  };
  struct Env {
    int stage = kDone, counter = 0;
    float x = 0, y = 0;
    uint32_t lfoPhase = 0;
    int lfoDelayLeft = 0;
    float lfoRamp = 0, lfoOut = 0;
    // Smooth timing.
    double t = 0, lx = 0, ly = 0, lfoPh = 0, lfoT = 0, lfoV = 0;
  };
  struct Ramp {  // a gain ramped across one tick (the reference's two-lane accumulation)
    float cur = 0, step = 0, even = 0, odd = 0, tail = 0;
    void begin(float target, int len);
    float at(int i, int len);
  };
  void derive();
  void tickClassic();
  void beginChunk();
  void controlSmooth();
  float law(float pan, float vol, bool left) const;
  void gainTargets(float& gl, float& gr) const;
  void filterCoefs(double x, double r);
  void filterSample(float& l, float& r, InsExt g);

  const InsSettings* s_ = nullptr;
  Derived d_[InsTargets];
  Env st_[InsTargets];
  bool classic_ = true;
  double sr_ = 44100, bpm_ = 120, spt_ = 100;  // samples per tick
  int ppq_ = 96;
  double modX_ = 0, modY_ = 0;
  float trackPan_ = 0;  // tracking's pan offset (balance law voices)
  bool host_ = false;
  float notePan_ = 0, noteVol_ = 1;
  // Classic tick grid: absolute song sample, next tick index, samples left in the chunk.
  int64_t pos_ = 0, tickIdx_ = 0;
  int chunkLen_ = 0, chunkPos_ = 0;
  bool first_ = true;
  // Combined modulation (per tick / per control update).
  float panOfs_ = 0, volGain_ = 1, modXv_ = 0, modYv_ = 0;
  double pitchCents_ = 0;
  // Gains.
  Ramp gL_, gR_;
  // Smooth timing: per-sample control values (one-pole smoothed) and the control clock.
  int ctl_ = 0;
  double sGain_ = 0, sPan_ = 0, sX_ = 0, sY_ = 0, sPitch_ = 0;
  bool smoothFirst_ = true;
  double tgtX_ = 0, tgtY_ = 0;
  // Filter.
  int mode_ = 0;
  bool doubled_ = false, filterOn_ = false;
  float fg_ = 1, fgStep_ = 0, fgTarget_ = 1, fd_ = 0;  // fast LP coefficient (ramped) and damping
  InsExt fv_[2] = {0, 0}, fy_[2] = {0, 0};          // fast LP state (stored as double between ticks)
  InsExt fgAcc_ = 1;                                      // fast LP coefficient, accumulated per sample
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, lpGain_ = 1;
  float bx_[2][2][2] = {}, by_[2][2][2] = {};  // [stage][channel][1, 2]
  float svfF_ = 0, svfQ_ = 1;
  InsExt svfLow_[2][2] = {}, svfBand_[2][2] = {};  // SVF state (stored as float between ticks)
};

}  // namespace gl

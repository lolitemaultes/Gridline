// Gridline — channel instrument envelopes (J-33): the per-note envelope / LFO
// for panning, volume, mod X (filter cutoff), mod Y (resonance) and pitch plus
// the channel filter that FL-style channels apply to sample voices and to
// "hybrid" native instruments. Settings are kept as the reference's raw
// integers (PluginState::ins text) so imports round-trip exactly; the runtime
// follows the reference laws:
//   time knob v → u = 1001^(v/65536) − 1 (0 at v ≤ 100); 0.064·u s, or
//   0.128·u beats when tempo-synced; envelopes / LFOs step once per tick of
//   the song's PPQ; segment curves by tension (exponential families);
//   volume gain (1 + lfo·amt)·env (env 0 → 1 → sustain → 0), pan offset
//   2·lfo·amt, mod X / Y move the 0..1 knob, pitch in cents;
//   LFO period from the speed knob (larger = slower), sine / triangle /
//   pulse, delay + attack ramp, optional global phase;
//   filter: fast LP (two-state resonant one-pole), RBJ LP / BP / HP / band
//   stop (×2 in series for LP x2), Chamberlin SVF LP (×2).
#pragma once
#include <cstdint>
#include <string>

namespace gl {

struct InsTarget {
  int flags = 0;     // bit0 env tempo sync, bit1 LFO tempo sync, bit2 full level when off, bit5 global LFO
  bool on = false;   // envelope enabled
  int predelay = 100, attack = 20000, hold = 20000, decay = 30000, sustain = 50, release = 20000, amount = 0;
  int lfoDelay = 100, lfoAttack = 20000, lfoAmount = 0, lfoSpeed = 32950, lfoShape = 0;
  int tensionA = 0, tensionD = 0, tensionR = 0;
  bool operator==(const InsTarget& o) const;
};

enum InsTargetIndex { InsPan, InsVol, InsModX, InsModY, InsPitch, InsTargets };

struct InsSettings {
  InsTarget t[InsTargets];
  int cutoff = 256, reso = 0, type = 0;  // 0 fast LP, 1 LP, 2 BP, 3 HP, 4 band stop, 5 LP x2, 6 SVF LP, 7 SVF LP x2
  InsSettings();  // the reference's new-channel defaults
  bool operator==(const InsSettings& o) const;
  // True when anything differs from a neutral channel (no envelope, LFO or filter in use).
  bool active() const;
  bool volumeEnvelope() const { return t[InsVol].on; }
};

std::string formatIns(const InsSettings& s);
bool parseIns(const std::string& text, InsSettings& s);

// Per-voice runtime. Not thread-shared; no allocation.
class InsVoice {
 public:
  // `modX` / `modY`: note mod X / Y offsets (0 = neutral).
  // `songTick`: the song position (ticks of `ppq`) at the note start; global
  // LFOs take their phase from it (shared, song-locked).
  void start(const InsSettings* s, double sampleRate, double bpm, int ppq, double modX = 0, double modY = 0, double songTick = 0);
  void release();
  bool active() const { return s_ != nullptr; }
  // The volume envelope finished (the voice can end).
  bool done() const { return s_ && st_[InsVol].stage == kDone && s_->t[InsVol].on; }
  // Pitch offset in semitones for the next samples (updates at ticks).
  double pitchSemis() const { return pitchCents_ / 100.0; }
  // Processes one stereo voice sample in place (filter, volume, pan); call once per sample.
  void process(float& l, float& r);

 private:
  enum Stage { kPredelay, kAttack, kHold, kDecay, kSustain, kRelease, kDone };
  struct Env {
    int stage = kDone, counter = 0;
    double x = 0, y = 0;
    uint32_t lfoPhase = 0, lfoInc = 0;
    int lfoDelayLeft = 0;
    double lfoRamp = 0, lfoOut = 0;
  };
  void tick();
  double ticks(int v, bool sync) const;
  const InsSettings* s_ = nullptr;
  Env st_[InsTargets];
  double sr_ = 44100, bpm_ = 120, spt_ = 100;  // samples per tick
  int ppq_ = 96;
  double tickPos_ = 0;
  bool first_ = true;
  double modX_ = 0, modY_ = 0;
  // Outputs (per tick) and ramps.
  double gain_ = 1, gainTarget_ = 1, gainStep_ = 0;
  double pan_ = 0, panTarget_ = 0, panStep_ = 0;
  double pitchCents_ = 0;
  // Filter.
  int mode_ = 0;
  bool doubled_ = false, fastOn_ = false;
  double fg_ = 1, fgTarget_ = 1, fgStep_ = 0, fd_ = 0;
  double fv_[2] = {0, 0}, fy_[2] = {0, 0};
  double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, lpGain_ = 1;
  double bx_[2][2][2] = {}, by_[2][2][2] = {};  // [stage][channel][1, 2]
  double svfF_ = 0, svfQ_ = 1, svfLow_[2][2] = {}, svfBand_[2][2] = {};
};

}  // namespace gl

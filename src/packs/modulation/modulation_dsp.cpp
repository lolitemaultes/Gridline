// Gridline — "modulation" pack: controller and routing runtimes.
// Controllers produce control outputs (0..1) that internal controller links
// route to any parameter (see ModLink / AudioEngine::evaluateLinks).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../core/Expr.h"
#include "../../engine/Dsp.h"
#include "../Packs.h"
#include "modulation.h"
#include "modulation_dsp.h"

namespace gl::packs::modulation {

double tensionCurve(double x, double t) {
  x = x < 0 ? 0 : (x > 1 ? 1 : x);
  if (std::fabs(t) < 1e-6) return x;
  // t > 0 bends the curve down (slow start), t < 0 up (fast start).
  double k = std::pow(8.0, t);
  return std::pow(x, k);
}

double lfoWave(int shape, double phase, uint64_t cycle) {
  double ph = phase - std::floor(phase);
  switch (shape) {
    case 0: return 0.5 - 0.5 * std::cos(dsp::kTwoPi * ph);  // sine, starts at the bottom
    case 1: return 1.0 - std::fabs(2.0 * ph - 1.0);         // triangle (0 → 1 → 0)
    case 2: return ph < 0.5 ? 1.0 : 0.0;                     // square
    case 3: return ph;                                       // rising saw
    default: {                                               // random, one value per cycle
      uint64_t x = cycle * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
      x ^= x >> 31;
      x *= 0xBF58476D1CE4E5B9ull;
      x ^= x >> 29;
      return static_cast<double>(x >> 11) / 9007199254740992.0;
    }
  }
}

namespace {

inline float prm(const PluginState& s, int k, float dflt = 0) {
  return static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : dflt;
}
inline double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
// One-pole coefficient for a time constant (seconds) at the sample rate.
inline double coefFor(double seconds, double sr) { return seconds <= 0 ? 1.0 : 1.0 - std::exp(-1.0 / (seconds * sr)); }

// ------------------------------------------------------------ Peak Follower
class PeakFollower final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    double sr = ctx.sampleRate;
    double atk = coefFor(prm(s, PkAttack, 0.002f), sr), dec = coefFor(prm(s, PkDecay, 0.25f), sr);
    double ramp = prm(s, PkRamp, 1) >= 0.5f ? coefFor(0.01, sr) : 1.0;
    double pBase = prm(s, PkBase), pAmt = prm(s, PkAmount, 1), pTen = prm(s, PkTension);
    double lBase = prm(s, LfBase), lAmt = prm(s, LfAmount, 1), lTen = prm(s, LfTension), lPh = prm(s, LfPhase);
    int shape = static_cast<int>(prm(s, LfShape) + 0.5f);
    bool sync = prm(s, LfSync, 1) >= 0.5f;
    double beats = rateBeats(prm(s, LfSyncRate, 2));
    double hz = sync ? ctx.tempo / 60.0 / beats : prm(s, LfRate, 1);
    double spt = sr * 60.0 / (ctx.tempo * std::max(1, ctx.ppq));
    bool mute = prm(s, PkMute) >= 0.5f;
    for (int i = 0; i < n; ++i) {
      double x = std::max(std::fabs(L[i]), std::fabs(R[i]));
      env_ += (x - env_) * (x > env_ ? atk : dec);
      double pk = clamp01(pBase + pAmt * tensionCurve(clamp01(env_), pTen));
      double phase;
      if (sync && ctx.playing) {  // locked to the song position
        double beat = (ctx.tick + i / spt) / std::max(1, ctx.ppq);
        phase = beat / beats + lPh;
      } else {
        free_ += hz / sr;
        if (free_ >= 1e6) free_ -= 1e6;
        phase = free_ + lPh;
      }
      double w = lfoWave(shape, phase, static_cast<uint64_t>(std::floor(phase)));
      double lf = clamp01(lBase + lAmt * tensionCurve(w, lTen));
      outPeak_ += (pk - outPeak_) * ramp;
      outLfo_ += (lf - outLfo_) * ramp;
      if (mute) L[i] = R[i] = 0;
    }
  }
  void reset() override { env_ = outPeak_ = outLfo_ = 0; }
  int controlOutputs(float* out, int max) const override {
    float v[3] = {static_cast<float>(outPeak_), static_cast<float>(outLfo_), static_cast<float>(outPeak_ * outLfo_)};
    int k = std::min(3, max);
    for (int i = 0; i < k; ++i) out[i] = v[i];
    return k;
  }

 private:
  double env_ = 0, free_ = 0, outPeak_ = 0, outLfo_ = 0;
};

// ------------------------------------------------------- X-Y / X-Y-Z pads
class PadController final : public Effect {
 public:
  explicit PadController(int axes) : axes_(axes) {}
  void process(const PluginState& s, const ProcessContext& ctx, float*, float*, int n) override {
    double target[3], lo[3], hi[3], ten[3] = {0, 0, 0};
    double smooth;
    if (axes_ == 2) {
      target[0] = prm(s, XyX, 0.5f);
      target[1] = prm(s, XyY, 0.5f);
      lo[0] = prm(s, XyXMin), hi[0] = prm(s, XyXMax, 1), ten[0] = prm(s, XyXTension);
      lo[1] = prm(s, XyYMin), hi[1] = prm(s, XyYMax, 1), ten[1] = prm(s, XyYTension);
      if (prm(s, XyInvertY) >= 0.5f) target[1] = 1.0 - target[1];
      smooth = prm(s, XySmooth, 0.05f);
    } else {
      for (int a = 0; a < 3; ++a) {
        target[a] = prm(s, XzX + a, 0.5f);
        lo[a] = prm(s, XzXMin + 2 * a);
        hi[a] = prm(s, XzXMax + 2 * a, 1);
      }
      smooth = prm(s, XzSmooth, 0.08f);
    }
    double c = coefFor(smooth, ctx.sampleRate);
    for (int a = 0; a < axes_; ++a) {
      if (!primed_) cur_[a] = target[a];
      for (int i = 0; i < n; ++i) cur_[a] += (target[a] - cur_[a]) * c;  // per sample: any block split gives the same result
      out_[a] = lo[a] + (hi[a] - lo[a]) * tensionCurve(cur_[a], ten[a]);
    }
    primed_ = true;
  }
  void reset() override { primed_ = false; }
  int controlOutputs(float* out, int max) const override {
    int k = std::min(axes_, max);
    for (int i = 0; i < k; ++i) out[i] = static_cast<float>(clamp01(out_[i]));
    return k;
  }

 private:
  int axes_;
  bool primed_ = false;
  double cur_[3] = {0.5, 0.5, 0.5}, out_[3] = {0.5, 0.5, 0.5};
};

// ------------------------------------------------------- Formula Controller
struct FormulaAux final : AuxData {
  Expr expr;
  std::string error;
};
enum FormulaVar { FvA, FvB, FvC, FvT, FvBeat, FvBar, FvTempo, FvIn, FvPlaying, FvCount };

class FormulaController final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    ChannelAux none;
    processAux(s, none, ctx, L, R, n);
  }
  void processAux(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R,
                  int n) override {
    const auto* fa = dynamic_cast<const FormulaAux*>(aux.custom.get());
    double sr = ctx.sampleRate;
    double c = coefFor(prm(s, FmSmooth, 0.005f), sr);
    double spt = sr * 60.0 / (ctx.tempo * std::max(1, ctx.ppq));
    double vars[FvCount] = {prm(s, FmA, 0.5f), prm(s, FmB, 0.5f), prm(s, FmC, 0.5f), 0, 0, 0, ctx.tempo, 0, ctx.playing ? 1.0 : 0.0};
    double beatsPerBar = 4;
    for (int i = 0; i < n; ++i) {
      double x = std::max(std::fabs(L[i]), std::fabs(R[i]));
      inEnv_ = std::max(x, inEnv_ * 0.9995);
      // Evaluate on a fixed 16-sample grid of this instance's own clock.
      if ((clock_ & 15) == 0) {
        vars[FvT] = static_cast<double>(clock_) / sr;
        double beat = (ctx.tick + i / spt) / std::max(1, ctx.ppq);
        vars[FvBeat] = beat;
        vars[FvBar] = beat / beatsPerBar;
        vars[FvIn] = clamp01(inEnv_);
        target_ = fa && !fa->expr.empty() ? clamp01(fa->expr.eval(vars, &rng_)) : vars[FvA];
        if (!primed_) {
          out_ = target_;
          primed_ = true;
        }
      }
      out_ += (target_ - out_) * c;
      ++clock_;
    }
  }
  void reset() override {
    primed_ = false;
    inEnv_ = 0;
  }
  int controlOutputs(float* out, int max) const override {
    if (max < 1) return 0;
    out[0] = static_cast<float>(clamp01(out_));
    return 1;
  }

 private:
  bool primed_ = false;
  uint64_t clock_ = 0;
  uint32_t rng_ = 0x1234567u;
  double inEnv_ = 0, target_ = 0, out_ = 0;
};

// ------------------------------------------------------ Envelope Controller
class EnvelopeController final : public Generator {
 public:
  bool active() const override { return false; }  // produces no sound
  int controlOutputs(float* out, int max) const override {
    int k = std::min(kEnvArts, max);
    for (int i = 0; i < k; ++i) out[i] = static_cast<float>(art_[i].out);
    return k;
  }

 protected:
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    if (held_ < kHeld) heldNotes_[held_++] = {id, pitch, velocity};
    for (int a = 0; a < kEnvArts; ++a)
      if (inRange(a, pitch)) trigger(a, velocity);
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < held_; ++i)
      if (heldNotes_[i].id == id) {
        double p = heldNotes_[i].pitch;
        heldNotes_[i] = heldNotes_[--held_];
        for (int a = 0; a < kEnvArts; ++a)
          if (inRange(a, p) && !anyHeld(a)) release(a);
        return;
      }
  }
  void onAllOff() override {
    held_ = 0;
    for (int a = 0; a < kEnvArts; ++a) release(a);
  }
  void onKill() override {
    held_ = 0;
    for (auto& a : art_) {
      a = Art{};
    }
  }
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    st_ = &s;
    ctx_ = ctx;
    continuous_ = prm(s, EcContinuous) >= 0.5f;
  }
  void render(float*, float*, int n) override {
    const PluginState& s = *st_;
    double sr = ctx_.sampleRate;
    double spt = sr * 60.0 / (ctx_.tempo * std::max(1, ctx_.ppq));
    for (int a = 0; a < kEnvArts; ++a) {
      Art& r = art_[a];
      int np = std::clamp(static_cast<int>(std::lround(prm(s, envParam(a, EaNumPoints), 4))), 2, kEnvPoints);
      int sus = static_cast<int>(std::lround(prm(s, envParam(a, EaSustain), 2)));  // 0 = none, k = point k
      if (sus > np) sus = np;
      bool loop = prm(s, envParam(a, EaLoop)) >= 0.5f;
      double base = prm(s, envParam(a, EaBase)), envAmt = prm(s, envParam(a, EaEnvAmt), 1);
      double lfoAmt = prm(s, envParam(a, EaLfoAmt));
      int mode = static_cast<int>(prm(s, envParam(a, EaMode)) + 0.5f);
      int shape = static_cast<int>(prm(s, envParam(a, EaLfoShape)) + 0.5f);
      bool sync = prm(s, envParam(a, EaLfoSync), 1) >= 0.5f;
      double beats = rateBeats(prm(s, envParam(a, EaLfoSyncRate), 3));
      double hz = sync ? ctx_.tempo / 60.0 / beats : prm(s, envParam(a, EaLfoRate), 2);
      double delay = prm(s, envParam(a, EaLfoDelay));
      for (int i = 0; i < n; ++i) {
        // envelope: stage k moves from `from` to point k over its time
        if (r.stage >= 0 && r.stage < np) {
          bool holding = r.gate && sus > 0 && r.stage == sus;  // past the sustain point: wait for release
          if (!holding) {
            double t = std::max(1e-4, static_cast<double>(prm(s, envPointTime(a, r.stage), 0.1f)));
            double lvl = prm(s, envPointLevel(a, r.stage));
            r.pos += 1.0 / (t * sr);
            if (r.pos >= 1.0) {
              r.level = lvl;
              r.from = lvl;
              r.pos = 0;
              ++r.stage;
              if (r.gate && sus > 0 && r.stage == sus && loop) r.stage = 0;  // loop up to the sustain point
            } else {
              r.level = r.from + (lvl - r.from) * r.pos;
            }
          }
        }
        // LFO
        bool lfoOn = continuous_ || r.gate || (r.stage >= 0 && r.stage < np);
        double lfoBi = 0;
        if (lfoOn && lfoAmt != 0) {
          double phase;
          if (sync && ctx_.playing && r.stage < 0) {
            phase = (ctx_.tick + i / spt) / std::max(1, ctx_.ppq) / beats;
          } else {
            r.phase += hz / sr;
            phase = r.phase;
          }
          double fade = delay > 0 ? std::min(1.0, r.sinceTrigger / (delay * sr)) : 1.0;
          lfoBi = (lfoWave(shape, phase, static_cast<uint64_t>(std::floor(phase))) * 2.0 - 1.0) * fade;
        }
        r.sinceTrigger += 1.0;
        double env = r.level * r.velScale;
        double out = mode == 0 ? base + envAmt * env + lfoAmt * lfoBi * 0.5
                               : base + envAmt * env * (1.0 - std::fabs(lfoAmt) * 0.5 * (1.0 - lfoBi));
        r.out = clamp01(out);
      }
    }
  }

 private:
  static constexpr int kHeld = 64;
  struct Held {
    uint32_t id;
    double pitch;
    float vel;
  };
  struct Art {
    int stage = -1;   // -1 idle
    double pos = 0, from = 0, level = 0, phase = 0, sinceTrigger = 0, velScale = 1, out = 0;
    bool gate = false;
  };
  bool inRange(int a, double pitch) const {
    if (!st_) return true;
    double lo = prm(*st_, envParam(a, EaKeyLow)), hi = prm(*st_, envParam(a, EaKeyHigh), 127);
    return pitch >= lo - 0.5 && pitch <= hi + 0.5;
  }
  bool anyHeld(int a) const {
    for (int i = 0; i < held_; ++i)
      if (inRange(a, heldNotes_[i].pitch)) return true;
    return false;
  }
  void trigger(int a, float vel) {
    Art& r = art_[a];
    bool retrig = !st_ || prm(*st_, envParam(a, EaRetrig), 1) >= 0.5f;
    if (!retrig && r.gate) return;  // legato: keep going
    double velAmt = st_ ? prm(*st_, envParam(a, EaVelAmt)) : 0;
    r.velScale = 1.0 - velAmt * (1.0 - vel);
    r.stage = 0;
    r.pos = 0;
    r.from = retrig ? 0.0 : r.level;
    if (retrig) r.level = 0;
    r.phase = 0;
    r.sinceTrigger = 0;
    r.gate = true;
  }
  void release(int a) {
    Art& r = art_[a];
    if (!r.gate) return;
    r.gate = false;
    if (!st_) return;
    int np = std::clamp(static_cast<int>(std::lround(prm(*st_, envParam(a, EaNumPoints), 4))), 2, kEnvPoints);
    int sus = static_cast<int>(std::lround(prm(*st_, envParam(a, EaSustain), 2)));
    if (sus > 0 && r.stage >= 0 && r.stage <= std::min(sus, np - 1)) {  // jump to the release part
      r.from = r.level;
      r.pos = 0;
      r.stage = std::min(sus, np);
    }
  }
  const PluginState* st_ = nullptr;
  ProcessContext ctx_;
  bool continuous_ = false;
  Held heldNotes_[kHeld] = {};
  int held_ = 0;
  Art art_[kEnvArts];
};

// ------------------------------------------------------ Keyboard Controller
struct KeyAux final : AuxData {
  float values[128];
};

class KeyboardController final : public Generator {
 public:
  bool active() const override { return false; }
  int controlOutputs(float* out, int max) const override {
    float v[4] = {static_cast<float>(value_), velocity_, gate_ ? 1.0f : 0.0f, static_cast<float>(note_ / 127.0)};
    int k = std::min(4, max);
    for (int i = 0; i < k; ++i) out[i] = v[i];
    return k;
  }

 protected:
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    if (held_ < kHeld) held_ids_[held_] = id, held_pitch_[held_++] = pitch;
    note_ = std::clamp(pitch, 0.0, 127.0);
    velocity_ = velocity;
    gate_ = true;
    target_ = keyValue(pitch);
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < held_; ++i)
      if (held_ids_[i] == id) {
        held_ids_[i] = held_ids_[--held_];
        held_pitch_[i] = held_pitch_[held_];
        break;
      }
    if (held_ > 0) {  // fall back to the most recent key still held
      note_ = held_pitch_[held_ - 1];
      target_ = keyValue(note_);
    } else {
      gate_ = false;
      if (st_ && prm(*st_, KcNoteOff) >= 0.5f) target_ = prm(*st_, KcNoteOffValue);
    }
  }
  void onAllOff() override {
    held_ = 0;
    gate_ = false;
    if (st_ && prm(*st_, KcNoteOff) >= 0.5f) target_ = prm(*st_, KcNoteOffValue);
  }
  void onKill() override { onAllOff(); }
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    st_ = &s;
    keys_ = dynamic_cast<const KeyAux*>(aux.custom.get());
    atk_ = coefFor(prm(s, KcAttack, 0.01f), ctx.sampleRate);
    rel_ = coefFor(prm(s, KcRelease, 0.05f), ctx.sampleRate);
  }
  void render(float*, float*, int n) override {
    for (int i = 0; i < n; ++i) value_ += (target_ - value_) * (target_ > value_ ? atk_ : rel_);
  }

 private:
  double keyValue(double pitch) const {
    int k = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    return keys_ ? keys_->values[k] : k / 127.0;
  }
  static constexpr int kHeld = 64;
  uint32_t held_ids_[kHeld] = {};
  double held_pitch_[kHeld] = {};
  int held_ = 0;
  const PluginState* st_ = nullptr;
  const KeyAux* keys_ = nullptr;
  double atk_ = 1, rel_ = 1, value_ = 0, target_ = 0, note_ = 60;
  float velocity_ = 0;
  bool gate_ = false;
};

// ---------------------------------------------------------------- Voltage Out
class VoltageOut final : public Generator {
 public:
  bool active() const override { return enabled_; }
  int controlOutputs(float* out, int max) const override {
    float v[3] = {static_cast<float>(std::clamp(pitch_ / 127.0, 0.0, 1.0)), held_ > 0 ? 1.0f : 0.0f, velocity_};
    int k = std::min(3, max);
    for (int i = 0; i < k; ++i) out[i] = v[i];
    return k;
  }

 protected:
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    if (held_ < kHeld) held_ids_[held_] = id, held_pitch_[held_++] = pitch;
    targetPitch_ = pitch;
    if (!hasPitch_) pitch_ = pitch;
    hasPitch_ = true;
    velocity_ = velocity;
    trigLeft_ = pulseSamples_;
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < held_; ++i)
      if (held_ids_[i] == id) {
        held_ids_[i] = held_ids_[--held_];
        held_pitch_[i] = held_pitch_[held_];
        break;
      }
    if (held_ > 0) targetPitch_ = held_pitch_[held_ - 1];
  }
  void onAllOff() override { held_ = 0; }
  void onKill() override {
    held_ = 0;
    trigLeft_ = clockLeft_ = 0;
  }
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    st_ = &s;
    ctx_ = ctx;
    enabled_ = prm(s, CvEnable) >= 0.5f;
    pulseSamples_ = std::max(1, static_cast<int>(prm(s, CvPulse, 0.01f) * ctx.sampleRate));
    double glide = prm(s, CvGlide);
    glideCoef_ = glide > 0 ? coefFor(glide, ctx.sampleRate) : 1.0;
    pos_ = 0;
  }
  void render(float* L, float* R, int n) override {
    static const int kPpq[] = {2, 4, 8, 12, 16, 24, 32, 48, 64, 96};
    const PluginState& s = *st_;
    int range = static_cast<int>(prm(s, CvRange) + 0.5f);
    double vMax = range == 0 ? 5.0 : 10.0, vMin = range == 2 ? 0.0 : -vMax;
    double pitchScale = prm(s, CvPitchScale) >= 0.5f ? 1.2 : 1.0;
    int ppqIdx = std::clamp(static_cast<int>(prm(s, CvClockPpq, 5) + 0.5f), 0, 9);
    double spt = ctx_.sampleRate * 60.0 / (ctx_.tempo * std::max(1, ctx_.ppq));
    double clockTicks = static_cast<double>(std::max(1, ctx_.ppq)) / kPpq[ppqIdx];
    for (int i = 0; i < n; ++i, ++pos_) {
      pitch_ += (targetPitch_ - pitch_) * glideCoef_;
      if (ctx_.playing) {  // clock pulse at each clock tick of the song position
        double t0 = ctx_.tick + (pos_ - 1) / spt, t1 = ctx_.tick + pos_ / spt;
        if (pos_ == 0 || std::floor(t1 / clockTicks) != std::floor(t0 / clockTicks)) {
          if (pos_ != 0 || std::fmod(t1, clockTicks) < 1.0 / spt) clockLeft_ = pulseSamples_;
        }
      }
      double gateV = held_ > 0 ? vMax : 0.0;
      double v[2];
      for (int side = 0; side < 2; ++side) {
        int src = static_cast<int>(prm(s, side ? CvRightSrc : CvLeftSrc) + 0.5f);
        double volts = 0;
        switch (src) {
          case CvsPitch: volts = (pitch_ - 60.0) / 12.0 * pitchScale; break;
          case CvsGate: volts = gateV; break;
          case CvsVelocity: volts = held_ > 0 ? velocity_ * vMax : 0.0; break;
          case CvsClock: volts = clockLeft_ > 0 ? vMax : 0.0; break;
          case CvsTrigger: volts = trigLeft_ > 0 ? vMax : 0.0; break;
          default: volts = 0; break;
        }
        double ofs = prm(s, side ? CvRightOfs : CvLeftOfs);
        bool bip = prm(s, side ? CvRightBipolar : CvLeftBipolar) >= 0.5f;
        double offV = bip ? ofs * vMax : (ofs * 0.5 + 0.5) * vMax * (ofs != 0 ? 1.0 : 0.0);
        double scale = prm(s, side ? CvRightScale : CvLeftScale, 1);
        volts = std::clamp(volts * scale + offV, vMin, vMax);
        v[side] = volts / 10.0;  // full scale = 10 V
      }
      if (trigLeft_ > 0) --trigLeft_;
      if (clockLeft_ > 0) --clockLeft_;
      if (enabled_) {
        L[i] += static_cast<float>(v[0]);
        R[i] += static_cast<float>(v[1]);
      }
    }
  }

 private:
  static constexpr int kHeld = 64;
  uint32_t held_ids_[kHeld] = {};
  double held_pitch_[kHeld] = {};
  int held_ = 0;
  const PluginState* st_ = nullptr;
  ProcessContext ctx_;
  bool enabled_ = false, hasPitch_ = false;
  double pitch_ = 60, targetPitch_ = 60, glideCoef_ = 1;
  float velocity_ = 0;
  int pulseSamples_ = 441, trigLeft_ = 0, clockLeft_ = 0;
  int64_t pos_ = 0;
};

// ------------------------------------------------------------ Control Surface
class ControlSurface final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float*, float*, int n) override {
    double c = coefFor(prm(s, SfSmooth, 0.02f), ctx.sampleRate);
    for (int i = 0; i < kSurfaceControls; ++i) {
      double t = prm(s, i);
      if (!primed_) cur_[i] = t;
      for (int j = 0; j < n; ++j) cur_[i] += (t - cur_[i]) * c;
    }
    primed_ = true;
  }
  void reset() override { primed_ = false; }
  int controlOutputs(float* out, int max) const override {
    int k = std::min(kSurfaceControls, max);
    for (int i = 0; i < k; ++i) out[i] = static_cast<float>(clamp01(cur_[i]));
    return k;
  }

 private:
  bool primed_ = false;
  double cur_[kSurfaceControls] = {};
};

// --------------------------------------------------------- sends and buses
class MixerSend final : public Effect {
 public:
  void setSideIo(const SideIo& io) override { io_ = io; }
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    double pl = 1, pr = 1;
    dsp::panGains(prm(s, SnPan), pl, pr);  // balance law: centre = unity
    float lvl = prm(s, SnLevel, 1), dry = prm(s, SnDry, 1);
    float sl = static_cast<float>(pl) * lvl, sr = static_cast<float>(pr) * lvl;
    if (io_.sendL && io_.sendR)
      for (int i = 0; i < n; ++i) {
        io_.sendL[i] += L[i] * sl;
        io_.sendR[i] += R[i] * sr;
      }
    for (int i = 0; i < n; ++i) {
      L[i] *= dry;
      R[i] *= dry;
    }
  }
  void reset() override {}

 private:
  SideIo io_;
};

class BusSendFx final : public Effect {
 public:
  void setSideIo(const SideIo& io) override { io_ = io; }
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    float lvl = prm(s, BsLevel, 1), dry = prm(s, BsDry, 1);
    for (int i = 0; i < n; ++i) {
      if (io_.sendL) {
        io_.sendL[i] += L[i] * lvl;
        io_.sendR[i] += R[i] * lvl;
      }
      L[i] *= dry;
      R[i] *= dry;
    }
  }
  void reset() override {}

 private:
  SideIo io_;
};

class BusReturnFx final : public Effect {
 public:
  void setSideIo(const SideIo& io) override { io_ = io; }
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    float lvl = prm(s, BrLevel, 1), dry = prm(s, BrDry, 1);
    for (int i = 0; i < n; ++i) {
      float rl = io_.recvL ? io_.recvL[i] : 0.f, rr = io_.recvR ? io_.recvR[i] : 0.f;
      L[i] = L[i] * dry + rl * lvl;
      R[i] = R[i] * dry + rr * lvl;
    }
  }
  void reset() override {}

 private:
  SideIo io_;
};

// --------------------------------------------------------- Sidechain Ducker
class SidechainDucker final : public Effect {
 public:
  void setSideIo(const SideIo& io) override { io_ = io; }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    double sr = ctx.sampleRate;
    double atk = coefFor(prm(s, SdAttack, 0.003f), sr), rel = coefFor(prm(s, SdRelease, 0.15f), sr);
    double det = coefFor(0.01, sr);
    double thr = prm(s, SdThreshold, -24), ratio = std::max(1.0f, prm(s, SdRatio, 4)), range = prm(s, SdDepth, 60);
    bool own = prm(s, SdSource) >= 0.5f;
    bool listen = prm(s, SdListen) >= 0.5f;
    double maxGr = 0;
    for (int i = 0; i < n; ++i) {
      float kl = own ? L[i] : (io_.scL ? io_.scL[i] : 0.f);
      float kr = own ? R[i] : (io_.scR ? io_.scR[i] : 0.f);
      double x = std::max(std::fabs(kl), std::fabs(kr));
      env_ = x > env_ ? x : env_ + (x - env_) * det;  // fast peak detector; timing comes from the gain smoothing
      double lev = dsp::gainToDb(env_);
      double over = lev - thr;
      double gr = over > 0 ? std::min(range, over * (1.0 - 1.0 / ratio)) : 0.0;
      // Smooth the gain itself so steps in the key do not click.
      grSm_ += (gr - grSm_) * (gr > grSm_ ? atk : rel);
      double g = dsp::dbToGain(-grSm_);
      maxGr = std::max(maxGr, grSm_);
      if (listen) {
        L[i] = kl;
        R[i] = kr;
      } else {
        L[i] = static_cast<float>(L[i] * g);
        R[i] = static_cast<float>(R[i] * g);
      }
    }
    meter_ = static_cast<float>(-maxGr);
    input_ = static_cast<float>(dsp::gainToDb(env_));
  }
  void reset() override { env_ = grSm_ = 0; }
  float meter() const override { return meter_; }
  float inputLevel() const override { return input_; }

 private:
  SideIo io_;
  double env_ = 0, grSm_ = 0;
  float meter_ = 0, input_ = -120;
};

}  // namespace

// Pack-level helpers for the formula controller's prepared data and the
// keyboard controller's key table (also used by the editors).
bool compileFormula(const std::string& text, Expr& out, std::string* err) {
  static const std::vector<std::string> vars = {"a", "b", "c", "t", "beat", "bar", "tempo", "in", "playing"};
  return out.compile(text.empty() ? kDefaultFormula : text, vars, err);
}

std::vector<float> parseKeyValues(const std::string& data) {
  std::vector<float> v(128);
  for (int k = 0; k < 128; ++k) v[static_cast<size_t>(k)] = static_cast<float>(k / 127.0);
  const char* p = data.c_str();
  for (int k = 0; k < 128 && *p; ++k) {
    char* end = nullptr;
    double x = std::strtod(p, &end);
    if (end == p) break;
    v[static_cast<size_t>(k)] = static_cast<float>(clamp01(x));
    p = end;
    while (*p == ',' || *p == ' ' || *p == '\n') ++p;
  }
  return v;
}

std::string formatKeyValues(const std::vector<float>& v) {
  std::string s;
  char buf[32];
  for (size_t k = 0; k < v.size() && k < 128; ++k) {
    std::snprintf(buf, sizeof buf, k ? ",%.4g" : "%.4g", static_cast<double>(v[k]));
    s += buf;
  }
  return s;
}

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == "gl.envctl") return std::make_unique<EnvelopeController>();
  if (type == "gl.keyctl") return std::make_unique<KeyboardController>();
  if (type == "gl.cvctl") return std::make_unique<VoltageOut>();
  if (type == "gl.patcher") return createPatchHostGenerator();
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == "gl.peakctl") return std::make_unique<PeakFollower>();
  if (type == "gl.xyctl") return std::make_unique<PadController>(2);
  if (type == "gl.xyzctl") return std::make_unique<PadController>(3);
  if (type == "gl.formulactl") return std::make_unique<FormulaController>();
  if (type == "gl.surface") return std::make_unique<ControlSurface>();
  if (type == "gl.send") return std::make_unique<MixerSend>();
  if (type == "gl.bussend") return std::make_unique<BusSendFx>();
  if (type == "gl.busreturn") return std::make_unique<BusReturnFx>();
  if (type == "gl.scduck") return std::make_unique<SidechainDucker>();
  if (type == "gl.patcherfx") return createPatchHostEffect();
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.formulactl") {
    auto fa = std::make_shared<FormulaAux>();
    if (!compileFormula(state.data, fa->expr, &fa->error)) compileFormula(kDefaultFormula, fa->expr, nullptr);
    aux.custom = fa;
    return true;
  }
  if (state.type == "gl.keyctl") {
    auto ka = std::make_shared<KeyAux>();
    std::vector<float> v = parseKeyValues(state.data);
    std::copy(v.begin(), v.end(), ka->values);
    aux.custom = ka;
    return true;
  }
  if (state.type == "gl.patcher" || state.type == "gl.patcherfx") {
    preparePatchHost(state, aux);
    return true;
  }
  return false;
}

}  // namespace gl::packs::modulation

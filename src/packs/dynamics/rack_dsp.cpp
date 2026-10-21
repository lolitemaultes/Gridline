// Gridline — "dynamics" pack: Amp Rack (gl.amprack) and Multi FX
// (gl.effector) runtimes.
#include <atomic>
#include <cstdio>
#include "dyn_dsp.h"
#include "effector_ref.h"
#include "modrack_ref.h"
#include "pedal_ref.h"

namespace gl::packs::dynamics {

// ===================================================================== rack tables

namespace {

struct KnobDef {
  const char* label;
  double def;
};
struct ModuleDef {
  const char* name;
  KnobDef k[kRackKnobs];
};

const ModuleDef kModules[RmCount] = {
    {"Empty", {{nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}}},
    {"Overdrive", {{"Drive", 0.5}, {"Tone", 0.55}, {"Body", 0.5}, {"Bias", 0.5}, {nullptr, 0}, {"Level", 0.7}}},
    {"Fuzz", {{"Fuzz", 0.6}, {"Tone", 0.5}, {"Bias", 0.3}, {"Octave", 0.0}, {"Gate", 0.1}, {"Level", 0.6}}},
    {"Amp + Cab", {{"Gain", 0.5}, {"Bass", 0.5}, {"Mid", 0.5}, {"Treble", 0.5}, {"Cabinet", 0.25}, {"Level", 0.7}}},
    {"Compressor", {{"Thresh", 0.6}, {"Ratio", 0.3}, {"Attack", 0.3}, {"Release", 0.4}, {"Makeup", 0.2}, {nullptr, 0}}},
    {"Graphic EQ", {{"100", 0.5}, {"300", 0.5}, {"800", 0.5}, {"2k", 0.5}, {"5k", 0.5}, {"10k", 0.5}}},
    {"Wah", {{"Pedal", 0.5}, {"Auto", 0.0}, {"LFO", 0.0}, {"Q", 0.6}, {"Range", 0.5}, {"Level", 0.7}}},
    {"Chorus", {{"Rate", 0.3}, {"Depth", 0.5}, {"Delay", 0.4}, {"Feedback", 0.5}, {"Tone", 0.7}, {"Width", 0.8}}},
    {"Phaser", {{"Rate", 0.3}, {"Depth", 0.7}, {"Feedback", 0.4}, {"Stages", 0.4}, {"Center", 0.5}, {"Spread", 0.5}}},
    {"Delay", {{"Time", 0.45}, {"Feedback", 0.35}, {"Tone", 0.6}, {"Sync", 0.0}, {"Ping-pong", 0.0}, {nullptr, 0}}},
    {"Reverb", {{"Size", 0.5}, {"Decay", 0.4}, {"Damping", 0.5}, {"Pre-delay", 0.1}, {"Width", 1.0}, {nullptr, 0}}},
    {"Noise Gate", {{"Thresh", 0.35}, {"Range", 1.0}, {"Attack", 0.2}, {"Hold", 0.3}, {"Release", 0.4}, {nullptr, 0}}},
    {"Tremolo", {{"Rate", 0.4}, {"Depth", 0.6}, {"Shape", 0.0}, {"Sync", 0.0}, {"Stereo", 0.0}, {nullptr, 0}}},
};

const char* kCabNames[5] = {"Direct", "Open 1x12", "Closed 2x12", "Stack 4x12", "Small box"};
const char* kSyncNames[7] = {"Free", "1/16", "1/8", "1/8 dot", "1/4", "1/4 dot", "1/2"};
const double kSyncBeats[7] = {0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};

inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double expMap(double lo, double hi, double t) { return lo * std::pow(hi / lo, clampd(t, 0, 1)); }
inline int stepIndex(double v, int count) { return std::clamp(static_cast<int>(v * count), 0, count - 1); }

}  // namespace

const char* rackModuleName(int type) { return type >= 0 && type < RmCount ? kModules[type].name : "?"; }
const char* rackKnobLabel(int type, int k) {
  if (type < 0 || type >= RmCount || k < 0 || k >= kRackKnobs) return nullptr;
  return kModules[type].k[k].label;
}
double rackKnobDefault(int type, int k) {
  if (type < 0 || type >= RmCount || k < 0 || k >= kRackKnobs) return 0.5;
  return kModules[type].k[k].def;
}

void rackKnobText(int type, int k, double v, char* buf, int size) {
  v = clampd(v, 0, 1);
  auto pct = [&]() { std::snprintf(buf, static_cast<size_t>(size), "%.0f%%", v * 100); };
  auto db = [&](double d) { std::snprintf(buf, static_cast<size_t>(size), "%+.1f dB", d); };
  auto hz = [&](double h) {
    if (h >= 1000)
      std::snprintf(buf, static_cast<size_t>(size), "%.1f kHz", h / 1000);
    else
      std::snprintf(buf, static_cast<size_t>(size), "%.0f Hz", h);
  };
  auto ms = [&](double m) {
    if (m >= 1000)
      std::snprintf(buf, static_cast<size_t>(size), "%.2f s", m / 1000);
    else
      std::snprintf(buf, static_cast<size_t>(size), "%.1f ms", m);
  };
  switch (type) {
    case RmOverdrive:
      if (k == 0) return db(v * 40);
      if (k == 1) return hz(expMap(800, 12000, v));
      if (k == 2) return db((v - 0.5) * 18);
      if (k == 5) return db(v * 30 - 24);
      return pct();
    case RmFuzz:
      if (k == 0) return db(10 + v * 50);
      if (k == 1) return hz(expMap(600, 9000, v));
      if (k == 4) return db(-80 + v * 50);
      if (k == 5) return db(v * 30 - 24);
      return pct();
    case RmAmp:
      if (k == 0) return db(v * 45);
      if (k >= 1 && k <= 3) return db((v - 0.5) * 24);
      if (k == 4) {
        std::snprintf(buf, static_cast<size_t>(size), "%s", kCabNames[stepIndex(v, 5)]);
        return;
      }
      return db(v * 30 - 24);
    case RmCompressor:
      if (k == 0) return db(-48 + v * 48);
      if (k == 1) {
        std::snprintf(buf, static_cast<size_t>(size), "%.1f:1", 1 + v * v * 19);
        return;
      }
      if (k == 2) return ms(expMap(0.1, 100, v));
      if (k == 3) return ms(expMap(10, 1500, v));
      if (k == 4) return db(v * 24);
      return pct();
    case RmEq: return db((v - 0.5) * 24);
    case RmWah:
      if (k == 2) {
        if (v < 0.01) {
          std::snprintf(buf, static_cast<size_t>(size), "Off");
          return;
        }
        std::snprintf(buf, static_cast<size_t>(size), "%.2f Hz", expMap(0.1, 8, v));
        return;
      }
      if (k == 5) return db(v * 30 - 24);
      return pct();
    case RmChorus:
      if (k == 0) {
        std::snprintf(buf, static_cast<size_t>(size), "%.2f Hz", expMap(0.05, 5, v));
        return;
      }
      if (k == 2) return ms(lerp(2, 25, v));
      if (k == 3) {
        std::snprintf(buf, static_cast<size_t>(size), "%+.0f%%", (v - 0.5) * 180);
        return;
      }
      if (k == 4) return hz(expMap(1500, 18000, v));
      return pct();
    case RmPhaser:
      if (k == 0) {
        std::snprintf(buf, static_cast<size_t>(size), "%.2f Hz", expMap(0.03, 6, v));
        return;
      }
      if (k == 3) {
        std::snprintf(buf, static_cast<size_t>(size), "%d", 2 + 2 * stepIndex(v, 6));
        return;
      }
      if (k == 4) return hz(expMap(150, 5000, v));
      return pct();
    case RmDelay:
      if (k == 0) return ms(expMap(20, 1000, v));
      if (k == 2) return hz(expMap(1000, 18000, v));
      if (k == 3) {
        std::snprintf(buf, static_cast<size_t>(size), "%s", kSyncNames[stepIndex(v, 7)]);
        return;
      }
      return pct();
    case RmReverb:
      if (k == 1) return ms(expMap(200, 10000, v));
      if (k == 3) return ms(v * 100);
      return pct();
    case RmGate:
      if (k == 0) return db(-80 + v * 70);
      if (k == 1) return db(-v * 80);
      if (k == 2) return ms(expMap(0.1, 20, v));
      if (k == 3) return ms(v * 300);
      if (k == 4) return ms(expMap(5, 1000, v));
      return pct();
    case RmTremolo:
      if (k == 0) {
        std::snprintf(buf, static_cast<size_t>(size), "%.2f Hz", expMap(0.5, 15, v));
        return;
      }
      if (k == 3) {
        std::snprintf(buf, static_cast<size_t>(size), "%s", kSyncNames[stepIndex(v, 7)]);
        return;
      }
      if (k == 4) {
        std::snprintf(buf, static_cast<size_t>(size), "%.0f°", v * 180);
        return;
      }
      return pct();
    default: return pct();
  }
}

// ===================================================================== rack slot

// Every module type's state lives in the slot, so switching or reordering
// types never allocates on the audio thread.
class RackSlot {
 public:
  RackSlot() : delay_{DelayLine(65536), DelayLine(65536)}, mod_{DelayLine(4096), DelayLine(4096)}, rev_(8192) {}

  void reset() {
    for (int c = 0; c < 2; ++c) {
      for (auto& f : f_[c]) f.reset();
      for (auto& b : eq_[c]) b.clear();
      os_[c].reset();
      delay_[c].reset();
      mod_[c].reset();
      ph_[c].reset();
      dc_[c].reset();
      fbState_[c] = 0;
    }
    rev_.reset();
    env_.v = 0;
    gateGain_ = 1;
    holdLeft_ = 0;
    compGain_ = 0;
    lfo_ = 0;
  }

  // Per-block setup (coefficients that do not move per sample).
  void configure(int type, const double* k, double sr, double tempo) {
    if (type != type_) {
      reset();
      type_ = type;
      lastSr_ = 0;
    }
    bool same = sr == lastSr_;
    for (int i = 0; i < kRackKnobs; ++i) same = same && k[i] == lastK_[i];
    sr_ = sr;
    tempo_ = tempo;
    for (int i = 0; i < kRackKnobs; ++i) k_[i] = k[i];
    if (same) return;
    lastSr_ = sr;
    for (int i = 0; i < kRackKnobs; ++i) lastK_[i] = k[i];
    for (int c = 0; c < 2; ++c) dc_[c].set(sr);
    switch (type) {
      case RmOverdrive:
        for (int c = 0; c < 2; ++c) {
          eq_[c][0].set(dsp::Biquad::LowShelf, sr, 250, 0.7, (k[2] - 0.5) * 18);
          eq_[c][1].set(dsp::Biquad::HighPass, sr, 90, 0.7, 0);
          f_[c][0].set(sr, expMap(800, 12000, k[1]), 0.7071);
        }
        break;
      case RmFuzz:
        for (int c = 0; c < 2; ++c) f_[c][0].set(sr, expMap(600, 9000, k[1]), 0.8);
        break;
      case RmAmp: {
        static const double cab[5][5] = {
            // hp, lp, peak Hz, peak dB, notch Hz
            {20, 20000, 1000, 0, 0},
            {90, 5200, 2200, 4, 4500},
            {80, 4300, 1600, 5, 3600},
            {70, 3800, 1200, 6, 2900},
            {150, 4800, 2600, 7, 5200},
        };
        int ci = stepIndex(k[4], 5);
        for (int c = 0; c < 2; ++c) {
          eq_[c][0].set(dsp::Biquad::LowShelf, sr, 120, 0.7, (k[1] - 0.5) * 24);
          eq_[c][1].set(dsp::Biquad::Peak, sr, 700, 0.8, (k[2] - 0.5) * 24);
          eq_[c][2].set(dsp::Biquad::HighShelf, sr, 3000, 0.7, (k[3] - 0.5) * 24);
          eq_[c][3].set(dsp::Biquad::HighPass, sr, cab[ci][0], 0.7, 0);
          eq_[c][4].set(dsp::Biquad::LowPass, sr, cab[ci][1], ci ? 0.9 : 0.7071, 0);
          eq_[c][5].set(dsp::Biquad::Peak, sr, cab[ci][2], 1.2, cab[ci][3]);
          f_[c][0].set(sr, 120, 0.7071);  // inter-stage high-pass
          if (ci) eq_[c][6].set(dsp::Biquad::Notch, sr, cab[ci][4], 3.0, 0);
        }
        cab_ = ci;
        break;
      }
      case RmEq: {
        static const double hz[6] = {100, 300, 800, 2000, 5000, 10000};
        for (int c = 0; c < 2; ++c)
          for (int b = 0; b < 6; ++b) eq_[c][b].set(dsp::Biquad::Peak, sr, hz[b], 1.1, (k[b] - 0.5) * 24);
        break;
      }
      case RmChorus:
        for (int c = 0; c < 2; ++c) f_[c][0].set(sr, expMap(1500, 18000, k[4]), 0.7071);
        break;
      case RmDelay:
        for (int c = 0; c < 2; ++c) f_[c][0].set(sr, expMap(1000, 18000, k[2]), 0.7071);
        break;
      case RmReverb: rev_.set(sr, k[0], expMap(0.2, 10, k[1]), k[2] * 0.85, k[3] * 100); break;
      default: break;
    }
  }

  void process(double& l, double& r, int osf) {
    const double* k = k_;
    const double sr = sr_;
    switch (type_) {
      case RmOverdrive: {
        double drive = dbToLin(k[0] * 40), bias = k[3], lvl = dbToLin(k[5] * 30 - 24);
        double* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c) {
          double x = eq_[c][1].process(eq_[c][0].process(*io[c])) * drive;
          x = os_[c].process(x, osf, [&](double u) { return overdriveCurve(u, bias); });
          x = dc_[c].process(x);
          f_[c][0].tick(x);
          *io[c] = f_[c][0].lp * lvl;
        }
        break;
      }
      case RmFuzz: {
        double drive = dbToLin(10 + k[0] * 50), bias = k[2] * 0.6, oct = k[3];
        double gateTh = dbToLin(-80 + k[4] * 50), lvl = dbToLin(k[5] * 30 - 24);
        double a = std::max(std::fabs(l), std::fabs(r));
        double e = env_.process(a, timeCoef(0.001, sr), timeCoef(0.08, sr));
        double gTarget = e > gateTh ? 1.0 : 0.0;
        gateGain_ = gTarget + (gateGain_ - gTarget) * timeCoef(0.01, sr);
        double* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c) {
          double x = *io[c];
          x = x + (std::fabs(x) - x) * oct;  // full-wave rectifier blend: octave up
          x *= drive;
          x = os_[c].process(x, osf, [&](double u) {
            double v = u + bias;
            return v / (std::fabs(v) + 0.08) - bias / (std::fabs(bias) + 0.08);
          });
          x = dc_[c].process(x) * 0.6;
          f_[c][0].tick(x);
          *io[c] = f_[c][0].lp * lvl * gateGain_;
        }
        break;
      }
      case RmAmp: {
        double gain = dbToLin(k[0] * 45), lvl = dbToLin(k[5] * 30 - 24);
        double* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c) {
          double x = *io[c] * gain;
          x = os_[c].process(x, osf, [&](double u) {
            double s1 = std::tanh(u * 1.5 + 0.1) - std::tanh(0.1);
            return std::tanh(s1 * 2.2) * 0.8;
          });
          f_[c][0].tick(x);
          x = x - f_[c][0].lp * 0.8;  // tighten lows between stages
          x = dc_[c].process(x);
          for (int b = 0; b < 3; ++b) x = eq_[c][b].process(x);
          if (cab_) {
            for (int b = 3; b < 7; ++b) x = eq_[c][b].process(x);
          }
          *io[c] = x * lvl;
        }
        break;
      }
      case RmCompressor: {
        double th = -48 + k[0] * 48, ratio = 1 + k[1] * k[1] * 19;
        double atk = timeCoef(expMap(0.1, 100, k[2]) * 0.001, sr), rel = timeCoef(expMap(10, 1500, k[3]) * 0.001, sr);
        double a = std::max(std::fabs(l), std::fabs(r));
        double e = env_.process(a, 0.0, timeCoef(0.02, sr));
        double lev = linToDb(std::max(e, 1e-7));
        double target = lev > th ? (th - lev) * (1.0 - 1.0 / ratio) : 0.0;
        compGain_ = target + (compGain_ - target) * (target < compGain_ ? atk : rel);
        double g = dbToLin(compGain_ + k[4] * 24);
        l *= g;
        r *= g;
        break;
      }
      case RmEq: {
        double* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c)
          for (int b = 0; b < 6; ++b) *io[c] = eq_[c][b].process(*io[c]);
        break;
      }
      case RmWah: {
        double a = std::max(std::fabs(l), std::fabs(r));
        double e = env_.process(a, timeCoef(0.004, sr), timeCoef(0.12, sr));
        double pos = k[0];
        if (k[2] >= 0.01) {
          lfo_ += expMap(0.1, 8, k[2]) / sr;
          lfo_ -= std::floor(lfo_);
          pos = clampd(pos + 0.5 * std::sin(kTwoPi * lfo_), 0, 1);
        }
        pos = clampd(pos + k[1] * std::min(1.0, e * 4.0), 0, 1);
        double lo = expMap(250, 600, 1.0 - k[4]), hi = expMap(1500, 3500, k[4]);
        double hz = expMap(lo, hi, pos), q = 1.5 + k[3] * 8;
        double lvl = dbToLin(k[5] * 30 - 24);
        double* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c) {
          f_[c][0].set(sr, hz, q);
          f_[c][0].tick(*io[c]);
          *io[c] = f_[c][0].bp * 2.0 * lvl;
        }
        break;
      }
      case RmChorus: {
        lfo_ += expMap(0.05, 5, k[0]) / sr;
        lfo_ -= std::floor(lfo_);
        double base = lerp(2, 25, k[2]) * 0.001 * sr, depth = k[1] * 0.8 * base;
        double fb = (k[3] - 0.5) * 1.6, width = k[5];
        double* io[2] = {&l, &r};
        double wet[2];
        for (int c = 0; c < 2; ++c) {
          double ph = lfo_ + (c ? 0.25 * width : 0.0);
          double d = base + depth * std::sin(kTwoPi * ph);
          double x = *io[c];
          double y = mod_[c].frac(std::max(1.0, d));
          f_[c][0].tick(y);
          y = f_[c][0].lp;
          mod_[c].push(x + y * fb * 0.9);
          wet[c] = y;
        }
        for (int c = 0; c < 2; ++c) *io[c] = *io[c] * 0.7 + wet[c] * 0.7;
        break;
      }
      case RmPhaser: {
        lfo_ += expMap(0.03, 6, k[0]) / sr;
        lfo_ -= std::floor(lfo_);
        int stages = 2 + 2 * stepIndex(k[3], 6);
        double center = expMap(150, 5000, k[4]);
        double* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c) {
          double ph = lfo_ + (c ? k[5] * 0.5 : 0.0);
          double m = std::sin(kTwoPi * ph) * k[1];
          double hz = center * std::pow(4.0, m);
          double y = ph_[c].process(*io[c], allpassCoef(hz, sr), stages, k[2] * 0.85);
          *io[c] = (*io[c] + y) * 0.5;
        }
        break;
      }
      case RmDelay: {
        double tMs = expMap(20, 1000, k[0]);
        int si = stepIndex(k[3], 7);
        if (si > 0) tMs = kSyncBeats[si] * beatSeconds(tempo_) * 1000.0;
        double d = clampd(tMs * 0.001 * sr, 1, static_cast<double>(delay_[0].capacity() - 4));
        double fb = k[1] * 0.95, pp = k[4];
        double y[2] = {delay_[0].frac(d), delay_[1].frac(d)};
        for (int c = 0; c < 2; ++c) {
          f_[c][0].tick(y[c]);
          y[c] = f_[c][0].lp;
        }
        double in[2] = {l, r};
        double mono = (l + r) * 0.5;
        // ping-pong: input enters the left line only and the lines feed each other
        double inL = lerp(in[0], mono, pp), inR = lerp(in[1], 0.0, pp);
        double fbL = lerp(y[0], y[1], pp), fbR = lerp(y[1], y[0], pp);
        delay_[0].push(inL + fbL * fb);
        delay_[1].push(inR + fbR * fb);
        l = in[0] + y[0] * 0.6;
        r = in[1] + y[1] * 0.6;
        break;
      }
      case RmReverb: {
        double wl, wr;
        rev_.process((l + r) * 0.5, wl, wr);
        double w = k[4];
        double m = (wl + wr) * 0.5;
        wl = m + (wl - m) * w;
        wr = m + (wr - m) * w;
        l += wl * 0.5;
        r += wr * 0.5;
        break;
      }
      case RmGate: {
        double th = dbToLin(-80 + k[0] * 70), floorG = dbToLin(-k[1] * 80);
        double atk = timeCoef(expMap(0.1, 20, k[2]) * 0.001, sr), rel = timeCoef(expMap(5, 1000, k[4]) * 0.001, sr);
        double a = std::max(std::fabs(l), std::fabs(r));
        double e = env_.process(a, 0.0, timeCoef(0.01, sr));
        double target;
        if (e > th) {
          target = 1.0;
          holdLeft_ = k[3] * 0.3 * sr;
        } else if (holdLeft_ > 0) {
          target = 1.0;
          holdLeft_ -= 1.0;
        } else {
          target = floorG;
        }
        gateGain_ = target + (gateGain_ - target) * (target > gateGain_ ? atk : rel);
        l *= gateGain_;
        r *= gateGain_;
        break;
      }
      case RmTremolo: {
        int si = stepIndex(k[3], 7);
        double hz = si > 0 ? 1.0 / (kSyncBeats[si] * beatSeconds(tempo_)) : expMap(0.5, 15, k[0]);
        lfo_ += hz / sr;
        lfo_ -= std::floor(lfo_);
        double* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c) {
          double ph = lfo_ + (c ? k[4] * 0.5 : 0.0);
          double s = std::sin(kTwoPi * ph);
          double sq = std::tanh(s * 8.0);
          double m = lerp(s, sq, k[2]);
          *io[c] *= 1.0 - k[1] * (0.5 - 0.5 * m);
        }
        break;
      }
      default: break;
    }
  }

 private:
  int type_ = -1, cab_ = 0;
  double k_[kRackKnobs] = {}, lastK_[kRackKnobs] = {};
  double sr_ = 44100, lastSr_ = 0, tempo_ = 120;
  SvfTpt f_[2][2];
  dsp::Biquad eq_[2][7];
  Oversampler os_[2];
  DelayLine delay_[2], mod_[2];
  FdnReverb rev_;
  PhaserChain ph_[2];
  DcBlock dc_[2];
  Follower env_;
  double gateGain_ = 1, holdLeft_ = 0, compGain_ = 0, lfo_ = 0;
  double fbState_[2] = {};
};

// ===================================================================== gl.amprack

class AmpRackFx final : public DynFx {
 public:
  AmpRackFx() : DynFx("gl.amprack") {}
  ~AmpRackFx() override {
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete cab_;
  }
  AmpRackFx(const AmpRackFx&) = delete;
  AmpRackFx& operator=(const AmpRackFx&) = delete;
  // J-42: UI thread: convolver states for impulse cabinets (module rack with a cabinet directory).
  void prepareHost(const PluginState& s, double sampleRate) override {
    delete retired_.exchange(nullptr);
    cabRate().store(sampleRate, std::memory_order_relaxed);  // the Program prepares the aux next, at this rate
    if ((pi(s, RkModel) != 1 && pi(s, RkModel) != 2) || cab::dirOf(s.data).empty()) return;
    const int need = cab::partsFor(sampleRate);
    if (need <= capUi_) return;
    capUi_ = need;
    auto* b = new CabBank;
    for (auto& r : b->run) r = std::make_unique<cab::Run>(need);
    delete pending_.exchange(b);
  }
  void processAux(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) override {
    cabRate().store(ctx.sampleRate, std::memory_order_relaxed);
    if (!retired_.load(std::memory_order_acquire))
      if (CabBank* b = pending_.exchange(nullptr)) retired_.store(cab_, std::memory_order_release), cab_ = b;
    const auto* a = dynamic_cast<const cab::Aux*>(aux.custom.get());
    cab::Run* runs[kDrModules] = {};
    if (cab_)
      for (int k = 0; k < kDrModules; ++k) runs[k] = cab_->run[k].get();
    if (a && a->rate != ctx.sampleRate) a = nullptr;  // prepared for another rate (the next build fixes it)
    mods_.setCabinets(a, cab_ ? runs : nullptr);
    const cab::Kernel* bk = a && pi(s, RkModel) == 2 ? a->k[0].get() : nullptr;
    if (bk != board_.cabK && runs[0]) runs[0]->clear();
    board_.cabK = bk, board_.cabRun = runs[0];
    process(s, ctx, L, R, n);
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    const int model = pi(s, RkModel);
    // J-68: the reference models publish input / output peaks too (editor meters).
    auto peakOf = [](const float* l, const float* r, int count) {
      float pk = 0;
      for (int i = 0; i < count; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
      return static_cast<double>(pk);
    };
    if (model == 1 || model == 2) input_ = static_cast<float>(linToDb(peakOf(L, R, n)));
    struct OutPeak {
      float *l, *r;
      int n;
      float* dst;
      bool on;
      ~OutPeak() {
        if (!on) return;
        float pk = 0;
        for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        *dst = static_cast<float>(linToDb(pk));
      }
    } outPeak{L, R, n, &meter_, model == 1 || model == 2};
    if (model == 1) {  // J-35 module rack
      drack::RackState rs;
      rs.count = std::clamp(pi(s, DrCount), 0, kDrModules);
      rs.softClip = pb(s, DrSoftClip);
      for (int m = 0; m < kDrModules; ++m) {
        drack::ModuleState& ms = rs.m[m];
        int* f[DmP0] = {&ms.kind, &ms.algo, &ms.os, &ms.removeDc, &ms.disabled, &ms.ftype, &ms.fopt, &ms.ir, &ms.chorusMode};
        for (int k = 0; k < DmP0; ++k) *f[k] = pi(s, drParam(m, k));
        for (int k = 0; k < kDrParams; ++k) ms.p[k] = pi(s, drParam(m, DmP0 + k));
      }
      mods_.process(rs, sr, L, R, n);
      safety(L, R, n);
      return;
    }
    if (model == 2) {  // J-35 pedal board
      float v[50];
      for (int k = 0; k < 50; ++k) v[k] = static_cast<float>(p(s, PbValue0 + k));
      board_.process(v, sr, L, R, n);
      safety(L, R, n);
      return;
    }
    const double in = dbToLin(p(s, RkInput)), out = dbToLin(p(s, RkOutput));
    const bool soft = pb(s, RkSoftClip);
    const int osf = osFactor(pi(s, RkOversample));
    int type[kRackSlots];
    bool on[kRackSlots];
    double mix[kRackSlots];
    for (int sl = 0; sl < kRackSlots; ++sl) {
      type[sl] = std::clamp(pi(s, rkParam(sl, RsType)), 0, RmCount - 1);
      on[sl] = pb(s, rkParam(sl, RsOn)) && type[sl] != RmEmpty;
      mix[sl] = clampd(p(s, rkParam(sl, RsMix)), 0, 1);
      double k[kRackKnobs];
      for (int j = 0; j < kRackKnobs; ++j) k[j] = clampd(p(s, rkParam(sl, RsK0 + j)), 0, 1);
      slots_[sl].configure(type[sl], k, sr, ctx.tempo);
    }
    double peakIn = 0, peakOut = 0;
    for (int i = 0; i < n; ++i) {
      double l = L[i] * in, r = R[i] * in;
      peakIn = std::max(peakIn, std::max(std::fabs(l), std::fabs(r)));
      for (int sl = 0; sl < kRackSlots; ++sl) {
        if (!on[sl]) continue;
        double dl = l, dr = r;
        slots_[sl].process(l, r, osf);
        l = dl + (l - dl) * mix[sl];
        r = dr + (r - dr) * mix[sl];
      }
      l *= out;
      r *= out;
      if (soft) {
        l = softSat(l, 0.7, 1.0);
        r = softSat(r, 0.7, 1.0);
      }
      l = clampd(l, -16.0, 16.0);
      r = clampd(r, -16.0, 16.0);
      peakOut = std::max(peakOut, std::max(std::fabs(l), std::fabs(r)));
      L[i] = static_cast<float>(l);
      R[i] = static_cast<float>(r);
    }
    input_ = static_cast<float>(linToDb(peakIn));
    meter_ = static_cast<float>(linToDb(peakOut));
  }
  void reset() override {
    for (auto& sl : slots_) sl.reset();
    mods_.reset();
    board_.reset();
  }
  // The drive modules (overdrive, fuzz, amp) oversample their shaper, which
  // delays their wet signal; a slot's undelayed dry part is mixed back in, so
  // count the slots whose wet part dominates (mix >= 50 %).
  int latencyFor(const PluginState& s, double) const override {
    if (pi(s, RkModel) != 0) return 0;  // reference models: no reported latency (IIR resamplers, as the reference)
    const int os = osLatency(osFactor(pi(s, RkOversample)));
    int lat = 0;
    for (int sl = 0; sl < kRackSlots; ++sl) {
      const int type = std::clamp(pi(s, rkParam(sl, RsType)), 0, RmCount - 1);
      const bool on = pb(s, rkParam(sl, RsOn)) && type != RmEmpty;
      const bool drive = type == RmOverdrive || type == RmFuzz || type == RmAmp;
      if (on && drive && clampd(p(s, rkParam(sl, RsMix)), 0, 1) >= 0.5) lat += os;
    }
    return lat;
  }

 private:
  // Output safety of the reference models, as the guitar rack: ±16 (+24 dBFS) —
  // only absurd settings (every gain at its maximum) ever reach it.
  static void safety(float* L, float* R, int n) {
    for (int i = 0; i < n; ++i) L[i] = std::clamp(L[i], -16.f, 16.f), R[i] = std::clamp(R[i], -16.f, 16.f);
  }
  struct CabBank {
    std::unique_ptr<cab::Run> run[kDrModules];
  };
  RackSlot slots_[kRackSlots];
  drack::Rack mods_;
  CabBank* cab_ = nullptr;
  std::atomic<CabBank*> pending_{nullptr}, retired_{nullptr};
  int capUi_ = 0;
  pedal::Board board_;
};

// ===================================================================== effector tables

namespace {
const char* kEfNames[kEfKinds] = {"Distort", "Lo-fi", "Flanger", "Phaser", "Filter", "Delay",
                                  "Reverb",  "Ring",  "Stereo",  "Stutter", "Crush", "Tremolo"};
const char* kEfAxes[kEfKinds][2] = {
    {"Drive", "Tone"},   {"Band", "Wobble"},  {"Delay", "Feedback"}, {"Center", "Feedback"},
    {"Cutoff", "Reso"},  {"Time", "Feedback"}, {"Size", "Damping"},  {"Freq", "Shape"},
    {"Width", "Haas"},   {"Grain", "Pitch"},  {"Bits", "Rate"},     {"Rate", "Depth"},
};
const double kEfRates[8] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};
}  // namespace

const char* effectorKindName(int kind) { return kind >= 0 && kind < kEfKinds ? kEfNames[kind] : "?"; }
const char* effectorAxisLabel(int kind, int axis) {
  if (kind < 0 || kind >= kEfKinds) return "";
  return kEfAxes[kind][axis ? 1 : 0];
}
double effectorRateBeats(int choice) { return kEfRates[std::clamp(choice, 0, 7)]; }
double effectorShape(int shape, double ph, double randA, double randB) {
  switch (shape) {
    case 0: return std::sin(kTwoPi * ph);
    case 1: return 1.0 - 4.0 * std::fabs(ph - 0.5);  // triangle, starts at -1
    case 2: return 2.0 * ph - 1.0;                    // ramp up
    case 3: return ph < 0.5 ? 1.0 : -1.0;
    case 4: return randA;                             // stepped random
    default: {                                        // smooth random
      double t = 0.5 - 0.5 * std::cos(kPi * ph);
      return randA + (randB - randA) * t;
    }
  }
}

// ===================================================================== gl.effector

class EffectorFx final : public DynFx {
 public:
  EffectorFx()
      : DynFx("gl.effector"),
        delay_{DelayLine(262144), DelayLine(262144)},
        mod_{DelayLine(8192), DelayLine(8192)},
        grain_{DelayLine(262144), DelayLine(262144)},
        rev_(16384) {}

  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    if (pb(s, EfModel)) {
      float v[12];
      for (int k = 0; k < 12; ++k) v[k] = static_cast<float>(p(s, EfRawKind + k));
      double peak = 0;
      for (int i = 0; i < n; ++i) peak = std::max(peak, static_cast<double>(std::max(std::fabs(L[i]), std::fabs(R[i]))));
      ref_.process(v, ctx.sampleRate, ctx.tempo, ctx.tick / std::max(1, ctx.ppq), L, R, n);
      curX_ = ref_.curX(), curY_ = ref_.curY();
      input_ = static_cast<float>(linToDb(peak));
      return;
    }
    const double sr = ctx.sampleRate;
    const int kind = std::clamp(pi(s, EfKind), 0, kEfKinds - 1);
    if (kind != kind_) {
      resetState();
      kind_ = kind;
    }
    const double X = p(s, EfX), Y = p(s, EfY), mx = p(s, EfModX), my = p(s, EfModY);
    const int shape = pi(s, EfModShape);
    const double beats = effectorRateBeats(pi(s, EfModRate));
    const double in = dbToLin(p(s, EfInput)), out = dbToLin(p(s, EfOutput)), mix = clampd(p(s, EfMix), 0, 1);
    const bool bypass = pb(s, EfBypass);
    const double spb = beatSeconds(ctx.tempo);
    const double inc = 1.0 / (beats * spb * sr);
    if (ctx.playing) {
      double ph = ctx.tick / std::max(1, ctx.ppq) / beats;
      phase_ = ph - std::floor(ph);
    }
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      double m = effectorShape(shape, phase_, randA_, randB_);
      phase_ += inc;
      if (phase_ >= 1.0) {
        phase_ -= std::floor(phase_);
        randA_ = randB_;
        randB_ = noise_.next();
      }
      // Y follows the same LFO a quarter cycle later so the X/Y point draws a loop.
      double q = phase_ + 0.25;
      q -= std::floor(q);
      double x = clampd(X + mx * 0.5 * m, 0, 1);
      double y = clampd(Y + my * 0.5 * effectorShape(shape, q, randA_, randB_), 0, 1);
      curX_ = x;
      curY_ = y;
      double l = L[i] * in, r = R[i] * in;
      peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
      double dl = l, dr = r;
      if (!bypass) {
        fx(kind, x, y, sr, spb, l, r);
        l = dl + (l - dl) * mix;
        r = dr + (r - dr) * mix;
      }
      L[i] = static_cast<float>(clampd(l * out, -16, 16));
      R[i] = static_cast<float>(clampd(r * out, -16, 16));
    }
    input_ = static_cast<float>(linToDb(peak));
  }
  // The distortion kind runs its shaper at 2x (wet delayed, dry mixed back
  // undelayed): latent while it dominates the mix.
  int latencyFor(const PluginState& s, double) const override {
    if (pb(s, EfModel)) return 0;
    const int kind = std::clamp(pi(s, EfKind), 0, kEfKinds - 1);
    return kind == 0 && !pb(s, EfBypass) && clampd(p(s, EfMix), 0, 1) >= 0.5 ? osLatency(2) : 0;
  }
  // meters(): current modulated X / Y (0..1) for the pad display.
  int meters(float* o, int max) const override {
    if (max < 2) return 0;
    o[0] = static_cast<float>(curX_);
    o[1] = static_cast<float>(curY_);
    return 2;
  }
  void reset() override {
    resetState();
    phase_ = 0;
    ref_.reset();
  }

 private:
  void resetState() {
    for (int c = 0; c < 2; ++c) {
      delay_[c].reset();
      mod_[c].reset();
      grain_[c].reset();
      f_[c].reset();
      f2_[c].reset();
      ph_[c].reset();
      os_[c].reset();
      dc_[c].reset();
      held_[c] = 0;
    }
    rev_.reset();
    hold_ = 0;
    lfo2_ = 0;
    ring_ = 0;
    grainPos_ = writeAbs_ = 0;
    grainStart_[0] = grainStart_[1] = 0;
    grainCount_[0] = grainCount_[1] = 0;
  }

  void fx(int kind, double x, double y, double sr, double spb, double& l, double& r) {
    double* io[2] = {&l, &r};
    switch (kind) {
      case 0: {  // distortion
        double drive = dbToLin(x * 42);
        double tone = expMap(500, 18000, y);
        for (int c = 0; c < 2; ++c) {
          double v = os_[c].process(*io[c] * drive, 2, [](double u) { return std::tanh(u); });
          f_[c].set(sr, tone, 0.7071);
          f_[c].tick(dc_[c].process(v));
          *io[c] = f_[c].lp * dbToLin(-x * 12);
        }
        break;
      }
      case 1: {  // lo-fi: band limit + wobble
        double lp = expMap(18000, 1200, x), hp = expMap(20, 500, x);
        lfo2_ += 0.8 / sr;
        lfo2_ -= std::floor(lfo2_);
        double d = 1.0 + (1.0 + std::sin(kTwoPi * lfo2_)) * y * 0.004 * sr;
        for (int c = 0; c < 2; ++c) {
          mod_[c].push(*io[c]);
          double v = mod_[c].frac(d);
          f_[c].set(sr, lp, 0.8);
          f_[c].tick(v);
          f2_[c].set(sr, hp, 0.7071);
          f2_[c].tick(f_[c].lp);
          *io[c] = std::tanh(f2_[c].hp * 1.4) / 1.4 * 1.2;
        }
        break;
      }
      case 2: {  // flanger
        double d = expMap(0.3, 8, x) * 0.001 * sr, fb = (y - 0.5) * 1.8;
        for (int c = 0; c < 2; ++c) {
          double w = mod_[c].frac(d + (c ? 0.13 * d : 0.0));
          mod_[c].push(*io[c] + w * fb);
          *io[c] = (*io[c] + w) * 0.6;
        }
        break;
      }
      case 3: {  // phaser
        double hz = expMap(100, 6000, x);
        for (int c = 0; c < 2; ++c) {
          double v = ph_[c].process(*io[c], allpassCoef(hz * (c ? 1.15 : 1.0), sr), 6, y * 0.9);
          *io[c] = (*io[c] + v) * 0.5;
        }
        break;
      }
      case 4: {  // filter
        double hz = expMap(40, 18000, x), q = 0.6 + y * y * 14;
        for (int c = 0; c < 2; ++c) {
          f_[c].set(sr, hz, q);
          f_[c].tick(*io[c]);
          *io[c] = f_[c].lp;
        }
        break;
      }
      case 5: {  // tempo delay, ping-pong
        static const double div[6] = {0.125, 0.25, 0.375, 0.5, 0.75, 1.0};
        double beats = div[std::clamp(static_cast<int>(x * 6), 0, 5)];
        double d = clampd(beats * spb * sr, 1, static_cast<double>(delay_[0].capacity() - 4));
        double fb = y * 0.92;
        double a = delay_[0].frac(d), b = delay_[1].frac(d);
        delay_[0].push((l + r) * 0.5 + b * fb);
        delay_[1].push(a * fb);
        l += a * 0.7;
        r += b * 0.7;
        break;
      }
      case 6: {  // reverb
        if (std::fabs(x - revX_) > 1e-3 || std::fabs(y - revY_) > 1e-3 || sr != revSr_) {
          rev_.set(sr, x, 0.4 + x * x * 7.0, y * 0.85, 12);
          revX_ = x, revY_ = y, revSr_ = sr;
        }
        double wl, wr;
        rev_.process((l + r) * 0.5, wl, wr);
        l += wl * 0.6;
        r += wr * 0.6;
        break;
      }
      case 7: {  // ring modulator
        ring_ += expMap(20, 5000, x) / sr;
        ring_ -= std::floor(ring_);
        double s = std::sin(kTwoPi * ring_);
        double m = s + (std::tanh(s * 10.0) - s) * y;
        l *= m;
        r *= m;
        break;
      }
      case 8: {  // stereo width + Haas
        double w = x * 2.0;
        double mid = (l + r) * 0.5, side = (l - r) * 0.5 * w;
        double nl = mid + side, nr = mid - side;
        mod_[1].push(nr);
        double d = 1.0 + y * 0.02 * sr;
        l = nl;
        r = y > 0.001 ? mod_[1].frac(d) : nr;
        break;
      }
      case 9: {  // granular stutter: two overlapping grains, each segment repeated
        static const double div[6] = {1.0 / 32, 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0};
        double len = std::max(64.0, div[std::clamp(static_cast<int>(x * 6), 0, 5)] * spb * sr);
        len = std::floor(std::min(len, static_cast<double>(grain_[0].capacity()) * 0.15));
        double rate = std::pow(2.0, (y - 0.5) * 2.0);  // ±1 octave
        for (int c = 0; c < 2; ++c) grain_[c].push(*io[c]);
        writeAbs_ += 1.0;
        double out[2] = {0, 0};
        for (int g = 0; g < 2; ++g) {
          double t = std::fmod(grainPos_ + g * len * 0.5, len);
          if (t < 1.0) {
            // a new grain: every third one captures the most recent segment
            if (grainCount_[g]++ % 3 == 0 || writeAbs_ - grainStart_[g] > len * 4.0) grainStart_[g] = writeAbs_ - len;
          }
          double win = 0.5 - 0.5 * std::cos(kTwoPi * t / len);
          double back = writeAbs_ - (grainStart_[g] + t * rate);
          back = clampd(back, 1.0, static_cast<double>(grain_[0].capacity() - 4));
          for (int c = 0; c < 2; ++c) out[c] += grain_[c].frac(back) * win;
        }
        grainPos_ += 1.0;
        if (grainPos_ >= len * 64.0) grainPos_ = std::fmod(grainPos_, len);
        l = out[0];
        r = out[1];
        break;
      }
      case 10: {  // crusher
        double bits = lerp(16, 2, x);
        double steps = std::pow(2.0, bits - 1);
        int div = 1 + static_cast<int>(y * 39);
        if (hold_ <= 0) {
          held_[0] = std::round(l * steps) / steps;
          held_[1] = std::round(r * steps) / steps;
          hold_ = div;
        }
        --hold_;
        l = held_[0];
        r = held_[1];
        break;
      }
      default: {  // tremolo
        lfo2_ += expMap(0.5, 20, x) / sr;
        lfo2_ -= std::floor(lfo2_);
        double g = 1.0 - y * (0.5 - 0.5 * std::sin(kTwoPi * lfo2_));
        l *= g;
        r *= g;
        break;
      }
    }
  }

  efref::Effector ref_;
  DelayLine delay_[2], mod_[2], grain_[2];
  FdnReverb rev_;
  SvfTpt f_[2], f2_[2];
  PhaserChain ph_[2];
  Oversampler os_[2];
  DcBlock dc_[2];
  dsp::Noise noise_;
  int kind_ = -1;
  int hold_ = 0;
  double held_[2] = {};
  double phase_ = 0, randA_ = 0, randB_ = 0.5;
  double lfo2_ = 0, ring_ = 0, grainPos_ = 0, writeAbs_ = 0, grainStart_[2] = {};
  uint32_t grainCount_[2] = {};
  double revX_ = -1, revY_ = -1, revSr_ = 0;
  double curX_ = 0.5, curY_ = 0.5;
};

std::unique_ptr<Effect> createRackEffect(const std::string& type) {
  if (type == "gl.amprack") return std::make_unique<AmpRackFx>();
  if (type == "gl.effector") return std::make_unique<EffectorFx>();
  return nullptr;
}

}  // namespace gl::packs::dynamics

// Gridline — "synths" pack: DSP building blocks shared by Analog and Wavetable
// (realtime-safe: no allocation after construction, deterministic).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Dsp.h"
#include "../../engine/Generators.h"
#include "synths.h"

namespace gl::packs::synths {

inline float prm(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}
inline int prmi(const PluginState& s, int i, int dflt = 0) { return static_cast<int>(std::lround(prm(s, i, static_cast<float>(dflt)))); }
inline bool prmb(const PluginState& s, int i) { return prm(s, i) >= 0.5f; }

// Smooth saturator (≈tanh, bounded to ±1).
inline double softClip(double x) {
  x = std::clamp(x, -3.0, 3.0);
  return x * (27.0 + x * x) / (27.0 + 9.0 * x * x);
}

// Integer hash → [-1, 1] (deterministic random for S&H LFOs, phases, drift).
inline double hashUnit(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return static_cast<double>(x) / 2147483647.5 - 1.0;
}

// Integrated polyBLEP (polyBLAMP) residual for slope discontinuities.
inline double polyBlamp(double t, double dt) {
  if (t < dt) {
    t = t / dt - 1.0;
    return -t * t * t / 3.0;
  }
  if (t > 1.0 - dt) {
    t = (t - 1.0) / dt + 1.0;
    return t * t * t / 3.0;
  }
  return 0.0;
}

// LFO waveform at phase p (0..1) of cycle `cycle` (for the random shapes).
inline double lfoValue(int shape, double p, uint64_t cycle, uint32_t seed) {
  switch (shape) {
    case LfoSine: return std::sin(dsp::kTwoPi * p);
    case LfoTriangle: return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
    case LfoSawUp: return 2.0 * p - 1.0;
    case LfoSawDown: return 1.0 - 2.0 * p;
    case LfoSquare: return p < 0.5 ? 1.0 : -1.0;
    case LfoSampleHold: return hashUnit(static_cast<uint32_t>(cycle) * 2654435761u ^ seed);
    case LfoSmooth: {
      double a = hashUnit(static_cast<uint32_t>(cycle) * 2654435761u ^ seed);
      double b = hashUnit(static_cast<uint32_t>(cycle + 1) * 2654435761u ^ seed);
      double w = 0.5 - 0.5 * std::cos(dsp::kPi * p);
      return a + (b - a) * w;
    }
    default: return 0.0;
  }
}

// Running LFO phase: cycles counted so S&H shapes step once per cycle.
struct LfoPhase {
  double phase = 0;
  uint64_t cycle = 0;
  void advance(double inc) {
    phase += inc;
    if (phase >= 1.0) {
      double w = std::floor(phase);
      phase -= w;
      cycle += static_cast<uint64_t>(w);
    }
  }
  void reset(double p = 0) {
    phase = p;
    cycle = 0;
  }
};

// LFO rate in Hz from free rate or tempo division.
inline double lfoHz(bool sync, int div, double hz, double tempo) {
  if (!sync) return hz;
  div = std::clamp(div, 0, DivCount - 1);
  return std::max(1.0, tempo) / 60.0 / kDivBeats[div];
}

// Multimode filter (one channel): 12 dB = one SVF, 24 dB = two cascaded
// (Butterworth-aligned at zero resonance). Optional pre-drive.
struct MultiFilter {
  dsp::Svf a, b;
  int mode = FltLp24;
  double k = 1.4;  // damping of the resonant stage (for BP/notch/peak scaling)
  void reset() {
    a.reset();
    b.reset();
  }
  void set(double sr, double hz, double reso, int m) {
    mode = m;
    reso = std::clamp(reso, 0.0, 1.0);
    bool four = m == FltLp24 || m == FltBp24 || m == FltHp24;
    if (four) {
      const double r1 = (2.0 - 1.848) / 1.98, r2base = (2.0 - 0.765) / 1.98;
      a.set(sr, hz, r1 * (1.0 - reso));
      b.set(sr, hz, r2base + (1.0 - r2base) * reso * 0.995);
      k = b.k;
    } else {
      const double base = (2.0 - 1.414) / 1.98;
      a.set(sr, hz, base + (1.0 - base) * reso * 0.995);
      k = a.k;
    }
  }
  double process(double x) {
    double bp = 0, hp = 0, lp = 0;
    switch (mode) {
      case FltLp12: return a.process(x);
      case FltLp24: return b.process(a.process(x));
      case FltBp12: a.process(x, &bp); return bp * std::sqrt(k) * 1.2;
      case FltBp24: {
        a.process(x, &bp);
        double bp2 = 0;
        b.process(bp * a.k, &bp2);
        return bp2 * std::sqrt(k) * 1.4;
      }
      case FltHp12: a.process(x, nullptr, &hp); return hp;
      case FltHp24: {
        a.process(x, nullptr, &hp);
        double hp2 = 0;
        b.process(hp, nullptr, &hp2);
        return hp2;
      }
      case FltNotch: lp = a.process(x, &bp); return x - k * bp;
      case FltPeak: lp = a.process(x, &bp); return x + 3.0 * k * bp;
      default: return x;
    }
    (void)lp;
  }
  // |H| at hz for display (analytic SVF responses, mono).
  static double magnitude(int mode, double hz, double cutoff, double reso, double sr) {
    reso = std::clamp(reso, 0.0, 1.0);
    auto stage = [&](double r, int kind, double& mag) {
      double kk = 2.0 - 1.98 * r;
      double g = std::tan(dsp::kPi * std::clamp(cutoff, 10.0, sr * 0.49) / sr);
      double w = std::tan(dsp::kPi * std::clamp(hz, 1.0, sr * 0.49) / sr) / g;  // prewarped normalized frequency
      double re = 1.0 - w * w, im = kk * w;
      double den = std::sqrt(re * re + im * im);
      double num = kind == 0 ? 1.0 : kind == 1 ? w : kind == 2 ? w * w : std::fabs(1.0 - w * w);
      mag *= num / std::max(den, 1e-9);
      return kk;
    };
    double mag = 1.0;
    bool four = mode == FltLp24 || mode == FltBp24 || mode == FltHp24;
    const double base12 = (2.0 - 1.414) / 1.98, r1 = (2.0 - 1.848) / 1.98, r2base = (2.0 - 0.765) / 1.98;
    double r12 = base12 + (1.0 - base12) * reso * 0.995;
    switch (mode) {
      case FltLp12: stage(r12, 0, mag); break;
      case FltLp24: stage(r1 * (1 - reso), 0, mag); stage(r2base + (1 - r2base) * reso * 0.995, 0, mag); break;
      case FltHp12: stage(r12, 2, mag); break;
      case FltHp24: stage(r1 * (1 - reso), 2, mag); stage(r2base + (1 - r2base) * reso * 0.995, 2, mag); break;
      case FltBp12: { double kk = stage(r12, 1, mag); mag *= std::sqrt(kk) * 1.2; break; }
      case FltBp24: {
        double k1 = stage(r1 * (1 - reso), 1, mag);
        double k2 = stage(r2base + (1 - r2base) * reso * 0.995, 1, mag);
        mag *= k1 * std::sqrt(k2) * 1.4;
        break;
      }
      case FltNotch: stage(r12, 3, mag); break;
      case FltPeak: {
        double kk = 2.0 - 1.98 * r12;
        double g = std::tan(dsp::kPi * std::clamp(cutoff, 10.0, sr * 0.49) / sr);
        double w = std::tan(dsp::kPi * std::clamp(hz, 1.0, sr * 0.49) / sr) / g;
        // 1 + 3k·BP(s): numerator s² + 4k s + 1 over s² + k s + 1.
        double re = 1 - w * w, imN = 4 * kk * w, imD = kk * w;
        mag = std::sqrt((re * re + imN * imN) / std::max(re * re + imD * imD, 1e-12));
        break;
      }
      default: break;
    }
    (void)four;
    return mag;
  }
};

// Stereo effects strip: drive → chorus → delay → reverb. Buffers are sized
// for sample rates up to 192 kHz in the constructor; process() never allocates.
class FxRack {
 public:
  FxRack() {
    chorus_[0].assign(kChorusLen, 0.f);
    chorus_[1].assign(kChorusLen, 0.f);
    delay_[0].assign(kDelayLen, 0.f);
    delay_[1].assign(kDelayLen, 0.f);
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 8; ++i) comb_[c][i].buf.assign(static_cast<size_t>((kComb[i] + 23) * kMaxScale) + 2, 0.f);
      for (int i = 0; i < 4; ++i) ap_[c][i].buf.assign(static_cast<size_t>((kAllpass[i] + 23) * kMaxScale) + 2, 0.f);
    }
  }

  void configure(const PluginState& s, int base, double sr, double tempo) {
    sr_ = sr;
    drive_ = prm(s, base + FxDrive);
    bool cho = prmb(s, base + FxChoOn), dly = prmb(s, base + FxDlyOn), rev = prmb(s, base + FxRevOn);
    if (cho && !choOn_) clear(chorus_);
    if (dly && !dlyOn_) clear(delay_);
    if (rev && !revOn_) clearReverb();
    choOn_ = cho, dlyOn_ = dly, revOn_ = rev;
    choRate_ = prm(s, base + FxChoRate, 0.6f);
    choDepth_ = prm(s, base + FxChoDepth, 0.5f);
    choMix_ = prm(s, base + FxChoMix, 0.5f);
    int div = std::clamp(prmi(s, base + FxDlyDiv, Div1_8D), 0, DivCount - 1);
    double secs = std::min(2.5, kDivBeats[div] * 60.0 / std::max(20.0, tempo));
    dlyTarget_ = std::clamp(secs * sr, 1.0, static_cast<double>(kDelayLen - 4));
    dlyFb_ = prm(s, base + FxDlyFb, 0.4f);
    dlyMix_ = prm(s, base + FxDlyMix, 0.25f);
    double scale = std::min(kMaxScale, sr / 44100.0);
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 8; ++i) comb_[c][i].len = static_cast<int>((kComb[i] + (c ? 23 : 0)) * scale);
      for (int i = 0; i < 4; ++i) ap_[c][i].len = static_cast<int>((kAllpass[i] + (c ? 23 : 0)) * scale);
    }
    revFb_ = 0.7 + 0.28 * prm(s, base + FxRevSize, 0.6f);
    revDamp_ = 0.05 + 0.5 * prm(s, base + FxRevDamp, 0.5f);
    revMix_ = prm(s, base + FxRevMix, 0.25f);
    dampLp_ = std::exp(-dsp::kTwoPi * 5500.0 / sr);
  }

  bool tailActive() const { return choOn_ || dlyOn_ || revOn_; }

  void process(float* L, float* R, int n) {
    if (drive_ > 0.001f) {
      double g = 1.0 + drive_ * 12.0, makeup = 1.0 / softClip(g * 0.5) * 0.5;
      for (int i = 0; i < n; ++i) {
        L[i] = static_cast<float>(softClip(L[i] * g) * makeup);
        R[i] = static_cast<float>(softClip(R[i] * g) * makeup);
      }
    }
    if (choOn_) {
      double inc = choRate_ / sr_, baseD = 0.007 * sr_, depth = (0.0005 + 0.0045 * choDepth_) * sr_;
      for (int i = 0; i < n; ++i) {
        chorus_[0][static_cast<size_t>(choW_)] = L[i];
        chorus_[1][static_cast<size_t>(choW_)] = R[i];
        double s0 = std::sin(dsp::kTwoPi * choPh_), s1 = std::cos(dsp::kTwoPi * choPh_);
        float wl = readFrac(chorus_[0], choW_, baseD + depth * (0.5 + 0.5 * s0), kChorusLen);
        float wr = readFrac(chorus_[1], choW_, baseD + depth * (0.5 + 0.5 * s1), kChorusLen);
        L[i] = static_cast<float>(L[i] * (1.0 - choMix_ * 0.5) + wl * choMix_);
        R[i] = static_cast<float>(R[i] * (1.0 - choMix_ * 0.5) + wr * choMix_);
        choW_ = (choW_ + 1) & (kChorusLen - 1);
        choPh_ += inc;
        if (choPh_ >= 1) choPh_ -= 1;
      }
    }
    if (dlyOn_) {
      for (int i = 0; i < n; ++i) {
        dlyTime_ += (dlyTarget_ - dlyTime_) * 0.0005;  // glide on tempo changes
        float dl = readFrac(delay_[0], dlyW_, dlyTime_, kDelayLen);
        float dr = readFrac(delay_[1], dlyW_, dlyTime_, kDelayLen);
        // Ping-pong: each side feeds the other; mild high cut in the loop.
        dlpL_ = dl + (dlpL_ - dl) * 0.35;
        dlpR_ = dr + (dlpR_ - dr) * 0.35;
        delay_[0][static_cast<size_t>(dlyW_)] = static_cast<float>((L[i] + R[i]) * 0.5 + dlpR_ * dlyFb_);
        delay_[1][static_cast<size_t>(dlyW_)] = static_cast<float>(dlpL_ * dlyFb_);
        L[i] += static_cast<float>(dl * dlyMix_);
        R[i] += static_cast<float>(dr * dlyMix_);
        dlyW_ = (dlyW_ + 1) & (kDelayLen - 1);
      }
    }
    if (revOn_) {
      for (int i = 0; i < n; ++i) {
        double in = (L[i] + R[i]) * 0.015;
        double out[2] = {0, 0};
        for (int c = 0; c < 2; ++c) {
          for (int k = 0; k < 8; ++k) out[c] += comb_[c][k].run(in, revFb_, revDamp_);
          for (int k = 0; k < 4; ++k) out[c] = ap_[c][k].run(out[c]);
        }
        L[i] += static_cast<float>(out[0] * revMix_ * 1.4);
        R[i] += static_cast<float>(out[1] * revMix_ * 1.4);
      }
    }
  }

 private:
  static constexpr int kChorusLen = 8192;     // 42 ms at 192 kHz
  static constexpr int kDelayLen = 1 << 19;   // 2.7 s at 192 kHz
  static constexpr double kMaxScale = 192000.0 / 44100.0;
  static constexpr int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
  static constexpr int kAllpass[4] = {556, 441, 341, 225};

  struct Comb {
    std::vector<float> buf;
    int len = 1, pos = 0;
    double store = 0;
    double run(double in, double fb, double damp) {
      if (pos >= len) pos = 0;
      double y = buf[static_cast<size_t>(pos)];
      store = y * (1 - damp) + store * damp;
      if (std::fabs(store) < 1e-20) store = 0;
      buf[static_cast<size_t>(pos)] = static_cast<float>(in + store * fb);
      if (++pos >= len) pos = 0;
      return y;
    }
  };
  struct Allpass {
    std::vector<float> buf;
    int len = 1, pos = 0;
    double run(double in) {
      if (pos >= len) pos = 0;
      double b = buf[static_cast<size_t>(pos)];
      double out = b - in;
      buf[static_cast<size_t>(pos)] = static_cast<float>(in + b * 0.5);
      if (++pos >= len) pos = 0;
      return out;
    }
  };

  static void clear(std::vector<float>* bufs) {
    std::fill(bufs[0].begin(), bufs[0].end(), 0.f);
    std::fill(bufs[1].begin(), bufs[1].end(), 0.f);
  }
  void clearReverb() {
    for (auto& ch : comb_)
      for (auto& c : ch) std::fill(c.buf.begin(), c.buf.end(), 0.f), c.store = 0;
    for (auto& ch : ap_)
      for (auto& a : ch) std::fill(a.buf.begin(), a.buf.end(), 0.f);
  }
  static float readFrac(const std::vector<float>& b, int w, double delay, int len) {
    double rp = w - delay;
    while (rp < 0) rp += len;
    int i0 = static_cast<int>(rp);
    double f = rp - i0;
    int i1 = (i0 + 1) & (len - 1);
    i0 &= len - 1;
    return static_cast<float>(b[static_cast<size_t>(i0)] * (1 - f) + b[static_cast<size_t>(i1)] * f);
  }

  double sr_ = 44100;
  float drive_ = 0;
  bool choOn_ = false, dlyOn_ = false, revOn_ = false;
  std::vector<float> chorus_[2], delay_[2];
  int choW_ = 0, dlyW_ = 0;
  double choPh_ = 0, choRate_ = 0.6, choDepth_ = 0.5, choMix_ = 0.5;
  double dlyTime_ = 1000, dlyTarget_ = 1000, dlyFb_ = 0.4, dlyMix_ = 0.25, dlpL_ = 0, dlpR_ = 0;
  Comb comb_[2][8];
  Allpass ap_[2][4];
  double revFb_ = 0.84, revDamp_ = 0.3, revMix_ = 0.25, dampLp_ = 0;
};

// Note stack for mono/legato modes (last-note priority).
struct NoteStack {
  struct Note {
    uint32_t id;
    double pitch;
    float vel;
  };
  std::array<Note, 32> notes{};
  int count = 0;
  void push(uint32_t id, double pitch, float vel) {
    remove(id);
    if (count == static_cast<int>(notes.size())) {
      for (int i = 1; i < count; ++i) notes[static_cast<size_t>(i - 1)] = notes[static_cast<size_t>(i)];
      --count;
    }
    notes[static_cast<size_t>(count++)] = {id, pitch, vel};
  }
  bool remove(uint32_t id) {
    for (int i = 0; i < count; ++i)
      if (notes[static_cast<size_t>(i)].id == id) {
        for (int j = i + 1; j < count; ++j) notes[static_cast<size_t>(j - 1)] = notes[static_cast<size_t>(j)];
        --count;
        return true;
      }
    return false;
  }
  const Note* top() const { return count ? &notes[static_cast<size_t>(count - 1)] : nullptr; }
  void clear() { count = 0; }
};

// Factories (one per synth source file).
std::unique_ptr<Generator> makeAnalog();
std::unique_ptr<Generator> makeWavetable();
std::unique_ptr<Generator> makeMatrixFm();
std::unique_ptr<Generator> makeGms();
// Wave-file banks of a Groove Synth state (UI thread; null when none).
std::shared_ptr<const AuxData> buildGmsAux(const PluginState& s);
namespace mfm {
// Parsed drawn data + operator tables for a Matrix FM state (UI thread).
std::shared_ptr<const AuxData> buildMatrixFmAux(const PluginState& s);
}  // namespace mfm

}  // namespace gl::packs::synths

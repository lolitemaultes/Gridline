// Gridline — "mini" pack: tone family (Bands, Sieve, Rungs, Scorch, Vowel,
// Tines, Chime).
#include "mini_rt.h"

namespace gl::packs::mini::rt {

namespace {

using BQ = dsp::Biquad;

// ---- Bands: three-band EQ on Linkwitz-Riley (4th order) splits -----------------
class Bands final : public MiniFx {
  // Per channel: low split LP/HP (2 biquads each), high split LP/HP on the upper
  // branch, and the low band's allpass compensation at the high split.
  BQ lo1_[2][2], hi1_[2][2], lo2_[2][2], hi2_[2][2], apL_[2][2], apH_[2][2];
  Smooth g_[3];
  void clear() override {
    for (int c = 0; c < 2; ++c)
      for (int k = 0; k < 2; ++k)
        for (BQ* b : {&lo1_[c][k], &hi1_[c][k], &lo2_[c][k], &hi2_[c][k], &apL_[c][k], &apH_[c][k]}) b->clear();
    for (auto& g : g_) {
      g.setRate(sr_);
      g.init = false;
    }
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double f1 = pv(s, bands::LowSplit);
    const double f2 = std::max(f1 * 1.5, static_cast<double>(pv(s, bands::HighSplit)));
    for (int c = 0; c < 2; ++c)
      for (int k = 0; k < 2; ++k) {
        lo1_[c][k].set(BQ::LowPass, sr_, f1, 0.70710678, 0);
        hi1_[c][k].set(BQ::HighPass, sr_, f1, 0.70710678, 0);
        lo2_[c][k].set(BQ::LowPass, sr_, f2, 0.70710678, 0);
        hi2_[c][k].set(BQ::HighPass, sr_, f2, 0.70710678, 0);
        apL_[c][k].set(BQ::LowPass, sr_, f2, 0.70710678, 0);
        apH_[c][k].set(BQ::HighPass, sr_, f2, 0.70710678, 0);
      }
    const double t[3] = {dbg(pv(s, bands::Low)), dbg(pv(s, bands::Mid)), dbg(pv(s, bands::High))};
    for (int i = 0; i < n; ++i) {
      const double gl = g_[0].next(t[0]), gm = g_[1].next(t[1]), gh = g_[2].next(t[2]);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        double low = lo1_[c][1].process(lo1_[c][0].process(x));
        double up = hi1_[c][1].process(hi1_[c][0].process(x));
        // LR4 low + high at f2 sum to an allpass: keeps the low band in phase with the rest.
        low = apL_[c][1].process(apL_[c][0].process(low)) + apH_[c][1].process(apH_[c][0].process(low));
        double mid = lo2_[c][1].process(lo2_[c][0].process(up));
        double high = hi2_[c][1].process(hi2_[c][0].process(up));
        *io[c] = static_cast<float>(low * gl + mid * gm + high * gh);
      }
    }
  }
};

// ---- Sieve: one multi-mode filter with a slope selector -----------------------
class Sieve final : public MiniFx {
  BQ st_[2][4];
  void clear() override {
    for (auto& ch : st_)
      for (auto& b : ch) b.clear();
  }

 public:
  static BQ::Type typeOf(int t) {
    static const BQ::Type map[7] = {BQ::LowPass, BQ::HighPass, BQ::BandPass, BQ::Notch, BQ::Peak, BQ::LowShelf, BQ::HighShelf};
    return map[std::clamp(t, 0, 6)];
  }

 private:
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int type = std::clamp(pi(s, sieve::Type), 0, 6);
    const int stages = type <= 3 ? std::clamp(pi(s, sieve::Slope), 0, 3) + 1 : 1;
    const double hz = pv(s, sieve::Cutoff), q = pv(s, sieve::Q), gain = pv(s, sieve::Gain);
    for (auto& ch : st_)
      for (int k = 0; k < stages; ++k) ch[k].set(typeOf(type), sr_, hz, k == 0 ? q : 0.70710678, gain);
    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      for (int k = 0; k < stages; ++k) {
        l = st_[0][k].process(l);
        r = st_[1][k].process(r);
      }
      L[i] = static_cast<float>(l);
      R[i] = static_cast<float>(r);
    }
  }
};

// ---- Rungs: four-stage ladder low pass with a saturating feedback loop ----------
class Rungs final : public MiniFx {
  double z_[2][4] = {}, y4_[2] = {};
  Smooth fc_, mix_;
  void clear() override {
    for (auto& c : z_)
      for (double& v : c) v = 0;
    y4_[0] = y4_[1] = 0;
    fc_.setRate(sr_, 20);
    mix_.setRate(sr_);
    fc_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double target = std::clamp(static_cast<double>(pv(s, rungs::Cutoff)), 20.0, sr_ * 0.45);
    const double k = 3.9 * std::clamp(static_cast<double>(pv(s, rungs::Resonance)), 0.0, 1.0);
    const double driveDb = pv(s, rungs::Drive), dg = dbg(driveDb);
    const double comp = 1.0 / (1.0 + driveDb / 24.0);
    const int mode = std::clamp(pi(s, rungs::Mode), 0, 3);
    const double mixT = pv(s, rungs::Mix);
    for (int i = 0; i < n; ++i) {
      const double fc = fc_.next(target), wet = mix_.next(mixT);
      const double g = std::tan(kPi * fc / sr_), G = g / (1.0 + g);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        const double u = std::tanh(dg * x * (1.0 + 0.5 * k) - k * y4_[c]);
        double y[4], in = u;
        for (int st = 0; st < 4; ++st) {
          double v = (in - z_[c][st]) * G;
          y[st] = v + z_[c][st];
          z_[c][st] = flushd(y[st] + v);
          in = y[st];
        }
        y4_[c] = y[3];
        double out;
        switch (mode) {
          case 1: out = y[1]; break;
          case 2: out = 2.0 * (y[1] - y[3]); break;
          case 3: out = u - 4 * y[0] + 6 * y[1] - 4 * y[2] + y[3]; break;
          default: out = y[3]; break;
        }
        *io[c] = static_cast<float>(x * (1.0 - wet) + out * comp * wet);
      }
    }
  }
};

// ---- Scorch: state-variable filter with drive inside the loop ------------------
inline double flavour(int f, double x) {
  switch (f) {
    case 1: return std::tanh(x + 0.3) - 0.29131261;  // Hot: asymmetric (tanh 0.3 removed)
    case 2: return std::sin(x);                      // Fold
    case 3: return x / (1.0 + std::fabs(x));         // Lean
    default: return std::tanh(x);                    // Soft
  }
}

class Scorch final : public MiniFx {
  double ic1_[2] = {}, ic2_[2] = {};
  dsp::DcBlock dc_[2];
  Smooth fc_, mix_;
  void clear() override {
    ic1_[0] = ic1_[1] = ic2_[0] = ic2_[1] = 0;
    for (auto& d : dc_) {
      d.reset();
      d.setFreq(sr_);
    }
    fc_.setRate(sr_, 20);
    mix_.setRate(sr_);
    fc_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int type = std::clamp(pi(s, scorch::Type), 0, 3), fl = std::clamp(pi(s, scorch::Flavour), 0, 3);
    const double target = std::clamp(static_cast<double>(pv(s, scorch::Cutoff)), 20.0, sr_ * 0.45);
    const double k = 1.0 / std::max(0.5, static_cast<double>(pv(s, scorch::Q)));
    const double drive = std::clamp(static_cast<double>(pv(s, scorch::Drive)), 0.0, 1.0);
    const double dg = 1.0 + drive * 15.0, comp = 1.0 / (1.0 + drive * 3.0), tame = 0.08 * drive;
    const double mixT = pv(s, scorch::Mix);
    for (int i = 0; i < n; ++i) {
      const double g = std::tan(kPi * fc_.next(target) / sr_), wet = mix_.next(mixT);
      const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        const double v0 = flavour(fl, x * dg);
        const double v3 = v0 - ic2_[c];
        const double v1 = a1 * ic1_[c] + a2 * v3, v2 = ic2_[c] + a2 * ic1_[c] + a3 * v3;
        ic1_[c] = flushd(2.0 * v1 - ic1_[c]);
        ic1_[c] /= 1.0 + tame * std::fabs(ic1_[c]);  // the band state saturates with drive
        ic2_[c] = flushd(2.0 * v2 - ic2_[c]);
        const double hp = v0 - k * v1 - v2;
        double y = type == 0 ? v2 : type == 1 ? v1 : type == 2 ? hp : v2 + hp;
        if (fl == 1) y = dc_[c].process(y);
        *io[c] = static_cast<float>(x * (1.0 - wet) + y * comp * wet);
      }
    }
  }
};

// ---- Vowel: three formant band passes steered by an open/front pad ------------
class Vowel final : public MiniFx {
  BQ f_[2][3];
  Smooth mix_;
  void clear() override {
    for (auto& c : f_)
      for (auto& b : c) b.clear();
    mix_.setRate(sr_);
    mix_.init = false;
  }

 public:
  // Formant centres (Hz) for an open/front position and a shift in semitones.
  static void formants(double open, double front, double shift, double out[3]) {
    const double k = std::pow(2.0, shift / 12.0);
    out[0] = (280.0 + (800.0 - 280.0) * std::clamp(open, 0.0, 1.0)) * k;
    out[1] = (850.0 + (2300.0 - 850.0) * std::clamp(front, 0.0, 1.0)) * k;
    out[2] = (2500.0 + 600.0 * std::clamp(front, 0.0, 1.0)) * k;
  }

 private:
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    double fz[3];
    formants(pv(s, vowel::X), pv(s, vowel::Y), pv(s, vowel::Shift), fz);
    const double q = 2.0 + 14.0 * std::clamp(static_cast<double>(pv(s, vowel::Sharpness)), 0.0, 1.0);
    for (auto& c : f_)
      for (int k = 0; k < 3; ++k) c[k].set(BQ::BandPass, sr_, fz[k], q * (1.0 + 0.5 * k), 0);
    const double mixT = pv(s, vowel::Mix);
    const double amp[3] = {1.0, 0.8, 0.4};
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mixT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        double y = 0;
        for (int k = 0; k < 3; ++k) y += amp[k] * f_[c][k].process(x);
        *io[c] = static_cast<float>(x * (1.0 - wet) + 1.4 * y * wet);
      }
    }
  }
};

// ---- Tines: tuned feedback comb with damping ----------------------------------
class Tines final : public MiniFx {
  Ring d_[2];
  dsp::OnePole lp_[2];
  Smooth del_[2], mix_;

 public:
  Tines() {
    for (auto& d : d_) d.init(static_cast<size_t>(kBufRate / 20.0) + 8);
  }

 private:
  void clear() override {
    for (int c = 0; c < 2; ++c) {
      d_[c].clear();
      lp_[c].z = 0;
      del_[c].setRate(sr_, 15);
      del_[c].init = false;
    }
    mix_.setRate(sr_);
    mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double f = pv(s, tines::Freq), fb = std::clamp(static_cast<double>(pv(s, tines::Feedback)), -0.95, 0.95);
    const double fr = f * std::pow(2.0, pv(s, tines::Stereo) / 12.0);
    const double damp = std::clamp(static_cast<double>(pv(s, tines::Damp)), 0.0, 1.0);
    for (auto& l : lp_) l.setFreq(sr_, 20000.0 * std::pow(500.0 / 20000.0, damp));
    const double pol = pi(s, tines::Polarity) == 1 ? -1.0 : 1.0, norm = 1.0 - std::fabs(fb);
    const double t[2] = {sr_ / std::max(1.0, f), sr_ / std::max(1.0, fr)};
    const double mixT = pv(s, tines::Mix);
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mixT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        const double y = x + fb * lp_[c].process(d_[c].read(del_[c].next(t[c])));
        d_[c].push(flush(static_cast<float>(y)));
        *io[c] = static_cast<float>(x * (1.0 - wet) + pol * y * norm * wet);
      }
    }
  }
};

// ---- Chime: bank of tuned two-pole resonators ---------------------------------
class Chime final : public MiniFx {
  static constexpr int kPartials = 12;
  struct Res {
    double b0 = 0, a1 = 0, a2 = 0, y1[2] = {}, y2[2] = {}, x1[2] = {}, x2[2] = {}, amp = 0;
    bool on = false;
  } r_[kPartials];
  Smooth mix_;
  void clear() override {
    for (auto& r : r_) r.y1[0] = r.y1[1] = r.y2[0] = r.y2[1] = r.x1[0] = r.x1[1] = r.x2[0] = r.x2[1] = 0;
    mix_.setRate(sr_);
    mix_.init = false;
  }

 public:
  // Frequency ratio of partial k (0-based) for a partial set.
  static double ratio(int set, int k) {
    if (set == 1) return 2.0 * k + 1.0;
    if (set == 2) return std::pow(k + 1.0, 1.42);  // hollow: stretched, bell-like
    return k + 1.0;
  }

 private:
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double f0 = dsp::noteToHz(pv(s, chime::Note));
    const double decay = std::max(5.0, static_cast<double>(pv(s, chime::Decay))) * 0.001;
    const double bright = std::clamp(static_cast<double>(pv(s, chime::Bright)), 0.0, 1.0);
    const int set = std::clamp(pi(s, chime::Partials), 0, 2);
    double sum = 0;
    for (int k = 0; k < kPartials; ++k) {
      Res& r = r_[k];
      const double f = f0 * ratio(set, k);
      r.on = f < sr_ * 0.45;
      if (!r.on) continue;
      const double tau = decay / (1.0 + 0.15 * k * (1.0 - 0.5 * bright));
      const double rad = std::exp(-1.0 / (tau * sr_));
      r.b0 = 0.5 * (1.0 - rad * rad);
      r.a1 = -2.0 * rad * std::cos(kTwoPi * f / sr_);
      r.a2 = rad * rad;
      r.amp = std::pow(0.15 + 0.85 * bright, k) / std::sqrt(k + 1.0);
      sum += r.amp;
    }
    const double norm = 1.5 / std::max(1.0, sum);
    const double mixT = pv(s, chime::Mix);
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mixT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        double y = 0;
        for (auto& r : r_) {
          if (!r.on) continue;
          const double v = r.b0 * (x - r.x2[c]) - r.a1 * r.y1[c] - r.a2 * r.y2[c];
          r.x2[c] = r.x1[c];
          r.x1[c] = x;
          r.y2[c] = r.y1[c];
          r.y1[c] = flushd(v);
          y += r.amp * v;
        }
        *io[c] = static_cast<float>(x * (1.0 - wet) + y * norm * wet);
      }
    }
  }
};

}  // namespace

std::unique_ptr<Effect> createTone(const std::string& type) {
  if (type == "gl.minibands") return std::make_unique<Bands>();
  if (type == "gl.minisieve") return std::make_unique<Sieve>();
  if (type == "gl.minirungs") return std::make_unique<Rungs>();
  if (type == "gl.miniscorch") return std::make_unique<Scorch>();
  if (type == "gl.minivowel") return std::make_unique<Vowel>();
  if (type == "gl.minitines") return std::make_unique<Tines>();
  if (type == "gl.minichime") return std::make_unique<Chime>();
  return nullptr;
}

}  // namespace gl::packs::mini::rt

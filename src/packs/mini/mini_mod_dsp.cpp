// Gridline — "mini" pack: motion and pitch families (Double, Crowd, Jet, Orbit,
// Slide, Interval).
#include "mini_rt.h"

namespace gl::packs::mini::rt {

namespace {

inline double wrap01(double p) { return p - std::floor(p); }

// ---- Double: chorus with 2 to 4 voices spread across the stereo field ---------
class Double final : public MiniFx {
  Ring d_;
  double ph_ = 0;
  Smooth del_, dep_, mix_;

 public:
  Double() { d_.init(static_cast<size_t>(0.065 * kBufRate)); }

 private:
  void clear() override {
    d_.clear();
    ph_ = 0;
    for (Smooth* s : {&del_, &dep_, &mix_}) {
      s->setRate(sr_, 20);
      s->init = false;
    }
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int voices = std::clamp(pi(s, dbl::Voices), 0, 2) + 2;
    const double rate = pv(s, dbl::Rate), spread = std::clamp(static_cast<double>(pv(s, dbl::Spread)), 0.0, 1.0);
    const double delT = pv(s, dbl::Delay) * 0.001 * sr_, depT = pv(s, dbl::Depth) * 0.001 * sr_, mT = pv(s, dbl::Mix);
    double gl[4], gr[4];
    for (int k = 0; k < voices; ++k) {
      const double pan = spread * (-1.0 + 2.0 * k / (voices - 1));
      gl[k] = std::sqrt(0.5 * (1.0 - pan));
      gr[k] = std::sqrt(0.5 * (1.0 + pan));
    }
    const double norm = std::sqrt(2.0 / voices);
    for (int i = 0; i < n; ++i) {
      const double base = del_.next(delT), depth = dep_.next(depT), wet = mix_.next(mT);
      d_.push(0.5f * (L[i] + R[i]));
      double wl = 0, wr = 0;
      for (int k = 0; k < voices; ++k) {
        const double lfo = std::sin(kTwoPi * wrap01(ph_ + static_cast<double>(k) / voices));
        const double v = d_.read(std::max(0.0001 * sr_, base + depth * 0.5 * (1.0 + lfo)) + 1.0);
        wl += v * gl[k];
        wr += v * gr[k];
      }
      ph_ = wrap01(ph_ + rate / sr_);
      L[i] = static_cast<float>(L[i] * (1.0 - wet) + wl * norm * wet);
      R[i] = static_cast<float>(R[i] * (1.0 - wet) + wr * norm * wet);
    }
  }
};

// ---- Crowd: up to 12 slowly drifting voices (unison thickener) -----------------
class Crowd final : public MiniFx {
  static constexpr int kVoices = 12;
  Ring d_;
  double ph_[kVoices] = {}, pan_[kVoices] = {};
  Smooth det_, mix_;

 public:
  Crowd() { d_.init(static_cast<size_t>(0.04 * kBufRate)); }

 private:
  void clear() override {
    d_.clear();
    for (int k = 0; k < kVoices; ++k) {
      ph_[k] = wrap01(k * 0.381966);
      pan_[k] = wrap01(k * 0.618034);
    }
    det_.setRate(sr_, 30);
    mix_.setRate(sr_);
    det_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int voices = std::clamp(static_cast<int>(std::lround(pv(s, crowd::Voices))), 2, kVoices);
    const double spread = std::clamp(static_cast<double>(pv(s, crowd::Spread)), 0.0, 1.0);
    const bool swirl = pi(s, crowd::Motion) == 1;
    const double detT = std::clamp(static_cast<double>(pv(s, crowd::Detune)), 0.0, 1.0) * 0.006 * sr_, mT = pv(s, crowd::Mix);
    const double centre = 0.014 * sr_, norm = std::sqrt(2.0 / voices);
    for (int i = 0; i < n; ++i) {
      const double depth = det_.next(detT), wet = mix_.next(mT);
      d_.push(0.5f * (L[i] + R[i]));
      double wl = 0, wr = 0;
      for (int k = 0; k < voices; ++k) {
        const double rate = (0.13 + 0.37 * wrap01(k * 0.618034)) * (swirl ? 4.0 : 1.0);
        ph_[k] = wrap01(ph_[k] + rate / sr_);
        const double v = d_.read(centre + depth * std::sin(kTwoPi * ph_[k]) + 1.0);
        double pan = spread * (-1.0 + 2.0 * k / (voices - 1));
        if (swirl) {
          pan_[k] = wrap01(pan_[k] + rate * 0.25 / sr_);
          pan = spread * std::sin(kTwoPi * pan_[k]);
        }
        wl += v * std::sqrt(0.5 * (1.0 - pan));
        wr += v * std::sqrt(0.5 * (1.0 + pan));
      }
      L[i] = static_cast<float>(L[i] * (1.0 - wet) + wl * norm * wet);
      R[i] = static_cast<float>(R[i] * (1.0 - wet) + wr * norm * wet);
    }
  }
};

// ---- Jet: flanger with feedback ------------------------------------------------
class Jet final : public MiniFx {
  Ring d_[2];
  double ph_ = 0;
  Smooth del_, mix_;

 public:
  Jet() {
    for (auto& d : d_) d.init(static_cast<size_t>(0.021 * kBufRate));
  }

 private:
  void clear() override {
    d_[0].clear();
    d_[1].clear();
    ph_ = 0;
    del_.setRate(sr_, 20);
    mix_.setRate(sr_);
    del_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double delT = pv(s, jet::Delay) * 0.001 * sr_, depth = std::clamp(static_cast<double>(pv(s, jet::Depth)), 0.0, 1.0);
    const double rate = pv(s, jet::Rate), fb = std::clamp(static_cast<double>(pv(s, jet::Feedback)), -0.95, 0.95);
    const double off = 0.5 * std::clamp(static_cast<double>(pv(s, jet::Stereo)), 0.0, 1.0), mT = pv(s, jet::Mix);
    for (int i = 0; i < n; ++i) {
      const double base = del_.next(delT), wet = mix_.next(mT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double lfo = std::sin(kTwoPi * wrap01(ph_ + (c ? off : 0.0)));
        const double y = d_[c].read(std::max(1.0, base * (1.0 + 0.95 * depth * lfo)));
        const double x = *io[c];
        d_[c].push(flush(static_cast<float>(x + fb * y)));
        *io[c] = static_cast<float>(x * (1.0 - wet) + y * wet);
      }
      ph_ = wrap01(ph_ + rate / sr_);
    }
  }
};

// ---- Orbit: phaser (2 to 12 first-order allpass stages) ------------------------
class Orbit final : public MiniFx {
  static constexpr int kMaxStages = 12;
  dsp::Allpass1 ap_[2][kMaxStages];
  double last_[2] = {}, ph_ = 0;
  Smooth mix_;
  void clear() override {
    for (auto& c : ap_)
      for (auto& a : c) a.reset();
    last_[0] = last_[1] = 0;
    ph_ = 0;
    mix_.setRate(sr_);
    mix_.init = false;
  }

 public:
  static int stagesOf(int choice) {
    static const int kStages[5] = {2, 4, 6, 8, 12};
    return kStages[std::clamp(choice, 0, 4)];
  }

 private:
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int stages = stagesOf(pi(s, orbit::Stages));
    const double centre = pv(s, orbit::Centre), depth = std::clamp(static_cast<double>(pv(s, orbit::Depth)), 0.0, 1.0);
    const double rate = pv(s, orbit::Rate), fb = std::clamp(static_cast<double>(pv(s, orbit::Feedback)), -0.9, 0.9);
    const double off = 0.5 * std::clamp(static_cast<double>(pv(s, orbit::Stereo)), 0.0, 1.0), mT = pv(s, orbit::Mix);
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double lfo = std::sin(kTwoPi * wrap01(ph_ + (c ? off : 0.0)));
        ap_[c][0].setFreq(sr_, centre * std::pow(2.0, 3.0 * depth * lfo));
        const double x = *io[c];
        double y = x + fb * last_[c];
        for (int k = 0; k < stages; ++k) {
          ap_[c][k].a = ap_[c][0].a;
          y = ap_[c][k].process(y);
        }
        last_[c] = flushd(y);
        *io[c] = static_cast<float>(x * (1.0 - wet) + y * wet);
      }
      ph_ = wrap01(ph_ + rate / sr_);
    }
  }
};

// ---- Slide: frequency shifter (allpass Hilbert pair + quadrature oscillator) ---
struct Hilbert {
  // Two cascades of four second-order allpass sections whose outputs stay
  // about 90 degrees apart over most of the audio band.
  static constexpr double kA[4] = {0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
  static constexpr double kB[4] = {0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};
  double xa[4][2] = {}, ya[4][2] = {}, xb[4][2] = {}, yb[4][2] = {}, delayB = 0;
  static double section(double a, double x, double (&xs)[2], double (&ys)[2]) {
    const double y = a * a * (x + ys[1]) - xs[1];
    xs[1] = xs[0];
    xs[0] = x;
    ys[1] = ys[0];
    ys[0] = flushd(y);
    return y;
  }
  void process(double x, double& re, double& im) {
    double a = x, b = x;
    for (int k = 0; k < 4; ++k) {
      a = section(kA[k], a, xa[k], ya[k]);
      b = section(kB[k], b, xb[k], yb[k]);
    }
    re = a;
    im = delayB;
    delayB = b;
  }
  void reset() { *this = Hilbert{}; }
};

class Slide final : public MiniFx {
  Hilbert h_[2];
  double ph_[2] = {}, fbv_[2] = {};
  Smooth sh_, mix_;
  void clear() override {
    for (auto& h : h_) h.reset();
    ph_[0] = ph_[1] = fbv_[0] = fbv_[1] = 0;
    sh_.setRate(sr_, 20);
    mix_.setRate(sr_);
    sh_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double shT = pv(s, slide::Shift), st = pv(s, slide::Stereo);
    const double fb = std::clamp(static_cast<double>(pv(s, slide::Feedback)), 0.0, 0.9), mT = pv(s, slide::Mix);
    for (int i = 0; i < n; ++i) {
      const double shift = sh_.next(shT), wet = mix_.next(mT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double f = shift + (c == 0 ? -st : st);
        const double x = *io[c];
        double re, im;
        h_[c].process(x + fb * fbv_[c], re, im);
        const double y = re * std::cos(kTwoPi * ph_[c]) + im * std::sin(kTwoPi * ph_[c]);  // upper sideband
        ph_[c] = wrap01(ph_[c] + f / sr_);
        fbv_[c] = flushd(y);
        *io[c] = static_cast<float>(x * (1.0 - wet) + y * wet);
      }
    }
  }
};

// ---- Interval: two-tap granular pitch shifter ----------------------------------
class Interval final : public MiniFx {
  Ring d_[2];
  double p_ = 0, off_[2] = {};
  dsp::Noise noise_;
  Smooth mix_;

 public:
  Interval() {
    for (auto& d : d_) d.init(static_cast<size_t>(0.32 * kBufRate));
  }

 private:
  void clear() override {
    d_[0].clear();
    d_[1].clear();
    p_ = 0;
    off_[0] = off_[1] = 0;
    noise_.seed(0x1d7e3a5bu);
    mix_.setRate(sr_);
    mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double ratio = std::pow(2.0, (pv(s, interval::Pitch) + pv(s, interval::Fine) / 100.0) / 12.0);
    const double maxG = (d_[0].maxDelay() - 4.0) / 1.5;
    const double G = std::clamp(pv(s, interval::Grain) * 0.001 * sr_, 64.0, maxG);
    const double jit = std::clamp(static_cast<double>(pv(s, interval::Jitter)), 0.0, 1.0), mT = pv(s, interval::Mix);
    const double dp = (1.0 - ratio) / G;
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mT);
      const double q0 = wrap01(p_ + 0.5);
      p_ += dp;
      if (p_ >= 1.0 || p_ < 0.0) {  // tap 1 restarts while its window is closed
        p_ = wrap01(p_);
        off_[0] = jit * 0.5 * G * (0.5 + 0.5 * noise_.next());
      }
      const double q = wrap01(p_ + 0.5);
      if (std::fabs(q - q0) > 0.5) off_[1] = jit * 0.5 * G * (0.5 + 0.5 * noise_.next());
      const double w1 = std::sin(kPi * p_) * std::sin(kPi * p_), w2 = 1.0 - w1;
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        d_[c].push(static_cast<float>(x));
        // J-140: band-limited taps at the pitch ratio (were Hermite: aliased upward).
        const pitch::SincTable& tab = pitch::table(pitch::Quality::Compact);
        const double y = w1 * d_[c].band(2.0 + off_[0] + p_ * G, ratio, tab) + w2 * d_[c].band(2.0 + off_[1] + q * G, ratio, tab);
        *io[c] = static_cast<float>(x * (1.0 - wet) + y * wet);
      }
    }
  }
};

}  // namespace

std::unique_ptr<Effect> createMotion(const std::string& type) {
  if (type == "gl.minidouble") return std::make_unique<Double>();
  if (type == "gl.minicrowd") return std::make_unique<Crowd>();
  if (type == "gl.minijet") return std::make_unique<Jet>();
  if (type == "gl.miniorbit") return std::make_unique<Orbit>();
  if (type == "gl.minislide") return std::make_unique<Slide>();
  if (type == "gl.miniinterval") return std::make_unique<Interval>();
  return nullptr;
}

}  // namespace gl::packs::mini::rt

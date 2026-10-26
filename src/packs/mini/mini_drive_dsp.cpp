// Gridline — "mini" pack: drive family (Burn, Bend, Warp, Crumble, Clang).
#include "mini_curves.h"
#include "mini_rt.h"

namespace gl::packs::mini::rt {

namespace {

constexpr int kOsStages = 2;  // 4x oversampling for the shapers

// Dry path delayed to match the oversampler, so Mix never combs.
template <int N>
struct DryAlign {
  dsp::FixedDelay<N> d;
  float process(float x, int lat) {
    d.push(x);
    return lat <= 0 ? x : d.tap(lat + 1);
  }
};

// ---- Burn: oversampled saturation with six characters --------------------------
class Burn final : public MiniFx {
  dsp::Oversampler os_[2];
  dsp::DcBlock dc_[2];
  dsp::OnePole tone_[2];
  DryAlign<64> dry_[2];
  Smooth dr_, out_, mix_;

 public:
  int latencyFor(const PluginState&, double) const override { return dsp::Oversampler::latency(kOsStages); }

 private:
  void clear() override {
    for (int c = 0; c < 2; ++c) {
      os_[c].reset();
      dc_[c].reset();
      dc_[c].setFreq(sr_);
      tone_[c].z = 0;
      dry_[c].d.clear();
    }
    for (Smooth* s : {&dr_, &out_, &mix_}) {
      s->setRate(sr_);
      s->init = false;
    }
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int type = std::clamp(pi(s, burn::Type), 0, 5);
    const double bias = 0.5 * pv(s, burn::Bias), tone = std::clamp(static_cast<double>(pv(s, burn::Tone)), -1.0, 1.0);
    const double dT = dbg(pv(s, burn::Drive)), oT = dbg(pv(s, burn::Output)), mT = pv(s, burn::Mix);
    for (auto& t : tone_) t.setFreq(sr_, 1200.0);
    const int lat = dsp::Oversampler::latency(kOsStages);
    auto f = [type, bias](double v) { return curve::burn(type, v + bias) - curve::burn(type, bias); };
    for (int i = 0; i < n; ++i) {
      const double d = dr_.next(dT), o = out_.next(oT), wet = mix_.next(mT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const float x = *io[c];
        double y = dc_[c].process(os_[c].run(x * d, kOsStages, f));
        const double lp = tone_[c].process(y), hi = y - lp;
        y = tone >= 0 ? y + tone * hi : lp + (1.0 + tone) * hi;
        *io[c] = static_cast<float>(dry_[c].process(x, lat) * (1.0 - wet) + y * o * wet);
      }
    }
  }
};

// ---- Bend: wave shaper with selectable shapes and edge rules -------------------
class Bend final : public MiniFx {
  dsp::Oversampler os_[2];
  dsp::DcBlock dc_[2];
  DryAlign<64> dry_[2];
  Smooth dr_, mix_;

 public:
  int latencyFor(const PluginState&, double) const override { return dsp::Oversampler::latency(kOsStages); }

 private:
  void clear() override {
    for (int c = 0; c < 2; ++c) {
      os_[c].reset();
      dc_[c].reset();
      dc_[c].setFreq(sr_);
      dry_[c].d.clear();
    }
    dr_.setRate(sr_);
    mix_.setRate(sr_);
    dr_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int shape = std::clamp(pi(s, bend::Shape), 0, 5), edge = std::clamp(pi(s, bend::Edge), 0, 2);
    const double amount = pv(s, bend::Amount), dT = dbg(pv(s, bend::Drive)), mT = pv(s, bend::Mix);
    const int lat = dsp::Oversampler::latency(kOsStages);
    const double off = curve::bend(shape, amount, edge, 0.0);
    auto f = [shape, amount, edge, off](double v) { return curve::bend(shape, amount, edge, v) - off; };
    for (int i = 0; i < n; ++i) {
      const double d = dr_.next(dT), wet = mix_.next(mT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const float x = *io[c];
        double y = os_[c].run(x * d, kOsStages, f);
        if (shape == 4 || shape == 5) y = dc_[c].process(y);
        *io[c] = static_cast<float>(dry_[c].process(x, lat) * (1.0 - wet) + y * wet);
      }
    }
  }
};

// ---- Warp: phase distortion (the signal modulates its own delay) ---------------
class Warp final : public MiniFx {
  Ring d_[2];
  dsp::OnePole lp_[2];
  dsp::DcBlock dcIn_[2];
  Smooth dr_, mix_;

 public:
  Warp() {
    for (auto& d : d_) d.init(1024);
  }
  static int centre(double sr) { return std::clamp(static_cast<int>(std::lround(0.0005 * sr)), 2, 480); }
  int latencyFor(const PluginState&, double sampleRate) const override { return centre(sampleRate); }

 private:
  void clear() override {
    for (int c = 0; c < 2; ++c) {
      d_[c].clear();
      lp_[c].z = 0;
      dcIn_[c].reset();
      dcIn_[c].setFreq(sr_);
    }
    dr_.setRate(sr_);
    mix_.setRate(sr_);
    dr_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double dT = pv(s, warp::Drive), bias = pv(s, warp::Bias), st = std::clamp(static_cast<double>(pv(s, warp::Stereo)), 0.0, 1.0);
    const double mT = pv(s, warp::Mix);
    for (auto& l : lp_) l.setFreq(sr_, pv(s, warp::Tone));
    const double D0 = centre(sr_);
    for (int i = 0; i < n; ++i) {
      const double drive = dr_.next(dT), wet = mix_.next(mT);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const float x = *io[c];
        double m = drive * (3.0 * lp_[c].process(dcIn_[c].process(x)) + bias);
        if (c == 1) m *= 1.0 - 2.0 * st;
        m = std::clamp(m, -0.95, 0.95);
        d_[c].push(x);
        const double y = d_[c].read(D0 * (1.0 + m) + 1.0);
        *io[c] = static_cast<float>(d_[c].at(static_cast<size_t>(D0) + 1) * (1.0 - wet) + y * wet);
      }
    }
  }
};

// ---- Crumble: sample-rate and bit-depth reduction ------------------------------
class Crumble final : public MiniFx {
  double ph_ = 0, period_ = 1, hold_[2] = {};
  dsp::Noise noise_;
  dsp::OnePole sm_[2];
  Smooth mix_;
  void clear() override {
    ph_ = 1.0;  // sample on the first input
    period_ = 1;
    hold_[0] = hold_[1] = 0;
    noise_.seed(0x5eed1234u);
    sm_[0].z = sm_[1].z = 0;
    mix_.setRate(sr_);
    mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double rate = std::clamp(static_cast<double>(pv(s, crumble::Rate)), 50.0, sr_);
    const double steps = std::pow(2.0, std::clamp(static_cast<double>(pv(s, crumble::Bits)), 1.0, 16.0) - 1.0);
    const double jit = pv(s, crumble::Jitter), dith = pv(s, crumble::Dither);
    const double smooth = std::clamp(static_cast<double>(pv(s, crumble::Smooth)), 0.0, 1.0);
    const bool filt = smooth > 0.001;
    if (filt)
      for (auto& f : sm_) f.setFreq(sr_, std::min(20000.0, rate * 0.5) * std::pow(0.15, smooth));
    const double mT = pv(s, crumble::Mix), inc = rate / sr_;
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mT);
      ph_ += inc;
      if (ph_ >= period_) {
        ph_ -= period_;
        period_ = 1.0 + jit * 0.9 * noise_.next();
        if (ph_ >= period_) ph_ = 0;
        for (int c = 0; c < 2; ++c) {
          const double x = c == 0 ? L[i] : R[i];
          double d = 0;
          if (dith > 0 && x != 0) d = dith * (noise_.next() + noise_.next()) * 0.5;
          hold_[c] = std::round(x * steps + d) / steps;
        }
      }
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        double y = hold_[c];
        if (filt) y = sm_[c].process(y);
        *io[c] = static_cast<float>(*io[c] * (1.0 - wet) + y * wet);
      }
    }
  }
};

// ---- Clang: ring modulator (oscillator or sidechain carrier) -------------------
inline double carrier(int shape, double ph, double dt) {
  switch (shape) {
    case 1: return ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph;  // Triangle
    case 2: {                                                   // Square
      double v = ph < 0.5 ? 1.0 : -1.0;
      return v + dsp::polyBlep(ph, dt) - dsp::polyBlep(std::fmod(ph + 0.5, 1.0), dt);
    }
    case 3: return 2.0 * ph - 1.0 - dsp::polyBlep(ph, dt);  // Ramp
    default: return std::sin(kTwoPi * ph);
  }
}

class Clang final : public MiniFx {
  double ph_[2] = {};
  Smooth f_, mix_;
  void clear() override {
    ph_[0] = ph_[1] = 0;
    f_.setRate(sr_, 20);
    mix_.setRate(sr_);
    f_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const int shape = std::clamp(pi(s, clang::Shape), 0, 3);
    const double fT = pv(s, clang::Freq), bias = std::clamp(static_cast<double>(pv(s, clang::Bias)), 0.0, 1.0);
    const double spread = std::clamp(static_cast<double>(pv(s, clang::Spread)), 0.0, 1.0);
    const bool side = pi(s, clang::Source) == 1;
    const double mT = pv(s, clang::Mix);
    for (int i = 0; i < n; ++i) {
      const double f = f_.next(fT), wet = mix_.next(mT);
      const double dt[2] = {f / sr_, f * (1.0 + 0.06 * spread) / sr_};
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        double car;
        if (side) {
          const float* sc = c == 0 ? io_.scL : io_.scR;
          car = sc ? sc[i] : 0.0;
        } else {
          car = carrier(shape, ph_[c], dt[c]);
          ph_[c] += dt[c];
          if (ph_[c] >= 1.0) ph_[c] -= std::floor(ph_[c]);
        }
        const double x = *io[c];
        *io[c] = static_cast<float>(x * (1.0 - wet) + x * ((1.0 - bias) * car + bias) * wet);
      }
    }
  }
};

}  // namespace

std::unique_ptr<Effect> createDrive(const std::string& type) {
  if (type == "gl.miniburn") return std::make_unique<Burn>();
  if (type == "gl.minibend") return std::make_unique<Bend>();
  if (type == "gl.miniwarp") return std::make_unique<Warp>();
  if (type == "gl.minicrumble") return std::make_unique<Crumble>();
  if (type == "gl.miniclang") return std::make_unique<Clang>();
  return nullptr;
}

}  // namespace gl::packs::mini::rt

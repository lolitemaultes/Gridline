// Gridline — "mini" pack: time family (Echo, Volley, Vault, Rewind, Halt, Pulse).
#include "mini_rt.h"

namespace gl::packs::mini::rt {

namespace {

// Synced or free time (samples) of Echo.
double echoSamples(const PluginState& s, const ProcessContext& c, double sr) {
  if (pb(s, echo::Sync)) return beatsToSamples(kNoteBeats[std::clamp(pi(s, echo::Steps), 0, kNoteValues - 1)], tempoOf(c), sr);
  return pv(s, echo::Time) * 0.001 * sr;
}

// ---- Echo: tempo-synced feedback delay with ping-pong, tone and ducking --------
class Echo final : public MiniFx {
  Ring d_[2];
  Tilt tone_[2];
  Follower duck_;
  Smooth t_, mix_;

 public:
  Echo() {
    for (auto& d : d_) d.init(static_cast<size_t>(2.6 * kBufRate));
  }

 private:
  void clear() override {
    for (int c = 0; c < 2; ++c) {
      d_[c].clear();
      tone_[c].reset();
    }
    duck_.env = 0;
    t_.setRate(sr_, 60);
    mix_.setRate(sr_);
    t_.init = mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext& c, float* L, float* R, int n) override {
    const double tT = std::clamp(echoSamples(s, c, sr_), 1.0, d_[0].maxDelay());
    const double fb = std::clamp(static_cast<double>(pv(s, echo::Feedback)), 0.0, 1.0);
    const bool pp = pb(s, echo::PingPong);
    const double duck = pv(s, echo::Duck), mT = pv(s, echo::Mix);
    for (auto& t : tone_) t.set(sr_, pv(s, echo::Tone));
    duck_.set(sr_, 5.0, 250.0);
    for (int i = 0; i < n; ++i) {
      const double t = t_.next(tT), wet = mix_.next(mT);
      const double yl = d_[0].read(t), yr = d_[1].read(t);
      const double xl = L[i], xr = R[i];
      if (pp) {
        d_[0].push(flush(static_cast<float>(tone_[0].process(0.5 * (xl + xr) + fb * yr))));
        d_[1].push(flush(static_cast<float>(tone_[1].process(fb * yl))));
      } else {
        d_[0].push(flush(static_cast<float>(tone_[0].process(xl + fb * yl))));
        d_[1].push(flush(static_cast<float>(tone_[1].process(xr + fb * yr))));
      }
      const double env = duck_.process(std::max(std::fabs(xl), std::fabs(xr)));
      const double wg = wet * (1.0 - duck * std::clamp(env * 4.0, 0.0, 1.0));
      L[i] = static_cast<float>(xl * (1.0 - wet) + yl * wg);
      R[i] = static_cast<float>(xr * (1.0 - wet) + yr * wg);
    }
  }
};

// ---- Volley: two delays (left time, right time x ratio) with cross feedback ----
class Volley final : public MiniFx {
  Ring d_[2];
  Tilt tone_[2];
  Smooth t_[2], mix_;

 public:
  Volley() {
    for (auto& d : d_) d.init(static_cast<size_t>(2.6 * kBufRate));
  }

 private:
  void clear() override {
    for (int c = 0; c < 2; ++c) {
      d_[c].clear();
      tone_[c].reset();
      t_[c].setRate(sr_, 60);
      t_[c].init = false;
    }
    mix_.setRate(sr_);
    mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double tl = pv(s, volley::Time) * 0.001 * sr_, tr = tl * pv(s, volley::Ratio);
    const double target[2] = {std::clamp(tl, 1.0, d_[0].maxDelay()), std::clamp(tr, 1.0, d_[1].maxDelay())};
    const double fb = std::clamp(static_cast<double>(pv(s, volley::Feedback)), 0.0, 1.0);
    const double cross = std::clamp(static_cast<double>(pv(s, volley::Cross)), 0.0, 1.0), mT = pv(s, volley::Mix);
    for (auto& t : tone_) t.set(sr_, pv(s, volley::Tone));
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mT);
      const double yl = d_[0].read(t_[0].next(target[0])), yr = d_[1].read(t_[1].next(target[1]));
      const double xl = L[i], xr = R[i];
      d_[0].push(flush(static_cast<float>(tone_[0].process(xl + fb * ((1.0 - cross) * yl + cross * yr)))));
      d_[1].push(flush(static_cast<float>(tone_[1].process(xr + fb * ((1.0 - cross) * yr + cross * yl)))));
      L[i] = static_cast<float>(xl * (1.0 - wet) + yl * wet);
      R[i] = static_cast<float>(xr * (1.0 - wet) + yr * wet);
    }
  }
};

// ---- Vault: eight-line feedback delay network reverb ---------------------------
class Vault final : public MiniFx {
  static constexpr int kLines = 8;
  static constexpr double kBase[kLines] = {1051, 1213, 1399, 1559, 1747, 1913, 2087, 2269};  // samples at 48 kHz
  static constexpr double kEarlyMs[6] = {7, 13, 19, 29, 37, 47};
  Ring line_[kLines], pre_[2];
  dsp::OnePole damp_[kLines];
  Smooth mix_;

 public:
  Vault() {
    for (auto& l : line_) l.init(static_cast<size_t>(2269.0 * 1.5 * kBufRate / 48000.0) + 8);
    for (auto& p : pre_) p.init(static_cast<size_t>(0.28 * kBufRate));
  }

 private:
  void clear() override {
    for (auto& l : line_) l.clear();
    for (auto& p : pre_) p.clear();
    for (auto& d : damp_) d.z = 0;
    mix_.setRate(sr_);
    mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double size = std::clamp(static_cast<double>(pv(s, vault::Size)), 0.0, 1.0);
    const double decay = std::max(0.1, static_cast<double>(pv(s, vault::Decay)));
    const double scale = (0.35 + 1.15 * size) * sr_ / 48000.0;
    const double dampHz = 20000.0 * std::pow(0.05, std::clamp(static_cast<double>(pv(s, vault::Damp)), 0.0, 1.0));
    double len[kLines], g[kLines];
    for (int k = 0; k < kLines; ++k) {
      len[k] = std::clamp(kBase[k] * scale, 8.0, line_[k].maxDelay());
      g[k] = std::pow(10.0, -3.0 * len[k] / (sr_ * decay));
      damp_[k].setFreq(sr_, dampHz);
    }
    const double pd = std::clamp(pv(s, vault::PreDelay) * 0.001 * sr_, 1.0, pre_[0].maxDelay() * 0.7);
    double early[6];
    for (int j = 0; j < 6; ++j) early[j] = std::min(pre_[0].maxDelay(), pd + kEarlyMs[j] * (0.5 + size) * 0.001 * sr_);
    const double width = std::clamp(static_cast<double>(pv(s, vault::Width)), 0.0, 1.0);
    const double er = pv(s, vault::Early), mT = pv(s, vault::Mix);
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mT);
      pre_[0].push(L[i]);
      pre_[1].push(R[i]);
      const double inL = pre_[0].read(pd + 1.0), inR = pre_[1].read(pd + 1.0);
      double o[kLines], sum = 0;
      for (int k = 0; k < kLines; ++k) {
        o[k] = damp_[k].process(line_[k].read(len[k])) * g[k];
        sum += o[k];
      }
      double wl = 0, wr = 0;
      for (int k = 0; k < kLines; ++k) {
        const double v = o[k] - 0.25 * sum + 0.35 * ((k & 1) ? inR : inL) * ((k & 2) ? -1.0 : 1.0);
        line_[k].push(flush(static_cast<float>(v)));
        ((k & 1) ? wr : wl) += o[k] * ((k & 4) ? -1.0 : 1.0);
      }
      double el = 0, erR = 0;
      for (int j = 0; j < 6; ++j) {
        const double gj = 0.5 / (1.0 + j);
        (j & 1 ? erR : el) += gj * pre_[j & 1].read(early[j] + 1.0);
      }
      wl = 0.35 * wl + er * el;
      wr = 0.35 * wr + er * erR;
      const double M = 0.5 * (wl + wr), S = 0.5 * (wl - wr) * width;
      L[i] = static_cast<float>(L[i] * (1.0 - wet) + (M + S) * wet);
      R[i] = static_cast<float>(R[i] * (1.0 - wet) + (M - S) * wet);
    }
  }
};

// ---- Rewind: plays each segment of the input backwards -------------------------
class Rewind final : public MiniFx {
  Ring d_[2];
  BeatClock clock_;
  double pos_ = 0;
  Smooth mix_;

 public:
  Rewind() {
    for (auto& d : d_) d.init(static_cast<size_t>(2.7 * kBufRate));
  }

 private:
  void clear() override {
    d_[0].clear();
    d_[1].clear();
    clock_.free = 0;
    pos_ = -1.0;  // the first sample starts a segment
    mix_.setRate(sr_);
    mix_.init = false;
  }
  void run(const PluginState& s, const ProcessContext& c, float* L, float* R, int n) override {
    const bool sync = pb(s, rewind::Sync);
    const double beats = kRewindBeats[std::clamp(pi(s, rewind::Steps), 0, 4)];
    const double maxLen = d_[0].maxDelay() * 0.5 - 4.0;
    const double len = std::clamp(sync ? beatsToSamples(beats, tempoOf(c), sr_) : pv(s, rewind::Length) * 0.001 * sr_, 16.0, maxLen);
    const double fade = std::clamp(static_cast<double>(pv(s, rewind::Fade)), 0.0, 0.5) * len;
    const double mT = pv(s, rewind::Mix);
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mT);
      if (sync) {
        const double b = clock_.at(c, i) / beats;
        pos_ = (b - std::floor(b)) * len;
      } else {
        pos_ += 1.0;
        if (pos_ >= len) pos_ -= len;
      }
      clock_.advance(c);
      double g = 1.0;
      if (fade > 1.0) g = std::min({1.0, pos_ / fade, (len - pos_) / fade});
      float* io[2] = {L + i, R + i};
      for (int ch = 0; ch < 2; ++ch) {
        const double x = *io[ch];
        d_[ch].push(static_cast<float>(x));
        const double y = d_[ch].read(2.0 * pos_ + 2.0) * g;
        *io[ch] = static_cast<float>(x * (1.0 - wet) + y * wet);
      }
    }
  }
};

// ---- Halt: tape stop and spin-up ---------------------------------------------
class Halt final : public MiniFx {
  enum Mode { Running, Stopping, Stopped, Starting, Catching };
  Ring d_[2];
  Mode mode_ = Running;
  double u_ = 0, lag_ = 0, xf_ = 0, speed_ = 1;

 public:
  Halt() {
    for (auto& d : d_) d.init(static_cast<size_t>(4.2 * kBufRate));
  }

 private:
  void clear() override {
    d_[0].clear();
    d_[1].clear();
    mode_ = Running;
    u_ = lag_ = xf_ = 0;
    speed_ = 1;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const bool run = pb(s, halt::Run);
    const double stopN = std::max(1.0, pv(s, halt::StopTime) * 0.001 * sr_);
    const double startN = std::max(1.0, pv(s, halt::StartTime) * 0.001 * sr_);
    const double curve = std::clamp(static_cast<double>(pv(s, halt::Curve)), 0.25, 4.0);
    const double maxLag = d_[0].maxDelay() - 8.0, xfN = 0.01 * sr_;
    for (int i = 0; i < n; ++i) {
      if (!run && (mode_ == Running || mode_ == Starting || mode_ == Catching)) {
        // A stop during the spin-up starts from the speed reached so far.
        u_ = mode_ == Starting ? std::clamp(1.0 - std::pow(speed_, 1.0 / curve), 0.0, 1.0) : 0.0;
        mode_ = Stopping;
      } else if (run && (mode_ == Stopping || mode_ == Stopped)) {
        mode_ = Starting;
        u_ = 0;
        lag_ = 0;
      }
      double gain = 1.0;
      switch (mode_) {
        case Stopping:
          u_ += 1.0 / stopN;
          speed_ = std::pow(std::max(0.0, 1.0 - u_), curve);
          if (u_ >= 1.0) mode_ = Stopped;
          break;
        case Starting:
          u_ += 1.0 / startN;
          speed_ = std::pow(std::min(1.0, u_), curve);
          if (u_ >= 1.0) {
            mode_ = Catching;
            xf_ = 0;
          }
          break;
        case Stopped: speed_ = 0; break;
        default: speed_ = 1; break;
      }
      if (mode_ == Stopping || mode_ == Starting) lag_ = std::min(maxLag, lag_ + (1.0 - speed_));
      if (mode_ == Stopping || mode_ == Starting || mode_ == Stopped) gain = std::min(1.0, speed_ * 8.0);
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const float x = *io[c];
        d_[c].push(x);
        double y;
        if (mode_ == Running) {
          y = x;
        } else if (mode_ == Catching) {
          const double t = std::min(1.0, xf_ / xfN);
          y = (1.0 - t) * d_[c].read(lag_ + 1.0) + t * x;
        } else {
          y = d_[c].read(lag_ + 1.0) * gain;
        }
        *io[c] = static_cast<float>(y);
      }
      if (mode_ == Catching && (xf_ += 1.0) >= xfN) {
        mode_ = Running;
        lag_ = 0;
      }
    }
  }
};

// ---- Pulse: 16-step volume gate ----------------------------------------------
class Pulse final : public MiniFx {
  BeatClock clock_;
  double env_ = 1;
  bool init_ = false;
  Smooth mix_;
  void clear() override {
    clock_.free = 0;
    init_ = false;
    mix_.setRate(sr_);
    mix_.init = false;
  }

 public:
  // Step (0..15) and the fraction through it at a step position, with swing.
  static int stepAt(double stepPos, double swing, double& frac) {
    const double pair = std::floor(stepPos / 2.0);
    const double pp = stepPos - pair * 2.0;
    const double b = 1.0 + 0.5 * std::clamp(swing, 0.0, 1.0);
    const int inPair = pp < b ? 0 : 1;
    frac = inPair == 0 ? pp / b : (pp - b) / (2.0 - b);
    return static_cast<int>(static_cast<long long>(pair * 2.0 + inPair) % pulse::kSteps);
  }

 private:
  void run(const PluginState& s, const ProcessContext& c, float* L, float* R, int n) override {
    bool on[pulse::kSteps];
    for (int k = 0; k < pulse::kSteps; ++k) on[k] = pb(s, pulse::Step0 + k);
    const double stepBeats = kPulseBeats[std::clamp(pi(s, pulse::Rate), 0, 4)];
    const double att = coefFor(sr_, pv(s, pulse::Attack)), rel = coefFor(sr_, pv(s, pulse::Release));
    const double floorDb = pv(s, pulse::Floor), fg = floorDb <= -59.9 ? 0.0 : dbg(floorDb);
    const double swing = pv(s, pulse::Swing), mT = pv(s, pulse::Mix);
    if (!init_) {
      env_ = fg;
      init_ = true;
    }
    for (int i = 0; i < n; ++i) {
      const double wet = mix_.next(mT);
      double frac;
      const int step = stepAt(std::max(0.0, clock_.at(c, i)) / stepBeats, swing, frac);
      clock_.advance(c);
      const double target = on[step] && frac < 0.9 ? 1.0 : fg;
      env_ = flushd(target + (env_ - target) * (target > env_ ? att : rel));
      const double g = 1.0 - wet + wet * env_;
      L[i] = static_cast<float>(L[i] * g);
      R[i] = static_cast<float>(R[i] * g);
    }
    meter_ = static_cast<float>(todb(env_));
  }
};

}  // namespace

std::unique_ptr<Effect> createTime(const std::string& type) {
  if (type == "gl.miniecho") return std::make_unique<Echo>();
  if (type == "gl.minivolley") return std::make_unique<Volley>();
  if (type == "gl.minivault") return std::make_unique<Vault>();
  if (type == "gl.minirewind") return std::make_unique<Rewind>();
  if (type == "gl.minihalt") return std::make_unique<Halt>();
  if (type == "gl.minipulse") return std::make_unique<Pulse>();
  return nullptr;
}

}  // namespace gl::packs::mini::rt

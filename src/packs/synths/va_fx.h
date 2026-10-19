// Gridline — "synths" pack, J-22 group "va": effect blocks of the reference-
// architecture synths (stereo modulated delay with low / high cut in the
// loop, chorus, phaser, plate-style reverb) and small voice helpers. All
// buffers are allocated in constructors; process() never allocates.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/Dsp.h"
#include "synths_common.h"

namespace gl::packs::synths::va {

// Fractional read from a power-of-two ring buffer.
inline float ringRead(const std::vector<float>& b, int w, double delay) {
  const int mask = static_cast<int>(b.size()) - 1;
  double rp = static_cast<double>(w) - delay;
  const double len = static_cast<double>(b.size());
  while (rp < 0) rp += len;
  int i0 = static_cast<int>(rp);
  double f = rp - i0;
  int i1 = (i0 + 1) & mask;
  i0 &= mask;
  return static_cast<float>(b[static_cast<size_t>(i0)] * (1 - f) + b[static_cast<size_t>(i1)] * f);
}

// Stereo delay: independent left / right times, cross-free feedback through a
// low-cut / high-cut pair, slow time modulation (depth), wet level.
class StereoDelay {
 public:
  StereoDelay() {
    for (auto& b : buf_) b.assign(kLen, 0.f);
  }
  // Times in seconds; fb 0..~0.95; depth 0..1; cuts in Hz; wet 0..1.
  void set(double sr, bool on, double tl, double tr, double fb, double depth, double loCut, double hiCut, double wet) {
    if (on && !on_) clear();
    on_ = on;
    sr_ = sr;
    tgt_[0] = std::clamp(tl * sr, 1.0, kLen - 8.0);
    tgt_[1] = std::clamp(tr * sr, 1.0, kLen - 8.0);
    fb_ = std::clamp(fb, 0.0, 0.97);
    depth_ = std::clamp(depth, 0.0, 1.0);
    wet_ = wet;
    for (int c = 0; c < 2; ++c) {
      lo_[c].setFreq(sr, std::clamp(loCut, 10.0, sr * 0.45));
      hi_[c].setFreq(sr, std::clamp(hiCut, 50.0, sr * 0.45));
    }
  }
  bool on() const { return on_; }
  void process(float* L, float* R, int n) {
    if (!on_) return;
    const double inc = 0.35 / sr_;
    for (int i = 0; i < n; ++i) {
      ph_ += inc;
      if (ph_ >= 1) ph_ -= 1;
      float in[2] = {L[i], R[i]};
      float wetS[2];
      for (int c = 0; c < 2; ++c) {
        cur_[c] += (tgt_[c] - cur_[c]) * 0.0004;
        const double mod = depth_ * 0.004 * sr_ * (0.5 + 0.5 * std::sin(dsp::kTwoPi * (ph_ + 0.25 * c)));
        float d = ringRead(buf_[c], w_, cur_[c] + mod);
        // Loop filters: high cut (one-pole low pass) then low cut (one-pole high pass).
        double h = hi_[c].process(d);
        double l = h - lo_[c].process(h);
        buf_[c][static_cast<size_t>(w_)] = static_cast<float>(in[c] + l * fb_);
        wetS[c] = static_cast<float>(l);
      }
      w_ = (w_ + 1) & (kLen - 1);
      L[i] += static_cast<float>(wetS[0] * wet_);
      R[i] += static_cast<float>(wetS[1] * wet_);
    }
  }
  void clear() {
    for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.f);
    for (int c = 0; c < 2; ++c) lo_[c].z = hi_[c].z = 0;
  }

 private:
  static constexpr int kLen = 1 << 19;  // 2.7 s at 192 kHz
  std::vector<float> buf_[2];
  dsp::OnePole lo_[2], hi_[2];
  int w_ = 0;
  bool on_ = false;
  double sr_ = 44100, tgt_[2] = {1000, 1000}, cur_[2] = {1000, 1000}, fb_ = 0, depth_ = 0, wet_ = 0, ph_ = 0;
};

// Two-voice stereo chorus (quadrature LFOs).
class Chorus {
 public:
  Chorus() {
    for (auto& b : buf_) b.assign(kLen, 0.f);
  }
  void set(double sr, bool on, double rateHz, double depth, double wet, double dry = 1.0) {
    if (on && !on_)
      for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.f);
    on_ = on;
    sr_ = sr;
    rate_ = rateHz;
    depth_ = std::clamp(depth, 0.0, 1.0);
    wet_ = wet;
    dry_ = dry;
  }
  void process(float* L, float* R, int n) {
    if (!on_) return;
    const double inc = rate_ / sr_, base = 0.006 * sr_, span = (0.0004 + 0.006 * depth_) * sr_;
    for (int i = 0; i < n; ++i) {
      buf_[0][static_cast<size_t>(w_)] = L[i];
      buf_[1][static_cast<size_t>(w_)] = R[i];
      const double s0 = std::sin(dsp::kTwoPi * ph_), s1 = std::sin(dsp::kTwoPi * (ph_ + 0.25));
      float a = ringRead(buf_[0], w_, base + span * (0.5 + 0.5 * s0));
      float b = ringRead(buf_[1], w_, base + span * (0.5 + 0.5 * s1));
      L[i] = static_cast<float>(L[i] * dry_ + a * wet_);
      R[i] = static_cast<float>(R[i] * dry_ + b * wet_);
      w_ = (w_ + 1) & (kLen - 1);
      ph_ += inc;
      if (ph_ >= 1) ph_ -= 1;
    }
  }

 private:
  static constexpr int kLen = 8192;
  std::vector<float> buf_[2];
  int w_ = 0;
  bool on_ = false;
  double sr_ = 44100, rate_ = 0.5, depth_ = 0.5, wet_ = 0.5, dry_ = 1.0, ph_ = 0;
};

// Six-stage stereo phaser around a centre frequency.
class Phaser {
 public:
  void set(double sr, bool on, double centreHz, double rateHz, double depth, double fb, double mix) {
    if (on && !on_) reset();
    on_ = on;
    sr_ = sr;
    centre_ = centreHz;
    rate_ = rateHz;
    depth_ = std::clamp(depth, 0.0, 1.0);
    fb_ = std::clamp(fb, 0.0, 0.95);
    mix_ = std::clamp(mix, 0.0, 1.0);
  }
  void process(float* L, float* R, int n) {
    if (!on_) return;
    float* io[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      ph_ += rate_ / sr_;
      if (ph_ >= 1) ph_ -= 1;
      for (int c = 0; c < 2; ++c) {
        const double lfo = std::sin(dsp::kTwoPi * (ph_ + 0.25 * c));
        const double hz = std::clamp(centre_ * std::pow(2.0, 2.5 * depth_ * lfo), 40.0, sr_ * 0.45);
        double y = io[c][i] + last_[c] * fb_;
        for (auto& s : st_[c]) {
          s.setFreq(sr_, hz);
          y = s.process(y);
        }
        last_[c] = std::fabs(y) < 1e-20 ? 0 : y;
        io[c][i] = static_cast<float>(io[c][i] * (1.0 - 0.5 * mix_) + y * mix_ * 0.7);
      }
    }
  }

 private:
  void reset() {
    for (auto& c : st_)
      for (auto& s : c) s.reset();
    last_[0] = last_[1] = 0;
  }
  std::array<std::array<dsp::Allpass1, 6>, 2> st_{};
  double last_[2] = {0, 0};
  bool on_ = false;
  double sr_ = 44100, centre_ = 800, rate_ = 0.5, depth_ = 0.5, fb_ = 0, mix_ = 0.5, ph_ = 0;
};

// Plate-style reverb: four input diffusers into a stereo comb bank with
// damping, decay and a high cut on the wet signal.
class Reverb {
 public:
  Reverb() {
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 8; ++i) comb_[c][i].buf.assign(static_cast<size_t>((kComb[i] + 31) * kMaxScale) + 2, 0.f);
      for (int i = 0; i < 4; ++i) ap_[c][i].buf.assign(static_cast<size_t>((kAp[i] + 31) * kMaxScale) + 2, 0.f);
    }
  }
  // decay 0..1 (room size), hiCut Hz, damp 0..1, mix 0..1.
  void set(double sr, bool on, double decay, double hiCut, double damp, double mix) {
    if (on && !on_) clear();
    on_ = on;
    const double scale = std::min(kMaxScale, sr / 44100.0);
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 8; ++i) comb_[c][i].len = std::max(1, static_cast<int>((kComb[i] + (c ? 31 : 0)) * scale));
      for (int i = 0; i < 4; ++i) ap_[c][i].len = std::max(1, static_cast<int>((kAp[i] + (c ? 31 : 0)) * scale));
      hc_[c].setFreq(sr, std::clamp(hiCut, 200.0, sr * 0.45));
    }
    fb_ = 0.68 + 0.3 * std::clamp(decay, 0.0, 1.0);
    damp_ = 0.05 + 0.6 * std::clamp(damp, 0.0, 1.0);
    mix_ = mix;
  }
  bool on() const { return on_; }
  void process(float* L, float* R, int n) {
    if (!on_) return;
    for (int i = 0; i < n; ++i) {
      const double in = (L[i] + R[i]) * 0.015;
      double out[2] = {0, 0};
      for (int c = 0; c < 2; ++c) {
        for (auto& k : comb_[c]) out[c] += k.run(in, fb_, damp_);
        for (auto& a : ap_[c]) out[c] = a.run(out[c]);
        out[c] = hc_[c].process(out[c]);
      }
      L[i] += static_cast<float>(out[0] * mix_ * 1.4);
      R[i] += static_cast<float>(out[1] * mix_ * 1.4);
    }
  }
  void clear() {
    for (auto& ch : comb_)
      for (auto& k : ch) std::fill(k.buf.begin(), k.buf.end(), 0.f), k.store = 0;
    for (auto& ch : ap_)
      for (auto& a : ch) std::fill(a.buf.begin(), a.buf.end(), 0.f);
    hc_[0].z = hc_[1].z = 0;
  }

 private:
  static constexpr double kMaxScale = 192000.0 / 44100.0;
  static constexpr int kComb[8] = {1123, 1201, 1281, 1361, 1433, 1499, 1567, 1627};
  static constexpr int kAp[4] = {563, 443, 347, 229};
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
  struct Ap {
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
  Comb comb_[2][8];
  Ap ap_[2][4];
  dsp::OnePole hc_[2];
  bool on_ = false;
  double fb_ = 0.84, damp_ = 0.3, mix_ = 0.2;
};

// Linear-segment ADSR with exponential-ish decay / release (times in seconds).
struct Env {
  enum Stage { Idle, Attack, Decay, Sustain, Release };
  Stage st = Idle;
  double v = 0, a = 0.001, d = 0.1, s = 1, r = 0.1, sr = 44100;
  void set(double sampleRate, double at, double dt, double sus, double rt) {
    sr = sampleRate;
    a = std::max(at, 0.0005);
    d = std::max(dt, 0.0005);
    s = std::clamp(sus, 0.0, 1.0);
    r = std::max(rt, 0.0005);
  }
  void gate(bool on) {
    if (on)
      st = Attack;
    else if (st != Idle)
      st = Release;
  }
  bool active() const { return st != Idle; }
  double next() {
    switch (st) {
      case Idle: return 0;
      case Attack:
        v += 1.0 / (a * sr);
        if (v >= 1) v = 1, st = Decay;
        break;
      case Decay: {
        const double k = std::exp(-4.6 / (d * sr));  // -40 dB over the decay time
        v = s + (v - s) * k;
        if (v - s < 1e-4) v = s, st = Sustain;
        break;
      }
      case Sustain: v = s; break;
      case Release: {
        const double k = std::exp(-6.9 / (r * sr));  // -60 dB over the release time
        v *= k;
        if (v < 1e-5) v = 0, st = Idle;
        break;
      }
    }
    return v;
  }
};

// Tempo divisions (in 16th notes) shared by the group's sync controls.
inline double sixteenthsToHz(double sixteenths, double tempo) {
  return std::max(20.0, tempo) / 60.0 * 4.0 / std::max(0.0625, sixteenths);
}

// MIDI CC links of a reference state ("cc <tag> <controller>" lines in
// PluginState::data), prepared on the UI thread.
struct CcLinks : AuxData {
  struct L {
    uint8_t cc, tag;
  };
  std::array<L, 128> l{};
  int n = 0;
};
inline std::shared_ptr<const AuxData> parseCcLinks(const std::string& data) {
  auto c = std::make_shared<CcLinks>();
  size_t at = 0;
  while (at < data.size() && c->n < static_cast<int>(c->l.size())) {
    size_t nl = data.find('\n', at);
    if (nl == std::string::npos) nl = data.size();
    int tag = -1, cc = -1;
    if (data.compare(at, 3, "cc ") == 0 && std::sscanf(data.c_str() + at + 3, "%d %d", &tag, &cc) == 2 && tag >= 0 && tag < 256 && cc >= 0 &&
        cc < 128)
      c->l[static_cast<size_t>(c->n++)] = {static_cast<uint8_t>(cc), static_cast<uint8_t>(tag)};
    at = nl + 1;
  }
  return c;
}

// Parameter ranges of a catalogue type (for normalized modulation / CC links).
template <size_t N>
inline void paramRanges(const char* type, std::array<float, N>& lo, std::array<float, N>& hi) {
  for (size_t k = 0; k < N; ++k) lo[k] = 0.f, hi[k] = 1.f;
  if (const PluginInfo* info = findPlugin(type))
    for (size_t k = 0; k < N && k < info->params.size(); ++k) {
      lo[k] = static_cast<float>(info->params[k].min);
      hi[k] = static_cast<float>(info->params[k].max);
    }
}

// Clamped parameter read.
inline float pv(const PluginState& s, int i, float d = 0.f) { return prm(s, i, d); }

}  // namespace gl::packs::synths::va

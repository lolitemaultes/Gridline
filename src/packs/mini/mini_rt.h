// Gridline — "mini" pack: realtime building blocks shared by the mini_*_dsp.cpp
// files. Buffers are sized in constructors; nothing here allocates afterwards.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/Dsp.h"
#include "../../engine/Effects.h"
#include "../../engine/pitch/Kernel.h"
#include "mini.h"

namespace gl::packs::mini::rt {

using dsp::kPi;
using dsp::kTwoPi;

// Delay buffers are sized for this rate; at higher rates the longest times shrink.
inline constexpr double kBufRate = 96000.0;

inline float pv(const PluginState& s, int k) {
  if (k >= 0 && static_cast<size_t>(k) < s.params.size()) return s.params[static_cast<size_t>(k)];
  const PluginInfo* info = findPlugin(s.type);
  return info && k >= 0 && static_cast<size_t>(k) < info->params.size() ? static_cast<float>(info->params[static_cast<size_t>(k)].def)
                                                                           : 0.f;
}
inline int pi(const PluginState& s, int k) { return static_cast<int>(std::lround(pv(s, k))); }
inline bool pb(const PluginState& s, int k) { return pv(s, k) >= 0.5f; }
inline double dbg(double db) { return std::pow(10.0, db / 20.0); }
inline double todb(double g) { return g > 1e-6 ? 20.0 * std::log10(g) : -120.0; }
inline float flush(float x) { return std::fabs(x) < 1e-20f ? 0.f : x; }
inline double flushd(double x) { return std::fabs(x) < 1e-20 ? 0.0 : x; }
inline double coefFor(double sr, double ms) { return std::exp(-1.0 / std::max(1.0, ms * 0.001 * sr)); }
// Beats of one tempo-synced value -> samples.
inline double beatsToSamples(double beats, double tempo, double sr) { return beats * 60.0 / std::max(10.0, tempo) * sr; }
// The host tempo; 120 BPM when the host gives none (a plugin host without a transport).
inline double tempoOf(const ProcessContext& c) { return c.tempo > 0 ? c.tempo : 120.0; }

// Per-sample parameter smoother (about 10 ms); snaps on the first value.
struct Smooth {
  double v = 0, a = 0.995;
  bool init = false;
  void setRate(double sr, double ms = 10.0) { a = coefFor(sr, ms); }
  void snap(double x) {
    v = x;
    init = true;
  }
  double next(double target) {
    if (!init) snap(target);
    v = target + (v - target) * a;
    return v;
  }
};

// Circular delay line with a fixed power-of-two capacity (allocated once).
struct Ring {
  std::vector<float> b;
  size_t mask = 0, w = 0;
  void init(size_t minSize) {
    size_t n = 16;
    while (n < minSize) n <<= 1;
    b.assign(n, 0.f);
    mask = n - 1;
    w = 0;
  }
  void clear() {
    std::fill(b.begin(), b.end(), 0.f);
    w = 0;
  }
  double maxDelay() const { return static_cast<double>(b.size()) - 4.0; }
  void push(float x) {
    b[w & mask] = x;
    ++w;
  }
  float at(size_t d) const { return b[(w - d) & mask]; }  // d >= 1: written d samples ago
  // Fractional read (cubic Hermite), d clamped to [1, size - 4].
  float read(double d) const {
    d = std::clamp(d, 1.0, maxDelay());
    size_t i = static_cast<size_t>(d);
    float f = static_cast<float>(d - static_cast<double>(i));
    float xm1 = at(i > 1 ? i - 1 : 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
  }
  float readLinear(double d) const {
    d = std::clamp(d, 1.0, maxDelay());
    size_t i = static_cast<size_t>(d);
    float f = static_cast<float>(d - static_cast<double>(i));
    return at(i) + (at(i + 1) - at(i)) * f;
  }  // J-140: band-limited read with the head moving `rate` samples per sample
  // (the shared kernel), d clamped like read().
  float band(double d, double rate, const pitch::SincTable& t) const {
    return pitch::readBack(b.data(), mask, w, std::clamp(d, 1.0, maxDelay()), rate, t);
  }
};

// Tilt in a feedback path: < 0 darkens (low pass), > 0 thins (high pass), 0 = flat.
struct Tilt {
  dsp::OnePole lp, hp;
  double amt = 0;
  void set(double sr, double tone) {
    amt = std::clamp(tone, -1.0, 1.0);
    lp.setFreq(sr, 20000.0 * std::pow(800.0 / 20000.0, std::max(0.0, -amt)));
    hp.setFreq(sr, 20.0 * std::pow(2000.0 / 20.0, std::max(0.0, amt)));
  }
  double process(double x) {
    if (amt < 0) return lp.process(x);
    if (amt > 0) return x - hp.process(x);
    return x;
  }
  void reset() { lp.z = hp.z = 0; }
};

// Base of every Mini effect: tracks the sample rate, keeps the side buffers and meters.
class MiniFx : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& c, float* L, float* R, int n) final {
    if (c.sampleRate != sr_) {
      sr_ = c.sampleRate > 0 ? c.sampleRate : 48000.0;
      clear();
    }
    run(s, c, L, R, n);
  }
  void reset() override { clear(); }
  void setSideIo(const SideIo& io) override { io_ = io; }
  float meter() const override { return meter_; }
  float inputLevel() const override { return input_; }

 protected:
  virtual void run(const PluginState& s, const ProcessContext& c, float* L, float* R, int n) = 0;
  virtual void clear() = 0;
  double sr_ = 0;
  SideIo io_;
  float meter_ = 0, input_ = -120;
};

// Peak follower (dB domain helpers for the dynamics family).
struct Follower {
  double env = 0, att = 0, rel = 0;
  void set(double sr, double attMs, double relMs) {
    att = coefFor(sr, attMs);
    rel = coefFor(sr, relMs);
  }
  double process(double x) {
    double a = x > env ? att : rel;
    env = x + (env - x) * a;
    return env;
  }
};

// Free-running or transport-locked position in beats for sample i of a block.
struct BeatClock {
  double free = 0;  // beats, advanced every sample
  double at(const ProcessContext& c, int i) const {
    if (c.playing) return c.tick / std::max(1, c.ppq) + i * tempoOf(c) / 60.0 / c.sampleRate;
    return free;
  }
  void advance(const ProcessContext& c) { free += tempoOf(c) / 60.0 / c.sampleRate; }
};

// Family factories (one per mini_<family>_dsp.cpp); nullptr for other types.
std::unique_ptr<Effect> createUtility(const std::string& type);
std::unique_ptr<Effect> createTone(const std::string& type);
std::unique_ptr<Effect> createDynamics(const std::string& type);
std::unique_ptr<Effect> createDrive(const std::string& type);
std::unique_ptr<Effect> createMotion(const std::string& type);
std::unique_ptr<Effect> createTime(const std::string& type);

}  // namespace gl::packs::mini::rt

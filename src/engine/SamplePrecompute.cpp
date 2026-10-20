// Gridline — sampler precomputed sample effects (J-23).
//
// Order and laws (reference sampler, "legacy FX" stage; N = frames − 1,
// n = frame, raw values in brackets are the reference knob units):
//   1. stereo delay [0..4096, 2048 = off]: one channel starts |v − 2048|
//      frames at 44.1 kHz later (v > 2048: left), the sample grows by that much.
//   2. per channel, per frame:
//      time warp ("pogo") [0..512, 256 = off], a = |v − 256| / 256: read
//        position p = a·w(n) + (1 − a)·n with w(n) = N·sin(n/N · π/2) above 256
//        and N + N·asin(n/N − 1)·2/π below; 4-point Hermite read of the
//        stereo-delayed copy;
//      ring modulation [mix 0..256, freq 0..256]: x·(1 − m) + m·x·sin(φ),
//        φ advancing (f + 8)·44100 / 5760 Hz, starting at 90° per channel;
//      low pass [cutoff 16..1024, 1024 = off; resonance 0..640]: two-state
//        filter v += c·(x − y), y += v, v ·= d with
//        c = (e^(K·cut/1024) − 1) / 600 · 44100 / sr,
//        d = 1 − (e^(K·(1024 − res)/1024) − 1) / 600, K = ln 601;
//      ramp in [0..1024]: × max(0, 1 − v/256 · (N − n)/N);
//      ramp out [0..1024]: × max(0, 1 − v/256 · n/N);
//      boost [0..256]: × (1 + v/16), then optional clip to ±1;
//   3. tilt [0..256, 128 = off]: y[n] = x[n] + k·y[n−1],
//      k = (⌊192·v/256⌋ − 96) / 128;
//   4. stereo fade: left × cos(n/N · π/2), right × sin(n/N · π/2);
//   5. reverb [mix 0..256, type]: Gridline's own tail (the reference algorithm
//      was not reproduced), trailing frames below 1/512 trimmed.
#include "SamplePrecompute.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"

namespace gl {

namespace {
constexpr double kPi = 3.14159265358979323846;
double par(const PluginState& st, int k) {
  if (k >= 0 && static_cast<size_t>(k) < st.params.size()) return st.params[static_cast<size_t>(k)];
  const PluginInfo* info = findPlugin(st.type);
  return info && static_cast<size_t>(k) < info->params.size() ? info->params[static_cast<size_t>(k)].def : 0.0;
}
}  // namespace

bool SamplePrecomputeFx::neutral() const {
  return rampIn <= 0 && rampOut <= 0 && boost <= 0 && std::fabs(tilt) < 1.0 / 256 && cutoff >= 1 && ringMix <= 0 &&
         std::fabs(stereoDelayMs) < 1e-3 && std::fabs(warp) < 1.0 / 512 && reverbMix <= 0 && !fadeStereo;
}

std::string SamplePrecomputeFx::key() const {
  char b[320];
  std::snprintf(b, sizeof b, "|pre:%.6g,%.6g,%.6g,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%d,%d", rampIn, rampOut, boost, clip ? 1 : 0, tilt,
                cutoff, reso, ringMix, ringHz, stereoDelayMs, warp, reverbMix, reverbType, fadeStereo ? 1 : 0);
  return b;
}

SamplePrecomputeFx samplePrecomputeFx(const PluginState& st) {
  using namespace pp;
  SamplePrecomputeFx f;
  if (st.type != "gl.sampler") return f;
  f.rampIn = std::clamp(par(st, SmpPreRampIn), 0.0, 4.0);
  f.rampOut = std::clamp(par(st, SmpPreRampOut), 0.0, 4.0);
  f.boost = std::clamp(par(st, SmpPreBoost), 0.0, 1.0);
  f.clip = par(st, SmpPreClip) >= 0.5;
  f.tilt = std::clamp(par(st, SmpPreTilt), -1.0, 1.0);
  f.cutoff = std::clamp(par(st, SmpPreCutoff), 1.0 / 64, 1.0);
  f.reso = std::clamp(par(st, SmpPreReso), 0.0, 1.0);
  f.ringMix = std::clamp(par(st, SmpPreRingMix), 0.0, 1.0);
  f.ringHz = std::clamp(par(st, SmpPreRingFreq), 61.25, 2021.25);
  f.stereoDelayMs = std::clamp(par(st, SmpPreStereoDelay), -46.44, 46.44);
  f.warp = std::clamp(par(st, SmpPreWarp), -1.0, 1.0);
  f.reverbMix = std::clamp(par(st, SmpPreReverbMix), 0.0, 1.0);
  f.reverbType = static_cast<int>(std::lround(par(st, SmpPreReverbType))) ? 1 : 0;
  f.fadeStereo = par(st, SmpPreFadeStereo) >= 0.5;
  return f;
}

namespace {

// Own reverb tail: four damped combs into two allpasses per channel.
void reverbTail(std::vector<float>& ch, double sr, int type, double mix, int seedOfs) {
  static const double combMs[4] = {29.7, 37.1, 41.1, 43.7}, apMs[2] = {5.0, 1.7};
  const double t60 = type ? 2.4 : 0.9;
  const size_t n0 = ch.size();
  const size_t tail = static_cast<size_t>(t60 * sr);
  std::vector<float> in(ch);
  in.resize(n0 + tail, 0.f);
  std::vector<double> out(n0 + tail, 0.0);
  for (int c = 0; c < 4; ++c) {
    const size_t d = static_cast<size_t>((combMs[c] + seedOfs * 0.37) * 0.001 * sr * (type ? 1.4 : 1.0)) + 1;
    const double g = std::pow(10.0, -3.0 * static_cast<double>(d) / sr / t60);
    std::vector<double> buf(d, 0.0);
    double lp = 0;
    size_t w = 0;
    for (size_t i = 0; i < out.size(); ++i) {
      const double y = buf[w];
      lp = y * 0.7 + lp * 0.3;
      buf[w] = in[i] + lp * g;
      w = w + 1 == d ? 0 : w + 1;
      out[i] += y * 0.25;
    }
  }
  for (int a = 0; a < 2; ++a) {
    const size_t d = static_cast<size_t>(apMs[a] * 0.001 * sr) + 1;
    std::vector<double> buf(d, 0.0);
    size_t w = 0;
    for (double& x : out) {
      const double b = buf[w];
      const double v = x + 0.5 * b;
      buf[w] = v;
      x = b - 0.5 * v;
      w = w + 1 == d ? 0 : w + 1;
    }
  }
  ch.resize(n0 + tail, 0.f);
  for (size_t i = 0; i < ch.size(); ++i) ch[i] = static_cast<float>(ch[i] + mix * out[i]);
}

}  // namespace

SamplePtr precomputeSample(const SamplePtr& in, const SamplePrecomputeFx& fx) {
  if (!in || fx.neutral() || in->frames() < 4) return in;
  auto s = std::make_shared<Sample>(*in);
  const double sr = s->sampleRate > 0 ? s->sampleRate : 44100.0;
  const double f8 = 44100.0 / sr;  // reference: 44100 / sample rate
  const bool mono = s->left == s->right;
  const int nch = mono ? 1 : 2;
  // 1. Stereo delay.
  {
    const int raw = static_cast<int>(std::lround(2048.0 + fx.stereoDelayMs * 44.1));
    if (raw != 2048) {
      const int ofs = static_cast<int>(std::lround(std::abs(raw - 2048) / f8));
      const size_t n = s->frames();
      std::vector<float> L(n + static_cast<size_t>(ofs), 0.f), R(n + static_cast<size_t>(ofs), 0.f);
      const size_t oL = raw > 2048 ? static_cast<size_t>(ofs) : 0, oR = raw < 2048 ? static_cast<size_t>(ofs) : 0;
      for (size_t i = 0; i < n; ++i) L[i + oL] = s->left[i], R[i + oR] = s->right[i];
      s->left.swap(L), s->right.swap(R);
    }
  }
  const bool stereoNow = !mono || std::fabs(fx.stereoDelayMs) >= 1e-3;
  const int chans = stereoNow ? 2 : nch;
  const long N = static_cast<long>(s->frames()) - 1;
  // 2. Per-frame stage.
  const int pogo = static_cast<int>(std::lround(256.0 + fx.warp * 256.0));
  const double a = std::abs(pogo - 256) / 256.0;
  const double mix = std::round(fx.ringMix * 256.0) / 256.0;
  const double ringStep = fx.ringHz / sr * 16384.0;  // table steps per frame
  const int cutRaw = static_cast<int>(std::lround(fx.cutoff * 1024.0));
  const int resRaw = static_cast<int>(std::lround(fx.reso * 640.0));
  const bool lp = cutRaw < 1024 || resRaw > 0;
  const double K = std::log(601.0);
  const double c = (std::exp(cutRaw / 1024.0 * K) - 1.0) / 600.0 * f8;
  const double damp = 1.0 - (std::exp((1024 - resRaw) / 1024.0 * K) - 1.0) / 600.0;
  const int rampInRaw = static_cast<int>(std::lround(fx.rampIn * 256.0)), rampOutRaw = static_cast<int>(std::lround(fx.rampOut * 256.0));
  const int boostRaw = static_cast<int>(std::lround(fx.boost * 256.0));
  const double boostGain = boostRaw * 0.0625 + 1.0;
  for (int ch = 0; ch < chans; ++ch) {
    std::vector<float>& x = ch ? s->right.mut() : s->left;
    const std::vector<float> src = x;  // warp reads the unwarped copy
    double phase = static_cast<double>(ch) * 4096.0;
    double fv = 0, fy = 0;
    for (long n = 0; n <= N; ++n) {
      double v = x[static_cast<size_t>(n)];
      if (pogo != 256) {
        double w;
        if (pogo < 256) w = static_cast<double>(N) * std::asin(static_cast<double>(n) / N - 1.0) * 0.6366197723675814 + N;
        else w = static_cast<double>(N) * std::sin(static_cast<double>(n) / N * 1.5707963267948966);
        const double p = a * w + (1.0 - a) * n;
        const long i = static_cast<long>(p);
        const double f = p - static_cast<double>(i);
        auto at = [&](long k) { return k >= 0 && k <= N ? static_cast<double>(src[static_cast<size_t>(k)]) : 0.0; };
        const double xm1 = i < 1 ? 0.0 : at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
        const double A = ((x0 - x1) * 3.0 - xm1 + x2) * 0.5, B = (x1 * 2.0 + xm1) - (x0 * 5.0 + x2) * 0.5, C = (x1 - xm1) * 0.5;
        v = ((A * f + B) * f + C) * f + x0;
      }
      if (mix > 0) {
        const double ring = std::sin(2.0 * kPi * std::floor(phase) / 16384.0) * v * mix;
        phase += ringStep;
        while (phase >= 16384.0) phase -= 16384.0;
        v = (1.0 - mix) * v + ring;
      }
      if (lp) {
        fv = c * (v - fy) + fv;
        fy += fv;
        fv *= damp;
        v = fy;
      }
      if (rampInRaw > 0) v *= std::max(0.0, 1.0 - rampInRaw * static_cast<double>(N - n) / N / 256.0);
      if (rampOutRaw > 0) v *= std::max(0.0, 1.0 - rampOutRaw * static_cast<double>(n) / N / 256.0);
      if (boostRaw > 0) v *= boostGain;
      if (fx.clip) v = std::clamp(v, -1.0, 1.0);
      x[static_cast<size_t>(n)] = static_cast<float>(v);
    }
  }
  // 3. Tilt.
  const int tiltRaw = std::clamp(static_cast<int>(std::lround(128.0 + fx.tilt * 128.0)), 0, 256);
  if (tiltRaw != 128) {
    const double k = static_cast<double>((tiltRaw * 192) / 256 - 96) * 0.0078125;
    for (int ch = 0; ch < chans; ++ch) {
      std::vector<float>& x = ch ? s->right.mut() : s->left;
      double prev = 0;
      for (float& v : x) prev = v = static_cast<float>(v + prev * k);
    }
  }
  if (chans == 1) s->setMono();  // J-30: one copy of mono material
  // 4. Stereo fade.
  if (fx.fadeStereo && N > 0) {
    const double step = 1.5707963267948966 / static_cast<double>(N);
    std::vector<float>& right = s->right.mut();  // its own copy before the left channel changes
    for (long n = 0; n <= N; ++n) {
      s->left[static_cast<size_t>(n)] = static_cast<float>(s->left[static_cast<size_t>(n)] * std::cos(n * step));
      right[static_cast<size_t>(n)] = static_cast<float>(right[static_cast<size_t>(n)] * std::cos((N - n) * step));
    }
  }
  // 5. Reverb (own algorithm) and tail trim.
  if (fx.reverbMix > 0) {
    const size_t n0 = s->frames();
    std::vector<float>& right = s->right.mut();  // its own copy before the left channel changes
    reverbTail(s->left, sr, fx.reverbType, fx.reverbMix, 0);
    reverbTail(right, sr, fx.reverbType, fx.reverbMix, 1);
    size_t n = s->frames();
    while (n > n0 && std::fabs(s->left[n - 1]) < 0.001953125f && std::fabs(s->right[n - 1]) < 0.001953125f) --n;
    s->left.resize(n), s->right.resize(n);
  }
  return s;
}

SamplePtr precomputedSample(const std::string& path, const SamplePrecomputeFx& fx) {
  SamplePtr raw = sampleCache().get(path);
  if (!raw || fx.neutral()) return raw;
  static std::mutex mu;
  static std::map<std::string, std::pair<std::weak_ptr<const Sample>, SamplePtr>> cache;
  const std::string key = path + fx.key();
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(key);
  if (it != cache.end() && it->second.first.lock() == raw) return it->second.second;
  SamplePtr out = precomputeSample(raw, fx);
  cache[key] = {raw, out};
  if (cache.size() > 64)  // drop entries whose source sample is gone
    for (auto i = cache.begin(); i != cache.end();) i = i->second.first.expired() ? cache.erase(i) : std::next(i);
  return out;
}

}  // namespace gl

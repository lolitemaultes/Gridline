// Gridline — "plasma" pack: the oscillators of one note: wavetable (two warp
// slots: phase warps, filters, shapers, FM / PD / AM / RM from the other
// sources), sample, multisample, granular and spectral (STFT resynthesis with
// spectral warps), plus the sub and noise oscillators.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../engine/pitch/Kernel.h"
#include "plasma_dsp.h"

namespace gl::packs::plasma {

namespace {
inline double frac(double x) { return x - std::floor(x); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float remapAt(const std::array<float, 257>& c, double x) {
  const double u = std::clamp(x, 0.0, 1.0) * 256.0;
  const int i = std::min(255, static_cast<int>(u));
  const float f = static_cast<float>(u - i);
  return c[static_cast<size_t>(i)] + (c[static_cast<size_t>(i + 1)] - c[static_cast<size_t>(i)]) * f;
}
inline double bendPow(double p, double a) { return std::pow(std::clamp(p, 0.0, 1.0), 1.0 - 0.85 * a); }
inline int fmSourceOf(int m) {  // 33..69: index of the modulation source (0..5)
  if (m >= 33 && m <= 56) return (m - 33) % 6;
  if (m >= 58 && m <= 69) return (m - 58) % 6;
  return 0;
}
inline int harmLevel(double h) {  // mip level that keeps at most h harmonics
  if (h >= 1024) return 0;
  if (h < 1) return MipTable::kLevels - 1;
  const int l = static_cast<int>(std::ceil(std::log2(1024.0 / h)));
  return std::clamp(l, 0, MipTable::kLevels - 1);
}
// J-140: band-limited sample read (was Catmull-Rom: aliasing when pitched up),
// the compact shared kernel (unison voices); rate in frames per output frame.
inline float sampleAt(const float* d, int64_t n, double pos, double rate) {
  if (n <= 0) return 0;
  return pitch::read(d, n, pos, std::fabs(rate), pitch::table(pitch::Quality::Compact));
}
float window(int shape, double t, float skew, float param) {  // t 0..1 over the grain
  if (skew != 0) {  // skew moves the peak
    const double k = std::pow(2.0, -skew * 2.0);
    t = std::pow(std::clamp(t, 0.0, 1.0), k);
  }
  t = std::clamp(t, 0.0, 1.0);
  switch (shape) {
    case 1: return static_cast<float>(1 - (2 * t - 1) * (2 * t - 1));                                  // welch
    case 2: { const double s = 0.15 + 0.35 * param, x = (t - 0.5) / s; return static_cast<float>(std::exp(-0.5 * x * x)); }
    case 3: return static_cast<float>(0.35875 - 0.48829 * std::cos(kPi2 * t) + 0.14128 * std::cos(2 * kPi2 * t) - 0.01168 * std::cos(3 * kPi2 * t));
    case 4: { const double x = (t - 0.5) * (4 + 8 * param); return static_cast<float>(std::fabs(x) < 1e-6 ? 1.0 : std::sin(3.14159265 * x) / (3.14159265 * x) * (0.5 - 0.5 * std::cos(kPi2 * t))); }
    case 5: {  // tukey
      const double a = 0.1 + 0.8 * param;
      if (t < a / 2) return static_cast<float>(0.5 - 0.5 * std::cos(kPi2 * t / a));
      if (t > 1 - a / 2) return static_cast<float>(0.5 - 0.5 * std::cos(kPi2 * (1 - t) / a));
      return 1.f;
    }
    case 6: return static_cast<float>(1 - std::fabs(2 * t - 1));
    case 7: { const double e = 0.05 + 0.4 * param; return static_cast<float>(std::min({1.0, t / e, (1 - t) / e})); }
    case 8: return static_cast<float>(std::exp(-t * (2 + 10 * param)) * std::min(1.0, t * 200));
    case 9: return static_cast<float>(std::exp(-(1 - t) * (2 + 10 * param)) * std::min(1.0, (1 - t) * 200));
    default: return static_cast<float>(0.5 - 0.5 * std::cos(kPi2 * t));                              // hann
  }
}
}  // namespace

OscVoice::OscVoice()
    : ola_(new float[2 * 2048]()), sphase_(new float[8 * 1024]()), prevMag_(new float[1024]()), work_(new cpx[2 * 2048]),
      fmRing_(new float[2048]()), smag_(new float[1024]()), sfrq_(new float[1024]()), stmp_(new float[1024]()), sy_(new cpx[2048]) {}

void OscVoice::start(const OscSet& s, float phase, float randPhase, float randStart, int phaseMode, const double* memory, uint32_t seed,
                     int key, float vel) {
  rng_ = seed ? seed : 1;
  released_ = finished_ = false;
  key_ = key;
  vel_ = vel;
  lastMono_ = 0;
  for (int u = 0; u < kMaxUnison; ++u) {
    const float rp = randPhase * rngUni(rng_);
    phase_[u] = (phaseMode > 0 && memory) ? memory[u] : frac(static_cast<double>(phase) + rp);
    selfFb_[u] = 0;
    dirU_[u] = 1;
    // granular unison: when each stream starts its first grain (together, evenly spread, exponential, random)
    const int U = std::clamp(s.unison, 1, kMaxUnison);
    switch (s.grTrig) {
      case 0: spawnAcc_[u] = 1.0; break;
      case 1: spawnAcc_[u] = 1.0 - static_cast<double>(u) / U; break;
      case 2: spawnAcc_[u] = 1.0 - (std::exp2(static_cast<double>(u)) - 1.0) / (std::exp2(static_cast<double>(U)) - 1.0); break;
      default: spawnAcc_[u] = u == 0 ? 1.0 : rngUni(rng_); break;
    }
  }
  postZ_[0][0] = postZ_[0][1] = postZ_[1][0] = postZ_[1][1] = 0;
  // sample playheads
  const Sample* smp = s.src ? s.src->sample.get() : nullptr;
  zone_ = -1;
  if (s.type == OscMultisample && s.src) {
    const int k = std::clamp(key + static_cast<int>(std::lround(s.timbre * 12)), 0, 127);
    const int v = std::clamp(static_cast<int>(vel * 127), 0, 127);
    int best = -1, bestD = 1 << 30;
    for (size_t z = 0; z < s.src->zones.size(); ++z) {
      const Zone& zn = s.src->zones[z];
      const int d = (k < zn.loKey ? zn.loKey - k : k > zn.hiKey ? k - zn.hiKey : 0) * 4 + (v < zn.loVel ? zn.loVel - v : v > zn.hiVel ? v - zn.hiVel : 0);
      if (d < bestD) bestD = d, best = static_cast<int>(z);
    }
    zone_ = best;
    if (best >= 0) smp = s.src->zones[static_cast<size_t>(best)].s.get();
  }
  const double len = smp ? static_cast<double>(smp->frames()) : 0.0;
  for (int u = 0; u < kMaxUnison; ++u) {
    double st = std::clamp(static_cast<double>(s.start), 0.0, 1.0) * len;
    if (s.type == OscSample && s.src && !s.src->slices.empty()) {
      const int si = std::clamp(s.slice - 1, 0, static_cast<int>(s.src->slices.size()) - 1);
      st = s.src->slices[static_cast<size_t>(si)];
    }
    if (zone_ >= 0) st = static_cast<double>(s.src->zones[static_cast<size_t>(zone_)].start);
    st += randStart * rngUni(rng_) * len * 0.1 * (u > 0 ? 1 : 0);
    play_[u] = s.reverse ? std::max(0.0, std::clamp(static_cast<double>(s.end), 0.0, 1.0) * len - 1) : st;
  }
  for (auto& g : grains_) g.active = false;
  // spectral
  olaPos_ = 512;
  specPos_ = s.position;
  specStarted_ = false;
  std::fill(ola_.get(), ola_.get() + 4096, 0.f);
  std::fill(prevMag_.get(), prevMag_.get() + 1024, 0.f);
  for (int i = 0; i < 8 * 1024; ++i) sphase_[static_cast<size_t>(i)] = static_cast<float>(rngUni(rng_) * kPi2) * randPhase;
  dispPos_ = s.wtPos;
}

void OscVoice::savePhases(double* out) const {
  for (int u = 0; u < kMaxUnison; ++u) out[u] = phase_[u];
}

// ------------------------------------------------------------------ wavetable
float OscVoice::wavetableSample(const OscSet& s, int u, double& ph, double inc, const float* fm) {
  const MipTable& t = *s.mip;
  double p = ph;
  double mul = 1, incMul = 1;
  float gain = 1;
  bool flip = false;
  const int modes[2] = {s.w1, s.w2};
  float amt[2] = {s.a1 + s.uniW1 * s.uw[u] * 0.5f, s.a2 + s.uniW2 * s.uw[u] * 0.5f};
  const float var[2] = {s.v1, s.v2};
  for (int k = 0; k < 2; ++k) {
    amt[k] = std::clamp(amt[k], 0.f, 1.f);
    const int m = modes[k];
    const float a = amt[k];
    if (m <= 0 || a <= 1e-6f) continue;
    switch (m) {
      case 1: {  // sync
        const double M = 1.0 + a * a * 15.0;
        if (var[k] > 0) gain *= static_cast<float>(1.0 - var[k] * std::pow(p, 3.0));
        p = frac(p * M);
        mul *= M;
        break;
      }
      case 2: p = bendPow(p, a); mul *= 1 + a * 3; break;
      case 3: p = 1.0 - bendPow(1.0 - p, a); mul *= 1 + a * 3; break;
      case 4: p = p < 0.5 ? 0.5 * bendPow(p * 2, a) : 1.0 - 0.5 * bendPow(2 - p * 2, a); mul *= 1 + a * 3; break;
      case 5: {  // PWM: the cycle squeezes into a shorter part, the rest is held at zero
        const double w = 1.0 - a * 0.98, off = var[k] * (1 - w);
        const double q = p - off;
        if (q < 0 || q >= w) {
          gain = 0;
          p = 0;
        } else {
          p = q / w;
        }
        mul *= 1.0 / w;
        break;
      }
      case 6: if (p < 0.5) p = 0.5 * bendPow(p * 2, a); mul *= 1 + a * 2; break;
      case 7: if (p >= 0.5) p = 0.5 + 0.5 * (1.0 - bendPow(2 - p * 2, a)); mul *= 1 + a * 2; break;
      case 8: p = p < 0.5 ? 0.5 * bendPow(p * 2, a) : 0.5 + 0.5 * (1.0 - bendPow(2 - p * 2, a)); mul *= 1 + a * 2; break;
      case 9: flip = p >= 1.0 - a; break;
      case 10: p = p + ((p < 0.5 ? p * 2 : 2 - p * 2) - p) * a; mul *= 1 + a; break;
      case 11: if (s.src) p = frac(p + (remapAt(s.src->remap[0], p) - p) * a); mul *= 1 + a * 2; break;
      case 12:
        if (s.src) {
          const double q = p < 0.5 ? 0.5 * remapAt(s.src->remap[1], p * 2) : 1.0 - 0.5 * remapAt(s.src->remap[1], 2 - p * 2);
          p = frac(p + (q - p) * a);
        }
        mul *= 1 + a * 2;
        break;
      case 14:
        if (s.src) p = frac(p + (frac(remapAt(s.src->remap[3], p) * (1.0 + a * 7.0)) - p) * std::min(1.f, a * 8.f));
        mul *= 1 + a * 7;
        break;
      case 15: {
        const double steps = 2.0 + std::pow(1.0 - a, 3.0) * 1000.0;
        p = std::floor(p * steps) / steps;
        break;
      }
      default:
        if (m >= 33 && m <= 38) {
          incMul *= 1.0 + a * a * 16.0 * fm[fmSourceOf(m)];
          mul *= 1 + a * a * 8;
        } else if (m >= 39 && m <= 44) {
          incMul *= std::exp2(a * 4.0 * fm[fmSourceOf(m)]);
          mul *= std::exp2(a * 2.0);
        } else if (m >= 45 && m <= 50) {
          p = frac(p + a * a * 2.0 * fm[fmSourceOf(m)]);
          mul *= 1 + a * a * 8;
        } else if (m >= 51 && m <= 56) {
          p = frac(p + a * fm[fmSourceOf(m)] * 0.5 * (0.5 - 0.5 * std::cos(kPi2 * p)));
          mul *= 1 + a * 3;
        } else if (m == 57) {
          p = frac(p + a * selfFb_[u] * 0.5);
          mul *= 1 + a * 3;
        }
        break;
    }
  }
  const int level = mipLevel(inc * mul);
  const double fpos = std::clamp(static_cast<double>(s.wtPos + s.uniWt * s.uw[u] * 0.5f), 0.0, 1.0) * (t.frames - 1);
  float x = mipRead(t, level, fpos, s.smooth, p);
  for (int k = 0; k < 2; ++k) {
    const int m = modes[k];
    const float a = amt[k];
    if (m <= 0 || a <= 1e-6f) continue;
    if (m == 13 && s.src) {
      x = lerpf(x, remapAt(s.src->remap[2], (x + 1) * 0.5) * 2.f - 1.f, a);
    } else if (m == 16) {
      const float x2 = mipRead(t, level, fpos, s.smooth, frac(p + 0.5));
      const float even = (x + x2) * 0.5f, odd = (x - x2) * 0.5f;
      x = var[k] >= 0.5f ? x - a * odd : x - a * even;
    } else if (m == 17) {
      const int l2 = std::max(level, harmLevel(std::pow(1024.0, 1.0 - a)));
      if (l2 != level) x = mipRead(t, l2, fpos, s.smooth, p);
    } else if (m == 18) {
      const double h = std::pow(1024.0, a) * 0.5;
      if (h >= 1) x -= mipRead(t, std::max(level, harmLevel(h)), fpos, s.smooth, p);
    } else if (m >= 19 && m <= 32) {
      float st = 0;
      x = lerpf(x, shapeSample(warpDistMode(m), x, a, (var[k] - 0.5f) * 2.f, nullptr, nullptr, st), std::min(1.f, a * 4.f));
    } else if (m >= 58 && m <= 63) {
      x *= 1.f - a + a * (fm[fmSourceOf(m)] * 0.5f + 0.5f);
    } else if (m >= 64 && m <= 69) {
      x *= 1.f - a + a * fm[fmSourceOf(m)];
    }
  }
  if (flip) x = -x;
  selfFb_[u] = x;
  ph = frac(ph + inc * incMul);
  return x * gain;
}

// ------------------------------------------------------------------ sample / multisample
void OscVoice::samplePlay(const OscSet& s, float& l, float& r) {
  const Sample* smp = s.src ? s.src->sample.get() : nullptr;
  const Zone* zn = nullptr;
  if (s.type == OscMultisample) {
    if (!s.src || zone_ < 0 || zone_ >= static_cast<int>(s.src->zones.size())) return;
    zn = &s.src->zones[static_cast<size_t>(zone_)];
    smp = zn->s.get();
  }
  if (!smp || smp->frames() < 4) {
    finished_ = true;
    return;
  }
  const int64_t n = static_cast<int64_t>(smp->frames());
  const float* dl = smp->left.data();
  const float* dr = smp->mono() ? dl : smp->right.vec().data();
  double a = std::clamp(static_cast<double>(s.start), 0.0, 1.0) * n, b = std::clamp(static_cast<double>(s.end), 0.0, 1.0) * n;
  if (s.type == OscSample && s.src && !s.src->slices.empty()) {
    const int si = std::clamp(s.slice - 1, 0, static_cast<int>(s.src->slices.size()) - 1);
    a = s.src->slices[static_cast<size_t>(si)];
    if (s.sliceMode == 0 && si + 1 < static_cast<int>(s.src->slices.size())) b = s.src->slices[static_cast<size_t>(si + 1)];
    else b = static_cast<double>(n);
  }
  int loopMode = s.loopMode;
  double ls, le;
  if (zn) {
    a = static_cast<double>(zn->start);
    b = zn->end > 0 ? static_cast<double>(std::min<int64_t>(zn->end, n)) : static_cast<double>(n);
    loopMode = zn->loopMode == 0 ? 0 : 1;
    if (zn->loopMode == 2 && released_) loopMode = 0;
    ls = static_cast<double>(zn->loopStart), le = static_cast<double>(std::min<int64_t>(zn->loopEnd, n - 1));
  } else if (s.relLoop) {
    ls = a + (b - a) * s.loopStart, le = a + (b - a) * s.loopEnd;
  } else {
    ls = s.loopStart * n, le = s.loopEnd * n;
  }
  if (b <= a + 2) b = a + 2;
  ls = std::clamp(ls, a, b - 1), le = std::clamp(le, ls + 1, b);
  if (loopMode == 4 && released_) loopMode = 0;  // tailed: play out after the release
  const double rootRate = smp->sampleRate / s.srv;
  const double center = zn ? zn->center - zn->tune / 100.0 : 60.0;
  const float zg = zn ? static_cast<float>(std::pow(10.0, zn->gainDb / 20.0)) : 1.f;
  bool any = false;
  for (int u = 0; u < s.unison; ++u) {
    if (dirU_[u] == 0) continue;
    double& pos = play_[u];
    const double rate = std::exp2((s.pitch + s.det[u] - center) / 12.0) * rootRate * s.scan;
    const double step = (s.reverse ? -rate : rate) * dirU_[u];
    float xl = sampleAt(dl, n, pos, step), xr = dr == dl ? xl : sampleAt(dr, n, pos, step);
    // loop crossfade: near the loop end, blend towards the loop start
    if (loopMode == 1 && s.loopXfade > 0 && !s.reverse) {
      const double xf = (le - ls) * s.loopXfade * 0.5;
      if (pos > le - xf && xf > 1) {
        const double w = (pos - (le - xf)) / xf;
        const double p2 = ls - (le - pos);
        xl = lerpf(xl, sampleAt(dl, n, p2, step), static_cast<float>(w));
        xr = lerpf(xr, dr == dl ? sampleAt(dl, n, p2, step) : sampleAt(dr, n, p2, step), static_cast<float>(w));
      }
    }
    pos += step;
    if (loopMode == 0) {
      if (pos >= b || pos < a) dirU_[u] = 0;
    } else if (loopMode == 1 || loopMode == 4) {
      if (step > 0 && pos >= le) pos = ls + (pos - le);
      if (step < 0 && pos < ls) pos = le - (ls - pos);
    } else if (loopMode == 2) {  // reverse loop
      if (pos >= le) { pos = le; dirU_[u] = -1; }
      if (pos < ls) pos = le - (ls - pos);
    } else if (loopMode == 3) {  // ping-pong
      if (pos >= le) { pos = le - (pos - le); dirU_[u] = -dirU_[u]; }
      if (pos < ls) { pos = ls + (ls - pos); dirU_[u] = -dirU_[u]; }
    }
    l += xl * s.pl[u] * zg;
    r += xr * s.pr[u] * zg;
    any = true;
  }
  if (!any) finished_ = true;
  dispPos_ = n > 0 ? play_[0] / static_cast<double>(n) : 0;
}

// ------------------------------------------------------------------ granular
void OscVoice::granular(const OscSet& s, float& l, float& r) {
  // source: the sample, else the wavetable's raw frames as one long cycle sequence
  const Sample* smp = s.src ? s.src->sample.get() : nullptr;
  const float* dl = nullptr;
  const float* dr = nullptr;
  int64_t n = 0;
  double srcRate = 44100, rootHz = 261.6256;
  if (smp && smp->frames() > 16) {
    dl = smp->left.data();
    dr = smp->mono() ? dl : smp->right.vec().data();
    n = static_cast<int64_t>(smp->frames());
    srcRate = smp->sampleRate;
  } else if (s.mip && s.mip->raw) {
    dl = dr = s.mip->raw->data.data();
    n = static_cast<int64_t>(s.mip->raw->data.size());
    srcRate = rootHz * kFrameSize;  // one cycle per frame at middle C
  }
  if (!dl || n < 16) return;
  const double base = std::clamp(static_cast<double>(s.position), 0.0, 1.0);
  const int streams = std::clamp(s.unison, 1, kMaxUnison);
  for (int u = 0; u < streams; ++u) {
    spawnAcc_[u] += s.density / s.srv;
    if (spawnAcc_[u] < 1) continue;
    spawnAcc_[u] -= std::floor(spawnAcc_[u]);
    int active = 0;
    for (const auto& g : grains_) active += g.active ? 1 : 0;
    if (active >= std::min(64, s.maxGrains)) continue;
    for (auto& g : grains_) {
      if (g.active) continue;
      const float ro = s.rnd[0], rd = s.rnd[1], rpi = s.rnd[2], rl = s.rnd[3], rp = s.rnd[4], rg = s.rnd[5], rw = s.rnd[6], rs = s.rnd[7];
      const double pos = std::clamp(base + rngBi(rng_) * ro * 0.5, 0.0, 1.0) * static_cast<double>(n - 1);
      const double lenMs = s.lengthMs * (1.0 + rngBi(rng_) * rl * 0.9);
      g.len = std::max(16, static_cast<int>(lenMs * 0.001 * s.srv));
      g.left = g.len;
      const double semis = s.pitch - 60.0 + s.det[u] + rngBi(rng_) * rpi * 12.0;
      double inc = std::exp2(semis / 12.0) * srcRate / s.srv;
      const bool rev = rngUni(rng_) < std::max(s.grReverse, rd * 0.5f);
      if (rev) inc = -inc;
      g.pos = pos;
      g.inc = inc;
      const float pan = std::clamp(rngBi(rng_) * rp, -1.f, 1.f);
      g.pl = s.pl[u] * (pan > 0 ? 1 - pan : 1);
      g.pr = s.pr[u] * (pan < 0 ? 1 + pan : 1);
      g.gain = 1.f - rngUni(rng_) * rg;
      g.win = rw > 0 && rngUni(rng_) < rw ? static_cast<int>(rngNext(rng_) % 10) : s.winShape;
      g.skew = std::clamp(s.winSkew + rngBi(rng_) * rs, -1.f, 1.f);
      g.param = s.winParam;
      g.warp[0] = rngBi(rng_) * s.rnd[8];
      g.warp[1] = rngBi(rng_) * s.rnd[9];
      g.active = true;
      break;
    }
  }
  float sl = 0, sr = 0, wsum = 0, ws[2] = {0, 0};
  for (auto& g : grains_) {
    if (!g.active) continue;
    const double t = 1.0 - static_cast<double>(g.left) / g.len;
    const float w = window(g.win, t, g.skew, g.param) * g.gain;
    wsum += std::fabs(w), ws[0] += std::fabs(w) * g.warp[0], ws[1] += std::fabs(w) * g.warp[1];
    double p = g.pos;
    if (p < 0 || p >= static_cast<double>(n)) p = std::fmod(std::fmod(p, static_cast<double>(n)) + static_cast<double>(n), static_cast<double>(n));
    const float xl = sampleAt(dl, n, p, g.inc), xr = dr == dl ? xl : sampleAt(dr, n, p, g.inc);
    sl += xl * w * g.pl;
    sr += xr * w * g.pr;
    g.pos += g.inc;
    if (--g.left <= 0) g.active = false;
  }
  const float norm = 1.f / std::sqrt(std::max(1.f, static_cast<float>(s.density * s.lengthMs * 0.001)));
  l += sl * norm;
  r += sr * norm;
  grainWarp_[0] = wsum > 1e-6f ? ws[0] / wsum : 0.f;
  grainWarp_[1] = wsum > 1e-6f ? ws[1] / wsum : 0.f;
  dispPos_ = base;
}

// ------------------------------------------------------------------ spectral
void OscVoice::spectralHop(const OscSet& s) {
  constexpr int N = 2048, H = 512, B = 1024;
  cpx* XL = work_.get();
  cpx* XR = work_.get() + N;
  std::fill(XL, XL + N, cpx(0, 0));
  std::fill(XR, XR + N, cpx(0, 0));
  // source partials: magnitude per partial index and its frequency (Hz at this note)
  float* mag = smag_.get();
  float* frq = sfrq_.get();
  float* tmp = stmp_.get();
  int count = 0;
  const SpecSrc* sp = s.src ? s.src->spec.get() : nullptr;
  if (sp && sp->frames > 0) {
    const double fpos = std::clamp(specPos_, 0.0, 1.0) * (sp->frames - 1);
    const int f0 = static_cast<int>(fpos);
    const int f1 = std::min(sp->frames - 1, f0 + 1);
    const float t = static_cast<float>(fpos - f0);
    const double ratio = std::exp2((s.pitch - 60.0) / 12.0);
    for (int k = 1; k < B; ++k) {
      const float a = sp->mag[static_cast<size_t>(f0) * B + k], b = sp->mag[static_cast<size_t>(f1) * B + k];
      mag[count] = a + (b - a) * t;
      frq[count] = static_cast<float>(k * sp->rate / SpecSrc::kN * ratio);
      ++count;
    }
    // advance the scan
    const double dur = static_cast<double>(sp->frames) * SpecSrc::kHop / sp->rate;  // seconds
    specPos_ += s.scan * (H / s.srv) / std::max(1e-3, dur);
    const double lo = std::clamp(static_cast<double>(s.start), 0.0, 1.0), hi = std::clamp(static_cast<double>(s.end), lo + 1e-3, 1.0);
    if (specPos_ > hi) specPos_ = s.loopMode == 0 ? hi : lo + std::fmod(specPos_ - lo, hi - lo);
    if (specPos_ < lo) specPos_ = lo;
  } else if (s.mip) {
    const MipTable& t = *s.mip;
    const double fp = std::clamp(static_cast<double>(s.wtPos), 0.0, 1.0) * (t.frames - 1);
    const int f0 = static_cast<int>(fp);
    const int f1 = std::min(t.frames - 1, f0 + 1);
    const float w = static_cast<float>(fp - f0);
    for (int h = 1; h <= MipTable::kHarm; ++h) {
      const double hz = s.hz * h;
      if (hz >= s.srv * 0.5) break;
      const float a = t.mag[static_cast<size_t>(f0) * MipTable::kHarm + static_cast<size_t>(h - 1)];
      const float b = t.mag[static_cast<size_t>(f1) * MipTable::kHarm + static_cast<size_t>(h - 1)];
      mag[count] = a + (b - a) * w;
      frq[count] = static_cast<float>(hz);
      ++count;
    }
  }
  // spectral warps on the partials
  float peak = 0;
  for (int i = 0; i < count; ++i) peak = std::max(peak, mag[i]);
  const int modes[2] = {s.w1, s.w2};
  const float amt[2] = {s.a1, s.a2}, var[2] = {s.v1, s.v2};
  for (int k = 0; k < 2; ++k) {
    const int m = modes[k];
    const float a = std::clamp(amt[k], 0.f, 1.f);
    if (m < 70 || a <= 1e-6f) continue;
    switch (m) {
      case 70: for (int i = 0; i < count; ++i) frq[i] *= std::pow(static_cast<float>(i + 1), a * 0.06f); break;  // detune / stretch
      case 71: for (int i = 0; i < count && i < B; ++i) mag[i] = lerpf(mag[i], prevMag_[i], a * 0.97f); break;          // smear
      case 72: {  // spread (blur across neighbours)
        const int w = 1 + static_cast<int>(a * 12);
        for (int i = 0; i < count; ++i) tmp[i] = mag[i];
        for (int i = 0; i < count; ++i) {
          float acc = 0;
          int c2 = 0;
          for (int j = std::max(0, i - w); j <= std::min(count - 1, i + w); ++j) acc += tmp[j], ++c2;
          mag[i] = lerpf(tmp[i], acc / static_cast<float>(c2), a);
        }
        break;
      }
      case 73:  // add harmonics: every partial gains an octave above
        for (int i = count - 1; i >= 0; --i) {
          const int j = (i + 1) * 2 - 1;
          if (j < count) mag[j] += mag[i] * a * 0.7f;
        }
        break;
      case 74:  // add subharmonics
        for (int i = 0; i < count; ++i) {
          const int j = (i + 1) / 2 - 1;
          if (j >= 0) mag[j] += mag[i] * a * 0.5f;
        }
        break;
      case 75: for (int i = 0; i < count; ++i) if (mag[i] < peak * a * a) mag[i] = 0; break;  // gate
      case 76: if (rngUni(rng_) < a) for (int i = 0; i < 8 * B; ++i) sphase_[static_cast<size_t>(i)] = 0; break;  // robotize
      case 77: for (int i = 0; i < count; ++i) frq[i] += (var[k] >= 0.5f ? -1.f : 1.f) * a * 2000.f; break;  // linear shift
      case 78: {  // mirror around a centre frequency
        const float c = 200.f + a * 8000.f;
        for (int i = 0; i < count; ++i) frq[i] = std::fabs(2 * c - frq[i]);
        break;
      }
      case 79:
      case 80:
      case 81:
      case 82:
      case 83:
      case 84: {  // peak follow / octave / harmonic variants
        for (int i = 0; i < count; ++i) tmp[i] = (i > 0 && i + 1 < count && mag[i] >= mag[i - 1] && mag[i] >= mag[i + 1]) ? mag[i] : 0.f;
        for (int i = 0; i < count; ++i) {
          float v = tmp[i];
          if (m == 80 && i / 2 > 0) v += tmp[i / 2];
          if (m == 81 && i * 2 + 1 < count) v += tmp[i * 2 + 1];
          if (m == 82 && i > 2) v += tmp[i - 2] * 0.5f;
          if (m == 83 && i + 2 < count) v += tmp[i + 2] * 0.5f;
          if (m == 84) v += tmp[static_cast<size_t>(std::clamp(static_cast<int>(i * (1 + var[k])), 0, count - 1))] * 0.5f;
          mag[i] = lerpf(mag[i], v * 1.5f, a);
        }
        break;
      }
      case 85:
      case 86: {  // shepard: a moving log-frequency window
        const float c = std::log2(50.f) + a * 8.f, w = m == 85 ? 0.6f : 1.5f;
        for (int i = 0; i < count; ++i) {
          const float d = (std::log2(std::max(1.f, frq[i])) - c) / w;
          mag[i] *= std::exp(-d * d);
        }
        break;
      }
      case 87: for (int i = 0; i < count; ++i) mag[i] *= 0.5f + 0.5f * std::cos(frq[i] * 0.001f * (1 + a * 20.f)); break;  // comb
      case 88:
      case 89: {  // pitch shift (89 keeps the spectral envelope)
        const float r = std::exp2((var[k] >= 0.5f ? -1.f : 1.f) * a * 12.f / 12.f);
        for (int i = 0; i < count; ++i) frq[i] *= r;
        if (m == 89) {
          for (int i = 0; i < count; ++i) tmp[i] = mag[i];
          for (int i = 0; i < count; ++i) mag[i] = tmp[std::clamp(static_cast<int>(i / r), 0, count - 1)];
        }
        break;
      }
      case 90: for (int i = 0; i < 8 * B; ++i) sphase_[static_cast<size_t>(i)] += a * static_cast<float>(i % B) * 0.01f; break;
      case 91: {  // formant shift
        for (int i = 0; i < count; ++i) tmp[i] = mag[i];
        const float r = std::exp2((a * 2.f - 1.f) * 1.f);
        for (int i = 0; i < count; ++i) mag[i] = tmp[std::clamp(static_cast<int>(i / r), 0, count - 1)];
        break;
      }
      default: break;  // mask / vocode handled on the output bins
    }
  }
  // KEEP TRANSIENTS: a sharp rise of the source spectrum (an onset) restarts the partial phases,
  // so attacks stay crisp instead of smearing over the hop
  bool onset = false;
  if (s.transients && sp && specStarted_) {
    double rise = 0, total = 0;
    for (int i = 0; i < count && i < B; ++i) {
      const double d = static_cast<double>(mag[i]) - prevMag_[i];
      if (d > 0) rise += d;
      total += mag[i];
    }
    onset = total > 1e-6 && rise > 0.3 * total;
  }
  for (int i = 0; i < count && i < B; ++i) prevMag_[i] = mag[i];
  specStarted_ = true;
  // place partials into the output bins (L / R), one phase accumulator per unison voice and partial
  const int U = std::clamp(s.unison, 1, 8);
  // J-201: a sample resynthesised here plays 12.5 dB above the same sample in Sample mode, as the
  // source format's spectral oscillator does (measured on three library samples: +12.2..+12.6 dB);
  // it was 13.5 dB below it.
  const float norm = 0.5f * N * 0.5f * (sp ? 0.6f * 18.8f : 1.f) / std::sqrt(static_cast<float>(U));
  // J-201: the band limits use the filter cutoff scale (0 = 8 Hz .. 1 = 22 kHz, as the import writes
  // them); a linear fraction of the oversampled Nyquist silenced imported spectral oscillators.
  const float lo = s.sLo <= 0.f ? 0.f : static_cast<float>(cutoffHz(s.sLo));
  const float hi = s.sHi >= 1.f ? static_cast<float>(s.srv) : std::max(lo + 1.f, static_cast<float>(cutoffHz(s.sHi)));
  const float cut = s.sCut;
  for (int u = 0; u < U; ++u) {
    const float r = std::exp2(s.det[u] / 12.f);
    float* ph = sphase_.get() + static_cast<size_t>(u) * B;
    for (int i = 0; i < count; ++i) {
      const float f = frq[i] * r;
      if (f <= 0 || f >= s.srv * 0.49) continue;
      float a = mag[i] * norm;
      // spectral filter: band edges shifted by CUT, mixed by MIX
      const float fs = f * std::exp2(cut * -4.f);
      const float inBand = (fs >= lo && fs <= hi) ? 1.f : 0.f;
      a *= lerpf(1.f, inBand, s.sMix);
      if (a < 1e-7f) continue;
      if (onset) ph[i] = 0;
      ph[i] += static_cast<float>(kPi2 * f * H / s.srv);
      // PHASE LOCK: partials keep their phase relation to the lowest one (holds the waveform shape)
      if (s.phaseLock && i > 0 && frq[0] > 0) ph[i] = std::fmod(ph[0] * frq[i] / frq[0], static_cast<float>(kPi2));
      if (ph[i] > 1e4f) ph[i] = std::fmod(ph[i], static_cast<float>(kPi2));
      const float bin = f * N / static_cast<float>(s.srv);
      const int b0 = static_cast<int>(bin);
      if (b0 < 1 || b0 + 1 >= N / 2) continue;
      const float fr = bin - static_cast<float>(b0);
      const cpx c = std::polar(a, ph[i]);
      XL[b0] += c * (1 - fr) * s.pl[u];
      XL[b0 + 1] += c * fr * s.pl[u];
      XR[b0] += c * (1 - fr) * s.pr[u];
      XR[b0 + 1] += c * fr * s.pr[u];
    }
  }
  // mask / vocode from another source (its recent output)
  for (int k = 0; k < 2; ++k) {
    const int m = modes[k];
    if (m < 92 || m > 103 || amt[k] <= 1e-6f) continue;
    cpx* Y = sy_.get();
    for (int i = 0; i < N; ++i) {
      const float w = 0.5f - 0.5f * std::cos(static_cast<float>(kPi2) * i / N);
      Y[i] = cpx(fmRing_[static_cast<size_t>((fmPos_ + i) & (N - 1))] * w, 0);
    }
    fft2048().forward(Y);
    float ymax = 1e-9f;
    for (int b = 1; b < N / 2; ++b) ymax = std::max(ymax, std::abs(Y[b]));
    for (int b = 1; b < N / 2; ++b) {
      const float g = std::abs(Y[b]) / ymax;
      const float mm = m <= 97 ? lerpf(1.f, g, amt[k]) : lerpf(1.f, g * 4.f, amt[k]);
      XL[b] *= mm, XR[b] *= mm;
    }
  }
  for (int b = 1; b < N / 2; ++b) {
    XL[N - b] = std::conj(XL[b]);
    XR[N - b] = std::conj(XR[b]);
  }
  XL[0] = XR[0] = XL[N / 2] = XR[N / 2] = cpx(0, 0);
  fft2048().inverse(XL);
  fft2048().inverse(XR);
  // shift the overlap buffer by one hop, add the new windowed frame
  float* oL = ola_.get();
  float* oR = ola_.get() + N;
  std::memmove(oL, oL + H, sizeof(float) * (N - H));
  std::memmove(oR, oR + H, sizeof(float) * (N - H));
  std::fill(oL + N - H, oL + N, 0.f);
  std::fill(oR + N - H, oR + N, 0.f);
  for (int i = 0; i < N; ++i) {
    const float w = 0.5f - 0.5f * std::cos(static_cast<float>(kPi2) * i / N);
    oL[i] += XL[i].real() * w * 0.5f;
    oR[i] += XR[i].real() * w * 0.5f;
  }
  olaPos_ = 0;
}

void OscVoice::spectral(const OscSet& s, const float* fm, float& l, float& r) {
  // keep the mask / vocode source (first FM-style source of the warp slots)
  int src = 0;
  for (int m : {s.w1, s.w2})
    if (m >= 92 && m <= 103) src = (m - 92) % 6;
  fmRing_[static_cast<size_t>(fmPos_)] = fm[src];
  fmPos_ = (fmPos_ + 1) & 2047;
  if (olaPos_ >= 512) spectralHop(s);
  float xl = ola_[static_cast<size_t>(olaPos_)], xr = ola_[static_cast<size_t>(2048 + olaPos_)];
  ++olaPos_;
  // time-domain warps that also apply to spectral oscillators
  const int modes[2] = {s.w1, s.w2};
  const float amt[2] = {s.a1, s.a2};
  for (int k = 0; k < 2; ++k) {
    const int m = modes[k];
    const float a = std::clamp(amt[k], 0.f, 1.f);
    if (a <= 1e-6f) continue;
    if (m >= 19 && m <= 32) {
      float st = 0;
      xl = lerpf(xl, shapeSample(warpDistMode(m), xl, a, 0, nullptr, nullptr, st), std::min(1.f, a * 4));
      xr = lerpf(xr, shapeSample(warpDistMode(m), xr, a, 0, nullptr, nullptr, st), std::min(1.f, a * 4));
    } else if (m >= 58 && m <= 63) {
      const float g = 1.f - a + a * (fm[fmSourceOf(m)] * 0.5f + 0.5f);
      xl *= g, xr *= g;
    } else if ((m >= 64 && m <= 69) || (m >= 33 && m <= 57)) {  // RM; FM / PD approximated as ring modulation
      const float g = 1.f - a + a * fm[fmSourceOf(m)];
      xl *= g, xr *= g;
    }
  }
  l += xl;
  r += xr;
  dispPos_ = specPos_;
}

// ------------------------------------------------------------------ per sample
void OscVoice::tick(const OscSet& s, const float* fm, float& l, float& r, float& mono) {
  l = r = mono = 0;
  if (!s.on || finished_) return;
  switch (s.type) {
    case OscSample:
    case OscMultisample: {
      samplePlay(s, l, r);
      break;
    }
    case OscGranular: granular(s, l, r); break;
    case OscSpectral: spectral(s, fm, l, r); break;
    default: {
      if (!s.mip) return;
      for (int u = 0; u < s.unison; ++u) {
        const float x = wavetableSample(s, u, phase_[u], s.inc[u], fm);
        l += x * s.pl[u];
        r += x * s.pr[u];
      }
      dispPos_ = s.wtPos;
      break;
    }
  }
  if (s.type != OscWavetable) {  // filter / shaper / AM / RM warps for the sample-based types
    const int modes[2] = {s.w1, s.w2};
    const float amt[2] = {s.a1, s.a2};
    for (int k = 0; k < 2 && s.type != OscSpectral; ++k) {
      const int m = modes[k];
      const float a = std::clamp(amt[k] + (s.type == OscGranular ? grainWarp_[k] : 0.f), 0.f, 1.f);  // + the grains' random warp
      if (m <= 0 || a <= 1e-6f) continue;
      if (m == 17 || m == 18) {
        const float c = static_cast<float>(1.0 - std::exp(-kPi2 * 20000.0 * std::pow(m == 17 ? 0.001 : 1.0, m == 17 ? a : 1.0 - a) / s.srv));
        postZ_[k][0] += (l - postZ_[k][0]) * c;
        postZ_[k][1] += (r - postZ_[k][1]) * c;
        if (m == 17) l = postZ_[k][0], r = postZ_[k][1];
        else l -= postZ_[k][0], r -= postZ_[k][1];
      } else if (m >= 19 && m <= 32) {
        float st = 0;
        l = lerpf(l, shapeSample(warpDistMode(m), l, a, 0, nullptr, nullptr, st), std::min(1.f, a * 4));
        r = lerpf(r, shapeSample(warpDistMode(m), r, a, 0, nullptr, nullptr, st), std::min(1.f, a * 4));
      } else if (m >= 58 && m <= 63) {
        const float g = 1.f - a + a * (fm[fmSourceOf(m)] * 0.5f + 0.5f);
        l *= g, r *= g;
      } else if (m >= 64 && m <= 69) {
        const float g = 1.f - a + a * fm[fmSourceOf(m)];
        l *= g, r *= g;
      }
    }
  }
  mono = (l + r) * 0.5f;
  lastMono_ = mono;
}

// ------------------------------------------------------------------ sub / noise
float SubVoice::tick(int shape, double inc) {
  const double p = ph;
  ph += inc;
  if (ph >= 1) ph -= std::floor(ph);
  const double dt = std::min(0.5, std::fabs(inc));
  auto blep = [&](double t) {
    if (t < dt) {
      t /= dt;
      return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
      t = (t - 1.0) / dt;
      return t * t + t + t + 1.0;
    }
    return 0.0;
  };
  switch (shape) {
    case SubRoundRect: return static_cast<float>(std::tanh(std::sin(kPi2 * p) * 3.0) / std::tanh(3.0));
    case SubTriangle: return static_cast<float>(p < 0.5 ? p * 4 - 1 : 3 - p * 4);
    case SubSaw: return static_cast<float>(2 * p - 1 - blep(p));
    case SubSquare: {
      double v = p < 0.5 ? 1.0 : -1.0;
      v += blep(p);
      v -= blep(frac(p + 0.5));
      return static_cast<float>(v);
    }
    case SubPulse: {
      double v = p < 0.25 ? 1.0 : -1.0;
      v += blep(p);
      v -= blep(frac(p + 0.75));
      return static_cast<float>(v * 0.8);
    }
    default: return static_cast<float>(std::sin(kPi2 * p));
  }
}

void NoiseVoice::start(const NoiseSrc* n, const Sample* file, float phase, float rnd, uint32_t seed) {
  const double len = file ? static_cast<double>(file->frames()) : n ? static_cast<double>(n->L.size()) : 0.0;
  uint32_t s = seed ? seed : 7;
  pos = std::clamp(static_cast<double>(phase) + rnd * rngUni(s), 0.0, 1.0) * len;
  if (pos >= len) pos = 0;
  lp = 0;
  tz[0][0] = tz[0][1] = tz[1][0] = tz[1][1] = 0;
  done = false;
}

void NoiseVoice::tick(const NoiseSrc* n, const Sample* file, double rate, bool oneShot, float color, float& l, float& r) {
  l = r = 0;
  if (done) return;
  const float* dl;
  const float* dr;
  int64_t len;
  if (file && file->frames() > 0) {
    dl = file->left.data();
    dr = file->mono() ? dl : file->right.vec().data();
    len = static_cast<int64_t>(file->frames());
  } else if (n && !n->L.empty()) {
    dl = n->L.data();
    dr = n->R.empty() ? dl : n->R.data();
    len = static_cast<int64_t>(n->L.size());
  } else {
    return;
  }
  // J-140: band-limited read (was linear), wrapping like the position does.
  const pitch::SincTable& tab = pitch::table(pitch::Quality::Compact);
  auto wrapRead = [&](const float* d) {
    float o;
    pitch::readWith<1>(
        [d, len](int64_t k, int) {
          k %= len;
          return d[k < 0 ? k + len : k];
        },
        pos, std::fabs(rate), tab, &o);
    return o;
  };
  float xl = wrapRead(dl);
  float xr = dr == dl ? xl : wrapRead(dr);
  pos += rate;
  if (pos >= static_cast<double>(len)) {
    if (oneShot || (n && !n->loop && !file)) done = true;
    pos = std::fmod(pos, static_cast<double>(len));
  }
  // colour: tilt (dark < 0 < bright)
  if (color != 0) {
    const float c = 0.15f;
    lp += ((xl + xr) * 0.5f - lp) * c;
    if (color < 0) {
      xl = lerpf(xl, lp, -color), xr = lerpf(xr, lp, -color);
    } else {
      xl = xl + (xl - lp) * color, xr = xr + (xr - lp) * color;
    }
  }
  l = xl;
  r = xr;
}

}  // namespace gl::packs::plasma

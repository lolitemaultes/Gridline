// Gridline — "edison" pack: Loupe's repair tools: deverb, declip and the drum loop stretch
// (edison_repair.h). Methods are general published techniques: a statistical late reverb model
// with spectral subtraction, the distribution of free decay rates for the reverb time,
// autoregressive least-squares interpolation for clipped gaps, and onset-locked slicing.
#include "edison_repair.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../../engine/Dsp.h"
#include "../../engine/TimeStretch.h"
#include "edison_internal.h"

namespace gl::packs::edison {

using namespace gl::dsp;

namespace {

int frameSize(double rate) { return std::clamp(nextPow2(static_cast<int64_t>(rate * 0.021)), 256, 4096); }

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const double k = std::clamp(q, 0.0, 1.0) * static_cast<double>(v.size() - 1);
  const size_t i = static_cast<size_t>(k);
  const double f = k - static_cast<double>(i);
  return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}

// Short-time processing of both channels at once (one complex FFT carries left in the real
// part and right in the imaginary part). Frames of `fft` every `hop` over x[x0, x1); per frame
// fn(k, power, gain) sees the power per bin (both channels) and sets one real gain per bin.
// Overlap-add with Hann windows, normalised. Returns false when stopped.
template <class F>
bool gainStft(const Audio& a, int64_t x0, int64_t x1, int fft, int hop, Progress* p, double p0, double p1, std::vector<float>& outL,
              std::vector<float>& outR, F fn) {
  const int64_t n = x1 - x0;
  const Fft F_(fft);
  const std::vector<double> w = hannWindow(fft);
  const int half = fft / 2, bins = half + 1;
  std::vector<double> accL(static_cast<size_t>(n), 0.0), accR(static_cast<size_t>(n), 0.0), norm(static_cast<size_t>(n), 0.0);
  std::vector<cplx> Z(static_cast<size_t>(fft));
  std::vector<double> pw(static_cast<size_t>(bins)), g(static_cast<size_t>(bins));
  const float* L = a.L.data();
  const float* R = a.channels == 1 ? a.L.data() : a.R.data();
  const int64_t frames = n / hop + 2;
  for (int64_t k = 0; k < frames; ++k) {
    if ((k & 63) == 0) {
      if (stopped(p)) return false;
      report(p, p0 + (p1 - p0) * static_cast<double>(k) / static_cast<double>(frames));
    }
    const int64_t c0 = x0 + k * hop - half;
    for (int i = 0; i < fft; ++i) {
      const int64_t s = c0 + i;
      const bool in = s >= x0 && s < x1;
      Z[static_cast<size_t>(i)] = in ? cplx(L[s] * w[static_cast<size_t>(i)], R[s] * w[static_cast<size_t>(i)]) : cplx(0, 0);
    }
    F_.forward(Z);
    for (int b = 0; b < bins; ++b) {
      const cplx zk = Z[static_cast<size_t>(b)], zn = std::conj(Z[static_cast<size_t>((fft - b) % fft)]);
      const cplx xl = 0.5 * (zk + zn), xr = cplx(0, -0.5) * (zk - zn);
      pw[static_cast<size_t>(b)] = std::norm(xl) + std::norm(xr);
    }
    fn(k, pw, g);
    for (int b = 0; b < bins; ++b) {
      Z[static_cast<size_t>(b)] *= g[static_cast<size_t>(b)];
      if (b > 0 && b < half) Z[static_cast<size_t>(fft - b)] *= g[static_cast<size_t>(b)];
    }
    F_.inverse(Z);
    for (int i = 0; i < fft; ++i) {
      const int64_t s = c0 + i - x0;
      if (s < 0 || s >= n) continue;
      const double wi = w[static_cast<size_t>(i)];
      accL[static_cast<size_t>(s)] += Z[static_cast<size_t>(i)].real() * wi;
      accR[static_cast<size_t>(s)] += Z[static_cast<size_t>(i)].imag() * wi;
      norm[static_cast<size_t>(s)] += wi * wi;
    }
  }
  outL.resize(static_cast<size_t>(n));
  outR.resize(static_cast<size_t>(n));
  for (int64_t s = 0; s < n; ++s) {
    const double q = norm[static_cast<size_t>(s)] > 1e-9 ? 1.0 / norm[static_cast<size_t>(s)] : 0.0;
    outL[static_cast<size_t>(s)] = static_cast<float>(accL[static_cast<size_t>(s)] * q);
    outR[static_cast<size_t>(s)] = static_cast<float>(accR[static_cast<size_t>(s)] * q);
  }
  return true;
}

// Writes processed audio (which covers [x0, x0 + y.size())) into `a` over r, fading from the
// untouched audio into it and back over `fade` frames at edges that are not the file's ends.
void writeBack(Audio& a, Range r, int64_t x0, const std::vector<float>& yL, const std::vector<float>& yR, int64_t fade) {
  const int64_t n = a.frames();
  const int64_t fin = r.a > 0 ? std::min(fade, r.len() / 2) : 0, fout = r.b < n ? std::min(fade, r.len() / 2) : 0;
  for (int64_t i = r.a; i < r.b; ++i) {
    double w = 1;
    if (i - r.a < fin) w = static_cast<double>(i - r.a + 1) / static_cast<double>(fin + 1);
    if (r.b - 1 - i < fout) w = std::min(w, static_cast<double>(r.b - i) / static_cast<double>(fout + 1));
    const size_t j = static_cast<size_t>(i - x0);
    a.L[static_cast<size_t>(i)] = static_cast<float>(a.L[static_cast<size_t>(i)] * (1 - w) + yL[j] * w);
    if (a.channels == 2) a.R[static_cast<size_t>(i)] = static_cast<float>(a.R[static_cast<size_t>(i)] * (1 - w) + yR[j] * w);
  }
  fixMono(a);
}

}  // namespace

// ------------------------------------------------------------------ reverb time
DecayEstimate estimateDecay(const Audio& a, Range r, Progress* p) {
  DecayEstimate e;
  r.a = std::clamp<int64_t>(r.a, 0, a.frames());
  r.b = std::clamp<int64_t>(r.b, r.a, a.frames());
  const int fft = frameSize(a.rate), hop = fft / 4;
  if (r.len() < fft * 4) return e;
  const double hopSec = hop / a.rate;
  // Band energy envelopes (dB) per frame.
  const int K = DecayEstimate::kBands;
  std::vector<std::vector<double>> env(static_cast<size_t>(K));
  const Fft F(fft);
  const std::vector<double> w = hannWindow(fft);
  std::vector<cplx> buf(static_cast<size_t>(fft));
  const int64_t frames = (r.len() - fft) / hop + 1;
  std::vector<int> bandOf(static_cast<size_t>(fft / 2 + 1), -1);
  for (int b = 1; b <= fft / 2; ++b) {
    const double hz = b * a.rate / fft;
    for (int k = 0; k < K; ++k)
      if (hz >= 125.0 * std::pow(2.0, k) && hz < 250.0 * std::pow(2.0, k)) bandOf[static_cast<size_t>(b)] = k;
  }
  for (auto& v : env) v.reserve(static_cast<size_t>(frames));
  std::vector<double> acc(static_cast<size_t>(K));
  for (int64_t f = 0; f < frames; ++f) {
    if ((f & 127) == 0) {
      if (stopped(p)) return DecayEstimate{};
      report(p, 0.9 * static_cast<double>(f) / static_cast<double>(frames));
    }
    const int64_t s0 = r.a + f * hop;
    for (int i = 0; i < fft; ++i) {
      const double v = a.channels == 1 ? a.L[static_cast<size_t>(s0 + i)] : 0.5 * (a.L[static_cast<size_t>(s0 + i)] + a.R[static_cast<size_t>(s0 + i)]);
      buf[static_cast<size_t>(i)] = cplx(v * w[static_cast<size_t>(i)], 0);
    }
    F.forward(buf);
    std::fill(acc.begin(), acc.end(), 1e-14);
    for (int b = 1; b <= fft / 2; ++b)
      if (bandOf[static_cast<size_t>(b)] >= 0) acc[static_cast<size_t>(bandOf[static_cast<size_t>(b)])] += std::norm(buf[static_cast<size_t>(b)]);
    for (int k = 0; k < K; ++k) env[static_cast<size_t>(k)].push_back(acc[static_cast<size_t>(k)]);
  }
  // Free decays: from a local peak well above the band's floor, the level falls steadily; the
  // straight-line fit of the falling part (after the first 30 ms, above the floor) is its rate.
  // A sound that stops at once decays at the room's rate and a sound that fades decays slower,
  // so the room is the quicker end of the distribution (its lower third).
  const int64_t skip = std::max<int64_t>(1, std::llround(0.03 / hopSec));
  const int64_t minFit = std::max<int64_t>(5, std::llround(0.06 / hopSec));
  std::vector<double> all;
  for (int k = 0; k < K; ++k) {
    std::vector<double> d(env[static_cast<size_t>(k)].size());
    const auto& pw = env[static_cast<size_t>(k)];
    for (size_t i = 0; i < pw.size(); ++i) {  // three-frame smoothing (power), then dB
      const double m = (pw[i > 0 ? i - 1 : i] + pw[i] + pw[i + 1 < pw.size() ? i + 1 : i]) / 3.0;
      d[i] = 10.0 * std::log10(m);
    }
    const double top = *std::max_element(d.begin(), d.end());
    const double floorDb = std::max(percentile(d, 0.1), top - 90.0);
    std::vector<double> t60s;
    const size_t look = static_cast<size_t>(std::max<int64_t>(2, std::llround(0.02 / hopSec)));
    const size_t maxRun = static_cast<size_t>(std::llround(2.5 / hopSec));
    for (size_t i = 1; i + 1 < d.size();) {
      // A decay starts at a peak (the highest point within 20 ms either side) well above the floor.
      bool peak = d[i] >= floorDb + 25;
      for (size_t q = i > look ? i - look : 0; peak && q < std::min(d.size(), i + look + 1); ++q) peak = d[q] <= d[i];
      if (!peak) {
        ++i;
        continue;
      }
      // It runs while the level keeps falling (wobbles of up to 3 dB over its lowest point).
      size_t j = i;
      double low = d[i];
      while (j + 1 < d.size() && j - i < maxRun && d[j + 1] <= low + 3.0) {
        ++j;
        low = std::min(low, d[j]);
      }
      // The fit is the part from 5 dB to 25 dB down (the first drop is the sound itself ending,
      // the rest is the room), as reverb times are measured, and above the floor.
      const double hiDb = d[i] - 5.0, loDb = std::max(d[i] - 25.0, floorDb + 6.0);
      size_t a0 = i;
      while (a0 < j && d[a0] > hiDb) ++a0;
      a0 = std::max(a0, i + static_cast<size_t>(skip));
      size_t a1 = a0;
      while (a1 < j && d[a1] > loDb) ++a1;
      if (a1 > a0 + static_cast<size_t>(minFit) && d[a0] - d[a1] >= 12) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const double nn = static_cast<double>(a1 - a0 + 1);
        for (size_t q = a0; q <= a1; ++q) {
          const double x = static_cast<double>(q - a0);
          sx += x, sy += d[q], sxx += x * x, sxy += x * d[q];
        }
        const double slope = (nn * sxy - sx * sy) / std::max(1e-9, nn * sxx - sx * sx);  // dB per frame
        if (slope < 0) {
          const double t60 = -60.0 / (slope / hopSec);
          if (t60 > 0.08 && t60 < 12) t60s.push_back(t60);
        }
      }
      i = std::max(j, i + 1);
    }
    e.decays += static_cast<int>(t60s.size());
    if (t60s.size() >= 2) e.band[static_cast<size_t>(k)] = percentile(t60s, 0.3);
    all.insert(all.end(), t60s.begin(), t60s.end());
  }
  if (all.size() >= 2) {
    std::vector<double> bands;
    for (double v : e.band)
      if (v > 0) bands.push_back(v);
    e.t60 = bands.size() >= 2 ? percentile(bands, 0.5) : percentile(all, 0.3);
  }
  report(p, 1.0);
  return e;
}

double decayAt(const DecayEstimate& e, double hz) {
  const int K = DecayEstimate::kBands;
  auto at = [&](int k) { return e.band[static_cast<size_t>(k)] > 0 ? e.band[static_cast<size_t>(k)] : e.t60; };
  const double x = std::log2(std::max(1.0, hz) / 177.0);  // band index on a log axis
  if (x <= 0) return at(0);
  if (x >= K - 1) return at(K - 1);
  const int k = static_cast<int>(x);
  const double f = x - k;
  return at(k) * (1 - f) + at(k + 1) * f;
}

// ------------------------------------------------------------------ deverb
Content deverb(const Content& c, Range r, const DeverbSettings& s, Progress* p) {
  r = rangeOrAll(c, r);
  const double amt = std::clamp(s.amount, 0.0, 1.0);
  if (r.empty() || amt <= 0) return c;
  const Audio& in = c.a();
  const double rate = in.rate;
  const int fft = frameSize(rate), hop = fft / 4, bins = fft / 2 + 1;
  const double hopSec = hop / rate;
  // Late reverb starts after the early reflections (kept) or right after the direct sound.
  const int N = std::max(1, static_cast<int>(std::lround((s.keepEarly ? 0.05 : 0.012) / hopSec)));
  const double Tl = N * hopSec;
  // Per bin: power decay per frame and over the early part.
  std::vector<double> late(static_cast<size_t>(bins));
  double tMax = 0;
  for (int b = 0; b < bins; ++b) {
    double t60 = s.useEstimate && s.estimate.valid() ? decayAt(s.estimate, b * rate / fft) : s.t60;
    t60 = std::clamp(t60, 0.05, 20.0);
    tMax = std::max(tMax, t60);
    late[static_cast<size_t>(b)] = std::pow(10.0, -6.0 * Tl / t60);
  }
  // Amount: how far the estimate is subtracted and the deepest cut.
  const double over = 0.8 + 1.0 * amt;
  const double floorG = dbToGain(-(8.0 + 22.0 * amt));
  const double relax = std::exp(-hopSec / 0.025);  // gains fall back over 25 ms (rise at once)
  const double aP = std::exp(-hopSec / 0.02);      // the power the model is fed: smoothed over 20 ms
  // Context before the range so the model knows the reverb already ringing at its start.
  const int64_t ctx = std::min<int64_t>(r.a, static_cast<int64_t>(std::min(3.0, tMax * 1.5) * rate));
  const int64_t x0 = r.a - ctx, x1 = r.b;
  std::vector<double> Ps(static_cast<size_t>(bins), 0.0), Pm(static_cast<size_t>(bins), 0.0), G(static_cast<size_t>(bins), 1.0), gt(static_cast<size_t>(bins)),
      est(static_cast<size_t>(bins));
  // Ring of the last N frames' smoothed power.
  std::vector<std::vector<double>> hist(static_cast<size_t>(N), std::vector<double>(static_cast<size_t>(bins), 0.0));
  std::vector<float> yL, yR;
  const bool ok = gainStft(in, x0, x1, fft, hop, p, 0.0, 0.97, yL, yR, [&](int64_t k, const std::vector<double>& P, std::vector<double>& g) {
    const size_t slot = static_cast<size_t>(k % N);  // holds frame k - N until overwritten
    // Late reverb (the statistical room model): what was heard N frames ago, decayed over
    // those N frames at the room's rate. Smoothed over neighbouring bins, so the estimate is
    // steady where the reverb is noise-like.
    const auto& past = hist[slot];
    for (int b = 0; b < bins; ++b) {
      const double l = past[static_cast<size_t>(std::max(0, b - 1))], m = past[static_cast<size_t>(b)], h = past[static_cast<size_t>(std::min(bins - 1, b + 1))];
      est[static_cast<size_t>(b)] = late[static_cast<size_t>(b)] * (0.25 * l + 0.5 * m + 0.25 * h);
    }
    for (int b = 0; b < bins; ++b) {
      const size_t bi = static_cast<size_t>(b);
      Ps[bi] = 0.55 * P[bi] + 0.45 * Ps[bi];
      Pm[bi] = aP * Pm[bi] + (1 - aP) * P[bi];
      double g2 = 1.0 - over * est[bi] / std::max(Ps[bi], 1e-24);
      g2 = std::max(g2, floorG * floorG);
      const double gi = std::sqrt(g2);
      G[bi] = gi > G[bi] ? gi : relax * G[bi] + (1 - relax) * gi;
    }
    // The ring keeps the smoothed power of this frame; a sound held on keeps feeding it, a
    // sound that stops leaves only its decay.
    hist[slot] = Pm;
    // Light smoothing across frequency (no isolated bins: no musical noise).
    for (int b = 0; b < bins; ++b) {
      const double l = G[static_cast<size_t>(std::max(0, b - 1))], m = G[static_cast<size_t>(b)], h = G[static_cast<size_t>(std::min(bins - 1, b + 1))];
      gt[static_cast<size_t>(b)] = 0.25 * l + 0.5 * m + 0.25 * h;
    }
    g = gt;
  });
  if (!ok) return c;
  Audio o = in;
  writeBack(o, r, x0, yL, yR, static_cast<int64_t>(rate * 0.01));
  report(p, 1.0);
  return withAudio(c, std::move(o));
}

// ------------------------------------------------------------------ declip
namespace {

struct Run {
  int64_t a, b;
  int sign;
};

// Clipped runs of one channel over r at the levels (thrPos > 0, thrNeg < 0).
std::vector<Run> clipRuns(const float* x, Range r, double thrPos, double thrNeg, bool flat, double peak) {
  std::vector<Run> out;
  const double flatTol = std::max(1e-4 * peak, 1e-7);
  for (int64_t i = r.a; i < r.b;) {
    const int sg = x[i] >= thrPos ? 1 : x[i] <= thrNeg ? -1 : 0;
    if (sg == 0) {
      ++i;
      continue;
    }
    int64_t j = i;
    double lo = x[i], hi = x[i];
    while (j + 1 < r.b && (sg > 0 ? x[j + 1] >= thrPos : x[j + 1] <= thrNeg)) {
      ++j;
      lo = std::min<double>(lo, x[j]), hi = std::max<double>(hi, x[j]);
    }
    if (j > i && (!flat || hi - lo <= flatTol)) out.push_back({i, j + 1, sg});
    i = j + 1;
  }
  return out;
}

// Autoregressive coefficients (a[0] = 1) of order p from x by the autocorrelation method.
std::vector<double> arCoeffs(const std::vector<double>& x, int p) {
  const size_t n = x.size();
  std::vector<double> w(n);
  for (size_t i = 0; i < n; ++i) w[i] = x[i] * (0.5 - 0.5 * std::cos(kTwoPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n)));
  std::vector<double> rr(static_cast<size_t>(p + 1), 0.0);
  for (int l = 0; l <= p; ++l)
    for (size_t i = static_cast<size_t>(l); i < n; ++i) rr[static_cast<size_t>(l)] += w[i] * w[i - static_cast<size_t>(l)];
  rr[0] *= 1.0 + 1e-9;
  rr[0] += 1e-18;
  std::vector<double> a(static_cast<size_t>(p + 1), 0.0), tmp;
  a[0] = 1;
  double err = rr[0];
  for (int i = 1; i <= p; ++i) {  // Levinson-Durbin
    double acc = rr[static_cast<size_t>(i)];
    for (int j = 1; j < i; ++j) acc += a[static_cast<size_t>(j)] * rr[static_cast<size_t>(i - j)];
    const double k = -acc / err;
    tmp = a;
    for (int j = 1; j < i; ++j) a[static_cast<size_t>(j)] = tmp[static_cast<size_t>(j)] + k * tmp[static_cast<size_t>(i - j)];
    a[static_cast<size_t>(i)] = k;
    err *= 1 - k * k;
    if (err <= 1e-20) break;
  }
  return a;
}

// The missing samples of xw (indices `miss`, sorted) that minimise the prediction error of the
// AR model a over the window, the rest fixed. Banded normal equations, Cholesky.
bool solveMissing(std::vector<double>& xw, const std::vector<int64_t>& miss, const std::vector<double>& a) {
  const int p = static_cast<int>(a.size()) - 1;
  const int64_t n = static_cast<int64_t>(xw.size());
  const int m = static_cast<int>(miss.size());
  if (m == 0) return true;
  std::vector<double> ra(static_cast<size_t>(p + 1), 0.0);  // autocorrelation of the coefficients
  for (int l = 0; l <= p; ++l)
    for (int j = 0; j + l <= p; ++j) ra[static_cast<size_t>(l)] += a[static_cast<size_t>(j)] * a[static_cast<size_t>(j + l)];
  // Residual of the window with the missing samples at zero.
  std::vector<double> x0 = xw;
  for (int64_t u : miss) x0[static_cast<size_t>(u)] = 0;
  std::vector<double> e(static_cast<size_t>(n), 0.0);
  for (int64_t t = p; t < n; ++t) {
    double v = 0;
    for (int j = 0; j <= p; ++j) v += a[static_cast<size_t>(j)] * x0[static_cast<size_t>(t - j)];
    e[static_cast<size_t>(t)] = v;
  }
  // B (band p) and rhs.
  const int bw = p;
  std::vector<double> B(static_cast<size_t>(m) * static_cast<size_t>(bw + 1), 0.0);  // B[u][u - d], d = 0..bw
  auto at = [&](int u, int d) -> double& { return B[static_cast<size_t>(u) * static_cast<size_t>(bw + 1) + static_cast<size_t>(d)]; };
  std::vector<double> rhs(static_cast<size_t>(m), 0.0);
  for (int u = 0; u < m; ++u) {
    const int64_t iu = miss[static_cast<size_t>(u)];
    for (int d = 0; d <= bw && u - d >= 0; ++d) {
      const int64_t lag = iu - miss[static_cast<size_t>(u - d)];
      if (lag <= p) at(u, d) = ra[static_cast<size_t>(lag)];
    }
    double v = 0;
    for (int j = 0; j <= p; ++j) {
      const int64_t t = iu + j;
      if (t >= p && t < n) v += a[static_cast<size_t>(j)] * e[static_cast<size_t>(t)];
    }
    rhs[static_cast<size_t>(u)] = -v;
  }
  // Banded Cholesky: B = L L^T, L stored in place.
  for (int u = 0; u < m; ++u) {
    for (int d = std::min(bw, u); d >= 0; --d) {
      const int v = u - d;
      double sum = at(u, d);
      for (int q = 1; q <= bw && v - q >= 0 && d + q <= bw; ++q) sum -= at(u, d + q) * at(v, q);
      if (d == 0) {
        if (sum <= 1e-30) return false;
        at(u, 0) = std::sqrt(sum);
      } else {
        at(u, d) = sum / at(v, 0);
      }
    }
  }
  std::vector<double> y(static_cast<size_t>(m));
  for (int u = 0; u < m; ++u) {
    double sum = rhs[static_cast<size_t>(u)];
    for (int d = 1; d <= bw && u - d >= 0; ++d) sum -= at(u, d) * y[static_cast<size_t>(u - d)];
    y[static_cast<size_t>(u)] = sum / at(u, 0);
  }
  for (int u = m - 1; u >= 0; --u) {
    double sum = y[static_cast<size_t>(u)];
    for (int d = 1; d <= bw && u + d < m; ++d) sum -= at(u + d, d) * y[static_cast<size_t>(u + d)];
    y[static_cast<size_t>(u)] = sum / at(u, 0);
  }
  for (int u = 0; u < m; ++u) xw[static_cast<size_t>(miss[static_cast<size_t>(u)])] = y[static_cast<size_t>(u)];
  return true;
}

}  // namespace

ClipScan findClipping(const Audio& a, Range r, bool autoLevel, double clipDb) {
  ClipScan sc;
  r.a = std::clamp<int64_t>(r.a, 0, a.frames());
  r.b = std::clamp<int64_t>(r.b, r.a, a.frames());
  const int nch = a.channels == 1 ? 1 : 2;
  for (int ch = 0; ch < nch; ++ch)
    for (int64_t i = r.a; i < r.b; ++i) sc.peakPos = std::max<double>(sc.peakPos, a.ch(ch)[i]), sc.peakNeg = std::min<double>(sc.peakNeg, a.ch(ch)[i]);
  const double peak = std::max(sc.peakPos, -sc.peakNeg);
  if (peak <= 1e-6) return sc;
  const bool au = autoLevel || clipDb <= -100;
  const double rel = au ? 1.0 - 5e-5 : dbToGain(std::min(0.0, clipDb));
  const double tp = std::max(1e-6, sc.peakPos * rel), tn = std::min(-1e-6, sc.peakNeg * rel);
  sc.level = tp;
  std::vector<Range> all;
  for (int ch = 0; ch < nch; ++ch)
    for (const Run& q : clipRuns(a.ch(ch), r, tp, tn, au, peak)) {
      all.push_back({q.a, q.b});
      sc.samples += q.b - q.a;
    }
  std::sort(all.begin(), all.end(), [](const Range& x, const Range& y) { return x.a < y.a; });
  for (const Range& q : all) {
    if (!sc.regions.empty() && q.a <= sc.regions.back().b) sc.regions.back().b = std::max(sc.regions.back().b, q.b);
    else sc.regions.push_back(q);
  }
  return sc;
}

Content declip(const Content& c, Range r, const DeclipSettings& s, Progress* p, int* fixedRegions) {
  r = rangeOrAll(c, r);
  if (fixedRegions) *fixedRegions = 0;
  if (r.empty()) return c;
  const ClipScan scan = findClipping(c.a(), r, s.autoLevel, s.clipDb);
  Audio o = c.a();
  const int64_t n = o.frames();
  const double peak = std::max(scan.peakPos, -scan.peakNeg);
  const bool au = s.autoLevel || s.clipDb <= -100;
  const double rel = au ? 1.0 - 5e-5 : dbToGain(std::min(0.0, s.clipDb));
  const double tp = std::max(1e-6, scan.peakPos * rel), tn = std::min(-1e-6, scan.peakNeg * rel);
  const int nch = o.channels == 1 ? 1 : 2;
  int64_t total = 0;
  std::vector<std::vector<Run>> runs(static_cast<size_t>(nch));
  for (int ch = 0; ch < nch; ++ch) {
    runs[static_cast<size_t>(ch)] = clipRuns(o.ch(ch), r, tp, tn, au, peak);
    total += static_cast<int64_t>(runs[static_cast<size_t>(ch)].size());
  }
  constexpr int64_t kLongest = 2048;  // longer gaps cannot be rebuilt from their surroundings
  int64_t done = 0;
  bool changed = false;
  for (int ch = 0; ch < nch; ++ch) {
    float* x = o.ch(ch);
    const auto& rs = runs[static_cast<size_t>(ch)];
    std::vector<uint8_t> clipped(static_cast<size_t>(n), 0);  // still to rebuild
    for (const Run& q : rs)
      if (q.b - q.a <= kLongest)
        for (int64_t i = q.a; i < q.b; ++i) clipped[static_cast<size_t>(i)] = static_cast<uint8_t>(q.sign > 0 ? 1 : 2);
    for (size_t k = 0; k < rs.size();) {
      if (stopped(p)) return c;
      report(p, 0.95 * static_cast<double>(done) / static_cast<double>(std::max<int64_t>(1, total)));
      // A cluster: this run and the following ones whose surroundings overlap (to 8192 frames).
      const int64_t len0 = rs[k].b - rs[k].a;
      const int64_t K = std::clamp<int64_t>(4 * len0, 512, 2048);
      int64_t w0 = std::max<int64_t>(0, rs[k].a - K), w1 = std::min<int64_t>(n, rs[k].b + K);
      size_t e = k + 1;
      while (e < rs.size() && rs[e].a - K < w1 && std::min<int64_t>(n, rs[e].b + K) - w0 <= 8192) {
        w1 = std::max(w1, std::min<int64_t>(n, rs[e].b + std::clamp<int64_t>(4 * (rs[e].b - rs[e].a), 512, 2048)));
        ++e;
      }
      done += static_cast<int64_t>(e - k);
      std::vector<double> xw(static_cast<size_t>(w1 - w0));
      for (int64_t i = w0; i < w1; ++i) xw[static_cast<size_t>(i - w0)] = x[i];
      const int p = static_cast<int>(std::clamp<int64_t>((w1 - w0) / 16, 16, 64));
      // Every clipped sample inside the window is rebuilt at once (other runs' too); those
      // within the model's order of an edge wait for a window of their own.
      std::vector<int64_t> miss;
      std::vector<int> sgn;
      for (int64_t i = w0 + p; i < w1 - p; ++i)
        if (clipped[static_cast<size_t>(i)]) miss.push_back(i - w0), sgn.push_back(clipped[static_cast<size_t>(i)] == 1 ? 1 : -1);
      bool good = !miss.empty() && miss.front() >= p && static_cast<int64_t>(xw.size()) - miss.back() > p;
      std::vector<double> ar;
      for (int it = 0; it < 4 && good; ++it) {
        ar = arCoeffs(xw, p);
        // Rebuild, then hold every sample that came out under its clip level at that level
        // (it was at least that loud) and rebuild the rest again.
        std::vector<int64_t> free = miss;
        std::vector<int> fs = sgn;
        for (int pass = 0; pass < 4 && good; ++pass) {
          good = solveMissing(xw, free, ar);
          std::vector<int64_t> nf;
          std::vector<int> ns;
          bool held = false;
          for (size_t u = 0; u < free.size(); ++u) {
            const size_t i = static_cast<size_t>(free[u]);
            if (fs[u] > 0 ? xw[i] < tp : xw[i] > tn) {
              xw[i] = fs[u] > 0 ? tp : tn;
              held = true;
            } else {
              nf.push_back(free[u]), ns.push_back(fs[u]);
            }
          }
          if (!held) break;
          free.swap(nf), fs.swap(ns);
          if (free.empty()) break;
        }
      }
      // A rebuilt peak far above the clip level is a failed solve: keep the clipped samples.
      if (good)
        for (int64_t u : miss)
          if (std::fabs(xw[static_cast<size_t>(u)]) > 4.0 * peak) good = false;
      if (good) {
        for (int64_t u : miss) {
          x[w0 + u] = static_cast<float>(xw[static_cast<size_t>(u)]);
          clipped[static_cast<size_t>(w0 + u)] = 0;
        }
        changed = true;
      }
      k = e;
    }
  }
  if (!changed) return c;
  if (fixedRegions) *fixedRegions = static_cast<int>(scan.regions.size());
  if (s.fitGain) {
    double pk = 0;
    for (int ch = 0; ch < nch; ++ch)
      for (int64_t i = r.a; i < r.b; ++i) pk = std::max<double>(pk, std::fabs(o.ch(ch)[i]));
    const double ceil = dbToGain(std::min(0.0, s.ceilingDb));
    if (pk > ceil) {
      const double g = ceil / pk;
      const int64_t ramp = std::min<int64_t>(static_cast<int64_t>(o.rate * 0.02), r.len() / 4);
      for (int ch = 0; ch < nch; ++ch)
        for (int64_t i = r.a; i < r.b; ++i) {
          double w = 1;  // the gain eases in and out at edges inside the file
          if (r.a > 0 && i - r.a < ramp) w = static_cast<double>(i - r.a) / static_cast<double>(ramp);
          if (r.b < n && r.b - 1 - i < ramp) w = std::min(w, static_cast<double>(r.b - 1 - i) / static_cast<double>(ramp));
          o.ch(ch)[i] = static_cast<float>(o.ch(ch)[i] * (1.0 - w + w * g));
        }
    }
  }
  report(p, 1.0);
  return withAudio(c, std::move(o));
}

// ------------------------------------------------------------------ denoise
namespace {

// Exponential integral E1(x), x > 0 (series below 1, rational approximation above).
double expint1(double x) {
  x = std::max(x, 1e-10);
  if (x < 1.0) {
    double sum = 0, term = 1;
    for (int k = 1; k < 30; ++k) {
      term *= -x / k;
      sum -= term / k;
    }
    return -0.5772156649015329 - std::log(x) + sum;
  }
  const double num = x * x + 2.334733 * x + 0.250621, den = x * x + 3.330657 * x + 1.681534;
  return std::exp(-x) / x * num / den;
}

// One channel through the noise estimator at one frame size; out = the cleaned signal.
bool denoiseChannel(const float* x, int64_t n, double rate, int fft, const DenoiseSettings& s, const float* tw, Progress* p, double p0, double p1,
                    std::vector<float>& out) {
  const int hop = fft / 4, bins = fft / 2 + 1;
  const double hopSec = hop / rate;
  // The profile (mean magnitudes at its own frame size) as power per bin at this size.
  std::vector<double> prof(static_cast<size_t>(bins), 0.0);
  const bool haveProf = s.profile.valid();
  if (haveProf) {
    const int P = s.profile.fft, pb = P / 2 + 1;
    const double pr = s.profile.rate > 0 ? s.profile.rate : rate;
    const double scale = static_cast<double>(fft) / static_cast<double>(P) * (4.0 / kPi);
    for (int b = 0; b < bins; ++b) {
      const double hz = b * rate / fft;
      const double lo = std::max(0.0, (hz - 0.5 * rate / fft) * P / pr), hi = std::min(pb - 1.0, (hz + 0.5 * rate / fft) * P / pr);
      double acc = 0;
      int cnt = 0;
      for (int q = static_cast<int>(std::ceil(lo)); q <= static_cast<int>(std::floor(hi)); ++q, ++cnt) acc += static_cast<double>(s.profile.mag[static_cast<size_t>(q)]) * s.profile.mag[static_cast<size_t>(q)];
      if (cnt == 0) {  // finer than the profile: interpolate
        const double fq = std::clamp(hz * P / pr, 0.0, pb - 1.0);
        const int q0 = static_cast<int>(fq), q1 = std::min(pb - 1, q0 + 1);
        const double m = s.profile.mag[static_cast<size_t>(q0)] * (1 - (fq - q0)) + s.profile.mag[static_cast<size_t>(q1)] * (fq - q0);
        acc = m * m, cnt = 1;
      }
      prof[static_cast<size_t>(b)] = acc / cnt * scale;
    }
  }
  const bool track = s.adaptive || !haveProf;
  const double over = std::pow(10.0, (std::clamp(s.sensitivity, 0.0, 1.0) - 0.5) * 12.0 / 10.0);  // -6..+6 dB
  const double gmin = dbToGain(-std::clamp(s.reductionDb, 0.0, 60.0));
  const double alpha = 0.92 + 0.07 * std::clamp(s.smoothing, 0.0, 1.0);  // decision-directed weight per 23 ms of hops
  const double aDD = std::pow(alpha, hopSec / 0.0116);
  const double ximin = std::max(gmin * gmin * 0.5, 1e-4);
  // Minimum statistics: the smoothed power's minimum over ~1.5 s (8 sub-windows; 3 s for tonal
  // noise, which must outlast held notes), with its bias.
  const int V = std::max(2, static_cast<int>(std::lround((s.character == 1 ? 0.38 : 0.19) / hopSec)));
  constexpr int U = 8;
  const double aS = std::pow(0.85, hopSec / 0.0116);
  std::vector<double> ps(static_cast<size_t>(bins), 0.0), curMin(static_cast<size_t>(bins), 1e30), lam(prof);
  std::vector<std::vector<double>> sub(U, std::vector<double>(static_cast<size_t>(bins), 1e30));
  std::vector<double> prevA2(static_cast<size_t>(bins), 0.0), g(static_cast<size_t>(bins), 1.0);
  std::vector<double> capped;
  int subPos = 0, subCount = 0;
  bool first = true;
  const Fft F(fft);
  const std::vector<double> w = hannWindow(fft);
  std::vector<double> acc(static_cast<size_t>(n), 0.0), norm(static_cast<size_t>(n), 0.0);
  std::vector<cplx> X(static_cast<size_t>(fft));
  const int64_t frames = n / hop + 2;
  for (int64_t k = 0; k < frames; ++k) {
    if ((k & 63) == 0) {
      if (stopped(p)) return false;
      report(p, p0 + (p1 - p0) * static_cast<double>(k) / static_cast<double>(frames));
    }
    const int64_t c0 = k * hop - fft / 2;
    for (int i = 0; i < fft; ++i) {
      const int64_t t = c0 + i;
      X[static_cast<size_t>(i)] = cplx(t >= 0 && t < n ? x[t] * w[static_cast<size_t>(i)] : 0.0, 0.0);
    }
    F.forward(X);
    if (track) {
      for (int b = 0; b < bins; ++b) {
        const size_t bi = static_cast<size_t>(b);
        const double y2 = std::norm(X[bi]);
        ps[bi] = first ? y2 : aS * ps[bi] + (1 - aS) * y2;
        curMin[bi] = std::min(curMin[bi], ps[bi]);
      }
      if (++subCount >= V || first) {
        if (!first) subCount = 0;
        sub[static_cast<size_t>(subPos)] = curMin;
        subPos = (subPos + 1) % U;
        std::fill(curMin.begin(), curMin.end(), 1e30);
      }
      for (int b = 0; b < bins; ++b) {
        double m = 1e30;
        for (int u = 0; u < U; ++u) m = std::min(m, sub[static_cast<size_t>(u)][static_cast<size_t>(b)]);
        m = std::min(m, curMin[static_cast<size_t>(b)]);
        const double t = m < 1e29 ? m * 1.9 : ps[static_cast<size_t>(b)];  // the minimum's bias
        lam[static_cast<size_t>(b)] = haveProf ? std::clamp(t, prof[static_cast<size_t>(b)] * 0.25, prof[static_cast<size_t>(b)] * 4.0 + 1e-24) : t;
      }
      // Broadband noise has a smooth spectrum: a narrow peak in the estimate is a held note, not
      // noise, so the estimate stays within 3 dB of the median of its neighbourhood.
      if (s.character != 1 && !haveProf) {
        const int half = std::max(3, fft / 128);
        std::vector<double> nb;
        capped.resize(lam.size());
        for (int b = 0; b < bins; ++b) {
          nb.assign(lam.begin() + std::max(0, b - half), lam.begin() + std::min(bins, b + half + 1));
          std::nth_element(nb.begin(), nb.begin() + static_cast<long>(nb.size() / 2), nb.end());
          capped[static_cast<size_t>(b)] = std::min(lam[static_cast<size_t>(b)], 2.0 * nb[nb.size() / 2]);
        }
        lam.swap(capped);
      }
    }
    first = false;
    // At a transient the decision-directed estimate follows at once (no lag on the attack).
    const double twk = tw && k * hop < n ? static_cast<double>(tw[k * hop]) : 0.0;
    const double aK = aDD * (1.0 - 0.9 * twk);
    for (int b = 0; b < bins; ++b) {
      const size_t bi = static_cast<size_t>(b);
      const double y2 = std::norm(X[bi]);
      const double L = std::max(lam[bi] * over, 1e-24);
      const double gam = y2 / L;
      double xi = aK * prevA2[bi] / L + (1 - aK) * std::max(gam - 1.0, 0.0);
      xi = std::max(xi, ximin);
      const double v = xi / (1 + xi) * gam;
      double G = xi / (1 + xi) * std::exp(0.5 * expint1(v));
      G = std::clamp(G, gmin, 1.0);
      g[bi] = G;
      prevA2[bi] = G * G * y2;
    }
    for (int b = 0; b < bins; ++b) {  // light smoothing across frequency
      const size_t bi = static_cast<size_t>(b);
      const double G = fft >= 1024 ? 0.25 * g[static_cast<size_t>(std::max(0, b - 1))] + 0.5 * g[bi] + 0.25 * g[static_cast<size_t>(std::min(bins - 1, b + 1))] : g[bi];
      X[bi] *= G;
      if (b > 0 && b < fft / 2) X[static_cast<size_t>(fft - b)] *= G;
    }
    F.inverse(X);
    for (int i = 0; i < fft; ++i) {
      const int64_t t = c0 + i;
      if (t < 0 || t >= n) continue;
      acc[static_cast<size_t>(t)] += X[static_cast<size_t>(i)].real() * w[static_cast<size_t>(i)];
      norm[static_cast<size_t>(t)] += w[static_cast<size_t>(i)] * w[static_cast<size_t>(i)];
    }
  }
  out.resize(static_cast<size_t>(n));
  for (int64_t t = 0; t < n; ++t) out[static_cast<size_t>(t)] = static_cast<float>(norm[static_cast<size_t>(t)] > 1e-9 ? acc[static_cast<size_t>(t)] / norm[static_cast<size_t>(t)] : 0.0);
  return true;
}

// How much of each sample belongs to a transient (0..1): the 5 ms level jumping above the 50 ms
// before it, held from `lead` frames before the jump and released over 40 ms.
std::vector<float> transientWeight(const Audio& a, int64_t x0, int64_t n, int64_t lead) {
  std::vector<float> w(static_cast<size_t>(n), 0.f);
  const int64_t s5 = std::max<int64_t>(8, static_cast<int64_t>(a.rate * 0.005)), s50 = s5 * 10;
  std::vector<double> e(static_cast<size_t>(n + 1), 0.0);
  for (int64_t i = 0; i < n; ++i) {
    const double v = a.channels == 1 ? a.L[static_cast<size_t>(x0 + i)] : 0.5 * (a.L[static_cast<size_t>(x0 + i)] + a.R[static_cast<size_t>(x0 + i)]);
    e[static_cast<size_t>(i + 1)] = e[static_cast<size_t>(i)] + v * v;
  }
  auto mean = [&](int64_t f0, int64_t f1) {
    f0 = std::clamp<int64_t>(f0, 0, n), f1 = std::clamp<int64_t>(f1, 0, n);
    return f1 > f0 ? (e[static_cast<size_t>(f1)] - e[static_cast<size_t>(f0)]) / static_cast<double>(f1 - f0) : 0.0;
  };
  const double rel = std::exp(-1.0 / (a.rate * 0.04));
  double cur = 0;
  for (int64_t i = 0; i < n; ++i) {
    const double now = mean(i, i + s5), before = mean(i - s50, i);
    const double r = 10.0 * std::log10((now + 1e-12) / (before + 1e-12));
    const double t = now > 1e-8 ? std::clamp((r - 6.0) / 6.0, 0.0, 1.0) : 0.0;
    cur = std::max(t, cur * rel);
    w[static_cast<size_t>(i)] = static_cast<float>(cur);
  }
  // Lead: a long frame smears an attack this far ahead of it.
  const double back = std::exp(-1.0 / std::max<double>(1, static_cast<double>(lead) * 0.5));
  double b = 0;
  for (int64_t i = n - 1; i >= 0; --i) {
    b = std::max<double>(w[static_cast<size_t>(i)], b * back);
    w[static_cast<size_t>(i)] = static_cast<float>(b);
  }
  return w;
}

}  // namespace

Content denoiseHq(const Content& c, Range r, const DenoiseSettings& s, Progress* p) {
  r = rangeOrAll(c, r);
  if (r.empty()) return c;
  const Audio& in = c.a();
  const double rate = in.rate;
  const int fl = std::clamp(nextPow2(static_cast<int64_t>(rate * (s.character == 1 ? 0.085 : 0.042))), 512, 16384);
  const int fs = std::clamp(nextPow2(static_cast<int64_t>(rate * 0.010)), 128, 2048);
  // Context: a second before the range lets the estimates settle before it starts.
  const int64_t ctx = std::min<int64_t>(r.a, static_cast<int64_t>(rate));
  const int64_t x0 = r.a - ctx, n = r.b - x0;
  const std::vector<float> tw = transientWeight(in, x0, n, fl / 2);
  const int nch = in.channels == 1 ? 1 : 2;
  std::vector<float> y[2];
  for (int ch = 0; ch < nch; ++ch) {
    std::vector<float> yl, ys;
    const double base = static_cast<double>(ch) / nch;
    if (!denoiseChannel(in.ch(ch) + x0, n, rate, fl, s, tw.data(), p, base, base + 0.7 / nch, yl)) return c;
    if (!denoiseChannel(in.ch(ch) + x0, n, rate, fs, s, tw.data(), p, base + 0.7 / nch, base + 1.0 / nch, ys)) return c;
    y[ch].resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      const float wt = tw[static_cast<size_t>(i)];
      y[ch][static_cast<size_t>(i)] = yl[static_cast<size_t>(i)] * (1 - wt) + ys[static_cast<size_t>(i)] * wt;
    }
  }
  if (nch == 1) y[1] = y[0];
  Audio o = in;
  writeBack(o, r, x0, y[0], y[1], static_cast<int64_t>(rate * 0.01));
  report(p, 1.0);
  return withAudio(c, std::move(o));
}

// ------------------------------------------------------------------ declick
namespace {
// Clicks of one channel over r (the model refitted every 1024 frames on 2048).
std::vector<Range> clicksOf(const float* x, int64_t n, Range r, double rate, const DeclickSettings& s, Progress* p, double p0, double p1, bool& ok) {
  ok = true;
  std::vector<Range> out;
  constexpr int P = 24, B = 2048, H = 1024;
  const double K = 3.0 + (1.0 - std::clamp(s.sensitivity, 0.0, 1.0)) * 9.0;
  const int64_t maxLen = std::max<int64_t>(1, static_cast<int64_t>(rate * std::clamp(s.maxClickMs, 0.05, 20.0) * 0.001));
  std::vector<uint8_t> bad(static_cast<size_t>(r.len()), 0);
  std::vector<float> res(static_cast<size_t>(r.len()), 0.f);  // |forward residual| over r
  constexpr int64_t M = 512;  // the residual is also made this far either side, for local levels
  std::vector<double> blk(B), ef, eb, mag, m;
  for (int64_t b0 = r.a; b0 < r.b; b0 += H) {
    if (stopped(p)) {
      ok = false;
      return out;
    }
    report(p, p0 + (p1 - p0) * static_cast<double>(b0 - r.a) / static_cast<double>(std::max<int64_t>(1, r.len())));
    const int64_t w0 = std::clamp<int64_t>(b0 - (B - H) / 2, 0, std::max<int64_t>(0, n - B));
    if (n < B) break;
    for (int i = 0; i < B; ++i) blk[static_cast<size_t>(i)] = x[w0 + i];
    const std::vector<double> a = arCoeffs(blk, P);
    const int64_t h1 = std::min<int64_t>(H, r.b - b0);
    const int64_t e0 = std::max<int64_t>(0, b0 - M), e1 = std::min<int64_t>(n, b0 + h1 + M);
    ef.assign(static_cast<size_t>(e1 - e0), 0.0), eb.assign(static_cast<size_t>(e1 - e0), 0.0);
    for (int64_t t = e0; t < e1; ++t) {
      double f = 0, bk = 0;
      for (int j = 0; j <= P; ++j) {
        f += a[static_cast<size_t>(j)] * (t - j >= 0 ? x[t - j] : 0.0);
        bk += a[static_cast<size_t>(j)] * (t + j < n ? x[t + j] : 0.0);
      }
      ef[static_cast<size_t>(t - e0)] = f, eb[static_cast<size_t>(t - e0)] = bk;
    }
    // Robust level of the prediction error: the median of |e| over the block, raised where a
    // loud, noise-like passage (a snare, a hat) is around: the lower third of |e| over the
    // 23 ms around each stretch of 64 frames (clicks, even dense crackle, hardly move it).
    mag.resize(ef.size());
    for (size_t i = 0; i < ef.size(); ++i) mag[i] = std::fabs(ef[i]);
    m.assign(mag.begin() + (b0 - e0), mag.begin() + (b0 - e0 + h1));
    std::nth_element(m.begin(), m.begin() + static_cast<long>(m.size() / 2), m.end());
    const double sigBlock = m[m.size() / 2] / 0.6745;
    for (int64_t s0 = b0; s0 < b0 + h1; s0 += 64) {
      const int64_t m0 = std::max(e0, s0 + 32 - M), m1 = std::min(e1, s0 + 32 + M);
      m.assign(mag.begin() + (m0 - e0), mag.begin() + (m1 - e0));
      const size_t q = m.size() * 3 / 10;
      std::nth_element(m.begin(), m.begin() + static_cast<long>(q), m.end());
      const double sigma = std::max({1e-7, sigBlock, 0.7 * m[q] / 0.385});
      for (int64_t i = s0; i < std::min(b0 + h1, s0 + 64); ++i) {
        res[static_cast<size_t>(i - r.a)] = static_cast<float>(std::fabs(ef[static_cast<size_t>(i - e0)]) / sigma);
        if (std::fabs(ef[static_cast<size_t>(i - e0)]) > K * sigma && std::fabs(eb[static_cast<size_t>(i - e0)]) > K * sigma) bad[static_cast<size_t>(i - r.a)] = 1;
      }
    }
  }
  // Samples near a detection (left out when measuring what surrounds a click: crackle is dense).
  std::vector<uint8_t> nearBad(static_cast<size_t>(r.len()), 0);
  for (int64_t i = 0; i < r.len(); ++i)
    if (bad[static_cast<size_t>(i)])
      for (int64_t q = std::max<int64_t>(0, i - 12); q < std::min(r.len(), i + 13); ++q) nearBad[static_cast<size_t>(q)] = 1;
  auto skip = [&](int64_t i) { return i >= r.a && i < r.b && nearBad[static_cast<size_t>(i - r.a)]; };
  // Energy of x over [f0, f1) (clamped), for telling a click from the start of a new sound.
  auto power = [&](int64_t f0, int64_t f1, bool slope) {
    f0 = std::clamp<int64_t>(f0, 1, n), f1 = std::clamp<int64_t>(f1, 1, n);
    double e = 0;
    int64_t cnt = 0;
    for (int64_t i = f0; i < f1; ++i) {
      if (skip(i)) continue;
      const double v = slope ? static_cast<double>(x[i]) - x[i - 1] : x[i];
      e += v * v, ++cnt;
    }
    return cnt > 0 ? e / static_cast<double>(cnt) : 0.0;
  };
  // The residual around a run (in units of the local level), other detections left out.
  auto around = [&](int64_t a0, int64_t a1) {
    double e = 0;
    const int64_t gw = std::max<int64_t>(8, static_cast<int64_t>(rate * 0.002));
    int64_t cnt = 0;
    for (int64_t i = a0 - gw; i < a1 + P + gw; ++i) {
      if (i >= a0 - 4 && i < a1 + P + 4) continue;  // the forward residual of a click rings on for the model order
      if (i < r.a || i >= r.b || nearBad[static_cast<size_t>(i - r.a)]) continue;
      e += static_cast<double>(res[static_cast<size_t>(i - r.a)]) * res[static_cast<size_t>(i - r.a)], ++cnt;
    }
    return cnt > 8 ? std::sqrt(e / static_cast<double>(cnt)) : 0.0;
  };
  const int64_t g1 = 2, g2 = std::max<int64_t>(8, static_cast<int64_t>(rate * 0.003));
  // Runs (gaps of up to 3 frames joined), a frame of margin each side, short ones only.
  for (int64_t i = 0; i < r.len();) {
    if (!bad[static_cast<size_t>(i)]) {
      ++i;
      continue;
    }
    int64_t j = i;
    for (int64_t q = i; q < r.len() && q <= j + 4; ++q)
      if (bad[static_cast<size_t>(q)]) j = q;
    // A margin either side: 2 frames before, half the length after (a pop's decaying tail).
    const int64_t a0 = std::max<int64_t>(0, r.a + i - 2), a1 = std::min<int64_t>(n, r.a + j + 1 + std::max<int64_t>(2, (j - i + 1) / 2));
    // A click leaves the sound after it as it was before it; a hit or a new note starts louder.
    // (Its slope too: a hat over a kick's tail adds little level but much slope.)
    // A click stands out of its surroundings; inside a noisy sound (a hat, a snare, breath)
    // the residual is as large all around.
    double pk = 0;
    for (int64_t q = a0; q < a1; ++q) pk = std::max(pk, static_cast<double>(res[static_cast<size_t>(std::clamp<int64_t>(q, r.a, r.b - 1) - r.a)]));
    const double lvl = std::max(1.0, around(a0, a1));
    const bool alone = pk >= 0.6 * K * lvl;
    // (A very strong one is a click wherever it is.)
    const bool onset = pk < 4.0 * K * lvl && (power(a1 + g1, a1 + g2, false) > 4.0 * power(a0 - g2, a0 - g1, false) + 1e-10 ||
                                              power(a1 + g1, a1 + g2, true) > 4.0 * power(a0 - g2, a0 - g1, true) + 1e-10);
    if (j - i + 1 <= maxLen && !onset && alone) out.push_back({a0, a1});
    i = j + 1;
  }
  return out;
}

// Rebuilds x over each run from the model of the audio around it.
void rebuild(float* x, int64_t n, const std::vector<Range>& found) {
  // Runs whose margins touch become one (every missing frame once).
  std::vector<Range> runs;
  for (const Range& q : found) {
    if (!runs.empty() && q.a <= runs.back().b) runs.back().b = std::max(runs.back().b, q.b);
    else runs.push_back(q);
  }
  for (size_t k = 0; k < runs.size();) {
    // Neighbouring runs share one window.
    int64_t w0 = std::max<int64_t>(0, runs[k].a - 512), w1 = std::min<int64_t>(n, runs[k].b + 512);
    size_t e = k + 1;
    while (e < runs.size() && runs[e].a - 512 < w1 && runs[e].b + 512 - w0 <= 8192) w1 = std::min<int64_t>(n, runs[e].b + 512), ++e;
    std::vector<double> xw(static_cast<size_t>(w1 - w0));
    for (int64_t i = w0; i < w1; ++i) xw[static_cast<size_t>(i - w0)] = x[i];
    std::vector<int64_t> miss;
    for (size_t q = k; q < e; ++q)
      for (int64_t i = runs[q].a; i < runs[q].b; ++i) miss.push_back(i - w0);
    constexpr int P = 32;
    bool good = !miss.empty() && miss.front() >= P && static_cast<int64_t>(xw.size()) - miss.back() > P;
    double peak = 0;
    for (double v : xw) peak = std::max(peak, std::fabs(v));
    for (int it = 0; it < 2 && good; ++it) {
      std::vector<double> est = xw;  // the model from the audio with the gaps filled so far
      if (it == 0) {
        for (int64_t u : miss) est[static_cast<size_t>(u)] = 0;
      }
      good = solveMissing(xw, miss, arCoeffs(est, P));
    }
    if (good)
      for (int64_t u : miss)
        if (!std::isfinite(xw[static_cast<size_t>(u)]) || std::fabs(xw[static_cast<size_t>(u)]) > 2 * peak + 1e-6) good = false;
    if (good)
      for (int64_t u : miss) x[w0 + u] = static_cast<float>(xw[static_cast<size_t>(u)]);
    k = e;
  }
}
}  // namespace

std::vector<Range> findClicks(const Audio& a, Range r, const DeclickSettings& s, Progress* p) {
  r.a = std::clamp<int64_t>(r.a, 0, a.frames());
  r.b = std::clamp<int64_t>(r.b, r.a, a.frames());
  std::vector<Range> all;
  const int nch = a.channels == 1 ? 1 : 2;
  for (int ch = 0; ch < nch; ++ch) {
    bool ok = true;
    auto c = clicksOf(a.ch(ch), a.frames(), r, a.rate, s, p, static_cast<double>(ch) / nch, static_cast<double>(ch + 1) / nch, ok);
    if (!ok) return {};
    all.insert(all.end(), c.begin(), c.end());
  }
  std::sort(all.begin(), all.end(), [](const Range& x, const Range& y) { return x.a < y.a; });
  std::vector<Range> out;
  for (const Range& q : all) {
    if (!out.empty() && q.a <= out.back().b) out.back().b = std::max(out.back().b, q.b);
    else out.push_back(q);
  }
  return out;
}

Content declickHq(const Content& c, Range r, const DeclickSettings& s, Progress* p, int* fixed) {
  r = rangeOrAll(c, r);
  if (fixed) *fixed = 0;
  if (r.empty()) return c;
  Audio o = c.a();
  const int nch = o.channels == 1 ? 1 : 2;
  int count = 0;
  bool any = false;
  for (int ch = 0; ch < nch; ++ch) {
    bool ok = true;
    const auto runs = clicksOf(o.ch(ch), o.frames(), r, o.rate, s, p, 0.9 * ch / nch, 0.9 * (ch + 1) / nch, ok);
    if (!ok) return c;
    if (runs.empty()) continue;
    rebuild(o.ch(ch), o.frames(), runs);
    count += static_cast<int>(runs.size());
    any = true;
  }
  if (!any) return c;
  if (fixed) *fixed = nch == 2 ? (count + 1) / 2 : count;
  report(p, 1.0);
  return withAudio(c, std::move(o));
}

// ------------------------------------------------------------------ gate
Content gateHq(const Content& c, Range r, const GateSettings& s, Progress* p) {
  r = rangeOrAll(c, r);
  if (r.empty()) return c;
  Audio a = c.a();
  const double sr = a.rate;
  const double open = dbToGain(s.thresholdDb), close = dbToGain(s.thresholdDb - std::max(0.0, s.hysteresisDb));
  const double floorDb = s.floorDb <= -90 ? -120.0 : s.floorDb;
  const double up = -floorDb / std::max(1.0, s.attackMs * 0.001 * sr);     // dB per frame
  const double down = -floorDb / std::max(1.0, s.releaseMs * 0.001 * sr);
  const int64_t hold = static_cast<int64_t>(std::max(0.0, s.holdMs) * 0.001 * sr);
  const int64_t look = static_cast<int64_t>(std::max(0.0, s.attackMs) * 0.001 * sr);
  const double envRel = std::exp(-1.0 / (0.010 * sr));
  // The peak envelope (instant rise, 10 ms fall), read `look` frames ahead.
  std::vector<float> env(static_cast<size_t>(r.len()));
  double e = 0;
  for (int64_t i = r.a; i < r.b; ++i) {
    const double v = std::max(std::fabs(a.L[static_cast<size_t>(i)]), std::fabs(a.R[static_cast<size_t>(i)]));
    e = std::max(v, e * envRel);
    env[static_cast<size_t>(i - r.a)] = static_cast<float>(e);
  }
  bool isOpen = false;
  int64_t held = 0;
  double gdb = floorDb;
  std::vector<float> gains(static_cast<size_t>(r.len()));
  for (int64_t i = 0; i < r.len(); ++i) {
    if ((i & 0xFFFF) == 0 && stopped(p)) return c;
    const double lv = env[static_cast<size_t>(std::min(r.len() - 1, i + look))];
    if (lv >= open) isOpen = true, held = hold;
    else if (isOpen && lv < close) {
      if (held > 0) --held;
      else isOpen = false;
    } else if (isOpen) {
      held = hold;
    }
    gdb = isOpen ? std::min(0.0, gdb + up) : std::max(floorDb, gdb - down);
    gains[static_cast<size_t>(i)] = gdb <= -119.9 ? 0.f : static_cast<float>(dbToGain(gdb));
  }
  for (int ch = 0; ch < 2; ++ch)
    for (int64_t i = r.a; i < r.b; ++i) a.ch(ch)[i] *= gains[static_cast<size_t>(i - r.a)];
  return withAudio(c, std::move(a));
}

// ------------------------------------------------------------------ spectral brush
Content spectralGainSoft(const Content& c, const std::vector<SpectralRect>& rects, double gainDb, Progress* p) {
  if (rects.empty() || c.frames() == 0) return c;
  const Audio& in = c.a();
  const double rate = in.rate;
  const int fft = std::clamp(nextPow2(static_cast<int64_t>(rate * 0.085)), 1024, 16384), hop = fft / 4, bins = fft / 2 + 1;
  const double ft = rate * 0.015, fo = 1.0 / 6.0;  // edge widths: frames, octaves
  int64_t lo = c.frames(), hi = 0;
  for (const auto& q : rects) lo = std::min(lo, q.f0), hi = std::max(hi, q.f1);
  const Range r = clampRange(c, {lo - static_cast<int64_t>(ft) - fft, hi + static_cast<int64_t>(ft) + fft});
  if (r.empty()) return c;
  std::vector<float> yL, yR;
  std::vector<double> m(static_cast<size_t>(bins));
  const bool ok = gainStft(in, r.a, r.b, fft, hop, p, 0.0, 0.97, yL, yR, [&](int64_t k, const std::vector<double>&, std::vector<double>& g) {
    const double t = static_cast<double>(r.a + k * hop);
    std::fill(m.begin(), m.end(), 0.0);
    for (const auto& q : rects) {
      const double dt = t < q.f0 ? q.f0 - t : t > q.f1 ? t - q.f1 : 0.0;
      if (dt >= ft) continue;
      const double wt = 0.5 + 0.5 * std::cos(kPi * dt / ft);
      const double l0 = std::log2(std::max(1.0, q.hz0)), l1 = std::log2(std::max(1.0, q.hz1));
      for (int b = 1; b < bins; ++b) {
        const double lf = std::log2(b * rate / fft);
        const double df = lf < l0 ? l0 - lf : lf > l1 ? lf - l1 : 0.0;
        if (df >= fo) continue;
        m[static_cast<size_t>(b)] = std::max(m[static_cast<size_t>(b)], wt * (0.5 + 0.5 * std::cos(kPi * df / fo)));
      }
    }
    for (int b = 0; b < bins; ++b) g[static_cast<size_t>(b)] = dbToGain(gainDb * m[static_cast<size_t>(b)]);
  });
  if (!ok) return c;
  Audio o = in;
  writeBack(o, {std::max<int64_t>(0, lo - static_cast<int64_t>(ft)), std::min<int64_t>(c.frames(), hi + static_cast<int64_t>(ft))}, r.a, yL, yR, 0);
  report(p, 1.0);
  return withAudio(c, std::move(o));
}

// ------------------------------------------------------------------ drum loop stretch
namespace {
// The hits of a loop, to the sample: where the energy of the next 5 ms jumps over that of the
// 5 ms before (of the signal, or of its slope, which hears hats and snares over a kick's tail),
// then the exact frame by the same jump over 1 ms. Frames of r, from r.a.
std::vector<int64_t> loopHits(const Audio& a, Range r, double sensitivity) {
  std::vector<int64_t> out;
  const int64_t n = r.len();
  if (n < 64) return out;
  std::vector<double> e1(static_cast<size_t>(n + 1), 0.0), e2(static_cast<size_t>(n + 1), 0.0);
  double prev = 0;
  for (int64_t i = 0; i < n; ++i) {
    const double v = a.channels == 1 ? a.L[static_cast<size_t>(r.a + i)] : 0.5 * (a.L[static_cast<size_t>(r.a + i)] + a.R[static_cast<size_t>(r.a + i)]);
    const double d = (v - prev) * 4.0;  // the slope, scaled to a level like the signal's around 3 kHz
    prev = v;
    e1[static_cast<size_t>(i + 1)] = e1[static_cast<size_t>(i)] + v * v;
    e2[static_cast<size_t>(i + 1)] = e2[static_cast<size_t>(i)] + d * d;
  }
  auto sum = [&](const std::vector<double>& e, int64_t f0, int64_t f1) {
    f0 = std::clamp<int64_t>(f0, 0, n), f1 = std::clamp<int64_t>(f1, 0, n);
    return e[static_cast<size_t>(f1)] - e[static_cast<size_t>(f0)];
  };
  const int64_t W = std::max<int64_t>(16, static_cast<int64_t>(a.rate * 0.005));
  const int64_t w1 = std::max<int64_t>(8, static_cast<int64_t>(a.rate * 0.001));
  double top1 = 0, top2 = 0;
  for (int64_t i = 0; i + W <= n; i += W / 2) top1 = std::max(top1, sum(e1, i, i + W)), top2 = std::max(top2, sum(e2, i, i + W));
  if (top1 <= 0) return out;
  const double sens = std::clamp(sensitivity, 0.0, 1.0);
  const double floorDb = -(30.0 + 30.0 * sens);  // quieter than this (below the loudest 5 ms) is not a hit
  const double e1min = top1 * std::pow(10.0, floorDb / 10.0), e2min = top2 * std::pow(10.0, floorDb / 10.0);
  const double eps1 = top1 * 1e-6, eps2 = top2 * 1e-6 + 1e-30;
  auto jump = [&](int64_t at, int64_t w) {  // dB: the louder of the two energies' jumps at `at`
    const double a1 = sum(e1, at, at + w), b1 = sum(e1, at - w, at), a2 = sum(e2, at, at + w), b2 = sum(e2, at - w, at);
    const double j1 = a1 * W / w >= e1min ? 10.0 * std::log10((a1 + eps1) / (b1 + eps1)) : 0.0;
    const double j2 = a2 * W / w >= e2min ? 10.0 * std::log10((a2 + eps2) / (b2 + eps2)) : 0.0;
    return std::max(j1, j2);
  };
  const int64_t step = 32;
  const int64_t K = (n + step - 1) / step;
  std::vector<double> odf(static_cast<size_t>(K));
  for (int64_t k = 0; k < K; ++k) odf[static_cast<size_t>(k)] = jump(k * step, W);
  const double thr = 5.0 + 10.0 * (1.0 - sens);
  const int64_t near = std::max<int64_t>(1, static_cast<int64_t>(a.rate * 0.03) / step);
  int64_t last = -1;
  for (int64_t k = 0; k < K; ++k) {
    const double v = odf[static_cast<size_t>(k)];
    if (v < thr) continue;
    bool peak = true;
    for (int64_t q = std::max<int64_t>(0, k - near); peak && q <= std::min(K - 1, k + near); ++q)
      if (odf[static_cast<size_t>(q)] > v || (odf[static_cast<size_t>(q)] == v && q < k)) peak = false;
    if (!peak) continue;
    // The exact frame: the sharpest 1 ms jump within 4 ms of the coarse one.
    int64_t best = k * step;
    double bv = -1e9;
    for (int64_t u = std::max<int64_t>(0, k * step - 4 * w1); u <= std::min(n - 1, k * step + 4 * w1); ++u) {
      const double jv = jump(u, w1);
      if (jv > bv) bv = jv, best = u;
    }
    // The first frame of the attack: back to where the signal leaves the level before it.
    const double before = std::sqrt(sum(e1, best - w1, best) / static_cast<double>(w1));
    int64_t on = best;
    while (on > 0 && on > best - w1 && std::fabs(a.L[static_cast<size_t>(r.a + on - 1)]) > 2.0 * before + 1e-9) --on;
    if (last >= 0 && on - last < static_cast<int64_t>(a.rate * 0.03)) continue;
    out.push_back(on);
    last = on;
  }
  return out;
}
}  // namespace

LoopTempo analyseLoop(const Audio& a, Range r, double sensitivity, int beatsPerBar) {
  LoopTempo t;
  r.a = std::clamp<int64_t>(r.a, 0, a.frames());
  r.b = std::clamp<int64_t>(r.b, r.a, a.frames());
  if (r.len() < 2048 || a.rate <= 0) return t;
  t.hits = loopHits(a, r, sensitivity);
  const double sec = static_cast<double>(r.len()) / a.rate;
  const int beats = std::max(1, beatsPerBar);
  const double fromHits = estimateTempo(t.hits, a.rate);
  // A loop is usually a whole number of bars: the bar count whose tempo agrees with the hits
  // (or lies nearest 120 BPM when they say nothing) gives the tempo from the exact length.
  double best = 0, bestErr = 1e9;
  for (double bars : {0.25, 0.5, 1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 32.0}) {
    const double bpm = 60.0 * beats * bars / sec;
    if (bpm < 50 || bpm > 220) continue;
    const double err = std::fabs(std::log(bpm / (fromHits > 0 ? fromHits : 120.0)));
    if (err < bestErr) bestErr = err, best = bars;
  }
  if (best > 0 && (fromHits <= 0 || bestErr < std::log(1.04))) {
    t.bars = best;
    t.bpm = 60.0 * beats * best / sec;
  } else if (fromHits > 0) {
    t.bpm = fromHits;
    t.bars = sec * fromHits / (60.0 * beats);
  }
  return t;
}

int64_t drumStretchLength(const DrumStretchSettings& s, double rate) {
  const double bpm = std::clamp(s.targetBpm, 20.0, 400.0);
  return std::max<int64_t>(1, std::llround(std::max(0.01, s.bars) * std::max(1, s.beatsPerBar) * 60.0 / bpm * rate));
}

Audio drumStretchAudio(const Audio& loop, const std::vector<int64_t>& hitsIn, int64_t newLen, const DrumStretchSettings& s, Progress* p) {
  Audio y = makeAudio(loop.rate, loop.channels, std::max<int64_t>(0, newLen));
  const int64_t n = loop.frames();
  if (n == 0 || newLen <= 0) return y;
  const double f = static_cast<double>(newLen) / static_cast<double>(n);
  std::vector<int64_t> hits;
  for (int64_t h : hitsIn)
    if (h >= 0 && h < n && (hits.empty() || h > hits.back())) hits.push_back(h);
  auto newPos = [&](int64_t h) { return std::llround(static_cast<double>(h) * f); };
  const int nch = loop.channels == 1 ? 1 : 2;
  auto wrapN = [](int64_t i, int64_t m) { return ((i % m) + m) % m; };
  report(p, 0.05);
  if (s.tails == 1) {
    // Slices: each part from one hit to the next plays as it is at its new place; one that has
    // to stop early, or ends before the next hit begins, fades out over 4 ms.
    // Each slice starts 1 ms before its hit (the attack stays whole) and the hit lands exactly
    // on its new place.
    const int64_t pre = std::max<int64_t>(1, static_cast<int64_t>(loop.rate * 0.001));
    std::vector<int64_t> st = hits;
    if (st.empty() || st.front() > 3 * pre) st.insert(st.begin(), 0);
    auto from = [&](size_t j) { return j == 0 && st[0] <= 3 * pre ? int64_t{0} : st[j] - pre; };
    auto to = [&](size_t j) { return j == 0 && st[0] <= 3 * pre ? int64_t{0} : newPos(st[j]) - pre; };
    const int64_t fadeMax = std::max<int64_t>(8, static_cast<int64_t>(loop.rate * 0.004));
    for (size_t j = 0; j < st.size(); ++j) {
      if (stopped(p)) return Audio{};
      const int64_t s0 = from(j), s1 = j + 1 < st.size() ? from(j + 1) : n;
      const int64_t d0 = to(j), d1 = j + 1 < st.size() ? to(j + 1) : newLen;
      const int64_t slot = d1 - d0, len = std::min(s1 - s0, slot);
      if (len <= 0) continue;
      const int64_t fade = len != slot || len != s1 - s0 ? std::min(fadeMax, len / 4) : 0;
      for (int ch = 0; ch < nch; ++ch)
        for (int64_t i = 0; i < len; ++i) {
          const double g = fade > 0 && i >= len - fade ? static_cast<double>(len - 1 - i) / static_cast<double>(fade) : 1.0;
          y.ch(ch)[d0 + i] = static_cast<float>(loop.ch(ch)[s0 + i] * g);
        }
    }
    fixMono(y);
    report(p, 1.0);
    return y;
  }
  // Stretch: the whole loop goes through the stretch as a circle (a piece of its end before it
  // and of its start after it, so the result runs on into itself), then each hit's attack is
  // laid back in from the original, unchanged, at its new place, with 2 ms blends either side.
  const int64_t pad = std::min<int64_t>(n, static_cast<int64_t>(loop.rate * 0.5));
  const int64_t padOut = std::llround(static_cast<double>(pad) * f);
  {
    std::vector<float> in[2], out[2];
    for (int ch = 0; ch < nch; ++ch) {
      in[ch].resize(static_cast<size_t>(n + 2 * pad));
      for (int64_t i = 0; i < n + 2 * pad; ++i) in[ch][static_cast<size_t>(i)] = loop.ch(ch)[wrapN(i - pad, n)];
      out[ch].assign(static_cast<size_t>(newLen + 2 * padOut), 0.f);
    }
    const float* ip[2] = {in[0].data(), nch == 2 ? in[1].data() : in[0].data()};
    float* op[2] = {out[0].data(), nch == 2 ? out[1].data() : out[0].data()};
    stretch::Settings set;
    set.mode = stretch::kStretch;
    stretch::process(ip, nch, n + 2 * pad, op, newLen + 2 * padOut, loop.rate, set);
    for (int ch = 0; ch < nch; ++ch)
      for (int64_t i = 0; i < newLen; ++i) y.ch(ch)[i] = out[ch][static_cast<size_t>(i + padOut)];
  }
  if (stopped(p)) return Audio{};
  report(p, 0.8);
  const int64_t X = std::max<int64_t>(4, static_cast<int64_t>(loop.rate * 0.002));
  const int64_t A = std::max<int64_t>(0, static_cast<int64_t>(loop.rate * std::clamp(s.attackMs, 0.0, 200.0) * 0.001));
  for (size_t i = 0; i < hits.size(); ++i) {
    const int64_t h = hits[i], nh = newPos(h);
    const int64_t nextOld = i + 1 < hits.size() ? hits[i + 1] : hits.front() + n;
    const int64_t nextNew = i + 1 < hits.size() ? newPos(hits[i + 1]) : newPos(hits.front()) + newLen;
    const int64_t room = std::min(nextOld - h, nextNew - nh) - 2 * X - 1;
    if (room < 0) continue;
    const int64_t Ai = std::min(A, room);
    const int64_t total = Ai + 2 * X;
    for (int64_t t = 0; t < total; ++t) {
      double w = 1;
      if (t < X) w = static_cast<double>(t + 1) / static_cast<double>(X + 1);
      else if (t >= X + Ai) w = static_cast<double>(total - t) / static_cast<double>(X + 1);
      const int64_t src = wrapN(h - X + t, n), dst = wrapN(nh - X + t, newLen);
      for (int ch = 0; ch < nch; ++ch) y.ch(ch)[dst] = static_cast<float>(y.ch(ch)[dst] * (1 - w) + loop.ch(ch)[src] * w);
    }
  }
  fixMono(y);
  report(p, 1.0);
  return y;
}

Content drumStretch(const Content& c, Range r, const DrumStretchSettings& s, Progress* p) {
  r = rangeOrAll(c, r);
  if (r.len() < 2048) return c;
  const Audio loop = extract(c.a(), r);
  const LoopTempo lt = analyseLoop(loop, {0, loop.frames()}, s.sensitivity, s.beatsPerBar);
  const int64_t newLen = drumStretchLength(s, loop.rate);
  if (newLen == r.len()) return c;
  Audio y = drumStretchAudio(loop, lt.hits, newLen, s, p);
  if (stopped(p) || y.frames() != newLen) return c;
  Content o = replaceRange(c, r, y);
  const double f = static_cast<double>(newLen) / static_cast<double>(r.len());
  for (const auto& m : c.markers)
    if (m.pos > r.a && m.pos < r.b) o = addMarker(o, r.a + std::llround(static_cast<double>(m.pos - r.a) * f), m.name);
  return o;
}

}  // namespace gl::packs::edison

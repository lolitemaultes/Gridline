// Gridline — "edison" pack: the editor's spectrogram (analyseSpecView). One column per screen
// pixel, one row per pixel on a log-frequency axis, from four analysis windows:
//   k = 0  0.8 s     rows below about 570 Hz (from a copy decimated to under 3 kHz)
//   k = 1  150 ms    rows below about 2.3 kHz (decimated to about 11 kHz)
//   k = 2  37.5 ms   rows above about 280 Hz (full rate)
//   k = 3  9.4 ms    rows above about 1.1 kHz (full rate)
// Each band keeps, per pixel, the lower of two neighbouring resolutions (the long one is sharp
// in frequency, the short one sharp in time; a tone is low in the short one only off its line,
// a hit low in the long one only away from its moment), and the bands cross-fade over an
// octave. Every window resolves a semitone at its band's lowest frequency (100 Hz, 400 Hz,
// 1.6 kHz). Windows are Blackman-Harris (sidelobes under -92 dB) zero-padded to a power of two.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>
#include "edison.h"

namespace gl::packs::edison {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kWin = 4;
constexpr double kWinSec[kWin] = {0.8, 0.15, 0.0375, 0.009375};
constexpr double kEdgeLo = 400.0, kEdgeHi = 1600.0;  // band edges (centres of the cross-fades)

// Radix-2 complex FFT on split float arrays.
struct Plan {
  int n = 0;
  std::vector<float> cs, sn;
  std::vector<int> rev;
  explicit Plan(int n_) : n(n_), cs(static_cast<size_t>(n_ / 2)), sn(static_cast<size_t>(n_ / 2)), rev(static_cast<size_t>(n_)) {
    for (int i = 0; i < n / 2; ++i) {
      cs[static_cast<size_t>(i)] = static_cast<float>(std::cos(2 * kPi * i / n));
      sn[static_cast<size_t>(i)] = static_cast<float>(-std::sin(2 * kPi * i / n));
    }
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b)
        if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      rev[static_cast<size_t>(i)] = r;
    }
  }
  void forward(float* re, float* im) const {
    for (int i = 0; i < n; ++i) {
      const int r = rev[static_cast<size_t>(i)];
      if (i < r) std::swap(re[i], re[r]), std::swap(im[i], im[r]);
    }
    for (int len = 2; len <= n; len <<= 1) {
      const int h = len / 2, step = n / len;
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < h; ++j) {
          const float wr = cs[static_cast<size_t>(j * step)], wi = sn[static_cast<size_t>(j * step)];
          const int a = i + j, b = a + h;
          const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
          re[b] = re[a] - tr, im[b] = im[a] - ti;
          re[a] += tr, im[a] += ti;
        }
    }
  }
};

// Half-band low-pass (31 taps, Kaiser beta 10): flat to 0.125 fs, below -100 dB from 0.375 fs.
struct HalfBand {
  static constexpr int kHalf = 15;
  float h[kHalf + 1] = {};  // h[0] centre, h[odd] the only other non-zero taps
  HalfBand() {
    auto i0 = [](double x) {
      double s = 1, t = 1;
      for (int k = 1; k < 40; ++k) t *= (x / (2 * k)) * (x / (2 * k)), s += t;
      return s;
    };
    const double beta = 10.0;
    double sum = 0;
    double v[kHalf + 1] = {};
    for (int n = 1; n <= kHalf; n += 2) {
      const double r = static_cast<double>(n) / (kHalf + 1);
      v[n] = std::sin(kPi * n / 2) / (kPi * n) * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
      sum += 2 * v[n];
    }
    h[0] = 0.5f;
    for (int n = 1; n <= kHalf; n += 2) h[n] = static_cast<float>(v[n] * 0.5 / sum);
  }
};
const HalfBand& halfBand() {
  static const HalfBand hb;
  return hb;
}

// Audio as one analysis window sees it: frame base + j * D is index j.
struct Src {
  const float* L = nullptr;
  const float* R = nullptr;
  int64_t base = 0, len = 0;
  int D = 1;
};

// Decimates frames [s0, s1) of the audio by D (a power of two) into l / r; index j is frame
// s0 + j * D. Zero outside the audio. The edges (about 16 D frames) are not exact: callers
// ask for that much more than they read.
void decimate(const Audio& a, int64_t s0, int64_t s1, int D, std::vector<float>& l, std::vector<float>& r, std::vector<float>& tl, std::vector<float>& tr) {
  const int64_t n = std::max<int64_t>(0, s1 - s0);
  const bool st = a.channels > 1;
  l.assign(static_cast<size_t>(n), 0.f);
  r.assign(st ? static_cast<size_t>(n) : 0, 0.f);
  const int64_t lo = std::max<int64_t>(0, s0), hi = std::min<int64_t>(a.frames(), s1);
  for (int64_t i = lo; i < hi; ++i) {
    l[static_cast<size_t>(i - s0)] = a.L[static_cast<size_t>(i)];
    if (st) r[static_cast<size_t>(i - s0)] = a.R[static_cast<size_t>(i)];
  }
  const HalfBand& hb = halfBand();
  for (int d = 1; d < D; d *= 2) {
    for (int c = 0; c < (st ? 2 : 1); ++c) {
      std::vector<float>& x = c == 0 ? l : r;
      std::vector<float>& y = c == 0 ? tl : tr;
      const int64_t m = static_cast<int64_t>(x.size());
      const int64_t out = (m + 1) / 2;
      y.assign(static_cast<size_t>(out), 0.f);
      for (int64_t j = 0; j < out; ++j) {
        const int64_t p = 2 * j;
        float acc = hb.h[0] * x[static_cast<size_t>(p)];
        if (p >= HalfBand::kHalf && p + HalfBand::kHalf < m) {
          for (int k = 1; k <= HalfBand::kHalf; k += 2) acc += hb.h[k] * (x[static_cast<size_t>(p - k)] + x[static_cast<size_t>(p + k)]);
        } else {
          for (int k = 1; k <= HalfBand::kHalf; k += 2) {
            const float xa = p - k >= 0 ? x[static_cast<size_t>(p - k)] : 0.f, xb = p + k < m ? x[static_cast<size_t>(p + k)] : 0.f;
            acc += hb.h[k] * (xa + xb);
          }
        }
        y[static_cast<size_t>(j)] = acc;
      }
      x.swap(y);
    }
  }
}

struct Win {
  int D = 1, L = 0, N = 0;      // decimation, window length and FFT size (decimated samples)
  double rate = 0, df = 0;      // its sample rate, bin spacing (Hz)
  double frames = 0;            // window length in full-rate frames
  bool used = false, dense = false;
  int nsub = 1;
  std::vector<float> w;         // the window
  float scale = 0;              // power to amplitude^2 for a sine (2 / sum(w))^2
  const Plan* plan = nullptr;
  // Per row: the bin the row centre falls on, and the bins inside the row.
  std::vector<float> pc;
  std::vector<int> b0, b1;
};

float interpRow(const float* db, int nb, float pc, int b0, int b1) {
  const int i = std::clamp(static_cast<int>(std::floor(pc + 0.5f)), 1, nb - 2);
  const float d = std::clamp(pc - static_cast<float>(i), -0.5f, 0.5f);
  const float ym = db[i - 1], y0 = db[i], yp = db[i + 1];
  float v = y0 + d * (yp - ym) * 0.5f + d * d * (yp - 2 * y0 + ym) * 0.5f;
  const float lo = std::min(y0, std::min(ym, yp)), hi = std::max(y0, std::max(ym, yp)) + 1.5f;
  v = std::clamp(v, lo, hi);
  for (int b = std::max(0, b0); b <= std::min(nb - 1, b1); ++b) v = std::max(v, db[b]);
  return v;
}

float smooth01(double x) {  // 0 below -0.5, 1 above 0.5 (octaves)
  const double t = std::clamp(x + 0.5, 0.0, 1.0);
  return static_cast<float>(t * t * (3 - 2 * t));
}

}  // namespace

SpecView analyseSpecView(const Audio& a, const SpecViewRequest& rq, const std::atomic<bool>* cancel, int threads) {
  SpecView out;
  out.req = rq;
  out.rate = a.rate;
  if (rq.cols <= 0 || rq.rows <= 1 || rq.to <= rq.from || a.rate <= 0 || a.frames() == 0) return out;
  const double rate = a.rate, ny = rate * 0.5;
  const double minHz = std::clamp(rq.minHz, 1.0, ny * 0.5), maxHz = rq.maxHz > minHz ? std::min(rq.maxHz, ny) : ny;
  const int rows = rq.rows, cols = rq.cols;
  const double hop = static_cast<double>(rq.to - rq.from) / cols;
  const double resScale = rq.resolution <= 0 ? 0.5 : rq.resolution >= 2 ? 2.0 : 1.0;

  // Row frequencies (centre and edges).
  std::vector<double> fc(static_cast<size_t>(rows)), fa(static_cast<size_t>(rows)), fb(static_cast<size_t>(rows));
  const double span = std::log(maxHz / minHz);
  for (int r = 0; r < rows; ++r) {
    auto f = [&](double t) { return minHz * std::exp(span * t); };
    const double t = 1.0 - (r + 0.5) / rows;
    fc[static_cast<size_t>(r)] = f(t);
    fa[static_cast<size_t>(r)] = f(t - 0.5 / rows);
    fb[static_cast<size_t>(r)] = f(t + 0.5 / rows);
  }
  // Band weights per row: A = min(k0, k1), B = min(k1, k2), C = min(k2, k3).
  std::vector<float> wA(static_cast<size_t>(rows)), wC(static_cast<size_t>(rows));
  for (int r = 0; r < rows; ++r) {
    const float up1 = smooth01(std::log2(fc[static_cast<size_t>(r)] / kEdgeLo)), up2 = smooth01(std::log2(fc[static_cast<size_t>(r)] / kEdgeHi));
    wA[static_cast<size_t>(r)] = 1 - up1;
    wC[static_cast<size_t>(r)] = up1 * up2;
  }
  const double topA = kEdgeLo * std::sqrt(2.0), topB = kEdgeHi * std::sqrt(2.0), botB = kEdgeLo / std::sqrt(2.0), botC = kEdgeHi / std::sqrt(2.0);

  Win W[kWin];
  std::vector<std::unique_ptr<Plan>> plans;
  for (int k = 0; k < kWin; ++k) {
    Win& w = W[k];
    const double top = k == 0 ? topA : k == 1 ? topB : ny;  // highest frequency the window serves
    const double bot = k <= 1 ? 0.0 : k == 2 ? botB : botC;
    w.used = top >= minHz && bot <= maxHz;
    // The largest decimation whose half-band stages stay flat to `top`.
    w.D = 1;
    while (k <= 1 && rate / (w.D * 2) >= 4 * top && w.D < 64) w.D *= 2;
    w.rate = rate / w.D;
    w.L = std::max(16, static_cast<int>(std::lround(kWinSec[k] * resScale * w.rate)));
    w.N = 16;
    while (w.N < w.L) w.N *= 2;
    w.df = w.rate / w.N;
    w.frames = static_cast<double>(w.L) * w.D;
    w.w.resize(static_cast<size_t>(w.L));
    double sum = 0;
    for (int i = 0; i < w.L; ++i) {
      const double x = (i + 0.5) / w.L;
      const double v = 0.35875 - 0.48829 * std::cos(2 * kPi * x) + 0.14128 * std::cos(4 * kPi * x) - 0.01168 * std::cos(6 * kPi * x);
      w.w[static_cast<size_t>(i)] = static_cast<float>(v);
      sum += v;
    }
    w.scale = static_cast<float>((2.0 / sum) * (2.0 / sum));
    Plan* p = nullptr;
    for (auto& q : plans)
      if (q->n == w.N) p = q.get();
    if (!p) plans.push_back(std::make_unique<Plan>(w.N)), p = plans.back().get();
    w.plan = p;
    w.pc.resize(static_cast<size_t>(rows));
    w.b0.resize(static_cast<size_t>(rows));
    w.b1.resize(static_cast<size_t>(rows));
    for (int r = 0; r < rows; ++r) {
      w.pc[static_cast<size_t>(r)] = static_cast<float>(fc[static_cast<size_t>(r)] / w.df);
      w.b0[static_cast<size_t>(r)] = static_cast<int>(std::ceil(fa[static_cast<size_t>(r)] / w.df));
      w.b1[static_cast<size_t>(r)] = static_cast<int>(std::floor(fb[static_cast<size_t>(r)] / w.df));
    }
    // Columns further apart than half a window: up to 8 windows per column (loudest kept).
    w.nsub = std::clamp(static_cast<int>(std::ceil(hop / (w.frames * 0.5))), 1, 8);
    w.dense = w.D == 1 || hop < w.frames;
  }
  // Work budget (about 1.5 G operations): fewer windows per column on very long views.
  {
    auto cost = [&]() {
      double c = 0;
      for (const Win& w : W)
        if (w.used) c += w.nsub * (w.N * std::log2(static_cast<double>(w.N)) * 5.0 + (w.dense ? 0.0 : w.frames * 32.0));
      return c * cols;
    };
    while (cost() > 1.5e9) {
      bool any = false;
      for (Win& w : W)
        if (w.nsub > 1) w.nsub = std::max(1, w.nsub / 2), any = true;
      if (!any) break;
    }
  }

  // Decimated copies for the dense low windows (shared by every column).
  std::vector<float> dl[kWin], dr[kWin];
  Src src[kWin];
  for (int k = 0; k < kWin; ++k) {
    Win& w = W[k];
    src[k].D = w.D;
    if (!w.used) continue;
    if (w.D == 1) {
      src[k].L = a.L.data(), src[k].R = a.channels > 1 ? a.R.data() : nullptr, src[k].base = 0, src[k].len = a.frames();
    } else if (w.dense) {
      const int64_t pad = static_cast<int64_t>(w.frames) + 32LL * w.D;
      const int64_t s0 = (rq.from - pad) / w.D * w.D, s1 = rq.to + pad;
      std::vector<float> tl, tr;
      decimate(a, s0, s1, w.D, dl[k], dr[k], tl, tr);
      src[k].L = dl[k].data(), src[k].R = a.channels > 1 ? dr[k].data() : nullptr, src[k].base = s0, src[k].len = static_cast<int64_t>(dl[k].size());
    }
  }
  if (cancel && cancel->load()) return out;

  out.db.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), -200.f);
  std::atomic<int> next{0};
  std::atomic<bool> stop{false};
  const bool stereo = a.channels > 1;
  auto worker = [&]() {
    int maxN = 16;
    for (const Win& w : W) maxN = std::max(maxN, w.N);
    std::vector<float> re(static_cast<size_t>(maxN)), im(static_cast<size_t>(maxN));
    std::vector<float> pw[kWin], db[kWin];
    std::vector<float> ll, lr, tl, tr;
    std::vector<float> rowv[kWin], rowg[kWin];  // per row: each window's own value, and its value as the gate
    for (int k = 0; k < kWin; ++k) {
      pw[k].resize(static_cast<size_t>(W[k].N / 2 + 1));
      db[k].resize(static_cast<size_t>(W[k].N / 2 + 1));
      rowv[k].resize(static_cast<size_t>(rows));
      rowg[k].resize(static_cast<size_t>(rows));
    }
    constexpr int kChunk = 8;
    while (!stop.load(std::memory_order_relaxed)) {
      const int c0 = next.fetch_add(kChunk);
      if (c0 >= cols) break;
      for (int c = c0; c < std::min(cols, c0 + kChunk); ++c) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
          stop = true;
          break;
        }
        for (int k = 0; k < kWin; ++k) {
          const Win& w = W[k];
          if (!w.used) continue;
          const int nb = w.N / 2 + 1;
          std::fill(pw[k].begin(), pw[k].end(), 0.f);
          for (int j = 0; j < w.nsub; ++j) {
            const double centre = static_cast<double>(rq.from) + (c + (j + 0.5) / w.nsub) * hop;
            Src s = src[k];
            if (!w.dense) {  // a decimated copy of this window's stretch only
              const int64_t pad = 32LL * w.D;
              const int64_t s0 = (static_cast<int64_t>(std::floor(centre - w.frames * 0.5)) - pad) / w.D * w.D;
              decimate(a, s0, static_cast<int64_t>(std::ceil(centre + w.frames * 0.5)) + pad + w.D, w.D, ll, lr, tl, tr);
              s.L = ll.data(), s.R = stereo ? lr.data() : nullptr, s.base = s0, s.len = static_cast<int64_t>(ll.size());
            }
            const int64_t start = static_cast<int64_t>(std::llround((centre - static_cast<double>(s.base)) / w.D - w.L * 0.5));
            for (int i = 0; i < w.L; ++i) {
              const int64_t p = start + i;
              const bool in = p >= 0 && p < s.len;
              const float g = w.w[static_cast<size_t>(i)];
              re[static_cast<size_t>(i)] = in ? s.L[p] * g : 0.f;
              im[static_cast<size_t>(i)] = in && s.R ? s.R[p] * g : 0.f;
            }
            std::fill(re.begin() + w.L, re.begin() + w.N, 0.f);
            std::fill(im.begin() + w.L, im.begin() + w.N, 0.f);
            w.plan->forward(re.data(), im.data());
            float* P = pw[k].data();
            if (s.R) {  // L + iR in one transform: |XL|^2 + |XR|^2 = (|Z(b)|^2 + |Z(N-b)|^2) / 2
              for (int b = 0; b < nb; ++b) {
                const int m = b == 0 ? 0 : w.N - b;
                const float v = (re[static_cast<size_t>(b)] * re[static_cast<size_t>(b)] + im[static_cast<size_t>(b)] * im[static_cast<size_t>(b)] +
                                 re[static_cast<size_t>(m)] * re[static_cast<size_t>(m)] + im[static_cast<size_t>(m)] * im[static_cast<size_t>(m)]) *
                                0.25f;
                P[b] = std::max(P[b], v);
              }
            } else {
              for (int b = 0; b < nb; ++b) P[b] = std::max(P[b], re[static_cast<size_t>(b)] * re[static_cast<size_t>(b)] + im[static_cast<size_t>(b)] * im[static_cast<size_t>(b)]);
            }
          }
          for (int b = 0; b < nb; ++b) db[k][static_cast<size_t>(b)] = 10.f * std::log10(std::max(1e-20f, pw[k][static_cast<size_t>(b)] * w.scale));
        }
        for (int r = 0; r < rows; ++r) {
          const double f = fc[static_cast<size_t>(r)];
          for (int k = 0; k < kWin; ++k) {
            const Win& w = W[k];
            const bool need = w.used && (k == 0 ? f < topA : k == 1 ? f < topB : k == 2 ? f > botB : f > botC) && (w.D == 1 || f < w.rate * 0.45);
            const size_t ri = static_cast<size_t>(r);
            rowv[k][ri] = need ? interpRow(db[k].data(), w.N / 2 + 1, w.pc[ri], w.b0[ri], w.b1[ri]) : 0.f;
            // As the gate (the shorter window of a pair) it only has to say whether anything is
            // there at this moment: the loudest bin within 3 of the row, so the random dips of
            // noise in it do not punch holes into the longer window's picture.
            if (k > 0)
              rowg[k][ri] = need ? interpRow(db[k].data(), w.N / 2 + 1, w.pc[ri], std::min(w.b0[ri], static_cast<int>(w.pc[ri]) - 3),
                                             std::max(w.b1[ri], static_cast<int>(w.pc[ri]) + 4))
                                 : 0.f;
          }
          const float a0 = wA[static_cast<size_t>(r)], c0w = wC[static_cast<size_t>(r)], b0w = 1 - a0 - c0w;
          float v = 0;
          if (a0 > 0) v += a0 * std::min(rowv[0][static_cast<size_t>(r)], rowg[1][static_cast<size_t>(r)]);
          if (b0w > 0) v += b0w * std::min(rowv[1][static_cast<size_t>(r)], rowg[2][static_cast<size_t>(r)]);
          if (c0w > 0) v += c0w * std::min(rowv[2][static_cast<size_t>(r)], rowg[3][static_cast<size_t>(r)]);
          out.db[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)] = std::max(-200.f, v);
        }
      }
    }
  };
  int T = threads > 0 ? threads : static_cast<int>(std::thread::hardware_concurrency() / 2);
  T = std::clamp(T, 1, 4);  // leaves cores for the audio and the interface
  T = std::min(T, std::max(1, cols / 16));
  std::vector<std::thread> pool;
  for (int t = 1; t < T; ++t) pool.emplace_back(worker);
  worker();
  for (auto& t : pool) t.join();
  if (stop.load()) out.db.clear();
  return out;
}

}  // namespace gl::packs::edison

// Gridline — "spacefx" pack, J-42: Band Delay reference model (BdModel = 1).
// The reference multiband delay: 16 fixed bands from one of three filter banks
// (gentle band passes, steep 8th-order Linkwitz–Riley pairs, or a linear-phase
// FIR bank), each band through its own time-domain delay line with feedback
// (smoothed interpolated delay, or 100 ms tap cross-fades in keep-pitch mode),
// then a tapered volume and constant-power pan from 8 morphable banks; the
// wet sum is high-passed, saturated (limiter / tanh / clip) and cross-faded with
// the dry signal. Own code after the J-42 study notes. Memory is sized for the
// sample rate by prepare() (called from the UI thread when possible).
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gl::packs::spacefx::mbd {

inline constexpr int kBands = 16, kBanks = 8, kVals = kBanks * 3 * kBands;
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kEdge[kBands] = {31, 47, 71, 112, 172, 264, 413, 624, 958, 1502, 2267, 3611, 5458, 8391, 12663, 18994};

// Engine values (0..1 unless noted), as the reference's parameter block.
struct Settings {
  float bank[kVals];  // [bank][target 0 delay, 1 volume, 2 pan][band]
  float scale = 0.5f, tenth = 0, feedback = 0, smooth = 0.25f, mode = 0, keep = 0, morph = 0, mix = 1, harmonic = 0, weight = 0, hp = 0, sat = 0;
};

struct Bq {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float x1[2] = {0, 0}, x2[2] = {0, 0}, y1[2] = {0, 0}, y2[2] = {0, 0};
  float run(int c, float x) {
    const float y = b0 * x + b1 * x1[c] + b2 * x2[c] - a1 * y1[c] - a2 * y2[c];
    x2[c] = x1[c], x1[c] = x, y2[c] = y1[c], y1[c] = y;
    return y;
  }
  // type 0 pass, 1 LP, 2 HP, 6 constant-skirt BP, 9 all-pass, 10 first-order LP, 11 first-order HP.
  void design(int type, double f, double Q, double g, double sr) {
    double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
    const double w = 2 * kPi * f / sr, cs = std::cos(w), al = std::sin(w) / (2 * Q);
    switch (type) {
      case 1: B0 = (1 - cs) / 2, B1 = 1 - cs, B2 = (1 - cs) / 2, A0 = 1 + al, A1 = -2 * cs, A2 = 1 - al; break;
      case 2: B0 = (1 + cs) / 2, B1 = -(1 + cs), B2 = (1 + cs) / 2, A0 = 1 + al, A1 = -2 * cs, A2 = 1 - al; break;
      case 6: B0 = Q * al, B1 = 0, B2 = -Q * al, A0 = 1 + al, A1 = -2 * cs, A2 = 1 - al; break;
      case 9: B0 = 1 - al, B1 = -2 * cs, B2 = 1 + al, A0 = 1 + al, A1 = -2 * cs, A2 = 1 - al; break;
      case 10:
      case 11: {
        const double t = std::tan(kPi * f / sr);
        if (type == 10) B0 = t, B1 = t;
        else B0 = 1, B1 = -1;
        A0 = t + 1, A1 = t - 1;
        break;
      }
      default: break;
    }
    b0 = static_cast<float>(g * B0 / A0), b1 = static_cast<float>(g * B1 / A0), b2 = static_cast<float>(g * B2 / A0);
    a1 = static_cast<float>(A1 / A0), a2 = static_cast<float>(A2 / A0);
  }
};

// Radix-2 complex FFT (double).
struct Fft {
  int n = 0;
  std::vector<std::complex<double>> w;
  std::vector<int> rev;
  void init(int size) {
    n = size;
    w.resize(static_cast<size_t>(n / 2));
    for (int k = 0; k < n / 2; ++k) w[static_cast<size_t>(k)] = std::polar(1.0, -2 * kPi * k / n);
    rev.assign(static_cast<size_t>(n), 0);
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b)
        if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      rev[static_cast<size_t>(i)] = r;
    }
  }
  void run(std::complex<double>* a, bool inv) const {
    for (int i = 0; i < n; ++i)
      if (i < rev[static_cast<size_t>(i)]) std::swap(a[i], a[rev[static_cast<size_t>(i)]]);
    for (int len = 2; len <= n; len <<= 1)
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < len / 2; ++j) {
          std::complex<double> t = w[static_cast<size_t>(j * (n / len))];
          if (inv) t = std::conj(t);
          const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * t;
          a[i + j] = u + v, a[i + j + len / 2] = u - v;
        }
  }
};

class Engine {
 public:
  bool ready() const { return sr_ > 0; }
  int latency(const Settings& s) const { return s.mode >= 0.666666f ? 1536 : 0; }
  // Sizes every buffer for the rate (allocation: call from the UI thread when possible).
  void prepare(double sr) {
    sr_ = sr;
    int len = 1;
    while (len < static_cast<int>(std::lround(sr))) len <<= 1;
    mask_ = len - 1;
    for (auto& b : line_)
      for (auto& c : b) c.assign(static_cast<size_t>(len), 0.f);
    fft_.init(2048);
    for (auto& k : kern_) k.assign(2048, {0, 0});
    for (auto& b : firIn_) b.assign(1024 * 2, 0.f);
    for (auto& b : firOut_) b.assign(2048 * 2, 0.f);
    work_.assign(2048, {0, 0});
    work2_.assign(2048, {0, 0});
    dry_[0].assign(1546, 0.f), dry_[1].assign(1546, 0.f);
    design();
    reset();
  }
  void reset() {
    for (auto& b : line_)
      for (auto& c : b) std::fill(c.begin(), c.end(), 0.f);
    for (auto& b : iir_)
      for (auto& q : b) std::fill(q.x1, q.x1 + 2, 0.f), std::fill(q.x2, q.x2 + 2, 0.f), std::fill(q.y1, q.y1 + 2, 0.f), std::fill(q.y2, q.y2 + 2, 0.f);
    for (auto& b : firIn_) std::fill(b.begin(), b.end(), 0.f);
    for (auto& b : firOut_) std::fill(b.begin(), b.end(), 0.f);
    for (auto& d : dry_) std::fill(d.begin(), d.end(), 0.f);
    w_ = firPos_ = dryW_ = 0;
    for (int u = 0; u < kBands; ++u) cur_[u] = -1, gL_[u] = gR_[u] = -1, tap_[0][u] = tap_[1][u] = -1;
    q_ = 1, fading_ = false, hpS_ = -1, mixS_ = -1;
    hpX_[0] = hpX_[1] = hpY_[0] = hpY_[1] = 0, env_[0] = env_[1] = 0;
    primed_ = false;
  }
  void process(const Settings& s, float* L, float* R, int n) {
    if (sr_ <= 0) return;
    if (!primed_ || std::memcmp(&s, &last_, sizeof s) != 0) update(s), last_ = s;
    const bool linear = s.mode >= 0.666666f, gentle = s.mode < 0.333333f, keep = s.keep > 0.5f;
    const double kSmooth = s.smooth <= 0 ? 1.0 : 1.0 - std::pow(0.1, 1.0 / (s.smooth * sr_));
    const float kGain = static_cast<float>(1.0 - std::pow(0.1, 1.0 / (0.05 * sr_)));
    const float k5 = static_cast<float>(1.0 - std::pow(0.1, 1.0 / (0.005 * sr_)));
    const float cA = static_cast<float>(1.0 - std::pow(0.1, 1.0 / (0.01 * sr_))), cR = static_cast<float>(1.0 - std::pow(0.1, 1.0 / (0.1 * sr_)));
    const int sat = s.sat < 0.333f ? 0 : s.sat < 0.666f ? 1 : 2;
    for (int i = 0; i < n; ++i) {
      const float in[2] = {L[i], R[i]};
      float bandX[kBands][2];
      if (linear) firBands(in, bandX);
      else
        for (int u = 0; u < kBands; ++u)
          for (int c = 0; c < 2; ++c) {
            float x = in[c];
            for (int k = 0; k < nSec_[u]; ++k) x = iir_[u][k].run(c, x);
            bandX[u][c] = x;
          }
      (void)gentle;
      float wet[2] = {0, 0};
      for (int u = 0; u < kBands; ++u) {
        float y[2];
        if (!keep) {
          if (cur_[u] < 0) cur_[u] = D_[u];
          cur_[u] += kSmooth * (D_[u] - cur_[u]);
          const int ci = static_cast<int>(cur_[u]);
          const float ph = static_cast<float>(cur_[u] - ci);
          for (int c = 0; c < 2; ++c) {
            const std::vector<float>& b = line_[u][c];
            y[c] = (1 - ph) * b[static_cast<size_t>((w_ - ci) & mask_)] + ph * b[static_cast<size_t>((w_ - ci - 1) & mask_)];
          }
        } else {
          if (tap_[0][u] < 0) tap_[0][u] = tap_[1][u] = D_[u];
          for (int c = 0; c < 2; ++c) {
            const std::vector<float>& b = line_[u][c];
            y[c] = static_cast<float>(q_) * b[static_cast<size_t>((w_ - tap_[0][u]) & mask_)] +
                   static_cast<float>(1 - q_) * b[static_cast<size_t>((w_ - tap_[1][u]) & mask_)];
          }
        }
        for (int c = 0; c < 2; ++c) line_[u][c][static_cast<size_t>(w_)] = bandX[u][c] + std::clamp(a_[u] * y[c], -1.f, 1.f);
        if (gL_[u] < 0) gL_[u] = tgL_[u], gR_[u] = tgR_[u];
        wet[0] += y[0] * gL_[u], wet[1] += y[1] * gR_[u];
        gL_[u] += (tgL_[u] - gL_[u]) * kGain, gR_[u] += (tgR_[u] - gR_[u]) * kGain;
      }
      w_ = (w_ + 1) & mask_;
      // Keep-pitch tap cross-fade.
      if (keep) {
        if (!fading_ && pendingTaps_) {
          const int idle = q_ >= 0.5 ? 1 : 0;
          for (int u = 0; u < kBands; ++u) tap_[idle][u] = D_[u];
          fadeTo_ = idle == 1 ? 0.0 : 1.0;
          fading_ = true, pendingTaps_ = false;
        }
        if (fading_) {
          const double st = 1.0 / (0.1 * sr_);
          q_ = fadeTo_ > q_ ? std::min(1.0, q_ + st) : std::max(0.0, q_ - st);
          if (q_ == fadeTo_) fading_ = false;
        }
      }
      // High pass, saturation, dry/wet.
      hpS_ = hpS_ < 0 ? s.hp : hpS_ + (s.hp - hpS_) * k5;
      const double fc = std::min(10.0 * std::pow(10.0, 2.0 * hpS_), 0.49 * sr_);
      const float t = static_cast<float>(std::tan(kPi * fc / sr_));
      mixS_ = mixS_ < 0 ? s.mix : mixS_ + (s.mix - mixS_) * k5;
      float out[2];
      for (int c = 0; c < 2; ++c) {
        float x = wet[c];
        const float yh = (x - hpX_[c]) / (1 + t) + hpY_[c] * (1 - t) / (1 + t);
        hpX_[c] = x, hpY_[c] = yh;
        x = yh;
        if (sat == 0) {
          const float ax = std::fabs(x);
          env_[c] += (ax - env_[c]) * (ax >= env_[c] ? cA : cR);
          x = x / std::max(env_[c], 1.f);
        } else if (sat == 1) {
          x = std::tanh(x);
        } else {
          x = std::clamp(x, -1.f, 1.f);
        }
        float d = in[c];
        if (linear) {
          std::vector<float>& db = dry_[c];
          const int at = (dryW_ - 1536 + 1546) % 1546;
          db[static_cast<size_t>(dryW_)] = in[c];
          d = db[static_cast<size_t>(at)];
        }
        out[c] = d * (1 - mixS_) + x * mixS_;
      }
      if (linear) dryW_ = (dryW_ + 1) % 1546;
      L[i] = out[0], R[i] = out[1];
    }
  }

 private:
  void design() {
    // GENTLE (one or two sections) and STEEP (eight sections) banks; both kept, chosen per mode.
    static const double kQ[4] = {0.5411959887, 1.3065630198, 0.5411959887, 1.3065630198};
    for (int m = 0; m < 2; ++m)
      for (int u = 0; u < kBands; ++u) {
        Bq* q = bank_[m][u];
        for (int k = 0; k < 8; ++k) q[k] = Bq{};
        int nsec = 0;
        if (m == 0) {
          if (u == 0) q[0].design(10, 31, 0.5, 1.0, sr_), nsec = 1;
          else if (u < 15) q[0].design(6, kEdge[u], 0.5, 0.5, sr_), nsec = 1;
          else q[0].design(11, 18994, 0.5, 0.5, sr_), q[1].design(9, 18994, 0.5, 2.0, sr_), nsec = 2;
        } else {
          for (int k = 0; k < 4; ++k) {
            const double g = k == 0 ? 1.4142f : 1.0;
            if (u == 0) q[k].design(1, 31, kQ[k], g, sr_), q[4 + k].design(0, 1, 1, 1, sr_);
            else if (u < 15) q[k].design(2, kEdge[u - 1], kQ[k], g, sr_), q[4 + k].design(1, kEdge[u], kQ[k], 1, sr_);
            else q[k].design(2, 12663, kQ[k], g, sr_), q[4 + k].design(9, 18994, kQ[k], 1, sr_);
          }
          nsec = 8;
        }
        bankN_[m][u] = nsec;
      }
    // Linear-phase kernels: frequency-sampled band masks, 1024 taps (sine window).
    const double d = sr_ / 2048.0;
    std::vector<std::complex<double>> a(2048);
    for (int u = 0; u < kBands; ++u) {
      double lo = u == 0 ? 0 : kEdge[u - 1], hi = u == 0 ? 31 : u == 15 ? sr_ / 2 : kEdge[u];
      if (u == 15) lo = 12663;
      const int k0 = static_cast<int>(lo / d), k1 = static_cast<int>(hi / d);
      std::fill(a.begin(), a.end(), std::complex<double>(0, 0));
      a[1024] = 1;
      fft_.run(a.data(), false);
      for (int k = 0; k <= 1024; ++k) {
        double m = (k >= k0 && k < k1) ? 1 : 0;
        if (u == 15 && k == 1024) m = 1;
        a[static_cast<size_t>(k)] *= m;
        if (k > 0 && k < 1024) a[static_cast<size_t>(2048 - k)] *= m;
      }
      fft_.run(a.data(), true);
      std::vector<std::complex<double>>& K = kern_[u];
      std::fill(K.begin(), K.end(), std::complex<double>(0, 0));
      for (int j = 0; j < 1024; ++j) K[static_cast<size_t>(j)] = a[static_cast<size_t>(512 + j)].real() / 2048.0 * std::sin(kPi * j / 1023.0);
      fft_.run(K.data(), false);
    }
  }
  // Overlap-add FIR bank: 1024-sample blocks, output read during the next block.
  void firBands(const float* in, float (*out)[2]) {
    for (int c = 0; c < 2; ++c) firIn_[c][static_cast<size_t>(firPos_)] = in[c];
    for (int u = 0; u < kBands; ++u)
      for (int c = 0; c < 2; ++c) out[u][c] = firAcc_[u][c][firPos_];
    if (++firPos_ < 1024) return;
    firPos_ = 0;
    for (int i = 0; i < 2048; ++i)
      work_[static_cast<size_t>(i)] = i < 1024 ? std::complex<double>(firIn_[0][static_cast<size_t>(i)], firIn_[1][static_cast<size_t>(i)]) : 0.0;
    fft_.run(work_.data(), false);
    std::vector<std::complex<double>>& y = work2_;
    for (int u = 0; u < kBands; ++u) {
      for (int i = 0; i < 2048; ++i) y[static_cast<size_t>(i)] = work_[static_cast<size_t>(i)] * kern_[u][static_cast<size_t>(i)];
      fft_.run(y.data(), true);
      for (int c = 0; c < 2; ++c) {
        float* acc = firAcc_[u][c];
        for (int i = 0; i < 1024; ++i) acc[i] = acc[i + 1024], acc[i + 1024] = 0;
        for (int i = 0; i < 2048; ++i) acc[i] += static_cast<float>((c == 0 ? y[static_cast<size_t>(i)].real() : y[static_cast<size_t>(i)].imag()) / 2048.0);
      }
    }
  }
  void update(const Settings& s) {
    const int m = s.mode < 0.333333f ? 0 : 1;
    for (int u = 0; u < kBands; ++u) {
      nSec_[u] = bankN_[m][u];
      for (int k = 0; k < 8; ++k) {
        Bq& dst = iir_[u][k];
        const Bq& src = bank_[m][u][k];
        dst.b0 = src.b0, dst.b1 = src.b1, dst.b2 = src.b2, dst.a1 = src.a1, dst.a2 = src.a2;
      }
    }
    const double mm = 7.0 * s.morph;
    const int bi = std::clamp(static_cast<int>(mm), 0, 7);
    const double f = mm - bi;
    auto B = [&](int t, int u) {
      const float a = s.bank[(bi * 3 + t) * kBands + u], b = bi + 1 < kBanks ? s.bank[((bi + 1) * 3 + t) * kBands + u] : a;
      return (1 - f) * a + f * b;
    };
    for (int u = 0; u < kBands; ++u) {
      const double pan = std::clamp(B(2, u), 0.0, 1.0), v = B(1, u);
      const double g = 0.98435 * v * v * v - 0.231573 * v * v + 0.247223 * v;
      tgL_[u] = static_cast<float>(g * std::sqrt(1 - pan) * 1.4144272);
      tgR_[u] = static_cast<float>(g * std::sqrt(pan) * 1.4144272);
      double sec;
      if (s.harmonic >= 0.5f) {
        const int k = static_cast<int>((2.0 * s.scale - 1.0) * 48.0);
        const double r = std::pow(2.0, 4.0 * (B(0, u) - 0.5));
        sec = std::min(1.0, 1.0 / (523.2512 * std::pow(2.0, k / 12.0) * r));
      } else {
        const double dd = B(0, u), w = s.scale < 0.5f ? 1 - dd : dd;
        sec = w * std::fabs(2.0 * s.scale - 1.0) * (s.tenth >= 0.5f ? 0.1 : 1.0);
      }
      int D = static_cast<int>(sec * sr_);
      if (D < 2) D = 1;
      D_[u] = std::min(D, mask_ - 1);
      if (s.weight >= 0.1f) {
        static const double TAB[7] = {0, 0.02, 0.1, 0.5, 2.5, 10, 10};
        const double x = 5 * 0.95 * s.feedback;
        const int xi = std::clamp(static_cast<int>(x), 0, 5);
        const double T = TAB[xi] + (TAB[xi + 1] - TAB[xi]) * (x - xi);
        const int Dp = std::max(D_[u], static_cast<int>(sr_ / 1000));
        a_[u] = T <= 0 ? 0.f : static_cast<float>(std::pow(0.1, Dp / (T * sr_)));
      } else {
        a_[u] = static_cast<float>(0.95 * s.feedback);
      }
    }
    if (primed_) pendingTaps_ = true;
    primed_ = true;
  }

  double sr_ = 0;
  int mask_ = 0, w_ = 0, firPos_ = 0, dryW_ = 0;
  std::vector<float> line_[kBands][2];
  Bq bank_[2][kBands][8], iir_[kBands][8];
  int bankN_[2][kBands] = {}, nSec_[kBands] = {};
  Fft fft_;
  std::vector<std::complex<double>> kern_[kBands], work_, work2_;
  std::vector<float> firIn_[2], firOut_[2], dry_[2];
  float firAcc_[kBands][2][2048] = {};
  int D_[kBands] = {}, tap_[2][kBands] = {};
  double cur_[kBands] = {}, q_ = 1, fadeTo_ = 1, hpS_ = -1, mixS_ = -1;
  float a_[kBands] = {}, tgL_[kBands] = {}, tgR_[kBands] = {}, gL_[kBands] = {}, gR_[kBands] = {};
  float hpX_[2] = {}, hpY_[2] = {}, env_[2] = {};
  bool fading_ = false, pendingTaps_ = false, primed_ = false;
  Settings last_{};
};

// "mbd8" data line: 384 band values (bank, target, band order).
inline bool parseBanks(const std::string& data, float* out) {
  const size_t at = data.find("mbd8");
  for (int i = 0; i < kVals; ++i) out[i] = (i / kBands) % 3 == 2 ? 0.5f : (i / kBands) % 3 == 1 ? 1.f : 0.f;
  if (at == std::string::npos) return false;
  const char* p = data.c_str() + at + 4;
  for (int i = 0; i < kVals; ++i) {
    char* end = nullptr;
    const float v = std::strtof(p, &end);
    if (end == p) break;
    out[i] = std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f;
    p = end;
  }
  return true;
}

}  // namespace gl::packs::spacefx::mbd

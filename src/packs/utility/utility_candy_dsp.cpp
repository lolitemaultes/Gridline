// Gridline — "utility" pack: Scope reference display engine (meters with
// per-type ballistics, reassigned spectrum). Runs on the UI thread.
#include "utility_candy.h"
#include <algorithm>
#include <cmath>

namespace gl::packs::utility {

namespace {
constexpr double kPi = 3.14159265358979323846;
double curveK(double c) { return (std::pow(6001.0, std::clamp(c, 0.0, 1.0)) - 1.0) / 6.0; }
}  // namespace

double candyMeterPos(double amp, double curve) {
  double a = std::clamp(amp * 0.5, 0.0, 1.0);  // 1.0 = +6 dB
  double k = curveK(curve);
  if (k < 1e-6) return a;
  return std::log1p(k * a) / std::log1p(k);
}

double candyMeterAmp(double pos, double curve) {
  pos = std::clamp(pos, 0.0, 1.0);
  double k = curveK(curve);
  if (k < 1e-6) return pos * 2.0;
  return std::expm1(pos * std::log1p(k)) / k * 2.0;
}

// ------------------------------------------------------------------ meter

void CandyMeter::configure(const Config& c) {
  bool rate = c.sr != designedSr_;
  bool type = c.type != c_.type;
  c_ = c;
  if (rate) {
    designedSr_ = c_.sr;
    block_ = std::max(1, static_cast<int>(std::lround(c_.sr / 50.0)));
    // A-weighting (IEC 61672) from its analogue poles, each prewarped, then
    // normalised to 0 dB at 1 kHz.
    const double cc = 2.0 * c_.sr;
    auto warp = [&](double f) { return cc * std::tan(kPi * std::min(f, c_.sr * 0.49) / c_.sr); };
    const double w1 = warp(20.598997), w2 = warp(107.65265), w3 = warp(737.86223), w4 = warp(12194.217);
    const double num[3][3] = {{1, 0, 0}, {1, 0, 0}, {0, 0, 1}};  // n2, n1, n0
    const double den[3][2] = {{2 * w1, w1 * w1}, {w2 + w3, w2 * w3}, {2 * w4, w4 * w4}};  // d1, d0
    for (int s = 0; s < 3; ++s) {
      double n2 = num[s][0], n1 = num[s][1], n0 = num[s][2], d1 = den[s][0], d0 = den[s][1];
      double a0 = cc * cc + d1 * cc + d0;
      awB_[s][0] = (n2 * cc * cc + n1 * cc + n0) / a0;
      awB_[s][1] = (2 * n0 - 2 * n2 * cc * cc) / a0;
      awB_[s][2] = (n2 * cc * cc - n1 * cc + n0) / a0;
      awA_[s][0] = (2 * d0 - 2 * cc * cc) / a0;
      awA_[s][1] = (cc * cc - d1 * cc + d0) / a0;
    }
    double w = 2 * kPi * 1000.0 / c_.sr, mag = 1;
    for (int s = 0; s < 3; ++s) {
      double nr = awB_[s][0] + awB_[s][1] * std::cos(w) + awB_[s][2] * std::cos(2 * w);
      double ni = -(awB_[s][1] * std::sin(w) + awB_[s][2] * std::sin(2 * w));
      double dr = 1 + awA_[s][0] * std::cos(w) + awA_[s][1] * std::cos(2 * w);
      double di = -(awA_[s][0] * std::sin(w) + awA_[s][1] * std::sin(2 * w));
      mag *= std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    awGain_ = 1.0 / std::max(mag, 1e-12);
    lufs_.setSampleRate(c_.sr);
  }
  if (rate || type) reset();
}

void CandyMeter::reset() {
  fill_ = 0;
  for (int ch = 0; ch < 2; ++ch) {
    blockPeak_[ch] = env_[ch] = vu1_[ch] = vu2_[ch] = ms_[ch] = leq_[ch] = 0;
    level_[ch] = pos_[ch] = hold_[ch] = holdLeft_[ch] = 0;
    for (auto& s : aw_[ch]) s[0] = s[1] = 0;
  }
  lufs_.reset();
}

void CandyMeter::feed(const float* L, const float* R, size_t n) {
  const double sr = c_.sr;
  const double aPpmA = std::exp(-1.0 / (0.0025 * sr)), aPpmR = std::exp(-1.0 / (0.65 * sr));
  const double aRms = std::exp(-1.0 / (0.1 * sr)), aVu = std::exp(-1.0 / (0.045 * sr)), aLeq = std::exp(-1.0 / (1.0 * sr));
  const float* in[2] = {L, R};
  size_t i = 0;
  while (i < n) {
    size_t m = std::min(n - i, static_cast<size_t>(block_ - fill_));
    if (c_.type == SmtLoudness) lufs_.process(L + i, R + i, static_cast<int>(m));
    for (int ch = 0; ch < 2; ++ch) {
      const float* x = in[ch] + i;
      for (size_t k = 0; k < m; ++k) {
        double v = x[k], av = std::fabs(v);
        blockPeak_[ch] = std::max(blockPeak_[ch], av);
        switch (c_.type) {
          case SmtPpm:
            env_[ch] = av > env_[ch] ? av + (env_[ch] - av) * aPpmA : env_[ch] * aPpmR;
            ms_[ch] = std::max(ms_[ch], env_[ch]);  // block maximum of the envelope
            break;
          case SmtRms: env_[ch] = v * v + (env_[ch] - v * v) * aRms; break;
          case SmtVu:
            vu1_[ch] = av + (vu1_[ch] - av) * aVu;
            vu2_[ch] = vu1_[ch] + (vu2_[ch] - vu1_[ch]) * aVu;
            break;
          case SmtLeqA: {
            double y = v * awGain_;
            for (int s = 0; s < 3; ++s) {
              double o = awB_[s][0] * y + aw_[ch][s][0];
              aw_[ch][s][0] = awB_[s][1] * y - awA_[s][0] * o + aw_[ch][s][1];
              aw_[ch][s][1] = awB_[s][2] * y - awA_[s][1] * o;
              y = o;
            }
            leq_[ch] = y * y + (leq_[ch] - y * y) * aLeq;
            break;
          }
          default: break;
        }
      }
    }
    fill_ += static_cast<int>(m);
    i += m;
    if (fill_ >= block_) endBlock();
  }
  for (int ch = 0; ch < 2; ++ch) {
    for (auto& s : aw_[ch])
      for (double& z : s)
        if (std::fabs(z) < 1e-25) z = 0;
    if (env_[ch] < 1e-25) env_[ch] = 0;
  }
}

void CandyMeter::endBlock() {
  fill_ = 0;
  const double dtMs = 1000.0 * block_ / c_.sr;
  const double fall = dtMs / std::max(1.0, c_.fallMs);
  for (int ch = 0; ch < 2; ++ch) {
    double lv = 0;
    switch (c_.type) {
      case SmtPeak: lv = blockPeak_[ch]; break;
      case SmtPpm: lv = ms_[ch]; break;
      case SmtRms: lv = std::sqrt(env_[ch]); break;
      case SmtVu: lv = vu2_[ch] * 1.1107207345; break;  // a sine reads its RMS
      case SmtLeqA: lv = std::sqrt(leq_[ch]); break;
      default: {
        float l = lufs_.momentary();
        lv = l <= kNoLoudness + 1 ? 0.0 : std::pow(10.0, l / 20.0);
        break;
      }
    }
    blockPeak_[ch] = 0;
    ms_[ch] = 0;
    level_[ch] = lv;
    double target = candyMeterPos(lv, c_.curve);
    pos_[ch] = std::max(target, pos_[ch] - fall);
    if (!c_.holdInfinite && c_.holdMs <= 0) {
      hold_[ch] = pos_[ch];
    } else if (pos_[ch] >= hold_[ch]) {
      hold_[ch] = pos_[ch];
      holdLeft_[ch] = c_.holdMs;
    } else if (!c_.holdInfinite) {
      holdLeft_[ch] -= dtMs;
      if (holdLeft_[ch] <= 0) hold_[ch] = std::max(pos_[ch], hold_[ch] - fall);
    }
  }
}

// --------------------------------------------------------------- spectrum

double candyAxisHz(double u, double curve, double nyquist) {
  u = std::clamp(u, 0.0, 1.0);
  curve = std::clamp(curve, 0.0, 1.0);
  double lin = 20.0 + u * (nyquist - 20.0), lg = 20.0 * std::pow(nyquist / 20.0, u);
  return std::exp((1 - curve) * std::log(lin) + curve * std::log(lg));
}

void CandySpectrum::init(int fftSize) {
  if (fftSize == n_) return;
  n_ = fftSize;
  fft_.init(n_);
  const size_t N = static_cast<size_t>(n_);
  win_.assign(N, 0.f);
  dwin_.assign(N, 0.f);
  re_.assign(N, 0.f);
  im_.assign(N, 0.f);
  db_.assign(N / 2 + 1, -200.f);
  hz_.assign(N / 2 + 1, 0.f);
  plain_.assign(N / 2 + 1, -200.f);
  double sum = 0;
  for (int i = 0; i < n_; ++i) {
    double ph = 2 * kPi * i / n_;
    win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(ph));
    dwin_[static_cast<size_t>(i)] = static_cast<float>(kPi / n_ * std::sin(ph));  // dw/dn
    sum += win_[static_cast<size_t>(i)];
  }
  norm_ = static_cast<float>(2.0 / sum);
}

void CandySpectrum::analyse(const float* x, double sr, bool sharpen) {
  const int N = n_;
  for (int i = 0; i < N; ++i) {
    size_t k = static_cast<size_t>(i);
    re_[k] = x[i] * win_[k];
    im_[k] = sharpen ? x[i] * dwin_[k] : 0.f;
  }
  fft_.transform(re_.data(), im_.data(), false);
  const double binHz = sr / N;
  for (int k = 0; k <= N / 2; ++k) {
    size_t a = static_cast<size_t>(k), b = static_cast<size_t>((N - k) % N);
    // Separate the two real transforms packed in re / im.
    double hr = 0.5 * (re_[a] + re_[b]), hi = 0.5 * (im_[a] - im_[b]);   // X_h
    double dr = 0.5 * (im_[a] + im_[b]), di = -0.5 * (re_[a] - re_[b]);  // X_dh
    if (!sharpen) hr = re_[a], hi = im_[a];
    double p = hr * hr + hi * hi;
    double mag = std::sqrt(p) * norm_;
    db_[a] = static_cast<float>(20.0 * std::log10(std::max(mag, 1e-10)));
    plain_[a] = db_[a];
    double f = k;
    if (sharpen && p > 1e-20) {
      // Reassigned frequency: k − Im(X_dh · conj(X_h)) / |X_h|² · N / 2π.
      double im = di * hr - dr * hi;
      double off = -im / p * N / (2 * kPi);
      if (std::fabs(off) < 2.0) f = k + off;
    }
    hz_[a] = static_cast<float>(std::max(0.0, f) * binHz);
  }
}

double CandySpectrum::peakHz() const {
  size_t best = 1;
  for (size_t k = 1; k < db_.size(); ++k)
    if (db_[k] > db_[best]) best = k;
  return hz_[best];
}

void CandySpectrum::columns(int cols, double curve, bool pink, double gainDb, double sr, std::vector<float>& out) const {
  out.assign(static_cast<size_t>(std::max(cols, 1)), -200.f);
  if (n_ <= 0) return;
  const double nyq = sr * 0.5, binHz = sr / n_;
  // Maximum of the (reassigned) bins that land in each column.
  std::vector<float> mx(out.size(), -200.f);
  std::vector<double> edge(out.size() + 1);
  for (size_t c = 0; c <= out.size(); ++c) edge[c] = candyAxisHz(static_cast<double>(c) / static_cast<double>(out.size()), curve, nyq);
  for (size_t k = 1; k < db_.size(); ++k) {
    double f = hz_[k];
    if (f < edge.front() || f >= edge.back()) continue;
    size_t c = static_cast<size_t>(std::upper_bound(edge.begin(), edge.end(), f) - edge.begin());
    c = c > 0 ? c - 1 : 0;
    if (c < mx.size()) mx[c] = std::max(mx[c], db_[k]);
  }
  for (size_t c = 0; c < out.size(); ++c) {
    double f0 = edge[c], f1 = edge[c + 1], fc = std::sqrt(f0 * f1);
    float v = mx[c];
    if (v <= -199.f && f1 - f0 < binHz) {  // columns narrower than a bin: interpolate the plain spectrum
      double pos = fc / binHz;
      size_t i = std::min(static_cast<size_t>(pos), plain_.size() - 2);
      double t = std::clamp(pos - static_cast<double>(i), 0.0, 1.0);
      v = static_cast<float>(plain_[i] * (1 - t) + plain_[i + 1] * t);
    }
    if (v > -199.f) v += static_cast<float>(gainDb + (pink ? 3.0 * std::log2(std::max(fc, 20.0) / 1000.0) : 0.0));
    out[c] = v;
  }
}

}  // namespace gl::packs::utility

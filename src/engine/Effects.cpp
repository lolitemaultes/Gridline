#include "../packs/Packs.h"
#include "../host/HostedPlugins.h"
#include "Effects.h"
#include <atomic>
#include "EffectsK3.h"
#include <array>
#include <cmath>
#include <vector>
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "EqDesign.h"

namespace gl {

using namespace dsp;

namespace {

float param(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}

// Circular delay line with fractional (linear) reads.
struct DelayLine {
  std::vector<float> buf;
  size_t w = 0;
  void init(size_t len) {
    buf.assign(std::max<size_t>(len, 4), 0.f);
    w = 0;
  }
  void push(float x) {
    buf[w] = x;
    w = (w + 1) % buf.size();
  }
  // Sample written `d` samples ago (d >= 1).
  float read(double d) const {
    double len = static_cast<double>(buf.size());
    d = std::clamp(d, 1.0, len - 2);
    double rp = static_cast<double>(w) - d;
    while (rp < 0) rp += len;
    size_t i0 = static_cast<size_t>(rp) % buf.size(), i1 = (i0 + 1) % buf.size();
    float fr = static_cast<float>(rp - std::floor(rp));
    return buf[i0] + (buf[i1] - buf[i0]) * fr;
  }
  void clear() { std::fill(buf.begin(), buf.end(), 0.f); }
};

// ---------------------------------------------------------------------------
// Linear-phase EQ: an N-tap symmetric FIR sampled from the bands' magnitude
// response (Blackman window), run by FFT overlap-add in N-sample blocks.
// Latency: N samples of block buffering + N/2 of FIR group delay. Buffers are
// sized for the largest N once, when linear phase is first switched on (like a
// sample-rate change), so EQs that never use it cost no memory.
class LinearPhaseEq {
 public:
  static constexpr int kMaxTaps = 16384, kMaxFft = 2 * kMaxTaps;
  void allocate() {
    if (!re_.empty()) return;
    re_.assign(kMaxFft, 0.f), im_.assign(kMaxFft, 0.f);
    hre_.assign(kMaxFft, 0.f), him_.assign(kMaxFft, 0.f);
    for (int c = 0; c < 2; ++c) in_[c].assign(kMaxTaps, 0.f), out_[c].assign(kMaxTaps, 0.f), ola_[c].assign(kMaxTaps, 0.f);
    mag_.assign(kMaxTaps / 2 + 1, 1.0);
    cs_.assign(kMaxFft / 2, 0.f), sn_.assign(kMaxFft / 2, 0.f);
    for (int i = 0; i < kMaxFft / 2; ++i) {
      cs_[static_cast<size_t>(i)] = static_cast<float>(std::cos(2.0 * kPi * i / kMaxFft));
      sn_[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * kPi * i / kMaxFft));
    }
  }
  int taps() const { return n_; }
  void setTaps(int n) {
    allocate();
    n = std::clamp(n, 64, kMaxTaps);
    if (n == n_) return;
    n_ = n;
    reset();
    dirty_ = true;
  }
  void markDirty() { dirty_ = true; }
  void reset() {
    pos_ = 0;
    if (re_.empty()) return;
    for (int c = 0; c < 2; ++c) std::fill(in_[c].begin(), in_[c].end(), 0.f), std::fill(out_[c].begin(), out_[c].end(), 0.f),
                                std::fill(ola_[c].begin(), ola_[c].end(), 0.f);
  }
  // Rebuilds the FIR from the band filters (coefficients only) and the output gain.
  void design(const EqBandFilters* bands, int count, double gain) {
    const int N = n_, half = N / 2;
    for (int k = 0; k <= half; ++k) {
      double w = 2.0 * kPi * k / N;
      double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
      double m = gain;
      for (int b = 0; b < count; ++b)
        for (int st = 0; st < bands[b].count; ++st) {
          const dsp::Biquad& q = bands[b].stage[st];
          double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = -(q.b1 * s1 + q.b2 * s2);
          double dr = 1 + q.a1 * c1 + q.a2 * c2, di = -(q.a1 * s1 + q.a2 * s2);
          m *= std::sqrt((nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di));
        }
      mag_[static_cast<size_t>(k)] = m;
    }
    // Zero-phase impulse (real, even): h[i] = (1/N) Σ A[k] e^{i 2π k i / N}, centred at N/2, windowed.
    const int F = 2 * N;
    std::fill(re_.begin(), re_.begin() + N, 0.f), std::fill(im_.begin(), im_.begin() + N, 0.f);
    for (int k = 0; k <= half; ++k) {
      // linear phase: A[k] · e^{-iπk} (shift by N/2) → alternating sign
      float v = static_cast<float>(mag_[static_cast<size_t>(k)] * ((k & 1) ? -1.0 : 1.0));
      re_[static_cast<size_t>(k)] = v;
      if (k > 0 && k < half) re_[static_cast<size_t>(N - k)] = v;
    }
    fft(re_.data(), im_.data(), N, true);
    for (int i = 0; i < N; ++i) {
      double wdw = 0.42 - 0.5 * std::cos(2 * kPi * i / (N - 1)) + 0.08 * std::cos(4 * kPi * i / (N - 1));
      hre_[static_cast<size_t>(i)] = static_cast<float>(re_[static_cast<size_t>(i)] / N * wdw);
      him_[static_cast<size_t>(i)] = 0.f;
    }
    std::fill(hre_.begin() + N, hre_.begin() + F, 0.f), std::fill(him_.begin() + N, him_.begin() + F, 0.f);
    fft(hre_.data(), him_.data(), F, false);
    dirty_ = false;
  }
  bool dirty() const { return dirty_; }
  // Processes in place; `redesign` is called at block boundaries while dirty.
  template <class Redesign>
  void process(float* L, float* R, int n, Redesign&& redesign) {
    float* ch[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      for (int c = 0; c < 2; ++c) {
        in_[c][static_cast<size_t>(pos_)] = ch[c][i];
        ch[c][i] = out_[c][static_cast<size_t>(pos_)];
      }
      if (++pos_ == n_) {
        if (dirty_) redesign();
        block();
        pos_ = 0;
      }
    }
  }

 private:
  void block() {
    const int N = n_, F = 2 * N;
    for (int c = 0; c < 2; ++c) {
      std::copy(in_[c].begin(), in_[c].begin() + N, re_.begin());
      std::fill(re_.begin() + N, re_.begin() + F, 0.f);
      std::fill(im_.begin(), im_.begin() + F, 0.f);
      fft(re_.data(), im_.data(), F, false);
      for (int k = 0; k < F; ++k) {
        float a = re_[static_cast<size_t>(k)], b = im_[static_cast<size_t>(k)], c2 = hre_[static_cast<size_t>(k)], d = him_[static_cast<size_t>(k)];
        re_[static_cast<size_t>(k)] = a * c2 - b * d;
        im_[static_cast<size_t>(k)] = a * d + b * c2;
      }
      fft(re_.data(), im_.data(), F, true);
      const float sc = 1.0f / static_cast<float>(F);
      for (int i = 0; i < N; ++i) {
        out_[c][static_cast<size_t>(i)] = re_[static_cast<size_t>(i)] * sc + ola_[c][static_cast<size_t>(i)];
        ola_[c][static_cast<size_t>(i)] = re_[static_cast<size_t>(N + i)] * sc;
      }
    }
  }
  // In-place radix-2 complex FFT of size n (power of two ≤ kMaxFft); inverse without scaling.
  void fft(float* re, float* im, int n, bool inverse) const {
    for (int i = 1, j = 0; i < n; ++i) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) std::swap(re[i], re[j]), std::swap(im[i], im[j]);
    }
    const float sg = inverse ? 1.0f : -1.0f;
    for (int len = 2; len <= n; len <<= 1) {
      int half = len >> 1, step = kMaxFft / len;
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < half; ++j) {
          float wr = cs_[static_cast<size_t>(j * step)], wi = sg * sn_[static_cast<size_t>(j * step)];
          int a = i + j, b = a + half;
          float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - xr, im[b] = im[a] - xi;
          re[a] += xr, im[a] += xi;
        }
    }
  }
  int n_ = 0, pos_ = 0;
  bool dirty_ = true;
  std::vector<float> re_, im_, hre_, him_, cs_, sn_;
  std::vector<float> in_[2], out_[2], ola_[2];
  std::vector<double> mag_;
};

// ---------------------------------------------------------------------------
class EqFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    if (ctx.sampleRate != sr_ || s.params != last_) {
      // J-30: only the bands whose own settings changed are redesigned (an
      // automated band used to redesign all seven every control block).
      const bool all = ctx.sampleRate != sr_ || s.params.size() != last_.size() || changed(s, pp::EqHQ) || changed(s, pp::EqModel);
      auto bandChanged = [&](int b) {
        if (all) return true;
        for (int f = 0; f < pp::EqFields; ++f)
          if (changed(s, pp::eqParam(b, f))) return true;
        for (int f = 0; f < pp::EqExtFields; ++f)
          if (changed(s, pp::eqExt(b, f))) return true;
        for (int f = 0; f < pp::EqFlFields; ++f)
          if (changed(s, pp::eqFl(b, f))) return true;
        return false;
      };
      bool redesign[pp::kEqBands];
      for (int b = 0; b < pp::kEqBands; ++b) redesign[b] = bandChanged(b);
      sr_ = ctx.sampleRate;
      last_ = s.params;
      for (int b = 0; b < pp::kEqBands; ++b) {
        if (!redesign[b]) continue;
        EqBandFilters f;
        designEqBand(s, b, sr_, f);
        for (int c = 0; c < 2; ++c) {
          // Keep filter memory when only coefficients change (no clicks).
          for (int k = 0; k < 4; ++k) {
            f.stage[k].x1 = band_[c][b].stage[k].x1;
            f.stage[k].x2 = band_[c][b].stage[k].x2;
            f.stage[k].y1 = band_[c][b].stage[k].y1;
            f.stage[k].y2 = band_[c][b].stage[k].y2;
          }
          band_[c][b] = f;
        }
      }
      out_ = param(s, pp::EqModel, 1) >= 1.5f ? param(s, pp::EqMainLin, 1) : dbToGain(param(s, pp::EqMain));  // J-26 classic: linear
      linDirty_ = true;
      listen_ = std::clamp(static_cast<int>(param(s, pp::EqListen) + 0.5f), 0, pp::kEqBands);
      if (listen_ > 0)
        for (auto& l : listenF_) {
          dsp::Biquad nb;
          designEqListen(s, listen_ - 1, sr_, nb, static_cast<int>(param(s, pp::EqSoloShift, 1) + 0.5f));
          nb.x1 = l.x1, nb.x2 = l.x2, nb.y1 = l.y1, nb.y2 = l.y2;
          l = nb;
        }
    }
    float* ch[2] = {L, R};
    const int linTaps = linearTaps(s);
    if (linTaps > 0) {  // linear phase: FIR of the same magnitude response
      lin_.setTaps(linTaps);
      if (linDirty_) lin_.markDirty(), linDirty_ = false;
      lin_.process(L, R, n, [&] {
        EqBandFilters f[pp::kEqBands];
        for (int b = 0; b < pp::kEqBands; ++b) designEqBand(s, b, sr_, f[b]);
        lin_.design(f, pp::kEqBands, out_);
      });
      return;
    }
    if (listen_ > 0) {  // band solo: hear only the region around the band
      for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) ch[c][i] = static_cast<float>(listenF_[c].process(ch[c][i]));
      return;
    }
    for (int c = 0; c < 2; ++c)
      for (int b = 0; b < pp::kEqBands; ++b) {
        EqBandFilters& f = band_[c][b];
        for (int k = 0; k < f.count; ++k)
          for (int i = 0; i < n; ++i) ch[c][i] = static_cast<float>(f.stage[k].process(ch[c][i]));
      }
    for (int i = 0; i < n; ++i) {
      L[i] = static_cast<float>(L[i] * out_);
      R[i] = static_cast<float>(R[i] * out_);
    }
  }
  void reset() override {
    for (auto& c : band_)
      for (auto& b : c)
        for (auto& st : b.stage) st.clear();
    listenF_[0].clear(), listenF_[1].clear();
    lin_.reset();
  }
  // Linear phase: N samples of block buffering + N/2 of FIR group delay.
  int latencyFor(const PluginState& s, double) const override { return 3 * linearTaps(s) / 2; }

 private:
  // FIR length of the linear-phase mode (0 = minimum phase; band solo
  // always runs the IIR listen filter), clamped as LinearPhaseEq::setTaps does.
  static int linearTaps(const PluginState& s) {
    const int listen = std::clamp(static_cast<int>(param(s, pp::EqListen) + 0.5f), 0, pp::kEqBands);
    const int taps = pp::eqLinearTaps(static_cast<int>(param(s, pp::EqLinear) + 0.5f));
    return taps > 0 && listen == 0 ? std::clamp(taps, 64, LinearPhaseEq::kMaxTaps) : 0;
  }
  // Parameter `i` differs from the last processed state (same-size vectors).
  bool changed(const PluginState& s, int i) const {
    const size_t k = static_cast<size_t>(i);
    return k < s.params.size() && k < last_.size() && s.params[k] != last_[k];
  }
  EqBandFilters band_[2][pp::kEqBands];
  LinearPhaseEq lin_;
  bool linDirty_ = true;
  dsp::Biquad listenF_[2];
  int listen_ = 0;
  std::vector<float> last_;
  double sr_ = 0, out_ = 1;
};

// ---------------------------------------------------------------------------
class PassFx final : public Effect {  // analyzer: display only (publishes stereo statistics)
 public:
  void process(const PluginState&, const ProcessContext&, float* L, float* R, int n) override {
    double pl = 0, pr = 0, sl = 0, sr = 0, slr = 0;
    for (int i = 0; i < n; ++i) {
      pl = std::max(pl, static_cast<double>(std::fabs(L[i]))), pr = std::max(pr, static_cast<double>(std::fabs(R[i])));
      sl += static_cast<double>(L[i]) * L[i], sr += static_cast<double>(R[i]) * R[i], slr += static_cast<double>(L[i]) * R[i];
    }
    // Smoothed statistics: [0] peak L dB, [1] peak R dB, [2] correlation -1..1, [3] RMS dB.
    accL_ = accL_ * 0.9 + sl, accR_ = accR_ * 0.9 + sr, accLR_ = accLR_ * 0.9 + slr, accN_ = accN_ * 0.9 + n;
    m_[0] = static_cast<float>(gainToDb(pl)), m_[1] = static_cast<float>(gainToDb(pr));
    double den = std::sqrt(accL_ * accR_);
    m_[2] = den > 1e-12 ? static_cast<float>(accLR_ / den) : 1.f;
    m_[3] = static_cast<float>(10.0 * std::log10(std::max(1e-12, (accL_ + accR_) * 0.5 / std::max(1.0, accN_))));
  }
  void reset() override { accL_ = accR_ = accLR_ = accN_ = 0; }
  int meters(float* out, int max) const override {
    int k = std::min(max, 4);
    for (int i = 0; i < k; ++i) out[i] = m_[i];
    return k;
  }

 private:
  double accL_ = 0, accR_ = 0, accLR_ = 0, accN_ = 0;
  float m_[4] = {-120, -120, 1, -120};
};

}  // namespace

std::unique_ptr<Effect> createEffectW2(const std::string& type);  // Effects2.cpp
std::unique_ptr<Effect> createEffectW3(const std::string& type);  // Effects3.cpp (dynamics)
std::unique_ptr<Effect> createEffectW4(const std::string& type);  // Effects4.cpp
std::unique_ptr<Effect> createEffectW5(const std::string& type);  // Effects5.cpp (delay, reverb, modulation, filter)

namespace {
std::atomic<EffectFactory> gTestFactory{nullptr};
}
void setTestEffectFactory(EffectFactory f) { gTestFactory.store(f); }

int effectLatency(const PluginState& state, double sampleRate) {
  if (state.type.empty() || isHostedType(state.type)) return 0;
  auto fx = createEffect(state.type);
  return fx ? std::max(0, fx->latencyFor(state, sampleRate)) : 0;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (EffectFactory f = gTestFactory.load())
    if (auto fx = f(type)) return fx;
  if (isHostedType(type)) return host::createHostedEffect(type);
  if (auto fx = createEffectK3(type)) return fx;
  if (auto fx = createEffectW3(type)) return fx;
  if (auto fx = createEffectW4(type)) return fx;
  if (auto fx = createEffectW5(type)) return fx;
  if (auto fx = createEffectW2(type)) return fx;
  if (auto fx = packs::createEffect(type)) return fx;
  if (type == "gl.eq") return std::make_unique<EqFx>();
  if (type == "gl.analyzer") return std::make_unique<PassFx>();
  return nullptr;
}

}  // namespace gl

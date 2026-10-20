// Gridline — Wave 2 effects: Graphic EQ, Waveshaper, Auto Pan, Shaper,
// Time Gate, Maximizer, Freq Shifter, Low Lifter, Tuner.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <vector>
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "Effects.h"
#include "WaveCurve.h"
#include <functional>
#include <string>

namespace gl {

using namespace dsp;

namespace {

float param(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}

// Tempo-synced phase in [0,1) for a period of `beats` beats, or free-running.
struct SyncPhase {
  double free = 0;
  double at(const ProcessContext& ctx, double beats, int i) const {
    double spb = 60.0 / std::max(1.0, ctx.tempo) * ctx.sampleRate;  // samples per beat
    double b = ctx.tick / std::max(1, ctx.ppq) + i / spb;
    double ph = b / beats;
    return ph - std::floor(ph);
  }
};

// =============================================================== Graphic EQ
class GeqFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    if (s.params != last_ || ctx.sampleRate != sr_) {
      last_ = s.params;
      sr_ = ctx.sampleRate;
      bool classic = param(s, GeqLayout) >= 0.5f;
      double oct = param(s, GeqBandwidth, 1.27f);
      double qPeak = std::sqrt(std::pow(2.0, oct)) / (std::pow(2.0, oct) - 1.0);  // octaves → Q
      for (int b = 0; b < kGeqBands; ++b)
        for (int c = 0; c < 2; ++c) {
          Biquad::Type t = b == 0 ? Biquad::LowShelf : b == kGeqBands - 1 ? Biquad::HighShelf : Biquad::Peak;
          double x1 = f_[c][b].x1, x2 = f_[c][b].x2, y1 = f_[c][b].y1, y2 = f_[c][b].y2;
          f_[c][b].set(t, sr_, classic ? kGeqFreqsClassic[b] : kGeqFreqs[b], b == 0 || b == kGeqBands - 1 ? 0.707 : qPeak, param(s, b));
          f_[c][b].x1 = x1, f_[c][b].x2 = x2, f_[c][b].y1 = y1, f_[c][b].y2 = y2;
        }
      out_ = dbToGain(param(s, GeqOutput));
      mix_ = param(s, GeqMix, 1);
    }
    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      for (int b = 0; b < kGeqBands; ++b) {
        l = f_[0][b].process(l);
        r = f_[1][b].process(r);
      }
      L[i] = static_cast<float>((L[i] + (l - L[i]) * mix_) * out_);
      R[i] = static_cast<float>((R[i] + (r - R[i]) * mix_) * out_);
    }
  }
  void reset() override {
    for (auto& c : f_)
      for (auto& b : c) b.clear();
  }

 private:
  Biquad f_[2][pp::kGeqBands];
  std::vector<float> last_;
  double sr_ = 0, out_ = 1, mix_ = 1;
};

// =============================================================== Waveshaper
double shapeCurve(const PluginState& s, double x) {
  using namespace pp;
  bool sym = param(s, WsBipolar, 1) >= 0.5f;
  int interp = static_cast<int>(param(s, WsInterp) + 0.5f);
  auto pt = [&](int i) { return static_cast<double>(param(s, std::clamp(i, 0, kShapePoints - 1))); };
  auto lookup = [&](double u) {  // u in [-1,1]
    double f = (u + 1) * 0.5 * (kShapePoints - 1);
    int i = std::clamp(static_cast<int>(f), 0, kShapePoints - 2);
    double t = f - i;
    if (interp == 2) return pt(static_cast<int>(std::lround(f)));  // steps
    if (interp == 1) {  // Catmull-Rom through the points
      double p0 = pt(i - 1), p1 = pt(i), p2 = pt(i + 1), p3 = pt(i + 2);
      double t2 = t * t, t3 = t2 * t;
      return 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
    }
    return pt(i) * (1 - t) + pt(i + 1) * t;
  };
  x = std::clamp(x, -1.0, 1.0);
  if (sym) {
    double a = std::fabs(x);
    double y = lookup(a) - lookup(0);  // zero stays zero
    return x < 0 ? -y : y;
  }
  return lookup(x);
}

// J-45: the reference waveshaper's 2x stage: a polyphase IIR half-band (two
// chains of three first-order all-pass sections at the low rate, double state,
// float between stages). Up: one input → (B chain, A chain); down: even → A,
// odd → B, (A + B) / 2.
struct WsHalfband {
  static constexpr double kA[3] = {0x1.2dd40d58c3526p-3, 0x1.ee0c3bd22db5fp-2, 0x1.c4193c17c4250p-1};
  static constexpr double kB[3] = {0x1.40bad9fb6183ap-5, 0x1.35e90e2f3bc08p-2, 0x1.596741f70a58fp-1};
  double ax[3] = {}, ay[3] = {}, bx[3] = {}, by[3] = {};
  static double chain(const double* c, double* x1, double* y1, double x) {
    for (int i = 0; i < 3; ++i) {
      const double y = c[i] * (x - y1[i]) + x1[i];
      x1[i] = x, y1[i] = y, x = y;
    }
    return x;
  }
  void up(float x, float& first, float& second) {
    first = static_cast<float>(chain(kB, bx, by, x));
    second = static_cast<float>(chain(kA, ax, ay, x));
  }
  float down(float even, float odd) { return static_cast<float>((chain(kB, bx, by, odd) + chain(kA, ax, ay, even)) * 0.5); }
  void flush() {  // once per block: tiny output states go to zero
    for (int i = 0; i < 3; ++i) {
      if (std::fabs(ay[i]) < 0x1p-24) ay[i] = 0;
      if (std::fabs(by[i]) < 0x1p-24) by[i] = 0;
    }
  }
};

class WaveshaperFx final : public Effect {
 public:
  WaveshaperFx() : tab_(32770, 0.f), fl_(2 * 2 * kFlChunk * 16, 0.f), flTab_(32770, 0) {}
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const bool classic = param(s, WsClassic, 1) >= 0.5f;
    const bool unipolar = param(s, WsBipolar, 1) >= 0.5f;
    if (classic && wscurve::has(s.data.c_str())) {  // J-45: the reference engine
      for (int o = 0; o < n; o += kFlChunk) processFl(s, ctx, L + o, R + o, std::min(kFlChunk, n - o));
      return;
    }
    bool center = param(s, WsCenter) >= 0.5f;
    int os = stages(s);
    int lat = Oversampler::latency(os);
    // Free-point curve → lookup table (16384 steps unipolar, 32768 bipolar), rebuilt when it changes.
    const bool hasCurve = wscurve::has(s.data.c_str());
    if (hasCurve) {
      size_t h = std::hash<std::string>{}(s.data) ^ (unipolar ? 0x9e3779b97f4a7c15ull : 0);
      if (h != tabHash_) {
        tabHash_ = h;
        wscurve::parse(s.data.c_str(), curve_);
        tabN_ = unipolar ? 16384 : 32768;
        for (int i = 0; i <= tabN_; ++i) tab_[static_cast<size_t>(i)] = static_cast<float>(curve_.eval(static_cast<double>(i) / tabN_));
        tab_[static_cast<size_t>(tabN_) + 1] = tab_[static_cast<size_t>(tabN_)];
      }
    }
    auto f = [&](double x) {
      if (!hasCurve) return shapeCurve(s, x);
      if (unipolar) {
        double a = std::min(std::fabs(x), 1.0) * tabN_;
        int i = static_cast<int>(a);
        double y = tab_[static_cast<size_t>(i)] + (tab_[static_cast<size_t>(i) + 1] - tab_[static_cast<size_t>(i)]) * (a - i);
        return x < 0 ? -y : y;
      }
      double a = (std::clamp(x, -1.0, 1.0) + 1.0) * 0.5 * tabN_;
      int i = static_cast<int>(a);
      double y = tab_[static_cast<size_t>(i)] + (tab_[static_cast<size_t>(i) + 1] - tab_[static_cast<size_t>(i)]) * (a - i);
      return 2.0 * y - 1.0;
    };
    // Gain staging. Reference: pre, post factors; wet = mix x post, dry = (post - wet) x pre x post.
    double pre, wet, dry, post = 1;
    if (classic) {
      // Reference curves: pre = (4^x - 1) / 3, post = 0.1 (11^x - 1) (exactly 1 at the default).
      double xp = param(s, WsPreAmp, 1), xg = param(s, WsPost, 1);
      pre = (std::pow(4.0, xp) - 1.0) / 3.0;
      double pg = xg == 1.0f ? 1.0 : 0.1 * (std::pow(11.0, xg) - 1.0), mix = param(s, WsMix, 1);
      wet = mix * pg;
      dry = (pg - wet) * pre * pg;
    } else {
      pre = dbToGain(param(s, WsPreGain, 6)), post = dbToGain(param(s, WsPostGain, -3));
      double mix = param(s, WsMix, 1);
      wet = mix * post, dry = 1 - mix;
    }
    const bool smooth = param(s, WsSmooth) >= 0.5f;
    const double sc = smooth ? 1.0 - std::pow(0.1, 1.0 / (0.005 * ctx.sampleRate)) : 1.0;
    if (!primed_) gPre_ = pre, gWet_ = wet, gDry_ = dry, primed_ = true;
    float peak = 0;
    for (int i = 0; i < n; ++i) {
      gPre_ += (pre - gPre_) * sc, gWet_ += (wet - gWet_) * sc, gDry_ += (dry - gDry_) * sc;
      double xl = L[i], xr = R[i];
      peak = std::max(peak, static_cast<float>(std::max(std::fabs(xl), std::fabs(xr)) * gPre_));
      double l = os ? osL_.run(xl * gPre_, os, f) : f(xl * gPre_), r = os ? osR_.run(xr * gPre_, os, f) : f(xr * gPre_);
      if (center) {  // DC blocker (asymmetric curves add offset)
        double ol = l - xl_ + 0.995 * yl_, orr = r - xr_ + 0.995 * yr_;
        xl_ = l, yl_ = ol, xr_ = r, yr_ = orr;
        l = ol, r = orr;
      }
      dl_.push(L[i]), dr_.push(R[i]);
      double dL = lat ? dl_.tap(lat) : xl, dR = lat ? dr_.tap(lat) : xr;
      L[i] = static_cast<float>(l * gWet_ + dL * gDry_);
      R[i] = static_cast<float>(r * gWet_ + dR * gDry_);
    }
    peak_ = std::max(peak, peak_ * 0.8f);
  }
  void reset() override {
    xl_ = yl_ = xr_ = yr_ = 0;
    osL_.reset(), osR_.reset(), dl_.clear(), dr_.clear();
    primed_ = false;
    for (auto& st : flUp_) st[0] = st[1] = WsHalfband{};  // the reference's flush: oversamplers and DC remover
    for (auto& st : flDown_) st[0] = st[1] = WsHalfband{};
    dcM_[0] = dcM_[1] = dcV_[0] = dcV_[1] = 0;
  }
  int meters(float* out, int max) const override {  // input level after the pre amp (editor marker)
    if (max < 1) return 0;
    out[0] = peak_;
    return 1;
  }
  // Oversampling delays the wet path; the dry path is delayed to match. The
  // reference engine mixes the dry signal at the high rate: no latency.
  int latencyFor(const PluginState& s, double) const override {
    if (param(s, pp::WsClassic, 1) >= 0.5f && wscurve::has(s.data.c_str())) return 0;
    return Oversampler::latency(stages(s));
  }

 private:
  static int stages(const PluginState& s) { return std::clamp(static_cast<int>(param(s, pp::WsOversample) + 0.5f), 0, 3); }
  double xl_ = 0, yl_ = 0, xr_ = 0, yr_ = 0, gPre_ = 1, gWet_ = 1, gDry_ = 0;
  bool primed_ = false;
  float peak_ = 0;
  Oversampler osL_, osR_;
  FixedDelay<64> dl_, dr_;
  std::vector<float> tab_;
  int tabN_ = 16384;
  size_t tabHash_ = 0;
  wscurve::Curve curve_;

  // ---- J-45 reference engine: 2^os oversampling (polyphase IIR half-bands),
  // int16 curve table (rounded or interpolated with HQ), dry mixed at the high
  // rate, gains smoothed per oversampled frame, DC remover at the host rate.
  static constexpr int kFlChunk = 1024;
  void buildFlTable(const PluginState& s, bool unipolar) {
    const size_t h = std::hash<std::string>{}(s.data) ^ (unipolar ? 0x51ed2701u : 0x2b7e1516u);
    if (h == flHash_) return;
    flHash_ = h;
    wscurve::parse(s.data.c_str(), curve_);
    flN_ = unipolar ? 16384 : 32768;
    const int M = flN_ - 1;
    const double centre = unipolar ? 0.0 : 0.5, scale = unipolar ? 32767.0 : 65535.0;
    const auto& c = curve_;
    for (int i = 0; i < flN_; ++i) {
      const double xq = static_cast<double>(i) / M;
      float v;
      if (c.n <= 0) {
        v = 0;
      } else if (xq <= c.x[0]) {
        v = static_cast<float>(c.y[0] - centre);
      } else {
        const int k = c.segmentAt(xq);
        if (k >= c.n) {
          v = static_cast<float>(c.y[c.n - 1] - centre);
        } else {
          const float start = static_cast<float>(c.y[k - 1] - centre);
          const float delta = static_cast<float>(static_cast<double>(c.y[k]) - centre - start);
          const double w = c.x[k] - c.x[k - 1];
          const double f = w > 0 ? (xq - c.x[k - 1]) / w : 1.0;
          const float r = static_cast<float>(c.mode[k] == menv::Smooth ? menv::shape(menv::Smooth, c.c[k], f, c.tanS[k], c.tanE[k])
                                                                        : menv::shape(c.mode[k], c.c[k], f));
          v = r * delta + start;
        }
      }
      const double q = std::nearbyint(static_cast<double>(v) * scale);
      flTab_[static_cast<size_t>(i) + 1] = static_cast<int16_t>(std::clamp(q, -32768.0, 32767.0));
    }
    flTab_[0] = flTab_[1];
    flTab_[static_cast<size_t>(flN_) + 1] = flTab_[static_cast<size_t>(flN_)];
  }
  // Table entry i (guards: -1 → 0, N → N - 1).
  double T(int i) const { return flTab_[static_cast<size_t>(i + 1)]; }
  double shapeFl(double x, double g, bool unipolar, bool hq) const {
    const int M = flN_ - 1;
    if (unipolar) {
      const double xs = x * (g * M);
      if (!hq) {
        const double r = std::nearbyint(xs);
        if (r >= 0) return T(static_cast<int>(std::min(r, static_cast<double>(M))));
        return -T(static_cast<int>(std::min(-r, static_cast<double>(M))));
      }
      const double ti = std::trunc(xs);
      const double f = xs - ti;
      if (ti >= 0) {
        const int j = static_cast<int>(std::min(ti, static_cast<double>(M)));
        return T(j) + (T(j + 1) - T(j)) * f;
      }
      if (ti < -2147483647.0) return 0;
      const int j = static_cast<int>(std::min(-ti, static_cast<double>(M)));
      return -(T(j) + (T(j + 1) - T(j)) * (-f));
    }
    const double t = x * (g * M * 0.5) + M * 0.5;
    if (!hq) return T(static_cast<int>(std::clamp(std::nearbyint(t), 0.0, static_cast<double>(M))));
    const double ti = std::trunc(t);
    const int i = static_cast<int>(std::clamp(ti, -1.0, static_cast<double>(M)));
    return T(i) + (T(i + 1) - T(i)) * (t - ti);
  }
  void processFl(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
    using namespace pp;
    const bool unipolar = param(s, WsBipolar, 1) >= 0.5f;
    const bool hq = param(s, WsHq) >= 0.5f, smooth = param(s, WsSmooth) >= 0.5f, center = param(s, WsCenter) >= 0.5f;
    const int os = std::clamp(static_cast<int>(param(s, WsOversample) + 0.5f), 0, 4);
    if (os != flOs_) {  // new stage set: zeroed
      flOs_ = os;
      for (auto& st : flUp_) st[0] = st[1] = WsHalfband{};
      for (auto& st : flDown_) st[0] = st[1] = WsHalfband{};
    }
    buildFlTable(s, unipolar);
    // Gain targets in the reference's float steps (stored knob values / 128).
    const float p0 = static_cast<float>(std::nearbyint(param(s, WsPreAmp, 1) * 128.0)) * 0.0078125f;
    const float p1 = static_cast<float>(std::nearbyint(param(s, WsMix, 1) * 128.0)) * 0.0078125f;
    const float p2 = static_cast<float>(std::nearbyint(param(s, WsPost, 1) * 128.0)) * 0.0078125f;
    float G = static_cast<float>(std::exp(static_cast<double>(p0) * 1.3862943611198906) - 1.0);
    G = G / 3.0f;
    const float P = p2 == 1.0f ? 1.0f : static_cast<float>((std::exp(static_cast<double>(p2 * 2.3978953f)) - 1.0) * static_cast<double>(0.1f));
    const float B = p1 * P;
    const float A = ((P - B) * G) * P;
    const float k = static_cast<float>(1.0 - std::pow(0.09999999999999998, 1.0 / (0.005 * ctx.sampleRate)));
    const int f = 1 << os;
    const int m = n * f;
    float* buf[2] = {fl_.data(), fl_.data() + 2 * kFlChunk * 16};
    float* tmp[2] = {fl_.data() + kFlChunk * 16, fl_.data() + 3 * kFlChunk * 16};
    float* io[2] = {L, R};
    float peak = 0;
    for (int ch = 0; ch < 2; ++ch) {
      std::copy(io[ch], io[ch] + n, buf[ch]);
      int len = n;
      for (int st = 0; st < os; ++st) {
        WsHalfband& h = flUp_[st][ch];
        for (int i = 0; i < len; ++i) h.up(buf[ch][i], tmp[ch][2 * i], tmp[ch][2 * i + 1]);
        h.flush();
        len *= 2;
        std::copy(tmp[ch], tmp[ch] + len, buf[ch]);
      }
      for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(io[ch][i]) * G);
    }
    const double wetScale = 1.0 / 32768.0;
    for (int j = 0; j < m; ++j) {
      float g = G, a = A, b = B;
      if (smooth) {
        sPre_ += (G - sPre_) * k;
        sWet_ += (B - sWet_) * k;
        sDry_ += (A - sDry_) * k;
        g = sPre_, a = sDry_, b = sWet_;
      }
      const double bs = static_cast<double>(b) * wetScale;
      for (int ch = 0; ch < 2; ++ch) {
        const double x = buf[ch][j];
        buf[ch][j] = static_cast<float>(x * a + shapeFl(x, g, unipolar, hq) * bs);
      }
    }
    for (int ch = 0; ch < 2; ++ch) {
      int len = m;
      for (int st = os - 1; st >= 0; --st) {
        WsHalfband& h = flDown_[st][ch];
        len /= 2;
        for (int i = 0; i < len; ++i) buf[ch][i] = h.down(buf[ch][2 * i], buf[ch][2 * i + 1]);
        h.flush();
      }
      float* out = io[ch];
      if (center) {
        const double c = static_cast<float>(2.2835e-5 * (44100.0 / ctx.sampleRate));
        double& dm = dcM_[ch];
        double& dv = dcV_[ch];
        for (int i = 0; i < n; ++i) {
          const double x = buf[ch][i];
          dv += (x - dm) * c;
          dm += dv;
          dv *= static_cast<double>(0.96f);
          out[i] = static_cast<float>(x - dm);
        }
        if (std::fabs(dm) <= 0x1p-24) dm = 0;
        if (std::fabs(dv) <= 0x1p-24) dv = 0;
      } else {
        std::copy(buf[ch], buf[ch] + n, out);
      }
    }
    peak_ = std::max(peak, peak_ * 0.8f);
  }
  std::vector<float> fl_;
  std::vector<int16_t> flTab_;
  int flN_ = 16384, flOs_ = -1;
  size_t flHash_ = 0;
  WsHalfband flUp_[4][2], flDown_[4][2];
  float sPre_ = 0, sWet_ = 0, sDry_ = 0;  // smoothed gains: start at zero (the reference never primes them)
  double dcM_[2] = {}, dcV_[2] = {};
};

class TunerFx final : public Effect {  // display only (pitch detection in the editor)
 public:
  void process(const PluginState&, const ProcessContext&, float*, float*, int) override {}
  void reset() override {}
};

}  // namespace

double waveshaperCurve(const PluginState& s, double x) {
  if (!wscurve::has(s.data.c_str())) return shapeCurve(s, x);
  // Editor-side evaluation: parse once per distinct curve text.
  static thread_local std::string key;
  static thread_local wscurve::Curve c;
  if (key != s.data) key = s.data, wscurve::parse(s.data.c_str(), c);
  bool uni = param(s, pp::WsBipolar, 1) >= 0.5f;
  x = std::clamp(x, -1.0, 1.0);
  if (uni) {
    double y = c.eval(std::fabs(x));
    return x < 0 ? -y : y;
  }
  return 2.0 * c.eval((x + 1.0) * 0.5) - 1.0;
}

std::unique_ptr<Effect> createEffectW2(const std::string& type) {
  if (type == "gl.geq") return std::make_unique<GeqFx>();
  if (type == "gl.waveshaper") return std::make_unique<WaveshaperFx>();
  if (type == "gl.tuner") return std::make_unique<TunerFx>();
  return nullptr;
}

}  // namespace gl

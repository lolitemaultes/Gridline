// Gridline — Wave 2 effects: Graphic EQ, Waveshaper, Auto Pan, Shaper,
// Time Gate, Maximizer, Freq Shifter, Low Lifter, Tuner.
#include <array>
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

class WaveshaperFx final : public Effect {
 public:
  WaveshaperFx() : tab_(32770, 0.f) {}
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const bool classic = param(s, WsClassic, 1) >= 0.5f;
    const bool unipolar = param(s, WsBipolar, 1) >= 0.5f;
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
  }
  int meters(float* out, int max) const override {  // input level after the pre amp (editor marker)
    if (max < 1) return 0;
    out[0] = peak_;
    return 1;
  }
  // Oversampling delays the wet path; the dry path is delayed to match.
  int latencyFor(const PluginState& s, double) const override { return Oversampler::latency(stages(s)); }

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

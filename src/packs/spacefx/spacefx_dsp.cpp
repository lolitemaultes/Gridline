// Gridline — "spacefx" pack: runtimes and prepared data.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include "../Packs.h"
#include "sfx_common.h"
#include "sfx_convolver.h"
#include "sfx_delays.h"
#include "sfx_hall.h"
#include "sfx_mod.h"
#include "spacefx.h"

namespace gl::packs::spacefx {

using namespace detail;

// ------------------------------------------------------------ shared helpers
double bandPointHz(int point) { return 20.0 * std::pow(1000.0, clampd(point, 0, kBdPoints - 1) / (kBdPoints - 1.0)); }

BandCurves defaultBandCurves() {
  BandCurves c{};
  for (int i = 0; i < kBdPoints; ++i) {
    double u = static_cast<double>(i) / (kBdPoints - 1);
    double wob = 0.3 * std::sin(u * 4 * kPi);
    c.v[0][BtDelay][i] = static_cast<float>(0.15 + 0.75 * u);
    c.v[0][BtFeedback][i] = 0.45f;
    c.v[0][BtPan][i] = static_cast<float>(0.5 + wob);
    c.v[0][BtLevel][i] = 1.f;
    c.v[1][BtDelay][i] = static_cast<float>(0.9 - 0.75 * u);
    c.v[1][BtFeedback][i] = static_cast<float>(0.35 + 0.3 * u);
    c.v[1][BtPan][i] = static_cast<float>(0.5 - wob);
    c.v[1][BtLevel][i] = static_cast<float>(1.0 - 0.35 * u);
  }
  return c;
}

BandCurves parseBandCurves(const std::string& data) {
  BandCurves c = defaultBandCurves();
  if (data.compare(0, 3, "bd1") != 0) return c;
  const char* p = data.c_str() + 3;
  float* out = &c.v[0][0][0];
  const int total = 2 * BtCount * kBdPoints;
  for (int i = 0; i < total; ++i) {
    char* end = nullptr;
    float v = std::strtof(p, &end);
    if (end == p) break;
    out[i] = std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.5f;
    p = end;
  }
  return c;
}

std::string formatBandCurves(const BandCurves& c) {
  std::string s = "bd1";
  const float* in = &c.v[0][0][0];
  char buf[24];
  for (int i = 0; i < 2 * BtCount * kBdPoints; ++i) {
    std::snprintf(buf, sizeof buf, " %.4g", static_cast<double>(in[i]));
    s += buf;
  }
  return s;
}

void hallLineLengths(double size01, int out[16]) { hallRefLengths(size01, out); }

double hallRt60(const PluginState& s, double hz) {
  if (pi(s, HvFreeze, 0) != 0) return 99.0;
  if (pi(s, HvModel, 1) == 1) {
    // Reference: mean line, two-band shelf (DC gain from Decay, Nyquist from Decay × Brightness).
    const double sr = 48000;
    int lens[16];
    hallRefLengths(pv(s, HvSize, 0.55f), lens);
    double d = 0;
    for (int l : lens) d += l / 44100.0;
    d /= 16.0;
    double T = clampd(pv(s, HvDecay, 2.6f), 0.25, 20.0), B = std::max(1e-3, static_cast<double>(pv(s, HvBright, 0.85f)));
    double glo = std::pow(10.0, -3.0 * d / T), ghi = std::pow(10.0, -3.0 * d / (B * T));
    double a = sr * 0.000149807;
    double b0 = (glo + a * ghi) / (1 + a), b1 = (glo - a * ghi) / (1 + a), a1 = (1 - a) / (1 + a);
    std::complex<double> z = std::polar(1.0, -kTwoPi * std::min(hz, 0.5 * sr) / sr);
    double g = std::abs((b0 + b1 * z) / (1.0 + a1 * z));
    if (g >= 0.999999) return 99.0;
    return -3.0 * d / std::log10(std::max(1e-9, g));
  }
  const double sr = 48000;
  double scale = hallSizeScale(pv(s, HvSize, 0.55f));
  double d = 0;
  for (double ms : kHallBaseMs) d += ms;
  d = d / kHallLines * 0.001 * scale;
  double rt = std::max(0.05, static_cast<double>(pv(s, HvDecay, 2.6f)));
  double gL = std::pow(10.0, -3.0 * d / (rt * pv(s, HvLowMult, 1.2f))), gM = std::pow(10.0, -3.0 * d / rt);
  double gH = std::pow(10.0, -3.0 * d / (rt * pv(s, HvHighMult, 0.55f)));
  double g = hallBandGain(sr, hz, gL, gM, gH, pv(s, HvLowXover, 250), pv(s, HvHighXover, 4500));
  if (g >= 0.999999) return 99.0;
  return -3.0 * d / std::log10(std::max(1e-9, g));
}

BbdModeSettings bbdSettings(const PluginState& s, double tempo) {
  if (pi(s, BcModel, 0) == 1) {  // reference model: fixed-mode table / custom controls
    int mode = std::clamp(pi(s, BcMode, 0), 0, 3);
    if (mode < 3) {
      flc::BcFixed f = flc::bcFixedMode(mode);
      return {f.t1, f.t2, f.rate, f.wave};
    }
    double rate = std::max(0.0, static_cast<double>(pv(s, BcRate, 0.5f)));
    if (pb(s, BcSync)) rate = std::max(1.0, tempo) / 60.0 / (std::max(1.0, static_cast<double>(pv(s, BcSyncTicks, 192))) / 192.0);
    return {pv(s, BcTime1, 1.7f), pv(s, BcTime2, 5.4f), rate, pi(s, BcWave, 1)};
  }
  switch (pi(s, BcMode, 0)) {
    case 0: return {1.66, 5.35, 0.513, 1};
    case 1: return {1.66, 5.35, 0.863, 1};
    case 2: return {3.2, 3.65, 9.75, 0};
    default: break;
  }
  double a = pv(s, BcTime1, 1.7f), b = pv(s, BcTime2, 5.4f);
  double rate = pv(s, BcRate, 0.5f);
  if (pb(s, BcSync)) rate = std::max(1.0, tempo) / 60.0 / kLfoSyncBeats[std::clamp(pi(s, BcSyncRate, 4), 0, 8)];
  return {std::min(a, b), std::max(a, b), rate, pi(s, BcWave, 1)};
}

double phaserLfo(int wave, double phase, double tension) {
  double fl = std::floor(phase), f = phase - fl;
  double v;
  switch (wave) {
    case 1: v = f < 0.5 ? 2 * f : 2 - 2 * f; break;
    case 2: v = 0.5 + 0.5 * std::tanh(6.0 * std::sin(kTwoPi * f)) / std::tanh(6.0); break;
    case 3: v = f; break;
    case 4: v = 1 - f; break;
    case 5: {
      auto h = [](double c) {
        uint32_t x = static_cast<uint32_t>(static_cast<int64_t>(c)) * 2654435761u;
        x ^= x >> 15;
        x *= 2246822519u;
        x ^= x >> 13;
        return (x & 0xFFFF) / 65535.0;
      };
      double t = f * f * (3 - 2 * f);
      v = h(fl) + (h(fl + 1) - h(fl)) * t;
      break;
    }
    default: v = 0.5 - 0.5 * std::cos(kTwoPi * f); break;
  }
  double e = std::pow(2.0, (0.5 - clampd(tension, 0, 1)) * 4.0);
  return std::pow(clampd(v, 0, 1), e);
}

void setConvolverRateHint(double sr) { g_convRate.store(sr, std::memory_order_relaxed); }
double convolverRateHint() {
  double r = g_convRate.load(std::memory_order_relaxed);
  return r > 0 ? r : 44100.0;
}

// ------------------------------------------------------------ convolver data
namespace {
std::vector<EnvPoint> parsePoints(const std::string& s) {
  std::vector<EnvPoint> out;
  const char* p = s.c_str();
  while (*p) {
    char* end = nullptr;
    EnvPoint q;
    double v[4] = {0, 0.5, 0, 0};
    int got = 0;
    for (int k = 0; k < 4; ++k) {
      v[k] = std::strtod(p, &end);
      if (end == p) break;
      ++got;
      p = end;
      if (*p == ',') ++p;
    }
    if (got >= 2 && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])) {
      q.t = std::max(0.0, v[0]);
      q.v = clampd(v[1], 0, 1);
      q.k = clampd(v[2], -1, 1);
      q.c = std::isfinite(v[3]) ? std::clamp(static_cast<int>(v[3]), 0, 12) : 0;
      out.push_back(q);
    }
    while (*p && *p != ';') ++p;
    if (*p == ';') ++p;
    if (got == 0 && !*p) break;
  }
  std::stable_sort(out.begin(), out.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.t < b.t; });
  return out;
}
void formatPoints(std::string& s, const std::vector<EnvPoint>& pts) {
  char buf[96];
  for (size_t i = 0; i < pts.size(); ++i) {
    if (pts[i].c != 0)
      std::snprintf(buf, sizeof buf, "%s%.10g,%.9g,%.7g,%d", i ? ";" : "", pts[i].t, pts[i].v, pts[i].k, pts[i].c);
    else
      std::snprintf(buf, sizeof buf, "%s%.10g,%.9g,%.7g", i ? ";" : "", pts[i].t, pts[i].v, pts[i].k);
    s += buf;
  }
}
}  // namespace

ConvData parseConvData(const std::string& data) {
  ConvData d;
  size_t pos = 0;
  bool first = true;
  while (pos <= data.size()) {
    size_t nl = data.find('\n', pos);
    std::string line = data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (first) {
      d.path = line;
      first = false;
    } else if (line.compare(0, 5, "#env ") == 0) {
      d.env = parsePoints(line.substr(5));
    } else if (line.compare(0, 4, "#eq ") == 0) {
      d.eq = parsePoints(line.substr(4));
    } else if (line.compare(0, 5, "#pan ") == 0) {
      d.pan = parsePoints(line.substr(5));
    } else if (line.compare(0, 5, "#sep ") == 0) {
      d.sep = parsePoints(line.substr(5));
    } else if (line.compare(0, 6, "#mode ") == 0) {  // J-106: "eq=smooth;env=straight"
      size_t p = 6;
      while (p < line.size()) {
        size_t e = line.find(';', p);
        if (e == std::string::npos) e = line.size();
        const std::string it = line.substr(p, e - p);
        const size_t eqp = it.find('=');
        if (eqp != std::string::npos) {
          const std::string k = it.substr(0, eqp), v = it.substr(eqp + 1);
          const int m = v == "smooth" ? CurveSmooth : v == "straight" ? CurveStraight : CurveAuto;
          if (k == "eq") d.eqMode = m;
          else if (k == "env") d.envMode = m;
        }
        p = e + 1;
      }
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return d;
}

std::string formatConvData(const ConvData& d) {
  std::string s = d.path;
  if (d.env.empty() && d.eq.empty() && d.pan.empty() && d.sep.empty() && d.envMode == CurveAuto && d.eqMode == CurveAuto) return s;
  s += "\n#v 1";
  if (d.envMode != CurveAuto || d.eqMode != CurveAuto) {
    const auto name = [](int m) { return m == CurveSmooth ? "smooth" : "straight"; };
    s += "\n#mode ";
    if (d.eqMode != CurveAuto) s += std::string("eq=") + name(d.eqMode);
    if (d.envMode != CurveAuto) s += std::string(d.eqMode != CurveAuto ? ";" : "") + "env=" + name(d.envMode);
  }
  if (!d.env.empty()) {
    s += "\n#env ";
    formatPoints(s, d.env);
  }
  if (!d.eq.empty()) {
    s += "\n#eq ";
    formatPoints(s, d.eq);
  }
  if (!d.pan.empty()) {
    s += "\n#pan ";
    formatPoints(s, d.pan);
  }
  if (!d.sep.empty()) {
    s += "\n#sep ";
    formatPoints(s, d.sep);
  }
  return s;
}

double envAt(const std::vector<EnvPoint>& pts, double t) {
  if (pts.empty()) return 0.5;
  if (t <= pts.front().t) return pts.front().v;
  if (t >= pts.back().t) return pts.back().v;
  size_t i = static_cast<size_t>(std::upper_bound(pts.begin(), pts.end(), t, [](double x, const EnvPoint& q) { return x < q.t; }) - pts.begin());
  const EnvPoint& a = pts[i - 1];
  const EnvPoint& b = pts[i];
  double span = b.t - a.t;
  double u = span > 1e-12 ? (t - a.t) / span : 1.0;
  // Tension bends the segment: > 0 slow start, < 0 fast start (tension of the segment's end point).
  double k = b.k;
  if (k > 0)
    u = std::pow(u, 1.0 + 5.0 * k);
  else if (k < 0)
    u = 1.0 - std::pow(1.0 - u, 1.0 - 5.0 * k);
  return a.v + (b.v - a.v) * u;
}

double convEqDb(const std::vector<EnvPoint>& eq, double hz, bool smooth) {
  if (eq.empty()) return 0;
  double x = std::log(std::max(hz, 1.0) / 20.0) / std::log(1000.0);
  const double v = smooth ? SmoothCurve(eq).at(clampd(x, 0, 1)) : envAt(eq, clampd(x, 0, 1));
  return (clampd(v, 0, 1) - 0.5) * 36.0;
}

// ------------------------------------------------- J-106: smooth curves
SmoothCurve::SmoothCurve(const std::vector<EnvPoint>& pts) {
  const size_t n = pts.size();
  x.resize(n), y.resize(n), m.assign(n, 0.0);
  for (size_t i = 0; i < n; ++i) x[i] = pts[i].t, y[i] = pts[i].v;
  if (n < 3) return;  // one segment: zero end slopes (a smoothstep between the two points)
  // Interior tangents: zero at a local extremum or a vertical step, else the
  // weighted harmonic mean of the neighbouring secant slopes (Fritsch–Butland),
  // which keeps every segment monotone (|m| <= 3 × each secant). Ends: zero slope.
  for (size_t i = 1; i + 1 < n; ++i) {
    const double h0 = x[i] - x[i - 1], h1 = x[i + 1] - x[i];
    if (!(h0 > 1e-12) || !(h1 > 1e-12)) continue;
    const double d0 = (y[i] - y[i - 1]) / h0, d1 = (y[i + 1] - y[i]) / h1;
    if (d0 * d1 <= 0) continue;
    const double w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
    m[i] = (w1 + w2) / (w1 / d0 + w2 / d1);
  }
}

double SmoothCurve::at(double t) const {
  const size_t n = x.size();
  if (n == 0) return 0.5;
  if (t <= x.front()) return y.front();
  if (t >= x.back()) return y.back();
  const size_t i = static_cast<size_t>(std::upper_bound(x.begin(), x.end(), t) - x.begin());  // x[i-1] <= t < x[i]
  const size_t a = i - 1;
  const double h = x[i] - x[a];
  if (!(h > 1e-12)) return y[i];
  const double s = (t - x[a]) / h, s2 = s * s, s3 = s2 * s;
  const double v = (2 * s3 - 3 * s2 + 1) * y[a] + (s3 - 2 * s2 + s) * h * m[a] + (3 * s2 - 2 * s3) * y[i] + (s3 - s2) * h * m[i];
  // Monotone by construction; the clamp only removes rounding outside the segment's range.
  return std::clamp(v, std::min(y[a], y[i]), std::max(y[a], y[i]));
}

ConvCurve::ConvCurve(const std::vector<EnvPoint>& pts, bool smooth) : p_(&pts), sm_(smooth), sc_(smooth ? pts : std::vector<EnvPoint>{}) {}

namespace {
double levelDb(double g) { return 20.0 * std::log10(std::max(1e-4, g)); }
}  // namespace
double convEqErrDb(double a, double b) {
  const auto ref = [](double v) { return levelDb((std::pow(3.0, 2.0 * clampd(v, 0, 1)) - 1.0) * 0.5); };  // reference build law
  return std::max(36.0 * std::fabs(a - b), std::fabs(ref(a) - ref(b)));                                      // editing chain: ±18 dB
}
double convEnvErrDb(double a, double b) { return std::fabs(levelDb(2.0 * clampd(a, 0, 1)) - levelDb(2.0 * clampd(b, 0, 1))); }

std::vector<EnvPoint> smoothToSegments(const std::vector<EnvPoint>& pts, double (*errDb)(double, double), double tolDb) {
  std::vector<EnvPoint> out;
  if (pts.size() < 2) {
    for (EnvPoint q : pts) out.push_back(EnvPoint{q.t, q.v, 0, 0});
    return out;
  }
  const SmoothCurve sc(pts);
  // Recursive bisection of each knot interval until the chord is within tolerance
  // (checked on 24 interior samples per chord; depth-limited).
  const std::function<void(double, double, int)> split = [&](double t0, double t1, int depth) {
    const double v0 = sc.at(t0), v1 = sc.at(t1);
    bool ok = true;
    for (int j = 1; j < 24 && ok; ++j) {
      const double f = j / 24.0, t = t0 + (t1 - t0) * f;
      ok = errDb(v0 + (v1 - v0) * f, sc.at(t)) <= tolDb;
    }
    if (!ok && depth < 16) {
      const double tm = 0.5 * (t0 + t1);
      split(t0, tm, depth + 1);
      out.push_back(EnvPoint{tm, sc.at(tm), 0, 0});
      split(tm, t1, depth + 1);
    }
  };
  out.push_back(EnvPoint{pts[0].t, pts[0].v, 0, 0});
  for (size_t i = 1; i < pts.size(); ++i) {
    if (pts[i].t - pts[i - 1].t > 1e-12) split(pts[i - 1].t, pts[i].t, 0);
    out.push_back(EnvPoint{pts[i].t, pts[i].v, 0, 0});
  }
  return out;
}

IrView convolverIr(const PluginState& s) {
  std::lock_guard<std::mutex> lk(irMutex());
  return irEntry(s)->view;
}

// ------------------------------------------------------------ pack entries
std::unique_ptr<Generator> createGenerator(const std::string& type) {
  (void)type;
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == "gl.convolver") return std::make_unique<ConvolverFx>();
  if (type == "gl.multidelay") return std::make_unique<MultiDelayFx>();
  if (type == "gl.banddelay") return std::make_unique<BandDelayFx>();
  if (type == "gl.hall") return std::make_unique<HallFx>();
  if (type == "gl.ensemble") return std::make_unique<EnsembleFx>();
  if (type == "gl.bbdchorus") return std::make_unique<BbdChorusFx>();
  if (type == "gl.swirl") return std::make_unique<SwirlFx>();
  if (type == "gl.vphaser") return std::make_unique<RetroPhaserFx>();
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.convolver") {
    std::lock_guard<std::mutex> lk(irMutex());
    IrCacheEntry* e = irEntry(state);
    aux.samples.clear();
    aux.samples.push_back(irKernel(*e, convolverRateHint()));
    return true;
  }
  if (state.type == "gl.banddelay") {
    BandCurves c = parseBandCurves(state.data);
    auto smp = std::make_shared<Sample>();
    smp->path = BandDelayFx::kCurveTag;
    const float* f = &c.v[0][0][0];
    smp->left.assign(f, f + sizeof(BandCurves) / sizeof(float));
    aux.samples.clear();
    aux.samples.push_back(smp);
    return true;
  }
  return false;
}

}  // namespace gl::packs::spacefx
static_assert(gl::packs::spacefx::SwModel == 15, "migration in normalizePluginState uses index 15");

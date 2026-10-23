// Gridline — J-106: Convolver smooth curves ("make smooth curve BETWEEN POINTS by
// default but you can disable it by clicking one button"). The curve modes
// (Smooth / Straight) of the IR EQ and IR envelope: the monotone cubic itself,
// defaults and compatibility (older states stay Straight and render the same),
// the data round trip, the rendered IR spectrum against the drawn curve in both
// IR builds, and the FL export of a Smooth curve as straight points (< 0.2 dB).
// The editor side (default Smooth, the toggle, one undo step, save + load) is
// tests/j105_conv_points.cmake.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "../flp_export/enc_common.h"
#include "core/PluginInfo.h"
#include "core/WavWriter.h"
#include "engine/Effects.h"
#include "packs/Packs.h"
#include "packs/spacefx/sfx_convolver.h"
#include "packs/spacefx/spacefx.h"

using namespace gl;
namespace sx = gl::packs::spacefx;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 48000.0;

const std::vector<sx::EnvPoint> kEq = {{0, 0.5, 0}, {0.18, 0.72, 0.4}, {0.42, 0.3, 0}, {0.63, 0.66, -0.3}, {0.8, 0.6, 0}, {1, 0.5, 0}};

std::string tmpDir() {
  fs::path d = fs::temp_directory_path() / "gridline-j106";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d.string();
}

// Stereo IR file: one impulse at `at` seconds (room for the zero-phase EQ's response on both sides).
std::string writeImpulse(double seconds, double at) {
  AudioBuffer b(2, static_cast<size_t>(seconds * kSr));
  const size_t i = static_cast<size_t>(at * kSr);
  b.data[i] = 0.5f;
  b.data[b.frames + i] = 0.5f;
  const std::string p = tmpDir() + "/impulse.wav";
  writeWav(p, b, static_cast<int>(kSr), 24);
  return p;
}

PluginState convState(const std::string& path, int model) {
  PluginState s = defaultPluginState("gl.convolver");
  auto set = [&](int k, float v) { s.params[static_cast<size_t>(k)] = v; };
  set(sx::CvSource, sx::SrcFile);
  set(sx::CvFadeOut, 0);
  set(sx::CvLength, 60);
  set(sx::CvNormalize, 0);
  set(sx::CvStereo, 0);
  set(sx::CvDry, 0);
  set(sx::CvWet, 1);
  set(sx::CvModel, static_cast<float>(model));
  s.data = path;
  return s;
}

// Left output of the effect for a unit impulse (what is heard).
std::vector<float> render(const PluginState& s, size_t n) {
  auto fx = packs::createEffect("gl.convolver");
  fx->prepareHost(s, kSr);
  ChannelAux aux;
  packs::prepareAux(s, aux);
  std::vector<float> L(n, 0.f), R(n, 0.f);
  L[0] = R[0] = 1;
  ProcessContext c;
  c.sampleRate = kSr;
  for (size_t i = 0; i < n; i += 256) {
    const int k = static_cast<int>(std::min<size_t>(256, n - i));
    fx->processAux(s, aux, c, L.data() + i, R.data() + i, k);
  }
  return L;
}

// Magnitude spectrum (N = 2^17 bins over the rate).
constexpr int kN = 1 << 17;
std::vector<double> magnitude(const std::vector<float>& x) {
  sx::detail::Fft f;
  f.init(kN);
  std::vector<float> a(2 * kN, 0.f);
  for (size_t i = 0; i < x.size() && i < static_cast<size_t>(kN); ++i) a[2 * i] = x[i];
  f.run(a.data(), false);
  std::vector<double> m(kN / 2 + 1);
  for (int k = 0; k <= kN / 2; ++k) m[static_cast<size_t>(k)] = std::hypot(a[2 * k], a[2 * k + 1]);
  return m;
}

// Measured EQ (output with the curve over output with EQ MIX 0), dB at hz.
double measuredDb(const std::vector<double>& on, const std::vector<double>& off, double hz) {
  const size_t k = static_cast<size_t>(std::lround(hz * kN / kSr));
  return 20.0 * std::log10(std::max(1e-12, on[k]) / std::max(1e-12, off[k]));
}

std::string withEq(const std::string& path, const std::vector<sx::EnvPoint>& eq, int mode) {
  sx::ConvData d;
  d.path = path;
  d.eq = eq;
  d.eqMode = mode;
  return sx::formatConvData(d);
}

void testCurve() {
  const sx::SmoothCurve sc(kEq);
  // Exactly through every point.
  bool through = true;
  for (const sx::EnvPoint& q : kEq) through = through && sc.at(q.t) == q.v;
  expect(through, "smooth: the curve passes exactly through every point");
  // No overshoot: each segment stays within its two points' range; no corners (slopes match at the knots).
  bool bounded = true, noCorner = true;
  for (size_t i = 1; i < kEq.size(); ++i) {
    const double lo = std::min(kEq[i - 1].v, kEq[i].v), hi = std::max(kEq[i - 1].v, kEq[i].v);
    for (int j = 0; j <= 400; ++j) {
      const double v = sc.at(kEq[i - 1].t + (kEq[i].t - kEq[i - 1].t) * j / 400.0);
      bounded = bounded && v >= lo - 1e-12 && v <= hi + 1e-12;
    }
  }
  const double e = 1e-6;
  for (size_t i = 1; i + 1 < kEq.size(); ++i) {
    const double t = kEq[i].t, l = (sc.at(t) - sc.at(t - e)) / e, r = (sc.at(t + e) - sc.at(t)) / e;
    noCorner = noCorner && std::fabs(l - r) < 1e-3 * std::max(1.0, std::fabs(l));
  }
  expect(bounded, "smooth: no overshoot beyond the neighbouring points' range");
  expect(noCorner, "smooth: no corners at the points (continuous slope)");
  // Flat ends: zero slope at the first / last point, held beyond them.
  const double s0 = (sc.at(e) - sc.at(0)) / e, s1 = (sc.at(1) - sc.at(1 - e)) / e;
  expect(std::fabs(s0) < 1e-3 && std::fabs(s1) < 1e-3 && sc.at(-1) == 0.5 && sc.at(2) == 0.5, "smooth: flat ends");
  // A monotone run stays monotone; two points give an S between them; one point is a constant.
  const std::vector<sx::EnvPoint> mono = {{0, 0.1, 0}, {0.3, 0.2, 0}, {0.35, 0.8, 0}, {1, 0.9, 0}};
  const sx::SmoothCurve sm(mono);
  bool inc = true;
  for (int j = 1; j <= 1000; ++j) inc = inc && sm.at(j / 1000.0) >= sm.at((j - 1) / 1000.0) - 1e-15;
  const sx::SmoothCurve two({{0.2, 0.2, 0}, {0.6, 0.8, 0}}), one({{0.5, 0.3, 0}});
  expect(inc && std::fabs(two.at(0.4) - 0.5) < 1e-12 && one.at(0.1) == 0.3 && one.at(0.9) == 0.3, "smooth: monotone data stays monotone, 2 / 1 point cases");
  // The engine evaluators are this curve: legacy ConvCurve, reference EnvEval, convEqDb.
  const sx::ConvCurve cc(kEq, true), cs(kEq, false);
  const sx::detail::ref::EnvEval ev(kEq, true);
  bool same = true, straightSame = true;
  for (int j = 0; j <= 200; ++j) {
    const double t = j / 200.0;
    same = same && cc.at(t) == sc.at(t) && ev.at(t) == sc.at(t);
    straightSame = straightSame && cs.at(t) == sx::envAt(kEq, t);
  }
  const double hz = 20.0 * std::pow(1000.0, 0.3);
  expect(same && straightSame && std::fabs(sx::convEqDb(kEq, hz, true) - (sc.at(0.3) - 0.5) * 36.0) < 1e-9,
         "smooth: one shared evaluator (editor drawing, legacy build, reference build)");
}

void testModes() {
  // New instance: no curve, no stored mode -> shown (and given to the first point) as Smooth.
  const PluginState fresh = defaultPluginState("gl.convolver");
  const sx::ConvData d0 = sx::parseConvData(fresh.data);
  expect(d0.eqMode == sx::CurveAuto && sx::curveModeShown(d0.eqMode, d0.eq) == sx::CurveSmooth &&
             sx::curveModeShown(d0.envMode, d0.env) == sx::CurveSmooth,
         "modes: a new instance's curves are Smooth");
  // Older states (points, no mode): Straight; their data round-trips byte for byte.
  const std::string old = "/x/ir.wav\n#v 1\n#env 0,0.5,0;1.5,0.25,0.2\n#eq 0,0.75,0;0.5,0.4,-0.3;1,0.25,0";
  const sx::ConvData od = sx::parseConvData(old);
  expect(od.eqMode == sx::CurveAuto && od.envMode == sx::CurveAuto && !sx::curveSmooth(od.eqMode) &&
             sx::curveModeShown(od.eqMode, od.eq) == sx::CurveStraight && sx::formatConvData(od) == old,
         "modes: older curves load as Straight, data unchanged");
  // Save / load keeps the modes (both, either, and on an empty curve).
  sx::ConvData m = od;
  m.eqMode = sx::CurveSmooth, m.envMode = sx::CurveStraight;
  sx::ConvData r = sx::parseConvData(sx::formatConvData(m));
  sx::ConvData e;
  e.path = "/p";
  e.envMode = sx::CurveStraight;
  sx::ConvData re = sx::parseConvData(sx::formatConvData(e));
  m.envMode = sx::CurveAuto;
  sx::ConvData r2 = sx::parseConvData(sx::formatConvData(m));
  expect(r.eqMode == sx::CurveSmooth && r.envMode == sx::CurveStraight && r.eq.size() == 3 && re.envMode == sx::CurveStraight &&
             re.env.empty() && sx::curveModeShown(re.envMode, re.env) == sx::CurveStraight && r2.eqMode == sx::CurveSmooth &&
             r2.envMode == sx::CurveAuto,
         "modes: save + load keeps each curve's mode");
  // Straight renders exactly like a state without a mode; Smooth renders differently.
  sx::setConvolverRateHint(kSr);
  for (int model : {0, 1}) {
    const std::string p = writeImpulse(1.0, 0.5);
    PluginState a = convState(p, model), b = a, c = a;
    a.data = withEq(p, kEq, sx::CurveAuto);
    b.data = withEq(p, kEq, sx::CurveStraight);
    c.data = withEq(p, kEq, sx::CurveSmooth);
    const std::vector<float> ya = render(a, 60000), yb = render(b, 60000), yc = render(c, 60000);
    double dab = 0, dac = 0;
    for (size_t i = 0; i < ya.size(); ++i) dab = std::max(dab, std::fabs(static_cast<double>(ya[i]) - yb[i])), dac = std::max(dac, std::fabs(static_cast<double>(ya[i]) - yc[i]));
    char nm[160];
    std::snprintf(nm, sizeof nm, "modes (%s build): no mode = Straight renders identically (max diff %.1g); Smooth differs (%.2g)",
                  model ? "reference" : "editing", dab, dac);
    expect(dab == 0 && dac > 1e-4, nm);
  }
}

// The rendered IR spectrum follows the drawn Smooth curve.
void testSpectrum() {
  sx::setConvolverRateHint(kSr);
  const std::string p = writeImpulse(1.0, 0.5);
  for (int model : {0, 1}) {
    PluginState on = convState(p, model), off = on;
    on.data = withEq(p, kEq, sx::CurveSmooth);
    off.data = on.data;
    off.params[sx::CvEqMix] = 0;
    const std::vector<double> mOn = magnitude(render(on, 100000)), mOff = magnitude(render(off, 100000));
    const sx::detail::ref::EnvEval ev(kEq, true);
    double worst = 0, worstHz = 0;
    // Editing build: the curve's own law, 30 Hz .. 18 kHz. Reference build: its Gaussian spectral
    // smoothing (~23 Hz wide) blurs the log axis' low end, so from 400 Hz.
    const double f0 = model ? 400.0 : 30.0;
    for (double hz = f0; hz <= 18000; hz *= 1.03) {
      const double drawn = model ? 20.0 * std::log10(std::max(1e-6, static_cast<double>(sx::detail::ref::eqGain(ev.at(sx::detail::ref::eqX(hz))))))
                                 : sx::convEqDb(kEq, hz, true);
      const double err = std::fabs(measuredDb(mOn, mOff, hz) - drawn);
      if (err > worst) worst = err, worstHz = hz;
    }
    char nm[160];
    std::snprintf(nm, sizeof nm, "spectrum (%s build): rendered EQ matches the drawn smooth curve (worst %.3f dB at %.0f Hz)",
                  model ? "reference" : "editing", worst, worstHz);
    expect(worst < (model ? 0.3 : 0.1), nm);
  }
}

// FL export: Smooth curves go out as straight points within 0.2 dB.
void testExport() {
  // The approximation itself, measured on a dense grid with both straight laws FL / Gridline use.
  const std::vector<sx::EnvPoint> eq = {{0, 0.5, 0}, {0.1, 0.05, 0}, {0.3, 0.95, 0}, {0.55, 0.2, 0}, {0.7, 0.62, 0}, {1, 0.4, 0}};
  const std::vector<sx::EnvPoint> env = {{0, 0.0, 0}, {0.05, 1.0, 0}, {0.6, 0.3, 0}, {2.5, 0.02, 0}, {4.0, 0.5, 0}};
  struct Case {
    const char* name;
    const std::vector<sx::EnvPoint>* pts;
    double (*err)(double, double);
  } cases[] = {{"EQ", &eq, sx::convEqErrDb}, {"envelope", &env, sx::convEnvErrDb}};
  for (const Case& c : cases) {
    const std::vector<sx::EnvPoint> seg = sx::smoothToSegments(*c.pts, c.err, 0.1);
    const sx::SmoothCurve sc(*c.pts);
    const sx::detail::ref::EnvEval fl(seg);
    double worst = 0;
    bool keeps = true;
    for (const sx::EnvPoint& q : *c.pts)
      keeps = keeps && std::any_of(seg.begin(), seg.end(), [&](const sx::EnvPoint& s) { return s.t == q.t && s.v == q.v; });
    const double span = c.pts->back().t;
    for (int j = 0; j <= 20000; ++j) {
      const double t = span * j / 20000.0, want = sc.at(t);
      worst = std::max({worst, c.err(sx::envAt(seg, t), want), c.err(fl.at(t), want)});
    }
    char nm[160];
    std::snprintf(nm, sizeof nm, "export (%s): %zu points -> %zu straight points, worst error %.3f dB (< 0.2), originals kept", c.name,
                  c.pts->size(), seg.size(), worst);
    expect(worst < 0.2 && keeps && seg.size() < 400, nm);
  }
  // Through the Fruity Convolver encoder: the decoded curve follows the Smooth one.
  namespace enc = gl::flp::enc;
  const enc::Encoder* e = enc::findEncoder("Fruity Convolver", "gl.convolver");
  gl::flp::PluginMapping m;
  if (e && e->defaults && enc::decodeState("Fruity Convolver", e->defaults(), m, enctest::ctx())) {
    PluginState g = m.state;
    sx::ConvData cd = sx::parseConvData(g.data);
    cd.eq = eq, cd.eqMode = sx::CurveSmooth;
    cd.env = env, cd.envMode = sx::CurveSmooth;
    g.data = sx::formatConvData(cd);
    g.params[sx::CvEnvOn] = 1;
    enc::Result r = enc::encode(g, "Fruity Convolver", nullptr, enctest::ctx());
    gl::flp::PluginMapping back;
    const bool ok = r.hasState && enc::decodeState("Fruity Convolver", r.state, back, enctest::ctx());
    const sx::ConvData bd = sx::parseConvData(back.state.data);
    double worst = 1e9;
    if (ok && bd.eq.size() > eq.size() && bd.env.size() > env.size()) {
      worst = 0;
      const sx::detail::ref::EnvEval fe(bd.eq), fv(bd.env);
      const sx::SmoothCurve se(eq), sv(env);
      for (int j = 0; j <= 4000; ++j) {
        const double x = j / 4000.0, t = 4.0 * j / 4000.0;
        worst = std::max({worst, sx::convEqErrDb(fe.at(x), se.at(x)), sx::convEnvErrDb(fv.at(t), sv.at(t))});
      }
    }
    char nm[160];
    std::snprintf(nm, sizeof nm, "export: FL Convolver state decodes to the smooth EQ / envelope within 0.2 dB (worst %.3f dB)", worst);
    expect(ok && worst < 0.2, nm);
    // A Straight (or no-mode) curve is written as is.
    cd.eqMode = sx::CurveAuto, cd.envMode = sx::CurveStraight;
    g.data = sx::formatConvData(cd);
    r = enc::encode(g, "Fruity Convolver", nullptr, enctest::ctx());
    gl::flp::PluginMapping b2;
    const bool ok2 = r.hasState && enc::decodeState("Fruity Convolver", r.state, b2, enctest::ctx());
    const sx::ConvData d2 = sx::parseConvData(b2.state.data);
    expect(ok2 && d2.eq.size() == eq.size() && d2.env.size() == env.size(), "export: Straight curves keep their own points");
  } else {
    expect(false, "export: Fruity Convolver encoder defaults decode");
  }
}

}  // namespace

int main() {
  testCurve();
  testModes();
  testSpectrum();
  testExport();
  return finish();
}

// Gridline — "dynamics" pack tests: catalogue, silence, bounds, block-size
// invariance, crossover flatness, drawn-curve accuracy, limiter ceiling.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/PluginInfo.h"
#include "engine/Effects.h"
#include "packs/dynamics/dynamics.h"

using namespace gl;
using namespace gl::packs::dynamics;

namespace {

const char* kTypes[] = {"gl.master", "gl.emphasis", "gl.enhance", "gl.amprack", "gl.overdrive",
                        "gl.fastdist", "gl.squeeze", "gl.effector", "gl.transplit"};
const double kSr = 48000.0;

struct Buf {
  std::vector<float> L, R;
  explicit Buf(size_t n = 0) : L(n, 0.f), R(n, 0.f) {}
  size_t size() const { return L.size(); }
};

// Runs the effect over the whole buffer in blocks of `block` frames.
Buf run(Effect& fx, const PluginState& st, Buf b, int block, bool playing = false) {
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = 120;
  ctx.playing = playing;
  for (size_t i = 0; i < b.size(); i += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), b.size() - i));
    ctx.tick = static_cast<double>(i) / kSr * (120.0 / 60.0) * ctx.ppq;
    fx.process(st, ctx, b.L.data() + i, b.R.data() + i, n);
  }
  return b;
}
Buf run(const PluginState& st, const Buf& b, int block = 256) {
  auto fx = createEffect(st.type);
  return run(*fx, st, b, block);
}

Buf sine(double hz, double amp, double seconds, double phaseR = 0.0) {
  Buf b(static_cast<size_t>(seconds * kSr));
  for (size_t i = 0; i < b.size(); ++i) {
    double t = static_cast<double>(i) / kSr;
    b.L[i] = static_cast<float>(amp * std::sin(2 * M_PI * hz * t));
    b.R[i] = static_cast<float>(amp * std::sin(2 * M_PI * hz * t + phaseR));
  }
  return b;
}
Buf noise(double amp, double seconds, uint32_t seed = 1) {
  Buf b(static_cast<size_t>(seconds * kSr));
  uint32_t s = seed;
  auto rnd = [&]() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<double>(static_cast<int32_t>(s)) / 2147483648.0;
  };
  for (size_t i = 0; i < b.size(); ++i) {
    b.L[i] = static_cast<float>(amp * rnd());
    b.R[i] = static_cast<float>(amp * rnd());
  }
  return b;
}
// Drum-like bursts: decaying noise hits every 250 ms.
Buf hits(double amp, double seconds) {
  Buf b = noise(1.0, seconds, 7);
  for (size_t i = 0; i < b.size(); ++i) {
    double t = std::fmod(static_cast<double>(i) / kSr, 0.25);
    double e = amp * std::exp(-t * 18.0);
    b.L[i] = static_cast<float>(b.L[i] * e);
    b.R[i] = static_cast<float>(b.R[i] * e);
  }
  return b;
}

double rms(const Buf& b, size_t from, size_t to) {
  double a = 0;
  size_t n = 0;
  for (size_t i = from; i < to && i < b.size(); ++i, ++n) a += b.L[i] * b.L[i] + b.R[i] * b.R[i];
  return n ? std::sqrt(a / (2.0 * static_cast<double>(n))) : 0.0;
}
double peak(const Buf& b, size_t from = 0, size_t to = ~size_t(0)) {
  double m = 0;
  for (size_t i = from; i < to && i < b.size(); ++i) m = std::max(m, static_cast<double>(std::max(std::fabs(b.L[i]), std::fabs(b.R[i]))));
  return m;
}
bool allFinite(const Buf& b) {
  for (size_t i = 0; i < b.size(); ++i)
    if (!std::isfinite(b.L[i]) || !std::isfinite(b.R[i])) return false;
  return true;
}
double maxDiff(const Buf& a, const Buf& b) {
  double m = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    m = std::max(m, static_cast<double>(std::max(std::fabs(a.L[i] - b.L[i]), std::fabs(a.R[i] - b.R[i]))));
  return m;
}
double db(double g) { return 20.0 * std::log10(std::max(g, 1e-12)); }

void set(PluginState& s, int k, double v) { s.params[static_cast<size_t>(k)] = static_cast<float>(v); }

// Master state with every band transparent.
PluginState neutralMaster() {
  PluginState s = defaultPluginState("gl.master");
  for (int b = 0; b < kMsBands; ++b) {
    set(s, msParam(b, MbRatioHi), 1);
    set(s, msParam(b, MbRatioLo), 1);
    set(s, msParam(b, MbGate), -90);
    set(s, msParam(b, MbSatThresh), 0);
    set(s, msParam(b, MbSatCeil), 12);
  }
  set(s, MsOversample, 0);
  set(s, MsLookahead, 0);
  return s;
}

}  // namespace

int main() {
  // ---------------------------------------------------------------- catalogue
  for (const char* ty : kTypes) {
    const PluginInfo* info = findPlugin(ty);
    expect(info != nullptr, (std::string("catalogue has ") + ty).c_str());
    if (!info) continue;
    expect(info->kind == PluginKind::Effect, "is an effect");
    expect(info->presets.size() >= 2 && info->presets.size() <= 6, (std::string(ty) + " has 2-6 presets").c_str());
    expect(info->editorWidth > 0, "editor width set");
    bool rangesOk = true;
    for (const auto& d : info->params) rangesOk = rangesOk && d.min <= d.def && d.def <= d.max && !d.name.empty();
    expect(rangesOk, (std::string(ty) + " defaults inside ranges").c_str());
    auto fx = createEffect(ty);
    expect(fx != nullptr, (std::string("creates ") + ty).c_str());
  }
  expect(static_cast<int>(findPlugin("gl.master")->params.size()) == kMsCount, "master param count");
  expect(static_cast<int>(findPlugin("gl.amprack")->params.size()) == kRkCount, "rack param count");

  // ---------------------------------------------------------------- silence, finiteness, block size
  for (const char* ty : kTypes) {
    const PluginInfo* info = findPlugin(ty);
    std::vector<PluginState> states = {defaultPluginState(ty)};
    for (size_t pr = 0; pr < info->presets.size(); ++pr) states.push_back(presetState(*info, pr));
    bool silent = true, finite = true, invariant = true;
    for (const auto& st : states) {
      Buf z(static_cast<size_t>(kSr * 0.5));
      Buf o = run(st, z, 256);
      silent = silent && peak(o) < 1e-6;
      Buf in = noise(0.5, 0.4, 3);
      Buf a = run(st, in, 64), b = run(st, in, 1000), c = run(st, in, 333);
      finite = finite && allFinite(a);
      invariant = invariant && maxDiff(a, b) < 1e-5 && maxDiff(a, c) < 1e-5;
    }
    expect(silent, (std::string(ty) + ": silence in, silence out (all presets)").c_str());
    expect(finite, (std::string(ty) + ": finite output (all presets)").c_str());
    expect(invariant, (std::string(ty) + ": block-size invariant (all presets)").c_str());

    // Parameter extremes with a hot input stay finite and bounded.
    bool ok = true;
    for (int mode = 0; mode < 3; ++mode) {
      PluginState st = defaultPluginState(ty);
      for (size_t k = 0; k < info->params.size(); ++k) {
        const ParamDesc& d = info->params[k];
        double v = mode == 0 ? d.min : mode == 1 ? d.max : d.fromNormalized(std::fmod(k * 0.37, 1.0));
        st.params[k] = static_cast<float>(d.clamp(v));
      }
      Buf o = run(st, noise(2.0, 0.3, 11), 128);
      ok = ok && allFinite(o) && peak(o) < 64.0;
    }
    expect(ok, (std::string(ty) + ": extremes finite and bounded").c_str());

    // Short (old) parameter vectors: normalised and also safe raw.
    PluginState shortSt = defaultPluginState(ty);
    shortSt.params.resize(shortSt.params.size() / 2);
    Buf raw = run(shortSt, noise(0.3, 0.1), 256);
    normalizePluginState(shortSt);
    expect(shortSt.params.size() == info->params.size() && allFinite(raw), (std::string(ty) + ": short state handled").c_str());
  }

  // ---------------------------------------------------------------- master: crossover flatness
  for (int slope = 0; slope < 3; ++slope) {
    PluginState st = neutralMaster();
    set(st, MsLowSlope, slope);
    set(st, MsHighSlope, slope);
    double worst = 0;
    for (double hz : {40.0, 160.0, 500.0, 1000.0, 3500.0, 9000.0}) {
      Buf in = sine(hz, 0.25, 0.6);
      Buf o = run(st, in, 256);
      size_t a = static_cast<size_t>(kSr * 0.2), e = in.size();
      worst = std::max(worst, std::fabs(db(rms(o, a, e) / rms(in, a, e))));
    }
    char name[96];
    std::snprintf(name, sizeof name, "master: neutral bands sum flat (slope %d, worst %.3f dB)", slope, worst);
    expect(worst < 0.1, name);
  }
  {
    // Mixed slopes too.
    PluginState st = neutralMaster();
    set(st, MsLowSlope, 0);
    set(st, MsHighSlope, 2);
    Buf in = noise(0.2, 0.5, 5);
    Buf o = run(st, in, 256);
    double d = db(rms(o, 4800, o.size()) / rms(in, 4800, in.size()));
    expect(std::fabs(d) < 0.25, "master: mixed slopes keep broadband level");
  }

  // ---------------------------------------------------------------- master: drawn curve
  {
    PluginState st = neutralMaster();
    for (int b = 0; b < 3; ++b) set(st, msParam(b, MbMode), 0);  // bands bypassed
    set(st, msParam(3, MbThreshHi), -20);
    set(st, msParam(3, MbRatioHi), 4);
    set(st, msParam(3, MbKnee), 6);
    set(st, msParam(3, MbAttack), 1);
    set(st, msParam(3, MbRelease), 80);
    bool ok = true;
    for (double inDb : {-40.0, -22.0, -17.0, -10.0, -4.0}) {
      Buf in = sine(1000, std::pow(10.0, inDb / 20.0), 0.8);
      Buf o = run(st, in, 256);
      double outDb = db(peak(o, o.size() / 2));
      double want = masterCurve(st, 3, inDb);
      char name[128];
      std::snprintf(name, sizeof name, "master: output follows drawn curve at %.0f dB (got %.2f, curve %.2f)", inDb, outDb, want);
      expect(std::fabs(outDb - want) < 0.6, name);
      ok = ok && std::isfinite(outDb);
    }
    // Gain-reduction meters report the master band.
    auto fx = createEffect("gl.master");
    run(*fx, st, sine(1000, 0.5, 0.5), 256);
    float m[4];
    int k = fx->meters(m, 4);
    expect(k == 4 && fx->meter() < -3.0f && fx->meter() == m[3], "master: meters expose gain reduction");
    expect(fx->inputLevel() > -8.0f && fx->inputLevel() < -4.0f, "master: input level reported");
  }
  {
    // Upward compression lifts quiet material by the drawn amount (mid band).
    PluginState st = neutralMaster();
    set(st, msParam(0, MbMode), 2);  // mute low
    set(st, msParam(2, MbMode), 2);  // mute high
    set(st, msParam(1, MbThreshLo), -40);
    set(st, msParam(1, MbRatioLo), 0.5);
    set(st, msParam(1, MbKnee), 0);
    set(st, msParam(1, MbRelease), 50);
    Buf in = sine(1000, std::pow(10.0, -60 / 20.0), 1.0);
    Buf o = run(st, in, 256);
    double outDb = db(peak(o, o.size() / 2));
    double want = masterCurve(st, 1, -60);
    char name[128];
    std::snprintf(name, sizeof name, "master: below-threshold lift follows the curve (got %.2f, curve %.2f)", outDb, want);
    expect(std::fabs(outDb - want) < 0.75 && want > -55, name);
  }
  {
    // Gate closes on quiet input.
    PluginState st = neutralMaster();
    set(st, msParam(3, MbGate), -40);
    Buf o = run(st, sine(1000, 0.003, 0.6), 256);
    expect(peak(o, o.size() / 2) < 0.0005, "master: gate closes below its threshold");
    expect(masterCurveParams(-20, 4, 0, -60, 1, -90, -8) == -17.0, "master: curve math (above)");
  }
  {
    // Solo routes a single band.
    PluginState st = neutralMaster();
    set(st, MsSolo, 1);  // low
    Buf o = run(st, sine(8000, 0.25, 0.4), 256);
    expect(rms(o, 4800, o.size()) < 0.25 * 0.02, "master: soloing low removes highs");
  }

  // ---------------------------------------------------------------- loudness: ceiling
  {
    bool ok = true;
    double worst = -100;
    for (int ch = 0; ch < 4; ++ch)
      for (int os = 0; os < 3; ++os)
        for (int msMode = 0; msMode < 2; ++msMode) {
          PluginState st = defaultPluginState("gl.emphasis");
          set(st, EmInput, 18);
          set(st, EmCeiling, -1);
          set(st, EmCharacter, ch);
          set(st, EmOversample, os);
          set(st, EmStereoMode, msMode);
          set(st, EmLowEmph, 4);
          set(st, EmHighEmph, -3);
          Buf in = noise(0.8, 0.4, 9);
          Buf in2 = sine(60, 0.9, 0.4, 1.0);
          for (Buf* b : {&in, &in2}) {
            Buf o = run(st, *b, 512);
            double pk = peak(o);
            worst = std::max(worst, db(pk));
            ok = ok && pk <= std::pow(10.0, -1.0 / 20.0) + 1e-6 && allFinite(o);
          }
        }
    char name[96];
    std::snprintf(name, sizeof name, "loudness: ceiling respected (worst %.3f dBFS)", worst);
    expect(ok, name);
    // Quiet material passes almost unchanged.
    PluginState st = defaultPluginState("gl.emphasis");
    Buf in = sine(440, 0.1, 0.5);
    Buf o = run(st, in, 256);
    expect(std::fabs(db(rms(o, 4800, o.size()) / rms(in, 4800, in.size()))) < 0.1, "loudness: transparent below the ceiling");
    // The limiter actually limits (gain reduction meter).
    auto fx = createEffect("gl.emphasis");
    PluginState hot = defaultPluginState("gl.emphasis");
    set(hot, EmInput, 12);
    run(*fx, hot, sine(200, 0.9, 0.3), 256);
    expect(fx->meter() < -6.0f, "loudness: gain reduction reported");
    // Delta outputs what was removed: silence when nothing is limited.
    set(st, EmDelta, 1);
    Buf d = run(st, sine(440, 0.05, 0.4), 256);
    expect(peak(d, 4800) < 0.002, "loudness: delta is silent when nothing is limited");
  }

  // ---------------------------------------------------------------- enhance
  {
    PluginState st = defaultPluginState("gl.enhance");
    set(st, EnAmount, 0);
    Buf in = sine(700, 0.2, 0.5);
    Buf o = run(st, in, 256);
    // κ2: the amount is the voicing's band mix (reference behaviour): at 0 the bands
    // pass dry and only the master stage acts; more amount = more band processing.
    PluginState full = st;
    set(full, EnAmount, 1);
    Buf of = run(full, in, 256);
    expect(std::fabs(db(rms(o, 9600, o.size()) / rms(in, 9600, in.size()))) < 6.0 && db(rms(of, 9600, of.size()) / rms(o, 9600, o.size())) > 1.0,
           "enhance: amount blends the band processing in");
    bool louder = true;
    for (int mode = 0; mode < 4; ++mode) {
      set(st, EnMode, mode);
      set(st, EnAmount, 1);
      Buf q = noise(0.05, 0.8, 4);
      Buf e = run(st, q, 256);
      double gain = rms(e, 19200, e.size()) / rms(q, 19200, q.size());
      std::printf("# enhance mode %d gain %.2f dB peak %.3f\n", mode, db(gain), peak(e));
      louder = louder && gain > 1.2 && peak(e) < 1.05;
    }
    expect(louder, "enhance: every voicing raises loudness and stays under 0 dBFS");
  }

  // ---------------------------------------------------------------- distortions bounded
  {
    PluginState st = defaultPluginState("gl.overdrive");
    set(st, OdPreAmp, 1);
    set(st, OdBoost, 1);
    set(st, OdPostGain, 0);
    Buf o = run(st, noise(4.0, 0.3), 256);
    expect(allFinite(o) && peak(o) < 4.0, "overdrive: bounded at maximum drive");
    expect(std::fabs(overdriveCurve(0, 0.8)) < 1e-12 && std::fabs(overdriveCurve(100, 0.5)) <= 2.5, "overdrive: curve bounded and centred");

    PluginState fd = defaultPluginState("gl.fastdist");
    set(fd, FdClassic, 0);
    set(fd, FdPreAmp, 24);
    set(fd, FdThreshold, 0.4);
    bool ok = true;
    for (int type = 0; type < 2; ++type) {
      set(fd, FdType, type);
      Buf f = run(fd, sine(220, 1.0, 0.3), 256);
      ok = ok && peak(f) < 0.4 * 1.2 && allFinite(f);
      for (double x = -10; x <= 10; x += 0.01) ok = ok && std::fabs(fastDistCurve(x, 0.4, type)) <= 0.4 + 1e-9;
    }
    expect(ok, "clip dist: output stays under the threshold (clip and fold)");

    PluginState sq = defaultPluginState("gl.squeeze");
    set(sq, SqModel, 0);  // the legacy engine's sections (J-26 reference: tests/test_j26.cpp)
    set(sq, SqSquare, 1);
    set(sq, SqSteps, 4);
    set(sq, SqPunchAmount, 1);
    set(sq, SqRingAmount, 1);
    set(sq, SqRateDiv, 16);
    set(sq, SqFilterOn, 1);
    set(sq, SqResonance, 1);
    Buf s = run(sq, noise(1.0, 0.3), 256);
    expect(allFinite(s) && peak(s) < 16, "squeeze: everything on stays bounded");
    // Each section changes the sound.
    Buf ref = sine(300, 0.4, 0.2);
    bool changes = true;
    for (int k : {SqSquare, SqPunchAmount, SqRingAmount}) {
      PluginState t = defaultPluginState("gl.squeeze");
      set(t, SqModel, 0);
      set(t, k, 1);
      changes = changes && maxDiff(run(t, ref), ref) > 0.01;
    }
    PluginState t = defaultPluginState("gl.squeeze");
    set(t, SqModel, 0);
    set(t, SqRateDiv, 8);
    changes = changes && maxDiff(run(t, ref), ref) > 0.01;
    expect(changes, "squeeze: squarize, puncher, ring and rate sections are audible");
    expect(maxDiff(run(defaultPluginState("gl.squeeze"), ref), ref) < 1e-6, "squeeze: default is transparent");
  }

  // ---------------------------------------------------------------- amp rack
  {
    bool ok = true, audible = true;
    for (int type = 1; type < RmCount; ++type) {
      PluginState st = defaultPluginState("gl.amprack");
      for (int s = 0; s < kRackSlots; ++s) set(st, rkParam(s, RsOn), 0);
      set(st, rkParam(0, RsType), type);
      set(st, rkParam(0, RsOn), 1);
      for (int k = 0; k < kRackKnobs; ++k) set(st, rkParam(0, RsK0 + k), rackKnobDefault(type, k));
      if (type == RmEq) set(st, rkParam(0, RsK0 + 2), 1.0);
      if (type == RmCompressor) set(st, rkParam(0, RsK0), 0.3);
      set(st, RkSoftClip, 0);
      Buf in = sine(330, type == RmGate ? 0.001 : 0.3, 0.5);
      Buf o = run(st, in, 256);
      ok = ok && allFinite(o) && peak(o) < 16;
      bool aud = maxDiff(o, in) > 0.02 * peak(in);
      if (!aud) std::printf("# module %s inaudible\n", rackModuleName(type));
      audible = audible && aud;
      expect(rackKnobLabel(type, 0) != nullptr, (std::string("rack: module ") + rackModuleName(type) + " has labels").c_str());
    }
    expect(ok, "rack: every module finite and bounded");
    expect(audible, "rack: every module changes the sound");
    // Swapping two slots changes the order (and therefore the output).
    PluginState a = defaultPluginState("gl.amprack");
    PluginState b = a;
    for (int k = 0; k < RsStride; ++k) std::swap(b.params[static_cast<size_t>(rkParam(0, k))], b.params[static_cast<size_t>(rkParam(1, k))]);
    Buf in = sine(220, 0.4, 0.3);
    expect(maxDiff(run(a, in), run(b, in)) > 0.01, "rack: module order matters");
    // Empty / disabled rack is transparent.
    PluginState e = defaultPluginState("gl.amprack");
    for (int s = 0; s < kRackSlots; ++s) set(e, rkParam(s, RsOn), 0);
    set(e, RkSoftClip, 0);
    expect(maxDiff(run(e, in), in) < 1e-6, "rack: bypassed slots are transparent");
  }

  // ---------------------------------------------------------------- multi fx
  {
    bool ok = true, audible = true;
    for (int kind = 0; kind < kEfKinds; ++kind) {
      PluginState st = defaultPluginState("gl.effector");
      set(st, EfModel, 0);
      set(st, EfKind, kind);
      set(st, EfX, 0.7);
      set(st, EfY, 0.3);
      set(st, EfModX, 0.5);
      Buf in = sine(440, 0.3, 0.6);
      Buf o = run(st, in, 256);
      ok = ok && allFinite(o) && peak(o) < 16;
      bool aud = maxDiff(o, in) > 0.01;
      if (!aud) std::printf("# kind %s inaudible\n", effectorKindName(kind));
      audible = audible && aud;
    }
    expect(ok, "multi fx: all twelve effects finite and bounded");
    expect(audible, "multi fx: all twelve effects audible");
    PluginState st = defaultPluginState("gl.effector");
    set(st, EfModel, 0);  // legacy engine (J-26 reference: tests/test_j26.cpp)
    set(st, EfBypass, 1);
    Buf in = sine(440, 0.3, 0.2);
    expect(maxDiff(run(st, in), in) < 1e-6, "multi fx: bypass is transparent");
    // Modulation moves the X/Y point.
    set(st, EfBypass, 0);
    set(st, EfModX, 1);
    set(st, EfModRate, 0);
    auto fx = createEffect("gl.effector");
    float lo = 1, hi = 0;
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    Buf blk = sine(440, 0.1, 0.6);
    for (size_t i = 0; i + 256 <= blk.size(); i += 256) {
      fx->process(st, ctx, blk.L.data() + i, blk.R.data() + i, 256);
      float m[2];
      fx->meters(m, 2);
      lo = std::min(lo, m[0]);
      hi = std::max(hi, m[0]);
    }
    expect(hi - lo > 0.6f, "multi fx: modulation sweeps X");
    // Playing transport keeps the LFO aligned regardless of block size.
    Buf n1 = noise(0.3, 0.4);
    set(st, EfKind, 4);
    auto f1 = createEffect("gl.effector");
    auto f2 = createEffect("gl.effector");
    expect(maxDiff(run(*f1, st, n1, 64, true), run(*f2, st, n1, 1000, true)) < 1e-4, "multi fx: transport-synced LFO block invariant");
  }

  // ---------------------------------------------------------------- transient split
  {
    Buf in = hits(0.5, 1.0);
    PluginState up = defaultPluginState("gl.transplit");
    set(up, TsAttack, 1);
    PluginState down = up;
    set(down, TsAttack, -1);
    Buf a = run(up, in), b = run(down, in);
    double crestIn = peak(in) / rms(in, 0, in.size());
    double crestUp = peak(a) / rms(a, 0, a.size()), crestDown = peak(b) / rms(b, 0, b.size());
    std::printf("# crest factor in %.2f, attack boost %.2f, attack cut %.2f\n", crestIn, crestUp, crestDown);
    expect(crestUp > crestIn * 1.1, "transient split: attack boost raises the crest factor");
    expect(crestDown < crestIn * 0.95, "transient split: attack cut lowers the crest factor");
    PluginState sus = defaultPluginState("gl.transplit");
    set(sus, TsAttack, 0);
    set(sus, TsRelease, 1);
    Buf c = run(sus, in);
    expect(rms(c, 0, c.size()) > rms(in, 0, in.size()) * 1.1, "transient split: sustain boost raises the tails");
    PluginState flat = defaultPluginState("gl.transplit");
    set(flat, TsAttack, 0);
    set(flat, TsSplitFreq, 1000);
    Buf f = run(flat, sine(3000, 0.3, 0.3));
    expect(std::fabs(db(rms(f, 4800, f.size()) / 0.3 * std::sqrt(2.0))) < 0.1, "transient split: neutral split is flat");

    // Neutral levels with no effect: the two parts sum back to the input.
    PluginState unity = defaultPluginState("gl.transplit");
    set(unity, TsAttack, 0);
    Buf u = run(unity, in);
    expect(maxDiff(u, in) < 1e-5, "transient split: transient + sustain parts sum to the input");
    // Each part can be switched off; the transient part alone is spikier.
    PluginState tOnly = unity, sOnly = unity;
    set(tOnly, TsSusOn, 0);
    set(sOnly, TsTransOn, 0);
    Buf to = run(tOnly, in), so = run(sOnly, in);
    expect(maxDiff(run(unity, Buf(in.size())), Buf(in.size())) == 0.0, "transient split: silence in, silence out");
    expect(peak(to) / rms(to, 0, to.size()) > peak(so) / rms(so, 0, so.size()) * 1.2, "transient split: transient part is the spiky one");
    Buf sum(in.size());
    for (size_t i = 0; i < in.size(); ++i) sum.L[i] = to.L[i] + so.L[i], sum.R[i] = to.R[i] + so.R[i];
    expect(maxDiff(sum, in) < 1e-4, "transient split: parts are complementary");
    // Every effect slot: finite, block-size invariant, audibly different from off.
    for (int side = 0; side < 2; ++side)
      for (int fx = 0; fx < TsFxOff; ++fx) {
        PluginState e = unity;
        set(e, TsTransFx + side * kTsSlotStride, fx);
        set(e, TsTransFxMix + side * kTsSlotStride, 1.0);
        set(e, TsTransFxA + side * kTsSlotStride, 0.7);
        set(e, TsTransFxB + side * kTsSlotStride, 0.3);
        auto f1 = createEffect("gl.transplit");
        auto f2 = createEffect("gl.transplit");
        Buf a1 = run(*f1, e, in, 64), a2 = run(*f2, e, in, 1000);
        char name[96];
        std::snprintf(name, sizeof name, "transient split: %s slot effect %d finite, block invariant, audible", side ? "sustain" : "transient", fx);
        expect(allFinite(a1) && maxDiff(a1, a2) < 1e-5 && maxDiff(a1, in) > 1e-3 && peak(a1) < 4.0, name);
      }
    // Echo on the sustain part: energy arrives after the input stops.
    {
      PluginState e = unity;
      set(e, TsSusFx, TsFxEcho);
      set(e, TsSusFxMix, 1.0);
      set(e, TsSusFxA, 0.3);
      set(e, TsSusSync, 1);  // tempo-synced: B = 0 → one tick; B = 95/767 → 96 ticks = one beat
      set(e, TsSusFxB, 95.0 / 767.0);
      Buf tone = sine(220, 0.3, 0.2);
      Buf padded(static_cast<size_t>(kSr * 1.0));
      for (size_t i = 0; i < tone.size(); ++i) padded.L[i] = tone.L[i], padded.R[i] = tone.R[i];
      Buf o = run(e, padded);
      const size_t beat = static_cast<size_t>(kSr * 0.5);  // 120 BPM
      expect(rms(o, beat + 100, beat + 9000) > 0.05 && rms(o, static_cast<size_t>(kSr * 0.25), beat - 100) < 0.01,
             "transient split: synced echo repeats one beat later");
    }
    // Tilt: positive tilt brightens the transient part.
    {
      PluginState tl = unity;
      set(tl, TsSusOn, 0);
      PluginState br = tl;
      set(br, TsTransTilt, 1.0);
      Buf a1 = run(tl, hits(0.5, 0.5)), a2 = run(br, hits(0.5, 0.5));
      expect(rms(a2, 0, a2.size()) > rms(a1, 0, a1.size()), "transient split: tilt changes the transient part's balance");
    }
    // Look-ahead reports its latency; off by default.
    {
      auto fx = createEffect("gl.transplit");
      PluginState la = unity;
      expect(fx->latencyFor(la, 48000) == 0, "transient split: no latency by default");
      set(la, TsLookahead, 1);
      expect(fx->latencyFor(la, 48000) == 72, "transient split: look-ahead latency 1.5 ms");
      Buf imp(4800);
      imp.L[100] = imp.R[100] = 0.5f;
      Buf o = run(*fx, la, imp, 256);
      size_t at = 0;
      for (size_t i = 0; i < o.size(); ++i)
        if (std::fabs(o.L[i]) > std::fabs(o.L[at])) at = i;
      expect(at == 172, "transient split: look-ahead delays the audio by its reported latency");
    }
  }

  return finish();
}

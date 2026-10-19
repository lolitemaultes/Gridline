// J-1 space pack, part 2: BBD chorus (Vintage Chorus), vintage phaser
// (Vintage Phaser) and multi delay (Fruity Delay Bank) reference models:
// control curves, mode behaviours, legacy states, decoders and FL presets.
#include "../../src/core/flp/FlpFx2.h"
#include "../../src/packs/spacefx/sfx_flcurves.h"
#include "../../src/packs/spacefx/spacefx.h"

namespace {

namespace s2 = gl::packs::spacefx;
namespace fc = gl::packs::spacefx::flc;

struct S2Blob {
  std::vector<uint8_t> b;
  S2Blob& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
};

struct S2Env {
  flp::File file;
  Song song;
  flp::Report report;
  flp::Context ctx{file, song, report};
};

bool s2Decode(const std::string& name, const std::vector<uint8_t>& st, flp::PluginMapping& m) {
  S2Env e;
  return flp::fx2::decodeEmbedded(e.ctx, name, st.data(), st.size(), m);
}
float s2P(const PluginState& s, int k) { return s.params[static_cast<size_t>(k)]; }
void s2Set(PluginState& s, int k, double v) { s.params[static_cast<size_t>(k)] = static_cast<float>(v); }

struct S2Out {
  std::vector<float> l, r;
};
S2Out s2Run(const PluginState& s, const std::vector<float>& inL, const std::vector<float>& inR, int block = 256, double sr = jt::kSr) {
  auto fx = createEffect(s.type);
  S2Out o{inL, inR};
  jt::run(*fx, s, o.l, o.r, block, sr);
  return o;
}
S2Out s2Run(const PluginState& s, const std::vector<float>& in, int block = 256) { return s2Run(s, in, in, block); }
double s2MaxDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0) {
  double d = 0;
  for (size_t i = from; i < a.size() && i < b.size(); ++i) d = std::max(d, std::fabs(static_cast<double>(a[i]) - b[i]));
  return d;
}
std::vector<float> s2Impulse(size_t n, size_t at = 0, float v = 1.f) {
  std::vector<float> x(n, 0.f);
  if (at < n) x[at] = v;
  return x;
}
std::vector<float> s2Noise(size_t n, uint32_t seed) {
  std::vector<float> x(n);
  for (auto& v : x) {
    seed = seed * 1664525u + 1013904223u;
    v = static_cast<float>(static_cast<int32_t>(seed) / 2147483648.0 * 0.4);
  }
  return x;
}
// Peak |x| in [a, b).
double s2Peak(const std::vector<float>& x, size_t a, size_t b) {
  double p = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) p = std::max(p, std::fabs(static_cast<double>(x[i])));
  return p;
}
// Every parameter at its minimum / maximum: the effect stays finite.
bool s2Extremes(const char* type) {
  const PluginInfo* info = findPlugin(type);
  bool ok = true;
  for (int side = 0; side < 2; ++side) {
    PluginState s = defaultPluginState(type);
    for (size_t i = 0; i < s.params.size(); ++i) s.params[i] = static_cast<float>(side ? info->params[i].max : info->params[i].min);
    for (size_t i = 0; i < s.params.size(); ++i)
      if (info->params[i].choices.size() == 2 && info->params[i].choices[0] == "Legacy") s.params[i] = 1;  // reference model
    S2Out o = s2Run(s, s2Noise(24000, 7 + static_cast<uint32_t>(side)), 256);
    ok = ok && jt::finite(o.l) && jt::finite(o.r) && s2Peak(o.l, 0, o.l.size()) < 1e5;
  }
  return ok;
}
// Block-size invariance of the reference model (64 vs 1000 frames per block).
bool s2BlockInvariant(const PluginState& s, double tol = 1e-6) {
  std::vector<float> x = s2Noise(30000, 99);
  S2Out a = s2Run(s, x, 64), b = s2Run(s, x, 1000);
  return s2MaxDiff(a.l, b.l) <= tol && s2MaxDiff(a.r, b.r) <= tol;
}
// Pre-J-1 state (fewer parameters) normalises to the legacy model.
bool s2OldIsLegacy(const char* type, int count, int modelParam) {
  PluginState s = defaultPluginState(type);
  s.params.resize(static_cast<size_t>(count));
  normalizePluginState(s);
  return s.params.size() == findPlugin(type)->params.size() && s2P(s, modelParam) == 0 && s2P(defaultPluginState(type), modelParam) == 1;
}
// FL preset folder: decoded / exact counts (only when FL is installed).
void s2Presets(const char* folder, const char* type, int& total, int& ok, int& exact) {
  total = ok = exact = 0;
  fs::path dir = fs::path(jt::kFlFx) / folder;
  if (!fs::exists(dir)) return;
  for (const auto& f : fs::directory_iterator(dir)) {
    if (f.path().extension() != ".fst") continue;
    ++total;
    flp::PresetResult r;
    if (flp::importFlPreset(f.path().string(), r) && r.kind == flp::PresetResult::Plugin && r.state.type == type) {
      ++ok;
      if (r.exact) ++exact;
    }
  }
}

// ================================================================ BBD chorus
void testBbdChorusRef() {
  // Curves (FL engine laws).
  expect(jt::near(fc::bcTimeMs(0), 0.2, 1e-12) && jt::near(fc::bcTimeMs(1), 10.0, 1e-12) && jt::near(fc::bcTimeMs(0.5), 5.1, 1e-12),
         "bbd: time = 9.8n + 0.2 ms");
  expect(fc::bcSpeedHz(0) == 0 && jt::near(fc::bcSpeedHz(0.5), 2.5, 1e-12) && jt::near(fc::bcSpeedHz(1), 10, 1e-12), "bbd: speed = 10n² Hz");
  expect(jt::near(fc::bcHighPassHz(0), 10, 1e-12) && jt::near(fc::bcHighPassHz(1), 170, 1e-12) && jt::near(fc::bcFeedback(1), 0.63, 1e-12),
         "bbd: high pass 10..170 Hz, feedback 0.63n");
  expect(fc::bcSyncTicks(0) == 6144 && fc::bcSyncTicks(1) == 1, "bbd: synced period 1..6144 ticks");
  expect(fc::bcHissLevel(0) == 0 && jt::near(20 * std::log10(fc::bcHissLevel(1)), -40, 1e-9) &&
             jt::near(20 * std::log10(fc::bcHissLevel(0.2)), -80, 1e-9),
         "bbd: hiss −90..−40 dB, 0 = off");
  expect(jt::near(fc::refTri(0), 0.5, 1e-12) && jt::near(fc::refTri(0.25), 1, 1e-12) && jt::near(fc::refTri(0.75), 0, 1e-12) &&
             jt::near(fc::refSine(0.25), 1, 1e-12),
         "bbd: triangle starts at 0.5 rising, sine 0.5 + 0.5 sin");
  // Fixed modes.
  PluginState st = defaultPluginState("gl.bbdchorus");
  expect(s2P(st, s2::BcModel) == 1, "bbd: new instances use the reference model");
  s2::BbdModeSettings m1 = s2::bbdSettings(st, 120);
  s2Set(st, s2::BcMode, 1);
  s2::BbdModeSettings m2 = s2::bbdSettings(st, 120);
  s2Set(st, s2::BcMode, 2);
  s2::BbdModeSettings m12 = s2::bbdSettings(st, 120);
  expect(jt::near(m1.rateHz, 0.4, 1e-12) && jt::near(m2.rateHz, 0.6, 1e-12) && m1.wave == 1 && jt::near(m1.minMs, 1.08, 1e-12) &&
             jt::near(m1.maxMs, 5.23, 1e-12),
         "bbd: modes I / II = 0.4 / 0.6 Hz triangle, 1.08–5.23 ms");
  expect(jt::near(m12.rateHz, 8, 1e-12) && m12.wave == 0 && jt::near(m12.minMs, 2.85, 1e-12) && jt::near(m12.maxMs, 3.37, 1e-12),
         "bbd: mode I+II = 8 Hz sine, 2.85–3.37 ms");
  // Frozen LFO (speed 0): a steady sine stays periodic; a moving LFO does not.
  PluginState cu = defaultPluginState("gl.bbdchorus");
  s2Set(cu, s2::BcMode, 3);
  s2Set(cu, s2::BcHiss, 0);
  s2Set(cu, s2::BcRate, 0);
  s2Set(cu, s2::BcTime1, 2);
  s2Set(cu, s2::BcTime2, 8);
  s2Set(cu, s2::BcStartPhase, 0.3);
  std::vector<float> sn = jt::sine(480, 48000, 0.5);  // period 100 samples
  S2Out fr = s2Run(cu, sn);
  double dFrozen = 0, dMoving = 0;
  for (size_t i = 24000; i + 100 < sn.size(); ++i) dFrozen = std::max(dFrozen, std::fabs(static_cast<double>(fr.l[i]) - fr.l[i + 100]));
  s2Set(cu, s2::BcRate, 2.5);
  S2Out mv = s2Run(cu, sn);
  for (size_t i = 24000; i + 100 < sn.size(); ++i) dMoving = std::max(dMoving, std::fabs(static_cast<double>(mv.l[i]) - mv.l[i + 100]));
  expect(dFrozen < 1e-4 && dMoving > 0.02, "bbd: speed 0 freezes the LFO (static delay)");
  // Equal-power mix: mix 0 = dry only (× gain), mix 1 = wet only.
  PluginState mx = cu;
  s2Set(mx, s2::BcMix, 0);
  s2Set(mx, s2::BcGain, 6);
  S2Out dryOnly = s2Run(mx, sn);
  expect(jt::near(s2Peak(dryOnly.l, 24000, 48000), 0.5 * std::pow(10.0, 6 / 20.0), 2e-3), "bbd: mix 0 = dry × output gain (dB on dry + wet)");
  // Static delay = time 1 when frozen at phase 0 (sine 0.5 + 0.5 sin(0) = 0.5 → midpoint of time 1..time 2).
  PluginState dl = cu;
  s2Set(dl, s2::BcRate, 0);
  s2Set(dl, s2::BcStartPhase, 0.75);  // sine minimum → time 1
  s2Set(dl, s2::BcWave, 0);
  s2Set(dl, s2::BcMix, 1);
  s2Set(dl, s2::BcHighPass, 10);
  s2Set(dl, s2::BcTime1, 5);
  S2Out imp = s2Run(dl, s2Impulse(4800, 100));
  size_t at = 0;
  double pk = 0;
  for (size_t i = 0; i < imp.l.size(); ++i)
    if (std::fabs(imp.l[i]) > pk) pk = std::fabs(imp.l[i]), at = i;
  expect(at >= 100 + 238 && at <= 100 + 246, "bbd: line delay = time 1 at the LFO minimum (5 ms + band-limit)");
  // Mode I+II modulates faster than mode I.
  PluginState a = defaultPluginState("gl.bbdchorus"), b = a;
  s2Set(a, s2::BcHiss, 0);
  s2Set(b, s2::BcHiss, 0);
  s2Set(b, s2::BcMode, 2);
  S2Out oa = s2Run(a, sn), ob = s2Run(b, sn);
  expect(s2MaxDiff(oa.l, ob.l, 4800) > 0.01, "bbd: I+II differs from I");
  // Output gate: silence + hiss → gate closes; without gate the hiss stays.
  PluginState g = defaultPluginState("gl.bbdchorus");
  s2Set(g, s2::BcHiss, 1);
  s2Set(g, s2::BcNoiseGate, 0);
  std::vector<float> sil(48000, 0.f);
  S2Out og = s2Run(g, sil);
  s2Set(g, s2::BcNoiseGate, 1);
  S2Out og2 = s2Run(g, sil);
  expect(jt::rms(og.l, 24000) > 1e-4 && jt::rms(og2.l, 24000) < 0.05 * jt::rms(og.l, 24000), "bbd: noise gate silences hiss when the output is quiet");
  // Legacy states, block-size invariance, extremes.
  expect(s2OldIsLegacy("gl.bbdchorus", 17, s2::BcModel), "bbd: pre-J-1 state loads as legacy");
  PluginState lg = defaultPluginState("gl.bbdchorus");
  s2Set(lg, s2::BcModel, 0);
  S2Out l1 = s2Run(lg, sn), l2 = s2Run(lg, sn);
  expect(s2MaxDiff(l1.l, l2.l) == 0 && s2MaxDiff(l1.l, s2Run(defaultPluginState("gl.bbdchorus"), sn).l, 4800) > 1e-3,
         "bbd: legacy model is deterministic and differs from the reference");
  PluginState bi = cu;
  s2Set(bi, s2::BcFeedback, 0.5);
  s2Set(bi, s2::BcHiss, 0.6);
  expect(s2BlockInvariant(bi), "bbd: block-size invariant");
  expect(s2Extremes("gl.bbdchorus"), "bbd: extremes stay finite");
  // Decoder: custom mode, sine LFO, curves; mode / link.
  {
    S2Blob s;
    s.i32(1);
    for (int v : {2, 0, 122881, 1, 3000, 600, 6000, 1500, 1, 1, 6000, 6000, 3000, 0, 1, 1, 1500}) s.i32(v);
    flp::PluginMapping m;
    bool ok = s2Decode("Vintage Chorus", s.b, m);
    const PluginState& p = m.state;
    expect(ok && m.exact && s2P(p, s2::BcModel) == 1 && s2P(p, s2::BcMode) == 3 && s2P(p, s2::BcWave) == 0, "bbd decode: custom + sine, exact");
    expect(jt::near(s2P(p, s2::BcRate), 2.5, 1e-4) && jt::near(s2P(p, s2::BcTime1), 1.18, 1e-5) && jt::near(s2P(p, s2::BcTime2), 10, 1e-5) &&
               jt::near(s2P(p, s2::BcFeedback), 0.63, 1e-6) && jt::near(s2P(p, s2::BcHighPass), 170, 1e-4) && jt::near(s2P(p, s2::BcGain), 6, 1e-5) &&
               s2P(p, s2::BcHiss) == 0 && s2P(p, s2::BcInvertWet) == 1 && s2P(p, s2::BcSync) == 1 && jt::near(s2P(p, s2::BcStartPhase), 0.25, 1e-6),
           "bbd decode: speed / times / feedback / HP / gain / switches");
    S2Blob t;
    t.i32(1);
    for (int v : {0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) t.i32(v);
    flp::PluginMapping m2;
    expect(s2Decode("Vintage Chorus", t.b, m2) && s2P(m2.state, s2::BcMode) == 2 && s2P(m2.state, s2::BcRate) == 0 &&
               s2P(m2.state, s2::BcWave) == 1,
           "bbd decode: mode I + link = I+II, speed 0 kept, triangle");
  }
  if (jt::haveFl()) {
    int total, ok, exact;
    s2Presets("Vintage Chorus", "gl.bbdchorus", total, ok, exact);
    char nm[96];
    std::snprintf(nm, sizeof nm, "bbd: FL presets decode exactly (%d/%d, exact %d)", ok, total, exact);
    expect(total > 0 && ok == total && exact == total, nm);
  }
}

// ============================================================ Vintage phaser
void testPhaserRef() {
  expect(jt::near(fc::vpFreqHz(0), 1.25, 1e-12) && jt::near(fc::vpFreqHz(1), 20480, 1e-6) && jt::near(fc::vpFreqHz(0.5), 160, 1e-9),
         "phaser: sweep 1.25·2^(14n) Hz");
  expect(jt::near(fc::vpRateRangeHz(0), 0.025, 1e-12) && jt::near(fc::vpRateRangeHz(1), 150, 1e-9), "phaser: rate range n⁴·149.975 + 0.025");
  expect(fc::vpSyncTicks(0) == 6144 && fc::vpSyncTicks(1) == 1, "phaser: synced period 1..6144 ticks");
  expect(fc::vpTensionK(0) == 0 && jt::near(fc::vpBend(0.3, 0), 0.3, 1e-12) && fc::vpBend(0.5, fc::vpTensionK(0.5)) > 0.7 &&
             jt::near(fc::vpBend(1, 5), 1, 1e-12) && jt::near(fc::vpBend(0, 5), 0, 1e-12),
         "phaser: tension bend keeps the ends, bends the middle");
  expect(jt::near(fc::vpEchoLpHz(0), 320, 1e-9) && jt::near(fc::vpEchoLpHz(1), 20480, 1e-6) && jt::near(fc::vpEchoHpHz(0), 20, 1e-9) &&
             jt::near(fc::vpEchoHpHz(1), 20480, 1e-6) && jt::near(fc::vpHpHz(0), 5, 1e-12) && jt::near(fc::vpHpHz(1), 200, 1e-9),
         "phaser: echo LP / HP and input / feedback HP curves");
  expect(jt::near(fc::vpSweepPos(0.7, 0.3, 1, 0), 0.7, 1e-12) && jt::near(fc::vpSweepPos(1, 1, 0, 0), 1, 1e-12) &&
             jt::near(fc::vpSweepPos(0, 1, 0, 0), 1, 1e-12) && fc::vpSweepPos(1, 1, 0, 1) < 0.5,
         "phaser: depth shrinks toward the top with speed (scale), pull-down lowers it");
  PluginState st = defaultPluginState("gl.vphaser");
  expect(s2P(st, s2::VpModel) == 1, "phaser: new instances use the reference model");
  // Manual notch in both engines, at 4× and 8×.
  auto notchDepth = [](PluginState s) {
    double lo = 1e9, hi = 0;
    for (double f = 100; f < 10000; f *= 1.05) {
      std::vector<float> x = jt::sine(f, 9600, 0.25);
      S2Out o = s2Run(s, x, 512);
      double r = jt::rms(o.l, 4800);
      lo = std::min(lo, r), hi = std::max(hi, r);
    }
    return lo / hi;
  };
  PluginState mn = st;
  s2Set(mn, s2::VpManual, 1);
  s2Set(mn, s2::VpManualFreq, 1000);
  s2Set(mn, s2::VpFeedback, 0);
  s2Set(mn, s2::VpMix, 0.5);
  s2Set(mn, s2::VpEngine, 0);
  double dc = notchDepth(mn);
  s2Set(mn, s2::VpEngine, 1);
  double da = notchDepth(mn);
  s2Set(mn, s2::VpHq, 1);
  double dh = notchDepth(mn);
  char nm[128];
  std::snprintf(nm, sizeof nm, "phaser: classic / analog / HQ carve notches (%.3f %.3f %.3f)", dc, da, dh);
  expect(dc < 0.1 && da < 0.1 && dh < 0.1, nm);
  // Engine switch: analog saturates (hot input differs from classic beyond the linear range).
  PluginState en = st;
  s2Set(en, s2::VpFeedback, 0.8);
  s2Set(en, s2::VpDrive, 15);
  s2Set(en, s2::VpEngine, 0);
  std::vector<float> hot = jt::sine(220, 24000, 0.9);
  S2Out c0 = s2Run(en, hot);
  s2Set(en, s2::VpEngine, 1);
  S2Out c1 = s2Run(en, hot);
  expect(jt::finite(c1.l) && s2MaxDiff(c0.l, c1.l, 4800) > 0.05 && s2Peak(c1.l, 4800, 24000) < 3.0, "phaser: analog engine differs and stays bounded");
  // HQ: 8× vs 4× give nearly the same level on a mid tone.
  PluginState hq = st;
  S2Out h4 = s2Run(hq, jt::sine(500, 24000, 0.3));
  s2Set(hq, s2::VpHq, 1);
  S2Out h8 = s2Run(hq, jt::sine(500, 24000, 0.3));
  expect(std::fabs(jt::rms(h4.l, 4800) - jt::rms(h8.l, 4800)) < 0.02, "phaser: HQ (8×) matches 4× level");
  // Echo: impulse returns after the echo time; keep-pitch crossfades to a new time.
  PluginState ec = st;
  s2Set(ec, s2::VpMix, 0);
  s2Set(ec, s2::VpDelayOn, 1);
  s2Set(ec, s2::VpDelaySync, 0);
  s2Set(ec, s2::VpDelayTime, 100);
  s2Set(ec, s2::VpDelayMix, 1);
  s2Set(ec, s2::VpDelayFb, 0);
  s2Set(ec, s2::VpDelayLp, 20480);
  s2Set(ec, s2::VpDelayHp, 20);
  s2Set(ec, s2::VpDelayStereo, 1);
  S2Out e1 = s2Run(ec, s2Impulse(24000, 100));
  expect(s2Peak(e1.l, 100 + 4790, 100 + 4860) > 0.3 && s2Peak(e1.l, 300, 4800) < 0.02, "phaser: echo at 1000n ms (100 ms, + oversampling latency)");
  {
    auto fx = createEffect("gl.vphaser");
    PluginState k = ec;
    s2Set(k, s2::VpDelayKeep, 1);
    std::vector<float> l = s2Impulse(48000, 100), r = l;
    ProcessContext ctx;
    ctx.sampleRate = jt::kSr;
    fx->process(k, ctx, l.data(), r.data(), 2400);
    s2Set(k, s2::VpDelayTime, 200);
    for (size_t i = 2400; i < l.size(); i += 240) fx->process(k, ctx, l.data() + i, r.data() + i, 240);
    // Second impulse 600 ms in: the echo now comes 200 ms later.
    std::vector<float> l2(48000, 0.f), r2;
    l2[28800] = 1;
    r2 = l2;
    for (size_t i = 0; i < l2.size(); i += 240) fx->process(k, ctx, l2.data() + i, r2.data() + i, 240);
    expect(jt::finite(l) && s2Peak(l2, 28800 + 9590, 28800 + 9660) > 0.3 && s2Peak(l2, 28800 + 4790, 28800 + 4860) < 0.05,
           "phaser: keep-pitch moves the echo to the new time");
  }
  // Legacy, block-size invariance, extremes.
  expect(s2OldIsLegacy("gl.vphaser", 32, s2::VpModel), "phaser: pre-J-1 state loads as legacy");
  PluginState bi = st;
  s2Set(bi, s2::VpDelayOn, 1);
  s2Set(bi, s2::VpNoise, 0.7);
  s2Set(bi, s2::VpDelayKeep, 1);
  expect(s2BlockInvariant(bi), "phaser: block-size invariant");
  expect(s2Extremes("gl.vphaser"), "phaser: extremes stay finite");
  // Decoder: version 3 + version 1 (remapped), clamping of out-of-range values.
  {
    S2Blob s;
    s.i32(3);
    int t[36] = {125, 2048000, 3000, 0, 125, 1, 122881, 0, 1, 0, 1500, 3000, 1, 1, 6000, 3000, 384, 0, 3000, 204800, 2000, 50, 1, 0, 1,
                 6000, 650000, 80000, 2000, 25, 150000, 6000, 0, 1, 327500, 1};
    for (int v : t) s.i32(v);
    s.b.push_back(1);
    flp::PluginMapping m;
    bool ok = s2Decode("Vintage Phaser", s.b, m);
    const PluginState& p = m.state;
    expect(ok && m.exact && s2P(p, s2::VpModel) == 1 && s2P(p, s2::VpEngine) == 1 && s2P(p, s2::VpHq) == 1 && s2P(p, s2::VpInvertFb) == 1,
           "phaser decode v3: model, engine, HQ, inverted feedback");
    expect(jt::near(s2P(p, s2::VpFreqMin), 1.25, 1e-5) && jt::near(s2P(p, s2::VpFreqMax), 20480, 0.01) && jt::near(s2P(p, s2::VpFeedback), 0.45, 1e-6) &&
               s2P(p, s2::VpWave) == 0 && jt::near(s2P(p, s2::VpRate), 0.025 * std::pow(6000.0, 0.5), 1e-3),
           "phaser decode v3: sweep, feedback, sine, speed in the rate range");
    expect(jt::near(s2P(p, s2::VpDelayTime), 1000.0 * 383 / 767, 1e-3) && s2P(p, s2::VpDelayTicks) == 384 && s2P(p, s2::VpDelayKeep) == 1 &&
               jt::near(s2P(p, s2::VpDelayLp), 20480, 0.01) && jt::near(s2P(p, s2::VpMix), 0.5, 1e-6) && jt::near(s2P(p, s2::VpColorGain), 30, 1e-4) &&
               jt::near(s2P(p, s2::VpColorFreq), 650, 1e-3) && jt::near(s2P(p, s2::VpInputHp), 200, 1e-3) && jt::near(s2P(p, s2::VpDrive), 0, 1e-4),
           "phaser decode v3: echo, mix, colour, HP, drive curves");
    // Out-of-range raw values clamp like FL (n clamped to 0..1).
    S2Blob o;
    o.i32(3);
    for (int i = 0; i < 36; ++i) o.i32(i == 21 ? 6000 : (i == 27 ? 15715264 : (i == 0 ? 0 : 0)));
    o.b.push_back(0);
    flp::PluginMapping mo;
    expect(s2Decode("Vintage Phaser", o.b, mo) && jt::near(s2P(mo.state, s2::VpMix), 1, 1e-6) && jt::near(s2P(mo.state, s2::VpInputHp), 200, 1e-3) &&
               jt::near(s2P(mo.state, s2::VpFreqMin), 1.25, 1e-6),
           "phaser decode: out-of-range values clamp");
    S2Blob v1;
    v1.i32(1);
    for (int i = 0; i < 32; ++i) v1.i32(i == 7 ? 4000 : (i == 31 ? 6000 : (i == 10 ? 100 : 0)));  // file 7 → tag 11, 31 → tag 9, 10 → tag 21
    flp::PluginMapping m1;
    expect(s2Decode("Vintage Phaser", v1.b, m1) && jt::near(s2P(m1.state, s2::VpPhaseLR), 4000.0 / 6000, 1e-6) &&
               jt::near(s2P(m1.state, s2::VpTension), 1, 1e-6) && jt::near(s2P(m1.state, s2::VpMix), 1, 1e-6) && s2P(m1.state, s2::VpEngine) == 0 &&
               s2P(m1.state, s2::VpNoiseGate) == 1,
           "phaser decode v1: remapped order, classic engine, gate default");
  }
  if (jt::haveFl()) {
    int total, ok, exact;
    s2Presets("Vintage Phaser", "gl.vphaser", total, ok, exact);
    std::snprintf(nm, sizeof nm, "phaser: FL presets decode exactly (%d/%d, exact %d)", ok, total, exact);
    expect(total > 0 && ok == total && exact == total, nm);
  }
}

// ============================================================== Delay bank
// One enabled unit, everything neutral: dry 0, wet 1, ms time.
PluginState s2Bank(double ms) {
  PluginState s = defaultPluginState("gl.multidelay");
  s2Set(s, s2::MdDry, 0);
  s2Set(s, s2::MdWet, 1);
  for (int k = 0; k < s2::kTaps; ++k) {
    s2Set(s, s2::mdParam(k, s2::MdOn), k == 0);
    s2Set(s, s2::mdParam(k, s2::MdOutPan), 0);
    s2Set(s, s2::mdParam(k, s2::MdOutLevel), 1);
    s2Set(s, s2::mdParam(k, s2::MdFbFType), 0);
    s2Set(s, s2::mdExt(k, s2::MdxTempo), 0);
    s2Set(s, s2::mdParam(k, s2::MdTimeMs), ms);
  }
  return s;
}

void testDelayBankRef() {
  expect(fc::mdGain(0) == 0 && jt::near(fc::mdGain(1), 1, 1e-12) && jt::near(fc::mdGain(-1), -1, 1e-12) &&
             jt::near(fc::mdGain(1.25), (std::pow(11.0, 1.25) - 1) / 10, 1e-12) && jt::near(fc::mdGain(0.5), (std::sqrt(11.0) - 1) / 10, 1e-12),
         "bank: level law (11^x − 1)/10");
  expect(fc::mdOffsetMs(0) == 0 && jt::near(fc::mdOffsetMs(512), 500, 1e-9) && jt::near(fc::mdOffsetMs(-512), -500, 1e-9) &&
             jt::near(fc::mdOffsetMs(256), (std::sqrt(1001.0) - 1) / 2, 1e-9),
         "bank: time offset (1001^(|v|/512) − 1)/2 ms");
  expect(jt::near(fc::mdCutHz(1, 1), 22050.0, 1e-6) && jt::near(fc::mdCutHz(1, 0), 22050 * 0.003, 1e-9) &&
             jt::near(fc::mdCutHz(2, 1), 13230, 1e-3) && jt::near(fc::mdCutHz(6, 1), 13230, 1e-3) && jt::near(fc::mdCutHz(3, 0), 22050 * 0.0001, 1e-9),
         "bank: cutoff curves (LP 66 Hz..22 kHz, others 2.2 Hz..13.2 kHz)");
  expect(jt::near(fc::mdQ(0), 0.1, 1e-12) && jt::near(fc::mdQ(1), 50, 1e-12) && jt::near(fc::mdBw(0), 0.01, 1e-12) && jt::near(fc::mdPeakQ(1), 0.1, 1e-12) &&
             jt::near(fc::mdShelfS(0), 1, 1e-9) && jt::near(fc::mdFilterA(128), 1, 1e-5) && jt::near(fc::mdFilterA(256), 2, 1e-5) &&
             jt::near(fc::mdFilterA(0), 0.1, 1e-12),
         "bank: resonance / bandwidth / slope / gain curves");
  expect(fc::mdGrainWindow(0, 101, 0) < 1e-9 && jt::near(fc::mdGrainWindow(50, 101, 0), 1, 1e-9) && fc::mdGrainWindow(25, 101, 0.5) > fc::mdGrainWindow(25, 101, 0) &&
             fc::mdGrainWindow(25, 101, -0.5) < fc::mdGrainWindow(25, 101, 0),
         "bank: grain window (Hann, bent by shape)");
  PluginState st = defaultPluginState("gl.multidelay");
  expect(s2P(st, s2::MdModel) == 1 && static_cast<int>(st.params.size()) == s2::MdTotal, "bank: new instances use the reference model");
  // Normal mode: echoes at the unit time, each pass scaled by FB vol (first included).
  const size_t D = 4800;  // 100 ms
  PluginState nm0 = s2Bank(100);
  s2Set(nm0, s2::mdParam(0, s2::MdFeedback), 0.5);
  S2Out o = s2Run(nm0, s2Impulse(24000, 10));
  expect(jt::near(o.l[10 + D], 0.5, 1e-6) && jt::near(o.l[10 + 2 * D], 0.25, 1e-6) && s2Peak(o.l, 0, 10 + D) < 1e-9, "bank: echoes at the unit time, × FB vol each pass");
  // Leave first echo: the first echo is unprocessed.
  s2Set(nm0, s2::mdExt(0, s2::MdxLeaveFirst), 1);
  S2Out lf = s2Run(nm0, s2Impulse(24000, 10));
  expect(jt::near(lf.l[10 + D], 1.0, 1e-6) && jt::near(lf.l[10 + 2 * D], 0.5, 1e-6), "bank: leave first echo");
  // Ping-pong: left input, echoes alternate sides.
  PluginState pp = s2Bank(100);
  s2Set(pp, s2::mdParam(0, s2::MdFbMode), s2::MfbPingPong);
  s2Set(pp, s2::mdParam(0, s2::MdFeedback), 1);
  s2Set(pp, s2::mdExt(0, s2::MdxLeaveFirst), 1);
  std::vector<float> il = s2Impulse(24000, 10), ir(24000, 0.f);
  S2Out po = s2Run(pp, il, ir);
  expect(jt::near(po.l[10 + D], 1, 1e-6) && std::fabs(po.r[10 + D]) < 1e-9 && jt::near(po.r[10 + 2 * D], 1, 1e-6) && std::fabs(po.l[10 + 2 * D]) < 1e-9 &&
             jt::near(po.l[10 + 3 * D], 1, 1e-6),
         "bank: ping-pong alternates sides");
  // Invert mode: stored pass swaps sides (output of pass 2 on the right).
  PluginState iv = pp;
  s2Set(iv, s2::mdParam(0, s2::MdFbMode), s2::MfbInvert);
  S2Out io = s2Run(iv, il, ir);
  expect(jt::near(io.l[10 + D], 1, 1e-6) && jt::near(io.r[10 + 2 * D], 1, 1e-6) && jt::near(io.l[10 + 3 * D], 1, 1e-6), "bank: invert swaps L/R every pass");
  // Off mode: no delay line (filter / level path only).
  PluginState of = s2Bank(100);
  s2Set(of, s2::mdParam(0, s2::MdFbMode), s2::MfbOff);
  S2Out oo = s2Run(of, s2Impulse(4800, 10));
  expect(jt::near(oo.l[10], 1, 1e-6) && s2Peak(oo.l, 11, 4800) < 1e-9, "bank: off mode passes the unit input");
  // Stereo offset: positive delays the right side.
  PluginState so = s2Bank(100);
  s2Set(so, s2::mdParam(0, s2::MdOffset), 10);
  s2Set(so, s2::mdExt(0, s2::MdxLeaveFirst), 1);
  S2Out sof = s2Run(so, s2Impulse(24000, 10));
  expect(jt::near(sof.l[10 + D], 1, 1e-6) && jt::near(sof.r[10 + D + 480], 1, 1e-6), "bank: time offset delays one side");
  // Separation +1 blends straight and cross equally (mono feedback).
  PluginState sp = pp;
  s2Set(sp, s2::mdParam(0, s2::MdFbMode), s2::MfbNormal);
  s2Set(sp, s2::mdParam(0, s2::MdSep), 1);
  S2Out spo = s2Run(sp, il, ir);
  expect(jt::near(spo.l[10 + 2 * D], 0.5, 1e-6) && jt::near(spo.r[10 + 2 * D], 0.5, 1e-6), "bank: separation +1 = mono feedback");
  // Filters: LP vs HP in the feedback; cascade steeper; post filter on the output.
  auto band = [](const S2Out& x, bool high) {
    double e = 0;
    // one-pole split at ~2 kHz
    double lp = 0;
    for (float v : x.l) {
      lp += 0.23 * (v - lp);
      double h = v - lp;
      e += high ? h * h : lp * lp;
    }
    return e;
  };
  PluginState fl = s2Bank(20);
  s2Set(fl, s2::mdParam(0, s2::MdFeedback), 0.7);
  s2Set(fl, s2::mdParam(0, s2::MdFbFType), s2::MfLowPass);
  s2Set(fl, s2::mdParam(0, s2::MdFbCut), 800);
  s2Set(fl, s2::mdParam(0, s2::MdFbRes), 0.3);
  std::vector<float> nz = s2Noise(24000, 3);
  S2Out lpO = s2Run(fl, nz);
  s2Set(fl, s2::mdExt(0, s2::MdxFbStages), 3);
  S2Out lp3 = s2Run(fl, nz);
  s2Set(fl, s2::mdExt(0, s2::MdxFbStages), 1);
  s2Set(fl, s2::mdParam(0, s2::MdFbFType), s2::MfHighPass);
  S2Out hpO = s2Run(fl, nz);
  expect(band(lpO, true) / band(lpO, false) < band(hpO, true) / band(hpO, false) && band(lp3, true) < band(lpO, true),
         "bank: LP / HP feedback filters, cascade is steeper");
  bool typesFinite = true;
  for (int ty = 0; ty < s2::MfCount; ++ty) {
    s2Set(fl, s2::mdParam(0, s2::MdFbFType), ty);
    s2Set(fl, s2::mdParam(0, s2::MdInFType), ty);
    s2Set(fl, s2::mdExt(0, s2::MdxInPost), ty & 1);
    S2Out t = s2Run(fl, nz, 128);
    typesFinite = typesFinite && jt::finite(t.l) && jt::rms(t.l) > 1e-4 && jt::rms(t.l) < 10;
  }
  expect(typesFinite, "bank: all eight filter types (pre / post) work");
  // Grain: windowed echoes (silent at the window start).
  PluginState gr = s2Bank(100);
  s2Set(gr, s2::mdParam(0, s2::MdGateDiv), 4);
  s2Set(gr, s2::mdExt(0, s2::MdxLeaveFirst), 1);
  std::vector<float> dc(24000, 0.5f);
  S2Out go = s2Run(gr, dc);
  expect(std::fabs(go.l[2 * D]) < 0.01 && std::fabs(go.l[2 * D + 600]) > 0.4, "bank: grain window chops the echo (period = time / div)");
  // Oversampling keeps the echo time.
  PluginState ov = nm0;
  s2Set(ov, s2::MdOversample, 1);
  S2Out ovo = s2Run(ov, s2Impulse(24000, 10));
  size_t pk = 0;
  for (size_t i = 0; i < ovo.l.size(); ++i)
    if (std::fabs(ovo.l[i]) > std::fabs(ovo.l[pk])) pk = i;
  char ovn[96];
  std::snprintf(ovn, sizeof ovn, "bank: 2× oversampling keeps the time (peak %d at %+d)", static_cast<int>(1000 * std::fabs(ovo.l[pk])), static_cast<int>(pk) - static_cast<int>(10 + D));
  expect(jt::finite(ovo.l) && pk >= 10 + D && pk <= 10 + D + 30 && std::fabs(ovo.l[pk]) > 0.5, ovn);
  // Legacy, block-size invariance, extremes.
  expect(s2OldIsLegacy("gl.multidelay", s2::MdCount, s2::MdModel), "bank: pre-J-1 state loads as legacy");
  PluginState bi = s2Bank(90);
  s2Set(bi, s2::mdParam(1, s2::MdOn), 1);
  s2Set(bi, s2::mdParam(0, s2::MdToNext), 0.7);
  s2Set(bi, s2::mdParam(0, s2::MdGateDiv), 3);
  s2Set(bi, s2::mdParam(0, s2::MdFbFType), s2::MfPeak);
  s2Set(bi, s2::mdParam(1, s2::MdFbMode), s2::MfbPingPong);
  s2Set(bi, s2::mdParam(1, s2::MdFbPan), -0.4);
  expect(s2BlockInvariant(bi), "bank: block-size invariant (constant settings)");
  expect(s2Extremes("gl.multidelay"), "bank: extremes stay finite");
  // Decoder: v15 time fix, ms mode, filter mapping, modes, exactness.
  auto bankBlob = [](int version, int time, uint8_t tempo, int fbType, int mode, int pan) {
    S2Blob b;
    b.i32(version);
    for (int x : {128, 128, 128, 128}) b.i32(x);
    for (int k = 0; k < 8; ++k)
      for (int x : {k == 0 ? pan : 0, 128, 33145, 17500, 128, 0, 64, 0, time, 0, 33145, 17500, 128, 0, 128, 0}) b.i32(x);
    b.i32(0);
    std::vector<uint8_t> rec(8 * 32, 0);
    rec[8] = static_cast<uint8_t>(fbType);
    rec[17] = static_cast<uint8_t>(mode);
    rec[29] = 1;
    rec[31] = tempo;
    b.b.insert(b.b.end(), rec.begin(), rec.end());
    return b.b;
  };
  flp::PluginMapping m15, m16, mms, mf;
  expect(s2Decode("Fruity Delay Bank", bankBlob(15, 192, 1, 0, 1, 0), m15) && jt::near(s2P(m15.state, s2::mdParam(0, s2::MdSteps)), 4, 1e-6) &&
             s2P(m15.state, s2::mdExt(0, s2::MdxTempo)) == 1 && m15.exact,
         "bank decode: version 15 time × 10, exact");
  expect(s2Decode("Fruity Delay Bank", bankBlob(16, 1920, 0, 0, 3, 0), mms) && jt::near(s2P(mms.state, s2::mdParam(0, s2::MdTimeMs)), 500, 1e-4) &&
             s2P(mms.state, s2::mdExt(0, s2::MdxTempo)) == 0 && s2P(mms.state, s2::mdParam(0, s2::MdFbMode)) == s2::MfbPingPong,
         "bank decode: ms time v/3.84, ping-pong mode");
  expect(s2Decode("Fruity Delay Bank", bankBlob(16, 1920, 1, 2, 2, 0), mf) && s2P(mf.state, s2::mdParam(0, s2::MdFbFType)) == s2::MfBandPass &&
             s2P(mf.state, s2::mdParam(0, s2::MdFbMode)) == s2::MfbInvert && !mf.exact,
         "bank decode: FL band pass → band pass, invert; filters not exact (frequency scale assumed)");
  expect(s2Decode("Fruity Delay Bank", bankBlob(16, 1920, 1, 0, 1, -64), m16) && !m16.exact && jt::near(s2P(m16.state, s2::MdWet), 1, 1e-9) &&
             jt::near(s2P(m16.state, s2::mdParam(0, s2::MdFeedback)), 0.5, 1e-9),
         "bank decode: levels / feedback; pan not exact (pan law assumed)");
  if (jt::haveFl()) {
    int total, ok, exact;
    s2Presets("Fruity Delay Bank", "gl.multidelay", total, ok, exact);
    char nm[96];
    std::snprintf(nm, sizeof nm, "bank: FL presets decode (%d/%d, exact %d)", ok, total, exact);
    expect(total > 0 && ok == total, nm);
  }
}

void testJ1Space2() {
  testBbdChorusRef();
  testPhaserRef();
  testDelayBankRef();
}

}  // namespace

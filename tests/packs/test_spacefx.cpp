// Gridline — "spacefx" pack tests: convolution against direct convolution,
// tap timing, spectral delay timing, reverb decay / freeze, modulation
// sanity, silence, extremes, block-size invariance, state normalization.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/PluginInfo.h"
#include "engine/Effects.h"
#include "packs/Packs.h"
#include "packs/spacefx/sfx_convolver.h"
#include "packs/spacefx/spacefx.h"

using namespace gl;
namespace sfx = gl::packs::spacefx;

namespace {

const char* kTypes[] = {"gl.convolver", "gl.multidelay", "gl.banddelay", "gl.hall", "gl.ensemble", "gl.bbdchorus", "gl.swirl", "gl.vphaser"};
constexpr double kSr = 48000.0;

struct Buf {
  std::vector<float> l, r;
  explicit Buf(size_t n = 0) : l(n, 0.f), r(n, 0.f) {}
  size_t size() const { return l.size(); }
};

Buf noise(size_t n, uint32_t seed, float amp = 0.5f) {
  Buf b(n);
  std::mt19937 g(seed);
  std::uniform_real_distribution<float> d(-amp, amp);
  for (size_t i = 0; i < n; ++i) {
    b.l[i] = d(g);
    b.r[i] = d(g);
  }
  return b;
}

ProcessContext ctx(double tempo = 120) {
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = tempo;
  return c;
}

// Runs a fresh effect over the buffer in blocks of `block` frames.
Buf run(const PluginState& st, const Buf& in, int block, double tempo = 120) {
  auto fx = packs::createEffect(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  Buf out = in;
  ProcessContext c = ctx(tempo);
  for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.size() - pos));
    fx->processAux(st, aux, c, out.l.data() + pos, out.r.data() + pos, n);
  }
  return out;
}

double maxDiff(const Buf& a, const Buf& b) {
  double m = 0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max({m, std::fabs(static_cast<double>(a.l[i]) - b.l[i]), std::fabs(static_cast<double>(a.r[i]) - b.r[i])});
  return m;
}
double peak(const Buf& a, size_t from = 0, size_t to = SIZE_MAX) {
  double m = 0;
  for (size_t i = from; i < std::min(to, a.size()); ++i) m = std::max({m, std::fabs(static_cast<double>(a.l[i])), std::fabs(static_cast<double>(a.r[i]))});
  return m;
}
double rms(const std::vector<float>& x, size_t from, size_t to) {
  double e = 0;
  to = std::min(to, x.size());
  for (size_t i = from; i < to; ++i) e += static_cast<double>(x[i]) * x[i];
  return to > from ? std::sqrt(e / static_cast<double>(to - from)) : 0.0;
}
bool finite(const Buf& a) {
  for (size_t i = 0; i < a.size(); ++i)
    if (!std::isfinite(a.l[i]) || !std::isfinite(a.r[i])) return false;
  return true;
}
void setp(PluginState& s, int k, float v) { s.params[static_cast<size_t>(k)] = v; }

std::vector<double> directConv(const std::vector<float>& x, const std::vector<float>& h, size_t n) {
  std::vector<double> y(n, 0.0);
  for (size_t i = 0; i < x.size(); ++i) {
    if (x[i] == 0.f) continue;
    for (size_t k = 0; k < h.size() && i + k < n; ++k) y[i + k] += static_cast<double>(x[i]) * h[k];
  }
  return y;
}

// ------------------------------------------------------------------ tests
void testCatalogue() {
  for (const char* t : kTypes) {
    const PluginInfo* info = findPlugin(t);
    expect(info != nullptr, (std::string("catalogue has ") + t).c_str());
    if (!info) continue;
    expect(info->kind == PluginKind::Effect, (std::string(t) + " is an effect").c_str());
    expect(info->presets.size() >= 2 && info->presets.size() <= 6, (std::string(t) + " has 2-6 presets").c_str());
    expect(info->editorWidth > 0, (std::string(t) + " sets an editor width").c_str());
    expect(packs::createEffect(t) != nullptr, (std::string(t) + " creates a runtime").c_str());
    bool ok = true;
    for (size_t p = 0; p < info->presets.size(); ++p) {
      PluginState st = presetState(*info, p);
      Buf out = run(st, noise(8000, 3), 256);
      ok = ok && finite(out) && peak(out) < 20;
    }
    expect(ok, (std::string(t) + " presets produce finite output").c_str());
  }
  expect(sfx::MdTotal == static_cast<int>(findPlugin("gl.multidelay")->params.size()), "multi delay param count matches layout");
}

void testSilenceAndExtremes() {
  for (const char* t : kTypes) {
    PluginState st = defaultPluginState(t);
    Buf out = run(st, Buf(24000), 512);
    expect(peak(out) < 1e-6, (std::string(t) + " silence in → silence out").c_str());
    const PluginInfo* info = findPlugin(t);
    bool ok = true;
    for (int mode = 0; mode < 3; ++mode) {
      PluginState x = defaultPluginState(t);
      std::mt19937 g(static_cast<uint32_t>(mode + 11));
      for (size_t k = 0; k < info->params.size(); ++k) {
        const ParamDesc& d = info->params[k];
        double n = mode == 0 ? 0.0 : (mode == 1 ? 1.0 : std::uniform_real_distribution<double>(0, 1)(g));
        x.params[k] = static_cast<float>(d.fromNormalized(n));
      }
      Buf in = noise(48000, 7, 1.0f);
      Buf out2 = run(x, in, 333);
      // The multi delay's reference model chains 8 units with signed gains up to
      // 1.9 and +12 dB filter stages (as its reference does): finite, not bounded.
      ok = ok && finite(out2) && (std::string(t) == "gl.multidelay" ? peak(out2) < 1e12 : peak(out2) < 200);
    }
    expect(ok, (std::string(t) + " extreme parameters stay finite").c_str());
  }
}

void testBlockInvariance() {
  for (const char* t : kTypes) {
    PluginState st = defaultPluginState(t);
    if (std::string(t) == "gl.multidelay") {
      setp(st, sfx::mdParam(0, sfx::MdModDepth), 0.4f);
      setp(st, sfx::mdParam(0, sfx::MdGateDiv), 3.f);
      setp(st, sfx::mdParam(1, sfx::MdDiffuse), 0.5f);
    }
    Buf in = noise(30000, 21);
    Buf a = run(st, in, 1024);
    Buf b = run(st, in, 1);
    Buf c = run(st, in, 77);
    double d = std::max(maxDiff(a, b), maxDiff(a, c));
    char name[96];
    std::snprintf(name, sizeof name, "%s block-size invariant (max diff %.2g)", t, d);
    expect(d < 1e-5, name);
  }
}

void testConvolverMatchesDirect() {
  sfx::setConvolverRateHint(kSr);
  for (int src : {sfx::SrcCabinet, sfx::SrcRoom}) {
    PluginState st = defaultPluginState("gl.convolver");
    setp(st, sfx::CvSource, static_cast<float>(src));
    setp(st, sfx::CvSize, 0.2f);
    setp(st, sfx::CvStereo, 0);  // stereo: L→L, R→R
    setp(st, sfx::CvDry, 0);
    setp(st, sfx::CvWet, 1);
    sfx::IrView v = sfx::convolverIr(st);
    expect(v.ir && v.ir->frames() > 100, "built-in IR is generated");
    if (!v.ir) continue;
    Buf in = noise(3000, 5);
    size_t n = std::min<size_t>(v.ir->frames() + 3000, 40000);
    Buf padded(n);
    std::copy(in.l.begin(), in.l.end(), padded.l.begin());
    std::copy(in.r.begin(), in.r.end(), padded.r.begin());
    Buf out = run(st, padded, 480);
    auto el = directConv(in.l, v.ir->left, n), er = directConv(in.r, v.ir->right, n);
    double err = 0, ref = 0;
    for (size_t i = 0; i < n; ++i) {
      err = std::max({err, std::fabs(out.l[i] - el[i]), std::fabs(out.r[i] - er[i])});
      ref = std::max({ref, std::fabs(el[i]), std::fabs(er[i])});
    }
    char name[120];
    std::snprintf(name, sizeof name, "convolver (%s, %zu-tap IR) equals direct convolution (rel err %.2g)", src == sfx::SrcCabinet ? "cabinet" : "room",
                  v.ir->frames(), err / std::max(1e-9, ref));
    expect(ref > 0.01 && err / ref < 2e-3, name);
  }
  // True stereo: a left-only input reaches the right output through the cross path.
  {
    PluginState st = defaultPluginState("gl.convolver");
    setp(st, sfx::CvSource, sfx::SrcChamber);
    setp(st, sfx::CvSize, 0.1f);
    setp(st, sfx::CvStereo, 1);
    setp(st, sfx::CvDry, 0);
    setp(st, sfx::CvWet, 1);
    std::vector<float> lr;
    {
      std::lock_guard<std::mutex> lk(sfx::detail::irMutex());
      sfx::detail::IrCacheEntry* e = sfx::detail::irEntry(st);
      expect(e->edited.paths == 4, "true stereo IR has four paths");
      lr = e->edited.p[2];
    }
    Buf in(20000);
    in.l[10] = 1.f;
    Buf out = run(st, in, 256);
    double err = 0, ref = 0;
    for (size_t i = 0; i + 10 < out.size() && i < lr.size(); ++i) {
      err = std::max(err, std::fabs(static_cast<double>(out.r[i + 10]) - lr[i]));
      ref = std::max(ref, std::fabs(static_cast<double>(lr[i])));
    }
    expect(ref > 0 && err / ref < 2e-3, "true stereo cross path (L→R) matches the IR");
  }
  // Tempo-synced length cuts the tail; pre-delay shifts it.
  {
    PluginState st = defaultPluginState("gl.convolver");
    setp(st, sfx::CvSource, sfx::SrcHall);
    setp(st, sfx::CvDry, 0);
    setp(st, sfx::CvSync, 1);
    setp(st, sfx::CvSyncLen, 0);  // 1/16 at 120 bpm = 0.125 s
    Buf in(48000);
    in.l[0] = in.r[0] = 1.f;
    Buf out = run(st, in, 512);
    expect(peak(out, 0, 4800) > 1e-3 && peak(out, static_cast<size_t>(0.2 * kSr)) < 1e-6, "tempo-synced length truncates the tail");
    setp(st, sfx::CvSync, 0);
    setp(st, sfx::CvPreDelay, 100);
    Buf out2 = run(st, in, 512);
    expect(peak(out2, 0, 4790) < 1e-7 && peak(out2, 4800, 9600) > 1e-3, "pre-delay delays the wet signal");
  }
  // Missing file → no wet signal, no crash; rate mismatch still runs.
  {
    PluginState st = defaultPluginState("gl.convolver");
    setp(st, sfx::CvSource, sfx::SrcFile);
    st.data = "/nonexistent/impulse.wav";
    setp(st, sfx::CvDry, 0);
    Buf out = run(st, noise(4000, 1), 128);
    expect(peak(out) == 0.0, "missing impulse file gives silence");
    expect(!sfx::convolverIr(st).error.empty(), "missing impulse file reports an error");
    sfx::setConvolverRateHint(44100);
    PluginState h = defaultPluginState("gl.convolver");
    Buf o2 = run(h, noise(4000, 2), 128);
    expect(finite(o2) && peak(o2) > 0, "kernel prepared for another rate still processes");
    sfx::setConvolverRateHint(kSr);
  }
}

void testMultiDelayTaps() {
  // Legacy model semantics (global sync, unscaled first echo); the J-1
  // reference model is covered by tests/j1/j1_space2.inc.
  PluginState st = defaultPluginState("gl.multidelay");
  setp(st, sfx::MdModel, 0);
  setp(st, sfx::MdSync, 0);
  setp(st, sfx::MdDry, 0);
  setp(st, sfx::MdWet, 1);
  for (int k = 0; k < 2; ++k) {
    setp(st, sfx::mdParam(k, sfx::MdFbFType), 0);
    setp(st, sfx::mdParam(k, sfx::MdOutLevel), 1);
    setp(st, sfx::mdParam(k, sfx::MdOutPan), 0);
  }
  setp(st, sfx::mdParam(0, sfx::MdTimeMs), 100);
  setp(st, sfx::mdParam(0, sfx::MdFeedback), 0.5f);
  setp(st, sfx::mdParam(1, sfx::MdTimeMs), 250);
  setp(st, sfx::mdParam(1, sfx::MdFeedback), 0);
  Buf in(30000);
  in.l[0] = in.r[0] = 1.f;
  Buf out = run(st, in, 256);
  auto near = [](double a, double b) { return std::fabs(a - b) < 0.02; };
  expect(near(out.l[4800], 1.0) && near(out.r[4800], 1.0), "tap 1 echo at 100 ms");
  expect(near(out.l[9600], 0.5), "tap 1 second echo at 200 ms with half level");
  expect(near(out.l[12000], 1.0), "tap 2 echo at 250 ms");
  expect(std::fabs(out.l[4700]) < 1e-6 && std::fabs(out.l[26000]) < 1e-6, "no output between echoes");
  // Cascade: tap 1 routes into tap 2.
  setp(st, sfx::mdParam(0, sfx::MdFeedback), 0);
  setp(st, sfx::mdParam(0, sfx::MdToNext), 1);
  Buf c = run(st, in, 256);
  expect(near(c.l[4800 + 12000], 1.0), "to-next cascade echoes at t1 + t2");
  // Tempo sync: 4 steps at 120 bpm = 0.5 s.
  PluginState sy = st;
  setp(sy, sfx::MdSync, 1);
  setp(sy, sfx::mdParam(0, sfx::MdSteps), 4);
  setp(sy, sfx::mdParam(0, sfx::MdToNext), 0);
  setp(sy, sfx::mdParam(1, sfx::MdOn), 0);
  Buf s2 = run(sy, Buf(in), 512, 120);
  expect(near(s2.l[24000], 1.0), "synced tap: 4 steps at 120 bpm = 500 ms");
  // Ping-pong: second echo appears on the right.
  PluginState pp = sy;
  setp(pp, sfx::mdParam(0, sfx::MdFbMode), 1);
  setp(pp, sfx::mdParam(0, sfx::MdFeedback), 0.5f);
  setp(pp, sfx::mdParam(0, sfx::MdSteps), 1);  // 125 ms
  Buf p2 = run(pp, in, 512);
  expect(std::fabs(p2.r[6000]) < 1e-6 && std::fabs(p2.r[12000]) > 0.2 && std::fabs(p2.l[12000]) < 1e-6, "ping-pong alternates sides");
}

void testBandDelay() {
  PluginState st = defaultPluginState("gl.banddelay");
  sfx::BandCurves c = sfx::defaultBandCurves();
  for (int b = 0; b < 2; ++b)
    for (int i = 0; i < sfx::kBdPoints; ++i) {
      c.v[b][sfx::BtDelay][i] = 0.5f;
      c.v[b][sfx::BtFeedback][i] = 0.f;
      c.v[b][sfx::BtPan][i] = 0.5f;
      c.v[b][sfx::BtLevel][i] = 1.f;
    }
  st.data = sfx::formatBandCurves(c);
  sfx::BandCurves back = sfx::parseBandCurves(st.data);
  expect(std::fabs(back.v[1][sfx::BtLevel][31] - 1.f) < 1e-4 && std::fabs(back.v[0][sfx::BtDelay][7] - 0.5f) < 1e-4, "band curves round-trip through data");
  setp(st, sfx::BdMaxTime, 0.5f);  // → 250 ms
  setp(st, sfx::BdDry, 0);
  setp(st, sfx::BdWet, 1);
  setp(st, sfx::BdLowCut, 20);
  Buf in(48000);
  in.l[1000] = in.r[1000] = 1.f;
  Buf out = run(st, in, 300);
  size_t arg = 0;
  double m = 0;
  for (size_t i = 0; i < out.size(); ++i)
    if (std::fabs(out.l[i]) > m) m = std::fabs(out.l[i]), arg = i;
  double t = (static_cast<double>(arg) - 1000) / kSr;
  char name[96];
  std::snprintf(name, sizeof name, "band delay echo lands at the drawn time (%.3f s, expect 0.25)", t);
  expect(std::fabs(t - 0.25) < 0.0065 && m > 0.3, name);
  expect(peak(out, 0, 1000 + static_cast<size_t>(0.2 * kSr)) < 0.05, "no early leakage before the band delay");
  // Feedback produces a second echo; morph to a bank with level 0 silences.
  sfx::BandCurves f = c;
  for (int i = 0; i < sfx::kBdPoints; ++i) f.v[0][sfx::BtFeedback][i] = 1.f, f.v[1][sfx::BtLevel][i] = 0.f;
  PluginState fs = st;
  fs.data = sfx::formatBandCurves(f);
  setp(fs, sfx::BdFeedback, 0.5f);
  Buf o2 = run(fs, in, 512);
  expect(peak(o2, 1000 + 23000, 1000 + 25000) > 0.15, "band feedback repeats the echo");
  setp(fs, sfx::BdMorph, 1);
  Buf o3 = run(fs, in, 512);
  expect(peak(o3) < 1e-4, "morph to a silent bank removes the wet signal");
  ChannelAux aux;
  expect(packs::prepareAux(st, aux) && aux.sample(0) != nullptr, "band delay prepares curve data");
  ChannelAux aux2;
  expect(!packs::prepareAux(defaultPluginState("gl.hall"), aux2), "hall needs no prepared data");
}

void testHall() {
  PluginState st = defaultPluginState("gl.hall");
  setp(st, sfx::HvDry, -60);
  setp(st, sfx::HvWet, 0);
  setp(st, sfx::HvDecay, 1.0f);
  setp(st, sfx::HvCharacter, 0);
  setp(st, sfx::HvModDepth, 0);
  Buf in(static_cast<size_t>(kSr * 3));
  in.l[0] = in.r[0] = 1.f;
  Buf out = run(st, in, 512);
  double e1 = rms(out.l, static_cast<size_t>(0.25 * kSr), static_cast<size_t>(0.35 * kSr));
  double e2 = rms(out.l, static_cast<size_t>(1.25 * kSr), static_cast<size_t>(1.35 * kSr));
  double drop = 20 * std::log10(e1 / std::max(1e-12, e2));
  char name[96];
  std::snprintf(name, sizeof name, "hall tail decays ~60 dB per RT60 (measured %.1f dB over 1 s, RT 1 s)", drop);
  expect(drop > 40 && drop < 85, name);
  expect(peak(out, static_cast<size_t>(2.8 * kSr)) < 1e-3, "hall tail dies out");
  double rt = sfx::hallRt60(st, 1000);
  std::snprintf(name, sizeof name, "hallRt60 at 1 kHz near the decay setting (%.2f s)", rt);
  expect(rt > 0.6 && rt < 1.3, name);
  expect(sfx::hallRt60(st, 10000) < sfx::hallRt60(st, 1000), "high frequencies decay faster (default damping)");
  // Freeze holds the tail.
  PluginState fz = st;
  setp(fz, sfx::HvFreeze, 1);
  setp(fz, sfx::HvDecay, 1.0f);
  auto fx = packs::createEffect("gl.hall");
  ProcessContext c = ctx();
  Buf b = noise(static_cast<size_t>(kSr * 0.5), 4, 0.3f);
  fx->process(st, c, b.l.data(), b.r.data(), static_cast<int>(b.size()));
  Buf z(static_cast<size_t>(kSr * 3));
  for (size_t pos = 0; pos < z.size(); pos += 1000) fx->process(fz, c, z.l.data() + pos, z.r.data() + pos, 1000);
  double f1 = rms(z.l, static_cast<size_t>(0.5 * kSr), static_cast<size_t>(1.0 * kSr)), f2 = rms(z.l, static_cast<size_t>(2.4 * kSr), static_cast<size_t>(2.9 * kSr));
  std::snprintf(name, sizeof name, "freeze sustains the tail (%.1f dB change over 2 s)", 20 * std::log10(f2 / std::max(1e-12, f1)));
  expect(f1 > 1e-4 && f2 > f1 * 0.7, name);
  // Ducking lowers the wet signal while the input is loud.
  PluginState dk = st;
  setp(dk, sfx::HvModel, 0);  // duck / gate / swell envelope modes are legacy-model controls
  setp(dk, sfx::HvEnvMode, 1);
  setp(dk, sfx::HvEnvAmount, 1);
  setp(dk, sfx::HvEnvThresh, -40);
  setp(dk, sfx::HvDecay, 3);
  Buf loud = noise(static_cast<size_t>(kSr), 9, 0.5f);
  PluginState nd = dk;
  setp(nd, sfx::HvEnvMode, 0);
  Buf ducked = run(dk, loud, 512), plain = run(nd, loud, 512);
  expect(rms(ducked.l, 24000, 48000) < 0.3 * rms(plain.l, 24000, 48000), "ducking reduces the wet level under a loud input");
  // Shimmer adds pitched energy (more output than without).
  PluginState sh = st;
  setp(sh, sfx::HvFbOn, 1);
  setp(sh, sfx::HvDecay, 3);
  Buf so = run(sh, noise(static_cast<size_t>(kSr * 0.3), 6), 512);
  expect(finite(so), "shimmer stays finite");
}

void testModulation() {
  // Each modulation effect changes a sine and keeps its level sensible.
  Buf sine(static_cast<size_t>(kSr));
  for (size_t i = 0; i < sine.size(); ++i) sine.l[i] = sine.r[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265 * 440 * i / kSr));
  for (const char* t : {"gl.ensemble", "gl.bbdchorus", "gl.swirl", "gl.vphaser"}) {
    PluginState st = defaultPluginState(t);
    Buf out = run(st, sine, 256);
    double d = 0;
    for (size_t i = 0; i < out.size(); ++i) d = std::max(d, std::fabs(static_cast<double>(out.l[i]) - sine.l[i]));
    double r = rms(out.l, 4800, out.size());
    char name[96];
    std::snprintf(name, sizeof name, "%s modulates the signal (diff %.2f, rms %.2f)", t, d, r);
    expect(d > 0.05 && r > 0.1 && r < 1.0, name);
    expect(maxDiff(out, Buf(out.size())) > 0 && std::fabs(rms(out.l, 4800, out.size()) - rms(out.r, 4800, out.size())) < 0.3, (std::string(t) + " stereo balanced").c_str());
  }
  // Phaser in manual mode carves a notch.
  PluginState ph = defaultPluginState("gl.vphaser");
  setp(ph, sfx::VpManual, 1);
  setp(ph, sfx::VpManualFreq, 1000);
  setp(ph, sfx::VpFeedback, 0);
  setp(ph, sfx::VpMix, 0.5f);
  double lo = 1e9, hi = 0;
  for (double f = 100; f < 10000; f *= 1.12) {
    Buf s(9600);
    for (size_t i = 0; i < s.size(); ++i) s.l[i] = s.r[i] = static_cast<float>(std::sin(2 * 3.14159265 * f * i / kSr));
    Buf o = run(ph, s, 512);
    double r = rms(o.l, 4800, 9600);
    lo = std::min(lo, r);
    hi = std::max(hi, r);
  }
  expect(lo < 0.1 * hi, "manual phaser has a deep notch");
  // Bucket chorus mode table.
  PluginState bc = defaultPluginState("gl.bbdchorus");
  sfx::BbdModeSettings m1 = sfx::bbdSettings(bc, 120);
  setp(bc, sfx::BcMode, 2);
  sfx::BbdModeSettings m3 = sfx::bbdSettings(bc, 120);
  expect(m1.rateHz < 1 && m3.rateHz > 5 && m3.maxMs - m3.minMs < m1.maxMs - m1.minMs, "fast mode is quicker and shallower");
  expect(std::fabs(sfx::phaserLfo(0, 0.0, 0.5)) < 1e-9 && std::fabs(sfx::phaserLfo(0, 0.5, 0.5) - 1.0) < 1e-9, "phaser LFO sine spans 0..1");
}

void testNormalize() {
  for (const char* t : kTypes) {
    PluginState st;
    st.type = t;
    st.params = {0.5f, 0.2f};
    normalizePluginState(st);
    const PluginInfo* info = findPlugin(t);
    expect(st.params.size() == info->params.size(), (std::string(t) + " old/short state is padded").c_str());
    Buf out = run(st, noise(6000, 8), 256);
    expect(finite(out), (std::string(t) + " padded state runs").c_str());
  }
}

}  // namespace

int main() {
  sfx::setConvolverRateHint(kSr);
  testCatalogue();
  testSilenceAndExtremes();
  testBlockInvariance();
  testConvolverMatchesDirect();
  testMultiDelayTaps();
  testBandDelay();
  testHall();
  testModulation();
  testNormalize();
  return finish();
}

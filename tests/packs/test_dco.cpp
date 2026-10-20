// Gridline — J-22 "dco" group tests: DCO Synth (gl.dcosynth), Warm Trio
// (gl.warmsynth), FM Duo (gl.fmduo), and the decoders of FL Studio's shipped
// Kepler / Kepler Exo / SimSynth / Fruity DX10 states.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/synths/dco.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;
using namespace gl::packs::synths::dco;

namespace {

struct Ev {
  int at;
  bool on;
  uint32_t id;
  double pitch;
  float vel;
};
struct Out {
  std::vector<float> L, R;
  bool finite = true;
  float peak = 0;
  bool activeAtEnd = false;
};
Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block, double sr = 44100.0, double tempo = 120) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.f);
  o.R.assign(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) {
    o.finite = false;
    return o;
  }
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = tempo;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on) g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else g->noteOff(e.id, e.at - pos);
      }
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
  }
  o.activeAtEnd = g->active();
  return o;
}
Out note(const PluginState& st, int len = 22050, int off = 16000, double pitch = 60, float vel = 0.8f, int block = 256) {
  return render(st, {{0, true, 1, pitch, vel}, {off, false, 1, 0, 0}}, len, block);
}
double rms(const std::vector<float>& x, int a, int b) {
  double s = 0;
  for (int i = a; i < b; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / std::max(1, b - a));
}
// Brightness: RMS of the first difference over the RMS (higher = more highs).
double bright(const std::vector<float>& x, int a, int b) {
  double d = 0, s = 0;
  for (int i = a + 1; i < b; ++i) {
    const double v = x[static_cast<size_t>(i)], p = x[static_cast<size_t>(i - 1)];
    d += (v - p) * (v - p);
    s += v * v;
  }
  return s > 0 ? std::sqrt(d / s) : 0.0;
}
double zcHz(const std::vector<float>& x, int a, int b, double sr = 44100) {
  int z = 0;
  for (int i = a + 1; i < b; ++i)
    if (x[static_cast<size_t>(i - 1)] <= 0 && x[static_cast<size_t>(i)] > 0) ++z;
  return z * sr / (b - a);
}
// Normalised autocorrelation at a (fractional) lag.
double periodicity(const std::vector<float>& x, int a, int b, double lag) {
  const int l = static_cast<int>(std::lround(lag));
  double xy = 0, xx = 0, yy = 0;
  for (int i = a; i + l < b; ++i) {
    const double u = x[static_cast<size_t>(i)], v = x[static_cast<size_t>(i + l)];
    xy += u * v;
    xx += u * u;
    yy += v * v;
  }
  return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0.0;
}
double lrDiff(const Out& o, int a, int b) {
  double d = 0;
  for (int i = a; i < b; ++i) d += std::fabs(o.L[static_cast<size_t>(i)] - o.R[static_cast<size_t>(i)]);
  return d / (b - a);
}
PluginState fresh(const char* type) {
  PluginState s;
  s.type = type;
  normalizePluginState(s);
  return s;
}
PluginState with(PluginState s, std::vector<std::pair<int, float>> v) {
  for (auto& [i, x] : v) s.params[static_cast<size_t>(i)] = x;
  return s;
}
bool sameOut(const Out& a, const Out& b) {
  if (a.L.size() != b.L.size()) return false;
  for (size_t i = 0; i < a.L.size(); ++i)
    if (std::fabs(a.L[i] - b.L[i]) > 1e-5f || std::fabs(a.R[i] - b.R[i]) > 1e-5f) return false;
  return true;
}
// Random positions for every parameter (extremes / fuzz).
PluginState randomized(const char* type, uint32_t seed) {
  PluginState s = fresh(type);
  const PluginInfo* info = findPlugin(type);
  std::mt19937 rng(seed);
  for (size_t i = 0; i < info->params.size(); ++i) {
    const ParamDesc& d = info->params[i];
    std::uniform_real_distribution<double> u(0, 1);
    double r = u(rng);
    if (seed % 3 == 0) r = r < 0.5 ? 0.0 : 1.0;  // extremes
    s.params[i] = static_cast<float>(d.clamp(d.min + r * (d.max - d.min)));
  }
  return s;
}

void testCatalogue() {
  for (const char* t : {"gl.dcosynth", "gl.warmsynth", "gl.fmduo"}) {
    const PluginInfo* p = findPlugin(t);
    expect(p && p->kind == PluginKind::Generator, (std::string(t) + ": registered").c_str());
    if (!p) continue;
    expect(p->presets.size() >= 2 && p->presets.size() <= 6, (std::string(t) + ": 2-6 factory presets").c_str());
    bool presetsSound = true;
    for (const auto& pr : p->presets) {
      PluginState s = fresh(t);
      for (auto& [i, v] : pr.values) s.params[static_cast<size_t>(i)] = v;
      normalizePluginState(s);
      Out o = note(s, 30000, 20000);
      if (!o.finite || o.peak < 1e-3f || o.peak > 8) {
        presetsSound = false;
        std::printf("# preset %s/%s peak %g\n", t, pr.name.c_str(), static_cast<double>(o.peak));
      }
    }
    expect(presetsSound, (std::string(t) + ": every factory preset sounds and is finite").c_str());
    // Short (old) param vectors normalise to defaults.
    PluginState s;
    s.type = t;
    s.params = {0.5f};
    normalizePluginState(s);
    expect(s.params.size() == p->params.size(), (std::string(t) + ": short state normalised").c_str());
    Out silent = render(fresh(t), {}, 4096, 256);
    expect(silent.finite && silent.peak == 0.f && !silent.activeAtEnd, (std::string(t) + ": silent without notes").c_str());
  }
  expect(findPlugin("gl.dcosynth")->params.size() == DcCount && DcCount == 174, "DCO Synth: tags 0..154 + matrix + panel + engine model");
}

void testFmDuo() {
  PluginState s = fresh("gl.fmduo");
  Out o = note(s);
  expect(o.finite && o.peak > 0.01f, "FM Duo: sounds");
  // Decay above 0.98 holds; low decay dies away.
  Out hold = note(with(s, {{FdDecay, 1.0f}}), 44100, 40000);
  Out fast = note(with(s, {{FdDecay, 0.3f}}), 44100, 40000);
  expect(rms(hold.L, 30000, 34000) > 10 * rms(fast.L, 30000, 34000), "FM Duo: decay law (hold at max, fast decay at low)");
  // Modulator init level brightens.
  PluginState plain = with(s, {{FdM1Init, 0}, {FdM1Sustain, 0}, {FdM2Init, 0}, {FdM2Sustain, 0}, {FdWaveform, 0}, {FdDecay, 1}});
  PluginState rich = with(plain, {{FdM1Init, 0.8f}, {FdM1Sustain, 0.8f}, {FdM1Coarse, 0.25f}});
  expect(bright(note(rich).L, 4000, 12000) > 1.5 * bright(note(plain).L, 4000, 12000), "FM Duo: modulator depth brightens");
  // Pure carrier is a sine at the note.
  Out sine = note(plain);
  expect(std::fabs(zcHz(sine.L, 4000, 12000) - 261.6) < 6, "FM Duo: carrier at the note pitch");
  Out oct = note(with(plain, {{FdOctave, 1}}));
  expect(std::fabs(zcHz(oct.L, 4000, 12000) - 523.3) < 10, "FM Duo: octave switch");
  // Release.
  Out rel = render(with(s, {{FdDecay, 1.0f}, {FdRelease, 0.0f}}), {{0, true, 1, 60, 0.8f}, {8000, false, 1, 0, 0}}, 30000, 256);
  expect(rms(rel.L, 20000, 30000) < 1e-3 && !rel.activeAtEnd, "FM Duo: short release ends the voice");
  // Velocity sensitivity of the modulators.
  PluginState vs = with(rich, {{FdM1Vel, 1.0f}});
  expect(bright(note(vs, 22050, 16000, 60, 0.2f).L, 4000, 12000) < bright(note(vs, 22050, 16000, 60, 1.0f).L, 4000, 12000),
         "FM Duo: velocity → modulation depth");
  expect(note(with(s, {{FdModThru, 1.0f}, {FdM1Init, 1}, {FdM1Sustain, 1}})).finite, "FM Duo: mod thru finite");
  Out a = note(rich, 12000, 8000, 60, 0.8f, 64), b = note(rich, 12000, 8000, 60, 0.8f, 1000);
  expect(sameOut(a, b), "FM Duo: block-size invariant");
}

void testWarm() {
  PluginState s = fresh("gl.warmsynth");
  Out o = note(s);
  expect(o.finite && o.peak > 0.01f, "Warm Trio: sounds");
  bool shapes = true;
  for (int sh = 0; sh < WsCount; ++sh) {
    Out x = note(with(s, {{WmOsc1Shape, static_cast<float>(sh)}}));
    shapes = shapes && x.finite && x.peak > 0.005f;
  }
  expect(shapes, "Warm Trio: every oscillator shape sounds");
  Out tri = note(with(s, {{WmOsc1Shape, WsTriangle}}));
  expect(std::fabs(zcHz(tri.L, 4000, 12000) - 261.6) < 8, "Warm Trio: oscillator at the note pitch");
  Out up = note(with(s, {{WmOsc1Shape, WsTriangle}, {WmOsc1Coarse, 12}}));
  expect(std::fabs(zcHz(up.L, 4000, 12000) - 523.3) < 12, "Warm Trio: coarse +12");
  const double b0 = bright(note(with(s, {{WmOsc1Shape, WsSine}, {WmOsc1Pw, 0}})).L, 4000, 12000);
  const double b1 = bright(note(with(s, {{WmOsc1Shape, WsSine}, {WmOsc1Pw, 0}, {WmOsc1Oct1, 1}, {WmOsc1Oct2, 1}})).L, 4000, 12000);
  expect(b1 > 1.5 * b0, "Warm Trio: +1/+2 octave buttons layer harmonics");
  const double open = bright(note(s).L, 4000, 12000);
  const double closed = bright(note(with(s, {{WmSvfOn, 1}, {WmCut, 0.25f}, {WmSvfEnv, 0.5f}})).L, 4000, 12000);
  expect(closed < 0.6 * open, "Warm Trio: filter darkens");
  Out hp = note(with(s, {{WmSvfOn, 1}, {WmCut, 0.5f}, {WmSvfHigh, 1.0f}}));
  expect(hp.finite && bright(hp.L, 4000, 12000) > bright(note(with(s, {{WmSvfOn, 1}, {WmCut, 0.5f}})).L, 4000, 12000),
         "Warm Trio: high output brighter than low");
  Out ring = note(with(s, {{WmOsc2On, 1}, {WmRing, 1}, {WmOsc2Coarse, 7}}));
  expect(ring.finite && ring.peak > 0.001f, "Warm Trio: ring 1x2");
  Out ch = note(with(s, {{WmChorus, 1}}));
  expect(lrDiff(ch, 6000, 12000) > 1e-4, "Warm Trio: chorus widens (left differs)");
  Out warm = note(with(s, {{WmOsc1Warm, 1}}));
  expect(warm.finite && rms(warm.L, 4000, 12000) > rms(o.L, 4000, 12000), "Warm Trio: warm layers a detuned copy");
  Out vib = note(with(s, {{WmLfoOn, 1}, {WmOsc1Lfo, 1.0f}, {WmLfoRate, 0.9f}, {WmOsc1Shape, WsTriangle}}));
  expect(vib.finite && std::fabs(zcHz(vib.L, 2000, 6000) - zcHz(vib.L, 8000, 12000)) > 1.0, "Warm Trio: LFO → pitch");
  Out slow = note(with(s, {{WmAmpA, 0.6f}}), 22050, 16000);
  expect(rms(slow.L, 0, 2000) < 0.5 * rms(slow.L, 12000, 16000), "Warm Trio: attack time");
  Out a = note(s, 12000, 8000, 60, 0.8f, 64), b = note(s, 12000, 8000, 60, 0.8f, 999);
  expect(sameOut(a, b), "Warm Trio: block-size invariant");
}

void testDco() {
  PluginState s = fresh("gl.dcosynth");
  Out o = note(s);
  expect(o.finite && o.peak > 0.01f, "DCO Synth: sounds (defaults)");
  // VCA gate default: holds while held, ends after release.
  expect(!render(s, {{0, true, 1, 60, 0.8f}, {4000, false, 1, 0, 0}}, 30000, 256).activeAtEnd, "DCO Synth: gate VCA releases");
  Out saw = note(with(s, {{DcCutoff, 1}}));
  std::printf("# saw zc %g bright %g peak %g\n", zcHz(saw.L, 4000, 12000), bright(saw.L, 4000, 12000), static_cast<double>(saw.peak));
  Out pul = note(with(s, {{DcDco1Saw, 0}, {DcDco1Pulse, 1}, {DcDco1Pw, 0}, {DcDco1PwSrc, 1}, {DcCutoff, 1}}));
  expect(std::fabs(zcHz(pul.L, 4000, 12000) - 261.6) < 10, "DCO Synth: pulse at the note pitch");
  expect(saw.peak > 0.01f && pul.peak > 0.01f, "DCO Synth: saw and pulse");
  const double openB = bright(saw.L, 4000, 12000);
  const double closedB = bright(note(with(s, {{DcCutoff, 0.35f}})).L, 4000, 12000);
  expect(closedB < 0.5 * openB, "DCO Synth: cutoff darkens");
  const double s6 = bright(note(with(s, {{DcCutoff, 0.5f}, {DcSlope, 0}})).L, 4000, 12000);
  const double s24 = bright(note(with(s, {{DcCutoff, 0.5f}, {DcSlope, 3}})).L, 4000, 12000);
  expect(s24 < s6, "DCO Synth: steeper slope is darker");
  Out res = note(with(s, {{DcCutoff, 0.45f}, {DcReso, 0.95f}}));
  expect(res.finite && res.peak < 8, "DCO Synth: high resonance stays bounded");
  Out envSweep = note(with(s, {{DcCutoff, 0.3f}, {DcEnv1ToCut, 0.9f}, {DcEnv1D, 0.6f}, {DcEnv1S, 0}}));
  std::printf("# env sweep brightness %g -> %g\n", bright(envSweep.L, 200, 2000), bright(envSweep.L, 12000, 15000));
  expect(bright(envSweep.L, 200, 2000) > 1.5 * bright(envSweep.L, 12000, 15000), "DCO Synth: envelope sweeps the cutoff");
  Out vcaEnv = note(with(s, {{DcVcaSrc, 0}, {DcEnv1D, 0.2f}, {DcEnv1S, 0}}));
  expect(rms(vcaEnv.L, 12000, 15000) < 0.2 * rms(vcaEnv.L, 200, 2000), "DCO Synth: envelope VCA decays");
  Out two = note(with(s, {{DcDco2On, 1}, {DcDco2Saw, 1}, {DcDco2Semi, 0.625f}, {DcCutoff, 1}}));
  expect(two.finite && rms(two.L, 4000, 12000) > 0.9 * rms(saw.L, 4000, 12000), "DCO Synth: DCO 2 adds a voice");
  Out sync = note(with(s, {{DcDco2On, 1}, {DcDco2Saw, 1}, {DcDco2Semi, 0.6f}, {DcSync, 1}, {DcDco1Level, 0}, {DcCutoff, 1}}));
  std::printf("# sync periodicity %g\n", periodicity(sync.L, 4000, 12000, 44100.0 / 261.63));
  expect(sync.finite && periodicity(sync.L, 4000, 12000, 44100.0 / 261.63) > 0.9 &&
             periodicity(note(with(s, {{DcDco2On, 1}, {DcDco2Saw, 1}, {DcDco2Semi, 0.6f}, {DcDco1Level, 0}, {DcCutoff, 1}})).L, 4000,
                         12000, 44100.0 / 261.63) < 0.9,
         "DCO Synth: hard sync locks DCO 2 to DCO 1's period");
  Out xm = note(with(s, {{DcDco2On, 1}, {DcDco2Saw, 1}, {DcXmod12, 1}, {DcXmod21, 1}, {DcFilterFm, 1}, {DcAm, 1}}));
  expect(xm.finite, "DCO Synth: cross mod / AM / filter FM finite");
  Out chorus = note(with(s, {{DcChorus, 1}}));
  expect(lrDiff(chorus, 6000, 12000) > 1e-4, "DCO Synth: chorus I widens");
  Out fx = note(with(s, {{DcEqOn, 1}, {DcEqGainMid, 1}, {DcDlyOn, 1}, {DcDlyInput, 1}, {DcRevOn, 1}, {DcRevWet, 1},
                         {DcSatOn, 1}, {DcSatAmount, 1}, {DcEnsOn, 1}, {DcEnsMix, 1}}),
                40000, 10000);
  expect(fx.finite && rms(fx.L, 20000, 30000) > 1e-4, "DCO Synth: FX chain rings after release");
  // Matrix: LFO 1 → cutoff modulates brightness over time.
  Out mx = note(with(s, {{DcMxSrc1, 1}, {DcMxDst1, DcCutoff + 1}, {DcMxAmt1, 1.0f}, {DcCutoff, 0.5f}, {DcLfo1Rate, 0.7f}}), 44100,
                40000);
  double bmin = 1e9, bmax = 0;
  for (int k = 0; k < 16; ++k) {
    const double b = bright(mx.L, 4000 + k * 2000, 5500 + k * 2000);
    bmin = std::min(bmin, b);
    bmax = std::max(bmax, b);
  }
  expect(bmax > 1.3 * bmin, "DCO Synth: matrix LFO → cutoff");
  Out pitchMx = note(with(s, {{DcMxSrc1, 3}, {DcMxDst1, kDcDestDco1Pitch}, {DcMxAmt1, 1.0f}, {DcVcaSrc, 1}, {DcCutoff, 1}}));
  expect(zcHz(pitchMx.L, 4000, 12000) > 330, "DCO Synth: matrix env 1 → DCO 1 pitch");
  // Mono glide.
  Out glide = render(with(s, {{DcMono, 1}, {DcPorta, 0.3f}, {DcCutoff, 1}}),
                     {{0, true, 1, 48, 0.8f}, {8000, true, 2, 72, 0.8f}, {30000, false, 2, 0, 0}, {30000, false, 1, 0, 0}}, 32000, 256);
  const double early = zcHz(glide.L, 8200, 9800), late = zcHz(glide.L, 26000, 30000);
  expect(early < late * 0.8 && std::fabs(late - 523.3) < 40, "DCO Synth: mono portamento glides");
  // Arpeggiator: several pitches from one held chord.
  Out arp = render(with(s, {{DcArpOn, 1}, {DcArpSync, 1}, {DcArpRate, 1.0f}, {DcArpGate, 0.5f}, {DcCutoff, 1}}),
                   {{0, true, 1, 60, 0.8f}, {0, true, 2, 64, 0.8f}, {0, true, 3, 67, 0.8f}}, 44100, 256);
  int changes = 0;
  double prev = -1;
  for (int k = 0; k < 20; ++k) {
    const double z = zcHz(arp.L, k * 2000 + 200, k * 2000 + 1800);
    if (prev > 0 && z > 0 && std::fabs(z - prev) > 15) ++changes;
    if (z > 0) prev = z;
  }
  expect(arp.finite && changes >= 3, "DCO Synth: arpeggiator steps through the chord");
  // Classic panel: one DCO, envelope or gate VCA.
  Out one = note(with(s, {{DcPanel, PanelOne}, {DcDco2On, 1}, {DcDco2Saw, 1}, {DcDco2Semi, 0.75f}}));
  expect(one.finite && std::fabs(zcHz(one.L, 4000, 12000) - zcHz(note(with(s, {{DcPanel, PanelOne}})).L, 4000, 12000)) < 5,
         "DCO Synth: one-DCO panel ignores DCO 2");
  bool fuzz = true;
  for (uint32_t seed = 1; seed <= 24; ++seed) {
    Out r = note(randomized("gl.dcosynth", seed), 12000, 8000);
    if (!r.finite || r.peak > 50) {
      fuzz = false;
      std::printf("# seed %u peak %g\n", seed, static_cast<double>(r.peak));
    }
  }
  expect(fuzz, "DCO Synth: random / extreme settings stay finite");
  bool fuzz2 = true;
  for (uint32_t seed = 1; seed <= 24; ++seed) {
    fuzz2 = fuzz2 && note(randomized("gl.warmsynth", seed), 12000, 8000).finite && note(randomized("gl.fmduo", seed), 12000, 8000).finite;
  }
  expect(fuzz2, "Warm Trio / FM Duo: random / extreme settings stay finite");
  Out a = note(with(s, {{DcChorus, 1}, {DcRevOn, 1}}), 12000, 8000, 60, 0.8f, 16);
  Out b = note(with(s, {{DcChorus, 1}, {DcRevOn, 1}}), 12000, 8000, 60, 0.8f, 16 * 50);
  expect(sameOut(a, b), "DCO Synth: block-size invariant (control step multiples)");
  expect(note(s, 12000, 8000, 60, 0.8f, 128).finite && note(fresh("gl.dcosynth"), 12000, 8000).finite, "DCO Synth: 48k / blocks");
  Out hi = render(s, {{0, true, 1, 60, 0.8f}, {8000, false, 1, 0, 0}}, 12000, 256, 96000.0);
  expect(hi.finite && hi.peak > 0.01f, "DCO Synth: 96 kHz");
}

// --------------------------------------------------------------- decoders
flp::Event stateEvent(const std::vector<uint8_t>& d) {
  flp::Event e;
  e.id = 213;
  e.data = d.data();
  e.size = static_cast<uint32_t>(d.size());
  return e;
}
bool decodeRaw(const char* plugin, const std::vector<uint8_t>& data, flp::PluginMapping& m) {
  flp::File f;
  Song song;
  flp::Report rep;
  flp::Context c(f, song, rep);
  const flp::PluginDecoder* dec = flp::findDecoder(plugin);
  if (!dec) return false;
  std::vector<flp::Event> evs = {stateEvent(data)};
  return (*dec)(c, evs, m);
}

struct Bld {
  std::vector<uint8_t> b;
  Bld& u8(uint8_t v) {
    b.push_back(v);
    return *this;
  }
  Bld& u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Bld& f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u32(u);
  }
  Bld& f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(u >> (8 * i)));
    return *this;
  }
  Bld& str(const char* s) {
    for (const char* p = s; *p; ++p) b.push_back(static_cast<uint8_t>(*p));
    b.push_back(0);
    return *this;
  }
};

void testDecodersSynthetic() {
  // DX10 v1.
  {
    Bld b;
    b.u32(1);
    for (int t = 0; t <= 20; ++t) b.u32(static_cast<uint32_t>(t * 3000));
    b.u32(static_cast<uint32_t>(-2));
    flp::PluginMapping m;
    expect(decodeRaw("Fruity DX10", b.b, m) && m.glType == "gl.fmduo" && m.exact, "DX10 → FM Duo (exact)");
    expect(std::fabs(m.state.params[FdDecay] - 3000.f / 65536.f) < 1e-6f && m.state.params[FdOctave] == -2.f, "DX10: tags in order, octave");
    expect(m.param && m.param(13) == FdLfoRate && std::fabs(m.value(13, 0.25) - 0.25) < 1e-9 && m.value(21, 1.0) == 3.0,
           "DX10: automation identity");
    std::vector<uint8_t> shortS(b.b.begin(), b.b.begin() + 60);
    flp::PluginMapping bad;
    expect(!decodeRaw("Fruity DX10", shortS, bad), "DX10: truncated state refused");
  }
  // SimSynth: shapes and switch slots.
  {
    Bld b;
    double d[45] = {};
    d[3] = 0.8;         // osc 1 level
    d[6] = 0.0;         // osc 1 shape → selector 2 (triangle)
    d[13] = 0.3;        // osc 2 shape → selector 4 (sine)
    d[20] = 0.4;        // osc 3 shape → noise
    d[24] = 0.1;        // LFO ramp
    d[1] = 36.0 / 48.0;  // osc 1 coarse +12
    d[26] = 0.6;        // cutoff
    for (double x : d) b.f64(x);
    for (int i = 0; i < 19; ++i) b.u32(0);
    int sw[19] = {};
    sw[0] = 1;   // osc 1 on
    sw[14] = 1;  // filter on
    sw[18] = 1;  // chorus
    for (int x : sw) b.u32(static_cast<uint32_t>(x));
    for (int i = 0; i < 12; ++i) b.u32(0);
    b.u32(14);
    flp::PluginMapping m;
    expect(decodeRaw("SimSynth", b.b, m) && m.glType == "gl.warmsynth" && m.exact, "SimSynth → Warm Trio (exact)");
    const auto& p = m.state.params;
    expect(std::fabs(p[WmOsc1Level] - 0.8f) < 1e-6f && p[WmOsc1Coarse] == 12.f && std::fabs(p[WmCut] - 0.6f) < 1e-6f,
           "SimSynth: knobs from the stored doubles");
    expect(p[WmOsc1Shape] == WsTriangle && p[WmOsc2Shape] == WsSine && p[WmOsc3Shape] == WsNoise && p[WmLfoShape] == WlRamp,
           "SimSynth: shape values → selector entries");
    expect(p[WmOsc1On] == 1 && p[WmSvfOn] == 1 && p[WmChorus] == 1 && p[WmOsc2On] == 0, "SimSynth: switch slots");
    std::vector<uint8_t> shortS(b.b.begin(), b.b.end() - 4);
    flp::PluginMapping bad;
    expect(!decodeRaw("SimSynth", shortS, bad), "SimSynth: wrong size refused");
  }
  // Kepler (one DCO), version 4 with 36 floats and the option bytes.
  {
    Bld b;
    b.u32(0).u32(0).u32(0).u32(9).str("Kepler-1").u32(4).u32(36);
    for (int k = 0; k < 36; ++k) b.f32(k == 14 ? 1.f : k == 19 ? 0.4f : k == 25 ? 1.f : k == 30 ? 2.f / 3.f : 0.f);
    b.u8(1).f32(1.f).f32(0.5f).u8(1).u8(1).u8(1);
    const uint32_t size = static_cast<uint32_t>(b.b.size());
    std::memcpy(b.b.data() + 4, &size, 4);
    b.b[4] = static_cast<uint8_t>(size - 12);
    flp::PluginMapping m;
    expect(decodeRaw("Kepler", b.b, m) && m.glType == "gl.dcosynth" && m.exact, "Kepler → DCO Synth (exact)");
    const auto& p = m.state.params;
    expect(p[DcPanel] == PanelOne && p[DcDco1Saw] == 1 && std::fabs(p[DcCutoff] - 0.4f) < 1e-6f && p[DcChorus] == 2,
           "Kepler: one-DCO panel, tags mapped through the classic table");
    expect(p[DcArpQuantize] == 1 && std::fabs(p[DcVelVol] - 1.f) < 1e-6f && p[DcChorusGate] == 1 && std::fabs(p[DcChorusNoise] - 0.09f) < 1e-6f,
           "Kepler: option bytes");
    expect(m.param && m.param(19) == DcCutoff && m.param(33) == DcVolume, "Kepler: automation tags → controls");
    std::vector<uint8_t> trunc(b.b.begin(), b.b.end() - 2);
    flp::PluginMapping bad;
    expect(!decodeRaw("Kepler", trunc, bad), "Kepler: truncated state refused");
  }
  // Kepler Exo with a matrix row to DCO 2 pitch.
  {
    Bld b;
    b.u32(1).u32(1).u32(0).u32(0).u32(0).u32(0).u32(0).u32(9).str("Kepler-2").u32(2).u32(151);
    for (int t = 0; t < 151; ++t) b.f32(t == 37 ? 0.7f : t == 12 ? 1.f : 0.f);
    b.u32(1).u32(8);
    for (int r = 0; r < 8; ++r) {
      b.u32(r == 0 ? 2u : 0xFFFFFFFFu);
      b.u32(r == 0 ? 257u : 0xFFFFFFFFu);
    }
    b.u32(2).u32(56).u32(0);
    for (int i = 0; i < 56; ++i) b.u8(0);
    flp::PluginMapping m;
    expect(decodeRaw("Kepler Exo", b.b, m) && m.glType == "gl.dcosynth" && m.exact, "Kepler Exo → DCO Synth (exact)");
    const auto& p = m.state.params;
    expect(std::fabs(p[DcCutoff] - 0.7f) < 1e-6f && p[DcDco2On] == 1 && p[DcPanel] == PanelTwo, "Kepler Exo: tags 1:1");
    expect(p[DcMxSrc1] == 3 && p[DcMxDst1] == kDcDestDco2Pitch && p[DcMxSrc1 + 1] == 0, "Kepler Exo: matrix rows");
    std::vector<uint8_t> trunc(b.b.begin(), b.b.end() - 1);
    flp::PluginMapping bad;
    expect(!decodeRaw("Kepler Exo", trunc, bad), "Kepler Exo: truncated state refused");
    std::vector<uint8_t> junk(40, 0xFF);
    expect(!decodeRaw("Kepler Exo", junk, bad) && !decodeRaw("Kepler", junk, bad) && !decodeRaw("SimSynth", junk, bad) &&
               !decodeRaw("Fruity DX10", junk, bad),
           "junk states refused");
  }
}

void corpus(const char* plugin, const char* type) {
  if (!testfl::flInstalled()) {
    std::printf("# FL Studio not installed: %s corpus skipped\n", plugin);
    return;
  }
  for (int pass = 0; pass < 2; ++pass) {
    auto all = pass == 0 ? testfl::decodePresets(plugin) : testfl::decodeProjects(plugin);
    int total = 0, decoded = 0, exact = 0, finite = 0, sounding = 0;
    for (const auto& d : all) {
      ++total;
      if (!d.ok || d.state.type != type) {
        std::printf("# not decoded: %s\n", d.where.c_str());
        continue;
      }
      ++decoded;
      exact += d.exact;
      testfl::maybeDump(d);
      Out o = render(d.state, {{0, true, 1, 60, 0.8f}, {16000, false, 1, 0, 0}}, 24000, 512);
      const bool fin = o.finite && o.peak < 20;
      finite += fin;
      sounding += o.peak > 1e-3f;
      if (!fin) std::printf("# unstable: %s (peak %g)\n", d.where.c_str(), static_cast<double>(o.peak));
      else if (o.peak <= 1e-3f) std::printf("# silent: %s\n", d.where.c_str());
    }
    std::printf("# %s %s: %d states, %d decoded (walked to the end), %d exact, %d finite, %d sounding\n", plugin,
                pass ? "projects" : "presets", total, decoded, exact, finite, sounding);
    if (total == 0) continue;
    const std::string p = std::string(plugin) + (pass ? " projects" : " presets");
    expect(decoded == total, (p + ": every state decodes").c_str());
    expect(finite == decoded, (p + ": every state renders finite").c_str());
    expect(sounding >= decoded * 9 / 10, (p + ": states sound").c_str());
  }
}

}  // namespace

int main() {
  testCatalogue();
  testFmDuo();
  testWarm();
  testDco();
  testDecodersSynthetic();
  corpus("Kepler", "gl.dcosynth");
  corpus("Kepler Exo", "gl.dcosynth");
  corpus("SimSynth", "gl.warmsynth");
  corpus("Fruity DX10", "gl.fmduo");
  return finish();
}

// Gridline — J-4 tests: Additive's subtractive-additive set (colours, custom
// table, timbre EQ, harmonic-domain filter shapes / resonance, phaser shapes,
// harmonizer, pluck options, unison 9, voice modes, LFO sources, effects
// kinds, compression) and the full decode of FL Studio's shipped
// subtractive-additive synth states (presets + projects) into it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/models.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;
using namespace gl::packs::models;

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

Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block, double sr = 44100.0) {
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
  ctx.tempo = 120;
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

double tone(const std::vector<float>& x, int a, int b, double f, double sr = 44100) {
  const double w = 2 * 3.14159265358979 * f / sr, c = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (int i = a; i < b; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * (i - a) / (b - a));
    const double s0 = x[static_cast<size_t>(i)] * win + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (b - a);
}
double zcHz(const std::vector<float>& x, int a, int b, double sr = 44100) {
  int z = 0;
  for (int i = a + 1; i < b; ++i)
    if (x[static_cast<size_t>(i - 1)] <= 0 && x[static_cast<size_t>(i)] > 0) ++z;
  return z * sr / (b - a);
}
float peakIn(const Out& o, int a, int b) {
  float p = 0;
  for (int i = std::max(0, a); i < std::min(b, static_cast<int>(o.L.size())); ++i)
    p = std::max({p, std::fabs(o.L[static_cast<size_t>(i)]), std::fabs(o.R[static_cast<size_t>(i)])});
  return p;
}

PluginState base() {
  PluginState s = defaultPluginState("gl.additive");
  s.params[AdFltType] = AfOff;
  s.params[AdSustain] = 1;
  return s;
}
void set(PluginState& s, int i, float v) { s.params[static_cast<size_t>(i)] = v; }
const std::vector<Ev> kOne = {{0, true, 1, 45, 0.9f}, {30000, false, 1, 0, 0}};  // A2 = 110 Hz

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.additive");
  expect(info != nullptr, "catalogue entry");
  if (!info) return finish();
  expect(static_cast<int>(info->params.size()) == AdCount, "parameter count");
  bool sane = true;
  for (const auto& p : info->params) sane = sane && !p.name.empty() && p.def >= p.min && p.def <= p.max;
  expect(sane, "every parameter named, defaults in range");
  // Older (pre-J-4) states: padded with defaults that keep the previous voicing.
  {
    PluginState s = defaultPluginState("gl.additive");
    s.params.resize(AdFine + 1);
    normalizePluginState(s);
    expect(static_cast<int>(s.params.size()) == AdCount && s.params[AdVelVol] == 1 && s.params[AdPlayMode] == ApAuto &&
               s.params[AdDelType] == AdlPingPong,
           "old states normalize to the previous voicing");
  }
  // Colours: a square-saw has weaker even partials than the saw.
  {
    PluginState a = base(), b = base();
    set(b, AdSpectrumA, SpSqSaw);
    Out oa = render(a, kOne, 20000, 256), ob = render(b, kOne, 20000, 256);
    const double ra = tone(oa.L, 4000, 16000, 440) / tone(oa.L, 4000, 16000, 330);
    const double rb = tone(ob.L, 4000, 16000, 440) / tone(ob.L, 4000, 16000, 330);
    expect(oa.finite && ob.finite && rb < 0.5 * ra, "square-saw colour thins the even partials");
  }
  // Custom table (data): only the 3rd harmonic.
  {
    PluginState s = base();
    set(s, AdSpectrumA, SpCustom);
    AdditiveData d;
    d.level = {0, 0, 1};
    d.phase = {0, 0, 0.25f};
    s.data = serializeAdditiveData(d);
    Out o = render(s, kOne, 20000, 256);
    expect(tone(o.L, 4000, 16000, 330) > 20 * tone(o.L, 4000, 16000, 110), "custom timbre table plays its harmonics");
    AdditiveData e;
    expect(parseAdditiveData(s.data, e) && e.level.size() == 3 && e.phase[2] == 0.25f, "custom table round trip");
    expect(!parseAdditiveData("junk", e) && parseAdditiveData("add 1\nc 99999 1\np x\n", e), "malformed custom data ignored");
    std::vector<float> cyc(256);
    for (int i = 0; i < 256; ++i) cyc[static_cast<size_t>(i)] = static_cast<float>(std::sin(2 * 3.14159265358979 * 2 * i / 256.0));
    AdditiveData an = analyzeCycle(cyc.data(), 256, 8);
    expect(an.level.size() == 8 && an.level[1] > 0.99f && an.level[0] < 0.01f, "single-cycle analysis finds the harmonic");
  }
  // Timbre EQ: band 6 cut darkens the top.
  {
    PluginState a = base(), b = base();
    set(b, AdTeqBase + 5, -1);
    set(b, AdTeqBase + 4, -1);
    Out oa = render(a, kOne, 20000, 256), ob = render(b, kOne, 20000, 256);
    expect(tone(ob.L, 4000, 16000, 110 * 20) < 0.3 * tone(oa.L, 4000, 16000, 110 * 20), "timbre EQ bands cut upper partials");
  }
  // Sub-harmonics 1 / 3 / 4.
  {
    PluginState s = base();
    set(s, AdSub3, 1);
    Out o = render(s, kOne, 20000, 256);
    expect(tone(o.L, 4000, 16000, 165.0) > 1e-3, "half-step partial at 1.5 x f0");
  }
  // Every filter shape / resonance shape: finite, and low-pass shapes cut the top.
  {
    bool ok = true, cuts = true;
    for (int f = 1; f < AfCount; ++f)
      for (int r = 0; r < ArCount; r += 4) {
        PluginState s = base();
        set(s, AdFltType, static_cast<float>(f)), set(s, AdResType, static_cast<float>(r));
        set(s, AdFltCutoff, 600), set(s, AdFltReso, 0.8f), set(s, AdResOsc, 0.5f), set(s, AdFltKey, 0);
        Out o = render(s, kOne, 8000, 128);
        ok = ok && o.finite && o.peak < 8;
      }
    for (int f : {AfLowPass, AfSoftLp, AfSlope, AfHybrid, AfFin}) {
      PluginState s = base();
      set(s, AdFltType, static_cast<float>(f)), set(s, AdFltCutoff, 400), set(s, AdFltWidth, 0.5f), set(s, AdFltKey, 0);
      Out o = render(s, kOne, 20000, 256), r = render(base(), kOne, 20000, 256);
      cuts = cuts && tone(o.L, 4000, 16000, 110 * 16) < 0.2 * tone(r.L, 4000, 16000, 110 * 16);
    }
    expect(ok, "filter shapes x resonance shapes finite");
    expect(cuts, "low-pass family removes upper partials");
  }
  // Self-oscillation sounds at the cutoff even with a sine timbre.
  {
    PluginState s = base();
    set(s, AdSpectrumA, SpSine), set(s, AdFltType, AfLowPass), set(s, AdFltCutoff, 1500), set(s, AdFltKey, 0), set(s, AdResOsc, 1);
    Out o = render(s, kOne, 20000, 256);
    expect(tone(o.L, 4000, 16000, 1500) > 1e-3, "resonance self-oscillation");
  }
  // Phaser shapes are finite and notch the spectrum.
  {
    bool ok = true;
    for (int p = 0; p < ApCount; ++p) {
      PluginState s = base();
      set(s, AdPhType, static_cast<float>(p)), set(s, AdPhMix, 1), set(s, AdPhSpeed, 0.5f), set(s, AdPhScale, p % 2 ? 1.f : 0.f);
      Out o = render(s, kOne, 8000, 128);
      ok = ok && o.finite && o.peak > 1e-3f;
    }
    expect(ok, "phaser shapes finite and audible");
  }
  // Harmonizer adds partials (octave copies) at every position.
  {
    bool ok = true;
    for (int pos = 0; pos < 4; ++pos) {
      PluginState s = base();
      set(s, AdSpectrumA, SpSquare), set(s, AdHzMix, 1), set(s, AdHzType, AhHarm), set(s, AdHzWidth, 1), set(s, AdHzPos, static_cast<float>(pos));
      Out o = render(s, kOne, 20000, 256), r = render([] {
        PluginState q = base();
        q.params[AdSpectrumA] = SpSquare;
        return q;
      }(), kOne, 20000, 256);
      ok = ok && tone(o.L, 4000, 16000, 220) > 10 * std::max(1e-7, tone(r.L, 4000, 16000, 220));
    }
    expect(ok, "harmonizer fills the even partials of a square (all positions)");
  }
  // Pluck on release: the top stays until the note is released.
  {
    PluginState s = base();
    set(s, AdPluck, 1), set(s, AdPluckTime, 0.05f), set(s, AdPluckRel, 1), set(s, AdRelease, 2);
    Out o = render(s, {{0, true, 1, 45, 1}, {20000, false, 1, 0, 0}}, 44100, 256);
    const double before = tone(o.L, 12000, 20000, 110 * 12), after = tone(o.L, 30000, 38000, 110 * 12);
    expect(before > 4 * after, "pluck only on release");
  }
  // Unison of 9 copies widens the stereo field.
  {
    PluginState s = base();
    set(s, AdUniOrder, 9), set(s, AdUniDetune, 30), set(s, AdUniSpread, 1), set(s, AdUniType, 2);
    Out o = render(s, kOne, 20000, 256);
    double diff = 0;
    for (size_t i = 4000; i < 16000; ++i) diff += std::fabs(o.L[i] - o.R[i]);
    expect(o.finite && diff > 1, "unison order 9 spreads copies");
  }
  // Mono portamento glides; poly porta keeps a voice per note.
  {
    PluginState s = base();
    set(s, AdSpectrumA, SpSine), set(s, AdPlayMode, ApMonoPorta), set(s, AdGlide, 0.2f);
    Out o = render(s, {{0, true, 1, 57, 1}, {11025, true, 2, 69, 1}}, 44100, 256);
    const double mid = zcHz(o.L, 11025 + 1500, 11025 + 4500), end = zcHz(o.L, 30000, 40000);
    expect(mid > 230 && mid < 420 && std::fabs(end - 440) < 10, "mono portamento glide");
  }
  // LFO special source: velocity drives the cutoff.
  {
    auto bright = [&](float vel) {
      PluginState s = base();
      set(s, AdFltType, AfLowPass), set(s, AdFltCutoff, 500), set(s, AdFltKey, 0), set(s, AdLfoShape, AlVelocity), set(s, AdLfoCutoff, 1);
      Out o = render(s, {{0, true, 1, 45, vel}}, 20000, 256);
      return tone(o.L, 4000, 16000, 110 * 10) / std::max(1e-9, tone(o.L, 4000, 16000, 110));
    };
    expect(bright(1.0f) > 3 * bright(0.1f), "velocity as LFO source moves the cutoff");
  }
  // Stereo tremolo.
  {
    PluginState s = base();
    set(s, AdTremolo, 1), set(s, AdLfoRate, 4);
    Out o = render(s, kOne, 30000, 256);
    double diff = 0;
    for (size_t i = 2000; i < 28000; ++i) diff += std::fabs(std::fabs(o.L[i]) - std::fabs(o.R[i]));
    expect(diff > 5, "stereo tremolo moves between the sides");
  }
  // Effects kinds + compression: finite, tails ring.
  {
    bool ok = true;
    for (int k = 0; k < 9; ++k) {
      PluginState s = base();
      set(s, AdFx + FxChorus, 1), set(s, AdChType, static_cast<float>(k));
      set(s, AdFx + FxDelay, 0.5f), set(s, AdDelType, static_cast<float>(k % 3));
      set(s, AdFx + FxReverb, 0.5f), set(s, AdRevType, static_cast<float>(k % 6));
      set(s, AdCompType, static_cast<float>(k % 5)), set(s, AdComp, 1);
      Out o = render(s, {{0, true, 1, 57, 1}, {8000, false, 1, 0, 0}}, 40000, 256);
      ok = ok && o.finite && o.peak < 4 && peakIn(o, 30000, 40000) > 1e-4f;
    }
    expect(ok, "chorus / delay / reverb kinds and compression types");
  }
  // Block-size invariance with modulation running.
  {
    PluginState s = presetState(*info, 1);
    set(s, AdTremolo, 0.5f), set(s, AdHzMix, 0.5f), set(s, AdUniOrder, 3);
    Out a = render(s, kOne, 30000, 1), b = render(s, kOne, 30000, 64), c = render(s, kOne, 30000, 999);
    float d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max({d, std::fabs(a.L[i] - b.L[i]), std::fabs(a.L[i] - c.L[i])});
    expect(d < 1e-4f, "block-size invariant");
  }
  // Parameter extremes.
  {
    PluginState mn = base(), mx = base();
    for (size_t i = 0; i < info->params.size(); ++i) {
      mn.params[i] = static_cast<float>(info->params[i].min);
      mx.params[i] = static_cast<float>(info->params[i].max);
    }
    Out a = render(mn, kOne, 20000, 256), b = render(mx, kOne, 20000, 256);
    expect(a.finite && b.finite && b.peak < 50, "parameter extremes finite");
  }

  // FL Studio's shipped states → Additive (presets + projects).
  if (testfl::flInstalled()) {
    for (int pass = 0; pass < 2; ++pass) {
      auto all = pass == 0 ? testfl::decodePresets("Harmless") : testfl::decodeProjects("Harmless");
      int total = 0, decoded = 0, finite = 0, sounding = 0;
      for (const auto& d : all) {
        ++total;
        if (!d.ok || d.state.type != "gl.additive") {
          std::printf("# not decoded: %s\n", d.where.c_str());
          continue;
        }
        ++decoded;
        testfl::maybeDump(d);
        Out o = render(d.state, {{0, true, 1, 60, 0.8f}, {8000, false, 1, 0, 0}}, 16000, 512);
        finite += o.finite && o.peak < 20;
        sounding += o.peak > 1e-3f;
        if (!o.finite || o.peak >= 20) std::printf("# unstable: %s (peak %g)\n", d.where.c_str(), o.peak);
        else if (o.peak <= 1e-3f) std::printf("# silent: %s\n", d.where.c_str());
      }
      std::printf("# Harmless %s: %d states, %d decoded, %d finite, %d sounding\n", pass ? "projects" : "presets", total, decoded, finite,
                  sounding);
      expect(total > 0 && decoded == total, pass ? "every project state decodes" : "every shipped preset decodes");
      expect(finite == decoded, "every decoded state renders finite");
      expect(sounding >= decoded * 95 / 100, "decoded states sound");
    }
  } else {
    std::printf("# FL Studio not installed: corpus checks skipped\n");
  }
  return finish();
}

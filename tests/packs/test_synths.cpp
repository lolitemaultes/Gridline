// Gridline — "synths" pack tests: Analog (gl.analog) and Wavetable (gl.wavetable).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/synths/synths.h"
#include "../Check.h"

using namespace gl;
using namespace gl::packs::synths;

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
};

Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block, double sr = 44100.0) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.f);
  o.R.assign(static_cast<size_t>(total), 0.f);
  auto g = createGenerator(st.type);
  if (!g) {
    o.finite = false;
    return o;
  }
  ChannelAux aux;
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 128;
  for (int pos = 0; pos < total; pos += block) {
    int n = std::min(block, total - pos);
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on)
          g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else
          g->noteOff(e.id, e.at - pos);
      }
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
  }
  return o;
}

float peakIn(const Out& o, int a, int b) {
  float p = 0;
  for (int i = std::max(0, a); i < std::min(b, static_cast<int>(o.L.size())); ++i)
    p = std::max({p, std::fabs(o.L[static_cast<size_t>(i)]), std::fabs(o.R[static_cast<size_t>(i)])});
  return p;
}

// Energy of frequency f (Goertzel) over [a, b) of the left channel.
double tone(const Out& o, int a, int b, double f, double sr = 44100) {
  double w = 2 * 3.14159265358979 * f / sr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (int i = a; i < b; ++i) {
    double win = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * (i - a) / (b - a));
    double s0 = o.L[static_cast<size_t>(i)] * win + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (b - a);
}

// Ratio (dB) of energy away from harmonics of f0 to total energy, via a DFT of n samples.
double inharmonicDb(const std::vector<float>& x, int start, int n, double f0, double sr) {
  std::vector<std::complex<double>> X(static_cast<size_t>(n / 2));
  double total = 0, bad = 0;
  for (int k = 1; k < n / 2; ++k) {
    std::complex<double> acc = 0;
    double w = -2 * 3.14159265358979 * k / n;
    for (int i = 0; i < n; ++i) {
      double win = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / n);
      acc += x[static_cast<size_t>(start + i)] * win * std::complex<double>(std::cos(w * i), std::sin(w * i));
    }
    double e = std::norm(acc), f = k * sr / n;
    total += e;
    double h = f / f0, dist = std::fabs(h - std::round(h)) * f0;
    if (dist > 4.0 * sr / n && f > f0 * 0.5) bad += e;
  }
  return 10 * std::log10(std::max(bad, 1e-30) / std::max(total, 1e-30));
}

PluginState withParams(const std::string& type, std::initializer_list<std::pair<int, float>> v) {
  PluginState s = defaultPluginState(type);
  for (auto [k, x] : v) s.params[static_cast<size_t>(k)] = x;
  normalizePluginState(s);
  return s;
}

std::vector<Ev> chordEvents() {
  return {{0, true, 1, 60, 0.9f}, {100, true, 2, 64, 0.7f}, {333, true, 3, 67, 1.0f}, {22050, false, 1, 0, 0},
          {22050, false, 2, 0, 0}, {26000, false, 3, 0, 0}};
}

const char* kTypes[2] = {"gl.analog", "gl.wavetable"};

void testCatalog() {
  for (const char* type : kTypes) {
    const PluginInfo* info = findPlugin(type);
    expect(info != nullptr, (std::string(type) + " is in the catalogue").c_str());
    if (!info) continue;
    int expected = std::string(type) == "gl.analog" ? AnCount : WtCount;
    expect(static_cast<int>(info->params.size()) == expected, (std::string(type) + " parameter count").c_str());
    expect(info->presets.size() >= 6 && info->presets.size() <= 12, (std::string(type) + " has 6-12 presets").c_str());
    expect(info->editorWidth >= 900, (std::string(type) + " has a wide editor").c_str());
    bool namesOk = true, inRange = true, defaultsOk = true;
    for (const auto& d : info->params) {
      if (d.name.empty()) namesOk = false;
      if (d.clamp(d.def) != d.def) defaultsOk = false;
    }
    for (const auto& p : info->presets)
      for (auto [k, v] : p.values)
        if (k < 0 || k >= expected || info->params[static_cast<size_t>(k)].clamp(v) != static_cast<double>(v)) {
          inRange = false;
          std::printf("# preset %s param %d (%s) value %g out of range\n", p.name.c_str(), k,
                      k >= 0 && k < expected ? info->params[static_cast<size_t>(k)].name.c_str() : "?", static_cast<double>(v));
        }
    expect(namesOk, (std::string(type) + " parameters are named").c_str());
    expect(defaultsOk, (std::string(type) + " defaults are in range").c_str());
    expect(inRange, (std::string(type) + " preset values are in range").c_str());
  }
}

void testPresetsSound() {
  for (const char* type : kTypes) {
    const PluginInfo* info = findPlugin(type);
    if (!info) continue;
    for (size_t p = 0; p <= info->presets.size(); ++p) {
      PluginState st = p == 0 ? defaultPluginState(type) : presetState(*info, p - 1);
      std::string name = p == 0 ? "default" : info->presets[p - 1].name;
      Out o = render(st, chordEvents(), 44100 * 2, 512);
      float early = peakIn(o, 0, 26000);
      bool ok = o.finite && early > 2e-3f && o.peak < 4.0f;
      if (!ok) std::printf("# %s / %s: finite=%d peak=%g early=%g\n", type, name.c_str(), o.finite, static_cast<double>(o.peak), static_cast<double>(early));
      expect(ok, (std::string(type) + " sounds: " + name).c_str());
    }
  }
}

void testSilenceAfterRelease() {
  for (const char* type : kTypes) {
    PluginState st = defaultPluginState(type);
    Out o = render(st, {{0, true, 1, 60, 1.f}, {4410, false, 1, 0, 0}}, 44100 * 3, 256);
    expect(o.finite && peakIn(o, 44100 * 2, 44100 * 3) == 0.f, (std::string(type) + " is silent after the release").c_str());
    Out none = render(st, {}, 8192, 256);
    expect(none.peak == 0.f, (std::string(type) + " is silent without notes").c_str());
  }
}

void testExtremes() {
  for (const char* type : kTypes) {
    const PluginInfo* info = findPlugin(type);
    if (!info) continue;
    std::vector<Ev> evs = {{0, true, 1, 0, 1.f}, {10, true, 2, 127, 1.f}, {20, true, 3, 60, 0.01f}, {8000, false, 1, 0, 0},
                           {9000, false, 2, 0, 0}, {9500, false, 3, 0, 0}};
    bool ok = true;
    for (int mode = 0; mode < 2; ++mode) {
      PluginState s = defaultPluginState(type);
      for (size_t k = 0; k < info->params.size(); ++k) s.params[k] = static_cast<float>(mode ? info->params[k].max : info->params[k].min);
      if (info->params[0].name == "Volume") s.params[0] = 1;
      Out o = render(s, evs, 20000, 300);
      if (!o.finite || o.peak > 50) {
        ok = false;
        std::printf("# %s all-%s: finite=%d peak=%g\n", type, mode ? "max" : "min", o.finite, static_cast<double>(o.peak));
      }
    }
    // Deterministic random states.
    uint32_t seed = 12345;
    auto rnd = [&]() {
      seed = seed * 1664525u + 1013904223u;
      return (seed >> 8) / 16777216.0;
    };
    for (int t = 0; t < 24; ++t) {
      PluginState s = defaultPluginState(type);
      for (size_t k = 0; k < info->params.size(); ++k) s.params[k] = static_cast<float>(info->params[k].fromNormalized(rnd()));
      normalizePluginState(s);
      Out o = render(s, evs, 12000, 97 + t * 13);
      if (!o.finite || o.peak > 50) {
        ok = false;
        std::printf("# %s random %d: finite=%d peak=%g\n", type, t, o.finite, static_cast<double>(o.peak));
      }
    }
    expect(ok, (std::string(type) + " finite output at parameter extremes").c_str());
  }
}

void testBlockInvariance() {
  struct Case {
    const char* type;
    int preset;  // -1 default
  };
  const Case cases[] = {{"gl.analog", -1}, {"gl.analog", 0}, {"gl.analog", 4}, {"gl.analog", 6}, {"gl.analog", 8},
                        {"gl.wavetable", -1}, {"gl.wavetable", 0}, {"gl.wavetable", 4}, {"gl.wavetable", 9}};
  for (const auto& c : cases) {
    const PluginInfo* info = findPlugin(c.type);
    if (!info) continue;
    PluginState st = c.preset < 0 ? defaultPluginState(c.type) : presetState(*info, static_cast<size_t>(c.preset));
    std::vector<Ev> evs = {{5, true, 1, 48, 0.8f}, {3000, true, 2, 55, 0.6f}, {7001, true, 3, 62, 1.f}, {20000, false, 1, 0, 0},
                           {21111, false, 2, 0, 0}, {30000, false, 3, 0, 0}};
    Out a = render(st, evs, 40000, 64), b = render(st, evs, 40000, 1000), d = render(st, evs, 40000, 37);
    double diff = 0;
    for (size_t i = 0; i < a.L.size(); ++i)
      diff = std::max({diff, static_cast<double>(std::fabs(a.L[i] - b.L[i])), static_cast<double>(std::fabs(a.R[i] - d.R[i]))});
    std::string name = std::string(c.type) + " block-size invariant (" + (c.preset < 0 ? "default" : info->presets[static_cast<size_t>(c.preset)].name) + ")";
    if (diff >= 1e-4) std::printf("# max diff %g\n", diff);
    expect(a.finite && diff < 1e-4 && a.peak > 1e-3f, name.c_str());
  }
}

void testVoiceStealing() {
  // Analog: two-voice polyphony, pure sines; the third note steals the oldest.
  PluginState s = withParams("gl.analog", {{AnVoices, 2}, {anOsc(0, OscWave), WaveSine}, {anOsc(1, OscOn), 0}, {AnFltMode, FltLp12},
                                           {AnCutoff, 20000}, {AnFltEnv, 0}, {AnKeyTrack, 0}, {AnAmpS, 1}, {AnAmpR, 0.01f}});
  Out o = render(s, {{0, true, 1, 45, 1.f}, {2000, true, 2, 69, 1.f}, {4000, true, 3, 81, 1.f}}, 16000, 128);
  double a = tone(o, 8000, 16000, 110), b = tone(o, 8000, 16000, 440), c = tone(o, 8000, 16000, 880);
  expect(o.finite && a < 0.02 * b && b > 0.01 && c > 0.01, "analog steals the oldest voice");
  PluginState w = withParams("gl.wavetable", {{WtVoices, 2}, {wtOsc(0, WoPos), 0}, {wtFlt(0, WfOn), 0}, {wtEnv(0, 2), 1},
                                              {wtMx(0, 0), WsNone}});
  Out q = render(w, {{0, true, 1, 45, 1.f}, {2000, true, 2, 69, 1.f}, {4000, true, 3, 81, 1.f}}, 16000, 128);
  double qa = tone(q, 8000, 16000, 110), qb = tone(q, 8000, 16000, 440);
  expect(q.finite && qa < 0.02 * qb && qb > 0.01, "wavetable steals the oldest voice");
  // Releasing a stolen note's id must not cut the voice that took it over.
  Out r = render(s, {{0, true, 1, 45, 1.f}, {2000, true, 2, 69, 1.f}, {4000, true, 3, 81, 1.f}, {6000, false, 1, 0, 0}}, 16000, 128);
  expect(tone(r, 9000, 16000, 880) > 0.01 && tone(r, 9000, 16000, 440) > 0.01, "noteOff of a stolen note leaves the others playing");
}

double zeroCrossHz(const Out& o, int a, int b, double sr = 44100) {
  int n = 0;
  double first = -1, last = -1;
  for (int i = a + 1; i < b; ++i)
    if (o.L[static_cast<size_t>(i - 1)] <= 0 && o.L[static_cast<size_t>(i)] > 0) {
      double t = i - 1 + o.L[static_cast<size_t>(i - 1)] / (o.L[static_cast<size_t>(i - 1)] - o.L[static_cast<size_t>(i)]);
      if (first < 0) first = t;
      last = t;
      ++n;
    }
  return n > 1 ? (n - 1) * sr / (last - first) : 0;
}

void testGlide() {
  for (const char* type : kTypes) {
    bool analog = std::string(type) == "gl.analog";
    PluginState s = analog ? withParams(type, {{AnPlayMode, PlayLegato}, {AnGlide, 0.25f}, {anOsc(0, OscWave), WaveSine}, {anOsc(1, OscOn), 0},
                                               {AnFltMode, FltLp12}, {AnCutoff, 20000}, {AnFltEnv, 0}, {AnAmpS, 1}})
                           : withParams(type, {{WtPlayMode, PlayLegato}, {WtGlide, 0.25f}, {wtOsc(0, WoPos), 0}, {wtOsc(0, WoRandPhase), 0},
                                               {wtFlt(0, WfOn), 0}, {wtEnv(0, 2), 1}, {wtMx(0, 0), WsNone}});
    Out o = render(s, {{0, true, 1, 60, 1.f}, {22050, true, 2, 72, 1.f}}, 44100 * 3, 512);
    double before = zeroCrossHz(o, 10000, 22000), mid = zeroCrossHz(o, 22050 + 2000, 22050 + 4500), after = zeroCrossHz(o, 100000, 130000);
    bool ok = std::fabs(before - 261.6) < 3 && mid > 280 && mid < 500 && std::fabs(after - 523.3) < 4;
    if (!ok) std::printf("# %s glide: before %.1f mid %.1f after %.1f\n", type, before, mid, after);
    expect(o.finite && ok, (std::string(type) + " legato glide reaches the new pitch gradually").c_str());
    // Mono mode: releasing the top note falls back to the held one.
    Out m = render(s, {{0, true, 1, 60, 1.f}, {11025, true, 2, 72, 1.f}, {44100, false, 2, 0, 0}}, 44100 * 3, 512);
    double back = zeroCrossHz(m, 100000, 130000);
    expect(std::fabs(back - 261.6) < 4, (std::string(type) + " mono returns to the held note").c_str());
  }
}

void testAliasing() {
  // Bright waves at a high note: energy between harmonics stays low.
  const double f0 = 440.0 * std::pow(2.0, (96 - 69) / 12.0);  // ~2093 Hz
  struct C {
    const char* name;
    PluginState st;
    double limitDb;
  };
  std::vector<C> cases;
  auto an = [](int wave) {
    return withParams("gl.analog", {{anOsc(0, OscWave), static_cast<float>(wave)}, {anOsc(1, OscOn), 0}, {AnFltMode, FltLp12}, {AnCutoff, 20000},
                                    {AnFltEnv, 0}, {AnKeyTrack, 0}, {AnAmpS, 1}, {AnVelAmp, 0}});
  };
  cases.push_back({"analog saw", an(WaveSaw), -30});
  cases.push_back({"analog pulse", an(WavePulse), -30});
  cases.push_back({"analog triangle", an(WaveTriangle), -45});
  for (int t = 0; t < TblCount; ++t)
    cases.push_back({wavetableNames()[t], withParams("gl.wavetable", {{wtOsc(0, WoTable), static_cast<float>(t)}, {wtOsc(0, WoPos), 0.6f},
                                                                      {wtFlt(0, WfOn), 0}, {wtEnv(0, 2), 1}, {wtMx(0, 0), WsNone}}),
                     -45});
  for (auto& c : cases) {
    Out o = render(c.st, {{0, true, 1, 96, 1.f}}, 12000, 512);
    double db = inharmonicDb(o.L, 4000, 4096, f0, 44100);
    std::printf("# %s inharmonic %.1f dB\n", c.name, db);
    expect(db < c.limitDb, (std::string("no audible aliasing: ") + c.name).c_str());
  }
}

void testArp() {
  const PluginInfo* info = findPlugin("gl.analog");
  size_t idx = 0;
  for (size_t i = 0; i < info->presets.size(); ++i)
    if (info->presets[i].name == "Arp Bubbles") idx = i;
  PluginState s = presetState(*info, idx);
  s.params[AnFxDrive + FxDlyOn] = 0;
  Out o = render(s, {{0, true, 1, 60, 1.f}, {0, true, 2, 64, 1.f}, {0, true, 3, 67, 1.f}, {44100, false, 1, 0, 0}, {44100, false, 2, 0, 0},
                     {44100, false, 3, 0, 0}},
                 44100 * 3, 333);
  // 1/16 at 128 BPM = 5168 samples per step: count note onsets via the envelope.
  int onsets = 0;
  double env = 0, prev = 0;
  for (int i = 0; i < 44100; ++i) {
    env = std::max(static_cast<double>(std::fabs(o.L[static_cast<size_t>(i)])), env * 0.999);
    if (i % 256 == 0) {
      if (env > prev * 1.6 && env > 0.01) ++onsets;
      prev = env;
    }
  }
  expect(o.finite && onsets >= 5, "arpeggiator steps through held notes");
  expect(peakIn(o, 44100 * 2, 44100 * 3) < 1e-6f, "arpeggiator stops when notes are released");
}

void testFxTail() {
  PluginState s = withParams("gl.analog", {{AnFxDrive + FxRevOn, 1}, {AnFxDrive + FxRevMix, 1}, {AnAmpR, 0.01f}});
  auto g = createGenerator("gl.analog");
  ChannelAux aux;
  ProcessContext ctx;
  std::vector<float> L(4410), R(4410);
  g->noteOn(1, 60, 1, 0);
  g->process(s, aux, ctx, L.data(), R.data(), 4410);
  g->noteOff(1, 0);
  for (int i = 0; i < 4; ++i) g->process(s, aux, ctx, L.data(), R.data(), 4410);
  expect(g->active(), "reverb tail keeps the instrument active after release");
  g->kill();
  expect(!g->active(), "kill silences the instrument");
}

void testNormalize() {
  for (const char* type : kTypes) {
    const PluginInfo* info = findPlugin(type);
    PluginState s;
    s.type = type;
    s.params = {2.0f, 99.0f};  // old, short state with out-of-range values
    normalizePluginState(s);
    bool ok = s.params.size() == info->params.size() && s.params[0] == static_cast<float>(info->params[0].clamp(2.0)) &&
              s.params[1] == static_cast<float>(info->params[1].clamp(99.0)) &&
              s.params.back() == static_cast<float>(info->params.back().def);
    expect(ok, (std::string(type) + " normalizePluginState pads and clamps").c_str());
    PluginState l = defaultPluginState(type);
    l.params.resize(l.params.size() + 5, 3.f);
    normalizePluginState(l);
    expect(l.params.size() == info->params.size(), (std::string(type) + " normalizePluginState truncates").c_str());
    Out o = render(s, chordEvents(), 30000, 512);
    expect(o.finite, (std::string(type) + " plays a normalized old state").c_str());
  }
}

void testTables() {
  bool finite = true, bounded = true, limited = true;
  for (int t = 0; t < TblCount; ++t)
    for (int f = 0; f < kWtFrames; ++f)
      for (int L = 0; L < kWtLevels; ++L) {
        const float* x = wavetableFrame(t, f, L);
        for (int i = 0; i < kWtSize; ++i) {
          if (!std::isfinite(x[i])) finite = false;
          if (std::fabs(x[i]) > 1.5f) bounded = false;
        }
        if (L == 4 && f == kWtFrames / 2) {  // level 4 keeps 64 harmonics: nothing at harmonic 100
          std::complex<double> acc = 0;
          for (int i = 0; i < kWtSize; ++i) acc += static_cast<double>(x[i]) * std::polar(1.0, -2 * 3.14159265358979 * 100 * i / kWtSize);
          if (std::abs(acc) / kWtSize > 1e-5) limited = false;
        }
      }
  expect(finite, "wavetables are finite");
  expect(bounded, "wavetables are normalised");
  expect(limited, "wavetable mip levels are band-limited");
}

// Informational: realtime factor of the heaviest factory sounds (6 held notes).
void reportCpu() {
  struct C {
    const char* type;
    const char* preset;
  };
  const C cases[] = {{"gl.analog", "Trance Stack"}, {"gl.wavetable", "Hyper Saw"}};
  for (const auto& c : cases) {
    const PluginInfo* info = findPlugin(c.type);
    size_t idx = 0;
    for (size_t i = 0; i < info->presets.size(); ++i)
      if (info->presets[i].name == c.preset) idx = i;
    PluginState st = presetState(*info, idx);
    std::vector<Ev> evs;
    for (int k = 0; k < 6; ++k) evs.push_back({0, true, static_cast<uint32_t>(k + 1), 48.0 + k * 5, 0.8f});
    auto t0 = std::chrono::steady_clock::now();
    Out o = render(st, evs, 44100 * 2, 512);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("# cpu: %s / %s, 6 voices: %.1fx realtime\n", c.type, c.preset, 2.0 / std::max(secs, 1e-9));
    expect(o.finite, (std::string(c.type) + " heavy preset renders").c_str());
  }
}

}  // namespace

int main() {
  reportCpu();
  testCatalog();
  testPresetsSound();
  testSilenceAfterRelease();
  testExtremes();
  testBlockInvariance();
  testVoiceStealing();
  testGlide();
  testAliasing();
  testArp();
  testFxTail();
  testNormalize();
  testTables();
  return finish();
}

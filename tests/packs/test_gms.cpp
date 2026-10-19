// Gridline — J-4 tests: Groove Synth (gl.gms) and the decode of FL Studio's
// shipped groove-machine synth states (.gmsynth presets + projects).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpGen.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/synths/gms.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;
using namespace gl::packs::synths::gms;

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
  PluginState s = defaultPluginState("gl.gms");
  s.params[GCutoff] = 20000;
  s.params[GReso] = 0;
  return s;
}
void set(PluginState& s, int i, float v) { s.params[static_cast<size_t>(i)] = v; }
const std::vector<Ev> kOne = {{0, true, 1, 57, 0.9f}, {22050, false, 1, 0, 0}};  // A3 = 220 Hz

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.gms");
  expect(info != nullptr, "catalogue entry");
  if (!info) return finish();
  expect(static_cast<int>(info->params.size()) == GCount, "parameter count");
  expect(info->presets.size() >= 2 && info->presets.size() <= 6, "2-6 factory presets");
  bool sane = true;
  for (const auto& p : info->params) sane = sane && !p.name.empty() && p.def >= p.min && p.def <= p.max;
  expect(sane, "every parameter named, defaults in range");
  {
    PluginState s;
    s.type = "gl.gms";
    s.params = {0, 0};
    normalizePluginState(s);
    expect(static_cast<int>(s.params.size()) == GCount, "normalizePluginState pads");
  }
  // Default: a saw at the note pitch; release ends.
  {
    Out o = render(base(), kOne, 44100 * 2, 256);
    expect(o.finite && o.peak > 0.05f, "default patch sounds");
    expect(std::fabs(zcHz(o.L, 4000, 20000) - 220) < 4, "plays the note pitch");
    expect(tone(o.L, 4000, 20000, 440) > 0.2 * tone(o.L, 4000, 20000, 220), "saw has harmonics");
    expect(peakIn(o, 70000, 88200) < 1e-4f && !o.activeAtEnd, "release ends in silence");
  }
  // Built-in waves: sine has no harmonics; band-limited at high notes.
  {
    PluginState s = base();
    set(s, osc(0, OsWave), WvSine);
    Out o = render(s, kOne, 20000, 256);
    expect(tone(o.L, 4000, 16000, 440) < 0.01 * tone(o.L, 4000, 16000, 220), "sine wave is pure");
    PluginState h = base();
    Out hi = render(h, {{0, true, 1, 115, 1}}, 20000, 256);  // ~ 6.2 kHz saw
    bool ok = hi.finite;
    expect(ok && tone(hi.L, 4000, 16000, 44100 - 3 * 6271.9) < 0.05 * tone(hi.L, 4000, 16000, 6271.9), "high saw stays band-limited");
    bool all = true;
    for (int w = 0; w < WvFile; ++w) {
      PluginState q = base();
      set(q, osc(0, OsWave), static_cast<float>(w));
      Out r = render(q, kOne, 8000, 256);
      all = all && r.finite && r.peak > 0.01f && r.peak < 4;
    }
    expect(all, "every built-in wave sounds");
  }
  // Wave file in the data (a written single-cycle WAV: square).
  {
    const std::string path = "/tmp/gl_gms_test_cycle.wav";
    {
      std::ofstream f(path, std::ios::binary);
      const int n = 256;
      auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
      auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
      f.write("RIFF", 4), u32(36 + n * 2), f.write("WAVE", 4), f.write("fmt ", 4), u32(16), u16(1), u16(1), u32(44100), u32(88200), u16(2),
          u16(16), f.write("data", 4), u32(n * 2);
      for (int i = 0; i < n; ++i) u16(static_cast<uint16_t>(static_cast<int16_t>(i < n / 2 ? 20000 : -20000)));
    }
    PluginState s = base();
    set(s, osc(0, OsWave), WvFile);
    WaveFiles w;
    w.path[0] = path;
    s.data = serializeWaveFiles(w);
    Out o = render(s, kOne, 20000, 256);
    expect(o.finite && tone(o.L, 4000, 16000, 440) < 0.05 * tone(o.L, 4000, 16000, 660), "file wave (square) has odd harmonics only");
    WaveFiles r = parseWaveFiles(s.data);
    expect(r.path[0] == path && r.path[1].empty(), "wave-file data round trip");
    expect(parseWaveFiles("junk").path[0].empty() && parseWaveFiles("gms 1\nw 9 x\nw 1\n").path[1].empty(), "malformed wave data ignored");
  }
  // Osc 2 → osc 1 modulation kinds.
  {
    auto side = [&](int kind) {
      PluginState s = base();
      set(s, osc(0, OsWave), WvSine), set(s, osc(1, OsWave), WvSine), set(s, osc(1, OsSemi), 7);
      set(s, GModKind, static_cast<float>(kind)), set(s, GModAmt, 0.3f);
      Out o = render(s, kOne, 20000, 256);
      return o;
    };
    Out fm = side(GkFm), am = side(GkAm), sy = side(GkSync);
    const double f2 = 220 * std::pow(2.0, 7 / 12.0);
    expect(tone(fm.L, 4000, 16000, 220 + 2 * f2) > 1e-3, "FM adds sidebands");
    expect(tone(am.L, 4000, 16000, f2 + 220) > 1e-3, "AM adds sum frequencies");
    expect(sy.finite && tone(sy.L, 4000, 16000, 440) > 1e-3, "hard sync adds harmonics to a sine");
  }
  // Filter: low pass removes the top; high pass removes the bottom; band pass.
  {
    PluginState lp = base(), hp = base(), bp = base();
    set(lp, GCutoff, 400), set(hp, GCutoff, 2000), set(hp, GFltType, GfHighPass), set(bp, GCutoff, 1000), set(bp, GFltType, GfBandPass);
    Out a = render(lp, kOne, 20000, 256), b = render(hp, kOne, 20000, 256), c = render(bp, kOne, 20000, 256), r = render(base(), kOne, 20000, 256);
    expect(tone(a.L, 4000, 16000, 220 * 12) < 0.05 * tone(r.L, 4000, 16000, 220 * 12), "ladder low pass");
    expect(tone(b.L, 4000, 16000, 220) < 0.1 * tone(r.L, 4000, 16000, 220), "high pass");
    expect(c.finite && tone(c.L, 4000, 16000, 220) < 0.5 * tone(r.L, 4000, 16000, 220), "band pass");
  }
  // Envelope 1 → pitch, LFO 2 → pitch.
  {
    PluginState s = base();
    set(s, osc(0, OsWave), WvSine), set(s, eg(0, EgDest), EdPitch), set(s, eg(0, EgAmt), 1.0f), set(s, eg(0, EgDec), 0.5f);
    Out o = render(s, kOne, 30000, 256);
    expect(zcHz(o.L, 100, 1000) > 450 && std::fabs(zcHz(o.L, 20000, 30000) - 220) < 6, "envelope to pitch sweeps down");
    PluginState l = base();
    set(l, osc(0, OsWave), WvSine), set(l, lfo(1, LfAmt), 1), set(l, lfo(1, LfDest), LdPitch), set(l, lfo(1, LfShape), LsSquare),
        set(l, lfo(1, LfRate), 2);
    Out q = render(l, kOne, 22050, 256);
    const double a = zcHz(q.L, 1000, 9000), b = zcHz(q.L, 12000, 20000);
    expect(std::fabs(a - b) > 100, "square LFO to pitch jumps an octave");
  }
  // Unison: stereo spread.
  {
    PluginState s = base();
    set(s, GUniVoices, 8), set(s, GUniDetune, 0.5f), set(s, GUniStereo, 1);
    Out o = render(s, kOne, 20000, 256);
    double diff = 0;
    for (size_t i = 4000; i < 16000; ++i) diff += std::fabs(o.L[i] - o.R[i]);
    expect(o.finite && diff > 1, "unison spreads voices");
  }
  // Mono slide.
  {
    PluginState s = base();
    set(s, osc(0, OsWave), WvSine), set(s, GMono, 1), set(s, GSlide, 1), set(s, GSlideTime, 0.3f);
    Out o = render(s, {{0, true, 1, 45, 1}, {11025, true, 2, 57, 1}}, 44100, 256);
    const double mid = zcHz(o.L, 11025 + 500, 11025 + 3000), end = zcHz(o.L, 33000, 44000);
    expect(mid > 112 && mid < 210 && std::fabs(end - 220) < 5, "mono slide glides");
  }
  // Every effect slot: finite, audible, tails ring for delay / reverb.
  {
    bool ok = true;
    for (int k = 0; k < kFxSlots; ++k) {
      PluginState s = base();
      set(s, fx(k, FfOn), 1), set(s, fx(k, FfX), 0.6f), set(s, fx(k, FfY), 0.4f), set(s, fx(k, FfLfo), 0.5f);
      Out o = render(s, {{0, true, 1, 57, 1}, {8000, false, 1, 0, 0}}, 40000, 256);
      const bool fine = o.finite && o.peak > 1e-3f && o.peak < 8;
      if (!fine) std::printf("# fx slot %d: peak %g\n", k, o.peak);
      ok = ok && fine;
      if (k == FxDelay || k == FxReverb) ok = ok && peakIn(o, 25000, 40000) > 1e-4f;
    }
    expect(ok, "effect slots (and delay / reverb tails)");
  }
  // Master EQ low shelf.
  {
    PluginState s = base();
    set(s, GEqOn, 1), set(s, GEq1, -12);
    Out o = render(s, {{0, true, 1, 33, 1}}, 20000, 256), r = render(base(), {{0, true, 1, 33, 1}}, 20000, 256);
    expect(tone(o.L, 4000, 16000, 55) < 0.5 * tone(r.L, 4000, 16000, 55), "EQ low shelf cut");
  }
  // Block-size invariance.
  {
    PluginState s = presetState(*info, 1);
    Out a = render(s, kOne, 30000, 1), b = render(s, kOne, 30000, 64), c = render(s, kOne, 30000, 999);
    float d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max({d, std::fabs(a.L[i] - b.L[i]), std::fabs(a.L[i] - c.L[i])});
    expect(d < 1e-4f, "block-size invariant");
  }
  // Presets + extremes + 96 kHz.
  for (size_t i = 0; i < info->presets.size(); ++i) {
    Out o = render(presetState(*info, i), {{0, true, 1, 60, 0.8f}, {20000, false, 1, 0, 0}}, 50000, 512);
    char name[96];
    std::snprintf(name, sizeof name, "preset '%s' sounds (peak %.2f)", info->presets[i].name.c_str(), o.peak);
    expect(o.finite && o.peak > 0.02f && o.peak < 4.f, name);
  }
  {
    PluginState mn = base(), mx = base();
    for (size_t i = 0; i < info->params.size(); ++i) {
      mn.params[i] = static_cast<float>(info->params[i].min);
      mx.params[i] = static_cast<float>(info->params[i].max);
    }
    Out a = render(mn, kOne, 30000, 256), b = render(mx, kOne, 30000, 256);
    expect(a.finite && b.finite && b.peak < 1e3f, "parameter extremes finite");
    Out c = render(presetState(*info, 2), kOne, 30000, 300, 96000);
    expect(c.finite && c.peak > 0.02f, "96 kHz");
  }
  // Malformed states are refused.
  {
    flp::File f;
    Song song;
    flp::Report rep;
    flp::Context c(f, song, rep);
    std::vector<uint8_t> junk(100, 7);
    flp::Event e;
    e.id = 213;
    e.data = junk.data();
    e.size = static_cast<uint32_t>(junk.size());
    flp::PluginMapping m;
    expect(!flp::gen::decodeGmsFull(c, {e}, m), "junk state refused");
    std::vector<uint8_t> shortState(0x300, 0);
    shortState[0] = '4', shortState[1] = 'n', shortState[2] = 'y', shortState[3] = 'S';
    e.data = shortState.data();
    e.size = static_cast<uint32_t>(shortState.size());
    expect(!flp::gen::decodeGmsFull(c, {e}, m), "truncated state refused");
  }
  // FL Studio's shipped states.
  if (testfl::flInstalled()) {
    for (int pass = 0; pass < 2; ++pass) {
      auto all = pass == 0 ? testfl::decodePresets("GMS", {".gmsynth"}) : testfl::decodeProjects("GMS");
      int total = 0, decoded = 0, finite = 0, sounding = 0, files = 0;
      for (const auto& d : all) {
        ++total;
        if (!d.ok || d.state.type != "gl.gms") {
          std::printf("# not decoded: %s\n", d.where.c_str());
          continue;
        }
        ++decoded;
        testfl::maybeDump(d);
        files += d.state.data.find("Synth Waves") != std::string::npos;
        Out o = render(d.state, {{0, true, 1, 60, 0.8f}, {8000, false, 1, 0, 0}}, 16000, 512);
        finite += o.finite && o.peak < 20;
        sounding += o.peak > 1e-3f;
        if (!o.finite || o.peak >= 20) std::printf("# unstable: %s (peak %g)\n", d.where.c_str(), o.peak);
        else if (o.peak <= 1e-3f) std::printf("# silent: %s\n", d.where.c_str());
      }
      std::printf("# GMS %s: %d states, %d decoded, %d finite, %d sounding, %d using FL's wave files\n", pass ? "projects" : "presets", total,
                  decoded, finite, sounding, files);
      expect(total > 0 && decoded == total, pass ? "every project state decodes" : "every shipped preset decodes");
      expect(finite == decoded, "every decoded state renders finite");
      expect(sounding >= decoded * 95 / 100, "decoded states sound");
    }
  } else {
    std::printf("# FL Studio not installed: corpus checks skipped\n");
  }
  return finish();
}

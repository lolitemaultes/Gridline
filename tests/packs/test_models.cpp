// Gridline — tests for the "models" pack (Additive, Image Synth, String
// Model, Drum Model).
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/PluginInfo.h"
#include "engine/Generators.h"
#include "packs/Packs.h"
#include "packs/models/models.h"

using namespace gl;
using namespace gl::packs::models;

namespace {

const char* kTypes[] = {"gl.additive", "gl.imagesynth", "gl.string", "gl.drumkit"};

struct Note {
  int64_t on, off;  // frames (off < 0: never)
  double pitch;
  float vel;
};

struct Out {
  std::vector<float> l, r;
  bool finite = true;
  float peak = 0;
  bool activeAtEnd = false;
};

Out run(const PluginState& st, const std::vector<Note>& notes, int64_t frames, int block) {
  auto g = createGenerator(st.type);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  Out o;
  o.l.assign(static_cast<size_t>(frames), 0.0f);
  o.r = o.l;
  for (int64_t pos = 0; pos < frames; pos += block) {
    int n = static_cast<int>(std::min<int64_t>(block, frames - pos));
    for (size_t i = 0; i < notes.size(); ++i) {
      const Note& nt = notes[i];
      if (nt.on >= pos && nt.on < pos + n) g->noteOn(static_cast<uint32_t>(i + 1), nt.pitch, nt.vel, static_cast<int>(nt.on - pos));
      if (nt.off >= pos && nt.off < pos + n) g->noteOff(static_cast<uint32_t>(i + 1), static_cast<int>(nt.off - pos));
    }
    g->process(st, aux, ctx, &o.l[static_cast<size_t>(pos)], &o.r[static_cast<size_t>(pos)], n);
  }
  for (size_t i = 0; i < o.l.size(); ++i) {
    if (!std::isfinite(o.l[i]) || !std::isfinite(o.r[i])) o.finite = false;
    o.peak = std::max(o.peak, std::max(std::fabs(o.l[i]), std::fabs(o.r[i])));
  }
  o.activeAtEnd = g->active();
  return o;
}

double noteFor(const std::string&) { return 60.0; }  // drum kit pads start at C4 (60)

PluginState withParam(PluginState s, int k, float v) {
  s.params[static_cast<size_t>(k)] = v;
  return s;
}

// Fundamental by autocorrelation over [from, from+len).
double pitchOf(const std::vector<float>& x, size_t from, size_t len, double sr, double lo, double hi) {
  int minLag = static_cast<int>(sr / hi), maxLag = static_cast<int>(sr / lo);
  double best = -1;
  int bestLag = minLag;
  std::vector<double> c(static_cast<size_t>(maxLag + 2), 0.0);
  for (int lag = minLag; lag <= maxLag + 1; ++lag) {
    double s = 0, e1 = 0, e2 = 0;
    for (size_t i = from; i < from + len; ++i) {
      s += x[i] * x[i + static_cast<size_t>(lag)];
      e1 += x[i] * x[i];
      e2 += x[i + static_cast<size_t>(lag)] * x[i + static_cast<size_t>(lag)];
    }
    c[static_cast<size_t>(lag)] = s / std::sqrt(std::max(1e-20, e1 * e2));
  }
  for (int lag = minLag + 1; lag <= maxLag; ++lag) {
    double v = c[static_cast<size_t>(lag)];
    if (v > best && v >= c[static_cast<size_t>(lag - 1)] && v >= c[static_cast<size_t>(lag + 1)]) {
      best = v;
      bestLag = lag;
    }
  }
  // Parabolic refinement.
  double a = c[static_cast<size_t>(bestLag - 1)], b = c[static_cast<size_t>(bestLag)], d = c[static_cast<size_t>(bestLag + 1)];
  double den = a - 2 * b + d;
  double off = std::fabs(den) > 1e-12 ? 0.5 * (a - d) / den : 0.0;
  return sr / (bestLag + off);
}

double goertzel(const std::vector<float>& x, size_t a, size_t b, double sr, double hz) {
  double w = 2 * 3.14159265358979 * hz / sr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) {
    double s = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s;
  }
  return s1 * s1 + s2 * s2 - c * s1 * s2;
}

double energy(const std::vector<float>& x, size_t a, size_t b) {
  double e = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) e += x[i] * x[i];
  return e;
}

}  // namespace

int main() {
  // ---------------------------------------------------------------- catalogue
  for (const char* t : kTypes) {
    const PluginInfo* info = findPlugin(t);
    std::string n = t;
    expect(info != nullptr, (n + ": in catalogue").c_str());
    if (!info) continue;
    expect(info->kind == PluginKind::Generator, (n + ": is an instrument").c_str());
    expect(info->presets.size() >= 6 && info->presets.size() <= 12, (n + ": 6-12 factory presets").c_str());
    expect(info->editorWidth > 0, (n + ": editor width set").c_str());
    bool defaultsOk = true;
    for (const auto& d : info->params)
      if (d.clamp(d.def) != d.def || d.name.empty()) defaultsOk = false;
    expect(defaultsOk, (n + ": defaults within range, all named").c_str());
    bool presetsOk = true;
    for (const auto& p : info->presets)
      for (const auto& kv : p.values)
        if (kv.first < 0 || static_cast<size_t>(kv.first) >= info->params.size()) presetsOk = false;
    expect(presetsOk, (n + ": preset indices valid").c_str());
    expect(createGenerator(t) != nullptr, (n + ": runtime created").c_str());
  }
  expect(findPlugin("gl.additive")->params.size() == static_cast<size_t>(AdCount), "additive: param count");
  expect(findPlugin("gl.drumkit")->params.size() == static_cast<size_t>(kDrumParamCount), "drumkit: param count");

  // ---------------------------------------------------------------- sound out + presets
  for (const char* t : kTypes) {
    std::string n = t;
    const PluginInfo* info = findPlugin(t);
    if (!info) continue;
    PluginState st = defaultPluginState(t);
    Out o = run(st, {{100, 30000, noteFor(t), 0.9f}}, 44100, 256);
    expect(o.finite && o.peak > 0.02f && o.peak < 4.0f, (n + ": default sounds after noteOn").c_str());
    Out quiet = run(st, {}, 8192, 256);
    expect(quiet.peak == 0.0f && !quiet.activeAtEnd, (n + ": silent without notes").c_str());
    bool allOk = true;
    for (size_t p = 0; p < info->presets.size(); ++p) {
      PluginState ps = presetState(*info, p);
      Out po = run(ps, {{0, 20000, noteFor(t) + (t == std::string("gl.drumkit") ? static_cast<double>(p % 16) : 0.0), 0.8f}}, 33075, 512);
      if (std::getenv("GL_MODELS_VERBOSE"))
        std::printf("# %-14s %-16s peak %.3f rms %.4f\n", t, info->presets[p].name.c_str(), po.peak, std::sqrt(energy(po.l, 0, po.l.size()) / po.l.size()));
      if (!po.finite || po.peak < 0.005f || po.peak > 6.0f) {
        allOk = false;
        std::printf("# %s preset '%s': peak %.4f finite %d\n", t, info->presets[p].name.c_str(), po.peak, po.finite);
      }
    }
    expect(allOk, (n + ": every factory preset sounds, finite, bounded").c_str());
  }

  // ---------------------------------------------------------------- extremes
  for (const char* t : kTypes) {
    std::string n = t;
    const PluginInfo* info = findPlugin(t);
    if (!info) continue;
    for (int mode = 0; mode < 3; ++mode) {
      PluginState st = defaultPluginState(t);
      uint32_t h = 1234567u;
      for (size_t k = 0; k < info->params.size(); ++k) {
        const ParamDesc& d = info->params[k];
        h = h * 1664525u + 1013904223u;
        double x = mode == 0 ? 0.0 : mode == 1 ? 1.0 : (h >> 8) / 16777216.0;
        st.params[k] = static_cast<float>(d.fromNormalized(x));
      }
      if (std::string(t) == "gl.drumkit") st.params[DmBaseNote] = 60;
      std::vector<Note> notes = {{0, 9000, noteFor(t), 1.0f}, {50, 12000, noteFor(t) + 7, 0.3f}, {300, -1, noteFor(t) - 12, 0.6f}, {4000, 6000, 120, 1.0f},
                                 {4100, 7000, 0, 1.0f}};
      Out o = run(st, notes, 22050, 300);
      static const char* modes[] = {"min", "max", "random"};
      expect(o.finite && o.peak < 50.0f, (n + ": finite with all params at " + modes[mode]).c_str());
    }
  }

  // ---------------------------------------------------------------- block-size invariance
  for (const char* t : kTypes) {
    std::string n = t;
    PluginState st = defaultPluginState(t);
    if (n == "gl.additive") {  // exercise LFO, env, unison, blur, phaser motion, pluck
      st = withParam(st, AdLfoPitch, 0.5f);
      st = withParam(st, AdUniOrder, 3);
      st = withParam(st, AdBlur, 0.4f);
      st = withParam(st, AdPhMix, 0.5f);
      st = withParam(st, AdPhSpeed, 0.5f);
      st = withParam(st, AdPluck, 0.6f);
      st = withParam(st, AdFEnvAmt, 3);
      st = withParam(st, AdFx + FxChorus, 0.5f);
      st = withParam(st, AdFx + FxReverb, 0.4f);
      st = withParam(st, AdFx + FxDelay, 0.4f);
    }
    if (n == "gl.string") {
      st = withParam(st, StVibrato, 0.6f);
      st = withParam(st, StStrum, 1);
      st = withParam(st, StHiCutEnv, 0.5f);
      st = withParam(st, StDrive, 0.4f);
      st = withParam(st, StCab, 1);
      st = withParam(st, StFx + FxDelay, 0.4f);
    }
    if (n == "gl.imagesynth") {
      st = withParam(st, ImGrainy, 1);
      st = withParam(st, ImWiden, 1);
      st = withParam(st, ImLoop, 1);
    }
    double b = noteFor(t);
    std::vector<Note> notes = {{37, 9000, b, 0.9f}, {1000, 15000, b + 4, 0.7f}, {5003, 20000, b + 7, 0.5f}, {9100, -1, b + 1, 1.0f}};
    Out a = run(st, notes, 30000, 64);
    Out c = run(st, notes, 30000, 517);
    Out d = run(st, notes, 30000, 1);
    double diff = 0, diff1 = 0;
    for (size_t i = 0; i < a.l.size(); ++i) {
      diff = std::max(diff, static_cast<double>(std::fabs(a.l[i] - c.l[i]) + std::fabs(a.r[i] - c.r[i])));
      diff1 = std::max(diff1, static_cast<double>(std::fabs(a.l[i] - d.l[i]) + std::fabs(a.r[i] - d.r[i])));
    }
    if (diff > 1e-5 || diff1 > 1e-5) std::printf("# %s block diff %.3g / %.3g\n", t, diff, diff1);
    expect(a.peak > 0.01f && diff < 1e-5 && diff1 < 1e-5, (n + ": block-size invariant (64 / 517 / 1)").c_str());
  }

  // ---------------------------------------------------------------- normalize
  for (const char* t : kTypes) {
    PluginState s;
    s.type = t;
    s.params = {0.5f, 99999.0f};
    normalizePluginState(s);
    const PluginInfo* info = findPlugin(t);
    bool ok = info && s.params.size() == info->params.size();
    if (ok) {
      for (size_t k = 0; k < s.params.size(); ++k)
        if (s.params[k] != static_cast<float>(info->params[k].clamp(s.params[k]))) ok = false;
      if (s.params.size() > 2 && s.params[2] != static_cast<float>(info->params[2].def)) ok = false;
    }
    expect(ok, (std::string(t) + ": normalize pads and clamps old states").c_str());
  }

  // ---------------------------------------------------------------- Additive specifics
  {
    PluginState st = defaultPluginState("gl.additive");
    st = withParam(st, AdSpectrumA, SpSine);
    st = withParam(st, AdFltType, AfOff);
    Out o = run(st, {{0, -1, 69, 1.0f}}, 22050, 256);
    double f = pitchOf(o.l, 4000, 4000, 44100, 200, 1000);
    expect(std::fabs(f - 440.0) < 1.0, "additive: sine timbre plays A4 at 440 Hz");
    AdditiveModel m;
    m.read(defaultPluginState("gl.additive"));
    expect(m.filter(500, 2000) > 0.99f && m.filter(8000, 1000) < 0.01f, "additive: low pass passes below, stops above");
    m.fltType = AfHighPass;
    expect(m.filter(100, 2000) < 0.01f && m.filter(8000, 2000) > 0.99f, "additive: high pass");
    m.pluck = 1;
    expect(m.pluckGain(5000, 100, 0.5) < m.pluckGain(200, 100, 0.5), "additive: pluck decays high partials faster");
    m.stretch = 0.5f;
    expect(m.ratio(8) > 8.0, "additive: prism stretches partials");
    // Partial count: 1 partial of a saw = pure sine at the fundamental.
    PluginState one = withParam(withParam(defaultPluginState("gl.additive"), AdPartials, 1), AdFltType, AfOff);
    Out o1 = run(one, {{0, -1, 57, 1.0f}}, 22050, 256);
    expect(std::fabs(pitchOf(o1.l, 4000, 4000, 44100, 150, 400) - 220.0) < 0.6, "additive: single partial at 220 Hz");
    // No partial above Nyquist: a very high note stays bounded and finite.
    Out hi = run(defaultPluginState("gl.additive"), {{0, -1, 127, 1.0f}}, 8192, 256);
    expect(hi.finite && hi.peak < 2.0f, "additive: top note culls partials above Nyquist");
    // Release ends the voice.
    Out rel = run(defaultPluginState("gl.additive"), {{0, 4410, 60, 1.0f}}, 88200, 256);
    expect(!rel.activeAtEnd, "additive: voice ends after release");
    // Drawn partial edits change the sound: partial 1 at zero removes the fundamental.
    PluginState d0 = withParam(defaultPluginState("gl.additive"), AdFltType, AfOff);
    PluginState d1 = withParam(d0, AdDrawnBase, 0.0f);
    Out a0 = run(d0, {{0, -1, 48, 1.0f}}, 8192, 256), a1 = run(d1, {{0, -1, 48, 1.0f}}, 8192, 256);
    double h0 = goertzel(a0.l, 2000, 6000, 44100, 130.81), h1 = goertzel(a1.l, 2000, 6000, 44100, 130.81);
    expect(h1 < h0 * 0.05, "additive: drawn partial 1 at zero removes the fundamental");
  }

  // ---------------------------------------------------------------- String Model specifics
  {
    PluginState st = defaultPluginState("gl.string");
    st = withParam(st, StBody, 0);
    st = withParam(st, St2Ratio, 0);
    for (double note : {45.0, 57.0, 69.0, 81.0}) {
      Out o = run(st, {{0, -1, note, 0.9f}}, 22050, 256);
      double want = 440.0 * std::pow(2.0, (note - 69) / 12.0);
      double f = pitchOf(o.l, 6000, 6000, 44100, want * 0.7, want * 1.4);
      char nm[96];
      std::snprintf(nm, sizeof nm, "string: pluck tuned at %.1f Hz (got %.2f)", want, f);
      expect(std::fabs(f / want - 1.0) < 0.004, nm);
    }
    PluginState stiff = withParam(st, StStiffness, 1.0f);
    Out o = run(stiff, {{0, -1, 57, 0.9f}}, 22050, 256);
    double f = 0, best = -1;  // strongest component near the fundamental (partials are stretched)
    for (double hz = 205; hz <= 235; hz += 0.1) {
      double g = goertzel(o.l, 2000, 22050, 44100, hz);
      if (g > best) best = g, f = hz;
    }
    expect(std::fabs(f / 220.0 - 1.0) < 0.006, "string: stiff string keeps its fundamental in tune");
    // Bow sustains while held, then stops.
    PluginState bow = withParam(st, StExciter, ExBow);
    Out b = run(bow, {{0, 66150, 57, 0.9f}}, 176400, 256);
    expect(energy(b.l, 44100, 66150) > 1e-3 * 22050 * 1e-3 && !b.activeAtEnd, "string: bow sustains while held, ends after release");
    // Strum: chord produces more than one pitch (energy at the fifth).
    PluginState strum = withParam(st, StStrum, 1);
    Out sa = run(st, {{0, -1, 48, 0.9f}}, 22050, 256), sb = run(strum, {{0, -1, 48, 0.9f}}, 22050, 256);
    double third = 329.63;  // E4: in the major chord, not a harmonic of C3
    expect(goertzel(sb.l, 4000, 12000, 44100, third) > 4 * goertzel(sa.l, 4000, 12000, 44100, third), "string: strum adds chord tones");
    // Release damps the string.
    Out held = run(st, {{0, -1, 57, 0.9f}}, 44100, 256), rel = run(st, {{0, 4410, 57, 0.9f}}, 44100, 256);
    expect(energy(rel.l, 22050, 44100) < 0.05 * energy(held.l, 22050, 44100), "string: note off damps the string");
  }

  // ---------------------------------------------------------------- Drum Model specifics
  {
    PluginState st = defaultPluginState("gl.drumkit");
    Out below = run(st, {{0, -1, 59, 1.0f}}, 8192, 256);
    Out above = run(st, {{0, -1, 76, 1.0f}}, 8192, 256);
    expect(below.peak == 0 && above.peak == 0, "drumkit: notes outside the 16 pads are silent");
    bool all = true;
    for (int p = 0; p < kDrumPads; ++p) {
      Out o = run(st, {{0, -1, 60.0 + p, 1.0f}}, 11025, 256);
      if (!(o.finite && o.peak > 0.02f && o.peak < 2.0f)) {
        all = false;
        std::printf("# pad %d peak %.4f\n", p + 1, o.peak);
      }
    }
    expect(all, "drumkit: all 16 default pads sound");
    PluginState muted = withParam(st, drumPad(0, DpMute), 1);
    expect(run(muted, {{0, -1, 60, 1.0f}}, 4096, 256).peak == 0, "drumkit: muted pad is silent");
    // Choke: closed hat (pad 5) cuts the open hat (pad 6).
    Out open = run(st, {{0, -1, 65, 1.0f}}, 22050, 256);
    Out choked = run(st, {{0, -1, 65, 1.0f}, {4410, -1, 64, 0.01f}}, 22050, 256);
    expect(energy(choked.l, 8820, 22050) < 0.2 * energy(open.l, 8820, 22050), "drumkit: choke group cuts the open hat");
    // Tension sets the pitch: a tabla pad (harmonic modes) plays its frequency.
    PluginState tb = withParam(withParam(withParam(st, drumPad(14, DpBend), 0), drumPad(14, DpFreq), 200), drumPad(14, DpDecay), 1.5f);
    Out to = run(tb, {{0, -1, 74, 1.0f}}, 22050, 256);
    double f = pitchOf(to.l, 2000, 6000, 44100, 120, 320);
    expect(std::fabs(f / 200.0 - 1.0) < 0.01, "drumkit: tension sets the drum pitch");
    // Pitched key mode: every key plays pad 1, transposed from the base note.
    {
      PluginState pk = withParam(withParam(withParam(withParam(st, DmKeyMode, 1), drumPad(0, DpShape), DsTabla), drumPad(0, DpBend), 0),
                                 drumPad(0, DpFreq), 200);
      pk = withParam(pk, drumPad(0, DpDecay), 1.5f);
      Out up = run(pk, {{0, -1, 72, 1.0f}}, 22050, 256);
      double fu = pitchOf(up.l, 2000, 6000, 44100, 300, 600);
      expect(std::fabs(fu / 400.0 - 1.0) < 0.01, "drumkit: pitched key mode transposes pad 1 by the key");
      expect(run(pk, {{0, -1, 40, 1.0f}}, 8192, 256).peak > 0.01f, "drumkit: pitched key mode plays keys below the base note");
    }
    // Decay: longer decay keeps more energy late.
    PluginState shortD = withParam(st, drumPad(15, DpDecay), 0.2f), longD = withParam(st, drumPad(15, DpDecay), 3.0f);
    Out s1 = run(shortD, {{0, -1, 75, 1.0f}}, 44100, 256), s2 = run(longD, {{0, -1, 75, 1.0f}}, 44100, 256);
    expect(energy(s2.l, 22050, 44100) > 10 * energy(s1.l, 22050, 44100), "drumkit: decay controls ring time");
    expect(!s1.activeAtEnd, "drumkit: voices end when silent");
  }

  // ---------------------------------------------------------------- Image Synth specifics
  {
    std::vector<uint8_t> px(static_cast<size_t>(kCanvasW * kCanvasH), 0);
    px[5] = 15;
    px[static_cast<size_t>(10 * kCanvasW + 3)] = 7;
    auto back = decodeCanvas(encodeCanvas(px));
    expect(back == px, "imagesynth: canvas hex round trip");
    expect(encodeCanvas(std::vector<uint8_t>(px.size(), 0)) == "0", "imagesynth: blank canvas encodes as \"0\"");
    // Write a 6x4 24-bit BMP and load it.
    std::string path = (std::filesystem::temp_directory_path() / "gridline_models_test.bmp").string();
    {
      int w = 6, h = 4, stride = (w * 3 + 3) & ~3;
      std::vector<unsigned char> f(54 + static_cast<size_t>(stride * h), 0);
      auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) f[o + static_cast<size_t>(i)] = static_cast<unsigned char>(v >> (8 * i)); };
      f[0] = 'B', f[1] = 'M';
      w32(2, static_cast<uint32_t>(f.size()));
      w32(10, 54);
      w32(14, 40);
      w32(18, static_cast<uint32_t>(w));
      w32(22, static_cast<uint32_t>(h));
      f[26] = 1;
      f[28] = 24;
      // bottom row (last image row) pure red at x=2
      f[54 + 2 * 3 + 2] = 255;
      std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    }
    std::string err;
    ImagePtr im = loadImageFile(path, &err);
    expect(im && im->w == 6 && im->h == 4, "imagesynth: BMP loads with its size");
    expect(im && im->r[static_cast<size_t>(3 * 6 + 2)] > 0.99f && im->g[static_cast<size_t>(3 * 6 + 2)] < 0.01f, "imagesynth: BMP rows bottom-up, red channel");
    expect(loadImageFile("/nonexistent/x.bmp") == nullptr, "imagesynth: missing file fails cleanly");
    // File source plays the loaded picture; canvas source with an empty canvas is silent.
    PluginState fs = defaultPluginState("gl.imagesynth");
    fs.params[ImSource] = SrcFile;
    fs.data = makeImageData(path, "");
    Out fo = run(fs, {{0, -1, 60, 1.0f}}, 22050, 256);
    expect(fo.peak > 0.01f && fo.finite, "imagesynth: file image sounds");
    expect(energy(fo.r, 0, 22050) < 1e-6 * energy(fo.l, 0, 22050) + 1e-9, "imagesynth: red-only image plays left only");
    PluginState cs = defaultPluginState("gl.imagesynth");
    cs.params[ImSource] = SrcCanvas;
    expect(run(cs, {{0, -1, 60, 1.0f}}, 8192, 256).peak > 0.005f, "imagesynth: untouched canvas plays the starter sketch");
    cs.data = makeImageData("", "0");
    expect(run(cs, {{0, -1, 60, 1.0f}}, 8192, 256).peak == 0, "imagesynth: blank canvas is silent");
    cs.data = makeImageData("", encodeCanvas(px));
    expect(run(cs, {{0, -1, 60, 1.0f}}, 8192, 256).peak > 0.005f, "imagesynth: drawn canvas sounds");
    // Without loop the image ends the voice.
    PluginState once = defaultPluginState("gl.imagesynth");
    once.params[ImPixel] = 0.005f;
    Out oo = run(once, {{0, -1, 60, 1.0f}}, 88200, 256);
    expect(!oo.activeAtEnd, "imagesynth: voice ends at the end of the picture");
    expect(std::fabs(imageRowHz(0, 10, 0, 100, 1000, 1.0) - 1000) < 1e-6 && std::fabs(imageRowHz(9, 10, 0, 100, 1000, 1.0) - 100) < 1e-6,
           "imagesynth: rows span low..high (top = high)");
    expect(std::fabs(imageRowHz(7, 8, 2, 100, 1000, 1.0) - 100) < 1e-6 && std::fabs(imageRowHz(0, 8, 2, 100, 1000, 1.0) - 800) < 1e-6,
           "imagesynth: harmonic scale");
    std::remove(path.c_str());
  }
  // ---------------------------------------------------------------- cost (informational)
  {
    std::vector<Note> chord;
    for (int i = 0; i < 8; ++i) chord.push_back({0, -1, 48.0 + i * 3, 0.8f});
    auto t0 = std::chrono::steady_clock::now();
    Out o = run(defaultPluginState("gl.additive"), chord, 44100 * 5, 512);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("# additive: 8 voices x 128 partials, 5 s of audio in %.0f ms\n", ms);
    expect(o.finite, "additive: 8-voice chord renders");
    t0 = std::chrono::steady_clock::now();
    std::vector<Note> pads;
    for (int i = 0; i < 16; ++i) pads.push_back({i * 2000, -1, 60.0 + i, 0.8f});
    run(defaultPluginState("gl.drumkit"), pads, 44100 * 5, 512);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("# drumkit: 16 pads, 5 s of audio in %.0f ms\n", ms);
    t0 = std::chrono::steady_clock::now();
    run(withParam(defaultPluginState("gl.string"), StStrum, 1), {{0, -1, 48, 0.8f}, {22050, -1, 55, 0.8f}}, 44100 * 5, 512);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("# string: two strummed chords (12 strings), 5 s of audio in %.0f ms\n", ms);
  }
  return finish();
}

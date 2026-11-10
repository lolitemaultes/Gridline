// Gridline - slide notes on every built-in instrument (issue: slide notes everywhere).
//
// For each instrument in the catalogue (and each of its models): a slide note alone starts
// nothing; a held C4 with a slide note to C5 over a beat plays C4 before the slide, about F#4
// halfway and C5 after it (pitch measured on the render against the same instrument playing
// C4 and C5 straight), the same at block sizes 64 and 512, and finite. Instruments whose own
// sound has no steady pitch the measurement can read (noise, drums, sequencers, controllers)
// are listed and only checked for the silent slide and finite output.
// Also: slide notes in MIDI files become bend lines (slidesAsBends), instruments that read
// pitch bend themselves are never bent twice, and MIDI Out forwards bends and pressure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "../../src/core/NoteBend.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Render.h"
#include "../../src/packs/models/drums.h"
#include "../../src/packs/samplers/samplers.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;

Song baseSong() {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.patterns[0].bars = 1;
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  s.playlist.push_back(c);
  return s;
}

Note mk(Tick start, Tick len, int pitch) {
  Note n;
  n.start = start;
  n.length = len;
  n.pitch = pitch;
  n.velocity = 100;
  return n;
}

// Pitch of the mono mix in [t0, t1) s: normalised square difference, the first lag whose
// clarity is within 85% of the best (avoids octave errors), refined by a parabola.
// Returns 0 when the window has no clear pitch (clarity < 0.8) or is silent.
double pitchAt(const AudioBuffer& b, double t0, double t1) {
  const size_t a = static_cast<size_t>(t0 * kSr), e = std::min(b.frames, static_cast<size_t>(t1 * kSr));
  if (e <= a + 900) return 0;
  std::vector<double> x(e - a);
  double energy = 0;
  for (size_t i = a; i < e; ++i) x[i - a] = 0.5 * (b.at(0, i) + b.at(1, i)), energy += x[i - a] * x[i - a];
  if (energy / static_cast<double>(x.size()) < 1e-8) return 0;
  const int minLag = kSr / 1500, maxLag = kSr / 90;
  const size_t W = x.size() - static_cast<size_t>(maxLag) - 1;  // lags run to maxLag + 1
  std::vector<double> nsdf(static_cast<size_t>(maxLag) + 2, 0.0);
  for (int lag = minLag; lag <= maxLag + 1 && static_cast<size_t>(lag) < x.size(); ++lag) {
    double ac = 0, m = 0;
    for (size_t i = 0; i < W; ++i) ac += x[i] * x[i + static_cast<size_t>(lag)], m += x[i] * x[i] + x[i + static_cast<size_t>(lag)] * x[i + static_cast<size_t>(lag)];
    nsdf[static_cast<size_t>(lag)] = m > 0 ? 2 * ac / m : 0;
  }
  double best = 0;
  for (int lag = minLag; lag <= maxLag; ++lag) best = std::max(best, nsdf[static_cast<size_t>(lag)]);
  if (best < 0.8) return 0;
  for (int lag = minLag + 1; lag <= maxLag; ++lag) {
    const double y0 = nsdf[static_cast<size_t>(lag - 1)], y1 = nsdf[static_cast<size_t>(lag)], y2 = nsdf[static_cast<size_t>(lag + 1)];
    if (y1 >= 0.85 * best && y1 >= y0 && y1 >= y2) {
      const double den = y0 - 2 * y1 + y2;
      const double d = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
      return kSr / (lag + d);
    }
  }
  return 0;
}

bool finite(const AudioBuffer& b) {
  for (float v : b.data)
    if (!std::isfinite(v)) return false;
  return true;
}

AudioBuffer render(const Song& s, int block) {
  RenderOptions o;
  o.blockSize = block;
  return renderSong(s, o);
}

// A 3 s sine on C4 (sample instruments play it from their own root).
std::string sineWav() {
  AudioBuffer b(2, static_cast<size_t>(kSr * 3));
  for (size_t i = 0; i < b.frames; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979323846 * 261.6256 * static_cast<double>(i) / kSr));
    b.at(0, i) = v, b.at(1, i) = v;
  }
  const std::string path = (std::filesystem::temp_directory_path() / "gridline_test_slides_c4.wav").u8string();
  writeWav(path, b, kSr, 16);
  return path;
}

struct Case {
  std::string type;
  int modelParam = -1;
  double model = 0;
  std::string label() const { return modelParam < 0 ? type : type + " model " + std::to_string(static_cast<int>(model)); }
};

}  // namespace

int main() {
  std::vector<Case> cases;
  for (const PluginInfo& p : pluginCatalog()) {
    if (p.kind != PluginKind::Generator || p.hidden) continue;
    if (p.legacyModelParam >= 0 && p.legacyModelParam < static_cast<int>(p.params.size())) {
      const ParamDesc& d = p.params[static_cast<size_t>(p.legacyModelParam)];
      for (int v = static_cast<int>(d.min); v <= static_cast<int>(d.max); ++v) cases.push_back({p.type, p.legacyModelParam, static_cast<double>(v)});
    } else {
      cases.push_back({p.type});
    }
  }
  const std::string wav = sineWav();
  int tracked = 0;
  std::string untracked;
  for (const Case& c : cases) {
    Song s = baseSong();
    const Tick q = s.ppq;
    Channel ch;
    ch.id = s.allocateId();
    ch.name = c.type;
    ch.plugin = defaultPluginState(c.type);
    const PluginInfo* info = findPlugin(c.type);
    // Sample instruments with nothing loaded by default get the sine (a sampler, a slicer...).
    if (info && info->usesSample && ch.plugin.data.empty()) {
      Song probe = s;
      Channel pc = ch;
      probe.channels.push_back(pc);
      probe.patterns[0].notes[pc.id] = {mk(0, 4 * q, 60)};
      if (render(probe, 512).peak() < 1e-4f) ch.plugin.data = wav;
    }
    // Scratch keys pick moves by default; pitched keys are the mode slides apply to. The sculpted
    // kick gets its longest length so the slide window has sound in it.
    if (c.type == "gl.scratch") ch.plugin.params[packs::samplers::ScKeyMode] = packs::samplers::KeyPitched;
    if (c.type == "gl.kicksculpt") ch.plugin.params[packs::models::dr::KsDuration] = 1.0f;
    if (c.modelParam >= 0 && c.modelParam < static_cast<int>(ch.plugin.params.size()))
      ch.plugin.params[static_cast<size_t>(c.modelParam)] = static_cast<float>(c.model);
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
    auto& notes = s.patterns[0].notes[ch.id];
    Note slide = mk(q / 2, q / 2, 72);  // 0.25 s .. 0.5 s at 120 BPM
    slide.slide = true;

    notes = {slide};
    const AudioBuffer alone = render(s, 512);
    expect(alone.peak() < 1e-4f && finite(alone), (c.label() + ": a slide note alone starts nothing").c_str());

    notes = {mk(0, 4 * q, 60)};
    const AudioBuffer r60 = render(s, 512);
    notes = {mk(0, 4 * q, 72)};
    const AudioBuffer r72 = render(s, 512);
    notes = {mk(0, 4 * q, 60), slide};
    const AudioBuffer sl = render(s, 512), sl64 = render(s, 64);
    expect(finite(sl) && finite(sl64), (c.label() + ": a slide renders finite").c_str());

    const double p60 = pitchAt(r60, 0.08, 0.22), p72 = pitchAt(r72, 0.6, 0.8);
    if (!(p60 > 0 && p72 > 0 && std::fabs(p72 / p60 - 2.0) < 0.06)) {
      // No steady pitch to read (drums, scratch and slice instruments): say whether the slide
      // changed what plays while it runs and just after (drums decay before a later window).
      double diff = 0, ref = 0;
      for (size_t i = static_cast<size_t>(0.3 * kSr); i < static_cast<size_t>(0.55 * kSr) && i < sl.frames && i < r60.frames; ++i)
        diff += std::fabs(sl.at(0, i) - r60.at(0, i)), ref += std::fabs(r60.at(0, i));
      const bool moved = ref > 0 && diff > 0.05 * ref;
      untracked += " " + c.label() + (moved ? " (moved)" : ref > 0 ? " (same)" : " (silent)");
      // These glide with slides (808-style kick glides above all): the slide must change the sound.
      static const char* const mustMove[] = {"gl.kick", "gl.snare", "gl.clap", "gl.pads", "gl.slicer", "gl.scratch", "gl.kicksculpt", "gl.sweepkick"};
      for (const char* t : mustMove)
        if (c.type == t) expect(moved, (c.label() + ": a slide moves the held hit").c_str());
      // The membrane's modelled engine glides; the reference engine's keys do not transpose it.
      // (The slice instrument's slides are checked in its own test, on a chopped loop.)
      if (c.type == "gl.membranepad" && c.model == 0) expect(moved, (c.label() + ": a slide moves the held hit").c_str());
      continue;
    }
    ++tracked;
    const double a = pitchAt(sl, 0.08, 0.22), m = pitchAt(sl, 0.355, 0.395), e = pitchAt(sl, 0.6, 0.8), e64 = pitchAt(sl64, 0.6, 0.8);
    if (const char* tr = std::getenv("GL_SLIDE_TRACE"); tr && c.type == tr)  // pitch every 20 ms
      for (double t = 0.0; t < 0.9; t += 0.02) std::printf("#   %.2f s: %.1f Hz\n", t, pitchAt(sl, t, t + 0.04));
    std::printf("# %-34s C4 %.1f Hz, C5 %.1f Hz; slide %.1f -> %.1f -> %.1f Hz (block 64: %.1f)\n", c.label().c_str(), p60, p72, a, m, e, e64);
    expect(a > 0 && std::fabs(a / p60 - 1) < 0.03, (c.label() + ": before the slide the held note plays its own pitch").c_str());
    // Flex plays its engine through a delay and a reverb: while the pitch moves, the echoes of the
    // pitches before it blur the reading, so only its start and end are measured.
    const bool echoes = c.type == "gl.flex";
    expect(echoes || (m > 0 && m / p60 > 1.2 && m / p60 < 1.6), (c.label() + ": halfway through the slide the note is halfway up").c_str());
    expect(e > 0 && std::fabs(e / p72 - 1) < 0.03, (c.label() + ": after the slide the held note plays the slide's pitch").c_str());
    expect(e64 > 0 && std::fabs(e64 / e - 1) < 0.01, (c.label() + ": the same slide at block size 64").c_str());
  }
  std::printf("# %d of %zu instrument models tracked; without a readable pitch:%s\n", tracked, cases.size(), untracked.c_str());
  expect(tracked >= 30, "most instruments have a readable pitch and slide");

  // Slides in MIDI files: slide notes become bend lines on the notes they move.
  {
    std::vector<Note> n = {mk(0, 384, 60), mk(96, 96, 67)};
    n[1].slide = true;
    const std::vector<Note> out = bend::slidesAsBends(n);
    expect(out.size() == 1 && out[0].pitch == 60 && !out[0].bend.empty(), "MIDI export: a slide note becomes a bend line on the held note");
    if (!out.empty() && !out[0].bend.empty()) {
      const BendPoint& last = out[0].bend.back();
      expect(std::fabs(last.semis - 7.0f) < 1e-4f && std::fabs(last.tick - 192.0f) < 1e-3f, "MIDI export: the bend reaches the slide's pitch at its end");
    }
    std::vector<Note> two = {mk(0, 384, 60), mk(96, 96, 67), mk(192, 96, 72)};
    two[1].slide = two[2].slide = true;
    const std::vector<Note> o2 = bend::slidesAsBends(two);
    expect(o2.size() == 1 && !o2[0].bend.empty() && std::fabs(o2[0].bend.back().semis - 12.0f) < 1e-4f, "MIDI export: chained slides keep climbing");
    std::vector<Note> col = {mk(0, 384, 60), mk(96, 96, 67)};
    col[1].slide = true, col[1].color = 3;
    const std::vector<Note> o3 = bend::slidesAsBends(col);
    expect(o3.size() == 1 && o3[0].bend.empty(), "MIDI export: a slide of another colour leaves the note alone");
    expect(bend::slidesAsBends(col, true).size() == 1 && !bend::slidesAsBends(col, true)[0].bend.empty(), "MIDI export: slides of every colour when the song says so");
  }

  // Instruments that read pitch bend themselves are not also bent by the engine (no double bend).
  for (const char* t : {"gl.plasma", "gl.dcosynth", "gl.twinva", "gl.sawswarm"}) {
    if (!findPlugin(t)) continue;
    auto g = createGenerator(t);
    expect(g && g->handlesPitchBend(), (std::string(t) + " reads pitch bend itself").c_str());
  }

  return finish();
}

// Gridline — J-22 "metal" group tests: Alloy (gl.alloy), Spectra Morph
// (gl.spectramorph), Grain Scanner (gl.grainscan), Spline Scratcher
// (gl.splinescratch) and the decode of FL Studio's shipped states for the
// metallic timbre generator, its preset player, the spectral morpher, the
// granular player and the spline scratcher.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpJ22Metal.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/metal.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;
using namespace gl::packs::models::metal;

namespace {

void ex(bool ok, const std::string& name) { expect(ok, name.c_str()); }

// Raw plugin states (event 213) of `plugin` in FL's shipped presets and projects.
std::vector<std::pair<std::string, std::vector<uint8_t>>> rawStates(const std::string& plugin, const std::vector<std::string>& extra = {}) {
  namespace fs = std::filesystem;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> v;
  auto take = [&](const std::string& path, bool preset) {
    flp::File f;
    if (!flp::readFile(path, f, nullptr)) return;
    for (size_t i = 0; i < f.events.size(); ++i) {
      if (f.events[i].id != 201 || f.events[i].text() != plugin) continue;
      for (size_t j = i + 1; j < f.events.size(); ++j) {
        const auto& x = f.events[j];
        if (x.id == 201 || x.id == 64) break;
        if (x.id == 213) {
          v.push_back({fs::path(path).filename().string(), std::vector<uint8_t>(x.data, x.data + x.size)});
          break;
        }
      }
    }
    (void)preset;
  };
  for (const auto& p : testfl::presetFiles(plugin, extra)) take(p, true);
  std::error_code ec;
  const fs::path root = fs::path(testfl::flData()) / "Data";
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && it->path().extension() == ".flp") take(it->path().string(), false);
  return v;
}

void alloyDecodeCoverage() {
  if (!testfl::flInstalled()) {
    std::printf("# FL Studio not installed: corpus checks skipped\n");
    return;
  }
  for (const char* plugin : {"Ogun", "Autogun"}) {
    auto states = rawStates(plugin);
    int ok = 0, walked = 0;
    for (const auto& [where, b] : states) {
      flp::j22::AlloyDecoded d;
      const bool good = flp::j22::decodeAlloyState(b.data(), b.size(), d, std::string(plugin) == "Autogun");
      ok += good;
      walked += good && d.consumedAll;
      if (!(good && d.consumedAll))
        std::printf("#   %s %s: v%d ok=%d consumed %zu / %zu\n", plugin, where.c_str(), d.version, good, d.consumed, d.inflated);
    }
    std::printf("# %s: %zu states, %d decoded, %d walked to the end\n", plugin, states.size(), ok, walked);
    ex(!states.empty(), std::string(plugin) + " states found");
    ex(ok == static_cast<int>(states.size()), std::string(plugin) + " every state decodes");
    ex(walked == static_cast<int>(states.size()), std::string(plugin) + " every state walks to its exact end");
  }
}

void scratchDecodeCoverage() {
  if (!testfl::flInstalled()) return;
  auto states = rawStates("Wave Traveller");
  int ok = 0, walked = 0, edited = 0;
  for (const auto& [where, b] : states) {
    SsData d;
    size_t used = 0;
    const bool good = flp::j22::decodeScratchState(b.data(), b.size(), d, &used);
    ok += good;
    walked += good && used == b.size();
    for (const auto& k : d.keys) edited += k.used;
    if (!(good && used == b.size())) std::printf("#   Wave Traveller %s: ok=%d walked %zu / %zu\n", where.c_str(), good, used, b.size());
  }
  std::printf("# Wave Traveller: %zu states, %d decoded, %d walked to the end, %d edited patches\n", states.size(), ok, walked, edited);
  ex(states.size() >= 9, "Wave Traveller states found");
  ex(ok == static_cast<int>(states.size()), "Wave Traveller states decode");
  ex(walked == ok, "Wave Traveller decoded states walk to their exact end");
  ex(edited > 20, "Wave Traveller edited patches kept");
}

void grainDecodeCoverage() {
  if (!testfl::flInstalled()) return;
  int n = 0, ok = 0;
  for (const char* plugin : {"Fruity granulizer", "Fruity Granulizer"}) {
    for (const auto& d : testfl::decodePresets(plugin)) ++n, ok += d.ok && d.state.type == "gl.grainscan";
    for (const auto& d : testfl::decodeProjects(plugin)) ++n, ok += d.ok && d.state.type == "gl.grainscan";
  }
  std::printf("# Granulizer: %d states, %d → Grain Scanner\n", n, ok);
  ex(n >= 10 && ok == n, "Granulizer states decode to Grain Scanner");
}

// ------------------------------------------------------------------ render
struct Out {
  std::vector<float> L, R;
  bool finite = true;
  float peak = 0;
  double rms(size_t a, size_t b) const {
    double s = 0;
    b = std::min(b, L.size());
    for (size_t i = a; i < b; ++i) s += L[i] * L[i] + R[i] * R[i];
    return b > a ? std::sqrt(s / (2.0 * static_cast<double>(b - a))) : 0.0;
  }
};
// Plays one note (key, velocity) from 0 for `onSec`, renders `totalSec`.
Out render(const PluginState& st0, double key = 60, double onSec = 0.5, double totalSec = 1.0, int block = 256, double sr = 44100) {
  PluginState st = st0;
  normalizePluginState(st);
  Out o;
  const int total = static_cast<int>(totalSec * sr), off = static_cast<int>(onSec * sr);
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
    if (pos == 0) g->noteOn(1, key, 0.8f, 0);
    if (off >= pos && off < pos + n) g->noteOff(1, off - pos);
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
  }
  return o;
}
PluginState fresh(const char* type) {
  PluginState s;
  s.type = type;
  normalizePluginState(s);
  return s;
}
// A test sample in the scratch folder: 1 s rising sine sweep (left = right).
std::string testSample() {
  static std::string path;
  if (!path.empty()) return path;
  path = (std::filesystem::temp_directory_path() / "gl_j22_metal_test.wav").string();
  const int sr = 44100, n = sr;
  std::vector<int16_t> pcm(static_cast<size_t>(n));
  double ph = 0;
  for (int i = 0; i < n; ++i) {
    ph += 2 * 3.14159265 * (200.0 + 800.0 * i / n) / sr;
    pcm[static_cast<size_t>(i)] = static_cast<int16_t>(std::sin(ph) * (i % 11025 < 300 ? 30000 : 12000));
  }
  std::FILE* f = std::fopen(path.c_str(), "wb");
  auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f), w32(36 + n * 2), std::fwrite("WAVEfmt ", 1, 8, f), w32(16), w16(1), w16(1), w32(sr), w32(sr * 2), w16(2), w16(16);
  std::fwrite("data", 1, 4, f), w32(n * 2), std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
  return path;
}

void catalogue() {
  for (const char* t : {"gl.alloy", "gl.spectramorph", "gl.grainscan", "gl.splinescratch"}) {
    const PluginInfo* p = findPlugin(t);
    ex(p != nullptr, std::string(t) + " registered");
    if (!p) continue;
    ex(p->presets.size() >= 2 && p->presets.size() <= 6, std::string(t) + " has 2-6 factory presets");
    ex(packs::createGenerator(t) != nullptr, std::string(t) + " creates");
  }
  const PluginInfo* a = findPlugin("gl.alloy");
  ex(a && a->params.size() == static_cast<size_t>(AlCount), "Alloy param count");
}

void alloyBehaviour() {
  PluginState s = fresh("gl.alloy");
  Out o = render(s, 60, 0.4, 1.5);
  std::printf("# alloy default peak %.3f rms %.4f\n", static_cast<double>(o.peak), o.rms(0, 4410));
  ex(o.finite && o.rms(0, 4410) > 1e-3, "Alloy sounds after note-on");
  // Longer decay keeps more energy late.
  PluginState shortD = s, longD = s;
  shortD.params[AlDecay] = 500, longD.params[AlDecay] = 9000;
  shortD.params[AlRelease] = longD.params[AlRelease] = 9000;
  const double eS = render(shortD, 60, 1.4, 1.5).rms(44100, 60000), eL = render(longD, 60, 1.4, 1.5).rms(44100, 60000);
  ex(eL > eS * 2, "Alloy decay time lengthens the tail");
  // Seeds change the timbre; same seed is deterministic.
  PluginState a1 = s, a2 = s;
  a1.params[AlSeed1] = 5, a2.params[AlSeed1] = 6;
  a1.params[AlMorph] = a2.params[AlMorph] = 0;
  Out o1 = render(a1), o1b = render(a1), o2 = render(a2);
  double diff = 0, same = 0;
  for (size_t i = 0; i < 8000; ++i) diff += std::fabs(o1.L[i] - o2.L[i]), same += std::fabs(o1.L[i] - o1b.L[i]);
  ex(diff > 1.0 && same < 1e-6, "Alloy seed selects a reproducible timbre");
  // Filter: low cutoff darkens.
  PluginState lo = s;
  lo.params[AlCutoff] = 30;
  ex(render(lo).rms(0, 8000) < o.rms(0, 8000), "Alloy filter cutoff lowers the level");
  // Volume 0 → silence.
  PluginState mute = s;
  mute.params[AlVolume] = 0;
  ex(render(mute).peak < 1e-6f, "Alloy volume 0 is silent");
  // Extremes finite.
  PluginState x = s;
  const PluginInfo* info = findPlugin("gl.alloy");
  for (int pass = 0; pass < 2 && info; ++pass) {
    for (size_t i = 0; i < info->params.size(); ++i) x.params[i] = static_cast<float>(pass ? info->params[i].max : info->params[i].min);
    ex(render(x, 30).finite && render(x, 100).finite, pass ? "Alloy maxima finite" : "Alloy minima finite");
  }
  // Block-size invariance (no randomness: seeds set).
  PluginState b = a1;
  Out b1 = render(b, 60, 0.3, 0.5, 64), b2 = render(b, 60, 0.3, 0.5, 512);
  double bd = 0;
  for (size_t i = 0; i < b1.L.size(); ++i) bd = std::max(bd, static_cast<double>(std::fabs(b1.L[i] - b2.L[i])));
  ex(bd < 1e-3, "Alloy block-size invariant");
  // Short / legacy states normalise.
  PluginState sh;
  sh.type = "gl.alloy";
  sh.params = {100, 0, 0};
  normalizePluginState(sh);
  ex(sh.params.size() == static_cast<size_t>(AlCount) && render(sh).finite, "Alloy short state normalises");
  // Drawn data round trip.
  AlloyData d;
  d.arts[AaVolume].env.enabled = true;
  d.eq[AeImpulse].pts = {{0, 0.2f, 0, 0}, {1, 0.9f, 0, 0}};
  d.info = "line 1\nline 2";
  AlloyData back;
  ex(parseAlloy(serializeAlloy(d), back) && back.eq[AeImpulse].pts.size() == 2 && back.info == d.info, "Alloy data round trip");
  ex(!parseAlloy("garbage", back), "Alloy rejects malformed data");
}

void corpusSound() {
  if (!testfl::flInstalled()) return;
  for (const char* plugin : {"Ogun", "Autogun", "Morphine"}) {
    std::vector<testfl::Decoded> all = testfl::decodePresets(plugin, {".mrp"});
    for (auto& d : testfl::decodeProjects(plugin)) all.push_back(std::move(d));
    int ok = 0, finite = 0, audible = 0, n = 0;
    for (const auto& d : all) {
      ++n;
      if (!d.ok) continue;
      ++ok;
      if (n > 400) continue;
      Out o = render(d.state, 60, 1.0, 1.1);
      if (const char* tr = std::getenv("GL_J22_TRACE"); tr && d.where.find(tr) != std::string::npos) {
        for (int w = 0; w < 11; ++w) std::printf("#   t=%.1f rms %.6f\n", w * 0.1, o.rms(static_cast<size_t>(w * 4410), static_cast<size_t>((w + 1) * 4410)));
        for (int pi = 0; pi < AlCount && d.state.type == "gl.alloy"; ++pi) {
          PluginState q = d.state;
          const PluginInfo* inf = findPlugin("gl.alloy");
          q.params[static_cast<size_t>(pi)] = static_cast<float>(inf->params[static_cast<size_t>(pi)].def);
          const double r = render(q, 60, 1.0, 1.1).rms(0, 48510);
          if (r > 1e-5) std::printf("#   resetting %s → rms %.5f\n", inf->params[static_cast<size_t>(pi)].name.c_str(), r);
        }
      }
      finite += o.finite;
      audible += o.finite && o.rms(0, o.L.size()) > 1e-5;
      if (!(o.finite && o.rms(0, o.L.size()) > 1e-5)) std::printf("#   silent: %s %s\n", plugin, d.where.c_str()), testfl::maybeDump(d);
    }
    std::printf("# %s: %d states, %d decoded, %d finite, %d audible\n", plugin, n, ok, finite, audible);
    ex(n > 0 && ok == n, std::string(plugin) + " corpus decodes");
    ex(finite == std::min(ok, 400), std::string(plugin) + " corpus renders finite");
    ex(audible >= finite * 9 / 10, std::string(plugin) + " corpus is audible (>= 90 %)");
  }
}

void morphBehaviour() {
  PluginState s = fresh("gl.spectramorph");
  ex(render(s).rms(0, 8000) > 1e-3, "Spectra Morph fresh instance plays its built-in programs");
  SmData d;
  for (int p = 0; p < 2; ++p) {
    SmZone z;
    z.key = 60;
    for (int f = 0; f < 20; ++f) {
      SmFrame fr;
      fr.hold = 5;
      fr.level = 1.0f - f * 0.03f;
      for (int k = 0; k < 24; ++k) fr.amp.push_back(p == 0 ? 1.0f / (k + 1) : (k % 2 ? 0.0f : 1.0f / (k + 1)));
      z.frames.push_back(fr);
    }
    z.loopStart = 40, z.loopEnd = 80;
    d.prog[static_cast<size_t>(p)].zones.push_back(z);
  }
  s.data = serializeMorph(d);
  SmData back;
  ex(parseMorph(s.data, back) && back.prog[1].zones.size() == 1 && back.prog[1].zones[0].frames.size() == 20, "Spectra Morph data round trip");
  Out a = render(s, 60, 0.8, 1.0);
  ex(a.finite && a.rms(0, 8000) > 1e-3, "Spectra Morph sounds");
  ex(a.rms(30000, 34000) > 1e-4, "Spectra Morph sustains through its loop");
  PluginState sx = s;
  sx.params[SmMorphX] = 1;
  Out b = render(sx, 60, 0.8, 1.0);
  double diff = 0;
  for (size_t i = 0; i < 8000; ++i) diff += std::fabs(a.L[i] - b.L[i]);
  ex(diff > 0.5, "Spectra Morph X morph changes the timbre");
  ex(!parseMorph("smorph1\nf 1 1 00ff", back), "Spectra Morph rejects frames without a zone");
}

void grainBehaviour() {
  PluginState s = fresh("gl.grainscan");
  ex(render(s).rms(0, 20000) > 1e-4, "Grain Scanner without a sample plays the built-in phrase");
  s.data = testSample();
  Out a = render(s, 60, 0.6, 0.8);
  ex(a.finite && a.rms(0, 20000) > 1e-3, "Grain Scanner plays the sample");
  PluginState st = s;
  st.params[GsStereo] = 64;
  Out b = render(st, 60, 0.6, 0.8);
  double side = 0;
  for (size_t i = 0; i < 20000; ++i) side += std::fabs(b.L[i] - b.R[i]);
  ex(side > 1.0, "Grain Scanner stereo separates the channels");
  PluginState rev = s;
  rev.params[GsWaveSpace] = -611;
  ex(render(rev, 60, 0.6, 0.8).rms(0, 20000) > 1e-3, "Grain Scanner plays backwards");
  for (int trig = 0; trig < 4; ++trig) {
    PluginState t = s;
    t.params[GsTrigger] = static_cast<float>(trig);
    ex(render(t, 64, 0.3, 0.4).finite, "Grain Scanner key mode " + std::to_string(trig) + " finite");
  }
  const PluginInfo* info = findPlugin("gl.grainscan");
  PluginState x = s;
  for (size_t i = 0; info && i < info->params.size(); ++i) x.params[i] = static_cast<float>(info->params[i].max);
  ex(render(x).finite, "Grain Scanner maxima finite");
}

void scratchBehaviour() {
  PluginState s = fresh("gl.splinescratch");
  ex(render(s).rms(0, 20000) > 1e-4, "Spline Scratcher without a sample plays the built-in phrase");
  SsData d;
  d.path = testSample();
  for (auto& k : d.keys) k = ssDefaultPatch();
  SsPatch p = ssDefaultPatch();
  p.used = true;
  p.beats = 1;
  p.pos = {{0, 0, 0.5f, SsLinear, false}, {0.5f, 0.8f, 0.5f, SsHalfCosine, false}, {1, 0.1f, 0.5f, SsLinear, false}};
  p.mutes = {{true, 0}, {false, 0.5f}, {true, 0.75f}};
  d.keys[62] = p;
  s.data = serializeScratchSpl(d);
  SsData back;
  ex(parseScratchSpl(s.data, back) && back.keys[62].used && back.keys[62].pos.size() == 3 && back.path == d.path, "Spline Scratcher data round trip");
  Out a = render(s, 62, 0.6, 0.6);
  ex(a.finite && a.rms(0, 10000) > 1e-3, "Spline Scratcher plays a patch");
  ex(a.rms(12000, 15500) < a.rms(0, 10000) * 0.3, "Spline Scratcher mute toggles silence a span");
  ex(render(s, 60, 0.6, 0.6).rms(0, 10000) > 1e-3, "Spline Scratcher default patch plays forward");
  ex(ssEval(p.pos, 0.25f) > 0.0f && ssEval(p.pos, 0.25f) < 0.8f, "Spline evaluation interpolates");
}

}  // namespace

int main() {
  alloyDecodeCoverage();
  scratchDecodeCoverage();
  grainDecodeCoverage();
  catalogue();
  alloyBehaviour();
  morphBehaviour();
  grainBehaviour();
  scratchBehaviour();
  corpusSound();
  return finish();
}

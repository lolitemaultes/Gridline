// Gridline — FLP export speed (J-51): Gridline-made songs whose plugins go
// through the encoders' solvers (synth channels, Band Delay curve fits) export
// within generous time bounds, and repeat exports are byte-identical.
//
// GRIDLINE_FLP_PERF_DUMP=<dir>: also writes each song (.gridline.json) and its
// export (.flp) there, for byte comparisons between builds.
//
// Q-2: load tolerance. Wall time on a busy machine (parallel ctest, other
// builds) can be many times the unloaded time, so each bound is checked on
// the best of up to three attempts, and a case whose wall time still misses
// passes only when its CPU time (process user + system, every thread) is
// within the CPU bound — CPU time hardly grows with machine load, so a real
// regression (more work) still fails.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "core/Platform.h"
#include "Check.h"
#include "core/Demo.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/FlpEncode.h"
#include "core/flp/FlpExport.h"
#include "packs/spacefx/spacefx.h"

using namespace gl;

namespace {

struct Rng {
  uint64_t s;
  uint64_t next() {
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return s;
  }
  double u() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  size_t pick(size_t n) { return static_cast<size_t>(next() % n); }
};

// A state of `type`: tweaks < 0 moves about half the parameters to random
// values (stress); else a factory preset (or the defaults) with `tweaks`
// knobs turned, as a song made in Gridline has them.
PluginState makeState(const std::string& type, int tweaks, Rng& r) {
  PluginState s = defaultPluginState(type);
  const PluginInfo* info = findPlugin(type);
  if (!info) return s;
  if (tweaks >= 0 && !info->presets.empty() && r.u() < 0.6) s = presetState(*info, r.pick(info->presets.size()));
  auto move = [&](size_t k) {
    const ParamDesc& d = info->params[k];
    s.params[k] = static_cast<float>(d.clamp(d.fromNormalized(r.u())));
  };
  const size_t n = std::min(s.params.size(), info->params.size());
  if (n == 0) return s;
  if (tweaks < 0) {
    for (size_t k = 0; k < n; ++k)
      if (r.u() < 0.5) move(k);
  } else {
    for (int i = 0; i < tweaks; ++i) move(r.pick(n));
  }
  return s;
}

// Band Delay with drawn curves (the curve-fit path).
PluginState bandDelay(Rng& r) {
  namespace sfx = gl::packs::spacefx;
  PluginState s = defaultPluginState("gl.banddelay");
  s.params[sfx::BdMorph] = static_cast<float>(r.u());
  s.params[sfx::BdFeedback] = static_cast<float>(r.u() * 0.9);
  sfx::BandCurves c = sfx::defaultBandCurves();
  for (auto& g : c.v)
    for (auto& t : g) {
      const double a = r.u(), b = r.u(), ph = r.u() * 6.28;
      for (int i = 0; i < sfx::kBdPoints; ++i) t[i] = static_cast<float>(std::clamp(a + 0.4 * b * std::sin(ph + i * 0.3), 0.0, 1.0));
    }
  s.data = sfx::formatBandCurves(c);
  return s;
}

bool encodable(const PluginInfo& p) {
  if (isHostedType(p.type) || p.usesSample) return false;
  return flp::enc::nativeEncoder(defaultPluginState(p.type)) != nullptr;
}

// The demo song plus `synths` plugin channels and `effects` mixer slots
// (encodable types in turn / at random) and `delays` Band Delays.
Song makeSong(uint64_t seed, int synths, int effects, int delays, int tweaks) {
  Rng r{seed * 0x9E3779B97F4A7C15ull + 1};
  Song s = makeDemoSong();
  std::vector<std::string> gens, fxs;
  for (const auto& p : pluginCatalog()) {
    if (!encodable(p)) continue;
    if (p.kind == PluginKind::Generator) gens.push_back(p.type);
    else if (p.kind == PluginKind::Effect && p.type != "gl.banddelay") fxs.push_back(p.type);
  }
  const size_t g0 = gens.empty() ? 0 : r.pick(gens.size());
  for (int i = 0; i < synths && !gens.empty(); ++i) {
    Channel c;
    c.id = s.allocateId();
    c.plugin = makeState(gens[(g0 + static_cast<size_t>(i)) % gens.size()], tweaks, r);
    c.name = "Synth " + std::to_string(i + 1);
    s.channels.push_back(c);
  }
  std::vector<PluginState> slots;
  for (int i = 0; i < effects && !fxs.empty(); ++i) slots.push_back(makeState(fxs[r.pick(fxs.size())], tweaks, r));
  for (int i = 0; i < delays; ++i) slots.push_back(bandDelay(r));
  for (size_t i = 0; i < slots.size(); i += 4) {
    MixerTrack m;
    m.id = s.allocateId();
    m.name = "Fx " + std::to_string(i / 4 + 1);
    for (size_t k = 0; k < 4 && i + k < slots.size(); ++k) m.slots[k].plugin = slots[i + k];
    s.mixer.push_back(m);
  }
  return s;
}

// Process CPU time (user + system, all threads), seconds.
double cpuSeconds() { return gl::platform::cpuSeconds(); }

struct Timing {
  double wall = -1, cpu = -1;
};

Timing exportTimed(const Song& s, flp::Bytes& out, flp::ExportReport& rep) {
  flp::ExportOptions o;
  o.factoryData = "/nonexistent-fl-data";
  const double c0 = cpuSeconds();
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = flp::exportFlp(s, out, rep, o);
  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!ok) return {};
  return {dt, cpuSeconds() - c0};
}

double exportSeconds(const Song& s, flp::Bytes& out, flp::ExportReport& rep) { return exportTimed(s, out, rep).wall; }

}  // namespace

// Every encodable generator (or effect) once, with a few knobs turned.
Song everyType(uint64_t seed, PluginKind kind) {
  Rng r{seed * 0x9E3779B97F4A7C15ull + 7};
  Song s = makeDemoSong();
  std::vector<PluginState> fx;
  for (const auto& p : pluginCatalog()) {
    if (!encodable(p) || p.kind != kind) continue;
    if (kind == PluginKind::Generator) {
      Channel c;
      c.id = s.allocateId();
      c.plugin = makeState(p.type, 4, r);
      c.name = p.name;
      s.channels.push_back(c);
    } else {
      fx.push_back(p.type == "gl.banddelay" ? bandDelay(r) : makeState(p.type, 4, r));
    }
  }
  for (size_t i = 0; i < fx.size(); i += 8) {
    MixerTrack m;
    m.id = s.allocateId();
    for (size_t k = 0; k < 8 && i + k < fx.size(); ++k) m.slots[k].plugin = fx[i + k];
    s.mixer.push_back(m);
  }
  return s;
}

int main() {
  const char* dump = std::getenv("GRIDLINE_FLP_PERF_DUMP");
  struct Case {
    const char* name;
    uint64_t seed;
    int synths, effects, delays, tweaks;
    double bound;  // seconds, generous (before J-51: demo 0.8 s, typical 23 s / 12 s, delays 37 s, synths 67 s, mixed 73 s)
    // Q-2: CPU seconds (all threads) when the wall time misses under load —
    // about 4x the CPU time measured at Q-2 (demo 0.09 s, typical 1.06 s,
    // delays 2.4 s, synths 3.0 s, mixed 4.8 s on 8 solver threads).
    double cpuBound;
  };
  const Case cases[] = {
      {"demo", 0, 0, 0, 0, 0, 1.5, 0.6},
      {"typical", 1, 8, 12, 2, 4, 3.0, 4.0},
      {"typical2", 5, 8, 12, 2, 4, 3.0, 4.0},
      {"delays", 2, 0, 0, 12, 0, 5.0, 8.0},
      {"synths", 3, 40, 0, 0, -1, 20.0, 12.0},
      {"mixed", 4, 24, 40, 6, -1, 20.0, 18.0},
  };
  for (const Case& c : cases) {
    const Song s = makeSong(c.seed, c.synths, c.effects, c.delays, c.tweaks);
    flp::Bytes a, b, again;
    flp::ExportReport ra, rb, rc;
    flp::enc::clearEncodeCache();  // (time the solving, not the cache)
    Timing t = exportTimed(s, a, ra);
    Timing tCached = exportTimed(s, again, rc);
    flp::enc::clearEncodeCache();
    Timing t2 = exportTimed(s, b, rb);
    // Q-2: a busy machine gets two more attempts (best wall time, best CPU time).
    for (int retry = 0; retry < 2 && (t.wall >= c.bound || t2.wall >= c.bound); ++retry) {
      flp::Bytes x;
      flp::ExportReport rx;
      flp::enc::clearEncodeCache();
      const Timing r = exportTimed(s, x, rx);
      if (r.wall >= 0) t.wall = std::min(t.wall, r.wall), t.cpu = std::min(t.cpu, r.cpu);
    }
    for (int retry = 0; retry < 2 && tCached.wall >= std::max(0.5, 0.5 * t.wall); ++retry) {
      flp::Bytes x;
      flp::ExportReport rx;
      const Timing r = exportTimed(s, x, rx);  // the cache still holds the solutions
      if (r.wall >= 0) tCached.wall = std::min(tCached.wall, r.wall), tCached.cpu = std::min(tCached.cpu, r.cpu);
    }
    int exact = 0, other = 0;
    for (const auto& p : ra.plugins) (p.quality == "exact" || p.quality == "verbatim" ? exact : other) += 1;
    std::printf("# %s: %zu plugins (%d exact, %d other), %zu bytes, export %.3f s (cpu %.3f s), again %.3f s, cached %.3f s (cpu %.3f s)\n", c.name,
                ra.plugins.size(), exact, other, a.size(), t.wall, t.cpu, t2.wall, tCached.wall, tCached.cpu);
    expect(t.wall >= 0 && t2.wall >= 0 && tCached.wall >= 0, (std::string(c.name) + ": exports").c_str());
    expect(a == b, (std::string(c.name) + ": a second export, solved again, is byte-identical").c_str());
    expect(a == again, (std::string(c.name) + ": an export from the cached solutions is byte-identical").c_str());
    const bool inTime = t.wall < c.bound || t.cpu < c.cpuBound;
    if (t.wall >= c.bound && inTime) std::printf("# %s: wall time over the bound (machine load), CPU time within it\n", c.name);
    expect(inTime, (std::string(c.name) + ": export within the time bound (wall, or CPU under load)").c_str());
    const bool cachedFast = tCached.wall < std::max(0.5, 0.5 * t.wall) || tCached.cpu < std::max(0.25, 0.5 * t.cpu);
    expect(cachedFast, (std::string(c.name) + ": a repeat export reuses the solved states").c_str());
    if (dump) {
      const std::string base = std::string(dump) + "/" + c.name;
      saveProject(base + ".gridline.json", s);
      if (FILE* f = std::fopen((base + ".flp").c_str(), "wb")) {
        std::fwrite(a.data(), 1, a.size(), f);
        std::fclose(f);
      }
    }
  }
  // The fields' forward laws against the decoders: every 3rd law value of
  // songs with every encodable plugin type is decoded too and must agree
  // (a law that disagrees could change the bytes the search writes).
  flp::enc::setVerifyLaws(3);
  const int before = flp::enc::lawMismatches();
  const std::pair<const char*, Song> checked[] = {{"allgens", everyType(11, PluginKind::Generator)},
                                                  {"allfx", everyType(12, PluginKind::Effect)},
                                                  {"verify13", makeSong(13, 8, 12, 2, 4)},
                                                  {"verify14", makeSong(14, 12, 0, 0, -1)}};
  for (const auto& [name, s] : checked) {
    flp::enc::clearEncodeCache();
    flp::Bytes out;
    flp::ExportReport rep;
    exportSeconds(s, out, rep);
    if (dump) saveProject(std::string(dump) + "/" + name + ".gridline.json", s);
  }
  flp::enc::setVerifyLaws(0);
  std::printf("# law mismatches: %d\n", flp::enc::lawMismatches() - before);
  expect(flp::enc::lawMismatches() == before, "forward laws agree with the decoders");
  return finish();
}

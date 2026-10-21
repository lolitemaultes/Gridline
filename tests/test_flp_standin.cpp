// Gridline — FLP export of Gridline-only plugins (J-80): every built-in type
// exports as an FL plugin, a mapped FL plugin, rendered samples, or a neutral
// stand-in with a documented reason; the demo's rendered channels re-import
// with their samples and sound as before; renders are deterministic.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include "../src/core/Demo.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/ProjectInterop.h"
#include "../src/core/Song.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/core/flp/FlpPackage.h"
#include "../src/core/flp/FlpStandIn.h"
#include "../src/engine/Render.h"
#include "../src/packs/samplers/dwave.h"
#include "../src/packs/synths/samplekeys.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

fs::path scratch() {
  fs::path p = fs::temp_directory_path() / "gridline-test-flp-standin";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

std::string readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

// The demo project file (as `gridline export-flp demo/demo.gridline.json`).
Song demoFile() {
  Song s;
  std::string err;
  if (!loadProject(std::string(GRIDLINE_SOURCE_DIR) + "/demo/demo.gridline.json", s, &err)) s = makeDemoSong();
  return s;
}

// A small song with one instance of `type`: its only channel playing two
// notes (instrument) or a slot of insert 1 (effect; the channel goes there).
Song oneOf(const PluginInfo& pi) {
  Song s = makeDemoSong();
  s.channels.resize(1);
  Channel& c = s.channels[0];
  c.name = "Probe";
  c.mixerTrack = s.mixer[1].id;
  c.plugin = pi.kind == PluginKind::Generator ? defaultPluginState(pi.type) : defaultPluginState("gl.sampler");
  for (auto& p : s.patterns) {
    p.steps.clear(), p.notes.clear(), p.stepProps.clear();
    p.notes[c.id] = {Note(0, s.ppq, 60, 100), Note(s.ppq * 2, s.ppq, 64, 100)};
  }
  s.automations.clear(), s.links.clear(), s.modLinks.clear();
  s.patterns.resize(1);
  s.arrangements.clear(), s.currentArrangement = 0;
  s.playlist.clear();
  Clip clip;
  clip.kind = ClipKind::Pattern;
  clip.pattern = s.patterns[0].id;
  clip.id = s.allocateId();
  clip.start = 0;
  clip.track = 0;
  clip.length = s.barTicks();
  s.playlist.push_back(clip);
  for (auto& t : s.mixer) t.slots.clear();
  if (pi.kind != PluginKind::Generator) {
    s.mixer[1].slots.resize(1);
    s.mixer[1].slots[0].plugin = defaultPluginState(pi.type);
  }
  return s;
}

const flp::ExportReport::Plugin* findEntry(const flp::ExportReport& r, const std::string& where) {
  for (const auto& p : r.plugins)
    if (p.where == where) return &p;
  return nullptr;
}

void testCatalogue() {
  int placeholders = 0;
  for (const PluginInfo& pi : pluginCatalog()) {
    if (isHostedType(pi.type) || pi.type == "gl.sampler") continue;  // the sampler is FL's own sampler channel
    Song s = oneOf(pi);
    flp::ExportOptions o;
    o.useOrigin = false;
    o.sampleDir = (scratch() / "catalogue" / pi.type).string();
    flp::ExportReport rep;
    flp::Bytes b;
    const bool ok = flp::exportFlp(s, b, rep, o);
    const auto* p = findEntry(rep, pi.kind == PluginKind::Generator ? "channel 'Probe'" : "insert 1 slot 1");
    bool good = ok && p;
    if (good && p->quality == "placeholder") {
      // Neutral: only with a reason of its own.
      ++placeholders;
      good = !p->note.empty() && p->note != "no FL counterpart" && p->note.find("written as an empty sampler") == std::string::npos;
      std::printf("# %-16s neutral: %s\n", pi.type.c_str(), p->note.c_str());
    } else if (good) {
      std::printf("# %-16s -> %s [%s]\n", pi.type.c_str(), p->flName.c_str(), p->quality.c_str());
    }
    expect(good, (pi.type + " exports with its sound, or neutral with a documented reason").c_str());
  }
  std::printf("# neutral types: %d\n", placeholders);
  // Effects in their Gridline-own (legacy) engine: written as the reference engine's FL plugin.
  for (const PluginInfo& pi : pluginCatalog()) {
    if (pi.kind == PluginKind::Generator || pi.legacyModelParam < 0) continue;
    Song s = oneOf(pi);
    s.mixer[1].slots[0].plugin.params[static_cast<size_t>(pi.legacyModelParam)] = 0;
    flp::ExportOptions o;
    o.useOrigin = false;
    flp::ExportReport rep;
    flp::Bytes b;
    flp::exportFlp(s, b, rep, o);
    const auto* p = findEntry(rep, "insert 1 slot 1");
    std::printf("# %-16s legacy engine -> %s [%s]\n", pi.type.c_str(), p ? p->flName.c_str() : "-", p ? p->quality.c_str() : "-");
    // J-81: unless FL cannot hold that plugin in a mixer slot (a VFX note plugin runs only inside Patcher).
    const bool slotless = p && p->quality == "placeholder" && p->note.find("only inside Patcher") != std::string::npos;
    expect(p && (p->quality != "placeholder" || slotless), (pi.type + " (Gridline's own engine) exports as an FL plugin").c_str());
  }
}

// One channel of the song alone (every other channel muted).
double channelRms(Song s, const std::string& name) {
  bool found = false;
  for (auto& c : s.channels) {
    c.muted = c.name != name;
    found = found || c.name == name;
  }
  if (!found) return -1;
  RenderOptions ro;
  ro.tail = s.ppq * 4;
  AudioBuffer a = renderSong(s, ro);
  double e = 0;
  for (float v : a.data) e += static_cast<double>(v) * v;
  return a.data.empty() ? 0 : std::sqrt(e / static_cast<double>(a.data.size()));
}

void testDemo() {
  const Song demo = demoFile();
  const fs::path dir = scratch() / "demo";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const std::string path = (dir / "demo1.flp").string();
  flp::ExportOptions o;
  o.factoryData = flp::defaultFactoryData();
  o.projectDir = dir.string();
  flp::ExportReport rep;
  std::string err;
  expect(flp::exportFlpFile(demo, path, rep, o, &err), "demo exports");
  expect(fs::is_directory(dir / "demo1"), "samples go to a folder named after the project");
  int placeholders = 0, rendered = 0, mapped = 0;
  for (const auto& p : rep.plugins) {
    placeholders += p.quality == "placeholder";
    rendered += p.quality == "rendered";
    mapped += p.quality == "mapped";
  }
  std::printf("# demo: rendered %d, mapped %d, placeholders %d\n", rendered, mapped, placeholders);
  expect(placeholders == 0, "demo: no placeholders (was 8)");
  expect(rendered >= 6, "demo: the Gridline drum and synth channels are rendered");
  expect(mapped >= 3, "demo: chorus, delay and keys are mapped");
  bool porta = false;
  for (const auto& w : rep.warnings) porta = porta || w.find("porta") != std::string::npos;
  expect(!porta, "no porta time warning when no note glides");
  const std::string summary = flp::exportSummary(rep, "demo1.flp");
  std::printf("# %s\n", summary.c_str());
  expect(summary.find("rendered to samples") != std::string::npos && summary.find("effects mapped") != std::string::npos,
         "summary names renders and mappings");

  // Re-import: sampler channels and DirectWave zones refer to existing rendered files.
  Song back;
  flp::Report irep;
  expect(importFlp(path, back, irep, &err, o.factoryData), "exported demo re-imports");
  int samplers = 0, zones = 0, missing = 0;
  for (const auto& c : back.channels) {
    if (c.plugin.type == "gl.sampler" && !c.plugin.data.empty()) {
      ++samplers;
      missing += !fs::is_regular_file(c.plugin.data);
    }
    if (c.plugin.type == "gl.waveprogram") {
      packs::samplers::dw::DwProgram p;
      if (packs::samplers::dw::parseProgram(c.plugin.data, p))
        for (const auto& z : p.zones) ++zones, missing += !fs::is_regular_file(z.path);
    }
  }
  std::printf("# re-import: %d sampler channels, %d DirectWave zones, %d missing\n", samplers, zones, missing);
  expect(samplers >= 1 && zones >= 1 && missing == 0, "re-imported channels reference the rendered files");

  // Determinism: the same project gives identical files.
  std::map<std::string, std::string> first;
  for (const auto& f : fs::directory_iterator(dir / "demo1")) first[f.path().filename().string()] = readAll(f.path());
  const std::string flp1 = readAll(path);
  flp::ExportReport rep2;
  expect(flp::exportFlpFile(demo, path, rep2, o, &err), "demo exports again");
  bool same = readAll(path) == flp1;
  size_t n = 0;
  for (const auto& f : fs::directory_iterator(dir / "demo1")) {
    ++n;
    auto it = first.find(f.path().filename().string());
    same = same && it != first.end() && it->second == readAll(f.path());
  }
  expect(same && n == first.size() && n > 0, "renders are deterministic (identical .flp and samples)");

  // Sound: no channel that was audible is silent after the round trip.
  for (const auto& c : demo.channels) {
    if (c.plugin.type == "gl.keys" && !packs::synths::skeys::keySet("Rhodes")) continue;  // FL Keys plays FL's sound sets
    const double a = channelRms(demo, c.name), b = channelRms(back, c.name);
    const double db = a > 0 && b > 0 ? 20 * std::log10(b / a) : -999;
    std::printf("# channel %-9s rms %.5f -> %.5f (%+.1f dB)\n", c.name.c_str(), a, b, db);
    if (a < 1e-4) continue;
    const bool renderedCh = c.plugin.type != "gl.keys";  // rendered: close; mapped: audible
    expect(renderedCh ? std::fabs(db) < 3.0 : db > -12.0, ("channel '" + c.name + "' sounds after the FL round trip").c_str());
  }
}

void testZip() {
  const Song demo = demoFile();
  const fs::path dir = scratch() / "zip";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  flp::PackageOptions po;
  po.flp.factoryData = flp::defaultFactoryData();
  flp::PackageReport pr;
  std::string err;
  const std::string zip = (dir / "demo1.zip").string();
  expect(flp::exportFlpZip(demo, zip, "demo1", po, pr, &err), "zipped loop package exports");
  int wavs = 0;
  for (const auto& [src, entry] : pr.files) wavs += entry.size() > 4 && entry.substr(entry.size() - 4) == ".wav";
  std::printf("# package: %zu files, %d rendered samples\n", pr.files.size(), wavs);
  expect(wavs >= 6, "the package holds the rendered samples");
  // Unpacked and opened again: the DirectWave zones find their samples beside the project.
  Song back;
  std::string proj;
  expect(unpackZippedProject(zip, (dir / "unpacked").string(), proj, &err), "package unpacks");
  flp::Report irep;
  expect(importFlp(proj, back, irep, &err, po.flp.factoryData), "package project imports");
  resolveRelativePaths(back, fs::path(proj).parent_path().string());
  int missing = 0, files = 0;
  for (const auto& c : back.channels) {
    if (c.plugin.type == "gl.sampler" && !c.plugin.data.empty()) ++files, missing += !fs::is_regular_file(c.plugin.data);
    if (c.plugin.type == "gl.waveprogram") {
      packs::samplers::dw::DwProgram p;
      if (packs::samplers::dw::parseProgram(c.plugin.data, p))
        for (const auto& z : p.zones) {
          ++files;
          const fs::path zp = fs::path(z.path).is_absolute() ? fs::path(z.path) : fs::path(proj).parent_path() / z.path;
          missing += !fs::is_regular_file(zp);
        }
    }
  }
  std::printf("# package re-import: %d sample references, %d missing\n", files, missing);
  expect(files > 0 && missing == 0, "package samples resolve after unpacking");
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  testDemo();
  testZip();
  testCatalogue();
  return finish();
}

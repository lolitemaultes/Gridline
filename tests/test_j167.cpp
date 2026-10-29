// Gridline — J-167: the project loader's progress / cancel model (engine/ProjectLoad.h).
//  * a load reads on a worker, then prepares every sample and plugin as its own work unit:
//    counts, monotonic progress, a log line per unit with its time, warnings for missing
//    samples and unavailable plugins;
//  * the prepared plugins are the ones the swap's build uses (nothing is created at the swap);
//  * a slow plugin (dev delay) takes a step of its own and is announced a step before it runs;
//  * cancel while reading, while preparing plugins, while waiting for samples: the open
//    project (document, undo, runtimes) is exactly as it was, nothing prepared is kept, the
//    abandoned reader is joined;
//  * a corrupt file fails with the reason; finish() runs a load to its end without frames.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "../src/core/Document.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/engine/ProjectLoad.h"
#include "../src/engine/Sample.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;
namespace fs = std::filesystem;
using projload::Level;
using projload::Loader;
using projload::Phase;

namespace {

std::string writeTone(const fs::path& dir, int i) {
  AudioBuffer b(2, 4410 + static_cast<size_t>(i) * 100);
  for (size_t f = 0; f < b.frames; ++f)
    b.data[f] = b.data[b.frames + f] = 0.3f * static_cast<float>(std::sin(static_cast<double>(f) * 0.05 * (1 + i)));
  const fs::path p = dir / ("tone" + std::to_string(i) + ".wav");
  writeWav(p.string(), b, 44100);
  return p.string();
}

constexpr int kSamples = 6;

// The new project: the starter channels, one sampler per sample, a sampler whose file is
// gone, an unavailable hosted plugin and two mixer effects.
Song newSong(const fs::path& dir, int& plugins) {
  Song s;
  s.mixer.clear();
  MixerTrack master;
  master.id = s.allocateId();
  master.name = "Master";
  s.mixer.push_back(master);
  MixerTrack ins;
  ins.id = s.allocateId();
  ins.name = "Drums";
  ins.slots.resize(kEffectSlots);
  ins.slots[0].plugin = defaultPluginState("gl.reverb");
  ins.slots[2].plugin = defaultPluginState("gl.delay");
  s.mixer.push_back(ins);
  auto add = [&](const std::string& name, const std::string& type, const std::string& data) {
    Channel c;
    c.id = s.allocateId();
    c.name = name;
    c.plugin = defaultPluginState(type);
    c.plugin.type = type;
    c.plugin.data = data;
    c.mixerTrack = ins.id;
    s.channels.push_back(c);
  };
  for (int i = 0; i < kSamples; ++i) add("Tone " + std::to_string(i), "gl.sampler", writeTone(dir, i));
  add("Gone", "gl.sampler", (dir / "missing-kick.wav").string());
  add("Lead", "gl.synth", "");
  add("Not here", "vst3:00000000000000000000000000000000", "");
  Pattern p;
  p.id = s.allocateId();
  p.name = "Pattern 1";
  s.patterns.push_back(p);
  plugins = static_cast<int>(s.channels.size()) + 2;
  return s;
}

// The open project: what every cancel must leave exactly as it was.
struct OldProject {
  Document doc;
  ProgramBuilder builder;
  std::string text;
  std::vector<const void*> runtimes;
  void init() {
    Song s;
    s.mixer.clear();
    MixerTrack master;
    master.id = s.allocateId();
    master.name = "Master";
    master.slots.resize(kEffectSlots);
    master.slots[1].plugin = defaultPluginState("gl.chorus");
    s.mixer.push_back(master);
    Channel c;
    c.id = s.allocateId();
    c.name = "Old kick";
    c.plugin = defaultPluginState("gl.kick");
    c.mixerTrack = master.id;
    s.channels.push_back(c);
    doc.reset(s);
    doc.addChannel("Old synth", "gl.synth");  // an undo step that must survive
    builder.build(doc.song());
    text = projectToText(doc.song());
    runtimes = snapshot();
  }
  std::vector<const void*> snapshot() const {
    std::vector<const void*> v;
    for (const auto& c : doc.song().channels) v.push_back(builder.channelRuntime(c.id).get());
    v.push_back(builder.effectRuntime(doc.song().mixer[0].id, 1).get());
    return v;
  }
  bool untouched() const { return projectToText(doc.song()) == text && doc.history().canUndo() && doc.history().position() == 1 && snapshot() == runtimes; }
};

bool hasLine(const Loader& L, Level lv, const std::string& part) {
  for (const auto& l : L.log())
    if (l.level == lv && l.text().find(part) != std::string::npos) return true;
  return false;
}

void runToEnd(Loader& L, int& steps, bool& monotonic) {
  double last = 0;
  monotonic = true;
  steps = 0;
  while (L.step(0.004) && steps < 200000) {
    ++steps;
    const double p = L.progress();
    if (p + 1e-9 < last) monotonic = false;
    last = p;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

}  // namespace

int main() {
  const fs::path dir = gl::test::hermeticTestEnv("j167");
  OldProject old;
  old.init();
  expect(!old.runtimes.empty() && old.runtimes.front() != nullptr, "the open project has its runtimes");

  int plugins = 0;
  const Song fresh = newSong(dir, plugins);
  const std::string path = (dir / "new.gridline.json").string();
  expect(saveProject(path, fresh), "fixture project saved");

  // ---- a whole load ----
  {
    Loader L;
    projload::Request r;
    r.path = path;
    L.start(r);
    expect(L.phase() == Phase::Reading && L.active() && L.unitsTotal() == 0, "reading first (units not known yet)");
    int steps = 0;
    bool mono = false;
    runToEnd(L, steps, mono);
    expect(L.phase() == Phase::Ready, "the load is ready");
    expect(L.samplesTotal() == kSamples && L.samplesDone() == kSamples, "every existing sample is a unit");
    expect(L.pluginsTotal() == plugins && L.pluginsDone() == plugins, "every instrument and effect is a unit");
    expect(L.unitsTotal() == 2 + kSamples + plugins && L.unitsDone() == L.unitsTotal() - 1, "units: reading + samples + plugins + build");
    expect(mono, "progress never goes back");
    expect(hasLine(L, Level::Warning, "missing-kick.wav"), "a missing sample is a warning");
    expect(hasLine(L, Level::Warning, "Plugin not available"), "an unavailable plugin is a warning");
    expect(hasLine(L, Level::Info, "Loaded sample tone0.wav"), "each sample is logged");
    expect(hasLine(L, Level::Info, "Started Reverb") || hasLine(L, Level::Info, "Started"), "each plugin is logged with its time");
    expect(L.warnings() >= 2, "the warnings are counted");
    expect(L.now().kind == projload::Now::Kind::Building, "the last step is the build");
    expect(L.result().song.channels.size() == fresh.channels.size(), "the new song is complete");
    // The swap: the prepared runtimes are the ones the build uses.
    ProgramBuilder& pre = L.prepared();
    const Song& s = L.result().song;
    const void* lead = pre.channelRuntime(s.channels[static_cast<size_t>(kSamples) + 1].id).get();
    const void* reverb = pre.effectRuntime(s.mixer[1].id, 0).get();
    expect(lead && reverb, "plugins were prepared before the swap");
    ProgramBuilder target;  // (the app's builder, holding the old project)
    target.build(old.doc.song());
    target.adoptPreloaded(std::move(pre));
    auto prog = target.build(s);
    expect(prog && target.channelRuntime(s.channels[static_cast<size_t>(kSamples) + 1].id).get() == lead &&
               target.effectRuntime(s.mixer[1].id, 0).get() == reverb,
           "the build after the swap reuses the prepared plugins (none started at the swap)");
    expect(!L.cancellable(), "a ready load cannot be cancelled");
    L.cancel();
    expect(L.phase() == Phase::Ready, "cancel does nothing once ready");
    L.clear();
    expect(L.phase() == Phase::Idle && L.log().empty(), "clear goes back to idle");
  }

  // ---- a slow plugin takes a step of its own, named a step before it runs ----
  {
    Loader L;
    projload::Request r;
    r.path = path;
    r.devDelayMs = 5;
    L.start(r);
    bool oneAtATime = true, announced = true;
    int last = 0, guard = 0;
    while (L.active() && ++guard < 100000) {
      const projload::Now before = L.now();
      L.step(0.001);
      const int d = L.pluginsDone() - last;
      if (d > 1) oneAtATime = false;
      if (d == 1 && !(before.kind == projload::Now::Kind::Plugin && before.index == last + 1)) announced = false;
      last = L.pluginsDone();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    expect(L.phase() == Phase::Ready && last == plugins, "the slow load ends");
    expect(oneAtATime, "one slow plugin per step (the interface draws between them)");
    expect(announced, "every slow plugin was named before its step ran");
  }

  // ---- cancel while reading ----
  {
    Loader L;
    projload::Request r;
    r.path = path;
    r.devDelayMs = 300;
    L.start(r);
    L.step();
    expect(L.phase() == Phase::Reading, "still reading");
    L.cancel();
    expect(L.phase() == Phase::Cancelled && !L.active(), "cancelled at once while reading");
    expect(L.retiredRunning() == 1, "the reader finishes in the background");
    expect(old.untouched(), "cancel while reading: the open project is exactly as it was");
    for (int i = 0; i < 400 && L.retiredRunning() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    L.joinFinished();
    expect(L.retiredRunning() == 0, "the abandoned reader is joined once done");
    L.start(r);  // a new load after a cancel works
    L.finish();
    expect(L.phase() == Phase::Ready, "a later load completes");
  }

  // ---- cancel while preparing plugins ----
  {
    Loader L;
    projload::Request r;
    r.path = path;
    r.devDelayMs = 2;
    L.start(r);
    int guard = 0;
    while (L.active() && L.pluginsDone() < 3 && ++guard < 100000) {
      L.step(0.001);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    expect(L.phase() == Phase::Preparing && L.pluginsDone() >= 3, "preparing plugins");
    const ObjectId first = L.result().song.mixer[1].id;
    expect(L.prepared().effectRuntime(first, 0) != nullptr, "some plugins are prepared");
    L.cancel();
    expect(L.phase() == Phase::Cancelled, "cancelled while preparing plugins");
    expect(L.prepared().effectRuntime(first, 0) == nullptr, "the prepared plugins are destroyed");
    expect(hasLine(L, Level::Info, "Cancelled after"), "the cancel is logged");
    expect(old.untouched(), "cancel while preparing plugins: the open project is exactly as it was");
  }

  // ---- cancel while waiting for samples ----
  {
    sampleCache().clear();
    sampleCache().pauseBackground(true);  // the decoders hold: samples stay pending
    Loader L;
    projload::Request r;
    r.path = path;
    L.start(r);
    int guard = 0;
    while (L.active() && (L.phase() == Phase::Reading || L.pluginsDone() < L.pluginsTotal()) && ++guard < 100000) {
      L.step(0.01);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    L.step(0.01);
    expect(L.phase() == Phase::Preparing && L.samplesDone() < L.samplesTotal(), "plugins done, samples still decoding");
    expect(L.now().kind == projload::Now::Kind::Sample && L.now().name.find("tone") == 0, "the step names the sample it waits for");
    L.cancel();
    expect(L.phase() == Phase::Cancelled && L.samplesDone() == 0, "cancelled while waiting for samples");
    sampleCache().pauseBackground(false);
    expect(old.untouched(), "cancel while waiting for samples: the open project is exactly as it was");
  }

  // ---- failures ----
  {
    const std::string bad = (dir / "broken.gridline.json").string();
    std::ofstream(bad) << "{\"version\": 9, \"channels\": [ {";
    Loader L;
    projload::Request r;
    r.path = bad;
    L.start(r);
    L.finish();
    expect(L.phase() == Phase::Failed && !L.error().empty(), "a corrupt file fails with a reason");
    expect(hasLine(L, Level::Error, "Could not open project"), "the failure is in the log");
    expect(old.untouched(), "a failed load leaves the open project as it was");
    projload::Request m;
    m.path = (dir / "nowhere.gridline.json").string();
    L.start(m);
    L.finish();
    expect(L.phase() == Phase::Failed, "a missing file fails");
  }

  // ---- a new start while loading replaces the running load ----
  {
    Loader L;
    projload::Request r;
    r.path = path;
    r.devDelayMs = 100;
    L.start(r);
    r.devDelayMs = 0;
    L.start(r);
    L.finish();
    expect(L.phase() == Phase::Ready && L.pluginsDone() == plugins, "the newest request wins");
    L.shutdown();
    expect(L.phase() == Phase::Idle && L.retiredRunning() == 0, "shutdown joins every reader");
  }
  return finish();
}

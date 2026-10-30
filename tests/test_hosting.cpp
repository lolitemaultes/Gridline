// Gridline — plugin hosting tests beyond VST3 basics: CLAP, VST2, AU (macOS,
// Apple's built-in units), MIDI controllers / programs / multi-output for
// every format, the out-of-process sandbox, and FL "Fruity Wrapper" import.
// Uses the in-tree test plugins only (tests/clap, tests/vst2, tests/vst3).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpWrapper.h"
#include "engine/AudioEngine.h"
#include "engine/Render.h"
#include "host/EditorLayout.h"
#include "host/HostedPlugins.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"
#include "host/vst3/Vst3Module.h"
#include "host/vst3/Vst3Scan.h"
#include "engine/Program.h"
#include "clap/Probe.h"
#include "vst2/Probe.h"
#include "vst3/Probe.h"
#include "host/Dylib.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

const std::string kClapBundle = GRIDLINE_CLAP_TESTPLUG;
const std::string kVst2Bundle = GRIDLINE_VST2_TESTPLUG;
const std::string kVst3Bundle = GRIDLINE_TESTPLUG_BUNDLE;
const std::string kClapSynth = "clap:org.gridline.test.synth";
const std::string kClapGain = "clap:org.gridline.test.gain";
const std::string kVst2Synth = "vst2:476C5432";
const std::string kVst2Level = "vst2:476C5433";
const std::string kVst3Synth = "vst3:47524944" "4C494E45" "54535931" "00000001";

constexpr uint32_t fcc(const char (&s)[5]) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

ProcessContext ctx48() {
  ProcessContext c;
  c.sampleRate = 48000;
  c.tempo = 120;
  return c;
}

// Sandbox data-path checks render offline: on a slow (virtual) machine a real-time block may
// miss its budget and come back silent, which is the sandbox working as designed.
bool gOfflineRuns = false;
struct OfflineScope {
  OfflineScope() { gOfflineRuns = true; }
  ~OfflineScope() { gOfflineRuns = false; }
};
void runGen(Generator& g, const PluginState& st, std::vector<float>& L, std::vector<float>& R, int n, int block = 256) {
  ChannelAux aux;
  ProcessContext c = ctx48();
  c.offline = c.offline || gOfflineRuns;
  size_t base = L.size();
  L.resize(base + static_cast<size_t>(n), 0.0f);
  R.resize(base + static_cast<size_t>(n), 0.0f);
  for (int pos = 0; pos < n; pos += block) {
    int len = std::min(block, n - pos);
    g.process(st, aux, c, L.data() + base + pos, R.data() + base + pos, len);
  }
}
float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}
int crossings(const std::vector<float>& v, size_t from, size_t to) {
  int c = 0;
  for (size_t i = from + 1; i < to && i < v.size(); ++i) c += (v[i - 1] < 0) != (v[i] < 0);
  return c;
}
int firstNonZero(const std::vector<float>& v) {
  for (size_t i = 0; i < v.size(); ++i)
    if (std::fabs(v[i]) > 1e-7f) return static_cast<int>(i);
  return -1;
}
const host::ScannedClass* findClass(const host::ScanResult& r, const std::string& uid) {
  for (const auto& c : r.classes)
    if (c.uid == uid) return &c;
  return nullptr;
}
bool waitFor(const std::function<bool()>& f, double seconds) {
  auto t0 = std::chrono::steady_clock::now();
  while (!f()) {
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// ---------------------------------------------------------------- formats & helpers

void testHelpers() {
  expect(host::findFormat("vst3") && host::findFormat("clap") && host::findFormat("vst2"), "formats: VST3, CLAP, VST2");
#if defined(__APPLE__)
  expect(host::findFormat("au") != nullptr, "formats: AU on macOS");
#endif
  std::string f, u;
  expect(host::splitType("clap:org.x.y", f, u) && f == "clap" && u == "org.x.y", "type split");
  expect(!host::splitType("gl.synth", f, u), "built-in types are not hosted");
  expect(isHostedType("clap:a") && isHostedType("vst2:00000001") && isHostedType("au:aufx/dely/appl") && !isHostedType("gl.reverb"),
         "hosted type prefixes");
  expect(host::auKey(fcc("aufx"), fcc("dely"), fcc("appl")) == "aufx/dely/appl" && host::fourCharCode(0x01020304) == "0x01020304", "AU keys");
  expect(host::vst2Type(0x434E4B53) == "vst2:434E4B53", "VST2 type id");

  host::HostEventQueue q;
  bool pushed = true;
  for (int i = 0; i < 100; ++i) pushed = pushed && q.push({host::HostEvent::PerformEdit, static_cast<uint32_t>(i), i * 0.01, 0});
  host::HostEvent e;
  int n = 0;
  bool order = true;
  while (q.pop(e)) order = order && e.id == static_cast<uint32_t>(n++);
  expect(pushed && n == 100 && order, "host event queue: FIFO");
  // Producers on several threads.
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&q, t]() {
      for (int i = 0; i < 200; ++i) q.push({host::HostEvent::BeginEdit, static_cast<uint32_t>(t * 1000 + i), 0, 0});
    });
  for (auto& t : ts) t.join();
  n = 0;
  while (q.pop(e)) ++n;
  expect(n == 800, "host event queue: concurrent producers lose nothing");

  // Cache round trip keeps formats apart.
  host::ScanResult a, b;
  a.format = "clap";
  a.bundle = "/x/A.clap";
  a.ok = true;
  a.classes.push_back(host::ScannedClass{});
  a.classes[0].uid = "org.a";
  a.classes[0].auxOutputs = {"Sub"};
  a.classes[0].programs = {"P1", "P2"};
  b.format = "vst2";
  b.bundle = "/x/A.clap";  // same path, other format
  b.error = "crashed";
  fs::path tmp = fs::temp_directory_path() / "gridline-hosting-cache.json";
  host::ScanCache c(tmp.string());
  c.put(a);
  c.put(b);
  expect(c.save(), "cache saved");
  host::ScanCache c2(tmp.string());
  expect(c2.load() && c2.entries().size() == 2, "cache: entries of two formats with the same path");
  const host::ScanResult* ra = c2.find("/x/A.clap", "clap");
  expect(ra && ra->ok && ra->classes.size() == 1 && ra->classes[0].auxOutputs.size() == 1 && ra->classes[0].programs.size() == 2,
         "cache: CLAP entry with outputs and programs");
  const host::ScanResult* rb = c2.find("/x/A.clap", "vst2");
  expect(rb && !rb->ok && rb->error == "crashed", "cache: failed entry kept with its reason");
  fs::remove(tmp);
}

// ---------------------------------------------------------------- CLAP

void testClap() {
  host::ScanResult r = host::findFormat("clap")->scanInProcess(kClapBundle);
  expect(r.ok && r.classes.size() == 2, ("CLAP scan: 2 plugins " + r.error).c_str());
  const host::ScannedClass* synth = findClass(r, "org.gridline.test.synth");
  const host::ScannedClass* gain = findClass(r, "org.gridline.test.gain");
  expect(synth && synth->instrument && synth->eventInput && synth->outputChannels == 2 && synth->params.size() == 4,
         "CLAP synth: instrument, notes, stereo, 4 parameters");
  expect(synth && synth->auxOutputs.size() == 1 && synth->auxOutputs[0] == "Sub", "CLAP synth: extra output \"Sub\"");
  expect(synth && synth->params.size() == 4 && synth->params[2].stepCount == 2 && synth->params[2].labels.size() == 3 &&
             synth->params[2].labels[2] == "Fold",
         "CLAP stepped parameter scanned with value names");
  expect(gain && !gain->instrument && gain->inputChannels == 2 && gain->sidechainChannels == 2 && gain->latency == 16,
         "CLAP effect: stereo in, sidechain, latency 16");
  host::ScanResult iso = host::scanIsolated(GRIDLINE_CLI_EXE, "clap", kClapBundle, 30);
  expect(iso.ok && iso.classes.size() == 2, ("CLAP isolated scan " + iso.error).c_str());
  host::registerScanResult(r);
  const PluginInfo* pi = findPlugin(kClapSynth);
  expect(pi && pi->kind == PluginKind::Generator && pi->params.size() == 4 && pi->params[1].kind == ParamKind::Toggle &&
             pi->params[2].kind == ParamKind::Choice,
         "CLAP plugin registered as an instrument type");

  auto g = createGenerator(kClapSynth);
  expect(g != nullptr, ("CLAP instrument instantiates " + host::lastError()).c_str());
  if (!g) return;
  host::HostedInstance* hi = host::hostedInstance(g.get());
  PluginState st = defaultPluginState(kClapSynth);
  g->prepareHost(st, 48000);
  host::UiUpdate u;
  hi->pumpUi(u);
  expect(u.readback && u.values.size() == 4 && std::fabs(u.values[0] - 0.5f) < 1e-6f, "CLAP parameters read back");
  std::vector<float> L, R;
  g->noteOn(1, 69.0, 1.0f, 100);
  g->noteOff(1, 300);
  runGen(*g, st, L, R, 512, 512);
  expect(firstNonZero(L) == 100 && peak(L, 301) == 0.0f, "CLAP notes are sample-accurate");
  const float before = peak(L, 100, 300);
  st.params[0] = 1.0f;
  L.clear();
  R.clear();
  g->noteOn(2, 69.0, 1.0f, 0);
  runGen(*g, st, L, R, 2048);
  expect(std::fabs(peak(L) - 2 * before) < 0.02f, "CLAP parameter change reaches the plugin");
  // Multi-output: the "Sub" output plays one octave lower.
  const float* aL = nullptr;
  const float* aR = nullptr;
  expect(g->auxOutputCount() == 1 && g->auxOutput(0, aL, aR) && aL, "CLAP extra output available to the engine");
  // MIDI pitch bend (up 2 semitones) raises the frequency.
  L.clear();
  R.clear();
  runGen(*g, st, L, R, 9600);
  g->midi(0xE0, 0x7F, 0x7F, 0);
  runGen(*g, st, L, R, 9600);
  const int c0 = crossings(L, 0, 9600), c1 = crossings(L, 9600, 19200);
  expect(c0 > 0 && c1 > c0 + c0 / 16, "CLAP: MIDI pitch bend raises the pitch");
  g->midi(0xE0, 0x00, 0x40, 0);
  // Glide through the tuning note expression.
  expect(g->supportsGlide(), "CLAP: glide through tuning expressions");
  // An edit made in the plugin's editor (CC 20 test hook) comes back as a song edit.
  g->midi(0xB0, 20, 127, 0);
  runGen(*g, st, L, R, 256);
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(u.edits.size() == 1 && u.edits[0].index == 0 && std::fabs(u.edits[0].value - 1.0f) < 1e-6f && u.gestureEnd,
         "CLAP editor edit → parameter edit + gesture end");
  // State round trip.
  std::string data;
  expect(hi->captureState(data) && data.compare(0, 6, "clap1:") == 0, "CLAP state captured");
  auto g2 = createGenerator(kClapSynth);
  PluginState st2 = defaultPluginState(kClapSynth);
  st2.data = data;
  g2->prepareHost(st2, 48000);
  std::string data2;
  host::hostedInstance(g2.get())->captureState(data2);
  expect(data == data2, "CLAP state survives save/restore");
  expect(std::string(hi->backend()->format()) == "CLAP" && hi->paramText(2, 2.0f) == "Fold", "CLAP parameter text");

  // Effect: latency, level, sidechain.
  auto fx = createEffect(kClapGain);
  expect(fx != nullptr, "CLAP effect instantiates");
  if (!fx) return;
  PluginState fs1 = defaultPluginState(kClapGain);
  fx->prepareHost(fs1, 48000);
  expect(fx->latencySamples() == 16, "CLAP latency reported");
  const int n = 256;
  std::vector<float> in(n), x(n), y(n), scL(n, 0.25f), scR(n, 0.25f);
  for (int i = 0; i < n; ++i) in[static_cast<size_t>(i)] = std::sin(i * 0.05f) * 0.5f;
  x = in;
  y = in;
  ProcessContext c = ctx48();
  fx->processAux(fs1, ChannelAux{}, c, x.data(), y.data(), n);
  expect(std::fabs(x[100] - in[100 - 16]) < 1e-6f, "CLAP effect passes audio with its latency");
  fs1.params[1] = 1.0f;  // sidechain mix
  fs1.params[0] = 0.0f;
  SideIo io;
  io.scL = scL.data();
  io.scR = scR.data();
  fx->setSideIo(io);
  x = in;
  y = in;
  fx->processAux(fs1, ChannelAux{}, c, x.data(), y.data(), n);
  x = in;
  fx->processAux(fs1, ChannelAux{}, c, x.data(), y.data(), n);
  expect(std::fabs(x[100] - 0.25f) < 1e-6f, "CLAP sidechain input reaches the plugin");
}

// ---------------------------------------------------------------- VST2

void testVst2() {
  host::ScanResult r = host::findFormat("vst2")->scanInProcess(kVst2Bundle);
  expect(r.ok && r.classes.size() == 2, ("VST2 shell scan: 2 sub-plugins " + r.error).c_str());
  const host::ScannedClass* synth = findClass(r, "476C5432");
  const host::ScannedClass* lvl = findClass(r, "476C5433");
  expect(synth && synth->instrument && synth->name == "Gridline Test VST2 Synth" && synth->params.size() == 2 && synth->programs.size() == 3,
         "VST2 synth: instrument, 2 parameters, 3 programs");
  expect(synth && synth->auxOutputs.size() == 1 && synth->auxOutputs[0] == "Sub", "VST2 synth: 4 outputs = main + \"Sub\"");
  expect(lvl && !lvl->instrument && lvl->inputChannels == 2, "VST2 effect sub-plugin");
  host::ScanResult iso = host::scanIsolated(GRIDLINE_CLI_EXE, "vst2", kVst2Bundle, 30);
  expect(iso.ok && iso.classes.size() == 2, ("VST2 isolated scan " + iso.error).c_str());
  host::registerScanResult(r);

  auto g = createGenerator(kVst2Synth);
  expect(g != nullptr, ("VST2 instrument instantiates " + host::lastError()).c_str());
  if (!g) return;
  host::HostedInstance* hi = host::hostedInstance(g.get());
  PluginState st = defaultPluginState(kVst2Synth);
  g->prepareHost(st, 48000);
  std::vector<float> L, R;
  g->noteOn(1, 69.0, 1.0f, 0);
  runGen(*g, st, L, R, 9600);
  g->midi(0xE0, 0x7F, 0x7F, 0);
  runGen(*g, st, L, R, 9600);
  const int c0 = crossings(L, 0, 9600), c1 = crossings(L, 9600, 19200);
  expect(peak(L) > 0.3f && c1 > c0 + c0 / 16, "VST2: notes sound, pitch bend raises the pitch");
  g->midi(0xE0, 0, 0x40, 0);
  const float* aL = nullptr;
  const float* aR = nullptr;
  expect(g->auxOutputCount() == 1 && g->auxOutput(0, aL, aR), "VST2 extra output");
  // Programs: the plugin's own presets.
  expect(hi->programCount() == 3 && hi->programName(2) == "Loud", "VST2 program names");
  host::UiUpdate u;
  hi->pumpUi(u);
  expect(hi->selectProgram(2), "VST2 program selected");
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(u.readback && std::fabs(u.values[0] - 1.0f) < 1e-6f && u.captureState, "VST2 program: parameters read back, state captured");
  // Editor edit through the host callback (CC 20 test hook).
  g->midi(0xB0, 20, 0, 0);
  runGen(*g, st, L, R, 256);
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(!u.edits.empty() && u.edits[0].index == 0 && u.edits[0].value == 0.0f && u.gestureEnd, "VST2 editor edit → song edit");
  // Parameter-list state.
  std::string data;
  expect(hi->captureState(data) && data.compare(0, 7, "vst2p1:") == 0, "VST2 parameter state captured");
  auto g2 = createGenerator(kVst2Synth);
  PluginState st2 = st;
  st2.data = data;
  g2->prepareHost(st2, 48000);
  u = host::UiUpdate{};
  host::hostedInstance(g2.get())->pumpUi(u);
  expect(u.readback && u.values[0] == 0.0f, "VST2 parameter state restored");

  // Chunk state.
  auto fx = createEffect(kVst2Level);
  expect(fx != nullptr, "VST2 effect instantiates");
  if (!fx) return;
  PluginState fs1 = defaultPluginState(kVst2Level);
  fx->prepareHost(fs1, 48000);
  fs1.params[0] = 0.25f;  // an edit after loading
  std::vector<float> x(64, 0.5f), y(64, 0.5f);
  ProcessContext c = ctx48();
  fx->processAux(fs1, ChannelAux{}, c, x.data(), y.data(), 64);
  expect(std::fabs(x[10] - 0.25f) < 1e-6f, "VST2 effect parameter applied");
  std::string chunk;
  host::HostedInstance* fi = host::hostedInstance(fx.get());
  expect(fi->captureState(chunk) && chunk.compare(0, 7, "vst2c1:") == 0, "VST2 chunk state captured");
  auto fx2 = createEffect(kVst2Level);
  PluginState fs2 = defaultPluginState(kVst2Level);
  fs2.data = chunk;
  fx2->prepareHost(fs2, 48000);
  u = host::UiUpdate{};
  host::hostedInstance(fx2.get())->pumpUi(u);
  expect(u.readback && std::fabs(u.values[0] - 0.25f) < 1e-6f, "VST2 chunk state restored");
}

// ---------------------------------------------------------------- VST3 gaps

void testVst3Extras() {
  host::ScanResult r = vst3::scanBundleInProcess(kVst3Bundle);
  host::registerScanResult(r);
  const host::ScannedClass* synth = nullptr;
  for (const auto& c : r.classes)
    if (c.instrument) synth = &c;
  expect(synth && synth->programs.size() == 3 && synth->programs[2] == "Loud", "VST3 unit program list scanned");
  expect(synth && synth->auxOutputs.size() == 1 && synth->auxOutputs[0] == "Sub", "VST3 aux output bus scanned");
  auto g = createGenerator(kVst3Synth);
  if (!g) return;
  host::HostedInstance* hi = host::hostedInstance(g.get());
  PluginState st = defaultPluginState(kVst3Synth);
  g->prepareHost(st, 48000);
  std::vector<float> L, R;
  g->noteOn(1, 69.0, 1.0f, 0);
  runGen(*g, st, L, R, 9600);
  g->midi(0xE0, 0x7F, 0x7F, 0);  // IMidiMapping: pitch bend → Bend parameter
  runGen(*g, st, L, R, 9600);
  const int c0 = crossings(L, 0, 9600), c1 = crossings(L, 9600, 19200);
  expect(c0 > 0 && c1 > c0 + c0 / 16, "VST3: MIDI pitch bend reaches the plugin through IMidiMapping");
  g->midi(0xE0, 0, 0x40, 0);
  const float lv = peak(L, 9600);
  g->midi(0xB0, 7, 127, 0);  // CC 7 → Gain
  L.clear();
  R.clear();
  runGen(*g, st, L, R, 4800);
  expect(peak(L, 2400) > lv * 1.8f, "VST3: MIDI CC mapped to a parameter");
  const float* aL = nullptr;
  const float* aR = nullptr;
  expect(g->auxOutputCount() == 1 && g->auxOutput(0, aL, aR) && peak(std::vector<float>(aL, aL + 256)) > 0.1f,
         "VST3 aux output bus renders");
  host::UiUpdate u;
  hi->pumpUi(u);
  expect(hi->programCount() == 3 && hi->selectProgram(0), "VST3 program selected");
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(u.readback && std::fabs(u.values[0] - 0.25f) < 1e-6f, "VST3 program: parameters read back");
  L.clear();
  R.clear();
  g->noteOn(5, 69.0, 1.0f, 0);
  runGen(*g, st, L, R, 2400);
  expect(std::fabs(peak(L) - 0.25f) < 0.02f, "VST3 program change reaches the processor");
  const PluginInfo* info = findPlugin(kVst3Synth);
  expect(info && info->presets.size() == 3 && info->presets[1].name == "Normal", "programs listed as the plugin's presets");
}

// Multi-output routing through the engine: extra outputs → their own mixer tracks.
void testMultiOutRender() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  ObjectId pat = d.song().patterns[0].id;
  ObjectId ch = d.addChannel("multi", kVst3Synth);
  d.addNote(pat, ch, {0, 96, 69, 127});
  d.addClip(pat, 0, 0);
  ObjectId t1 = d.song().mixer[1].id, t2 = d.song().mixer[2].id;
  d.routeChannel(ch, t1);
  d.editSong("mute", [&](Song& s) { s.mixerTrack(t1)->muted = true; });
  RenderOptions o;
  o.blockSize = 256;
  AudioBuffer silent = renderSong(d.song(), o);
  expect(silent.peak() < 1e-6f, "main output on a muted track: silence");
  d.editChannel(ch, "route", [&](Channel& c) { c.auxOuts = {t2}; });
  AudioBuffer out = renderSong(d.song(), o);
  std::vector<float> left(out.plane(0), out.plane(0) + out.frames);
  const size_t a = 2000, b = 2000 + 11025;
  const int cr = crossings(left, a, b);  // 220 Hz over 0.25 s ≈ 110 crossings
  expect(out.peak() > 0.2f && cr > 90 && cr < 130, "extra output plays on its own mixer track (sub octave)");
  Json j = songToJson(d.song());
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err) && back.channel(ch) && back.channel(ch)->auxOuts.size() == 1 && back.channel(ch)->auxOuts[0] == t2,
         "output routing saved in the project");
  d.removeMixerTrack(t2);
  expect(d.song().channel(ch)->auxOuts.size() == 1 && d.song().channel(ch)->auxOuts[0] == kNoId, "deleting the track clears the routing");
}

// ---------------------------------------------------------------- AU (macOS)

void testAu() {
#if defined(__APPLE__)
  const host::Format* au = host::findFormat("au");
  std::vector<std::string> all = au->discover({});
  const bool hasDelay = std::find(all.begin(), all.end(), "aufx/dely/appl") != all.end();
  const bool hasDls = std::find(all.begin(), all.end(), "aumu/dls /appl") != all.end();
  expect(hasDelay && hasDls, "AU discovery lists Apple's delay and DLS synth");
  host::ScanResult r = au->scanInProcess("aufx/dely/appl");
  expect(r.ok && r.classes.size() == 1 && r.classes[0].name == "AUDelay" && r.classes[0].vendor == "Apple" && !r.classes[0].instrument,
         ("AU scan: AUDelay " + r.error).c_str());
  host::registerScanResult(r);
  host::ScanResult r2 = au->scanInProcess("aumu/dls /appl");
  expect(r2.ok && r2.classes.size() == 1 && r2.classes[0].instrument, "AU scan: DLS synth is an instrument");
  host::registerScanResult(r2);
  host::ScanResult iso = host::scanIsolated(GRIDLINE_CLI_EXE, "au", "aufx/dely/appl", 30);
  expect(iso.ok && iso.classes.size() == 1, ("AU isolated scan " + iso.error).c_str());

  auto fx = createEffect("au:aufx/dely/appl");
  expect(fx != nullptr, ("AU effect instantiates " + host::lastError()).c_str());
  if (fx) {
    PluginState st = defaultPluginState("au:aufx/dely/appl");
    fx->prepareHost(st, 48000);
    const int n = 512;
    std::vector<float> x(n), y(n);
    ProcessContext c = ctx48();
    float pk = 0;
    for (int b = 0; b < 120; ++b) {  // > 1 s: the default delay time
      for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = y[static_cast<size_t>(i)] = b == 0 && i < 64 ? 0.5f : 0.0f;
      fx->processAux(st, ChannelAux{}, c, x.data(), y.data(), n);
      if (b > 0) pk = std::max(pk, peak(x));
    }
    expect(pk > 0.01f, "AU delay repeats an impulse");
    host::HostedInstance* hi = host::hostedInstance(fx.get());
    host::UiUpdate u;
    hi->pumpUi(u);
    std::string data;
    expect(hi->captureState(data) && data.compare(0, 4, "au1:") == 0, "AU ClassInfo state captured");
    auto fx2 = createEffect("au:aufx/dely/appl");
    PluginState st2 = st;
    st2.data = data;
    fx2->prepareHost(st2, 48000);
    std::string data2;
    host::hostedInstance(fx2.get())->captureState(data2);
    expect(data == data2, "AU state survives save/restore");
    expect(!hi->paramText(0, st.params.empty() ? 0.f : st.params[0]).empty(), "AU parameter text");
  }
  auto g = createGenerator("au:aumu/dls /appl");
  expect(g != nullptr, "AU instrument instantiates");
  if (g) {
    PluginState st = defaultPluginState("au:aumu/dls /appl");
    g->prepareHost(st, 48000);
    std::vector<float> L, R;
    g->noteOn(1, 60.0, 0.9f, 0);
    runGen(*g, st, L, R, 9600, 512);
    g->midi(0xE0, 0x7F, 0x7F, 0);
    g->midi(0xB0, 1, 100, 0);
    runGen(*g, st, L, R, 4800, 512);
    expect(peak(L) > 0.01f, "AU instrument plays a note (MIDI)");
  }
#endif
}

// ---------------------------------------------------------------- sandbox

void testSandbox() {
  // The data-path checks wait for every block (offline rendering, bounded by the sandbox's 30 s
  // offline budget): a real-time block that misses its budget (4 block lengths: 21 ms for 256
  // frames at 48 kHz) comes back silent by design, which a loaded CI machine hits now and then.
  OfflineScope offline;
  host::setChildExecutable(GRIDLINE_CLI_EXE);
  host::setRunInSandbox(kClapSynth, true);
  auto g = createGenerator(kClapSynth);
  expect(g != nullptr, ("sandboxed instrument starts " + host::lastError()).c_str());
  if (!g) return;
  host::HostedInstance* hi = host::hostedInstance(g.get());
  expect(hi->sandboxed() && std::string(hi->backend()->format()) == "CLAP (sandbox)", "instance runs in the sandbox");
  PluginState st = defaultPluginState(kClapSynth);
  g->prepareHost(st, 48000);
  host::UiUpdate u;
  hi->pumpUi(u);
  expect(u.readback && u.values.size() == 4 && std::fabs(u.values[0] - 0.5f) < 1e-6f, "sandbox: parameters read back");
  std::vector<float> L, R;
  g->noteOn(1, 69.0, 1.0f, 100);
  g->noteOff(1, 300);
  runGen(*g, st, L, R, 512, 512);
  expect(firstNonZero(L) == 100 && peak(L, 301) == 0.0f && peak(L) > 0.4f, "sandbox: sample-accurate notes through shared memory");
  // Same output as in this process.
  host::setRunInSandbox(kClapSynth, false);
  auto ref = createGenerator(kClapSynth);
  host::setRunInSandbox(kClapSynth, true);
  ref->prepareHost(st, 48000);
  std::vector<float> L1, R1, L2, R2;
  g->noteOn(2, 64.0, 0.7f, 33);
  ref->noteOn(2, 64.0, 0.7f, 33);
  runGen(*g, st, L1, R1, 2048);
  runGen(*ref, st, L2, R2, 2048);
  bool same = L1.size() == L2.size();
  for (size_t i = 0; same && i < L1.size(); ++i) same = std::fabs(L1[i] - L2[i]) < 1e-6f;
  expect(same, "sandbox output identical to in-process output");
  expect(g->auxOutputCount() == 1, "sandbox: extra outputs");
  // Editor edits travel back through the control channel.
  g->midi(0xB0, 20, 127, 0);
  runGen(*g, st, L, R, 256);
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(u.edits.size() == 1 && std::fabs(u.edits[0].value - 1.0f) < 1e-6f && u.gestureEnd, "sandbox: editor edit → song edit");
  std::string data;
  expect(hi->captureState(data) && data.compare(0, 6, "clap1:") == 0, "sandbox: state captured");
  expect(hi->paramText(2, 1.0f) == "Hard", "sandbox: parameter text");
  expect(!g->stale(), "sandboxed instance is current");
  host::setRunInSandbox(kClapSynth, false);
  expect(g->stale(), "switching the sandbox off asks for a new instance");
  host::setRunInSandbox(kClapSynth, true);

  // A crash: the plugin process dies, the host goes on, the output is silent.
  g->noteOn(3, 1.0, 1.0f, 0);  // key 1: the test plugin aborts
  L.clear();
  R.clear();
  runGen(*g, st, L, R, 512);
  expect(waitFor([&] { return hi->crashed(); }, 5.0), "sandbox: plugin crash detected");
  expect(hi->crashReason().find("crashed") != std::string::npos, ("sandbox: crash reason: " + hi->crashReason()).c_str());
  L.clear();
  R.clear();
  g->noteOn(4, 69.0, 1.0f, 0);
  runGen(*g, st, L, R, 1024);
  expect(peak(L) == 0.0f, "sandbox: crashed plugin is silent");
  u = host::UiUpdate{};
  hi->pumpUi(u);
  host::UiUpdate u2;
  hi->pumpUi(u2);
  expect(u.crashed && !u2.crashed, "sandbox: crash reported to the UI once");
  std::string none;
  expect(!hi->captureState(none), "sandbox: no state from a crashed plugin (the song keeps the last one)");
  hi->requestReload();
  expect(g->stale(), "reload requested: next program build replaces the instance");
  auto g2 = createGenerator(kClapSynth);
  PluginState st2 = st;
  st2.data = data;
  g2->prepareHost(st2, 48000);
  L.clear();
  R.clear();
  g2->noteOn(1, 69.0, 1.0f, 0);
  runGen(*g2, st2, L, R, 1024);
  expect(peak(L) > 0.4f && !host::hostedInstance(g2.get())->crashed(), "sandbox: reloaded plugin plays again");

  // Effects in the sandbox, sidechain included.
  host::setRunInSandbox(kClapGain, true);
  auto fx = createEffect(kClapGain);
  expect(fx && host::hostedInstance(fx.get())->sandboxed(), "sandboxed effect starts");
  if (fx) {
    PluginState fs1 = defaultPluginState(kClapGain);
    fx->prepareHost(fs1, 48000);
    expect(fx->latencySamples() == 16, "sandbox: latency reported");
    std::vector<float> x(256), y(256), in(256);
    for (int i = 0; i < 256; ++i) in[static_cast<size_t>(i)] = std::sin(i * 0.05f) * 0.5f;
    x = in;
    y = in;
    ProcessContext c = ctx48();
    c.offline = gOfflineRuns;
    fx->processAux(fs1, ChannelAux{}, c, x.data(), y.data(), 256);
    host::HostedInstance* fi = host::hostedInstance(fx.get());
    char why[512];
    std::snprintf(why, sizeof why, "sandbox: effect processes audio (out[100] %.6f, in[84] %.6f; %s; %s)", static_cast<double>(x[100]),
                  static_cast<double>(in[84]), fi->backend()->diagnostics().c_str(),
                  fi->crashed() ? fi->crashReason().c_str() : "running");
    expect(std::fabs(x[100] - in[84]) < 1e-6f, why);
  }
  host::setRunInSandbox(kClapGain, false);
  host::setRunInSandbox(kClapSynth, false);
}

// ---------------------------------------------------------------- FL "Fruity Wrapper"

struct Rec {
  std::vector<uint8_t> b;
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void bytes(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
  void rec(uint32_t id, const std::vector<uint8_t>& data) {
    u32(id);
    u32(static_cast<uint32_t>(data.size()));
    u32(0);
    b.insert(b.end(), data.begin(), data.end());
  }
  void rec(uint32_t id, const std::string& s) { rec(id, std::vector<uint8_t>(s.begin(), s.end())); }
};

std::vector<uint8_t> u32v(uint32_t v) {
  Rec r;
  r.u32(v);
  return r.b;
}

std::vector<uint8_t> wrapper(uint32_t kind, const std::vector<uint8_t>& id51, const std::vector<uint8_t>& id52, const std::vector<uint8_t>& state,
                             const std::string& name, const std::string& vendor) {
  Rec w;
  w.u32(12);
  w.rec(1, std::vector<uint8_t>(20, 0));
  w.rec(2, std::vector<uint8_t>(25, 0));
  w.rec(50, [&] {
    Rec x;
    x.u32(kind);
    x.u32(0);
    x.u32(0);
    x.u32(0);
    return x.b;
  }());
  if (!id51.empty()) w.rec(51, id51);
  if (!id52.empty()) w.rec(52, id52);
  w.rec(54, name);
  if (!vendor.empty()) w.rec(56, vendor);
  w.rec(53, state);
  return w.b;
}

void testWrapper() {
  // VST2, parameter list state: id 'GlT2' (installed), gain 0.75.
  Rec s2;
  s2.i32(-9);
  s2.b.push_back(13);
  s2.i32(-2);
  s2.u32(0);
  for (int i = 0; i < 8; ++i) s2.b.push_back(0);
  s2.u32(2);
  float vals[2] = {0.75f, 1.0f};
  s2.b.insert(s2.b.end(), reinterpret_cast<uint8_t*>(vals), reinterpret_cast<uint8_t*>(vals) + 8);
  std::vector<uint8_t> w2 = wrapper(4, u32v(0x476C5432), {}, s2.b, "Gridline Test VST2 Synth", "Gridline Tests");
  flp::WrapperInfo wi;
  expect(flp::parseWrapper(w2.data(), w2.size(), wi) && wi.format == flp::WrapperInfo::Vst2 && wi.instrument && wi.vst2Id == 0x476C5432 &&
             wi.data.compare(0, 7, "vst2p1:") == 0,
         "wrapper: VST2 instance parsed (parameter list)");
  flp::WrapperMatch m = flp::matchWrapper(wi);
  expect(m.type == kVst2Synth && m.byId && m.stateKept, "wrapper: VST2 matched by unique id");
  auto g = createGenerator(m.type);
  PluginState st = defaultPluginState(m.type);
  st.data = wi.data;
  g->prepareHost(st, 48000);
  host::UiUpdate u;
  host::hostedInstance(g.get())->pumpUi(u);
  expect(u.readback && std::fabs(u.values[0] - 0.75f) < 1e-6f && u.values[1] == 1.0f, "wrapper: VST2 parameter values restored");

  // VST2 chunk: 'GlT3', chunk "GLT3" + level 0.125.
  Rec c3;
  c3.i32(-9);
  c3.b.push_back(13);
  c3.i32(-2);
  c3.u32(8);
  for (int i = 0; i < 8; ++i) c3.b.push_back(0);
  c3.bytes("GLT3");
  float lv = 0.125f;
  c3.b.insert(c3.b.end(), reinterpret_cast<uint8_t*>(&lv), reinterpret_cast<uint8_t*>(&lv) + 4);
  std::vector<uint8_t> w3 = wrapper(0, u32v(0x476C5433), {}, c3.b, "Gridline Test VST2 Level", "");
  expect(flp::parseWrapper(w3.data(), w3.size(), wi) && wi.data.compare(0, 7, "vst2c1:") == 0 && !wi.instrument, "wrapper: VST2 chunk parsed");
  m = flp::matchWrapper(wi);
  auto fx = createEffect(m.type);
  PluginState fs = defaultPluginState(m.type);
  fs.data = wi.data;
  fx->prepareHost(fs, 48000);
  u = host::UiUpdate{};
  host::hostedInstance(fx.get())->pumpUi(u);
  expect(m.type == kVst2Level && u.readback && std::fabs(u.values[0] - 0.125f) < 1e-6f, "wrapper: VST2 chunk restored into the plugin");

  // VST3: class id in COM order, component + controller state.
  auto vg = createGenerator(kVst3Synth);
  PluginState vs = defaultPluginState(kVst3Synth);
  vs.params[0] = 0.9f;
  vg->prepareHost(defaultPluginState(kVst3Synth), 48000);
  std::vector<float> L, R;
  runGen(*vg, vs, L, R, 256);
  std::string vdata, comp, ctrl;
  host::hostedInstance(vg.get())->captureState(vdata);
  host::decodeState(vdata, comp, ctrl);
  const uint8_t guid[16] = {0x44, 0x49, 0x52, 0x47, 0x49, 0x4C, 0x45, 0x4E, 0x54, 0x53, 0x59, 0x31, 0x00, 0x00, 0x00, 0x01};
  Rec v3;
  v3.u32(1);
  v3.rec(1, std::vector<uint8_t>(64, 0));
  v3.rec(3, comp);
  v3.rec(2, ctrl);
  v3.rec(4, [] {
    Rec t;
    t.u32(2);
    t.u32(0);  // FL index 0 → ParamID 0 (Gain)
    t.u32(3);  // FL index 1 → ParamID 3 (Mode)
    return t.b;
  }());
  std::vector<uint8_t> w4 = wrapper(8, {}, std::vector<uint8_t>(guid, guid + 16), v3.b, "Gridline Test Synth", "Gridline Tests");
  expect(flp::parseWrapper(w4.data(), w4.size(), wi) && wi.format == flp::WrapperInfo::Vst3 &&
             "vst3:" + wi.vst3Uid == kVst3Synth && wi.paramIds.size() == 2,
         "wrapper: VST3 class id converted from COM order");
  m = flp::matchWrapper(wi);
  expect(m.type == kVst3Synth && m.stateKept, "wrapper: VST3 matched by class id");
  std::string c2, k2;
  expect(host::decodeState(wi.data, c2, k2) && c2 == comp && k2 == ctrl, "wrapper: VST3 component + controller state carried over");

  // Through the importer's decoder (as in projects and presets).
  {
    flp::File file;
    Song song = makeEmptySong();
    flp::Report rep;
    flp::Context ctx(file, song, rep);
    std::vector<uint8_t> nameBytes;
    for (char ch : std::string("Fruity Wrapper")) nameBytes.push_back(static_cast<uint8_t>(ch)), nameBytes.push_back(0);
    nameBytes.push_back(0), nameBytes.push_back(0);
    std::vector<flp::Event> evs(2);
    evs[0].id = 201, evs[0].data = nameBytes.data(), evs[0].size = static_cast<uint32_t>(nameBytes.size());
    evs[1].id = 213, evs[1].data = w4.data(), evs[1].size = static_cast<uint32_t>(w4.size());
    const flp::PluginDecoder* dec = flp::findDecoder("Fruity Wrapper");
    flp::PluginMapping pm;
    expect(dec && (*dec)(ctx, evs, pm) && pm.glType == kVst3Synth && pm.exact && pm.state.data == wi.data, "wrapper decoder maps installed plugins");
    expect(pm.param && pm.param(1) == 2 && pm.value && pm.value(1, 1.0) == 2.0, "wrapper: FL parameter index → plugin parameter (automation)");
    expect(rep.counts["hosted plugins: installed"] == 1, "wrapper: installed plugins counted");

    // Missing plugin: placeholder with its saved type and state.
    std::vector<uint8_t> w5 = wrapper(4, u32v(0x12345678), {}, s2.b, "Somebody's Synth", "Nobody");
    evs[1].data = w5.data(), evs[1].size = static_cast<uint32_t>(w5.size());
    flp::PluginMapping pm2;
    expect((*dec)(ctx, evs, pm2) && pm2.glType == "vst2:12345678" && pm2.state.data.compare(0, 7, "vst2p1:") == 0 && !pm2.exact,
           "wrapper: missing plugin kept as a placeholder with its state");
    expect(rep.unmapped.count("Somebody's Synth (VST2) - not installed (kept with its state)") == 1, "wrapper: missing plugin reported");
    // Installed only in another format: matched by name (state not carried over).
    std::vector<uint8_t> w6 = wrapper(4, u32v(0x0BADF00D), {}, s2.b, "Gridline Test Synth (CLAP)", "Gridline Tests");
    evs[1].data = w6.data(), evs[1].size = static_cast<uint32_t>(w6.size());
    flp::PluginMapping pm3;
    expect((*dec)(ctx, evs, pm3) && pm3.glType == kClapSynth && pm3.state.data.empty(), "wrapper: other format matched by name");
    // A third-party plugin is never swapped for a Gridline built-in, even when
    // its name matches one of FL's own plugins: it stays the hosted plugin's type.
    std::vector<uint8_t> w7 = wrapper(4, u32v(0x0DEFACED), {}, s2.b, "Fruity Reeverb 2", "Other Audio Co");
    evs[1].data = w7.data(), evs[1].size = static_cast<uint32_t>(w7.size());
    flp::PluginMapping pm4;
    expect((*dec)(ctx, evs, pm4) && pm4.glType == "vst2:0DEFACED" && pm4.state.data.compare(0, 7, "vst2p1:") == 0,
           "wrapper: third-party plugin named like an FL plugin stays a hosted placeholder");
    expect(flp::isImageLineVendor("Image-Line") && flp::isImageLineVendor("image line software") && !flp::isImageLineVendor(""),
           "wrapper: Image-Line vendor recognised (an empty vendor is not Image-Line)");
    // Only Image-Line's own plugins may become built-ins: by vendor, or with
    // no vendor recorded by an Image-Line install path or FL's "Fruity " name.
    expect(flp::isImageLinePlugin("Image-Line", "Maximus", "") && flp::isImageLinePlugin("", "Fruity Fast LP", "") &&
               flp::isImageLinePlugin("", "Maximus", "C:\\Program Files\\Image-Line\\FL Studio\\Plugins\\Maximus.dll"),
           "wrapper: Image-Line plugins recognised");
    expect(!flp::isImageLinePlugin("", "Sytrus", "") && !flp::isImageLinePlugin("", "Maximus", "/Library/Audio/Plug-Ins/VST/Maximus.vst") &&
               !flp::isImageLinePlugin("Dada Life", "Fruity Limiter", "") && !flp::isImageLinePlugin("Xfer Records", "Serum 2", ""),
           "wrapper: unknown-vendor and third-party plugins are never treated as Image-Line's");
    // A wrapper with no vendor whose name matches an FL plugin stays the hosted plugin.
    std::vector<uint8_t> w8 = wrapper(4, u32v(0x5379746E), {}, s2.b, "Sytrus", "");
    evs[1].data = w8.data(), evs[1].size = static_cast<uint32_t>(w8.size());
    flp::PluginMapping pm5;
    expect((*dec)(ctx, evs, pm5) && pm5.glType == "vst2:5379746E", "wrapper: vendor-less plugin named like an FL plugin stays hosted");
    // Wrappers without a plugin identity (no format record: older files,
    // formats this importer does not know) go to the name-based decoder,
    // which still never turns a third-party plugin into a built-in.
    auto rawWrapper = [](const std::string& name, const std::string& vendor, const std::string& path) {
      Rec vals;
      vals.i32(-9), vals.i32(-507), vals.b.push_back(0xFF), vals.i32(0), vals.i32(0), vals.i32(0), vals.i32(2);
      const float v2[2] = {1.0f, 0.5f};
      vals.b.insert(vals.b.end(), reinterpret_cast<const uint8_t*>(v2), reinterpret_cast<const uint8_t*>(v2) + 8);
      Rec w;
      w.u32(8);
      if (!name.empty()) w.rec(54, name);
      if (!path.empty()) w.rec(55, path);
      if (!vendor.empty()) w.rec(56, vendor);
      w.rec(53, vals.b);
      return w.b;
    };
    auto decodeRaw = [&](const std::vector<uint8_t>& raw, const std::string& display) {
      std::vector<flp::Event> ev2 = evs;
      ev2[1].data = raw.data(), ev2[1].size = static_cast<uint32_t>(raw.size());
      std::vector<uint8_t> dn;
      for (char ch : display) dn.push_back(static_cast<uint8_t>(ch)), dn.push_back(0);
      dn.push_back(0), dn.push_back(0);
      if (!display.empty()) {
        flp::Event e;
        e.id = 203, e.data = dn.data(), e.size = static_cast<uint32_t>(dn.size());
        ev2.push_back(e);
      }
      flp::PluginMapping pm;
      const bool ok = (*dec)(ctx, ev2, pm);
      return ok ? pm.glType : std::string();
    };
    expect(decodeRaw(rawWrapper("Fruity Fast LP", "", ""), "") == "gl.fastlp", "wrapper (no identity): FL's own plugin still maps to its built-in");
    expect(decodeRaw(rawWrapper("Fruity Fast LP", "Other Audio Co", ""), "").compare(0, 3, "gl.") != 0,
           "wrapper (no identity): third-party vendor never becomes a built-in");
    expect(decodeRaw(rawWrapper("", "", ""), "Fruity Fast LP").compare(0, 3, "gl.") != 0,
           "wrapper (no identity): the user's channel name is never taken for the plugin");
    expect(decodeRaw(rawWrapper("Maximus", "", "/Library/Audio/Plug-Ins/VST/Maximus.vst"), "").compare(0, 3, "gl.") != 0,
           "wrapper (no identity): vendor-less plugin outside Image-Line's folders never becomes a built-in");
    expect(!flp::isImageLineVendor("Dada Life") && !flp::isImageLineVendor("Apple") && !flp::isImageLineVendor("Kilohearts"),
           "wrapper: other vendors are never converted to built-ins");
  }

#if defined(__APPLE__)
  // AU: manufacturer, type, subtype (u32 LE) + ClassInfo property list.
  auto ad = createEffect("au:aufx/dely/appl");
  if (ad) {
    ad->prepareHost(defaultPluginState("au:aufx/dely/appl"), 48000);
    std::string adata, plist;
    host::hostedInstance(ad.get())->captureState(adata);
    host::base64Decode(adata.substr(4), plist);
    Rec a3;
    a3.u32(0);
    a3.u32(static_cast<uint32_t>(plist.size()));
    a3.u32(0);
    a3.bytes(plist);
    Rec g3;
    g3.u32(fcc("appl"));
    g3.u32(fcc("aufx"));
    g3.u32(fcc("dely"));
    g3.u32(0);
    std::vector<uint8_t> w7 = wrapper(9, u32v(0), g3.b, a3.b, "AUDelay", "");
    expect(flp::parseWrapper(w7.data(), w7.size(), wi) && wi.auKey == "aufx/dely/appl" && wi.data == adata, "wrapper: AU description + ClassInfo");
    m = flp::matchWrapper(wi);
    expect(m.type == "au:aufx/dely/appl" && m.byId, "wrapper: AU matched by component description");
  }
#endif

  // FL Studio's own demo projects (when installed): 5 of FL's own effects
  // wrapped as Windows VST plugins → Gridline's built-in equivalents.
  const std::string demo =
      "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Demo projects/Visual/ZGameEditor Visualizer/"
      "JuiceBass - SeamlessR - Menagerie.flp";
  std::error_code ec;
  if (fs::exists(demo, ec)) {
    Song s;
    flp::Report rep;
    std::string err;
    const bool ok = importFlp(demo, s, rep, &err);
    const int missing = rep.counts["hosted plugins: missing (kept with their state)"], installed = rep.counts["hosted plugins: installed"];
    const int builtin = rep.counts["hosted plugins: FL plugins wrapped as VST → built-in"];
    std::printf("# FL demo: %d wrapped plugins installed, %d → built-in, %d missing\n", installed, builtin, missing);
    expect(ok && installed + missing + builtin == 5 && builtin == 5, "FL demo project: 5 wrapped FL effects mapped to built-ins (FL plugin decoders)");
    bool comp = false;
    for (const auto& m : s.mixer)
      for (const auto& sl : m.slots) comp = comp || sl.plugin.type == "gl.comp" || sl.plugin.type.rfind("gl.", 0) == 0;
    expect(comp, "FL demo project: wrapped effects became built-in slots");
  }
}


// ---------------------------------------------------------------- J-15: host features

template <class T>
T* probe(const std::string& binary, const char* symbol) {
  void* h = host::dl::loaded(binary);
  if (!h) h = host::dl::open(binary, nullptr, true);
  void* f = h ? host::dl::sym(h, symbol) : nullptr;
  return f ? host::toFunction<T* (*)()>(f)() : nullptr;
}
std::string clapBinary() {
#if defined(__APPLE__)
  return kClapBundle + "/Contents/MacOS/GridlineTest";
#else
  return kClapBundle;
#endif
}
std::string vst2Binary() {
#if defined(__APPLE__)
  return kVst2Bundle + "/Contents/MacOS/GridlineTestVst2";
#else
  return kVst2Bundle;
#endif
}

// A transport in 3/4 at 140 BPM, 7.5 quarter notes in, looping 3..12.
ProcessContext transport34() {
  ProcessContext c = ctx48();
  c.tempo = 140;
  c.ppq = 96;
  c.tick = 96 * 7.5;
  c.playing = true;
  c.tsNum = 3;
  c.tsDen = 4;
  c.looping = true;
  c.loopStart = 96 * 3;
  c.loopEnd = 96 * 12;
  return c;
}

void testTransport() {
  auto* cp = probe<GridlineClapProbe>(clapBinary(), "gridline_clap_probe");
  auto* v2 = probe<GridlineVst2Probe>(vst2Binary(), "gridline_vst2_probe");
  auto* v3 = probe<GridlineVst3Probe>(vst3::bundleBinaryPath(kVst3Bundle), "gridline_vst3_probe");
  expect(cp && v2 && v3, "test plugin probes reachable");
  if (!cp || !v2 || !v3) return;
  const ProcessContext c = transport34();
  // CLAP: transport event.
  {
    auto fx = createEffect(kClapGain);
    PluginState st = defaultPluginState(kClapGain);
    fx->prepareHost(st, 48000);
    std::vector<float> x(256, 0.f), y(256, 0.f);
    fx->processAux(st, ChannelAux{}, c, x.data(), y.data(), 256);
    const clap_event_transport_t& t = cp->transport;
    const double F = static_cast<double>(CLAP_BEATTIME_FACTOR);
    expect(cp->hadTransport && t.tempo == 140 && t.tsig_num == 3 && t.tsig_denom == 4 && (t.flags & CLAP_TRANSPORT_IS_PLAYING) &&
               (t.flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) && !(t.flags & CLAP_TRANSPORT_IS_RECORDING),
           "CLAP transport: tempo, 3/4, playing, loop");
    expect(t.song_pos_beats == static_cast<clap_beattime>(7.5 * F) && t.bar_start == static_cast<clap_beattime>(6 * F) && t.bar_number == 2 &&
               t.loop_start_beats == static_cast<clap_beattime>(3 * F) && t.loop_end_beats == static_cast<clap_beattime>(12 * F),
           "CLAP transport: position, bar start / number in 3/4, loop range");
  }
  // VST2: time info through the host callback; process level.
  {
    auto g = createGenerator(kVst2Synth);
    PluginState st = defaultPluginState(kVst2Synth);
    g->prepareHost(st, 48000);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    g->process(st, ChannelAux{}, c, L.data(), R.data(), 256);
    const auto& t = v2->time;
    namespace a = vst2abi;
    expect(v2->timeCalls > 0 && std::fabs(t.ppqPos - 7.5) < 1e-9 && std::fabs(t.barStartPpq - 6.0) < 1e-9 && t.tempo == 140 && t.sigNumerator == 3 &&
               t.sigDenominator == 4 && std::fabs(t.cycleStartPpq - 3) < 1e-9 && std::fabs(t.cycleEndPpq - 12) < 1e-9,
           "VST2 time info: position, bar, tempo, 3/4, cycle");
    expect((t.flags & a::timePlaying) && (t.flags & a::timeCycleActive) && (t.flags & a::timePpqValid) && (t.flags & a::timeBarsValid) &&
               (t.flags & a::timeCyclePosValid) && (t.flags & a::timeSigValid) && (t.flags & a::timeTempoValid),
           "VST2 time info flags");
    expect(v2->processLevel == a::levelRealtime, "VST2: realtime process level");
    ProcessContext off = c;
    off.offline = true;
    g->process(st, ChannelAux{}, off, L.data(), R.data(), 256);
    expect(v2->processLevel == a::levelOffline, "VST2: offline process level while exporting");
    g->noteOn(7, 60, 0.8f, 10);
    g->process(st, ChannelAux{}, c, L.data(), R.data(), 256);
    Generator::MidiOutEvent ev[16];
    const int n = g->takeMidiOut(ev, 16);
    expect(n == 1 && (ev[0].s & 0xF0) == 0x90 && ev[0].a == 72 && ev[0].offset == 10, "VST2 MIDI output (hostProcessEvents) reaches the engine");
  }
  // VST3: process context; offline process mode.
  {
    auto g = createGenerator(kVst3Synth);
    PluginState st = defaultPluginState(kVst3Synth);
    g->prepareHost(st, 48000);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    g->process(st, ChannelAux{}, c, L.data(), R.data(), 256);
    using PC = Steinberg::Vst::ProcessContext;
    const PC& t = v3->context;
    expect(v3->hadContext && std::fabs(t.projectTimeMusic - 7.5) < 1e-9 && std::fabs(t.barPositionMusic - 6.0) < 1e-9 && t.tempo == 140 &&
               t.timeSigNumerator == 3 && std::fabs(t.cycleStartMusic - 3) < 1e-9 && std::fabs(t.cycleEndMusic - 12) < 1e-9,
           "VST3 process context: position, bar, tempo, 3/4, cycle");
    expect((t.state & PC::kPlaying) && (t.state & PC::kCycleActive) && (t.state & PC::kCycleValid) && (t.state & PC::kTempoValid) &&
               (t.state & PC::kTimeSigValid) && (t.state & PC::kBarPositionValid) && !(t.state & PC::kRecording),
           "VST3 process context flags");
    expect(v3->processMode == Steinberg::Vst::kRealtime, "VST3: realtime process mode");
    ProcessContext off = c;
    off.offline = true;
    g->process(st, ChannelAux{}, off, L.data(), R.data(), 256);
    expect(v3->processMode == Steinberg::Vst::kOffline, "VST3: offline process mode while exporting");
  }
  // CLAP offline: the render extension during an export.
  {
    cp->renderMode = 0;
    Document d(makeEmptySong());
    ObjectId pat = d.song().patterns[0].id;
    ObjectId ch = d.addChannel("clap", kClapSynth);
    d.addNote(pat, ch, {0, 48, 69, 100});
    d.addClip(pat, 0, 0);
    RenderOptions o;
    o.blockSize = 512;
    AudioBuffer out = renderSong(d.song(), o);
    expect(out.peak() > 0.1f && cp->renderMode == CLAP_RENDER_OFFLINE, "CLAP: offline render mode told during an export");
  }
}

// J-109: Stop releases a hosted instrument's notes (note-offs for every note
// sent, sustain off, All Notes Off on every MIDI channel); the second Stop
// (panic) adds All Sound Off. The engine plays the plugin as the app does.
void testStopReleasesHosted() {
  auto* cp = probe<GridlineClapProbe>(clapBinary(), "gridline_clap_probe");
  expect(cp != nullptr, "J-109: CLAP probe reachable");
  if (!cp) return;
  Document d(makeEmptySong());
  ObjectId ch = d.addChannel("clap", kClapSynth);
  ProgramBuilder builder;
  builder.setSampleRate(48000);
  AudioEngine eng{48000};
  eng.setProgram(builder.build(d.song()));
  std::vector<float> L(256), R(256);
  auto run = [&](int blocks) {
    float pk = 0;
    for (int b = 0; b < blocks; ++b) {
      eng.process(L.data(), R.data(), 256);
      for (int i = 0; i < 256; ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
    }
    return pk;
  };
  run(4);
  eng.noteOn(ch, 69, 0.9f);  // a preview key
  eng.midiEvent(0x90, 72, 100);  // and a note from a MIDI keyboard (selected channel)
  eng.setMidiTarget(ch);
  eng.midiEvent(0x90, 74, 100);
  const float playing = run(8);
  const int offs0 = cp->noteOffs, ano0 = cp->allNotesOff, aso0 = cp->allSoundOff, sus0 = cp->sustainOff;
  expect(playing > 0.05f, "J-109: the hosted synth sounds from a live note");
  expect(!eng.stopPressed(), "J-109: first Stop is a release, not a panic");
  const float after = run(8);
  expect(cp->noteOffs - offs0 >= 2, "J-109: Stop sends the hosted plugin a note-off for every note Gridline sent it");
  expect(cp->allNotesOff - ano0 >= 16 && cp->sustainOff - sus0 >= 16, "J-109: Stop sends sustain off + All Notes Off (CC 123) on all 16 MIDI channels");
  expect(cp->allSoundOff == aso0, "J-109: the first Stop does not send All Sound Off");
  expect(after < 1e-6f || run(4) < 1e-6f, "J-109: the hosted synth is silent after Stop");
  expect(eng.stopPressed(), "J-109: Stop again while stopped is the panic");
  run(4);
  expect(cp->allSoundOff - aso0 >= 16, "J-109: the panic sends All Sound Off (CC 120) on all 16 MIDI channels");
  expect(eng.panicsDone() == 1 && !eng.panicking(), "J-109: the panic completed");
}

// Sidechain from the mixer: a sidechain send feeds a hosted effect's sidechain input.
void testSidechainRender() {
  auto render = [](float sideParam) {
    Document d(makeEmptySong());
    d.setParam(songParam(SongParam::Tempo), 120.0, false);
    ObjectId pat = d.song().patterns[0].id;
    ObjectId ch = d.addChannel("src", kClapSynth);
    d.addNote(pat, ch, {0, 96, 69, 127});
    d.addClip(pat, 0, 0);
    ObjectId t1 = d.song().mixer[1].id, t2 = d.song().mixer[2].id;
    d.routeChannel(ch, t1);
    d.editSong("sc", [&](Song& s) {
      MixerTrack* a = s.mixerTrack(t1);
      a->sends.clear();  // the source is heard only through the sidechain
      Send sc;
      sc.to = t2;
      sc.sidechain = true;
      a->sends.push_back(sc);
      MixerTrack* b = s.mixerTrack(t2);
      b->slots[0].plugin = defaultPluginState(kClapGain);
      b->slots[0].plugin.params[0] = 0.0f;  // level 0: only the sidechain is heard
      b->slots[0].plugin.params[1] = sideParam;
    });
    RenderOptions o;
    o.blockSize = 256;
    return renderSong(d.song(), o).peak();
  };
  const float off = render(0.0f), on = render(1.0f);
  expect(off < 1e-6f && on > 0.2f, "mixer sidechain send reaches a hosted effect's sidechain input");
}

// MIDI out of a plugin plays another channel (MIDI ports).
void testMidiOutRouting() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  ObjectId pat = d.song().patterns[0].id;
  ObjectId a = d.addChannel("arp", kClapSynth), b = d.addChannel("listener", kClapSynth);
  d.addNote(pat, a, {0, 96, 57, 127});  // 220 Hz; its echo is 440 Hz
  d.addClip(pat, 0, 0);
  ObjectId t1 = d.song().mixer[1].id, t2 = d.song().mixer[2].id;
  d.routeChannel(a, t1);
  d.routeChannel(b, t2);
  d.editSong("ports", [&](Song& s) {
    s.mixerTrack(t1)->muted = true;
    Channel* ca = s.channel(a);
    ca->plugin.params[3] = 1.0f;  // Echo on
    ca->midiOutPort = 3;
    s.channel(b)->midiInPort = 3;
  });
  RenderOptions o;
  o.blockSize = 256;
  AudioBuffer out = renderSong(d.song(), o);
  std::vector<float> left(out.plane(0), out.plane(0) + out.frames);
  const size_t s0 = 2000, s1 = 2000 + 11025;
  const int cr = crossings(left, s0, s1);  // 440 Hz over 0.25 s ≈ 220 crossings
  expect(out.peak() > 0.2f && cr > 200 && cr < 240, ("plugin MIDI output plays the listening channel (crossings " + std::to_string(cr) + ")").c_str());
  Json j = songToJson(d.song());
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err) && back.channel(a)->midiOutPort == 3 && back.channel(b)->midiInPort == 3, "MIDI ports saved in the project");
  // Without the port the listener stays silent.
  d.editChannel(b, "unport", [](Channel& c) { c.midiInPort = -1; });
  expect(renderSong(d.song(), o).peak() < 1e-6f, "no MIDI port: the listener stays silent");
  // "Detect": the instance reports a MIDI output and counts what it sent.
  auto g = createGenerator(kClapSynth);
  PluginState st = defaultPluginState(kClapSynth);
  st.params[3] = 1.0f;
  g->prepareHost(st, 48000);
  host::HostedInstance* hi = host::hostedInstance(g.get());
  std::vector<float> L, R;
  g->noteOn(1, 60, 1.0f, 5);
  runGen(*g, st, L, R, 256, 256);
  expect(hi->hasMidiOutput() && hi->midiOutCount() == 1 && hi->hostedClass().eventOutput, "MIDI output detected and counted");
}

// Parameter changes reach the plugin at their sample position.
void testSampleAccurate() {
  auto* cp = probe<GridlineClapProbe>(clapBinary(), "gridline_clap_probe");
  if (!cp) return;
  host::HostOptions fixedOpts;
  fixedOpts.fixedBuffers = true;
  fixedOpts.fixedSize = 256;
  host::setHostOptions(kClapGain, fixedOpts);
  auto fx = createEffect(kClapGain);
  PluginState st = defaultPluginState(kClapGain);  // level 1.0
  fx->prepareHost(st, 48000);
  expect(fx->latencySamples() == 16 + 256, "fixed size buffers: their length adds to the reported latency");
  std::vector<float> out;
  ProcessContext c = ctx48();
  for (int call = 0; call < 6; ++call) {  // engine blocks of 100 frames
    if (call == 2) st.params[0] = 0.25f;  // level 0.5 from input frame 200
    std::vector<float> x(100, 1.0f), y(100, 1.0f);
    fx->processAux(st, ChannelAux{}, c, x.data(), y.data(), 100);
    out.insert(out.end(), x.begin(), x.end());
  }
  expect(cp->lastParamTime == 200 && cp->lastFrames == 256, "fixed size buffers: 256-frame blocks, the change lands at offset 200");
  expect(out[271] == 0.0f && out[272] == 1.0f && out[471] == 1.0f && out[472] == 0.5f, "fixed size buffers: output changes on the exact sample");
  host::setHostOptions(kClapGain, host::HostOptions{});
  // Instruments: one process call per engine block, notes placed inside it.
  auto g = createGenerator(kClapSynth);
  PluginState gs = defaultPluginState(kClapSynth);
  g->prepareHost(gs, 48000);
  std::vector<float> L, R;
  g->noteOn(1, 69, 1.0f, 100);
  runGen(*g, gs, L, R, 512, 512);
  expect(cp->lastFrames == 512 && firstNonZero(L) == 100, "process maximum size buffers: one call, note at offset 100");
  host::HostOptions split;
  split.maxBuffers = false;
  host::setHostOptions(kClapSynth, split);
  expect(g->stale(), "changing the buffer option asks for a new instance");
  auto g2 = createGenerator(kClapSynth);
  g2->prepareHost(gs, 48000);
  L.clear();
  R.clear();
  g2->noteOn(1, 69, 1.0f, 100);
  runGen(*g2, gs, L, R, 512, 512);
  expect(cp->lastFrames == 412 && firstNonZero(L) == 100, "split buffers: the block is cut at the note");
  host::setHostOptions(kClapSynth, host::HostOptions{});
}

// A plugin reports a new latency (restart) → the instance and the PDC see it.
void testLatencyChange() {
  Document d(makeEmptySong());
  ObjectId t1 = d.song().mixer[1].id;
  d.editSong("fx", [&](Song& s) { s.mixerTrack(t1)->slots[0].plugin = defaultPluginState(kClapGain); });
  ProgramBuilder b;
  b.setSampleRate(48000);
  auto prog = b.build(d.song());
  auto rt = b.effectRuntime(t1, 0);
  expect(rt && rt->fx && rt->fx->latencySamples() == 16 && !b.latencyChanged(), "hosted effect: latency 16 at build");
  if (!rt || !rt->fx) return;
  PluginState st = d.song().mixerTrack(t1)->slots[0].plugin;
  st.params[2] = 1.0f;  // "Long": the plugin asks for a restart
  std::vector<float> x(256, 0.f), y(256, 0.f);
  ProcessContext c = ctx48();
  rt->fx->processAux(st, ChannelAux{}, c, x.data(), y.data(), 256);
  host::UiUpdate u;
  host::hostedInstance(rt->fx.get())->pumpUi(u);
  expect(rt->fx->latencySamples() == 64 && b.latencyChanged(), "latency change → restart → new latency → program rebuild for PDC");
}

// CLAP: links / automation as non-destructive modulation.
void testClapModulation() {
  auto* cp = probe<GridlineClapProbe>(clapBinary(), "gridline_clap_probe");
  if (!cp) return;
  host::HostOptions mo;
  mo.modulation = true;
  host::setHostOptions(kClapSynth, mo);
  auto g = createGenerator(kClapSynth);
  PluginState base = defaultPluginState(kClapSynth);  // gain 0.5
  g->prepareHost(base, 48000);
  PluginState live = base;
  live.params[0] = 0.8f;  // what a link / automation made of it
  std::vector<float> L, R;
  g->noteOn(1, 69, 1.0f, 0);
  runGen(*g, live, L, R, 2048, 512);
  host::HostedInstance* hi = host::hostedInstance(g.get());
  expect(std::fabs(cp->lastMod - 0.3) < 1e-6 && std::fabs(peak(L, 512) - 0.8f) < 0.01f, "CLAP modulation: amount 0.3 on top of 0.5");
  expect(std::fabs(hi->backend()->paramNormalized(1) - 0.5) < 1e-9, "CLAP modulation: the plugin keeps the song's value");
  live.params[0] = 0.5f;
  runGen(*g, live, L, R, 512, 512);
  expect(std::fabs(cp->lastMod) < 1e-9, "CLAP modulation: back to zero");
  host::setHostOptions(kClapSynth, host::HostOptions{});
}

// CLAP preset discovery + preset load; plugin bypass.
void testPresetsAndBypass() {
  auto g = createGenerator(kClapSynth);
  PluginState st = defaultPluginState(kClapSynth);
  g->prepareHost(st, 48000);
  host::HostedInstance* hi = host::hostedInstance(g.get());
  const auto& ps = hi->libraryPresets();
  expect(ps.size() == 2 && ps[0].name == "Loud Sine" && ps[1].name == "Soft Sine" && ps[0].category == "Lead" && ps[0].creator == "Gridline Tests",
         "CLAP preset discovery lists the plugin's presets");
  host::UiUpdate u;
  hi->pumpUi(u);
  expect(hi->loadLibraryPreset(0), "CLAP preset loaded");
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(u.readback && std::fabs(u.values[0] - 0.9f) < 1e-6f && u.captureState, "CLAP preset: parameters read back, state captured");

  // VST2 soft bypass.
  auto fx = createEffect(kVst2Level);
  PluginState fs = defaultPluginState(kVst2Level);
  fs.params[0] = 0.25f;  // x0.5
  fx->prepareHost(fs, 48000);
  host::Backend* b = host::hostedInstance(fx.get())->backend();
  std::vector<float> x(64, 0.5f), y(64, 0.5f);
  ProcessContext c = ctx48();
  fx->processAux(fs, ChannelAux{}, c, x.data(), y.data(), 64);
  const float wet = x[10];
  expect(b->hasBypass() && b->setBypass(true), "VST2 soft bypass available");
  x.assign(64, 0.5f);
  fx->processAux(fs, ChannelAux{}, c, x.data(), y.data(), 64);
  expect(std::fabs(wet - 0.25f) < 1e-6f && std::fabs(x[10] - 0.5f) < 1e-6f, "VST2 bypassed: input passes unchanged");
  // CLAP bypass parameter.
  auto cfx = createEffect(kClapGain);
  PluginState cs = defaultPluginState(kClapGain);
  cs.params[0] = 0.25f;  // level 0.5
  cfx->prepareHost(cs, 48000);
  host::Backend* cb = host::hostedInstance(cfx.get())->backend();
  expect(cb->hasBypass() && cb->setBypass(true), "CLAP bypass parameter available");
  x.assign(64, 1.0f);
  cfx->processAux(cs, ChannelAux{}, c, x.data(), y.data(), 64);
  expect(std::fabs(x[40] - 1.0f) < 1e-6f, "CLAP bypassed: input passes unchanged (after its latency)");
}

// Sandbox: MIDI output and modulation travel through shared memory.
void testSandboxFeatures() {
  OfflineScope offline;
  host::setChildExecutable(GRIDLINE_CLI_EXE);
  host::setRunInSandbox(kClapSynth, true);
  auto g = createGenerator(kClapSynth);
  if (!g) return;
  PluginState st = defaultPluginState(kClapSynth);
  st.params[3] = 1.0f;
  g->prepareHost(st, 48000);
  std::vector<float> L, R;
  g->noteOn(1, 60, 1.0f, 33);
  runGen(*g, st, L, R, 256, 256);
  Generator::MidiOutEvent ev[8];
  const int n = g->takeMidiOut(ev, 8);
  expect(n == 1 && ev[0].a == 72 && ev[0].offset == 33, "sandbox: plugin MIDI output comes back with its offset");
  host::HostedInstance* hi = host::hostedInstance(g.get());
  expect(hi->libraryPresets().size() == 2, "sandbox: preset discovery");
  host::setRunInSandbox(kClapSynth, false);
}


// ---- J-29: plugin windows (the plugin's own interface on top, the control
// bar under it, the settings panel under the bar) ----
void testPluginWindowLayout() {
  namespace L = host::layout;
  // Sizes installed plugins asked for on the development machine (tiny Apple
  // units up to large synths), plus extremes.
  const int sizes[][2] = {{48, 30},   {120, 90},  {235, 375}, {236, 375}, {484, 255},  {500, 200},  {522, 72},    {604, 308},
                          {600, 560}, {807, 638}, {845, 475}, {880, 475}, {1190, 740}, {1296, 754}, {2560, 1440}, {4000, 3000}};
  bool placed = true, noOverlap = true, sized = true, centred = true, folds = true, barFits = true;
  for (const auto& sz : sizes) {
    const int pw = sz[0], ph = sz[1];
    const L::WindowLayout w = L::layoutWindow(pw, ph, 0);
    const int ew = std::max(pw, L::kMinPluginW), eh = std::max(ph, L::kMinPluginH);
    // The interface keeps exactly its size at the top of the window.
    placed = placed && w.plugin.y == 0 && w.plugin.w == ew && w.plugin.h == eh;
    // The bar sits right under it at the window's full width; nothing covers the interface.
    placed = placed && w.bar.y == w.plugin.bottom() && w.bar.x == 0 && w.bar.w == w.w && w.panel.h == 0;
    noOverlap = noOverlap && !w.plugin.overlaps(w.bar) && !w.plugin.overlaps(w.panel) && !w.bar.overlaps(w.panel);
    // Window = interface + bar (narrow interfaces get the minimum width, centred).
    sized = sized && w.w == std::max(ew, L::kMinWindowW) && w.h == eh + w.bar.h;
    centred = centred && w.plugin.x == (w.w - ew) / 2 && w.plugin.x >= 0 && w.plugin.right() <= w.w;
    // Unfolding the settings panel grows the window under the bar; folding restores it exactly.
    const L::WindowLayout open = L::layoutWindow(pw, ph, L::kPanelH);
    folds = folds && open.plugin.y == 0 && open.plugin.h == eh && open.plugin.w == ew && open.panel.y == open.bar.bottom() &&
            open.panel.h == L::kPanelH && open.h == eh + open.bar.h + L::kPanelH && open.w >= L::kPanelMinW && open.w >= ew &&
            !open.plugin.overlaps(open.bar) && !open.bar.overlaps(open.panel);
    const L::WindowLayout again = L::layoutWindow(pw, ph, 0);
    folds = folds && again.w == w.w && again.h == w.h && again.plugin == w.plugin && again.bar == w.bar;
    barFits = barFits && w.bar.h == L::layoutBar(w.w).height;
  }
  expect(placed, "plugin window: interface on top at its own size, bar right under it");
  expect(noOverlap, "plugin window: the bar and the panel never cover the plugin's interface");
  expect(sized, "plugin window: window = plugin size + bar height (minimum width for tiny plugins)");
  expect(centred, "plugin window: plugins narrower than the minimum width sit centred");
  expect(folds, "plugin window: settings panel unfolds under the bar and folding restores the plugin-only size");
  expect(barFits, "plugin window: bar height follows its layout");
  {  // Example sizes from the test set.
    const L::WindowLayout a = L::layoutWindow(484, 255, 0), k = L::layoutWindow(236, 375, 0), s2 = L::layoutWindow(1190, 740, 0);
    expect(a.w == 484 && a.h == 255 + L::kBarRowH && k.w == 236 && k.h == 375 + 2 * L::kBarRowH && s2.w == 1190 && s2.h == 740 + L::kBarRowH,
           "plugin window: AUDelay 484x255 -> 484x283, kHs Delay 236x375 -> 236x431 (two-row bar), Serum 2 1190x740 -> 1190x768");
  }

  // Control bar: every item inside the bar, no two items overlap, the
  // essential ones always shown; one row when wide, two when narrow.
  bool inside = true, apart = true, essential = true, rows = true, routeShown = true;
  for (int width = 40; width <= 2400; width += 7)
  for (bool route : {false, true}) {  // J-195: an instrument's bar has the route button too
    const L::BarLayout b = L::layoutBar(width, route);
    const int bw = std::max(width, L::kMinWindowW);
    routeShown = routeShown && b.visible[L::kBarRoute] == route && b.rows == L::layoutBar(width).rows;
    const L::Rect bar{0, 0, bw, b.height};
    for (int i = 0; i < L::kBarItemCount; ++i) {
      if (!b.visible[i]) continue;
      inside = inside && bar.contains(b.items[i]) && b.items[i].w > 0 && b.items[i].h > 0;
      for (int j = i + 1; j < L::kBarItemCount; ++j)
        if (b.visible[j]) apart = apart && !b.items[i].overlaps(b.items[j]);
    }
    for (L::BarItem it : {L::kBarPrev, L::kBarPreset, L::kBarNext, L::kBarBypass, L::kBarSandbox, L::kBarGeneric, L::kBarSettings})
      essential = essential && b.visible[it];
    essential = essential && b.items[L::kBarPreset].w >= 24;
    rows = rows && (bw >= L::oneRowBarWidth() ? b.rows == 1 && b.height == L::kBarRowH : b.rows == 2 && b.height == 2 * L::kBarRowH);
    rows = rows && b.items[L::kBarPreset].w <= L::kPresetMaxW;
  }
  expect(inside, "control bar: every item inside the bar at every width");
  expect(apart, "control bar: items never overlap (min-width layout and wrapping)");
  expect(essential, "control bar: presets, bypass, sandbox, generic and settings stay usable at small widths");
  expect(rows, "control bar: one row when wide, two rows when narrow");
  expect(routeShown, "control bar: an instrument's bar always shows the route button, an effect's never; it never adds a row");
  expect(L::layoutBar(L::oneRowBarWidth(), true).visible[L::kBarCpu] && L::layoutBar(L::oneRowBarWidth(), true).rows == 1 &&
             L::layoutBar(1190, true).items[L::kBarPreset].w == L::layoutBar(1190).items[L::kBarPreset].w,
         "control bar: the route button takes the preset box's room only when the row is tight");
  expect(!L::layoutBar(L::kMinWindowW).visible[L::kBarCpu] && L::layoutBar(L::oneRowBarWidth()).visible[L::kBarCpu],
         "control bar: the readout is dropped first when narrow");

  // The user drags a resizable plugin's window: the plugin size asked for
  // gives back that window size.
  bool inverse = true;
  for (int ww = L::kMinWindowW; ww <= 2000; ww += 37)
    for (int wh = 160; wh <= 1400; wh += 53)
      for (int panel : {0, L::kPanelH}) {
        if (panel > 0 && ww < L::kPanelMinW) continue;
        if (wh - L::layoutBar(ww).height - panel < L::kMinPluginH) continue;  // below the minimum plugin height
        int pw = 0, ph = 0;
        L::pluginSizeForWindow(ww, wh, panel, pw, ph);
        const L::WindowLayout w = L::layoutWindow(pw, ph, panel);
        inverse = inverse && w.w == ww && w.h == wh;
      }
  expect(inverse, "plugin window: user resize -> plugin size -> same window size");

  // Bar-only window docked under a sandboxed plugin's own window.
  const L::WindowLayout sb = L::layoutBarWindow(484, 0), sbOpen = L::layoutBarWindow(484, L::kPanelH);
  expect(sb.w == 484 && sb.h == sb.bar.h && sb.plugin.empty() && sbOpen.h == sbOpen.bar.h + L::kPanelH && sbOpen.panel.y == sbOpen.bar.bottom(),
         "sandbox bar window: bar at the plugin window's width, panel under it");

  // Placement: on screen, never resized; remembered positions win; new windows cascade.
  const L::Rect work{0, 25, 1440, 875};
  const L::Rect off = L::fitOnScreen(L::Rect{1300, 800, 400, 300}, work);
  const L::Rect huge = L::fitOnScreen(L::Rect{50, 60, 2000, 1200}, work);
  expect(work.contains(off) && off.w == 400 && off.h == 300 && huge.x == 0 && huge.y == 25 && huge.w == 2000,
         "plugin window: kept on screen without resizing (top-left wins when larger)");
  const L::Rect mem = L::placeWindow(500, 300, work, true, 200, 150, 3);
  const L::Rect c0 = L::placeWindow(500, 300, work, false, 0, 0, 0), c1 = L::placeWindow(500, 300, work, false, 0, 0, 1);
  expect(mem.x == 200 && mem.y == 150 && work.contains(c0) && work.contains(c1) && c1.x > c0.x && c1.y > c0.y,
         "plugin window: remembered position per instance, new windows cascade");

  // The plugin's own interface is always the default; the generic list is a
  // fallback (no interface) or an explicit choice.
  expect(L::chooseEditorMode(true, false) == L::EditorMode::Native && L::chooseEditorMode(true, true) == L::EditorMode::Generic &&
             L::chooseEditorMode(false, false) == L::EditorMode::Generic && L::chooseEditorMode(false, true) == L::EditorMode::Generic,
         "editor mode: native first, generic only as fallback or choice");
}

// Real instances: plugins with an interface open it by default, plugins
// without one fall back to the generic list; nothing is ever swapped for a
// Gridline built-in.
void testEditorDefaults() {
  namespace L = host::layout;
  auto gain = createEffect(kClapGain);
  if (gain) {
    host::HostedInstance* hi = host::hostedInstance(gain.get());
    expect(hi && !hi->backend()->hasEditor() &&
               L::chooseEditorMode(hi->backend()->hasEditor(), hi->options().genericEditor) == L::EditorMode::Generic,
           "editor defaults: plugin without an interface -> generic parameter list");
  }
#if defined(__APPLE__)
  for (const char* t : {"au:aufx/dely/appl", "au:aumu/dls /appl"}) {
    const std::string type = t;
    std::unique_ptr<Effect> fx;
    std::unique_ptr<Generator> gen;
    host::HostedInstance* hi = nullptr;
    if (type.compare(0, 8, "au:aumu/") == 0) {
      gen = createGenerator(type);
      hi = gen ? host::hostedInstance(gen.get()) : nullptr;
    } else {
      fx = createEffect(type);
      hi = fx ? host::hostedInstance(fx.get()) : nullptr;
    }
    if (!hi) continue;  // not on this system
    expect(hi->backend()->hasEditor() && L::chooseEditorMode(hi->backend()->hasEditor(), hi->options().genericEditor) == L::EditorMode::Native,
           (type + ": opens its own interface by default").c_str());
  }
#endif
  // Hosted types are only ever created as the hosted plugin: an unknown or
  // missing one yields no instance, never a built-in stand-in.
  expect(!createEffect("vst3:00000000000000000000000000000000") && !createGenerator("vst2:7FFFFFFF") && !createEffect("au:aufx/none/none"),
         "missing installed plugins are never replaced by built-ins at run time");
  PluginState missing;
  missing.type = "vst3:0123456789ABCDEF0123456789ABCDEF";
  missing.data = "vst3s1:AAAA:";
  normalizePluginState(missing);
  expect(missing.type == "vst3:0123456789ABCDEF0123456789ABCDEF" && missing.data == "vst3s1:AAAA:",
         "missing installed plugins keep their type and state");
}

// J-21 cross-feature: an offline render (export) of a song whose instrument
// runs in the sandbox must equal the in-process render, at any block size and
// with parallel mixer threads (no dropped / late blocks while rendering).
void testSandboxOfflineRender() {
  host::setChildExecutable(GRIDLINE_CLI_EXE);
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 132.0, false);
  ObjectId pat = d.song().patterns[0].id;
  ObjectId ch = d.addChannel("sandboxed", kClapSynth);
  for (int k = 0; k < 8; ++k) d.addNote(pat, ch, {k * 48, 40, 60 + k, 110});
  d.addClip(pat, 0, 0);
  RenderOptions o;
  o.blockSize = 256;
  host::setRunInSandbox(kClapSynth, false);
  AudioBuffer in = renderSong(d.song(), o);
  host::setRunInSandbox(kClapSynth, true);
  AudioBuffer sb = renderSong(d.song(), o);
  RenderOptions o2 = o;
  o2.blockSize = 97;
  o2.threads = 4;
  AudioBuffer sb2 = renderSong(d.song(), o2);
  host::setRunInSandbox(kClapSynth, false);
  expect(in.peak() > 0.1f, "sandbox render fixture makes sound");
  expect(in.frames == sb.frames && in.data == sb.data, "offline render: sandboxed instrument == in-process (bit-exact)");
  expect(sb2.frames == sb.frames && sb2.data == sb.data, "offline render: sandboxed, 97-frame blocks + 4 threads identical");
}

// J-103: quitting while a background plugin scan runs. The scan used to keep going after
// main() returned and lock the (destroyed) class registry mutex: "mutex lock failed",
// abort on quit. shutdownScan() kills the scanner child, joins the worker and refuses new
// scans; a cancelled entry is not cached. Runs last: the scan service stays closed.
void testScanShutdown() {
#if defined(_WIN32)
  std::printf("skip - scan shutdown: POSIX shell scanner stand-in\n");
#else
  const std::string dir = gl::test::hermeticTestEnv("hosting") + "/scan-shutdown";
  fs::create_directories(dir + "/plugins");
  fs::copy(kClapBundle, dir + "/plugins/" + fs::path(kClapBundle).filename().string(), fs::copy_options::recursive);
  // A scanner that hangs: only shutdownScan() (killing it) ends the entry in time.
  const std::string slow = dir + "/slow-scanner.sh";
  if (FILE* f = std::fopen(slow.c_str(), "w")) {
    std::fputs("#!/bin/sh\nexec sleep 30\n", f);
    std::fclose(f);
  }
  fs::permissions(slow, fs::perms::owner_all, fs::perm_options::add);

  // scanIsolated: a cancel flag ends a hanging child at once.
  std::atomic<bool> cancel{false};
  std::thread flip([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancel = true;
  });
  auto t0 = std::chrono::steady_clock::now();
  host::ScanResult r = host::scanIsolated(slow, "clap", kClapBundle, 30, &cancel);
  flip.join();
  double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  expect(!r.ok && r.error == "cancelled" && took < 5, "scan shutdown: a cancelled isolated scan kills its child at once");

  // The service: only CLAP, the copied test plugin in a user folder; forced so it is scanned.
  host::PluginPrefs p = host::pluginPrefs();
  p.extraFolders = {dir + "/plugins"};
  p.disabledFormats = {"vst3", "vst2", "au", "lv2"};
  host::setPluginPrefs(p);
  host::startScan(slow, true);
  bool scanning = false;
  for (int i = 0; i < 1000 && !scanning; ++i) {  // until a scanner child runs (discovery done)
    const host::ScanStatus st = host::scanStatus();
    scanning = st.running && !st.current.empty();
    if (!scanning) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  expect(scanning, "scan shutdown: a background scan is running");
  t0 = std::chrono::steady_clock::now();
  host::shutdownScan();
  took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("scan shutdown: shutdownScan() took %.3f s\n", took);
  expect(took < 5, "scan shutdown: shutdownScan() returns at once with a hanging scanner");
  expect(!host::scanStatus().running, "scan shutdown: no scan running afterwards");
  bool cachedCancel = false;
  for (const auto& e : host::cacheEntries()) cachedCancel = cachedCancel || e.error == "cancelled";
  expect(!cachedCancel, "scan shutdown: the cancelled entry is not cached (scanned again at the next start)");
  host::startScan(slow, true);
  expect(!host::scanStatus().running, "scan shutdown: no new scan starts after shutdown");
  host::shutdownScan();  // idempotent
  expect(!host::scanStatus().running, "scan shutdown: shutdownScan() twice is harmless");
#endif
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("hosting");
  testHelpers();
  testClap();
  testVst2();
  testVst3Extras();
  testMultiOutRender();
  testAu();
  testSandbox();
  testWrapper();
  testTransport();
  testSidechainRender();
  testStopReleasesHosted();
  testMidiOutRouting();
  testSampleAccurate();
  testLatencyChange();
  testClapModulation();
  testPresetsAndBypass();
  testSandboxFeatures();
  testPluginWindowLayout();
  testEditorDefaults();
  testSandboxOfflineRender();
  testScanShutdown();  // last: the scan service is closed afterwards
  return finish();
}

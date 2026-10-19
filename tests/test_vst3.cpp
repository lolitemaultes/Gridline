// Gridline — VST3 hosting tests. Uses only the in-tree test plugin
// (GridlineTest.vst3, built from tests/vst3/TestPlugin.cpp).
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Project.h"
#include "engine/Render.h"
#include "host/HostedPlugins.h"
#include "host/vst3/Vst3HostObjects.h"
#include "host/vst3/Vst3Module.h"
#include "host/vst3/Vst3Plugin.h"
#include "host/vst3/Vst3Scan.h"

namespace fs = std::filesystem;
using namespace gl;
namespace sv = Steinberg::Vst;

namespace {

const std::string kBundle = GRIDLINE_TESTPLUG_BUNDLE;
// Class ids of the test plugin (see TestPlugin.cpp).
const std::string kSynth = "vst3:47524944" "4C494E45" "54535931" "00000001";
const std::string kGain = "vst3:47524944" "4C494E45" "54535931" "00000003";

int firstNonZero(const std::vector<float>& v) {
  for (size_t i = 0; i < v.size(); ++i)
    if (std::fabs(v[i]) > 1e-7f) return static_cast<int>(i);
  return -1;
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

ProcessContext ctx48() {
  ProcessContext c;
  c.sampleRate = 48000;
  c.tempo = 120;
  return c;
}

// Renders n frames of an instrument in blocks of `block`.
void runGen(Generator& g, const PluginState& st, std::vector<float>& L, std::vector<float>& R, int n, int block = 256) {
  ChannelAux aux;
  ProcessContext c = ctx48();
  size_t base = L.size();
  L.resize(base + static_cast<size_t>(n), 0.0f);
  R.resize(base + static_cast<size_t>(n), 0.0f);
  for (int pos = 0; pos < n; pos += block) {
    int len = std::min(block, n - pos);
    g.process(st, aux, c, L.data() + base + pos, R.data() + base + pos, len);
  }
}

void testHostObjects() {
  // Event list: kept in sample-offset order, stable for equal offsets.
  vst3::EventList ev;
  int offsets[] = {40, 5, 40, 0, 17};
  for (int i = 0; i < 5; ++i) {
    sv::Event e{};
    e.sampleOffset = offsets[i];
    e.type = sv::Event::kNoteOnEvent;
    e.noteOn.noteId = i;
    ev.addEvent(e);
  }
  bool ordered = ev.getEventCount() == 5;
  int prev = -1, ids[5] = {};
  for (int i = 0; i < ev.getEventCount(); ++i) {
    sv::Event e{};
    ev.getEvent(i, e);
    ordered = ordered && e.sampleOffset >= prev;
    prev = e.sampleOffset;
    ids[i] = e.noteOn.noteId;
  }
  expect(ordered, "event list is ordered by sample offset");
  expect(ids[0] == 3 && ids[1] == 1 && ids[2] == 4 && ids[3] == 0 && ids[4] == 2, "event list keeps insertion order at equal offsets");
  sv::Event bad{};
  expect(ev.getEvent(9, bad) != Steinberg::kResultOk, "event list rejects out-of-range reads");

  // Parameter queues: ordered points, same offset replaces, fixed capacity.
  vst3::ParameterChanges pc(2);
  Steinberg::int32 idx = 0;
  auto* q = pc.addParameterData(7, idx);
  expect(q && idx == 0, "parameter queue added");
  q->addPoint(100, 0.5, idx);
  q->addPoint(10, 0.25, idx);
  q->addPoint(100, 0.75, idx);
  Steinberg::int32 off = 0;
  sv::ParamValue v = 0;
  q->getPoint(0, off, v);
  bool first = off == 10 && v == 0.25;
  q->getPoint(1, off, v);
  expect(first && off == 100 && v == 0.75 && q->getPointCount() == 2, "queue points ordered, same offset replaced");
  expect(pc.addParameterData(7, idx) == q && pc.getParameterCount() == 1, "same parameter reuses its queue");
  pc.addParameterData(8, idx);
  expect(pc.addParameterData(9, idx) == nullptr, "parameter changes respect capacity (no allocation)");
  pc.clear();
  expect(pc.getParameterCount() == 0, "parameter changes clear");

  // Streams.
  vst3::MemoryStream ms;
  int32_t a = 1234, b = 0;
  ms.write(&a, 4, nullptr);
  ms.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
  Steinberg::int32 got = 0;
  ms.read(&b, 4, &got);
  expect(b == 1234 && got == 4, "memory stream round trip");
  ms.read(&b, 4, &got);
  expect(got == 0, "memory stream read past end returns 0 bytes");

  // Base64 and state chunks.
  std::string bin;
  for (int i = 0; i < 300; ++i) bin += static_cast<char>(i * 37);
  std::string dec;
  expect(vst3::base64Decode(vst3::base64Encode(bin), dec) && dec == bin, "base64 round trip (binary)");
  expect(vst3::base64Encode("ab") == "YWI=" && vst3::base64Encode("abc") == "YWJj", "base64 known vectors");
  std::string c1, c2;
  expect(host::decodeState(host::encodeState(bin, "ctrl"), c1, c2) && c1 == bin && c2 == "ctrl", "state chunk encode/decode");
  expect(!host::decodeState("garbage", c1, c2), "foreign state data rejected");

  Steinberg::TUID uid;
  expect(vst3::hexToUid("00112233445566778899AABBCCDDEEFF", uid) && vst3::uidToHex(uid) == "00112233445566778899AABBCCDDEEFF",
         "class id hex round trip");

  // UTF-16 conversion.
  sv::String128 s16;
  vst3::toUtf16("Grünes \xF0\x9F\x8E\xB9", s16, 128);
  expect(vst3::toUtf8(s16) == "Grünes \xF0\x9F\x8E\xB9", "UTF-8/UTF-16 round trip incl. surrogates");
}

void testModuleAndScan() {
  std::string err;
  auto m = vst3::Module::load(kBundle, &err);
  expect(m != nullptr, ("test bundle loads " + err).c_str());
  if (!m) return;
  expect(m->classes().size() == 3, "factory lists 3 classes");
  int audio = 0, inst = 0;
  for (const auto& c : m->classes()) {
    audio += c.isAudioModule();
    inst += c.isAudioModule() && c.isInstrument();
  }
  expect(audio == 2 && inst == 1, "audio module classes: 1 instrument, 1 effect");
  expect(m->factoryVendor() == "Gridline Tests", "factory vendor");

  vst3::ScanResult r = vst3::scanBundleInProcess(kBundle);
  expect(r.ok && r.classes.size() == 2, "in-process scan finds 2 classes");
  const vst3::ScannedClass* synth = nullptr;
  const vst3::ScannedClass* gain = nullptr;
  for (const auto& c : r.classes) (c.instrument ? synth : gain) = &c;
  expect(synth && synth->eventInput && synth->outputChannels == 2 && synth->params.size() == 3,
         "synth: events, stereo out, 3 usable params (read-only/hidden skipped)");
  expect(gain && gain->sidechainChannels == 2 && gain->inputChannels == 2 && gain->latency == 32, "effect: sidechain bus, latency 32");
  if (synth) {
    const auto& mode = synth->params[2];
    expect(mode.title == "Mode" && mode.labels.size() == 3 && mode.labels[2] == "Fold", "discrete parameter labels scanned");
  }

  // JSON round trip of the scan result.
  vst3::ScanResult back;
  expect(vst3::fromJson(vst3::toJson(r), back) && back.classes.size() == 2 && back.classes[0].params.size() == r.classes[0].params.size(),
         "scan result JSON round trip");

  // Isolated scan through the CLI executable.
  vst3::ScanResult iso = vst3::scanBundleIsolated(GRIDLINE_CLI_EXE, kBundle, 30);
  expect(iso.ok && iso.classes.size() == 2, ("isolated scan (child process) " + iso.error).c_str());

  // A broken bundle fails in the child and is reported, not fatal.
  fs::path tmp = fs::temp_directory_path() / "gridline-vst3-test";
  fs::remove_all(tmp);
  fs::path broken = tmp / "Broken.vst3";
  fs::create_directories(fs::path(vst3::bundleBinaryPath(broken.string())).parent_path());
  { std::ofstream(vst3::bundleBinaryPath(broken.string())) << "not a binary"; }
  vst3::ScanResult bad = vst3::scanBundleIsolated(GRIDLINE_CLI_EXE, broken.string(), 30);
  expect(!bad.ok && !bad.error.empty(), ("broken bundle blacklisted: " + bad.error).c_str());

  // Cache: save/load, stale detection.
  vst3::ScanCache cache((tmp / "vst3-cache.json").string());
  cache.put(r);
  cache.put(bad);
  expect(cache.save(), "cache saved");
  vst3::ScanCache loaded((tmp / "vst3-cache.json").string());
  expect(loaded.load() && loaded.entries().size() == 2, "cache loaded");
  expect(loaded.stale({kBundle, broken.string()}).empty(), "unchanged bundles are not rescanned");
  expect(loaded.stale({(tmp / "New.vst3").string()}).size() == 1, "new bundles are scanned");
  const vst3::ScanResult* bl = loaded.find(broken.string());
  expect(bl && !bl->ok, "failed bundle stays blacklisted in the cache");
  fs::remove_all(tmp);

  // Bundle discovery.
  auto found = vst3::findBundles({fs::path(kBundle).parent_path().string()});
  expect(found.size() == 1 && found[0] == kBundle, "bundle discovery finds the test bundle");

  host::registerScanResult(r);
}

void testCatalogue() {
  const PluginInfo* s = findPlugin(kSynth);
  const PluginInfo* g = findPlugin(kGain);
  expect(s && s->kind == PluginKind::Generator && s->name == "Gridline Test Synth", "synth registered as instrument type");
  expect(g && g->kind == PluginKind::Effect && g->category == "Gridline Tests", "effect registered, grouped by vendor");
  if (!s || !g) return;
  // Read-only, hidden and bypass parameters are not exposed.
  expect(s->params.size() == 3 && s->params[0].name == "Gain" && s->params[1].kind == ParamKind::Toggle &&
             s->params[2].kind == ParamKind::Choice && s->params[2].choices.size() == 3,
         "synth parameters: Gain, toggle, 3-way choice");
  expect(g->params.size() == 2 && g->params[0].name == "Level", "effect parameters: bypass excluded");
  PluginState d = defaultPluginState(kSynth);
  expect(d.params.size() == 3 && std::fabs(d.params[0] - 0.5f) < 1e-6f, "default state uses scanned defaults");
  expect(isHostedType(kSynth) && !isHostedType("gl.synth"), "hosted type prefix");
  bool listed = false;
  for (const PluginInfo* p : dynamicPlugins()) listed |= p->type == kSynth;
  expect(listed, "dynamic plugins listed");
  // Parameters are addressable as ParamIds (automation, MIDI learn, links).
  Document doc(makeEmptySong());
  ObjectId ch = doc.addChannel("vst", kSynth);
  auto desc = describeParam(doc.song(), channelPluginParam(ch, 2));
  expect(desc && desc->kind == ParamKind::Choice, "hosted parameter resolves as ParamId");
  expect(doc.setParam(channelPluginParam(ch, 0), 0.9) && std::fabs(*getParam(doc.song(), channelPluginParam(ch, 0)) - 0.9) < 1e-6,
         "hosted parameter set through ParamId");
}

void testInstrument() {
  auto g = createGenerator(kSynth);
  expect(g != nullptr, ("hosted instrument instantiates " + host::lastError()).c_str());
  if (!g) return;
  host::HostedInstance* hi = host::hostedInstance(g.get());
  expect(hi && vst3::pluginOf(hi->backend()), "hosted instance reachable from the generator");
  PluginState st = defaultPluginState(kSynth);
  g->prepareHost(st, 48000);
  expect(vst3::pluginOf(hi->backend())->active() && vst3::pluginOf(hi->backend())->sampleRate() == 48000, "prepared and active at 48 kHz");
  expect(std::fabs(vst3::pluginOf(hi->backend())->paramNormalized(2) - 0.42) < 1e-9, "connection points + host messages reach the controller");

  host::UiUpdate u;
  hi->pumpUi(u);
  expect(u.readback && u.values.size() == 3 && std::fabs(u.values[0] - 0.5f) < 1e-6f, "initial parameter readback");

  std::vector<float> L, R;
  runGen(*g, st, L, R, 512);
  expect(peak(L) == 0.0f, "silence without notes");

  // Sample-accurate note on at offset 100 of a block, note off at 300.
  L.clear();
  R.clear();
  g->noteOn(1, 69.0, 1.0f, 100);
  g->noteOff(1, 300);
  runGen(*g, st, L, R, 512, 512);
  expect(firstNonZero(L) == 100, "note starts on its sample offset");
  expect(peak(L, 301) == 0.0f && peak(L, 100, 300) > 0.4f, "note stops on its sample offset");

  // Parameter changes: Gain 0.5 → 1.0 doubles the level.
  float before = peak(L, 100, 300);
  st.params[0] = 1.0f;
  L.clear();
  R.clear();
  g->noteOn(2, 69.0, 1.0f, 0);
  runGen(*g, st, L, R, 512);
  expect(std::fabs(peak(L) - 2 * before) < 0.02f, "parameter change reaches the processor");
  // ...and the controller learns the engine's value on the next UI pump.
  st.params[2] = 2.0f;  // Mode: choice index 2 → normalized 1.0
  runGen(*g, st, L, R, 256);
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(std::fabs(vst3::pluginOf(hi->backend())->paramNormalized(3) - 1.0) < 1e-9 && std::fabs(vst3::pluginOf(hi->backend())->paramNormalized(0) - 1.0) < 1e-9,
         "controller follows engine-side parameter values");

  // Fractional pitch → note tuning; glide via tuning note expression.
  expect(g->supportsGlide(), "glide supported through tuning note expression");
  L.clear();
  R.clear();
  g->noteOff(2, 0);
  g->noteOn(3, 60.0, 1.0f, 0);
  runGen(*g, st, L, R, 9600);
  g->glide(3, 72.0, 0.01, 0);
  runGen(*g, st, L, R, 9600 * 2);
  int lo = crossings(L, 0, 9600), hi2 = crossings(L, 9600 * 2, 9600 * 3);
  expect(lo > 0 && std::abs(hi2 - 2 * lo) <= 4, "glide up an octave doubles the frequency");

  // Edits made in the plugin's own editor come back as song edits.
  vst3::pluginOf(hi->backend())->setParamNormalized(5, 0.8);  // test hook: the controller performs an edit of Gain
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(u.edits.size() == 1 && u.edits[0].index == 0 && std::fabs(u.edits[0].value - 0.8f) < 1e-6f && u.gestureEnd,
         "performEdit becomes a parameter edit + gesture end");

  // State round trip: component (incl. notes played) + controller state.
  std::string data;
  expect(hi->captureState(data) && data.compare(0, 7, "vst3s1:") == 0, "state captured");
  auto g2 = createGenerator(kSynth);
  host::HostedInstance* h2 = host::hostedInstance(g2.get());
  PluginState st2 = defaultPluginState(kSynth);
  st2.data = data;
  g2->prepareHost(st2, 48000);
  u = host::UiUpdate{};
  h2->pumpUi(u);
  expect(u.readback && std::fabs(u.values[0] - 1.0f) < 1e-6f && std::fabs(u.values[2] - 2.0f) < 1e-6f,
         "restored state reads back into the parameters");
  std::string data2;
  h2->captureState(data2);
  std::string a1, b1, a2, b2;
  host::decodeState(data, a1, b1);
  host::decodeState(data2, a2, b2);
  expect(a1 == a2 && b1 == b2 && !b1.empty(), "component + controller state survive save/restore");
  // Same data again: no reload (prepareHost is cheap on every program build).
  g2->prepareHost(st2, 48000);
  u = host::UiUpdate{};
  h2->pumpUi(u);
  expect(!u.readback, "unchanged state is not reloaded");
}

void testEffect() {
  auto fx = createEffect(kGain);
  expect(fx != nullptr, "hosted effect instantiates");
  if (!fx) return;
  PluginState st = defaultPluginState(kGain);
  fx->prepareHost(st, 48000);
  expect(fx->latencySamples() == 32, "latency reported for compensation");
  const int n = 256;
  std::vector<float> L(n), R(n), scL(n, 0.25f), scR(n, -0.25f);
  for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = std::sin(i * 0.05f) * 0.5f;
  std::vector<float> in = L;
  ProcessContext c = ctx48();
  fx->processAux(st, ChannelAux{}, c, L.data(), R.data(), n);
  bool unity = true;
  for (int i = 0; i < n; ++i) unity = unity && std::fabs(L[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]) < 1e-6f;
  expect(unity, "effect at default level passes audio");
  st.params[0] = 1.0f;
  L = in;
  R = in;
  fx->processAux(st, ChannelAux{}, c, L.data(), R.data(), n);
  expect(std::fabs(L[40] - 2 * in[40]) < 1e-5f, "effect parameter change applied");
  // Sidechain bus: Level 0, Sidechain 1 → output is the sidechain signal.
  st.params[0] = 0.0f;
  st.params[1] = 1.0f;
  SideIo io;
  io.scL = scL.data();
  io.scR = scR.data();
  fx->setSideIo(io);
  L = in;
  R = in;
  fx->processAux(st, ChannelAux{}, c, L.data(), R.data(), n);
  expect(std::fabs(L[10] - 0.25f) < 1e-6f && std::fabs(R[10] + 0.25f) < 1e-6f, "sidechain input reaches the plugin");
  // Sample-rate change re-prepares the instance.
  fx->prepareHost(st, 44100);
  expect(vst3::pluginOf(host::hostedInstance(fx.get())->backend())->sampleRate() == 44100, "sample-rate change re-prepares");
}

void testProjectAndRender() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  ObjectId pat = d.song().patterns[0].id;
  ObjectId ch = d.addChannel("vst", kSynth);
  d.addNote(pat, ch, {0, 96, 69, 127});
  d.addClip(pat, 0, 0);
  ObjectId track = d.song().mixer[1].id;
  expect(d.setSlotPlugin(track, 0, kGain), "hosted effect inserted into a mixer slot");
  d.routeChannel(ch, track);
  RenderOptions o;
  o.blockSize = 333;
  AudioBuffer out = renderSong(d.song(), o);
  expect(out.peak() > 0.3f, "hosted instrument + effect render offline");

  // Project round trip keeps the hosted state; a missing plugin's slot is kept.
  Song s = d.song();
  s.channels.back().plugin.data = host::encodeState("abc", "def");
  s.mixer[1].slots[1].plugin = PluginState{"vst3:00000000000000000000000000000000", {0.3f}, "vst3s1:YQ==:"};
  Json j = songToJson(s);
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err), ("project with hosted plugins loads " + err).c_str());
  expect(back.channels.back().plugin == s.channels.back().plugin, "hosted instrument state survives the project round trip");
  expect(back.mixer[1].slots[1].plugin.type == "vst3:00000000000000000000000000000000" && back.mixer[1].slots[1].plugin.data == "vst3s1:YQ==:",
         "missing hosted plugin keeps its state");
  expect(createEffect("vst3:00000000000000000000000000000000") == nullptr && !host::lastError().empty(),
         "missing plugin: no instance, error reported");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("vst3");
  testHostObjects();
  testModuleAndScan();
  testCatalogue();
  testInstrument();
  testEffect();
  testProjectAndRender();
  return finish();
}

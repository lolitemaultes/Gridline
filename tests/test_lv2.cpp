// Gridline — LV2 hosting tests (PH-04): the Turtle reader, bundle discovery
// and scanning, parameters (ranges, enumerations, toggles, logarithmic),
// sample-accurate control changes, atom MIDI in / out, time:Position, the
// worker, state, latency, presets (also from another bundle), bypass,
// offline mode, interfaces (Cocoa on macOS, external), the sandbox and the
// compatibility sweep. Uses only the in-tree test bundles (tests/lv2).
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Json.h"
#include "core/Settings.h"
#include "engine/Program.h"
#include "host/HostedPlugins.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"
#include "host/lv2/Lv2World.h"
#include "host/lv2/Turtle.h"
#include "host/sandbox/Sandbox.h"
#include "lv2/Probe.h"
#include "core/Platform.h"
#include "host/Dylib.h"


namespace fs = std::filesystem;
using namespace gl;

namespace {

const std::string kDir = GRIDLINE_LV2_DIR;
const std::string kBundle = GRIDLINE_LV2_BUNDLE;
const std::string kGain = "lv2:urn:gridline:lv2test:gain";
const std::string kSynth = "lv2:urn:gridline:lv2test:synth";
const std::string kState = "lv2:urn:gridline:lv2test:state";

ProcessContext ctx48() {
  ProcessContext c;
  c.sampleRate = 48000;
  c.tempo = 120;
  return c;
}
void runGen(Generator& g, const PluginState& st, std::vector<float>& L, std::vector<float>& R, int n, int block = 256,
            const ProcessContext& c0 = ctx48()) {
  ChannelAux aux;
  size_t base = L.size();
  L.resize(base + static_cast<size_t>(n), 0.0f);
  R.resize(base + static_cast<size_t>(n), 0.0f);
  for (int pos = 0; pos < n; pos += block) {
    int len = std::min(block, n - pos);
    g.process(st, aux, c0, L.data() + base + pos, R.data() + base + pos, len);
  }
}
float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}
int firstNonZero(const std::vector<float>& v) {
  for (size_t i = 0; i < v.size(); ++i)
    if (std::fabs(v[i]) > 1e-7f) return static_cast<int>(i);
  return -1;
}
bool waitFor(const std::function<bool()>& f, double seconds) {
  auto t0 = std::chrono::steady_clock::now();
  while (!f()) {
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}
template <class T>
T* probe(const std::string& binary, const char* symbol) {
  void* h = host::dl::loaded(binary);
  if (!h) h = host::dl::open(binary, nullptr, true);
  void* f = h ? host::dl::sym(h, symbol) : nullptr;
  return f ? host::toFunction<T* (*)()>(f)() : nullptr;
}
std::string dspBinary() { return kBundle + "/GridlineTestLv2" + GRIDLINE_LV2_EXT; }
std::string uiBinary() { return kBundle + "/GridlineTestLv2UI" + GRIDLINE_LV2_EXT; }
int paramIndex(const std::string& type, const std::string& name) {
  const PluginInfo* pi = findPlugin(type);
  if (!pi) return -1;
  for (size_t k = 0; k < pi->params.size(); ++k)
    if (pi->params[k].name == name) return static_cast<int>(k);
  return -1;
}
const host::ScannedClass* findClass(const host::ScanResult& r, const std::string& uid) {
  for (const auto& c : r.classes)
    if (c.uid == uid) return &c;
  return nullptr;
}

// ---------------------------------------------------------------- Turtle

void testTurtle() {
  namespace t = host::lv2;
  expect(t::resolveIri("file:///a/b/c.ttl", "d.so") == "file:///a/b/d.so", "IRI: relative file");
  expect(t::resolveIri("file:///a/b/c.ttl", "../x/y.ttl") == "file:///a/x/y.ttl", "IRI: parent segments");
  expect(t::resolveIri("file:///a/b/c.ttl", "#frag") == "file:///a/b/c.ttl#frag", "IRI: fragment");
  expect(t::resolveIri("file:///a/b/c.ttl", "/abs.ttl") == "file:///abs.ttl", "IRI: absolute path");
  expect(t::resolveIri("file:///a/b/c.ttl", "urn:x:y") == "urn:x:y", "IRI: other scheme kept");
  std::string p;
  expect(t::fileUri("/a b/c%d.lv2") == "file:///a%20b/c%25d.lv2" && t::uriToPath("file:///a%20b/c%25d.lv2", p) && p == "/a b/c%d.lv2",
         "file URIs percent-encode paths (spaces)");

  t::Model m;
  std::string err;
  auto num = [&m](const std::string& subj, const std::string& pred) {
    const t::Node* n = m.object(subj, pred);
    return n ? n->number() : std::nan("");
  };
  auto truth = [&m](const std::string& subj, const std::string& pred) {
    const t::Node* n = m.object(subj, pred);
    return n && n->truth();
  };
  const std::string doc = R"TTL(
# comment
@prefix ex: <http://ex.org/ns#> .
@base <file:///bundle/x.lv2/manifest.ttl> .
PREFIX q: <http://q.org/>
<thing> a ex:Plugin , ex:Other ;
  ex:name "Name \"quoted\" é"@en ;
  ex:long """line 1
line "2" """ ;
  ex:int 42 ; ex:dec -0.25 ; ex:dbl 1.5E3 ; ex:yes true ; ex:no false ;
  ex:typed "7"^^<http://www.w3.org/2001/XMLSchema#int> ;
  ex:list ( 1 2 3 ) ;
  ex:port [ ex:index 0 ; ex:sym "in" ] , [ ex:index 1 ; ex:sym "out" ; ] ;
  q:local.name _:b1 ;
  ex:bin <thing.so> .
_:b1 ex:v 3 .
[] ex:anon "x" .
)TTL";
  const bool ok = m.loadString(doc, "file:///ignored/base.ttl", &err);
  expect(ok, ("Turtle document parses " + err).c_str());
  const std::string s = "file:///bundle/x.lv2/thing";
  expect(m.has(s, "http://www.w3.org/1999/02/22-rdf-syntax-ns#type", "http://ex.org/ns#Plugin") &&
             m.has(s, "http://www.w3.org/1999/02/22-rdf-syntax-ns#type", "http://ex.org/ns#Other"),
         "Turtle: 'a' with an object list, @base resolution");
  const host::lv2::Node* name = m.object(s, "http://ex.org/ns#name");
  expect(name && name->value == "Name \"quoted\" \xC3\xA9" && name->lang == "en", "Turtle: escapes, \\u, language tag");
  expect(m.text(s, "http://ex.org/ns#long") == "line 1\nline \"2\" ", "Turtle: long string");
  const auto* i42 = m.object(s, "http://ex.org/ns#int");
  const auto* dec = m.object(s, "http://ex.org/ns#dec");
  const auto* dbl = m.object(s, "http://ex.org/ns#dbl");
  expect(i42 && i42->number() == 42 && i42->datatype == "http://www.w3.org/2001/XMLSchema#integer" && dec && dec->number() == -0.25 &&
             dec->datatype.find("decimal") != std::string::npos && dbl && dbl->number() == 1500 && dbl->datatype.find("double") != std::string::npos,
         "Turtle: integer, decimal, double");
  expect(truth(s, "http://ex.org/ns#yes") && m.object(s, "http://ex.org/ns#no") && !truth(s, "http://ex.org/ns#no"), "Turtle: booleans");
  expect(num(s, "http://ex.org/ns#typed") == 7, "Turtle: typed literal");
  const auto ports = m.objects(s, "http://ex.org/ns#port");
  expect(ports.size() == 2 && ports[0]->isBlank() && m.text(ports[1]->key(), "http://ex.org/ns#sym") == "out", "Turtle: blank node property lists");
  const auto* lst = m.object(s, "http://ex.org/ns#list");
  expect(lst && lst->isBlank() && num(lst->key(), "http://www.w3.org/1999/02/22-rdf-syntax-ns#first") == 1, "Turtle: collections");
  const auto* b1 = m.object(s, "http://q.org/local.name");
  expect(b1 && b1->isBlank() && num(b1->key(), "http://ex.org/ns#v") == 3, "Turtle: SPARQL PREFIX, dotted local names, _:labels");
  expect(m.text(s, "http://ex.org/ns#bin") == "file:///bundle/x.lv2/thing.so", "Turtle: relative IRI objects");
  t::Model bad;
  expect(!bad.loadString("<a> <b> \"unterminated .", "file:///x", &err) && err.find("line") != std::string::npos, "Turtle: syntax errors reported");
  expect(!bad.loadString("<a> nope:b <c> .", "file:///x", &err), "Turtle: undefined prefix is an error");
}

// ---------------------------------------------------------------- scanning

void testScan() {
  const host::Format* f = host::findFormat("lv2");
  expect(f && std::string(f->label) == "LV2", "LV2 format registered");
  if (!f) return;
  std::string fmt, uid;
  expect(host::splitType(kGain, fmt, uid) && fmt == "lv2" && uid == "urn:gridline:lv2test:gain" && isHostedType(kGain), "lv2: type ids");
  const auto paths = f->defaultPaths();
  expect(std::find(paths.begin(), paths.end(), kDir) != paths.end(), "LV2_PATH folders are searched");
#if defined(__APPLE__)
  expect(std::find_if(paths.begin(), paths.end(), [](const std::string& p) { return p.find("Plug-Ins/LV2") != std::string::npos; }) != paths.end(),
         "macOS LV2 folders");
#endif
  const auto found = f->discover({kDir});
  expect(found.size() == 1 && found[0] == kBundle, "discovery: the plugin bundle (not the presets-only bundle)");
  host::ScanResult r = f->scanInProcess(kBundle);
  expect(r.ok && r.classes.size() == 3, ("LV2 scan: 3 plugins " + r.error).c_str());
  const auto* gain = findClass(r, "urn:gridline:lv2test:gain");
  const auto* synth = findClass(r, "urn:gridline:lv2test:synth");
  const auto* state = findClass(r, "urn:gridline:lv2test:state");
  expect(gain && !gain->instrument && gain->inputChannels == 2 && gain->outputChannels == 2 && gain->sidechainChannels == 1 &&
             gain->name == "Gridline Test Gain (LV2)" && gain->vendor == "Gridline Tests" && gain->version == "0.2.7" &&
             gain->subCategories == "AmplifierPlugin",
         "gain: stereo effect, sidechain, name / maintainer / version / class");
  expect(gain && gain->programs.size() == 3 && gain->programs[0] == "Extra" && gain->programs[1] == "Loud" && gain->programs[2] == "Quiet",
         "gain: presets of its bundle and of another bundle");
  if (gain) {
    const host::ScannedParam* mode = nullptr;
    const host::ScannedParam* enabled = nullptr;
    for (const auto& p : gain->params) {
      if (p.title == "Mode") mode = &p;
      if (p.title == "Enabled") enabled = &p;
    }
    expect(mode && mode->stepCount == 2 && mode->labels.size() == 3 && mode->labels[2] == "Hot", "enumeration: scale point names");
    expect(enabled && (enabled->flags & host::kParamBypass), "lv2:enabled is the bypass, not a parameter");
  }
  expect(synth && synth->instrument && synth->eventInput && synth->eventOutput && synth->outputChannels == 2 && synth->inputChannels == 0 &&
             synth->vendor == "Gridline Tests",
         "synth: instrument, atom MIDI in / out, stereo, project name as vendor");
  expect(state && state->latency == 32 && state->inputChannels == 1 && state->outputChannels == 1, "state plugin: mono, latency port 32");
  host::ScanResult iso = host::scanIsolated(GRIDLINE_CLI_EXE, "lv2", kBundle, 30);
  expect(iso.ok && iso.classes.size() == 3, ("LV2 isolated scan " + iso.error).c_str());
  expect(iso.mtime > 0 && iso.mtime == f->mtime(kBundle), "bundle stamp");
  host::registerScanResult(r);
  const PluginInfo* pi = findPlugin(kGain);
  expect(pi && pi->kind == PluginKind::Effect && pi->presets.size() == 3, "gain registered as an effect with its presets");
  const int mi = paramIndex(kGain, "Mode"), ii = paramIndex(kGain, "Invert");
  expect(mi >= 0 && pi->params[static_cast<size_t>(mi)].kind == ParamKind::Choice && ii >= 0 && pi->params[static_cast<size_t>(ii)].kind == ParamKind::Toggle,
         "enumeration -> choice, toggled -> toggle");
  expect(findPlugin(kSynth) && findPlugin(kSynth)->kind == PluginKind::Generator, "synth registered as an instrument");
  // A broken bundle is reported, not fatal.
  const std::string broken = (fs::path(gl::test::hermeticTestEnv("lv2")) / "Broken.lv2").string();
  fs::create_directories(broken);
  std::ofstream(broken + "/manifest.ttl") << "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n<urn:x> a lv2:Plugin ; lv2:binary <missing.so> ; <urn:p> \"open .\n";
  host::ScanResult rb = f->scanInProcess(broken);
  expect(!rb.ok && !rb.error.empty(), ("broken bundle fails with a reason: " + rb.error).c_str());
}

// ---------------------------------------------------------------- effect

void testGain() {
  auto fx = createEffect(kGain);
  expect(fx != nullptr, ("LV2 effect instantiates " + host::lastError()).c_str());
  if (!fx) return;
  host::HostedInstance* hi = host::hostedInstance(fx.get());
  PluginState st = defaultPluginState(kGain);
  fx->prepareHost(st, 48000);
  host::UiUpdate u;
  hi->pumpUi(u);
  const int gi = paramIndex(kGain, "Gain"), fi = paramIndex(kGain, "Frequency"), mi = paramIndex(kGain, "Mode"), ii = paramIndex(kGain, "Invert"),
            si = paramIndex(kGain, "Side");
  expect(gi >= 0 && fi >= 0 && mi >= 0 && ii >= 0 && si >= 0, "gain parameters present");
  if (gi < 0 || fi < 0 || mi < 0 || ii < 0 || si < 0) return;
  expect(u.readback && std::fabs(u.values[static_cast<size_t>(gi)] - 0.5f) < 1e-6f, "defaults read back (gain 1 of 0..2 = 0.5)");
  expect(std::fabs(st.params[static_cast<size_t>(fi)] - std::log(50.0f) / std::log(1000.0f)) < 1e-4f, "logarithmic default (1 kHz of 20..20000)");
  expect(hi->paramText(fi, 0.5f) == "632.5 Hz" && hi->paramText(mi, 2.0f) == "Hot" && hi->paramText(ii, 1.0f) == "On" &&
             hi->paramText(gi, 0.25f) == "0.500",
         ("parameter text: log, enumeration, toggle, plain (" + hi->paramText(fi, 0.5f) + ")").c_str());
  const int n = 256;
  std::vector<float> in(n), x(n), y(n);
  for (int i = 0; i < n; ++i) in[static_cast<size_t>(i)] = std::sin(i * 0.05f) * 0.5f;
  ProcessContext c = ctx48();
  auto run = [&](const PluginState& s) {
    x = in;
    y = in;
    fx->processAux(s, ChannelAux{}, c, x.data(), y.data(), n);
  };
  run(st);
  expect(std::fabs(x[100] - in[100]) < 1e-6f && std::fabs(y[100] - in[100]) < 1e-6f, "unity gain passes audio");
  st.params[static_cast<size_t>(gi)] = 0.25f;  // 0.5
  run(st);
  expect(std::fabs(x[100] - 0.5f * in[100]) < 1e-6f, "gain change reaches the plugin");
  st.params[static_cast<size_t>(ii)] = 1.0f;
  run(st);
  expect(std::fabs(x[100] + 0.5f * in[100]) < 1e-6f, "toggle reaches the plugin");
  st.params[static_cast<size_t>(ii)] = 0.0f;
  // Sidechain.
  std::vector<float> scL(n, 0.25f), scR(n, 0.25f);
  SideIo io;
  io.scL = scL.data();
  io.scR = scR.data();
  fx->setSideIo(io);
  st.params[static_cast<size_t>(si)] = 1.0f;
  run(st);
  expect(std::fabs(x[100] - (0.5f * in[100] + 0.25f)) < 1e-6f, "sidechain input reaches the plugin");
  st.params[static_cast<size_t>(si)] = 0.0f;
  fx->setSideIo(SideIo{});
  // Bypass through lv2:enabled.
  host::Backend* b = hi->backend();
  expect(b->hasBypass() && b->setBypass(true), "lv2:enabled bypass");
  run(st);
  expect(std::fabs(x[100] - in[100]) < 1e-6f, "bypassed: input passes unchanged");
  b->setBypass(false);
  run(st);
  expect(std::fabs(x[100] - 0.5f * in[100]) < 1e-6f, "bypass off again");
  // Presets: own bundle and another bundle.
  expect(hi->programCount() == 3 && hi->programName(2) == "Quiet", "presets listed");
  expect(hi->selectProgram(2), "preset selected");
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(u.readback && std::fabs(u.values[static_cast<size_t>(gi)] - 0.125f) < 1e-6f && std::fabs(u.values[static_cast<size_t>(mi)] - 1.0f) < 1e-6f &&
             u.captureState,
         "preset values read back (gain 0.25, mode Warm), state captured");
  for (size_t k = 0; k < u.values.size(); ++k) st.params[k] = u.values[k];
  run(st);
  expect(std::fabs(x[100] - 0.25f * in[100]) < 1e-6f, "preset applies to the sound");
  hi->selectProgram(0);  // "Extra": gain 0.5, inverted
  u = host::UiUpdate{};
  hi->pumpUi(u);
  expect(u.readback && std::fabs(u.values[static_cast<size_t>(ii)] - 1.0f) < 1e-6f && std::fabs(u.values[static_cast<size_t>(gi)] - 0.25f) < 1e-6f,
         "preset from a presets-only bundle");
  for (size_t k = 0; k < u.values.size(); ++k) st.params[k] = u.values[k];
  // State round trip.
  std::string data;
  expect(hi->captureState(data) && data.compare(0, 6, "lv2s1:") == 0, "LV2 state captured");
  auto fx2 = createEffect(kGain);
  PluginState st2 = st;
  st2.data = data;
  fx2->prepareHost(st2, 48000);
  std::string data2;
  host::hostedInstance(fx2.get())->captureState(data2);
  expect(data == data2, "LV2 state survives save / restore");
  x = in;
  y = in;
  fx2->processAux(st2, ChannelAux{}, c, x.data(), y.data(), n);
  expect(std::fabs(x[100] + 0.5f * in[100]) < 1e-6f, "restored instance sounds the same");
  // A different sample rate re-instantiates and keeps the values.
  st2.params[static_cast<size_t>(gi)] = 0.5f;
  fx2->prepareHost(st2, 96000);
  x = in;
  y = in;
  fx2->processAux(st2, ChannelAux{}, c, x.data(), y.data(), n);
  expect(host::hostedInstance(fx2.get())->backend()->sampleRate() == 96000 && std::fabs(x[100] + in[100]) < 1e-6f,
         "new sample rate: instance recreated with its values");
}

// Sample-accurate control changes: the host splits run() at the change.
void testSampleAccurate() {
  std::string err;
  auto b = host::createBackend("lv2", kBundle, "urn:gridline:lv2test:gain", &err);
  expect(b != nullptr, ("backend " + err).c_str());
  if (!b) return;
  expect(b->prepare(48000, 512), "prepare");
  for (int c = 0; c < 2; ++c) std::fill(b->inputChannel(c), b->inputChannel(c) + 512, 1.0f);
  b->addParamChange(4, 0.25, 100);  // gain 0.5 from sample 100
  b->addParamChange(4, 0.75, 300);  // gain 1.5 from sample 300
  host::Transport t;
  t.sampleRate = 48000;
  expect(b->process(512, t), "process");
  const float* o = b->outputChannel(0, 0);
  expect(o[99] == 1.0f && o[100] == 0.5f && o[299] == 0.5f && o[300] == 1.5f && o[511] == 1.5f, "control changes land on their sample");
  expect(std::fabs(b->paramNormalized(4) - 0.75) < 1e-6, "value cache follows the audio thread");
  // Random block sizes stay finite and consistent.
  bool fine = true;
  uint32_t rs = 1;
  for (int k = 0; k < 64; ++k) {
    rs = rs * 1664525u + 1013904223u;
    const int n = 1 + static_cast<int>((rs >> 8) % 512);
    b->addParamChange(4, (rs >> 4) % 100 / 100.0, static_cast<int>(rs % static_cast<uint32_t>(n)));
    b->process(n, t);
    for (int i = 0; i < n; ++i) fine = fine && std::isfinite(b->outputChannel(0, 0)[i]);
  }
  expect(fine, "random block sizes");
}

// ---------------------------------------------------------------- instrument

void testSynth() {
  auto* pr = probe<GridlineLv2Probe>(dspBinary(), "gridline_lv2_probe");
  expect(pr != nullptr, "LV2 test probe reachable");
  auto g = createGenerator(kSynth);
  expect(g != nullptr, ("LV2 instrument instantiates " + host::lastError()).c_str());
  if (!g || !pr) return;
  PluginState st = defaultPluginState(kSynth);
  g->prepareHost(st, 48000);
  std::vector<float> L, R;
  g->noteOn(1, 69.0, 1.0f, 100);
  g->noteOff(1, 300);
  runGen(*g, st, L, R, 512, 512);
  expect(firstNonZero(L) == 100 && peak(L, 301) == 0.0f && peak(L) > 0.3f, "atom MIDI notes are sample-accurate");
  // MIDI output (echo an octave up).
  const int ei = paramIndex(kSynth, "Echo");
  st.params[static_cast<size_t>(ei)] = 1.0f;
  L.clear();
  R.clear();
  g->noteOn(2, 60, 1.0f, 33);
  runGen(*g, st, L, R, 256, 256);
  Generator::MidiOutEvent ev[8];
  const int n = g->takeMidiOut(ev, 8);
  expect(n == 1 && (ev[0].s & 0xF0) == 0x90 && ev[0].a == 72 && ev[0].offset == 33, "atom MIDI output reaches the engine with its offset");
  // Transport: time:Position.
  ProcessContext c = ctx48();
  c.tempo = 140;
  c.ppq = 96;
  c.tick = 96 * 7.5;
  c.playing = true;
  c.tsNum = 3;
  c.tsDen = 4;
  const int before = pr->positions;
  runGen(*g, st, L, R, 256, 256, c);
  expect(pr->positions > before && pr->bpm == 140 && pr->speed == 1 && pr->beatsPerBar == 3 && pr->beatUnit == 4 && std::fabs(pr->beat - 7.5) < 1e-9 &&
             pr->bar == 2 && std::fabs(pr->barBeat - 1.5f) < 1e-6f,
         ("time:Position: tempo, metre, beat, bar (" + std::to_string(pr->beat) + ", bar " + std::to_string(pr->bar) + ")").c_str());
  // Continuing playback sends no new position; a jump does.
  const int p1 = pr->positions;
  ProcessContext c2 = c;
  c2.tick = c.tick + 96.0 * 256 / (48000 * 60.0 / 140);
  runGen(*g, st, L, R, 256, 256, c2);
  const int p2 = pr->positions;
  ProcessContext c3 = c;
  c3.tick = 0;
  runGen(*g, st, L, R, 256, 256, c3);
  expect(p2 == p1 && pr->positions == p2 + 1 && pr->beat == 0, "time:Position only on changes (jump: yes, continuous play: no)");
  host::HostedInstance* hi = host::hostedInstance(g.get());
  expect(!hi->backend()->hasEditor(), "no interface of its own: Gridline's generic parameter view");
}

// ---------------------------------------------------------------- worker / state / latency

void testState() {
  auto* pr = probe<GridlineLv2Probe>(dspBinary(), "gridline_lv2_probe");
  auto fx = createEffect(kState);
  expect(fx != nullptr && pr, ("state plugin instantiates " + host::lastError()).c_str());
  if (!fx || !pr) return;
  host::HostedInstance* hi = host::hostedInstance(fx.get());
  PluginState st = defaultPluginState(kState);
  fx->prepareHost(st, 48000);
  expect(fx->latencySamples() == 32 && hi->backend()->latency() == 32, "latency port reported to delay compensation");
  expect(pr->maxBlock >= 512, "options: maxBlockLength given");
  const int n = 256;
  std::vector<float> in(n, 0.0f), x(n), y(n);
  in[10] = 1.0f;
  ProcessContext c = ctx48();
  auto run = [&](const PluginState& s) {
    x = in;
    y = in;
    fx->processAux(s, ChannelAux{}, c, x.data(), y.data(), n);
  };
  run(st);
  expect(std::fabs(x[42] - 1.0f) < 1e-6f, "impulse delayed by 32 samples (mono plugin)");
  // Worker: a rising trigger schedules work on the worker thread.
  const int ti = paramIndex(kState, "Load");
  expect(ti >= 0, "trigger parameter");
  if (ti < 0) return;
  const int works = pr->works;
  st.params[static_cast<size_t>(ti)] = 1.0f;
  run(st);
  bool loaded = waitFor(
      [&] {
        run(st);
        return pr->responses > 0 && std::fabs(x[42] - 0.5f) < 1e-6f;
      },
      5.0);
  expect(loaded && pr->works == works + 1 && !pr->workOnRunThread, "worker: work on its own thread, response applied in the audio thread");
  std::string data;
  expect(hi->captureState(data) && data.compare(0, 6, "lv2s1:") == 0, "state with properties captured");
  {
    auto fx2 = createEffect(kState);
    PluginState st2 = st;
    st2.data = data;
    const int restores = pr->restores;
    fx2->prepareHost(st2, 48000);
    std::vector<float> a = in, b2 = in;
    fx2->processAux(st2, ChannelAux{}, c, a.data(), b2.data(), n);
    std::string data2;
    host::hostedInstance(fx2.get())->captureState(data2);
    expect(pr->restores > restores && std::fabs(a[42] - 0.5f) < 1e-6f && data2 == data, "state extension restores the loaded data");
  }
  // Offline (export): freeWheeling port on, work done synchronously.
  host::Backend* b = hi->backend();
  expect(b->setOffline(true), "offline mode understood");
  st.params[static_cast<size_t>(ti)] = 0.0f;
  run(st);
  st.params[static_cast<size_t>(ti)] = 1.0f;
  run(st);  // work done inside run(), the response follows right after it
  run(st);
  expect(pr->freewheel == 1.0f && pr->workOnRunThread && std::fabs(x[42] - 0.25f) < 1e-6f, "offline: freeWheeling, synchronous worker");
  b->setOffline(false);
  // Preset with state properties.
  expect(hi->programCount() == 1 && hi->selectProgram(0), "state preset");
  run(st);
  expect(std::fabs(x[42] - 0.75f) < 1e-6f, "preset state (xsd:float property) restored");
}

// ---------------------------------------------------------------- interfaces

void testEditors() {
  auto* up = probe<GridlineLv2UiProbe>(uiBinary(), "gridline_lv2_ui_probe");
  // External (kxstudio) interface of the state plugin.
  {
    std::string err;
    auto b = host::createBackend("lv2", kBundle, "urn:gridline:lv2test:state", &err);
    expect(b && b->hasEditor(), "external interface offered");
    if (b && b->prepare(48000, 512)) {
      auto e = b->createEditor();
      expect(e && e->external(), "external interface: own window");
      if (e) {
        expect(e->attach(nullptr), "external interface shown");
        up = probe<GridlineLv2UiProbe>(uiBinary(), "gridline_lv2_ui_probe");
        expect(up && up->shows == 1, "show() called");
        host::Transport t;
        t.sampleRate = 48000;
        b->process(64, t);  // the Int message (10) from show() reaches the plugin, which reports its counter
        e->idle();
        expect(up && up->runs >= 1 && up->atomEvents >= 1 && up->lastAtomInt == 10, "atom messages both ways (interface -> plugin -> interface)");
        expect(e->isOpen(), "open");
        if (up) up->closeNow = true;
        e->idle();
        expect(!e->isOpen(), "closing its window is noticed");
        e.reset();
      }
    }
  }
#if defined(__APPLE__)
  // Cocoa interface of the gain, embedded in a host view.
  {
    std::string err;
    auto b = host::createBackend("lv2", kBundle, "urn:gridline:lv2test:gain", &err);
    expect(b && b->hasEditor(), "Cocoa interface offered");
    if (!b || !b->prepare(48000, 512)) return;
    auto e = b->createEditor();
    expect(e && !e->external(), "Cocoa interface: embedded");
    if (!e) return;
    int w = 0, h = 0;
    expect(!e->size(w, h), "size unknown before the interface exists");
    void* parent = host::offscreenEditorParent(640, 420);
    int askedW = 0, askedH = 0;
    e->requestResize = [&](int nw, int nh) {
      askedW = nw;
      askedH = nh;
      return true;
    };
    expect(parent && e->attach(parent), "Cocoa interface attached");
    up = probe<GridlineLv2UiProbe>(uiBinary(), "gridline_lv2_ui_probe");
    expect(up && up->instances == 1 && up->resizeAsked && askedW == 320 && askedH == 180, "ui:resize request reaches the window");
    expect(e->size(w, h) && w == 320 && h == 180, "interface size");
    expect(up && up->lastGain == 1.0f && up->portEvents >= 8, "initial port values sent to the interface");
    std::vector<host::HostEvent> evs;
    b->drainHostEvents(evs);
    bool edit = false, end = false;
    for (const auto& ev : evs) {
      if (ev.kind == host::HostEvent::PerformEdit && ev.id == 8 && std::fabs(ev.value - 0.5) < 1e-6) edit = true;
      if (ev.kind == host::HostEvent::EndEdit && ev.id == 8) end = true;
    }
    expect(edit && end, "interface edit (echo = gain / 2) becomes a host edit");
    // A value change from the host reaches the interface on the next idle.
    b->addParamChange(4, 0.25, 0);
    host::Transport t;
    t.sampleRate = 48000;
    b->process(64, t);
    e->idle();
    expect(up && up->lastGain == 0.5f, "host value changes reach the interface");
    expect(e->canResize(), "resizable interface (ui:resize)");
    e->setSize(400, 240);
    expect(up && up->width == 400 && e->size(w, h) && w == 400 && h == 240, "host resizes the interface");
    e.reset();
    expect(up && up->instances == 0, "interface cleaned up");
    host::closeOffscreenEditorParent();
  }
#endif
}

// ---------------------------------------------------------------- sandbox

void testSandbox() {
  host::setChildExecutable(GRIDLINE_CLI_EXE);
  host::setRunInSandbox(kGain, true);
  auto fx = createEffect(kGain);
  expect(fx != nullptr, ("sandboxed LV2 effect starts " + host::lastError()).c_str());
  if (fx) {
    host::HostedInstance* hi = host::hostedInstance(fx.get());
    expect(hi->sandboxed() && std::string(hi->backend()->format()) == "LV2 (sandbox)", "LV2 runs in the sandbox");
    PluginState st = defaultPluginState(kGain);
    st.params[static_cast<size_t>(paramIndex(kGain, "Gain"))] = 0.25f;
    fx->prepareHost(st, 48000);
    std::vector<float> in(256), x(256), y(256);
    for (int i = 0; i < 256; ++i) in[static_cast<size_t>(i)] = std::sin(i * 0.05f) * 0.5f;
    x = in;
    y = in;
    ProcessContext c = ctx48();
    fx->processAux(st, ChannelAux{}, c, x.data(), y.data(), 256);
    expect(std::fabs(x[100] - 0.5f * in[100]) < 1e-6f, "sandbox: LV2 effect processes audio");
    std::string data;
    expect(hi->captureState(data) && data.compare(0, 6, "lv2s1:") == 0 && hi->programCount() == 3, "sandbox: state and presets");
  }
  host::setRunInSandbox(kGain, false);
  host::setRunInSandbox(kSynth, true);
  auto g = createGenerator(kSynth);
  if (g) {
    PluginState st = defaultPluginState(kSynth);
    g->prepareHost(st, 48000);
    std::vector<float> L, R;
    g->noteOn(1, 69.0, 1.0f, 100);
    runGen(*g, st, L, R, 512, 512);
    expect(firstNonZero(L) == 100, "sandbox: sample-accurate atom MIDI");
  }
  host::setRunInSandbox(kSynth, false);
}

// ---------------------------------------------------------------- compatibility sweep

void testPluginCheck() {
#if defined(__APPLE__)
  const bool kOffscreenEditors = true;  // plugin-check opens interfaces in a hidden window
#else
  const bool kOffscreenEditors = false;  // no offscreen parent there: the editor check is skipped
#endif
  // The sweep reads the plugin cache: put this scan into the (hermetic) config folder.
  const std::string cfg = fs::path(Settings::defaultPath()).parent_path().string();
  host::ScanCache cache(cfg + "/plugin-cache.json");
  cache.put(host::findFormat("lv2")->scanInProcess(kBundle));
  expect(cache.save(), "plugin cache written");
  const std::string report = (fs::path(gl::test::hermeticTestEnv("lv2")) / "check.json").string();
  std::vector<std::string> args = {GRIDLINE_CLI_EXE, "plugin-check", "--format", "lv2", "--json", report, "--timeout", "90"};
  platform::Process child;
  const bool started = child.start(GRIDLINE_CLI_EXE, args, platform::Process::kInheritStderr | platform::Process::kInheritStdin);
  if (started) child.wait();
  const int rc = started ? 0 : -1;
  std::ifstream f(report);
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  const bool parsed = jsonParse(ss.str(), j);
  expect(rc == 0 && !child.status().crashed && child.status().code == 0 && parsed, "plugin-check --format lv2 runs without failures");
  if (!parsed) return;
  const Json* plugins = j.find("plugins");
  int count = 0;
  std::string notes;
  if (plugins && plugins->is(Json::Type::Array))
    for (const auto& p : plugins->array) {
      ++count;
      const std::string s = p.text("status");
      // A block slower than real time on a busy machine (shared CI runner) is only a load
      // warning: audio checks whose detail is just levels and load do not count.
      bool loadOnly = s == "warn";
      if (const Json* ch = p.find("checks"); ch && ch->is(Json::Type::Array))
        for (const auto& c : ch->array)
          if (c.text("status") == "warn" && (c.text("detail").rfind("peak ", 0) != 0 || c.text("detail").find("denormal") != std::string::npos))
            loadOnly = false;
      if (s != "pass" && !loadOnly) notes += p.text("name") + ": " + s + "; ";
      if (const Json* ch = p.find("checks"); ch && ch->is(Json::Type::Array))
        for (const auto& c : ch->array)
          if (c.text("status") == "fail" || (kOffscreenEditors && c.text("check") == "editor" && c.text("status") != "pass" &&
                                             p.text("name").find("Synth") == std::string::npos))
            notes += p.text("name") + " / " + c.text("check") + ": " + c.text("status") + " " + c.text("detail") + "; ";
    }
  expect(count == 3, "sweep covers the three LV2 plugins");
  expect(notes.empty(), ("sweep: all pass, interfaces open and close (" + notes + ")").c_str());
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("lv2");
  platform::setEnv("LV2_PATH", kDir.c_str());
  testTurtle();
  testScan();
  testGain();
  testSampleAccurate();
  testSynth();
  testState();
  testEditors();
  testSandbox();
  testPluginCheck();
  return finish();
}

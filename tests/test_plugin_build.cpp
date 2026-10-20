// Gridline — Gridline as a plugin (PH-09): the built Gridline.clap and
// Gridline.vst3 bundles loaded by Gridline's own plugin host, as a DAW would
// load them. Instantiation, audio, MIDI notes, transport sync (tempo,
// position, play state, pattern wrap, own transport when not synced), MIDI
// channel routing and extra outputs, state round trip, macro automation,
// latency reporting, effect input, render determinism, offline mode, and
// `gridline plugin-check` on both bundles.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/Effects.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "host/Backend.h"
#include "host/EditorLayout.h"
#include "host/HostedPlugins.h"
#include "host/PluginScan.h"
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include "clap/Probe.h"
#include "host/sandbox/Sandbox.h"
#include "host/vst3/Vst3Module.h"
#include "vst3/Probe.h"
#endif

namespace fs = std::filesystem;
using namespace gl;

namespace {

const std::string kClap = GRIDLINE_PLUGIN_CLAP;
const std::string kVst3 = GRIDLINE_PLUGIN_VST3;
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

struct Host {
  ProcessContext ctx;
  Host() {
    ctx.sampleRate = kSr;
    ctx.tempo = 120;
    ctx.ppq = 96;
  }
  void advance(int n) {
    if (ctx.playing) ctx.tick += n * ctx.tempo * ctx.ppq / (60.0 * kSr);
  }
};

void runGen(Generator& g, const PluginState& st, Host& h, std::vector<float>& L, std::vector<float>& R, int frames,
            std::vector<std::vector<float>>* aux = nullptr) {
  ChannelAux none;
  for (int pos = 0; pos < frames; pos += kBlock) {
    const int n = std::min(kBlock, frames - pos);
    std::vector<float> l(static_cast<size_t>(n), 0.f), r(static_cast<size_t>(n), 0.f);
    g.process(st, none, h.ctx, l.data(), r.data(), n);
    L.insert(L.end(), l.begin(), l.end());
    R.insert(R.end(), r.begin(), r.end());
    if (aux) {
      aux->resize(static_cast<size_t>(g.auxOutputCount()));
      for (int k = 0; k < g.auxOutputCount(); ++k) {
        const float* al = nullptr;
        const float* ar = nullptr;
        auto& dst = (*aux)[static_cast<size_t>(k)];
        if (g.auxOutput(k, al, ar) && al)
          dst.insert(dst.end(), al, al + n);
        else
          dst.insert(dst.end(), static_cast<size_t>(n), 0.f);
      }
    }
    h.advance(n);
  }
}

float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}
float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  float d = a.size() == b.size() ? 0.f : 1e9f;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
  return d;
}
bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

// A small project: synth channels on inserts 1.., quarter-note steps on the
// first channel of the first pattern.
Song beatSong(int channels, double tempo) {
  Song s = makeEmptySong();
  s.tempo = tempo;
  Document doc(s);
  for (int c = 0; c < channels; ++c) {
    const ObjectId id = doc.addChannel("Voice " + std::to_string(c + 1), "gl.synth");
    const ObjectId mt = doc.song().mixer[static_cast<size_t>(c + 1)].id;
    doc.editChannel(id, "", [&](Channel& ch) { ch.mixerTrack = mt; });
  }
  Song out = doc.song();
  if (!out.patterns.empty() && !out.channels.empty()) {
    std::vector<uint8_t> steps(16, 0);
    for (int k = 0; k < 16; k += 4) steps[static_cast<size_t>(k)] = 100;
    out.patterns[0].steps[out.channels[0].id] = steps;
  }
  return out;
}

// Plugin state (the plugin's own format) for a project and settings.
std::string pluginState(const Song& s, bool follow, int midiMode, ObjectId channel, const std::vector<ParamId>& macros = {},
                        int mode = 0) {
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-plugin");
  j.object["version"] = Json::makeNumber(1);
  j.object["project"] = songToJson(s);
  Json set = Json::makeObject();
  Json f;
  f.type = Json::Type::Bool;
  f.boolean = follow;
  set.object["followHost"] = f;
  set.object["midiMode"] = Json::makeNumber(midiMode);
  set.object["mode"] = Json::makeNumber(mode);
  set.object["pattern"] = Json::makeNumber(s.patterns.empty() ? 0 : s.patterns[0].id);
  set.object["channel"] = Json::makeNumber(channel);
  j.object["settings"] = std::move(set);
  Json ms = Json::makeArray();
  for (const ParamId& p : macros) {
    Json m = Json::makeObject();
    m.object["owner"] = Json::makeNumber(p.owner);
    m.object["index"] = Json::makeNumber(p.index);
    ms.array.push_back(std::move(m));
  }
  j.object["macros"] = std::move(ms);
  return jsonStringify(j, 0);
}
std::string clapData(const std::string& state) { return "clap1:" + host::base64Encode(state); }
std::string vst3Data(const std::string& state) { return "vst3s1:" + host::base64Encode(state) + ":"; }

#if defined(__APPLE__)
// Audio Unit: the state travels in the unit's ClassInfo (a property list)
// under "data"; Gridline's host stores it as "au1:<base64 binary plist>".
constexpr OSType kAuManufacturer = 0x47724C6E;  // GrLn
constexpr OSType kAuInstrument = 0x4772496E;    // GrIn
constexpr OSType kAuEffect = 0x47724678;        // GrFx
std::string auDataFor(const std::string& state, OSType type, OSType subtype) {
  CFMutableDictionaryRef d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  auto num = [&](const char* key, SInt32 v) {
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(d, k, n);
    CFRelease(n);
    CFRelease(k);
  };
  num(kAUPresetVersionKey, 0);
  num(kAUPresetTypeKey, static_cast<SInt32>(type));
  num(kAUPresetSubtypeKey, static_cast<SInt32>(subtype));
  num(kAUPresetManufacturerKey, static_cast<SInt32>(kAuManufacturer));
  CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(state.data()), static_cast<CFIndex>(state.size()));
  CFDictionarySetValue(d, CFSTR(kAUPresetDataKey), data);
  CFDictionarySetValue(d, CFSTR(kAUPresetNameKey), CFSTR("Untitled"));
  CFRelease(data);
  CFDataRef bytes = CFPropertyListCreateData(nullptr, d, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
  CFRelease(d);
  std::string out(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), static_cast<size_t>(CFDataGetLength(bytes)));
  CFRelease(bytes);
  return "au1:" + host::base64Encode(out);
}
std::string auData(const std::string& state) { return auDataFor(state, kAudioUnitType_MusicDevice, kAuInstrument); }
std::string auFxData(const std::string& state) { return auDataFor(state, kAudioUnitType_Effect, kAuEffect); }
// The "data" of a ClassInfo property list.
std::string auStateOf(const std::string& plistBytes) {
  CFDataRef d = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(plistBytes.data()), static_cast<CFIndex>(plistBytes.size()));
  CFPropertyListRef p = CFPropertyListCreateWithData(nullptr, d, kCFPropertyListImmutable, nullptr, nullptr);
  CFRelease(d);
  std::string out;
  if (p && CFGetTypeID(p) == CFDictionaryGetTypeID()) {
    CFTypeRef v = CFDictionaryGetValue(static_cast<CFDictionaryRef>(p), CFSTR(kAUPresetDataKey));
    if (v && CFGetTypeID(v) == CFDataGetTypeID())
      out.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(static_cast<CFDataRef>(v))), static_cast<size_t>(CFDataGetLength(static_cast<CFDataRef>(v))));
  }
  if (p) CFRelease(p);
  return out;
}
// Registers Gridline.component's two units in this process (nothing is
// installed): Gridline's AU host then finds them like installed ones.
bool registerAu() {
  const std::string bin = std::string(GRIDLINE_PLUGIN_AU) + "/Contents/MacOS/Gridline";
  void* h = dlopen(bin.c_str(), RTLD_NOW | RTLD_LOCAL);
  auto factory = h ? reinterpret_cast<AudioComponentFactoryFunction>(dlsym(h, "GridlineAUFactory")) : nullptr;
  if (!factory) return false;
  const AudioComponentDescription inst{kAudioUnitType_MusicDevice, kAuInstrument, kAuManufacturer, 0, 0};
  const AudioComponentDescription fx{kAudioUnitType_Effect, kAuEffect, kAuManufacturer, 0, 0};
  const UInt32 version = 0x00000400;
  return AudioComponentRegister(&inst, CFSTR("Gridline: Gridline"), version, factory) &&
         AudioComponentRegister(&fx, CFSTR("Gridline: Gridline FX"), version, factory);
}
#endif

// Reference: the same song rendered by Gridline's own engine in this process.
std::vector<float> reference(const Song& s, double tempo, double startTick, int frames) {
  Song song = s;
  song.tempo = tempo;
  AudioEngine eng(kSr);
  ProgramBuilder b;
  b.setSampleRate(kSr);
  eng.setProgram(b.build(song));
  eng.setMode(PlayMode::Pattern);
  eng.setPattern(song.patterns[0].id);
  eng.setPosition(static_cast<Tick>(std::llround(startTick)));
  eng.play();
  std::vector<float> L(static_cast<size_t>(frames)), R(static_cast<size_t>(frames));
  for (int pos = 0; pos < frames; pos += kBlock) eng.process(L.data() + pos, R.data() + pos, std::min(kBlock, frames - pos));
  return L;
}

struct Loaded {
  std::unique_ptr<Generator> gen;
  PluginState st;
};

Loaded instrument(const std::string& type, const std::string& data = "") {
  Loaded l;
  l.gen = createGenerator(type);
  l.st = defaultPluginState(type);
  l.st.data = data;
  if (l.gen) l.gen->prepareHost(l.st, kSr);
  return l;
}

const host::ScannedClass* findClass(const host::ScanResult& r, const std::string& uid) {
  for (const auto& c : r.classes)
    if (c.uid == uid) return &c;
  return nullptr;
}

// ---------------------------------------------------------------- one format

void testFormat(const std::string& format, const std::string& bundle, const std::string& instUid, const std::string& fxUid,
                std::string (*wrap)(const std::string&), std::string (*fxWrap)(const std::string&) = nullptr) {
  const std::string tag = format + ": ";
  auto name = [&](const char* s) { return tag + s; };
  // Audio Units are scanned per component (key), the others per bundle.
  std::vector<host::ScanResult> scans;
  if (format == "au") {
    scans.push_back(host::findFormat(format)->scanInProcess(instUid));
    scans.push_back(host::findFormat(format)->scanInProcess(fxUid));
  } else {
    scans.push_back(host::findFormat(format)->scanInProcess(bundle));
  }
  host::ScanResult r = scans[0];
  for (size_t k = 1; k < scans.size(); ++k) {
    r.ok = r.ok && scans[k].ok;
    r.error += scans[k].error;
    r.classes.insert(r.classes.end(), scans[k].classes.begin(), scans[k].classes.end());
  }
  expect(r.ok && r.classes.size() == 2, (name("scan finds Gridline and Gridline FX ") + r.error).c_str());
  const host::ScannedClass* ic = findClass(r, instUid);
  const host::ScannedClass* fc = findClass(r, fxUid);
  expect(ic && ic->instrument && ic->eventInput && ic->outputChannels == 2 && ic->auxOutputs.size() == 15 && ic->params.size() == 16,
         name("instrument: notes in, stereo out, 15 extra outputs, 16 macros").c_str());
  expect(ic && ic->name == "Gridline" && ic->vendor == "Gridline", name("instrument name and vendor").c_str());
  expect(fc && !fc->instrument && fc->inputChannels == 2 && fc->sidechainChannels == 2 && fc->outputChannels == 2,
         name("effect: stereo in, sidechain, stereo out").c_str());
  if (!ic || !fc) return;
  for (const auto& s : scans) host::registerScanResult(s);
  const std::string inst = format + ":" + instUid, fx = format + ":" + fxUid;

  // Instantiate, silence, MIDI note → sound (the default project: starter kit, first channel selected).
  {
    Loaded g = instrument(inst);
    expect(g.gen != nullptr, name("instrument instantiates").c_str());
    if (!g.gen) return;
    Host h;
    std::vector<float> L, R;
    runGen(*g.gen, g.st, h, L, R, 4096);
    expect(peak(L) == 0.f && peak(R) == 0.f, name("stopped, no notes: silence").c_str());
    g.gen->noteOn(1, 60, 1.0f, 100);
    L.clear();
    R.clear();
    runGen(*g.gen, g.st, h, L, R, 8192);
    expect(peak(L, 0, 100) == 0.f && peak(L, 100) > 0.01f && finite(L) && finite(R), name("a MIDI note makes sound from its offset").c_str());
  }

  // Transport sync: project at 140 BPM, host at 120 BPM from beat 5.5 (pattern
  // of one bar: beat 1.5 in the pattern) = Gridline's own engine at 120 BPM from beat 1.5.
  const Song beat = beatSong(2, 140);
  const ObjectId ch0 = beat.channels[0].id, ch1 = beat.channels[1].id;
  const int frames = 48000;
  const std::vector<float> ref = reference(beat, 120, 1.5 * beat.ppq, frames);
  expect(peak(ref) > 0.01f, name("reference render has sound").c_str());
  {
    Loaded g = instrument(inst, wrap(pluginState(beat, true, 0, ch0)));
    Host h;
    h.ctx.ppq = beat.ppq;
    h.ctx.tempo = 120;
    h.ctx.tick = 5.5 * beat.ppq;
    std::vector<float> L, R;
    runGen(*g.gen, g.st, h, L, R, 2048);  // stopped host: Gridline waits
    expect(peak(L) == 0.f, name("host stopped: Gridline does not play").c_str());
    L.clear();
    R.clear();
    h.ctx.playing = true;
    runGen(*g.gen, g.st, h, L, R, frames);
    expect(maxDiff(L, ref) < 1e-5f, name("host plays: tempo, position and pattern wrap follow the host (matches the engine)").c_str());
    // Host stops: the notes end.
    h.ctx.playing = false;
    L.clear();
    R.clear();
    runGen(*g.gen, g.st, h, L, R, 48000);
    expect(peak(L, 36000) < 1e-4f, name("host stops: Gridline stops").c_str());
  }
  {  // Not synced: Gridline's own transport (stopped) ignores the host's play.
    Loaded g = instrument(inst, wrap(pluginState(beat, false, 0, ch0)));
    Host h;
    h.ctx.ppq = beat.ppq;
    h.ctx.playing = true;
    std::vector<float> L, R;
    runGen(*g.gen, g.st, h, L, R, 24000);
    expect(peak(L) == 0.f, name("sync off: the host's transport does not start Gridline").c_str());
  }

  // MIDI channel routing + extra outputs: MIDI channel 1 plays rack channel 1
  // (insert 1 → output 2), MIDI channel 2 plays rack channel 2 (insert 2 → output 3).
  {
    Song two = beat;
    two.patterns[0].steps.clear();
    Loaded g = instrument(inst, wrap(pluginState(two, true, 1, ch0)));
    expect(g.gen->auxOutputCount() == 15, name("15 extra outputs to the engine").c_str());
    Host h;
    std::vector<float> L, R;
    std::vector<std::vector<float>> aux;
    if (format == "vst3")  // VST 3 has no raw MIDI: notes arrive as note events on MIDI channel 1
      g.gen->noteOn(1, 60, 100 / 127.0f, 0);
    else
      g.gen->midi(0x90, 60, 100, 0);
    runGen(*g.gen, g.st, h, L, R, 4096, &aux);
    const bool firstOnly = aux.size() == 15 && peak(aux[0]) > 0.01f && peak(aux[1]) == 0.f && peak(L) > 0.01f;
    bool second = true;
    if (format != "vst3") {
      g.gen->midi(0x80, 60, 0, 0);
      g.gen->midi(0x91, 64, 100, 0);
      L.clear();
      R.clear();
      aux.clear();
      runGen(*g.gen, g.st, h, L, R, 4096, &aux);
      second = aux.size() == 15 && peak(aux[1]) > 0.01f && peak(aux[2]) == 0.f;
    }
    expect(firstOnly && second, name("MIDI channel k plays rack channel k; inserts reach their outputs").c_str());
    (void)ch1;
  }

  // State round trip: the state captured from a loaded instance loads into a
  // new one, captures the same and renders the same.
  {
    Loaded a = instrument(inst, wrap(pluginState(beat, true, 0, ch0, {channelParam(ch0, ChannelParam::Volume)})));
    std::string d1, d2;
    host::HostedInstance* ha = host::hostedInstance(a.gen.get());
    expect(ha && ha->captureState(d1) && !d1.empty(), name("state captured").c_str());
    Loaded b = instrument(inst, d1);
    host::HostedInstance* hb = host::hostedInstance(b.gen.get());
    expect(hb && hb->captureState(d2) && d1 == d2, name("state round trip: identical").c_str());
    std::string text;
    const std::string payload = d1.substr(d1.find(':') + 1, d1.find(':', d1.find(':') + 1) - d1.find(':') - 1);
    bool decoded = host::base64Decode(payload, text);
#if defined(__APPLE__)
    if (format == "au") text = auStateOf(text);  // ClassInfo "data"
#endif
    Json j;
    expect(decoded && jsonParse(text, j) && j.find("project") && j.find("samples") &&
               text.find("Voice 2") != std::string::npos,
           name("state is the whole project (JSON) with its sample list").c_str());
    Host h1, h2;
    h1.ctx.ppq = h2.ctx.ppq = beat.ppq;
    h1.ctx.playing = h2.ctx.playing = true;
    std::vector<float> L1, R1, L2, R2;
    runGen(*a.gen, a.st, h1, L1, R1, 24000);
    runGen(*b.gen, b.st, h2, L2, R2, 24000);
    expect(peak(L1) > 0.01f && maxDiff(L1, L2) == 0.f, name("restored instance renders identically").c_str());
  }

  // Macro automation: macro 1 drives the volume of channel 1.
  {
    Song one = beat;
    one.patterns[0].steps.clear();
    Loaded g = instrument(inst, wrap(pluginState(one, true, 0, ch0, {channelParam(ch0, ChannelParam::Volume)})));
    host::UiUpdate u;
    host::hostedInstance(g.gen.get())->pumpUi(u);
    Host h;
    std::vector<float> L, R;
    g.st.params[0] = 1.0f;
    g.gen->noteOn(7, 60, 1.0f, 0);
    runGen(*g.gen, g.st, h, L, R, 8192);
    const float loud = peak(L, 2048);
    g.st.params[0] = 0.0f;
    L.clear();
    R.clear();
    runGen(*g.gen, g.st, h, L, R, 8192);
    const float quiet = peak(L, 2048);
    expect(quiet < 1e-4f && loud > 0.01f, name("host automation of a macro moves the linked parameter").c_str());
    const std::string linked = host::hostedInstance(g.gen.get())->paramText(0, 0.5f);
    const std::string free = host::hostedInstance(g.gen.get())->paramText(1, 0.5f);
    std::printf("# macro texts: linked \"%s\", unlinked \"%s\" (quiet %g, loud %g)\n", linked.c_str(), free.c_str(), quiet, loud);
    expect(!linked.empty() && linked != free, name("macro text shows the linked parameter's value").c_str());
  }

  // Render determinism (realtime twice, then offline mode): bit-identical.
  {
    std::vector<float> runs[3];
    for (int k = 0; k < 3; ++k) {
      Loaded g = instrument(inst, wrap(pluginState(beat, true, 0, ch0)));
      if (k == 2) host::hostedInstance(g.gen.get())->backend()->setOffline(true);
      Host h;
      h.ctx.ppq = beat.ppq;
      h.ctx.playing = true;
      h.ctx.offline = k == 2;
      std::vector<float> R;
      runGen(*g.gen, g.st, h, runs[k], R, 48000);
    }
    expect(peak(runs[0]) > 0.01f && runs[0] == runs[1], name("render determinism: two renders are identical").c_str());
    expect(runs[0] == runs[2], name("offline render matches the realtime render").c_str());
  }

  // Effect: input through mixer insert 1 to the output; latency reported.
  {
    auto e = createEffect(fx);
    expect(e != nullptr, name("effect instantiates").c_str());
    if (!e) return;
    PluginState st = defaultPluginState(fx);
    e->prepareHost(st, kSr);
    const int n = 4096;
    std::vector<float> in(n), L(n), R(n);
    for (int i = 0; i < n; ++i) in[static_cast<size_t>(i)] = 0.5f * std::sin(static_cast<float>(i) * 0.03f);
    Host h;
    float err = 0, sig = 0;
    for (int pos = 0; pos < n; pos += kBlock) {
      std::copy(in.begin() + pos, in.begin() + pos + kBlock, L.begin() + pos);
      std::copy(in.begin() + pos, in.begin() + pos + kBlock, R.begin() + pos);
      e->processAux(st, ChannelAux{}, h.ctx, L.data() + pos, R.data() + pos, kBlock);
    }
    for (int i = 0; i < n; ++i) {
      err = std::max(err, std::fabs(L[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]));
      sig = std::max(sig, std::fabs(L[static_cast<size_t>(i)]));
    }
    expect(sig > 0.3f && err < 1e-3f, name("effect: the input passes through insert 1 and the master").c_str());
    expect(e->latencySamples() == 0, name("effect: no latency in the default project").c_str());
    Song lat = makeEmptySong();
    lat.mixer[0].latencyOffset = 300;  // master: manual latency, compensated
    auto e2 = createEffect(fx);
    PluginState st2 = defaultPluginState(fx);
    st2.data = (fxWrap ? fxWrap : wrap)(pluginState(lat, true, 0, kNoId));
    e2->prepareHost(st2, kSr);
    // The state arrives after activation: the plugin asks for a restart (CLAP
    // request_restart, VST 3 restartComponent) and reports the new latency
    // when the host activates it again.
    host::Backend* be = host::hostedInstance(e2.get())->backend();
    be->prepare(kSr / 2, 256);
    be->prepare(kSr, 512);
    expect(be->latency() == 300, name("latency (plugin delay compensation) reported to the host").c_str());
  }
}

#if defined(__APPLE__)
// The Audio Unit's Cocoa view (kAudioUnitProperty_CocoaUI) is Gridline's
// interface: Gridline's AU host opens it in a window that is never shown.
std::string readFile(const std::string& path);
void testAuView() {
  const std::string log = fs::temp_directory_path().append("au-view.log").string();
  setenv("GRIDLINE_PLUGIN_WINDOW_LOG", log.c_str(), 1);
  Loaded g = instrument("au:aumu/GrIn/GrLn");
  host::HostedInstance* hi = g.gen ? host::hostedInstance(g.gen.get()) : nullptr;
  std::unique_ptr<host::Editor> e = hi ? hi->backend()->createEditor() : nullptr;
  void* parent = e ? host::offscreenEditorParent(1280, 800) : nullptr;
  if (e) e->requestResize = [](int, int) { return true; };
  expect(parent && e->attach(parent), "au: Cocoa view attached");
  int w = 0, h = 0;
  expect(e && e->size(w, h) && w == 800 && h == 600 && e->canResize(), "au: the view takes the host's size and resizes");
  for (int k = 0; k < 60; ++k) {
    if (e) e->idle();
    host::pumpOffscreenEvents(0.01);
  }
  e.reset();
  host::closeOffscreenEditorParent();
  g.gen.reset();
  const std::string text = readFile(log);
  std::printf("# au view: %s", text.c_str());
  const size_t f = text.find("auview closed frames=");
  expect(text.find("auview opened size=800x600") != std::string::npos && f != std::string::npos &&
             std::atoi(text.c_str() + f + std::strlen("auview closed frames=")) > 5,
         "au: the view is Gridline's own interface, drawn, closed with the unit's view");
  unsetenv("GRIDLINE_PLUGIN_WINDOW_LOG");
}
#endif

// ---------------------------------------------------------------- MIDI controllers

// Notes plus controllers at sample offsets, through Gridline's host into the
// Gridline instrument: CLAP takes raw MIDI, the VST 3 host turns controllers,
// channel pressure and pitch bend into the parameters the plugin assigned
// with IMidiMapping (the plugin turns them back into MIDI).
enum Ctl { kBend = 1, kPressure = 2, kSustain = 4, kAllCtl = 7 };
std::vector<float> renderMidi(const std::string& inst, std::string (*wrap)(const std::string&), const Song& song, ObjectId ch, int ctl,
                              int bendAt = 37) {
  Loaded g = instrument(inst, wrap(pluginState(song, true, 0, ch)));
  std::vector<float> L, R;
  if (!g.gen) return L;
  Host h;
  for (int b = 0; b < 14; ++b) {
    if (b == 0) g.gen->noteOn(1, 60, 0.8f, 100);
    if (b == 2 && (ctl & kBend)) g.gen->midi(0xE0, 0x00, 0x60, bendAt);  // +1 semitone (range 2)
    if (b == 3 && (ctl & kPressure)) g.gen->midi(0xD0, 90, 0, 211);
    if (b == 4 && (ctl & kSustain)) g.gen->midi(0xB0, 64, 127, 5);
    if (b == 5) g.gen->noteOff(1, 300);
    if (b == 13 && (ctl & kSustain)) g.gen->midi(0xB0, 64, 0, 400);
    if (b == 11 && (ctl & kBend)) g.gen->midi(0xE0, 0x00, 0x40, 3);  // centre
    runGen(*g.gen, g.st, h, L, R, kBlock);
  }
  return L;
}

void testMidiControllers() {
  const std::string clapInst = "clap:org.gridline.gridline", vst3Inst = "vst3:47524944" "4C494E45" "504C5547" "00000001";
  Song one = beatSong(1, 120);
  one.patterns[0].steps.clear();
  const ObjectId ch = one.channels[0].id;
  // The VST 3 instrument assigns every controller of every MIDI channel.
  {
    host::ScanResult r = host::findFormat("vst3")->scanInProcess(kVst3);
    const host::ScannedClass* c = r.classes.empty() ? nullptr : &r.classes[0];
    expect(c && c->params.size() == 16, "vst3 MIDI: the controller parameters are hidden (16 macros listed)");
  }
  const std::vector<float> clapAll = renderMidi(clapInst, clapData, one, ch, kAllCtl);
  const std::vector<float> vst3All = renderMidi(vst3Inst, vst3Data, one, ch, kAllCtl);
  expect(peak(clapAll) > 0.01f && finite(vst3All) && maxDiff(clapAll, vst3All) == 0.f,
         "vst3 MIDI: pitch bend, channel pressure and sustain render exactly as through CLAP");
  // Pitch bend lands on its sample.
  const std::vector<float> vst3NoBend = renderMidi(vst3Inst, vst3Data, one, ch, kAllCtl & ~kBend);
  auto firstDiff = [](const std::vector<float>& a, const std::vector<float>& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
      if (a[i] != b[i]) return i;
    return a.size();
  };
  const size_t first = firstDiff(vst3All, vst3NoBend);
  const size_t later = firstDiff(renderMidi(vst3Inst, vst3Data, one, ch, kAllCtl, 37 + 64), vst3NoBend);
  const size_t at = static_cast<size_t>(2 * kBlock + 37);
  std::printf("# vst3 pitch bend at sample %zu: output changes from sample %zu (64 later: %zu)\n", at, first, later);
  expect(first >= at && first < at + 32 && later == first + 64, "vst3 MIDI: pitch bend is sample-accurate (IMidiMapping parameter point offset)");
  // Sustain (CC 64) holds the note past its note-off.
  const std::vector<float> vst3NoSustain = renderMidi(vst3Inst, vst3Data, one, ch, kAllCtl & ~kSustain);
  const size_t b8 = static_cast<size_t>(12 * kBlock);
  const float held = peak(vst3All, b8, b8 + kBlock), released = peak(vst3NoSustain, b8, b8 + kBlock);
  std::printf("# vst3 sustain: held %g, released %g\n", held, released);
  expect(held > 0.01f && released < held * 0.5f, "vst3 MIDI: sustain pedal (CC 64) holds the note");
}

// `gridline plugin-check` over both bundles, from a hermetic plugin cache.
void testPluginCheck() {
  host::ScanCache cache(fs::path(std::getenv("XDG_CONFIG_HOME")).append("gridline").append("plugin-cache.json").string());
  host::ScanResult c = host::findFormat("clap")->scanInProcess(kClap);
  host::ScanResult v = host::findFormat("vst3")->scanInProcess(kVst3);
  cache.put(c);
  cache.put(v);
  fs::create_directories(fs::path(cache.file()).parent_path());
  expect(cache.save(), "plugin cache written (hermetic config)");
  const std::string report = fs::temp_directory_path().append("plugin-check.json").string();
  const std::string cmd = std::string("\"") + GRIDLINE_CLI_EXE + "\" plugin-check Gridline --json \"" + report + "\" > \"" +
                          fs::temp_directory_path().append("plugin-check.txt").string() + "\" 2>&1";
  const int rc = std::system(cmd.c_str());
  std::string text;
  if (FILE* f = std::fopen(fs::temp_directory_path().append("plugin-check.txt").string().c_str(), "r")) {
    char buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
    std::fclose(f);
  }
  std::printf("# plugin-check output:\n%s", text.c_str());
  Json j;
  std::string rep;
  if (FILE* f = std::fopen(report.c_str(), "r")) {
    char buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) rep.append(buf, got);
    std::fclose(f);
  }
  const bool parsed = jsonParse(rep, j);
  const Json* plugins = parsed ? j.find("plugins") : nullptr;
  expect(rc == 0 && plugins && plugins->array.size() == 4, "gridline plugin-check: 4 classes (CLAP, VST3 x instrument, effect), none fails");
}

// ---------------------------------------------------------------- nested interfaces

#if defined(__APPLE__)
template <class T>
T* probe(const std::string& binary, const char* symbol) {
  void* h = dlopen(binary.c_str(), RTLD_NOW | RTLD_NOLOAD);
  void* f = h ? dlsym(h, symbol) : nullptr;
  return f ? reinterpret_cast<T* (*)()>(f)() : nullptr;
}

std::string readFile(const std::string& path) {
  std::string text;
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    char buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
    std::fclose(f);
  }
  return text;
}

// The line of `log` starting with `event` for `format` ("" when none).
std::string logLine(const std::string& log, const std::string& event, const std::string& format) {
  size_t pos = 0;
  while (pos < log.size()) {
    size_t end = log.find('\n', pos);
    if (end == std::string::npos) end = log.size();
    const std::string line = log.substr(pos, end - pos);
    if (line.rfind(event + " format=" + format + " ", 0) == 0) return line;
    pos = end + 1;
  }
  return {};
}
bool has(const std::string& line, const std::string& what) { return line.find(" " + what) != std::string::npos; }

// Installed plugins hosted inside the Gridline plugin show their own
// interface (never Gridline's parameter panel): the Gridline VST3 instrument,
// loaded by Gridline's host with its editor open in a window that is never
// shown, hosts the in-tree VST3 and CLAP test synths and the LV2 test gain
// (insert 1); each opens its own interface in a window owned by the editor's
// window, with Gridline's control bar under it, and closes with the editor.
void testNestedInterfaces() {
  const std::string log = fs::temp_directory_path().append("plugin-windows.log").string();
  setenv("GRIDLINE_PLUGIN_OPEN_HOSTED", "1", 1);
  setenv("GRIDLINE_PLUGIN_WINDOW_LOG", log.c_str(), 1);
  // The plugin cache the Gridline plugin reads (the application's, hermetic here).
  host::ScanCache cache(fs::path(std::getenv("XDG_CONFIG_HOME")).append("gridline").append("plugin-cache.json").string());
  const host::ScanResult v3 = host::findFormat("vst3")->scanInProcess(GRIDLINE_TESTPLUG_BUNDLE);
  const host::ScanResult cl = host::findFormat("clap")->scanInProcess(GRIDLINE_CLAP_TESTPLUG);
  const host::ScanResult lv = host::findFormat("lv2")->scanInProcess(GRIDLINE_LV2_BUNDLE);
  for (const auto* r : {&v3, &cl, &lv}) {
    cache.put(*r);
    host::registerScanResult(*r);
  }
  fs::create_directories(fs::path(cache.file()).parent_path());
  expect(cache.save(), "nested: plugin cache written");
  // Scanned after the cache exists: the Gridline plugin reads it with its first instance.
  const host::ScanResult gv = host::findFormat("vst3")->scanInProcess(kVst3);
  host::registerScanResult(gv);
  expect(v3.ok && cl.ok && lv.ok && gv.ok, "nested: test plugins and Gridline.vst3 scanned");

  const std::string vst3Synth = "vst3:47524944" "4C494E45" "54535931" "00000001";
  const std::string clapSynth = "clap:org.gridline.test.synth";
  const std::string lv2Gain = "lv2:urn:gridline:lv2test:gain";
  Song s = makeEmptySong();
  Document doc(s);
  const ObjectId c1 = doc.addChannel("VST3 synth", vst3Synth);
  doc.addChannel("CLAP synth", clapSynth);
  doc.setSlotPlugin(doc.song().mixer[1].id, 0, lv2Gain);
  const Song song = doc.song();
  expect(song.channels.size() == 2 && song.mixer[1].slots[0].plugin.type == lv2Gain, "nested: project with three installed plugins");

  Loaded g = instrument("vst3:47524944" "4C494E45" "504C5547" "00000001", vst3Data(pluginState(song, true, 0, c1)));
  host::HostedInstance* hi = g.gen ? host::hostedInstance(g.gen.get()) : nullptr;
  expect(hi && hi->backend()->hasEditor(), "nested: Gridline loads, with its editor");
  if (!hi) return;
  host::Backend* b = hi->backend();
  std::unique_ptr<host::Editor> e = b->createEditor();
  void* parent = e ? host::offscreenEditorParent(1280, 800) : nullptr;
  e->requestResize = [](int, int) { return true; };
  expect(parent && e->attach(parent), "nested: Gridline's editor attached (window never shown)");
  // Gridline's editor draws, opens every hosted plugin's interface, their bars draw.
  std::string text;
  for (int k = 0; k < 400; ++k) {
    e->idle();
    b->idle();
    host::pumpOffscreenEvents(0.01);
    text = readFile(log);
    const char* ev = std::getenv("GRIDLINE_PLUGIN_WINDOW_SHOT") ? "shot" : "drawn";  // dev: wait for the window images
    if (!logLine(text, ev, "VST3").empty() && !logLine(text, ev, "CLAP").empty() && !logLine(text, ev, "LV2").empty()) break;
  }
  std::printf("# nested plugin windows:\n%s", text.c_str());
  struct Expect {
    const char* format;
    const char* view;  // the plugin's own view, attached in the window
  };
  for (const Expect& x : {Expect{"VST3", "view=360x220"}, Expect{"CLAP", "view=320x200"}, Expect{"LV2", "view=320x180"}}) {
    const std::string open = logLine(text, "open", x.format), drawn = logLine(text, "drawn", x.format);
    const std::string tag = std::string("nested ") + x.format + ": ";
    expect(!open.empty() && has(open, "external=0") && has(open, "child=1"), (tag + "its own interface opens in a window owned by the host's window").c_str());
    expect(!drawn.empty() && has(drawn, x.view) && has(drawn, "child=1") && has(drawn, "visible=1"),
           (tag + "the plugin's view is in the window, Gridline's bar draws under it").c_str());
  }
  const std::string vOpen = logLine(text, "open", "VST3");
  const host::layout::WindowLayout L = host::layout::layoutWindow(360, 220, 0);
  expect(has(vOpen, "plugin=360x220") && has(vOpen, "window=" + std::to_string(L.w) + "x" + std::to_string(L.h)) &&
             has(vOpen, "bar=" + std::to_string(L.bar.h)) && L.bar.y == 220,
         "nested VST3: J-29 layout (plugin on top, bar under it)");
  auto* vp = probe<GridlineVst3Probe>(vst3::bundleBinaryPath(GRIDLINE_TESTPLUG_BUNDLE), "gridline_vst3_probe");
  auto* cp = probe<GridlineClapProbe>(std::string(GRIDLINE_CLAP_TESTPLUG) + "/Contents/MacOS/GridlineTest", "gridline_clap_probe");
  expect(vp && cp && vp->views == 1 && cp->views == 1, "nested: the VST3 and CLAP synths attached their own views");
  // Gridline's editor closes: the plugins' windows close with it.
  e.reset();
  host::closeOffscreenEditorParent();
  host::pumpOffscreenEvents(0.05);
  text = readFile(log);
  expect(!logLine(text, "close", "VST3").empty() && !logLine(text, "close", "CLAP").empty() && !logLine(text, "close", "LV2").empty(),
         "nested: the windows close with Gridline's editor");
  expect(vp && cp && vp->views == 0 && cp->views == 0, "nested: the plugins' views are detached");
  g.gen.reset();
  unsetenv("GRIDLINE_PLUGIN_OPEN_HOSTED");
  unsetenv("GRIDLINE_PLUGIN_WINDOW_LOG");
}
#endif

}  // namespace

int main() {
  test::hermeticTestEnv("plugin-build");
#if defined(__APPLE__)
  testNestedInterfaces();
#endif
  testFormat("clap", kClap, "org.gridline.gridline", "org.gridline.gridline-fx", clapData);
  testFormat("vst3", kVst3, "47524944" "4C494E45" "504C5547" "00000001", "47524944" "4C494E45" "504C5547" "00000002", vst3Data);
  testMidiControllers();
#if defined(__APPLE__)
  expect(registerAu(), "au: Gridline.component's units registered in this process (not installed)");
  testFormat("au", GRIDLINE_PLUGIN_AU, "aumu/GrIn/GrLn", "aufx/GrFx/GrLn", auData, auFxData);
  testAuView();
#endif
  testPluginCheck();
  return finish();
}

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
#include "host/HostedPlugins.h"
#include "host/PluginScan.h"

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
                std::string (*wrap)(const std::string&)) {
  const std::string tag = format + ": ";
  auto name = [&](const char* s) { return tag + s; };
  host::ScanResult r = host::findFormat(format)->scanInProcess(bundle);
  expect(r.ok && r.classes.size() == 2, (name("scan finds Gridline and Gridline FX ") + r.error).c_str());
  const host::ScannedClass* ic = findClass(r, instUid);
  const host::ScannedClass* fc = findClass(r, fxUid);
  expect(ic && ic->instrument && ic->eventInput && ic->outputChannels == 2 && ic->auxOutputs.size() == 15 && ic->params.size() == 16,
         name("instrument: notes in, stereo out, 15 extra outputs, 16 macros").c_str());
  expect(ic && ic->name == "Gridline" && ic->vendor == "Gridline", name("instrument name and vendor").c_str());
  expect(fc && !fc->instrument && fc->inputChannels == 2 && fc->sidechainChannels == 2 && fc->outputChannels == 2,
         name("effect: stereo in, sidechain, stereo out").c_str());
  if (!ic || !fc) return;
  host::registerScanResult(r);
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
    Json j;
    expect(host::base64Decode(payload, text) && jsonParse(text, j) && j.find("project") && j.find("samples") &&
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
    st2.data = wrap(pluginState(lat, true, 0, kNoId));
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

}  // namespace

int main() {
  test::hermeticTestEnv("plugin-build");
  testFormat("clap", kClap, "org.gridline.gridline", "org.gridline.gridline-fx", clapData);
  testFormat("vst3", kVst3, "47524944" "4C494E45" "504C5547" "00000001", "47524944" "4C494E45" "504C5547" "00000002", vst3Data);
  testPluginCheck();
  return finish();
}

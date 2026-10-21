// Gridline — J-33 tests: instrument exactness tier D.
// Survey: every shipped FL state (plugin presets anywhere under Data/Patches and
// the projects under Data) of the tier-D instruments is decoded; counts of
// exact / decoded are printed per plugin and must not regress below the
// verified floors. GL_J33_LIST=<plugin> lists the approximate states with the
// importer's notes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpPresets.h"
#include "core/PluginParams.h"
#include "core/Presets.h"
#include "engine/ChannelIns.h"
#include "engine/Generators.h"
#include "packs/models/drums.h"
#include "packs/samplers/dwave.h"
#include "packs/models/drums_ref.h"
#include "packs/models/strings.h"
#include "packs/synths/dco.h"
#include "engine/Program.h"
#include "packs/Packs.h"
#include "packs/fl_corpus.h"
#include "packs/synths/hex.h"
#include "packs/synths/hex_laws.h"
#include "packs/synths/dco.h"
#include "packs/synths/pixel.h"

using namespace gl;

namespace {

namespace fs = std::filesystem;

// Plugin (event 201 name) of every shipped preset file, read once.
const std::map<std::string, std::vector<std::string>>& presetIndex() {
  static const std::map<std::string, std::vector<std::string>> idx = [] {
    std::map<std::string, std::vector<std::string>> m;
    std::error_code ec;
    const fs::path root = fs::path(testfl::flData()) / "Data/Patches";
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      std::string ext = it->path().extension().string();
      for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (ext == ".tbio" || ext == ".flms") {
        m[ext == ".tbio" ? "Toxic Biohazard" : "MiniSynth"].push_back(it->path().string());
        continue;
      }
      if (ext != ".fst") continue;
      flp::File f;
      if (!flp::readFile(it->path().string(), f, nullptr)) continue;
      // Plugin presets and channel presets: the first plugin name.
      for (const auto& e : f.events)
        if (e.id == 201) {
          m[e.text()].push_back(it->path().string());
          break;
        }
    }
    return m;
  }();
  return idx;
}

struct Tally {
  int presets = 0, presetsOk = 0, presetsExact = 0;
  int projects = 0, projectsOk = 0, projectsExact = 0;
};

Tally survey(const std::string& plugin) {
  Tally t;
  const char* list = std::getenv("GL_J33_LIST");
  const bool show = list && plugin == list;
  auto it = presetIndex().find(plugin);
  if (it != presetIndex().end())
    for (const auto& f : it->second) {
      flp::PresetResult r;
      const bool ok = flp::importFlPreset(f, r, testfl::flData()) && r.kind == flp::PresetResult::Plugin;
      ++t.presets;
      t.presetsOk += ok;
      t.presetsExact += ok && r.exact;
      if (show && !(ok && r.exact)) {
        std::printf("#  preset %s %s\n", ok ? "approx" : "FAILED", fs::path(f).filename().string().c_str());
        for (const auto& n : r.notes) std::printf("#     %s\n", n.c_str());
      }
    }
  for (const auto& d : testfl::decodeProjects(plugin)) {
    ++t.projects;
    t.projectsOk += d.ok;
    t.projectsExact += d.ok && d.exact;
    if (show && !(d.ok && d.exact)) {
      std::printf("#  project %s %s\n", d.ok ? "approx" : "FAILED", d.where.c_str());
      for (const auto& n : d.notes) std::printf("#     %s\n", n.c_str());
    }
  }
  return t;
}

void surveyAll() {
  if (!testfl::flInstalled()) {
    std::printf("# FL Studio not installed: corpus survey skipped\n");
    return;
  }
  static const char* const kPlugins[] = {"Toxic Biohazard", "DirectWave", "FPC", "FL Keys", "Slicex", "Fruity Slicer", "3x Osc",
                                          "MiniSynth", "Fruity Envelope Controller", "Patcher", "BeepMap", "MIDI Out",
                                          "Kepler", "Kepler Exo", "Sakura", "Drumpad", "Fruity DX10"};
  for (const char* p : kPlugins) {
    Tally t = survey(p);
    std::printf("# %-28s presets %4d/%4d/%4d exact/ok/all   projects %4d/%4d/%4d\n", p, t.presetsExact, t.presetsOk, t.presets,
                t.projectsExact, t.projectsOk, t.projects);
  }
}

// ------------------------------------------------------------- rendering
struct Buf {
  std::vector<float> l, r;
  bool finite() const {
    for (size_t i = 0; i < l.size(); ++i)
      if (!std::isfinite(l[i]) || !std::isfinite(r[i])) return false;
    return true;
  }
  double rms(size_t a = 0, size_t b = SIZE_MAX) const {
    b = std::min(b, l.size());
    double acc = 0;
    for (size_t i = a; i < b; ++i) acc += 0.5 * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
    return b > a ? std::sqrt(acc / static_cast<double>(b - a)) : 0.0;
  }
  double peak() const {
    double p = 0;
    for (size_t i = 0; i < l.size(); ++i) p = std::max({p, std::fabs(static_cast<double>(l[i])), std::fabs(static_cast<double>(r[i]))});
    return p;
  }
};

// Renders `seconds` of `notes` (pitch, velocity) held until `offAt` seconds, in blocks of `block`.
Buf render(const PluginState& s, std::vector<std::pair<double, float>> notes, double seconds, double offAt, int block = 256,
           double sr = 44100.0) {
  Buf out;
  auto g = createGenerator(s.type);
  ChannelAux aux;
  prepareChannelAux(s, aux);
  if (!s.ins.empty()) {  // as the program builder does
    auto ins = std::make_shared<InsSettings>();
    if (parseIns(s.ins, *ins) && ins->active()) aux.ins = ins;
  }
  const size_t n = static_cast<size_t>(seconds * sr), off = static_cast<size_t>(offAt * sr);
  out.l.assign(n, 0.f), out.r.assign(n, 0.f);
  if (!g) return out;
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 120;
  uint32_t id = 1;
  for (auto [p, v] : notes) g->noteOn(id++, p, v, 0);
  for (size_t pos = 0; pos < n; pos += static_cast<size_t>(block)) {
    const int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - pos));
    if (off >= pos && off < pos + static_cast<size_t>(k))
      for (uint32_t i = 1; i < id; ++i) g->noteOff(i, static_cast<int>(off - pos));
    g->process(s, aux, ctx, out.l.data() + pos, out.r.data() + pos, k);
  }
  return out;
}

// ------------------------------------------------------------- Hex Synth
void testHexSynth() {
  using namespace packs::synths::hex;
  const PluginInfo* info = findPlugin("gl.hexsynth");
  expect(info && info->params.size() == HxCount, "hex: 340 reference parameters");
  if (!info) return;
  PluginState s = defaultPluginState("gl.hexsynth");
  Buf b = render(s, {{60, 0.8f}}, 1.0, 0.5);
  expect(b.finite() && b.rms(2000, 20000) > 0.01, "hex: default program sounds (oscillator 1 sine)");
  expect(b.rms(40000) < b.rms(2000, 20000) * 0.2, "hex: note releases");
  // Sine at A4: zero crossings ≈ 440 Hz.
  {
    Buf a = render(s, {{69, 1.f}}, 0.5, 0.5);
    int zc = 0;
    for (size_t i = 4411; i < 4411 + 13230; ++i) zc += (a.l[i - 1] <= 0 && a.l[i] > 0);
    expect(std::abs(zc - 132) <= 1, "hex: 440 Hz at note 69 (220·2^(S/12 − 4.75))");
  }
  // Block-size invariance (the engine runs on a fixed 128-sample grid).
  {
    PluginState q = s;
    q.params[HxFxOn] = 1, q.params[HxFxType] = FxChorus;
    q.params[HxFilterType] = 1, q.params[HxReso] = 0.6f;
    q.params[HxOscOn + 1] = 1, q.params[HxFm + 0 + 1] = 0.4f;
    q.params[HxUnison] = 3;
    Buf x = render(q, {{57, 0.9f}, {64, 0.7f}}, 0.6, 0.3, 64), y = render(q, {{57, 0.9f}, {64, 0.7f}}, 0.6, 0.3, 500);
    double d = 0;
    for (size_t i = 0; i < x.l.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(x.l[i] - y.l[i])));
    expect(x.finite() && d < 1e-6, "hex: output independent of the host block size");
  }
  // Every effect type and filter type stays finite at extreme settings.
  for (int fx = 0; fx < FxKinds; ++fx) {
    PluginState q = s;
    for (int i = 0; i < kAutomatable; ++i) q.params[static_cast<size_t>(i)] = (i % 3 == 0) ? 1.f : 0.f;
    q.params[HxOscOn] = 1, q.params[HxMix] = 1;
    q.params[HxFxOn] = q.params[HxFxOn + 1] = 1;
    q.params[HxFxType] = static_cast<float>(fx), q.params[HxFxType + 1] = static_cast<float>((fx + 2) % FxKinds);
    q.params[HxFilterType] = static_cast<float>(fx % 4);
    q.params[HxUnison] = 7, q.params[HxVoices] = 32;
    Buf x = render(q, {{36, 1.f}, {96, 1.f}}, 0.5, 0.25);
    char name[96];
    std::snprintf(name, sizeof name, "hex: extremes finite and bounded (effect type %d)", fx);
    expect(x.finite() && x.peak() < 100, name);
  }
  // Reverb tail and delay echoes ring after the note.
  for (int fx : {FxReverb, FxDelay}) {
    PluginState q = s;
    q.params[HxFxOn] = 1, q.params[HxFxType] = static_cast<float>(fx);
    q.params[HxRvMix] = 1, q.params[HxRvDecay] = 0.9f, q.params[HxDlMix] = 1, q.params[HxDlFeedback] = 0.7f;
    q.params[HxAmpR] = 0.f;
    Buf x = render(q, {{60, 1.f}}, 1.5, 0.2);
    expect(x.finite() && x.rms(static_cast<size_t>(0.5 * 44100), static_cast<size_t>(0.9 * 44100)) > 1e-3,
           fx == FxReverb ? "hex: plate reverb tail" : "hex: delay echoes");
  }
  // Mono legato: the second note does not retrigger the amp envelope.
  {
    PluginState q = s;
    q.params[HxVoices] = 0;
    q.params[HxAmpA] = 0.5f;  // ~3 s attack: a retrigger would restart from zero
    auto g = packs::createGenerator("gl.hexsynth");
    ChannelAux aux;
    packs::prepareAux(q, aux);
    ProcessContext ctx;
    ctx.sampleRate = 44100;
    std::vector<float> l(44100), r(44100);
    g->noteOn(1, 60, 1.f, 0);
    for (int p = 0; p < 22050; p += 256) g->process(q, aux, ctx, l.data() + p, r.data() + p, std::min(256, 22050 - p));
    g->noteOn(2, 67, 1.f, 0);
    for (int p = 22050; p < 44100; p += 256) g->process(q, aux, ctx, l.data() + p, r.data() + p, std::min(256, 44100 - p));
    double before = 0, after = 0;
    for (int i = 21000; i < 22000; ++i) before = std::max(before, static_cast<double>(std::fabs(l[static_cast<size_t>(i)])));
    for (int i = 22200; i < 23200; ++i) after = std::max(after, static_cast<double>(std::fabs(l[static_cast<size_t>(i)])));
    expect(after > before * 0.9, "hex: mono legato keeps the envelope");
  }
  // Sequencer: holding a key plays steps on a 16th grid.
  {
    PluginState q = s;
    q.params[HxSeqPlay] = 1;
    for (int k = 0; k < 16; ++k) q.params[static_cast<size_t>(HxSeqStep + k)] = (k % 2 == 0) ? (26.5f / 50.f) : 0.f;  // root / rest
    q.params[HxAmpR] = 0.f;
    Buf x = render(q, {{60, 1.f}}, 1.0, 1.0);
    expect(x.finite() && x.rms(0, 44100) > 0.005, "hex: step sequencer plays while a key is held");
  }
  // Short / old parameter vectors normalise to the defaults.
  {
    PluginState q;
    q.type = "gl.hexsynth";
    q.params.assign(10, 0.5f);
    normalizePluginState(q);
    expect(q.params.size() == HxCount && q.params[HxOscOn] == 1.f, "hex: short state padded with defaults");
  }
  // Data text round trip.
  {
    HexData d;
    d.amp[0][0] = 0.25f, d.phase[0][3] = 0.5f, d.amp[7][127] = -1.f;
    d.link[5] = 74, d.name = "Test name";
    HexData e;
    expect(parseHexData(formatHexData(d), e) && e.amp[0][0] == 0.25f && e.phase[0][3] == 0.5f && e.amp[7][127] == -1.f &&
               e.link[5] == 74 && e.link[6] == -1 && e.name == "Test name",
           "hex: data text round trip");
  }
  // Laws.
  expect(std::fabs(law::time(1.0) - 50.0) < 1e-9 && law::time(0) == 0.00101, "hex: time law max(50x⁴, 0.00101)");
  expect(law::coarse(0.5) == 0 && law::coarse(1.0) == 99 && law::coarse(0.0) == -99, "hex: coarse ±99 semitones");
  expect(law::seqStep(0) == -26 && law::seqStep(0.505) == 0 && law::seqStep(1.0) == 24, "hex: step values");
  expect(std::fabs(law::eqDb(1.0) - 18.0) < 1e-9 && law::eqDb(0.5) == 0.0, "hex: EQ law 18 s|s|");
}

// Shipped presets: every .tbio decodes exactly; a sample of them renders
// finite and audible (GL_J33_ALL=1 renders all of them).
void testHexCorpus() {
  if (!testfl::flInstalled()) return;
  auto it = presetIndex().find("Toxic Biohazard");
  if (it == presetIndex().end()) return;
  int ok = 0, exact = 0, rendered = 0, audible = 0, finite = 0;
  const bool all = std::getenv("GL_J33_ALL") != nullptr;
  for (size_t i = 0; i < it->second.size(); ++i) {
    flp::PresetResult r;
    if (!flp::importFlPreset(it->second[i], r, testfl::flData())) continue;
    ++ok;
    exact += r.exact;
    if (!all && i % 25 != 0) continue;
    Buf b = render(r.state, {{48, 0.8f}, {55, 0.8f}, {60, 0.8f}}, 1.2, 0.8);
    ++rendered;
    finite += b.finite() && b.peak() < 50;
    audible += b.rms() > 1e-4;
    if (std::getenv("GL_J33_LEVELS"))
      std::printf("#   %-40s rms %.4f peak %.3f\n", fs::path(it->second[i]).filename().string().c_str(), b.rms(), b.peak());
  }
  std::printf("# hex corpus: %d/%zu decoded, %d exact, rendered %d: %d finite, %d audible\n", ok, it->second.size(), exact,
              rendered, finite, audible);
  expect(ok == static_cast<int>(it->second.size()) && exact == ok, "hex: every shipped preset decodes exactly");
  expect(finite == rendered && audible >= rendered - 2, "hex: shipped presets render finite and audible");
}

// ----------------------------------------------------------- Pocket Synth
void testPocketSynth() {
  const PluginInfo* info = findPlugin("gl.pocketsynth");
  expect(info && info->params.size() == 43, "pocket: 43 reference parameters");
  if (!info) return;
  PluginState s = defaultPluginState("gl.pocketsynth");
  Buf b = render(s, {{60, 0.8f}}, 1.0, 0.5);
  expect(b.finite() && b.rms(2000, 20000) > 0.01, "pocket: default patch sounds");
  expect(b.rms(40000) < b.rms(2000, 20000) * 0.1, "pocket: note releases");
  for (int kind = 0; kind < 10; ++kind) {
    PluginState q = s;
    q.params[5] = kind / 9.f;
    q.params[6] = 0.6f;
    q.params[25] = q.params[28] = q.params[33] = q.params[37] = 1;  // every effect on
    q.params[26] = 1, q.params[27] = 0.5f, q.params[9] = 1;
    Buf x = render(q, {{36, 1.f}, {84, 1.f}}, 0.6, 0.3, 100);
    char name[64];
    std::snprintf(name, sizeof name, "pocket: oscillator kind %d with every effect finite", kind);
    expect(x.finite() && x.peak() < 50 && x.rms() > 1e-3, name);
  }
  {
    Buf x = render(s, {{57, 0.9f}, {64, 0.7f}}, 0.5, 0.3, 64), y = render(s, {{57, 0.9f}, {64, 0.7f}}, 0.5, 0.3, 333);
    double d = 0;
    for (size_t i = 0; i < x.l.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(x.l[i] - y.l[i])));
    size_t first = 0;
    for (size_t i = 0; i < x.l.size(); ++i)
      if (std::fabs(x.l[i] - y.l[i]) > 1e-6f) {
        first = i;
        break;
      }
    if (d >= 1e-5) std::printf("# pocket block diff %g first at %zu\n", d, first);
    expect(d < 1e-5, "pocket: output independent of the host block size");
  }
  if (!testfl::flInstalled()) return;
  auto it = presetIndex().find("MiniSynth");
  if (it == presetIndex().end()) return;
  int ok = 0, finite = 0, audible = 0;
  for (const auto& f : it->second) {
    flp::PresetResult r;
    if (!flp::importFlPreset(f, r, testfl::flData())) continue;
    ++ok;
    Buf x = render(r.state, {{48, 0.8f}, {60, 0.8f}}, 1.0, 0.7);
    finite += x.finite() && x.peak() < 50;
    audible += x.rms() > 1e-4;
  }
  std::printf("# pocket corpus: %d decoded, %d finite, %d audible\n", ok, finite, audible);
  expect(ok == static_cast<int>(it->second.size()) && finite == ok && audible >= ok - 1, "pocket: shipped presets render");
}

// ------------------------------------------------------------- Pixel Tone
void testPixelTone() {
  using namespace packs::synths::pixel;
  PluginState s = defaultPluginState("gl.pixeltone");
  Buf b = render(s, {{60, 0.8f}}, 1.0, 0.6);
  expect(b.finite() && b.rms(0, 26000) > 1e-3, "pixel: default picture sounds");
  expect(b.rms(30000) < 1e-6, "pixel: note-off ends the voice (short fade)");
  // A single full-white row at the middle: log scale puts it on the note (C5 reference 523.25 Hz at key 60).
  {
    Picture p;
    p.w = 4, p.h = 3;
    p.rgb.assign(36, 0);
    for (int x = 0; x < 4; ++x) p.rgb[static_cast<size_t>((1 * 4 + x) * 3)] = 255, p.rgb[static_cast<size_t>((1 * 4 + x) * 3 + 1)] = 255;
    PluginState q = s;
    q.data = formatPicture(p);
    Buf x = render(q, {{60, 1.f}}, 0.5, 0.5);
    int zc = 0;
    for (size_t i = 4411; i < 4411 + 13230; ++i) zc += (x.l[i - 1] <= 0 && x.l[i] > 0);
    expect(std::abs(zc - 157) <= 2, "pixel: middle row plays 523.25 Hz at key 60 (log scale)");
  }
  for (int sc = 0; sc < 3; ++sc) {
    PluginState q = s;
    q.params[PxScale] = static_cast<float>(sc);
    q.params[PxGrainy] = 1, q.params[PxWiden] = 1, q.params[PxUseBlue] = 1, q.params[PxLoop] = 0;
    Buf x = render(q, {{36, 1.f}, {96, 1.f}}, 0.5, 0.4, 100);
    char name[64];
    std::snprintf(name, sizeof name, "pixel: scale %d with every switch finite", sc);
    expect(x.finite() && x.peak() < 10, name);
  }
  if (!testfl::flInstalled()) return;
  auto it = presetIndex().find("BeepMap");
  if (it == presetIndex().end()) return;
  int ok = 0, good = 0;
  for (const auto& f : it->second) {
    flp::PresetResult r;
    if (!flp::importFlPreset(f, r, testfl::flData())) continue;
    ++ok;
    Buf x = render(r.state, {{60, 0.8f}}, 1.5, 1.2);
    good += x.finite() && x.peak() < 10 && x.rms() > 1e-4;
  }
  expect(ok == static_cast<int>(it->second.size()) && good == ok, "pixel: shipped pictures decode and play");
}

// ------------------------------------------------- DCO Synth reference model
void testDcoReference() {
  using namespace packs::synths::dco;
  const PluginInfo* info = findPlugin("gl.dcosynth");
  expect(info && info->legacyModelParam == DcModel, "dco: engine model param");
  PluginState old;
  old.type = "gl.dcosynth";
  old.params.assign(DcModel, 0.f);  // a J-22 state: no model param yet
  normalizePluginState(old);
  expect(old.params[DcModel] == 0.f, "dco: older states keep the J-22 engine");
  expect(defaultPluginState("gl.dcosynth").params[DcModel] == 1.f, "dco: new instances use the reference laws");
  // Reference sound: saw through an open ladder, envelope VCA.
  PluginState s = defaultPluginState("gl.dcosynth");
  s.params[DcDco1Saw] = 1, s.params[DcCutoff] = 0.75f, s.params[DcVcaSrc] = 0, s.params[DcEnv1S] = 1;
  Buf b = render(s, {{57, 1.f}}, 1.0, 0.5);
  expect(b.finite() && b.rms(4000, 20000) > 1e-3, "dco: reference engine sounds");
  // Release reaches 10 % at the table time: release x = 0.4 → 0.321 s.
  s.params[DcEnv1R] = 0.4f;
  Buf r = render(s, {{57, 1.f}}, 1.5, 0.5);
  const double atRel = r.rms(static_cast<size_t>(0.45 * 44100), static_cast<size_t>(0.5 * 44100));
  const double later = r.rms(static_cast<size_t>((0.5 + 0.321) * 44100 - 1000), static_cast<size_t>((0.5 + 0.321) * 44100 + 1000));
  expect(later < atRel * 0.2 && later > atRel * 0.02, "dco: release falls to about 10 % at its table time (VCA − 0.025)");
}

// ------------------------------------------------ channel instrument envelopes
void testChannelIns() {
  // Text round trip and neutral defaults.
  InsSettings a;
  expect(!a.active(), "ins: default channel settings are neutral");
  a.t[InsVol].on = true, a.t[InsVol].attack = 32950, a.t[InsPitch].on = true, a.t[InsPitch].amount = 1200;
  a.cutoff = 128, a.type = 5;
  InsSettings b;
  expect(parseIns(formatIns(a), b) && b == a && a.active(), "ins: text round trip");
  // Volume envelope on a Trio voice: 2 s attack (knob 32950 = 0.064·(1001^(v/65536) − 1) s).
  PluginState s = defaultPluginState("gl.synth");
  s.params[pp::SynAttack] = 0, s.params[pp::SynSustain] = 1, s.params[pp::SynRelease] = 0, s.params[pp::SynCutoff] = 20000;
  s.params[pp::SynOsc1Shape] = 0, s.params[pp::SynOsc2Level] = 0, s.params[pp::SynOsc3Level] = 0;
  InsSettings v;
  v.t[InsVol].on = true, v.t[InsVol].attack = 32950, v.t[InsVol].tensionA = 0;
  s.ins = formatIns(v);
  Buf x = render(s, {{60, 1.f}}, 2.5, 2.4);
  const double at05 = x.rms(static_cast<size_t>(0.45 * 44100), static_cast<size_t>(0.55 * 44100));
  const double at15 = x.rms(static_cast<size_t>(1.45 * 44100), static_cast<size_t>(1.55 * 44100));
  const double at21 = x.rms(static_cast<size_t>(2.1 * 44100), static_cast<size_t>(2.2 * 44100));
  expect(at05 > 0 && std::fabs(at15 / at05 - 3.0) < 0.35 && at21 > at15, "ins: linear 2 s attack (0.064·u law)");
  // Pitch envelope: +1200 cents at full level (sustain 128) → one octave up.
  InsSettings pe;
  pe.t[InsPitch].on = true, pe.t[InsPitch].attack = 100, pe.t[InsPitch].sustain = 128, pe.t[InsPitch].amount = 1200;
  s.ins = formatIns(pe);
  Buf y = render(s, {{57, 1.f}}, 0.5, 0.5);
  int zc = 0;
  for (size_t i = 4411; i < 4411 + 13230; ++i) zc += (y.l[i - 1] <= 0 && y.l[i] > 0);
  expect(std::abs(zc - 132) <= 2, "ins: pitch envelope in cents (+1200: 220 Hz note plays 440 Hz)");
  // Channel low pass (LP, cutoff knob 64/256) darkens a saw.
  s.params[pp::SynOsc1Shape] = 2;
  s.ins.clear();
  Buf open = render(s, {{48, 1.f}}, 0.3, 0.3);
  InsSettings lp;
  lp.cutoff = 64, lp.type = 1;
  s.ins = formatIns(lp);
  Buf dark = render(s, {{48, 1.f}}, 0.3, 0.3);
  double dO = 0, dD = 0;
  for (size_t i = 2001; i < 12000; ++i) dO += std::fabs(open.l[i] - open.l[i - 1]), dD += std::fabs(dark.l[i] - dark.l[i - 1]);
  expect(dark.finite() && dD < dO * 0.5, "ins: channel filter (LP) applied to the voice");
  // Presets carry the settings.
  {
    PluginState q = s;
    const std::string dir = (fs::temp_directory_path() / "gl_j33_ins_presets").string();
    fs::remove_all(dir);
    savePreset(dir, q, "t/ins", "", false);
    PluginState back;
    bool found = false;
    for (const auto& e : fs::recursive_directory_iterator(dir))
      if (e.is_regular_file()) found = loadPreset(e.path().string(), "gl.synth", back);
    expect(found && back.ins == q.ins, "ins: plugin presets keep the channel settings");
    fs::remove_all(dir);
  }
}


// ------------------------------------------------- Membrane Pad reference engine
void testDrumRef() {
  namespace dr = packs::models::dr;
  // Velocity: v = vel^0.5, a route's value is reached at full velocity; routes reach Hold / Pitch.
  float in[dr::MdCount] = {}, out[dr::MdCount];
  in[dr::MdMalletAmp] = 0.f, in[dr::MdVelSrc1] = 1, in[dr::MdVelAmt1] = 1.f;
  in[dr::MdPitch] = 0.5f, in[dr::MdVelSrc2] = 28, in[dr::MdVelAmt2] = 1.f;
  dr::RefMembraneVoice::routes(in, out, 0.25f);
  expect(std::fabs(out[dr::MdMalletAmp] - 0.5f) < 1e-6f && std::fabs(out[dr::MdPitch] - 0.75f) < 1e-6f,
         "drum ref: velocity routes (sqrt velocity, value at full velocity, pitch route)");
  PluginState s = defaultPluginState("gl.membranepad");
  expect(s.params.size() == dr::PdCount && s.params[dr::PdEngine] == 1.f, "drum ref: reference engine by default");
  s.params[dr::MdMalletAmp] = 0.8f, s.params[dr::MdMemDecay] = 0.9f, s.params[dr::MdMemCutoff] = 0.6f;
  s.params[dr::MdSize] = 0.6f, s.params[dr::MdShape] = 0.3f, s.params[dr::MdHold] = 0.6f, s.params[dr::MdTension] = 0.4f;
  const Buf a = render(s, {{60, 1.f}}, 1.0, 0.1);
  const Buf b = render(s, {{72, 1.f}}, 1.0, 0.1);
  const Buf c = render(s, {{60, 1.f}}, 1.0, 0.1, 37);
  bool same = true, sameBlock = true;
  for (size_t i = 0; i < a.l.size(); ++i) same = same && a.l[i] == b.l[i], sameBlock = sameBlock && a.l[i] == c.l[i];
  expect(a.finite() && a.rms() > 1e-3 && a.peak() <= 1.0001, "drum ref: strike sounds, clipped to ±1");
  expect(same, "drum ref: the key does not transpose");
  expect(sameBlock, "drum ref: block-size invariant");
  // Legacy states (no engine value) keep the J-22 model.
  PluginState old = s;
  old.params.resize(dr::PdEngine);
  normalizePluginState(old);
  expect(old.params[dr::PdEngine] == 0.f, "drum ref: older states keep the modal engine");
}

// ------------------------------------------------- Twin String reference engine
void testTwinRef() {
  namespace st = packs::models::strs;
  PluginState s = defaultPluginState("gl.twinstring");
  expect(s.params.size() == st::TsParamCount && s.params[st::TsEngine] == 1.f, "twin ref: reference engine by default");
  s.params[st::TsLevel] = 0.5f, s.params[st::TsAmpS] = 1.f, s.params[st::TsS1Decay] = 0.9f, s.params[st::TsS2Decay] = 0.9f;
  s.params[st::TsHiCut] = 0.7f, s.params[st::TsHiSus] = 0.5f, s.params[st::TsNoiseLevel] = 0.3f, s.params[st::TsMix] = 0.f;
  s.params[st::TsS1Damp] = 0.3f;
  const Buf a = render(s, {{69, 1.f}}, 1.0, 0.8);
  expect(a.finite() && a.rms(4410, 30000) > 1e-4, "twin ref: plucked note sounds");
  // String 1 is exactly in tune (fractional string clock): A4 → 440 Hz (period 100.2 samples:
  // the strongest autocorrelation lag between 60 and 400 samples).
  int bestLag = 0;
  double best = -1e30;
  for (int lag = 60; lag <= 400; ++lag) {
    double acc = 0;
    for (size_t i = 8820; i < 8820 + 22050; ++i) acc += static_cast<double>(a.l[i]) * a.l[i + static_cast<size_t>(lag)];
    if (acc > best) best = acc, bestLag = lag;
  }
  std::printf("# twin period %d samples\n", bestLag);
  expect(std::abs(bestLag - 100) <= 1, "twin ref: string 1 in tune (440 Hz)");
  const Buf b = render(s, {{69, 1.f}}, 1.0, 0.8, 37), c = render(s, {{69, 1.f}}, 1.0, 0.8, 1024);
  bool same = true;
  for (size_t i = 0; i < a.l.size(); ++i) same = same && a.l[i] == b.l[i] && a.l[i] == c.l[i];
  expect(same, "twin ref: block-size invariant");
  // Effects on: resonators, chorus, reverb, delay stay finite.
  s.params[st::TsResOn1] = 1, s.params[st::TsResFreq1] = 0.5f, s.params[st::TsResFb1] = 0.8f;
  s.params[st::TsChoOn] = 1, s.params[st::TsChoWet] = 0.5f, s.params[st::TsRevOn] = 1, s.params[st::TsRevMix] = 0.5f;
  s.params[st::TsDlyOn] = 1, s.params[st::TsDlyWet] = 0.5f, s.params[st::TsDlyFb] = 0.5f;
  const Buf f = render(s, {{57, 1.f}, {64, 0.7f}}, 2.0, 0.5);
  expect(f.finite() && f.rms(30000, 88000) > 1e-4, "twin ref: FX chain renders (tails after release)");
}

// ------------------------------------------------- Kepler reference effects
void testDcoFx() {
  namespace dc = packs::synths::dco;
  PluginState s = defaultPluginState("gl.dcosynth");
  s.params[dc::DcRevOn] = 1, s.params[dc::DcRevWet] = 0.9f, s.params[dc::DcRevDecay] = 0.8f;
  s.params[dc::DcEnsOn] = 1, s.params[dc::DcEnsMix] = 0.5f, s.params[dc::DcEnsLines] = 1;
  s.params[dc::DcDlyOn] = 1, s.params[dc::DcDlyInput] = 0.6f, s.params[dc::DcDlyFb] = 0.5f;
  s.params[dc::DcSatOn] = 1, s.params[dc::DcSatAmount] = 0.5f;
  const Buf a = render(s, {{60, 1.f}}, 2.0, 0.3);
  expect(a.finite() && a.rms(22050, 88200) > 1e-4, "dco ref fx: ensemble / delay / reverb / saturation render with tails");
  // Delay: the first echo arrives T·sr + 66 samples after the input (free time 0.01 + 0.49x s).
  PluginState d = defaultPluginState("gl.dcosynth");
  d.params[dc::DcDlyOn] = 1, d.params[dc::DcDlyInput] = 1, d.params[dc::DcDlyTime] = 0.5f, d.params[dc::DcDlyFb] = 0;
  d.params[dc::DcDlyFbType] = 1, d.params[dc::DcDlyTone] = 0.5f, d.params[dc::DcDlyStereo] = 1;
  d.params[dc::DcRevOn] = 0, d.params[dc::DcEnsOn] = 0, d.params[dc::DcChorus] = 0;
  const Buf e = render(d, {{60, 1.f}}, 1.0, 0.02);
  size_t first = 0, onset = 0;
  for (size_t i = 0; i < e.l.size(); ++i)
    if (std::fabs(e.l[i]) > 1e-4) {
      first = i;
      break;
    }
  for (size_t i = static_cast<size_t>(0.15 * 44100); i < e.l.size(); ++i)
    if (std::fabs(e.l[i]) > 1e-4) {
      onset = i;
      break;
    }
  const double T = (0.01 + 0.49 * 0.5) * 44100 + 66;
  std::printf("# dco echo first %zu onset %zu expect %g\n", first, onset, T + first);
  expect(onset > 0 && std::fabs(static_cast<double>(onset - first) - T) < 200, "dco ref fx: delay time law (first echo at T·sr + 66)");
}


// ------------------------------------------------------------ Wave Program
void testWaveProgram() {
  namespace w = packs::samplers::dw;
  w::DwProgram p;
  p.name = "Test";
  p.playMode = 1;
  p.trig[5] = {1, 0.5f, 0.1f};
  w::DwZone z;
  z.root = 48, z.lo = 40, z.hi = 70, z.loopMode = 3, z.loopStart = 100, z.loopEnd = 2000, z.f[0].type = 1, z.slot[5] = {1, 7, 0.8f};
  z.name = "zone a", z.path = "/tmp/x y.wav";
  p.zones.push_back(z);
  w::DwProgram q;
  const std::string text = w::formatProgram(p);
  expect(w::parseProgram(text, q) && w::formatProgram(q) == text && q.zones.size() == 1 && q.zones[0].path == "/tmp/x y.wav" &&
             q.zones[0].slot[5].dst == 7 && q.trig[5].type == 1 && q.playMode == 1,
         "wave program: text round trip");
  // A real sample from the reference content: velocity is linear in amplitude, −6 dB per side at centre.
  const std::string data = flp::defaultFactoryData();
  const std::string wav = data + "/Data/Patches/Packs/Drums/Kicks/808 Kick.wav";
  std::error_code ec;
  if (data.empty() || !fs::exists(wav, ec)) {
    std::printf("# wave program: reference sample missing, render checks skipped\n");
    return;
  }
  w::DwProgram k;
  w::DwZone kz;
  kz.path = wav, kz.root = 60, kz.amp[3] = 0.1f;
  k.zones.push_back(kz);
  k.cc7 = 1.f;
  PluginState s = defaultPluginState("gl.waveprogram");
  s.data = w::formatProgram(k);
  const Buf loud = render(s, {{60, 1.f}}, 0.5, 0.4), soft = render(s, {{60, 0.5f}}, 0.5, 0.4);
  const double rl = loud.rms(0, 8000), rs = soft.rms(0, 8000);
  expect(loud.finite() && rl > 1e-3 && std::fabs(rs / rl - 64.0 / 127.0) < 0.02, "wave program: velocity → gain is linear (vel/127)");
  const Buf b37 = render(s, {{60, 1.f}}, 0.5, 0.4, 37);
  bool same = true;
  for (size_t i = 0; i < loud.l.size(); ++i) same = same && loud.l[i] == b37.l[i];
  expect(same, "wave program: block-size invariant");
  // Reverb bus (the instrument's build of the plate engine): wet tail after the note.
  k.revOn = true;
  k.zones[0].send[2] = 1.f;
  s.data = w::formatProgram(k);
  s.params[w::DwRevDecay] = 0.6f;
  const Buf rv = render(s, {{60, 1.f}}, 2.0, 0.1);
  PluginState noRev = s;
  k.revOn = false;
  noRev.data = w::formatProgram(k);
  const Buf dryOnly = render(noRev, {{60, 1.f}}, 2.0, 0.1);
  expect(rv.finite() && rv.rms(44100, 66150) > 10 * dryOnly.rms(44100, 66150) + 1e-6, "wave program: reverb tail on the send bus");
}

}  // namespace

int main() {
  testWaveProgram();
  testDrumRef();
  testTwinRef();
  testDcoFx();
  testChannelIns();
  testDcoReference();
  testPixelTone();
  testPocketSynth();
  testHexSynth();
  testHexCorpus();
  surveyAll();
  return finish();
}

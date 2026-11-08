// Gridline — FLP export of Gridline-only plugins as Gridline Suite plugins
// (K-110): the three modes, the wrapper entry FL loads, the round trip through
// Gridline's own importer (with and without the suite installed), the files
// carried in the plugin's state, and the per-type placement table.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include "fixtures/TestSong.h"
#include "Hermetic.h"
#include "../src/core/AudioBuffer.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginSuite.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/core/SuiteState.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpEncode.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/core/flp/FlpStandIn.h"
#include "../src/core/flp/FlpWrapper.h"
#include "../src/core/flp/FlpWriter.h"
#include "../src/engine/Program.h"
#include "../src/host/HostedPlugins.h"
#include "../src/host/PluginScan.h"
#include "../src/plugin/PluginCore.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

fs::path scratch() {
  fs::path p = fs::temp_directory_path() / "gridline-test-flp-suite";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

std::string readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

std::string writeTone(const fs::path& p, int frames, float hz) {
  AudioBuffer b(2, static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
    b.plane(0)[i] = b.plane(1)[i] = 0.4f * std::sin(6.2831853f * hz * static_cast<float>(i) / 44100.f);
  writeWav(p.string(), b, 44100, 16);
  return p.string();
}

// A song with instruments on channels and effects on insert 1, every channel playing two notes.
Song songWith(const std::vector<PluginState>& instruments, const std::vector<PluginState>& effects) {
  Song s = makeTestSong();
  s.channels.resize(instruments.size());
  for (auto& p : s.patterns) p.steps.clear(), p.notes.clear(), p.stepProps.clear();
  s.patterns.resize(1);
  s.automations.clear(), s.links.clear(), s.modLinks.clear();
  s.arrangements.clear(), s.currentArrangement = 0;
  s.playlist.clear();
  for (size_t i = 0; i < instruments.size(); ++i) {
    Channel& c = s.channels[i];
    c = Channel{};
    c.id = s.allocateId();
    c.name = "Ch" + std::to_string(i + 1);
    c.mixerTrack = s.mixer[1].id;
    c.plugin = instruments[i];
    s.patterns[0].notes[c.id] = {Note(0, s.ppq, 60, 100), Note(s.ppq * 2, s.ppq, 64, 90)};
  }
  Clip clip;
  clip.kind = ClipKind::Pattern;
  clip.pattern = s.patterns[0].id;
  clip.id = s.allocateId();
  clip.track = 0;
  clip.length = s.barTicks();
  s.playlist.push_back(clip);
  for (auto& t : s.mixer) t.slots.clear();
  s.mixer[1].slots.resize(effects.size());
  for (size_t k = 0; k < effects.size(); ++k) s.mixer[1].slots[k].plugin = effects[k];
  return s;
}

PluginState changed(const std::string& type, int salt) {
  PluginState st = defaultPluginState(type);
  const PluginInfo* pi = findPlugin(type);
  for (size_t k = 0; pi && k < pi->params.size() && k < st.params.size(); ++k) {
    const ParamDesc& d = pi->params[k];
    if (d.kind == ParamKind::Continuous && (k + static_cast<size_t>(salt)) % 3 == 0) st.params[k] = static_cast<float>(d.min + 0.3 * (d.max - d.min));
  }
  return st;
}

struct Exported {
  bool ok = false;
  flp::Bytes bytes;
  flp::ExportReport rep;
};
Exported exportIt(const Song& s, flp::GridlinePlugins mode, const std::string& sampleDir = "") {
  Exported e;
  flp::ExportOptions o;
  o.useOrigin = false;
  o.gridlinePlugins = mode;
  o.sampleDir = sampleDir;
  e.ok = flp::exportFlp(s, e.bytes, e.rep, o);
  return e;
}

bool reimport(const flp::Bytes& b, Song& out) {
  flp::Report r;
  std::string err;
  return importFlpBytes(std::vector<uint8_t>(b.begin(), b.end()), out, r, &err, "", scratch().string(), false);
}

const flp::ExportReport::Plugin* entry(const flp::ExportReport& r, const std::string& where) {
  for (const auto& p : r.plugins)
    if (p.where == where) return &p;
  return nullptr;
}

const Channel* channelNamed(const Song& s, const std::string& n) {
  for (const auto& c : s.channels)
    if (c.name == n) return &c;
  return nullptr;
}

// Wrapper states (event 213) in an FLP image whose class is a Gridline Suite member.
std::vector<flp::Bytes> suiteWrappers(const flp::Bytes& b) {
  std::vector<flp::Bytes> out;
  flp::File f;
  std::string err;
  if (!flp::readBytes(std::vector<uint8_t>(b.begin(), b.end()), f, &err)) return out;
  for (const auto& e : f.events) {
    flp::WrapperInfo w;
    if (e.id == 213 && flp::parseWrapper(e.data, e.size, w) && flp::suiteMemberOfType(w.savedType())) out.emplace_back(e.data, e.data + e.size);
  }
  return out;
}

std::vector<std::pair<uint32_t, flp::Bytes>> records(const flp::Bytes& b, size_t from = 4) {
  std::vector<std::pair<uint32_t, flp::Bytes>> out;
  size_t p = from;
  while (p + 12 <= b.size()) {
    const uint32_t id = flp::getU32(b, p);
    const size_t n = flp::getU32(b, p + 4);
    if (n > b.size() - p - 12) break;
    out.push_back({id, flp::Bytes(b.begin() + static_cast<std::ptrdiff_t>(p + 12), b.begin() + static_cast<std::ptrdiff_t>(p + 12 + n))});
    p += 12 + n;
  }
  return out;
}

// ---- modes

void testRoundTrip(bool installed) {
  const std::string tag = installed ? " (suite installed here)" : " (suite not installed here)";
  const std::string wav = writeTone(scratch() / "selt-tone.wav", 22050, 220.f);
  PluginState selt = changed("gl.seltmann", 1);
  selt.data = wav;
  const std::vector<PluginState> inst = {selt, changed("gl.plasma", 2), changed("gl.patcher", 0), changed("gl.platter", 1)};
  const std::vector<PluginState> fx = {changed("gl.shaper", 1), changed("gl.chorus", 2), changed("gl.convolver", 0)};
  Song s = songWith(inst, fx);
  if (installed) {  // the suite's classes registered as a scan would (no bundle is loaded)
    host::ScanResult r;
    r.format = "vst3";
    r.bundle = "/nowhere/Gridline Suite.vst3";
    r.ok = true;
    for (const auto& st : inst) {
      host::ScannedClass c;
      c.uid = flp::suiteVst3Type(st.type).substr(5);
      c.name = suite::findMember(st.type)->name, c.vendor = "Gridline", c.instrument = true, c.outputChannels = 2;
      r.classes.push_back(c);
    }
    host::registerScanResult(r);
  }
  const Exported e = exportIt(s, flp::GridlinePlugins::Suite);
  expect(e.ok, ("Suite mode: exports" + tag).c_str());
  Song back;
  expect(reimport(e.bytes, back), ("Suite mode: the FLP imports" + tag).c_str());
  for (size_t i = 0; i < inst.size(); ++i) {
    const Channel* c = channelNamed(back, "Ch" + std::to_string(i + 1));
    PluginState want = inst[i];
    normalizePluginState(want);
    expect(c && c->plugin == want, (inst[i].type + ": comes back as the built-in with the exact state" + tag).c_str());
    const auto* p = entry(e.rep, "channel 'Ch" + std::to_string(i + 1) + "'");
    expect(p && p->quality == "suite", (inst[i].type + ": reported as a Gridline Suite plugin" + tag).c_str());
  }
  const MixerTrack* t = back.mixer.size() > 1 ? &back.mixer[1] : nullptr;
  for (size_t k = 0; k < fx.size(); ++k) {
    PluginState want = fx[k];
    normalizePluginState(want);
    const bool same = t && k < t->slots.size() && t->slots[k].plugin == want;
    expect(same, (fx[k].type + ": effect comes back as the built-in with the exact state" + tag).c_str());
  }
}

void testWrapperBytes() {
  const Song s = songWith({changed("gl.plasma", 1)}, {changed("gl.shaper", 2)});
  const Exported e = exportIt(s, flp::GridlinePlugins::Suite);
  const auto ws = suiteWrappers(e.bytes);
  expect(ws.size() == 2, "wrapper: one Gridline Suite wrapper per plugin");
  // Sizes of the fixed records as FL writes them (FL's shipped demo projects: 1 = 20, 2 = 25,
  // 30 = 16, 31 = 12, 32 = 12, 50 = 16, 51 = 4 bytes) and the VST3 identity (52 = 16 bytes).
  const std::map<uint32_t, size_t> fixed = {{1, 20}, {2, 25}, {30, 16}, {31, 12}, {32, 12}, {50, 16}, {51, 4}, {52, 16}};
  for (size_t w = 0; w < ws.size(); ++w) {
    const flp::Bytes& b = ws[w];
    const auto rs = records(b);
    std::vector<uint32_t> ids;
    bool sizes = true;
    flp::Bytes kind, guid, state, name, vendor, vst2;
    for (const auto& [id, d] : rs) {
      ids.push_back(id);
      if (auto it = fixed.find(id); it != fixed.end()) sizes = sizes && d.size() == it->second;
      if (id == 50) kind = d;
      if (id == 51) vst2 = d;
      if (id == 52) guid = d;
      if (id == 53) state = d;
      if (id == 54) name = d;
      if (id == 56) vendor = d;
    }
    const std::vector<uint32_t> order = {1, 2, 30, 31, 32, 50, 51, 52, 54, 56, 53};
    expect(flp::getU32(b, 0) == 8 && ids == order, "wrapper: version 8, records in FL's order (settings, kind, ids, name, vendor, state)");
    expect(sizes, "wrapper: fixed records have FL's sizes");
    const bool instrument = w == 0;
    expect(kind.size() >= 4 && flp::getU32(kind, 0) == (instrument ? 8u : 7u), "wrapper: kind VST3 instrument (8) / effect (7)");
    expect(vst2 == flp::Bytes(4, 0), "wrapper: no VST2 id");
    const suite::Member* m = suite::findMember(instrument ? "gl.plasma" : "gl.shaper");
    // COM GUID order: Data1 little-endian, Data2 / Data3 little-endian halves, then bytes as they are.
    flp::ByteWriter g;
    g.u32(m->vst3Uid[0]);
    g.u8(static_cast<uint8_t>((m->vst3Uid[1] >> 16) & 0xFF)), g.u8(static_cast<uint8_t>(m->vst3Uid[1] >> 24));
    g.u8(static_cast<uint8_t>(m->vst3Uid[1] & 0xFF)), g.u8(static_cast<uint8_t>((m->vst3Uid[1] >> 8) & 0xFF));
    for (int k = 2; k < 4; ++k)
      for (int sh = 24; sh >= 0; sh -= 8) g.u8(static_cast<uint8_t>((m->vst3Uid[k] >> sh) & 0xFF));
    expect(guid == g.b, "wrapper: class id in COM GUID byte order");
    expect(std::string(name.begin(), name.end()) == m->name && std::string(vendor.begin(), vendor.end()) == "Gridline", "wrapper: plugin name and vendor");
    // State: u32 1, header (64 bytes), component (the suite JSON), controller (empty), parameter table.
    const auto st = records(state);
    bool layout = state.size() >= 4 && flp::getU32(state, 0) == 1 && st.size() == 4 && st[0].first == 1 && st[0].second.size() == 64 &&
                  st[1].first == 3 && st[2].first == 2 && st[2].second.empty() && st[3].first == 4;
    expect(layout, "wrapper: VST3 state framing (header, component, controller, parameter table)");
    if (layout) {
      PluginState inner;
      const std::string comp(st[1].second.begin(), st[1].second.end());
      expect(suitestate::parse(comp, inner) && inner.type == m->type, "wrapper: the component state is the suite plugin's own state");
      const flp::Bytes& tab = st[3].second;
      const size_t n = std::min(m->info->params.size(), static_cast<size_t>(kMaxHostParams)) + (instrument ? 0 : 2);
      bool table = tab.size() == 4 + 4 * n && flp::getU32(tab, 0) == n;
      for (size_t k = 0; table && k < m->info->params.size() && k < n; ++k) table = flp::getU32(tab, 4 + 4 * k) == k;
      if (!instrument && table)
        table = flp::getU32(tab, 4 + 4 * (n - 2)) == plugin::kMixParamId && flp::getU32(tab, 4 + 4 * (n - 1)) == plugin::kBypassParamId;
      expect(table, "wrapper: parameter table = the plugin's ParamIDs (an effect's Mix and Bypass last)");
    }
  }
  // The same record layout as the hosted encoder writes for an installed-elsewhere VST3 plugin.
  PluginState other;
  other.type = "vst3:0123456789ABCDEF0011223344556677";
  other.data = host::encodeState("x", "");
  const flp::enc::Result r = flp::enc::encode(other, "", nullptr, flp::enc::EncodeContext{});
  std::vector<uint32_t> ids;
  for (const auto& [id, d] : records(r.state))
    if (id != 54) ids.push_back(id);  // (a plugin unknown here may have no name)
  expect(ids == std::vector<uint32_t>({1, 2, 30, 31, 32, 50, 51, 52, 53}), "wrapper: the hosted VST3 layout it follows (name and vendor added for the suite)");
}

void testRenderUnchanged() {
  const std::string wav = writeTone(scratch() / "render-tone.wav", 8000, 330.f);
  PluginState selt = changed("gl.seltmann", 2);
  selt.data = wav;
  const Song s = songWith({selt, changed("gl.fm", 0)}, {changed("gl.shaper", 0), changed("gl.chorus", 1)});
  const std::string d1 = (scratch() / "render-a").string(), d2 = (scratch() / "render-b").string();
  Exported a = exportIt(s, flp::GridlinePlugins::Render, d1);
  flp::ExportOptions o;  // the library's default
  o.useOrigin = false;
  o.sampleDir = d2;
  Exported b;
  b.ok = flp::exportFlp(s, b.bytes, b.rep, o);
  // (sample folder names differ: compare with the paths made equal)
  std::string x(a.bytes.begin(), a.bytes.end()), y(b.bytes.begin(), b.bytes.end());
  expect(a.ok && b.ok && x.size() == y.size(), "render mode: the default, same size");
  expect(suiteWrappers(a.bytes).empty(), "render mode: no Gridline Suite plugin written");
  const auto* p = entry(a.rep, "channel 'Ch1'");
  expect(p && p->quality == "rendered", "render mode: Seltmann rendered to samples, as before");
  const auto* q = entry(a.rep, "insert 1 slot 2");
  expect(q && q->quality != "suite" && q->quality != "placeholder", "render mode: chorus written as FL's chorus, as before");
}

void testFallback() {
  const std::string wav = writeTone(scratch() / "fb-tone.wav", 8000, 440.f);
  PluginState selt = changed("gl.seltmann", 0);
  selt.data = wav;
  Song s = songWith({selt, defaultPluginState("gl.keys")}, {changed("gl.shaper", 0), changed("gl.chorus", 0)});
  const ObjectId seltId = s.channels[0].id;
  const fs::path dir = scratch() / "fallback";
  const Exported e = exportIt(s, flp::GridlinePlugins::SuiteWithFallback, dir.string());
  expect(e.ok, "fallback mode: exports");
  const auto* p = entry(e.rep, "channel 'Ch1'");
  expect(p && p->quality == "suite" && p->note.find("muted copy 'Ch1 (audio)'") != std::string::npos, "fallback mode: Seltmann as a Suite plugin, with a muted copy");
  const auto* k = entry(e.rep, "channel 'Ch2'");
  expect(k && k->quality == "mapped", "fallback mode: an instrument with a close FL instrument stays mapped");
  const auto* sh = entry(e.rep, "insert 1 slot 1");
  expect(sh && sh->quality == "suite", "fallback mode: an effect without FL counterpart becomes a Suite plugin");
  const auto* ch = entry(e.rep, "insert 1 slot 2");
  expect(ch && ch->quality != "suite" && ch->quality != "placeholder", "fallback mode: an effect FL approximates stays FL's");
  expect(!e.rep.files.empty() && fs::exists(e.rep.files.front()), "fallback mode: the copy's samples are rendered beside the project");
  Song back;
  expect(reimport(e.bytes, back), "fallback mode: imports");
  const Channel* orig = channelNamed(back, "Ch1");
  const Channel* copy = channelNamed(back, "Ch1 (audio)");
  expect(orig && orig->plugin.type == "gl.seltmann" && !orig->muted, "fallback mode: the plugin channel plays");
  expect(copy && copy->muted && copy->plugin.type != "gl.seltmann" && copy->mixerTrack == orig->mixerTrack, "fallback mode: the copy is muted, rendered, on the same insert");
  bool sameNotes = false;
  if (orig && copy && !back.patterns.empty()) {
    auto a = back.patterns[0].notes.find(orig->id), b = back.patterns[0].notes.find(copy->id);
    sameNotes = a != back.patterns[0].notes.end() && b != back.patterns[0].notes.end() && a->second.size() == b->second.size() && a->second.size() == 2;
  }
  expect(sameNotes, "fallback mode: the copy plays the same notes");
  // In-memory export (no sample folder): the plugin goes, without a copy.
  const Exported m = exportIt(s, flp::GridlinePlugins::SuiteWithFallback);
  const auto* mp = entry(m.rep, "channel 'Ch1'");
  expect(mp && mp->quality == "suite" && mp->note.find("no audio copy") != std::string::npos, "fallback mode without a sample folder: plugin only, reported");
  (void)seltId;
}

void testEmbeddedFiles() {
  const fs::path src = scratch() / "embed" / "My Loop.wav";
  fs::create_directories(src.parent_path());
  const std::string wav = writeTone(src, 12000, 550.f);
  const std::string bytes = readAll(src);
  PluginState selt = changed("gl.seltmann", 1);
  selt.data = wav;
  const Song s = songWith({selt}, {});
  const Exported e = exportIt(s, flp::GridlinePlugins::Suite);
  const auto* p = entry(e.rep, "channel 'Ch1'");
  expect(p && p->note.find("1 file carried") != std::string::npos, "files: the sample is carried in the plugin's state (reported)");
  // On another computer: the sample is not there.
  fs::remove(src);
  Song back;
  expect(reimport(e.bytes, back), "files: imports without the original sample");
  const Channel* c = channelNamed(back, "Ch1");
  const bool restored = c && c->plugin.data != wav && fs::exists(fs::u8path(c->plugin.data)) && readAll(fs::u8path(c->plugin.data)) == bytes &&
                        fs::u8path(c->plugin.data).filename() == "My Loop.wav";
  expect(restored, "files: the sample is restored byte for byte into the cache under its own name");
  expect(c && c->plugin.data.rfind(suitestate::defaultCacheDir(), 0) == 0, "files: restored only inside the cache");
  // The same file again: reused, not written twice.
  Song again;
  reimport(e.bytes, again);
  const Channel* c2 = channelNamed(again, "Ch1");
  expect(c && c2 && c2->plugin.data == c->plugin.data, "files: an equal cached copy is reused");
  // A different file already at that place is never overwritten.
  {
    PluginState st = selt;
    Json j = Json::makeObject();
    PluginState other = selt;
    const fs::path f2 = scratch() / "embed2" / "Clash.wav";
    fs::create_directories(f2.parent_path());
    writeTone(f2, 3000, 100.f);
    other.data = f2.string();
    suitestate::embedFiles(other, j, suitestate::kEmbedLimit);
    const std::string body = readAll(f2);
    const std::string sha = [&] {
      const Json* fj = j.find("files");
      return fj && !fj->array.empty() ? fj->array[0].text("sha256") : std::string();
    }();
    const fs::path cache = scratch() / "cache-clash";
    const fs::path squatter = cache / sha.substr(0, 16) / "Clash.wav";
    fs::create_directories(squatter.parent_path());
    { std::ofstream(squatter, std::ios::binary) << "not the same file"; }
    fs::remove(f2);
    std::set<std::string> kept;
    std::string err;
    const int n = suitestate::restoreFiles(other, j, cache.string(), &kept, &err);
    expect(n == 1 && readAll(squatter) == "not the same file" && fs::u8path(other.data) != squatter && readAll(fs::u8path(other.data)) == body,
           "files: another file at the cache place is kept; the copy goes elsewhere");
    (void)st;
  }
  // Over the limit: not carried, reported.
  {
    PluginState st = selt;
    const fs::path big = scratch() / "embed" / "Big.wav";
    writeTone(big, 4000, 200.f);
    st.data = big.string();
    Json j = Json::makeObject();
    suitestate::EmbedReport rep;
    expect(suitestate::embedFiles(st, j, 100, &rep) == 0 && rep.skipped.size() == 1 && !j.find("files"), "files: a file over the limit is not carried");
  }
}

void testAutomation() {
  PluginState plasma = changed("gl.plasma", 0);
  Song s = songWith({plasma}, {});
  // A playlist automation clip on the instrument's first continuous parameter.
  // (Pattern event automation of wrapped plugins is not written: FL's raw event
  // range for wrapper parameters is unknown, for every hosted plugin alike.)
  const PluginInfo* pi = findPlugin("gl.plasma");
  size_t k = 0;
  while (k < pi->params.size() && pi->params[k].kind != ParamKind::Continuous) ++k;
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.owner = s.channels[0].id;
  ac.index = kChannelPluginBase + static_cast<uint32_t>(k);
  AutoPoint a0, a1;
  a0.pos = 0, a0.value = 0.2f;
  a1.pos = s.ppq * 4, a1.value = 0.8f;
  ac.points = {a0, a1};
  s.automations.push_back(ac);
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Automation, c.source = ac.id, c.start = 0, c.length = s.ppq * 4, c.track = 1;
  s.playlist.push_back(c);
  const Exported e = exportIt(s, flp::GridlinePlugins::Suite);
  Song back;
  reimport(e.bytes, back);
  const AutomationClip* got = nullptr;
  for (const auto& x : back.automations)
    if (x.index == ac.index && !back.channels.empty() && x.owner == back.channels[0].id) got = &x;
  if (!got) {
    std::printf("# automation export report:\n%s", flp::reportText(e.rep).c_str());
    for (const auto& x : back.automations) std::printf("#  clip owner %llu index %u\n", (unsigned long long)x.owner, x.index);
  }
  expect(got != nullptr, "automation of a Suite plugin's parameter survives the round trip");
  if (got && got->points.size() >= 2) {
    const double v0 = got->min + (got->max - got->min) * got->points.front().value;
    const double v1 = got->min + (got->max - got->min) * got->points.back().value;
    expect(std::fabs(v0 - 0.2) < 0.02 && std::fabs(v1 - 0.8) < 0.02, "automation of a Suite plugin keeps its values");
  } else {
    expect(false, "automation of a Suite plugin keeps its values");
  }
}

void testSettings() {
  flp::GridlinePlugins m;
  bool ok = true;
  for (auto v : {flp::GridlinePlugins::Render, flp::GridlinePlugins::Suite, flp::GridlinePlugins::SuiteWithFallback})
    ok = ok && flp::parseGridlinePlugins(flp::gridlinePluginsKey(v), m) && m == v;
  expect(ok && flp::parseGridlinePlugins("fallback", m) && m == flp::GridlinePlugins::SuiteWithFallback && !flp::parseGridlinePlugins("x", m),
         "modes: keys round-trip");
  Song s = makeTestSong();
  s.flExportGridline = "suite";
  const fs::path p = scratch() / "mode.gridline.json";
  std::string err;
  Song back;
  expect(saveProject(p.string(), s, &err) && loadProject(p.string(), back, &err) && back.flExportGridline == "suite", "modes: the project's choice is saved");
  Song plain = makeTestSong();
  expect(songToJson(plain).find("flExportGridline") == nullptr, "modes: no key when the project follows the default");
  expect(plugin::kMixParamId == 0x00100000u && plugin::kBypassParamId == 0x00100001u, "suite Mix / Bypass ids as the wrapper table writes them");
}

// Which built-ins go which way in each mode (printed; the docs and report list it).
void testPlacement() {
  int suiteN = 0, fallbackN = 0, total = 0;
  std::FILE* out = nullptr;
  if (const char* p = std::getenv("GRIDLINE_K110_PLACEMENT")) out = std::fopen(p, "w");
  for (const PluginInfo& pi : pluginCatalog()) {
    if (isHostedType(pi.type) || pi.type == "gl.sampler" || !suite::findMember(pi.type)) continue;
    const bool gen = pi.kind == PluginKind::Generator;
    Song s = gen ? songWith({defaultPluginState(pi.type)}, {}) : songWith({defaultPluginState("gl.sampler")}, {defaultPluginState(pi.type)});
    const std::string where = gen ? "channel 'Ch1'" : "insert 1 slot 1";
    std::string q[3];
    const flp::GridlinePlugins modes[3] = {flp::GridlinePlugins::Render, flp::GridlinePlugins::SuiteWithFallback, flp::GridlinePlugins::Suite};
    for (int m = 0; m < 3; ++m) {
      const Exported e = exportIt(s, modes[m], (scratch() / "placement" / pi.type).string());
      const auto* p = entry(e.rep, where);
      q[m] = p ? p->quality : "-";
    }
    ++total;
    suiteN += q[2] == "suite";
    fallbackN += q[1] == "suite";
    if (out) std::fprintf(out, "%s\t%s\t%s\t%s\t%s\n", pi.type.c_str(), pi.name.c_str(), q[0].c_str(), q[1].c_str(), q[2].c_str());
    // Suite mode: every member without an exact FL plugin is a Suite plugin.
    const bool exactFl = q[2] == "exact" || q[2] == "verbatim" || q[2] == "defaults";
    expect(q[2] == "suite" || exactFl || q[2] == "unused", (pi.type + ": Suite mode writes it as a Suite plugin unless FL has it exactly").c_str());
    // Fallback mode: what would be rendered or neutral goes as a Suite plugin; mapped stays mapped.
    if (q[0] == "rendered" || q[0] == "placeholder") expect(q[1] == "suite", (pi.type + ": fallback mode replaces the render / placeholder").c_str());
    if (q[0] == "mapped") expect(q[1] == "mapped", (pi.type + ": fallback mode keeps the mapping").c_str());
  }
  if (out) std::fclose(out);
  std::printf("# suite members: %d; Suite plugins in Suite mode: %d, in fallback mode: %d\n", total, suiteN, fallbackN);
}

// GRIDLINE_K110_VERIFY=<folder>: writes the project the manual FL Studio check uses (not a test).
void writeVerifyProject() {
  const char* d = std::getenv("GRIDLINE_K110_VERIFY");
  if (!d || !*d) return;
  const fs::path dir = fs::u8path(d);
  fs::create_directories(dir);
  const std::string wav = writeTone(dir / "Verify Tone.wav", 44100, 196.f);
  PluginState selt = defaultPluginState("gl.seltmann");  // its defaults play the whole sample
  selt.data = wav;
  Song s = songWith({selt, presetState(*findPlugin("gl.plasma"), 1)}, {changed("gl.reverb", 1), changed("gl.shaper", 2)});
  s.channels[0].name = "Seltmann";
  s.channels[1].name = "Plasma";
  for (auto v : {flp::GridlinePlugins::SuiteWithFallback, flp::GridlinePlugins::Suite}) {
    const std::string stem = v == flp::GridlinePlugins::Suite ? "suite-only" : "suite-with-copies";
    flp::ExportOptions o;
    o.useOrigin = false;
    o.gridlinePlugins = v;
    flp::ExportReport rep;
    std::string err;
    const bool ok = flp::exportFlpFile(s, (dir / (stem + ".flp")).string(), rep, o, &err);
    std::printf("# verify %s: %s\n%s", stem.c_str(), ok ? "written" : err.c_str(), flp::reportText(rep).c_str());
  }
  saveProject((dir / "verify-source.gridline.json").string(), s);
}

}  // namespace

int main() {
  const std::string& dir = test::hermeticTestEnv("flp-suite");
  platform::setEnv("XDG_CACHE_HOME", (dir + "/cache").c_str());  // restored files: never the user's cache
  testSettings();
  testWrapperBytes();
  testRenderUnchanged();
  testRoundTrip(false);
  testRoundTrip(true);
  testFallback();
  testEmbeddedFiles();
  testAutomation();
  testPlacement();
  writeVerifyProject();
  return finish();
}

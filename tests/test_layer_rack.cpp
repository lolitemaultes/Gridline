// K-169: split and layer instrument: zones, fades, transpose, layer solo, the cycle / no-repeat
// modes, macros on the internal controller links, layer presets, FLP export rules, undo, the
// collaboration element path, and old layers playing exactly as before.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "core/BrowserIndex.h"
#include "core/Document.h"
#include "core/LayerPick.h"
#include "core/LayerRack.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "packs/Packs.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 48000;

struct Rig {
  Song s = makeEmptySong();
  ObjectId layer = kNoId;
  std::vector<ObjectId> kids;
  ObjectId add(const std::string& name, const std::string& type = "gl.synth") {
    Channel c;
    c.id = s.allocateId();
    c.name = name;
    c.plugin = defaultPluginState(type);
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    return c.id;
  }
  explicit Rig(int n, int mode = 0) {
    for (int i = 0; i < n; ++i) kids.push_back(add("kid" + std::to_string(i)));
    layer = add("layer");
    s.channel(layer)->plugin = PluginState{};
    s.channel(layer)->layer = kids;
    s.channel(layer)->layerMode = mode;
  }
  Channel& L() { return *s.channel(layer); }
  void zone(int k, LayerZone z) {
    z.child = kids[static_cast<size_t>(k)];
    L().layerZones.push_back(z);
  }
  void notes(std::vector<Note> ns) { s.patterns[0].notes[layer] = std::move(ns); }
  std::vector<NoteEv> of(int k) const { return compilePatternChannel(s, s.patterns[0], kids[static_cast<size_t>(k)]); }
};

// The older random pick (a copy of the formula the engine has always used), to prove old layers unchanged.
uint32_t legacyHash(const NoteEv& e, ObjectId layer) {
  uint32_t h = static_cast<uint32_t>(e.on) * 2654435761u ^ static_cast<uint32_t>(e.pitch * 16.0f) * 40503u ^ layer * 97u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}

std::vector<Note> scale8() {
  std::vector<Note> v;
  for (int i = 0; i < 8; ++i) v.push_back(Note(i * 24, 20, 48 + i * 3, 100));
  return v;
}

bool hasKey(const Json& j, const std::string& k) {
  if (j.is(Json::Type::Object)) {
    if (j.find(k)) return true;
    for (const auto& [kk, v] : j.object)
      if (hasKey(v, k)) return true;
  }
  if (j.is(Json::Type::Array))
    for (const auto& v : j.array)
      if (hasKey(v, k)) return true;
  return false;
}

// ---------------------------------------------------------------- old layers

void testOldLayersUnchanged() {
  for (int mode : {0, 1}) {
    Rig r(3, mode);
    r.notes(scale8());
    const Song before = r.s;
    // The compiled notes: mode 0 every child plays all 8; mode 1 the legacy hash pick.
    std::vector<NoteEv> parent = compileChannelNotes(r.s, r.s.patterns[0], *r.s.channel(r.layer));
    bool same = true;
    for (int k = 0; k < 3; ++k) {
      std::vector<NoteEv> got = r.of(k);
      std::vector<float> want;
      for (const NoteEv& e : parent)
        if (mode == 0 || legacyHash(e, r.layer) % 3 == static_cast<uint32_t>(k)) want.push_back(e.pitch);
      same = same && got.size() == want.size();
      for (size_t i = 0; same && i < got.size(); ++i) same = got[i].pitch == want[i] && got[i].velocity == parent[0].velocity;
    }
    expect(same, mode ? "old random layer: the same child picks as before" : "old all layer: every child plays every note");
    // Saved: no new keys; loaded back: equal.
    const Json j = songToJson(r.s);
    expect(!hasKey(j, "layerZones") && !hasKey(j, "macros"), "old layer saves without the new keys");
    Song back;
    expect(songFromJson(j, back) && back.channel(r.layer)->layer == before.channel(r.layer)->layer &&
               back.channel(r.layer)->layerMode == mode && !back.channel(r.layer)->layerRack(),
           "old layer loads back the same");
    // A full zone entry is no zone: the old path keeps running.
    LayerZone full;
    r.zone(1, full);
    expect(!r.L().layerRack(), "a full zone keeps the old dispatch");
  }
}

void testFlpUnchanged() {
  Rig r(2, 1);
  r.notes(scale8());
  flp::Bytes a, b;
  flp::ExportReport ra, rb;
  flp::ExportOptions opt;
  opt.renderInstruments = false;
  expect(flp::exportFlp(r.s, a, ra, opt), "FLP export of an old layer");
  Rig z = r;
  LayerZone full;
  z.zone(0, full);  // stored but full: nothing changes
  expect(flp::exportFlp(z.s, b, rb, opt) && a == b, "FLP bytes identical with a full zone");
  Song back;
  flp::Report rep;
  std::string err;
  expect(importFlpBytes(a, back, rep, &err), "FLP of the layer imports");
  const Channel* bl = nullptr;
  for (const auto& c : back.channels)
    if (c.isLayer()) bl = &c;
  expect(bl && bl->layer.size() == 2 && bl->layerMode == 1 && !bl->layerRack(), "FLP layer round-trips with its random mode");
  // Zones, the newer modes and macros: written as plain layers, with a report line each.
  Rig n(2, kLayerCycle);
  LayerZone lo;
  lo.keyHi = 59;
  n.zone(0, lo);
  n.L().macros[0].name = "Tone";
  flp::Bytes c;
  flp::ExportReport rc;
  expect(flp::exportFlp(n.s, c, rc, opt), "FLP export of a zoned layer");
  auto has = [&](const char* w) {
    for (const auto& x : rc.warnings)
      if (x.find(w) != std::string::npos) return true;
    return false;
  };
  expect(has("cycle / no-repeat") && has("macros"), "FLP export reports the cycle mode and the macros");
  Rig n2(2, 0);
  n2.zone(0, lo);
  flp::ExportReport rz;
  flp::Bytes cz;
  flp::exportFlp(n2.s, cz, rz, opt);
  bool zw = false;
  for (const auto& x : rz.warnings) zw = zw || x.find("zones are not written") != std::string::npos;
  expect(zw, "FLP export reports zones it cannot write");
}

// ---------------------------------------------------------------- zones

void testZones() {
  Rig r(2);
  LayerZone lo, hi;
  lo.keyHi = 59, lo.keyFadeHi = 4;   // fades out over 56..59
  hi.keyLo = 56, hi.keyFadeLo = 4;   // fades in over 56..59
  r.zone(0, lo);
  r.zone(1, hi);
  r.notes({Note(0, 20, 40, 100), Note(24, 20, 57, 100), Note(48, 20, 80, 100)});
  auto a = r.of(0), b = r.of(1);
  expect(a.size() == 2 && a[0].pitch == 40 && a[1].pitch == 57, "low zone: 40 and 57 (in its fade)");
  expect(b.size() == 2 && b[0].pitch == 57 && b[1].pitch == 80, "high zone: 57 (in its fade) and 80");
  const float v = a[0].velocity;
  // Fade by velocity: key 57 is 3 keys below keyHi 59 (weight 3/5) and 2 above keyLo 56 (weight 2/5).
  expect(std::fabs(a[1].velocity - v * 3.0f / 5.0f) < 1e-5f && std::fabs(b[0].velocity - v * 2.0f / 5.0f) < 1e-5f,
         "crossfade scales the velocity by the fade weight");
  expect(std::fabs(layerZoneWeight(lo, 40, 100) - 1.0f) < 1e-6f && layerZoneWeight(lo, 60, 100) == 0.0f, "zone weight inside / outside");
  // Velocity zones (velocity switch).
  Rig v2(2);
  LayerZone soft, hard;
  soft.velHi = 79;
  hard.velLo = 80;
  v2.zone(0, soft);
  v2.zone(1, hard);
  v2.notes({Note(0, 20, 60, 40), Note(24, 20, 60, 120)});
  auto sa = v2.of(0), sb = v2.of(1);
  expect(sa.size() == 1 && sb.size() == 1 && sa[0].on == 0 && sb[0].on > 0, "velocity zones switch children by velocity");
  // Transpose and solo.
  Rig t(2);
  LayerZone up;
  up.transpose = 12;
  up.solo = true;
  t.zone(1, up);
  t.notes({Note(0, 20, 60, 100)});
  expect(t.of(0).empty() && t.of(1).size() == 1 && t.of(1)[0].pitch == 72, "layer solo and transpose");
  // The child's mute leaves it out of the picks; its own key range still applies.
  Rig m(2, kLayerCycle);
  m.s.channel(m.kids[0])->muted = true;
  m.notes(scale8());
  expect(m.of(0).empty() && m.of(1).size() == 8, "cycle skips a muted child");
}

void testModes() {
  Rig r(3, kLayerCycle);
  r.notes(scale8());
  std::vector<int> who(8, -1);
  for (int k = 0; k < 3; ++k)
    for (const NoteEv& e : r.of(k)) who[static_cast<size_t>((e.on) / 24)] = k;
  bool cyc = true;
  for (int i = 0; i < 8; ++i) cyc = cyc && who[static_cast<size_t>(i)] == i % 3;
  expect(cyc, "cycle: the children take turns 0, 1, 2, 0, ...");
  Rig nr(3, kLayerRandomNoRepeat);
  std::vector<Note> many;
  for (int i = 0; i < 40; ++i) many.push_back(Note(i * 12, 10, 60 + (i % 5), 100));
  nr.notes(many);
  std::vector<int> pick(40, -1);
  int total = 0;
  for (int k = 0; k < 3; ++k)
    for (const NoteEv& e : nr.of(k)) pick[static_cast<size_t>(e.on / 12)] = k, ++total;
  bool norep = total == 40;
  for (int i = 1; i < 40; ++i) norep = norep && pick[static_cast<size_t>(i)] != pick[static_cast<size_t>(i - 1)];
  std::set<int> used(pick.begin(), pick.end());
  expect(norep && used.size() == 3, "random without repeats: one child per note, never twice in a row, all used");
  // Random with zones picks only children that hold the note.
  Rig rz(3, kLayerRandom);
  LayerZone none;
  none.keyLo = 100;
  rz.zone(2, none);
  rz.notes(many);
  int n01 = static_cast<int>(rz.of(0).size() + rz.of(1).size());
  expect(rz.of(2).empty() && n01 == 40, "random with zones: every note plays, on children that hold it");
  // Deterministic: compiled twice, the same.
  expect(nr.of(1).size() == Rig(nr).of(1).size(), "the picks are the same every time");
  // The pick function itself.
  layer::PickState st;
  float w[3] = {1, 0, 1};
  expect(layer::pick(kLayerCycle, w, 3, 0, st) == 1u && layer::pick(kLayerCycle, w, 3, 0, st) == 4u && layer::pick(kLayerAll, w, 3, 0, st) == 5u,
         "pick: cycle over the eligible children, all = every eligible one");
}

// ---------------------------------------------------------------- live notes

float peakOf(ProgramBuilder& pb, ObjectId id) {
  auto rt = pb.channelRuntime(id);
  return rt ? rt->peak.load() : 0.0f;
}

void testLive() {
  Rig r(2);
  LayerZone lo, hi;
  lo.keyHi = 59;
  hi.keyLo = 60;
  r.zone(0, lo);
  r.zone(1, hi);
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  eng.setProgram(pb.build(r.s));
  std::vector<float> L(2048), R(2048);
  eng.noteOn(r.layer, 72, 0.8f);
  for (int i = 0; i < 4; ++i) eng.process(L.data(), R.data(), 512);
  expect(peakOf(pb, r.kids[1]) > 1e-3f && peakOf(pb, r.kids[0]) < 1e-6f, "live: a high note plays only the high zone");
  eng.noteOff(r.layer, 72);
  // Cycle live: three notes go to 0, 1, 0.
  Rig c(2, kLayerCycle);
  ProgramBuilder pc;
  AudioEngine e2(kSr);
  e2.setProgram(pc.build(c.s));
  e2.noteOn(c.layer, 60, 0.8f);
  for (int i = 0; i < 4; ++i) e2.process(L.data(), R.data(), 512);
  const bool first = peakOf(pc, c.kids[0]) > 1e-3f && peakOf(pc, c.kids[1]) < 1e-6f;
  e2.noteOn(c.layer, 62, 0.8f);
  for (int i = 0; i < 4; ++i) e2.process(L.data(), R.data(), 512);
  expect(first && peakOf(pc, c.kids[1]) > 1e-3f, "live cycle: the second note goes to the second child");
  // Block-size invariance of the sequenced zoned layer.
  Rig b(2);
  b.zone(0, lo);
  b.zone(1, hi);
  b.notes(scale8());
  auto renderAt = [&](int block) {
    ProgramBuilder p;
    AudioEngine e(kSr);
    e.setProgram(p.build(b.s));
    e.setMode(PlayMode::Pattern);
    e.setPattern(b.s.patterns[0].id);
    e.play();
    std::vector<float> out;
    std::vector<float> l(static_cast<size_t>(block)), rr(static_cast<size_t>(block));
    for (int n = 0; n < 48000; n += block) {
      e.process(l.data(), rr.data(), block);
      out.insert(out.end(), l.begin(), l.end());
    }
    out.resize(48000);
    return out;
  };
  const auto x = renderAt(256), y = renderAt(512);
  float diff = 0, peak = 0;
  for (size_t i = 0; i < x.size(); ++i) diff = std::max(diff, std::fabs(x[i] - y[i])), peak = std::max(peak, std::fabs(x[i]));
  expect(peak > 0.01f && diff < 1e-4f, "zoned layer renders the same at any block size");
}

// ---------------------------------------------------------------- macros

void testMacros() {
  Rig r(1);
  const ObjectId kid = r.kids[0];
  // A hosted plugin channel joins the layer (its parameters are link targets like any other).
  PluginInfo hi;
  hi.type = "vst3:k169-test";
  hi.name = "Test Host Synth";
  hi.kind = PluginKind::Generator;
  hi.params = {ParamDesc{"Cutoff", "", 0, 1, 0.5, ParamKind::Continuous, ParamScale::Linear, {}}};
  registerDynamicPlugin(hi);
  const ObjectId hosted = r.add("hosted", "vst3:k169-test");
  r.L().layer.push_back(hosted);
  // An effect on the child's own insert.
  MixerTrack& m = r.s.mixer[1];
  m.slots[0].plugin = defaultPluginState("gl.filter");
  r.s.channel(kid)->mixerTrack = m.id;
  Document d(r.s);
  const ObjectId L = r.layer;
  expect(describeParam(d.song(), layerMacroParam(L, 2)).has_value(), "a layer's macro is a parameter");
  expect(isControllerSource(d.song(), L, kLayerMacroSlot) && !isControllerSource(d.song(), kid, kLayerMacroSlot), "a layer is a macro source");
  const int pk = 0;
  const auto targets = layer::macroTargets(d.song(), L);
  const ParamId tBuilt = channelPluginParam(kid, pk), tHosted = channelPluginParam(hosted, 0), tSlot = slotParam(m.id, 0, 0);
  expect(std::find(targets.begin(), targets.end(), tSlot) != targets.end() && std::find(targets.begin(), targets.end(), tHosted) != targets.end(),
         "macro targets list the child's insert effects and hosted parameters");
  expect(layer::mapMacro(d, L, 0, tBuilt, 0.25f, 0.75f) && layer::mapMacro(d, L, 0, tHosted) && layer::mapMacro(d, L, 1, tSlot), "map macros");
  expect(layer::macroLinks(d.song(), L, 0).size() == 2 && layer::macroLinks(d.song(), L, 1).size() == 1, "macro links");
  expect(!layer::mapMacro(d, L, 0, layerMacroParam(L, 1)), "a macro cannot drive its own layer's macros");
  // Macro values reach the targets through the engine's link pass.
  d.setParam(layerMacroParam(L, 0), 1.0, false);
  d.setParam(layerMacroParam(L, 1), 0.0, false);
  ProgramBuilder pb;
  AudioEngine eng(kSr);
  eng.setProgram(pb.build(d.song()));
  std::vector<float> o(512), o2(512);
  eng.process(o.data(), o2.data(), 512);
  const Program* p = eng.program().get();
  const int ki = d.song().channelIndex(kid), hiIdx = d.song().channelIndex(hosted);
  const PluginInfo* si = findPlugin(d.song().channel(kid)->plugin.type);
  const ParamDesc& pd = si->params[static_cast<size_t>(pk)];
  const double want = pd.fromNormalized(0.75);
  const float got = p->channels[static_cast<size_t>(ki)].live.params[static_cast<size_t>(pk)];
  expect(std::fabs(got - want) < 1e-3 * std::max(1.0, std::fabs(want)), "macro at 100% sets the built-in target to the top of its range");
  expect(std::fabs(p->channels[static_cast<size_t>(hiIdx)].live.params[0] - 1.0f) < 1e-4f, "macro reaches the hosted plugin's parameter");
  const int mi = d.song().mixerIndex(m.id);
  const PluginInfo* fi = findPlugin("gl.filter");
  expect(std::fabs(p->mixer[static_cast<size_t>(mi)].slots[0].live.params[0] - fi->params[0].fromNormalized(0)) < 1e-3,
         "macro reaches an effect on the child's insert");
  // Save / load keeps them; removing the layer removes its links.
  Song back;
  expect(songFromJson(songToJson(d.song()), back) && back.modLinks.size() == 3 && back.channel(L)->macros[0].value == 1.0f, "macros saved and loaded");
  expect(layer::renameMacro(d, L, 0, "Tone") && paramDisplayName(d.song(), layerMacroParam(L, 0)).find("Tone") != std::string::npos, "rename a macro");
  d.removeChannel(L);
  expect(d.song().modLinks.empty(), "removing the layer removes its macro links");
}

// ---------------------------------------------------------------- undo, collab

void testUndoAndElements() {
  Rig r(3);
  Document d(r.s);
  LayerZone z;
  z.child = r.kids[0];
  z.keyHi = 50;
  expect(layer::setZone(d, r.layer, z), "set a zone");
  expect(layer::splitKeys(d, r.layer, 2) && d.song().channel(r.layer)->layerZones.size() == 3, "split keys gives every child a zone");
  expect(layer::moveChild(d, r.layer, 0, 2) && d.song().channel(r.layer)->layer.back() == r.kids[0], "reorder children");
  expect(layer::setMode(d, r.layer, kLayerCycle), "mode");
  d.undo();
  d.undo();
  d.undo();
  const Channel* l = d.song().channel(r.layer);
  expect(l->layerMode == 0 && l->layer.front() == r.kids[0] && l->layerZones.size() == 1 && l->layerZones[0].keyHi == 50, "undo steps back each edit");
  d.undo();
  expect(d.song().channel(r.layer)->layerZones.empty(), "undo the first zone");
  // Collaboration sends a channel as its project element: zones, mode and macros travel with it.
  Channel c = r.L();
  c.layerMode = kLayerRandomNoRepeat;
  LayerZone v;
  v.child = r.kids[1];
  v.velLo = 30, v.velFadeLo = 6, v.transpose = -5, v.solo = true;
  c.layerZones = {v};
  c.macros[3] = {"Width", 0.4f};
  Channel back;
  channelFromJson(channelToJson(c), back, r.s);
  back.id = c.id;
  expect(back == c, "a layer channel element round-trips (collaboration)");
  // A zone of a child that is not in the layer is dropped on load.
  Json j = channelToJson(c);
  Channel bad;
  j.object["layerZones"].array[0].object["child"] = Json::makeNumber(999999);
  channelFromJson(j, bad, r.s);
  expect(bad.layerZones.empty(), "a zone for a missing child is dropped");
}

// ---------------------------------------------------------------- presets

void testPresets() {
  Rig r(2, kLayerCycle);
  LayerZone lo;
  lo.keyHi = 59, lo.transpose = 7;
  r.zone(0, lo);
  r.L().macros[0].name = "Bright";
  MixerTrack& m = r.s.mixer[2];
  m.slots[1].plugin = defaultPluginState("gl.filter");
  r.s.channel(r.kids[1])->mixerTrack = m.id;
  Document src(r.s);
  layer::mapMacro(src, r.layer, 0, channelPluginParam(r.kids[0], 0));
  layer::mapMacro(src, r.layer, 0, slotParam(m.id, 1, 0));
  const fs::path dir = fs::temp_directory_path() / ("gl-k169-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string err;
  expect(layer::savePreset(dir.string(), src.song(), r.layer, "Mine/Split", &err), "save a layer preset");
  const auto list = layer::listPresets(dir.string());
  expect(list.size() == 1 && list[0].name == "Split" && list[0].folder == "Mine" && layer::isPresetFile(list[0].file), "listed under layers");
  Json j;
  expect(layer::readPreset(list[0].file, j, &err), "read it back");
  // Load into another project that already uses some ids and inserts.
  Rig other(1);
  other.add("filler");
  for (int k = 0; k < 20; ++k) other.s.allocateId();
  Document d(other.s);
  const size_t before = d.song().channels.size();
  const ObjectId nl = layer::loadPreset(d, j, 0, &err);
  const Channel* L = d.song().channel(nl);
  expect(nl != kNoId && L && L->layer.size() == 2 && d.song().channels.size() == before + 3, "load: a new layer with two new children");
  expect(L && L->layerMode == kLayerCycle && L->macros[0].name == "Bright" && L->layerZones.size() == 1 && L->layerZones[0].transpose == 7 &&
             L->layerZones[0].child == L->layer[0],
         "load: mode, macro names and zones remapped to the new children");
  bool fresh = true;
  for (ObjectId k : L->layer) fresh = fresh && std::find(other.s.channels.begin(), other.s.channels.end(), *d.song().channel(k)) == other.s.channels.end();
  const Channel* k1 = d.song().channel(L->layer[1]);
  const MixerTrack* ins = d.song().mixerTrack(k1->mixerTrack);
  expect(fresh && ins && k1->mixerTrack != d.song().masterId() && ins->slots[1].plugin.type == "gl.filter", "load: the child's insert effects on a free track");
  int links = 0;
  for (const auto& ml : d.song().modLinks)
    links += ml.sourceOwner == nl && ml.sourceSlot == kLayerMacroSlot && (ml.owner == L->layer[0] || ml.owner == ins->id);
  expect(links == 2, "load: macro links re-targeted to the new channels and insert");
  expect(d.song().channels.front().id == L->layer[0], "load: placed at the asked index");
  d.undo();
  expect(d.song().channels.size() == before && d.song().modLinks.empty(), "load is one undo step");
  // A bad document.
  Json bad = Json::makeObject();
  expect(layer::loadPreset(d, bad) == kNoId, "a bad document loads nothing");
  // Built-in layers load and play.
  const auto fac = layer::factoryPresets();
  expect(fac.size() >= 3, "built-in layers");
  bool allLoad = true, allMapped = true;
  for (const auto& [name, fj] : fac) {
    Document fd(makeEmptySong());
    const ObjectId fl = layer::loadPreset(fd, fj);
    const Channel* c = fd.song().channel(fl);
    allLoad = allLoad && c && c->isLayer() && c->layer.size() >= 2;
    if (!allLoad) std::printf("# built-in layer %s did not load\n", name.c_str());
    // Every named macro drives something.
    for (int k = 0; c && k < kLayerMacros; ++k)
      if (!c->macros[static_cast<size_t>(k)].name.empty() && layer::macroLinks(fd.song(), fl, k).empty()) {
        allMapped = false;
        std::printf("# built-in layer %s: macro %s drives nothing\n", name.c_str(), c->macros[static_cast<size_t>(k)].name.c_str());
      }
  }
  expect(allLoad, "every built-in layer loads");
  expect(allMapped, "every named macro of the built-in layers drives a control");
  {
    Document fd(makeEmptySong());
    const ObjectId fl = layer::loadPreset(fd, fac[0].second);
    ProgramBuilder pb;
    AudioEngine eng(kSr);
    eng.setProgram(pb.build(fd.song()));
    eng.noteOn(fl, 48, 0.9f);
    std::vector<float> L2(512), R2(512);
    float pk = 0;
    for (int i = 0; i < 16; ++i) {
      eng.process(L2.data(), R2.data(), 512);
      for (float v : L2) pk = std::max(pk, std::fabs(v));
    }
    expect(pk > 1e-3f && std::isfinite(pk), "a built-in layer sounds");
  }
  {  // the browser: a ".gllayer" file is a layer preset; it drops on the rack and the workspace only
    using namespace browse;
    expect(kindOf("/x/Bass split.gllayer") == Kind::Layer && kindOf("/x/a.GLLAYER") == Kind::Layer, "browser: .gllayer is a layer preset");
    DropItem it;
    it.kind = Kind::Layer;
    expect(dropAction(it, DropTarget::Rack) == DropAction::NewLayer && dropAction(it, DropTarget::Workspace) == DropAction::NewLayer &&
               dropAction(it, DropTarget::MixerSlot) == DropAction::None && dropAction(it, DropTarget::PluginWindow) == DropAction::None,
           "browser: a layer preset drops on the rack and the workspace");
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void testExtremes() {
  Rig r(2);
  Channel c = r.L();
  Json j = channelToJson(c);
  Json zs = Json::makeArray();
  Json z = Json::makeObject();
  z.object["child"] = Json::makeNumber(static_cast<double>(r.kids[0]));
  z.object["keyLo"] = Json::makeNumber(1e9);
  z.object["keyHi"] = Json::makeNumber(-5);
  z.object["keyFadeLo"] = Json::makeNumber(-1e9);
  z.object["transpose"] = Json::makeNumber(1e12);
  zs.array.push_back(z);
  j.object["layerZones"] = zs;
  j.object["layerMode"] = Json::makeNumber(77);
  Channel back;
  channelFromJson(j, back, r.s);
  expect(back.layerMode == kLayerModeCount - 1 && back.layerZones.size() == 1 && back.layerZones[0].keyLo == 0 &&
             back.layerZones[0].keyHi == 127 && back.layerZones[0].transpose == 48,
         "out-of-range zone values clamp on load");
  // Fades wider than the zone and odd velocities stay finite.
  LayerZone w;
  w.keyLo = 60, w.keyHi = 61, w.keyFadeLo = 127, w.keyFadeHi = 127;
  float s = 0;
  for (int k = 0; k < 128; ++k)
    for (int v = 0; v < 128; v += 7) s += layerZoneWeight(w, k, v);
  expect(std::isfinite(s) && s > 0, "zone weights stay finite with extreme fades");
}

}  // namespace

int main() {
  testOldLayersUnchanged();
  testFlpUnchanged();
  testZones();
  testModes();
  testLive();
  testMacros();
  testUndoAndElements();
  testPresets();
  testExtremes();
  return finish();
}

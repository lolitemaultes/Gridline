// Gridline — K-169: layer zones, children, macros and layer presets (see LayerRack.h).
#include "LayerRack.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "Document.h"
#include "MenuOps.h"
#include "Params.h"
#include "PluginInfo.h"
#include "Project.h"

namespace fs = std::filesystem;

namespace gl::layer {

namespace {

constexpr const char* kFormat = "gridline-layer";
constexpr int kVersion = 1;

const Channel* layerOf(const Document& d, ObjectId layer) {
  const Channel* c = d.song().channel(layer);
  return c && c->isLayer() ? c : nullptr;
}

void putZone(Channel& l, const LayerZone& z) {
  auto it = std::find_if(l.layerZones.begin(), l.layerZones.end(), [&](const LayerZone& x) { return x.child == z.child; });
  if (z.full()) {
    if (it != l.layerZones.end()) l.layerZones.erase(it);
  } else if (it != l.layerZones.end()) {
    *it = z;
  } else {
    l.layerZones.push_back(z);
  }
}

LayerZone clampZone(LayerZone z) {
  z.keyLo = std::clamp(z.keyLo, 0, 127);
  z.keyHi = std::clamp(z.keyHi, z.keyLo, 127);
  z.velLo = std::clamp(z.velLo, 0, 127);
  z.velHi = std::clamp(z.velHi, z.velLo, 127);
  const int kw = z.keyHi - z.keyLo + 1, vw = z.velHi - z.velLo + 1;
  z.keyFadeLo = std::clamp(z.keyFadeLo, 0, kw);
  z.keyFadeHi = std::clamp(z.keyFadeHi, 0, kw);
  z.velFadeLo = std::clamp(z.velFadeLo, 0, vw);
  z.velFadeHi = std::clamp(z.velFadeHi, 0, vw);
  z.transpose = std::clamp(z.transpose, -48, 48);
  return z;
}

bool inLayer(const Channel& l, ObjectId child) { return std::find(l.layer.begin(), l.layer.end(), child) != l.layer.end(); }

}  // namespace

LayerZone zoneOf(const Channel& layer, ObjectId child) {
  if (const LayerZone* z = layer.layerZone(child)) return *z;
  LayerZone z;
  z.child = child;
  return z;
}

bool setZone(Document& d, ObjectId layer, const LayerZone& zone, uint64_t mergeKey) {
  const Channel* l = layerOf(d, layer);
  if (!l || !inLayer(*l, zone.child)) return false;
  const LayerZone z = clampZone(zone);
  if (zoneOf(*l, z.child) == z) return false;
  return d.editChannel(layer, "layer zone", [&](Channel& c) { putZone(c, z); }, mergeKey);
}

bool splitKeys(Document& d, ObjectId layer, int fade) {
  const Channel* l = layerOf(d, layer);
  if (!l) return false;
  const std::vector<ObjectId> kids = l->layer;
  int lo = 128, hi = -1;
  for (const auto& p : d.song().patterns)
    if (auto it = p.notes.find(layer); it != p.notes.end())
      for (const Note& n : it->second) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
  if (hi < lo) lo = 48, hi = 83;  // C4..B6
  const int k = static_cast<int>(kids.size());
  if (hi - lo + 1 < k) lo = 0, hi = 127;
  const int width = hi - lo + 1;
  fade = std::clamp(fade, 0, 24);
  return d.editChannel(layer, "split layer keys", [&](Channel& c) {
    for (int i = 0; i < k; ++i) {
      LayerZone z = zoneOf(c, kids[static_cast<size_t>(i)]);
      z.keyLo = i == 0 ? 0 : lo + i * width / k;
      z.keyHi = i == k - 1 ? 127 : lo + (i + 1) * width / k - 1;
      // Fades overlap the neighbour: each side reaches `fade` keys into the next part.
      z.keyFadeLo = i == 0 ? 0 : fade;
      z.keyFadeHi = i == k - 1 ? 0 : fade;
      if (i > 0) z.keyLo = std::max(0, z.keyLo - fade / 2);
      if (i < k - 1) z.keyHi = std::min(127, z.keyHi + (fade + 1) / 2);
      putZone(c, clampZone(z));
    }
  });
}

bool splitVelocity(Document& d, ObjectId layer, int fade) {
  const Channel* l = layerOf(d, layer);
  if (!l) return false;
  const std::vector<ObjectId> kids = l->layer;
  const int k = static_cast<int>(kids.size());
  fade = std::clamp(fade, 0, 32);
  return d.editChannel(layer, "split layer velocity", [&](Channel& c) {
    for (int i = 0; i < k; ++i) {
      LayerZone z = zoneOf(c, kids[static_cast<size_t>(i)]);
      z.velLo = i == 0 ? 0 : i * 128 / k;
      z.velHi = i == k - 1 ? 127 : (i + 1) * 128 / k - 1;
      z.velFadeLo = i == 0 ? 0 : fade;
      z.velFadeHi = i == k - 1 ? 0 : fade;
      if (i > 0) z.velLo = std::max(0, z.velLo - fade / 2);
      if (i < k - 1) z.velHi = std::min(127, z.velHi + (fade + 1) / 2);
      putZone(c, clampZone(z));
    }
  });
}

bool resetZones(Document& d, ObjectId layer) {
  const Channel* l = layerOf(d, layer);
  if (!l || l->layerZones.empty()) return false;
  return d.editChannel(layer, "reset layer zones", [](Channel& c) { c.layerZones.clear(); });
}

bool setMode(Document& d, ObjectId layer, int mode) {
  const Channel* l = d.song().channel(layer);
  mode = std::clamp(mode, 0, kLayerModeCount - 1);
  if (!l || l->layerMode == mode) return false;
  return d.editChannel(layer, "layer mode", [&](Channel& c) { c.layerMode = mode; });
}

bool addChild(Document& d, ObjectId layer, ObjectId child) {
  const Channel* l = d.song().channel(layer);
  const Channel* c = d.song().channel(child);
  if (!l || !c || child == layer || inLayer(*l, child) || l->layer.size() >= static_cast<size_t>(kMaxLayerChildren)) return false;
  if (c->isLayer()) return false;  // no layers inside layers
  return d.editChannel(layer, "add layer child", [&](Channel& x) { x.layer.push_back(child); });
}

bool removeChild(Document& d, ObjectId layer, ObjectId child) {
  const Channel* l = d.song().channel(layer);
  if (!l || !inLayer(*l, child)) return false;
  return d.editChannel(layer, "remove layer child", [&](Channel& x) {
    x.layer.erase(std::remove(x.layer.begin(), x.layer.end(), child), x.layer.end());
    x.layerZones.erase(std::remove_if(x.layerZones.begin(), x.layerZones.end(), [&](const LayerZone& z) { return z.child == child; }),
                       x.layerZones.end());
  });
}

bool moveChild(Document& d, ObjectId layer, int from, int to) {
  const Channel* l = d.song().channel(layer);
  if (!l) return false;
  const int n = static_cast<int>(l->layer.size());
  if (from < 0 || from >= n) return false;
  to = std::clamp(to, 0, n - 1);
  if (from == to) return false;
  return d.editChannel(layer, "reorder layer children", [&](Channel& x) {
    ObjectId id = x.layer[static_cast<size_t>(from)];
    x.layer.erase(x.layer.begin() + from);
    x.layer.insert(x.layer.begin() + to, id);
  });
}

bool renameMacro(Document& d, ObjectId layer, int macro, const std::string& name) {
  const Channel* l = layerOf(d, layer);
  if (!l || macro < 0 || macro >= kLayerMacros || l->macros[static_cast<size_t>(macro)].name == name) return false;
  return d.editChannel(layer, "rename macro", [&](Channel& c) { c.macros[static_cast<size_t>(macro)].name = name.substr(0, 24); });
}

bool mapMacro(Document& d, ObjectId layer, int macro, ParamId target, float min, float max) {
  if (!layerOf(d, layer) || macro < 0 || macro >= kLayerMacros) return false;
  if (target.owner == layer && target.index >= kLayerMacroBase && target.index < kLayerMacroBase + kLayerMacros) return false;
  ModLink l;
  l.sourceOwner = layer;
  l.sourceSlot = kLayerMacroSlot;
  l.sourceOutput = macro;
  l.owner = target.owner;
  l.index = target.index;
  l.min = std::clamp(min, 0.0f, 1.0f);
  l.max = std::clamp(max, 0.0f, 1.0f);
  return d.setModLink(l);
}

std::vector<const ModLink*> macroLinks(const Song& s, ObjectId layer, int macro) {
  std::vector<const ModLink*> out;
  for (const auto& l : s.modLinks)
    if (l.sourceOwner == layer && l.sourceSlot == kLayerMacroSlot && l.sourceOutput == macro) out.push_back(&l);
  return out;
}

std::vector<ParamId> macroTargets(const Song& s, ObjectId layer) {
  std::vector<ParamId> out;
  const Channel* l = s.channel(layer);
  if (!l) return out;
  for (ObjectId k : l->layer) {
    const Channel* c = s.channel(k);
    if (!c) continue;
    out.push_back(channelParam(k, ChannelParam::Volume));
    out.push_back(channelParam(k, ChannelParam::Pan));
    out.push_back(channelParam(k, ChannelParam::Pitch));
    if (const PluginInfo* p = findPlugin(c->plugin.type))
      for (size_t i = 0; i < p->params.size() && i < c->plugin.params.size(); ++i) out.push_back(channelPluginParam(k, static_cast<int>(i)));
    for (size_t fx = 0; fx < c->noteFx.size(); ++fx)
      if (const PluginInfo* p = findPlugin(c->noteFx[fx].type))
        for (size_t i = 0; i < p->params.size(); ++i) out.push_back(noteFxParam(k, static_cast<int>(fx), static_cast<int>(i)));
    if (!ops::channelOwnsMixerTrack(s, k)) continue;
    if (const MixerTrack* m = s.mixerTrack(c->mixerTrack))
      for (size_t sl = 0; sl < m->slots.size(); ++sl) {
        const PluginInfo* p = findPlugin(m->slots[sl].plugin.type);
        if (!p) continue;
        out.push_back(slotMixParam(m->id, static_cast<int>(sl)));
        for (size_t i = 0; i < p->params.size(); ++i) out.push_back(slotParam(m->id, static_cast<int>(sl), static_cast<int>(i)));
      }
  }
  return out;
}

// ------------------------------------------------------------------ presets

std::string presetFolder(const std::string& presetRoot) { return (fs::path(presetRoot) / "layers").string(); }

bool isPresetFile(const std::string& path) {
  std::string e = fs::path(path).extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return e == ".gllayer";
}

std::vector<PresetEntry> listPresets(const std::string& presetRoot) {
  std::vector<PresetEntry> out;
  const fs::path base = presetFolder(presetRoot);
  std::error_code ec;
  if (!fs::is_directory(base, ec)) return out;
  for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec) || !isPresetFile(it->path().string())) continue;
    PresetEntry e;
    e.name = it->path().stem().string();
    e.folder = fs::relative(it->path().parent_path(), base, ec).generic_string();
    if (e.folder == ".") e.folder.clear();
    e.file = it->path().string();
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), [](const PresetEntry& a, const PresetEntry& b) {
    if (a.folder.empty() != b.folder.empty()) return a.folder.empty();
    return a.folder != b.folder ? a.folder < b.folder : a.name < b.name;
  });
  return out;
}

Json presetToJson(const Song& s, ObjectId layer, std::vector<std::string>* leftOut) {
  Json j = Json::makeObject();
  const Channel* l = s.channel(layer);
  if (!l) return j;
  j.object["format"] = Json::makeString(kFormat);
  j.object["version"] = Json::makeNumber(kVersion);
  j.object["name"] = Json::makeString(l->name);
  Json jl = channelToJson(*l);
  jl.object.erase("mixerTrack");
  j.object["layer"] = std::move(jl);
  Json kids = Json::makeArray();
  std::vector<ObjectId> inserts;  // mixer tracks saved (owned by one child)
  for (ObjectId k : l->layer) {
    const Channel* c = s.channel(k);
    if (!c) continue;
    Json e = Json::makeObject();
    Json jc = channelToJson(*c);
    jc.object.erase("mixerTrack");
    jc.object.erase("auxOuts");
    e.object["channel"] = std::move(jc);
    if (ops::channelOwnsMixerTrack(s, k)) {
      if (const MixerTrack* m = s.mixerTrack(c->mixerTrack)) {
        Json jm = mixerTrackToJson(*m);
        jm.object.erase("sends");  // a loaded insert sends to the master
        jm.object.erase("roomId");  // K-224: the Stage room is the project's, never part of the preset
        jm.object["id"] = Json::makeNumber(static_cast<double>(m->id));
        e.object["insert"] = std::move(jm);
        inserts.push_back(m->id);
      }
    } else if (c->mixerTrack != s.masterId() && leftOut) {
      leftOut->push_back(c->name);
    }
    kids.array.push_back(std::move(e));
  }
  j.object["children"] = std::move(kids);
  Json links = Json::makeArray();
  for (const auto& ml : s.modLinks) {
    if (ml.sourceOwner != layer || ml.sourceSlot != kLayerMacroSlot) continue;
    const bool inside = ml.owner == layer || std::find(l->layer.begin(), l->layer.end(), ml.owner) != l->layer.end() ||
                        std::find(inserts.begin(), inserts.end(), ml.owner) != inserts.end();
    if (!inside) continue;
    Json o = Json::makeObject();
    o.object["macro"] = Json::makeNumber(ml.sourceOutput);
    o.object["owner"] = Json::makeNumber(static_cast<double>(ml.owner));
    o.object["index"] = Json::makeNumber(ml.index);
    o.object["min"] = Json::makeNumber(ml.min);
    o.object["max"] = Json::makeNumber(ml.max);
    if (ml.curve != ModCurve::Linear) o.object["curve"] = Json::makeNumber(static_cast<int>(ml.curve));
    if (!ml.formula.empty()) o.object["formula"] = Json::makeString(ml.formula);
    if (ml.smoothMs != 0) o.object["smoothMs"] = Json::makeNumber(ml.smoothMs);
    if (!ml.enabled) {
      Json f;
      f.type = Json::Type::Bool;
      o.object["enabled"] = f;
    }
    links.array.push_back(std::move(o));
  }
  j.object["links"] = std::move(links);
  return j;
}

bool savePreset(const std::string& presetRoot, const Song& s, ObjectId layer, const std::string& relName, std::string* err) {
  const Channel* l = s.channel(layer);
  if (!l || !l->isLayer()) {
    if (err) *err = "Not a layer";
    return false;
  }
  fs::path file = fs::path(presetFolder(presetRoot)) / (relName + ".gllayer");
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  return writeFileAtomic(file.string(), jsonStringify(presetToJson(s, layer)) + "\n", err);
}

bool readPreset(const std::string& file, Json& out, std::string* err) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    if (err) *err = "Cannot open the file";
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  if (!jsonParse(ss.str(), out) || out.text("format") != kFormat) {
    if (err) *err = "Not a layer preset";
    return false;
  }
  return true;
}

ObjectId loadPreset(Document& d, const Json& preset, int at, std::string* err) {
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return kNoId;
  };
  if (preset.text("format") != kFormat) return fail("Not a layer preset");
  if (preset.num("version", 0) > kVersion) return fail("This layer preset needs a newer Gridline");
  const Json* jl = preset.find("layer");
  const Json* kids = preset.find("children");
  if (!jl || !jl->is(Json::Type::Object) || !kids || !kids->is(Json::Type::Array) || kids->array.empty()) return fail("The layer preset is empty");
  ObjectId out = kNoId;
  d.editSong("load layer preset", [&](Song& s) {
    std::map<ObjectId, ObjectId> ids;  // preset id -> new id (channels and inserts)
    std::vector<Channel> made;
    for (const auto& e : kids->array) {
      const Json* jc = e.find("channel");
      if (!jc || !jc->is(Json::Type::Object) || made.size() >= static_cast<size_t>(kMaxLayerChildren)) continue;
      Channel c;
      channelFromJson(*jc, c, s);
      c.id = s.allocateId();
      c.layer.clear();  // no layers inside layers
      c.layerZones.clear();
      c.mixerTrack = s.masterId();
      ids[jsonInt<ObjectId>(jc->num("id", 0))] = c.id;
      if (const Json* jm = e.find("insert"); jm && jm->is(Json::Type::Object)) {
        const ObjectId t = ops::freeMixerTrack(s);
        if (MixerTrack* m = t != kNoId ? s.mixerTrack(t) : nullptr) {
          MixerTrack fresh;
          mixerTrackFromJson(*jm, fresh);
          fresh.id = m->id;
          fresh.sends = m->sends;  // the free track's own routing (to the master)
          fresh.input = -1, fresh.armed = false;
          fresh.room = m->room;  // K-224: the free track stays in its room (old presets may still carry one)
          if (fresh.name.empty()) fresh.name = c.name;
          *m = std::move(fresh);
          c.mixerTrack = t;
          ids[jsonInt<ObjectId>(jm->num("id", 0))] = t;
        }
      }
      made.push_back(std::move(c));
      s.channels.push_back(made.back());  // placed below; here so the next free track sees it
    }
    s.channels.resize(s.channels.size() - made.size());
    Channel l;
    channelFromJson(*jl, l, s);
    l.id = s.allocateId();
    l.mixerTrack = s.masterId();
    std::vector<ObjectId> kidsNew;
    for (ObjectId k : l.layer)
      if (auto it = ids.find(k); it != ids.end()) kidsNew.push_back(it->second);
    if (kidsNew.empty())
      for (const Channel& c : made) kidsNew.push_back(c.id);
    for (auto& z : l.layerZones) z.child = ids.count(z.child) ? ids[z.child] : kNoId;
    l.layerZones.erase(std::remove_if(l.layerZones.begin(), l.layerZones.end(), [](const LayerZone& z) { return z.child == kNoId; }),
                       l.layerZones.end());
    l.layer = kidsNew;
    ids[jsonInt<ObjectId>(jl->num("id", 0))] = l.id;
    const size_t pos = at < 0 ? s.channels.size() : std::min(static_cast<size_t>(at), s.channels.size());
    made.push_back(l);
    s.channels.insert(s.channels.begin() + static_cast<std::ptrdiff_t>(pos), made.begin(), made.end());
    if (const Json* links = preset.find("links"); links && links->is(Json::Type::Array))
      for (const auto& o : links->array) {
        auto it = ids.find(jsonInt<ObjectId>(o.num("owner", 0)));
        if (it == ids.end()) continue;
        ModLink ml;
        ml.sourceOwner = l.id;
        ml.sourceSlot = kLayerMacroSlot;
        ml.sourceOutput = jsonInt<int>(std::clamp(o.num("macro", 0), 0.0, static_cast<double>(kLayerMacros - 1)));
        ml.owner = it->second;
        ml.index = jsonInt<uint32_t>(std::max(0.0, o.num("index", 0)));
        ml.min = static_cast<float>(std::clamp(o.num("min", 0), 0.0, 1.0));
        ml.max = static_cast<float>(std::clamp(o.num("max", 1), 0.0, 1.0));
        ml.curve = static_cast<ModCurve>(jsonInt<int>(std::clamp(o.num("curve", 0), 0.0, static_cast<double>(static_cast<int>(ModCurve::Count) - 1))));
        ml.formula = o.text("formula");
        ml.smoothMs = static_cast<float>(std::clamp(o.num("smoothMs", 0), 0.0, 10000.0));
        if (const Json* en = o.find("enabled"); en && en->is(Json::Type::Bool)) ml.enabled = en->boolean;
        if (!describeParam(s, {ml.owner, ml.index}) || findModLink(s, ml.owner, ml.index)) continue;
        s.modLinks.push_back(std::move(ml));
      }
    out = l.id;
  });
  return out;
}

// ------------------------------------------------------- factory presets

namespace {

int paramByName(const std::string& type, const char* word) {
  const PluginInfo* p = findPlugin(type);
  if (!p) return -1;
  std::string w = word;
  for (size_t i = 0; i < p->params.size(); ++i) {
    std::string n = p->params[i].name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (n.find(w) != std::string::npos) return static_cast<int>(i);
  }
  return -1;
}

struct Builder {
  Song s;
  Channel layer;
  Builder(const char* name, int mode) {
    s.mixer.push_back(MixerTrack{});
    s.mixer[0].id = s.allocateId();
    layer.id = s.allocateId();
    layer.name = name;
    layer.layerMode = mode;
  }
  ObjectId child(const char* name, const std::string& type, LayerZone z, Rgb color) {
    Channel c;
    c.id = s.allocateId();
    c.name = name;
    c.color = color;
    c.plugin = defaultPluginState(type);
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    layer.layer.push_back(c.id);
    z.child = c.id;
    if (!z.full()) layer.layerZones.push_back(z);
    return c.id;
  }
  void macro(int k, const char* name, ObjectId target, int param, float mn, float mx) {
    layer.macros[static_cast<size_t>(k)].name = name;
    if (param < 0) return;
    ModLink l;
    l.sourceOwner = layer.id;
    l.sourceSlot = kLayerMacroSlot;
    l.sourceOutput = k;
    l.owner = target;
    l.index = channelPluginParam(target, param).index;
    l.min = mn;
    l.max = mx;
    s.modLinks.push_back(l);
  }
  Json done() {
    if (!layer.layer.empty()) layer.color = s.channels.front().color;
    s.channels.push_back(layer);
    return presetToJson(s, layer.id);
  }
};

}  // namespace

std::vector<std::pair<std::string, Json>> factoryPresets() {
  std::vector<std::pair<std::string, Json>> out;
  {
    Builder b("Bass and keys split", kLayerAll);
    LayerZone lo, hi;
    lo.keyHi = 61, lo.keyFadeHi = 3;
    hi.keyLo = 58, hi.keyFadeLo = 3;
    const ObjectId bass = b.child("Split bass", "gl.fingerbass", lo, 0x4F8FD8);
    const ObjectId keys = b.child("Split keys", "gl.keys", hi, 0xD8A44F);
    b.macro(0, "Bass tone", bass, paramByName("gl.fingerbass", "treble"), 0, 1);
    b.macro(1, "Keys tone", keys, paramByName("gl.keys", "treble"), 0, 1);
    out.emplace_back("Bass and keys split", b.done());
  }
  {
    Builder b("Soft and hard keys", kLayerAll);
    LayerZone soft, hard;
    soft.velHi = 95, soft.velFadeHi = 16;
    hard.velLo = 80, hard.velFadeLo = 16;
    b.child("Soft keys", "gl.keys", soft, 0xD8A44F);
    const ObjectId pl = b.child("Hard pluck", "gl.pluck", hard, 0xD86A4F);
    b.macro(0, "Pluck decay", pl, paramByName("gl.pluck", "decay"), 0, 1);
    out.emplace_back("Soft and hard keys", b.done());
  }
  {
    Builder b("Wide stack", kLayerAll);
    LayerZone up;
    up.transpose = 12;
    const ObjectId a = b.child("Stack body", "gl.synth", LayerZone{}, 0x6AB04F);
    const ObjectId sw = b.child("Stack air", "gl.sawswarm", up, 0x4FB0A4);
    b.macro(0, "Brightness", a, paramByName("gl.synth", "cutoff"), 0.2f, 1);
    b.macro(1, "Air cutoff", sw, paramByName("gl.sawswarm", "cutoff"), 0.2f, 1);
    out.emplace_back("Wide stack", b.done());
  }
  {
    Builder b("Hat cycle", kLayerCycle);
    LayerZone z1, z2, z3;
    z2.transpose = 1;
    z3.transpose = -1;
    b.child("Hat one", "gl.hat", z1, 0xB0B04F);
    b.child("Hat two", "gl.hat", z2, 0xB0944F);
    b.child("Hat three", "gl.hat", z3, 0xB0784F);
    out.emplace_back("Hat cycle", b.done());
  }
  return out;
}

}  // namespace gl::layer

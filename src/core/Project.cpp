#include "Project.h"
#include "Meter.h"
#include "ClipLoop.h"
#include "ClipGain.h"
#include "Fades.h"
#include "Groove.h"
#include "Lanes.h"
#include "ProjectInterop.h"
#include "TrackModes.h"
#include "NoteVibrato.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include "Platform.h"
#include "Json.h"
#include "NoteBend.h"
#include "Params.h"
#include "PatternAutomation.h"
#include "PluginParams.h"
#include "PluginStandIn.h"
#include "../host/CrossFormat.h"
#include "TrackIconSet.h"
#include "TrackModes.h"
#include "NoteVibrato.h"
#include "StageRooms.h"

namespace gl {

namespace {
thread_local int tNoRackLaneMigration = 0;
}
NoRackLaneMigration::NoRackLaneMigration() { ++tNoRackLaneMigration; }
NoRackLaneMigration::~NoRackLaneMigration() { --tNoRackLaneMigration; }


namespace {
Json followToJson(const FollowSpec& f);  // grid2-1118 (below)
FollowSpec followFromJson(const Json* j);

// J-46: tick values read from a project stay within ±2^42 (≈ 4.4e12 ticks,
// far past any song) so that start + length sums can never overflow int64
// (a fuzzed project with start = INT64_MAX overflowed in the FLP exporter).
Tick jsonTick(double v) {
  constexpr double kLim = 4398046511104.0;  // 2^42
  return jsonInt<Tick>(std::isnan(v) ? 0.0 : std::clamp(v, -kLim, kLim));
}

Json num(double v) { return Json::makeNumber(v); }
Json str(const std::string& v) { return Json::makeString(v); }

Json* member(Json& j, const char* key) {
  auto it = j.object.find(key);
  return it == j.object.end() ? nullptr : &it->second;
}

bool fail(std::string* err, const std::string& msg) {
  if (err) *err = msg;
  return false;
}

// Base64 (RFC 4648) for binary blobs kept in the project (FL origin data, J-34).
std::string b64Encode(const std::string& in) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
    out += k[v >> 18], out += k[(v >> 12) & 63], out += k[(v >> 6) & 63], out += k[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    out += k[v >> 18], out += k[(v >> 12) & 63], out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
    out += k[v >> 18], out += k[(v >> 12) & 63], out += k[(v >> 6) & 63], out += '=';
  }
  return out;
}
bool b64Decode(const std::string& in, std::string& out) {
  out.clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=') break;
    else return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return true;
}

// v1 → v2: assign IDs in list order, turn index references into ID references.
void migrateV1(Json& j) {
  ObjectId next = kFirstObjectId;
  std::vector<ObjectId> chanIds, patIds;
  if (Json* chans = member(j, "channels"); chans && chans->is(Json::Type::Array))
    for (auto& c : chans->array) {
      chanIds.push_back(next);
      c.object["id"] = num(next++);
    }
  auto remapRows = [&](Json& rows) {
    if (!rows.is(Json::Type::Object)) return;
    std::map<std::string, Json> out;
    for (auto& [k, v] : rows.object) {
      int idx = std::atoi(k.c_str());
      if (idx >= 0 && idx < static_cast<int>(chanIds.size())) out[std::to_string(chanIds[static_cast<size_t>(idx)])] = v;
    }
    rows.object = std::move(out);
  };
  if (Json* pats = member(j, "patterns"); pats && pats->is(Json::Type::Array))
    for (auto& p : pats->array) {
      patIds.push_back(next);
      p.object["id"] = num(next++);
      if (auto it = p.object.find("steps"); it != p.object.end()) remapRows(it->second);
      if (auto it = p.object.find("notes"); it != p.object.end()) remapRows(it->second);
    }
  if (Json* clips = member(j, "playlist"); clips && clips->is(Json::Type::Array))
    for (auto& c : clips->array) {
      int idx = jsonInt<int>(c.num("pattern", -1));
      c.object["pattern"] = num(idx >= 0 && idx < static_cast<int>(patIds.size()) ? patIds[static_cast<size_t>(idx)] : 0);
      c.object["id"] = num(next++);
    }
  Json info = Json::makeObject();
  info.object["title"] = str(j.text("title", "Untitled"));
  j.object.erase("title");
  j.object["info"] = std::move(info);
  j.object["ppq"] = num(kDefaultPpq);
  j.object["nextId"] = num(next);
  j.object["version"] = num(2);
}

// v2 → v3: generator numbers become plugin states; mixer and playlist tracks appear.
void migrateV2(Json& j) {
  static const char* kTypes[] = {"gl.synth", "gl.kick", "gl.hat"};
  ObjectId next = jsonInt<ObjectId>(j.num("nextId", kFirstObjectId));
  Json mixer = Json::makeArray();
  ObjectId master = next++;
  {
    Json m = Json::makeObject();
    m.object["id"] = num(master);
    m.object["name"] = str("Master");
    mixer.array.push_back(std::move(m));
  }
  for (int i = 0; i < kDefaultInsertTracks; ++i) {
    Json m = Json::makeObject();
    m.object["id"] = num(next++);
    Json sends = Json::makeArray();
    Json s = Json::makeObject();
    s.object["to"] = num(master);
    s.object["level"] = num(1);
    sends.array.push_back(std::move(s));
    m.object["sends"] = std::move(sends);
    mixer.array.push_back(std::move(m));
  }
  if (Json* chans = member(j, "channels"); chans && chans->is(Json::Type::Array))
    for (auto& c : chans->array) {
      int g = jsonInt<int>(c.num("generator", 0));
      Json p = Json::makeObject();
      p.object["type"] = str(kTypes[g >= 0 && g < 3 ? g : 0]);
      c.object["plugin"] = std::move(p);
      c.object.erase("generator");
      c.object["mixerTrack"] = num(master);
    }
  j.object["mixer"] = std::move(mixer);
  j.object["nextId"] = num(next);
  j.object["version"] = num(3);
}

template <typename F>
void forRows(const Json& pat, const char* key, F&& fn) {
  const Json* m = pat.find(key);
  if (!m || !m->is(Json::Type::Object)) return;
  for (const auto& [k, v] : m->object) fn(static_cast<ObjectId>(std::strtoul(k.c_str(), nullptr, 10)), v);
}

Json boolean(bool v) {
  Json j;
  j.type = Json::Type::Bool;
  j.boolean = v;
  return j;
}
bool flag(const Json& o, const char* key, bool dflt = false) {
  const Json* v = o.find(key);
  if (!v) return dflt;
  if (v->is(Json::Type::Bool)) return v->boolean;
  if (v->is(Json::Type::Number)) return v->number != 0;
  return dflt;
}
Rgb color(const Json& o, Rgb dflt) {
  double v = o.num("color", -1);
  return v >= 0 && v <= 0xFFFFFF ? static_cast<Rgb>(v) : dflt;
}

Json pluginToJson(const PluginState& p) {
  Json j = Json::makeObject();
  j.object["type"] = str(p.type);
  Json params = Json::makeArray();
  for (float v : p.params) params.array.push_back(num(v));
  j.object["params"] = std::move(params);
  if (!p.data.empty()) j.object["data"] = str(p.data);
  if (!p.ins.empty()) j.object["ins"] = str(p.ins);
  if (!p.original.empty()) {  // stand-ins: the plugin they replace (PluginStandIn.h)
    Json o;
    if (jsonParse(p.original, o) && o.is(Json::Type::Object)) j.object["original"] = std::move(o);
  }
  // K-78: who a third-party plugin is, in every format it was installed in (host/CrossFormat.h).
  if (Json id = host::xformat::identityForSave(p); id.is(Json::Type::Object)) j.object["ident"] = std::move(id);
  if (!p.shown.empty()) {  // K-112c: the parameters a device row shows (only when chosen)
    Json sh = Json::makeArray();
    for (int k : p.shown) sh.array.push_back(num(k));
    j.object["shown"] = std::move(sh);
  }
  if (!p.warp.empty()) {  // a sample's warp markers (core/SampleWarp.h)
    Json w;
    if (jsonParse(p.warp, w) && w.is(Json::Type::Object)) j.object["warp"] = std::move(w);
  }
  if (!p.loupeWarp.empty()) {  // Loupe's warp markers not applied yet (core/SampleWarp.h)
    Json w;
    if (jsonParse(p.loupeWarp, w) && w.is(Json::Type::Object)) j.object["loupeWarp"] = std::move(w);
  }
  return j;
}

// J-211: a track / channel icon. Projects saved before Gridline showed FL's
// icons have only the FL number: the matching Gridline icon is shown for it.
int iconFromJson(const Json& j, int flIcon) {
  if (!j.find("icon")) return trackicon::fromFl(flIcon);
  const double v = j.num("icon", 0);  // an id out of range is no icon (not the nearest one)
  const int ic = v >= 1 && v <= trackicon::maxId() ? jsonInt<int>(v) : 0;
  return trackicon::info(ic) ? ic : 0;
}

// K-87: a freeze record (absent = not frozen).
Json freezeToJson(const FreezeRecord& f) {
  Json j = Json::makeObject();
  j.object["render"] = pluginToJson(f.render);
  j.object["fingerprint"] = str(f.fingerprint);
  j.object["sampleRate"] = num(f.sampleRate);
  if (f.arrangement) j.object["arrangement"] = num(f.arrangement);
  j.object["seconds"] = num(f.seconds);
  if (f.latency) j.object["latency"] = num(f.latency);
  return j;
}

// K-169 / K-245: the eight macro knobs of a channel or a mixer track (Null: all unnamed at 0, not written).
Json macrosToJson(const std::array<LayerMacro, kLayerMacros>& ms) {
  bool any = false;
  for (const LayerMacro& m : ms) any = any || !m.name.empty() || m.value != 0;
  if (!any) return Json();
  Json jm = Json::makeArray();
  for (const LayerMacro& m : ms) {
    Json o = Json::makeObject();
    if (!m.name.empty()) o.object["name"] = Json::makeString(m.name);
    if (m.value != 0) o.object["value"] = Json::makeNumber(m.value);
    jm.array.push_back(std::move(o));
  }
  return jm;
}

void macrosFromJson(const Json* jm, std::array<LayerMacro, kLayerMacros>& ms) {
  ms = {};
  if (!jm || !jm->is(Json::Type::Array)) return;
  for (size_t k = 0; k < jm->array.size() && k < static_cast<size_t>(kLayerMacros); ++k) {
    const Json& o = jm->array[k];
    if (!o.is(Json::Type::Object)) continue;
    ms[k].name = o.text("name").substr(0, 24);
    ms[k].value = static_cast<float>(std::clamp(o.num("value", 0), 0.0, 1.0));
  }
}

FreezeRecord freezeFromJson(const Json* j) {
  FreezeRecord f;
  if (!j || !j->is(Json::Type::Object)) return f;
  const Json* r = j->find("render");
  if (!r || !r->is(Json::Type::Object)) return f;
  f.render.type = r->text("type");
  f.render.data = r->text("data");
  if (f.render.type.empty() || f.render.data.empty()) return {};
  f.fingerprint = j->text("fingerprint");
  f.sampleRate = jsonInt<int>(std::clamp(j->num("sampleRate", 0), 0.0, 1e6));
  f.arrangement = jsonInt<int>(std::clamp(j->num("arrangement", 0), 0.0, 1e6));
  f.latency = jsonInt<int>(std::clamp(j->num("latency", 0), 0.0, 1e7));
  f.seconds = std::clamp(j->num("seconds", 0), 0.0, 1e7);
  return f;
}

// ---- Channel note settings, layers, display groups and per-note properties.
// Every field is written only when it differs from its default, so projects
// that do not use them are unchanged and older readers ignore the extra keys
// (no schema bump needed).
// The channel pitch in cents: "pitchCents" (0.7.7 on), else "pitch" in whole semitones (older
// projects, older collaborators) times 100.
float channelPitchFromJson(const Json& jc) {
  double cents = jc.find("pitchCents") ? jc.num("pitchCents", 0) : std::clamp(jc.num("pitch", 0), -24.0, 24.0) * 100.0;
  if (!std::isfinite(cents)) cents = 0;
  return static_cast<float>(std::clamp(cents, -kChannelPitchMaxCents, kChannelPitchMaxCents));
}

void channelExtrasToJson(const Channel& c, Json& jc) {
  const Channel d;
  if (c.transpose != d.transpose) jc.object["transpose"] = num(c.transpose);
  if (c.fine != d.fine) jc.object["fine"] = num(c.fine);
  if (c.bendRange != d.bendRange) jc.object["bendRange"] = num(c.bendRange);
  if (c.swingMix != d.swingMix) jc.object["swingMix"] = num(c.swingMix);
  if (c.timeShift != d.timeShift) jc.object["timeShift"] = num(c.timeShift);
  if (c.polyphony != d.polyphony) jc.object["polyphony"] = num(c.polyphony);
  if (c.mono) jc.object["mono"] = boolean(true);
  if (c.porta) jc.object["porta"] = boolean(true);
  if (c.portaTime != d.portaTime) jc.object["portaTime"] = num(c.portaTime);
  if (!c.expr.neutral()) {  // K-144
    Json e = Json::makeObject();
    if (!c.expr.pitch) e.object["pitch"] = boolean(false);
    if (c.expr.pressure != ExprUse::Instrument) e.object["pressure"] = num(static_cast<int>(c.expr.pressure));
    if (c.expr.timbre != ExprUse::Instrument) e.object["timbre"] = num(static_cast<int>(c.expr.timbre));
    if (c.expr.amount != 1.0f) e.object["amount"] = num(c.expr.amount);
    jc.object["expression"] = e;
  }
  if (c.cutGroup) jc.object["cutGroup"] = num(c.cutGroup);
  if (c.cutBy) jc.object["cutBy"] = num(c.cutBy);
  if (!(c.echo == d.echo)) {
    Json e = Json::makeObject();
    e.object["count"] = num(c.echo.count);
    e.object["feedback"] = num(c.echo.feedback);
    e.object["time"] = num(c.echo.time);
    e.object["pan"] = num(c.echo.pan);
    e.object["pitch"] = num(c.echo.pitch);
    e.object["pingPong"] = boolean(c.echo.pingPong);
    if (c.echo.modX != 0) e.object["modX"] = num(c.echo.modX);
    if (c.echo.modY != 0) e.object["modY"] = num(c.echo.modY);
    jc.object["echo"] = std::move(e);
  }
  if (!c.layer.empty()) {
    Json l = Json::makeArray();
    for (ObjectId k : c.layer) l.array.push_back(num(k));
    jc.object["layer"] = std::move(l);
    if (c.layerMode) jc.object["layerMode"] = num(c.layerMode);
    Json zs = Json::makeArray();  // K-169: zones that differ from the full keyboard
    for (const LayerZone& z : c.layerZones) {
      if (z.full() || std::find(c.layer.begin(), c.layer.end(), z.child) == c.layer.end()) continue;
      const LayerZone d;
      Json jz = Json::makeObject();
      jz.object["child"] = num(z.child);
      auto put = [&](const char* k, int v, int def) {
        if (v != def) jz.object[k] = num(v);
      };
      put("keyLo", z.keyLo, d.keyLo);
      put("keyHi", z.keyHi, d.keyHi);
      put("keyFadeLo", z.keyFadeLo, 0);
      put("keyFadeHi", z.keyFadeHi, 0);
      put("velLo", z.velLo, d.velLo);
      put("velHi", z.velHi, d.velHi);
      put("velFadeLo", z.velFadeLo, 0);
      put("velFadeHi", z.velFadeHi, 0);
      put("transpose", z.transpose, 0);
      if (z.solo) jz.object["solo"] = boolean(true);
      zs.array.push_back(std::move(jz));
    }
    if (!zs.array.empty()) jc.object["layerZones"] = std::move(zs);
  }
  if (Json jm = macrosToJson(c.macros); !jm.is(Json::Type::Null)) jc.object["macros"] = std::move(jm);  // K-169; K-245: every channel
  if (c.group >= 0) jc.object["group"] = num(c.group);
  if (!c.fullKeyRange()) {  // J-31 key range
    jc.object["keyLo"] = num(c.keyLo);
    jc.object["keyHi"] = num(c.keyHi);
  }
  if (c.swingTruncate) jc.object["swingTruncate"] = boolean(true);
  if (c.audioClip) jc.object["audioClip"] = boolean(true);  // J-39
  if (c.flIcon) jc.object["flIcon"] = num(c.flIcon);
  if (c.icon || c.flIcon) jc.object["icon"] = num(c.icon);  // J-211
  if (c.freeze.frozen()) jc.object["freeze"] = freezeToJson(c.freeze);  // K-87
  if (c.hibernate) jc.object["hibernate"] = boolean(true);
  if (c.keepRunning) jc.object["keepRunning"] = boolean(true);  // K-277
  if (c.rackHidden) jc.object["rackHidden"] = boolean(true);  // GitHub #58
  if (!c.automationLanes.empty()) {  // K-413: persistent channel automation lanes
    Json arr = Json::makeArray();
    for (const ChannelAutoLane& l : c.automationLanes) {
      Json jl = Json::makeObject();
      jl.object["index"] = num(l.index);
      if (!l.name.empty()) jl.object["name"] = str(l.name);
      if (l.color != AutomationClip{}.color) jl.object["color"] = num(l.color);
      arr.array.push_back(std::move(jl));
    }
    jc.object["automationLanes"] = std::move(arr);
  }
  if (!c.notes.empty()) jc.object["notes"] = str(c.notes);  // K-128
}

void channelExtrasFromJson(const Json& jc, Channel& c) {
  c.transpose = jsonInt<int>(std::clamp(jc.num("transpose", 0), -static_cast<double>(kChannelTransposeMax), static_cast<double>(kChannelTransposeMax)));
  c.fine = jsonInt<int>(std::clamp(jc.num("fine", 0), -200.0, 200.0));
  c.bendRange = jsonInt<int>(std::clamp(jc.num("bendRange", 2), 1.0, static_cast<double>(kChannelBendRangeMax)));
  // K-132 projects (FL import before 0.7.7): an automated pitch knob kept whole semitones in "pitch"
  // and its cents beyond them in "knobFine", also counted in "fine". The knob now holds cents: its
  // cents move from the fine tune to the pitch (the same sound, and automation still replaces it).
  if (!jc.find("pitchCents") && flag(jc, "pitchSmooth")) {
    const int knobFine = jsonInt<int>(std::clamp(jc.num("knobFine", 0), -10000.0, 10000.0));
    c.pitchCents = static_cast<float>(std::clamp(static_cast<double>(c.pitchCents) + knobFine, -kChannelPitchMaxCents, kChannelPitchMaxCents));
    c.fine = std::clamp(c.fine - knobFine, -200, 200);
  }
  c.swingMix = static_cast<float>(std::clamp(jc.num("swingMix", 1), 0.0, 1.0));
  c.timeShift = static_cast<float>(std::clamp(jc.num("timeShift", 0), 0.0, 1.0));
  c.polyphony = jsonInt<int>(std::clamp(jc.num("polyphony", 0), 0.0, 256.0));
  c.mono = flag(jc, "mono");
  c.porta = flag(jc, "porta");
  c.portaTime = static_cast<float>(std::clamp(jc.num("portaTime", 0.1), 0.0, 2.0));
  if (const Json* e = jc.find("expression"); e && e->is(Json::Type::Object)) {  // K-144
    auto use = [&](const char* k) { return static_cast<ExprUse>(jsonInt<int>(std::clamp(e->num(k, 0), 0.0, 3.0))); };
    c.expr.pitch = flag(*e, "pitch", true);
    c.expr.pressure = use("pressure");
    c.expr.timbre = use("timbre");
    c.expr.amount = static_cast<float>(std::clamp(e->num("amount", 1), 0.0, 1.0));
  }
  c.cutGroup = jsonInt<int>(std::clamp(jc.num("cutGroup", 0), 0.0, static_cast<double>(kMaxCutGroups)));
  c.cutBy = jsonInt<int>(std::clamp(jc.num("cutBy", 0), 0.0, static_cast<double>(kMaxCutGroups)));
  if (const Json* e = jc.find("echo"); e && e->is(Json::Type::Object)) {
    c.echo.count = jsonInt<int>(std::clamp(e->num("count", 0), 0.0, 10.0));
    c.echo.feedback = static_cast<float>(std::clamp(e->num("feedback", 0.5), 0.0, 1.0));
    c.echo.time = static_cast<float>(std::clamp(e->num("time", 0.375), 0.01, 16.0));
    c.echo.pan = static_cast<float>(std::clamp(e->num("pan", 0), -1.0, 1.0));
    c.echo.pitch = jsonInt<int>(std::clamp(e->num("pitch", 0), -1200.0, 1200.0));
    c.echo.pingPong = flag(*e, "pingPong");
    c.echo.modX = static_cast<float>(std::clamp(e->num("modX", 0), -1.0, 1.0));
    c.echo.modY = static_cast<float>(std::clamp(e->num("modY", 0), -1.0, 1.0));
  }
  if (const Json* l = jc.find("layer"); l && l->is(Json::Type::Array))
    for (const auto& v : l->array)
      if (v.is(Json::Type::Number) && v.number >= kFirstObjectId && c.layer.size() < static_cast<size_t>(kMaxLayerChildren))
        c.layer.push_back(jsonInt<ObjectId>(v.number));
  c.layerMode = jsonInt<int>(std::clamp(jc.num("layerMode", 0), 0.0, static_cast<double>(kLayerModeCount - 1)));
  if (const Json* zs = jc.find("layerZones"); zs && zs->is(Json::Type::Array))  // K-169
    for (const auto& jz : zs->array) {
      if (!jz.is(Json::Type::Object)) continue;
      LayerZone z;
      z.child = jsonInt<ObjectId>(std::max(0.0, jz.num("child", 0)));
      if (z.child < kFirstObjectId || std::find(c.layer.begin(), c.layer.end(), z.child) == c.layer.end() || c.layerZone(z.child)) continue;
      auto get = [&](const char* k, int def, int lo, int hi) { return jsonInt<int>(std::clamp(jz.num(k, def), static_cast<double>(lo), static_cast<double>(hi))); };
      z.keyLo = get("keyLo", 0, 0, 127);
      z.keyHi = get("keyHi", 127, 0, 127);
      if (z.keyHi < z.keyLo) std::swap(z.keyLo, z.keyHi);
      z.keyFadeLo = get("keyFadeLo", 0, 0, 127);
      z.keyFadeHi = get("keyFadeHi", 0, 0, 127);
      z.velLo = get("velLo", 0, 0, 127);
      z.velHi = get("velHi", 127, 0, 127);
      if (z.velHi < z.velLo) std::swap(z.velLo, z.velHi);
      z.velFadeLo = get("velFadeLo", 0, 0, 127);
      z.velFadeHi = get("velFadeHi", 0, 0, 127);
      z.transpose = get("transpose", 0, -48, 48);
      z.solo = flag(jz, "solo");
      if (!z.full()) c.layerZones.push_back(z);
    }
  macrosFromJson(jc.find("macros"), c.macros);
  c.group = jsonInt<int>(std::clamp(jc.num("group", -1), -1.0, 1e6));
  c.keyLo = jsonInt<int>(std::clamp(jc.num("keyLo", 0), 0.0, 127.0));  // J-31
  c.keyHi = jsonInt<int>(std::clamp(jc.num("keyHi", 127), 0.0, 127.0));
  if (c.keyHi < c.keyLo) std::swap(c.keyLo, c.keyHi);
  c.swingTruncate = flag(jc, "swingTruncate");
  c.audioClip = flag(jc, "audioClip");
  c.flIcon = jsonInt<int>(std::clamp(jc.num("flIcon", 0), 0.0, 65535.0));
  c.icon = iconFromJson(jc, c.flIcon);  // J-211
  c.freeze = freezeFromJson(jc.find("freeze"));  // K-87
  c.hibernate = flag(jc, "hibernate");
  c.keepRunning = flag(jc, "keepRunning");  // K-277
  c.rackHidden = flag(jc, "rackHidden");  // GitHub #58
  c.automationLanes.clear();  // K-413
  if (const Json* al = jc.find("automationLanes"); al && al->is(Json::Type::Array)) {
    for (const Json& jl : al->array) {
      if (!jl.is(Json::Type::Object) || c.automationLanes.size() >= 256) continue;
      ChannelAutoLane l;
      l.index = jsonInt<uint32_t>(std::max(0.0, jl.num("index", 0)));
      l.name = jl.text("name");
      l.color = color(jl, AutomationClip{}.color);
      bool dup = false;
      for (const auto& ex : c.automationLanes)
        if (ex.index == l.index) dup = true;
      if (!dup) c.automationLanes.push_back(std::move(l));
    }
  }
  c.notes = jc.text("notes");  // K-128
}

void noteToJson(const Note& n, Json& jn) {
  const Note d;
  if (n.pan != d.pan) jn.object["pan"] = num(n.pan);
  if (n.fine != d.fine) jn.object["fine"] = num(n.fine);
  if (n.release != d.release) jn.object["release"] = num(n.release);
  if (n.modX != d.modX) jn.object["modX"] = num(n.modX);
  if (n.modY != d.modY) jn.object["modY"] = num(n.modY);
  if (n.color != d.color) jn.object["color"] = num(n.color);
  if (n.slide) jn.object["slide"] = boolean(true);
  if (n.porta) jn.object["porta"] = boolean(true);
  if (n.program >= 0) jn.object["program"] = num(n.program);  // K-348
  if (n.muted) jn.object["muted"] = boolean(true);
  if (n.group > 0) jn.object["group"] = num(n.group);  // J-31
  if (n.frac != 0.f) jn.object["frac"] = num(n.frac);  // J-159
  if (n.endFrac != 0.f) jn.object["endFrac"] = num(n.endFrac);
  if (n.step) jn.object["step"] = boolean(true);  // K-22: a rack step in the piano roll
  // K-25: [[tick, semitones, curve, tension], ...] (curve / tension left out when 0); K-144: the
  // pressure (0..1) and timbre (-0.5..0.5 around the centre) lanes in the same form.
  auto line = [&](const std::vector<BendPoint>& pts, const char* key) {
    if (pts.empty()) return;
    Json b = Json::makeArray();
    for (const auto& p : pts) {
      Json jp = Json::makeArray();
      jp.array.push_back(num(p.tick));
      jp.array.push_back(num(p.semis));
      if (p.curve || p.tension != 0.f) jp.array.push_back(num(p.curve));
      if (p.tension != 0.f) jp.array.push_back(num(p.tension));  // K-28
      b.array.push_back(std::move(jp));
    }
    jn.object[key] = std::move(b);
  };
  line(n.bend, "bend");
  line(n.pressure, "pressure");
  line(n.timbre, "timbre");
  if (n.vib.active()) {  // K-262: the note's vibrato, the fields away from their defaults
    const NoteVibrato d;
    Json v = Json::makeObject();
    auto f = [&](const char* k, float x, float def) {
      if (x != def) v.object[k] = num(x);
    };
    f("depth", n.vib.depth, -1);
    f("rate", n.vib.rate, d.rate);
    if (n.vib.sync) v.object["sync"] = num(1);
    f("delay", n.vib.delay, d.delay);
    f("fadeIn", n.vib.fadeIn, d.fadeIn);
    f("fadeOut", n.vib.fadeOut, d.fadeOut);
    f("inCurve", n.vib.fadeInCurve, 0);
    f("outCurve", n.vib.fadeOutCurve, 0);
    f("shape", n.vib.shape, 0);
    f("phase", n.vib.phase, 0);
    f("rateEnd", n.vib.rateEnd, 0);
    f("centre", n.vib.centre, d.centre);
    jn.object["vib"] = std::move(v);
  }
  if (!n.tracker.empty()) {  // K-76: the note's tracker cells, as hex
    static const char* hex = "0123456789abcdef";
    std::string h;
    h.reserve(n.tracker.size() * 2);
    for (uint8_t b : n.tracker) h += hex[b >> 4], h += hex[b & 15];
    jn.object["trk"] = str(h);
  }
}

void noteFromJson(const Json& e, Note& n) {
  n.pan = jsonInt<int>(std::clamp(e.num("pan", 0), -static_cast<double>(kNotePanMax), static_cast<double>(kNotePanMax)));
  n.fine = jsonInt<int>(std::clamp(e.num("fine", 0), -static_cast<double>(kNoteFineMax), static_cast<double>(kNoteFineMax)));
  n.release = jsonInt<int>(std::clamp(e.num("release", kNoteReleaseDefault), 0.0, 127.0));
  n.modX = jsonInt<int>(std::clamp(e.num("modX", kNoteModCentre), 0.0, static_cast<double>(kNoteModMax)));
  n.modY = jsonInt<int>(std::clamp(e.num("modY", kNoteModCentre), 0.0, static_cast<double>(kNoteModMax)));
  n.program = jsonInt<int>(std::clamp(e.num("program", -1), -1.0, 127.0));  // K-348
  n.color = jsonInt<int>(std::clamp(e.num("color", 0), 0.0, static_cast<double>(kNoteColors - 1)));
  n.slide = flag(e, "slide");
  n.porta = flag(e, "porta");
  n.muted = flag(e, "muted");
  n.step = flag(e, "step");  // K-22
  n.group = jsonInt<int>(std::clamp(e.num("group", 0), 0.0, 1e9));  // J-31
  // J-159: sub-tick timing
  const double fr = e.num("frac", 0), ef = e.num("endFrac", 0);
  n.frac = std::isfinite(fr) && fr > 0 && fr < 1 ? static_cast<float>(fr) : 0.f;
  n.endFrac = std::isfinite(ef) && ef > -1 && ef < 1 ? static_cast<float>(ef) : 0.f;
  n.frac = std::min(n.frac, 0.99999994f);
  auto line = [&](std::vector<BendPoint>& pts, const char* key, double lo, double hi) {  // K-25, K-144
    pts.clear();
    const Json* b = e.find(key);
    if (!b || !b->is(Json::Type::Array)) return;
    for (const auto& jp : b->array) {
      if (!jp.is(Json::Type::Array) || jp.array.size() < 2) continue;
      auto at = [&](size_t i) { return i < jp.array.size() && jp.array[i].is(Json::Type::Number) ? jp.array[i].number : 0.0; };
      BendPoint p;
      p.tick = static_cast<float>(std::clamp(at(0), 0.0, 1e9));
      p.semis = static_cast<float>(std::clamp(at(1), lo, hi));
      p.curve = static_cast<uint8_t>(std::clamp(at(2), 0.0, 255.0));
      p.tension = static_cast<float>(std::clamp(at(3), -1.0, 1.0));  // K-28 (0 in older files)
      pts.push_back(p);
      if (pts.size() >= static_cast<size_t>(bend::kMaxPoints) * 2) break;
    }
    bend::normalize(pts);
  };
  line(n.bend, "bend", -1e3, 1e3);
  line(n.pressure, "pressure", 0.0, 1.0);
  line(n.timbre, "timbre", -0.5, 0.5);
  n.vib = NoteVibrato{};
  if (const Json* v = e.find("vib"); v && v->is(Json::Type::Object)) {  // K-262
    const NoteVibrato d;
    auto f = [&](const char* k, float def) {
      const double x = v->num(k, def);
      return std::isfinite(x) ? static_cast<float>(std::clamp(x, -1e6, 1e6)) : def;
    };
    n.vib.sync = flag(*v, "sync");
    n.vib.depth = f("depth", 0);
    n.vib.rate = f("rate", d.rate);
    n.vib.delay = f("delay", d.delay);
    n.vib.fadeIn = f("fadeIn", d.fadeIn);
    n.vib.fadeOut = f("fadeOut", d.fadeOut);
    n.vib.fadeInCurve = f("inCurve", 0);
    n.vib.fadeOutCurve = f("outCurve", 0);
    n.vib.shape = static_cast<uint8_t>(std::clamp(f("shape", 0), 0.0f, 255.0f));
    n.vib.phase = f("phase", 0);
    n.vib.rateEnd = f("rateEnd", 0);
    n.vib.centre = f("centre", d.centre);
    vib::clamp(n.vib);
  }
  n.tracker.clear();
  if (const Json* t = e.find("trk"); t && t->is(Json::Type::String) && t->str.size() % 2 == 0 && t->str.size() <= 2 * 65536) {  // K-76
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < t->str.size(); i += 2) {
      const int a = nib(t->str[i]), b = nib(t->str[i + 1]);
      if (a < 0 || b < 0) {
        n.tracker.clear();
        break;
      }
      n.tracker.push_back(static_cast<uint8_t>(a * 16 + b));
    }
  }
}
// ---- end of channel / note extras

PluginState pluginFromJson(const Json* j) {
  PluginState p;
  if (!j || !j->is(Json::Type::Object)) return p;
  p.type = j->text("type");
  p.data = j->text("data");
  p.ins = j->text("ins");
  if (const Json* o = j->find("original"); o && o->is(Json::Type::Object)) p.original = jsonCompact(*o);
  if (const Json* o = j->find("ident"); o && o->is(Json::Type::Object)) p.ident = jsonCompact(*o);
  if (const Json* o = j->find("warp"); o && o->is(Json::Type::Object)) p.warp = jsonCompact(*o);
  if (const Json* o = j->find("loupeWarp"); o && o->is(Json::Type::Object)) p.loupeWarp = jsonCompact(*o);
  if (const Json* a = j->find("params"); a && a->is(Json::Type::Array))
    for (const auto& v : a->array) p.params.push_back(static_cast<float>(v.is(Json::Type::Number) ? v.number : 0));
  if (const Json* a = j->find("shown"); a && a->is(Json::Type::Array))  // K-112c
    for (const auto& v : a->array)
      if (v.is(Json::Type::Number) && v.number >= -1 && v.number < 1e6) p.shown.push_back(static_cast<int>(v.number));
  normalizePluginState(p);
  return p;
}

// ---------------------------------------------------------------------------
// Arrangement: clip kinds (audio / automation), clip offsets and mute, audio
// clip settings, automation clips, song markers, playlist track heights.
// Every field is written only when it differs from its default, so projects
// that do not use these features serialize exactly as before.
void clipArrangementToJson(const Clip& c, Json& jc) {
  if (c.kind == ClipKind::Audio) jc.object["kind"] = str("audio");
  if (c.kind == ClipKind::Automation) jc.object["kind"] = str("automation");
  if (c.source != kNoId) jc.object["source"] = num(c.source);
  if (c.offset != 0) jc.object["offset"] = num(static_cast<double>(c.offset));
  if (c.loop) jc.object["loop"] = boolean(true);  // K-114
  if (c.loop && c.loopLength > 0) {  // K-140: a cut clip's own loop
    jc.object["loopStart"] = num(static_cast<double>(c.loopStart));
    jc.object["loopLength"] = num(static_cast<double>(c.loopLength));
  }
  if (c.muted) jc.object["muted"] = boolean(true);
  if (c.audioStart != 0) jc.object["audioStart"] = num(c.audioStart);
  if (c.audioEnd != 0) jc.object["audioEnd"] = num(c.audioEnd);
  if (c.fadeIn != 0) jc.object["fadeIn"] = num(c.fadeIn);
  if (c.fadeOut != 0) jc.object["fadeOut"] = num(c.fadeOut);
  if (c.gain != 1.0f) jc.object["gain"] = num(c.gain);
  if (c.pan != 0.0f) jc.object["pan"] = num(c.pan);  // K-414: per-clip stereo pan
  if (c.pitch != 0.0f) jc.object["pitch"] = num(c.pitch);
  if (c.stretch != 1.0) jc.object["stretch"] = num(c.stretch);
  if (c.speedPitch != 0.0f) jc.object["speedPitch"] = num(c.speedPitch);  // J-164
  if (c.rateFit != 1.0) jc.object["rateFit"] = num(c.rateFit);
  if (c.syncBpm != 0) jc.object["syncBpm"] = num(c.syncBpm);
  if (c.tempoLock) jc.object["tempoLock"] = boolean(true);  // J-193
  if (c.reverse) jc.object["reverse"] = boolean(true);
  if (c.takeGroup != kNoId) jc.object["takeGroup"] = num(c.takeGroup);
  if (c.take != 0) jc.object["take"] = num(c.take);
  if (c.takeRating != 0) jc.object["takeRating"] = num(c.takeRating);  // K-123
  if (c.launched) jc.object["launched"] = boolean(true);
  if (c.launchOnce) jc.object["launchOnce"] = boolean(true);  // K-112c checkpoint 54
  if (!c.launchSet.isDefault()) {  // grid2-1118: the clip's own launch settings and follow actions
    Json jl = Json::makeObject();
    if (c.launchSet.mode >= 0) {
      jl.object["mode"] = num(c.launchSet.mode);
    }
    if (c.launchSet.quant >= 0) {
      jl.object["quant"] = num(c.launchSet.quant);
    }
    if (c.launchSet.legato) {
      jl.object["legato"] = boolean(true);
    }
    if (!c.launchSet.follow.isDefault()) {
      jl.object["follow"] = followToJson(c.launchSet.follow);
    }
    jc.object["launchSet"] = std::move(jl);
  }
  if (c.group != kNoId) jc.object["group"] = num(c.group);  // J-31
  // J-38: fade curves and hand-set fades (automatic crossfades are derived, not saved).
  if (c.fadeInShape != 0) jc.object["fadeInShape"] = num(c.fadeInShape);
  if (c.fadeOutShape != 0) jc.object["fadeOutShape"] = num(c.fadeOutShape);
  if (c.fadeInTension != 0.0f) jc.object["fadeInTension"] = num(c.fadeInTension);
  if (c.fadeOutTension != 0.0f) jc.object["fadeOutTension"] = num(c.fadeOutTension);
  if (c.fadeInManual) jc.object["fadeInManual"] = boolean(true);
  if (c.fadeOutManual) jc.object["fadeOutManual"] = boolean(true);
  if (!c.name.empty()) jc.object["name"] = str(c.name);  // R07-01
  if (c.fadeInDeclick != 0) jc.object["fadeInDeclick"] = num(c.fadeInDeclick);  // J-43
  if (c.fadeOutDeclick != 0) jc.object["fadeOutDeclick"] = num(c.fadeOutDeclick);
  // K-112b3: pattern clip playback (written only when set).
  if (c.playTranspose != 0) jc.object["playTranspose"] = num(c.playTranspose);
  if (c.playVelocity != 0) jc.object["playVelocity"] = num(c.playVelocity);
  if (c.playDelay != 0) jc.object["playDelay"] = num(static_cast<double>(c.playDelay));
  if (c.playQuantize != 0) jc.object["playQuantize"] = num(static_cast<double>(c.playQuantize));
  if (c.kind == ClipKind::Audio && !c.warp.empty()) {  // K-85: warp markers (keyed by id: collaboration merges per marker)
    Json arr = Json::makeArray();
    for (const WarpMarker& m : c.warp) {
      Json jm = Json::makeObject();
      jm.object["id"] = num(m.id);
      jm.object["at"] = num(m.at);
      jm.object["to"] = num(m.to);
      arr.array.push_back(std::move(jm));
    }
    jc.object["warp"] = std::move(arr);
  }
}

void clipArrangementFromJson(const Json& jc, Clip& c) {
  std::string k = jc.text("kind", "pattern");
  c.kind = k == "audio" ? ClipKind::Audio : k == "automation" ? ClipKind::Automation : ClipKind::Pattern;
  c.source = jsonInt<ObjectId>(std::max(0.0, jc.num("source", 0)));
  c.offset = std::max<Tick>(0, jsonTick(jc.num("offset", 0)));
  c.loop = flag(jc, "loop");  // K-114 (projects saved before it: songFromJson, adoptOldClipLoops)
  if (c.loop && c.kind != ClipKind::Audio) {  // K-140
    c.loopLength = std::max<Tick>(0, jsonTick(jc.num("loopLength", 0)));
    c.loopStart = c.loopLength > 0 ? std::max<Tick>(0, jsonTick(jc.num("loopStart", 0))) : 0;
  }
  c.muted = flag(jc, "muted");
  c.audioStart = std::max(0.0, jc.num("audioStart", 0));
  c.audioEnd = std::max(0.0, jc.num("audioEnd", 0));
  c.fadeIn = std::clamp(jc.num("fadeIn", 0), 0.0, 3600.0);
  c.fadeOut = std::clamp(jc.num("fadeOut", 0), 0.0, 3600.0);
  c.gain = clipgain::clampGain(static_cast<float>(jc.num("gain", 1)));  // J-79: FL's range, up to +36 dB
  c.pan = static_cast<float>(std::clamp(jc.num("pan", 0.0), -1.0, 1.0));  // K-414
  if (!std::isfinite(c.pan)) c.pan = 0.0f;
  c.pitch = static_cast<float>(std::clamp(jc.num("pitch", 0), -4800.0, 4800.0));
  c.stretch = std::clamp(jc.num("stretch", 1), 0.0625, 16.0);
  c.speedPitch = static_cast<float>(std::clamp(jc.num("speedPitch", 0), -6000.0, 6000.0));  // J-164 (clip + channel pitch shift)
  c.rateFit = std::clamp(jc.num("rateFit", 1), 1.0 / 1024.0, 1024.0);
  c.syncBpm = std::clamp(jc.num("syncBpm", 0), 0.0, 1000.0);
  c.tempoLock = c.syncBpm > 0 && flag(jc, "tempoLock");  // J-193
  c.reverse = flag(jc, "reverse");
  c.takeGroup = jsonInt<ObjectId>(std::max(0.0, jc.num("takeGroup", 0)));
  c.take = jsonInt<int>(std::clamp(jc.num("take", 0), 0.0, 9999.0));
  c.takeRating = static_cast<uint8_t>(std::clamp(jc.num("takeRating", 0), 0.0, 3.0));  // K-123
  c.launched = flag(jc, "launched");
  c.launchOnce = flag(jc, "launchOnce");  // K-112c checkpoint 54
  c.launchSet = ClipLaunch{};
  if (const Json* jl = jc.find("launchSet"); jl && jl->is(Json::Type::Object)) {  // grid2-1118
    c.launchSet.mode = static_cast<int8_t>(jsonInt<int>(std::clamp(jl->num("mode", -1), -1.0, static_cast<double>(static_cast<int>(PadMode::Count) - 1))));
    c.launchSet.quant = static_cast<int8_t>(jsonInt<int>(std::clamp(jl->num("quant", -1), -1.0, static_cast<double>(static_cast<int>(LaunchGrid::Count) - 1))));
    c.launchSet.legato = flag(*jl, "legato");
    c.launchSet.follow = followFromJson(jl->find("follow"));
  }
  c.group = jsonInt<ObjectId>(std::max(0.0, jc.num("group", 0)));  // J-31
  // J-38
  // J-43: shapes 0 smooth / 1 straight (J-38 files: 1 linear = straight, 2 and 3 play smooth);
  // tension in FL's unit.
  c.fadeInShape = jc.num("fadeInShape", 0) == 1.0 ? 1 : 0;
  c.fadeOutShape = jc.num("fadeOutShape", 0) == 1.0 ? 1 : 0;
  c.fadeInTension = static_cast<float>(std::clamp(jc.num("fadeInTension", 0), -1e7, 1e7));
  c.fadeOutTension = static_cast<float>(std::clamp(jc.num("fadeOutTension", 0), -1e7, 1e7));
  c.fadeInDeclick = static_cast<uint8_t>(jsonInt<int>(std::clamp(jc.num("fadeInDeclick", 0), 0.0, 7.0)));
  c.fadeOutDeclick = static_cast<uint8_t>(jsonInt<int>(std::clamp(jc.num("fadeOutDeclick", 0), 0.0, 7.0)));
  c.fadeInManual = flag(jc, "fadeInManual");
  c.fadeOutManual = flag(jc, "fadeOutManual");
  c.name = jc.text("name");  // R07-01
  c.playTranspose = static_cast<int>(std::lround(std::clamp(jc.num("playTranspose", 0), -48.0, 48.0)));  // K-112b3
  c.playVelocity = static_cast<int>(std::lround(std::clamp(jc.num("playVelocity", 0), -127.0, 127.0)));
  c.playDelay = static_cast<Tick>(std::llround(std::clamp(jc.num("playDelay", 0), -1.0e6, 1.0e6)));
  c.playQuantize = static_cast<Tick>(std::llround(std::clamp(jc.num("playQuantize", 0), 0.0, 1.0e6)));
  c.warp.clear();  // K-85
  if (const Json* w = jc.find("warp"); w && w->is(Json::Type::Array) && c.kind == ClipKind::Audio) {
    for (const Json& e : w->array) {
      if (!e.is(Json::Type::Object) || c.warp.size() >= 4 * kMaxWarpMarkers) continue;
      WarpMarker m;
      m.id = jsonInt<uint32_t>(std::clamp(e.num("id", 0), 0.0, 4294967295.0));
      m.at = e.num("at", std::nan(""));
      m.to = e.num("to", std::nan(""));
      c.warp.push_back(m);
    }
    normalizeWarp(c.warp);
  }
}

void arrangementToJson(const Song& s, Json& j) {
  if (!s.automations.empty()) {
    Json arr = Json::makeArray();
    for (const auto& a : s.automations) {
      Json ja = Json::makeObject();
      ja.object["id"] = num(a.id);
      ja.object["name"] = str(a.name);
      ja.object["color"] = num(a.color);
      ja.object["owner"] = num(a.owner);
      ja.object["index"] = num(a.index);
      if (a.min != 0.0f) ja.object["min"] = num(a.min);
      if (a.max != 1.0f) ja.object["max"] = num(a.max);
      if (a.loop > 0) ja.object["loop"] = num(static_cast<double>(a.loop));
      if (a.flCurves) ja.object["flCurves"] = boolean(true);  // J-41 (absent: the pre-J-41 curves)
      if (a.events) ja.object["events"] = boolean(true);      // J-210
      Json pts = Json::makeArray();
      for (const auto& p : a.points) {
        Json jp = Json::makeArray();  // [pos, value, tension, shape(, frac(, fine))]
        jp.array.push_back(num(static_cast<double>(p.pos)));
        jp.array.push_back(num(p.value));
        jp.array.push_back(num(p.tension));
        jp.array.push_back(num(static_cast<int>(p.shape)));
        if (p.frac != 0.f || p.fine != 0.0) jp.array.push_back(num(p.frac));  // J-159: sub-tick position
        if (p.fine != 0.0) jp.array.push_back(num(p.fine));                    // J-163: exact value
        pts.array.push_back(std::move(jp));
      }
      ja.object["points"] = std::move(pts);
      arr.array.push_back(std::move(ja));
    }
    j.object["automations"] = std::move(arr);
  }
  if (!s.markers.empty()) {
    Json arr = Json::makeArray();
    for (const auto& m : s.markers) {
      Json jm = Json::makeObject();
      jm.object["pos"] = num(static_cast<double>(m.pos));
      jm.object["name"] = str(m.name);
      if (m.timeSigNum > 0) {
        jm.object["timeSigNum"] = num(m.timeSigNum);
        jm.object["timeSigDen"] = num(m.timeSigDen);
      }
      if (m.action != MarkerAction::None) jm.object["action"] = num(static_cast<int>(m.action));
      if (m.isKey()) {  // J-31 key marker
        jm.object["keyRoot"] = num(m.keyRoot);
        jm.object["keyScale"] = num(m.keyScale);
      }
      if (!m.meterForPatterns) jm.object["meterForPatterns"] = boolean(false);  // K-179
      arr.array.push_back(std::move(jm));
    }
    j.object["markers"] = std::move(arr);
  }
}

// Automation clips need unique ids (shared with every other object kind).
bool arrangementFromJson(const Json& j, Song& s, const std::function<bool(const Json&, ObjectId&)>& takeId, std::string* err) {
  if (const Json* arr = j.find("automations"); arr && arr->is(Json::Type::Array))
    for (const auto& ja : arr->array) {
      AutomationClip a;
      if (!takeId(ja, a.id)) return fail(err, "automation clip has missing or duplicate id");
      automationClipFromJson(ja, a);
      s.automations.push_back(std::move(a));
    }
  if (const Json* arr = j.find("markers"); arr && arr->is(Json::Type::Array))
    for (const auto& jm : arr->array) {
      Marker m;
      m.pos = std::max<Tick>(0, jsonTick(jm.num("pos", 0)));
      m.name = jm.text("name");
      m.timeSigNum = jsonInt<int>(std::clamp(jm.num("timeSigNum", 0), 0.0, 32.0));
      m.timeSigDen = jsonInt<int>(std::clamp(jm.num("timeSigDen", 0), 0.0, 32.0));
      if (m.timeSigNum == 0 || m.timeSigDen == 0) m.timeSigNum = m.timeSigDen = 0;
      m.action = static_cast<MarkerAction>(jsonInt<int>(std::clamp(jm.num("action", 0), 0.0, static_cast<double>(static_cast<int>(MarkerAction::Count) - 1))));
      m.keyRoot = jsonInt<int>(std::clamp(jm.num("keyRoot", -1), -1.0, 11.0));  // J-31
      m.keyScale = jsonInt<int>(std::clamp(jm.num("keyScale", 0), 0.0, 64.0));
      m.meterForPatterns = flag(jm, "meterForPatterns", true);  // K-179
      s.markers.push_back(std::move(m));
    }
  std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
  return true;
}
// --------------------------------------------------------- end arrangement --

// ---------------------------------------------------------------------------
// Pattern automation, playlist track live settings and folding, live area,
// several arrangements. Written only when they differ from their defaults, so
// projects that do not use them serialize exactly as before (same schema: old
// readers ignore the extra keys).
Json pointsToJson(const std::vector<AutoPoint>& pts) {
  Json arr = Json::makeArray();
  for (const auto& p : pts) {
    Json jp = Json::makeArray();  // [pos, value, tension, shape(, frac(, fine))]
    jp.array.push_back(num(static_cast<double>(p.pos)));
    jp.array.push_back(num(p.value));
    jp.array.push_back(num(p.tension));
    jp.array.push_back(num(static_cast<int>(p.shape)));
    if (p.frac != 0.f || p.fine != 0.0) jp.array.push_back(num(p.frac));  // J-159: sub-tick position
    if (p.fine != 0.0) jp.array.push_back(num(p.fine));                    // J-163: exact value
    arr.array.push_back(std::move(jp));
  }
  return arr;
}

std::vector<AutoPoint> pointsFromJson(const Json* pts) {
  std::vector<AutoPoint> out;
  if (!pts || !pts->is(Json::Type::Array)) return out;
  for (const auto& jp : pts->array) {
    if (!jp.is(Json::Type::Array) || jp.array.size() < 2) continue;
    auto at = [&](size_t i, double d) { return i < jp.array.size() && jp.array[i].is(Json::Type::Number) ? jp.array[i].number : d; };
    AutoPoint p;
    p.pos = std::max<Tick>(0, jsonTick(at(0, 0)));
    p.value = static_cast<float>(std::clamp(at(1, 0.5), 0.0, 1.0));
    p.tension = static_cast<float>(std::clamp(at(2, 0), -1.0, 1.0));
    p.shape = static_cast<AutoShape>(static_cast<int>(std::clamp(at(3, 0), 0.0, static_cast<double>(static_cast<int>(AutoShape::Count) - 1))));
    if (const double fr = at(4, 0); fr > 0 && fr < 1) p.frac = std::min(static_cast<float>(fr), 0.99999994f);  // J-159
    if (const double fi = at(5, 0); std::isfinite(fi) && std::fabs(fi) < 1e-6) p.fine = fi;  // J-163
    out.push_back(p);
  }
  std::stable_sort(out.begin(), out.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.at() < y.at(); });
  return out;
}

// J-11: per-step parameters, written as {channel: [[step, pitch, pan, release, fine, modX, modY, shift], …]}
// for the steps that differ from the defaults.
void stepPropsToJson(const Pattern& p, Json& jp) {
  Json obj = Json::makeObject();
  for (const auto& [ch, row] : p.stepProps) {
    Json arr = Json::makeArray();
    for (size_t i = 0; i < row.size(); ++i) {
      const StepProps& sp = row[i];
      if (sp.isDefault()) continue;
      Json e = Json::makeArray();
      for (int v : {static_cast<int>(i), sp.pitch, sp.pan, sp.release, sp.fine, sp.modX, sp.modY, sp.shift}) e.array.push_back(num(v));
      arr.array.push_back(std::move(e));
    }
    if (!arr.array.empty()) obj.object[std::to_string(ch)] = std::move(arr);
  }
  if (!obj.object.empty()) jp.object["stepProps"] = std::move(obj);
}

void stepPropsFromJson(const Json& jp, Pattern& p, const Song& s) {
  const Json* obj = jp.find("stepProps");
  if (!obj || !obj->is(Json::Type::Object)) return;
  for (const auto& [key, arr] : obj->object) {
    ObjectId ch = static_cast<ObjectId>(std::strtoul(key.c_str(), nullptr, 10));
    if (!s.channel(ch) || !arr.is(Json::Type::Array)) continue;
    const size_t total = static_cast<size_t>(lanes::steps(p, ch));  // K-255: a row on its own grid has its own count
    std::vector<StepProps> row;
    for (const auto& e : arr.array) {
      if (!e.is(Json::Type::Array) || e.array.size() < 8) continue;
      auto at = [&](size_t i) { return e.array[i].is(Json::Type::Number) ? static_cast<int>(e.array[i].number) : 0; };
      const int idx = at(0);
      if (idx < 0 || static_cast<size_t>(idx) >= total) continue;
      if (row.size() <= static_cast<size_t>(idx)) row.resize(static_cast<size_t>(idx) + 1);
      StepProps& sp = row[static_cast<size_t>(idx)];
      sp.pitch = std::clamp(at(1), -1, 127);
      sp.pan = std::clamp(at(2), -kNotePanMax, kNotePanMax);
      sp.release = std::clamp(at(3), 0, 127);
      sp.fine = std::clamp(at(4), -kNoteFineMax, kNoteFineMax);
      sp.modX = std::clamp(at(5), 0, kNoteModMax);
      sp.modY = std::clamp(at(6), 0, kNoteModMax);
      sp.shift = std::clamp(at(7), 0, kStepShiftMax);
    }
    if (row.empty()) continue;
    row.resize(total);
    p.stepProps[ch] = std::move(row);
  }
}

void patternExtrasToJson(const Pattern& p, Json& jp) {
  stepPropsToJson(p, jp);
  if (!p.loop) jp.object["loop"] = boolean(false);  // Q-1: written only when off (the default is on)
  if (p.ownMeter()) {  // K-179: written only when the pattern has its own meter
    jp.object["meterNum"] = num(p.meterNum);
    jp.object["meterDen"] = num(p.meterDen);
  }
  if (p.linkChannel != kNoId) {  // 0.7.8: a linked pattern (written only when linked)
    Json lk = Json::makeObject();
    lk.object["channel"] = num(p.linkChannel);
    char hb[24];
    std::snprintf(hb, sizeof hb, "%016llx", static_cast<unsigned long long>(p.linkHash));
    lk.object["hash"] = str(hb);
    if (p.linkCycles != 1) lk.object["cycles"] = num(p.linkCycles);
    jp.object["link"] = std::move(lk);
  }
  if (!p.lanes.empty()) {  // K-255: lanes with a grid or mode of their own ({"grid": 5, "runOn": true})
    Json obj = Json::makeObject();
    for (const auto& [ch, l] : p.lanes) {
      if (l.isDefault()) continue;
      Json jl = Json::makeObject();
      if (l.grid > 0) jl.object["grid"] = num(l.grid);
      if (l.mode == LaneMode::RunOn) jl.object["runOn"] = boolean(true);
      obj.object[std::to_string(ch)] = std::move(jl);
    }
    if (!obj.object.empty()) jp.object["lanes"] = std::move(obj);
  }
  if (!p.stepLoop.empty()) {  // J-31: per-channel step loop lengths
    Json loops = Json::makeObject();
    for (const auto& [ch, n] : p.stepLoop)
      if (n > 0) loops.object[std::to_string(ch)] = num(n);
    if (!loops.object.empty()) jp.object["stepLoop"] = std::move(loops);
  }
  if (p.automation.empty()) return;
  Json arr = Json::makeArray();
  for (const auto& lane : p.automation) {
    Json jl = Json::makeObject();
    jl.object["owner"] = num(lane.owner);
    jl.object["index"] = num(lane.index);
    jl.object["points"] = pointsToJson(lane.points);
    // J-173: pattern automation clips; each key only when it differs from a plain lane's.
    if (!lane.name.empty()) jl.object["name"] = str(lane.name);
    if (lane.color != AutomationClip{}.color) jl.object["color"] = num(lane.color);
    if (lane.min != 0.0f) jl.object["min"] = num(lane.min);
    if (lane.max != 1.0f) jl.object["max"] = num(lane.max);
    if (lane.loop > 0) jl.object["loop"] = num(static_cast<double>(lane.loop));
    if (lane.flCurves) jl.object["flCurves"] = boolean(true);
    if (lane.events) jl.object["events"] = boolean(true);  // J-210
    arr.array.push_back(std::move(jl));
  }
  jp.object["automation"] = std::move(arr);
}

// Lanes whose target no longer exists, or that repeat a target, are dropped.
void patternExtrasFromJson(const Json& jp, Pattern& p, const Song& s) {
  if (const Json* obj = jp.find("lanes"); obj && obj->is(Json::Type::Object))  // K-255 (before the step properties: a lane's row length)
    for (const auto& [k, v] : obj->object) {
      const ObjectId ch = static_cast<ObjectId>(std::strtoul(k.c_str(), nullptr, 10));
      if (!s.channel(ch) || !v.is(Json::Type::Object)) continue;
      PatternLane l;
      l.grid = jsonInt<int>(std::clamp(v.num("grid", 0), 0.0, static_cast<double>(kMaxStepsPerBar)));
      if (l.grid == p.stepsPerBar) l.grid = 0;
      if (const Json* ro = v.find("runOn"); ro && ro->is(Json::Type::Bool) && ro->boolean) l.mode = LaneMode::RunOn;
      if (!l.isDefault()) p.lanes[ch] = l;
    }
  stepPropsFromJson(jp, p, s);
  if (const Json* lp = jp.find("loop"); lp && lp->is(Json::Type::Bool)) p.loop = lp->boolean;  // Q-1
  p.meterNum = jsonInt<int>(std::clamp(jp.num("meterNum", 0), 0.0, 32.0));  // K-179
  p.meterDen = jsonInt<int>(std::clamp(jp.num("meterDen", 0), 0.0, 32.0));
  if (p.meterNum <= 0 || p.meterDen <= 0 || !normalizeMeter(p.meterNum, p.meterDen, s.ppq)) p.meterNum = p.meterDen = 0;
  p.linkChannel = kNoId, p.linkHash = 0, p.linkCycles = 1;
  if (const Json* lk = jp.find("link"); lk && lk->is(Json::Type::Object)) {  // 0.7.8: a linked pattern (its channel must exist)
    const ObjectId ch = jsonInt<ObjectId>(std::max(0.0, lk->num("channel", 0)));
    const unsigned long long h = std::strtoull(lk->text("hash", "").c_str(), nullptr, 16);
    if (s.channel(ch) && h != 0) {
      p.linkChannel = ch;
      p.linkHash = static_cast<uint64_t>(h);
      p.linkCycles = jsonInt<int>(std::clamp(lk->num("cycles", 1), 1.0, 64.0));
    }
  }
  if (const Json* loops = jp.find("stepLoop"); loops && loops->is(Json::Type::Object))  // J-31
    for (const auto& [k, v] : loops->object) {
      const ObjectId ch = static_cast<ObjectId>(std::strtoul(k.c_str(), nullptr, 10));
      if (!s.channel(ch) || !v.is(Json::Type::Number)) continue;
      const int n = jsonInt<int>(std::clamp(v.number, 0.0, 4096.0));
      if (n > 0) p.stepLoop[ch] = n;
    }
  const Json* arr = jp.find("automation");
  if (!arr || !arr->is(Json::Type::Array)) return;
  for (const auto& jl : arr->array) {
    PatternAutomation lane;
    lane.owner = jsonInt<ObjectId>(std::max(0.0, jl.num("owner", 0)));
    lane.index = jsonInt<uint32_t>(std::max(0.0, jl.num("index", 0)));
    lane.points = pointsFromJson(jl.find("points"));
    // J-173: clip fields (absent in projects saved before: an unnamed clip, range 0..1, the earlier curves)
    lane.name = jl.text("name", "");
    lane.color = color(jl, lane.color);
    lane.min = static_cast<float>(std::clamp(jl.num("min", 0), 0.0, 1.0));
    lane.max = static_cast<float>(std::clamp(jl.num("max", 1), 0.0, 1.0));
    lane.loop = std::max<Tick>(0, jsonTick(jl.num("loop", 0)));
    lane.flCurves = flag(jl, "flCurves");
    lane.events = flag(jl, "events");
    if (lane.points.empty() || !describeParam(s, {lane.owner, lane.index})) continue;
    bool dup = false;
    for (const auto& l : p.automation) dup = dup || (l.owner == lane.owner && l.index == lane.index);
    if (!dup) p.automation.push_back(std::move(lane));
  }
}

const PlaylistTrack kEmptyTrack{};  // K-314 B-03: what a track shown on this computer only is saved as

void trackExtrasToJson(const PlaylistTrack& t, Json& jt) {
  if (t.launch != LaunchGrid::FourBeats) jt.object["launch"] = num(static_cast<int>(t.launch));
  if (t.pad != PadMode::Trigger) jt.object["pad"] = num(static_cast<int>(t.pad));
  if (t.follow != FollowMode::Loop) jt.object["follow"] = num(static_cast<int>(t.follow));
  if (t.syncStart != LaunchGrid::Now) jt.object["syncStart"] = num(static_cast<int>(t.syncStart));
  if (t.groupSize > 0) jt.object["groupSize"] = num(t.groupSize);
  if (t.folded) jt.object["folded"] = boolean(true);
  if (t.gridFolded) jt.object["gridFolded"] = boolean(true);  // K-112c (checkpoint 52)
  if (t.locked) jt.object["locked"] = boolean(true);
  if (t.takeLanes) jt.object["takeLanes"] = boolean(true);  // K-123
  if (t.flIcon) jt.object["flIcon"] = num(t.flIcon);  // J-39
  if (t.icon || t.flIcon) jt.object["icon"] = num(t.icon);        // J-211
  // PL-08: track mode and links.
  if (t.mode != TrackMode::Unassigned) jt.object["mode"] = num(static_cast<int>(t.mode));
  if (t.mixer != kNoId) jt.object["mixer"] = num(t.mixer);
  if (t.channel != kNoId) jt.object["channel"] = num(t.channel);
  if (t.laneSet) {  // K-112b (R02-01): the track's automation lane
    Json jl = Json::makeObject();
    jl.object["owner"] = num(t.laneOwner);
    jl.object["index"] = num(t.laneIndex);
    if (t.laneShown) jl.object["shown"] = boolean(true);
    jt.object["lane"] = std::move(jl);
  } else if (t.laneShown) {  // K-314 B-04: the lane open on its default parameter (Show automation) is saved too
    jt.object["laneShown"] = boolean(true);
  }
  if (t.autoMode != 1) jt.object["autoMode"] = num(t.autoMode);  // K-112B: Read is the default
  if (t.sound != kNoId) jt.object["sound"] = num(t.sound);         // K-112B (R08-07)
  if (!t.noStop.empty()) {  // grid2-1118: the scenes whose empty slot has no stop button
    Json jn = Json::makeArray();
    for (int k : t.noStop) {
      jn.array.push_back(num(k));
    }
    jt.object["noStop"] = std::move(jn);
  }
}

void trackExtrasFromJson(const Json& jt, PlaylistTrack& t) {
  auto e = [&](const char* k, int dflt, int count) { return jsonInt<int>(std::clamp(jt.num(k, dflt), 0.0, static_cast<double>(count - 1))); };
  t.launch = static_cast<LaunchGrid>(e("launch", static_cast<int>(LaunchGrid::FourBeats), static_cast<int>(LaunchGrid::Count)));
  t.pad = static_cast<PadMode>(e("pad", 0, static_cast<int>(PadMode::Count)));
  t.follow = static_cast<FollowMode>(e("follow", 0, static_cast<int>(FollowMode::Count)));
  t.syncStart = static_cast<LaunchGrid>(e("syncStart", 0, static_cast<int>(LaunchGrid::Count)));
  t.groupSize = jsonInt<int>(std::clamp(jt.num("groupSize", 0), 0.0, static_cast<double>(kMaxPlaylistTracks)));
  t.folded = flag(jt, "folded");
  t.gridFolded = flag(jt, "gridFolded");
  t.locked = flag(jt, "locked");
  t.takeLanes = flag(jt, "takeLanes");  // K-123
  t.flIcon = jsonInt<int>(std::clamp(jt.num("flIcon", 0), 0.0, 65535.0));  // J-39
  t.icon = iconFromJson(jt, t.flIcon);                                         // J-211
  t.mode = static_cast<TrackMode>(e("mode", 0, static_cast<int>(TrackMode::Count)));  // PL-08 (stale links dropped after load)
  t.mixer = jsonInt<ObjectId>(std::max(0.0, jt.num("mixer", 0)));
  t.channel = jsonInt<ObjectId>(std::max(0.0, jt.num("channel", 0)));
  t.laneShown = flag(jt, "laneShown");  // K-314 B-04 (no parameter chosen)
  if (const Json* jl = jt.find("lane"); jl && jl->is(Json::Type::Object)) {  // K-112b (R02-01)
    t.laneSet = true;
    t.laneOwner = jsonInt<ObjectId>(std::max(0.0, jl->num("owner", 0)));
    t.laneIndex = jsonInt<uint32_t>(std::max(0.0, jl->num("index", 0)));
    t.laneShown = flag(*jl, "shown");
  }
  t.autoMode = static_cast<uint8_t>(e("autoMode", 1, 5));  // K-112B: Off, Read, Touch, Latch, Write
  t.sound = jsonInt<ObjectId>(std::max(0.0, jt.num("sound", 0)));  // K-112B (R08-07)
  t.noStop.clear();
  if (const Json* jn = jt.find("noStop"); jn && jn->is(Json::Type::Array)) {  // grid2-1118
    for (const Json& v : jn->array) {
      if (v.is(Json::Type::Number) && v.number >= 0 && v.number < 1e6) {
        t.noStop.push_back(jsonInt<int>(v.number));
      }
    }
    std::sort(t.noStop.begin(), t.noStop.end());
    t.noStop.erase(std::unique(t.noStop.begin(), t.noStop.end()), t.noStop.end());
  }
}

// Checkpoint 59 (N58-03): Clips' scene names and colours, [{"name", "color"}] by scene. K-314 (A-02):
// shared by the current arrangement ("live.scenes") and the parked ones ("liveScenes").
Json scenesToJson(const std::vector<SceneInfo>& scenes) {
  Json js = Json::makeArray();
  for (const SceneInfo& sc : scenes) {
    Json e = Json::makeObject();
    if (!sc.name.empty()) {
      e.object["name"] = Json::makeString(sc.name);
    }
    if (sc.color != 0) {
      e.object["color"] = num(static_cast<double>(sc.color));
    }
    if (sc.tempo > 0) {  // grid2-1118: the scene's tempo, time signature and follow actions
      e.object["tempo"] = num(sc.tempo);
    }
    if (sc.sigNum > 0 && sc.sigDen > 0) {
      e.object["sigNum"] = num(sc.sigNum);
      e.object["sigDen"] = num(sc.sigDen);
    }
    if (!sc.follow.isDefault()) {
      e.object["follow"] = followToJson(sc.follow);
    }
    js.array.push_back(std::move(e));
  }
  return js;
}

std::vector<SceneInfo> scenesFromJson(const Json* js) {
  std::vector<SceneInfo> out;
  if (!js || !js->is(Json::Type::Array)) {
    return out;
  }
  for (const Json& e : js->array) {
    SceneInfo sc;
    if (e.is(Json::Type::Object)) {
      sc.name = e.text("name", "");
      sc.color = static_cast<Rgb>(std::clamp(e.num("color", 0), 0.0, 16777215.0));
      sc.tempo = std::clamp(e.num("tempo", 0), 0.0, 999.0);  // grid2-1118
      if (sc.tempo > 0 && sc.tempo < 10) {
        sc.tempo = 10;
      }
      sc.sigNum = jsonInt<int>(std::clamp(e.num("sigNum", 0), 0.0, 32.0));
      sc.sigDen = jsonInt<int>(std::clamp(e.num("sigDen", 0), 0.0, 32.0));
      if (sc.sigNum <= 0 || (sc.sigDen != 1 && sc.sigDen != 2 && sc.sigDen != 4 && sc.sigDen != 8 && sc.sigDen != 16 && sc.sigDen != 32)) {
        sc.sigNum = sc.sigDen = 0;
      }
      sc.follow = followFromJson(e.find("follow"));
    }
    out.push_back(sc);
  }
  while (!out.empty() && out.back().empty()) {
    out.pop_back();
  }
  return out;
}

// grid2-1118: follow actions and a clip's own launch settings (written only when not the defaults).
Json followToJson(const FollowSpec& f) {
  Json j = Json::makeObject();
  const FollowSpec d;
  if (f.on) {
    j.object["on"] = boolean(true);
  }
  if (f.a != d.a) {
    j.object["a"] = num(static_cast<int>(f.a));
  }
  if (f.b != d.b) {
    j.object["b"] = num(static_cast<int>(f.b));
  }
  if (f.chanceA != d.chanceA) {
    j.object["chanceA"] = num(f.chanceA);
  }
  if (f.chanceB != d.chanceB) {
    j.object["chanceB"] = num(f.chanceB);
  }
  if (f.jumpA != 0) {
    j.object["jumpA"] = num(f.jumpA);
  }
  if (f.jumpB != 0) {
    j.object["jumpB"] = num(f.jumpB);
  }
  if (f.time != 0) {
    j.object["time"] = num(static_cast<double>(f.time));
  }
  if (f.linked) {
    j.object["linked"] = boolean(true);
  }
  if (f.loops != 1) {
    j.object["loops"] = num(f.loops);
  }
  return j;
}

FollowSpec followFromJson(const Json* j) {
  FollowSpec f;
  if (!j || !j->is(Json::Type::Object)) {
    return f;
  }
  const double last = static_cast<double>(static_cast<int>(FollowAction::Count) - 1);
  f.on = flag(*j, "on");
  f.a = static_cast<FollowAction>(jsonInt<int>(std::clamp(j->num("a", static_cast<double>(f.a)), 0.0, last)));
  f.b = static_cast<FollowAction>(jsonInt<int>(std::clamp(j->num("b", static_cast<double>(f.b)), 0.0, last)));
  f.chanceA = static_cast<uint8_t>(jsonInt<int>(std::clamp(j->num("chanceA", 100), 0.0, 100.0)));
  f.chanceB = static_cast<uint8_t>(jsonInt<int>(std::clamp(j->num("chanceB", 0), 0.0, 100.0)));
  f.jumpA = jsonInt<int>(std::clamp(j->num("jumpA", 0), 0.0, 99999.0));
  f.jumpB = jsonInt<int>(std::clamp(j->num("jumpB", 0), 0.0, 99999.0));
  f.time = std::max<Tick>(0, jsonTick(j->num("time", 0)));
  f.linked = flag(*j, "linked");
  f.loops = jsonInt<int>(std::clamp(j->num("loops", 1), 1.0, 999.0));
  return f;
}

Json clipToJson(const Clip& c) {
  Json jc = Json::makeObject();
  jc.object["id"] = num(c.id);
  jc.object["pattern"] = num(c.pattern);
  jc.object["track"] = num(c.track);
  jc.object["start"] = num(static_cast<double>(c.start));
  jc.object["length"] = num(static_cast<double>(c.length));
  clipArrangementToJson(c, jc);
  return jc;
}

void songExtrasToJson(const Song& s, Json& j) {
  if (s.cycleEnd > s.cycleStart) {  // K-112b (R02-04)
    Json jc = Json::makeObject();
    jc.object["start"] = num(static_cast<double>(s.cycleStart));
    jc.object["end"] = num(static_cast<double>(s.cycleEnd));
    if (s.cycleOn) jc.object["on"] = boolean(true);
    j.object["cycle"] = std::move(jc);
  }
  if (s.projectEnd > 0) j.object["projectEnd"] = num(static_cast<double>(s.projectEnd));  // K-112B (checkpoint 47)
  if (!s.pluginWindows.empty()) {  // K-297 (round 9): [[owner, slot], ...] (slot -1: the instrument)
    Json jw = Json::makeArray();
    for (const Song::OpenPluginWindow& w : s.pluginWindows) {
      Json e = Json::makeArray();
      e.array.push_back(num(static_cast<double>(w.owner)));
      e.array.push_back(num(w.slot));
      jw.array.push_back(std::move(e));
    }
    j.object["pluginWindows"] = std::move(jw);
  }
  if (!s.workspaceCopy.empty()) {  // K-237: the opt-in copy of the person's workspaces {layout id: set as JSON text}
    Json jw = Json::makeObject();
    for (const auto& [lay, text] : s.workspaceCopy) jw.object[lay] = Json::makeString(text);
    j.object["workspaces"] = std::move(jw);
  }
  if (!s.portLinks.empty()) {  // K-357: per-computer JACK / PipeWire port connections [[own, peer], ...]
    Json jl = Json::makeArray();
    for (const Song::PortLink& l : s.portLinks) {
      if (l.own.empty() || l.peer.empty()) continue;
      Json pair = Json::makeArray();
      pair.array.push_back(Json::makeString(l.own));
      pair.array.push_back(Json::makeString(l.peer));
      jl.array.push_back(std::move(pair));
    }
    if (!jl.array.empty()) j.object["portLinks"] = std::move(jl);
  }
  if (s.live.areaEnd > 0 || !s.keptSends.empty() || !s.live.scenes.empty() || s.live.sceneLen > 0) {
    Json jl = Json::makeObject();
    if (s.live.areaEnd > 0) jl.object["areaEnd"] = num(static_cast<double>(s.live.areaEnd));
    if (s.live.sceneLen > 0) {  // grid2-1118
      jl.object["sceneLen"] = num(static_cast<double>(s.live.sceneLen));
    }
    if (!s.live.scenes.empty()) {  // checkpoint 59 (N58-03): [{"name", "color"}] by scene
      jl.object["scenes"] = scenesToJson(s.live.scenes);
    }
    if (!s.keptSends.empty()) {  // K-112c checkpoint 57 (N56-01): [mixer, to, level, pre-fader, index]
      Json jk = Json::makeArray();
      for (const KeptSend& k : s.keptSends) {
        Json e = Json::makeArray();
        e.array.push_back(num(k.mixer));
        e.array.push_back(num(k.to));
        e.array.push_back(num(k.level));
        e.array.push_back(num(k.preFader ? 1 : 0));
        e.array.push_back(num(k.index));
        jk.array.push_back(std::move(e));
      }
      jl.object["keptSends"] = std::move(jk);
    }
    j.object["live"] = std::move(jl);
  }
  if (!s.smooth.library.empty() || !s.smooth.controls.empty()) {  // K-112D: the Smooth layout's Library pane, once set there
    Json js = Json::makeObject();
    if (!s.smooth.library.empty()) js.object["library"] = Json::makeString(s.smooth.library);
    if (!s.smooth.controls.empty()) {  // D5: Smart Controls mapped by hand, [strip, position, owner, index]
      Json jc = Json::makeArray();
      for (const SmartControlMap& m : s.smooth.controls) {
        Json e = Json::makeArray();
        e.array.push_back(num(m.strip));
        e.array.push_back(num(m.pos));
        e.array.push_back(num(m.owner));
        e.array.push_back(num(m.index));
        jc.array.push_back(std::move(e));
      }
      js.object["controls"] = std::move(jc);
    }
    j.object["smooth"] = std::move(js);
  }
  if (!s.rackAutoLanes.empty()) {  // K-413: non-channel persistent rack automation lanes
    Json arr = Json::makeArray();
    for (const SongAutoLane& l : s.rackAutoLanes) {
      Json jl = Json::makeObject();
      jl.object["owner"] = num(l.owner);
      jl.object["index"] = num(l.index);
      if (!l.name.empty()) jl.object["name"] = str(l.name);
      if (l.color != AutomationClip{}.color) jl.object["color"] = num(l.color);
      arr.array.push_back(std::move(jl));
    }
    j.object["rackAutoLanes"] = std::move(arr);
  } else {
    bool anyPatAuto = false, anyChLanes = false;
    for (const auto& c : s.channels) anyChLanes = anyChLanes || !c.automationLanes.empty();
    for (const auto& p : s.patterns) anyPatAuto = anyPatAuto || !p.automation.empty();
    if (anyPatAuto && !anyChLanes) j.object["rackAutoLanes"] = Json::makeArray();
  }
  // Arrangements: written when there are several, or the single one was renamed.
  if (s.arrangements.size() > 1 || (s.arrangements.size() == 1 && s.arrangements[0].name != "Arrangement")) {
    Json arr = Json::makeArray();
    for (size_t i = 0; i < s.arrangements.size(); ++i) {
      const Arrangement& a = s.arrangements[i];
      Json ja = Json::makeObject();
      ja.object["name"] = str(a.name);
      if (static_cast<int>(i) != s.currentArrangement) {  // the current one is the song's playlist / tracks / markers
        Json clips = Json::makeArray();
        for (const auto& c : a.playlist) clips.array.push_back(clipToJson(c));
        ja.object["playlist"] = std::move(clips);
        Json tracks = Json::makeArray();
        for (const auto& at : a.tracks) {
          const PlaylistTrack& t = at.shownHere ? kEmptyTrack : at;  // K-314 B-03
          Json jt = Json::makeObject();
          jt.object["name"] = str(t.name);
          jt.object["color"] = num(t.color);
          jt.object["muted"] = boolean(t.muted);
          if (t.height != 1.0f) jt.object["height"] = num(t.height);
          trackExtrasToJson(t, jt);
          tracks.array.push_back(std::move(jt));
        }
        ja.object["tracks"] = std::move(tracks);
        Json marks = Json::makeArray();
        for (const auto& m : a.markers) {
          Json jm = Json::makeObject();
          jm.object["pos"] = num(static_cast<double>(m.pos));
          jm.object["name"] = str(m.name);
          if (m.timeSigNum > 0) {
            jm.object["timeSigNum"] = num(m.timeSigNum);
            jm.object["timeSigDen"] = num(m.timeSigDen);
          }
          if (m.action != MarkerAction::None) jm.object["action"] = num(static_cast<int>(m.action));
          if (m.isKey()) {  // J-31 key marker
            jm.object["keyRoot"] = num(m.keyRoot);
            jm.object["keyScale"] = num(m.keyScale);
          }
          if (!m.meterForPatterns) jm.object["meterForPatterns"] = boolean(false);  // K-179
          marks.array.push_back(std::move(jm));
        }
        ja.object["markers"] = std::move(marks);
        if (a.live.areaEnd > 0) ja.object["liveAreaEnd"] = num(static_cast<double>(a.live.areaEnd));
        if (!a.live.scenes.empty()) {  // K-314 (A-02): a parked arrangement's scene names and colours
          ja.object["liveScenes"] = scenesToJson(a.live.scenes);
        }
        if (a.live.sceneLen > 0) {  // grid2-1118
          ja.object["liveSceneLen"] = num(static_cast<double>(a.live.sceneLen));
        }
      }
      arr.array.push_back(std::move(ja));
    }
    j.object["arrangements"] = std::move(arr);
    j.object["currentArrangement"] = num(s.currentArrangement);
  }
}

// After patterns, channels, automation clips and the current playlist: parked
// clips get unique ids; clips whose content is gone are dropped.
bool songExtrasFromJson(const Json& j, Song& s, const std::function<bool(const Json&, ObjectId&)>& takeId, std::string* err) {
  // K-237: the opt-in workspaces copy; only the four layouts, each a JSON text of reasonable size.
  if (const Json* jw = j.find("workspaces"); jw && jw->is(Json::Type::Object))
    for (const auto& [lay, v] : jw->object)
      if ((lay == "studio" || lay == "live" || lay == "logic" || lay == "tracker") && v.is(Json::Type::String) && !v.str.empty() && v.str.size() <= 262144)
        s.workspaceCopy[lay] = v.str;
  // K-297 (round 9): open plugin windows; one naming nothing in the song (or twice) is left out.
  if (const Json* jw = j.find("pluginWindows"); jw && jw->is(Json::Type::Array)) {
    for (const Json& e : jw->array) {
      if (!e.is(Json::Type::Array) || e.array.size() < 2 || !e.array[0].is(Json::Type::Number) || !e.array[1].is(Json::Type::Number)) continue;
      const double o = e.array[0].number, k = e.array[1].number;
      if (!std::isfinite(o) || !std::isfinite(k) || o < 1 || k < -1 || k >= kEffectSlots) continue;
      Song::OpenPluginWindow w{static_cast<ObjectId>(o), static_cast<int>(k)};
      const MixerTrack* m = w.slot >= 0 ? s.mixerTrack(w.owner) : nullptr;
      const bool exists = w.slot < 0 ? s.channel(w.owner) != nullptr
                                     : m && w.slot < static_cast<int>(m->slots.size()) && !m->slots[static_cast<size_t>(w.slot)].plugin.type.empty();
      if (exists && std::find(s.pluginWindows.begin(), s.pluginWindows.end(), w) == s.pluginWindows.end()) s.pluginWindows.push_back(w);
    }
  }
  if (const Json* jl = j.find("portLinks"); jl && jl->is(Json::Type::Array)) {  // K-357
    for (const Json& e : jl->array) {
      if (!e.is(Json::Type::Array) || e.array.size() != 2) continue;
      if (!e.array[0].is(Json::Type::String) || !e.array[1].is(Json::Type::String)) continue;
      if (e.array[0].str.empty() || e.array[1].str.empty()) continue;
      Song::PortLink l{e.array[0].str, e.array[1].str};
      if (std::find(s.portLinks.begin(), s.portLinks.end(), l) == s.portLinks.end()) s.portLinks.push_back(std::move(l));
    }
  }
  if (const Json* jl = j.find("live"); jl && jl->is(Json::Type::Object)) {
    s.live.areaEnd = std::max<Tick>(0, jsonTick(jl->num("areaEnd", 0)));
    s.live.sceneLen = std::max<Tick>(0, jsonTick(jl->num("sceneLen", 0)));  // grid2-1118
    s.live.view = jl->text("view", "");  // checkpoint 59: read once (0.7.7 dev saves), never written again
    s.live.scenes = scenesFromJson(jl->find("scenes"));  // checkpoint 59 (N58-03)
    if (s.live.view != "grid" && s.live.view != "arrangement") s.live.view.clear();
    s.keptSends.clear();
    if (const Json* jk = jl->find("keptSends"); jk && jk->is(Json::Type::Array)) {  // K-112c checkpoint 57 (N56-01)
      for (const Json& e : jk->array) {
        if (!e.is(Json::Type::Array) || e.array.size() != 5) {
          continue;
        }
        bool ok = true;
        for (const Json& v : e.array) {
          ok = ok && v.is(Json::Type::Number) && v.number >= 0 && v.number <= 4e9;
        }
        if (!ok || e.array[4].number > 4096) {
          continue;
        }
        KeptSend k;
        k.mixer = static_cast<ObjectId>(e.array[0].number);
        k.to = static_cast<ObjectId>(e.array[1].number);
        k.level = static_cast<float>(std::clamp(e.array[2].number, 0.0, 1.25));
        k.preFader = e.array[3].number != 0;
        k.index = static_cast<int>(e.array[4].number);
        s.keptSends.push_back(k);
      }
    }
  }
  if (const Json* js = j.find("smooth"); js && js->is(Json::Type::Object)) {  // K-112D
    s.smooth.library = js->text("library", "");
    if (s.smooth.library != "open" && s.smooth.library != "closed") s.smooth.library.clear();
    s.smooth.controls.clear();
    if (const Json* jc = js->find("controls"); jc && jc->is(Json::Type::Array)) {  // D5
      for (const Json& e : jc->array) {
        if (!e.is(Json::Type::Array) || e.array.size() != 4) continue;
        bool ok = true;
        for (const Json& v : e.array) ok = ok && v.is(Json::Type::Number) && v.number >= 0 && v.number <= 4e9;
        if (!ok || e.array[1].number > 63) continue;
        SmartControlMap m;
        m.strip = static_cast<ObjectId>(e.array[0].number);
        m.pos = static_cast<int>(e.array[1].number);
        m.owner = static_cast<ObjectId>(e.array[2].number);
        m.index = static_cast<uint32_t>(e.array[3].number);
        s.smooth.controls.push_back(m);
      }
    }
  }
  if (const Json* jc = j.find("cycle"); jc && jc->is(Json::Type::Object)) {  // K-112b (R02-04)
    s.cycleStart = std::max<Tick>(0, jsonTick(jc->num("start", 0)));
    s.cycleEnd = std::max<Tick>(0, jsonTick(jc->num("end", 0)));
    s.cycleOn = s.cycleEnd > s.cycleStart && flag(*jc, "on");
    if (s.cycleEnd <= s.cycleStart) s.cycleStart = s.cycleEnd = 0;
  }
  s.rackAutoLanes.clear();  // K-413
  bool hasSavedRackLanes = j.find("rackAutoLanes") != nullptr;
  for (const auto& ch : s.channels) hasSavedRackLanes = hasSavedRackLanes || !ch.automationLanes.empty();
  if (const Json* rl = j.find("rackAutoLanes"); rl && rl->is(Json::Type::Array)) {
    for (const Json& jl : rl->array) {
      if (!jl.is(Json::Type::Object) || s.rackAutoLanes.size() >= 256) continue;
      SongAutoLane l;
      l.owner = jsonInt<ObjectId>(std::max(0.0, jl.num("owner", 0)));
      l.index = jsonInt<uint32_t>(std::max(0.0, jl.num("index", 0)));
      l.name = jl.text("name");
      l.color = color(jl, AutomationClip{}.color);
      if (l.owner == kNoId || !describeParam(s, ParamId{l.owner, l.index})) continue;
      if (s.channel(l.owner)) ensureRackAutoLane(s, l.owner, l.index, l.name, l.color);
      else if (!hasRackAutoLane(s, l.owner, l.index)) s.rackAutoLanes.push_back(std::move(l));
    }
  }
  if (!hasSavedRackLanes && tNoRackLaneMigration == 0) migrateRackAutoLanes(s);  // K-413: old projects union pattern automation lanes
  s.projectEnd = std::max<Tick>(0, jsonTick(j.num("projectEnd", 0)));  // K-112B (checkpoint 47)
  const Json* arr = j.find("arrangements");
  if (!arr || !arr->is(Json::Type::Array) || arr->array.empty()) return true;
  const int cur = jsonInt<int>(std::clamp(j.num("currentArrangement", 0), 0.0, static_cast<double>(arr->array.size() - 1)));
  for (size_t i = 0; i < arr->array.size(); ++i) {
    const Json& ja = arr->array[i];
    Arrangement a;
    a.name = ja.text("name", "Arrangement");
    if (static_cast<int>(i) != cur) {
      if (const Json* clips = ja.find("playlist"); clips && clips->is(Json::Type::Array))
        for (const auto& jc : clips->array) {
          Clip c;
          if (!takeId(jc, c.id)) return fail(err, "arrangement clip has missing or duplicate id");
          c.pattern = jsonInt<ObjectId>(jc.num("pattern", 0));
          c.track = jsonInt<int>(std::clamp(jc.num("track", 0), 0.0, static_cast<double>(kMaxPlaylistTracks - 1)));
          c.start = std::max<Tick>(0, jsonTick(jc.num("start", 0)));
          c.length = std::max<Tick>(0, jsonTick(jc.num("length", 0)));
          clipArrangementFromJson(jc, c);
          if (c.kind == ClipKind::Audio && !s.channel(c.source)) continue;
          if (c.kind == ClipKind::Automation && !s.automation(c.source)) continue;
          if (c.kind == ClipKind::Pattern && !s.pattern(c.pattern)) continue;
          a.playlist.push_back(c);
        }
      if (const Json* tracks = ja.find("tracks"); tracks && tracks->is(Json::Type::Array))
        for (const auto& jt : tracks->array) {
          if (a.tracks.size() >= static_cast<size_t>(kMaxPlaylistTracks)) break;
          PlaylistTrack t;
          t.name = jt.text("name");
          t.color = color(jt, t.color);
          t.muted = flag(jt, "muted");
          t.height = static_cast<float>(std::clamp(jt.num("height", 1), 0.25, 4.0));
          trackExtrasFromJson(jt, t);
          a.tracks.push_back(t);
        }
      for (const auto& c : a.playlist)
        if (static_cast<size_t>(c.track) >= a.tracks.size()) a.tracks.resize(static_cast<size_t>(c.track) + 1);
      if (a.tracks.empty()) a.tracks.resize(kDefaultPlaylistTracks);
      if (const Json* marks = ja.find("markers"); marks && marks->is(Json::Type::Array))
        for (const auto& jm : marks->array) {
          Marker m;
          m.pos = std::max<Tick>(0, jsonTick(jm.num("pos", 0)));
          m.name = jm.text("name");
          m.timeSigNum = jsonInt<int>(std::clamp(jm.num("timeSigNum", 0), 0.0, 32.0));
          m.timeSigDen = jsonInt<int>(std::clamp(jm.num("timeSigDen", 0), 0.0, 32.0));
          if (m.timeSigNum == 0 || m.timeSigDen == 0) m.timeSigNum = m.timeSigDen = 0;
          m.action = static_cast<MarkerAction>(jsonInt<int>(std::clamp(jm.num("action", 0), 0.0, static_cast<double>(static_cast<int>(MarkerAction::Count) - 1))));
          m.keyRoot = jsonInt<int>(std::clamp(jm.num("keyRoot", -1), -1.0, 11.0));  // J-31
          m.keyScale = jsonInt<int>(std::clamp(jm.num("keyScale", 0), 0.0, 64.0));
          m.meterForPatterns = flag(jm, "meterForPatterns", true);  // K-179
          a.markers.push_back(std::move(m));
        }
      std::stable_sort(a.markers.begin(), a.markers.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
      a.live.areaEnd = std::max<Tick>(0, jsonTick(ja.num("liveAreaEnd", 0)));
      a.live.scenes = scenesFromJson(ja.find("liveScenes"));  // K-314 (A-02)
      a.live.sceneLen = std::max<Tick>(0, jsonTick(ja.num("liveSceneLen", 0)));  // grid2-1118
    }
    s.arrangements.push_back(std::move(a));
  }
  s.currentArrangement = cur;
  return true;
}

// K-151: the song's swing shape. Written only when it is not the classic step swing.
Json grooveToJson(const SongGroove& gIn) {
  SongGroove g = gIn;
  groove::normalize(g);
  Json j = Json::makeObject();
  j.object["grid"] = Json::makeString(groove::gridKey(g.shape.grid));
  j.object["cycle"] = num(g.shape.cycle);
  Json t = Json::makeArray();
  for (float v : g.shape.timing) t.array.push_back(num(v));
  j.object["timing"] = std::move(t);
  if (!g.shape.velocity.empty()) {
    Json v = Json::makeArray();
    for (float x : g.shape.velocity) v.array.push_back(num(x));
    j.object["velocity"] = std::move(v);
  }
  if (g.accent != 1.0f) j.object["accent"] = num(g.accent);
  if (!g.preset.empty()) j.object["preset"] = Json::makeString(g.preset);
  return j;
}

SongGroove grooveFromJson(const Json& j) {
  SongGroove g;
  g.shape.grid = groove::gridFromKey(j.text("grid", "step"));
  g.shape.cycle = jsonInt<int>(j.num("cycle", 2));
  auto floats = [](const Json* a, float dflt) {
    std::vector<float> v;
    if (a && a->is(Json::Type::Array))
      for (const Json& x : a->array) v.push_back(x.is(Json::Type::Number) ? static_cast<float>(x.number) : dflt);
    return v;
  };
  if (const Json* t = j.find("timing"); t && t->is(Json::Type::Array)) g.shape.timing = floats(t, 0.0f);
  g.shape.velocity = floats(j.find("velocity"), 1.0f);
  g.accent = static_cast<float>(j.num("accent", 1.0));
  g.preset = j.text("preset");
  groove::normalize(g);
  return g;
}
// ---------------------------------------------------- end pattern automation, arrangements, live --

}  // namespace

Json pluginStateToJson(const PluginState& p) { return pluginToJson(p); }
void noteExtrasToJson(const Note& n, Json& jn) { noteToJson(n, jn); }
void noteExtrasFromJson(const Json& jn, Note& n) { noteFromJson(jn, n); }
PluginState pluginStateFromJson(const Json& j) { return pluginFromJson(&j); }

// K-224: one Stage room's settings (Song::stage and each of Song::stageRooms share the shape).
Json stageToJson(const StageSettings& r) {
  Json st = Json::makeObject();
  if (r.id != kNoId) st.object["id"] = num(static_cast<double>(r.id));
  if (!r.name.empty()) st.object["name"] = str(r.name);
  if (r.color != 0) st.object["color"] = num(r.color);
  if (r.width != 1.0f) st.object["width"] = num(r.width);
  if (r.room != kNoId) st.object["room"] = num(static_cast<double>(r.room));
  st.object["depthStrength"] = num(r.depthStrength);
  st.object["roomKind"] = Json::makeString(rooms::key(r.roomType));
  if (!r.preDelay) st.object["preDelay"] = boolean(false);
  if (!r.early) st.object["early"] = boolean(false);
  if (!r.frontQuiet) st.object["frontQuiet"] = boolean(false);
  return st;
}

void stageFromJson(const Json& st, StageSettings& r) {
  if (const double v = st.num("id", 0); std::isfinite(v) && v >= kFirstObjectId && v <= 4e9) r.id = static_cast<ObjectId>(v);
  r.name = st.text("name");
  if (const double v = st.num("color", 0); std::isfinite(v) && v > 0 && v <= 0xFFFFFF) r.color = static_cast<Rgb>(v);
  if (const double v = st.num("width", 1); std::isfinite(v)) r.width = static_cast<float>(std::clamp(v, 0.0, 1.5));
  r.room = jsonInt<ObjectId>(st.num("room", 0));
  if (const double v = st.num("depthStrength", 0.5); std::isfinite(v)) r.depthStrength = static_cast<float>(std::clamp(v, 0.0, 1.0));
  if (st.find("roomKind")) r.roomType = static_cast<uint8_t>(rooms::ofKey(st.text("roomKind")));
  else if (const double v = st.num("roomType", 1); std::isfinite(v))  // an early Stage build: 0 small room .. 4 plate
    r.roomType = static_cast<uint8_t>(rooms::ofEarly(static_cast<int>(v)));
  r.preDelay = flag(st, "preDelay", true);
  r.early = flag(st, "early", true);
  r.frontQuiet = flag(st, "frontQuiet", true);
}

Json mixerTrackToJson(const MixerTrack& m) {
  Json jm = Json::makeObject();
  jm.object["id"] = num(m.id);
  jm.object["name"] = str(m.name);
  jm.object["color"] = num(m.color);
  jm.object["volume"] = num(m.volume);
  jm.object["pan"] = num(m.pan);
  jm.object["stereoSep"] = num(m.stereoSep);
  if (m.room != kNoId) jm.object["roomId"] = num(static_cast<double>(m.room));  // K-224: its Stage room
  if (m.depth != 0.0f) jm.object["depth"] = num(m.depth);  // Stage distance (absent: 0, older projects)
  if (m.focus != 0.0f) jm.object["focus"] = num(m.focus);  // Stage focus (absent: 0)
  jm.object["muted"] = boolean(m.muted);
  jm.object["solo"] = boolean(m.solo);
  jm.object["invertPolarity"] = boolean(m.invertPolarity);
  jm.object["swapLR"] = boolean(m.swapLR);
  if (m.input >= 0) jm.object["input"] = num(m.input);
  if (!m.inputStereo) jm.object["inputStereo"] = boolean(false);
  if (m.monitor != 0) jm.object["monitor"] = num(m.monitor);
  if (m.inputTrim != 0.0f) jm.object["inputTrim"] = num(m.inputTrim);  // K-123
  if (m.inputInvert) jm.object["inputInvert"] = boolean(true);
  if (m.armed) jm.object["armed"] = boolean(true);
  if (m.recordSafe) jm.object["recordSafe"] = boolean(true);  // K-123
  if (m.recPoint != 0) jm.object["recPoint"] = num(m.recPoint);  // J-31
  if (m.soloAlone) jm.object["soloAlone"] = boolean(true);
  if (m.separator) jm.object["separator"] = boolean(true);  // J-39
  if (m.locked) jm.object["locked"] = boolean(true);
  if (m.flIcon) jm.object["flIcon"] = num(m.flIcon);
  Json slots = Json::makeArray();
  for (const auto& sl : m.slots) {
    Json js = Json::makeObject();
    if (!sl.plugin.empty()) js.object["plugin"] = pluginToJson(sl.plugin);
    js.object["enabled"] = boolean(sl.enabled);
    js.object["mix"] = num(sl.mix);
    slots.array.push_back(std::move(js));
  }
  jm.object["slots"] = std::move(slots);
  Json sends = Json::makeArray();
  for (const auto& sd : m.sends) {
    Json js = Json::makeObject();
    js.object["to"] = num(sd.to);
    js.object["level"] = num(sd.level);
    if (sd.sidechain) js.object["sidechain"] = boolean(true);
    if (sd.preFader) js.object["preFader"] = boolean(true);
    if (sd.stageRoom) js.object["stageRoom"] = boolean(true);  // the Stage room send
    sends.array.push_back(std::move(js));
  }
  jm.object["sends"] = std::move(sends);
  // --- strip EQ (0.5 FL import cycle; additive, written only when non-default) ---
  {
    const MixerTrack def;
    if (m.eqGain != def.eqGain || m.eqFreq != def.eqFreq || m.eqQ != def.eqQ) {
      Json eq = Json::makeArray();
      for (int b = 0; b < 3; ++b) {
        Json jb = Json::makeObject();
        jb.object["gain"] = num(m.eqGain[static_cast<size_t>(b)]);
        jb.object["freq"] = num(m.eqFreq[static_cast<size_t>(b)]);
        jb.object["q"] = num(m.eqQ[static_cast<size_t>(b)]);
        eq.array.push_back(std::move(jb));
      }
      jm.object["eq"] = std::move(eq);
    }
  }
  // --- J-12 mixer parity (additive, written only when non-default) ---
  if (m.icon || m.flIcon) jm.object["icon"] = num(m.icon);
  if (m.disabled) jm.object["disabled"] = boolean(true);
  if (m.dock) jm.object["dock"] = num(m.dock);
  if (m.latencyOffset) jm.object["latencyOffset"] = num(m.latencyOffset);
  if (!m.threaded) jm.object["threaded"] = boolean(false);
  if (!m.group.empty()) jm.object["group"] = str(m.group);
  if (!m.group.empty() && m.groupLink) jm.object["groupLink"] = boolean(true);  // K-238
  if (m.hidden) jm.object["hidden"] = boolean(true);  // GitHub #58: how the mixer shows the strip
  if (m.narrow) jm.object["narrow"] = boolean(true);
  if (!m.group.empty() && m.groupFolded) jm.object["groupFolded"] = boolean(true);
  if (m.freeze.frozen()) jm.object["freeze"] = freezeToJson(m.freeze);  // K-87
  if (m.hibernate) jm.object["hibernate"] = boolean(true);
  if (Json mc = macrosToJson(m.macros); !mc.is(Json::Type::Null)) jm.object["macros"] = std::move(mc);  // K-245
  return jm;
}

Json lfoToJson(const SongLfo& l) {
  const SongLfo d;
  Json jl = Json::makeObject();
  if (!l.name.empty()) jl.object["name"] = str(l.name);
  if (l.colour != d.colour) jl.object["colour"] = num(l.colour);
  if (l.on != d.on) jl.object["on"] = boolean(l.on);
  if (l.shape != d.shape) jl.object["shape"] = num(static_cast<int>(l.shape));
  if (l.pulseWidth != d.pulseWidth) jl.object["pulseWidth"] = num(l.pulseWidth);
  if (l.randomSteps != d.randomSteps) jl.object["randomSteps"] = num(l.randomSteps);
  if (l.seed != d.seed) jl.object["seed"] = num(static_cast<double>(l.seed));
  if (!l.points.empty()) {
    Json pts = Json::makeArray();
    for (const auto& p : l.points) {
      Json jp = Json::makeArray();
      jp.array.push_back(num(p.x));
      jp.array.push_back(num(p.y));
      jp.array.push_back(num(p.curve));
      if (p.kind != kLfoPointCurve) jp.array.push_back(num(p.kind));  // 0.7.9
      pts.array.push_back(std::move(jp));
    }
    jl.object["points"] = std::move(pts);
  }
  if (l.drawnSteps != d.drawnSteps) jl.object["drawnSteps"] = boolean(l.drawnSteps);
  if (l.tempoSync != d.tempoSync) jl.object["tempoSync"] = boolean(l.tempoSync);
  if (l.rateNote != d.rateNote) jl.object["rateNote"] = num(l.rateNote);
  if (l.rateKind != d.rateKind) jl.object["rateKind"] = num(l.rateKind);
  if (l.rateHz != d.rateHz) jl.object["rateHz"] = num(l.rateHz);
  if (l.phase != d.phase) jl.object["phase"] = num(l.phase);
  if (l.depth != d.depth) jl.object["depth"] = num(l.depth);
  if (l.offset != d.offset) jl.object["offset"] = num(l.offset);
  if (l.smoothMs != d.smoothMs) jl.object["smoothMs"] = num(l.smoothMs);
  if (l.fadeIn != d.fadeIn) jl.object["fadeIn"] = num(l.fadeIn);
  if (l.unipolar != d.unipolar) jl.object["unipolar"] = boolean(l.unipolar);
  if (l.restart != d.restart) jl.object["restart"] = num(static_cast<int>(l.restart));
  if (l.noteChannel != d.noteChannel) jl.object["noteChannel"] = num(l.noteChannel);
  // 0.7.8: Chaos and Random sources (an LFO writes none of these).
  if (l.kind != d.kind) jl.object["kind"] = num(static_cast<int>(l.kind));
  if (l.character != d.character) jl.object["character"] = num(l.character);
  if (l.trigger != d.trigger) jl.object["trigger"] = num(static_cast<int>(l.trigger));
  if (l.centred != d.centred) jl.object["centred"] = boolean(l.centred);
  if (l.quantise != d.quantise) jl.object["quantise"] = num(l.quantise);
  if (l.roll != d.roll) jl.object["roll"] = num(static_cast<double>(l.roll));
  return jl;
}

bool lfoFromJson(const Json& jl, SongLfo& l) {
  l.name = jl.text("name");
  if (l.name.size() > 32) l.name.resize(32);
  l.colour = jsonInt<int>(std::clamp(jl.num("colour", 0), 0.0, static_cast<double>(kLfoColours - 1)));
  l.on = flag(jl, "on", true);
  l.shape = static_cast<LfoShape>(jsonInt<int>(std::clamp(jl.num("shape", 0), 0.0, static_cast<double>(static_cast<int>(LfoShape::Count) - 1))));
  l.pulseWidth = static_cast<float>(std::clamp(jl.num("pulseWidth", 0.5), 0.02, 0.98));
  l.randomSteps = jsonInt<int>(std::clamp(jl.num("randomSteps", 4), 1.0, 16.0));
  l.seed = jsonInt<uint32_t>(std::clamp(jl.num("seed", 1), 0.0, 4294967295.0));
  if (const Json* pts = jl.find("points"); pts && pts->is(Json::Type::Array))
    for (const auto& jp : pts->array) {
      if (!jp.is(Json::Type::Array) || jp.array.size() < 2 || l.points.size() >= static_cast<size_t>(kLfoMaxPoints)) continue;
      auto at = [&](size_t k, double lo, double hi) {
        return k < jp.array.size() && jp.array[k].is(Json::Type::Number) ? static_cast<float>(std::clamp(jp.array[k].number, lo, hi)) : 0.0f;
      };
      LfoPoint pt{at(0, 0, 1), at(1, -1, 1), at(2, -1, 1)};
      pt.kind = static_cast<uint8_t>(std::lround(at(3, 0, 2)));  // 0.7.9 (older files: 0, a curve)
      l.points.push_back(pt);
    }
  l.drawnSteps = flag(jl, "drawnSteps", false);
  l.tempoSync = flag(jl, "tempoSync", true);
  l.rateNote = jsonInt<int>(std::clamp(jl.num("rateNote", 5), 0.0, 9.0));
  l.rateKind = jsonInt<int>(std::clamp(jl.num("rateKind", 0), 0.0, 2.0));
  l.rateHz = static_cast<float>(std::clamp(jl.num("rateHz", 1), 0.01, 40.0));
  l.phase = static_cast<float>(std::clamp(jl.num("phase", 0), 0.0, 360.0));
  l.depth = static_cast<float>(std::clamp(jl.num("depth", 1), 0.0, 1.0));
  l.offset = static_cast<float>(std::clamp(jl.num("offset", 0), -1.0, 1.0));
  l.smoothMs = static_cast<float>(std::clamp(jl.num("smoothMs", 0), 0.0, 500.0));
  l.fadeIn = static_cast<float>(std::clamp(jl.num("fadeIn", 0), 0.0, 8.0));
  l.unipolar = flag(jl, "unipolar", false);
  l.restart = static_cast<LfoRestart>(jsonInt<int>(std::clamp(jl.num("restart", 1), 0.0, static_cast<double>(static_cast<int>(LfoRestart::Count) - 1))));
  l.noteChannel = jsonInt<ObjectId>(std::clamp(jl.num("noteChannel", 0), 0.0, 4e9));
  // 0.7.8: Chaos and Random. A kind this version does not know is left out with its links (the
  // links' source is then missing): it never plays as something else.
  const double kind = jl.num("kind", 0);
  if (kind < 0 || kind >= static_cast<double>(static_cast<int>(LfoKind::Count))) return false;
  l.kind = static_cast<LfoKind>(jsonInt<int>(kind));
  if (l.restart == LfoRestart::EachBar && l.kind != LfoKind::Chaos) l.restart = LfoRestart::OnPlay;
  l.character = static_cast<float>(std::clamp(jl.num("character", 0.5), 0.0, 1.0));
  l.trigger = static_cast<RandomTrigger>(jsonInt<int>(std::clamp(jl.num("trigger", 0), 0.0, static_cast<double>(static_cast<int>(RandomTrigger::Count) - 1))));
  l.centred = flag(jl, "centred", false);
  l.quantise = jsonInt<int>(std::clamp(jl.num("quantise", 0), 0.0, 16.0));
  if (l.quantise < 2) l.quantise = 0;
  l.roll = jsonInt<uint32_t>(std::clamp(jl.num("roll", 0), 0.0, 4294967295.0));
  if (l.kind == LfoKind::Random) l.tempoSync = l.trigger != RandomTrigger::Hz;
  return true;
}

Json channelToJson(const Channel& c) {
  Json jc = Json::makeObject();
  jc.object["id"] = num(c.id);
  jc.object["name"] = str(c.name);
  jc.object["color"] = num(c.color);
  jc.object["plugin"] = pluginToJson(c.plugin);
  jc.object["gain"] = num(c.gain);
  jc.object["pan"] = num(c.pan);
  jc.object["muted"] = boolean(c.muted);
  // The channel pitch: whole semitones under "pitch" (what builds before 0.7.7 read) and the exact
  // value in cents under "pitchCents" when it is not 0 (pitchCents wins when both are there).
  jc.object["pitch"] = num(std::clamp(std::round(static_cast<double>(c.pitchCents) / 100.0), -24.0, 24.0));
  if (c.pitchCents != 0.0f) jc.object["pitchCents"] = num(c.pitchCents);
  if (c.sustainPedal) jc.object["sustainPedal"] = boolean(true);
  jc.object["mixerTrack"] = num(c.mixerTrack);
  if (!c.auxOuts.empty()) {
    Json ao = Json::makeArray();
    for (ObjectId t : c.auxOuts) ao.array.push_back(num(t));
    jc.object["auxOuts"] = std::move(ao);
  }
  if (c.midiOutPort >= 0) jc.object["midiOutPort"] = num(c.midiOutPort);
  if (c.midiInPort >= 0) jc.object["midiInPort"] = num(c.midiInPort);
  if (!c.noteFx.empty()) {
    Json fx = Json::makeArray();
    for (const auto& f : c.noteFx) fx.array.push_back(pluginToJson(f));
    jc.object["noteFx"] = std::move(fx);
  }
  channelExtrasToJson(c, jc);
  return jc;
}

Json songToJson(const Song& s, bool withFlOrigin) {
  Json j = Json::makeObject();
  j.object["format"] = str(kProjectFormatTag);
  // K-224: version 9 adds Stage rooms; a project with one room stays version 8 (older builds open it).
  // grid2-1118: version 10: the clip grid holds one clip a slot, and clips and scenes carry launch settings,
  // follow actions, tempo and time signature: a project with a clip grid (a live area) or any of them.
  j.object["version"] = num(trackmodes::usesGridV10(s) ? kProjectSchemaVersion : s.stageRooms.empty() ? 8 : 9);
  Json info = Json::makeObject();
  info.object["title"] = str(s.info.title);
  info.object["author"] = str(s.info.author);
  info.object["genre"] = str(s.info.genre);
  info.object["url"] = str(s.info.url);
  info.object["comments"] = str(s.info.comments);
  if (s.info.created) info.object["created"] = num(static_cast<double>(s.info.created));
  if (s.info.workTime) info.object["workTime"] = num(static_cast<double>(s.info.workTime));
  j.object["info"] = std::move(info);
  j.object["ppq"] = num(s.ppq);
  j.object["tempo"] = num(s.tempo);
  j.object["timeSigNum"] = num(s.timeSigNum);
  j.object["timeSigDen"] = num(s.timeSigDen);
  j.object["swing"] = num(s.swing);
  if (!s.swingNotes) j.object["swingNotes"] = boolean(false);  // J-138
  if (!s.groove.isDefault()) j.object["groove"] = grooveToJson(s.groove);  // K-151 (absent: the classic step swing)
  // K-5: always written, so a project saved before slides kept to their colour (no key) is told apart.
  j.object["slidesByColour"] = boolean(!s.slidesAnyColour);
  // GitHub #87: always written, so a project saved before slides glided velocity (no key) is told apart.
  j.object["slidesVelocity"] = boolean(s.slidesVelocity);
  // K-114: always written, so a project saved before clips looped one by one (no key) is told apart.
  j.object["clipLoops"] = boolean(true);
  j.object["masterPitch"] = num(s.masterPitch);
  if (!s.autoCrossfades) j.object["autoCrossfades"] = boolean(false);  // J-38
  if (s.panLaw != PanLaw::Balance) j.object["panLaw"] = num(static_cast<double>(s.panLaw));  // the Stage (absent: Balance)
  // The stage is written when it differs from the default, and also when tracks stand on it with no
  // room chosen: "none" is then said outright (a load without it reads distance as the studio's).
  bool stageUsed = false;
  for (const auto& m : s.mixer) {
    stageUsed = stageUsed || m.depth != 0 || m.focus != 0;
    for (const auto& sd : m.sends) stageUsed = stageUsed || sd.stageRoom;
  }
  if (!s.stage.isPlain() || stageUsed || !s.stageRooms.empty()) j.object["stage"] = stageToJson(s.stage);
  if (!s.stageRooms.empty()) {  // K-224: the other Stage rooms (a project with one room has none)
    Json a = Json::makeArray();
    for (const auto& r : s.stageRooms) a.array.push_back(stageToJson(r));
    j.object["stageRooms"] = std::move(a);
  }
  if (!s.flExportGridline.empty()) j.object["flExportGridline"] = Json::makeString(s.flExportGridline);  // K-110
  if (!s.trackReceipt.empty()) {  // J-219: the Track Receipt's typed fields
    Json tr;
    if (jsonParse(s.trackReceipt, tr) && tr.type == Json::Type::Object) j.object["trackReceipt"] = std::move(tr);
  }
  j.object["nextId"] = num(s.nextId);

  Json chans = Json::makeArray();
  for (const auto& c : s.channels) chans.array.push_back(channelToJson(c));
  j.object["channels"] = std::move(chans);

  Json pats = Json::makeArray();
  for (const auto& p : s.patterns) {
    Json jp = Json::makeObject();
    jp.object["id"] = num(p.id);
    jp.object["name"] = str(p.name);
    jp.object["color"] = num(p.color);
    jp.object["stepsPerBar"] = num(p.stepsPerBar);
    jp.object["bars"] = num(p.bars);
    Json steps = Json::makeObject();
    for (const auto& [ch, row] : p.steps) {
      Json arr = Json::makeArray();
      for (uint8_t v : row) arr.array.push_back(num(v));
      steps.object[std::to_string(ch)] = std::move(arr);
    }
    jp.object["steps"] = std::move(steps);
    Json notes = Json::makeObject();
    for (const auto& [ch, ns] : p.notes) {
      Json arr = Json::makeArray();
      for (const auto& n : ns) {
        Json jn = Json::makeObject();
        jn.object["start"] = num(static_cast<double>(n.start));
        jn.object["length"] = num(static_cast<double>(n.length));
        jn.object["pitch"] = num(n.pitch);
        jn.object["velocity"] = num(n.velocity);
        noteToJson(n, jn);
        arr.array.push_back(std::move(jn));
      }
      notes.object[std::to_string(ch)] = std::move(arr);
    }
    jp.object["notes"] = std::move(notes);
    patternExtrasToJson(p, jp);  // pattern automation (see block above)
    pats.array.push_back(std::move(jp));
  }
  j.object["patterns"] = std::move(pats);

  Json clips = Json::makeArray();
  for (const auto& c : s.playlist) {
    Json jc = Json::makeObject();
    jc.object["id"] = num(c.id);
    jc.object["pattern"] = num(c.pattern);
    jc.object["track"] = num(c.track);
    jc.object["start"] = num(static_cast<double>(c.start));
    jc.object["length"] = num(static_cast<double>(c.length));
    clipArrangementToJson(c, jc);  // arrangement fields (see block above)
    clips.array.push_back(std::move(jc));
  }
  j.object["playlist"] = std::move(clips);

  Json tracks = Json::makeArray();
  // K-314 B-03: tracks shown on this computer only are written as empty tracks, the ones at the end not at all.
  size_t trackCount = s.tracks.size();
  while (trackCount > 0 && s.tracks[trackCount - 1].shownHere) {
    --trackCount;
  }
  for (size_t ti = 0; ti < trackCount; ++ti) {
    const PlaylistTrack& t = s.tracks[ti].shownHere ? kEmptyTrack : s.tracks[ti];
    Json jt = Json::makeObject();
    jt.object["name"] = str(t.name);
    jt.object["color"] = num(t.color);
    jt.object["muted"] = boolean(t.muted);
    if (t.height != 1.0f) jt.object["height"] = num(t.height);  // arrangement
    trackExtrasToJson(t, jt);    // live settings, folding (see block above)
    tracks.array.push_back(std::move(jt));
  }
  j.object["tracks"] = std::move(tracks);
  arrangementToJson(s, j);  // automation clips, markers (see block above)

  Json mixer = Json::makeArray();
  for (const auto& m : s.mixer) {
    Json jm = mixerTrackToJson(m);
    mixer.array.push_back(std::move(jm));
  }
  j.object["mixer"] = std::move(mixer);

  Json links = Json::makeArray();
  for (const auto& l : s.links) {
    Json jl = Json::makeObject();
    jl.object["cc"] = num(l.cc);
    jl.object["midiChannel"] = num(l.midiChannel);
    jl.object["owner"] = num(l.owner);
    jl.object["index"] = num(l.index);
    jl.object["min"] = num(l.min);
    jl.object["max"] = num(l.max);
    // Remote control settings: written only when they differ from a plain CC link.
    if (l.source != RemoteSource::CC) jl.object["source"] = num(static_cast<int>(l.source));
    if (l.port >= 0) jl.object["port"] = num(l.port);
    if (l.curve != ModCurve::Linear) jl.object["curve"] = num(static_cast<int>(l.curve));
    if (!l.formula.empty()) jl.object["formula"] = Json::makeString(l.formula);
    if (l.invert) jl.object["invert"] = num(1);
    if (l.takeover != RemoteTakeover::Jump) jl.object["takeover"] = num(static_cast<int>(l.takeover));
    if (l.encoder != RemoteEncoder::Absolute) jl.object["encoder"] = num(static_cast<int>(l.encoder));
    if (l.sensitivity != 1.0f) jl.object["sensitivity"] = num(l.sensitivity);
    if (l.button != RemoteButton::Value) jl.object["button"] = num(static_cast<int>(l.button));
    if (l.action != RemoteAction::None) jl.object["action"] = num(static_cast<int>(l.action));
    if (!l.oscAddress.empty()) jl.object["osc"] = Json::makeString(l.oscAddress);  // OSC links
    if (l.hiRes) jl.object["hires"] = num(1);  // a 14-bit CC
    links.array.push_back(std::move(jl));
  }
  j.object["links"] = std::move(links);

  Json mods = Json::makeArray();
  for (const auto& l : s.modLinks) {
    Json jl = Json::makeObject();
    jl.object["sourceOwner"] = num(l.sourceOwner);
    jl.object["sourceSlot"] = num(l.sourceSlot);
    jl.object["sourceOutput"] = num(l.sourceOutput);
    jl.object["owner"] = num(l.owner);
    jl.object["index"] = num(l.index);
    jl.object["min"] = num(l.min);
    jl.object["max"] = num(l.max);
    jl.object["curve"] = num(static_cast<int>(l.curve));
    if (!l.formula.empty()) jl.object["formula"] = str(l.formula);
    jl.object["smoothMs"] = num(l.smoothMs);
    jl.object["enabled"] = boolean(l.enabled);
    // K-247 (LFO links): written only when not the default, so links made before read and write as before.
    if (l.amount != 0.0f) jl.object["amount"] = num(l.amount);
    if (l.mode != kModAbsolute) jl.object["mode"] = num(l.mode);
    if (l.direction != kModBothWays) jl.object["direction"] = num(l.direction);
    if (l.invert) jl.object["invert"] = boolean(true);
    mods.array.push_back(std::move(jl));
  }
  j.object["modLinks"] = std::move(mods);
  if (!s.lfos.empty()) {  // K-247: the LFO board (written only when it has LFOs; each field only when not the default)
    Json lfos = Json::makeArray();
    for (const auto& l : s.lfos) {
      Json jl = lfoToJson(l);
      jl.object["id"] = num(l.id);
      lfos.array.push_back(std::move(jl));
    }
    j.object["lfos"] = std::move(lfos);
  }
  if (!s.channelGroups.empty()) {  // channel rack display groups (written only when used)
    Json groups = Json::makeArray();
    for (const auto& g : s.channelGroups) groups.array.push_back(str(g));
    j.object["channelGroups"] = std::move(groups);
  }
  if (!s.patternGroups.empty()) {  // J-31 pattern groups
    Json groups = Json::makeArray();
    for (const auto& g : s.patternGroups) {
      Json jg = Json::makeObject();
      jg.object["name"] = str(g.name);
      Json ids = Json::makeArray();
      for (ObjectId id : g.patterns) ids.array.push_back(num(id));
      jg.object["patterns"] = std::move(ids);
      groups.array.push_back(std::move(jg));
    }
    j.object["patternGroups"] = std::move(groups);
  }
  songExtrasToJson(s, j);  // live area, arrangements (see block above)
  if (withFlOrigin && !s.flOrigin.empty()) {  // FL Studio import data for the FL exporter (J-34)
    Json fo = Json::makeObject();
    for (const auto& [k, v] : s.flOrigin.blobs) fo.object[k] = str(b64Encode(v));
    j.object["flOrigin"] = std::move(fo);
  }
  if (!s.collabChat.empty()) {  // K-86 part 2: written only when messages exist (empty projects stay byte-identical)
    Json chat = Json::makeArray();
    for (const auto& m : s.collabChat) chat.array.push_back(chatMessageToJson(m));
    j.object["collabChat"] = std::move(chat);
  }
  return j;
}

// v5 → v6: mixer slot parameters moved from a stride of 100 (mix at 99) to a
// stride of 1000 (mix at 999). Rewrites MIDI and controller link targets that
// point at mixer-slot parameters.
void migrateV5(Json& j) {
  std::vector<double> mixerIds;
  if (const Json* mx = j.find("mixer"); mx && mx->is(Json::Type::Array))
    for (const auto& m : mx->array) mixerIds.push_back(m.num("id", -1));
  auto isMixer = [&](double id) { return std::find(mixerIds.begin(), mixerIds.end(), id) != mixerIds.end(); };
  for (const char* key : {"links", "modLinks"}) {
    auto it = j.object.find(key);
    if (it == j.object.end() || !it->second.is(Json::Type::Array)) continue;
    for (auto& l : it->second.array) {
      double owner = l.num("owner", -1), idx = l.num("index", 0);
      if (!isMixer(owner) || idx < 1000) continue;
      long long old = static_cast<long long>(idx) - 1000, slot = old / 100, k = old % 100;
      l.object["index"] = num(static_cast<double>(1000 + slot * 1000 + (k == 99 ? 999 : k)));
    }
  }
  j.object["version"] = num(6);
}

bool migrateProjectJson(Json& j, std::string* err) {
  if (!j.is(Json::Type::Object)) return fail(err, "root not object");
  if (j.text("format") != kProjectFormatTag) return fail(err, "bad format tag");
  int v = jsonInt<int>(j.num("version", 1));
  if (v > kProjectSchemaVersion)
    return fail(err, "project was saved by a newer Gridline (schema " + std::to_string(v) + ")");
  if (v < 1) return fail(err, "bad schema version");
  if (v == 1) migrateV1(j), v = 2;
  if (v == 2) migrateV2(j), v = 3;
  if (v == 3) j.object["version"] = num(4), v = 4;  // v4 adds optional "links"
  if (v == 4) j.object["version"] = num(5), v = 5;  // v5 adds optional "modLinks", "noteFx", send "sidechain"
  if (v == 5) migrateV5(j), v = 6;
  if (v == 6) j.object["version"] = num(7), v = 7;  // v7: see bitsLawV7 (applied to the loaded song)
  if (v == 7) j.object["version"] = num(8), v = 8;  // v8: see rateLawV8 (applied to the loaded song)
  if (v == 8) j.object["version"] = num(9), v = 9;  // v9 adds optional "stageRooms" and mixer "roomId"
  if (v == 9) {  // v10: grid2-1118 (songFromJson: splitSharedSlots)
    j.object["version"] = num(10), v = 10;
  }
  return true;
}

// K-5: some channel's notes in some pattern hold a slide note and notes of more than one
// colour (a slide there sounded on other colours' notes before slides kept to their colour).
bool slidesMeetOtherColours(const Song& s) {
  for (const auto& pat : s.patterns)
    for (const auto& [ch, ns] : pat.notes) {
      bool slide = false, mixed = false;
      for (const auto& n : ns) slide = slide || n.slide, mixed = mixed || n.color != ns.front().color;
      if (slide && mixed) return true;
    }
  return false;
}

bool slidesChangeVelocity(const Song& s) {
  for (const auto& pat : s.patterns)
    for (const auto& [ch, ns] : pat.notes)
      for (const auto& sl : ns) {
        if (!sl.slide || sl.muted) continue;
        for (const auto& n : ns)
          if (!n.slide && !n.muted && (s.slidesAnyColour || n.color == sl.color) && n.velocity != sl.velocity &&
              n.start <= sl.start && n.start + n.length > sl.start)
            return true;
      }
  return false;
}

// Moves normalized values that target parameter k of gl.bitcrush slots (automation
// clips and pattern lanes, MIDI and internal controller link ranges) through law
// (old normalized -> new normalized), so they play the same values after a
// descriptor change. Values stored in plugin states are migrated elsewhere.
static void remapBitcrushNormalized(Song& s, int k, double (*law)(double)) {
  auto targets = [&](ObjectId owner, uint32_t index) {
    if (index < kSlotBase || index >= kSlotBase + kSlotStride * static_cast<uint32_t>(kEffectSlots)) return false;
    const uint32_t slot = (index - kSlotBase) / kSlotStride, q = (index - kSlotBase) % kSlotStride;
    for (const auto& m : s.mixer)
      if (m.id == owner) return q == static_cast<uint32_t>(k) && m.slots[slot].plugin.type == "gl.bitcrush";
    return false;
  };
  auto range = [&](float& mn, float& mx, std::vector<AutoPoint>* pts) {
    const double a0 = mn, a1 = mx, b0 = law(a0), b1 = law(a1);
    if (pts)
      for (auto& p : *pts) {
        const double n = a0 + p.value * (a1 - a0);
        p.value = b1 != b0 ? static_cast<float>((law(n) - b0) / (b1 - b0)) : p.value;
      }
    mn = static_cast<float>(b0), mx = static_cast<float>(b1);
  };
  for (auto& c : s.automations)
    if (targets(c.owner, c.index)) range(c.min, c.max, &c.points);
  for (auto& pat : s.patterns)
    for (auto& lane : pat.automation)
      if (targets(lane.owner, lane.index))
        for (auto& p : lane.points) p.value = static_cast<float>(law(p.value));
  for (auto& l : s.links)
    if (targets(l.owner, l.index)) range(l.min, l.max, nullptr);
  for (auto& l : s.modLinks)
    if (targets(l.owner, l.index)) range(l.min, l.max, nullptr);
}

// v6 -> v7 (J-98): gl.bitcrush's Bits knob changed from a linear (1..24) to a
// logarithmic law, so most of the travel covers the audible 1..12 bits.
static double bitsLawV7(double n) { return std::log(1.0 + 23.0 * std::clamp(n, 0.0, 1.0)) / std::log(24.0); }

// v7 -> v8 (J-104): index 1 of gl.bitcrush was "Downsample" (linear 1..50, a host
// rate divider); it is now Rate in Hz (log 50 Hz .. Off). Old normalized values
// become the rate reference / N (the plugin states are converted the same way by
// normalizePluginState). More division is a lower rate, so ranges turn around.
static double rateLawV8(double n) {
  const double N = 1.0 + 49.0 * std::clamp(n, 0.0, 1.0);
  const PluginInfo* info = findPlugin("gl.bitcrush");
  return info ? info->params[pp::BcRate].toNormalized(bitcrushRateOfDivider(N)) : n;
}

void mixerTrackFromJson(const Json& jm, MixerTrack& m) {
  m.name = jm.text("name");
  m.color = color(jm, m.color);
  m.volume = static_cast<float>(std::clamp(jm.num("volume", 0.8), 0.0, 1.25));
  m.pan = static_cast<float>(std::clamp(jm.num("pan", 0), -1.0, 1.0));
  m.stereoSep = static_cast<float>(std::clamp(jm.num("stereoSep", 0), -1.0, 1.0));
  if (const double r = jm.num("roomId", 0); std::isfinite(r) && r >= kFirstObjectId && r <= 4e9) m.room = static_cast<ObjectId>(r);  // K-224
  if (const double dp = jm.num("depth", 0); std::isfinite(dp)) m.depth = static_cast<float>(std::clamp(dp, 0.0, 1.0));  // Stage distance
  if (const double fc = jm.num("focus", 0); std::isfinite(fc)) m.focus = static_cast<float>(std::clamp(fc, -1.0, 1.0));  // Stage focus
  m.muted = flag(jm, "muted");
  m.solo = flag(jm, "solo");
  m.invertPolarity = flag(jm, "invertPolarity");
  m.swapLR = flag(jm, "swapLR");
  m.input = jsonInt<int>(std::clamp(jm.num("input", -1), -1.0, static_cast<double>(kMaxInputChannels - 1)));
  if (const Json* st = jm.find("inputStereo"); st && st->is(Json::Type::Bool)) m.inputStereo = st->boolean;
  m.monitor = jsonInt<uint8_t>(std::clamp(jm.num("monitor", 0), 0.0, static_cast<double>(static_cast<int>(InputMonitor::Count) - 1)));
  m.recordSafe = flag(jm, "recordSafe");  // K-123
  m.armed = flag(jm, "armed") && !m.recordSafe;
  m.inputTrim = static_cast<float>(std::clamp(jm.num("inputTrim", 0), static_cast<double>(kInputTrimMin), static_cast<double>(kInputTrimMax)));  // K-123
  if (!std::isfinite(m.inputTrim)) m.inputTrim = 0.0f;
  m.inputInvert = flag(jm, "inputInvert");
  m.soloAlone = flag(jm, "soloAlone");  // J-31
  m.separator = flag(jm, "separator");  // J-39
  m.locked = flag(jm, "locked");
  m.flIcon = jsonInt<int>(std::clamp(jm.num("flIcon", 0), 0.0, 65535.0));
  m.recPoint = jsonInt<uint8_t>(std::clamp(jm.num("recPoint", 0), 0.0, static_cast<double>(static_cast<int>(RecordPoint::Count) - 1)));
  if (const Json* sl = jm.find("slots"); sl && sl->is(Json::Type::Array))
    for (size_t i = 0; i < sl->array.size() && i < m.slots.size(); ++i) {
      const Json& js = sl->array[i];
      m.slots[i].plugin = pluginFromJson(js.find("plugin"));
      if (!m.slots[i].plugin.empty() && !findPlugin(m.slots[i].plugin.type) && !isHostedType(m.slots[i].plugin.type))
        m.slots[i].plugin = {};  // unknown built-in type (a missing hosted plugin keeps its state)
      m.slots[i].enabled = flag(js, "enabled", true);
      m.slots[i].mix = static_cast<float>(std::clamp(js.num("mix", 1), 0.0, 1.0));
    }
  if (const Json* sd = jm.find("sends"); sd && sd->is(Json::Type::Array))
    for (const auto& js : sd->array)
      m.sends.push_back({jsonInt<ObjectId>(js.num("to", 0)), static_cast<float>(std::clamp(js.num("level", 1), 0.0, 1.25)),
                         flag(js, "sidechain"), flag(js, "preFader"),
                         flag(js, "stageRoom") || flag(js, "depthScaled")});  // depthScaled: an unreleased early Stage build
  // --- strip EQ (0.5 FL import cycle; absent = defaults) ---
  if (const Json* eq = jm.find("eq"); eq && eq->is(Json::Type::Array))
    for (size_t b = 0; b < eq->array.size() && b < 3; ++b) {
      const Json& jb = eq->array[b];
      m.eqGain[b] = static_cast<float>(std::clamp(jb.num("gain", 0), -18.0, 18.0));
      m.eqFreq[b] = static_cast<float>(std::clamp(jb.num("freq", kStripEqDefFreq[b]), 20.0, 20000.0));
      m.eqQ[b] = static_cast<float>(std::clamp(jb.num("q", kStripEqDefQ), 0.1, 10.0));
    }
  m.icon = iconFromJson(jm, m.flIcon);  // J-211 (flIcon is read above)
  m.disabled = flag(jm, "disabled");
  m.dock = jsonInt<int>(std::clamp(jm.num("dock", 0), -1.0, 1.0));
  m.latencyOffset = jsonInt<int>(std::clamp(jm.num("latencyOffset", 0), 0.0, static_cast<double>(kMaxManualLatency)));
  m.threaded = flag(jm, "threaded", true);
  m.group = jm.text("group");
  m.groupLink = !m.group.empty() && flag(jm, "groupLink");  // K-238
  m.hidden = flag(jm, "hidden");  // GitHub #58
  m.narrow = flag(jm, "narrow");
  m.groupFolded = !m.group.empty() && flag(jm, "groupFolded");
  m.freeze = freezeFromJson(jm.find("freeze"));  // K-87
  m.hibernate = flag(jm, "hibernate");
  macrosFromJson(jm.find("macros"), m.macros);  // K-245
}

// ---- J-111: one element at a time. Collaboration applies a collaborator's
// edit to the open song element by element; `s` is the song the references
// are checked against (the song being read, or the open song). The same code
// as a whole-project load, so both give the same element.
void channelFromJson(const Json& jc, Channel& c, const Song& s) {
  c.name = jc.text("name", "Channel");
  c.color = color(jc, c.color);
  c.plugin = pluginFromJson(jc.find("plugin"));
  c.gain = static_cast<float>(std::clamp(jc.num("gain", 0.78), 0.0, 1.25));
  c.pan = static_cast<float>(std::clamp(jc.num("pan", 0.0), -1.0, 1.0));
  c.muted = flag(jc, "muted");
  c.pitchCents = channelPitchFromJson(jc);
  c.sustainPedal = flag(jc, "sustainPedal");
  c.mixerTrack = jsonInt<ObjectId>(jc.num("mixerTrack", 0));
  if (!s.mixerTrack(c.mixerTrack)) c.mixerTrack = s.masterId();
  if (const Json* ao = jc.find("auxOuts"); ao && ao->is(Json::Type::Array))
    for (const auto& jt : ao->array) {
      ObjectId t = jt.is(Json::Type::Number) ? jsonInt<ObjectId>(jt.number) : kNoId;
      c.auxOuts.push_back(t != kNoId && s.mixerTrack(t) ? t : kNoId);
      if (c.auxOuts.size() >= 32) break;
    }
  c.midiOutPort = jsonInt<int>(std::clamp(jc.num("midiOutPort", -1.0), -1.0, 255.0));
  c.midiInPort = jsonInt<int>(std::clamp(jc.num("midiInPort", -1.0), -1.0, 255.0));
  if (const Json* fx = jc.find("noteFx"); fx && fx->is(Json::Type::Array))
    for (const auto& jf : fx->array) {
      PluginState ps = pluginFromJson(&jf);
      const PluginInfo* pi = findPlugin(ps.type);
      if (pi && pi->kind == PluginKind::NoteFx && c.noteFx.size() < static_cast<size_t>(kMaxNoteFx))
        c.noteFx.push_back(std::move(ps));
    }
  channelExtrasFromJson(jc, c);
}

bool patternFromJson(const Json& jp, Pattern& p, const Song& s) {
  p.name = jp.text("name", "Pattern");
  p.color = color(jp, p.color);
  p.stepsPerBar = jsonInt<int>(jp.num("stepsPerBar", 16));
  p.bars = jsonInt<int>(jp.num("bars", 1));
  if (p.stepsPerBar <= 0 || p.stepsPerBar > kMaxStepsPerBar || p.bars <= 0 || p.bars > kMaxPatternBars) return false;
  forRows(jp, "steps", [&](ObjectId ch, const Json& v) {
    if (!s.channel(ch) || !v.is(Json::Type::Array)) return;
    std::vector<uint8_t> row;
    for (const auto& e : v.array) {
      double x = e.is(Json::Type::Number) ? e.number : 0;
      row.push_back(static_cast<uint8_t>(x < 0 ? 0 : (x > 127 ? 127 : x)));
    }
    p.steps[ch] = std::move(row);
  });
  forRows(jp, "notes", [&](ObjectId ch, const Json& v) {
    if (!s.channel(ch) || !v.is(Json::Type::Array)) return;
    std::vector<Note> ns;
    for (const auto& e : v.array) {
      Note n;
      n.start = std::max<Tick>(0, jsonTick(e.num("start", 0)));
      n.length = std::max<Tick>(1, jsonTick(e.num("length", s.ppq)));
      n.pitch = jsonInt<int>(std::clamp(e.num("pitch", 60), 0.0, 127.0));
      n.velocity = jsonInt<int>(std::clamp(e.num("velocity", 100), 0.0, 127.0));
      noteFromJson(e, n);
      ns.push_back(n);
    }
    p.notes[ch] = std::move(ns);
  });
  patternExtrasFromJson(jp, p, s);  // pattern automation (see block above)
  return true;
}

int clipFromJson(const Json& jc, Clip& c, const Song& s) {
  c.pattern = jsonInt<ObjectId>(jc.num("pattern", 0));
  c.track = jsonInt<int>(jc.num("track", 0));
  c.start = jsonTick(jc.num("start", 0));
  c.length = jsonTick(jc.num("length", 0));
  clipArrangementFromJson(jc, c);
  if (c.kind == ClipKind::Audio && !s.channel(c.source)) return 1;
  if (c.kind == ClipKind::Automation && !s.automation(c.source)) return 1;
  if (c.kind == ClipKind::Pattern && !s.pattern(c.pattern)) return -2;
  if (c.track < 0 || c.track >= kMaxPlaylistTracks || c.start < 0 || c.length < 0) return -3;
  return 0;
}

void automationClipFromJson(const Json& ja, AutomationClip& a) {
  a.name = ja.text("name", "Automation");
  a.color = color(ja, a.color);
  a.owner = jsonInt<ObjectId>(std::max(0.0, ja.num("owner", 0)));
  a.index = jsonInt<uint32_t>(std::max(0.0, ja.num("index", 0)));
  a.min = static_cast<float>(std::clamp(ja.num("min", 0), 0.0, 1.0));
  a.max = static_cast<float>(std::clamp(ja.num("max", 1), 0.0, 1.0));
  a.loop = std::max<Tick>(0, jsonTick(ja.num("loop", 0)));
  a.flCurves = flag(ja, "flCurves");
  a.events = flag(ja, "events");
  if (const Json* pts = ja.find("points"); pts && pts->is(Json::Type::Array))
    for (const auto& jp : pts->array) {
      if (!jp.is(Json::Type::Array) || jp.array.size() < 2) continue;
      auto at = [&](size_t i, double d) { return i < jp.array.size() && jp.array[i].is(Json::Type::Number) ? jp.array[i].number : d; };
      AutoPoint p;
      p.pos = std::max<Tick>(0, jsonTick(at(0, 0)));
      p.value = static_cast<float>(std::clamp(at(1, 0.5), 0.0, 1.0));
      p.tension = static_cast<float>(std::clamp(at(2, 0), -1.0, 1.0));
      p.shape = static_cast<AutoShape>(static_cast<int>(std::clamp(at(3, 0), 0.0, static_cast<double>(static_cast<int>(AutoShape::Count) - 1))));
    if (const double fr = at(4, 0); fr > 0 && fr < 1) p.frac = std::min(static_cast<float>(fr), 0.99999994f);  // J-159
      if (const double fi = at(5, 0); std::isfinite(fi) && std::fabs(fi) < 1e-6) p.fine = fi;  // J-163
      a.points.push_back(p);
    }
  std::stable_sort(a.points.begin(), a.points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.at() < y.at(); });
}

bool songFromJson(const Json& src, Song& out, std::string* err) {
  Json j = src;
  const int srcVersion = jsonInt<int>(src.num("version", 1));
  if (!migrateProjectJson(j, err)) return false;
  if (!standin::resolving()) {
    host::xformat::resolveProjectJson(j);  // K-78: a plugin missing here in its saved format opens in another
    standin::resolveProjectJson(j);        // missing Serum / Kickstart <-> stand-ins
  }
  Song s;
  s.mixer.clear();
  if (const Json* info = j.find("info"); info && info->is(Json::Type::Object)) {
    s.info.title = info->text("title", "Untitled");
    s.info.author = info->text("author");
    s.info.genre = info->text("genre");
    s.info.url = info->text("url");
    s.info.comments = info->text("comments");
    s.info.created = jsonInt<int64_t>(info->num("created", 0));
    s.info.workTime = jsonInt<int64_t>(info->num("workTime", 0));
  }
  s.ppq = jsonInt<int>(j.num("ppq", kDefaultPpq));
  if (!isValidPpq(s.ppq)) return fail(err, "unsupported PPQ " + std::to_string(s.ppq));
  s.tempo = j.num("tempo", 140.0);
  auto tempoDesc = describeParam(s, songParam(SongParam::Tempo));
  if (s.tempo < tempoDesc->min || s.tempo > tempoDesc->max) return fail(err, "tempo out of range");
  s.timeSigNum = jsonInt<int>(j.num("timeSigNum", 4));
  s.timeSigDen = jsonInt<int>(j.num("timeSigDen", 4));
  if (s.timeSigNum < 1 || s.timeSigDen < 1) return fail(err, "bad time signature");
  s.swing = static_cast<float>(std::clamp(j.num("swing", 0), 0.0, 1.0));
  s.swingNotes = flag(j, "swingNotes", true);  // J-138
  if (const Json* g = j.find("groove"); g && g->is(Json::Type::Object)) s.groove = grooveFromJson(*g);  // K-151
  s.masterPitch = static_cast<float>(std::clamp(j.num("masterPitch", 0), -12.0, 12.0));
  s.autoCrossfades = flag(j, "autoCrossfades", true);  // J-38
  if (const double pl = j.num("panLaw", 0); std::isfinite(pl)) s.panLaw = static_cast<PanLaw>(std::clamp(static_cast<int>(pl), 0, 2));
  s.stage = StageSettings{};
  bool roomKnown = false;  // the project says which room it plays in (else: from its tracks, below)
  s.stageRooms.clear();
  if (const Json* st = j.find("stage"); st && st->is(Json::Type::Object)) {
    stageFromJson(*st, s.stage);
    roomKnown = true;
  }
  if (const Json* sr = j.find("stageRooms"); sr && sr->is(Json::Type::Array))  // K-224
    for (const Json& r : sr->array) {
      if (!r.is(Json::Type::Object) || s.stageRooms.size() >= 255) continue;
      StageSettings room;
      stageFromJson(r, room);
      if (room.id == kNoId) continue;  // a room the tracks cannot name
      s.stageRooms.push_back(std::move(room));
    }
  if (const Json* tr = j.find("trackReceipt"); tr && tr->type == Json::Type::Object) s.trackReceipt = jsonCompact(*tr);  // J-219
  s.flExportGridline = j.text("flExportGridline");  // K-110

  std::set<ObjectId> ids;
  ObjectId maxId = kSongId;
  auto takeId = [&](const Json& o, ObjectId& dst) {
    double v = o.num("id", 0);
    if (v < kFirstObjectId || v > 4e9) return false;
    dst = static_cast<ObjectId>(v);
    maxId = std::max(maxId, dst);
    return ids.insert(dst).second;  // IDs are unique across all object kinds
  };

  if (const Json* mixer = j.find("mixer")) {
    if (!mixer->is(Json::Type::Array)) return fail(err, "mixer not array");
    for (const auto& jm : mixer->array) {
      MixerTrack m;
      if (!takeId(jm, m.id)) return fail(err, "mixer track has missing or duplicate id");
      mixerTrackFromJson(jm, m);
      s.mixer.push_back(std::move(m));
    }
  }
  for (auto& m : s.mixer) {
    for (const auto& sd : m.sends)
      if (!s.mixerTrack(sd.to)) return fail(err, "mixer send references missing track");
  }
  {
    // K-224: room ids live in the same id space (never handed out again); a room whose id clashes
    // with another object or room loses it, so its tracks fall back to the first room.
    auto keepRoomId = [&](StageSettings& r) {
      if (r.id == kNoId) return;
      if (!ids.insert(r.id).second) {
        r.id = kNoId;
        return;
      }
      maxId = std::max(maxId, r.id);
    };
    keepRoomId(s.stage);
    for (auto& r : s.stageRooms) keepRoomId(r);
    s.stageRooms.erase(std::remove_if(s.stageRooms.begin(), s.stageRooms.end(), [](const StageSettings& r) { return r.id == kNoId; }),
                       s.stageRooms.end());
    if (!s.stageRooms.empty() && s.stage.id == kNoId) s.stage.id = ++maxId;  // never: written by every room-aware save
    // A track naming a room that is gone stands in the first room.
    for (auto& m : s.mixer) {
      bool known = m.room == kNoId || m.room == s.stage.id;
      for (const auto& r : s.stageRooms) known = known || r.id == m.room;
      if (!known) m.room = kNoId;
    }
  }
  // A project from an early Stage build that placed tracks without choosing a room plays in the room
  // its distance was made for (the studio, natural depth), so it sounds as it did.
  if (!roomKnown)
    for (const auto& m : s.mixer) {
      bool used = m.depth != 0 || m.focus != 0;
      for (const auto& sd : m.sends) used = used || sd.stageRoom;
      if (used) {
        s.stage.roomType = static_cast<uint8_t>(rooms::kStudio);
        break;
      }
    }
  {
    // Refuse routing cycles (the engine needs a DAG).
    for (auto& m : s.mixer) {
      MixerTrack saved = m;
      m.sends.clear();
      for (const auto& sd : saved.sends) {
        if (!canRoute(s, m.id, sd.to)) return fail(err, "mixer routing has a cycle");
        m.sends.push_back(sd);
      }
    }
  }

  if (const Json* groups = j.find("channelGroups"); groups && groups->is(Json::Type::Array))
    for (const auto& g : groups->array)
      if (g.is(Json::Type::String)) s.channelGroups.push_back(g.str);
  if (const Json* chans = j.find("channels")) {
    if (!chans->is(Json::Type::Array)) return fail(err, "channels not array");
    for (const auto& jc : chans->array) {
      Channel c;
      if (!takeId(jc, c.id)) return fail(err, "channel has missing or duplicate id");
      channelFromJson(jc, c, s);
      s.channels.push_back(c);
    }
    // Channel extras that reference other objects: drop layer children that do
    // not exist (or the layer itself) and groups that are not defined.
    for (auto& c : s.channels) {
      std::vector<ObjectId> kids;
      for (ObjectId k : c.layer)
        if (k != c.id && s.channel(k) && std::find(kids.begin(), kids.end(), k) == kids.end()) kids.push_back(k);
      c.layer = std::move(kids);
      if (c.group >= static_cast<int>(s.channelGroups.size())) c.group = -1;
    }
  }

  if (const Json* pats = j.find("patterns")) {
    if (!pats->is(Json::Type::Array)) return fail(err, "patterns not array");
    for (const auto& jp : pats->array) {
      Pattern p;
      if (!takeId(jp, p.id)) return fail(err, "pattern has missing or duplicate id");
      if (!patternFromJson(jp, p, s)) return fail(err, "bad pattern dims");
      s.patterns.push_back(std::move(p));
    }
  }
  if (const Json* groups = j.find("patternGroups"); groups && groups->is(Json::Type::Array))  // J-31
    for (const auto& jg : groups->array) {
      PatternGroup g;
      g.name = jg.text("name", "Group");
      if (const Json* ids = jg.find("patterns"); ids && ids->is(Json::Type::Array))
        for (const auto& v : ids->array) {
          const ObjectId id = v.is(Json::Type::Number) ? jsonInt<ObjectId>(v.number) : kNoId;
          if (s.pattern(id) && std::find(g.patterns.begin(), g.patterns.end(), id) == g.patterns.end()) g.patterns.push_back(id);
        }
      if (!g.patterns.empty()) s.patternGroups.push_back(std::move(g));  // J-36: like the editing ops, no empty groups
    }

  s.tracks.clear();
  if (const Json* tracks = j.find("tracks"); tracks && tracks->is(Json::Type::Array))
    for (const auto& jt : tracks->array) {
      if (s.tracks.size() >= static_cast<size_t>(kMaxPlaylistTracks)) break;
      PlaylistTrack t;
      t.name = jt.text("name");
      t.color = color(jt, t.color);
      t.muted = flag(jt, "muted");
      t.height = static_cast<float>(std::clamp(jt.num("height", 1), 0.25, 4.0));  // arrangement
      trackExtrasFromJson(jt, t);  // live settings, folding (see block above)
      s.tracks.push_back(t);
    }
  if (s.tracks.empty()) s.tracks.resize(kDefaultPlaylistTracks);
  if (!arrangementFromJson(j, s, takeId, err)) return false;  // automation clips, markers (before the clips)

  if (const Json* clips = j.find("playlist")) {
    if (!clips->is(Json::Type::Array)) return fail(err, "playlist not array");
    for (const auto& jc : clips->array) {
      Clip c;
      if (!takeId(jc, c.id)) return fail(err, "clip has missing or duplicate id");
      const int ok = clipFromJson(jc, c, s);  // arrangement fields; audio / automation clips with a missing source are dropped
      if (ok == 1) continue;
      if (ok == -2) return fail(err, "clip references missing pattern");
      if (ok == -3) return fail(err, "bad clip position");
      if (static_cast<size_t>(c.track) >= s.tracks.size()) s.tracks.resize(static_cast<size_t>(c.track) + 1);
      s.playlist.push_back(c);
    }
  }
  if (const Json* links = j.find("links"); links && links->is(Json::Type::Array))
    for (const auto& jl : links->array) {
      ControllerLink l;
      l.cc = jsonInt<int>(std::clamp(jl.num("cc", 1), 0.0, 127.0));
      l.midiChannel = jsonInt<int>(std::clamp(jl.num("midiChannel", -1), -1.0, 15.0));
      l.owner = jsonInt<ObjectId>(jl.num("owner", 0));
      l.index = jsonInt<uint32_t>(jl.num("index", 0));
      l.min = static_cast<float>(std::clamp(jl.num("min", 0), 0.0, 1.0));
      l.max = static_cast<float>(std::clamp(jl.num("max", 1), 0.0, 1.0));
      auto en = [&jl](const char* key, int count) {
        return jsonInt<int>(std::clamp(jl.num(key, 0), 0.0, static_cast<double>(count - 1)));
      };
      l.source = static_cast<RemoteSource>(en("source", static_cast<int>(RemoteSource::Count)));
      l.port = jsonInt<int>(std::clamp(jl.num("port", -1), -1.0, 255.0));
      l.curve = static_cast<ModCurve>(en("curve", static_cast<int>(ModCurve::Count)));
      l.formula = jl.text("formula");
      l.invert = jl.num("invert", 0) != 0;
      l.takeover = static_cast<RemoteTakeover>(en("takeover", static_cast<int>(RemoteTakeover::Count)));
      l.encoder = static_cast<RemoteEncoder>(en("encoder", static_cast<int>(RemoteEncoder::Count)));
      l.sensitivity = static_cast<float>(std::clamp(jl.num("sensitivity", 1), 0.01, 64.0));
      l.button = static_cast<RemoteButton>(en("button", static_cast<int>(RemoteButton::Count)));
      l.action = static_cast<RemoteAction>(en("action", static_cast<int>(RemoteAction::Count)));
      l.oscAddress = jl.text("osc");
      l.hiRes = jl.num("hires", 0) != 0 && l.source == RemoteSource::CC && l.cc < 32;
      if (l.source == RemoteSource::Nrpn || l.source == RemoteSource::Rpn)  // a parameter number of 14 bits
        l.cc = jsonInt<int>(std::clamp(jl.num("cc", 1), 0.0, 16383.0));
      if (l.source == RemoteSource::Osc && l.oscAddress.empty()) continue;  // an OSC link without an address
      // Drop links to deleted targets (action links have no target).
      if (l.action != RemoteAction::None || describeParam(s, {l.owner, l.index})) s.links.push_back(l);
    }
  if (const Json* lfos = j.find("lfos"); lfos && lfos->is(Json::Type::Array))  // K-247: the LFO board (before the links that use it)
    for (const auto& jl : lfos->array) {
      if (s.lfos.size() >= static_cast<size_t>(kMaxSongLfos) || !jl.is(Json::Type::Object)) break;
      SongLfo l;
      if (!takeId(jl, l.id)) return fail(err, "LFO has missing or duplicate id");
      if (!lfoFromJson(jl, l)) continue;
      if (!s.channel(l.noteChannel)) l.noteChannel = kNoId;
      s.lfos.push_back(std::move(l));
    }
  if (const Json* mods = j.find("modLinks"); mods && mods->is(Json::Type::Array))
    for (const auto& jl : mods->array) {
      ModLink l;
      l.sourceOwner = jsonInt<ObjectId>(jl.num("sourceOwner", 0));
      l.sourceSlot = jsonInt<int>(std::clamp(jl.num("sourceSlot", -1), static_cast<double>(kSongLfoSlot), static_cast<double>(kEffectSlots - 1)));  // K-169: -2 = layer macro; K-247: -3 = a board LFO
      l.sourceOutput = jsonInt<int>(std::clamp(jl.num("sourceOutput", 0), 0.0, static_cast<double>(kMaxControllerOutputs - 1)));
      l.owner = jsonInt<ObjectId>(jl.num("owner", 0));
      l.index = jsonInt<uint32_t>(jl.num("index", 0));
      l.min = static_cast<float>(std::clamp(jl.num("min", 0), 0.0, 1.0));
      l.max = static_cast<float>(std::clamp(jl.num("max", 1), 0.0, 1.0));
      l.curve = static_cast<ModCurve>(jsonInt<int>(std::clamp(jl.num("curve", 0), 0.0, static_cast<double>(static_cast<int>(ModCurve::Count) - 1))));
      l.formula = jl.text("formula");
      l.smoothMs = static_cast<float>(std::clamp(jl.num("smoothMs", 0), 0.0, 10000.0));
      l.enabled = flag(jl, "enabled", true);
      l.amount = static_cast<float>(std::clamp(jl.num("amount", 0), -1.0, 1.0));  // K-247
      l.mode = jl.num("mode", 0) >= 0.5 ? kModAround : kModAbsolute;
      l.direction = static_cast<uint8_t>(jsonInt<int>(std::clamp(jl.num("direction", 0), 0.0, static_cast<double>(kModDownOnly))));
      l.invert = flag(jl, "invert", false);
      // Drop links whose source or target no longer exists, and duplicate targets (K-247: an LFO's
      // second link to the same target).
      if (!describeParam(s, {l.owner, l.index}) || !isControllerSource(s, l.sourceOwner, l.sourceSlot)) continue;
      if (l.isLfo() ? findLfoLink(s, l.sourceOwner, l.owner, l.index) != nullptr : findModLink(s, l.owner, l.index) != nullptr) continue;
      s.modLinks.push_back(std::move(l));
    }
  if (!songExtrasFromJson(j, s, takeId, err)) return false;  // live area, arrangements (see block above)
  if (const Json* fo = j.find("flOrigin"); fo && fo->is(Json::Type::Object))
    for (const auto& [k, v] : fo->object) {
      std::string bytes;
      if (v.is(Json::Type::String) && b64Decode(v.str, bytes)) s.flOrigin.blobs[k] = std::move(bytes);
    }
  if (const Json* cc = j.find("collabChat"); cc && cc->is(Json::Type::Array)) {
    for (const auto& jm : cc->array) {
      ChatMessage m;
      if (chatMessageFromJson(jm, m)) s.collabChat.push_back(std::move(m));
    }
    std::sort(s.collabChat.begin(), s.collabChat.end(), chatMessageLess);
  }
  s.nextId = std::max<ObjectId>(jsonInt<ObjectId>(j.num("nextId", 0)), maxId + 1);
  trackmodes::dropStaleLinks(s);  // PL-08: links to deleted mixer tracks / channels
  if (srcVersion < 7) remapBitcrushNormalized(s, pp::BcBits, bitsLawV7);
  if (srcVersion < 8) remapBitcrushNormalized(s, pp::BcRate, rateLawV8);
  // K-5: slide notes glide only the notes of their own colour. A project saved before that
  // (no "slidesByColour") keeps sliding every colour when a slide there meets other colours.
  if (const Json* sc = j.find("slidesByColour"); sc && sc->is(Json::Type::Bool))
    s.slidesAnyColour = !sc->boolean;
  else
    s.slidesAnyColour = slidesMeetOtherColours(s);
  // GitHub #87: slide notes glide velocity. A project saved before that (no "slidesVelocity") keeps
  // its slides on pitch alone when one of them would change a note's velocity.
  if (const Json* sv = j.find("slidesVelocity"); sv && sv->is(Json::Type::Bool))
    s.slidesVelocity = sv->boolean;
  else
    s.slidesVelocity = !slidesChangeVelocity(s);
  // K-114: before clips looped one by one, a pattern's loop mode (or an automation
  // curve's loop length) looped every clip of it: each clip keeps the loop it played with.
  if (const Json* cl = j.find("clipLoops"); !cl || !cl->is(Json::Type::Bool)) adoptOldClipLoops(s);
  // grid2-1118: before version 10 a slot of the clip grid (a scene's stretch of the live area on a track)
  // could hold several clips, of which only the first launched: each further one moves to a scene of its
  // own, inserted under it (nothing is lost; the later scenes and the arrangement move down).
  if (srcVersion < 10) {
    trackmodes::splitSharedSlots(s);
  }
  out = std::move(s);
  return true;
}

bool loadProject(const std::string& path, Song& out, std::string* err) {
  std::ifstream is(path, std::ios::binary);
  if (!is) return fail(err, "cannot open " + path);
  std::stringstream ss;
  ss << is.rdbuf();
  return loadProjectText(ss.str(), path, out, err);
}

bool loadProjectText(const std::string& text, const std::string& path, Song& out, std::string* err) {
  Json j;
  size_t pos = 0;
  if (!jsonParse(text, j, &pos)) return fail(err, "JSON syntax error at byte " + std::to_string(pos));
  if (!songFromJson(j, out, err)) return false;
  // Portable projects (zipped / exported data files) refer to their files
  // relative to the project file.
  resolveRelativePaths(out, std::filesystem::path(path).parent_path().string());
  // K-435: files saved in Gridline's own folders (edited audio of a project not saved yet, unpacked
  // samples) that are no longer where the project says: found where those folders are now.
  relinkGridlineFiles(out, gridlineFileRoots());
  // Checkpoint 58 (N57-01): a kept send whose track a layout without it (or an older version) routed
  // elsewhere goes back into the track's sends. Nothing changes when no entry is stale.
  trackmodes::restoreKeptSends(out);
  return true;
}

std::string projectToText(const Song& song) { return jsonStringify(songToJson(song)) + "\n"; }

std::string fileWriteError(const std::string& path, int errnum) {
  const std::filesystem::path p(path);
  // The folder by its name only: the message must fit a notification card
  // (the full path travels as the notification's details).
  const std::string file = p.filename().string(), folder = p.parent_path().filename().string();
  const std::string where = folder.empty() ? std::string("this folder") : "the folder \"" + folder + "\"";
  const std::string head = "cannot write " + file + ": ";
  // Windows reports an over-long name as an invalid argument (or a missing file).
  if (file.size() > 255 && errnum != ENAMETOOLONG) errnum = ENAMETOOLONG;
  switch (errnum) {
    case EACCES:
    case EPERM:
    case EROFS: return head + "no permission to write in " + where + " (read-only folder or disk). Choose another folder.";
    case ENOENT:
    case ENOTDIR: return head + where + " does not exist. Choose an existing folder.";
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
      return head + "the disk is full. Free some space or choose another disk.";
    case ENAMETOOLONG: return head + "the file name or path is too long. Choose a shorter name or folder.";
    case EISDIR: return head + "a folder has that name. Choose another name.";
    default: break;
  }
  return head + (errnum ? std::string(std::strerror(errnum)) : std::string("write failed")) + ".";
}

bool checkWritableTarget(const std::string& path, std::string* err) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) return fail(err, fileWriteError(path, EISDIR));
  if (std::filesystem::exists(path, ec) && !platform::writable(path) && (errno == EACCES || errno == EPERM || errno == EROFS))
    return fail(err, "cannot replace " + std::filesystem::path(path).filename().string() +
                         ": the file is read-only. Choose another name, or make the file writable.");
  return true;
}

bool writeFileAtomic(const std::string& path, const std::string& text, std::string* err) {
  // J-78: a read-only file is not replaced behind its back (the rename would succeed).
  if (!checkWritableTarget(path, err)) return false;
  // A temp name per call: two writers of one file (e.g. the browser indexer's
  // worker and the UI thread, or two running instances sharing the config
  // folder's backups) must not write into, or rename away, each other's temp.
  static std::atomic<unsigned> serial{0};
  std::string tmp = path + ".tmp." + std::to_string(static_cast<long>(platform::processId())) + "." + std::to_string(serial.fetch_add(1));
  errno = 0;
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return fail(err, fileWriteError(path, errno));
  errno = 0;
  const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  const int werr = errno;
  const bool closed = std::fclose(f) == 0;
  const int cerr = errno;
  if (!wrote || !closed) {
    std::remove(tmp.c_str());  // J-78: no half-written temp left behind (e.g. disk full)
    return fail(err, fileWriteError(path, !wrote ? werr : cerr));
  }
  if (!platform::replaceFile(tmp, path)) {
    const int e = errno;
    std::remove(tmp.c_str());
    return fail(err, fileWriteError(path, e));
  }
  return true;
}

bool saveProject(const std::string& path, const Song& song, std::string* err) {
  return writeFileAtomic(path, projectToText(song), err);
}

}  // namespace gl

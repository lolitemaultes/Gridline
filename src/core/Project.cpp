#include "Project.h"
#include "ProjectInterop.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include "Json.h"
#include "Params.h"

namespace gl {

namespace {

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
      int idx = static_cast<int>(c.num("pattern", -1));
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
  ObjectId next = static_cast<ObjectId>(j.num("nextId", kFirstObjectId));
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
      int g = static_cast<int>(c.num("generator", 0));
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
  return j;
}

// ---- Channel note settings, layers, display groups and per-note properties.
// Every field is written only when it differs from its default, so projects
// that do not use them are unchanged and older readers ignore the extra keys
// (no schema bump needed).
void channelExtrasToJson(const Channel& c, Json& jc) {
  const Channel d;
  if (c.fine != d.fine) jc.object["fine"] = num(c.fine);
  if (c.swingMix != d.swingMix) jc.object["swingMix"] = num(c.swingMix);
  if (c.timeShift != d.timeShift) jc.object["timeShift"] = num(c.timeShift);
  if (c.polyphony != d.polyphony) jc.object["polyphony"] = num(c.polyphony);
  if (c.mono) jc.object["mono"] = boolean(true);
  if (c.porta) jc.object["porta"] = boolean(true);
  if (c.portaTime != d.portaTime) jc.object["portaTime"] = num(c.portaTime);
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
    jc.object["echo"] = std::move(e);
  }
  if (!c.layer.empty()) {
    Json l = Json::makeArray();
    for (ObjectId k : c.layer) l.array.push_back(num(k));
    jc.object["layer"] = std::move(l);
    if (c.layerMode) jc.object["layerMode"] = num(c.layerMode);
  }
  if (c.group >= 0) jc.object["group"] = num(c.group);
}

void channelExtrasFromJson(const Json& jc, Channel& c) {
  c.fine = static_cast<int>(std::clamp(jc.num("fine", 0), -200.0, 200.0));
  c.swingMix = static_cast<float>(std::clamp(jc.num("swingMix", 1), 0.0, 1.0));
  c.timeShift = static_cast<float>(std::clamp(jc.num("timeShift", 0), 0.0, 1.0));
  c.polyphony = static_cast<int>(std::clamp(jc.num("polyphony", 0), 0.0, 256.0));
  c.mono = flag(jc, "mono");
  c.porta = flag(jc, "porta");
  c.portaTime = static_cast<float>(std::clamp(jc.num("portaTime", 0.1), 0.0, 2.0));
  c.cutGroup = static_cast<int>(std::clamp(jc.num("cutGroup", 0), 0.0, static_cast<double>(kMaxCutGroups)));
  c.cutBy = static_cast<int>(std::clamp(jc.num("cutBy", 0), 0.0, static_cast<double>(kMaxCutGroups)));
  if (const Json* e = jc.find("echo"); e && e->is(Json::Type::Object)) {
    c.echo.count = static_cast<int>(std::clamp(e->num("count", 0), 0.0, 10.0));
    c.echo.feedback = static_cast<float>(std::clamp(e->num("feedback", 0.5), 0.0, 1.0));
    c.echo.time = static_cast<float>(std::clamp(e->num("time", 0.375), 0.01, 16.0));
    c.echo.pan = static_cast<float>(std::clamp(e->num("pan", 0), -1.0, 1.0));
    c.echo.pitch = static_cast<int>(std::clamp(e->num("pitch", 0), -1200.0, 1200.0));
    c.echo.pingPong = flag(*e, "pingPong");
  }
  if (const Json* l = jc.find("layer"); l && l->is(Json::Type::Array))
    for (const auto& v : l->array)
      if (v.is(Json::Type::Number) && v.number >= kFirstObjectId && c.layer.size() < static_cast<size_t>(kMaxLayerChildren))
        c.layer.push_back(static_cast<ObjectId>(v.number));
  c.layerMode = static_cast<int>(std::clamp(jc.num("layerMode", 0), 0.0, 1.0));
  c.group = static_cast<int>(std::clamp(jc.num("group", -1), -1.0, 1e6));
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
  if (n.muted) jn.object["muted"] = boolean(true);
}

void noteFromJson(const Json& e, Note& n) {
  n.pan = static_cast<int>(std::clamp(e.num("pan", 0), -static_cast<double>(kNotePanMax), static_cast<double>(kNotePanMax)));
  n.fine = static_cast<int>(std::clamp(e.num("fine", 0), -static_cast<double>(kNoteFineMax), static_cast<double>(kNoteFineMax)));
  n.release = static_cast<int>(std::clamp(e.num("release", kNoteReleaseDefault), 0.0, 127.0));
  n.modX = static_cast<int>(std::clamp(e.num("modX", kNoteModCentre), 0.0, static_cast<double>(kNoteModMax)));
  n.modY = static_cast<int>(std::clamp(e.num("modY", kNoteModCentre), 0.0, static_cast<double>(kNoteModMax)));
  n.color = static_cast<int>(std::clamp(e.num("color", 0), 0.0, static_cast<double>(kNoteColors - 1)));
  n.slide = flag(e, "slide");
  n.porta = flag(e, "porta");
  n.muted = flag(e, "muted");
}
// ---- end of channel / note extras

PluginState pluginFromJson(const Json* j) {
  PluginState p;
  if (!j || !j->is(Json::Type::Object)) return p;
  p.type = j->text("type");
  p.data = j->text("data");
  if (const Json* a = j->find("params"); a && a->is(Json::Type::Array))
    for (const auto& v : a->array) p.params.push_back(static_cast<float>(v.is(Json::Type::Number) ? v.number : 0));
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
  if (c.muted) jc.object["muted"] = boolean(true);
  if (c.audioStart != 0) jc.object["audioStart"] = num(c.audioStart);
  if (c.audioEnd != 0) jc.object["audioEnd"] = num(c.audioEnd);
  if (c.fadeIn != 0) jc.object["fadeIn"] = num(c.fadeIn);
  if (c.fadeOut != 0) jc.object["fadeOut"] = num(c.fadeOut);
  if (c.gain != 1.0f) jc.object["gain"] = num(c.gain);
  if (c.pitch != 0.0f) jc.object["pitch"] = num(c.pitch);
  if (c.stretch != 1.0) jc.object["stretch"] = num(c.stretch);
  if (c.syncBpm != 0) jc.object["syncBpm"] = num(c.syncBpm);
  if (c.reverse) jc.object["reverse"] = boolean(true);
  if (c.takeGroup != kNoId) jc.object["takeGroup"] = num(c.takeGroup);
  if (c.take != 0) jc.object["take"] = num(c.take);
}

void clipArrangementFromJson(const Json& jc, Clip& c) {
  std::string k = jc.text("kind", "pattern");
  c.kind = k == "audio" ? ClipKind::Audio : k == "automation" ? ClipKind::Automation : ClipKind::Pattern;
  c.source = static_cast<ObjectId>(std::max(0.0, jc.num("source", 0)));
  c.offset = std::max<Tick>(0, static_cast<Tick>(jc.num("offset", 0)));
  c.muted = flag(jc, "muted");
  c.audioStart = std::max(0.0, jc.num("audioStart", 0));
  c.audioEnd = std::max(0.0, jc.num("audioEnd", 0));
  c.fadeIn = std::clamp(jc.num("fadeIn", 0), 0.0, 3600.0);
  c.fadeOut = std::clamp(jc.num("fadeOut", 0), 0.0, 3600.0);
  c.gain = static_cast<float>(std::clamp(jc.num("gain", 1), 0.0, 16.0));
  c.pitch = static_cast<float>(std::clamp(jc.num("pitch", 0), -4800.0, 4800.0));
  c.stretch = std::clamp(jc.num("stretch", 1), 0.0625, 16.0);
  c.syncBpm = std::clamp(jc.num("syncBpm", 0), 0.0, 1000.0);
  c.reverse = flag(jc, "reverse");
  c.takeGroup = static_cast<ObjectId>(std::max(0.0, jc.num("takeGroup", 0)));
  c.take = static_cast<int>(std::clamp(jc.num("take", 0), 0.0, 9999.0));
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
      Json pts = Json::makeArray();
      for (const auto& p : a.points) {
        Json jp = Json::makeArray();  // [pos, value, tension, shape]
        jp.array.push_back(num(static_cast<double>(p.pos)));
        jp.array.push_back(num(p.value));
        jp.array.push_back(num(p.tension));
        jp.array.push_back(num(static_cast<int>(p.shape)));
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
      a.name = ja.text("name", "Automation");
      a.color = color(ja, a.color);
      a.owner = static_cast<ObjectId>(std::max(0.0, ja.num("owner", 0)));
      a.index = static_cast<uint32_t>(std::max(0.0, ja.num("index", 0)));
      a.min = static_cast<float>(std::clamp(ja.num("min", 0), 0.0, 1.0));
      a.max = static_cast<float>(std::clamp(ja.num("max", 1), 0.0, 1.0));
      a.loop = std::max<Tick>(0, static_cast<Tick>(ja.num("loop", 0)));
      if (const Json* pts = ja.find("points"); pts && pts->is(Json::Type::Array))
        for (const auto& jp : pts->array) {
          if (!jp.is(Json::Type::Array) || jp.array.size() < 2) continue;
          auto at = [&](size_t i, double d) { return i < jp.array.size() && jp.array[i].is(Json::Type::Number) ? jp.array[i].number : d; };
          AutoPoint p;
          p.pos = std::max<Tick>(0, static_cast<Tick>(at(0, 0)));
          p.value = static_cast<float>(std::clamp(at(1, 0.5), 0.0, 1.0));
          p.tension = static_cast<float>(std::clamp(at(2, 0), -1.0, 1.0));
          p.shape = static_cast<AutoShape>(static_cast<int>(std::clamp(at(3, 0), 0.0, static_cast<double>(static_cast<int>(AutoShape::Count) - 1))));
          a.points.push_back(p);
        }
      std::stable_sort(a.points.begin(), a.points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
      s.automations.push_back(std::move(a));
    }
  if (const Json* arr = j.find("markers"); arr && arr->is(Json::Type::Array))
    for (const auto& jm : arr->array) {
      Marker m;
      m.pos = std::max<Tick>(0, static_cast<Tick>(jm.num("pos", 0)));
      m.name = jm.text("name");
      m.timeSigNum = static_cast<int>(std::clamp(jm.num("timeSigNum", 0), 0.0, 32.0));
      m.timeSigDen = static_cast<int>(std::clamp(jm.num("timeSigDen", 0), 0.0, 32.0));
      if (m.timeSigNum == 0 || m.timeSigDen == 0) m.timeSigNum = m.timeSigDen = 0;
      m.action = static_cast<MarkerAction>(static_cast<int>(std::clamp(jm.num("action", 0), 0.0, static_cast<double>(static_cast<int>(MarkerAction::Count) - 1))));
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
    Json jp = Json::makeArray();  // [pos, value, tension, shape]
    jp.array.push_back(num(static_cast<double>(p.pos)));
    jp.array.push_back(num(p.value));
    jp.array.push_back(num(p.tension));
    jp.array.push_back(num(static_cast<int>(p.shape)));
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
    p.pos = std::max<Tick>(0, static_cast<Tick>(at(0, 0)));
    p.value = static_cast<float>(std::clamp(at(1, 0.5), 0.0, 1.0));
    p.tension = static_cast<float>(std::clamp(at(2, 0), -1.0, 1.0));
    p.shape = static_cast<AutoShape>(static_cast<int>(std::clamp(at(3, 0), 0.0, static_cast<double>(static_cast<int>(AutoShape::Count) - 1))));
    out.push_back(p);
  }
  std::stable_sort(out.begin(), out.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
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
  const size_t total = static_cast<size_t>(p.bars) * static_cast<size_t>(p.stepsPerBar);
  for (const auto& [key, arr] : obj->object) {
    ObjectId ch = static_cast<ObjectId>(std::strtoul(key.c_str(), nullptr, 10));
    if (!s.channel(ch) || !arr.is(Json::Type::Array)) continue;
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
  if (p.automation.empty()) return;
  Json arr = Json::makeArray();
  for (const auto& lane : p.automation) {
    Json jl = Json::makeObject();
    jl.object["owner"] = num(lane.owner);
    jl.object["index"] = num(lane.index);
    jl.object["points"] = pointsToJson(lane.points);
    arr.array.push_back(std::move(jl));
  }
  jp.object["automation"] = std::move(arr);
}

// Lanes whose target no longer exists, or that repeat a target, are dropped.
void patternExtrasFromJson(const Json& jp, Pattern& p, const Song& s) {
  stepPropsFromJson(jp, p, s);
  const Json* arr = jp.find("automation");
  if (!arr || !arr->is(Json::Type::Array)) return;
  for (const auto& jl : arr->array) {
    PatternAutomation lane;
    lane.owner = static_cast<ObjectId>(std::max(0.0, jl.num("owner", 0)));
    lane.index = static_cast<uint32_t>(std::max(0.0, jl.num("index", 0)));
    lane.points = pointsFromJson(jl.find("points"));
    if (lane.points.empty() || !describeParam(s, {lane.owner, lane.index})) continue;
    bool dup = false;
    for (const auto& l : p.automation) dup = dup || (l.owner == lane.owner && l.index == lane.index);
    if (!dup) p.automation.push_back(std::move(lane));
  }
}

void trackExtrasToJson(const PlaylistTrack& t, Json& jt) {
  if (t.launch != LaunchGrid::FourBeats) jt.object["launch"] = num(static_cast<int>(t.launch));
  if (t.pad != PadMode::Trigger) jt.object["pad"] = num(static_cast<int>(t.pad));
  if (t.follow != FollowMode::Loop) jt.object["follow"] = num(static_cast<int>(t.follow));
  if (t.syncStart != LaunchGrid::Now) jt.object["syncStart"] = num(static_cast<int>(t.syncStart));
  if (t.groupSize > 0) jt.object["groupSize"] = num(t.groupSize);
  if (t.folded) jt.object["folded"] = boolean(true);
  if (t.locked) jt.object["locked"] = boolean(true);
}

void trackExtrasFromJson(const Json& jt, PlaylistTrack& t) {
  auto e = [&](const char* k, int dflt, int count) { return static_cast<int>(std::clamp(jt.num(k, dflt), 0.0, static_cast<double>(count - 1))); };
  t.launch = static_cast<LaunchGrid>(e("launch", static_cast<int>(LaunchGrid::FourBeats), static_cast<int>(LaunchGrid::Count)));
  t.pad = static_cast<PadMode>(e("pad", 0, static_cast<int>(PadMode::Count)));
  t.follow = static_cast<FollowMode>(e("follow", 0, static_cast<int>(FollowMode::Count)));
  t.syncStart = static_cast<LaunchGrid>(e("syncStart", 0, static_cast<int>(LaunchGrid::Count)));
  t.groupSize = static_cast<int>(std::clamp(jt.num("groupSize", 0), 0.0, static_cast<double>(kMaxPlaylistTracks)));
  t.folded = flag(jt, "folded");
  t.locked = flag(jt, "locked");
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
  if (s.live.areaEnd > 0) {
    Json jl = Json::makeObject();
    jl.object["areaEnd"] = num(static_cast<double>(s.live.areaEnd));
    j.object["live"] = std::move(jl);
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
        for (const auto& t : a.tracks) {
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
          marks.array.push_back(std::move(jm));
        }
        ja.object["markers"] = std::move(marks);
        if (a.live.areaEnd > 0) ja.object["liveAreaEnd"] = num(static_cast<double>(a.live.areaEnd));
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
  if (const Json* jl = j.find("live"); jl && jl->is(Json::Type::Object))
    s.live.areaEnd = std::max<Tick>(0, static_cast<Tick>(jl->num("areaEnd", 0)));
  const Json* arr = j.find("arrangements");
  if (!arr || !arr->is(Json::Type::Array) || arr->array.empty()) return true;
  const int cur = static_cast<int>(std::clamp(j.num("currentArrangement", 0), 0.0, static_cast<double>(arr->array.size() - 1)));
  for (size_t i = 0; i < arr->array.size(); ++i) {
    const Json& ja = arr->array[i];
    Arrangement a;
    a.name = ja.text("name", "Arrangement");
    if (static_cast<int>(i) != cur) {
      if (const Json* clips = ja.find("playlist"); clips && clips->is(Json::Type::Array))
        for (const auto& jc : clips->array) {
          Clip c;
          if (!takeId(jc, c.id)) return fail(err, "arrangement clip has missing or duplicate id");
          c.pattern = static_cast<ObjectId>(jc.num("pattern", 0));
          c.track = static_cast<int>(std::clamp(jc.num("track", 0), 0.0, static_cast<double>(kMaxPlaylistTracks - 1)));
          c.start = std::max<Tick>(0, static_cast<Tick>(jc.num("start", 0)));
          c.length = std::max<Tick>(0, static_cast<Tick>(jc.num("length", 0)));
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
          m.pos = std::max<Tick>(0, static_cast<Tick>(jm.num("pos", 0)));
          m.name = jm.text("name");
          m.timeSigNum = static_cast<int>(std::clamp(jm.num("timeSigNum", 0), 0.0, 32.0));
          m.timeSigDen = static_cast<int>(std::clamp(jm.num("timeSigDen", 0), 0.0, 32.0));
          if (m.timeSigNum == 0 || m.timeSigDen == 0) m.timeSigNum = m.timeSigDen = 0;
          m.action = static_cast<MarkerAction>(static_cast<int>(std::clamp(jm.num("action", 0), 0.0, static_cast<double>(static_cast<int>(MarkerAction::Count) - 1))));
          a.markers.push_back(std::move(m));
        }
      std::stable_sort(a.markers.begin(), a.markers.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
      a.live.areaEnd = std::max<Tick>(0, static_cast<Tick>(ja.num("liveAreaEnd", 0)));
    }
    s.arrangements.push_back(std::move(a));
  }
  s.currentArrangement = cur;
  return true;
}
// ---------------------------------------------------- end pattern automation, arrangements, live --

}  // namespace

Json mixerTrackToJson(const MixerTrack& m) {
  Json jm = Json::makeObject();
  jm.object["id"] = num(m.id);
  jm.object["name"] = str(m.name);
  jm.object["color"] = num(m.color);
  jm.object["volume"] = num(m.volume);
  jm.object["pan"] = num(m.pan);
  jm.object["stereoSep"] = num(m.stereoSep);
  jm.object["muted"] = boolean(m.muted);
  jm.object["solo"] = boolean(m.solo);
  jm.object["invertPolarity"] = boolean(m.invertPolarity);
  jm.object["swapLR"] = boolean(m.swapLR);
  if (m.input >= 0) jm.object["input"] = num(m.input);
  if (!m.inputStereo) jm.object["inputStereo"] = boolean(false);
  if (m.monitor != 0) jm.object["monitor"] = num(m.monitor);
  if (m.armed) jm.object["armed"] = boolean(true);
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
  if (m.icon) jm.object["icon"] = num(m.icon);
  if (m.disabled) jm.object["disabled"] = boolean(true);
  if (m.dock) jm.object["dock"] = num(m.dock);
  if (m.latencyOffset) jm.object["latencyOffset"] = num(m.latencyOffset);
  if (!m.threaded) jm.object["threaded"] = boolean(false);
  if (!m.group.empty()) jm.object["group"] = str(m.group);
  return jm;
}

Json songToJson(const Song& s) {
  Json j = Json::makeObject();
  j.object["format"] = str(kProjectFormatTag);
  j.object["version"] = num(kProjectSchemaVersion);
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
  j.object["masterPitch"] = num(s.masterPitch);
  j.object["nextId"] = num(s.nextId);

  Json chans = Json::makeArray();
  for (const auto& c : s.channels) {
    Json jc = Json::makeObject();
    jc.object["id"] = num(c.id);
    jc.object["name"] = str(c.name);
    jc.object["color"] = num(c.color);
    jc.object["plugin"] = pluginToJson(c.plugin);
    jc.object["gain"] = num(c.gain);
    jc.object["pan"] = num(c.pan);
    jc.object["muted"] = boolean(c.muted);
    jc.object["pitch"] = num(c.pitch);
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
    chans.array.push_back(std::move(jc));
  }
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
  for (const auto& t : s.tracks) {
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
    mods.array.push_back(std::move(jl));
  }
  j.object["modLinks"] = std::move(mods);
  if (!s.channelGroups.empty()) {  // channel rack display groups (written only when used)
    Json groups = Json::makeArray();
    for (const auto& g : s.channelGroups) groups.array.push_back(str(g));
    j.object["channelGroups"] = std::move(groups);
  }
  songExtrasToJson(s, j);  // live area, arrangements (see block above)
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
  int v = static_cast<int>(j.num("version", 1));
  if (v > kProjectSchemaVersion)
    return fail(err, "project was saved by a newer Gridline (schema " + std::to_string(v) + ")");
  if (v < 1) return fail(err, "bad schema version");
  if (v == 1) migrateV1(j), v = 2;
  if (v == 2) migrateV2(j), v = 3;
  if (v == 3) j.object["version"] = num(4), v = 4;  // v4 adds optional "links"
  if (v == 4) j.object["version"] = num(5), v = 5;  // v5 adds optional "modLinks", "noteFx", send "sidechain"
  if (v == 5) migrateV5(j), v = 6;
  return true;
}

void mixerTrackFromJson(const Json& jm, MixerTrack& m) {
  m.name = jm.text("name");
  m.color = color(jm, m.color);
  m.volume = static_cast<float>(std::clamp(jm.num("volume", 0.8), 0.0, 1.25));
  m.pan = static_cast<float>(std::clamp(jm.num("pan", 0), -1.0, 1.0));
  m.stereoSep = static_cast<float>(std::clamp(jm.num("stereoSep", 0), -1.0, 1.0));
  m.muted = flag(jm, "muted");
  m.solo = flag(jm, "solo");
  m.invertPolarity = flag(jm, "invertPolarity");
  m.swapLR = flag(jm, "swapLR");
  m.input = static_cast<int>(std::clamp(jm.num("input", -1), -1.0, static_cast<double>(kMaxInputChannels - 1)));
  if (const Json* st = jm.find("inputStereo"); st && st->is(Json::Type::Bool)) m.inputStereo = st->boolean;
  m.monitor = static_cast<uint8_t>(std::clamp(jm.num("monitor", 0), 0.0, static_cast<double>(static_cast<int>(InputMonitor::Count) - 1)));
  m.armed = flag(jm, "armed");
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
      m.sends.push_back({static_cast<ObjectId>(js.num("to", 0)), static_cast<float>(std::clamp(js.num("level", 1), 0.0, 1.25)),
                         flag(js, "sidechain"), flag(js, "preFader")});
  // --- strip EQ (0.5 FL import cycle; absent = defaults) ---
  if (const Json* eq = jm.find("eq"); eq && eq->is(Json::Type::Array))
    for (size_t b = 0; b < eq->array.size() && b < 3; ++b) {
      const Json& jb = eq->array[b];
      m.eqGain[b] = static_cast<float>(std::clamp(jb.num("gain", 0), -18.0, 18.0));
      m.eqFreq[b] = static_cast<float>(std::clamp(jb.num("freq", kStripEqDefFreq[b]), 20.0, 20000.0));
      m.eqQ[b] = static_cast<float>(std::clamp(jb.num("q", kStripEqDefQ), 0.1, 10.0));
    }
  m.icon = static_cast<int>(std::clamp(jm.num("icon", 0), 0.0, 255.0));
  m.disabled = flag(jm, "disabled");
  m.dock = static_cast<int>(std::clamp(jm.num("dock", 0), -1.0, 1.0));
  m.latencyOffset = static_cast<int>(std::clamp(jm.num("latencyOffset", 0), 0.0, static_cast<double>(kMaxManualLatency)));
  m.threaded = flag(jm, "threaded", true);
  m.group = jm.text("group");
}

bool songFromJson(const Json& src, Song& out, std::string* err) {
  Json j = src;
  if (!migrateProjectJson(j, err)) return false;
  Song s;
  s.mixer.clear();
  if (const Json* info = j.find("info"); info && info->is(Json::Type::Object)) {
    s.info.title = info->text("title", "Untitled");
    s.info.author = info->text("author");
    s.info.genre = info->text("genre");
    s.info.url = info->text("url");
    s.info.comments = info->text("comments");
    s.info.created = static_cast<int64_t>(info->num("created", 0));
    s.info.workTime = static_cast<int64_t>(info->num("workTime", 0));
  }
  s.ppq = static_cast<int>(j.num("ppq", kDefaultPpq));
  if (!isValidPpq(s.ppq)) return fail(err, "unsupported PPQ " + std::to_string(s.ppq));
  s.tempo = j.num("tempo", 140.0);
  auto tempoDesc = describeParam(s, songParam(SongParam::Tempo));
  if (s.tempo < tempoDesc->min || s.tempo > tempoDesc->max) return fail(err, "tempo out of range");
  s.timeSigNum = static_cast<int>(j.num("timeSigNum", 4));
  s.timeSigDen = static_cast<int>(j.num("timeSigDen", 4));
  if (s.timeSigNum < 1 || s.timeSigDen < 1) return fail(err, "bad time signature");
  s.swing = static_cast<float>(std::clamp(j.num("swing", 0), 0.0, 1.0));
  s.masterPitch = static_cast<float>(std::clamp(j.num("masterPitch", 0), -12.0, 12.0));

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
      c.name = jc.text("name", "Channel");
      c.color = color(jc, c.color);
      c.plugin = pluginFromJson(jc.find("plugin"));
      c.gain = static_cast<float>(std::clamp(jc.num("gain", 0.78), 0.0, 1.25));
      c.pan = static_cast<float>(std::clamp(jc.num("pan", 0.0), -1.0, 1.0));
      c.muted = flag(jc, "muted");
      c.pitch = static_cast<int>(std::clamp(jc.num("pitch", 0), -24.0, 24.0));
      c.mixerTrack = static_cast<ObjectId>(jc.num("mixerTrack", 0));
      if (!s.mixerTrack(c.mixerTrack)) c.mixerTrack = s.masterId();
      if (const Json* ao = jc.find("auxOuts"); ao && ao->is(Json::Type::Array))
        for (const auto& jt : ao->array) {
          ObjectId t = jt.is(Json::Type::Number) ? static_cast<ObjectId>(jt.number) : kNoId;
          c.auxOuts.push_back(t != kNoId && s.mixerTrack(t) ? t : kNoId);
          if (c.auxOuts.size() >= 32) break;
        }
      c.midiOutPort = static_cast<int>(std::clamp(jc.num("midiOutPort", -1.0), -1.0, 255.0));
      c.midiInPort = static_cast<int>(std::clamp(jc.num("midiInPort", -1.0), -1.0, 255.0));
      if (const Json* fx = jc.find("noteFx"); fx && fx->is(Json::Type::Array))
        for (const auto& jf : fx->array) {
          PluginState ps = pluginFromJson(&jf);
          const PluginInfo* pi = findPlugin(ps.type);
          if (pi && pi->kind == PluginKind::NoteFx && c.noteFx.size() < static_cast<size_t>(kMaxNoteFx))
            c.noteFx.push_back(std::move(ps));
        }
      channelExtrasFromJson(jc, c);
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
      p.name = jp.text("name", "Pattern");
      p.color = color(jp, p.color);
      p.stepsPerBar = static_cast<int>(jp.num("stepsPerBar", 16));
      p.bars = static_cast<int>(jp.num("bars", 1));
      if (p.stepsPerBar <= 0 || p.stepsPerBar > 192 || p.bars <= 0 || p.bars > 1024) return fail(err, "bad pattern dims");
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
          n.start = std::max<Tick>(0, static_cast<Tick>(e.num("start", 0)));
          n.length = std::max<Tick>(1, static_cast<Tick>(e.num("length", s.ppq)));
          n.pitch = static_cast<int>(std::clamp(e.num("pitch", 60), 0.0, 127.0));
          n.velocity = static_cast<int>(std::clamp(e.num("velocity", 100), 0.0, 127.0));
          noteFromJson(e, n);
          ns.push_back(n);
        }
        p.notes[ch] = std::move(ns);
      });
      patternExtrasFromJson(jp, p, s);  // pattern automation (see block above)
      s.patterns.push_back(std::move(p));
    }
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
      c.pattern = static_cast<ObjectId>(jc.num("pattern", 0));
      c.track = static_cast<int>(jc.num("track", 0));
      c.start = static_cast<Tick>(jc.num("start", 0));
      c.length = static_cast<Tick>(jc.num("length", 0));
      clipArrangementFromJson(jc, c);  // arrangement fields; audio / automation clips with a missing source are dropped
      if (c.kind == ClipKind::Audio && !s.channel(c.source)) continue;
      if (c.kind == ClipKind::Automation && !s.automation(c.source)) continue;
      if (c.kind == ClipKind::Pattern && !s.pattern(c.pattern)) return fail(err, "clip references missing pattern");
      if (c.track < 0 || c.track >= kMaxPlaylistTracks || c.start < 0 || c.length < 0) return fail(err, "bad clip position");
      if (static_cast<size_t>(c.track) >= s.tracks.size()) s.tracks.resize(static_cast<size_t>(c.track) + 1);
      s.playlist.push_back(c);
    }
  }
  if (const Json* links = j.find("links"); links && links->is(Json::Type::Array))
    for (const auto& jl : links->array) {
      ControllerLink l;
      l.cc = static_cast<int>(std::clamp(jl.num("cc", 1), 0.0, 127.0));
      l.midiChannel = static_cast<int>(std::clamp(jl.num("midiChannel", -1), -1.0, 15.0));
      l.owner = static_cast<ObjectId>(jl.num("owner", 0));
      l.index = static_cast<uint32_t>(jl.num("index", 0));
      l.min = static_cast<float>(std::clamp(jl.num("min", 0), 0.0, 1.0));
      l.max = static_cast<float>(std::clamp(jl.num("max", 1), 0.0, 1.0));
      auto en = [&jl](const char* key, int count) {
        return static_cast<int>(std::clamp(jl.num(key, 0), 0.0, static_cast<double>(count - 1)));
      };
      l.source = static_cast<RemoteSource>(en("source", static_cast<int>(RemoteSource::Count)));
      l.port = static_cast<int>(std::clamp(jl.num("port", -1), -1.0, 255.0));
      l.curve = static_cast<ModCurve>(en("curve", static_cast<int>(ModCurve::Count)));
      l.formula = jl.text("formula");
      l.invert = jl.num("invert", 0) != 0;
      l.takeover = static_cast<RemoteTakeover>(en("takeover", static_cast<int>(RemoteTakeover::Count)));
      l.encoder = static_cast<RemoteEncoder>(en("encoder", static_cast<int>(RemoteEncoder::Count)));
      l.sensitivity = static_cast<float>(std::clamp(jl.num("sensitivity", 1), 0.01, 64.0));
      l.button = static_cast<RemoteButton>(en("button", static_cast<int>(RemoteButton::Count)));
      l.action = static_cast<RemoteAction>(en("action", static_cast<int>(RemoteAction::Count)));
      // Drop links to deleted targets (action links have no target).
      if (l.action != RemoteAction::None || describeParam(s, {l.owner, l.index})) s.links.push_back(l);
    }
  if (const Json* mods = j.find("modLinks"); mods && mods->is(Json::Type::Array))
    for (const auto& jl : mods->array) {
      ModLink l;
      l.sourceOwner = static_cast<ObjectId>(jl.num("sourceOwner", 0));
      l.sourceSlot = static_cast<int>(std::clamp(jl.num("sourceSlot", -1), -1.0, static_cast<double>(kEffectSlots - 1)));
      l.sourceOutput = static_cast<int>(std::clamp(jl.num("sourceOutput", 0), 0.0, static_cast<double>(kMaxControllerOutputs - 1)));
      l.owner = static_cast<ObjectId>(jl.num("owner", 0));
      l.index = static_cast<uint32_t>(jl.num("index", 0));
      l.min = static_cast<float>(std::clamp(jl.num("min", 0), 0.0, 1.0));
      l.max = static_cast<float>(std::clamp(jl.num("max", 1), 0.0, 1.0));
      l.curve = static_cast<ModCurve>(static_cast<int>(std::clamp(jl.num("curve", 0), 0.0, static_cast<double>(static_cast<int>(ModCurve::Count) - 1))));
      l.formula = jl.text("formula");
      l.smoothMs = static_cast<float>(std::clamp(jl.num("smoothMs", 0), 0.0, 10000.0));
      l.enabled = flag(jl, "enabled", true);
      // Drop links whose source or target no longer exists, and duplicate targets.
      if (!describeParam(s, {l.owner, l.index}) || !isControllerSource(s, l.sourceOwner, l.sourceSlot)) continue;
      if (findModLink(s, l.owner, l.index)) continue;
      s.modLinks.push_back(std::move(l));
    }
  if (!songExtrasFromJson(j, s, takeId, err)) return false;  // live area, arrangements (see block above)
  s.nextId = std::max<ObjectId>(static_cast<ObjectId>(j.num("nextId", 0)), maxId + 1);
  out = std::move(s);
  return true;
}

bool loadProject(const std::string& path, Song& out, std::string* err) {
  std::ifstream is(path, std::ios::binary);
  if (!is) return fail(err, "cannot open " + path);
  std::stringstream ss;
  ss << is.rdbuf();
  Json j;
  size_t pos = 0;
  if (!jsonParse(ss.str(), j, &pos)) return fail(err, "JSON syntax error at byte " + std::to_string(pos));
  if (!songFromJson(j, out, err)) return false;
  // Portable projects (zipped / exported data files) refer to their files
  // relative to the project file.
  resolveRelativePaths(out, std::filesystem::path(path).parent_path().string());
  return true;
}

std::string projectToText(const Song& song) { return jsonStringify(songToJson(song)) + "\n"; }

bool writeFileAtomic(const std::string& path, const std::string& text, std::string* err) {
  std::string tmp = path + ".tmp";
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) return fail(err, "cannot write " + tmp);
    os << text;
    os.flush();
    if (!os) return fail(err, "write failed: " + tmp);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return fail(err, "cannot replace " + path);
  }
  return true;
}

bool saveProject(const std::string& path, const Song& song, std::string* err) {
  return writeFileAtomic(path, projectToText(song), err);
}

}  // namespace gl

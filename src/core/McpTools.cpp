// Gridline — MCP tools (see Mcp.h): inspect and edit the project through
// Document, run project scripts, render, analyse, export and save.
//
// Each tool validates its arguments against its schema (Session does that
// before run) and again against the song (indices, types). Editing tools run
// inside one Document group opened by Session::callTool, so a call is one
// undo step and a failed call leaves the project as it was.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include "Automation.h"
#include "Mcp.h"
#include "MidiFile.h"
#include "Params.h"
#include "PluginBrand.h"
#include "PluginInfo.h"
#include "Presets.h"
#include "Project.h"
#include "WavWriter.h"
#include "engine/Loudness.h"
#include "engine/Render.h"
#include "flp/Flp.h"
#include "flp/FlpExport.h"
#include "flp/FlpPackage.h"

namespace gl::mcp {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxNameLen = 256;
constexpr size_t kMaxChannels = 999, kMaxClips = 100000, kMaxAutomation = 10000, kMaxMarkers = 10000;

// ---------------------------------------------------------------- schema ----
Json prop(const char* type, const std::string& desc) { return jobj({{"type", jstr(type)}, {"description", jstr(desc)}}); }
Json intP(const std::string& desc, double lo, double hi) {
  Json j = prop("integer", desc);
  j.object["minimum"] = jnum(lo);
  j.object["maximum"] = jnum(hi);
  return j;
}
Json numP(const std::string& desc, double lo = NAN, double hi = NAN) {
  Json j = prop("number", desc);
  if (!std::isnan(lo)) j.object["minimum"] = jnum(lo);
  if (!std::isnan(hi)) j.object["maximum"] = jnum(hi);
  return j;
}
Json strP(const std::string& desc, size_t maxLen = kMaxNameLen) {
  Json j = prop("string", desc);
  j.object["maxLength"] = jnum(static_cast<double>(maxLen));
  return j;
}
Json boolP(const std::string& desc) { return prop("boolean", desc); }
Json enumP(const std::string& desc, std::vector<std::string> values) {
  Json j = prop("string", desc);
  Json e = Json::makeArray();
  for (auto& v : values) e.array.push_back(jstr(v));
  j.object["enum"] = e;
  return j;
}
Json intEnumP(const std::string& desc, std::vector<int> values) {
  Json j = prop("integer", desc);
  Json e = Json::makeArray();
  for (int v : values) e.array.push_back(jnum(v));
  j.object["enum"] = e;
  return j;
}
Json arrP(const std::string& desc, Json items, size_t maxItems) {
  Json j = prop("array", desc);
  j.object["items"] = std::move(items);
  j.object["maxItems"] = jnum(static_cast<double>(maxItems));
  return j;
}
Json objSchema(std::vector<std::pair<std::string, Json>> props, std::vector<std::string> required = {}) {
  Json s = jobj({{"type", jstr("object")}, {"additionalProperties", jbool(false)}});
  Json p = Json::makeObject();
  for (auto& [k, v] : props) p.object[k] = std::move(v);
  s.object["properties"] = std::move(p);
  if (!required.empty()) {
    Json r = Json::makeArray();
    for (auto& k : required) r.array.push_back(jstr(k));
    s.object["required"] = std::move(r);
  }
  return s;
}

// Common argument descriptions.
const char* kChannelD = "Channel number in the channel rack (1 = first)";
const char* kPatternD = "Pattern number (1 = first)";
const char* kMixerD = "Mixer track: 0 = master, 1.. = inserts";
const char* kSlotD = "Effect slot of the mixer track (1..10)";
const char* kParamD =
    "Parameter: a name (\"volume\", \"pan\", \"mute\", \"pitch\" for channels; \"volume\", \"pan\", \"stereo\", \"mute\", "
    "\"eqLowGain\" ... for mixer tracks; \"mix\", \"enabled\" for effect slots; \"tempo\", \"swing\", \"masterPitch\" for the song), "
    "a plugin parameter number (\"1\" = first, see get_parameters) or a plugin parameter's name";

// ------------------------------------------------------------- arguments ----
bool has(const Json& a, const char* k) { return a.find(k) != nullptr; }
long long argInt(const Json& a, const char* k, long long d = 0) {
  const Json* v = a.find(k);
  return v && v->is(Json::Type::Number) ? static_cast<long long>(std::llround(v->number)) : d;
}
double argNum(const Json& a, const char* k, double d = 0) { return a.num(k, d); }
std::string argStr(const Json& a, const char* k, const std::string& d = "") { return a.text(k, d); }
bool argBool(const Json& a, const char* k, bool d = false) {
  const Json* v = a.find(k);
  return v && v->is(Json::Type::Bool) ? v->boolean : d;
}

ToolResult fail(const std::string& msg) {
  ToolResult r;
  r.error = msg;
  return r;
}
ToolResult ok(Json data, std::string summary) {
  ToolResult r;
  r.data = std::move(data);
  r.summary = std::move(summary);
  return r;
}

// ------------------------------------------------------------ conversions ----
Tick toTicks(const Song& s, double beats) {
  const double t = beats * s.ppq;
  return static_cast<Tick>(std::llround(std::clamp(t, -1e15, 1e15)));
}
double toBeats(const Song& s, Tick t) { return static_cast<double>(t) / std::max(1, s.ppq); }
double round4(double v) { return std::round(v * 10000.0) / 10000.0; }
Json beatsJ(const Song& s, Tick t) { return jnum(round4(toBeats(s, t))); }

std::string hexColor(Rgb c) {
  char b[8];
  std::snprintf(b, sizeof b, "#%06X", static_cast<unsigned>(c & 0xFFFFFF));
  return b;
}
bool parseColor(std::string s, Rgb& out) {
  if (!s.empty() && s[0] == '#') s.erase(0, 1);
  if (s.size() != 6) return false;
  for (char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  out = static_cast<Rgb>(std::stoul(s, nullptr, 16));
  return true;
}
std::string key(std::string s) {  // case / separator-insensitive name
  std::string o;
  for (char c : s)
    if (c != '_' && c != ' ' && c != '-') o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}
std::string clipName(std::string s) {
  if (s.size() > kMaxNameLen) s.resize(kMaxNameLen);
  return s;
}
const char* kindName(PluginKind k) { return k == PluginKind::Generator ? "instrument" : k == PluginKind::Effect ? "effect" : "note_fx"; }
const char* clipKindName(ClipKind k) { return k == ClipKind::Audio ? "audio" : k == ClipKind::Automation ? "automation" : "pattern"; }
std::string pluginName(const std::string& type) {
  const PluginInfo* p = findPlugin(type);
  return p ? p->name : type;
}

// ---------------------------------------------------------------- lookups ----
const Channel* channelAt(const Song& s, long long n, std::string& err) {
  if (n < 1 || n > static_cast<long long>(s.channels.size())) {
    err = "no channel " + std::to_string(n) + " (the rack has " + std::to_string(s.channels.size()) + ")";
    return nullptr;
  }
  return &s.channels[static_cast<size_t>(n - 1)];
}
const Pattern* patternAt(const Song& s, long long n, std::string& err) {
  if (n < 1 || n > static_cast<long long>(s.patterns.size())) {
    err = "no pattern " + std::to_string(n) + " (the project has " + std::to_string(s.patterns.size()) + ")";
    return nullptr;
  }
  return &s.patterns[static_cast<size_t>(n - 1)];
}
const MixerTrack* mixerAt(const Song& s, long long n, std::string& err) {
  if (n < 0 || n >= static_cast<long long>(s.mixer.size())) {
    err = "no mixer track " + std::to_string(n) + " (0 = master .. " + std::to_string(static_cast<long long>(s.mixer.size()) - 1) + ")";
    return nullptr;
  }
  return &s.mixer[static_cast<size_t>(n)];
}
int channelNumber(const Song& s, ObjectId id) { return s.channelIndex(id) + 1; }
int patternNumber(const Song& s, ObjectId id) { return s.patternIndex(id) + 1; }

// The plugin type or a usable error for a kind.
const PluginInfo* pluginOfKind(const std::string& type, PluginKind kind, std::string& err) {
  const PluginInfo* p = findPlugin(type);
  if (!p) {
    err = "unknown plugin type \"" + type + "\" (see list_plugin_catalog)";
    return nullptr;
  }
  if (p->kind != kind) {
    auto article = [](PluginKind k) { return k == PluginKind::Generator ? "an instrument" : k == PluginKind::Effect ? "an effect" : "a note processor"; };
    err = "\"" + type + "\" is " + article(p->kind) + ", not " + article(kind);
    return nullptr;
  }
  return p;
}
// Factory preset by name, then user preset by name (case-insensitive).
bool presetByName(const Environment& env, const std::string& type, const std::string& name, PluginState& out, std::string& err) {
  const PluginInfo* p = findPlugin(type);
  if (p)
    for (size_t i = 0; i < p->presets.size(); ++i)
      if (key(p->presets[i].name) == key(name)) {
        out = presetState(*p, i);
        return true;
      }
  if (!env.presetRoot.empty())
    for (const auto& e : listPresets(env.presetRoot, type))
      if (key(e.name) == key(name) || key(e.folder + "/" + e.name) == key(name)) {
        if (loadPreset(e.file, type, out)) return true;
      }
  err = "no preset \"" + name + "\" for " + type + " (see list_presets)";
  return false;
}

// ------------------------------------------------------------- parameters ----
struct ParamTarget {
  enum Kind { Song, Channel, Mixer, Slot } kind = Song;
  ObjectId owner = kNoId;
  int slot = -1;
};
bool targetOf(const Song& s, const Json& a, ParamTarget& t, std::string& err) {
  if (has(a, "slot")) {
    if (!has(a, "mixer")) {
      err = "slot needs mixer";
      return false;
    }
    const MixerTrack* m = mixerAt(s, argInt(a, "mixer"), err);
    if (!m) return false;
    long long k = argInt(a, "slot");
    if (k < 1 || k > kEffectSlots || static_cast<size_t>(k) > m->slots.size() || m->slots[static_cast<size_t>(k - 1)].plugin.empty()) {
      err = "effect slot " + std::to_string(k) + " of mixer track " + std::to_string(argInt(a, "mixer")) + " is empty";
      return false;
    }
    t = {ParamTarget::Slot, m->id, static_cast<int>(k - 1)};
    return true;
  }
  if (has(a, "mixer")) {
    const MixerTrack* m = mixerAt(s, argInt(a, "mixer"), err);
    if (!m) return false;
    t = {ParamTarget::Mixer, m->id, -1};
    return true;
  }
  if (has(a, "channel")) {
    const Channel* c = channelAt(s, argInt(a, "channel"), err);
    if (!c) return false;
    t = {ParamTarget::Channel, c->id, -1};
    return true;
  }
  t = {ParamTarget::Song, kSongId, -1};
  return true;
}
const std::vector<std::pair<const char*, MixerParam>>& mixerParamNames() {
  static const std::vector<std::pair<const char*, MixerParam>> v = {
      {"volume", MixerParam::Volume},         {"pan", MixerParam::Pan},             {"stereo", MixerParam::StereoSep},
      {"mute", MixerParam::Mute},             {"eqLowGain", MixerParam::EqLowGain}, {"eqMidGain", MixerParam::EqMidGain},
      {"eqHighGain", MixerParam::EqHighGain}, {"eqLowFreq", MixerParam::EqLowFreq}, {"eqMidFreq", MixerParam::EqMidFreq},
      {"eqHighFreq", MixerParam::EqHighFreq}, {"eqLowQ", MixerParam::EqLowQ},       {"eqMidQ", MixerParam::EqMidQ},
      {"eqHighQ", MixerParam::EqHighQ}};
  return v;
}
// Every parameter of a target with its API name.
std::vector<std::pair<std::string, ParamId>> paramsOf(const Song& s, const ParamTarget& t) {
  std::vector<std::pair<std::string, ParamId>> v;
  switch (t.kind) {
    case ParamTarget::Song:
      v = {{"tempo", songParam(SongParam::Tempo)}, {"swing", songParam(SongParam::Swing)}, {"masterPitch", songParam(SongParam::MasterPitch)}};
      break;
    case ParamTarget::Channel: {
      v = {{"volume", channelParam(t.owner, ChannelParam::Volume)},
           {"pan", channelParam(t.owner, ChannelParam::Pan)},
           {"mute", channelParam(t.owner, ChannelParam::Mute)},
           {"pitch", channelParam(t.owner, ChannelParam::Pitch)}};
      const Channel* c = s.channel(t.owner);
      for (size_t k = 0; c && k < c->plugin.params.size(); ++k) v.push_back({std::to_string(k + 1), channelPluginParam(t.owner, static_cast<int>(k))});
      break;
    }
    case ParamTarget::Mixer:
      for (const auto& [n, p] : mixerParamNames()) v.push_back({n, mixerParam(t.owner, p)});
      break;
    case ParamTarget::Slot: {
      v = {{"mix", slotMixParam(t.owner, t.slot)}, {"enabled", slotEnableParam(t.owner, t.slot)}};
      const MixerTrack* m = s.mixerTrack(t.owner);
      const size_t n = m && static_cast<size_t>(t.slot) < m->slots.size() ? m->slots[static_cast<size_t>(t.slot)].plugin.params.size() : 0;
      for (size_t k = 0; k < n; ++k) v.push_back({std::to_string(k + 1), slotParam(t.owner, t.slot, static_cast<int>(k))});
      break;
    }
  }
  v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& e) { return !describeParam(s, e.second); }), v.end());
  return v;
}
bool resolveParam(const Song& s, const Json& a, ParamId& id, std::string& err) {
  ParamTarget t;
  if (!targetOf(s, a, t, err)) return false;
  const std::string want = argStr(a, "param");
  const auto all = paramsOf(s, t);
  for (const auto& [n, p] : all)
    if (key(n) == key(want)) {
      id = p;
      return true;
    }
  for (const auto& [n, p] : all)
    if (auto d = describeParam(s, p); d && key(d->name) == key(want)) {
      id = p;
      return true;
    }
  err = "no parameter \"" + want + "\" here (get_parameters lists them)";
  return false;
}
Json paramJson(const Song& s, const std::string& apiName, ParamId id) {
  auto d = describeParam(s, id);
  auto v = getParam(s, id);
  Json j = jobj({{"param", jstr(apiName)}});
  if (!d) return j;
  const double val = v ? *v : d->def;
  j.object["name"] = jstr(d->name);
  j.object["value"] = jnum(round4(val));
  j.object["display"] = jstr(d->format(val));
  j.object["min"] = jnum(d->min);
  j.object["max"] = jnum(d->max);
  j.object["default"] = jnum(d->def);
  if (!d->unit.empty()) j.object["unit"] = jstr(d->unit);
  j.object["kind"] = jstr(d->kind == ParamKind::Toggle ? "toggle" : d->kind == ParamKind::Choice ? "choice" : "continuous");
  if (d->kind == ParamKind::Choice) {
    Json c = Json::makeArray();
    for (const auto& x : d->choices) c.array.push_back(jstr(x));
    j.object["choices"] = c;
  }
  if (isModulated(s, id)) j.object["modulated"] = jbool(true);
  return j;
}

// ---------------------------------------------------------------- summary ----
Json channelJson(const Song& s, size_t i) {
  const Channel& c = s.channels[i];
  size_t notes = 0;
  for (const auto& p : s.patterns)
    if (auto it = p.notes.find(c.id); it != p.notes.end()) notes += it->second.size();
  Json j = jobj({{"channel", jnum(static_cast<double>(i + 1))},
                 {"name", jstr(c.name)},
                 {"plugin", jstr(c.plugin.type)},
                 {"pluginName", jstr(pluginName(c.plugin.type))},
                 {"volume", jnum(round4(c.gain))},
                 {"pan", jnum(round4(c.pan))},
                 {"muted", jbool(c.muted)},
                 {"pitch", jnum(c.pitch)},
                 {"mixer", jnum(std::max(0, s.mixerIndex(c.mixerTrack)))},
                 {"color", jstr(hexColor(c.color))},
                 {"notes", jnum(static_cast<double>(notes))}});
  if (const PluginInfo* p = findPlugin(c.plugin.type); p && p->usesSample && !c.plugin.data.empty())
    j.object["sample"] = jstr(fs::path(c.plugin.data.substr(0, c.plugin.data.find('\n'))).filename().string());  // (first line: the file)
  if (!c.noteFx.empty()) {
    Json fx = Json::makeArray();
    for (const auto& f : c.noteFx) fx.array.push_back(jstr(f.type));
    j.object["noteFx"] = fx;
  }
  return j;
}
Json patternJson(const Song& s, size_t i) {
  const Pattern& p = s.patterns[i];
  Json chans = Json::makeArray();
  for (size_t c = 0; c < s.channels.size(); ++c) {
    const ObjectId id = s.channels[c].id;
    size_t notes = 0, steps = 0;
    if (auto it = p.notes.find(id); it != p.notes.end()) notes = it->second.size();
    if (auto it = p.steps.find(id); it != p.steps.end()) steps = static_cast<size_t>(std::count_if(it->second.begin(), it->second.end(), [](uint8_t v) { return v > 0; }));
    if (notes || steps)
      chans.array.push_back(jobj({{"channel", jnum(static_cast<double>(c + 1))}, {"notes", jnum(static_cast<double>(notes))}, {"steps", jnum(static_cast<double>(steps))}}));
  }
  return jobj({{"pattern", jnum(static_cast<double>(i + 1))},
               {"name", jstr(p.name)},
               {"bars", jnum(p.bars)},
               {"stepsPerBar", jnum(p.stepsPerBar)},
               {"lengthBeats", beatsJ(s, s.patternLength(p))},
               {"color", jstr(hexColor(p.color))},
               {"channels", chans}});
}
bool mixerUsed(const Song& s, size_t i) {
  if (i == 0) return true;
  const MixerTrack& m = s.mixer[i];
  if (!m.name.empty()) return true;
  for (const auto& sl : m.slots)
    if (!sl.plugin.empty()) return true;
  for (const auto& c : s.channels)
    if (c.mixerTrack == m.id) return true;
  for (const auto& o : s.mixer)
    for (const auto& snd : o.sends)
      if (snd.to == m.id && o.id != m.id) return true;
  return false;
}
Json mixerJson(const Song& s, size_t i) {
  const MixerTrack& m = s.mixer[i];
  Json slots = Json::makeArray();
  for (size_t k = 0; k < m.slots.size(); ++k) {
    const EffectSlot& sl = m.slots[k];
    if (sl.plugin.empty()) continue;
    slots.array.push_back(jobj({{"slot", jnum(static_cast<double>(k + 1))},
                                {"plugin", jstr(sl.plugin.type)},
                                {"pluginName", jstr(pluginName(sl.plugin.type))},
                                {"enabled", jbool(sl.enabled)},
                                {"mix", jnum(round4(sl.mix))}}));
  }
  Json sends = Json::makeArray();
  for (const auto& snd : m.sends)
    sends.array.push_back(jobj({{"to", jnum(s.mixerIndex(snd.to))}, {"level", jnum(round4(snd.level))}, {"sidechain", jbool(snd.sidechain)}}));
  Json chans = Json::makeArray();
  for (size_t c = 0; c < s.channels.size(); ++c)
    if (s.channels[c].mixerTrack == m.id || (i == 0 && s.channels[c].mixerTrack == kNoId)) chans.array.push_back(jnum(static_cast<double>(c + 1)));
  return jobj({{"mixer", jnum(static_cast<double>(i))},
               {"name", jstr(mixerTrackName(s, i))},
               {"volume", jnum(round4(m.volume))},
               {"pan", jnum(round4(m.pan))},
               {"muted", jbool(m.muted)},
               {"solo", jbool(m.solo)},
               {"effects", slots},
               {"sends", sends},
               {"channels", chans}});
}
Json markerJson(const Song& s, size_t i) {
  const Marker& m = s.markers[i];
  Json j = jobj({{"marker", jnum(static_cast<double>(i + 1))}, {"position", beatsJ(s, m.pos)}, {"name", jstr(m.name)}});
  if (m.timeSigNum > 0) j.object["timeSignature"] = jstr(std::to_string(m.timeSigNum) + "/" + std::to_string(m.timeSigDen));
  static const char* actions[] = {"none", "loop", "skip", "pause", "stop", "song loop"};
  if (m.action != MarkerAction::None && static_cast<size_t>(m.action) < 6) j.object["action"] = jstr(actions[static_cast<size_t>(m.action)]);
  return j;
}
double songSeconds(const Song& s) {
  TempoMap tm;
  tm.build(s);
  return tm.secondsAt(static_cast<double>(s.lengthTicks()));
}

}  // namespace

Json projectSummary(const Environment& env) {
  const Song& s = env.doc.song();
  Json j = jobj({{"title", jstr(s.info.title)},
                 {"author", jstr(s.info.author)},
                 {"genre", jstr(s.info.genre)},
                 {"tempo", jnum(round4(s.tempo))},
                 {"timeSignature", jstr(std::to_string(s.timeSigNum) + "/" + std::to_string(s.timeSigDen))},
                 {"ppq", jnum(s.ppq)},
                 {"swing", jnum(round4(s.swing))},
                 {"masterPitch", jnum(round4(s.masterPitch))},
                 {"beatsPerBar", jnum(round4(static_cast<double>(s.barTicks()) / s.ppq))},
                 {"lengthBeats", beatsJ(s, s.lengthTicks())},
                 {"lengthBars", jnum(round4(static_cast<double>(s.lengthTicks()) / std::max<Tick>(1, s.barTicks())))},
                 {"lengthSeconds", jnum(round4(songSeconds(s)))},
                 {"modified", jbool(env.doc.modified())},
                 {"projectFile", jstr(env.projectPath)},
                 {"editsAllowed", jbool(env.allowEdits)}});
  j.object["counts"] = jobj({{"channels", jnum(static_cast<double>(s.channels.size()))},
                             {"patterns", jnum(static_cast<double>(s.patterns.size()))},
                             {"clips", jnum(static_cast<double>(s.playlist.size()))},
                             {"mixerTracks", jnum(static_cast<double>(s.mixer.size()))},
                             {"automationClips", jnum(static_cast<double>(s.automations.size()))},
                             {"markers", jnum(static_cast<double>(s.markers.size()))}});
  Json chans = Json::makeArray(), pats = Json::makeArray(), mix = Json::makeArray(), marks = Json::makeArray();
  for (size_t i = 0; i < s.channels.size() && i < 200; ++i)
    chans.array.push_back(jobj({{"channel", jnum(static_cast<double>(i + 1))}, {"name", jstr(s.channels[i].name)}, {"plugin", jstr(s.channels[i].plugin.type)}}));
  for (size_t i = 0; i < s.patterns.size() && i < 200; ++i)
    pats.array.push_back(jobj({{"pattern", jnum(static_cast<double>(i + 1))}, {"name", jstr(s.patterns[i].name)}, {"bars", jnum(s.patterns[i].bars)}}));
  for (size_t i = 0; i < s.mixer.size() && mix.array.size() < 200; ++i)
    if (mixerUsed(s, i)) {
      Json fx = Json::makeArray();
      for (const auto& sl : s.mixer[i].slots)
        if (!sl.plugin.empty()) fx.array.push_back(jstr(pluginName(sl.plugin.type)));
      mix.array.push_back(jobj({{"mixer", jnum(static_cast<double>(i))}, {"name", jstr(mixerTrackName(s, i))}, {"effects", fx}}));
    }
  for (size_t i = 0; i < s.markers.size() && i < 200; ++i) marks.array.push_back(markerJson(s, i));
  j.object["channels"] = chans;
  j.object["patterns"] = pats;
  j.object["mixer"] = mix;
  j.object["markers"] = marks;
  return j;
}

Json pluginCatalogJson(const std::string& kind, const std::string& query, bool withParams) {
  Json list = Json::makeArray();
  auto add = [&](const PluginInfo& p) {
    if (!kind.empty() && kind != kindName(p.kind)) return;
    if (!query.empty()) {
      const std::string q = key(query);
      if (key(p.name).find(q) == std::string::npos && key(p.type).find(q) == std::string::npos && key(p.category).find(q) == std::string::npos &&
          key(pluginKeywords(p.type)).find(q) == std::string::npos)
        return;
    }
    Json j = jobj({{"type", jstr(p.type)},
                   {"name", jstr(p.name)},
                   {"category", jstr(p.category)},
                   {"kind", jstr(kindName(p.kind))},
                   {"parameters", jnum(static_cast<double>(p.params.size()))}});
    if (p.usesSample) j.object["usesSample"] = jbool(true);
    if (const std::string tl = pluginTagline(p.type); !tl.empty()) j.object["tagline"] = jstr(tl);
    if (const std::string kw = pluginKeywords(p.type); !kw.empty()) j.object["keywords"] = jstr(kw);
    Json pr = Json::makeArray();
    for (const auto& x : p.presets) pr.array.push_back(jstr(x.name));
    j.object["presets"] = pr;
    if (withParams) {
      Json ps = Json::makeArray();
      for (size_t k = 0; k < p.params.size(); ++k) {
        const ParamDesc& d = p.params[k];
        Json pj = jobj({{"param", jstr(std::to_string(k + 1))}, {"name", jstr(d.name)}, {"min", jnum(d.min)}, {"max", jnum(d.max)}, {"default", jnum(d.def)}});
        if (!d.unit.empty()) pj.object["unit"] = jstr(d.unit);
        if (d.kind == ParamKind::Choice) {
          Json c = Json::makeArray();
          for (const auto& x : d.choices) c.array.push_back(jstr(x));
          pj.object["choices"] = c;
        } else if (d.kind == ParamKind::Toggle) {
          pj.object["kind"] = jstr("toggle");
        }
        ps.array.push_back(std::move(pj));
      }
      j.object["params"] = ps;
    }
    if (isHostedType(p.type)) j.object["hosted"] = jbool(true);
    list.array.push_back(std::move(j));
  };
  for (const auto& p : pluginCatalog()) add(p);
  for (const PluginInfo* p : dynamicPlugins()) add(*p);
  return jobj({{"plugins", list}, {"count", jnum(static_cast<double>(list.array.size()))}});
}

namespace {

// ================================================================= tools ====

ToolResult tGetSong(Environment& env, const Json&) { return ok(projectSummary(env), "read the song"); }

ToolResult tListChannels(Environment& env, const Json&) {
  const Song& s = env.doc.song();
  Json list = Json::makeArray();
  for (size_t i = 0; i < s.channels.size(); ++i) list.array.push_back(channelJson(s, i));
  return ok(jobj({{"channels", list}}), std::to_string(s.channels.size()) + " channels");
}

ToolResult tListPatterns(Environment& env, const Json&) {
  const Song& s = env.doc.song();
  Json list = Json::makeArray();
  for (size_t i = 0; i < s.patterns.size(); ++i) list.array.push_back(patternJson(s, i));
  return ok(jobj({{"patterns", list}}), std::to_string(s.patterns.size()) + " patterns");
}

Json noteJson(const Song& s, const Note& n) {
  Json j = jobj({{"start", beatsJ(s, n.start)}, {"length", beatsJ(s, n.length)}, {"pitch", jnum(n.pitch)}, {"velocity", jnum(n.velocity)}});
  if (n.pan) j.object["pan"] = jnum(n.pan);
  if (n.fine) j.object["fine"] = jnum(n.fine);
  if (n.muted) j.object["muted"] = jbool(true);
  if (n.slide) j.object["slide"] = jbool(true);
  if (n.porta) j.object["porta"] = jbool(true);
  return j;
}

ToolResult tGetPattern(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  std::string err;
  const Pattern* p = patternAt(s, argInt(a, "pattern"), err);
  if (!p) return fail(err);
  ObjectId only = kNoId;
  if (has(a, "channel")) {
    const Channel* c = channelAt(s, argInt(a, "channel"), err);
    if (!c) return fail(err);
    only = c->id;
  }
  Json chans = Json::makeArray();
  size_t total = 0;
  bool truncated = false;
  for (size_t ci = 0; ci < s.channels.size(); ++ci) {
    const ObjectId id = s.channels[ci].id;
    if (only != kNoId && id != only) continue;
    Json cj = jobj({{"channel", jnum(static_cast<double>(ci + 1))}, {"name", jstr(s.channels[ci].name)}});
    bool any = false;
    if (auto it = p->notes.find(id); it != p->notes.end() && !it->second.empty()) {
      Json ns = Json::makeArray();
      for (const auto& n : it->second) {
        if (total >= 20000) {
          truncated = true;
          break;
        }
        ns.array.push_back(noteJson(s, n));
        ++total;
      }
      cj.object["notes"] = ns;
      any = true;
    }
    if (auto it = p->steps.find(id); it != p->steps.end() && std::any_of(it->second.begin(), it->second.end(), [](uint8_t v) { return v > 0; })) {
      Json st = Json::makeArray();
      for (uint8_t v : it->second) st.array.push_back(jnum(v));
      cj.object["steps"] = st;
      any = true;
    }
    if (any || only != kNoId) chans.array.push_back(std::move(cj));
  }
  Json j = patternJson(s, static_cast<size_t>(s.patternIndex(p->id)));
  j.object["channels"] = chans;
  j.object["stepLengthBeats"] = beatsJ(s, s.ticksPerStep(*p));
  if (truncated) j.object["truncated"] = jbool(true);
  return ok(j, "pattern " + std::to_string(argInt(a, "pattern")));
}

ToolResult tGetPlaylist(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  const Tick from = has(a, "from") ? toTicks(s, argNum(a, "from")) : std::numeric_limits<Tick>::min();
  const Tick to = has(a, "to") ? toTicks(s, argNum(a, "to")) : std::numeric_limits<Tick>::max();
  const long long onlyTrack = argInt(a, "track", 0);
  const size_t limit = static_cast<size_t>(argInt(a, "limit", 500));
  std::vector<const Clip*> v;
  for (const auto& c : s.playlist) {
    const Tick end = c.start + s.clipLength(c);
    if (end < from || c.start > to) continue;
    if (onlyTrack > 0 && c.track + 1 != onlyTrack) continue;
    v.push_back(&c);
  }
  std::stable_sort(v.begin(), v.end(), [](const Clip* x, const Clip* y) { return x->start != y->start ? x->start < y->start : x->track < y->track; });
  Json clips = Json::makeArray();
  std::vector<bool> used(s.tracks.size(), false);
  for (const Clip* c : v) {
    if (static_cast<size_t>(c->track) < used.size()) used[static_cast<size_t>(c->track)] = true;
    if (clips.array.size() >= limit) continue;
    Json j = jobj({{"clip", jnum(c->id)},
                   {"kind", jstr(clipKindName(c->kind))},
                   {"track", jnum(c->track + 1)},
                   {"start", beatsJ(s, c->start)},
                   {"length", beatsJ(s, s.clipLength(*c))}});
    if (c->muted) j.object["muted"] = jbool(true);
    if (c->kind == ClipKind::Pattern) {
      j.object["pattern"] = jnum(patternNumber(s, c->pattern));
      if (const Pattern* p = s.pattern(c->pattern)) j.object["name"] = jstr(p->name);
    } else if (c->kind == ClipKind::Audio) {
      j.object["channel"] = jnum(channelNumber(s, c->source));
    } else {
      for (size_t k = 0; k < s.automations.size(); ++k)
        if (s.automations[k].id == c->source) j.object["automation"] = jnum(static_cast<double>(k + 1)), j.object["name"] = jstr(s.automations[k].name);
    }
    clips.array.push_back(std::move(j));
  }
  Json tracks = Json::makeArray();
  for (size_t t = 0; t < s.tracks.size(); ++t)
    if (used[t] || !s.tracks[t].name.empty() || s.tracks[t].muted)
      tracks.array.push_back(jobj({{"track", jnum(static_cast<double>(t + 1))}, {"name", jstr(playlistTrackName(s, t))}, {"muted", jbool(s.tracks[t].muted)}}));
  Json j = jobj({{"tracks", tracks}, {"clips", clips}, {"totalClips", jnum(static_cast<double>(v.size()))}, {"trackCount", jnum(static_cast<double>(s.tracks.size()))}});
  if (v.size() > limit) j.object["truncated"] = jbool(true);
  return ok(j, std::to_string(v.size()) + " clips");
}

ToolResult tGetMixer(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  Json list = Json::makeArray();
  if (has(a, "mixer")) {
    std::string err;
    if (!mixerAt(s, argInt(a, "mixer"), err)) return fail(err);
    list.array.push_back(mixerJson(s, static_cast<size_t>(argInt(a, "mixer"))));
  } else {
    const bool all = argBool(a, "all");
    for (size_t i = 0; i < s.mixer.size(); ++i)
      if (all || mixerUsed(s, i)) list.array.push_back(mixerJson(s, i));
  }
  return ok(jobj({{"tracks", list}, {"trackCount", jnum(static_cast<double>(s.mixer.size()))}}), "mixer");
}

ToolResult tGetParameters(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  ParamTarget t;
  std::string err;
  if (!targetOf(s, a, t, err)) return fail(err);
  const std::string q = key(argStr(a, "query"));
  const size_t offset = static_cast<size_t>(argInt(a, "offset", 0)), limit = static_cast<size_t>(argInt(a, "limit", 200));
  Json list = Json::makeArray();
  size_t matched = 0;
  for (const auto& [n, id] : paramsOf(s, t)) {
    if (!q.empty()) {
      auto d = describeParam(s, id);
      if (key(n).find(q) == std::string::npos && (!d || key(d->name).find(q) == std::string::npos)) continue;
    }
    if (matched++ < offset || list.array.size() >= limit) continue;
    list.array.push_back(paramJson(s, n, id));
  }
  Json j = jobj({{"params", list}, {"total", jnum(static_cast<double>(matched))}});
  if (t.kind == ParamTarget::Channel) j.object["plugin"] = jstr(s.channel(t.owner)->plugin.type);
  if (t.kind == ParamTarget::Slot) j.object["plugin"] = jstr(s.mixerTrack(t.owner)->slots[static_cast<size_t>(t.slot)].plugin.type);
  return ok(j, std::to_string(matched) + " parameters");
}

ToolResult tGetAutomation(Environment& env, const Json& a) {
  const Song& s = env.doc.song();
  Json list = Json::makeArray();
  const long long only = argInt(a, "automation", 0);
  if (only > static_cast<long long>(s.automations.size())) return fail("no automation clip " + std::to_string(only));
  for (size_t i = 0; i < s.automations.size(); ++i) {
    if (only > 0 && static_cast<long long>(i + 1) != only) continue;
    const AutomationClip& ac = s.automations[i];
    const ParamId target{ac.owner, ac.index};
    Json pts = Json::makeArray();
    if (only > 0 || s.automations.size() <= 20)
      for (size_t k = 0; k < ac.points.size() && k < 5000; ++k)
        pts.array.push_back(jobj({{"position", beatsJ(s, ac.points[k].pos)}, {"value", jnum(round4(ac.points[k].value))}}));
    int placed = 0;
    for (const auto& c : s.playlist) placed += c.kind == ClipKind::Automation && c.source == ac.id;
    Json j = jobj({{"automation", jnum(static_cast<double>(i + 1))},
                   {"name", jstr(ac.name)},
                   {"target", jstr(paramDisplayName(s, target))},
                   {"pointCount", jnum(static_cast<double>(ac.points.size()))},
                   {"placements", jnum(placed)}});
    if (!pts.array.empty()) j.object["points"] = pts;
    list.array.push_back(std::move(j));
  }
  return ok(jobj({{"automation", list}}), std::to_string(list.array.size()) + " automation clips");
}

ToolResult tGetMarkers(Environment& env, const Json&) {
  const Song& s = env.doc.song();
  Json list = Json::makeArray();
  for (size_t i = 0; i < s.markers.size(); ++i) list.array.push_back(markerJson(s, i));
  return ok(jobj({{"markers", list}}), std::to_string(s.markers.size()) + " markers");
}

ToolResult tCatalog(Environment&, const Json& a) {
  Json j = pluginCatalogJson(argStr(a, "kind"), argStr(a, "query"), argBool(a, "includeParams"));
  return ok(j, "plugin catalogue");
}

ToolResult tPresets(Environment& env, const Json& a) {
  const std::string type = argStr(a, "type");
  const PluginInfo* p = findPlugin(type);
  if (!p) return fail("unknown plugin type \"" + type + "\" (see list_plugin_catalog)");
  Json factory = Json::makeArray(), user = Json::makeArray();
  for (const auto& x : p->presets) factory.array.push_back(jstr(x.name));
  if (!env.presetRoot.empty())
    for (const auto& e : listPresets(env.presetRoot, type)) {
      if (user.array.size() >= 2000) break;
      user.array.push_back(jstr(e.folder.empty() ? e.name : e.folder + "/" + e.name));
    }
  return ok(jobj({{"type", jstr(type)}, {"factory", factory}, {"user", user}}), "presets of " + type);
}

ToolResult tHistory(Environment& env, const Json&) {
  const UndoStack& h = env.doc.history();
  const auto labels = h.labels();
  Json steps = Json::makeArray();
  const size_t first = labels.size() > 30 ? labels.size() - 30 : 0;
  for (size_t i = first; i < labels.size(); ++i)
    steps.array.push_back(jobj({{"step", jnum(static_cast<double>(i + 1))}, {"label", jstr(labels[i])}, {"done", jbool(i < h.position())}}));
  return ok(jobj({{"steps", steps},
                  {"position", jnum(static_cast<double>(h.position()))},
                  {"undo", jstr(h.undoLabel())},
                  {"redo", jstr(h.redoLabel())},
                  {"modified", jbool(env.doc.modified())}}),
            "history");
}

// ------------------------------------------------------------------ edits ----

ToolResult tSetSong(Environment& env, const Json& a) {
  Document& d = env.doc;
  std::vector<std::string> done;
  if (has(a, "tempo")) {
    d.setParam(songParam(SongParam::Tempo), argNum(a, "tempo"), false);
    done.push_back("tempo");
  }
  if (has(a, "swing")) {
    d.setParam(songParam(SongParam::Swing), argNum(a, "swing"), false);
    done.push_back("swing");
  }
  if (has(a, "masterPitch")) {
    d.setParam(songParam(SongParam::MasterPitch), argNum(a, "masterPitch"), false);
    done.push_back("master pitch");
  }
  if (has(a, "numerator") || has(a, "denominator")) {
    const int num = static_cast<int>(argInt(a, "numerator", d.song().timeSigNum)), den = static_cast<int>(argInt(a, "denominator", d.song().timeSigDen));
    if (!d.setTimeSignature(num, den)) return fail("time signature must be 1..16 over 2, 4, 8 or 16");
    done.push_back("time signature");
  }
  if (has(a, "title") || has(a, "author") || has(a, "genre") || has(a, "comments")) {
    SongInfo info = d.song().info;
    info.title = clipName(argStr(a, "title", info.title));
    info.author = clipName(argStr(a, "author", info.author));
    info.genre = clipName(argStr(a, "genre", info.genre));
    info.comments = argStr(a, "comments", info.comments);
    d.setInfo(info);
    done.push_back("song info");
  }
  if (done.empty()) return fail("nothing to change: give tempo, numerator / denominator, swing, masterPitch, title, author, genre or comments");
  std::string sum;
  for (const auto& x : done) sum += (sum.empty() ? "" : ", ") + x;
  Json j = projectSummary(env);
  return ok(jobj({{"tempo", j.object["tempo"]}, {"timeSignature", j.object["timeSignature"]}, {"swing", j.object["swing"]}, {"title", j.object["title"]}}),
            "set " + sum);
}

bool applyChannelEdits(Environment& env, ObjectId id, const Json& a, std::string& err) {
  Document& d = env.doc;
  if (has(a, "plugin")) {
    const std::string type = argStr(a, "plugin");
    if (!pluginOfKind(type, PluginKind::Generator, err)) return false;
    if (d.song().channel(id)->plugin.type != type) d.setChannelPlugin(id, type);
  }
  if (has(a, "preset")) {
    PluginState st;
    if (!presetByName(env, d.song().channel(id)->plugin.type, argStr(a, "preset"), st, err)) return false;
    d.editChannel(id, "load preset", [&](Channel& c) {
      std::string keep = c.plugin.data;
      c.plugin = st;
      if (c.plugin.data.empty()) c.plugin.data = keep;
    });
  }
  if (has(a, "name")) d.renameChannel(id, clipName(argStr(a, "name")));
  if (has(a, "color")) {
    Rgb col;
    if (!parseColor(argStr(a, "color"), col)) {
      err = "color must be \"#RRGGBB\"";
      return false;
    }
    d.editChannel(id, "channel colour", [col](Channel& c) { c.color = col; });
  }
  if (has(a, "volume")) d.setParam(channelParam(id, ChannelParam::Volume), argNum(a, "volume"), false);
  if (has(a, "pan")) d.setParam(channelParam(id, ChannelParam::Pan), argNum(a, "pan"), false);
  if (has(a, "pitch")) d.setParam(channelParam(id, ChannelParam::Pitch), static_cast<double>(argInt(a, "pitch")), false);
  if (has(a, "muted")) d.setParam(channelParam(id, ChannelParam::Mute), argBool(a, "muted") ? 1.0 : 0.0, false);
  if (has(a, "mixer")) {
    const MixerTrack* m = mixerAt(d.song(), argInt(a, "mixer"), err);
    if (!m) return false;
    d.routeChannel(id, m->id);
  }
  return true;
}

ToolResult tAddChannel(Environment& env, const Json& a) {
  Document& d = env.doc;
  if (d.song().channels.size() >= kMaxChannels) return fail("too many channels");
  const std::string type = argStr(a, "plugin", "gl.synth");
  std::string err;
  const PluginInfo* p = pluginOfKind(type, PluginKind::Generator, err);
  if (!p) return fail(err);
  const std::string name = clipName(argStr(a, "name", p->name));
  int at = -1;
  if (has(a, "position")) {
    const long long pos = argInt(a, "position");
    if (pos < 1 || pos > static_cast<long long>(d.song().channels.size()) + 1) return fail("position out of range");
    at = static_cast<int>(pos - 1);
  }
  const ObjectId id = d.addChannel(name, type, at);
  if (id == kNoId) return fail("could not add the channel");
  Json rest = a;
  rest.object.erase("plugin");
  rest.object.erase("name");
  if (!applyChannelEdits(env, id, rest, err)) return fail(err);
  const int n = channelNumber(d.song(), id);
  return ok(channelJson(d.song(), static_cast<size_t>(n - 1)), "added channel " + std::to_string(n) + " \"" + name + "\" (" + type + ")");
}

ToolResult tEditChannel(Environment& env, const Json& a) {
  std::string err;
  const Channel* c = channelAt(env.doc.song(), argInt(a, "channel"), err);
  if (!c) return fail(err);
  const ObjectId id = c->id;
  if (!applyChannelEdits(env, id, a, err)) return fail(err);
  const int n = channelNumber(env.doc.song(), id);
  return ok(channelJson(env.doc.song(), static_cast<size_t>(n - 1)), "edited channel " + std::to_string(n));
}

ToolResult tRemoveChannel(Environment& env, const Json& a) {
  std::string err;
  const Channel* c = channelAt(env.doc.song(), argInt(a, "channel"), err);
  if (!c) return fail(err);
  const std::string name = c->name;
  if (!env.doc.removeChannel(c->id)) return fail("could not remove the channel");
  return ok(jobj({{"removed", jstr(name)}, {"channels", jnum(static_cast<double>(env.doc.song().channels.size()))}}), "removed channel \"" + name + "\"");
}

ToolResult tAddPattern(Environment& env, const Json& a) {
  Document& d = env.doc;
  if (d.song().patterns.size() >= static_cast<size_t>(kMaxPatterns)) return fail("too many patterns");
  const std::string name = clipName(argStr(a, "name", "Pattern " + std::to_string(d.song().patterns.size() + 1)));
  const ObjectId id = d.addPattern(name, static_cast<int>(argInt(a, "bars", 1)), static_cast<int>(argInt(a, "stepsPerBar", 16)));
  if (id == kNoId) return fail("could not add the pattern");
  const int n = patternNumber(d.song(), id);
  return ok(patternJson(d.song(), static_cast<size_t>(n - 1)), "added pattern " + std::to_string(n) + " \"" + name + "\"");
}

ToolResult tEditPattern(Environment& env, const Json& a) {
  Document& d = env.doc;
  std::string err;
  const Pattern* p = patternAt(d.song(), argInt(a, "pattern"), err);
  if (!p) return fail(err);
  const ObjectId id = p->id;
  if (has(a, "name")) d.renamePattern(id, clipName(argStr(a, "name")));
  if (has(a, "bars") || has(a, "stepsPerBar"))
    if (!d.setPatternLength(id, static_cast<int>(argInt(a, "bars", p->bars)), static_cast<int>(argInt(a, "stepsPerBar", p->stepsPerBar))))
      return fail("invalid pattern length");
  if (has(a, "color")) {
    Rgb col;
    if (!parseColor(argStr(a, "color"), col)) return fail("color must be \"#RRGGBB\"");
    d.editPattern(id, "pattern colour", [col](Pattern& x) { x.color = col; });
  }
  const int n = patternNumber(d.song(), id);
  return ok(patternJson(d.song(), static_cast<size_t>(n - 1)), "edited pattern " + std::to_string(n));
}

ToolResult tRemovePattern(Environment& env, const Json& a) {
  std::string err;
  const Pattern* p = patternAt(env.doc.song(), argInt(a, "pattern"), err);
  if (!p) return fail(err);
  const std::string name = p->name;
  if (!env.doc.removePattern(p->id)) return fail("could not remove the pattern");
  return ok(jobj({{"removed", jstr(name)}}), "removed pattern \"" + name + "\"");
}

ToolResult tSetNotes(Environment& env, const Json& a) {
  Document& d = env.doc;
  const Song& s = d.song();
  std::string err;
  const Pattern* p = patternAt(s, argInt(a, "pattern"), err);
  if (!p) return fail(err);
  const Channel* c = channelAt(s, argInt(a, "channel"), err);
  if (!c) return fail(err);
  const Json* in = a.find("notes");
  if (in->array.size() > env.maxNotesPerCall) return fail("too many notes in one call");
  std::vector<Note> notes;
  const std::string mode = argStr(a, "mode", "replace");
  if (mode == "add")
    if (auto it = p->notes.find(c->id); it != p->notes.end()) notes = it->second;
  for (const auto& n : in->array) {
    Note x;
    x.start = toTicks(s, argNum(n, "start"));
    x.length = std::max<Tick>(1, toTicks(s, argNum(n, "length", 1.0)));
    x.pitch = static_cast<int>(argInt(n, "pitch", 60));
    x.velocity = static_cast<int>(argInt(n, "velocity", 100));
    x.pan = static_cast<int>(argInt(n, "pan", 0));
    x.fine = static_cast<int>(argInt(n, "fine", 0));
    x.muted = argBool(n, "muted");
    x.slide = argBool(n, "slide");
    x.porta = argBool(n, "porta");
    notes.push_back(x);
  }
  const size_t count = notes.size();
  const ObjectId pid = p->id, cid = c->id;
  if (!d.setNotes(pid, cid, std::move(notes), "edit notes")) return fail("could not set the notes");
  return ok(jobj({{"pattern", jnum(patternNumber(d.song(), pid))}, {"channel", jnum(channelNumber(d.song(), cid))}, {"notes", jnum(static_cast<double>(count))}}),
            std::to_string(in->array.size()) + " notes (" + mode + ") in pattern " + std::to_string(argInt(a, "pattern")) + ", channel " +
                std::to_string(argInt(a, "channel")));
}

ToolResult tSetSteps(Environment& env, const Json& a) {
  Document& d = env.doc;
  const Song& s = d.song();
  std::string err;
  const Pattern* p = patternAt(s, argInt(a, "pattern"), err);
  if (!p) return fail(err);
  const Channel* c = channelAt(s, argInt(a, "channel"), err);
  if (!c) return fail(err);
  std::vector<uint8_t> row;
  for (const auto& v : a.find("steps")->array) row.push_back(static_cast<uint8_t>(std::clamp<long long>(std::llround(v.number), 0, 127)));
  const size_t n = static_cast<size_t>(p->bars) * static_cast<size_t>(p->stepsPerBar);
  if (row.size() > n) row.resize(n);
  const size_t on = static_cast<size_t>(std::count_if(row.begin(), row.end(), [](uint8_t v) { return v > 0; }));
  if (!d.setSteps(p->id, c->id, row)) return fail("could not set the steps");
  return ok(jobj({{"steps", jnum(static_cast<double>(row.size()))}, {"active", jnum(static_cast<double>(on))}, {"patternSteps", jnum(static_cast<double>(n))}}),
            std::to_string(on) + " steps in pattern " + std::to_string(argInt(a, "pattern")));
}

ToolResult tAddClip(Environment& env, const Json& a) {
  Document& d = env.doc;
  const Song& s = d.song();
  if (s.playlist.size() >= kMaxClips) return fail("too many clips");
  std::string err;
  const Pattern* p = patternAt(s, argInt(a, "pattern"), err);
  if (!p) return fail(err);
  const Tick len = has(a, "length") ? toTicks(s, argNum(a, "length")) : 0;
  const ObjectId id = d.addClip(p->id, static_cast<int>(argInt(a, "track") - 1), toTicks(s, argNum(a, "start")), len);
  if (id == kNoId) return fail("could not add the clip");
  return ok(jobj({{"clip", jnum(id)}}), "pattern clip on track " + std::to_string(argInt(a, "track")) + " at beat " + dump(jnum(argNum(a, "start"))));
}

ToolResult tEditClip(Environment& env, const Json& a) {
  Document& d = env.doc;
  const ObjectId id = static_cast<ObjectId>(argInt(a, "clip"));
  const Clip* c = d.song().clip(id);
  if (!c) return fail("no clip with id " + std::to_string(argInt(a, "clip")) + " (get_playlist lists them)");
  if (has(a, "track") || has(a, "start")) {
    const int track = has(a, "track") ? static_cast<int>(argInt(a, "track") - 1) : c->track;
    const Tick start = has(a, "start") ? toTicks(d.song(), argNum(a, "start")) : c->start;
    if (!d.moveClip(id, track, start)) return fail("could not move the clip");
  }
  if (has(a, "length") && !d.setClipLength(id, toTicks(d.song(), argNum(a, "length")))) return fail("could not change the length");
  if (has(a, "muted")) {
    const bool m = argBool(a, "muted");
    d.editClip(id, m ? "mute clip" : "unmute clip", [m](Clip& x) { x.muted = m; });
  }
  const Song& s = d.song();
  c = s.clip(id);
  return ok(jobj({{"clip", jnum(id)}, {"track", jnum(c->track + 1)}, {"start", beatsJ(s, c->start)}, {"length", beatsJ(s, s.clipLength(*c))}, {"muted", jbool(c->muted)}}),
            "edited clip " + std::to_string(id));
}

ToolResult tRemoveClip(Environment& env, const Json& a) {
  const ObjectId id = static_cast<ObjectId>(argInt(a, "clip"));
  if (!env.doc.song().clip(id)) return fail("no clip with id " + std::to_string(argInt(a, "clip")));
  if (!env.doc.removeClip(id)) return fail("could not remove the clip");
  return ok(jobj({{"removed", jnum(id)}}), "removed clip " + std::to_string(id));
}

ToolResult tAddAutomation(Environment& env, const Json& a) {
  Document& d = env.doc;
  if (d.song().automations.size() >= kMaxAutomation) return fail("too many automation clips");
  ParamId target;
  std::string err;
  if (!resolveParam(d.song(), a, target, err)) return fail(err);
  std::vector<AutoPoint> pts;
  for (const auto& p : a.find("points")->array) {
    AutoPoint x;
    x.pos = std::max<Tick>(0, toTicks(d.song(), argNum(p, "position")));
    x.value = static_cast<float>(std::clamp(argNum(p, "value"), 0.0, 1.0));
    pts.push_back(x);
  }
  std::stable_sort(pts.begin(), pts.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
  const std::string name = clipName(argStr(a, "name", paramDisplayName(d.song(), target)));
  ObjectId aid = kNoId;
  d.editSong("add automation clip", [&](Song& s) {
    AutomationClip ac;
    ac.id = aid = s.allocateId();
    ac.name = name;
    ac.owner = target.owner;
    ac.index = target.index;
    ac.points = pts;
    s.automations.push_back(ac);
  });
  Json j = jobj({{"automation", jnum(static_cast<double>(d.song().automations.size()))}, {"target", jstr(paramDisplayName(d.song(), target))}});
  if (has(a, "track")) {
    if (d.song().playlist.size() >= kMaxClips) return fail("too many clips");
    const int track = static_cast<int>(argInt(a, "track") - 1);
    const Tick start = toTicks(d.song(), argNum(a, "start", 0));
    const Tick len = has(a, "length") ? toTicks(d.song(), argNum(a, "length")) : 0;
    ObjectId cid = kNoId;
    d.editSong("add automation clip", [&](Song& s) {
      Clip c;
      c.id = cid = s.allocateId();
      c.kind = ClipKind::Automation;
      c.source = aid;
      c.track = track;
      c.start = start;
      c.length = len;
      s.playlist.push_back(c);
      if (static_cast<size_t>(track) >= s.tracks.size()) s.tracks.resize(static_cast<size_t>(track) + 1);
    });
    j.object["clip"] = jnum(cid);
  }
  return ok(j, "automation of " + paramDisplayName(d.song(), target) + " (" + std::to_string(pts.size()) + " points)");
}

ToolResult tAddPlugin(Environment& env, const Json& a) {
  Document& d = env.doc;
  std::string err;
  const MixerTrack* m = mixerAt(d.song(), argInt(a, "mixer"), err);
  if (!m) return fail(err);
  const std::string type = argStr(a, "plugin");
  if (!pluginOfKind(type, PluginKind::Effect, err)) return fail(err);
  int slot = -1;
  if (has(a, "slot")) {
    slot = static_cast<int>(argInt(a, "slot") - 1);
  } else {
    for (size_t k = 0; k < m->slots.size() && slot < 0; ++k)
      if (m->slots[k].plugin.empty()) slot = static_cast<int>(k);
    if (slot < 0) return fail("every effect slot of this mixer track is in use: give a slot to replace");
  }
  const ObjectId mid = m->id;
  if (!d.setSlotPlugin(mid, slot, type)) return fail("could not load the effect");
  if (has(a, "preset")) {
    PluginState st;
    if (!presetByName(env, type, argStr(a, "preset"), st, err)) return fail(err);
    d.editMixerTrack(mid, "load preset", [&](MixerTrack& t) { t.slots[static_cast<size_t>(slot)].plugin = st; });
  }
  return ok(jobj({{"mixer", jnum(argInt(a, "mixer"))}, {"slot", jnum(slot + 1)}, {"plugin", jstr(type)}, {"pluginName", jstr(pluginName(type))}}),
            pluginName(type) + " on mixer " + std::to_string(argInt(a, "mixer")) + " slot " + std::to_string(slot + 1));
}

ToolResult tRemovePlugin(Environment& env, const Json& a) {
  Document& d = env.doc;
  std::string err;
  const MixerTrack* m = mixerAt(d.song(), argInt(a, "mixer"), err);
  if (!m) return fail(err);
  const size_t k = static_cast<size_t>(argInt(a, "slot") - 1);
  if (k >= m->slots.size() || m->slots[k].plugin.empty()) return fail("that effect slot is empty");
  const std::string type = m->slots[k].plugin.type;
  if (!d.setSlotPlugin(m->id, static_cast<int>(k), "")) return fail("could not remove the effect");
  return ok(jobj({{"removed", jstr(type)}}), "removed " + pluginName(type) + " from mixer " + std::to_string(argInt(a, "mixer")));
}

ToolResult tSetParameter(Environment& env, const Json& a) {
  Document& d = env.doc;
  ParamId id;
  std::string err;
  if (!resolveParam(d.song(), a, id, err)) return fail(err);
  auto desc = describeParam(d.song(), id);
  double v = argNum(a, "value");
  if (argBool(a, "normalized")) v = desc->fromNormalized(std::clamp(v, 0.0, 1.0));
  if (!d.setParam(id, v, false)) return fail("could not set the parameter");
  Json j = paramJson(d.song(), argStr(a, "param"), id);
  return ok(j, paramDisplayName(d.song(), id) + " = " + j.text("display"));
}

ToolResult tEditMixerTrack(Environment& env, const Json& a) {
  Document& d = env.doc;
  std::string err;
  const MixerTrack* m = mixerAt(d.song(), argInt(a, "mixer"), err);
  if (!m) return fail(err);
  const ObjectId id = m->id;
  if (has(a, "name")) {
    const std::string n = clipName(argStr(a, "name"));
    d.editMixerTrack(id, "rename mixer track", [&n](MixerTrack& t) { t.name = n; });
  }
  if (has(a, "volume")) d.setParam(mixerParam(id, MixerParam::Volume), argNum(a, "volume"), false);
  if (has(a, "pan")) d.setParam(mixerParam(id, MixerParam::Pan), argNum(a, "pan"), false);
  if (has(a, "muted")) d.setParam(mixerParam(id, MixerParam::Mute), argBool(a, "muted") ? 1.0 : 0.0, false);
  if (has(a, "solo")) {
    const bool on = argBool(a, "solo");
    d.editMixerTrack(id, "solo", [on](MixerTrack& t) { t.solo = on; });
  }
  if (has(a, "routeTo")) {
    const MixerTrack* dest = mixerAt(d.song(), argInt(a, "routeTo"), err);
    if (!dest) return fail(err);
    if (dest->id == id) return fail("a track cannot route to itself");
    if (!canRoute(d.song(), id, dest->id)) return fail("that route would make a loop");
    const ObjectId to = dest->id;
    d.editMixerTrack(id, "route", [to](MixerTrack& t) {
      t.sends.clear();
      Send snd;
      snd.to = to;
      t.sends.push_back(snd);
    });
  }
  const int n = d.song().mixerIndex(id);
  return ok(mixerJson(d.song(), static_cast<size_t>(n)), "edited mixer track " + std::to_string(n));
}

ToolResult tAddMarker(Environment& env, const Json& a) {
  Document& d = env.doc;
  if (d.song().markers.size() >= kMaxMarkers) return fail("too many markers");
  Marker m;
  m.pos = std::max<Tick>(0, toTicks(d.song(), argNum(a, "position")));
  m.name = clipName(argStr(a, "name", "Marker"));
  if (has(a, "numerator") || has(a, "denominator")) {
    m.timeSigNum = static_cast<int>(argInt(a, "numerator", 4));
    m.timeSigDen = static_cast<int>(argInt(a, "denominator", 4));
  }
  d.editSong("add marker", [&m](Song& s) {
    s.markers.push_back(m);
    std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
  });
  int at = 0;
  for (size_t i = 0; i < d.song().markers.size(); ++i)
    if (d.song().markers[i] == m) at = static_cast<int>(i + 1);
  return ok(jobj({{"marker", jnum(at)}}), "marker \"" + m.name + "\" at beat " + dump(jnum(argNum(a, "position"))));
}

ToolResult tRemoveMarker(Environment& env, const Json& a) {
  Document& d = env.doc;
  const long long n = argInt(a, "marker");
  if (n < 1 || n > static_cast<long long>(d.song().markers.size())) return fail("no marker " + std::to_string(n));
  const std::string name = d.song().markers[static_cast<size_t>(n - 1)].name;
  d.editSong("delete marker", [n](Song& s) { s.markers.erase(s.markers.begin() + static_cast<long>(n - 1)); });
  return ok(jobj({{"removed", jstr(name)}}), "removed marker \"" + name + "\"");
}

ToolResult tRunScript(Environment& env, const Json& a) {
  if (!env.allowScripts) return fail("scripts are switched off in Gridline's settings");
  const std::string code = argStr(a, "code");
  if (code.size() > env.maxScriptBytes) return fail("script too long");
  script::ProjectScript ps;
  std::string err;
  if (!ps.loadSource(code, clipName(argStr(a, "name", "assistant script")), &err)) return fail("script error: " + err);
  script::ProjectRunResult r = ps.run(env.host, {}, env.scriptLimits, "", static_cast<uint32_t>(argInt(a, "seed", 1)));
  Json log = Json::makeArray();
  for (size_t i = 0; i < r.log.size() && i < 500; ++i) log.array.push_back(jstr(r.log[i]));
  if (!r.ok) {
    std::string msg = "script failed" + std::string(r.limitHit ? " (limit reached)" : "") + ": " + r.error + " (nothing was changed)";
    for (size_t i = 0; i < r.log.size() && i < 50; ++i) msg += "\n" + r.log[i];
    return fail(msg);
  }
  return ok(jobj({{"changed", jbool(r.changed)}, {"log", log}}), r.changed ? "script changed the project" : "script ran (no change)");
}

// ---------------------------------------------------------------- render ----
// J-61: renders are jobs. RenderPlan is what the calling thread prepares (a
// song snapshot, options); renderPlan runs on the worker with progress and
// cancel from the job.
struct RenderPlan {
  Song song;
  RenderOptions opt;
  double maxSec = 0;
};
bool planRender(Environment& env, const Json& a, RenderPlan& plan, std::string& err) {
  Song song = env.snapshot();
  RenderOptions& opt = plan.opt;
  opt.sampleRate = static_cast<int>(argInt(a, "sampleRate", 44100));
  opt.tail = song.ppq * 2;
  const double maxSec = std::min(env.maxRenderSeconds, argNum(a, "maxSeconds", env.maxRenderSeconds));
  if (argStr(a, "scope", "song") == "pattern") {
    std::string e;
    const Pattern* p = patternAt(song, argInt(a, "pattern", 1), e);
    if (!p) {
      err = e;
      return false;
    }
    opt.patternMode = true;
    opt.pattern = p->id;
  } else {
    if (song.playlist.empty()) {
      err = "the playlist is empty: render a pattern (scope \"pattern\") or add clips first";
      return false;
    }
    TempoMap tm;
    tm.build(song);
    const auto lim = static_cast<Tick>(tm.tickAtSeconds(maxSec));
    std::vector<Clip> kept;
    for (auto c : song.playlist)
      if (c.start < lim) {
        c.length = std::min<Tick>(song.clipLength(c), lim - c.start);
        kept.push_back(c);
      }
    song.playlist = std::move(kept);
  }
  plan.song = std::move(song);
  plan.maxSec = maxSec;
  return true;
}
bool renderPlan(RenderPlan& plan, Job& job, double share, AudioBuffer& buf, std::string& err) {
  const int sr = plan.opt.sampleRate;
  job.setStatus("Rendering");
  plan.opt.progress = [&job, share](double p) { job.progress.store(std::clamp(p, 0.0, 1.0) * share); };
  plan.opt.cancel = &job.cancel;
  buf = renderSong(plan.song, plan.opt);
  if (job.cancel.load()) {
    err = "cancelled in Gridline (nothing was written)";
    return false;
  }
  const size_t maxFrames = static_cast<size_t>((plan.maxSec + 10.0) * sr);
  if (buf.frames > maxFrames) {  // pattern mode ignores the cap above: cut here
    AudioBuffer cut(buf.channels, maxFrames);
    for (size_t c = 0; c < buf.channels; ++c) std::copy(buf.plane(c), buf.plane(c) + maxFrames, cut.plane(c));
    buf = std::move(cut);
  }
  if (buf.frames == 0) {
    err = "nothing to render";
    return false;
  }
  return true;
}
Json loudnessJson(const AudioBuffer& buf, int sr) {
  size_t clipped = 0, nonFinite = 0;
  double sum = 0;
  for (float v : buf.data) {
    if (!std::isfinite(v)) {
      ++nonFinite;
      continue;
    }
    sum += static_cast<double>(v) * v;
    clipped += std::fabs(v) > 1.0f;
  }
  const double rms = buf.data.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(buf.data.size()));
  const double lufs = integratedLoudness(buf, sr);
  Json j = jobj({{"lufs", jnum(std::round(lufs * 100) / 100)},
                 {"peakDb", jnum(std::round(samplePeakDb(buf) * 100) / 100)},
                 {"rmsDb", jnum(std::round(20.0 * std::log10(std::max(rms, 1e-9)) * 100) / 100)},
                 {"seconds", jnum(round4(static_cast<double>(buf.frames) / sr))},
                 {"sampleRate", jnum(sr)},
                 {"clippedSamples", jnum(static_cast<double>(clipped))},
                 {"silent", jbool(lufs <= kSilentLufs + 1)}});
  if (nonFinite) j.object["nonFiniteSamples"] = jnum(static_cast<double>(nonFinite));
  return j;
}

JobWork pAnalyze(Environment& env, const Json& a, std::string& err) {
  auto plan = std::make_shared<RenderPlan>();
  if (!planRender(env, a, *plan, err)) return nullptr;
  return [plan](Job& job) {
    AudioBuffer buf;
    std::string e;
    if (!renderPlan(*plan, job, 0.95, buf, e)) return fail(e);
    job.setStatus("Measuring loudness");
    Json j = loudnessJson(buf, plan->opt.sampleRate);
    return ok(j, "loudness " + dump(j.object["lufs"]) + " LUFS, peak " + dump(j.object["peakDb"]) + " dB");
  };
}

JobWork pRender(Environment& env, const Json& a, std::string& err) {
  std::string path;
  auto roots = env.writeRoots();
  if (!env.exportDir.empty() && roots.size() > 1) std::rotate(roots.begin(), roots.begin() + 1, roots.end());  // export folder first
  if (!resolveWritePath(argStr(a, "path"), roots, {".wav"}, path, err)) return nullptr;
  auto plan = std::make_shared<RenderPlan>();
  if (!planRender(env, a, *plan, err)) return nullptr;
  const int bits = static_cast<int>(argInt(a, "bits", 24));
  return [plan, path, bits](Job& job) {
    AudioBuffer buf;
    std::string e;
    if (!renderPlan(*plan, job, 0.9, buf, e)) return fail(e);
    const int sr = plan->opt.sampleRate;
    job.setStatus("Writing " + fs::path(path).filename().string());
    if (!writeWav(path, buf, sr, bits)) return fail("cannot write " + path);
    Json j = loudnessJson(buf, sr);
    j.object["path"] = jstr(path);
    j.object["bits"] = jnum(bits);
    ToolResult r = ok(j, "rendered " + fs::path(path).filename().string() + " (" + dump(j.object["lufs"]) + " LUFS)");
    r.changed = true;
    return r;
  };
}

JobWork pExport(Environment& env, const Json& a, std::string& err) {
  const std::string format = argStr(a, "format");
  const std::vector<std::string> exts = format == "midi" ? std::vector<std::string>{".mid", ".midi"} : format == "flp" ? std::vector<std::string>{".flp"} : std::vector<std::string>{".zip"};
  std::string path;
  auto roots = env.writeRoots();
  if (!env.exportDir.empty() && roots.size() > 1) std::rotate(roots.begin(), roots.begin() + 1, roots.end());
  if (!resolveWritePath(argStr(a, "path"), roots, exts, path, err)) return nullptr;
  auto song = std::make_shared<Song>(env.snapshot());
  ObjectId pattern = kNoId;
  if (format == "midi" && has(a, "pattern")) {
    const Pattern* p = patternAt(*song, argInt(a, "pattern"), err);
    if (!p) return nullptr;
    pattern = p->id;
  }
  const std::string factory = env.factoryData.empty() ? flp::defaultFactoryData() : env.factoryData;
  return [song, path, format, pattern, factory](Job& job) {
    std::string e;
    job.setStatus("Exporting " + fs::path(path).filename().string());
    if (format == "midi") {
      MidiFileData m = pattern != kNoId ? patternToMidi(*song, pattern) : songToMidi(*song);
      if (m.tracks.empty()) return fail("no notes to export");
      if (!writeMidiFile(path, m, &e)) return fail(e);
    } else if (format == "flp") {
      flp::ExportOptions opt;
      opt.factoryData = factory;
      flp::ExportReport rep;
      if (!flp::exportFlpFile(*song, path, rep, opt, &e)) return fail(e);
    } else {
      flp::PackageOptions opt;
      opt.flp.factoryData = factory;
      flp::PackageReport rep;
      if (!flp::exportFlpZip(*song, path, fs::path(path).stem().string(), opt, rep, &e)) return fail(e);
    }
    std::error_code ec;
    ToolResult r = ok(jobj({{"path", jstr(path)}, {"format", jstr(format)}, {"bytes", jnum(static_cast<double>(fs::file_size(path, ec)))}}),
                      "exported " + fs::path(path).filename().string());
    r.changed = true;
    return r;
  };
}

ToolResult tGetJob(Environment& env, const Json& a) {
  pollJobs(env);
  auto j = env.jobs.find(static_cast<uint64_t>(argInt(a, "job")));
  if (!j) return fail("no job " + std::to_string(argInt(a, "job")) + " (finished jobs are kept for a while only)");
  return ok(jobJson(*j), "job " + std::to_string(j->id));
}

ToolResult tCancelJob(Environment& env, const Json& a) {
  auto j = env.jobs.find(static_cast<uint64_t>(argInt(a, "job")));
  if (!j) return fail("no job " + std::to_string(argInt(a, "job")));
  if (!j->done.load()) j->cancel.store(true);
  return ok(jobJson(*j), "cancel job " + std::to_string(j->id));
}

ToolResult tSave(Environment& env, const Json& a) {
  std::string path = env.projectPath, err;
  if (has(a, "path")) {
    if (!resolveWritePath(argStr(a, "path"), env.writeRoots(), {".json"}, path, err)) return fail(err);
  } else {
    if (path.empty()) return fail("the project has no file yet: give a path (e.g. \"My song.gridline.json\" in the export folder)");
    std::string lp = path;
    for (char& c : lp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lp.size() < 5 || lp.compare(lp.size() - 5, 5, ".json") != 0)
      return fail("the project was opened from another format: give a path ending in .gridline.json");
  }
  if (env.save) {
    if (!env.save(path, err)) return fail(err.empty() ? "save failed" : err);
  } else {
    if (!saveProject(path, env.doc.song(), &err)) return fail(err);
    env.doc.markSaved();
  }
  env.projectPath = path;
  ToolResult r = ok(jobj({{"path", jstr(path)}}), "saved " + fs::path(path).filename().string());
  r.changed = true;
  return r;
}

ToolResult tUndo(Environment& env, const Json&) {
  Document& d = env.doc;
  const std::string label = d.history().undoLabel();
  if (label.rfind("AI: ", 0) != 0)
    return fail(label.empty() ? "nothing to undo" : "the last change (\"" + label + "\") was not made by an assistant; only assistant changes can be undone here");
  d.endGesture();
  d.undo();
  return ok(jobj({{"undone", jstr(label)}}), "undid \"" + label + "\"");
}

ToolResult tRedo(Environment& env, const Json&) {
  Document& d = env.doc;
  const std::string label = d.history().redoLabel();
  if (label.rfind("AI: ", 0) != 0) return fail(label.empty() ? "nothing to redo" : "the next step was not made by an assistant");
  d.redo();
  return ok(jobj({{"redone", jstr(label)}}), "redid \"" + label + "\"");
}

// ------------------------------------------------------------- catalogue ----
std::vector<Tool> buildTools() {
  std::vector<Tool> v;
  auto add = [&](const char* name, const char* title, const std::string& desc, Json schema, bool readOnly,
                 std::function<ToolResult(Environment&, const Json&)> fn) -> Tool& {
    Tool t;
    t.name = name;
    t.title = title;
    t.description = desc;
    t.inputSchema = std::move(schema);
    t.readOnly = readOnly;
    t.run = std::move(fn);
    v.push_back(std::move(t));
    return v.back();
  };
  const Json noArgs = objSchema({});
  // J-61: job tools wait for the result by default; "wait": false returns a job number.
  auto withWait = [](std::vector<std::pair<std::string, Json>> p) {
    p.push_back({"wait", boolP("Wait for the result (default true). false: return a job number at once and poll get_job")});
    return p;
  };
  Json noteItem = objSchema({{"start", numP("Start in beats from the pattern start", 0, 1e6)},
                             {"length", numP("Length in beats (default 1)", 0.0001, 1e6)},
                             {"pitch", intP("MIDI note number (60 = middle C)", 0, 127)},
                             {"velocity", intP("Velocity (default 100)", 1, 127)},
                             {"pan", intP("Note pan -64..64", -64, 64)},
                             {"fine", intP("Fine pitch in cents", -1200, 1200)},
                             {"muted", boolP("Kept but not played")},
                             {"slide", boolP("Slide note")},
                             {"porta", boolP("Glide from the previous note")}},
                            {"start", "pitch"});
  auto targetProps = [](bool withValue) {
    std::vector<std::pair<std::string, Json>> p = {
        {"channel", intP(std::string(kChannelD) + "; omit channel and mixer for song parameters", 1, 999)},
        {"mixer", intP(kMixerD, 0, 999)},
        {"slot", intP(std::string(kSlotD) + " (with mixer)", 1, kEffectSlots)},
        {"param", strP(kParamD, 128)}};
    if (withValue) {
      p.push_back({"value", numP("New value in the parameter's own units (see get_parameters), or 0..1 with normalized")});
      p.push_back({"normalized", boolP("value is 0..1 of the parameter's range")});
    }
    return p;
  };

  // ---- inspect
  add("get_song", "song overview",
      "Song settings (title, tempo, time signature, PPQ, swing, length in beats / bars / seconds), counts, a short list of "
      "channels, patterns, used mixer tracks and markers, and whether there are unsaved changes.",
      noArgs, true, tGetSong);
  add("list_channels", "list channels",
      "Every channel of the channel rack: number, name, instrument plugin type, volume (0..1.25), pan (-1..1), mute, pitch, "
      "mixer track, colour, note count and sample file name.",
      noArgs, true, tListChannels);
  add("list_patterns", "list patterns", "Every pattern: number, name, length in bars and steps per bar, and which channels have notes or steps in it.",
      noArgs, true, tListPatterns);
  add("get_pattern", "read a pattern",
      "Notes (start / length in beats, pitch, velocity) and step-sequencer rows (velocity per step, 0 = off) of a pattern, for one "
      "channel or every channel that has data.",
      objSchema({{"pattern", intP(kPatternD, 1, 999)}, {"channel", intP(kChannelD, 1, 999)}}, {"pattern"}), true, tGetPattern);
  add("get_playlist", "read the playlist",
      "Playlist clips (id, kind pattern / audio / automation, track, start and length in beats, muted) and the tracks in use. Clip "
      "ids are used by edit_clip and remove_clip.",
      objSchema({{"from", numP("Only clips that end after this beat", 0)},
                 {"to", numP("Only clips that start before this beat", 0)},
                 {"track", intP("Only this playlist track", 1, kMaxPlaylistTracks)},
                 {"limit", intP("Most clips returned (default 500)", 1, 5000)}}),
      true, tGetPlaylist);
  add("get_mixer", "read the mixer",
      "Mixer tracks: name, volume, pan, mute, solo, effects per slot, sends and the channels routed there. Without arguments only "
      "the master and tracks in use are listed.",
      objSchema({{"mixer", intP(kMixerD, 0, 999)}, {"all", boolP("List every mixer track, also unused ones")}}), true, tGetMixer);
  add("get_parameters", "read parameters",
      "Parameters of the song (no target), a channel (strip + instrument), a mixer track, or an effect in a mixer slot: API name, "
      "display name, value, formatted value, range, default and choices.",
      objSchema({{"channel", intP(kChannelD, 1, 999)},
                 {"mixer", intP(kMixerD, 0, 999)},
                 {"slot", intP(std::string(kSlotD) + " (with mixer)", 1, kEffectSlots)},
                 {"query", strP("Only parameters whose name contains this text", 128)},
                 {"offset", intP("Skip this many matches", 0, 100000)},
                 {"limit", intP("Most parameters returned (default 200)", 1, 2000)}}),
      true, tGetParameters);
  add("get_automation", "read automation", "Automation clips: target parameter, points (position in beats, value 0..1) and how often each is placed.",
      objSchema({{"automation", intP("Only this automation clip (1 = first)", 1, 100000)}}), true, tGetAutomation);
  add("get_markers", "read markers", "Song markers: position in beats, name, time signature changes and actions.", noArgs, true, tGetMarkers);
  add("list_plugin_catalog", "list plugins",
      "Built-in instruments, effects and note processors (and scanned third-party plugins): type id, name, category, parameter "
      "count, factory presets, and for flagships (Lattice, Shuttle, Kerf, ...) a tagline; search keywords keep former names "
      "(\"time gate\", \"matrix fm\") findable. Use the type id with add_channel, edit_channel and add_plugin.",
      objSchema({{"kind", enumP("Only this kind", {"instrument", "effect", "note_fx"})},
                 {"query", strP("Only plugins whose name, type, category or search keywords contain this text", 128)},
                 {"includeParams", boolP("Also list every parameter (large without a query)")}}),
      true, tCatalog);
  add("list_presets", "list presets", "Factory presets and the user's saved presets for a plugin type.",
      objSchema({{"type", strP("Plugin type id, e.g. \"gl.synth\"", 128)}}, {"type"}), true, tPresets);
  add("get_history", "read undo history", "The latest undo steps (assistant steps are labelled \"AI: ...\") and whether the project has unsaved changes.",
      noArgs, true, tHistory);

  // ---- edit
  add("set_song", "change song settings", "Sets tempo (BPM), time signature, swing (0..1), master pitch (semitones) and song info. One undo step.",
      objSchema({{"tempo", numP("Tempo in BPM", 10, 999)},
                 {"numerator", intP("Time signature numerator", 1, 16)},
                 {"denominator", intEnumP("Time signature denominator", {2, 4, 8, 16})},
                 {"swing", numP("Global swing 0..1", 0, 1)},
                 {"masterPitch", numP("Master pitch in semitones", -12, 12)},
                 {"title", strP("Song title")},
                 {"author", strP("Author")},
                 {"genre", strP("Genre")},
                 {"comments", strP("Comments", 16384)}}),
      false, tSetSong);
  const std::vector<std::pair<std::string, Json>> channelEdits = {
      {"name", strP("Channel name")},
      {"plugin", strP("Instrument plugin type (list_plugin_catalog kind=instrument)", 128)},
      {"preset", strP("Preset name for the instrument (list_presets)", 256)},
      {"volume", numP("Volume 0..1.25 (0.78 = default)", 0, 1.25)},
      {"pan", numP("Pan -1 (left) .. 1 (right)", -1, 1)},
      {"pitch", intP("Pitch in semitones", -24, 24)},
      {"muted", boolP("Mute")},
      {"mixer", intP("Route to this mixer track (0 = master)", 0, 999)},
      {"color", strP("Colour \"#RRGGBB\"", 7)}};
  {
    auto p = channelEdits;
    p.push_back({"position", intP("Place it at this rack position (default: last)", 1, 1000)});
    add("add_channel", "add channel", "Adds a channel with an instrument (default gl.synth), optionally with a preset, volume, routing. One undo step.",
        objSchema(p), false, tAddChannel);
    p = channelEdits;
    p.insert(p.begin(), {"channel", intP(kChannelD, 1, 999)});
    add("edit_channel", "edit channel", "Changes a channel's name, instrument, preset, volume, pan, pitch, mute, mixer routing or colour. One undo step.",
        objSchema(p, {"channel"}), false, tEditChannel);
  }
  add("remove_channel", "remove channel", "Deletes a channel and its notes in every pattern. One undo step.",
      objSchema({{"channel", intP(kChannelD, 1, 999)}}, {"channel"}), false, tRemoveChannel)
      .destructive = true;
  add("add_pattern", "add pattern", "Adds an empty pattern. One undo step.",
      objSchema({{"name", strP("Pattern name")}, {"bars", intP("Length in bars (default 1)", 1, kMaxPatternBars)}, {"stepsPerBar", intP("Step resolution (default 16)", 1, 64)}}),
      false, tAddPattern);
  add("edit_pattern", "edit pattern", "Renames a pattern, changes its length or colour. One undo step.",
      objSchema({{"pattern", intP(kPatternD, 1, 999)},
                 {"name", strP("Pattern name")},
                 {"bars", intP("Length in bars", 1, kMaxPatternBars)},
                 {"stepsPerBar", intP("Step resolution", 1, 64)},
                 {"color", strP("Colour \"#RRGGBB\"", 7)}},
                {"pattern"}),
      false, tEditPattern);
  add("remove_pattern", "remove pattern", "Deletes a pattern and its playlist clips. One undo step.",
      objSchema({{"pattern", intP(kPatternD, 1, 999)}}, {"pattern"}), false, tRemovePattern)
      .destructive = true;
  add("set_notes", "write notes",
      "Writes piano-roll notes of one channel in one pattern. mode \"replace\" (default) replaces that channel's notes (an empty list "
      "clears them), \"add\" keeps the existing ones. Times are in beats from the pattern start. One undo step.",
      objSchema({{"pattern", intP(kPatternD, 1, 999)},
                 {"channel", intP(kChannelD, 1, 999)},
                 {"notes", arrP("Notes", noteItem, 100000)},
                 {"mode", enumP("replace (default) or add", {"replace", "add"})}},
                {"pattern", "channel", "notes"}),
      false, tSetNotes);
  add("set_steps", "write steps",
      "Sets the step-sequencer row of one channel in one pattern: one velocity per step (0 = off, 100 = normal hit, up to 127). An "
      "all-zero row clears it. One undo step.",
      objSchema({{"pattern", intP(kPatternD, 1, 999)},
                 {"channel", intP(kChannelD, 1, 999)},
                 {"steps", arrP("Velocity per step", intP("Velocity 0..127", 0, 127), 4096)}},
                {"pattern", "channel", "steps"}),
      false, tSetSteps);
  add("add_clip", "add pattern clip", "Places a pattern on the playlist. Returns the clip id. One undo step.",
      objSchema({{"pattern", intP(kPatternD, 1, 999)},
                 {"track", intP("Playlist track (1 = first)", 1, kMaxPlaylistTracks)},
                 {"start", numP("Start in beats from the song start", 0, 1e7)},
                 {"length", numP("Length in beats (default: the pattern's length)", 0.0001, 1e7)}},
                {"pattern", "track", "start"}),
      false, tAddClip);
  add("edit_clip", "edit clip", "Moves a playlist clip, changes its length or mutes it. One undo step.",
      objSchema({{"clip", intP("Clip id (get_playlist)", 1, 4294967295.0)},
                 {"track", intP("Playlist track", 1, kMaxPlaylistTracks)},
                 {"start", numP("Start in beats", 0, 1e7)},
                 {"length", numP("Length in beats", 0.0001, 1e7)},
                 {"muted", boolP("Mute the clip")}},
                {"clip"}),
      false, tEditClip);
  add("remove_clip", "remove clip", "Deletes a playlist clip. One undo step.", objSchema({{"clip", intP("Clip id (get_playlist)", 1, 4294967295.0)}}, {"clip"}),
      false, tRemoveClip)
      .destructive = true;
  {
    auto p = targetProps(false);
    Json pt = objSchema({{"position", numP("Beats from the clip start", 0, 1e7)}, {"value", numP("0..1 of the parameter's range", 0, 1)}}, {"position", "value"});
    p.push_back({"points", arrP("Automation points", pt, 10000)});
    p.push_back({"name", strP("Clip name (default: the parameter's name)")});
    p.push_back({"track", intP("Also place the clip on this playlist track", 1, kMaxPlaylistTracks)});
    p.push_back({"start", numP("Start of the placed clip in beats (default 0)", 0, 1e7)});
    p.push_back({"length", numP("Length of the placed clip in beats (default: its points)", 0.0001, 1e7)});
    add("add_automation", "add automation",
        "Creates an automation clip for a parameter (same target arguments as set_parameter) with points, and optionally places it on "
        "the playlist. One undo step.",
        objSchema(p, {"param", "points"}), false, tAddAutomation);
  }
  add("add_plugin", "add effect",
      "Loads an effect into a mixer track's slot (the first free slot unless slot is given; an occupied slot is replaced), optionally "
      "with a preset. One undo step.",
      objSchema({{"mixer", intP(kMixerD, 0, 999)},
                 {"slot", intP(kSlotD, 1, kEffectSlots)},
                 {"plugin", strP("Effect plugin type (list_plugin_catalog kind=effect)", 128)},
                 {"preset", strP("Preset name (list_presets)", 256)}},
                {"mixer", "plugin"}),
      false, tAddPlugin);
  add("remove_plugin", "remove effect", "Removes the effect in a mixer slot. One undo step.",
      objSchema({{"mixer", intP(kMixerD, 0, 999)}, {"slot", intP(kSlotD, 1, kEffectSlots)}}, {"mixer", "slot"}), false, tRemovePlugin)
      .destructive = true;
  add("set_parameter", "set parameter",
      "Sets one parameter of the song, a channel (strip or instrument), a mixer track or an effect slot. One undo step.",
      objSchema(targetProps(true), {"param", "value"}), false, tSetParameter);
  add("edit_mixer_track", "edit mixer track", "Renames a mixer track, sets volume / pan / mute / solo, or routes it to one other track. One undo step.",
      objSchema({{"mixer", intP(kMixerD, 0, 999)},
                 {"name", strP("Track name")},
                 {"volume", numP("Volume 0..1.25 (0.8 = default)", 0, 1.25)},
                 {"pan", numP("Pan -1..1", -1, 1)},
                 {"muted", boolP("Mute")},
                 {"solo", boolP("Solo")},
                 {"routeTo", intP("Send only to this mixer track (replaces its sends)", 0, 999)}},
                {"mixer"}),
      false, tEditMixerTrack);
  add("add_marker", "add marker", "Adds a song marker, optionally a time signature change. One undo step.",
      objSchema({{"position", numP("Position in beats", 0, 1e7)},
                 {"name", strP("Marker name")},
                 {"numerator", intP("Time signature change: numerator", 1, 16)},
                 {"denominator", intEnumP("Time signature change: denominator", {2, 4, 8, 16})}},
                {"position"}),
      false, tAddMarker);
  add("remove_marker", "remove marker", "Deletes a song marker. One undo step.", objSchema({{"marker", intP("Marker number (1 = first)", 1, 100000)}}, {"marker"}),
      false, tRemoveMarker)
      .destructive = true;
  add("run_script", "run project script",
      "Runs a Gridline Lua project script (the `project` API of gridline://docs/scripting) in the sandbox with time, instruction and "
      "memory limits. The whole run is one undo step; a failing script changes nothing. Returns the script's log.",
      objSchema({{"code", strP("Lua source", 256u << 10)}, {"name", strP("Name shown in the undo history")}, {"seed", intP("math.random seed", 0, 2147483647)}},
                {"code"}),
      false, tRunScript);
  add("undo", "undo", "Undoes the latest change if an assistant made it (label \"AI: ...\").", noArgs, false, tUndo).grouped = false;
  add("redo", "redo", "Redoes the next assistant change that was undone.", noArgs, false, tRedo).grouped = false;

  // ---- audio and files
  const std::vector<std::pair<std::string, Json>> renderArgs = {
      {"scope", enumP("song (default: the arrangement) or pattern", {"song", "pattern"})},
      {"pattern", intP("Pattern for scope pattern (default 1)", 1, 999)},
      {"sampleRate", intEnumP("Sample rate (default 44100)", {44100, 48000, 88200, 96000})},
      {"maxSeconds", numP("Render at most this many seconds", 1, 3600)}};
  add("analyze_loudness", "analyse loudness",
      "Renders the song or a pattern in memory (no file) and measures integrated loudness (LUFS, BS.1770), sample peak, RMS, length "
      "and clipping.",
      objSchema(withWait(renderArgs)), true, nullptr)
      .prepare = pAnalyze;
  {
    auto p = renderArgs;
    p.insert(p.begin(), {"path", strP("Output .wav, relative to the export folder (or the project folder)", 1024)});
    p.push_back({"bits", intEnumP("Bit depth (default 24)", {16, 24})});
    Tool& t = add("render_audio", "render to WAV",
                  "Renders the song or a pattern to a WAV file in the export or project folder and reports its loudness.",
                  objSchema(withWait(p), {"path"}), false, nullptr);
    t.writesFiles = true;
    t.prepare = pRender;
  }
  {
    Tool& t = add("export_project", "export project",
                  "Exports MIDI (.mid, the song or one pattern), an FL Studio project (.flp) or an FL Studio zipped loop package (.zip) into the "
                  "export or project folder.",
                  objSchema(withWait({{"format", enumP("midi, flp or flp_zip", {"midi", "flp", "flp_zip"})},
                                      {"path", strP("Output file, relative to the export folder (or the project folder)", 1024)},
                                      {"pattern", intP("MIDI: only this pattern", 1, 999)}}),
                            {"format", "path"}),
                  false, nullptr);
    t.writesFiles = true;
    t.prepare = pExport;
  }
  add("get_job", "check a job",
      "State (running, done, failed, cancelled), progress 0..1 and, once done, the result of a render, loudness analysis or export "
      "started with \"wait\": false.",
      objSchema({{"job", intP("Job number returned by the call", 1, 1e15)}}, {"job"}), true, tGetJob);
  add("cancel_job", "cancel a job", "Stops a running render, loudness analysis or export (nothing is written).",
      objSchema({{"job", intP("Job number returned by the call", 1, 1e15)}}, {"job"}), true, tCancelJob);
  add("save_project", "save project",
      "Saves the project to its file, or to a new .gridline.json path inside the project or export folder.",
      objSchema({{"path", strP("Save as this file (relative to the project folder)", 1024)}}), false, tSave)
      .writesFiles = true;
  for (auto& t : v) {
    if (t.writesFiles || t.prepare) t.grouped = false;
    if (t.prepare && !t.run)  // the same work inside the call (no worker)
      t.run = [prep = t.prepare](Environment& env, const Json& args) {
        std::string err;
        JobWork w = prep(env, args, err);
        if (!w) return fail(err);
        Job job;
        return w(job);
      };
  }
  return v;
}

}  // namespace

const std::vector<Tool>& tools() {
  static const std::vector<Tool> v = buildTools();
  return v;
}

const Tool* findTool(const std::string& name) {
  for (const auto& t : tools())
    if (t.name == name) return &t;
  return nullptr;
}

}  // namespace gl::mcp

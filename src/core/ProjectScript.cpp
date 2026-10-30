// Gridline — project scripts (see ProjectScript.h and docs/scripting.md).
//
// Every C function here validates its arguments with luaL_check* / argerror
// (Lua is built as C++, so errors unwind with exceptions), re-reads the song
// after each Document call (edits may reallocate the containers), and charges
// its work against the VM's budget. Structural edits cost more, which bounds
// how much undo memory one run can build up.
#include "ProjectScript.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include "Params.h"
#include "lua/src/lauxlib.h"
#include "lua/src/lua.h"

namespace gl::script {

namespace fs = std::filesystem;

namespace {

constexpr const char* kHostKey = "gl.projectHost";
constexpr uint64_t kCostRead = 4;        // per call that reads
constexpr uint64_t kCostEdit = 1000;     // per object edit (undo keeps a copy)
constexpr uint64_t kCostStruct = 5000;   // add / remove / whole-song edits
constexpr size_t kMaxName = 256;
constexpr Tick kMaxTick = Tick{1} << 40;
constexpr size_t kMaxChannels = 999, kMaxPatterns = 999, kMaxClips = 100000, kMaxAutomation = 10000, kMaxMarkers = 10000;

ProjectHost& host(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
  auto* h = static_cast<ProjectHost*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!h) luaL_error(L, "project API not available");
  return *h;
}
Document& doc(lua_State* L) { return host(L).doc; }
const Song& song(lua_State* L) { return host(L).doc.song(); }
void charge(lua_State* L, uint64_t units) { Vm::from(L)->charge(L, units); }

// ---- argument helpers ----
double num(lua_State* L, int arg) {
  double v = luaL_checknumber(L, arg);
  if (!std::isfinite(v)) luaL_argerror(L, arg, "number must be finite");
  return v;
}
Tick tick(lua_State* L, int arg) {
  double v = num(L, arg);
  if (v < 0 || v > static_cast<double>(kMaxTick)) luaL_argerror(L, arg, "tick position out of range");
  return static_cast<Tick>(std::llround(v));
}
std::string str(lua_State* L, int arg) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, arg, &n);
  if (n > kMaxName) luaL_argerror(L, arg, "text too long (256 bytes at most)");
  return std::string(s, n);
}
Rgb color(lua_State* L, int arg) {
  lua_Integer v = luaL_checkinteger(L, arg);
  if (v < 0 || v > 0xFFFFFF) luaL_argerror(L, arg, "colour must be 0x000000..0xFFFFFF");
  return static_cast<Rgb>(v);
}
bool flag(lua_State* L, int arg) {
  luaL_checkany(L, arg);
  return lua_toboolean(L, arg) != 0;
}
// 1-based index into a container of n elements -> 0-based.
size_t index1(lua_State* L, int arg, size_t n, const char* what) {
  lua_Integer i = luaL_checkinteger(L, arg);
  if (i < 1 || static_cast<size_t>(i) > n) luaL_argerror(L, arg, what);
  return static_cast<size_t>(i - 1);
}
size_t chIdx(lua_State* L, int arg) { return index1(L, arg, song(L).channels.size(), "no such channel"); }
size_t patIdx(lua_State* L, int arg) { return index1(L, arg, song(L).patterns.size(), "no such pattern"); }
size_t autoIdx(lua_State* L, int arg) { return index1(L, arg, song(L).automations.size(), "no such automation clip"); }
size_t markerIdx(lua_State* L, int arg) { return index1(L, arg, song(L).markers.size(), "no such marker"); }
size_t trackIdx(lua_State* L, int arg) { return index1(L, arg, song(L).tracks.size(), "no such playlist track"); }
int slotIdx(lua_State* L, int arg) { return static_cast<int>(index1(L, arg, kEffectSlots, "effect slots are 1..10")); }
// Mixer: 0 = master.
size_t mixIdx(lua_State* L, int arg) {
  lua_Integer i = luaL_checkinteger(L, arg);
  if (i < 0 || static_cast<size_t>(i) >= song(L).mixer.size()) luaL_argerror(L, arg, "no such mixer track");
  return static_cast<size_t>(i);
}
ObjectId chId(lua_State* L, int arg) { return song(L).channels[chIdx(L, arg)].id; }
ObjectId patId(lua_State* L, int arg) { return song(L).patterns[patIdx(L, arg)].id; }
ObjectId mixId(lua_State* L, int arg) { return song(L).mixer[mixIdx(L, arg)].id; }
ObjectId autoId(lua_State* L, int arg) { return song(L).automations[autoIdx(L, arg)].id; }
// Clip lookups scan the playlist: charged by its size.
void chargeClipScan(lua_State* L, size_t lookups = 1) { charge(L, kCostRead + lookups * (song(L).playlist.size() / 16)); }
ObjectId clipId(lua_State* L, int arg) {
  chargeClipScan(L);
  lua_Integer v = luaL_checkinteger(L, arg);
  if (v <= 0 || v > 0xFFFFFFFFLL || !song(L).clip(static_cast<ObjectId>(v))) luaL_argerror(L, arg, "no such clip");
  return static_cast<ObjectId>(v);
}

void pushIndexOrNil(lua_State* L, int i0) {
  if (i0 < 0)
    lua_pushnil(L);
  else
    lua_pushinteger(L, i0 + 1);
}
void setI(lua_State* L, const char* k, lua_Integer v) {
  lua_pushinteger(L, v);
  lua_setfield(L, -2, k);
}
void setN(lua_State* L, const char* k, double v) {
  lua_pushnumber(L, v);
  lua_setfield(L, -2, k);
}
void setB(lua_State* L, const char* k, bool v) {
  lua_pushboolean(L, v);
  lua_setfield(L, -2, k);
}
void setS(lua_State* L, const char* k, const std::string& v) {
  lua_pushlstring(L, v.data(), v.size());
  lua_setfield(L, -2, k);
}

// ---- parameters (shared by channels, mixer, params) ----
void checkParamValid(lua_State* L, ParamId id, int arg) {
  if (!describeParam(song(L), id)) luaL_argerror(L, arg, "no such parameter");
}
int pushParam(lua_State* L, ParamId id) {
  auto v = getParam(song(L), id);
  if (!v) return luaL_error(L, "no such parameter");
  lua_pushnumber(L, *v);
  return 1;
}
void setParamValue(lua_State* L, ParamId id, double v) {
  charge(L, kCostEdit);
  if (!doc(L).setParam(id, v, false)) luaL_error(L, "no such parameter");
}
ParamId channelParamRef(lua_State* L, ObjectId ch, int arg) {
  if (lua_type(L, arg) == LUA_TSTRING) {
    std::string w = lua_tostring(L, arg);
    if (w == "volume") return channelParam(ch, ChannelParam::Volume);
    if (w == "pan") return channelParam(ch, ChannelParam::Pan);
    if (w == "mute") return channelParam(ch, ChannelParam::Mute);
    if (w == "pitch") return channelParam(ch, ChannelParam::Pitch);
    luaL_argerror(L, arg, "volume, pan, mute, pitch or a plugin parameter number");
  }
  lua_Integer k = luaL_checkinteger(L, arg);
  if (k < 1 || k > 4000) luaL_argerror(L, arg, "no such parameter");
  ParamId id = channelPluginParam(ch, static_cast<int>(k - 1));
  checkParamValid(L, id, arg);
  return id;
}
ParamId mixerParamRef(lua_State* L, ObjectId t, int arg) {
  static const std::pair<const char*, MixerParam> names[] = {
      {"volume", MixerParam::Volume},       {"pan", MixerParam::Pan},             {"stereo", MixerParam::StereoSep},
      {"mute", MixerParam::Mute},           {"eqLowGain", MixerParam::EqLowGain}, {"eqMidGain", MixerParam::EqMidGain},
      {"eqHighGain", MixerParam::EqHighGain}, {"eqLowFreq", MixerParam::EqLowFreq}, {"eqMidFreq", MixerParam::EqMidFreq},
      {"eqHighFreq", MixerParam::EqHighFreq}, {"eqLowQ", MixerParam::EqLowQ},   {"eqMidQ", MixerParam::EqMidQ},
      {"eqHighQ", MixerParam::EqHighQ}};
  std::string w = luaL_checkstring(L, arg);
  for (const auto& [n, p] : names)
    if (w == n) return mixerParam(t, p);
  luaL_argerror(L, arg, "unknown mixer parameter");
  return {};
}
ParamId slotParamRef(lua_State* L, ObjectId t, int slot, int arg) {
  lua_Integer k = luaL_checkinteger(L, arg);
  if (k < 1 || k > 990) luaL_argerror(L, arg, "no such parameter");
  ParamId id = slotParam(t, slot, static_cast<int>(k - 1));
  checkParamValid(L, id, arg);
  return id;
}
lua_Integer packRef(ParamId id) { return static_cast<lua_Integer>(id.packed()); }
ParamId refArg(lua_State* L, int arg) {
  lua_Integer v = luaL_checkinteger(L, arg);
  ParamId id = ParamId::unpack(static_cast<uint64_t>(v));
  if (v < 0 || !describeParam(song(L), id)) luaL_argerror(L, arg, "not a parameter reference (or it no longer resolves)");
  return id;
}
int pushParamInfo(lua_State* L, ParamId id, int what) {
  auto d = describeParam(song(L), id);
  if (!d) return luaL_error(L, "no such parameter");
  switch (what) {
    case 0: lua_pushstring(L, d->name.c_str()); return 1;
    case 1:
      lua_pushnumber(L, d->min);
      lua_pushnumber(L, d->max);
      lua_pushnumber(L, d->def);
      return 3;
    default: {
      auto v = getParam(song(L), id);
      lua_pushstring(L, d->format(v ? *v : d->def).c_str());
      return 1;
    }
  }
}

// ================================================================== song ====
int s_tempo(lua_State* L) {
  lua_pushnumber(L, song(L).tempo);
  return 1;
}
int s_setTempo(lua_State* L) {
  setParamValue(L, songParam(SongParam::Tempo), num(L, 1));
  return 0;
}
int s_ppq(lua_State* L) {
  lua_pushinteger(L, song(L).ppq);
  return 1;
}
int s_timeSig(lua_State* L) {
  lua_pushinteger(L, song(L).timeSigNum);
  lua_pushinteger(L, song(L).timeSigDen);
  return 2;
}
int s_setTimeSig(lua_State* L) {
  lua_Integer n = luaL_checkinteger(L, 1), d = luaL_checkinteger(L, 2);
  if (n < 1 || n > 16) luaL_argerror(L, 1, "1..16");
  if (d != 2 && d != 4 && d != 8 && d != 16) luaL_argerror(L, 2, "2, 4, 8 or 16");
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).setTimeSignature(static_cast<int>(n), static_cast<int>(d)));
  return 1;
}
int s_swing(lua_State* L) {
  lua_pushnumber(L, song(L).swing);
  return 1;
}
int s_setSwing(lua_State* L) {
  setParamValue(L, songParam(SongParam::Swing), num(L, 1));
  return 0;
}
int s_masterPitch(lua_State* L) {
  lua_pushnumber(L, song(L).masterPitch);
  return 1;
}
int s_setMasterPitch(lua_State* L) {
  setParamValue(L, songParam(SongParam::MasterPitch), num(L, 1));
  return 0;
}
int s_bar(lua_State* L) {
  lua_pushinteger(L, song(L).barTicks());
  return 1;
}
int s_beat(lua_State* L) {
  lua_pushinteger(L, song(L).beatTicks());
  return 1;
}
int s_length(lua_State* L) {
  charge(L, kCostRead + song(L).playlist.size() / 8);
  lua_pushinteger(L, song(L).lengthTicks());
  return 1;
}
int s_info(lua_State* L) {
  const SongInfo& i = song(L).info;
  lua_createtable(L, 0, 5);
  setS(L, "title", i.title);
  setS(L, "author", i.author);
  setS(L, "genre", i.genre);
  setS(L, "url", i.url);
  setS(L, "comments", i.comments);
  return 1;
}
int s_setInfo(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  SongInfo i = song(L).info;
  auto field = [&](const char* k, std::string& out, size_t max) {
    lua_getfield(L, 1, k);
    if (!lua_isnil(L, -1)) {
      size_t n = 0;
      const char* s = lua_tolstring(L, -1, &n);
      if (!s || lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "info.%s must be a string", k);
      out.assign(s, std::min(n, max));
    }
    lua_pop(L, 1);
  };
  field("title", i.title, kMaxName);
  field("author", i.author, kMaxName);
  field("genre", i.genre, kMaxName);
  field("url", i.url, kMaxName);
  field("comments", i.comments, 65536);
  charge(L, kCostStruct);
  doc(L).setInfo(i);
  return 0;
}

// ============================================================== channels ====
int c_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).channels.size()));
  return 1;
}
int c_id(lua_State* L) {
  lua_pushinteger(L, chId(L, 1));
  return 1;
}
int c_index(lua_State* L) {
  lua_Integer v = luaL_checkinteger(L, 1);
  pushIndexOrNil(L, v > 0 && v <= 0xFFFFFFFFLL ? song(L).channelIndex(static_cast<ObjectId>(v)) : -1);
  return 1;
}
int c_find(lua_State* L) {
  std::string n = str(L, 1);
  const auto& cs = song(L).channels;
  charge(L, kCostRead + cs.size() / 8);
  for (size_t i = 0; i < cs.size(); ++i)
    if (cs[i].name == n) {
      lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
      return 1;
    }
  lua_pushnil(L);
  return 1;
}
int c_name(lua_State* L) {
  lua_pushstring(L, song(L).channels[chIdx(L, 1)].name.c_str());
  return 1;
}
int c_setName(lua_State* L) {
  ObjectId id = chId(L, 1);
  std::string n = str(L, 2);
  charge(L, kCostEdit);
  doc(L).renameChannel(id, n);
  return 0;
}
int c_color(lua_State* L) {
  lua_pushinteger(L, song(L).channels[chIdx(L, 1)].color);
  return 1;
}
int c_setColor(lua_State* L) {
  ObjectId id = chId(L, 1);
  Rgb c = color(L, 2);
  charge(L, kCostEdit);
  doc(L).editChannel(id, "channel colour", [c](Channel& ch) { ch.color = c; });
  return 0;
}
int c_volume(lua_State* L) { return pushParam(L, channelParam(chId(L, 1), ChannelParam::Volume)); }
int c_setVolume(lua_State* L) {
  setParamValue(L, channelParam(chId(L, 1), ChannelParam::Volume), num(L, 2));
  return 0;
}
int c_pan(lua_State* L) { return pushParam(L, channelParam(chId(L, 1), ChannelParam::Pan)); }
int c_setPan(lua_State* L) {
  setParamValue(L, channelParam(chId(L, 1), ChannelParam::Pan), num(L, 2));
  return 0;
}
int c_muted(lua_State* L) {
  lua_pushboolean(L, song(L).channels[chIdx(L, 1)].muted);
  return 1;
}
int c_setMuted(lua_State* L) {
  setParamValue(L, channelParam(chId(L, 1), ChannelParam::Mute), flag(L, 2) ? 1.0 : 0.0);
  return 0;
}
int c_pitch(lua_State* L) { return pushParam(L, channelParam(chId(L, 1), ChannelParam::Pitch)); }
int c_setPitch(lua_State* L) {
  setParamValue(L, channelParam(chId(L, 1), ChannelParam::Pitch), num(L, 2));
  return 0;
}
int c_plugin(lua_State* L) {
  lua_pushstring(L, song(L).channels[chIdx(L, 1)].plugin.type.c_str());
  return 1;
}
int c_pluginName(lua_State* L) {
  const PluginInfo* p = findPlugin(song(L).channels[chIdx(L, 1)].plugin.type);
  lua_pushstring(L, p ? p->name.c_str() : "");
  return 1;
}
int c_sample(lua_State* L) {
  const Channel& c = song(L).channels[chIdx(L, 1)];
  const PluginInfo* p = findPlugin(c.plugin.type);
  if (p && p->usesSample && !c.plugin.data.empty())
    lua_pushstring(L, fs::path(c.plugin.data.substr(0, c.plugin.data.find('\n'))).filename().string().c_str());  // the name only: no paths for scripts
  else
    lua_pushnil(L);
  return 1;
}
const PluginInfo* generatorArg(lua_State* L, int arg) {
  std::string t = str(L, arg);
  const PluginInfo* p = findPlugin(t);
  if (!p || p->kind != PluginKind::Generator) luaL_argerror(L, arg, "not an instrument type");
  return p;
}
int c_setPlugin(lua_State* L) {
  ObjectId id = chId(L, 1);
  const PluginInfo* p = generatorArg(L, 2);
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).setChannelPlugin(id, p->type));
  return 1;
}
int c_route(lua_State* L) {
  const Song& s = song(L);
  lua_pushinteger(L, std::max(0, s.mixerIndex(s.channels[chIdx(L, 1)].mixerTrack)));
  return 1;
}
int c_setRoute(lua_State* L) {
  ObjectId id = chId(L, 1);
  ObjectId m = mixId(L, 2);
  charge(L, kCostEdit);
  doc(L).routeChannel(id, m);
  return 0;
}
int c_add(lua_State* L) {
  std::string name = luaL_opt(L, str, 1, std::string("Channel"));
  std::string type = lua_isnoneornil(L, 2) ? std::string("gl.synth") : generatorArg(L, 2)->type;
  if (song(L).channels.size() >= kMaxChannels) return luaL_error(L, "too many channels");
  lua_Integer at = luaL_optinteger(L, 3, static_cast<lua_Integer>(song(L).channels.size()) + 1);
  if (at < 1 || static_cast<size_t>(at) > song(L).channels.size() + 1) luaL_argerror(L, 3, "position out of range");
  charge(L, kCostStruct);
  ObjectId id = doc(L).addChannel(name, type, static_cast<int>(at - 1));
  pushIndexOrNil(L, song(L).channelIndex(id));
  return 1;
}
int c_remove(lua_State* L) {
  ObjectId id = chId(L, 1);
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).removeChannel(id));
  return 1;
}
int c_clone(lua_State* L) {
  ObjectId id = chId(L, 1);
  if (song(L).channels.size() >= kMaxChannels) return luaL_error(L, "too many channels");
  charge(L, kCostStruct);
  pushIndexOrNil(L, song(L).channelIndex(doc(L).cloneChannel(id)));
  return 1;
}
int c_move(lua_State* L) {
  ObjectId id = chId(L, 1);
  size_t to = chIdx(L, 2);
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).moveChannel(id, static_cast<int>(to)));
  return 1;
}
int c_paramCount(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).channels[chIdx(L, 1)].plugin.params.size()));
  return 1;
}
int c_paramName(lua_State* L) { return pushParamInfo(L, channelParamRef(L, chId(L, 1), 2), 0); }
int c_paramRange(lua_State* L) { return pushParamInfo(L, channelParamRef(L, chId(L, 1), 2), 1); }
int c_paramText(lua_State* L) { return pushParamInfo(L, channelParamRef(L, chId(L, 1), 2), 2); }
int c_param(lua_State* L) { return pushParam(L, channelParamRef(L, chId(L, 1), 2)); }
int c_setParam(lua_State* L) {
  ParamId id = channelParamRef(L, chId(L, 1), 2);
  setParamValue(L, id, num(L, 3));
  return 0;
}

// ============================================================== patterns ====
int p_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).patterns.size()));
  return 1;
}
int p_id(lua_State* L) {
  lua_pushinteger(L, patId(L, 1));
  return 1;
}
int p_index(lua_State* L) {
  lua_Integer v = luaL_checkinteger(L, 1);
  pushIndexOrNil(L, v > 0 && v <= 0xFFFFFFFFLL ? song(L).patternIndex(static_cast<ObjectId>(v)) : -1);
  return 1;
}
int p_find(lua_State* L) {
  std::string n = str(L, 1);
  const auto& ps = song(L).patterns;
  charge(L, kCostRead + ps.size() / 8);
  for (size_t i = 0; i < ps.size(); ++i)
    if (ps[i].name == n) {
      lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
      return 1;
    }
  lua_pushnil(L);
  return 1;
}
int p_name(lua_State* L) {
  lua_pushstring(L, song(L).patterns[patIdx(L, 1)].name.c_str());
  return 1;
}
int p_setName(lua_State* L) {
  ObjectId id = patId(L, 1);
  std::string n = str(L, 2);
  charge(L, kCostEdit);
  doc(L).renamePattern(id, n);
  return 0;
}
int p_color(lua_State* L) {
  lua_pushinteger(L, song(L).patterns[patIdx(L, 1)].color);
  return 1;
}
int p_setColor(lua_State* L) {
  ObjectId id = patId(L, 1);
  Rgb c = color(L, 2);
  charge(L, kCostEdit);
  doc(L).editPattern(id, "pattern colour", [c](Pattern& p) { p.color = c; });
  return 0;
}
int p_add(lua_State* L) {
  std::string name = luaL_opt(L, str, 1, std::string("Pattern"));
  lua_Integer bars = luaL_optinteger(L, 2, 1);
  lua_Integer spb = luaL_optinteger(L, 3, 16);
  if (bars < 1 || bars > 1024) luaL_argerror(L, 2, "bars 1..1024");
  if (spb < 1 || spb > 64) luaL_argerror(L, 3, "steps per bar 1..64");
  if (song(L).patterns.size() >= kMaxPatterns) return luaL_error(L, "too many patterns");
  charge(L, kCostStruct);
  ObjectId id = doc(L).addPattern(name, static_cast<int>(bars), static_cast<int>(spb));
  pushIndexOrNil(L, song(L).patternIndex(id));
  return 1;
}
int p_remove(lua_State* L) {
  ObjectId id = patId(L, 1);
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).removePattern(id));
  return 1;
}
int p_clone(lua_State* L) {
  ObjectId id = patId(L, 1);
  if (song(L).patterns.size() >= kMaxPatterns) return luaL_error(L, "too many patterns");
  charge(L, kCostStruct);
  pushIndexOrNil(L, song(L).patternIndex(doc(L).clonePattern(id)));
  return 1;
}
int p_length(lua_State* L) {
  const Pattern& p = song(L).patterns[patIdx(L, 1)];
  charge(L, kCostRead + p.notes.size());
  lua_pushinteger(L, song(L).patternContentLength(p));
  return 1;
}
int p_bars(lua_State* L) {
  const Pattern& p = song(L).patterns[patIdx(L, 1)];
  lua_pushinteger(L, p.bars);
  lua_pushinteger(L, p.stepsPerBar);
  return 2;
}
int p_setBars(lua_State* L) {
  ObjectId id = patId(L, 1);
  lua_Integer bars = luaL_checkinteger(L, 2);
  lua_Integer spb = luaL_optinteger(L, 3, song(L).pattern(id)->stepsPerBar);
  if (bars < 1 || bars > 1024) luaL_argerror(L, 2, "bars 1..1024");
  if (spb < 1 || spb > 64) luaL_argerror(L, 3, "steps per bar 1..64");
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).setPatternLength(id, static_cast<int>(bars), static_cast<int>(spb)));
  return 1;
}
int p_steps(lua_State* L) {
  const Pattern& p = song(L).patterns[patIdx(L, 1)];
  ObjectId ch = chId(L, 2);
  auto it = p.steps.find(ch);
  const size_t n = it == p.steps.end() ? 0 : it->second.size();
  charge(L, kCostRead + n / 4);
  lua_createtable(L, static_cast<int>(n), 0);
  for (size_t i = 0; i < n; ++i) {
    lua_pushinteger(L, it->second[i]);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}
int p_setSteps(lua_State* L) {
  ObjectId pat = patId(L, 1);
  ObjectId ch = chId(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 3));
  if (n > 4096) luaL_argerror(L, 3, "at most 4096 steps");
  std::vector<uint8_t> row(static_cast<size_t>(n));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, 3, i);
    double v = 0;
    if (lua_isboolean(L, -1))
      v = lua_toboolean(L, -1) ? 100 : 0;
    else if (lua_isnumber(L, -1))
      v = lua_tonumber(L, -1);
    else
      luaL_error(L, "step %d must be a number or a boolean", static_cast<int>(i));
    lua_pop(L, 1);
    row[static_cast<size_t>(i - 1)] = static_cast<uint8_t>(std::isfinite(v) ? std::clamp(std::round(v), 0.0, 127.0) : 0.0);
  }
  charge(L, kCostEdit + static_cast<uint64_t>(n));
  lua_pushboolean(L, doc(L).setSteps(pat, ch, std::move(row)));
  return 1;
}
int p_notes(lua_State* L) {
  const Pattern& p = song(L).patterns[patIdx(L, 1)];
  ObjectId ch = chId(L, 2);
  auto it = p.notes.find(ch);
  const size_t n = it == p.notes.end() ? 0 : it->second.size();
  charge(L, kCostRead + n * 4);
  lua_createtable(L, static_cast<int>(std::min<size_t>(n, 1u << 20)), 0);
  for (size_t i = 0; i < n; ++i) {
    pushNote(L, it->second[i], false);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}
std::vector<Note> readNotes(lua_State* L, int arg) {
  luaL_checktype(L, arg, LUA_TTABLE);
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, arg));
  if (static_cast<size_t>(n) > PianoRollScript::kMaxNotes) luaL_argerror(L, arg, "too many notes");
  std::vector<Note> out;
  out.reserve(static_cast<size_t>(n));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, arg, i);
    if (!lua_istable(L, -1)) luaL_error(L, "note %d is not a table", static_cast<int>(i));
    Note x;
    bool sel = false;
    std::string why;
    if (!readNote(L, lua_gettop(L), x, sel, &why)) luaL_error(L, "note %d: %s", static_cast<int>(i), why.c_str());
    lua_pop(L, 1);
    out.push_back(x);
  }
  std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  return out;
}
int p_setNotes(lua_State* L) {
  ObjectId pat = patId(L, 1);
  ObjectId ch = chId(L, 2);
  std::vector<Note> notes = readNotes(L, 3);
  charge(L, kCostEdit + notes.size() * 8);
  lua_pushboolean(L, doc(L).setNotes(pat, ch, std::move(notes), "script: notes"));
  return 1;
}
int p_addNote(lua_State* L) {
  ObjectId pat = patId(L, 1);
  ObjectId ch = chId(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  Note x;
  bool sel = false;
  std::string why;
  if (!readNote(L, 3, x, sel, &why)) return luaL_argerror(L, 3, why.c_str());
  const Pattern* p = song(L).pattern(pat);
  auto it = p->notes.find(ch);
  const size_t have = it == p->notes.end() ? 0 : it->second.size();
  if (have >= PianoRollScript::kMaxNotes) return luaL_error(L, "too many notes");
  charge(L, kCostEdit + have * 2);
  lua_pushboolean(L, doc(L).addNote(pat, ch, x));
  return 1;
}
int p_channels(lua_State* L) {
  const Song& s = song(L);
  const Pattern& p = s.patterns[patIdx(L, 1)];
  charge(L, kCostRead + s.channels.size());
  lua_newtable(L);
  lua_Integer k = 1;
  for (size_t i = 0; i < s.channels.size(); ++i) {
    ObjectId id = s.channels[i].id;
    auto n = p.notes.find(id);
    auto st = p.steps.find(id);
    bool used = (n != p.notes.end() && !n->second.empty()) ||
                (st != p.steps.end() && std::any_of(st->second.begin(), st->second.end(), [](uint8_t v) { return v > 0; }));
    if (used) {
      lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
      lua_rawseti(L, -2, k++);
    }
  }
  return 1;
}

// ============================================================== playlist ====
int pl_trackCount(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).tracks.size()));
  return 1;
}
int pl_setTrackCount(lua_State* L) {
  lua_Integer n = luaL_checkinteger(L, 1);
  if (n < 1 || n > kMaxPlaylistTracks) luaL_argerror(L, 1, "1..500 tracks");
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).setPlaylistTrackCount(static_cast<int>(n)));
  return 1;
}
int pl_trackName(lua_State* L) {
  lua_pushstring(L, playlistTrackName(song(L), trackIdx(L, 1)).c_str());
  return 1;
}
int pl_setTrackName(lua_State* L) {
  int t = static_cast<int>(trackIdx(L, 1));
  std::string n = str(L, 2);
  charge(L, kCostEdit);
  doc(L).editPlaylistTrack(t, "rename playlist track", [&n](PlaylistTrack& tr) { tr.name = n; });
  return 0;
}
int pl_trackColor(lua_State* L) {
  lua_pushinteger(L, song(L).tracks[trackIdx(L, 1)].color);
  return 1;
}
int pl_setTrackColor(lua_State* L) {
  int t = static_cast<int>(trackIdx(L, 1));
  Rgb c = color(L, 2);
  charge(L, kCostEdit);
  doc(L).editPlaylistTrack(t, "playlist track colour", [c](PlaylistTrack& tr) { tr.color = c; });
  return 0;
}
int pl_trackMuted(lua_State* L) {
  lua_pushboolean(L, song(L).tracks[trackIdx(L, 1)].muted);
  return 1;
}
int pl_setTrackMuted(lua_State* L) {
  int t = static_cast<int>(trackIdx(L, 1));
  bool m = flag(L, 2);
  charge(L, kCostEdit);
  doc(L).editPlaylistTrack(t, "mute playlist track", [m](PlaylistTrack& tr) { tr.muted = m; });
  return 0;
}
const char* kindName(ClipKind k) {
  switch (k) {
    case ClipKind::Audio: return "audio";
    case ClipKind::Automation: return "automation";
    default: return "pattern";
  }
}
// Object id -> index maps, built once per call (clip lists can be long).
struct Indexes {
  std::unordered_map<ObjectId, int> pat, ch, au;
  explicit Indexes(const Song& s) {
    for (size_t i = 0; i < s.patterns.size(); ++i) pat[s.patterns[i].id] = static_cast<int>(i);
    for (size_t i = 0; i < s.channels.size(); ++i) ch[s.channels[i].id] = static_cast<int>(i);
    for (size_t i = 0; i < s.automations.size(); ++i) au[s.automations[i].id] = static_cast<int>(i);
  }
  static int find(const std::unordered_map<ObjectId, int>& m, ObjectId id) {
    auto it = m.find(id);
    return it == m.end() ? -1 : it->second;
  }
};
void pushClip(lua_State* L, const Song& s, const Indexes& ix, const Clip& c) {
  lua_createtable(L, 0, 18);
  setI(L, "id", c.id);
  setS(L, "kind", kindName(c.kind));
  setI(L, "track", c.track + 1);
  setI(L, "start", c.start);
  setI(L, "length", s.clipLength(c));
  setI(L, "offset", c.offset);
  setB(L, "muted", c.muted);
  std::string name;
  if (c.kind == ClipKind::Pattern) {
    int i = Indexes::find(ix.pat, c.pattern);
    if (i >= 0) setI(L, "pattern", i + 1), name = s.patterns[static_cast<size_t>(i)].name;
  } else if (c.kind == ClipKind::Audio) {
    int i = Indexes::find(ix.ch, c.source);
    if (i >= 0) setI(L, "channel", i + 1), name = s.channels[static_cast<size_t>(i)].name;
    setN(L, "gain", c.gain);
    setN(L, "pitch", c.pitch);
    setB(L, "reverse", c.reverse);
    setN(L, "fadeIn", c.fadeIn);
    setN(L, "fadeOut", c.fadeOut);
  } else {
    int i = Indexes::find(ix.au, c.source);
    if (i >= 0) setI(L, "automation", i + 1), name = s.automations[static_cast<size_t>(i)].name;
  }
  setS(L, "name", name);
}
int pl_clips(lua_State* L) {
  const Song& s = song(L);
  int track = -1;
  if (!lua_isnoneornil(L, 1)) track = static_cast<int>(trackIdx(L, 1));
  std::vector<const Clip*> v;
  for (const auto& c : s.playlist)
    if (track < 0 || c.track == track) v.push_back(&c);
  std::stable_sort(v.begin(), v.end(), [](const Clip* a, const Clip* b) { return a->start != b->start ? a->start < b->start : a->track < b->track; });
  charge(L, kCostRead + (s.playlist.size() + s.patterns.size() + s.channels.size() + s.automations.size()) / 4 + v.size() * 8);
  const Indexes ix(s);
  lua_createtable(L, static_cast<int>(std::min<size_t>(v.size(), 1u << 20)), 0);
  for (size_t i = 0; i < v.size(); ++i) {
    pushClip(L, s, ix, *v[i]);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}
int pl_clip(lua_State* L) {
  chargeClipScan(L);
  lua_Integer v = luaL_checkinteger(L, 1);
  const Song& s = song(L);
  const Clip* c = v > 0 && v <= 0xFFFFFFFFLL ? s.clip(static_cast<ObjectId>(v)) : nullptr;
  if (!c) {
    lua_pushnil(L);
  } else {
    charge(L, kCostRead + (s.patterns.size() + s.channels.size() + s.automations.size()) / 4);
    pushClip(L, s, Indexes(s), *c);
  }
  return 1;
}
int trackArgForNew(lua_State* L, int arg) {
  lua_Integer t = luaL_checkinteger(L, arg);
  if (t < 1 || t > kMaxPlaylistTracks) luaL_argerror(L, arg, "track 1..500");
  return static_cast<int>(t - 1);
}
void checkClipRoom(lua_State* L) {
  if (song(L).playlist.size() >= kMaxClips) luaL_error(L, "too many clips");
}
int pl_addPatternClip(lua_State* L) {
  ObjectId pat = patId(L, 1);
  int track = trackArgForNew(L, 2);
  Tick start = tick(L, 3);
  Tick len = lua_isnoneornil(L, 4) ? 0 : tick(L, 4);
  checkClipRoom(L);
  charge(L, kCostStruct);
  ObjectId id = doc(L).addClip(pat, track, start, len);
  if (id == kNoId)
    lua_pushnil(L);
  else
    lua_pushinteger(L, id);
  return 1;
}
// Audio and automation clips: one whole-song step (Document has no adder for them).
int addSourceClip(lua_State* L, ClipKind kind, ObjectId source) {
  int track = trackArgForNew(L, 2);
  Tick start = tick(L, 3);
  Tick len = lua_isnoneornil(L, 4) ? 0 : tick(L, 4);
  checkClipRoom(L);
  charge(L, kCostStruct);
  const Song& s = song(L);
  if (kind == ClipKind::Audio && len == 0) len = s.barTicks();
  ObjectId id = s.peekId();
  doc(L).editSong(kind == ClipKind::Audio ? "add audio clip" : "add automation clip", [&](Song& x) {
    Clip c;
    c.id = x.allocateId();
    c.kind = kind;
    c.source = source;
    c.track = track;
    c.start = start;
    c.length = len;
    x.playlist.push_back(c);
    if (static_cast<size_t>(track) >= x.tracks.size()) x.tracks.resize(static_cast<size_t>(track) + 1);
  });
  lua_pushinteger(L, id);
  return 1;
}
int pl_addAudioClip(lua_State* L) {
  const Channel& c = song(L).channels[chIdx(L, 1)];
  const PluginInfo* p = findPlugin(c.plugin.type);
  if (!p || !p->usesSample) luaL_argerror(L, 1, "the channel has no sample");
  return addSourceClip(L, ClipKind::Audio, c.id);
}
int pl_addAutomationClip(lua_State* L) { return addSourceClip(L, ClipKind::Automation, autoId(L, 1)); }
int pl_removeClip(lua_State* L) {
  ObjectId id = clipId(L, 1);
  charge(L, kCostEdit);
  lua_pushboolean(L, doc(L).removeClip(id));
  return 1;
}
int pl_moveClip(lua_State* L) {
  ObjectId id = clipId(L, 1);
  size_t track = trackIdx(L, 2);
  Tick start = tick(L, 3);
  charge(L, kCostEdit);
  lua_pushboolean(L, doc(L).moveClip(id, static_cast<int>(track), start));
  return 1;
}
int pl_setClipLength(lua_State* L) {
  ObjectId id = clipId(L, 1);
  Tick len = tick(L, 2);
  charge(L, kCostEdit);
  lua_pushboolean(L, doc(L).setClipLength(id, len));
  return 1;
}
int pl_setClip(lua_State* L) {
  ObjectId id = clipId(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  Clip c = *song(L).clip(id);
  auto numField = [&](const char* k, double lo, double hi, auto& out) {
    lua_getfield(L, 2, k);
    if (!lua_isnil(L, -1)) {
      if (!lua_isnumber(L, -1)) luaL_error(L, "clip.%s must be a number", k);
      double v = lua_tonumber(L, -1);
      if (!std::isfinite(v)) luaL_error(L, "clip.%s must be finite", k);
      out = static_cast<std::decay_t<decltype(out)>>(std::clamp(v, lo, hi));
    }
    lua_pop(L, 1);
  };
  numField("offset", 0, static_cast<double>(kMaxTick), c.offset);
  numField("gain", 0, 4, c.gain);
  numField("pitch", -4800, 4800, c.pitch);
  numField("fadeIn", 0, 3600, c.fadeIn);
  numField("fadeOut", 0, 3600, c.fadeOut);
  c.muted = fieldBool(L, 2, "muted", c.muted);
  c.reverse = fieldBool(L, 2, "reverse", c.reverse);
  lua_getfield(L, 2, "fadeIn");
  if (!lua_isnil(L, -1)) c.fadeInManual = true;
  lua_pop(L, 1);
  lua_getfield(L, 2, "fadeOut");
  if (!lua_isnil(L, -1)) c.fadeOutManual = true;
  lua_pop(L, 1);
  charge(L, kCostEdit);
  lua_pushboolean(L, doc(L).editClip(id, "edit clip", [&c](Clip& x) { x = c; }));
  return 1;
}

// ============================================================ automation ====
int a_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).automations.size()));
  return 1;
}
int a_id(lua_State* L) {
  lua_pushinteger(L, autoId(L, 1));
  return 1;
}
int a_name(lua_State* L) {
  lua_pushstring(L, song(L).automations[autoIdx(L, 1)].name.c_str());
  return 1;
}
int a_setName(lua_State* L) {
  ObjectId id = autoId(L, 1);
  std::string n = str(L, 2);
  charge(L, kCostEdit);
  doc(L).editAutomation(id, "rename automation clip", [&n](AutomationClip& a) { a.name = n; });
  return 0;
}
int a_target(lua_State* L) {
  const AutomationClip& a = song(L).automations[autoIdx(L, 1)];
  ParamId id{a.owner, a.index};
  if (describeParam(song(L), id))
    lua_pushinteger(L, packRef(id));
  else
    lua_pushnil(L);
  return 1;
}
int a_targetName(lua_State* L) {
  const AutomationClip& a = song(L).automations[autoIdx(L, 1)];
  lua_pushstring(L, paramDisplayName(song(L), {a.owner, a.index}).c_str());
  return 1;
}
int a_points(lua_State* L) {
  const AutomationClip& a = song(L).automations[autoIdx(L, 1)];
  charge(L, kCostRead + a.points.size() * 4);
  lua_createtable(L, static_cast<int>(std::min<size_t>(a.points.size(), 1u << 20)), 0);
  for (size_t i = 0; i < a.points.size(); ++i) {
    const AutoPoint& p = a.points[i];
    lua_createtable(L, 0, 4);
    setI(L, "time", p.pos);
    setN(L, "value", p.value);
    setN(L, "tension", p.tension);
    setI(L, "shape", static_cast<int>(p.shape));
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}
AutoPoint readPoint(lua_State* L, int t) {
  AutoPoint p;
  double time = fieldNumber(L, t, "time", 0), value = fieldNumber(L, t, "value", 0.5), tension = fieldNumber(L, t, "tension", 0);
  double shape = fieldNumber(L, t, "shape", static_cast<double>(static_cast<int>(AutoShape::Curve)));
  if (!std::isfinite(time) || !std::isfinite(value) || !std::isfinite(tension) || !std::isfinite(shape))
    luaL_error(L, "automation point values must be finite numbers");
  p.pos = static_cast<Tick>(std::clamp(std::round(time), 0.0, static_cast<double>(kMaxTick)));
  p.value = static_cast<float>(std::clamp(value, 0.0, 1.0));
  p.tension = static_cast<float>(std::clamp(tension, -1.0, 1.0));
  p.shape = static_cast<AutoShape>(static_cast<int>(std::clamp(std::round(shape), 0.0, static_cast<double>(static_cast<int>(AutoShape::Curve)))));
  return p;
}
std::vector<AutoPoint> readPoints(lua_State* L, int arg) {
  luaL_checktype(L, arg, LUA_TTABLE);
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, arg));
  if (n > 100000) luaL_argerror(L, arg, "at most 100000 points");
  std::vector<AutoPoint> v;
  v.reserve(static_cast<size_t>(n));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, arg, i);
    if (!lua_istable(L, -1)) luaL_error(L, "point %d is not a table", static_cast<int>(i));
    v.push_back(readPoint(L, lua_gettop(L)));
    lua_pop(L, 1);
  }
  std::stable_sort(v.begin(), v.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.pos < b.pos; });
  return v;
}
int a_setPoints(lua_State* L) {
  ObjectId id = autoId(L, 1);
  std::vector<AutoPoint> pts = readPoints(L, 2);
  charge(L, kCostEdit + pts.size() * 8);
  doc(L).editAutomation(id, "edit automation points", [&pts](AutomationClip& a) { a.points = pts; });
  return 0;
}
int a_addPoint(lua_State* L) {
  ObjectId id = autoId(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  AutoPoint p = readPoint(L, 2);
  const size_t have = song(L).automation(id)->points.size();
  if (have >= 100000) return luaL_error(L, "too many points");
  charge(L, kCostEdit + have);
  doc(L).editAutomation(id, "add automation point", [&p](AutomationClip& a) {
    auto it = std::upper_bound(a.points.begin(), a.points.end(), p.pos, [](Tick t, const AutoPoint& q) { return t < q.pos; });
    a.points.insert(it, p);
  });
  return 0;
}
int a_range(lua_State* L) {
  const AutomationClip& a = song(L).automations[autoIdx(L, 1)];
  lua_pushnumber(L, a.min);
  lua_pushnumber(L, a.max);
  return 2;
}
int a_add(lua_State* L) {
  ParamId target = refArg(L, 1);
  std::string name = lua_isnoneornil(L, 2) ? paramDisplayName(song(L), target) : str(L, 2);
  if (name.size() > kMaxName) name.resize(kMaxName);
  std::vector<AutoPoint> pts;
  if (!lua_isnoneornil(L, 3)) pts = readPoints(L, 3);
  if (song(L).automations.size() >= kMaxAutomation) return luaL_error(L, "too many automation clips");
  charge(L, kCostStruct + pts.size() * 8);
  doc(L).editSong("add automation clip", [&](Song& s) {
    AutomationClip a;
    a.id = s.allocateId();
    a.name = name;
    a.owner = target.owner;
    a.index = target.index;
    a.points = pts;
    s.automations.push_back(a);
  });
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).automations.size()));
  return 1;
}
int a_remove(lua_State* L) {
  ObjectId id = autoId(L, 1);
  charge(L, kCostStruct);
  doc(L).editSong("delete automation clip", [id](Song& s) {
    s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(),
                                    [id](const Clip& c) { return c.kind == ClipKind::Automation && c.source == id; }),
                     s.playlist.end());
    s.automations.erase(std::remove_if(s.automations.begin(), s.automations.end(), [id](const AutomationClip& a) { return a.id == id; }),
                        s.automations.end());
  });
  return 0;
}

// ================================================================= mixer ====
int m_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).mixer.size()));
  return 1;
}
int m_id(lua_State* L) {
  lua_pushinteger(L, mixId(L, 1));
  return 1;
}
int m_name(lua_State* L) {
  lua_pushstring(L, mixerTrackName(song(L), mixIdx(L, 1)).c_str());
  return 1;
}
int m_setName(lua_State* L) {
  ObjectId id = mixId(L, 1);
  std::string n = str(L, 2);
  charge(L, kCostEdit);
  doc(L).editMixerTrack(id, "rename mixer track", [&n](MixerTrack& t) { t.name = n; });
  return 0;
}
int m_color(lua_State* L) {
  lua_pushinteger(L, song(L).mixer[mixIdx(L, 1)].color);
  return 1;
}
int m_setColor(lua_State* L) {
  ObjectId id = mixId(L, 1);
  Rgb c = color(L, 2);
  charge(L, kCostEdit);
  doc(L).editMixerTrack(id, "mixer track colour", [c](MixerTrack& t) { t.color = c; });
  return 0;
}
int m_volume(lua_State* L) { return pushParam(L, mixerParam(mixId(L, 1), MixerParam::Volume)); }
int m_setVolume(lua_State* L) {
  setParamValue(L, mixerParam(mixId(L, 1), MixerParam::Volume), num(L, 2));
  return 0;
}
int m_pan(lua_State* L) { return pushParam(L, mixerParam(mixId(L, 1), MixerParam::Pan)); }
int m_setPan(lua_State* L) {
  setParamValue(L, mixerParam(mixId(L, 1), MixerParam::Pan), num(L, 2));
  return 0;
}
int m_muted(lua_State* L) {
  lua_pushboolean(L, song(L).mixer[mixIdx(L, 1)].muted);
  return 1;
}
int m_setMuted(lua_State* L) {
  setParamValue(L, mixerParam(mixId(L, 1), MixerParam::Mute), flag(L, 2) ? 1.0 : 0.0);
  return 0;
}
int m_solo(lua_State* L) {
  lua_pushboolean(L, song(L).mixer[mixIdx(L, 1)].solo);
  return 1;
}
int m_setSolo(lua_State* L) {
  ObjectId id = mixId(L, 1);
  bool on = flag(L, 2);
  charge(L, kCostEdit);
  doc(L).editMixerTrack(id, "solo mixer track", [on](MixerTrack& t) { t.solo = on; });
  return 0;
}
int m_param(lua_State* L) { return pushParam(L, mixerParamRef(L, mixId(L, 1), 2)); }
int m_setParam(lua_State* L) {
  ParamId id = mixerParamRef(L, mixId(L, 1), 2);
  setParamValue(L, id, num(L, 3));
  return 0;
}
int m_add(lua_State* L) {
  if (static_cast<int>(song(L).mixer.size()) > kMaxMixerInserts) return luaL_error(L, "too many mixer tracks");
  charge(L, kCostStruct);
  ObjectId id = doc(L).addMixerTrack();
  int i = song(L).mixerIndex(id);
  if (i < 0)
    lua_pushnil(L);
  else
    lua_pushinteger(L, i);
  return 1;
}
int m_remove(lua_State* L) {
  size_t i = mixIdx(L, 1);
  if (i == 0) luaL_argerror(L, 1, "the master cannot be removed");
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).removeMixerTrack(song(L).mixer[i].id));
  return 1;
}
int m_sends(lua_State* L) {
  const Song& s = song(L);
  const MixerTrack& t = s.mixer[mixIdx(L, 1)];
  charge(L, kCostRead + t.sends.size() * 4);
  lua_createtable(L, static_cast<int>(t.sends.size()), 0);
  lua_Integer k = 1;
  for (const Send& sd : t.sends) {
    int to = s.mixerIndex(sd.to);
    if (to < 0) continue;
    lua_createtable(L, 0, 3);
    setI(L, "to", to);
    setN(L, "level", sd.level);
    setB(L, "sidechain", sd.sidechain);
    lua_rawseti(L, -2, k++);
  }
  return 1;
}
int m_setSend(lua_State* L) {
  size_t from = mixIdx(L, 1), to = mixIdx(L, 2);
  double level = num(L, 3);
  const Song& s = song(L);
  ObjectId a = s.mixer[from].id, b = s.mixer[to].id;
  charge(L, kCostEdit);
  lua_pushboolean(L, from != to && doc(L).setSend(a, b, static_cast<float>(std::clamp(level, 0.0, 1.25))));
  return 1;
}
// Sends only to `to` (level 1): removes the track's other sends.
int m_routeTo(lua_State* L) {
  size_t from = mixIdx(L, 1), to = mixIdx(L, 2);
  if (from == 0) luaL_argerror(L, 1, "the master has no sends");
  if (from == to) luaL_argerror(L, 2, "a track cannot send to itself");
  const Song& s = song(L);
  ObjectId a = s.mixer[from].id, b = s.mixer[to].id;
  if (!canRoute(s, a, b)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  std::vector<ObjectId> old;
  for (const Send& sd : s.mixer[from].sends)
    if (sd.to != b) old.push_back(sd.to);
  charge(L, kCostEdit * (old.size() + 1));
  bool ok = doc(L).setSend(a, b, 1.0f);
  for (ObjectId o : old) doc(L).setSend(a, o, 0.0f);
  lua_pushboolean(L, ok);
  return 1;
}
int m_setSidechain(lua_State* L) {
  size_t from = mixIdx(L, 1), to = mixIdx(L, 2);
  bool on = flag(L, 3);
  const Song& s = song(L);
  ObjectId a = s.mixer[from].id, b = s.mixer[to].id;
  charge(L, kCostEdit);
  lua_pushboolean(L, doc(L).setSendSidechain(a, b, on));
  return 1;
}
int m_channels(lua_State* L) {
  const Song& s = song(L);
  ObjectId id = s.mixer[mixIdx(L, 1)].id;
  const bool master = id == s.masterId();
  charge(L, kCostRead + s.channels.size());
  lua_newtable(L);
  lua_Integer k = 1;
  for (size_t i = 0; i < s.channels.size(); ++i)
    if (s.channels[i].mixerTrack == id || (master && s.channels[i].mixerTrack == kNoId)) {
      lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
      lua_rawseti(L, -2, k++);
    }
  return 1;
}
int m_slotCount(lua_State* L) {
  lua_pushinteger(L, kEffectSlots);
  return 1;
}
int m_slot(lua_State* L) {
  const MixerTrack& t = song(L).mixer[mixIdx(L, 1)];
  int k = slotIdx(L, 2);
  if (static_cast<size_t>(k) >= t.slots.size() || t.slots[static_cast<size_t>(k)].plugin.empty()) {
    lua_pushnil(L);
    return 1;
  }
  const EffectSlot& sl = t.slots[static_cast<size_t>(k)];
  const PluginInfo* p = findPlugin(sl.plugin.type);
  lua_createtable(L, 0, 5);
  setS(L, "type", sl.plugin.type);
  setS(L, "name", p ? p->name : sl.plugin.type);
  setB(L, "enabled", sl.enabled);
  setN(L, "mix", sl.mix);
  setI(L, "params", static_cast<lua_Integer>(sl.plugin.params.size()));
  return 1;
}
int m_setSlot(lua_State* L) {
  ObjectId id = mixId(L, 1);
  int k = slotIdx(L, 2);
  std::string type = lua_isnoneornil(L, 3) ? std::string() : str(L, 3);
  if (!type.empty()) {
    const PluginInfo* p = findPlugin(type);
    if (!p || p->kind != PluginKind::Effect) luaL_argerror(L, 3, "not an effect type");
  }
  charge(L, kCostStruct);
  lua_pushboolean(L, doc(L).setSlotPlugin(id, k, type));
  return 1;
}
const EffectSlot& filledSlot(lua_State* L, size_t m, int k) {
  const MixerTrack& t = song(L).mixer[m];
  if (static_cast<size_t>(k) >= t.slots.size() || t.slots[static_cast<size_t>(k)].plugin.empty()) luaL_argerror(L, 2, "the slot is empty");
  return t.slots[static_cast<size_t>(k)];
}
int m_setSlotEnabled(lua_State* L) {
  size_t m = mixIdx(L, 1);
  int k = slotIdx(L, 2);
  filledSlot(L, m, k);
  setParamValue(L, slotEnableParam(song(L).mixer[m].id, k), flag(L, 3) ? 1.0 : 0.0);
  return 0;
}
int m_setSlotMix(lua_State* L) {
  size_t m = mixIdx(L, 1);
  int k = slotIdx(L, 2);
  filledSlot(L, m, k);
  setParamValue(L, slotMixParam(song(L).mixer[m].id, k), num(L, 3));
  return 0;
}
int m_slotParamCount(lua_State* L) {
  size_t m = mixIdx(L, 1);
  int k = slotIdx(L, 2);
  const MixerTrack& t = song(L).mixer[m];
  lua_pushinteger(L, static_cast<size_t>(k) < t.slots.size() ? static_cast<lua_Integer>(t.slots[static_cast<size_t>(k)].plugin.params.size()) : 0);
  return 1;
}
ParamId slotRef(lua_State* L) {
  size_t m = mixIdx(L, 1);
  int k = slotIdx(L, 2);
  filledSlot(L, m, k);
  return slotParamRef(L, song(L).mixer[m].id, k, 3);
}
int m_slotParamName(lua_State* L) { return pushParamInfo(L, slotRef(L), 0); }
int m_slotParamRange(lua_State* L) { return pushParamInfo(L, slotRef(L), 1); }
int m_slotParamText(lua_State* L) { return pushParamInfo(L, slotRef(L), 2); }
int m_slotParam(lua_State* L) { return pushParam(L, slotRef(L)); }
int m_setSlotParam(lua_State* L) {
  ParamId id = slotRef(L);
  setParamValue(L, id, num(L, 4));
  return 0;
}

// ================================================================ params ====
int pr_song(lua_State* L) {
  std::string w = luaL_checkstring(L, 1);
  SongParam p = SongParam::Tempo;
  if (w == "tempo")
    p = SongParam::Tempo;
  else if (w == "swing")
    p = SongParam::Swing;
  else if (w == "masterPitch")
    p = SongParam::MasterPitch;
  else
    luaL_argerror(L, 1, "tempo, swing or masterPitch");
  lua_pushinteger(L, packRef(songParam(p)));
  return 1;
}
int pr_channel(lua_State* L) {
  lua_pushinteger(L, packRef(channelParamRef(L, chId(L, 1), 2)));
  return 1;
}
int pr_mixer(lua_State* L) {
  lua_pushinteger(L, packRef(mixerParamRef(L, mixId(L, 1), 2)));
  return 1;
}
int pr_send(lua_State* L) {
  ObjectId from = mixId(L, 1);
  size_t to = mixIdx(L, 2);
  ParamId id = sendParam(from, static_cast<int>(to));
  checkParamValid(L, id, 2);  // only existing sends resolve
  lua_pushinteger(L, packRef(id));
  return 1;
}
int pr_slot(lua_State* L) {
  lua_pushinteger(L, packRef(slotRef(L)));
  return 1;
}
int pr_slotMix(lua_State* L) {
  size_t m = mixIdx(L, 1);
  int k = slotIdx(L, 2);
  filledSlot(L, m, k);
  lua_pushinteger(L, packRef(slotMixParam(song(L).mixer[m].id, k)));
  return 1;
}
int pr_get(lua_State* L) { return pushParam(L, refArg(L, 1)); }
int pr_set(lua_State* L) {
  ParamId id = refArg(L, 1);
  setParamValue(L, id, num(L, 2));
  return 0;
}
int pr_name(lua_State* L) {
  ParamId id = refArg(L, 1);
  lua_pushstring(L, paramDisplayName(song(L), id).c_str());
  return 1;
}
int pr_range(lua_State* L) { return pushParamInfo(L, refArg(L, 1), 1); }
int pr_text(lua_State* L) { return pushParamInfo(L, refArg(L, 1), 2); }
int pr_normalized(lua_State* L) {
  ParamId id = refArg(L, 1);
  auto d = describeParam(song(L), id);
  auto v = getParam(song(L), id);
  lua_pushnumber(L, d && v ? d->toNormalized(*v) : 0.0);
  return 1;
}
int pr_toNormalized(lua_State* L) {
  ParamId id = refArg(L, 1);
  lua_pushnumber(L, describeParam(song(L), id)->toNormalized(num(L, 2)));
  return 1;
}
int pr_fromNormalized(lua_State* L) {
  ParamId id = refArg(L, 1);
  lua_pushnumber(L, describeParam(song(L), id)->fromNormalized(std::clamp(num(L, 2), 0.0, 1.0)));
  return 1;
}
int pr_setNormalized(lua_State* L) {
  ParamId id = refArg(L, 1);
  double n = std::clamp(num(L, 2), 0.0, 1.0);
  setParamValue(L, id, describeParam(song(L), id)->fromNormalized(n));
  return 0;
}

// =============================================================== markers ====
void pushMarker(lua_State* L, const Marker& m) {
  lua_createtable(L, 0, 5);
  setI(L, "time", m.pos);
  setS(L, "name", m.name);
  setI(L, "tsNum", m.timeSigNum);
  setI(L, "tsDen", m.timeSigDen);
  setI(L, "action", static_cast<int>(m.action));
}
Marker readMarker(lua_State* L, int t, Marker m) {
  auto get = [&](const char* k, double lo, double hi, double def) {
    double v = fieldNumber(L, t, k, def);
    if (!std::isfinite(v)) luaL_error(L, "marker.%s must be a finite number", k);
    return std::clamp(std::round(v), lo, hi);
  };
  m.pos = static_cast<Tick>(get("time", 0, static_cast<double>(kMaxTick), static_cast<double>(m.pos)));
  m.name = fieldString(L, t, "name", m.name);
  if (m.name.size() > kMaxName) m.name.resize(kMaxName);
  m.timeSigNum = static_cast<int>(get("tsNum", 0, 16, m.timeSigNum));
  m.timeSigDen = static_cast<int>(get("tsDen", 0, 16, m.timeSigDen));
  m.action = static_cast<MarkerAction>(static_cast<int>(get("action", 0, static_cast<int>(MarkerAction::Count) - 1, static_cast<int>(m.action))));
  return m;
}
void sortMarkers(std::vector<Marker>& v) {
  std::stable_sort(v.begin(), v.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
}
int mk_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).markers.size()));
  return 1;
}
int mk_get(lua_State* L) {
  pushMarker(L, song(L).markers[markerIdx(L, 1)]);
  return 1;
}
int mk_add(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Marker m = readMarker(L, 1, Marker{});
  if (song(L).markers.size() >= kMaxMarkers) return luaL_error(L, "too many markers");
  charge(L, kCostStruct);
  doc(L).editSong("add marker", [&m](Song& s) {
    s.markers.push_back(m);
    sortMarkers(s.markers);
  });
  const auto& v = song(L).markers;
  int at = -1;
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i] == m) at = static_cast<int>(i);
  pushIndexOrNil(L, at);
  return 1;
}
int mk_set(lua_State* L) {
  size_t i = markerIdx(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  Marker m = readMarker(L, 2, song(L).markers[i]);
  charge(L, kCostStruct);
  doc(L).editSong("edit marker", [&m, i](Song& s) {
    s.markers[i] = m;
    sortMarkers(s.markers);
  });
  return 0;
}
int mk_remove(lua_State* L) {
  size_t i = markerIdx(L, 1);
  charge(L, kCostStruct);
  doc(L).editSong("delete marker", [i](Song& s) { s.markers.erase(s.markers.begin() + static_cast<long>(i)); });
  return 0;
}

// ============================================================= selection ====
int se_channels(lua_State* L) {
  const Song& s = song(L);
  std::vector<int> idx;
  for (ObjectId id : host(L).selectedChannels()) {
    int i = s.channelIndex(id);
    if (i >= 0) idx.push_back(i);
  }
  std::sort(idx.begin(), idx.end());
  idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
  charge(L, kCostRead + idx.size());
  lua_createtable(L, static_cast<int>(idx.size()), 0);
  for (size_t k = 0; k < idx.size(); ++k) {
    lua_pushinteger(L, idx[k] + 1);
    lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
  }
  return 1;
}
int se_setChannels(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  const Song& s = song(L);
  const lua_Integer n = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(L, 1)), 100000);
  std::vector<ObjectId> ids;
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, 1, i);
    lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    if (v < 1 || static_cast<size_t>(v) > s.channels.size()) luaL_error(L, "selection entry %d is not a channel index", static_cast<int>(i));
    ids.push_back(s.channels[static_cast<size_t>(v - 1)].id);
  }
  charge(L, kCostRead + ids.size());
  host(L).selectChannels(ids);
  return 0;
}
int se_clips(lua_State* L) {
  const Song& s = song(L);
  std::vector<ObjectId> ids;
  chargeClipScan(L, host(L).selectedClips().size());
  for (ObjectId id : host(L).selectedClips())
    if (s.clip(id)) ids.push_back(id);
  charge(L, kCostRead + ids.size());
  lua_createtable(L, static_cast<int>(ids.size()), 0);
  for (size_t k = 0; k < ids.size(); ++k) {
    lua_pushinteger(L, ids[k]);
    lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
  }
  return 1;
}
int se_setClips(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  const lua_Integer n = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(L, 1)), static_cast<lua_Integer>(kMaxClips));
  std::vector<ObjectId> ids;
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, 1, i);
    lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    chargeClipScan(L);
    if (v <= 0 || v > 0xFFFFFFFFLL || !song(L).clip(static_cast<ObjectId>(v))) luaL_error(L, "selection entry %d is not a clip id", static_cast<int>(i));
    ids.push_back(static_cast<ObjectId>(v));
  }
  charge(L, kCostRead + ids.size() * 4);
  host(L).selectClips(ids);
  return 0;
}
int se_pattern(lua_State* L) {
  pushIndexOrNil(L, song(L).patternIndex(host(L).currentPattern()));
  return 1;
}
int se_setPattern(lua_State* L) {
  host(L).setCurrentPattern(patId(L, 1));
  return 0;
}
int se_mixer(lua_State* L) {
  lua_pushinteger(L, std::max(0, song(L).mixerIndex(host(L).selectedMixer())));
  return 1;
}
int se_setMixer(lua_State* L) {
  host(L).selectMixer(mixId(L, 1));
  return 0;
}
int se_time(lua_State* L) {
  auto [a, b] = host(L).timeSelection();
  if (b <= a) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, a);
  lua_pushinteger(L, b);
  return 2;
}
int se_setTime(lua_State* L) {
  Tick a = tick(L, 1), b = tick(L, 2);
  host(L).setTimeSelection(a, b);
  return 0;
}

// ============================================================= transport ====
void needTransport(lua_State* L) {
  if (!host(L).allowTransport) luaL_error(L, "this script may not control the transport (Settings: allow scripts to control the transport)");
}
int t_playing(lua_State* L) {
  lua_pushboolean(L, host(L).playing());
  return 1;
}
int t_recording(lua_State* L) {
  lua_pushboolean(L, host(L).recording());
  return 1;
}
int t_songMode(lua_State* L) {
  lua_pushboolean(L, host(L).songMode());
  return 1;
}
int t_position(lua_State* L) {
  lua_pushinteger(L, host(L).position());
  return 1;
}
int t_allowed(lua_State* L) {
  lua_pushboolean(L, host(L).allowTransport);
  return 1;
}
int t_play(lua_State* L) {
  needTransport(L);
  host(L).play(true);
  return 0;
}
int t_stop(lua_State* L) {
  needTransport(L);
  host(L).play(false);
  return 0;
}
int t_setPosition(lua_State* L) {
  Tick t = tick(L, 1);
  needTransport(L);
  host(L).setPosition(t);
  return 0;
}

// Every module table is read-only for scripts (a proxy with a locked
// metatable), so one script cannot replace a function another part relies on.
void module(lua_State* L, const char* name, const luaL_Reg* fns) {
  lua_newtable(L);  // proxy
  lua_newtable(L);  // metatable
  lua_newtable(L);  // functions
  luaL_setfuncs(L, fns, 0);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, [](lua_State* S) -> int { return luaL_error(S, "project modules are read-only"); });
  lua_setfield(L, -2, "__newindex");
  lua_pushstring(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, name);
}

}  // namespace

void openProjectApi(Vm& vm, ProjectHost& h) {
  lua_State* L = vm.L();
  if (!L) return;
  lua_pushlightuserdata(L, &h);
  lua_setfield(L, LUA_REGISTRYINDEX, kHostKey);
  static const luaL_Reg songFns[] = {{"tempo", s_tempo},
                                     {"setTempo", s_setTempo},
                                     {"ppq", s_ppq},
                                     {"timeSignature", s_timeSig},
                                     {"setTimeSignature", s_setTimeSig},
                                     {"swing", s_swing},
                                     {"setSwing", s_setSwing},
                                     {"masterPitch", s_masterPitch},
                                     {"setMasterPitch", s_setMasterPitch},
                                     {"bar", s_bar},
                                     {"beat", s_beat},
                                     {"length", s_length},
                                     {"info", s_info},
                                     {"setInfo", s_setInfo},
                                     {nullptr, nullptr}};
  static const luaL_Reg chFns[] = {{"count", c_count},
                                   {"id", c_id},
                                   {"index", c_index},
                                   {"find", c_find},
                                   {"name", c_name},
                                   {"setName", c_setName},
                                   {"color", c_color},
                                   {"setColor", c_setColor},
                                   {"volume", c_volume},
                                   {"setVolume", c_setVolume},
                                   {"pan", c_pan},
                                   {"setPan", c_setPan},
                                   {"muted", c_muted},
                                   {"setMuted", c_setMuted},
                                   {"pitch", c_pitch},
                                   {"setPitch", c_setPitch},
                                   {"plugin", c_plugin},
                                   {"pluginName", c_pluginName},
                                   {"setPlugin", c_setPlugin},
                                   {"sample", c_sample},
                                   {"route", c_route},
                                   {"setRoute", c_setRoute},
                                   {"add", c_add},
                                   {"remove", c_remove},
                                   {"clone", c_clone},
                                   {"move", c_move},
                                   {"paramCount", c_paramCount},
                                   {"paramName", c_paramName},
                                   {"paramRange", c_paramRange},
                                   {"paramText", c_paramText},
                                   {"param", c_param},
                                   {"setParam", c_setParam},
                                   {nullptr, nullptr}};
  static const luaL_Reg patFns[] = {{"count", p_count},     {"id", p_id},           {"index", p_index},
                                    {"find", p_find},       {"name", p_name},       {"setName", p_setName},
                                    {"color", p_color},     {"setColor", p_setColor}, {"add", p_add},
                                    {"remove", p_remove},   {"clone", p_clone},     {"length", p_length},
                                    {"bars", p_bars},       {"setBars", p_setBars}, {"steps", p_steps},
                                    {"setSteps", p_setSteps}, {"notes", p_notes},   {"setNotes", p_setNotes},
                                    {"addNote", p_addNote}, {"channels", p_channels}, {nullptr, nullptr}};
  static const luaL_Reg plFns[] = {{"trackCount", pl_trackCount},
                                   {"setTrackCount", pl_setTrackCount},
                                   {"trackName", pl_trackName},
                                   {"setTrackName", pl_setTrackName},
                                   {"trackColor", pl_trackColor},
                                   {"setTrackColor", pl_setTrackColor},
                                   {"trackMuted", pl_trackMuted},
                                   {"setTrackMuted", pl_setTrackMuted},
                                   {"clips", pl_clips},
                                   {"clip", pl_clip},
                                   {"addPatternClip", pl_addPatternClip},
                                   {"addAudioClip", pl_addAudioClip},
                                   {"addAutomationClip", pl_addAutomationClip},
                                   {"removeClip", pl_removeClip},
                                   {"moveClip", pl_moveClip},
                                   {"setClipLength", pl_setClipLength},
                                   {"setClip", pl_setClip},
                                   {nullptr, nullptr}};
  static const luaL_Reg autoFns[] = {{"count", a_count},         {"id", a_id},         {"name", a_name},
                                     {"setName", a_setName},     {"target", a_target}, {"targetName", a_targetName},
                                     {"points", a_points},       {"setPoints", a_setPoints}, {"addPoint", a_addPoint},
                                     {"range", a_range},         {"add", a_add},       {"remove", a_remove},
                                     {nullptr, nullptr}};
  static const luaL_Reg mixFns[] = {{"count", m_count},
                                    {"id", m_id},
                                    {"name", m_name},
                                    {"setName", m_setName},
                                    {"color", m_color},
                                    {"setColor", m_setColor},
                                    {"volume", m_volume},
                                    {"setVolume", m_setVolume},
                                    {"pan", m_pan},
                                    {"setPan", m_setPan},
                                    {"muted", m_muted},
                                    {"setMuted", m_setMuted},
                                    {"solo", m_solo},
                                    {"setSolo", m_setSolo},
                                    {"param", m_param},
                                    {"setParam", m_setParam},
                                    {"add", m_add},
                                    {"remove", m_remove},
                                    {"sends", m_sends},
                                    {"setSend", m_setSend},
                                    {"routeTo", m_routeTo},
                                    {"setSidechain", m_setSidechain},
                                    {"channels", m_channels},
                                    {"slotCount", m_slotCount},
                                    {"slot", m_slot},
                                    {"setSlot", m_setSlot},
                                    {"setSlotEnabled", m_setSlotEnabled},
                                    {"setSlotMix", m_setSlotMix},
                                    {"slotParamCount", m_slotParamCount},
                                    {"slotParamName", m_slotParamName},
                                    {"slotParamRange", m_slotParamRange},
                                    {"slotParamText", m_slotParamText},
                                    {"slotParam", m_slotParam},
                                    {"setSlotParam", m_setSlotParam},
                                    {nullptr, nullptr}};
  static const luaL_Reg parFns[] = {{"song", pr_song},     {"channel", pr_channel},   {"mixer", pr_mixer},
                                    {"send", pr_send},     {"slot", pr_slot},         {"slotMix", pr_slotMix},
                                    {"get", pr_get},       {"set", pr_set},           {"name", pr_name},
                                    {"range", pr_range},   {"text", pr_text},         {"normalized", pr_normalized},
                                    {"setNormalized", pr_setNormalized}, {"toNormalized", pr_toNormalized},
                                    {"fromNormalized", pr_fromNormalized}, {nullptr, nullptr}};
  static const luaL_Reg mkFns[] = {{"count", mk_count}, {"get", mk_get}, {"add", mk_add}, {"set", mk_set}, {"remove", mk_remove}, {nullptr, nullptr}};
  static const luaL_Reg selFns[] = {{"channels", se_channels}, {"setChannels", se_setChannels}, {"clips", se_clips},
                                    {"setClips", se_setClips},   {"pattern", se_pattern},         {"setPattern", se_setPattern},
                                    {"mixer", se_mixer},         {"setMixer", se_setMixer},       {"time", se_time},
                                    {"setTime", se_setTime},     {nullptr, nullptr}};
  static const luaL_Reg trFns[] = {{"isPlaying", t_playing}, {"isRecording", t_recording}, {"songMode", t_songMode},
                                   {"position", t_position}, {"allowed", t_allowed},       {"play", t_play},
                                   {"stop", t_stop},         {"setPosition", t_setPosition}, {nullptr, nullptr}};
  lua_newtable(L);
  module(L, "song", songFns);
  module(L, "channels", chFns);
  module(L, "patterns", patFns);
  module(L, "playlist", plFns);
  module(L, "automation", autoFns);
  module(L, "mixer", mixFns);
  module(L, "params", parFns);
  module(L, "markers", mkFns);
  module(L, "selection", selFns);
  module(L, "transport", trFns);
  // `project` itself is read-only too.
  lua_newtable(L);  // proxy
  lua_insert(L, -2);
  lua_newtable(L);  // metatable
  lua_insert(L, -2);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, [](lua_State* S) -> int { return luaL_error(S, "project modules are read-only"); });
  lua_setfield(L, -2, "__newindex");
  lua_pushstring(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_setmetatable(L, -2);
  lua_setglobal(L, "project");
}

ProjectRunResult runProject(Vm& vm, ProjectHost& h, const std::string& code, const std::string& chunk, const std::string& label) {
  ProjectRunResult r;
  const size_t logBefore = vm.log().size();
  const size_t histBefore = h.doc.history().position();
  const uint64_t serialBefore = h.doc.history().stateSerial();
  h.doc.endGesture();
  h.doc.beginGroup(label);
  r.ok = vm.exec(code, chunk, &r.error);
  if (r.ok) {
    h.doc.endGroup();
  } else {
    h.doc.cancelGroup();
    r.limitHit = !vm.abortReason().empty();
  }
  r.changed = h.doc.history().stateSerial() != serialBefore || h.doc.history().position() != histBefore;
  const auto& log = vm.log();
  for (size_t i = std::min(logBefore, log.size()); i < log.size(); ++i) r.log.push_back(log[i]);
  return r;
}

bool ProjectScript::loadFile(const std::string& path, std::string* err) {
  std::ifstream is(path, std::ios::binary);
  if (!is) {
    if (err) *err = "cannot read " + path;
    return false;
  }
  std::stringstream ss;
  ss << is.rdbuf();
  if (!loadSource(ss.str(), fs::path(path).stem().string(), err)) return false;
  path_ = path;
  return true;
}

bool ProjectScript::loadSource(const std::string& source, const std::string& name, std::string* err) {
  source_.clear();
  path_.clear();
  header_ = parseHeader(source);
  name_ = header_.get("name", name);
  form_ = Form{};
  Limits lim;
  lim.instructions = 2'000'000;
  lim.seconds = 1.0;
  // The form is built against a throwaway empty project, so top-level code
  // may look modules up (local ch = project.channels); the edits belong in
  // apply().
  Document scratch(makeEmptySong());
  SimpleProjectHost dummy(scratch);
  Vm vm(lim);
  if (!vm.ok() || !vm.exec(formPrelude(), "form", err)) return false;
  openProjectApi(vm, dummy);
  if (!vm.exec(source, name_, err)) return false;
  if (!buildForm(vm, form_, err)) return false;
  if (form_.description.empty()) form_.description = header_.get("description");
  source_ = source;
  return true;
}

ProjectRunResult ProjectScript::run(ProjectHost& h, const std::vector<FormParam>& values, const Limits& limits,
                                    const std::string& fileRoot, uint32_t seed) const {
  ProjectRunResult r;
  if (source_.empty()) {
    r.error = "no script loaded";
    return r;
  }
  Vm vm(limits, fileRoot, false);
  lua_State* L = vm.L();
  if (!vm.ok() || !vm.exec(formPrelude(), "form", &r.error)) return r;
  openProjectApi(vm, h);
  lua_getglobal(L, "math");
  lua_getfield(L, -1, "randomseed");
  lua_pushinteger(L, static_cast<lua_Integer>(seed));
  lua_call(L, 1, 0);
  lua_pop(L, 1);
  const std::string label = "script: " + name_;
  const uint64_t serialBefore = h.doc.history().stateSerial();
  const size_t posBefore = h.doc.history().position();
  h.doc.endGesture();
  h.doc.beginGroup(label);
  bool ok = vm.exec(source_, name_, &r.error);
  if (ok && vm.hasFunction("apply"))
    ok = vm.protect(
        [&](lua_State* S) {
          lua_getglobal(S, "apply");
          pushFormValues(S, form_, values);
          lua_call(S, 1, 0);
        },
        &r.error);
  if (ok) {
    h.doc.endGroup();
  } else {
    h.doc.cancelGroup();
    r.limitHit = !vm.abortReason().empty();
  }
  r.ok = ok;
  r.changed = h.doc.history().stateSerial() != serialBefore || h.doc.history().position() != posBefore;
  r.log = vm.log();
  return r;
}

}  // namespace gl::script

// Gridline — controller scripts (see ControllerScript.h).
#include "ControllerScript.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include "MidiTemplates.h"
#include "Params.h"
#include "lua/src/lauxlib.h"
#include "lua/src/lua.h"

namespace gl::script {

// -------------------------------------------------------------- SimpleHost ----

void SimpleHost::action(RemoteAction a) {
  actions.push_back(a);
  switch (a) {
    case RemoteAction::PlayPause: isPlaying = !isPlaying; break;
    case RemoteAction::Stop:
      isPlaying = false;
      pos = 0;
      break;
    case RemoteAction::Record: isRecording = !isRecording; break;
    case RemoteAction::SongMode: isSongMode = !isSongMode; break;
    case RemoteAction::Undo: doc.undo(); break;
    case RemoteAction::Redo: doc.redo(); break;
    default: break;
  }
}
bool SimpleHost::armed(int index) const { return std::find(armedTracks.begin(), armedTracks.end(), index) != armedTracks.end(); }
void SimpleHost::setArmed(int index, bool on) {
  armedTracks.erase(std::remove(armedTracks.begin(), armedTracks.end(), index), armedTracks.end());
  if (on) armedTracks.push_back(index);
}

namespace {

ControllerHost& host(lua_State* L) { return *static_cast<ControllerHost*>(Vm::from(L)->user); }
const Song& song(lua_State* L) { return host(L).doc.song(); }

// Normalized (0..1) value of a parameter; 0 when it does not resolve.
double normGet(const Song& s, ParamId pid) {
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  return d && v ? d->toNormalized(*v) : 0.0;
}
void normSet(Document& doc, ParamId pid, double v) {
  auto d = describeParam(doc.song(), pid);
  if (!d || !std::isfinite(v)) return;
  doc.setParam(pid, d->fromNormalized(std::clamp(v, 0.0, 1.0)), true);
}

// ---- transport ----
int t_play(lua_State* L) {
  if (!host(L).playing()) host(L).action(RemoteAction::PlayPause);
  return 0;
}
int t_stop(lua_State* L) {
  host(L).action(RemoteAction::Stop);
  return 0;
}
int t_toggle(lua_State* L) {
  host(L).action(RemoteAction::PlayPause);
  return 0;
}
int t_record(lua_State* L) {
  host(L).action(RemoteAction::Record);
  return 0;
}
int t_isPlaying(lua_State* L) {
  lua_pushboolean(L, host(L).playing());
  return 1;
}
int t_isRecording(lua_State* L) {
  lua_pushboolean(L, host(L).recording());
  return 1;
}
int t_songMode(lua_State* L) {
  lua_pushboolean(L, host(L).songMode());
  return 1;
}
int t_setSongMode(lua_State* L) {
  bool want = lua_toboolean(L, 1) != 0;
  if (host(L).songMode() != want) host(L).action(RemoteAction::SongMode);
  return 0;
}
int t_tempo(lua_State* L) {
  lua_pushnumber(L, song(L).tempo);
  return 1;
}
int t_setTempo(lua_State* L) {
  double bpm = luaL_checknumber(L, 1);
  if (std::isfinite(bpm)) host(L).doc.setParam(songParam(SongParam::Tempo), std::clamp(bpm, 10.0, 522.0), true);
  return 0;
}
int t_position(lua_State* L) {
  lua_pushinteger(L, host(L).position());
  return 1;
}
int t_setPosition(lua_State* L) {
  host(L).setPosition(std::max<lua_Integer>(0, luaL_checkinteger(L, 1)));
  return 0;
}
int t_ppq(lua_State* L) {
  lua_pushinteger(L, song(L).ppq);
  return 1;
}
int t_action(lua_State* L) {
  std::string n = luaL_checkstring(L, 1);
  for (int i = 1; i < static_cast<int>(RemoteAction::Count); ++i)
    if (n == templateActionKey(static_cast<RemoteAction>(i))) {
      host(L).action(static_cast<RemoteAction>(i));
      lua_pushboolean(L, 1);
      return 1;
    }
  return luaL_argerror(L, 1, "unknown action");
}

// ---- mixer ----
const MixerTrack& mixerArg(lua_State* L, int arg) {
  const Song& s = song(L);
  lua_Integer i = luaL_checkinteger(L, arg);
  if (i < 0 || static_cast<size_t>(i) >= s.mixer.size()) luaL_argerror(L, arg, "no such mixer track");
  return s.mixer[static_cast<size_t>(i)];
}
int m_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).mixer.size()));
  return 1;
}
int m_name(lua_State* L) {
  lua_Integer i = luaL_checkinteger(L, 1);
  mixerArg(L, 1);
  lua_pushstring(L, mixerTrackName(song(L), static_cast<size_t>(i)).c_str());
  return 1;
}
int m_volume(lua_State* L) {
  lua_pushnumber(L, normGet(song(L), mixerParam(mixerArg(L, 1).id, MixerParam::Volume)));
  return 1;
}
int m_setVolume(lua_State* L) {
  normSet(host(L).doc, mixerParam(mixerArg(L, 1).id, MixerParam::Volume), luaL_checknumber(L, 2));
  return 0;
}
int m_pan(lua_State* L) {
  lua_pushnumber(L, normGet(song(L), mixerParam(mixerArg(L, 1).id, MixerParam::Pan)));
  return 1;
}
int m_setPan(lua_State* L) {
  normSet(host(L).doc, mixerParam(mixerArg(L, 1).id, MixerParam::Pan), luaL_checknumber(L, 2));
  return 0;
}
int m_muted(lua_State* L) {
  lua_pushboolean(L, mixerArg(L, 1).muted);
  return 1;
}
int m_setMuted(lua_State* L) {
  ObjectId id = mixerArg(L, 1).id;
  host(L).doc.setParam(mixerParam(id, MixerParam::Mute), lua_toboolean(L, 2) ? 1.0 : 0.0, false);
  return 0;
}
int m_solo(lua_State* L) {
  lua_pushboolean(L, mixerArg(L, 1).solo);
  return 1;
}
int m_setSolo(lua_State* L) {
  ObjectId id = mixerArg(L, 1).id;
  bool on = lua_toboolean(L, 2) != 0;
  if (mixerArg(L, 1).solo != on) host(L).doc.editMixerTrack(id, on ? "solo track" : "unsolo track", [on](MixerTrack& t) { t.solo = on; });
  return 0;
}
int m_armed(lua_State* L) {
  mixerArg(L, 1);
  lua_pushboolean(L, host(L).armed(static_cast<int>(lua_tointeger(L, 1))));
  return 1;
}
int m_setArmed(lua_State* L) {
  mixerArg(L, 1);
  host(L).setArmed(static_cast<int>(lua_tointeger(L, 1)), lua_toboolean(L, 2) != 0);
  return 0;
}
int m_selected(lua_State* L) {
  lua_pushinteger(L, host(L).selectedMixer());
  return 1;
}
int m_select(lua_State* L) {
  mixerArg(L, 1);
  host(L).selectMixer(static_cast<int>(lua_tointeger(L, 1)));
  return 0;
}

// ---- channels (1-based rack positions) ----
const Channel& channelArg(lua_State* L, int arg) {
  const Song& s = song(L);
  lua_Integer i = luaL_checkinteger(L, arg);
  if (i < 1 || static_cast<size_t>(i) > s.channels.size()) luaL_argerror(L, arg, "no such channel");
  return s.channels[static_cast<size_t>(i - 1)];
}
int c_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(song(L).channels.size()));
  return 1;
}
int c_name(lua_State* L) {
  lua_pushstring(L, channelArg(L, 1).name.c_str());
  return 1;
}
int c_selected(lua_State* L) {
  int i = host(L).selectedChannel();
  if (i < 0)
    lua_pushnil(L);
  else
    lua_pushinteger(L, i + 1);
  return 1;
}
int c_select(lua_State* L) {
  channelArg(L, 1);
  host(L).selectChannel(static_cast<int>(lua_tointeger(L, 1)) - 1);
  return 0;
}
int c_volume(lua_State* L) {
  lua_pushnumber(L, normGet(song(L), channelParam(channelArg(L, 1).id, ChannelParam::Volume)));
  return 1;
}
int c_setVolume(lua_State* L) {
  normSet(host(L).doc, channelParam(channelArg(L, 1).id, ChannelParam::Volume), luaL_checknumber(L, 2));
  return 0;
}
int c_pan(lua_State* L) {
  lua_pushnumber(L, normGet(song(L), channelParam(channelArg(L, 1).id, ChannelParam::Pan)));
  return 1;
}
int c_setPan(lua_State* L) {
  normSet(host(L).doc, channelParam(channelArg(L, 1).id, ChannelParam::Pan), luaL_checknumber(L, 2));
  return 0;
}
int c_muted(lua_State* L) {
  lua_pushboolean(L, channelArg(L, 1).muted);
  return 1;
}
int c_setMuted(lua_State* L) {
  ObjectId id = channelArg(L, 1).id;
  host(L).doc.setParam(channelParam(id, ChannelParam::Mute), lua_toboolean(L, 2) ? 1.0 : 0.0, false);
  return 0;
}

// ---- focused plugin ----
const PluginState* focusedState(lua_State* L, std::pair<ObjectId, int>* where = nullptr) {
  auto f = host(L).focusedPlugin();
  if (where) *where = f;
  const Song& s = song(L);
  if (f.first == kNoId) return nullptr;
  if (f.second < 0) {
    const Channel* c = s.channel(f.first);
    return c ? &c->plugin : nullptr;
  }
  const MixerTrack* t = s.mixerTrack(f.first);
  if (!t || f.second >= static_cast<int>(t->slots.size())) return nullptr;
  const PluginState& p = t->slots[static_cast<size_t>(f.second)].plugin;
  return p.type.empty() ? nullptr : &p;
}
ParamId focusedPid(lua_State* L, int arg) {
  std::pair<ObjectId, int> w;
  const PluginState* st = focusedState(L, &w);
  if (!st) luaL_error(L, "no plugin window is focused");
  lua_Integer k = luaL_checkinteger(L, arg);
  if (k < 1 || static_cast<size_t>(k) > st->params.size()) luaL_argerror(L, arg, "no such parameter");
  int i = static_cast<int>(k - 1);
  return w.second < 0 ? channelPluginParam(w.first, i) : slotParam(w.first, w.second, i);
}
int p_name(lua_State* L) {
  const PluginState* st = focusedState(L);
  if (!st) {
    lua_pushnil(L);
    return 1;
  }
  const PluginInfo* info = findPlugin(st->type);
  lua_pushstring(L, info ? info->name.c_str() : st->type.c_str());
  return 1;
}
int p_paramCount(lua_State* L) {
  const PluginState* st = focusedState(L);
  lua_pushinteger(L, st ? static_cast<lua_Integer>(st->params.size()) : 0);
  return 1;
}
int p_paramName(lua_State* L) {
  auto d = describeParam(song(L), focusedPid(L, 1));
  lua_pushstring(L, d ? d->name.c_str() : "");
  return 1;
}
int p_param(lua_State* L) {
  lua_pushnumber(L, normGet(song(L), focusedPid(L, 1)));
  return 1;
}
int p_setParam(lua_State* L) {
  ParamId pid = focusedPid(L, 1);
  normSet(host(L).doc, pid, luaL_checknumber(L, 2));
  return 0;
}
int p_paramText(lua_State* L) {
  ParamId pid = focusedPid(L, 1);
  auto d = describeParam(song(L), pid);
  auto v = getParam(song(L), pid);
  lua_pushstring(L, d && v ? d->format(*v).c_str() : "");
  return 1;
}

// ---- device ----
uint8_t byteArg(lua_State* L, int arg) {
  lua_Integer v = luaL_checkinteger(L, arg);
  if (v < 0 || v > 255) luaL_argerror(L, arg, "byte out of range");
  return static_cast<uint8_t>(v);
}
int d_send(lua_State* L) {
  uint8_t s = byteArg(L, 1);
  if (s < 0x80 || s >= 0xF0) luaL_argerror(L, 1, "status must be a channel message (0x80..0xEF)");
  uint8_t a = byteArg(L, 2), b = static_cast<uint8_t>(luaL_optinteger(L, 3, 0) & 0x7F);
  host(L).midiOut(s, a & 0x7F, b);
  return 0;
}
int d_sendSysex(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
  if (n < 2 || n > 1024) luaL_argerror(L, 1, "sysex must hold 2..1024 bytes");
  std::vector<uint8_t> bytes;
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, 1, i);
    lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : -1;
    lua_pop(L, 1);
    if (v < 0 || v > 255) luaL_argerror(L, 1, "bytes must be integers 0..255");
    bytes.push_back(static_cast<uint8_t>(v));
  }
  if (bytes.front() != 0xF0 || bytes.back() != 0xF7) luaL_argerror(L, 1, "sysex must start with 0xF0 and end with 0xF7");
  for (size_t i = 1; i + 1 < bytes.size(); ++i)
    if (bytes[i] & 0x80) luaL_argerror(L, 1, "sysex data bytes must be 0..127");
  host(L).sysexOut(bytes);
  return 0;
}
int d_led(lua_State* L) {
  lua_Integer note = luaL_checkinteger(L, 1);
  bool on = lua_toboolean(L, 2) != 0;
  lua_Integer ch = luaL_optinteger(L, 3, 1);
  if (note < 0 || note > 127) luaL_argerror(L, 1, "note 0..127");
  if (ch < 1 || ch > 16) luaL_argerror(L, 3, "channel 1..16");
  host(L).midiOut(static_cast<uint8_t>(0x90 | (ch - 1)), static_cast<uint8_t>(note), on ? 127 : 0);
  return 0;
}
int d_name(lua_State* L) {
  lua_pushstring(L, host(L).deviceName().c_str());
  return 1;
}
int u_hint(lua_State* L) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, 1, &n);
  host(L).hint(std::string(s, std::min<size_t>(n, 256)));
  return 0;
}

void lib(lua_State* L, const char* name, const luaL_Reg* fns) {
  lua_newtable(L);
  luaL_setfuncs(L, fns, 0);
  lua_setglobal(L, name);
}

void pushMsg(lua_State* L, const MidiMessage& m) {
  lua_createtable(L, 0, 12);
  int t = lua_gettop(L);
  auto num = [&](const char* k, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, t, k);
  };
  num("status", m.status);
  num("data1", m.d1);
  num("data2", m.d2);
  num("channel", m.channel() + 1);
  num("port", m.port);
  const char* kind = "other";
  switch (m.kind()) {
    case 0x90: kind = m.d2 > 0 ? "noteOn" : "noteOff"; break;
    case 0x80: kind = "noteOff"; break;
    case 0xB0: kind = "cc"; break;
    case 0xE0: kind = "pitchBend"; break;
    case 0xC0: kind = "program"; break;
    case 0xA0: kind = "keyPressure"; break;
    case 0xD0: kind = "pressure"; break;
    default: break;
  }
  lua_pushstring(L, kind);
  lua_setfield(L, t, "kind");
  num("note", m.d1);
  num("velocity", m.d2);
  num("cc", m.d1);
  num("value", m.d2);
  num("bend", m.kind() == 0xE0 ? ((m.d2 << 7) | m.d1) - 8192 : 0);
  lua_pushboolean(L, 0);
  lua_setfield(L, t, "handled");
}

}  // namespace

void openControllerApi(Vm& vm, ControllerHost& h) {
  vm.user = &h;
  lua_State* L = vm.L();
  const luaL_Reg transport[] = {{"play", t_play},
                                {"stop", t_stop},
                                {"toggle", t_toggle},
                                {"record", t_record},
                                {"isPlaying", t_isPlaying},
                                {"isRecording", t_isRecording},
                                {"songMode", t_songMode},
                                {"setSongMode", t_setSongMode},
                                {"tempo", t_tempo},
                                {"setTempo", t_setTempo},
                                {"position", t_position},
                                {"setPosition", t_setPosition},
                                {"ppq", t_ppq},
                                {"action", t_action},
                                {nullptr, nullptr}};
  const luaL_Reg mixer[] = {{"count", m_count},   {"name", m_name},         {"volume", m_volume}, {"setVolume", m_setVolume},
                            {"pan", m_pan},       {"setPan", m_setPan},     {"muted", m_muted},   {"setMuted", m_setMuted},
                            {"solo", m_solo},     {"setSolo", m_setSolo},   {"armed", m_armed},   {"setArmed", m_setArmed},
                            {"selected", m_selected}, {"select", m_select}, {nullptr, nullptr}};
  const luaL_Reg channels[] = {{"count", c_count},   {"name", c_name},   {"selected", c_selected}, {"select", c_select},
                               {"volume", c_volume}, {"setVolume", c_setVolume}, {"pan", c_pan}, {"setPan", c_setPan},
                               {"muted", c_muted},   {"setMuted", c_setMuted},   {nullptr, nullptr}};
  const luaL_Reg plugin[] = {{"name", p_name},         {"paramCount", p_paramCount}, {"paramName", p_paramName},
                             {"param", p_param},       {"setParam", p_setParam},     {"paramText", p_paramText},
                             {nullptr, nullptr}};
  const luaL_Reg device[] = {{"send", d_send}, {"sendSysex", d_sendSysex}, {"led", d_led}, {"name", d_name}, {nullptr, nullptr}};
  const luaL_Reg ui[] = {{"hint", u_hint}, {nullptr, nullptr}};
  lib(L, "transport", transport);
  lib(L, "mixer", mixer);
  lib(L, "channels", channels);
  lib(L, "plugin", plugin);
  lib(L, "device", device);
  lib(L, "ui", ui);
}

// -------------------------------------------------------- ControllerScript ----

ControllerScript::~ControllerScript() { unload(); }

bool ControllerScript::loadFile(const std::string& path, ControllerHost& host, std::string* err) {
  std::ifstream is(path, std::ios::binary);
  if (!is) {
    if (err) *err = "cannot read " + path;
    return false;
  }
  std::stringstream ss;
  ss << is.rdbuf();
  std::string stem = path;
  if (size_t s = stem.find_last_of("/\\"); s != std::string::npos) stem = stem.substr(s + 1);
  if (size_t d = stem.rfind('.'); d != std::string::npos) stem = stem.substr(0, d);
  bool ok = loadSource(ss.str(), stem, host, err);
  path_ = path;
  return ok;
}

bool ControllerScript::loadSource(const std::string& source, const std::string& name, ControllerHost& host, std::string* err) {
  unload();
  error_.clear();
  host_ = &host;
  header_ = parseHeader(source);
  name_ = header_.get("name", name);
  Limits load = limits;
  load.instructions = std::max<uint64_t>(load.instructions, 5'000'000);
  vm_ = std::make_unique<Vm>(load);
  if (!vm_->ok()) {
    error_ = "scripting unavailable";
  } else {
    openControllerApi(*vm_, host);
    if (vm_->exec(source, name_, &error_) && vm_->hasFunction("onInit")) callHook("onInit", 0, 0);
    if (error_.empty()) {
      Limits run = limits;
      run.memoryBytes = load.memoryBytes;
      vm_->setLimits(run);
    }
  }
  if (!error_.empty()) {
    if (err) *err = error_;
    return false;
  }
  return true;
}

std::vector<std::string> ControllerScript::match() const {
  std::vector<std::string> out;
  std::string m = header_.get("match");
  size_t a = 0;
  while (a <= m.size()) {
    size_t b = m.find(';', a);
    if (b == std::string::npos) b = m.size();
    std::string part = m.substr(a, b - a);
    size_t x = part.find_first_not_of(" \t"), y = part.find_last_not_of(" \t");
    if (x != std::string::npos) out.push_back(part.substr(x, y - x + 1));
    a = b + 1;
  }
  return out;
}

bool ControllerScript::callHook(const char* fn, int nargs, int nres) {
  if (!active()) {
    lua_pop(vm_->L(), nargs);
    return false;
  }
  if (!vm_->hasFunction(fn)) {
    lua_pop(vm_->L(), nargs);
    for (int i = 0; i < nres; ++i) lua_pushnil(vm_->L());
    return true;
  }
  std::string err;
  if (!vm_->call(fn, nargs, nres, &err)) {
    error_ = std::string(fn) + ": " + err;
    return false;
  }
  return true;
}

bool ControllerScript::onMidiMsg(const MidiMessage& m, bool* handled) {
  if (handled) *handled = false;
  if (!active()) return false;
  lua_State* L = vm_->L();
  if (!vm_->hasFunction("onMidiMsg")) return true;
  pushMsg(L, m);
  int msg = lua_gettop(L);
  lua_pushvalue(L, msg);
  if (!callHook("onMidiMsg", 1, 1)) {
    lua_settop(L, msg - 1);
    return false;
  }
  bool h = lua_toboolean(L, -1) != 0 || fieldBool(L, msg, "handled", false);
  lua_settop(L, msg - 1);
  if (handled) *handled = h;
  return true;
}

bool ControllerScript::onIdle() { return !active() || !vm_->hasFunction("onIdle") || callHook("onIdle", 0, 0); }

bool ControllerScript::onRefresh(const RefreshFlags& f) {
  if (!active() || !vm_->hasFunction("onRefresh")) return active();
  lua_State* L = vm_->L();
  lua_createtable(L, 0, 5);
  const std::pair<const char*, bool> fl[] = {{"mixer", f.mixer}, {"channels", f.channels}, {"transport", f.transport},
                                             {"selection", f.selection}, {"plugin", f.plugin}};
  for (const auto& [k, v] : fl) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
  }
  return callHook("onRefresh", 1, 0);
}

void ControllerScript::unload() {
  if (vm_ && active() && vm_->hasFunction("onDeinit")) callHook("onDeinit", 0, 0);
  vm_.reset();
}

std::vector<std::string> runConsole(Vm& vm, const std::string& code, std::string* err) {
  size_t before = vm.log().size();
  bool expr = false;
  if (vm.ok()) {
    std::string e = "print(" + code + "\n)";
    lua_State* L = vm.L();
    if (luaL_loadbufferx(L, e.data(), e.size(), "=console", "t") == LUA_OK) expr = true;
    lua_pop(L, 1);
    if (expr) {
      vm.exec(e, "console", err);
    } else {
      vm.exec(code, "console", err);
    }
  } else if (err) {
    *err = "scripting unavailable";
  }
  const auto& log = vm.log();
  size_t from = std::min(before, log.size());
  if (log.size() >= Vm::kMaxLogLines) from = 0;
  return std::vector<std::string>(log.begin() + static_cast<long>(from), log.end());
}

}  // namespace gl::script

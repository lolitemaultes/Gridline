// Gridline — piano roll scripts (see PianoRollScript.h).
#include "PianoRollScript.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include "NoteTools.h"
#include "lua/src/lauxlib.h"
#include "lua/src/lua.h"

namespace gl::script {

namespace fs = std::filesystem;

namespace {

// Score and form helpers, defined in Lua before the script runs.
const char* kScorePrelude = R"LUA(
score = { ppq = 96, tsNum = 4, tsDen = 4, bar = 384, length = 0, root = 0, scale = 0, scaleName = '',
          scaleSteps = {}, selStart = 0, selEnd = 0, seed = 1, channel = '', pattern = '',
          _notes = {}, _markers = {} }
local S = score
local function note(t)
  local n = { time = 0, length = math.max(1, S.ppq // 4), pitch = 60, velocity = 100, pan = 0, fine = 0,
              release = 64, modx = 128, mody = 128, color = 0, slide = false, porta = false, muted = false,
              selected = false }
  if t ~= nil then
    if type(t) ~= 'table' then error('a note must be a table', 3) end
    for k, v in pairs(t) do n[k] = v end
  end
  return n
end
function score:newNote(t) return note(t) end
function score:count() return #self._notes end
function score:get(i)
  local n = self._notes[i]
  if n == nil then error('no note at index ' .. tostring(i), 2) end
  return n
end
function score:add(t)
  local n = note(t)
  self._notes[#self._notes + 1] = n
  return #self._notes, n
end
function score:remove(i)
  if self._notes[i] == nil then error('no note at index ' .. tostring(i), 2) end
  table.remove(self._notes, i)
end
function score:notes() return ipairs(self._notes) end
function score:hasSelection()
  for _, n in ipairs(self._notes) do if n.selected then return true end end
  return false
end
-- Indices of the selected notes, or of every note when none is selected.
function score:selection()
  local out, any = {}, self:hasSelection()
  for i, n in ipairs(self._notes) do if not any or n.selected then out[#out + 1] = i end end
  return out
end
-- The note tables the selection() indices refer to.
function score:targets()
  local out = {}
  for k, i in ipairs(self:selection()) do out[k] = self._notes[i] end
  return out
end
-- Removes the selected notes (all notes when none is selected or `all`).
function score:clear(all)
  if all or not self:hasSelection() then self._notes = {} return end
  local keep = {}
  for _, n in ipairs(self._notes) do if not n.selected then keep[#keep + 1] = n end end
  self._notes = keep
end
function score:sort()
  table.sort(self._notes, function(a, b)
    if a.time ~= b.time then return a.time < b.time end
    return a.pitch < b.pitch
  end)
end
function score:markerCount() return #self._markers end
function score:getMarker(i)
  local m = self._markers[i]
  if m == nil then error('no marker at index ' .. tostring(i), 2) end
  return m
end
function score:addMarker(t)
  if type(t) ~= 'table' then error('a marker must be a table', 2) end
  self._markers[#self._markers + 1] = { time = t.time or 0, name = t.name or '', tsNum = t.tsNum or 0,
                                        tsDen = t.tsDen or 0, action = t.action or 0 }
  return #self._markers
end
function score:removeMarker(i)
  if self._markers[i] == nil then error('no marker at index ' .. tostring(i), 2) end
  table.remove(self._markers, i)
end
function score:inScale(p)
  if #self.scaleSteps == 0 then return true end
  local d = (math.floor(p) - self.root) % 12
  for _, s in ipairs(self.scaleSteps) do if s == d then return true end end
  return false
end
-- Nearest pitch in the scale (ties go down).
function score:snap(p)
  p = math.floor(p)
  if #self.scaleSteps == 0 then return p end
  for k = 0, 6 do
    if self:inScale(p - k) then return p - k end
    if self:inScale(p + k) then return p + k end
  end
  return p
end
-- Pitch `degree` steps up the scale from `base` (negative = down).
function score:degree(base, degree)
  local p = self:snap(base)
  local step = degree >= 0 and 1 or -1
  for _ = 1, math.abs(math.floor(degree)) do
    repeat p = p + step until self:inScale(p)
  end
  return p
end

)LUA";

// Declarative parameter forms (shared by every script kind with a form()).
const char* kFormPrelude = R"LUA(
local Form = {}
Form.__index = Form
local function addp(f, p)
  if type(p.name) ~= 'string' or p.name == '' then error('a control needs a name', 3) end
  for _, q in ipairs(f._p) do
    if q.name == p.name then error('duplicate control name: ' .. p.name, 3) end
  end
  p.group = f._group
  f._p[#f._p + 1] = p
end
function Form:knob(name, value, min, max, hint) addp(self, { kind = 'knob', name = name, value = value, min = min, max = max, hint = hint }) end
function Form:knobInt(name, value, min, max, hint) addp(self, { kind = 'knobInt', name = name, value = value, min = min, max = max, hint = hint }) end
function Form:combo(name, items, value, hint) addp(self, { kind = 'combo', name = name, items = items, value = value, hint = hint }) end
function Form:check(name, value, hint) addp(self, { kind = 'check', name = name, value = value, hint = hint }) end
function Form:text(name, value, hint) addp(self, { kind = 'text', name = name, value = value, hint = hint }) end
function Form:group(name) self._group = name end
function Form:endGroup() self._group = nil end
function Form:description(t) self._desc = t end
function _gl_newForm() return setmetatable({ _p = {} }, Form) end
)LUA";

bool finite(double v) { return std::isfinite(v); }

}  // namespace

const char* formPrelude() { return kFormPrelude; }

// Reads the `_p` list of a form object at stack index idx.
bool readForm(lua_State* L, int idx, Form& out, std::string* err) {
  idx = lua_absindex(L, idx);
  out = Form{};
  out.description = fieldString(L, idx, "_desc", "");
  lua_getfield(L, idx, "_p");
  int list = lua_gettop(L);
  if (!lua_istable(L, list)) {
    if (err) *err = "the form's control list was replaced";
    lua_settop(L, list - 1);
    return false;
  }
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, list));
  for (lua_Integer i = 1; i <= n && i <= 128; ++i) {
    lua_rawgeti(L, list, i);
    int t = lua_gettop(L);
    if (!lua_istable(L, t)) {
      if (err) *err = "control " + std::to_string(i) + " is not a table";
      lua_settop(L, list - 1);
      return false;
    }
    FormParam p;
    std::string kind = fieldString(L, t, "kind", "");
    p.name = fieldString(L, t, "name", "");
    p.hint = fieldString(L, t, "hint", "");
    p.group = fieldString(L, t, "group", "");
    auto bad = [&](const std::string& why) {
      if (err) *err = "control '" + p.name + "': " + why;
      lua_settop(L, list - 1);
      return false;
    };
    if (kind == "knob" || kind == "knobInt") {
      p.kind = kind == "knob" ? FormKind::Knob : FormKind::KnobInt;
      p.min = fieldNumber(L, t, "min", 0);
      p.max = fieldNumber(L, t, "max", 1);
      p.value = fieldNumber(L, t, "value", p.min);
      if (!finite(p.min) || !finite(p.max) || !finite(p.value)) return bad("values must be finite numbers");
      if (p.max < p.min) std::swap(p.min, p.max);
      if (p.kind == FormKind::KnobInt) p.min = std::round(p.min), p.max = std::round(p.max), p.value = std::round(p.value);
      p.value = std::clamp(p.value, p.min, p.max);
    } else if (kind == "combo") {
      p.kind = FormKind::Combo;
      lua_getfield(L, t, "items");
      if (!lua_istable(L, -1)) return bad("items must be a list of strings");
      const lua_Integer k = static_cast<lua_Integer>(lua_rawlen(L, -1));
      for (lua_Integer j = 1; j <= k && j <= 256; ++j) {
        lua_rawgeti(L, -1, j);
        if (lua_type(L, -1) != LUA_TSTRING && lua_type(L, -1) != LUA_TNUMBER) return bad("items must be a list of strings");
        p.items.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
      if (p.items.empty()) return bad("needs at least one item");
      p.min = 1;
      p.max = static_cast<double>(p.items.size());
      double v = fieldNumber(L, t, "value", 1);
      p.value = std::clamp(finite(v) ? std::round(v) : 1.0, 1.0, p.max);
    } else if (kind == "check") {
      p.kind = FormKind::Check;
      p.max = 1;
      p.value = fieldBool(L, t, "value", false) ? 1 : 0;
    } else if (kind == "text") {
      p.kind = FormKind::Text;
      p.text = fieldString(L, t, "value", "");
      if (p.text.size() > 1024) p.text.resize(1024);
    } else {
      return bad("unknown control kind");
    }
    out.params.push_back(std::move(p));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return true;
}

void pushNote(lua_State* L, const Note& n, bool selected) {
  lua_createtable(L, 0, 14);
  auto num = [&](const char* k, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
  };
  auto flag = [&](const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
  };
  num("time", n.start);
  num("length", n.length);
  num("pitch", n.pitch);
  num("velocity", n.velocity);
  num("pan", n.pan);
  num("fine", n.fine);
  num("release", n.release);
  num("modx", n.modX);
  num("mody", n.modY);
  num("color", n.color);
  flag("slide", n.slide);
  flag("porta", n.porta);
  flag("muted", n.muted);
  flag("selected", selected);
}

namespace {
// Integer field with range check; false + message for non-numbers.
bool intField(lua_State* L, int t, const char* key, long long def, long long lo, long long hi, long long* out, std::string* err) {
  lua_getfield(L, t, key);
  double v = static_cast<double>(def);
  if (!lua_isnil(L, -1)) {
    if (!lua_isnumber(L, -1)) {
      lua_pop(L, 1);
      if (err) *err = std::string("field '") + key + "' must be a number";
      return false;
    }
    v = lua_tonumber(L, -1);
  }
  lua_pop(L, 1);
  if (!std::isfinite(v)) {
    if (err) *err = std::string("field '") + key + "' is not a finite number";
    return false;
  }
  double r = std::round(v);
  *out = static_cast<long long>(std::clamp(r, static_cast<double>(lo), static_cast<double>(hi)));
  return true;
}

}  // namespace

bool readNote(lua_State* L, int t, Note& n, bool& selected, std::string* err) {
  long long v = 0;
  const long long kMaxTick = 1LL << 40;
  if (!intField(L, t, "time", 0, 0, kMaxTick, &v, err)) return false;
  n.start = v;
  if (!intField(L, t, "length", 24, 1, kMaxTick, &v, err)) return false;
  n.length = v;
  if (!intField(L, t, "pitch", 60, 0, 127, &v, err)) return false;
  n.pitch = static_cast<int>(v);
  if (!intField(L, t, "velocity", 100, 0, 127, &v, err)) return false;
  n.velocity = static_cast<int>(v);
  if (!intField(L, t, "pan", 0, -kNotePanMax, kNotePanMax, &v, err)) return false;
  n.pan = static_cast<int>(v);
  if (!intField(L, t, "fine", 0, -kNoteFineMax, kNoteFineMax, &v, err)) return false;
  n.fine = static_cast<int>(v);
  if (!intField(L, t, "release", kNoteReleaseDefault, 0, 127, &v, err)) return false;
  n.release = static_cast<int>(v);
  if (!intField(L, t, "modx", kNoteModCentre, 0, kNoteModMax, &v, err)) return false;
  n.modX = static_cast<int>(v);
  if (!intField(L, t, "mody", kNoteModCentre, 0, kNoteModMax, &v, err)) return false;
  n.modY = static_cast<int>(v);
  if (!intField(L, t, "color", 0, 0, kNoteColors - 1, &v, err)) return false;
  n.color = static_cast<int>(v);
  n.slide = fieldBool(L, t, "slide", false);
  n.porta = fieldBool(L, t, "porta", false);
  n.muted = fieldBool(L, t, "muted", false);
  selected = fieldBool(L, t, "selected", false);
  return true;
}

namespace {

void setInt(lua_State* L, int t, const char* k, lua_Integer v) {
  lua_pushinteger(L, v);
  lua_setfield(L, t, k);
}
void setStr(lua_State* L, int t, const char* k, const std::string& v) {
  lua_pushlstring(L, v.data(), v.size());
  lua_setfield(L, t, k);
}

// Fills the global `score` from ctx.
void pushContext(lua_State* L, const PianoRollContext& ctx) {
  lua_getglobal(L, "score");
  int t = lua_gettop(L);
  setInt(L, t, "ppq", ctx.ppq);
  setInt(L, t, "tsNum", ctx.tsNum);
  setInt(L, t, "tsDen", ctx.tsDen);
  setInt(L, t, "bar", static_cast<lua_Integer>(ctx.ppq) * 4 * ctx.tsNum / std::max(1, ctx.tsDen));
  setInt(L, t, "length", ctx.length);
  setInt(L, t, "root", ((ctx.root % 12) + 12) % 12);
  setInt(L, t, "scale", ctx.scale);
  setInt(L, t, "selStart", ctx.selStart);
  setInt(L, t, "selEnd", ctx.selEnd);
  setInt(L, t, "seed", ctx.seed);
  setStr(L, t, "channel", ctx.channel);
  setStr(L, t, "pattern", ctx.pattern);
  const auto& sc = tools::scales();
  lua_newtable(L);
  if (ctx.scale > 0 && static_cast<size_t>(ctx.scale) < sc.size()) {
    lua_Integer k = 1;
    for (int s : sc[static_cast<size_t>(ctx.scale)].steps) {
      lua_pushinteger(L, s);
      lua_rawseti(L, -2, k++);
    }
    setStr(L, t, "scaleName", sc[static_cast<size_t>(ctx.scale)].name);
  }
  lua_setfield(L, t, "scaleSteps");
  lua_createtable(L, static_cast<int>(std::min<size_t>(ctx.notes.size(), 1u << 20)), 0);
  for (size_t i = 0; i < ctx.notes.size(); ++i) {
    pushNote(L, ctx.notes[i], i < ctx.sel.size() && ctx.sel[i]);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, t, "_notes");
  lua_newtable(L);
  for (size_t i = 0; i < ctx.markers.size(); ++i) {
    const Marker& m = ctx.markers[i];
    lua_createtable(L, 0, 5);
    int mt = lua_gettop(L);
    setInt(L, mt, "time", m.pos);
    setStr(L, mt, "name", m.name);
    setInt(L, mt, "tsNum", m.timeSigNum);
    setInt(L, mt, "tsDen", m.timeSigDen);
    setInt(L, mt, "action", static_cast<int>(m.action));
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, t, "_markers");
  lua_pop(L, 1);
}

}  // namespace

void pushFormValues(lua_State* L, const Form& form, const std::vector<FormParam>& values) {
  lua_newtable(L);
  for (const FormParam& d : form.params) {
    const FormParam* p = &d;
    for (const auto& v : values)
      if (v.name == d.name && v.kind == d.kind) p = &v;
    switch (d.kind) {
      case FormKind::Knob: lua_pushnumber(L, std::clamp(p->value, d.min, d.max)); break;
      case FormKind::KnobInt:
      case FormKind::Combo: lua_pushinteger(L, static_cast<lua_Integer>(std::clamp(std::round(p->value), d.min, d.max))); break;
      case FormKind::Check: lua_pushboolean(L, p->value >= 0.5); break;
      case FormKind::Text: lua_pushlstring(L, p->text.data(), p->text.size()); break;
    }
    lua_setfield(L, -2, d.name.c_str());
  }
}

bool buildForm(Vm& vm, Form& out, std::string* err) {
  out = Form{};
  if (!vm.hasFunction("form")) return true;
  // Protected: the script may have replaced _gl_newForm or the form's
  // fields (J-36: errors there aborted the process).
  bool read = false;
  std::string why;
  if (!vm.protect(
          [&](lua_State* L) {
            lua_getglobal(L, "_gl_newForm");
            lua_call(L, 0, 1);
            lua_getglobal(L, "form");
            lua_pushvalue(L, -2);
            lua_call(L, 1, 0);
            read = readForm(L, -1, out, &why);
          },
          err))
    return false;
  if (!read) {
    if (err) *err = why;
    return false;
  }
  return true;
}

int Form::find(const std::string& name) const {
  for (size_t i = 0; i < params.size(); ++i)
    if (params[i].name == name) return static_cast<int>(i);
  return -1;
}

std::vector<ScriptInfo> scanScripts(const std::vector<std::pair<std::string, bool>>& dirs) {
  std::map<std::string, ScriptInfo> byId;
  for (const auto& [dir, user] : dirs) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->path().extension() != ".lua") continue;
      std::ifstream is(it->path(), std::ios::binary);
      std::string head(4096, '\0');
      is.read(&head[0], static_cast<std::streamsize>(head.size()));
      head.resize(static_cast<size_t>(is.gcount()));
      Header h = parseHeader(head);
      ScriptInfo s;
      s.path = it->path().string();
      s.id = it->path().stem().string();
      s.name = h.get("name", s.id);
      s.category = h.get("category", "Other");
      s.description = h.get("description");
      s.author = h.get("author");
      s.user = user;
      byId[s.id] = s;
    }
  }
  std::vector<ScriptInfo> out;
  for (auto& [id, s] : byId) out.push_back(s);
  std::sort(out.begin(), out.end(), [](const ScriptInfo& a, const ScriptInfo& b) {
    return a.category != b.category ? a.category < b.category : a.name < b.name;
  });
  return out;
}

bool PianoRollScript::loadFile(const std::string& path, std::string* err) {
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

bool PianoRollScript::loadSource(const std::string& source, const std::string& name, std::string* err) {
  source_.clear();
  path_.clear();
  header_ = parseHeader(source);
  name_ = header_.get("name", name);
  form_ = Form{};
  Limits lim;
  lim.instructions = 2'000'000;
  lim.seconds = 1.0;
  Vm vm(lim);
  if (!vm.ok() || !vm.exec(kScorePrelude, "score", err) || !vm.exec(kFormPrelude, "form", err)) return false;
  if (!vm.exec(source, name_, err)) return false;
  if (!buildForm(vm, form_, err)) return false;
  if (form_.description.empty()) form_.description = header_.get("description");
  source_ = source;
  return true;
}

Limits previewLimits(const Limits& full) {
  Limits l = full;
  l.seconds = std::min(full.seconds, kPreviewSeconds);
  l.instructions = std::min(full.instructions, kPreviewInstructions);
  return l;
}

void PreviewScheduler::changed(double now, bool dragging) {
  changedAt_ = now;
  dragging_ = dragging;
  pending_ = dragging ? Pending::Preview : Pending::Full;
}

void PreviewScheduler::released() {
  dragging_ = false;
  // Values not run yet, or the last preview ran out of budget: one full run.
  if (pending_ == Pending::Preview || (pending_ == Pending::None && partial_)) pending_ = Pending::Full;
}

PreviewScheduler::Run PreviewScheduler::poll(double now) {
  switch (pending_) {
    case Pending::None: return Run::None;
    case Pending::Full: return Run::Full;
    case Pending::Preview:
      return now - changedAt_ >= kSettle || now - lastRunAt_ >= kMaxWait ? Run::Preview : Run::None;
  }
  return Run::None;
}

void PreviewScheduler::ran(Run kind, double now, bool limitHit) {
  if (kind == Run::None) return;
  lastRunAt_ = now;
  pending_ = Pending::None;
  partial_ = kind == Run::Preview && limitHit;
  if (partial_ && !dragging_) pending_ = Pending::Full;
}

PianoRollResult PianoRollScript::run(const PianoRollContext& ctx, const std::vector<FormParam>& values, const Limits& limits,
                                     const std::string& fileRoot) const {
  PianoRollResult r;
  if (source_.empty()) {
    r.error = "no script loaded";
    return r;
  }
  Vm vm(limits, fileRoot, false);
  lua_State* L = vm.L();
  if (!vm.ok() || !vm.exec(kScorePrelude, "score", &r.error) || !vm.exec(kFormPrelude, "form", &r.error)) return r;
  pushContext(L, ctx);
  // Deterministic randomness per run.
  lua_getglobal(L, "math");
  lua_getfield(L, -1, "randomseed");
  lua_pushinteger(L, static_cast<lua_Integer>(ctx.seed));
  lua_call(L, 1, 0);
  lua_pop(L, 1);
  bool ok = vm.exec(source_, name_, &r.error);
  if (ok && vm.hasFunction("apply"))
    ok = vm.protect(
        [&](lua_State* S) {
          lua_getglobal(S, "apply");
          lua_getglobal(S, "score");
          pushFormValues(S, form_, values);
          lua_call(S, 2, 0);
        },
        &r.error);
  if (!ok) {
    r.log = vm.log();
    r.limitHit = !vm.abortReason().empty();  // instruction / time limit (memory errors are not budget)
    return r;
  }
  // Read the notes back, protected: note tables may carry metamethods that
  // raise or loop (J-36: an error there aborted the process).
  std::string readErr;
  if (vm.protect([&](lua_State* S) { readBack(S, r); }, &readErr)) {
    r.markersChanged = r.markers != ctx.markers;
  } else {
    r.ok = false;
    r.notes.clear(), r.sel.clear(), r.markers.clear();
    r.error = readErr;
    r.limitHit = !vm.abortReason().empty();
  }
  r.log = vm.log();
  return r;
}

void PianoRollScript::readBack(lua_State* L, PianoRollResult& r) const {
  lua_getglobal(L, "score");
  int st = lua_gettop(L);
  if (!lua_istable(L, st)) {
    r.error = "score is not a table";
    return;
  }
  lua_getfield(L, st, "_notes");
  if (!lua_istable(L, -1)) {
    r.error = "score._notes is not a list";
    lua_settop(L, st - 1);
    return;
  }
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
  if (static_cast<size_t>(n) > kMaxNotes) {
    r.error = "too many notes (" + std::to_string(n) + ")";
    lua_settop(L, st - 1);
    return;
  }
  std::vector<Note> notes;
  std::vector<bool> sel;
  notes.reserve(static_cast<size_t>(n));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, -1, i);
    Note x;
    bool s = false;
    std::string why;
    if (!lua_istable(L, -1)) why = "is not a table";
    if (why.empty()) readNote(L, lua_gettop(L), x, s, &why);
    lua_pop(L, 1);
    if (!why.empty()) {
      r.error = "note " + std::to_string(i) + ": " + why;
      lua_settop(L, st - 1);
      return;
    }
    notes.push_back(x);
    sel.push_back(s);
  }
  lua_pop(L, 1);
  // Markers.
  lua_getfield(L, st, "_markers");
  if (lua_istable(L, -1)) {
    const lua_Integer m = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(L, -1)), 10000);
    for (lua_Integer i = 1; i <= m; ++i) {
      lua_rawgeti(L, -1, i);
      if (lua_istable(L, -1)) {
        int t = lua_gettop(L);
        Marker mk;
        long long v = 0;
        std::string why;
        if (intField(L, t, "time", 0, 0, 1LL << 40, &v, &why)) mk.pos = v;
        mk.name = fieldString(L, t, "name", "");
        if (mk.name.size() > 256) mk.name.resize(256);
        if (intField(L, t, "tsNum", 0, 0, 16, &v, &why)) mk.timeSigNum = static_cast<int>(v);
        if (intField(L, t, "tsDen", 0, 0, 16, &v, &why)) mk.timeSigDen = static_cast<int>(v);
        if (intField(L, t, "action", 0, 0, static_cast<long long>(MarkerAction::Count) - 1, &v, &why)) mk.action = static_cast<MarkerAction>(v);
        r.markers.push_back(mk);
      }
      lua_pop(L, 1);
    }
  }
  lua_settop(L, st - 1);
  std::stable_sort(r.markers.begin(), r.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });

  // Sort notes (start, pitch), selection follows.
  std::vector<size_t> order(notes.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return notes[a].start != notes[b].start ? notes[a].start < notes[b].start : notes[a].pitch < notes[b].pitch;
  });
  r.notes.reserve(notes.size());
  r.sel.reserve(notes.size());
  for (size_t i : order) r.notes.push_back(notes[i]), r.sel.push_back(sel[i]);
  r.ok = true;
}

}  // namespace gl::script

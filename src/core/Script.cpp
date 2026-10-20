// Gridline — sandboxed Lua VM (see Script.h).
#include "Script.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "lua/src/lauxlib.h"
#include "lua/src/lua.h"
#include "lua/src/lualib.h"

namespace gl::script {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ header ----

std::string Header::get(const std::string& key, const std::string& def) const {
  for (const auto& [k, v] : fields)
    if (k == key) return v;
  return def;
}

Header parseHeader(const std::string& source) {
  Header h;
  std::istringstream is(source);
  std::string line;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) continue;  // blank lines inside the header are fine
    if (line.compare(i, 2, "--") != 0) break;  // first code line ends the header
    std::string body = line.substr(i + 2);
    size_t c = body.find(':');
    if (c == std::string::npos) continue;
    std::string key = body.substr(0, c), val = body.substr(c + 1);
    auto trim = [](std::string& s) {
      size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
      s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    trim(key);
    trim(val);
    if (key.empty() || key.size() > 24 || key.find(' ') != std::string::npos) continue;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    // Repeated keys continue the value (long descriptions).
    bool merged = false;
    for (auto& [k, v] : h.fields)
      if (k == key) v += " " + val, merged = true;
    if (!merged) h.fields.emplace_back(key, val);
  }
  return h;
}

bool safeFileName(const std::string& n) {
  if (n.empty() || n.size() > 96 || n[0] == '.' || n.find("..") != std::string::npos) return false;
  for (unsigned char c : n)
    if (!(std::isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.')) return false;
  return true;
}

// --------------------------------------------------------------- helpers ----

double fieldNumber(lua_State* L, int idx, const char* key, double def) {
  lua_getfield(L, idx, key);
  double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
  lua_pop(L, 1);
  return v;
}
bool fieldBool(lua_State* L, int idx, const char* key, bool def) {
  lua_getfield(L, idx, key);
  bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}
std::string fieldString(lua_State* L, int idx, const char* key, const std::string& def) {
  lua_getfield(L, idx, key);
  std::string v = def;
  if (lua_type(L, -1) == LUA_TSTRING) {
    size_t n = 0;
    const char* s = lua_tolstring(L, -1, &n);
    v.assign(s, n);
  }
  lua_pop(L, 1);
  return v;
}

namespace {

// print / log: arguments joined by tabs, like Lua's print.
int l_print(lua_State* L) {
  Vm* vm = Vm::from(L);
  int n = lua_gettop(L);
  std::string line;
  for (int i = 1; i <= n; ++i) {
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    if (i > 1) line += '\t';
    line.append(s, std::min<size_t>(len, 4096));
    lua_pop(L, 1);
  }
  vm->addLog(line);
  return 0;
}

int l_collectgarbage(lua_State* L) {
  const char* opt = luaL_optstring(L, 1, "collect");
  if (std::strcmp(opt, "count") == 0) {
    int k = lua_gc(L, LUA_GCCOUNT, 0), b = lua_gc(L, LUA_GCCOUNTB, 0);
    lua_pushnumber(L, static_cast<lua_Number>(k) + static_cast<lua_Number>(b) / 1024.0);
    return 1;
  }
  if (std::strcmp(opt, "collect") == 0) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_pushinteger(L, 0);
    return 1;
  }
  return luaL_error(L, "collectgarbage option '%s' is not available in scripts", opt);
}

// files.*: plain files directly inside the VM's file folder.
bool resolve(lua_State* L, const char* name, fs::path* out, std::string* why) {
  Vm* vm = Vm::from(L);
  if (vm->fileRoot().empty()) {
    *why = "no file access for this script";
    return false;
  }
  if (!safeFileName(name)) {
    *why = std::string("file name not allowed: ") + name;
    return false;
  }
  std::error_code ec;
  fs::path root = fs::weakly_canonical(vm->fileRoot(), ec);
  fs::path p = root / name;
  // A link inside the folder must not lead out of it.
  auto st = fs::symlink_status(p, ec);
  if (!ec && fs::is_symlink(st)) {
    *why = "links are not followed";
    return false;
  }
  if (p.parent_path() != root) {
    *why = "outside the script folder";
    return false;
  }
  *out = p;
  return true;
}

int l_fread(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  fs::path p;
  std::string why;
  if (!resolve(L, name, &p, &why)) {
    lua_pushnil(L);
    lua_pushstring(L, why.c_str());
    return 2;
  }
  std::error_code ec;
  if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) > Vm::kMaxFileBytes) {
    lua_pushnil(L);
    lua_pushstring(L, "no such file (or too large)");
    return 2;
  }
  std::ifstream is(p, std::ios::binary);
  std::stringstream ss;
  ss << is.rdbuf();
  std::string s = ss.str();
  lua_pushlstring(L, s.data(), s.size());
  return 1;
}

int l_fwrite(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  size_t len = 0;
  const char* data = luaL_checklstring(L, 2, &len);
  Vm* vm = Vm::from(L);
  fs::path p;
  std::string why;
  if (!vm->fileWrite()) why = "this script may not write files";
  if (why.empty() && len > Vm::kMaxFileBytes) why = "file too large";
  if (!why.empty() || !resolve(L, name, &p, &why)) {
    lua_pushnil(L);
    lua_pushstring(L, why.c_str());
    return 2;
  }
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream os(p, std::ios::binary | std::ios::trunc);
  os.write(data, static_cast<std::streamsize>(len));
  if (!os) {
    lua_pushnil(L);
    lua_pushstring(L, "write failed");
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int l_fexists(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  fs::path p;
  std::string why;
  std::error_code ec;
  lua_pushboolean(L, resolve(L, name, &p, &why) && fs::is_regular_file(p, ec));
  return 1;
}

int l_flist(lua_State* L) {
  Vm* vm = Vm::from(L);
  lua_newtable(L);
  if (vm->fileRoot().empty()) return 1;
  std::vector<std::string> names;
  std::error_code ec;
  for (fs::directory_iterator it(vm->fileRoot(), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    std::string n = it->path().filename().string();
    if (safeFileName(n) && it->is_regular_file(e2) && !it->is_symlink(e2)) names.push_back(n);
  }
  std::sort(names.begin(), names.end());
  lua_Integer i = 1;
  for (const auto& n : names) {
    lua_pushstring(L, n.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

// Guards for the pattern functions: matching runs in C (no instruction
// hook), so very long subjects or patterns are refused.
const char* kPrelude = R"LUA(
do
  local find, match, gmatch, gsub, type, error = string.find, string.match, string.gmatch, string.gsub, type, error
  local function chk(s, p)
    if type(s) == 'string' and #s > 262144 then error('string too long for pattern matching', 3) end
    if type(p) == 'string' and #p > 512 then error('pattern too long', 3) end
  end
  string.find = function(s, p, ...) chk(s, p) return find(s, p, ...) end
  string.match = function(s, p, ...) chk(s, p) return match(s, p, ...) end
  string.gmatch = function(s, p, ...) chk(s, p) return gmatch(s, p, ...) end
  string.gsub = function(s, p, ...) chk(s, p) return gsub(s, p, ...) end
  -- J-36: finalizers run with debug hooks off (no instruction / time limit),
  -- so an endless __gc loop would hang the host: metatables with __gc are refused.
  local smt, rawget = setmetatable, rawget
  setmetatable = function(t, mt)
    if type(mt) == 'table' and rawget(mt, '__gc') ~= nil then error('__gc metamethods are not available in scripts', 2) end
    return smt(t, mt)
  end
  -- table.move loops in C (no hook): bound the element count.
  local move, tonumber = table.move, tonumber
  table.move = function(a1, f, e, t, ...)
    local nf, ne = tonumber(f), tonumber(e)
    if nf and ne and ne - nf > 16777216 then error('too many elements to move', 2) end
    return move(a1, f, e, t, ...)
  end
end
)LUA";

}  // namespace

// ------------------------------------------------------------------- Vm ----

Vm* Vm::from(lua_State* L) { return *static_cast<Vm**>(lua_getextraspace(L)); }

void* Vm::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  Vm* vm = static_cast<Vm*>(ud);
  if (!ptr) osize = 0;  // osize is a type tag for new blocks
  if (nsize == 0) {
    std::free(ptr);
    vm->mem_ -= std::min(vm->mem_, osize);
    return nullptr;
  }
  if (nsize > osize && vm->mem_ - osize + nsize > vm->limits_.memoryBytes) return nullptr;
  void* p = std::realloc(ptr, nsize);
  if (!p) return nullptr;
  vm->mem_ = vm->mem_ - osize + nsize;
  return p;
}

void Vm::hook(lua_State* L, lua_Debug* /*ar*/) {
  Vm* vm = from(L);
  vm->used_ += kHookEvery;
  if (vm->abort_.empty()) {
    if (vm->used_ > vm->limits_.instructions) {
      vm->abort_ = "script stopped: instruction limit reached";
    } else {
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - vm->start_).count();
      if (s > vm->limits_.seconds) vm->abort_ = "script stopped: time limit reached";
    }
  }
  if (!vm->abort_.empty()) {
    // From now on every instruction raises, so pcall cannot keep it running.
    lua_sethook(L, &Vm::hook, LUA_MASKCOUNT, 1);
    luaL_error(L, "%s", vm->abort_.c_str());
  }
}

Vm::Vm(Limits limits, std::string fileRoot, bool fileWrite)
    : limits_(limits), fileRoot_(std::move(fileRoot)), fileWrite_(fileWrite) {
  L_ = lua_newstate(&Vm::alloc, this);
  if (!L_) return;
  *static_cast<Vm**>(lua_getextraspace(L_)) = this;
  begin();
  sandbox();
}

Vm::~Vm() {
  if (L_) lua_close(L_);
}

void Vm::addLog(const std::string& line) {
  if (logSink) logSink(line);
  if (log_.size() >= kMaxLogLines) log_.erase(log_.begin());
  log_.push_back(line);
}

void Vm::sandbox() {
  lua_State* L = L_;
  const std::pair<const char*, lua_CFunction> libs[] = {{"_G", luaopen_base},          {LUA_TABLIBNAME, luaopen_table},
                                                        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                                                        {LUA_UTF8LIBNAME, luaopen_utf8},  {LUA_COLIBNAME, luaopen_coroutine}};
  for (const auto& [name, fn] : libs) {
    luaL_requiref(L, name, fn, 1);
    lua_pop(L, 1);
  }
  for (const char* n : {"dofile", "loadfile", "load", "require", "warn", "collectgarbage", "print"}) {
    lua_pushnil(L);
    lua_setglobal(L, n);
  }
  lua_getglobal(L, "string");
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);
  lua_register(L, "print", l_print);
  lua_register(L, "log", l_print);
  lua_register(L, "collectgarbage", l_collectgarbage);
  // Deterministic random numbers unless the host seeds them.
  lua_getglobal(L, "math");
  lua_getfield(L, -1, "randomseed");
  lua_pushinteger(L, 0x5EED);
  lua_call(L, 1, 0);
  lua_pop(L, 1);
  // files API.
  lua_newtable(L);
  const luaL_Reg files[] = {{"read", l_fread}, {"write", l_fwrite}, {"exists", l_fexists}, {"list", l_flist}, {nullptr, nullptr}};
  luaL_setfuncs(L, files, 0);
  lua_setglobal(L, "files");
  std::string err;
  exec(kPrelude, "sandbox", &err);
}

void Vm::begin() {
  used_ = 0;
  abort_.clear();
  start_ = std::chrono::steady_clock::now();
  lua_sethook(L_, &Vm::hook, LUA_MASKCOUNT, kHookEvery);
}

bool Vm::finish(int status, int nres, std::string* err) {
  if (status != LUA_OK) {
    std::string msg = abort_;
    if (msg.empty()) {
      if (status == LUA_ERRMEM)
        msg = "script stopped: memory limit reached";
      else if (lua_type(L_, -1) == LUA_TSTRING)
        msg = lua_tostring(L_, -1);
      else
        msg = "script error (non-string error value)";
    }
    lua_pop(L_, 1);
    if (err) *err = msg;
    return false;
  }
  if (!abort_.empty()) {  // stopped, but the script caught the error
    lua_pop(L_, nres);
    if (err) *err = abort_;
    return false;
  }
  return true;
}

bool Vm::exec(const std::string& code, const std::string& chunkName, std::string* err) {
  if (!L_) {
    if (err) *err = "scripting unavailable";
    return false;
  }
  begin();
  int top = lua_gettop(L_);
  int st = luaL_loadbufferx(L_, code.data(), code.size(), ("=" + chunkName).c_str(), "t");
  if (st == LUA_OK) st = lua_pcall(L_, 0, 0, 0);
  bool ok = finish(st, 0, err);
  lua_settop(L_, top);
  return ok;
}

// Raw lookup in the globals table: a script may give _G an __index
// metamethod, and running Lua code (or raising) outside a protected call
// would abort the process (J-36).
static void rawGlobal(lua_State* L, const char* name) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  lua_pushstring(L, name);
  lua_rawget(L, -2);
  lua_remove(L, -2);
}

bool Vm::hasFunction(const char* fn) {
  if (!L_) return false;
  rawGlobal(L_, fn);
  bool f = lua_isfunction(L_, -1);
  lua_pop(L_, 1);
  return f;
}

namespace {
int protectTrampoline(lua_State* L) {
  auto* fn = static_cast<const std::function<void(lua_State*)>*>(lua_touserdata(L, 1));
  lua_remove(L, 1);  // the arguments are now 1..n
  (*fn)(L);
  return 0;
}
}  // namespace

bool Vm::protect(const std::function<void(lua_State*)>& fn, std::string* err, int nargs) {
  if (!L_) {
    if (err) *err = "scripting unavailable";
    return false;
  }
  begin();
  int top = lua_gettop(L_) - nargs;
  lua_pushcfunction(L_, protectTrampoline);
  lua_insert(L_, top + 1);
  lua_pushlightuserdata(L_, const_cast<std::function<void(lua_State*)>*>(&fn));
  lua_insert(L_, top + 2);
  int st = lua_pcall(L_, nargs + 1, 0, 0);
  bool ok = finish(st, 0, err);
  lua_settop(L_, top);
  return ok;
}

bool Vm::call(const char* fn, int nargs, int nres, std::string* err) {
  if (!L_) {
    if (err) *err = "scripting unavailable";
    return false;
  }
  rawGlobal(L_, fn);
  if (!lua_isfunction(L_, -1)) {
    lua_pop(L_, 1 + nargs);
    if (err) *err = std::string("the script has no function '") + fn + "'";
    return false;
  }
  lua_insert(L_, -(nargs + 1));
  begin();
  int st = lua_pcall(L_, nargs, nres, 0);
  return finish(st, nres, err);
}

}  // namespace gl::script

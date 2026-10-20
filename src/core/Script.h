// Gridline — embedded scripting (SC-01): a sandboxed Lua 5.4 virtual machine.
//
// What a script gets: the base library without file / code loading (no
// dofile, loadfile, load, require, collectgarbage options other than "count" /
// "collect", no string.dump, no warn), string / table / math / utf8 /
// coroutine, `print` / `log` into the VM's log, and a vetted `files` API that
// reads (and optionally writes) plain files in one folder only. No io, os,
// package or debug library exists.
//
// Limits: an instruction budget and a wall-clock budget per call (checked
// every kHookEvery VM instructions; once exceeded, the VM keeps raising so a
// script cannot swallow the error with pcall) and a memory cap enforced by the
// allocator. Pattern-matching functions refuse very long subjects / patterns.
//
// Lua is compiled as C++, so errors unwind with exceptions and C++ objects in
// the glue are destroyed normally.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace gl::script {

struct Limits {
  uint64_t instructions = 20'000'000;  // per call (exec / call)
  double seconds = 2.0;                // per call
  size_t memoryBytes = 64u << 20;      // whole VM
};

// Script header: "-- key: value" comment lines at the top of a script file
// (name, category, description, author, match, …). Keys are lower case.
struct Header {
  std::vector<std::pair<std::string, std::string>> fields;
  std::string get(const std::string& key, const std::string& def = "") const;
};
Header parseHeader(const std::string& source);

// True for names the files API accepts: 1..96 characters of letters, digits,
// space, '_', '-', '.', not starting with '.', no "..".
bool safeFileName(const std::string& name);

class Vm {
 public:
  static constexpr int kHookEvery = 1000;
  static constexpr size_t kMaxLogLines = 2000;
  static constexpr size_t kMaxFileBytes = 1u << 20;

  // `fileRoot` = folder the files API works in ("" = no files API);
  // `fileWrite` = files.write is allowed.
  explicit Vm(Limits limits = {}, std::string fileRoot = "", bool fileWrite = false);
  ~Vm();
  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;

  lua_State* L() { return L_; }
  bool ok() const { return L_ != nullptr; }

  // Runs a text chunk (binary chunks are refused). false + message on error.
  bool exec(const std::string& code, const std::string& chunkName, std::string* err = nullptr);
  // Calls global function `fn` with the `nargs` values on top of the stack;
  // leaves `nres` results on success. Missing function = error.
  bool call(const char* fn, int nargs, int nres, std::string* err = nullptr);
  bool hasFunction(const char* fn);
  // Runs C++ code that reads or writes Lua values (lua_getfield, lua_call…)
  // in protected mode with the call limits: metamethods or errors raised
  // there are reported instead of aborting the process. The `nargs` values on
  // top of the stack are popped and passed in as stack indices 1..nargs.
  bool protect(const std::function<void(lua_State*)>& fn, std::string* err = nullptr, int nargs = 0);

  // Output of print / log (newest last), and an optional live sink.
  const std::vector<std::string>& log() const { return log_; }
  void clearLog() { log_.clear(); }
  std::function<void(const std::string&)> logSink;
  void addLog(const std::string& line);

  const Limits& limits() const { return limits_; }
  void setLimits(const Limits& l) { limits_ = l; }  // memory cap: for later allocations
  uint64_t instructionsUsed() const { return used_; }
  // Work done in C loops (vector operations of the API modules) counts
  // against the instruction budget: `units` are added, the time limit is
  // checked, and a Lua error is raised once a limit is reached (SC-05).
  void charge(lua_State* L, uint64_t units);
  size_t memoryUsed() const { return mem_; }
  // Why the last call was stopped ("" = it was not).
  const std::string& abortReason() const { return abort_; }

  const std::string& fileRoot() const { return fileRoot_; }
  bool fileWrite() const { return fileWrite_; }

  // Free pointer for API modules (controller host, …).
  void* user = nullptr;
  static Vm* from(lua_State* L);

 private:
  static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
  static void hook(lua_State* L, lua_Debug* ar);
  void begin();
  bool finish(int status, int nres, std::string* err);
  void sandbox();

  lua_State* L_ = nullptr;
  Limits limits_;
  std::string fileRoot_;
  bool fileWrite_ = false;
  size_t mem_ = 0;
  uint64_t used_ = 0;
  std::chrono::steady_clock::time_point start_;
  std::string abort_;
  std::vector<std::string> log_;
};

// Lua value helpers for the API modules (stack index `idx`).
double fieldNumber(lua_State* L, int idx, const char* key, double def);
bool fieldBool(lua_State* L, int idx, const char* key, bool def);
std::string fieldString(lua_State* L, int idx, const char* key, const std::string& def);

}  // namespace gl::script

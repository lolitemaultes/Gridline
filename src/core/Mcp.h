// Gridline — MCP server (SC-07): lets AI assistants (Claude Desktop, Claude
// Code, any Model Context Protocol client) inspect and edit a project.
//
// The protocol is JSON-RPC 2.0 (MCP revisions 2024-11-05, 2025-03-26 and
// 2025-06-18): initialize / notifications/initialized, ping, tools/list,
// tools/call, resources/list, resources/read, resources/templates/list,
// prompts/list, prompts/get. Transports live in McpTransport.h (stdio lines
// for `gridline mcp`, Streamable HTTP on 127.0.0.1 inside the app).
//
// Every tool call that changes the project is one undo step labelled
// "AI: <what>" (Document groups), so the app shows it in the history and the
// user can undo it. Tools that change the project or write files run only
// while McpEnvironment::allowEdits is on (the app's "Allow changes" setting).
// Renders, loudness analysis and exports are background jobs (J-61): they
// work on a snapshot of the song, so the app runs them on a worker thread and
// keeps drawing; the reply follows when they finish (with progress
// notifications when the client asks for them), or, with "wait": false, the
// call returns a job number for get_job / cancel_job.
// File paths are confined to the project folder and the chosen export folder;
// there is no tool that runs a shell or reads arbitrary files. Calls are rate
// limited and argument sizes are bounded.
//
// Positions and lengths in tool arguments and results are in quarter-note
// beats (ticks / PPQ). Indices are 1-based (channels, patterns, playlist
// tracks, markers, effect slots, automation clips) except mixer tracks:
// 0 = master, 1.. = inserts, as in the project scripting API.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Document.h"
#include "Json.h"
#include "ProjectScript.h"

namespace gl::mcp {

inline constexpr const char* kLatestProtocol = "2025-06-18";
// Every protocol revision this server speaks, newest first.
const std::vector<std::string>& supportedProtocols();

// JSON-RPC error codes.
enum ErrorCode : int {
  kParseError = -32700,
  kInvalidRequest = -32600,
  kMethodNotFound = -32601,
  kInvalidParams = -32602,
  kInternalError = -32603,
  kRateLimited = -32000,       // too many calls: wait and retry
  kResourceNotFound = -32002,  // MCP: resources/read of an unknown URI
};
// A request other than initialize / ping before initialize: kInvalidRequest.

// ---- JSON helpers ------------------------------------------------------------
// One-line serialization (no newlines: the stdio transport is line framed),
// with escaped keys and \u escapes for control characters.
std::string dump(const Json& v);
// Parses untrusted text: refuses nesting deeper than `maxDepth` and inputs
// larger than `maxBytes` before parsing.
bool parse(const std::string& text, Json& out, size_t maxBytes = 8u << 20, int maxDepth = 64);
Json jbool(bool b);
Json jnum(double v);
Json jstr(const std::string& s);
Json jobj(std::initializer_list<std::pair<const std::string, Json>> kv = {});
Json jarr(std::vector<Json> items = {});

// ---- activity log ----------------------------------------------------------------
struct Activity {
  double time = 0;        // seconds since the epoch
  std::string client;     // clientInfo.name of the session
  std::string tool;       // tool name (or "connect")
  std::string summary;    // short description of the arguments / result
  bool ok = true;
  bool changed = false;   // the call recorded an undo step or wrote a file
};

class ActivityLog {
 public:
  static constexpr size_t kMax = 500;
  void add(Activity a);
  const std::deque<Activity>& entries() const { return v_; }
  void clear() { v_.clear(); }
  uint64_t serial() const { return serial_; }  // increments on every add
  double lastTime() const { return v_.empty() ? 0.0 : v_.back().time; }

 private:
  std::deque<Activity> v_;
  uint64_t serial_ = 0;
};

// Token bucket: `burst` calls at once, refilled at `perSecond`.
class RateLimiter {
 public:
  RateLimiter(double perSecond = 10, double burst = 60) : rate_(perSecond), burst_(burst), tokens_(burst) {}
  bool take(double now);
  void configure(double perSecond, double burst) { rate_ = perSecond, burst_ = burst, tokens_ = std::min(tokens_, burst); }

 private:
  double rate_, burst_, tokens_, last_ = -1;
};

// ---- tool results -------------------------------------------------------------------
struct ToolResult {
  Json data;              // structured result (an object)
  std::string error;      // non-empty: the call failed (isError result)
  bool changed = false;   // wrote a file (project edits are detected from the undo stack)
  std::string summary;    // for the activity log
};

// ---- background jobs (J-61) -----------------------------------------------------------
// A long tool call (render, loudness analysis, export). The work only sees
// what the tool prepared on the calling thread (a song snapshot, resolved
// paths), never the Document.
struct Job {
  uint64_t id = 0;
  std::string tool, title, client, protocol;
  double started = 0;                  // Environment::clock()
  std::atomic<double> progress{0};     // 0..1 (worker)
  std::atomic<bool> cancel{false};     // set by the app's Cancel button / cancel_job
  std::atomic<bool> done{false};       // the worker finished; `result` is valid
  bool finalized = false;              // calling thread: logged, `reply` built
  ToolResult result;
  Json reply;                          // the tools/call result object, once finalized
  std::string status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
  }
  void setStatus(std::string s) {
    std::lock_guard<std::mutex> lk(mu_);
    status_ = std::move(s);
  }

 private:
  mutable std::mutex mu_;
  std::string status_;
};
using JobWork = std::function<ToolResult(Job&)>;

class JobRunner {
 public:
  static constexpr size_t kMaxRunning = 4;   // more at once: the call is refused
  static constexpr size_t kKeepFinished = 32;
  JobRunner() = default;
  JobRunner(const JobRunner&) = delete;
  JobRunner& operator=(const JobRunner&) = delete;
  ~JobRunner() { cancelAll(); }
  // false (CLI, tests): the work runs inside the call, on the calling thread.
  bool background = false;
  // Asked per call when background: true = run this one on the calling thread
  // (the app: hosted plugins whose offline instances need the main thread).
  std::function<bool()> runInline;
  bool useWorker() const { return background && !(runInline && runInline()); }
  // Starts `work` on a worker thread; false when too many jobs are running.
  bool start(const std::shared_ptr<Job>& job, JobWork work);
  // Records a job that already ran inline (so get_job finds it).
  void keep(const std::shared_ptr<Job>& job);
  // Calling thread: joins finished workers and calls `finalize` once per job.
  void poll(const std::function<void(Job&)>& finalize);
  std::shared_ptr<Job> find(uint64_t id) const;
  std::vector<std::shared_ptr<Job>> running() const;  // started, not finalized
  uint64_t nextId() { return ++lastId_; }
  void cancelAll();  // cancels and joins every worker

 private:
  struct Slot {
    std::shared_ptr<Job> job;
    std::thread worker;
  };
  std::vector<Slot> slots_;
  uint64_t lastId_ = 0;
};

// ---- what the tools work on --------------------------------------------------------
struct Environment {
  Environment(Document& d, script::ProjectHost& h) : doc(d), host(h) {}
  Document& doc;
  script::ProjectHost& host;      // project scripts (selection / transport)
  std::string projectPath;        // the project file ("" = not saved yet)
  std::string exportDir;          // the user's export folder ("" = none)
  std::string presetRoot;         // user preset library ("" = factory presets only)
  std::string factoryData;        // FL Studio data folder for FLP export ("" = detect)
  bool allowEdits = true;         // tools that change the project / write files
  bool allowScripts = true;       // run_script (the app's "Run scripts" setting)
  std::string editsOffMessage = "Changes are switched off: enable \"Allow assistants to change the project\" in Gridline's settings.";
  script::Limits scriptLimits;    // run_script budgets
  double maxRenderSeconds = 900;  // longest render / analysis
  size_t maxNotesPerCall = 100000;
  size_t maxScriptBytes = 256u << 10;
  ActivityLog log;
  RateLimiter limiter;
  // Saves the project to `path` (Gridline JSON). Default: saveProject + markSaved.
  std::function<bool(const std::string& path, std::string& err)> save;
  // The song used for rendering / export (default: doc.song()); the app
  // captures hosted plugin states first.
  std::function<Song()> songSnapshot;
  // Called after every logged activity (the app shows a toast / indicator).
  std::function<void(const Activity&)> onActivity;
  // Clock (tests replace it).
  std::function<double()> now;
  // Background jobs (renders, loudness, exports); the app sets background.
  JobRunner jobs;

  Song snapshot() const { return songSnapshot ? songSnapshot() : doc.song(); }
  double clock() const;
  // Folders file writes may go to (canonical, existing): project folder first.
  std::vector<std::string> writeRoots() const;
};

// Resolves a path an assistant asked to write: relative paths are taken from
// the first of `roots`; the result must stay inside one of `roots` (after
// resolving "..", symlinks) and end in one of `exts` (lower case, with dot).
// Creates missing parent folders inside the root. false + err when refused.
bool resolveWritePath(const std::string& requested, const std::vector<std::string>& roots, const std::vector<std::string>& exts,
                      std::string& out, std::string& err);

// ---- tools -----------------------------------------------------------------------

struct Tool {
  std::string name, title, description;
  Json inputSchema;       // JSON Schema object
  bool readOnly = true;   // false: changes the project or writes files
  bool writesFiles = false;
  bool destructive = false;
  bool grouped = true;    // false: the tool manages the undo stack itself (undo / redo)
  std::function<ToolResult(Environment&, const Json& args)> run;
  // Job tools (J-61): prepares on the calling thread (snapshot, paths; "" +
  // err when refused) and returns the work for a worker. `run` then runs
  // both in one go.
  std::function<JobWork(Environment&, const Json& args, std::string& err)> prepare;
};

// The tool catalogue (McpTools.cpp).
const std::vector<Tool>& tools();
const Tool* findTool(const std::string& name);
// Checks `args` against a tool's input schema (types, required, enums,
// ranges, unknown keys). false + message when invalid.
bool validateArgs(const Json& schema, const Json& args, std::string& err);

// Resources: project summary, plugin catalogue, scripting reference.
struct Resource {
  std::string uri, name, title, description, mimeType;
};
std::vector<Resource> resources();
bool readResource(Environment& env, const std::string& uri, std::string& text, std::string& mimeType);
// Project summary as JSON (also the get_song tool).
Json projectSummary(const Environment& env);
// Built-in plugin catalogue as JSON (types, names, kinds, parameters, presets).
Json pluginCatalogJson(const std::string& kind, const std::string& query, bool withParams);

// Calling thread: finished jobs are logged (activity log, onActivity) and
// their tools/call result is built. The HTTP endpoint calls it every poll.
void pollJobs(Environment& env);
// get_job's view of a job (state, progress, status, result once finished).
Json jobJson(const Job& j);

// ---- one protocol session -----------------------------------------------------------
class Session {
 public:
  explicit Session(Environment& env) : env_(env) {}
  // Handles one JSON-RPC message (a request, a notification, a response or a
  // batch). Returns the reply text ("" when nothing is to be sent back).
  std::string handle(const std::string& text);
  // Same on parsed JSON; `reply` is null when nothing is to be sent back.
  Json handleJson(const Json& msg);
  bool initialized() const { return initialized_; }
  const std::string& protocolVersion() const { return protocol_; }
  const std::string& clientName() const { return client_; }

  static Json errorReply(const Json& id, int code, const std::string& message);

  // Deferred replies (J-61): with deferral on, a tools/call that started a
  // background job and waits for it answers nothing from handleJson; the
  // transport takes the pending entry and sends the reply (and progress)
  // when the job is finalized. Off (stdio, tests): such calls run inline.
  struct Pending {
    Json id;
    std::shared_ptr<Job> job;
    Json progressToken;  // null = the client asked for no progress
  };
  void setDeferral(bool on) { defer_ = on; }
  std::vector<Pending> takePending() { return std::move(pending_); }
  // The JSON-RPC reply of a finalized pending call.
  static Json pendingReply(const Pending& p);

 private:
  Json handleOne(const Json& msg);
  Json dispatch(const std::string& method, const Json& params, bool& ok, int& code, std::string& msg);
  Json callTool(const Json& params, bool& ok, int& code, std::string& msg);
  Environment& env_;
  bool initialized_ = false, gotInitialize_ = false;
  std::string protocol_ = kLatestProtocol;
  std::string client_ = "assistant";
  bool defer_ = false;
  std::shared_ptr<Job> deferred_;  // set by callTool for the current request
  std::vector<Pending> pending_;
};

}  // namespace gl::mcp

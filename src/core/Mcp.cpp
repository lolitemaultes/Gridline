// Gridline — MCP protocol session, JSON helpers, path policy, argument
// validation, resources and prompts (see Mcp.h). The tools are in McpTools.cpp.
#include "Mcp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include "Log.h"
#include "Project.h"

namespace gl::mcp {

namespace fs = std::filesystem;

const char* scriptingReferenceText();  // generated from docs/scripting.md (CMake)

const std::vector<std::string>& supportedProtocols() {
  static const std::vector<std::string> v = {"2025-06-18", "2025-03-26", "2024-11-05"};
  return v;
}

// ================================================================== JSON ====

namespace {
void dumpString(const std::string& s, std::string& out) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char esc[8];
          std::snprintf(esc, sizeof esc, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += esc;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}
void dumpValue(const Json& v, std::string& out) {
  switch (v.type) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Json::Type::Number: {
      char buf[40];
      if (!std::isfinite(v.number))
        std::snprintf(buf, sizeof buf, "null");
      else if (v.number == std::floor(v.number) && std::fabs(v.number) < 9.0e15)
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.number));
      else
        std::snprintf(buf, sizeof buf, "%.10g", v.number);
      out += buf;
      break;
    }
    case Json::Type::String: dumpString(v.str, out); break;
    case Json::Type::Array: {
      out += '[';
      for (size_t i = 0; i < v.array.size(); ++i) {
        if (i) out += ',';
        dumpValue(v.array[i], out);
      }
      out += ']';
      break;
    }
    case Json::Type::Object: {
      out += '{';
      bool first = true;
      for (const auto& [k, val] : v.object) {
        if (!first) out += ',';
        first = false;
        dumpString(k, out);
        out += ':';
        dumpValue(val, out);
      }
      out += '}';
      break;
    }
  }
}
}  // namespace

std::string dump(const Json& v) {
  std::string out;
  dumpValue(v, out);
  return out;
}

bool parse(const std::string& text, Json& out, size_t maxBytes, int maxDepth) {
  if (text.size() > maxBytes) return false;
  // The parser recurses per nesting level: bound the depth first.
  int depth = 0;
  bool inStr = false, esc = false;
  for (char c : text) {
    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '[' || c == '{') {
      if (++depth > maxDepth) return false;
    } else if (c == ']' || c == '}') {
      --depth;
    }
  }
  out = Json();
  return jsonParse(text, out);
}

Json jbool(bool b) {
  Json j;
  j.type = Json::Type::Bool;
  j.boolean = b;
  return j;
}
Json jnum(double v) { return Json::makeNumber(v); }
Json jstr(const std::string& s) { return Json::makeString(s); }
Json jobj(std::initializer_list<std::pair<const std::string, Json>> kv) {
  Json j = Json::makeObject();
  for (const auto& [k, v] : kv) j.object[k] = v;
  return j;
}
Json jarr(std::vector<Json> items) {
  Json j = Json::makeArray();
  j.array = std::move(items);
  return j;
}

// ========================================================= log, limiter ====

void ActivityLog::add(Activity a) {
  v_.push_back(std::move(a));
  while (v_.size() > kMax) v_.pop_front();
  ++serial_;
}

bool RateLimiter::take(double now) {
  if (last_ < 0) last_ = now;
  tokens_ = std::min(burst_, tokens_ + std::max(0.0, now - last_) * rate_);
  last_ = now;
  if (tokens_ < 1.0) return false;
  tokens_ -= 1.0;
  return true;
}

double Environment::clock() const {
  if (now) return now();
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::vector<std::string> Environment::writeRoots() const {
  std::vector<std::string> v;
  std::error_code ec;
  auto add = [&](const fs::path& p) {
    if (p.empty()) return;
    fs::path c = fs::weakly_canonical(fs::absolute(p, ec), ec);
    if (ec || !fs::is_directory(c, ec)) return;
    for (const auto& s : v)
      if (s == c.string()) return;
    v.push_back(c.string());
  };
  if (!projectPath.empty()) add(fs::path(projectPath).parent_path().empty() ? fs::path(".") : fs::path(projectPath).parent_path());
  if (!exportDir.empty()) {
    fs::create_directories(exportDir, ec);
    add(exportDir);
  }
  return v;
}

// ============================================================ path policy ====

namespace {
bool within(const fs::path& p, const fs::path& root) {
  auto a = p.begin(), b = root.begin();
  for (; b != root.end(); ++a, ++b) {
    if (b->empty()) continue;  // trailing separator
    if (a == p.end() || *a != *b) return false;
  }
  return a != p.end();
}
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

bool resolveWritePath(const std::string& requested, const std::vector<std::string>& roots, const std::vector<std::string>& exts,
                      std::string& out, std::string& err) {
  if (roots.empty()) {
    err = "no folder is available for files: save the project first, or choose an export folder in Gridline's settings";
    return false;
  }
  if (requested.empty() || requested.size() > 1024 || requested.find('\0') != std::string::npos) {
    err = "invalid path";
    return false;
  }
  std::error_code ec;
  fs::path req(requested);
  fs::path full = req.is_absolute() ? req : fs::path(roots.front()) / req;
  fs::path canon = fs::weakly_canonical(full, ec);
  if (ec) {
    err = "invalid path";
    return false;
  }
  bool inside = false;
  for (const auto& r : roots) {
    fs::path rc = fs::weakly_canonical(fs::path(r), ec);
    if (!ec && within(canon, rc)) inside = true;
  }
  if (!inside) {
    std::string list;
    for (const auto& r : roots) list += (list.empty() ? "" : ", ") + r;
    err = "path is outside the folders assistants may write to (" + list + ")";
    return false;
  }
  const std::string name = lower(canon.filename().string());
  bool extOk = exts.empty();
  for (const auto& e : exts)
    if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) extOk = true;
  if (!extOk) {
    std::string list;
    for (const auto& e : exts) list += (list.empty() ? "" : " ") + e;
    err = "the file name must end in " + list;
    return false;
  }
  if (fs::is_symlink(fs::symlink_status(canon, ec)) || fs::is_directory(canon, ec)) {
    err = "the path is a folder or a link";
    return false;
  }
  fs::create_directories(canon.parent_path(), ec);
  // The parent must still resolve inside a root (no link created in between).
  fs::path parent = fs::weakly_canonical(canon.parent_path(), ec);
  bool stillInside = false;
  for (const auto& r : roots) {
    fs::path rc = fs::weakly_canonical(fs::path(r), ec);
    if (!ec && (parent == rc || within(parent, rc))) stillInside = true;
  }
  if (!stillInside || !fs::is_directory(parent, ec)) {
    err = "cannot create the folder for " + canon.string();
    return false;
  }
  out = canon.string();
  return true;
}

// ============================================================ validation ====

namespace {
bool typeMatches(const std::string& t, const Json& v) {
  switch (v.type) {
    case Json::Type::Null: return t == "null";
    case Json::Type::Bool: return t == "boolean";
    case Json::Type::Number: return t == "number" || (t == "integer" && v.number == std::floor(v.number) && std::fabs(v.number) < 9.0e15);
    case Json::Type::String: return t == "string";
    case Json::Type::Array: return t == "array";
    case Json::Type::Object: return t == "object";
  }
  return false;
}
bool validateAt(const Json& schema, const Json& v, const std::string& where, std::string& err) {
  if (const Json* t = schema.find("type")) {
    bool ok = false;
    if (t->is(Json::Type::String)) ok = typeMatches(t->str, v);
    else
      for (const auto& x : t->array) ok = ok || (x.is(Json::Type::String) && typeMatches(x.str, v));
    if (!ok) {
      err = where + ": wrong type (expected " + (t->is(Json::Type::String) ? t->str : std::string("one of the listed types")) + ")";
      return false;
    }
  }
  if (const Json* e = schema.find("enum"); e && e->is(Json::Type::Array)) {
    bool ok = false;
    for (const auto& x : e->array) ok = ok || dump(x) == dump(v);
    if (!ok) {
      std::string list;
      for (const auto& x : e->array) list += (list.empty() ? "" : ", ") + dump(x);
      err = where + ": must be one of " + list;
      return false;
    }
  }
  if (v.is(Json::Type::Number)) {
    if (const Json* m = schema.find("minimum"); m && v.number < m->number) {
      err = where + ": below the minimum " + dump(*m);
      return false;
    }
    if (const Json* m = schema.find("maximum"); m && v.number > m->number) {
      err = where + ": above the maximum " + dump(*m);
      return false;
    }
  }
  if (v.is(Json::Type::String)) {
    if (const Json* m = schema.find("maxLength"); m && static_cast<double>(v.str.size()) > m->number) {
      err = where + ": text is too long (at most " + dump(*m) + " bytes)";
      return false;
    }
  }
  if (v.is(Json::Type::Array)) {
    if (const Json* m = schema.find("maxItems"); m && static_cast<double>(v.array.size()) > m->number) {
      err = where + ": too many items (at most " + dump(*m) + ")";
      return false;
    }
    if (const Json* it = schema.find("items"))
      for (size_t i = 0; i < v.array.size(); ++i)
        if (!validateAt(*it, v.array[i], where + "[" + std::to_string(i) + "]", err)) return false;
  }
  if (v.is(Json::Type::Object)) {
    const Json* props = schema.find("properties");
    if (const Json* req = schema.find("required"))
      for (const auto& r : req->array)
        if (!v.find(r.str)) {
          err = where + ": missing required argument \"" + r.str + "\"";
          return false;
        }
    const Json* addl = schema.find("additionalProperties");
    for (const auto& [k, val] : v.object) {
      const Json* ps = props ? props->find(k) : nullptr;
      if (!ps) {
        if (addl && addl->is(Json::Type::Bool) && !addl->boolean) {
          err = where + ": unknown argument \"" + k + "\"";
          return false;
        }
        continue;
      }
      if (!validateAt(*ps, val, where == "arguments" ? k : where + "." + k, err)) return false;
    }
  }
  return true;
}
}  // namespace

bool validateArgs(const Json& schema, const Json& args, std::string& err) { return validateAt(schema, args, "arguments", err); }

// ============================================================= resources ====

std::vector<Resource> resources() {
  return {
      {"gridline://project/summary", "project-summary", "Project summary",
       "The open project: song settings, channels, patterns, playlist, mixer and markers (JSON).", "application/json"},
      {"gridline://plugins/catalog", "plugin-catalog", "Plugin catalogue",
       "Every built-in instrument, effect and note processor with its type id, parameters and factory presets (JSON).",
       "application/json"},
      {"gridline://docs/scripting", "scripting-reference", "Scripting reference",
       "Gridline's Lua scripting reference (docs/scripting.md): the sandbox and the `project` API used by run_script.",
       "text/markdown"},
  };
}

bool readResource(Environment& env, const std::string& uri, std::string& text, std::string& mimeType) {
  if (uri == "gridline://project/summary") {
    text = jsonStringify(projectSummary(env), 2);
    mimeType = "application/json";
    return true;
  }
  if (uri == "gridline://plugins/catalog") {
    text = jsonStringify(pluginCatalogJson("", "", true), 1);
    mimeType = "application/json";
    return true;
  }
  if (uri == "gridline://docs/scripting") {
    text = scriptingReferenceText();
    mimeType = "text/markdown";
    return true;
  }
  return false;
}

// =============================================================== prompts ====

namespace {
struct PromptArg {
  const char* name;
  const char* description;
  bool required;
};
struct Prompt {
  const char* name;
  const char* title;
  const char* description;
  std::vector<PromptArg> args;
};
const std::vector<Prompt>& prompts() {
  static const std::vector<Prompt> v = {
      {"make_beat", "Make a beat", "Build a short drum and bass pattern in the open project and place it in the playlist.",
       {{"style", "Genre or feel, e.g. \"house\", \"boom bap\", \"drum and bass\"", true},
        {"bars", "Pattern length in bars (default 2)", false}}},
      {"review_mix", "Review the mix", "Inspect the mixer and loudness of the open project and suggest (or make) improvements.", {}},
  };
  return v;
}
std::string promptText(const Prompt& p, const Json& args) {
  const std::string name = p.name;
  if (name == "make_beat") {
    std::string bars = args.text("bars", "2");
    return "In the open Gridline project, make a " + args.text("style", "simple") + " beat " + bars +
           " bars long. First call get_song and list_channels. Add channels with suitable built-in instruments "
           "(list_plugin_catalog kind=instrument) where needed, create a pattern with add_pattern, write drum steps with "
           "set_steps and a bass line with set_notes, place the pattern on the playlist with add_clip, then call "
           "analyze_loudness and report the result. Every call is one undo step the user can undo in Gridline.";
  }
  return "Review the mix of the open Gridline project. Call get_mixer and analyze_loudness, look at channel volumes "
         "(list_channels) and effects, then explain what you would change. Only change parameters (set_parameter, "
         "add_plugin) after describing the change.";
}
}  // namespace

// =============================================================== session ====

Json Session::errorReply(const Json& id, int code, const std::string& message) {
  Json r = jobj({{"jsonrpc", jstr("2.0")}});
  r.object["id"] = id;
  r.object["error"] = jobj({{"code", jnum(code)}, {"message", jstr(message)}});
  return r;
}

std::string Session::handle(const std::string& text) {
  Json msg;
  if (!parse(text, msg)) return dump(errorReply(Json(), kParseError, "Parse error"));
  Json r = handleJson(msg);
  return r.is(Json::Type::Null) ? std::string() : dump(r);
}

Json Session::handleJson(const Json& msg) {
  if (msg.is(Json::Type::Array)) {
    if (msg.array.empty()) return errorReply(Json(), kInvalidRequest, "Invalid Request: empty batch");
    if (msg.array.size() > 100) return errorReply(Json(), kInvalidRequest, "Invalid Request: batch too large");
    Json out = Json::makeArray();
    for (const auto& m : msg.array) {
      Json r = handleOne(m);
      if (!r.is(Json::Type::Null)) out.array.push_back(std::move(r));
    }
    return out.array.empty() ? Json() : out;
  }
  return handleOne(msg);
}

Json Session::handleOne(const Json& msg) {
  if (!msg.is(Json::Type::Object)) return errorReply(Json(), kInvalidRequest, "Invalid Request");
  const Json* idp = msg.find("id");
  const Json* method = msg.find("method");
  if (!method && (msg.find("result") || msg.find("error"))) return Json();  // a response from the client: nothing to do
  Json id = idp ? *idp : Json();
  if (msg.text("jsonrpc") != "2.0" || !method || !method->is(Json::Type::String))
    return errorReply(idp && (id.is(Json::Type::String) || id.is(Json::Type::Number)) ? id : Json(), kInvalidRequest, "Invalid Request");
  if (!idp) {  // notification: never answered
    if (method->str == "notifications/initialized") initialized_ = gotInitialize_;
    return Json();
  }
  if (!id.is(Json::Type::String) && !id.is(Json::Type::Number)) return errorReply(Json(), kInvalidRequest, "Invalid Request: id must be a string or a number");
  Json params = msg.find("params") ? *msg.find("params") : Json::makeObject();
  if (!params.is(Json::Type::Object)) return errorReply(id, kInvalidParams, "Invalid params: params must be an object");
  bool ok = true;
  int code = 0;
  std::string err;
  Json result;
  try {
    result = dispatch(method->str, params, ok, code, err);
  } catch (const std::exception& e) {
    ok = false, code = kInternalError, err = std::string("Internal error: ") + e.what();
  }
  if (!ok) return errorReply(id, code, err);
  Json r = jobj({{"jsonrpc", jstr("2.0")}});
  r.object["id"] = id;
  r.object["result"] = std::move(result);
  return r;
}

Json Session::dispatch(const std::string& method, const Json& params, bool& ok, int& code, std::string& msg) {
  auto fail = [&](int c, const std::string& m) {
    ok = false, code = c, msg = m;
    return Json();
  };
  if (method == "initialize") {
    std::string want = params.text("protocolVersion");
    const auto& sup = supportedProtocols();
    protocol_ = std::find(sup.begin(), sup.end(), want) != sup.end() ? want : std::string(kLatestProtocol);
    if (const Json* ci = params.find("clientInfo")) {
      std::string n = ci->text("name");
      if (!n.empty()) client_ = n.substr(0, 64);
    }
    gotInitialize_ = true;
    Activity a;
    a.time = env_.clock();
    a.client = client_;
    a.tool = "connect";
    a.summary = "protocol " + protocol_;
    env_.log.add(a);
    if (env_.onActivity) env_.onActivity(a);
    logging::info("mcp", client_ + " connected (protocol " + protocol_ + ")");
    Json caps = jobj({{"tools", jobj({{"listChanged", jbool(false)}})},
                      {"resources", jobj({{"subscribe", jbool(false)}, {"listChanged", jbool(false)}})},
                      {"prompts", jobj({{"listChanged", jbool(false)}})}});
    return jobj({{"protocolVersion", jstr(protocol_)},
                 {"capabilities", caps},
                 {"serverInfo", jobj({{"name", jstr("gridline")}, {"title", jstr("Gridline")}, {"version", jstr("0.4.0")}})},
                 {"instructions",
                  jstr("Gridline is a pattern-based digital audio workstation. Tools inspect and edit the open project; every "
                       "editing call is one undo step in Gridline (labelled \"AI: ...\"). Positions and lengths are in "
                       "quarter-note beats. Channel, pattern, playlist track, marker and effect slot numbers start at 1; mixer "
                       "track 0 is the master. Read gridline://docs/scripting before using run_script.")}});
  }
  if (method == "ping") return Json::makeObject();
  if (!gotInitialize_) return fail(kInvalidRequest, "Invalid Request: call initialize first");
  if (method == "tools/list") {
    Json list = Json::makeArray();
    for (const auto& t : tools()) {
      Json ann = jobj({{"title", jstr(t.title)},
                       {"readOnlyHint", jbool(t.readOnly)},
                       {"destructiveHint", jbool(t.destructive)},
                       {"idempotentHint", jbool(t.readOnly)},
                       {"openWorldHint", jbool(false)}});
      Json j = jobj({{"name", jstr(t.name)}, {"description", jstr(t.description)}, {"inputSchema", t.inputSchema}, {"annotations", ann}});
      if (protocol_ >= "2025-06-18") j.object["title"] = jstr(t.title);
      list.array.push_back(std::move(j));
    }
    return jobj({{"tools", list}});
  }
  if (method == "tools/call") return callTool(params, ok, code, msg);
  if (method == "resources/list") {
    Json list = Json::makeArray();
    for (const auto& r : resources())
      list.array.push_back(jobj({{"uri", jstr(r.uri)},
                                 {"name", jstr(r.name)},
                                 {"title", jstr(r.title)},
                                 {"description", jstr(r.description)},
                                 {"mimeType", jstr(r.mimeType)}}));
    return jobj({{"resources", list}});
  }
  if (method == "resources/templates/list") return jobj({{"resourceTemplates", Json::makeArray()}});
  if (method == "resources/read") {
    const std::string uri = params.text("uri");
    if (uri.empty()) return fail(kInvalidParams, "Invalid params: uri is required");
    std::string text, mime;
    if (!readResource(env_, uri, text, mime)) return fail(kResourceNotFound, "Resource not found: " + uri);
    return jobj({{"contents", jarr({jobj({{"uri", jstr(uri)}, {"mimeType", jstr(mime)}, {"text", jstr(text)}})})}});
  }
  if (method == "prompts/list") {
    Json list = Json::makeArray();
    for (const auto& p : prompts()) {
      Json args = Json::makeArray();
      for (const auto& a : p.args)
        args.array.push_back(jobj({{"name", jstr(a.name)}, {"description", jstr(a.description)}, {"required", jbool(a.required)}}));
      list.array.push_back(jobj({{"name", jstr(p.name)}, {"title", jstr(p.title)}, {"description", jstr(p.description)}, {"arguments", args}}));
    }
    return jobj({{"prompts", list}});
  }
  if (method == "prompts/get") {
    const std::string name = params.text("name");
    Json args = params.find("arguments") && params.find("arguments")->is(Json::Type::Object) ? *params.find("arguments") : Json::makeObject();
    for (const auto& p : prompts()) {
      if (name != p.name) continue;
      for (const auto& a : p.args)
        if (a.required && !args.find(a.name)) return fail(kInvalidParams, std::string("Invalid params: missing prompt argument ") + a.name);
      Json m = jobj({{"role", jstr("user")}, {"content", jobj({{"type", jstr("text")}, {"text", jstr(promptText(p, args))}})}});
      return jobj({{"description", jstr(p.description)}, {"messages", jarr({m})}});
    }
    return fail(kInvalidParams, "Invalid params: unknown prompt " + name);
  }
  return fail(kMethodNotFound, "Method not found: " + method);
}

Json Session::callTool(const Json& params, bool& ok, int& code, std::string& msg) {
  auto fail = [&](int c, const std::string& m) {
    ok = false, code = c, msg = m;
    return Json();
  };
  const std::string name = params.text("name");
  const Tool* tool = findTool(name);
  if (!tool) return fail(kInvalidParams, "Unknown tool: " + name);
  Json args = params.find("arguments") ? *params.find("arguments") : Json::makeObject();
  if (args.is(Json::Type::Null)) args = Json::makeObject();
  if (!args.is(Json::Type::Object)) return fail(kInvalidParams, "Invalid params: arguments must be an object");
  std::string verr;
  if (!validateArgs(tool->inputSchema, args, verr)) return fail(kInvalidParams, "Invalid arguments for " + name + ": " + verr);
  const double now = env_.clock();
  if (!env_.limiter.take(now)) return fail(kRateLimited, "Rate limit: too many tool calls, wait a moment and retry");

  Activity act;
  act.time = now;
  act.client = client_;
  act.tool = name;
  ToolResult r;
  if (!tool->readOnly && !env_.allowEdits) {
    r.error = env_.editsOffMessage;
  } else {
    Document& doc = env_.doc;
    const uint64_t serial = doc.history().stateSerial();
    const size_t depth = doc.groupDepth();
    const bool grouped = !tool->readOnly && tool->grouped;
    if (grouped) {
      doc.endGesture();
      doc.beginGroup("AI: " + tool->title);
    }
    try {
      r = tool->run(env_, args);
    } catch (const std::exception& e) {
      r.error = std::string("internal error: ") + e.what();
    }
    if (grouped) {
      while (doc.groupDepth() > depth + 1) doc.endGroup();  // a tool left a group open
      if (r.error.empty())
        doc.endGroup();
      else
        doc.cancelGroup();
      doc.endGesture();
    }
    r.changed = r.changed || doc.history().stateSerial() != serial;
  }
  act.ok = r.error.empty();
  act.changed = r.changed;
  act.summary = act.ok ? r.summary : r.error;
  if (act.summary.size() > 200) act.summary = act.summary.substr(0, 197) + "...";
  env_.log.add(act);
  if (env_.onActivity) env_.onActivity(act);
  if (!tool->readOnly || !act.ok) logging::info("mcp", client_ + ": " + name + (act.ok ? "" : " failed") + ": " + act.summary);

  Json result = Json::makeObject();
  if (!r.error.empty()) {
    result.object["content"] = jarr({jobj({{"type", jstr("text")}, {"text", jstr(r.error)}})});
    result.object["isError"] = jbool(true);
    return result;
  }
  if (!r.data.is(Json::Type::Object)) r.data = jobj({{"result", r.data}});
  result.object["content"] = jarr({jobj({{"type", jstr("text")}, {"text", jstr(dump(r.data))}})});
  result.object["isError"] = jbool(false);
  if (protocol_ >= "2025-06-18") result.object["structuredContent"] = r.data;
  return result;
}

}  // namespace gl::mcp

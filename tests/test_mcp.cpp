// Gridline — SC-07 tests: the MCP server.
// Protocol conformance (initialize handshake and version negotiation, JSON-RPC
// error codes, notifications, batches, tools/list schema validity, resources,
// prompts), every tool on a sample song with one undo step per editing call
// and no change on failure, read-only mode, path restrictions, rate and size
// limits, the loopback HTTP transport (bind address, token, sessions, Host /
// Origin checks, oversized bodies), the attach bridge, and the CLI stdio server.
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
#include "core/Net.h"
#include "core/Platform.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "fixtures/TestSong.h"
#include "core/Document.h"
#include "core/Mcp.h"
#include "core/McpTransport.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/ProjectScript.h"

using namespace gl;
using namespace gl::mcp;
namespace fs = std::filesystem;

namespace {
// `text` written to `path`, opened for reading (a stdin stand-in).
std::FILE* streamOf(const std::string& text, const fs::path& path) {
  {
    std::ofstream os(path, std::ios::binary);
    os << text;
  }
  return std::fopen(path.string().c_str(), "rb");
}
std::string fileText(const fs::path& path) {
  std::ifstream is(path, std::ios::binary);
  std::stringstream ss;
  ss << is.rdbuf();
  return ss.str();
}
}  // namespace

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

fs::path tempDir(const std::string& name) {
  fs::path d = fs::temp_directory_path() / ("gridline_mcp_" + name + "_" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return fs::weakly_canonical(d);
}

Json parseJ(const std::string& s) {
  Json j;
  parse(s, j);
  return j;
}

struct Fixture {
  fs::path dir;
  Document doc;
  script::SimpleProjectHost host{doc};
  Environment env{doc, host};
  Session session{env};
  int nextId = 1;
  double fakeNow = 1000.0;

  explicit Fixture(const std::string& name, bool init = true) {
    dir = tempDir(name);
    Song s = makeTestSong();
    const std::string path = (dir / "song.gridline.json").string();
    saveProject(path, s);
    doc.reset(s);
    env.projectPath = path;
    env.now = [this] { return fakeNow += 0.5; };  // never rate limited unless a test says so
    if (init) {
      rpc("initialize", jobj({{"protocolVersion", jstr("2025-06-18")}, {"capabilities", Json::makeObject()},
                              {"clientInfo", jobj({{"name", jstr("test-client")}, {"version", jstr("1")}})}}));
      session.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    }
  }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  Json rpc(const std::string& method, Json params = Json::makeObject()) {
    Json m = jobj({{"jsonrpc", jstr("2.0")}, {"id", jnum(nextId++)}, {"method", jstr(method)}, {"params", params}});
    return parseJ(session.handle(dump(m)));
  }
  // tools/call: the result object (or the error reply when the call failed at protocol level).
  Json call(const std::string& tool, Json args = Json::makeObject()) {
    Json r = rpc("tools/call", jobj({{"name", jstr(tool)}, {"arguments", args}}));
    if (const Json* res = r.find("result")) return *res;
    return r;
  }
  static bool isError(const Json& res) { return res.find("isError") && res.find("isError")->boolean; }
  static Json data(const Json& res) {
    if (const Json* s = res.find("structuredContent")) return *s;
    return Json();
  }
  static std::string text(const Json& res) {
    const Json* c = res.find("content");
    return c && !c->array.empty() ? c->array[0].text("text") : std::string();
  }
  // An editing call that must succeed as exactly one undo step labelled "AI: ...".
  Json edit(const std::string& tool, Json args, const char* what) {
    const size_t before = doc.history().position();
    Json r = call(tool, std::move(args));
    const bool ok = !isError(r) && r.find("content");
    if (!ok) std::printf("# %s: %s %s\n", what, text(r).c_str(), dump(r).c_str());
    expect(ok, what);
    expect(doc.history().position() == before + 1 && doc.history().undoLabel().rfind("AI: ", 0) == 0,
           (std::string(what) + ": one undo step labelled AI").c_str());
    return data(r);
  }
};

int errorCode(const Json& reply) {
  const Json* e = reply.find("error");
  return e ? static_cast<int>(e->num("code")) : 0;
}

// ============================================================== protocol ====
void protocolTests() {
  Fixture f("proto", false);
  // Before initialize: only initialize and ping.
  expect(errorCode(f.rpc("tools/list")) == kInvalidRequest, "tools/list before initialize: invalid request");
  expect(f.rpc("ping").find("result") != nullptr, "ping before initialize");
  Json init = f.rpc("initialize", jobj({{"protocolVersion", jstr("2025-06-18")},
                                         {"capabilities", Json::makeObject()},
                                         {"clientInfo", jobj({{"name", jstr("conformance")}, {"version", jstr("0")}})}}));
  const Json* res = init.find("result");
  expect(res && res->text("protocolVersion") == "2025-06-18", "initialize: requested version accepted");
  expect(init.text("jsonrpc") == "2.0" && init.find("id") && init.find("id")->number == 3, "initialize: jsonrpc 2.0 reply with the id");
  expect(res && res->find("capabilities") && res->find("capabilities")->find("tools") && res->find("capabilities")->find("resources") &&
             res->find("capabilities")->find("prompts"),
         "initialize: tools, resources, prompts capabilities");
  expect(res && res->find("serverInfo") && res->find("serverInfo")->text("name") == "gridline", "initialize: serverInfo");
  expect(res && !res->text("instructions").empty(), "initialize: instructions");
  expect(f.session.clientName() == "conformance", "client name kept");
  expect(f.session.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(), "notification: no reply");
  expect(f.session.initialized(), "initialized after the notification");
  expect(f.session.handle(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})").empty(), "cancel notification: no reply");
  expect(f.session.handle(R"({"jsonrpc":"2.0","id":99,"result":{}})").empty(), "a response from the client: no reply");

  // Version negotiation.
  {
    Fixture g("proto2", false);
    Json r = g.rpc("initialize", jobj({{"protocolVersion", jstr("1999-01-01")}, {"capabilities", Json::makeObject()}}));
    expect(r.find("result") && r.find("result")->text("protocolVersion") == kLatestProtocol, "unknown version: server proposes its latest");
    Fixture h("proto3", false);
    r = h.rpc("initialize", jobj({{"protocolVersion", jstr("2024-11-05")}, {"capabilities", Json::makeObject()}}));
    expect(r.find("result") && r.find("result")->text("protocolVersion") == "2024-11-05", "2024-11-05 accepted");
    Json c = h.call("get_song");
    expect(!Fixture::isError(c) && !c.find("structuredContent") && !Fixture::text(c).empty(), "2024-11-05: text content, no structuredContent");
  }

  // JSON-RPC errors.
  Json pe = parseJ(f.session.handle("{not json"));
  expect(errorCode(pe) == kParseError && pe.find("id") && pe.find("id")->is(Json::Type::Null), "parse error -32700 with id null");
  expect(errorCode(parseJ(f.session.handle(R"({"id":1,"method":"ping"})"))) == kInvalidRequest, "missing jsonrpc: -32600");
  expect(errorCode(parseJ(f.session.handle(R"({"jsonrpc":"2.0","id":1})"))) == kInvalidRequest, "missing method: -32600");
  expect(errorCode(parseJ(f.session.handle(R"({"jsonrpc":"2.0","id":{"x":1},"method":"ping"})"))) == kInvalidRequest, "object id: -32600");
  expect(errorCode(parseJ(f.session.handle(R"("hello")"))) == kInvalidRequest, "non-object message: -32600");
  expect(errorCode(f.rpc("no/such/method")) == kMethodNotFound, "unknown method: -32601");
  expect(errorCode(parseJ(f.session.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":[1]})"))) == kInvalidParams,
         "params not an object: -32602");
  expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("no_such_tool")}}))) == kInvalidParams, "unknown tool: -32602");
  expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("get_pattern")}, {"arguments", Json::makeObject()}}))) == kInvalidParams,
         "missing required argument: -32602");
  expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("get_pattern")}, {"arguments", jobj({{"pattern", jstr("one")}})}}))) == kInvalidParams,
         "wrong argument type: -32602");
  expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("get_pattern")}, {"arguments", jobj({{"pattern", jnum(1.5)}})}}))) == kInvalidParams,
         "non-integer for an integer: -32602");
  expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("set_song")}, {"arguments", jobj({{"tempo", jnum(5000)}})}}))) == kInvalidParams,
         "argument above the maximum: -32602");
  expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("get_song")}, {"arguments", jobj({{"bogus", jnum(1)}})}}))) == kInvalidParams,
         "unknown argument: -32602");
  expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("export_project")}, {"arguments", jobj({{"format", jstr("mp9")}, {"path", jstr("x")}})}}))) ==
             kInvalidParams,
         "value outside the enum: -32602");
  Json sid = parseJ(f.session.handle(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})"));
  expect(sid.find("id") && sid.find("id")->str == "abc", "string ids echoed");

  // Batches.
  Json batch = parseJ(f.session.handle(
      R"([{"jsonrpc":"2.0","id":10,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"},{"jsonrpc":"2.0","id":11,"method":"tools/list"}])"));
  expect(batch.is(Json::Type::Array) && batch.array.size() == 2, "batch: one reply per request, none for notifications");
  expect(errorCode(parseJ(f.session.handle("[]"))) == kInvalidRequest, "empty batch: -32600");
  expect(f.session.handle(R"([{"jsonrpc":"2.0","method":"notifications/initialized"}])").empty(), "batch of notifications: no reply");

  // tools/list schema validity.
  Json tl = f.rpc("tools/list");
  const Json* list = tl.find("result") ? tl.find("result")->find("tools") : nullptr;
  expect(list && list->array.size() >= 30, "tools/list: at least 30 tools");
  std::regex nameRe("^[a-z][a-z0-9_]{1,63}$");
  bool namesOk = true, schemasOk = true, descOk = true, annOk = true, reqOk = true, emptyOk = true;
  std::vector<std::string> seen;
  static const std::vector<std::string> kTypes = {"object", "array", "string", "integer", "number", "boolean"};
  for (const auto& t : list ? list->array : std::vector<Json>{}) {
    const std::string n = t.text("name");
    if (!std::regex_match(n, nameRe) || std::count(seen.begin(), seen.end(), n)) namesOk = false, std::printf("# bad name %s\n", n.c_str());
    seen.push_back(n);
    if (t.text("description").size() < 20) descOk = false;
    const Json* s = t.find("inputSchema");
    if (!s || s->text("type") != "object" || !s->find("properties") || !s->find("properties")->is(Json::Type::Object)) {
      schemasOk = false;
      continue;
    }
    for (const auto& [k, p] : s->find("properties")->object) {
      const std::string ty = p.text("type");
      if (std::find(kTypes.begin(), kTypes.end(), ty) == kTypes.end() || p.text("description").empty()) {
        schemasOk = false;
        std::printf("# %s.%s: bad property schema\n", n.c_str(), k.c_str());
      }
      if (ty == "array" && !p.find("items")) schemasOk = false;
      if (const Json* mn = p.find("minimum"); mn && p.find("maximum") && mn->number > p.find("maximum")->number) schemasOk = false;
    }
    bool needsArgs = false;
    if (const Json* r = s->find("required")) {
      needsArgs = !r->array.empty();
      for (const auto& x : r->array)
        if (!s->find("properties")->find(x.str)) reqOk = false;
    }
    std::string err;
    if (!needsArgs && !validateArgs(*s, Json::makeObject(), err)) emptyOk = false;
    const Json* an = t.find("annotations");
    const Tool* tool = findTool(n);
    if (!an || !tool || !an->find("readOnlyHint") || an->find("readOnlyHint")->boolean != tool->readOnly) annOk = false;
  }
  expect(namesOk, "tools/list: unique snake_case names");
  expect(descOk, "tools/list: every tool is described");
  expect(schemasOk, "tools/list: input schemas are objects with typed, described properties");
  expect(reqOk, "tools/list: required arguments are declared properties");
  expect(emptyOk, "tools/list: tools without required arguments accept {}");
  expect(annOk, "tools/list: readOnlyHint matches the tool");
  for (const char* must : {"get_song", "list_channels", "list_patterns", "get_pattern", "get_playlist", "get_mixer", "get_parameters", "get_automation",
                           "get_markers", "list_plugin_catalog", "list_presets", "set_song", "add_channel", "edit_channel", "set_notes", "set_steps",
                           "add_clip", "add_automation", "add_plugin", "remove_plugin", "set_parameter", "add_marker", "run_script", "render_audio",
                           "analyze_loudness", "export_project", "save_project", "get_job", "cancel_job"})
    expect(std::count(seen.begin(), seen.end(), std::string(must)) == 1, (std::string("tool ") + must).c_str());

  // Resources.
  Json rl = f.rpc("resources/list");
  const Json* rs = rl.find("result") ? rl.find("result")->find("resources") : nullptr;
  expect(rs && rs->array.size() == 3, "resources/list: project summary, plugin catalogue, scripting reference");
  for (const auto& r : rs ? rs->array : std::vector<Json>{}) {
    Json rr = f.rpc("resources/read", jobj({{"uri", jstr(r.text("uri"))}}));
    const Json* c = rr.find("result") ? rr.find("result")->find("contents") : nullptr;
    expect(c && c->array.size() == 1 && c->array[0].text("uri") == r.text("uri") && !c->array[0].text("text").empty() &&
               c->array[0].text("mimeType") == r.text("mimeType"),
           ("resources/read " + r.text("uri")).c_str());
  }
  Json doc = f.rpc("resources/read", jobj({{"uri", jstr("gridline://docs/scripting")}}));
  expect(contains(dump(doc), "Scripting reference") && contains(dump(doc), "project.song"), "scripting reference is docs/scripting.md");
  Json sum = f.rpc("resources/read", jobj({{"uri", jstr("gridline://project/summary")}}));
  expect(contains(dump(sum), "Gridline Demo"), "project summary names the song");
  expect(errorCode(f.rpc("resources/read", jobj({{"uri", jstr("file:///etc/passwd")}}))) == kResourceNotFound, "unknown resource: -32002");
  expect(f.rpc("resources/templates/list").find("result") != nullptr, "resources/templates/list");

  // Prompts.
  Json pl = f.rpc("prompts/list");
  expect(pl.find("result") && pl.find("result")->find("prompts")->array.size() >= 2, "prompts/list");
  Json pg = f.rpc("prompts/get", jobj({{"name", jstr("make_beat")}, {"arguments", jobj({{"style", jstr("house")}})}}));
  expect(pg.find("result") && contains(dump(pg), "house") && pg.find("result")->find("messages")->array.size() == 1, "prompts/get make_beat");
  expect(errorCode(f.rpc("prompts/get", jobj({{"name", jstr("make_beat")}}))) == kInvalidParams, "prompts/get: missing argument -32602");
}

// ================================================================= tools ====
std::string effectWithPreset(std::string* preset) {
  for (const auto& p : pluginCatalog())
    if (p.kind == PluginKind::Effect && p.type.rfind("gl.", 0) == 0 && !p.presets.empty()) {
      *preset = p.presets.back().name;
      return p.type;
    }
  return "gl.reverb";
}

void toolTests() {
  Fixture f("tools");
  const Song& s = f.doc.song();
  // ---- inspect
  Json song = Fixture::data(f.call("get_song"));
  expect(song.num("tempo") == s.tempo && song.text("title") == s.info.title, "get_song");
  expect(song.find("counts") && song.find("counts")->num("channels") == static_cast<double>(s.channels.size()), "get_song: counts");
  Json ch = Fixture::data(f.call("list_channels"));
  expect(ch.find("channels") && ch.find("channels")->array.size() == s.channels.size() && ch.find("channels")->array[0].text("name") == s.channels[0].name,
         "list_channels");
  Json pats = Fixture::data(f.call("list_patterns"));
  expect(pats.find("patterns") && pats.find("patterns")->array.size() == s.patterns.size(), "list_patterns");
  Json p1 = Fixture::data(f.call("get_pattern", jobj({{"pattern", jnum(1)}})));
  expect(p1.find("channels") && !p1.find("channels")->array.empty(), "get_pattern: channels with data");
  Json pl = Fixture::data(f.call("get_playlist"));
  expect(pl.num("totalClips") == static_cast<double>(s.playlist.size()), "get_playlist: every clip");
  expect(pl.find("clips") && !pl.find("clips")->array.empty() && pl.find("clips")->array[0].num("clip") > 0, "get_playlist: clip ids");
  Json mx = Fixture::data(f.call("get_mixer"));
  expect(mx.find("tracks") && mx.find("tracks")->array[0].num("mixer", -1) == 0, "get_mixer: master first");
  Json pc = Fixture::data(f.call("get_parameters", jobj({{"channel", jnum(1)}})));
  expect(pc.find("params") && pc.find("params")->array.size() > 4 && pc.find("params")->array[0].text("param") == "volume", "get_parameters: channel");
  Json ps = Fixture::data(f.call("get_parameters"));
  expect(ps.find("params") && ps.find("params")->array[0].text("param") == "tempo", "get_parameters: song");
  Json pq = Fixture::data(f.call("get_parameters", jobj({{"mixer", jnum(0)}, {"query", jstr("eq")}})));
  expect(pq.num("total") == 9, "get_parameters: mixer track with a query");
  expect(Fixture::isError(f.call("get_parameters", jobj({{"mixer", jnum(1)}, {"slot", jnum(10)}}))), "get_parameters: empty slot is an error");
  expect(!Fixture::isError(f.call("get_automation")), "get_automation");
  expect(!Fixture::isError(f.call("get_markers")), "get_markers");
  Json cat = Fixture::data(f.call("list_plugin_catalog", jobj({{"kind", jstr("effect")}})));
  bool allFx = cat.find("plugins") && !cat.find("plugins")->array.empty();
  for (const auto& p : cat.find("plugins") ? cat.find("plugins")->array : std::vector<Json>{}) allFx = allFx && p.text("kind") == "effect";
  expect(allFx, "list_plugin_catalog: kind filter");
  Json catq = Fixture::data(f.call("list_plugin_catalog", jobj({{"query", jstr("gl.synth")}, {"includeParams", jbool(true)}})));
  expect(catq.find("plugins") && !catq.find("plugins")->array.empty() && catq.find("plugins")->array[0].find("params"), "list_plugin_catalog: params");
  Json pr = Fixture::data(f.call("list_presets", jobj({{"type", jstr("gl.kick")}})));
  expect(pr.find("factory") && contains(dump(*pr.find("factory")), "Deep 808"), "list_presets: factory presets");
  expect(Fixture::isError(f.call("list_presets", jobj({{"type", jstr("gl.nope")}}))), "list_presets: unknown type");
  expect(!Fixture::isError(f.call("get_history")), "get_history");
  expect(f.doc.history().position() == 0, "reading changes nothing");

  // ---- edit: one undo step each
  f.edit("set_song", jobj({{"tempo", jnum(133)}, {"numerator", jnum(3)}, {"denominator", jnum(4)}, {"title", jstr("MCP")}}), "set_song");
  expect(s.tempo == 133 && s.timeSigNum == 3 && s.info.title == "MCP", "set_song: values");
  const size_t nch = s.channels.size();
  Json added = f.edit("add_channel", jobj({{"name", jstr("Boom")}, {"plugin", jstr("gl.kick")}, {"preset", jstr("Deep 808")}, {"volume", jnum(0.5)}}),
                      "add_channel with preset and volume");
  expect(s.channels.size() == nch + 1 && s.channels.back().name == "Boom" && std::fabs(s.channels.back().gain - 0.5f) < 1e-6, "add_channel: values");
  expect(s.channels.back().plugin == presetState(*findPlugin("gl.kick"), 1), "add_channel: preset loaded");
  const int newCh = static_cast<int>(added.num("channel"));
  expect(newCh == static_cast<int>(nch + 1), "add_channel: returns its number");
  f.edit("edit_channel", jobj({{"channel", jnum(newCh)}, {"pan", jnum(-0.5)}, {"name", jstr("Boom 2")}, {"mixer", jnum(3)}, {"color", jstr("#112233")}}),
         "edit_channel");
  expect(s.channels.back().pan == -0.5f && s.channels.back().name == "Boom 2" && s.channels.back().mixerTrack == s.mixer[3].id &&
             s.channels.back().color == 0x112233,
         "edit_channel: values");
  Json ap = f.edit("add_pattern", jobj({{"name", jstr("AI beat")}, {"bars", jnum(2)}}), "add_pattern");
  const int pat = static_cast<int>(ap.num("pattern"));
  expect(pat == static_cast<int>(s.patterns.size()) && s.patterns.back().bars == 2, "add_pattern: values");
  f.edit("edit_pattern", jobj({{"pattern", jnum(pat)}, {"bars", jnum(4)}, {"name", jstr("AI beat 2")}}), "edit_pattern");
  expect(s.patterns.back().bars == 4 && s.patterns.back().name == "AI beat 2", "edit_pattern: values");
  Json notes = Json::makeArray();
  for (int i = 0; i < 3; ++i) notes.array.push_back(jobj({{"start", jnum(i)}, {"length", jnum(0.5)}, {"pitch", jnum(60 + i)}, {"velocity", jnum(90)}}));
  f.edit("set_notes", jobj({{"pattern", jnum(pat)}, {"channel", jnum(newCh)}, {"notes", notes}}), "set_notes replace");
  const ObjectId pid = s.patterns.back().id, cid = s.channels.back().id;
  expect(s.pattern(pid)->notes.at(cid).size() == 3 && s.pattern(pid)->notes.at(cid)[1].start == s.ppq && s.pattern(pid)->notes.at(cid)[1].length == s.ppq / 2,
         "set_notes: beats converted to ticks");
  f.edit("set_notes", jobj({{"pattern", jnum(pat)}, {"channel", jnum(newCh)}, {"mode", jstr("add")}, {"notes", jarr({jobj({{"start", jnum(3)}, {"pitch", jnum(72)}})})}}),
         "set_notes add");
  expect(s.pattern(pid)->notes.at(cid).size() == 4, "set_notes add keeps the others");
  Json pn = Fixture::data(f.call("get_pattern", jobj({{"pattern", jnum(pat)}, {"channel", jnum(newCh)}})));
  expect(contains(dump(pn), "\"pitch\":72"), "get_pattern reads them back");
  Json steps = Json::makeArray();
  for (int i = 0; i < 16; ++i) steps.array.push_back(jnum(i % 4 == 0 ? 100 : 0));
  f.edit("set_steps", jobj({{"pattern", jnum(pat)}, {"channel", jnum(newCh)}, {"steps", steps}}), "set_steps");
  expect(s.pattern(pid)->steps.at(cid)[4] == 100 && s.pattern(pid)->steps.at(cid)[5] == 0, "set_steps: values");
  Json clip = f.edit("add_clip", jobj({{"pattern", jnum(pat)}, {"track", jnum(5)}, {"start", jnum(16)}}), "add_clip");
  const ObjectId clipId = static_cast<ObjectId>(clip.num("clip"));
  expect(s.clip(clipId) && s.clip(clipId)->track == 4 && s.clip(clipId)->start == 16 * s.ppq, "add_clip: placed");
  f.edit("edit_clip", jobj({{"clip", jnum(clipId)}, {"start", jnum(32)}, {"track", jnum(6)}, {"muted", jbool(true)}}), "edit_clip");
  expect(s.clip(clipId)->start == 32 * s.ppq && s.clip(clipId)->track == 5 && s.clip(clipId)->muted, "edit_clip: values");
  f.edit("remove_clip", jobj({{"clip", jnum(clipId)}}), "remove_clip");
  expect(!s.clip(clipId), "remove_clip: gone");
  const size_t autos = s.automations.size(), clips = s.playlist.size();
  Json au = f.edit("add_automation",
                   jobj({{"channel", jnum(newCh)}, {"param", jstr("volume")}, {"track", jnum(7)}, {"start", jnum(0)},
                         {"points", jarr({jobj({{"position", jnum(0)}, {"value", jnum(0)}}), jobj({{"position", jnum(8)}, {"value", jnum(1)}})})}}),
                   "add_automation with a placement (one step for both)");
  expect(s.automations.size() == autos + 1 && s.playlist.size() == clips + 1 && s.automations.back().points.size() == 2 &&
             s.automations.back().points[1].pos == 8 * s.ppq,
         "add_automation: clip, points and placement");
  std::string fxPreset;
  const std::string fx = effectWithPreset(&fxPreset);
  Json pg = f.edit("add_plugin", jobj({{"mixer", jnum(2)}, {"plugin", jstr(fx)}, {"preset", jstr(fxPreset)}}), "add_plugin with preset");
  const int slot = static_cast<int>(pg.num("slot"));
  expect(slot >= 1 && s.mixer[2].slots[static_cast<size_t>(slot - 1)].plugin.type == fx, "add_plugin: in the first free slot");
  Json sp = f.edit("set_parameter", jobj({{"mixer", jnum(2)}, {"slot", jnum(slot)}, {"param", jstr("mix")}, {"value", jnum(0.25)}}), "set_parameter slot mix");
  expect(std::fabs(s.mixer[2].slots[static_cast<size_t>(slot - 1)].mix - 0.25f) < 1e-6, "set_parameter: slot mix");
  f.edit("set_parameter", jobj({{"mixer", jnum(2)}, {"slot", jnum(slot)}, {"param", jstr("1")}, {"value", jnum(1)}, {"normalized", jbool(true)}}),
         "set_parameter by number, normalized");
  {
    const ParamDesc& d = findPlugin(fx)->params[0];
    expect(std::fabs(s.mixer[2].slots[static_cast<size_t>(slot - 1)].plugin.params[0] - static_cast<float>(d.fromNormalized(1.0))) < 1e-4,
           "set_parameter: normalized value mapped");
  }
  f.edit("set_parameter", jobj({{"param", jstr("tempo")}, {"value", jnum(101)}}), "set_parameter song tempo");
  expect(s.tempo == 101, "set_parameter: tempo");
  f.edit("set_parameter", jobj({{"param", jstr("tempo")}, {"value", jnum(102)}}), "set_parameter again: a second step (no merge)");
  f.edit("set_parameter", jobj({{"channel", jnum(newCh)}, {"param", jstr(findPlugin("gl.kick")->params[0].name)}, {"value", jnum(findPlugin("gl.kick")->params[0].min)}}),
         "set_parameter by the plugin parameter's name");
  expect(Fixture::isError(f.call("set_parameter", jobj({{"channel", jnum(newCh)}, {"param", jstr("nonsense")}, {"value", jnum(1)}}))),
         "set_parameter: unknown name is an error");
  f.edit("edit_mixer_track", jobj({{"mixer", jnum(2)}, {"name", jstr("FX bus")}, {"volume", jnum(0.6)}, {"routeTo", jnum(3)}}), "edit_mixer_track");
  expect(s.mixer[2].name == "FX bus" && s.mixer[2].sends.size() == 1 && s.mixer[2].sends[0].to == s.mixer[3].id, "edit_mixer_track: values");
  expect(Fixture::isError(f.call("edit_mixer_track", jobj({{"mixer", jnum(3)}, {"routeTo", jnum(2)}}))), "edit_mixer_track: a loop is refused");
  f.edit("remove_plugin", jobj({{"mixer", jnum(2)}, {"slot", jnum(slot)}}), "remove_plugin");
  expect(s.mixer[2].slots[static_cast<size_t>(slot - 1)].plugin.empty(), "remove_plugin: slot empty");
  const size_t marks = s.markers.size();
  f.edit("add_marker", jobj({{"position", jnum(16)}, {"name", jstr("Drop")}}), "add_marker");
  expect(s.markers.size() == marks + 1, "add_marker: added");
  int mk = 0;
  for (size_t i = 0; i < s.markers.size(); ++i)
    if (s.markers[i].name == "Drop") mk = static_cast<int>(i + 1);
  f.edit("remove_marker", jobj({{"marker", jnum(mk)}}), "remove_marker");
  expect(s.markers.size() == marks, "remove_marker: removed");
  Json sc = f.edit("run_script", jobj({{"code", jstr("project.song.setTempo(120)\nprint('hi from lua')")}}), "run_script");
  expect(s.tempo == 120 && contains(dump(sc), "hi from lua"), "run_script: edits and log");
  f.edit("remove_pattern", jobj({{"pattern", jnum(pat)}}), "remove_pattern");
  f.edit("remove_channel", jobj({{"channel", jnum(newCh)}}), "remove_channel");
  expect(s.channels.size() == nch, "remove_channel: removed");

  // ---- failures change nothing
  {
    const Song before = s;
    const size_t pos = f.doc.history().position();
    Json r = f.call("set_notes", jobj({{"pattern", jnum(999)}, {"channel", jnum(1)}, {"notes", Json::makeArray()}}));
    expect(Fixture::isError(r) && contains(Fixture::text(r), "no pattern"), "bad pattern: tool error");
    r = f.call("add_channel", jobj({{"plugin", jstr("gl.reverb")}}));
    expect(Fixture::isError(r) && contains(Fixture::text(r), "effect"), "an effect as an instrument: tool error");
    r = f.call("add_channel", jobj({{"plugin", jstr("gl.kick")}, {"preset", jstr("No such preset")}}));
    expect(Fixture::isError(r), "missing preset: tool error");
    r = f.call("run_script", jobj({{"code", jstr("project.song.setTempo(90)\nerror('stop')")}}));
    expect(Fixture::isError(r) && contains(Fixture::text(r), "stop"), "failing script: error with message");
    r = f.call("run_script", jobj({{"code", jstr("while true do end")}}));
    expect(Fixture::isError(r) && contains(Fixture::text(r), "limit"), "endless script stopped by the budget");
    expect(f.doc.history().position() == pos && f.doc.song() == before, "failed calls: no undo step, project unchanged (partial edits rolled back)");
  }

  // ---- undo / redo through the server, only for assistant steps
  {
    const double t0 = s.tempo;
    f.edit("set_song", jobj({{"tempo", jnum(140)}}), "set_song for undo");
    Json u = f.call("undo");
    expect(!Fixture::isError(u) && s.tempo == t0, "undo: reverts the assistant step");
    Json r = f.call("redo");
    expect(!Fixture::isError(r) && s.tempo == 140, "redo");
    f.doc.renameChannel(s.channels[0].id, "User edit");
    expect(Fixture::isError(f.call("undo")) && s.channels[0].name == "User edit", "undo refuses a user's step");
    // Every assistant step undoes with the app's own undo too.
    f.doc.undo();
    f.doc.undo();
    expect(s.tempo == t0, "the app's undo reverts assistant steps one per call");
  }
}

// ========================================================== audio, files ====
void fileTests() {
  Fixture f("files");
  Json lo = Fixture::data(f.call("analyze_loudness", jobj({{"maxSeconds", jnum(4)}})));
  expect(std::isfinite(lo.num("lufs")) && lo.num("lufs") > -60 && lo.num("lufs") < 0 && lo.num("seconds") > 3 && lo.num("seconds") < 16,
         "analyze_loudness: song");
  Json lp = Fixture::data(f.call("analyze_loudness", jobj({{"scope", jstr("pattern")}, {"pattern", jnum(1)}})));
  expect(lp.num("seconds") > 0 && lp.find("peakDb"), "analyze_loudness: pattern");
  Json rr = f.call("render_audio", jobj({{"path", jstr("renders/mix.wav")}, {"maxSeconds", jnum(2)}, {"bits", jnum(16)}}));
  const fs::path wav = f.dir / "renders" / "mix.wav";
  expect(!Fixture::isError(rr) && fs::exists(wav) && fs::file_size(wav) > 44 * 1000, "render_audio: WAV in the project folder");
  {
    std::ifstream is(wav, std::ios::binary);
    char hdr[4] = {};
    is.read(hdr, 4);
    expect(std::string(hdr, 4) == "RIFF", "render_audio: RIFF header");
  }
  expect(f.doc.history().position() == 0, "render: no undo step");
  for (auto [fmt, name] : std::vector<std::pair<std::string, std::string>>{{"midi", "song.mid"}, {"flp", "song.flp"}, {"flp_zip", "pack.zip"}}) {
    Json r = f.call("export_project", jobj({{"format", jstr(fmt)}, {"path", jstr("exports/" + name)}}));
    if (Fixture::isError(r)) std::printf("# %s: %s\n", fmt.c_str(), Fixture::text(r).c_str());
    expect(!Fixture::isError(r) && fs::exists(f.dir / "exports" / name) && fs::file_size(f.dir / "exports" / name) > 20, ("export_project " + fmt).c_str());
  }
  Json pm = f.call("export_project", jobj({{"format", jstr("midi")}, {"path", jstr("p1.mid")}, {"pattern", jnum(1)}}));
  expect(!Fixture::isError(pm) && fs::exists(f.dir / "p1.mid"), "export_project: one pattern as MIDI");
  expect(Fixture::isError(f.call("export_project", jobj({{"format", jstr("midi")}, {"path", jstr("song.wav")}}))), "export: wrong extension refused");

  // save
  f.edit("set_song", jobj({{"tempo", jnum(111)}}), "set_song before save");
  expect(f.doc.modified(), "modified before save");
  Json sv = f.call("save_project");
  expect(!Fixture::isError(sv) && !f.doc.modified(), "save_project: saved");
  Song back;
  expect(loadProject((f.dir / "song.gridline.json").string(), back) && back.tempo == 111, "save_project: file has the change");
  Json sa = f.call("save_project", jobj({{"path", jstr("versions/v2.gridline.json")}}));
  expect(!Fixture::isError(sa) && fs::exists(f.dir / "versions" / "v2.gridline.json") && f.env.projectPath == (f.dir / "versions" / "v2.gridline.json").string(),
         "save_project: save as inside the project folder");
  expect(Fixture::isError(f.call("save_project", jobj({{"path", jstr("../../escape.gridline.json")}}))), "save_project: outside refused");
  expect(Fixture::isError(f.call("save_project", jobj({{"path", jstr("notes.txt")}}))), "save_project: non-project extension refused");
  expect(Fixture::isError(f.call("render_audio", jobj({{"path", jstr("/etc/x.wav")}}))), "render_audio: absolute path outside refused");

  // Export folder as a second root; unsaved project without folders.
  {
    const fs::path ex = tempDir("exportdir");
    f.env.exportDir = ex.string();
    Json r = f.call("render_audio", jobj({{"path", jstr("x.wav")}, {"maxSeconds", jnum(1)}}));
    expect(!Fixture::isError(r) && fs::exists(ex / "x.wav"), "relative paths go to the export folder when there is one");
    const fs::path projDir = fs::path(f.env.projectPath).parent_path();  // versions/ since the save as
    r = f.call("render_audio", jobj({{"path", jstr((projDir / "y.wav").string())}, {"maxSeconds", jnum(1)}}));
    expect(!Fixture::isError(r) && fs::exists(projDir / "y.wav"), "absolute path in the project folder allowed");
    std::error_code ec;
    fs::remove_all(ex, ec);
    f.env.exportDir.clear();
    f.env.projectPath.clear();
    r = f.call("render_audio", jobj({{"path", jstr("z.wav")}}));
    expect(Fixture::isError(r) && contains(Fixture::text(r), "save the project first"), "no folders: writes refused");
    expect(Fixture::isError(f.call("save_project")), "unsaved project: save needs a path");
  }
}

// ========================================================== path policy ====
void pathTests() {
  const fs::path root = tempDir("paths"), other = tempDir("paths_other");
  std::string out, err;
  const std::vector<std::string> roots = {root.string()};
  expect(resolveWritePath("a.wav", roots, {".wav"}, out, err) && out == (root / "a.wav").string(), "relative path inside");
  expect(resolveWritePath("sub/dir/a.wav", roots, {".wav"}, out, err) && fs::is_directory(root / "sub" / "dir"), "subfolders are created inside");
  expect(!resolveWritePath("../a.wav", roots, {".wav"}, out, err), "parent escape refused");
  expect(!resolveWritePath("sub/../../a.wav", roots, {".wav"}, out, err), "nested escape refused");
  expect(!resolveWritePath((other / "a.wav").string(), roots, {".wav"}, out, err), "absolute path elsewhere refused");
  expect(resolveWritePath((root / "b.wav").string(), roots, {".wav"}, out, err), "absolute path inside allowed");
  expect(!resolveWritePath("a.exe", roots, {".wav"}, out, err), "extension enforced");
  expect(!resolveWritePath("A.WAV.sh", roots, {".wav"}, out, err), "extension must be the end");
  expect(resolveWritePath("Upper.WAV", roots, {".wav"}, out, err), "extension case-insensitive");
  expect(!resolveWritePath("", roots, {".wav"}, out, err), "empty path refused");
  expect(!resolveWritePath(std::string("a\0b.wav", 7), roots, {".wav"}, out, err), "NUL refused");
  expect(!resolveWritePath("a.wav", {}, {".wav"}, out, err), "no roots: refused");
  expect(!resolveWritePath(root.string(), roots, {}, out, err), "the root itself refused");
  std::error_code ec;
  fs::create_directory_symlink(other, root / "link", ec);
  expect(!ec && !resolveWritePath("link/a.wav", roots, {".wav"}, out, err), "symlinked folder pointing outside refused");
  fs::create_symlink(other / "missing.wav", root / "dangling.wav", ec);
  expect(!ec && !resolveWritePath("dangling.wav", roots, {".wav"}, out, err), "dangling file symlink refused");
  fs::create_directories(root / "isdir.wav");
  expect(!resolveWritePath("isdir.wav", roots, {".wav"}, out, err), "a folder refused");
  expect(resolveWritePath("c.wav", {root.string(), other.string()}, {".wav"}, out, err) && out == (root / "c.wav").string(), "first root for relative paths");
  expect(resolveWritePath((other / "c.wav").string(), {root.string(), other.string()}, {".wav"}, out, err), "second root allowed");
  fs::remove_all(root, ec);
  fs::remove_all(other, ec);
}

// ============================================================= safety ====
void safetyTests() {
  {  // read-only (the app's default until the user allows changes)
    Fixture f("ro");
    f.env.allowEdits = false;
    const Song before = f.doc.song();
    Json r = f.call("set_song", jobj({{"tempo", jnum(99)}}));
    expect(Fixture::isError(r) && contains(Fixture::text(r), "switched off"), "read-only: edits refused with an explanation");
    r = f.call("render_audio", jobj({{"path", jstr("x.wav")}}));
    expect(Fixture::isError(r) && !fs::exists(f.dir / "x.wav"), "read-only: no files written");
    r = f.call("run_script", jobj({{"code", jstr("project.song.setTempo(99)")}}));
    expect(Fixture::isError(r), "read-only: scripts refused");
    expect(f.doc.song() == before && f.doc.history().position() == 0, "read-only: project unchanged");
    expect(!Fixture::isError(f.call("get_song")) && !Fixture::isError(f.call("analyze_loudness", jobj({{"maxSeconds", jnum(1)}}))), "read-only: reading works");
    const auto& log = f.env.log.entries();
    expect(!log.empty() && log.front().tool == "connect" && std::any_of(log.begin(), log.end(), [](const Activity& a) { return a.tool == "set_song" && !a.ok; }),
           "activity log records connections and refused calls");
    f.env.allowEdits = true;
    f.env.allowScripts = false;
    expect(Fixture::isError(f.call("run_script", jobj({{"code", jstr("print(1)")}}))), "scripts switched off: run_script refused");
  }
  {  // activity log of changes
    Fixture f("log");
    f.call("set_song", jobj({{"tempo", jnum(100)}}));
    const Activity& a = f.env.log.entries().back();
    expect(a.tool == "set_song" && a.ok && a.changed && a.client == "test-client" && contains(a.summary, "tempo"), "activity: tool, client, summary, changed");
    f.call("get_song");
    expect(!f.env.log.entries().back().changed, "activity: reads are not changes");
  }
  {  // rate limit
    Fixture f("rate");
    double now = 5000;
    f.env.now = [&now] { return now; };
    f.env.limiter.configure(1, 3);
    int ok = 0, limited = 0;
    for (int i = 0; i < 5; ++i) {
      Json r = f.rpc("tools/call", jobj({{"name", jstr("get_song")}}));
      if (r.find("result")) ++ok;
      if (errorCode(r) == kRateLimited) ++limited;
    }
    expect(ok == 3 && limited == 2, "rate limit: burst then -32000");
    now += 2.0;
    expect(f.rpc("tools/call", jobj({{"name", jstr("get_song")}})).find("result") != nullptr, "rate limit: refills over time");
  }
  {  // size limits
    Fixture f("size");
    std::string deep(200, '[');
    deep += std::string(200, ']');
    expect(errorCode(parseJ(f.session.handle(deep))) == kParseError, "nesting depth limited");
    Json big;
    expect(!parse(std::string(100, ' '), big, 50), "byte limit");
    f.env.maxNotesPerCall = 5;
    Json notes = Json::makeArray();
    for (int i = 0; i < 6; ++i) notes.array.push_back(jobj({{"start", jnum(i)}, {"pitch", jnum(60)}}));
    expect(Fixture::isError(f.call("set_notes", jobj({{"pattern", jnum(1)}, {"channel", jnum(1)}, {"notes", notes}}))), "notes per call limited");
    f.env.maxScriptBytes = 10;
    expect(Fixture::isError(f.call("run_script", jobj({{"code", jstr("print('a long script')")}}))), "script size limited");
    expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("set_song")}, {"arguments", jobj({{"title", jstr(std::string(300, 'x'))}})}}))) == kInvalidParams,
           "string length limited by the schema");
    Json manySteps = Json::makeArray();
    for (int i = 0; i < 5000; ++i) manySteps.array.push_back(jnum(1));
    expect(errorCode(f.rpc("tools/call", jobj({{"name", jstr("set_steps")}, {"arguments", jobj({{"pattern", jnum(1)}, {"channel", jnum(1)}, {"steps", manySteps}})}}))) ==
               kInvalidParams,
           "array size limited by the schema");
  }
}

// ================================================================ jobs ====
// J-61: renders, loudness and exports run as background jobs in the app; the
// calling thread (the UI loop) never waits for them.
Json waitJob(Fixture& f, double id) {
  Json j;
  for (int i = 0; i < 6000; ++i) {
    pollJobs(f.env);
    j = Fixture::data(f.call("get_job", jobj({{"job", jnum(id)}})));
    if (j.text("state") != "running") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return j;
}

void jobTests() {
  Fixture f("jobs");
  f.env.allowEdits = true;
  f.env.jobs.background = true;
  // Deferred reply: the session answers nothing now, the transport later.
  f.session.setDeferral(true);
  Json m = jobj({{"jsonrpc", jstr("2.0")}, {"id", jnum(77)}, {"method", jstr("tools/call")},
                 {"params", jobj({{"name", jstr("analyze_loudness")}, {"arguments", jobj({{"maxSeconds", jnum(3)}})},
                                  {"_meta", jobj({{"progressToken", jstr("tok-1")}})}})}});
  const auto t0 = std::chrono::steady_clock::now();
  Json now = f.session.handleJson(m);
  const double callMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  f.session.setDeferral(false);
  auto pending = f.session.takePending();
  expect(now.is(Json::Type::Null) && pending.size() == 1 && pending[0].job, "job: deferred tools/call answers later");
  expect(pending.size() == 1 && pending[0].progressToken.str == "tok-1", "job: progress token kept");
  std::printf("# analyze_loudness call returned in %.1f ms\n", callMs);
  bool finalized = false;
  for (int i = 0; i < 6000 && !pending.empty() && !finalized; ++i) {
    pollJobs(f.env);
    finalized = pending[0].job->finalized;
    if (!finalized) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(finalized, "job: finishes on a worker");
  if (finalized) {
    Json r = Session::pendingReply(pending[0]);
    const Json* res = r.find("result");
    expect(r.num("id") == 77 && res && Fixture::data(*res).find("lufs") && !Fixture::isError(*res), "job: reply carries the loudness");
    expect(!f.env.log.entries().empty() && f.env.log.entries().back().tool == "analyze_loudness", "job: logged when finished");
  }

  // "wait": false returns a job number; get_job reports progress and the result.
  Json h = Fixture::data(f.call("render_audio", jobj({{"path", jstr("jobs/a.wav")}, {"maxSeconds", jnum(2)}, {"wait", jbool(false)}})));
  expect(h.num("job") > 0 && (h.text("state") == "running" || h.text("state") == "done"), "job: wait false returns a job number");
  Json done = waitJob(f, h.num("job"));
  expect(done.text("state") == "done" && done.find("result") && fs::exists(f.dir / "jobs" / "a.wav"), "job: get_job returns the render result");
  expect(Fixture::isError(f.call("get_job", jobj({{"job", jnum(99999)}}))), "job: unknown job number refused");

  // Cancel: nothing is written.
  Json c = Fixture::data(f.call("render_audio", jobj({{"path", jstr("jobs/c.wav")}, {"maxSeconds", jnum(600)}, {"wait", jbool(false)}})));
  f.call("cancel_job", jobj({{"job", c.find("job") ? *c.find("job") : jnum(0)}}));
  Json cj = waitJob(f, c.num("job"));
  expect(cj.text("state") == "cancelled" && !fs::exists(f.dir / "jobs" / "c.wav"), "job: cancel_job stops the render, no file");

  // Hosted plugins (runInline): the work runs inside the call.
  f.env.jobs.runInline = [] { return true; };
  f.session.setDeferral(true);
  Json in = f.session.handleJson(jobj({{"jsonrpc", jstr("2.0")}, {"id", jnum(5)}, {"method", jstr("tools/call")},
                                       {"params", jobj({{"name", jstr("analyze_loudness")}, {"arguments", jobj({{"maxSeconds", jnum(1)}})}})}}));
  f.session.setDeferral(false);
  expect(f.session.takePending().empty() && in.find("result") && Fixture::data(*in.find("result")).find("lufs"), "job: inline when the app says so");
  f.env.jobs.runInline = nullptr;

  // Refused before starting: bad path, edits off.
  expect(Fixture::isError(f.call("render_audio", jobj({{"path", jstr("../../x.wav")}, {"wait", jbool(false)}}))), "job: path checked before the job starts");
  f.env.allowEdits = false;
  expect(Fixture::isError(f.call("render_audio", jobj({{"path", jstr("x.wav")}}))), "job: edits off refuses renders");
  f.env.allowEdits = true;

  // Event stream parsing (attach bridge).
  std::string buf;
  auto ev = sseEvents(buf, "event: message\ndata: {\"a\":1}\n\nevent: message\r\ndata: {\"b\"");
  expect(ev.size() == 1 && ev[0] == "{\"a\":1}", "sse: complete event split");
  ev = sseEvents(buf, ":2}\r\n\r\n");
  expect(ev.size() == 1 && ev[0] == "{\"b\":2}" && buf.empty(), "sse: event across chunks");
}

// The app's endpoint: a render over HTTP streams progress and never blocks a poll.
void httpJobTests() {
  Fixture f("httpjobs");
  f.env.allowEdits = true;
  f.env.jobs.background = true;
  HttpEndpoint ep(f.env, "job-token-0123456789abcdef");
  std::string err;
  expect(ep.start(0, &err), "HTTP jobs: listening");
  const int port = ep.port();
  auto hdrs = [](const std::string& session, bool sse) {
    std::vector<std::pair<std::string, std::string>> h = {{"Content-Type", "application/json"},
                                                          {"Accept", sse ? "application/json, text/event-stream" : "application/json"},
                                                          {"Authorization", "Bearer job-token-0123456789abcdef"}};
    if (!session.empty()) h.push_back({"Mcp-Session-Id", session});
    return h;
  };
  HttpReply init, streamed, plain;
  std::vector<std::string> chunks;
  double worstPollMs = 0;
  std::atomic<bool> done{false};
  std::thread client([&] {
    httpRequest(port, "POST", "/mcp", hdrs("", false),
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"job-test"}}})", init);
    const std::string sid = init.headers.count("mcp-session-id") ? init.headers["mcp-session-id"] : "";
    httpRequest(port, "POST", "/mcp", hdrs(sid, false), R"({"jsonrpc":"2.0","method":"notifications/initialized"})", plain);
    httpRequest(port, "POST", "/mcp", hdrs(sid, true),
                R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"render_audio","arguments":{"path":"s.wav","maxSeconds":20},"_meta":{"progressToken":7}}})",
                streamed, nullptr, 120, [&](const std::string& c) { chunks.push_back(c); });
    httpRequest(port, "POST", "/mcp", hdrs(sid, false),
                R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"analyze_loudness","arguments":{"maxSeconds":2}}})", plain);
    done = true;
  });
  const auto t0 = std::chrono::steady_clock::now();
  while (!done && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) {
    const auto p0 = std::chrono::steady_clock::now();
    ep.poll(0);
    worstPollMs = std::max(worstPollMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  client.join();
  std::printf("# worst endpoint poll while rendering: %.1f ms\n", worstPollMs);
  expect(worstPollMs < 250, "HTTP jobs: polling never waits for the render");
  expect(streamed.status == 200 && contains(streamed.headers["content-type"], "text/event-stream"), "HTTP jobs: event stream with a progress token");
  expect(contains(streamed.body, "notifications/progress") && contains(streamed.body, "\"progressToken\":7"), "HTTP jobs: progress notifications");
  expect(contains(streamed.body, "\"id\":2") && contains(streamed.body, "\"lufs\"") && fs::exists(f.dir / "s.wav"), "HTTP jobs: final reply in the stream");
  expect(!chunks.empty(), "HTTP jobs: body delivered as it arrives");
  expect(plain.status == 200 && contains(plain.headers["content-type"], "application/json") && contains(plain.body, "\"lufs\""),
         "HTTP jobs: plain JSON reply without a progress token");
}

// =============================================================== HTTP ====
struct Served {
  HttpEndpoint& ep;
  std::atomic<bool> done{false};
  std::thread client;
  template <class F>
  Served(HttpEndpoint& e, F fn) : ep(e) {
    client = std::thread([this, fn] {
      fn();
      done = true;
    });
    const auto t0 = std::chrono::steady_clock::now();
    while (!done && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) ep.poll(5);
    client.join();
  }
};

void httpTests() {
  Fixture f("http");
  HttpEndpoint ep(f.env, "s3cret-token-0123456789");
  std::string err;
  expect(ep.start(0, &err) && ep.port() > 0, "HTTP: listens on a free port");
  expect(ep.server().boundAddress() == "127.0.0.1", "HTTP: bound to 127.0.0.1 only");
  const int port = ep.port();
  const std::string init = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"http-test"}}})";
  auto hdrs = [](const std::string& token, const std::string& session) {
    std::vector<std::pair<std::string, std::string>> h = {{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
    if (!token.empty()) h.push_back({"Authorization", "Bearer " + token});
    if (!session.empty()) h.push_back({"Mcp-Session-Id", session});
    return h;
  };
  HttpReply noTok, badTok, ok, noSess, list, notif, unknown, get, del, del2, wrongPath;
  std::string session;
  Served run(ep, [&] {
    httpRequest(port, "POST", "/mcp", hdrs("", ""), init, noTok);
    httpRequest(port, "POST", "/mcp", hdrs("wrong", ""), init, badTok);
    httpRequest(port, "POST", "/mcp", hdrs("s3cret-token-0123456789", ""), init, ok);
    session = ok.headers.count("mcp-session-id") ? ok.headers["mcp-session-id"] : "";
    httpRequest(port, "POST", "/mcp", hdrs("s3cret-token-0123456789", ""), R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", noSess);
    httpRequest(port, "POST", "/mcp", hdrs("s3cret-token-0123456789", session), R"({"jsonrpc":"2.0","method":"notifications/initialized"})", notif);
    httpRequest(port, "POST", "/mcp", hdrs("s3cret-token-0123456789", session), R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})", list);
    httpRequest(port, "POST", "/mcp", hdrs("s3cret-token-0123456789", "nope"), R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})", unknown);
    httpRequest(port, "GET", "/mcp", hdrs("s3cret-token-0123456789", session), "", get);
    httpRequest(port, "POST", "/other", hdrs("s3cret-token-0123456789", session), "{}", wrongPath);
    httpRequest(port, "DELETE", "/mcp", hdrs("s3cret-token-0123456789", session), "", del);
    httpRequest(port, "DELETE", "/mcp", hdrs("s3cret-token-0123456789", session), "", del2);
  });
  expect(noTok.status == 401 && noTok.headers.count("www-authenticate"), "HTTP: no token -> 401");
  expect(badTok.status == 401, "HTTP: wrong token -> 401");
  expect(ok.status == 200 && !session.empty() && contains(ok.body, "\"protocolVersion\":\"2025-03-26\""), "HTTP: initialize -> 200 + Mcp-Session-Id");
  expect(noSess.status == 400, "HTTP: no session id -> 400");
  expect(notif.status == 202 && notif.body.empty(), "HTTP: notification -> 202 Accepted");
  expect(list.status == 200 && contains(list.body, "\"tools\"") && contains(list.headers["content-type"], "application/json"), "HTTP: tools/list in the session");
  expect(unknown.status == 404, "HTTP: unknown session -> 404");
  expect(get.status == 405, "HTTP: GET (no event stream) -> 405");
  expect(wrongPath.status == 404, "HTTP: other paths -> 404");
  expect(del.status == 200 && del2.status == 404, "HTTP: DELETE ends the session");

  // Host / Origin (DNS rebinding) and token checks on the endpoint logic.
  HttpServer::Request rq;
  rq.method = "POST";
  rq.path = "/mcp";
  rq.body = init;
  rq.headers = {{"host", "127.0.0.1:" + std::to_string(port)}, {"authorization", "Bearer s3cret-token-0123456789"}};
  expect(ep.handle(rq).status == 200, "endpoint: loopback host accepted");
  rq.headers["host"] = "localhost:1234";
  expect(ep.handle(rq).status == 200, "endpoint: localhost accepted");
  rq.headers["host"] = "evil.example:80";
  expect(ep.handle(rq).status == 403, "endpoint: foreign Host -> 403");
  rq.headers["host"] = "127.0.0.1";
  rq.headers["origin"] = "https://evil.example";
  expect(ep.handle(rq).status == 403, "endpoint: foreign Origin -> 403");
  rq.headers["origin"] = "http://localhost:6274";
  expect(ep.handle(rq).status == 200, "endpoint: loopback Origin accepted");
  rq.headers.erase("origin");
  rq.headers["authorization"] = "Bearer s3cret-token-012345678";  // one character short
  expect(ep.handle(rq).status == 401, "endpoint: near-miss token -> 401");
  rq.headers["authorization"] = "Bearer s3cret-token-0123456789";
  rq.headers["mcp-protocol-version"] = "1999-01-01";
  expect(ep.handle(rq).status == 400, "endpoint: unsupported MCP-Protocol-Version -> 400");
  rq.headers.erase("mcp-protocol-version");
  rq.body = "{oops";
  expect(ep.handle(rq).status == 400, "endpoint: malformed JSON -> 400");
  for (int i = 0; i < 40; ++i) {
    rq.body = init;
    ep.handle(rq);
  }
  expect(ep.sessionCount() <= HttpEndpoint::kMaxSessions, "endpoint: session count bounded");
  HttpEndpoint empty(f.env, "");
  rq.headers["authorization"] = "Bearer ";
  expect(empty.handle(rq).status == 401, "endpoint: an empty token never matches");

  // Oversized body: refused from the Content-Length alone.
  {
    std::string reply;
    Served big(ep, [&] {
      const net::Sock fd = net::tcpSocket();
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(static_cast<uint16_t>(port));
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        const std::string req = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer s3cret-token-0123456789\r\nContent-Length: 999999999\r\n\r\n{";
        (void)!net::sendSome(fd, req.data(), req.size());
        char buf[4096];
        long n;
        while ((n = net::recvSome(fd, buf, sizeof buf)) > 0) reply.append(buf, static_cast<size_t>(n));
      }
      net::closeSock(fd);
    });
    expect(contains(reply, " 413 "), "HTTP: oversized body -> 413");
  }
  // Endpoint file: owner-only.
  const std::string efile = (f.dir / "mcp-endpoint.json").string();
  expect(writeEndpointFile(efile, port, ep.token()), "endpoint file written");
#if !defined(_WIN32)
  struct stat st {};
  expect(::stat(efile.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "endpoint file is owner-only (0600)");
#endif
  int p2 = 0;
  std::string t2;
  expect(readEndpointFile(efile, p2, t2) && p2 == port && t2 == ep.token(), "endpoint file read back");

  // Attach bridge: stdio <-> HTTP.
  {
    const std::string in = init + "\n" + R"({"jsonrpc":"2.0","method":"notifications/initialized"})" + "\n" +
                           R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_song"}})" + "\n";
    // Files stand in for the stdio streams (fmemopen / open_memstream are POSIX only).
    std::FILE* fin = streamOf(in, f.dir / "bridge-in.txt");
    std::FILE* fout = std::fopen((f.dir / "bridge-out.txt").string().c_str(), "wb");
    Served bridge(ep, [&] { attachBridge(fin, fout, efile); });
    std::fclose(fin);
    std::fclose(fout);
    const std::string out = fileText(f.dir / "bridge-out.txt");
    std::vector<std::string> lines;
    std::istringstream ls(out);
    for (std::string l; std::getline(ls, l);) lines.push_back(l);
    expect(lines.size() == 2 && contains(lines[0], "\"serverInfo\"") && contains(lines[1], "Gridline Demo"), "attach bridge: initialize and a tool call");
  }
  {  // bridge without a running app: JSON-RPC errors for requests
    const std::string in = init + "\n" + R"({"jsonrpc":"2.0","method":"notifications/initialized"})" + "\n";
    std::FILE* fin = streamOf(in, f.dir / "bridge-in2.txt");
    std::FILE* fout = std::fopen((f.dir / "bridge-out2.txt").string().c_str(), "wb");
    attachBridge(fin, fout, (f.dir / "missing.json").string());
    std::fclose(fin);
    std::fclose(fout);
    const std::string out = fileText(f.dir / "bridge-out2.txt");
    Json r = parseJ(out.substr(0, out.find('\n')));
    expect(errorCode(r) != 0 && r.find("id") && r.find("id")->number == 1 && std::count(out.begin(), out.end(), '\n') == 1,
           "attach bridge: app not running -> error reply, nothing for notifications");
  }
  ep.stop();
  expect(!ep.running(), "HTTP: stopped");
  expect(constantTimeEqual("abc", "abc") && !constantTimeEqual("abc", "abd") && !constantTimeEqual("abc", "abcd"), "constant-time compare");
  expect(randomToken().size() == 32 && randomToken() != randomToken(), "random tokens");
}

// ============================================================ CLI stdio ====
void cliTests() {
  const fs::path dir = tempDir("cli");
  Song s = makeTestSong();
  const fs::path song = dir / "cli song.gridline.json";
  saveProject(song.string(), s);
  const fs::path in = dir / "in.jsonl", out = dir / "out.jsonl", err = dir / "err.txt";
  {
    std::ofstream os(in);
    os << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli"}}})" << "\n"
       << R"({"jsonrpc":"2.0","method":"notifications/initialized"})" << "\n"
       << R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"set_song","arguments":{"tempo":150}}})" << "\n"
       << R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"render_audio","arguments":{"path":"cli.wav","maxSeconds":1}}})" << "\n"
       << R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"save_project"}})" << "\n"
       << "\n"
       << "garbage\n";
  }
  auto q = [](const fs::path& p) { return platform::shellQuote(p.string()); };
  const std::string cmd = platform::shellQuote(GRIDLINE_CLI_EXE) + " mcp " + q(song) + " < " + q(in) + " > " + q(out) + " 2> " + q(err);
  const int rc = platform::runShell(cmd);
  expect(rc == 0, "gridline mcp: exits 0 at end of input");
  std::ifstream is(out);
  std::vector<std::string> lines;
  for (std::string l; std::getline(is, l);) lines.push_back(l);
  bool allJson = !lines.empty();
  for (const auto& l : lines) {
    Json j;
    allJson = allJson && parse(l, j) && j.text("jsonrpc") == "2.0";
  }
  expect(lines.size() == 5 && allJson, "gridline mcp: one JSON-RPC line per request, nothing else on stdout");
  expect(lines.size() == 5 && errorCode(parseJ(lines[4])) == kParseError, "gridline mcp: garbage -> parse error");
  Song back;
  expect(loadProject(song.string(), back) && back.tempo == 150, "gridline mcp: edit + save_project wrote the file");
  expect(fs::exists(dir / "cli.wav"), "gridline mcp: render_audio beside the project");
  std::ifstream es(err);
  std::stringstream ess;
  ess << es.rdbuf();
  expect(contains(ess.str(), "set_song"), "gridline mcp: activity on stderr");
  // Read-only flag.
  {
    std::ofstream os(in);
    os << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{}}})" << "\n"
       << R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"set_song","arguments":{"tempo":90}}})" << "\n";
  }
  const std::string cmd2 = platform::shellQuote(GRIDLINE_CLI_EXE) + " mcp " + q(song) + " --read-only < " + q(in) + " > " + q(out) + " 2> " + q(err);
  expect(platform::runShell(cmd2) == 0, "gridline mcp --read-only runs");
  std::ifstream is2(out);
  std::string l1, l2;
  std::getline(is2, l1);
  std::getline(is2, l2);
  expect(contains(l2, "\"isError\":true") && contains(l2, "read-only"), "gridline mcp --read-only refuses edits");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  protocolTests();
  toolTests();
  fileTests();
  pathTests();
  safetyTests();
  httpTests();
  jobTests();
  httpJobTests();
  cliTests();
  return finish();
}

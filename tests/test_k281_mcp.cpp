// Gridline - assistant tools reach everything (MCP, part two).
//
// Coverage guard: every keyboard command of the registry is listed by list_commands, every plugin
// type is listed by the catalogue and can be put on a channel or a mixer slot and have a parameter
// set through the tools, every setting is listed by get_settings with its access (and every access
// rule still names a setting), every field the project file writes for a channel, pattern, mixer
// track, playlist track, clip, automation clip, marker, Stage room or the song is described for
// inspect / patch, and every tool is described with a schema. A feature that adds a command, plugin,
// setting or project field without that fails here.
//
// Tool families against a real in-process project: create, edit, undo, query for inspect / patch,
// batch, parameter paths, find, duplicate, notes, automation, mixer tracks and sends, Stage rooms,
// arrangements, transport and selection, the app's commands (with a stand-in app), settings, help.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include "Check.h"
#include "core/Document.h"
#include "core/Keymap.h"
#include "core/Mcp.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/ProjectScript.h"
#include "core/Settings.h"
#include "core/StageRoomOps.h"
#include "fixtures/TestSong.h"

using namespace gl;
using namespace gl::mcp;
namespace fs = std::filesystem;

namespace {

Json parseJ(const std::string& s) {
  Json j;
  parse(s, j);
  return j;
}

struct Fixture {
  Document doc;
  script::SimpleProjectHost host{doc};
  Environment env{doc, host};
  Session session{env};
  int nextId = 1;
  double fakeNow = 1000.0;

  explicit Fixture(Song s = makeTestSong()) {
    doc.reset(std::move(s));
    env.now = [this] { return fakeNow += 0.5; };
    rpc("initialize", jobj({{"protocolVersion", jstr("2025-06-18")}, {"clientInfo", jobj({{"name", jstr("k281")}})}}));
  }
  Json rpc(const std::string& method, Json params = Json::makeObject()) {
    Json m = jobj({{"jsonrpc", jstr("2.0")}, {"id", jnum(nextId++)}, {"method", jstr(method)}, {"params", params}});
    return parseJ(session.handle(dump(m)));
  }
  Json call(const std::string& tool, Json args = Json::makeObject()) {
    Json r = rpc("tools/call", jobj({{"name", jstr(tool)}, {"arguments", args}}));
    if (const Json* res = r.find("result")) return *res;
    return r;
  }
  Json callJ(const std::string& tool, const std::string& args) { return call(tool, parseJ(args)); }
  static bool isError(const Json& res) { return !res.find("result") && (res.find("error") || (res.find("isError") && res.find("isError")->boolean)); }
  static Json data(const Json& res) { return res.find("structuredContent") ? *res.find("structuredContent") : Json(); }
  static std::string text(const Json& res) {
    if (const Json* e = res.find("error")) return e->text("message");
    const Json* c = res.find("content");
    return c && !c->array.empty() ? c->array[0].text("text") : std::string();
  }
  // An edit that must succeed as one undo step.
  Json edit(const std::string& tool, const std::string& args, const std::string& what) {
    const size_t before = doc.history().position();
    Json r = callJ(tool, args);
    const bool ok = !isError(r);
    if (!ok) std::printf("# %s: %s\n", what.c_str(), text(r).c_str());
    expect(ok, what.c_str());
    expect(doc.history().position() == before + 1 && doc.history().undoLabel().rfind("AI: ", 0) == 0, (what + ": one undo step").c_str());
    return data(r);
  }
  // A call that must fail, change nothing and say something.
  std::string refused(const std::string& tool, const std::string& args, const std::string& what) {
    const Song before = doc.song();
    const size_t pos = doc.history().position();
    Json r = callJ(tool, args);
    expect(isError(r), what.c_str());
    expect(doc.song() == before && doc.history().position() == pos, (what + ": nothing changed").c_str());
    return text(r);
  }
};

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// ================================================================ coverage ====
void commandCoverage() {
  Fixture f;
  Json r = Fixture::data(f.callJ("list_commands", "{\"limit\": 500}"));
  std::set<std::string> ids;
  const size_t total = static_cast<size_t>(r.num("total"));
  for (size_t off = 0; off < total; off += 500) {
    Json page = Fixture::data(f.callJ("list_commands", "{\"limit\": 500, \"offset\": " + std::to_string(off) + "}"));
    for (const auto& c : page.find("commands")->array) ids.insert(c.text("id"));
  }
  size_t missing = 0;
  for (const auto& c : keys::commands())
    if (!ids.count(std::string("cmd:") + c.id)) {
      if (missing++ < 5) std::printf("# keyboard command not listed: %s\n", c.id);
    }
  expect(missing == 0 && !keys::commands().empty(), "coverage: every keyboard command is listed by list_commands");
  expect(contains(dump(r), "needs the Gridline app"), "coverage: headless list_commands says the app runs them");
}

void pluginCoverage() {
  Fixture f;
  Json cat = Fixture::data(f.callJ("list_plugin_catalog", "{}"));
  std::set<std::string> listed;
  for (const auto& p : cat.find("plugins")->array) listed.insert(p.text("type"));
  size_t missing = 0, failed = 0, tried = 0;
  for (const auto& p : pluginCatalog()) {
    if (p.hidden) continue;
    if (!listed.count(p.type)) {
      ++missing;
      std::printf("# plugin not in the catalogue: %s\n", p.type.c_str());
    }
    ++tried;
    std::string ok1, path;
    if (p.kind == PluginKind::Generator) {
      Json r = f.call("add_channel", jobj({{"plugin", jstr(p.type)}}));
      if (Fixture::isError(r)) {
        ++failed;
        std::printf("# add_channel %s: %s\n", p.type.c_str(), Fixture::text(r).c_str());
        continue;
      }
      path = "channel/" + std::to_string(f.doc.song().channels.size()) + "/1";
    } else if (p.kind == PluginKind::Effect) {
      Json r = f.call("add_plugin", jobj({{"mixer", jnum(1)}, {"slot", jnum(1)}, {"plugin", jstr(p.type)}}));
      if (Fixture::isError(r)) {
        ++failed;
        std::printf("# add_plugin %s: %s\n", p.type.c_str(), Fixture::text(r).c_str());
        continue;
      }
      path = "mixer/1/slot/1/1";
    } else {  // note processors: the channel's noteFx list, through patch
      Json fx = jarr({jobj({{"type", jstr(p.type)}})});
      Json r = f.call("patch", jobj({{"path", jstr("channel/1")}, {"set", jobj({{"noteFx", fx}})}}));
      const Channel& c = f.doc.song().channels[0];
      if (Fixture::isError(r) || c.noteFx.empty() || c.noteFx[0].type != p.type) {
        ++failed;
        std::printf("# note processor %s: %s\n", p.type.c_str(), Fixture::text(r).c_str());
      }
      continue;
    }
    if (p.params.empty()) continue;
    Json r = f.call("set_parameters", jobj({{"changes", jarr({jobj({{"path", jstr(path)}, {"value", jnum(0.5)}, {"normalized", jbool(true)}})})}}));
    if (Fixture::isError(r)) {
      ++failed;
      std::printf("# set_parameters %s (%s): %s\n", path.c_str(), p.type.c_str(), Fixture::text(r).c_str());
    }
  }
  expect(missing == 0, "coverage: every plugin type is in list_plugin_catalog");
  expect(failed == 0 && tried > 50, "coverage: every plugin type can be placed and have a parameter set through the tools");
}

void settingsCoverage() {
  Fixture f;
  Settings st;
  int saved = 0;
  f.env.app.settings = &st;
  f.env.app.settingsChanged = [&saved] { ++saved; };
  Json r = Fixture::data(f.callJ("get_settings", "{\"limit\": 2000}"));
  std::set<std::string> listed;
  for (const auto& s : r.find("settings")->array) listed.insert(s.text("key"));
  size_t missing = 0;
  for (const auto& k : st.keys())
    if (!listed.count(k)) ++missing;
  expect(missing == 0 && listed.size() == st.keys().size(), "coverage: every setting is listed by get_settings");
  for (const auto& rule : settingAccessRules()) {
    bool used = false;
    for (const auto& k : st.keys()) used = used || k.compare(0, rule.size(), rule) == 0;
    if (!used) std::printf("# access rule names no setting: %s\n", rule.c_str());
    expect(used, ("coverage: the access rule \"" + rule + "\" still names a setting").c_str());
  }
  bool hiddenShown = false;
  for (const auto& s : r.find("settings")->array)
    if (s.text("access") == "hidden" && s.find("value")) hiddenShown = true;
  expect(!hiddenShown, "settings: hidden settings (tokens) show no value");
  expect(settingAccess("mcp.allowEdits") == SettingAccess::ReadOnly && settingAccess("mcp.token") == SettingAccess::Hidden &&
             settingAccess("scripts.enabled") == SettingAccess::ReadOnly && settingAccess("osc.network") == SettingAccess::ReadOnly,
         "settings: the assistant, script and network switches stay with the user");
  std::string why = Fixture::text(f.callJ("set_setting", "{\"key\": \"mcp.allowEdits\", \"value\": true}"));
  expect(contains(why, "user") && !st.getBool("mcp.allowEdits"), "settings: an assistant cannot allow itself more");
  Json ok = f.callJ("set_setting", "{\"key\": \"ui.smoothScroll\", \"value\": false}");
  expect(!Fixture::isError(ok) && !st.getBool("ui.smoothScroll") && saved == 1, "settings: set_setting changes and saves a setting");
  why = Fixture::text(f.callJ("set_setting", "{\"key\": \"ui.smoothScroll\", \"value\": 3}"));
  expect(contains(why, "bool"), "settings: a wrong value type says the type");
  why = Fixture::text(f.callJ("set_setting", "{\"key\": \"ui.nothing\", \"value\": 3}"));
  expect(contains(why, "get_settings"), "settings: an unknown key points at get_settings");
}

// Top-level keys of every element the project file writes, in several projects, against the
// fields help describes (inspect / patch reach every key anyway; the description must keep up).
void fieldCoverage() {
  Fixture f;
  Json objects = Fixture::data(f.callJ("help", "{\"topic\": \"objects\"}"));
  std::map<std::string, std::set<std::string>> documented;
  for (const auto& o : objects.find("objects")->array)
    for (const auto& [k, v] : o.find("fields")->object) documented[o.text("kind")].insert(k);
  std::vector<Song> songs;
  songs.push_back(makeTestSong());
  for (const char* dir : {"assets/demo", "assets/templates"}) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      const std::string p = it->path().string();
      if (p.find(".gridline") == std::string::npos || it->is_directory()) continue;
      Song s;
      if (loadProject(p, s)) songs.push_back(std::move(s));
    }
  }
  {  // a song using the less common fields
    Document d(makeTestSong());
    d.editChannel(d.song().channels[0].id, "", [](Channel& c) {
      c.echo.count = 2, c.mono = true, c.porta = true, c.cutGroup = 1, c.cutBy = 2, c.keyLo = 10, c.hibernate = true, c.keepRunning = true;
      c.midiOutPort = 3, c.midiInPort = 4, c.polyphony = 2, c.swingTruncate = true, c.timeShift = 0.1f, c.fine = 5, c.icon = 2;
      c.layer = {c.id + 1000}, c.layerMode = 2;
    });
    d.editMixerTrack(d.song().mixer[1].id, "", [](MixerTrack& m) {
      m.depth = 0.3f, m.focus = 0.2f, m.disabled = true, m.dock = 1, m.group = "Drums", m.input = 0, m.armed = true, m.separator = true;
      m.locked = true, m.soloAlone = true, m.invertPolarity = true, m.swapLR = true, m.latencyOffset = 5, m.threaded = false, m.icon = 3;
    });
    d.editPlaylistTrack(0, "", [](PlaylistTrack& t) { t.locked = true, t.groupSize = 1, t.folded = true, t.icon = 1, t.height = 2, t.pad = PadMode::Gate; });
    roomops::addRoom(d, 0, nullptr, "Hall", 0);
    songs.push_back(d.song());
  }
  static const std::set<std::string> managed = {"id", "format", "version", "nextId", "clipLoops", "currentArrangement"};
  std::map<std::string, std::set<std::string>> undocumented;
  auto scan = [&](const std::string& kind, const Json* arr) {
    if (!arr || !arr->is(Json::Type::Array)) return;
    for (const auto& o : arr->array)
      for (const auto& [k, v] : o.object)
        if (!documented[kind].count(k) && !managed.count(k)) undocumented[kind].insert(k);
  };
  for (const Song& s : songs) {
    const Json j = songToJson(s, false);
    for (const auto& [k, v] : j.object) {
      static const std::set<std::string> collections = {"channels", "patterns", "playlist", "tracks", "mixer", "automations", "markers", "links", "modLinks", "stageRooms", "arrangements"};
      if (!documented["song"].count(k) && !managed.count(k) && !collections.count(k)) undocumented["song"].insert(k);
    }
    scan("channel", j.find("channels"));
    scan("pattern", j.find("patterns"));
    scan("mixer", j.find("mixer"));
    scan("track", j.find("tracks"));
    scan("clip", j.find("playlist"));
    scan("automation", j.find("automations"));
    scan("marker", j.find("markers"));
    scan("room", j.find("stageRooms"));
    scan("link", j.find("links"));
    scan("modlink", j.find("modLinks"));
  }
  std::string list;
  for (const auto& [kind, keys] : undocumented)
    for (const auto& k : keys) list += " " + kind + "." + k;
  if (!list.empty()) std::printf("# project fields help does not describe:%s\n", list.c_str());
  expect(list.empty() && songs.size() >= 2, "coverage: every project field is described for inspect / patch");
}

void toolCoverage() {
  Fixture f;
  Json list = f.rpc("tools/list");
  size_t bad = 0, n = 0;
  std::set<std::string> names;
  for (const auto& t : list.find("result")->find("tools")->array) {
    ++n;
    names.insert(t.text("name"));
    const Json* s = t.find("inputSchema");
    if (t.text("description").size() < 30 || !s || s->text("type") != "object") {
      ++bad;
      std::printf("# tool not described: %s\n", t.text("name").c_str());
    }
    Json h = f.call("help", jobj({{"topic", jstr(t.text("name"))}}));
    if (Fixture::isError(h)) ++bad;
  }
  expect(bad == 0 && n >= 50, "coverage: every tool has a description, a schema and a help topic");
  for (const char* old : {"get_song", "list_channels", "set_notes", "set_parameter", "edit_mixer_track", "add_clip", "run_script", "undo", "render_audio", "save_project"})
    expect(names.count(old) > 0, (std::string("compatibility: ") + old + " still exists").c_str());
  const std::string err = Fixture::text(f.callJ("edit_channel", "{\"channel\": 1, \"volumee\": 1}"));
  expect(contains(err, "it takes:") && contains(err, "volume"), "errors: an unknown argument lists the arguments the tool takes");
}

// ================================================================ families ====
void inspectPatchTests() {
  Fixture f;
  Json r = Fixture::data(f.callJ("inspect", "{\"path\": \"channel/2\", \"fields\": true}"));
  expect(r.find("value") && r.find("value")->text("name") == "Clap" && r.find("fields") && r.find("fields")->find("echo"), "inspect: a channel with its fields");
  r = Fixture::data(f.callJ("inspect", "{\"path\": \"song\"}"));
  expect(r.find("value")->find("counts") && !r.find("value")->find("channels"), "inspect: the song shows counts, not its collections");
  expect(contains(f.refused("inspect", "{\"path\": \"channels\"}", "inspect: a list is refused"), "channel/1"), "inspect: a list points at an element path");
  expect(contains(f.refused("inspect", "{\"path\": \"channel/99\"}", "inspect: a missing channel is refused"), "1..8"), "inspect: the error says the range");

  const Song before = f.doc.song();
  r = f.edit("patch", "{\"path\": \"channel/2\", \"set\": {\"name\": \"Lead\", \"color\": \"#102030\", \"echo\": {\"count\": 3}, \"mono\": true}}", "patch: a channel's fields");
  const Channel& c = f.doc.song().channels[1];
  expect(c.name == "Lead" && c.color == 0x102030 && c.echo.count == 3 && c.mono, "patch: name, colour, nested echo and a note setting changed");
  expect(!r.find("changed")->array[0].find("ignored"), "patch: nothing ignored");
  f.call("undo");
  expect(f.doc.song() == before, "patch: undo restores the channel");
  expect(contains(f.refused("patch", "{\"path\": \"channel/2\", \"set\": {\"nmae\": \"x\"}}", "patch: only unknown fields are refused"), "nmae"),
         "patch: the refusal names the unknown field");
  r = f.edit("patch", "{\"path\": \"channel/2\", \"set\": {\"nmae\": \"x\", \"name\": \"Lead 2\"}}", "patch: an unknown field beside a known one");
  expect(contains(dump(r), "nmae") && contains(dump(r), "ignored") && f.doc.song().channels[1].name == "Lead 2", "patch: an unknown field is reported as ignored");
  f.refused("patch", "{\"path\": \"channel/2\", \"set\": {\"id\": 5}}", "patch: an id cannot change");
  f.refused("patch", "{\"path\": \"song\", \"set\": {\"channels\": []}}", "patch: a collection is refused on the song");
  f.edit("patch", "{\"path\": \"song\", \"set\": {\"tempo\": 101, \"info\": {\"genre\": \"Ambient\"}}}", "patch: song settings");
  expect(f.doc.song().tempo == 101 && f.doc.song().info.genre == "Ambient" && f.doc.song().info.title == "Gridline Demo", "patch: tempo and a nested info field (others kept)");
  f.edit("patch", "{\"path\": \"track/3/name\", \"value\": \"Drums\"}", "patch: one field by its path");
  expect(f.doc.song().tracks[2].name == "Drums", "patch: a playlist track's name");
  f.edit("patch", "{\"patches\": [{\"path\": \"mixer/1\", \"set\": {\"depth\": 0.5, \"stereoSep\": 0.25}}, {\"path\": \"room/1\", \"set\": {\"width\": 1.25}}]}",
         "patch: several objects in one step");
  expect(f.doc.song().mixer[1].depth == 0.5f && f.doc.song().stage.width == 1.25f, "patch: mixer Stage depth and the room's width");
  const ObjectId clip = f.doc.song().playlist.empty() ? kNoId : f.doc.song().playlist[0].id;
  if (clip != kNoId) {
    f.edit("patch", "{\"path\": \"clip/" + std::to_string(clip) + "\", \"set\": {\"muted\": true, \"playTranspose\": 2}}", "patch: a clip by its id");
    expect(f.doc.song().clip(clip)->muted, "patch: the clip is muted");
  }
  r = f.edit("patch", "{\"path\": \"channel/1\", \"set\": {\"pan\": 5}}", "patch: a value out of range");
  expect(contains(dump(r), "adjusted") && f.doc.song().channels[0].pan <= 1.0f, "patch: an out-of-range value is adjusted and reported");
  f.refused("patch", "{\"path\": \"channel/1\", \"set\": {\"freeze\": {}}}", "patch: freeze goes through the app's commands");
}

void batchTests() {
  Fixture f;
  const size_t patterns = f.doc.song().patterns.size();
  Json r = f.edit("batch",
                  "{\"calls\": [{\"tool\": \"add_pattern\", \"arguments\": {\"name\": \"Verse\"}}, {\"tool\": \"set_steps\", \"arguments\": {\"pattern\": " +
                      std::to_string(patterns + 1) + ", \"channel\": 1, \"steps\": [100, 0, 100, 0]}}, {\"tool\": \"get_song\"}]}",
                  "batch: three calls as one step");
  expect(f.doc.song().patterns.size() == patterns + 1 && r.find("results")->array.size() == 3, "batch: the pattern was made and every call answered");
  const std::string err = f.refused("batch", "{\"calls\": [{\"tool\": \"add_pattern\"}, {\"tool\": \"set_steps\", \"arguments\": {\"pattern\": 999, \"channel\": 1, \"steps\": [1]}}]}",
                                    "batch: a failing call undoes the batch");
  expect(contains(err, "call 2") && contains(err, "set_steps"), "batch: the error names the failing call");
  f.refused("batch", "{\"calls\": [{\"tool\": \"save_project\"}]}", "batch: saves run on their own");
  f.env.allowEdits = false;
  expect(!Fixture::isError(f.callJ("batch", "{\"calls\": [{\"tool\": \"get_song\"}, {\"tool\": \"list_channels\"}]}")), "batch: reads run with changes switched off");
  f.refused("batch", "{\"calls\": [{\"tool\": \"add_pattern\"}]}", "batch: edits are refused with changes switched off");
}

void parameterTests() {
  Fixture f;
  f.edit("set_parameters", "{\"changes\": [{\"path\": \"song/tempo\", \"value\": 99}, {\"path\": \"channel/1/volume\", \"value\": 0.5}, {\"path\": \"mixer/2/pan\", \"value\": -0.5}]}",
         "set_parameters: three parameters, one step");
  expect(f.doc.song().tempo == 99 && std::fabs(f.doc.song().channels[0].gain - 0.5f) < 1e-4 && std::fabs(f.doc.song().mixer[2].pan + 0.5f) < 1e-4, "set_parameters: values set");
  Json r = Fixture::data(f.callJ("get_values", "{\"paths\": [\"song/tempo\", \"channel/1/volume\", \"channel/1/nothing\"]}"));
  expect(r.find("params")->array.size() == 3 && r.find("params")->array[0].num("value") == 99 && r.find("params")->array[2].find("error"), "get_values: values and a per-path error");
  const std::string err = f.refused("set_parameters", "{\"changes\": [{\"path\": \"song/tempo\", \"value\": 120}, {\"path\": \"bogus/1\", \"value\": 1}]}", "set_parameters: a bad path changes nothing");
  expect(contains(err, "change 2") && contains(err, "parameter paths"), "set_parameters: the error explains the path syntax");
}

void familyTests() {
  Fixture f;
  // find
  Json r = Fixture::data(f.callJ("find", "{\"query\": \"kick\"}"));
  expect(contains(dump(r), "channel/1") && contains(dump(r), "gl.kick"), "find: a channel and the plugin type");
  // duplicate
  const size_t chans = f.doc.song().channels.size();
  f.edit("duplicate", "{\"channel\": 1}", "duplicate: a channel");
  expect(f.doc.song().channels.size() == chans + 1, "duplicate: one more channel");
  f.edit("duplicate", "{\"pattern\": 1}", "duplicate: a pattern");
  // notes with every property
  f.edit("set_notes", "{\"pattern\": 1, \"channel\": 6, \"notes\": [{\"start\": 0, \"length\": 1, \"pitch\": 48, \"release\": 20, \"color\": 3, \"bend\": [[0, 0], [0.5, 2]]}, {\"start\": 1.03, \"pitch\": 50}]}",
         "set_notes: notes with bend, release and colour");
  r = Fixture::data(f.callJ("get_pattern", "{\"pattern\": 1, \"channel\": 6}"));
  const std::string pd = dump(r);
  expect(contains(pd, "\"release\":20") && contains(pd, "\"bend\"") && contains(pd, "\"color\":3"), "get_pattern: the note properties read back");
  f.edit("edit_notes", "{\"pattern\": 1, \"channel\": 6, \"op\": \"quantize\", \"amount\": 0.25}", "edit_notes: quantize");
  const auto& ns = f.doc.song().patterns[0].notes.at(f.doc.song().channels[5].id);
  bool onGrid = true;
  for (const auto& n : ns) onGrid = onGrid && n.start % (f.doc.song().ppq / 4) == 0;
  expect(onGrid, "edit_notes: notes on the grid");
  f.edit("edit_notes", "{\"pattern\": 1, \"channel\": 6, \"op\": \"transpose\", \"amount\": 12}", "edit_notes: transpose");
  f.refused("edit_notes", "{\"pattern\": 1, \"channel\": 6, \"op\": \"transpose\", \"lowestPitch\": 127}", "edit_notes: nothing matches");
  // automation
  f.edit("add_automation", "{\"channel\": 1, \"param\": \"volume\", \"points\": [{\"position\": 0, \"value\": 0}, {\"position\": 4, \"value\": 1, \"shape\": \"s_curve\", \"tension\": 0.3}]}",
         "add_automation: points with shape and tension");
  const size_t na = f.doc.song().automations.size();
  expect(f.doc.song().automations.back().points[1].shape == AutoShape::SCurve, "add_automation: the shape is kept");
  f.edit("edit_automation", "{\"automation\": " + std::to_string(na) + ", \"addPoints\": [{\"position\": 2, \"value\": 0.5, \"shape\": \"hold\"}], \"name\": \"Fade\"}",
         "edit_automation: add a point, rename");
  expect(f.doc.song().automations.back().points.size() == 3 && f.doc.song().automations.back().name == "Fade", "edit_automation: three points");
  f.edit("remove_automation", "{\"automation\": " + std::to_string(na) + "}", "remove_automation");
  expect(f.doc.song().automations.size() == na - 1, "remove_automation: gone");
  // mixer tracks and sends
  const size_t mt = f.doc.song().mixer.size();
  f.edit("add_mixer_track", "{\"name\": \"Bus\"}", "add_mixer_track");
  expect(f.doc.song().mixer.size() == mt + 1 && f.doc.song().mixer.back().name == "Bus", "add_mixer_track: named");
  f.edit("set_send", "{\"mixer\": 1, \"to\": " + std::to_string(mt) + ", \"level\": 0.5}", "set_send: a send");
  bool sent = false;
  for (const auto& s : f.doc.song().mixer[1].sends) sent = sent || (s.to == f.doc.song().mixer.back().id && std::fabs(s.level - 0.5f) < 1e-4);
  expect(sent, "set_send: the send is there");
  f.refused("set_send", "{\"mixer\": " + std::to_string(mt) + ", \"to\": 1, \"level\": 1}", "set_send: a loop is refused");
  f.edit("remove_mixer_track", "{\"mixer\": " + std::to_string(mt) + "}", "remove_mixer_track");
  // Stage rooms
  f.edit("stage_room", "{\"action\": \"add\", \"name\": \"Hall\"}", "stage_room: add a room");
  expect(roomops::roomCount(f.doc.song()) == 2, "stage_room: two rooms");
  f.edit("stage_room", "{\"action\": \"move_tracks\", \"room\": 2, \"tracks\": [3]}", "stage_room: move a track");
  expect(roomops::trackRoom(f.doc.song(), f.doc.song().mixer[3].id) == 1, "stage_room: the track stands in room 2");
  // arrangements
  f.edit("arrangement", "{\"action\": \"add\", \"name\": \"Edit\"}", "arrangement: add");
  r = Fixture::data(f.callJ("arrangement", "{}"));
  expect(r.find("arrangements")->array.size() == 2, "arrangement: two listed");
  // transport and selection
  r = Fixture::data(f.callJ("transport", "{\"action\": \"play\", \"position\": 8}"));
  expect(f.host.playing() && f.host.position() == 8 * f.doc.song().ppq && r.find("playing")->boolean, "transport: plays from beat 8");
  f.callJ("select", "{\"channels\": [2, 3], \"pattern\": 2}");
  expect(f.host.selectedChannels().size() == 2 && f.host.currentPattern() == f.doc.song().patterns[1].id, "select: channels and pattern");
  // help
  r = Fixture::data(f.callJ("help", "{}"));
  expect(contains(r.text("text"), "inspect"), "help: an overview");
  r = Fixture::data(f.callJ("help", "{\"topic\": \"patch\"}"));
  expect(r.find("examples") && !r.find("examples")->array.empty(), "help: a tool's examples");
  expect(Fixture::isError(f.callJ("help", "{\"topic\": \"nothing\"}")), "help: an unknown topic says the topics");
  // summary
  r = Fixture::data(f.call("get_song"));
  expect(r.find("stageRooms") && r.find("arrangements") && r.find("transport"), "get_song: rooms, arrangements and the transport");
}

void appTests() {
  Fixture f;
  expect(contains(Fixture::text(f.callJ("run_command", "{\"id\": \"cmd:file.save\"}")), "needs the Gridline app"), "app: headless run_command says the app is needed");
  // A stand-in app: two rows; running one edits the project.
  std::vector<std::string> ran;
  f.env.app.commands = [] {
    std::vector<AppBridge::Command> v(3);
    v[0].id = "menu:Patterns/Clone", v[0].kind = "command", v[0].label = "Clone pattern";
    v[1].id = "cmd:file.new", v[1].kind = "command", v[1].label = "New";
    v[2].id = "menu:Tools/Off", v[2].kind = "command", v[2].label = "Off", v[2].enabled = false, v[2].hint = "Open a pattern first";
    return v;
  };
  f.env.app.runCommand = [&](const std::string& id, bool& queued, std::string&) {
    ran.push_back(id);
    queued = false;
    f.doc.clonePattern(f.doc.song().patterns[0].id);
    return true;
  };
  Json r = Fixture::data(f.callJ("list_commands", "{\"query\": \"clone\"}"));
  expect(!r.find("commands")->array.empty() && r.find("commands")->array[0].text("id") == "menu:Patterns/Clone", "app: list_commands ranks the rows");
  f.edit("run_command", "{\"id\": \"menu:Patterns/Clone\"}", "app: run_command runs a row as one step");
  expect(ran.size() == 1, "app: the row ran once");
  expect(contains(f.refused("run_command", "{\"id\": \"cmd:file.new\"}", "app: a command replacing the project is refused"), "ask the user"), "app: the refusal says why");
  expect(contains(f.refused("run_command", "{\"id\": \"menu:Tools/Off\"}", "app: a disabled row is refused"), "Open a pattern first"), "app: the refusal gives the row's reason");
  f.refused("run_command", "{\"id\": \"menu:None\"}", "app: an unknown row is refused");
}

}  // namespace

int main() {
  commandCoverage();
  pluginCoverage();
  settingsCoverage();
  fieldCoverage();
  toolCoverage();
  inspectPatchTests();
  batchTests();
  parameterTests();
  familyTests();
  appTests();
  return finish();
}

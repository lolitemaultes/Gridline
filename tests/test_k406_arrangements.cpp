// Gridline — K-406 multiple arrangements unit tests:
// reorder (moveArrangement), 1-undo-step operations, JSON save/load round-trip
// + legacy single-arrangement compatibility, specific & all-arrangement export,
// MCP arrangement tool (incl. move), and 2-peer collaboration sync.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Arrangements.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/Mcp.h"
#include "core/Project.h"
#include "core/ProjectScript.h"
#include "engine/ExportJob.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

Json parseJ(const std::string& s) {
  Json j;
  jsonParse(s, j);
  return j;
}

Song makeTwoArrangementSong() {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  // Add one simple synth channel.
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Lead";
  ch.plugin = defaultPluginState("gl.synth");
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);

  // Pattern 1 (1 bar, C5) and Pattern 2 (2 bars, G5).
  const ObjectId p1Id = s.patterns[0].id;
  {
    Pattern& p1 = s.patterns[0];
    p1.name = "Main Pat";
    p1.bars = 1;
    Note n;
    n.start = 0;
    n.length = s.beatTicks() * 2;
    n.pitch = 60;
    n.velocity = 100;
    p1.notes[ch.id].push_back(n);
  }
  Pattern p2;
  p2.id = s.allocateId();
  p2.name = "Radio Pat";
  p2.bars = 2;
  {
    Note n;
    n.start = 0;
    n.length = s.beatTicks() * 3;
    n.pitch = 67;
    n.velocity = 100;
    p2.notes[ch.id].push_back(n);
  }
  s.patterns.push_back(p2);

  // Arrangement 0: "Main Mix" with 1-bar clip + marker.
  ensureArrangementList(s);
  renameArrangement(s, 0, "Main Mix");
  {
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Pattern;
    c.pattern = p1Id;
    c.track = 0;
    c.start = 0;
    c.length = s.barTicks();
    s.playlist.push_back(c);
    Marker m;
    m.pos = 0;
    m.name = "Intro";
    s.markers.push_back(m);
  }
  // Arrangement 1: "Radio Edit" with 2-bar clip + its own marker.
  const int radioIdx = addArrangement(s, "Radio Edit", false);
  selectArrangement(s, radioIdx);
  {
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Pattern;
    c.pattern = p2.id;
    c.track = 1;
    c.start = 0;
    c.length = s.barTicks() * 2;
    s.playlist.push_back(c);
    Marker m;
    m.pos = s.barTicks();
    m.name = "Hook";
    s.markers.push_back(m);
  }
  selectArrangement(s, 0);
  return s;
}

void testCoreMoveAndHelpers() {
  Song s = makeTwoArrangementSong();
  const int extIdx = addArrangement(s, "Extended", true);
  expect(extIdx == 2 && arrangementCount(s) == 3, "three arrangements created");
  expect(s.currentArrangement == 0, "adding does not change current");
  expect(arrangementLengthTicks(s, 0) == s.barTicks(), "active arrangement length");
  expect(arrangementLengthTicks(s, 1) == s.barTicks() * 2, "parked arrangement length");
  expect(arrangementLengthTicks(s, 2) == s.barTicks(), "duplicated parked arrangement length");

  // Move current (0 -> 2): currentArrangement follows it to 2.
  expect(moveArrangement(s, 0, 2), "move current 0 -> 2");
  expect(s.currentArrangement == 2, "current follows moved item to 2");
  expect(arrangementName(s, 0) == "Radio Edit" && arrangementName(s, 1) == "Extended" && arrangementName(s, 2) == "Main Mix",
         "order after moving 0 -> 2");
  expect(s.playlist.size() == 1 && s.markers.size() == 1 && s.markers[0].name == "Intro", "active data stays intact");

  // Move parked (0 -> 2) across current (2): current shifts down to 1.
  expect(moveArrangement(s, 0, 2), "move parked 0 -> 2 across current");
  expect(s.currentArrangement == 1, "current shifted to 1");
  expect(arrangementName(s, 0) == "Extended" && arrangementName(s, 1) == "Main Mix" && arrangementName(s, 2) == "Radio Edit",
         "order after moving parked across current");

  // Out of bounds rejected.
  expect(!moveArrangement(s, -1, 0) && !moveArrangement(s, 0, 99), "out-of-bounds move rejected");

  // songWithArrangement leaves original untouched and exposes target arrangement.
  Song radioCopy = songWithArrangement(s, 2);
  expect(radioCopy.currentArrangement == 2 && radioCopy.markers.size() == 1 && radioCopy.markers[0].name == "Hook",
         "songWithArrangement exposes parked arrangement");
  expect(s.currentArrangement == 1 && s.markers[0].name == "Intro", "original song unchanged by songWithArrangement");
}

void testUndoOneStepPerAction() {
  Document doc(makeTwoArrangementSong());
  const size_t pos0 = doc.history().position();
  doc.editSong("switch arrangement", [](Song& s) { selectArrangement(s, 1); });
  expect(doc.history().position() == pos0 + 1, "switch arrangement is 1 undo step");
  expect(doc.song().currentArrangement == 1 && arrangementName(doc.song(), 1) == "Radio Edit", "switched to Radio Edit");

  doc.editSong("reorder arrangement", [](Song& s) { moveArrangement(s, 1, 0); });
  expect(doc.history().position() == pos0 + 2, "reorder arrangement is 1 undo step");
  expect(doc.song().currentArrangement == 0 && arrangementName(doc.song(), 0) == "Radio Edit", "reordered to index 0");

  expect(doc.undo(), "undo reorder");
  expect(doc.song().currentArrangement == 1 && arrangementName(doc.song(), 0) == "Main Mix", "undo restored order");

  expect(doc.undo(), "undo switch");
  expect(doc.song().currentArrangement == 0 && arrangementName(doc.song(), 0) == "Main Mix", "undo restored previous arrangement");

  expect(doc.redo() && doc.redo(), "redo both steps");
  expect(doc.song().currentArrangement == 0 && arrangementName(doc.song(), 0) == "Radio Edit", "redo matches reordered state");
}

void testJsonRoundTripAndLegacy() {
  Song s = makeTwoArrangementSong();
  moveArrangement(s, 1, 0);
  const std::string json = jsonStringify(songToJson(s), 0);
  Song loaded;
  std::string err;
  const bool parsedOk = songFromJson(parseJ(json), loaded, &err);
  if (!parsedOk) std::printf("# songFromJson err: %s\n", err.c_str());
  expect(parsedOk, "multi-arrangement JSON parses");
  expect(arrangementCount(loaded) == 2 && loaded.currentArrangement == 1, "arrangement count & current preserved");
  expect(arrangementName(loaded, 0) == "Radio Edit" && arrangementName(loaded, 1) == "Main Mix", "arrangement names & order preserved");
  expect(arrangementLengthTicks(loaded, 0) == s.barTicks() * 2 && arrangementLengthTicks(loaded, 1) == s.barTicks(),
         "both active and parked lengths preserved");

  // Legacy single-arrangement project (empty arrangements vector) still loads cleanly.
  Song legacy = makeEmptySong();
  expect(legacy.arrangements.empty() && arrangementCount(legacy) == 1 && arrangementName(legacy, 0) == "Arrangement",
         "legacy single-arrangement defaults");
  Song legacyLoaded;
  expect(songFromJson(parseJ(jsonStringify(songToJson(legacy), 0)), legacyLoaded, &err), "legacy song round-trips");
  expect(legacyLoaded.arrangements.empty() && arrangementCount(legacyLoaded) == 1, "legacy song stays compact");
}

void testExportArrangements() {
  Song s = makeTwoArrangementSong();
  const fs::path dir = fs::temp_directory_path() / "gl_k406_export_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  // 1. Export specific parked arrangement (index 1 = Radio Edit, 2 bars) while index 0 (Main Mix, 1 bar) is active.
  {
    ExportSettings es;
    es.folder = (dir / "single").string();
    es.baseName = "Track";
    es.formats = {ExportFormat::Wav};
    es.wavBits = 16;
    es.render.sampleRate = 22050;
    es.render.tail = TailMode::Cut;
    es.arrangement = 1;
    ExportOutcome o = runExport(s, es);
    expect(o.errors.empty() && o.files.size() == 1, "single parked arrangement export succeeded");
    // 2 bars at 120 BPM = 4.0 s = 88200 frames at 22050 Hz (1 bar would be 44100).
    expect(o.frames > 75000 && o.peak > 0.01f, "exported parked arrangement has 2-bar length and audio");
  }

  // 2. Export ALL arrangements (arrangement = -2).
  {
    ExportSettings es;
    es.folder = (dir / "all").string();
    es.baseName = "Track";
    es.formats = {ExportFormat::Wav};
    es.wavBits = 16;
    es.render.sampleRate = 22050;
    es.render.tail = TailMode::Cut;
    es.arrangement = -2;
    ExportOutcome o = runExport(s, es);
    expect(o.errors.empty() && o.files.size() == 2, "all-arrangements export wrote 2 files");
    bool hasMain = false, hasRadio = false;
    for (const auto& f : o.files) {
      if (f.find("Main Mix") != std::string::npos) hasMain = true;
      if (f.find("Radio Edit") != std::string::npos) hasRadio = true;
    }
    expect(hasMain && hasRadio, "all-arrangements export named files after each arrangement");
  }
  fs::remove_all(dir, ec);
}

void testMcpMoveArrangement() {
  Document doc(makeTwoArrangementSong());
  script::SimpleProjectHost host{doc};
  mcp::Environment env{doc, host};
  mcp::Session session{env};
  double fakeNow = 1000.0;
  env.now = [&] { return fakeNow += 0.5; };
  auto rpc = [&](const std::string& method, Json params) {
    Json req = Json::makeObject();
    req.object["jsonrpc"] = Json::makeString("2.0");
    req.object["id"] = Json::makeNumber(1);
    req.object["method"] = Json::makeString(method);
    req.object["params"] = std::move(params);
    return parseJ(session.handle(jsonStringify(req, 0)));
  };
  {
    Json ip = Json::makeObject();
    ip.object["protocolVersion"] = Json::makeString("2025-06-18");
    Json ci = Json::makeObject();
    ci.object["name"] = Json::makeString("k406");
    ip.object["clientInfo"] = std::move(ci);
    rpc("initialize", std::move(ip));
  }
  Json p = Json::makeObject();
  p.object["name"] = Json::makeString("arrangement");
  p.object["arguments"] = parseJ("{\"action\": \"move\", \"arrangement\": 2, \"to\": 1}");
  Json r = rpc("tools/call", std::move(p));
  expect(r.find("error") == nullptr, "MCP arrangement move succeeded");
  expect(arrangementName(doc.song(), 0) == "Radio Edit" && arrangementName(doc.song(), 1) == "Main Mix",
         "MCP move reordered arrangements");
  expect(doc.song().currentArrangement == 1, "MCP move kept Main Mix active at index 1");
}

void testCollabSync() {
  sim::Clock clk;
  sim::Net net(406);
  sim::Peer a{"A", Document(makeTwoArrangementSong()), collab::crypto::Identity::generate(), nullptr, sim::tmpDir("k406_a")};
  sim::Peer b{"B", Document(makeEmptySong()), collab::crypto::Identity::generate(), nullptr, sim::tmpDir("k406_b")};
  std::string err;
  a.s = collab::Session::create(a.doc, sim::config(a.folder, a.id, "A", &clk), &err);
  expect(a.s != nullptr, "collab: host session created");
  b.s = collab::Session::join(b.doc, sim::config(b.folder, b.id, "B", &clk), sim::ticketOf(*a.s), &err);
  expect(b.s != nullptr, "collab: joiner session created");
  a.s->admitMember(sim::memberOf(b));
  net.add(&a);
  net.add(&b);
  net.connect(&a, &b);

  sim::settle(net, {&a, &b}, clk);
  expect(arrangementCount(b.doc.song()) == 2, "collab: peer B received both arrangements on join");

  // Peer A switches to Radio Edit, duplicates it, reorders it to index 0.
  a.doc.editSong("switch arrangement", [](Song& s) { selectArrangement(s, 1); });
  a.doc.editSong("duplicate arrangement", [](Song& s) { selectArrangement(s, addArrangement(s, "Club Mix", true)); });
  a.doc.editSong("reorder arrangement", [](Song& s) { moveArrangement(s, 2, 0); });
  sim::settle(net, {&a, &b}, clk);

  expect(arrangementCount(b.doc.song()) == 3, "collab: peer B has 3 arrangements");
  expect(b.doc.song().currentArrangement == 0 && arrangementName(b.doc.song(), 0) == "Club Mix",
         "collab: peer B synced current arrangement and order");
  expect(arrangementName(b.doc.song(), 1) == "Main Mix" && arrangementName(b.doc.song(), 2) == "Radio Edit",
         "collab: peer B synced all arrangement names in order");
  expect(b.doc.song().playlist.size() == a.doc.song().playlist.size(), "collab: active playlist clips match");

  // Peer B deletes Radio Edit (index 2).
  b.doc.editSong("delete arrangement", [](Song& s) { removeArrangement(s, 2); });
  sim::settle(net, {&a, &b}, clk);
  expect(arrangementCount(a.doc.song()) == 2 && arrangementName(a.doc.song(), 1) == "Main Mix",
         "collab: peer A received arrangement deletion from peer B");
}

}  // namespace

int main() {
  testCoreMoveAndHelpers();
  testUndoOneStepPerAction();
  testJsonRoundTripAndLegacy();
  testExportArrangements();
  testMcpMoveArrangement();
  testCollabSync();
  return finish();
}

// Gridline - K-255: a channel's lane in a pattern (its own grid, its length and the length's mode).
//  * The fields: defaults, save and load (an old file opens unchanged, a new one round trips),
//    undo of every edit, re-gridding set steps (nearest or in order), lengths in a lane's own steps.
//  * Collaboration: each field reaches the collaborator and one person's undo takes it back for both.
//  * The MCP tool set_lane and the pattern summary.
//  * Copies of a channel take its lane (clone, delete and undo).
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/Grouping.h"
#include "core/Lanes.h"
#include "core/Mcp.h"
#include "core/Project.h"
#include "core/ProjectScript.h"
#include "core/StepNotes.h"
#include "core/TrackerGrid.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

// The song as its file text (compact) and back.
std::string dump(const Json& j) { return jsonCompact(j); }
bool parse(const std::string& t, Json& out) { return jsonParse(t, out); }
std::string saved(const Song& s) { return jsonCompact(songToJson(s)); }
bool load(const std::string& t, Song& out, std::string* err) {
  Json j;
  return jsonParse(t, j) && songFromJson(j, out, err);
}

struct Rig {
  Document d;
  ObjectId pat = kNoId, kick = kNoId, shaker = kNoId;
  Rig() {
    d.reset(makeEmptySong());
    kick = d.addChannel("Kick", "gl.kick");
    shaker = d.addChannel("Shaker", "gl.synth");
    pat = d.song().patterns[0].id;
    d.setStep(pat, kick, 0, 100);
    d.setStep(pat, shaker, 0, 100);
    d.setStep(pat, shaker, 4, 90);
    d.setStep(pat, shaker, 12, 80);
  }
  const Pattern& p() const { return *d.song().pattern(pat); }
};

void testDefaults() {
  Rig r;
  const Pattern& p = r.p();
  expect(lanes::grid(p, r.kick) == 16 && !lanes::ownGrid(p, r.kick), "a row is on the pattern's grid");
  expect(lanes::steps(p, r.kick) == 16 && lanes::length(p, r.kick) == 0, "16 steps, no length of its own");
  expect(lanes::mode(p, r.kick) == LaneMode::Restart && !lanes::runsOn(p, r.kick), "restarts with the pattern");
  expect(!lanes::any(p) && lanes::plain(p, r.kick), "no lanes: plays as 0.7.7");
  expect(lanes::lineUpPasses(r.d.song(), p) == 1, "nothing runs on: lines up every pass");
}

void testGrid() {
  Rig r;
  const uint64_t before = r.d.revision();
  expect(lanes::setGrid(r.d, r.pat, {r.shaker}, 20), "the Shaker on 20 a bar");
  const Pattern& p = r.p();
  expect(lanes::grid(p, r.shaker) == 20 && lanes::ownGrid(p, r.shaker) && lanes::steps(p, r.shaker) == 20, "20 steps of its own");
  const auto& row = p.steps.at(r.shaker);
  // Nearest: 16ths 0, 4, 12 sound at 0, 1/4, 3/4 of the bar: steps 0, 5, 15 of 20.
  expect(row.size() == 20 && row[0] == 100 && row[5] == 90 && row[15] == 80, "set steps keep where they sound");
  expect(r.d.revision() != before, "one edit");
  r.d.undo();
  expect(!lanes::ownGrid(r.p(), r.shaker) && r.p().steps.at(r.shaker)[4] == 90, "undo: back to the pattern grid, steps as they were");
  expect(lanes::setGrid(r.d, r.pat, {r.shaker}, 5, lanes::Regrid::Order), "5 a bar, keeping the order");
  const auto& r5 = r.p().steps.at(r.shaker);
  expect(r5.size() == 5 && r5[0] == 100 && r5[4] == 90, "order: step 5 stays step 5, step 13 is past the end");
  expect(!lanes::setGrid(r.d, r.pat, {r.shaker}, 5), "the same grid again: nothing recorded");
  expect(lanes::setGrid(r.d, r.pat, {r.shaker}, 0), "back to the pattern grid");
  expect(!lanes::ownGrid(r.p(), r.shaker) && r.p().lanes.empty(), "no lane entry left");
  expect(lanes::setGrid(r.d, r.pat, {r.shaker}, 16) == false, "the pattern's own count is the pattern grid");
  // Grid like every row.
  lanes::setGrid(r.d, r.pat, {r.shaker}, 12);
  expect(lanes::gridLikeAll(r.d, r.pat, r.shaker) && lanes::grid(r.p(), r.kick) == 12, "every row like this one");
  // Regrid helper on its own: two steps on one keep the louder.
  std::vector<uint8_t> row2 = {50, 120, 0, 0};
  std::vector<StepProps> props2;
  lanes::regridRow(row2, props2, 1, 4, 2, lanes::Regrid::Nearest);
  expect(row2.size() == 2 && row2[0] == 50 && row2[1] == 120, "4 to 2: steps land on the nearest new step");
  std::vector<uint8_t> row3 = {50, 120, 0, 0};
  lanes::regridRow(row3, props2, 1, 4, 1, lanes::Regrid::Nearest);
  expect(row3.size() == 1 && row3[0] == 120, "two on one: the louder stays");
}

void testLength() {
  Rig r;
  lanes::setGrid(r.d, r.pat, {r.shaker}, 20);
  expect(lanes::setLength(r.d, r.pat, {r.shaker}, 17), "a length of 17 of its 20 steps");
  expect(lanes::length(r.p(), r.shaker) == 17, "length in the lane's own steps");
  expect(ops::setStepLoop(r.d, r.pat, {r.shaker}, 18) && lanes::length(r.p(), r.shaker) == 18, "the step loop menu counts the lane's steps");
  expect(!lanes::setLength(r.d, r.pat, {r.shaker}, 18), "no change: nothing recorded");
  const LaneMode on = LaneMode::RunOn;
  expect(lanes::setLength(r.d, r.pat, {r.shaker}, 7, &on), "length and mode together");
  expect(lanes::length(r.p(), r.shaker) == 7 && lanes::runsOn(r.p(), r.shaker), "7 steps, running on");
  r.d.undo();
  expect(lanes::length(r.p(), r.shaker) == 18 && !lanes::runsOn(r.p(), r.shaker), "one undo step takes both back");
  expect(lanes::setLength(r.d, r.pat, {r.shaker}, 20) && lanes::length(r.p(), r.shaker) == 0, "the whole row: no loop");
  // Nearest re-gridding scales the length.
  lanes::setLength(r.d, r.pat, {r.shaker}, 10);
  lanes::setGrid(r.d, r.pat, {r.shaker}, 0);
  expect(lanes::length(r.p(), r.shaker) == 8, "20 to 16 a bar: a length of 10 becomes 8");
  expect(lanes::setMode(r.d, r.pat, {r.kick}, LaneMode::RunOn) && lanes::mode(r.p(), r.kick) == LaneMode::RunOn, "mode alone");
  expect(!lanes::runsOn(r.p(), r.kick), "a mode without a length does not run on");
  expect(lanes::setMode(r.d, r.pat, {r.kick}, LaneMode::Restart) && !r.p().lanes.count(r.kick), "back to restart: no entry");
}

void testFiles() {
  Rig r;
  // An old file (no lanes) opens unchanged and saves without the new key.
  const std::string old = saved(r.d.song());
  expect(old.find("\"lanes\"") == std::string::npos, "a song without lanes writes no lanes key");
  Song back;
  std::string err;
  expect(load(old, back, &err) && back.patterns[0] == r.d.song().patterns[0], "an old file opens unchanged");
  lanes::setGrid(r.d, r.pat, {r.shaker}, 7);
  const LaneMode on = LaneMode::RunOn;
  lanes::setLength(r.d, r.pat, {r.shaker}, 5, &on);
  StepProps sp;
  sp.pitch = 64;
  r.d.editPattern(r.pat, "props", [&](Pattern& q) {
    q.stepProps[r.shaker] = std::vector<StepProps>(7);
    q.stepProps[r.shaker][6] = sp;  // past the pattern grid's 16? no: step 7 of 7 (index 6)
  });
  const std::string now = saved(r.d.song());
  expect(now.find("\"lanes\"") != std::string::npos && now.find("\"runOn\"") != std::string::npos, "lanes saved");
  Song back2;
  expect(load(now, back2, &err), "loads");
  expect(back2.patterns[0] == r.d.song().patterns[0], "round trip: grid, length, mode and properties");
  // Step properties past the pattern grid's count are kept for a lane with more steps.
  lanes::setGrid(r.d, r.pat, {r.shaker}, 24);
  r.d.editPattern(r.pat, "props", [&](Pattern& q) {
    q.stepProps[r.shaker] = std::vector<StepProps>(24);
    q.stepProps[r.shaker][20] = sp;
  });
  Song back3;
  expect(load(saved(r.d.song()), back3, &err) && back3.patterns[0].stepProps.at(r.shaker).size() == 24 &&
             back3.patterns[0].stepProps.at(r.shaker)[20].pitch == 64,
         "properties of step 21 of 24 survive the round trip");
  // A lane entry naming the pattern's own count reads as the pattern grid.
  std::string j = now;
  const size_t at = j.find("\"grid\":7");
  expect(at != std::string::npos, "grid written as a number");
  if (at != std::string::npos) j.replace(at, 8, "\"grid\":16");
  Song back4;
  expect(load(j, back4, &err) && !lanes::ownGrid(back4.patterns[0], r.shaker), "a grid equal to the pattern's is no grid of its own");
}

void testChannelCopies() {
  Rig r;
  lanes::setGrid(r.d, r.pat, {r.shaker}, 5);
  const ObjectId c = r.d.cloneChannel(r.shaker, Document::CloneNotes::All);
  expect(c != kNoId && lanes::grid(r.p(), c) == 5, "a clone keeps the lane's grid");
  const PatternLane before = lanes::of(r.p(), r.shaker);
  r.d.removeChannel(r.shaker);
  expect(!r.p().lanes.count(r.shaker), "a deleted channel's lane goes with it");
  r.d.undo();
  expect(lanes::of(r.p(), r.shaker) == before, "undo brings the lane back");
}

void testMcp() {
  Rig r;
  script::SimpleProjectHost host{r.d};
  mcp::Environment env{r.d, host};
  mcp::Session session{env};
  int id = 1;
  auto rpc = [&](const std::string& method, Json params) {
    Json m = mcp::jobj({{"jsonrpc", mcp::jstr("2.0")}, {"id", mcp::jnum(id++)}, {"method", mcp::jstr(method)}, {"params", params}});
    Json out;
    parse(session.handle(dump(m)), out);
    return out;
  };
  rpc("initialize", mcp::jobj({{"protocolVersion", mcp::jstr("2025-06-18")}, {"capabilities", Json::makeObject()},
                               {"clientInfo", mcp::jobj({{"name", mcp::jstr("t")}, {"version", mcp::jstr("1")}})}}));
  session.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  const size_t hist = r.d.history().position();
  Json res = rpc("tools/call", mcp::jobj({{"name", mcp::jstr("set_lane")},
                                          {"arguments", mcp::jobj({{"pattern", mcp::jnum(1)}, {"channel", mcp::jnum(2)}, {"grid", mcp::jnum(20)},
                                                                   {"length", mcp::jnum(7)}, {"mode", mcp::jstr("runOn")}})}}));
  const Json* sc = res.find("result") ? res.find("result")->find("structuredContent") : nullptr;
  expect(sc && sc->num("grid", 0) == 20 && sc->num("length", 0) == 7 && sc->text("mode") == "runOn", "set_lane sets grid, length and mode");
  expect(lanes::grid(r.p(), r.shaker) == 20 && lanes::runsOn(r.p(), r.shaker), "the song has them");
  expect(r.d.history().position() == hist + 1, "one undo step");
  Json info = rpc("tools/call", mcp::jobj({{"name", mcp::jstr("get_pattern")}, {"arguments", mcp::jobj({{"pattern", mcp::jnum(1)}})}}));
  const std::string txt = dump(info);
  expect(txt.find("\"grid\"") != std::string::npos && txt.find("runOn") != std::string::npos, "the pattern summary lists the lane");
  Json bad = rpc("tools/call", mcp::jobj({{"name", mcp::jstr("set_lane")},
                                          {"arguments", mcp::jobj({{"pattern", mcp::jnum(1)}, {"channel", mcp::jnum(2)}, {"mode", mcp::jstr("sideways")}})}}));
  expect(dump(bad).find("isError") != std::string::npos || dump(bad).find("error") != std::string::npos, "an unknown mode is refused");
  // The generic set on the project JSON reaches the field too.
  Json set = rpc("tools/call", mcp::jobj({{"name", mcp::jstr("set_steps")},
                                          {"arguments", mcp::jobj({{"pattern", mcp::jnum(1)}, {"channel", mcp::jnum(2)},
                                                                   {"steps", mcp::jarr(std::vector<Json>(20, mcp::jnum(100)))}})}}));
  expect(r.p().steps.at(r.shaker).size() == 20, "set_steps takes the lane's own 20 steps");
}

// Each field reaches the collaborator; one person's undo takes it back for both.
void testCollab() {
  Clock clock;
  Net net(7855);
  Peer a, b;
  a.label = "Alex", b.label = "Sam";
  a.id = crypto::Identity::generate(), b.id = crypto::Identity::generate();
  a.doc.reset(makeEmptySong());
  const ObjectId sh = a.doc.addChannel("Shaker", "gl.synth");
  const ObjectId pat = a.doc.song().patterns[0].id;
  a.doc.setStep(pat, sh, 4, 100);
  a.folder = tmpDir("k255_a"), b.folder = tmpDir("k255_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  auto bp = [&]() -> const Pattern& { return *b.doc.song().pattern(pat); };
  lanes::setGrid(a.doc, pat, {sh}, 5);
  settle(net, {&a, &b}, clock);
  expect(lanes::grid(bp(), sh) == 5 && bp().steps.at(sh).size() == 5, "a grid reaches the collaborator (with its steps)");
  lanes::setLength(b.doc, pat, {sh}, 3);
  settle(net, {&a, &b}, clock);
  expect(lanes::length(*a.doc.song().pattern(pat), sh) == 3, "a length from the collaborator arrives");
  lanes::setMode(a.doc, pat, {sh}, LaneMode::RunOn);
  settle(net, {&a, &b}, clock);
  expect(lanes::runsOn(bp(), sh), "the mode reaches the collaborator");
  expect(songText(a) == songText(b), "both songs the same");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(!lanes::runsOn(bp(), sh) && lanes::length(bp(), sh) == 3, "undo of the mode: restarts again for both, the length stays");
  b.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(lanes::length(*a.doc.song().pattern(pat), sh) == 0, "the collaborator's undo takes the length back for both");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(!lanes::ownGrid(bp(), sh) && bp().steps.at(sh)[4] == 100 && songText(a) == songText(b), "undo of the grid: steps back on the pattern grid");
}

// The Tracker and the piano roll read a lane the same way the rack does.
void testViews() {
  Rig r;
  const Song& s = r.d.song();
  lanes::setGrid(r.d, r.pat, {r.shaker}, 5, lanes::Regrid::Order);  // steps 0 and 4 stay (12 drops past 5)
  const trk::Grid g = trk::gridOf(s, r.p());
  const trk::Track t = trk::buildTrack(s, r.p(), r.shaker, g);
  expect(t.ownGrid && t.loop == 0, "a lane on its own grid in the Tracker");
  // Step 4 of 5 sits at 4/5 of a bar = row 12.8: shown on row 13, marked off the grid.
  bool row13 = false, row0 = false;
  for (int c = 0; c < t.columns; ++c) {
    const trk::Cell& a = t.at(13, c);
    if (a.kind == trk::Cell::Step && a.step == 4 && a.offGrid) row13 = true;
    const trk::Cell& b = t.at(0, c);
    if (b.kind == trk::Cell::Step && b.step == 0 && !b.offGrid) row0 = true;
  }
  expect(row0 && row13, "lane steps at their nearest rows (step 4 of 5 on row 13, marked ~)");
  // A track loop at row 8: 8 rows on the pattern grid; on 5 a bar, the nearest lane step (2.5 -> 3).
  expect(trk::loopStepsAtRow(s, r.p(), r.kick, g, 8) == 8 && trk::loopStepsAtRow(s, r.p(), r.shaker, g, 8) == 3, "the loop at a row, in the lane's steps");
  expect(trk::loopStepsAtRow(s, r.p(), r.kick, g, 0) == 0, "no loop at the first row");
  lanes::setLength(r.d, r.pat, {r.kick}, 6);
  lanes::setLength(r.d, r.pat, {r.shaker}, 3);
  expect(trk::buildTrack(s, r.p(), r.kick, g).loop == 6 && trk::loopRows(s, r.p(), r.shaker, g) == 10, "the loop in rows (3 of 5 a bar: row 9.6 -> 10)");
  // The piano roll shows the lane's steps at their exact places.
  const std::vector<Note> ns = stepnotes::fromSteps(s, r.p(), r.shaker);
  expect(ns.size() == 2 && ns[1].start == 307 && ns[1].frac > 0.19f && ns[1].frac < 0.21f, "step 4 of 5 at 307.2 ticks in the piano roll");
  expect(stepnotes::hasSteps(s, r.p(), r.shaker), "the piano roll sees the lane's steps");
}

}  // namespace

int main() {
  testDefaults();
  testGrid();
  testLength();
  testFiles();
  testChannelCopies();
  testMcp();
  testCollab();
  testViews();
  return finish();
}

// Gridline - Stage rooms: a project's mixer tracks grouped into rooms with their own stage settings.
// Rooms and their edits (one undo step each), moving tracks between rooms (alone, as a group, with
// their feeders), templates (factory and the user's own, saved and listed), save / load (a one-room
// project stays as it was), the engine (each room's own sound, one room bit for bit as before, room
// width), the box selection rules, collaboration and the FL export.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/LayerRack.h"
#include "core/MixerOps.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/StageRoomOps.h"
#include "core/StageRooms.h"
#include "core/StageSelect.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/Effects.h"
#include "engine/Program.h"

using namespace gl;
namespace ro = gl::roomops;
namespace sel = gl::stagesel;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
bool has(const std::vector<ObjectId>& v, ObjectId id) { return std::find(v.begin(), v.end(), id) != v.end(); }

const Send* sendTo(const Song& s, ObjectId from, ObjectId to) {
  const MixerTrack* m = s.mixerTrack(from);
  if (!m) return nullptr;
  for (const auto& sd : m->sends)
    if (sd.to == to) return &sd;
  return nullptr;
}

// One step, undone and redone: the song comes back exactly each way.
void oneStep(Document& d, const char* what, const std::function<void()>& fn) {
  d.endGesture();
  const size_t at = d.history().position();
  const Song before = d.song();
  fn();
  d.endGesture();
  const bool one = d.history().position() == at + 1;
  const Song after = d.song();
  d.undo();
  const bool back = d.history().position() == at && d.song() == before;
  d.redo();
  expect(one && back && d.song() == after, what);
}

// ---------------------------------------------------------------- rooms
void testRooms() {
  Document d(makeEmptySong());
  const ObjectId t1 = d.song().mixer[1].id, t2 = d.song().mixer[2].id, t3 = d.song().mixer[3].id;
  expect(ro::roomCount(d.song()) == 1 && ro::roomName(d.song(), 0) == "Room 1" && ro::trackRoom(d.song(), t1) == 0,
         "a new project has one room and every track stands in it");
  // The first room gets a space: its track is "Room" while it is the only room.
  const ObjectId sp0 = mixops::ensureStageRoom(d, {t1, t2, t3});
  expect(d.song().mixerTrack(sp0) && d.song().mixerTrack(sp0)->name == "Room", "one room: its space track is called Room");
  mixops::setRoomType(d, rooms::kHall);
  mixops::setDepthStrength(d, 0.8, false);
  mixops::setRoomWidth(d, 1.2, false);
  int r1 = -1;
  oneStep(d, "a new room: one step", [&] { r1 = ro::addRoom(d, 0, nullptr, "Second song"); });
  const Song& s = d.song();
  expect(r1 == 1 && ro::roomCount(s) == 2 && ro::roomName(s, 1) == "Second song", "the new room comes after the last");
  expect(ro::roomAt(s, 1).roomType == rooms::kHall && near(ro::roomAt(s, 1).depthStrength, 0.8) && near(ro::roomAt(s, 1).width, 1.2, 1e-6),
         "a new room copies the room it came from: space, depth and width");
  expect(ro::roomTracks(s, 1).empty(), "a new room starts empty (no tracks or places copied)");
  const MixerTrack* sp1 = mixops::stageRoom(s, 1);
  expect(sp1 && sp1->id != sp0 && sp1->name == "Second song space" && ro::trackRoom(s, sp1->id) == 1 && ro::isSpaceTrack(s, sp1->id),
         "the new room's space track is made at once, named after it, standing in it");
  expect(d.song().mixerTrack(sp0)->name == "Room 1 space", "with two rooms the first room's space track is named after its room");
  expect(s.stage.id != kNoId && ro::roomIdAt(s, 1) != kNoId && ro::roomIdAt(s, 1) != s.stage.id, "every room has an id once there are two");
  expect(ro::roomTracks(s, 0).size() + 1 == s.mixer.size() - 2, "the other tracks are still in the first room");
  // The new room's space plays the hall too (the space's reverb copied).
  expect(near(mixops::roomSize(s, 1), mixops::roomSize(s, 0), 1e-6), "the new space sounds like the old one (its size)");

  oneStep(d, "rename a room: one step", [&] { ro::renameRoom(d, 1, "Track two"); });
  expect(ro::roomName(d.song(), 1) == "Track two" && mixops::stageRoom(d.song(), 1)->name == "Track two space", "renaming renames its space track");
  oneStep(d, "room colour: one step", [&] { ro::setRoomColor(d, 1, 0x336699); });
  expect(ro::roomColor(d.song(), 1) == 0x336699 && ro::roomColor(d.song(), 0) != 0, "room colours: chosen or from the palette");
  oneStep(d, "room width: one step", [&] { mixops::setRoomWidth(d, 0.5, false, 1); });
  expect(near(ro::roomAt(d.song(), 1).width, 0.5, 1e-6) && near(ro::roomAt(d.song(), 0).width, 1.2, 1e-6), "room width is per room");
  oneStep(d, "a room's depth: one step and only that room", [&] { mixops::setDepthStrength(d, 0.1, false, 1); });
  expect(near(ro::roomAt(d.song(), 0).depthStrength, 0.8, 1e-6), "the other room keeps its depth");

  // From a template.
  int r2 = -1;
  const ro::RoomTemplate& club = ro::factoryTemplates()[2];
  oneStep(d, "a new room from a template: one step", [&] { r2 = ro::addRoom(d, 1, &club, "Third"); });
  expect(r2 == 2 && ro::roomAt(d.song(), 2).roomType == club.roomType && near(ro::roomAt(d.song(), 2).width, club.width, 1e-6),
         "a template gives its settings");
  // Reorder, with the first room changing place.
  const ObjectId firstId = d.song().stage.id;
  oneStep(d, "moving a room: one step", [&] { ro::moveRoom(d, 0, 2); });
  expect(ro::roomIdAt(d.song(), 2) == firstId && ro::trackRoom(d.song(), t1) == 2 && ro::roomName(d.song(), 0) == "Track two",
         "moving the first room: its tracks go with it");
  ro::moveRoom(d, 2, 0);
  expect(ro::trackRoom(d.song(), t1) == 0 && ro::roomIdAt(d.song(), 0) == firstId, "and back");
  // Delete (K-270): a room with tracks is refused; once they are moved out it goes in one step.
  d.setParam(mixerParam(t3, MixerParam::Pan), 0.2, false);  // a place on the stage: in use
  ro::moveTracksToRoom(d, {t3}, 2);
  d.endGesture();
  const size_t at = d.history().position();
  expect(!ro::deleteRoom(d, 2) && ro::roomCount(d.song()) == 3 && d.history().position() == at, "a room with a track in it is not deleted");
  ro::moveTracksToRoom(d, {t3}, 1);
  oneStep(d, "deleting an empty room: one step", [&] { ro::deleteRoom(d, 2); });
  expect(ro::roomCount(d.song()) == 2 && ro::trackRoom(d.song(), t3) == 1, "the moved track stays where it was moved");
}

// ---------------------------------------------------------------- moving tracks
void testMoveTracks() {
  Document d(makeEmptySong());
  const ObjectId t1 = d.song().mixer[1].id, t2 = d.song().mixer[2].id, t3 = d.song().mixer[3].id, t4 = d.song().mixer[4].id;
  const ObjectId sp0 = mixops::ensureStageRoom(d, {t1, t2, t3});
  mixops::setRoomType(d, rooms::kStudio);
  d.setParam(mixerParam(t1, MixerParam::Pan), -0.4, false);
  d.setParam(mixerParam(t1, MixerParam::Depth), 0.6, false);
  d.setParam(mixerParam(t2, MixerParam::Pan), 0.3, false);
  const ObjectId custom = t3;  // a room send someone turned up
  d.setSend(custom, sp0, 0.5f);
  const int r1 = ro::addRoom(d, 0, &ro::factoryTemplates()[3], "Song two");  // a concert hall
  const ObjectId sp1 = mixops::stageRoom(d.song(), r1)->id;
  // A group of tracks dragged into the other room: one step.
  int moved = 0;
  oneStep(d, "a group dropped on another room: one undo step", [&] {
    d.beginPointerGesture();
    moved = ro::moveTracksToRoom(d, {t1, t2, t3, t4}, r1);
    d.endPointerGesture();
  });
  const Song& s = d.song();
  expect(moved == 4 && ro::trackRoom(s, t1) == r1 && ro::trackRoom(s, t2) == r1 && ro::trackRoom(s, t3) == r1 && ro::trackRoom(s, t4) == r1,
         "the whole group moves to the room it was dropped on");
  expect(near(s.mixerTrack(t1)->pan, -0.4, 1e-6) && near(s.mixerTrack(t1)->depth, 0.6, 1e-6) && near(s.mixerTrack(t2)->pan, 0.3, 1e-6),
         "moved tracks keep their places");
  const Send* a = sendTo(s, t1, sp1);
  expect(a && a->stageRoom && near(a->level, mixops::roomSendLevel(s, r1), 1e-6) && !sendTo(s, t1, sp0),
         "their room send goes to the new room's space, at its level");
  const Send* c = sendTo(s, custom, sp1);
  expect(c && near(c->level, 0.5, 1e-6), "a room send someone changed keeps its level");
  const Send* f = sendTo(s, t4, sp1);
  expect(f && f->stageRoom && near(f->level, mixops::roomSendLevel(s, r1), 1e-6) && sendTo(s, t4, s.masterId()),
         "a track with no room send takes the new room's sound, at a new track's level there");
  // And back again: one step.
  oneStep(d, "the group dropped back on the first room: one undo step", [&] {
    d.beginPointerGesture();
    moved = ro::moveTracksToRoom(d, {t1, t2, t3, t4}, 0);
    d.endPointerGesture();
  });
  expect(moved == 4 && ro::trackRoom(d.song(), t1) == 0 && ro::trackRoom(d.song(), t4) == 0 && sendTo(d.song(), t1, sp0) && !sendTo(d.song(), t1, sp1),
         "vice versa: the group moves back, with its room sends");
  // A drag that only moved positions, then dropped on a room: one step for both.
  oneStep(d, "a drag that moves and then drops on a room: still one step", [&] {
    d.beginPointerGesture();
    d.setParam(mixerParam(t2, MixerParam::Pan), 0.1, false);
    d.setParam(mixerParam(t2, MixerParam::Pan), 0.3, false);  // back where it was (the rail reverts the move)
    ro::moveTracksToRoom(d, {t2}, r1);
    d.endPointerGesture();
  });
  expect(ro::moveTracksToRoom(d, {t2}, r1) == 0, "a track already in the room is skipped");
  expect(ro::moveTracksToRoom(d, {d.song().masterId(), sp0}, r1) == 0, "the master and space tracks never move");
  // Feeders: a track that sends into a moved bus comes too when asked.
  const ObjectId bus = d.song().mixer[6].id, src = d.song().mixer[7].id;
  d.setSend(src, d.song().masterId(), 0.0f);
  d.setSend(src, bus, 1.0f);
  expect(has(ro::feedersOf(d.song(), {bus}), src), "the tracks that feed a bus");
  ro::moveTracksToRoom(d, {bus}, r1, true);
  expect(ro::trackRoom(d.song(), bus) == r1 && ro::trackRoom(d.song(), src) == r1, "a bus brings the tracks that feed it");
  expect(sendTo(d.song(), bus, mixops::stageRoom(d.song(), r1)->id) && !sendTo(d.song(), src, mixops::stageRoom(d.song(), r1)->id),
         "the bus takes the room's sound, the track that only feeds it stays dry");
  // A new track lands in the room this computer looks at.
  d.setNewTrackRoom(ro::roomIdAt(d.song(), r1));  // session state (this computer), not an edit
  const ObjectId nt = d.addMixerTrack();
  expect(ro::trackRoom(d.song(), nt) == r1, "a new track stands in the room shown");
}

// ---------------------------------------------------------------- the arrangement's tracks
// A mixer track state or a layer preset is a sound: loading one never moves a track to another room.
void testStatesKeepRooms() {
  Document d(makeEmptySong());
  const ObjectId t1 = d.song().mixer[1].id, t2 = d.song().mixer[2].id;
  const int r1 = ro::addRoom(d, 0, nullptr, "Second song");
  ro::moveTracksToRoom(d, {t1}, r1);
  const std::string text = mixops::trackStateText(*d.song().mixerTrack(t1));
  expect(text.find("roomId") == std::string::npos, "a saved track state holds no room");
  expect(mixops::applyTrackState(d, t2, text) && ro::trackRoom(d.song(), t2) == 0, "loading a track state keeps the track in its room");
  // A state file that still carries a room (written by hand or by an early build) changes nothing either.
  Json j;
  jsonParse(text, j);
  j.object["roomId"] = Json::makeNumber(static_cast<double>(ro::roomIdAt(d.song(), r1)));
  expect(mixops::applyTrackState(d, t2, jsonStringify(j)) && ro::trackRoom(d.song(), t2) == 0, "a state with a room in it still keeps the room");
  // Layer presets: the children's inserts are saved without their room and load into a free track's room.
  Song s = d.song();
  Channel kid;
  kid.id = s.allocateId();
  kid.name = "kid";
  kid.plugin = defaultPluginState("gl.synth");
  kid.mixerTrack = t1;  // its own insert, in the second room
  s.channels.push_back(kid);
  Channel lay;
  lay.id = s.allocateId();
  lay.name = "layer";
  lay.mixerTrack = s.masterId();
  lay.layer = {kid.id};
  s.channels.push_back(lay);
  Json pj = layer::presetToJson(s, lay.id);
  bool roomless = false;
  if (const Json* kids = pj.find("children"); kids && kids->is(Json::Type::Array) && !kids->array.empty())
    if (const Json* ins = kids->array[0].find("insert")) roomless = !ins->find("roomId");
  expect(roomless, "a layer preset's insert holds no room");
  if (auto ch = pj.object.find("children"); ch != pj.object.end() && !ch->second.array.empty())
    if (auto ins = ch->second.array[0].object.find("insert"); ins != ch->second.array[0].object.end())
      ins->second.object["roomId"] = Json::makeNumber(static_cast<double>(ro::roomIdAt(d.song(), r1)));  // as an old preset may
  Document d2(makeEmptySong());
  const ObjectId nl = layer::loadPreset(d2, pj);
  const Channel* L = d2.song().channel(nl);
  const Channel* k1 = L && !L->layer.empty() ? d2.song().channel(L->layer[0]) : nullptr;
  const MixerTrack* m = k1 ? d2.song().mixerTrack(k1->mixerTrack) : nullptr;
  expect(m && m->id != d2.song().masterId() && m->room == kNoId && ro::trackRoom(d2.song(), *m) == 0, "a loaded layer preset's insert stays in the free track's room");
}

void testPicking() {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.mixerTrack = s.mixer[5].id;
  s.channels.push_back(c);
  Pattern p;
  p.id = s.allocateId();
  p.steps[c.id] = {100, 0, 0, 0};
  s.patterns.push_back(p);
  Clip clip;
  clip.pattern = p.id;
  s.playlist.push_back(clip);
  s.mixer[5].sends = {{s.mixer[8].id, 1.0f, false}};
  const std::vector<ObjectId> used = ro::tracksOfArrangement(s, 0);
  expect(has(used, s.mixer[5].id) && has(used, s.mixer[8].id) && used.size() == 2, "an arrangement's tracks: its channels' tracks and their buses");
  expect(ro::tracksOfPattern(s, p.id).size() == 2, "a pattern's tracks");
}

// ---------------------------------------------------------------- templates
void testTemplates() {
  const auto& f = ro::factoryTemplates();
  expect(f.size() == 6, "six factory templates");
  bool named = true;
  for (const auto& t : f) named = named && !t.name.empty() && !t.hint.empty() && t.factory;
  expect(named, "every factory template has a name and a hint");
  Document d(makeEmptySong());
  mixops::ensureStageRoom(d, {d.song().mixer[1].id});
  mixops::setRoomType(d, 7);
  mixops::setRoomSize(d, 0.9, false);
  mixops::setRoomWidth(d, 0.7, false);
  ro::RoomTemplate t = ro::templateOf(d.song(), 0, "Wide cave");
  ro::RoomTemplate back;
  expect(ro::templateFromJsonText(ro::templateToJsonText(t), back) && back.roomType == 7 && near(back.width, 0.7, 1e-6) && near(back.size, 0.9, 1e-5) &&
             back.reverb.size() == t.reverb.size(),
         "a template round-trips through its file text");
  expect(!ro::templateFromJsonText("{\"format\":\"something else\"}", back) && !ro::templateFromJsonText("nonsense", back), "other files are not templates");
  const std::string root = std::filesystem::temp_directory_path().string() + "/gl_k224_presets";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::string path;
  expect(ro::saveUserTemplate(root, t, "Caves/Wide cave", &path) && std::filesystem::exists(path), "a user template is saved in its folder");
  const auto mine = ro::listUserTemplates(root);
  expect(mine.size() == 1 && mine[0].name == "Wide cave" && mine[0].folder == "Caves" && !mine[0].factory, "and listed");
  // Applying it to another room: settings only.
  const int r = ro::addRoom(d, 0, &f[0], "Plain");
  const ObjectId t2 = d.song().mixer[2].id;
  ro::moveTracksToRoom(d, {t2}, r);
  d.setParam(mixerParam(t2, MixerParam::Pan), 0.25, false);
  oneStep(d, "applying a template: one step", [&] { ro::applyTemplate(d, r, mine[0]); });
  expect(ro::roomAt(d.song(), r).roomType == 7 && near(ro::roomAt(d.song(), r).width, 0.7, 1e-6) && near(mixops::roomSize(d.song(), r), 0.9, 1e-5),
         "the template's settings and space sound");
  expect(ro::trackRoom(d.song(), t2) == r && near(d.song().mixerTrack(t2)->pan, 0.25, 1e-6), "its tracks and places stay");
  expect(ro::deleteUserTemplate(mine[0]) && ro::listUserTemplates(root).empty() && !ro::deleteUserTemplate(f[0]), "user templates delete, factory ones never");
  std::filesystem::remove_all(root, ec);
}

// ---------------------------------------------------------------- save / load
void testSaveLoad() {
  {
    Document d(makeEmptySong());
    mixops::ensureStageRoom(d, {d.song().mixer[1].id});
    const Json j = songToJson(d.song());
    expect(static_cast<int>(j.num("version")) == 8 && !j.find("stageRooms"), "a one-room project saves as before (version 8)");
    Song back;
    expect(songFromJson(j, back) && back == d.song(), "and loads the same");
  }
  Document d(makeEmptySong());
  const ObjectId t1 = d.song().mixer[1].id;
  mixops::ensureStageRoom(d, {t1});
  const int r = ro::addRoom(d, 0, &ro::factoryTemplates()[4], "B side", 0x884422);
  ro::moveTracksToRoom(d, {t1}, r);
  mixops::setRoomWidth(d, 0.8, false, r);
  const Json j = songToJson(d.song());
  expect(static_cast<int>(j.num("version")) == 9 && j.find("stageRooms"), "two rooms: version 9");
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err) && back == d.song(), "rooms save and load exactly");
  expect(ro::trackRoom(back, t1) == r && ro::roomName(back, r) == "B side" && back.stageRooms[0].color == 0x884422, "membership, names and colours survive");
  expect(back.nextId > ro::roomIdAt(back, r), "room ids are never handed out again");
  // A newer file refuses; a file with a track in a room that is gone puts it in the first room.
  Json k = j;
  k.object["version"] = Json::makeNumber(10);
  expect(!songFromJson(k, back), "a newer schema is refused");
  Json g = j;
  g.object.erase("stageRooms");
  g.object["version"] = Json::makeNumber(9);
  expect(songFromJson(g, back) && ro::roomCount(back) == 1 && ro::trackRoom(back, t1) == 0, "a track whose room is gone stands in the first room");
}

// ---------------------------------------------------------------- the engine
class TestSine final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double f = s.params.empty() ? 100.0 : s.params[0];
    for (int i = 0; i < n; ++i, ++t_) {
      const float v = static_cast<float>(0.25 * std::sin(2.0 * M_PI * f * static_cast<double>(t_) / 44100.0));
      L[i] = v;
      R[i] = s.params.size() > 1 && s.params[1] > 0.5f ? -0.5f * v : v;
    }
  }
  void reset() override { t_ = 0; }

 private:
  long t_ = 0;
};
std::unique_ptr<Effect> testFactory(const std::string& type) {
  if (type == "test.sine") return std::make_unique<TestSine>();
  return nullptr;
}
struct Out {
  std::vector<float> L, R;
};
Out run(const Song& s, int frames, int block = 256) {
  ProgramBuilder b;
  AudioEngine e(44100);
  b.setSampleRate(44100);
  e.setThreads(1);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Pattern);
  e.play();
  Out o;
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  for (int done = 0; done < frames; done += block) {
    const int n = std::min(block, frames - done);
    e.process(L.data(), R.data(), n);
    o.L.insert(o.L.end(), L.begin(), L.begin() + n);
    o.R.insert(o.R.end(), R.begin(), R.begin() + n);
  }
  return o;
}
bool same(const Out& a, const Out& b) {
  return a.L.size() == b.L.size() && std::memcmp(a.L.data(), b.L.data(), a.L.size() * 4) == 0 && std::memcmp(a.R.data(), b.R.data(), a.R.size() * 4) == 0;
}
bool finite(const Out& o) {
  for (size_t i = 0; i < o.L.size(); ++i)
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) return false;
  return true;
}
double rms(const std::vector<float>& x) {
  double a = 0;
  for (float v : x) a += static_cast<double>(v) * v;
  return std::sqrt(a / static_cast<double>(std::max<size_t>(1, x.size())));
}

Song sineSong(float depth, float pan, float sep, bool invertR = false) {
  Song s = makeEmptySong();
  s.mixer[1].slots[0].plugin.type = "test.sine";
  s.mixer[1].slots[0].plugin.params = {220.0f, invertR ? 1.0f : 0.0f};
  s.mixer[1].depth = depth;
  s.mixer[1].pan = pan;
  s.mixer[1].stereoSep = sep;
  s.stage.roomType = static_cast<uint8_t>(rooms::kStudio);
  return s;
}

void testEngine() {
  const int N = 8192;
  // One room, ids and names written (a project that once had more rooms): bit for bit as before.
  {
    Song a = sineSong(0.6f, -0.3f, 0.2f, true);
    Song b = a;
    b.stage.id = b.allocateId();
    b.stage.name = "Only room";
    b.mixer[1].room = b.stage.id;
    expect(same(run(a, N), run(b, N)), "one room: renders bit for bit as a project without rooms");
  }
  // A track in a second room plays that room's depth.
  {
    Song a = sineSong(1.0f, 0, 0);
    Song b = a;
    StageSettings r;
    r.id = b.allocateId();
    r.roomType = static_cast<uint8_t>(rooms::kStudio);
    r.depthStrength = 1.0f;  // a stronger back of the stage
    b.stageRooms.push_back(r);
    Song c = b;
    c.mixer[1].room = r.id;
    const Out oa = run(a, N), ob = run(b, N), oc = run(c, N);
    expect(same(oa, ob), "an empty second room changes nothing");
    expect(finite(oc) && rms(oc.L) < rms(oa.L) * 0.9, "a track in another room plays that room's depth (its own distance gain)");
    Song e = c;
    e.stageRooms[0].roomType = 0;  // that room has no space: distance is not heard
    Song f = sineSong(0.0f, 0, 0);
    expect(same(run(e, N), run(f, N)), "a room with no space: its tracks' distance is silent, as in a one-room project");
  }
  // Room width: 1 exactly as the track; 0 a centred mono image; wider spreads further.
  {
    Song a = sineSong(0.0f, -0.5f, 0.0f, true);
    Song w1 = a;
    w1.stage.width = 1.0f;
    expect(same(run(a, N), run(w1, N)), "room width 1: bit for bit");
    Song w0 = a;
    w0.stage.width = 0.0f;
    const Out o = run(w0, N);
    double diff = 0;
    for (size_t i = 0; i < o.L.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(o.L[i] - o.R[i])));
    expect(diff < 1e-5, "room width 0: the room plays in mono, in the middle");
    Song wide = a;
    wide.stage.width = 1.5f;
    const Out ow = run(wide, N), on = run(a, N);
    expect(finite(ow) && rms(ow.L) > rms(on.L), "a wider room pans further out");
    Song m = a;
    m.stage.width = 0.0f;
    m.mixer[1].room = kNoId;
    Song keepMaster = m;
    keepMaster.mixer[0].stereoSep = 0.0f;
    expect(same(run(m, N), run(keepMaster, N)), "the room's width never touches the master");
  }
}

// ---------------------------------------------------------------- K-270: delete rules, soloed rooms
void testDeleteRules() {
  Document d(makeEmptySong());
  const auto id = [&](size_t k) { return d.song().mixer[k].id; };
  const ObjectId t1 = id(1), t2 = id(2), t5 = id(5), t6 = id(6), t7 = id(7);
  mixops::ensureStageRoom(d, {t1, t2});
  mixops::setRoomType(d, rooms::kStudio);
  const int r1 = ro::addRoom(d, 0, nullptr, "Two"), r2 = ro::addRoom(d, 0, nullptr, "Three");
  expect(r1 == 1 && r2 == 2, "three rooms");
  // What counts as a track in a room: a channel playing into it, effects, a name, a place... never the
  // space track, never an unused insert.
  const ObjectId ch = d.addChannel("Lead", "");
  d.routeChannel(ch, t6);
  ro::moveTracksToRoom(d, {t6, t5}, r1);  // t6 carries a channel, t5 is an unused insert
  d.editMixerTrack(t7, "name", [](MixerTrack& m) { m.name = "Bus"; });
  ro::moveTracksToRoom(d, {t7}, r1);
  d.endGesture();
  const Song& s = d.song();
  const std::vector<ObjectId> used = ro::roomUsedTracks(s, r1);
  const MixerTrack* sp1 = mixops::stageRoom(s, r1);
  expect(has(used, t6) && has(used, t7) && !has(used, t5) && sp1 && !has(used, sp1->id),
         "a room's tracks: a channel's track and a named track count, an unused insert and the space track do not");
  size_t n = 0;
  expect(ro::deleteBlock(s, r1, &n) == ro::DeleteBlock::HasTracks && n == 2, "a room with tracks: blocked, with how many");
  const Song before = d.song();
  const size_t at = d.history().position();
  expect(!ro::deleteRoom(d, r1) && d.song() == before && d.history().position() == at, "refused: nothing changes, no undo step");
  expect(ro::deleteBlock(s, 7) == ro::DeleteBlock::NoSuchRoom && !ro::deleteRoom(d, 7) && !ro::deleteRoom(d, -1), "no such room");
  // An empty room (only unused inserts and its space): deleted, its space with it, its inserts to the room before.
  ro::moveTracksToRoom(d, {id(8)}, r2);
  d.endGesture();
  const ObjectId sp2 = mixops::stageRoom(d.song(), r2)->id;
  expect(ro::deleteBlock(d.song(), r2) == ro::DeleteBlock::None, "an empty room can go");
  oneStep(d, "deleting an empty room: one undo step (room, space track and memberships come back)", [&] { ro::deleteRoom(d, r2); });
  expect(ro::roomCount(d.song()) == 2 && !d.song().mixerTrack(sp2) && ro::trackRoom(d.song(), id(8)) == r1, "its space goes, its unused inserts join the room before");
  d.undo();
  expect(ro::roomCount(d.song()) == 3 && d.song().mixerTrack(sp2) && ro::trackRoom(d.song(), id(8)) == r2, "undo brings the room back as it was");
  d.redo();
  // Move the tracks out, then the room goes.
  ro::moveTracksToRoom(d, {t6, t7}, 0);
  d.endGesture();
  expect(ro::deleteRoom(d, r1) && ro::roomCount(d.song()) == 1, "with its tracks moved out the room is deleted");
  expect(ro::trackRoom(d.song(), t6) == 0 && d.song().channel(ch)->mixerTrack == t6, "the moved tracks keep their channel links");
  expect(mixops::stageRoom(d.song(), 0) && mixops::stageRoom(d.song(), 0)->name == "Room", "one room left: its space is called Room again");
  expect(ro::deleteBlock(d.song(), 0) == ro::DeleteBlock::LastRoom && !ro::deleteRoom(d, 0), "the last room is never deleted");
  // Deleting the first room (empty): the next one becomes the first and takes its unused inserts.
  const int r = ro::addRoom(d, 0, nullptr, "Next");
  ro::moveTracksToRoom(d, ro::roomUsedTracks(d.song(), 0), r);
  d.endGesture();
  const ObjectId nextId = ro::roomIdAt(d.song(), r);
  expect(ro::deleteRoom(d, 0) && ro::roomCount(d.song()) == 1 && ro::roomIdAt(d.song(), 0) == nextId, "an empty first room goes, the next is first");
  bool all = true;
  for (size_t k = 1; k < d.song().mixer.size(); ++k) all = all && ro::trackRoom(d.song(), d.song().mixer[k]) == 0;
  expect(all && ro::trackRoom(d.song(), t6) == 0, "every track is in the room left");
  // Saved like a project that never had rooms (no stageRooms, version 8).
  const std::string text = jsonCompact(songToJson(d.song(), false));
  expect(text.find("stageRooms") == std::string::npos && text.find("\"version\":8") != std::string::npos, "one room again: saved as version 8 with no stageRooms");
}

void testSoloRooms() {
  // Which tracks play with some rooms soloed.
  Song s = makeEmptySong();
  StageSettings a, b;
  s.stage.id = s.allocateId();
  a.id = s.allocateId(), b.id = s.allocateId();
  s.stageRooms = {a, b};
  const ObjectId t1 = s.mixer[1].id, t2 = s.mixer[2].id, t3 = s.mixer[3].id, bus = s.mixer[4].id;
  s.mixer[1].room = s.stage.id;
  s.mixer[2].room = a.id;
  s.mixer[3].room = b.id;
  s.mixer[4].room = s.stage.id;
  for (size_t k = 5; k < s.mixer.size(); ++k) s.mixer[k].room = s.stage.id;
  s.mixer[2].sends.push_back(Send{});
  s.mixer[2].sends.back().to = bus;  // a track in room 2 feeds a bus in room 1
  const std::vector<ObjectId> keep = ro::hearOnlyKeep(s, {a.id, b.id});
  const auto kept = [&](ObjectId x) { return std::binary_search(keep.begin(), keep.end(), x); };
  expect(kept(t2) && kept(t3) && kept(bus) && kept(s.masterId()) && !kept(t1), "two rooms soloed: their tracks, the bus they feed and the master play");
  Song p = s;
  expect(ro::hearOnly(p, {a.id}) && p.mixerTrack(t1)->muted && !p.mixerTrack(t2)->muted && p.mixerTrack(t3)->muted && !p.mixerTrack(bus)->muted,
         "one room soloed: the others are muted in the engine's copy");
  expect(!s.mixerTrack(t1)->muted, "the song itself is never touched (nothing saved or shared)");
  Song q = s;
  expect(!ro::hearOnly(q, {}) && !ro::hearOnly(q, {987654}) && q == s, "no room soloed (or a room that is gone): nothing changes");
  Song one = makeEmptySong();
  Song oneCopy = one;
  expect(!ro::hearOnly(oneCopy, {one.stage.id}) && oneCopy == one, "one room: soloing it changes nothing");

  // In the sound: a sine in the first room, a second room soloed: silence; both soloed: as before.
  const int N = 4096;
  Song snd = sineSong(0.0f, 0, 0);
  StageSettings r;
  snd.stage.id = snd.allocateId();
  r.id = snd.allocateId();
  snd.stageRooms.push_back(r);
  for (size_t k = 1; k < snd.mixer.size(); ++k) snd.mixer[k].room = snd.stage.id;
  Song other = snd, both = snd;
  ro::hearOnly(other, {r.id});
  ro::hearOnly(both, {r.id, snd.stage.id});
  const Out o0 = run(snd, N), o1 = run(other, N), o2 = run(both, N);
  expect(rms(o0.L) > 1e-3 && rms(o1.L) < 1e-7 && finite(o1), "another room soloed: the first room's sine is silent");
  expect(same(o0, o2), "every room soloed: bit for bit as with none");
}

// ---------------------------------------------------------------- box select
void testBoxSelect() {
  const sel::Box b = sel::Box::of(100, 80, 20, 10);
  expect(b.x0 == 20 && b.y0 == 10 && b.x1 == 100 && b.y1 == 80, "a box from any two corners");
  expect(b.touches(105, 50, 8) && !b.touches(120, 50, 8) && b.contains(50, 50), "a token touched by the box is caught");
  const std::vector<ObjectId> before = {11, 12}, caught = {12, 13};
  expect(sel::applyBand(before, caught, sel::BandMode::Replace) == std::vector<ObjectId>({12, 13}), "a plain box replaces the choice");
  expect(sel::applyBand(before, caught, sel::BandMode::Add) == std::vector<ObjectId>({11, 12, 13}), "Shift adds");
  expect(sel::applyBand(before, caught, sel::BandMode::Toggle) == std::vector<ObjectId>({11, 13}), "Ctrl / Cmd flips each caught track");
  expect(sel::applyBand(before, {}, sel::BandMode::Add) == before && sel::applyBand(before, {}, sel::BandMode::Replace).empty(),
         "a box that catches nothing: Shift keeps, plain clears");
  expect(sel::pressToken(before, 12, sel::BandMode::Replace) == before, "pressing a chosen track keeps the whole choice (a group drag)");
  expect(sel::pressToken(before, 14, sel::BandMode::Replace) == std::vector<ObjectId>({14}), "pressing another track chooses it alone");
  expect(sel::pressToken(before, 12, sel::BandMode::Toggle) == std::vector<ObjectId>({11}), "Ctrl / Cmd press flips one");
}

// ---------------------------------------------------------------- collaborators
void testCollab() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(224);
  Peer a, b;
  a.label = "Alex", b.label = "Sam";
  a.id = crypto::Identity::generate(), b.id = crypto::Identity::generate();
  a.doc.reset(makeEmptySong());
  a.folder = tmpDir("k224_a"), b.folder = tmpDir("k224_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  const ObjectId t1 = a.doc.song().mixer[1].id, t2 = a.doc.song().mixer[2].id;
  mixops::ensureStageRoom(a.doc, {t1, t2});
  const int r = ro::addRoom(a.doc, 0, nullptr, "Encore");
  settle(net, {&a, &b}, clock);
  expect(ro::roomCount(b.doc.song()) == 2 && ro::roomName(b.doc.song(), 1) == "Encore" && songText(a) == songText(b), "collab: a new room reaches the other side");
  ro::moveTracksToRoom(b.doc, {t1, t2}, r);
  b.doc.endGesture();
  settle(net, {&a, &b}, clock);
  expect(ro::trackRoom(a.doc.song(), t1) == r && ro::trackRoom(a.doc.song(), t2) == r && songText(a) == songText(b), "collab: tracks moved between rooms reach the other side");
  b.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(ro::trackRoom(a.doc.song(), t1) == 0 && ro::roomCount(a.doc.song()) == 2, "collab: undo takes back only the move");
  // K-270: a collaborator cannot delete a room with tracks; an emptied room's delete reaches everyone.
  b.doc.setParam(mixerParam(t1, MixerParam::Pan), 0.25, false);
  ro::moveTracksToRoom(b.doc, {t1}, r);
  b.doc.endGesture();
  settle(net, {&a, &b}, clock);
  expect(!ro::deleteRoom(a.doc, r), "collab: a room with a collaborator's track in it is not deleted");
  settle(net, {&a, &b}, clock);
  expect(ro::roomCount(b.doc.song()) == 2 && ro::trackRoom(b.doc.song(), t1) == r && songText(a) == songText(b), "collab: nothing was sent, both keep the room");
  ro::moveTracksToRoom(a.doc, {t1}, 0);
  a.doc.endGesture();
  expect(ro::deleteRoom(a.doc, r), "collab: emptied, it can go");
  settle(net, {&a, &b}, clock);
  expect(ro::roomCount(b.doc.song()) == 1 && ro::trackRoom(b.doc.song(), t1) == 0 && songText(a) == songText(b), "collab: the delete reaches the other side");
}

// ---------------------------------------------------------------- FL export
void testFlpExport() {
  Song s = makeEmptySong();
  s.mixer[1].depth = 1.0f;
  s.mixer[2].depth = 1.0f;
  StageSettings r;
  r.id = s.allocateId();
  r.roomType = 0;  // a room with no space: its tracks' distance is not written
  s.stage.roomType = static_cast<uint8_t>(rooms::kStudio);
  s.stageRooms.push_back(r);
  s.mixer[2].room = r.id;
  flp::Bytes out;
  flp::ExportReport rep;
  expect(flp::exportFlp(s, out, rep, flp::ExportOptions{}) && !out.empty(), "a project with rooms exports");
  expect(rep.counts["mixer: Stage rooms written as plain tracks (room width left out)"] == 1, "the report says rooms are written as plain tracks");
  expect(rep.counts["mixer: track depth written as level, EQ and width"] == 1, "each track's distance is baked with its own room (none in a room with no space)");
}

}  // namespace

int main() {
  testRooms();
  testDeleteRules();
  testMoveTracks();
  testStatesKeepRooms();
  testPicking();
  testTemplates();
  testSaveLoad();
  setTestEffectFactory(&testFactory);
  testEngine();
  testSoloRooms();
  setTestEffectFactory(nullptr);
  testBoxSelect();
  testCollab();
  testFlpExport();
  return finish();
}

// Gridline - Stage room edits while the engine plays (K-288). A big room (150 tracks, many with
// effects and every one with a room send, and stand-in instruments that play nothing and so rest) is
// renamed, duplicated ("Duplicate settings" and the "+" menu's "Same as"), given a template, reordered,
// emptied and deleted, and its tracks moved to another room and back, one after another, while an audio
// thread with worker threads plays the project. The interface publishes a new Program after every edit,
// as the app does once per frame. Nothing the audio thread reads may be freed or changed under it.
//
// The crash: an instrument at rest is not run, so it keeps the pointers it took into the aux of the
// Program it last ran in, and the engine still asks it things every block (supportsGlide while bending
// notes). Each edit's new Program retired that one, and once it was freed the stand-in instrument read
// freed memory. Built with ASan and UBSan, this test reported that read before the fix.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/MixerOps.h"
#include "core/Song.h"
#include "core/StageRoomOps.h"
#include "core/StageRooms.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

using namespace gl;
namespace ro = gl::roomops;

namespace {

constexpr int kTracks = 150;

// 150 tracks, each fed by an instrument playing steps, a third of them with effects, all in the first
// room with a hall as its space (every track sends into it).
void bigRoom(Document& d) {
  const ObjectId pat = d.song().patterns[0].id;
  while (static_cast<int>(d.song().mixer.size()) < kTracks + 1) d.addMixerTrack();
  const char* fx[] = {"gl.eq", "gl.delay", "gl.chorus", "gl.reverb", "gl.clipper"};
  std::vector<ObjectId> tracks;
  for (int k = 0; k < kTracks; ++k) {
    const ObjectId t = d.song().mixer[static_cast<size_t>(k + 1)].id;
    tracks.push_back(t);
    const ObjectId c = d.addChannel("c" + std::to_string(k), "gl.kick");
    d.routeChannel(c, t);
    d.setStep(pat, c, k % 16, 100);
    if (k % 3 == 0) d.setSlotPlugin(t, 0, fx[k % 5]);
    if (k % 7 == 0) d.setSlotPlugin(t, 1, fx[(k + 2) % 5]);
  }
  // Stand-in instruments (a project brought in from another format) that play nothing: they rest.
  for (int k = 0; k < 24; ++k) {
    const ObjectId c = d.addChannel("standin" + std::to_string(k), "gl.flex");
    d.routeChannel(c, tracks[static_cast<size_t>(k * 5)]);
  }
  d.addClip(pat, 0, 0);
  mixops::ensureStageRoom(d, tracks);
  mixops::setRoomType(d, rooms::kHall, tracks);
  d.endGesture();
}

struct Player {
  AudioEngine engine{44100};
  ProgramBuilder builder;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> blocks{0};
  std::thread audio;
  explicit Player(const Song& s) {
    builder.setSampleRate(44100);
    engine.setThreads(4);
    engine.setProgram(builder.build(s));
    engine.setMode(PlayMode::Pattern);
    engine.setPattern(s.patterns[0].id);
    engine.play();
    audio = std::thread([this] {
      std::vector<float> L(256), R(256);
      while (!stop.load()) {
        engine.process(L.data(), R.data(), 256);
        blocks.fetch_add(1);
      }
    });
  }
  // What the app does once per frame after an edit.
  void publish(const Document& d) {
    engine.setProgram(builder.build(d.song()));
    engine.collectGarbage();
  }
  ~Player() {
    stop.store(true);
    audio.join();
  }
};

// The user's report: a room renamed, then a new room made with the big room's settings.
void duplicateBigRoom() {
  Document d(makeEmptySong());
  bigRoom(d);
  Player p(d.song());
  bool ok = true;
  for (int i = 0; i < 40; ++i) {
    ro::renameRoom(d, 0, "Album " + std::to_string(i));
    p.publish(d);
    const int n = ro::addRoom(d, 0, nullptr, ro::roomName(d.song(), 0) + " 2", 0);  // Duplicate settings
    p.publish(d);
    const int m = ro::addRoom(d, n, nullptr);  // "+": Same as the room shown
    p.publish(d);
    ok = ok && n >= 1 && m == n + 1 && ro::roomAt(d.song(), n).roomType == rooms::kHall;
    ro::renameRoom(d, n, "Copy " + std::to_string(i));
    p.publish(d);
    if (i % 2 == 0) {
      d.undo();
      p.publish(d);
      d.undo();
      p.publish(d);
      d.redo();
      p.publish(d);
    }
    while (ro::roomCount(d.song()) > 1) {
      ro::deleteRoom(d, ro::roomCount(d.song()) - 1);
      p.publish(d);
    }
  }
  expect(ok, "a big room is duplicated again and again while it plays: each copy has its space");
  expect(p.blocks.load() > 0, "the engine played all through");
}

// Every other room edit, with all the big room's tracks moving at once.
void everyRoomEdit() {
  Document d(makeEmptySong());
  bigRoom(d);
  Player p(d.song());
  const ro::RoomTemplate cathedral = ro::factoryTemplates().back();
  int moved = 0;
  for (int i = 0; i < 12; ++i) {
    const int b = ro::addRoom(d, 0, nullptr, "B");
    p.publish(d);
    const int c = ro::addRoom(d, 0, &cathedral, "C");
    p.publish(d);
    moved += ro::moveTracksToRoom(d, ro::roomTracks(d.song(), 0), b);
    p.publish(d);
    ro::applyTemplate(d, b, cathedral);
    p.publish(d);
    ro::moveRoom(d, b, 0);
    p.publish(d);
    ro::moveRoom(d, 0, 2);
    p.publish(d);
    ro::setRoomColor(d, 1, 0x336699);
    p.publish(d);
    const int at = ro::roomIndex(d.song(), ro::roomIdAt(d.song(), 2));
    const std::vector<ObjectId> all = ro::roomTracks(d.song(), at);
    if (!all.empty()) {
      ro::moveTracksToRoom(d, {all.front()}, c);  // one track
      p.publish(d);
    }
    moved += ro::moveTracksToRoom(d, ro::roomTracks(d.song(), at), c);
    p.publish(d);
    mixops::setRoomType(d, rooms::kStudio, ro::roomTracks(d.song(), ro::roomIndex(d.song(), ro::roomIdAt(d.song(), c))), c);
    p.publish(d);
    d.undo();
    p.publish(d);
    d.redo();
    p.publish(d);
    // Back to one room: everything into the first, then the empty rooms go.
    for (int r = 1; r < ro::roomCount(d.song()); ++r) {
      ro::moveTracksToRoom(d, ro::roomTracks(d.song(), r), 0);
      p.publish(d);
    }
    while (ro::roomCount(d.song()) > 1) {
      if (!ro::deleteRoom(d, ro::roomCount(d.song()) - 1)) break;
      p.publish(d);
    }
  }
  expect(moved >= 12 * kTracks, "every move took the whole room");
  expect(ro::roomCount(d.song()) == 1, "back to one room each time");
}

// The Program an instrument at rest last ran in stays alive while it rests, however many edits arrive,
// and goes once the instrument has run in a newer one.
void restingKeepsItsProgram() {
  Document d(makeEmptySong());
  bigRoom(d);
  std::vector<ObjectId> standins;
  for (const auto& c : d.song().channels)
    if (c.plugin.type == "gl.flex") standins.push_back(c.id);
  const ObjectId flex = standins.back();
  Player p(d.song());
  const std::shared_ptr<ChannelRuntime> rt = p.builder.channelRuntime(flex);
  expect(rt != nullptr, "the stand-in has a runtime");
  if (!rt) return;
  std::weak_ptr<const Program> first = p.engine.program();
  for (int i = 0; i < 20000 && !rt->restingUi.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  expect(rt->restingUi.load(), "the silent stand-in comes to rest");
  for (int i = 0; i < 300; ++i) {
    ro::renameRoom(d, 0, "Rest " + std::to_string(i));
    p.publish(d);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  expect(rt->restingUi.load() && !first.expired(), "the Program the resting stand-in last ran in outlives 300 newer ones");
  // Played again: each stand-in runs in the newest Program, and the old one can go (while any of them
  // still rests in it, it stays).
  for (ObjectId c : standins) p.engine.noteOn(c, 60, 0.8f);
  for (int i = 0; i < 20000 && rt->restingUi.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  for (ObjectId c : standins) p.engine.noteOff(c, 60);
  for (int i = 0; i < 2000 && !first.expired(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    p.engine.collectGarbage();
  }
  expect(first.expired(), "and goes once the stand-ins have run in a newer one");
}

}  // namespace

int main() {
  restingKeepsItsProgram();
  duplicateBigRoom();
  everyRoomEdit();
  return finish();
}

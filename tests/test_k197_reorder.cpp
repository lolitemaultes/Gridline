// Gridline - mixer tracks and playlist tracks in a new order (GitHub issue #46): every place that
// stores a mixer index follows its track (send level automation and links, plugin targets as mixer
// indexes and as route numbers, side inputs), a reordered project renders bit for bit the same,
// playlist clips and track settings travel with their tracks, groups keep their members, undo /
// redo, save / load, collaboration and the FL export.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/MenuOps.h"
#include "core/MixerOps.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/StageRooms.h"
#include "core/TrackOrder.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/Effects.h"
#include "engine/Program.h"

using namespace gl;
namespace to = gl::trackorder;

namespace {

// ---------------------------------------------------------------- a routed project
// A steady sine (param 0 Hz; param 1 = 1: the right channel inverted).
class TestSine final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double f = s.params.empty() ? 100.0 : s.params[0];
    const float mode = s.params.size() > 1 ? s.params[1] : 0.0f;
    for (int i = 0; i < n; ++i, ++t_) {
      const float v = static_cast<float>(0.25 * std::sin(2.0 * M_PI * f * static_cast<double>(t_) / 44100.0));
      L[i] += v;
      R[i] += mode > 0.5f ? -v : v;
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

struct Ids {
  ObjectId a, b, c, d, e, f;  // inserts 1..6 ("A".."F")
  ObjectId autoClip, channel;
};

int idx(const Song& s, ObjectId id) { return s.mixerIndex(id); }

// A: two sines' source, sends to C and to the room F; a Mixer Send to D (a mixer index).
// B: a sine, sidechain into C; a Mixer Send to C by route number.
// C: Transient Split (route numbers): transients to E, detection from B; sends to E.
// E: sends to D. F: the Stage's room. A channel on D with an extra output to E.
// Automation of A's send to C (playlist) and C's send to E (pattern); MIDI / OSC and internal links.
Song routedSong(Ids& ids) {
  Song s = makeEmptySong();
  const char* names[] = {"A", "B", "C", "D", "E", "F"};
  for (int k = 0; k < 6; ++k) s.mixer[static_cast<size_t>(k + 1)].name = names[k];
  ids.a = s.mixer[1].id, ids.b = s.mixer[2].id, ids.c = s.mixer[3].id, ids.d = s.mixer[4].id, ids.e = s.mixer[5].id, ids.f = s.mixer[6].id;
  MixerTrack& A = *s.mixerTrack(ids.a);
  A.slots[0].plugin.type = "test.sine";
  A.slots[0].plugin.params = {440.0f, 0.0f};
  A.sends.push_back({ids.c, 0.7f, false, false});
  A.sends.push_back({ids.f, 0.25f, false, false, true});
  A.slots[1].plugin = defaultPluginState("gl.send");
  A.slots[1].plugin.params[0] = static_cast<float>(idx(s, ids.d));  // target: D as a mixer index
  A.slots[1].plugin.params[1] = 0.5f;
  MixerTrack& B = *s.mixerTrack(ids.b);
  B.slots[0].plugin.type = "test.sine";
  B.slots[0].plugin.params = {660.0f, 1.0f};
  B.sends.push_back({ids.c, 0.5f, true, false});
  B.slots[1].plugin = defaultPluginState("gl.send");
  B.slots[1].plugin.params[4] = 1.0f;  // route numbering
  {
    const std::vector<int> outs = flRouteOutputs(s, idx(s, ids.b));
    const int n = static_cast<int>(std::find(outs.begin() + 1, outs.end(), idx(s, ids.c)) - outs.begin());
    B.slots[1].plugin.params[0] = static_cast<float>(n);
  }
  MixerTrack& C = *s.mixerTrack(ids.c);
  C.sends.push_back({ids.e, 0.6f, false, false});
  C.slots[0].plugin = defaultPluginState("gl.transplit");
  if (const PluginInfo* pi = findPlugin("gl.transplit")) {
    const std::vector<int> outs = flRouteOutputs(s, idx(s, ids.c));
    const int n = static_cast<int>(std::find(outs.begin() + 1, outs.end(), idx(s, ids.e)) - outs.begin());
    C.slots[0].plugin.params[static_cast<size_t>(pi->sendParams[0])] = static_cast<float>(n);
    const std::vector<int> ins = flRouteInputs(s, idx(s, ids.c));
    const int m = static_cast<int>(std::find(ins.begin() + 1, ins.end(), idx(s, ids.b)) - ins.begin());
    C.slots[0].plugin.params[static_cast<size_t>(pi->sideInParam)] = static_cast<float>(m);
  }
  s.mixerTrack(ids.e)->sends.push_back({ids.d, 0.4f, false, false});
  s.stage.room = ids.f;
  s.stage.roomType = static_cast<uint8_t>(rooms::kStudio);
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Keys";
  ch.mixerTrack = ids.d;
  ch.auxOuts = {ids.e};
  s.channels.push_back(ch);
  ids.channel = ch.id;
  s.tracks[0].mode = TrackMode::Audio;
  s.tracks[0].mixer = ids.d;
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.owner = ids.a;
  ac.index = sendParam(ids.a, idx(s, ids.c)).index;
  ac.points = {{0, 0.2f}, {static_cast<Tick>(s.ppq) * 8, 0.9f}};
  s.automations.push_back(ac);
  ids.autoClip = ac.id;
  Clip cl;
  cl.id = s.allocateId();
  cl.kind = ClipKind::Automation;
  cl.source = ac.id;
  cl.track = 1;
  cl.start = 0;
  cl.length = static_cast<Tick>(s.ppq) * 8;
  s.playlist.push_back(cl);
  AutomationClip lane;
  lane.owner = ids.c;
  lane.index = sendParam(ids.c, idx(s, ids.e)).index;
  lane.points = {{0, 0.5f}};
  s.patterns[0].automation.push_back(lane);
  ControllerLink osc;
  osc.source = RemoteSource::Osc;
  osc.oscAddress = "/fader/1";
  osc.owner = ids.e;
  osc.index = sendParam(ids.e, idx(s, ids.d)).index;
  s.links.push_back(osc);
  ControllerLink cc;
  cc.cc = 7;
  cc.owner = ids.a;
  cc.index = sendParam(ids.a, idx(s, ids.f)).index;
  s.links.push_back(cc);
  ModLink ml;
  s.mixerTrack(ids.e)->slots[2].plugin = defaultPluginState("gl.xyctl");  // a controller source on E
  ml.sourceOwner = ids.e;
  ml.sourceSlot = 2;
  ml.owner = ids.c;  // C's send level to E, driven by the X-Y pad on E
  ml.index = sendParam(ids.c, idx(s, ids.e)).index;
  s.modLinks.push_back(ml);
  return s;
}

// Every mixer reference of the project as track ids: what a reorder must keep.
struct Facts {
  std::vector<ObjectId> sendParamDest;  // automation, pattern lane, links, mod link
  ObjectId aSendTo = kNoId, bSendTo = kNoId, cTransients = kNoId, cDetect = kNoId;
  ObjectId room = kNoId, channelTrack = kNoId, channelAux = kNoId, audioTrack = kNoId;
  std::vector<std::pair<ObjectId, ObjectId>> sends;
  bool operator==(const Facts& o) const {
    return sendParamDest == o.sendParamDest && aSendTo == o.aSendTo && bSendTo == o.bSendTo && cTransients == o.cTransients &&
           cDetect == o.cDetect && room == o.room && channelTrack == o.channelTrack && channelAux == o.channelAux && audioTrack == o.audioTrack &&
           sends == o.sends;
  }
};

ObjectId at(const Song& s, int i) { return i >= 0 && static_cast<size_t>(i) < s.mixer.size() ? s.mixer[static_cast<size_t>(i)].id : kNoId; }
ObjectId sendDest(const Song& s, uint32_t index) { return index >= kSendBase && index < kSlotBase ? at(s, static_cast<int>(index - kSendBase)) : kNoId; }

// A plugin's destination as the engine reads it (engine/Program.cpp sendDestination).
ObjectId pluginDest(const Song& s, ObjectId host, int slot, int param) {
  const PluginState& p = s.mixerTrack(host)->slots[static_cast<size_t>(slot)].plugin;
  const PluginInfo* pi = findPlugin(p.type);
  const int v = static_cast<int>(std::lround(p.params[static_cast<size_t>(param)]));
  bool routed = pi && pi->routeModeParam == kRouteIndexAlways;
  if (pi && pi->routeModeParam >= 0) routed = p.params[static_cast<size_t>(pi->routeModeParam)] >= 0.5f;
  return at(s, routed ? flRouteOutput(s, idx(s, host), v) : v);
}

Facts factsOf(const Song& s, const Ids& ids) {
  Facts f;
  f.sendParamDest.push_back(sendDest(s, s.automation(ids.autoClip)->index));
  f.sendParamDest.push_back(sendDest(s, s.patterns[0].automation[0].index));
  for (const auto& l : s.links) f.sendParamDest.push_back(sendDest(s, l.index));
  for (const auto& l : s.modLinks) f.sendParamDest.push_back(sendDest(s, l.index));
  f.aSendTo = pluginDest(s, ids.a, 1, 0);
  f.bSendTo = pluginDest(s, ids.b, 1, 0);
  const PluginInfo* tp = findPlugin("gl.transplit");
  f.cTransients = pluginDest(s, ids.c, 0, tp->sendParams[0]);
  const PluginState& t = s.mixerTrack(ids.c)->slots[0].plugin;
  f.cDetect = at(s, flRouteInput(s, idx(s, ids.c), static_cast<int>(std::lround(t.params[static_cast<size_t>(tp->sideInParam)]))));
  f.room = mixops::stageRoom(s) ? mixops::stageRoom(s)->id : kNoId;
  f.channelTrack = s.channel(ids.channel)->mixerTrack;
  f.channelAux = s.channel(ids.channel)->auxOuts[0];
  f.audioTrack = s.tracks[0].mixer;
  for (const auto& m : s.mixer)
    for (const auto& sd : m.sends) f.sends.push_back({m.id, sd.to});
  std::sort(f.sends.begin(), f.sends.end());
  return f;
}

std::string text(const Song& s) { return jsonStringify(songToJson(s), 0); }

// On a failed check: where two texts first differ.
void showDiff(const std::string& a, const std::string& b) {
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  if (i == a.size() && i == b.size()) return;
  const size_t from = i > 160 ? i - 160 : 0;
  std::printf("# differ at %zu:\n#  %s\n#  %s\n", i, a.substr(from, 260).c_str(), b.substr(from, 260).c_str());
}

std::vector<std::string> names(const Song& s) {
  std::vector<std::string> v;
  for (size_t i = 0; i < s.mixer.size(); ++i) v.push_back(mixerTrackName(s, i));
  return v;
}

// ---------------------------------------------------------------- the remap
void testRemap() {
  Ids ids{};
  const Song s0 = routedSong(ids);
  const Facts f0 = factsOf(s0, ids);
  expect(f0.aSendTo == ids.d && f0.bSendTo == ids.c && f0.cTransients == ids.e && f0.cDetect == ids.b, "fixture: the plugin targets name D, C, E and B");
  expect(f0.sendParamDest[0] == ids.c && f0.sendParamDest[1] == ids.e && f0.sendParamDest[2] == ids.d && f0.sendParamDest[3] == ids.f &&
             f0.sendParamDest[4] == ids.e,
         "fixture: the send level targets name C, E, D, F and E");
  // A few moves by hand.
  Song s = s0;
  expect(to::reorderMixer(s, to::mixerOrderAfterMove(s, {ids.c}, 1)), "move C to the front");
  expect(s.mixer[1].id == ids.c && s.mixer[2].id == ids.a && s.mixer[0].id == s0.masterId(), "C first, the master stays first");
  expect(factsOf(s, ids) == f0, "C first: every reference still names the same tracks");
  s = s0;
  expect(to::reorderMixer(s, to::mixerOrderAfterMove(s, {ids.a, ids.e}, static_cast<int>(s.mixer.size()))), "move A and E to the end");
  expect(s.mixer[s.mixer.size() - 2].id == ids.a && s.mixer.back().id == ids.e, "A and E at the end, in their order");
  expect(factsOf(s, ids) == f0, "A and E at the end: every reference still names the same tracks");
  s = s0;
  expect(to::reorderMixer(s, to::mixerOrderAfterMove(s, {ids.f}, 2)) && s.mixer[2].id == ids.f && factsOf(s, ids) == f0,
         "the room track moves like any other, its sends follow");
  // The master never moves; bad orders are refused.
  s = s0;
  std::vector<ObjectId> bad;
  for (const auto& m : s.mixer) bad.push_back(m.id);
  std::swap(bad[0], bad[1]);
  expect(!to::reorderMixer(s, bad) && text(s) == text(s0), "an order without the master first is refused");
  bad = {s.masterId()};
  expect(!to::reorderMixer(s, bad) && text(s) == text(s0), "an order missing tracks is refused");
  expect(to::mixerOrderAfterMove(s, {s.masterId()}, 3).empty(), "the master alone: nothing moves");
  // Random orders, then back: every reference follows, and the way back is the project itself.
  std::mt19937 rng(197);
  bool all = true, back = true;
  for (int round = 0; round < 40; ++round) {
    Song r = s0;
    std::vector<ObjectId> order;
    for (const auto& m : r.mixer) order.push_back(m.id);
    std::shuffle(order.begin() + 1, order.end(), rng);
    std::vector<ObjectId> orig;
    for (const auto& m : r.mixer) orig.push_back(m.id);
    all = all && to::reorderMixer(r, order) && factsOf(r, ids) == f0;
    back = back && to::reorderMixer(r, orig) && text(r) == text(s0);
  }
  expect(all, "40 random orders: every reference names the same tracks");
  expect(back, "... and putting them back gives the project exactly");
}

// ---------------------------------------------------------------- the sound
struct Out {
  std::vector<float> L, R;
};
Out render(const Song& s, int frames) {
  ProgramBuilder b;
  AudioEngine e(44100);
  b.setSampleRate(44100);
  e.setThreads(1);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Song);
  e.play();
  Out o;
  const int block = 256;
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
double peak(const Out& o) {
  double p = 0;
  for (float v : o.L) p = std::max(p, static_cast<double>(std::fabs(v)));
  return p;
}

void testRender() {
  setTestEffectFactory(&testFactory);
  Ids ids{};
  Song s0 = routedSong(ids);
  // Three sources summed on one bus (A, B by sidechain, the D / E chain): the summing order matters.
  s0.mixerTrack(ids.d)->slots[0].plugin.type = "test.sine";
  s0.mixerTrack(ids.d)->slots[0].plugin.params = {220.0f, 0.0f};
  const int N = 44100;
  const Out x = render(s0, N);
  expect(peak(x) > 0.05, "the routed project makes sound");
  std::mt19937 rng(46);
  bool ok = true;
  for (int round = 0; round < 6; ++round) {
    Song r = s0;
    std::vector<ObjectId> order;
    for (const auto& m : r.mixer) order.push_back(m.id);
    std::shuffle(order.begin() + 1, order.end(), rng);
    to::reorderMixer(r, order);
    ok = ok && same(render(r, N), x);
  }
  expect(ok, "a reordered project renders bit for bit the same (6 random orders, automation of a send, sidechain, room, route numbers)");
  {  // a track inserted in the middle and one deleted: the rest still sounds the same
    Document d(s0);
    mixops::insertTracks(d, 2, 1);
    const ObjectId added = d.song().mixer[2].id;
    Song kept = d.song();  // the new track's own send to the master is not a reference
    kept.mixerTrack(added)->sends.clear();
    expect(factsOf(kept, ids) == factsOf(s0, ids), "a track inserted in the middle: every reference still names the same tracks");
    expect(same(render(d.song(), N), x), "... and the project renders the same");
    d.removeMixerTrack(added);
    expect(factsOf(d.song(), ids) == factsOf(s0, ids), "deleting it again: every reference still names the same tracks");
  }
  setTestEffectFactory(nullptr);
}

// ---------------------------------------------------------------- undo, save, menu, keys
void testDocument() {
  Ids ids{};
  Song s0 = routedSong(ids);
  for (auto& m : s0.mixer)  // the test effect is not saved; built-in plugins only
    for (auto& sl : m.slots)
      if (sl.plugin.type == "test.sine") sl.plugin = PluginState{};
  Document d(s0);
  const size_t h0 = d.history().position();
  expect(to::moveMixerTracks(d, {ids.c, ids.e}, 1), "move C and E to the front");
  expect(d.history().position() == h0 + 1 && d.history().undoLabel() == "move mixer tracks", "one undo step, \"move mixer tracks\"");
  expect(d.song().mixer[1].id == ids.c && d.song().mixer[2].id == ids.e && factsOf(d.song(), ids) == factsOf(s0, ids), "moved, references kept");
  const std::string moved = text(d.song());
  expect(!to::moveMixerTracks(d, {ids.c, ids.e}, 1) && d.history().position() == h0 + 1, "dropping them where they are records nothing");
  d.undo();
  expect(text(d.song()) == text(s0), "undo: the original order and references");
  d.redo();
  expect(text(d.song()) == moved, "redo: moved again");
  // Docking to the neighbour's dock with the drop.
  expect(to::moveMixerTracks(d, {ids.b}, 1, static_cast<int>(MixerDock::Left)) && d.song().mixerTrack(ids.b)->dock == -1 &&
             d.song().mixer[1].id == ids.b,
         "a drop among left-docked strips docks the track left, in the same step");
  d.undo();
  // Move left / right (menu and Alt+arrows): the selection moves together, never past the master.
  expect(to::stepMixerTracks(d, {ids.a, ids.b}, 1), "A and B one place right");
  {
    const Song& s = d.song();
    expect(idx(s, ids.a) + 1 == idx(s, ids.b), "... they stay together, in order");
  }
  expect(!to::stepMixerTracks(d, {d.song().mixer[1].id}, -1), "the first insert does not move left of the master");
  expect(ops::moveMixerTrack(d, ids.d, -1) && factsOf(d.song(), ids) == factsOf(s0, ids), "the menu's Move left keeps the routing too");
  // Save and load.
  Song back;
  std::string err;
  const bool loaded = loadProjectText(projectToText(d.song()), "", back, &err);
  if (!loaded) std::printf("# load: %s\n", err.c_str());
  showDiff(text(back), text(d.song()));
  expect(loaded && text(back) == text(d.song()), "save and load keep the new order and references");
  // Projects saved before open as they were (no new fields, nothing moved on load).
  {
    // From the source folder (CMake passes it), so it is found wherever the test runs.
    std::ifstream f(std::string(GRIDLINE_SOURCE_DIR) + "/tests/fixtures/test_song.gridline.json");
    expect(static_cast<bool>(f), "the older project's fixture opens");
    std::stringstream ss;
    ss << f.rdbuf();
    Song a, b;
    const bool ok = loadProjectText(ss.str(), "", a) && loadProjectText(projectToText(a), "", b);
    expect(ok && text(a) == text(b), "an older project loads, saves and loads the same");
  }
}

// ---------------------------------------------------------------- playlist
Song playlistSong(std::vector<ObjectId>& clips) {
  Song s = makeEmptySong();
  s.tracks.resize(8);
  for (int k = 0; k < 8; ++k) {
    PlaylistTrack& t = s.tracks[static_cast<size_t>(k)];
    t.name = "T" + std::to_string(k);
    t.color = static_cast<Rgb>(0x101010 * (k + 1));
    t.height = 1.0f + 0.25f * static_cast<float>(k % 3);
    t.locked = k == 2;
    t.icon = k + 1;
    Clip c;
    c.id = s.allocateId();
    c.pattern = s.patterns[0].id;
    c.track = k;
    c.start = static_cast<Tick>(k) * s.ppq;
    s.playlist.push_back(c);
    clips.push_back(c.id);
  }
  s.tracks[2].mode = TrackMode::Audio;
  s.tracks[2].mixer = s.mixer[3].id;
  return s;
}

std::string order(const Song& s) {
  std::string o;
  for (const auto& t : s.tracks) o += t.name + (t.groupSize ? "(" + std::to_string(t.groupSize) + ")" : "") + " ";
  return o;
}

// Every clip still on the track that has its name.
bool clipsFollow(const Song& s, const std::vector<ObjectId>& clips) {
  for (size_t k = 0; k < clips.size(); ++k) {
    const Clip* c = s.clip(clips[k]);
    if (!c || s.tracks[static_cast<size_t>(c->track)].name != "T" + std::to_string(k)) return false;
  }
  return true;
}

void testPlaylist() {
  std::vector<ObjectId> clips;
  const Song s0 = playlistSong(clips);
  Document d(s0);
  std::vector<int> now;
  expect(to::movePlaylistTracks(d, {2}, 6, &now) && order(d.song()) == "T0 T1 T3 T4 T5 T2 T6 T7 " && now == std::vector<int>{5},
         "drag T2 down between T5 and T6");
  {
    const PlaylistTrack& t = d.song().tracks[5];
    expect(t.color == s0.tracks[2].color && t.height == s0.tracks[2].height && t.locked && t.icon == 3 && t.mode == TrackMode::Audio &&
               t.mixer == s0.mixer[3].id,
           "its colour, height, lock, icon and audio link travel with it");
  }
  expect(clipsFollow(d.song(), clips), "every clip stays on its track");
  expect(d.history().undoLabel() == "move playlist track", "one undo step");
  d.undo();
  expect(text(d.song()) == text(s0), "undo: back as it was");
  expect(to::movePlaylistTracks(d, {6, 1}, 0, &now) && order(d.song()) == "T1 T6 T0 T2 T3 T4 T5 T7 " && clipsFollow(d.song(), clips) && now == std::vector<int>{0, 1},
         "two selected tracks to the top, in their order, with their clips");
  d.undo();
  expect(!to::movePlaylistTracks(d, {3}, 3) && !to::movePlaylistTracks(d, {3}, 4), "a drop where the track already is records nothing");
  // Groups: a header brings its members; tracks dropped inside a group join it; members moved out leave.
  d.editPlaylistTrack(3, "group", [](PlaylistTrack& t) { t.groupSize = 2; });
  const Song grouped = d.song();
  expect(to::movePlaylistTracks(d, {3}, 0) && order(d.song()) == "T3(2) T4 T5 T0 T1 T2 T6 T7 " && clipsFollow(d.song(), clips),
         "a group header moves with its group");
  d.undo();
  expect(to::movePlaylistTracks(d, {0}, 5) && order(d.song()) == "T1 T2 T3(3) T4 T0 T5 T6 T7 ", "a track dropped inside a group joins it");
  d.undo();
  expect(to::movePlaylistTracks(d, {4}, 8) && order(d.song()) == "T0 T1 T2 T3(1) T5 T6 T7 T4 ", "a member moved out leaves its group");
  d.undo();
  expect(to::movePlaylistTracks(d, {4, 5}, 8) && order(d.song()) == "T0 T1 T2 T3 T6 T7 T4 T5 ", "all members moved out: no group left");
  d.undo();
  // Up / down by one (menu and Alt+Up / Down): past a folded group as a whole.
  d.editPlaylistTrack(3, "fold", [](PlaylistTrack& t) { t.folded = true; });
  expect(to::stepPlaylistTracks(d, {2}, 1, &now) && order(d.song()) == "T0 T1 T3(2) T4 T5 T2 T6 T7 " && now == std::vector<int>{5},
         "down past a folded group: below the whole group");
  expect(to::stepPlaylistTracks(d, {5}, -1, &now) && order(d.song()) == "T0 T1 T2 T3(2) T4 T5 T6 T7 " && now == std::vector<int>{2},
         "up past a folded group: above its header");
  expect(!to::stepPlaylistTracks(d, {0}, -1) && !to::stepPlaylistTracks(d, {7}, 1), "nothing moves past either end");
  expect(ops::movePlaylistTrack(d, 1, 1) && order(d.song()) == "T0 T2 T1 T3(2) T4 T5 T6 T7 " && clipsFollow(d.song(), clips),
         "the track menu's Move down");
  (void)grouped;
  // Other arrangements keep theirs; save and load keep the new order.
  Song back;
  expect(loadProjectText(projectToText(d.song()), "", back) && text(back) == text(d.song()), "save and load keep the playlist order");
}

// ---------------------------------------------------------------- collaboration
void testCollab() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(197);
  Peer a, b;
  a.label = "Alex", b.label = "Sam";
  a.id = crypto::Identity::generate(), b.id = crypto::Identity::generate();
  Ids ids{};
  Song start = routedSong(ids);
  for (auto& m : start.mixer)  // built-in plugins only (a collaborative project refuses unknown ones)
    for (auto& sl : m.slots)
      if (sl.plugin.type == "test.sine") sl.plugin = PluginState{};
  a.doc.reset(start);
  a.folder = tmpDir("k197_a"), b.folder = tmpDir("k197_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  if (!a.s || !b.s) std::printf("# collab session: %s\n", err.c_str());
  showDiff(songText(a), songText(b));
  expect(songText(a) == songText(b), "collab: both have the project");
  const Facts f0 = factsOf(a.doc.song(), ids);
  to::moveMixerTracks(a.doc, {ids.e, ids.b}, 1);
  settle(net, {&a, &b}, clock);
  expect(songText(a) == songText(b) && b.doc.song().mixer[1].id == ids.b && b.doc.song().mixer[2].id == ids.e, "collab: a mixer move reaches the other side");
  expect(factsOf(b.doc.song(), ids) == f0, "collab: ... with every reference following");
  to::movePlaylistTracks(b.doc, {1}, 4);
  settle(net, {&a, &b}, clock);
  const ObjectId clip = a.doc.song().playlist[0].id;
  expect(songText(a) == songText(b) && a.doc.song().clip(clip)->track == 3, "collab: a playlist move reaches the other side with its clips");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(songText(a) == songText(b) && b.doc.song().mixer[1].id == ids.a && b.doc.song().clip(clip)->track == 3,
         "collab: undo takes back only my own move");
}

// ---------------------------------------------------------------- FL export
void testFlpExport() {
  Ids ids{};
  Song s = routedSong(ids);
  for (auto& m : s.mixer)
    for (auto& sl : m.slots)
      if (sl.plugin.type == "test.sine") sl.plugin = PluginState{};
  to::reorderMixer(s, to::mixerOrderAfterMove(s, {ids.e, ids.c}, 1));  // E, C, A, B, D, F
  flp::Bytes out;
  flp::ExportReport rep;
  expect(flp::exportFlp(s, out, rep, flp::ExportOptions{}) && !out.empty(), "FL export of a reordered mixer: written");
  Song back;
  flp::Report r;
  std::string err;
  expect(importFlpBytes(out, back, r, &err), "FL export: reads back");
  std::vector<std::string> want = names(s), got = names(back);
  bool order = got.size() >= 7;
  for (size_t i = 0; i < 7 && order; ++i) order = got[i] == want[i];
  expect(order, "FL export: the inserts in the new order");
  // Sends by name, as the reordered project has them.
  auto sendsByName = [](const Song& x) {
    std::vector<std::string> v;
    for (size_t i = 1; i < x.mixer.size() && i < 7; ++i)
      for (const auto& sd : x.mixer[i].sends)
        if (const int t = x.mixerIndex(sd.to); t >= 0) v.push_back(mixerTrackName(x, i) + ">" + mixerTrackName(x, static_cast<size_t>(t)) + (sd.sidechain ? "sc" : ""));
    std::sort(v.begin(), v.end());
    return v;
  };
  expect(sendsByName(back) == sendsByName(s), "FL export: every send and sidechain to the same track by name");
  // B's Mixer Send (a route number: FL numbers send targets by the routes of the track) still names C.
  const MixerTrack* B = nullptr;
  for (const auto& m : back.mixer)
    if (m.name == "B") B = &m;
  bool target = false;
  if (B)
    for (int k = 0; k < kEffectSlots && !target; ++k) {
      if (B->slots[static_cast<size_t>(k)].plugin.type != "gl.send") continue;
      const MixerTrack* dt = back.mixerTrack(pluginDest(back, B->id, k, 0));
      target = dt && dt->name == "C";
    }
  expect(target, "FL export: the Mixer Send on B still targets C");
}

}  // namespace

int main() {
  testRemap();
  testRender();
  testDocument();
  testPlaylist();
  testCollab();
  testFlpExport();
  return finish();
}

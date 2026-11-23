// Gridline: 0.7.8 picks, round 2 (render tag, Loupe's warp markers in the project).
//  * a render to the library is tagged a loop when its audio lasts a whole number of bars (within
//    1 ms) and ends on a bar; anything else (a ringing tail, a trimmed one-shot, a clip off the bar
//    line, a bar and a half) is a one-shot; the sheet's guess before the preview agrees;
//  * a render written again with the other tag (the sheet's chip flipped) keeps its file and audio
//    and carries the new tag;
//  * Loupe's warp markers (PluginState::loupeWarp) are saved with the project and load back; a
//    project without them saves byte for byte as before; setting them is one undo step;
//  * in a collaboration session they reach the other person, and undo takes them back there too.
#include <cmath>
#include <filesystem>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/SampleWarp.h"
#include "core/Song.h"
#include "engine/LoopFile.h"
#include "engine/SourceRender.h"

using namespace gl;
namespace sr = gl::srcrender;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;

struct Fixture {
  Song s;
  ObjectId kick = kNoId, pattern = kNoId, clip = kNoId;
};

Fixture fixture() {
  Fixture f;
  f.s = makeEmptySong();
  f.s.tempo = 120;
  Channel k;
  k.id = f.s.allocateId();
  k.name = "Kick";
  k.plugin = defaultPluginState("gl.synth");
  f.s.channels.push_back(k);
  f.kick = k.id;
  Pattern p;
  p.id = f.s.allocateId();
  p.name = "Break";
  p.bars = 2;
  std::vector<uint8_t> steps(32, 0);
  for (int i : {0, 4, 8, 12, 16, 20, 24, 28}) steps[static_cast<size_t>(i)] = 110;
  p.steps[k.id] = steps;
  f.s.patterns.push_back(p);
  f.pattern = p.id;
  Clip c;
  c.id = f.s.allocateId();
  c.pattern = p.id;
  c.start = 0;
  f.s.playlist.push_back(c);
  f.clip = c.id;
  return f;
}

// An output of `seconds` of audio starting at song tick `start`.
sr::Output fake(const Song& s, double seconds, Tick start = 0) {
  sr::Output o;
  o.sampleRate = kRate;
  o.tempo = s.tempo;
  o.ppq = s.ppq;
  o.start = start;
  o.audio.resize(2, static_cast<size_t>(std::llround(seconds * kRate)));
  return o;
}

void loopTag() {
  Fixture f = fixture();
  const Song& s = f.s;
  const double bar = 60.0 / s.tempo * 4;  // 4/4 at 120: 2 s
  expect(sr::tagsAsLoop(s, fake(s, 2 * bar)), "two whole bars from the start: a loop");
  expect(sr::tagsAsLoop(s, fake(s, 2 * bar + 0.0009)), "within 1 ms of two bars: a loop");
  expect(!sr::tagsAsLoop(s, fake(s, 2 * bar + 0.003)), "3 ms over: a one-shot");
  expect(!sr::tagsAsLoop(s, fake(s, 1.5 * bar)), "a bar and a half: a one-shot");
  expect(!sr::tagsAsLoop(s, fake(s, 0.3)), "a short hit: a one-shot");
  expect(!sr::tagsAsLoop(s, fake(s, 1 * bar, s.barTicks() / 2)), "a whole bar that ends mid-bar: a one-shot");
  expect(sr::tagsAsLoop(s, fake(s, 1 * bar, s.barTicks() * 3)), "a whole bar from bar 4: a loop");
  {
    sr::Output o = fake(s, 0);
    expect(!sr::tagsAsLoop(s, o), "nothing rendered: a one-shot");
  }
  // The engine's own renders.
  const sr::Source pat = sr::channelSource(f.pattern, f.kick);
  sr::Options o = sr::defaultOptions(pat);
  o.sampleRate = kRate;
  o.through = sr::Through::Dry;
  o.tail = TailMode::Cut;
  o.oneShotTrim = false;
  sr::Output r = sr::render(s, pat, o);
  expect(r.error.empty() && r.audio.frames > 0, "the row renders");
  expect(sr::tagsAsLoop(s, r), "a two bar row, its tail cut: a loop");
  expect(sr::expectsLoop(s, pat, o), "and the sheet guesses so before the preview");
  o.loops = 3;
  expect(sr::tagsAsLoop(s, sr::render(s, pat, o)) && sr::expectsLoop(s, pat, o), "three passes: six bars, a loop");
  o.loops = 1;
  o.tail = TailMode::Leave;
  o.maxTailSeconds = 1.0;
  sr::Output ring = sr::render(s, pat, o);
  if (ring.audio.frames > r.audio.frames) expect(!sr::tagsAsLoop(s, ring), "the tail left ringing: a one-shot");
  expect(!sr::expectsLoop(s, pat, o), "the sheet guesses a one-shot with the tail left ringing");
  o.tail = TailMode::Cut;
  o.oneShotTrim = true;
  expect(!sr::expectsLoop(s, pat, o), "one-shot trim: the sheet guesses a one-shot");
  const sr::Source notes = sr::notesSource(f.pattern, f.kick, {0});
  expect(!sr::expectsLoop(s, notes, sr::defaultOptions(notes)), "notes render as a one-shot");
  // A clip that starts half a bar in: its two bars end mid-bar.
  Song off = s;
  off.playlist[0].start = off.barTicks() / 2;
  sr::Options co = sr::defaultOptions(sr::clipSource(f.clip));
  co.sampleRate = kRate;
  co.tail = TailMode::Cut;
  co.oneShotTrim = false;
  expect(!sr::expectsLoop(off, sr::clipSource(f.clip), co), "a clip off the bar line: a one-shot");
  sr::Output oc = sr::render(off, sr::clipSource(f.clip), co);
  expect(oc.error.empty() && !sr::tagsAsLoop(off, oc), "and its render too");
  expect(sr::expectsLoop(s, sr::clipSource(f.clip), co) && sr::tagsAsLoop(s, sr::render(s, sr::clipSource(f.clip), co)), "on the bar line: a loop");

  // Flipping the tag writes the same file again with the other tag.
  const std::string dir = sim::tmpDir("k078p2_render");
  r.oneShot = false;
  const std::string p1 = sr::writeRender(r, dir);
  loopfile::Info li;
  expect(!p1.empty() && loopfile::read(p1, li) && li.loop, "written as a loop");
  const auto size1 = fs::file_size(p1);
  r.oneShot = true;
  const std::string p2 = sr::writeRender(r, dir);
  expect(p2 == p1, "the same render is the same file");
  expect(loopfile::read(p2, li) && !li.loop, "now tagged a one-shot");
  expect(fs::file_size(p2) == size1, "with the same audio");
  r.oneShot = false;
  expect(sr::writeRender(r, dir) == p1 && loopfile::read(p1, li) && li.loop, "and back to a loop");
}

void loupeWarpFile() {
  Fixture f = fixture();
  Song s = f.s;
  // (a song made here gets its mixer track on the first load: compare from a loaded one)
  Song first;
  expect(loadProjectText(projectToText(s), "", first), "loads");
  s = first;
  const std::string before = projectToText(s);
  expect(before.find("loupeWarp") == std::string::npos, "no Loupe markers: nothing written");
  Song back;
  expect(loadProjectText(before, "", back), "loads again");
  expect(back.channel(f.kick)->plugin.loupeWarp.empty(), "no Loupe markers: none after loading");
  expect(projectToText(back) == before, "a project without Loupe markers saves as before");
  SampleWarp w;
  w.markers = {{1, 0.0, 0.0}, {2, 0.5, 0.6}};
  normalizeWarp(w.markers);
  const std::string text = sampleWarpToText(w);
  expect(!text.empty(), "markers as text");
  Document d(s);
  const uint64_t u0 = d.history().stateSerial();
  d.editChannel(f.kick, "Loupe: warp markers", [&](Channel& c) { c.plugin.loupeWarp = text; });
  expect(d.history().stateSerial() != u0, "setting them is an edit");
  const std::string saved = projectToText(d.song());
  expect(saved.find("\"loupeWarp\"") != std::string::npos, "saved with the project");
  Song again;
  expect(loadProjectText(saved, "", again), "and loads");
  SampleWarp w2;
  expect(sampleWarpFromText(again.channel(f.kick)->plugin.loupeWarp, w2) && w2.markers == w.markers, "the same markers come back");
  expect(projectToText(again) == saved, "and save the same again");
  d.undo();
  expect(d.song().channel(f.kick)->plugin.loupeWarp.empty() && d.history().stateSerial() == u0, "one undo step takes them away");
  d.redo();
  expect(d.song().channel(f.kick)->plugin.loupeWarp == text, "redo brings them back");
}

void loupeWarpCollab() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(78);
  Peer a, b;
  a.label = "Ann", b.label = "Ben";
  for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
  Fixture f = fixture();
  a.doc.reset(f.s);
  a.folder = tmpDir("k078p2_a");
  b.folder = tmpDir("k078p2_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  expect(a.s != nullptr, "a session");
  if (!a.s) return;
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  SampleWarp w;
  w.markers = {{1, 0.0, 0.0}, {2, 0.4, 0.5}};
  normalizeWarp(w.markers);
  const std::string text = sampleWarpToText(w);
  a.doc.editChannel(f.kick, "Loupe: warp markers", [&](Channel& c) { c.plugin.loupeWarp = text; });
  settle(net, {&a, &b}, clock);
  const Channel* cb = b.doc.song().channel(f.kick);
  expect(cb && cb->plugin.loupeWarp == text, "Ann's Loupe markers reach Ben");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  cb = b.doc.song().channel(f.kick);
  expect(cb && cb->plugin.loupeWarp.empty(), "her undo takes them away on Ben's computer");
  a.doc.redo();
  settle(net, {&a, &b}, clock);
  cb = b.doc.song().channel(f.kick);
  expect(cb && cb->plugin.loupeWarp == text, "and her redo brings them back");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k078picks2");
  loopTag();
  loupeWarpFile();
  loupeWarpCollab();
  return finish();
}

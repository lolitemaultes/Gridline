// Gridline: Loupe's audio tools on playlist audio clips (engine/ClipTools.h). Checks:
//  * a clip alone on its channel: a new file in the edited-audio folder, the source file untouched
//    (bytes and time), the channel plays the new file, one undo step brings the original back;
//  * the clip's used part only: a clip slipped and trimmed inside its file is amplified, the audio
//    outside its part is the source's, and the song renders exactly the old clip at the new level;
//  * Reverse on a slipped clip renders the clip's own audio backwards; a reversed clip fades in
//    where it is heard to start;
//  * a sample shared by two clips: only the processed clip gets a channel of its own, named
//    "<name> (edited)", the other keeps the original; an edited copy processed again (shared once
//    more) is not named "(edited) (edited)", and its new file is named without the ending;
//  * several clips at once: each its own file, one undo step for all (Reverse too);
//  * a selection inside the clip: only that part changes;
//  * length: the drum loop stretch to half the tempo doubles the clip's audio and its length; reverb
//    lengthens the clip by its tail;
//  * Clean up (denoise) runs on a noisy clip and leaves the source alone;
//  * a stopped run changes nothing;
//  * collaboration: the edit reaches the other side as one edit with the new file, undo too.
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/ClipTools.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "packs/edison/edison.h"

using namespace gl;
namespace ed = gl::packs::edison;
namespace ct = gl::cliptools;
namespace fs = std::filesystem;

namespace {

std::string gDir, gEdited;
constexpr double kRate = 44100;
constexpr double kTicksPerSec = 192;  // 120 BPM, 96 ticks a beat

// Two seconds: left a ramp 0 .. 0.5, right a 330 Hz tone at 0.3 (every part tells where it came from).
ed::Audio rampAudio(double sec = 2.0) {
  const auto n = static_cast<int64_t>(sec * kRate);
  ed::Audio a = ed::makeAudio(kRate, 2, n);
  for (int64_t i = 0; i < n; ++i) {
    a.L[static_cast<size_t>(i)] = 0.5f * static_cast<float>(i) / static_cast<float>(n);
    a.R[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(2 * M_PI * 330 * static_cast<double>(i) / kRate));
  }
  return a;
}

std::string writeWav(const std::string& name, const ed::Audio& a) {
  const std::string p = gDir + "/" + name + ".wav";
  ed::writeWavFile(p, ed::makeContent(a), 32);
  return p;
}

std::string bytes(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

ed::Audio load(const std::string& p) {
  ed::Content c;
  ed::loadContent(p, c);
  return c.audio ? c.a() : ed::Audio{};
}

ObjectId addChannel(Song& s, const std::string& path) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = fs::path(path).stem().string();
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  return ch.id;
}

ObjectId addClip(Song& s, ObjectId ch, Tick start, Tick length, int track, double audioStart = 0) {
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch;
  c.track = track;
  c.start = start;
  c.length = length;
  c.audioStart = audioStart;
  s.playlist.push_back(c);
  return c.id;
}

Song song() {
  Song s = makeEmptySong();
  s.tempo = 120;
  return s;
}

Tick ticks(double sec) { return static_cast<Tick>(std::llround(sec * kTicksPerSec)); }

AudioBuffer render(const Song& s, Tick end) {
  RenderJob j;
  j.sampleRate = static_cast<int>(kRate);
  j.scope = RenderScope::Range;
  j.start = 0;
  j.end = end;
  j.tail = TailMode::Cut;
  j.masterEffects = false;
  return renderAudio(s, j).audio;
}

std::string pathOf(const Song& s, ObjectId clip) {
  const Clip* c = s.clip(clip);
  const Channel* ch = c ? s.channel(c->source) : nullptr;
  return ch ? ch->plugin.data : std::string();
}

ed::ToolParams tool(ed::ToolKind k) {
  ed::ToolParams p;
  p.kind = k;
  return p;
}

void testAlone() {
  const std::string src = writeWav("alone", rampAudio());
  const std::string before = bytes(src);
  const auto mtime = fs::last_write_time(src);
  Song s = song();
  const ObjectId ch = addChannel(s, src);
  const ObjectId id = addClip(s, ch, 0, ticks(2.0), 0);
  Document doc(s);
  const size_t pos = doc.history().position();
  std::string why;
  const int n = ct::processClips(doc, {id}, tool(ed::ToolKind::Reverse), gEdited, 0, 0, &why);
  expect(n == 1, ("reverse: one clip changed " + why).c_str());
  const std::string now = pathOf(doc.song(), id);
  expect(!now.empty() && now != src && fs::is_regular_file(now), "the clip plays a new file");
  expect(fs::path(now).parent_path() == fs::path(gEdited), "in the edited-audio folder");
  expect(bytes(src) == before && fs::last_write_time(src) == mtime, "the source file is untouched");
  expect(doc.song().clip(id)->source == ch && doc.song().channels.size() == s.channels.size(), "a clip alone on its channel keeps the channel");
  const ed::Audio out = load(now);
  expect(out.frames() == static_cast<int64_t>(2.0 * kRate) && std::fabs(out.L[0] - 0.5f * (out.frames() - 1) / out.frames()) < 1e-5f, "the file is the audio backwards");
  expect(doc.history().position() == pos + 1, "one undo step");
  expect(doc.undo() && pathOf(doc.song(), id) == src, "undo: the clip plays the original again");
}

void testUsedPart() {
  const std::string src = writeWav("part", rampAudio());
  Song s = song();
  const ObjectId ch = addChannel(s, src);
  // Plays [0.5, 1.25) of the file from 1 s into the song.
  const ObjectId id = addClip(s, ch, ticks(1.0), ticks(0.75), 0, 0.5);
  const Tick end = ticks(2.5);
  const AudioBuffer was = render(s, end);
  Document doc(s);
  ed::ToolParams p = tool(ed::ToolKind::Amplify);
  p.amplifyDb = -6.0206;
  expect(ct::processClips(doc, {id}, p, gEdited) == 1, "amplify the slipped, trimmed clip");
  const ed::Audio orig = load(src), out = load(pathOf(doc.song(), id));
  expect(out.frames() == orig.frames(), "a tool that keeps the length writes the whole file");
  const auto f = [](double sec) { return static_cast<size_t>(std::llround(sec * kRate)); };
  expect(out.L[f(0.25)] == orig.L[f(0.25)] && out.L[f(1.6)] == orig.L[f(1.6)], "outside the clip's part the file is the source's");
  expect(std::fabs(out.L[f(0.9)] - 0.5f * orig.L[f(0.9)]) < 1e-4f, "inside it the audio is amplified");
  const Clip* c = doc.song().clip(id);
  expect(c && c->audioStart == 0.5 && c->length == ticks(0.75) && c->start == ticks(1.0), "the clip keeps its start, slip and length");
  const AudioBuffer now = render(doc.song(), end);
  double d = was.frames == now.frames ? 0.0 : 1.0;
  for (size_t k = 0; k < 2; ++k)
    for (size_t i = 0; i < std::min(was.frames, now.frames); ++i) d = std::max(d, static_cast<double>(std::fabs(now.at(k, i) - 0.5f * was.at(k, i))));
  std::printf("# amplified render: max difference to half the old one %.3g\n", d);
  expect(d < 2e-3, "the song plays the same part of the clip at half the level");
}

void testReverseSlipped() {
  const std::string src = writeWav("revslip", rampAudio());
  Song s = song();
  const ObjectId ch = addChannel(s, src);
  const ObjectId id = addClip(s, ch, 0, ticks(1.0), 0, 0.5);  // plays [0.5, 1.5)
  const ed::Audio orig = load(src);
  Document doc(s);
  expect(ct::processClips(doc, {id}, tool(ed::ToolKind::Reverse), gEdited) == 1, "reverse a slipped clip");
  const ed::Audio out = load(pathOf(doc.song(), id));
  const auto f = [](double sec) { return static_cast<int64_t>(std::llround(sec * kRate)); };
  expect(out.L[static_cast<size_t>(f(0.5))] == orig.L[static_cast<size_t>(f(1.5) - 1)], "the clip's part runs backwards in the new file");
  expect(out.L[static_cast<size_t>(f(0.2))] == orig.L[static_cast<size_t>(f(0.2))], "the rest stays");
  // A reversed clip fades in where it is heard to start: the end of its part of the file.
  Song r = song();
  const ObjectId rch = addChannel(r, src);
  const ObjectId rid = addClip(r, rch, 0, ticks(2.0), 0);
  r.clip(rid)->reverse = true;
  Document rd(r);
  expect(ct::processClips(rd, {rid}, tool(ed::ToolKind::FadeIn), gEdited) == 1, "fade in a reversed clip");
  const ed::Audio fo = load(pathOf(rd.song(), rid));
  expect(fo.frames() == orig.frames() && std::fabs(fo.L[static_cast<size_t>(fo.frames() - 2)]) < 1e-3f && std::fabs(fo.L[10] - orig.L[10]) < 1e-6f,
         "reversed: the fade is at the file's end, where the clip starts");
}

void testShared() {
  const std::string src = writeWav("shared", rampAudio());
  Song s = song();
  const ObjectId ch = addChannel(s, src);
  const ObjectId a = addClip(s, ch, 0, ticks(1.0), 0);
  const ObjectId b = addClip(s, ch, ticks(1.0), ticks(1.0), 1);
  Document doc(s);
  expect(ct::processClips(doc, {a}, tool(ed::ToolKind::Reverse), gEdited) == 1, "reverse one of two clips of a sample");
  const Song& x = doc.song();
  expect(x.clip(a)->source != ch && x.channel(x.clip(a)->source) && pathOf(x, a) != src, "the processed clip gets a channel of its own");
  expect(x.clip(b)->source == ch && pathOf(x, b) == src, "the other clip keeps the original");
  const Channel* nc = x.channel(x.clip(a)->source);
  expect(nc && nc->mixerTrack == x.channel(ch)->mixerTrack && nc->plugin.type == x.channel(ch)->plugin.type, "the new channel is the old one's copy");
  expect(nc && nc->name == "shared (edited)" && x.channel(ch)->name == "shared", "named \"<name> (edited)\", the original keeps its name");
  // The edited copy shared again (a clip duplicated onto it) and processed once more.
  const ObjectId copyId = x.clip(a)->source;
  ObjectId a2 = kNoId;
  doc.editSong("dup", [&](Song& y) { a2 = addClip(y, copyId, ticks(2.0), ticks(1.0), 2); });
  ed::ToolParams amp = tool(ed::ToolKind::Amplify);
  amp.amplifyDb = -3;
  expect(ct::processClips(doc, {a}, amp, gEdited) == 1, "amplify the edited clip");
  const Song& z = doc.song();
  const Channel* nc2 = z.channel(z.clip(a)->source);
  expect(nc2 && nc2->id != copyId && z.clip(a2)->source == copyId, "a channel of its own again");
  expect(nc2 && nc2->name == "shared (edited)", "not \"(edited) (edited)\"");
  expect(fs::path(pathOf(z, a)).filename().string().find("edited") == std::string::npos, "the new file is named without the ending");
  doc.undo();
  doc.undo();
  expect(doc.undo() && doc.song().clip(a)->source == ch && doc.song().channels.size() == s.channels.size(), "undo: back on the shared channel, the copy gone");
}

void testEditedName() {
  expect(ct::editedName("Kick") == "Kick (edited)", "Kick: Kick (edited)");
  expect(ct::editedName("Kick (edited)") == "Kick (edited)", "an edited name stays as it is");
  expect(ct::uneditedName("Kick (edited)") == "Kick" && ct::uneditedName("Kick") == "Kick", "the name without the ending");
  expect(ct::editedName(" (edited)") == " (edited) (edited)", "a name that is only the ending is a name");
}

void testMulti() {
  const std::string src = writeWav("multi", rampAudio());
  Song s = song();
  const ObjectId ch = addChannel(s, src);
  const ObjectId a = addClip(s, ch, 0, ticks(0.5), 0);
  const ObjectId b = addClip(s, ch, ticks(1.0), ticks(0.5), 1, 1.0);
  const ObjectId ch2 = addChannel(s, writeWav("multi2", rampAudio(1.0)));
  const ObjectId c = addClip(s, ch2, ticks(2.0), ticks(1.0), 2);
  Document doc(s);
  const size_t pos = doc.history().position();
  ed::ToolParams p = tool(ed::ToolKind::Normalize);
  p.normalizeDb = -3;
  expect(ct::processClips(doc, {a, b, c}, p, gEdited) == 3, "normalize three clips");
  const Song& x = doc.song();
  expect(doc.history().position() == pos + 1, "one undo step for all of them");
  expect(pathOf(x, a) != pathOf(x, b) && pathOf(x, a) != src && pathOf(x, b) != src && pathOf(x, c) != pathOf(s, c), "each clip its own new file");
  expect(x.clip(a)->source != x.clip(b)->source, "the two clips of one sample on channels of their own");
  // Each clip's own part was normalized: clip b plays [1.0, 1.5) of the file.
  const ed::Audio fb = load(pathOf(x, b));
  float pk = 0;
  for (int64_t i = static_cast<int64_t>(1.0 * kRate); i < static_cast<int64_t>(1.5 * kRate); ++i) pk = std::max({pk, std::fabs(fb.L[static_cast<size_t>(i)]), std::fabs(fb.R[static_cast<size_t>(i)])});
  expect(std::fabs(20 * std::log10(pk) + 3) < 0.05, "clip b's part peaks at -3 dB");
  expect(doc.undo() && pathOf(doc.song(), a) == src && pathOf(doc.song(), b) == src && pathOf(doc.song(), c) == pathOf(s, c), "undo brings every original back");
  // Reverse (run straight from the menu, with no settings) on all three: one undo step too.
  const size_t pos2 = doc.history().position();
  expect(ct::processClips(doc, {a, b, c}, tool(ed::ToolKind::Reverse), gEdited) == 3 && doc.history().position() == pos2 + 1, "reverse three clips in one step");
  expect(doc.history().undoLabel() == "Reverse 3 clips", "named for the three clips");
  expect(doc.undo() && pathOf(doc.song(), a) == src && pathOf(doc.song(), c) == pathOf(s, c), "one undo brings them back");
}

void testSelection() {
  const std::string src = writeWav("sel", rampAudio());
  Song s = song();
  const ObjectId ch = addChannel(s, src);
  const ObjectId id = addClip(s, ch, 0, ticks(2.0), 0);
  Document doc(s);
  ed::ToolParams p = tool(ed::ToolKind::Amplify);
  p.amplifyDb = -60;
  // Ticks [0.5 s, 1.0 s) of the clip.
  expect(ct::processClips(doc, {id}, p, gEdited, static_cast<double>(ticks(0.5)), static_cast<double>(ticks(1.0))) == 1, "amplify a selection in the clip");
  const ed::Audio orig = load(src), out = load(pathOf(doc.song(), id));
  const auto f = [](double sec) { return static_cast<size_t>(std::llround(sec * kRate)); };
  expect(out.L[f(0.4)] == orig.L[f(0.4)] && out.L[f(1.2)] == orig.L[f(1.2)], "outside the selection: unchanged");
  expect(std::fabs(out.L[f(0.75)]) < 1e-3f * std::fabs(orig.L[f(0.75)]) + 1e-6f, "inside it: the tool's result");
}

// Hits every beat (120 BPM): a short noise burst each.
ed::Audio drumAudio(double sec) {
  const auto n = static_cast<int64_t>(sec * kRate);
  ed::Audio a = ed::makeAudio(kRate, 2, n);
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  const int64_t beat = static_cast<int64_t>(0.5 * kRate);
  for (int64_t i = 0; i < n; ++i) {
    const int64_t k = i % beat;
    const float env = static_cast<float>(std::exp(-static_cast<double>(k) / (0.02 * kRate)));
    a.L[static_cast<size_t>(i)] = a.R[static_cast<size_t>(i)] = 0.8f * env * u(rng);
  }
  return a;
}

void testLength() {
  const std::string src = writeWav("drums", drumAudio(2.0));
  Song s = song();
  const ObjectId ch = addChannel(s, src);
  const ObjectId id = addClip(s, ch, 0, ticks(2.0), 0);  // one bar at 120 BPM
  Document doc(s);
  ed::ToolParams p = tool(ed::ToolKind::DrumStretch);
  p.drum.sourceBpm = 120;
  p.drum.targetBpm = 60;
  p.drum.bars = 1;  // a bar at the new tempo: twice as long
  expect(ct::processClips(doc, {id}, p, gEdited) == 1, "drum loop stretch to half the tempo");
  const ed::Audio out = load(pathOf(doc.song(), id));
  expect(std::llabs(out.frames() - static_cast<int64_t>(4.0 * kRate)) <= 2, "the new file holds the loop at its new length (4 s)");
  const Clip* c = doc.song().clip(id);
  std::printf("# stretched clip length %lld ticks (was %lld)\n", c ? static_cast<long long>(c->length) : -1LL, static_cast<long long>(ticks(2.0)));
  expect(c && std::llabs(c->length - ticks(4.0)) <= 1 && c->audioStart == 0 && c->audioEnd == 0, "the clip takes the new length and plays all of the new file");
  expect(doc.undo() && doc.song().clip(id)->length == ticks(2.0) && pathOf(doc.song(), id) == src, "undo: the old loop and length");
  // Several loops: the same tempo change, each its own new length.
  Song m = song();
  const ObjectId mch = addChannel(m, src);
  const ObjectId m1 = addClip(m, mch, 0, ticks(2.0), 0);
  const ObjectId m2 = addClip(m, mch, ticks(2.0), ticks(1.0), 1);  // half the loop
  Document md(m);
  expect(ct::processClips(md, {m1, m2}, p, gEdited) == 2, "two loops stretched together");
  expect(std::llabs(md.song().clip(m1)->length - ticks(4.0)) <= 1 && std::llabs(md.song().clip(m2)->length - ticks(2.0)) <= 1, "each loop twice as long");
  // Reverb: the clip grows by its tail.
  Song r = song();
  const ObjectId rch = addChannel(r, writeWav("verb", rampAudio(1.0)));
  const ObjectId rid = addClip(r, rch, 0, ticks(1.0), 0);
  Document rd(r);
  expect(ct::processClips(rd, {rid}, tool(ed::ToolKind::Reverb), gEdited) == 1, "reverb on a clip");
  expect(rd.song().clip(rid)->length > ticks(1.0), "the clip grows by the reverb's tail");
}

void testDenoise() {
  ed::Audio a = rampAudio();
  std::mt19937 rng(11);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (int64_t i = 0; i < a.frames(); ++i) a.R[static_cast<size_t>(i)] += 0.02f * u(rng), a.L[static_cast<size_t>(i)] += 0.02f * u(rng);
  const std::string src = writeWav("noisy", a);
  const std::string before = bytes(src);
  Song s = song();
  const ObjectId id = addClip(s, addChannel(s, src), 0, ticks(2.0), 0);
  Document doc(s);
  expect(ct::processClips(doc, {id}, tool(ed::ToolKind::Denoise), gEdited) == 1, "clean up a noisy clip");
  const ed::Audio out = load(pathOf(doc.song(), id));
  double diff = 0;
  for (int64_t i = 0; i < out.frames(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(out.R[static_cast<size_t>(i)] - a.R[static_cast<size_t>(i)])));
  expect(out.frames() == a.frames() && diff > 1e-3, "the cleaned file differs from the noisy one");
  expect(bytes(src) == before, "the source stays as it was");
}

void testStopped() {
  const std::string src = writeWav("stop", rampAudio());
  Song s = song();
  const ObjectId id = addClip(s, addChannel(s, src), 0, ticks(2.0), 0);
  ct::Target t;
  expect(ct::target(s, id, t), "a target");
  ed::Progress pr;
  pr.cancel = true;
  const ct::Result r = ct::render(t, tool(ed::ToolKind::Denoise), gEdited, &pr);
  expect(r.path.empty(), "a stopped run writes nothing");
  Song x = s;
  expect(!ct::apply(x, r) && pathOf(x, id) == src, "and changes nothing");
}

void testCollab() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(78);
  Peer a, b;
  a.label = "Ann", b.label = "Ben";
  for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
  a.folder = tmpDir("k078ct_a");
  b.folder = tmpDir("k078ct_b");
  const std::string src = (fs::path(a.folder) / "take.wav").string();
  ed::writeWavFile(src, ed::makeContent(rampAudio()), 32);
  Song s = song();
  const ObjectId ch = addChannel(s, src);
  const ObjectId id = addClip(s, ch, 0, ticks(2.0), 0);
  const ObjectId other = addClip(s, ch, ticks(2.0), ticks(2.0), 1);
  a.doc.reset(s);
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
  const size_t posB = b.doc.history().position();
  expect(ct::processClips(a.doc, {id}, tool(ed::ToolKind::Reverse), (fs::path(a.folder) / "Edited audio").string()) == 1, "Ann reverses one clip");
  settle(net, {&a, &b}, clock);
  run(net, {&a, &b}, clock, 2.0);
  settle(net, {&a, &b}, clock);
  const Song& y = b.doc.song();
  const Clip* bc = y.clip(id);
  const Channel* bch = bc ? y.channel(bc->source) : nullptr;
  expect(bc && bc->source != ch && bch, "Ben's clip plays the new channel");
  expect(y.clip(other) && y.clip(other)->source == ch, "Ben's other clip keeps the original");
  std::error_code ec;
  const bool here = bch && fs::is_regular_file(bch->plugin.data, ec);
  expect(here, "the new file arrives on Ben's computer");
  if (here) {
    const ed::Audio mine = load(pathOf(a.doc.song(), id)), theirs = load(bch->plugin.data);
    expect(mine.L == theirs.L && mine.R == theirs.R, "with the same sound");
  }
  expect(b.doc.history().position() <= posB + 1, "one edit");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(b.doc.song().clip(id) && b.doc.song().clip(id)->source == ch, "Ann's undo brings the original back on Ben's computer");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k078ct");
  gEdited = gDir + "/Edited audio";
  testAlone();
  testUsedPart();
  testReverseSlipped();
  testShared();
  testEditedName();
  testMulti();
  testSelection();
  testLength();
  testDenoise();
  testStopped();
  testCollab();
  return finish();
}

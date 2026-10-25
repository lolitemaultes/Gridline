// Gridline — J-118: the playlist's clip brush (engine/PlaylistTools.h). A clip
// clicked in the playlist is painted as it is:
//  * makeClipLike copies every clip setting (length, audio start / end, offset,
//    fades and their curves, gain, pitch, stretch, tempo sync, reverse) and
//    resets what belongs to the original only (id, place, mute, group, take,
//    launch state);
//  * makeBrushClip / brushLength use the brush only for clips of its source;
//    without a brush (a source picked in the picker) a new clip gets the
//    source's natural length, as before;
//  * pattern and automation brushes keep their length and content offset;
//  * painted copies go through Document::editSong: one undo step, redo, and a
//    project save / load round trip keep every copied setting.
// The app (click a trimmed clip, paint, pick a sample in the picker) is
// tests/j118_paint_brush.cmake.
#include <filesystem>
#include <string>
#include "../src/core/Document.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/core/WavWriter.h"
#include "../src/engine/ClipOps.h"
#include "../src/engine/PlaylistTools.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;

std::string writeNoise(const char* name, double seconds) {
  const std::string path = (fs::temp_directory_path() / name).string();
  AudioBuffer b(2, static_cast<size_t>(seconds * kRate));
  uint32_t r = 12345;
  for (size_t i = 0; i < b.frames; ++i) {
    r = r * 1664525u + 1013904223u;
    b.at(0, i) = b.at(1, i) = 0.4f * (static_cast<float>(r >> 8) / static_cast<float>(1u << 24) - 0.5f);
  }
  writeWav(path, b, kRate, 16);
  return path;
}

ObjectId addSampler(Song& s, const std::string& wav, const char* name) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  return ch.id;
}

// A trimmed, shortened audio clip with every setting away from its default.
Clip trimmedClip(Song& s, ObjectId chan) {
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Audio, c.source = chan, c.track = 2, c.start = s.barTicks();
  c.length = s.beatTicks() * 3;
  c.audioStart = 0.5, c.audioEnd = 1.75;
  c.fadeIn = 0.05, c.fadeOut = 0.12, c.fadeInManual = c.fadeOutManual = true;
  c.fadeInShape = 1, c.fadeOutTension = -2.5f, c.fadeInDeclick = 2;
  c.gain = 0.5f, c.pitch = 150.0f, c.stretch = 1.25, c.syncBpm = 0, c.reverse = true;
  c.muted = true, c.group = 77, c.takeGroup = 78, c.take = 2, c.launched = true;
  s.playlist.push_back(c);
  return c;
}

// The settings a brush copy keeps (everything but id, place, mute, group, take, launch).
bool sameSettings(const Clip& a, const Clip& b) {
  Clip x = a, y = b;
  for (Clip* c : {&x, &y}) c->id = kNoId, c->track = 0, c->start = 0, c->muted = false, c->group = kNoId, c->takeGroup = kNoId, c->take = 0, c->launched = false;
  return x == y;
}

void testMakeClipLike(Song& s, const Clip& brush) {
  const Clip c = pltools::makeClipLike(brush, 5, 1000);
  expect(c.id == kNoId && c.track == 5 && c.start == 1000, "makeClipLike: a new clip (no id) at the asked track and start");
  expect(c.length == brush.length && c.audioStart == brush.audioStart && c.audioEnd == brush.audioEnd, "makeClipLike: length, audio start and end kept");
  expect(c.fadeIn == brush.fadeIn && c.fadeOut == brush.fadeOut && c.fadeInManual && c.fadeOutManual && c.fadeInShape == 1 &&
             c.fadeOutTension == -2.5f && c.fadeInDeclick == 2,
         "makeClipLike: fades, their curves and hand-set flags kept");
  expect(c.gain == 0.5f && c.pitch == 150.0f && c.stretch == 1.25 && c.reverse, "makeClipLike: gain, pitch, stretch and reverse kept");
  expect(!c.muted && c.group == kNoId && c.takeGroup == kNoId && c.take == 0 && !c.launched,
         "makeClipLike: not muted, no group, no recording take, not launched");
  expect(sameSettings(c, brush), "makeClipLike: every other setting equal");
  expect(pltools::makeClipLike(brush, 0, -50).start == 0, "makeClipLike: never before the song start");
  (void)s;
}

void testBrushChoice(Song& s, const Clip& brush, ObjectId otherChan) {
  const pltools::Source src = pltools::sourceOf(brush);
  const Clip painted = pltools::makeBrushClip(s, src, &brush, 3, 2000);
  expect(sameSettings(painted, brush) && painted.track == 3 && painted.start == 2000, "makeBrushClip: the brush's source -> a copy of the brush");
  expect(pltools::brushLength(s, src, &brush) == brush.length, "brushLength: the brush's length");

  const Clip natural = pltools::makeBrushClip(s, src, nullptr, 3, 2000);
  const Tick nat = pltools::sourceLength(s, src);
  expect(natural.length == nat && natural.audioStart == 0 && natural.fadeIn == 0 && natural.gain == 1.0f && !natural.reverse,
         "makeBrushClip: no brush (picked in the picker) -> the source's natural clip");
  expect(nat > brush.length, "... which is longer than the trimmed brush (the bug: the base sample at its full length)");
  expect(pltools::brushLength(s, src, nullptr) == nat, "brushLength: no brush -> the natural length");

  const pltools::Source other{ClipKind::Audio, otherChan};
  const Clip o = pltools::makeBrushClip(s, other, &brush, 3, 2000);
  expect(o.source == otherChan && o.length == pltools::sourceLength(s, other) && o.audioStart == 0 && o.gain == 1.0f,
         "makeBrushClip: another source than the brush's -> that source's natural clip");
  expect(pltools::brushLength(s, other, &brush) == pltools::sourceLength(s, other), "brushLength: another source -> its natural length");
}

void testPatternBrush(Song& s) {
  Pattern p;
  p.id = s.allocateId();
  p.name = "P";
  s.patterns.push_back(p);
  Clip b;
  b.id = s.allocateId(), b.kind = ClipKind::Pattern, b.pattern = p.id, b.track = 0, b.start = 0;
  b.length = s.beatTicks() * 6, b.offset = s.beatTicks();
  const pltools::Source src = pltools::sourceOf(b);
  const Clip c = pltools::makeBrushClip(s, src, &b, 1, 384);
  expect(c.kind == ClipKind::Pattern && c.pattern == p.id && c.length == b.length && c.offset == b.offset,
         "pattern brush: the copy keeps the clip's length and offset");
  expect(pltools::brushLength(s, src, &b) == b.length, "pattern brush: its length paints");
  const Clip n = pltools::makeBrushClip(s, src, nullptr, 1, 384);
  expect(n.length == 0 && n.offset == 0, "pattern without a brush: the pattern's natural clip (length 0 = natural)");

  AutomationClip ac;
  ac.id = s.allocateId();
  ac.points = {{0, 0.0f, 0.0f, AutoShape::Curve}, {s.barTicks() * 2, 1.0f, 0.0f, AutoShape::Curve}};
  s.automations.push_back(ac);
  Clip ab;
  ab.id = s.allocateId(), ab.kind = ClipKind::Automation, ab.source = ac.id, ab.length = s.beatTicks() * 3, ab.offset = s.beatTicks() * 2;
  const Clip c2 = pltools::makeBrushClip(s, pltools::sourceOf(ab), &ab, 2, 0);
  expect(c2.kind == ClipKind::Automation && c2.length == ab.length && c2.offset == ab.offset, "automation brush: length and offset kept");
}

// Painting through the document (what the playlist does): one undo step,
// redo, and a save / load round trip keep every copied setting.
void testDocument(Song s, const Clip& brush) {
  Document d(std::move(s));
  const size_t n0 = d.song().playlist.size();
  const pltools::Source src = pltools::sourceOf(brush);
  const Tick len = pltools::brushLength(d.song(), src, &brush);
  std::vector<ObjectId> ids;
  d.editSong("paint clips", [&](Song& x) {
    for (Tick t : pltools::paintSlots(0, len * 3, len)) {
      Clip c = pltools::makeBrushClip(x, src, &brush, 6, t);
      c.id = x.allocateId();
      x.playlist.push_back(c);
      ids.push_back(c.id);
    }
  });
  expect(ids.size() == 4 && d.song().playlist.size() == n0 + 4, "paint: four copies laid back to back");
  bool all = true;
  for (size_t i = 0; i < ids.size(); ++i) {
    const Clip* c = d.song().clip(ids[i]);
    all = all && c && sameSettings(*c, brush) && c->start == static_cast<Tick>(i) * len && d.song().clipLength(*c) == len;
  }
  expect(all, "paint: every copy has the brush's settings and length");
  expect(d.undo() && d.song().playlist.size() == n0, "undo: the painted clips go in one step");
  expect(d.redo() && d.song().playlist.size() == n0 + 4, "redo: back");
  const Clip* c = d.song().clip(ids[2]);
  expect(c && sameSettings(*c, brush), "redo: settings intact");

  Song loaded;
  std::string err;
  const bool ok = songFromJson(songToJson(d.song()), loaded, &err);
  expect(ok, ("save / load: the project round trips " + err).c_str());
  const Clip* lc = loaded.clip(ids[2]);
  expect(lc && c && *lc == *c, "save / load: a painted copy keeps every setting");
}

}  // namespace

int main() {
  Song s;
  const ObjectId loop = addSampler(s, writeNoise("gl_j118_loop.wav", 4.0), "Loop");
  const ObjectId pad = addSampler(s, writeNoise("gl_j118_pad.wav", 2.0), "Pad");
  const Clip brush = trimmedClip(s, loop);
  testMakeClipLike(s, brush);
  testBrushChoice(s, brush, pad);
  testPatternBrush(s);
  testDocument(s, brush);
  return finish();
}

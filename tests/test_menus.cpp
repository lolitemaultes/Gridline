// Gridline — tests for the operations behind the main menu and the context
// menus (core/MenuOps): each one edits the song correctly and undoes in one step.
#include <algorithm>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/MenuOps.h"

using namespace gl;

namespace {

// Two channels, one pattern with steps on the first and notes on the second,
// two clips of that pattern on track 0.
struct Fixture {
  Document d{makeEmptySong()};
  ObjectId kick = kNoId, lead = kNoId, pat = kNoId, c1 = kNoId, c2 = kNoId;
  Fixture() {
    kick = d.addChannel("Kick", "gl.kick");
    lead = d.addChannel("Lead", "gl.synth");
    pat = d.song().patterns[0].id;
    d.setStep(pat, kick, 0, 100);
    d.setStep(pat, kick, 8, 100);
    d.setNotes(pat, lead, {{0, 96, 60, 100}, {96, 96, 64, 90}, {192, 96, 67, 80}});
    c1 = d.addClip(pat, 0, 0);
    c2 = d.addClip(pat, 0, d.song().barTicks());
    d.reset(d.song());  // fixture is the baseline: no history
  }
  bool undoesToBaseline(const Song& base) {
    bool ok = d.undo();
    return ok && d.song() == base;
  }
};

std::vector<bool> allFalse(size_t n) { return std::vector<bool>(n, false); }

void testPatterns() {
  Fixture f;
  Song base = f.d.song();
  // Empty pattern search.
  expect(ops::firstEmptyPattern(f.d.song()) == kNoId, "no empty pattern at first");
  ObjectId e = ops::nextEmptyOrNewPattern(f.d, f.pat);
  expect(e != kNoId && f.d.song().patterns.size() == 2, "next empty creates a pattern");
  expect(ops::firstEmptyPattern(f.d.song()) == e, "first empty finds it");
  expect(ops::nextEmptyOrNewPattern(f.d, f.pat) == e, "next empty reuses an existing empty pattern");
  expect(ops::nextPatternName(f.d.song()) == "Pattern 3", "next pattern name skips used names");
  // Unused patterns.
  auto unused = ops::unusedPatterns(f.d.song());
  expect(unused.size() == 1 && unused[0] == e, "unused pattern detected");
  expect(ops::deleteUnusedPatterns(f.d) == 1 && f.d.song().patterns.size() == 1, "delete unused patterns");
  f.d.undo();
  expect(f.d.song().patterns.size() == 2, "undo restores unused pattern");
  f.d.undo();
  expect(f.d.song() == base, "undo add pattern");
  // Move / insert.
  ObjectId p2 = f.d.addPattern("Two");
  expect(ops::movePattern(f.d, p2, 0) && f.d.song().patterns[0].id == p2, "move pattern up");
  ObjectId ins = ops::insertPattern(f.d, p2, "Inserted");
  expect(f.d.song().patterns[0].id == ins, "insert pattern before");
  // Transpose.
  f.d.reset(base);
  expect(ops::transposePattern(f.d, f.pat, 12), "transpose pattern");
  expect(f.d.song().pattern(f.pat)->notes.at(f.lead)[0].pitch == 72, "transposed pitch");
  expect(f.undoesToBaseline(base), "transpose undo");
  expect(ops::setPatternColor(f.d, f.pat, 0x123456) && f.d.song().pattern(f.pat)->color == 0x123456, "pattern colour");
}

void testSplitByChannel() {
  Fixture f;
  Song base = f.d.song();
  auto ids = ops::splitPatternByChannel(f.d, f.pat);
  const Song& s = f.d.song();
  expect(ids.size() == 2, "split makes one pattern per channel");
  expect(s.patterns.size() == 3, "original pattern kept");
  expect(s.pattern(ids[0])->steps.count(f.kick) && !s.pattern(ids[0])->notes.count(f.lead), "first split has only kick");
  expect(s.pattern(ids[1])->notes.count(f.lead) && !s.pattern(ids[1])->steps.count(f.kick), "second split has only lead");
  expect(s.pattern(ids[1])->name == "Pattern 1 - Lead", "split names");
  int onOrig = 0, onSplit = 0;
  for (const auto& c : s.playlist) (c.pattern == f.pat ? onOrig : onSplit)++;
  expect(onOrig == 0 && onSplit == 4, "clips replaced by split clips");
  bool tracksOk = true;
  for (const auto& c : s.playlist) tracksOk = tracksOk && (c.pattern == ids[0] ? c.track == 0 : c.track == 1);
  expect(tracksOk, "split clips on consecutive tracks");
  expect(f.undoesToBaseline(base), "split undoes in one step");
  ObjectId empty = f.d.addPattern("Empty");
  expect(ops::splitPatternByChannel(f.d, empty).empty(), "empty pattern does not split");
}

void testChannels() {
  Fixture f;
  Song base = f.d.song();
  expect(ops::fillSteps(f.d, f.pat, f.lead, 4), "fill every 4");
  {
    const auto& row = f.d.song().pattern(f.pat)->steps.at(f.lead);
    int on = 0;
    for (size_t k = 0; k < row.size(); ++k) on += row[k] ? 1 : 0;
    expect(on == 4 && row[0] && row[4] && row[12] && !row[1], "fill every 4 pattern");
  }
  expect(ops::fillSteps(f.d, f.pat, f.lead, 4, 2) && f.d.song().pattern(f.pat)->steps.at(f.lead)[2] == 100, "fill with offset");
  f.d.reset(base);
  expect(ops::rotateChannel(f.d, f.pat, f.kick, 1), "rotate right");
  expect(f.d.song().pattern(f.pat)->steps.at(f.kick)[1] == 100 && f.d.song().pattern(f.pat)->steps.at(f.kick)[0] == 0, "rotated steps");
  expect(f.undoesToBaseline(base), "rotate undoes in one step");
  expect(ops::rotateChannel(f.d, f.pat, f.lead, -1), "rotate notes left");
  {
    const auto& ns = f.d.song().pattern(f.pat)->notes.at(f.lead);
    bool wrapped = false;
    for (const auto& n : ns) wrapped = wrapped || (n.pitch == 60 && n.start == f.d.song().barTicks() - f.d.song().ppq / 4);
    expect(wrapped, "first note wraps to the end");
  }
  f.d.reset(base);
  expect(ops::clearChannel(f.d, f.pat, f.kick) && !f.d.song().pattern(f.pat)->steps.count(f.kick), "clear channel");
  expect(f.undoesToBaseline(base), "clear undo");
  // Colours, mute, transpose, sort, move.
  ObjectId third = f.d.addChannel("Alpha", "gl.hat");
  ops::setChannelsColor(f.d, {f.kick}, 0x000000);
  ops::setChannelsColor(f.d, {third}, 0xFFFFFF);
  expect(ops::gradientColor(f.d, {f.kick, f.lead, third}), "gradient");
  Rgb mid = f.d.song().channel(f.lead)->color;
  expect(mid == 0x808080 || mid == 0x7F7F7F, "gradient midpoint");
  expect(!ops::gradientColor(f.d, {f.kick, f.lead}), "gradient needs three channels");
  expect(ops::setChannelsMuted(f.d, {f.kick, f.lead}, true) && f.d.song().channel(f.lead)->muted, "mute channels");
  expect(ops::transposeChannels(f.d, f.pat, {f.lead}, -1) && f.d.song().pattern(f.pat)->notes.at(f.lead)[0].pitch == 59, "transpose channels");
  expect(ops::sortChannels(f.d, ops::ChannelSort::Name) && f.d.song().channels[0].id == third, "sort by name");
  expect(ops::moveChannels(f.d, {f.kick}, 1) && f.d.song().channels.back().id == f.kick, "move channel down");
  expect(!ops::moveChannels(f.d, {f.kick}, 1), "cannot move past the end");
  auto clones = ops::cloneChannels(f.d, {f.kick});
  expect(clones.size() == 1 && f.d.song().pattern(f.pat)->steps.count(clones[0]), "clone keeps steps");
  expect(ops::removeChannels(f.d, {clones[0], third}) && f.d.song().channels.size() == 2, "delete channels");
  ObjectId unusedCh = f.d.addChannel("Unused", "gl.clap");
  auto u = ops::unusedChannels(f.d.song());
  expect(u.size() == 1 && u[0] == unusedCh, "unused channels");
}

void testRouting() {
  Fixture f;
  Song base = f.d.song();
  const Song& s = f.d.song();
  expect(ops::mixerTrackIsFree(s, s.mixer[1].id), "insert 1 is free");
  expect(!ops::mixerTrackIsFree(s, s.masterId()), "master is never free");
  auto used = ops::routeToFreeMixerTracks(f.d, {f.kick, f.lead});
  expect(used.size() == 2 && used[0] == s.mixer[1].id && used[1] == s.mixer[2].id, "routes to inserts 1 and 2");
  expect(s.channel(f.kick)->mixerTrack == used[0] && s.mixerTrack(used[0])->name == "Kick", "track named after channel");
  expect(!ops::mixerTrackIsFree(s, used[0]), "used track no longer free");
  expect(f.undoesToBaseline(base), "route undoes in one step");
  // Consecutive routing.
  ObjectId t3 = s.mixer[3].id;
  expect(ops::routeChannels(f.d, {f.kick, f.lead}, t3, true), "route from track on");
  expect(s.channel(f.kick)->mixerTrack == t3 && s.channel(f.lead)->mixerTrack == s.mixer[4].id, "consecutive routing");
  expect(ops::routeChannels(f.d, {f.kick, f.lead}, t3, false) && s.channel(f.lead)->mixerTrack == t3, "route all to one");
  // Mixer track ops.
  ObjectId m1 = s.mixer[1].id, m2 = s.mixer[2].id;
  expect(ops::moveMixerTrack(f.d, m1, 1) && s.mixer[2].id == m1 && s.mixer[1].id == m2, "move mixer track right");
  expect(!ops::moveMixerTrack(f.d, s.masterId(), 1), "master does not move");
  expect(!ops::moveMixerTrack(f.d, m2, -1), "nothing moves into the master position");
  size_t before = s.mixer.size();
  ObjectId ins = ops::insertMixerTrack(f.d, 1);
  expect(ins && s.mixer[1].id == ins && s.mixer.size() == before + 1, "insert mixer track");
  expect(s.mixer[1].sends.size() == 1 && s.mixer[1].sends[0].to == s.masterId(), "new track sends to master");
  f.d.editMixerTrack(m1, "", [](MixerTrack& m) { m.volume = 0.2f; m.pan = 0.5f; m.muted = true; });
  expect(ops::resetMixerTrack(f.d, m1) && s.mixerTrack(m1)->volume == MixerTrack{}.volume && !s.mixerTrack(m1)->muted, "reset mixer track");
  f.d.setSend(m1, m2, 1.0f);
  expect(ops::routeOnlyTo(f.d, m1, m2) && s.mixerTrack(m1)->sends.size() == 1 && s.mixerTrack(m1)->sends[0].to == m2, "route only to");
  expect(!ops::routeOnlyTo(f.d, m2, m1), "route only to refuses a cycle");
  expect(ops::soloMixerTrack(f.d, m2) && s.mixerTrack(m2)->solo, "solo");
  expect(ops::soloMixerTrack(f.d, m2) && !s.mixerTrack(m2)->solo, "solo toggles off");
  f.d.setSlotPlugin(m2, 0, "gl.reverb");
  expect(ops::setSlotEnabled(f.d, m2, 0, false) && !s.mixerTrack(m2)->slots[0].enabled, "disable slot");
  // Trim.
  f.d.reset(base);
  size_t n0 = f.d.song().mixer.size();
  int trimmed = ops::trimUnusedMixerTracks(f.d);
  expect(trimmed == static_cast<int>(n0) - 1 && f.d.song().mixer.size() == 1, "trim removes trailing free tracks");
  f.d.undo();
  ops::routeToFreeMixerTracks(f.d, {f.kick});
  expect(ops::trimUnusedMixerTracks(f.d) == static_cast<int>(n0) - 2, "trim keeps used tracks");
}

void testPlaylist() {
  Fixture f;
  Song base = f.d.song();
  const Song& s = f.d.song();
  size_t nt = s.tracks.size();
  expect(ops::insertPlaylistTrack(f.d, 0) && s.tracks.size() == nt + 1 && s.clip(f.c1)->track == 1, "insert track shifts clips");
  expect(ops::movePlaylistTrack(f.d, 1, -1) && s.clip(f.c1)->track == 0, "move track moves clips");
  expect(ops::clonePlaylistTrack(f.d, 0) && s.playlist.size() == 4, "clone track copies clips");
  int onTrack1 = 0;
  for (const auto& c : s.playlist) onTrack1 += c.track == 1;
  expect(onTrack1 == 2, "cloned clips on the new track");
  expect(ops::deletePlaylistTrack(f.d, 1) && s.playlist.size() == 2, "delete track removes its clips");
  expect(ops::clearPlaylistTrack(f.d, 0) && s.playlist.empty(), "clear track");
  f.d.reset(base);
  auto dup = ops::duplicateClips(f.d, {f.c1, f.c2});
  expect(dup.size() == 2 && s.clip(dup[0])->start == 2 * s.barTicks(), "duplicate clips after the selection");
  f.d.reset(base);
  ObjectId np = ops::makeClipUnique(f.d, f.c2);
  expect(np && s.clip(f.c2)->pattern == np && s.clip(f.c1)->pattern == f.pat, "make unique");
  expect(*s.pattern(np) == [&] { Pattern p = *s.pattern(f.pat); p.id = np; p.name += " #2"; return p; }(), "unique pattern is a copy");
  expect(f.undoesToBaseline(base), "make unique undo");
  // Merge.
  ObjectId merged = ops::mergeClips(f.d, {f.c1, f.c2});
  expect(merged && s.playlist.size() == 1, "merge leaves one clip");
  const Clip* mc = s.clip(merged);
  const Pattern* mp = s.pattern(mc->pattern);
  expect(mc->start == 0 && mc->length == 2 * s.barTicks() && mp->bars == 2, "merged clip spans both");
  expect(mp->notes.at(f.lead).size() == 6, "merged notes from both clips");
  expect(mp->notes.at(f.kick).size() == 4 && mp->notes.at(f.kick)[2].start == s.barTicks(), "steps become notes");
  expect(f.undoesToBaseline(base), "merge undo");
  // Chop at pattern repeats.
  f.d.setClipLength(f.c1, 3 * s.barTicks());
  auto pieces = ops::chopClip(f.d, f.c1, s.barTicks());
  expect(pieces.size() == 3 && s.clip(pieces[2])->start == 2 * s.barTicks(), "chop into repeats");
  expect(ops::chopClip(f.d, pieces[0], s.barTicks() / 2).empty(), "chop refuses offsets inside the pattern");
  expect(ops::removeClips(f.d, {pieces[0], pieces[1]}) && s.playlist.size() == 2, "remove clips");
}

void testNoteTools() {
  using V = std::vector<Note>;
  V n = {{3, 10, 60, 100}, {50, 10, 64, 100}, {97, 10, 67, 100}};
  auto sel = allFalse(n.size());
  ops::quantize(n, sel, 48, false);
  expect(n[0].start == 0 && n[1].start == 48 && n[2].start == 96, "quantize starts");
  ops::legato(n, sel);
  expect(n[0].length == 48 && n[1].length == 48 && n[2].length == 10, "legato");
  V c = {{0, 96, 60, 100}};
  auto cs = allFalse(1);
  ops::chop(c, cs, 24);
  expect(c.size() == 4 && c[3].start == 72 && c[3].length == 24, "chop");
  ops::glue(c, cs);
  expect(c.size() == 1 && c[0].length == 96, "glue");
  V t = {{0, 10, 60, 100}, {10, 10, 70, 100}};
  std::vector<bool> ts = {false, true};
  ops::transpose(t, ts, 12);
  expect(t[0].pitch == 60 && t[1].pitch == 82, "transpose only selection");
  ops::flipVertical(t, ts);
  expect(t[1].pitch == 82, "flip single note stays");
  auto all = allFalse(2);
  ops::flipVertical(t, all);
  expect(t[0].pitch == 82 && t[1].pitch == 60, "flip vertical");
  ops::flipHorizontal(t, all);
  expect(t[0].start == 0 && t[0].pitch == 60 && t[1].start == 10, "flip horizontal");
  ops::scaleVelocity(t, all, 0.5);
  expect(t[0].velocity == 50, "scale velocity");
  V h = t;
  auto hs = allFalse(2);
  ops::humanize(h, hs, 10, 0, 42);
  V h2 = t;
  auto hs2 = allFalse(2);
  ops::humanize(h2, hs2, 10, 0, 42);
  expect(h == h2, "humanize is deterministic per seed");
  bool inRange = true;
  for (size_t i = 0; i < h.size(); ++i) inRange = inRange && std::abs(h[i].velocity - 50) <= 10;
  expect(inRange, "humanize within range");
  V chord = {{0, 96, 60, 100}, {0, 96, 64, 100}, {0, 96, 67, 100}};
  auto chs = allFalse(3);
  ops::strum(chord, chs, 6);
  expect(chord[0].start == 0 && chord[1].start == 6 && chord[2].start == 12 && chord[2].length == 84, "strum");
  V arp = {{0, 96, 60, 100}, {0, 96, 64, 100}, {0, 96, 67, 100}};
  auto as = allFalse(3);
  ops::arpeggiate(arp, as, 24);
  expect(arp.size() == 4 && arp[0].pitch == 60 && arp[1].pitch == 64 && arp[2].pitch == 67 && arp[3].pitch == 60 && arp[3].start == 72,
         "arpeggiate");
  V d = {{0, 24, 60, 100}, {24, 24, 62, 100}};
  std::vector<bool> ds = {true, false};
  ops::duplicate(d, ds, 24);
  expect(d.size() == 3 && std::count(ds.begin(), ds.end(), true) == 1, "duplicate selects only the copies");
  bool copySelected = false;
  for (size_t i = 0; i < d.size(); ++i) copySelected = copySelected || (ds[i] && d[i].pitch == 60 && d[i].start == 24);
  expect(copySelected, "duplicate selects the copy");
  ops::deleteNotes(d, ds);
  expect(d.size() == 2, "delete selection");
  // Selection helpers.
  V s4 = {{0, 10, 60, 100}, {5, 10, 60, 100}, {20, 10, 62, 100}, {30, 10, 64, 100}};
  std::vector<bool> ss;
  ops::selectOdd(s4, ss);
  expect(ss[0] && !ss[1] && ss[2] && !ss[3], "select every other");
  ops::invertSelection(ss);
  expect(!ss[0] && ss[1], "invert selection");
  ops::selectOverlapping(s4, ss);
  expect(ss[0] && ss[1] && !ss[2], "select overlapping");
  ss = {false, false, true, false};
  ops::selectSamePitch(s4, ss);
  expect(ss[2] && !ss[0], "select same pitch");
  ops::selectAll(s4, ss);
  expect(std::all_of(ss.begin(), ss.end(), [](bool b) { return b; }), "select all");
  ops::selectNone(ss);
  expect(std::none_of(ss.begin(), ss.end(), [](bool b) { return b; }), "select none");
}

void testProgressions() {
  const auto& ps = ops::progressions();
  expect(ps.size() >= 6, "progression list");
  auto n = ops::progressionNotes(ps[0], 60, 384, 4);
  expect(n.size() == 16, "four chords of bass + triad");
  // I V vi IV in C: C, G, A minor, F.
  bool firstIsC = n[0].pitch == 48 && n[1].pitch == 60 && n[2].pitch == 64 && n[3].pitch == 67;
  expect(firstIsC, "first chord is the tonic triad");
  bool sixth = false;
  for (const auto& x : n) sixth = sixth || (x.start == 2 * 384 && x.pitch == 72);  // A minor = A C E (C5 = 72)
  expect(sixth, "third chord is vi");
  auto r = ops::progressionRiff(ps[0], 60, 384, 48, 2);
  expect(r.size() == 16 && r[0].pitch == 60 && r[1].pitch == 64, "riff arpeggiates the chord");
  const ops::Progression* minor = nullptr;
  for (const auto& p : ps)
    if (p.minor) minor = &p;
  auto m = ops::progressionNotes(*minor, 57, 384, 1);
  expect(m[2].pitch == 60, "minor triad has a minor third");
}

}  // namespace

int main() {
  testPatterns();
  testSplitByChannel();
  testChannels();
  testRouting();
  testPlaylist();
  testNoteTools();
  testProgressions();
  return finish();
}

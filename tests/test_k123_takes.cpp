// Gridline - K-123 stage 3: loop recording into take lanes, comping and flatten.
// Takes are made directly (one constant level per pass, so what plays tells which take it is),
// committed as the recorder commits them, then comped, undone, saved, shared and flattened.
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/collab/LocalState.h"
#include "../src/core/Document.h"
#include "../src/core/Grouping.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioRecord.h"
#include "../src/engine/Render.h"
#include "../src/engine/TakeLanes.h"
#include "../src/engine/Sample.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;

namespace {

std::string gDir;
constexpr double kSr = 48000;

float levelOf(int take) { return 0.1f * static_cast<float>(take + 1); }

// Three loop passes of one bar on mixer track 1 (the last pass a beat short, as when stopped early).
Song recordedSong(TakeCommitResult* out = nullptr, bool lanes = true) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.mixer[1].input = 0;
  s.mixer[1].inputStereo = true;
  const Tick bar = s.barTicks();
  const double secPerTick = 60.0 / (s.tempo * s.ppq);
  std::vector<RecordedTake> takes;
  for (int p = 0; p < 3; ++p) {
    RecordedTake t;
    t.mixer = s.mixer[1].id;
    t.pass = p;
    t.startTick = static_cast<double>(bar);  // the loop: bar 2
    const auto n = static_cast<size_t>(std::llround(static_cast<double>(bar) * secPerTick * kSr * (p == 2 ? 0.75 : 1.0)));
    t.L.assign(n, levelOf(p));
    t.R.assign(n, levelOf(p));
    takes.push_back(std::move(t));
  }
  TakeCommitOptions o;
  o.dir = gDir + "/Recorded";
  o.sampleRate = kSr;
  o.lanes = lanes;
  TakeCommitResult r = commitTakes(s, takes, o);
  if (out) *out = r;
  return s;
}

void testLanesOnOneTrack() {
  TakeCommitResult r;
  Song s = recordedSong(&r);
  expect(r.clips.size() == 3, "three takes placed");
  if (r.clips.size() != 3) return;
  const Clip* c0 = s.clip(r.clips[0]);
  const int tr = c0->track;
  bool same = true;
  for (ObjectId id : r.clips) same = same && s.clip(id)->track == tr && s.clip(id)->takeGroup == c0->takeGroup;
  expect(same, "every pass lands on the recorded track, one take group");
  expect(s.tracks[static_cast<size_t>(tr)].takeLanes && takes::lanesShown(s, tr), "the track shows its take lanes");
  expect(takes::laneCount(s, tr) == 3, "one lane per take");
  expect(s.tracks[static_cast<size_t>(tr)].name == "Insert 1", "the track is named after the insert, not one take");
  expect(takes::playingTake(s, tr, s.barTicks() + 10) == 2, "the newest take plays");
  // The earlier layout stays available.
  TakeCommitResult r2;
  Song old = recordedSong(&r2, false);
  expect(r2.clips.size() == 3 && old.clip(r2.clips[1])->track == old.clip(r2.clips[0])->track + 1 && !old.tracks[0].takeLanes,
         "lanes off: passes stack on the tracks below");
  // The lanes move together: selecting one take selects the group.
  const auto sel = ops::expandClipGroups(s, {r.clips[1]});
  expect(sel.size() == 3, "a click on one take selects every take of the lanes");
}

void testCompSwipe() {
  TakeCommitResult r;
  Song s = recordedSong(&r);
  const int tr = s.clip(r.clips[0])->track;
  const Tick bar = s.barTicks(), beat = s.beatTicks();
  // Swipe take 1 (index 0) over beat 2 and take 2 over beat 4 (take 3 is short: it has no audio there).
  expect(takes::compRange(s, tr, 0, bar + beat, bar + 2 * beat), "swipe: take 1 over beat 2");
  expect(takes::playingTake(s, tr, bar + beat + 5) == 0, "beat 2 plays take 1");
  expect(takes::playingTake(s, tr, bar + 5) == 2 && takes::playingTake(s, tr, bar + 2 * beat + 5) == 2, "around it take 3 stays");
  // Take 3 ends after three beats: a swipe of take 3 past its end leaves the comp there alone.
  expect(takes::compRange(s, tr, 1, bar + 3 * beat, bar + 4 * beat), "take 2 over beat 4");
  expect(takes::playingTake(s, tr, bar + 3 * beat + 5) == 1, "beat 4 plays take 2");
  expect(!takes::compRange(s, tr, 2, bar + 3 * beat, bar + 4 * beat), "take 3 has nothing in beat 4: no change");
  expect(takes::playingTake(s, tr, bar + 3 * beat + 5) == 1, "beat 4 still plays take 2");
  // Exactly one take plays at every moment of the loop.
  bool one = true;
  for (Tick t = bar; t < 2 * bar; t += beat / 4) {
    int n = 0;
    for (const Clip& c : s.playlist)
      if (c.takeGroup != kNoId && !c.muted && t >= c.start && t < c.start + s.clipLength(c)) ++n;
    one = one && n == 1;
  }
  expect(one, "one take plays at a time");
  // A click picks a take for the section under the pointer (the piece playing there).
  expect(takes::compPick(s, tr, 2, bar + beat + 5), "click: take 3 for beat 2's section");
  expect(takes::playingTake(s, tr, bar + beat + 5) == 2, "the section plays take 3 again");
  // Pieces of one take that meet again are joined, so comps do not leave a trail of clips.
  int take3 = 0;
  for (const Clip& c : s.playlist)
    if (c.takeGroup != kNoId && c.take == 2 && !c.muted) ++take3;
  expect(take3 == 1, "take 3's playing pieces joined into one clip");
  const std::vector<char> in = takes::takesInComp(s, tr);
  expect(in.size() == 3 && !in[0] && in[1] && in[2], "takes in the comp listed");
}

void testUndoSaveShare() {
  TakeCommitResult r;
  Document doc;
  doc.reset(recordedSong(&r));
  const int tr = doc.song().clip(r.clips[0])->track;
  const Tick bar = doc.song().barTicks(), beat = doc.song().beatTicks();
  const std::vector<Clip> before = doc.song().playlist;
  doc.editSong("comp takes", [&](Song& x) { takes::compRange(x, tr, 0, bar, bar + beat); });
  expect(takes::playingTake(doc.song(), tr, bar + 5) == 0, "comp applied");
  doc.undo();
  expect(doc.song().playlist == before, "one undo puts the comp back");
  doc.redo();
  expect(takes::playingTake(doc.song(), tr, bar + 5) == 0, "redo");
  // Saved: lanes, takes and the comp come back exactly.
  Song back;
  std::string err;
  expect(songFromJson(songToJson(doc.song()), back, &err), "save and load");
  expect(back.playlist == doc.song().playlist && back.tracks[static_cast<size_t>(tr)].takeLanes, "takes, comp and lanes saved losslessly");
  // Shared: what a collaborator receives keeps the takes and the comp (song data). K-314 B: showing the
  // take lanes is view state, each person's own (as folding), so it is not sent.
  Json j = songToJson(doc.song(), false);
  collab::maskSessionJson(j);
  Song peer;
  expect(songFromJson(j, peer, &err), "collab copy loads");
  expect(peer.playlist == doc.song().playlist, "collab: takes and comp are shared");
  expect(!peer.tracks[static_cast<size_t>(tr)].takeLanes, "collab: the take lanes shown stay this person's own");
}

void testRemoveUnusedAndGather() {
  TakeCommitResult r;
  Song s = recordedSong(&r);
  const int tr = s.clip(r.clips[0])->track;
  const ObjectId g = s.clip(r.clips[0])->takeGroup;
  const Tick bar = s.barTicks(), beat = s.beatTicks();
  takes::compRange(s, tr, 0, bar, bar + beat);
  const int removed = takes::removeUnused(s, g);
  expect(removed > 0, "unused takes removed");
  bool allPlay = true;
  for (const Clip& c : s.playlist)
    if (c.takeGroup == g) allPlay = allPlay && !c.muted;
  expect(allPlay && takes::playingTake(s, tr, bar + 5) == 0 && takes::playingTake(s, tr, bar + beat + 5) == 2, "the comp is kept");
  // An old project: the takes stacked on three tracks gather into lanes when asked.
  TakeCommitResult r2;
  Song old = recordedSong(&r2, false);
  const ObjectId g2 = old.clip(r2.clips[0])->takeGroup;
  expect(takes::spreadOverTracks(old, g2), "old layout: spread over tracks");
  expect(takes::gatherIntoLanes(old, g2) == 2 && !takes::spreadOverTracks(old, g2), "gathered onto the first track");
  expect(takes::lanesShown(old, old.clip(r2.clips[0])->track), "and shown as lanes");
}

void testFlatten() {
  TakeCommitResult r;
  Song s = recordedSong(&r);
  const int tr = s.clip(r.clips[0])->track;
  const ObjectId g = s.clip(r.clips[0])->takeGroup;
  const Tick bar = s.barTicks(), beat = s.beatTicks();
  takes::compRange(s, tr, 0, bar + beat, bar + 2 * beat);
  takes::compRange(s, tr, 1, bar + 3 * beat, bar + 4 * beat);
  std::string err;
  const ObjectId id = takes::flatten(s, g, gDir + "/Recorded", &err);
  expect(id != kNoId, ("flatten: " + err).c_str());
  if (id == kNoId) return;
  const Clip* c = s.clip(id);
  bool gone = true;
  for (const Clip& k : s.playlist) gone = gone && k.takeGroup != g;
  expect(gone && c->track == tr && c->start == bar && c->length == bar && c->takeGroup == kNoId, "one clip over the comp, the takes gone");
  const Channel* ch = s.channel(c->source);
  expect(ch && ch->mixerTrack == s.mixer[1].id && ch->name == "Insert 1 comp", "plays through the takes' mixer track");
  SamplePtr smp = ch ? sampleCache().get(ch->plugin.data) : nullptr;
  expect(smp && smp->frames() > 0, "the comp file reads back");
  if (!smp) return;
  const double fpb = 60.0 / s.tempo * kSr;  // frames per beat
  auto at = [&](double beats) { return smp->left[static_cast<size_t>(beats * fpb)]; };
  expect(std::fabs(at(0.5) - levelOf(2)) < 1e-4f && std::fabs(at(1.5) - levelOf(0)) < 1e-4f && std::fabs(at(2.5) - levelOf(2)) < 1e-4f &&
             std::fabs(at(3.5) - levelOf(1)) < 1e-4f,
         "each beat holds the take the comp picked");
  // At a join the two takes cross over (no step from one level to the other).
  const auto j = static_cast<size_t>(fpb);
  const float mid = smp->left[j];
  expect(mid > levelOf(0) + 0.01f && mid < levelOf(2) - 0.01f, "a short crossfade at the join");
  // Undoable as one step.
  Document doc;
  doc.reset(recordedSong(&r));
  const std::vector<Clip> before = doc.song().playlist;
  doc.editSong("flatten comp", [&](Song& x) { takes::flatten(x, x.clip(r.clips[0])->takeGroup, gDir + "/Recorded"); });
  doc.undo();
  expect(doc.song().playlist == before, "flatten undoes in one step");
}

// The comp plays what the lanes show: a render hears take 1 in beat 2 and take 3 around it.
void testCompPlays() {
  TakeCommitResult r;
  Song s = recordedSong(&r);
  const int tr = s.clip(r.clips[0])->track;
  const Tick bar = s.barTicks(), beat = s.beatTicks();
  takes::compRange(s, tr, 0, bar + beat, bar + 2 * beat);
  RenderOptions o;
  o.sampleRate = static_cast<int>(kSr);
  AudioBuffer b = renderSong(s, o);
  const double fpb = 60.0 / s.tempo * kSr;
  auto lvl = [&](double beats) {
    const auto i = static_cast<size_t>(beats * fpb);
    return i < b.frames ? std::fabs(b.at(0, i)) : 0.0f;
  };
  const float a = lvl(4.5), m = lvl(5.5), c = lvl(6.5);
  expect(a > 0 && m > 0 && std::fabs(a - c) < 1e-3f * std::max(1.0f, a), "around the swipe the same take plays");
  expect(std::fabs(m / a - levelOf(0) / levelOf(2)) < 0.02f, "in the swipe take 1 plays");
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k123_takes");
  testLanesOnOneTrack();
  testCompSwipe();
  testUndoSaveShare();
  testRemoveUnusedAndGather();
  testFlatten();
  testCompPlays();
  return finish();
}

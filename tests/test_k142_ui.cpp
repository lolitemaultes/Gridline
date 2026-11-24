// Gridline - K-142 (0.7.8), the interface slice of the CPU sweep: the parts of its fixes that can
// be checked without a window.
//  * idlepace::planWait: shown, the idle frames as before; nothing shown, no frames while idle,
//    only a changed project's housekeeping (every 5 s) and the CPU report's steps (drawing nothing);
//    the sleeps between looks at what changes without an event: a refresh shown; hidden, none but
//    the watchdog's beat (5 s), or 0.25 s with a game controller attached.
//  * trackerorder::ofDoc: the Tracker's cached order list is exactly trk::orderList after every
//    kind of change (edit, undo, redo, another project), a list held stays as it was, and another
//    song is never served from the cache.
//  * trackmodes::playedMixers: the same mixer tracks in the same order as the algorithm before the
//    fix (which looked at every clip's pattern again), on the demo song and on it grown large.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "../src/app/IdlePace.h"
#include "../src/app/TrackerOrder.h"
#include "../src/core/Demo.h"
#include "../src/core/Document.h"
#include "../src/core/TrackModes.h"
#include "../src/core/TrackerGrid.h"
#include "Check.h"

using namespace gl;

namespace {

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

void testPlanWait() {
  idlepace::IdlePace p;
  p.active(0.0);
  p.framed(0.0);
  // Shown and busy (within a second of activity): no wait.
  idlepace::Wait w = idlepace::planWait(p, 0.5, true, false, 0.0, 1e300);
  expect(w.left == 0 && !w.reportOnly, "shown, active: the next frame at once");
  // Shown and idle: the idle frame interval, a refresh's sleep at a time.
  p.framed(2.0);
  w = idlepace::planWait(p, 2.03, true, false, 2.0, 1e300);
  expect(near(w.left, 0.07) && near(w.nap, idlepace::kShownPoll), "shown, idle: waits for the next idle frame a refresh at a time");
  w = idlepace::planWait(p, 2.2, true, false, 2.0, 1e300);
  expect(w.left == 0, "shown, idle: draws when the idle frame is due");
  // Long idle: 4 frames a second; an open plugin window keeps 10.
  p.framed(6.0);
  w = idlepace::planWait(p, 6.05, true, false, 6.0, 1e300);
  expect(near(w.left, 0.2), "shown, idle over 5 s: the next frame a quarter second after the last");
  expect(near(w.nap, idlepace::kDeepPoll), "shown, idle over 5 s: looks twenty times a second meanwhile");
  p.setDeep(false);
  w = idlepace::planWait(p, 6.05, true, false, 6.0, 1e300);
  expect(near(w.left, 0.05), "shown, idle over 5 s with a plugin window open: still 10 a second");
  expect(near(w.nap, idlepace::kShownPoll), "shown, idle with a plugin window open: looks every refresh");
  p.setDeep(true);
  p.active(6.0);
  w = idlepace::planWait(p, 6.5, true, false, 6.0, 1e300);
  expect(w.left == 0, "activity again: full rate at once");
  p.active(0.0);
  // Nothing shown, the project unchanged: no frame however long (and the active second does not draw either).
  w = idlepace::planWait(p, 1000.0, false, false, 2.0, 1e300);
  expect(w.left > 1e299 && near(w.nap, idlepace::kHiddenBeat) && !w.reportOnly, "hidden, unchanged project: no frames, no looks (only the watchdog's beat)");
  w = idlepace::planWait(p, 1000.0, false, false, 2.0, 1e300, true);
  expect(w.left > 1e299 && near(w.nap, idlepace::kHiddenPoll), "hidden with a game controller attached: looks four times a second");
  idlepace::IdlePace busy;
  busy.active(10.0);
  w = idlepace::planWait(busy, 10.1, false, false, 10.0, 1e300);
  expect(w.left > 1e299, "hidden right after activity: still no frame (only events and what changes on its own draw)");
  // Nothing shown, the project changed: one frame every kHiddenHousekeeping seconds (backups, autosave).
  w = idlepace::planWait(p, 4.0, false, true, 2.0, 1e300);
  expect(near(w.left, 3.0) && !w.reportOnly, "hidden, changed project: waits for the housekeeping frame");
  w = idlepace::planWait(p, 7.5, false, true, 2.0, 1e300);
  expect(w.left == 0 && !w.reportOnly, "hidden, changed project: the housekeeping frame is drawn when due");
  // The CPU report's next step: a turn of the loop that draws nothing.
  w = idlepace::planWait(p, 50.0, false, false, 2.0, 0.0);
  expect(w.left == 0 && w.reportOnly, "hidden: the report's step is a turn without a frame");
  w = idlepace::planWait(p, 50.0, false, false, 2.0, 0.1);
  expect(near(w.left, 0.1) && near(w.nap, 0.1), "hidden: the wait ends at the report's step");
  w = idlepace::planWait(p, 7.5, false, true, 2.0, 0.0);
  expect(w.left == 0 && !w.reportOnly, "hidden: housekeeping and the report due together draw the frame");
  // Pacing off (scripted runs): never waits.
  idlepace::IdlePace off;
  off.setEnabled(false);
  w = idlepace::planWait(off, 100.0, false, false, 0.0, 1e300);
  expect(w.left == 0, "pacing off: no wait, shown or not");
}

bool sameOrder(const std::vector<trk::OrderEntry>& a, const std::vector<trk::OrderEntry>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].pattern != b[i].pattern || a[i].patterns != b[i].patterns ||
        a[i].clips != b[i].clips || a[i].audio != b[i].audio)
      return false;
  return true;
}

void testOrderCache() {
  Document doc(makeDemoTrack());
  expect(!doc.song().playlist.empty(), "demo song has clips");
  trackerorder::List first = trackerorder::ofDoc(doc, doc.song());
  expect(sameOrder(*first, trk::orderList(doc.song())), "order list: as trk::orderList");
  expect(trackerorder::ofDoc(doc, doc.song()) == first, "order list: the same list while the song is unchanged");
  const std::vector<trk::OrderEntry> before = *first;
  // An edit: a clip moved far along the song.
  const ObjectId moved = doc.song().playlist.front().id;
  doc.editClip(moved, "move", [](Clip& c) { c.start += 1000000; });
  trackerorder::List after = trackerorder::ofDoc(doc, doc.song());
  expect(after != first && sameOrder(*after, trk::orderList(doc.song())), "order list: made again after an edit");
  expect(sameOrder(*first, before), "order list: a list held stays as it was");
  doc.undo();
  expect(sameOrder(*trackerorder::ofDoc(doc, doc.song()), trk::orderList(doc.song())) && sameOrder(*trackerorder::ofDoc(doc, doc.song()), before),
         "order list: after undo");
  doc.redo();
  expect(sameOrder(*trackerorder::ofDoc(doc, doc.song()), trk::orderList(doc.song())), "order list: after redo");
  // A clip added (a new entry at the end).
  const Clip& c0 = doc.song().playlist.front();
  doc.addClip(c0.pattern, c0.track, doc.song().lengthTicks() + 384, 0);
  expect(sameOrder(*trackerorder::ofDoc(doc, doc.song()), trk::orderList(doc.song())), "order list: after a clip is added");
  // Another project.
  Song empty;
  doc.reset(empty);
  expect(trackerorder::ofDoc(doc, doc.song())->empty() && trk::orderList(doc.song()).empty(), "order list: another (empty) project");
  doc.reset(makeDemoTrack());
  expect(sameOrder(*trackerorder::ofDoc(doc, doc.song()), before), "order list: the demo again");
  // Another song (a copy being edited) is worked out, not served from the cache.
  Song copy = doc.song();
  copy.playlist.erase(copy.playlist.begin());
  expect(sameOrder(*trackerorder::ofDoc(doc, copy), trk::orderList(copy)) && !sameOrder(*trackerorder::ofDoc(doc, copy), before),
         "order list: another song is its own");
  expect(sameOrder(*trackerorder::ofDoc(doc, doc.song()), before), "order list: the document's again after another song");
  // Another document with the same revision count.
  Document other(makeDemoTrack());
  other.editClip(other.song().playlist.front().id, "move", [](Clip& c) { c.start += 1000000; });
  expect(sameOrder(*trackerorder::ofDoc(other, other.song()), trk::orderList(other.song())), "order list: another document");
}

// The algorithm before the fix, kept here as the reference.
std::vector<ObjectId> playedMixersBefore(const Song& s, int track) {
  std::vector<ObjectId> out;
  if (const MixerTrack* m = trackmodes::linkedMixer(s, track)) return {m->id};
  auto take = [&](ObjectId channel) {
    const Channel* c = s.channel(channel);
    if (!c) return;
    const ObjectId m = c->mixerTrack != kNoId ? c->mixerTrack : s.masterId();
    if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  };
  for (const Clip& c : s.playlist) {
    if (c.track != track) continue;
    if (c.kind == ClipKind::Audio) {
      take(c.source);
    } else if (c.kind == ClipKind::Pattern) {
      const Pattern* p = s.pattern(c.pattern);
      if (!p) continue;
      for (const auto& [ch, v] : p->steps)
        if (std::any_of(v.begin(), v.end(), [](uint8_t x) { return x != 0; })) take(ch);
      for (const auto& [ch, v] : p->notes)
        if (!v.empty()) take(ch);
    }
  }
  return out;
}

// The demo grown as the CPU sweep's large project (tools/cpu/cpurun.py make_large): its channels
// copied (with their notes in every pattern, on their own mixer tracks for every third copy) and
// its arrangement repeated.
Song grown(int channels, int clips) {
  Song s = makeDemoTrack();
  const std::vector<Channel> base = s.channels;
  for (size_t k = 0; static_cast<int>(s.channels.size()) < channels; ++k) {
    Channel c = base[k % base.size()];
    const ObjectId src = c.id;
    c.id = s.allocateId();
    if (k % 3 == 0 && s.mixer.size() > 2) c.mixerTrack = s.mixer[1 + k % (s.mixer.size() - 1)].id;
    for (Pattern& p : s.patterns) {
      if (auto it = p.notes.find(src); it != p.notes.end()) p.notes[c.id] = it->second;
      if (auto it = p.steps.find(src); it != p.steps.end()) p.steps[c.id] = it->second;
    }
    s.channels.push_back(c);
  }
  const std::vector<Clip> pl = s.playlist;
  Tick span = 0;
  for (const Clip& c : pl) span = std::max(span, c.start + s.clipLength(c));
  for (int rep = 1; static_cast<int>(s.playlist.size()) < clips; ++rep)
    for (const Clip& c : pl) {
      if (static_cast<int>(s.playlist.size()) >= clips) break;
      Clip n = c;
      n.id = s.allocateId();
      n.start = c.start + rep * span;
      s.playlist.push_back(n);
    }
  return s;
}

void testPlayedMixers() {
  for (int big = 0; big < 2; ++big) {
    const Song s = big ? grown(200, 2000) : makeDemoTrack();
    int same = 0, tracks = 0, several = 0;
    for (int t = -1; t <= static_cast<int>(s.tracks.size()); ++t) {
      const std::vector<ObjectId> now = trackmodes::playedMixers(s, t), was = playedMixersBefore(s, t);
      ++tracks;
      if (now == was) ++same;
      if (now.size() > 1) ++several;
    }
    char name[160];
    std::snprintf(name, sizeof name, "playedMixers: %s song, %d of %d tracks the same as before (%d with several)", big ? "large" : "demo", same, tracks,
                  several);
    expect(same == tracks, name);
    if (big) expect(several > 0, "playedMixers: the large song has tracks playing into several mixer tracks (order matters)");
  }
}

}  // namespace

int main() {
  testPlanWait();
  testOrderCache();
  testPlayedMixers();
  return finish();
}

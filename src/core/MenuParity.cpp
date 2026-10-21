// Gridline — J-27 menu parity operations (see MenuParity.h).
#include "MenuParity.h"
#include "ClipSources.h"
#include "ChordTools.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "Automation.h"
#include "NoteTools.h"
#include "PatternAutomation.h"
#include "PluginParams.h"

namespace gl::ops {

namespace {

// Indices of the notes a tool acts on: the selection, or all when none.
std::vector<size_t> affected(const std::vector<Note>& notes, const NoteSel& sel) {
  std::vector<size_t> out;
  bool any = false;
  for (size_t i = 0; i < notes.size() && i < sel.size(); ++i) any = any || sel[i];
  for (size_t i = 0; i < notes.size(); ++i)
    if (!any || (i < sel.size() && sel[i])) out.push_back(i);
  return out;
}

Rgb blend(Rgb a, Rgb b, double t) {
  auto ch = [&](int shift) {
    double x = ((a >> shift) & 0xFF) * (1.0 - t) + ((b >> shift) & 0xFF) * t;
    return static_cast<Rgb>(std::clamp(static_cast<int>(std::lround(x)), 0, 255)) << shift;
  };
  return ch(16) | ch(8) | ch(0);
}

bool isSampler(const Channel& c) {
  return c.plugin.type == "gl.sampler" && c.plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode);
}

}  // namespace

// ------------------------------------------------------- playlist tracks ----

int setAllTracksLocked(Document& d, bool locked) {
  int n = 0;
  for (const auto& t : d.song().tracks) n += t.locked != locked;
  if (!n) return 0;
  d.editSong(locked ? "lock all playlist tracks" : "unlock all playlist tracks", [locked](Song& s) {
    for (auto& t : s.tracks) t.locked = locked;
  });
  return n;
}

int setAllGroupsFolded(Document& d, bool folded) {
  int n = 0;
  for (const auto& t : d.song().tracks) n += t.groupSize > 0 && t.folded != folded;
  if (!n) return 0;
  d.editSong(folded ? "collapse track groups" : "expand track groups", [folded](Song& s) {
    for (auto& t : s.tracks)
      if (t.groupSize > 0) t.folded = folded;
  });
  return n;
}

std::pair<int, int> trackGroupOf(const Song& s, int track) {
  const int n = static_cast<int>(s.tracks.size());
  if (track < 0 || track >= n) return {-1, 0};
  for (int h = track; h >= 0; --h) {
    const int size = s.tracks[static_cast<size_t>(h)].groupSize;
    if (size > 0 && h + size >= track) return {h, std::min(size, n - 1 - h)};
  }
  return {-1, 0};
}

bool autoColorTrackGroup(Document& d, int track) {
  const auto group = trackGroupOf(d.song(), track);
  const int head = group.first, size = group.second;
  if (head < 0 || size <= 0) return false;
  const Rgb from = d.song().tracks[static_cast<size_t>(head)].color;
  const Rgb to = blend(from, 0x000000, 0.55);
  d.editSong("auto colour track group", [&](Song& s) {
    for (int k = 0; k <= size; ++k) s.tracks[static_cast<size_t>(head + k)].color = blend(from, to, static_cast<double>(k) / size);
  });
  return true;
}

int sendPatternClipsToTrack(Document& d, ObjectId pattern, int track) {
  const Song& s = d.song();
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return 0;
  int n = 0;
  for (const auto& c : s.playlist) n += c.kind == ClipKind::Pattern && c.pattern == pattern && c.track != track;
  if (!n) return 0;
  d.editClips("send pattern clips to track", [&](std::vector<Clip>& cs) {
    for (auto& c : cs)
      if (c.kind == ClipKind::Pattern && c.pattern == pattern) c.track = track;
  });
  return n;
}

int firstEmptyTrack(const Song& s) {
  std::vector<bool> used(s.tracks.size(), false);
  for (const auto& c : s.playlist)
    if (c.track >= 0 && c.track < static_cast<int>(used.size())) used[static_cast<size_t>(c.track)] = true;
  for (size_t i = 0; i < used.size(); ++i)
    if (!used[i]) return static_cast<int>(i);
  return -1;
}

// ------------------------------------------------------------ audio clips ----

int pitchAudioClips(Document& d, const std::set<ObjectId>& clips, float cents) {
  int n = 0;
  for (const auto& c : d.song().playlist) n += c.kind == ClipKind::Audio && clips.count(c.id);
  if (!n || cents == 0.0f) return 0;
  d.editSong("pitch audio clips", [&](Song& s) {
    for (auto& c : s.playlist)
      if (c.kind == ClipKind::Audio && clips.count(c.id)) c.pitch = std::clamp(c.pitch + cents, -4800.0f, 4800.0f);
  });
  return n;
}

int reverseAudioClips(Document& d, const std::set<ObjectId>& clips) {
  int n = 0;
  for (const auto& c : d.song().playlist) n += c.kind == ClipKind::Audio && clips.count(c.id);
  if (!n) return 0;
  d.editSong("reverse audio clips", [&](Song& s) {
    for (auto& c : s.playlist)
      if (c.kind == ClipKind::Audio && clips.count(c.id)) c.reverse = !c.reverse;
  });
  return n;
}

ObjectId makeAudioClipUnique(Document& d, ObjectId clip) {
  const Song& s0 = d.song();
  const Clip* c = s0.clip(clip);
  if (!c || c->kind != ClipKind::Audio) return kNoId;
  const int idx = s0.channelIndex(c->source);
  if (idx < 0) return kNoId;
  ObjectId nid = kNoId;
  d.editSong("make audio clip unique", [&](Song& s) {
    Channel copy = s.channels[static_cast<size_t>(idx)];
    nid = s.allocateId();
    copy.id = nid;
    copy.name += " #2";
    copy.layer.clear();
    s.channels.insert(s.channels.begin() + idx + 1, copy);
    if (Clip* k = s.clip(clip)) k->source = nid;
  });
  return nid;
}

int setAudioClipTimeMode(Document& d, const std::set<ObjectId>& clips, int mode) {
  const Song& s = d.song();
  std::set<ObjectId> chans;
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Audio && (clips.empty() || clips.count(c.id)))
      if (const Channel* ch = s.channel(c.source); ch && isSampler(*ch) && ch->plugin.params[pp::SmpStretchMode] != mode)
        chans.insert(ch->id);
  if (chans.empty()) return 0;
  d.editSong(mode ? "switch audio clips to stretch" : "switch audio clips to resample", [&](Song& x) {
    for (auto& ch : x.channels)
      if (chans.count(ch.id)) ch.plugin.params[pp::SmpStretchMode] = static_cast<float>(mode);
  });
  return static_cast<int>(chans.size());
}

int setSamplerDeclick(Document& d, const std::vector<ObjectId>& channels, int mode) {
  mode = std::clamp(mode, 0, 4);
  std::set<ObjectId> chans;
  for (ObjectId id : channels)
    if (const Channel* ch = d.song().channel(id); ch && isSampler(*ch) && ch->plugin.params[pp::SmpDeclick] != mode)
      chans.insert(id);
  if (chans.empty()) return 0;
  d.editSong("change declicking", [&](Song& x) {
    for (auto& ch : x.channels)
      if (chans.count(ch.id)) ch.plugin.params[pp::SmpDeclick] = static_cast<float>(mode);
  });
  return static_cast<int>(chans.size());
}

// ------------------------------------------------------------- automation ----

namespace {
// Points of `ac` visible through a placement [offset, offset + len), at
// placement-relative positions, in parameter-normalized values; with values
// at both edges so the merged curve keeps the clip's shape there.
std::vector<AutoPoint> visiblePoints(const AutomationClip& ac, Tick offset, Tick len) {
  std::vector<AutoPoint> out;
  auto norm = [&](double v) { return static_cast<float>(ac.min + v * (ac.max - ac.min)); };
  if (ac.points.empty() || len <= 0) return out;
  auto contentValue = [&](Tick t) {
    if (ac.loop > 0) t %= ac.loop;
    return automationValue(ac, static_cast<double>(t));
  };
  out.push_back(AutoPoint{0, norm(contentValue(offset)), 0, AutoShape::Curve});
  const Tick cycle = ac.loop > 0 ? ac.loop : 0;
  const Tick k0 = cycle ? offset / cycle : 0, k1 = cycle ? (offset + len) / cycle : 0;
  for (Tick k = k0; k <= k1; ++k)
    for (const auto& p : ac.points) {
      const Tick at = p.pos + k * cycle;
      if (at <= offset || at >= offset + len) continue;
      AutoPoint q = p;
      q.pos = at - offset;
      q.value = norm(p.value);
      out.push_back(q);
    }
  AutoPoint last{len, norm(contentValue(offset + len - 1)), 0, AutoShape::Curve};
  out.push_back(last);
  return out;
}
}  // namespace

ObjectId mergeAutomationClips(Document& d, const std::set<ObjectId>& clips) {
  const Song& s = d.song();
  std::vector<const Clip*> list;
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Automation && clips.count(c.id) && s.automation(c.source)) list.push_back(&c);
  if (list.empty()) return kNoId;
  std::sort(list.begin(), list.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
  const AutomationClip* first = s.automation(list.front()->source);
  std::vector<const Clip*> same;
  for (const Clip* c : list) {
    const AutomationClip* ac = s.automation(c->source);
    if (ac->owner == first->owner && ac->index == first->index) same.push_back(c);
  }
  if (same.size() < 2) return kNoId;
  const Tick start = same.front()->start;
  Tick end = start;
  for (const Clip* c : same) end = std::max(end, c->start + s.clipLength(*c));
  AutomationClip merged;
  merged.name = first->name;
  merged.color = first->color;
  merged.owner = first->owner;
  merged.index = first->index;
  merged.min = 0.0f;
  merged.max = 1.0f;
  merged.flCurves = first->flCurves;
  for (const Clip* c : same) {
    const AutomationClip* ac = s.automation(c->source);
    for (auto p : visiblePoints(*ac, c->offset, s.clipLength(*c))) {
      p.pos += c->start - start;
      // Later clips win where placements overlap.
      merged.points.erase(std::remove_if(merged.points.begin(), merged.points.end(),
                                         [&](const AutoPoint& q) { return q.pos == p.pos; }),
                          merged.points.end());
      merged.points.push_back(p);
    }
  }
  std::stable_sort(merged.points.begin(), merged.points.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.pos < b.pos; });
  const int track = same.front()->track;
  std::set<ObjectId> drop;
  for (const Clip* c : same) drop.insert(c->id);
  ObjectId newClip = kNoId;
  d.editSong("merge automation clips", [&](Song& x) {
    merged.id = x.allocateId();
    x.automations.push_back(merged);
    x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(), [&](const Clip& c) { return drop.count(c.id) > 0; }),
                     x.playlist.end());
    Clip k;
    k.id = newClip = x.allocateId();
    k.kind = ClipKind::Automation;
    k.source = merged.id;
    k.track = track;
    k.start = start;
    k.length = end - start;
    x.playlist.push_back(k);
    // Contents no placement uses any more go too.
    x.automations.erase(std::remove_if(x.automations.begin(), x.automations.end(),
                                       [&](const AutomationClip& a) {
                                         for (const auto& c : x.playlist)
                                           if (c.kind == ClipKind::Automation && c.source == a.id) return false;
                                         return true;
                                       }),
                        x.automations.end());
  });
  return newClip;
}

ObjectId laneToAutomationClip(Document& d, ObjectId pattern, ObjectId owner, uint32_t index) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return kNoId;
  const PatternAutomation* lane = findPatternLane(*p, owner, index);
  if (!lane || lane->points.empty()) return kNoId;
  std::vector<Clip> places;
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Pattern && c.pattern == pattern) places.push_back(c);
  int track = firstEmptyTrack(s);
  if (track < 0) track = static_cast<int>(s.tracks.size()) - 1;
  AutomationClip ac;
  ac.flCurves = false;  // from pattern automation: same curves (J-41)
  ac.name = paramDisplayName(s, ParamId{owner, index});
  ac.color = p->color;
  ac.owner = owner;
  ac.index = index;
  ac.points = lane->points;
  const Tick patLen = std::max<Tick>(1, s.patternContentLength(*p));
  ObjectId id = kNoId;
  d.editSong("turn events into automation clip", [&](Song& x) {
    ac.id = id = x.allocateId();
    ac.loop = patLen;  // the events repeat with the pattern
    x.automations.push_back(ac);
    if (places.empty()) {
      Clip k;
      k.kind = ClipKind::Automation;
      k.source = id;
      k.id = x.allocateId();
      k.track = track;
      k.length = patLen;
      x.playlist.push_back(k);
    }
    for (const Clip& pc : places) {
      Clip k;
      k.kind = ClipKind::Automation;
      k.source = id;
      k.id = x.allocateId();
      k.track = track;
      k.start = pc.start;
      k.offset = pc.offset;
      k.length = x.clipLength(pc);
      x.playlist.push_back(k);
    }
    if (Pattern* q = x.pattern(pattern)) setPatternLane(*q, owner, index, {});
  });
  return id;
}

std::vector<ObjectId> unusedAutomationClips(const Song& s) {
  return unusedClipSources(s, ClipKind::Automation);  // J-82: clips in other arrangements count too
}

int deleteUnusedAutomationClips(Document& d) {
  auto unused = unusedAutomationClips(d.song());
  if (unused.empty()) return 0;
  std::set<ObjectId> drop(unused.begin(), unused.end());
  d.editSong("delete unused automation clips", [&](Song& s) {
    s.automations.erase(std::remove_if(s.automations.begin(), s.automations.end(),
                                       [&](const AutomationClip& a) { return drop.count(a.id) > 0; }),
                        s.automations.end());
  });
  return static_cast<int>(unused.size());
}

// ------------------------------------------------------------------ notes ----

void keepTimeSlot(std::vector<Note>& notes, NoteSel& sel, int part, int parts, Tick period, bool remove) {
  if (parts < 1 || part < 1 || part > parts || period <= 0) return;
  sel.resize(notes.size(), false);
  const auto idx = affected(notes, sel);
  std::vector<bool> drop(notes.size(), false);
  for (size_t i : idx) {
    const Tick inPeriod = ((notes[i].start % period) + period) % period;
    const int slot = static_cast<int>(inPeriod * parts / period) + 1;
    drop[i] = remove ? slot == part : slot != part;
  }
  std::vector<Note> keep;
  NoteSel ks;
  for (size_t i = 0; i < notes.size(); ++i)
    if (!drop[i]) keep.push_back(notes[i]), ks.push_back(sel[i]);
  notes.swap(keep);
  sel.swap(ks);
}

void invertChords(std::vector<Note>& notes, NoteSel& sel, int dir) {
  if (dir == 0) return;
  sel.resize(notes.size(), false);
  std::map<Tick, std::vector<size_t>> chords;
  for (size_t i : affected(notes, sel)) chords[notes[i].start].push_back(i);
  for (auto& [t, v] : chords) {
    if (v.size() < 2) continue;
    auto cmp = [&](size_t a, size_t b) { return notes[a].pitch < notes[b].pitch; };
    if (dir > 0) {
      size_t lo = *std::min_element(v.begin(), v.end(), cmp);
      if (notes[lo].pitch + 12 <= 127) notes[lo].pitch += 12;
    } else {
      size_t hi = *std::max_element(v.begin(), v.end(), cmp);
      if (notes[hi].pitch - 12 >= 0) notes[hi].pitch -= 12;
    }
  }
  sortNotes(notes, sel);
}

void selectStacked(const std::vector<Note>& notes, NoteSel& sel) {
  sel.assign(notes.size(), false);
  std::map<Tick, int> count;
  for (const auto& n : notes) ++count[n.start];
  for (size_t i = 0; i < notes.size(); ++i) sel[i] = count[notes[i].start] > 1;
}

ScaleGuess detectScale(const std::vector<Note>& notes, const NoteSel& sel) {
  double weight[12] = {};
  double total = 0;
  for (size_t i : affected(notes, sel)) {
    const double w = static_cast<double>(std::max<Tick>(1, notes[i].length));
    weight[((notes[i].pitch % 12) + 12) % 12] += w;
    total += w;
  }
  ScaleGuess best;
  if (total <= 0) return best;
  const auto& sc = tools::scales();
  for (size_t k = 1; k < sc.size(); ++k)
    for (int root = 0; root < 12; ++root) {
      double in = 0;
      for (int step : sc[k].steps) in += weight[(root + step) % 12];
      double fit = in / total;
      // Prefer smaller scales on ties (they say more), then the root with the most weight.
      const double rank = fit - 0.001 * static_cast<double>(sc[k].steps.size()) + 1e-6 * weight[root] / total;
      const double bestRank =
          best.scale ? best.fit - 0.001 * static_cast<double>(sc[static_cast<size_t>(best.scale)].steps.size()) +
                           1e-6 * weight[best.root] / total
                     : -1.0;
      if (rank > bestRank + 1e-12) best = ScaleGuess{root, static_cast<int>(k), fit};
    }
  return best;
}

std::vector<int> scaleChord(int pitch, int root, int scale, int count, bool topDown) {
  const auto& sc = tools::scales();
  if (scale <= 0 || scale >= static_cast<int>(sc.size()) || count < 1) return {std::clamp(pitch, 0, 127)};
  pitch = tools::snapPitchToScale(std::clamp(pitch, 0, 127), root, scale);
  // Scale pitches around the clicked one, in order.
  std::vector<int> ladder;
  for (int p = std::max(0, pitch - 36); p <= std::min(127, pitch + 36); ++p)
    if (tools::inScale(p, root, scale)) ladder.push_back(p);
  const auto at = std::find(ladder.begin(), ladder.end(), pitch);
  if (at == ladder.end()) return {pitch};
  const int i0 = static_cast<int>(at - ladder.begin());
  std::vector<int> out;
  for (int k = 0; k < count; ++k) {
    const int i = topDown ? i0 - 2 * k : i0 + 2 * k;
    if (i < 0 || i >= static_cast<int>(ladder.size())) break;
    out.push_back(ladder[static_cast<size_t>(i)]);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string chordName(const std::vector<Note>& notes, const NoteSel& sel) {
  // J-31: chord detection is shared with the chord progression editor (ChordTools).
  std::vector<int> pitches;
  for (size_t i : affected(notes, sel)) pitches.push_back(notes[i].pitch);
  chord::Chord c;
  if (!chord::detect(pitches, c)) return "";
  std::string n = chord::chordName(c);
  if (c.inversion > 0) {
    const auto& st = chord::qualities()[static_cast<size_t>(c.quality)].steps;
    n += "/" + chord::noteName(c.root + st[static_cast<size_t>(c.inversion) % st.size()]);
  }
  return n;
}

// ----------------------------------------------------- time selection ----

std::pair<Tick, Tick> clipsTimeRange(const Song& s, const std::set<ObjectId>& clips) {
  Tick lo = 0, hi = 0;
  bool any = false;
  for (const auto& c : s.playlist) {
    if (!clips.count(c.id)) continue;
    const Tick e = c.start + s.clipLength(c);
    lo = any ? std::min(lo, c.start) : c.start;
    hi = any ? std::max(hi, e) : e;
    any = true;
  }
  return {lo, hi};
}

std::pair<Tick, Tick> shiftTimeSelection(Tick from, Tick to, int dir) {
  const Tick len = to - from;
  if (len <= 0 || dir == 0) return {from, to};
  Tick a = dir > 0 ? from + len : std::max<Tick>(0, from - len);
  return {a, a + len};
}

// ------------------------------------------------------------- markers ----

int addMarkerPair(Document& d, Tick from, Tick to) {
  if (from < 0 || to <= from) return 0;
  d.beginGroup("add two markers");
  addMarker(d, from, nextMarkerName(d.song()));
  addMarker(d, to, nextMarkerName(d.song()));
  d.endGroup();
  return 2;
}

int placeLoopMarkers(Document& d, Tick from, Tick to) {
  if (from < 0 || to <= from) return -1;
  d.beginGroup("place song loop marker");
  int i = addMarker(d, from, "Loop");
  editMarker(d, i, "place song loop marker", [](Marker& m) { m.action = MarkerAction::Loop; });
  // A marker at the end bounds the loop (the loop runs to the next marker).
  bool endExists = false;
  for (const auto& m : d.song().markers) endExists = endExists || m.pos == to;
  if (!endExists) addMarker(d, to, "Loop end");
  d.endGroup();
  for (size_t k = 0; k < d.song().markers.size(); ++k)
    if (d.song().markers[k].pos == from && d.song().markers[k].action == MarkerAction::Loop) return static_cast<int>(k);
  return -1;
}

int addSongLoopMarker(Document& d, Tick pos) {
  if (pos < 0) return -1;
  // As FL's place song loop marker: the song's loop marker is replaced (one
  // per song); placing it where it already is removes it (a toggle).
  bool here = false;
  for (const auto& m : d.song().markers) here = here || (m.action == MarkerAction::SongLoop && m.pos == pos);
  int index = -1;
  d.editSong("place song loop marker", [&](Song& s) {
    s.markers.erase(std::remove_if(s.markers.begin(), s.markers.end(), [](const Marker& m) { return m.action == MarkerAction::SongLoop; }),
                    s.markers.end());
    if (here) return;
    Marker m;
    m.pos = pos;
    m.name = "Loop";
    m.action = MarkerAction::SongLoop;
    auto it = std::upper_bound(s.markers.begin(), s.markers.end(), pos, [](Tick p, const Marker& k) { return p < k.pos; });
    index = static_cast<int>(it - s.markers.begin());
    s.markers.insert(it, m);
  });
  return index;
}

int placeLoop(Document& d, Tick selStart, Tick selEnd, Tick pos) {
  return selEnd > selStart ? placeLoopMarkers(d, selStart, selEnd) : addSongLoopMarker(d, pos);
}

int songLoopMarker(const Song& s) {
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const Tick end = std::max(bar, (s.lengthTicks() + bar - 1) / bar * bar);  // where song playback wraps
  int best = -1;
  for (size_t i = 0; i < s.markers.size(); ++i)
    if (s.markers[i].action == MarkerAction::SongLoop && s.markers[i].pos < end &&
        (best < 0 || s.markers[i].pos >= s.markers[static_cast<size_t>(best)].pos))
      best = static_cast<int>(i);
  return best;
}

int addJumpToNextBar(Document& d, Tick pos) {
  if (pos < 0) return -1;
  const Tick bar = std::max<Tick>(1, d.song().barTicks());
  const Tick next = (pos / bar + 1) * bar;
  d.beginGroup("add jump to next bar");
  int i = addMarker(d, pos, "Jump");
  editMarker(d, i, "add jump to next bar", [](Marker& m) { m.action = MarkerAction::Skip; });
  bool endExists = false;
  for (const auto& m : d.song().markers) endExists = endExists || m.pos == next;
  if (!endExists) addMarker(d, next, nextMarkerName(d.song()));
  d.endGroup();
  for (size_t k = 0; k < d.song().markers.size(); ++k)
    if (d.song().markers[k].pos == pos && d.song().markers[k].action == MarkerAction::Skip) return static_cast<int>(k);
  return -1;
}

// ---------------------------------------------------------- time display ----

const std::vector<SmpteRate>& smpteRates() {
  static const std::vector<SmpteRate> r = {
      {"23.976 fps", 24000.0 / 1001.0, false}, {"24 fps", 24, false},           {"25 fps", 25, false},
      {"29.97 fps (drop frame)", 30000.0 / 1001.0, true}, {"29.97 fps", 30000.0 / 1001.0, false},
      {"30 fps", 30, false}, {"48 fps", 48, false}, {"50 fps", 50, false},
      {"59.94 fps (drop frame)", 60000.0 / 1001.0, true}, {"59.94 fps", 60000.0 / 1001.0, false},
      {"60 fps", 60, false}, {"72 fps", 72, false}, {"120 fps", 120, false}};
  return r;
}

std::string formatSongTime(const Song& s, double tick, TimeFormat f, bool steps, int smpteRate) {
  char buf[96];
  tick = std::max(0.0, tick);
  if (f == TimeFormat::BarBeatTick) {
    const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks());
    const Tick unit = steps ? std::max<Tick>(1, beat / 4) : beat;
    const Tick t = static_cast<Tick>(tick);
    std::snprintf(buf, sizeof buf, "%03lld:%02lld:%03lld", static_cast<long long>(t / bar + 1),
                  static_cast<long long>((t % bar) / unit + 1), static_cast<long long>(t % unit));
    return buf;
  }
  TempoMap map;
  map.build(s);
  const double sec = std::max(0.0, map.secondsAt(tick));
  if (f == TimeFormat::MinSecCs) {
    const long long cs = static_cast<long long>(std::floor(sec * 100.0 + 1e-9));
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", cs / 6000, (cs / 100) % 60, cs % 100);
    return buf;
  }
  const auto& rates = smpteRates();
  const SmpteRate& r = rates[static_cast<size_t>(std::clamp(smpteRate, 0, static_cast<int>(rates.size()) - 1))];
  const int nominal = static_cast<int>(std::lround(r.fps));
  long long frame = static_cast<long long>(std::floor(sec * r.fps + 1e-9));
  if (r.drop) {
    // Drop-frame labels: skip frame numbers 0 and 1 (2 and 3 at 59.94) at the
    // start of every minute except every tenth minute.
    const long long dropN = nominal >= 60 ? 4 : 2;
    const long long perMin = nominal * 60 - dropN, per10 = perMin * 10 + dropN;
    const long long d10 = frame / per10, m = frame % per10;
    frame += dropN * 9 * d10 + (m > dropN ? dropN * ((m - dropN) / perMin) : 0);
  }
  const long long ff = frame % nominal, totalSec = frame / nominal;
  std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld%c%02lld", totalSec / 3600, (totalSec / 60) % 60, totalSec % 60,
                r.drop ? ';' : ':', ff);
  return buf;
}

// ------------------------------------------------------------ score log ----

void ScoreLog::noteOn(double t, int pitch, int velocity) {
  if (!enabled) return;
  events.push_back(Event{t, std::clamp(pitch, 0, 127), std::clamp(velocity, 1, 127)});
  prune(t);
}

void ScoreLog::noteOff(double t, int pitch) {
  if (!enabled) return;
  events.push_back(Event{t, std::clamp(pitch, 0, 127), 0});
  prune(t);
}

void ScoreLog::prune(double now) {
  while (!events.empty() && events.front().time < now - keepSeconds) events.pop_front();
}

std::vector<Note> scoreLogNotes(const ScoreLog& log, double now, double seconds, double bpm, int ppq) {
  std::vector<Note> out;
  if (bpm <= 0 || ppq <= 0) return out;
  const double from = now - seconds;
  const double tps = bpm / 60.0 * ppq;  // ticks per second
  struct Held {
    double t;
    int vel;
  };
  std::map<int, std::vector<Held>> held;
  std::vector<std::pair<double, Note>> raw;  // (start seconds, note with length in ticks)
  auto close = [&](int pitch, double t) {
    auto& v = held[pitch];
    if (v.empty()) return;
    Held h = v.front();
    v.erase(v.begin());
    if (h.t < from) return;
    Note n;
    n.pitch = pitch;
    n.velocity = h.vel;
    n.length = std::max<Tick>(1, static_cast<Tick>(std::lround((t - h.t) * tps)));
    raw.push_back({h.t, n});
  };
  for (const auto& e : log.events) {
    if (e.time > now) break;
    if (e.velocity > 0) held[e.pitch].push_back(Held{e.time, e.velocity});
    else close(e.pitch, e.time);
  }
  for (auto& [pitch, v] : held)
    while (!v.empty()) close(pitch, now);
  if (raw.empty()) return out;
  std::stable_sort(raw.begin(), raw.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  const double t0 = raw.front().first;
  for (auto& [t, n] : raw) {
    n.start = static_cast<Tick>(std::lround((t - t0) * tps));
    out.push_back(n);
  }
  NoteSel sel(out.size(), false);
  sortNotes(out, sel);
  return out;
}

int dumpScoreLog(Document& d, ObjectId pattern, ObjectId channel, const ScoreLog& log, double now, double seconds, bool replace) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  if (!p || !s.channel(channel)) return 0;
  auto add = scoreLogNotes(log, now, seconds, s.tempo, s.ppq);
  if (add.empty()) return 0;
  std::vector<Note> notes;
  if (!replace)
    if (auto it = p->notes.find(channel); it != p->notes.end()) notes = it->second;
  notes.insert(notes.end(), add.begin(), add.end());
  NoteSel sel(notes.size(), false);
  sortNotes(notes, sel);
  Tick end = 0;
  for (const auto& n : notes) end = std::max(end, n.start + n.length);
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const int bars = static_cast<int>(std::max<Tick>(p->bars, (end + bar - 1) / bar));
  d.beginGroup("score log dump");
  if (bars != p->bars) d.setPatternLength(pattern, bars, p->stepsPerBar);
  d.setNotes(pattern, channel, notes, "score log dump");
  d.endGroup();
  return static_cast<int>(add.size());
}

MidiFileData notesToMidiFile(const std::vector<Note>& notes, int ppq, double bpm, const std::string& name) {
  MidiFileData m;
  m.division = std::max(1, ppq);
  m.tempo = bpm > 0 ? bpm : 120.0;
  MidiTrack t;
  t.name = name;
  Tick t0 = 0;
  bool any = false;
  for (const auto& n : notes) t0 = any ? std::min(t0, n.start) : n.start, any = true;
  for (const auto& n : notes) {
    if (n.muted) continue;
    MidiNote x;
    x.start = n.start - t0;
    x.length = std::max<Tick>(1, n.length);
    x.pitch = std::clamp(n.pitch, 0, 127);
    x.velocity = std::clamp(n.velocity, 1, 127);
    t.notes.push_back(x);
  }
  m.tracks.push_back(std::move(t));
  return m;
}

// ------------------------------------------------------------- channels ----

ObjectId cloneChannelWithLinks(Document& d, ObjectId channel) {
  if (!d.song().channel(channel)) return kNoId;
  d.beginGroup("clone channel with links");
  ObjectId nid = d.cloneChannel(channel);
  if (nid) {
    d.editSong("clone channel with links", [&](Song& s) {
      std::vector<ControllerLink> add;
      for (const auto& l : s.links)
        if (l.action == RemoteAction::None && l.owner == channel) {
          ControllerLink k = l;
          k.owner = nid;
          add.push_back(k);
        }
      s.links.insert(s.links.end(), add.begin(), add.end());
      std::vector<ModLink> madd;
      for (const auto& l : s.modLinks)
        if (l.owner == channel) {
          ModLink k = l;
          k.owner = nid;
          madd.push_back(k);
        }
      s.modLinks.insert(s.modLinks.end(), madd.begin(), madd.end());
    });
  }
  d.endGroup();
  return nid;
}

bool autoFillSteps(Document& d, ObjectId pattern, ObjectId channel) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || !d.song().channel(channel)) return false;
  auto it = p->steps.find(channel);
  if (it == p->steps.end()) return false;
  const int per = std::max(1, p->stepsPerBar);
  const int total = per * std::max(1, p->bars);
  std::vector<uint8_t> row = it->second;
  row.resize(static_cast<size_t>(total), 0);
  bool any = false;
  for (int i = 0; i < per; ++i) any = any || row[static_cast<size_t>(i)];
  if (!any) return false;
  for (int i = per; i < total; ++i) row[static_cast<size_t>(i)] = row[static_cast<size_t>(i % per)];
  if (row == it->second) return false;
  return d.setSteps(pattern, channel, row);
}

int groupLayerChildren(Document& d, ObjectId layer, const std::string& name) {
  const Channel* l = d.song().channel(layer);
  if (!l || l->layer.empty()) return -1;
  std::set<ObjectId> kids(l->layer.begin(), l->layer.end());
  int g = -1;
  d.editSong("group layer children", [&](Song& s) {
    g = static_cast<int>(s.channelGroups.size());
    s.channelGroups.push_back(name.empty() ? std::string("Layer") : name);
    for (auto& c : s.channels)
      if (kids.count(c.id)) c.group = g;
  });
  return g;
}

int deleteLayerChildren(Document& d, ObjectId layer) {
  const Channel* l = d.song().channel(layer);
  if (!l || l->layer.empty()) return 0;
  std::vector<ObjectId> kids;
  for (ObjectId k : l->layer)
    if (k != layer && d.song().channel(k)) kids.push_back(k);
  d.beginGroup("delete layer children");
  for (ObjectId k : kids) d.removeChannel(k);
  if (const Channel* still = d.song().channel(layer); still && !still->layer.empty())
    d.editChannel(layer, "delete layer children", [](Channel& c) { c.layer.clear(); });
  d.endGroup();
  return static_cast<int>(kids.size());
}

// ------------------------------------------------------------ live mode ----

Tick prepareLiveArea(Document& d) {
  const Song& s = d.song();
  if (s.playlist.empty()) return 0;
  // Per track: the distinct sources in order of first appearance (song clips only).
  struct Src {
    ClipKind kind;
    ObjectId id;
    Clip sample;
  };
  std::map<int, std::vector<Src>> perTrack;
  std::vector<Clip> ordered = s.playlist;
  std::stable_sort(ordered.begin(), ordered.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
  for (const auto& c : ordered) {
    if (c.start < s.live.areaEnd) continue;
    const ObjectId src = c.kind == ClipKind::Pattern ? c.pattern : c.source;
    auto& v = perTrack[c.track];
    bool seen = false;
    for (const auto& x : v) seen = seen || (x.kind == c.kind && x.id == src);
    if (!seen) v.push_back(Src{c.kind, src, c});
  }
  const Tick bar = std::max<Tick>(1, s.barTicks());
  Tick area = 0;
  for (const auto& [t, v] : perTrack) {
    Tick sum = 0;
    for (const auto& x : v) sum += (s.clipLength(x.sample) + bar - 1) / bar * bar;
    area = std::max(area, sum);
  }
  if (area <= 0) return 0;
  const Tick shift = area;
  d.editSong("prepare for live mode", [&](Song& x) {
    for (auto& c : x.playlist)
      if (c.start >= x.live.areaEnd) c.start += shift;
    for (auto& m : x.markers)
      if (m.pos >= x.live.areaEnd) m.pos += shift;
    const Tick base = x.live.areaEnd;
    for (const auto& [t, v] : perTrack) {
      Tick at = base;
      for (const auto& src : v) {
        Clip k = src.sample;
        k.id = x.allocateId();
        k.start = at;
        k.muted = false;
        x.playlist.push_back(k);
        at += (x.clipLength(k) + bar - 1) / bar * bar;
      }
    }
    x.live.areaEnd = base + shift;
  });
  return area;
}

}  // namespace gl::ops

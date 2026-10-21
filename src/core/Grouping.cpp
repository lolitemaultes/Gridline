// Gridline — groups and loops (J-31). See Grouping.h.
#include "Grouping.h"
#include <algorithm>
#include <map>

namespace gl::tools {

std::vector<int> cloneNotes(std::vector<Note>& notes, std::vector<bool>& sel) {
  sel.resize(notes.size(), false);
  const size_t n = notes.size();
  int maxGroup = 0;
  for (const Note& x : notes) maxGroup = std::max(maxGroup, x.group);
  std::map<int, int> fresh;
  std::vector<int> map(n, -1);
  for (size_t i = 0; i < n; ++i) {
    if (!sel[i]) continue;
    Note c = notes[i];
    if (c.group != 0) {
      auto [it, added] = fresh.emplace(c.group, 0);
      if (added) it->second = ++maxGroup;
      c.group = it->second;
    }
    map[i] = static_cast<int>(notes.size());
    notes.push_back(c);
  }
  sel.assign(notes.size(), false);
  for (size_t i = n; i < notes.size(); ++i) sel[i] = true;
  return map;
}

namespace {
bool anySel(const ops::NoteSel& sel) { return std::find(sel.begin(), sel.end(), true) != sel.end(); }
bool isSel(const ops::NoteSel& sel, size_t i) { return i < sel.size() && sel[i]; }
}  // namespace

int groupNotes(std::vector<Note>& notes, const ops::NoteSel& sel) {
  size_t count = 0;
  for (size_t i = 0; i < notes.size(); ++i) count += isSel(sel, i) ? 1 : 0;
  if (count < 2) return 0;
  int next = 1;
  for (const Note& n : notes) next = std::max(next, n.group + 1);
  for (size_t i = 0; i < notes.size(); ++i)
    if (isSel(sel, i)) notes[i].group = next;
  return next;
}

int ungroupNotes(std::vector<Note>& notes, const ops::NoteSel& sel) {
  const bool all = !anySel(sel);
  int n = 0;
  for (size_t i = 0; i < notes.size(); ++i)
    if ((all || isSel(sel, i)) && notes[i].group != 0) {
      notes[i].group = 0;
      ++n;
    }
  return n;
}

int selectGroups(const std::vector<Note>& notes, ops::NoteSel& sel) {
  sel.resize(notes.size(), false);
  std::set<int> groups;
  for (size_t i = 0; i < notes.size(); ++i)
    if (sel[i] && notes[i].group != 0) groups.insert(notes[i].group);
  int added = 0;
  for (size_t i = 0; i < notes.size(); ++i)
    if (!sel[i] && groups.count(notes[i].group)) {
      sel[i] = true;
      ++added;
    }
  return added;
}

}  // namespace gl::tools

namespace gl::ops {

// ------------------------------------------------------------ pattern groups ----

int patternGroupOf(const Song& s, ObjectId pattern) {
  for (size_t g = 0; g < s.patternGroups.size(); ++g) {
    const auto& v = s.patternGroups[g].patterns;
    if (std::find(v.begin(), v.end(), pattern) != v.end()) return static_cast<int>(g);
  }
  return -1;
}

std::vector<ObjectId> patternsInGroup(const Song& s, int group) {
  std::vector<ObjectId> out;
  if (group < 0 || group >= static_cast<int>(s.patternGroups.size())) return out;
  const auto& v = s.patternGroups[static_cast<size_t>(group)].patterns;
  for (const auto& p : s.patterns)
    if (std::find(v.begin(), v.end(), p.id) != v.end()) out.push_back(p.id);
  return out;
}

namespace {
void dropEmptyGroups(Song& s) {
  for (auto& g : s.patternGroups)
    g.patterns.erase(std::remove_if(g.patterns.begin(), g.patterns.end(), [&](ObjectId id) { return !s.pattern(id); }),
                     g.patterns.end());
  s.patternGroups.erase(std::remove_if(s.patternGroups.begin(), s.patternGroups.end(),
                                       [](const PatternGroup& g) { return g.patterns.empty(); }),
                        s.patternGroups.end());
}
}  // namespace

int groupPatterns(Document& d, const std::vector<ObjectId>& patterns, const std::string& name) {
  std::vector<ObjectId> ids;
  for (ObjectId id : patterns)
    if (d.song().pattern(id) && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  if (ids.empty()) return -1;
  int index = -1;
  d.editSong("group patterns", [&](Song& s) {
    for (auto& g : s.patternGroups)
      g.patterns.erase(std::remove_if(g.patterns.begin(), g.patterns.end(),
                                      [&](ObjectId id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }),
                       g.patterns.end());
    dropEmptyGroups(s);
    PatternGroup g;
    g.name = name.empty() ? "Group " + std::to_string(s.patternGroups.size() + 1) : name;
    g.patterns = ids;
    s.patternGroups.push_back(std::move(g));
    index = static_cast<int>(s.patternGroups.size()) - 1;
  });
  return index;
}

bool renamePatternGroup(Document& d, int group, const std::string& name) {
  if (group < 0 || group >= static_cast<int>(d.song().patternGroups.size()) || name.empty()) return false;
  if (d.song().patternGroups[static_cast<size_t>(group)].name == name) return false;
  d.editSong("rename pattern group", [&](Song& s) { s.patternGroups[static_cast<size_t>(group)].name = name; });
  return true;
}

int ungroupPatterns(Document& d, const std::vector<ObjectId>& patterns) {
  int n = 0;
  for (ObjectId id : patterns) n += patternGroupOf(d.song(), id) >= 0 ? 1 : 0;
  if (n == 0) return 0;
  d.editSong("ungroup patterns", [&](Song& s) {
    for (auto& g : s.patternGroups)
      g.patterns.erase(std::remove_if(g.patterns.begin(), g.patterns.end(),
                                      [&](ObjectId id) { return std::find(patterns.begin(), patterns.end(), id) != patterns.end(); }),
                       g.patterns.end());
    dropEmptyGroups(s);
  });
  return n;
}

bool deletePatternGroup(Document& d, int group) {
  if (group < 0 || group >= static_cast<int>(d.song().patternGroups.size())) return false;
  d.editSong("delete pattern group", [&](Song& s) { s.patternGroups.erase(s.patternGroups.begin() + group); });
  return true;
}

// --------------------------------------------------------------- clip groups ----

ObjectId groupClips(Document& d, const std::set<ObjectId>& clips) {
  size_t n = 0;
  for (ObjectId id : clips) n += d.song().clip(id) ? 1 : 0;
  if (n < 2) return kNoId;
  ObjectId g = kNoId;
  d.editSong("group clips", [&](Song& s) {
    g = s.allocateId();
    for (auto& c : s.playlist)
      if (clips.count(c.id)) c.group = g;
  });
  return g;
}

int ungroupClips(Document& d, const std::set<ObjectId>& clips) {
  // Whole groups come apart: every clip of a group a selected clip is in.
  std::set<ObjectId> groups;
  for (ObjectId id : clips)
    if (const Clip* c = d.song().clip(id); c && c->group != kNoId) groups.insert(c->group);
  if (groups.empty()) return 0;
  int n = 0;
  for (const auto& c : d.song().playlist) n += groups.count(c.group) ? 1 : 0;
  d.editClips("ungroup clips", [&](std::vector<Clip>& v) {
    for (auto& c : v)
      if (groups.count(c.group)) c.group = kNoId;
  });
  return n;
}

std::set<ObjectId> expandClipGroups(const Song& s, const std::set<ObjectId>& clips) {
  std::set<ObjectId> groups, out = clips;
  for (ObjectId id : clips)
    if (const Clip* c = s.clip(id); c && c->group != kNoId) groups.insert(c->group);
  if (groups.empty()) return out;
  for (const auto& c : s.playlist)
    if (groups.count(c.group)) out.insert(c.id);
  return out;
}

// ----------------------------------------------------------------- step loops ----

int stepLoop(const Pattern& p, ObjectId channel) {
  auto it = p.stepLoop.find(channel);
  return it == p.stepLoop.end() ? 0 : std::max(0, it->second);
}

bool setStepLoop(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int steps) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || channels.empty()) return false;
  const int total = p->bars * p->stepsPerBar;
  steps = std::max(0, steps);
  if (steps >= total) steps = 0;
  bool change = false;
  for (ObjectId ch : channels) change = change || stepLoop(*p, ch) != steps;
  if (!change) return false;
  return d.editPattern(pattern, steps > 0 ? "loop channel steps" : "stop looping channel steps", [&](Pattern& q) {
    for (ObjectId ch : channels) {
      if (steps > 0)
        q.stepLoop[ch] = steps;
      else
        q.stepLoop.erase(ch);
    }
  });
}

int contentSteps(const Song& s, const Pattern& p, ObjectId channel) {
  int last = 0;
  if (auto it = p.steps.find(channel); it != p.steps.end())
    for (size_t i = 0; i < it->second.size(); ++i)
      if (it->second[i] != 0) last = static_cast<int>(i) + 1;
  const Tick st = s.ticksPerStep(p);
  if (auto it = p.notes.find(channel); it != p.notes.end() && st > 0)
    for (const Note& n : it->second) last = std::max(last, static_cast<int>((n.start + n.length + st - 1) / st));
  return last;
}

int loopToContent(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int unit) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p) return 0;
  unit = std::max(1, unit);
  const int total = p->bars * p->stepsPerBar;
  std::map<ObjectId, int> want;
  for (ObjectId ch : channels) {
    const int c = contentSteps(d.song(), *p, ch);
    int L = c <= 0 ? 0 : (c + unit - 1) / unit * unit;
    if (L >= total) L = 0;
    want[ch] = L;
  }
  int looping = 0;
  for (const auto& [ch, L] : want) looping += L > 0 ? 1 : 0;
  d.editPattern(pattern, "loop channels to their content", [&](Pattern& q) {
    for (const auto& [ch, L] : want) {
      if (L > 0)
        q.stepLoop[ch] = L;
      else
        q.stepLoop.erase(ch);
    }
  });
  return looping;
}

std::vector<uint8_t> loopedRow(const std::vector<uint8_t>& row, int loop, int total) {
  std::vector<uint8_t> out(static_cast<size_t>(std::max(0, total)), 0);
  for (int s = 0; s < total; ++s) {
    const int src = loop > 0 && loop < total ? s % loop : s;
    if (src < static_cast<int>(row.size())) out[static_cast<size_t>(s)] = row[static_cast<size_t>(src)];
  }
  return out;
}

int burnStepLoops(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p) return 0;
  const int total = p->bars * p->stepsPerBar;
  const Tick st = d.song().ticksPerStep(*p);
  std::vector<ObjectId> todo;
  for (ObjectId ch : channels)
    if (stepLoop(*p, ch) > 0) todo.push_back(ch);
  if (todo.empty()) return 0;
  d.editPattern(pattern, "burn step loops", [&](Pattern& q) {
    for (ObjectId ch : todo) {
      const int loop = stepLoop(q, ch);
      if (auto it = q.steps.find(ch); it != q.steps.end()) it->second = loopedRow(it->second, loop, total);
      if (auto it = q.stepProps.find(ch); it != q.stepProps.end()) {
        std::vector<StepProps> props(static_cast<size_t>(total));
        for (int s = 0; s < total; ++s)
          if (static_cast<size_t>(s % loop) < it->second.size()) props[static_cast<size_t>(s)] = it->second[static_cast<size_t>(s % loop)];
        it->second = std::move(props);
      }
      if (auto it = q.notes.find(ch); it != q.notes.end() && st > 0) {
        const Tick L = st * loop, len = st * total;
        std::vector<Note> out;
        for (const Note& n : it->second) {
          if (n.start >= L) continue;
          for (Tick r = 0; n.start + r < len; r += L) {
            Note k = n;
            k.start += r;
            out.push_back(k);
          }
        }
        std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) {
          return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
        });
        it->second = std::move(out);
      }
      q.stepLoop.erase(ch);
    }
  });
  return static_cast<int>(todo.size());
}

// --------------------------------------------------------- layer key splits ----

bool setKeyRange(Document& d, ObjectId channel, int lo, int hi) {
  lo = std::clamp(lo, 0, 127);
  hi = std::clamp(hi, 0, 127);
  if (hi < lo) std::swap(lo, hi);
  const Channel* c = d.song().channel(channel);
  if (!c || (c->keyLo == lo && c->keyHi == hi)) return false;
  return d.editChannel(channel, "set key range", [&](Channel& x) {
    x.keyLo = lo;
    x.keyHi = hi;
  });
}

bool splitLayerChildren(Document& d, ObjectId layer) {
  const Channel* L = d.song().channel(layer);
  if (!L || L->layer.empty()) return false;
  std::vector<ObjectId> kids;
  for (ObjectId k : L->layer)
    if (d.song().channel(k)) kids.push_back(k);
  if (kids.empty()) return false;
  int lo = 128, hi = -1;
  for (const auto& p : d.song().patterns)
    if (auto it = p.notes.find(layer); it != p.notes.end())
      for (const Note& n : it->second) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
  if (hi < lo) lo = 48, hi = 83;  // C4..B6
  const int k = static_cast<int>(kids.size());
  if (hi - lo + 1 < k) lo = 0, hi = 127;
  const int width = hi - lo + 1;
  d.editSong("split layer children", [&](Song& s) {
    for (int i = 0; i < k; ++i) {
      Channel* c = s.channel(kids[static_cast<size_t>(i)]);
      c->keyLo = i == 0 ? 0 : lo + i * width / k;
      c->keyHi = i == k - 1 ? 127 : lo + (i + 1) * width / k - 1;
    }
  });
  return true;
}

bool resetLayerChildren(Document& d, ObjectId layer) {
  const Channel* L = d.song().channel(layer);
  if (!L || L->layer.empty()) return false;
  bool any = false;
  for (ObjectId k : L->layer)
    if (const Channel* c = d.song().channel(k)) any = any || !c->fullKeyRange();
  if (!any) return false;
  const std::vector<ObjectId> kids = L->layer;
  d.editSong("reset layer children", [&](Song& s) {
    for (ObjectId k : kids)
      if (Channel* c = s.channel(k)) c->keyLo = 0, c->keyHi = 127;
  });
  return true;
}

// ----------------------------------------------------------------- alt solo ----

bool altSoloMixerTracks(Document& d, const std::vector<ObjectId>& tracks) {
  std::set<ObjectId> ids;
  for (ObjectId id : tracks)
    if (d.song().mixerTrack(id) && id != d.song().masterId()) ids.insert(id);
  if (ids.empty()) return false;
  bool already = true;
  for (const auto& m : d.song().mixer) already = already && (ids.count(m.id) ? (m.solo && m.soloAlone) : !m.solo);
  d.editSong(already ? "unsolo mixer tracks" : "alt solo mixer tracks", [&](Song& s) {
    for (auto& m : s.mixer) {
      m.solo = !already && ids.count(m.id);
      m.soloAlone = m.solo;
    }
  });
  return true;
}

// ------------------------------------------------------------ truncated swing ----

int setSwingTruncate(Document& d, const std::vector<ObjectId>& channels, int mode) {
  std::vector<ObjectId> ids;
  for (ObjectId id : channels)
    if (const Channel* c = d.song().channel(id)) {
      const bool want = mode == 2 ? !c->swingTruncate : mode == 1;
      if (want != c->swingTruncate) ids.push_back(id);
    }
  if (ids.empty()) return 0;
  d.editSong(mode == 0 ? "turn off truncated swing" : "truncate swing notes", [&](Song& s) {
    for (ObjectId id : ids)
      if (Channel* c = s.channel(id)) c->swingTruncate = !c->swingTruncate;
  });
  return static_cast<int>(ids.size());
}

}  // namespace gl::ops

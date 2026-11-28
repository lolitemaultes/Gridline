// Gridline — K-198 follow-up: one note selection over several channels (see MultiNoteEdit.h).
#include "MultiNoteEdit.h"
#include <algorithm>
#include <numeric>
#include "Grouping.h"
#include "StepNotes.h"

namespace gl::multinote {

namespace {
// The clamps and order of the piano roll's commit (and Document::setNotes), keeping `sel` aligned.
void sortKeeping(std::vector<Note>& notes, std::vector<bool>& sel) {
  for (auto& n : notes) {
    n.pitch = std::clamp(n.pitch, 0, 127);
    n.velocity = std::clamp(n.velocity, 0, 127);  // GitHub #87
    n.start = std::max<Tick>(n.start, 0);
    n.length = std::max<Tick>(n.length, 1);
  }
  sel.resize(notes.size(), false);
  std::vector<size_t> idx(notes.size());
  std::iota(idx.begin(), idx.end(), size_t{0});
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    return notes[a].start != notes[b].start ? notes[a].start < notes[b].start : notes[a].pitch < notes[b].pitch;
  });
  std::vector<Note> n2;
  std::vector<bool> s2;
  n2.reserve(notes.size());
  for (size_t i : idx) n2.push_back(notes[i]), s2.push_back(sel[i]);
  notes.swap(n2);
  sel.swap(s2);
}

std::set<size_t> indicesOf(const std::vector<bool>& sel) {
  std::set<size_t> out;
  for (size_t i = 0; i < sel.size(); ++i)
    if (sel[i]) out.insert(i);
  return out;
}
}  // namespace

size_t count(const Selection& sel) {
  size_t n = 0;
  for (const auto& [c, ix] : sel) n += ix.size();
  return n;
}

void prune(Selection& sel) {
  for (auto it = sel.begin(); it != sel.end();) it = it->second.empty() ? sel.erase(it) : std::next(it);
}

Lists lists(const Song& s, ObjectId pattern, const std::set<ObjectId>& channels) {
  Lists out;
  const Pattern* p = s.pattern(pattern);
  if (!p) return out;
  for (ObjectId c : channels)
    if (s.channel(c)) out[c] = stepnotes::view(s, *p, c);
  return out;
}

Lists lists(const Song& s, ObjectId pattern, const Selection& sel) {
  std::set<ObjectId> chans;
  for (const auto& [c, ix] : sel) chans.insert(c);
  return lists(s, pattern, chans);
}

int clampPitchDelta(const Lists& base, const Selection& sel, int dp) {
  for (const auto& [c, ix] : sel) {
    auto it = base.find(c);
    if (it == base.end()) continue;
    for (size_t i : ix)
      if (i < it->second.size()) dp = std::clamp(dp, -it->second[i].pitch, 127 - it->second[i].pitch);
  }
  return dp;
}

Tick earliestStart(const Lists& base, const Selection& sel, Tick fallback) {
  bool any = false;
  Tick t = fallback;
  for (const auto& [c, ix] : sel) {
    auto it = base.find(c);
    if (it == base.end()) continue;
    for (size_t i : ix)
      if (i < it->second.size()) t = any ? std::min(t, it->second[i].start) : it->second[i].start, any = true;
  }
  return t;
}

Selection transform(Document& d, ObjectId pattern, const Lists& base, const Selection& sel,
                    const std::function<void(Note&)>& fn, bool copy, const std::string& label, uint64_t mergeKey) {
  Selection out;
  for (const auto& [c, ix] : sel) {
    auto it = base.find(c);
    if (it == base.end() || !d.song().channel(c)) continue;
    std::vector<Note> ns = it->second;
    std::vector<bool> flag(ns.size(), false);
    for (size_t i : ix)
      if (i < ns.size()) flag[i] = true;
    if (copy) tools::cloneNotes(ns, flag);  // Q-3: copies keep every property, grouped ones in new groups
    for (size_t i = 0; i < ns.size(); ++i)
      if (flag[i]) fn(ns[i]);
    sortKeeping(ns, flag);
    stepnotes::write(d, pattern, c, ns, label, mergeKey);
    if (std::set<size_t> s = indicesOf(flag); !s.empty()) out[c] = std::move(s);
  }
  return out;
}

void erase(Document& d, ObjectId pattern, const Lists& base, const Selection& sel, const std::string& label) {
  for (const auto& [c, ix] : sel) {
    auto it = base.find(c);
    if (it == base.end() || ix.empty() || !d.song().channel(c)) continue;
    std::vector<Note> keep;
    for (size_t i = 0; i < it->second.size(); ++i)
      if (!ix.count(i)) keep.push_back(it->second[i]);
    stepnotes::write(d, pattern, c, keep, label);
  }
}

std::vector<ChannelNote> copy(const Lists& base, const Selection& sel) {
  std::vector<ChannelNote> out;
  for (const auto& [c, ix] : sel) {
    auto it = base.find(c);
    if (it == base.end()) continue;
    for (size_t i : ix)
      if (i < it->second.size()) out.push_back(ChannelNote{c, it->second[i]});
  }
  return out;
}

Selection paste(Document& d, ObjectId pattern, const std::vector<ChannelNote>& clip, Tick shift, const std::string& label) {
  Selection out;
  const Pattern* p = d.song().pattern(pattern);
  if (!p) return out;
  std::map<ObjectId, std::vector<Note>> add;
  for (const ChannelNote& cn : clip)
    if (d.song().channel(cn.channel)) {
      Note n = cn.note;
      n.start += shift;
      add[cn.channel].push_back(n);
    }
  for (auto& [c, extra] : add) {
    std::vector<Note> ns = stepnotes::view(d.song(), *d.song().pattern(pattern), c);
    std::vector<bool> flag(ns.size(), false);
    for (const Note& n : extra) ns.push_back(n), flag.push_back(true);
    sortKeeping(ns, flag);
    stepnotes::write(d, pattern, c, ns, label);
    out[c] = indicesOf(flag);
  }
  return out;
}

Tick pasteShift(const std::vector<ChannelNote>& clip, Tick grid) {
  if (clip.empty()) return 0;
  Tick lo = clip[0].note.start, hi = 0;
  for (const auto& cn : clip) lo = std::min(lo, cn.note.start), hi = std::max(hi, cn.note.start + cn.note.length);
  const Tick span = hi - lo;
  grid = std::max<Tick>(grid, 1);
  return ((span + grid / 2) / grid) * grid;  // nearest grid line, as the piano roll's snapNearest
}

}  // namespace gl::multinote

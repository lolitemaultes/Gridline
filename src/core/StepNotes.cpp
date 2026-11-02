// Gridline — K-22: channel rack steps in the piano roll (see StepNotes.h).
#include "StepNotes.h"
#include <algorithm>

namespace gl::stepnotes {

namespace {
// The same clamps and order as Document::setNotes.
void normalise(std::vector<Note>& notes) {
  for (auto& n : notes) {
    n.pitch = std::clamp(n.pitch, 0, 127);
    n.velocity = std::clamp(n.velocity, 1, 127);
    n.start = std::max<Tick>(n.start, 0);
    n.length = std::max<Tick>(n.length, 1);
  }
  std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
}
}  // namespace

std::vector<Note> fromSteps(const Song& s, const Pattern& p, ObjectId channel) {
  std::vector<Note> out;
  const auto it = p.steps.find(channel);
  const Tick tps = s.ticksPerStep(p);
  if (it == p.steps.end() || tps <= 0) return out;
  const std::vector<StepProps>* props = nullptr;
  if (auto pt = p.stepProps.find(channel); pt != p.stepProps.end()) props = &pt->second;
  const size_t total = static_cast<size_t>(std::max(0, p.bars)) * static_cast<size_t>(std::max(0, p.stepsPerBar));
  const auto& row = it->second;
  for (size_t k = 0; k < row.size() && k < total; ++k) {
    if (!row[k]) continue;
    const StepProps sp = props && k < props->size() ? (*props)[k] : StepProps{};
    Note n;
    n.start = static_cast<Tick>(k) * tps + tps * sp.shift / kStepShiftMax;
    n.length = tps;
    n.pitch = sp.pitch < 0 ? 60 : sp.pitch;  // the step's default key (C5), as it plays
    n.velocity = std::clamp<int>(row[k], 1, 127);
    n.pan = sp.pan;
    n.release = sp.release;
    n.fine = sp.fine;
    n.modX = sp.modX;
    n.modY = sp.modY;
    n.step = true;
    out.push_back(n);
  }
  return out;
}

bool hasSteps(const Song& s, const Pattern& p, ObjectId channel) {
  const auto it = p.steps.find(channel);
  if (it == p.steps.end() || s.ticksPerStep(p) <= 0) return false;
  const size_t total = static_cast<size_t>(std::max(0, p.bars)) * static_cast<size_t>(std::max(0, p.stepsPerBar));
  for (size_t k = 0; k < it->second.size() && k < total; ++k)
    if (it->second[k]) return true;
  return false;
}

std::vector<Note> view(const Song& s, const Pattern& p, ObjectId channel) {
  std::vector<Note> out = fromSteps(s, p, channel);
  if (auto it = p.notes.find(channel); it != p.notes.end()) out.insert(out.end(), it->second.begin(), it->second.end());
  if (p.steps.count(channel)) normalise(out);  // only notes: already in order
  return out;
}

bool write(Document& d, ObjectId pattern, ObjectId channel, std::vector<Note> notes, const std::string& label, uint64_t mergeKey) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  if (!p || !s.channel(channel)) return false;
  if (!p->steps.count(channel) && !p->stepProps.count(channel)) return d.setNotes(pattern, channel, std::move(notes), label, mergeKey);
  normalise(notes);
  if (notes == view(s, *p, channel)) return true;  // nothing changed: the steps stay steps
  return d.editPattern(
      pattern, label,
      [&](Pattern& q) {
        if (notes.empty())
          q.notes.erase(channel);
        else
          q.notes[channel] = notes;
        q.steps.erase(channel);  // they are in `notes` now
        q.stepProps.erase(channel);
      },
      mergeKey);
}

bool playsAsStep(const Song& s, const Pattern& p, const Note& n) {
  const Tick tps = s.ticksPerStep(p);
  return n.step && tps > 0 && n.length == tps;
}

Snapshot capture(const Song& s, ObjectId pattern, ObjectId channel) {
  Snapshot snap;
  snap.pattern = pattern;
  snap.channel = channel;
  const Pattern* p = s.pattern(pattern);
  if (!p) return snap;
  if (auto it = p->notes.find(channel); it != p->notes.end()) snap.notes = it->second;
  if (auto it = p->steps.find(channel); it != p->steps.end()) snap.row = it->second, snap.hadRow = true;
  if (auto it = p->stepProps.find(channel); it != p->stepProps.end()) snap.props = it->second, snap.hadProps = true;
  return snap;
}

bool restore(Document& d, const Snapshot& snap, const std::string& label, uint64_t mergeKey) {
  const Pattern* p = d.song().pattern(snap.pattern);
  if (!p) return false;
  const ObjectId ch = snap.channel;
  return d.editPattern(
      snap.pattern, label,
      [&](Pattern& q) {
        if (snap.notes.empty())
          q.notes.erase(ch);
        else
          q.notes[ch] = snap.notes;
        if (snap.hadRow)
          q.steps[ch] = snap.row;
        else
          q.steps.erase(ch);
        if (snap.hadProps)
          q.stepProps[ch] = snap.props;
        else
          q.stepProps.erase(ch);
      },
      mergeKey);
}

}  // namespace gl::stepnotes

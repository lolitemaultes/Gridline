// Gridline — piano roll event lane editing (Q-4). See NoteLaneEdit.h.
#include "NoteLaneEdit.h"
#include <algorithm>
#include <limits>

namespace gl::notelane {

double segmentValue(double t0, double v0, double t1, double v1, double t) {
  if (t1 < t0) std::swap(t0, t1), std::swap(v0, v1);
  if (t1 - t0 <= 1e-9) return v1;
  const double f = std::clamp((t - t0) / (t1 - t0), 0.0, 1.0);
  return v0 + (v1 - v0) * f;
}

namespace {
bool editable(const std::vector<bool>& only, size_t i) { return only.empty() || (i < only.size() && only[i]); }
}  // namespace

std::vector<size_t> nearestInSweep(const std::vector<Note>& notes, const std::vector<bool>& only, double t0, double t1) {
  const double lo = std::min(t0, t1), hi = std::max(t0, t1);
  // Distinct start ticks of the editable notes, ascending.
  std::vector<Tick> starts;
  for (size_t i = 0; i < notes.size(); ++i)
    if (editable(only, i)) starts.push_back(notes[i].start);
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  // A start is "nearest" somewhere in [lo, hi] when its cell (half way to the
  // neighbouring starts) overlaps the range.
  std::vector<Tick> hit;
  const double inf = std::numeric_limits<double>::infinity();
  for (size_t k = 0; k < starts.size(); ++k) {
    const double s = static_cast<double>(starts[k]);
    const double cLo = k == 0 ? -inf : 0.5 * (static_cast<double>(starts[k - 1]) + s);
    const double cHi = k + 1 == starts.size() ? inf : 0.5 * (s + static_cast<double>(starts[k + 1]));
    if (cHi >= lo && cLo <= hi) hit.push_back(starts[k]);
  }
  std::vector<size_t> out;
  for (size_t i = 0; i < notes.size(); ++i)
    if (editable(only, i) && std::binary_search(hit.begin(), hit.end(), notes[i].start)) out.push_back(i);
  return out;
}

std::vector<size_t> notesBetween(const std::vector<Note>& notes, const std::vector<bool>& only, double t0, double t1) {
  const double lo = std::min(t0, t1), hi = std::max(t0, t1);
  std::vector<size_t> out;
  for (size_t i = 0; i < notes.size(); ++i) {
    const double s = static_cast<double>(notes[i].start);
    if (editable(only, i) && s >= lo && s <= hi) out.push_back(i);
  }
  return out;
}

namespace {
bool setAll(std::vector<Note>& notes, const std::vector<size_t>& idx, tools::NoteProp p, double t0, double v0, double t1, double v1,
            SetFn set) {
  if (!set) set = tools::setNotePropNorm;
  bool changed = false;
  for (size_t i : idx) {
    const Note before = notes[i];
    set(notes[i], p, segmentValue(t0, v0, t1, v1, static_cast<double>(notes[i].start)));
    changed = changed || !(before == notes[i]);
  }
  return changed;
}
}  // namespace

bool paintSegment(std::vector<Note>& notes, const std::vector<bool>& only, tools::NoteProp p, double t0, double v0, double t1,
                  double v1, SetFn set) {
  return setAll(notes, nearestInSweep(notes, only, t0, t1), p, t0, v0, t1, v1, set);
}

bool applyLine(std::vector<Note>& notes, const std::vector<bool>& only, tools::NoteProp p, double tA, double vA, double tB, double vB,
               SetFn set, double slackTicks) {
  const double lo = std::min(tA, tB) - slackTicks, hi = std::max(tA, tB) + slackTicks;
  return setAll(notes, notesBetween(notes, only, lo, hi), p, tA, vA, tB, vB, set);
}

}  // namespace gl::notelane

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

// ---- K-423: per-lane snap grids ----------------------------------------------

const std::vector<GridOption>& gridOptions(tools::NoteProp p) {
  static const std::vector<GridOption> kFine = {
      {"Off", "OFF", 0},
      {"1 semitone", "1 ST", 24},
      {"1/2 semitone (50 ct)", "50 CT", 48},
      {"2 semitones", "2 ST", 12},
      {"3 semitones", "3 ST", 8},
      {"Octave", "OCT", 2},
  };
  static const std::vector<GridOption> kPan = {
      {"Off", "OFF", 0},
      {"1/4 (50%)", "1/4", 4},
      {"1/8 (25%)", "1/8", 8},
      {"1/10 (20%)", "1/10", 10},
  };
  static const std::vector<GridOption> kUnipolar = {
      {"Off", "OFF", 0},
      {"1/4", "1/4", 4},
      {"1/8", "1/8", 8},
      {"1/16", "1/16", 16},
      {"10%", "10%", 10},
  };
  if (p == tools::NoteProp::Fine) return kFine;
  if (p == tools::NoteProp::Pan) return kPan;
  return kUnipolar;
}

const char* gridSettingKey(tools::NoteProp p) {
  switch (p) {
    case tools::NoteProp::Pan: return "piano.laneGrid.pan";
    case tools::NoteProp::Release: return "piano.laneGrid.release";
    case tools::NoteProp::Fine: return "piano.laneGrid.fine";
    case tools::NoteProp::ModX: return "piano.laneGrid.modX";
    case tools::NoteProp::ModY: return "piano.laneGrid.modY";
    default: return "piano.laneGrid.velocity";
  }
}

int clampGridMode(tools::NoteProp p, int mode) {
  const auto& opts = gridOptions(p);
  if (opts.empty()) return 0;
  return std::clamp(mode, 0, static_cast<int>(opts.size()) - 1);
}

int gridSteps(tools::NoteProp p, int mode) {
  const auto& opts = gridOptions(p);
  return opts[static_cast<size_t>(clampGridMode(p, mode))].steps;
}

double snapValue(tools::NoteProp p, int mode, double frac, bool bypass) {
  frac = std::clamp(frac, 0.0, 1.0);
  if (bypass) return frac;
  const int steps = gridSteps(p, mode);
  if (steps <= 0) return frac;
  return std::clamp(std::round(frac * static_cast<double>(steps)) / static_cast<double>(steps), 0.0, 1.0);
}

std::vector<double> gridLines(tools::NoteProp p, int mode) {
  std::vector<double> out;
  const int steps = gridSteps(p, mode);
  if (steps <= 0) return out;
  out.reserve(static_cast<size_t>(steps));
  for (int k = 1; k <= steps; ++k) out.push_back(static_cast<double>(k) / static_cast<double>(steps));
  return out;
}

}  // namespace gl::notelane

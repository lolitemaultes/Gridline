// Gridline — timeline maths (see Timeline.h).
#include "Timeline.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl {

const char* snapName(Snap s) {
  switch (s) {
    case Snap::Line: return "Line";
    case Snap::Cell: return "Cell";
    case Snap::None: return "None";
    case Snap::Sixth: return "1/6 step";
    case Snap::Quarter: return "1/4 step";
    case Snap::Third: return "1/3 step";
    case Snap::Half: return "1/2 step";
    case Snap::Step: return "Step";
    case Snap::Beat: return "Beat";
    case Snap::Bar: return "Bar";
  }
  return "?";
}

Tick snapTicks(const Song& s, Snap snap) {
  Tick step = std::max<Tick>(1, s.ppq / 4);
  switch (snap) {
    case Snap::None: return 1;
    case Snap::Sixth: return std::max<Tick>(1, step / 6);
    case Snap::Quarter: return std::max<Tick>(1, step / 4);
    case Snap::Third: return std::max<Tick>(1, step / 3);
    case Snap::Half: return std::max<Tick>(1, step / 2);
    case Snap::Line:
    case Snap::Cell:
    case Snap::Step: return step;
    case Snap::Beat: return std::max<Tick>(1, s.beatTicks());
    case Snap::Bar: return std::max<Tick>(1, s.barTicks());
  }
  return 1;
}

Tick snapDown(Tick t, Tick g) { return g <= 1 ? t : (t >= 0 ? t / g * g : -((-t + g - 1) / g * g)); }
Tick snapNearest(Tick t, Tick g) { return g <= 1 ? t : snapDown(t + g / 2, g); }

namespace timeline {

namespace {

Tick stepTicks(const Song& s) { return std::max<Tick>(1, s.ppq / 4); }

// Grid sizes from coarsest to finest, each dividing the previous where possible.
std::vector<Tick> ladder(const Song& s) {
  const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks()), step = stepTicks(s);
  std::vector<Tick> v;
  for (Tick k : {64, 32, 16, 8, 4, 2}) v.push_back(bar * k);
  v.push_back(bar);
  if (beat < bar) v.push_back(beat);
  if (step < beat) v.push_back(step);
  Tick d = std::min({bar, beat, step});
  while (d > 1) {
    if (d % 2 == 0) d /= 2;
    else if (d % 3 == 0) d /= 3;
    else d = 1;
    v.push_back(d);
  }
  if (v.back() != 1) v.push_back(1);
  return v;
}

}  // namespace

double minPxPerTick(const Song& s) { return kMinBeatPx / static_cast<double>(std::max<Tick>(1, s.beatTicks())); }
// Deep enough that a single tick is at least 16 px wide at every PPQ.
double maxPxPerTick(const Song& s) {
  return std::max(16.0, kMaxBeatPx / static_cast<double>(std::max<Tick>(1, s.beatTicks())));
}
bool atMaxZoom(const Song& s, double pxPerTick) { return pxPerTick >= maxPxPerTick(s) * 0.98; }

Tick visibleGrid(const Song& s, double pxPerTick, double minPx) {
  const std::vector<Tick> v = ladder(s);
  // Finest first: the first size whose lines are far enough apart.
  for (auto it = v.rbegin(); it != v.rend(); ++it)
    if (static_cast<double>(*it) * pxPerTick >= minPx) return *it;
  return v.front();
}

Tick snapGrid(const Song& s, Snap mode, double pxPerTick, bool altHeld) {
  const bool adaptive = mode == Snap::Line || mode == Snap::Cell;
  if (altHeld) return mode == Snap::None ? visibleGrid(s, pxPerTick) : 1;  // Alt inverts snapping
  if (!adaptive) return snapTicks(s, mode);
  return atMaxZoom(s, pxPerTick) ? 1 : visibleGrid(s, pxPerTick);
}

Tick snapTick(Tick t, Tick grid, Snap mode) { return mode == Snap::Cell ? snapDown(t, grid) : snapNearest(t, grid); }

Tick tickAtPx(float px, float pxPerTick) {
  constexpr double kLim = 4.0e15;  // far past any song, well inside Tick
  const double t = static_cast<double>(std::floor(px / pxPerTick));
  if (std::isnan(t)) return 0;
  return static_cast<Tick>(std::clamp(t, -kLim, kLim));
}

double clampZoom(const Song& s, double pxPerTick) { return std::clamp(pxPerTick, minPxPerTick(s), maxPxPerTick(s)); }

void zoomAt(const Song& s, float& pxPerTick, float& scrollX, double factor, float anchorPx) {
  const double tickAtAnchor = (static_cast<double>(anchorPx) + scrollX) / pxPerTick;
  const double z = clampZoom(s, pxPerTick * factor);
  pxPerTick = static_cast<float>(z);
  scrollX = static_cast<float>(std::max(0.0, tickAtAnchor * z - anchorPx));
}

double fitZoom(const Song& s, Tick ticks, float widthPx) {
  return clampZoom(s, static_cast<double>(std::max(1.0f, widthPx)) / static_cast<double>(std::max<Tick>(1, ticks)));
}

std::string rulerLabel(const Song& s, Tick t, Tick grid) {
  const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks()), step = stepTicks(s);
  t = std::max<Tick>(0, t);
  const long long b = t / bar + 1, bt = (t % bar) / beat + 1, st = (t % beat) / step + 1, tk = (t % beat) % step;
  char buf[96];
  if (grid >= bar) std::snprintf(buf, sizeof buf, "%lld", b);
  else if (grid >= beat) std::snprintf(buf, sizeof buf, "%lld.%lld", b, bt);
  else if (grid >= step) std::snprintf(buf, sizeof buf, "%lld.%lld.%lld", b, bt, st);
  else if (tk == 0) std::snprintf(buf, sizeof buf, "%lld.%lld.%lld", b, bt, st);
  else std::snprintf(buf, sizeof buf, "%lld.%lld.%lld:%lld", b, bt, st, tk);
  return buf;
}

std::string gridName(const Song& s, Tick grid) {
  const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks()), step = stepTicks(s);
  if (grid <= 1) return "free";
  if (grid >= bar && grid % bar == 0) return grid == bar ? "bar" : std::to_string(grid / bar) + " bars";
  if (grid == beat) return "beat";
  if (grid == step) return "step";
  if (grid < step && step % grid == 0) return "1/" + std::to_string(step / grid) + " step";
  return std::to_string(grid) + " ticks";
}

}  // namespace timeline
}  // namespace gl

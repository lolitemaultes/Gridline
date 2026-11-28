// Gridline - marker areas (see MarkerAreas.h).
#include "MarkerAreas.h"
#include <algorithm>
#include <utility>

namespace gl::markers {

bool isAreaMarker(const Marker& m) { return !m.isMeter() && !m.isKey(); }

std::vector<Area> areas(const std::vector<Marker>& markers, Tick start, Tick end) {
  std::vector<Area> out;
  if (end <= start) return out;
  auto at = [&](int i) -> const Marker& { return markers[static_cast<size_t>(i)]; };
  std::vector<int> order;
  for (size_t i = 0; i < markers.size(); ++i)
    if (isAreaMarker(markers[i]) && markers[i].pos < end) order.push_back(static_cast<int>(i));
  std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return at(x).pos < at(y).pos; });
  // Markers before `start` move to it: the one nearest to it names the first area. Markers at one
  // position make one area: the first named one names it (the first one when none has a name).
  std::vector<std::pair<Tick, int>> cuts;  // (position, marker)
  for (int i : order) {
    const Tick p = std::max(start, at(i).pos);
    if (!cuts.empty() && cuts.back().first == p) {
      const Marker& kept = at(cuts.back().second);
      if (kept.pos < at(i).pos || (kept.pos == at(i).pos && kept.name.empty() && !at(i).name.empty())) cuts.back().second = i;
      continue;
    }
    cuts.push_back({p, i});
  }
  if (cuts.empty()) return out;
  if (cuts.front().first > start) out.push_back(Area{start, cuts.front().first, std::string(), -1});
  for (size_t k = 0; k < cuts.size(); ++k) {
    Area a;
    a.start = cuts[k].first;
    a.end = k + 1 < cuts.size() ? cuts[k + 1].first : end;
    a.marker = cuts[k].second;
    a.name = at(a.marker).name;
    if (a.end > a.start) out.push_back(a);
  }
  return out;
}

}  // namespace gl::markers

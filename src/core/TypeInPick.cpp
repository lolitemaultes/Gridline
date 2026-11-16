// Gridline - K-311: see TypeInPick.h.
#include "TypeInPick.h"
#include <algorithm>
#include <cmath>

namespace gl::typein {

std::vector<ParamId> paramsAt(const std::vector<Event>& events, float x, float y, float pressX, float pressY, unsigned hovered) {
  auto inside = [](const Event& e, float px, float py) { return px >= e.x0 && px < e.x1 && py >= e.y0 && py < e.y1; };
  int at = -1;
  for (int i = static_cast<int>(events.size()) - 1; i >= 0 && at < 0; --i) {
    const Event& e = events[static_cast<size_t>(i)];
    if (e.item && hovered != 0 && e.id == hovered && inside(e, x, y)) at = i;
  }
  for (int i = static_cast<int>(events.size()) - 1; i >= 0 && at < 0; --i) {  // the topmost item under the pointer
    const Event& e = events[static_cast<size_t>(i)];
    if (e.item && e.id != 0 && inside(e, x, y)) at = i;
  }
  std::vector<ParamId> out;
  if (at < 0 || !inside(events[static_cast<size_t>(at)], pressX, pressY)) return out;
  auto add = [&out](const ParamId& p) {
    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  };
  // The parameters it asked for on its way in (since the item before), in the order asked.
  int from = at;
  while (from > 0 && !events[static_cast<size_t>(from - 1)].item) --from;
  for (int i = from; i < at; ++i) add(events[static_cast<size_t>(i)].pid);
  // None: the ones it asked for after its item (up to the next item).
  if (out.empty())
    for (int i = at + 1; i < static_cast<int>(events.size()) && !events[static_cast<size_t>(i)].item; ++i) add(events[static_cast<size_t>(i)].pid);
  // Still none: one of a row of cells drawn for one parameter (a shape selector asks once, then adds
  // a cell per shape): the nearest parameters asked for before it, a few items back at most.
  // The cells in between must look like it (about its height), so a lone button after a knob is
  // never taken for that knob.
  if (out.empty()) {
    const float h = events[static_cast<size_t>(at)].y1 - events[static_cast<size_t>(at)].y0;
    auto alike = [h](const Event& e) { return std::fabs((e.y1 - e.y0) - h) <= 0.3f * h; };
    int j = at - 1;
    while (j >= 0 && at - j <= 12 && events[static_cast<size_t>(j)].item && alike(events[static_cast<size_t>(j)])) --j;
    if (j >= 0 && at - j <= 12 && !events[static_cast<size_t>(j)].item)
      for (; j >= 0 && !events[static_cast<size_t>(j)].item; --j) add(events[static_cast<size_t>(j)].pid);
  }
  if (static_cast<int>(out.size()) > kMaxParams) out.clear();  // a display that reads many: no guess
  return out;
}

}  // namespace gl::typein

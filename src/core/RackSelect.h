// Gridline - K-232: the channel rack's select buttons (the small light right of each
// channel's name). Pure selection rules (tests/test_k232.cpp):
//  * click: adds the channel to the selection, or takes it out again (the last selected
//    channel stays selected); it becomes the channel the keyboard and the piano roll follow;
//  * right-click: selects that channel only;
//  * Shift+click: selects every channel from the followed one to this one.
// Selecting never opens a window: the typing keyboard plays every selected instrument.
#pragma once
#include <algorithm>
#include <set>
#include <vector>
#include "Types.h"

namespace gl::rackselect {

enum class Click { Toggle, Only, Range };

// `order`: the rack's channels top to bottom. `sel` / `primary`: the selection and the channel it
// follows (App::selChannels / selChannel), updated in place.
inline void click(const std::vector<ObjectId>& order, std::set<ObjectId>& sel, ObjectId& primary, ObjectId id, Click how) {
  if (std::find(order.begin(), order.end(), id) == order.end()) return;
  if (how == Click::Only) {
    sel = {id};
    primary = id;
    return;
  }
  if (how == Click::Range) {
    auto a = std::find(order.begin(), order.end(), primary), b = std::find(order.begin(), order.end(), id);
    if (a == order.end()) a = b;
    if (a > b) std::swap(a, b);
    sel = std::set<ObjectId>(a, b + 1);
    primary = id;
    return;
  }
  if (sel.count(id)) {
    if (sel.size() == 1) return;  // the last selected channel stays selected
    sel.erase(id);
    if (primary == id)
      for (ObjectId o : order)
        if (sel.count(o)) {
          primary = o;
          break;
        }
    return;
  }
  sel.insert(id);
  primary = id;
}

// The channels the typing keyboard plays: the followed one first, then the other selected ones
// in rack order.
inline std::vector<ObjectId> playTargets(const std::vector<ObjectId>& order, const std::set<ObjectId>& sel, ObjectId primary) {
  std::vector<ObjectId> out;
  if (primary != kNoId) out.push_back(primary);
  for (ObjectId o : order)
    if (o != primary && sel.count(o)) out.push_back(o);
  return out;
}

}  // namespace gl::rackselect

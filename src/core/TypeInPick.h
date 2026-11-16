// Gridline - K-311: which parameter(s) a right-click on a plugin editor control means, from the
// order in which the editor asked for parameters and added items on that frame (app/TypeInMenu.h
// records it). Pure (tests/test_k311_typein.cpp).
#pragma once
#include <vector>
#include "Params.h"

namespace gl::typein {

// What a right release at `at` (the press at `pressAt`) offers, from a recorded sequence: the
// parameters asked for before the item under the pointer (since the item before it), else after it
// (up to the next item). Empty when no item is under the pointer, the press was elsewhere, or the
// item asked for no parameter or for too many (more than kMaxParams). Pure (tests).
struct Event {
  bool item = false;
  unsigned id = 0;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  ParamId pid{};
};
constexpr int kMaxParams = 8;
// `hovered`: ImGui's hovered item (0: none known); it wins over the rectangles when recorded.
std::vector<ParamId> paramsAt(const std::vector<Event>& events, float x, float y, float pressX, float pressY, unsigned hovered = 0);

}  // namespace gl::typein

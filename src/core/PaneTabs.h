// Gridline - K-237: the tab strip of a layout pane (plan option C, phase 1).
//
// Pure rules, no ImGui: the order of a pane's tabs, pinned tabs (always first, never closed or
// dragged away by accident), the active tab, closing with a short history so the last closed tab
// can come back where it was, cycling (Ctrl+Tab), and which tabs fit a strip of a given width
// (the rest go to the overflow menu, the active tab always stays in view). The state is saved as
// text per region in the layout's state (layout.<id>.state), never in a project.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gl::panetabs {

struct Strip {
  std::vector<std::string> order;  // the tabs in strip order (window names)
  std::set<std::string> pinned;
  std::string active;
  bool operator==(const Strip& o) const { return order == o.order && pinned == o.pinned && active == o.active; }
};

// Keeps the tabs that are still members (in their order), adds new members at the end, moves
// pinned tabs first, and picks an active tab that is a member (the first one when the active tab
// went away).
void sync(Strip& s, const std::vector<std::string>& members);

// Moves tab `name` to position `to` (0 = first). A pinned tab stays among the pinned ones and an
// unpinned tab after them. False when the tab is unknown or did not move.
bool move(Strip& s, const std::string& name, int to);

// Pins or unpins a tab: pinning moves it to the end of the pinned tabs, unpinning to the first
// place after them.
bool setPinned(Strip& s, const std::string& name, bool pin);

// One closed tab: which pane, which window and where it stood.
struct Closed {
  std::string region, window;
  int index = 0;
};
class History {
 public:
  static constexpr size_t kMax = 20;
  void push(const Closed& c);
  bool empty() const { return items_.empty(); }
  // The last closed tab (and it leaves the history).
  Closed pop();
  // Forget a window (it was opened again some other way).
  void forget(const std::string& window);
  const std::vector<Closed>& items() const { return items_; }

 private:
  std::vector<Closed> items_;
};

// Closes a tab: refused for a pinned tab or the last tab. The active tab moves to its right-hand
// neighbour (or the left one at the end). The closed tab goes to `h` with `region`.
bool close(Strip& s, const std::string& name, const std::string& region, History& h);
// Puts a closed tab back at its old place (clamped) and makes it active.
void reopen(Strip& s, const Closed& c);

// The next or previous tab from the active one, wrapping (Ctrl+Tab, Ctrl+Shift+Tab).
std::string cycle(const Strip& s, int dir);

// Which tabs show in a strip `avail` px wide when tab i needs widths[i] px and the overflow
// button needs `overflowW`: the indices shown, in order. Pinned tabs and the active tab are kept
// first; others drop from the end. Every tab shows when they all fit without the button.
std::vector<int> fit(const Strip& s, const std::vector<float>& widths, float avail, float overflowW);

// Text form for the layout state: "a|b|c;pinned=a;active=b" (window names never hold '|' or ';').
std::string save(const Strip& s);
Strip load(const std::string& text);

}  // namespace gl::panetabs

// Gridline - zone keys for the Studio layout: hold Tab and press a digit to jump to a zone (a window, or
// two that a second press swaps between); Tab pressed and let go alone goes back to the zone before.
//
// A small state machine fed the key events of a frame (Tab down, a digit, Tab up, a key repeat, a cancel
// when the keys go somewhere else) that says what to do: go to zone n, go back, or nothing, and whether
// the digit was taken (it then never plays a note or reaches a command). The zones themselves (which
// windows, saved per person) and the short history of where the person was are here too.
//
// Pure, no ImGui; tested alone (tests/test_k340_zones.cpp). The app feeds it (app/ZoneKeysUi.cpp).
#pragma once
#include <array>
#include <string>
#include <vector>

namespace gl::zones {

constexpr int kZones = 9;  // Tab+1 .. Tab+9

struct Action {
  enum Kind { None, Zone, Back } kind = None;
  int zone = 0;          // 1..9 for Zone
  bool swallow = false;  // the key was taken by the layer
};

class Layer {
 public:
  // Tab went down (repeat: a key repeat while held, ignored).
  Action tabDown(bool repeat = false);
  // A digit key 1..9 (0 and others give nothing) went down.
  Action digit(int n);
  // Tab came up: alone (no digit since it went down) it goes back.
  Action tabUp();
  // The keys went elsewhere (a text field, a popup, the window lost focus): Tab up does nothing then.
  void cancel();
  bool held() const { return held_; }

 private:
  bool held_ = false;
  bool used_ = false;
};

// What each zone opens: one window, or two that pressing the zone again swaps between.
struct Zone {
  std::array<std::string, 2> windows;  // stable window names ("" = none)
  bool empty() const { return windows[0].empty() && windows[1].empty(); }
  int count() const { return (windows[0].empty() ? 0 : 1) + (windows[1].empty() ? 0 : 1); }
  bool operator==(const Zone& o) const { return windows == o.windows; }
};
using Zones = std::array<Zone, kZones>;

// 1 the playlist, 2 the piano roll, 3 the mixer and the channel rack; 4 to 9 empty.
Zones defaults();
// Text for settings: "1=Playlist;2=Piano roll;3=Mixer|Channel rack" (empty zones left out). Reading
// skips broken parts and zone numbers out of range; "" gives the defaults.
std::string toText(const Zones& z);
Zones fromText(const std::string& text);

// Which window a jump to zone `z` shows: its first window, or (pressed again while there, `here` being
// the zone's window in front, 0 or 1) the other one. -1 for an empty zone.
int pick(const Zone& z, bool alreadyThere, int here);

// The last two zones visited (Tab alone toggles between them).
class History {
 public:
  void visit(int zone);  // a jump (or the person clicking into a zone's window)
  int back();            // the zone to go back to (0: none), and the two swap
  int current() const { return cur_; }
  int previous() const { return prev_; }

 private:
  int cur_ = 0, prev_ = 0;
};

}  // namespace gl::zones

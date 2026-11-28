// Gridline: GitHub #88, a held key whose auto-repeat arrives as a release and a new press.
//
// A held key repeats while it stays down. Most systems mark the repeats (SDL's `repeat` field), and
// every part of Gridline that plays a note from a key ignores those. But some ways a key reaches
// Gridline send each repeat as a release followed at once by a new press: X11 without detectable
// auto-repeat (SDL folds those pairs itself, but not for the keys of a plugin's window), remote
// desktop and screen sharing clients, virtual machines, key remapping tools and some keyboards' own
// software, and a held key reported up and down again while the keyboard focus moves between
// Gridline's own windows (SDL on Windows releases every key when a window loses the focus). Each pair
// ended the note and started it again: the typing keyboard "repeated" a held note.
//
// A finger cannot let go of a key and press it again within a few milliseconds, so a press that
// follows the same key's release that closely is the key still being held: here the release is held
// back for that long, and when the press comes in time both become one repeat (the release is dropped,
// the press is marked as a repeat). Every part of the interface then sees one press and one release
// for the whole hold, as the system's own repeats give. Releases with no press after them go on
// unchanged, only that much later.
//
// Header only, SDL only: the program (main.cpp) and the test (tests/test_key_repeat_pairs.cpp) share it.
#pragma once
#include <SDL.h>
#include <deque>

namespace gl::keyrepeat {

// How long a release waits for the same key's press (milliseconds, SDL event timestamps).
constexpr Uint32 kPairMs = 12;

// The keys this applies to: the letter, digit and punctuation keys (every typing-keyboard and
// Tracker note key) and the keypad's - and + (the typing keyboard's octave keys). Not Return, Esc,
// Backspace, Tab, Space, the arrows or the modifiers (an event of one of those lets the waiting
// releases go first).
inline bool watched(SDL_Scancode sc) {
  const int u = static_cast<int>(sc);
  return (u >= SDL_SCANCODE_A && u <= SDL_SCANCODE_0) || (u >= SDL_SCANCODE_MINUS && u <= SDL_SCANCODE_SLASH) ||
         u == SDL_SCANCODE_NONUSBACKSLASH || u == SDL_SCANCODE_KP_MINUS || u == SDL_SCANCODE_KP_PLUS;
}

class Pairs {
 public:
  // An event as SDL gave it: what to handle now goes to `out` (in order).
  void push(const SDL_Event& e, std::deque<SDL_Event>& out) {
    // Another key (a modifier, Return, an arrow...) never overtakes a release that waits: each key event
    // carries the modifiers held at its time, and an "E up (Ctrl+Alt held)" handled after "Ctrl up" and
    // "Alt up" would set Ctrl and Alt down again, left stuck until the next key.
    if ((e.type == SDL_KEYUP || e.type == SDL_KEYDOWN) && !watched(e.key.keysym.scancode)) flush(0, out, true);
    if (e.type == SDL_KEYUP && watched(e.key.keysym.scancode)) {
      held_.push_back(e);
      return;
    }
    if (e.type == SDL_KEYDOWN && watched(e.key.keysym.scancode)) {
      for (auto it = held_.begin(); it != held_.end(); ++it) {
        // (any window: a key held while the focus moves between Gridline's windows is still the same press)
        if (it->key.keysym.scancode != e.key.keysym.scancode) continue;
        if (stamp(e) - stamp(*it) > kPairMs) break;  // (too late: a new press; the release goes first, below)
        held_.erase(it);
        SDL_Event r = e;
        r.key.repeat = 1;  // the key never went up
        ++folded_;
        out.push_back(r);
        return;
      }
      // A new press of a key whose release waits: that release happened first.
      for (auto it = held_.begin(); it != held_.end();) {
        if (it->key.keysym.scancode == e.key.keysym.scancode) {
          out.push_back(*it);
          it = held_.erase(it);
        } else {
          ++it;
        }
      }
    }
    out.push_back(e);
  }
  // The releases whose time ran out by `nowMs` (SDL_GetTicks()), in order; every one with `all`.
  void flush(Uint32 nowMs, std::deque<SDL_Event>& out, bool all = false) {
    while (!held_.empty() && (all || nowMs - stamp(held_.front()) > kPairMs)) {
      out.push_back(held_.front());
      held_.pop_front();
    }
  }
  bool pending() const { return !held_.empty(); }
  // How many release and press pairs became repeats so far (the checks read it).
  unsigned folded() const { return folded_; }

 private:
  static Uint32 stamp(const SDL_Event& e) { return e.key.timestamp; }
  std::deque<SDL_Event> held_;
  unsigned folded_ = 0;
};

}  // namespace gl::keyrepeat

// Gridline, GitHub #88: a held key whose auto-repeat arrives as a release and a new press is one press
// (src/app/KeyRepeatPairs.h). Runs without a window: the filter only reorders and rewrites events.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "Check.h"
#include "app/KeyRepeatPairs.h"
#include <SDL.h>
#include <deque>

using namespace gl;

namespace {

SDL_Event key(Uint32 type, SDL_Scancode sc, Uint32 ms, bool repeat = false, Uint32 window = 1) {
  SDL_Event e{};
  e.type = type;
  e.key.timestamp = ms;
  e.key.windowID = window;
  e.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
  e.key.repeat = repeat ? 1 : 0;
  e.key.keysym.scancode = sc;
  return e;
}

int count(const std::deque<SDL_Event>& q, Uint32 type, bool repeat) {
  int n = 0;
  for (const SDL_Event& e : q) n += e.type == type && (type != SDL_KEYDOWN || (e.key.repeat != 0) == repeat);
  return n;
}

}  // namespace

int main() {
  using keyrepeat::Pairs;
  {  // the system's own repeats pass as they are
    Pairs p;
    std::deque<SDL_Event> q;
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_Z, 100), q);
    for (Uint32 t = 600; t < 1200; t += 33) p.push(key(SDL_KEYDOWN, SDL_SCANCODE_Z, t, true), q);
    p.push(key(SDL_KEYUP, SDL_SCANCODE_Z, 1300), q);
    expect(p.pending() && q.size() == 20, "marked repeats pass through (the release waits)");
    p.flush(1300 + keyrepeat::kPairMs + 1, q);
    expect(count(q, SDL_KEYDOWN, false) == 1 && count(q, SDL_KEYDOWN, true) == 19 && count(q, SDL_KEYUP, false) == 1, "one press, the repeats, one release");
    expect(!p.pending(), "nothing left waiting");
  }
  {  // a modifier's release never overtakes a waiting release (its modifiers would stick)
    Pairs p;
    std::deque<SDL_Event> q;
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_LCTRL, 100), q);
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_E, 110), q);
    p.push(key(SDL_KEYUP, SDL_SCANCODE_E, 150), q);
    expect(p.pending(), "the letter's release waits");
    p.push(key(SDL_KEYUP, SDL_SCANCODE_LCTRL, 151), q);
    expect(!p.pending() && q.size() == 4, "the modifier's release lets it go");
    expect(q[2].type == SDL_KEYUP && q[2].key.keysym.scancode == SDL_SCANCODE_E && q[3].type == SDL_KEYUP &&
               q[3].key.keysym.scancode == SDL_SCANCODE_LCTRL,
           "in the order they happened: the letter, then the modifier");
  }
  {  // repeats sent as release + press pairs (same millisecond, or a few apart): one press, repeats, one release
    Pairs p;
    std::deque<SDL_Event> q;
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_Z, 100), q);
    Uint32 t = 600;
    for (int i = 0; i < 20; ++i, t += 33) {
      p.push(key(SDL_KEYUP, SDL_SCANCODE_Z, t), q);
      p.push(key(SDL_KEYDOWN, SDL_SCANCODE_Z, t + static_cast<Uint32>(i % 3)), q);
      p.flush(t + 2, q);  // (a frame in between: nothing is due yet)
    }
    p.push(key(SDL_KEYUP, SDL_SCANCODE_Z, t), q);
    expect(count(q, SDL_KEYUP, false) == 0, "the pairs' releases are held back or dropped");
    p.flush(t + keyrepeat::kPairMs + 1, q);
    expect(count(q, SDL_KEYDOWN, false) == 1, "one fresh press");
    expect(count(q, SDL_KEYDOWN, true) == 20, "every pair became a repeat");
    expect(count(q, SDL_KEYUP, false) == 1 && q.back().type == SDL_KEYUP, "one release, last");
    expect(p.folded() == 20, "20 pairs folded");
  }
  {  // a real release, then a real press later: both stay
    Pairs p;
    std::deque<SDL_Event> q;
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_X, 0), q);
    p.push(key(SDL_KEYUP, SDL_SCANCODE_X, 200), q);
    p.flush(205, q);
    expect(count(q, SDL_KEYUP, false) == 0 && p.pending(), "a release waits a moment");
    p.flush(200 + keyrepeat::kPairMs + 1, q);
    expect(count(q, SDL_KEYUP, false) == 1 && !p.pending(), "then goes on");
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_X, 290), q);
    expect(count(q, SDL_KEYDOWN, false) == 2, "a press 90 ms later is a new press");
  }
  {  // a late press while the release still waits (a slow frame): the release goes first, the press stays new
    Pairs p;
    std::deque<SDL_Event> q;
    p.push(key(SDL_KEYUP, SDL_SCANCODE_C, 100), q);
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_C, 180), q);
    expect(q.size() == 2 && q[0].type == SDL_KEYUP && q[1].type == SDL_KEYDOWN && q[1].key.repeat == 0, "release then a new press, in order");
  }
  {  // other keys are not held up by a waiting release, and pair only with themselves
    Pairs p;
    std::deque<SDL_Event> q;
    p.push(key(SDL_KEYUP, SDL_SCANCODE_Z, 100), q);
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_X, 101), q);
    expect(q.size() == 1 && q[0].key.keysym.scancode == SDL_SCANCODE_X && q[0].key.repeat == 0, "another key's press passes, not a repeat");
    p.flush(100 + keyrepeat::kPairMs + 1, q);
    expect(q.size() == 2 && q[1].type == SDL_KEYUP, "the release follows when due");
  }
  {  // keys that are not note keys are never held back
    Pairs p;
    std::deque<SDL_Event> q;
    for (SDL_Scancode sc : {SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_LEFT, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL})
      p.push(key(SDL_KEYUP, sc, 10), q);
    expect(q.size() == 6 && !p.pending(), "Space, Return, Esc, arrows and modifiers pass at once");
    expect(keyrepeat::watched(SDL_SCANCODE_A) && keyrepeat::watched(SDL_SCANCODE_0) && keyrepeat::watched(SDL_SCANCODE_SLASH) &&
               keyrepeat::watched(SDL_SCANCODE_LEFTBRACKET) && keyrepeat::watched(SDL_SCANCODE_KP_PLUS),
           "letters, digits, punctuation and the keypad octave keys are watched");
  }
  {  // the focus moves between Gridline's windows while the key is held: up in one, down in the other
    Pairs p;
    std::deque<SDL_Event> q;
    p.push(key(SDL_KEYUP, SDL_SCANCODE_Z, 500, false, 1), q);
    SDL_Event lost{};
    lost.type = SDL_WINDOWEVENT;
    lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    p.push(lost, q);
    p.push(key(SDL_KEYDOWN, SDL_SCANCODE_Z, 503, false, 2), q);
    expect(q.size() == 2 && q[0].type == SDL_WINDOWEVENT && q[1].type == SDL_KEYDOWN && q[1].key.repeat == 1, "still held: a repeat in the new window");
  }
  {  // flush(all) lets every waiting release go
    Pairs p;
    std::deque<SDL_Event> q;
    p.push(key(SDL_KEYUP, SDL_SCANCODE_Q, 100), q);
    p.push(key(SDL_KEYUP, SDL_SCANCODE_W, 101), q);
    p.flush(101, q, true);
    expect(q.size() == 2 && !p.pending(), "flush all");
  }
  return finish();
}

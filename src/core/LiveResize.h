// Gridline — J-116: when a frame is drawn from inside the event pump (live resize).
//
// While the user resizes a window, some window systems run a modal loop inside the event pump:
// macOS (AppKit's live resize tracking), Windows (DefWindowProc's size / move loop). The main loop
// is blocked there, so without help the window shows its last frame stretched until the mouse is
// let go. Gridline watches SDL's window events as they are generated (SDL_AddEventWatch) and draws
// one complete frame from the watch when this gate says so:
//
// - only from inside the main loop's event pump, never from inside a frame (re-entrancy: the frame
//   itself resizes windows, which reports events synchronously on some systems);
// - only when the pump is blocked: the platform says a modal size / move loop runs, or the pump
//   has not returned for `blockedAfter` seconds. Elsewhere (X11, Wayland, the BSDs: no modal loop,
//   the events arrive in the pump and the main loop draws the new size right after it) nothing is
//   drawn from the watch, so a size step never gets two frames;
// - for a size event that changes the size of the window from the one it was last drawn at, at
//   once; for an expose / move (Windows' and macOS's live-resize timers, a modal move) or a size
//   event of an unchanged size (SDL reports SIZE_CHANGED then RESIZED, then the view's EXPOSED, for
//   one step; macOS's 60 Hz live-resize timer lands between two drag steps) only when
//   `minInterval` has passed since the last frame began: one frame per step, and still ~60
//   frames a second while the window is held still (Windows' 10 ms timer: every other tick);
// - and, for those, not right as the last frame ends (`settle`): a frame slower than
//   `minInterval` (a loaded machine, software GL) would otherwise be followed at once by one more
//   frame for each of the step's own SIZE_CHANGED / RESIZED / EXPOSED (and on X11 the size SDL
//   reports while the frame resized the window), each as slow, all at the size just drawn.
#pragma once

namespace gl::live {

enum class Kind { Resize, Expose, Move };

struct Gate {
  double blockedAfter = 0.05;  // s: a pump that has not returned for this long is blocked
  double minInterval = 0.014;  // s: frames for timer / expose / move events at most this often (~70 Hz)
  double settle = 0.005;       // s: and not within this long of the last frame's end
};

// sinceLastFrame: since the last frame began; sinceLastFrameEnd: since it ended.
inline bool shouldDraw(Kind kind, bool inPump, bool inFrame, bool modal, double pumpElapsed, bool sizeChanged, double sinceLastFrame,
                       double sinceLastFrameEnd, const Gate& g = Gate{}) {
  if (!inPump || inFrame) return false;
  if (!modal && pumpElapsed < g.blockedAfter) return false;
  if (kind == Kind::Resize && sizeChanged) return true;
  return sinceLastFrame >= g.minInterval && sinceLastFrameEnd >= g.settle;
}

}  // namespace gl::live

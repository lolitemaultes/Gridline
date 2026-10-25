// Gridline — the native pieces of a plugin window: the view a plugin's
// interface is attached to, stacking above the main window, backing scale.
// macOS: NativeViewMac.mm (AppKit); X11: NativeViewX11.cpp (Xlib, resolved at
// run time from the library SDL already loaded).
#pragma once
#include <string>
#include "../EditorLayout.h"
#include "../KeyForward.h"

namespace gl::host::native {

// A top-level window as the window system knows it.
struct Window {
  void* display = nullptr;  // X11 Display* (nullptr on macOS)
  void* handle = nullptr;   // NSWindow* / X11 Window id
  bool valid() const { return handle != nullptr; }
};

// The window a view (NSView*) is in (macOS; tests).
Window windowOfView(void* view);

// Creates the view plugin editors attach to, at `r` (top-left based, in the
// window's content coordinates). Returns the NSView* / X11 Window id to pass
// to Editor::attach, nullptr on failure.
void* createContainer(const Window& w, const layout::Rect& r);
// Moves / resizes it (keeps it pinned to the top of the content area on macOS).
void setContainerFrame(const Window& w, void* container, const layout::Rect& r);
void destroyContainer(const Window& w, void* container);
// Keeps `child` above `parent` (FL-style floating plugin windows): an AppKit
// child window / an X11 transient. `on` = false undoes it before closing.
void keepAbove(const Window& child, const Window& parent, bool on);
// J-114 (macOS): `w`, when shown, ordered just above `ref` without being activated (a detached
// Gridline window kept above the main window when that comes forward). Elsewhere a no-op:
// keepAbove's ownership does it.
void orderAbove(const Window& w, const Window& ref);
// J-116: the window system runs a modal size / move loop of one of this application's windows
// right now (macOS: a window is in live resize; Windows: the UI thread is in move / size mode).
// The main loop is blocked inside the event pump then. False where no such loop exists (X11).
bool inModalSizeLoop();
// Device pixels per logical unit of the window (2 on Retina screens).
double backingScale(const Window& w);
// The X resource database of the window's display (XResourceManagerString:
// "Xft.dpi:\t96\n..."), empty when there is none or off X11 (J-91: the
// desktop's scale setting).
std::string xResources(const Window& w);
// J-111g: the system's mouse pointer, so collaborators' cursors are drawn its size:
// the height of its arrow (the tip to the end of the tail, outline included) in the
// window units of `w` (points on macOS, pixels on Windows and X11, logical pixels on
// Wayland). macOS: the arrow cursor's image (its visible part) times the
// accessibility pointer size; Windows: SM_CYCURSOR; X11: Xcursor.size, XCURSOR_SIZE,
// GNOME's cursor-size, else 24 (core/UiScale.h pointerArrowHeight). `source` names
// where the size came from (reports). Never 0: a default when nothing says.
struct PointerSize {
  double arrowH = 0;
  std::string source;
};
PointerSize pointerSize(const Window& w);
// Window number for screen capture of just this window (macOS), -1 otherwise.
long windowNumber(const Window& w);
// Writes an image of just this window's content to a PNG file: its views
// (the plugin's interface) as they draw themselves, with `overlay` (RGBA,
// overlayW x overlayH device pixels, rows bottom-up as GL reads them) laid
// over the bottom of the content (the GL-drawn control bar). False when
// unsupported.
bool captureWindow(const Window& w, const std::string& pngPath, const unsigned char* overlay, int overlayW, int overlayH);

// ---- keyboard (J-66, see host/KeyForward.h) ----
// Keys a plugin's interface inside `container` did not use queue up for
// Gridline: a key reaching the container through the responder chain (the
// interface passed it on) is queued unless a text field has the keyboard;
// its key-up follows even when the interface swallows key-ups. createContainer
// containers forward from the start; other views (sandbox, plugin build) call
// watchKeys, and their keyDown: / keyUp: hand the event to containerKey.
void watchKeys(void* container);
void unwatchKeys(void* container);
// From a container's keyDown: / keyUp: (NSEvent*); true when queued.
bool containerKey(void* container, void* nsEvent, bool down);
bool popKey(void* container, KeyEvent& e);
// Who has the keyboard inside the container's window.
enum class KeyFocus {
  Host,        // not the plugin's interface (Gridline's own views, the window)
  Plugin,      // a view of the interface (unused keys come through the container)
  PluginText,  // a text field of the interface: every key stays with it
};
KeyFocus keyFocus(void* container);
// The window lost / regained the keyboard: the interface's view that had it
// gets it back (SDL hands key windows' first responder to its own text input
// view, which would take the keyboard away from the plugin).
void saveKeyFocus(void* container);
void restoreKeyFocus(void* container);
// Tests: a view inside `container` like a plugin's (kind 0: passes keys on,
// 1: uses every key, 2: a text field; 3: the interface's own first view that
// accepts it) made first responder; a key event as
// the window system would deliver it. Returns the view / false.
void* devTestKeyView(void* container, int kind);
bool devSendKey(void* container, uint16_t code, bool down, uint32_t mods, const char* chars);

// Main loop pacing (macOS, FrameClockMac.mm): frames are presented without
// vsync and the UI thread waits for the display's next refresh inside the
// Cocoa run loop, so hosted plugin interfaces (timers, display link
// callbacks, AppKit's display pass) run while Gridline idles instead of
// being starved by a blocking vsync swap. frameClockStart returns false
// where unsupported (X11: keep the swap interval). frameClockFollow keeps the
// clock on the display the window is on; frameClockWait sleeps until the
// refresh after the frame just presented.
bool frameClockStart(const Window& w);
void frameClockFollow(const Window& w);
void frameClockWait();
void frameClockStop();
// J-111: while `on` (a collaborative session is open), the system must not
// throttle Gridline in the background (macOS App Nap: timers and the run loop
// slowed when the window is hidden or covered). No-op elsewhere.
void keepAwake(bool on);

// Dev measurement (`--frame-stats`, macOS): drawing done by plugin
// interfaces since the last take. startDrawStats hooks the interface's view
// classes inside `container` (call again after the interface changed).
struct DrawStats {
  long views = 0;         // -drawRect: calls of plugin views
  long layers = 0;        // CALayer -drawInContext: calls (layer-drawn interfaces)
  long gl = 0;            // OpenGL frames presented by plugin contexts
  long metal = 0;         // Metal drawables taken
  long runLoopTurns = 0;  // main run loop iterations
};
void startDrawStats(void* container);
DrawStats takeDrawStats();
// Dev measurement: a mouse drag at (x, y) of the container (top-left based),
// up and down like a knob being turned, for `seconds`, as posted user input.
void devDrag(const Window& w, void* container, int x, int y, double seconds);
// Dev (J-114): one left-button mouse event as the window system would deliver it to `w`
// (kind 0 moved, 1 down, 2 dragged, 3 up) at (x, y) of its content, top-left based, in
// window units; may be outside the window (a drag continuing past its edge). Goes
// through AppKit and SDL's own event handling; the system pointer does not move.
void devPostMouse(const Window& w, int kind, double x, double y);
// Dev (J-116, macOS): the user resizing `w` by its bottom-right corner: a mouse-down on the corner
// followed by `steps` drags (dx, dy window units in all, right / down positive) and the release,
// through AppKit's own live-resize tracking (the system pointer does not move). Returns once the
// resize is over. No-op elsewhere.
void devLiveResize(const Window& w, double dx, double dy, int steps);
// Dev (J-116, macOS): what the window's red close button does (-[NSWindow performClose:]). No-op elsewhere.
void devPerformClose(const Window& w);

}  // namespace gl::host::native

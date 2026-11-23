// Gridline — the native pieces of a plugin window: the view a plugin's
// interface is attached to, stacking above the main window, backing scale.
// macOS: NativeViewMac.mm (AppKit); X11: NativeViewX11.cpp (Xlib, resolved at
// run time from the library SDL already loaded).
#pragma once
#include <string>
#include <vector>
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
// 0.7.8: an installed plugin's interface inside one of Gridline's own windows (its plugin window
// inside the main window, or popped out of it). The container (returned: the plugin attaches to it)
// sits in a clipping view of its own in `w`, at `full` (window content coordinates, top-left based).
// nullptr on failure.
void* createDockedContainer(const Window& w, const layout::Rect& full);
// Shows the part `visible` of the interface whose rectangle is `full`, with `holes` (inside
// `visible`) cut out of it: Gridline's windows, menus, tooltips and notices in front of it.
// `visible` empty hides it. The interface keeps its size and its place in `full`, whatever is cut.
void setDockedFrame(const Window& w, void* container, const layout::Rect& full, const layout::Rect& visible,
                    const std::vector<layout::Rect>& holes);
void destroyDockedContainer(const Window& w, void* container);
// The window system can cut holes in a docked interface (macOS, Windows); elsewhere an interface
// with anything of Gridline's in front of it hides while that is there (layout::dockedShow).
bool dockedHolesCut();
// A mouse button went down on the docked interface since the last call (macOS: its plugin window
// then comes to the front of Gridline's windows). False elsewhere.
bool takeDockedPress(void* container);
// The keyboard goes back from the interface to Gridline's own view of its window (a click on
// Gridline's part of the window while the interface had the keyboard). No-op where the window
// system moves the keyboard itself.
void releaseKeyFocus(void* container);
// K-37: `w` moved on the screen. X11: a window inside another gets no ConfigureNotify of its own when
// its toplevel moves, so the container is sent one, as ICCCM has a window manager do for a toplevel:
// what is embedded in it may watch only the container, and needs to know where it is on the screen
// (yabridge's Wine windows work out mouse positions from this). Elsewhere a no-op.
void containerMoved(const Window& w, void* container);
// Keeps `child` above `parent` (FL-style floating plugin windows): an AppKit
// child window / an X11 transient. `on` = false undoes it before closing.
void keepAbove(const Window& child, const Window& parent, bool on);
// J-114 (macOS): `w`, when shown, ordered just above `ref` without being activated (a detached
// Gridline window kept above the main window when that comes forward). Elsewhere a no-op:
// keepAbove's ownership does it.
void orderAbove(const Window& w, const Window& ref);
// 0.7.8 (macOS): while `on`, `w` cannot become the key window. SDL_ShowWindow makes the window it
// shows the key window on macOS whatever SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN says, and the
// window that loses the keys loses the held mouse button's gesture and every held key with them (SDL
// lets go of all keys when its keyboard focus goes). A window shown between this call with `on` and
// the one with `off` comes to the front and leaves the keys where they are; afterwards a click makes
// it key as usual. Elsewhere a no-op (the hint works there).
void setTakesNoKeys(const Window& w, bool on);
// A click went to `child`, a window kept above `parent` (keepAbove): it comes in front of the other
// windows kept above that parent. macOS: AppKit keeps a parent's child windows in the order they
// were added, whichever one is clicked, so the child is taken off and added again on top. X11 and
// Windows: the window manager / the owned-window rules raise a clicked window already (no-op).
void raiseChild(const Window& child, const Window& parent);
// J-116: the window system runs a modal size / move loop of one of this application's windows
// right now (macOS: a window is in live resize; Windows: the UI thread is in move / size mode).
// The main loop is blocked inside the event pump then. False where no such loop exists (X11).
bool inModalSizeLoop();
// An X server answers on this session's display: a real one or XWayland, which may start now (X11
// systems; false elsewhere). Before SDL starts, to choose its window system (core/VideoDriverChoice.h).
bool xServerAnswers();
// K-153: when Gridline's own windows are Wayland windows (SDL's Wayland driver), a plugin's X11
// interface cannot embed in them; it opens in a window of its own on XWayland instead: a top-level
// X11 window, on an X connection of Gridline's (X11 systems; nullptr elsewhere or with no X server).
// `w` x `h` X pixels; a fixed-size window when not `resizable`. Returns the window id to attach to.
void* sideWindowOpen(const std::string& title, int w, int h, bool resizable);
// Changes its size (the interface asked for it).
void sideWindowResize(void* side, int w, int h, bool resizable);
// Reads the window's events: false once the user closed it; `w`, `h` set to its size when the
// window manager changed it (else left as they were).
bool sideWindowPump(void* side, int& w, int& h);
void sideWindowClose(void* side);
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
// Dev (0.7.8, macOS): a picture of the main window `main` as the window server stacks this
// application's windows over it: its own frame (`rgba`, w x h device pixels, rows bottom-up as GL
// reads them) at the bottom, then every other visible window of the application that is over its
// area (plugin windows, lifted menus and notices), in their real order, as the window server
// composites them. Only this application's windows: never the screen. False when unsupported.
bool captureStack(const Window& main, const unsigned char* rgba, int w, int h, const std::string& pngPath);
// Dev (0.7.8): the window's stacking level (macOS NSWindow level; 0 elsewhere).
long windowLevel(const Window& w);

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
// K-142 (0.7.8): idle pacing. frameClockIdle(true) stops the refresh source while the interface
// idles (no wake every refresh), false starts it again before frames are drawn at the full rate.
// frameClockSleep sleeps the UI thread in its run loop up to `seconds`; it returns early (true)
// when input or another source woke it. frameClockWake wakes it from any thread. Elsewhere these
// do nothing (false): the loop waits on SDL there.
void frameClockIdle(bool idle);
bool frameClockSleep(double seconds);
void frameClockWake();
// K-142: whether any part of the window is on screen (macOS: not covered by other windows, not on
// another Space, the application not hidden). True where the platform cannot tell.
// watchOnScreen wakes the UI thread (frameClockWake) whenever that changes; onScreenChanges counts
// the changes seen.
bool windowOnScreen(const Window& w);
void watchOnScreen(const Window& w);
int onScreenChanges();
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
// Dev (K-106, macOS): Command+W pressed on `w` as AppKit delivers it: the Command key's flags change,
// the W key down (SDL's own key handling first, then the menu bar's key equivalents, as -[NSApp
// sendEvent:] does), the W key up. phase 0 also lets go of Command at once (all four in one batch of
// events, a quick tap), 1 keeps Command held, 2 only lets it go. When the menu's Close item has no
// window to act on (a hidden run: no key window) its action goes to `w`, as it would with `w` key.
// Returns what happened (one line). No-op elsewhere.
std::string devCommandW(const Window& w, int phase);

}  // namespace gl::host::native

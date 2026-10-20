// Gridline — the native pieces of a plugin window: the view a plugin's
// interface is attached to, stacking above the main window, backing scale.
// macOS: NativeViewMac.mm (AppKit); X11: NativeViewX11.cpp (Xlib, resolved at
// run time from the library SDL already loaded).
#pragma once
#include <string>
#include "../EditorLayout.h"

namespace gl::host::native {

// A top-level window as the window system knows it.
struct Window {
  void* display = nullptr;  // X11 Display* (nullptr on macOS)
  void* handle = nullptr;   // NSWindow* / X11 Window id
  bool valid() const { return handle != nullptr; }
};

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
// Device pixels per logical unit of the window (2 on Retina screens).
double backingScale(const Window& w);
// Window number for screen capture of just this window (macOS), -1 otherwise.
long windowNumber(const Window& w);
// Writes an image of just this window's content to a PNG file: its views
// (the plugin's interface) as they draw themselves, with `overlay` (RGBA,
// overlayW x overlayH device pixels, rows bottom-up as GL reads them) laid
// over the bottom of the content (the GL-drawn control bar). False when
// unsupported.
bool captureWindow(const Window& w, const std::string& pngPath, const unsigned char* overlay, int overlayW, int overlayH);

}  // namespace gl::host::native

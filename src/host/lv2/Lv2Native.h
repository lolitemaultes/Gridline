// Gridline — the window-system side of LV2 plugin interfaces: putting a
// CocoaUI's NSView into the host's container view (macOS, Lv2NativeMac.mm),
// or reading / setting the size of an X11UI's child window (Lv2NativeX11.cpp,
// Xlib resolved at run time like ui/NativeViewX11.cpp).
#pragma once

namespace gl::host::lv2::native {

// Makes `widget` (NSView* / X11 Window id) a child of `parent` at the top
// left when the interface did not do so itself. False on failure.
bool embed(void* widget, void* parent);
// Current size of the widget in logical pixels; false when unknown.
bool widgetSize(void* widget, int& w, int& h);
void setWidgetSize(void* widget, int w, int h);
// Takes the widget out of its parent (before the interface is cleaned up).
void remove(void* widget);

}  // namespace gl::host::lv2::native

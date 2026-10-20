// Gridline — LV2 CocoaUI views in plugin windows (see Lv2Native.h).
#import <AppKit/AppKit.h>
#include "Lv2Native.h"

namespace gl::host::lv2::native {

namespace {
// Top left of the container, flipped or not.
void pinTopLeft(NSView* v) {
  NSView* p = [v superview];
  if (!p) return;
  const CGFloat h = [v frame].size.height;
  [v setFrameOrigin:NSMakePoint(0, [p isFlipped] ? 0 : [p bounds].size.height - h)];
}
}  // namespace

bool embed(void* widget, void* parent) {
  if (!widget || !parent) return false;
  NSView* v = (NSView*)widget;
  NSView* p = (NSView*)parent;
  if ([v superview] != p) {
    [v removeFromSuperview];
    [p addSubview:v];
  }
  pinTopLeft(v);
  return true;
}

bool widgetSize(void* widget, int& w, int& h) {
  if (!widget) return false;
  NSSize s = [(NSView*)widget frame].size;
  if (s.width < 1 || s.height < 1) return false;
  w = static_cast<int>(s.width);
  h = static_cast<int>(s.height);
  return true;
}

void setWidgetSize(void* widget, int w, int h) {
  if (!widget) return;
  NSView* v = (NSView*)widget;
  [v setFrameSize:NSMakeSize(w, h)];
  pinTopLeft(v);
}

void remove(void* widget) {
  if (widget) [(NSView*)widget removeFromSuperview];
}

}  // namespace gl::host::lv2::native

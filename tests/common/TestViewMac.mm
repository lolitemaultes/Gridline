// Gridline tests — the test plugins' interface view on macOS (see TestView.h).
// Drawn in drawRect, so window captures of views that are not on screen
// show it too.
#import <AppKit/AppKit.h>
#include <cstdio>
#include <cstdlib>
#include "TestView.h"

#ifndef GRIDLINE_TEST_VIEW_CLASS
#define GRIDLINE_TEST_VIEW_CLASS GridlineTestView
#endif

@interface GRIDLINE_TEST_VIEW_CLASS : NSView {
 @public
  NSColor* fill;
  NSString* label;
}
@end

@implementation GRIDLINE_TEST_VIEW_CLASS
// GRIDLINE_TEST_LOG_MOUSE: the mouse as the interface gets it, for the host's tests (an on-screen
// keyboard played by dragging across it needs every drag, from the press to the release).
static int gDown = 0, gDragged = 0, gUp = 0;
static bool logMouse() { return std::getenv("GRIDLINE_TEST_LOG_MOUSE") != nullptr; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  (void)event;
  return YES;
}
- (void)mouseDown:(NSEvent*)event {
  (void)event;
  gDown++;
  gDragged = 0;
  if (logMouse()) std::fprintf(stderr, "testview mouse down %d\n", gDown);
}
- (void)mouseDragged:(NSEvent*)event {
  (void)event;
  gDragged++;
}
- (void)mouseUp:(NSEvent*)event {
  (void)event;
  gUp++;
  if (logMouse()) std::fprintf(stderr, "testview mouse up %d after %d drags\n", gUp, gDragged);
}
- (void)drawRect:(NSRect)dirty {
  (void)dirty;
  [fill setFill];
  NSRectFill([self bounds]);
  NSDictionary* attrs = @{NSForegroundColorAttributeName : [NSColor whiteColor], NSFontAttributeName : [NSFont systemFontOfSize:14]};
  [label drawAtPoint:NSMakePoint(16, [self bounds].size.height * 0.5 - 8) withAttributes:attrs];
}
- (void)dealloc {
  [fill release];
  [label release];
  [super dealloc];
}
@end

void* gridlineTestViewAttach(void* parent, int w, int h, const char* label, float r, float g, float b) {
  if (!parent) return nullptr;
  GRIDLINE_TEST_VIEW_CLASS* v = [[GRIDLINE_TEST_VIEW_CLASS alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
  v->fill = [[NSColor colorWithCalibratedRed:r green:g blue:b alpha:1] retain];
  v->label = [[NSString alloc] initWithUTF8String:label ? label : ""];
  [(NSView*)parent addSubview:v];
  return v;  // retained: released in gridlineTestViewRemove
}

void gridlineTestViewResize(void* view, int w, int h) {
  if (view) [(NSView*)view setFrameSize:NSMakeSize(w, h)];
}

void gridlineTestViewRemove(void* view) {
  if (!view) return;
  NSView* v = (NSView*)view;
  [v removeFromSuperview];
  [v release];
}

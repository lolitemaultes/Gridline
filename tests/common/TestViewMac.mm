// Gridline tests — the test plugins' interface view on macOS (see TestView.h).
// Drawn in drawRect, so window captures of views that are not on screen
// show it too.
#import <AppKit/AppKit.h>
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

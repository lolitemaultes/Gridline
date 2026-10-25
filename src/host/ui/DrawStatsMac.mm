// Gridline — dev measurement (`--frame-stats`): how often hosted plugin
// interfaces actually draw. Counts, process-wide, the drawing entry points a
// plugin interface uses on macOS: -drawRect: of the view classes found inside
// plugin containers (CoreGraphics interfaces, JUCE's default renderer),
// -[NSOpenGLContext flushBuffer] of contexts that are not Gridline's own (SDL)
// ones, and -[CAMetalLayer nextDrawable] (Metal interfaces). Main run loop
// iterations are counted too: everything an interface does on the main
// thread (timers, repaints, display link callbacks) needs one.
// Only active once startDrawStats() was called (never in normal runs).
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include "NativeView.h"

namespace gl::host::native {

namespace {
std::atomic<long> gViews{0}, gLayers{0}, gGl{0}, gMetal{0}, gTurns{0};
bool gStarted = false;
std::set<Class> gSwizzled;
std::map<Class, IMP> gDrawRect;
std::mutex gLock;  // gDrawRect (main thread only in practice)

void swizzleDrawRect(Class c) {
  if (!c || gSwizzled.count(c)) return;
  gSwizzled.insert(c);
  if (getenv("GRIDLINE_DRAW_STATS_LOG")) std::fprintf(stderr, "[draw-stats] view class %s\n", class_getName(c));
  // Only classes that implement drawRect: themselves.
  unsigned n = 0;
  Method* ms = class_copyMethodList(c, &n);
  Method own = nullptr;
  for (unsigned i = 0; i < n; ++i)
    if (method_getName(ms[i]) == @selector(drawRect:)) own = ms[i];
  free(ms);
  if (!own) return;
  const Class cls = c;
  IMP counted = imp_implementationWithBlock(^(id self, NSRect r) {
    gViews.fetch_add(1, std::memory_order_relaxed);
    IMP o = nullptr;
    {
      std::lock_guard<std::mutex> g(gLock);
      o = gDrawRect[cls];
    }
    if (o) reinterpret_cast<void (*)(id, SEL, NSRect)>(o)(self, @selector(drawRect:), r);
  });
  IMP prev = method_setImplementation(own, counted);
  std::lock_guard<std::mutex> g(gLock);
  gDrawRect[c] = prev;
}

void scan(NSView* v) {
  if (!v) return;
  // The class that implements drawRect: (dynamic subclasses inherit it).
  for (Class k = [v class]; k && k != [NSView class]; k = class_getSuperclass(k)) swizzleDrawRect(k);
  if (getenv("GRIDLINE_DRAW_STATS_LOG")) {
    CALayer* l = [v layer];
    std::fprintf(stderr, "[draw-stats] view %s frame %.0fx%.0f layer %s wantsLayer %d", class_getName([v class]), [v frame].size.width, [v frame].size.height,
                 l ? class_getName([l class]) : "-", [v wantsLayer] ? 1 : 0);
    if ([v respondsToSelector:@selector(isPaused)] && [v respondsToSelector:@selector(preferredFramesPerSecond)])
      std::fprintf(stderr, " paused %d setNeedsDisplay %d fps %ld", [[v valueForKey:@"paused"] boolValue] ? 1 : 0,
                   [[v valueForKey:@"enableSetNeedsDisplay"] boolValue] ? 1 : 0, static_cast<long>([[v valueForKey:@"preferredFramesPerSecond"] longValue]));
    std::fprintf(stderr, "\n");
  }
  for (NSView* s in [v subviews]) scan(s);
}

void swizzleGlobal() {
  {  // NSOpenGLContext -flushBuffer (plugin GL interfaces; SDL's contexts excluded)
    Method m = class_getInstanceMethod(NSClassFromString(@"NSOpenGLContext"), @selector(flushBuffer));
    __block IMP orig = nullptr;
    IMP counted = imp_implementationWithBlock(^(id self) {
      if (strncmp(class_getName([self class]), "SDL", 3) != 0) gGl.fetch_add(1, std::memory_order_relaxed);
      reinterpret_cast<void (*)(id, SEL)>(orig)(self, @selector(flushBuffer));
    });
    if (m) orig = method_setImplementation(m, counted);
  }
  {  // CALayer -drawInContext: (layer-drawn interfaces: VSTGUI, layer delegates)
    Method m = class_getInstanceMethod(NSClassFromString(@"CALayer"), @selector(drawInContext:));
    __block IMP orig = nullptr;
    IMP counted = imp_implementationWithBlock(^(id self, CGContextRef c) {
      gLayers.fetch_add(1, std::memory_order_relaxed);
      reinterpret_cast<void (*)(id, SEL, CGContextRef)>(orig)(self, @selector(drawInContext:), c);
    });
    if (m) orig = method_setImplementation(m, counted);
  }
  {  // CAMetalLayer -nextDrawable (one per Metal frame)
    Method m = class_getInstanceMethod(NSClassFromString(@"CAMetalLayer"), @selector(nextDrawable));
    __block IMP orig = nullptr;
    IMP counted = imp_implementationWithBlock(^id(id self) {
      gMetal.fetch_add(1, std::memory_order_relaxed);
      return reinterpret_cast<id (*)(id, SEL)>(orig)(self, @selector(nextDrawable));
    });
    if (m) orig = method_setImplementation(m, counted);
  }
  CFRunLoopObserverRef o = CFRunLoopObserverCreateWithHandler(kCFAllocatorDefault, kCFRunLoopAfterWaiting | kCFRunLoopBeforeSources, true, 0,
                                                              ^(CFRunLoopObserverRef, CFRunLoopActivity) { gTurns.fetch_add(1, std::memory_order_relaxed); });
  CFRunLoopAddObserver(CFRunLoopGetMain(), o, kCFRunLoopCommonModes);
  CFRelease(o);
}
}  // namespace

void startDrawStats(void* container) {
  if (!gStarted) {
    gStarted = true;
    swizzleGlobal();
  }
  scan(static_cast<NSView*>(container));
}

void devDrag(const Window& w, void* container, int x, int y, double seconds) {
  if (!w.handle || !container) return;
  NSWindow* win = (NSWindow*)w.handle;
  NSView* c = (NSView*)container;
  // Container point (top-left based) -> window coordinates.
  const NSRect f = [c frame];
  const NSPoint p0 = [[c superview] convertPoint:NSMakePoint(f.origin.x + x, f.origin.y + f.size.height - y) toView:nil];
  auto post = [win](NSEventType t, NSPoint p) {
    NSEvent* e = [NSEvent mouseEventWithType:t
                                    location:p
                               modifierFlags:0
                                   timestamp:[[NSProcessInfo processInfo] systemUptime]
                                windowNumber:[win windowNumber]
                                     context:nil
                                 eventNumber:0
                                  clickCount:1
                                    pressure:t == NSEventTypeLeftMouseUp ? 0.0f : 1.0f];
    [NSApp postEvent:e atStart:NO];
  };
  post(NSEventTypeLeftMouseDown, p0);
  const double start = [[NSProcessInfo processInfo] systemUptime];
  // Common modes: also fires inside an interface's own mouse tracking loop.
  NSTimer* t = [NSTimer timerWithTimeInterval:1.0 / 120.0
                                      repeats:YES
                                        block:^(NSTimer* timer) {
                                          const double el = [[NSProcessInfo processInfo] systemUptime] - start;
                                          // Up and down by 60 px, twice a second (a knob being turned).
                                          const double ph = std::fmod(el * 2.0, 2.0);
                                          const double dy = 60.0 * (ph < 1.0 ? ph : 2.0 - ph);
                                          const NSPoint p = NSMakePoint(p0.x, p0.y + dy);
                                          if (el >= seconds) {
                                            post(NSEventTypeLeftMouseUp, p);
                                            [timer invalidate];
                                          } else {
                                            post(NSEventTypeLeftMouseDragged, p);
                                          }
                                        }];
  [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
}

void devPostMouse(const Window& w, int kind, double x, double y) {
  if (!w.handle) return;
  NSWindow* win = (NSWindow*)w.handle;
  const NSRect content = [win contentRectForFrameRect:[win frame]];
  const NSPoint p = NSMakePoint(x, content.size.height - y);  // window base coordinates (bottom-left)
  const NSEventType t = kind == 1 ? NSEventTypeLeftMouseDown : kind == 2 ? NSEventTypeLeftMouseDragged : kind == 3 ? NSEventTypeLeftMouseUp : NSEventTypeMouseMoved;
  NSEvent* e = [NSEvent mouseEventWithType:t
                                  location:p
                             modifierFlags:0
                                 timestamp:[[NSProcessInfo processInfo] systemUptime]
                              windowNumber:[win windowNumber]
                                   context:nil
                               eventNumber:0
                                clickCount:kind == 1 || kind == 3 ? 1 : 0
                                  pressure:kind == 1 || kind == 2 ? 1.0f : 0.0f];
  [win sendEvent:e];  // dispatched now, like AppKit does for the window under (or holding) the pointer
}

void devLiveResize(const Window& w, double dx, double dy, int steps) {
  if (!w.handle || steps <= 0) return;
  NSWindow* win = (NSWindow*)w.handle;
  const NSRect f = [win frame];
  const NSPoint start = NSMakePoint(f.size.width - 1, 1);  // the bottom-right corner, window base coordinates
  const double t0 = [[NSProcessInfo processInfo] systemUptime];
  auto make = [&](NSEventType t, NSPoint p, double at, NSInteger number) {
    return [NSEvent mouseEventWithType:t
                              location:p
                         modifierFlags:0
                             timestamp:at
                          windowNumber:number
                               context:nil
                           eventNumber:0
                            clickCount:t == NSEventTypeLeftMouseDragged ? 0 : 1
                              pressure:t == NSEventTypeLeftMouseUp ? 0.0f : 1.0f];
  };
  // The mouse-down enters AppKit's resize tracking on this thread (it returns on the release);
  // the drags and the release arrive meanwhile, ~60 per second like a hand-held mouse, posted
  // from another thread (-postEvent:atStart: may be called from any thread; queued all at once
  // AppKit would coalesce them into one step).
  NSMutableArray* events = [NSMutableArray array];
  // Window base coordinates, read against the window's frame when AppKit takes each drag: the
  // right edge stays at the window's x origin, the bottom edge moves the origin along with the
  // pointer, so each drag lies one step below the (moved) bottom edge.
  NSPoint p = start;
  for (int k = 1; k <= steps; ++k) {
    p = NSMakePoint(start.x + dx * k / steps, start.y - dy / steps);
    [events addObject:make(NSEventTypeLeftMouseDragged, p, t0 + 0.016 * k, [win windowNumber])];
  }
  [events addObject:make(NSEventTypeLeftMouseUp, p, t0 + 0.016 * (steps + 1), [win windowNumber])];
  [events retain];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
    for (NSEvent* e in events) {
      usleep(16000);
      [NSApp postEvent:e atStart:NO];
    }
    [events release];
  });
  [win sendEvent:make(NSEventTypeLeftMouseDown, start, t0, [win windowNumber])];
}

void devPerformClose(const Window& w) {
  if (w.handle) [(NSWindow*)w.handle performClose:nil];
}

DrawStats takeDrawStats() {
  DrawStats s;
  s.views = gViews.exchange(0);
  s.layers = gLayers.exchange(0);
  s.gl = gGl.exchange(0);
  s.metal = gMetal.exchange(0);
  s.runLoopTurns = gTurns.exchange(0);
  return s;
}

}  // namespace gl::host::native

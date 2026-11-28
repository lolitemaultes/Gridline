// Gridline — K-410: native macOS trackpad gesture capture (NSEvent).
//
// Captures native NSEventTypeMagnify (smooth continuous pinch magnification),
// NSEventTypeSmartMagnify (two-finger double-tap smart-zoom), and precise
// two-finger NSEventTypeScrollWheel deltas (hasPreciseScrollingDeltas == YES)
// via a local AppKit event monitor without swizzling or retain cycles.
//
// Guards:
//  - Manual reference counting (MRC), matching the rest of host/ui/*.mm.
//  - Zero retain cycles: the monitor block captures no Objective-C objects.
//  - Disabled cleanly in quiet/hidden test runs (quiet::on()) so real hardware
//    gestures never interfere with automated tests.
//  - Multi-window aware: checks main window and detached Gridline viewports
//    via WindowOffsetFn; leaves hosted third-party plugin windows untouched.
#import <AppKit/AppKit.h>
#include <cmath>
#include "../../core/TrackpadGesture.h"
#include "QuietWindows.h"

namespace gl::gesture::native {

namespace {

NSWindow* gMainWindow = nil;
id gMonitor = nil;
id gCloseObserver = nil;
WakeFn gWake = nullptr;
WindowOffsetFn gWindowOffset = nullptr;
bool gInstalled = false;

Phase phaseFromNS(NSEventPhase p) {
  if (p & NSEventPhaseBegan) return Phase::Began;
  if (p & NSEventPhaseChanged) return Phase::Changed;
  if (p & NSEventPhaseEnded) return Phase::Ended;
  if (p & NSEventPhaseCancelled) return Phase::Cancelled;
  return Phase::None;
}

bool resolveWindow(NSWindow* win, float& offX, float& offY, uint32_t& winId) {
  if (!win || ![win isVisible]) return false;
  if (win == gMainWindow) {
    offX = 0.0f;
    offY = 0.0f;
    winId = 0;
    return true;
  }
  if (gWindowOffset && gWindowOffset(static_cast<void*>(win), offX, offY, winId)) {
    return true;
  }
  return false;
}

bool eventPoint(NSEvent* ev, NSWindow* win, float offX, float offY, float& outX, float& outY) {
  NSView* cv = [win contentView];
  if (!cv) return false;
  const NSPoint local = [cv convertPoint:[ev locationInWindow] fromView:nil];
  outX = offX + static_cast<float>(local.x);
  outY = offY + static_cast<float>(cv.isFlipped ? local.y : cv.bounds.size.height - local.y);
  return std::isfinite(outX) && std::isfinite(outY);
}

NSEvent* handleEvent(NSEvent* ev) {
  if (!ev || !gl::gesture::enabled() || gl::quiet::on()) return ev;
  NSWindow* win = [ev window];
  float offX = 0.0f, offY = 0.0f;
  uint32_t winId = 0;
  if (!resolveWindow(win, offX, offY, winId)) return ev;

  const NSEventType type = [ev type];
  if (type == NSEventTypeMagnify) {
    float x = 0.0f, y = 0.0f;
    if (!eventPoint(ev, win, offX, offY, x, y)) return ev;
    TrackpadGesture g;
    g.kind = Kind::PinchZoom;
    g.phase = phaseFromNS([ev phase]);
    g.x = x;
    g.y = y;
    g.magnification = static_cast<float>([ev magnification]);
    g.precise = true;
    g.logical = false;
    g.windowId = winId;
    post(g);
    if (gWake) gWake();
    return nil;  // consumed cleanly; avoids AppKit default beep
  }

  if (type == NSEventTypeSmartMagnify) {
    float x = 0.0f, y = 0.0f;
    if (!eventPoint(ev, win, offX, offY, x, y)) return ev;
    TrackpadGesture g;
    g.kind = Kind::SmartZoom;
    g.phase = Phase::Ended;
    g.x = x;
    g.y = y;
    g.precise = true;
    g.logical = false;
    g.windowId = winId;
    post(g);
    if (gWake) gWake();
    return nil;
  }

  if (type == NSEventTypeScrollWheel) {
    // Only capture genuine smooth trackpad / Magic Mouse two-finger scrolls;
    // discrete mouse wheel clicks stay on the standard SDL_MOUSEWHEEL path.
    if (![ev hasPreciseScrollingDeltas]) return ev;
    // Modifier-held scrolls (Ctrl/Cmd/Alt) stay on the existing wheel-zoom path.
    const NSEventModifierFlags mods = [ev modifierFlags];
    if ((mods & (NSEventModifierFlagControl | NSEventModifierFlagCommand | NSEventModifierFlagOption)) != 0) return ev;

    float x = 0.0f, y = 0.0f;
    if (!eventPoint(ev, win, offX, offY, x, y)) return ev;
    const float dx = -static_cast<float>([ev scrollingDeltaX]);
    const float dy = -static_cast<float>([ev scrollingDeltaY]);
    if (dx != 0.0f || dy != 0.0f) {
      TrackpadGesture g;
      g.kind = Kind::Pan2D;
      const Phase ph = phaseFromNS([ev phase]);
      g.phase = ph != Phase::None ? ph : phaseFromNS([ev momentumPhase]);
      g.x = x;
      g.y = y;
      g.dx = dx;
      g.dy = dy;
      g.precise = true;
      g.logical = false;
      g.windowId = winId;
      post(g);
      if (gWake) gWake();
    }
    // Keep returning `ev` so ImGui scroll panels (Browser, Mixer, Settings,
    // menus) keep scrolling naturally; gesture canvases consume Pan2D and skip
    // io.MouseWheel on that frame so there is zero double-scrolling.
    return ev;
  }

  return ev;
}

}  // namespace

bool install(void* mainNativeWindow, WakeFn wake, WindowOffsetFn windowOffset) {
  if (gInstalled) return true;
  if (gl::quiet::on()) return false;
  NSWindow* w = static_cast<NSWindow*>(mainNativeWindow);
  if (!w) return false;

  @autoreleasepool {
    gMainWindow = w;
    gWake = wake;
    gWindowOffset = windowOffset;
    const NSEventMask mask = NSEventMaskMagnify | NSEventMaskSmartMagnify | NSEventMaskScrollWheel;
    gMonitor = [[NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                      handler:^NSEvent*(NSEvent* ev) { return handleEvent(ev); }] retain];
    if (!gMonitor) {
      gMainWindow = nil;
      gWake = nullptr;
      gWindowOffset = nullptr;
      return false;
    }
    gCloseObserver = [[[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
                                                                        object:w
                                                                         queue:nil
                                                                    usingBlock:^(NSNotification*) { gMainWindow = nil; }] retain];
    gInstalled = true;
  }
  return true;
}

void uninstall() {
  if (!gInstalled) return;
  @autoreleasepool {
    if (gMonitor) {
      [NSEvent removeMonitor:gMonitor];
      [gMonitor release];
      gMonitor = nil;
    }
    if (gCloseObserver) {
      [[NSNotificationCenter defaultCenter] removeObserver:gCloseObserver];
      [gCloseObserver release];
      gCloseObserver = nil;
    }
    gMainWindow = nil;
    gWake = nullptr;
    gWindowOffset = nullptr;
    gInstalled = false;
  }
}

bool installed() { return gInstalled; }

}  // namespace gl::gesture::native

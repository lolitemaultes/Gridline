// Gridline — the sandbox child's editor window (macOS): one plain Cocoa
// window whose content view hosts the plugin's editor.
#import <AppKit/AppKit.h>
#include <poll.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "../ui/NativeView.h"
#include "Sandbox.h"

namespace {
NSWindow* gWindow = nil;
void* gContainer = nullptr;  // the view the editor attaches to (keys pass through it, J-66)
bool gOpen = false;
id gCloseObserver = nil;

void ensureApp() {
  static bool started = false;
  if (started) return;
  started = true;
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp finishLaunching];
}
}  // namespace

namespace gl::host {

namespace {
// Height of the screen whose top-left corner is the origin of top-left based
// global coordinates (the main / menu bar screen).
CGFloat originScreenHeight() {
  NSArray<NSScreen*>* screens = [NSScreen screens];
  return [screens count] > 0 ? [[screens objectAtIndex:0] frame].size.height : 0;
}
}  // namespace

bool sandboxWindowFrame(int& x, int& y, int& w, int& h) {
  if (!gWindow || !gOpen) return false;
  NSRect f = [gWindow frame];
  x = static_cast<int>(f.origin.x);
  y = static_cast<int>(originScreenHeight() - f.origin.y - f.size.height);
  w = static_cast<int>(f.size.width);
  h = static_cast<int>(f.size.height);
  return true;
}

void* sandboxOpenWindow(const std::string& title, int w, int h, bool resizable, int x, int y) {
  ensureApp();
  sandboxCloseWindow();
  NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
  if (resizable) style |= NSWindowStyleMaskResizable;
  gWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h) styleMask:style backing:NSBackingStoreBuffered defer:NO];
  [gWindow setReleasedWhenClosed:NO];
  [gWindow setTitle:[NSString stringWithUTF8String:title.c_str()]];
  gCloseObserver = [[[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
                                                                      object:gWindow
                                                                       queue:nil
                                                                  usingBlock:^(NSNotification*) { gOpen = false; }] retain];
  if (x == gl::host::Editor::kNoPlace || y == gl::host::Editor::kNoPlace) {
    [gWindow center];
  } else {  // frame top-left at (x, y), top-left based
    [gWindow setFrameTopLeftPoint:NSMakePoint(x, originScreenHeight() - y)];
  }
  // The editor's parent: Gridline's plugin container filling the window (keys
  // the interface does not use come back through it).
  native::Window nw;
  nw.handle = gWindow;
  gContainer = native::createContainer(nw, layout::Rect{0, 0, w, h});
  if (gContainer) [(NSView*)gContainer setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [gWindow makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  gOpen = true;
  return gContainer ? gContainer : [gWindow contentView];
}

bool sandboxPopKey(KeyEvent& e) { return gContainer && native::popKey(gContainer, e); }

void sandboxDevStats(int idles) {
  static const bool on = std::getenv("GRIDLINE_SANDBOX_STATS") != nullptr;
  if (!on || !gContainer) return;
  static double last = 0;
  if (idles % 30 == 1) native::startDrawStats(gContainer);
  if (idles == 240)
    if (const char* d = std::getenv("GRIDLINE_DEV_DRAG")) {
      int x = 0, y = 0;
      double s = 0;
      native::Window nw;
      nw.handle = gWindow;
      if (std::sscanf(d, "%d,%d,%lf", &x, &y, &s) == 3) native::devDrag(nw, gContainer, x, y, s);
    }
  const double now = CFAbsoluteTimeGetCurrent();
  if (last == 0) {
    last = now;
    native::takeDrawStats();
    return;
  }
  if (now - last < 1.0) return;
  const native::DrawStats d = native::takeDrawStats();
  const double span = now - last;
  std::fprintf(stderr, "[sandbox-stats] plugin ui/s: views %.0f layers %.0f gl %.0f metal %.0f | runloop %.0f/s\n", d.views / span, d.layers / span,
               d.gl / span, d.metal / span, d.runLoopTurns / span);
  last = now;
}

void sandboxDevKeys() {
  if (!gContainer) return;
  if (!native::devTestKeyView(gContainer, 3)) native::devTestKeyView(gContainer, 0);
  native::devSendKey(gContainer, 0x31, true, 0, " ");
  native::devSendKey(gContainer, 0x31, false, 0, " ");
}

bool sandboxWaitEvents(int fd, double seconds) {
  auto ready = [fd]() {
    pollfd pf{fd, POLLIN, 0};
    return ::poll(&pf, 1, 0) > 0;
  };
  sandboxPumpEvents();
  if (ready()) return true;
  // The parent's calls wake the run loop through a CFFileDescriptor source.
  static CFFileDescriptorRef src = nullptr;
  static int srcFd = -1;
  if (srcFd != fd) {
    if (src) {
      CFFileDescriptorInvalidate(src);
      CFRelease(src);
    }
    src = CFFileDescriptorCreate(kCFAllocatorDefault, fd, false, [](CFFileDescriptorRef, CFOptionFlags, void*) {}, nullptr);
    srcFd = fd;
    if (src) {
      CFRunLoopSourceRef rls = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, src, 0);
      CFRunLoopAddSource(CFRunLoopGetMain(), rls, kCFRunLoopDefaultMode);
      CFRelease(rls);
    }
  }
  if (src) CFFileDescriptorEnableCallBacks(src, kCFFileDescriptorReadCallBack);  // one-shot: armed per wait
  @autoreleasepool {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
  }
  sandboxPumpEvents();
  return ready();
}

void sandboxResizeWindow(int w, int h) {
  if (gWindow) [gWindow setContentSize:NSMakeSize(w, h)];
}

bool sandboxWindowOpen() { return gWindow != nil && gOpen; }

void sandboxCloseWindow() {
  if (gCloseObserver) {
    [[NSNotificationCenter defaultCenter] removeObserver:gCloseObserver];
    [gCloseObserver release];
    gCloseObserver = nil;
  }
  if (gContainer) {
    native::Window nw;
    nw.handle = gWindow;
    native::destroyContainer(nw, gContainer);
    gContainer = nullptr;
  }
  if (gWindow) {
    if (gOpen) [gWindow close];
    [gWindow release];
    gWindow = nil;
  }
  gOpen = false;
}

// Plugin checks: a window that never appears on screen (far off any
// display, never ordered front) whose content view parents an editor.
namespace {
NSWindow* gOffscreen = nil;
}
void* offscreenEditorParent(int w, int h) {
  static bool started = false;
  if (!started) {
    started = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
  }
  closeOffscreenEditorParent();
  // On the main screen's area (editors ask their window for its screen and
  // its backing scale) but fully transparent, click-through and never
  // ordered in: nothing is ever drawn on the display.
  NSRect screen = [[NSScreen mainScreen] frame];
  gOffscreen = [[NSWindow alloc] initWithContentRect:NSMakeRect(screen.origin.x, screen.origin.y, std::max(64, w), std::max(48, h))
                                           styleMask:NSWindowStyleMaskBorderless
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
  [gOffscreen setReleasedWhenClosed:NO];
  [gOffscreen setAlphaValue:0.0];
  [gOffscreen setIgnoresMouseEvents:YES];
  [gOffscreen setOpaque:NO];
  return [gOffscreen contentView];
}
void closeOffscreenEditorParent() {
  if (!gOffscreen) return;
  [gOffscreen close];
  [gOffscreen release];
  gOffscreen = nil;
}
void pumpOffscreenEvents(double seconds) {
  @autoreleasepool {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    for (;;) {
      NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
      if (!ev) break;
      [NSApp sendEvent:ev];
    }
  }
}

void sandboxPumpEvents() {
  if (!gWindow) return;
  @autoreleasepool {
    for (;;) {
      NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES];
      if (!ev) break;
      [NSApp sendEvent:ev];
    }
  }
}

}  // namespace gl::host

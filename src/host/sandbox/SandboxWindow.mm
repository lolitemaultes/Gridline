// Gridline — the sandbox child's editor window (macOS): one plain Cocoa
// window whose content view hosts the plugin's editor.
#import <AppKit/AppKit.h>
#include <algorithm>
#include "Sandbox.h"

namespace {
NSWindow* gWindow = nil;
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
  [gWindow makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  gOpen = true;
  return [gWindow contentView];
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

// Gridline — quiet windows on macOS (host/ui/QuietWindows.h):
//   1. off (no GRIDLINE_QUIET_WINDOWS, or "0"): install() changes nothing in AppKit, so a normal run
//      of Gridline behaves exactly as before (no window is made in this part);
//   2. on: windows ordered in are on the window server's list but fully transparent, wherever they
//      are; the code still reads the alpha and the mouse setting it asked for; the app never
//      becomes active (prohibited, every activation request dropped) and the key window is kept
//      inside the app: makeKeyAndOrderFront / closing move it, with the notifications SDL and
//      window delegates listen to.
// A window is only ever ordered in after the check that the guards are in place passed.
#import <AppKit/AppKit.h>
#include <objc/runtime.h>
#include <cstdlib>
#include <string>
#include "Check.h"
#include "host/ui/QuietWindows.h"

using namespace gl;

@interface GLQuietKeyWatch : NSObject
@property(nonatomic) int became;
@property(nonatomic) int resigned;
@end
@implementation GLQuietKeyWatch
- (void)windowDidBecomeKey:(NSNotification*)n {
  (void)n;
  ++self.became;
}
- (void)windowDidResignKey:(NSNotification*)n {
  (void)n;
  ++self.resigned;
}
@end

namespace {
IMP impOf(Class c, SEL s) {
  Method m = class_getInstanceMethod(c, s);
  return m ? method_getImplementation(m) : nullptr;
}
struct Imps {
  IMP order, orderRegardless, makeKey, isKey, alpha, appKey, activate, policy;
  bool operator==(const Imps& o) const {
    return order == o.order && orderRegardless == o.orderRegardless && makeKey == o.makeKey && isKey == o.isKey && alpha == o.alpha &&
           appKey == o.appKey && activate == o.activate && policy == o.policy;
  }
};
Imps imps() {
  Class W = [NSWindow class], A = [NSApplication class];
  return {impOf(W, @selector(orderWindow:relativeTo:)), impOf(W, @selector(orderFrontRegardless)), impOf(W, @selector(makeKeyAndOrderFront:)),
          impOf(W, @selector(isKeyWindow)),         impOf(W, @selector(alphaValue)),           impOf(A, @selector(keyWindow)),
          impOf(A, @selector(activateIgnoringOtherApps:)), impOf(A, @selector(setActivationPolicy:))};
}
void pump(double seconds) {
  NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
  for (;;) {
    NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
    if (!e) break;
    [NSApp sendEvent:e];
  }
}
// The window server's alpha of a window (-1: not on its list of windows on screen).
double serverAlpha(NSWindow* w) {
  double a = -1;
  CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
  for (NSDictionary* d in (NSArray*)list)
    if ([[d objectForKey:(id)kCGWindowNumber] longValue] == [w windowNumber]) a = [[d objectForKey:(id)kCGWindowAlpha] doubleValue];
  if (list) CFRelease(list);
  return a;
}
// The same once the window server lists the window: ordering a window in reaches the server
// asynchronously, so a slow (shared) machine may list it only some time after the call returns.
// Waits for the listing (up to `seconds`), then reads it: the first alpha the server shows.
double serverAlphaListed(NSWindow* w, double seconds = 10.0) {
  NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
  double a = serverAlpha(w);
  while (a < 0 && [until timeIntervalSinceNow] > 0) {
    pump(0.02);
    a = serverAlpha(w);
  }
  if (a != 0.0) std::printf("# window \"%s\": the window server's alpha %.2f (-1: not listed)\n", [[w title] UTF8String], a);
  return a;
}
NSWindow* makeWindow(NSRect r, const char* title) {
  NSWindow* w = [[NSWindow alloc] initWithContentRect:r
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
  [w setReleasedWhenClosed:NO];
  [w setTitle:[NSString stringWithUTF8String:title]];
  return w;
}
}  // namespace

int main() {
  @autoreleasepool {
    // ---- 1. off: nothing changes ----
    unsetenv("GRIDLINE_QUIET_WINDOWS");
    const Imps before = imps();
    expect(!quiet::on(), "off without the variable");
    quiet::install();
    expect(!quiet::installed() && imps() == before, "off: install() leaves AppKit as it is");
    setenv("GRIDLINE_QUIET_WINDOWS", "0", 1);
    expect(!quiet::on(), "off with the variable set to 0");
    quiet::enableForHiddenRun();
    expect(!quiet::on() && std::string(std::getenv("GRIDLINE_QUIET_WINDOWS")) == "0", "a --hidden run keeps an explicit 0 (windows shown)");
    quiet::install();
    expect(!quiet::installed() && imps() == before, "0: install() leaves AppKit as it is");
    unsetenv("GRIDLINE_QUIET_WINDOWS");
    unsetenv("SDL_MAC_BACKGROUND_APP");
    quiet::enableForHiddenRun();
    expect(quiet::on(), "a --hidden run turns it on (for the processes it starts too)");

    // ---- 2. on ----
    quiet::install();
    const Imps after = imps();
    const bool guarded = quiet::installed() && after.order != before.order && after.orderRegardless != before.orderRegardless &&
                         after.makeKey != before.makeKey && after.isKey != before.isKey && after.appKey != before.appKey &&
                         after.activate != before.activate && after.policy != before.policy;
    expect(guarded, "on: the guards are in place");
    const char* bg = std::getenv("SDL_MAC_BACKGROUND_APP");
    expect(bg && std::string(bg) == "1", "on: SDL's start-up activation is off");
    if (!guarded) return finish();  // never order a window in without them

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];  // what SDL / the sandbox ask for
    [NSApp finishLaunching];
    expect([NSApp activationPolicy] == NSApplicationActivationPolicyProhibited, "the app stays prohibited (no Dock icon, never active)");

    GLQuietKeyWatch* watch = [[GLQuietKeyWatch alloc] init];
    NSWindow* a = makeWindow(NSMakeRect(200, 200, 320, 200), "quiet A");
    [a setDelegate:(id<NSWindowDelegate>)watch];
    [a makeKeyAndOrderFront:nil];
    pump(0.3);
    expect([a isVisible], "a window is ordered in");
    expect(serverAlphaListed(a) == 0.0, "the window server shows it fully transparent");
    expect([a alphaValue] == 1.0 && ![a ignoresMouseEvents], "the code still reads the alpha and mouse setting it asked for");
    expect([a isKeyWindow] && [NSApp keyWindow] == a && watch.became == 1, "it is the key window inside the app (delegate told)");
    [a setAlphaValue:0.5];
    expect([a alphaValue] == 0.5 && serverAlpha(a) == 0.0, "a later alpha is kept for the code, the screen stays clear");

    NSWindow* b = makeWindow(NSMakeRect(260, 260, 320, 200), "quiet B");
    [b orderFront:nil];
    pump(0.1);
    expect(serverAlphaListed(b) == 0.0 && ![b isKeyWindow] && [a isKeyWindow], "ordered front without the keys: the keys stay");
    [b makeKeyWindow];
    expect([b isKeyWindow] && ![a isKeyWindow] && watch.resigned == 1, "the keys move to another window (the first told)");
    [b close];
    pump(0.1);
    expect([a isKeyWindow] && watch.became == 2, "closing the key window gives the keys to the one in front");
    [a orderFrontRegardless];
    expect(serverAlpha(a) == 0.0, "orderFrontRegardless: still clear");

    // Child windows (plugin windows above the main window) and the screen rectangle are untouched.
    NSWindow* c = makeWindow(NSMakeRect(300, 300, 200, 120), "quiet child");
    [a addChildWindow:c ordered:NSWindowAbove];
    pump(0.1);
    expect([c parentWindow] == a && [c isVisible] && serverAlphaListed(c) == 0.0, "a child window is ordered in, clear");
    expect(NSEqualRects([c frame], [c frameRectForContentRect:NSMakeRect(300, 300, 200, 120)]), "where the code put it");

    [NSApp activateIgnoringOtherApps:YES];
    [[NSRunningApplication currentApplication] activateWithOptions:NSApplicationActivateAllWindows];
    pump(0.3);
    const quiet::Check k = quiet::check();
    std::printf("# %s\n", quiet::describe(k).c_str());
    expect(k.blockedActivations >= 2, "requests to activate are dropped");
    expect(!k.activeNow && k.becameActive == 0 && k.ownActivations == 0, "the app never became active");
    expect(k.onScreen >= 2 && k.visible == 0, "its windows are on the window server's list, none visible");
    expect(k.policy == "prohibited" && k.keyWindow == "quiet A", "the check reports the policy and the key window");
    expect(k.windowsQuieted >= 3, "every window ordered in was counted");

    [a removeChildWindow:c];
    [c close];
    [a close];
    [c release];
    [b release];
    [a release];
    [watch release];
  }
  return finish();
}

// Gridline — quiet windows on macOS (QuietWindows.h). Manual reference counting, like the rest of
// the project's Objective-C++.
//
// Every method replaced here is replaced only by install(), which does nothing unless
// GRIDLINE_QUIET_WINDOWS is on: a normal run of Gridline (and a plugin bundle in another host,
// which links this code too) keeps AppKit exactly as it is.
#import <AppKit/AppKit.h>
#include <libproc.h>
#include <objc/runtime.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "QuietWindows.h"

namespace gl::quiet {

namespace {

bool gInstalled = false;
int gBecameActive = 0;
int gOwnActivations = 0;
int gBlocked = 0;
int gQuieted = 0;
NSWindow* gKey = nil;  // the window with the keyboard inside the app (retained)

// Associated-object keys: the values the code set (the real ones stay 0 / YES once quieted).
char kQuietedKey, kAlphaKey, kIgnoresKey;

// The original implementations.
void (*oOrderWindow)(id, SEL, NSWindowOrderingMode, NSInteger) = nullptr;
void (*oOrderFrontRegardless)(id, SEL) = nullptr;
void (*oMakeKeyWindow)(id, SEL) = nullptr;
void (*oMakeKeyAndOrderFront)(id, SEL, id) = nullptr;
void (*oClose)(id, SEL) = nullptr;
BOOL (*oIsKeyWindow)(id, SEL) = nullptr;
void (*oSetAlpha)(id, SEL, CGFloat) = nullptr;
CGFloat (*oAlpha)(id, SEL) = nullptr;
void (*oSetIgnores)(id, SEL, BOOL) = nullptr;
BOOL (*oIgnores)(id, SEL) = nullptr;
id (*oAppKeyWindow)(id, SEL) = nullptr;
BOOL (*oSetPolicy)(id, SEL, NSApplicationActivationPolicy) = nullptr;

// Replaces `sel` of `cls` (an inherited method gets an override in `cls`), returning the old one.
IMP replace(Class cls, SEL sel, IMP mine) {
  Method m = class_getInstanceMethod(cls, sel);
  if (!m) return nullptr;
  const IMP orig = method_getImplementation(m);
  if (!class_addMethod(cls, sel, mine, method_getTypeEncoding(m))) method_setImplementation(m, mine);
  return orig;
}

void logLine(const std::string& line) {
  const char* path = std::getenv("GRIDLINE_QUIET_WINDOWS_LOG");
  if (!path || !*path) return;
  if (FILE* f = std::fopen(path, "a")) {
    const char* cfg = std::getenv("GRIDLINE_CONFIG_HOME");  // (a test's own folder: which test it was)
    std::fprintf(f, "%d %s %s | %s\n", static_cast<int>(getpid()), getprogname(), line.c_str(), cfg ? cfg : "-");
    std::fclose(f);
  }
}

std::string titleOf(NSWindow* w) {
  NSString* t = [w title];
  std::string s = t ? std::string([t UTF8String] ? [t UTF8String] : "") : std::string();
  if (s.empty()) s = class_getName([w class]);
  return s;
}

bool isQuieted(NSWindow* w) { return objc_getAssociatedObject(w, &kQuietedKey) != nil; }

// The window goes fully transparent and lets the mouse through before it is ordered in.
void quietLook(NSWindow* w) {
  if (!w) return;
  if (!isQuieted(w)) {
    if (!objc_getAssociatedObject(w, &kAlphaKey))
      objc_setAssociatedObject(w, &kAlphaKey, [NSNumber numberWithDouble:oAlpha(w, @selector(alphaValue))], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (!objc_getAssociatedObject(w, &kIgnoresKey))
      objc_setAssociatedObject(w, &kIgnoresKey, [NSNumber numberWithBool:oIgnores(w, @selector(ignoresMouseEvents))], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(w, &kQuietedKey, [NSNumber numberWithBool:YES], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    ++gQuieted;
    const NSRect f = [w frame];
    logLine("quieted window \"" + titleOf(w) + "\" " + std::to_string(static_cast<int>(f.size.width)) + "x" +
            std::to_string(static_cast<int>(f.size.height)));
  }
  if (oAlpha(w, @selector(alphaValue)) != 0.0) oSetAlpha(w, @selector(setAlphaValue:), 0.0);
  if (!oIgnores(w, @selector(ignoresMouseEvents))) oSetIgnores(w, @selector(setIgnoresMouseEvents:), YES);
}

// ---- the key window, kept by the app --------------------------------------------------------
void setKey(NSWindow* w) {
  if (w == gKey) return;
  NSWindow* old = gKey;
  gKey = [w retain];
  NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
  if (old) [nc postNotificationName:NSWindowDidResignKeyNotification object:old];
  if (w) [nc postNotificationName:NSWindowDidBecomeKeyNotification object:w];
  [old release];
}

bool canTakeKeys(NSWindow* w) { return w && [w canBecomeKeyWindow]; }

// The key window went away: the front one of the others that can take the keys gets them (AppKit
// does the same when the key window of the active app closes).
void passKeyOn(NSWindow* going) {
  if (going != gKey) return;
  NSWindow* next = nil;
  for (NSWindow* o in [NSApp orderedWindows])
    if (o != going && [o isVisible] && canTakeKeys(o)) {
      next = o;
      break;
    }
  setKey(next);
}

// ---- replacements -----------------------------------------------------------------------------
void qOrderWindow(id self, SEL sel, NSWindowOrderingMode mode, NSInteger rel) {
  if (mode != NSWindowOut) quietLook(self);
  oOrderWindow(self, sel, mode, rel);
  if (mode == NSWindowOut) passKeyOn(self);
}
void qOrderFrontRegardless(id self, SEL sel) {
  quietLook(self);
  oOrderFrontRegardless(self, sel);
}
void qMakeKeyWindow(id self, SEL sel) {
  oMakeKeyWindow(self, sel);
  if ([self isVisible] && canTakeKeys(self)) setKey(self);  // (a window that is not ordered in never becomes key)
}
void qMakeKeyAndOrderFront(id self, SEL sel, id sender) {
  quietLook(self);
  oMakeKeyAndOrderFront(self, sel, sender);
  if (canTakeKeys(self)) setKey(self);
}
void qClose(id self, SEL sel) {
  [self retain];
  oClose(self, sel);
  passKeyOn(self);
  [self release];
}
BOOL qIsKeyWindow(id self, SEL) { return self == gKey ? YES : NO; }
void qSetAlpha(id self, SEL sel, CGFloat a) {
  objc_setAssociatedObject(self, &kAlphaKey, [NSNumber numberWithDouble:a], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  oSetAlpha(self, sel, isQuieted(self) ? 0.0 : a);
}
CGFloat qAlpha(id self, SEL sel) {
  NSNumber* v = objc_getAssociatedObject(self, &kAlphaKey);
  return v ? [v doubleValue] : oAlpha(self, sel);
}
void qSetIgnores(id self, SEL sel, BOOL on) {
  objc_setAssociatedObject(self, &kIgnoresKey, [NSNumber numberWithBool:on], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  oSetIgnores(self, sel, isQuieted(self) ? YES : on);
}
BOOL qIgnores(id self, SEL sel) {
  NSNumber* v = objc_getAssociatedObject(self, &kIgnoresKey);
  return v ? [v boolValue] : oIgnores(self, sel);
}
id qAppKeyWindow(id, SEL) { return gKey; }
BOOL qSetPolicy(id self, SEL sel, NSApplicationActivationPolicy) {
  return oSetPolicy(self, sel, NSApplicationActivationPolicyProhibited);
}
void blocked(const char* what) {
  ++gBlocked;
  logLine(std::string("dropped ") + what);
}
void qActivateIgnoring(id, SEL, BOOL) { blocked("activateIgnoringOtherApps:"); }
void qActivate(id, SEL) { blocked("activate"); }
BOOL qRunningActivate(id, SEL, NSApplicationActivationOptions) {
  blocked("activateWithOptions:");
  return NO;
}
BOOL qRunningActivateFrom(id, SEL, id, NSApplicationActivationOptions) {
  blocked("activateFromApplication:options:");
  return NO;
}

pid_t parentOf(pid_t pid) {
  proc_bsdinfo info{};
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) != static_cast<int>(sizeof info)) return 0;
  return static_cast<pid_t>(info.pbi_ppid);
}
// This process or one it started (the plugin sandbox, the scanner, the crash dialog).
bool isOwn(pid_t pid) { return pid == getpid() || (pid > 0 && parentOf(pid) == getpid()); }

void prohibit() {
  if (NSApp && [NSApp activationPolicy] != NSApplicationActivationPolicyProhibited)
    oSetPolicy(NSApp, @selector(setActivationPolicy:), NSApplicationActivationPolicyProhibited);
}

}  // namespace

void install() {
  if (gInstalled || !on()) return;
  gInstalled = true;
  // SDL's start-up activation (and its "regular" activation policy) off.
  setenv("SDL_MAC_BACKGROUND_APP", "1", 1);
  @autoreleasepool {
    // A quiet app is never the active app, and macOS throttles a background app whose windows
    // cannot be seen (App Nap: slower timers and run loop). A test run is timed like a run in front.
    static id<NSObject> awake = [[[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep |
                                                                                        NSActivityLatencyCritical
                                                                                reason:@"Quiet test run, timed like a run in front"] retain];
    (void)awake;
    Class W = [NSWindow class];
    oOrderWindow = reinterpret_cast<decltype(oOrderWindow)>(replace(W, @selector(orderWindow:relativeTo:), reinterpret_cast<IMP>(&qOrderWindow)));
    oOrderFrontRegardless =
        reinterpret_cast<decltype(oOrderFrontRegardless)>(replace(W, @selector(orderFrontRegardless), reinterpret_cast<IMP>(&qOrderFrontRegardless)));
    oMakeKeyWindow = reinterpret_cast<decltype(oMakeKeyWindow)>(replace(W, @selector(makeKeyWindow), reinterpret_cast<IMP>(&qMakeKeyWindow)));
    oMakeKeyAndOrderFront =
        reinterpret_cast<decltype(oMakeKeyAndOrderFront)>(replace(W, @selector(makeKeyAndOrderFront:), reinterpret_cast<IMP>(&qMakeKeyAndOrderFront)));
    oClose = reinterpret_cast<decltype(oClose)>(replace(W, @selector(close), reinterpret_cast<IMP>(&qClose)));
    oIsKeyWindow = reinterpret_cast<decltype(oIsKeyWindow)>(replace(W, @selector(isKeyWindow), reinterpret_cast<IMP>(&qIsKeyWindow)));
    oSetAlpha = reinterpret_cast<decltype(oSetAlpha)>(replace(W, @selector(setAlphaValue:), reinterpret_cast<IMP>(&qSetAlpha)));
    oAlpha = reinterpret_cast<decltype(oAlpha)>(replace(W, @selector(alphaValue), reinterpret_cast<IMP>(&qAlpha)));
    oSetIgnores = reinterpret_cast<decltype(oSetIgnores)>(replace(W, @selector(setIgnoresMouseEvents:), reinterpret_cast<IMP>(&qSetIgnores)));
    oIgnores = reinterpret_cast<decltype(oIgnores)>(replace(W, @selector(ignoresMouseEvents), reinterpret_cast<IMP>(&qIgnores)));
    Class A = [NSApplication class];
    oAppKeyWindow = reinterpret_cast<decltype(oAppKeyWindow)>(replace(A, @selector(keyWindow), reinterpret_cast<IMP>(&qAppKeyWindow)));
    oSetPolicy = reinterpret_cast<decltype(oSetPolicy)>(replace(A, @selector(setActivationPolicy:), reinterpret_cast<IMP>(&qSetPolicy)));
    replace(A, @selector(activateIgnoringOtherApps:), reinterpret_cast<IMP>(&qActivateIgnoring));
    replace(A, NSSelectorFromString(@"activate"), reinterpret_cast<IMP>(&qActivate));  // macOS 14
    Class R = [NSRunningApplication class];
    replace(R, @selector(activateWithOptions:), reinterpret_cast<IMP>(&qRunningActivate));
    replace(R, NSSelectorFromString(@"activateFromApplication:options:"), reinterpret_cast<IMP>(&qRunningActivateFrom));  // macOS 14

    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    // The application object is made later (by SDL): prohibited before it finishes launching.
    [nc addObserverForName:NSApplicationWillFinishLaunchingNotification object:nil queue:nil usingBlock:^(NSNotification*) { prohibit(); }];
    [nc addObserverForName:NSApplicationDidBecomeActiveNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  ++gBecameActive;
                  std::fprintf(stderr, "[quiet] the app became active (it should not in a quiet run)\n");
                  logLine("became active");
                  [NSApp deactivate];
                }];
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserverForName:NSWorkspaceDidActivateApplicationNotification
                                                                    object:nil
                                                                     queue:nil
                                                                usingBlock:^(NSNotification* n) {
                                                                  NSRunningApplication* app = [[n userInfo] objectForKey:NSWorkspaceApplicationKey];
                                                                  if (app && isOwn([app processIdentifier])) {
                                                                    ++gOwnActivations;
                                                                    logLine("the system activated pid " + std::to_string([app processIdentifier]));
                                                                  }
                                                                }];
    prohibit();  // when the application object exists already
  }
}

bool installed() { return gInstalled; }

void noteQuieted() { ++gQuieted; }

Check check() {
  Check c;
  c.quiet = on();
  c.guards = gInstalled;
  c.becameActive = gBecameActive;
  c.ownActivations = gOwnActivations;
  c.blockedActivations = gBlocked;
  c.windowsQuieted = gQuieted;
  @autoreleasepool {
    if (NSApp) {
      switch ([NSApp activationPolicy]) {
        case NSApplicationActivationPolicyRegular: c.policy = "regular"; break;
        case NSApplicationActivationPolicyAccessory: c.policy = "accessory"; break;
        case NSApplicationActivationPolicyProhibited: c.policy = "prohibited"; break;
      }
      if (NSWindow* k = [NSApp keyWindow]) c.keyWindow = titleOf(k);
    }
    NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
    c.activeNow = [[NSRunningApplication currentApplication] isActive] || (front && isOwn([front processIdentifier]));
    // The window server's view: every window of this app and the ones it started that is ordered
    // in on this Space, and whether any of it can be seen (not fully transparent, on a display).
    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, displays, &count) != kCGErrorSuccess) count = 0;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    for (NSDictionary* d in (NSArray*)list) {
      const pid_t pid = [[d objectForKey:(id)kCGWindowOwnerPID] intValue];
      if (!isOwn(pid)) continue;
      ++c.onScreen;
      CGRect b = CGRectZero;
      CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)[d objectForKey:(id)kCGWindowBounds], &b);
      const double alpha = [[d objectForKey:(id)kCGWindowAlpha] doubleValue];
      bool onDisplay = false;
      for (uint32_t i = 0; i < count; ++i) onDisplay = onDisplay || CGRectIntersectsRect(CGDisplayBounds(displays[i]), b);
      if (alpha > 0.0 && onDisplay && b.size.width > 0 && b.size.height > 0) {
        ++c.visible;
        char buf[160];
        std::snprintf(buf, sizeof buf, "%spid %d %dx%d at %d,%d alpha %.2f", c.detail.empty() ? "" : "; ", static_cast<int>(pid),
                      static_cast<int>(b.size.width), static_cast<int>(b.size.height), static_cast<int>(b.origin.x), static_cast<int>(b.origin.y), alpha);
        c.detail += buf;
      }
    }
    if (list) CFRelease(list);
  }
  return c;
}

}  // namespace gl::quiet

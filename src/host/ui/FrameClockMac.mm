// Gridline — display-synchronised frame pacing on macOS (J-66).
//
// SDL presents with vsync by blocking the main thread inside the swap until
// the display refreshes (a condition variable fed by a display link). For
// that time — most of every frame — the main thread is outside the Cocoa run
// loop, and so is every hosted plugin interface: their timers, their display
// link callbacks, and AppKit's display pass (which only runs when the run loop
// is about to sleep) wait for the next brief event poll. Interfaces then
// redraw at a fraction of the display rate (Symbiosis-wrapped AUs measured
// ~12 fps while a knob was turned, Kilohearts ~38 fps).
//
// Instead Gridline presents without vsync and waits for the next refresh
// *inside* the run loop: a display link thread counts refreshes and wakes the
// main run loop, which sleeps in CFRunLoopRunInMode in between — exactly like
// an idle Cocoa application, so plugin interfaces get every display pass.
//
// The CoreVideo display link functions are resolved at run time (they are
// deprecated in the newest SDKs but remain the per-display refresh source on
// every supported macOS version); without them a 60 Hz timer paces frames.
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <atomic>
#include "NativeView.h"

namespace gl::host::native {

namespace {
using LinkRef = void*;
using OutputFn = int32_t (*)(LinkRef, const void*, const void*, uint64_t, uint64_t*, void*);
struct CoreVideo {
  int32_t (*createWithActive)(LinkRef*) = nullptr;
  int32_t (*setCallback)(LinkRef, OutputFn, void*) = nullptr;
  int32_t (*setDisplay)(LinkRef, uint32_t) = nullptr;
  int32_t (*start)(LinkRef) = nullptr;
  int32_t (*stop)(LinkRef) = nullptr;
  void (*release)(LinkRef) = nullptr;
  double (*actualPeriod)(LinkRef) = nullptr;
  bool ok = false;
};
const CoreVideo& coreVideo() {
  static CoreVideo cv = [] {
    CoreVideo c;
    void* h = dlopen("/System/Library/Frameworks/CoreVideo.framework/CoreVideo", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return c;
    c.createWithActive = reinterpret_cast<decltype(c.createWithActive)>(dlsym(h, "CVDisplayLinkCreateWithActiveCGDisplays"));
    c.setCallback = reinterpret_cast<decltype(c.setCallback)>(dlsym(h, "CVDisplayLinkSetOutputCallback"));
    c.setDisplay = reinterpret_cast<decltype(c.setDisplay)>(dlsym(h, "CVDisplayLinkSetCurrentCGDisplay"));
    c.start = reinterpret_cast<decltype(c.start)>(dlsym(h, "CVDisplayLinkStart"));
    c.stop = reinterpret_cast<decltype(c.stop)>(dlsym(h, "CVDisplayLinkStop"));
    c.release = reinterpret_cast<decltype(c.release)>(dlsym(h, "CVDisplayLinkRelease"));
    c.actualPeriod = reinterpret_cast<decltype(c.actualPeriod)>(dlsym(h, "CVDisplayLinkGetActualOutputVideoRefreshPeriod"));
    c.ok = c.createWithActive && c.setCallback && c.setDisplay && c.start && c.stop && c.release;
    return c;
  }();
  return cv;
}

struct Clock {
  LinkRef link = nullptr;
  std::atomic<uint64_t> ticks{0};
  CFRunLoopSourceRef wake = nullptr;
  CFRunLoopRef mainLoop = nullptr;
  uint32_t display = 0;
  uint64_t seen = 0;           // refresh count when the last frame was presented
  bool paused = false;         // K-142: the link stopped while the interface idles (frameClockIdle)
  id sdlCtx = nil;  // K-142 (0.7.8 close): the context whose own display link is held stopped (retained)
  double fallbackNext = 0;     // timer pacing without a display link
};
Clock* gClock = nullptr;

int32_t onRefresh(LinkRef, const void*, const void*, uint64_t, uint64_t*, void* ctx) {
  auto* c = static_cast<Clock*>(ctx);
  c->ticks.fetch_add(1, std::memory_order_release);
  CFRunLoopSourceSignal(c->wake);
  CFRunLoopWakeUp(c->mainLoop);
  return 0;
}

void noop(void*) {}

// K-142 (0.7.8 close): SDL's OpenGL context on macOS runs a display link of its own from the moment
// it is made, to pace swaps when vsync is on (it signals a condition each refresh). Gridline presents
// without vsync whenever this frame clock runs (swap interval 0, main.cpp), so that link only wakes a
// thread 60 times a second for nothing; it is stopped while the interface idles, with this clock's
// own, and started again with it. Found by its instance variable (SDLOpenGLContext's `displayLink`);
// a context without one (another SDL version) is left alone.
LinkRef sdlLinkOf(id ctx) {  // (an NSOpenGLContext, named by id: the class is deprecated)
  if (!ctx) return nullptr;
  Ivar iv = class_getInstanceVariable([ctx class], "displayLink");
  if (!iv) return nullptr;
  const char* enc = ivar_getTypeEncoding(iv);
  if (!enc || enc[0] != '^') return nullptr;
  return *reinterpret_cast<LinkRef*>(reinterpret_cast<char*>(ctx) + ivar_getOffset(iv));
}

// Stops (hold true) the current context's own link, or starts again the one it stopped. The context
// is retained meanwhile, so a context deleted while idle (a graphics reset) is still there to look at;
// SDL's cleanup clears its link, and a link that is gone or changed is not started.
void holdSdlLink(Clock& c, bool hold) {
  const CoreVideo& cv = coreVideo();
  if (hold) {
    if (c.sdlCtx) return;
    id ctx = [NSClassFromString(@"NSOpenGLContext") currentContext];
    LinkRef l = sdlLinkOf(ctx);
    if (!l || cv.stop(l) != 0) return;
    c.sdlCtx = [ctx retain];
  } else if (c.sdlCtx) {
    if (LinkRef l = sdlLinkOf(c.sdlCtx)) cv.start(l);
    [c.sdlCtx release];
    c.sdlCtx = nil;
  }
}

uint32_t displayOf(const Window& w) {
  if (!w.handle) return 0;
  NSScreen* s = [(NSWindow*)w.handle screen];
  if (!s) return 0;
  NSNumber* n = [[s deviceDescription] objectForKey:@"NSScreenNumber"];
  return n ? [n unsignedIntValue] : 0;
}
}  // namespace

bool frameClockStart(const Window& w) {
  if (gClock) return true;
  auto* c = new Clock;
  c->mainLoop = CFRunLoopGetMain();
  CFRunLoopSourceContext ctx{};
  ctx.perform = noop;
  c->wake = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &ctx);
  CFRunLoopAddSource(c->mainLoop, c->wake, kCFRunLoopCommonModes);
  const CoreVideo& cv = coreVideo();
  if (cv.ok && cv.createWithActive(&c->link) == 0 && c->link) {
    cv.setCallback(c->link, onRefresh, c);
    c->display = displayOf(w);
    if (c->display) cv.setDisplay(c->link, c->display);
    if (cv.start(c->link) != 0) {
      cv.release(c->link);
      c->link = nullptr;
    }
  }
  gClock = c;
  return true;
}

void frameClockFollow(const Window& w) {
  if (!gClock || !gClock->link) return;
  const uint32_t d = displayOf(w);
  if (d && d != gClock->display) {
    gClock->display = d;
    coreVideo().setDisplay(gClock->link, d);
  }
}

void frameClockWait() {
  Clock* c = gClock;
  if (!c) return;
  const double now = CFAbsoluteTimeGetCurrent();
  if (!c->link) {  // no display link: 60 Hz from a deadline
    if (c->fallbackNext < now - 0.1) c->fallbackNext = now;
    c->fallbackNext += 1.0 / 60.0;
    for (double left = c->fallbackNext - now; left > 0; left = c->fallbackNext - CFAbsoluteTimeGetCurrent())
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, left, false);
    return;
  }
  // Sleep in the run loop until the display refreshed after this frame was
  // presented (vsync semantics: a late frame waits for the next refresh).
  // Bounded: a display that stops refreshing (sleep, unplugged) never stalls the UI.
  const uint64_t target = c->seen;
  const double deadline = now + 0.05;
  // A frame that took longer than a refresh still lets the run loop sleep
  // once: AppKit draws views (plugin interfaces) only before it sleeps.
  if (c->ticks.load(std::memory_order_acquire) > target) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0002, false);
  while (c->ticks.load(std::memory_order_acquire) <= target) {
    const double left = deadline - CFAbsoluteTimeGetCurrent();
    if (left <= 0) break;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, left, true);
  }
  c->seen = c->ticks.load(std::memory_order_acquire);
}

void frameClockStop() {
  Clock* c = gClock;
  if (!c) return;
  gClock = nullptr;
  if (coreVideo().ok) holdSdlLink(*c, false);
  if (c->link) {
    coreVideo().stop(c->link);  // returns once no callback is running
    coreVideo().release(c->link);
  }
  CFRunLoopRemoveSource(c->mainLoop, c->wake, kCFRunLoopCommonModes);
  CFRelease(c->wake);
  delete c;
}

// K-142 (0.7.8): while the interface idles the display link stops, so nothing wakes the main
// thread every refresh; the loop sleeps in the run loop until input, a wake or its own deadline.
void frameClockIdle(bool idle) {
  Clock* c = gClock;
  if (!c || !c->link || c->paused == idle) return;
  if (idle) {
    coreVideo().stop(c->link);  // returns once no callback is running
  } else if (coreVideo().start(c->link) != 0) {
    return;  // (stays paused; frameClockWait's deadline paces the frames meanwhile)
  }
  holdSdlLink(*c, idle);  // K-142 (0.7.8 close): SDL's own link rests with it
  c->paused = idle;
}

bool frameClockSleep(double seconds) {
  if (seconds <= 0) return false;
  // Returns when a source of the run loop fired (an input event from the window server, a wake
  // from another thread, a notification) or the time is up. Timers (plugin interfaces) run meanwhile.
  const CFRunLoopRunResult r = CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
  if (r == kCFRunLoopRunFinished) {  // nothing to wait on in the mode (should not happen): sleep plainly
    [NSThread sleepForTimeInterval:seconds];
    return false;
  }
  return r == kCFRunLoopRunHandledSource || r == kCFRunLoopRunStopped;
}

bool displayWaitStart(void*) { return false; }  // (the run loop: frameClockSleep)
bool displayWaitStartWayland(void*) { return false; }
bool displaySleep(double) { return false; }

void frameClockWake() {
  Clock* c = gClock;
  if (!c) return;
  CFRunLoopSourceSignal(c->wake);
  CFRunLoopWakeUp(c->mainLoop);
}

namespace {
std::atomic<int> gOnScreenChanges{0};
id gOcclusionObserver = nil;
}  // namespace

bool windowOnScreen(const Window& w) {
  if (!w.handle) return true;
  return ([(NSWindow*)w.handle occlusionState] & NSWindowOcclusionStateVisible) != 0;
}

void watchOnScreen(const Window& w) {
  if (!w.handle || gOcclusionObserver) return;
  gOcclusionObserver = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeOcclusionStateNotification
                                                                         object:(NSWindow*)w.handle
                                                                          queue:nil
                                                                     usingBlock:^(NSNotification*) {
                                                                       gOnScreenChanges.fetch_add(1);
                                                                       frameClockWake();
                                                                     }];
  [gOcclusionObserver retain];
}

int onScreenChanges() { return gOnScreenChanges.load(); }

namespace {
id<NSObject> gAwake = nil;
}

void keepAwake(bool on) {
  if (on == (gAwake != nil)) return;
  if (on) {
    gAwake = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                                                            reason:@"Collaborative project: staying connected to collaborators"];
    [gAwake retain];
  } else {
    [[NSProcessInfo processInfo] endActivity:gAwake];
    [gAwake release];
    gAwake = nil;
  }
}

}  // namespace gl::host::native

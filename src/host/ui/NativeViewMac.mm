// Gridline — native pieces of a plugin window (macOS / AppKit).
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "NativeView.h"
#include "ObjcClass.h"
#include "../../core/UiScale.h"

namespace {
using gl::objc::RuntimeClass;

// Container view for a plugin's interface: flipped is irrelevant to plugins
// (they place their own view at the origin), but it must not let the window
// background or the GL surface underneath draw over it. Keys the interface
// passes up the responder chain arrive here and go to Gridline (J-66).
// Classes made at run time (ObjcClass.h): a plugin bundle that links this code
// never shares a class name with the app or with another bundle.
Class containerClass() {
  static RuntimeClass rc("GridlinePluginContainer", [NSView class]);
  static Class c = [] {
    static Class self_ = Nil;
    rc.method(@selector(isOpaque), +[](id, SEL) -> BOOL { return NO; })
        .method(@selector(acceptsFirstMouse:), +[](id, SEL, NSEvent*) -> BOOL { return YES; })  // first click goes straight to the plugin's control
        // Not passed on: the window would beep for a key nobody handled.
        .method(@selector(keyDown:), +[](id self, SEL sel, NSEvent* e) {
          if (!gl::host::native::containerKey(self, e, true)) gl::objc::callSuper<void>(self, self_, sel, e);
        })
        .method(@selector(keyUp:), +[](id self, SEL sel, NSEvent* e) {
          if (!gl::host::native::containerKey(self, e, false)) gl::objc::callSuper<void>(self, self_, sel, e);
        });
    self_ = rc.done();
    return self_;
  }();
  return c;
}

// 0.7.8: the view a docked interface's container sits in. It shows the visible part of the
// interface (its frame), cuts holes where Gridline draws in front of it (a mask on its layer),
// and is flipped so the container is placed from the top-left like Gridline's coordinates.
Class dockClipClass() {
  static RuntimeClass rc("GridlineDockClip", [NSView class]);
  static Class c = rc.method(@selector(isOpaque), +[](id, SEL) -> BOOL { return NO; })
                       .method(@selector(isFlipped), +[](id, SEL) -> BOOL { return YES; })
                       .method(@selector(acceptsFirstMouse:), +[](id, SEL, NSEvent*) -> BOOL { return YES; })
                       .done();
  return c;
}

// Test views (devTestKeyView): an interface view that passes keys on, one
// that uses every key.
Class testKeyPassClass() {
  static RuntimeClass rc("GridlineTestKeyPassView", [NSView class]);
  static Class c = rc.method(@selector(acceptsFirstResponder), +[](id, SEL) -> BOOL { return YES; }).done();
  return c;
}
Class testKeyEatClass() {
  static RuntimeClass rc("GridlineTestKeyEatView", [NSView class]);
  static Class c = rc.method(@selector(acceptsFirstResponder), +[](id, SEL) -> BOOL { return YES; })
                       .method(@selector(keyDown:), +[](id, SEL, NSEvent*) {})
                       .method(@selector(keyUp:), +[](id, SEL, NSEvent*) {})
                       .done();
  return c;
}
}  // namespace

namespace gl::host::native {

namespace {
// ---- keys (J-66) ----
struct Watch {
  std::deque<KeyEvent> queue;
  std::set<uint16_t> held;  // forwarded key-downs whose key-up is due
  NSView* lastFocus = nil;  // not retained: only used once found in the container again
};
std::map<void*, Watch>& watches() {
  static std::map<void*, Watch> m;
  return m;
}
uint32_t modsOf(NSEventModifierFlags f) {
  uint32_t m = 0;
  if (f & NSEventModifierFlagShift) m |= kKeyShift;
  if (f & NSEventModifierFlagControl) m |= kKeyCtrl;
  if (f & NSEventModifierFlagOption) m |= kKeyAlt;
  if (f & NSEventModifierFlagCommand) m |= kKeyCmd;
  return m;
}
bool isText(NSResponder* r) { return r && [r isKindOfClass:[NSText class]]; }
bool inside(NSResponder* r, NSView* container) {
  return r && container && [r isKindOfClass:[NSView class]] && [(NSView*)r isDescendantOf:container];
}
bool contains(NSView* root, NSView* v) {
  if (root == v) return true;
  for (NSView* s in [root subviews])
    if (contains(s, v)) return true;
  return false;
}
void push(Watch& w, NSEvent* e, bool down) {
  KeyEvent k;
  k.down = down;
  k.repeat = down && [e isARepeat];
  k.code = [e keyCode];
  k.mods = modsOf([e modifierFlags]);
  w.queue.push_back(k);
  if (w.queue.size() > 256) w.queue.pop_front();  // nobody reads it: keep it bounded
}
// Key-ups of forwarded keys reach Gridline even when the interface swallows them.
void installKeyUpMonitor() {
  static id monitor = nil;
  if (monitor) return;
  monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyUp
                                                   handler:^NSEvent*(NSEvent* e) {
                                                     for (auto& [c, w] : watches()) {
                                                       if ([(NSView*)c window] != [e window] || !w.held.count([e keyCode])) continue;
                                                       w.held.erase([e keyCode]);
                                                       push(w, e, false);
                                                     }
                                                     return e;
                                                   }] retain];
}

NSView* contentOf(const Window& w) {
  if (!w.handle) return nil;
  return [(NSWindow*)w.handle contentView];
}
// Top-left based rect in the content view -> AppKit frame (bottom-left based
// unless the content view is flipped).
NSRect toFrame(NSView* content, const layout::Rect& r) {
  const CGFloat H = [content bounds].size.height;
  const CGFloat y = [content isFlipped] ? r.y : H - r.y - r.h;
  return NSMakeRect(r.x, y, r.w, r.h);
}
}  // namespace

Window windowOfView(void* view) {
  Window w;
  if (view) w.handle = [(NSView*)view window];
  return w;
}

void* createContainer(const Window& w, const layout::Rect& r) {
  NSView* content = contentOf(w);
  if (!content) return nullptr;
  NSView* v = [[containerClass() alloc] initWithFrame:toFrame(content, r)];
  // Stays glued to the top-left corner when the window grows or shrinks
  // below it (settings panel unfolding).
  [v setAutoresizingMask:[content isFlipped] ? NSViewMaxYMargin : NSViewMinYMargin];
  // The plugin's interface never draws outside its area (over the control
  // bar), whatever size its own views take: AppKit stopped clipping
  // subviews by default in macOS 14.
  if ([v respondsToSelector:@selector(setClipsToBounds:)]) [v setClipsToBounds:YES];
  [v setWantsLayer:YES];
  [[v layer] setMasksToBounds:YES];
  [content addSubview:v positioned:NSWindowAbove relativeTo:nil];
  watchKeys(v);
  return v;  // retained: released in destroyContainer
}

void setContainerFrame(const Window& w, void* container, const layout::Rect& r) {
  NSView* content = contentOf(w);
  if (!content || !container) return;
  NSView* v = (NSView*)container;
  NSRect f = toFrame(content, r);
  if (!NSEqualRects([v frame], f)) [v setFrame:f];
}

namespace {
struct Docked {
  NSView* clip = nil;  // retained
  bool pressed = false;
  layout::Rect visible;
  size_t holes = 0;
};
std::map<void*, Docked>& docked() {
  static std::map<void*, Docked> m;
  return m;
}
// A press on a docked interface is noted for Gridline (its window comes to the front); the event goes on.
void installPressMonitor() {
  static id monitor = nil;
  if (monitor) return;
  const NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown;
  monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                   handler:^NSEvent*(NSEvent* e) {
                                                     NSWindow* win = [e window];
                                                     NSView* content = win ? [win contentView] : nil;
                                                     if (!content) return e;
                                                     NSView* hit = [content hitTest:[content convertPoint:[e locationInWindow] fromView:nil]];
                                                     if (!hit) return e;
                                                     for (auto& [c, d] : docked())
                                                       if ([d.clip window] == win && [hit isDescendantOf:d.clip]) d.pressed = true;
                                                     return e;
                                                   }] retain];
}
}  // namespace

void* createDockedContainer(const Window& w, const layout::Rect& full) {
  NSView* content = contentOf(w);
  if (!content) return nullptr;
  NSView* clip = [[dockClipClass() alloc] initWithFrame:toFrame(content, full)];
  [clip setAutoresizingMask:NSViewNotSizable];
  [clip setWantsLayer:YES];
  [[clip layer] setMasksToBounds:YES];
  if ([clip respondsToSelector:@selector(setClipsToBounds:)]) [clip setClipsToBounds:YES];
  NSView* v = [[containerClass() alloc] initWithFrame:NSMakeRect(0, 0, std::max(1, full.w), std::max(1, full.h))];
  [v setAutoresizingMask:NSViewNotSizable];
  if ([v respondsToSelector:@selector(setClipsToBounds:)]) [v setClipsToBounds:YES];
  [v setWantsLayer:YES];
  [[v layer] setMasksToBounds:YES];
  [clip addSubview:v];
  [content addSubview:clip positioned:NSWindowAbove relativeTo:nil];
  Docked d;
  d.clip = clip;
  d.visible = full;
  docked()[v] = d;
  installPressMonitor();
  watchKeys(v);
  return v;  // retained: released in destroyDockedContainer
}

void setDockedFrame(const Window& w, void* container, const layout::Rect& full, const layout::Rect& visible, const std::vector<layout::Rect>& holes) {
  auto it = docked().find(container);
  NSView* content = contentOf(w);
  if (it == docked().end() || !content) return;
  Docked& d = it->second;
  NSView* clip = d.clip;
  NSView* v = (NSView*)container;
  // QuartzCore by name (not linked: AppKit loads it), as DrawStatsMac.mm does.
  static id const tx = NSClassFromString(@"CATransaction");
  static id const shape = NSClassFromString(@"CAShapeLayer");
  [tx begin];
  [tx setDisableActions:YES];  // moves at once, never animated
  if (visible.empty()) {
    if (![clip isHidden]) [clip setHidden:YES];
  } else {
    // Back in its window when it was popped out and in again (the plugin's view keeps its window).
    if ([clip superview] != content) {
      [clip removeFromSuperview];
      [content addSubview:clip positioned:NSWindowAbove relativeTo:nil];
    }
    const NSRect cf = toFrame(content, visible);
    if (!NSEqualRects([clip frame], cf)) [clip setFrame:cf];
    const NSRect vf = NSMakeRect(full.x - visible.x, full.y - visible.y, std::max(1, full.w), std::max(1, full.h));  // the clip view is flipped
    if (!NSEqualRects([v frame], vf)) [v setFrame:vf];
    if (holes.empty()) {
      if ([[clip layer] mask]) [[clip layer] setMask:nil];
    } else {
      CAShapeLayer* mask = static_cast<CAShapeLayer*>([shape layer]);
      const CGRect b = [[clip layer] bounds];
      [mask setFrame:b];
      CGMutablePathRef path = CGPathCreateMutable();
      CGPathAddRect(path, nullptr, b);
      // A flipped view's layer has the view's y axis (AppKit keeps them alike): holes go top-down there
      // (seen in a capture: the layer's own flags said otherwise and the holes came out mirrored).
      const bool flipped = [clip isFlipped];
      for (const layout::Rect& h : holes) {
        const CGFloat hx = h.x - visible.x, hy = h.y - visible.y;
        CGPathAddRect(path, nullptr, CGRectMake(hx, flipped ? hy : b.size.height - hy - h.h, h.w, h.h));
      }
      [mask setPath:path];
      [mask setFillRule:@"even-odd"];  // kCAFillRuleEvenOdd
      CGPathRelease(path);
      [[clip layer] setMask:mask];
    }
    if ([clip isHidden]) [clip setHidden:NO];
  }
  [tx commit];
  d.visible = visible;
  d.holes = holes.size();
}

void destroyDockedContainer(const Window& w, void* container) {
  (void)w;
  auto it = docked().find(container);
  if (it == docked().end()) return;
  NSView* clip = it->second.clip;
  docked().erase(it);
  unwatchKeys(container);
  NSView* v = (NSView*)container;
  [v removeFromSuperview];
  [v release];
  [clip removeFromSuperview];
  [clip release];
}

bool dockedHolesCut() { return true; }

bool takeDockedPress(void* container) {
  auto it = docked().find(container);
  if (it == docked().end() || !it->second.pressed) return false;
  it->second.pressed = false;
  return true;
}

void releaseKeyFocus(void* container) {
  NSView* c = (NSView*)container;
  NSWindow* win = c ? [c window] : nil;
  if (!win || !inside([win firstResponder], c)) return;
  // SDL's own text view takes the keys of its window (it adds it to the content view): back to it,
  // else to the content view.
  NSView* content = [win contentView];
  NSView* to = content;
  for (NSView* s in [content subviews])
    if ([NSStringFromClass([s class]) hasPrefix:@"SDL"] && [s acceptsFirstResponder]) to = s;
  [win makeFirstResponder:to];
}

void containerMoved(const Window&, void*) {}

void destroyContainer(const Window& w, void* container) {
  (void)w;
  if (!container) return;
  NSView* v = (NSView*)container;
  unwatchKeys(container);
  [v removeFromSuperview];
  [v release];
}

void keepAbove(const Window& child, const Window& parent, bool on) {
  if (!child.handle || !parent.handle || child.handle == parent.handle) return;
  NSWindow* c = (NSWindow*)child.handle;
  NSWindow* p = (NSWindow*)parent.handle;
  if (on) {
    if ([c parentWindow] != p) [p addChildWindow:c ordered:NSWindowAbove];
  } else if ([c parentWindow] == p) {
    [p removeChildWindow:c];
  }
}

void orderAbove(const Window& w, const Window& ref) {
  if (!w.handle || !ref.handle || w.handle == ref.handle) return;
  NSWindow* c = (NSWindow*)w.handle;
  if (![c isVisible]) return;
  [c orderWindow:NSWindowAbove relativeTo:[(NSWindow*)ref.handle windowNumber]];
}

namespace {
// setTakesNoKeys: the windows that may not become key right now. The window's class (SDL's) answers
// canBecomeKeyWindow / canBecomeMainWindow through this check first, then as it did.
std::set<void*>& noKeyWindows() {
  static std::set<void*> s;
  return s;
}
std::map<std::pair<Class, SEL>, IMP>& originalImps() {
  static std::map<std::pair<Class, SEL>, IMP> m;
  return m;
}
BOOL askOriginal(id self, SEL sel) {
  for (Class c = [self class]; c; c = class_getSuperclass(c)) {
    const auto it = originalImps().find({c, sel});
    if (it != originalImps().end()) return it->second ? reinterpret_cast<BOOL (*)(id, SEL)>(it->second)(self, sel) : NO;
  }
  return NO;
}
BOOL mayBecome(id self, SEL sel) {
  if (noKeyWindows().count((void*)self)) return NO;
  return askOriginal(self, sel);
}
void guardClass(Class cls) {
  for (SEL sel : {@selector(canBecomeKeyWindow), @selector(canBecomeMainWindow)}) {
    if (originalImps().count({cls, sel})) continue;
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) continue;
    const IMP orig = method_getImplementation(m);
    const IMP mine = reinterpret_cast<IMP>(&mayBecome);
    if (orig == mine) continue;
    originalImps()[{cls, sel}] = orig;
    // The class's own method is replaced; an inherited one gets an override in this class only.
    if (!class_addMethod(cls, sel, mine, method_getTypeEncoding(m))) method_setImplementation(m, mine);
  }
}
}  // namespace

void setTakesNoKeys(const Window& w, bool on) {
  if (!w.handle) return;
  NSWindow* nw = (NSWindow*)w.handle;
  if (on) {
    guardClass([nw class]);
    noKeyWindows().insert(w.handle);
  } else {
    noKeyWindows().erase(w.handle);
  }
}

void raiseChild(const Window& child, const Window& parent) {
  if (!child.handle || !parent.handle || child.handle == parent.handle) return;
  NSWindow* c = (NSWindow*)child.handle;
  NSWindow* p = (NSWindow*)parent.handle;
  if ([c parentWindow] != p || ![c isVisible]) return;
  if ([[p childWindows] lastObject] == c) return;  // already the top one
  [p removeChildWindow:c];
  [p addChildWindow:c ordered:NSWindowAbove];  // added last: above the parent's other child windows
}

bool inModalSizeLoop() {
  // J-116: AppKit's live-resize tracking runs inside the event pump (the mouse-down on a window
  // edge does not return until the release); SDL reports windowDidResize: and its 60 Hz
  // live-resize timer (EXPOSED) from in there.
  for (NSWindow* w in [NSApp windows])
    if ([w inLiveResize]) return true;
  return false;
}

double backingScale(const Window& w) {
  if (!w.handle) return 1.0;
  return [(NSWindow*)w.handle backingScaleFactor];
}

std::string xResources(const Window&) { return {}; }  // X11 only
bool xServerAnswers() { return false; }
void* sideWindowOpen(const std::string&, int, int, bool) { return nullptr; }
void sideWindowResize(void*, int, int, bool) {}
bool sideWindowPump(void*, int&, int&) { return false; }
void sideWindowClose(void*) {}

PointerSize pointerSize(const Window&) {
  PointerSize r;
  double mult = 1.0;  // System Settings > Accessibility > Display > Pointer size
  if (CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("mouseDriverCursorSize"), CFSTR("com.apple.universalaccess"))) {
    double d = 0;
    if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberDoubleType, &d) && d >= 1 && d <= 4) mult = d;
    CFRelease(v);
  }
  @autoreleasepool {
    NSImage* img = [[NSCursor arrowCursor] image];
    const NSSize sz = img ? [img size] : NSZeroSize;
    double visible = 0;
    if (sz.width >= 4 && sz.height >= 4 && sz.width <= 256 && sz.height <= 256) {
      // the visible part of the image (the arrow and its outline, not the room for the shadow)
      const int k = 4;  // samples per point
      const int W = static_cast<int>(std::ceil(sz.width * k)), H = static_cast<int>(std::ceil(sz.height * k));
      std::vector<unsigned char> px(static_cast<size_t>(W) * static_cast<size_t>(H) * 4, 0);
      CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
      CGContextRef ctx = CGBitmapContextCreate(px.data(), static_cast<size_t>(W), static_cast<size_t>(H), 8, static_cast<size_t>(W) * 4, cs,
                                               kCGImageAlphaPremultipliedLast);
      CGColorSpaceRelease(cs);
      if (ctx) {
        NSGraphicsContext* g = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:g];
        [img drawInRect:NSMakeRect(0, 0, W, H) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1.0];
        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(ctx);
        int top = -1, bottom = -1;
        for (int y = 0; y < H; ++y)
          for (int x = 0; x < W; ++x)
            if (px[(static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 4 + 3] >= 128) {
              if (top < 0) top = y;
              bottom = y;
              break;
            }
        if (top >= 0) visible = static_cast<double>(bottom - top + 1) / k;
      }
    }
    if (visible >= 8) {
      r.arrowH = visible * mult;
      r.source = "NSCursor arrow image";
    } else {
      r.arrowH = uiscale::pointerArrowHeight(uiscale::PointerSystem::MacOS, sz.height, mult);
      r.source = sz.height > 0 ? "NSCursor image size" : "macOS default";
    }
  }
  if (mult > 1.0) r.source += " x accessibility pointer size " + std::to_string(mult).substr(0, 4);
  return r;
}

long windowNumber(const Window& w) {
  if (!w.handle) return -1;
  return static_cast<long>([(NSWindow*)w.handle windowNumber]);
}

void watchKeys(void* container) {
  if (!container) return;
  installKeyUpMonitor();
  watches()[container];
}

void unwatchKeys(void* container) { watches().erase(container); }

bool containerKey(void* container, void* nsEvent, bool down) {
  auto it = watches().find(container);
  if (it == watches().end() || !nsEvent) return false;
  NSEvent* e = (NSEvent*)nsEvent;
  Watch& w = it->second;
  if (!down) {  // the key-up of a forwarded key (unless the monitor sent it already)
    if (!w.held.erase([e keyCode])) return false;
    push(w, e, false);
    return true;
  }
  // A text field of the interface keeps every key (a field editor never
  // passes typing on, but some pass Return / Escape / Tab).
  if (isText([[(NSView*)container window] firstResponder])) return false;
  w.held.insert([e keyCode]);
  push(w, e, true);
  return true;
}

bool popKey(void* container, KeyEvent& e) {
  auto it = watches().find(container);
  if (it == watches().end() || it->second.queue.empty()) return false;
  e = it->second.queue.front();
  it->second.queue.pop_front();
  return true;
}

KeyFocus keyFocus(void* container) {
  NSView* c = (NSView*)container;
  if (!c || ![c window]) return KeyFocus::Host;
  NSResponder* r = [[c window] firstResponder];
  if (!inside(r, c)) return KeyFocus::Host;
  return isText(r) ? KeyFocus::PluginText : KeyFocus::Plugin;
}

void saveKeyFocus(void* container) {
  auto it = watches().find(container);
  NSView* c = (NSView*)container;
  if (it == watches().end() || !c || ![c window]) return;
  Watch& w = it->second;
  // Keys held while the window loses the keyboard are released for Gridline.
  for (uint16_t code : w.held) {
    KeyEvent k;
    k.down = false;
    k.code = code;
    w.queue.push_back(k);
  }
  w.held.clear();
  NSResponder* r = [[c window] firstResponder];
  if (!inside(r, c)) return;
  // A field editor stands in for its text field.
  if (isText(r) && [[(NSText*)r delegate] isKindOfClass:[NSView class]]) r = (NSView*)[(NSText*)r delegate];
  w.lastFocus = (NSView*)r;
}

void restoreKeyFocus(void* container) {
  auto it = watches().find(container);
  NSView* c = (NSView*)container;
  if (it == watches().end() || !c || ![c window] || !it->second.lastFocus) return;
  NSView* v = it->second.lastFocus;
  it->second.lastFocus = nil;
  if (inside([[c window] firstResponder], c) || !contains(c, v)) return;  // still focused / gone
  [[c window] makeFirstResponder:v];
}

void* devTestKeyView(void* container, int kind) {
  NSView* c = (NSView*)container;
  if (!c) return nullptr;
  const NSRect r = NSMakeRect(0, 0, [c bounds].size.width, [c bounds].size.height);
  NSView* v = nil;
  if (kind == 3) {  // the interface's own first view that takes the keyboard
    NSMutableArray<NSView*>* todo = [NSMutableArray arrayWithArray:[c subviews]];
    while ([todo count] > 0) {
      NSView* x = [todo objectAtIndex:0];
      [todo removeObjectAtIndex:0];
      if ([x acceptsFirstResponder]) {
        [[c window] makeFirstResponder:x];
        return [[c window] firstResponder] == x ? x : nullptr;
      }
      [todo addObjectsFromArray:[x subviews]];
    }
    return nullptr;
  }
  if (kind == 2) {
    NSTextField* t = [[NSTextField alloc] initWithFrame:NSMakeRect(4, 4, 120, 22)];
    [t setEditable:YES];
    v = t;
  } else if (kind == 1) {
    v = [[testKeyEatClass() alloc] initWithFrame:r];
  } else {
    v = [[testKeyPassClass() alloc] initWithFrame:r];
  }
  [c addSubview:v];
  [v release];  // owned by the container
  [[c window] makeFirstResponder:v];
  return v;
}

bool devSendKey(void* container, uint16_t code, bool down, uint32_t mods, const char* chars) {
  NSView* c = (NSView*)container;
  NSWindow* win = c ? [c window] : nil;
  if (!win) return false;
  NSEventModifierFlags f = 0;
  if (mods & kKeyShift) f |= NSEventModifierFlagShift;
  if (mods & kKeyCtrl) f |= NSEventModifierFlagControl;
  if (mods & kKeyAlt) f |= NSEventModifierFlagOption;
  if (mods & kKeyCmd) f |= NSEventModifierFlagCommand;
  NSString* s = [NSString stringWithUTF8String:chars ? chars : ""];
  NSEvent* e = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                                location:NSZeroPoint
                           modifierFlags:f
                               timestamp:[[NSProcessInfo processInfo] systemUptime]
                            windowNumber:[win windowNumber]
                                 context:nil
                              characters:s
             charactersIgnoringModifiers:s
                               isARepeat:NO
                                 keyCode:code];
  if (!e) return false;
  [win sendEvent:e];
  return true;
}

bool captureWindow(const Window& w, const std::string& pngPath, const unsigned char* overlay, int overlayW, int overlayH) {
  NSView* content = contentOf(w);
  if (!content) return false;
  @autoreleasepool {
    const NSRect bounds = [content bounds];
    NSBitmapImageRep* rep = nil;
    // The window server's image of this window (GL / Metal plugin views
    // included). Resolved at run time: the call left the newest SDK headers.
    using CreateImage = CGImageRef (*)(CGRect, uint32_t, uint32_t, uint32_t);
    static auto createImage = reinterpret_cast<CreateImage>(dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"));
    bool composited = false;
    if (createImage) {
      const uint32_t id = static_cast<uint32_t>([(NSWindow*)w.handle windowNumber]);
      if (CGImageRef img = createImage(CGRectNull, 1u << 3 /* including window */, id, 1u << 0 /* ignore framing */)) {
        rep = [[[NSBitmapImageRep alloc] initWithCGImage:img] autorelease];
        CGImageRelease(img);
        composited = rep != nil;
      }
    }
    if (!rep) {
      // Views as they draw themselves (layer-backed GL / Metal views stay black).
      rep = [content bitmapImageRepForCachingDisplayInRect:bounds];
      if (!rep) return false;
      [content cacheDisplayInRect:bounds toBitmapImageRep:rep];
    }
    NSInteger pw = [rep pixelsWide], ph = [rep pixelsHigh];
    if (composited && ph > overlayH && overlay) {
      // The window image includes the title bar: keep the content area.
      const CGFloat scale = [(NSWindow*)w.handle backingScaleFactor];
      const NSInteger contentH = static_cast<NSInteger>(bounds.size.height * scale);
      if (ph > contentH) {
        CGImageRef full = [rep CGImage];
        CGImageRef cropped = CGImageCreateWithImageInRect(full, CGRectMake(0, ph - contentH, pw, contentH));
        if (cropped) {
          rep = [[[NSBitmapImageRep alloc] initWithCGImage:cropped] autorelease];
          CGImageRelease(cropped);
          pw = [rep pixelsWide], ph = [rep pixelsHigh];
        }
      }
    }
    if (composited) overlay = nullptr;  // the window server image has the GL bar already
    if (overlay && overlayW > 0 && overlayH > 0) {
      // GL rows are bottom-up: the overlay's first row is the window's bottom row.
      const NSInteger cols = std::min<NSInteger>(pw, overlayW);
      for (NSInteger y = 0; y < std::min<NSInteger>(ph, overlayH); ++y)
        for (NSInteger x = 0; x < cols; ++x) {
          const unsigned char* p = overlay + (static_cast<size_t>(y) * static_cast<size_t>(overlayW) + static_cast<size_t>(x)) * 4;
          NSUInteger px[4] = {p[0], p[1], p[2], 255};
          [rep setPixel:px atX:x y:ph - 1 - y];
        }
    }
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:[NSString stringWithUTF8String:pngPath.c_str()] atomically:YES];
  }
}

long windowLevel(const Window& w) { return w.handle ? static_cast<long>([(NSWindow*)w.handle level]) : 0; }

bool captureStack(const Window& main, const unsigned char* rgba, int w, int h, const std::string& pngPath) {
  NSWindow* m = (NSWindow*)main.handle;
  if (!m || !rgba || w <= 0 || h <= 0) return false;
  using CreateFromArray = CGImageRef (*)(CGRect, CFArrayRef, uint32_t);
  static auto createImage = reinterpret_cast<CreateFromArray>(dlsym(RTLD_DEFAULT, "CGWindowListCreateImageFromArray"));
  @autoreleasepool {
    const NSRect cf = [m contentRectForFrameRect:[m frame]];
    NSScreen* primary = [[NSScreen screens] firstObject];
    if (!primary) return false;
    const CGFloat primaryH = [primary frame].size.height;
    const CGRect area = CGRectMake(cf.origin.x, primaryH - cf.origin.y - cf.size.height, cf.size.width, cf.size.height);
    std::vector<const void*> ids;
    for (NSWindow* o in [NSApp orderedWindows])  // front to back
      if (o != m && [o isVisible] && NSIntersectsRect([o frame], [m frame])) ids.push_back(reinterpret_cast<const void*>(static_cast<uintptr_t>([o windowNumber])));
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(nullptr, static_cast<size_t>(w), static_cast<size_t>(h), 8, static_cast<size_t>(w) * 4, cs,
                                             static_cast<uint32_t>(kCGImageAlphaPremultipliedLast));
    if (!ctx) {
      CGColorSpaceRelease(cs);
      return false;
    }
    // The main window's own frame: GL rows are bottom-up, as a bitmap context's (origin bottom-left).
    std::vector<unsigned char> flip(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) std::memcpy(flip.data() + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 4, rgba + static_cast<size_t>(y) * static_cast<size_t>(w) * 4, static_cast<size_t>(w) * 4);
    std::memcpy(CGBitmapContextGetData(ctx), flip.data(), flip.size());
    bool ok = true;
    if (!ids.empty() && createImage) {
      CFArrayRef arr = CFArrayCreate(nullptr, ids.data(), static_cast<CFIndex>(ids.size()), nullptr);
      CGImageRef top = createImage(area, arr, 1u << 0 /* ignore framing */);
      CFRelease(arr);
      if (top) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), top);
        CGImageRelease(top);
      } else {
        ok = false;  // the window server gave nothing (no permission to read windows)
      }
    }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    if (!img) return false;
    NSBitmapImageRep* rep = [[[NSBitmapImageRep alloc] initWithCGImage:img] autorelease];
    CGImageRelease(img);
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return ok && png && [png writeToFile:[NSString stringWithUTF8String:pngPath.c_str()] atomically:YES];
  }
}

}  // namespace gl::host::native

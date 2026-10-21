// Gridline — native pieces of a plugin window (macOS / AppKit).
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include "NativeView.h"

// Container view for a plugin's interface: flipped is irrelevant to plugins
// (they place their own view at the origin), but it must not let the window
// background or the GL surface underneath draw over it. Keys the interface
// passes up the responder chain arrive here and go to Gridline (J-66).
@interface GridlinePluginContainer : NSView
@end
@implementation GridlinePluginContainer
- (BOOL)isOpaque {
  return NO;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  (void)event;
  return YES;  // first click goes straight to the plugin's control
}
- (void)keyDown:(NSEvent*)e {
  // Not passed on: the window would beep for a key nobody handled.
  if (!gl::host::native::containerKey(self, e, true)) [super keyDown:e];
}
- (void)keyUp:(NSEvent*)e {
  if (!gl::host::native::containerKey(self, e, false)) [super keyUp:e];
}
@end

// Test views (devTestKeyView): an interface view that passes keys on, one
// that uses every key.
@interface GridlineTestKeyPassView : NSView
@end
@implementation GridlineTestKeyPassView
- (BOOL)acceptsFirstResponder {
  return YES;
}
@end
@interface GridlineTestKeyEatView : NSView
@end
@implementation GridlineTestKeyEatView
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (void)keyDown:(NSEvent*)e {
  (void)e;
}
- (void)keyUp:(NSEvent*)e {
  (void)e;
}
@end

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
  GridlinePluginContainer* v = [[GridlinePluginContainer alloc] initWithFrame:toFrame(content, r)];
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

double backingScale(const Window& w) {
  if (!w.handle) return 1.0;
  return [(NSWindow*)w.handle backingScaleFactor];
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
    v = [[GridlineTestKeyEatView alloc] initWithFrame:r];
  } else {
    v = [[GridlineTestKeyPassView alloc] initWithFrame:r];
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

}  // namespace gl::host::native

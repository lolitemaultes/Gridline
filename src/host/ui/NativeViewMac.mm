// Gridline — native pieces of a plugin window (macOS / AppKit).
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <algorithm>
#include "NativeView.h"

// Container view for a plugin's interface: flipped is irrelevant to plugins
// (they place their own view at the origin), but it must not let the window
// background or the GL surface underneath draw over it.
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
@end

namespace gl::host::native {

namespace {
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

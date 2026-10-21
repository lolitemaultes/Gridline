// Gridline — the plugin's editor on macOS (PH-09): an NSView child of the
// host's view with its own OpenGL 3.2 core context (one share group for the
// process), redrawn by a main-run-loop timer at 60 Hz. Mouse, wheel, keys and
// text go to the view's Dear ImGui context; HiDPI through the backing scale.
// Manual reference counting (the project builds Objective-C++ without ARC).
//
// Hosted plugins' windows (MacHostWindow): an NSWindow added as a child of
// the host's window that shows Gridline (ordered above it, moving with it),
// its content top-left based: the plugin's interface in a container view on
// top, the control bar (a GL view drawing the bar's surface) under it. The
// window becomes key when Gridline's editor had the keyboard; closing it
// gives the keyboard back to the host's window.
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl3.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "PluginCore.h"
#include "PluginView.h"
#include "../host/ui/NativeView.h"

// NSOpenGL is deprecated (still supported); the editor uses it like SDL does.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// Each plugin binary (CLAP, VST3) registers its own class name, so loading
// both into one host never makes the Objective-C runtime pick one at random.
#ifndef GRIDLINE_PLUGIN_VIEW_CLASS
#define GRIDLINE_PLUGIN_VIEW_CLASS GridlinePluginGLView
#endif

// The hosted plugins' windows use classes named after the view class.
#define GRIDLINE_CAT2(a, b) a##b
#define GRIDLINE_CAT(a, b) GRIDLINE_CAT2(a, b)
#define GRIDLINE_HOSTED_CONTENT_CLASS GRIDLINE_CAT(GRIDLINE_PLUGIN_VIEW_CLASS, HostedContent)
#define GRIDLINE_HOSTED_CONTAINER_CLASS GRIDLINE_CAT(GRIDLINE_PLUGIN_VIEW_CLASS, HostedContainer)
#define GRIDLINE_HOSTED_DELEGATE_CLASS GRIDLINE_CAT(GRIDLINE_PLUGIN_VIEW_CLASS, HostedDelegate)

namespace gl::plugin {
class MacView;
class MacHostWindow;
}

@interface GRIDLINE_PLUGIN_VIEW_CLASS : NSView {
 @public
  gl::plugin::MacView* owner;
  NSTrackingArea* tracking;
}
@end

// Content of a hosted plugin's window: top-left based (the J-29 layout's units).
@interface GRIDLINE_HOSTED_CONTENT_CLASS : NSView
@end
// The view the hosted plugin's interface is attached to.
@interface GRIDLINE_HOSTED_CONTAINER_CLASS : NSView
@end
@interface GRIDLINE_HOSTED_DELEGATE_CLASS : NSObject <NSWindowDelegate> {
 @public
  gl::plugin::MacHostWindow* owner;
}
@end

namespace gl::plugin {

bool viewSupported(WindowApi api) { return api == WindowApi::Cocoa; }
WindowApi nativeWindowApi() { return WindowApi::Cocoa; }

namespace {

NSOpenGLPixelFormat* pixelFormat() {
  static NSOpenGLPixelFormat* fmt = [] {
    NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
                                            NSOpenGLPFAColorSize,     24,
                                            NSOpenGLPFAAlphaSize,     8,
                                            NSOpenGLPFAStencilSize,   8,
                                            NSOpenGLPFADoubleBuffer,  NSOpenGLPFAAccelerated,
                                            0};
    return [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
  }();
  return fmt;
}

// Root of the share group: textures (logo mark, waveforms) are shared by every view.
NSOpenGLContext* rootContext() {
  static NSOpenGLContext* ctx = pixelFormat() ? [[NSOpenGLContext alloc] initWithFormat:pixelFormat() shareContext:nil] : nil;
  return ctx;
}

ImGuiKey keyOf(unsigned short code) {
  switch (code) {
    case kVK_ANSI_A: return ImGuiKey_A;
    case kVK_ANSI_B: return ImGuiKey_B;
    case kVK_ANSI_C: return ImGuiKey_C;
    case kVK_ANSI_D: return ImGuiKey_D;
    case kVK_ANSI_E: return ImGuiKey_E;
    case kVK_ANSI_F: return ImGuiKey_F;
    case kVK_ANSI_G: return ImGuiKey_G;
    case kVK_ANSI_H: return ImGuiKey_H;
    case kVK_ANSI_I: return ImGuiKey_I;
    case kVK_ANSI_J: return ImGuiKey_J;
    case kVK_ANSI_K: return ImGuiKey_K;
    case kVK_ANSI_L: return ImGuiKey_L;
    case kVK_ANSI_M: return ImGuiKey_M;
    case kVK_ANSI_N: return ImGuiKey_N;
    case kVK_ANSI_O: return ImGuiKey_O;
    case kVK_ANSI_P: return ImGuiKey_P;
    case kVK_ANSI_Q: return ImGuiKey_Q;
    case kVK_ANSI_R: return ImGuiKey_R;
    case kVK_ANSI_S: return ImGuiKey_S;
    case kVK_ANSI_T: return ImGuiKey_T;
    case kVK_ANSI_U: return ImGuiKey_U;
    case kVK_ANSI_V: return ImGuiKey_V;
    case kVK_ANSI_W: return ImGuiKey_W;
    case kVK_ANSI_X: return ImGuiKey_X;
    case kVK_ANSI_Y: return ImGuiKey_Y;
    case kVK_ANSI_Z: return ImGuiKey_Z;
    case kVK_ANSI_0: return ImGuiKey_0;
    case kVK_ANSI_1: return ImGuiKey_1;
    case kVK_ANSI_2: return ImGuiKey_2;
    case kVK_ANSI_3: return ImGuiKey_3;
    case kVK_ANSI_4: return ImGuiKey_4;
    case kVK_ANSI_5: return ImGuiKey_5;
    case kVK_ANSI_6: return ImGuiKey_6;
    case kVK_ANSI_7: return ImGuiKey_7;
    case kVK_ANSI_8: return ImGuiKey_8;
    case kVK_ANSI_9: return ImGuiKey_9;
    case kVK_ANSI_Minus: return ImGuiKey_Minus;
    case kVK_ANSI_Equal: return ImGuiKey_Equal;
    case kVK_ANSI_LeftBracket: return ImGuiKey_LeftBracket;
    case kVK_ANSI_RightBracket: return ImGuiKey_RightBracket;
    case kVK_ANSI_Backslash: return ImGuiKey_Backslash;
    case kVK_ANSI_Semicolon: return ImGuiKey_Semicolon;
    case kVK_ANSI_Quote: return ImGuiKey_Apostrophe;
    case kVK_ANSI_Comma: return ImGuiKey_Comma;
    case kVK_ANSI_Period: return ImGuiKey_Period;
    case kVK_ANSI_Slash: return ImGuiKey_Slash;
    case kVK_ANSI_Grave: return ImGuiKey_GraveAccent;
    case kVK_Return: return ImGuiKey_Enter;
    case kVK_ANSI_KeypadEnter: return ImGuiKey_KeypadEnter;
    case kVK_Tab: return ImGuiKey_Tab;
    case kVK_Space: return ImGuiKey_Space;
    case kVK_Delete: return ImGuiKey_Backspace;
    case kVK_ForwardDelete: return ImGuiKey_Delete;
    case kVK_Escape: return ImGuiKey_Escape;
    case kVK_LeftArrow: return ImGuiKey_LeftArrow;
    case kVK_RightArrow: return ImGuiKey_RightArrow;
    case kVK_UpArrow: return ImGuiKey_UpArrow;
    case kVK_DownArrow: return ImGuiKey_DownArrow;
    case kVK_Home: return ImGuiKey_Home;
    case kVK_End: return ImGuiKey_End;
    case kVK_PageUp: return ImGuiKey_PageUp;
    case kVK_PageDown: return ImGuiKey_PageDown;
    case kVK_F1: return ImGuiKey_F1;
    case kVK_F2: return ImGuiKey_F2;
    case kVK_F3: return ImGuiKey_F3;
    case kVK_F4: return ImGuiKey_F4;
    case kVK_F5: return ImGuiKey_F5;
    case kVK_F6: return ImGuiKey_F6;
    case kVK_F7: return ImGuiKey_F7;
    case kVK_F8: return ImGuiKey_F8;
    case kVK_F9: return ImGuiKey_F9;
    case kVK_F10: return ImGuiKey_F10;
    case kVK_F11: return ImGuiKey_F11;
    case kVK_F12: return ImGuiKey_F12;
    case kVK_ANSI_Keypad0: return ImGuiKey_Keypad0;
    case kVK_ANSI_Keypad1: return ImGuiKey_Keypad1;
    case kVK_ANSI_Keypad2: return ImGuiKey_Keypad2;
    case kVK_ANSI_Keypad3: return ImGuiKey_Keypad3;
    case kVK_ANSI_Keypad4: return ImGuiKey_Keypad4;
    case kVK_ANSI_Keypad5: return ImGuiKey_Keypad5;
    case kVK_ANSI_Keypad6: return ImGuiKey_Keypad6;
    case kVK_ANSI_Keypad7: return ImGuiKey_Keypad7;
    case kVK_ANSI_Keypad8: return ImGuiKey_Keypad8;
    case kVK_ANSI_Keypad9: return ImGuiKey_Keypad9;
    case kVK_ANSI_KeypadDecimal: return ImGuiKey_KeypadDecimal;
    case kVK_ANSI_KeypadDivide: return ImGuiKey_KeypadDivide;
    case kVK_ANSI_KeypadMultiply: return ImGuiKey_KeypadMultiply;
    case kVK_ANSI_KeypadMinus: return ImGuiKey_KeypadSubtract;
    case kVK_ANSI_KeypadPlus: return ImGuiKey_KeypadAdd;
    default: return ImGuiKey_None;
  }
}

std::string& clipboardText() {
  static std::string s;
  return s;
}

}  // namespace

// A GL view drawing a Surface: Gridline's workspace (the editor, with its
// Instance) or the control bar of a hosted plugin's window.
class MacView final : public View {
 public:
  explicit MacView(Instance& inst) : inst_(&inst), own_(std::make_unique<Frame>(inst)), surface_(own_.get()) {}
  // A bar: no minimum size, no instance housekeeping.
  MacView(Surface& s, int w, int h) : surface_(&s), minW_(1), minH_(1), w_(w), h_(h) {}
  ~MacView() override { detach(); }

  bool attach(void* parent) override {
    NSView* p = static_cast<NSView*>(parent);
    if (!p || view_ || !pixelFormat()) return false;
    GRIDLINE_PLUGIN_VIEW_CLASS* v = [[GRIDLINE_PLUGIN_VIEW_CLASS alloc] initWithFrame:NSMakeRect(0, 0, w_, h_)];
    v->owner = this;
    [v setWantsBestResolutionOpenGLSurface:YES];
    [v setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [p addSubview:v];
    view_ = v;
    ctx_ = [[NSOpenGLContext alloc] initWithFormat:pixelFormat() shareContext:rootContext()];
    if (!ctx_) {
      detach();
      return false;
    }
    GLint swap = inst_ ? 1 : 0;  // bars never wait for the display (the editor paces)
    [ctx_ setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];
    MacView* self = this;
    timer_ = [[NSTimer timerWithTimeInterval:1.0 / 60.0
                                     repeats:YES
                                       block:^(NSTimer*) {
                                         self->render();
                                       }] retain];
    [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
    if (inst_) inst_->setEditorView(this);
    return true;
  }

  void detach() override {
    if (inst_ && inst_->editorView() == this) {
      closeHostedWindows(*inst_);  // their bars share this frame's fonts
      inst_->setEditorView(nullptr);
    }
    if (timer_) {
      [timer_ invalidate];
      [timer_ release];
      timer_ = nil;
    }
    if (ctx_) {
      NSOpenGLContext* prev = [NSOpenGLContext currentContext];
      [ctx_ makeCurrentContext];
      surface_->shutdown();
      [NSOpenGLContext clearCurrentContext];
      [ctx_ clearDrawable];
      if (prev && prev != ctx_) [prev makeCurrentContext];
      [ctx_ release];
      ctx_ = nil;
    } else {
      surface_->shutdown();
    }
    if (view_) {
      view_->owner = nullptr;
      [view_ removeFromSuperview];
      [view_ release];
      view_ = nil;
    }
    glReady_ = false;
  }

  void setSize(int w, int h) override {
    w_ = std::max(w, minW_);
    h_ = std::max(h, minH_);
    if (view_) [view_ setFrame:NSMakeRect(0, 0, w_, h_)];
  }
  void size(int& w, int& h) const override {
    if (view_) {
      const NSRect b = [view_ bounds];
      w = static_cast<int>(b.size.width);
      h = static_cast<int>(b.size.height);
      return;
    }
    w = w_;
    h = h_;
  }
  void setScale(double) override {}
  void setVisible(bool on) override {
    if (view_) [view_ setHidden:!on];
    if (inst_ && inst_->editorView() == this) setHostedWindowsVisible(*inst_, on);
  }
  void tick() override {}
  int frames() const override { return frames_; }
  Frame* frame() override { return glReady_ ? own_.get() : nullptr; }
  NSView* nsView() const { return view_; }

  void render() {
    if (!view_ || !ctx_ || [view_ isHidden] || ![view_ window]) return;
    // Housekeeping first: hosted plugins' interfaces may bind their own GL context.
    if (inst_) inst_->idle();
    if (!view_ || !ctx_) return;
    if ([ctx_ view] != view_) [ctx_ setView:view_];
    [ctx_ makeCurrentContext];
    if (!glReady_) {
      glReady_ = surface_->init("#version 150");
      if (!glReady_) return;
      surface_->makeCurrent();
      ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
      pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:[NSString stringWithUTF8String:text ? text : ""] forType:NSPasteboardTypeString];
      };
      pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        clipboardText() = s ? std::string([s UTF8String]) : std::string();
        return clipboardText().c_str();
      };
    }
    const NSRect b = [view_ bounds];
    const NSRect px = [view_ convertRectToBacking:b];
    const int fw = static_cast<int>(px.size.width), fh = static_cast<int>(px.size.height);
    if (fw != lastFbW_ || fh != lastFbH_) {
      [ctx_ update];
      lastFbW_ = fw;
      lastFbH_ = fh;
    }
    surface_->draw(static_cast<int>(b.size.width), static_cast<int>(b.size.height), fw, fh);
    if (inst_) devShot(fw, fh);
    if (capture_) {  // the bar's pixels for a window capture (rows bottom-up)
      capture_->assign(static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4, 0);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, capture_->data());
      captureW_ = fw;
      captureH_ = fh;
      capture_ = nullptr;
    }
    [ctx_ flushBuffer];
    updateCursor();
    ++frames_;
  }
  // The next frame's pixels go to `px` (window captures).
  void readNextFrame(std::vector<unsigned char>* px) { capture_ = px; }
  int capturedW() const { return captureW_; }
  int capturedH() const { return captureH_; }

  // ---- input (called by the NSView) ----
  void mousePos(NSEvent* e) {
    if (!glReady_) return;
    surface_->makeCurrent();
    const NSPoint p = [view_ convertPoint:[e locationInWindow] fromView:nil];
    const float s = surface_->uiScale();
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(p.x) / s, static_cast<float>(p.y) / s);
  }
  void mouseButton(NSEvent* e, int button, bool down) {
    if (!glReady_) return;
    mousePos(e);
    modifiers([e modifierFlags]);
    ImGui::GetIO().AddMouseButtonEvent(button, down);
  }
  void mouseLeft() {
    if (!glReady_) return;
    surface_->makeCurrent();
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    inside_ = false;
  }
  void mouseEntered() { inside_ = true; }
  void wheel(NSEvent* e) {
    if (!glReady_) return;
    surface_->makeCurrent();
    double dx = [e scrollingDeltaX], dy = [e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) {
      dx *= 0.1;
      dy *= 0.1;
    }
    ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(dx), static_cast<float>(dy));
  }
  void modifiers(NSEventModifierFlags f) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (f & NSEventModifierFlagControl) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (f & NSEventModifierFlagShift) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (f & NSEventModifierFlagOption) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (f & NSEventModifierFlagCommand) != 0);
  }
  void key(NSEvent* e, bool down) {
    if (!glReady_) return;
    surface_->makeCurrent();
    ImGuiIO& io = ImGui::GetIO();
    const NSEventModifierFlags f = [e modifierFlags];
    modifiers(f);
    const ImGuiKey k = keyOf([e keyCode]);
    if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
    if (down && !(f & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) {
      NSString* chars = [e characters];
      for (NSUInteger i = 0; i < [chars length]; ++i) {
        const unichar c = [chars characterAtIndex:i];
        if (c < 0x20 || c == 0x7F || (c >= 0xF700 && c <= 0xF8FF)) continue;  // controls, function keys
        io.AddInputCharacterUTF16(c);
      }
    }
  }
  void flags(NSEvent* e) {
    if (!glReady_) return;
    surface_->makeCurrent();
    modifiers([e modifierFlags]);
  }
  // A key a hosted plugin's interface did not use (J-66).
  void hostKey(const host::KeyEvent& k) {
    if (!glReady_) return;
    surface_->makeCurrent();
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (k.mods & host::kKeyCtrl) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (k.mods & host::kKeyShift) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (k.mods & host::kKeyAlt) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (k.mods & host::kKeyCmd) != 0);
    const ImGuiKey key = keyOf(k.code);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, k.down);
  }

 private:
  // Dev check: GRIDLINE_PLUGIN_SHOT=<file.ppm> saves the 60th frame (our own GL surface only).
  void devShot(int w, int h) {
    static const char* path = std::getenv("GRIDLINE_PLUGIN_SHOT");
    if (!path || frames_ != 60 || w <= 0 || h <= 0) return;
    std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    if (FILE* f = std::fopen(path, "wb")) {
      std::fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (int y = h - 1; y >= 0; --y) std::fwrite(px.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3, 1, static_cast<size_t>(w) * 3, f);
      std::fclose(f);
    }
  }
  void updateCursor() {
    if (!inside_) return;
    const int c = surface_->cursor();
    if (c == cursor_) return;
    cursor_ = c;
    NSCursor* nc = [NSCursor arrowCursor];
    switch (c) {
      case ImGuiMouseCursor_TextInput: nc = [NSCursor IBeamCursor]; break;
      case ImGuiMouseCursor_ResizeAll: nc = [NSCursor closedHandCursor]; break;
      case ImGuiMouseCursor_ResizeNS: nc = [NSCursor resizeUpDownCursor]; break;
      case ImGuiMouseCursor_ResizeEW: nc = [NSCursor resizeLeftRightCursor]; break;
      case ImGuiMouseCursor_Hand: nc = [NSCursor pointingHandCursor]; break;
      case ImGuiMouseCursor_NotAllowed: nc = [NSCursor operationNotAllowedCursor]; break;
      default: break;
    }
    [nc set];
  }

  Instance* inst_ = nullptr;
  std::unique_ptr<Frame> own_;
  Surface* surface_ = nullptr;
  int minW_ = kMinViewW, minH_ = kMinViewH;
  GRIDLINE_PLUGIN_VIEW_CLASS* view_ = nil;
  NSOpenGLContext* ctx_ = nil;
  NSTimer* timer_ = nil;
  int w_ = kDefaultViewW, h_ = kDefaultViewH;
  int lastFbW_ = 0, lastFbH_ = 0;
  int frames_ = 0;
  int cursor_ = -1;
  bool glReady_ = false;
  bool inside_ = false;
  std::vector<unsigned char>* capture_ = nullptr;
  int captureW_ = 0, captureH_ = 0;
};

std::unique_ptr<View> createView(Instance& inst) { return std::make_unique<MacView>(inst); }

// ---------------------------------------------------------------- hosted plugins' windows

namespace {

// Global top-left based coordinates <-> AppKit screen coordinates (origin at
// the bottom-left of the primary screen).
CGFloat primaryHeight() {
  NSArray<NSScreen*>* screens = [NSScreen screens];
  return [screens count] > 0 ? [[screens objectAtIndex:0] frame].size.height : 900;
}

}  // namespace

class MacHostWindow final : public HostWindow {
 public:
  MacHostWindow(Surface& bar, int w, int h) : bar_(bar, w, h) {}
  ~MacHostWindow() override {
    bar_.detach();
    if (container_) {
      host::native::unwatchKeys(container_);
      [container_ removeFromSuperview];
      [container_ release];
      container_ = nil;
    }
    if (editorView_) {
      [editorView_ release];
      editorView_ = nil;
    }
    if (holder_) {
      [holder_ removeFromSuperview];
      [holder_ release];
      holder_ = nil;
    }
    if (win_) {
      const bool wasKey = [win_ isKeyWindow];
      NSWindow* parent = [win_ parentWindow];
      if (parent) [parent removeChildWindow:win_];
      [win_ orderOut:nil];
      [win_ setDelegate:nil];
      [win_ close];
      [win_ release];
      win_ = nil;
      if (wasKey && parent && [parent isVisible]) [parent makeKeyWindow];  // the keyboard goes back to the host's window
    }
    if (delegate_) {
      delegate_->owner = nullptr;
      [delegate_ release];
      delegate_ = nil;
    }
  }

  bool create(NSView* ownerView, const std::string& title, const host::layout::WindowLayout& L, bool resizable, bool barOnly, const int* place,
              int index) {
    owner_ = [ownerView window];
    if (!owner_) return false;
    editorView_ = [ownerView retain];
    NSWindowStyleMask style = barOnly ? NSWindowStyleMaskBorderless
                                      : (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                         (resizable ? NSWindowStyleMaskResizable : 0));
    win_ = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, L.w, L.h) styleMask:style backing:NSBackingStoreBuffered defer:NO];
    if (!win_) return false;
    [win_ setReleasedWhenClosed:NO];
    [win_ setTitle:[NSString stringWithUTF8String:title.c_str()]];
    [win_ setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    GRIDLINE_HOSTED_CONTENT_CLASS* content = [[GRIDLINE_HOSTED_CONTENT_CLASS alloc] initWithFrame:NSMakeRect(0, 0, L.w, L.h)];
    [win_ setContentView:content];
    [content release];
    delegate_ = [[GRIDLINE_HOSTED_DELEGATE_CLASS alloc] init];
    delegate_->owner = this;
    [win_ setDelegate:delegate_];
    // The host's window is a hidden check window (plugin checks, tests):
    // so is this one.
    if ([owner_ alphaValue] == 0.0) {
      [win_ setAlphaValue:0.0];
      [win_ setIgnoresMouseEvents:YES];
      offscreen_ = true;
    }
    if (!barOnly) {
      container_ = [[GRIDLINE_HOSTED_CONTAINER_CLASS alloc] initWithFrame:NSMakeRect(L.plugin.x, L.plugin.y, L.plugin.w, L.plugin.h)];
      if ([container_ respondsToSelector:@selector(setClipsToBounds:)]) [container_ setClipsToBounds:YES];
      [container_ setWantsLayer:YES];
      [[container_ layer] setMasksToBounds:YES];
      [content addSubview:container_];
      host::native::watchKeys(container_);  // keys the interface does not use go to Gridline's editor
    }
    holder_ = [[NSView alloc] initWithFrame:NSMakeRect(0, L.bar.y, L.w, std::max(1, L.h - L.bar.y))];
    [content addSubview:holder_];
    if (!bar_.attach(holder_)) return false;
    setLayout(L);
    // Position: remembered, else cascaded from the top-left of the host's window.
    const NSRect of = [owner_ frame];
    int x = 0, y = 0;
    if (place) {
      x = place[0];
      y = place[1];
    } else {
      x = static_cast<int>(of.origin.x) + 40 + 26 * (index % 8);
      y = static_cast<int>(primaryHeight() - NSMaxY(of)) + 60 + 26 * (index % 8);
    }
    moveTopLeft(x, y);
    if (!offscreen_) {  // keep the title bar on a screen
      NSScreen* sc = [owner_ screen] ? [owner_ screen] : [NSScreen mainScreen];
      const NSRect vis = [sc visibleFrame];
      NSRect f = [win_ frame];
      f.origin.x = std::clamp(f.origin.x, vis.origin.x, std::max(vis.origin.x, NSMaxX(vis) - f.size.width));
      f.origin.y = std::clamp(f.origin.y, vis.origin.y, std::max(vis.origin.y, NSMaxY(vis) - f.size.height));
      [win_ setFrameOrigin:f.origin];
    }
    if (resizable) [win_ setContentMinSize:NSMakeSize(host::layout::kMinWindowW, L.bar.h + host::layout::kMinPluginH)];
    return true;
  }

  void* container() const override { return container_; }

  void setLayout(const host::layout::WindowLayout& L) override {
    if (!win_) return;
    ++applying_;
    const NSRect fr = [win_ frame];
    const CGFloat top = NSMaxY(fr);
    NSRect cr = [win_ contentRectForFrameRect:fr];
    if (static_cast<int>(cr.size.width) != L.w || static_cast<int>(cr.size.height) != L.h) {
      cr.size = NSMakeSize(L.w, L.h);
      NSRect nf = [win_ frameRectForContentRect:cr];
      nf.origin.y = top - nf.size.height;  // the window grows downwards, like the application's
      [win_ setFrame:nf display:YES];
    }
    if (container_) {
      const NSRect pf = NSMakeRect(L.plugin.x, L.plugin.y, L.plugin.w, L.plugin.h);
      if (!NSEqualRects([container_ frame], pf)) [container_ setFrame:pf];
    }
    const int bh = std::max(1, L.h - L.bar.y);
    [holder_ setFrame:NSMakeRect(0, L.bar.y, L.w, bh)];
    bar_.setSize(L.w, bh);
    --applying_;
  }

  void show() override {
    if (!win_) return;
    shown_ = true;
    if ([win_ parentWindow] != owner_) [owner_ addChildWindow:win_ ordered:NSWindowAbove];
    if (offscreen_) {
      [win_ orderFront:nil];
      return;
    }
    // The plugin's interface takes the keyboard when Gridline's editor had it.
    if ([owner_ isKeyWindow] || [NSApp keyWindow] == nil)
      [win_ makeKeyAndOrderFront:nil];
    else
      [win_ orderFront:nil];
  }
  void setVisible(bool on) override {
    if (!win_ || !shown_) return;
    if (on) {
      if ([win_ parentWindow] != owner_) [owner_ addChildWindow:win_ ordered:NSWindowAbove];
      [win_ orderFront:nil];
    } else {
      if ([win_ parentWindow]) [[win_ parentWindow] removeChildWindow:win_];
      [win_ orderOut:nil];
    }
  }
  void raise() override {
    if (!win_) return;
    if (offscreen_)
      [win_ orderFront:nil];
    else
      [win_ makeKeyAndOrderFront:nil];
  }
  bool topLeft(int& x, int& y) const override {
    if (!win_) return false;
    const NSRect f = [win_ frame];
    x = static_cast<int>(f.origin.x);
    y = static_cast<int>(primaryHeight() - NSMaxY(f));
    return true;
  }
  void moveTopLeft(int x, int y) override {
    if (win_) [win_ setFrameTopLeftPoint:NSMakePoint(x, primaryHeight() - y)];
  }
  double backingScale() const override { return win_ ? [win_ backingScaleFactor] : 1.0; }
  void tick() override {
    host::KeyEvent k;
    while (container_ && host::native::popKey(container_, k)) forwardKey(k);
  }
  void forwardKey(const host::KeyEvent& k) override {
    if (editorView_ && [editorView_ isKindOfClass:[GRIDLINE_PLUGIN_VIEW_CLASS class]]) {
      gl::plugin::MacView* v = ((GRIDLINE_PLUGIN_VIEW_CLASS*)editorView_)->owner;
      if (v) v->hostKey(k);
    }
  }
  int barFrames() const override { return bar_.frames(); }
  bool ownedByEditor() const override { return win_ && owner_ && [win_ parentWindow] == owner_; }
  bool visible() const override { return win_ && [win_ isVisible]; }
  bool pluginViewSize(int& w, int& h) const override {
    if (!container_ || [[container_ subviews] count] == 0) return false;
    const NSRect f = [[[container_ subviews] objectAtIndex:0] frame];
    w = static_cast<int>(f.size.width);
    h = static_cast<int>(f.size.height);
    return true;
  }
  bool capture(const std::string& pngPath) override {
    if (!win_) return false;
    // The bar's pixels (GL surfaces do not draw into a view cache), laid
    // over the bottom of the window's own image.
    // Views as they draw themselves (works for windows that are not on
    // screen), the GL bar read back from its context.
    std::vector<unsigned char> px;
    bar_.readNextFrame(&px);
    for (int k = 0; k < 6 && px.empty(); ++k) bar_.render();
    bar_.readNextFrame(nullptr);
    @autoreleasepool {
      NSView* content = [win_ contentView];
      const NSRect bounds = [content bounds];
      NSBitmapImageRep* rep = [content bitmapImageRepForCachingDisplayInRect:bounds];
      if (!rep) return false;
      [content cacheDisplayInRect:bounds toBitmapImageRep:rep];
      const NSInteger pw = [rep pixelsWide], ph = [rep pixelsHigh];
      const NSInteger ow = bar_.capturedW(), oh = bar_.capturedH();
      if (!px.empty())  // GL rows are bottom-up: the first is the window's bottom row
        for (NSInteger y = 0; y < std::min(ph, oh); ++y)
          for (NSInteger x = 0; x < std::min(pw, ow); ++x) {
            const unsigned char* p = px.data() + (static_cast<size_t>(y) * static_cast<size_t>(ow) + static_cast<size_t>(x)) * 4;
            NSUInteger c[4] = {p[0], p[1], p[2], 255};
            [rep setPixel:c atX:x y:ph - 1 - y];
          }
      NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
      return png && [png writeToFile:[NSString stringWithUTF8String:pngPath.c_str()] atomically:YES];
    }
  }

  // From the window's delegate.
  void userClosed() {
    if (onClose) onClose();
  }
  void userResized() {
    if (applying_ || !win_ || !onUserResize) return;
    const NSRect cr = [win_ contentRectForFrameRect:[win_ frame]];
    onUserResize(static_cast<int>(cr.size.width), static_cast<int>(cr.size.height));
  }

 private:
  MacView bar_;
  NSWindow* owner_ = nil;  // the host's window that shows Gridline's editor
  NSWindow* win_ = nil;
  NSView* container_ = nil;
  NSView* holder_ = nil;
  NSView* editorView_ = nil;  // Gridline's editor (retained): unused plugin keys go there
  GRIDLINE_HOSTED_DELEGATE_CLASS* delegate_ = nil;
  int applying_ = 0;
  bool offscreen_ = false;
  bool shown_ = false;
};

std::unique_ptr<HostWindow> createHostWindow(View& owner, const std::string& title, const host::layout::WindowLayout& L, bool resizable,
                                             bool barOnly, Surface& bar, const int* place, int index) {
  NSView* ov = static_cast<MacView&>(owner).nsView();
  if (!ov) return nullptr;
  auto w = std::make_unique<MacHostWindow>(bar, L.w, std::max(1, L.h - L.bar.y));
  if (!w->create(ov, title, L, resizable, barOnly, place, index)) return nullptr;
  return w;
}

GlBinding currentGl() {
  GlBinding g;
  g.a = [NSOpenGLContext currentContext];
  return g;
}
void restoreGl(const GlBinding& g) {
  if (g.a)
    [static_cast<NSOpenGLContext*>(g.a) makeCurrentContext];
  else
    [NSOpenGLContext clearCurrentContext];
}

}  // namespace gl::plugin

@implementation GRIDLINE_HOSTED_CONTENT_CLASS
- (BOOL)isFlipped {
  return YES;
}
@end

@implementation GRIDLINE_HOSTED_CONTAINER_CLASS
- (BOOL)isOpaque {
  return NO;
}
// Keys the plugin's interface passed on (J-66), queued for Gridline's editor.
- (void)keyDown:(NSEvent*)e {
  if (!gl::host::native::containerKey(self, e, true)) [super keyDown:e];
}
- (void)keyUp:(NSEvent*)e {
  if (!gl::host::native::containerKey(self, e, false)) [super keyUp:e];
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  (void)event;
  return YES;  // the first click goes straight to the plugin's control
}
@end

@implementation GRIDLINE_HOSTED_DELEGATE_CLASS
- (BOOL)windowShouldClose:(NSWindow*)sender {
  (void)sender;
  if (owner) owner->userClosed();  // closed on the next idle, by Gridline
  return NO;
}
- (void)windowDidResize:(NSNotification*)note {
  (void)note;
  if (owner) owner->userResized();
}
@end

@implementation GRIDLINE_PLUGIN_VIEW_CLASS
- (BOOL)isFlipped {
  return YES;
}
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  (void)event;
  return YES;
}
- (void)updateTrackingAreas {
  if (tracking) {
    [self removeTrackingArea:tracking];
    [tracking release];
  }
  tracking = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                          options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                                                  NSTrackingInVisibleRect
                                            owner:self
                                         userInfo:nil];
  [self addTrackingArea:tracking];
  [super updateTrackingAreas];
}
- (void)dealloc {
  if (tracking) {
    [self removeTrackingArea:tracking];
    [tracking release];
  }
  [super dealloc];
}
- (void)mouseDown:(NSEvent*)e {
  [[self window] makeFirstResponder:self];
  if (owner) owner->mouseButton(e, 0, true);
}
- (void)mouseUp:(NSEvent*)e {
  if (owner) owner->mouseButton(e, 0, false);
}
- (void)rightMouseDown:(NSEvent*)e {
  [[self window] makeFirstResponder:self];
  if (owner) owner->mouseButton(e, 1, true);
}
- (void)rightMouseUp:(NSEvent*)e {
  if (owner) owner->mouseButton(e, 1, false);
}
- (void)otherMouseDown:(NSEvent*)e {
  if (owner) owner->mouseButton(e, 2, true);
}
- (void)otherMouseUp:(NSEvent*)e {
  if (owner) owner->mouseButton(e, 2, false);
}
- (void)mouseMoved:(NSEvent*)e {
  if (owner) owner->mousePos(e);
}
- (void)mouseDragged:(NSEvent*)e {
  if (owner) owner->mousePos(e);
}
- (void)rightMouseDragged:(NSEvent*)e {
  if (owner) owner->mousePos(e);
}
- (void)otherMouseDragged:(NSEvent*)e {
  if (owner) owner->mousePos(e);
}
- (void)mouseEntered:(NSEvent*)e {
  (void)e;
  if (owner) owner->mouseEntered();
}
- (void)mouseExited:(NSEvent*)e {
  (void)e;
  if (owner) owner->mouseLeft();
}
- (void)scrollWheel:(NSEvent*)e {
  if (owner) owner->wheel(e);
}
- (void)keyDown:(NSEvent*)e {
  if (owner) owner->key(e, true);
}
- (void)keyUp:(NSEvent*)e {
  if (owner) owner->key(e, false);
}
- (void)flagsChanged:(NSEvent*)e {
  if (owner) owner->flags(e);
}
@end
#endif

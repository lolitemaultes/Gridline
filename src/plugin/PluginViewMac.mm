// Gridline — the plugin's editor on macOS (PH-09): an NSView child of the
// host's view with its own OpenGL 3.2 core context (one share group for the
// process), redrawn by a main-run-loop timer at 60 Hz. Mouse, wheel, keys and
// text go to the view's Dear ImGui context; HiDPI through the backing scale.
// Manual reference counting (the project builds Objective-C++ without ARC).
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl3.h>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "PluginCore.h"
#include "PluginView.h"

// NSOpenGL is deprecated (still supported); the editor uses it like SDL does.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// Each plugin binary (CLAP, VST3) registers its own class name, so loading
// both into one host never makes the Objective-C runtime pick one at random.
#ifndef GRIDLINE_PLUGIN_VIEW_CLASS
#define GRIDLINE_PLUGIN_VIEW_CLASS GridlinePluginGLView
#endif

namespace gl::plugin {
class MacView;
}

@interface GRIDLINE_PLUGIN_VIEW_CLASS : NSView {
 @public
  gl::plugin::MacView* owner;
  NSTrackingArea* tracking;
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

class MacView final : public View {
 public:
  explicit MacView(Instance& inst) : inst_(inst), frame_(inst) {}
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
    GLint swap = 1;
    [ctx_ setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];
    MacView* self = this;
    timer_ = [[NSTimer timerWithTimeInterval:1.0 / 60.0
                                     repeats:YES
                                       block:^(NSTimer*) {
                                         self->render();
                                       }] retain];
    [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
    return true;
  }

  void detach() override {
    if (timer_) {
      [timer_ invalidate];
      [timer_ release];
      timer_ = nil;
    }
    if (ctx_) {
      [ctx_ makeCurrentContext];
      frame_.shutdown();
      [NSOpenGLContext clearCurrentContext];
      [ctx_ clearDrawable];
      [ctx_ release];
      ctx_ = nil;
    } else {
      frame_.shutdown();
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
    w_ = std::max(w, kMinViewW);
    h_ = std::max(h, kMinViewH);
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
  }
  void tick() override {}
  int frames() const override { return frames_; }

  void render() {
    if (!view_ || !ctx_ || [view_ isHidden] || ![view_ window]) return;
    if ([ctx_ view] != view_) [ctx_ setView:view_];
    [ctx_ makeCurrentContext];
    if (!glReady_) {
      glReady_ = frame_.init("#version 150");
      if (!glReady_) return;
      frame_.makeCurrent();
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
    inst_.idle();
    frame_.draw(static_cast<int>(b.size.width), static_cast<int>(b.size.height), fw, fh);
    devShot(fw, fh);
    [ctx_ flushBuffer];
    updateCursor();
    ++frames_;
  }

  // ---- input (called by the NSView) ----
  void mousePos(NSEvent* e) {
    if (!glReady_) return;
    frame_.makeCurrent();
    const NSPoint p = [view_ convertPoint:[e locationInWindow] fromView:nil];
    const float s = frame_.uiScale();
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
    frame_.makeCurrent();
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    inside_ = false;
  }
  void mouseEntered() { inside_ = true; }
  void wheel(NSEvent* e) {
    if (!glReady_) return;
    frame_.makeCurrent();
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
    frame_.makeCurrent();
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
    frame_.makeCurrent();
    modifiers([e modifierFlags]);
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
    const int c = frame_.cursor();
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

  Instance& inst_;
  Frame frame_;
  GRIDLINE_PLUGIN_VIEW_CLASS* view_ = nil;
  NSOpenGLContext* ctx_ = nil;
  NSTimer* timer_ = nil;
  int w_ = kDefaultViewW, h_ = kDefaultViewH;
  int lastFbW_ = 0, lastFbH_ = 0;
  int frames_ = 0;
  int cursor_ = -1;
  bool glReady_ = false;
  bool inside_ = false;
};

std::unique_ptr<View> createView(Instance& inst) { return std::make_unique<MacView>(inst); }

}  // namespace gl::plugin

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

// Plugin interfaces that draw with OpenGL through GLX, inside Gridline's process, while Gridline's own
// windows are Wayland windows drawing through EGL.
//
// A thread can have one current OpenGL context, from one API: while Gridline's EGL context is current,
// a plugin's glXMakeCurrent is refused, and its drawing then lands in (or crashes) a context that is not
// its own. And a GLX context the plugin leaves current keeps Gridline from binding its EGL context again.
// ForeignGlScope brackets the calls into such interfaces: Gridline's context and any GLX context are
// released before, and after them any GLX context is released and Gridline's is bound again.
// On X11 (where SDL draws through GLX like the plugins) it does nothing: the existing guards there already restore the context.
#pragma once

#include <SDL.h>

#include <cstring>
#include <optional>

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <dlfcn.h>
#endif

namespace gl::foreigngl {

inline bool hostOnEgl() {
  const char* d = SDL_GetCurrentVideoDriver();
  return d && std::strcmp(d, "wayland") == 0;
}

// Releases a GLX context left current on this thread (only when a plugin loaded the GLX library).
inline void releaseGlx() {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  using CurDisplay = void* (*)();
  using CurContext = void* (*)();
  using MakeCurrent = int (*)(void*, unsigned long, void*);
  for (const char* lib : {"libGLX.so.0", "libGL.so.1"}) {
    void* h = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD);
    if (!h) continue;
    auto curDisplay = reinterpret_cast<CurDisplay>(dlsym(h, "glXGetCurrentDisplay"));
    auto curContext = reinterpret_cast<CurContext>(dlsym(h, "glXGetCurrentContext"));
    auto makeCurrent = reinterpret_cast<MakeCurrent>(dlsym(h, "glXMakeCurrent"));
    if (curDisplay && curContext && makeCurrent && curContext()) {
      if (void* dpy = curDisplay()) makeCurrent(dpy, 0, nullptr);
    }
    dlclose(h);  // NOLOAD took a reference; the library stays loaded for the plugin
    return;
  }
#endif
}

class ForeignGlScope {
 public:
  ForeignGlScope() {
    if (!hostOnEgl()) return;
    win_ = SDL_GL_GetCurrentWindow();
    gl_ = SDL_GL_GetCurrentContext();
    if (win_ && gl_) SDL_GL_MakeCurrent(win_, nullptr);
    releaseGlx();  // one an interface left current since (its timers draw too)
    active_ = true;
  }
  ~ForeignGlScope() {
    if (!active_) return;
    releaseGlx();
    if (win_ && gl_) {
      SDL_GL_MakeCurrent(win_, nullptr);  // SDL skips a bind it believes is current already
      SDL_GL_MakeCurrent(win_, gl_);
    }
  }
  ForeignGlScope(const ForeignGlScope&) = delete;
  ForeignGlScope& operator=(const ForeignGlScope&) = delete;

 private:
  SDL_Window* win_ = nullptr;
  SDL_GLContext gl_ = nullptr;
  bool active_ = false;
};

// For hosted UI's run-loop bracket (setRunLoopGlBracket): a scope from enter to leave.
inline void runLoopBracket(bool enter) {
  static std::optional<ForeignGlScope> scope;
  if (enter)
    scope.emplace();
  else
    scope.reset();
}

}  // namespace gl::foreigngl

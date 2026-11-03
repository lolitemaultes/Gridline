// Gridline: K-47: see GlRecovery.h.
#include "GlRecovery.h"
#include <SDL_opengl.h>
#include <atomic>
#include <cstdlib>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/Log.h"

namespace gl::glrecovery {

namespace {

// (GL_KHR_robustness; not every system's headers have them.)
constexpr GLenum kContextLost = 0x0507;
constexpr GLenum kGuiltyReset = 0x8253, kInnocentReset = 0x8254, kUnknownReset = 0x8255;
#if !defined(__APPLE__)
constexpr GLenum kResetStrategy = 0x8256, kLoseOnReset = 0x8252;
#endif

#if !defined(APIENTRY)
#define APIENTRY
#endif
using ResetStatusFn = GLenum(APIENTRY*)(void);

ResetStatusFn gResetStatus = nullptr;
std::atomic<bool> gSimulated{false};

#if !defined(__APPLE__)
void drainErrors() {
  for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
  }
}
#endif

}  // namespace

SDL_GLContext createContext(SDL_Window* window) {
#if !defined(__APPLE__)
  // Robustness first; a driver that refuses it gets what it always got.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_RESET_NOTIFICATION, SDL_GL_CONTEXT_RESET_LOSE_CONTEXT);
  if (SDL_GLContext c = SDL_GL_CreateContext(window)) return c;
  logging::info("graphics", std::string("No robust OpenGL context here: ") + SDL_GetError());
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_RESET_NOTIFICATION, SDL_GL_CONTEXT_RESET_NO_NOTIFICATION);
#endif
  return SDL_GL_CreateContext(window);
}

void attach() {
  gResetStatus = nullptr;
#if !defined(__APPLE__)
  drainErrors();
  GLint strategy = 0;
  glGetIntegerv(kResetStrategy, &strategy);
  const bool loses = glGetError() == GL_NO_ERROR && static_cast<GLenum>(strategy) == kLoseOnReset;
  drainErrors();
  if (loses) {
    for (const char* name : {"glGetGraphicsResetStatus", "glGetGraphicsResetStatusARB", "glGetGraphicsResetStatusKHR"}) {
      gResetStatus = reinterpret_cast<ResetStatusFn>(SDL_GL_GetProcAddress(name));
      if (gResetStatus) break;
    }
  }
#endif
  logging::info("graphics", gResetStatus ? "The driver reports resets of the OpenGL context" : "The driver does not report resets of the OpenGL context");
}

bool canReport() { return gResetStatus != nullptr; }

const char* poll() {
  if (gSimulated.exchange(false)) return "simulated loss of the context";
  if (gResetStatus) {
    switch (gResetStatus()) {
      case GL_NO_ERROR: break;
      case kGuiltyReset: return "reset, caused by this context";
      case kInnocentReset: return "reset, caused elsewhere";
      case kUnknownReset: return "reset, cause unknown";
      default: return "reset";
    }
  }
  for (int i = 0; i < 4; ++i) {
    const GLenum e = glGetError();
    if (e == GL_NO_ERROR) break;
    if (e == kContextLost) return "context lost";
  }
  static unsigned frames = 0;
  if ((++frames & 31u) == 0 && !glGetString(GL_VERSION)) return "the context no longer answers";
  return nullptr;
}

void simulateLoss() { gSimulated = true; }

void waitForResetToEnd(int maxMs) {
  if (!gResetStatus) return;
  const Uint32 start = SDL_GetTicks();
  while (gResetStatus() != GL_NO_ERROR && SDL_GetTicks() - start < static_cast<Uint32>(maxMs)) SDL_Delay(50);
}

SDL_GLContext recreateContext(SDL_Window* window, int maxMs) {
  if (std::getenv("GRIDLINE_DEV_GL_NO_NEW_CONTEXT")) return nullptr;
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);  // (the windows that share leave it on)
  const Uint32 start = SDL_GetTicks();
  for (;;) {
    if (SDL_GLContext c = createContext(window)) return c;
    if (SDL_GetTicks() - start >= static_cast<Uint32>(maxMs)) return nullptr;
    SDL_Delay(250);
  }
}

void restoreTextures() {
  for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
    if (t->Status == ImTextureStatus_WantDestroy || t->WantDestroyNextFrame) continue;
    t->SetTexID(ImTextureID_Invalid);
    t->BackendUserData = nullptr;
    t->Status = ImTextureStatus_WantCreate;
  }
}

}  // namespace gl::glrecovery

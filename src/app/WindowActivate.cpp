// Gridline: bringing the main window forward with the later launch's token (see WindowActivate.h).
#include "WindowActivate.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <cstdint>
#include <cstring>

#if defined(SDL_VIDEO_DRIVER_WAYLAND) && !defined(__APPLE__) && !defined(_WIN32)
#include <dlfcn.h>
#define GL_WAYLAND_ACTIVATE 1
#endif

namespace gl::activate {

#if defined(GL_WAYLAND_ACTIVATE)
namespace {

// The few libwayland-client pieces this needs, declared as wayland-util.h has them (a stable ABI),
// so no Wayland headers or library are needed to build Gridline.
struct WlInterface;
struct WlMessage {
  const char* name;
  const char* signature;
  const WlInterface** types;
};
struct WlInterface {
  const char* name;
  int version;
  int methodCount;
  const WlMessage* methods;
  int eventCount;
  const WlMessage* events;
};
struct WlProxy;
struct WlDisplay;
struct WlQueue;

struct RegistryListener {
  void (*global)(void* data, WlProxy* registry, uint32_t name, const char* interface, uint32_t version);
  void (*globalRemove)(void* data, WlProxy* registry, uint32_t name);
};

// xdg-activation-v1 (staging protocol), requests only: destroy, get_activation_token, activate.
const WlInterface* gNoTypes[] = {nullptr, nullptr};
WlInterface gTokenInterface = {"xdg_activation_token_v1", 1, 0, nullptr, 0, nullptr};
const WlInterface* gTokenTypes[] = {&gTokenInterface};
const WlInterface* gActivateTypes[] = {nullptr, nullptr};  // the surface's interface, set at run time
WlMessage gActivationRequests[] = {
    {"destroy", "", gNoTypes},
    {"get_activation_token", "n", gTokenTypes},
    {"activate", "so", gActivateTypes},
};
const WlInterface gActivationInterface = {"xdg_activation_v1", 1, 3, gActivationRequests, 0, nullptr};

struct Lib {
  bool ok = false;
  const WlInterface* registryInterface = nullptr;
  const WlInterface* surfaceInterface = nullptr;
  WlQueue* (*createQueue)(WlDisplay*) = nullptr;
  void (*destroyQueue)(WlQueue*) = nullptr;
  void* (*createWrapper)(void*) = nullptr;
  void (*destroyWrapper)(void*) = nullptr;
  void (*setQueue)(WlProxy*, WlQueue*) = nullptr;
  WlProxy* (*marshalConstructor)(WlProxy*, uint32_t, const WlInterface*, ...) = nullptr;
  WlProxy* (*marshalConstructorVersioned)(WlProxy*, uint32_t, const WlInterface*, uint32_t, ...) = nullptr;
  void (*marshal)(WlProxy*, uint32_t, ...) = nullptr;
  int (*addListener)(WlProxy*, void (**)(void), void*) = nullptr;
  void (*destroy)(WlProxy*) = nullptr;
  int (*roundtripQueue)(WlDisplay*, WlQueue*) = nullptr;
  int (*flush)(WlDisplay*) = nullptr;
};

template <typename T>
void bind(void* h, const char* name, T& out, bool& ok) {
  out = reinterpret_cast<T>(dlsym(h, name));
  ok = ok && out != nullptr;
}

const Lib& lib() {
  static Lib l = [] {
    Lib r;
    // SDL's Wayland driver has it loaded already; RTLD_NOLOAD never loads anything new.
    void* h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libwayland-client.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return r;
    bool ok = true;
    r.registryInterface = static_cast<const WlInterface*>(dlsym(h, "wl_registry_interface"));
    r.surfaceInterface = static_cast<const WlInterface*>(dlsym(h, "wl_surface_interface"));
    ok = r.registryInterface && r.surfaceInterface;
    bind(h, "wl_display_create_queue", r.createQueue, ok);
    bind(h, "wl_event_queue_destroy", r.destroyQueue, ok);
    bind(h, "wl_proxy_create_wrapper", r.createWrapper, ok);
    bind(h, "wl_proxy_wrapper_destroy", r.destroyWrapper, ok);
    bind(h, "wl_proxy_set_queue", r.setQueue, ok);
    bind(h, "wl_proxy_marshal_constructor", r.marshalConstructor, ok);
    bind(h, "wl_proxy_marshal_constructor_versioned", r.marshalConstructorVersioned, ok);
    bind(h, "wl_proxy_marshal", r.marshal, ok);
    bind(h, "wl_proxy_add_listener", r.addListener, ok);
    bind(h, "wl_proxy_destroy", r.destroy, ok);
    bind(h, "wl_display_roundtrip_queue", r.roundtripQueue, ok);
    bind(h, "wl_display_flush", r.flush, ok);
    r.ok = ok;
    if (ok) gActivateTypes[1] = r.surfaceInterface;
    return r;
  }();
  return l;
}

struct Found {
  uint32_t name = 0;
};

void onGlobal(void* data, WlProxy*, uint32_t name, const char* interface, uint32_t) {
  if (interface && std::strcmp(interface, "xdg_activation_v1") == 0) static_cast<Found*>(data)->name = name;
}
void onGlobalRemove(void*, WlProxy*, uint32_t) {}

// Hands `token` to the compositor for `surface`, on a queue of its own (SDL's events stay SDL's).
bool waylandActivate(WlDisplay* display, WlProxy* surface, const std::string& token) {
  const Lib& L = lib();
  if (!L.ok || !display || !surface) return false;
  constexpr uint32_t kGetRegistry = 1, kBind = 0, kDestroy = 0, kActivate = 2;
  WlQueue* queue = L.createQueue(display);
  if (!queue) return false;
  auto* wrapper = static_cast<WlProxy*>(L.createWrapper(display));
  bool done = false;
  if (wrapper) {
    L.setQueue(wrapper, queue);
    WlProxy* registry = L.marshalConstructor(wrapper, kGetRegistry, L.registryInterface, nullptr);
    if (registry) {
      static const RegistryListener listener = {onGlobal, onGlobalRemove};
      Found found;
      L.addListener(registry, reinterpret_cast<void (**)(void)>(const_cast<RegistryListener*>(&listener)), &found);
      if (L.roundtripQueue(display, queue) >= 0 && found.name != 0) {
        WlProxy* activation =
            L.marshalConstructorVersioned(registry, kBind, &gActivationInterface, 1u, found.name, gActivationInterface.name, 1u, nullptr);
        if (activation) {
          L.marshal(activation, kActivate, token.c_str(), surface);
          L.marshal(activation, kDestroy);
          L.destroy(activation);
          done = true;
        }
      }
      L.destroy(registry);
    }
    L.destroyWrapper(wrapper);
  }
  L.flush(display);
  L.destroyQueue(queue);
  return done;
}

}  // namespace
#endif

bool bringForward(SDL_Window* w, const std::string& token) {
  if (!w) return false;
  if (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(w);
#if defined(GL_WAYLAND_ACTIVATE)
  if (!token.empty()) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(w, &info) && info.subsystem == SDL_SYSWM_WAYLAND &&
        waylandActivate(reinterpret_cast<WlDisplay*>(info.info.wl.display), reinterpret_cast<WlProxy*>(info.info.wl.surface), token))
      return true;
  }
#else
  (void)token;
#endif
  SDL_RaiseWindow(w);
  return false;
}

}  // namespace gl::activate

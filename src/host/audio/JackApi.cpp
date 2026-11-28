// Gridline: runtime-loaded JACK / PipeWire C API table (K-357). See JackApi.h.
#include "JackApi.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#include <sys/stat.h>
#endif

namespace gl::jack {

bool apiComplete(const Api& a) {
  return a.client_open && a.client_close && a.get_client_name && a.activate && a.deactivate && a.get_sample_rate &&
         a.get_buffer_size && a.set_process_callback && a.set_buffer_size_callback && a.set_sample_rate_callback &&
         a.set_xrun_callback && a.on_shutdown && a.port_register && a.port_unregister && a.port_get_buffer &&
         a.port_name && a.port_short_name && a.port_flags && a.port_by_name && a.port_get_all_connections &&
         a.get_ports && a.connect && a.disconnect && a.free;
}

bool fakeEnabled() {
  const char* e = std::getenv("GRIDLINE_FAKE_JACK");
  return e && *e && *e != '0';
}

bool backendAvailable() {
#if defined(__linux__)
  return true;
#else
  return fakeEnabled();
#endif
}

#if defined(__linux__)
namespace {

bool fileExists(const std::string& p) {
  struct stat st {};
  return !p.empty() && ::stat(p.c_str(), &st) == 0;
}

bool pipewireSocketPresent() {
  const char* rt = std::getenv("XDG_RUNTIME_DIR");
  if (!rt || !*rt) return false;
  return fileExists(std::string(rt) + "/pipewire-0");
}

bool containsPipeWire(const std::string& s) {
  for (size_t i = 0; i + 8 <= s.size(); ++i) {
    char buf[9];
    for (size_t k = 0; k < 8; ++k) {
      const char c = s[i + k];
      buf[k] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    buf[8] = '\0';
    if (std::strcmp(buf, "pipewire") == 0) return true;
  }
  return false;
}

template <typename Fn>
bool sym(void* h, const char* name, Fn& out) {
  void* p = dlsym(h, name);
  if (!p) return false;
  static_assert(sizeof(out) == sizeof(p));
  std::memcpy(&out, &p, sizeof(out));
  return true;
}

template <typename Fn>
void optSym(void* h, const char* name, Fn& out) {
  void* p = dlsym(h, name);
  if (!p) {
    out = nullptr;
    return;
  }
  static_assert(sizeof(out) == sizeof(p));
  std::memcpy(&out, &p, sizeof(out));
}

bool resolveApi(void* handle, const std::string& path, Api& a, std::string* err) {
  a = Api{};
  bool ok = true;
  const char* missing = nullptr;
  auto req = [&](const char* name, auto& fn) {
    if (!sym(handle, name, fn)) {
      if (!missing) missing = name;
      ok = false;
    }
  };
  req("jack_client_open", a.client_open);
  req("jack_client_close", a.client_close);
  req("jack_get_client_name", a.get_client_name);
  req("jack_activate", a.activate);
  req("jack_deactivate", a.deactivate);
  req("jack_get_sample_rate", a.get_sample_rate);
  req("jack_get_buffer_size", a.get_buffer_size);
  req("jack_set_process_callback", a.set_process_callback);
  req("jack_set_buffer_size_callback", a.set_buffer_size_callback);
  req("jack_set_sample_rate_callback", a.set_sample_rate_callback);
  req("jack_set_xrun_callback", a.set_xrun_callback);
  req("jack_on_shutdown", a.on_shutdown);
  req("jack_port_register", a.port_register);
  req("jack_port_unregister", a.port_unregister);
  req("jack_port_get_buffer", a.port_get_buffer);
  req("jack_port_name", a.port_name);
  req("jack_port_short_name", a.port_short_name);
  req("jack_port_flags", a.port_flags);
  req("jack_port_by_name", a.port_by_name);
  req("jack_port_get_all_connections", a.port_get_all_connections);
  req("jack_get_ports", a.get_ports);
  req("jack_connect", a.connect);
  req("jack_disconnect", a.disconnect);
  req("jack_free", a.free);

  if (!ok) {
    if (err) *err = std::string("Incompatible JACK library (missing symbol ") + (missing ? missing : "?") + ").";
    return false;
  }

  optSym(handle, "jack_set_buffer_size", a.set_buffer_size);
  optSym(handle, "jack_set_freewheel_callback", a.set_freewheel_callback);
  optSym(handle, "jack_on_info_shutdown", a.on_info_shutdown);
  optSym(handle, "jack_set_latency_callback", a.set_latency_callback);
  optSym(handle, "jack_set_client_registration_callback", a.set_client_registration_callback);
  optSym(handle, "jack_set_port_registration_callback", a.set_port_registration_callback);
  optSym(handle, "jack_set_port_connect_callback", a.set_port_connect_callback);
  optSym(handle, "jack_set_graph_order_callback", a.set_graph_order_callback);
  optSym(handle, "jack_port_set_alias", a.port_set_alias);
  optSym(handle, "jack_port_unset_alias", a.port_unset_alias);
  optSym(handle, "jack_port_get_latency_range", a.port_get_latency_range);
  optSym(handle, "jack_port_set_latency_range", a.port_set_latency_range);
  optSym(handle, "jack_recompute_total_latencies", a.recompute_total_latencies);
  optSym(handle, "jack_port_uuid", a.port_uuid);
  optSym(handle, "jack_set_property", a.set_property);
  optSym(handle, "jack_remove_property", a.remove_property);
  optSym(handle, "jack_get_version_string", a.get_version_string);
  optSym(handle, "jack_frame_time", a.frame_time);

  a.libraryPath = path;
  a.viaPipeWire = containsPipeWire(path);
  if (!a.viaPipeWire && a.get_version_string) {
    if (const char* v = a.get_version_string(); v && containsPipeWire(v)) a.viaPipeWire = true;
  }
  return true;
}

bool probeServer(const Api& a) {
  int st = 0;
  Client* c = a.client_open("Gridline-probe", kNoStartServer, &st);
  if (!c) return false;
  a.client_close(c);
  return true;
}

std::vector<std::string> pipewireLibCandidates() {
  std::vector<std::string> out;
  const char* bases[] = {
      "/usr/lib/x86_64-linux-gnu/pipewire-0.3/jack/libjack.so.0",
      "/usr/lib/aarch64-linux-gnu/pipewire-0.3/jack/libjack.so.0",
      "/usr/lib64/pipewire-0.3/jack/libjack.so.0",
      "/usr/lib/pipewire-0.3/jack/libjack.so.0",
      "/usr/local/lib/pipewire-0.3/jack/libjack.so.0",
  };
  if (const char* snap = std::getenv("SNAP"); snap && *snap) {
    for (const char* b : bases) out.push_back(std::string(snap) + b);
  }
  for (const char* b : bases) out.emplace_back(b);
  return out;
}

struct Loaded {
  std::string key;
  void* handle = nullptr;
  Api api;
};

}  // namespace

const Api* loadApi(const std::string& explicitPath, std::string* err) {
  static std::mutex mu;
  static std::vector<Loaded> cache;
  std::lock_guard<std::mutex> lock(mu);

  auto findCached = [&](const std::string& key) -> const Api* {
    for (const auto& l : cache)
      if (l.key == key) return &l.api;
    return nullptr;
  };
  auto storeCached = [&](const std::string& key, void* h, Api&& a) -> const Api* {
    cache.push_back({key, h, std::move(a)});
    return &cache.back().api;
  };

  if (!explicitPath.empty()) {
    if (const Api* c = findCached(explicitPath)) return c;
    void* h = dlopen(explicitPath.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
      const char* de = dlerror();
      if (err) *err = "Could not load JACK library \"" + explicitPath + "\": " + (de ? de : "file not found");
      return nullptr;
    }
    Api a;
    if (!resolveApi(h, explicitPath, a, err)) {
      dlclose(h);
      return nullptr;
    }
    return storeCached(explicitPath, h, std::move(a));
  }

  // 1. Prefer an already-loaded libjack.so.0 (avoids two conflicting copies if RtMidi loaded it).
  if (void* existing = dlopen("libjack.so.0", RTLD_LAZY | RTLD_NOLOAD)) {
    Api a;
    if (resolveApi(existing, "libjack.so.0", a, nullptr) && (a.viaPipeWire || !pipewireSocketPresent() || probeServer(a))) {
      if (const Api* c = findCached(a.libraryPath)) return c;
      return storeCached(a.libraryPath, existing, std::move(a));
    }
  }

  // 2. Try default libjack.so.0; if PipeWire socket is present and default doesn't answer, prefer PipeWire's copy.
  void* defH = dlopen("libjack.so.0", RTLD_LAZY | RTLD_LOCAL);
  Api defApi;
  bool defOk = defH && resolveApi(defH, "libjack.so.0", defApi, nullptr);
  if (defOk && (defApi.viaPipeWire || !pipewireSocketPresent() || probeServer(defApi))) {
    if (const Api* c = findCached("libjack.so.0")) return c;
    return storeCached("libjack.so.0", defH, std::move(defApi));
  }

  // 3. Try PipeWire's pipewire-0.3/jack/libjack.so.0 directories (Debian/Ubuntu without pw-jack, Snap).
  for (const std::string& cand : pipewireLibCandidates()) {
    if (!fileExists(cand)) continue;
    if (const Api* c = findCached(cand)) return c;
    void* pwH = dlopen(cand.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!pwH) continue;
    Api pwApi;
    if (resolveApi(pwH, cand, pwApi, nullptr)) {
      pwApi.viaPipeWire = true;
      return storeCached(cand, pwH, std::move(pwApi));
    }
    dlclose(pwH);
  }

  // 4. Fall back to the default libjack.so.0 even if no server is running yet, so Host::open reports "no server".
  if (defOk) {
    if (const Api* c = findCached("libjack.so.0")) return c;
    return storeCached("libjack.so.0", defH, std::move(defApi));
  }
  if (defH) dlclose(defH);

  if (err) *err = "JACK / PipeWire is not installed: install your system's PipeWire JACK package or JACK.";
  return nullptr;
}

#else

const Api* loadApi(const std::string& explicitPath, std::string* err) {
  if (err) {
    if (!explicitPath.empty())
      *err = "Could not load JACK library \"" + explicitPath + "\": JACK / PipeWire is only available on Linux.";
    else
      *err = "JACK / PipeWire is only available on Linux.";
  }
  return nullptr;
}

#endif

}  // namespace gl::jack

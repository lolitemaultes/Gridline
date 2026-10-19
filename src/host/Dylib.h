// Gridline — loading plugin binaries: macOS bundles (CFBundle) and plain
// shared objects (dlopen). Loaded binaries are never unloaded (plugin code
// may still run from other threads or registered callbacks).
#pragma once
#include <cstring>
#include <memory>
#include <string>

namespace gl::host {

class Dylib {
 public:
  // Opens a bundle directory or a shared object file; cached per path.
  static std::shared_ptr<Dylib> open(const std::string& path, std::string* err);
  const std::string& path() const { return path_; }
  void* function(const char* name) const;  // exported function, nullptr if absent
  void* data(const char* name) const;      // exported data symbol, nullptr if absent

 private:
  std::string path_;
  void* handle_ = nullptr;
  bool bundle_ = false;
};

// Function pointer from a symbol address without a pointer-to-object cast.
template <class Fn>
Fn toFunction(void* sym) {
  Fn f = nullptr;
  static_assert(sizeof(Fn) == sizeof(void*), "function pointer size");
  std::memcpy(&f, &sym, sizeof f);
  return f;
}

}  // namespace gl::host

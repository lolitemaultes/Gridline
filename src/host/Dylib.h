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
#if defined(__APPLE__)
  bool bundle_ = false;  // a CFBundle (macOS)
#endif
};

// Plain shared objects (dlopen / LoadLibrary), for code that manages the handle itself.
namespace dl {
// Opens a shared object (Windows: a DLL, its folder searched for its dependencies).
// `now`: resolve every symbol at load (RTLD_NOW). nullptr on failure, with the reason in *err.
void* open(const std::string& path, std::string* err = nullptr, bool now = false);
// The handle of a shared object this process has already loaded, nullptr otherwise.
void* loaded(const std::string& path);
void* sym(void* handle, const char* name);
void close(void* handle);
// The file of the shared object (or executable) that contains `address`, empty when unknown.
std::string pathOf(const void* address);
}  // namespace dl

// Function pointer from a symbol address without a pointer-to-object cast.
template <class Fn>
Fn toFunction(void* sym) {
  Fn f = nullptr;
  static_assert(sizeof(Fn) == sizeof(void*), "function pointer size");
  std::memcpy(&f, &sym, sizeof f);
  return f;
}

}  // namespace gl::host

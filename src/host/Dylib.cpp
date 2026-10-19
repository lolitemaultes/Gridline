#include "Dylib.h"
#include <filesystem>
#include <map>
#include <mutex>
#include <dlfcn.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace fs = std::filesystem;

namespace gl::host {

std::shared_ptr<Dylib> Dylib::open(const std::string& path, std::string* err) {
  static std::mutex lock;
  static auto* cache = new std::map<std::string, std::shared_ptr<Dylib>>;  // never destroyed
  std::lock_guard<std::mutex> g(lock);
  if (auto it = cache->find(path); it != cache->end()) return it->second;
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return std::shared_ptr<Dylib>();
  };
  auto d = std::make_shared<Dylib>();
  d->path_ = path;
  std::error_code ec;
#if defined(__APPLE__)
  if (fs::is_directory(path, ec)) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()), true);
    if (!url) return fail("bad path");
    CFBundleRef bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!bundle) return fail("not a bundle");
    CFErrorRef cferr = nullptr;
    if (!CFBundleLoadExecutableAndReturnError(bundle, &cferr)) {
      std::string why = "cannot load the bundle executable";
      if (cferr) {
        if (CFStringRef desc = CFErrorCopyDescription(cferr)) {
          char buf[512];
          if (CFStringGetCString(desc, buf, sizeof buf, kCFStringEncodingUTF8)) why = buf;
          CFRelease(desc);
        }
        CFRelease(cferr);
      }
      CFRelease(bundle);
      return fail(why);
    }
    d->handle_ = bundle;
    d->bundle_ = true;
    (*cache)[path] = d;
    return d;
  }
#endif
  if (!fs::exists(path, ec)) return fail("file not found");
  void* h = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!h) return fail(std::string("dlopen: ") + dlerror());
  d->handle_ = h;
  (*cache)[path] = d;
  return d;
}

void* Dylib::function(const char* name) const {
#if defined(__APPLE__)
  if (bundle_) {
    CFStringRef s = CFStringCreateWithCString(nullptr, name, kCFStringEncodingUTF8);
    void* p = CFBundleGetFunctionPointerForName(static_cast<CFBundleRef>(handle_), s);
    CFRelease(s);
    return p;
  }
#endif
  return handle_ ? dlsym(handle_, name) : nullptr;
}

void* Dylib::data(const char* name) const {
#if defined(__APPLE__)
  if (bundle_) {
    CFStringRef s = CFStringCreateWithCString(nullptr, name, kCFStringEncodingUTF8);
    void* p = CFBundleGetDataPointerForName(static_cast<CFBundleRef>(handle_), s);
    CFRelease(s);
    return p;
  }
#endif
  return handle_ ? dlsym(handle_, name) : nullptr;
}

}  // namespace gl::host

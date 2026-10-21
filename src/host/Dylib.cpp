#include "Dylib.h"
#include <filesystem>
#include <map>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
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
  std::string why;
  void* h = dl::open(path, &why);
  if (!h) return fail(why);
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
  return handle_ ? dl::sym(handle_, name) : nullptr;
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
  return handle_ ? dl::sym(handle_, name) : nullptr;
}

namespace dl {

#if defined(_WIN32)
namespace {
std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  for (auto& c : w)
    if (c == L'/') c = L'\\';
  return w;
}
std::string lastError() {
  const DWORD code = GetLastError();
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                 reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
  std::string out = "error " + std::to_string(code);
  if (msg) {
    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, msg, -1, s.data(), n, nullptr, nullptr);
    LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    if (!s.empty()) out = s + " (" + out + ")";
  }
  return out;
}
}  // namespace

void* open(const std::string& path, std::string* err, bool) {
  // The DLL's own folder is searched for the DLLs it depends on (plugins ship them beside it).
  HMODULE h = LoadLibraryExW(widen(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h && err) *err = "LoadLibrary: " + lastError();
  return reinterpret_cast<void*>(h);
}
void* loaded(const std::string& path) {
  HMODULE h = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, widen(path).c_str(), &h)) return nullptr;
  return reinterpret_cast<void*>(h);
}
void* sym(void* handle, const char* name) {
  if (!handle) return nullptr;
  FARPROC p = GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
  void* v = nullptr;
  static_assert(sizeof(p) == sizeof(v), "function pointer size");
  std::memcpy(&v, &p, sizeof v);
  return v;
}
void close(void* handle) {
  if (handle) FreeLibrary(reinterpret_cast<HMODULE>(handle));
}
std::string pathOf(const void* address) {
  HMODULE h = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(address), &h))
    return {};
  std::wstring buf(32768, L'\0');
  DWORD n = GetModuleFileNameW(h, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0) return {};
  buf.resize(n);
  int m = WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(m > 0 ? m - 1 : 0), '\0');
  if (m > 1) WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, s.data(), m, nullptr, nullptr);
  return s;
}
#else
void* open(const std::string& path, std::string* err, bool now) {
  void* h = dlopen(path.c_str(), (now ? RTLD_NOW : RTLD_LAZY) | RTLD_LOCAL);
  if (!h && err) {
    const char* e = dlerror();
    *err = std::string("dlopen: ") + (e ? e : "failed");
  }
  return h;
}
void* loaded(const std::string& path) { return dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD); }
void* sym(void* handle, const char* name) { return handle ? dlsym(handle, name) : nullptr; }
void close(void* handle) {
  if (handle) dlclose(handle);
}
std::string pathOf(const void* address) {
  Dl_info info{};
  if (!dladdr(const_cast<void*>(address), &info) || !info.dli_fname) return {};
  return info.dli_fname;
}
#endif

}  // namespace dl

}  // namespace gl::host

#include "Vst3Module.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <dlfcn.h>
#include <sys/utsname.h>
#endif

namespace fs = std::filesystem;

namespace gl::vst3 {

namespace {
std::string fixed(const char* s, size_t cap) { return std::string(s, strnlen(s, cap)); }

// Function pointer from a symbol address without a pointer-to-object cast.
template <class Fn>
Fn toFunction(void* sym) {
  Fn f = nullptr;
  static_assert(sizeof(Fn) == sizeof(void*), "function pointer size");
  std::memcpy(&f, &sym, sizeof f);
  return f;
}
}  // namespace

bool ClassInfo::isInstrument() const {
  // "Instrument|Synth" or "Instrument" → instrument; "Fx|Instrument" is an effect
  // that can also be loaded as an instrument: treat as an effect.
  std::string first = subCategories.substr(0, subCategories.find('|'));
  return first == "Instrument";
}

std::string bundleBinaryPath(const std::string& bundlePath) {
  fs::path b(bundlePath);
#if defined(__APPLE__)
  return (b / "Contents" / "MacOS" / b.stem()).string();
#else
  struct utsname u {};
  std::string arch = uname(&u) == 0 ? u.machine : "x86_64";
  if (arch == "i686" || arch == "i586" || arch == "i486") arch = "i386";
  if (arch == "arm64") arch = "aarch64";
  return (b / "Contents" / (arch + "-linux") / (b.stem().string() + ".so")).string();
#endif
}

std::vector<std::string> defaultSearchPaths() {
  std::vector<std::string> v;
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home) v.push_back(std::string(home) + "/Library/Audio/Plug-Ins/VST3");
  v.push_back("/Library/Audio/Plug-Ins/VST3");
#else
  if (home) v.push_back(std::string(home) + "/.vst3");
  v.push_back("/usr/lib/vst3");
  v.push_back("/usr/local/lib/vst3");
#endif
  return v;
}

std::vector<std::string> findBundles(const std::vector<std::string>& folders) {
  std::vector<std::string> out;
  for (const auto& f : folders) {
    std::error_code ec;
    if (!fs::is_directory(f, ec)) continue;
    fs::recursive_directory_iterator it(f, fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const fs::path& p = it->path();
      if (p.extension() == ".vst3") {
        if (it->is_directory(ec)) {
          out.push_back(p.string());
          it.disable_recursion_pending();
        }
      }
      if (it.depth() > 6) it.disable_recursion_pending();
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Module::~Module() {
  factory_.reset();
  if (!handle_) return;
#if defined(__APPLE__)
  auto bundle = static_cast<CFBundleRef>(handle_);
  if (!leak_) {
    void* exitSym = CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit"));
    if (!exitSym) exitSym = CFBundleGetFunctionPointerForName(bundle, CFSTR("BundleExit"));
    if (exitSym) toFunction<bool (*)()>(exitSym)();
  }
  CFRelease(bundle);
#else
  if (!leak_) {
    if (void* exitSym = dlsym(handle_, "ModuleExit")) toFunction<bool (*)()>(exitSym)();
    dlclose(handle_);
  }
#endif
}

std::unique_ptr<Module> Module::load(const std::string& bundlePath, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return std::unique_ptr<Module>();
  };
  std::unique_ptr<Module> m(new Module);
  m->path_ = bundlePath;
  void* factorySym = nullptr;
#if defined(__APPLE__)
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(bundlePath.c_str()),
                                                         static_cast<CFIndex>(bundlePath.size()), true);
  if (!url) return fail("bad path");
  CFBundleRef bundle = CFBundleCreate(nullptr, url);
  CFRelease(url);
  if (!bundle) return fail("not a bundle");
  CFErrorRef cferr = nullptr;
  if (!CFBundleLoadExecutableAndReturnError(bundle, &cferr)) {
    std::string why = "cannot load the bundle executable";
    if (cferr) {
      if (CFStringRef d = CFErrorCopyDescription(cferr)) {
        char buf[512];
        if (CFStringGetCString(d, buf, sizeof buf, kCFStringEncodingUTF8)) why = buf;
        CFRelease(d);
      }
      CFRelease(cferr);
    }
    CFRelease(bundle);
    return fail(why);
  }
  m->handle_ = bundle;
  void* entry = CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry"));
  if (!entry) entry = CFBundleGetFunctionPointerForName(bundle, CFSTR("BundleEntry"));
  if (entry && !toFunction<bool (*)(CFBundleRef)>(entry)(bundle)) {
    m->leak_ = true;  // entry failed: do not call exit
    return fail("bundleEntry failed");
  }
  factorySym = CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory"));
#else
  std::string bin = bundleBinaryPath(bundlePath);
  std::error_code ec;
  if (!fs::exists(bin, ec)) {
    if (fs::is_regular_file(bundlePath, ec))
      bin = bundlePath;  // single-file plugin
    else
      return fail("no binary for this platform (" + bin + ")");
  }
  void* h = dlopen(bin.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!h) return fail(std::string("dlopen: ") + dlerror());
  m->handle_ = h;
  if (void* entry = dlsym(h, "ModuleEntry")) {
    if (!toFunction<bool (*)(void*)>(entry)(h)) {
      m->leak_ = true;
      return fail("ModuleEntry failed");
    }
  }
  factorySym = dlsym(h, "GetPluginFactory");
#endif
  if (!factorySym) return fail("no GetPluginFactory export");
  sb::IPluginFactory* f = toFunction<sb::IPluginFactory* (*)()>(factorySym)();
  if (!f) return fail("GetPluginFactory returned null");
  m->factory_ = Ptr<sb::IPluginFactory>(f);
  sb::PFactoryInfo fi{};
  if (f->getFactoryInfo(&fi) == sb::kResultOk) m->vendor_ = fixed(fi.vendor, sizeof fi.vendor);
  m->readClasses();
  return m;
}

void Module::readClasses() {
  classes_.clear();
  auto f2 = Ptr<sb::IPluginFactory2>::query<sb::IPluginFactory2>(factory_.get());
  auto f3 = Ptr<sb::IPluginFactory3>::query<sb::IPluginFactory3>(factory_.get());
  const sb::int32 n = factory_->countClasses();
  for (sb::int32 i = 0; i < n; ++i) {
    ClassInfo c;
    sb::PClassInfo2 i2{};
    sb::PClassInfoW iw{};
    if (f3 && f3->getClassInfoUnicode(i, &iw) == sb::kResultOk) {
      c.uid = uidToHex(iw.cid);
      c.name = toUtf8(iw.name, sizeof iw.name / sizeof iw.name[0]);
      c.category = fixed(iw.category, sizeof iw.category);
      c.subCategories = fixed(iw.subCategories, sizeof iw.subCategories);
      c.vendor = toUtf8(iw.vendor, sizeof iw.vendor / sizeof iw.vendor[0]);
      c.version = toUtf8(iw.version, sizeof iw.version / sizeof iw.version[0]);
      c.sdkVersion = toUtf8(iw.sdkVersion, sizeof iw.sdkVersion / sizeof iw.sdkVersion[0]);
      c.flags = iw.classFlags;
    } else if (f2 && f2->getClassInfo2(i, &i2) == sb::kResultOk) {
      c.uid = uidToHex(i2.cid);
      c.name = fixed(i2.name, sizeof i2.name);
      c.category = fixed(i2.category, sizeof i2.category);
      c.subCategories = fixed(i2.subCategories, sizeof i2.subCategories);
      c.vendor = fixed(i2.vendor, sizeof i2.vendor);
      c.version = fixed(i2.version, sizeof i2.version);
      c.sdkVersion = fixed(i2.sdkVersion, sizeof i2.sdkVersion);
      c.flags = i2.classFlags;
    } else {
      sb::PClassInfo i1{};
      if (factory_->getClassInfo(i, &i1) != sb::kResultOk) continue;
      c.uid = uidToHex(i1.cid);
      c.name = fixed(i1.name, sizeof i1.name);
      c.category = fixed(i1.category, sizeof i1.category);
    }
    if (c.vendor.empty()) c.vendor = vendor_;
    classes_.push_back(std::move(c));
  }
}

const ClassInfo* Module::findClass(const std::string& uid) const {
  for (const auto& c : classes_)
    if (c.uid == uid) return &c;
  return nullptr;
}

std::shared_ptr<Module> Module::shared(const std::string& bundlePath, std::string* err) {
  static std::mutex lock;
  static auto* cache = new std::map<std::string, std::shared_ptr<Module>>;  // never destroyed (see header)
  std::lock_guard<std::mutex> g(lock);
  auto it = cache->find(bundlePath);
  if (it != cache->end()) return it->second;
  std::shared_ptr<Module> m(load(bundlePath, err).release());
  if (m) (*cache)[bundlePath] = m;
  return m;
}

}  // namespace gl::vst3

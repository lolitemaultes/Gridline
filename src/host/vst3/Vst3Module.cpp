#include "Vst3Module.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include "../Dylib.h"
#include "../PluginScan.h"
#include "../../core/Platform.h"
#include "Vst3HostObjects.h"
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#if !defined(_WIN32)
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

// The bundle's architecture folder: <arch>-win (Windows), <arch>-linux (Linux and the BSDs,
// which use the Linux layout).
std::string bundleArchFolder() {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64-win";
#elif defined(_M_IX86) || defined(__i386__)
  return "x86-win";
#else
  return "x86_64-win";
#endif
#else
  struct utsname u {};
  std::string arch = uname(&u) == 0 ? u.machine : "x86_64";
  if (arch == "i686" || arch == "i586" || arch == "i486") arch = "i386";
  if (arch == "arm64") arch = "aarch64";
  if (arch == "amd64") arch = "x86_64";  // FreeBSD / OpenBSD / NetBSD name
  return arch + "-linux";
#endif
}

std::string findBundleBinary(const std::string& bundlePath, const std::vector<std::string>& archFolders, const std::string& ext) {
  const fs::path b = fs::u8path(bundlePath);
  std::error_code ec;
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  for (const std::string& arch : archFolders) {
    const fs::path dir = b / "Contents" / fs::u8path(arch);
    // The SDK's layout: the binary has the bundle's name (Windows: with .vst3; Linux: .so).
    const fs::path named = dir / fs::u8path(ext == ".vst3" ? b.filename().u8string() : b.stem().u8string() + ext);
    if (fs::is_regular_file(named, ec)) return named.u8string();
    // K-267: a bundle whose binary has another name (renamed bundle folder, a vendor's own naming):
    // the one file of the platform's kind in the folder, the first by name when there are more.
    std::vector<std::string> found;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (it->is_regular_file(e2) && lower(it->path().extension().u8string()) == ext) found.push_back(it->path().u8string());
    }
    ec.clear();
    if (!found.empty()) return *std::min_element(found.begin(), found.end());
  }
  return {};
}

std::vector<std::string> bundleArchFolders() {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
  return {"arm64-win", "arm64x-win", "arm64ec-win"};  // Windows on ARM: arm64x / arm64ec binaries run too
#else
  return {bundleArchFolder()};
#endif
#else
  return {bundleArchFolder()};
#endif
}

std::string bundleBinaryPath(const std::string& bundlePath) {
  const fs::path b = fs::u8path(bundlePath);
#if defined(__APPLE__)
  return (b / "Contents" / "MacOS" / b.stem()).u8string();
#elif defined(_WIN32)
  if (std::string found = findBundleBinary(bundlePath, bundleArchFolders(), ".vst3"); !found.empty()) return found;
  return (b / "Contents" / fs::u8path(bundleArchFolder()) / b.filename()).u8string();  // (missing: named in the error)
#else
  if (std::string found = findBundleBinary(bundlePath, bundleArchFolders(), ".so"); !found.empty()) return found;
  return (b / "Contents" / fs::u8path(bundleArchFolder()) / fs::u8path(b.stem().u8string() + ".so")).u8string();
#endif
}

std::vector<std::string> defaultSearchPaths() {
  std::vector<std::string> v;
  const char* home = platform::homeEnv();
#if defined(__APPLE__)
  if (home) v.push_back(std::string(home) + "/Library/Audio/Plug-Ins/VST3");
  v.push_back("/Library/Audio/Plug-Ins/VST3");
#elif defined(_WIN32)
  // %LOCALAPPDATA%\Programs\Common\VST3 (per user) and %COMMONPROGRAMFILES%\VST3.
  (void)home;
  // K-267: read as UTF-8 (a user name with letters outside the system's code page).
  if (const std::string la = platform::getEnv("LOCALAPPDATA"); !la.empty()) v.push_back(la + "\\Programs\\Common\\VST3");
  if (const std::string cf = platform::getEnv("CommonProgramFiles"); !cf.empty())
    v.push_back(cf + "\\VST3");
  else
    v.push_back("C:\\Program Files\\Common Files\\VST3");
#else
  if (home) v.push_back(std::string(home) + "/.vst3");
  v.push_back("/usr/lib/vst3");
  v.push_back("/usr/local/lib/vst3");
#if !defined(__linux__)
  v.push_back("/usr/pkg/lib/vst3");  // NetBSD pkgsrc
#endif
#endif
  return v;
}

std::vector<std::string> findBundles(const std::vector<std::string>& folders) {
  // K-73: the shared search (each real folder once, links followed, stops on quit, shows where it is).
  // Bundles are folders; on Windows a VST 3.6 or older plugin can be a single .vst3 DLL too.
#if defined(_WIN32)
  return packagesOrInside(host::findEntries(folders, ".vst3", true, true), bundleArchFolders());
#else
  return host::findEntries(folders, ".vst3", true, false);
#endif
}

std::vector<std::string> packagesOrInside(const std::vector<std::string>& found, const std::vector<std::string>& archFolders) {
  // K-267: as the VST3 SDK's own search on Windows: a folder named *.vst3 that holds no binary for
  // this computer is not a bundle but a folder of plugins (an installer's vendor folder): its
  // plugins are listed instead.
  std::vector<std::string> out;
  for (const std::string& e : found) {
    std::error_code ec;
    const fs::path p = fs::u8path(e);
    if (!fs::is_directory(p, ec) || fs::exists(p / "Contents", ec) || !findBundleBinary(e, archFolders, ".vst3").empty()) {
      out.push_back(e);
      continue;
    }
    std::vector<std::string> inner = host::findEntries({e}, ".vst3", true, true);
    if (inner.empty()) out.push_back(e);  // nothing inside either: listed (and fails with the reason)
    for (auto& i : inner) out.push_back(std::move(i));
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
#if defined(_WIN32)
    if (void* exitSym = host::dl::sym(handle_, "ExitDll")) toFunction<bool (*)()>(exitSym)();
#else
    if (void* exitSym = host::dl::sym(handle_, "ModuleExit")) toFunction<bool (*)()>(exitSym)();
#endif
    host::dl::close(handle_);
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
  if (!fs::exists(fs::u8path(bin), ec)) {
    if (fs::is_regular_file(fs::u8path(bundlePath), ec))
      bin = bundlePath;  // single-file plugin
    else
      return fail("no binary for this platform (" + bin + ")");
  }
  std::string why;
  void* h = host::dl::open(bin, &why);
  if (!h) return fail(why);
  m->handle_ = h;
#if defined(_WIN32)
  if (void* entry = host::dl::sym(h, "InitDll")) {
    if (!toFunction<bool (*)()>(entry)()) {
      m->leak_ = true;
      return fail("InitDll failed");
    }
  }
#else
  if (void* entry = host::dl::sym(h, "ModuleEntry")) {
    if (!toFunction<bool (*)(void*)>(entry)(h)) {
      m->leak_ = true;
      return fail("ModuleEntry failed");
    }
  }
#endif
  factorySym = host::dl::sym(h, "GetPluginFactory");
#endif
  if (!factorySym) return fail("no GetPluginFactory export");
  sb::IPluginFactory* f = toFunction<sb::IPluginFactory* (*)()>(factorySym)();
  if (!f) return fail("GetPluginFactory returned null");
  m->factory_ = Ptr<sb::IPluginFactory>(f);
  // K-267: the host context, as hosts give it to a factory of version 3 before anything else of it
  // (plugins that need it to create their classes fail "cannot create the component" without it).
  if (auto f3 = Ptr<sb::IPluginFactory3>::query<sb::IPluginFactory3>(f)) f3->setHostContext(static_cast<sv::IHostApplication*>(hostContext()));
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
    if (c.name.empty()) {  // K-267: a factory whose wide-text info leaves the name out
      sb::PClassInfo i1{};
      if (factory_->getClassInfo(i, &i1) == sb::kResultOk) c.name = fixed(i1.name, sizeof i1.name);
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

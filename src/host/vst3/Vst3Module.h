// Gridline — VST3 hosting: loading plugin bundles and reading their factories.
//   macOS  <name>.vst3/Contents/MacOS/<exe>               (CFBundle, bundleEntry)
//   Linux  <name>.vst3/Contents/<arch>-linux/<name>.so    (dlopen, ModuleEntry)
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Vst3Common.h"

namespace gl::vst3 {

// One class exported by a plugin factory.
struct ClassInfo {
  std::string uid;            // 32 hex digits
  std::string name;
  std::string category;       // "Audio Module Class", "Component Controller Class", …
  std::string subCategories;  // e.g. "Instrument|Synth", "Fx|Delay" (factory 2+)
  std::string vendor, version, sdkVersion;
  uint32_t flags = 0;
  bool isAudioModule() const { return category == "Audio Module Class"; }
  // Instruments: sub-categories name "Instrument" and not "Fx" first.
  bool isInstrument() const;
};

class Module {
 public:
  ~Module();
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  // Loads a bundle (not cached). Returns nullptr and sets `err` on failure.
  static std::unique_ptr<Module> load(const std::string& bundlePath, std::string* err);
  // Process-wide cache: a bundle is loaded once and stays loaded (unloading
  // plugin code while any of its objects might still run is never safe).
  static std::shared_ptr<Module> shared(const std::string& bundlePath, std::string* err);

  const std::string& path() const { return path_; }
  sb::IPluginFactory* factory() const { return factory_.get(); }
  const std::string& factoryVendor() const { return vendor_; }
  const std::vector<ClassInfo>& classes() const { return classes_; }
  const ClassInfo* findClass(const std::string& uid) const;
  // Creates an instance of class `uid` with interface I; empty on failure.
  template <class I>
  Ptr<I> create(const std::string& uid) const {
    sb::TUID cid;
    void* obj = nullptr;
    if (!factory_ || !hexToUid(uid, cid)) return Ptr<I>();
    if (factory_->createInstance(cid, I::iid.toTUID(), &obj) != sb::kResultOk || !obj) return Ptr<I>();
    return Ptr<I>(static_cast<I*>(obj));
  }
  // Skip the module exit call in the destructor (scanner: plugins that crash
  // on exit must not turn a good scan into a failure).
  void leakOnDestroy() { leak_ = true; }

 private:
  Module() = default;
  void readClasses();
  std::string path_, vendor_;
  void* handle_ = nullptr;  // CFBundleRef (macOS) / dlopen handle (Linux)
  Ptr<sb::IPluginFactory> factory_;
  std::vector<ClassInfo> classes_;
  bool leak_ = false;
};

// Platform binary inside a bundle (Linux: Contents/<arch>-linux/<name>.so).
std::string bundleBinaryPath(const std::string& bundlePath);
// Default VST3 search folders for this platform (existing or not).
std::vector<std::string> defaultSearchPaths();
// Every *.vst3 bundle below the folders (bundles are not searched inside).
std::vector<std::string> findBundles(const std::vector<std::string>& folders);

}  // namespace gl::vst3

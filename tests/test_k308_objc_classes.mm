// Gridline - K-308: Objective-C class names of the plugin bundles (macOS).
//
// Single-plugin downloads carry the same engine binary (Plasma.vst3, Loupe.vst3, ...). Classes
// compiled into it would clash in a host that loads two of them ("Class X is implemented in
// both"), so the editors' classes are made at run time under a name of their own per loaded image
// (host/ui/ObjcClass.h), and the one class a host finds by name, the Audio Unit's view factory,
// gets a per-bundle name from the packaging step (tools/release/package-plugins.py).
//
// Checks: the run-time class helper (names, instance variables, super calls); then two real
// single-plugin downloads made from this build's suite bundles, loaded in this process: no class
// name in common, nothing logged about duplicate classes, and each Audio Unit's view factory found
// in its own bundle under its own name.
#import <AudioToolbox/AUCocoaUIView.h>
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/PluginInfo.h"
#include "core/PluginSuite.h"
#include "host/ui/ObjcClass.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

std::string quote(const std::string& s) {
  std::string r = "'";
  for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return r + "'";
}

void testRuntimeClass() {
  const std::string n1 = objc::uniqueName("GridlineProbe"), n2 = objc::uniqueName("GridlineProbe");
  expect(n1 == n2 && n1.rfind("GridlineProbe_", 0) == 0 && n1.size() > 15, "a run-time class name is stable in one image: " + n1);
  static objc::RuntimeClass rc("GridlineProbe", [NSObject class]);
  static Class self_ = Nil;
  self_ = rc.pointer("owner")
              .method(@selector(description), +[](id self, SEL sel) -> NSString* {
                NSString* base = objc::callSuper<NSString*>(self, self_, sel);
                return [@"probe:" stringByAppendingString:base];
              })
              .method(@selector(isFlipped), +[](id, SEL) -> BOOL { return YES; })
              .done();
  expect(self_ != Nil && std::string(class_getName(self_)) == n1, "the class registers under its name");
  expect(objc_getClass(n1.c_str()) == self_, "the runtime finds it by name");
  id o = [[self_ alloc] init];
  int marker = 7;
  expect(objc::getPointer<int>(o, "owner") == nullptr, "a new object's pointer is empty");
  objc::setPointer(o, "owner", &marker);
  expect(objc::getPointer<int>(o, "owner") == &marker, "the pointer reads back");
  expect(objc::getPointer<int>(o, "nothing") == nullptr, "an unknown variable reads as empty");
  NSString* d = [o description];
  expect(d && [d hasPrefix:@"probe:<GridlineProbe_"], std::string("super is called from a method: ") + (d ? [d UTF8String] : "nil"));
  expect(((BOOL(*)(id, SEL))objc_msgSend)(o, @selector(isFlipped)) == YES, "a BOOL method answers");
  [o release];
  objc::RuntimeClass again("GridlineProbe", [NSObject class]);
  expect(again.done() == Nil, "a second class of the same name in one image is refused, not a clash");
}

// The image's own compiled class names (path as loaded).
std::set<std::string> classesOf(const std::string& binary) {
  std::set<std::string> out;
  unsigned n = 0;
  const char** names = objc_copyClassNamesForImage(binary.c_str(), &n);
  for (unsigned i = 0; i < n; ++i) out.insert(names[i]);
  std::free(names);
  return out;
}

// stderr into a file while `fn` runs; returns what was written.
template <typename F>
std::string captureStderr(const std::string& file, F fn) {
  std::fflush(stderr);
  const int saved = dup(2);
  const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  dup2(fd, 2);
  close(fd);
  fn();
  std::fflush(stderr);
  dup2(saved, 2);
  close(saved);
  std::ifstream in(file);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void testSingleDownloads(const std::string& tmp) {
  const char* kPicks[] = {"gl.plasma", "gl.edison"};
  // The catalogue the release writes with `gridline suite-info gridline --json` (the fields the packager reads).
  const std::string cat = tmp + "/catalogue.json";
  {
    std::ofstream f(cat);
    f << "[";
    bool first = true;
    for (const suite::Member& m : suite::members("gridline")) {
      f << (first ? "" : ",") << "{\"type\":\"" << m.type << "\",\"name\":\"" << m.name << "\",\"kind\":\""
        << (m.instrument ? "instrument" : "effect") << "\",\"category\":\"" << (m.info ? m.info->category : "") << "\",\"au\":\""
        << suite::fourcc(m.auType) << "/" << suite::fourcc(m.auSubtype) << "/GrLn\"}";
      first = false;
    }
    f << "]";
  }
  const std::string out = tmp + "/out";
  std::string cmd = quote(GRIDLINE_PYTHON) + " -I " + quote(GRIDLINE_PACKAGE_PLUGINS) + " --suite gridline --family macos --version 0.0.0 --catalogue " +
                    quote(cat) + " --input " + quote(GRIDLINE_PLUGIN_DIR) + " --out " + quote(out) + " --sign --singles-only";
  for (const char* p : kPicks) cmd += std::string(" --only ") + p;
  cmd += " > " + quote(tmp + "/package.log") + " 2>&1";
  expect(std::system(cmd.c_str()) == 0, "the packager makes two single-plugin downloads from this build");
  std::vector<std::string> names = {"Plasma", "Loupe"};
  for (const std::string& n : names) {
    const std::string zip = out + "/Gridline-Suite-" + n + "-0.0.0-macos.zip";
    const std::string unz = "ditto -x -k " + quote(zip) + " " + quote(tmp + "/x");
    expect(fs::exists(zip) && std::system(unz.c_str()) == 0, "unpacked " + n);
  }
  auto vst3 = [&](const std::string& n) { return tmp + "/x/" + n + "/VST3/Gridline Suite/" + n + ".vst3/Contents/MacOS/Gridline Suite"; };
  auto au = [&](const std::string& n) { return tmp + "/x/" + n + "/Components/" + n + ".component"; };
  std::vector<std::string> binaries = {vst3("Plasma"), vst3("Loupe"), au("Plasma") + "/Contents/MacOS/Gridline Suite",
                                       au("Loupe") + "/Contents/MacOS/Gridline Suite"};
  for (const std::string& b : binaries) expect(fs::exists(b), "binary there: " + b);

  // Load all four (two VST3, two Audio Units) the way a host does; nothing may be logged about duplicates.
  std::vector<void*> handles;
  NSBundle* plasmaAu = [NSBundle bundleWithPath:[NSString stringWithUTF8String:au("Plasma").c_str()]];
  NSBundle* loupeAu = [NSBundle bundleWithPath:[NSString stringWithUTF8String:au("Loupe").c_str()]];
  const std::string log = captureStderr(tmp + "/stderr.txt", [&] {
    for (int i = 0; i < 2; ++i) handles.push_back(dlopen(binaries[static_cast<size_t>(i)].c_str(), RTLD_NOW | RTLD_LOCAL));
    [plasmaAu load];
    [loupeAu load];
  });
  expect(handles[0] && handles[1] && [plasmaAu isLoaded] && [loupeAu isLoaded], "the four single-plugin binaries load in one process");
  expect(log.find("implemented in both") == std::string::npos, "no duplicate class is logged: " + log.substr(0, 300));

  // Compiled classes per image: none in common, the only one left is the AU view factory.
  std::vector<std::set<std::string>> sets;
  for (const std::string& b : binaries) {
    Dl_info info{};
    void* h = dlopen(b.c_str(), RTLD_NOW | RTLD_NOLOAD);
    void* sym = h ? dlsym(h, "GridlineSuiteAUFactory") : nullptr;
    if (!sym && h) sym = dlsym(h, "GetPluginFactory");
    const std::string loaded = sym && dladdr(sym, &info) && info.dli_fname ? info.dli_fname : b;
    sets.push_back(classesOf(loaded));
    if (h) dlclose(h);
  }
  std::set<std::string> common;
  for (size_t i = 0; i < sets.size(); ++i)
    for (size_t j = i + 1; j < sets.size(); ++j)
      for (const std::string& c : sets[i])
        if (sets[j].count(c)) common.insert(c);
  std::string list;
  for (const std::string& c : common) list += c + " ";
  expect(common.empty(), "no compiled class name shared by two bundles: " + list);
  std::string compiled;
  for (const auto& s : sets)
    for (const std::string& c : s) compiled += c + " ";
  expect(sets[0].empty() && sets[1].empty(), "the VST3 binaries compile in no class (all made at run time): " + compiled);

  // Each AU's view factory, found by name in its own bundle as hosts do.
  auto factoryOf = [](const std::set<std::string>& s) {
    for (const std::string& c : s)
      if (c.find("Factory_T") != std::string::npos) return c;
    return std::string();
  };
  const std::string fp = factoryOf(sets[2]), fl = factoryOf(sets[3]);
  expect(!fp.empty() && !fl.empty() && fp != fl && fp.find("0000000000000000") == std::string::npos, "each AU has its own factory class: " + fp + " " + fl);
  Class cp = fp.empty() ? Nil : [plasmaAu classNamed:[NSString stringWithUTF8String:fp.c_str()]];
  Class cl = fl.empty() ? Nil : [loupeAu classNamed:[NSString stringWithUTF8String:fl.c_str()]];
  expect(cp != Nil && cl != Nil && cp != cl, "each bundle finds its own factory by name");
  expect(cp && [cp conformsToProtocol:@protocol(AUCocoaUIBase)], "the factory is a Cocoa view factory");
  // (The binaries stay loaded: Objective-C images are not unloaded.)
}

}  // namespace

int main() {
  const std::string& hermetic = test::hermeticTestEnv("k308_objc");
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  @autoreleasepool {
    testRuntimeClass();
    const std::string tmp = hermetic + "/single";
    fs::create_directories(tmp);
    testSingleDownloads(tmp);
  }
  return finish();
}

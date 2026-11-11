// K-180: an LV2 bundle of hundreds of plugins scans in little memory, and every plugin is listed.
// A bundle of about two hundred plugins (one shared binary, one .ttl a plugin) took gigabytes a
// second while it was scanned: every plugin's description read the whole bundle into a model of its
// own, every instance stayed alive after it was described, and the walk over the other bundles ran
// again for each plugin. The scanner was stopped at its memory cap and the bundle never loaded.
// This test writes such bundles itself, around a test binary of 256 plugins that each hold 24 MB
// (tests/lv2/ManyPlugins.cpp; plugin 7 refuses to start), and checks:
//   1. a description reads only its plugin's files, the files are parsed once and shared, the
//      other bundles are looked through once for many descriptions, and another copy of a plugin
//      (the small bundle repeats the big one's first 20 plugins, as when installed twice) is not
//      mixed into its description;
//   2. the scanner child (this program) scans a bundle of 240 plugins under a 1 GB cap, lists 239
//      and the one that failed, and its peak memory is no larger than for a bundle of 20;
//   3. in the scan service: the bundle is cached (in plugin-cache.json too), the plugin that failed
//      is listed on its own, and a later scan does not scan the bundle again;
//   4. paths spelt the Windows way (a bundle path with '\', a file URI with "/C:/", other case) are
//      the same file and inside the same bundle (checked on every system). The scanner child
//      reports its own peak memory, so the memory checks run on Windows too.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Check.h"
#include "Hermetic.h"
#include "core/Platform.h"
#include "host/HostedPlugins.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"
#include "host/lv2/Lv2World.h"
#include "host/lv2/Turtle.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

const std::string kLv2 = "http://lv2plug.in/ns/lv2core#";
std::string uriOf(int n) { return "urn:gridline:lv2many:" + std::to_string(n); }

// A bundle of `count` plugins of the test binary: the manifest, one .ttl a plugin (with 40 control
// ports more than the binary uses, so each is a real description's size) and one presets file that
// every plugin's preset points to (a file shared by many plugins).
std::string writeBundle(const std::string& dir, const std::string& name, int count) {
  const fs::path b = fs::path(dir) / (name + ".lv2");
  fs::create_directories(b);
  const std::string bin = fs::path(GRIDLINE_LV2_MANY_BINARY).filename().string();
  fs::copy_file(GRIDLINE_LV2_MANY_BINARY, b / bin, fs::copy_options::overwrite_existing);
  const char* head =
      "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
      "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n@prefix doap: <http://usefulinc.com/ns/doap#> .\n\n";
  std::ofstream man(b / "manifest.ttl"), pre(b / "presets.ttl");
  man << head;
  pre << head;
  for (int n = 0; n < count; ++n) {
    const std::string u = "<" + uriOf(n) + ">", p = "<" + uriOf(n) + "#warm>";
    man << u << " a lv2:Plugin ; lv2:binary <" << bin << "> ; rdfs:seeAlso <p" << n << ".ttl> .\n";
    man << p << " a pset:Preset ; lv2:appliesTo " << u << " ; rdfs:seeAlso <presets.ttl> .\n";
    pre << p << " a pset:Preset ; rdfs:label \"Warm " << n << "\" ; lv2:port [ lv2:symbol \"gain\" ; pset:value 0.5 ] .\n";
    std::ofstream t(b / ("p" + std::to_string(n) + ".ttl"));
    t << head << u << " a lv2:Plugin , lv2:AmplifierPlugin ;\n  doap:name \"Many " << n << "\" ;\n  lv2:port\n";
    t << "  [ a lv2:InputPort , lv2:AudioPort ; lv2:index 0 ; lv2:symbol \"in_l\" ; lv2:name \"In L\" ] ,\n";
    t << "  [ a lv2:InputPort , lv2:AudioPort ; lv2:index 1 ; lv2:symbol \"in_r\" ; lv2:name \"In R\" ] ,\n";
    t << "  [ a lv2:OutputPort , lv2:AudioPort ; lv2:index 2 ; lv2:symbol \"out_l\" ; lv2:name \"Out L\" ] ,\n";
    t << "  [ a lv2:OutputPort , lv2:AudioPort ; lv2:index 3 ; lv2:symbol \"out_r\" ; lv2:name \"Out R\" ] ,\n";
    t << "  [ a lv2:InputPort , lv2:ControlPort ; lv2:index 4 ; lv2:symbol \"gain\" ; lv2:name \"Gain\" ; lv2:default 1.0 ; "
         "lv2:minimum 0.0 ; lv2:maximum 2.0 ] ,\n";
    t << "  [ a lv2:OutputPort , lv2:ControlPort ; lv2:index 5 ; lv2:symbol \"latency\" ; lv2:name \"Latency\" ; "
         "lv2:designation lv2:latency ; lv2:minimum 0 ; lv2:maximum 1000 ]";
    for (int k = 0; k < 40; ++k)
      t << " ,\n  [ a lv2:InputPort , lv2:ControlPort ; lv2:index " << 6 + k << " ; lv2:symbol \"band" << k << "\" ; lv2:name \"Band "
        << k << "\" ; lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 3 ; lv2:portProperty lv2:enumeration ;\n"
        << "    lv2:scalePoint [ rdfs:label \"Off\" ; <http://www.w3.org/1999/02/22-rdf-syntax-ns#value> 0 ] , [ rdfs:label \"Low\" ; "
        << "<http://www.w3.org/1999/02/22-rdf-syntax-ns#value> 1 ] , [ rdfs:label \"Mid\" ; <http://www.w3.org/1999/02/22-rdf-syntax-ns#value> 2 ] ,"
        << " [ rdfs:label \"High\" ; <http://www.w3.org/1999/02/22-rdf-syntax-ns#value> 3 ] ]";
    t << " .\n";
  }
  return b.string();
}

void usePrefs(const std::string& folder) {
  host::PluginPrefs p = host::pluginPrefs();
  p.disabledFormats = {"vst3", "vst2", "au", "clap"};
  p.noDefaultFolders = {"vst3", "vst2", "clap", "lv2"};
  p.formatFolders.clear();
  p.formatFolders["lv2"] = {folder};
  p.extraFolders.clear();
  p.disabledFolders.clear();
  host::setPluginPrefs(p);
  platform::setEnv("LV2_PATH", "");
}

// 1. What a description reads.
void testDescribe(const std::string& folder, const std::string& bundle, int count) {
  const int builds0 = host::lv2::bundleIndexBuilds();
  auto d0 = host::lv2::describe(bundle, uriOf(0), nullptr);
  auto d1 = host::lv2::describe(bundle, uriOf(1), nullptr);
  expect(d0 && d1 && d0->name == "Many 0" && d1->name == "Many 1", "describe: plugins of the big bundle are described");
  if (!d0 || !d1) return;
  expect(d0->ports.size() == 46 && d0->presets.size() == 1 && d0->bundle == bundle && d0->presets[0].label == "Warm 0", "describe: ports and the preset from the shared file");
  expect(d0->model->objects(uriOf(1), kLv2 + "port").empty(), "describe: a plugin's model holds only its own files (not the next plugin's)");
  auto manifest = host::lv2::sharedFile((fs::path(bundle) / "manifest.ttl").string());
  auto presets = host::lv2::sharedFile((fs::path(bundle) / "presets.ttl").string());
  expect(manifest && d0->model->includes(manifest.get()) && d1->model->includes(manifest.get()), "describe: the manifest is parsed once and shared");
  expect(presets && d0->model->includes(presets.get()) && d1->model->includes(presets.get()), "describe: a file of many plugins is parsed once and shared");
  // Every file of the bundle together, as each description used to hold.
  host::lv2::Model all;
  for (const auto& e : fs::directory_iterator(bundle))
    if (e.path().extension() == ".ttl") all.loadFile(e.path().string());
  std::printf("describe: a plugin's model %zu triples, the whole bundle %zu\n", d0->model->size(), all.size());
  expect(d0->model->size() * 20 < all.size(), "describe: a plugin's model is a small part of the bundle");
  for (int n = 2; n < count; n += 7) {
    auto d = host::lv2::describe(bundle, uriOf(n), nullptr);
    expect(d != nullptr, ("describe: plugin " + std::to_string(n)).c_str());
  }
  std::printf("describe: other bundles looked through %d times for %d descriptions\n", host::lv2::bundleIndexBuilds() - builds0, 2 + count / 7);
  expect(host::lv2::bundleIndexBuilds() - builds0 <= 1, "describe: the other bundles are looked through once, not once a plugin");
  expect(host::lv2::describe(bundle, uriOf(0), nullptr) == d0, "describe: a description in use is reused");
  host::lv2::pluginBundles({folder});
  host::lv2::describe(bundle, uriOf(3), nullptr);
  expect(host::lv2::bundleIndexBuilds() - builds0 == 2, "describe: looking for bundles again makes the next description look again");
}

// The scanner child (this program) writes its own peak memory as it goes: every system, Windows too.
std::string peakFile(const std::string& dir, const std::string& bundle) { return dir + "/peak-" + fs::path(bundle).stem().string() + ".txt"; }
void reportPeak(const std::string& file) {
  std::thread([file] {
    double last = -1;
    for (;;) {
      const double now = platform::peakRssMb();
      if (now > last) {
        last = now;
        const std::string tmp = file + ".tmp";
        std::ofstream(tmp) << now;
        std::error_code ec;
        fs::rename(tmp, file, ec);  // whole numbers only, even if the child ends in the middle
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }).detach();
}
double readPeak(const std::string& file) {
  std::ifstream f(file);
  double v = 0;
  f >> v;
  return v;
}

const host::ScannedClass* findClass(const host::ScanResult& r, const std::string& uid) {
  for (const auto& c : r.classes)
    if (c.uid == uid) return &c;
  return nullptr;
}

// 2. The scanner child.
void testScanChild(const std::string& dir, const std::string& small, int smallCount, const std::string& big, int bigCount) {
  const std::string exe = platform::executablePath();
  auto t0 = std::chrono::steady_clock::now();
  host::ScanResult rs = host::scanIsolated(exe, "lv2", small, 120);
  const double peakSmall = readPeak(peakFile(dir, small));
  const double tSmall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  t0 = std::chrono::steady_clock::now();
  host::ScanResult rb = host::scanIsolated(exe, "lv2", big, 120);
  const double peakBig = readPeak(peakFile(dir, big));
  const double tBig = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("scan: %d plugins %s in %.1f s, %d plugins %s in %.1f s; peak memory of the scanner %.0f MB, then %.0f MB\n", smallCount,
              rs.ok ? "ok" : rs.error.c_str(), tSmall, bigCount, rb.ok ? "ok" : rb.error.substr(0, 200).c_str(), tBig, peakSmall, peakBig);
  expect(rs.ok && static_cast<int>(rs.classes.size()) == smallCount - 1, "scan: the small bundle is listed");
  expect(rb.ok, "scan: the bundle of 240 plugins scans under a 1 GB cap (not stopped for its memory)");
  expect(static_cast<int>(rb.classes.size()) == bigCount - 1, "scan: every plugin that starts is listed");
  bool all = true;
  for (int n = 0; n < bigCount; ++n) all = all && (n == 7 || findClass(rb, uriOf(n)));
  expect(all, "scan: each plugin of the bundle is there");
  expect(rb.failed.size() == 1 && rb.failed[0].uid == uriOf(7) && rb.failed[0].name == "Many 7" && !rb.failed[0].error.empty(),
         "scan: the plugin that refuses to start is listed as failed on its own");
  const host::ScannedClass* c = findClass(rb, uriOf(239));
  expect(c && c->inputChannels == 2 && c->outputChannels == 2 && c->programs.size() == 1 && c->programs[0] == "Warm 239" && c->params.size() == 41,
         "scan: a plugin's facts (channels, preset, parameters)");
  expect(peakSmall > 24 && peakBig > 24, "scan: the scanner's peak memory is known (one plugin's 24 MB at least)");
  expect(peakBig < peakSmall + 64, "scan: the scanner's peak memory does not grow with the number of plugins");
  expect(peakBig < 400, "scan: the scanner's peak memory stays far under the cap");
}

// 4. Windows spellings of one path, checked on every system (the functions take the Windows rules
// as a flag). On Windows a bundle's path has '\' separators where its files' URIs give '/', so a
// description read none of the plugin's own files there.
void testWindowsPaths() {
  namespace l = host::lv2;
  const std::string bundle = "C:\\Users\\me\\AppData\\Roaming\\LV2\\Many Big.lv2";
  std::string p;
  const std::string uri = l::resolveIri("file:///C:/Users/me/AppData/Roaming/LV2/Many%20Big.lv2/manifest.ttl", "p0.ttl");
  expect(l::uriToPath(uri, p) && l::pathInside(p, bundle, true), "windows: a file URI's path is inside the bundle spelt with '\\'");
  expect(l::pathKey(bundle + "\\", true) == "c:/users/me/appdata/roaming/lv2/many big.lv2" && l::pathKey("/C:/Users/me", true) == "c:/users/me",
         "windows: one spelling (separators, trailing '\\', the URI's '/C:', case)");
  expect(l::pathKey("C:\\x\\tmp/lv2\\ManyBig.lv2\\p0.ttl", true) == l::pathKey("c:/X/TMP/lv2/manybig.lv2/P0.TTL", true),
         "windows: mixed separators and case are the same file");
  expect(l::pathInside("C:\\x\\tmp/lv2\\ManyBig.lv2\\p0.ttl", "c:/x/TMP/lv2/manybig.lv2/", true), "windows: mixed spellings inside the bundle");
  expect(!l::pathInside("C:/Users/me/AppData/Roaming/LV2/Many Big.lv2b/p0.ttl", bundle, true), "windows: a bundle with a longer name is not inside");
  expect(!l::pathInside(bundle, bundle, true), "windows: the bundle itself is not inside it");
  // The scan service lists a bundle as discovery spells it ('/'); the same bundle spelt as Windows
  // gives it to the program (backslashes, a temp folder of mixed separators).
  expect(l::pathKey("C:\\Users\\me\\AppData\\Local\\Temp\\gridline-lv2many-1/lv2\\ManyBig.lv2", true) ==
             l::pathKey("C:/Users/me/AppData/Local/Temp/gridline-lv2many-1/lv2/ManyBig.lv2", true),
         "windows: the scan service's spelling of a bundle and the system's are the same");
  expect(l::pathInside("D:/a.ttl", "D:\\", true), "windows: a drive's root folder");
  expect(!l::pathInside("/x/A.lv2/p.ttl", "/x/a.lv2", false) && l::pathInside("/x/a.lv2/p.ttl", "/x/a.lv2/", false),
         "other systems: case counts, a trailing '/' does not");
}

int starts(const std::string& dir, const std::string& bundle) {
  std::ifstream f(dir + "/starts-" + fs::path(bundle).stem().string() + ".txt");
  std::string s;
  f >> s;
  return static_cast<int>(s.size());
}

// 3. The scan service and its cache.
void testService(const std::string& dir, const std::string& folder, const std::string& big, int bigCount) {
  const std::string cfg = dir + "/service";
  fs::create_directories(cfg);
  host::initHosting(cfg);
  usePrefs(folder);
  const std::string exe = platform::executablePath();
  auto run = [&] {
    host::startScan(exe, false);
    for (int i = 0; i < 3000 && !host::scanStatus().running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    host::stopScan();
  };
  const int before = starts(dir, big);
  run();
  const int first = starts(dir, big) - before;
  int classes = -1, failedRows = 0;
  std::string failedName;
  for (const auto& e : host::cacheEntries()) {
    // The service lists a bundle as discovery spells it ('/' separators); `big` has this system's
    // own (backslashes on Windows): the same bundle when compared in one spelling.
    if (host::lv2::pathKey(e.entry) != host::lv2::pathKey(big)) continue;
    if (e.ok) classes = e.classes;
    if (!e.ok && !e.plugin.empty()) ++failedRows, failedName = e.plugin;
  }
  std::printf("service: %d plugins cached, %d failed on their own (%s)\n", classes, failedRows, failedName.c_str());
  expect(classes == bigCount - 1, "service: the big bundle is cached with its plugins");
  expect(failedRows == 1 && failedName == "Many 7", "service: the plugin that failed is listed on its own");
  expect(host::scanStatus().failed >= 1, "service: it counts as failed");
  std::ifstream f(cfg + "/plugin-cache.json");
  std::stringstream ss;
  ss << f.rdbuf();
  expect(ss.str().find(fs::path(big).filename().string()) != std::string::npos && ss.str().find(uriOf(bigCount - 1)) != std::string::npos,
         "service: the bundle is in plugin-cache.json");
  run();
  expect(first == 1 && starts(dir, big) - before == 1, "service: a later scan does not scan the cached bundle again");
  host::shutdownScan();
}

}  // namespace

int main(int argc, char** argv) {
  // The scanner child (scanIsolated and the scan service run this program).
  if (argc >= 4 && std::strcmp(argv[1], "--scan-plugin") == 0) {
    if (const char* d = std::getenv("K180_DIR"); d && *d) {
      std::ofstream(std::string(d) + "/starts-" + fs::path(argv[3]).stem().string() + ".txt", std::ios::app) << "+";
      reportPeak(peakFile(d, argv[3]));
    }
    return host::childMain(argc, argv);
  }
  const std::string& dir = gl::test::hermeticTestEnv("lv2many");
  platform::setEnv("K180_DIR", dir.c_str());
  platform::setEnv("GRIDLINE_SCAN_MEMORY_MB", "1024");  // the old scan passed this within a few dozen plugins
  const std::string folder = dir + "/lv2";
  const std::string small = writeBundle(folder, "ManySmall", 20);
  const std::string big = writeBundle(folder, "ManyBig", 240);
  host::loadPluginPrefs(dir + "/config");
  usePrefs(folder);
  testDescribe(folder, big, 240);
  testWindowsPaths();
  testScanChild(dir, small, 20, big, 240);
  testService(dir, folder, big, 240);
  return finish();
}

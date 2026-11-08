// K-13: plugin search folders per format. Settings round trip (and old files), the rows a format
// lists (Default switch, environment variable, own folders, folders for every format, missing,
// listed twice, inside another), the OS's default folders, and the scan service: plugins come
// only from folders that are on, leave the list when their folder is removed or switched off,
// nested folders find a plugin once, one folder is rescanned on its own, and child processes
// read the same settings. Fixture plugins only (the test CLAP plugin copied into temp folders);
// every format's Default folders are switched off, so the user's installed plugins are never scanned.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Check.h"
#include "Hermetic.h"
#include "core/Platform.h"
#include "core/PluginInfo.h"
#include "host/HostedPlugins.h"
#include "host/PluginFolders.h"
#include "host/PluginGroups.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

const std::string kClapFixture = GRIDLINE_CLAP_TESTPLUG;

bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

const host::FolderRow* rowOf(const std::vector<host::FolderRow>& rows, const std::string& path, host::FolderSource src) {
  for (const auto& r : rows)
    if (r.path == path && r.source == src) return &r;
  return nullptr;
}

// A stand-in format whose default folders are temp folders (".fix" files are its plugins).
std::vector<std::string> gDefaults;
host::Format fakeFormat(const char* id) {
  host::Format f{};
  f.id = id;
  f.label = "FAKE";
  f.defaultPaths = [] { return gDefaults; };
  f.discover = [](const std::vector<std::string>& folders) { return host::findEntries(folders, ".fix", false, true); };
  return f;
}
void touch(const std::string& p) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream(p) << "x";
}

void testRoundTrip(const std::string& dir) {
  const std::string cfg = dir + "/roundtrip";
  fs::create_directories(cfg);
  host::loadPluginPrefs(cfg);
  host::PluginPrefs p = host::pluginPrefs();
  expect(p.formatFolders.empty() && p.disabledFolders.empty() && p.noDefaultFolders.empty(), "fresh settings: no own folders, every Default on");
  p.extraFolders = {"/every/one"};
  p.formatFolders["vst3"] = {"/a/vst3", "/b/vst3"};
  p.formatFolders["clap"] = {"C:\\Plugins\\CLAP"};
  p.disabledFolders = {host::folderKey("vst3", "/b/vst3"), host::folderKey("*", "/every/one")};
  p.noDefaultFolders = {"lv2", "vst2"};
  host::setPluginPrefs(p);
  host::loadPluginPrefs(cfg);  // read back from plugin-manager.json
  const host::PluginPrefs q = host::pluginPrefs();
  expect(q == p, "settings round trip: per-format folders, switched-off folders, Default switches");
  expect(q.formatFolders.at("clap").front() == "C:\\Plugins\\CLAP", "Windows paths kept as written");
  expect(host::pluginPrefsFile() == cfg + "/plugin-manager.json", "settings file known (handed to child processes)");

  // A file from before K-13: the folders for every format stay, everything else defaults.
  const std::string old = dir + "/old";
  fs::create_directories(old);
  std::ofstream(old + "/plugin-manager.json") << R"({"version":1,"extraFolders":["/x/y"],"disabledFormats":["vst2"]})";
  host::loadPluginPrefs(old);
  const host::PluginPrefs o = host::pluginPrefs();
  expect(o.extraFolders == std::vector<std::string>{"/x/y"} && o.disabledFormats.count("vst2") && o.formatFolders.empty() &&
             o.disabledFolders.empty() && o.noDefaultFolders.empty(),
         "old settings file: folders kept, new switches default");
  // A child process reads the parent's file and never writes it.
  host::loadPluginPrefs(cfg);
  const std::string file = host::pluginPrefsFile();
  host::loadPluginPrefsFile(old + "/plugin-manager.json");
  expect(host::pluginPrefs().extraFolders == std::vector<std::string>{"/x/y"} && host::pluginPrefsFile().empty(), "child: settings read only");
  // childMain reads --plugin-prefs (no child mode named: returns -1, settings loaded).
  std::string a0 = "gridline", a1 = "--plugin-prefs", a2 = file;
  char* argv[] = {a0.data(), a1.data(), a2.data(), nullptr};
  expect(host::childMain(3, argv) == -1, "childMain without a child mode: not a child");
  host::loadPluginPrefs(cfg);
}

void testRows(const std::string& dir) {
  const std::string d1 = dir + "/rows/defA", d2 = dir + "/rows/missingDefault", own = dir + "/rows/own", envDir = dir + "/rows/env",
                    every = dir + "/rows/every", nested = dir + "/rows/defA/inner";
  fs::create_directories(d1);
  fs::create_directories(own);
  fs::create_directories(envDir);
  fs::create_directories(every);
  fs::create_directories(nested);
  touch(d1 + "/one.fix");
  touch(nested + "/two.fix");
  touch(own + "/three.fix");
  touch(envDir + "/four.fix");
  touch(every + "/five.fix");
  gDefaults = {d1, d2};
  const host::Format f = fakeFormat("clap");  // uses CLAP_PATH as its variable
  platform::setEnv("CLAP_PATH", envDir.c_str());

  host::PluginPrefs p;
  p.formatFolders["clap"] = {own, d1 + "/", nested, dir + "/rows/gone"};
  p.extraFolders = {every};
  auto rows = host::folderRows(f, p);
  const auto* r1 = rowOf(rows, d1, host::FolderSource::Default);
  const auto* r2 = rowOf(rows, d2, host::FolderSource::Default);
  const auto* re = rowOf(rows, envDir, host::FolderSource::Env);
  const auto* ro = rowOf(rows, own, host::FolderSource::User);
  const auto* rdup = rowOf(rows, d1 + "/", host::FolderSource::User);
  const auto* rin = rowOf(rows, nested, host::FolderSource::User);
  const auto* rgone = rowOf(rows, dir + "/rows/gone", host::FolderSource::User);
  const auto* rev = rowOf(rows, every, host::FolderSource::Every);
  expect(r1 && r1->searched(), "default folder listed and searched");
  expect(r2 && !r2->exists && !r2->searched(), "missing default folder shown as missing, not searched");
  expect(re && re->searched(), "environment variable folder listed (always on)");
  expect(ro && ro->searched() && rev && rev->searched(), "own folder and folder for every format searched");
  expect(rdup && rdup->duplicate && !rdup->searched(), "the same folder spelt differently: searched once");
  expect(rin && rin->searched() && rin->inside == d1, "a folder inside another is marked");
  expect(rgone && !rgone->exists, "an own folder that is gone is shown as missing");
  const auto found = host::discoverFormat(f, p);
  expect(found.size() == 5, "every plugin found once (nested and duplicate folders)");

  // Switches: Default off (environment variable still searched), one own folder off.
  p.noDefaultFolders.insert("clap");
  p.disabledFolders.insert(host::folderKey("clap", own));
  p.disabledFolders.insert(host::folderKey("*", every));
  const auto folders = host::searchFolders(f, p);
  expect(!has(folders, d1) && has(folders, envDir) && !has(folders, own) && !has(folders, every), "Default off, own folder off: only the rest is searched");
  expect(has(folders, d1 + "/") && has(folders, nested), "a folder listed again is searched once the Default is off");
  const auto found2 = host::discoverFormat(f, p);
  expect(found2.size() == 3, "switched-off folders' plugins are not found (env, own copy of the default, nested)");
  platform::setEnv("CLAP_PATH", "");

  // AU: folders come from the system's registry; nothing to add or switch.
  expect(!host::folderSearchApplies("au") && host::folderSearchApplies("vst3") && host::folderSearchApplies("lv2"), "AU folders are not Gridline's to change");
  expect(std::string(host::folderEnvVar("vst2")) == "VST_PATH" && std::string(host::folderEnvVar("vst3")) == "VST3_PATH" &&
             std::string(host::folderEnvVar("lv2")) == "LV2_PATH" && std::string(host::folderEnvVar("au")).empty(),
         "environment variables per format");
  expect(host::folderInside("/a/b/c", "/a/b") && !host::folderInside("/a/bc", "/a/b") && !host::folderInside("/a/b", "/a/b"), "inside: whole path parts only");
}

void testDefaultsPerOs() {
  auto defaults = [](const char* id) {
    const host::Format* f = host::findFormat(id);
    return f ? f->defaultPaths() : std::vector<std::string>{};
  };
  auto any = [](const std::vector<std::string>& v, const std::string& part) {
    for (const auto& x : v)
      if (x.find(part) != std::string::npos) return true;
    return false;
  };
#if defined(__APPLE__)
  expect(has(defaults("vst3"), "/Library/Audio/Plug-Ins/VST3"), "macOS VST3 default");
  expect(has(defaults("clap"), "/Library/Audio/Plug-Ins/CLAP"), "macOS CLAP default");
  expect(has(defaults("vst2"), "/Library/Audio/Plug-Ins/VST"), "macOS VST2 default");
  expect(has(defaults("lv2"), "/Library/Audio/Plug-Ins/LV2"), "macOS LV2 default");
  expect(has(defaults("au"), "/Library/Audio/Plug-Ins/Components"), "macOS AU folders shown");
#elif defined(_WIN32)
  expect(any(defaults("vst3"), "Common Files\\VST3") || any(defaults("vst3"), "Common Files/VST3"), "Windows VST3 default (Common Files)");
  expect(any(defaults("clap"), "CLAP"), "Windows CLAP default (Common Files)");
  expect(any(defaults("vst2"), "VSTPlugins") && any(defaults("vst2"), "Steinberg"), "Windows VST2 defaults (VSTPlugins, Steinberg)");
  expect(any(defaults("lv2"), "LV2"), "Windows LV2 default");
#else
  expect(has(defaults("vst3"), "/usr/lib/vst3") && has(defaults("vst3"), "/usr/local/lib/vst3"), "Unix VST3 defaults");
  expect(has(defaults("clap"), "/usr/lib/clap"), "Unix CLAP default");
  expect(has(defaults("vst2"), "/usr/lib/vst"), "Unix VST2 default");
  expect(has(defaults("lv2"), "/usr/lib/lv2"), "Unix LV2 default");
  expect(any(defaults("vst3"), "/.vst3") && any(defaults("clap"), "/.clap") && any(defaults("lv2"), "/.lv2"), "Unix user folders");
#endif
  (void)any;
}

std::vector<std::string> cachedClap() {
  std::vector<std::string> v;
  for (const auto& e : host::cacheEntries())
    if (e.format == "clap" && e.ok) v.push_back(e.entry);
  return v;
}
std::string clapType() {
  for (const auto& c : host::hostedClasses())
    if (c->format == "clap") return c->type;
  return {};
}
std::string bundleOf(const std::string& type) {
  auto c = host::hostedClass(type);
  return c ? c->bundle : std::string();
}

void testScanService(const std::string& dir) {
  const std::string exe = GRIDLINE_CLI_EXE;
  const std::string cfg = dir + "/service", f1 = dir + "/service/one", f2 = dir + "/service/two";
  fs::create_directories(f1);
  fs::create_directories(f2);
  const std::string name = fs::path(kClapFixture).filename().string();
  fs::copy(kClapFixture, f1 + "/" + name, fs::copy_options::recursive);
  fs::copy(kClapFixture, f2 + "/" + name, fs::copy_options::recursive);
  const std::string e1 = (fs::path(f1) / name).generic_string(), e2 = (fs::path(f2) / name).generic_string();
  host::initHosting(cfg);
  host::PluginPrefs p = host::pluginPrefs();
  p.disabledFormats = {"vst3", "vst2", "au", "lv2"};
  p.noDefaultFolders = {"vst3", "vst2", "clap", "lv2"};  // never the computer's installed plugins
  p.formatFolders["clap"] = {f1, f2};
  p.disabledFolders = {host::folderKey("clap", f2)};
  host::setPluginPrefs(p);
  platform::setEnv("CLAP_PATH", "");

  host::startScan(exe, false);
  host::stopScan();
  auto c = cachedClap();
  const std::string type = clapType();
  expect(c.size() == 1 && c[0] == e1, "scan: the plugin in the folder that is on, not the one switched off");
  expect(!type.empty() && bundleOf(type) == e1, "scan: its class is listed from that folder");
  expect(host::listedInBrowser(type), "scan: the browser lists it");

  // Switch folders: the first off, the second on. requestScan drops the first one's plugin.
  p.disabledFolders = {host::folderKey("clap", f1)};
  host::setPluginPrefs(p);
  host::requestScan(exe);
  host::stopScan();
  c = cachedClap();
  expect(c.size() == 1 && c[0] == e2, "rescan: the plugin now comes from the folder switched on");
  expect(bundleOf(type) == e2, "rescan: its class moved to that folder");

  // Remove every folder: the plugin leaves the list (a project keeps its placeholder).
  p.formatFolders.erase("clap");
  p.disabledFolders.clear();
  host::setPluginPrefs(p);
  host::requestScan(exe);
  host::stopScan();
  expect(cachedClap().empty(), "removed folder: its plugin leaves the cache");
  expect(!host::hostedClass(type) && !host::listedInBrowser(type), "removed folder: its class leaves the list and the browser");
  expect(findPlugin(type) != nullptr && isHostedType(type), "removed folder: a project's plugin type stays known (placeholder)");

  // Nested and duplicate folders: the plugin is found and scanned once.
  p.formatFolders["clap"] = {dir + "/service", f1, f1 + "/"};
  host::setPluginPrefs(p);
  int n = 0;
  for (const auto& [fid, e] : host::discoverPluginEntries()) n += fid == "clap" ? 1 : 0;
  expect(n == 2, "nested and duplicate folders: each plugin found once (one, two)");
  host::requestScan(exe);
  host::stopScan();
  expect(cachedClap().size() == 2, "nested folders: two entries cached");

  // Rescan one folder: only its plugin is scanned.
  host::rescanFolder(exe, "clap", f2);
  host::stopScan();
  expect(host::scanStatus().total == 1 && host::scanStatus().done == 1, "rescan a folder: only its plugin");
  // K-71: "*" (a folder for every format): the formats that are on, searched on the scan worker.
  host::rescanFolder(exe, "*", f2);
  host::stopScan();
  expect(host::scanStatus().total == 1 && host::scanStatus().done == 1, "rescan a folder for every format: its plugin");

  // A missing folder never breaks a scan.
  p.formatFolders["clap"].push_back(dir + "/service/not-here");
  host::setPluginPrefs(p);
  host::requestScan(exe);
  host::stopScan();
  expect(cachedClap().size() == 2 && !host::scanStatus().running, "missing folder: the scan runs as before");
  host::shutdownScan();
}

}  // namespace

int main() {
  const std::string dir = gl::test::hermeticTestEnv("plugin-folders");
  testRoundTrip(dir);
  testRows(dir);
  testDefaultsPerOs();
  testScanService(dir);
  return finish();
}

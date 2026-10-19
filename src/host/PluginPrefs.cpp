#include "PluginPrefs.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include "../core/Json.h"
#include "../core/Project.h"

namespace fs = std::filesystem;

namespace gl::host {

namespace {
struct Store {
  std::mutex lock;
  std::string file;
  PluginPrefs prefs;
  std::atomic<uint64_t> generation{0};
};
Store& store() {
  static auto* s = new Store;  // never destroyed
  return *s;
}

Json jbool(bool b) {
  Json j;
  j.type = Json::Type::Bool;
  j.boolean = b;
  return j;
}
bool readBool(const Json& o, const char* key, bool dflt) {
  const Json* v = o.find(key);
  return v && v->is(Json::Type::Bool) ? v->boolean : dflt;
}

Json strings(const std::vector<std::string>& v) {
  Json a = Json::makeArray();
  for (const auto& s : v) a.array.push_back(Json::makeString(s));
  return a;
}
Json strings(const std::set<std::string>& v) { return strings(std::vector<std::string>(v.begin(), v.end())); }
std::vector<std::string> readStrings(const Json& o, const char* key) {
  std::vector<std::string> out;
  if (const Json* l = o.find(key); l && l->is(Json::Type::Array))
    for (const auto& s : l->array) out.push_back(s.str);
  return out;
}

void save(const Store& s) {
  if (s.file.empty()) return;
  Json j = Json::makeObject();
  j.object["version"] = Json::makeNumber(1);
  j.object["extraFolders"] = strings(s.prefs.extraFolders);
  j.object["disabledFormats"] = strings(s.prefs.disabledFormats);
  j.object["sandbox"] = strings(s.prefs.sandbox);
  j.object["favourites"] = strings(s.prefs.favourites);
  Json cats = Json::makeObject();
  for (const auto& [k, v] : s.prefs.categories) cats.object[k] = Json::makeString(v);
  j.object["categories"] = std::move(cats);
  Json opts = Json::makeObject();
  for (const auto& [k, o] : s.prefs.options) {
    if (o == HostOptions{}) continue;
    Json x = Json::makeObject();
    x.object["fixedBuffers"] = jbool(o.fixedBuffers);
    x.object["fixedSize"] = Json::makeNumber(o.fixedSize);
    x.object["maxBuffers"] = jbool(o.maxBuffers);
    x.object["notifyRender"] = jbool(o.notifyRender);
    x.object["modulation"] = jbool(o.modulation);
    x.object["genericEditor"] = jbool(o.genericEditor);
    opts.object[k] = std::move(x);
  }
  j.object["options"] = std::move(opts);
  std::error_code ec;
  fs::create_directories(fs::path(s.file).parent_path(), ec);
  writeFileAtomic(s.file, jsonStringify(j, 1));  // per-writer temp name (several instances share the file)
}
}  // namespace

void loadPluginPrefs(const std::string& configDir) {
  Store& s = store();
  std::lock_guard<std::mutex> g(s.lock);
  s.file = configDir.empty() ? std::string() : configDir + "/plugin-manager.json";
  s.prefs = PluginPrefs{};
  std::ifstream f(s.file, std::ios::binary);
  if (f) {
    std::stringstream ss;
    ss << f.rdbuf();
    Json j;
    if (jsonParse(ss.str(), j) && j.is(Json::Type::Object)) {
      s.prefs.extraFolders = readStrings(j, "extraFolders");
      for (auto& x : readStrings(j, "disabledFormats")) s.prefs.disabledFormats.insert(x);
      for (auto& x : readStrings(j, "sandbox")) s.prefs.sandbox.insert(x);
      for (auto& x : readStrings(j, "favourites")) s.prefs.favourites.insert(x);
      if (const Json* c = j.find("categories"); c && c->is(Json::Type::Object))
        for (const auto& [k, v] : c->object)
          if (v.is(Json::Type::String) && !v.str.empty()) s.prefs.categories[k] = v.str;
      if (const Json* o = j.find("options"); o && o->is(Json::Type::Object))
        for (const auto& [k, v] : o->object) {
          if (!v.is(Json::Type::Object)) continue;
          HostOptions h;
          h.fixedBuffers = readBool(v, "fixedBuffers", h.fixedBuffers);
          h.fixedSize = jsonInt<int>(std::clamp(v.num("fixedSize", h.fixedSize), 32.0, 4096.0));
          h.maxBuffers = readBool(v, "maxBuffers", h.maxBuffers);
          h.notifyRender = readBool(v, "notifyRender", h.notifyRender);
          h.modulation = readBool(v, "modulation", h.modulation);
          h.genericEditor = readBool(v, "genericEditor", h.genericEditor);
          s.prefs.options[k] = h;
        }
    }
  }
  ++s.generation;
}

PluginPrefs pluginPrefs() {
  Store& s = store();
  std::lock_guard<std::mutex> g(s.lock);
  return s.prefs;
}

void setPluginPrefs(const PluginPrefs& p) {
  Store& s = store();
  std::lock_guard<std::mutex> g(s.lock);
  if (s.prefs == p) return;
  s.prefs = p;
  save(s);
  ++s.generation;
}

bool runsInSandbox(const std::string& type) {
  // GRIDLINE_SANDBOX_ALL=1: every hosted plugin in the sandbox (testing).
  static const bool all = [] {
    const char* e = std::getenv("GRIDLINE_SANDBOX_ALL");
    return e && *e && *e != '0';
  }();
  if (all) return true;
  Store& s = store();
  std::lock_guard<std::mutex> g(s.lock);
  return s.prefs.sandbox.count(type) > 0;
}

void setRunInSandbox(const std::string& type, bool on) {
  PluginPrefs p = pluginPrefs();
  if (on)
    p.sandbox.insert(type);
  else
    p.sandbox.erase(type);
  setPluginPrefs(p);
}

bool formatEnabled(const std::string& format) {
  Store& s = store();
  std::lock_guard<std::mutex> g(s.lock);
  return s.prefs.disabledFormats.count(format) == 0;
}

HostOptions hostOptions(const std::string& type) {
  Store& s = store();
  std::lock_guard<std::mutex> g(s.lock);
  auto it = s.prefs.options.find(type);
  return it == s.prefs.options.end() ? HostOptions{} : it->second;
}

void setHostOptions(const std::string& type, const HostOptions& o) {
  PluginPrefs p = pluginPrefs();
  if (o == HostOptions{})
    p.options.erase(type);
  else
    p.options[type] = o;
  setPluginPrefs(p);
}

uint64_t pluginPrefsGeneration() { return store().generation.load(); }

}  // namespace gl::host

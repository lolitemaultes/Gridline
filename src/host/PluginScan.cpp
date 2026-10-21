#include "PluginScan.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include "../core/Project.h"
#include "sandbox/Sandbox.h"
#include "../core/Platform.h"

namespace fs = std::filesystem;

namespace gl::host {

// Per-format descriptions (vst3/Vst3Backend.cpp, clap/ClapBackend.cpp, …).
Format vst3Format();
Format clapFormat();
Format vst2Format();
Format lv2Format();
#if defined(__APPLE__)
Format auFormat();
#endif

namespace {
constexpr double kCacheVersion = 3;  // 3: every format in one cache, host parameter flags
Json str(const std::string& s) { return Json::makeString(s); }
Json num(double v) { return Json::makeNumber(v); }
Json boolean(bool b) {
  Json j;
  j.type = Json::Type::Bool;
  j.boolean = b;
  return j;
}
bool flag(const Json& o, const char* key) {
  const Json* v = o.find(key);
  return v && v->is(Json::Type::Bool) && v->boolean;
}
Json strings(const std::vector<std::string>& v) {
  Json a = Json::makeArray();
  for (const auto& s : v) a.array.push_back(str(s));
  return a;
}
std::vector<std::string> readStrings(const Json& o, const char* key) {
  std::vector<std::string> out;
  if (const Json* l = o.find(key); l && l->is(Json::Type::Array))
    for (const auto& s : l->array) out.push_back(s.str);
  return out;
}
}  // namespace

Json toJson(const ScanResult& r) {
  Json j = Json::makeObject();
  j.object["format"] = str(r.format);
  j.object["bundle"] = str(r.bundle);
  j.object["mtime"] = num(static_cast<double>(r.mtime));
  j.object["ok"] = boolean(r.ok);
  if (!r.error.empty()) j.object["error"] = str(r.error);
  Json classes = Json::makeArray();
  for (const auto& c : r.classes) {
    Json jc = Json::makeObject();
    jc.object["uid"] = str(c.uid);
    jc.object["name"] = str(c.name);
    jc.object["vendor"] = str(c.vendor);
    jc.object["version"] = str(c.version);
    jc.object["subCategories"] = str(c.subCategories);
    jc.object["sdkVersion"] = str(c.sdkVersion);
    jc.object["instrument"] = boolean(c.instrument);
    jc.object["eventInput"] = boolean(c.eventInput);
    if (c.eventOutput) jc.object["eventOutput"] = boolean(true);
    jc.object["inputs"] = num(c.inputChannels);
    jc.object["outputs"] = num(c.outputChannels);
    jc.object["sidechain"] = num(c.sidechainChannels);
    jc.object["latency"] = num(c.latency);
    if (!c.auxOutputs.empty()) jc.object["auxOutputs"] = strings(c.auxOutputs);
    if (!c.programs.empty()) jc.object["programs"] = strings(c.programs);
    Json ps = Json::makeArray();
    for (const auto& p : c.params) {
      Json jp = Json::makeObject();
      jp.object["id"] = num(p.id);
      jp.object["title"] = str(p.title);
      if (!p.units.empty()) jp.object["units"] = str(p.units);
      if (p.stepCount) jp.object["steps"] = num(p.stepCount);
      jp.object["def"] = num(p.def);
      if (p.flags) jp.object["flags"] = num(p.flags);
      if (!p.labels.empty()) jp.object["labels"] = strings(p.labels);
      ps.array.push_back(std::move(jp));
    }
    jc.object["params"] = std::move(ps);
    classes.array.push_back(std::move(jc));
  }
  j.object["classes"] = std::move(classes);
  return j;
}

bool fromJson(const Json& j, ScanResult& r) {
  if (!j.is(Json::Type::Object)) return false;
  r = ScanResult{};
  r.format = j.text("format");
  if (r.format.empty()) r.format = "vst3";
  r.bundle = j.text("bundle");
  r.mtime = jsonInt<int64_t>(j.num("mtime"));
  r.ok = flag(j, "ok");
  r.error = j.text("error");
  if (const Json* cs = j.find("classes"); cs && cs->is(Json::Type::Array))
    for (const auto& jc : cs->array) {
      ScannedClass c;
      c.uid = jc.text("uid");
      c.name = jc.text("name");
      c.vendor = jc.text("vendor");
      c.version = jc.text("version");
      c.subCategories = jc.text("subCategories");
      c.sdkVersion = jc.text("sdkVersion");
      c.instrument = flag(jc, "instrument");
      c.eventInput = flag(jc, "eventInput");
      c.eventOutput = flag(jc, "eventOutput");
      c.inputChannels = jsonInt<int>(jc.num("inputs"));
      c.outputChannels = jsonInt<int>(jc.num("outputs"));
      c.sidechainChannels = jsonInt<int>(jc.num("sidechain"));
      c.latency = jsonInt<int>(jc.num("latency"));
      c.auxOutputs = readStrings(jc, "auxOutputs");
      c.programs = readStrings(jc, "programs");
      if (const Json* ps = jc.find("params"); ps && ps->is(Json::Type::Array))
        for (const auto& jp : ps->array) {
          ScannedParam p;
          p.id = jsonInt<uint32_t>(jp.num("id"));
          p.title = jp.text("title");
          p.units = jp.text("units");
          p.stepCount = jsonInt<int>(jp.num("steps"));
          p.def = std::clamp(jp.num("def"), 0.0, 1.0);
          p.flags = jsonInt<int32_t>(jp.num("flags"));
          p.labels = readStrings(jp, "labels");
          c.params.push_back(std::move(p));
        }
      if (!c.uid.empty() && (r.format != "vst3" || c.uid.size() == 32)) r.classes.push_back(std::move(c));
    }
  return !r.bundle.empty();
}

// ---------------------------------------------------------------- formats

const std::vector<Format>& formats() {
  static const std::vector<Format> f = [] {
    std::vector<Format> v{vst3Format(), clapFormat(), vst2Format(), lv2Format()};
#if defined(__APPLE__)
    v.push_back(auFormat());
#endif
    return v;
  }();
  return f;
}

const Format* findFormat(const std::string& id) {
  for (const auto& f : formats())
    if (id == f.id) return &f;
  return nullptr;
}

bool splitType(const std::string& type, std::string& format, std::string& uid) {
  size_t c = type.find(':');
  if (c == std::string::npos || c == 0) return false;
  format = type.substr(0, c);
  uid = type.substr(c + 1);
  return findFormat(format) != nullptr && !uid.empty();
}

std::unique_ptr<Backend> createBackend(const std::string& format, const std::string& entry, const std::string& uid, std::string* err) {
  const Format* f = findFormat(format);
  if (!f) {
    if (err) *err = "unsupported plugin format " + format;
    return nullptr;
  }
  return f->create(entry, uid, err);
}

// ---------------------------------------------------------------- files

int64_t pathMtime(const std::string& path) {
  std::error_code ec;
  int64_t best = 0;
  auto stamp = [&](const fs::path& p) {
    auto t = fs::last_write_time(p, ec);
    if (ec) return;
    auto s = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    best = std::max<int64_t>(best, static_cast<int64_t>(s));
  };
  stamp(path);
  // Bundles: the binaries change on updates even when the folder does not.
  if (fs::is_directory(path, ec)) {
    fs::path contents = fs::path(path) / "Contents";
    for (const char* sub : {"MacOS", "x86_64-linux", "aarch64-linux", "i386-linux"}) {
      fs::path d = contents / sub;
      if (!fs::is_directory(d, ec)) continue;
      for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) stamp(it->path());
    }
    stamp(contents / "Info.plist");
  }
  return best;
}

std::vector<std::string> findEntries(const std::vector<std::string>& folders, const std::string& ext, bool directories, bool files) {
  std::vector<std::string> out;
  for (const auto& f : folders) {
    std::error_code ec;
    if (!fs::is_directory(f, ec)) continue;
    fs::recursive_directory_iterator it(f, fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const fs::path& p = it->path();
      const bool dir = it->is_directory(ec);
      std::string e = p.extension().string();
      for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (dir) {
        // Never look inside bundles (of any format).
        bool bundle = e == ".vst3" || e == ".vst" || e == ".clap" || e == ".component" || e == ".lv2" || e == ".app" || e == ".bundle" || e == ".framework";
        if (e == ext && directories) out.push_back(p.string());
        if (bundle) it.disable_recursion_pending();
      } else if (e == ext && files) {
        out.push_back(p.string());
      }
      if (it.depth() > 6) it.disable_recursion_pending();
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string fourCharCode(uint32_t v) {
  const char c[4] = {static_cast<char>((v >> 24) & 0xFF), static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                     static_cast<char>(v & 0xFF)};
  for (char ch : c)
    if (ch < 32 || ch > 126 || ch == '/') {
      char b[16];
      std::snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(v));
      return b;
    }
  return std::string(c, 4);
}

std::string auKey(uint32_t type, uint32_t subtype, uint32_t manufacturer) {
  return fourCharCode(type) + "/" + fourCharCode(subtype) + "/" + fourCharCode(manufacturer);
}

std::string vst2Type(uint32_t uniqueId) {
  char b[16];
  std::snprintf(b, sizeof b, "vst2:%08X", static_cast<unsigned>(uniqueId));
  return b;
}

void addEnvPaths(std::vector<std::string>& v, const char* var) {
  const char* e = std::getenv(var);
  if (!e) return;
  std::string s = e;
  size_t a = 0;
  while (a <= s.size()) {
#if defined(_WIN32)
    size_t b = s.find(';', a);  // Windows paths hold ':' (C:\...)
#else
    size_t b = s.find(':', a);
#endif
    std::string p = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (!p.empty() && std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
    if (b == std::string::npos) break;
    a = b + 1;
  }
}

std::string envFolder(const char* var, const char* sub) {
  const char* e = std::getenv(var);
  if (!e || !*e) return {};
  return (fs::path(e) / sub).string();
}

void addSystemPluginFolders(std::vector<std::string>& v, const char* winCommon, const char* unixLib) {
#if defined(_WIN32)
  // Per user: %LOCALAPPDATA%\Programs\Common\<format>; everyone: %COMMONPROGRAMFILES%\<format>.
  for (const std::string& p : {envFolder("LOCALAPPDATA", (std::string("Programs/Common/") + winCommon).c_str()), envFolder("CommonProgramFiles", winCommon)})
    if (!p.empty()) v.push_back(p);
  (void)unixLib;
#else
  (void)winCommon;
  for (const char* prefix : {"/usr/lib/", "/usr/local/lib/", "/usr/pkg/lib/"}) {
    const std::string p = std::string(prefix) + unixLib;
    if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
  }
#endif
}

std::string selfExecutablePath() {
  const std::string p = platform::executablePath();
  if (p.empty()) return p;
  std::error_code ec;
  auto canon = fs::canonical(p, ec);
  return ec ? p : canon.string();
}

// ---------------------------------------------------------------- child processes

int childMain(int argc, char** argv) {
  std::string format, entry, out, sandbox;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scan-vst3") == 0 && i + 1 < argc) {
      format = "vst3";
      entry = argv[++i];
    } else if (std::strcmp(argv[i], "--scan-plugin") == 0 && i + 2 < argc) {
      format = argv[++i];
      entry = argv[++i];
    } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if (std::strcmp(argv[i], "--plugin-sandbox") == 0 && i + 1 < argc) {
      sandbox = argv[++i];
    }
  }
  if (!sandbox.empty()) return sandboxChildMain(sandbox);
  if (entry.empty()) return -1;
  ScanResult r;
  r.format = format;
  r.bundle = entry;
  if (const Format* f = findFormat(format))
    r = f->scanInProcess(entry);
  else
    r.error = "unsupported plugin format " + format;
  std::string text = jsonStringify(toJson(r), 0);
  if (out.empty()) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
  } else {
    std::ofstream f(out, std::ios::binary);
    f << text;
    if (!f) return 3;
  }
  // Leave without static destructors or module exit calls (plugins that crash
  // while unloading must not fail the scan).
  std::_Exit(r.ok ? 0 : 2);
}

ScanResult scanIsolated(const std::string& exe, const std::string& format, const std::string& entry, double timeoutSeconds) {
  ScanResult r;
  r.format = format;
  r.bundle = entry;
  const Format* fmt = findFormat(format);
  r.mtime = fmt ? fmt->mtime(entry) : 0;
  static std::atomic<int> serial{0};
  fs::path tmp = fs::temp_directory_path() / ("gridline-plugin-scan-" + std::to_string(platform::processId()) + "-" + std::to_string(serial++) + ".json");
  std::vector<std::string> args = {exe, "--scan-plugin", format, entry, "--out", tmp.string()};
  platform::Process child;
  if (!child.start(exe, args)) {
    r.error = "cannot start the scanner process";
    return r;
  }
  const bool done = child.wait(timeoutSeconds);
  if (!done) {
    child.kill();
    child.wait();
    r.error = "timed out while loading";
    std::error_code ec;
    fs::remove(tmp, ec);
    return r;
  }
  std::string text;
  {
    std::ifstream f(tmp, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
  }
  std::error_code ec;
  fs::remove(tmp, ec);
  if (child.status().crashed) {
    r.error = "crashed while loading (" + child.status().describe().substr(9);
    return r;
  }
  Json j;
  ScanResult parsed;
  if (text.empty() || !jsonParse(text, j) || !fromJson(j, parsed)) {
    r.error = "scanner exited with code " + std::to_string(child.status().code);
    return r;
  }
  parsed.format = format;
  parsed.bundle = entry;
  parsed.mtime = r.mtime;
  return parsed;
}

// ---------------------------------------------------------------- cache

bool ScanCache::load() {
  entries_.clear();
  std::ifstream f(file_, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || !j.is(Json::Type::Object)) return false;
  if (j.num("version") != kCacheVersion) return false;  // older layout: rescan everything
  if (const Json* b = j.find("bundles"); b && b->is(Json::Type::Array))
    for (const auto& e : b->array) {
      ScanResult r;
      if (fromJson(e, r)) entries_.push_back(std::move(r));
    }
  return true;
}

bool ScanCache::save() const {
  if (file_.empty()) return false;
  Json j = Json::makeObject();
  j.object["version"] = num(kCacheVersion);
  Json b = Json::makeArray();
  for (const auto& e : entries_) b.array.push_back(toJson(e));
  j.object["bundles"] = std::move(b);
  std::error_code ec;
  fs::create_directories(fs::path(file_).parent_path(), ec);
  // Per-writer temp name: the app's scanner and `gridline scan-plugins` (or a
  // second instance) may save the shared cache at the same time.
  return writeFileAtomic(file_, jsonStringify(j, 1));
}

const ScanResult* ScanCache::find(const std::string& bundle, const std::string& format) const {
  for (const auto& e : entries_)
    if (e.bundle == bundle && (format.empty() || e.format == format)) return &e;
  return nullptr;
}

void ScanCache::put(ScanResult r) {
  for (auto& e : entries_)
    if (e.bundle == r.bundle && e.format == r.format) {
      e = std::move(r);
      return;
    }
  entries_.push_back(std::move(r));
}

void ScanCache::remove(const std::string& bundle, const std::string& format) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const ScanResult& e) { return e.bundle == bundle && (format.empty() || e.format == format); }),
                 entries_.end());
}

std::vector<std::string> ScanCache::stale(const std::vector<std::string>& bundles, const std::string& format) const {
  const Format* f = findFormat(format);
  std::vector<std::string> out;
  for (const auto& b : bundles) {
    const ScanResult* e = find(b, format);
    if (!e || !f || e->mtime != f->mtime(b)) out.push_back(b);
  }
  return out;
}

}  // namespace gl::host

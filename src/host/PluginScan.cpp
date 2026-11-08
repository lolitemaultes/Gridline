#include "PluginScan.h"
#include "PluginPrefs.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
constexpr double kKillWaitSeconds = 10;       // K-73: how long a killed scanner child is waited for
constexpr double kScanLongestSeconds = 1800;  // K-73: one entry at most, however it progresses (shells)
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
    best = std::max<int64_t>(best, platform::fileTimeSeconds(t));
  };
  stamp(path);
  // Bundles: the binaries change on updates even when the folder does not.
  if (fs::is_directory(path, ec)) {
    fs::path contents = fs::path(path) / "Contents";
    for (const char* sub : {"MacOS", "x86_64-linux", "aarch64-linux", "i386-linux", "x86_64-win", "arm64-win", "arm64x-win", "arm64ec-win"}) {
      fs::path d = contents / sub;
      if (!fs::is_directory(d, ec)) continue;
      for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) stamp(it->path());
    }
    stamp(contents / "Info.plist");
  }
  return best;
}

namespace {
// K-71: the scan worker's search (DiscoveryScope).
thread_local const std::atomic<bool>* tDiscoveryCancel = nullptr;
thread_local DiscoveryScope::Progress* tDiscoveryProgress = nullptr;
// Folders that hold no plugins and never end (Unix pseudo filesystems).
bool specialFolder(const std::string& real) {
#if defined(_WIN32)
  (void)real;
  return false;
#else
  for (const char* top : {"/proc", "/sys", "/dev"}) {
    const size_t n = std::strlen(top);
    if (real.compare(0, n, top) == 0 && (real.size() == n || real[n] == '/')) return true;
  }
  return false;
#endif
}
}  // namespace

DiscoveryScope::DiscoveryScope(const std::atomic<bool>* cancel, Progress progress) {
  tDiscoveryCancel = cancel;
  tDiscoveryProgress = progress ? new Progress(std::move(progress)) : nullptr;
}

DiscoveryScope::~DiscoveryScope() {
  delete tDiscoveryProgress;
  tDiscoveryProgress = nullptr;
  tDiscoveryCancel = nullptr;
}

std::vector<std::string> findEntries(const std::vector<std::string>& folders, const std::string& ext, bool directories, bool files) {
  std::vector<std::string> out;
  // K-71: every real folder once. A folder's real path is its parent's plus its name, or for a
  // symlink the link's target, so a loop (a link back up) is caught without a lookup per folder.
  std::unordered_set<std::string> visited;
  struct Dir {
    fs::path path, real;
    int depth;  // of its children (a search folder's children: 0)
  };
  std::vector<Dir> todo;
  size_t searched = 0;
  for (const auto& f : folders) {
    std::error_code ec;
    if (!fs::is_directory(f, ec)) continue;
    fs::path real = fs::canonical(f, ec);
    if (ec || specialFolder(real.generic_string()) || !visited.insert(real.generic_string()).second) continue;
    todo.push_back({fs::path(f), real, 0});
    while (!todo.empty()) {
      if (tDiscoveryCancel && tDiscoveryCancel->load()) return out;
      const Dir d = std::move(todo.back());
      todo.pop_back();
      if (tDiscoveryProgress && searched++ % 64 == 0) (*tDiscoveryProgress)(d.path.generic_string());
      ec.clear();
      for (fs::directory_iterator it(d.path, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) try {
        const fs::path& p = it->path();
        std::error_code e2;
        const bool dir = it->is_directory(e2);  // follows a symlink
        std::string e = p.extension().string();
        for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!dir) {
          if (e == ext && files) out.push_back(p.generic_string());
          continue;
        }
        if (e == ext && directories) out.push_back(p.generic_string());
        // Never look inside bundles (of any format).
        const bool bundle = e == ".vst3" || e == ".vst" || e == ".clap" || e == ".component" || e == ".lv2" || e == ".app" || e == ".bundle" || e == ".framework";
        if (bundle || d.depth > 6) continue;
        fs::path sub = it->is_symlink(e2) ? fs::canonical(p, e2) : d.real / p.filename();
        if (e2) continue;
        std::string key = sub.generic_string();
        if (specialFolder(key) || !visited.insert(std::move(key)).second) continue;
        todo.push_back({p, std::move(sub), d.depth + 1});
      } catch (const std::exception&) {
        // K-73: a name the system cannot spell in UTF-8 (Windows) is passed over, the search goes on.
      }
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

namespace {
// K-73: the scanner child's progress file (<out>.progress): one byte per plugin described.
std::string& progressFile() {
  static auto* f = new std::string;
  return *f;
}
}  // namespace

void scanProgress() {
  if (progressFile().empty()) return;
  std::ofstream(progressFile(), std::ios::binary | std::ios::app) << '+';
}

std::string scanProgressFile(const std::string& outFile) { return outFile + ".progress"; }

std::string peDllPrecheck(const std::string& path, const std::vector<std::string>& entryPoints, uint16_t hostMachine) {
  if (hostMachine == 0) {
#if defined(_M_ARM64) || defined(__aarch64__)
    hostMachine = 0xAA64;
#elif defined(_M_IX86) || defined(__i386__)
    hostMachine = 0x14C;
#else
    hostMachine = 0x8664;
#endif
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  // Little-endian fields read from the file; false past its end.
  auto read = [&](std::streamoff at, void* to, size_t n) {
    if (at < 0 || at + static_cast<std::streamoff>(n) > size) return false;
    f.clear();
    f.seekg(at);
    return static_cast<bool>(f.read(static_cast<char*>(to), static_cast<std::streamsize>(n)));
  };
  auto u16 = [&](std::streamoff at, uint32_t& v) {
    unsigned char b[2];
    if (!read(at, b, 2)) return false;
    v = static_cast<uint32_t>(b[0] | (b[1] << 8));
    return true;
  };
  auto u32 = [&](std::streamoff at, uint32_t& v) {
    unsigned char b[4];
    if (!read(at, b, 4)) return false;
    v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
  };
  const std::string notDll = "not a plugin (not a Windows DLL)";
  char mz[2];
  uint32_t peAt = 0, sig = 0, machine = 0, sections = 0, optSize = 0, magic = 0;
  if (!read(0, mz, 2) || mz[0] != 'M' || mz[1] != 'Z' || !u32(0x3C, peAt) || !u32(peAt, sig) || sig != 0x00004550) return notDll;
  if (!u16(peAt + 4, machine) || !u16(peAt + 6, sections) || !u16(peAt + 20, optSize)) return notDll;
  const bool host64 = hostMachine != 0x14C;
  if (host64 && machine == 0x14C) return "a 32-bit plugin (this Gridline loads 64-bit plugins only)";
  if (!host64 && (machine == 0x8664 || machine == 0xAA64)) return "a 64-bit plugin (this Gridline loads 32-bit plugins only)";
  const std::streamoff opt = static_cast<std::streamoff>(peAt) + 24;
  if (!u16(opt, magic) || (magic != 0x10B && magic != 0x20B)) return notDll;
  // The export directory (data directory 0).
  const std::streamoff dirs = opt + (magic == 0x20B ? 112 : 96);
  uint32_t dirCount = 0, expRva = 0, expSize = 0;
  if (!u32(opt + (magic == 0x20B ? 108 : 92), dirCount) || dirCount < 1 || !u32(dirs, expRva) || !u32(dirs + 4, expSize) || expRva == 0) return {};
  // Virtual addresses to file offsets through the section table.
  const std::streamoff table = opt + optSize;
  auto fileOffset = [&](uint32_t rva, std::streamoff& out) {
    for (uint32_t i = 0; i < sections && i < 96; ++i) {
      uint32_t vsize = 0, va = 0, rawSize = 0, raw = 0;
      const std::streamoff s = table + 40 * static_cast<std::streamoff>(i);
      if (!u32(s + 8, vsize) || !u32(s + 12, va) || !u32(s + 16, rawSize) || !u32(s + 20, raw)) return false;
      const uint32_t span = std::max(vsize, rawSize);
      if (rva >= va && rva - va < span) {
        out = static_cast<std::streamoff>(raw) + (rva - va);
        return true;
      }
    }
    return false;
  };
  std::streamoff exp = 0;
  uint32_t names = 0, namesRva = 0;
  if (!fileOffset(expRva, exp) || !u32(exp + 24, names) || !u32(exp + 32, namesRva) || names == 0) return {};  // packed: the scan decides
  std::streamoff nameTable = 0;
  if (!fileOffset(namesRva, nameTable)) return {};
  for (uint32_t i = 0; i < names && i < 65536; ++i) {
    uint32_t rva = 0;
    std::streamoff at = 0;
    if (!u32(nameTable + 4 * static_cast<std::streamoff>(i), rva) || !fileOffset(rva, at)) return {};
    char name[64] = {};
    const std::streamoff n = std::min<std::streamoff>(static_cast<std::streamoff>(sizeof name) - 1, size - at);
    if (n <= 0 || !read(at, name, static_cast<size_t>(n))) return {};
    name[sizeof name - 1] = '\0';
    for (const auto& e : entryPoints)
      if (e == name) return {};
  }
  std::string list;
  for (const auto& e : entryPoints) list += (list.empty() ? "" : " or ") + e;
  return "not a plugin (no " + list + " in it)";
}

int childMain(int argc, char** argv) {
  std::string format, entry, out, sandbox, prefsFile;
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
    } else if (std::strcmp(argv[i], "--plugin-prefs") == 0 && i + 1 < argc) {
      prefsFile = argv[++i];
    }
  }
  // K-13: the parent's plugin folders (read only), so a child searches what the parent does.
  if (!prefsFile.empty() && (!sandbox.empty() || !entry.empty())) loadPluginPrefsFile(prefsFile);
  if (!sandbox.empty()) {
    platform::quietCrashDialogs();
    return sandboxChildMain(sandbox);
  }
  if (entry.empty()) return -1;
  platform::quietCrashDialogs();
  ScanResult r;
  r.format = format;
  r.bundle = entry;
  if (!out.empty()) progressFile() = scanProgressFile(out);
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

ScanResult scanIsolated(const std::string& exe, const std::string& format, const std::string& entry, double timeoutSeconds,
                        const std::atomic<bool>* cancel) {
  ScanResult r;
  r.format = format;
  r.bundle = entry;
  const Format* fmt = findFormat(format);
  r.mtime = fmt ? fmt->mtime(entry) : 0;
  // K-73: what is plainly not a plugin for this computer (a 32-bit DLL in a 64-bit Gridline, a DLL
  // without a plugin entry point) is told without loading it in a child.
  if (fmt && fmt->precheck) {
    if (std::string why = fmt->precheck(entry); !why.empty()) {
      r.error = why;
      return r;
    }
  }
  static std::atomic<int> serial{0};
  fs::path tmp = fs::temp_directory_path() / ("gridline-plugin-scan-" + std::to_string(platform::processId()) + "-" + std::to_string(serial++) + ".json");
  const fs::path progress = scanProgressFile(tmp.string());
  std::vector<std::string> args = {exe, "--scan-plugin", format, entry, "--out", tmp.string()};
  if (const std::string pf = pluginPrefsFile(); !pf.empty()) args.insert(args.end(), {"--plugin-prefs", pf});  // K-13
  platform::Process child;
  if (!child.start(exe, args)) {
    r.error = "cannot start the scanner process";
    return r;
  }
  auto cleanUp = [&] {
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::remove(progress, ec);
  };
  // K-73: a killed child that does not end soon after (stuck in a driver) is left behind: a scan
  // never waits for it for good.
  auto stop = [&] {
    child.kill();
    child.wait(kKillWaitSeconds);
  };
  // Waited for in short steps so a quit (cancel) does not wait for a slow plugin. K-73: the timeout
  // counts from the child's last sign of progress (a shell of hundreds of plugins takes minutes, each
  // of them quick), and kScanLongestSeconds at most in all.
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  auto lastProgress = t0, lastLook = t0;
  std::uintmax_t progressSize = 0;
  bool done = false, cancelled = false;
  for (;;) {
    const auto now = Clock::now();
    if (timeoutSeconds >= 0 && now - lastLook >= std::chrono::milliseconds(250)) {
      lastLook = now;
      std::error_code ec;
      const std::uintmax_t n = fs::file_size(progress, ec);
      if (!ec && n > progressSize) {
        progressSize = n;
        lastProgress = now;
      }
    }
    double left = timeoutSeconds < 0 ? 1.0 : timeoutSeconds - std::chrono::duration<double>(now - lastProgress).count();
    if (timeoutSeconds >= 0 && std::chrono::duration<double>(now - t0).count() > std::max(timeoutSeconds, kScanLongestSeconds)) left = 0;
    if ((done = child.wait(std::clamp(left, 0.0, 0.05))) || left <= 0) break;
    if (cancel && cancel->load()) {
      cancelled = true;
      break;
    }
  }
  if (cancelled) {
    stop();
    r.error = "cancelled";
    cleanUp();
    return r;
  }
  if (!done) {
    stop();
    r.error = progressSize > 0 ? "timed out while loading (after " + std::to_string(progressSize) + " plugins)" : "timed out while loading";
    cleanUp();
    return r;
  }
  std::string text;
  {
    std::ifstream f(tmp, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
  }
  cleanUp();
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

void ScanCache::remove(const std::vector<std::pair<std::string, std::string>>& entries) {
  const std::set<std::pair<std::string, std::string>> drop(entries.begin(), entries.end());
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const ScanResult& e) { return drop.count({e.format, e.bundle}) > 0; }),
                 entries_.end());
}

std::vector<std::string> ScanCache::stale(const std::vector<std::string>& bundles, const std::string& format) const {
  const Format* f = findFormat(format);
  std::unordered_map<std::string, const ScanResult*> cached;  // K-71: one lookup per entry, not a search of the cache
  for (const auto& e : entries_)
    if (e.format == format) cached.emplace(e.bundle, &e);
  std::vector<std::string> out;
  for (const auto& b : bundles) {
    const auto it = cached.find(b);
    if (it == cached.end() || !f || it->second->mtime != f->mtime(b)) out.push_back(b);
  }
  return out;
}

}  // namespace gl::host

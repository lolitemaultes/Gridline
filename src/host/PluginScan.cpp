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
  if (!r.failed.empty()) {
    Json failed = Json::makeArray();
    for (const auto& f : r.failed) {
      Json jf = Json::makeObject();
      jf.object["uid"] = str(f.uid);
      jf.object["name"] = str(f.name);
      jf.object["error"] = str(f.error);
      failed.array.push_back(std::move(jf));
    }
    j.object["failed"] = std::move(failed);
  }
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
  if (const Json* fs = j.find("failed"); fs && fs->is(Json::Type::Array))
    for (const auto& jf : fs->array) {
      ScanFailure f{jf.text("uid"), jf.text("name"), jf.text("error")};
      if (!f.uid.empty() || !f.name.empty()) r.failed.push_back(std::move(f));
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
  stamp(fs::u8path(path));
  // Bundles: the binaries change on updates even when the folder does not.
  if (fs::is_directory(fs::u8path(path), ec)) {
    fs::path contents = fs::u8path(path) / "Contents";
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
    if (!fs::is_directory(fs::u8path(f), ec)) continue;
    fs::path real = fs::canonical(fs::u8path(f), ec);
    if (ec || specialFolder(real.generic_u8string()) || !visited.insert(real.generic_u8string()).second) continue;
    todo.push_back({fs::u8path(f), real, 0});
    while (!todo.empty()) {
      if (tDiscoveryCancel && tDiscoveryCancel->load()) return out;
      const Dir d = std::move(todo.back());
      todo.pop_back();
      if (tDiscoveryProgress && searched++ % 64 == 0) (*tDiscoveryProgress)(d.path.generic_u8string());
      ec.clear();
      for (fs::directory_iterator it(d.path, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) try {
        const fs::path& p = it->path();
        std::error_code e2;
        const bool dir = it->is_directory(e2);  // follows a symlink
        std::string e = p.extension().u8string();
        for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!dir) {
          if (e == ext && files) out.push_back(p.generic_u8string());
          continue;
        }
        if (e == ext && directories) out.push_back(p.generic_u8string());
        // Never look inside bundles (of any format).
        const bool bundle = e == ".vst3" || e == ".vst" || e == ".clap" || e == ".component" || e == ".lv2" || e == ".app" || e == ".bundle" || e == ".framework";
        if (bundle || d.depth > 6) continue;
        fs::path sub = it->is_symlink(e2) ? fs::canonical(p, e2) : d.real / p.filename();
        if (e2) continue;
        std::string key = sub.generic_u8string();
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
  const std::string s = platform::getEnv(var);  // K-267: UTF-8
  if (s.empty()) return;
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
  const std::string e = platform::getEnv(var);  // K-267: UTF-8
  if (e.empty()) return {};
  return (fs::u8path(e) / fs::u8path(sub)).u8string();
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
  auto canon = fs::canonical(fs::u8path(p), ec);
  return ec ? p : canon.u8string();
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
  std::ofstream(fs::u8path(progressFile()), std::ios::binary | std::ios::app) << '+';
  platform::pumpThreadMessages(0.02);  // K-267: work the plugin posted to this thread (Windows)
}

std::string scanProgressFile(const std::string& outFile) { return outFile + ".progress"; }

std::string peMachineMismatch(uint16_t machine, uint16_t hostMachine) {
  constexpr uint16_t kX86 = 0x14C, kX64 = 0x8664, kArm64 = 0xAA64;
  if (machine == hostMachine) return {};
  if (machine == kX86) return "a 32-bit plugin (this Gridline loads 64-bit plugins only)";
  if (hostMachine == kX86 && (machine == kX64 || machine == kArm64)) return "a 64-bit plugin (this Gridline loads 32-bit plugins only)";
  // An x64 or ARM64EC image (both carry the x64 machine) cannot load into a native ARM64 process.
  // An ARM64X image carries the ARM64 machine and loads into an x64 process on ARM computers: the
  // scan decides, as for any other kind.
  if (hostMachine == kArm64 && machine == kX64) return "an x64 plugin (this Gridline runs natively on ARM and loads ARM64 plugins only)";
  return {};
}

std::string peDllPrecheck(const std::string& path, const std::vector<std::string>& entryPoints, uint16_t hostMachine) {
  (void)entryPoints;  // K-267: the export table is not trusted (see the header)
  if (hostMachine == 0) {
#if defined(_M_ARM64) || defined(__aarch64__)
    hostMachine = 0xAA64;
#elif defined(_M_IX86) || defined(__i386__)
    hostMachine = 0x14C;
#else
    hostMachine = 0x8664;
#endif
  }
  std::ifstream f(fs::u8path(path), std::ios::binary);
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
  uint32_t peAt = 0, sig = 0, machine = 0, magic = 0;
  if (!read(0, mz, 2) || mz[0] != 'M' || mz[1] != 'Z' || !u32(0x3C, peAt) || !u32(peAt, sig) || sig != 0x00004550) return notDll;
  if (!u16(peAt + 4, machine)) return notDll;
  if (std::string why = peMachineMismatch(static_cast<uint16_t>(machine), hostMachine); !why.empty()) return why;
  if (!u16(static_cast<std::streamoff>(peAt) + 24, magic) || (magic != 0x10B && magic != 0x20B)) return notDll;
  return {};
}

void scanStep(const std::string& what) {
  if (progressFile().empty()) return;  // only in the scanner child
  std::fprintf(stderr, "scan: %s\n", what.c_str());
  std::fflush(stderr);
  platform::pumpThreadMessages(0.02);  // K-267: work the plugin posted to this thread (Windows)
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
    platform::initPluginThread();  // K-267: the plugin's thread, as Gridline's own interface thread
    return sandboxChildMain(sandbox);
  }
  if (entry.empty()) return -1;
  platform::quietCrashDialogs();
  // K-267: plugins load on a thread with OLE (a single-threaded apartment) and a message loop
  // (scanStep dispatches the thread's waiting messages), as on the interface thread of a host.
  const bool ole = platform::initPluginThread();
  ScanResult r;
  r.format = format;
  r.bundle = entry;
  if (!out.empty()) progressFile() = scanProgressFile(out);
  scanStep("start: " + format + " " + entry + (ole ? "" : " (the thread has no OLE)"));
  if (const Format* f = findFormat(format))
    r = f->scanInProcess(entry);
  else
    r.error = "unsupported plugin format " + format;
  scanStep("described " + std::to_string(r.classes.size()) + " plugins" + (r.error.empty() ? "" : " (" + r.error + ")") + ", writing the result");
  std::string text = jsonStringify(toJson(r), 0);
  if (out.empty()) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
  } else {
    // K-267: written under another name and renamed: a result file the parent sees is complete, so
    // it can take it even when the plugin then hangs or crashes while the process ends.
    const fs::path final = fs::u8path(out), part = fs::u8path(out + ".part");
    {
      std::ofstream f(part, std::ios::binary);
      f << text;
      f.flush();
      if (!f) platform::exitNow(3);
    }
    std::error_code ec;
    fs::rename(part, final, ec);
    if (ec) platform::exitNow(3);
  }
  scanStep("result written");
  // Leave without static destructors or module exit calls. K-267: on Windows also without the DLL
  // unload notifications a normal exit sends every loaded plugin: plugins that hang or crash while
  // unloading must not fail the scan.
  platform::exitNow(r.ok ? 0 : 2);
}

namespace {
std::uint64_t envMegabytes(const char* name) {
  const char* e = std::getenv(name);
  if (!e || !*e) return 0;
  const double mb = std::atof(e);
  return mb > 0 ? static_cast<std::uint64_t>(mb * 1048576.0) : 0;
}
constexpr std::uint64_t kGiB = 1ull << 30, kMiB = 1ull << 20;
// The tree holds real memory (512 MB, or a quarter of the cap when that is less): not a bystander.
std::uint64_t lowMemoryFloor(std::uint64_t cap) { return std::min<std::uint64_t>(512ull << 20, cap / 4); }
}  // namespace

std::uint64_t scanMemoryCap(std::uint64_t physicalBytes) {
  if (const std::uint64_t e = envMegabytes("GRIDLINE_SCAN_MEMORY_MB")) return e;
  if (physicalBytes == 0) return 4 * kGiB;
  return std::clamp<std::uint64_t>(physicalBytes / 4, 2 * kGiB, 8 * kGiB);
}

std::uint64_t scanMemoryReserve(std::uint64_t physicalBytes) {
  if (const std::uint64_t e = envMegabytes("GRIDLINE_SCAN_RESERVE_MB")) return e;
  return std::clamp<std::uint64_t>(physicalBytes / 10, 512 * kMiB, 2 * kGiB);
}

bool scanMemoryLow(std::uint64_t treeBytes, std::uint64_t availableBytes, std::uint64_t physicalBytes) {
  if (availableBytes == 0) return false;  // the system does not say: the cap alone
  return treeBytes >= lowMemoryFloor(scanMemoryCap(physicalBytes)) && availableBytes < scanMemoryReserve(physicalBytes);
}

std::string scanMemoryError(std::uint64_t treeBytes, bool low) {
  char buf[160];
  if (low)
    std::snprintf(buf, sizeof buf, "used too much memory while loading (%.1f GB, the computer was running out of memory)",
                  static_cast<double>(treeBytes) / static_cast<double>(kGiB));
  else
    std::snprintf(buf, sizeof buf, "used too much memory while loading (more than %.1f GB)", static_cast<double>(treeBytes) / static_cast<double>(kGiB));
  return buf;
}

bool scanRetryable(const ScanReport& r) { return r.outcome == ScanReport::Outcome::TimedOut || r.outcome == ScanReport::Outcome::Crashed; }

ScanTimeouts scanTimeouts(double baseSeconds, int attempt) {
  ScanTimeouts t;
  const double base = std::max(0.1, baseSeconds);
  // The first plugin: loading the module and its first start (licence checks, content unpacked,
  // databases built) take longer than the next plugin of a shell. A retry (alone) waits twice as long.
  const double factor = attempt > 0 ? 2.0 : 1.0;
  t.first = 2 * base * factor;
  t.each = base * factor;
  t.longest = std::max(kScanLongestSeconds, t.first);
  return t;
}

bool scanTimedOut(const ScanTimeouts& t, double sinceStart, double sinceProgress, int progress) {
  if (sinceStart > t.longest) return true;
  return progress > 0 ? sinceProgress > t.each : sinceStart > t.first;
}

namespace {
// The last lines of a child's stderr file (its steps), at most `maxBytes`.
std::string tailOf(const fs::path& file, size_t maxBytes) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return {};
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  const std::streamoff from = std::max<std::streamoff>(0, size - static_cast<std::streamoff>(maxBytes));
  f.seekg(from);
  std::string s(static_cast<size_t>(size - from), '\0');
  f.read(s.data(), static_cast<std::streamsize>(s.size()));
  s.resize(static_cast<size_t>(f.gcount()));
  if (from > 0)
    if (const size_t nl = s.find('\n'); nl != std::string::npos) s.erase(0, nl + 1);  // whole lines only
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}
}  // namespace

ScanResult scanIsolated(const std::string& exe, const std::string& format, const std::string& entry, double timeoutSeconds,
                        const std::atomic<bool>* cancel, ScanReport* report, int attempt) {
  ScanReport rep;
  ScanResult r;
  r.format = format;
  r.bundle = entry;
  const Format* fmt = findFormat(format);
  r.mtime = fmt ? fmt->mtime(entry) : 0;
  auto done = [&](ScanResult res, ScanReport::Outcome o) {
    rep.outcome = o;
    if (report) *report = rep;
    return res;
  };
  // K-73: what is plainly not a plugin for this computer (a 32-bit DLL in a 64-bit Gridline) is told
  // without loading it in a child.
  if (fmt && fmt->precheck) {
    if (std::string why = fmt->precheck(entry); !why.empty()) {
      r.error = why;
      return done(std::move(r), ScanReport::Outcome::NotForThisComputer);
    }
  }
  static std::atomic<int> serial{0};
  // K-267: paths are UTF-8 everywhere (a user folder with letters outside the system's code page).
  const fs::path tmp = fs::temp_directory_path() / fs::u8path("gridline-plugin-scan-" + std::to_string(platform::processId()) + "-" +
                                                              std::to_string(serial++) + ".json");
  const std::string tmpName = tmp.u8string();
  const fs::path progress = fs::u8path(scanProgressFile(tmpName)), part = fs::u8path(tmpName + ".part"), errLog = fs::u8path(tmpName + ".log");
  std::vector<std::string> args = {exe, "--scan-plugin", format, entry, "--out", tmpName};
  if (const std::string pf = pluginPrefsFile(); !pf.empty()) args.insert(args.end(), {"--plugin-prefs", pf});  // K-13
  // K-154: the child and whatever it starts (a bridge's host process) form one group / job: its
  // memory is watched as one, and it is ended as one.
  const std::uint64_t physical = platform::physicalMemoryBytes();
  const std::uint64_t cap = scanMemoryCap(physical);
  platform::Process child;
  // Windows: a hard stop well past the watched cap. K-267: it counts committed memory, which plugins
  // reserve far beyond what they use (sample streaming, graphics drivers); a failed allocation crashes
  // the plugin, which looked like a broken plugin. The watched cap below stops a runaway plugin first.
  child.setTreeMemoryLimit(2 * cap);
  child.redirectOutput({}, errLog.u8string());  // K-267: the child's steps, kept for the scan log
  const auto t0 = std::chrono::steady_clock::now();
  auto cleanUp = [&] {
    child.killTree();  // helpers the plugin left running
    rep.childLog = tailOf(errLog, 2048);
    std::error_code ec;
    for (const fs::path& p : {tmp, progress, part, errLog}) fs::remove(p, ec);
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  };
  if (!child.start(exe, args, platform::Process::kTree)) {
    r.error = "cannot start the scanner process";
    cleanUp();
    return done(std::move(r), ScanReport::Outcome::Failed);
  }
  // K-73: a killed child that does not end soon after (stuck in a driver) is left behind: a scan
  // never waits for it for good.
  auto stop = [&] {
    child.kill();
    child.wait(kKillWaitSeconds);
  };
  auto lastMemoryLook = std::chrono::steady_clock::time_point{};
  std::uint64_t memoryUsed = 0;
  bool tooMuchMemory = false, lowMemory = false;
  // Waited for in short steps so a quit (cancel) does not wait for a slow plugin. K-73 / K-267: the
  // timeouts count per plugin (scanTimeouts): the first plugin may take longer than the next one of
  // a shell, which shows its progress plugin by plugin.
  using Clock = std::chrono::steady_clock;
  const ScanTimeouts limits = scanTimeouts(timeoutSeconds, attempt);
  auto lastProgress = t0, lastLook = t0;
  std::uintmax_t progressSize = 0;
  bool exited = false, cancelled = false, timedOut = false, resultFirst = false;
  Clock::time_point resultSeen{};
  for (;;) {
    const auto now = Clock::now();
    if (now - lastLook >= std::chrono::milliseconds(250)) {
      lastLook = now;
      std::error_code ec;
      const std::uintmax_t n = fs::file_size(progress, ec);
      if (!ec && n > progressSize) {
        progressSize = n;
        lastProgress = now;
      }
      // K-267: the complete result is there (written, then renamed) while the child still runs: the
      // plugin hangs on its way out (unloading). Given a moment to end, then ended; the result counts.
      if (!resultFirst && fs::exists(tmp, ec)) {
        resultFirst = true;
        resultSeen = now;
      }
    }
    // K-154: four looks a second at the tree's memory (a runaway plugin can take a gigabyte a second;
    // a look reads each process's status once, a few milliseconds at most).
    if (now - lastMemoryLook >= std::chrono::milliseconds(250)) {
      lastMemoryLook = now;
      memoryUsed = child.treeMemoryBytes();
      rep.peakMemory = std::max(rep.peakMemory, memoryUsed);
      if (memoryUsed > cap) {
        tooMuchMemory = true;
        break;
      }
      if (memoryUsed >= lowMemoryFloor(cap) && scanMemoryLow(memoryUsed, platform::availableMemoryBytes(), physical)) {
        tooMuchMemory = lowMemory = true;
        break;
      }
    }
    if ((exited = child.wait(0.05))) break;
    if (resultFirst && now - resultSeen > std::chrono::seconds(3)) break;
    if (timeoutSeconds >= 0) {
      const double sinceStart = std::chrono::duration<double>(now - t0).count();
      const double sinceProgress = std::chrono::duration<double>(now - lastProgress).count();
      if (scanTimedOut(limits, sinceStart, sinceProgress, static_cast<int>(progressSize))) {
        timedOut = true;
        break;
      }
    }
    if (cancel && cancel->load()) {
      cancelled = true;
      break;
    }
  }
  rep.progress = static_cast<int>(progressSize);
  if (cancelled) {
    stop();
    r.error = "cancelled";
    cleanUp();
    return done(std::move(r), ScanReport::Outcome::Cancelled);
  }
  if (tooMuchMemory) {
    stop();
    rep.exit = "stopped by Gridline";
    r.error = lowMemory ? scanMemoryError(memoryUsed, true) : scanMemoryError(cap, false);
    cleanUp();
    return done(std::move(r), ScanReport::Outcome::TooMuchMemory);
  }
  if (!exited) {
    stop();
    rep.exit = "stopped by Gridline";
    if (resultFirst) rep.note = "the result was written, then the plugin did not let the process end (ended by Gridline)";
  } else {
    rep.exit = child.status().describe();
  }
  std::string text;
  bool tooLarge = false;
  {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(tmp, ec);
    if (!ec && size > kScanResultMaxBytes) {
      tooLarge = true;  // K-154: never read into memory
    } else if (!ec) {
      std::ifstream f(tmp, std::ios::binary);
      std::stringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    }
  }
  const bool limitHit = child.treeMemoryLimitHit();
  cleanUp();
  if (tooLarge) {
    r.error = "described itself at a size no plugin has (the scan result was too large)";
    return done(std::move(r), ScanReport::Outcome::Failed);
  }
  Json j;
  ScanResult parsed;
  const bool haveResult = !text.empty() && jsonParse(text, j) && fromJson(j, parsed);
  // K-267: a complete result counts however the child then ended: a plugin that crashes or hangs while
  // its process ends (Windows sends every loaded DLL an unload notification) still loaded and was described.
  if (haveResult) {
    if (exited && child.status().crashed) rep.note = "the result was written, then the process " + child.status().describe();
    parsed.format = format;
    parsed.bundle = entry;
    parsed.mtime = r.mtime;
    const ScanReport::Outcome o = parsed.ok ? ScanReport::Outcome::Listed : ScanReport::Outcome::Failed;
    return done(std::move(parsed), o);
  }
  if (limitHit) {  // K-154: Windows: the job's limit stopped it
    r.error = scanMemoryError(2 * cap, false);
    return done(std::move(r), ScanReport::Outcome::TooMuchMemory);
  }
  if (!exited || timedOut) {
    r.error = progressSize > 0 ? "timed out while loading (after " + std::to_string(progressSize) + " plugins)" : "timed out while loading";
    return done(std::move(r), ScanReport::Outcome::TimedOut);
  }
  if (child.status().crashed) {
    r.error = "crashed while loading (" + child.status().describe().substr(9);
    return done(std::move(r), ScanReport::Outcome::Crashed);
  }
  r.error = "scanner exited with code " + std::to_string(child.status().code);
  return done(std::move(r), ScanReport::Outcome::Failed);
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

ScanResult* ScanCache::slot(const std::string& bundle, const std::string& format) {
  for (auto& e : entries_)
    if (e.bundle == bundle && e.format == format) return &e;
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

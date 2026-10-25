// Gridline: the Serum preset library of Plasma's preset browser: the
// Serum 2 / Serum preset folders found on this system (plasma_import_dsp.cpp
// serumRootCandidates) plus the folders the user added, indexed on a
// background thread. The index is kept in a cache file so the browser lists
// the library at once on the next start; a rescan replaces it when files
// change.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>
#include "../../core/Json.h"
#include "../../core/Project.h"
#include "plasma.h"

namespace gl::packs::plasma {

namespace fs = std::filesystem;

namespace {

std::string lowerExt(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

// The folder holding a root's presets: "<root>/Presets" when present.
fs::path presetDir(const std::string& root) {
  std::error_code ec;
  const fs::path p = fs::u8path(root) / "Presets";
  return fs::is_directory(p, ec) ? p : fs::u8path(root);
}

struct Library {
  std::mutex mu;
  std::shared_ptr<const LibrarySnapshot> snap = std::make_shared<LibrarySnapshot>();
  std::thread worker;
  std::atomic<bool> busy{false}, cancel{false};
  std::chrono::steady_clock::time_point lastScan{};
  bool cacheRead = false;
  ~Library() {
    cancel = true;
    if (worker.joinable()) worker.join();
  }
};
Library& lib() {
  static Library l;
  return l;
}

void publish(std::vector<std::string> roots, std::vector<LibraryPreset> presets, bool indexing) {
  Library& l = lib();
  auto s = std::make_shared<LibrarySnapshot>();
  s->roots = std::move(roots);
  s->presets = std::move(presets);
  s->indexing = indexing;
  std::lock_guard<std::mutex> lk(l.mu);
  s->generation = l.snap->generation + 1;
  l.snap = s;
}

bool readCache(const std::string& file, std::vector<std::string>& roots, std::vector<LibraryPreset>& out, std::vector<std::string>& extra) {
  std::error_code ec;
  if (file.empty() || !fs::is_regular_file(fs::u8path(file), ec)) return false;
  std::ifstream in(fs::u8path(file), std::ios::binary);
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Json j;
  if (!jsonParse(text, j) || !j.is(Json::Type::Object) || j.num("version", 0) != 1) return false;
  if (const Json* r = j.find("roots"); r && r->is(Json::Type::Array))
    for (const auto& v : r->array)
      if (v.is(Json::Type::String)) roots.push_back(v.str);
  if (const Json* e = j.find("extraFolders"); e && e->is(Json::Type::Array))
    for (const auto& v : e->array)
      if (v.is(Json::Type::String)) extra.push_back(v.str);
  if (const Json* p = j.find("presets"); p && p->is(Json::Type::Array))
    for (const auto& v : p->array) {
      if (!v.is(Json::Type::Array) || v.array.size() < 5) continue;
      LibraryPreset e;
      e.file = v.array[0].str;
      e.name = v.array[1].str;
      e.bank = v.array[2].str;
      e.root = v.array[3].str;
      e.serum2 = v.array[4].is(Json::Type::Number) && v.array[4].number != 0;
      if (!e.file.empty()) out.push_back(std::move(e));
    }
  return true;
}

void writeCache(const std::string& file, const std::vector<std::string>& roots, const std::vector<LibraryPreset>& presets) {
  if (file.empty()) return;
  Json j = Json::makeObject();
  j.object["version"] = Json::makeNumber(1);
  Json r = Json::makeArray(), e = Json::makeArray(), p = Json::makeArray();
  for (const auto& s : roots) r.array.push_back(Json::makeString(s));
  for (const auto& s : extraSerumFolders()) e.array.push_back(Json::makeString(s));
  for (const auto& x : presets) {
    Json row = Json::makeArray();
    row.array.push_back(Json::makeString(x.file));
    row.array.push_back(Json::makeString(x.name));
    row.array.push_back(Json::makeString(x.bank));
    row.array.push_back(Json::makeString(x.root));
    row.array.push_back(Json::makeNumber(x.serum2 ? 1 : 0));
    p.array.push_back(std::move(row));
  }
  j.object["roots"] = r;
  j.object["extraFolders"] = e;
  j.object["presets"] = p;
  std::error_code ec;
  fs::create_directories(fs::u8path(file).parent_path(), ec);
  writeFileAtomic(file, jsonCompact(j) + "\n");
}

}  // namespace

std::vector<LibraryPreset> scanLibrary(const std::vector<std::string>& roots, const std::atomic<bool>* cancel) {
  std::vector<LibraryPreset> out;
  std::set<std::string> seen;
  for (const auto& root : roots) {
    const fs::path base = presetDir(root);
    std::error_code ec;
    if (!fs::is_directory(base, ec)) continue;
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
      if (cancel && *cancel) return out;
      if (it.depth() > 8) {
        it.disable_recursion_pending();
        continue;
      }
      std::error_code fe;
      if (!it->is_regular_file(fe)) continue;
      const std::string ext = lowerExt(it->path());
      if (ext != ".serumpreset" && ext != ".fxp") continue;
      const std::string file = it->path().u8string();
      if (!seen.insert(file).second) continue;
      LibraryPreset e;
      e.file = file;
      e.name = it->path().stem().u8string();
      const std::string rel = it->path().parent_path().lexically_relative(base).generic_u8string();
      e.bank = rel == "." ? "" : rel;
      e.root = root;
      e.serum2 = ext == ".serumpreset";
      out.push_back(std::move(e));
      if (out.size() >= 100000) return out;
    }
  }
  std::sort(out.begin(), out.end(), [](const LibraryPreset& a, const LibraryPreset& b) {
    if (a.root != b.root) return a.root < b.root;
    if (a.bank != b.bank) return a.bank < b.bank;
    return a.name < b.name;
  });
  return out;
}

void libraryRefresh(const std::string& cacheFile, bool force) {
  Library& l = lib();
  if (!l.cacheRead) {  // the previous index first: the browser lists it while the rescan runs
    l.cacheRead = true;
    std::vector<std::string> roots, extra;
    std::vector<LibraryPreset> presets;
    if (readCache(cacheFile, roots, presets, extra)) {
      if (extraSerumFolders().empty() && !extra.empty()) setExtraSerumFolders(extra);
      publish(roots, std::move(presets), true);
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (l.busy || (!force && l.lastScan.time_since_epoch().count() != 0 && now - l.lastScan < std::chrono::seconds(15))) return;
  if (l.worker.joinable()) l.worker.join();
  l.busy = true;
  l.lastScan = now;
  {
    std::lock_guard<std::mutex> lk(l.mu);
    if (!l.snap->indexing) {
      auto s = std::make_shared<LibrarySnapshot>(*l.snap);
      s->indexing = true;
      l.snap = s;
    }
  }
  l.worker = std::thread([cacheFile] {
    Library& lb = lib();
    std::vector<std::string> roots = defaultSerumRoots();
    std::vector<LibraryPreset> presets = scanLibrary(roots, &lb.cancel);
    if (!lb.cancel) {
      bool same = false;
      {
        std::lock_guard<std::mutex> lk(lb.mu);
        const auto& old = *lb.snap;
        same = old.roots == roots && old.presets.size() == presets.size() &&
               std::equal(presets.begin(), presets.end(), old.presets.begin(), [](const LibraryPreset& a, const LibraryPreset& b) { return a.file == b.file; });
      }
      if (!same) writeCache(cacheFile, roots, presets);
      publish(std::move(roots), std::move(presets), false);
    }
    lb.busy = false;
  });
}

std::shared_ptr<const LibrarySnapshot> librarySnapshot() {
  Library& l = lib();
  std::lock_guard<std::mutex> lk(l.mu);
  return l.snap;
}

void librarySaveFolders(const std::string& cacheFile) {
  auto s = librarySnapshot();
  writeCache(cacheFile, s->roots, s->presets);
}

}  // namespace gl::packs::plasma

// Gridline - K-321: the Track Receipt's file hashes (see ReceiptHashes.h).
#include "ReceiptHashes.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "Json.h"
#include "Platform.h"
#include "SafeFs.h"
#include "Sha256.h"

namespace gl::receipt {

namespace fs = std::filesystem;

std::string hashFromName(const std::string& path) {
  const std::string stem = safefs::toPath(path).stem().u8string();
  if (stem.size() != 64) return "";
  for (char c : stem)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return "";
  std::string h = stem;
  for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return h;
}

HashCache::HashCache(std::string file) : file_(std::move(file)) {}

void HashCache::load() {
  if (loaded_) return;
  loaded_ = true;
  if (file_.empty()) return;
  std::ifstream f(safefs::toPath(file_), std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || j.type != Json::Type::Object) return;
  const Json* files = j.find("files");
  if (!files || files->type != Json::Type::Object) return;
  for (const auto& [path, e] : files->object) {
    if (e.type != Json::Type::Object) continue;
    Entry x;
    x.size = jsonInt<uint64_t>(e.num("size", 0));
    x.mtime = std::strtoll(e.text("mtime").c_str(), nullptr, 10);  // (text: a double would round it)
    x.hash = e.text("sha256");
    if (x.hash.size() == 64) entries_[path] = x;
  }
}

std::string HashCache::get(const std::string& path) {
  if (std::string h = hashFromName(path); !h.empty()) return h;
  std::error_code ec;
  const fs::path p = safefs::toPath(path);
  const uint64_t size = fs::file_size(p, ec);
  if (ec) return "";
  const auto t = fs::last_write_time(p, ec);
  if (ec) return "";
  const int64_t mtime = static_cast<int64_t>(t.time_since_epoch().count());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    load();
    auto it = entries_.find(path);
    if (it != entries_.end() && it->second.size == size && it->second.mtime == mtime) return it->second.hash;
  }
  const std::string h = sha256File(path);
  if (h.empty()) return "";
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[path] = Entry{size, mtime, h};
  dirty_ = true;
  ++hashed_;
  return h;
}

bool HashCache::save() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!dirty_ || file_.empty()) return true;
  Json files = Json::makeObject();
  for (const auto& [path, e] : entries_) {
    Json o = Json::makeObject();
    o.object["size"] = Json::makeNumber(static_cast<double>(e.size));
    o.object["mtime"] = Json::makeString(std::to_string(e.mtime));
    o.object["sha256"] = Json::makeString(e.hash);
    files.object[path] = o;
  }
  Json j = Json::makeObject();
  j.object["v"] = Json::makeNumber(1);
  j.object["files"] = files;
  std::error_code ec;
  fs::create_directories(safefs::toPath(file_).parent_path(), ec);
  const std::string tmp = file_ + ".tmp";
  {
    std::ofstream f(safefs::toPath(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << jsonCompact(j) << "\n";
    if (!f) return false;
  }
  if (!platform::replaceFile(tmp, file_)) return false;
  dirty_ = false;
  return true;
}

HashCache& HashCache::shared() {
  static HashCache c((fs::u8path(platform::configRoot()) / "receipt-hashes.json").u8string());
  return c;
}

}  // namespace gl::receipt

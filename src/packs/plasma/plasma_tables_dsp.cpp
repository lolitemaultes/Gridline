// Gridline: every wavetable Plasma can play, for the layer picker and the Library: the factory
// tables, the user's own table folders, and the tables of the installed wavetable library
// (read where they are at runtime, never copied). A file is listed only when it is a WAV the
// engine reads (loadTableFile); the header is checked here so the scan stays quick.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include "plasma.h"

namespace gl::packs::plasma {

namespace fs = std::filesystem;

namespace {

bool isWavName(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return e == ".wav";
}

uint32_t le32(const unsigned char* b) { return b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24); }
uint16_t le16(const unsigned char* b) { return static_cast<uint16_t>(b[0] | (b[1] << 8)); }

}  // namespace

// The WAV's header: RIFF / WAVE, a fmt chunk the reader knows (PCM 8..32 bit or float) and a data
// chunk with at least one sample.
bool tableFileReadable(const std::string& path) {
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) return false;
  unsigned char h[12];
  if (!in.read(reinterpret_cast<char*>(h), 12)) return false;
  if (std::string(reinterpret_cast<char*>(h), 4) != "RIFF" || std::string(reinterpret_cast<char*>(h) + 8, 4) != "WAVE") return false;
  bool fmt = false;
  for (int guard = 0; guard < 64; ++guard) {
    unsigned char ch[8];
    if (!in.read(reinterpret_cast<char*>(ch), 8)) return false;
    const std::string id(reinterpret_cast<char*>(ch), 4);
    const uint32_t sz = le32(ch + 4);
    if (id == "fmt ") {
      unsigned char f[16];
      if (sz < 16 || !in.read(reinterpret_cast<char*>(f), 16)) return false;
      const uint16_t tag = le16(f), chans = le16(f + 2), bits = le16(f + 14);
      const bool pcm = tag == 1 || tag == 0xFFFE, flt = tag == 3 || tag == 0xFFFE;
      if (chans < 1 || !((pcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (flt && bits == 32))) return false;
      fmt = true;
      in.seekg(static_cast<std::streamoff>(sz - 16 + (sz & 1)), std::ios::cur);
    } else if (id == "data") {
      return fmt && sz > 0;
    } else {
      in.seekg(static_cast<std::streamoff>(sz + (sz & 1)), std::ios::cur);
    }
    if (!in) return false;
  }
  return false;
}

std::string userTableDir(const std::string& configDir) { return (fs::u8path(configDir) / "plasma-tables").u8string(); }

std::vector<std::string> libraryTableDirs(const std::vector<std::string>& roots) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& r : roots) {
    const fs::path t = fs::u8path(r) / "Tables";
    if (fs::is_directory(t, ec)) out.push_back(t.u8string());
  }
  return out;
}

TableCatalog buildTableCatalog(const std::vector<std::string>& userDirs, const std::vector<std::string>& libraryDirs) {
  TableCatalog cat;
  const auto& fn = factoryTableNames();
  for (int i = 0; i < static_cast<int>(fn.size()); ++i) cat.entries.push_back({fn[static_cast<size_t>(i)], "Factory", "", i + 1});
  auto scanDir = [&](const std::string& dir, const std::string& top) {
    std::error_code ec;
    const fs::path base = fs::u8path(dir);
    if (!fs::is_directory(base, ec)) return;
    std::vector<TableEntry> found;
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      if (it.depth() > 6) {
        it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file(ec) || !isWavName(it->path())) continue;
      const std::string p = it->path().u8string();
      if (!tableFileReadable(p)) continue;
      const fs::path rel = it->path().parent_path().lexically_relative(base);
      std::string group = top;
      if (!rel.empty() && rel != ".") group += ": " + rel.generic_u8string();
      found.push_back({it->path().stem().u8string(), group, "file:" + p, 0});
    }
    std::sort(found.begin(), found.end(), [](const TableEntry& a, const TableEntry& b) {
      if (a.group != b.group) return naturalCompare(a.group, b.group) < 0;
      return naturalCompare(a.name, b.name) < 0;
    });
    cat.entries.insert(cat.entries.end(), found.begin(), found.end());
  };
  for (const auto& d : userDirs) scanDir(d, "My tables");
  for (const auto& d : libraryDirs) scanDir(d, "Library");
  for (const auto& e : cat.entries) {
    if (cat.groups.empty() || cat.groups.back().name != e.group) cat.groups.push_back({e.group, 0});
    ++cat.groups.back().count;
  }
  return cat;
}

}  // namespace gl::packs::plasma

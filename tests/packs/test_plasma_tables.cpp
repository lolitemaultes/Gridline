// Gridline: every wavetable Plasma can play is listed (the layer picker and the Library share
// the catalog): the factory tables, the user's table folder and the installed library's Tables
// folders by group with their sizes. The count listed equals the count the engine loads: every
// listed file loads with loadTableFile, and the files it would not read are left out.
// Synthetic folders only.
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../../src/core/WavWriter.h"
#include "../../src/packs/plasma/plasma.h"
#include "../Check.h"

namespace pl = gl::packs::plasma;
namespace fs = std::filesystem;

namespace {

void wav(const fs::path& p, int frames, int bits) {
  fs::create_directories(p.parent_path());
  gl::AudioBuffer b(1, static_cast<size_t>(2048 * frames));
  for (size_t i = 0; i < b.frames; ++i) b.data[i] = 0.5f * static_cast<float>(std::sin(i * 6.2831853 / 2048.0 * (1 + static_cast<double>(i / 2048))));
  gl::writeWav(p.u8string(), b, 44100, bits);
}

void junk(const fs::path& p, const std::string& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << bytes;
}

}  // namespace

int main() {
  const fs::path base = fs::temp_directory_path() / ("gl-plasma-tables-" + std::to_string(static_cast<long>(std::rand())));
  fs::remove_all(base);
  const fs::path user = base / "user", root = base / "root", tables = root / "Tables";
  wav(user / "Mine A.wav", 4, 16);
  wav(user / "deep" / "Mine B.WAV", 1, 24);
  wav(tables / "Analog" / "Saw stack.wav", 8, 16);
  wav(tables / "Analog" / "Pulse 2.wav", 2, 16);
  wav(tables / "Analog" / "Pulse 10.wav", 2, 16);
  wav(tables / "Digital" / "Grit.wav", 16, 24);
  wav(tables / "Top.wav", 1, 16);
  junk(tables / "Digital" / "Broken.wav", "not a wave file at all");
  junk(tables / "Digital" / "Empty.wav", "");
  junk(tables / "Digital" / "notes.txt", "a text file");
  junk(tables / "Analog" / "Half.wav", std::string("RIFF\x10\x00\x00\x00WAVEfmt ", 16));

  const auto dirs = pl::libraryTableDirs({root.u8string(), (base / "missing").u8string()});
  expect(dirs.size() == 1, "only roots with a Tables folder");
  const pl::TableCatalog cat = pl::buildTableCatalog({user.u8string()}, dirs);
  const int factory = static_cast<int>(pl::factoryTableNames().size());
  expect(factory > 0, "factory tables exist");
  expect(static_cast<int>(cat.entries.size()) == factory + 7, "factory + every readable file");

  // the count listed = the count the engine loads
  int loads = 0;
  for (const auto& e : cat.entries) {
    if (e.factory > 0) loads += pl::factoryTable(e.factory - 1) != nullptr;
    else loads += e.ref.rfind("file:", 0) == 0 && pl::loadTableFile(e.ref.substr(5)) != nullptr;
  }
  expect(loads == static_cast<int>(cat.entries.size()), "every listed table loads");
  int engineFiles = 0;  // and nothing the engine reads is missing
  for (const auto& d : {user, tables})
    for (const auto& f : fs::recursive_directory_iterator(d))
      if (f.is_regular_file() && pl::loadTableFile(f.path().u8string())) ++engineFiles;
  expect(factory + engineFiles == static_cast<int>(cat.entries.size()), "the catalog lists every file the engine reads");

  // groups with their sizes, in entry order
  int sum = 0;
  for (const auto& g : cat.groups) sum += g.count;
  expect(sum == static_cast<int>(cat.entries.size()), "group sizes add up");
  auto group = [&](const std::string& n) {
    for (const auto& g : cat.groups)
      if (g.name == n) return g.count;
    return -1;
  };
  expect(group("Factory") == factory, "Factory group");
  expect(group("My tables") == 1 && group("My tables: deep") == 1, "user folders by sub-folder");
  expect(group("Library") == 1 && group("Library: Analog") == 3 && group("Library: Digital") == 1, "library by folder");
  // natural order inside a group: Pulse 2 before Pulse 10
  int p2 = -1, p10 = -1;
  for (size_t i = 0; i < cat.entries.size(); ++i) {
    if (cat.entries[i].name == "Pulse 2") p2 = static_cast<int>(i);
    if (cat.entries[i].name == "Pulse 10") p10 = static_cast<int>(i);
  }
  expect(p2 >= 0 && p10 > p2, "natural name order");
  expect(!pl::tableFileReadable((tables / "Digital" / "Broken.wav").u8string()), "a broken file is not readable");
  expect(pl::tableFileReadable((tables / "Top.wav").u8string()), "a WAV is readable");

  // no user folder, no library: the factory tables alone
  const pl::TableCatalog f0 = pl::buildTableCatalog({(base / "none").u8string()}, {});
  expect(static_cast<int>(f0.entries.size()) == factory && f0.groups.size() == 1, "factory only");
  fs::remove_all(base);
  return finish();
}

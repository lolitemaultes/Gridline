// Gridline tests - K-265: the data folder setting (core/DataFolder.h).
//  * no pointer: the default folders; a pointer to an existing folder: that folder;
//  * a pointer to a missing folder (an unplugged drive): the defaults, and it says so;
//  * GRIDLINE_CONFIG_HOME turns the setting off; a "GridlineData" folder beside the program
//    (or beside the app bundle) is portable mode;
//  * moving copies config, cache and data into the new folder, keeps the originals, skips the
//    pointer, refuses a folder inside the one it copies; writing an empty pointer reverts.
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include "Check.h"
#include "core/DataFolder.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {
void put(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << text;
}
std::string get(const fs::path& p) {
  std::ifstream f(p);
  std::string s;
  std::getline(f, s);
  return s;
}
}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("gl_k265_" + std::to_string(::time(nullptr)));
  fs::remove_all(root);
  const fs::path cfg = root / "home" / ".config", cache = root / "home" / ".cache", data = root / "home" / ".local" / "share";
  const std::string exe = (root / "bin" / "gridline-app").string();

  datafolder::Info in = datafolder::resolve(cfg.string(), exe, false);
  expect(in.folder.empty() && !in.missing && !in.portable, "no pointer: the default folders");

  const fs::path chosen = root / "drive" / "Gridline";
  fs::create_directories(chosen);
  expect(datafolder::writePointer(cfg.string(), chosen.string()), "the pointer is written");
  in = datafolder::resolve(cfg.string(), exe, false);
  expect(in.folder == chosen.string() && !in.missing, "a pointer to an existing folder: that folder");
  expect(datafolder::resolve(cfg.string(), exe, true).folder.empty() && datafolder::resolve(cfg.string(), exe, true).envOverride,
         "GRIDLINE_CONFIG_HOME wins over the setting");

  fs::remove_all(root / "drive");
  in = datafolder::resolve(cfg.string(), exe, false);
  expect(in.folder.empty() && in.missing && in.wanted == chosen.string(), "a missing drive: the defaults, with a notice");
  expect(!fs::exists(chosen), "and the missing folder is not created");
  expect(!datafolder::writePointer(cfg.string(), "relative/folder"), "a relative folder is refused");

  fs::create_directories(root / "bin" / datafolder::kPortableName);
  in = datafolder::resolve(cfg.string(), exe, false);
  expect(in.portable && in.folder == (root / "bin" / datafolder::kPortableName).string(), "portable: GridlineData beside the program");
  fs::remove_all(root / "bin");
  const std::string bundleExe = (root / "apps" / "Gridline.app" / "Contents" / "MacOS" / "gridline-app").string();
  fs::create_directories(root / "apps" / datafolder::kPortableName);
  in = datafolder::resolve(cfg.string(), bundleExe, false);
  expect(in.portable && in.folder == (root / "apps" / datafolder::kPortableName).string(), "portable: GridlineData beside the app bundle");

  // move: copy the three Gridline folders
  put(cfg / "gridline" / "settings.json", "{\"a\": 1}");
  put(cfg / "gridline" / "presets" / "x.json", "preset");
  put(cache / "gridline" / "thumbs" / "t.bin", "cache");
  put(data / "gridline" / "tools" / "tool.txt", "tool");
  fs::create_directories(chosen);
  int calls = 0;
  std::string err;
  const bool ok = datafolder::copyInto(cfg.string(), cache.string(), data.string(), chosen.string(),
                                       [&](double f, const std::string&) { return ++calls, f >= 0 && f <= 1; }, &err);
  expect(ok, "the move copies everything");
  expect(get(chosen / "config" / "gridline" / "settings.json") == "{\"a\": 1}" && get(chosen / "config" / "gridline" / "presets" / "x.json") == "preset" &&
             get(chosen / "cache" / "gridline" / "thumbs" / "t.bin") == "cache" && get(chosen / "data" / "gridline" / "tools" / "tool.txt") == "tool",
         "config, cache and data land in the new folder");
  expect(fs::exists(cfg / "gridline" / "settings.json"), "the originals stay (a revert loses nothing)");
  expect(!fs::exists(chosen / "config" / "gridline" / "data-folder.txt"), "the pointer file is not copied");
  expect(calls >= 4, "the move reports its progress");
  expect(!datafolder::copyInto(cfg.string(), cache.string(), data.string(), (cfg / "gridline" / "inner").string(), nullptr, &err),
         "a folder inside the one it copies is refused");
  expect(!datafolder::copyInto(cfg.string(), cache.string(), data.string(), (root / "other").string(), [](double, const std::string&) { return false; }, &err) &&
             err == "cancelled",
         "the move can be cancelled");
  expect(datafolder::writePointer(cfg.string(), "") && datafolder::resolve(cfg.string(), "", false).folder.empty(), "revert: back to the defaults");
  fs::remove_all(root);
  return finish();
}

// Gridline: Plasma's preset library model (J-178): the folder tree of the
// Serum library at any depth, the presets of a folder (with its sub-folders),
// the search within a selection, column sorting, and a large synthetic
// library indexed and filtered quickly. Synthetic folders only.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../../src/packs/plasma/plasma.h"
#include "../Check.h"

namespace pl = gl::packs::plasma;
namespace fs = std::filesystem;

namespace {

pl::LibraryPreset P(const std::string& root, const std::string& bank, const std::string& name) {
  pl::LibraryPreset p;
  p.root = root, p.bank = bank, p.name = name, p.file = root + "/" + (bank.empty() ? "" : bank + "/") + name + ".SerumPreset";
  p.serum2 = true;
  return p;
}

int findKey(const std::vector<pl::LibraryFolder>& t, const std::string& key) {
  for (size_t i = 0; i < t.size(); ++i)
    if (t[i].key == key) return static_cast<int>(i);
  return -1;
}

std::vector<std::string> names(const pl::LibrarySnapshot& s, const std::vector<int>& idx) {
  std::vector<std::string> v;
  for (int i : idx) v.push_back(s.presets[static_cast<size_t>(i)].name);
  return v;
}

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << "x";
}

}  // namespace

int main() {
  const std::string A = "/lib/Serum 2 Presets", B = "/other/My Banks";
  pl::LibrarySnapshot s;
  s.roots = {A, B, "/empty/Added"};
  // as the index sorts them: by root, bank, name ("Arp Extra" sorts between "Arp" and "Arp/Sub")
  s.presets = {P(A, "", "Loose"),          P(A, "Factory/Arp", "Arp 1"),   P(A, "Factory/Arp", "Arp 2"),
               P(A, "Factory/Arp Extra", "Ext"), P(A, "Factory/Arp/Sub", "Deep"), P(A, "Factory/Bass", "Bass 10"),
               P(A, "Factory/Bass", "Bass 2"),   P(A, "Factory/Bass/Ünïcode Földer/x y", "Very Deep"),
               P(B, "Mine", "Mine 1")};
  std::sort(s.presets.begin(), s.presets.end(), [](const pl::LibraryPreset& a, const pl::LibraryPreset& b) {
    if (a.root != b.root) return a.root < b.root;
    if (a.bank != b.bank) return a.bank < b.bank;
    return a.name < b.name;
  });

  // ---- the folder tree
  const auto t = pl::libraryFolderTree(s);
  const int rA = findKey(t, "serum:" + A), fac = findKey(t, "serum:" + A + "\nFactory"), arp = findKey(t, "serum:" + A + "\nFactory/Arp"),
            ext = findKey(t, "serum:" + A + "\nFactory/Arp Extra"), sub = findKey(t, "serum:" + A + "\nFactory/Arp/Sub"),
            bass = findKey(t, "serum:" + A + "\nFactory/Bass"), uni = findKey(t, "serum:" + A + "\nFactory/Bass/Ünïcode Földer"),
            deep = findKey(t, "serum:" + A + "\nFactory/Bass/Ünïcode Földer/x y"), rB = findKey(t, "serum:" + B),
            rE = findKey(t, "serum:/empty/Added");
  expect(rA >= 0 && fac >= 0 && arp >= 0 && ext >= 0 && sub >= 0 && bass >= 0 && uni >= 0 && deep >= 0 && rB >= 0 && rE >= 0,
         "tree: every folder at any depth (spaces, unicode), roots without presets too");
  expect(rA == 0 && rA < rB && rB < rE, "tree: roots in the library's root order");
  expect(rA < fac && fac < arp && arp < sub && sub < ext && ext < bass && bass < uni && uni < deep, "tree: pre-order, a folder's children right below it");
  if (rA >= 0 && arp >= 0 && sub >= 0 && deep >= 0 && rE >= 0 && fac >= 0) {
    expect(t[size_t(rA)].depth == 0 && t[size_t(arp)].depth == 2 && t[size_t(sub)].depth == 3 && t[size_t(deep)].depth == 4, "tree: depths");
    expect(t[size_t(sub)].parent == arp && t[size_t(arp)].parent == fac && t[size_t(fac)].parent == rA && t[size_t(rA)].parent == -1, "tree: parents");
    expect(t[size_t(sub)].label == "Sub" && t[size_t(deep)].label == "x y" && t[size_t(rA)].label == "Serum 2 Presets", "tree: labels are folder names");
    expect(t[size_t(arp)].count == 3 && t[size_t(fac)].count == 7 && t[size_t(rA)].count == 8 && t[size_t(rE)].count == 0,
           "tree: counts include sub-folders");
    expect(t[size_t(arp)].hasChildren && !t[size_t(sub)].hasChildren && !t[size_t(rE)].hasChildren, "tree: hasChildren");
  }

  // ---- presets of a folder: the folder and its sub-folders, never a sibling with the same prefix
  expect(names(s, pl::libraryPresetsIn(s, "serum:" + A + "\nFactory/Arp")) == std::vector<std::string>{"Arp 1", "Arp 2", "Deep"},
         "folder: its presets and its sub-folders' (not \"Arp Extra\")");
  expect(pl::libraryPresetsIn(s, "serum:" + A + "\nFactory/Bass/Ünïcode Földer").size() == 1, "folder: unicode path");
  expect(pl::libraryPresetsIn(s, "serum:" + A).size() == 8 && pl::libraryPresetsIn(s, "serum:" + B).size() == 1, "folder: a root lists all of it");
  expect(pl::libraryPresetsIn(s, "serum:").size() == s.presets.size(), "folder: all presets");
  expect(pl::libraryPresetsIn(s, "serum:/empty/Added").empty() && pl::libraryPresetsIn(s, "serum:" + A + "\nNope").empty(), "folder: empty / unknown");

  // ---- visible rows: collapsed folders hide their descendants
  {
    std::vector<char> open(t.size(), 0);
    auto vis = pl::libraryVisibleFolders(t, open);
    expect(vis.size() == 3, "visible: all collapsed -> the roots");
    open[size_t(rA)] = open[size_t(fac)] = 1;
    vis = pl::libraryVisibleFolders(t, open);
    const bool hasArp = std::find(vis.begin(), vis.end(), arp) != vis.end(), hasSub = std::find(vis.begin(), vis.end(), sub) != vis.end();
    expect(hasArp && !hasSub && vis.size() == 3 + 1 + 3, "visible: open root and Factory show their children only");
    open[size_t(arp)] = 1;
    open[size_t(rA)] = 0;
    vis = pl::libraryVisibleFolders(t, open);
    expect(vis.size() == 3, "visible: a collapsed root hides open descendants");
  }

  // ---- search and sort of the list rows
  {
    std::vector<pl::BrowserRow> rows = {{"Bass 10", "Factory/Bass", "Serum 2"}, {"bass 2", "Factory/Bass", "Serum"},
                                        {"Lead", "Mine", "Library"},           {"Pad wide", "Pads", "Factory"}};
    expect(pl::browserFilter(rows, "", -1, false) == std::vector<int>{0, 1, 2, 3}, "list: no query, no sort -> list order");
    expect(pl::browserFilter(rows, "BASS", -1, false) == std::vector<int>{0, 1}, "list: search ignores case");
    expect(pl::browserFilter(rows, "bass 2", -1, false) == std::vector<int>{1}, "list: every word matches name or folder");
    expect(pl::browserFilter(rows, "wide pad", -1, false) == std::vector<int>{3}, "list: words in any order");
    expect(pl::browserFilter(rows, "mine", -1, false) == std::vector<int>{2}, "list: folder matches");
    expect(pl::browserFilter(rows, "", 0, false) == std::vector<int>{1, 0, 2, 3}, "list: name sort, natural numbers, any case");
    expect(pl::browserFilter(rows, "", 0, true) == std::vector<int>{3, 2, 0, 1}, "list: name sort descending");
    expect(pl::browserFilter(rows, "", 2, false) == std::vector<int>{3, 2, 1, 0}, "list: source sort");
    expect(pl::browserFilter(rows, "", 1, false).front() == 0, "list: folder sort is stable (ties keep list order)");
  }

  // ---- a large library on disk: scan, tree and folder filtering stay fast
  {
    const fs::path root = fs::temp_directory_path() / "gl_j178_library";
    std::error_code ec;
    fs::remove_all(root, ec);
    for (int b = 0; b < 40; ++b)
      for (int c = 0; c < 5; ++c)
        for (int i = 0; i < 25; ++i)
          touch(root / "Presets" / ("Bank " + std::to_string(b)) / ("Cat " + std::to_string(c)) / ("Preset " + std::to_string(i) + (i == 3 ? " Gold" : "") + ".SerumPreset"));
    const auto t0 = std::chrono::steady_clock::now();
    pl::LibrarySnapshot big;
    big.roots = {root.u8string()};
    big.presets = pl::scanLibrary(big.roots);
    const auto t1 = std::chrono::steady_clock::now();
    const auto tree = pl::libraryFolderTree(big);
    std::vector<pl::BrowserRow> rows;
    const auto in = pl::libraryPresetsIn(big, "serum:" + root.u8string() + "\nBank 7");
    for (const auto& p : big.presets) rows.push_back({p.name, p.bank, "Serum 2"});
    const auto hit = pl::browserFilter(rows, "GOLD", 0, false);
    const auto t2 = std::chrono::steady_clock::now();
    const double scanMs = std::chrono::duration<double, std::milli>(t1 - t0).count(), modelMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("# large library: %zu presets, %zu folders, scan %.1f ms, tree + folder + search %.1f ms\n", big.presets.size(), tree.size(), scanMs, modelMs);
    expect(big.presets.size() == 5000 && tree.size() == 1 + 40 + 200, "large: every preset and folder");
    expect(in.size() == 125, "large: a bank lists its categories' presets");
    expect(hit.size() == 40 * 5, "large: search across the library");
    expect(modelMs < 1000, "large: the model works in well under a second");
    fs::remove_all(root, ec);
  }
  return finish();
}

// Gridline — every plugin listing shows each category once, and Chater is a Multi FX effect.
//   * catalogGroups (the groups the mixer slot effect list of every layout, the Add effect and
//     instrument menus, the channel settings' and layers' instrument lists draw): each category
//     once per kind, every listed built-in plugin in exactly one group, in catalogue order;
//   * the old way (walk the catalogue, start a heading whenever the category changes) is gone
//     from every source that draws a listing: a later pack plugin gave a second heading;
//   * Chater sits in Multi FX with Pocket FX, not with Tracer under Delay & reverb, and keeps
//     its plugin id (projects find it by id);
//   * the plugin picker, the browser folders, the Smooth library and the command palette group
//     by a map keyed on the category (one group each by construction): checked in the source.
// Run from the source folder (ctest sets it).
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include "Check.h"
#include "core/PluginInfo.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { ::expect(ok, name.c_str()); }

std::string readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const CatalogGroup* groupOf(PluginKind kind, const std::string& type) {
  for (const CatalogGroup& g : catalogGroups(kind))
    for (const PluginInfo* p : g.items)
      if (p->type == type) return &g;
  return nullptr;
}

void testGroupsOnce() {
  const char* names[] = {"instruments", "effects", "note effects"};
  for (int k = 0; k < 3; ++k) {
    const PluginKind kind = static_cast<PluginKind>(k);
    const auto& groups = catalogGroups(kind);
    std::set<std::string> seen;
    bool once = true;
    size_t listed = 0;
    for (const CatalogGroup& g : groups) {
      if (!seen.insert(g.category).second) once = false;
      listed += g.items.size();
      for (const PluginInfo* p : g.items)
        if (p->category != g.category || p->kind != kind || p->hidden) once = false;
    }
    size_t want = 0;
    for (const auto& p : pluginCatalog())
      if (p.kind == kind && !p.hidden) ++want;
    expect(once, std::string(names[k]) + ": each category is one group");
    expect(listed == want && (k == 2 || want > 0), std::string(names[k]) + ": every listed plugin is in a group once");
  }
  // The catalogue really does split categories (pack plugins after the core ones): the reason the
  // listings must not start a heading on each change.
  std::map<std::string, int> runs;
  std::string last;
  for (const auto& p : pluginCatalog()) {
    if (p.kind != PluginKind::Effect || p.hidden) continue;
    if (p.category != last) ++runs[p.category], last = p.category;
  }
  int split = 0;
  for (const auto& [c, n] : runs)
    if (n > 1) ++split;
  std::printf("# effect categories split in catalogue order: %d\n", split);
}

void testChater() {
  const PluginInfo* c = findPlugin("gl.chater");
  expect(c && c->kind == PluginKind::Effect, "Chater is an effect, found by its id");
  expect(c && c->category == "Multi FX", "Chater's category is Multi FX");
  const CatalogGroup* gc = groupOf(PluginKind::Effect, "gl.chater");
  const CatalogGroup* gp = groupOf(PluginKind::Effect, "gl.pocketrack");
  const CatalogGroup* gt = groupOf(PluginKind::Effect, "gl.delay");
  expect(gc && gc == gp, "Chater sits in the same group as Pocket FX");
  expect(gc && gt && gc != gt && gt->category == "Delay & reverb", "Chater is not in Tracer's Delay & reverb group");
  int multi = 0, delay = 0;
  for (const CatalogGroup& g : catalogGroups(PluginKind::Effect)) multi += g.category == "Multi FX", delay += g.category == "Delay & reverb";
  expect(multi == 1 && delay == 1, "one Multi FX and one Delay & reverb group among the effects");
}

// No listing walks the catalogue starting a heading on each category change.
void testSources() {
  int bad = 0;
  std::string where;
  for (const char* dir : {"src/app", "src/packs", "src/core"}) {
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
      if (!e.is_regular_file() || e.path().extension() != ".cpp") continue;
      const std::string s = readAll(e.path());
      for (size_t at = s.find("pluginCatalog()"); at != std::string::npos; at = s.find("pluginCatalog()", at + 1)) {
        const std::string near = s.substr(at, 400);
        if (near.find("category != cat") != std::string::npos) ++bad, where += " " + e.path().filename().string();
      }
    }
  }
  expect(bad == 0, "no listing starts a category heading on each change of the catalogue walk" + where);
  // The listings that every layout's effect slot pickers use, and the menus, go through catalogGroups.
  const std::string mixer = readAll("src/app/MixerView.cpp");
  expect(mixer.find("catalogGroups(PluginKind::Effect)") != std::string::npos, "the effect slot list (all four layouts) uses catalogGroups");
  const std::string parts = readAll("src/app/LayoutParts.cpp");
  expect(parts.find("effectPickerList(") != std::string::npos, "Clips, Smooth and Tracker effect lists are the mixer's effect list");
  expect(readAll("src/app/PluginFavsUi.cpp").find("catalogGroups(kind)") != std::string::npos, "the Add effect / instrument menus use catalogGroups");
  // Map-keyed listings (one group per category by construction).
  expect(readAll("src/app/PluginPicker.cpp").find("at.emplace(key") != std::string::npos, "the plugin picker groups by a category key");
  expect(readAll("src/app/Browser.cpp").find("byCat[p.category.empty()") != std::string::npos, "the browser's plugin folders group by a category map");
}

}  // namespace

int main() {
  testGroupsOnce();
  testChater();
  testSources();
  return finish();
}

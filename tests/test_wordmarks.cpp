// Gridline — J-130 tests: plugin wordmarks. Every plugin in the catalogue has
// an embedded Tile wordmark made for its current name (so no editor header
// falls back to plain text; only hosted plugins, which are not in the
// catalogue, keep their name as text); each one parses, rasterises with a
// family tile, cut-outs and a name; it stays sharp (tile edges on whole device
// pixels at 1x and 2x) and scales with the device height; the suite logos
// exist; no stray wordmark is left without a plugin.
#include <cstdio>
#include <set>
#include <string>
#include "../src/app/icons/Wordmark.h"
#include "../src/core/PluginInfo.h"
#include "Check.h"

using namespace gl;
namespace wm = gl::icons::wordmark;

namespace {
int covered(const std::vector<uint8_t>& a, int min = 1) {
  int n = 0;
  for (uint8_t v : a) n += v >= min;
  return n;
}
uint8_t at(const wm::Raster& r, const std::vector<uint8_t>& a, int x, int y) {
  return a[static_cast<size_t>(y) * static_cast<size_t>(r.w) + static_cast<size_t>(x)];
}
}  // namespace

int main() {
  const auto& cat = pluginCatalog();
  expect(cat.size() > 100, "catalogue has the built-in plugins");
  std::set<std::string> types;
  int missing = 0, wrongName = 0, bad = 0, noHoles = 0, soft = 0, unscaled = 0;
  for (const PluginInfo& p : cat) {
    types.insert(p.type);
    const char* svg = wm::find(p.type);
    if (!svg) {
      std::printf("# no wordmark: %s (%s): add a row to tools/wordmarks/plugins.py and run make_wordmarks.py\n", p.type.c_str(),
                  p.name.c_str());
      ++missing;
      continue;
    }
    if (wm::title(svg) != p.name) {
      std::printf("# %s: wordmark made for \"%s\", the plugin is now \"%s\" (run make_wordmarks.py)\n", p.type.c_str(),
                  wm::title(svg).c_str(), p.name.c_str());
      ++wrongName;
    }
    wm::Raster r1, r2;
    if (!wm::rasterize(svg, 24, r1) || !wm::rasterize(svg, 48, r2) || wm::markColour(p.type) == 0 || covered(r1.mark) == 0 ||
        covered(r1.word) < 20) {
      std::printf("# %s: does not rasterise into a tile and a name\n", p.type.c_str());
      ++bad;
      continue;
    }
    // The pictogram is cut out of the 20 x 20 tile: well under 400 solid pixels.
    if (covered(r1.mark, 250) > 330) ++noHoles, std::printf("# %s: no cut-out in the tile\n", p.type.c_str());
    // Tile top edge on a whole pixel row: y = 2 solid, y = 1 clear (1x); y = 4 / 3 at 2x.
    if (at(r1, r1.mark, 10, 2) < 250 || at(r1, r1.mark, 10, 1) > 5 || at(r2, r2.mark, 20, 4) < 250 || at(r2, r2.mark, 20, 3) > 5)
      ++soft, std::printf("# %s: tile edge not on a whole pixel\n", p.type.c_str());
    if (r2.h != 48 || r2.w < 2 * r1.w - 2 || r2.w > 2 * r1.w + 2) ++unscaled;
  }
  expect(missing == 0, "every catalogue plugin has a wordmark (no header falls back to plain text)");
  expect(wrongName == 0, "every wordmark carries its plugin's current name");
  expect(bad == 0, "every wordmark rasterises into a family tile and a name");
  expect(noHoles == 0, "every tile has its pictogram cut out");
  expect(soft == 0, "tile edges land on whole device pixels at 1x and 2x");
  expect(unscaled == 0, "wordmarks are rasterised at the device height (2x is twice as wide, not stretched)");

  for (const char* key : {"suite", "suite-mini"}) {
    wm::Raster r;
    const bool ok = wm::rasterize(wm::find(key), 24, r) && covered(r.word) > 100 && covered(r.dim) > 20;
    expect(ok, (std::string(key) + ": suite logo with the Tile mark, gridline and suite").c_str());
  }
  {
    wm::Raster r;
    expect(wm::rasterize(wm::find("suite-mini"), 24, r) && covered(r.mark) > 20, "suite-mini: \"mini\" in the mark colour");
  }

  // Every embedded wordmark belongs to a plugin, a suite logo, or a Gridline Suite Mini
  // plugin (J-132) whose pack may not be in this build.
  int n = 0, stray = 0;
  const wm::Source* s = wm::embedded(&n);
  for (int i = 0; i < n; ++i) {
    const std::string k = s[i].key;
    if (k == "suite" || k == "suite-mini" || types.count(k) || k.compare(0, 7, "gl.mini") == 0) continue;
    std::printf("# stray wordmark %s\n", k.c_str());
    ++stray;
  }
  expect(stray == 0, "no wordmark without a plugin");
  expect(wm::find("gl.no-such-plugin") == nullptr && wm::markColour("gl.no-such-plugin") == 0, "unknown types have none");
  return finish();
}

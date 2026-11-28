// Gridline — K-450 tests: plugin icons. Every plugin in the catalogue has its own icon
// (assets/icons/ui/plugin-<id>.svg, the tile of its wordmark on the icon grid); no two plugins
// share one or have nearly the same one; each reads at 16 px (its pictogram cut out of the tile,
// visible) and stays crisp (tile edges on whole device pixels at 1x and 2x); the icon and the
// editor's wordmark show the same picture in the same colour; no icon is left without a plugin
// (or a reserved id: a plugin still on its way, such as Scribe).
//
// GRIDLINE_PLUGIN_ICON_SHEET=<dir>: contact sheets (PPM) of every icon at 16, 24, 32 and 48 px,
// at 1x and 2x, on the Dark and Light faces, plus <dir>/order.txt (one "type name" per row).
// GRIDLINE_PLUGIN_ICON_BEFORE=<dir of wordmark SVGs>: the sheets show the tiles of those wordmarks
// instead (the icons as they were drawn before K-450, cropped from the wordmarks).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "../src/app/icons/IconAtlas.h"
#include "../src/app/icons/Wordmark.h"
#include "../src/core/PluginInfo.h"
#include "Check.h"

using namespace gl;
namespace wm = gl::icons::wordmark;

namespace {

std::string stem(const std::string& type) { return "plugin-" + type.substr(3); }

bool raster(const std::string& type, int px, std::vector<uint8_t>& a) {
  const char* svg = icons::findSvg(stem(type));
  return svg && icons::rasterize(svg, px, icons::strokePx(static_cast<float>(px), 1.0f), a) && a.size() == static_cast<size_t>(px * px);
}

// The pictogram as holes in the tile: 255 - coverage, inside the tile's inner square (the rounded
// corners left out), at px.
std::vector<float> holes(const std::vector<uint8_t>& a, int px) {
  std::vector<float> h;
  const int m = static_cast<int>(std::ceil(px * 3.0 / 24.0));
  for (int y = m; y < px - m; ++y)
    for (int x = m; x < px - m; ++x) h.push_back(1.0f - static_cast<float>(a[static_cast<size_t>(y * px + x)]) / 255.0f);
  return h;
}

// Weighted Jaccard of two hole maps (1: the same picture).
float similarity(const std::vector<float>& a, const std::vector<float>& b) {
  double mn = 0, mx = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) mn += std::min(a[i], b[i]), mx += std::max(a[i], b[i]);
  return mx > 0 ? static_cast<float>(mn / mx) : 1.0f;
}

uint32_t markColour(const char* svg) {
  const char* p = svg ? std::strstr(svg, "id=\"mark") : nullptr;
  const char* f = p ? std::strstr(p, "fill=\"#") : nullptr;
  return f ? static_cast<uint32_t>(std::strtoul(f + 7, nullptr, 16)) : 0;
}

// ---- contact sheets ----

struct Rgb {
  std::vector<uint8_t> px;
  int w = 0, h = 0;
  Rgb(int ww, int hh, uint32_t bg) : px(static_cast<size_t>(ww) * hh * 3), w(ww), h(hh) {
    for (size_t i = 0; i < px.size(); i += 3) px[i] = (bg >> 16) & 0xFF, px[i + 1] = (bg >> 8) & 0xFF, px[i + 2] = bg & 0xFF;
  }
  void fill(int x0, int y0, int x1, int y1, uint32_t c) {
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
      for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
        uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 3];
        p[0] = (c >> 16) & 0xFF, p[1] = (c >> 8) & 0xFF, p[2] = c & 0xFF;
      }
  }
  void blend(int x0, int y0, const std::vector<uint8_t>& a, int aw, int ah, uint32_t c) {
    for (int y = 0; y < ah; ++y)
      for (int x = 0; x < aw; ++x) {
        const int X = x0 + x, Y = y0 + y;
        if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
        const float t = static_cast<float>(a[static_cast<size_t>(y) * aw + x]) / 255.0f;
        uint8_t* p = &px[(static_cast<size_t>(Y) * w + X) * 3];
        const int src[3] = {static_cast<int>((c >> 16) & 0xFF), static_cast<int>((c >> 8) & 0xFF), static_cast<int>(c & 0xFF)};
        for (int k = 0; k < 3; ++k) p[k] = static_cast<uint8_t>(std::lround(p[k] + (src[k] - p[k]) * t));
      }
  }
  void save(const std::string& path) const {
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fprintf(f, "P6\n%d %d\n255\n", w, h);
      std::fwrite(px.data(), 1, px.size(), f);
      std::fclose(f);
    }
  }
};

// The colour a tile is drawn in on the light faces (theme::lit: deepened toward #101418 by 0.34).
uint32_t litLight(uint32_t c) {
  auto ch = [&](int s, int to) { return static_cast<uint32_t>(std::lround(((c >> s) & 0xFF) + (to - static_cast<int>((c >> s) & 0xFF)) * 0.34)); };
  return (ch(16, 0x10) << 16) | (ch(8, 0x14) << 8) | ch(0, 0x18);
}

// One icon at `devPx` device px: the new icon, or the tile cropped from an old wordmark.
bool iconAt(const std::string& type, int devPx, const std::string& beforeDir, std::vector<uint8_t>& a, int& aw, int& ah, uint32_t& colour) {
  if (beforeDir.empty()) {
    const char* svg = icons::findSvg(stem(type));
    if (!svg || !icons::rasterize(svg, devPx, icons::strokePx(static_cast<float>(devPx), 1.0f), a)) return false;
    aw = ah = devPx;
    colour = markColour(svg);
    return true;
  }
  std::ifstream f(beforeDir + "/" + type.substr(3) + ".svg");
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  wm::Raster r;
  const int h = static_cast<int>(std::lround(devPx * 24.0 / 20.0));  // the wordmark height whose tile is devPx
  if (!wm::rasterize(text.c_str(), h, r)) return false;
  const int y0 = static_cast<int>(std::lround(h * 2.0 / 24.0));
  aw = std::min(r.w, devPx + 1), ah = std::min(r.h - y0, devPx + 1);
  a.assign(static_cast<size_t>(aw) * ah, 0);
  for (int y = 0; y < ah; ++y)
    for (int x = 0; x < aw; ++x) a[static_cast<size_t>(y) * aw + x] = r.mark[static_cast<size_t>(y + y0) * r.w + x];
  colour = markColour(text.c_str());
  return true;
}

void sheets(const std::string& dir, const std::vector<const PluginInfo*>& list, const std::string& beforeDir) {
  const int sizes[] = {16, 24, 32, 48};
  const int rowH = 104, perPage = 40, gap = 12, left = 12;
  int width = left;
  for (int sc = 1; sc <= 2; ++sc)
    for (int s : sizes) width += s * sc + gap;
  width += 16 + left;  // a gap between the 1x and 2x groups
  struct Face {
    const char* name;
    uint32_t bg, cell;
    bool light;
  } faces[] = {{"dark", 0x353D42, 0x434C51, false}, {"light", 0xE0E5E8, 0xEEF1F3, true}};
  std::ofstream order(dir + "/order.txt");
  for (const PluginInfo* p : list) order << p->type << " " << p->name << "\n";
  const int pages = static_cast<int>((list.size() + perPage - 1) / perPage);
  for (const Face& fc : faces)
    for (int pg = 0; pg < pages; ++pg) {
      const int n = std::min(perPage, static_cast<int>(list.size()) - pg * perPage);
      Rgb img(width, n * rowH + 8, fc.bg);
      for (int i = 0; i < n; ++i) {
        const PluginInfo* p = list[static_cast<size_t>(pg * perPage + i)];
        const int y = 4 + i * rowH;
        img.fill(4, y, width - 4, y + rowH - 4, fc.cell);
        int x = left;
        for (int sc = 1; sc <= 2; ++sc) {
          for (int s : sizes) {
            std::vector<uint8_t> a;
            int aw = 0, ah = 0;
            uint32_t col = 0;
            const int dev = s * sc;
            if (iconAt(p->type, dev, beforeDir, a, aw, ah, col))
              img.blend(x, y + (rowH - 4 - dev) / 2, a, aw, ah, fc.light ? litLight(col) : col);
            x += dev + gap;
          }
          x += 16;
        }
      }
      char name[64];
      std::snprintf(name, sizeof name, "/%s-%d.ppm", fc.name, pg + 1);
      img.save(dir + name);
    }
}

}  // namespace

int main() {
  const auto& cat = pluginCatalog();
  expect(cat.size() > 100, "catalogue has the built-in plugins");
  std::vector<const PluginInfo*> list;
  for (const PluginInfo& p : cat) list.push_back(&p);
  // Reserved: icons drawn for plugins that are not in this build yet (they are checked like the
  // others: own picture, readable, unique, made for that name).
  static std::vector<PluginInfo> reserved;
  for (const auto& rv : {std::pair<const char*, const char*>{"gl.scribe", "Scribe"}}) {
    if (std::any_of(cat.begin(), cat.end(), [&](const PluginInfo& p) { return p.type == rv.first; })) continue;
    PluginInfo r;
    r.type = rv.first;
    r.name = rv.second;
    reserved.push_back(r);
  }
  for (const PluginInfo& r : reserved) list.push_back(&r);
  std::sort(list.begin(), list.end(), [](const PluginInfo* a, const PluginInfo* b) { return a->type < b->type; });

  // ---- every plugin has its own icon, made for it ----
  int missing = 0, wrongName = 0, noColour = 0;
  std::set<std::string> types;
  for (const PluginInfo* p : list) {
    types.insert(p->type);
    const char* svg = icons::findSvg(stem(p->type));
    if (!svg) {
      std::printf("# no icon: %s (%s): add a row to tools/wordmarks/plugins.py and run make_wordmarks.py\n", p->type.c_str(), p->name.c_str());
      ++missing;
      continue;
    }
    const char* t0 = std::strstr(svg, "<title>");
    const char* t1 = t0 ? std::strstr(t0, "</title>") : nullptr;
    if (!t0 || !t1 || std::string(t0 + 7, t1) != p->name) ++wrongName, std::printf("# %s: icon made for another name\n", p->type.c_str());
    if (!markColour(svg)) ++noColour;
  }
  expect(missing == 0, "every registered plugin has its own icon (none falls back to a generic one)");
  expect(wrongName == 0, "every icon was made for its plugin's current name");
  expect(noColour == 0, "every icon has a family colour");
  {
    int n = 0, stray = 0;
    const icons::SvgSource* s = icons::embeddedSvgs(&n);
    for (int i = 0; i < n; ++i) {
      const std::string name = s[i].name;
      if (name.rfind("plugin-", 0) != 0) continue;
      const std::string type = "gl." + name.substr(7);
      if (!types.count(type) && type.compare(0, 7, "gl.mini") != 0) ++stray, std::printf("# stray icon %s\n", name.c_str());
    }
    expect(stray == 0, "no icon without a plugin");
  }

  // ---- unique: no two plugins share an icon, none nearly alike ----
  {
    std::map<std::string, std::string> byCut;  // the pictogram's shapes -> type
    int shared = 0;
    for (const PluginInfo* p : list) {
      const char* svg = icons::findSvg(stem(p->type));
      if (!svg) continue;
      const char* c = std::strstr(svg, "id=\"cut");
      const std::string cut = c ? std::string(c, std::strstr(c, "</svg>") ? std::strstr(c, "</svg>") : c + std::strlen(c)) : "";
      auto [it, fresh] = byCut.emplace(cut, p->type);
      if (!fresh) ++shared, std::printf("# %s and %s share one pictogram\n", it->second.c_str(), p->type.c_str());
    }
    expect(shared == 0, "no two plugins share an icon");
    std::vector<std::vector<float>> h(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
      std::vector<uint8_t> a;
      if (raster(list[i]->type, 24, a)) h[i] = holes(a, 24);
    }
    struct Pair {
      float s;
      size_t i, j;
    };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < list.size(); ++i)
      for (size_t j = i + 1; j < list.size(); ++j)
        if (!h[i].empty() && !h[j].empty()) pairs.push_back({similarity(h[i], h[j]), i, j});
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.s > b.s; });
    int alike = 0;
    for (size_t k = 0; k < pairs.size() && k < 12; ++k) {
      std::printf("# most alike %zu: %s / %s %.3f\n", k + 1, list[pairs[k].i]->type.c_str(), list[pairs[k].j]->type.c_str(), static_cast<double>(pairs[k].s));
      alike += pairs[k].s > 0.75f;
    }
    expect(alike == 0, "no two icons nearly alike (pictograms overlap under 75% at 24 px)");
  }

  // ---- readable and crisp ----
  {
    int faint = 0, soft = 0, unmatched = 0;
    for (const PluginInfo* p : list) {
      std::vector<uint8_t> a;
      // 16 px: the pictogram shows: enough clear texels inside the tile.
      if (raster(p->type, 16, a)) {
        int clear = 0;
        for (float v : holes(a, 16)) clear += v >= 0.6f;
        if (clear < 10) ++faint, std::printf("# %s at 16 px: the pictogram hardly shows (%d clear texels)\n", p->type.c_str(), clear);
      }
      // Tile edges on whole device pixels: the middle of each edge solid at 16 / 24 / 32 / 48 and their 2x.
      for (int px : {16, 24, 32, 48, 64, 96}) {
        if (!raster(p->type, px, a)) continue;
        const int m = px / 2;
        const uint8_t e[4] = {a[static_cast<size_t>(m)], a[static_cast<size_t>((px - 1) * px + m)], a[static_cast<size_t>(m * px)],
                              a[static_cast<size_t>(m * px + px - 1)]};
        if (*std::min_element(e, e + 4) < 245) {
          ++soft, std::printf("# %s at %d px: a tile edge is soft\n", p->type.c_str(), px);
          break;
        }
      }
      // The editor's wordmark shows the same picture in the same colour.
      const char* wsvg = wm::find(p->type);
      wm::Raster r;
      if (wsvg && raster(p->type, 20, a) && wm::rasterize(wsvg, 24, r) && r.w >= 20) {
        std::vector<uint8_t> tile(400);
        for (int y = 0; y < 20; ++y)
          for (int x = 0; x < 20; ++x) tile[static_cast<size_t>(y * 20 + x)] = r.mark[static_cast<size_t>((y + 2) * r.w + x)];
        const float sim = similarity(holes(a, 20), holes(tile, 20));
        if (sim < 0.6f || wm::markColour(p->type) != IM_COL32((markColour(icons::findSvg(stem(p->type))) >> 16) & 0xFF,
                                                              (markColour(icons::findSvg(stem(p->type))) >> 8) & 0xFF,
                                                              markColour(icons::findSvg(stem(p->type))) & 0xFF, 255))
          ++unmatched, std::printf("# %s: the icon and the wordmark's tile differ (%.2f)\n", p->type.c_str(), static_cast<double>(sim));
      }
    }
    expect(faint == 0, "every pictogram shows at 16 px");
    expect(soft == 0, "tile edges land on whole device pixels at 16 to 48 px, 1x and 2x");
    expect(unmatched == 0, "every icon matches its editor wordmark's tile (picture and colour)");
  }

  if (const char* dir = std::getenv("GRIDLINE_PLUGIN_ICON_SHEET")) {
    const char* before = std::getenv("GRIDLINE_PLUGIN_ICON_BEFORE");
    sheets(dir, list, before ? before : "");
    std::printf("# contact sheets in %s\n", dir);
  }
  return finish();
}

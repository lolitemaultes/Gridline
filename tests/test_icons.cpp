// Gridline — Q-13 tests: the SVG icon atlas. Every ui::Icon has an embedded
// SVG that parses and rasterises; the atlas packs them at the exact device
// size; drawing never rasterises an icon twice; a scale / density change
// clears the atlas and rasterises again at the new size; straight strokes land
// on whole pixels at 1x.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include "../src/app/Widgets.h"
#include "../src/app/icons/IconAtlas.h"
#include "../src/app/icons/IconTexture.h"
#include "../third_party/imgui/imgui_internal.h"
#include "Check.h"

using namespace gl;

namespace {
int coverage(const std::vector<uint8_t>& a) {
  int n = 0;
  for (uint8_t v : a) n += v > 0;
  return n;
}
// Rows (of a px-wide bitmap) that are fully opaque across at least `run` texels.
int solidRows(const std::vector<uint8_t>& a, int px, int run) {
  int rows = 0;
  for (int y = 0; y < px; ++y) {
    int best = 0, cur = 0;
    for (int x = 0; x < px; ++x) {
      cur = a[static_cast<size_t>(y) * px + x] >= 250 ? cur + 1 : 0;
      best = std::max(best, cur);
    }
    rows += best >= run;
  }
  return rows;
}
}  // namespace

int main() {
  {  // J-77: tool strip icons are sized like the top-bar toggles (metronome etc.)
    expect(icons::toggleIconBox(28.0f) == 16.0f, "top-bar toggle (28 px button): 16 px icon box");
    expect(icons::toolIconBox(24.0f) == 16.0f, "tool strip (24 px button): 16 px icon box");
    expect(icons::toolIconBox(24.0f) == icons::toggleIconBox(28.0f), "tool icons match the top-bar toggles");
    expect(icons::toolIconBox(24.0f) < 20.0f, "tool icons are smaller than before (20 px)");
    expect(icons::toolIconBox(32.0f) == 20.0f && icons::toolIconBox(36.0f) == 24.0f, "larger strips keep the proportion on standard sizes");
    expect((24.0f - icons::toolIconBox(24.0f)) * 0.5f == 4.0f, "4 px clear around the glyph in a 24 px button");
  }
  int n = 0;
  const icons::SvgSource* svgs = icons::embeddedSvgs(&n);
  expect(n >= 50, "the icon set is embedded");

  // ---- every referenced icon exists, and every vendored SVG is referenced ----
  {
    std::set<std::string> used;
    bool all = true;
    for (int i = 0; i < static_cast<int>(ui::Icon::Count); ++i) {
      const char* name = ui::iconSvgName(static_cast<ui::Icon>(i));
      if (!name || !icons::findSvg(name)) {
        std::printf("# missing SVG for icon %d (%s)\n", i, name ? name : "null");
        all = false;
      } else {
        used.insert(name);
      }
    }
    expect(all, "every ui::Icon maps to an embedded SVG");
    // The track icon set (core/TrackIconSet: mixer, playlist and channel icons) uses the track-* SVGs by name.
    for (int i = 0; i < n; ++i)
      if (std::string(svgs[i].name).rfind("track-", 0) == 0) used.insert(svgs[i].name);
    bool none = true;
    for (int i = 0; i < n; ++i)
      if (!used.count(svgs[i].name)) {
        std::printf("# unused SVG %s\n", svgs[i].name);
        none = false;
      }
    expect(none, "only the icons in use are vendored");
    // The source files are the ones embedded (assets/icons/ui/<stem>.svg).
    bool files = true;
    for (int i = 0; i < n; ++i) {
      std::ifstream f(std::string("assets/icons/ui/") + svgs[i].name + ".svg");
      std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      files = files && text == svgs[i].svg;
    }
    expect(files, "embedded SVGs match assets/icons/ui");
    // Tool strip family: distinct glyphs (the playback tool is not the transport play).
    std::set<std::string> tools;
    for (ui::Icon t : {ui::Icon::ToolDraw, ui::Icon::ToolPaint, ui::Icon::ToolDelete, ui::Icon::ToolMute, ui::Icon::ToolSlip,
                       ui::Icon::ToolSlice, ui::Icon::ToolSelect, ui::Icon::ToolZoom, ui::Icon::ToolPlay, ui::Icon::ToolDetune,
                       ui::Icon::ToolSnap})
      tools.insert(ui::iconSvgName(t));
    expect(tools.size() == 11, "eleven distinct tool glyphs");
    expect(std::string(ui::iconSvgName(ui::Icon::ToolPlay)) != ui::iconSvgName(ui::Icon::Play), "playback tool differs from transport play");
    expect(std::string(ui::iconSvgName(ui::Icon::Eye)) != ui::iconSvgName(ui::Icon::EyeOff), "eye toggle: two states");
  }

  // ---- rasterisation: every SVG parses and draws inside its box ----
  {
    bool ok = true;
    for (int i = 0; i < n; ++i) {
      std::vector<uint8_t> a;
      for (int px : {16, 20, 24, 32, 48}) {
        const bool r = icons::rasterize(svgs[i].svg, px, icons::strokePx(static_cast<float>(px), 1.0f), a);
        const int cov = coverage(a);
        if (!r || a.size() != static_cast<size_t>(px * px) || cov < px || cov > px * px * 9 / 10) {
          std::printf("# %s at %d px: parsed %d, coverage %d\n", svgs[i].name, px, r, cov);
          ok = false;
        }
      }
    }
    expect(ok, "every SVG rasterises at 16 / 20 / 24 / 32 / 48 px");
    std::vector<uint8_t> a;
    expect(!icons::rasterize("<svg", 16, 1.5f, a) || coverage(a) == 0, "broken SVG text draws nothing");
    expect(icons::strokePx(16, 1) == 1.5f && icons::strokePx(24, 1) == 2.0f && icons::strokePx(16, 2) == 3.0f,
           "stroke: 1.5 px at 16, 2 px at 24, scaled with density");
  }

  // ---- K-3: every icon's ink lies inside its box (nothing cut off) ----
  // Ink bounds (each shape's exact geometry, stroked shapes widened by half the
  // stroke drawn at that size) against the 24-unit box at the smallest track
  // icon size (12), a common one (16) and the largest (64); then the bitmaps at
  // 16 and 64: no edge row or column is solid ink (a shape that runs past the
  // box shows there as a cut, fully covered border line).
  {
    bool inside = true, edges = true;
    for (int i = 0; i < n; ++i) {
      for (int px : {12, 16, 64}) {
        float bb[4] = {0, 0, 0, 0};
        const float strokeUnits = icons::strokePx(static_cast<float>(px), 1.0f) * 24.0f / static_cast<float>(px);
        if (!icons::inkBounds(svgs[i].svg, strokeUnits, bb)) continue;
        if (std::min(bb[0], bb[1]) < -0.01f || std::max(bb[2], bb[3]) > 24.01f) {
          std::printf("# %s at %d px: ink x %.2f..%.2f, y %.2f..%.2f leaves the 24-unit box\n", svgs[i].name, px, bb[0], bb[2], bb[1], bb[3]);
          inside = false;
        }
        if (px == 12) continue;
        std::vector<uint8_t> a;
        icons::rasterize(svgs[i].svg, px, icons::strokePx(static_cast<float>(px), 1.0f), a);
        if (a.size() != static_cast<size_t>(px * px)) continue;
        auto solidLine = [&](bool row, int k) {
          int run = 0, best = 0;
          for (int j = 0; j < px; ++j) {
            const uint8_t v = row ? a[static_cast<size_t>(k) * px + j] : a[static_cast<size_t>(j) * px + k];
            best = std::max(best, run = v >= 250 ? run + 1 : 0);
          }
          return best >= std::max(3, px / 5);
        };
        if (solidLine(true, 0) || solidLine(true, px - 1) || solidLine(false, 0) || solidLine(false, px - 1)) {
          std::printf("# %s at %d px: solid ink along the box edge (cut off)\n", svgs[i].name, px);
          edges = false;
        }
      }
    }
    // GRIDLINE_ICON_SHEET=<file.pgm>: a contact sheet of every icon at 12 and 64 px (dark ink on
    // white, each box outlined in grey one texel outside it), to look over by eye.
    if (const char* sheet = std::getenv("GRIDLINE_ICON_SHEET")) {
      const int cols = 16, cell = 12 + 64 + 12, rows = (n + cols - 1) / cols, W = cols * cell, H = rows * 72;
      std::vector<uint8_t> img(static_cast<size_t>(W) * H, 255);
      for (int i = 0; i < n; ++i) {
        const int ox = (i % cols) * cell + 4, oy = (i / cols) * 72 + 4;
        int x0 = ox;
        for (int px : {12, 64}) {
          std::vector<uint8_t> a;
          icons::rasterize(svgs[i].svg, px, icons::strokePx(static_cast<float>(px), 1.0f), a);
          for (int k = -1; k <= px; ++k)
            for (int e : {-1, px}) {
              img[static_cast<size_t>(oy + e) * W + x0 + k] = 170;
              img[static_cast<size_t>(oy + k) * W + x0 + e] = 170;
            }
          for (int y = 0; y < px && a.size() == static_cast<size_t>(px * px); ++y)
            for (int x = 0; x < px; ++x) img[static_cast<size_t>(oy + y) * W + x0 + x] = static_cast<uint8_t>(255 - a[static_cast<size_t>(y) * px + x]);
          x0 += px + 4;
        }
      }
      if (FILE* f = std::fopen(sheet, "wb")) {
        std::fprintf(f, "P5\n%d %d\n255\n", W, H);
        std::fwrite(img.data(), 1, img.size(), f);
        std::fclose(f);
      }
    }
    expect(inside, "every icon's ink (stroke included) stays inside its 24-unit box at 12, 16 and 64 px");
    expect(edges, "no icon is cut by its box edge at 16 and 64 px");
    float bb[4] = {0, 0, 0, 0};
    expect(icons::inkBounds(icons::findSvg("track-snare"), 2.0f, bb) && bb[3] <= 21.0f && bb[1] >= 3.0f && bb[3] - 12.0f <= 12.0f - bb[1] + 0.5f,
           "snare drum: the shell's bottom curve sits inside the box, the drum centred (K-3)");
  }

  // ---- crisp straight strokes at 1x (grid fitting) ----
  {
    std::vector<uint8_t> a;
    icons::rasterize(icons::findSvg("menu"), 16, icons::strokePx(16, 1), a);
    expect(solidRows(a, 16, 10) == 3, "menu at 16 px: three solid bars");
    icons::rasterize(icons::findSvg("minus"), 24, icons::strokePx(24, 1), a);
    expect(solidRows(a, 24, 12) == 2, "minus at 24 px: a 2 px solid bar");
    icons::rasterize(icons::findSvg("slip"), 20, icons::strokePx(20, 1), a);
    int solidCols = 0;
    for (int x = 0; x < 20; ++x) {
      int run = 0, best = 0;
      for (int y = 0; y < 20; ++y) best = std::max(best, run = a[static_cast<size_t>(y) * 20 + x] >= 170 ? run + 1 : 0);
      solidCols += best >= 10;
    }
    expect(solidCols >= 2, "slip at 20 px: both clip edges are even, strong columns (grid fitted)");
  }

  // ---- the atlas: packing, sizes, scale changes ----
  {
    icons::Atlas at;
    bool ok = true;
    for (int i = 0; i < n; ++i)
      for (float s : {16.0f, 20.0f, 24.0f}) {
        const icons::Atlas::Rect* r = at.get(i, s);
        ok = ok && r && r->size == static_cast<int>(s) && r->x >= 1 && r->y >= 1 && r->x + r->size < icons::Atlas::kPage &&
             r->y + r->size < icons::Atlas::kPage;
      }
    expect(ok, "atlas builds: every icon at 16 / 20 / 24 px fits");
    expect(at.rasterCount() == n * 3 && at.entryCount() == n * 3, "one rasterisation per icon and size");
    for (int i = 0; i < n; ++i) at.get(i, 20.0f);
    expect(at.rasterCount() == n * 3, "a cached icon is never rasterised again");
    // No overlap between packed rects (pad included).
    std::vector<icons::Atlas::Rect> rs;
    for (int i = 0; i < n; ++i) rs.push_back(*at.get(i, 24.0f));
    bool disjoint = true;
    for (size_t i = 0; i < rs.size(); ++i)
      for (size_t j = i + 1; j < rs.size(); ++j)
        if (rs[i].page == rs[j].page && rs[i].x < rs[j].x + rs[j].size + 1 && rs[j].x < rs[i].x + rs[i].size + 1 &&
            rs[i].y < rs[j].y + rs[j].size + 1 && rs[j].y < rs[i].y + rs[i].size + 1)
          disjoint = false;
    expect(disjoint, "packed icons never overlap");
    const int before = at.rasterCount();
    expect(!at.setDensity(1.0f), "same density: atlas kept");
    expect(at.setDensity(1.5f) && at.entryCount() == 0 && at.resetCount() == 1, "density 1.5: atlas cleared");
    const icons::Atlas::Rect* r = at.get("pencil", 16.0f);
    expect(r && r->size == 24 && at.rasterCount() == before + 1, "16 px icon at 1.5x: rasterised again at 24 device px");
    at.setDensity(2.0f);
    r = at.get("pencil", 16.0f);
    expect(r && r->size == 32, "16 px icon at 2x: 32 device px");
    expect(at.get("no-such-icon", 16.0f) == nullptr, "unknown icon: nothing");
  }

  // ---- drawing through ImGui (headless): no per-frame rasterisation ----
  {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // headless: textures are never uploaded
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    auto frame = [&](float density) {
      io.DisplayFramebufferScale = ImVec2(density, density);
      ImGui::NewFrame();
      ImDrawList* dl = ImGui::GetForegroundDrawList();
      for (int i = 0; i < n; ++i)  // every embedded icon (ui::Icon and the track icon set)
        icons::draw(dl, svgs[i].name, ImVec2(20.0f + (i % 200) * 3.3f, 40.0f + (i / 200) * 24.0f), 20.0f,
                    IM_COL32_WHITE);
      ImGui::Render();
    };
    // The atlas installs the glyph hook (ImGui's own arrows, checks, crosses as
    // SVG icons); ImGui's implicit debug window would draw them from frame 2,
    // so this count covers icons::draw alone.
    icons::atlas();
    expect(ImGui::GetCurrentContext()->GlyphHook != nullptr, "the atlas installs ImGui's glyph hook");
    ImGui::GetCurrentContext()->GlyphHook = nullptr;
    frame(1.0f);
    icons::Atlas& at = icons::atlas();
    const int first = at.rasterCount();
    expect(first == n, "first frame: each icon rasterised once");
    for (int k = 0; k < 5; ++k) frame(1.0f);
    expect(at.rasterCount() == first, "later frames: no rasterisation");
    expect(icons::textureCount() >= 1 && ImGui::GetCurrentContext()->UserTextures.Size == icons::textureCount(),
           "atlas pages registered as ImGui textures");
    // Quads land on whole device pixels with texels mapped 1:1.
    bool snapped = true;
    for (const ImDrawVert& v : ImGui::GetDrawData()->CmdLists[0]->VtxBuffer)
      snapped = snapped && v.pos.x == std::round(v.pos.x) && v.pos.y == std::round(v.pos.y);
    expect(snapped, "1x: icon quads on whole pixels");
    frame(1.5f);
    expect(at.resetCount() == 1 && at.rasterCount() == first + n, "UI scale 1.5: every icon rasterised again, once");
    snapped = true;
    for (const ImDrawVert& v : ImGui::GetDrawData()->CmdLists[0]->VtxBuffer)
      snapped = snapped && std::fabs(v.pos.x * 1.5f - std::round(v.pos.x * 1.5f)) < 1e-3f &&
                std::fabs(v.pos.y * 1.5f - std::round(v.pos.y * 1.5f)) < 1e-3f;
    expect(snapped, "1.5x: icon quads on whole device pixels");
    frame(1.5f);
    frame(1.5f);
    expect(at.rasterCount() == first + n, "1.5x: later frames reuse the atlas");
    frame(2.0f);
    expect(at.resetCount() == 2 && at.rasterCount() == first + 2 * n, "2x: rasterised again for the new density");
    // The page textures hold white texels with the icon coverage in alpha (tinted by the vertex colour).
    ImTextureData* t = ImGui::GetCurrentContext()->UserTextures[0];
    bool white = true;
    int lit = 0;
    for (int i = 0; i < t->Width * t->Height; ++i) {
      const unsigned char* p = static_cast<unsigned char*>(t->GetPixels()) + i * 4;
      lit += p[3] > 0;
      white = white && (p[3] == 0 || (p[0] == 255 && p[1] == 255 && p[2] == 255));
    }
    expect(white && lit > 0, "page texture: white + alpha");
    ImGui::DestroyContext();
  }
  return finish();
}

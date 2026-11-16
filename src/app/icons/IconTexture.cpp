// Gridline — Q-13 interface icons on screen (see IconTexture.h).
#include "IconTexture.h"
#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>
#include "../../../third_party/imgui/imgui_internal.h"

namespace gl::icons {

namespace {

struct State {
  Atlas atlas;
  std::vector<ImTextureData*> tex;  // one RGBA page texture per atlas page (white + coverage alpha)
  // drawFit: chosen box (logical px) per (icon, target ink w, h in device px); cleared with the atlas.
  struct Fit {
    float box = 0, stroke = 0;  // logical px; stroke override (device px, 0: the rule's)
    bool fits = false;          // false: no size fits, the smallest is used
  };
  std::unordered_map<uint64_t, Fit> fits;
  std::unordered_map<int, std::pair<float, float>> refInk;  // icon -> ink fraction (w, h) of a 48 px raster
  int fitsReset = -1;
  std::vector<uint64_t> pending;  // fits asked for while deferring, worked out later (serviceDeferred)
  std::vector<std::pair<int, float>> pendingDraws;  // K-291: draw() sizes asked for while deferring (icon, logical px)
  std::unordered_map<int, std::array<float, 5>> geom;  // icon -> geometry bounds (viewBox units), stroked flag
};

std::unordered_map<ImGuiContext*, std::unique_ptr<State>>& states() {
  static std::unordered_map<ImGuiContext*, std::unique_ptr<State>> m;
  return m;
}

bool glyphHook(ImDrawList* dl, int glyph, ImVec2 inkMin, ImVec2 inkMax, ImU32 col);

State& state() {
  auto& p = states()[ImGui::GetCurrentContext()];
  if (!p) {
    p = std::make_unique<State>();
    ImGui::GetCurrentContext()->GlyphHook = glyphHook;  // ImGui's own arrows, checks, crosses: SVG too
  }
  return *p;
}

// ImGui's built-in glyphs (tree / combo / menu arrows, check marks, close
// crosses, step buttons) drawn as SVG icons in the same ink bounds.
bool glyphHook(ImDrawList* dl, int glyph, ImVec2 inkMin, ImVec2 inkMax, ImU32 col) {
  static const char* const kNames[] = {"caret-left", "caret-right", "caret-up", "caret-down", "check", "x", "minus", "plus"};
  if (glyph < 0 || glyph >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return false;
  const int idx = state().atlas.indexOf(kNames[glyph]);
  if (idx < 0) return false;
  if (glyph == 6 || glyph == 7) {  // step "-" / "+": the box is the font glyph's cell; take its ink
    ImFont* f = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    ImFontBaked* baked = f ? f->GetFontBaked(size) : nullptr;
    const ImWchar ch = glyph == 6 ? '-' : '+';
    const ImFontGlyph* g = baked ? baked->FindGlyph(ch) : nullptr;
    if (g) {  // RenderText's position, snapped to whole pixels as it draws
      const ImVec2 pos(inkMin.x - g->X0 * size / baked->Size, inkMin.y - g->Y0 * size / baked->Size);
      glyphInk(f, size, ch, ImVec2(static_cast<float>(static_cast<int>(pos.x)), static_cast<float>(static_cast<int>(pos.y))), &inkMin, &inkMax);
    }
  }
  return drawFit(dl, idx, inkMin, inkMax, col, true);  // too small for the icon: ImGui draws its own
}

// Copies changed atlas texels into the page textures and queues their upload.
void sync(State& st) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  auto& pages = st.atlas.pages();
  while (st.tex.size() < pages.size()) {
    ImTextureData* t = IM_NEW(ImTextureData)();
    t->Create(ImTextureFormat_RGBA32, Atlas::kPage, Atlas::kPage);
    st.tex.push_back(t);
    pages[st.tex.size() - 1].cleared = true;
  }
  for (size_t i = 0; i < pages.size(); ++i) {
    ImTextureData* t = st.tex[i];
    if (!g.UserTextures.contains(t)) {  // a new context at this address, or first use: (re)create
      t->SetTexID(ImTextureID_Invalid);
      t->BackendUserData = nullptr;
      t->Status = ImTextureStatus_WantCreate;
      ImGui::RegisterUserTexture(t);
      pages[i].cleared = true;
    }
    Atlas::Page& pg = pages[i];
    int x0 = pg.dirtyX0, y0 = pg.dirtyY0, x1 = pg.dirtyX1, y1 = pg.dirtyY1;
    if (pg.cleared) x0 = y0 = 0, x1 = y1 = Atlas::kPage;
    if (x1 <= x0 || y1 <= y0) continue;
    for (int y = y0; y < y1; ++y) {
      const uint8_t* a = &pg.alpha[static_cast<size_t>(y) * Atlas::kPage];
      uint32_t* out = reinterpret_cast<uint32_t*>(t->GetPixelsAt(0, y));
      for (int x = x0; x < x1; ++x) out[x] = IM_COL32(255, 255, 255, a[x]);
    }
    if (t->Status == ImTextureStatus_OK || t->Status == ImTextureStatus_WantUpdates)
      ImTextureDataQueueUpload(t, x0, y0, x1 - x0, y1 - y0);
    pg.cleared = false;
    pg.dirtyX0 = pg.dirtyY0 = Atlas::kPage;
    pg.dirtyX1 = pg.dirtyY1 = 0;
  }
}

}  // namespace

Atlas& atlas() { return state().atlas; }
int textureCount() { return static_cast<int>(state().tex.size()); }

float standardSize(float logicalPx) {
  static const float kSizes[] = {10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 64};
  if (logicalPx > 64) return std::round(logicalPx);
  float best = kSizes[0];
  for (float s : kSizes)
    if (std::fabs(s - logicalPx) < std::fabs(best - logicalPx)) best = s;
  return best;
}

float toggleIconBox(float button) { return standardSize(std::round(button * 0.52f) * 1.2f); }
float toolIconBox(float button) { return standardSize(button * (2.0f / 3.0f)); }

namespace {
bool gDefer = false;  // K-112: a layout switch frame: new icon sizes are put off (see drawFit, draw)
}  // namespace

void draw(ImDrawList* dl, int svgIndex, ImVec2 c, float logicalPx, ImU32 col) {
  if (!dl || svgIndex < 0 || (col & IM_COL32_A_MASK) == 0) return;
  State& st = state();
  const ImGuiIO& io = ImGui::GetIO();
  const float d = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
  st.atlas.setDensity(d);
  const Atlas::Rect* r = gDefer ? st.atlas.find(svgIndex, logicalPx) : st.atlas.get(svgIndex, logicalPx);
  if (!r && gDefer) {
    // K-291: a size not made yet is rasterised after the switch frame (serviceDeferred, a
    // rasterisation costs up to a few ms); meanwhile the largest size made so far stands in,
    // scaled into the box, under the switch snapshot.
    const std::pair<int, float> want(svgIndex, logicalPx);
    if (std::find(st.pendingDraws.begin(), st.pendingDraws.end(), want) == st.pendingDraws.end()) st.pendingDraws.push_back(want);
    const Atlas::Rect* best = st.atlas.largestMade(svgIndex);
    if (!best) return;
    sync(st);
    ImTextureData* t = st.tex[static_cast<size_t>(best->page)];
    const float n = static_cast<float>(st.atlas.devicePx(logicalPx));
    const float x0 = std::round(c.x * d - n * 0.5f) / d, y0 = std::round(c.y * d - n * 0.5f) / d;
    const float inv = 1.0f / static_cast<float>(Atlas::kPage), m = static_cast<float>(best->size);
    dl->AddImage(t->GetTexRef(), ImVec2(x0, y0), ImVec2(x0 + n / d, y0 + n / d), ImVec2(static_cast<float>(best->x) * inv, static_cast<float>(best->y) * inv),
                 ImVec2((static_cast<float>(best->x) + m) * inv, (static_cast<float>(best->y) + m) * inv), col);
    return;
  }
  if (!r) return;
  sync(st);
  ImTextureData* t = st.tex[static_cast<size_t>(r->page)];
  // Whole device pixels: the backend maps logical px to device px by d.
  const float n = static_cast<float>(r->size);
  const float x0 = std::round(c.x * d - n * 0.5f) / d, y0 = std::round(c.y * d - n * 0.5f) / d;
  const float inv = 1.0f / static_cast<float>(Atlas::kPage);
  dl->AddImage(t->GetTexRef(), ImVec2(x0, y0), ImVec2(x0 + n / d, y0 + n / d), ImVec2(static_cast<float>(r->x) * inv, static_cast<float>(r->y) * inv),
               ImVec2((static_cast<float>(r->x) + n) * inv, (static_cast<float>(r->y) + n) * inv), col);
}

void draw(ImDrawList* dl, const char* svgName, ImVec2 c, float logicalPx, ImU32 col) {
  if (svgName) draw(dl, state().atlas.indexOf(svgName), c, logicalPx, col);
}

namespace {
// The largest box whose ink fits `tw` x `th` device px (the search rasterises the icon at the
// sizes it tries: costly the first time an icon is asked for at a new size).
State::Fit computeFit(State& st, int svgIndex, int tw, int th, float d) {
  float box = 0, thin = 0;
  bool fits = false;
  auto ref = st.refInk.find(svgIndex);
  if (ref == st.refInk.end()) {
    int n = 0;
    const SvgSource* src = embeddedSvgs(&n);
    std::vector<uint8_t> a;
    float fx = 1, fy = 1;
    if (svgIndex < n && rasterize(src[svgIndex].svg, 48, strokePx(48, 1), a)) {
      int x0 = 48, y0 = 48, x1 = 0, y1 = 0;
      for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
          if (a[static_cast<size_t>(y) * 48 + x] >= Atlas::kInkAlpha) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
      if (x1 > x0) fx = static_cast<float>(x1 - x0) / 48.0f, fy = static_cast<float>(y1 - y0) / 48.0f;
    }
    ref = st.refInk.emplace(svgIndex, std::make_pair(fx, fy)).first;
  }
  // Largest device box whose ink fits: start one step above the estimate, step
  // down. A target too small for the stroke rule (a glyph a few pixels across,
  // a 1 px bar) is tried again with 1 device px strokes, as thin as it was.
  const int start = std::clamp(static_cast<int>(std::ceil(std::min(tw / ref->second.first, th / ref->second.second))) + 1, 3,
                               Atlas::kPage - 2 * Atlas::kPad);
  int px = start;
  for (float stroke : {0.0f, 1.0f}) {
    for (px = start; px >= 3 && !fits; --px) {
      const Atlas::Rect* r = st.atlas.get(svgIndex, static_cast<float>(px) / d, stroke);
      if (!r) break;
      fits = r->inkX1 - r->inkX0 <= tw && r->inkY1 - r->inkY0 <= th;
    }
    if (fits) ++px;
    thin = stroke;
    if (fits) break;
  }
  px = std::max(px, 3);
  box = static_cast<float>(px) / d;
  return {box, thin, fits};
}

// K-112: while deferring (a layout switch frame), a fit not worked out yet is queued and the icon
// drawn from a size already rasterised, scaled into its box, until serviceDeferred() has the
// exact one (a frame or two later, under the switch snapshot).

bool drawApprox(State& st, ImDrawList* dl, int svgIndex, float bx0, float by0, int tw, int th, float d, ImU32 col) {
  const Atlas::Rect* best = nullptr;
  for (const auto& kv : st.fits)
    if (static_cast<int>(kv.first >> 40) == svgIndex && kv.second.box > 0)
      if (const Atlas::Rect* r = st.atlas.get(svgIndex, kv.second.box, kv.second.stroke); r && (!best || r->size > best->size)) best = r;
  if (!best || best->inkX1 <= best->inkX0 || best->inkY1 <= best->inkY0) return false;
  sync(st);
  ImTextureData* t = st.tex[static_cast<size_t>(best->page)];
  const float iw = static_cast<float>(best->inkX1 - best->inkX0), ih = static_cast<float>(best->inkY1 - best->inkY0);
  const float k = std::min(static_cast<float>(tw) / iw, static_cast<float>(th) / ih);
  const float n = static_cast<float>(best->size) * k;
  const float x0 = (bx0 + (static_cast<float>(tw) - iw * k) * 0.5f - static_cast<float>(best->inkX0) * k) / d;
  const float y0 = (by0 + (static_cast<float>(th) - ih * k) * 0.5f - static_cast<float>(best->inkY0) * k) / d;
  const float inv = 1.0f / static_cast<float>(Atlas::kPage);
  dl->AddImage(t->GetTexRef(), ImVec2(x0, y0), ImVec2(x0 + n / d, y0 + n / d), ImVec2(static_cast<float>(best->x) * inv, static_cast<float>(best->y) * inv),
               ImVec2((static_cast<float>(best->x) + static_cast<float>(best->size)) * inv, (static_cast<float>(best->y) + static_cast<float>(best->size)) * inv), col);
  return true;
}

}  // namespace

bool drawFit(ImDrawList* dl, int svgIndex, ImVec2 inkMin, ImVec2 inkMax, ImU32 col, bool onlyIfFits) {
  if (!dl || svgIndex < 0) return false;
  if ((col & IM_COL32_A_MASK) == 0) return true;
  State& st = state();
  const ImGuiIO& io = ImGui::GetIO();
  const float d = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
  st.atlas.setDensity(d);
  if (st.fitsReset != st.atlas.resetCount()) {
    st.fits.clear();
    st.pending.clear();
    st.fitsReset = st.atlas.resetCount();
  }
  // Target: the device pixels the old glyph's box covers three quarters of (a
  // box's corner pixels are only partly inked by slanted or flat-ended strokes).
  // The icon's ink (every visible texel) stays inside them: never larger.
  const float bx0 = std::round(inkMin.x * d + 0.25f), by0 = std::round(inkMin.y * d + 0.25f);
  const int tw = std::max(1, static_cast<int>(std::round(inkMax.x * d - 0.25f) - bx0));
  const int th = std::max(1, static_cast<int>(std::round(inkMax.y * d - 0.25f) - by0));
  const uint64_t key = (static_cast<uint64_t>(svgIndex) << 40) | (static_cast<uint64_t>(tw) << 20) | static_cast<uint64_t>(th);
  auto it = st.fits.find(key);
  if (it == st.fits.end()) {
    if (gDefer) {
      if (std::find(st.pending.begin(), st.pending.end(), key) == st.pending.end()) st.pending.push_back(key);
      const bool drawn = drawApprox(st, dl, svgIndex, bx0, by0, tw, th, d, col);
      return drawn || !onlyIfFits;
    }
    it = st.fits.emplace(key, computeFit(st, svgIndex, tw, th, d)).first;
  }
  const float box = it->second.box, thin = it->second.stroke;
  const bool fits = it->second.fits;
  if (!fits && onlyIfFits) return false;
  const Atlas::Rect* r = st.atlas.get(svgIndex, box, thin);
  if (!r) return false;
  sync(st);
  ImTextureData* t = st.tex[static_cast<size_t>(r->page)];
  const float n = static_cast<float>(r->size);
  // The ink centred in the target pixels, on whole device pixels.
  const float x0 = (bx0 + static_cast<float>((tw - (r->inkX1 - r->inkX0)) / 2 - r->inkX0)) / d;
  const float y0 = (by0 + static_cast<float>((th - (r->inkY1 - r->inkY0)) / 2 - r->inkY0)) / d;
  const float inv = 1.0f / static_cast<float>(Atlas::kPage);
  dl->AddImage(t->GetTexRef(), ImVec2(x0, y0), ImVec2(x0 + n / d, y0 + n / d), ImVec2(static_cast<float>(r->x) * inv, static_cast<float>(r->y) * inv),
               ImVec2((static_cast<float>(r->x) + n) * inv, (static_cast<float>(r->y) + n) * inv), col);
  return true;
}

void setDeferFits(bool on) { gDefer = on; }

int serviceDeferred(double budgetMs) {
  State& st = state();
  const auto t0 = std::chrono::steady_clock::now();
  int done = 0;
  // K-291: the draw() sizes a switch frame put off, a few a frame (the next draw finds them made).
  while (!st.pendingDraws.empty()) {
    const std::pair<int, float> w = st.pendingDraws.front();
    st.pendingDraws.erase(st.pendingDraws.begin());
    if (!st.atlas.find(w.first, w.second)) st.atlas.get(w.first, w.second), ++done;
    if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= budgetMs) return done;
  }
  if (st.pending.empty() || st.fitsReset != st.atlas.resetCount()) {
    if (st.fitsReset != st.atlas.resetCount()) st.pending.clear();
    return done;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const float d = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
  while (!st.pending.empty()) {
    const uint64_t key = st.pending.front();
    st.pending.erase(st.pending.begin());
    if (!st.fits.count(key)) {
      const int svg = static_cast<int>(key >> 40), tw = static_cast<int>((key >> 20) & 0xFFFFF), th = static_cast<int>(key & 0xFFFFF);
      st.fits.emplace(key, computeFit(st, svg, tw, th, d));
      ++done;
    }
    if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= budgetMs) break;
  }
  return done;
}

int deferredCount() { return static_cast<int>(state().pending.size() + state().pendingDraws.size()); }

bool glyphInk(ImFont* font, float size, ImWchar ch, ImVec2 pos, ImVec2* inkMin, ImVec2* inkMax) {
  ImFontBaked* baked = font ? font->GetFontBaked(size) : nullptr;
  const ImFontGlyph* g = baked ? baked->FindGlyph(ch) : nullptr;
  if (!g || !g->Visible) return false;
  const float k = size / baked->Size;
  ImVec2 mn(pos.x + g->X0 * k, pos.y + g->Y0 * k), mx(pos.x + g->X1 * k, pos.y + g->Y1 * k);
  ImTextureData* tex = font->OwnerAtlas ? font->OwnerAtlas->TexData : nullptr;
  if (tex && tex->Pixels && tex->Width > 0 && tex->Height > 0) {
    // Texels at least half covered, mapped back onto the glyph's quad.
    const int tx0 = static_cast<int>(std::lround(g->U0 * static_cast<float>(tex->Width)));
    const int ty0 = static_cast<int>(std::lround(g->V0 * static_cast<float>(tex->Height)));
    const int tx1 = static_cast<int>(std::lround(g->U1 * static_cast<float>(tex->Width)));
    const int ty1 = static_cast<int>(std::lround(g->V1 * static_cast<float>(tex->Height)));
    int ix0 = tx1, iy0 = ty1, ix1 = tx0, iy1 = ty0;
    for (int y = std::max(0, ty0); y < std::min(ty1, tex->Height); ++y)
      for (int x = std::max(0, tx0); x < std::min(tx1, tex->Width); ++x) {
        const unsigned char* p = static_cast<const unsigned char*>(tex->GetPixelsAt(x, y));
        const int a = tex->Format == ImTextureFormat_Alpha8 ? p[0] : p[3];
        if (a >= 128) ix0 = std::min(ix0, x), iy0 = std::min(iy0, y), ix1 = std::max(ix1, x + 1), iy1 = std::max(iy1, y + 1);
      }
    if (ix1 > ix0 && tx1 > tx0 && ty1 > ty0) {
      const float sx = (mx.x - mn.x) / static_cast<float>(tx1 - tx0), sy = (mx.y - mn.y) / static_cast<float>(ty1 - ty0);
      const ImVec2 a(mn.x + static_cast<float>(ix0 - tx0) * sx, mn.y + static_cast<float>(iy0 - ty0) * sy);
      mx = ImVec2(mn.x + static_cast<float>(ix1 - tx0) * sx, mn.y + static_cast<float>(iy1 - ty0) * sy);
      mn = a;
    }
  }
  *inkMin = mn, *inkMax = mx;
  return true;
}

bool drawFit(ImDrawList* dl, const char* svgName, ImVec2 inkMin, ImVec2 inkMax, ImU32 col, bool onlyIfFits) {
  return svgName && drawFit(dl, state().atlas.indexOf(svgName), inkMin, inkMax, col, onlyIfFits);
}

void drawGlyph(ImDrawList* dl, const char* svgName, ImVec2 c, float unitPx, float oldStroke, ImU32 col) {
  if (!dl || !svgName) return;
  State& st = state();
  const int idx = st.atlas.indexOf(svgName);
  if (idx < 0) return;
  auto it = st.geom.find(idx);
  if (it == st.geom.end()) {
    std::array<float, 5> g{2, 2, 22, 22, 1};
    float b[4];
    bool stroked = true;
    int n = 0;
    const SvgSource* src = embeddedSvgs(&n);
    if (idx < n && geometryBounds(src[idx].svg, b, &stroked)) g = {b[0], b[1], b[2], b[3], stroked ? 1.0f : 0.0f};
    it = st.geom.emplace(idx, g).first;
  }
  const auto& g = it->second;
  // Old strokes reach half their width past round ends and none past flat ones: take
  // 0.35 of it, so the box is at most a third of a pixel generous on flat ends.
  const float k = unitPx / 10.0f, h = g[4] > 0 ? oldStroke * 0.35f : 0.0f;
  drawFit(dl, idx, ImVec2(c.x + (g[0] - 12) * k - h, c.y + (g[1] - 12) * k - h), ImVec2(c.x + (g[2] - 12) * k + h, c.y + (g[3] - 12) * k + h), col);
}

}  // namespace gl::icons

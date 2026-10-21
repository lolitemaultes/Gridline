// Gridline — Q-13 interface icons on screen (see IconTexture.h).
#include "IconTexture.h"
#include <algorithm>
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
};

std::unordered_map<ImGuiContext*, std::unique_ptr<State>>& states() {
  static std::unordered_map<ImGuiContext*, std::unique_ptr<State>> m;
  return m;
}

State& state() {
  auto& p = states()[ImGui::GetCurrentContext()];
  if (!p) p = std::make_unique<State>();
  return *p;
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

void draw(ImDrawList* dl, int svgIndex, ImVec2 c, float logicalPx, ImU32 col) {
  if (!dl || svgIndex < 0 || (col & IM_COL32_A_MASK) == 0) return;
  State& st = state();
  const ImGuiIO& io = ImGui::GetIO();
  const float d = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
  st.atlas.setDensity(d);
  const Atlas::Rect* r = st.atlas.get(svgIndex, logicalPx);
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

}  // namespace gl::icons

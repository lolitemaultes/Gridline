// Gridline — the Tracks layout's (Smooth's) radio buttons and progress bars, drawn headless and rasterised: a selected
// radio is filled with the layout's blue and carries a white dot, an unselected one is the plain field,
// in the light and dark themes, at 100% and 200%; outside the layout ImGui's own radio comes back.
// (The bug: Smooth's white check mark drew the selected dot white on the light theme's white field, so
// no option ever looked selected.)
#define IMGUI_DEFINE_MATH_OPERATORS
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "Check.h"
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_internal.h"
#include "app/SmoothToggles.h"
#include "app/Theme.h"

using namespace gl;

namespace {

struct Px {
  float r = 0, g = 0, b = 0;
};

// The frame's draw data rasterised at `scale` (as the window's framebuffer at that interface scale),
// over a white-free grey so a white mark shows against it; untextured triangles only (no text drawn).
struct Raster {
  int w = 0, h = 0;
  float scale = 1;
  std::vector<Px> px;
  Px at(float x, float y) const {
    const int ix = std::clamp(int(std::floor(x * scale)), 0, w - 1), iy = std::clamp(int(std::floor(y * scale)), 0, h - 1);
    return px[size_t(iy * w + ix)];
  }
};

Raster raster(const ImDrawData* dd, float scale, int lw, int lh) {
  Raster r;
  r.scale = scale, r.w = int(lw * scale), r.h = int(lh * scale);
  r.px.assign(size_t(r.w * r.h), Px{0.5f, 0.5f, 0.5f});
  for (const ImDrawList* dl : dd->CmdLists) {
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
      if (cmd.UserCallback) continue;
      for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
        const ImDrawVert* v[3];
        for (int k = 0; k < 3; ++k) v[k] = &dl->VtxBuffer[int(cmd.VtxOffset + dl->IdxBuffer[int(cmd.IdxOffset + i + unsigned(k))])];
        const float x0 = v[0]->pos.x * scale, y0 = v[0]->pos.y * scale, x1 = v[1]->pos.x * scale, y1 = v[1]->pos.y * scale,
                    x2 = v[2]->pos.x * scale, y2 = v[2]->pos.y * scale;
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(area) < 1e-6f) continue;
        const int bx0 = std::max(0, int(std::floor(std::min({x0, x1, x2})))), bx1 = std::min(r.w - 1, int(std::ceil(std::max({x0, x1, x2}))));
        const int by0 = std::max(0, int(std::floor(std::min({y0, y1, y2})))), by1 = std::min(r.h - 1, int(std::ceil(std::max({y0, y1, y2}))));
        for (int y = by0; y <= by1; ++y)
          for (int x = bx0; x <= bx1; ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float a = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
            const float b = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
            const float c = 1.0f - a - b;
            if (a < 0 || b < 0 || c < 0) continue;
            const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(v[0]->col), cb = ImGui::ColorConvertU32ToFloat4(v[1]->col),
                         cc = ImGui::ColorConvertU32ToFloat4(v[2]->col);
            const float al = a * ca.w + b * cb.w + c * cc.w;
            Px& d = r.px[size_t(y * r.w + x)];
            d.r = d.r * (1 - al) + (a * ca.x + b * cb.x + c * cc.x) * al;
            d.g = d.g * (1 - al) + (a * ca.y + b * cb.y + c * cc.y) * al;
            d.b = d.b * (1 - al) + (a * ca.z + b * cb.z + c * cc.z) * al;
          }
      }
    }
  }
  return r;
}

float dist(Px p, ImU32 c) {
  const ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
  return std::max({std::fabs(p.r - v.x), std::fabs(p.g - v.y), std::fabs(p.b - v.z)});
}
float dist(Px p, ImVec4 v) { return std::max({std::fabs(p.r - v.x), std::fabs(p.g - v.y), std::fabs(p.b - v.z)}); }

struct Drawn {
  ImRect on, off;  // the selected radio's circle box, the unselected one's
  ImRect bar;      // a progress bar at 40%
};

Drawn frame() {
  Drawn d;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 200);
  io.DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(10, 10));
  ImGui::SetNextWindowSize(ImVec2(300, 150));
  ImGui::Begin("radios", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
  static int pick = 0;
  ImGui::RadioButton("##a", &pick, 0);
  d.on = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMin() + ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
  ImGui::RadioButton("##b", &pick, 1);
  d.off = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMin() + ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
  ImGui::ProgressBar(0.4f, ImVec2(200, 0), "");
  d.bar = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  ImGui::End();
  ImGui::Render();
  return d;
}

void check(bool dark, float scale) {
  theme::apply(dark);
  ImGuiStyle& st = ImGui::GetStyle();
  smoothtoggles::Saved saved;
  smoothtoggles::apply(st, saved);
  const ImVec4 field = st.Colors[ImGuiCol_FrameBg], white = st.Colors[ImGuiCol_CheckMark];
  frame();
  const Drawn d = frame();  // (the second frame: the items have settled)
  const Raster r = raster(ImGui::GetDrawData(), scale, 400, 200);
  const ImVec2 c = d.on.GetCenter(), c2 = d.off.GetCenter();
  const float rad = (d.on.GetHeight() - 1.0f) * 0.5f;
  char b[200];
  const char* tag = dark ? "dark" : "light";
  const Px ring = r.at(c.x + rad * 0.7f, c.y), dot = r.at(c.x, c.y);
  std::snprintf(b, sizeof b, "Smooth %s %.0f%%: the selected radio is filled with the blue (ring %.2f %.2f %.2f)", tag, scale * 100, ring.r, ring.g,
                ring.b);
  expect(dist(ring, smoothtoggles::accent()) < 0.04f, b);
  std::snprintf(b, sizeof b, "Smooth %s %.0f%%: ... with a white dot in its middle (%.2f %.2f %.2f)", tag, scale * 100, dot.r, dot.g, dot.b);
  expect(dist(dot, white) < 0.04f && dist(dot, smoothtoggles::accent()) > 0.3f, b);
  const Px offRing = r.at(c2.x + rad * 0.7f, c2.y), offDot = r.at(c2.x, c2.y);
  std::snprintf(b, sizeof b, "Smooth %s %.0f%%: the unselected radio is the plain field, no blue, no dot", tag, scale * 100);
  expect(dist(offRing, field) < 0.04f && dist(offDot, field) < 0.04f && dist(offDot, smoothtoggles::accent()) > 0.3f, b);
  const ImVec2 b0 = d.bar.Min, b1 = d.bar.Max;
  const Px filled = r.at(b0.x + (b1.x - b0.x) * 0.2f, (b0.y + b1.y) * 0.5f), empty = r.at(b0.x + (b1.x - b0.x) * 0.8f, (b0.y + b1.y) * 0.5f);
  std::snprintf(b, sizeof b, "Smooth %s %.0f%%: a progress bar fills with the blue (%.2f %.2f %.2f), the rest is the field", tag, scale * 100, filled.r,
                filled.g, filled.b);
  expect(dist(filled, smoothtoggles::accent()) < 0.04f && dist(empty, field) < 0.04f, b);
  // outside the layout (restore): ImGui's own radio, the theme's mark on the field
  smoothtoggles::restore(st, saved);
  frame();
  const Drawn e = frame();
  const Raster q = raster(ImGui::GetDrawData(), scale, 400, 200);
  const Px oField = q.at(e.off.GetCenter().x, e.off.GetCenter().y), oDot = q.at(e.on.GetCenter().x, e.on.GetCenter().y);  // (unselected: the field)
  std::snprintf(b, sizeof b, "other layouts (%s %.0f%%): the radio is ImGui's own again, the theme's mark on the field (dot %.2f %.2f %.2f, field %.2f %.2f %.2f)",
                tag, scale * 100, oDot.r, oDot.g, oDot.b, oField.r, oField.g, oField.b);
  expect(dist(oDot, st.Colors[ImGuiCol_CheckMark]) < 0.04f && dist(oField, st.Colors[ImGuiCol_FrameBg]) < 0.04f && GImGuiGridlineRadioDot < 0, b);
  const Px oBar = q.at(e.bar.Min.x + (e.bar.Max.x - e.bar.Min.x) * 0.2f, (e.bar.Min.y + e.bar.Max.y) * 0.5f);
  std::snprintf(b, sizeof b, "other layouts (%s %.0f%%): a progress bar keeps the theme's own fill", tag, scale * 100);
  expect(dist(oBar, st.Colors[ImGuiCol_PlotHistogram]) < 0.04f, b);
}

}  // namespace

int main() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // headless: textures are never uploaded
  io.Fonts->AddFontDefault();
  for (bool dark : {false, true})
    for (float scale : {1.0f, 2.0f}) check(dark, scale);
  ImGui::DestroyContext();
  return finish();
}

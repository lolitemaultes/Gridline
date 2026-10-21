// Gridline — splash screen (logo guide §08: lockup, Paper on surface-950, 56 px)
// with a loading bar driven by the real start-up steps.
#include "GridlineVersion.h"
#include <algorithm>
#include <cmath>
#include "../core/I18n.h"
#include "../../third_party/imgui/imgui.h"
#include "Theme.h"

namespace gl {

void drawSplash(ImFont* bold, ImFont* mono, float progress, const char* status, double time) {
  const auto& P = theme::pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::rgb(0x0B0B0B)));
  ImGui::Begin("##splash", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 paper = theme::rgb(0xFFFFFF), bg = theme::rgb(0x0B0B0B);
  ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

  // Lockup, centred, 56 px tall (measure first, then draw).
  const float h = 56.0f;
  float gap = h * 0.2f, fs = h * 0.86f;
  ImVec2 ts = bold ? bold->CalcTextSizeA(fs, 1e9f, 0, "gridline") : ImVec2(fs * 4, fs);
  float w = h + gap + ts.x;
  float fade = static_cast<float>(std::clamp(time / 0.35, 0.0, 1.0));
  theme::drawLockup(dl, ImVec2(c.x - w * 0.5f, c.y - h * 0.5f - 34), h, paper, bg, bold, fade);

  // Tagline + version (mono, faint).
  ImGui::PushFont(mono, 11.0f);
  const char* tag = "THE GRID, CUT CLEAN";
  ImVec2 tsz = ImGui::CalcTextSize(tag);
  dl->AddText(ImVec2(c.x - tsz.x * 0.5f, c.y + 18), theme::withAlpha(theme::rgb(0x8A8A8A), fade), tag);

  // Loading bar.
  const float barW = 260, barH = 3;
  ImVec2 b0(c.x - barW * 0.5f, c.y + 62);
  dl->AddRectFilled(b0, ImVec2(b0.x + barW, b0.y + barH), theme::rgb(0x262626), barH);
  dl->AddRectFilled(b0, ImVec2(b0.x + barW * std::clamp(progress, 0.0f, 1.0f), b0.y + barH), paper, barH);
  ImVec2 ssz = ImGui::CalcTextSize(status);
  dl->AddText(ImVec2(c.x - ssz.x * 0.5f, b0.y + 12), theme::rgb(0x6B6B6B), status);

  const char* ver = "Version " GRIDLINE_VERSION;
  ImVec2 vsz = ImGui::CalcTextSize(ver);
  dl->AddText(ImVec2(vp->Pos.x + vp->Size.x - vsz.x - 16, vp->Pos.y + vp->Size.y - vsz.y - 14), theme::rgb(0x4A4A4A), ver);
  dl->AddText(ImVec2(vp->Pos.x + 16, vp->Pos.y + vp->Size.y - vsz.y - 14), theme::rgb(0x4A4A4A), i18n::tr("Free and open for everybody"));
  ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  (void)P;
}

}  // namespace gl

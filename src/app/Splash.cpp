// Gridline — splash screen (logo guide §08: lockup, Paper on surface-950, 56 px)
// with a loading bar driven by the real start-up steps.
#include "Splash.h"
#include "GridlineVersion.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../core/I18n.h"
#include "../../third_party/imgui/imgui.h"
#include "Theme.h"

namespace gl {

namespace {
// The brand's Paper (logo guide §08): the splash and the start screen's update view are always drawn
// in it on the brand black, whatever the theme.
ImU32 brandPaper() { return theme::rgb(0xFFFFFF); }
}  // namespace

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
  const ImU32 paper = brandPaper(), bg = theme::rgb(0x0B0B0B);
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

// ---- automatic update states (SplashUpdate.cpp drives them) ----
namespace {

std::string megabytes(int64_t b) {
  char t[32];
  std::snprintf(t, sizeof t, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
  return t;
}

}  // namespace

void drawSplashUpdate(ImFont* bold, ImFont* mono, const SplashUpdateView& v, double time) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::rgb(0x0B0B0B)));
  ImGui::Begin("##splash", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 paper = brandPaper(), bg = theme::rgb(0x0B0B0B);
  const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
  const float h = 56.0f, gap = h * 0.2f, fs = h * 0.86f;
  const ImVec2 ts = bold ? bold->CalcTextSizeA(fs, 1e9f, 0, "gridline") : ImVec2(fs * 4, fs);
  const float w = h + gap + ts.x;
  theme::drawLockup(dl, ImVec2(c.x - w * 0.5f, c.y - h * 0.5f - 34), h, paper, bg, bold, 1.0f);
  ImGui::PushFont(mono, 11.0f);
  const char* tag = "THE GRID, CUT CLEAN";
  const ImVec2 tsz = ImGui::CalcTextSize(tag);
  dl->AddText(ImVec2(c.x - tsz.x * 0.5f, c.y + 18), theme::rgb(0x8A8A8A), tag);

  // The bar: the download's share, or the start-up progress while checking.
  const float barW = 260, barH = 3;
  const ImVec2 b0(c.x - barW * 0.5f, c.y + 62);
  float f = std::clamp(v.progress, 0.0f, 1.0f);
  dl->AddRectFilled(b0, ImVec2(b0.x + barW, b0.y + barH), theme::rgb(0x262626), barH);
  if (v.state == SplashUpdateView::Checking) {
    // A short segment sweeping across: working, no amount known.
    const float seg = 56.0f, t = static_cast<float>(std::fmod(time * 0.9, 1.0));
    const float x0 = b0.x + (barW + seg) * t - seg;
    dl->AddRectFilled(ImVec2(std::max(b0.x, x0), b0.y), ImVec2(std::min(b0.x + barW, x0 + seg), b0.y + barH), paper, barH);
  } else {
    dl->AddRectFilled(b0, ImVec2(b0.x + barW * f, b0.y + barH), paper, barH);
  }
  char line[256];
  std::string status, detail;
  const char* ver = v.version.c_str();
  switch (v.state) {
    case SplashUpdateView::Checking: status = i18n::tr("Checking for updates"); break;
    case SplashUpdateView::Downloading: {
      std::snprintf(line, sizeof line, i18n::tr("Downloading Gridline %s"), ver);
      status = line;
      if (v.total > 0) {
        std::snprintf(line, sizeof line, i18n::tr("%d%%  \xC2\xB7  %s of %s"), static_cast<int>(std::floor(f * 100.0f)), megabytes(v.got).c_str(),
                      megabytes(v.total).c_str());
        detail = line;
      }
      break;
    }
    case SplashUpdateView::Verifying: status = i18n::tr("Checking the download"); break;
    case SplashUpdateView::Installing:
      std::snprintf(line, sizeof line, i18n::tr("Installing Gridline %s"), ver);
      status = line;
      break;
    case SplashUpdateView::Permission:  // the system's own prompt is up (or comes as Gridline closes)
      std::snprintf(line, sizeof line, v.windows ? i18n::tr("Installing Gridline %s: Windows will ask for permission")
                                                 : i18n::tr("Installing Gridline %s: your system will ask for your password"),
                    ver);
      status = line;
      break;
    case SplashUpdateView::Restarting:
      std::snprintf(line, sizeof line, i18n::tr("Restarting into Gridline %s"), ver);
      status = line;
      break;
    case SplashUpdateView::Failed:
      std::snprintf(line, sizeof line, i18n::tr("Couldn't update. Starting Gridline %s"), GRIDLINE_VERSION);
      status = line;
      break;
    case SplashUpdateView::Background: status = i18n::tr("The update continues in the background"); break;
  }
  const ImVec2 ssz = ImGui::CalcTextSize(status.c_str());
  dl->AddText(ImVec2(std::floor(c.x - ssz.x * 0.5f), b0.y + 12), theme::withAlpha(paper, 0.74f), status.c_str());
  if (!detail.empty()) {
    const ImVec2 dsz = ImGui::CalcTextSize(detail.c_str());
    dl->AddText(ImVec2(std::floor(c.x - dsz.x * 0.5f), b0.y + 30), theme::rgb(0x6B6B6B), detail.c_str());
  }
  const char* verText = "Version " GRIDLINE_VERSION;
  const ImVec2 vsz = ImGui::CalcTextSize(verText);
  dl->AddText(ImVec2(vp->Pos.x + vp->Size.x - vsz.x - 16, vp->Pos.y + vp->Size.y - vsz.y - 14), theme::rgb(0x4A4A4A), verText);
  dl->AddText(ImVec2(vp->Pos.x + 16, vp->Pos.y + vp->Size.y - vsz.y - 14), theme::rgb(0x4A4A4A), i18n::tr("Free and open for everybody"));
  ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

}  // namespace gl

// Gridline - the layout style report (K-112). See LayoutReport.h.
#include "LayoutReport.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/LayoutPreset.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LayoutStyleUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::layoutui {

namespace {

void line(std::string& out, const char* fmt, ...) IM_FMTARGS(2);
void line(std::string& out, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  out += buf;
  out += '\n';
}

uint32_t fnv(const void* data, size_t n, uint32_t h = 2166136261u) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

void dumpNode(std::string& out, const ImGuiDockNode* n, int depth) {
  if (!n) return;
  std::string windows;
  for (const ImGuiWindow* w : n->Windows) windows += std::string(windows.empty() ? "" : ",") + ui::stableWindowName(w->Name);
  line(out, "dock %*s%s pos %.0f %.0f size %.0f %.0f split %d central %d windows [%s]", depth * 2, "", n->IsLeafNode() ? "leaf" : "split",
       static_cast<double>(n->Pos.x), static_cast<double>(n->Pos.y), static_cast<double>(n->Size.x), static_cast<double>(n->Size.y),
       n->IsLeafNode() ? -1 : static_cast<int>(n->SplitAxis), n->IsCentralNode() ? 1 : 0, windows.c_str());
  dumpNode(out, n->ChildNodes[0], depth + 1);
  dumpNode(out, n->ChildNodes[1], depth + 1);
}

}  // namespace

std::string styleReport(App& a) {
  std::string out;
  const ImGuiStyle& s = ImGui::GetStyle();
  line(out, "layout %s menus %s", layout::id(active()), layout::id(menuStyle(a)));
  line(out, "bars top %.1f status %.1f", static_cast<double>(theme::kTopBarH), static_cast<double>(theme::kStatusBarH));
  if (styled()) out += layout::describe(style());  // K-112: a layout style is applied (never in Studio)
  const float f[] = {s.Alpha, s.DisabledAlpha, s.WindowPadding.x, s.WindowPadding.y, s.WindowRounding, s.WindowBorderSize,
                     s.WindowMinSize.x, s.WindowMinSize.y, s.WindowTitleAlign.x, s.ChildRounding, s.ChildBorderSize, s.PopupRounding,
                     s.PopupBorderSize, s.FramePadding.x, s.FramePadding.y, s.FrameRounding, s.FrameBorderSize, s.ItemSpacing.x,
                     s.ItemSpacing.y, s.ItemInnerSpacing.x, s.ItemInnerSpacing.y, s.CellPadding.x, s.CellPadding.y, s.IndentSpacing,
                     s.ScrollbarSize, s.ScrollbarRounding, s.GrabMinSize, s.GrabRounding, s.TabRounding, s.TabBorderSize,
                     s.SeparatorTextBorderSize, s.DockingSeparatorSize, s.FontSizeBase, s.FontScaleMain};
  std::string sizes;
  for (float v : f) {
    char b[24];
    std::snprintf(b, sizeof b, "%s%.2f", sizes.empty() ? "" : " ", static_cast<double>(v));
    sizes += b;
  }
  line(out, "style %s", sizes.c_str());
  line(out, "colours %08x", fnv(s.Colors, sizeof s.Colors));
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  line(out, "viewport %.0f %.0f work %.0f %.0f %.0f %.0f", static_cast<double>(vp->Size.x), static_cast<double>(vp->Size.y),
       static_cast<double>(vp->WorkPos.x), static_cast<double>(vp->WorkPos.y), static_cast<double>(vp->WorkSize.x), static_cast<double>(vp->WorkSize.y));
  if (ImGuiWindow* host = ImGui::FindWindowByName("##dockhost")) {
    line(out, "dockhost pos %.0f %.0f size %.0f %.0f", static_cast<double>(host->Pos.x), static_cast<double>(host->Pos.y),
         static_cast<double>(host->Size.x), static_cast<double>(host->Size.y));
    dumpNode(out, ImGui::DockBuilderGetNode(host->GetID("GridlineDock")), 0);
  }
  line(out, "open %s", openWindows(a).c_str());
  if (shellBuilt(a, active())) out += shellReport(a);
  return out;
}

}  // namespace gl::layoutui

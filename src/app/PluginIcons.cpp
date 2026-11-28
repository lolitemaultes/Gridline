// Gridline — K-450 plugin icons (see PluginIcons.h).
#include "PluginIcons.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include "../../third_party/imgui/imgui_internal.h"
#include "Theme.h"
#include "Widgets.h"
#include "icons/IconAtlas.h"
#include "icons/IconTexture.h"

namespace gl::ui {

namespace {

struct Entry {
  std::string name;  // "plugin-<id>"
  ImU32 colour = 0;  // the tile's authored colour (dark faces)
};

// type -> icon, built once from the embedded SVGs.
const std::unordered_map<std::string, Entry>& table() {
  static const std::unordered_map<std::string, Entry> m = [] {
    std::unordered_map<std::string, Entry> r;
    int n = 0;
    const icons::SvgSource* s = icons::embeddedSvgs(&n);
    for (int i = 0; i < n; ++i) {
      if (std::strncmp(s[i].name, "plugin-", 7) != 0) continue;
      Entry e;
      e.name = s[i].name;
      const char* p = std::strstr(s[i].svg, "id=\"mark");
      const char* f = p ? std::strstr(p, "fill=\"#") : nullptr;
      if (f) {
        const unsigned long v = std::strtoul(f + 7, nullptr, 16);
        e.colour = IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 255);
      }
      r.emplace(std::string("gl.") + (s[i].name + 7), e);
    }
    return r;
  }();
  return m;
}

PluginIconCounts gCounts;

}  // namespace

const char* pluginIconSurfaceName(PluginIconSurface s) {
  switch (s) {
    case PluginIconSurface::Browser: return "browser";
    case PluginIconSurface::Picker: return "picker";
    case PluginIconSurface::Rack: return "rack";
    case PluginIconSurface::WindowTitle: return "window";
    case PluginIconSurface::MixerSlot: return "mixer";
    case PluginIconSurface::DeviceBand: return "deviceband";
    case PluginIconSurface::Smooth: return "smooth";
    case PluginIconSurface::Tracker: return "tracker";
    case PluginIconSurface::Palette: return "palette";
    case PluginIconSurface::Drag: return "drag";
    case PluginIconSurface::Menu: return "menu";
    case PluginIconSurface::Count: break;
  }
  return "?";
}

const char* pluginIconName(const std::string& type) {
  const auto& t = table();
  auto it = t.find(type);
  return it == t.end() ? nullptr : it->second.name.c_str();
}

ImU32 pluginIconColour(const std::string& type) {
  const auto& t = table();
  auto it = t.find(type);
  if (it == t.end() || !it->second.colour) return 0;
  return theme::lit(it->second.colour);
}

bool drawPluginIcon(ImDrawList* dl, const std::string& type, ImVec2 c, float size, PluginIconSurface s, float alpha, ImU32 tint) {
  const auto& t = table();
  auto it = t.find(type);
  if (it == t.end()) return false;
  if (dl && size > 0) {
    ImU32 col = tint ? tint : pluginIconColour(type);
    if (alpha < 1.0f) col = theme::withAlpha(col, std::max(0.0f, alpha));
    icons::draw(dl, it->second.name.c_str(), c, std::round(size), col);
  }
  ++gCounts.own[static_cast<int>(s)];
  return true;
}

void notePluginIconFallback(const std::string& type, PluginIconSurface s) {
  ++gCounts.generic[static_cast<int>(s)];
  if (hasPluginIcon(type)) ++gCounts.mistakes[static_cast<int>(s)];
}

namespace {
const char kTitlePad[] = "      ";  // the room for the icon at the start of a window title
}  // namespace

std::string pluginWindowTitle(const std::string& type, const std::string& title) {
  return hasPluginIcon(type) ? kTitlePad + title : title;
}

void drawPluginWindowTitleIcon(const std::string& type) {
  if (!hasPluginIcon(type)) return;
  ImGuiContext& g = *GImGui;
  ImGuiWindow* w = g.CurrentWindow;
  if (!w) return;
  ImRect r;
  ImDrawList* dl = nullptr;
  if (w->DockIsActive) {
    ImGuiDockNode* n = w->DockNode;
    // Every tab of the node, the selected one and the others (their windows' Begin returns false
    // but still sets the tab's rect this frame); a tab scrolled out of the bar draws nothing.
    if (!n || n->IsHiddenTabBar() || n->IsNoTabBar() || !n->HostWindow || !n->TabBar) return;
    r = w->DC.DockTabItemRect;
    if (!(w->DC.DockTabItemStatusFlags & ImGuiItemStatusFlags_Visible)) return;
    r.ClipWithFull(n->TabBar->BarRect);
    dl = n->HostWindow->DrawList;
  } else {
    if (w->Flags & ImGuiWindowFlags_NoTitleBar) return;
    r = w->TitleBarRect();
    dl = w->DrawList;
  }
  if (!dl || r.GetWidth() <= 0 || r.GetHeight() <= 0) return;
  // ImGui sets the title (and a tab's label) from the frame padding on: the pad's spaces are there.
  const float span = ImGui::CalcTextSize(kTitlePad).x;
  const float size = std::min(14.0f, std::floor(r.GetHeight() - 6.0f));
  if (size < 8 || r.GetWidth() < span + 24) return;
  // Where the title starts: after the frame padding, or (a centred title, as in Smooth) where ImGui
  // centres the whole title, pad included, on the bar (its side pads are kept even).
  float x0 = r.Min.x + g.Style.FramePadding.x;
  const float align = w->DockIsActive ? 0.0f : g.Style.WindowTitleAlign.x;
  if (align > 0.0f) {
    const float textW = ImGui::CalcTextSize(w->Name, ImGui::FindRenderedTextEnd(w->Name)).x;
    x0 = std::max(x0, r.Min.x + (r.GetWidth() - textW) * align);
  }
  const ImVec2 c(x0 + (span - 4.0f) * 0.5f, r.GetCenter().y);
  dl->PushClipRect(r.Min, r.Max, false);  // the title bar is outside the window's content clip
  drawPluginIcon(dl, type, c, size, PluginIconSurface::WindowTitle);
  dl->PopClipRect();
}

const PluginIconCounts& pluginIconCounts() { return gCounts; }
void resetPluginIconCounts() { gCounts = PluginIconCounts{}; }

bool pluginTextButton(const std::string& type, const std::string& name, const char* id, bool active, const char* hint, PluginIconSurface s) {
  if (!hasPluginIcon(type)) {
    notePluginIconFallback(type, s);
    return TextButton((name + id).c_str(), active, 0, hint);
  }
  // The label keeps the icon's room in front of the name (the same pad as the window titles).
  const bool r = TextButton((kTitlePad + name + id).c_str(), active, 0, hint);
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const float span = ImGui::CalcTextSize(kTitlePad).x;
  const float size = std::min(14.0f, std::floor(b.y - a.y - 6.0f));
  if (size >= 8) {
    // The label is centred in the button: its pad starts where the text does.
    const float textW = ImGui::CalcTextSize((kTitlePad + name).c_str()).x;
    const float x0 = a.x + std::max(0.0f, (b.x - a.x - textW) * 0.5f);
    drawPluginIcon(ImGui::GetWindowDrawList(), type, ImVec2(x0 + (span - 4.0f) * 0.5f, (a.y + b.y) * 0.5f), size, s);
  }
  return r;
}

}  // namespace gl::ui

// Gridline - K-19: plugin editor zoom, see EditorZoomUi.h.
#include "EditorZoomUi.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/EditorZoom.h"
#include "../core/I18n.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::edzoomui {
namespace {

struct Slot {
  std::string type;
  bool zoomable = true;
  float contentW = 0;
  ImVec2 design{0, 0};  // the content's size at 100 % (logical px), last frame
  float z = 1;          // this frame's zoom
  float autoZ = 0;      // the automatic zoom picked when the window appeared (0 = not yet)
  int lastFrame = -10;  // last frame the window was drawn
  int shownFrames = 0;  // frames since it appeared
  int recentre = 0;     // frames left to hold the window's centre at `centre` (it settles on a new size)
  ImVec2 centre{0, 0};
  // this frame's magnified region
  bool active = false;
  ImGuiWindow* host = nullptr;
  ImGuiWindow* child = nullptr;
  ImVec2 origin{0, 0};
  ImRect clip;
  ImVec2 savedFb{1, 1};
  ImDrawList* fg = nullptr;
  int fgStart = 0;
  // drag-resizing with the corner grip
  bool dragging = false;
  float dragZ0 = 1, liveZ = 1;
  ImVec2 dragM0{0, 0}, dragW0{1, 1};
};

std::map<std::string, Slot>& slots() {
  static std::map<std::string, Slot> m;
  return m;
}
std::map<std::string, ImVec2>& designCache() {  // per plugin type: the last design size (opens at once at its zoom)
  static std::map<std::string, ImVec2> m;
  return m;
}

// Family windows of last frame's magnified regions (for the pointer): the window, its
// region's origin and zoom, and the clip of the region (inside: child windows) or none (popups).
struct Member {
  ImVec2 origin;
  float z = 1;
  bool inside = true;
  ImRect clip;
};
std::unordered_map<const ImGuiWindow*, Member>& members() {
  static std::unordered_map<const ImGuiWindow*, Member> m;
  return m;
}

float rememberedZoom(App& a, const std::string& type) { return edzoom::zoomFor(a.settings.getString("ui.editorZoom"), type); }
void remember(App& a, const std::string& type, float z) { a.settings.set("ui.editorZoom", edzoom::withZoom(a.settings.getString("ui.editorZoom"), type, z)); }

ImVec2 chrome() {
  const ImGuiStyle& st = ImGui::GetStyle();
  return ImVec2(2 * st.WindowPadding.x + st.ScrollbarSize, 2 * st.WindowPadding.y + ImGui::GetFrameHeight());
}

ImVec2 mapIn(const Member& m, ImVec2 q) { return ImVec2(edzoom::toEditor(m.origin.x, q.x, m.z), edzoom::toEditor(m.origin.y, q.y, m.z)); }
ImVec2 mapOut(ImVec2 o, float z, ImVec2 p) { return ImVec2(edzoom::toScreen(o.x, p.x, z), edzoom::toScreen(o.y, p.y, z)); }

void scaleVertices(ImDrawList* dl, int from, ImVec2 o, float z) {
  for (int i = from; i < dl->VtxBuffer.Size; ++i) dl->VtxBuffer[i].pos = mapOut(o, z, dl->VtxBuffer[i].pos);
}

void grip(App& a, Slot& s) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (!w || w->SkipItems || !s.zoomable) return;
  const float g = theme::kS4;
  const ImVec2 br(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
  const ImRect bb(ImVec2(br.x - g, br.y - g), br);
  const ImGuiID id = w->GetID("##edzoomgrip");
  ImGui::PushClipRect(w->Rect().Min, w->Rect().Max, false);
  ImGui::KeepAliveID(id);
  bool hovered = false, held = false;
  const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
  const ImGuiIO& io = ImGui::GetIO();
  if (pressed) {
    s.dragging = true;
    s.dragZ0 = s.liveZ = s.z;
    s.dragM0 = io.MousePos;
    s.dragW0 = ImVec2(std::max(1.0f, w->Size.x), std::max(1.0f, w->Size.y));
  }
  if (s.dragging && held) {
    const float rx = (s.dragW0.x + io.MousePos.x - s.dragM0.x) / s.dragW0.x;
    const float ry = (s.dragW0.y + io.MousePos.y - s.dragM0.y) / s.dragW0.y;
    s.liveZ = edzoom::roundZoom(s.dragZ0 * std::max(rx, ry));  // proportions kept: the larger stretch wins
  } else if (s.dragging) {
    s.dragging = false;
    remember(a, s.type, s.liveZ);
  }
  if (hovered || held) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    char b[160];
    std::snprintf(b, sizeof b, "%s  (%.0f %%)", i18n::tr("Drag to resize the editor; its size is remembered for this plugin"),
                  static_cast<double>(s.z * 100.0f));
    a.setHint(b);
  }
  const auto& P = theme::pal();
  const ImU32 c = held ? P.hot : hovered ? P.text : P.textFaint;
  ImDrawList* dl = w->DrawList;
  for (int k = 1; k <= 3; ++k) {  // three short diagonals in the corner
    const float d = static_cast<float>(k) * 4.0f;
    dl->AddLine(ImVec2(br.x - 3 - d, br.y - 3), ImVec2(br.x - 3, br.y - 3 - d), c, 1.0f);
  }
  ImGui::PopClipRect();
}

}  // namespace

float prepare(App& a, const char* key, const std::string& type, float contentW, bool zoomable) {
  Slot& s = slots()[key];
  const int frame = ImGui::GetFrameCount();
  if (s.lastFrame < frame - 1 || s.type != type) {  // (re)appeared, or another plugin in the window
    s.shownFrames = 0;
    s.autoZ = 0;
    s.dragging = false;
    if (s.type != type) {
      auto it = designCache().find(type);
      s.design = it != designCache().end() ? it->second : ImVec2(0, 0);
    }
  }
  if (s.lastFrame != frame) ++s.shownFrames;
  s.lastFrame = frame;
  s.type = type;
  s.zoomable = zoomable;
  s.contentW = contentW;
  if (!zoomable) return s.z = 1.0f;
  const ImVec2 area = ui::EditorArea();
  const ImVec2 ch = chrome();
  const float winW = contentW + ch.x, winH = s.design.y > 0 ? s.design.y + ch.y : 0.0f;
  float want = s.dragging ? s.liveZ : rememberedZoom(a, type);
  if (!(want > 0)) {
    if (s.autoZ <= 0 && winH > 0) s.autoZ = edzoom::autoZoom(winW, winH, area.x, area.y);
    want = s.autoZ > 0 ? s.autoZ : 1.0f;
  }
  const float z = edzoom::fitZoom(want, winW, winH, area.x, area.y);
  const float prev = s.z;
  s.z = z > 1.001f ? z : 1.0f;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  if (s.z > 1.0f && s.shownFrames <= 4) {  // opened magnified: centred on its final size
    s.centre = ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f);
    s.recentre = 4;
  } else if (std::fabs(prev - s.z) > 0.001f && !s.dragging && s.host && s.shownFrames > 1) {
    // Another size picked: the window grows or shrinks about its centre (a drag keeps its corner).
    s.centre = ImVec2(s.host->Pos.x + s.host->Size.x * 0.5f, s.host->Pos.y + s.host->Size.y * 0.5f);
    s.recentre = 4;  // the auto-sized window settles over a few frames
  }
  if (s.recentre > 0) {
    --s.recentre;
    ImGui::SetNextWindowPos(s.centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  }
  return s.z;
}

void begin(App&, const char* key) {
  Slot& s = slots()[key];
  ImGuiContext& g = *GImGui;
  s.host = ImGui::GetCurrentWindow();
  s.child = nullptr;
  s.active = s.z > 1.0f;
  s.origin = ImGui::GetCursorScreenPos();
  if (!s.active) {
    ImGui::BeginGroup();
    return;
  }
  s.clip = s.host->InnerClipRect;
  ImGuiViewportP* vp = s.host->Viewport;
  s.savedFb = vp->FramebufferScale;
  vp->FramebufferScale = ImVec2(s.savedFb.x * s.z, s.savedFb.y * s.z);  // glyphs baked at the magnified density
  g.DrawListSharedData.InitialFringeScale = 1.0f / s.z;                 // anti-aliasing stays one device pixel wide
  s.fg = ImGui::GetForegroundDrawList(vp);
  s.fgStart = s.fg->VtxBuffer.Size;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##edzoom", ImVec2(s.contentW, 0), ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar();
  s.child = ImGui::GetCurrentWindow();
}

void end(App& a, const char* key) {
  Slot& s = slots()[key];
  ImGuiContext& g = *GImGui;
  if (!s.active) {
    ImGui::EndGroup();
    s.design = ImVec2(std::max(s.contentW, ImGui::GetItemRectSize().x), ImGui::GetItemRectSize().y);
  } else {
    s.design = s.child ? s.child->Size : ImVec2(s.contentW, 0);
    ImGui::EndChild();
    ImGuiViewportP* vp = s.host->Viewport;
    vp->FramebufferScale = s.savedFb;
    g.DrawListSharedData.InitialFringeScale = 1.0f;
    ImGui::SetFontRasterizerDensity(s.savedFb.x);
    if (s.fg) scaleVertices(s.fg, s.fgStart, s.origin, s.z);  // overlays the editor drew on the foreground
    s.fg = nullptr;
    // The window holds the magnified picture.
    ImGui::SetCursorScreenPos(s.origin);
    ImGui::Dummy(ImVec2(std::floor(s.design.x * s.z), std::floor(s.design.y * s.z)));
  }
  if (s.design.y > 0) designCache()[s.type] = s.design;
  grip(a, s);
}

void sizeMenu(App& a, const std::string& type, const char* key) {
  const float remembered = rememberedZoom(a, type);
  const auto it = slots().find(key);
  const float shown = it != slots().end() && it->second.type == type ? it->second.z : 1.0f;
  if (!ImGui::BeginMenu(i18n::tr("Editor size"))) return;
  char b[64];
  std::snprintf(b, sizeof b, "%s (%.0f %%)", i18n::tr("Automatic"), static_cast<double>(remembered > 0 ? 0.0f : shown * 100.0f));
  if (ImGui::MenuItem(remembered > 0 ? i18n::tr("Automatic") : b, nullptr, remembered <= 0)) remember(a, type, 0);
  ui::Hint("Fits the screen: small editors open larger on a large workspace");
  ImGui::Separator();
  for (float z : edzoom::kSteps) {
    std::snprintf(b, sizeof b, "%.0f %%", static_cast<double>(z * 100.0f));
    if (ImGui::MenuItem(b, nullptr, remembered > 0 && std::fabs(remembered - z) < 0.005f)) remember(a, type, z);
  }
  if (remembered > 0 && std::none_of(std::begin(edzoom::kSteps), std::end(edzoom::kSteps), [&](float z) { return std::fabs(remembered - z) < 0.005f; })) {
    std::snprintf(b, sizeof b, "%.0f %%", static_cast<double>(remembered * 100.0f));
    ImGui::MenuItem(b, nullptr, true, false);
  }
  ImGui::EndMenu();
}

void mapPointer() {
  ImGuiContext& g = *GImGui;
  static ImU32 lastId = 0;
  static int downMask = 0;
  static bool captured = false;
  static Member capture;
  static ImVec2 lastReal(-FLT_MAX, -FLT_MAX);
  auto hit = [&g](ImVec2 q, Member& out) {
    for (int i = g.Windows.Size - 1; i >= 0; --i) {
      const ImGuiWindow* w = g.Windows[i];
      if (!w->Active || w->Hidden || (w->Flags & ImGuiWindowFlags_NoMouseInputs)) continue;
      if (g.MouseViewport && w->Viewport != g.MouseViewport) continue;
      const auto it = members().find(w);
      if (it == members().end()) {
        if (w->OuterRectClipped.Contains(q)) return false;  // another window is on top here
        continue;
      }
      const Member& m = it->second;
      ImRect r(mapOut(m.origin, m.z, w->OuterRectClipped.Min), mapOut(m.origin, m.z, w->OuterRectClipped.Max));
      if (m.inside) r.ClipWithFull(m.clip);
      if (r.Contains(q)) {
        out = m;
        return true;
      }
    }
    return false;
  };
  auto map = [&](ImVec2 q) {
    Member m;
    if (downMask != 0) return captured ? mapIn(capture, q) : q;
    return hit(q, m) ? mapIn(m, q) : q;
  };
  bool released = false;
  for (ImGuiInputEvent& e : g.InputEventsQueue) {
    if (e.EventId <= lastId) continue;
    lastId = e.EventId;
    if (e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX > -FLT_MAX * 0.5f) {
      lastReal = ImVec2(e.MousePos.PosX, e.MousePos.PosY);
      const ImVec2 m = map(lastReal);
      e.MousePos.PosX = m.x;
      e.MousePos.PosY = m.y;
    } else if (e.Type == ImGuiInputEventType_MouseButton) {
      const int bit = 1 << std::clamp(e.MouseButton.Button, 0, 7);
      if (e.MouseButton.Down) {
        if (downMask == 0) captured = lastReal.x > -FLT_MAX * 0.5f && hit(lastReal, capture);
        downMask |= bit;
      } else if (downMask & bit) {
        downMask &= ~bit;
        released = downMask == 0;
      }
    }
  }
  if (released && captured) {  // the pointer is over whatever lies there now
    captured = false;
    const ImVec2 m = map(lastReal);
    ImGui::GetIO().AddMousePosEvent(m.x, m.y);
    lastId = g.InputEventsNextEventId - 1;
  }
}

void afterRender() {
  ImGuiContext& g = *GImGui;
  members().clear();
  const int frame = g.FrameCount;
  for (auto& [key, s] : slots()) {
    if (!s.active || s.lastFrame != frame || !s.child) {
      s.active = false;
      continue;
    }
    for (ImGuiWindow* w : g.Windows) {
      if (w->LastFrameActive != frame || !w->DrawList) continue;
      bool inside = true, family = false;
      for (const ImGuiWindow* p = w; p; p = p->ParentWindow) {
        if (p == s.child) {
          family = true;
          break;
        }
        if (!(p->Flags & ImGuiWindowFlags_ChildWindow)) inside = false;
      }
      if (!family) continue;
      ImDrawList* dl = w->DrawList;
      scaleVertices(dl, 0, s.origin, s.z);
      for (ImDrawCmd& c : dl->CmdBuffer) {
        ImRect r(mapOut(s.origin, s.z, ImVec2(c.ClipRect.x, c.ClipRect.y)), mapOut(s.origin, s.z, ImVec2(c.ClipRect.z, c.ClipRect.w)));
        if (inside) r.ClipWithFull(s.clip);
        c.ClipRect = ImVec4(r.Min.x, r.Min.y, r.Max.x, r.Max.y);
      }
      members()[w] = Member{s.origin, s.z, inside, s.clip};
    }
    s.active = false;
  }
}

void devDump(const char* key) {
  const auto it = slots().find(key);
  if (it == slots().end() || it->second.lastFrame < ImGui::GetFrameCount() - 1) {
    std::printf("edzoom %s: not drawn\n", key);
  } else {
    const Slot& s = it->second;
    const ImGuiWindow* h = s.host;
    std::printf("edzoom %s: type %s zoom %.2f design %.0fx%.0f origin %.1f,%.1f window %.1f,%.1f %.1fx%.1f\n", key, s.type.c_str(),
                static_cast<double>(s.z), static_cast<double>(s.design.x), static_cast<double>(s.design.y), static_cast<double>(s.origin.x),
                static_cast<double>(s.origin.y), h ? static_cast<double>(h->Pos.x) : 0.0, h ? static_cast<double>(h->Pos.y) : 0.0,
                h ? static_cast<double>(h->Size.x) : 0.0, h ? static_cast<double>(h->Size.y) : 0.0);
  }
  std::fflush(stdout);
}

ImVec2 toScreen(const char* key, ImVec2 p) {
  const auto it = slots().find(key);
  if (it == slots().end() || it->second.z <= 1.0f) return p;
  return mapOut(it->second.origin, it->second.z, p);
}

}  // namespace gl::edzoomui

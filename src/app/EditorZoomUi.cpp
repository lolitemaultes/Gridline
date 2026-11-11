// Gridline - K-19: plugin editor zoom, see EditorZoomUi.h.
#include "EditorZoomUi.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
  ImVec2 savedMax{0, 0}, savedIdeal{0, 0};  // K-136: the host's content extent before a reduced editor
  ImDrawList* fg = nullptr;
  int fgStart = 0;
  // K-147 fit report: the editor's ink (what it drew, screen px) against its window
  bool inkValid = false, bodyValid = false;
  bool grouped = false;  // drawn at 100 % straight into the window (no region): measured from hostVtx0
  unsigned hostVtx0 = 0, hostVtx1 = ~0u;
  float overflow = 0;  // how far the laid-out items reach past the region on the right (design px)
  ImRect ink, body, inner, innerClip;
  // the plugin's editor body (markBody): where it starts and the width it says it has
  const ImDrawList* bodyDl = nullptr;
  unsigned bodyVtx = 0, bodyVtxEnd = ~0u;
  float bodyY = 0, bodyEndY = FLT_MAX, bodyW = 0;
  int bodyFrame = -1;
  ImVec2 pad{0, 0};
  // drag-resizing with the corner grip
  bool dragging = false;
  float dragZ0 = 1, liveZ = 1;
  ImVec2 dragM0{0, 0}, dragW0{1, 1};
};

float& current() {
  static float z = 1;
  return z;
}
ImVec2& currentOrigin() {  // the top-left of the editor region being drawn (its zoom's fixed point)
  static ImVec2 o{0, 0};
  return o;
}

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
// K-136: GRIDLINE_PLUGIN_WINDOW_SIZE (tests): a fixed global size instead of the setting
// (the test suite runs the editors at their design size, 1, unless a test clears it).
float globalZoom(App& a) {
  static const char* env = std::getenv("GRIDLINE_PLUGIN_WINDOW_SIZE");
  if (env && *env) return edzoom::globalSize(std::strtod(env, nullptr));
  return edzoom::globalSize(a.settings.getNumber("ui.pluginWindowSize"));
}
void remember(App& a, const std::string& type, float z) { a.settings.set("ui.editorZoom", edzoom::withZoom(a.settings.getString("ui.editorZoom"), type, z)); }

// K-147: the window around the editor: its padding on both sides and the title bar (no
// scrollbar: the window is sized to fit, it only scrolls when even kFitMin is too large).
ImVec2 chrome() {
  const ImGuiStyle& st = ImGui::GetStyle();
  return ImVec2(2 * st.WindowPadding.x, 2 * st.WindowPadding.y + ImGui::GetFrameHeight());
}

ImVec2 mapIn(const Member& m, ImVec2 q) { return ImVec2(edzoom::toEditor(m.origin.x, q.x, m.z), edzoom::toEditor(m.origin.y, q.y, m.z)); }
ImVec2 mapOut(ImVec2 o, float z, ImVec2 p) { return ImVec2(edzoom::toScreen(o.x, p.x, z), edzoom::toScreen(o.y, p.y, z)); }

void scaleVertices(ImDrawList* dl, int from, ImVec2 o, float z) {
  for (int i = from; i < dl->VtxBuffer.Size; ++i) dl->VtxBuffer[i].pos = mapOut(o, z, dl->VtxBuffer[i].pos);
}

// K-147: the bounding box of what a draw list shows (screen px). A command clipped only
// by its window (no clip rectangle of the editor's own) counts unclipped, so content that
// runs past the window is found; one the editor clipped itself counts as clipped.
void addInk(ImRect& ink, bool& any, const ImDrawList* dl, int cmd0, int cmd1, const ImRect& wclip, ImVec2 o, float z, unsigned vtxFrom = 0,
            unsigned vtxTo = ~0u) {
  cmd1 = std::min(cmd1, dl->CmdBuffer.Size);
  for (int c = std::max(0, cmd0); c < cmd1; ++c) {
    const ImDrawCmd& cmd = dl->CmdBuffer[c];
    if (cmd.UserCallback || cmd.ElemCount == 0) continue;
    ImRect bb(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (unsigned i = 0; i < cmd.ElemCount; ++i) {
      const unsigned vi = dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + i)] + cmd.VtxOffset;
      if (vi >= static_cast<unsigned>(dl->VtxBuffer.Size) || vi < vtxFrom || vi >= vtxTo) continue;
      const ImDrawVert& v = dl->VtxBuffer[static_cast<int>(vi)];
      if ((v.col & IM_COL32_A_MASK) == 0) continue;  // anti-aliasing fringes
      bb.Add(v.pos);
    }
    if (bb.Min.x > bb.Max.x) continue;
    const ImRect clip(cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w);
    const bool own = clip.Min.x > wclip.Min.x + 0.5f || clip.Min.y > wclip.Min.y + 0.5f || clip.Max.x < wclip.Max.x - 0.5f || clip.Max.y < wclip.Max.y - 0.5f;
    if (own) bb.ClipWithFull(clip);
    if (bb.Min.x >= bb.Max.x || bb.Min.y >= bb.Max.y) continue;
    ink.Add(ImRect(mapOut(o, z, bb.Min), mapOut(o, z, bb.Max)));
    any = true;
  }
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
  if (!(want > 0)) {  // K-136: the global size (or the automatic one, when that is on and larger)
    const bool fit = a.settings.getBool("ui.pluginWindowFit");
    if (fit && s.autoZ <= 0 && winH > 0) s.autoZ = edzoom::autoZoom(winW, winH, area.x, area.y);
    want = std::max(globalZoom(a), fit && s.autoZ > 0 ? s.autoZ : 0.0f);
  }
  // K-147: the editor (not its chrome) is what scales: it gets the room the chrome leaves.
  // (a few px to spare: a window exactly as tall as the room would show a scrollbar for a rounding px)
  float z = edzoom::fitZoom(want, contentW, s.design.y, area.x - ch.x - 4.0f, area.y - ch.y - 4.0f);
  // Tests that pin the size (GRIDLINE_PLUGIN_WINDOW_SIZE, see globalZoom) click at design
  // positions: there a large editor keeps its size and scrolls, as before K-147.
  static const bool pinned = [] {
    const char* env = std::getenv("GRIDLINE_PLUGIN_WINDOW_SIZE");
    return env && *env;
  }();
  if (pinned) z = std::max(z, std::min(edzoom::clampZoom(want), 1.0f));
  const float prev = s.z;
  s.z = std::fabs(z - 1.0f) > 0.001f ? z : 1.0f;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  if (s.shownFrames <= 4 && (s.z != 1.0f || std::fabs(prev - s.z) > 0.001f)) {  // opened at another size: centred on its final size
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
  // K-147: every built-in editor is drawn in its own region of exactly its width (at 100 %
  // too), so the window, the header and the editor agree at every size.
  s.active = s.zoomable && s.z != 1.0f;
  s.grouped = !s.active;
  s.hostVtx0 = static_cast<unsigned>(s.host->DrawList->VtxBuffer.Size);
  s.hostVtx1 = ~0u;
  s.origin = ImGui::GetCursorScreenPos();
  current() = s.z;
  currentOrigin() = s.origin;
  s.inner = s.host->InnerRect;
  s.innerClip = s.host->InnerClipRect;
  s.pad = s.host->WindowPadding;
  if (!s.active) {
    ImGui::BeginGroup();
    return;
  }
  s.clip = s.host->InnerClipRect;
  s.savedMax = s.host->DC.CursorMaxPos;
  s.savedIdeal = s.host->DC.IdealMaxPos;
  if (s.z < 1.0f)  // K-136: a reduced editor is laid out at its full design size, past the smaller window's edges
    ImGui::PushClipRect(ImVec2(s.origin.x - 4096, s.origin.y - 4096), ImVec2(s.origin.x + 16384, s.origin.y + 16384), false);
  ImGuiViewportP* vp = s.host->Viewport;
  s.savedFb = vp->FramebufferScale;
  if (s.z != 1.0f) {
    vp->FramebufferScale = ImVec2(s.savedFb.x * s.z, s.savedFb.y * s.z);  // glyphs baked at the magnified density
    g.DrawListSharedData.InitialFringeScale = 1.0f / s.z;                 // anti-aliasing stays one device pixel wide
  }
  s.fg = ImGui::GetForegroundDrawList(vp);
  s.fgStart = s.fg->VtxBuffer.Size;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##edzoom", ImVec2(s.contentW, 0), ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar();
  s.child = ImGui::GetCurrentWindow();
}

float currentZoom() { return current(); }
ImVec2 toScreenHere(ImVec2 p) { return current() == 1.0f ? p : mapOut(currentOrigin(), current(), p); }

void markBodyEnd() {
  for (auto& [key, s] : slots())
    if (s.bodyFrame == ImGui::GetFrameCount() && s.bodyDl == ImGui::GetWindowDrawList() && s.bodyVtxEnd == ~0u) {
      s.bodyVtxEnd = static_cast<unsigned>(s.bodyDl->VtxBuffer.Size);
      s.bodyEndY = ImGui::GetCursorScreenPos().y;
    }
}

void markBody(float width) {
  for (auto& [key, s] : slots())
    if (s.lastFrame == ImGui::GetFrameCount() && s.host && ((s.active && current() == s.z && s.child) || (s.grouped && s.zoomable)) &&
        ImGui::GetCurrentWindow()->RootWindow == s.host->RootWindow) {
      s.bodyDl = ImGui::GetWindowDrawList();
      s.bodyVtx = static_cast<unsigned>(s.bodyDl->VtxBuffer.Size);
      s.bodyY = ImGui::GetCursorScreenPos().y;
      s.bodyVtxEnd = ~0u;
      s.bodyEndY = FLT_MAX;
      s.bodyW = width;
      s.bodyFrame = ImGui::GetFrameCount();
    }
}

void end(App& a, const char* key) {
  Slot& s = slots()[key];
  current() = 1;
  ImGuiContext& g = *GImGui;
  if (!s.active) {
    ImGui::EndGroup();
    s.design = ImVec2(std::max(s.contentW, ImGui::GetItemRectSize().x), ImGui::GetItemRectSize().y);
    s.overflow = s.zoomable ? std::max(0.0f, ImGui::GetItemRectMax().x - (s.origin.x + s.contentW)) : 0.0f;
  } else {
    s.design = s.child ? s.child->Size : ImVec2(s.contentW, 0);
    s.overflow = s.child ? std::max(0.0f, s.child->DC.CursorMaxPos.x - (s.child->Pos.x + s.contentW)) : 0.0f;
    ImGui::EndChild();
    if (s.z < 1.0f) {  // the window holds only the reduced picture (not the design size laid out above)
      ImGui::PopClipRect();
      s.host->DC.CursorMaxPos = s.savedMax;
      s.host->DC.IdealMaxPos = s.savedIdeal;
    }
    ImGuiViewportP* vp = s.host->Viewport;
    vp->FramebufferScale = s.savedFb;
    g.DrawListSharedData.InitialFringeScale = 1.0f;
    ImGui::SetFontRasterizerDensity(s.savedFb.x);
    if (s.fg) scaleVertices(s.fg, s.fgStart, s.origin, s.z);  // overlays the editor drew on the foreground
    s.fg = nullptr;
    // The window holds the magnified picture (K-147: whole pixels up, never a clipped row).
    ImGui::SetCursorScreenPos(s.origin);
    ImGui::Dummy(ImVec2(std::floor(s.design.x * s.z + 0.01f), std::ceil(s.design.y * s.z - 0.01f)));
  }
  if (s.design.y > 0) designCache()[s.type] = s.design;
  if (s.host) s.hostVtx1 = static_cast<unsigned>(s.host->DrawList->VtxBuffer.Size);  // (the size grip is not the editor)
  grip(a, s);
}

int nextWindowSize(const char* key) {
  const Slot& s = slots()[key];
  const ImVec2 area = ui::EditorArea(), ch = chrome();
  const ImGuiStyle& st = ImGui::GetStyle();
  float w = std::floor(s.contentW * s.z + 0.01f) + ch.x;
  // Under the fit floor the editor is taller than the room: the window scrolls, with room for the bar.
  if (s.design.y > 0 && std::ceil(s.design.y * s.z) + ch.y > area.y + 0.5f) w += st.ScrollbarSize;
  const bool scrollX = w > area.x + 0.5f;
  w = std::min(w, area.x);
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, std::max(1.0f, area.y)));
  return scrollX ? ImGuiWindowFlags_HorizontalScrollbar : 0;
}

void sizeMenu(App& a, const std::string& type, const char* key) {
  const float remembered = rememberedZoom(a, type);
  const auto it = slots().find(key);
  const float shown = it != slots().end() && it->second.type == type ? it->second.z : 1.0f;
  if (!ImGui::BeginMenu(i18n::tr("Editor size"))) return;
  char b[96];
  // K-136: no size of its own: the global plugin window size (Settings > Interface).
  std::snprintf(b, sizeof b, "%s (%.0f %%)", i18n::tr("Same as other plugins"),
                static_cast<double>((remembered > 0 ? globalZoom(a) : shown) * 100.0f));
  if (ImGui::MenuItem(b, nullptr, remembered <= 0)) remember(a, type, 0);
  ui::Hint("The plugin window size set in Settings > Interface");
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
  // K-136: a reduced editor (under 100 %) is laid out larger than it shows, so its windows'
  // rectangles reach past the picture. While the pointer is not over the picture they take
  // no mouse input this frame (ImGui finds the hovered window before they begin again).
  Member over;
  const bool onPicture = lastReal.x > -FLT_MAX * 0.5f && hit(lastReal, over);
  if (!onPicture && !(downMask != 0 && captured))
    for (auto& [w, m] : members())
      if (m.z < 1.0f) const_cast<ImGuiWindow*>(w)->Flags |= ImGuiWindowFlags_NoMouseInputs;
}

// K-147: the ink of the editor in `s` this frame (before the magnified picture is scaled).
void measure(Slot& s, int frame) {
  ImGuiContext& g = *GImGui;
  s.inkValid = false;
  if (!s.host || !s.zoomable || s.lastFrame != frame || (!s.grouped && !(s.active && s.child))) return;  // built-in editors only
  // At 100 % the editor is drawn straight into its window (from hostVtx0 on): that window's clip is the edge.
  ImGuiWindow* const top = s.grouped ? s.host : s.child;
  ImRect ink(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX), body = ink;
  bool any = false, anyBody = false;
  const bool marked = s.bodyFrame == frame && s.bodyDl;
  const ImVec2 o = s.active ? s.origin : ImVec2(0, 0);
  const float z = s.active ? s.z : 1.0f;
  for (ImGuiWindow* w : g.Windows) {  // the child windows inside the editor (sections, the magnified region)
    if (w->LastFrameActive != frame || !w->DrawList) continue;
    if (w == s.host ? !s.grouped : !(w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
    if (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) continue;
    bool family = false;
    for (const ImGuiWindow* p = w; p; p = p->ParentWindow) {
      if (p == top) {
        family = true;
        break;
      }
      if (!(p->Flags & ImGuiWindowFlags_ChildWindow) || (p->Flags & ImGuiWindowFlags_Popup)) break;
    }
    if (!family && !(s.grouped && w == s.host)) continue;
    // A child window is clipped by its own rectangle: what runs past it counts as ink too.
    ImRect wclip = w->InnerClipRect;
    if (w == s.child) wclip = ImRect(-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX);
    const unsigned from = s.grouped && w == s.host ? s.hostVtx0 : 0u;
    addInk(ink, any, w->DrawList, 0, w->DrawList->CmdBuffer.Size, wclip, o, z, from, s.grouped && w == s.host ? s.hostVtx1 : ~0u);
    // The body: what the editor drew after markBody() (in that window from the mark on; child
    // windows that start below it).
    if (marked && w->DrawList == s.bodyDl) addInk(body, anyBody, w->DrawList, 0, w->DrawList->CmdBuffer.Size, wclip, o, z, s.bodyVtx, s.bodyVtxEnd);
    else if (marked && w != s.child && w != s.host && w->Pos.y >= s.bodyY - 0.5f && w->Pos.y < s.bodyEndY - 0.5f) addInk(body, anyBody, w->DrawList, 0, w->DrawList->CmdBuffer.Size, wclip, o, z);
  }
  s.ink = ink;
  s.inkValid = any;
  s.body = body;
  s.bodyValid = anyBody;
}

void afterRender() {
  ImGuiContext& g = *GImGui;
  members().clear();
  const int frame = g.FrameCount;
  for (auto& [key, s] : slots()) {
    measure(s, frame);
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

ImVec2 devWindowSize(const char* key) {
  const auto it = slots().find(key);
  if (it == slots().end() || !it->second.host || it->second.lastFrame < ImGui::GetFrameCount() - 1) return ImVec2(0, 0);
  return it->second.host->Size;
}

void devDump(const char* key) {
  const auto it = slots().find(key);
  if (it == slots().end() || it->second.lastFrame < ImGui::GetFrameCount() - 1) {
    std::printf("edzoom %s: not drawn\n", key);
  } else {
    const Slot& s = it->second;
    const ImGuiWindow* h = s.host;
    const ImGuiWindow* hw = GImGui->HoveredWindow;  // K-136: is the pointer over this editor (its window or a child)?
    std::printf("edzoom %s: type %s zoom %.2f design %.0fx%.0f origin %.1f,%.1f window %.1f,%.1f %.1fx%.1f hovered %d\n", key, s.type.c_str(),
                static_cast<double>(s.z), static_cast<double>(s.design.x), static_cast<double>(s.design.y), static_cast<double>(s.origin.x),
                static_cast<double>(s.origin.y), h ? static_cast<double>(h->Pos.x) : 0.0, h ? static_cast<double>(h->Pos.y) : 0.0,
                h ? static_cast<double>(h->Size.x) : 0.0, h ? static_cast<double>(h->Size.y) : 0.0, hw && h && hw->RootWindow == h->RootWindow ? 1 : 0);
    // K-147: what the editor drew against its window: clipped (past the window's clip, left
    // top right bottom) and spare room (inside the padding: left, right, bottom), in screen px.
    if (s.inkValid) {
      const ImRect& k = s.ink;
      const float oL = std::max(0.0f, s.innerClip.Min.x - k.Min.x), oT = std::max(0.0f, s.innerClip.Min.y - k.Min.y);
      const float oR = std::max(0.0f, k.Max.x - s.innerClip.Max.x), oB = std::max(0.0f, k.Max.y - s.innerClip.Max.y);
      const float eL = k.Min.x - (s.inner.Min.x + s.pad.x), eR = (s.inner.Max.x - s.pad.x) - k.Max.x;
      const float eB = (s.inner.Max.y - s.pad.y) - k.Max.y;
      // The body (the plugin's own editor under the header): its ink width and the width it
      // was given, both at this size (screen px).
      const float bw = s.bodyValid ? s.body.GetWidth() : 0.0f;
      std::printf("edfit %s: type %s zoom %.2f inner %.0fx%.0f ink %.0fx%.0f clipped %.0f,%.0f,%.0f,%.0f spare %.0f,%.0f,%.0f body %.0f at %.0f of %.0f region %.0f overflow %.0f\n", key,
                  s.type.c_str(), static_cast<double>(s.z), static_cast<double>(s.inner.GetWidth()), static_cast<double>(s.inner.GetHeight()),
                  static_cast<double>(k.GetWidth()), static_cast<double>(k.GetHeight()), static_cast<double>(oL), static_cast<double>(oT),
                  static_cast<double>(oR), static_cast<double>(oB), static_cast<double>(eL), static_cast<double>(eR), static_cast<double>(eB),
                  static_cast<double>(bw), static_cast<double>(s.bodyValid ? s.body.Min.x - s.origin.x : 0.0f), static_cast<double>(s.bodyW * s.z), static_cast<double>(s.contentW * s.z), static_cast<double>(s.overflow * s.z));
    }
  }
  std::fflush(stdout);
}

ImVec2 toScreen(const char* key, ImVec2 p) {
  const auto it = slots().find(key);
  if (it == slots().end() || it->second.z == 1.0f) return p;
  return mapOut(it->second.origin, it->second.z, p);
}

}  // namespace gl::edzoomui

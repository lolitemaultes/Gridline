// Gridline - the Tracker layout's header band (K-106): the Order list part. See TrackerView.h.
//
// In the Tracker layout the part fills the header region with the classic tracker's boxes, drawn in
// Gridline's own style (bevelled boxes from the theme's surface roles, numbers in the monospaced face):
// the order list (derived from the playlist), the song box (steppers), the scope box (a level cell per
// channel), two button columns, the instrument and sample lists, and the page strip that picks what the
// centre shows. Every button runs a Gridline command; nothing here is stored in the project.
// In the other layouts the part is the order list alone.
#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/MenuOps.h"
#include "../core/MenuParity.h"
#include "../core/Params.h"
#include "../core/Project.h"
#include "../core/ProjectFiles.h"
#include "../core/BrowserIndex.h"
#include "../core/FileOps.h"
#include "../core/TrackerGrid.h"
#include "App.h"
#include "ChromeBanner.h"
#include "BrowserUi.h"
#include "ContextMenus.h"
#include "HostedUi.h"
#include "../core/PluginInfo.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "MenuParityUi.h"
#include "MixerMeters.h"
#include "PixelGrid.h"
#include "../packs/PacksUi.h"
#include "Theme.h"
#include "TrackerEditors.h"
#include "TrackerLook.h"
#include "TrackerPages.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"
#include "GridlineVersion.h"
#include "DisplayConfirmUi.h"
#include "Midi.h"
#include "../core/Remote.h"
#include "../packs/tracker/tracker.h"

namespace gl::trackerview {

using theme::pal;

ImFont* monoFont(App& a) { return a.fontMono ? a.fontMono : ImGui::GetFont(); }

// N-07: the values' ink: the theme's hot colour, darkened step by step until it reads on the sunken
// fields (about 4.5:1; the light theme's orange on light grey was 2:1).
ImU32 valueInk() {
  const auto& P = pal();
  static ImU32 forHot = 0, forBg = 0, ink = 0;
  if (forHot == P.hot && forBg == P.surface950 && ink) return ink;
  forHot = P.hot, forBg = P.surface950;
  ink = P.hot;
  const bool light = theme::contrastRatio(P.surface950, IM_COL32(0, 0, 0, 255)) > theme::contrastRatio(P.surface950, IM_COL32(255, 255, 255, 255));
  for (int i = 1; i <= 10 && theme::contrastRatio(ink, P.surface950) < 4.5f; ++i)
    ink = theme::mix(P.hot, light ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255), 0.08f * static_cast<float>(i));
  return ink;
}

void bevelBox(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool sunken) {
  const auto& P = pal();
  const float dpp = pixelgrid::frameScale();
  p0 = ImVec2(pixelgrid::edge(p0.x, dpp), pixelgrid::edge(p0.y, dpp));
  p1 = ImVec2(pixelgrid::edge(p1.x, dpp), pixelgrid::edge(p1.y, dpp));
  const float w = pixelgrid::whole(1.0f, dpp);
  // Two steps of edge: a raised face is lit at the top and left and shaded at the bottom and right
  // (an outer edge, and a softer inner one); a sunken well the other way round. Theme roles only.
  const ImU32 face = sunken ? P.surface950 : P.surface700;
  dl->AddRectFilled(p0, p1, face);
  const ImU32 lit = theme::mix(P.surface700, P.text, 0.42f), lit2 = theme::mix(P.surface700, P.text, 0.16f);
  const ImU32 shade = theme::mix(P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), shade2 = theme::mix(P.surface700, P.surface950, 0.55f);
  auto edges = [&](ImVec2 q0, ImVec2 q1, ImU32 tl, ImU32 br) {
    dl->AddRectFilled(q0, ImVec2(q1.x, q0.y + w), tl);
    dl->AddRectFilled(q0, ImVec2(q0.x + w, q1.y), tl);
    dl->AddRectFilled(ImVec2(q0.x, q1.y - w), q1, br);
    dl->AddRectFilled(ImVec2(q1.x - w, q0.y + w), q1, br);
  };
  if (sunken) {
    edges(p0, p1, shade, theme::mix(P.surface700, P.text, 0.22f));
  } else {
    edges(p0, p1, lit, shade);
    if (p1.x - p0.x > 6 * w && p1.y - p0.y > 6 * w) edges(ImVec2(p0.x + w, p0.y + w), ImVec2(p1.x - w, p1.y - w), lit2, shade2);
  }
}

bool bevelButton(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  const bool click = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  const bool off = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;  // still to come, or nothing to act on
  bevelBox(dl, p0, p1, held || on);
  if (on) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::withAlpha(P.hot, 0.85f));
  else if (hov && !held) dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 2), ImVec2(p1.x - 2, p1.y - 2), theme::withAlpha(P.surface600, 0.6f));
  const float fs = ImGui::GetFontSize();
  // Pressed, the label sinks a pixel with the face.
  const ImVec2 d = held ? ImVec2(1, 1) : ImVec2(0, 0);
  theme::centreText(dl, a.fontRegular, fs, ImVec2(p0.x + d.x, p0.y + d.y), ImVec2(p1.x + d.x, p1.y + d.y), on ? theme::contrastText(P.hot) : off ? P.textFaint : P.text, i18n::tr(label));
  if (hint) ui::Hint(hint);
  if (click) trackerview::keysBackToPattern();
  return click;
}

// cp15: a button label that does not fit its width gives way to its short form (narrow windows).
const char* fitLabel(App& a, const char* label, const char* brief, float width) {
  if (!brief) return label;
  const float w = a.fontRegular->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, i18n::tr(label)).x;
  return w + 8 > width ? brief : label;
}

void boxTitle(App& a, ImVec2 p, const char* text) {
  ImGui::GetWindowDrawList()->AddText(a.fontBold ? a.fontBold : a.fontRegular, ImGui::GetFontSize(), p, pal().text, i18n::tr(text));
}

// A radio row, as the classic screens' page and subject lists (refs 164, 166): a small diamond, lit
// when on, then the label. The whole row takes the click.
bool radioRow(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  const bool click = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered();
  const float fs = ImGui::GetFontSize();
  const float r = std::max(3.0f, std::round(fs * 0.3f));
  const ImVec2 c(std::round(p0.x + 4 + r), std::round((p0.y + p1.y) * 0.5f));
  const ImVec2 q[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
  dl->AddQuadFilled(q[0], q[1], q[2], q[3], on ? P.hot : P.surface950);
  dl->AddQuad(q[0], q[1], q[2], q[3], on ? theme::mix(P.hot, P.text, 0.3f) : theme::mix(P.surface700, P.text, 0.42f));
  if (hov && !on) dl->AddRectFilled(ImVec2(p0.x + 2 * r + 8, p0.y + 1), ImVec2(p1.x, p1.y - 1), theme::withAlpha(P.surface600, 0.5f));
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(p0.x + 2 * r + 10, std::round((p0.y + p1.y - fs) * 0.5f)), p1.x - p0.x - 2 * r - 12,
                      on ? P.text : P.textDim, i18n::tr(label));
  if (hint) ui::Hint(hint);
  if (click) trackerview::keysBackToPattern();
  return click;
}

// A check row, as the classic screens' check boxes (Trim, Advanced edit, the settings pages): a
// small sunken square, ticked when on, then the label (none: the square alone, centred).
bool checkRow(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  const bool click = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered();
  const bool off = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
  const float fs = ImGui::GetFontSize();
  const float b = std::max(8.0f, std::round(std::min(fs * 0.8f, p1.y - p0.y - 2)));
  const bool bare = !label || !*label;
  const float bx = bare ? std::round((p0.x + p1.x - b) * 0.5f) : std::round(p0.x + 3);
  const ImVec2 q0(bx, std::round((p0.y + p1.y - b) * 0.5f)), q1(q0.x + b, q0.y + b);
  bevelBox(dl, q0, q1, true);
  if (hov && !off) dl->AddRect(ImVec2(q0.x - 1, q0.y - 1), ImVec2(q1.x + 1, q1.y + 1), theme::withAlpha(P.hot, 0.6f));
  if (on) {
    const ImU32 ink = off ? P.textFaint : P.hot;
    const float t = std::max(1.5f, std::round(b * 0.16f));
    dl->AddLine(ImVec2(q0.x + b * 0.2f, q0.y + b * 0.52f), ImVec2(q0.x + b * 0.43f, q1.y - b * 0.22f), ink, t);
    dl->AddLine(ImVec2(q0.x + b * 0.43f, q1.y - b * 0.22f), ImVec2(q1.x - b * 0.18f, q0.y + b * 0.22f), ink, t);
  }
  if (!bare)
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(q1.x + 6, std::round((p0.y + p1.y - fs) * 0.5f)), p1.x - q1.x - 8, off ? P.textFaint : on ? P.text : P.textDim,
                        i18n::tr(label));
  if (hint) ui::Hint(hint);
  if (click) trackerview::keysBackToPattern();
  return click && !off;
}

// A small arrow button (a filled triangle on a bevel face); repeats while held.
bool arrowButton(App& a, const char* id, ImVec2 p0, ImVec2 p1, bool up, const char* hint) {
  const auto& P = pal();
  ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
  const bool click = bevelButton(a, id, "", p0, p1, false, hint);
  ImGui::PopItemFlag();
  const bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cx = std::round((p0.x + p1.x) * 0.5f) + (held ? 1 : 0), cy = std::round((p0.y + p1.y) * 0.5f) + (held ? 1 : 0);
  const float r = std::max(2.0f, std::floor(std::min(p1.x - p0.x, p1.y - p0.y) * 0.22f));
  if (up) dl->AddTriangleFilled(ImVec2(cx - r - 1, cy + r * 0.6f), ImVec2(cx + r + 1, cy + r * 0.6f), ImVec2(cx, cy - r * 0.8f), P.text);
  else dl->AddTriangleFilled(ImVec2(cx - r - 1, cy - r * 0.6f), ImVec2(cx + r + 1, cy - r * 0.6f), ImVec2(cx, cy + r * 0.8f), P.text);
  (void)a;
  return click;
}

// cp17 (moved from TrackerPages.cpp, unchanged): a horizontal value bar (volume, pan) in a sunken box; dragging sets it. Returns true while it changed.
bool valueBar(const char* id, ImVec2 p0, ImVec2 p1, double v, double lo, double hi, bool centred, double* out, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bevelBox(dl, p0, p1, true);
  // cp16: the classic slider: a sunken groove with a raised bevel knob at the value (before: a solid
  // green bar, the modern look); the part up to the value (from the middle when centred) a thin line.
  const float x0 = p0.x + 2, x1 = p1.x - 2;
  const float k = static_cast<float>((std::clamp(v, lo, hi) - lo) / (hi - lo));
  const float knobW = std::max(8.0f, std::round((p1.y - p0.y) * 0.6f));
  const float at = std::round(x0 + knobW * 0.5f + (x1 - x0 - knobW) * k);
  const float cy = std::round((p0.y + p1.y) * 0.5f);
  const ImU32 line = act ? P.hot : theme::withAlpha(P.led, hov ? 0.9f : 0.7f);
  if (centred) {
    const float mid = std::round((x0 + x1) * 0.5f);
    dl->AddRectFilled(ImVec2(mid, p0.y + 3), ImVec2(mid + 1, p1.y - 3), P.textFaint);
    dl->AddRectFilled(ImVec2(std::min(mid, at), cy - 1), ImVec2(std::max(mid, at) + 1, cy + 1), line);
  } else {
    dl->AddRectFilled(ImVec2(x0 + 1, cy - 1), ImVec2(at, cy + 1), line);
  }
  const ImVec2 k0(at - knobW * 0.5f, p0.y + 2), k1(at + knobW * 0.5f, p1.y - 2);
  dl->AddRectFilled(k0, k1, act ? theme::mix(P.surface600, P.hot, 0.35f) : hov ? P.surface600 : P.surface700);
  bevelBox(dl, k0, k1, act);
  if (hint) ui::Hint(hint);
  if (act && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) || ImGui::IsItemActivated())) {
    const float mk = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    *out = lo + (hi - lo) * mk;
    return true;
  }
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {  // double-click: the default
    *out = centred ? (lo + hi) * 0.5 : 0.8;
    return true;
  }
  return false;
}

// cp17: the modal box (TrackerLook.h).
bool beginModalBox(App& a, const char* id, const char* title, ImVec2 size) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar(2);
  if (!open) return false;
  const ImVec2 p0 = ImGui::GetWindowPos(), p1(p0.x + ImGui::GetWindowWidth(), p0.y + ImGui::GetWindowHeight());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bevelBox(dl, p0, p1, false);
  bevelBox(dl, ImVec2(p0.x + 3, p0.y + 3), ImVec2(p1.x - 3, p1.y - 3), true);
  boxTitle(a, ImVec2(p0.x + 10, p0.y + 7), title);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 10, p0.y + 12 + ImGui::GetFontSize()));
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
  return true;
}
void endModalBox() { ImGui::EndPopup(); }

// cp17: a sideways scroll bar in the band's look: a sunken trough between two arrow buttons, the
// raised thumb over the part in view. Dragging the thumb scrolls; a click in the trough beside it
// pages; the arrows (held: repeating) step by `step`.
bool scrollBar(App& a, const char* id, ImVec2 p0, ImVec2 p1, double* start, double view, double total, double step, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const double was = *start;
  const double most = std::max(0.0, total - view);
  ImGui::PushID(id);
  const float h = p1.y - p0.y, aw = std::round(std::max(h, 10.0f));
  // The arrows.
  for (int side = 0; side < 2; ++side) {
    const ImVec2 b0(side ? p1.x - aw : p0.x, p0.y), b1(side ? p1.x : p0.x + aw, p1.y);
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    const bool click = bevelButton(a, side ? "##sbr" : "##sbl", "", b0, b1, false, hint);
    ImGui::PopItemFlag();
    const bool held = ImGui::IsItemActive();
    const float cx = std::round((b0.x + b1.x) * 0.5f) + (held ? 1 : 0), cy = std::round((b0.y + b1.y) * 0.5f) + (held ? 1 : 0);
    const float r = std::max(2.0f, std::floor(std::min(b1.x - b0.x, b1.y - b0.y) * 0.22f));
    if (side) dl->AddTriangleFilled(ImVec2(cx - r * 0.6f, cy - r - 1), ImVec2(cx - r * 0.6f, cy + r + 1), ImVec2(cx + r * 0.8f, cy), P.text);
    else dl->AddTriangleFilled(ImVec2(cx + r * 0.6f, cy - r - 1), ImVec2(cx + r * 0.6f, cy + r + 1), ImVec2(cx - r * 0.8f, cy), P.text);
    if (click) *start += side ? step : -step;
  }
  // The trough and the thumb.
  const float t0 = p0.x + aw + 1, t1 = p1.x - aw - 1, span = std::max(1.0f, t1 - t0);
  bevelBox(dl, ImVec2(t0, p0.y), ImVec2(t1, p1.y), true);
  const double tot = std::max(total, 1e-9);
  float th0 = t0 + static_cast<float>(span * std::clamp(*start, 0.0, most) / tot);
  float th1 = std::min(t1, th0 + std::max(static_cast<float>(span * std::min(1.0, view / tot)), h * 1.5f));
  th0 = std::round(std::min(th0, th1 - h * 1.5f)), th1 = std::round(th1);
  ImGui::SetCursorScreenPos(ImVec2(t0, p0.y));
  ImGui::InvisibleButton("##trough", ImVec2(span, std::max(1.0f, h)));
  static float grab = -1;
  static ImGuiID grabId = 0;
  const ImGuiID me = ImGui::GetItemID();
  if (ImGui::IsItemActivated()) {
    const float mx = io.MousePos.x;
    if (mx >= th0 && mx < th1) {
      grab = mx - th0, grabId = me;
    } else {
      grab = -1;
      *start += mx < th0 ? -view : view;
    }
  }
  if (ImGui::IsItemActive() && grab >= 0 && grabId == me) *start = (io.MousePos.x - grab - t0) / span * tot;
  if (!ImGui::IsItemActive() && grabId == me) grab = -1, grabId = 0;
  if (hint) ui::Hint(hint);
  bevelBox(dl, ImVec2(th0, p0.y + 1), ImVec2(th1, p1.y - 1), false);
  ImGui::PopID();
  *start = std::clamp(*start, 0.0, most);
  return *start != was;
}

// One stepper row: the label, the value in a sunken field right after it, the arrows at the box's
// right edge (down, up). `w` is the row's width, `labelW` the label column's.
int stepper(App& a, const char* id, const char* label, const std::string& value, ImVec2 p0, float w, float h, const char* hint, float labelW, bool fill,
            double* typed) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  dl->AddText(a.fontRegular, fs, ImVec2(p0.x, std::round(p0.y + (h - fs) * 0.5f)), P.textDim, i18n::tr(label));
  // T-21: a row too narrow for its label, value and two full arrows narrows the arrows first.
  const float need0 = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(label)).x + mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x + 16;
  const float bw = std::max(std::round(h * 0.8f), std::min(std::round(h * 1.35f), std::floor((w - need0) / 2)));
  const float x2 = p0.x + w - bw, x1 = x2 - bw;
  float vw = std::round(mono->CalcTextSizeA(fs, FLT_MAX, 0, "000.0").x + 8);
  const float lx = labelW >= 0 ? labelW : std::round(w * 0.42f);
  if (fill) vw = std::max(vw, x1 - 2 - (p0.x + lx));  // the field runs from the label column to the arrows
  float v0 = std::min(p0.x + lx, x1 - 2 - vw);
  // A narrow box: the field gives way to the label (down to its value's own width), never over it.
  const float labelEnd = std::ceil(p0.x + a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(label)).x + 4);
  if (v0 < labelEnd) {
    v0 = labelEnd;
    vw = std::max(std::round(mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x + 6), x1 - 2 - v0);
  }
  int r = 0;
  ImGui::PushID(id);
  bevelBox(dl, ImVec2(v0, p0.y), ImVec2(v0 + vw, p0.y + h), true);
  const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x;
  dl->AddText(mono, fs, ImVec2(std::round(v0 + vw - 4 - tw), std::round(p0.y + (h - fs) * 0.5f)), valueInk(), value.c_str());
  static ImGuiID editing = 0;
  static char ebuf[24] = {};
  const ImGuiID vid = ImGui::GetID("##val");
  if (typed && editing == vid) {
    dl->AddRectFilled(ImVec2(v0 + 2, p0.y + 2), ImVec2(v0 + vw - 2, p0.y + h - 2), ImGui::GetColorU32(ImGuiCol_FrameBg));
    ImGui::SetCursorScreenPos(ImVec2(v0 + 1, std::round(p0.y + (h - fs) * 0.5f) - ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(vw - 2);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushFont(mono);
    if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##valedit", ebuf, sizeof ebuf,
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsDecimal);
    const bool done = enter || ImGui::IsItemDeactivated();
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (done) {
      editing = 0;
      char* end = nullptr;
      const double v = std::strtod(ebuf, &end);
      if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && end != ebuf && std::isfinite(v)) *typed = v;
    }
  } else {
    ImGui::SetCursorScreenPos(ImVec2(v0, p0.y));
    // A click (no drag) on a typed value opens it for typing (K-297 round 7).
    const bool clicked = ImGui::InvisibleButton("##val", ImVec2(vw, h));
    if (clicked && typed && ImGui::GetStateStorage()->GetFloat(ImGui::GetItemID() + 1, 0.0f) < 3.0f) {
      editing = vid;
      std::snprintf(ebuf, sizeof ebuf, "%s", value.c_str());
    }
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) r = ImGui::GetIO().MouseWheel > 0 ? 1 : -1;
    // Dragging the value steps it: a step every few pixels, right or up for more.
    if (ImGui::IsItemActive()) {
      ImGuiStorage* st = ImGui::GetStateStorage();
      const ImGuiID key = ImGui::GetItemID();
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      float acc = st->GetFloat(key, 0.0f) + (d.x - d.y) / std::max(4.0f, std::round(fs * 0.5f));
      const int n = static_cast<int>(acc);
      if (n != 0) r = n, acc -= static_cast<float>(n);
      st->SetFloat(key, acc);
      st->SetFloat(key + 1, st->GetFloat(key + 1, 0.0f) + std::fabs(d.x) + std::fabs(d.y));  // how far it moved (a drag is no click)
      if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if (!clicked) {
      ImGui::GetStateStorage()->SetFloat(ImGui::GetItemID(), 0.0f);
      ImGui::GetStateStorage()->SetFloat(ImGui::GetItemID() + 1, 0.0f);
    }
    if (hint) ui::Hint(hint);
  }
  if (arrowButton(a, "##up", ImVec2(x1, p0.y), ImVec2(x1 + bw, p0.y + h), true, hint)) r = 1;
  if (arrowButton(a, "##dn", ImVec2(x2, p0.y), ImVec2(x2 + bw, p0.y + h), false, hint)) r = -1;
  ImGui::PopID();
  return r;
}

namespace {

// A read-only value row (label, value in a sunken field at the right).
void readout(App& a, const char* label, const std::string& value, ImVec2 p0, float w, float h, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const float ty = std::round(p0.y + (h - fs) * 0.5f);
  dl->AddText(a.fontRegular, fs, ImVec2(p0.x, ty), P.textDim, i18n::tr(label));
  const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x;
  dl->AddText(mono, fs, ImVec2(std::round(p0.x + w - tw), ty), valueInk(), value.c_str());
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(label, ImVec2(std::max(1.0f, w), h));
  if (hint) ui::Hint(hint);
}

// What the Loop to stepper needs besides its arrows (as stepper() measures it, for a two-digit value).
float loopNeed(App& a, const char* label) {
  const float fs = ImGui::GetFontSize();
  return a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(label)).x + monoFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, "00").x + 16;
}

// The Rows arrows: the pattern a bar longer or shorter; R4-07: at most 256 rows, as Expand.
void stepRows(App& a, const Pattern* p, int d) {
  if (!p || d == 0) return;
  const int bars = std::max(1, p->bars + d);
  const int perBar = std::max(1, trk::gridOf(a.doc.song(), *p).rows / std::max(1, p->bars));
  if (d > 0 && bars * perBar > 256) {
    // R5-09: in the status line, where Expand says the same.
    status(i18n::tr("A pattern is at most 256 rows long"));
    return;
  }
  // R6-02: the song keeps its order: the entries after this pattern's move along.
  if (bars != p->bars) trk::setPatternLengthInSong(a.doc, p->id, bars, p->stepsPerBar);
}

// The entry at the song position (outside the song: the first entry of the shown pattern).
int currentEntry(App& a, const std::vector<trk::OrderEntry>& ol) {
  // The entry under the song position when it plays the pattern shown (or while the song plays);
  // otherwise the pattern shown's first entry, so the list and the editor always agree.
  const Tick pos = songPosition(a);
  int at = -1;
  for (size_t i = 0; i < ol.size() && at < 0; ++i)
    if (pos >= ol[i].start && pos < ol[i].end) at = static_cast<int>(i);
  // N-01: the entry under the song position is the current one, always (an audio part too), so Ins,
  // Del and the arrows act where the song is; syncOrder keeps the position on the shown pattern.
  if (at >= 0) return at;
  for (size_t i = 0; i < ol.size(); ++i)
    if (ol[i].pattern == a.curPattern) return static_cast<int>(i);
  return at;
}

// The order list rows (both the header box and the part on its own).
void orderRows(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  // R10-05: five rows where they fit, as the classic list (two entries above the current one, two
  // below): the rows and their text tighten down to about four fifths of the text size for them.
  // cp14: the entries in the band's own text size, as the classic list (its digits are the size of
  // every other label there); the header gives the box the height for that (orderListNeed). Only a
  // very short band tightens them, down to about nine tenths.
  float rowH = std::round(fs + 1), rf = fs;
  if (std::floor((p1.y - p0.y - 2) / rowH) < 5) {
    const float r5 = std::floor((p1.y - p0.y - 2) / 5);
    if (r5 >= std::round(fs * 0.78f) + 1) rowH = r5, rf = std::min(fs, std::max(std::round(fs * 0.9f), r5 - 1));
  }
  const std::vector<trk::OrderEntry> ol = trk::orderList(s);
  if (ol.empty()) {
    const bool other = std::any_of(s.playlist.begin(), s.playlist.end(), [](const Clip& c) { return c.kind != ClipKind::Pattern; });
    // Wrapped: the box is narrow.
    dl->AddText(a.fontRegular, rf, ImVec2(p0.x + 4, p0.y + 2), P.textFaint,
                i18n::tr(other ? "No pattern clips. Automation clips are on the Song page." : "The song is empty: type notes, or Ins puts the pattern in"), nullptr,
                p1.x - p0.x - 8);
    return;
  }
  const int current = currentEntry(a, ol);
  // R7-11: whole rows only, an odd count, the current entry in the middle (as the classic order
  // list: blank rows above the first entry and below the last), the rows centred in the box.
  int vis = std::max(1, static_cast<int>(std::floor((p1.y - p0.y - 2) / rowH)));
  if (vis > 1 && vis % 2 == 0) --vis;
  const int first = std::max(0, current) - vis / 2;
  const float top = std::round(p0.y + (p1.y - p0.y - static_cast<float>(vis) * rowH) * 0.5f);
  dl->PushClipRect(p0, p1, true);
  for (int k = 0; k < vis && first + k < static_cast<int>(ol.size()); ++k) {
    const int i = first + k;
    if (i < 0) continue;
    const trk::OrderEntry& e = ol[static_cast<size_t>(i)];
    const float y = top + static_cast<float>(k) * rowH;
    if (i == current) dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p1.x, y + rowH), theme::withAlpha(P.hot, 0.85f));
    const ImU32 col = i == current ? theme::contrastText(P.hot) : P.text;
    const Pattern* pat = s.pattern(e.pattern);
    // T-13: an audio part (no pattern plays there) shows "AU" and its top clip's channel; a pattern
    // entry with audio under it is marked with a "~" after its number.
    const bool audioPart = e.pattern == kNoId;
    std::string name = pat ? pat->name : std::string();
    if (audioPart && !e.audio.empty())
      if (const Clip* c = s.clip(e.audio.front()))
        if (const Channel* ch = s.channel(c->source)) name = ch->name;
    char line[96];
    std::snprintf(line, sizeof line, "%s  %s%s", trk::hex2(i).c_str(), audioPart ? "AU" : trk::hex2(trk::patternNumber(s, e.pattern)).c_str(),
                  e.patterns > 1 ? "+" : !audioPart && !e.audio.empty() ? "~" : " ");
    dl->AddText(mono, rf, ImVec2(p0.x + 4, std::round(y + (rowH - rf) * 0.5f)), col, line);
    const float lw = mono->CalcTextSizeA(rf, FLT_MAX, 0, line).x;
    if (!name.empty())
      theme::textEllipsis(dl, a.fontRegular, rf, ImVec2(p0.x + 4 + lw + 6, std::round(y + (rowH - rf) * 0.5f)), p1.x - (p0.x + 4 + lw + 10),
                          i == current ? col : P.textDim, name.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##entry", ImVec2(std::max(1.0f, p1.x - p0.x), rowH))) {
      if (!audioPart) a.selectPattern(e.pattern);
      setSongPosition(a, e.start);  // R4-01: kept while a pattern plays (Stop keeps it)
    }
    ImGui::PopID();
    ui::Hint(audioPart ? "Only audio clips play here (no pattern) \xC2\xB7 click: go there \xC2\xB7 the Song page edits them"
             : e.patterns > 1 ? "Several patterns play here (the top track's is shown) \xC2\xB7 click: go there"
             : !e.audio.empty() ? "A part of the song with audio clips under its pattern (~) \xC2\xB7 click: go there and show its pattern"
                                : "A part of the song (cut where clips start and end) \xC2\xB7 click: go there and show its pattern");
  }
  // The free entries after the song's end: their numbers, faint (the list keeps its rows filled).
  for (int i = static_cast<int>(ol.size()); i - first < vis; ++i) {
    const float y = top + static_cast<float>(i - first) * rowH;
    dl->AddText(mono, rf, ImVec2(p0.x + 4, std::round(y + (rowH - rf) * 0.5f)), theme::withAlpha(P.textFaint, 0.6f), trk::hex2(i).c_str());
  }
  dl->PopClipRect();
}

// The page strip: the Gridline pages the classic tracker has no screen for. Instrument, Sample,
// Files, Settings and Help are switched from the transport column (one control each).
struct Page {
  const char* label;
  const char* window;
  const char* hint;
};
const Page kPages[] = {
    {i18n::N_("Pattern"), "Pattern editor", i18n::N_("The pattern in rows (Esc: edit mode, F1 to F8: octave)")},
    {i18n::N_("Song"), "Playlist", i18n::N_("The arrangement, a row per bar")},
    {i18n::N_("Roll"), "Piano roll", i18n::N_("The same notes in the piano roll")},
    {i18n::N_("Steps"), "Channel rack", i18n::N_("The channels and their steps")},
    {i18n::N_("Mix"), "Mixer", i18n::N_("The mixer")},
    {i18n::N_("Pads"), "Live pads", i18n::N_("Clips launched by hand")},
    // T-11: the audio editor docked in the centre is a page too, lit while it is in front.
    {i18n::N_("Audio"), "Loupe", i18n::N_("The audio editor: the selected channel's sample")},
};

bool windowFront(App& a, const char* window) {
  const bool* f = layoutui::windowOpenFlag(a, window);
  const bool loupe = std::strcmp(window, "Loupe") == 0;  // pinned by a setting, not a window flag
  if (loupe ? !a.settings.getBool("ui.loupePinned") : (!f || !*f)) return false;
  // A page docked with others is lit only while it is the tab in front.
  const ImGuiWindow* w = ImGui::FindWindowByName((std::string("###") + window).c_str());  // "Label###English"
  return !w || !w->DockIsActive || !w->DockNode || w->DockNode->VisibleWindow == w;
}

bool pageOn(App& a, const Page& pg) { return windowFront(a, pg.window); }

// The header's wordmark, a band filling the slot [p0, p1]: the whole slot in the fixed Gridline
// brand orange (255, 181, 71; never the chosen accent or theme), lit by a one pixel line along the
// top and shaded by one along the bottom (both from that orange), so it reads as a raised bar. At
// the left, the Tile mark inverted (the fixed brand dark (38, 44, 48), its cut-outs in the orange),
// square, as tall as the slot less a 3 px margin (the same margin at the left).
// Then "GRIDLINE TRACKER" in capitals, JetBrains Mono Bold, in the same dark colour and no
// shadow: the largest size whose cap ink fits the height within the margins and whose line fits
// the width, with the letters spread evenly (the space counts as a letter, at most a third of a
// letter apart) so the line runs from 5 px after the mark to the right margin, or sits centred
// between them in a wide band. Everything sits on device pixels, so crisp at any
// scale; a slot squeezed narrow shrinks the text, which stays inside the bar.
void logoLine(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const float dpp = pixelgrid::frameScale();
  p0 = ImVec2(pixelgrid::edge(p0.x, dpp), pixelgrid::edge(p0.y, dpp));
  p1 = ImVec2(pixelgrid::edge(p1.x, dpp), pixelgrid::edge(p1.y, dpp));
  const float px = pixelgrid::whole(1.0f, dpp);
  const ImU32 brand = IM_COL32(255, 181, 71, 255);  // the Gridline brand orange and dark: fixed, whatever accent or theme is chosen
  const ImU32 dark = IM_COL32(38, 44, 48, 255);
  dl->AddRectFilled(p0, p1, brand);
  dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + px), theme::mix(brand, IM_COL32(255, 255, 255, 255), 0.45f));
  dl->AddRectFilled(ImVec2(p0.x, p1.y - px), p1, theme::mix(brand, IM_COL32(0, 0, 0, 255), 0.30f));
  const float m = pixelgrid::whole(3.0f, dpp);
  const float mk = pixelgrid::edge(p1.y - p0.y - m * 2, dpp);
  if (mk < 6 || p1.x - p0.x < mk + m * 3) return;  // a band squeezed too small for any wordmark
  theme::drawMark(dl, ImVec2(p0.x + m, p0.y + m), mk, dark, brand);
  // The text, in capitals (the second word translated, then upper-cased).
  std::string text = std::string("GRIDLINE ") + i18n::tr("tracker");
  for (char& c : text)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  ImFont* font = a.fontMonoBold ? a.fontMonoBold : monoFont(a);
  const float x0 = p0.x + m + mk + pixelgrid::whole(5.0f, dpp), x1 = p1.x - m;
  const float availW = x1 - x0, availH = p1.y - p0.y - m * 2;
  if (availW < 10) return;
  const int glyphs = static_cast<int>(text.size());
  auto adv = [](ImFontBaked* fb, char c) { return fb->GetCharAdvance(static_cast<ImWchar>(static_cast<unsigned char>(c))); };
  auto find = [](ImFontBaked* fb, char c) { return fb->FindGlyph(static_cast<ImWchar>(static_cast<unsigned char>(c))); };
  // At a size: the cap ink's top and bottom below the pen's top ("H"), and the ink's width (the
  // first glyph's left bearing and the last one's right side left out).
  auto capInk = [&](float fs, float* top, float* bottom) {
    const ImFontGlyph* g = font->GetFontBaked(fs)->FindGlyph('H');
    *top = g ? g->Y0 : fs * 0.2f;
    *bottom = g ? g->Y1 : fs * 0.9f;
  };
  auto inkW = [&](float fs) {
    ImFontBaked* fb = font->GetFontBaked(fs);
    float w = 0;
    for (char c : text) w += adv(fb, c);
    const ImFontGlyph* f = find(fb, text.front());
    const ImFontGlyph* l = find(fb, text.back());
    return w - (f ? f->X0 : 0.0f) - (l ? adv(fb, text.back()) - l->X1 : 0.0f);
  };
  float fs = std::floor(availH * 1.6f), top = 0, bottom = 0;
  capInk(fs, &top, &bottom);
  while (fs > 6 && (bottom - top > availH || inkW(fs) > availW)) capInk(fs -= 1, &top, &bottom);
  // The spare width spread evenly between the glyphs: the last glyph's ink ends at the margin.
  ImFontBaked* fb = font->GetFontBaked(fs);
  // T-22: the spread stops at a third of a letter; a wider band centres the line in it instead of
  // letter-spacing it across.
  const float track = std::min(std::max(0.0f, (availW - inkW(fs)) / static_cast<float>(std::max(1, glyphs - 1))), adv(fb, 'G') * 0.34f);
  const float lineW = inkW(fs) + track * static_cast<float>(std::max(0, glyphs - 1));
  const float y = pixelgrid::edge(p0.y + m + (availH - (bottom - top)) * 0.5f - top, dpp);
  float x = std::round(x0 + std::max(0.0f, (availW - lineW) * 0.5f));
  if (const ImFontGlyph* f = find(fb, text.front())) x -= f->X0;  // the first glyph's ink starts at x0
  for (char c : text) {
    const char one[2] = {c, 0};
    if (c != ' ') dl->AddText(font, fs, ImVec2(pixelgrid::edge(x, dpp), y), dark, one);
    x += adv(fb, c) + track;
  }
}

// What the scope box shows: the channel scopes, or a tool swapped in (one at a time).
enum class ScopeTool { Scopes, Transpose, Trim, AdvEdit, Midi, Sample };
ScopeTool gTool = ScopeTool::Scopes;

void scopes(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const int n = static_cast<int>(s.channels.size());
  if (n == 0) {
    dl->AddText(a.fontRegular, fs, ImVec2(p0.x + 6, p0.y + 4), P.textFaint, i18n::tr("No channels"));
    return;
  }
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  // As many scopes as stay readable (a name's few letters each); more channels: the page of them
  // holding the selected channel.
  const int all = n;
  // Always two rows, half the tracks in each (the classic scope box: 4 tracks 2x2, 8 tracks 4x2).
  const int rows = all > 1 ? 2 : 1;
  const int maxCols = std::max(1, static_cast<int>(W / std::max(24.0f, fs * 3.2f)));
  const int perPage = rows * maxCols;
  const int sel = std::max(0, s.channelIndex(a.selChannel));
  const int first = all > perPage ? (sel / perPage) * perPage : 0;
  const int shown = std::min(all - first, perPage);
  const int cols = std::max(1, std::min(maxCols, (shown + rows - 1) / rows));
  const float cw = std::floor((W - static_cast<float>(cols - 1)) / static_cast<float>(cols));
  const float ch = std::floor((H - static_cast<float>(rows - 1)) / static_cast<float>(rows));
  static ops::ChannelSolo solo;
  static ObjectId gScopePress = kNoId;  // the scope a press started on (its buttons decide on release)
  static bool gScopeLeft = false, gScopeRight = false, gScopeSoloed = false;
  const bool multi = a.settings.getBool("tracker.multiEdit");
  const bool lines = a.settings.getBool("tracker.scopeLines");
  // cp14: each scope draws its channel's own output (the channel's tap, before its fader) over the
  // last few hundredths of a second, as the classic scopes draw each channel's sample: points, or a
  // joined line with Settings, Layout, Scopes as lines. Stopped or silent, the centre line alone.
  static std::vector<float> wave;
  const bool playing = a.engine.playing();
  const size_t span = std::clamp<size_t>(static_cast<size_t>(std::max(1.0, a.engine.sampleRate()) / 40.0), 64, ScopeTap::kSize);
  // K-106 (showcase): the names are shown when every one fits its scope whole, else none (the numbers
  // alone, as the classic scopes; the name is in each scope's hint and the pattern's track header):
  // a row of half names ("2 Sn...", "10 ...") read as clipped text.
  bool namesFit = true;
  for (int i = 0; i < shown && namesFit; ++i) {
    char num[16];
    std::snprintf(num, sizeof num, "%d", first + i + trackBase(a));
    const float room = cw - 14 - mono->CalcTextSizeA(fs, FLT_MAX, 0, num).x;
    namesFit = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, s.channels[static_cast<size_t>(first + i)].name.c_str()).x <= room;
  }
  for (int i = 0; i < shown; ++i) {
    const Channel& c = s.channels[static_cast<size_t>(first + i)];
    const float cx = p0.x + static_cast<float>(i % cols) * (cw + 1), cy = p0.y + static_cast<float>(i / cols) * (ch + 1);
    const ImVec2 c0(cx, cy), c1(i % cols == cols - 1 ? p1.x : cx + cw, i / cols == rows - 1 ? p1.y : cy + ch);
    bevelBox(dl, c0, c1, true);
    const float mid = std::round((c0.y + fs + 2 + c1.y) * 0.5f);
    const float amp = std::max(1.0f, (c1.y - c0.y - fs - 8) * 0.5f);
    if (c.muted) {  // a muted channel is crossed out
      dl->AddLine(ImVec2(c0.x + 3, c0.y + fs + 4), ImVec2(c1.x - 3, c1.y - 3), theme::withAlpha(P.textDim, 0.6f));
      dl->AddLine(ImVec2(c0.x + 3, c1.y - 3), ImVec2(c1.x - 3, c0.y + fs + 4), theme::withAlpha(P.textDim, 0.6f));
    } else {
      const float x0 = c0.x + 3, x1 = c1.x - 3;
      const int px = std::max(1, static_cast<int>(x1 - x0));
      const auto rt = playing ? a.builder.channelRuntime(c.id) : nullptr;
      bool have = false;
      if (rt) {
        wave.resize(span);
        have = rt->tap.read(wave.data(), span);
      }
      float peakV = 0;
      for (size_t k = 0; have && k < span; ++k) peakV = std::max(peakV, std::fabs(wave[k]));
      if (!have || peakV < 1e-4f) {
        dl->AddRectFilled(ImVec2(x0, mid), ImVec2(x1, mid + 1), theme::withAlpha(P.led, 0.55f));
      } else {
        const ImU32 ink = theme::withAlpha(P.led, 0.95f);
        ImVec2 prev;
        for (int k = 0; k <= px; ++k) {
          const size_t at = std::min(span - 1, static_cast<size_t>(k) * span / static_cast<size_t>(px + 1));
          const float v = std::clamp(wave[at], -1.0f, 1.0f);
          const ImVec2 pt(x0 + static_cast<float>(k), std::round(mid - v * amp));
          if (lines) {
            if (k > 0) dl->AddLine(ImVec2(prev.x + 0.5f, prev.y + 0.5f), ImVec2(pt.x + 0.5f, pt.y + 0.5f), ink, 1.0f);
          } else {
            dl->AddRectFilled(pt, ImVec2(pt.x + 1, pt.y + 1), ink);
          }
          prev = pt;
        }
      }
    }
    char label[16];
    std::snprintf(label, sizeof label, "%d", first + i + trackBase(a));
    dl->AddText(mono, fs, ImVec2(c0.x + 4, c0.y + 2), c.muted ? P.textFaint : P.text, label);
    const float lw = mono->CalcTextSizeA(fs, FLT_MAX, 0, label).x;
    float nx = c0.x + 10 + lw;
    if (!c.muted && ops::channelSoloed(s, c.id, solo)) {
      dl->AddText(a.fontRegular, fs * 0.85f, ImVec2(nx, c0.y + 3), P.solo, i18n::tr("solo"));
      nx += a.fontRegular->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, i18n::tr("solo")).x + 6;
    }
    // The channel's name, so a scope says whose it is.
    if (namesFit && c1.x - 4 - nx > 12) theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, c0.y + 2), c1.x - 4 - nx, c.muted ? P.textFaint : P.textDim, c.name.c_str());
    ImGui::SetCursorScreenPos(c0);
    ImGui::PushID(i);
    // M-32 to M-34 (R5-01): the press decides, on the scope it started on: a left click mutes or
    // unmutes; a right-click turns the track on or off for multichannel edit (a muted one is unmuted
    // and turned on); both buttons together, or Alt and a right-click, solo it (again: every channel
    // back). The action waits for the release, so a right press that becomes both buttons only solos.
    ImGui::InvisibleButton("##scope", ImVec2(std::max(1.0f, c1.x - c0.x), std::max(1.0f, c1.y - c0.y)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool downL = ImGui::IsMouseDown(ImGuiMouseButton_Left), downR = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) && gScopePress == kNoId) {
      gScopePress = c.id;
      gScopeLeft = gScopeRight = gScopeSoloed = false;
    }
    // R6-09: a press this scope missed (the frame it came in, the band was not yet hovered: another
    // window had the mouse, or the window was not yet focused) still counts on its release, when it
    // was pressed inside this scope.
    if (gScopePress == kNoId && !downL && !downR) {
      const ImGuiIO& io = ImGui::GetIO();
      const ImRect box(c0, c1);
      for (int b : {ImGuiMouseButton_Left, ImGuiMouseButton_Right})
        if (ImGui::IsMouseReleased(b) && box.Contains(io.MouseClickedPos[b]) && box.Contains(io.MousePos)) {
          gScopePress = c.id;
          gScopeLeft = b == ImGuiMouseButton_Left, gScopeRight = b == ImGuiMouseButton_Right, gScopeSoloed = false;
        }
    }
    if (gScopePress == c.id) {
      gScopeLeft = gScopeLeft || downL;
      gScopeRight = gScopeRight || downR;
      if (downL && downR && !gScopeSoloed) {
        ops::toggleChannelSolo(a.doc, c.id, solo);
        gScopeSoloed = true;
      }
      if (!downL && !downR) {  // released: a single button's click acts now
        if (!gScopeSoloed && gScopeLeft != gScopeRight) {
          if (gScopeLeft) {
            a.doc.setParam(channelParam(c.id, ChannelParam::Mute), c.muted ? 0 : 1, false);
          } else if (ImGui::GetIO().KeyAlt) {
            ops::toggleChannelSolo(a.doc, c.id, solo);
          } else {
            const bool on = multiTrackOn(a, c.id);
            if (c.muted) {
              a.doc.setParam(channelParam(c.id, ChannelParam::Mute), 0, false);
              if (!on) toggleMultiTrack(a, c.id);
            } else {
              toggleMultiTrack(a, c.id);
            }
            // R5-06: said, since the tag only shows while multichannel edit is on.
            const bool nowOn = multiTrackOn(a, c.id);
            std::string msg = c.name + ": " + i18n::tr(nowOn ? "in multichannel edit" : "left out of multichannel edit");
            if (!multi) msg += std::string(" (") + i18n::tr("Multichannel edit is off in Settings") + ")";
            a.notify(msg);
          }
        }
        gScopePress = kNoId;
        gScopeSoloed = false;
      }
    }
    ImGui::PopID();
    // R5-06: a track on for multichannel edit carries a REC tag at its bottom left (ref 167).
    if (multi && multiTrackOn(a, c.id) && !c.muted) {
      const float ts = std::max(9.0f, std::round(fs * 0.72f));
      const char* rec = i18n::tr("REC");
      const ImVec2 tz = a.fontRegular->CalcTextSizeA(ts, FLT_MAX, 0, rec);
      const ImVec2 r0(c0.x + 3, c1.y - 3 - tz.y - 2), r1(c0.x + 3 + tz.x + 6, c1.y - 3);
      if (r1.x < c1.x - 2 && r0.y > c0.y + fs + 2) {
        dl->AddRectFilled(r0, r1, P.arm, 2.0f);
        dl->AddText(a.fontBold ? a.fontBold : a.fontRegular, ts, ImVec2(r0.x + 3, r0.y + 1), theme::contrastText(P.arm), rec);
      }
    }
    std::string tip = c.name + " \xC2\xB7 " + i18n::tr("click: mute or unmute \xC2\xB7 right-click: on or off for multichannel edit \xC2\xB7 both buttons, or Alt and right-click: solo");
    ui::Hint(tip.c_str());
  }
  // A press that ended outside every scope (moved off, or the page changed) is forgotten.
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) gScopePress = kNoId;
}

// The classic Transpose box (ref 149): a header row (the title, then "current instrument" and "all
// instruments" over their halves), four scope rows (track, pattern, song, block), and in each half
// up a semitone, down, up an octave, down (the octave buttons wider).
void transposeTool(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float rows = 5, gap = 2;
  const float rh = std::floor((p1.y - p0.y - 4 - gap * (rows - 1)) / rows);
  struct Scope {
    const char* label;
    TransposeScope scope;
    const char* hint;
  };
  const Scope sc[] = {{i18n::N_("Track"), TransposeScope::Track, i18n::N_("The cursor's track")},
                      {i18n::N_("Pattern"), TransposeScope::Pattern, i18n::N_("Every track of the pattern shown")},
                      {i18n::N_("Song"), TransposeScope::Song, i18n::N_("Every pattern of the song")},
                      {i18n::N_("Block"), TransposeScope::Block, i18n::N_("The marked block (or the cursor's row)")}};
  float labelW = 0;
  for (const Scope& r : sc) labelW = std::max(labelW, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(r.label)).x);
  labelW = std::round(std::max(labelW + 12, (p1.x - p0.x) * 0.18f));
  const float x0 = p0.x + 2, xs = x0 + labelW, halfW = std::floor((p1.x - 2 - xs - gap) / 2);
  // Header: three raised boxes (the title, then the two halves).
  const float hy0 = p0.y + 2, hy1 = hy0 + rh;
  const char* heads[] = {i18n::N_("Transpose"), i18n::N_("Current instrument"), i18n::N_("All instruments")};
  const float hx[] = {x0, xs, xs + halfW + gap, p1.x - 2};
  for (int h = 0; h < 3; ++h) {
    const float hx1 = h == 0 ? xs - gap : h == 1 ? xs + halfW : hx[3];
    bevelBox(dl, ImVec2(hx[h], hy0), ImVec2(hx1, hy1));
    ImFont* f = h == 0 && a.fontBold ? a.fontBold : a.fontRegular;
    const char* t = i18n::tr(heads[h]);
    const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    const float tx = h == 0 ? hx[h] + 4 : std::round(hx[h] + std::max(4.0f, (hx1 - hx[h] - tw) * 0.5f));
    dl->PushClipRect(ImVec2(hx[h], hy0), ImVec2(hx1, hy1), true);
    dl->AddText(f, fs, ImVec2(tx, std::round(hy0 + (rh - fs) * 0.5f)), h == 0 ? P.text : P.textDim, t);
    dl->PopClipRect();
  }
  const int steps[] = {1, -1, 12, -12};
  const char* names[] = {"+1", "-1", "+12", "-12"};
  // Up and down a semitone narrow, the octave buttons wider (as the classic 21 / 21 / 36 / 36).
  const float unit = (halfW - gap * 3) / 114.0f;
  const float bw[] = {std::floor(21 * unit), std::floor(21 * unit), std::floor(36 * unit), 0};
  for (int r = 0; r < 4; ++r) {
    const float y = std::round(hy1 + gap + static_cast<float>(r) * (rh + gap));
    const float yb = r == 3 ? p1.y - 2 : y + rh;
    dl->AddText(a.fontRegular, fs, ImVec2(x0 + 4, std::round(y + (yb - y - fs) * 0.5f)), P.textDim, i18n::tr(sc[r].label));
    for (int half = 0; half < 2; ++half) {
      const bool cur = half == 0;
      float x = xs + static_cast<float>(half) * (halfW + gap);
      const float xEnd = half == 0 ? xs + halfW : p1.x - 2;
      for (int k = 0; k < 4; ++k) {
        const float xr = k == 3 ? xEnd : x + bw[k];
        ImGui::PushID(r * 16 + half * 8 + k);
        // R6-07: a track is one instrument here, so the current instrument's Track is the same as
        // every instrument's Track (off, saying so); on Pattern and Block it is the instrument's own
        // track there, on Song its track in every pattern.
        const bool same = cur && sc[r].scope == TransposeScope::Track;
        const std::string hint = same ? std::string(i18n::tr("A track holds one instrument: use All instruments, Track"))
                                      : std::string(i18n::tr(sc[r].hint)) + " \xC2\xB7 " +
                                            i18n::tr(cur ? "only the current instrument's track (each track is one instrument)" : "every instrument's notes");
        if (same) ImGui::BeginDisabled();
        if (bevelButton(a, "##tr", names[k], ImVec2(x, y), ImVec2(xr, yb), false, hint.c_str()) && !same) transposeScope(a, sc[r].scope, steps[k], cur);
        if (same) ImGui::EndDisabled();
        ImGui::PopID();
        x = xr + gap;
      }
    }
  }
}

extern ObjectId gSourceChannel;
extern int gSourceSlot;
bool copyInstrumentSound(App& a, ObjectId dst, ObjectId src, bool exchange);

// ---- Clean up (the classic Trim box): what to remove at the left, the size estimate and the two
// buttons at the right. Calculate works the clean-up out on a copy of the song and shows the
// project's size before and after; Clean up asks, then runs the ticked clean-ups as one undo step.
struct TrimPick {
  bool patterns = true, tracks = true, mixer = true, automation = true;
  bool samples = true, afterLoop = true, eightBit = false;  // cp14: as the classic trim's defaults
};
TrimPick gTrim;
long long gSizeNow = -1, gSizeAfter = -1;  // bytes (-1: not worked out since the song last changed)
uint64_t gSizeRev = 0;

// The tracks no pattern uses (no notes, no steps, no audio clip plays from them), never all of them.
std::vector<ObjectId> unusedTracks(const Song& s) {
  std::vector<ObjectId> out;
  for (ObjectId id : ops::unusedChannels(s)) {
    const bool audio = std::any_of(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return c.source == id; });
    if (!audio) out.push_back(id);
  }
  if (!out.empty() && out.size() >= s.channels.size()) out.erase(out.begin());  // the song keeps a track
  return out;
}

struct TrimCount {
  int patterns = 0, tracks = 0, mixer = 0, automation = 0, samples = 0;  // samples: sample edits (gone, shortened, 8-bit)
  int total() const { return patterns + tracks + mixer + automation + samples; }
};
TrimCount runTrim(Document& d, const TrimPick& t) {
  TrimCount n;
  d.beginGroup("clean up");
  if (t.patterns) n.patterns = ops::deleteUnusedPatterns(d);
  if (t.tracks) {
    const std::vector<ObjectId> gone = unusedTracks(d.song());
    if (!gone.empty() && ops::removeChannels(d, gone)) n.tracks = static_cast<int>(gone.size());
  }
  if (t.automation) n.automation = ops::deleteUnusedAutomationClips(d);
  if (t.mixer) n.mixer = ops::trimUnusedMixerTracks(d);  // last: the tracks removed above free their mixer tracks
  // cp14: the classic trim's sample parts, on every Tracker Sampler left.
  if (t.samples || t.afterLoop || t.eightBit) {
    std::vector<ObjectId> ids;
    for (const Channel& c : d.song().channels) ids.push_back(c.id);
    for (ObjectId id : ids) {
      const Channel* c = d.song().channel(id);
      if (!c) continue;
      PluginState st = c->plugin;
      packs::trackerpack::SampleTrim k;
      if (!packs::trackerpack::trimSampleState(st, t.samples, t.afterLoop, t.eightBit, &k)) continue;
      d.editChannel(id, "clean up samples", [&st](Channel& ch) { ch.plugin = st; });
      n.samples += k.total();
    }
  }
  d.endGroup(false);
  return n;
}

// The project's size as the estimate counts it: its text and the sample data its Tracker Samplers
// play (each sample's frames as 16-bit values, or 8-bit ones).
long long projectBytes(const Song& s) {
  long long b = static_cast<long long>(projectToText(s).size());
  for (const Channel& c : s.channels) b += packs::trackerpack::sampleBytes(c.plugin);
  return b;
}

std::string sizeText(long long bytes) {
  if (bytes < 0) return i18n::tr("Unknown");
  char b[32];
  if (bytes < 10 * 1024) std::snprintf(b, sizeof b, "%lld B", bytes);
  else if (bytes < 10 * 1024 * 1024) std::snprintf(b, sizeof b, "%lld kB", (bytes + 1023) / 1024);
  else std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return b;
}

void trimTool(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  ImFont* bold = a.fontBold ? a.fontBold : a.fontRegular;
  if (a.doc.revision() != gSizeRev) gSizeNow = gSizeAfter = -1;  // the song changed: the estimate is old
  // cp14: three boxes, as the classic Trim's: the song's parts, the samples' parts, the estimate.
  const float gap = 2, W = p1.x - p0.x;
  const float w1 = std::floor(W * 0.34f), w2 = std::floor(W * 0.33f);
  const ImVec2 l0 = p0, l1(p0.x + w1, p1.y), m0(l1.x + gap, p0.y), m1(l1.x + gap + w2, p1.y), r0(m1.x + gap, p0.y), r1 = p1;
  bevelBox(dl, l0, l1);
  bevelBox(dl, m0, m1);
  bevelBox(dl, r0, r1);
  const float rh = std::floor((p1.y - p0.y - 6) / 6);
  auto rowY = [&](int i) { return std::round(p0.y + 3 + static_cast<float>(i) * rh); };
  const float ty = std::round(rowY(0) + (rh - fs) * 0.5f);
  dl->AddText(bold, fs, ImVec2(l0.x + 6, ty), P.text, i18n::tr("Song parts to remove"));
  dl->AddText(bold, fs, ImVec2(m0.x + 6, ty), P.text, i18n::tr("Samples"));
  struct Pick {
    const char* id;
    const char* label;
    bool* on;
    const char* hint;
  };
  const Pick picks[] = {
      {"##trpat", i18n::N_("Unused patterns"), &gTrim.patterns, i18n::N_("Patterns no clip of the song plays")},
      {"##trtrk", i18n::N_("Unused tracks"), &gTrim.tracks, i18n::N_("Tracks with no notes or steps in any pattern (their instruments go with them)")},
      {"##trmix", i18n::N_("Unused mixer tracks"), &gTrim.mixer, i18n::N_("Empty mixer tracks at the end of the mixer")},
      {"##traut", i18n::N_("Unused automation clips"), &gTrim.automation, i18n::N_("Automation clips the song never plays")},
      {"##trsmp", i18n::N_("Unused samples"), &gTrim.samples, i18n::N_("Samples no key of their instrument plays (the others move up in order)")},
      {"##trlop", i18n::N_("Sample data after loop"), &gTrim.afterLoop, i18n::N_("A looped sample ends where its loop ends (the file is kept)")},
      {"##tr8b", i18n::N_("Samples at 8 bits"), &gTrim.eightBit, i18n::N_("Every sample plays at 8 bits, as a small old module (off by default)")}};
  for (int i = 0; i < 7; ++i) {
    const bool left = i < 4;
    const float y = rowY((left ? i : i - 4) + 1);
    const float x0 = left ? l0.x + 6 : m0.x + 6, x1 = left ? l1.x - 4 : m1.x - 4;
    if (checkRow(a, picks[i].id, picks[i].label, ImVec2(x0, y), ImVec2(x1, y + rh), *picks[i].on, picks[i].hint)) {
      *picks[i].on = !*picks[i].on;
      gSizeNow = gSizeAfter = -1;
    }
  }
  // The estimate: three readouts, the values right-aligned.
  const char* rows[] = {i18n::N_("Project size now"), i18n::N_("Size after clean-up"), i18n::N_("Bytes saved")};
  const long long vals[] = {gSizeNow, gSizeAfter, gSizeNow >= 0 && gSizeAfter >= 0 ? gSizeNow - gSizeAfter : -1};
  for (int i = 0; i < 3; ++i) {
    const float y = std::round(rowY(i) + (rh - fs) * 0.5f);
    dl->AddText(a.fontRegular, fs, ImVec2(r0.x + 6, y), P.textDim, i18n::tr(rows[i]));
    const std::string v = sizeText(vals[i]);
    const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, v.c_str()).x;
    dl->AddText(mono, fs, ImVec2(std::round(r1.x - 6 - tw), y), vals[i] >= 0 ? valueInk() : P.textFaint, v.c_str());
  }
  const bool any = gTrim.patterns || gTrim.tracks || gTrim.mixer || gTrim.automation || gTrim.samples || gTrim.afterLoop || gTrim.eightBit;
  const float by0 = std::round(p1.y - 3 - rh * 1.5f), bw = std::floor((r1.x - r0.x - 8 - gap) / 2);
  if (bevelButton(a, "##trcalc", "Calculate", ImVec2(r0.x + 4, by0), ImVec2(r0.x + 4 + bw, p1.y - 3), false,
                  "Works out the project's size after the clean-up (nothing changes)")) {
    Document tmp(a.doc.song());
    gSizeNow = projectBytes(a.doc.song());
    const TrimCount n = runTrim(tmp, gTrim);
    gSizeAfter = projectBytes(tmp.song());
    gSizeRev = a.doc.revision();
    // R7-14: counts after their names ("Patterns 1"), never "1 patterns".
    a.notify(n.total() ? std::string(i18n::tr("Would go: patterns")) + " " + std::to_string(n.patterns) + ", " + i18n::tr("tracks") + " " + std::to_string(n.tracks) +
                             ", " + i18n::tr("mixer tracks") + " " + std::to_string(n.mixer) + ", " + i18n::tr("automation clips") + " " + std::to_string(n.automation) +
                             ", " + i18n::tr("sample edits") + " " + std::to_string(n.samples)
                       : std::string(i18n::tr("Nothing to clean up")));
  }
  if (!any) ImGui::BeginDisabled();
  if (bevelButton(a, "##trrun", "Clean up", ImVec2(r0.x + 4 + bw + gap, by0), ImVec2(r1.x - 4, p1.y - 3), false,
                  "Removes what is ticked, as one undo step (asks first)")) {
    a.confirm = {};
    a.confirm.open = true;
    a.confirm.title = i18n::tr("Clean up the project?");
    a.confirm.subtitle = i18n::tr("The ticked unused parts are removed. Undo brings them back.");
    a.confirm.okLabel = i18n::N_("Clean up");
    a.confirm.action = [&a] {
      const TrimCount n = runTrim(a.doc, gTrim);
      gSizeNow = gSizeAfter = -1;
      a.notify(n.total() ? std::string(i18n::tr("Cleaned up: ")) + std::to_string(n.total()) + " " + i18n::tr("unused parts removed")
                         : std::string(i18n::tr("Nothing to clean up")));
    };
  }
  if (!any) ImGui::EndDisabled();
}

// ---- Advanced edit (the classic box): the instrument remap at the left (the old instrument, the
// new one, and the four scopes), the clipboard masks at the right (copy, paste and transparent per
// part of a cell, and the switch that turns them on).
ObjectId gRemapOld = kNoId;  // the old instrument (none picked: the instrument list's source mark)

void advEditTool(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float gap = 2, W = p1.x - p0.x;
  const float lw = std::floor(W * 0.42f);
  const ImVec2 l0 = p0, l1(p0.x + lw, p1.y), r0(p0.x + lw + gap, p0.y), r1 = p1;
  bevelBox(dl, l0, l1);
  bevelBox(dl, r0, r1);
  const float rh = std::floor((p1.y - p0.y - 6) / 6);
  auto rowY = [&](int i) { return std::round(p0.y + 3 + static_cast<float>(i) * rh); };
  ImFont* bold = a.fontBold ? a.fontBold : a.fontRegular;
  // Remap.
  dl->AddText(bold, fs, ImVec2(l0.x + 6, std::round(rowY(0) + (rh - fs) * 0.5f)), P.text, i18n::tr("Instrument remap"));
  if (s.channelIndex(gRemapOld) < 0) gRemapOld = s.channelIndex(gSourceChannel) >= 0 ? gSourceChannel : kNoId;
  const int oldI = s.channelIndex(gRemapOld), newI = s.channelIndex(a.selChannel);
  const int n = static_cast<int>(s.channels.size());
  const float sw = lw - 12;
  if (const int d = stepper(a, "remold", "Old number", oldI >= 0 ? trk::hex2(oldI + 1) : std::string("--"), ImVec2(l0.x + 6, rowY(1) + 1), sw, rh - 2,
                            "The instrument whose notes move (the instrument list's source mark sets it too)", sw);  // R7-12: the value against its arrows
      d && n > 0)
    gRemapOld = s.channels[static_cast<size_t>(std::clamp((oldI < 0 ? 0 : oldI + d), 0, n - 1))].id;
  if (const int d = stepper(a, "remnew", "New number", newI >= 0 ? trk::hex2(newI + 1) : std::string("--"), ImVec2(l0.x + 6, rowY(2) + 1), sw, rh - 2,
                            "The instrument they move to: the current instrument (the instrument list picks it too)", sw);
      d && n > 0) {
    const ObjectId to = s.channels[static_cast<size_t>(std::clamp((newI < 0 ? 0 : newI + d), 0, n - 1))].id;
    a.selectChannel(to);
  }
  struct Sc {
    const char* id;
    const char* label;
    TransposeScope scope;
    const char* hint;
  };
  const Sc sc[] = {{"##rmtrk", i18n::N_("Track"), TransposeScope::Track, i18n::N_("In the cursor's track (the old instrument's)")},
                   {"##rmpat", i18n::N_("Pattern"), TransposeScope::Pattern, i18n::N_("In the pattern shown")},
                   {"##rmsng", i18n::N_("Song"), TransposeScope::Song, i18n::N_("In every pattern of the song")},
                   {"##rmblk", i18n::N_("Block"), TransposeScope::Block, i18n::N_("In the marked block")}};
  const float bw = std::floor((lw - 8 - gap) / 2), by0 = rowY(3) + 2, bh = std::floor((p1.y - 3 - by0 - gap) / 2);
  for (int i = 0; i < 4; ++i) {
    const float x = l0.x + 4 + static_cast<float>(i % 2) * (bw + gap), y = by0 + static_cast<float>(i / 2) * (bh + gap);
    if (bevelButton(a, sc[i].id, sc[i].label, ImVec2(x, y), ImVec2(x + bw, i / 2 ? p1.y - 3 : y + bh), false, sc[i].hint))
      remapInstrument(a, sc[i].scope, gRemapOld, a.selChannel);
  }
  // Masks.
  trk::Masks& m = masks();
  const bool on = masking();
  if (checkRow(a, "##mask", "Masking", ImVec2(r0.x + 6, rowY(0)), ImVec2(r0.x + (r1.x - r0.x) * 0.55f, rowY(0) + rh), on,
               "Turns the clipboard masks on: copies, cuts and pastes take and write only the ticked parts"))
    setMasking(!on);
  const float cw = std::round(std::max(fs * 1.6f, rh));
  const float cx[3] = {r1.x - 6 - cw * 3, r1.x - 6 - cw * 2, r1.x - 6 - cw};
  const char* heads[3] = {i18n::N_("C"), i18n::N_("P"), i18n::N_("T")};
  const char* headHints[3] = {i18n::N_("Copy: the parts a copy or cut takes"), i18n::N_("Paste: the parts a paste writes"),
                              i18n::N_("Transparent: an empty part pasted leaves what is there")};
  for (int c = 0; c < 3; ++c) {
    const char* t = i18n::tr(heads[c]);
    const float tw = bold->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    dl->AddText(bold, fs, ImVec2(std::round(cx[c] + (cw - tw) * 0.5f), std::round(rowY(0) + (rh - fs) * 0.5f)), on ? P.text : P.textFaint, t);
    ImGui::SetCursorScreenPos(ImVec2(cx[c], rowY(0)));
    ImGui::PushID(c);
    ImGui::InvisibleButton("##mhead", ImVec2(cw, rh));
    ImGui::PopID();
    ui::Hint(headHints[c]);
  }
  const char* parts[trk::kParts] = {i18n::N_("Note"), i18n::N_("Volume"), i18n::N_("Pan"), i18n::N_("Effect")};
  for (int k = 0; k < trk::kParts; ++k) {
    const float y = rowY(k + 1);
    dl->AddText(a.fontRegular, fs, ImVec2(r0.x + 10, std::round(y + (rh - fs) * 0.5f)), on ? P.textDim : P.textFaint, i18n::tr(parts[k]));
    bool* flags[3] = {&m.copy[k], &m.paste[k], &m.transparent[k]};
    // R7-12: with Masking off the ticks are dimmed with their labels (they do nothing then).
    if (!on) ImGui::BeginDisabled();
    for (int c = 0; c < 3; ++c) {
      ImGui::PushID(k * 4 + c);
      if (checkRow(a, "##mk", "", ImVec2(cx[c], y), ImVec2(cx[c] + cw, y + rh), *flags[c], headHints[c])) *flags[c] = !*flags[c];
      ImGui::PopID();
    }
    if (!on) ImGui::EndDisabled();
  }
}

// ---- K-106: the classic editors' extensions, in the scope box as the other tools ----
// The rows of a tool box: a header row of raised boxes, then `rows - 1` rows under it.
struct ToolRows {
  float y0 = 0, rh = 0, gap = 2;
  float y(int r) const { return std::round(y0 + static_cast<float>(r) * (rh + gap)); }
};
ToolRows toolRows(ImVec2 p0, ImVec2 p1, int rows) {
  ToolRows t;
  t.y0 = p0.y + 2;
  t.rh = std::floor((p1.y - p0.y - 4 - t.gap * static_cast<float>(rows - 1)) / static_cast<float>(rows));
  return t;
}
// A raised header box with its text (bold for the tool's title, dim for a part's name).
void headBox(App& a, ImVec2 q0, ImVec2 q1, const char* text, bool title) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  bevelBox(dl, q0, q1);
  ImFont* f = title && a.fontBold ? a.fontBold : a.fontRegular;
  const char* t = i18n::tr(text);
  const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
  const float tx = title ? q0.x + 4 : std::round(q0.x + std::max(4.0f, (q1.x - q0.x - tw) * 0.5f));
  dl->PushClipRect(q0, q1, true);
  dl->AddText(f, fs, ImVec2(tx, std::round(q0.y + (q1.y - q0.y - fs) * 0.5f)), title ? pal().text : pal().textDim, t);
  dl->PopClipRect();
}
// A note in a tool box where its controls would be (dim, wrapped to the width, from the top).
void toolNote(App& a, ImVec2 q0, ImVec2 q1, const std::string& text) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  dl->PushClipRect(q0, q1, true);
  dl->AddText(a.fontRegular, fs, ImVec2(q0.x + 4, q0.y + 2), pal().textDim, text.c_str(), nullptr, q1.x - q0.x - 8);
  dl->PopClipRect();
}
float labelWidth(App& a, std::initializer_list<const char*> labels, float pad) {
  float w = 0;
  for (const char* l : labels) w = std::max(w, a.fontRegular->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, i18n::tr(l)).x);
  return std::round(w + pad);
}

// A small arrow button pointing left or right (repeats while held).
bool sideArrow(App& a, const char* id, ImVec2 p0, ImVec2 p1, bool left, const char* hint) {
  ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
  const bool click = bevelButton(a, id, "", p0, p1, false, hint);
  ImGui::PopItemFlag();
  const bool held = ImGui::IsItemActive();
  const float cx = std::round((p0.x + p1.x) * 0.5f) + (held ? 1 : 0), cy = std::round((p0.y + p1.y) * 0.5f) + (held ? 1 : 0);
  const float r = std::max(2.0f, std::floor(std::min(p1.x - p0.x, p1.y - p0.y) * 0.22f));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (left) dl->AddTriangleFilled(ImVec2(cx + r * 0.6f, cy - r - 1), ImVec2(cx + r * 0.6f, cy + r + 1), ImVec2(cx - r * 0.8f, cy), pal().text);
  else dl->AddTriangleFilled(ImVec2(cx - r * 0.6f, cy - r - 1), ImVec2(cx - r * 0.6f, cy + r + 1), ImVec2(cx + r * 0.8f, cy), pal().text);
  return click;
}
int gMidiFirst = 0;  // the first MIDI input device in view in the MIDI box

// R11-10: a device name that does not fit keeps its start and its end ("Launchkey...MIDI 2"):
// devices of one make differ at the end of their names (the full name is in the hint).
std::string midEllipsis(ImFont* f, float fs, const std::string& s, float maxW) {
  if (f->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str()).x <= maxW) return s;
  const size_t n = s.size();
  for (size_t keep = n; keep > 2; --keep) {
    const size_t tail = keep / 2, head = keep - tail;
    // Never split a UTF-8 sequence: move the cuts off continuation bytes.
    size_t h = head, t = n - tail;
    while (h > 0 && (static_cast<unsigned char>(s[h]) & 0xC0) == 0x80) --h;
    while (t < n && (static_cast<unsigned char>(s[t]) & 0xC0) == 0x80) ++t;
    const std::string out = s.substr(0, h) + "..." + s.substr(t);
    if (f->CalcTextSizeA(fs, FLT_MAX, 0, out.c_str()).x <= maxW) return out;
  }
  return "...";
}

// The MIDI box (the classic instrument editor's extension): the current instrument's MIDI out
// (send its notes, channel, program, bend range, its own sound muted) at the left, the MIDI input
// (devices, velocity, what the notes play) at the right.
void midiTool(App& a, ImVec2 p0, ImVec2 p1) {
  const ToolRows r = toolRows(p0, p1, 4);
  const float gap = r.gap, x0 = p0.x + 2, xE = p1.x - 2;
  const float titleW = std::round(std::max(labelWidth(a, {i18n::N_("MIDI")}, 14), (xE - x0) * 0.10f));
  const float xs = x0 + titleW + gap, halfW = std::floor((xE - xs - gap) / 2), xm = xs + halfW + gap;
  headBox(a, ImVec2(x0, r.y(0)), ImVec2(xs - gap, r.y(0) + r.rh), i18n::N_("MIDI"), true);
  headBox(a, ImVec2(xs, r.y(0)), ImVec2(xs + halfW, r.y(0) + r.rh), i18n::N_("Current instrument"), false);
  headBox(a, ImVec2(xm, r.y(0)), ImVec2(xE, r.y(0) + r.rh), i18n::N_("MIDI input"), false);
  // The rows under the title box: what the halves act on.
  {
    const Channel* ch = a.doc.song().channel(a.selChannel);
    const int idx = ch ? a.doc.song().channelIndex(ch->id) : -1;
    char num[16];
    std::snprintf(num, sizeof num, "%02X", idx + trackBase(a));
    const ImVec2 q0(x0, r.y(1)), q1(xs - gap, p1.y - 2);
    bevelBox(ImGui::GetWindowDrawList(), q0, q1, true);
    if (ch) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float fs = ImGui::GetFontSize();
      dl->AddText(monoFont(a), fs, ImVec2(q0.x + 4, q0.y + 3), valueInk(), num);
      dl->PushClipRect(q0, q1, true);
      dl->AddText(a.fontRegular, fs, ImVec2(q0.x + 4, q0.y + 3 + fs + 2), pal().text, ch->name.c_str(), nullptr, q1.x - q0.x - 8);
      dl->PopClipRect();
    }
  }
  // ---- the current instrument ----
  namespace tp = packs::trackerpack;
  const tp::HeaderView v = a.selChannel != kNoId ? tp::headerView(a, a.selChannel) : tp::HeaderView{};
  const float lw = labelWidth(a, {i18n::N_("Channel"), i18n::N_("Program"), i18n::N_("Bend")}, 10);
  if (!v.tracker) {
    toolNote(a, ImVec2(xs, r.y(1)), ImVec2(xs + halfW, p1.y - 2),
             a.selChannel == kNoId ? std::string(i18n::tr("No instrument is selected"))
                                   : std::string(i18n::tr("MIDI out belongs to Tracker Sampler instruments. A plugin's own MIDI ports are in its Channel settings.")));
  } else {
    tp::MidiSettings m = v.midi;
    const char* out = a.midi && !a.midi->output.empty() ? a.midi->output.c_str() : nullptr;
    std::string hint = std::string(i18n::tr("The notes this instrument plays also go to the MIDI output, on its channel and program")) + " (" +
                       (out ? out : i18n::tr("no output is chosen: Settings, MIDI")) + ")";
    if (checkRow(a, "##mon", i18n::N_("Send notes to the MIDI output"), ImVec2(xs, r.y(1)), ImVec2(xs + halfW, r.y(1) + r.rh), m.on, hint.c_str())) {
      m.on = !m.on;
      tp::headerSetMidi(a, a.selChannel, m, "MIDI output", 0);
    }
    const float cw = std::floor((halfW - gap) / 2);
    char b[16];
    if (!m.on) ImGui::BeginDisabled();
    std::snprintf(b, sizeof b, "%d", m.channel + 1);
    if (const int d = stepper(a, "##mch", i18n::N_("Channel"), b, ImVec2(xs, r.y(2)), cw, r.rh, i18n::tr("The MIDI channel its notes go out on (1 to 16)"), lw, true)) {
      m.channel = std::clamp(m.channel + d, 0, 15);
      tp::headerSetMidi(a, a.selChannel, m, "MIDI channel", 1);
    }
    std::snprintf(b, sizeof b, "%d", m.program + 1);
    if (const int d = stepper(a, "##mprg", i18n::N_("Program"), b, ImVec2(xs + cw + gap, r.y(2)), halfW - cw - gap, r.rh,
                              i18n::tr("The program change sent before its notes (1 to 128)"), lw, true)) {
      m.program = std::clamp(m.program + d, 0, 127);
      tp::headerSetMidi(a, a.selChannel, m, "MIDI program", 2);
    }
    if (!m.on) ImGui::EndDisabled();
    std::snprintf(b, sizeof b, "%d", m.bend);
    if (const int d = stepper(a, "##mbend", i18n::N_("Bend"), b, ImVec2(xs, r.y(3)), cw, r.rh, i18n::tr("Semitones the pitch wheel bends its notes (0 to 36)"), lw, true)) {
      m.bend = std::clamp(m.bend + d, 0, 36);
      tp::headerSetMidi(a, a.selChannel, m, "pitch wheel range", 3);
    }
    // R11-07: as the classic "mute computer", a MIDI option: it acts (and can be changed) only
    // while the notes go to MIDI; the instrument list marks an instrument it silences.
    if (!m.on) ImGui::BeginDisabled();
    if (checkRow(a, "##mmute", i18n::N_("Mute its own sound"), ImVec2(xs + cw + gap, r.y(3)), ImVec2(xs + halfW, r.y(3) + r.rh), m.mute,
                 i18n::tr("While its notes go to MIDI, the instrument's own sound is silent (the external synth plays alone)"))) {
      m.mute = !m.mute;
      tp::headerSetMidi(a, a.selChannel, m, "mute instrument", 0);
    }
    if (!m.on) ImGui::EndDisabled();
  }
  // ---- the MIDI input (Settings, MIDI keeps the same values) ----
  if (!a.midi) {
    toolNote(a, ImVec2(xm, r.y(1)), ImVec2(xE, p1.y - 2), i18n::tr("MIDI is not available in this run"));
    return;
  }
  MidiManager& mm = *a.midi;
  bool changed = false, reopen = false;
  {
    const float y = r.y(1);
    if (mm.inputs.empty()) {
      const char* t = mm.serviceAvailable() ? i18n::tr("No MIDI input device is connected") : i18n::tr("The MIDI service has not answered yet");
      ImGui::GetWindowDrawList()->AddText(a.fontRegular, ImGui::GetFontSize(), ImVec2(xm + 4, std::round(y + (r.rh - ImGui::GetFontSize()) * 0.5f)), pal().textDim, t);
    } else {
      // cp14: every device, side by side; when they do not all fit, the row scrolls (the arrows at
      // its right end, or the wheel over it) and a count says which are in view.
      const int total = static_cast<int>(mm.inputs.size());
      const float fsz = ImGui::GetFontSize();
      const float minW = std::round(fsz * 6.0f), aw = std::round(r.rh * 1.1f);
      int fit = std::max(1, static_cast<int>(std::floor((xE - xm + gap) / (minW + gap))));
      const bool scroll = total > fit;
      float areaE = xE;
      if (scroll) {
        const std::string t = std::to_string(total), widest = t + "-" + t + "/" + t;
        const float cw = std::ceil(monoFont(a)->CalcTextSizeA(fsz, FLT_MAX, 0, widest.c_str()).x) + 8;
        areaE = xE - 2 * aw - cw - gap * 2;
        fit = std::max(1, static_cast<int>(std::floor((areaE - xm + gap) / (minW + gap))));
      }
      if (ImGui::IsMouseHoveringRect(ImVec2(xm, y), ImVec2(xE, y + r.rh)) && ImGui::GetIO().MouseWheel != 0)
        gMidiFirst += ImGui::GetIO().MouseWheel > 0 ? -1 : 1;
      gMidiFirst = std::clamp(gMidiFirst, 0, std::max(0, total - fit));
      const int n = std::min(fit, total - gMidiFirst);
      const float dw = std::floor((areaE - xm - gap * static_cast<float>(n - 1)) / static_cast<float>(n));
      for (int k = 0; k < n; ++k) {
        const int i = gMidiFirst + k;
        MidiInputConfig* c = mm.inputConfig(mm.inputs[static_cast<size_t>(i)]);
        if (!c) continue;
        const float dx = xm + static_cast<float>(k) * (dw + gap);
        ImGui::PushID(i);
        const std::string hint = std::string(i18n::tr("Notes and controls from this device")) + ": " + c->name;
        const float boxW = std::max(8.0f, std::round(std::min(fsz * 0.8f, r.rh - 2)));
        const std::string shown = midEllipsis(a.fontRegular, fsz, c->name, dw - boxW - 14);
        if (checkRow(a, "##mdev", shown.c_str(), ImVec2(dx, y), ImVec2(dx + dw, y + r.rh), c->enabled, hint.c_str())) c->enabled = !c->enabled, changed = reopen = true;
        ImGui::PopID();
      }
      if (scroll) {
        const std::string count = std::to_string(gMidiFirst + 1) + "-" + std::to_string(gMidiFirst + n) + "/" + std::to_string(total);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ax = xE - 2 * aw - gap;
        const float tw = monoFont(a)->CalcTextSizeA(fsz, FLT_MAX, 0, count.c_str()).x;
        dl->AddText(monoFont(a), fsz, ImVec2(std::round(ax - gap - 4 - tw), std::round(y + (r.rh - fsz) * 0.5f)), pal().textDim, count.c_str());
        if (sideArrow(a, "##mdevl", ImVec2(ax, y), ImVec2(ax + aw, y + r.rh), true, i18n::tr("The devices before these (the wheel scrolls too)")))
          gMidiFirst = std::max(0, gMidiFirst - 1);
        if (sideArrow(a, "##mdevr", ImVec2(ax + aw + gap, y), ImVec2(xE, y + r.rh), false, i18n::tr("The devices after these (the wheel scrolls too)")))
          gMidiFirst = std::min(total - fit, gMidiFirst + 1);
      }
    }
  }
  MidiRouting& rt = mm.routing;
  const float lw2 = labelWidth(a, {i18n::N_("Velocity"), i18n::N_("Fixed")}, 10);
  const bool fixed = rt.velocity == VelocityCurve::Fixed;
  const float vw = fixed ? std::floor((xE - xm - gap) * 0.6f) : xE - xm;
  if (const int d = stepper(a, "##mvel", i18n::N_("Velocity"), velocityCurveName(rt.velocity), ImVec2(xm, r.y(2)), vw, r.rh,
                            i18n::tr("How hard the keys play: the curve the played velocities go through"), lw2, true)) {
    const int nC = static_cast<int>(VelocityCurve::Count);
    rt.velocity = static_cast<VelocityCurve>(((static_cast<int>(rt.velocity) + d) % nC + nC) % nC);
    changed = true;
  }
  if (fixed) {
    char b[16];
    std::snprintf(b, sizeof b, "%d", rt.fixedVelocity);
    if (const int d = stepper(a, "##mfix", i18n::N_("Fixed"), b, ImVec2(xm + vw + gap, r.y(2)), xE - xm - vw - gap, r.rh, i18n::tr("The velocity every note plays at"), -1, true)) {
      rt.fixedVelocity = std::clamp(rt.fixedVelocity + d, 1, 127);
      changed = true;
    }
  }
  const float rw = std::floor((xE - xm - gap) / 2);
  if (radioRow(a, "##mcur", i18n::N_("Current instrument"), ImVec2(xm, r.y(3)), ImVec2(xm + rw, r.y(3) + r.rh), !rt.byChannel,
               i18n::tr("Every MIDI channel plays the instrument selected here")) && rt.byChannel)
    rt.byChannel = false, changed = true;
  if (radioRow(a, "##mbych", i18n::N_("By MIDI channel"), ImVec2(xm + rw + gap, r.y(3)), ImVec2(xE, r.y(3) + r.rh), rt.byChannel,
               i18n::tr("Each MIDI channel plays the instrument Settings, MIDI routes it to")) && !rt.byChannel)
    rt.byChannel = true, changed = true;
  if (reopen) mm.apply();
  if (changed) {
    a.publishMidi();
    mm.save((std::filesystem::path(a.configDir()) / "midi.json").string());
  }
}

// The Sample tools box (the classic sample editor's extension): the current instrument's
// selected sample and the range dragged out on it in its editor (none: the whole sample), the
// clipboard, and the range edits in three rows of buttons.
void sampleTool(App& a, ImVec2 p0, ImVec2 p1) {
  namespace tp = packs::trackerpack;
  const ToolRows r = toolRows(p0, p1, 4);
  const float gap = r.gap, x0 = p0.x + 2, xE = p1.x - 2;
  // R11-08: wide enough for "Clipboard" and a seven-digit length with a gap, and for the two clear buttons.
  const float numW = monoFont(a)->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, "0000000").x;
  const float titleW = std::round(std::max({labelWidth(a, {i18n::N_("Sample tools")}, 14), labelWidth(a, {i18n::N_("Clipboard"), i18n::N_("Range")}, 18) + numW + 12,
                                            labelWidth(a, {i18n::N_("No range"), i18n::N_("Empty clip")}, 14) * 2, (xE - x0) * 0.16f}));
  const float xs = x0 + titleW + gap;
  headBox(a, ImVec2(x0, r.y(0)), ImVec2(xs - gap, r.y(0) + r.rh), i18n::N_("Sample tools"), true);
  const ObjectId ch = a.selChannel;
  const tp::HeaderView v = ch != kNoId ? tp::headerView(a, ch) : tp::HeaderView{};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  // The title box's column: the range and the clipboard, read only.
  auto info = [&](int row, const char* label, const std::string& value, const char* hint) {
    readout(a, label, value, ImVec2(x0 + 4, r.y(row)), titleW - 8, r.rh, hint);
  };
  if (!v.tracker) {
    const Song& s = a.doc.song();
    const Channel* c = s.channel(ch);
    std::string why = ch == kNoId ? std::string(i18n::tr("No instrument is selected"))
                                  : std::string(i18n::tr("Sample tools edit a Tracker Sampler's samples (module instruments, and tracks added with Add)."));
    const bool sampler = c && c->plugin.type == "gl.sampler" && !c->plugin.data.empty();
    if (sampler) why += std::string(" ") + i18n::tr("This track's sample opens on the Audio page.");
    headBox(a, ImVec2(xs, r.y(0)), ImVec2(xE, r.y(0) + r.rh), c ? c->name.c_str() : "", false);
    const float bw = sampler ? std::round(labelWidth(a, {i18n::N_("Audio page")}, 24)) : 0;
    toolNote(a, ImVec2(xs, r.y(1)), ImVec2(xE - bw - gap, p1.y - 2), why);
    if (sampler && bevelButton(a, "##smpaudio", i18n::N_("Audio page"), ImVec2(xE - bw, r.y(1)), ImVec2(xE, r.y(1) + r.rh), false, i18n::tr("The audio editor on this track's sample")))
      showPage(a, "Loupe");
    return;
  }
  // SX-10..SX-15: the source block at the right (two columns of the eight): the source readout,
  // Copy and Swap of the sound and of the sample, the two marks.
  const float bw = std::floor((xE - xs - gap * 8) / 8);
  const float xb = xE - 2 * bw - gap;           // the block's left edge
  const float xm = xb - 2 * gap;                // the main grid's right edge (a wider gap between them)
  const float bwm = std::floor((xm - xs - gap * 5) / 6);
  {
    const Song& s = a.doc.song();
    const int si = s.channelIndex(gSourceChannel);
    const ObjectId srcCh = si >= 0 ? gSourceChannel : ch;
    std::string val = si >= 0 ? trk::hex2(si + 1) : std::string("--");
    val += ".";
    char sn[12];
    std::snprintf(sn, sizeof sn, "%02d", gSourceSlot + 1);
    val += gSourceSlot >= 0 ? std::string(sn) : std::string("--");
    const std::string smpName = gSourceSlot >= 0 ? tp::headerSampleName(a, srcCh, gSourceSlot) : std::string();
    std::string hint = i18n::tr("The source: an instrument (click its number in the list) and a sample (click its number in the sample list)");
    if (!smpName.empty()) hint += std::string(" \xC2\xB7 ") + i18n::tr("source sample:") + " " + smpName;
    bevelBox(dl, ImVec2(xb, r.y(0)), ImVec2(xE, r.y(0) + r.rh), true);
    readout(a, i18n::N_("Source"), val, ImVec2(xb + 4, r.y(0)), xE - xb - 8, r.rh, hint.c_str());
    const int ci = s.channelIndex(ch);
    const bool insOk = si >= 0 && ci >= 0 && si != ci;
    const std::string srcSmp = gSourceSlot >= 0 ? tp::headerSampleName(a, srcCh, gSourceSlot) : std::string();
    const bool smpCopyOk = !srcSmp.empty() && !(srcCh == ch && gSourceSlot == v.slot);
    const bool smpSwapOk = gSourceSlot >= 0 && gSourceSlot != v.slot && v.slot >= 0;
    struct SB {
      const char* label;
      const char* brief;
      const char* hint;
      bool on, lit;
    };
    const SB blk[3][2] = {
        {{i18n::N_("Copy sound"), i18n::N_("Copy ins"), i18n::N_("Copy sound: the source instrument's sound (its samples, envelopes and settings) over this one; the name and the notes stay"), insOk, false},
         {i18n::N_("Copy sample"), i18n::N_("Copy smp"), i18n::N_("Copy sample: the source sample over the selected sample (an instrument with no sample takes it as its first)"), smpCopyOk, false}},
        {{i18n::N_("Swap sound"), i18n::N_("Swap ins"), i18n::N_("Swap sound: this instrument and the source trade sounds; names and notes stay, so the patterns play the other sound"), insOk, false},
         {i18n::N_("Swap sample"), i18n::N_("Swap smp"), i18n::N_("Swap sample: the selected sample and this instrument's source-numbered sample trade places (the key map stays)"), smpSwapOk, false}},
        {{i18n::N_("Mark instr."), i18n::N_("Mark ins"), i18n::N_("Mark: this instrument becomes the source (again: no source)"), ci >= 0, ch == gSourceChannel},
         {i18n::N_("Mark sample"), i18n::N_("Mark smp"), i18n::N_("Mark: the selected sample's number becomes the source sample (again: none)"), v.slot >= 0, v.slot >= 0 && gSourceSlot == v.slot}}};
    for (int row = 0; row < 3; ++row) {
      const float y = r.y(row + 1), yb = row == 2 ? p1.y - 2 : y + r.rh;
      for (int k = 0; k < 2; ++k) {
        const SB& bt = blk[row][k];
        const float bx = k == 0 ? xb : xb + bw + gap, bx1 = k == 0 ? xb + bw : xE;
        ImGui::PushID(900 + row * 2 + k);
        if (!bt.on) ImGui::BeginDisabled();
        if (bevelButton(a, "##smpsrc", fitLabel(a, bt.label, bt.brief, bx1 - bx), ImVec2(bx, y), ImVec2(bx1, yb), bt.lit, i18n::tr(bt.hint))) {
          const int id = row * 2 + k;
          if (id == 0) copyInstrumentSound(a, ch, gSourceChannel, false);
          else if (id == 1) tp::headerCopySample(a, ch, srcCh, gSourceSlot);
          else if (id == 2) copyInstrumentSound(a, ch, gSourceChannel, true);
          else if (id == 3) tp::headerExchangeSamples(a, ch, gSourceSlot);
          else if (id == 4) gSourceChannel = gSourceChannel == ch ? kNoId : ch;
          else gSourceSlot = gSourceSlot == v.slot ? -1 : v.slot;
        }
        if (!bt.on) ImGui::EndDisabled();
        ImGui::PopID();
      }
    }
  }
  // The header row: the sample (arrows step through the instrument's samples) and its length.
  {
    const float aw = std::round(r.rh * 1.2f);
    const float y = r.y(0);
    if (arrowButton(a, "##smpprev", ImVec2(xs, y), ImVec2(xs + aw, y + r.rh), true, i18n::tr("The instrument's previous sample"))) tp::headerStepSample(a, ch, -1);
    if (arrowButton(a, "##smpnext", ImVec2(xs + aw + gap, y), ImVec2(xs + 2 * aw + gap, y + r.rh), false, i18n::tr("The instrument's next sample"))) tp::headerStepSample(a, ch, 1);
    const ImVec2 q0(xs + 2 * aw + 2 * gap, y), q1(xm, y + r.rh);
    bevelBox(dl, q0, q1, true);
    char num[12];
    std::snprintf(num, sizeof num, "%02d", v.slot + 1);
    const float ty = std::round(y + (r.rh - fs) * 0.5f);
    dl->PushClipRect(q0, q1, true);
    if (v.slot >= 0) {
      dl->AddText(mono, fs, ImVec2(q0.x + 4, ty), valueInk(), num);
      const float nx = q0.x + 8 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00").x;
      char len[48];
      std::snprintf(len, sizeof len, "%d frames  %.0f Hz", static_cast<int>(v.frames), v.rate);
      const float lw = mono->CalcTextSizeA(fs, FLT_MAX, 0, len).x;
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), q1.x - nx - lw - 12, pal().text, v.name.c_str());
      dl->AddText(mono, fs, ImVec2(q1.x - 4 - lw, ty), pal().textDim, len);
    } else {
      dl->AddText(a.fontRegular, fs, ImVec2(q0.x + 4, ty), pal().textDim, i18n::tr("The instrument has no sample yet: drop audio on its editor"));
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton("##smpname", ImVec2(q1.x - q0.x, q1.y - q0.y));
    ui::Hint("The current instrument's selected sample (its editor selects another; the arrows step through them)");
  }
  char rng[48], clip[24];
  if (v.range.empty()) std::snprintf(rng, sizeof rng, "%s", i18n::tr("all"));
  else std::snprintf(rng, sizeof rng, "%d", static_cast<int>(v.range.b - v.range.a));
  std::snprintf(clip, sizeof clip, "%d", static_cast<int>(v.clipFrames));
  info(1, i18n::N_("Range"), rng, "Frames in the range dragged out on the sample in its editor (all: no range, the tools take the whole sample)");
  info(2, i18n::N_("Clipboard"), clip, "Frames on the sample clipboard (shared by every Tracker Sampler)");
  // R11-09: No range and Empty clipboard side by side (the classic extension's clear buttons).
  const float cbw = std::floor((xs - gap - x0 - gap) / 2);
  if (v.range.empty()) ImGui::BeginDisabled();
  if (bevelButton(a, "##smpclr", i18n::N_("No range"), ImVec2(x0, r.y(3)), ImVec2(x0 + cbw, r.y(3) + r.rh), false, i18n::tr("Clears the range: the tools take the whole sample")))
    tp::headerEdit(a, ch, tp::HeaderEdit::ClearRange);
  if (v.range.empty()) ImGui::EndDisabled();
  if (v.clipFrames <= 0) ImGui::BeginDisabled();
  if (bevelButton(a, "##smpclip", i18n::N_("Empty clip"), ImVec2(x0 + cbw + gap, r.y(3)), ImVec2(xs - gap, r.y(3) + r.rh), false,
                  i18n::tr("Empties the sample clipboard (frees its memory)")))
    tp::headerEdit(a, ch, tp::HeaderEdit::EmptyClipboard);
  if (v.clipFrames <= 0) ImGui::EndDisabled();
  // Three rows of six: the range edits, the clipboard, the edits that ask for values first.
  const bool have = v.slot >= 0, part = !v.range.empty() && v.range.b - v.range.a < v.frames;
  struct Btn {
    const char* label;
    const char* hint;
    int edit;            // tp::HeaderEdit, or -1
    const char* popup;   // or a popup
    bool on;
    const char* brief = nullptr;  // cp15: the short label when the button is narrow
  };
  const Btn rows[3][6] = {
      {{i18n::N_("Reverse"), i18n::N_("Plays the range (or the sample) backwards"), static_cast<int>(tp::HeaderEdit::Reverse), nullptr, have, nullptr},
       {i18n::N_("Normalise"), i18n::N_("The loudest point to full scale"), static_cast<int>(tp::HeaderEdit::Normalise), nullptr, have, i18n::N_("Normal.")},
       {i18n::N_("Fade in"), i18n::N_("From silence to full volume"), static_cast<int>(tp::HeaderEdit::FadeIn), nullptr, have, nullptr},
       {i18n::N_("Fade out"), i18n::N_("From full volume to silence"), static_cast<int>(tp::HeaderEdit::FadeOut), nullptr, have, nullptr},
       {i18n::N_("Silence"), i18n::N_("The range becomes silence"), static_cast<int>(tp::HeaderEdit::Silence), nullptr, have && !v.range.empty(), nullptr},
       {i18n::N_("Fix DC"), i18n::N_("Removes the DC offset (centres the wave on zero)"), static_cast<int>(tp::HeaderEdit::FixDc), nullptr, have, nullptr}},
      {{i18n::N_("Crop"), i18n::N_("Only the range stays"), static_cast<int>(tp::HeaderEdit::Crop), nullptr, part, nullptr},
       {i18n::N_("Delete"), i18n::N_("The range goes"), static_cast<int>(tp::HeaderEdit::Delete), nullptr, part, nullptr},
       {i18n::N_("Cut"), i18n::N_("The range to the clipboard, then out of the sample"), static_cast<int>(tp::HeaderEdit::Cut), nullptr, part, nullptr},
       {i18n::N_("Copy"), i18n::N_("The range (or the sample) to the clipboard"), static_cast<int>(tp::HeaderEdit::Copy), nullptr, have, nullptr},
       {i18n::N_("Paste"), i18n::N_("The clipboard over the range, or in at the range's start"), static_cast<int>(tp::HeaderEdit::Paste), nullptr, have && v.clipFrames > 0, nullptr},
       {i18n::N_("Range all"), i18n::N_("The range takes the whole sample"), static_cast<int>(tp::HeaderEdit::SelectAll), nullptr, have, i18n::N_("All")}},
      {{i18n::N_("Volume..."), i18n::N_("A volume ramp from the start to the end"), -1, "volume", have, i18n::N_("Vol...")},
       {i18n::N_("Resample..."), i18n::N_("More or fewer frames for the same pitch"), -1, "resample", have, i18n::N_("Resmp...")},
       {i18n::N_("Echo..."), i18n::N_("Repeats, each quieter than the last"), -1, "echo", have, nullptr},
       {i18n::N_("Mix..."), i18n::N_("Another sample of the instrument mixed in"), -1, "mix", have && v.samples > 1, nullptr},
       {i18n::N_("X-fade loop..."), i18n::N_("Smooths the loop's seam"), -1, "xfade", have && v.looped, i18n::N_("X-fade...")},
       {i18n::N_("Trim loop"), i18n::N_("Drops what follows the loop"), static_cast<int>(tp::HeaderEdit::TrimLoop), nullptr, have && v.looped, i18n::N_("Trim lp")}}};
  const char* openPopup = nullptr;
  for (int row = 0; row < 3; ++row) {
    const float y = r.y(row + 1), yb = row == 2 ? p1.y - 2 : y + r.rh;
    for (int k = 0; k < 6; ++k) {
      const Btn& bt = rows[row][k];
      const float bx = xs + static_cast<float>(k) * (bwm + gap), bx1 = k == 5 ? xm : bx + bwm;
      ImGui::PushID(row * 8 + k);
      if (!bt.on) ImGui::BeginDisabled();
      if (bevelButton(a, "##smpt", fitLabel(a, bt.label, bt.brief, bx1 - bx), ImVec2(bx, y), ImVec2(bx1, yb), false, i18n::tr(bt.hint))) {
        if (bt.popup) openPopup = bt.popup;
        else tp::headerEdit(a, ch, static_cast<tp::HeaderEdit>(bt.edit));
      }
      if (!bt.on) ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  // R11-04: the value popups open in the same ID scope headerPopups draws them in (opened inside
  // the button's PushID, their IDs never matched), and the keys stay in the band (the button's
  // click handed them to the pattern, whose focus closed the popup again).
  if (openPopup) {
    tp::headerOpenPopup(openPopup);
    keepKeysHere();
  }
  tp::headerPopups(a, ch);
}

// The instrument editor: the Device chain in the lower band, showing the channel's instrument (the
// Tracker hosts Channel settings there). Shown while the chain is open or a channel's settings are asked for.
bool instrumentShown(App& a) {
  if (trackereditors::pluginWindowOpen(a)) return false;  // cp18: the channel settings float as the plugin window
  const bool* f = layoutui::windowOpenFlag(a, "Device chain");
  return a.settingsChannel != kNoId || (f && *f);
}
// The height the instrument editor's band needs to show the editor whole, from its last frame: the
// chain's own rows above its body plus the body's content (0 while it has not been drawn). The
// body's content does not depend on the band's height, so sizing the band by it is stable.
float instrumentEditorNeed() {
  const ImGuiWindow* w = ImGui::FindWindowByName("###Device chain");
  if (!w || !w->WasActive) return 0;
  for (const ImGuiWindow* c : w->DC.ChildWindows) {
    if (!c->WasActive || !std::strstr(c->Name, "##devbody")) continue;
    return (c->Pos.y - w->Pos.y) + c->ContentSize.y + c->WindowPadding.y * 2 + w->WindowPadding.y + 2;
  }
  return 0;
}

// Closed: the settings asked for and the chain itself (R5-03: back to the main screen closes it too).
void closeInstrument(App& a) {
  if (trackereditors::pluginWindowOpen(a)) {  // the plugin window floats on its own (its title bar closes it)
    if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) *f = false;
    return;
  }
  trackereditors::close(a);  // cp18: the band's screen (a preview, held notes and its keys end with it)
}

// What the whole header shows: the boxes, or a screen swapped in (Help).
enum class Swap { None, Help, Disk, Config, About };
Swap gSwap = Swap::None;
bool gFolded = false;  // cp17: the header folded to its strip while an editor is open in a small window
bool gHeaderTyping = false;  // a field of the band is being typed in (the keys stay there)
// R4-11: the instrument being renamed in its list (its menu's Rename), and the name typed so far.
ObjectId gRenaming = kNoId;
char gRenameBuf[128] = {};
int gHelpSubject = 0;
// R12-06: a config list longer than its rows: the "first-last/total" count with up and down arrows
// before it (the wheel scrolls too), its right end at `right`, its top at `top`. Returns the step to
// the first row in view (the caller clamps).
int listArrows(App& a, const char* id, const std::string& count, float right, float top, int step) {
  const float fs = ImGui::GetFontSize(), h = std::round(fs + 2), aw = std::round(h * 1.2f);
  ImFont* mono = monoFont(a);
  const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, count.c_str()).x;
  const float ax = std::round(right - 2 * aw - 2);
  ImGui::GetWindowDrawList()->AddText(mono, fs, ImVec2(std::round(ax - 6 - tw), top), pal().textDim, count.c_str());
  int d = 0;
  ImGui::PushID(id);
  if (arrowButton(a, "##up", ImVec2(ax, top - 1), ImVec2(ax + aw, top - 1 + h), true, "The rows before these (the wheel scrolls too)")) d = -step;
  if (arrowButton(a, "##dn", ImVec2(ax + aw + 2, top - 1), ImVec2(ax + 2 * aw + 2, top - 1 + h), false, "The rows after these (the wheel scrolls too)")) d = step;
  ImGui::PopID();
  return d;
}

int gConfigPage = 0;  // the settings screen's page: 0 layout, 1 miscellaneous, 2 audio, 3 MIDI input (shown in the classic order: audio first)
int gCfgDevFirst = 0, gCfgMidiFirst = 0;  // the first output device and MIDI input in view on those pages

struct HelpRow {
  const char* keys;
  const char* what;
};
// The pattern editor's keys in the Tracker key set (Gridline's own wording).
const HelpRow kPatternKeys[] = {
    {"Z S X D C ... /", i18n::N_("Notes in the octave shown (the row above: the black keys)")},
    {"Q 2 W 3 E ... P", i18n::N_("Notes an octave up")},
    {"F1 ... F8", i18n::N_("The octave")},
    {"Caps Lock, < >", i18n::N_("A note off (the note above ends here)")},
    {"Esc", i18n::N_("Edit mode on or off (off: the keys only play)")},
    {"0 ... 9, A ... F", i18n::N_("A digit: written, then down by the edit step (effects: after the last digit)")},
    {"`  Shift+`", i18n::N_("Edit step one more, one fewer")},
    {"Ctrl+0 ... 9", i18n::N_("Edit step")},
    {"Up Down PgUp PgDn", i18n::N_("Rows (16 at a time with the page keys)")},
    {"Home End", i18n::N_("The first, the last row")},
    {"Left Right", i18n::N_("The fields of a cell")},
    {"Tab  Shift+Tab", i18n::N_("The next, the previous track")},
    {"Alt+Q ... I, Alt+A ... K", i18n::N_("The first eight tracks, the next eight")},
    {"Ctrl+Shift+Up Down", i18n::N_("The previous, the next instrument (Shift+arrows mark a block here)")},
    {"F9 ... F12", i18n::N_("Jump rows (Shift: keep the cursor's row there, Ctrl: play the pattern from it)")},
    {"Shift or Alt+arrows", i18n::N_("Mark a block")},
    {"Alt+C", i18n::N_("Mark the track")},
    {"Ctrl+C X V, Alt+F4 F3 F5", i18n::N_("Copy, cut, paste the block (masks apply; cut, paste: edit mode)")},
    {"Shift+F3 F4 F5", i18n::N_("Cut, copy, paste the track (own clipboard; cut, paste: edit mode)")},
    {"Ctrl+F3 F4 F5", i18n::N_("Cut, copy, paste the pattern (own clipboard; cut, paste: edit mode)")},
    {"Delete", i18n::N_("Clears the field (the block when one is marked)")},
    {"Shift+Delete", i18n::N_("Clears the cell")},
    {"Ctrl+Delete  Alt+Delete", i18n::N_("Clears volume and effect, the effect")},
    {"Ctrl+keypad + -", i18n::N_("The master volume up, down")},
    {"Keypad 1 ... 8, Enter", i18n::N_("Instruments 1 to 8 of the keypad's group, the next group")},
    {"Alt+1 ... 0", i18n::N_("Writes macro 1 to 10 into the volume or effect field at the cursor")},
    {"Shift+Alt+1 ... 0", i18n::N_("Stores that field as macro 1 to 10 (volumes and effects keep a bank each)")},
    {"Insert", i18n::N_("A row in (Shift: in every track)")},
    {"Backspace", i18n::N_("The row above out (Shift: in every track)")},
    {"Ctrl+Up Down", i18n::N_("The block a semitone up, down (Gridline's own)")},
    {"Shift+F1 F2", i18n::N_("The track a semitone down, up (F7 F8 the same)")},
    {"Ctrl+F1 F2", i18n::N_("The pattern a semitone down, up")},
    {"Alt+F1 F2", i18n::N_("The block a semitone down, up")},
    {"Ctrl+Left Right", i18n::N_("The previous, the next pattern (past the last: a new one)")},
    {"Ctrl+Shift+Left Right", i18n::N_("The previous, the next song position")},
    {"Space", i18n::N_("Play, stop")},
    {"Enter, Right Ctrl", i18n::N_("Play the song from the current order entry")},
    {"Ctrl+Enter, Right Alt", i18n::N_("Play the pattern (Shift+Enter: from the cursor's row)")},
    {"Right Shift (alone)", i18n::N_("Record the pattern: the keys write on the row playing")},
};

// The help screen (ref 164): the band swaps to it, the pattern stays below. Left: a "Subjects" title,
// the subjects as a radio list, Exit at the bottom left. Right: a sunken text pane with the subject's
// heading in a large face and the text under it, a scrollbar at its right edge.
const char* const kHelpSubjects[] = {i18n::N_("Pattern keys"), i18n::N_("Effect letters"), i18n::N_("Screen keys"), i18n::N_("Using the Tracker")};

// "Using the Tracker": the screen, box by box, in Gridline's words.
struct HelpPara {
  const char* head;
  const char* text;
};
const HelpPara kUsing[] = {
    {i18n::N_("The header band"),
     i18n::N_("The band at the top holds the song: the order list at the left, the song and pattern boxes beside it, the scopes under them, "
              "two button columns, and the instrument list at the right. Its screens (help, settings, files, about) swap into the band; "
              "the pattern always stays in view below.")},
    {i18n::N_("Order list"),
     i18n::N_("Each entry is a place in the song and the pattern it plays. Click an entry to go there; the arrows step through the song; "
              "Insert and Delete add and remove entries. The song is the playlist: an entry is a pattern clip on it.")},
    {i18n::N_("Song and pattern boxes"),
     i18n::N_("Tempo, rows per beat, the edit step, the pattern shown and its length. Expand spreads the rows twice as far apart, "
              "Shrink brings them back.")},
    {i18n::N_("Scopes"),
     i18n::N_("One cell per track. A click mutes or unmutes; a right-click turns the track on or off for multichannel edit; both buttons "
              "solo it.")},
    {i18n::N_("Pattern editor"),
     i18n::N_("One track per instrument, one row per step. Esc turns edit mode on; the note keys then write notes at the cursor and move "
              "it by the edit step. Each field takes hex digits; Delete clears it.")},
    {i18n::N_("Instruments"),
     i18n::N_("The list at the right: a click picks the instrument the note keys play and write; a click on its number marks it as the "
              "source; a right-click opens its menu (rename, clone, move, copy or swap sounds with the source). Channel settings opens "
              "its editor in the centre.")},
    {i18n::N_("Source and destination"),
     i18n::N_("The current instrument and sample are the destination. The source is marked: a click on an instrument's number, and on a "
              "sample's number in the sample list. Instrument remap (Advanced edit) moves the source's notes to the current instrument; "
              "the Sample tools copy or swap sounds and samples between them, each one undo step.")},
    {i18n::N_("Recording"),
     i18n::N_("Record pattern and Record song play and write what the keys play on the row playing. Settings, Miscellaneous: Record snap "
              "moves each note to the nearest row (or every 2, 4, 8 or 16 rows), Record key releases writes the note's end, Multichannel "
              "record spreads the notes over the enabled tracks.")},
};

void helpScreen(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const float leftW = std::round(std::max(150.0f, fs * 11));
  bevelBox(dl, p0, ImVec2(p0.x + leftW, p1.y));
  const float bh = std::round(fs + 9), rr = std::round(fs + 5);
  boxTitle(a, ImVec2(p0.x + 6, p0.y + 4), "Subjects");
  const int nSubj = static_cast<int>(sizeof kHelpSubjects / sizeof kHelpSubjects[0]);
  for (int i = 0; i < nSubj; ++i) {
    const float y = std::round(p0.y + 6 + fs + static_cast<float>(i) * rr);
    ImGui::PushID(i);
    if (radioRow(a, "##subj", kHelpSubjects[i], ImVec2(p0.x + 4, y), ImVec2(p0.x + leftW - 4, y + rr), gHelpSubject == i)) gHelpSubject = i;
    ImGui::PopID();
  }
  {
    // cp15: the demo module (a song written across many tracks, as a module is); unsaved work is asked about first.
    const float y = p1.y - 4 - bh * 3 - 4;
    if (bevelButton(a, "##demomod", "Demo module", ImVec2(p0.x + 4, y), ImVec2(p0.x + leftW - 4, y + bh), false,
                    "Opens the demo module: four 64-row patterns on many tracks at once (you are asked to save the open song first)"))
      a.guardUnsaved([&a]() { a.newFromTemplate("tracker"); });
  }
  {
    const float y = p1.y - 4 - bh * 2 - 2;
    if (bevelButton(a, "##allkeys", "Every shortcut", ImVec2(p0.x + 4, y), ImVec2(p0.x + leftW - 4, y + bh), a.showShortcuts,
                    "Every command and its keys, from the shortcut set in use"))
      a.showShortcuts = !a.showShortcuts;
  }
  if (bevelButton(a, "##helpexit", "Exit", ImVec2(p0.x + 4, p1.y - 4 - bh), ImVec2(p0.x + leftW - 4, p1.y - 4), false, "Back to the header")) gSwap = Swap::None;
  // The text pane: the heading in a large face, then the text (it scrolls; the bar always shows).
  const ImVec2 t0(p0.x + leftW + 2, p0.y), t1 = p1;
  bevelBox(dl, t0, t1, true);
  ImGui::SetCursorScreenPos(ImVec2(t0.x + 1, t0.y + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  if (ImGui::BeginChild("##helptext", ImVec2(std::max(1.0f, t1.x - t0.x - 2), std::max(1.0f, t1.y - t0.y - 2)), ImGuiChildFlags_AlwaysUseWindowPadding,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
    ImDrawList* cl = ImGui::GetWindowDrawList();
    const float rh = std::round(fs + 4);
    ImVec2 c = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    {
      const float hs = std::round(fs * 1.7f);
      ImFont* hf = a.fontBold ? a.fontBold : a.fontRegular;
      cl->AddText(hf, hs, c, P.text, i18n::tr(kHelpSubjects[std::clamp(gHelpSubject, 0, nSubj - 1)]));
      c.y += hs + 6;
    }
    float y = c.y;
    if (gHelpSubject == 0) {
      float kw = 0;
      for (const HelpRow& r : kPatternKeys) kw = std::max(kw, mono->CalcTextSizeA(fs, FLT_MAX, 0, r.keys).x);
      kw += 16;
      // The rows flow into columns side by side as the band is wide, so more of them show at once
      // (two or three columns at a usual size; one, scrolling, when narrow).
      const int n = static_cast<int>(sizeof kPatternKeys / sizeof kPatternKeys[0]);
      const float visH = ImGui::GetContentRegionAvail().y;
      const int perCol = std::max(1, static_cast<int>(visH / rh));
      const int want = (n + perCol - 1) / perCol;
      const int ncol = std::max(1, std::min(want, static_cast<int>(w / std::max(1.0f, kw + fs * 18))));
      const int rowsPer = (n + ncol - 1) / ncol;
      const float colW2 = std::floor(w / static_cast<float>(ncol));
      for (int i = 0; i < n; ++i) {
        const HelpRow& r = kPatternKeys[i];
        const float cx = c.x + static_cast<float>(i / rowsPer) * colW2, cy = y + static_cast<float>(i % rowsPer) * rh;
        cl->AddText(mono, fs, ImVec2(cx, cy + 2), P.hot, r.keys);
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(cx + kw, cy + 2), colW2 - kw - 8, P.text, i18n::tr(r.what));
      }
      y += static_cast<float>(rowsPer) * rh;
    } else if (gHelpSubject == 2) {
      // The screens and their keys, read from the shortcut set in use (a key changed in the
      // shortcut editor shows here as it is).
      // R6-06: the Tracker's own names (the page tabs, the header's buttons), every key of each.
      struct ScreenKey {
        const char* id;
        const char* what;
      };
      static const ScreenKey ids[] = {{"view.patternEditor", i18n::N_("The Pattern page")},
                                      {"view.playlist", i18n::N_("The Song page")},
                                      {"view.pianoRoll", i18n::N_("The Roll page")},
                                      {"view.channelRack", i18n::N_("The Steps page")},
                                      {"view.mixer", i18n::N_("The Mix page")},
                                      {"view.pads", i18n::N_("The Pads page")},
                                      {"view.channelSettings", i18n::N_("Channel settings: the instrument's editor under the header")},
                                      {"view.audioEditor", i18n::N_("The audio editor")},
                                      {"view.files", i18n::N_("The Files screen")},
                                      {"view.settings", i18n::N_("The Settings screen")},
                                      {"help.trackerKeys", i18n::N_("This help")},
                                      {"view.closeAll", i18n::N_("Back to the main screen (every screen closes, the Pattern page in front)")},
                                      {"pattern.new", i18n::N_("A new pattern (Ins puts it in the song)")},
                                      {"view.headerBand", i18n::N_("The header as one strip, the pattern taller")},
                                      {"view.transposeBox", i18n::N_("The Transpose box")},
                                      {"view.advancedEdit", i18n::N_("The Advanced edit box")},
                                      {"view.cleanUp", i18n::N_("The Clean up box")},
                                      {"view.midiBox", i18n::N_("The MIDI box: the instrument's MIDI out and the MIDI input")},
                                      {"view.sampleTools", i18n::N_("The Sample tools box: range edits on the instrument's sample")},
                                      {"view.sampleEditor", i18n::N_("The Sample editor: the instrument's samples under the pattern")},
                                      {"view.zoomTracksOut", i18n::N_("Narrower tracks, more in view (the Song page: its lanes or timeline)")},
                                      {"view.zoomTracksIn", i18n::N_("Wider tracks, fewer in view")},
                                      {"view.zoomTracksFit", i18n::N_("Every track in view (Ctrl+wheel zooms too)")},
                                      {"view.browser", i18n::N_("The browser")},
                                      {"view.pluginPicker", i18n::N_("The plugin picker")},
                                      {"help.shortcuts", i18n::N_("Every shortcut")}};
      float kw = 0;
      for (const ScreenKey& e : ids) kw = std::max(kw, mono->CalcTextSizeA(fs, FLT_MAX, 0, keys::labels(e.id).c_str()).x);
      kw = std::min(kw, w * 0.45f) + 16;
      for (const ScreenKey& e : ids) {
        if (!keys::findCommand(e.id)) continue;
        const std::string k = keys::labels(e.id);
        theme::textEllipsis(cl, mono, fs, ImVec2(c.x, y + 2), kw - 12, k.empty() ? P.textFaint : P.hot, k.empty() ? "-" : k.c_str());
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(c.x + kw, y + 2), w - kw, P.text, i18n::tr(e.what));
        y += rh;
      }
    } else if (gHelpSubject == 3) {
      // Sub-headings in the bold face, each paragraph wrapped under it, indented.
      ImGui::SetCursorScreenPos(ImVec2(c.x, y));
      for (const HelpPara& h : kUsing) {
        ImGui::PushFont(a.fontBold ? a.fontBold : a.fontRegular, fs);
        ImGui::TextUnformatted(i18n::tr(h.head));
        ImGui::PopFont();
        ImGui::Indent(fs * 1.5f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(100.0f, w - fs * 1.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(P.textDim));
        ImGui::TextWrapped("%s", i18n::tr(h.text));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Unindent(fs * 1.5f);
        ImGui::Dummy(ImVec2(1, 4));
      }
      y = ImGui::GetCursorScreenPos().y;
    } else {
      cl->AddText(a.fontRegular, fs, ImVec2(c.x, y + 2), P.textDim,
                  i18n::tr("The effect field shows a note's own properties as a letter and two hex digits:"));
      y += rh + 4;
      const int typical[] = {0, 0, 0x0A, 0x80, 0x80, 0x40};
      for (int i = 0; trk::kEffectLetters[i]; ++i) {
        const std::string h = trk::effectHelp(trk::kEffectLetters[i], typical[i]);
        // R7-10: G and P are on or off (written 01); the others take any two digits.
        const char l = trk::kEffectLetters[i];
        const std::string code = std::string(1, l) + (l == 'G' || l == 'P' ? "01" : "xx");
        cl->AddText(mono, fs, ImVec2(c.x, y + 2), P.hot, code.c_str());
        theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(c.x + fs * 4, y + 2), w - fs * 4, P.text, h.c_str());
        y += rh;
      }
      y += 4;
      cl->AddText(a.fontRegular, fs, ImVec2(c.x, y + 2), P.textDim, i18n::tr("Type the letter, then the digits; Delete puts the property back."));
      y += rh;
    }
    ImGui::SetCursorScreenPos(ImVec2(c.x, y));
    ImGui::Dummy(ImVec2(1, 1));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

// Files (the header's left part, the classic Disk op. screen): Gridline's own file screen. Left to
// right: what the list shows (songs or samples) and the format Save writes; the file commands; the
// places (the folder above, the root, home, the project's folder, the recent files); the list,
// folders first, with each file's size; under them the folder shown and the name field. A click
// picks a file (its name goes to the field), a double-click opens a folder or the file (a song opens
// as Open does, a sample goes into the current instrument). Backspace is the folder above.
bool gDiskTyping = false;  // the name field is being typed in

struct DiskEntry {
  std::string name;
  bool dir = false;
  uintmax_t size = 0;
};
int gDiskKind = 0;    // 0 songs (projects, song files, MIDI), 1 samples
int gDiskFormat = 0;  // Save writes: 0 a project, 1 an audio file (the export dialog)
bool gDiskRecent = false;  // the list shows the recent files instead of a folder
std::string gDiskDir, gDiskSel;
char gDiskName[256] = {};
std::vector<DiskEntry> gDiskList;
std::string gDiskListed;  // what the list was read for (folder, kind, all, sort)
double gDiskListedAt = -10;

std::string diskHome() {
  const char* h = std::getenv("HOME");
#ifdef _WIN32
  if (!h) h = std::getenv("USERPROFILE");
#endif
  return h ? std::string(h) : std::string("/");
}

std::string& diskDir(App& a) {
  if (gDiskDir.empty()) {
    std::error_code ec;
    const std::string kept = a.settings.getString("tracker.diskFolder");
    if (!kept.empty() && std::filesystem::is_directory(kept, ec)) gDiskDir = kept;
    else if (!a.projectPath.empty()) gDiskDir = std::filesystem::path(a.projectPath).parent_path().string();
    if (gDiskDir.empty()) gDiskDir = diskHome();
  }
  return gDiskDir;
}

void diskGo(App& a, const std::string& dir) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
    a.notify(std::string(i18n::tr("No such folder: ")) + dir, true);
    return;
  }
  gDiskDir = std::filesystem::path(dir).lexically_normal().string();
  if (gDiskDir.size() > 1 && (gDiskDir.back() == '/' || gDiskDir.back() == '\\')) gDiskDir.pop_back();
  gDiskSel.clear();
  gDiskRecent = false;
  gDiskListed.clear();
  a.settings.set("tracker.diskFolder", gDiskDir);
}

bool diskShows(App& a, const std::string& path) {
  if (a.settings.getBool("tracker.diskAll")) return true;
  const browse::Kind k = browse::kindOf(path);
  return gDiskKind == 1 ? k == browse::Kind::Audio : k == browse::Kind::Project || k == browse::Kind::Flp || k == browse::Kind::Midi;
}

void diskRead(App& a) {
  const bool byType = a.settings.getString("tracker.diskSort") == "type", all = a.settings.getBool("tracker.diskAll");
  const std::string key = diskDir(a) + "|" + std::to_string(gDiskKind) + (all ? "a" : "") + (byType ? "t" : "") + (gDiskRecent ? "r" : "");
  if (key == gDiskListed && ImGui::GetTime() - gDiskListedAt < 2.0) return;  // read again every two seconds (files come and go)
  gDiskListed = key;
  gDiskListedAt = ImGui::GetTime();
  gDiskList.clear();
  std::error_code ec;
  if (gDiskRecent) {
    for (const std::string& f : a.recentFiles) {
      DiskEntry e;
      e.name = f;
      e.size = std::filesystem::file_size(f, ec);
      if (!ec) gDiskList.push_back(e);
      ec.clear();
    }
    return;
  }
  for (std::filesystem::directory_iterator it(gDiskDir, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.') continue;  // hidden files stay hidden
    DiskEntry e;
    e.name = name;
    std::error_code ec2;
    e.dir = it->is_directory(ec2);
    if (!e.dir) {
      if (!diskShows(a, it->path().string())) continue;
      e.size = it->file_size(ec2);
    }
    gDiskList.push_back(e);
  }
  auto lowerOf = [](std::string t) {
    for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return t;
  };
  std::sort(gDiskList.begin(), gDiskList.end(), [&](const DiskEntry& x, const DiskEntry& y) {
    if (x.dir != y.dir) return x.dir;  // folders first
    if (byType && !x.dir) {
      const std::string ex = lowerOf(std::filesystem::path(x.name).extension().string()), ey = lowerOf(std::filesystem::path(y.name).extension().string());
      if (ex != ey) return ex < ey;
    }
    return lowerOf(x.name) < lowerOf(y.name);
  });
  // A picked file that is gone (moved to the trash, renamed elsewhere) is no longer picked.
  if (!gDiskSel.empty() && std::none_of(gDiskList.begin(), gDiskList.end(), [](const DiskEntry& e) { return e.name == gDiskSel; })) gDiskSel.clear();
}

std::string diskSize(uintmax_t b) {
  char t[32];
  if (b < 1000) std::snprintf(t, sizeof t, "%u B", static_cast<unsigned>(b));
  else if (b < 1000ull * 1000) std::snprintf(t, sizeof t, "%.0f kB", static_cast<double>(b) / 1024.0);
  else std::snprintf(t, sizeof t, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
  return t;
}

void diskOpen(App& a, const std::string& path) {
  if (gDiskKind == 1 && browse::kindOf(path) == browse::Kind::Audio) {
    if (a.doc.song().channelIndex(a.selChannel) < 0) {
      a.notify(i18n::tr("Pick an instrument first: the sample goes into the current instrument"), true);
      return;
    }
    // cp14: a Tracker Sampler takes the file into its selected sample (its other samples and keys
    // stay), as the classic disk op loads into the current sample; other instruments as before.
    const Channel* ch = a.doc.song().channel(a.selChannel);
    if (ch && ch->plugin.type == tracker::kInstrumentType) {
      const int k = packs::trackerpack::headerLoadSample(a, a.selChannel, path);
      if (k < 0) a.notify(i18n::tr("That file holds no audio Gridline can read"), true);
      else a.notify(std::string(i18n::tr("Loaded into sample")) + " " + std::to_string(k + 1));
      return;
    }
    browserui::loadChannelSample(a, a.selChannel, path);
    return;
  }
  browserui::openFile(a, path);
}

// The name field's text as a path in the folder shown ("" when empty).
std::string diskTarget(App& a) {
  std::string n = gDiskName;
  while (!n.empty() && n.back() == ' ') n.pop_back();
  if (n.empty()) return "";
  std::filesystem::path p(n);
  return p.is_absolute() ? p.string() : (std::filesystem::path(diskDir(a)) / p).string();
}

// The file name a rename to `typed` gives `path`: a file keeps its extension when none is typed
// (as the browser's rename).
std::string diskRenamed(const std::string& path, const std::string& typed) {
  std::string n = std::filesystem::path(typed).filename().string();
  while (!n.empty() && n.back() == ' ') n.pop_back();
  std::error_code ec;
  const std::string ext = std::filesystem::is_directory(path, ec) ? std::string() : std::filesystem::path(path).extension().string();
  auto low = [](std::string t) {
    for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return t;
  };
  if (!ext.empty() && (n.size() < ext.size() || low(n.substr(n.size() - ext.size())) != low(ext))) n += ext;
  return n;
}

void diskSave(App& a) {
  if (gDiskFormat == 1) {  // an audio file: the export dialog (its own name and settings)
    a.showExport = true;
    return;
  }
  std::string path = diskTarget(a);
  if (path.empty()) {
    const std::string cur = a.projectFile();
    path = (std::filesystem::path(diskDir(a)) / (cur.empty() ? std::string("Untitled") : std::filesystem::path(cur).stem().string())).string();
  }
  // A project file name: "Song" and "Song.mp3" become "Song.gridline" and "Song.mp3.gridline" (a
  // project is never written under an audio or other file's name); .gridline and .json stay.
  {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != kProjectExt && ext != ".json") path += kProjectExt;
  }
  std::error_code ec;
  auto write = [&a, path] {
    if (a.saveProject(path)) {
      gDiskListed.clear();  // R7-14: saveProject says "Saved" itself (one toast)
    }
  };
  if (std::filesystem::exists(path, ec)) {
    a.confirm = {};
    a.confirm.open = true;
    a.confirm.title = std::string(i18n::tr("Replace ")) + std::filesystem::path(path).filename().string() + "?";
    a.confirm.subtitle = i18n::tr("A file of that name is in this folder.");
    a.confirm.okLabel = i18n::N_("Replace");
    a.confirm.action = write;
  } else {
    write();
  }
}

void diskScreen(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  ImFont* bold = a.fontBold ? a.fontBold : a.fontRegular;
  diskDir(a);
  diskRead(a);
  browserTrashDialog(a);
  const float W = p1.x - p0.x, gap = 2;
  const float rh = std::max(std::round(fs + 4), std::floor((p1.y - p0.y - 6) / 11));
  // Columns: kinds and format, commands, places, the list (the rest).
  float c1 = std::round(std::max(fs * 6.5f, W * 0.17f)), c2 = std::round(std::max(fs * 6.0f, W * 0.15f)), c3 = std::round(std::max(fs * 4.5f, W * 0.11f));
  const float bottomH = std::round(rh * 2 + 6);
  const float yB = p1.y - bottomH;
  const float x1 = p0.x + c1, x2 = x1 + c2, x3 = x2 + c3;
  // ---- the kinds and the format ----
  bevelBox(dl, p0, ImVec2(x1, yB));
  float y = p0.y + 3;
  dl->AddText(bold, fs, ImVec2(p0.x + 6, std::round(y + (rh - fs) * 0.5f)), P.text, i18n::tr("Show"));
  y += rh;
  const char* kinds[] = {i18n::N_("Songs"), i18n::N_("Samples")};
  const char* kindHints[] = {i18n::N_("Projects, song files and MIDI files: Save saves the project, a double-click opens one"),
                             i18n::N_("Audio files: a double-click loads one into the current instrument")};
  for (int i = 0; i < 2; ++i, y += rh) {
    ImGui::PushID(i);
    if (radioRow(a, "##dkind", kinds[i], ImVec2(p0.x + 3, y), ImVec2(x1 - 3, y + rh), gDiskKind == i, kindHints[i])) gDiskKind = i, gDiskListed.clear();
    ImGui::PopID();
  }
  y += std::round(rh * 0.5f);
  dl->AddText(bold, fs, ImVec2(p0.x + 6, std::round(y + (rh - fs) * 0.5f)), gDiskKind == 0 ? P.text : P.textFaint, i18n::tr("Save as"));
  y += rh;
  const char* formats[] = {i18n::N_("Project"), i18n::N_("Audio file")};
  const char* formatHints[] = {i18n::N_("Save writes the project under the name in the field"), i18n::N_("Save opens the export: the song as an audio file")};
  if (gDiskKind != 0) ImGui::BeginDisabled();
  for (int i = 0; i < 2; ++i, y += rh) {
    ImGui::PushID(10 + i);
    if (radioRow(a, "##dfmt", formats[i], ImVec2(p0.x + 3, y), ImVec2(x1 - 3, y + rh), gDiskFormat == i, formatHints[i])) gDiskFormat = i;
    ImGui::PopID();
  }
  if (gDiskKind != 0) ImGui::EndDisabled();
  // ---- the commands ----
  bevelBox(dl, ImVec2(x1, p0.y), ImVec2(x2, yB));
  const bool all = a.settings.getBool("tracker.diskAll");
  const std::string selPath = gDiskSel.empty() ? std::string() : gDiskRecent ? gDiskSel : (std::filesystem::path(gDiskDir) / gDiskSel).string();
  // R7-02: where Files may change files (its own rule: any folder the user may write in). Asked
  // again when the pick, the folder or the listing changes; R7-07: a prompt that moved a file to the
  // trash reads the list again at once (the picked file goes with it).
  static std::string trashAsked;  // R8-05: the file the trash prompt was about
  if (filesTrashMoved()) {
    gDiskListed.clear(), gDiskListedAt = -10;
    // Gone to the trash: the Name field no longer offers its name (a Save would write it again).
    std::error_code ec;
    if (!trashAsked.empty() && !std::filesystem::exists(std::filesystem::u8path(trashAsked), ec) &&
        (std::filesystem::u8path(trashAsked).filename().u8string() == gDiskName || std::filesystem::u8path(trashAsked).stem().u8string() == gDiskName))
      gDiskName[0] = 0;
    trashAsked.clear();
  }
  static std::string whyKey, whySel, whyHere;
  if (const std::string key = selPath + "|" + gDiskDir + "|" + gDiskListed + (gDiskRecent ? "r" : ""); key != whyKey) {
    whyKey = key;
    whySel = selPath.empty() ? std::string() : filesLocked(a, selPath, false);
    whyHere = gDiskRecent ? std::string() : filesLocked(a, gDiskDir, true);
  }
  {
    const int n = 8;
    const float bh = std::floor((yB - p0.y - 4 - static_cast<float>(n - 1)) / static_cast<float>(n));
    int i = 0;
    auto slotY = [&](int k) { return std::round(p0.y + 2 + static_cast<float>(k) * (bh + 1)); };
    auto btn = [&](const char* id, const char* label, const char* hint, bool enabled = true) {
      const float yy = slotY(i++);
      if (!enabled) ImGui::BeginDisabled();
      const bool c = bevelButton(a, id, label, ImVec2(x1 + 2, yy), ImVec2(x2 - 2, yy + bh), false, hint);
      if (!enabled) ImGui::EndDisabled();
      return c && enabled;
    };
    if (btn("##dsave", "Save", "Saves under the name in the field, in this folder (asks before replacing a file); an empty field: the project's name",
            gDiskKind == 0))
      diskSave(a);
    if (btn("##ddel", "Delete", whySel.empty() ? "Moves the picked file or folder to the trash (asks first)" : whySel.c_str(), !selPath.empty() && whySel.empty()))
      filesAskTrash(selPath), trashAsked = selPath;
    if (btn("##dren", "Rename", whySel.empty() ? "Renames the picked file to the name in the field (the project's references follow)" : whySel.c_str(),
            !selPath.empty() && gDiskName[0] && whySel.empty())) {
      const std::string err = filesRename(a, selPath, gDiskName);
      if (!err.empty()) {
        a.notify(err, true);
      } else if (!gDiskRecent) {
        gDiskSel = diskRenamed(selPath, gDiskName);  // the name as the rename made it
        std::snprintf(gDiskName, sizeof gDiskName, "%s", gDiskSel.c_str());
        gDiskListed.clear();
      } else {
        gDiskSel.clear(), gDiskListed.clear();  // the recent list keeps the old path: nothing stays picked
      }
    }
    if (btn("##dmkdir", "New folder", whyHere.empty() ? "Makes a folder here, named as the field says (empty: New folder)" : whyHere.c_str(),
            !gDiskRecent && whyHere.empty())) {
      const fileops::Result r = fileops::makeFolder(gDiskDir, gDiskName[0] ? std::string(gDiskName) : std::string(i18n::tr("New folder")));
      if (!r.ok) a.notify(i18n::tr(r.error.c_str()), true);
      else gDiskSel = std::filesystem::path(r.path).filename().string(), gDiskListed.clear();
    }
    if (btn("##dref", "Refresh", "Reads the folder again")) gDiskListed.clear();
    if (btn("##dgo", "Go to", "Goes to the folder typed in the field (a full path, or one inside this folder; ~ is home)", gDiskName[0] != 0)) {
      std::string t = gDiskName;
      if (!t.empty() && t[0] == '~') t = diskHome() + t.substr(1);
      std::filesystem::path p(t);
      diskGo(a, p.is_absolute() ? p.string() : (std::filesystem::path(gDiskDir) / p).string());
    }
    {
      const float yy = slotY(i++);
      if (checkRow(a, "##dall", "All files", ImVec2(x1 + 2, yy), ImVec2(x2 - 2, yy + bh), all, "Lists every file, not only the songs or samples"))
        a.settings.set("tracker.diskAll", !all), gDiskListed.clear();
    }
    if (btn("##dexit", "Exit", "Back to the header")) gSwap = Swap::None;
  }
  // ---- the places ----
  bevelBox(dl, ImVec2(x2, p0.y), ImVec2(x3, yB));
  {
    struct Place {
      const char* id;
      const char* label;
      const char* hint;
    };
    const Place places[] = {{"##dup", i18n::N_("Up"), i18n::N_("The folder above (Backspace)")},
                            {"##droot", i18n::N_("Root"), i18n::N_("The top of the file system")},
                            {"##dhome", i18n::N_("Home"), i18n::N_("Your home folder")},
                            {"##dproj", i18n::N_("Project"), i18n::N_("The open project's folder")},
                            {"##drec", i18n::N_("Recent"), i18n::N_("The files opened or saved last")}};
    const float bh = std::max(rh, std::floor((yB - p0.y - 4 - 4) / 5.0f));
    for (int i = 0; i < 5; ++i) {
      const float yy = std::round(p0.y + 2 + static_cast<float>(i) * (bh + 1));
      const bool on = i == 4 && gDiskRecent;
      const bool enabled = i != 3 || !a.projectPath.empty();
      if (!enabled) ImGui::BeginDisabled();
      const bool c = bevelButton(a, places[i].id, places[i].label, ImVec2(x2 + 2, yy), ImVec2(x3 - 2, std::min(yy + bh, yB - 2)), on, places[i].hint);
      if (!enabled) ImGui::EndDisabled();
      if (!c || !enabled) continue;
      switch (i) {
        case 0: diskParent(a); break;
        case 1: diskGo(a, std::filesystem::path(gDiskDir).root_path().string()); break;
        case 2: diskGo(a, diskHome()); break;
        case 3: diskGo(a, std::filesystem::path(a.projectPath).parent_path().string()); break;
        default: gDiskRecent = !gDiskRecent, gDiskSel.clear(), gDiskListed.clear(); break;
      }
    }
  }
  // ---- the list ----
  const ImVec2 l0(x3 + gap, p0.y), l1(p1.x, yB);
  bevelBox(dl, l0, l1, true);
  ImGui::SetCursorScreenPos(ImVec2(l0.x + 2, l0.y + 2));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::BeginChild("##disklist", ImVec2(std::max(1.0f, l1.x - l0.x - 4), std::max(1.0f, l1.y - l0.y - 4)), ImGuiChildFlags_None)) {
    ImDrawList* cl = ImGui::GetWindowDrawList();
    const float lrh = std::round(fs + 3), lw = ImGui::GetContentRegionAvail().x;
    if (gDiskList.empty())
      cl->AddText(a.fontRegular, fs, ImVec2(ImGui::GetCursorScreenPos().x + 4, ImGui::GetCursorScreenPos().y + 2), P.textFaint,
                  i18n::tr(gDiskRecent ? "No recent files" : all ? "This folder is empty" : "No songs or samples here (All files lists the rest)"));
    std::string open;
    for (size_t i = 0; i < gDiskList.size(); ++i) {
      const DiskEntry& e = gDiskList[i];
      const ImVec2 q = ImGui::GetCursorScreenPos();
      ImGui::PushID(static_cast<int>(i));
      const bool click = ImGui::InvisibleButton("##dl", ImVec2(std::max(1.0f, lw), lrh));
      const bool hov = ImGui::IsItemHovered(), dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
      ImGui::PopID();
      const bool sel = e.name == gDiskSel;
      if (sel) cl->AddRectFilled(q, ImVec2(q.x + lw, q.y + lrh), theme::withAlpha(P.surface600, 0.95f));
      else if (hov) cl->AddRectFilled(q, ImVec2(q.x + lw, q.y + lrh), theme::withAlpha(P.surface700, 0.6f));
      const std::string shown = gDiskRecent ? std::filesystem::path(e.name).filename().string() : e.dir ? e.name + "/" : e.name;
      const std::string size = e.dir ? std::string() : diskSize(e.size);
      const float sw = size.empty() ? 0 : mono->CalcTextSizeA(fs, FLT_MAX, 0, size.c_str()).x;
      theme::textEllipsis(cl, a.fontRegular, fs, ImVec2(q.x + 4, q.y + 1), lw - sw - 14, e.dir ? P.text : sel ? P.text : P.textDim, shown.c_str());
      if (!size.empty()) cl->AddText(mono, fs, ImVec2(std::round(q.x + lw - 4 - sw), q.y + 1), sel ? valueInk() : P.textFaint, size.c_str());
      if (gDiskRecent && hov) ui::Hint(e.name.c_str());
      if (click) {
        gDiskSel = e.name;
        std::snprintf(gDiskName, sizeof gDiskName, "%s", std::filesystem::path(e.name).filename().string().c_str());
      }
      if (dbl) open = e.dir ? (std::filesystem::path(gDiskDir) / e.name).string() : gDiskRecent ? e.name : (std::filesystem::path(gDiskDir) / e.name).string();
      if (dbl && e.dir) open = "/dir:" + open;
    }
    if (open.rfind("/dir:", 0) == 0) diskGo(a, open.substr(5));
    else if (!open.empty()) diskOpen(a, open);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  // ---- the folder shown and the name field ----
  bevelBox(dl, ImVec2(p0.x, yB + gap), p1);
  const float ty = std::round(yB + gap + 3 + (rh - fs) * 0.5f);
  const char* pathLabel = i18n::tr(gDiskRecent ? "Recent files" : "Folder");
  const float plw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, pathLabel).x + 8;
  dl->AddText(a.fontRegular, fs, ImVec2(p0.x + 6, ty), P.textDim, pathLabel);
  {
    // The folder, its end in view (a long path loses its start).
    std::string path = gDiskRecent ? std::string() : gDiskDir;
    const float maxW = p1.x - p0.x - plw - 14;
    while (path.size() > 4 && mono->CalcTextSizeA(fs, FLT_MAX, 0, path.c_str()).x > maxW) path = "..." + path.substr(std::min(path.size(), static_cast<size_t>(6)));
    dl->AddText(mono, fs, ImVec2(p0.x + 6 + plw, ty), valueInk(), path.c_str());
  }
  const float fy = std::round(yB + gap + 3 + rh);
  const char* nameLabel = i18n::tr("Name");
  const float nlw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, nameLabel).x + 8;
  dl->AddText(a.fontRegular, fs, ImVec2(p0.x + 6, std::round(fy + (rh - fs) * 0.5f)), P.textDim, nameLabel);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 + nlw, fy));
  ImGui::SetNextItemWidth(std::max(40.0f, p1.x - p0.x - nlw - 14));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, std::max(0.0f, std::floor((rh - fs) * 0.5f) - 1)));
  ImGui::InputText("##dname", gDiskName, sizeof gDiskName);
  ImGui::PopStyleVar();
  ui::Hint("The name Save, Rename, New folder and Go to use");
  gDiskTyping = ImGui::IsItemActive();
}

}  // namespace

bool diskParent(App& a) {
  if (gSwap != Swap::Disk) return false;
  if (gDiskRecent) {  // the recent files: back to the folder
    gDiskRecent = false;
    gDiskListed.clear();
    return true;
  }
  const std::filesystem::path p(diskDir(a));
  const std::filesystem::path up = p.parent_path();
  if (!up.empty() && up != p) diskGo(a, up.string());
  return true;
}

namespace {
// cp14: the Audio page (the classic config's audio page, as Gridline's audio settings): the output
// device list (the wheel scrolls it), the sample rate and buffer size as radios, the latency. A
// change applies at once and asks to keep it, as in the Settings window.
template <class Group, class RowY>
void configAudio(App& a, ImVec2 t0, ImVec2 t1, float pad, float rowH, Group group, RowY rowY) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  Settings& st = a.settings;
  const int rowsFit = std::max(1, static_cast<int>(std::floor((t1.y - pad - 4 - rowY(0)) / (rowH + 1))));
  const float W = t1.x - t0.x - pad * 4;
  const float wDev = std::floor(W * 0.46f), wRate = std::floor(W * 0.2f);
  const float xa = t0.x + pad, xb = xa + wDev + pad, xc = xb + wRate + pad, xe = t1.x - pad;
  group(xa, xa + wDev, "Output device", rowsFit);
  if (st.getString("audio.backend") == "asio") {
    dl->AddText(a.fontRegular, fs, ImVec2(xa + 8, std::round(rowY(0) + (rowH - fs) * 0.5f)), P.textDim, i18n::tr("ASIO devices: All settings, Audio"));
  } else {
    // The system default first, then every device.
    const int total = static_cast<int>(a.audioDevices.size()) + 1;
    if (ImGui::IsMouseHoveringRect(ImVec2(xa, rowY(0)), ImVec2(xa + wDev, rowY(rowsFit))) && ImGui::GetIO().MouseWheel != 0)
      gCfgDevFirst += ImGui::GetIO().MouseWheel > 0 ? -1 : 1;
    gCfgDevFirst = std::clamp(gCfgDevFirst, 0, std::max(0, total - rowsFit));
    for (int r = 0; r < rowsFit && gCfgDevFirst + r < total; ++r) {
      const int i = gCfgDevFirst + r;
      const std::string name = i == 0 ? std::string(i18n::tr("System default")) : a.audioDevices[static_cast<size_t>(i - 1)];
      const bool on = i == 0 ? a.audioDevice.empty() : a.audioDevice == name;
      ImGui::PushID(i);
      if (radioRow(a, "##cfgdev", name.c_str(), ImVec2(xa + 4, rowY(r)), ImVec2(xa + wDev - 4, rowY(r) + rowH), on,
                   i18n::tr("Sound goes out through this device (Keep or Revert after the change)")) && !on)
        ui::confirmAudioDevice(a, i == 0 ? std::string() : name);
      ImGui::PopID();
    }
    if (total > rowsFit) {
      const std::string count = std::to_string(gCfgDevFirst + 1) + "-" + std::to_string(std::min(total, gCfgDevFirst + rowsFit)) + "/" + std::to_string(total);
      gCfgDevFirst += listArrows(a, "##cfgdevpg", count, xa + wDev - 6, t0.y + pad + 3, 1);
    }
  }
  static const int kRates[] = {44100, 48000, 88200, 96000};
  group(xb, xb + wRate, "Sample rate", 4);
  for (int i = 0; i < 4; ++i) {
    const std::string label = std::to_string(kRates[i]);
    ImGui::PushID(30 + i);
    if (radioRow(a, "##cfgrate", label.c_str(), ImVec2(xb + 4, rowY(i)), ImVec2(xb + wRate - 4, rowY(i) + rowH), a.audioSampleRate == kRates[i],
                 i18n::tr("Samples a second the sound is made at (Keep or Revert after the change)")) &&
        a.audioSampleRate != kRates[i]) {
      const dispconf::Snapshot before = dispconf::take(st, dispconf::audioKeys());
      st.set("audio.sampleRate", kRates[i]);
      ui::confirmAudio(a, before);
    }
    ImGui::PopID();
  }
  static const int kBufs[] = {64, 128, 256, 512, 1024, 2048};
  group(xc, xe, "Buffer size", 4);
  const float half = std::floor((xe - xc - 8) / 2);
  for (int i = 0; i < 6; ++i) {
    const float rx = xc + 4 + static_cast<float>(i / 3) * half, ry = rowY(i % 3);
    const std::string label = std::to_string(kBufs[i]);
    ImGui::PushID(40 + i);
    if (radioRow(a, "##cfgbuf", label.c_str(), ImVec2(rx, ry), ImVec2(rx + half, ry + rowH), a.audioBuffer == kBufs[i],
                 i18n::tr("Frames the sound is made in at a time: fewer answer sooner, more are safer from dropouts")) &&
        a.audioBuffer != kBufs[i]) {
      const dispconf::Snapshot before = dispconf::take(st, dispconf::audioKeys());
      st.set("audio.bufferFrames", kBufs[i]);
      ui::confirmAudio(a, before);
    }
    ImGui::PopID();
  }
  char lat[64];
  std::snprintf(lat, sizeof lat, "%.1f ms", 1000.0 * a.audioBuffer / std::max(1, a.audioSampleRate));
  const float ly = std::round(rowY(3) + (rowH - fs) * 0.5f);
  dl->AddText(a.fontRegular, fs, ImVec2(xc + 8, ly), P.textDim, i18n::tr("Latency"));
  ImFont* mono = monoFont(a);
  dl->AddText(mono, fs, ImVec2(std::round(xe - 8 - mono->CalcTextSizeA(fs, FLT_MAX, 0, lat).x), ly), valueInk(), lat);
}

// cp14: the MIDI input page (the classic config's MIDI input page): every input device with its
// on / off box (the wheel scrolls a long list), the velocity curve, and where the notes go.
template <class Group, class RowY>
void configMidi(App& a, ImVec2 t0, ImVec2 t1, float pad, float rowH, Group group, RowY rowY) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const int rowsFit = std::max(1, static_cast<int>(std::floor((t1.y - pad - 4 - rowY(0)) / (rowH + 1))));
  const float W = t1.x - t0.x - pad * 3;
  const float wDev = std::floor(W * 0.6f);
  const float xa = t0.x + pad, xb = xa + wDev + pad, xe = t1.x - pad;
  group(xa, xa + wDev, "Input devices", rowsFit);
  group(xb, xe, "Notes", 3);
  if (!a.midi) {
    dl->AddText(a.fontRegular, fs, ImVec2(xa + 8, std::round(rowY(0) + (rowH - fs) * 0.5f)), P.textDim, i18n::tr("MIDI is off in this run"));
    return;
  }
  MidiManager& mm = *a.midi;
  bool changed = false, reopen = false;
  // The devices in two columns, as many rows as fit.
  const int total = static_cast<int>(mm.inputs.size());
  if (total == 0) {
    const char* t = mm.serviceAvailable() ? i18n::tr("No MIDI input device is connected") : i18n::tr("The MIDI service has not answered yet");
    dl->AddText(a.fontRegular, fs, ImVec2(xa + 8, std::round(rowY(0) + (rowH - fs) * 0.5f)), P.textDim, t);
  } else {
    const int cols = 2, fit = rowsFit * cols;
    if (ImGui::IsMouseHoveringRect(ImVec2(xa, rowY(0)), ImVec2(xa + wDev, rowY(rowsFit))) && ImGui::GetIO().MouseWheel != 0)
      gCfgMidiFirst += ImGui::GetIO().MouseWheel > 0 ? -cols : cols;
    gCfgMidiFirst = std::clamp(gCfgMidiFirst, 0, std::max(0, ((total - fit + cols - 1) / cols) * cols));
    const float cw = std::floor((wDev - 8) / static_cast<float>(cols));
    for (int k = 0; k < fit && gCfgMidiFirst + k < total; ++k) {
      const int i = gCfgMidiFirst + k;
      MidiInputConfig* c = mm.inputConfig(mm.inputs[static_cast<size_t>(i)]);
      if (!c) continue;
      const float x = xa + 4 + static_cast<float>(k % cols) * cw, y = rowY(k / cols);
      ImGui::PushID(i);
      const std::string hint = std::string(i18n::tr("Notes and controls from this device")) + ": " + c->name;
      const float boxW = std::max(8.0f, std::round(std::min(fs * 0.8f, rowH - 2)));
      const std::string shown = midEllipsis(a.fontRegular, fs, c->name, cw - 2 - boxW - 14);
      if (checkRow(a, "##cfgmdev", shown.c_str(), ImVec2(x, y), ImVec2(x + cw - 2, y + rowH), c->enabled, hint.c_str())) c->enabled = !c->enabled, changed = reopen = true;
      ImGui::PopID();
    }
    if (total > fit) {
      const std::string count = std::to_string(gCfgMidiFirst + 1) + "-" + std::to_string(std::min(total, gCfgMidiFirst + fit)) + "/" + std::to_string(total);
      gCfgMidiFirst += listArrows(a, "##cfgmidipg", count, xa + wDev - 6, t0.y + pad + 3, cols);
    }
  }
  MidiRouting& rt = mm.routing;
  const float gw = xe - xb - 8;
  const float lw2 = labelWidth(a, {i18n::N_("Velocity"), i18n::N_("Fixed")}, 10);
  if (const int d = stepper(a, "##cfgmvel", i18n::N_("Velocity"), velocityCurveName(rt.velocity), ImVec2(xb + 4, rowY(0)), gw, rowH,
                            i18n::tr("How hard the keys play: the curve the played velocities go through"), lw2, true)) {
    const int nC = static_cast<int>(VelocityCurve::Count);
    rt.velocity = static_cast<VelocityCurve>(((static_cast<int>(rt.velocity) + d) % nC + nC) % nC);
    changed = true;
  }
  if (radioRow(a, "##cfgmcur", i18n::N_("Current instrument"), ImVec2(xb + 4, rowY(1)), ImVec2(xe - 4, rowY(1) + rowH), !rt.byChannel,
               i18n::tr("Every MIDI channel plays the instrument selected here")) && rt.byChannel)
    rt.byChannel = false, changed = true;
  if (radioRow(a, "##cfgmbych", i18n::N_("By MIDI channel"), ImVec2(xb + 4, rowY(2)), ImVec2(xe - 4, rowY(2) + rowH), rt.byChannel,
               i18n::tr("Each MIDI channel plays the instrument Settings, MIDI routes it to")) && !rt.byChannel)
    rt.byChannel = true, changed = true;
  if (reopen) mm.apply();
  if (changed) {
    a.publishMidi();
    mm.save((std::filesystem::path(a.configDir()) / "midi.json").string());
  }
}

// The settings screen (the classic Config screen, refs 161 and 166): the pattern editor's own
// options in the header band (the band swaps to it, the pattern stays in view below), on two
// pages as the classic Layout and Miscellaneous pages; every other setting (audio, MIDI, keys) is
// one click away in the Settings window, as the classic I/O Devices page.
void configScreen(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float leftW = std::round(std::max(150.0f, fs * 11));
  // The left panel (refs 161, 166): a title, the pages as a radio list, then the buttons stacked at
  // the bottom (every other setting, Exit last).
  bevelBox(dl, p0, ImVec2(p0.x + leftW, p1.y));
  const float bh = std::round(fs + 9), rr = std::round(fs + 5);
  boxTitle(a, ImVec2(p0.x + 6, p0.y + 4), "Settings");
  // cp14: the classic config's four pages that map to Gridline settings, in its order.
  const int order[] = {2, 0, 1, 3};
  const char* pages[] = {i18n::N_("Layout"), i18n::N_("Miscellaneous"), i18n::N_("Audio"), i18n::N_("MIDI input")};
  const char* pageHints[] = {i18n::N_("How the pattern looks: columns, numbers, rows, tracks"),
                             i18n::N_("Editing, recording, the clipboard and the Files screen"),
                             i18n::N_("The output device, the sample rate and the buffer size"),
                             i18n::N_("The MIDI input devices, the velocity curve and where the notes go")};
  for (int j = 0; j < 4; ++j) {
    const int i = order[j];
    const float y = std::round(p0.y + 6 + fs + static_cast<float>(j) * rr);
    ImGui::PushID(i);
    if (radioRow(a, "##cfgpage", pages[i], ImVec2(p0.x + 4, y), ImVec2(p0.x + leftW - 4, y + rr), gConfigPage == i, pageHints[i])) gConfigPage = i;
    ImGui::PopID();
  }
  if (bevelButton(a, "##cfgall", "All settings", ImVec2(p0.x + 4, p1.y - 4 - bh * 2 - 2), ImVec2(p0.x + leftW - 4, p1.y - 4 - bh - 2), a.showSettings,
                  "Every setting: audio, interface, keys, plugins (the Settings window)"))
    a.showSettings = !a.showSettings;
  if (bevelButton(a, "##cfgexit", "Exit", ImVec2(p0.x + 4, p1.y - 4 - bh), ImVec2(p0.x + leftW - 4, p1.y - 4), false, "Back to the header")) gSwap = Swap::None;
  const ImVec2 t0(p0.x + leftW + 2, p0.y), t1 = p1;
  bevelBox(dl, t0, t1);
  // The page: titled groups side by side (the classic config pages' small titled boxes).
  const float pad = 6;
  const float rowH = std::round(fs + 6);
  // A group: a sunken box with its title; its rows under the title.
  auto group = [&](float gx0, float gx1, const char* title, int rows) {
    const float gy1 = std::min(t1.y - pad, t0.y + pad + fs + 6 + static_cast<float>(rows) * (rowH + 1) + 4);
    bevelBox(dl, ImVec2(gx0, t0.y + pad), ImVec2(gx1, gy1), true);
    dl->AddText(a.fontBold ? a.fontBold : a.fontRegular, fs, ImVec2(gx0 + 6, t0.y + pad + 3), P.text, i18n::tr(title));
  };
  const float rowY0 = t0.y + pad + fs + 8;
  auto rowY = [&](int r) { return rowY0 + static_cast<float>(r) * (rowH + 1); };
  // A setting on or off: a check row.
  auto checkSetting = [&](const char* id, const char* label, const char* key, float x0, float x1, int r, const char* hint) {
    const bool on = a.settings.getBool(key);
    if (checkRow(a, id, label, ImVec2(x0 + 4, rowY(r)), ImVec2(x1 - 4, rowY(r) + rowH), on, hint)) a.settings.set(key, !on);
  };
  if (gConfigPage == 0) {
    const float colW = std::floor((t1.x - t0.x - pad * 5) / 4);
    const float xa = t0.x + pad, xb = xa + colW + pad, xc = xb + colW + pad, xd = xc + colW + pad;
    group(xa, xa + colW, "Columns", 3);
    checkSetting("##cfgvol", "Volume column", "tracker.volumeColumn", xa, xa + colW, 0, "Each note's volume beside it");
    checkSetting("##cfgpan", "Pan column", "tracker.panColumn", xa, xa + colW, 1, "Each note's pan beside it");
    checkSetting("##cfgfx", "Effect column", "tracker.effectColumn", xa, xa + colW, 2, "Each note's effect letter and value");
    group(xb, xb + colW, "Numbers and names", 5);
    {
      const bool hex = a.settings.getString("tracker.rowBase") != "dec";
      if (checkRow(a, "##cfghex", "Hexadecimal rows", ImVec2(xb + 4, rowY(0)), ImVec2(xb + colW - 4, rowY(0) + rowH), hex,
                   "Row numbers in hexadecimal (00 to 3F), or decimal (00 to 63)"))
        a.settings.set("tracker.rowBase", std::string(hex ? "dec" : "hex"));
      const bool flats = a.settings.getBool("tracker.flats");
      if (checkRow(a, "##cfgflats", "Flats (Db, not C#)", ImVec2(xb + 4, rowY(1)), ImVec2(xb + colW - 4, rowY(1) + rowH), flats,
                   "Black keys named with flats (Db4) instead of sharps (C#4)"))
        a.settings.set("tracker.flats", !flats);
      checkSetting("##cfgdots", "Dots in empty fields", "tracker.emptyDots", xb, xb + colW, 2, "Empty fields drawn as faint dots, or left blank");
      checkSetting("##cfgnums", "Track numbers", "tracker.trackNumbers", xb, xb + colW, 3, "The track headers show each track's number before its name");
      const int base = trackBase(a);
      if (checkRow(a, "##cfgbase", "Count tracks from 1", ImVec2(xb + 4, rowY(4)), ImVec2(xb + colW - 4, rowY(4) + rowH), base == 1,
                   "The first track is number 1, as its instrument; off, number 0 (as older versions of the classic tracker counted)"))
        a.settings.set("tracker.trackBase", static_cast<double>(base == 0 ? 1 : 0));
    }
    group(xc, xc + colW, "Look", 4);
    checkSetting("##cfgtall", "Tall rows", "tracker.tallRows", xc, xc + colW, 0, "More room between the rows (fewer rows in view)");
    checkSetting("##cfgbeats", "Beat and bar rows lit", "tracker.beatHighlight", xc, xc + colW, 1, "Rows on a beat or a bar are lit, and so are their numbers");
    checkSetting("##cfgframes", "Lines between tracks", "tracker.trackFrames", xc, xc + colW, 2, "A line between each two tracks (off: only the headers are divided)");
    checkSetting("##cfgscope", "Scopes as lines", "tracker.scopeLines", xc, xc + colW, 3, "The scopes join each channel's wave into a line (off: points, as the classic scopes)");
    // The classic "maximum visible channels": radios, at most this many tracks share the width.
    static const int kVis[] = {0, 4, 6, 8, 12, 16};
    group(xd, t1.x - pad, "Tracks in view", 3);
    const int vis = static_cast<int>(a.settings.getNumber("tracker.visibleTracks"));
    const float half = std::floor((t1.x - pad - xd - 8) / 2);
    for (int i = 0; i < 6; ++i) {
      const float rx = xd + 4 + static_cast<float>(i / 3) * half, ry = rowY(i % 3);
      const std::string label = kVis[i] > 0 ? std::to_string(kVis[i]) : std::string(i18n::tr("All that fit"));
      ImGui::PushID(i);
      if (radioRow(a, "##cfgvis", label.c_str(), ImVec2(rx, ry), ImVec2(rx + half, ry + rowH), vis == kVis[i],
                   "At most this many tracks share the pattern's width (wider tracks; the others scroll)"))
        a.settings.set("tracker.visibleTracks", static_cast<double>(kVis[i]));
      ImGui::PopID();
    }
  } else if (gConfigPage == 2) {
    configAudio(a, t0, t1, pad, rowH, group, rowY);
  } else if (gConfigPage == 3) {
    configMidi(a, t0, t1, pad, rowH, group, rowY);
  } else {
    const float colW = std::floor((t1.x - t0.x - pad * 5) / 4);
    const float xa = t0.x + pad, xb = xa + colW + pad, xc = xb + colW + pad, xd = xc + colW + pad;
    group(xa, xa + colW, "Cursor", 3);
    if (checkRow(a, "##cfgfollow", "Follow playback", ImVec2(xa + 4, rowY(0)), ImVec2(xa + colW - 4, rowY(0) + rowH), follow(),
                 "The cursor follows the play row while playing"))
      setFollow(!follow());
    // R7-12: the steppers' values sit against their arrows (a compact row, not the column's width).
    const float sw = std::min(colW - 8, std::round(ImGui::GetFontSize() * 11.5f));
    if (const int d = stepper(a, "cfgstep", "Edit step", std::to_string(editStep()), ImVec2(xa + 4, rowY(1)), sw, rowH, "Rows the cursor moves after each note or value (Ctrl+0 to 9)", sw))
      setEditStep(editStep() + d);
    if (const int d = stepper(a, "cfgoct", "Octave", std::to_string(a.typing.octave), ImVec2(xa + 4, rowY(2)), sw, rowH, "The note keys' octave (F1 to F8)", sw))
      a.typing.octave = std::clamp(a.typing.octave + d, 0, 8);
    group(xb, xb + colW, "Several tracks", 4);
    checkSetting("##cfgmulti", "Multichannel edit", "tracker.multiEdit", xb, xb + colW, 0,
          "Each note typed moves the cursor to the next enabled track; notes held together land on one row (a scope's right-click turns its track on or off)");
    checkSetting("##cfgmrec", "Multichannel record", "tracker.multiRecord", xb, xb + colW, 1, "While recording, each note goes to the next enabled track");
    checkSetting("##cfgjazz", "Multichannel play", "tracker.multiJazz", xb, xb + colW, 2,
          "With edit mode off, the notes played sound on the enabled tracks in turn (a chord on several instruments)");
    checkSetting("##cfgkoff", "Record key releases", "tracker.recordKeyOff", xb, xb + colW, 3, "While recording, a key let go ends its note on the row playing");
    group(xc, xc + colW, "Editing", 3);
    checkSetting("##cfgcut", "Cut keeps a copy", "tracker.cutCopies", xc, xc + colW, 0, "A cut puts what it took in the clipboard; off, a cut only deletes");
    // R7-15: the classic "change pattern length when inserting or deleting a line" (by whole bars here).
    checkSetting("##cfginsdel", "Insert, delete: length", "tracker.insDelLength", xc, xc + colW, 1,
                 "Shift+Insert and Shift+Backspace (a line on every track) change the pattern's length: a bar more when a note would be pushed off the end, a bar less when the last bar is left empty");
    // cp15: the classic record quantize: off, or the rows a recorded note snaps to (1, 2, 4, 8, 16).
    {
      static const int kSnap[] = {0, 1, 2, 4, 8, 16};
      const int cur = static_cast<int>(a.settings.getNumber("tracker.recSnap"));
      int idx = 0;
      for (int i = 0; i < 6; ++i)
        if (kSnap[i] == cur) idx = i;
      const std::string val = cur <= 0 ? std::string(i18n::tr("off")) : std::to_string(cur) + " " + i18n::tr(cur == 1 ? "row" : "rows");
      const float sw2 = std::min(colW - 8, std::round(ImGui::GetFontSize() * 11.5f));
      if (const int d = stepper(a, "cfgsnap", "Record snap", val, ImVec2(xc + 4, rowY(2)), sw2, rowH,
                                "While recording, a note goes to the nearest row that is a multiple of this (1: the nearest row); off: the row playing", sw2))
        a.settings.set("tracker.recSnap", static_cast<double>(kSnap[std::clamp(idx + d, 0, 5)]));
    }
    group(xd, t1.x - pad, "Files screen", 3);
    {
      const bool byType = a.settings.getString("tracker.diskSort") == "type";
      dl->AddText(a.fontRegular, fs, ImVec2(xd + 8, std::round(rowY(0) + (rowH - fs) * 0.5f)), P.textDim, i18n::tr("Sort by"));
      for (int i = 0; i < 2; ++i) {
        const float ry = rowY(1 + i);
        ImGui::PushID(20 + i);
        if (radioRow(a, "##cfgsort", i == 0 ? "Name" : "Type, then name", ImVec2(xd + 4, ry), ImVec2(t1.x - pad - 4, ry + rowH), byType == (i == 1),
                     "The order of the Files list (folders always first)"))
          a.settings.set("tracker.diskSort", std::string(i == 0 ? "name" : "type"));
        ImGui::PopID();
      }
    }
  }
}

// About (refs 141, 160): the band swaps to a dark field with a starfield, the wordmark large in the
// upper half (the fixed brand orange), the version and credits centred under it, Exit at the bottom left.
void aboutScreen(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const ImU32 field = IM_COL32(11, 11, 11, 255);
  bevelBox(dl, p0, p1, true);
  dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), field);
  // The stars: fixed places (a small hash), drifting left slowly; brighter ones nearer.
  const float W = p1.x - p0.x - 4, H = p1.y - p0.y - 4;
  const double t = ImGui::GetTime();
  dl->PushClipRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), true);
  for (unsigned i = 0; i < 160; ++i) {
    unsigned h = i * 2654435761u;
    h ^= h >> 13;
    const float fx = static_cast<float>(h % 10007) / 10007.0f, fy = static_cast<float>((h >> 8) % 9973) / 9973.0f;
    const int depth = 1 + static_cast<int>(h % 3);
    const float x = p0.x + 2 + std::fmod(fx * W - static_cast<float>(t * 6.0 * depth) + W * 4, W);
    const float y = p0.y + 2 + fy * H;
    const int g = 70 + depth * 55;
    dl->AddRectFilled(ImVec2(std::round(x), std::round(y)), ImVec2(std::round(x) + 1, std::round(y) + 1), IM_COL32(g, g, g, 255));
  }
  dl->PopClipRect();
  // The wordmark: about half the band wide, centred in the upper half.
  // K-315: the chrome banner (ChromeBanner.h) over the upper part; a click on it shows the next shape.
  const float ly = std::round(p0.y + 4), lh = chromebanner::heightFor(H, fs);
  chromebanner::draw(dl, ImVec2(p0.x + 3, ly), ImVec2(p1.x - 3, ly + lh));
  float y = ly + lh + std::round(fs * 0.9f);
  auto centred = [&](ImFont* f, ImU32 col, const char* text) {
    const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0, text).x;
    dl->AddText(f, fs, ImVec2(std::round(p0.x + (p1.x - p0.x - tw) * 0.5f), y), col, text);
    y += fs + 3;
  };
  const std::string version = std::string(i18n::tr("Version")) + " " + GRIDLINE_VERSION;
  centred(a.fontBold ? a.fontBold : a.fontRegular, IM_COL32(237, 237, 237, 255), version.c_str());
  centred(a.fontRegular, IM_COL32(163, 163, 163, 255), i18n::tr("A free, open, pattern-based DAW for everybody."));
  centred(a.fontRegular, IM_COL32(112, 112, 112, 255), i18n::tr("Licences and credits: Help, About Gridline"));
  const float bh = std::round(fs + 9), bw = std::round(std::max(70.0f, fs * 5));
  if (bevelButton(a, "##aboutexit", "Exit", ImVec2(p0.x + 4, p1.y - 4 - bh), ImVec2(p0.x + 4 + bw, p1.y - 4), false, "Back to the header")) gSwap = Swap::None;
  (void)P;
}

// The band while the pattern is extended (the classic Extend): a short strip, the order list (the
// entry before, the current one, the one after) with its arrows, Ins and Del, the song's length and
// loop with Exit (the band back), the pattern and its length with Expand and Shrink, a compact
// instrument list, and under them a status row with the master volume and the position. The
// pattern page under it gets about 40 rows.
float extendedStripH() { return std::round(ImGui::GetFontSize() + 4) * 4 + 4 * 3 + 8 + 4; }

// The order list's edits from the band and the strip (one undo step each; refused with a reason).
void orderEdit(App& a, bool insert) {
  const std::vector<trk::OrderEntry> ol = trk::orderList(a.doc.song());
  const int cur = currentEntry(a, ol);
  const trk::OrderEdit r = insert ? trk::orderInsert(a.doc, cur < 0 ? static_cast<int>(ol.size()) : cur, a.curPattern) : trk::orderRemove(a.doc, cur);
  if (r == trk::OrderEdit::Spans) a.notify(i18n::tr("Its clips run into the next entries: change them on the Song page"));
  else if (r == trk::OrderEdit::OtherClips) a.notify(i18n::tr("Audio or automation clips start there: change them on the Song page"));
}
// Loop to: the entry the song goes back to at its end (the song loop marker, saved and shared).
void loopStepper(App& a, const std::vector<trk::OrderEntry>& ol, ImVec2 p, float w, float h, const char* label, float labelW) {
  const Song& s = a.doc.song();
  int loopAt = -1;
  Tick loopPos = 0;
  for (const Marker& m : s.markers)
    if (m.action == MarkerAction::SongLoop)
      for (size_t i = 0; i < ol.size() && loopAt < 0; ++i)
        if (m.pos < ol[i].end) loopAt = static_cast<int>(i), loopPos = m.pos;
  if (const int d = stepper(a, "xloop", label, loopAt < 0 ? std::string("--") : trk::hex2(loopAt), p, w, h, "The entry the song goes back to when it ends (none: it stops)",
                            labelW)) {
    const int to = loopAt + d;
    if (to < 0 && loopAt >= 0) ops::addSongLoopMarker(a.doc, loopPos);  // placed where it is: removed
    else if (to >= 0 && to < static_cast<int>(ol.size())) ops::addSongLoopMarker(a.doc, ol[static_cast<size_t>(to)].start);
  }
}

void extendedStrip(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const float W = av.x, H = std::max(fs + 8, av.y);
  const float pad = 4, rh = std::round(fs + 4), y0 = o.y + pad;
  const float stH = rh + 4, yE = o.y + H - stH;  // the boxes, then the status row
  auto rowY = [&](int r) { return std::round(y0 + static_cast<float>(r) * (rh + 4)); };
  const std::vector<trk::OrderEntry> ol = trk::orderList(s);
  const int cur = currentEntry(a, ol);
  float x = o.x;
  // Order box: three entries, the arrows that step the song position, Ins and Del.
  {
    const float aw = std::round(rh * 1.35f), iw = std::round(std::max(fs * 2.6f, 34.0f));
    const float bw = std::round(std::max(fs * 13.5f, 170.0f)) + iw + 2;  // R12-06: room for the pattern names
    bevelBox(dl, ImVec2(x, o.y), ImVec2(x + bw, yE));
    const ImVec2 l0(x + pad, y0), l1(x + bw - pad - aw - iw - 4, yE - pad);
    {
      const float ix = x + bw - pad - iw, ih = std::floor((l1.y - l0.y - 2) / 2);
      if (bevelButton(a, "##xins", "Ins", ImVec2(ix, l0.y), ImVec2(ix + iw, l0.y + ih), false, "Inserts the shown pattern at the current entry (the song after it moves right)"))
        orderEdit(a, true);
      if (cur < 0) ImGui::BeginDisabled();
      if (bevelButton(a, "##xdel", "Del", ImVec2(ix, l0.y + ih + 2), ImVec2(ix + iw, l1.y), false, "Removes the current entry (its clips; the song after it moves left)"))
        orderEdit(a, false);
      if (cur < 0) ImGui::EndDisabled();
    }
    bevelBox(dl, l0, l1, true);
    orderRows(a, ImVec2(l0.x + 1, l0.y + 1), ImVec2(l1.x - 1, l1.y - 1));
    int d = 0;
    const float ah = std::floor((l1.y - l0.y - 2) / 2);
    if (arrowButton(a, "##xposup", ImVec2(l1.x + 2, l0.y), ImVec2(l1.x + 2 + aw, l0.y + ah), true, "The entry before (the song position)")) d = -1;
    if (arrowButton(a, "##xposdn", ImVec2(l1.x + 2, l0.y + ah + 2), ImVec2(l1.x + 2 + aw, l1.y), false, "The next entry (the song position)")) d = 1;
    if (d && !ol.empty()) {
      const int to = std::clamp(std::max(0, cur) + d, 0, static_cast<int>(ol.size()) - 1);
      if (ol[static_cast<size_t>(to)].pattern != kNoId) a.selectPattern(ol[static_cast<size_t>(to)].pattern);
      setSongPosition(a, ol[static_cast<size_t>(to)].start);
    }
    x += bw;
  }
  // Song box: length, the loop, Exit.
  {
    const float bw = std::round(std::max(fs * 10.0f, 128.0f));
    bevelBox(dl, ImVec2(x, o.y), ImVec2(x + bw, yE));
    // Length's digits over the Loop to field's (the field against its arrows), as in the main band.
    const float sw = bw - pad * 2 - 4;
    const float arrows = 2 * std::max(std::round(rh * 0.8f), std::min(std::round(rh * 1.35f), std::floor((sw - loopNeed(a, "Loop to")) / 2)));
    readout(a, "Length", trk::hex2(static_cast<int>(ol.size())), ImVec2(x + pad + 2, rowY(0)), std::max(1.0f, sw - arrows - 6), rh, "Entries in the song");
    loopStepper(a, ol, ImVec2(x + pad + 2, rowY(1)), sw, rh, "Loop to", sw);
    if (bevelButton(a, "##xback", "Exit", ImVec2(x + pad, rowY(2)), ImVec2(x + bw - pad, std::min(yE - pad, rowY(2) + rh)), false,
                    "The header band back (Header band, Ctrl+Alt+Z; the pattern keeps its place)"))
      setExtended(a, false);
    x += bw;
  }
  // Pattern box: the pattern, its rows, Expand and Shrink.
  {
    const float bw = std::round(std::max(fs * 11.0f, 140.0f));
    bevelBox(dl, ImVec2(x, o.y), ImVec2(x + bw, yE));
    const Pattern* p = s.pattern(a.curPattern);
    trk::Grid g = p ? trk::gridOf(s, *p) : trk::Grid{};
    const float rw = bw - pad * 2, lw = std::round(rw * 0.36f);
    if (const int d = stepper(a, "xpat", "Pattern", p ? trk::hex2(trk::patternNumber(s, p->id)) : std::string("--"), ImVec2(x + pad, rowY(0)), rw, rh,
                              "The pattern shown (past the last one: a new, empty pattern; the piano roll and steps show it too)", lw))
    {
      // R11-01: a step past the last pattern adds one, and the pattern list may move: the pattern
      // and its grid are looked up again before the Rows field reads them.
      stepPattern(a, d);  // R10-03
      p = s.pattern(a.curPattern);
      g = p ? trk::gridOf(s, *p) : trk::Grid{};
    }
    if (const int d = stepper(a, "xlen", "Rows", p ? trk::rowText(trk::playedRows(a.doc.song(), *p, g), true) : std::string("--"), ImVec2(x + pad, rowY(1)), rw, rh,
                              "The pattern's length in rows (a bar at a time)", lw))
      stepRows(a, p, d);
    const float hw = std::floor((rw - 2) / 2), yb = rowY(2), yb1 = std::min(yE - pad, yb + rh);
    if (bevelButton(a, "##xexp", "Expand", ImVec2(x + pad, yb), ImVec2(x + pad + hw, yb1), false, "Notes twice as far apart: the block, or the whole pattern (made twice as long)"))
      scaleBlock(a, true);
    if (bevelButton(a, "##xshr", "Shrink", ImVec2(x + pad + hw + 2, yb), ImVec2(x + bw - pad, yb1), false, "Notes twice as close: the block, or the whole pattern (made half as long)")) scaleBlock(a, false);
    x += bw;
  }
  // Instrument list, compact: the page of six around the selected channel, in two columns.
  {
    const float x1 = o.x + W;
    bevelBox(dl, ImVec2(x, o.y), ImVec2(x1, yE));
    const ImVec2 l0(x + pad, y0), l1(x1 - pad, yE - pad);
    if (l1.x - l0.x > fs * 6) {
      bevelBox(dl, l0, l1, true);
      const int ci = s.channelIndex(a.selChannel);
      const int cols = l1.x - l0.x > fs * 24 ? 2 : 1, per = 3 * cols;
      // The page follows the selection; the wheel over the list steps a page (the instruments past
      // the first six are reached without the band).
      static int page = 0, lastCi = -2;
      const int pages = std::max(1, (static_cast<int>(s.channels.size()) + per - 1) / per);
      if (ci != lastCi) page = std::max(0, ci) / per, lastCi = ci;
      if (ImGui::IsMouseHoveringRect(l0, l1) && ImGui::GetIO().MouseWheel != 0) page += ImGui::GetIO().MouseWheel > 0 ? -1 : 1;
      page = std::clamp(page, 0, pages - 1);
      const float cw = std::floor((l1.x - l0.x) / static_cast<float>(cols));
      const float lrh = std::floor((l1.y - l0.y - 2) / 3);
      dl->PushClipRect(l0, l1, true);
      for (int k = 0; k < per; ++k) {
        const int i = page * per + k;
        const float cx = l0.x + static_cast<float>(k / 3) * cw, cy = l0.y + 1 + static_cast<float>(k % 3) * lrh;
        const float ty = std::round(cy + (lrh - fs) * 0.5f);
        const std::string num = trk::hex2(i + 1);
        if (i >= static_cast<int>(s.channels.size())) {
          dl->AddText(mono, fs, ImVec2(cx + 9, ty), theme::withAlpha(P.textFaint, 0.6f), num.c_str());
          continue;
        }
        const Channel& c = s.channels[static_cast<size_t>(i)];
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::PushID(3000 + i);
        const bool click = ImGui::InvisibleButton("##xins", ImVec2(std::max(1.0f, cw - 1), lrh));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ui::Hint("Click: select (new notes use it) \xC2\xB7 wheel: the next or previous six");
        if (click) {
          a.selectChannel(c.id);
          cursorToChannel(a, c.id);
        }
        if (i == ci) dl->AddRectFilled(ImVec2(cx + 1, cy), ImVec2(cx + cw - 1, cy + lrh), theme::withAlpha(P.surface600, 0.95f));
        else if (hov) dl->AddRectFilled(ImVec2(cx + 1, cy), ImVec2(cx + cw - 1, cy + lrh), theme::withAlpha(P.surface700, 0.6f));
        dl->AddRectFilled(ImVec2(cx + 2, cy + 2), ImVec2(cx + 5, cy + lrh - 2), theme::rgb(c.color));
        dl->AddText(mono, fs, ImVec2(cx + 9, ty), i == ci ? valueInk() : P.textDim, num.c_str());
        const float nx = cx + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
        theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), cx + cw - nx - 4, i == ci ? P.text : c.muted ? P.textFaint : P.textDim, c.name.c_str());
      }
      dl->PopClipRect();
    }
  }
  // The status row: the master volume at the left, the position at the right (as the band's strip).
  {
    bevelBox(dl, ImVec2(o.x, yE), ImVec2(o.x + W, o.y + H));
    const float ty = std::round(yE + (stH - fs) * 0.5f);
    float sx = o.x + 8;
    if (!s.mixer.empty()) {
      const double vol = getParam(s, mixerParam(s.masterId(), MixerParam::Volume)).value_or(0.8);
      char mv[16];
      std::snprintf(mv, sizeof mv, "%d%%", static_cast<int>(std::lround(vol * 100)));
      const char* l = i18n::tr("Master");
      dl->AddText(a.fontRegular, fs, ImVec2(sx, ty), P.textDim, l);
      sx += a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, l).x + 5;
      dl->AddText(mono, fs, ImVec2(sx, ty), valueInk(), mv);
    }
    const std::string pos = menuparity::positionText(a, a.engine.playing() ? a.engine.position() : static_cast<double>(songPosition(a)));
    const char* l = i18n::tr("Position");
    const float pw = mono->CalcTextSizeA(fs, FLT_MAX, 0, pos.c_str()).x, lw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, l).x + 5;
    dl->AddText(a.fontRegular, fs, ImVec2(std::round(o.x + W - 8 - pw - lw), ty), P.textDim, l);
    dl->AddText(mono, fs, ImVec2(std::round(o.x + W - 8 - pw), ty), valueInk(), pos.c_str());
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + av.y));
  ImGui::Dummy(ImVec2(1, 0));
}

void pageStrip(App& a, ImVec2 o, float W, float py0, float stripH);
extern ObjectId gSourceChannel;
void instrumentMenu(App& a, ObjectId id);

void header(App& a) {
  const auto& P = pal();
  if (gSwap == Swap::Help || gSwap == Swap::Config || gSwap == Swap::About) {
    const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    const float stripH = std::round(ImGui::GetFontSize() + 9);
    const float H = std::max(120.0f, std::floor(av.y - stripH - 2));
    if (gSwap == Swap::Help) helpScreen(a, o, ImVec2(o.x + av.x, o.y + H));
    else if (gSwap == Swap::About) aboutScreen(a, o, ImVec2(o.x + av.x, o.y + H));
    else configScreen(a, o, ImVec2(o.x + av.x, o.y + H));
    pageStrip(a, o, av.x, o.y + H + 2, stripH);
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + av.y));
    ImGui::Dummy(ImVec2(1, 0));
    return;
  }
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const float stripH = std::round(fs + 9);
  const float H = std::max(120.0f, std::floor(av.y - stripH - 2));
  const float W = av.x;
  const float k = H / 173.0f;  // the classic band's proportions at this height
  // Columns: the button columns and bank strip keep their scaled width; the left block and the
  // lists share what is left.
  // A button column is as wide as its longest label needs (translated), never cut.
  float labelW = 0;
  for (const char* l : {"About Gridline", "Metronome", "New", "Clean up", "Header band", "Transpose", "MIDI", "Sample tools", "Advanced edit", "Play song",
                        "Play pattern", "Stop", "Record song", "Record pattern", "Files", "Channel settings", "Audio editor", "Instrument", "Sample", "Settings", "Tracker help"})
    labelW = std::max(labelW, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(l)).x);
  const float colW = std::max(std::round(65 * k), std::ceil(labelW + 16)), bankW = std::round(45 * k);
  const float rest = std::max(200.0f, W - colW * 2 - bankW);
  const float extra = std::max(0.0f, rest - (291 + 166) * k);
  // T-22: a wide window gives the boxes at most three quarters more than their classic width (wider, their
  // labels, values and arrows drift apart); the instrument list takes the rest, in columns.
  const float leftW = std::floor(std::min({rest * 0.64f, 291 * k + extra * 0.5f, 291 * k * 1.75f}));
  const float listW = std::floor(W - leftW - colW * 2 - bankW);
  const float xL = o.x, xA = o.x + leftW, xB = xA + colW, xI = xB + colW, xK = xI + listW;
  const float y0 = o.y;
  const float stH = std::max(std::round(15 * k), std::round(fs + 4));  // the strip never shorter than its text
  const float pad = std::round(4 * k);
  // The smallest windows (960x600, 800x500): the song and pattern boxes keep three whole rows under
  // the wordmark's thinnest line (K-297's handoff: Step and Expand ran into the Master strip); the
  // channel boxes under the strip give up the height.
  const float topMin = std::min(std::round(fs + 8), std::round(32 * k)) + 3 * std::round(fs + 3) + 4 + pad * 2;
  const float rowH = std::max(14.0f, std::round(11 * k));
  // cp14: the order list's five entries in the band's text size (the list box, its Len and Loop row,
  // the box's margins), so its text is never smaller than the labels beside it.
  const float orderNeed = pad * 2 + 4 + 5 * std::round(fs + 1) + 2 + 2 + rowH;
  const float topH = std::min(std::max({std::round(77 * k), topMin, orderNeed}), std::floor(H - stH - 2 * std::round(fs + 4)));
  const float yS = y0 + topH, yC = yS + stH, yE = y0 + H;

  // ---- button columns: tools (A) and transport and screens (B), ten rows each ----
  const float bh10 = std::floor((H - 4 - 9) / 10.0f);
  auto colBox = [&](float x) { bevelBox(dl, ImVec2(x, y0), ImVec2(x + colW, yE)); };
  auto slot = [&](int i, float* y0o, float* y1o) {
    *y0o = std::round(y0 + 2 + static_cast<float>(i) * (bh10 + 1));
    *y1o = i == 9 ? yE - 2 : *y0o + bh10;
  };
  // Files: the header's left part (the boxes and the tool column) shows the files instead.
  if (gSwap == Swap::Disk) {
    diskScreen(a, ImVec2(xL, y0), ImVec2(xB, yE));
  } else {
  // ---- order box ----
  const float orderW = std::round(leftW * 112.0f / 291.0f);
  {
    bevelBox(dl, ImVec2(xL, y0), ImVec2(xL + orderW, yS));
    const float bw = std::round(orderW * 0.30f);
    const float lx1 = xL + orderW - pad - bw - 2;
    // T-21: a short band (a small window) would leave the list a row or two: Length and Loop share
    // one row there, so the list keeps the entry before and after the current one in view.
    // R10-05: Length and Loop always share that row, so the list has room for five entries.
    const bool oneRow = true;
    const float listB = yS - pad - rowH * (oneRow ? 1 : 2) - 2;
    // R10-04: the song position's arrows at the list's right edge (up at the top, down at the
    // bottom), as the classic position editor has them; Ctrl+Shift+Left and Right do the same.
    const float paw = std::round(rowH * 1.1f);
    const float lxs = lx1 - paw - 1;
    bevelBox(dl, ImVec2(xL + pad, y0 + pad), ImVec2(lxs, listB), true);
    orderRows(a, ImVec2(xL + pad + 1, y0 + pad + 1), ImVec2(lxs - 1, listB - 1));
    {
      const float my = std::floor((y0 + pad + listB) * 0.5f);
      if (arrowButton(a, "##posup", ImVec2(lxs + 1, y0 + pad), ImVec2(lx1, my), true, "The previous song position (Ctrl+Shift+Left)")) stepSongPosition(a, -1);
      if (arrowButton(a, "##posdn", ImVec2(lxs + 1, my + 1), ImVec2(lx1, listB), false, "The next song position (Ctrl+Shift+Right)")) stepSongPosition(a, 1);
    }
    // Insert the shown pattern at the current entry, or remove the current entry (playlist edits).
    const float bx = lx1 + 2, bh = std::floor((listB - y0 - pad - 4) / 3);
    const std::vector<trk::OrderEntry> ol = trk::orderList(s);
    const int cur = currentEntry(a, ol);
    auto told = [&](trk::OrderEdit r) {
      if (r == trk::OrderEdit::Spans) a.notify(i18n::tr("Its clips run into the next entries: change them on the Song page"));
      else if (r == trk::OrderEdit::OtherClips) a.notify(i18n::tr("Audio or automation clips start there: change them on the Song page"));
    };
    const float r1 = y0 + pad + bh, r2 = r1 + 2 + bh;
    if (bevelButton(a, "##oins", "Ins", ImVec2(bx, y0 + pad), ImVec2(bx + bw, r1), false,
                    "Inserts the shown pattern at the current entry (the song after it moves right)"))
      told(trk::orderInsert(a.doc, cur < 0 ? static_cast<int>(ol.size()) : cur, a.curPattern));
    if (cur < 0) ImGui::BeginDisabled();
    // The current entry's pattern, one up or down (the entry plays the next or previous pattern).
    const float hw = std::floor((bw - 1) / 2);
    int dp = 0;
    if (arrowButton(a, "##opup", ImVec2(bx, r1 + 2), ImVec2(bx + hw, r2), true, "The current entry plays the next pattern")) dp = 1;
    if (arrowButton(a, "##opdn", ImVec2(bx + hw + 1, r1 + 2), ImVec2(bx + bw, r2), false, "The current entry plays the previous pattern")) dp = -1;
    if (dp && cur >= 0 && ol[static_cast<size_t>(cur)].pattern == kNoId) {
      told(trk::OrderEdit::OtherClips);  // an audio part
    } else if (dp && cur >= 0) {
      const int pi = s.patternIndex(ol[static_cast<size_t>(cur)].pattern) + dp;
      if (pi >= 0 && pi < static_cast<int>(s.patterns.size())) {
        const ObjectId to = s.patterns[static_cast<size_t>(pi)].id;
        const trk::OrderEdit r = trk::orderSetPattern(a.doc, cur, to);
        told(r);
        if (r == trk::OrderEdit::Done) a.curPattern = to;  // R12-03: the editor shows what the current entry plays
      }
    }
    if (bevelButton(a, "##odel", "Del", ImVec2(bx, r2 + 2), ImVec2(bx + bw, listB), false,
                    "Removes the current entry (its clips; the song after it moves left)"))
      told(trk::orderRemove(a.doc, cur));
    if (cur < 0) ImGui::EndDisabled();
    const char* lenHint = "Entries in the song (from the playlist's pattern clips; AU: a part where only audio plays)";
    float loopX = xL + pad, loopW = orderW - pad * 2, loopY = listB + 2 + rowH;
    if (oneRow) {  // "Len 06" then the Loop stepper on the same row
      const std::string len = trk::hex2(static_cast<int>(ol.size()));
      const float lenW = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Len")).x + mono->CalcTextSizeA(fs, FLT_MAX, 0, len.c_str()).x) + 6;
      readout(a, "Len", len, ImVec2(xL + pad, listB + 2), lenW, rowH, lenHint);
      loopX += lenW + 6, loopW -= lenW + 6, loopY = listB + 2;
    } else {
      // The length's digits stand over the Loop to field's (the field sits against its arrows), so the
      // box's two numbers read as one column (before: Length at the box's edge above the arrows, the
      // loop field in mid-row).
      const float arrows = 2 * std::max(std::round(rowH * 0.8f), std::min(std::round(rowH * 1.35f), std::floor((loopW - loopNeed(a, "Loop to")) / 2)));
      readout(a, "Length", trk::hex2(static_cast<int>(ol.size())), ImVec2(xL + pad, listB + 2), std::max(1.0f, loopW - arrows - 6), rowH, lenHint);
    }
    // Loop to: the entry the song goes back to at its end (the song loop marker, saved and shared).
    int loopAt = -1;
    Tick loopPos = 0;
    for (const Marker& m : s.markers)
      if (m.action == MarkerAction::SongLoop)
        for (size_t i = 0; i < ol.size() && loopAt < 0; ++i)
          if (m.pos < ol[i].end) loopAt = static_cast<int>(i), loopPos = m.pos;
    if (const int d = stepper(a, "loop", oneRow ? "Loop" : "Loop to", loopAt < 0 ? std::string("--") : trk::hex2(loopAt), ImVec2(loopX, loopY),
                              loopW, rowH, "The entry the song goes back to when it ends (none: it stops)", oneRow ? 0.0f : loopW)) {
      const int to = loopAt + d;
      if (to < 0 && loopAt >= 0) ops::addSongLoopMarker(a.doc, loopPos);  // placed where it is: removed
      else if (to >= 0 && to < static_cast<int>(ol.size())) ops::addSongLoopMarker(a.doc, ol[static_cast<size_t>(to)].start);
    }
  }
  // ---- logo slot, song box, pattern box ----
  const float xs = xL + orderW, midW = leftW - orderW;
  // The wordmark gives up height first when the band is short: the song and pattern boxes keep
  // three rows of text (a 1.5x or 2x interface in a small window).
  const float boxNeed = 3 * std::round(fs + 4) + 4 + pad * 2;
  const float logoH = std::round(std::clamp(topH - boxNeed, std::min(fs + 8, 32 * k), 32 * k));
  {
    bevelBox(dl, ImVec2(xs, y0), ImVec2(xs + midW, y0 + logoH));
    logoLine(a, dl, ImVec2(xs, y0), ImVec2(xs + midW, y0 + logoH));
    ImGui::SetCursorScreenPos(ImVec2(xs, y0));
    // R5-04: only the mark at the left is the home button (a stray click on the wordmark, the biggest
    // label in the band, stays in the project; the wordmark is decoration, as the classic logo).
    if (ImGui::InvisibleButton("##logo", ImVec2(logoH, logoH))) a.home.showHome(a.now);
    ui::Hint("Home: recent projects and templates");
    ImGui::SetCursorScreenPos(ImVec2(xs + logoH, y0));  // K-315: the wordmark opens About
    if (ImGui::InvisibleButton("##wordmark", ImVec2(std::max(1.0f, midW - logoH), logoH))) gSwap = Swap::About;
    ui::Hint("About Gridline");
  }
  const float songW = std::round(midW * 94.0f / 179.0f), patW = midW - songW;
  const float by0 = y0 + logoH, bh = yS - by0;
  const float srh = std::max(14.0f, std::floor((bh - pad * 2 - 4) / 3));
  {
    bevelBox(dl, ImVec2(xs, by0), ImVec2(xs + songW, yS));
    const float rw = songW - pad * 2, lw = std::round(rw * 0.30f);
    float y = by0 + pad;
    char bpm[16];
    const bool whole = std::fabs(s.tempo - std::round(s.tempo)) < 0.05;
    std::snprintf(bpm, sizeof bpm, whole ? "%.0f" : "%.1f", s.tempo);
    double typedBpm = std::nan("");
    if (const int d = stepper(a, "bpm", "Tempo", bpm, ImVec2(xs + pad, y), rw, srh,
                              "The song tempo in beats per minute: click it to type one (arrows: steps of 1, Shift: 0.1)", lw, false, &typedBpm)) {
      const double t = ImGui::GetIO().KeyShift ? std::round(s.tempo * 10 + d) / 10.0 : std::round(s.tempo) + d;
      a.doc.setParam(songParam(SongParam::Tempo), std::clamp(t, 10.0, 522.0), true);
    }
    if (!std::isnan(typedBpm) && std::fabs(std::clamp(typedBpm, 10.0, 522.0) - s.tempo) > 1e-6)
      a.doc.setParam(songParam(SongParam::Tempo), std::clamp(std::round(typedBpm * 10) / 10.0, 10.0, 522.0), true);
    y += srh + 2;
    const Pattern* p = s.pattern(a.curPattern);
    const trk::Grid g = p ? trk::gridOf(s, *p) : trk::Grid{};
    // cp13: its digits stand in the Tempo and Step fields' column (before: at the box's edge, under
    // their arrows), as the Length over the Loop to field.
    // R11-10: the digits end where the Tempo field's digits end, worked out as stepper() places that
    // field (its arrows' width from the Tempo row, the field from the label column; before, the
    // arrows' left edge was used, which is right of a field narrower than its row).
    float digitsEnd;
    {
      ImFont* mono = monoFont(a);
      const float needT = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Tempo")).x + mono->CalcTextSizeA(fs, FLT_MAX, 0, bpm).x + 16;
      const float bwT = std::max(std::round(srh * 0.8f), std::min(std::round(srh * 1.35f), std::floor((rw - needT) / 2)));
      const float x1 = xs + pad + rw - 2 * bwT;
      const float vw = std::round(mono->CalcTextSizeA(fs, FLT_MAX, 0, "000.0").x + 8);
      digitsEnd = std::min(xs + pad + lw, x1 - 2 - vw) + vw - 4;
    }
    readout(a, "Rows/beat", std::to_string(g.rowsPerBeat), ImVec2(xs + pad, y), std::max(1.0f, digitsEnd - (xs + pad)), srh, "Rows per beat: the pattern's steps per beat");
    y += srh + 2;
    if (const int d = stepper(a, "step", "Step", std::to_string(editStep()), ImVec2(xs + pad, y), rw, srh, "Rows the cursor moves after each note (Ctrl+0 to 9)", lw))
      setEditStep(editStep() + d);
  }
  {
    const float xp = xs + songW;
    bevelBox(dl, ImVec2(xp, by0), ImVec2(xp + patW, yS));
    const float rw = patW - pad * 2, lw = std::round(rw * 0.30f);
    float y = by0 + pad;
    const Pattern* p = s.pattern(a.curPattern);
    trk::Grid g = p ? trk::gridOf(s, *p) : trk::Grid{};
    if (const int d = stepper(a, "pat", "Pattern", p ? trk::hex2(trk::patternNumber(s, p->id)) : std::string("--"), ImVec2(xp + pad, y), rw, srh,
                              "The pattern shown (past the last one: a new, empty pattern; the piano roll and steps show it too)", lw))
    {
      // R11-01: a step past the last pattern adds one, and the pattern list may move: the pattern
      // and its grid are looked up again before the Rows field reads them.
      stepPattern(a, d);  // R10-03
      p = s.pattern(a.curPattern);
      g = p ? trk::gridOf(s, *p) : trk::Grid{};
    }
    y += srh + 2;
    if (const int d = stepper(a, "len", "Rows", p ? trk::rowText(trk::playedRows(s, *p, g), true) : std::string("--"), ImVec2(xp + pad, y), rw, srh,
                              "The pattern's length in rows (a bar at a time)", lw))
      stepRows(a, p, d);
    y += srh + 2;
    const float hw = std::floor((rw - 2) / 2);
    if (bevelButton(a, "##exp", "Expand", ImVec2(xp + pad, y), ImVec2(xp + pad + hw, y + srh), false, "Notes twice as far apart: the block, or the whole pattern (made twice as long)"))
      scaleBlock(a, true);
    if (bevelButton(a, "##shr", "Shrink", ImVec2(xp + pad + hw + 2, y), ImVec2(xp + patW - pad, y + srh), false, "Notes twice as close: the block, or the whole pattern (made half as long)"))
      scaleBlock(a, false);
  }
  // ---- status strip: master level, octave, time (displays) ----
  {
    bevelBox(dl, ImVec2(xL, yS), ImVec2(xA, yC));
    const float ty = std::round(yS + (stH - fs) * 0.5f);
    // A display: the label, the value; the wheel steps it, a click acts. Returns the wheel step.
    // `right` draws it ending at x instead of starting there.
    float x = xL + pad + 2;
    auto display = [&](const char* id, const char* label, const std::string& value, const char* hint, bool* clicked, bool right = false) {
      const char* l = i18n::tr(label);
      const float lw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, l).x + 5;
      const float w = lw + mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x;
      const float x0 = std::round(right ? x - w : x);
      ImGui::SetCursorScreenPos(ImVec2(x0 - 2, yS + 1));
      const bool c = ImGui::InvisibleButton(id, ImVec2(w + 4, std::max(1.0f, stH - 2)));
      const bool hov = ImGui::IsItemHovered();
      if (hov) dl->AddRectFilled(ImVec2(x0 - 2, yS + 2), ImVec2(x0 + w + 2, yC - 2), theme::withAlpha(P.surface600, 0.5f));
      dl->AddText(a.fontRegular, fs, ImVec2(x0, ty), P.textDim, l);
      dl->AddText(mono, fs, ImVec2(x0 + lw, ty), valueInk(), value.c_str());
      ui::Hint(hint);
      if (clicked) *clicked = c;
      x = right ? x0 - 14 : x0 + w + 14;
      return hov && ImGui::GetIO().MouseWheel != 0 ? (ImGui::GetIO().MouseWheel > 0 ? 1 : -1) : 0;
    };
    bool click = false;
    if (!s.mixer.empty()) {
      const ParamId volId = mixerParam(s.masterId(), MixerParam::Volume);
      const double vol = getParam(s, volId).value_or(0.8);
      char mv[16];
      std::snprintf(mv, sizeof mv, "%d%%", static_cast<int>(std::lround(vol * 100)));
      // A display: the master fader lives on the Mix page (one control each).
      display("##master", "Master", mv, "The master volume (click: the Mix page, where its fader is)", &click);
      if (click) showPage(a, "Mixer");
    }
    {
      const ParamId pit = songParam(SongParam::MasterPitch);
      const double v = getParam(s, pit).value_or(0.0);
      char txt[16];
      std::snprintf(txt, sizeof txt, "%+d", static_cast<int>(std::lround(v)));
      if (const int d = display("##pitch", "Pitch", txt, "The master pitch in semitones (wheel: a semitone \xC2\xB7 click: back to 0)", &click))
        a.doc.setParam(pit, std::clamp(std::round(v) + d, -12.0, 12.0), true);
      if (click && std::lround(v) != 0) a.doc.setParam(pit, 0.0, false);
    }
    display("##oct", "Octave", std::to_string(a.typing.octave), "The octave the note keys type (F1 to F8 set it)", nullptr);
    // The song position at the right end (right-click: the time format, as the position display elsewhere).
    x = xA - pad - 2;
    const std::string pos = menuparity::positionText(a, a.engine.playing() ? a.engine.position() : static_cast<double>(songPosition(a)));
    display("##pos", "Position", pos, "The song position (right-click: its format)", nullptr, true);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##posfmt");
    if (ImGui::BeginPopup("##posfmt")) {
      menuparity::timeFormatMenu(a);
      ImGui::EndPopup();
    }
  }
  // ---- scope box (or the tool swapped in) ----
  {
    bevelBox(dl, ImVec2(xL, yC), ImVec2(xA, yE));
    const ImVec2 i0(xL + 3, yC + 3), i1(xA - 3, yE - 3);
    if (gTool == ScopeTool::Transpose) transposeTool(a, i0, i1);
    else if (gTool == ScopeTool::Trim) trimTool(a, i0, i1);
    else if (gTool == ScopeTool::AdvEdit) advEditTool(a, i0, i1);
    else if (gTool == ScopeTool::Midi) midiTool(a, i0, i1);
    else if (gTool == ScopeTool::Sample) sampleTool(a, i0, i1);
    else scopes(a, i0, i1);
  }
  colBox(xA);
  {
    const float bx0 = xA + 2, bx1 = xA + colW - 2;
    auto b = [&](int i, const char* id, const char* label, const char* hint, bool on = false) {
      float t0, t1;
      slot(i, &t0, &t1);
      return bevelButton(a, id, label, ImVec2(bx0, t0), ImVec2(bx1, t1), on, hint);
    };
    // Labels as the menus word the same command (Help > About Gridline, Options > Metronome, File > New).
    if (b(0, "##about", "About Gridline", "The version and the credits (the header shows them)", gSwap == Swap::About)) gSwap = Swap::About;
    if (b(1, "##metro", "Metronome", "The click while playing and recording", a.metronome)) a.metronome = !a.metronome;
    if (b(2, "##new", "New", "A new, empty project (asks to save first)")) a.guardUnsaved([&a]() { a.newProject(); });
    if (b(3, "##clean", "Clean up", "Removes unused patterns, tracks, mixer tracks and automation clips; works out the size saved (the scope box shows it; Ctrl+Alt+R)",
          gTool == ScopeTool::Trim))
      gTool = gTool == ScopeTool::Trim ? ScopeTool::Scopes : ScopeTool::Trim;
    if (b(4, "##ext", "Header band", "The pattern gets the room: this band becomes a strip and about 40 rows show (its Exit brings the band back)")) setExtended(a, true);
    if (b(5, "##trans", "Transpose", "Transpose tools in the scope box (Ctrl+Alt+T)", gTool == ScopeTool::Transpose))
      gTool = gTool == ScopeTool::Transpose ? ScopeTool::Scopes : ScopeTool::Transpose;
    // The classic editors' extensions: the instrument's MIDI and the sample range tools.
    if (b(6, "##iext", "MIDI", "The current instrument's MIDI out and the MIDI input in the scope box (Ctrl+Alt+J)", gTool == ScopeTool::Midi))
      gTool = gTool == ScopeTool::Midi ? ScopeTool::Scopes : ScopeTool::Midi;
    if (b(7, "##sext", "Sample tools", "Range edits on the current instrument's sample in the scope box (Ctrl+Alt+K)", gTool == ScopeTool::Sample))
      gTool = gTool == ScopeTool::Sample ? ScopeTool::Scopes : ScopeTool::Sample;
    if (b(8, "##adv", "Advanced edit", "Instrument remap and the clipboard masks in the scope box (Ctrl+Alt+A)", gTool == ScopeTool::AdvEdit))
      gTool = gTool == ScopeTool::AdvEdit ? ScopeTool::Scopes : ScopeTool::AdvEdit;
    // The last row: Add and Remove side by side (the classic Add / Sub, the track count).
    float t0, t1;
    slot(9, &t0, &t1);
    const float hw = std::floor((bx1 - bx0 - 2) / 2);
    if (bevelButton(a, "##addtr", "Add", ImVec2(bx0, t0), ImVec2(bx0 + hw, t1), false, "Adds a track at the end (a sampler channel)")) {
      // At the end, as the classic tracker adds channels (the numbers of the others stay; one undo step).
      a.doc.beginGroup("add track");
      // R5-05: the track is added and the screen stays as it was (its editor does not open; Channel
      // settings opens it).
      const ObjectId id = a.addInstrument("gl.tracker");
      if (id) a.doc.moveChannel(id, static_cast<int>(a.doc.song().channels.size()) - 1);
      a.doc.endGroup(false);
      if (id) a.selectChannel(id);
    }
    const Channel* last = s.channels.empty() ? nullptr : &s.channels.back();
    bool lastEmpty = last != nullptr;
    for (const Pattern& p : s.patterns) {
      if (!last) break;
      if (p.notes.count(last->id) && !p.notes.at(last->id).empty()) lastEmpty = false;
      if (p.steps.count(last->id))
        for (uint8_t v : p.steps.at(last->id)) lastEmpty = lastEmpty && v == 0;
    }
    if (!lastEmpty) ImGui::BeginDisabled();
    if (bevelButton(a, "##subtr", "Remove", ImVec2(bx0 + hw + 2, t0), ImVec2(bx1, t1), false, "Removes the last track while it has no notes or steps") && last) {
      const ObjectId gone = last->id;
      if (a.settingsChannel == gone) a.settingsChannel = kNoId;  // its settings close with it (no other channel's open instead)
      a.doc.removeChannel(gone);
    }
    if (!lastEmpty) ImGui::EndDisabled();
  }
  }  // not the files
  colBox(xB);
  {
    const float bx0 = xB + 2, bx1 = xB + colW - 2;
    auto b = [&](int i, const char* id, const char* label, const char* hint, bool on = false) {
      float t0, t1;
      slot(i, &t0, &t1);
      return bevelButton(a, id, label, ImVec2(bx0, t0), ImVec2(bx1, t1), on, hint);
    };
    const bool playing = a.engine.playing();
    auto start = [&](PlayMode m, bool rec) {
      if (m == PlayMode::Song) songMode(a);  // from the song position (kept by a pattern play)
      else a.engine.setMode(m);
      if (m == PlayMode::Pattern) a.engine.setPattern(a.curPattern);
      a.recordArmed = rec;
      if (!playing) menuparity::playPressed(a);
    };
    if (b(0, "##psong", "Play song", "Plays the song from the position", playing && !a.recordArmed && a.engine.mode() == PlayMode::Song)) start(PlayMode::Song, false);
    if (b(1, "##ppat", "Play pattern", "Plays the pattern shown, looped", playing && !a.recordArmed && a.engine.mode() == PlayMode::Pattern)) start(PlayMode::Pattern, false);
    if (b(2, "##stop", "Stop", "Stops (again: back to the start)")) menuparity::stopPressed(a);
    if (b(3, "##rsong", "Record song", "Plays the song and records what is played", playing && a.recordArmed && a.engine.mode() == PlayMode::Song)) start(PlayMode::Song, true);
    if (b(4, "##rpat", "Record pattern", "Loops the pattern and records what is played", playing && a.recordArmed && a.engine.mode() == PlayMode::Pattern))
      start(PlayMode::Pattern, true);
    if (b(5, "##files", "Files", "Files: save, open, recent files and the project folder (the header's left part shows them)", gSwap == Swap::Disk))
      gSwap = gSwap == Swap::Disk ? Swap::None : Swap::Disk;
    // cp18: Instrument lights while the instrument's screen shows (a Tracker Sampler's Instrument
    // editor, any other instrument's Channel frame); Sample while the Sample editor shows.
    const bool instOn = trackereditors::showing(a, trackereditors::Editor::Instrument) || (instrumentShown(a) && !trackereditors::shown(a));
    if (b(6, "##inst", "Instrument", "The current instrument's editor under the header (Ctrl+Alt+I; again: closes it)", instOn)) {
      if (instOn) closeInstrument(a);
      else if (a.selChannel != kNoId) trackereditors::open(a, trackereditors::Editor::Instrument, a.selChannel);
    }
    // R4-02: the button toggles the audio editor's page in place, as the classic screen buttons do:
    // shown, a click brings the pattern back (the editor stays docked behind it, never floated).
    // cp18: a Tracker Sampler's Sample editor is in the lower band instead; a Sampler's audio is the
    // audio editor's page; another instrument has no sample to edit.
    const Channel* selC = s.channel(a.selChannel);
    const bool trackerSel = trackereditors::hasSampleEditor(a, a.selChannel);
    const bool samplerSel = selC && selC->plugin.type == "gl.sampler";
    const bool audioFront = windowFront(a, "Loupe") && !instrumentShown(a);  // under the instrument editor it is not in view
    if (trackerSel) {
      const bool on = trackereditors::showing(a, trackereditors::Editor::Sample);
      if (b(7, "##smp", "Sample", "The Sample editor under the header (Ctrl+Alt+E; again: closes it)", on)) {
        if (on) closeInstrument(a);
        else trackereditors::open(a, trackereditors::Editor::Sample, a.selChannel);
      }
    } else {
      if (!samplerSel && !audioFront) ImGui::BeginDisabled();
      if (b(7, "##smp", "Sample", samplerSel ? "The Sampler's audio in the audio editor's page (again: the pattern)" : "This instrument has no sample to edit (a Tracker Sampler or a Sampler has)",
            audioFront))
        showPage(a, audioFront ? "Pattern editor" : "Loupe");
      if (!samplerSel && !audioFront) ImGui::EndDisabled();
    }
    if (b(8, "##cfg", "Settings", "The pattern editor's settings (the header shows them) and the way to every other setting", gSwap == Swap::Config))
      gSwap = gSwap == Swap::Config ? Swap::None : Swap::Config;
    if (b(9, "##help", "Tracker help", "The pattern keys, the effect letters and the screen keys (the header shows them)", gSwap == Swap::Help)) gSwap = Swap::Help;
  }
  // ---- instrument list, sample list, song name; the bank strip ----
  {
    const float nameH = std::round(H * 24.0f / 173.0f), yN = yE - nameH;
    // R12-04 (cp16): the samples stand beside the instruments, a column the band's height (before:
    // a strip under them, one row high at 1440x900). A narrow band keeps them under the
    // instruments, with at least three rows.
    const bool smpSide = listW >= fs * 30;
    const float smpW = smpSide ? std::floor(std::clamp(listW * 0.34f, fs * 13, fs * 20)) : 0.0f;
    const float xIe = xK - smpW;  // the instruments' right edge
    const float yI1 = smpSide ? yN : std::round(std::min(y0 + (yN - y0) * 0.62f, yN - 3 * std::round(fs + 1) - pad * 2 - 4));
    // Instruments.
    bevelBox(dl, ImVec2(xI, y0), ImVec2(xIe, yI1));
    const ImVec2 l0(xI + pad, y0 + pad), l1(xIe - pad, yI1 - pad);
    bevelBox(dl, l0, l1, true);
    const float lrh = std::max(14.0f, std::round(fs + 2));
    const int ci = s.channelIndex(a.selChannel);
    const int rowsVis = std::max(1, static_cast<int>((l1.y - l0.y - 2) / lrh));
    // A wide list shows its page in columns (each about 16 letters wide, at most four).
    const int cols = std::clamp(static_cast<int>((l1.x - l0.x) / std::max(160.0f, fs * 16)), 1, 4);
    const float colWi = std::floor((l1.x - l0.x) / static_cast<float>(cols));
    const int vis = rowsVis * cols;
    static int bank = 0;
    const int banks = std::max(1, (static_cast<int>(s.channels.size()) + vis - 1) / vis);
    // The page follows the selection when the selection changes (a page picked by hand stays).
    static int lastCi = -2;
    if (ci != lastCi && ci >= 0 && (ci < bank * vis || ci >= (bank + 1) * vis)) bank = ci / vis;
    lastCi = ci;
    bank = std::clamp(bank, 0, banks - 1);
    dl->PushClipRect(l0, l1, true);
    for (int r = 0; r < vis; ++r) {
      const int i = bank * vis + r;
      const float y = l0.y + 1 + static_cast<float>(r % rowsVis) * lrh;
      const float cx0 = l0.x + static_cast<float>(r / rowsVis) * colWi, cx1 = r / rowsVis == cols - 1 ? l1.x : cx0 + colWi;
      const std::string num = trk::hex2(i + 1);
      if (i >= static_cast<int>(s.channels.size())) {
        // R10-07: an empty slot can be picked, as in the classic list: the click makes the next
        // instrument there (a Tracker Sampler, as Add does; one undo step), selected, so a sample
        // loaded next fills it instead of replacing the current instrument.
        ImGui::SetCursorScreenPos(ImVec2(cx0, y));
        ImGui::PushID(1000 + i);
        const bool pick = ImGui::InvisibleButton("##insfree", ImVec2(std::max(1.0f, cx1 - cx0), lrh));
        const bool hovFree = ImGui::IsItemHovered();
        ImGui::PopID();
        ui::Hint("An empty slot: click to make an instrument here (a Tracker Sampler), then load a sample into it");
        if (hovFree) dl->AddRectFilled(ImVec2(cx0 + 1, y), ImVec2(cx1 - 1, y + lrh), theme::withAlpha(P.surface700, 0.6f));
        dl->AddText(mono, fs, ImVec2(cx0 + 9, y + 1), theme::withAlpha(P.textFaint, 0.6f), num.c_str());
        if (pick) {
          a.doc.beginGroup("add instrument");
          const ObjectId id = a.addInstrument("gl.tracker");
          if (id) a.doc.moveChannel(id, static_cast<int>(a.doc.song().channels.size()) - 1);
          a.doc.endGroup(false);
          if (id) {
            a.selectChannel(id);
            cursorToChannel(a, id);
          }
          break;  // the song changed: the list is drawn again next frame
        }
        continue;
      }
      const Channel& c = s.channels[static_cast<size_t>(i)];
      ImGui::SetCursorScreenPos(ImVec2(cx0, y));
      ImGui::PushID(1000 + i);
      const bool click = ImGui::InvisibleButton("##ins", ImVec2(std::max(1.0f, cx1 - cx0), lrh));
      const bool hov = ImGui::IsItemHovered();
      if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.settingsChannel = c.id;
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##insmenu");
      const auto revBefore = a.doc.revision();
      instrumentMenu(a, c.id);
      ImGui::PopID();
      // R11-01 (audit): the menu's Clone, Move and Exchange change the channel list, which may move
      // it (`c` would point at the old storage): the list is drawn again next frame.
      if (a.doc.revision() != revBefore) break;
      ui::Hint(packs::trackerpack::midiMuted(c.plugin)
                   ? i18n::N_("MIDI: its notes go to the MIDI output and its own sound is muted (the MIDI box, Ctrl+Alt+J) \xC2\xB7 click: select \xC2\xB7 double-click: its instrument")
                   : i18n::N_("Click: select (new notes use it) \xC2\xB7 click its number: mark it as the source \xC2\xB7 double-click: its instrument \xC2\xB7 right-click: move, clone, rename, mark, exchange"));
      // M-57: a click on the number marks the instrument as the source (again: unmarked), as the
      // classic list's number column; a click on the name selects it.
      const bool onNumber = ImGui::GetIO().MouseClickedPos[0].x < cx0 + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00").x + 2;
      if (click && onNumber) {
        gSourceChannel = gSourceChannel == c.id ? kNoId : c.id;
      } else if (click) {  // R3-06: the pattern keeps the keys, the cursor on the instrument's track
        a.selectChannel(c.id);
        cursorToChannel(a, c.id);
      }
      if (i == ci) dl->AddRectFilled(ImVec2(cx0 + 1, y), ImVec2(cx1 - 1, y + lrh), theme::withAlpha(P.surface600, 0.95f));
      else if (hov) dl->AddRectFilled(ImVec2(cx0 + 1, y), ImVec2(cx1 - 1, y + lrh), theme::withAlpha(P.surface700, 0.6f));
      dl->AddRectFilled(ImVec2(cx0 + 2, y + 2), ImVec2(cx0 + 5, y + lrh - 2), theme::rgb(c.color));
      if (c.id == gSourceChannel) dl->AddRect(ImVec2(cx0 + 1, y), ImVec2(cx1 - 1, y + lrh), P.hot);  // the source mark
      const float ty = std::round(y + (lrh - fs) * 0.5f);
      dl->AddText(mono, fs, ImVec2(cx0 + 9, ty), i == ci ? valueInk() : P.textDim, num.c_str());
      const float nx = cx0 + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
      if (gRenaming == c.id) {
        // Renamed in place: Enter or a click elsewhere keeps the name (one undo step), Esc does not.
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(nx - 2, std::round(y + (lrh - fs) * 0.5f) - ImGui::GetStyle().FramePadding.y));
        ImGui::SetNextItemWidth(std::max(24.0f, cx1 - nx - 2));
        ImGui::PushID(3000 + i);
        static int focusFrame = -1;
        if (focusFrame != ImGui::GetFrameCount() - 1 && !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        focusFrame = ImGui::GetFrameCount();
        const bool enter = ImGui::InputText("##insname", gRenameBuf, sizeof gRenameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (enter || ImGui::IsItemDeactivated()) {
          const std::string t = gRenameBuf;
          if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && !t.empty() && t != c.name) a.doc.renameChannel(c.id, t);
          gRenaming = kNoId;
          focusFrame = -1;
        }
        ImGui::PopID();
        dl->PushClipRect(l0, l1, true);
      } else {
        // R11-07: an instrument whose own sound is muted (its notes go to MIDI only) says so.
        float nameEnd = cx1 - 4;
        if (packs::trackerpack::midiMuted(c.plugin)) {
          const char* tag = "MIDI";
          const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, tag).x;
          dl->AddText(mono, fs, ImVec2(std::round(cx1 - 4 - tw), ty), P.hot, tag);
          nameEnd -= tw + 6;
        }
        theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), nameEnd - nx, i == ci ? P.text : c.muted ? P.textFaint : P.textDim, c.name.c_str());
      }
    }
    dl->PopClipRect();
    // Samples.
    const ImVec2 sb0 = smpSide ? ImVec2(xIe, y0) : ImVec2(xI, yI1);
    bevelBox(dl, sb0, ImVec2(xK, yN));
    const ImVec2 s0(sb0.x + pad, sb0.y + pad), s1(xK - pad, yN - pad);
    bevelBox(dl, s0, s1, true);
    const Channel* sc = s.channel(a.selChannel);
    // cp14 (R11-09, R11-10): a Tracker Sampler's samples as the classic sample list: each slot's
    // number and the name it was loaded as (an edit's file name never shows), the selected one lit,
    // a click selects it (the Sample tools and the editor follow), the wheel scrolls a long list.
    // cp16 (CPU): read from the instrument (its text parsed) only when the channel, the song or the
    // editor's sample selection changed, not every frame.
    static packs::trackerpack::HeaderSamples hsCache;
    static ObjectId hsChannel = kNoId;
    static uint64_t hsRev = ~uint64_t{0}, hsSel = 0;
    {
      const ObjectId ch = sc ? sc->id : kNoId;
      const uint64_t sel = sc ? packs::trackerpack::headerSampleSelKey(ch) : 0;
      // (and twice a second anyway, for a change that reaches the instrument outside the document)
      if (ch != hsChannel || a.doc.revision() != hsRev || sel != hsSel || ImGui::GetFrameCount() % 30 == 0) {
        hsCache = sc ? packs::trackerpack::headerSamples(a, ch) : packs::trackerpack::HeaderSamples{};
        hsChannel = ch, hsRev = a.doc.revision();
        hsSel = sc ? packs::trackerpack::headerSampleSelKey(ch) : 0;  // (reading it may have picked a selection)
      }
    }
    const packs::trackerpack::HeaderSamples& hs = hsCache;
    std::string sample;
    if (sc && !hs.tracker && !sc->plugin.data.empty() && sc->plugin.data.find('/') != std::string::npos)
      sample = std::filesystem::path(sc->plugin.data).filename().string();
    dl->PushClipRect(s0, s1, true);
    // The first row's text line (a box under a row high: centred in it).
    const float sy = std::round(s1.y - s0.y < (fs + 1) * 2 ? s0.y + (s1.y - s0.y - fs) * 0.5f : s0.y + 1 + 0.5f);
    if (!hs.list.empty()) {
      static int smpFirst = 0;
      const float srh = std::round(fs + 1);
      const int fit = std::max(1, static_cast<int>(std::floor((s1.y - s0.y - 2) / srh)));
      const int total = static_cast<int>(hs.list.size());
      int selRow = 0;
      for (int k = 0; k < total; ++k)
        if (hs.list[static_cast<size_t>(k)].first == hs.sel) selRow = k;
      static int lastSel = -2;
      if (hs.sel != lastSel) {  // the selection in view (another instrument, another sample)
        if (selRow < smpFirst) smpFirst = selRow;
        if (selRow >= smpFirst + fit) smpFirst = selRow - fit + 1;
        lastSel = hs.sel;
      }
      if (ImGui::IsMouseHoveringRect(s0, s1) && ImGui::GetIO().MouseWheel != 0) smpFirst += ImGui::GetIO().MouseWheel > 0 ? -1 : 1;
      smpFirst = std::clamp(smpFirst, 0, std::max(0, total - fit));
      const float y0s = s0.y + 1;
      for (int r = 0; r < fit && smpFirst + r < total; ++r) {
        const auto& [slot, name] = hs.list[static_cast<size_t>(smpFirst + r)];
        const float ry = std::round(y0s + static_cast<float>(r) * srh);
        ImGui::SetCursorScreenPos(ImVec2(s0.x, ry));
        ImGui::PushID(5000 + slot);
        const bool rowClick = ImGui::InvisibleButton("##smprow", ImVec2(std::max(1.0f, s1.x - s0.x), srh));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ui::Hint("Click: this sample is the one the Sample tools and the editor work on \xC2\xB7 click its number: mark it as the source sample (the wheel scrolls the list)");
        // M-60: a click on the number marks the source sample (again: unmarked), as on the instruments.
        if (rowClick && ImGui::GetIO().MouseClickedPos[0].x < s0.x + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00").x + 2)
          gSourceSlot = gSourceSlot == slot ? -1 : slot;
        else if (rowClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))  // cp18: the Sample editor on this sample
          trackereditors::open(a, trackereditors::Editor::Sample, sc->id, slot);
        else if (rowClick)
          packs::trackerpack::headerSelectSample(a, sc->id, slot);
        if (slot == gSourceSlot) dl->AddRect(ImVec2(s0.x + 1, ry), ImVec2(s1.x - 1, ry + srh), P.hot);  // the source sample mark
        if (slot == hs.sel) dl->AddRectFilled(ImVec2(s0.x + 1, ry), ImVec2(s1.x - 1, ry + srh), theme::withAlpha(P.surface600, 0.95f));
        else if (hov) dl->AddRectFilled(ImVec2(s0.x + 1, ry), ImVec2(s1.x - 1, ry + srh), theme::withAlpha(P.surface700, 0.6f));
        char num[12];
        std::snprintf(num, sizeof num, "%02d", slot + 1);
        const float ty = std::round(ry + (srh - fs) * 0.5f);
        dl->AddText(mono, fs, ImVec2(s0.x + 9, ty), slot == hs.sel ? valueInk() : P.textDim, num);
        const float nx = s0.x + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
        // cp20: the sample's marks at the row's end: loops (L forward, P ping-pong), 8 bits, its file missing.
        const uint8_t mk = smpFirst + r < static_cast<int>(hs.marks.size()) ? hs.marks[static_cast<size_t>(smpFirst + r)] : 0;
        float mx = s1.x - 4;
        if (mk) {
          namespace tpk = packs::trackerpack;
          const float mfs = std::round(fs * 0.85f);
          auto mark = [&](const char* t, ImU32 col) {
            const float tw = mono->CalcTextSizeA(mfs, FLT_MAX, 0, t).x;
            mx -= tw;
            dl->AddText(mono, mfs, ImVec2(std::round(mx), std::round(ry + (srh - mfs) * 0.5f)), col, t);
            mx -= 4;
          };
          if (mk & tpk::kMarkMissing) {
            mark("!", P.hot);
          }
          if (mk & tpk::kMark8Bit) {
            mark("8", P.textDim);
          }
          if (mk & tpk::kMarkPingPong) {
            mark("P", P.textDim);
          } else if (mk & tpk::kMarkLoop) {
            mark("L", P.textDim);
          }
          if (hov) {
            std::string why;
            if (mk & tpk::kMarkPingPong) {
              why += i18n::tr("loops back and forth");
            } else if (mk & tpk::kMarkLoop) {
              why += i18n::tr("loops");
            }
            if (mk & tpk::kMark8Bit) {
              why += std::string(why.empty() ? "" : ", ") + i18n::tr("plays at 8 bits");
            }
            if (mk & tpk::kMarkMissing) {
              why += std::string(why.empty() ? "" : ", ") + i18n::tr("its file is missing (or still arriving)");
            }
            trackerview::status(std::string(i18n::tr("Sample")) + " " + num + ": " + why);
          }
        }
        theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), mx - nx, slot == hs.sel ? P.text : P.textDim, name.c_str());
      }
    } else if (!sample.empty()) {
      dl->AddText(mono, fs, ImVec2(s0.x + 9, sy), P.hot, "01");
      dl->AddText(a.fontRegular, fs, ImVec2(s0.x + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x, sy), P.text, sample.c_str());
    } else {
      const char* why = !sc                       ? i18n::tr("No channel selected")
                        : hs.tracker && hs.sound  ? i18n::tr("No sample yet: its built-in sound plays")
                        : hs.tracker              ? i18n::tr("No sample yet: load one (Files, Samples) or drop audio on it")
                                                  : i18n::tr("No sample: a synthesized sound");
      dl->AddText(a.fontRegular, fs, ImVec2(s0.x + 9, sy), P.textFaint, why, nullptr, std::max(1.0f, s1.x - s0.x - 14));
    }
    dl->PopClipRect();
    // Song name.
    bevelBox(dl, ImVec2(xI, yN), ImVec2(xK, yE));
    const ImVec2 n0(xI + pad, yN + 3), n1(xK - pad, yE - 3);
    bevelBox(dl, n0, n1, true);
    // Edited in place: a click opens the field, Enter or a click elsewhere keeps it (one undo step).
    static bool editing = false;
    static char buf[128] = {};
    if (editing) {
      ImGui::SetCursorScreenPos(ImVec2(n0.x + 2, std::round(n0.y + (n1.y - n0.y - fs) * 0.5f) - ImGui::GetStyle().FramePadding.y));
      ImGui::SetNextItemWidth(n1.x - n0.x - 4);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
      if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
      const bool enter = ImGui::InputText("##titleedit", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      const bool done = enter || ImGui::IsItemDeactivated();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
      if (done) {
        editing = false;
        const std::string t = buf;
        if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && t != s.info.title) a.doc.editSong("rename song", [t](Song& song) { song.info.title = t; });
      }
    } else {
      const std::string title = s.info.title.empty() ? std::string(i18n::tr("Untitled song")) : s.info.title;
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(n0.x + 6, std::round(n0.y + (n1.y - n0.y - fs) * 0.5f)), n1.x - n0.x - 12,
                          s.info.title.empty() ? P.textFaint : P.text, title.c_str());
      ImGui::SetCursorScreenPos(n0);
      if (ImGui::InvisibleButton("##title", ImVec2(std::max(1.0f, n1.x - n0.x), std::max(1.0f, n1.y - n0.y)))) {
        editing = true;
        std::snprintf(buf, sizeof buf, "%s", s.info.title.c_str());
      }
      ui::Hint("The song's title (click to edit)");
    }
    if (gRenaming != kNoId && !s.channel(gRenaming)) gRenaming = kNoId;
    gHeaderTyping = editing || gRenaming != kNoId || (gSwap == Swap::Disk && gDiskTyping);
    // Bank strip: eight pages of the instrument list, the column filled; pages past the last
    // channel are there but off (like the list's numbers past the last channel).
    // More than eight pages: the strip shows the eight around the page in front (the wheel over
    // it steps a page).
    bevelBox(dl, ImVec2(xK, y0), ImVec2(o.x + W, yE));
    if (ImGui::IsMouseHoveringRect(ImVec2(xK, y0), ImVec2(o.x + W, yE)) && ImGui::GetIO().MouseWheel != 0)
      bank = std::clamp(bank + (ImGui::GetIO().MouseWheel > 0 ? -1 : 1), 0, banks - 1);
    const int group = bank / 8;
    // T-09: more than eight pages: a ninth button, Swap, shows the next eight (as the classic bank
    // swap between the first and the second 64 instruments), wrapping round to the first.
    const bool swap = banks > 8;
    const int slots = swap ? 9 : 8;
    const float kb = std::floor((H - 4 - static_cast<float>(slots - 1)) / static_cast<float>(slots));
    for (int gb = 0; gb < slots; ++gb) {
      const float t0 = std::round(y0 + 2 + static_cast<float>(gb) * (kb + 1));
      const float t1 = gb == slots - 1 ? yE - 2 : t0 + kb;
      if (gb == 8) {
        char hint[96];
        const int next = (group + 1) * 8 < banks ? group + 1 : 0;
        std::snprintf(hint, sizeof hint, "%s %02X-%02X", i18n::tr("The next eight pages:"), next * 8 * vis + 1, std::min(banks, next * 8 + 8) * vis);
        if (bevelButton(a, "##bankswap", "Swap", ImVec2(xK + 2, t0), ImVec2(o.x + W - 2, t1), false, hint)) bank = next * 8;
        break;
      }
      const int b = group * 8 + gb;
      char lbl[16];
      std::snprintf(lbl, sizeof lbl, "%02X-%02X", b * vis + 1, (b + 1) * vis);
      ImGui::PushID(2000 + b);
      if (b >= banks) ImGui::BeginDisabled();
      if (bevelButton(a, "##bank", lbl, ImVec2(xK + 2, t0), ImVec2(o.x + W - 2, t1), b == bank, "Instruments of this page")) bank = b;
      if (b >= banks) ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  pageStrip(a, o, W, yE + 2, stripH);
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + av.y));
  ImGui::Dummy(ImVec2(1, 0));
}

// The classic instrument list's source mark and its exchange and copy (the extension screens'
// "Xchg ins." and the number column's source click), plus moving and cloning: a right-click on an
// instrument. The source is kept for the session.
ObjectId gSourceChannel = kNoId;
int gSourceSlot = -1;  // M-60: the source sample's number (any instrument's slot of that number)

// SX-12 / SX-14 (the classic "Copy ins." and "Xchg ins."): the source instrument's sound (its
// instrument plugin and that plugin's state: samples, envelopes, settings) copied over `dst`, or
// the two trading sounds. As in the classic editor the names stay, and so do the notes, colours and
// mixer routes: the patterns play the other sound. One undo step.
bool copyInstrumentSound(App& a, ObjectId dst, ObjectId src, bool exchange) {
  const Song& s = a.doc.song();
  const Channel* d = s.channel(dst);
  const Channel* c = s.channel(src);
  if (!d || !c || dst == src) {
    a.notify(i18n::tr("Mark another instrument as the source first (click its number in the list)"));
    return false;
  }
  const bool typeChanged = d->plugin.type != c->plugin.type;
  a.doc.editSong(exchange ? "exchange instrument sounds" : "copy instrument sound", [&](Song& song) {
    Channel* x = song.channel(dst);
    Channel* y = song.channel(src);
    if (!x || !y) return;
    if (exchange) std::swap(x->plugin, y->plugin);
    else x->plugin = y->plugin;
    if (!typeChanged) return;
    // The old instruments' parameters and outputs are gone (as when an instrument is replaced).
    auto gone = [&](ObjectId id) {
      return [id](const ModLink& l) {
        return (l.owner == id && l.index >= kChannelPluginBase && l.index < kNoteFxBase) || (l.sourceOwner == id && l.sourceSlot < 0);
      };
    };
    auto& links = song.modLinks;
    links.erase(std::remove_if(links.begin(), links.end(), gone(dst)), links.end());
    if (exchange) links.erase(std::remove_if(links.begin(), links.end(), gone(src)), links.end());
  });
  return true;
}

void instrumentMenu(App& a, ObjectId id) {
  if (!ImGui::BeginPopup("##insmenu")) return;
  const Song& s = a.doc.song();
  const int i = s.channelIndex(id), n = static_cast<int>(s.channels.size()), src = s.channelIndex(gSourceChannel);
  if (ImGui::MenuItem(i18n::tr("Move up"), nullptr, false, i > 0)) a.doc.moveChannel(id, i - 1);
  if (ImGui::MenuItem(i18n::tr("Move down"), nullptr, false, i >= 0 && i + 1 < n)) a.doc.moveChannel(id, i + 1);
  if (ImGui::MenuItem(i18n::tr("Clone (after it)"))) a.doc.cloneChannel(id);
  if (ImGui::MenuItem(i18n::tr("Rename")) && i >= 0) {  // R4-11: typed in place in the list
    gRenaming = id;
    std::snprintf(gRenameBuf, sizeof gRenameBuf, "%s", s.channels[static_cast<size_t>(i)].name.c_str());
  }
  // cp17 (K-316 3.3, the user's Q3): the instrument's editor under the header (Ctrl+Alt+E), and its
  // plugin window only from here: an installed plugin's own window, a built-in's editor (Channel
  // settings, in the lower band).
  if (i >= 0) {
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Edit instrument"))) trackereditors::open(a, trackereditors::Editor::Instrument, id);
    if (trackereditors::hasSampleEditor(a, id) && ImGui::MenuItem(i18n::tr("Edit samples"), "Ctrl+Alt+E")) trackereditors::open(a, trackereditors::Editor::Sample, id);
    if (ImGui::MenuItem(i18n::tr("Open plugin window"))) trackereditors::openPluginWindow(a, id);
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Mark as the source"), nullptr, gSourceChannel == id)) gSourceChannel = gSourceChannel == id ? kNoId : id;
  const std::string srcNum = src >= 0 ? " " + trk::hex2(src + 1) : std::string();
  const bool other = src >= 0 && src != i && i >= 0;
  // cp15: the sound (the classic copy and exchange: names and notes stay) and the places.
  if (ImGui::MenuItem((std::string(i18n::tr("Copy the source's sound here")) + srcNum).c_str(), nullptr, false, other)) copyInstrumentSound(a, id, gSourceChannel, false);
  if (ImGui::MenuItem((std::string(i18n::tr("Swap sounds with the source")) + srcNum).c_str(), nullptr, false, other)) copyInstrumentSound(a, id, gSourceChannel, true);
  const std::string xchg = std::string(i18n::tr("Swap places with the source")) + srcNum;
  if (ImGui::MenuItem(xchg.c_str(), nullptr, false, other)) {
    const int lo = std::min(i, src), hi = std::max(i, src);
    const ObjectId a0 = s.channels[static_cast<size_t>(lo)].id, b0 = s.channels[static_cast<size_t>(hi)].id;
    a.doc.beginGroup("exchange instruments");
    a.doc.moveChannel(a0, hi);
    a.doc.moveChannel(b0, lo);
    a.doc.endGroup(false);
  }
  ImGui::EndPopup();
}

void pageStrip(App& a, ImVec2 o, float W, float py0, float stripH) {
  const float fs = ImGui::GetFontSize(), py1 = py0 + stripH;
  float px = o.x;
  for (const Page& pg : kPages) {
    const char* label = i18n::tr(pg.label);
    const float w = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, label).x) + 24;
    if (px + w > o.x + W) break;
    ImGui::PushID(pg.window);
    if (bevelButton(a, "##page", pg.label, ImVec2(px, py0), ImVec2(px + w, py1), pageOn(a, pg), pg.hint)) showPage(a, pg.window);
    ImGui::PopID();
    px += w + 2;
  }
}
}  // namespace

namespace {
Tick gSongPos = 0;  // the song position while the engine is in pattern mode (its counter then counts the pattern)
}

// R4-01, R4-04: the song position. In song mode the engine's; in pattern mode (playing a pattern, or
// stopped after one) the one kept here, which the order list, Ins, Del and Enter act on.
Tick songPosition(App& a) { return a.engine.mode() == PlayMode::Pattern ? gSongPos : static_cast<Tick>(a.engine.position()); }

void setSongPosition(App& a, Tick t) {
  gSongPos = std::max<Tick>(0, t);
  if (a.engine.mode() != PlayMode::Pattern) a.engine.setPosition(gSongPos, true);
}

void stepSongPosition(App& a, int d) {
  const std::vector<trk::OrderEntry> ol = trk::orderList(a.doc.song());
  if (ol.empty() || d == 0) return;
  const int cur = currentEntry(a, ol);
  const int i = std::clamp(cur < 0 ? 0 : cur + d, 0, static_cast<int>(ol.size()) - 1);
  if (i == cur) return;
  const trk::OrderEntry& e = ol[static_cast<size_t>(i)];
  if (e.pattern != kNoId) a.selectPattern(e.pattern);
  setSongPosition(a, e.start);
}

void songMode(App& a) {
  if (a.engine.mode() == PlayMode::Song) return;
  const Tick t = gSongPos;
  a.engine.setMode(PlayMode::Song);
  a.engine.setPosition(t, true);
}

// N-01, T-15: the order list's current entry is the one under the song position. When the shown
// pattern changes (Ctrl+Left and Right, the Pattern stepper, a layout switch) to one that does not
// play there, the position goes to that pattern's first entry, so the list, the editor and the
// position agree and the order buttons act on what is shown. R4-01: the same while a pattern plays
// (the pattern play counts its own rows; the song position it keeps is the one picked last).
void syncOrder(App& a, bool entering) {
  static ObjectId last = kNoId;
  static uint64_t epoch = ~uint64_t{0};
  static bool wasPlaying = false;
  const bool playing = a.engine.playing(), patternMode = a.engine.mode() == PlayMode::Pattern;
  const bool stopped = wasPlaying && !playing;
  wasPlaying = playing;
  const Song& s = a.doc.song();
  if (!patternMode) gSongPos = static_cast<Tick>(a.engine.position());
  // R3-01, R4-04: a pattern played and stopped leaves the song position where it was (or where it
  // was picked while the pattern played) and the engine in pattern mode at the pattern's start, so
  // Play (Space) loops the pattern again and the order list, Del and Enter act where they did.
  if (stopped && patternMode) a.engine.setPosition(0, true);
  // R3-02: entering the layout, or a project opened: the position stays (Studio's playhead too); the
  // pattern under it is the one shown.
  if (entering || a.doc.projectEpoch() != epoch) {
    if (a.doc.projectEpoch() != epoch && patternMode) gSongPos = 0;  // (another project: its start)
    epoch = a.doc.projectEpoch();
    if (!playing)
      if (const ObjectId at = trk::patternAt(s, songPosition(a)); at != kNoId && at != a.curPattern) a.selectPattern(at);
    last = a.curPattern;
    return;
  }
  const bool changed = a.curPattern != last;
  last = a.curPattern;
  if (!changed || (playing && !patternMode) || a.curPattern == kNoId) return;
  if (trk::patternTickAt(s, a.curPattern, songPosition(a)) >= 0) return;
  for (const trk::OrderEntry& e : trk::orderList(s))
    if (e.pattern == a.curPattern) {
      setSongPosition(a, e.start);
      return;
    }
}

// R4-03: the band never keeps the keyboard (the classic screen has no focus outside its text
// fields): once a click in it is done (an order entry, a bank, the sample box, a scope, a menu item,
// any of its buttons), the keys go back to the pattern when its page is the one shown. A text
// field being typed in, a menu or a popup still open, and a drag keep them until they end.
void headerKeepsNoKeys() {
  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || gHeaderTyping) return;
  if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) return;
  if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
  for (int b = 0; b < ImGuiMouseButton_COUNT; ++b)
    if (ImGui::IsMouseDown(b)) return;
  keysBackToPattern();
}

void mainScreen(App& a) {
  packs::closeAudioEditorWindows(a);  // R4-02: a floating audio editor closes with the other screens
  closeInstrument(a);                 // R5-03: the instrument editor closes too
  gSwap = Swap::None;
  gTool = ScopeTool::Scopes;
  setExtended(a, false);
  showPage(a, "Pattern editor");
  keysBackToPattern();
}

void toggleScopeTool(App& a, int which) {
  static const ScopeTool kTools[] = {ScopeTool::Scopes, ScopeTool::Transpose, ScopeTool::Trim, ScopeTool::AdvEdit, ScopeTool::Midi, ScopeTool::Sample};
  const ScopeTool t = which >= 1 && which <= 5 ? kTools[which] : ScopeTool::Scopes;
  // The scope box is in the main band: a screen swapped over it, or the strip, gives way first.
  const bool shownNow = gTool == t && gSwap == Swap::None && !extended();
  gSwap = Swap::None;
  setExtended(a, false);
  gTool = shownNow ? ScopeTool::Scopes : t;
}
int scopeTool() { return static_cast<int>(gTool); }  // (the enum's order: 0 scopes, 1 Transpose ... 5 Sample tools)

void setHeaderSwap(int which) {
  gSwap = which == 1 ? Swap::Help : which == 2 ? Swap::Disk : which == 3 ? Swap::Config : which == 4 ? Swap::About : Swap::None;
}
// The audio editor's key (Ctrl+Alt+8) does what its header button does: the audio editor in front
// (the instrument editor closes: the two share the lower band, as the classic sample and instrument
// editors), or, in front already, the pattern back.
void toggleAudioPage(App& a) {
  if (trackereditors::hasSampleEditor(a, a.selChannel)) {  // cp18: a Tracker Sampler's Sample editor
    trackereditors::toggle(a, trackereditors::Editor::Sample, true);
    return;
  }
  showPage(a, windowFront(a, "Loupe") && !instrumentShown(a) ? "Pattern editor" : "Loupe");
}
int headerSwap() { return gSwap == Swap::Help ? 1 : gSwap == Swap::Disk ? 2 : gSwap == Swap::Config ? 3 : gSwap == Swap::About ? 4 : 0; }
void setConfigPage(int page) { gConfigPage = std::clamp(page, 0, 3); }
void filesLoadSample(App& a, const std::string& path) {
  const int was = gDiskKind;
  gDiskKind = 1;  // the Samples kind: a double-click on an audio file there
  diskOpen(a, path);
  gDiskKind = was;
}

void orderList(App& a) {
  if (layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker)) {
    trackerpages::enteringFrame();  // keeps the Tracker's frame count (see there)
    const PerfScope perfScope(PerfBand);  // cp16: the band's frame time ("tracker perf")
    // The Tracker starts on the pattern page, with the keys (a layout loaded from its saved state
    // too). K-297 (cp16): back from another layout, the page that was in front comes back in front,
    // as the classic tracker keeps the screen it was on: a song half arranged on the Song page, or a
    // mix on the Mix page, is where the musician left it (before: always the pattern). The keys go
    // to the pattern when that is the page.
    static int lastFrame = -10, lastFrameSeen = -10;
    const int frame = ImGui::GetFrameCount();
    // K-297 (round 8): a restart brings back the page that was in front at quit. R8-04: the audio
    // editor in front when the Tracker went off screen comes back in front, as the instrument editor
    // in the lower band does.
    static bool audioFront = false;
    static const char* front = nullptr;  // the page in front on the Tracker's last frame
    if (frame - lastFrame > 1) {
      const std::string start = takeStartPage();
      if (const bool* sf = start.empty() ? nullptr : layoutui::windowOpenFlag(a, start); sf && *sf) showPage(a, start.c_str());
      else if (audioFront) showPage(a, "Loupe");
      else if (front && std::strcmp(front, "Pattern editor") != 0) showPage(a, front);
      else if (const bool* f = layoutui::windowOpenFlag(a, "Pattern editor"); f && *f) showPage(a, "Pattern editor");
    } else {
      audioFront = windowFront(a, "Loupe") && !instrumentShown(a);
      for (const Page& pg : kPages)
        if (std::strcmp(pg.window, "Loupe") != 0 && pageOn(a, pg)) front = pg.window;
    }
    lastFrame = frame;
    // K-297: the Settings window, opened from anywhere (the menu, its key, All settings), docks with
    // the centre's pages: it comes to the front as a page does (before: docked behind the pattern,
    // so nothing seemed to open).
    {
      static bool settingsWas = false;
      if (a.showSettings && !settingsWas) bringPage("Settings");
      settingsWas = a.showSettings;
    }
    pumpPage(a);
    syncOrder(a, frame - lastFrameSeen > 1);
    lastFrameSeen = frame;
    // K-106 cp17 (K-316 3.2, user's Q1): the lower band's editor keeps the pattern's nine rows: the
    // band gets what is left under the header once the pattern has its header line, nine rows and
    // its bars. In a small window (about 640 to 760 px tall) where even the editor's least height does
    // not fit under the full header, the header folds to its compact strip while an editor is open
    // (back when it closes). The full band's height is the one it had when last shown whole.
    static float fullBandH = 0;
    const float workH = ImGui::GetMainViewport()->WorkSize.y, fs0 = ImGui::GetFontSize();
    const float patRowH = std::max(fs0, patternRowHeight());
    const float patternMin = fs0 * 7.0f + 16 + patRowH * 9;  // (the page strip, track headers, bars and nine rows)
    trackereditors::tend(a);  // cp18: a preview whose screen went ends
    const float editorNeed = trackereditors::need(a) > 0 ? trackereditors::need(a) : instrumentEditorNeed();
    const float editorMin = std::min(std::round(fs0 * 21.5f), editorNeed > 0 ? std::ceil(editorNeed) : std::round(fs0 * 21.5f));
    const float bandFull = fullBandH > 0 ? fullBandH : std::round(fs0 * 17.7f);
    gFolded = !extended() && instrumentShown(a) && workH - bandFull - patternMin < editorMin;
    auto sizeLower = [&](float headerH) {
      if (!instrumentShown(a)) return;
      const float space = workH - headerH;  // the lower band and the pattern
      const float most = std::floor(std::max(gFolded ? editorMin : space * 0.5f, space - patternMin));
      layoutui::setBandSize("tracker.lower", editorNeed > 0 ? std::clamp(std::ceil(editorNeed), std::min(200.0f, most), most) : most, 0.9f);
    };
    if (extended() || gFolded) {  // the classic Extend: the band is one strip, the pattern gets the rows
      layoutui::setBandSize("tracker.header", extendedStripH());
      if (gFolded) sizeLower(extendedStripH());
      extendedStrip(a);
      headerKeepsNoKeys();
      return;
    }
    if (gSwap == Swap::None) fullBandH = ImGui::GetWindowHeight();
    // Help grows the band (about 45 percent of the window) while it shows, so its rows show at once;
    // the pattern below stays in view and editable. Settings fits the band as it is (R6-12: its
    // groups are compact, as the classic config pages).
    if (gSwap == Swap::Help)
      layoutui::setBandSize("tracker.header", std::max(ImGui::GetWindowHeight(), std::floor(ImGui::GetMainViewport()->WorkSize.y * 0.45f)));
    // The instrument's editor (Channel settings) takes the whole lower band, as the classic
    // instrument editor replaces the pattern there (ref 145; R6-05): everything under the header but
    // the page tabs and one track-header line, so the editor shows whole. Never restyled.
    // The editor shows the current instrument, as the classic one does: another instrument picked
    // while it is open (the list, the keypad, Ctrl+Shift+Up/Down) shows in it at once.
    {
      static ObjectId lastSel = kNoId;
      if (a.selChannel != lastSel) {
        if (a.settingsChannel != kNoId && a.selChannel != kNoId && a.doc.song().channelIndex(a.selChannel) >= 0 && !trackereditors::pluginWindowOpen(a))
          a.settingsChannel = a.selChannel;
        lastSel = a.selChannel;
      }
    }
    // K-106 cp13: the editor takes the height it needs and the pattern keeps the rest, at least
    // its header line and nine rows (the cursor row in the middle), so notes stay in view and
    // editable while the instrument is edited. A taller editor scrolls in its band.
    // (Under the band: the page strip, the track header line, the scroll bar and the status line
    // besides the rows, about seven text heights in all.)
    sizeLower(ImGui::GetWindowHeight());
    // T-21: a band too short for its ten button rows at the interface's text size (a small window)
    // draws its text smaller, down to three quarters, rather than letting it run over the edges.
    {
      const float fs0 = ImGui::GetFontSize(), h = ImGui::GetContentRegionAvail().y;
      const float need = fs0 * 13.0f + 22;  // ten button rows of text plus their bevels, and the page strip
      const float scale = h < need ? std::max(0.75f, h / need) : 1.0f;
      if (scale < 1.0f) ImGui::SetWindowFontScale(scale);
      header(a);
      if (scale < 1.0f) ImGui::SetWindowFontScale(1.0f);
    }
    headerKeepsNoKeys();
    return;
  }
  const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  orderRows(a, o, ImVec2(o.x + av.x, o.y + std::max(av.y, 60.0f)));
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + std::max(av.y, 60.0f)));
  ImGui::Dummy(ImVec2(1, 0));
}

}  // namespace gl::trackerview

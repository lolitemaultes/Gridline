#include "Widgets.h"
#include "DialogLayout.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "LayoutAudit.h"
#include "AlignAudit.h"
#include "../packs/modulation/modulation_ui.h"
#include "App.h"
#include "Keys.h"
#include "Windows.h"
#include "Theme.h"
#include "icons/IconTexture.h"
#include "../core/I18n.h"
#include "../core/WheelRoute.h"

namespace gl::ui {

using theme::pal;
using i18n::tr;

namespace {
constexpr float kPi = 3.14159265f;

void arc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float th) {
  dl->PathArcTo(c, r, a0, a1, 32);
  dl->PathStroke(col, 0, th);
}
}  // namespace

// ---- J-83: wheel routing ----
namespace {
wheelroute::Router gWheel;
bool gPaneScrolls = false;    // innermost WheelPolicyScope
ImGuiID gDragBlocked = 0;     // pressed while the pane was scrolling: no drag until released
}  // namespace

WheelPolicyScope::WheelPolicyScope(bool paneScrolls) : prev_(gPaneScrolls) { gPaneScrolls = paneScrolls; }
WheelPolicyScope::~WheelPolicyScope() { gPaneScrolls = prev_; }
bool wheelPaneScrolls() { return gPaneScrolls; }
bool wheelScrolling() { return gWheel.scrolling(); }
bool wheelFocusActive() { return gWheel.focused() != 0; }
bool wheelModifierHeld() { return keys::modifierIncludes("mixer.wheelAdjust"); }
void wheelClaim() { gWheel.claim(); }
void wheelFrameBegin() { gWheel.beginFrame(ImGui::GetTime(), ImGui::GetIO().MouseWheel); }
void wheelFrameEnd() { gWheel.endFrame(); }
const char* wheelHintText() { return tr("Drag to change, Ctrl+wheel to fine-tune"); }

WheelGate wheelGate(bool takesWheel) {
  WheelGate g;
  const bool hov = ImGui::IsItemHovered();
  if (!gPaneScrolls) {  // adjusting pane (the normal mixer, every other view): as before
    g.scale = hov && takesWheel ? 1.0f : 0.0f;
    return g;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const ImGuiID id = ImGui::GetItemID();
  const bool act = ImGui::IsItemActive();
  if (ImGui::IsItemActivated() && !gWheel.mayStartDrag(true)) gDragBlocked = id;
  const bool blocked = gDragBlocked == id;
  if (blocked && !act) gDragBlocked = 0;
  g.dragOk = !blocked;
  if (hov) gWheel.hovered(id);
  // A click (press and release without dragging) gives the control wheel focus.
  if (ImGui::IsItemDeactivated() && !blocked && io.MouseDragMaxDistanceSqr[0] < 16.0f) gWheel.focus(id);
  if (!hov || !takesWheel) return g;
  const wheelroute::Query q{true, wheelModifierHeld(), gWheel.hasFocus(id)};
  if (gWheel.route(q) == wheelroute::Route::Adjust) {
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the pane stays put under the control
    if (io.MouseWheel != 0) gWheel.claim();
    g.scale = wheelroute::Router::scale(q);
  } else {
    g.hint = true;
  }
  return g;
}

// J-55: hints, tooltips, labels, section captions, button and tab texts are
// translated here, so callers pass plain English.
void Hint(const std::string& text) {
  if (HintHovered()) app().setHint(tr(text.c_str()));
}
void Hint(const char* text) {
  if (text && HintHovered()) app().setHint(tr(text));
}
bool HintHovered() {
  // While files are dragged (browser / other applications) the status bar says what the drop does (J-75).
  if (const ImGuiPayload* p = ImGui::GetDragDropPayload();
      p && (p->IsDataType("GL_FILE") || p->IsDataType("GL_ITEM") || p->IsDataType("GL_FILES") || p->IsDataType("GL_PLUGIN")))  // J-97: plugins too
    return false;
  return ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

void Label(const char* text) {
  ImGui::PushFont(app().fontMono, theme::kFontLabel);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", tr(text));
  ImGui::PopFont();
}

void Tooltip(const char* text) {
  if (!text || !*text) return;
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay) && !ImGui::IsItemActive() &&
      ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(tr(text));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void FocusRing() {
  if (!ImGui::IsItemActive()) return;
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), pal().accent,
                                      ImGui::GetStyle().FrameRounding + 1, 0, 1.5f);
}

namespace {
struct ToolFlash {
  int tool = -1;
  double until = 0;
  std::string text;
};
std::map<std::string, ToolFlash>& toolFlashes() {
  static std::map<std::string, ToolFlash> m;
  return m;
}

// Q-13: every icon is an SVG on the 24-unit grid (assets/icons/ui), drawn from
// the icon atlas. `s` is the glyph box of the old vector set: the SVG box is
// 1.2x that (the grid keeps a 2-unit margin), snapped to a standard size.
float svgBox(float s) { return icons::standardSize(s * 1.2f); }
}  // namespace

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col) {
  icons::draw(dl, iconSvgName(icon), c, svgBox(s), col);
}

void drawIconBox(ImDrawList* dl, Icon icon, ImVec2 c, float box, ImU32 col) {
  icons::draw(dl, iconSvgName(icon), c, icons::standardSize(box), col);
}

void drawIconFit(ImDrawList* dl, Icon icon, ImVec2 inkMin, ImVec2 inkMax, ImU32 col) {
  icons::drawFit(dl, iconSvgName(icon), inkMin, inkMax, col);
}

void drawIconGlyph(ImDrawList* dl, Icon icon, ImVec2 c, float unitPx, float oldStroke, ImU32 col) {
  icons::drawGlyph(dl, iconSvgName(icon), c, unitPx, oldStroke, col);
}

ImVec2 TextSizeExact(const char* s) {
  ImFont* f = ImGui::GetFont();
  if (!f || !s) return ImVec2(0, ImGui::GetFontSize());
  return f->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, s, ImGui::FindRenderedTextEnd(s));
}

const char* labelText(const char* s) {
  if (!s || s[0] != '\x1b') return s;
  const char* t = std::strchr(s, '\x1f');
  return t ? t + 1 : s + std::strlen(s);
}

void drawLabel(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* s) {
  if (!s) return;
  if (s[0] != '\x1b') {
    dl->AddText(font, size, pos, col, s);
    return;
  }
  const char* text = labelText(s);
  const std::string svg(s + 1, static_cast<size_t>(text - 1 - (s + 1)));
  // Ink bounds of the text the icon stands for, as AddText would draw it.
  if (!font) font = ImGui::GetFont();
  ImFontBaked* baked = font ? font->GetFontBaked(size) : nullptr;
  if (!baked) return;
  const float k = size / baked->Size;
  // AddText snaps the start to the nearest whole pixel (J-128), then advances by the glyphs' widths.
  pos = ImVec2(std::floor(pos.x + 0.5f), std::floor(pos.y + 0.5f));
  float x = pos.x, x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (const char* c = text; *c;) {
    unsigned int ch = 0;
    c += ImTextCharFromUtf8(&ch, c, nullptr);
    const ImFontGlyph* g = baked->FindGlyph(static_cast<ImWchar>(ch));
    if (!g) continue;
    ImVec2 a, b;
    if (icons::glyphInk(font, size, static_cast<ImWchar>(ch), ImVec2(x, pos.y), &a, &b))
      x0 = std::min(x0, a.x), y0 = std::min(y0, a.y), x1 = std::max(x1, b.x), y1 = std::max(y1, b.y);
    x += g->AdvanceX * k;
  }
  // Text too small for its icon (a 2 px minus) stays text.
  if (x1 <= x0 || !icons::drawFit(dl, svg.c_str(), ImVec2(x0, y0), ImVec2(x1, y1), col, true)) dl->AddText(font, size, pos, col, text);
}

void drawLabel(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* s) { drawLabel(dl, ImGui::GetFont(), ImGui::GetFontSize(), pos, col, s); }

void installGlyphHook() { icons::atlas(); }

bool IconButton(const char* id, Icon icon, bool active, float size, const char* hint, ImU32 activeCol, ImU32 idleCol, bool onPress) {
  ImGui::PushID(id);
  ImVec2 p = ImGui::GetCursorScreenPos();
  // J-128: after a taller item on the same line (combo, text button), centre on
  // the row instead of hanging from its top; the next item keeps the row's top.
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  const float lineH = win->DC.IsSameLine ? win->DC.CurrLineSize.y : 0.0f;
  const float dy = lineH > size + 1.0f && lineH < size * 2.0f ? std::floor((lineH - size) * 0.5f) : 0.0f;
  if (dy > 0) p.y += dy, ImGui::SetCursorScreenPos(p);
  bool pressed = ImGui::InvisibleButton("##ib", ImVec2(size, size), onPress ? static_cast<ImGuiButtonFlags>(ImGuiButtonFlags_PressedOnClick) : static_cast<ImGuiButtonFlags>(ImGuiButtonFlags_None));  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  // Studio look (both themes): an active button keeps a quiet raised face and
  // lights its glyph in the tool's role colour (activeCol) or the lit-glyph orange.
  const ImU32 glyph = activeCol ? theme::lit(activeCol) : P.hot;
  ImU32 bg = held ? P.surface600 : active ? theme::mix(hov ? P.surface700 : P.surface800, glyph, P.dark ? 0.10f : 0.14f) : hov ? P.surface700 : 0;
  ImU32 fg = active ? glyph : hov ? P.text : idleCol ? theme::lit(idleCol) : P.textDim;
  if (bg) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), bg, theme::kRadius);
  if (active) dl->AddRect(p, ImVec2(p.x + size, p.y + size), theme::withAlpha(fg, P.dark ? 0.28f : 0.45f), theme::kRadius);
  drawIcon(dl, icon, ImVec2(std::round(p.x + size * 0.5f), std::round(p.y + size * 0.5f)), std::round(size * 0.56f), fg);
  if (hint) {
    Hint(hint);
    Tooltip(hint);
  }
  ImGui::PopID();
  return pressed;
}

bool ToggleIcon(const char* id, Icon icon, bool on, float size, const char* hint, float mirror) {
  ImGui::PushID(id);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool pressed = ImGui::InvisibleButton("##ti", ImVec2(size, size));
  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  ImU32 bg = held ? P.surface600 : hov ? P.surface700 : on ? P.surface800 : 0;
  if (bg) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), bg, theme::kRadius);
  // On: lit glyph (window / option toggles light up in the view role colour).
  const ImU32 lit = theme::litRole(theme::role::kView);
  ImU32 fg = on ? lit : hov ? P.text : P.textFaint;
  const ImVec2 ic(std::round(p.x + size * 0.5f), std::round(p.y + size * 0.5f - (on ? 1 : 0)));
  const int v0 = dl->VtxBuffer.Size;
  drawIcon(dl, icon, ic, std::round(size * 0.52f), fg);
  if (mirror != 1.0f)  // J-91: the glyph's quad(s) scaled about the centre (mirrored at -1)
    for (int i = v0; i < dl->VtxBuffer.Size; ++i) dl->VtxBuffer[i].pos.x = ic.x + (dl->VtxBuffer[i].pos.x - ic.x) * mirror;
  if (on) {
    float w = std::round(size * 0.36f);
    float x = std::round(p.x + (size - w) * 0.5f);
    dl->AddRectFilled(ImVec2(x, p.y + size - 4), ImVec2(x + w, p.y + size - 2), lit, 1.0f);
  }
  if (hint) {
    Hint(hint);
    Tooltip(hint);
  }
  ImGui::PopID();
  return pressed;
}

bool EyeToggle(const char* id, bool shown, float size) {
  return IconButton(id, eyeIcon(shown), shown, size, eyeHint(shown));
}

std::string toolTip(const char* name, const char* desc, const char* keyId) {
  std::string s = tr(name);
  if (const char* k = keyId ? keys::label(keyId) : nullptr) s += std::string(" (") + k + ")";
  s += std::string(": ") + tr(desc);
  return s;
}

bool ToolButton(const char* id, Icon icon, bool active, float size, const char* name, const char* desc, const char* keyId, ImU32 roleCol) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##tool", ImVec2(size, size));
  const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  const ImU32 lit = roleCol ? theme::lit(roleCol) : P.hot;
  const ImVec2 q1(p.x + size, p.y + size);
  // Faces: pressed (deepest), active (tinted by the tool's role colour), hover.
  if (held) dl->AddRectFilled(p, q1, P.surface600, theme::kRadius);
  else if (active) dl->AddRectFilled(p, q1, theme::mix(hov ? P.surface700 : P.surface800, lit, P.dark ? 0.14f : 0.16f), theme::kRadius);
  else if (hov) dl->AddRectFilled(p, q1, P.surface700, theme::kRadius);
  if (active) {
    dl->AddRect(p, q1, theme::withAlpha(lit, P.dark ? 0.35f : 0.55f), theme::kRadius);
    // lit underline, as the top bar's window toggles draw it (ToggleIcon): 2 px, 36 % of the cell
    const float uw = std::round(size * 0.36f), ux = std::round(p.x + (size - uw) * 0.5f);
    dl->AddRectFilled(ImVec2(ux, q1.y - 4), ImVec2(ux + uw, q1.y - 2), lit, 1.0f);
  }
  const ImU32 fg = active ? lit : held || hov ? P.text : P.textDim;
  // Every tool's ink fits the same square (12 px in a 24 px control), centred: the SVGs fill their
  // 24-unit grid differently (the pencil reaches the corners, the brush is narrow), so drawing them in
  // one SVG box made them different sizes and put some on the underline.
  const float ink = std::round(icons::toolIconBox(size) * 0.75f);
  const ImVec2 ic(p.x + size * 0.5f, p.y + size * 0.5f - (active ? 1.0f : 0.0f));
  drawIconFit(dl, icon, ImVec2(ic.x - ink * 0.5f, ic.y - ink * 0.5f), ImVec2(ic.x + ink * 0.5f, ic.y + ink * 0.5f), fg);
  if (hov) {
    const std::string tip = toolTip(name, desc, keyId);
    app().setHint(tip);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay) && !held && ImGui::BeginTooltip()) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
      ImGui::TextUnformatted(tip.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
  }
  ImGui::PopID();
  return pressed;
}

void noteActiveTool(const char* strip, int tool, const char* name, const char* keyId) {
  ToolFlash& f = toolFlashes()[strip];
  const double now = ImGui::GetTime();
  if (f.tool != tool) {
    if (f.tool >= 0) {
      std::string k = keyId && keys::label(keyId) ? std::string(" (") + keys::label(keyId) + ")" : std::string();
      f.text = std::string(tr("Tool")) + ": " + tr(name) + k;
      f.until = now + 1.6;
    }
    f.tool = tool;
  }
  if (now < f.until && app().hint.empty()) app().setHint(f.text);
}

bool TextButton(const char* label, bool active, float width, const char* hint) {
  const auto& P = pal();
  if (active) {  // lit: raised face tinted by the lit-glyph colour, orange text (both themes)
    const float t = P.dark ? 0.16f : 0.14f;
    ImGui::PushStyleColor(ImGuiCol_Button, theme::vec(theme::mix(P.surface800, P.hot, t)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(theme::mix(P.surface700, P.hot, t + 0.02f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::vec(theme::mix(P.surface600, P.hot, t + 0.04f)));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.hot));
  }
  bool r = false;
  if (label && label[0] == '\x1b') {  // icon label: the SVG where the text would be
    const char* text = labelText(label);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushID(label);
    r = ImGui::Button("##icon", ImVec2(width > 0 ? width : ts.x + st.FramePadding.x * 2, 0));
    ImGui::PopID();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 pos(a.x + std::max(0.0f, (b.x - a.x - ts.x) * st.ButtonTextAlign.x), a.y + std::max(0.0f, (b.y - a.y - ts.y) * st.ButtonTextAlign.y));
    drawLabel(ImGui::GetWindowDrawList(), pos, ImGui::GetColorU32(ImGuiCol_Text), label);
  } else {
    r = ImGui::Button(tr(label), ImVec2(width, 0));
  }
  if (active) ImGui::PopStyleColor(4);
  if (hint) {
    Hint(hint);
    Tooltip(hint);
  }
  return r;
}

bool DangerButton(const char* label, bool armed, float width, const char* hint) {
  const auto& P = pal();
  if (armed) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::vec(P.danger));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(theme::mix(P.danger, P.paper, 0.12f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::vec(theme::mix(P.danger, P.ink, 0.15f)));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::contrastText(P.danger)));
  } else {
    const float t = P.dark ? 0.10f : 0.08f;
    ImGui::PushStyleColor(ImGuiCol_Button, theme::vec(theme::mix(P.surface800, P.danger, t)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(theme::mix(P.surface700, P.danger, t + 0.06f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::vec(theme::mix(P.surface600, P.danger, t + 0.10f)));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::lit(P.danger)));
  }
  const bool r = ImGui::Button(tr(label), ImVec2(width, 0));
  ImGui::PopStyleColor(4);
  if (hint) {
    Hint(hint);
    Tooltip(hint);
  }
  return r;
}

float TextButtonWidth(const char* label) {
  const char* t = label && label[0] == '\x1b' ? labelText(label) : tr(label);
  return ImGui::CalcTextSize(t, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

void StatusDot(ImU32 col, bool pulse) {
  const float h = ImGui::GetTextLineHeight(), r = 3.5f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float baseOff = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
  ImGui::Dummy(ImVec2(r * 2 + 2, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // on the text's line: a frame-aligned line (AlignTextToFramePadding) starts its text lower
  const ImVec2 c(p.x + r + 1, p.y + baseOff + h * 0.5f);
  if (pulse) {
    const float k = 0.5f + 0.5f * static_cast<float>(std::sin(ImGui::GetTime() * 4.0));
    dl->AddCircleFilled(c, r + 2.5f * k, theme::withAlpha(col, 0.25f * (1 - k)), 16);
  }
  dl->AddCircleFilled(c, r, col, 16);
  ImGui::SameLine(0, theme::kS1 + 2);
}

bool Segmented(const char* id, const char* const* labels, int count, int* index, float segWidth, float height,
               const char* const* hints) {
  const auto& P = pal();
  ImGui::PushID(id);
  if (height <= 0) height = ImGui::GetFrameHeight();
  std::vector<const char*> trLabels(static_cast<size_t>(std::max(0, count)));
  for (int i = 0; i < count; ++i) trLabels[static_cast<size_t>(i)] = tr(labels[i]);
  labels = trLabels.data();
  float pad = ImGui::GetStyle().FramePadding.x;
  float w = segWidth;
  if (w <= 0)
    for (int i = 0; i < count; ++i) w = std::max(w, ImGui::CalcTextSize(labels[i]).x + pad * 2);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w * count + 4, p.y + height), P.field, theme::kRadius);
  if (!P.dark) dl->AddRect(p, ImVec2(p.x + w * count + 4, p.y + height), P.line, theme::kRadius);
  bool changed = false;
  for (int i = 0; i < count; ++i) {
    ImGui::PushID(i);
    ImVec2 q(p.x + 2 + w * i, p.y + 2);
    ImGui::SetCursorScreenPos(q);
    if (ImGui::InvisibleButton("##seg", ImVec2(w, height - 4)) && *index != i) {
      *index = i;
      changed = true;
    }
    bool hov = ImGui::IsItemHovered();
    bool on = *index == i;
    const ImU32 onBg = theme::mix(P.dark ? P.surface700 : P.surface800, P.hot, 0.14f);
    if (on)
      dl->AddRectFilled(q, ImVec2(q.x + w, q.y + height - 4), onBg, std::max(1.0f, theme::kRadius - 1));
    else if (hov)
      dl->AddRectFilled(q, ImVec2(q.x + w, q.y + height - 4), P.surface700, std::max(1.0f, theme::kRadius - 1));
    ImVec2 ts = ImGui::CalcTextSize(labels[i], nullptr, true);
    const ImU32 onFg = P.hot;
    if (ts.x > w - 6) {  // J-55: a longer translation is fitted to its segment (full text on hover)
      const std::string shown(labels[i], ImGui::FindRenderedTextEnd(labels[i]));
      theme::textEllipsis(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(std::round(q.x + 3), std::round(q.y + (height - 4 - ts.y) * 0.5f)),
                          w - 6, on ? onFg : hov ? P.text : P.textDim, shown.c_str());
      if (!(hints && hints[i])) Tooltip(shown.c_str());
    } else {
      dl->AddText(ImVec2(std::round(q.x + (w - ts.x) * 0.5f), std::round(q.y + (height - 4 - ts.y) * 0.5f)),
                  on ? onFg : hov ? P.text : P.textDim, labels[i], ImGui::FindRenderedTextEnd(labels[i]));
    }
    if (hints && hints[i]) {
      Hint(hints[i]);
      Tooltip(hints[i]);
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(w * count + 4, height));
  ImGui::PopID();
  return changed;
}

bool Knob(const char* id, double* value, const ParamDesc& d, float radius, const char* label, bool bipolar,
          bool* released, const std::function<void()>& extraMenu) {
  ImGui::PushID(id);
  const auto& P = pal();
  const float labelFs = std::min(theme::kFontCaption, ImGui::GetFontSize());
  float w = std::max(radius * 2.0f, label ? ImGui::GetFont()->CalcTextSizeA(labelFs, 1e9f, 0, label).x : 0.0f);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float labelH = label ? labelFs + 2.0f : 0.0f;
  // J-128: without a label the item is exactly the face, so the item's centre is the
  // knob's centre (rows of bare knobs line up with the controls beside them).
  alignaudit::nextKind(align::kKnob);  // J-128 panel audit: a dial, not a button
  ImGui::InvisibleButton("##knob", ImVec2(w, radius * 2.0f + (label ? labelH + 2 : 0.0f)));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const WheelGate wg = wheelGate();  // J-83
  bool changed = false;
  double n = d.toNormalized(*value);
  ImGuiIO& io = ImGui::GetIO();
  if (act && wg.dragOk && io.MouseDelta.y != 0 && !io.KeyAlt) {
    double speed = io.KeyShift ? 0.0008 : 0.005;
    double nn = std::clamp(n - io.MouseDelta.y * speed, 0.0, 1.0);
    if (d.kind == ParamKind::Choice) {
      static std::map<ImGuiID, double> accum;
      double& a = accum[ImGui::GetItemID()];
      a = std::clamp((ImGui::IsItemActivated() ? n : a) - io.MouseDelta.y * 0.01, 0.0, 1.0);
      nn = a;
    }
    double v = d.fromNormalized(nn);
    if (v != *value) {
      *value = v;
      changed = true;
    }
  }
  if (wg.scale > 0 && io.MouseWheel != 0 && !act) {
    double step = d.kind == ParamKind::Choice
                      ? 1.0 / std::max<size_t>(1, d.choices.empty() ? 1 : d.choices.size() - 1)
                      : (io.KeyShift ? 0.005 : 0.03) * wg.scale;
    double v = d.fromNormalized(std::clamp(n + io.MouseWheel * step, 0.0, 1.0));
    if (v != *value) {
      *value = v;
      changed = true;
    }
    if (released) *released = true;
  }
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    *value = d.def;
    changed = true;
    if (released) *released = true;
  }
  if (released && ImGui::IsItemDeactivated()) *released = true;

  // Draw: track arc 270°, value arc, pointer dot.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 c(p.x + w * 0.5f, p.y + radius);
  float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  float an = a0 + static_cast<float>(d.toNormalized(*value)) * (a1 - a0);
  const float th = radius >= 14.0f ? 2.5f : 2.0f;
  // Wheel: dark cap with a lighter top edge, rail ring, value ring (light at
  // rest, lit orange while touched), indicator line.
  const ImU32 cap = act ? P.surface600 : hov ? P.surface700 : P.surface800;
  dl->AddCircleFilled(c, radius - th - 1.0f, cap, 32);
  if (P.dark) dl->AddCircle(c, radius - th - 1.5f, theme::withAlpha(P.paper, 0.10f), 32, 1.0f);
  arc(dl, c, radius - th * 0.5f, a0, a1, P.dark ? P.field : P.line, th);
  float from = bipolar ? (a0 + a1) * 0.5f : a0;
  const ImU32 ringRest = P.knobFill;
  if (std::fabs(an - from) > 0.01f)
    arc(dl, c, radius - th * 0.5f, std::min(from, an), std::max(from, an), act || hov ? P.hot : ringRest, th);
  ImVec2 dir(std::cos(an), std::sin(an));
  dl->PathLineTo(ImVec2(c.x + dir.x * radius * 0.15f, c.y + dir.y * radius * 0.15f));  // a path: AddLine is half a pixel off centre
  dl->PathLineTo(ImVec2(c.x + dir.x * (radius - th - 3.0f), c.y + dir.y * (radius - th - 3.0f)));
  dl->PathStroke(hov || act ? P.paper : P.text, 0, 2.0f);
  if (label) {
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(labelFs, 1e9f, 0, label);
    dl->AddText(ImGui::GetFont(), labelFs, ImVec2(std::round(c.x - ts.x * 0.5f), p.y + radius * 2.0f + 2.0f), P.textDim, label);
  }
  std::string txt = d.name + ": " + d.format(*value);
  if (hov || act) app().setHint(txt + "   (drag · Shift fine · double-click reset · right-click type)" + (wg.hint ? std::string("   ") + wheelHintText() : ""));
  if (act) ImGui::SetTooltip("%s", d.format(*value).c_str());
  if (wg.hint) Tooltip("Drag to change, Ctrl+wheel to fine-tune");  // J-83: the wheel scrolls the pane here

  // Type-in value.
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (ImGui::IsItemClicked(ImGuiMouseButton_Left) && io.KeyAlt))
    ImGui::OpenPopup("##type");
  if (ImGui::BeginPopup("##type")) {
    static double typed = 0;
    if (ImGui::IsWindowAppearing()) typed = *value;
    ImGui::TextUnformatted(d.name.c_str());
    ImGui::SetNextItemWidth(140);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputDouble("##v", &typed, 0, 0, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue)) {
      *value = d.clamp(typed);
      changed = true;
      if (released) *released = true;
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::Button(i18n::tr("Reset to default"))) {
      *value = d.def;
      changed = true;
      if (released) *released = true;
      ImGui::CloseCurrentPopup();
    }
    if (extraMenu) {
      ImGui::Separator();
      extraMenu();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

bool ParamKnob(const char* id, ParamId pid, float radius, const char* label, bool bipolar) {
  App& a = app();
  auto d = describeParam(a.doc.song(), pid);
  auto v = getParam(a.doc.song(), pid);
  if (!d || !v) {
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    return false;
  }
  audit::bindControl(pid);  // J-92 binding audit
  double val = *v;
  bool released = false;
  bool linked = false;
  for (const auto& l : a.doc.song().links) linked = linked || (l.owner == pid.owner && l.index == pid.index);
  bool modulated = isModulated(a.doc.song(), pid);  // internal controller link
  auto menu = [&a, pid]() {
    packs::modulation::drawLinkMenu(a, pid);  // controller links (modulation pack)
    knobContextMenu(a, pid);                  // values, MIDI learn, reset
  };
  ImVec2 kp = ImGui::GetCursorScreenPos();
  bool ch = Knob(id, &val, *d, radius, label, bipolar, &released, menu);
  if (modulated) packs::modulation::drawModulationMarker(a, pid, ImVec2(kp.x + radius, kp.y + radius), radius);
  if (linked || modulated || (a.midiLearning && a.midiLearnTarget == pid)) {  // small dot marks linked / learning knobs
    ImU32 c = a.midiLearning && a.midiLearnTarget == pid ? pal().danger : pal().accent;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(kp.x + radius * 2 - 2, kp.y + 2), 2.5f, c);
  }
  if (ch) a.doc.setParam(pid, val, true);
  if (released) a.doc.endGesture();
  return ch;
}

bool ParamFader(const char* id, ParamId pid, ImVec2 size) {
  App& a = app();
  auto d = describeParam(a.doc.song(), pid);
  auto v = getParam(a.doc.song(), pid);
  if (!d || !v) return false;
  audit::bindControl(pid);  // J-92 binding audit
  ImGui::PushID(id);
  const auto& P = pal();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##fader", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const WheelGate wg = wheelGate();  // J-83: the thin mixer scrolls under the pointer
  ImGuiIO& io = ImGui::GetIO();
  double n = d->toNormalized(*v);
  bool changed = false;
  float capH = 18.0f, travel = size.y - capH;
  if (act && wg.dragOk && io.MouseDelta.y != 0) {
    double nn = std::clamp(n - io.MouseDelta.y / travel * (io.KeyShift ? 0.15 : 1.0), 0.0, 1.0);
    a.doc.setParam(pid, d->fromNormalized(nn), true);
    changed = true;
  }
  if (wg.scale > 0 && io.MouseWheel != 0 && !act) {
    a.doc.setParam(pid, d->fromNormalized(std::clamp(n + io.MouseWheel * 0.02 * wg.scale, 0.0, 1.0)), true);
    a.doc.endGesture();
  }
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    a.doc.setParam(pid, d->def, false);
    changed = true;
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::BeginPopupContextItem("##fadermenu")) {  // values, controller links
    knobContextMenu(a, pid);
    ImGui::EndPopup();
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float cx = std::round(p.x + size.x * 0.5f);
  float t0 = p.y + capH * 0.5f, t1 = p.y + size.y - capH * 0.5f;
  double cur = d->toNormalized(*getParam(a.doc.song(), pid));
  float y = std::round(p.y + static_cast<float>(1.0 - cur) * travel);
  // Track (sunken slot) with a value fill below the cap: neutral at rest, accent while touched.
  dl->AddRectFilled(ImVec2(cx - 2, t0), ImVec2(cx + 2, t1), P.surface950, 2);
  dl->AddRectFilled(ImVec2(cx - 1, y + capH * 0.5f), ImVec2(cx + 1, t1),
                    act || hov ? theme::withAlpha(P.hot, 0.9f) : P.lineStrong, 1);
  // Scale ticks every eighth of the travel + the default (unity) mark.
  for (int i = 0; i <= 8; ++i) {
    float ty = std::round(t0 + (t1 - t0) * i / 8.0f);
    dl->AddLine(ImVec2(p.x + 1, ty), ImVec2(p.x + 4, ty), P.line, 1);
    dl->AddLine(ImVec2(p.x + size.x - 4, ty), ImVec2(p.x + size.x - 1, ty), P.line, 1);
  }
  float uy = std::round(t0 + static_cast<float>(1.0 - d->toNormalized(d->def)) * travel);
  dl->AddLine(ImVec2(p.x, uy), ImVec2(p.x + 6, uy), P.textFaint, 1);
  dl->AddLine(ImVec2(p.x + size.x - 6, uy), ImVec2(p.x + size.x, uy), P.textFaint, 1);
  // Cap: raised block with a centre grip line.
  ImU32 capCol = act ? theme::mix(P.knobFill, P.hot, 0.35f) : hov ? P.knobFill : theme::mix(P.textDim, P.surface600, 0.35f);
  if (!P.dark) capCol = act ? theme::mix(P.surface800, P.hot, 0.3f) : hov ? P.paper : P.surface800;
  dl->AddRectFilled(ImVec2(p.x + 3, y + 1), ImVec2(p.x + size.x - 3, y + capH + 1), theme::withAlpha(P.ink, 0.35f), 3);
  dl->AddRectFilled(ImVec2(p.x + 3, y), ImVec2(p.x + size.x - 3, y + capH), capCol, 3);
  dl->AddLine(ImVec2(p.x + 6, y + capH * 0.5f), ImVec2(p.x + size.x - 6, y + capH * 0.5f), P.surface900, 2);
  if (hov || act) {
    std::string txt = d->name + ": " + d->format(*getParam(a.doc.song(), pid));
    if (wg.hint) txt += std::string("   ") + wheelHintText();
    app().setHint(txt);
    if (act) ImGui::SetTooltip("%s", d->format(*getParam(a.doc.song(), pid)).c_str());
  }
  if (wg.hint) Tooltip("Drag to change, Ctrl+wheel to fine-tune");  // J-83 (delayed, after the hover settles)
  ImGui::PopID();
  return changed;
}

void Meter(ImGuiID key, float peakL, float peakR, ImVec2 size, bool vertical) {
  struct State {
    float l = 0, r = 0, holdL = 0, holdR = 0;
    double holdT = 0;
  };
  static std::map<ImGuiID, State> states;
  State& s = states[key];
  float dt = ImGui::GetIO().DeltaTime;
  float decay = std::exp(-dt * 6.0f);
  s.l = std::max(peakL, s.l * decay);
  s.r = std::max(peakR, s.r * decay);
  double now = ImGui::GetTime();
  if (s.l >= s.holdL || s.r >= s.holdR || now - s.holdT > 1.5) {
    s.holdL = s.l;
    s.holdR = s.r;
    s.holdT = now;
  }
  const auto& P = pal();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto toPos = [](float g) {  // dB scale -60..+6
    float db = g > 1e-6f ? 20.0f * std::log10(g) : -100.0f;
    return std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
  };
  const float zero = toPos(1.0f), warn = toPos(0.5f);  // 0 dBFS and -6 dBFS
  for (int ch = 0; ch < 2; ++ch) {
    float v = toPos(ch == 0 ? s.l : s.r), hold = toPos(ch == 0 ? s.holdL : s.holdR);
    if (vertical) {
      float w = std::floor((size.x - 1) * 0.5f);
      ImVec2 a(p.x + ch * (w + 1), p.y), b(a.x + w, p.y + size.y);
      dl->AddRectFilled(a, b, P.surface950, 1);
      auto yAt = [&](float f) { return b.y - f * size.y; };
      float top = yAt(v);
      dl->AddRectFilled(ImVec2(a.x, std::max(top, yAt(warn))), b, P.meterLow);
      if (v > warn) dl->AddRectFilled(ImVec2(a.x, std::max(top, yAt(zero))), ImVec2(b.x, yAt(warn)), P.meterMid);
      if (v > zero) dl->AddRectFilled(ImVec2(a.x, top), ImVec2(b.x, yAt(zero)), P.meterHigh);
      if (hold > 0.01f) {
        float hy = std::round(yAt(hold));
        dl->AddLine(ImVec2(a.x, hy), ImVec2(b.x, hy), hold > zero ? P.danger : P.text, 1);
      }
    } else {
      float h = std::floor((size.y - 1) * 0.5f);
      ImVec2 a(p.x, p.y + ch * (h + 1)), b(p.x + size.x, a.y + h);
      dl->AddRectFilled(a, b, P.surface950, 1);
      auto xAt = [&](float f) { return a.x + f * size.x; };
      float right = xAt(v);
      dl->AddRectFilled(a, ImVec2(std::min(right, xAt(warn)), b.y), P.meterLow);
      if (v > warn) dl->AddRectFilled(ImVec2(xAt(warn), a.y), ImVec2(std::min(right, xAt(zero)), b.y), P.meterMid);
      if (v > zero) dl->AddRectFilled(ImVec2(xAt(zero), a.y), ImVec2(right, b.y), P.meterHigh);
    }
  }
  // 0 dB reference mark.
  if (vertical)
    dl->AddLine(ImVec2(p.x - 2, std::round(p.y + size.y - zero * size.y)), ImVec2(p.x, std::round(p.y + size.y - zero * size.y)), P.textFaint);
}

bool BeginCombo(const char* id, const char* preview, float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetFrameHeight();
  ImGui::SetNextItemWidth(width);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(theme::kS2, ImGui::GetStyle().FramePadding.y));
  std::string pv = theme::fitText(preview, width - theme::kS2 * 2 - 14);
  bool open = ImGui::BeginCombo(id, pv.c_str(), ImGuiComboFlags_NoArrowButton);
  ImGui::PopStyleVar();
  drawIcon(dl, Icon::ChevronDown, ImVec2(std::round(p.x + width - 12), std::round(p.y + h * 0.5f)), 10, pal().textDim);
  return open;
}

// ------------------------------------------------------------ layout ----

std::string windowTitle(const char* english) {
  // ImHashStr restarts at "###", so "Mixer" and "Mischpult###Mixer" share one id.
  return std::string(i18n::tr(english)) + "###" + english;
}

const char* stableWindowName(const char* name) {
  if (!name) return "";
  const char* p = std::strstr(name, "###");
  return p ? p + 3 : name;
}

bool BeginDockWindow(const char* name, bool* open, ImVec2 firstSize) {
  ImGui::SetNextWindowSize(firstSize, ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  bool r = ImGui::Begin(windowTitle(name).c_str(), open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  return r;
}

void EndDockWindow() { ImGui::End(); }

void BeginToolbar(const char* id) {
  const auto& P = pal();
  const float padY = std::floor((theme::kToolbarH - ImGui::GetFrameHeight()) * 0.5f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, padY));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kS1, theme::kS1));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
  ImGui::BeginChild(id, ImVec2(0, theme::kToolbarH), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
}

void EndToolbar() {
  ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, wp.y + ws.y - 1), ImVec2(wp.x + ws.x, wp.y + ws.y - 1), pal().line);
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
}

void ToolbarDivider() {
  ImGui::SameLine(0, theme::kS2);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetFrameHeight();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 4), ImVec2(p.x, p.y + h - 4), pal().line);
  ImGui::Dummy(ImVec2(1, h));
  ImGui::SameLine(0, theme::kS2);
}

void EmptyState(ImDrawList* dl, ImVec2 mn, ImVec2 mx, const char* title, const char* body) {
  const auto& P = pal();
  App& a = app();
  float w = mx.x - mn.x, h = mx.y - mn.y;
  if (w < 40 || h < 30) return;
  if (title) title = tr(title);
  if (body) body = tr(body);
  float mark = 80.0f;
  bool showMark = h >= 190 && w >= 160;
  float titleH = title ? theme::kFontBody + 2 : 0, bodyH = body ? theme::kFontCaption + 2 : 0;
  float total = (showMark ? mark + theme::kS4 : 0) + titleH + (body ? theme::kS1 + bodyH : 0);
  float y = std::round(mn.y + (h - total) * 0.5f);
  float cx = mn.x + w * 0.5f;
  if (showMark) {
    theme::drawMark(dl, ImVec2(std::round(cx - mark * 0.5f), y), mark, P.logo, P.surface900, 0.5f);
    y += mark + theme::kS4;
  }
  float maxW = w - theme::kS6 * 2;
  {  // text plate: keeps the message readable over editor grids (studio hint-panel look)
    float tw = 0;
    if (title) tw = std::max(tw, a.fontBold->CalcTextSizeA(theme::kFontBody, 1e9f, 0, title).x);
    if (body) tw = std::max(tw, a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, body).x);
    tw = std::min(tw, maxW);
    const float th = titleH + (body ? theme::kS1 + bodyH : 0);
    if (tw > 0) {
      const ImVec2 q0(std::round(cx - tw * 0.5f - theme::kS3), y - theme::kS2), q1(std::round(cx + tw * 0.5f + theme::kS3), y + th + theme::kS2);
      dl->AddRectFilled(q0, q1, theme::withAlpha(theme::popupBg(P), 0.88f), theme::kRadiusLg);
      dl->AddRect(q0, q1, P.dark ? theme::withAlpha(P.ink, 0.35f) : P.line, theme::kRadiusLg);
    }
  }
  auto line = [&](ImFont* f, float fs, ImU32 col, const char* t) {
    ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0, t);
    float tw = std::min(ts.x, maxW);
    theme::textEllipsis(dl, f, fs, ImVec2(std::round(cx - tw * 0.5f), y), maxW, col, t);
    y += fs + 2;
  };
  if (title) line(a.fontBold, theme::kFontBody, P.textDim, title);
  if (body) {
    y += theme::kS1;
    line(a.fontRegular, theme::kFontCaption, P.textFaint, body);
  }
}

namespace {
struct SectionFrame {
  ImDrawListSplitter split;
  ImVec2 start;
  float right = 0, workMaxX = 0, contentMaxX = 0;
  float width = 0;  // the width it takes in the layout (its border is drawn 1 px inside)
  std::string caption;
};
std::vector<std::unique_ptr<SectionFrame>> gSections;
constexpr float kSectionPad = theme::kS2;
}  // namespace

void BeginSection(const char* caption, float width) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  auto f = std::make_unique<SectionFrame>();
  f->start = ImGui::GetCursorScreenPos();
  const float avail = ImGui::GetContentRegionAvail().x;
  f->width = width > 0 ? std::min(width, avail) : avail;
  f->right = f->start.x + f->width - 1;  // border stays inside the clip rect
  f->caption = caption ? tr(caption) : "";
  f->workMaxX = w->WorkRect.Max.x;
  f->contentMaxX = w->ContentRegionRect.Max.x;
  f->split.Split(w->DrawList, 2);
  f->split.SetCurrentChannel(w->DrawList, 1);
  // Children use GetContentRegionAvail(): end the work rect at the panel's inner edge.
  w->WorkRect.Max.x = w->ContentRegionRect.Max.x = f->right - kSectionPad;
  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(0, f->caption.empty() ? kSectionPad - ImGui::GetStyle().ItemSpacing.y : theme::kFontLabel + 4));
  ImGui::Indent(kSectionPad);
  gSections.push_back(std::move(f));
}

void EndSection() {
  if (gSections.empty()) return;
  std::unique_ptr<SectionFrame> f = std::move(gSections.back());
  gSections.pop_back();
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  ImGui::Unindent(kSectionPad);
  ImGui::Dummy(ImVec2(0, std::max(0.0f, kSectionPad - ImGui::GetStyle().ItemSpacing.y)));
  ImGui::EndGroup();
  w->WorkRect.Max.x = f->workMaxX;
  w->ContentRegionRect.Max.x = f->contentMaxX;
  const ImVec2 mx(f->right, ImGui::GetItemRectMax().y);
  // Reserve the full panel width (fill panels keep auto-resizing windows stable). J-111h: the whole
  // width it was given, not the border's: one pixel less made an auto-resizing window (AlwaysAutoResize,
  // sizes its width to its content) one pixel narrower every frame, down to its minimum width.
  ImGui::SetCursorScreenPos(ImVec2(f->start.x, mx.y));
  ImGui::Dummy(ImVec2(f->width, 0));
  f->split.SetCurrentChannel(w->DrawList, 0);
  const auto& P = pal();
  ImDrawList* dl = w->DrawList;
  const ImU32 face = P.dark ? theme::mix(P.surface900, P.field, 0.38f) : theme::mix(P.surface900, P.surface800, 0.55f);
  dl->AddRectFilled(f->start, mx, face, theme::kRadiusLg);
  dl->AddRect(f->start, mx, P.dark ? theme::withAlpha(P.ink, 0.45f) : P.line, theme::kRadiusLg);
  if (P.dark)  // lower lip catches the light: the box reads as recessed into the slate
    dl->AddLine(ImVec2(f->start.x + 3, mx.y), ImVec2(mx.x - 3, mx.y), theme::withAlpha(P.paper, 0.06f));
  if (!f->caption.empty()) {
    App& a = app();
    theme::textEllipsis(dl, a.fontMono, theme::kFontLabel, ImVec2(f->start.x + kSectionPad, f->start.y + 5),
                        mx.x - f->start.x - kSectionPad * 2, P.textFaint, f->caption.c_str());
  }
  f->split.Merge(dl);
  ImGui::Dummy(ImVec2(0, 2));
}

bool TabStrip(const char* id, const char* const* labels, int count, int* index, const char* const* hints) {
  const auto& P = pal();
  ImGui::PushID(id);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q0 = ImGui::GetCursorScreenPos();
  const float tabH = 28.0f, full = ImGui::GetContentRegionAvail().x;
  // J-55: translated labels; when the tabs do not fit (more pages, longer
  // languages) the side padding shrinks, then every tab is cut to fit with an
  // ellipsis (full text in the tooltip).
  std::vector<const char*> tl(static_cast<size_t>(std::max(0, count)));
  float textSum = 0;
  for (int i = 0; i < count; ++i) {
    tl[static_cast<size_t>(i)] = tr(labels[i]);
    textSum += ImGui::CalcTextSize(tl[static_cast<size_t>(i)]).x;
  }
  const float gaps = 2.0f * std::max(0, count - 1);
  float padX = theme::kS4, squeeze = 1.0f;
  if (count > 0 && textSum + gaps + 2 * padX * count > full) {
    padX = std::max(theme::kS1 + 2, std::floor((full - gaps - textSum) / (2.0f * count)));
    if (textSum + gaps + 2 * padX * count > full) squeeze = std::max(0.2f, (full - gaps - 2 * padX * count) / std::max(1.0f, textSum));
  }
  float x = q0.x;
  bool changed = false;
  for (int i = 0; i < count; ++i) {
    const char* text = tl[static_cast<size_t>(i)];
    const float textW = std::floor(ImGui::CalcTextSize(text).x * squeeze);
    const float tw = std::round(textW + 2 * padX);
    ImGui::SetCursorScreenPos(ImVec2(x, q0.y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##tab", ImVec2(tw, tabH)) && *index != i) {
      *index = i;
      changed = true;
    }
    const bool hov = ImGui::IsItemHovered(), on = *index == i;
    if (hints && hints[i]) {
      Hint(hints[i]);
      Tooltip(hints[i]);
    } else if (squeeze < 1.0f) {
      Tooltip(labels[i]);
    }
    ImGui::PopID();
    if (on || hov)
      dl->AddRectFilled(ImVec2(x, q0.y), ImVec2(x + tw, q0.y + tabH), on ? P.surface800 : theme::mix(P.surface900, P.surface800, 0.5f),
                        theme::kRadius, ImDrawFlags_RoundCornersTop);
    if (on) dl->AddRectFilled(ImVec2(x + 1, q0.y), ImVec2(x + tw - 1, q0.y + 2), P.hot, 1.0f);
    theme::textEllipsis(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(std::round(x + padX), std::round(q0.y + (tabH - ImGui::GetFontSize()) * 0.5f)),
                        textW + 1, on ? P.hot : hov ? P.text : P.textDim, text);
    x += tw + 2;
  }
  dl->AddLine(ImVec2(q0.x, q0.y + tabH), ImVec2(q0.x + full, q0.y + tabH), P.dark ? theme::withAlpha(P.ink, 0.5f) : P.lineStrong);
  ImGui::SetCursorScreenPos(ImVec2(q0.x, q0.y + tabH));
  ImGui::Dummy(ImVec2(std::max(0.0f, std::min(full, x - q0.x)), theme::kS2));
  ImGui::PopID();
  return changed;
}

void DragPreview(Icon icon, const char* text, ImU32 tint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 22.0f, tw = std::min(ImGui::CalcTextSize(text).x, 260.0f);
  const ImU32 bar = tint ? tint : P.hot;
  dl->AddRectFilled(p, ImVec2(p.x + 4, p.y + h), bar, 2.0f);
  drawIcon(dl, icon, ImVec2(std::round(p.x + 17), std::round(p.y + h * 0.5f)), 12, theme::lit(bar));
  theme::textEllipsis(dl, nullptr, 0, ImVec2(p.x + 30, std::round(p.y + (h - ImGui::GetFontSize()) * 0.5f)), tw + 1, P.text, text);
  ImGui::Dummy(ImVec2(tw + 36, h));
}

void Led(ImDrawList* dl, ImVec2 c, float r, bool on, float level) {
  const auto& P = pal();
  // Studio LED (both themes): green lamp, brighter with activity; off = dark
  // (light theme: pale) lens with a thin rim.
  const ImU32 rim = P.dark ? theme::withAlpha(P.ink, 0.5f) : theme::withAlpha(P.lineStrong, 0.9f);
  if (!on) {
    dl->AddCircleFilled(c, r, theme::mix(P.dark ? P.field : P.surface700, P.led, 0.12f), 16);
    dl->AddCircle(c, r, rim, 16, 1.0f);
    return;
  }
  const float lv = std::clamp(level, 0.0f, 1.0f);
  if (lv > 0.05f) dl->AddCircleFilled(c, r + 2.5f, theme::withAlpha(P.led, 0.22f * std::min(1.0f, lv * 2)), 16);
  dl->AddCircleFilled(c, r, theme::mix(theme::mix(P.led, P.field, P.dark ? 0.25f : 0.05f), theme::mix(P.led, P.paper, 0.35f), lv), 16);
  dl->AddCircle(c, r, P.dark ? theme::withAlpha(P.ink, 0.45f) : theme::mix(P.led, P.ink, 0.35f), 16, 1.0f);
  dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.32f, theme::withAlpha(P.paper, 0.35f), 8);
}

void StepPad(ImDrawList* dl, ImVec2 a, ImVec2 b, bool on, bool alt, bool hovered, ImU32 tint, float vel) {
  const auto& P = pal();
  const float r = std::min(theme::kRadius, (b.x - a.x) * 0.25f);
  if (!on) {
    ImU32 body = alt ? P.stepOffAlt : P.stepOff;
    if (hovered) body = theme::mix(body, P.text, 0.14f);
    dl->AddRectFilled(a, b, theme::mix(body, P.ink, 0.28f), r);  // shadow edge
    dl->AddRectFilled(a, ImVec2(b.x, b.y - 1), body, r);
    dl->AddLine(ImVec2(a.x + r, a.y + 0.5f), ImVec2(b.x - r, a.y + 0.5f), theme::withAlpha(P.paper, P.dark ? 0.10f : 0.5f));
    return;
  }
  // Lit pad: light face, slightly tinted by the channel colour; brightness follows velocity.
  ImU32 face = theme::mix(P.stepOn, tint, P.dark ? 0.22f : 0.5f);
  face = theme::mix(theme::mix(face, alt ? P.stepOffAlt : P.stepOff, 0.35f), face, 0.45f + 0.55f * std::clamp(vel, 0.0f, 1.0f));
  if (hovered) face = theme::mix(face, P.paper, 0.15f);
  dl->AddRectFilled(a, b, theme::mix(face, P.ink, 0.35f), r);
  dl->AddRectFilled(a, ImVec2(b.x, b.y - 1), face, r);
  dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + std::max(2.0f, (b.y - a.y) * 0.35f)), theme::withAlpha(P.paper, 0.22f), r);
}

void Lcd(ImDrawList* dl, ImVec2 p, ImVec2 size, const char* text, bool hovered, bool active) {
  const auto& P = pal();
  App& a = app();
  const ImU32 lcdBg = P.dark ? P.field : P.surface950;
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), active ? P.surface700 : hovered ? theme::mix(lcdBg, P.surface800, 0.5f) : lcdBg,
                    theme::kRadius);
  dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), active ? P.accent : P.line, theme::kRadius);
  float fs = std::min(theme::kFontLcd, size.y - 8);
  ImVec2 ts = a.fontMonoBold->CalcTextSizeA(fs, 1e9f, 0, text);
  // Shrink to fit narrow (compact) displays instead of spilling over the frame.
  const float room = size.x - 12.0f;
  if (ts.x > room && ts.x > 0) {
    fs = std::max(9.0f, std::floor(fs * room / ts.x));
    ts = a.fontMonoBold->CalcTextSizeA(fs, 1e9f, 0, text);
  }
  dl->AddText(a.fontMonoBold, fs, ImVec2(std::round(p.x + (size.x - ts.x) * 0.5f), std::round(p.y + (size.y - ts.y) * 0.5f)),
              P.text, text);
}

const uint32_t* contentPalette(int* count) {
  static const uint32_t pal[] = {0x5A8FD8, 0x4FB8B0, 0x6EBF6A, 0xA9C24B, 0xC9A43C, 0xD8775A, 0xE5484D, 0xD85A8F,
                                 0xA56CD8, 0x7A6FE0, 0x8FA0B5, 0x9A9A9A, 0x6B6B6B, 0x50565E, 0xBDBDBD, 0xEDEDED};
  *count = static_cast<int>(sizeof pal / sizeof pal[0]);
  return pal;
}

bool ColorSwatch(const char* id, uint32_t* rgb, ImVec2 size) {
  ImGui::PushID(id);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton("##sw", size);
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), theme::rgb(*rgb), 3);
  if (ImGui::IsItemHovered())
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), pal().text, 3);
  if (clicked) ImGui::OpenPopup("##pal");
  bool changed = false;
  if (ImGui::BeginPopup("##pal")) {
    int n = 0;
    const uint32_t* cols = contentPalette(&n);
    for (int i = 0; i < n; ++i) {
      if (i % 8) ImGui::SameLine(0, 4);
      ImGui::PushID(i);
      ImVec2 q = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("c", ImVec2(22, 22))) {
        *rgb = cols[i];
        changed = true;
        ImGui::CloseCurrentPopup();
      }
      ImGui::GetWindowDrawList()->AddRectFilled(q, ImVec2(q.x + 22, q.y + 22), theme::rgb(cols[i]), 4);
      if (cols[i] == *rgb) ImGui::GetWindowDrawList()->AddRect(q, ImVec2(q.x + 22, q.y + 22), pal().text, 4, 2);
      ImGui::PopID();
    }
    float f[3] = {((*rgb >> 16) & 0xFF) / 255.f, ((*rgb >> 8) & 0xFF) / 255.f, (*rgb & 0xFF) / 255.f};
    ImGui::SetNextItemWidth(8 * 26.0f);
    if (ImGui::ColorEdit3("##custom", f, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
      *rgb = (static_cast<uint32_t>(f[0] * 255 + 0.5f) << 16) | (static_cast<uint32_t>(f[1] * 255 + 0.5f) << 8) |
             static_cast<uint32_t>(f[2] * 255 + 0.5f);
      changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", i18n::tr("Custom"));
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

void NextFloatingWindowSize(float width) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float maxH = std::max(200.0f, vp->WorkSize.y - theme::kTopBarH - theme::kStatusBarH - 2 * theme::kS2);
  // Room for a vertical scrollbar so a scrolling editor never clips sideways.
  const float w = width + ImGui::GetStyle().ScrollbarSize;
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, maxH));
}

void KeepWindowInViewport() { TrackWindowInViewport(); }  // J-91: clamped before layout (DialogLayout.h)

}  // namespace gl::ui

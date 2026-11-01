// Gridline — "edison" pack: Loupe's controls (J-207). See edison_style.h.
#include "edison_style.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "edison_ui.h"

namespace gl::packs::edison::sty {

float Metrics::px(float v) const { return std::round(v * s); }
float Metrics::font(float v) const { return std::max(8.0f, std::round(v * s * 2.0f) * 0.5f); }

Metrics metricsFor(float width) {
  Metrics m;
  m.s = std::clamp(width / kDesignW, 0.6f, 1.0f);
  return m;
}

namespace {

ImU32 litCol(const Btn& o) { return o.litRole ? C(o.litRole) : C(col::Lit); }

// One face for every Loupe button (shared skin faces): raised at rest, lighter
// on hover, recessed while pressed, tinted with the lit colour when latched.
void face(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hov, bool act, const Btn& o) {
  const float r = kRadius;
  if (o.flat && !hov && !act) return;  // flat: lit shows in the glyph only
  if (act && o.enabled) {
    skin::insetField(dl, a, b, C(col::BtnLo), r);
    return;
  }
  ImU32 bg = hov && o.enabled ? C(col::BtnHi) : C(col::Btn);
  if (o.lit) bg = theme::mix(bg, litCol(o), 0.14f);
  skin::raisedFace(dl, a, b, bg, r);
}

ImU32 glyphCol(bool hov, const Btn& o) {
  if (!o.enabled) return C(col::GlyphDim, 0.45f);
  if (o.lit) return litCol(o);
  return hov ? C(col::Text) : C(col::Glyph);
}

// Lit mark: a short bar under the glyph (the latched state reads without colour too).
void litMark(ImDrawList* dl, ImVec2 a, ImVec2 b, const Btn& o) {
  if (!o.lit || o.flat || !o.mark) return;
  const float w = std::round((b.x - a.x) * 0.4f), x = std::round((a.x + b.x - w) * 0.5f);
  dl->AddRectFilled(ImVec2(x, b.y - 4), ImVec2(x + w, b.y - 2), litCol(o), 1.0f);
}

bool item(const char* id, ImVec2 a, ImVec2 b, const Btn& o, bool& hov, bool& act) {
  ImGui::SetCursorScreenPos(a);
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  hov = ImGui::IsItemHovered();
  act = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool clicked = o.enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
  if (o.right) *o.right = o.enabled && ImGui::IsItemClicked(ImGuiMouseButton_Right);
  if (o.hint && *o.hint) {
    ui::Hint(o.hint);
    ui::Tooltip(o.hint);
  }
  return clicked;
}

// ---- dev rect dump
struct NoteRow {
  std::string group, name;
  float x0, y0, x1, y1;
};
std::vector<NoteRow> gNotes;
const char* rectsPath() {
  static const char* p = std::getenv("GRIDLINE_EDISON_RECTS");
  return p && *p ? p : nullptr;
}

}  // namespace

bool iconButton(const char* id, ImVec2 a, ImVec2 b, ui::Icon icon, float iconPx, const Btn& o) {
  bool hov = false, act = false;
  const bool clicked = item(id, a, b, o, hov, act);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  face(dl, a, b, hov, act, o);
  const ImVec2 c(std::round((a.x + b.x) * 0.5f), std::round((a.y + b.y) * 0.5f - (o.lit && !o.flat && o.mark ? 1.0f : 0.0f)));
  ui::drawIcon(dl, icon, c, iconPx, glyphCol(hov, o));
  litMark(dl, a, b, o);
  return clicked;
}

bool textButton(const char* id, ImVec2 a, ImVec2 b, const char* label, ImFont* font, float fs, const Btn& o) {
  bool hov = false, act = false;
  const bool clicked = item(id, a, b, o, hov, act);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  face(dl, a, b, hov, act, o);
  const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, ui::labelText(label));
  const ImU32 c = !o.enabled ? C(col::TextDim, 0.6f) : o.lit ? litCol(o) : hov ? C(col::Text) : C(col::Text, 0.88f);
  ui::drawLabel(dl, font, fs, ImVec2(std::round((a.x + b.x - ts.x) * 0.5f), std::round((a.y + b.y - ts.y) * 0.5f)), c, label);
  if (!o.flat) litMark(dl, a, b, o);
  return clicked;
}

int segmented(const char* id, ImVec2 a, ImVec2 b, const char* const* labels, int n, int sel, ImFont* font, float fs,
              const char* const* hints) {
  ImGui::PushID(id);
  int clicked = -1;
  const float w = b.x - a.x;
  for (int i = 0; i < n; ++i) {
    const float x0 = std::round(a.x + w * static_cast<float>(i) / static_cast<float>(n));
    const float x1 = std::round(a.x + w * static_cast<float>(i + 1) / static_cast<float>(n));
    Btn o;
    o.lit = i == sel;
    o.hint = hints ? hints[i] : nullptr;
    char cid[16];
    std::snprintf(cid, sizeof cid, "##s%d", i);
    // Cells of a switch touch: one control, one face per cell, a hairline between.
    if (textButton(cid, ImVec2(x0, a.y), ImVec2(x1, b.y), labels[i], font, fs, o)) clicked = i;
  }
  ImGui::PopID();
  return clicked;
}

void field(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* caption, const char* value, ImU32 valueCol, ImFont* font, float fs,
           float capFs, int align) {
  skin::insetField(dl, a, b, C(col::Lcd), kRadius);
  const float cy = (a.y + b.y) * 0.5f, inset = std::round(std::max(6.0f, (b.y - a.y) * 0.3f));
  dl->PushClipRect(a, b, true);
  float x0 = a.x + inset;
  if (caption && *caption) {
    textAt(dl, font, capFs, x0, cy, C(col::TextDim), caption, 0);
    x0 += textW(font, capFs, caption) + inset;
  }
  if (value && *value) {
    if (align == 0)
      textAt(dl, font, fs, x0, cy, valueCol, value, 0);
    else if (align == 1)
      textAt(dl, font, fs, (a.x + b.x) * 0.5f, cy, valueCol, value, 1);
    else
      textAt(dl, font, fs, b.x - inset, cy, valueCol, value, 2);
  }
  dl->PopClipRect();
}

bool dropField(const char* id, ImVec2 a, ImVec2 b, const char* value, ImFont* font, float fs, bool enabled, const char* hint,
               ui::Icon lead) {
  Btn o;
  o.enabled = enabled;
  o.hint = hint;
  bool hov = false, act = false;
  const bool clicked = item(id, a, b, o, hov, act);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, a, b, hov && enabled ? skin::lighten(C(col::Lcd), 0.06f) : C(col::Lcd), kRadius);
  const float h = b.y - a.y, cy = (a.y + b.y) * 0.5f, inset = std::round(std::max(6.0f, h * 0.3f));
  const float chev = std::round(h * 0.5f);
  float x = a.x + inset;
  if (lead != ui::Icon::Count) {
    ui::drawIcon(dl, lead, ImVec2(std::round(x + chev * 0.5f), std::round(cy)), chev, enabled ? C(col::Glyph) : C(col::GlyphDim, 0.5f));
    x += chev + std::round(inset * 0.6f);
  }
  dl->PushClipRect(a, ImVec2(b.x - chev - inset, b.y), true);
  textAt(dl, font, fs, x, cy, enabled ? C(col::LcdText) : C(col::TextDim), value, 0);
  dl->PopClipRect();
  ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(std::round(b.x - inset - chev * 0.5f + 2), std::round(cy)), chev,
               enabled ? (hov ? C(col::Text) : C(col::Glyph)) : C(col::GlyphDim, 0.5f));
  return clicked;
}

bool check(const char* id, ImVec2 tl, float box, float w, const char* cap, bool on, bool enabled, ImFont* font, float fs, const char* hint) {
  Btn o;
  o.enabled = enabled;
  o.hint = hint;
  bool hov = false, act = false;
  const bool clicked = item(id, tl, ImVec2(tl.x + w, tl.y + box), o, hov, act);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 b(tl.x + box, tl.y + box);
  skin::insetField(dl, tl, b, hov && enabled ? skin::lighten(C(col::Lcd), 0.08f) : C(col::Lcd), 2.0f);
  if (on) {
    const float in = std::round(box * 0.25f);
    dl->AddRectFilled(ImVec2(tl.x + in, tl.y + in), ImVec2(b.x - in, b.y - in), enabled ? C(col::Led) : C(col::Led, 0.4f), 1.0f);
  }
  textAt(dl, font, fs, b.x + std::round(box * 0.5f), (tl.y + b.y) * 0.5f,
         !enabled ? C(col::TextDim, 0.6f) : on ? C(col::Text) : hov ? C(col::Text, 0.9f) : C(col::TextDim), cap, 0);
  return clicked;
}

void caption(ImDrawList* dl, float x, float cy, const char* text, ImU32 c, ImFont* font, float fs) { textAt(dl, font, fs, x, cy, c, text, 0); }

float textW(ImFont* font, float fs, const char* text) { return font->CalcTextSizeA(fs, FLT_MAX, 0, ui::labelText(text)).x; }

void textAt(ImDrawList* dl, ImFont* font, float fs, float x, float cy, ImU32 c, const char* text, int align) {
  if (!text || !*text) return;
  const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, ui::labelText(text));
  const float tx = align == 0 ? x : align == 1 ? x - ts.x * 0.5f : x - ts.x;
  ui::drawLabel(dl, font, fs, ImVec2(std::round(tx), std::round(cy - ts.y * 0.5f)), c, text);
}

void note(const char* group, const char* name, ImVec2 a, ImVec2 b) {
  if (!rectsPath()) return;
  gNotes.push_back(NoteRow{group, name, a.x, a.y, b.x, b.y});
}

void frameEnd() {
  const char* path = rectsPath();
  if (!path) return;
  static bool written = false;
  if (!written && ImGui::GetFrameCount() >= 25 && !gNotes.empty()) {
    if (FILE* f = std::fopen(path, "w")) {
      std::fprintf(f, "group\tname\tx0\ty0\tx1\ty1\n");
      for (const auto& n : gNotes) std::fprintf(f, "%s\t%s\t%.2f\t%.2f\t%.2f\t%.2f\n", n.group.c_str(), n.name.c_str(), n.x0, n.y0, n.x1, n.y1);
      std::fclose(f);
      written = true;
    }
  }
  gNotes.clear();
}

}  // namespace gl::packs::edison::sty

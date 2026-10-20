#include "Widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../packs/modulation/modulation_ui.h"
#include "App.h"
#include "Windows.h"
#include "Theme.h"
#include "../core/I18n.h"

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

// J-55: hints, tooltips, labels, section captions, button and tab texts are
// translated here, so callers pass plain English.
void Hint(const std::string& text) {
  if (HintHovered()) app().setHint(tr(text.c_str()));
}
void Hint(const char* text) {
  if (text && HintHovered()) app().setHint(tr(text));
}
bool HintHovered() { return ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem); }

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

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col) {
  const float h = s * 0.5f, th = std::max(1.5f, s * 0.11f);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
  switch (icon) {
    case Icon::Play: dl->AddTriangleFilled(P(-0.55f, -0.7f), P(0.75f, 0), P(-0.55f, 0.7f), col); break;
    case Icon::Pause:
      dl->AddRectFilled(P(-0.6f, -0.65f), P(-0.15f, 0.65f), col, 1.5f);
      dl->AddRectFilled(P(0.15f, -0.65f), P(0.6f, 0.65f), col, 1.5f);
      break;
    case Icon::Stop: dl->AddRectFilled(P(-0.6f, -0.6f), P(0.6f, 0.6f), col, 2.0f); break;
    case Icon::Record: dl->AddCircleFilled(c, h * 0.62f, col, 24); break;
    case Icon::Plus:
      dl->AddLine(P(-0.6f, 0), P(0.6f, 0), col, th);
      dl->AddLine(P(0, -0.6f), P(0, 0.6f), col, th);
      break;
    case Icon::Minus: dl->AddLine(P(-0.6f, 0), P(0.6f, 0), col, th); break;
    case Icon::Close:
      dl->AddLine(P(-0.5f, -0.5f), P(0.5f, 0.5f), col, th);
      dl->AddLine(P(0.5f, -0.5f), P(-0.5f, 0.5f), col, th);
      break;
    case Icon::Pattern:  // 2x2 cells
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
          dl->AddRectFilled(P(-0.7f + x * 0.75f, -0.7f + y * 0.75f), P(-0.05f + x * 0.75f, -0.05f + y * 0.75f), col, 1.5f);
      break;
    case Icon::Song:  // stacked lanes
      for (int y = 0; y < 3; ++y) dl->AddRectFilled(P(-0.75f + y * 0.25f, -0.7f + y * 0.5f), P(0.75f - (2 - y) * 0.2f, -0.35f + y * 0.5f), col, 1.5f);
      break;
    case Icon::Draw:
      dl->AddLine(P(-0.6f, 0.6f), P(0.5f, -0.5f), col, th * 1.4f);
      dl->AddTriangleFilled(P(-0.75f, 0.75f), P(-0.45f, 0.65f), P(-0.65f, 0.45f), col);
      break;
    case Icon::Paint:
      dl->AddRectFilled(P(-0.7f, -0.7f), P(0.5f, -0.15f), col, 2);
      dl->AddLine(P(0.5f, -0.4f), P(0.75f, -0.4f), col, th);
      dl->AddLine(P(0.75f, -0.4f), P(0.75f, 0.05f), col, th);
      dl->AddLine(P(0.75f, 0.05f), P(-0.05f, 0.05f), col, th);
      dl->AddRectFilled(P(-0.17f, 0.05f), P(0.07f, 0.75f), col, 1);
      break;
    case Icon::Erase:
    case Icon::Trash:
      dl->AddRect(P(-0.5f, -0.45f), P(0.5f, 0.75f), col, 2, th);
      dl->AddLine(P(-0.7f, -0.55f), P(0.7f, -0.55f), col, th);
      dl->AddLine(P(-0.2f, -0.75f), P(0.2f, -0.75f), col, th);
      break;
    case Icon::Select:
      for (int i = 0; i < 4; ++i) {
        float a = -0.7f + i * 0.4f;
        dl->AddLine(P(a, -0.7f), P(a + 0.2f, -0.7f), col, th);
        dl->AddLine(P(a, 0.7f), P(a + 0.2f, 0.7f), col, th);
        dl->AddLine(P(-0.7f, a), P(-0.7f, a + 0.2f), col, th);
        dl->AddLine(P(0.7f, a), P(0.7f, a + 0.2f), col, th);
      }
      break;
    case Icon::Slice:
      dl->AddLine(P(0.4f, -0.8f), P(-0.4f, 0.8f), col, th);
      dl->AddRect(P(-0.7f, -0.3f), P(0.7f, 0.3f), col, 2, th * 0.7f);
      break;
    case Icon::Mute:
      dl->AddTriangleFilled(P(-0.7f, -0.25f), P(-0.1f, -0.7f), P(-0.1f, 0.7f), col);
      dl->AddRectFilled(P(-0.75f, -0.25f), P(-0.35f, 0.25f), col);
      dl->AddLine(P(0.2f, -0.35f), P(0.75f, 0.35f), col, th);
      dl->AddLine(P(0.75f, -0.35f), P(0.2f, 0.35f), col, th);
      break;
    case Icon::Metronome:
      dl->AddTriangle(P(-0.55f, 0.75f), P(0.55f, 0.75f), P(0, -0.75f), col, th);
      dl->AddLine(P(0, 0.4f), P(0.55f, -0.45f), col, th);
      break;
    case Icon::Loop:
      arc(dl, c, h * 0.62f, -kPi * 0.95f, kPi * 0.55f, col, th);
      dl->AddTriangleFilled(P(-0.85f, -0.05f), P(-0.35f, -0.05f), P(-0.6f, -0.45f), col);
      break;
    case Icon::Undo:
    case Icon::Redo: {
      float d = icon == Icon::Undo ? 1.f : -1.f;
      arc(dl, P(0.05f * -d, 0.15f), h * 0.55f, icon == Icon::Undo ? -kPi * 0.95f : -kPi * 0.05f,
          icon == Icon::Undo ? kPi * 0.25f : kPi * 1.25f, col, th);
      dl->AddTriangleFilled(P(-0.8f * d, -0.35f), P(-0.25f * d, -0.35f), P(-0.55f * d, 0.15f), col);
      break;
    }
    case Icon::Save:
      dl->AddRect(P(-0.7f, -0.7f), P(0.7f, 0.7f), col, 2, th);
      dl->AddRectFilled(P(-0.4f, -0.7f), P(0.35f, -0.2f), col);
      dl->AddRect(P(-0.4f, 0.15f), P(0.4f, 0.7f), col, 1, th * 0.8f);
      break;
    case Icon::Open:
    case Icon::Folder:
      dl->AddRectFilled(P(-0.75f, -0.6f), P(-0.1f, -0.35f), col, 1.5f);
      dl->AddRect(P(-0.75f, -0.4f), P(0.75f, 0.65f), col, 2, th);
      break;
    case Icon::NewFile:
    case Icon::File:
      dl->AddRect(P(-0.55f, -0.75f), P(0.55f, 0.75f), col, 2, th);
      dl->AddLine(P(-0.25f, -0.2f), P(0.25f, -0.2f), col, th * 0.8f);
      dl->AddLine(P(-0.25f, 0.15f), P(0.25f, 0.15f), col, th * 0.8f);
      break;
    case Icon::Keyboard:
      dl->AddRect(P(-0.8f, -0.55f), P(0.8f, 0.55f), col, 2, th);
      for (int i = -1; i <= 1; ++i) dl->AddLine(P(i * 0.4f, -0.55f), P(i * 0.4f, 0.55f), col, th * 0.7f);
      for (int i = -1; i <= 1; i += 2) dl->AddRectFilled(P(i * 0.4f - 0.12f, -0.55f), P(i * 0.4f + 0.12f, 0.05f), col);
      break;
    case Icon::Browser:
      for (int i = 0; i < 4; ++i) {
        float y = -0.65f + i * 0.43f;
        dl->AddRectFilled(P(-0.75f + (i % 2) * 0.3f, y - 0.07f), P(-0.5f + (i % 2) * 0.3f, y + 0.07f), col);
        dl->AddLine(P(-0.35f + (i % 2) * 0.3f, y), P(0.75f, y), col, th * 0.8f);
      }
      break;
    case Icon::Rack:
      for (int r = 0; r < 3; ++r)
        for (int x = 0; x < 4; ++x)
          dl->AddRectFilled(P(-0.8f + x * 0.42f, -0.7f + r * 0.5f), P(-0.5f + x * 0.42f, -0.4f + r * 0.5f), col, 1);
      break;
    case Icon::Piano:
      dl->AddRect(P(-0.75f, -0.75f), P(0.75f, 0.75f), col, 2, th);
      dl->AddRectFilled(P(-0.55f, -0.45f), P(0.0f, -0.2f), col);
      dl->AddRectFilled(P(-0.2f, 0.05f), P(0.55f, 0.3f), col);
      break;
    case Icon::Playlist:
      dl->AddRectFilled(P(-0.8f, -0.65f), P(0.1f, -0.25f), col, 1.5f);
      dl->AddRectFilled(P(-0.3f, -0.15f), P(0.8f, 0.25f), col, 1.5f);
      dl->AddRectFilled(P(-0.8f, 0.35f), P(0.4f, 0.75f), col, 1.5f);
      break;
    case Icon::Mixer:
      for (int i = 0; i < 3; ++i) {
        float x = -0.55f + i * 0.55f, y = i == 1 ? 0.25f : -0.25f;
        dl->AddLine(P(x, -0.75f), P(x, 0.75f), col, th * 0.8f);
        dl->AddRectFilled(P(x - 0.2f, y - 0.12f), P(x + 0.2f, y + 0.12f), col, 1);
      }
      break;
    case Icon::Settings:
      dl->AddCircle(c, h * 0.35f, col, 16, th);
      for (int i = 0; i < 8; ++i) {
        float a = i * kPi / 4;
        dl->AddLine(ImVec2(c.x + std::cos(a) * h * 0.5f, c.y + std::sin(a) * h * 0.5f),
                    ImVec2(c.x + std::cos(a) * h * 0.8f, c.y + std::sin(a) * h * 0.8f), col, th * 1.3f);
      }
      break;
    case Icon::Audio:
    case Icon::Wave:
      for (int i = 0; i < 7; ++i) {
        float x = -0.75f + i * 0.25f, a = 0.2f + 0.55f * std::fabs(std::sin(i * 1.3f + 0.4f));
        dl->AddLine(P(x, -a), P(x, a), col, th);
      }
      break;
    case Icon::Plug:
      dl->AddRect(P(-0.5f, -0.2f), P(0.5f, 0.45f), col, 3, th);
      dl->AddLine(P(-0.22f, -0.75f), P(-0.22f, -0.2f), col, th);
      dl->AddLine(P(0.22f, -0.75f), P(0.22f, -0.2f), col, th);
      dl->AddLine(P(0, 0.45f), P(0, 0.8f), col, th);
      break;
    case Icon::Up: dl->AddTriangleFilled(P(-0.6f, 0.35f), P(0.6f, 0.35f), P(0, -0.4f), col); break;
    case Icon::Down: dl->AddTriangleFilled(P(-0.6f, -0.35f), P(0.6f, -0.35f), P(0, 0.4f), col); break;
    case Icon::Left: dl->AddTriangleFilled(P(0.35f, -0.6f), P(0.35f, 0.6f), P(-0.4f, 0), col); break;
    case Icon::Right: dl->AddTriangleFilled(P(-0.35f, -0.6f), P(-0.35f, 0.6f), P(0.4f, 0), col); break;
    case Icon::Magnet:
      arc(dl, P(0, 0.05f), h * 0.5f, 0, kPi, col, th * 1.6f);
      dl->AddLine(P(-0.5f, 0.05f), P(-0.5f, -0.6f), col, th * 1.6f);
      dl->AddLine(P(0.5f, 0.05f), P(0.5f, -0.6f), col, th * 1.6f);
      break;
    case Icon::Menu:
      for (int i = -1; i <= 1; ++i) dl->AddLine(P(-0.65f, i * 0.45f), P(0.65f, i * 0.45f), col, th);
      break;
    case Icon::Zoom:
      dl->AddCircle(P(-0.15f, -0.15f), h * 0.45f, col, 16, th);
      dl->AddLine(P(0.2f, 0.2f), P(0.7f, 0.7f), col, th * 1.4f);
      break;
    case Icon::Help:
      dl->AddCircle(c, h * 0.75f, col, 20, th);
      dl->AddText(nullptr, s * 0.7f, ImVec2(c.x - s * 0.17f, c.y - s * 0.38f), col, "?");
      break;
    case Icon::Clone:
      dl->AddRect(P(-0.7f, -0.7f), P(0.3f, 0.3f), col, 2, th);
      dl->AddRect(P(-0.3f, -0.3f), P(0.7f, 0.7f), col, 2, th);
      break;
    case Icon::More:
      for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(P(i * 0.55f, 0), std::max(1.3f, s * 0.085f), col, 12);
      break;
    case Icon::Search:
      dl->AddCircle(P(-0.12f, -0.12f), h * 0.5f, col, 20, th);
      dl->AddLine(P(0.28f, 0.28f), P(0.72f, 0.72f), col, th * 1.2f);
      break;
    case Icon::Chevron:
      dl->AddLine(P(-0.2f, -0.5f), P(0.25f, 0), col, th);
      dl->AddLine(P(0.25f, 0), P(-0.2f, 0.5f), col, th);
      break;
    case Icon::ChevronDown:
      dl->AddLine(P(-0.5f, -0.2f), P(0, 0.25f), col, th);
      dl->AddLine(P(0, 0.25f), P(0.5f, -0.2f), col, th);
      break;
    case Icon::Slip:  // content sliding inside a clip frame
      dl->AddRect(P(-0.8f, -0.55f), P(0.8f, 0.55f), col, 2, th * 0.8f);
      dl->AddLine(P(-0.45f, 0), P(0.45f, 0), col, th);
      dl->AddTriangleFilled(P(-0.6f, 0), P(-0.3f, -0.25f), P(-0.3f, 0.25f), col);
      dl->AddTriangleFilled(P(0.6f, 0), P(0.3f, -0.25f), P(0.3f, 0.25f), col);
      break;
    case Icon::Detune:  // a pitch bend: a note bar with a curved arrow
      dl->AddRectFilled(P(-0.8f, 0.35f), P(0.1f, 0.65f), col, 1);
      dl->AddBezierQuadratic(P(-0.2f, 0.2f), P(0.45f, 0.15f), P(0.55f, -0.55f), col, th);
      dl->AddTriangleFilled(P(0.55f, -0.8f), P(0.32f, -0.42f), P(0.78f, -0.42f), col);
      break;
    case Icon::Stamp:  // stacked chord notes
      for (int i = 0; i < 3; ++i) dl->AddRectFilled(P(-0.7f, -0.7f + i * 0.5f), P(0.7f, -0.42f + i * 0.5f), col, 1);
      break;
    case Icon::Lock:
      arc(dl, P(0, -0.2f), h * 0.36f, kPi, 2 * kPi, col, th);
      dl->AddLine(P(-0.36f, -0.2f), P(-0.36f, 0.05f), col, th);
      dl->AddLine(P(0.36f, -0.2f), P(0.36f, 0.05f), col, th);
      dl->AddRectFilled(P(-0.6f, 0.05f), P(0.6f, 0.75f), col, 2);
      break;
  }
}

bool IconButton(const char* id, Icon icon, bool active, float size, const char* hint, ImU32 activeCol, ImU32 idleCol) {
  ImGui::PushID(id);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool pressed = ImGui::InvisibleButton("##ib", ImVec2(size, size));
  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
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

bool ToggleIcon(const char* id, Icon icon, bool on, float size, const char* hint) {
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
  drawIcon(dl, icon, ImVec2(std::round(p.x + size * 0.5f), std::round(p.y + size * 0.5f - (on ? 1 : 0))),
           std::round(size * 0.52f), fg);
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

bool TextButton(const char* label, bool active, float width, const char* hint) {
  const auto& P = pal();
  if (active) {  // lit: raised face tinted by the lit-glyph colour, orange text (both themes)
    const float t = P.dark ? 0.16f : 0.14f;
    ImGui::PushStyleColor(ImGuiCol_Button, theme::vec(theme::mix(P.surface800, P.hot, t)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(theme::mix(P.surface700, P.hot, t + 0.02f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::vec(theme::mix(P.surface600, P.hot, t + 0.04f)));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.hot));
  }
  bool r = ImGui::Button(tr(label), ImVec2(width, 0));
  if (active) ImGui::PopStyleColor(4);
  if (hint) {
    Hint(hint);
    Tooltip(hint);
  }
  return r;
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
  ImGui::InvisibleButton("##knob", ImVec2(w, radius * 2.0f + labelH + 2));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = false;
  double n = d.toNormalized(*value);
  ImGuiIO& io = ImGui::GetIO();
  if (act && io.MouseDelta.y != 0 && !io.KeyAlt) {
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
  if (hov && io.MouseWheel != 0 && !act) {
    double step = d.kind == ParamKind::Choice
                      ? 1.0 / std::max<size_t>(1, d.choices.empty() ? 1 : d.choices.size() - 1)
                      : (io.KeyShift ? 0.005 : 0.03);
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
  dl->AddLine(ImVec2(c.x + dir.x * radius * 0.15f, c.y + dir.y * radius * 0.15f),
              ImVec2(c.x + dir.x * (radius - th - 3.0f), c.y + dir.y * (radius - th - 3.0f)), hov || act ? P.paper : P.text, 2.0f);
  if (label) {
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(labelFs, 1e9f, 0, label);
    dl->AddText(ImGui::GetFont(), labelFs, ImVec2(std::round(c.x - ts.x * 0.5f), p.y + radius * 2.0f + 2.0f), P.textDim, label);
  }
  std::string txt = d.name + ": " + d.format(*value);
  if (hov || act) app().setHint(txt + "   (drag · Shift fine · double-click reset · right-click type)");
  if (act) ImGui::SetTooltip("%s", d.format(*value).c_str());

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
  ImGui::PushID(id);
  const auto& P = pal();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##fader", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();
  double n = d->toNormalized(*v);
  bool changed = false;
  float capH = 18.0f, travel = size.y - capH;
  if (act && io.MouseDelta.y != 0) {
    double nn = std::clamp(n - io.MouseDelta.y / travel * (io.KeyShift ? 0.15 : 1.0), 0.0, 1.0);
    a.doc.setParam(pid, d->fromNormalized(nn), true);
    changed = true;
  }
  if (hov && io.MouseWheel != 0 && !act) {
    a.doc.setParam(pid, d->fromNormalized(std::clamp(n + io.MouseWheel * 0.02, 0.0, 1.0)), true);
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
    app().setHint(txt);
    if (act) ImGui::SetTooltip("%s", d->format(*getParam(a.doc.song(), pid)).c_str());
  }
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

bool BeginDockWindow(const char* name, bool* open, ImVec2 firstSize) {
  ImGui::SetNextWindowSize(firstSize, ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  bool r = ImGui::Begin(name, open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
  f->right = f->start.x + (width > 0 ? std::min(width, avail) : avail) - 1;  // border stays inside the clip rect
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
  // Reserve the full panel width (fill panels keep auto-resizing windows stable).
  ImGui::SetCursorScreenPos(ImVec2(f->start.x, mx.y));
  ImGui::Dummy(ImVec2(f->right - f->start.x, 0));
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

void KeepWindowInViewport() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  const float x0 = vp->WorkPos.x, x1 = vp->WorkPos.x + vp->WorkSize.x;
  const float y0 = vp->WorkPos.y + theme::kTopBarH, y1 = vp->WorkPos.y + vp->WorkSize.y - theme::kStatusBarH;
  ImVec2 p = pos;
  p.x = std::max(x0, std::min(p.x, x1 - size.x));
  p.y = std::max(y0, std::min(p.y, y1 - size.y));
  if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImGui::IsWindowFocused()) {
    // While the user drags the window, only keep its title bar reachable.
    p = pos;
    p.x = std::clamp(p.x, x0 - size.x + 80.0f, x1 - 80.0f);
    p.y = std::clamp(p.y, y0, y1 - 32.0f);
  }
  if (p.x != pos.x || p.y != pos.y) ImGui::SetWindowPos(p);
}

}  // namespace gl::ui

// Gridline — "models" pack: editor helpers (drawable bar editors, combos,
// knob rows, small graphs) shared by the pack's editors. UI thread only.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include "../../app/App.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../PacksUi.h"
#include "models.h"

namespace gl::packs::models::ui2 {

using theme::pal;

inline float val(const PluginState& s, int k, float d = 0) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : d;
}
inline int ival(const PluginState& s, int k, int d = 0) { return static_cast<int>(std::lround(val(s, k, static_cast<float>(d)))); }

// Choice parameter as a compact combo box.
inline void combo(App& a, const PluginTarget& t, int param, float width, const char* label = nullptr) {
  const PluginState* st = t.state(a.doc.song());
  const PluginInfo* info = t.info(a.doc.song());
  if (!st || !info || static_cast<size_t>(param) >= info->params.size()) return;
  const ParamDesc& d = info->params[static_cast<size_t>(param)];
  int cur = std::clamp(ival(*st, param), 0, static_cast<int>(d.choices.size()) - 1);
  ImGui::PushID(param);
  if (label) {
    ImGui::PushFont(a.fontMono, 10.0f);
    ImGui::TextColored(theme::vec(pal().textFaint), "%s", label);
    ImGui::PopFont();
  }
  ImGui::SetNextItemWidth(width);
  if (ImGui::BeginCombo("##c", d.choices[static_cast<size_t>(cur)].c_str())) {
    for (size_t i = 0; i < d.choices.size(); ++i)
      if (ImGui::Selectable(d.choices[i].c_str(), static_cast<int>(i) == cur)) a.doc.setParam(t.pid(param), static_cast<double>(i), false);
    ImGui::EndCombo();
  }
  ImGui::PopID();
}

// A row of knob cells.
inline void knobs(App& a, const PluginTarget& t, std::initializer_list<std::pair<int, const char*>> ks, float cell = 60, float radius = 14) {
  bool first = true;
  for (const auto& k : ks) {
    if (!first) ImGui::SameLine(0, 0);
    first = false;
    pui::knobCell(a, t, k.first, k.second, cell, radius);
  }
}

// Toggle as a small pill, vertically centred against a knob row.
inline void toggleCell(App& a, const PluginTarget& t, int param, const char* label, float cell = 60) {
  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(cell, 14));
  float x = ImGui::GetCursorPosX();
  ImGui::SetCursorPosX(x + 2);
  pui::toggleButton(a, t, param, label);
  ImGui::EndGroup();
}

// Small caption in the mono face.
inline void caption(App& a, const char* text, ImU32 col = 0) {
  ImGui::PushFont(a.fontMono, 10.0f);
  ImGui::TextColored(theme::vec(col ? col : pal().textFaint), "%s", text);
  ImGui::PopFont();
}

// Drawable bar editor over `count` consecutive 0..1 parameters starting at
// `base`. Drag to draw (strokes are interpolated), right-drag resets to `rest`.
// Returns the hovered bar (or -1).
inline int barEditor(App& a, const PluginTarget& t, const PluginState& st, int base, int count, ImVec2 size, const char* id,
                     ImU32 col, float rest = 1.0f, const char* hint = nullptr) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::PushID(id);
  ImGui::InvisibleButton("##bars", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), P.surface950, 6);
  for (int g = 1; g < 4; ++g) dl->AddLine(ImVec2(p.x, p.y + size.y * g / 4), ImVec2(p.x + size.x, p.y + size.y * g / 4), P.gridStep);
  float cw = size.x / static_cast<float>(count);
  int hovered = -1;
  ImVec2 m = ImGui::GetIO().MousePos;
  if (hov) hovered = std::clamp(static_cast<int>((m.x - p.x) / cw), 0, count - 1);
  for (int k = 0; k < count; ++k) {
    float v = std::clamp(val(st, base + k, rest), 0.0f, 1.0f);
    float x0 = p.x + k * cw;
    float h = v * (size.y - 6);
    ImU32 c = k == hovered ? P.paper : theme::withAlpha(col, 0.45f + 0.55f * v);
    dl->AddRectFilled(ImVec2(x0 + 1, p.y + size.y - 3 - h), ImVec2(x0 + cw - 1, p.y + size.y - 3), c, 1.5f);
  }
  static int lastK = -1;
  static float lastV = 0;
  if (act) {
    bool right = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    int k = std::clamp(static_cast<int>((m.x - p.x) / cw), 0, count - 1);
    float v = right ? rest : std::clamp(1 - (m.y - p.y - 3) / (size.y - 6), 0.0f, 1.0f);
    if (ImGui::IsItemActivated()) lastK = -1;
    PluginState ns = st;
    int k0 = lastK < 0 ? k : lastK;
    float v0 = lastK < 0 ? v : lastV;
    int lo = std::min(k0, k), hi = std::max(k0, k);
    for (int i = lo; i <= hi; ++i) {
      float f = hi == lo ? 1.0f : static_cast<float>(i - k0) / static_cast<float>(k - k0);
      ns.params[static_cast<size_t>(base + i)] = v0 + (v - v0) * f;
    }
    lastK = k;
    lastV = v;
    if (ns.params != st.params) setPluginState(a, t, ns, "draw levels", 0x4D4F444C00000000ull ^ (t.key() << 8) ^ static_cast<uint64_t>(base));
  }
  if (ImGui::IsItemDeactivated()) {
    a.doc.endGesture();
    lastK = -1;
  }
  if (hov) {
    char b[96];
    std::snprintf(b, sizeof b, "%s %d: %.0f%%   drag: draw · right-drag: reset", hint ? hint : "Bar", hovered + 1,
                  100.0f * std::clamp(val(st, base + hovered, rest), 0.0f, 1.0f));
    a.setHint(b);
  }
  ImGui::PopID();
  return hovered;
}

// Sets several parameters as one undo step.
inline void setParams(App& a, const PluginTarget& t, std::initializer_list<std::pair<int, float>> kv, const char* label) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return;
  PluginState ns = *st;
  for (const auto& p : kv)
    if (p.first >= 0 && static_cast<size_t>(p.first) < ns.params.size()) ns.params[static_cast<size_t>(p.first)] = p.second;
  setPluginState(a, t, ns, label);
}

// Graph frame: dark panel with optional mono label; returns draw origin.
inline ImVec2 graphFrame(ImVec2 size, const char* id, const char* label, App& a) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), pal().surface950, 6);
  if (label) {
    ImGui::PushFont(a.fontMono, 10.0f);
    dl->AddText(ImVec2(p.x + 8, p.y + 5), pal().textFaint, label);
    ImGui::PopFont();
  }
  return p;
}

// Log-frequency axis labels along the bottom of a graph (lo..hi Hz).
inline void freqLabels(App& a, ImDrawList* dl, ImVec2 p, ImVec2 size, double lo, double hi) {
  static const double marks[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
  ImGui::PushFont(a.fontMono, 9.5f);
  for (double hz : marks) {
    if (hz < lo || hz > hi) continue;
    float x = p.x + static_cast<float>(std::log(hz / lo) / std::log(hi / lo)) * size.x;
    dl->AddLine(ImVec2(x, p.y + 4), ImVec2(x, p.y + size.y - 4), pal().gridStep);
    char b[16];
    if (hz >= 1000)
      std::snprintf(b, sizeof b, "%.0fk", hz / 1000);
    else
      std::snprintf(b, sizeof b, "%.0f", hz);
    dl->AddText(ImVec2(x + 3, p.y + size.y - 14), pal().textFaint, b);
  }
  ImGui::PopFont();
}

}  // namespace gl::packs::models::ui2

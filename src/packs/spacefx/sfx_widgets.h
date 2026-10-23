// Gridline — "spacefx" pack: editor building blocks (included by spacefx_ui.cpp only).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../../app/App.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/Params.h"

namespace gl::packs::spacefx::ui2 {

using theme::pal;

inline float val(const PluginState& s, int k) {
  audit::stateRead(k);  // J-92 binding audit
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.f; }
inline const ParamDesc* desc(App& a, const PluginTarget& t, int k) {
  const PluginInfo* info = t.info(a.doc.song());
  return info && static_cast<size_t>(k) < info->params.size() ? &info->params[static_cast<size_t>(k)] : nullptr;
}
inline std::string fmt(App& a, const PluginTarget& t, int k) {
  const ParamDesc* d = desc(a, t, k);
  const PluginState* st = t.state(a.doc.song());
  return d && st ? d->format(val(*st, k)) : std::string();
}
inline uint64_t gestureKey(const PluginTarget& t, uint64_t salt) { return 0x5346580000000000ull ^ (t.key() << 12) ^ salt; }

// Dark rounded graph background with a hairline frame.
inline void graphBg(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, P.surface950, 8);
  dl->AddRect(p0, p1, P.line, 8);
}

inline void smallText(App& a, ImDrawList* dl, ImVec2 p, ImU32 col, const char* txt, float size = 10.0f) {
  ImGui::PushFont(a.fontMono, size);
  dl->AddText(p, col, txt);
  ImGui::PopFont();
}

// Vertical fader with value above and label below, in a fixed-width cell.
inline void faderCell(App& a, const PluginTarget& t, int param, const char* label, float h = 104, float cell = 58) {
  ImGui::PushID(param);
  ImGui::BeginGroup();
  float x = ImGui::GetCursorPosX();
  std::string v = fmt(a, t, param);
  ImGui::PushFont(a.fontMono, 10.0f);
  ImVec2 ts = ImGui::CalcTextSize(v.c_str());
  ImGui::SetCursorPosX(x + std::max(0.f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(pal().text), "%s", v.c_str());
  ImGui::PopFont();
  ImGui::SetCursorPosX(x + (cell - 22) * 0.5f);
  ui::ParamFader("f", t.pid(param), ImVec2(22, h));
  ImGui::PushFont(nullptr, 12.0f);
  ts = ImGui::CalcTextSize(label);
  ImGui::SetCursorPosX(x + std::max(0.f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(pal().textDim), "%s", label);
  ImGui::PopFont();
  ImGui::SetCursorPosX(x);
  ImGui::Dummy(ImVec2(cell, 0));
  ImGui::EndGroup();
  ImGui::PopID();
}

// Row of knob cells (SameLine between them).
inline void knobRow(App& a, const PluginTarget& t, std::initializer_list<std::pair<int, const char*>> ks, float cell = 62, float r = 15) {
  bool first = true;
  for (const auto& k : ks) {
    if (!first) ImGui::SameLine(0, 0);
    first = false;
    pui::knobCell(a, t, k.first, k.second, cell, r);
  }
}

// Sets one parameter as part of a drag gesture (merged undo step).
inline void dragParam(App& a, const PluginTarget& t, int k, double v) { a.doc.setParam(t.pid(k), v, true); }

// 2-D pad bound to two parameters (x right, y up), FL-style XY controller.
inline void xyPad(App& a, const PluginTarget& t, int px, int py, ImVec2 size, const char* title) {
  const Song& s = a.doc.song();
  const PluginState* stp = t.state(s);
  const ParamDesc* dx = desc(a, t, px);
  const ParamDesc* dy = desc(a, t, py);
  if (!stp || !dx || !dy) return;
  const auto& P = pal();
  ImGui::PushID(px * 131 + py);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##xy", size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p.x + size.x, p.y + size.y);
  graphBg(dl, p, p1);
  for (int i = 1; i < 4; ++i) {
    float gx = p.x + size.x * i / 4.0f, gy = p.y + size.y * i / 4.0f;
    dl->AddLine(ImVec2(gx, p.y + 4), ImVec2(gx, p1.y - 4), P.gridStep);
    dl->AddLine(ImVec2(p.x + 4, gy), ImVec2(p1.x - 4, gy), P.gridStep);
  }
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemActive()) {
    double nx = std::clamp((io.MousePos.x - p.x - 6) / (size.x - 12), 0.0f, 1.0f);
    double ny = std::clamp(1.0f - (io.MousePos.y - p.y - 6) / (size.y - 12), 0.0f, 1.0f);
    PluginState st = *stp;
    st.params[static_cast<size_t>(px)] = static_cast<float>(dx->fromNormalized(nx));
    st.params[static_cast<size_t>(py)] = static_cast<float>(dy->fromNormalized(ny));
    setPluginState(a, t, st, std::string("edit ") + title, gestureKey(t, static_cast<uint64_t>(px * 977 + py)));
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    PluginState st = *stp;
    st.params[static_cast<size_t>(px)] = static_cast<float>(dx->def);
    st.params[static_cast<size_t>(py)] = static_cast<float>(dy->def);
    setPluginState(a, t, st, std::string("reset ") + title);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  const PluginState& cur = *t.state(a.doc.song());
  float cx = p.x + 6 + static_cast<float>(dx->toNormalized(val(cur, px))) * (size.x - 12);
  float cy = p.y + 6 + static_cast<float>(1.0 - dy->toNormalized(val(cur, py))) * (size.y - 12);
  dl->AddLine(ImVec2(cx, p.y + 2), ImVec2(cx, p1.y - 2), theme::withAlpha(P.text, 0.25f));
  dl->AddLine(ImVec2(p.x + 2, cy), ImVec2(p1.x - 2, cy), theme::withAlpha(P.text, 0.25f));
  bool hot = ImGui::IsItemActive() || ImGui::IsItemHovered();
  dl->AddCircleFilled(ImVec2(cx, cy), hot ? 6.f : 5.f, ImGui::IsItemActive() ? P.accent : P.paper);
  smallText(a, dl, ImVec2(p.x + 7, p.y + 5), P.textFaint, title);
  if (hot) a.setHint(std::string(title) + ": " + dx->name + " " + dx->format(val(cur, px)) + " · " + dy->name + " " + dy->format(val(cur, py)) +
                     "   (drag; double-click resets)");
  ImGui::PopID();
}

// Segmented buttons for a choice parameter with custom labels / width.
inline void choiceRow(App& a, const PluginTarget& t, int k, float width, const char* const* labels = nullptr) {
  const ParamDesc* d = desc(a, t, k);
  const PluginState* st = t.state(a.doc.song());
  if (!d || !st) return;
  int cur = static_cast<int>(std::lround(val(*st, k)));
  size_t n = d->choices.size();
  float w = (width - (n - 1) * 2.0f) / static_cast<float>(n);
  ImGui::PushID(k);
  for (size_t i = 0; i < n; ++i) {
    if (i) ImGui::SameLine(0, 2);
    ImGui::PushID(static_cast<int>(i));
    if (ui::TextButton(labels ? labels[i] : d->choices[i].c_str(), static_cast<int>(i) == cur, w)) a.doc.setParam(t.pid(k), static_cast<double>(i), false);
    ImGui::PopID();
  }
  ImGui::PopID();
}

// Compact "< value >" stepper for long choice lists (tempo divisions).
inline void stepper(App& a, const PluginTarget& t, int k) {
  const ParamDesc* d = desc(a, t, k);
  const PluginState* st = t.state(a.doc.song());
  if (!d || !st) return;
  int cur = static_cast<int>(std::lround(val(*st, k))), n = static_cast<int>(d->choices.size());
  ImGui::PushID(k);
  if (ui::TextButton("<", false, 20)) a.doc.setParam(t.pid(k), std::max(0, cur - 1), false);
  ImGui::SameLine(0, 2);
  ui::TextButton(d->format(cur).c_str(), true, 58);
  ImGui::SameLine(0, 2);
  if (ui::TextButton(">", false, 20)) a.doc.setParam(t.pid(k), std::min(n - 1, cur + 1), false);
  ImGui::PopID();
}

// Small mono caption inside the current window.
inline void caption(App& a, const char* text) {
  ImGui::PushFont(a.fontMono, 10.0f);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", text);
  ImGui::PopFont();
}

}  // namespace gl::packs::spacefx::ui2

// Gridline — "synths" pack: editor drawing helpers (Gridline monochrome style).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../engine/Dsp.h"
#include "synths.h"
#include "synths_common.h"

namespace gl::packs::synths::ui2 {

using theme::pal;

inline float val(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.f; }
inline int ival(const PluginState& s, int k) { return static_cast<int>(std::lround(val(s, k))); }

// Compact knob cell: knob + label; the label turns into the value while the
// knob is hovered or dragged (the hint bar shows it too). Followed by SameLine
// (the caller ends rows with ImGui::NewLine()).
inline void knob(App& a, const PluginTarget& t, int param, const char* label, float cell = 52, float radius = 12) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return;
  ImGui::PushID(param);
  ImGui::BeginGroup();
  float x = ImGui::GetCursorPosX();
  ImGui::SetCursorPosX(x + (cell - radius * 2) * 0.5f);
  bool bip = d->min < 0 && d->max > 0;
  gl::ui::ParamKnob("k", pid, radius, nullptr, bip);
  bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  std::string text = hot ? d->format(*getParam(s, pid)) : std::string(label ? label : d->name.c_str());
  ImGui::PushFont(hot ? a.fontMono : nullptr, hot ? 11.0f : 12.0f);
  ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(hot ? pal().text : pal().textDim), "%s", text.c_str());
  ImGui::PopFont();
  ImGui::SetCursorPosX(x);
  ImGui::Dummy(ImVec2(cell, 0));
  ImGui::EndGroup();
  ImGui::PopID();
  ImGui::SameLine(0, 0);
}

// Small caps label in the mono font.
inline void caption(App& a, const char* text) {
  ImGui::PushFont(a.fontMono, 10.0f);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", text);
  ImGui::PopFont();
}

// Choice parameter as a compact combo box.
inline void combo(App& a, const PluginTarget& t, int param, float width) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  if (!d) return;
  int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
  ImGui::PushID(param);
  ImGui::SetNextItemWidth(width);
  std::string preview = cur >= 0 && cur < static_cast<int>(d->choices.size()) ? d->choices[static_cast<size_t>(cur)] : "";
  if (ImGui::BeginCombo("##c", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
    for (size_t i = 0; i < d->choices.size(); ++i)
      if (ImGui::Selectable(d->choices[i].c_str(), static_cast<int>(i) == cur)) a.doc.setParam(pid, static_cast<double>(i), false);
    ImGui::EndCombo();
  }
  ui::Hint(d->name);
  ImGui::PopID();
}

// Toggle parameter as a small pill with a fixed width.
inline void toggle(App& a, const PluginTarget& t, int param, const char* label, float width = 0, const char* hint = nullptr) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  bool on = getParam(s, pid).value_or(0) >= 0.5;
  ImGui::PushID(param);
  if (ui::TextButton(label, on, width, hint)) a.doc.setParam(pid, on ? 0 : 1, false);
  ImGui::PopID();
}

// Segmented buttons for a choice parameter with explicit (short) labels.
inline void segmented(App& a, const PluginTarget& t, int param, const char* const* labels, int count, float width) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
  float w = (width - (count - 1) * 2.0f) / static_cast<float>(count);
  ImGui::PushID(param);
  for (int i = 0; i < count; ++i) {
    if (i) ImGui::SameLine(0, 2);
    ImGui::PushID(i);
    if (ui::TextButton(labels[i], i == cur, w)) a.doc.setParam(pid, i, false);
    ImGui::PopID();
  }
  ImGui::PopID();
}

// Local page tabs (UI state only).
inline void tabs(const char* id, const char* const* names, int count, int& sel, float width) {
  ImGui::PushID(id);
  float w = (width - (count - 1) * 2.0f) / static_cast<float>(count);
  for (int i = 0; i < count; ++i) {
    if (i) ImGui::SameLine(0, 2);
    ImGui::PushID(i);
    if (ui::TextButton(names[i], i == sel, w)) sel = i;
    ImGui::PopID();
  }
  ImGui::PopID();
}

// Waveform glyphs: 0 sine, 1 triangle, 2 saw (ramp down), 3 square/pulse, 4 noise,
// 5 ramp up, 6 ramp down, 7 step random, 8 smooth random.
inline void glyph(ImDrawList* dl, int kind, ImVec2 c, float w, float h, ImU32 col, float pw = 0.5f) {
  ImVec2 pts[40];
  int n = 0;
  auto at = [&](float ph, float y) { pts[n++] = ImVec2(c.x - w / 2 + w * ph, c.y - y * h / 2); };
  switch (kind) {
    case 0: for (int i = 0; i <= 32; ++i) at(i / 32.f, std::sin(i / 32.f * 6.2831853f)); break;
    case 1: at(0, 0); at(0.25f, 1); at(0.75f, -1); at(1, 0); break;
    case 2: at(0, 1); at(1, -1); at(1, 1); break;
    case 3: at(0, 1); at(pw, 1); at(pw, -1); at(1, -1); at(1, 1); break;
    case 4: for (int i = 0; i <= 24; ++i) at(i / 24.f, static_cast<float>(hashUnit(static_cast<uint32_t>(i * 977 + 5)))); break;
    case 5: at(0, -1); at(1, 1); at(1, -1); break;
    case 6: at(0, 1); at(1, -1); at(1, 1); break;
    case 7: {
      const float v[5] = {0.6f, -0.4f, 0.9f, -0.8f, 0.2f};
      for (int i = 0; i < 5; ++i) at(i / 5.f, v[i]), at((i + 1) / 5.f, v[i]);
      break;
    }
    default:
      for (int i = 0; i <= 32; ++i) {
        float x = i / 32.f;
        at(x, 0.7f * std::sin(x * 9.0f) * std::cos(x * 4.0f + 1.0f));
      }
      break;
  }
  dl->AddPolyline(pts, n, col, 0, 1.5f);
}

// Icon picker for a choice parameter; glyphs[i] picks the glyph of option i.
inline void glyphPicker(App& a, const PluginTarget& t, int param, const int* glyphs, int count, float bw = 28, float bh = 22) {
  const Song& s = a.doc.song();
  ParamId pid = t.pid(param);
  auto d = describeParam(s, pid);
  int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
  const auto& P = pal();
  ImGui::PushID(param);
  for (int i = 0; i < count; ++i) {
    if (i) ImGui::SameLine(0, 2);
    ImGui::PushID(i);
    ImVec2 q = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("g", ImVec2(bw, bh))) a.doc.setParam(pid, i, false);
    bool on = i == cur, hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + bh), on ? P.text : hov ? P.surface600 : P.surface700, 4);
    glyph(dl, glyphs[i], ImVec2(q.x + bw / 2, q.y + bh / 2), bw - 10, bh - 10, on ? P.ink : P.textDim);
    if (hov && d && i < static_cast<int>(d->choices.size())) a.setHint(d->name + ": " + d->choices[static_cast<size_t>(i)]);
    ImGui::PopID();
  }
  ImGui::PopID();
}

// Dark display well; returns its top-left. Reserves the space.
inline ImVec2 well(ImVec2 size, const char* id) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, size);
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), pal().surface950, 6);
  return p;
}

// One cycle of fn(phase) → [-1, 1] inside a well.
inline void cycleGraph(ImDrawList* dl, ImVec2 p, ImVec2 size, const std::function<double(double)>& fn, ImU32 col, float thick = 1.6f) {
  const int n = std::max(16, static_cast<int>(size.x) - 8);
  std::vector<ImVec2> pts(static_cast<size_t>(n));
  float midY = p.y + size.y * 0.5f, amp = size.y * 0.5f - 6;
  for (int i = 0; i < n; ++i) {
    double ph = static_cast<double>(i) / (n - 1);
    double y = std::clamp(fn(ph), -1.2, 1.2);
    pts[static_cast<size_t>(i)] = ImVec2(p.x + 4 + (size.x - 8) * static_cast<float>(ph), midY - static_cast<float>(y) * amp);
  }
  dl->AddLine(ImVec2(p.x + 4, midY), ImVec2(p.x + size.x - 4, midY), pal().gridStep);
  dl->AddPolyline(pts.data(), n, col, 0, thick);
}

// Filter magnitude response curve in a well (dB range ±24), with cutoff marker.
inline void filterCurve(ImDrawList* dl, ImVec2 p, ImVec2 size, int mode, double cutoff, double reso, ImU32 col, bool grid = true) {
  if (grid) pui::drawFreqGrid(dl, p, ImVec2(p.x + size.x, p.y + size.y), 24, false);
  std::vector<ImVec2> pts;
  pts.reserve(static_cast<size_t>(size.x));
  for (int x = 0; x < static_cast<int>(size.x); x += 2) {
    double hz = pui::xToFreq(p.x + x, p.x, size.x);
    double db = dsp::gainToDb(MultiFilter::magnitude(mode, hz, cutoff, reso, 48000));
    float y = p.y + size.y * 0.5f - static_cast<float>(std::clamp(db, -26.0, 26.0) / 24.0) * size.y * 0.5f;
    pts.push_back(ImVec2(p.x + x, y));
  }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, 1.8f);
  float cx = pui::freqToX(cutoff, p.x, size.x);
  dl->AddLine(ImVec2(cx, p.y + 4), ImVec2(cx, p.y + size.y - 4), theme::withAlpha(col, 0.35f));
}

// Drag a filter's cutoff (x) and resonance (y) on its response well.
inline void filterDrag(App& a, const PluginTarget& t, ImVec2 p, ImVec2 size, int cutParam, int resParam, uint64_t salt) {
  if (ImGui::IsItemActive()) {
    ImGuiIO& io = ImGui::GetIO();
    PluginState st = *t.state(a.doc.song());
    double hz = pui::xToFreq(io.MousePos.x, p.x, size.x);
    st.params[static_cast<size_t>(cutParam)] = static_cast<float>(std::clamp(hz, 20.0, 20000.0));
    st.params[static_cast<size_t>(resParam)] = static_cast<float>(std::clamp(1.0 - (io.MousePos.y - p.y) / size.y, 0.0, 1.0));
    setPluginState(a, t, st, "edit filter", 0x464C540000000000ull ^ t.key() ^ salt);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) a.setHint("Drag: cutoff (left/right) and resonance (up/down)");
}

// Vertical divider between groups on one row.
inline void divider(float h) {
  ImGui::SameLine(0, 6);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 4), ImVec2(p.x, p.y + h - 4), pal().line);
  ImGui::Dummy(ImVec2(1, h));
  ImGui::SameLine(0, 6);
}

// Live output oscilloscope (latest samples of the instrument tap), trigger-aligned.
inline void liveScope(App& a, const PluginTarget& t, ImVec2 size) {
  const auto& P = pal();
  ImVec2 p = well(size, "##scope");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  static float buf[2048];
  const ScopeTap* tap = t.tap(a);
  float midY = p.y + size.y * 0.5f;
  dl->AddLine(ImVec2(p.x + 4, midY), ImVec2(p.x + size.x - 4, midY), P.gridStep);
  if (!tap || !tap->read(buf, 2048)) return;
  int start = 0;
  for (int i = 1023; i > 1; --i)  // rising zero crossing in the older half
    if (buf[i - 1] <= 0 && buf[i] > 0) {
      start = i;
      break;
    }
  const int n = 900;
  float peak = 1e-3f;
  for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(buf[std::min(2047, start + i)]));
  float g = (size.y * 0.5f - 5) / std::max(peak, 0.25f);
  ImVec2 pts[180];
  for (int k = 0; k < 180; ++k) {
    float v = buf[std::min(2047, start + k * n / 180)];
    pts[k] = ImVec2(p.x + 4 + (size.x - 8) * k / 179.0f, midY - v * g);
  }
  dl->AddPolyline(pts, 180, P.text, 0, 1.4f);
}

// Effects strip body (drive, chorus, delay, reverb) for an FX block at `base`.
inline void fxBody(App& a, const PluginTarget& t, int base, float inner) {
  float cell = std::min(60.f, (inner - 60) / 10);
  const float rowH = 76;
  ImGui::BeginGroup();
  caption(a, "DRIVE");
  ImGui::Dummy(ImVec2(0, 14));
  knob(a, t, base + FxDrive, "Amount", cell);
  ImGui::EndGroup();
  divider(rowH);
  struct G {
    const char* name;
    int on, k1, k2, k3;
    const char *l1, *l2, *l3;
  };
  const G groups[3] = {{"Chorus", FxChoOn, FxChoRate, FxChoDepth, FxChoMix, "Rate", "Depth", "Mix"},
                       {"Delay", FxDlyOn, FxDlyDiv, FxDlyFb, FxDlyMix, "Time", "Feedback", "Mix"},
                       {"Reverb", FxRevOn, FxRevSize, FxRevDamp, FxRevMix, "Size", "Damping", "Mix"}};
  for (int g = 0; g < 3; ++g) {
    if (g) divider(rowH);
    ImGui::BeginGroup();
    toggle(a, t, base + groups[g].on, groups[g].name, cell * 3 - 4);
    knob(a, t, base + groups[g].k1, groups[g].l1, cell);
    knob(a, t, base + groups[g].k2, groups[g].l2, cell);
    knob(a, t, base + groups[g].k3, groups[g].l3, cell);
    ImGui::NewLine();
    ImGui::EndGroup();
  }
}

}  // namespace gl::packs::synths::ui2

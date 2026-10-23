// Gridline — "dynamics" pack: editor helpers shared by the pack's editors
// (meters, scrolling monitors, custom-labelled knobs, small graphs).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../engine/Program.h"
#include "dynamics.h"

namespace gl::packs::dynamics::ui {

using theme::pal;

inline float val(const PluginState& s, int k) {
  audit::stateRead(k);  // J-92 binding audit
  if (k >= 0 && static_cast<size_t>(k) < s.params.size()) return s.params[static_cast<size_t>(k)];
  const PluginInfo* info = findPlugin(s.type);
  return info && k >= 0 && static_cast<size_t>(k) < info->params.size() ? static_cast<float>(info->params[static_cast<size_t>(k)].def) : 0.f;
}
inline int ival(const PluginState& s, int k) { return static_cast<int>(std::lround(val(s, k))); }

// Live runtime of a mixer-slot effect (meters), or null.
inline std::shared_ptr<EffectRuntime> runtime(App& a, const PluginTarget& t) {
  if (t.isChannel()) return nullptr;
  return a.builder.effectRuntime(t.owner, t.slot);
}

// Sets one parameter (undoable). `drag` merges consecutive edits of a gesture.
inline void setParam(App& a, const PluginTarget& t, int k, double v, bool drag = false) {
  a.doc.setParam(t.pid(k), v, drag);
}

// Knob with an editor-supplied label and value text (for generic slots).
inline void knobText(App& a, const PluginTarget& t, int param, const char* label, const char* text, float cell = 64,
                     float radius = 16, bool bipolar = false) {
  const auto& P = pal();
  ImGui::PushID(param);
  ImGui::BeginGroup();
  float x = ImGui::GetCursorPosX();
  ImGui::SetCursorPosX(x + (cell - radius * 2) * 0.5f);
  gl::ui::ParamKnob("k", t.pid(param), radius, nullptr, bipolar);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImVec2 ts = ImGui::CalcTextSize(text);
  ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(P.text), "%s", text);
  ImGui::PopFont();
  ImGui::PushFont(nullptr, 12.0f);
  ts = ImGui::CalcTextSize(label);
  ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(P.textDim), "%s", label);
  ImGui::PopFont();
  ImGui::SetCursorPosX(x);
  ImGui::Dummy(ImVec2(cell, 0));
  ImGui::EndGroup();
  ImGui::PopID();
}

// Small mono caption.
inline void caption(App& a, ImDrawList* dl, ImVec2 p, ImU32 col, const char* text, float size = 10.0f) {
  ImGui::PushFont(a.fontMono, size);
  dl->AddText(p, col, text);
  ImGui::PopFont();
}

// Level bar (dBFS, bottom-up), for in/out meters.
inline void levelBar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float db, float floorDb = -60.0f, bool horizontal = false) {
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, P.surface950, 3);
  float f = std::clamp((db - floorDb) / -floorDb, 0.0f, 1.0f);
  ImU32 c = db > -0.5f ? P.danger : P.text;
  if (horizontal)
    dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 2), ImVec2(p0.x + 2 + f * (p1.x - p0.x - 4), p1.y - 2), theme::withAlpha(c, 0.85f), 2);
  else
    dl->AddRectFilled(ImVec2(p0.x + 2, p1.y - 2 - f * (p1.y - p0.y - 4)), ImVec2(p1.x - 2, p1.y - 2), theme::withAlpha(c, 0.85f), 2);
}

// Peak (linear) of the newest `n` samples of a tap.
inline float tapPeak(const ScopeTap* tap, size_t n = 1024) {
  if (!tap) return 0.f;
  static float buf[ScopeTap::kSize];
  if (!tap->read(buf, n)) return 0.f;
  float m = 0;
  for (size_t i = 0; i < n; ++i) m = std::max(m, std::fabs(buf[i]));
  return m;
}
inline float toDb(float lin) { return lin > 1e-6f ? 20.0f * std::log10(lin) : -120.0f; }

// Scrolling history of up to 6 traces, advanced at a fixed rate.
struct History {
  static constexpr int N = 512;
  static constexpr int kTraces = 6;
  float v[kTraces][N] = {};
  int head = 0;
  double last = -1;
  bool primed = false;
  // Pushes the current values; `rate` columns per second.
  bool due(double rate) {
    double now = ImGui::GetTime();
    if (last < 0) last = now;
    if (now - last < 1.0 / rate) return false;
    last += 1.0 / rate;
    if (now - last > 0.5) last = now;
    return true;
  }
  void push(const float* vals, int count) {
    if (!primed) {  // start flat at the first values (not at 0, which may mean 0 dB)
      for (int k = 0; k < kTraces; ++k) std::fill(v[k], v[k] + N, k < count ? vals[k] : 0.f);
      primed = true;
    }
    for (int k = 0; k < kTraces; ++k) v[k][head] = k < count ? vals[k] : 0.f;
    head = (head + 1) % N;
  }
  // Value of trace k, i columns back (0 = newest).
  float at(int k, int i) const { return v[k][((head - 1 - i) % N + N) % N]; }
};
inline History& history(uint64_t key) {
  static std::map<uint64_t, History> h;
  return h[key];
}

}  // namespace gl::packs::dynamics::ui

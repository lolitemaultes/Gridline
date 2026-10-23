// Gridline - lit keys and pad flashes (J-77).
#include "KeyLight.h"
#include <algorithm>
#include <unordered_map>
#include "App.h"
#include "SmoothScrollUi.h"
#include "Theme.h"

namespace gl::keylight {

namespace {
struct Entry {
  ChannelLights lights;
  int frame = -1;
  double time = 0;
};
std::unordered_map<ObjectId, Entry>& entries() {
  static std::unordered_map<ObjectId, Entry> e;
  return e;
}
}  // namespace

const ChannelLights* lights(App& a, ObjectId channel) {
  if (channel == kNoId) return nullptr;
  auto rt = a.builder.channelRuntime(channel);
  if (!rt) return nullptr;
  Entry& e = entries()[channel];
  const int frame = ImGui::GetFrameCount();
  if (e.frame != frame) {
    const double now = ImGui::GetTime();
    const float dt = e.frame < 0 ? 0.0f : static_cast<float>(std::clamp(now - e.time, 0.0, 0.25));
    e.lights.update(rt->keys, dt, scrollui::animations());  // J-91
    e.frame = frame;
    e.time = now;
  }
  return &e.lights;
}

float key(App& a, ObjectId channel, int k) {
  const ChannelLights* l = lights(a, channel);
  return l ? l->key(k) : 0.0f;
}

float flash(App& a, ObjectId channel, int k) {
  const ChannelLights* l = lights(a, channel);
  return l ? l->flash(k) : 0.0f;
}

float flashAny(App& a, ObjectId channel) {
  const ChannelLights* l = lights(a, channel);
  float m = 0;
  if (l)
    for (int k = 0; k < NoteActivity::kKeys; ++k) m = std::max(m, l->flash(k));
  return m;
}

void drawFlash(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float level, float rounding) {
  if (level <= 0.003f) return;
  const float l = std::min(1.0f, level);
  dl->AddRectFilled(p0, p1, theme::withAlpha(col, 0.72f * l), rounding);
  dl->AddRectFilled(p0, p1, theme::withAlpha(IM_COL32_WHITE, 0.12f * l * l), rounding);  // the hit's peak reads as light, not just colour
  // Brighter face towards the top (light falls on the pad) + a lit rim.
  const ImVec2 c0(p0.x + 2, p0.y + 2), c1(p1.x - 2, p0.y + (p1.y - p0.y) * 0.5f);
  const ImU32 hi = theme::withAlpha(IM_COL32_WHITE, 0.22f * l), lo = theme::withAlpha(IM_COL32_WHITE, 0.0f);
  dl->AddRectFilledMultiColor(c0, c1, hi, hi, lo, lo);
  dl->AddRect(p0, p1, theme::withAlpha(theme::mix(col, IM_COL32_WHITE, 0.35f), 0.9f * l), rounding, 0, 1.5f);
}

}  // namespace gl::keylight

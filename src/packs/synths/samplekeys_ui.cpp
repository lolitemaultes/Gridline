// Gridline — "synths" pack, J-33 group "samplekeys": Sample Keys editor
// (gl.samplekeys): the sound set selector and the 15 knobs.
#include <cstdio>
#include "../flc_kit.h"
#include "samplekeys.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;
using namespace skeys;

namespace {

const skin::Skin& keysSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2E2A26, 0x1C1916, 0x423C35, 0xE0B070, 0x50483F, 0xEDE5DA);
    s.select = skin::hex(0xE0B070);
    s.accent2 = skin::hex(0xE0B070);
    return s;
  }();
  return k;
}

}  // namespace

void drawSampleKeysEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = keysSkin();
  constexpr float kW = 620, kH = 200;
  ImGui::PushID("gl.samplekeys");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  skin::text(dl, F.p(12, 10), K.accent2C(), "SAMPLE KEYS", F.font(14));
  // Sound set.
  const auto& names = keySetNames();
  const bool installed = keySetInstalled(st.data);
  std::string shown = st.data.empty() ? (names.empty() ? std::string("(no sound sets found)") : names[0]) : st.data;
  if (!st.data.empty() && !installed) shown += names.empty() ? " (not installed)" : " (plays " + names[0] + ")";
  ImGui::SetCursorScreenPos(F.p(150, 8));
  ImGui::SetNextItemWidth(260 * F.s);
  if (ImGui::BeginCombo("##set", shown.c_str())) {
    for (const auto& n : names)
      if (ImGui::Selectable(n.c_str(), n == st.data)) {
        PluginState ns = st;
        ns.data = n;
        setPluginState(a, t, ns, "sound set");
      }
    ImGui::EndCombo();
  }
  static const char* labels[SkCount] = {"Decay", "Release", "Hard", "Vel>Hard", "Muffle", "Vel>Muf", "Vel sens", "Stereo",
                                        "Tune", "Detune", "Stretch", "Treble", "Pan/Trem", "LFO", "Drive"};
  for (int i = 0; i < SkCount; ++i) {
    const float x = 14.f + 76.f * static_cast<float>(i % 8), y = 44.f + 76.f * static_cast<float>(i / 8);
    flk::wheel(c, i, x, y, 34, labels[i]);
  }
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths

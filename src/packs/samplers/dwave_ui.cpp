// Gridline — "samplers" pack, J-33 group "dwave": Wave Program editor
// (gl.waveprogram): program controls and the zone list.
#include <cstdio>
#include <string>
#include "../flc_kit.h"
#include "dwave.h"

namespace gl::packs::samplers {


using namespace dw;

namespace {

const skin::Skin& wpSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2A2E33, 0x181B1F, 0x3A4048, 0x7FB8E6, 0x4A525C, 0xE2E8EE);
    s.select = skin::hex(0x7FB8E6);
    s.accent2 = skin::hex(0x7FB8E6);
    return s;
  }();
  return k;
}

}  // namespace

void drawWaveProgramEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  static thread_local std::string key;
  static thread_local DwProgram prog;
  if (st.data != key) {
    key = st.data;
    if (!parseProgram(st.data, prog)) prog = DwProgram{};
  }
  const skin::Skin& K = wpSkin();
  constexpr float kW = 700, kH = 330;
  ImGui::PushID("gl.waveprogram");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  skin::text(dl, F.p(12, 10), K.accent2C(), "WAVE PROGRAM", F.font(14));
  char b[200];
  std::snprintf(b, sizeof b, "%s  ·  %zu zones  ·  %s", prog.name.c_str(), prog.zones.size(),
                prog.playMode == 0 ? "poly" : prog.playMode == 1 ? "mono" : "legato");
  flk::text(c, 150, 14, b);
  static const int tags[] = {DwMaster, DwGlide, DwLfo1Rate, DwLfo2Rate, DwDelayTime, DwDelayFb, DwRevRoom, DwRevDecay,
                             DwChoDepth, DwChoRate, DwModVal1, DwModVal2, DwModVal3, DwModVal4, DwDriveA, DwDriveB};
  static const char* names[] = {"Master", "Glide", "LFO 1", "LFO 2", "Dly time", "Dly fb", "Room", "Decay",
                                "Cho depth", "Cho rate", "Mod 1", "Mod 2", "Mod 3", "Mod 4", "Drive A", "Drive B"};
  for (int i = 0; i < 16; ++i) flk::wheel(c, tags[i], 14.f + 86.f * static_cast<float>(i % 8), 40.f + 70.f * static_cast<float>(i / 8), 32, names[i]);
  // Zone list.
  ImGui::SetCursorScreenPos(F.p(10, 190));
  ImGui::BeginChild("zones", ImVec2(680 * F.s, 130 * F.s), true);
  for (size_t i = 0; i < prog.zones.size(); ++i) {
    const DwZone& z = prog.zones[i];
    std::snprintf(b, sizeof b, "%3zu  keys %3d-%3d  root %3d  vel %3d-%3d  %s", i + 1, z.lo, z.hi, z.root, z.vlo, z.vhi,
                  z.name.empty() ? z.path.c_str() : z.name.c_str());
    ImGui::TextUnformatted(b);
  }
  ImGui::EndChild();
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::samplers

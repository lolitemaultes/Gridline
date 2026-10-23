// Gridline — "samplers" pack, J-33 group "dwave": Wave Program editor
// (gl.waveprogram): program controls and the zone list.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../../core/I18n.h"
#include "../../core/PluginBrand.h"
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
  constexpr float kW = 700, kH = 400;
  ImGui::PushID("gl.waveprogram");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  skin::text(dl, F.p(12, 10), K.accent2C(), pluginTitle("gl.waveprogram").c_str(), F.font(14));
  char b[200];
  std::snprintf(b, sizeof b, "%s  ·  %zu zones  ·  %s", prog.name.c_str(), prog.zones.size(),
                prog.playMode == 0 ? "poly" : prog.playMode == 1 ? "mono" : "legato");
  flk::text(c, 150, 14, b);
  // J-92: every program control (LFO phase / attack, delay filters, reverb damping /
  // diffusion, chorus delay / feedback had none): three rows of nine.
  static const int tags[] = {DwMaster,    DwGlide,       DwLfo1Rate,     DwLfo1Phase, DwLfo1Attack, DwLfo2Rate, DwLfo2Phase,
                             DwLfo2Attack, DwDelayTime,  DwDelayFb,      DwDelayLowCut, DwDelayHighCut, DwRevRoom, DwRevDamp,
                             DwRevDiffusion, DwRevDecay, DwChoDelay,     DwChoDepth,  DwChoRate,    DwChoFb,    DwModVal1,
                             DwModVal2,   DwModVal3,     DwModVal4,      DwDriveA,    DwDriveB};
  static const char* names[] = {"Master", "Glide", "LFO 1", i18n::N_("LFO 1 phase"), i18n::N_("LFO 1 fade"), "LFO 2",
                                i18n::N_("LFO 2 phase"), i18n::N_("LFO 2 fade"), "Dly time", "Dly fb", i18n::N_("Dly low cut"),
                                i18n::N_("Dly high cut"), "Room", i18n::N_("Damping"), i18n::N_("Diffusion"), "Decay",
                                i18n::N_("Cho delay"), "Cho depth", "Cho rate", i18n::N_("Cho fb"), "Mod 1", "Mod 2", "Mod 3", "Mod 4",
                                "Drive A", "Drive B"};
  constexpr int kTags = static_cast<int>(sizeof tags / sizeof tags[0]);
  for (int i = 0; i < kTags; ++i)
    flk::wheel(c, tags[i], 14.f + 76.f * static_cast<float>(i % 9), 34.f + 50.f * static_cast<float>(i / 9), 30, i18n::tr(names[i]));
  // J-68: output peak meter (the channel's live output) next to the title.
  {
    float peak = 0;
    if (const ScopeTap* tap = t.tap(a)) {
      float buf[512];
      if (tap->read(buf, 512))
        for (float v : buf) peak = std::max(peak, std::fabs(v));
    }
    const float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f, k = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    dl->AddRectFilled(F.p(560, 12), F.p(690, 22), K.wellC(), 2 * F.s);
    if (k > 0) dl->AddRectFilled(F.p(561, 13), F.p(561 + 128 * k, 21), db > -0.5f ? skin::hex(0xE5484D) : K.accent2C(), 2 * F.s);
  }
  // J-68: zone map: every zone as its key (x) × velocity (y) rectangle with
  // its root key marked, redrawn from the program as it changes.
  {
    const ImVec2 z0 = F.p(10, 186), z1 = F.p(690, 254);
    skin::graphWell(dl, z0, z1, K);
    auto kx = [&](double key) { return z0.x + 2 + static_cast<float>(key / 128.0) * (z1.x - z0.x - 4); };
    auto vy = [&](double vel) { return z1.y - 2 - static_cast<float>(vel / 127.0) * (z1.y - z0.y - 4); };
    for (int o = 0; o <= 10; ++o) dl->AddLine(ImVec2(kx(o * 12), z0.y + 1), ImVec2(kx(o * 12), z1.y - 1), K.graphGrid);
    for (size_t i = 0; i < prog.zones.size(); ++i) {
      const DwZone& z = prog.zones[i];
      const ImU32 col = theme::withAlpha(K.accent2C(), 0.18f + 0.12f * static_cast<float>(i % 3));
      dl->AddRectFilled(ImVec2(kx(z.lo), vy(z.vhi)), ImVec2(kx(z.hi + 1), vy(z.vlo)), col);
      dl->AddRect(ImVec2(kx(z.lo), vy(z.vhi)), ImVec2(kx(z.hi + 1), vy(z.vlo)), theme::withAlpha(K.accent2C(), 0.7f));
      dl->AddLine(ImVec2(kx(z.root + 0.5), vy(z.vhi)), ImVec2(kx(z.root + 0.5), vy(z.vlo)), K.text, 1.2f);
    }
  }
  // Zone list.
  ImGui::SetCursorScreenPos(F.p(10, 260));
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

// Gridline — Grain Scanner editor on the reference granular player's 392 × 429
// form: GRAINS (attack, hold, grain spacing, wave spacing + the grain shape
// view), EFFECTS (pan, depth, speed, randomness), TRANSIENTS (hold, source)
// and TIME (start, loop, tempo lock, hold, key mode) panels, then the sample
// waveform with the start point and the load button; the channel volume
// envelope row (the reference instrument's channel envelope) underneath.
#include <filesystem>
#include "../../app/App.h"
#include "metal_widgets.h"

namespace gl::packs::models::metal {

using namespace ui;

namespace {

constexpr float kW = 392, kH = 489;

const skin::Skin& gsSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(bgr(5196613), bgr(5657164), 0x2C3134, 0x9FD3E8, 0x5E666B, 0xD5DADD);
    s.select = skin::hex(0x9FD3E8);
    s.accent2 = skin::hex(0xF0B14A);
    s.pillBg = skin::hex(bgr(3880753));
    s.graphBg = skin::hex(0x1D2124);
    s.graphGrid = skin::hex(0x2B3135);
    s.graphGridStrong = skin::hex(0x3A4248);
    s.graphCurve = skin::hex(0x9FD3E8);
    s.lcdBg = skin::hex(bgr(3091238));
    s.lcdText = skin::hex(0xD5DADD);
    return s;
  }();
  return k;
}

void commitPath(App& a, const PluginTarget& t, const std::string& p) {
  const PluginState* now = t.state(a.doc.song());
  if (!now) return;
  PluginState ns = *now;
  ns.data = p;
  setPluginState(a, t, ns, "load sample");
}

}  // namespace

void drawGrainScan(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = gsSkin();
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  flk::Ctx c{a, t, F, K, dl};
  ImGui::PushID("gl.grainscan");
  // ---- grains
  flk::section(c, 6, 6, 380, 99, "Grains");
  flk::wheel(c, GsAttack, 25, 28, 50, "Attack");
  flk::wheel(c, GsHold, 89, 28, 50, "Hold");
  flk::wheel(c, GsGrainSpace, 155, 28, 50, "Gr. spacing");
  flk::wheel(c, GsWaveSpace, 224, 28, 50, "Wave spacing", skin::WheelBigLine | skin::WheelCentered);
  {  // grain shape: attack / hold / release over two grain periods
    const ImVec2 g0 = F.p(305, 27), g1 = F.p(376, 93);
    skin::graphWell(dl, g0, g1, K);
    const float att = prmf(st, GsAttack, 204) / 500.0f, hold = prmf(st, GsHold, 50) / 500.0f;
    const float total = std::max(0.05f, 2 * att + hold);
    const float wpx = (g1.x - g0.x - 8), hpx = (g1.y - g0.y - 10);
    auto px = [&](float tt) { return g0.x + 4 + tt / (2 * total) * wpx; };
    for (int rep = 0; rep < 2; ++rep) {
      const float o = rep * total * 0.5f;
      const ImVec2 pts[4] = {ImVec2(px(o), g1.y - 5), ImVec2(px(o + att), g1.y - 5 - hpx), ImVec2(px(o + att + hold), g1.y - 5 - hpx),
                             ImVec2(px(o + 2 * att + hold), g1.y - 5)};
      dl->AddPolyline(pts, 4, rep ? K.accent2C() : K.graphCurve, ImDrawFlags_None, 2.0f);
    }
  }
  // ---- effects
  flk::section(c, 6, 112, 380, 99, "Effects");
  flk::wheel(c, GsStereo, 25, 134, 50, "Pan");
  flk::wheel(c, GsFxDepth, 89, 134, 50, "Depth");
  flk::wheel(c, GsFxSpeed, 155, 134, 50, "Speed");
  flk::wheel(c, GsRandom, 224, 134, 50, "Rand");
  flk::wheel(c, GsVolume, 300, 134, 50, "Volume");
  // ---- transients
  flk::section(c, 6, 218, 184, 99, "Transients");
  flk::wheel(c, GsTransHold, 25, 240, 50, "Hold");
  static const char* src[3] = {"AUTO", "SLICES", "MARKERS"};
  flk::sw(c, GsTransSource, 101, 245, 84, 51, src, 3);
  // ---- time
  flk::section(c, 196, 218, 190, 99, "Time");
  flk::wheel(c, GsStart, 214, 240, 50, "Start");
  flk::chk(c, GsLoop, 238, 222, 45, 14, "LOOP");
  flk::chk(c, GsTempoLock, 290, 222, 92, 14, "TEMPO LOCKED");
  flk::tog(c, GsHoldOn, 312, 249, 61, 25, "HOLD");
  flk::text(c, 268, 293, "KEY TO");
  flk::menu(c, GsTrigger, 308, 288, 69, 20);
  // ---- waveform
  {
    const ImVec2 w0 = F.p(6, 324), w1 = F.p(386, 423);
    skin::graphWell(dl, w0, w1, K);
    SamplePtr smp = sampleOf(st.data);
    waveform(dl, smp.get(), ImVec2(w0.x + 3, w0.y + 3), ImVec2(w1.x - 3, w1.y - 3), K.fill);
    const float s0 = prmf(st, GsStart, 0) / 4096.0f;
    const float x = w0.x + (w1.x - w0.x) * s0;
    dl->AddLine(ImVec2(x, w0.y), ImVec2(x, w1.y), K.accent2C(), 2);
    std::string nm = st.data.empty() ? std::string("built-in phrase — drop a sample on the channel or load one") : std::filesystem::path(st.data).filename().string();
    skin::text(dl, F.p(12, 328), K.textDim, nm.c_str(), F.font(8));
    PluginTarget tt = t;
    if (flk::btn(c, "##load", 340, 326, 43, 18, "LOAD", false))
      openSampleDialog(a, "Load sample", [&a, tt](const std::string& p) { commitPath(a, tt, p); });
  }
  // ---- channel volume envelope (reference: the channel's own envelope)
  flk::section(c, 6, 430, 380, 54, "Volume envelope");
  flk::chk(c, GsEnvOn, 120, 433, 40, 13, "ON");
  flk::wheel(c, GsEnvAttack, 40, 448, 28, "Att");
  flk::wheel(c, GsEnvHold, 100, 448, 28, "Hold");
  flk::wheel(c, GsEnvDecay, 160, 448, 28, "Dec");
  flk::wheel(c, GsEnvSustain, 220, 448, 28, "Sus");
  flk::wheel(c, GsEnvRelease, 280, 448, 28, "Rel");
  flk::wheel(c, GsRoot, 340, 448, 28, "Root");
  ImGui::PopID();
  F.end(kW, kH);
}

}  // namespace gl::packs::models::metal

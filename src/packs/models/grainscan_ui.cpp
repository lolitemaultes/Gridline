// Gridline — Grain Scanner editor on the reference granular player's 392 × 429
// form: GRAINS (attack, hold, grain spacing, wave spacing + the grain shape
// view), EFFECTS (pan, depth, speed, randomness), TRANSIENTS (hold, source)
// and TIME (start, loop, tempo lock, hold, key mode) panels, then the sample
// waveform with the start point and the load button; the channel volume
// envelope row (the reference instrument's channel envelope) underneath.
#include <filesystem>
#include "../../app/UrlImportUi.h"
#include "../../app/App.h"
#include "metal_widgets.h"
#include "../../core/I18n.h"

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
  {  // the grains of one window: each grain's attack / hold / release shape at its real spacing
     // (grain spacing), and the read head across them (wave spacing: rising forwards, flat
     // when frozen, falling backwards), with the playback speed they give
    const ImVec2 g0 = F.p(305, 27), g1 = F.p(376, 93);
    skin::graphWell(dl, g0, g1, K);
    const GsTiming tm = gsTiming(prmf(st, GsAttack, 204), prmf(st, GsHold, 50), prmf(st, GsGrainSpace, 611), prmf(st, GsWaveSpace, 611), 44100.0);
    const GsPreview pv = gsPreview(tm);
    const float wpx = (g1.x - g0.x - 8), hpx = (g1.y - g0.y - 14);
    const float base = g1.y - 5, top = base - hpx;
    auto px = [&](float f) { return g0.x + 4 + f * wpx; };
    dl->PushClipRect(g0, g1, true);
    for (int k = 0; k < pv.count; ++k) {
      const ImVec2 pts[4] = {ImVec2(px(pv.x0[k]), base), ImVec2(px(pv.x1[k]), top), ImVec2(px(pv.x2[k]), top), ImVec2(px(pv.x3[k]), base)};
      dl->AddPolyline(pts, 4, (k & 1) ? K.accent2C() : K.graphCurve, ImDrawFlags_None, 1.6f);
    }
    const float headH = std::max(4.0f, hpx - F.font(7.5f) - 4.0f);  // (the read head keeps under the speed text)
    for (int k = 0; k + 1 < pv.count; ++k)
      dl->AddLine(ImVec2(px(pv.x0[k]), base - pv.head[k] * headH), ImVec2(px(pv.x0[k + 1]), base - pv.head[k + 1] * headH), theme::withAlpha(K.text, 0.55f), 1.0f);
    for (int k = 0; k < pv.count; ++k) dl->AddCircleFilled(ImVec2(px(pv.x0[k]), base - pv.head[k] * headH), 1.8f * F.s, theme::withAlpha(K.text, 0.8f), 8);
    char spd[24];
    if (std::fabs(pv.speed) < 0.005) std::snprintf(spd, sizeof spd, "%s", i18n::tr("held"));
    else std::snprintf(spd, sizeof spd, "%s%.2fx", pv.speed < 0 ? "-" : "", std::fabs(pv.speed));
    skin::text(dl, ImVec2(g0.x + 4, g0.y + 4), K.textDim, spd, F.font(7.5f));  // (the well's inset: 3 px at least)
    dl->PopClipRect();
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
    std::string nm = st.data.empty() ? std::string("built-in phrase: drop a sample on the channel or load one") : std::filesystem::path(st.data).filename().string();
    dl->PushClipRect(F.p(12, 326), F.p(284, 344), true);
    skin::text(dl, F.p(12, 328), K.textDim, nm.c_str(), F.font(8));
    dl->PopClipRect();
    PluginTarget tt = t;
    if (flk::btn(c, "##url", 292, 326, 43, 18, "URL", false)) urlimportui::openForPlugin(a, t, 0);
    if (ImGui::IsItemHovered()) a.setHint("Import from URL: a web link's audio to scan");
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

bool loadGrainScanAudio(App& a, const PluginTarget& t, const std::string& path) {
  commitPath(a, t, path);
  return true;
}

}  // namespace gl::packs::models::metal

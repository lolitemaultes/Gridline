// Gridline — "synths" pack, J-22: FM Duo editor (gl.fmduo) on the reference
// two-modulator FM synth's 536 × 340 form and slate palette: MISC panel
// (waveform, octave, LFO rate, vibrato), AMPLITUDE (envelope view, attack /
// decay / release), MODULATION 1 and 2 (ratio readout, coarse / fine /
// velocity, envelope view and init / time / sustain / release; thru on
// modulation 1), name strip and key strip. Drawn with the shared skin kit.
#include <cmath>
#include "../flc_kit.h"
#include "dco.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;
using namespace dco;

namespace {

const skin::Skin& duoSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x303E45, 0x5D6973, 0x46535B, 0xF0C060, 0x3A474E, 0xE2E8EB);
    s.well = skin::hex(0x5D6973);
    s.select = skin::hex(0xF0C060);
    s.accent2 = skin::hex(0xF0C060);
    s.lcdBg = skin::hex(0x1D2529);
    s.lcdText = skin::hex(0xF0C060);
    s.graphBg = skin::hex(0x1D2529);
    s.graphGrid = skin::hex(0x2B353A);
    s.graphGridStrong = skin::hex(0x36434A);
    s.graphCurve = skin::hex(0xF0C060);
    return s;
  }();
  return k;
}

double ratioOf(double coarse, double fine) {
  double r = std::floor(40.1 * coarse * coarse);
  if (fine < 0.5) {
    r += 0.2 * fine * fine;
  } else {
    static const double steps[5] = {0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75};
    r += steps[std::clamp(static_cast<int>(fine * 8.9) - 4, 0, 4)];
  }
  return r;
}

// Carrier envelope: rise e^(8a−8), fall e^(8d−5) (holds above 0.98), release e^(5r−5).
void ampView(const flk::Ctx& c, const PluginState& st, float x, float y, float w, float h) {
  ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(c.dl, p0, p1, c.K);
  const double ta = std::exp(8 * val(st, FdAttack) - 8) * 3, d = val(st, FdDecay), td = d > 0.98 ? 1e9 : std::exp(8 * d - 5);
  const double tr = std::exp(5 * val(st, FdRelease) - 5);
  const double span = 2.0;
  ImVec2 pts[64];
  for (int i = 0; i < 64; ++i) {
    const double tt = span * i / 63.0;
    const double held = 1.2, a = 1 - std::exp(-tt / std::max(1e-4, ta / 3));
    double v = a * (tt < held ? std::exp(-tt / td) : std::exp(-held / td) * std::exp(-(tt - held) / std::max(1e-4, tr)));
    pts[i] = ImVec2(p0.x + static_cast<float>(i / 63.0) * (p1.x - p0.x), p1.y - 2 - static_cast<float>(v) * (p1.y - p0.y - 4));
  }
  c.dl->AddPolyline(pts, 64, c.K.graphCurve, 0, 1.5f);
}
// Modulator envelope: init → sustain over the time constant, then release.
void modView(const flk::Ctx& c, const PluginState& st, int base, float x, float y, float w, float h) {
  ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(c.dl, p0, p1, c.K);
  const double init = val(st, base + 2), tm = std::exp(7 * val(st, base + 3) - 6), sus = val(st, base + 4);
  const double tr = std::exp(8 * val(st, base + 5) - 5);
  ImVec2 pts[64];
  for (int i = 0; i < 64; ++i) {
    const double tt = 2.0 * i / 63.0, held = 1.2;
    double v = tt < held ? sus + (init - sus) * std::exp(-tt / tm)
                         : (sus + (init - sus) * std::exp(-held / tm)) * std::exp(-(tt - held) / std::max(1e-4, tr));
    pts[i] = ImVec2(p0.x + static_cast<float>(i / 63.0) * (p1.x - p0.x), p1.y - 2 - static_cast<float>(v) * (p1.y - p0.y - 4));
  }
  c.dl->AddPolyline(pts, 64, c.K.graphCurve, 0, 1.5f);
}

void modPanel(const flk::Ctx& c, const PluginState& st, int base, float x0, const char* title, bool thru) {
  flk::section(c, x0, 2, 176, 201, title);
  const float dx = x0 - 180;
  flk::wheel(c, base, 185 + dx, 37, 36, "Coarse");
  flk::wheel(c, base + 1, 228 + dx, 37, 36, "Fine");
  flk::wheel(c, base + 6, 273 + dx, 37, 36, "Vel");
  if (thru) flk::wheel(c, FdModThru, 317 + dx, 37, 36, "Thru");
  char b[32];
  std::snprintf(b, sizeof b, "x%.3g", ratioOf(val(st, base), val(st, base + 1)));
  flk::lcd(c, 300 + dx, 20, 50, 13, b);
  modView(c, st, base, 184 + dx, 93, 170, 50);
  const char* n[4] = {"Init", "Time", "Sus", "Rel"};
  for (int i = 0; i < 4; ++i) flk::wheel(c, base + 2 + i, 185 + dx + 44.f * i, 150, 36, n[i]);
}

}  // namespace

void drawFmDuoEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = duoSkin();
  constexpr float kW = 536, kH = 340;
  ImGui::PushID("gl.fmduo");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};

  flk::section(c, 2, 2, 176, 89, "MISC");
  flk::wheel(c, FdWaveform, 6, 37, 36, "Wave");
  flk::wheel(c, FdOctave, 50, 37, 36, "Octave", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, FdLfoRate, 95, 37, 36, "LFO rate");
  flk::wheel(c, FdVibrato, 140, 37, 36, "Vib");

  flk::section(c, 2, 93, 176, 143, "AMPLITUDE");
  ampView(c, st, 6, 122, 170, 50);
  flk::wheel(c, FdAttack, 6, 180, 36, "Att");
  flk::wheel(c, FdDecay, 50, 180, 36, "Dec");
  flk::wheel(c, FdRelease, 95, 180, 36, "Rel");

  modPanel(c, st, FdM1Coarse, 180, "MODULATION 1", true);
  modPanel(c, st, FdM2Coarse, 358, "MODULATION 2", false);

  flk::section(c, 180, 205, 354, 31, "");
  skin::text(dl, F.p(190, 212), K.accent2C(), "FM DUO", F.font(14));
  flk::text(c, 300, 218, "CARRIER + TWO MODULATORS", K.textDim, 8.0f);
  skin::keyboard("##duokb", F.p(2, 238), F.sz(532, 99), 36, 96, K);
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths

// Gridline — "synths" pack, J-22: Finger Bass editor on the reference bass
// guitar's 340 × 150 grid and palette (slate panel, crimson value arcs): the
// three stepped tone wheels (bass, mid, treble) and a small string / pickup
// drawing.
#include "../../app/App.h"
#include "../../app/InstrumentSkin.h"
#include "va.h"
#include "va_ui.h"

namespace gl::packs::synths::va {

namespace {

using iskin::rgb;
constexpr float kW = 340, kH = 150;

iskin::Palette bassPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x3A4146);
  p.bgBottom = rgb(0x31373B);
  p.panel = rgb(0x262B2E);
  p.panelLine = rgb(0x1C2023);
  p.rail = rgb(0x5A656B);
  p.fill = rgb(0xD03060);
  p.cap = rgb(0x4C555B);
  p.label = rgb(0xAEB7BC);
  p.text = rgb(0xE4E8EA);
  p.textDim = rgb(0x8A9499);
  return p;
}

}  // namespace

void drawFingerBassEditor(App& a, const PluginTarget& t, float width) {
  if (!t.state(a.doc.song())) return;
  iskin::Skin k = iskin::begin(width, kW, kH, bassPalette());
  ImGui::PushID("gl.fingerbass");
  iskin::background(k, 0, 0, kW, kH, k.c.bgTop, k.c.bgBottom, 4);
  iskin::text(k, 12, 6, 200, 18, "FINGER BASS", 12, k.c.text, 0, false, true);
  // Strings + pickup (decoration).
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int s = 0; s < 4; ++s) {
    const float y = 22.f + s * 5.f;
    dl->AddLine(k.at(200, y), k.at(330, y + 2), rgb(0x9AA3A8, 0.55f), (1.6f - s * 0.25f) * k.s);
  }
  iskin::bevel(k, 280, 16, 14, 26, rgb(0x15181A), k.c.panelLine, 2);
  wheelCap(a, t, k, BbBass, 50, 45, 56, k.c.fill, "BASS", true);
  wheelCap(a, t, k, BbMid, 120, 66, 56, k.c.fill, "MID", true);
  wheelCap(a, t, k, BbTreble, 190, 75, 56, k.c.fill, "TREBLE", true);
  iskin::end(k, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths::va

// Gridline — Plucker editor on the reference plucked-string generator's
// 360 × 156 grid and palette (warm orange panel): the large decay and colour
// wheels, the three option buttons on the right edge, the name plate.
#include "../../core/PluginBrand.h"
#include "../flc_kit.h"
#include "models_widgets.h"
#include "strings.h"
#include "strings_laws.h"

namespace gl::packs::models {

using namespace strs;

namespace {

const skin::Skin& pluckSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0xF9B352, 0xE9A242, 0x374046, 0x2B3136, 0xC9822A, 0x374046);
    s.text = skin::hex(0x2B3136);
    s.textDim = skin::hex(0x374046);
    s.select = skin::hex(0xFFCE83);
    s.fill = skin::hex(0xFFF2D8);
    s.handle = skin::hex(0x374046);
    s.lcdBg = skin::hex(0x374046);
    s.lcdText = skin::hex(0xFFCE83);
    s.pillBg = skin::hex(0x374046);
    s.pillText = skin::hex(0xFFCE83);
    return s;
  }();
  return k;
}

// Option button bound to a toggle parameter (FL-style flat button).
void optButton(const flk::Ctx& c, int param, float x, float y, const char* text, ImU32 col) {
  const PluginState* st = c.t.state(c.a.doc.song());
  bool on = st && ui2::val(*st, param) >= 0.5f;
  ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + 55, y + 25);
  c.dl->AddRectFilled(p0, p1, on ? col : skin::darken(col, 0.35f), 3 * c.F.s);
  c.dl->AddRect(p0, p1, skin::darken(col, 0.6f), 3 * c.F.s);
  skin::label(c.dl, (p0.x + p1.x) * 0.5f, p0.y + 7 * c.F.s, text, on ? skin::hex(0x2B3136) : skin::hex(0x5A4A30), c.F.font(10));
  ImGui::SetCursorScreenPos(p0);
  ImGui::PushID(param);
  if (ImGui::InvisibleButton("##opt", ImVec2(p1.x - p0.x, p1.y - p0.y))) c.a.doc.setParam(c.t.pid(param), on ? 0.0 : 1.0, false);
  if (ImGui::IsItemHovered()) {
    auto d = describeParam(c.a.doc.song(), c.t.pid(param));
    if (d) c.a.setHint(d->name + ": " + (on ? "on" : "off"));
  }
  ImGui::PopID();
}

}  // namespace

void drawPlucker(App& a, const PluginTarget& t, float width) {
  const skin::Skin& K = pluckSkin();
  constexpr float kW = 360, kH = 156;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  ImGui::PushID("gl.plucker");
  dl->AddRectFilled(F.p(3, 124), F.p(164, 149), skin::hex(0x374046), 3 * F.s);
  if (!pui::titleWordmark(dl, "gl.plucker", F.p(12, 127), F.font(17)))  // J-136: the wordmark
    skin::text(dl, F.p(12, 127), skin::hex(0xFFCE83), pluginTitle("gl.plucker").c_str(), F.font(17));
  skin::text(dl, F.p(12, 12), K.textDim, "DECAY", F.font(10));
  skin::wheel(a, t, PkDecay, F.p(58, 25), 88 * F.s, K, skin::WheelBigLine);
  skin::text(dl, F.p(160, 34), K.textDim, "COLOUR", F.font(10));
  skin::wheel(a, t, PkColor, F.p(178, 45), 88 * F.s, K, skin::WheelBigLine);
  optButton(c, PkNormalize, 301, 5, "EVEN", skin::hex(0xF9A83C));
  optButton(c, PkGate, 301, 32, "GATE", skin::hex(0xF9A83C));
  optButton(c, PkWiden, 301, 127, "WIDE", skin::hex(0xFF9B37));
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::models

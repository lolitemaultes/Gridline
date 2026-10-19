// Gridline — Drum Model editor (J-19) on the reference modelled-drum
// instrument's 762-wide grid and palette (its pattern sequencer row is not
// part of this instrument): the pads panel shows one bank of eight channels
// (volume / pan wheels + pad tile: click to play and select, right-click to
// mute) with the master column (level, bank select 1-8 / 9-16, base note,
// velocity, key mode); the selected pad's strike / membrane / pitch / noise /
// output blocks below; then the live view of its resonant modes and the kit
// equalizer / limiter.
#include "../flc_kit.h"
#include <map>
#include "../../engine/Dsp.h"
#include "models_widgets.h"

namespace gl::packs::models {

using namespace ui2;

namespace {

std::map<uint64_t, int>& selection() {
  static std::map<uint64_t, int> m;
  return m;
}

std::string noteLabel(int n) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  char b[8];
  std::snprintf(b, sizeof b, "%s%d", names[((n % 12) + 12) % 12], n / 12 - 1);
  return b;
}

// A short description of what a pad currently sounds like.
std::string padLabel(const PluginState& st, int pad) {
  int shape = ival(st, drumPad(pad, DpShape));
  float f = val(st, drumPad(pad, DpFreq), 100), dec = val(st, drumPad(pad, DpDecay), 0.4f), nz = val(st, drumPad(pad, DpNoise));
  switch (shape) {
    case DsMembrane:
      if (nz > 0.3f) return f < 400 ? "Snare" : "Clap";
      if (f < 85) return dec > 1.2f ? "Boom" : "Kick";
      return dec > 1.2f ? "Timpani" : "Drum";
    case DsTom:
      if (f < 110) return "Low tom";
      if (f < 160) return "Mid tom";
      if (f < 260) return "High tom";
      return f < 380 ? "Conga" : "Bongo";
    case DsBar: return f > 1500 ? "Clave" : (nz > 0.1f ? "Rim" : "Wood bar");
    case DsPlate:
      if (f > 2000) return dec < 0.2f ? "Closed hat" : (dec < 1.0f ? "Open hat" : "Cymbal");
      if (nz > 0.6f && dec < 0.1f) return "Clap";
      return "Plate";
    case DsBell: return f < 900 ? "Cowbell" : "Bell";
    case DsTabla: return "Tabla";
    default: return "Pad";
  }
}

void padTile(App& a, const PluginTarget& t, const PluginState& st, int pad, int& sel, ImVec2 sz) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q = ImGui::GetCursorScreenPos();
  ImGui::PushID(pad);
  ImGui::InvisibleButton("pad", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  int base = ival(st, DmBaseNote, 60);
  bool muted = val(st, drumPad(pad, DpMute)) >= 0.5f;
  if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    sel = pad;
    a.engine.noteOn(t.owner, base + pad, 0.9f);
  }
  if (ImGui::IsItemDeactivated()) a.engine.noteOff(t.owner, base + pad);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.doc.setParam(t.pid(drumPad(pad, DpMute)), muted ? 0 : 1, false);
  ImU32 bg = act ? theme::mix(P.surface700, P.paper, 0.3f) : (pad == sel ? P.surface600 : (hov ? P.surface700 : P.surface900));
  dl->AddRectFilled(q, ImVec2(q.x + sz.x, q.y + sz.y), bg, 6);
  if (pad == sel) dl->AddRect(q, ImVec2(q.x + sz.x, q.y + sz.y), P.accent, 6, 0, 1.5f);
  ImGui::PushFont(a.fontMono, 10.0f);
  char num[8];
  std::snprintf(num, sizeof num, "%02d", pad + 1);
  dl->AddText(ImVec2(q.x + 6, q.y + 5), P.textFaint, num);
  std::string nl = noteLabel(base + pad);
  dl->AddText(ImVec2(q.x + sz.x - 6 - ImGui::CalcTextSize(nl.c_str()).x, q.y + 5), P.textFaint, nl.c_str());
  ImGui::PopFont();
  std::string name = padLabel(st, pad);
  dl->PushClipRect(q, ImVec2(q.x + sz.x - 3, q.y + sz.y), true);
  ImGui::PushFont(nullptr, 12.5f);
  dl->AddText(ImVec2(q.x + 6, q.y + 22), muted ? P.textFaint : P.text, name.c_str());
  ImGui::PopFont();
  dl->PopClipRect();
  // Level bar and mute marker.
  float lv = val(st, drumPad(pad, DpLevel), 0.8f);
  float by = q.y + sz.y - 9;
  dl->AddRectFilled(ImVec2(q.x + 6, by), ImVec2(q.x + sz.x - 6, by + 3), P.surface950, 1.5f);
  dl->AddRectFilled(ImVec2(q.x + 6, by), ImVec2(q.x + 6 + (sz.x - 12) * lv, by + 3), muted ? P.textFaint : P.textDim, 1.5f);
  if (muted) {
    ImGui::PushFont(a.fontMono, 10.0f);
    dl->AddText(ImVec2(q.x + 6, q.y + sz.y - 26), P.danger, "MUTE");
    ImGui::PopFont();
  }
  if (hov) a.setHint("Pad " + std::to_string(pad + 1) + " (" + nl + ")   click: play and edit · right-click: mute");
  ImGui::PopID();
}

// The selected pad's modes on a log-frequency axis (height = level, brightness
// = how long the mode rings); the dotted marks show where a pitch bend starts.
void modeView(App& a, const PluginState& st, int pad, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = graphFrame(size, "##modes", "MODES", a);
  const double lo = 20, hi = 20000;
  freqLabels(a, dl, p, size, lo, hi);
  int shape = std::clamp(ival(st, drumPad(pad, DpShape)), 0, DsCount - 1);
  const float* r = drumRatios(shape);
  const float* g = drumGains(shape);
  double f0 = val(st, drumPad(pad, DpFreq), 100) * std::pow(2.0, val(st, drumPad(pad, DpTune)) / 12.0);
  float spread = val(st, drumPad(pad, DpSpread), 1), material = val(st, drumPad(pad, DpMaterial), 0.4f);
  float decay = val(st, drumPad(pad, DpDecay), 0.4f), pos = val(st, drumPad(pad, DpPosition), 0.3f);
  float bend = val(st, drumPad(pad, DpBend));
  auto xOf = [&](double hz) { return p.x + static_cast<float>(std::log(hz / lo) / std::log(hi / lo)) * size.x; };
  const float top = p.y + 18, bot = p.y + size.y - 16;
  for (int k = 0; k < kDrumModes; ++k) {
    double ratio = 1.0 + (r[k] - 1.0) * spread, hz = f0 * ratio;
    if (hz >= hi || hz <= lo) continue;
    float gain = g[k] / (1.0f + (1.0f - pos) * k * 0.6f);
    double t60 = decay / (1.0 + material * (ratio - 1.0) * 2.0);
    float ring = static_cast<float>(std::clamp(std::log(t60 / 0.01) / std::log(4.0 / 0.01), 0.15, 1.0));
    float x = xOf(hz);
    dl->AddLine(ImVec2(x, bot), ImVec2(x, bot - gain * (bot - top)), theme::withAlpha(P.paper, ring), 2.5f);
    if (bend != 0 && k == 0) {
      float xb = xOf(std::clamp(hz * std::pow(2.0, bend / 12.0), lo, hi));
      for (float xx = std::min(x, xb); xx < std::max(x, xb); xx += 6) dl->AddLine(ImVec2(xx, top + 2), ImVec2(xx + 3, top + 2), P.accent, 1.5f);
      dl->AddCircleFilled(ImVec2(xb, top + 2), 3, P.accent);
    }
  }
  float nz = val(st, drumPad(pad, DpNoise));
  if (nz > 0) {  // noise band
    double tone = val(st, drumPad(pad, DpNoiseTone), 4000);
    float x0 = xOf(std::max(lo, tone / 2.2)), x1 = xOf(std::min(hi, tone * 2.2));
    dl->AddRectFilled(ImVec2(x0, bot - nz * 0.5f * (bot - top)), ImVec2(x1, bot), theme::withAlpha(P.accent, 0.18f), 3);
  }
}

const skin::Skin& drumkitSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3B3836, 0x2C3338, 0xE0E0E0, 0xE8AC13, 0x626465, 0xD3D2B5);
    s.handle = skin::hex(0x212121);
    s.select = skin::hex(0xE8AC13);
    s.accent2 = skin::hex(0x18A0C8);
    s.lcdBg = skin::hex(0x171717);
    s.lcdText = skin::hex(0xDAD0B2);
    s.pillBg = skin::hex(0x171717);
    s.pillText = skin::hex(0xD3D2B5);
    s.graphBg = skin::hex(0x171717);
    s.graphGrid = skin::hex(0x262626);
    s.graphCurve = skin::hex(0xE8AC13);
    return s;
  }();
  return k;
}
// Channel / master wheels: silver rail, blue value, light cap.
const skin::Skin& drumkitBlue() {
  static const skin::Skin k = [] {
    skin::Skin s = drumkitSkin();
    s.rail = skin::hex(0xC0C0C0);
    s.fill = skin::hex(0x1D6E94);
    s.fillPressed = skin::hex(0x2A8FBF);
    s.cap = skin::hex(0xBFBFBF);
    return s;
  }();
  return k;
}
const skin::Skin& drumkitChannel() {
  static const skin::Skin k = [] {
    skin::Skin s = drumkitSkin();
    s.rail = skin::hex(0x0F0F0F);
    s.fill = skin::hex(0xC1C1C1);
    s.fillPressed = skin::hex(0xE0E0E0);
    s.cap = skin::hex(0x9EA0A0);
    return s;
  }();
  return k;
}

std::map<uint64_t, int>& banks() {
  static std::map<uint64_t, int> m;
  return m;
}

}  // namespace

void drawDrumModel(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = drumkitSkin();
  constexpr float kW = 762, kH = 462;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  flk::Ctx cB{a, t, F, drumkitBlue(), dl};
  ImGui::PushID("gl.drumkit");
  int& sel = selection()[t.key()];
  sel = std::clamp(sel, 0, kDrumPads - 1);
  int& bank = banks()[t.key()];
  bank = std::clamp(bank, 0, 1);
  // ---- pads panel: one bank of 8 channels (volume / pan wheels + pad tile)
  skin::well(dl, F.p(6, 6), F.p(606, 206), K, 4 * F.s);
  for (int i = 0; i < 8; ++i) {
    int pad = bank * 8 + i;
    float x = 10 + (i % 4) * 149, y = 10 + (i / 4) * 98;
    ImGui::PushID(pad);
    skin::wheel(a, t, drumPad(pad, DpLevel), F.p(x, y + 10), 34 * F.s, drumkitChannel(), skin::WheelBigLine);
    skin::wheel(a, t, drumPad(pad, DpPan), F.p(x + 4, y + 54), 26 * F.s, drumkitChannel(), skin::WheelBigLine | skin::WheelCentered);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(F.p(x + 42, y + 2));
    padTile(a, t, st, pad, sel, F.sz(100, 88));
  }
  // ---- master column: level, bank select, base note, velocity, key mode
  skin::well(dl, F.p(610, 6), F.p(756, 206), K, 4 * F.s);
  flk::text(c, 618, 14, "MASTER");
  flk::wheel(cB, DmVolume, 700, 10, 32, "Level");
  if (flk::btn(c, "##bank1", 614, 62, 68, 40, "1-8", bank == 0)) bank = 0;
  if (flk::btn(c, "##bank2", 684, 62, 68, 40, "9-16", bank == 1)) bank = 1;
  flk::text(c, 618, 112, "BASE NOTE");
  flk::digi(c, DmBaseNote, 618, 124, 60, 18);
  flk::wheel(cB, DmVelocity, 700, 112, 30, "Velocity");
  flk::text(c, 618, 154, "KEYS");
  flk::menu(c, DmKeyMode, 618, 168, 132, 20);

  // ---- selected pad: strike · membrane · pitch · noise · output
  {
    std::string ttl = "PAD " + std::to_string(sel + 1) + "  ·  " + padLabel(st, sel);
    skin::pill(dl, F.p(6, 212), F.p(220, 226), K, ttl.c_str(), F.font(8));
  }
  auto sect = [&](float x, float w, const char* title) { flk::section(c, x, 230, w, 132, title); };
  sect(6, 120, "STRIKE");
  flk::wheel(c, drumPad(sel, DpMallet), 16, 252, 30, "Hardness");
  flk::wheel(c, drumPad(sel, DpClick), 72, 252, 30, "Click");
  flk::wheel(c, drumPad(sel, DpPosition), 44, 308, 30, "Position");
  sect(130, 200, "MEMBRANE");
  flk::menu(c, drumPad(sel, DpShape), 140, 250, 110, 18);
  flk::wheel(cB, drumPad(sel, DpFreq), 262, 248, 32, "Tension");
  flk::wheel(cB, drumPad(sel, DpSpread), 140, 300, 32, "Spread");
  flk::wheel(cB, drumPad(sel, DpDecay), 200, 300, 32, "Decay");
  flk::wheel(cB, drumPad(sel, DpMaterial), 262, 300, 32, "Material");
  sect(334, 120, "PITCH");
  flk::wheel(c, drumPad(sel, DpTune), 344, 252, 30, "Tune", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, drumPad(sel, DpBend), 400, 252, 30, "Bend", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, drumPad(sel, DpBendTime), 372, 308, 30, "Bend time");
  sect(458, 140, "NOISE · WIRES");
  flk::wheel(c, drumPad(sel, DpNoise), 470, 252, 30, "Level");
  flk::wheel(c, drumPad(sel, DpNoiseDecay), 534, 252, 30, "Decay");
  flk::wheel(c, drumPad(sel, DpNoiseTone), 470, 308, 30, "Tone");
  flk::wheel(c, drumPad(sel, DpWires), 534, 308, 30, "Wires");
  sect(602, 154, "OUTPUT");
  flk::wheel(cB, drumPad(sel, DpLevel), 612, 252, 30, "Level");
  flk::wheel(cB, drumPad(sel, DpPan), 664, 252, 30, "Pan", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cB, drumPad(sel, DpDrive), 714, 252, 30, "Drive");
  flk::wheel(cB, drumPad(sel, DpChoke), 612, 308, 30, "Choke");
  flk::tog(c, drumPad(sel, DpMute), 666, 314, 78, 20, "MUTE");

  // ---- modes view, kit EQ / limiter
  ImGui::SetCursorScreenPos(F.p(6, 368));
  modeView(a, st, sel, F.sz(436, 88));
  skin::well(dl, F.p(446, 368), F.p(756, 456), K, 4 * F.s);
  flk::text(c, 454, 374, "EQUALIZER");
  flk::wheel(cB, DmLow, 458, 390, 32, "Low");
  flk::wheel(cB, DmMid, 512, 390, 32, "Mid");
  flk::wheel(cB, DmHigh, 566, 390, 32, "High");
  flk::wheel(cB, DmLofi, 626, 390, 32, "Lo-fi");
  flk::text(c, 686, 374, "LIMITER");
  flk::tog(c, DmLimiter, 684, 396, 64, 22, "ON");
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::models

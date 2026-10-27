// Gridline — "modulation" pack: note processor chain panel and editors. The
// editors follow the reference note-processor family: slate panel, sober
// panels, tab selectors and drop-downs, a dark title strip along the bottom,
// one value colour per processor (layout per reference form where one exists:
// sequencer, key mapper, keyboard splitter, level scaler, colour mapper, script).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "../../core/PluginBrand.h"
#include "../../engine/NoteFx.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_skin.h"
#include "modulation_ui.h"
#include "modulation_widgets.h"

namespace gl::packs::modulation {

using theme::pal;

namespace {

int fxPosition(const EditTarget& e) {
  auto rt = e.a->builder.noteFxRuntime(e.owner, e.noteFx);
  return rt ? rt->position.load() : -1;
}

// Channel picker writing one routing target into the processor's data.
void targetCombo(const EditTarget& e, int slot, float width, const char* none = "This channel") {
  const Song& s = e.a->doc.song();
  std::vector<ObjectId> targets = noteFxTargets(e.st ? e.st->data : "");
  ObjectId cur = static_cast<size_t>(slot) < targets.size() ? targets[static_cast<size_t>(slot)] : kNoId;
  const Channel* cc = cur != kNoId && cur != e.owner ? s.channel(cur) : nullptr;
  ImGui::PushID(slot + 0x7000);
  ImGui::SetNextItemWidth(width);
  if (ImGui::BeginCombo("##target", cc ? cc->name.c_str() : none)) {
    auto pick = [&](ObjectId id) {
      std::vector<ObjectId> t = targets;
      if (t.size() <= static_cast<size_t>(slot)) t.resize(static_cast<size_t>(slot) + 1, e.owner);
      t[static_cast<size_t>(slot)] = id;
      PluginState st = *e.st;
      st.data = makeNoteFxData(t, noteFxBody(st.data));
      e.set(st, "route notes", 0);
    };
    if (ImGui::Selectable(none, !cc)) pick(e.owner);
    for (const auto& c : s.channels)
      if (c.id != e.owner && ImGui::Selectable(c.name.c_str(), c.id == cur)) pick(c.id);
    ImGui::EndCombo();
  }
  ui::Hint("Channel that plays these notes");
  ImGui::PopID();
}

// Combo framed to a form rectangle (ImGui combo sized to the box).
void targetBox(const EditTarget& e, int slot, const skin::Form& F, float x, float y, float w, float h, const char* none = "This channel") {
  ImGui::SetCursorScreenPos(F.p(x, y));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, std::max(1.0f, (h * F.s - ImGui::GetFontSize()) * 0.5f)));
  targetCombo(e, slot, w * F.s, none);
  ImGui::PopStyleVar();
}

// Lane label left of a row (form units).
void laneLabel(ImDrawList* dl, const skin::Form& F, float x, float y, const skin::Skin& k, const char* s) { sk::text(dl, F, x, y, k, s, 7.5f); }

// ---------------------------------------------------------------- Arpeggiator
// No reference panel of its own (the reference arpeggiator lives in the
// channel settings): note-processor family layout.
void arpEditor(EditTarget& e) {
  const skin::Skin& K = sk::noteFx(0xFFA830);
  constexpr float kW = 600, kH = 238;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::panelBox(dl, F, 8, 8, 584, 76, K, "PATTERN");
  sk::selector(e, ArDirection, F.p(16, 26), F.sz(568, 22), K);
  laneLabel(dl, F, 16, 58, K, "RANGE");
  sk::selector(e, ArRange, F.p(60, 54), F.sz(360, 22), K);
  sk::check(e, ArLatch, F.p(440, 56), F.sz(120, 18), K, "LATCH");
  // Step strip: which note of the pattern is playing.
  {
    int pos = fxPosition(e);
    int cells = std::clamp(4 * (e.ip(ArRange) + 1), 4, 16);
    ImVec2 p0 = F.p(8, 90), p1 = F.p(592, 110);
    dl->AddRectFilled(p0, p1, K.panelLo, 3);
    float cw = (p1.x - p0.x) / static_cast<float>(cells);
    for (int i = 0; i < cells; ++i)
      dl->AddRectFilled(ImVec2(p0.x + i * cw + 2, p0.y + 3), ImVec2(p0.x + (i + 1) * cw - 2, p1.y - 3),
                        pos >= 0 && pos % cells == i ? K.fill : skin::lighten(K.panelLo, (i / 4) % 2 ? 0.14f : 0.08f), 2);
  }
  sk::panelBox(dl, F, 8, 116, 376, 80, K, "TIMING");
  sk::menuBox(e, ArRate, F.p(16, 140), F.sz(72, 22), K);
  sk::text(dl, F, 52, 168, K, "RATE", 7.5f, true);
  sk::wheelL(e, ArGate, F, 112, 132, 36, K, "GATE");
  sk::wheelL(e, ArRepeat, F, 172, 132, 36, K, "REPEATS");
  sk::wheelL(e, ArSwing, F, 232, 132, 36, K, "SWING");
  sk::wheelL(e, ArTranspose, F, 300, 132, 36, K, "TRANSPOSE");
  sk::panelBox(dl, F, 392, 116, 200, 80, K, "VELOCITY");
  sk::selector(e, ArVelMode, F.p(400, 134), F.sz(116, 56), K, nullptr, 1);
  sk::wheelL(e, ArVelocity, F, 536, 136, 34, K, "FIXED");
  sk::titleStrip(dl, F, 0, 202, kW, 36, K, "Arpeggiator", "gl.arp");
  F.end(0, kH);
}

// ------------------------------------------------------------ Chord Generator
void chordEditor(EditTarget& e) {
  const skin::Skin& K = sk::noteFx(0x87F279);
  const auto& shapes = chordShapes();
  const int rows = std::max(1, (static_cast<int>(shapes.size()) + 5) / 6);
  const float gridH = rows * 18.0f;
  const float kbY = 30 + gridH + 8;
  const float lowY = kbY + 56;
  const float kH = lowY + 90 + 40;
  constexpr float kW = 600;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::panelBox(dl, F, 8, 8, 584, kbY + 48 - 8, K, "CHORD");
  std::vector<std::string> names;
  for (const auto& s : shapes) names.push_back(s.name);
  sk::selector(e, ChType, F.p(16, 26), F.sz(568, gridH), K, &names, 6);
  if (e.st) {
    float iv[8];
    int n = chordVoicing(*e.st, iv);
    int tr = e.ip(ChTranspose);
    bool lit[128] = {};
    for (int i = 0; i < n; ++i) {
      int k = static_cast<int>(std::lround(60 + tr + iv[i]));
      if (k >= 0 && k < 128) lit[k] = true;
    }
    skin::keyboard("##chordkb", F.p(16, kbY), F.sz(568, 40), 36, 95, K, lit, -1, -1, 60);
  }
  sk::panelBox(dl, F, 8, lowY, 330, 84, K, "VOICING");
  laneLabel(dl, F, 16, lowY + 22, K, "INVERSION");
  sk::selector(e, ChInversion, F.p(84, lowY + 18), F.sz(200, 20), K);
  laneLabel(dl, F, 16, lowY + 50, K, "SPREAD");
  sk::selector(e, ChSpread, F.p(84, lowY + 46), F.sz(200, 20), K);
  sk::wheelL(e, ChTranspose, F, 294, lowY + 16, 32, K, "TRANS");
  sk::check(e, ChBass, F.p(286, lowY + 64), F.sz(50, 16), K, "BASS");
  sk::panelBox(dl, F, 346, lowY, 246, 84, K, "STRUM");
  sk::wheelL(e, ChStrum, F, 356, lowY + 18, 36, K, "STRUM");
  sk::wheelL(e, ChVelSpread, F, 406, lowY + 18, 36, K, "VEL SPRD");
  sk::selector(e, ChStrumDir, F.p(456, lowY + 24), F.sz(128, 22), K);
  sk::titleStrip(dl, F, 0, kH - 36, kW, 36, K, "Chord Generator", "gl.chord");
  F.end(0, kH);
}

// ------------------------------------------------------------- Note Sequencer
// Reference sequencer panel 984 × 560: settings column on the left, the step
// map, per-lane settings underneath, the title strip along the bottom.
void seqEditor(EditTarget& e) {
  const skin::Skin& K = sk::noteFx(0x9ED1A5);
  const int pos = fxPosition(e);
  const int len = std::clamp(e.ip(SqLength, 16), 1, kSeqSteps);
  constexpr float kW = 760, kH = 318;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  // Settings column.
  sk::panelBox(dl, F, 6, 6, 126, 268, K, "PLAYBACK");
  sk::menuBox(e, SqRate, F.p(14, 26), F.sz(110, 22), K);
  sk::text(dl, F, 69, 51, K, "RATE", 7.0f, true);
  sk::wheelL(e, SqLength, F, 18, 70, 36, K, "LENGTH");
  sk::wheelL(e, SqSwing, F, 74, 70, 36, K, "SWING");
  sk::wheelL(e, SqTranspose, F, 46, 128, 36, K, "TRANSPOSE");
  sk::check(e, SqThru, F.p(14, 190), F.sz(112, 18), K, "PASS HELD");
  sk::menuBox(e, SqVelMode, F.p(14, 248), F.sz(110, 20), K);
  if (ImGui::IsItemHovered())
    e.a->setHint("Velocity: Step x played scales each step's velocity by how hard the key is played; Step only plays the step velocity");
  {
    char buf[24];
    std::snprintf(buf, sizeof buf, "STEP %d / %d", pos >= 0 ? pos + 1 : 0, len);
    skin::lcd(dl, F.p(14, 220), F.p(124, 240), K, buf, F.font(8.0f));
  }
  // Step map.
  const float gx = 196, gw = 556, cw = gw / kSeqSteps;
  sk::panelBox(dl, F, 138, 6, 616, 268, K);
  for (int s = 0; s < kSeqSteps; ++s) {
    ImU32 c = s == pos ? K.fill : (s < len ? skin::lighten(K.panelLo, 0.18f) : K.panelLo);
    dl->AddRectFilled(F.p(gx + s * cw + 2, 12), F.p(gx + (s + 1) * cw - 2, 20), c, 2);
    if (s % 4 == 0) {
      char n[4];
      std::snprintf(n, sizeof n, "%d", s + 1);
      sk::text(dl, F, gx + s * cw + 3, 21, K, n, 6.5f, false, K.graphText);
    }
  }
  laneLabel(dl, F, 144, 36, K, "STEP");
  laneLabel(dl, F, 144, 60, K, "NOTE");
  laneLabel(dl, F, 144, 96, K, "OCTAVE");
  laneLabel(dl, F, 144, 146, K, "VELOCITY");
  laneLabel(dl, F, 144, 204, K, "GATE");
  laneLabel(dl, F, 144, 250, K, "TIE");
  static const char* sel[] = {"1", "2", "3", "4", "5", "6", "Lo", "Hi", "All", "Rnd"};
  for (int s = 0; s < kSeqSteps; ++s) {
    ImGui::PushID(s);
    bool on = e.p(seqParam(s, SqOn), 1) >= 0.5f;
    if (sk::button("##on", F.p(gx + s * cw + 1, 32), F.sz(cw - 2, 18), K, on ? "" : "-", on)) e.setParam(seqParam(s, SqOn), on ? 0 : 1, false);
    int v = std::clamp(e.ip(seqParam(s, SqNote)), 0, 9);
    ImVec2 n0 = F.p(gx + s * cw + 1, 56), n1 = F.p(gx + (s + 1) * cw - 1, 74);
    ImGui::SetCursorScreenPos(n0);
    if (ImGui::InvisibleButton("##note", ImVec2(n1.x - n0.x, n1.y - n0.y))) e.setParam(seqParam(s, SqNote), (v + 1) % 10, false);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) e.setParam(seqParam(s, SqNote), (v + 9) % 10, false);
    if (ImGui::IsItemHovered()) e.a->setHint("Which held note this step plays (click: next, right-click: previous)");
    dl->AddRectFilled(n0, n1, skin::hex(0x9ED1A5), 2);
    ImGui::PushFont(e.a->fontMonoBold, F.font(7.5f));
    ImVec2 ts = ImGui::CalcTextSize(sel[v]);
    dl->AddText(ImVec2(std::round((n0.x + n1.x - ts.x) * 0.5f), std::round((n0.y + n1.y - ts.y) * 0.5f)), skin::hex(0x2E2E2E), sel[v]);
    ImGui::PopFont();
    bool tie = e.p(seqParam(s, SqTie)) >= 0.5f;
    if (sk::button("##tie", F.p(gx + s * cw + 1, 246), F.sz(cw - 2, 18), K, tie ? "TIE" : "", tie)) e.setParam(seqParam(s, SqTie), tie ? 0 : 1, false);
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(F.p(gx, 80));
  w::barLane(e, "oct", seqParam(0, SqOctave), kSeqSteps, SqFields, F.sz(gw, 42), true, pos);
  ImGui::SetCursorScreenPos(F.p(gx, 126));
  w::barLane(e, "vel", seqParam(0, SqVelocity), kSeqSteps, SqFields, F.sz(gw, 62), false, pos);
  ImGui::SetCursorScreenPos(F.p(gx, 194));
  w::barLane(e, "gate", seqParam(0, SqGate), kSeqSteps, SqFields, F.sz(gw, 46), false, pos);
  sk::titleStrip(dl, F, 0, 282, kW, 36, K, "Note Sequencer", "gl.notesq");
  F.end(0, kH);
}

// ----------------------------------------------------------------- Key Mapper
// Reference panel 775 × 373: the map on a bevel, base key / offset / mode /
// slide row underneath, the title strip along the bottom.
void keymapEditor(EditTarget& e) {
  const skin::Skin& K = sk::noteFx(0xFF8040);
  static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  constexpr float kW = 640, kH = 272;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::panelBox(dl, F, 6, 6, 628, 160, K);
  sk::text(dl, F, 12, 10, K, "MAP  (SEMITONES PER KEY, BOTTOM = MUTED)", 7.0f);
  ImGui::SetCursorScreenPos(F.p(12, 24));
  w::barLane(e, "map", KmMap, 12, 1, F.sz(616, 92), true, -1, names);
  {
    int root = e.ip(KmRoot);
    const auto& sc = scales();
    unsigned mask = sc[static_cast<size_t>(std::clamp(e.ip(KmScale), 0, static_cast<int>(sc.size()) - 1))].mask;
    bool lit[128] = {};
    if (mask != 0xFFF)
      for (int k = 48; k <= 83; ++k) lit[k] = (mask >> (((k - root) % 12 + 12) % 12)) & 1u;
    skin::keyboard("##kmkb", F.p(12, 120), F.sz(616, 40), 48, 83, K, lit, -1, -1, 48 + root);
  }
  laneLabel(dl, F, 12, 178, K, "ROOT");
  sk::menuBox(e, KmRoot, F.p(48, 174), F.sz(54, 22), K);
  laneLabel(dl, F, 114, 178, K, "SCALE");
  sk::menuBox(e, KmScale, F.p(154, 174), F.sz(150, 22), K);
  laneLabel(dl, F, 318, 178, K, "SNAP");
  sk::selector(e, KmSnap, F.p(352, 174), F.sz(282, 22), K);
  laneLabel(dl, F, 12, 210, K, "OFFSET");
  sk::hslider(e, KmTranspose, F.p(58, 205), F.sz(118, 22), K, skin::hex(0xFF4040));
  sk::digi(e, KmTranspose, F.p(180, 206), F.sz(46, 20), K);
  laneLabel(dl, F, 240, 210, K, "OCTAVE");
  sk::hslider(e, KmOctave, F.p(288, 205), F.sz(90, 22), K, skin::hex(0xFF4040));
  sk::digi(e, KmOctave, F.p(382, 206), F.sz(40, 20), K);
  laneLabel(dl, F, 436, 210, K, "MODE");
  sk::menuBox(e, KmMulti, F.p(472, 206), F.sz(162, 20), K);
  sk::titleStrip(dl, F, 0, 236, kW, 36, K, pluginDisplayName("gl.keymap").c_str(), "gl.keymap");
  F.end(0, kH);
}

// --------------------------------------------------------------- Key Splitter
// Reference panel 941 × 388: zone selector and transpose along the top, the
// zone map over the keyboard, the title strip along the bottom.
std::map<uint64_t, int>& splitZone() {
  static std::map<uint64_t, int> m;
  return m;
}

void splitEditor(EditTarget& e) {
  const skin::Skin& K = sk::noteFx(0x4ECCFF);
  const ImU32 zoneCol[kSplitZones] = {skin::hex(0xFFA830), skin::hex(0x4ECCFF), skin::hex(0x87F279), skin::hex(0xFF6699)};
  int& zone = splitZone()[e.key];
  zone = std::clamp(zone, 0, kSplitZones - 1);
  constexpr float kW = 680, kH = 250;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  // Zone tabs (dark selector strip).
  dl->AddRectFilled(F.p(6, 6), F.p(406, 38), K.panelLo, 3);
  for (int z = 0; z < kSplitZones; ++z) {
    ImGui::PushID(z);
    char nm[16];
    std::snprintf(nm, sizeof nm, "ZONE %d", z + 1);
    bool on = e.p(splitParam(z, SpOn)) >= 0.5f;
    if (sk::button("##zone", F.p(8 + z * 99.5f, 8), F.sz(97, 28), K, nm, z == zone)) zone = z;
    skin::ledDot(dl, F.p(20 + z * 99.5f, 22), 3.5f * F.s, on, K, zoneCol[z]);
    ImGui::PopID();
  }
  static const std::vector<std::string> modes = {"ALL ZONES", "FIRST ZONE"};
  sk::selector(e, SpMode, F.p(414, 8), F.sz(260, 28), K, &modes);
  // Zone map over the keyboard.
  {
    ImVec2 m0 = F.p(6, 46), m1 = F.p(674, 86);
    skin::graphWell(dl, m0, m1, K);
    float kw = m1.x - m0.x;
    for (int z = 0; z < kSplitZones; ++z) {
      if (e.p(splitParam(z, SpOn)) < 0.5f) continue;
      float lo = e.p(splitParam(z, SpLow)), hi = e.p(splitParam(z, SpHigh), 127);
      float y = m0.y + 3 + z * (m1.y - m0.y - 6) / kSplitZones;
      float h = (m1.y - m0.y - 6) / kSplitZones - 2;
      dl->AddRectFilled(ImVec2(m0.x + lo / 128 * kw, y), ImVec2(m0.x + (hi + 1) / 128 * kw, y + h), theme::withAlpha(zoneCol[z], z == zone ? 0.9f : 0.45f), 2);
    }
    int lo = e.ip(splitParam(zone, SpLow)), hi = e.ip(splitParam(zone, SpHigh), 127);
    skin::keyboard("##splitkb", F.p(6, 88), F.sz(668, 40), 0, 127, K, nullptr, lo, hi, -1);
  }
  // Selected zone.
  ImGui::PushID(zone);
  sk::panelBox(dl, F, 6, 136, 668, 68, K);
  {
    char cap[24];
    std::snprintf(cap, sizeof cap, "ZONE %d", zone + 1);
    sk::text(dl, F, 12, 140, K, cap, 8.0f, false, zoneCol[zone]);
  }
  sk::check(e, splitParam(zone, SpOn), F.p(12, 160), F.sz(60, 18), K, "ON");
  laneLabel(dl, F, 82, 156, K, "KEYS");
  sk::digi(e, splitParam(zone, SpLow), F.p(82, 170), F.sz(52, 22), K, w::noteName(e.ip(splitParam(zone, SpLow))).c_str());
  sk::digi(e, splitParam(zone, SpHigh), F.p(138, 170), F.sz(52, 22), K, w::noteName(e.ip(splitParam(zone, SpHigh), 127)).c_str());
  laneLabel(dl, F, 204, 156, K, "TRANSPOSE");
  sk::hslider(e, splitParam(zone, SpTranspose), F.p(204, 170), F.sz(110, 22), K);
  sk::digi(e, splitParam(zone, SpTranspose), F.p(318, 170), F.sz(52, 22), K);
  laneLabel(dl, F, 384, 156, K, "VELOCITY");
  sk::digi(e, splitParam(zone, SpVelLow), F.p(384, 170), F.sz(50, 22), K);
  sk::digi(e, splitParam(zone, SpVelHigh), F.p(438, 170), F.sz(50, 22), K);
  laneLabel(dl, F, 502, 156, K, "PLAYS ON");
  targetBox(e, zone, F, 502, 170, 166, 22);
  ImGui::PopID();
  sk::titleStrip(dl, F, 0, 214, kW, 36, K, pluginDisplayName("gl.split").c_str(), "gl.split");
  F.end(0, kH);
}

// --------------------------------------------------------------- Level Scaler
// Reference panel 566 × 481: target tabs, the control panel (centre /
// multiply / offset sliders, tension / humanize wheels) beside the live curve.
void velEditor(EditTarget& e) {
  const skin::Skin& K = sk::noteFx(0xFFA830);
  if (!e.st) return;
  constexpr float kW = 566, kH = 390;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  // J-45 reference model: six targets, the selected one's controls are shown.
  const bool ref = e.ip(VsModel) == 1;
  const int tg = std::clamp(e.ip(VsEdit), 0, kVsTargets - 1);
  const int pC = ref ? vsRef(tg, VfCenter) : VsCenter, pM = ref ? vsRef(tg, VfMultiply) : VsMultiply,
            pO = ref ? vsRef(tg, VfOffset) : VsOffset, pT = ref ? vsRef(tg, VfTension) : VsTension,
            pH = ref ? vsRef(tg, VfHumanize) : VsHumanize, pHm = ref ? vsRef(tg, VfMode) : VsHumanMode;
  sk::selector(e, ref ? VsEdit : VsTarget, F.p(8, 8), F.sz(ref ? 420 : 240, 26), K);
  sk::panelBox(dl, F, 8, 38, 550, 304, K);
  const char* sl[3] = {"CENTRE", "MULTIPLY", "OFFSET"};
  const int sp[3] = {pC, pM, pO};
  for (int i = 0; i < 3; ++i) {
    sk::text(dl, F, 32 + i * 48, 46, K, sl[i], 6.5f, true);
    sk::vslider(e, sp[i], F.p(12 + i * 48, 60), F.sz(40, 184), K);
  }
  sk::wheelL(e, pT, F, 14, 252, 34, K, "TENSION");
  sk::wheelL(e, pH, F, 62, 252, 34, K, "HUMANIZE");
  if (!ref) sk::wheelL(e, VsMin, F, 110, 252, 34, K, "MIN");
  static const std::vector<std::string> hmodes = {"SUB", "SPREAD", "ADD"};
  sk::selector(e, pHm, F.p(12, 304), F.sz(142, 30), K, &hmodes);
  // Live display.
  ImVec2 p0 = F.p(166, 44), p1 = F.p(552, 336);
  skin::graphWell(dl, p0, p1, K);
  const ImVec2 sz(p1.x - p0.x, p1.y - p0.y);
  for (int g = 1; g < 4; ++g) {
    dl->AddLine(ImVec2(p0.x + sz.x * g / 4, p0.y + 1), ImVec2(p0.x + sz.x * g / 4, p1.y - 1), K.graphGrid);
    dl->AddLine(ImVec2(p0.x + 1, p0.y + sz.y * g / 4), ImVec2(p1.x - 1, p0.y + sz.y * g / 4), K.graphGrid);
  }
  double center = e.p(pC), mul = e.p(pM, 1), ofs = e.p(pO), ten = e.p(pT), hum = e.p(pH);
  int hm = e.ip(pHm, 1);
  double lo = ref ? 0.0 : e.p(VsMin), hi = ref ? 1.0 : e.p(VsMax, 1);
  if (lo > hi) std::swap(lo, hi);
  ImVec2 top[48], bot[48], mid[48];
  for (int i = 0; i < 48; ++i) {
    double x = i / 47.0;
    double y = ref ? vsRefCurve(x, center, mul, ofs, ten) : center + (tensionCurve(x, ten) - center) * mul + ofs;
    double y0 = hm == 0 ? y - hum : (hm == 2 ? y : y - hum), y1 = hm == 0 ? y : y + hum;
    if (ref) y0 = hm == 2 ? y : y - hum * y, y1 = hm == 0 ? y : y + hum * (1 - y);
    auto Y = [&](double v) { return p1.y - static_cast<float>(std::clamp(v, lo, hi)) * sz.y; };
    float X = p0.x + static_cast<float>(x) * sz.x;
    mid[i] = ImVec2(X, Y(y));
    top[i] = ImVec2(X, Y(y1));
    bot[i] = ImVec2(X, Y(y0));
  }
  if (hum > 0) {
    for (int i = 0; i + 1 < 48; ++i)
      dl->AddQuadFilled(top[i], top[i + 1], bot[i + 1], bot[i], theme::withAlpha(K.fill, 0.12f));
  }
  dl->AddPolyline(mid, 48, K.graphCurve, 0, 2.0f);
  float cy = p1.y - static_cast<float>(center) * sz.y;
  dl->AddLine(ImVec2(p0.x + 1, cy), ImVec2(p1.x - 1, cy), theme::withAlpha(K.text, 0.4f));
  // Title strip with the maximum wheel and reset (the reference keeps its
  // persistence wheel at the strip's right end).
  sk::titleStrip(dl, F, 0, 348, kW, 42, K, pluginDisplayName("gl.velscale").c_str(), "gl.velscale");
  if (!ref) {
    sk::text(dl, F, 360, 362, K, "MAX", 7.5f);
    sk::wheel(e, VsMax, F.p(386, 354), 30 * F.s, K);
  }
  if (sk::button("##reset", F.p(440, 358), F.sz(110, 22), K, "RESET CURVE")) {
    PluginState st = defaultPluginState(e.st->type);
    st.params[VsTarget] = e.p(VsTarget);
    st.params[VsModel] = e.p(VsModel);
    st.params[VsEdit] = e.p(VsEdit);
    st.data = e.st->data;
    e.set(st, "reset velocity scale", 0);
  }
  F.end(0, kH);
}

// -------------------------------------------------------------- Colour Router
// Reference panel 319 × 230: source tabs over the mapping panel (grid of
// drop-downs); targets list beside it.
void colourEditor(EditTarget& e) {
  const skin::Skin& K = sk::noteFx(0xFF8040);
  constexpr float kW = 560, kH = 254;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::selector(e, CmSource, F.p(10, 8), F.sz(390, 26), K);
  sk::panelBox(dl, F, 10, 40, 390, 170, K, "GROUP -> OUTPUT");
  {
    // Light sunken drop-downs (the reference colour mapper's style).
    skin::Skin L = K;
    L.lcdBg = skin::hex(0xAAD7AF);
    L.text = skin::hex(0x152D18);
    L.textDim = skin::hex(0x2E4A31);
    for (int g = 0; g < kColorGroups; ++g) {
      float x = 16 + (g % 4) * 95.5f, y = 60 + (g / 4) * 37;
      char lbl[8];
      std::snprintf(lbl, sizeof lbl, "%d", g + 1);
      sk::text(dl, F, x, y, K, lbl, 6.5f);
      sk::menuBox(e, CmMap + g, F.p(x, y + 10), F.sz(91, 22), L);
    }
  }
  sk::panelBox(dl, F, 408, 8, 144, 202, K, "TARGETS");
  for (int t = 0; t < 8; ++t) {
    ImGui::PushID(t);
    char lbl[8];
    std::snprintf(lbl, sizeof lbl, "%d", t + 1);
    sk::text(dl, F, 414, 30 + t * 22, K, lbl, 7.5f);
    targetBox(e, t, F, 426, 26 + t * 22, 120, 19, "(none)");
    ImGui::PopID();
  }
  sk::titleStrip(dl, F, 0, 218, kW, 36, K, "Colour Router", "gl.colormap");
  F.end(0, kH);
}

// ---------------------------------------------------------------- Note Script
// Reference panel 799 × 521: sheet tabs, compile bar, the script memo (light,
// dark text), the title strip along the bottom.
struct ScriptUi {
  char buf[2048] = {};
  std::string loaded;
  bool init = false;
};

void scriptEditor(EditTarget& e) {
  const skin::Skin& K = sk::noteFx(0xFFBC40);
  if (!e.st) return;
  static std::map<uint64_t, ScriptUi> ui;
  ScriptUi& u = ui[e.key];
  std::string body = noteFxBody(e.st->data);
  if (!u.init || (body != u.loaded && !ImGui::IsAnyItemActive())) {
    std::snprintf(u.buf, sizeof u.buf, "%s", body.empty() ? defaultNoteScript() : body.c_str());
    u.loaded = body;
    u.init = true;
  }
  constexpr float kW = 640, kH = 380;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  // Controls row.
  sk::panelBox(dl, F, 6, 6, 628, 76, K, "USER INTERFACE");
  const int abc[3] = {NsA, NsB, NsC};
  const char* nm[3] = {"a", "b", "c"};
  for (int i = 0; i < 3; ++i) {
    sk::wheel(e, abc[i], F.p(24 + i * 70, 22), 50 * F.s, K);
    ImGui::PushFont(e.a->fontMonoBold, F.font(11.0f));
    ImVec2 ts = ImGui::CalcTextSize(nm[i]);
    ImVec2 c = F.p(49 + i * 70, 47);
    dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), K.fill, nm[i]);
    ImGui::PopFont();
  }
  sk::check(e, NsEnable, F.p(250, 38), F.sz(110, 20), K, "ENABLED");
  // Compile bar + memo.
  NoteScriptProgram prog;
  compileNoteScript(u.buf, prog);
  bool running = std::string(u.buf) == (u.loaded.empty() ? defaultNoteScript() : u.loaded);
  if (sk::button("##compile", F.p(6, 90), F.sz(105, 28), K, "COMPILE") && prog.error.empty()) {
    PluginState st = *e.st;
    st.data = makeNoteFxData(noteFxTargets(st.data), u.buf);
    e.set(st, "edit note script", 0);
    u.loaded = u.buf;
  }
  {
    const char* msg = prog.error.empty() ? (running ? "Running" : "Valid - press Compile") : prog.error.c_str();
    ImGui::PushFont(e.a->fontMono, F.font(8.5f));
    dl->PushClipRect(F.p(118, 90), F.p(634, 118), true);
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(F.p(122, 0).x, std::round(F.p(0, 104).y - ts.y * 0.5f)), prog.error.empty() ? K.textDim : skin::hex(0xFF6B6B), msg);
    dl->PopClipRect();
    ImGui::PopFont();
  }
  ImGui::PushFont(e.a->fontMono, std::max(12.0f, F.font(11.0f)));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(skin::hex(0xC9D1D6)));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(skin::hex(0x101418)));
  ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, theme::vec(skin::hex(0x68B4FF, 0.6f)));
  ImGui::SetCursorScreenPos(F.p(6, 124));
  ImGui::InputTextMultiline("##script", u.buf, sizeof u.buf, F.sz(628, 168));
  ImGui::PopStyleColor(3);
  ImGui::PopFont();
  sk::text(dl, F, 8, 298, K, "Lines: pitch = ...   velocity = ...   play = ...   (# starts a comment)", 7.0f);
  sk::text(dl, F, 8, 312, K, "Inputs: pitch vel color a b c beat t n     e.g.  play = rand() < a", 7.0f);
  sk::titleStrip(dl, F, 0, 334, kW, 46, K, "Note Script", "gl.notescript");
  F.end(0, kH);
}

std::map<ObjectId, int>& selected() {
  static std::map<ObjectId, int> m;
  return m;
}

}  // namespace

void drawNoteFxEditor(App& a, ObjectId channel, int index, float width) {
  EditTarget e = noteFxTarget(a, channel, index, width);
  if (!e.st) return;
  const std::string& ty = e.st->type;
  ImGui::PushID(static_cast<int>(e.key));
  if (ty == "gl.arp") arpEditor(e);
  else if (ty == "gl.chord") chordEditor(e);
  else if (ty == "gl.notesq") seqEditor(e);
  else if (ty == "gl.keymap") keymapEditor(e);
  else if (ty == "gl.split") splitEditor(e);
  else if (ty == "gl.velscale") velEditor(e);
  else if (ty == "gl.colormap") colourEditor(e);
  else if (ty == "gl.notescript") scriptEditor(e);
  ImGui::PopID();
}

void drawChannelNoteFx(App& a, ObjectId channel, float width) {
  const auto& P = pal();
  const Channel* c = a.doc.song().channel(channel);
  if (!c) return;
  int& sel = selected()[channel];
  int n = static_cast<int>(c->noteFx.size());
  sel = n ? std::clamp(sel, 0, n - 1) : 0;
  ImGui::PushID("notefx");
  pui::beginSection("NOTE FX", width);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImGui::TextColored(theme::vec(P.textFaint), n ? "Notes pass through these processors, top to bottom, before the instrument."
                                                  : "No note processors. Add an arpeggiator, chords, a step sequencer, key mapping...");
  ImGui::PopFont();
  int removeAt = -1;
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i);
    const PluginInfo* info = findPlugin(c->noteFx[static_cast<size_t>(i)].type);
    std::string label = std::to_string(i + 1) + "  " + (info ? info->name : c->noteFx[static_cast<size_t>(i)].type);
    if (ui::TextButton(label.c_str(), i == sel, width - 120)) sel = i;
    ImGui::SameLine(0, 4);
    if (ui::IconButton("up", ui::Icon::Up, false, 24, "Move up") && i > 0) {
      a.doc.moveNoteFx(channel, i, i - 1);
      sel = i - 1;
    }
    ImGui::SameLine(0, 2);
    if (ui::IconButton("dn", ui::Icon::Down, false, 24, "Move down") && i + 1 < n) {
      a.doc.moveNoteFx(channel, i, i + 1);
      sel = i + 1;
    }
    ImGui::SameLine(0, 2);
    if (ui::IconButton("rm", ui::Icon::Trash, false, 24, "Remove")) removeAt = i;
    ImGui::PopID();
  }
  if (removeAt >= 0) a.doc.removeNoteFx(channel, removeAt);
  ImGui::SetNextItemWidth(220);
  if (n < kMaxNoteFx && ImGui::BeginCombo("##add", "Add note processor...")) {
    for (const auto& p : pluginCatalog())
      if (p.kind == PluginKind::NoteFx && ImGui::Selectable(p.name.c_str()))
        if (a.doc.addNoteFx(channel, p.type)) sel = static_cast<int>(a.doc.song().channel(channel)->noteFx.size()) - 1;
    ImGui::EndCombo();
  }
  pui::endSection();
  c = a.doc.song().channel(channel);
  if (c && sel >= 0 && sel < static_cast<int>(c->noteFx.size())) drawNoteFxEditor(a, channel, sel, width);
  ImGui::PopID();
}

}  // namespace gl::packs::modulation

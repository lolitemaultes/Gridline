// Gridline — instrument editors, reference-representative layouts (J-2):
// Trio Synth, Keys, Drum Pads, Slicer, Multisampler. Each editor places its
// controls on the reference form's own grid (form pixels, see the layout
// tables in each function) through the instrument skin (InstrumentSkin.h),
// with a per-plugin palette taken from the reference colour roles.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include "../core/I18n.h"
#include "../core/PluginParams.h"
#include "InstrumentSkin.h"
#include "PluginEditorsCommon.h"

namespace gl {

bool drawInstrumentEditorJ2(App& a, const PluginTarget& t, float width);
bool drawPadsEditorJ2(App& a, const PluginTarget& t, float width);      // PluginEditorsInstrPads.cpp
bool drawSlicerEditorJ2(App& a, const PluginTarget& t, float width);    // PluginEditorsInstrSlicer.cpp
bool drawMultiEditorJ2(App& a, const PluginTarget& t, float width);     // PluginEditorsInstrMulti.cpp

namespace {

using namespace pp;
using iskin::rgb;
using iskin::lerp;

// Opens the file dialog for an audio file; `ok` receives the chosen path.
void chooseSample(App& a, const char* title, const std::string& current, std::function<void(const std::string&)> ok) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = title;
  const char* home = std::getenv("HOME");
  a.fileDialog.dir = current.empty() || current.rfind("builtin:", 0) == 0 ? (home ? home : ".") : std::filesystem::path(current).parent_path().string();
  a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".wv"};
  a.fileDialog.onOk = std::move(ok);
}

// ============================================================================ Trio Synth
// Reference form: 558 × 389. Three oscillator rows 108 px apart:
//   number tab (-1, 23, 36×52) · shape selector (36, 11, 152×76) with the invert
//   switch in its corner (152, 51, 34×34) · stereo phase slider (211, 6, 34×85) ·
//   stereo detune slider (248, 6, 34×85) · coarse wheel (302, 14, 70) with the
//   semitone readout in its cap · fine wheel (372, 21, 56) · pan wheel (436, 21, 56) ·
//   mix wheel (487, 14, 70; osc 2 / 3).
// Caption rows at y 95 and 203; bottom strip: AM switch (5, 336, 88×31), HQ
// switch (84, 336, 45×31), phase randomness wheel (134, 321, 60), title block
// (424, 303). Below the form: the channel section (envelope · filter · LFO · voice).
constexpr float kTrioW = 558, kTrioH = 389, kTrioChanH = 150;

iskin::Palette trioPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x67737A);
  p.bgBottom = rgb(0x4E595F);
  p.panel = rgb(0x29343A);
  p.panelLine = rgb(0x1D282E);
  p.rail = rgb(0x5A656B);
  p.fill = rgb(0xB1E758);   // pitch wheels / selected shape / invert
  p.fill2 = rgb(0xFFA830);  // pan / mix / phase randomness
  p.fill3 = rgb(0xF4462D);  // stereo sliders
  p.cap = rgb(0x62676A);
  p.handle = rgb(0x273035);
  p.label = rgb(0x1D282E);
  p.text = rgb(0xE4E9EC);
  p.textDim = rgb(0x8D989E);
  p.lcdBg = rgb(0x1A1F22);
  p.lcdText = rgb(0xB1E758);
  p.led = rgb(0xB1E758);
  p.ledOff = rgb(0x394043);
  return p;
}

// Shape cells in the selector: reference order first (sine, triangle, square,
// saw | rounded saw, noise, custom) plus the 25 % pulse. Values = Trio choices.
struct ShapeCell {
  int value, icon;
};
const ShapeCell kTrioShapes[8] = {{0, 0}, {1, 1}, {3, 3}, {2, 5}, {5, 8}, {4, 4}, {6, 6}, {7, 7}};

void drawShapeIcon(ImDrawList* dl, int icon, ImVec2 c, float w, float h, ImU32 col, float th) {
  if (icon == 7) {  // custom wave: a sample outline
    ImVec2 pts[24];
    for (int i = 0; i < 24; ++i) {
      float u = i / 23.0f;
      float y = std::sin(u * 6.2831853f) * (0.6f + 0.4f * std::sin(u * 37.0f)) * std::exp(-u * 0.6f);
      pts[i] = ImVec2(c.x - w * 0.5f + w * u, c.y - y * h * 0.5f);
    }
    dl->AddPolyline(pts, 24, col, 0, th);
    return;
  }
  ed::waveIcon(dl, icon, c, w, h, col, th);
}

void trioOscRow(App& a, const PluginTarget& t, const iskin::Skin& k, const PluginState& st, int o) {
  const float dy = 108.0f * static_cast<float>(o);
  const int shapeP[3] = {SynOsc1Shape, SynOsc2Shape, SynOsc3Shape};
  const int coarseP[3] = {SynOsc1Coarse, SynOsc2Coarse, SynOsc3Coarse};
  const int fineP[3] = {SynOsc1Fine, SynOsc2Fine, SynOsc3Fine};
  const int levelP[3] = {SynOsc1Level, SynOsc2Level, SynOsc3Level};
  ImDrawList* dl = k.dl();
  ImGui::PushID(o);
  // Number tab with an arrow-cut right edge.
  {
    ImVec2 p0 = k.at(0, 23 + dy), p1 = k.at(34, 75 + dy);
    float tip = k.px(7);
    ImVec2 pts[5] = {p0, ImVec2(p1.x - tip, p0.y), ImVec2(p1.x, (p0.y + p1.y) * 0.5f), ImVec2(p1.x - tip, p1.y), ImVec2(p0.x, p1.y)};
    dl->AddConvexPolyFilled(pts, 5, k.c.panel);
    dl->AddPolyline(pts, 5, k.c.panelLine, ImDrawFlags_Closed, 1.0f);
    char num[4];
    std::snprintf(num, sizeof num, "%d", o + 1);
    iskin::text(k, 0, 23 + dy, 30, 52, num, 30, rgb(0x7D888E), 1, false, false);
  }
  // Shape selector (two rows of cells) + invert switch in its corner.
  iskin::bevel(k, 36, 11 + dy, 152, 76, k.c.panel, k.c.panelLine, 4);
  const int cur = ed::ival(st, shapeP[o]);
  auto d = describeParam(a.doc.song(), t.pid(shapeP[o]));
  for (int i = 0; i < 8; ++i) {
    const bool top = i < 4;
    const float cw = top ? 38.0f : 28.5f;
    const float cx = 36 + (top ? i : i - 4) * cw, cy = (top ? 13 : 51) + dy, ch = top ? 36 : 34;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(k.at(cx, cy));
    if (ImGui::InvisibleButton("##shape", ImVec2(k.px(cw), k.px(ch))) && cur != kTrioShapes[i].value)
      a.doc.setParam(t.pid(shapeP[o]), kTrioShapes[i].value, false);
    const bool on = cur == kTrioShapes[i].value, hov = ImGui::IsItemHovered();
    if (on) iskin::bevel(k, cx + 2, cy + 1, cw - 4, ch - 2, rgb(0x323D43), lerp(k.c.fill, k.c.panel, 0.35f), 3);
    else if (hov) iskin::bevel(k, cx + 2, cy + 1, cw - 4, ch - 2, rgb(0x2F3A40), 0, 3);
    ImVec2 c = k.at(cx + cw * 0.5f, cy + ch * 0.5f);
    drawShapeIcon(dl, kTrioShapes[i].icon, c, k.px(cw * 0.58f), k.px(ch * 0.42f), on ? k.c.fill : hov ? k.c.text : k.c.textDim,
                  std::max(1.2f, k.px(on ? 2.0f : 1.4f)));
    if (hov && d) a.setHint(d->name + ": " + d->choices[static_cast<size_t>(kTrioShapes[i].value)] + "   (click to select)");
    ImGui::PopID();
  }
  iskin::switchBtn(a, t, k, SynOsc1Invert + o, 152, 51 + dy, 34, 34, [&](ImDrawList* l, ImVec2 p0, ImVec2 p1, bool on) {
    ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    float r = (p1.x - p0.x) * 0.30f;
    l->AddCircle(c, r * 1.35f, on ? k.c.fill : k.c.rail, 24, std::max(1.0f, k.px(1.5f)));
    iskin::glyphInvert(l, c, r, on ? k.c.fill : k.c.textDim);
  });
  // Stereo phase offset + stereo detune sliders.
  iskin::slider(a, t, k, SynOsc1Phase + o, 211, 6 + dy, 34, 85, k.c.fill3, true);
  iskin::slider(a, t, k, SynOsc1Detune + o, 248, 6 + dy, 34, 85, k.c.fill3, true);
  // Coarse wheel with its semitone readout in the cap.
  iskin::wheel(a, t, k, coarseP[o], 302, 14 + dy, 70, k.c.fill, iskin::WheelCentered);
  {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%d", ed::ival(st, coarseP[o]));
    iskin::text(k, 325, 41 + dy, 24, 16, buf, 13, rgb(0xE8EDF0), 1, true, true);
  }
  iskin::wheel(a, t, k, fineP[o], 372, 21 + dy, 56, k.c.fill, iskin::WheelCentered);
  iskin::wheel(a, t, k, SynOsc1Pan + o, 436, 21 + dy, 56, k.c.fill2, iskin::WheelCentered);
  // Mix level (osc 2 / 3 in the reference; osc 1's level sits in the same column).
  if (o == 0) {
    iskin::wheel(a, t, k, levelP[o], 494, 21 + dy, 56, lerp(k.c.fill2, k.c.cap, 0.35f));
  } else {
    iskin::wheel(a, t, k, levelP[o], 487, 14 + dy, 70, k.c.fill2);
  }
  ImGui::PopID();
}

void trioCaptions(const iskin::Skin& k, float y, bool mixIcon) {
  ImDrawList* dl = k.dl();
  iskin::text(k, 176, y, 61, 15, "PHASE OFS", 10.5f, k.c.label, 1, false, true);
  iskin::text(k, 256, y, 44, 15, "DETUNE", 10.5f, k.c.label, 1, false, true);
  iskin::text(k, 314, y, 46, 15, "COARSE", 12, k.c.label, 1, false, true);
  iskin::text(k, 377, y, 46, 15, "FINE", 12, k.c.label, 1, false, true);
  iskin::glyphPan(dl, k.at(464, y + 7.5f), k.px(8), rgb(0x2D383E));
  if (mixIcon) {  // mix: a small level wedge
    ImVec2 a0 = k.at(512, y + 12), a1 = k.at(532, y + 12), a2 = k.at(532, y + 3);
    dl->AddTriangleFilled(a0, a1, a2, rgb(0x2D383E));
  }
}

void trioEnvGraph(const iskin::Skin& k, const PluginState& st, float x, float y, float w, float h) {
  iskin::bevel(k, x, y, w, h, k.c.lcdBg, k.c.panelLine, 3);
  const double at = ed::val(st, SynAttack, 0.005), dc = ed::val(st, SynDecay, 0.3), su = ed::val(st, SynSustain, 0.7), re = ed::val(st, SynRelease, 0.25);
  auto tx = [](double s) { return std::log10(1.0 + s * 40.0) / std::log10(201.0); };  // 0..5 s → 0..1
  float ax = static_cast<float>(tx(at)), dx = static_cast<float>(tx(dc)), rx = static_cast<float>(tx(re));
  float span = ax + dx + 0.35f + rx;
  float sc = (w - 10) / span;
  ImVec2 p0 = k.at(x + 5, y + h - 5);
  float top = k.at(0, y + 6).y, bot = p0.y;
  ImVec2 pts[5] = {p0, ImVec2(p0.x + k.px(ax * sc), top), ImVec2(p0.x + k.px((ax + dx) * sc), bot - (bot - top) * static_cast<float>(su)),
                   ImVec2(p0.x + k.px((ax + dx + 0.35f) * sc), bot - (bot - top) * static_cast<float>(su)),
                   ImVec2(p0.x + k.px(span * sc), bot)};
  ImDrawList* dl = k.dl();
  for (int i = 0; i < 4; ++i) {
    ImVec2 q[4] = {pts[i], pts[i + 1], ImVec2(pts[i + 1].x, bot), ImVec2(pts[i].x, bot)};
    dl->AddConvexPolyFilled(q, 4, theme::withAlpha(k.c.fill, 0.12f));
  }
  dl->AddPolyline(pts, 5, k.c.fill, 0, std::max(1.2f, k.px(1.6f)));
  for (int i = 1; i < 4; ++i) dl->AddCircleFilled(pts[i], k.px(2.2f), k.c.fill);
}

// Labelled wheel for the channel section: wheel + caption under it.
void cwheel(App& a, const PluginTarget& t, const iskin::Skin& k, int param, float x, float y, const char* cap, ImU32 fill,
            float size = 40) {
  iskin::wheel(a, t, k, param, x, y, size, fill);
  iskin::text(k, x - 8, y + size + 1, size + 16, 12, cap, 9.5f, k.c.textDim, 1, false, true);
}

void editorTrio(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  iskin::Skin k = iskin::begin(width, kTrioW, kTrioH + kTrioChanH, trioPalette());
  // Form background + soft top highlight.
  iskin::background(k, 0, 0, kTrioW, kTrioH, k.c.bgTop, k.c.bgBottom, 4);
  k.dl()->AddRectFilledMultiColor(k.at(0, 0), k.at(kTrioW, 40), theme::withAlpha(IM_COL32_WHITE, 0.06f), theme::withAlpha(IM_COL32_WHITE, 0.06f), 0, 0);
  for (int o = 0; o < 3; ++o) trioOscRow(a, t, k, st, o);
  trioCaptions(k, 95, true);
  trioCaptions(k, 203, true);
  // Bottom strip: AM, HQ, phase randomness; osc 3 / osc 2 modulation modes.
  {
    bool am = ed::ival(st, SynOsc3Mode) == SynModeAM;
    ImGui::SetCursorScreenPos(k.at(5, 336));
    if (ImGui::InvisibleButton("##am", ImVec2(k.px(84), k.px(31)))) a.doc.setParam(t.pid(SynOsc3Mode), am ? SynModeNormal : SynModeAM, false);
    if (ImGui::IsItemHovered()) a.setHint("Oscillator 3 modulates the amplitude of oscillators 1 + 2 (click to switch)");
    float bs = 12;
    iskin::bevel(k, 9, 336 + 9.5f, bs, bs, am ? k.c.led : k.c.ledOff, k.c.panelLine, 2);
    iskin::text(k, 9 + bs + 5, 336, 60, 31, "AM OSC 3", 11, am ? k.c.text : rgb(0x7D888E), 0, false, true);
  }
  iskin::check(a, t, k, SynHQ, 84, 336, 45, 31, "HQ", 11);
  iskin::wheel(a, t, k, SynPhaseRand, 134, 321, 60, k.c.fill2, 0, rgb(0x556067));
  iskin::text(k, 211, 344, 64, 15, "PHASE RAND", 10.5f, k.c.textDim, 0, false, true);
  // Extra modulation routings (Trio): osc 3 mode and osc 2 → osc 1, as LCD selectors.
  iskin::text(k, 286, 318, 62, 12, "OSC 3 MODE", 9, k.c.textDim, 0, false, true);
  iskin::digi(a, t, k, SynOsc3Mode, 286, 331, 62, 18, {}, 10.5f);
  iskin::text(k, 354, 318, 62, 12, "OSC 2 > 1", 9, k.c.textDim, 0, false, true);
  iskin::digi(a, t, k, SynOsc2Mod, 354, 331, 62, 18, {}, 10.5f);
  // Custom wave: sample name + load.
  {
    bool anyCustom = ed::ival(st, SynOsc1Shape) == 7 || ed::ival(st, SynOsc2Shape) == 7 || ed::ival(st, SynOsc3Shape) == 7;
    std::string nm = st.data.empty() ? "(no sample)" : std::filesystem::path(st.data).filename().string();
    iskin::text(k, 286, 356, 130, 12, anyCustom ? "CUSTOM WAVE" : "CUSTOM WAVE (unused)", 9, k.c.textDim, 0, false, true);
    if (iskin::button(k, "##cwave", 286, 368, 130, 16, nm.c_str(), false, 9) && t.isChannel())
      chooseSample(a, "Custom wave sample", st.data, [&a, owner = t.owner](const std::string& p) {
        a.doc.editChannel(owner, "load custom wave", [&](Channel& c) { c.plugin.data = p; });
      });
    if (ImGui::IsItemHovered()) a.setHint("Custom-wave oscillators play one cycle of this sample (click to load)");
  }
  // Title block (Gridline's own name).
  iskin::text(k, 424, 303, 129, 50, "Trio", 40, rgb(0x7D888E), 2, false, true);
  iskin::text(k, 424, 349, 129, 24, "SYNTH", 15, rgb(0x5D686E), 2, false, true);

  // ---- channel section (envelope · filter · LFO · voice), same palette.
  const float y0 = kTrioH + 6, h = kTrioChanH - 8;
  iskin::background(k, 0, y0, kTrioW, h, rgb(0x46515A), rgb(0x3B454C), 4);
  auto section = [&](float x, float w, const char* title) {
    iskin::bevel(k, x, y0 + 6, w, h - 12, theme::withAlpha(IM_COL32_BLACK, 0.10f), k.c.panelLine, 4);
    iskin::text(k, x + 6, y0 + 8, w - 12, 13, title, 9.5f, k.c.textDim, 0, false, true);
  };
  section(4, 176, "AMP ENVELOPE");
  trioEnvGraph(k, st, 10, y0 + 23, 164, 42);
  const ImU32 env = k.c.fill;
  cwheel(a, t, k, SynAttack, 14, y0 + 72, "ATT", env, 30);
  cwheel(a, t, k, SynDecay, 56, y0 + 72, "DEC", env, 30);
  cwheel(a, t, k, SynSustain, 98, y0 + 72, "SUS", env, 30);
  cwheel(a, t, k, SynRelease, 140, y0 + 72, "REL", env, 30);
  section(184, 196, "FILTER");
  iskin::digi(a, t, k, SynFilterType, 190, y0 + 23, 76, 16, {}, 10);
  cwheel(a, t, k, SynCutoff, 192, y0 + 46, "CUT", k.c.fill2, 34);
  cwheel(a, t, k, SynResonance, 236, y0 + 46, "RES", k.c.fill2, 34);
  cwheel(a, t, k, SynFilterEnv, 280, y0 + 24, "ENV", k.c.fill3, 30);
  cwheel(a, t, k, SynFilterDecay, 320, y0 + 24, "E.DEC", k.c.fill3, 30);
  cwheel(a, t, k, SynKeyTrack, 280, y0 + 80, "KEY", k.c.fill3, 30);
  cwheel(a, t, k, SynVelCutoff, 320, y0 + 80, "VEL", k.c.fill3, 30);
  section(384, 170, "LFO \xC2\xB7 VOICE");
  iskin::digi(a, t, k, SynLfoTarget, 390, y0 + 23, 76, 16, {}, 10);
  iskin::digi(a, t, k, SynLfoShape, 470, y0 + 23, 78, 16, {}, 10);
  cwheel(a, t, k, SynLfoRate, 392, y0 + 46, "RATE", k.c.fill, 30);
  cwheel(a, t, k, SynLfoAmount, 432, y0 + 46, "AMT", k.c.fill, 30);
  cwheel(a, t, k, SynGlide, 472, y0 + 46, "GLIDE", k.c.fill, 30);
  cwheel(a, t, k, SynNoise, 512, y0 + 46, "NOISE", k.c.fill, 30);
  iskin::text(k, 390, y0 + 96, 50, 12, "VOICES", 9, k.c.textDim, 0, false, true);
  iskin::digi(a, t, k, SynVoices, 390, y0 + 109, 44, 18, [](double v) { return std::to_string(static_cast<int>(std::lround(v))); }, 11);
  cwheel(a, t, k, SynVolume, 472, y0 + 92, "VOLUME", k.c.fill2, 30);
  // J-65: reference oscillator model (sample-exact to the imported instrument's oscillators).
  iskin::check(a, t, k, SynModel, 437, y0 + 102, 34, 24, "REF", 9.5f);
  iskin::end(k, kTrioH + kTrioChanH);
}

// ============================================================================ Keys
// Reference form: 480 × 270, dark panel. Two rows of 42 px wheels (y 62 and 124):
//   row 1: decay (14) · release (72) · overdrive (135) · LFO rate (192) ·
//          velocity → muffle (255) · muffle (311) · tune (425)
//   row 2: pan / tremolo (14) · stereo (72) · treble (135) · stretch (192) ·
//          velocity → hardness (255) · hardness (311) · velocity sensitivity (364) · detune (425)
// Sound-set selector (328, 9, 127×19) + info button (454, 9); keyboard well
// (0, 183, 480×89) with the keyboard at (9, 190, 462×71).
constexpr float kKeysW = 480, kKeysH = 270;

iskin::Palette keysPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x262626);
  p.bgBottom = rgb(0x1A1A1A);
  p.panel = rgb(0x2C2C2C);
  p.panelLine = rgb(0x111111);
  p.rail = rgb(0x707072);
  p.fill = rgb(0x393935);
  p.fill2 = rgb(0xFE8025);
  p.fill3 = rgb(0x6C6C6C);
  p.cap = rgb(0x828282);
  p.pointer = rgb(0x262624);
  p.label = rgb(0x9A9A96);
  p.text = rgb(0xE6E6E2);
  p.textDim = rgb(0x8A8A86);
  p.lcdBg = rgb(0x9F9F9F);
  p.lcdText = rgb(0x363636);
  p.led = rgb(0xFE8025);
  p.ledOff = rgb(0x3A3A3A);
  return p;
}

void editorKeys(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  iskin::Skin k = iskin::begin(width, kKeysW, kKeysH, keysPalette());
  iskin::background(k, 0, 0, kKeysW, kKeysH, k.c.bgTop, k.c.bgBottom, 4);
  iskin::bevel(k, 0, 183, kKeysW, 87, rgb(0x252525), 0, 0);
  // Title.
  iskin::text(k, 14, 6, 120, 32, "KEYS", 26, rgb(0xD8D8D4), 0, false, true);
  // Sound set (model) selector + info.
  iskin::digi(a, t, k, KyModel, 328, 9, 127, 19, {}, 10.5f);
  iskin::bevel(k, 456, 9, 16, 19, k.c.lcdBg, k.c.panelLine, 2);
  iskin::text(k, 456, 9, 16, 19, "i", 11, k.c.lcdText, 1, false, true);
  ImGui::SetCursorScreenPos(k.at(456, 9));
  ImGui::InvisibleButton("##kinfo", ImVec2(k.px(16), k.px(19)));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", i18n::tr("Tine / Reed / Bright tine / Soft: modelled electric pianos\nGrand / Upright piano: hammered-string models"));
  // Extras in the header strip: volume, key-tracked pan, velocity curve.
  iskin::wheel(a, t, k, KyVolume, 170, 8, 28, k.c.fill2);
  iskin::text(k, 160, 35, 48, 9, "VOLUME", 8, k.c.textDim, 1, false, true);
  iskin::wheel(a, t, k, KyKeyPan, 214, 8, 28, k.c.fill2);
  iskin::text(k, 204, 35, 48, 9, "KEY PAN", 8, k.c.textDim, 1, false, true);
  iskin::digi(a, t, k, KyVelCurve, 254, 13, 64, 17, {}, 9.5f);
  iskin::text(k, 254, 32, 64, 10, "VEL CURVE", 8, k.c.textDim, 1, false, true);
  // Group captions.
  auto group = [&](float x, float w, const char* s) {
    iskin::text(k, x, 47, w, 10, s, 9, k.c.label, 1, false, true);
    k.dl()->AddLine(k.at(x + 2, 58), k.at(x + w - 2, 58), rgb(0x3A3A3A), 1.0f);
  };
  group(10, 108, "ENVELOPE");
  group(131, 108, "DRIVE \xC2\xB7 LFO");
  group(251, 106, "MUFFLE");
  group(420, 52, "TUNE");
  auto w = [&](int param, float x, float y, const char* cap, int flags = iskin::WheelBigLine) {
    iskin::wheel(a, t, k, param, x, y, 42, k.c.fill, flags);
    iskin::text(k, x - 10, y + 42, 62, 12, cap, 8.5f, k.c.textDim, 1, false, true);
  };
  w(KyDecay, 14, 62, "DECAY");
  w(KyRelease, 72, 62, "RELEASE");
  w(KyDrive, 135, 62, "OVERDRIVE");
  w(KyTremRate, 192, 62, "LFO RATE");
  w(KyVelMuffle, 255, 62, "VEL");
  w(KyMuffle, 311, 62, "MUFFLE");
  w(KyTune, 425, 62, "TUNE");
  // Row 2.
  iskin::wheel(a, t, k, KyTremDepth, 14, 124, 42, k.c.fill, iskin::WheelBigLine);
  {  // pan / tremolo switch under the amount wheel
    const bool pan = ed::ival(st, KyModMode) == 1;
    if (iskin::button(k, "##trem", 3, 167, 31, 12, "TREM", !pan, 7.5f)) a.doc.setParam(t.pid(KyModMode), 0, false);
    if (iskin::button(k, "##apan", 36, 167, 31, 12, "PAN", pan, 7.5f)) a.doc.setParam(t.pid(KyModMode), 1, false);
  }
  w(KyStereo, 72, 124, "SPREAD");
  w(KyTreble, 135, 124, "TREBLE");
  w(KyStretch, 192, 124, "STRETCH");
  w(KyVelHardness, 255, 124, "VEL");
  w(KyHardness, 311, 124, "HARDNESS");
  w(KyVelocity, 364, 124, "VEL SENS");
  w(KyDetune, 425, 124, "DETUNE");
  // Keyboard.
  iskin::KeyColors kc;
  iskin::keyboard(a, t, k, 9, 190, 462, 71, 48, 22, kc);
  iskin::end(k, kKeysH);
}

}  // namespace

bool drawInstrumentEditorJ2(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty == "gl.synth") {
    editorTrio(a, t, width);
    return true;
  }
  if (ty == "gl.keys") {
    editorKeys(a, t, width);
    return true;
  }
  if (ty == "gl.pads") return drawPadsEditorJ2(a, t, width);
  if (ty == "gl.slicer") return drawSlicerEditorJ2(a, t, width);
  return false;
}

}  // namespace gl

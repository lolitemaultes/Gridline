// Gridline — "spectral" pack: Turntable (gl.turntable) editor. The platter is
// the main surface: grab it and turn it to scratch (the hand holds the record,
// Position follows the drag), the waveform below shows the play position and
// scratches the same way when dragged sideways. Hand-spring controls on the
// left, motor controls on the right, the motor switch under the platter, the
// sample row and the waveform at the bottom. All controls sit on shared row
// centrelines (form units, scaled by skin::Form).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include "../../app/App.h"
#include "../../app/PluginSkin.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../core/I18n.h"
#include "../../engine/Program.h"
#include "../models/metal_widgets.h"
#include "../spacefx/sfx_skin.h"
#include "turntable.h"

namespace gl::packs::spectral {

namespace {

using models::metal::ui::openSampleDialog;
using models::metal::ui::sampleOf;
using models::metal::ui::waveform;

constexpr float kTau = 6.2831853f;
// Form (reference units).
constexpr float kW = 496, kH = 508;
constexpr float kLeftCx = 49, kRightCx = 447;       // column centres
constexpr float kPlatterCx = 248, kPlatterCy = 150, kPlatterR = 128;
constexpr float kRowA = 292, kRowB = 326, kRowH = 24;  // motor row, sample row
constexpr float kWaveY = 360, kWaveH = 136;
// Mouse travel: the reference moves 0.25 % of the sample per pixel at full
// sensitivity on a form half this size, so 0.125 % per form unit here.
constexpr double kPerUnit = 0.00125;

const skin::Skin& deckSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x2A3033, 0x5F696D, 0xEDEFF1, 0x667279, 0xB9BFC3);
    s.fillPressed = skin::hex(0xFFA830);
    s.select = skin::hex(0xF8895A);
    s.lcdBg = skin::hex(0x232829);
    s.graphBg = skin::hex(0x232829);
    s.graphGrid = skin::hex(0x30373A);
    s.handle = skin::hex(0x6A7579);
    s.accent2 = skin::hex(0xFFA830);
    return s;
  }();
  return k;
}

float prm(const PluginState& s, int k) {
  if (k >= 0 && static_cast<size_t>(k) < s.params.size()) return s.params[static_cast<size_t>(k)];
  const PluginInfo* info = findPlugin("gl.turntable");
  return info && static_cast<size_t>(k) < info->params.size() ? static_cast<float>(info->params[static_cast<size_t>(k)].def) : 0.f;
}
void setP(App& a, const PluginTarget& t, int k, double v, bool merge) { a.doc.setParam(t.pid(k), v, merge); }

void commitPath(App& a, const PluginTarget& t, const std::string& p) {
  const PluginState* now = t.state(a.doc.song());
  if (!now) return;
  PluginState ns = *now;
  ns.data = p;
  setPluginState(a, t, ns, "load sample");
}

// One hand gesture on the platter or the waveform (one at a time).
struct Hand {
  int where = 0;  // 0 none, 1 platter, 2 waveform
  float holdBefore = 0;
  double pos = 0;
  int turns = 0;
  float lastAngle = 0, lastX = 0;
};
Hand& hand() {
  static Hand h;
  return h;
}

void grab(App& a, const PluginTarget& t, const PluginState& st, int where, double livePos) {
  Hand& h = hand();
  h.where = where;
  h.holdBefore = prm(st, TtHold);
  h.pos = std::clamp(livePos, 0.0, 1.0);
  h.turns = 0;
  setP(a, t, TtPosition, h.pos, true);
  setP(a, t, TtTurns, 0, true);
  setP(a, t, TtHold, 1, true);
}
// Moves the hand by `units` form units of travel (positive = forward).
void move(App& a, const PluginTarget& t, const PluginState& st, double units) {
  Hand& h = hand();
  if (units == 0) return;
  const double fine = ImGui::GetIO().KeyShift ? 0.25 : 1.0;
  h.pos += units * kPerUnit * std::clamp(static_cast<double>(prm(st, TtSense)), 0.0, 1.0) * fine;
  while (h.pos >= 1.0 && h.turns < 9) h.pos -= 1.0, ++h.turns;
  while (h.pos < 0.0 && h.turns > -9) h.pos += 1.0, --h.turns;
  h.pos = std::clamp(h.pos, 0.0, 1.0);
  setP(a, t, TtPosition, h.pos, true);
  setP(a, t, TtTurns, h.turns, true);
}
void release(App& a, const PluginTarget& t) {
  Hand& h = hand();
  setP(a, t, TtHold, h.holdBefore >= 0.5f ? 1 : 0, true);
  h.where = 0;
  a.doc.endGesture();
}

void timeText(char* buf, size_t n, double sec) {
  const int m = static_cast<int>(sec / 60.0);
  std::snprintf(buf, n, "%d:%05.2f", m, sec - m * 60.0);
}

// File name for the sample row: built-in phrase, missing, or the file's name
// (shortened from the front to fit `maxChars`).
std::string sampleName(const PluginState& st, bool missing, size_t maxChars) {
  if (st.data.empty()) return "Built-in phrase";
  std::string n = std::filesystem::u8path(st.data).filename().u8string();
  if (missing) n = "Missing: " + n;
  if (n.size() > maxChars && maxChars > 3) n = n.substr(0, maxChars - 3) + "...";
  return n;
}

void drawPlatter(ImDrawList* dl, const skin::Form& F, const skin::Skin& K, float pos, bool held, bool loaded) {
  const ImU32 orange = skin::hex(0xFFA830);
  const ImVec2 c = F.p(kPlatterCx, kPlatterCy);
  const float R = F.u(kPlatterR);
  // Plinth shadow, rim, vinyl.
  dl->AddCircleFilled(ImVec2(c.x, c.y + F.u(2)), R + F.u(4), skin::darken(K.panel, 0.45f), 128);
  dl->AddCircleFilled(c, R + F.u(3), skin::hex(0x4A5357), 128);
  dl->AddCircleFilled(c, R, skin::hex(0x151819), 128);
  // Grooves.
  for (float r = R * 0.40f; r < R - F.u(6); r += F.u(4)) dl->AddCircle(c, r, theme::withAlpha(K.text, 0.05f), 128, 1.0f);
  const float ang = pos * kTau - kTau * 0.25f;  // one turn = the whole sample, 0 at the top
  // Strobe dots on the rim turn with the record.
  for (int i = 0; i < 48; ++i) {
    const float a = ang + kTau * static_cast<float>(i) / 48.0f;
    const ImVec2 d(c.x + std::cos(a) * (R + F.u(1.5f)), c.y + std::sin(a) * (R + F.u(1.5f)));
    dl->AddCircleFilled(d, F.u(1.1f), theme::withAlpha(K.text, i == 0 ? 0.9f : 0.28f), 8);
  }
  // Centre label with its marker line (the platter angle at a glance).
  const float rl = R * 0.33f;
  dl->AddCircleFilled(c, rl, loaded ? orange : skin::darken(orange, 0.5f), 64);
  dl->AddCircle(c, rl * 0.72f, theme::withAlpha(skin::hex(0x151819), 0.35f), 64, 1.0f);
  const ImVec2 m0(c.x + std::cos(ang) * rl * 0.18f, c.y + std::sin(ang) * rl * 0.18f);
  const ImVec2 m1(c.x + std::cos(ang) * (R - F.u(5)), c.y + std::sin(ang) * (R - F.u(5)));
  dl->AddLine(m0, m1, theme::withAlpha(K.text, 0.85f), std::max(2.0f, F.u(2.5f)));
  dl->AddCircleFilled(c, F.u(4), skin::hex(0xD8DCDE), 16);  // spindle
  if (held) dl->AddCircle(c, R + F.u(6), theme::withAlpha(orange, 0.8f), 128, std::max(1.5f, F.u(2)));
}

void drawWave(ImDrawList* dl, const skin::Form& F, const skin::Skin& K, const Sample* smp, bool missing, float pos, bool showCue, float cue,
              float speed) {
  const ImU32 orange = skin::hex(0xFFA830);
  const ImVec2 w0 = F.p(12, kWaveY), w1 = F.p(12 + 472, kWaveY + kWaveH);
  skin::graphWell(dl, w0, w1, K);
  const ImVec2 q0(w0.x + F.u(4), w0.y + F.u(18)), q1(w1.x - F.u(4), w1.y - F.u(4));
  const float fs = F.font(7.5f);
  if (!smp || smp->frames() < 2) {
    const char* msg = missing ? "Sample not found: load it again" : "No sample";
    ImGui::PushFont(nullptr, fs);
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    ImGui::PopFont();
    skin::text(dl, ImVec2(std::round((w0.x + w1.x - ts.x) * 0.5f), std::round((w0.y + w1.y - ts.y) * 0.5f)), K.textDim, msg, fs);
    return;
  }
  for (int i = 1; i < 8; ++i) {
    const float x = q0.x + (q1.x - q0.x) * static_cast<float>(i) / 8.0f;
    dl->AddLine(ImVec2(x, q0.y), ImVec2(x, q1.y), K.graphGrid, 1.0f);
  }
  waveform(dl, smp, q0, q1, theme::withAlpha(K.fill, 0.55f));
  if (showCue) {
    const float x = q0.x + (q1.x - q0.x) * std::clamp(cue, 0.0f, 1.0f);
    dl->AddLine(ImVec2(x, q0.y), ImVec2(x, q1.y), theme::withAlpha(K.select, 0.8f), 1.0f);
    dl->AddTriangleFilled(ImVec2(x - F.u(4), q0.y), ImVec2(x + F.u(4), q0.y), ImVec2(x, q0.y + F.u(5)), K.select);
  }
  const float x = q0.x + (q1.x - q0.x) * std::clamp(pos, 0.0f, 1.0f);
  dl->AddLine(ImVec2(x, q0.y), ImVec2(x, q1.y), orange, std::max(1.5f, F.u(1.5f)));
  // Readouts in the well's top strip: position / length left, platter speed right.
  char a[24], b[24], line[64];
  timeText(a, sizeof a, pos * smp->seconds());
  timeText(b, sizeof b, smp->seconds());
  std::snprintf(line, sizeof line, "%s / %s", a, b);
  const float ty = w0.y + F.u(4);
  skin::text(dl, ImVec2(std::round(w0.x + F.u(6)), std::round(ty)), K.graphText, line, fs);
  std::snprintf(line, sizeof line, "PLATTER %+.0f%%", static_cast<double>(speed) * 100.0);
  ImGui::PushFont(nullptr, fs);
  const float lw = ImGui::CalcTextSize(line).x;
  ImGui::PopFont();
  skin::text(dl, ImVec2(std::round(w1.x - F.u(6) - lw), std::round(ty)), K.graphText, line, fs);
}

void optionsPopup(App& a, const PluginTarget& t, const PluginState& st) {
  if (!ImGui::BeginPopup("##ttopts")) return;
  ImGui::TextDisabled("%s", i18n::tr("Notes"));
  const int notes = static_cast<int>(std::lround(prm(st, TtNotes)));
  static const char* modes[3] = {"Off: the motor switch only", "Motor: a held key runs the motor", "Cue + motor: each note starts at the cue"};
  for (int i = 0; i < 3; ++i)
    if (ImGui::RadioButton(i18n::tr(modes[i]), notes == i)) setP(a, t, TtNotes, i, false);
  bool kt = prm(st, TtKeyTrack) >= 0.5f;
  if (ImGui::Checkbox(i18n::tr("Key track (C5 plays at Speed)"), &kt)) setP(a, t, TtKeyTrack, kt ? 1 : 0, false);
  float cue = prm(st, TtCue) * 100.0f;
  ImGui::SetNextItemWidth(220);
  if (ImGui::SliderFloat(i18n::tr("Cue point"), &cue, 0.0f, 100.0f, "%.1f %%")) setP(a, t, TtCue, cue / 100.0f, true);
  if (ImGui::IsItemDeactivatedAfterEdit()) a.doc.endGesture();
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("Ctrl+click the waveform to set the cue point."));
  ImGui::EndPopup();
}

}  // namespace

void editorTurntable(App& a, const PluginTarget& t, float width) {
  using namespace flskin;
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = deckSkin();
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const ImU32 led = skin::hex(0xF8895A);

  // Live platter (the runtime of this channel; the parameters when none runs).
  const TurntableTelemetry* tel = nullptr;
  std::shared_ptr<ChannelRuntime> rt;
  if (t.isChannel()) {
    rt = a.builder.channelRuntime(t.owner);
    if (rt && rt->gen) tel = turntableTelemetry(rt->gen.get());
  }
  const SamplePtr smp = sampleOf(st.data);
  const bool missing = !st.data.empty() && !smp;
  const float pos = tel ? tel->pos.load(std::memory_order_relaxed) : prm(st, TtPosition);
  const float speed = tel ? tel->speed.load(std::memory_order_relaxed) : 0.0f;
  const bool held = tel ? tel->held.load(std::memory_order_relaxed) : prm(st, TtHold) >= 0.5f;

  // ---- left column: hand spring (2 x 2), pan, volume
  cap(dl, F, kLeftCx, 4, "SMOOTH", K.textDim);
  {
    static const char* nums[4] = {"1", "2", "3", "4"};
    for (int i = 0; i < 4; ++i) {
      const float x = kLeftCx - 36 + 40 * static_cast<float>(i % 2), y = 18 + 50 * static_cast<float>(i / 2);
      W(a, t, TtSmooth1 + i, F, x, y, 32, K, nums[i]);
    }
  }
  if (ImGui::IsItemHovered()) a.setHint("Hand spring: 1 settle time, 2 momentum, 3 stiffness, 4 carry");
  W(a, t, TtPan, F, kLeftCx - 28, 134, 56, K, "PAN", skin::WheelBigLine | skin::WheelCentered);
  W(a, t, TtVolume, F, kLeftCx - 28, 212, 56, K, "VOLUME");

  // ---- right column: turns readout, speed, acceleration, sensitivity
  cap(dl, F, kRightCx, 4, "TURNS", K.textDim);
  {
    char b[16];
    std::snprintf(b, sizeof b, "%+d", static_cast<int>(std::lround(prm(st, TtTurns))));
    skin::lcd(dl, F.p(kRightCx - 37, 16), F.p(kRightCx + 37, 38), K, b, F.font(8.0f));
  }
  W(a, t, TtSpeed, F, kRightCx - 28, 56, 56, K, "SPEED");
  W(a, t, TtAccel, F, kRightCx - 28, 134, 56, K, "ACCEL");
  if (ImGui::IsItemHovered()) a.setHint("How quickly the platter reaches the motor speed");
  W(a, t, TtSense, F, kRightCx - 28, 212, 56, K, "SENSE");
  if (ImGui::IsItemHovered()) a.setHint("Mouse travel per pixel when scratching the platter or the waveform");

  // ---- platter
  {
    drawPlatter(dl, F, K, pos, held, smp != nullptr);
    const ImVec2 c = F.p(kPlatterCx, kPlatterCy);
    const float R = F.u(kPlatterR + 4);
    ImGui::SetCursorScreenPos(ImVec2(c.x - R, c.y - R));
    ImGui::InvisibleButton("##platter", ImVec2(2 * R, 2 * R));
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float dx = m.x - c.x, dy = m.y - c.y;
    const bool inside = dx * dx + dy * dy <= R * R;
    const float ma = std::atan2(dy, dx);
    Hand& h = hand();
    if (ImGui::IsItemActivated() && inside) {
      grab(a, t, st, 1, pos);
      h.lastAngle = ma;
    }
    if (ImGui::IsItemActive() && h.where == 1) {
      float d = ma - h.lastAngle;
      if (d > kTau * 0.5f) d -= kTau;
      if (d < -kTau * 0.5f) d += kTau;
      h.lastAngle = ma;
      // Travel along the rim at the cursor's radius (clockwise = forward), in form units.
      const float r = std::max(F.u(24), std::sqrt(dx * dx + dy * dy));
      move(a, t, st, static_cast<double>(d * r / F.s));
      a.setHint("Scratching: clockwise plays forward, anticlockwise pulls the record back (Shift: fine)");
    }
    if (ImGui::IsItemDeactivated() && h.where == 1) release(a, t);
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive() && inside) a.setHint("Grab the record and turn it to scratch (Shift: fine)");
  }

  // ---- motor row: hold, motor switch, mute
  chk(a, t, TtHold, F, kLeftCx - 37, kRowA, 74, kRowH, K, "HOLD", led);
  if (ImGui::IsItemHovered()) a.setHint("The hand holds the record: the platter follows Position");
  {
    static const char* labels[3] = {"REVERSE", "PAUSE", "FORWARD"};
    sel(a, t, TtTransport, F, kPlatterCx - 124, kRowA, 248, kRowH, K, true, labels);
    if (ImGui::IsItemHovered()) a.setHint("Motor: the platter runs toward this direction at Speed");
  }
  chk(a, t, TtMute, F, kRightCx - 37, kRowA, 74, kRowH, K, "MUTE", led);
  if (ImGui::IsItemHovered()) a.setHint("Cut the output (a crossfader cut)");

  // ---- sample row: load, name, options
  if (btn("##ttload", F, kLeftCx - 37, kRowB, 74, kRowH, K, "LOAD", false)) {
    const PluginTarget tt = t;
    openSampleDialog(a, "Load sample", [&a, tt](const std::string& p) { commitPath(a, tt, p); });
  }
  if (ImGui::IsItemHovered()) a.setHint("Load a sample onto the record (or drop one on the editor)");
  {
    const float fs = F.font(8.0f);
    const float lw = F.u(306);
    const size_t maxChars = static_cast<size_t>(std::max(4.0f, lw / (fs * 0.56f)));
    const std::string name = sampleName(st, missing, maxChars);
    skin::lcd(dl, F.p(kPlatterCx - 153, kRowB), F.p(kPlatterCx + 153, kRowB + kRowH), K, name.c_str(), fs);
  }
  if (btn("##ttopt", F, kRightCx - 37, kRowB, 74, kRowH, K, "NOTES", false)) ImGui::OpenPopup("##ttopts");
  if (ImGui::IsItemHovered()) a.setHint("How notes play the record: motor, cue point and key tracking");
  optionsPopup(a, t, st);

  // ---- waveform: play position, cue, drag to scratch
  {
    const bool showCue = std::lround(prm(st, TtNotes)) == TnCue;
    drawWave(dl, F, K, smp.get(), missing, pos, showCue, prm(st, TtCue), speed);
    const ImVec2 w0 = F.p(12, kWaveY), w1 = F.p(12 + 472, kWaveY + kWaveH);
    ImGui::SetCursorScreenPos(w0);
    ImGui::InvisibleButton("##ttwave", ImVec2(w1.x - w0.x, w1.y - w0.y));
    Hand& h = hand();
    const float mx = ImGui::GetIO().MousePos.x;
    if (ImGui::IsItemActivated()) {
      if (ImGui::GetIO().KeyCtrl) {
        const float q0 = w0.x + F.u(4), q1 = w1.x - F.u(4);
        setP(a, t, TtCue, std::clamp((mx - q0) / std::max(1.0f, q1 - q0), 0.0f, 1.0f), false);
      } else if (smp) {
        grab(a, t, st, 2, pos);
        h.lastX = mx;
      }
    }
    if (ImGui::IsItemActive() && h.where == 2) {
      move(a, t, st, static_cast<double>((mx - h.lastX) / F.s));
      h.lastX = mx;
      a.setHint("Scratching: drag right plays forward, left pulls back (Shift: fine)");
    }
    if (ImGui::IsItemDeactivated() && h.where == 2) release(a, t);
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) a.setHint("Drag sideways to scratch; Ctrl+click sets the cue point");
  }
  F.end(kW, kH);
}

}  // namespace gl::packs::spectral

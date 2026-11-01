// Gridline — "spectral" pack: Turntable (gl.turntable) editor (J-206). Laid
// out on the reference deck's panel (247 x 303 reference px, scaled to the
// window): the four hand-spring wheels, pan and volume, Hold and Mute on the
// left; the platter in the middle with the motor selector under it; Turns,
// Speed, Acceleration and Sensitivity on the right; the load button, the
// sample name, the options and zoom buttons; the waveform scope at the bottom.
//
// Scratching: grab the platter (inside the record) or the scope. The hand
// takes the record (Hold on, Turns 0, Position = where it plays now) and the
// mouse moves it by 0.25 % of the sample per reference pixel at full
// Sensitivity (Ctrl: a quarter of that): up and down on the platter, sideways
// on the scope. Passing the end of the sample adds a turn (up to 9 either
// way). Letting go hands the record back to the motor (Hold off).
//
// Dropping an audio file anywhere on the editor (browser, desktop, a playlist
// clip, a channel row) loads it onto the record, like the load button.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "../../third_party/imgui/imgui_internal.h"
#include "../../app/App.h"
#include "../../app/BrowserUi.h"
#include "../../app/ExternalDragUi.h"
#include "../../app/PluginSkin.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../engine/Program.h"
#include "../models/metal_widgets.h"
#include "../spacefx/sfx_skin.h"
#include "turntable.h"

namespace gl::packs::spectral {

namespace {

using models::metal::ui::openSampleDialog;
using models::metal::ui::waveform;

constexpr float kTau = 6.2831853f;
// The reference panel (px) and its parts.
constexpr float kW = 247, kH = 303;
constexpr float kPlX = 51, kPlY = 12, kPlD = 150;  // platter box
constexpr float kScX = 3, kScY = 229, kScW = 241, kScH = 71;
constexpr float kZoomGrow = 50;  // the large editor: a taller scope (the reference's zoom grows it 1.7 x)
// Mouse travel per reference px at full sensitivity (share of the sample).
constexpr double kPerPx = 0.0025;

// Colour roles of the reference panel.
constexpr int kPanel = 0x31373B, kRail = 0x5F696D, kFill = 0xEDEFF1, kCap = 0x667279, kOrange = 0xFFA830, kLed = 0xF8895A,
              kButton = 0x6A7579, kPathBg = 0x354651, kPathText = 0xB9BFC3, kOverlay = 0x404040, kWell = 0x22272A, kCaption = 0x9AA3A7;

const skin::Skin& deckSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(kPanel, 0x2A3033, kRail, kFill, kCap, kPathText);
    s.fillPressed = skin::hex(kOrange);
    s.select = skin::hex(kLed);
    s.lcdBg = skin::hex(kWell);
    s.graphBg = skin::hex(kWell);
    s.graphGrid = skin::hex(0x30373A);
    s.handle = skin::hex(kButton);
    s.accent2 = skin::hex(kOrange);
    return s;
  }();
  return k;
}

skin::WheelLook wheelLook(bool centred) {
  skin::WheelLook w;
  w.rail = skin::hex(kRail);
  w.fill = skin::hex(kFill);
  w.fillPressed = skin::hex(kOrange);
  w.cap = skin::hex(kCap);
  w.pointer = skin::hex(0xE4ECEE);
  w.centred = centred;
  return w;
}

float prm(const PluginState& s, int k) {
  if (k >= 0 && static_cast<size_t>(k) < s.params.size()) return s.params[static_cast<size_t>(k)];
  const PluginInfo* info = findPlugin("gl.turntable");
  return info && static_cast<size_t>(k) < info->params.size() ? static_cast<float>(info->params[static_cast<size_t>(k)].def) : 0.f;
}
void setP(App& a, const PluginTarget& t, int k, double v, bool merge) { a.doc.setParam(t.pid(k), v, merge); }

// Font size for a reference text height of `px` (readable at 100 %).
float fontPx(const skin::Form& F, float px) { return std::max(9.0f, std::round(px * F.s)); }

void text(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const char* s, ImU32 col, float px, int align = 0) {
  skin::textAt(dl, F.p(x, y), F.p(x + w, y + h), col, s, fontPx(F, px), align);
}

// Loads a file onto the record (the load button, a drop): the file plays, the
// "empty deck" mark of an FL import without a sample goes.
void loadSample(App& a, const PluginTarget& t, const std::string& p) {
  const PluginState* now = t.state(a.doc.song());
  if (!now || p.empty()) return;
  PluginState ns = *now;
  ns.data = p;
  if (ns.params.size() < static_cast<size_t>(TtCount)) normalizePluginState(ns);
  if (ns.params.size() > static_cast<size_t>(TtEmpty)) ns.params[TtEmpty] = 0.f;
  setPluginState(a, t, ns, "load sample");
}

// One hand gesture on the platter or the scope (one at a time).
struct Hand {
  int where = 0;  // 0 none, 1 platter, 2 scope
  double pos = 0;
  int turns = 0;
};
Hand& hand() {
  static Hand h;
  return h;
}

void grab(App& a, const PluginTarget& t, int where, double livePos) {
  Hand& h = hand();
  h.where = where;
  h.pos = std::isfinite(livePos) ? std::clamp(livePos, 0.0, 1.0) : 0.0;
  h.turns = 0;
  setP(a, t, TtPosition, h.pos, true);
  setP(a, t, TtTurns, 0, true);
  setP(a, t, TtHold, 1, true);
}
// Moves the hand by `px` reference pixels of mouse travel (positive = forward).
void move(App& a, const PluginTarget& t, const PluginState& st, double px) {
  Hand& h = hand();
  if (px == 0 || !std::isfinite(px)) return;
  const double fine = ImGui::GetIO().KeyCtrl ? 0.25 : 1.0;
  const double sens = std::clamp(static_cast<double>(prm(st, TtSense)), 0.0, 1.0);
  h.pos += px * kPerPx * sens * fine;
  while (h.pos > 1.0 && h.turns < 9) h.pos -= 1.0, ++h.turns;
  while (h.pos < 0.0 && h.turns > -9) h.pos += 1.0, --h.turns;
  h.pos = std::clamp(h.pos, 0.0, 1.0);
  setP(a, t, TtPosition, h.pos, true);
  setP(a, t, TtTurns, h.turns, true);
}
void release(App& a, const PluginTarget& t) {
  setP(a, t, TtHold, 0, true);
  hand().where = 0;
  a.doc.endGesture();
}

// What the name field shows.
std::string sampleName(const PluginState& st, bool missing) {
  if (st.data.empty()) return prm(st, TtEmpty) >= 0.5f ? i18n::tr("Empty deck: load or drop a sample") : i18n::tr("Built-in sound");
  std::string n = std::filesystem::u8path(st.data).filename().u8string();
  if (missing) n = std::string(i18n::tr("Missing: ")) + n;
  return n;
}

// Text clipped from the right to fit `w` screen px (an ellipsis marks the cut).
std::string fitText(const std::string& s, float w, float size) {
  ImGui::PushFont(nullptr, size);
  std::string out = s;
  if (ImGui::CalcTextSize(out.c_str()).x > w) {
    while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > w) out.pop_back();
    out += "...";
  }
  ImGui::PopFont();
  return out;
}

void drawPlatter(ImDrawList* dl, const skin::Form& F, float pos, bool held, bool playing) {
  const ImU32 orange = skin::hex(kOrange);
  const ImVec2 c = F.p(kPlX + kPlD * 0.5f, kPlY + kPlD * 0.5f);
  const float R = F.u(kPlD * 0.5f);
  dl->AddCircleFilled(ImVec2(c.x, c.y + F.u(1)), R, skin::darken(skin::hex(kPanel), 0.45f), 128);
  dl->AddCircleFilled(c, R, skin::hex(0x4A5357), 128);  // rim
  const float Rv = R - F.u(3);
  dl->AddCircleFilled(c, Rv, skin::hex(0x141718), 128);  // vinyl
  for (float r = Rv * 0.42f; r < Rv - F.u(3); r += F.u(2.5f)) dl->AddCircle(c, r, IM_COL32(255, 255, 255, 12), 128, 1.0f);
  const float ang = pos * kTau - kTau * 0.25f;  // one turn = the whole sample, the start at the top
  // Strobe dots on the rim turn with the record.
  for (int i = 0; i < 40; ++i) {
    const float a = ang + kTau * static_cast<float>(i) / 40.0f;
    const ImVec2 d(c.x + std::cos(a) * (R - F.u(1.5f)), c.y + std::sin(a) * (R - F.u(1.5f)));
    dl->AddCircleFilled(d, std::max(1.0f, F.u(0.6f)), i == 0 ? orange : IM_COL32(220, 226, 228, 80), 8);
  }
  // Label with the marker line (the platter angle at a glance).
  const float rl = Rv * 0.34f;
  dl->AddCircleFilled(c, rl, playing ? orange : skin::darken(orange, 0.5f), 64);
  dl->AddCircle(c, rl * 0.70f, IM_COL32(20, 23, 24, 90), 64, 1.0f);
  const ImVec2 m0(c.x + std::cos(ang) * rl * 0.2f, c.y + std::sin(ang) * rl * 0.2f);
  const ImVec2 m1(c.x + std::cos(ang) * (Rv - F.u(3)), c.y + std::sin(ang) * (Rv - F.u(3)));
  dl->AddLine(m0, m1, IM_COL32(236, 239, 241, 220), std::max(2.0f, F.u(1.2f)));
  dl->AddCircleFilled(c, std::max(2.0f, F.u(2.2f)), skin::hex(0xD8DCDE), 16);  // spindle
  if (held) dl->AddCircle(c, R + F.u(1), orange, 128, std::max(1.5f, F.u(1)));
}

// A box with an LED strip and a caption, bound to a toggle.
void ledBox(App& a, const PluginTarget& t, const PluginState& st, const skin::Form& F, int k, float x, float y, float w, float h,
            const char* cap, const char* tip) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool on = prm(st, k) >= 0.5f;
  ImGui::SetCursorScreenPos(F.p(x, y));
  ImGui::PushID(k);
  const bool click = ImGui::InvisibleButton("##led", F.sz(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImGui::PopID();
  if (click) setP(a, t, k, on ? 0 : 1, false);
  if (hov) a.setHint(tip);
  skin::insetField(dl, F.p(x, y), F.p(x + w, y + h), hov ? skin::hex(0x2A3134) : skin::hex(0x262C2F), F.u(1.5f));
  const ImU32 led = on ? skin::hex(kLed) : skin::hex(0x4B5357);
  dl->AddRectFilled(F.p(x + 4, y + 3), F.p(x + w - 4, y + 5.5f), led, F.u(1));
  if (on) dl->AddRect(F.p(x + 3.5f, y + 2.5f), F.p(x + w - 3.5f, y + 6), theme::withAlpha(skin::hex(kLed), 0.35f), F.u(1), 0, 1.0f);
  text(dl, F, x, y + 7, w, h - 8, cap, on ? skin::hex(0xE4ECEE) : skin::hex(kCaption), 5.5f);
}

void glyphTransport(ImDrawList* dl, int i, ImVec2 c, float r, ImU32 col) {
  if (i == 0) {  // reverse
    dl->AddTriangleFilled(ImVec2(c.x - r, c.y), ImVec2(c.x + r * 0.8f, c.y - r), ImVec2(c.x + r * 0.8f, c.y + r), col);
  } else if (i == 1) {  // pause
    dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.25f, c.y + r), col);
    dl->AddRectFilled(ImVec2(c.x + r * 0.25f, c.y - r), ImVec2(c.x + r * 0.8f, c.y + r), col);
  } else {  // forward
    dl->AddTriangleFilled(ImVec2(c.x + r, c.y), ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.8f, c.y + r), col);
  }
}

void transportSelector(App& a, const PluginTarget& t, const PluginState& st, const skin::Form& F) {
  constexpr float x = 46, y = 171, w = 156, h = 25;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, F.p(x, y), F.p(x + w, y + h), skin::hex(0x262C2F), F.u(2));
  const int cur = std::clamp(static_cast<int>(std::lround(prm(st, TtTransport))), 0, 2);
  static const char* tips[3] = {i18n::N_("Motor: reverse"), i18n::N_("Motor: pause (the platter coasts to a stop)"), i18n::N_("Motor: forward")};
  for (int i = 0; i < 3; ++i) {
    const float cx0 = x + w * static_cast<float>(i) / 3.0f, cx1 = x + w * static_cast<float>(i + 1) / 3.0f;
    ImGui::SetCursorScreenPos(F.p(cx0, y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##tr", F.sz(cx1 - cx0, h)) && i != cur) setP(a, t, TtTransport, i, false);
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) a.setHint(i18n::tr(tips[i]));
    if (i == cur) dl->AddRectFilled(F.p(cx0 + 2, y + 2), F.p(cx1 - 2, y + h - 2), skin::hex(0x3A4246), F.u(1.5f));
    if (i > 0) dl->AddLine(F.p(cx0, y + 4), F.p(cx0, y + h - 4), skin::hex(0x1C2022), 1.0f);
    const ImU32 col = i == cur ? skin::hex(kOrange) : hov ? skin::hex(0xC9CFD2) : skin::hex(0x7D878B);
    glyphTransport(dl, i, F.p((cx0 + cx1) * 0.5f, y + h * 0.5f), F.u(5), col);
  }
}

void drawScope(ImDrawList* dl, const skin::Form& F, float sh, const Sample* smp, const std::string& msg, float pos, bool showCue, float cue) {
  const ImVec2 w0 = F.p(kScX, kScY), w1 = F.p(kScX + kScW, kScY + sh);
  skin::insetField(dl, w0, w1, skin::hex(kWell), F.u(2));
  const ImVec2 q0(w0.x + F.u(2), w0.y + F.u(3)), q1(w1.x - F.u(2), w1.y - F.u(3));
  if (!smp || smp->frames() < 2) {
    skin::textAt(dl, w0, w1, skin::hex(kCaption), msg.c_str(), fontPx(F, 6.0f));
    return;
  }
  for (int i = 1; i < 8; ++i) {
    const float x = q0.x + (q1.x - q0.x) * static_cast<float>(i) / 8.0f;
    dl->AddLine(ImVec2(x, q0.y), ImVec2(x, q1.y), skin::hex(0x2D3437), 1.0f);
  }
  dl->AddLine(ImVec2(q0.x, (q0.y + q1.y) * 0.5f), ImVec2(q1.x, (q0.y + q1.y) * 0.5f), skin::hex(0x2D3437), 1.0f);
  waveform(dl, smp, q0, q1, theme::withAlpha(skin::hex(kFill), 0.55f));
  if (showCue) {
    const float x = q0.x + (q1.x - q0.x) * std::clamp(cue, 0.0f, 1.0f);
    dl->AddLine(ImVec2(x, q0.y), ImVec2(x, q1.y), theme::withAlpha(skin::hex(kLed), 0.8f), 1.0f);
    dl->AddTriangleFilled(ImVec2(x - F.u(2.5f), q0.y), ImVec2(x + F.u(2.5f), q0.y), ImVec2(x, q0.y + F.u(3)), skin::hex(kLed));
  }
  const float x = std::round(q0.x + (q1.x - q0.x) * std::clamp(pos, 0.0f, 1.0f));
  dl->AddLine(ImVec2(x, w0.y + 1), ImVec2(x, w1.y - 1), skin::hex(kOrange), std::max(1.5f, F.u(0.8f)));
}

// The options page (the reference's overlay over the controls): the timecode
// input settings (kept for FL projects) and Gridline's note settings.
void optionsPage(App& a, const PluginTarget& t, const PluginState& st, const skin::Form& F, const skin::Skin& K) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(F.p(0, 0), F.p(kW, 204), skin::hex(kOverlay), F.u(2), ImDrawFlags_RoundCornersTop);
  const ImU32 cap = skin::hex(kCaption), bright = skin::hex(0xE4ECEE);
  text(dl, F, 4, 7, 150, 12, i18n::tr("TIMECODE INPUT"), cap, 5.5f, -1);
  flskin::menu(a, t, TtTimecode, F, 3, 22, 155, 20, K);
  if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Timecode record format (kept with the project; the deck plays its own sample)"));
  ledBox(a, t, st, F, TtRiaa, 192, 22, 50, 20, i18n::tr("PHONO"), i18n::tr("Phono preamp for the timecode input (kept with the project)"));

  skin::insetField(dl, F.p(3, 50), F.p(244, 201), skin::hex(0x353B3E), F.u(2));
  text(dl, F, 10, 54, 120, 12, i18n::tr("NOTES"), cap, 5.5f, -1);
  {
    static const char* labels[3] = {i18n::N_("Off"), i18n::N_("Motor"), i18n::N_("Cue + motor")};
    static const char* tips[3] = {i18n::N_("Notes do nothing: the motor selector runs the platter"),
                                  i18n::N_("A held key runs the motor forward"),
                                  i18n::N_("Each note starts the record at the cue point at full speed")};
    constexpr float x = 10, y = 68, w = 227, h = 20;
    skin::insetField(dl, F.p(x, y), F.p(x + w, y + h), skin::hex(0x262C2F), F.u(2));
    const int cur = std::clamp(static_cast<int>(std::lround(prm(st, TtNotes))), 0, 2);
    for (int i = 0; i < 3; ++i) {
      const float c0 = x + w * static_cast<float>(i) / 3.0f, c1 = x + w * static_cast<float>(i + 1) / 3.0f;
      ImGui::SetCursorScreenPos(F.p(c0, y));
      ImGui::PushID(100 + i);
      if (ImGui::InvisibleButton("##nm", F.sz(c1 - c0, h)) && i != cur) setP(a, t, TtNotes, i, false);
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      if (hov) a.setHint(i18n::tr(tips[i]));
      if (i == cur) dl->AddRectFilled(F.p(c0 + 2, y + 2), F.p(c1 - 2, y + h - 2), skin::hex(0x4A5357), F.u(1.5f));
      text(dl, F, c0, y, c1 - c0, h, i18n::tr(labels[i]), i == cur ? skin::hex(kOrange) : hov ? bright : cap, 5.8f);
    }
  }
  ledBox(a, t, st, F, TtKeyTrack, 10, 100, 64, 22, i18n::tr("KEY TRACK"), i18n::tr("The key sets the motor speed (C5 plays at Speed)"));
  skin::wheel(a, t, TtCue, F, 92, 98, 26, 26, wheelLook(false));
  if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Cue point: where Cue + motor notes start (Shift+click the scope to set it)"));
  text(dl, F, 82, 125, 46, 10, i18n::tr("CUE"), cap, 5.5f);
  text(dl, F, 10, 146, 227, 12, i18n::tr("Timecode settings are kept for FL projects."), cap, 5.5f, -1);
  text(dl, F, 10, 160, 227, 12, i18n::tr("The deck plays its sample; the timecode input is not read."), cap, 5.5f, -1);
}

struct EdState {
  bool options = false;
};
EdState& edState(const PluginTarget& t) {
  static std::vector<std::pair<std::pair<ObjectId, int>, EdState>> v;
  for (auto& e : v)
    if (e.first.first == t.owner && e.first.second == t.slot) return e.second;
  v.push_back({{t.owner, t.slot}, EdState{}});
  // dev hook (screenshots): GRIDLINE_TURNTABLE_OPTIONS=1 opens the options page
  if (const char* o = std::getenv("GRIDLINE_TURNTABLE_OPTIONS"); o && *o == '1') v.back().second.options = true;
  return v.back().second;
}

// The whole editor takes dragged audio files (it is smaller than the plugin
// window's own drop zone, so it wins over it).
void dropTarget(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1) {
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!pl) return;
  std::string why;
  const std::string path = browserui::draggedAudioFile(a, pl, &why);
  if (path.empty() && why.empty()) return;
  if (!ImGui::BeginDragDropTargetCustom(ImRect(p0, p1), ImGui::GetID("##ttdrop"))) return;
  if (path.empty()) {
    ImGui::SetTooltip("%s", why.c_str());
    a.setHint(why);
    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
  } else if (const ImGuiPayload* x =
                 ImGui::AcceptDragDropPayload(pl->DataType, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery)) {
    if (x->Delivery) {
      xdragui::markConsumed();
      loadSample(a, t, path);
    } else {
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      fg->AddRectFilled(p0, p1, theme::withAlpha(skin::hex(kOrange), 0.10f), 4);
      fg->AddRect(p0, p1, skin::hex(kOrange), 4, 0, 2.5f);
      char tip[512];
      std::snprintf(tip, sizeof tip, i18n::tr("Load %s onto the record"), std::filesystem::u8path(path).filename().u8string().c_str());
      ImGui::SetTooltip("%s", tip);
      a.setHint(tip);
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
  }
  ImGui::EndDragDropTarget();
}

}  // namespace

void editorTurntable(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = deckSkin();
  const bool zoom = prm(st, TtZoom) >= 0.5f;
  const float grow = zoom ? kZoomGrow : 0.0f;
  const float formH = kH + grow, scopeH = kScH + grow;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, formH), K, 4);
  EdState& es = edState(t);

  // Live platter (this channel's runtime; the parameters when none runs).
  const TurntableTelemetry* tel = nullptr;
  std::shared_ptr<ChannelRuntime> rt;
  if (t.isChannel()) {
    rt = a.builder.channelRuntime(t.owner);
    if (rt && rt->gen) tel = turntableTelemetry(rt->gen.get());
  }
  const SamplePtr smp = turntableSample(st);
  const bool missing = !st.data.empty() && !smp;
  float pos = tel ? tel->pos.load(std::memory_order_relaxed) : prm(st, TtPosition);
  if (!std::isfinite(pos)) pos = 0.0f;
  const bool held = tel ? tel->held.load(std::memory_order_relaxed) : prm(st, TtHold) >= 0.5f;
  const float speed = tel ? tel->speed.load(std::memory_order_relaxed) : 0.0f;
  const ImU32 cap = skin::hex(kCaption);

  if (es.options) {
    optionsPage(a, t, st, F, K);
  } else {
    // ---- left: hand spring 2 x 2, pan, volume, hold, mute
    static const char* smoothTips[4] = {i18n::N_("Smooth 1: how long the hand takes to settle"), i18n::N_("Smooth 2: momentum of the hand"),
                                        i18n::N_("Smooth 3: stiffness of the hand"), i18n::N_("Smooth 4: carry (momentum share of each step)")};
    const float sx[4] = {6, 27, 6, 27}, sy[4] = {9, 9, 30, 30};
    for (int i = 0; i < 4; ++i) {
      skin::wheel(a, t, TtSmooth1 + i, F, sx[i], sy[i], 20, 20, wheelLook(false));
      if (ImGui::IsItemHovered()) a.setHint(i18n::tr(smoothTips[i]));
    }
    text(dl, F, 2, 51, 49, 10, i18n::tr("SMOOTH"), cap, 5.5f);
    skin::wheel(a, t, TtPan, F, 7, 67, 28, 28, wheelLook(true));
    text(dl, F, 1, 96, 40, 9, i18n::tr("PAN"), cap, 5.5f);
    skin::wheel(a, t, TtVolume, F, 7, 106, 28, 28, wheelLook(false));
    text(dl, F, 1, 136, 40, 13, i18n::tr("VOLUME"), cap, 5.5f);
    ledBox(a, t, st, F, TtHold, 8, 151, 28, 20, i18n::tr("HOLD"), i18n::tr("The hand holds the record: the platter follows Position"));
    ledBox(a, t, st, F, TtMute, 8, 177, 28, 20, i18n::tr("MUTE"), i18n::tr("Cut the output (a crossfader cut)"));

    // ---- right: turns, speed, acceleration, sensitivity
    text(dl, F, 203, 9, 36, 10, i18n::tr("TURNS"), cap, 5.5f);
    {
      char b[16];
      std::snprintf(b, sizeof b, "%+d", static_cast<int>(std::lround(std::clamp(prm(st, TtTurns), -9.0f, 9.0f))));
      skin::DigiLook dlook;
      dlook.bg = skin::hex(kWell);
      dlook.text = skin::hex(kOrange);
      skin::digi(a, t, TtTurns, F, 207, 21, 28, 16, dlook, b);
      if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Whole turns added to Position while the hand holds the record"));
    }
    skin::wheel(a, t, TtSpeed, F, 211, 84, 28, 28, wheelLook(false));
    if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Motor speed (100 % = the sample's own speed)"));
    text(dl, F, 203, 113, 44, 9, i18n::tr("SPEED"), cap, 5.5f);
    skin::wheel(a, t, TtAccel, F, 211, 123, 28, 28, wheelLook(false));
    if (ImGui::IsItemHovered()) a.setHint(i18n::tr("How quickly the platter reaches the motor speed"));
    text(dl, F, 203, 152, 44, 9, i18n::tr("ACCEL"), cap, 5.5f);
    skin::wheel(a, t, TtSense, F, 211, 162, 28, 28, wheelLook(false));
    if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Mouse travel when scratching the platter or the scope"));
    text(dl, F, 203, 191, 44, 12, i18n::tr("SENSE"), cap, 5.5f);

    // ---- platter: grab inside the record, drag up / down
    drawPlatter(dl, F, pos, held, smp != nullptr && std::fabs(speed) > 1e-4f);
    {
      const ImVec2 c = F.p(kPlX + kPlD * 0.5f, kPlY + kPlD * 0.5f);
      const float R = F.u(kPlD * 0.5f);
      ImGui::SetCursorScreenPos(F.p(kPlX, kPlY));
      ImGui::InvisibleButton("##platter", F.sz(kPlD, kPlD));
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float dx = m.x - c.x, dy = m.y - c.y;
      const bool inside = dx * dx + dy * dy <= R * R;
      Hand& h = hand();
      if (ImGui::IsItemActivated() && inside) grab(a, t, 1, pos);
      if (ImGui::IsItemActive() && h.where == 1) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        move(a, t, st, static_cast<double>(ImGui::GetIO().MouseDelta.y / F.s));
        a.setHint(i18n::tr("Scratching: down plays forward, up pulls the record back (Ctrl: fine)"));
      }
      if (ImGui::IsItemDeactivated() && h.where == 1) release(a, t);
      if (ImGui::IsItemHovered() && !ImGui::IsItemActive() && inside) a.setHint(i18n::tr("Grab the record and move it up or down to scratch"));
    }
    transportSelector(a, t, st, F);
  }

  // ---- bottom row: load, sample name, options, zoom
  if (skin::button(F, "##ttload", 2, 205, 34, 20, "", skin::hex(kButton), skin::hex(0xE4ECEE), i18n::tr("Load a sample (or drop one on the deck)"))) {
    const PluginTarget tt = t;
    openSampleDialog(a, i18n::tr("Load sample"), [&a, tt](const std::string& p) { loadSample(a, tt, p); });
  }
  ui::drawIconFitC(dl, ui::Icon::Folder, F.p(19, 215), F.sz(12, 9), skin::hex(0xE4ECEE));
  {
    const ImVec2 p0 = F.p(39, 207), p1 = F.p(206, 223);
    dl->AddRectFilled(p0, p1, skin::hex(kPathBg), F.u(1.5f));
    const float fs = fontPx(F, 5.8f);
    const std::string name = fitText(sampleName(st, missing), p1.x - p0.x - F.u(6), fs);
    skin::textAt(dl, ImVec2(p0.x + F.u(2), p0.y), p1, missing ? skin::hex(kLed) : skin::hex(kPathText), name.c_str(), fs, -1);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##ttname", ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (ImGui::IsItemHovered()) a.setHint(st.data.empty() ? sampleName(st, false) : st.data);
  }
  if (skin::menuButton(F, "##ttopt", 207, 205, 19, 20, es.options ? skin::hex(kOrange) : skin::hex(0xC9CFD2))) es.options = !es.options;
  {
    bool z = zoom;
    ImGui::SetCursorScreenPos(F.p(226, 205));
    if (ImGui::InvisibleButton("##ttzoom", F.sz(19, 20))) setP(a, t, TtZoom, z ? 0 : 1, false);
    const bool hov = ImGui::IsItemHovered();
    if (hov) a.setHint(i18n::tr("Large scope"));
    ui::drawIconFitC(dl, ui::Icon::Zoom, F.p(235.5f, 215), F.sz(10, 10), z ? skin::hex(kOrange) : hov ? skin::hex(0xFFFFFF) : skin::hex(0xC9CFD2));
  }

  // ---- scope: play position, cue, drag sideways to scratch
  {
    const bool showCue = std::lround(prm(st, TtNotes)) == TnCue;
    const std::string msg = missing ? std::string(i18n::tr("Sample not found: load it again")) : sampleName(st, false);
    drawScope(dl, F, scopeH, smp.get(), msg, pos, showCue, prm(st, TtCue));
    const ImVec2 w0 = F.p(kScX, kScY), w1 = F.p(kScX + kScW, kScY + scopeH);
    ImGui::SetCursorScreenPos(w0);
    ImGui::InvisibleButton("##ttscope", ImVec2(w1.x - w0.x, w1.y - w0.y));
    Hand& h = hand();
    if (ImGui::IsItemActivated()) {
      if (ImGui::GetIO().KeyShift) {
        const float q0 = w0.x + F.u(2), q1 = w1.x - F.u(2);
        setP(a, t, TtCue, std::clamp((ImGui::GetIO().MousePos.x - q0) / std::max(1.0f, q1 - q0), 0.0f, 1.0f), false);
      } else if (smp) {
        grab(a, t, 2, pos);
      }
    }
    if (ImGui::IsItemActive() && h.where == 2) {
      move(a, t, st, static_cast<double>(ImGui::GetIO().MouseDelta.x / F.s));
      a.setHint(i18n::tr("Scratching: drag right plays forward, left pulls back (Ctrl: fine)"));
    }
    if (ImGui::IsItemDeactivated() && h.where == 2) release(a, t);
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) a.setHint(i18n::tr("Drag sideways to scratch; Shift+click sets the cue point"));
  }
  dropTarget(a, t, F.o, F.p(kW, formH));
  F.end(kW, formH);
}

}  // namespace gl::packs::spectral

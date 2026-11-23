// Gridline — top bar and bottom hint/status bar.
//
// The top bar follows FL Studio's main-window grouping (menus · transport ·
// tempo/position · pattern · master · monitor · window buttons), rendered as
// Gridline panels. It is responsive: every group knows its width up front, and
// when the window is too narrow the least important groups move into the "⋯"
// overflow popup instead of overlapping or being clipped.
#include "RecordCard.h"
#include "AutomationEditor.h"
#include "../core/MenuParity.h"
#include "../core/Timeline.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "CollabUi.h"
#include "ContextMenus.h"
#include "../core/I18n.h"
#include "ToastText.h"
#include "McpUi.h"
#include "Keys.h"
#include "MenuParityUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Arrangements.h"
#include "../core/Automation.h"
#include "../core/LevelMeter.h"
#include "../core/Motion.h"
#include "SmoothScrollUi.h"
#include "App.h"
#include "LayoutParts.h"
#include "LayoutPresets.h"
#include "LiveLook.h"
#include "LayoutShellUi.h"
#include "LayoutStyleUi.h"
#include "WindowRaise.h"
#include "../core/LayoutShell.h"
#include "../core/LayoutStyle.h"
#include "Theme.h"
#include "Viewports.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "MeterUi.h"
#include "MixerMeters.h"
#include "../core/MeterEdit.h"
#include "../core/Meter.h"
#include "../core/ChordTools.h"
#include "../core/NoteTools.h"
#include "Windows.h"
#include "../core/MenuOps.h"
#include "SystemMenu.h"
#include "UndoUi.h"
#include "ToolbarUi.h"
#include "../core/Toolbar.h"

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

namespace {

// ---- geometry (design tokens) ----
constexpr float kPad = theme::kS3;          // outer padding left/right
constexpr float kLcdMid = 13;  // cp53: Smooth's display: the values' centre line, from its top (captions under it)
constexpr float kPanelH = 36.0f;            // panel height (6 px margin top/bottom)
constexpr float kPanelPad = theme::kS1;     // inner padding of a panel
constexpr float kGap = theme::kS2;          // gap between panels
constexpr float kBtn = theme::kIconBtn;     // 28 px icon buttons
constexpr float kInner = 2.0f;              // gap between buttons inside a panel
constexpr float kMenuPadX = 9.0f;
constexpr float kMenuPadXCompact = 6.0f;

// K-297: a menu opens under its title, and one taller than the room below (Studio's View and
// Options, Clips' View at 960 x 600) scrolls instead of running off the window's bottom.
void menuAt(ImVec2 pos) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(160.0f, vp->WorkPos.y + vp->WorkSize.y - pos.y - 4.0f)));
}

// K-79: the menus come from the menu bar style of the layout preset (core/MenuStyle.h); the
// studio style is File, Edit, Add, Patterns, View, Options, Tools, Help. A style's unnamed menu
// is the program menu, opened from the mark.
std::vector<const menustyle::Menu*> barMenus(const App& a, const menustyle::Menu** program) {
  std::vector<const menustyle::Menu*> v;
  *program = nullptr;
  for (const menustyle::Menu& m : menustyle::menus(layoutui::menuStyle(a))) {
    if (m.name) v.push_back(&m);
    else *program = &m;
  }
  return v;
}


void noteRectAt(const char* name, ImVec2 a, ImVec2 b);  // dev: an "sdlat" target (below)
void noteRect(const char* name);                        // the last item's rectangle

// Tempo LCD: drag / wheel / double-click to type. Returns true when changed.
// K-112b: `big` (the Tracks layout's display): the whole BPM large and bold, the decimals small
// after it (only when there are any), `height` tall; the same drag, wheel, menu and typing.
struct BigTempo {
  float x = 0, wholeW = 0, fracW = 0, size = 0;
  std::string whole, frac;
};
BigTempo bigTempoLayout(App& a, double v, ImVec2 p, float width) {
  BigTempo b;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  std::string s = buf;
  const size_t dot = s.find('.');
  b.whole = s.substr(0, dot);
  std::string f = s.substr(dot + 1);
  while (!f.empty() && f.back() == '0') f.pop_back();
  b.frac = f.empty() ? std::string() : "." + f;
  b.size = theme::kFontTitle;  // about half the position digits, as the reference displays show it
  b.wholeW = a.fontBold->CalcTextSizeA(b.size, 1e9f, 0, b.whole.c_str()).x;
  b.fracW = b.frac.empty() ? 0.0f : a.fontBold->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, b.frac.c_str()).x;
  b.x = std::round(p.x + (width - b.wholeW - b.fracW) * 0.5f);
  return b;
}

// R05-16: a position as the reference's display counts it: bar, beat, division (a sixteenth) and
// tick, each from 1, through the song's meter changes.
struct Lbbdt {
  long long bar = 1;
  int beat = 1, div = 1, tick = 1;
};
Lbbdt lcdBbdt(const Song& s, double at) {
  const MeterMap mm(s);
  const MeterMap::Bbt b = mm.toBbt(static_cast<Tick>(std::max(0.0, at)));
  const Tick dv = std::max<Tick>(1, s.ppq / 4);
  Lbbdt q;
  q.bar = static_cast<long long>(b.bar) + 1, q.beat = b.beat + 1;
  q.div = static_cast<int>(b.tick / dv) + 1, q.tick = static_cast<int>(b.tick % dv) + 1;
  return q;
}
// The clock time at a position: h:mm:ss.cc (the tempo changes followed).
std::string lcdClock(const Song& s, double at) {
  TempoMap map;
  map.build(s);
  const long long cs = static_cast<long long>(std::floor(std::max(0.0, map.secondsAt(at)) * 100.0 + 1e-9));
  char b[40];
  std::snprintf(b, sizeof b, "%lld:%02lld:%02lld.%02lld", cs / 360000, (cs / 6000) % 60, (cs / 100) % 60, cs % 100);
  return b;
}
// A typed position: "bar beat div tick" (spaces, dots or commas between; the fields left out are
// 1), or a clock time with colons ("1:30", "0:01:30.5"). Nothing for text that is neither.
std::optional<Tick> parseLcdPosition(const Song& s, const char* text) {
  std::string t(text);
  if (t.find(':') != std::string::npos) {
    double sec = 0;
    size_t i = 0;
    while (i <= t.size()) {
      const size_t j = t.find(':', i);
      const std::string part = t.substr(i, j == std::string::npos ? std::string::npos : j - i);
      char* end = nullptr;
      const double v = std::strtod(part.c_str(), &end);
      if (part.empty() || end == part.c_str()) return std::nullopt;
      sec = sec * 60.0 + v;
      if (j == std::string::npos) break;
      i = j + 1;
    }
    TempoMap map;
    map.build(s);
    return static_cast<Tick>(std::llround(std::max(0.0, map.tickAtSeconds(sec))));
  }
  long long f[4] = {1, 1, 1, 1};
  int n = 0;
  const char* p = text;
  while (*p && n < 4) {
    while (*p == ' ' || *p == '.' || *p == ',' || *p == '\t') ++p;
    if (!*p) break;
    char* end = nullptr;
    const long long v = std::strtoll(p, &end, 10);
    if (end == p) return std::nullopt;
    f[n++] = std::max(1LL, v);
    p = end;
  }
  if (n == 0) return std::nullopt;
  const MeterMap mm(s);
  const Tick bar = mm.barStart(f[0] - 1);
  const Tick beat = mm.beatLenAt(bar), dv = std::max<Tick>(1, s.ppq / 4);
  return bar + static_cast<Tick>(f[1] - 1) * beat + static_cast<Tick>(f[2] - 1) * dv + static_cast<Tick>(f[3] - 1);
}

// R05-16: the Tracks layout's display modes, in the reference's order (ui.lcdMode keeps 0 and 1
// as they were: 0 beats and project, 1 custom).
void lcdModeItems(App& a) {
  const int cur = std::clamp(static_cast<int>(a.settings.getNumber("ui.lcdMode")), 0, 5);
  static const std::pair<int, const char*> modes[] = {{0, i18n::N_("Beats and project")}, {2, i18n::N_("Time and project")},
                                                      {3, i18n::N_("Beats and time")},    {4, i18n::N_("Beats")},
                                                      {5, i18n::N_("Time")},              {1, i18n::N_("Custom")}};
  for (const auto& [m, name] : modes)
    if (ImGui::MenuItem(i18n::tr(name), nullptr, cur == m)) a.settings.set("ui.lcdMode", m);
}

bool tempoLcd(App& a, double* v, float width, float lcdH = kBtn, bool big = false) {  // K-112c: lcdH (Live: a slimmer field); K-112b: big
  ImGui::PushID("tempo");
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = lcdH;
  ImGui::InvisibleButton("##md", ImVec2(width, h));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = false;
  ImGuiIO& io = ImGui::GetIO();
  char buf[32];
  // K-112c (Live): the tempo in the sans face with two decimals, as the refs' control bars show it.
  const bool live = livelook::active(a);
  const char* fmt = live ? "%.2f" : "%.3f";
  std::snprintf(buf, sizeof buf, fmt, *v);
  const float fsT = theme::kFontCaption + 1;
  auto liveDotX = [&](const char* b) {
    const float tw = a.fontRegular->CalcTextSizeA(fsT, 1e9f, 0, b).x;
    const float x0 = std::round(p.x + (width - tw) * 0.5f);
    return x0 + a.fontRegular->CalcTextSizeA(fsT, 1e9f, 0, b, b + std::strcspn(b, ".")).x;
  };
  // Two zones split at the decimal point: dragging the whole part steps whole BPM and keeps the
  // decimals (96.250 -> 97.250); dragging the decimals moves smoothly (0.25 BPM per pixel).
  const BigTempo bt0 = big ? bigTempoLayout(a, *v, p, width) : BigTempo{};
  const float dotX = big ? bt0.x + bt0.wholeW : live ? liveDotX(buf) : ui::LcdCharX(p, ImVec2(width, lcdH), buf, static_cast<int>(std::strcspn(buf, ".")));
  enum Zone { None, Whole, Fraction };
  static Zone zone = None;
  static double startBpm = 0, dragPx = 0;
  if (ImGui::IsItemActivated()) {
    zone = io.MouseClickedPos[0].x < dotX ? Whole : Fraction;
    startBpm = *v, dragPx = 0;
  }
  if (!act) zone = None;
  if (act && io.MouseDelta.y != 0) {
    if (zone == Whole) {
      constexpr double kPxPerBpm = 4.0, kFinePxPerBpm = 16.0;  // Shift: slower, still whole steps
      dragPx -= static_cast<double>(io.MouseDelta.y) / (io.KeyShift ? kFinePxPerBpm : kPxPerBpm);
      const double steps = std::trunc(dragPx);
      double to = startBpm + steps;
      if (to > 522.0) to -= std::ceil(to - 522.0);  // stay in range, keeping the decimals
      if (to < 10.0) to += std::ceil(10.0 - to);
      to = std::clamp(to, 10.0, 522.0);
      if (to != *v) *v = to, changed = true;
    } else {
      *v = std::clamp(*v - io.MouseDelta.y * 0.25 * (io.KeyShift ? 0.1 : 1.0), 10.0, 522.0);
      changed = true;
    }
  }
  if (hov && io.MouseWheel != 0) {  // over the whole number: whole BPM; over the decimals: 0.1 (Shift: 0.01)
    const double step = io.MousePos.x < dotX ? 1.0 : io.KeyShift ? 0.01 : 0.1;
    *v = std::clamp(std::round((*v + io.MouseWheel * step) * 1000.0) / 1000.0, 10.0, 522.0);
    changed = true;
  }
  if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  if (ImGui::BeginPopupContextItem("##tempomenu")) {  // tempo: controller link, tap tempo
    knobContextMenu(a, songParam(SongParam::Tempo));
    actionLinkMenuItems(a, RemoteAction::TapTempo);
    ImGui::EndPopup();
  }
  std::snprintf(buf, sizeof buf, fmt, *v);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (big) {
    const auto& P = pal();
    const BigTempo bt = bigTempoLayout(a, *v, p, width);
    // R01-26: the tempo over its mode, as 216 shows "137" over "KEEP" (the project keeps its tempo:
    // imported audio follows it), the TEMPO caption under both.
    // cp53 (polish): the tempo on the display's values line, its caption on the captions' row (the
    // "KEEP" line under it crowded the 36 px display, and the tempo's caption already names it).
    const float ty = std::round(p.y + kLcdMid - bt.size * 0.55f);
    dl->AddText(a.fontBold, bt.size, ImVec2(bt.x, ty), act ? P.hot : P.text, bt.whole.c_str());
    if (!bt.frac.empty())
      dl->AddText(a.fontBold, theme::kFontCaption, ImVec2(bt.x + bt.wholeW + 1, std::round(ty + bt.size - theme::kFontCaption)), P.textDim, bt.frac.c_str());
  } else if (live) {
    const auto& P = pal();
    const ImVec2 q1(p.x + width, p.y + lcdH);
    dl->AddRectFilled(p, q1, hov || act ? theme::mix(P.field, P.surface800, 0.5f) : (P.dark ? P.field : P.surface950), theme::kRadiusSm);
    dl->AddRect(p, q1, theme::mix(P.dark ? P.field : P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), theme::kRadiusSm);
    theme::centreText(dl, a.fontRegular, fsT, p, q1, act ? P.hot : P.text, buf);  // K-112c (round 28): the text colour, lit while dragged (276, 280, 357)
  } else {
    ui::Lcd(dl, p, ImVec2(width, lcdH), buf, hov, act);
  }
  noteRectAt("tempoint", p, ImVec2(dotX, p.y + lcdH));
  noteRectAt("tempofrac", ImVec2(dotX, p.y), ImVec2(p.x + width, p.y + lcdH));
  const bool overWhole = act ? zone == Whole : io.MousePos.x < dotX;
  if ((hov || act) && !big) {  // which digits a drag moves: a short accent bar under them
    const auto& P = pal();
    const float x0 = overWhole ? ui::LcdCharX(p, ImVec2(width, lcdH), buf, 0) : dotX;
    const float x1 = overWhole ? dotX : ui::LcdCharX(p, ImVec2(width, lcdH), buf, static_cast<int>(std::strlen(buf)));
    const float y = std::round(p.y + lcdH - 4.0f);
    dl->AddLine(ImVec2(std::round(x0) + 1.0f, y), ImVec2(std::round(x1) - 1.0f, y), theme::withAlpha(P.accent, act ? 0.9f : 0.5f), 1.5f);
  }
  if (overWhole) ui::Hint("Tempo (BPM): drag or scroll the whole number to step whole BPM (the decimals stay), double-click to type");
  else ui::Hint("Tempo (BPM): drag or scroll the decimals to fine-tune (scroll: 0.1, Shift: 0.01), double-click to type");
  ui::Tooltip("Tempo (BPM)");
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ImGui::OpenPopup("##type");
  if (ImGui::BeginPopup("##type")) {
    static double typed;
    if (ImGui::IsWindowAppearing()) {
      typed = *v;
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputDouble("##t", &typed, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
      *v = std::clamp(typed, 10.0, 522.0);
      changed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", i18n::tr("BPM"));
    ImGui::EndPopup();
  }
  ImGui::PopID();
  (void)a;
  return changed;
}

// K-48: the tap tempo button. It acts when it goes down, the moment of the beat (as the transport
// buttons do); it stays lit while a run of taps is open and flashes with each tap.
void tapButton(App& a, float size) {
  const char* key = keys::label("transport.tapTempo");
  const std::string hint = std::string(i18n::tr("Tap tempo: click on every beat. The tempo follows from the second tap, and a pause of over 2 seconds starts over. Right-click to link a controller.")) +
                           (key ? std::string(" (") + key + ")" : std::string());
  const ImVec2 p = ImGui::GetCursorScreenPos();
  if (ui::IconButton("taptempo", ui::Icon::TapTempo, a.tapListening(), size, hint.c_str(), 0, 0, true)) a.tapTempo();
  noteRect("taptempo");
  if (ImGui::BeginPopupContextItem("##tapmenu")) {
    actionLinkMenuItems(a, RemoteAction::TapTempo);
    ImGui::EndPopup();
  }
  constexpr double kFlash = 0.16;
  const double age = a.tapAge();
  if (age < kFlash && scrollui::animations())
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size, p.y + size), theme::withAlpha(pal().hot, 0.32f * static_cast<float>(1.0 - age / kFlash)), theme::kRadius);
}

void panel(ImDrawList* dl, float x, float w, float top) {
  const auto& P = pal();
  theme::devNote("topbar.panel", P.surface900);  // K-177 dev
  dl->AddRectFilled(ImVec2(x, top), ImVec2(x + w, top + kPanelH), P.surface900, theme::kRadiusLg);
  if (P.dark) dl->AddLine(ImVec2(x + 3, top + 0.5f), ImVec2(x + w - 3, top + 0.5f), theme::withAlpha(P.paper, 0.07f));
  dl->AddRect(ImVec2(x, top), ImVec2(x + w, top + kPanelH), P.dark ? theme::withAlpha(P.ink, 0.5f) : P.line, theme::kRadiusLg);
}

// J-143: the OUT meter. The master bus peak per channel (left on top, right
// below) with the shared ballistics, on the mixer master meter's -60..+6 dB
// scale and zones (green to -6 dB, amber to 0 dB, red only past full scale),
// a hold marker per channel, a faint 0 dB line and a clip lamp at the end
// that lights only on real overs. Nothing is lit in silence.
void outMeter(ImDrawList* dl, ImVec2 p, ImVec2 size, const LevelMeter& m) {
  const auto& P = pal();
  const float lampW = 4.0f, lampGap = 2.0f;
  const float bw = std::floor(size.x - lampW - lampGap);
  const ImU32 well = P.dark ? P.surface950 : theme::mix(P.surface800, P.ink, 0.12f);
  dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + size.y), well, 2);
  const float inner = bw - 2;
  auto xAt = [&](float db) { return p.x + 1 + std::round(LevelMeter::toPos(db) * inner); };
  const float x0 = p.x + 1, xWarn = xAt(-6.0f), xZero = xAt(0.0f);
  // The headroom past 0 dBFS reads as a dim red zone, so the scale is legible at rest.
  dl->AddRectFilled(ImVec2(xZero, p.y + 1), ImVec2(p.x + bw - 1, p.y + size.y - 1), theme::withAlpha(P.meterHigh, 0.14f));
  const float rowH = std::floor((size.y - 3) * 0.5f);
  for (int ch = 0; ch < 2; ++ch) {
    const float y0 = p.y + 1 + static_cast<float>(ch) * (rowH + 1), y1 = y0 + rowH;
    const float lv = m.levelDb(ch);
    if (lv > LevelMeter::kFloorDb) {
      const float xe = xAt(lv);
      if (xe > x0) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::min(xe, xWarn), y1), P.meterLow);
      if (xe > xWarn) dl->AddRectFilled(ImVec2(xWarn, y0), ImVec2(std::min(xe, xZero), y1), P.meterMid);
      if (xe > xZero) dl->AddRectFilled(ImVec2(xZero, y0), ImVec2(xe, y1), P.meterHigh);
    }
    const float hd = m.holdDb(ch);
    if (hd > LevelMeter::kFloorDb) {
      const float hx = std::max(x0 + 1, xAt(hd));
      dl->AddRectFilled(ImVec2(hx - 1, y0), ImVec2(hx, y1), hd > 0.0f ? P.meterHigh : P.text);
    }
  }
  dl->AddLine(ImVec2(xZero + 0.5f, p.y + 1), ImVec2(xZero + 0.5f, p.y + size.y - 1), theme::withAlpha(P.textFaint, 0.7f), 1.0f);
  const ImVec2 l0(p.x + bw + lampGap, p.y), l1(p.x + size.x, p.y + size.y);
  dl->AddRectFilled(l0, l1, m.clipping() ? P.danger : well, 1.5f);
}

void patternList(App& a) {
  const Song& s = a.doc.song();
  for (const auto& p : s.patterns) {
    ImGui::PushID(static_cast<int>(p.id));
    ImVec2 q = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    if (ImGui::Selectable("##pat", p.id == a.curPattern, 0, ImVec2(std::max(160.0f, ImGui::GetContentRegionAvail().x), 0)))
      a.selectPattern(p.id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q.x + 6, q.y + h - 2), theme::rgb(p.color), 2);
    dl->AddText(ImVec2(q.x + 14, q.y), pal().text, p.name.c_str());
    ImGui::PopID();
  }
}

}  // namespace

// ------------------------------------------------------------ top bar ----

namespace {
struct DevRect {
  const char* name;
  ImVec2 a, b;
  int frame = -1;
};
DevRect gDevRects[] = {{"play", {}, {}}, {"stop", {}, {}}, {"rec", {}, {}}, {"metro", {}, {}}, {"brsearch", {}, {}}, {"brclear", {}, {}}, {"chroute", {}, {}}, {"chroutefree", {}, {}}, {"ceroutefree", {}, {}}, {"chroutelist", {}, {}},
                     // 0.7.8: the Audio clip window's mixer track box (ChannelRoute.h box), its first item and first tracks
                     {"ceroute", {}, {}}, {"ceroutelist", {}, {}}, {"ceroutetrack0", {}, {}}, {"ceroutetrack1", {}, {}}, {"ceroutetrack2", {}, {}}, {"ceroutetrack3", {}, {}}, {"ceview", {}, {}}, {"ceviewmain", {}, {}}, {"ceviewadvanced", {}, {}}, {"tempoint", {}, {}}, {"tempofrac", {}, {}}, {"loupedrag", {}, {}}, {"taptempo", {}, {}}, {"tapmenu", {}, {}}, {"tapmenutap", {}, {}}, {"menutools", {}, {}}, {"bar_transport", {}, {}}, {"bar_tempo", {}, {}}, {"bar_pos", {}, {}}, {"bar_pattern", {}, {}}, {"bar_master", {}, {}}, {"bar_monitor", {}, {}}, {"bar_windows", {}, {}}, {"bar_more", {}, {}}, {"prstamp", {}, {}}, {"prstamplbl", {}, {}}, {"prstampchip", {}, {}}, {"brcollapse", {}, {}}, {"capture", {}, {}}, {"sessrec", {}, {}}, {"drawmode", {}, {}}, {"devswap", {}, {}}, {"devsave", {}, {}}, {"brchip", {}, {}}, {"devfold", {}, {}}, {"devwindow", {}, {}}, {"ptabsrc", {}, {}}, {"ptabdst", {}, {}}, {"follow", {}, {}}, {"loop", {}, {}},
                     // K-337: the browser tab strip, its menus and the Clips / Smooth places
                     {"braddtab", {}, {}}, {"braddfolder", {}, {}}, {"brfilemenu", {}, {}}, {"brtabmrename", {}, {}}, {"brtabmclose", {}, {}}, {"brtabname", {}, {}},
                     {"brtab0", {}, {}}, {"brtab1", {}, {}}, {"brtab2", {}, {}}, {"brtab3", {}, {}}, {"brtab4", {}, {}}, {"brtab5", {}, {}},
                     {"brtab6", {}, {}}, {"brtab7", {}, {}}, {"brplace0", {}, {}}, {"brplace1", {}, {}}, {"brplace2", {}, {}},
                     {"brreopenall", {}, {}}, {"brreopenproject", {}, {}}, {"brreopenplugins", {}, {}}, {"brreopensounds", {}, {}},
                     {"brreopenstarred", {}, {}}, {"mixsplit", {}, {}},
                     // hidden browser folders and mixer strips
                     {"brhidefolder", {}, {}}, {"brshowfolder", {}, {}}, {"brshowhidden", {}, {}}, {"mixhidden", {}, {}}, {"mixshowall", {}, {}}, {"clipshiddenstrip", {}, {}},
                     {"trkmixhidden", {}, {}}, {"fxslotempty", {}, {}},
                     {"sb_mix", {}, {}}, {"sb_dev", {}, {}}, {"sb_clip", {}, {}},  // 0.7.8: the Clips status bar's band buttons
                     // 0.7.8: the Clips control bar's meter, metronome menu arrow and launch grid
                     {"clipssig", {}, {}}, {"clipsmetroarrow", {}, {}}, {"clipsquant", {}, {}},
                     // Smooth's control bar: the pane and panel buttons (sdlat lb_lists ...)
                     {"lb_lib", {}, {}}, {"lb_ins", {}, {}}, {"lb_ed", {}, {}}, {"lb_sm", {}, {}}, {"lb_mx", {}, {}},
                     {"lb_lists", {}, {}}, {"lb_notes", {}, {}}, {"lb_browser", {}, {}}};
float gMetroMirror = 1.0f;
bool gClipsMetroLeft = false;  // (dev: the Clips metronome's left circle is the filled one: an odd beat while playing)
bool gClipsMetroMenu = false;  // (dev: its menu is open)
double gMetroBeat = 0;  // (dev: the song position, in beats, the glyph's pose was drawn for)
bool gMetroTicking = false;  // (dev: ... whether it ticked then, and from which beat)
double gMetroAnchor = 0;
float gDevFitWidth = 0;  // K-132: dev, the width the bar fits its groups into (0 = the window's)
void noteRectAt(const char* name, ImVec2 a, ImVec2 b) {
  for (DevRect& r : gDevRects)
    if (std::strcmp(r.name, name) == 0) r.a = a, r.b = b, r.frame = ImGui::GetFrameCount();
}
void noteRect(const char* name) { noteRectAt(name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }  // the last item's rectangle
std::map<std::string, std::pair<bool, int>> gBarLit;  // (dev) Smooth's pane buttons: lit, frame drawn
}  // namespace

void devNoteRect(const char* name) { noteRect(name); }
std::string devBarLit() {
  std::string s;
  for (const auto& [id, v] : gBarLit)
    if (v.second >= ImGui::GetFrameCount() - 1) s += (s.empty() ? "" : " ") + id + "=" + (v.first ? "1" : "0");
  return s;
}
void devNoteRectAt(const char* name, float x0, float y0, float x1, float y1) { noteRectAt(name, ImVec2(x0, y0), ImVec2(x1, y1)); }
void devTopBarFitWidth(float w) { gDevFitWidth = std::max(0.0f, w); }
bool topBarButtonRect(const char* name, float& x0, float& y0, float& x1, float& y1) {
  for (const DevRect& r : gDevRects)
    if (std::strcmp(r.name, name) == 0 && r.frame >= ImGui::GetFrameCount() - 1) {
      x0 = r.a.x, y0 = r.a.y, x1 = r.b.x, y1 = r.b.y;
      return true;
    }
  return false;
}
float topBarMetronomeMirror() { return gMetroMirror; }
double topBarMetronomeBeat() { return gMetroBeat; }
void clipsMetronomeState(bool* leftLit, bool* menuOpen) {
  if (leftLit) *leftLit = gClipsMetroLeft;
  if (menuOpen) *menuOpen = gClipsMetroMenu;
}
bool topBarMetronomeTicking(double* anchorBeat) {
  if (anchorBeat) *anchorBeat = gMetroAnchor;
  return gMetroTicking;
}

static float drawTopBarLogic(App& a);
static float drawTopBarLive(App& a);
static float drawLogicControlBar(App& a);
static float drawTrackerMenuRow(App& a);

// K-256: a pinned command's button on a bar: its glyph (or its name) on a flat face; a click runs it, a
// right-click opens its menu (name or icon, where it shows, order, unpin).
static bool pinButton(App& a, ImDrawList* dl, const toolbar::Pin& pn, ImVec2 p, ImVec2 size) {
  const auto& P = pal();
  const std::string label = toolbarui::pinLabel(pn);
  ImGui::SetCursorScreenPos(p);
  const bool click = ImGui::InvisibleButton("##pin", size);
  const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  if (held || hov) dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), held ? P.surface700 : P.surface800, theme::kRadius);
  if (pn.icon) {
    ui::drawIcon(dl, toolbarui::pinIcon(pn), ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f), 16, hov ? P.text : P.textDim);
  } else {
    const float fs = ImGui::GetFontSize();
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(p.x + 9, std::round(p.y + (size.y - fs) * 0.5f)), size.x - 18, hov ? P.text : P.textDim, label.c_str());
  }
  if (hov) ImGui::SetTooltip("%s", label.c_str());
  if (click) toolbarui::runPin(a, pn);
  if (ImGui::BeginPopupContextItem("##pinmenu")) {
    toolbarui::pinMenu(a, pn);
    ImGui::EndPopup();
  }
  return click;
}

// K-256: a right-click on an empty bit of a bar (between `y0` and `y1`, no control under the pointer) opens
// the bar list, in every layout; the list draws inside the bar's window.
static void barListOnEmpty(App& a, float y0, float y1) {
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const bool onBar = mp.y >= y0 && mp.y < y1 && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
  if ((onBar && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) || devMenuTake("Customise")) toolbarui::openBarList(a);
  toolbarui::drawBarList(a);
}

float drawTopBar(App& a) {
  if (layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic)) return drawLogicControlBar(a);  // K-112b
  if (layoutui::active() == layout::Preset::Logic) return drawTopBarLogic(a);  // K-79: two rows
  if (livelook::active(a)) return drawTopBarLive(a);                          // K-112c: menu row and one flat control row
  if (layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker)) return drawTrackerMenuRow(a);  // K-106
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = theme::kTopBarH;
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  theme::devNote("topbar", P.chrome);  // K-177 dev: the top bar background
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.chrome));
  ImGui::Begin("##topbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);  // popups opened from the bar use the normal style
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Song& s = a.doc.song();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();
  const float panelTop = wp.y + std::floor((h - kPanelH) * 0.5f);
  const float btnTop = panelTop + (kPanelH - kBtn) * 0.5f;
  dl->AddLine(ImVec2(wp.x, wp.y + h - 1), ImVec2(wp.x + W, wp.y + h - 1), P.line);

  // ---- group widths (full, or compact when the full bar does not fit) ----
  const menustyle::Menu* program = nullptr;
  const std::vector<const menustyle::Menu*> kMenus = barMenus(a, &program);
  const size_t kMenuCount = kMenus.size();
  std::vector<float> menuW(kMenuCount, 0.0f);
  float menusTotal = 0;
  float segW = 50.0f, tempoW = 84.0f, posW = 118.0f, patW = 150.0f;
  float wTransport = 0, wTempo = 0, wPos = 0, wPattern = 0;
  const float wMaster = kPanelPad * 2 + 26 + kInner + 26 + theme::kS2 + kBtn * 2 + kInner;
  const float wMonitor = kPanelPad * 2 + 96.0f;
  float breathing = theme::kS6;  // room between the left and right clusters
  const float wWindows = kPanelPad * 2 + kBtn * 5 + kInner * 4;
  const float wMark = kBtn;
  const float wMore = kBtn;
  // K-112: menus in the system menu bar (macOS, a Linux global menu) leave the top bar.
  const bool sysMenus = sysmenu::active();
  auto measure = [&](bool compact) {
    menusTotal = 0;
    const float pad = compact ? kMenuPadXCompact : kMenuPadX;
    for (size_t i = 0; i < kMenuCount && !sysMenus; ++i) {
      menuW[i] = std::ceil(ImGui::CalcTextSize(i18n::tr(kMenus[i]->name)).x + pad * 2);
      menusTotal += menuW[i];
    }
    segW = compact ? 40.0f : 46.0f;
    tempoW = compact ? 72.0f : 84.0f;
    posW = compact ? 104.0f : 118.0f;
    patW = compact ? 120.0f : 150.0f;
    wTransport = kPanelPad * 2 + (segW * 3 + 4) + theme::kS2 + kBtn * 3 + kInner * 2;
    wTempo = kPanelPad * 2 + tempoW + kInner + kBtn;  // the display and the tap button
    wPos = posW + kInner;  // joins the tempo panel
    wPattern = kPanelPad * 2 + patW + kInner + kBtn;
  };

  // ---- fit: compact sizes first, then hide the lowest priority groups ----
  // K-256: the bar model says which groups are on (Settings > Layouts, the bar list) and their order in
  // each side; the pinned commands are a group of their own.
  enum G { Menus, Transport, Tempo, Pos, Pat, Master, Monitor, Windows, Pins, Count };
  static const char* const kPartOf[Count] = {"menus", "transport", "tempo", "position", "pattern", "master", "monitor", "windows", toolbar::kPins};
  const toolbar::Bar& bar = toolbarui::model(a).bar;
  const std::vector<toolbar::Pin> pins = toolbarui::barPins(a);
  bool show[Count], off[Count];
  for (int g = 0; g < Count; ++g) show[g] = toolbar::shown(bar, kPartOf[g]), off[g] = !show[g];
  if (pins.empty()) show[Pins] = false, off[Pins] = true;  // nothing pinned: the group draws nothing
  if (off[Tempo]) show[Pos] = false;                        // the position shares the tempo's panel
  auto pinW = [&](const toolbar::Pin& pn) {
    return pn.icon ? kBtn : std::ceil(ImGui::CalcTextSize(toolbarui::pinLabel(pn).c_str()).x) + 18;
  };
  float wPins = 0;
  for (const toolbar::Pin& pn : pins) wPins += pinW(pn) + kInner;
  if (!pins.empty()) wPins += kPanelPad * 2 - kInner;
  float widths[Count] = {};
  auto setWidths = [&]() {
    const float w[Count] = {sysMenus ? 0.0f : menusTotal + theme::kS2, wTransport, wTempo, wPos, wPattern, wMaster, wMonitor, wWindows, wPins};
    std::copy(w, w + Count, widths);
  };
  auto squeezed = [&](int g) { return !show[g] && !off[g]; };  // left the bar for want of room (in More)
  measure(false);
  setWidths();
  auto required = [&]() {
    float r = kPad * 2 + wMark;
    int n = 0;
    for (int g = 0; g < Count; ++g)
      if (show[g]) {
        r += widths[g];
        if (g != Pos) ++n;
      }
    bool anyHidden = false;
    for (int g = 0; g < Count; ++g) anyHidden = anyHidden || squeezed(g);
    r += n * kGap + breathing;  // gaps + breathing room between the left and right clusters
    if (anyHidden) r += wMore + kGap;
    return r;
  };
  const float fitW = gDevFitWidth > 0 ? std::min(W, gDevFitWidth) : W;  // K-132: dev override (tests)
  if (required() > fitW) {
    measure(true);
    setWidths();
  }
  if (required() > fitW) {  // K-48: tighter still before any group has to go
    breathing = kGap;
    patW = 112.0f;
    wPattern = kPanelPad * 2 + patW + kInner + kBtn;
    setWidths();
  }
  // Menus stay visible as long as possible (a right click on the mark opens them too). K-256: the model's
  // leave order (the pins first, then the output meter, the master controls, the window buttons, the
  // position, the pattern picker, the menus, the tempo); a group marked always shown stays.
  for (const std::vector<std::string>& level : toolbar::leaveOrder(layout::Preset::Studio, bar))
    for (const std::string& id : level)
      for (int g = 0; g < Count; ++g)
        if (id == kPartOf[g] && required() > fitW) show[g] = false;
  if (!show[Tempo]) show[Pos] = false;
  bool overflow = false;
  for (int g = 0; g < Count; ++g) overflow = overflow || squeezed(g);

  // ---- left cluster: mark + menus ----
  float x = wp.x + kPad;
  {
    ImGui::SetCursorScreenPos(ImVec2(x, btnTop));
    ImGui::InvisibleButton("##mark", ImVec2(kBtn, kBtn));
    bool mh = ImGui::IsItemHovered() || ImGui::IsPopupOpen("##mainmenu");
    // Mark as the home button (guide §08 B: 28 px, hover 0.8 opacity). J-141: a click shows the
    // welcome launcher (the project stays open behind it); a right click opens the main menu.
    theme::drawMark(dl, ImVec2(x, btnTop), kBtn, P.logo, P.chrome, mh ? 0.8f : 1.0f);
    ui::Hint("Home: recent projects and templates (right-click: main menu)");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (program) ImGui::OpenPopup("##programmenu");  // K-79: a style with a program menu opens it from the mark
      else a.home.showHome(a.now);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##mainmenu");
    menuAt(ImVec2(x, wp.y + h - 2));
    if (ImGui::BeginPopup("##mainmenu")) {
      for (const menustyle::Menu* m : kMenus)
        if (ImGui::BeginMenu(i18n::tr(m->name))) {
          drawStyleMenu(a, *m);
          ImGui::EndMenu();
        }
      ImGui::EndPopup();
    }
    menuAt(ImVec2(x, wp.y + h - 2));
    if (program && ImGui::BeginPopup("##programmenu")) {
      drawStyleMenu(a, *program);
      ImGui::EndPopup();
    }
    x += kBtn + theme::kS2;
  }
  auto drawMenus = [&]() {
    if (sysMenus) return;
    static int openMenu = -1;
    bool anyOpen = false;
    for (size_t i = 0; i < kMenuCount; ++i) {
      std::string pid = std::string("##menu_") + kMenus[i]->name;
      ImVec2 bp(x, btnTop);
      ImGui::SetCursorScreenPos(bp);
      ImGui::PushID(static_cast<int>(i));
      bool clicked = ImGui::InvisibleButton("##m", ImVec2(menuW[i], kBtn));
      if (std::strcmp(kMenus[i]->name, "Tools") == 0) noteRect("menutools");
      bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
      ImGui::PopID();
      bool isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (clicked && !isOpen) ImGui::OpenPopup(pid.c_str());
      // Menu-bar behaviour: while one menu is open, hovering another switches to it.
      if (hov && openMenu >= 0 && openMenu != static_cast<int>(i) && !isOpen) ImGui::OpenPopup(pid.c_str());
      isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (isOpen || hov)
        dl->AddRectFilled(bp, ImVec2(bp.x + menuW[i], bp.y + kBtn), isOpen ? P.surface800 : P.surface900, theme::kRadius);
      ImVec2 ts = ImGui::CalcTextSize(i18n::tr(kMenus[i]->name));
      dl->AddText(ImVec2(std::round(bp.x + (menuW[i] - ts.x) * 0.5f), std::round(bp.y + (kBtn - ts.y) * 0.5f)),
                  isOpen || hov ? P.text : P.textDim, i18n::tr(kMenus[i]->name));
      menuAt(ImVec2(bp.x, wp.y + h - 2));
      if (ImGui::BeginPopup(pid.c_str())) {
        anyOpen = true;
        openMenu = static_cast<int>(i);
        drawStyleMenu(a, *kMenus[i]);
        ImGui::EndPopup();
      }
      x += menuW[i];
    }
    if (!anyOpen) openMenu = -1;
    x += theme::kS2;
  };

  // ---- transport ----
  auto drawTransport = [&]() {
    panel(dl, x, wTransport, panelTop);
    noteRectAt("bar_transport", ImVec2(x, panelTop), ImVec2(x + wTransport, panelTop + kPanelH));
    float cx = x + kPanelPad;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    int mode = a.engine.mode() == PlayMode::Song ? 1 : a.engine.mode() == PlayMode::Live ? 2 : 0;
    const char* labels[] = {"PAT", "SONG", "LIVE"};
    const char* hints[] = {keys::withKeys(i18n::N_("Pattern mode: play the selected pattern (%s)"), {keys::cmdKey("transport.songMode", "L")}), keys::withKeys(i18n::N_("Song mode: play the playlist arrangement (%s)"), {keys::cmdKey("transport.songMode", "L")}),
                           "Live mode: launch the clips of the playlist's live area by hand (playlist or pads)"};
    ImGui::PushFont(a.fontBold, theme::kFontCaption);
    if (ui::Segmented("mode", labels, 3, &mode, segW, kBtn, hints))
      a.engine.setMode(mode == 2 ? PlayMode::Live : mode == 1 ? PlayMode::Song : PlayMode::Pattern);
    ImGui::PopFont();
    if (ImGui::BeginPopupContextItem("##modemenu")) {
      actionLinkMenuItems(a, RemoteAction::SongMode);
      ImGui::EndPopup();
    }
    cx += segW * 3 + 4 + theme::kS2;
    bool playing = a.engine.playing();
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    // J-91: the transport acts when the button goes down (a quick tap or a press that slides off still counts).
    // J-112: while a plugin plays on its own (transport stopped), Play stops it first; the tooltip says so.
    const std::string playTip = menuparity::playButtonHint(a);
    if (ui::IconButton("play", playing ? ui::Icon::Pause : ui::Icon::Play, playing, kBtn, playTip.empty() ? i18n::N_("Play / pause (Space)") : playTip.c_str(),
                       theme::rgb(theme::role::kPlay), theme::rgb(theme::role::kPlay, 0.8f), true))
      playing ? a.engine.pause() : (void)menuparity::playPressed(a);
    noteRect("play");
    if (ImGui::BeginPopupContextItem("##playmenu")) {
      actionLinkMenuItems(a, RemoteAction::PlayPause);
      ImGui::EndPopup();
    }
    if (a.engine.countingIn()) {  // count-in: the play button pulses
      ImVec2 c(cx + kBtn * 0.5f, btnTop + kBtn * 0.5f);
      float ph = static_cast<float>(std::fmod(a.now, 0.5) / 0.5);
      dl->AddCircle(c, kBtn * 0.5f + 2 + ph * 3, theme::withAlpha(P.accent, 1.0f - ph), 0, 1.5f);
    }
    cx += kBtn + kInner;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    menuparity::transportTick(a);  // J-27: where playback starts (stop can return there)
    if (ui::IconButton("stop", ui::Icon::Stop, false, kBtn,
                       "Stop and return to the set song position; press again (or double-click) to go back to the beginning and silence "
                       "everything (right-click: options)",
                       0, 0, true))
      menuparity::stopButtonPressed(a);
    noteRect("stop");
    if (ImGui::BeginPopupContextItem("##stopmenu")) {
      menuparity::stopMenuItems(a);
      ImGui::Separator();
      actionLinkMenuItems(a, RemoteAction::Stop);
      ImGui::EndPopup();
    }
    cx += kBtn + kInner;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (ui::IconButton("rec", ui::Icon::Record, a.recordArmed, kBtn,
                       "Record: notes, controller moves and armed mixer inputs (right-click: what to record, punch, count-in)", P.danger,
                       theme::withAlpha(P.danger, 0.7f), true))
      a.recordArmed = !a.recordArmed, recordcard::recordClicked(a, ImGui::GetIO().KeyShift);  // K-123: the card
    noteRect("rec");
    if (ImGui::BeginPopupContextItem("##recmenu")) {
      recordOptionsMenu(a);
      ImGui::EndPopup();
    }
    if (a.stepRecord || a.countInBars > 0) {  // small marks: step record (S) / count-in (bars)
      char tag[8];
      std::snprintf(tag, sizeof tag, "%s", a.stepRecord ? "S" : std::to_string(a.countInBars).c_str());
      dl->AddText(a.fontMonoBold, 9.0f, ImVec2(cx + kBtn - 7, btnTop - 1), P.danger, tag);
    }
    x += wTransport + kGap;
  };

  // ---- tempo + position (one panel) ----
  auto drawTempo = [&]() {
    float pw = wTempo + (show[Pos] ? wPos : 0);
    panel(dl, x, pw, panelTop);
    ImGui::SetCursorScreenPos(ImVec2(x + kPanelPad, btnTop));
    double tempo = s.tempo;
    // With tempo automation, show the tempo at the song position while it plays.
    static TempoMap map;
    static uint64_t mapRev = ~0ull;
    static const Song* mapSong = nullptr;
    if (mapRev != a.doc.revision() || mapSong != &s) {
      map.build(s);
      mapRev = a.doc.revision();
      mapSong = &s;
    }
    const bool automated = !map.constant() && a.engine.mode() == PlayMode::Song;
    if (automated) tempo = map.bpmAt(a.engine.position());
    const double shown = tempo;
    if (tempoLcd(a, &tempo, tempoW) && !(automated && tempo == shown)) a.doc.setParam(songParam(SongParam::Tempo), tempo, true);
    if (automated) ui::Hint("Tempo follows the tempo automation clip; dragging changes the base tempo");
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    ImGui::SetCursorScreenPos(ImVec2(x + kPanelPad + tempoW + kInner, btnTop));
    tapButton(a, kBtn);
    noteRectAt("bar_tempo", ImVec2(x, panelTop), ImVec2(x + wTempo, panelTop + kPanelH));
    if (show[Pos]) {
      const std::string pos = menuparity::positionText(a, a.engine.position());  // J-27: time formats
      ImVec2 p(x + kPanelPad + tempoW + kInner + kBtn + kInner, btnTop);
      ImGui::SetCursorScreenPos(p);
      noteRectAt("bar_pos", p, ImVec2(p.x + posW, p.y + kBtn));
      ImGui::InvisibleButton("##pos", ImVec2(posW, kBtn));
      if (ImGui::BeginPopupContextItem("##posmenu")) {
        menuparity::timeFormatMenu(a);
        ImGui::EndPopup();
      }
      ui::Lcd(dl, p, ImVec2(posW, kBtn), pos.c_str(), false, false);
      ui::Hint("Song position (right-click: bars and beats, minutes and seconds, or timecode)");
      ui::Tooltip("Song position (right-click for the time format)");
    }
    x += pw + kGap;
  };

  // ---- pattern selector ----
  auto drawPattern = [&]() {
    panel(dl, x, wPattern, panelTop);
    noteRectAt("bar_pattern", ImVec2(x, panelTop), ImVec2(x + wPattern, panelTop + kPanelH));
    const Pattern* cur = s.pattern(a.curPattern);
    ImVec2 p(x + kPanelPad, btnTop);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##patbtn", ImVec2(patW, kBtn));
    bool hov = ImGui::IsItemHovered(), open = ImGui::IsPopupOpen("##patpop");
    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##patpop");
    // Mouse wheel steps through patterns (like FL's pattern spinner).
    if (hov && ImGui::GetIO().MouseWheel != 0 && !s.patterns.empty()) {
      int pi = std::clamp(s.patternIndex(a.curPattern) - static_cast<int>(ImGui::GetIO().MouseWheel), 0,
                          static_cast<int>(s.patterns.size()) - 1);
      a.selectPattern(s.patterns[static_cast<size_t>(pi)].id);
    }
    dl->AddRectFilled(p, ImVec2(p.x + patW, p.y + kBtn), open ? P.surface700 : hov ? P.surface800 : P.surface950, theme::kRadius);
    dl->AddRect(p, ImVec2(p.x + patW, p.y + kBtn), open ? P.accent : P.line, theme::kRadius);
    if (cur) dl->AddRectFilled(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 12, p.y + kBtn - 8), theme::rgb(cur->color), 2);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontBody, ImVec2(p.x + 20, std::round(p.y + (kBtn - theme::kFontBody) * 0.5f)),
                        patW - 20 - 24, P.text, cur ? cur->name.c_str() : "No pattern");
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p.x + patW - 13, p.y + kBtn * 0.5f), 12, P.textDim);
    ui::Hint("Selected pattern: click to choose, scroll to step (edited by the channel rack and piano roll)");
    ImGui::SetNextWindowPos(ImVec2(p.x, wp.y + h - 2));
    ImGui::SetNextWindowSizeConstraints(ImVec2(patW, 0), ImVec2(320, 420));
    if (ImGui::BeginPopup("##patpop")) {
      patternList(a);
      ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + patW + kInner, btnTop));
    if (ui::IconButton("newpat", ui::Icon::Plus, false, kBtn, "New pattern (F4)")) {
      ObjectId id = a.doc.addPattern("Pattern " + std::to_string(s.patterns.size() + 1));
      if (id) a.selectPattern(id);
    }
    x += wPattern + kGap;
  };

  // ---- K-256: the pinned commands (one panel) ----
  auto drawPins = [&]() {
    panel(dl, x, wPins, panelTop);
    noteRectAt("bar_pins", ImVec2(x, panelTop), ImVec2(x + wPins, panelTop + kPanelH));
    float cx = x + kPanelPad;
    for (size_t k = 0; k < pins.size(); ++k) {
      const float w = pinW(pins[k]);
      ImGui::PushID(static_cast<int>(k));
      pinButton(a, dl, pins[k], ImVec2(cx, btnTop), ImVec2(w, kBtn));
      ImGui::PopID();
      cx += w + kInner;
    }
    x += wPins + kGap;
  };

  // ---- right cluster (right-aligned): master · monitor · windows · overflow ----
  float rx = wp.x + W - kPad;
  if (collabui::active(a) && W > 900) rx = collabui::topBar(a, dl, rx, panelTop, kPanelH);  // J-111: collaborators
  if (overflow) {
    rx -= wMore;
    ImGui::SetCursorScreenPos(ImVec2(rx, btnTop));
    if (ui::IconButton("more", ui::Icon::More, ImGui::IsPopupOpen("##overflow"), kBtn, "More controls"))
      ImGui::OpenPopup("##overflow");
    noteRect("bar_more");
    rx -= kGap;
  }
  auto drawWindows = [&]() {
    rx -= wWindows;
    panel(dl, rx, wWindows, panelTop);
    noteRectAt("bar_windows", ImVec2(rx, panelTop), ImVec2(rx + wWindows, panelTop + kPanelH));
    struct Wn { const char* id; ui::Icon icon; bool* flag; const char* cmd; const char* name; };
    const Wn wins[] = {{"w_pl", ui::Icon::Playlist, &a.showPlaylist, "view.playlist", i18n::N_("Playlist")},
                       {"w_cr", ui::Icon::Rack, &a.showRack, "view.channelRack", i18n::N_("Channel rack")},
                       {"w_pr", ui::Icon::Piano, &a.showPiano, "view.pianoRoll", i18n::N_("Piano roll")},
                       {"w_mx", ui::Icon::Mixer, &a.showMixer, "view.mixer", i18n::N_("Mixer")},
                       {"w_br", ui::Icon::Browser, &a.showBrowser, "view.browser", i18n::N_("Browser")}};
    float cx = rx + kPanelPad;
    for (const auto& w : wins) {
      ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
      // J-55: the hint names the current shortcut (Settings > Shortcuts may have changed it).
      const char* key = keys::label(w.cmd);
      const std::string hint = std::string(i18n::tr(w.name)) + (key ? std::string(" (") + key + ")" : std::string());
      if (ui::ToggleIcon(w.id, w.icon, *w.flag, kBtn, hint.c_str())) {
        if (!*w.flag || !ui::bringTabForward(w.name)) {  // K-297: a tab hidden behind another comes forward
          *w.flag = !*w.flag;
          if (*w.flag) a.focusWindow = w.name;
        }
      }
      cx += kBtn + kInner;
    }
    rx -= kGap;
  };
  // J-143: the master bus peaks since the last frame (a take, read here only, once per frame,
  // whether the meter is on the bar, in the overflow popup or hidden).
  static LevelMeter out;
  out.update(a.engine.takeMasterPeakL(), a.engine.takeMasterPeakR(), ImGui::GetIO().DeltaTime);
  // Two rows: OUT + stereo peak meter, CPU + load bar and percentage.
  auto monitor = [&](ImVec2 p, float w) {
    const float lf = theme::kFontLabel, labelW = 26.0f;
    const float rowH = 10.0f;
    dl->AddText(a.fontMono, lf, ImVec2(p.x, p.y), P.textFaint, "OUT");
    outMeter(dl, ImVec2(p.x + labelW, p.y + 1), ImVec2(w - labelW, rowH - 1), out);
    // J-61: an overloaded engine reads as a full meter (100 %), never more.
    const float load = std::clamp(a.engine.cpuLoad(), 0.0f, 1.0f);
    char cpu[16];
    std::snprintf(cpu, sizeof cpu, "%d%%", CpuMeter::displayPercent(a.engine.cpuLoad()));
    const float y2 = p.y + rowH + 4;
    const ImU32 col = load > 0.8f ? P.danger : P.textDim;
    dl->AddText(a.fontMono, lf, ImVec2(p.x, y2), P.textFaint, "CPU");
    float tw = a.fontMono->CalcTextSizeA(lf, 1e9f, 0, cpu).x;
    dl->AddText(a.fontMono, lf, ImVec2(p.x + w - tw, y2), col, cpu);
    float bx0 = p.x + labelW, bx1 = p.x + w - 30.0f;
    float by = y2 + std::floor(lf * 0.5f) - 1;
    dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + 3), P.surface950, 2);
    dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + (bx1 - bx0) * std::min(load, 1.0f), by + 3), col, 2);
  };
  auto drawMonitor = [&]() {
    rx -= wMonitor;
    panel(dl, rx, wMonitor, panelTop);
    noteRectAt("bar_monitor", ImVec2(rx, panelTop), ImVec2(rx + wMonitor, panelTop + kPanelH));
    ImVec2 p(rx + kPanelPad + 4, panelTop + 7);
    monitor(p, wMonitor - kPanelPad * 2 - 8);
    ImGui::SetCursorScreenPos(ImVec2(rx, panelTop));
    if (ImGui::InvisibleButton("##mon", ImVec2(wMonitor, kPanelH))) a.showPerformance = !a.showPerformance;
    ui::Hint("Master output level and audio processing load (click: Performance window)");
    rx -= kGap;
  };
  auto masterControls = [&](bool inPopup) {
    if (!s.mixer.empty()) ui::ParamKnob("mvol", mixerParam(s.masterId(), MixerParam::Volume), 13.0f, inPopup ? "Volume" : nullptr);
    ImGui::SameLine(0, kInner);
    ui::ParamKnob("mpitch", songParam(SongParam::MasterPitch), 13.0f, inPopup ? "Pitch" : nullptr, true);
  };
  auto drawMaster = [&]() {
    rx -= wMaster;
    panel(dl, rx, wMaster, panelTop);
    noteRectAt("bar_master", ImVec2(rx, panelTop), ImVec2(rx + wMaster, panelTop + kPanelH));
    ImGui::SetCursorScreenPos(ImVec2(rx + kPanelPad, panelTop + (kPanelH - 26) * 0.5f));
    ImGui::BeginGroup();
    masterControls(false);
    ImGui::EndGroup();
    float cx = rx + kPanelPad + 26 + kInner + 26 + theme::kS2;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    // 0.7.8 (user): the metronome icon no longer flips on the beat; it always shows its rest pose.
    // (J-91 / J-97 tick-tock retired; the ticker stays for the dev pose readout.)
    const bool ticking = false;
    const double beatTicks = static_cast<double>(std::max<Tick>(1, s.beatTicks()));  // the metronome's beat (time signature)
    static motion::MetronomeTicker ticker;
    // K-179: in song time the beats of the meter map (each section's own beat).
    const double beatNow = a.engine.mode() != PlayMode::Pattern ? MeterMap(s).beatCount(a.engine.position()) : a.engine.position() / beatTicks;
    const float mirror = static_cast<float>(ticker.update(beatNow, ticking));
    if (ui::ToggleIcon("metro", ui::Icon::Metronome, a.metronome, kBtn, keys::withKeys(i18n::N_("Metronome (%s)"), {keys::cmdKey("options.metronome", "Ctrl+M")}), mirror)) a.metronome = !a.metronome;
    noteRect("metro");
    gMetroMirror = mirror;
    gMetroBeat = beatNow;
    gMetroTicking = ticker.ticking();
    gMetroAnchor = ticker.anchor();
    if (ImGui::BeginPopupContextItem("##metromenu")) {
      actionLinkMenuItems(a, RemoteAction::Metronome);
      ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(cx + kBtn + kInner, btnTop));
    if (ui::ToggleIcon("typing", ui::Icon::Keyboard, a.typingKeyboard, kBtn, keys::withKeys(i18n::N_("Typing keyboard to piano (%s, right-click: options)"), {keys::cmdKey("options.typingKeyboard", "Ctrl+T")})))
      a.typingKeyboard = !a.typingKeyboard;
    if (ImGui::BeginPopupContextItem("##typingmenu")) {
      typingKeyboardMenu(a);
      ImGui::EndPopup();
    }
    rx -= kGap;
  };
  // 0.7.8: any part can stand on either side (the bar model says which). Each draw above lays its part
  // from `x` rightwards (the left side's) or from `rx` leftwards (the right side's) and steps past it and
  // its gap; a part on the other side is drawn by setting that cursor where it goes. Every part is one
  // kGap from the next; the menus, flat text buttons rather than panels, keep kS2 more beside them.
  auto groupOf = [&](const std::string& id) {
    for (int k = 0; k < Count; ++k)
      if (id == kPartOf[k]) return k;
    return -1;
  };
  auto partW = [&](int g) { return g == Menus ? menusTotal : g == Tempo ? wTempo + (show[Pos] ? wPos : 0) : widths[g]; };
  auto drawGroup = [&](int g) {
    switch (g) {
      case Menus: drawMenus(); break;
      case Transport: drawTransport(); break;
      case Tempo: drawTempo(); break;
      case Pat: drawPattern(); break;
      case Pins: drawPins(); break;
      case Master: drawMaster(); break;
      case Monitor: drawMonitor(); break;
      case Windows: drawWindows(); break;
      default: break;
    }
  };
  auto drawn = [&](int g) { return g >= 0 && g != Pos && show[g] && !(g == Menus && sysMenus); };
  auto fromRight = [&](int g) { return g == Master || g == Monitor || g == Windows; };  // (their draws lay from rx)
  // The left side in the model's order (the mark always first), from the mark rightwards.
  const float leftEdge = x, rightEdge = rx;  // (a right-side draw on the left moves rx: kept here)
  {
    bool first = true;
    for (const toolbar::Part* part : toolbar::sectionParts(layout::Preset::Studio, bar, "left")) {
      const int g = groupOf(part->id);
      if (!drawn(g)) continue;
      if (first && g != Menus) x += kGap;
      first = false;
      const float w = partW(g), x0 = x;
      if (fromRight(g)) rx = x + w;  // (drawn from its right end)
      drawGroup(g);
      toolbarui::notePart(part->id, "left", x0, x0 + w);
      x = x0 + w + (g == Menus ? theme::kS2 : 0.0f) + kGap;
    }
  }
  // The right side in the model's order, laid from the window's edge inwards.
  {
    float r = rightEdge;
    toolbarui::noteBarEdges(leftEdge, r);
    const std::vector<const toolbar::Part*> right = toolbar::sectionParts(layout::Preset::Studio, bar, "right");
    bool edge = true;
    for (auto it = right.rbegin(); it != right.rend(); ++it) {
      const int g = groupOf((*it)->id);
      if (!drawn(g)) continue;
      if (g == Menus && !edge) r -= theme::kS2;
      const float w = partW(g);
      if (fromRight(g)) rx = r;
      else x = r - w;  // (drawn from its left end)
      drawGroup(g);
      toolbarui::notePart((*it)->id, "right", r - w, r);
      r -= w + kGap + (g == Menus ? theme::kS2 : 0.0f);
      edge = false;
    }
  }
  (void)x;

  // ---- overflow popup: whatever did not fit ----
  ImGui::SetNextWindowPos(ImVec2(wp.x + W - kPad, wp.y + h - 2), ImGuiCond_Always, ImVec2(1, 0));
  if (ImGui::BeginPopup("##overflow")) {
    bool sep = false;
    auto section = [&](const char* t) {
      if (sep) ImGui::Separator();
      sep = true;
      ui::Label(t);
    };
    if (squeezed(Menus) && !sysMenus) {
      section("MENU");
      for (const menustyle::Menu* m : kMenus)
        if (ImGui::BeginMenu(i18n::tr(m->name))) {
          drawStyleMenu(a, *m);
          ImGui::EndMenu();
        }
    }
    if (squeezed(Tempo) || squeezed(Pos)) {
      section("TIME");
      if (squeezed(Tempo)) {
        double tempo = s.tempo;
        ImGui::SetNextItemWidth(140);
        if (ImGui::DragScalar("BPM", ImGuiDataType_Double, &tempo, 0.25f, nullptr, nullptr, "%.3f"))
          a.doc.setParam(songParam(SongParam::Tempo), std::clamp(tempo, 10.0, 522.0), true);
        if (ImGui::IsItemDeactivated()) a.doc.endGesture();
        ImGui::SameLine(0, theme::kS2);
        tapButton(a, kBtn);  // K-48: goes with the tempo
      }
      const std::string pos = menuparity::positionText(a, a.engine.position());
      ImGui::PushFont(a.fontMonoBold, theme::kFontBody);
      ImGui::TextUnformatted(pos.c_str());
      ImGui::PopFont();
    }
    if (squeezed(Pat) && ImGui::BeginMenu(i18n::tr("Pattern"))) {
      patternList(a);
      ImGui::EndMenu();
    }
    if (squeezed(Master)) {
      section("MASTER");
      masterControls(true);
      if (ImGui::MenuItem(i18n::tr("Metronome"), keys::label("options.metronome"), a.metronome)) a.metronome = !a.metronome;
      if (ImGui::MenuItem(i18n::tr("Typing keyboard"), keys::label("options.typingKeyboard"), a.typingKeyboard)) a.typingKeyboard = !a.typingKeyboard;
    }
    if (squeezed(Monitor)) {
      section("OUTPUT");
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(160, 26));
      dl = ImGui::GetWindowDrawList();
      monitor(p, 160);
    }
    if (squeezed(Pins)) {  // K-256
      section("PINNED");
      for (size_t k = 0; k < pins.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        if (ImGui::MenuItem(toolbarui::pinLabel(pins[k]).c_str())) toolbarui::runPin(a, pins[k]);
        ImGui::PopID();
      }
    }
    if (squeezed(Windows)) {
      section("WINDOWS");
      if (ImGui::MenuItem(i18n::tr("Playlist"), keys::label("view.playlist"), a.showPlaylist)) a.showPlaylist = !a.showPlaylist;
      if (ImGui::MenuItem(i18n::tr("Channel rack"), keys::label("view.channelRack"), a.showRack)) a.showRack = !a.showRack;
      if (ImGui::MenuItem(i18n::tr("Piano roll"), keys::label("view.pianoRoll"), a.showPiano)) a.showPiano = !a.showPiano;
      if (ImGui::MenuItem(i18n::tr("Mixer"), keys::label("view.mixer"), a.showMixer)) a.showMixer = !a.showMixer;
      if (ImGui::MenuItem(i18n::tr("Browser"), keys::label("view.browser"), a.showBrowser)) a.showBrowser = !a.showBrowser;
    }
    ImGui::EndPopup();
  }
  barListOnEmpty(a, wp.y, wp.y + h);  // K-256

  ImGui::End();
  return h;
}

// K-79: the tracks layout's top bar, two rows: the menus on their own row (they never hide),
// then the control row: pane toggles, transport, one display (position, tempo, metre), the
// modes and the output monitor.
static float drawTopBarLogic(App& a) {
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  constexpr float kMenuRowH = 26.0f, kCtrlRowH = 44.0f, kMenuBtnH = 22.0f;
  const float h = kMenuRowH + kCtrlRowH;
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.chrome));
  ImGui::Begin("##topbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Song& s = a.doc.song();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();
  dl->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + kMenuRowH), P.surface950);
  dl->AddLine(ImVec2(wp.x, wp.y + kMenuRowH - 0.5f), ImVec2(wp.x + W, wp.y + kMenuRowH - 0.5f), P.line);
  dl->AddLine(ImVec2(wp.x, wp.y + h - 1), ImVec2(wp.x + W, wp.y + h - 1), P.line);

  // ---- menu row ----
  const menustyle::Menu* program = nullptr;
  const std::vector<const menustyle::Menu*> menus = barMenus(a, &program);
  {
    float x = wp.x + kPad;
    const float my = wp.y + (kMenuRowH - kMenuBtnH) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(x, my));
    ImGui::InvisibleButton("##mark", ImVec2(kMenuBtnH, kMenuBtnH));
    const bool mh = ImGui::IsItemHovered() || ImGui::IsPopupOpen("##programmenu");
    theme::drawMark(dl, ImVec2(x + 2, my + 2), kMenuBtnH - 4, P.logo, P.surface950, mh ? 0.8f : 1.0f);
    ui::Hint(program ? "Gridline: settings, shortcuts, the welcome screen" : "Home: recent projects and templates");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (program) ImGui::OpenPopup("##programmenu");
      else a.home.showHome(a.now);
    }
    menuAt(ImVec2(x, wp.y + kMenuRowH - 1));
    if (program && ImGui::BeginPopup("##programmenu")) {
      drawStyleMenu(a, *program);
      ImGui::EndPopup();
    }
    x += kMenuBtnH + theme::kS2;
    static int openMenu = -1;
    bool anyOpen = false;
    const float pad = W < 1100 ? kMenuPadXCompact : kMenuPadX;
    for (size_t i = 0; i < menus.size() && !sysmenu::active(); ++i) {  // K-112: not while the system menu bar has them
      const char* name = i18n::tr(menus[i]->name);
      const float mw = std::ceil(ImGui::CalcTextSize(name).x + pad * 2);
      const std::string pid = std::string("##menu_") + menus[i]->name;
      const ImVec2 bp(x, my);
      ImGui::SetCursorScreenPos(bp);
      ImGui::PushID(static_cast<int>(i));
      const bool clicked = ImGui::InvisibleButton("##m", ImVec2(mw, kMenuBtnH));
      if (std::strcmp(menus[i]->name, "Tools") == 0) noteRect("menutools");
      const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
      ImGui::PopID();
      bool isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (clicked && !isOpen) ImGui::OpenPopup(pid.c_str());
      if (hov && openMenu >= 0 && openMenu != static_cast<int>(i) && !isOpen) ImGui::OpenPopup(pid.c_str());
      isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (isOpen || hov) dl->AddRectFilled(bp, ImVec2(bp.x + mw, bp.y + kMenuBtnH), isOpen ? P.surface800 : P.surface900, theme::kRadius);
      const ImVec2 ts = ImGui::CalcTextSize(name);
      dl->AddText(ImVec2(std::round(bp.x + (mw - ts.x) * 0.5f), std::round(bp.y + (kMenuBtnH - ts.y) * 0.5f)), isOpen || hov ? P.text : P.textDim, name);
      menuAt(ImVec2(bp.x, wp.y + kMenuRowH - 1));
      if (ImGui::BeginPopup(pid.c_str())) {
        anyOpen = true;
        openMenu = static_cast<int>(i);
        drawStyleMenu(a, *menus[i]);
        ImGui::EndPopup();
      }
      x += mw;
    }
    if (!anyOpen) openMenu = -1;
  }

  // ---- control row ----
  const float panelTop = wp.y + kMenuRowH + std::floor((kCtrlRowH - kPanelH) * 0.5f);
  const float btnTop = panelTop + (kPanelH - kBtn) * 0.5f;
  const float posW = 118.0f, tempoW = 84.0f, sigW = 46.0f, segW = 44.0f, patW = 140.0f;
  const float wViews = kPanelPad * 2 + kBtn * 5 + kInner * 4;
  const float wTransport = kPanelPad * 2 + kBtn * 6 + kInner * 5;
  const float wDisplay = kPanelPad * 2 + posW + kInner + tempoW + kInner + kBtn + kInner + sigW;
  const float wModes = kPanelPad * 2 + kBtn * 2 + kInner + theme::kS2 + segW * 3 + 4;
  const float wPattern = kPanelPad * 2 + patW + kInner + kBtn;
  const float wMonitor = kPanelPad * 2 + 96.0f;
  const float wLists = kPanelPad * 2 + kBtn;
  // Narrow windows drop the least used groups first: the pattern, the monitor, the modes.
  bool showPattern = true, showMonitor = true, showModes = true;
  auto need = [&]() {
    return kPad * 2 + wViews + wTransport + wDisplay + wLists + kGap * 4 + (showModes ? wModes + kGap : 0) + (showPattern ? wPattern + kGap : 0) +
           (showMonitor ? wMonitor + kGap : 0);
  };
  if (need() > W) showPattern = false;
  if (need() > W) showMonitor = false;
  if (need() > W) showModes = false;

  float x = wp.x + kPad;
  // Pane toggles, lit while shown (the hint names the shortcut).
  {
    panel(dl, x, wViews, panelTop);
    noteRectAt("bar_views", ImVec2(x, panelTop), ImVec2(x + wViews, panelTop + kPanelH));
    const bool editorsOn = a.showPiano || a.showRack;
    struct Tg {
      const char* id;
      ui::Icon icon;
      bool on;
      const char* cmd;
      const char* name;
      int what;
    };
    const Tg tg[] = {{"t_br", ui::Icon::Browser, a.showBrowser, "view.browser", i18n::N_("Browser"), 0},
                     {"t_mx", ui::Icon::Mixer, a.showMixer, "view.mixer", i18n::N_("Mixer"), 1},
                     {"t_ed", ui::Icon::Piano, editorsOn, "view.editors", i18n::N_("Editors"), 2},
                     {"t_cr", ui::Icon::Rack, a.showRack, "view.channelRack", i18n::N_("Channel rack"), 3},
                     {"t_pl", ui::Icon::Playlist, a.showPlaylist, "view.playlist", i18n::N_("Playlist"), 4}};
    float cx = x + kPanelPad;
    for (const Tg& t : tg) {
      ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
      const char* key = keys::label(t.cmd);
      const std::string hint = std::string(i18n::tr(t.name)) + (key ? std::string(" (") + key + ")" : std::string());
      if (ui::ToggleIcon(t.id, t.icon, t.on, kBtn, hint.c_str())) {
        switch (t.what) {
          case 0: a.showBrowser = !a.showBrowser; break;
          case 1: a.showMixer = !a.showMixer; break;
          case 2: layoutui::toggleEditors(a); break;
          case 3: a.showRack = !a.showRack; break;
          default: a.showPlaylist = !a.showPlaylist; break;
        }
      }
      cx += kBtn + kInner;
    }
    x += wViews + kGap;
  }
  // Transport: to the start, back and forward a bar, stop, play, record.
  {
    panel(dl, x, wTransport, panelTop);
    noteRectAt("bar_transport", ImVec2(x, panelTop), ImVec2(x + wTransport, panelTop + kPanelH));
    float cx = x + kPanelPad;
    const Tick bar = std::max<Tick>(1, s.barTicks());
    const Tick pos = static_cast<Tick>(a.engine.position());
    auto hintKey = [](const char* text, const char* cmd) {
      const char* k = keys::label(cmd);
      return std::string(i18n::tr(text)) + (k ? std::string(" (") + k + ")" : std::string());
    };
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (ui::IconButton("tostart", ui::Icon::ChevronLeft, false, kBtn, hintKey("Go to the start", "transport.toStart").c_str())) a.engine.setPosition(0);
    cx += kBtn + kInner;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    const bool songBars = a.engine.mode() != PlayMode::Pattern;  // K-179: real bars of the song's meter map
    if (ui::IconButton("barback", ui::Icon::Left, false, kBtn, hintKey("Back one bar", "transport.rewindBar").c_str()))
      a.engine.setPosition(std::max<Tick>(0, songBars ? MeterMap(s).barStartAt(std::max<Tick>(0, pos - 1)) : (pos - 1) / bar * bar));
    cx += kBtn + kInner;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (ui::IconButton("barfwd", ui::Icon::Right, false, kBtn, hintKey("Forward one bar", "transport.forwardBar").c_str()))
      a.engine.setPosition(songBars ? MeterMap(s).nextBar(pos) : (pos / bar + 1) * bar);
    cx += kBtn + kInner;
    menuparity::transportTick(a);
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (ui::IconButton("stop", ui::Icon::Stop, false, kBtn, "Stop (right-click: options)", 0, 0, true)) menuparity::stopButtonPressed(a);
    noteRect("stop");
    if (ImGui::BeginPopupContextItem("##stopmenu")) {
      menuparity::stopMenuItems(a);
      ImGui::EndPopup();
    }
    cx += kBtn + kInner;
    const bool playing = a.engine.playing();
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    const std::string playTip = menuparity::playButtonHint(a);
    if (ui::IconButton("play", playing ? ui::Icon::Pause : ui::Icon::Play, playing, kBtn, playTip.empty() ? hintKey("Play / pause", "transport.play").c_str() : playTip.c_str(),
                       theme::rgb(theme::role::kPlay), theme::rgb(theme::role::kPlay, 0.8f), true))
      playing ? a.engine.pause() : (void)menuparity::playPressed(a);
    noteRect("play");
    cx += kBtn + kInner;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (ui::IconButton("rec", ui::Icon::Record, a.recordArmed, kBtn, hintKey("Record (right-click: what to record, punch, count-in)", "transport.record").c_str(),
                       P.arm, theme::withAlpha(P.arm, 0.7f), true))
      a.recordArmed = !a.recordArmed, recordcard::recordClicked(a, ImGui::GetIO().KeyShift);  // K-123: the card
    noteRect("rec");
    if (ImGui::BeginPopupContextItem("##recmenu")) {
      recordOptionsMenu(a);
      ImGui::EndPopup();
    }
    x += wTransport + kGap;
  }
  // One display: position, tempo (tap beside it), metre.
  {
    panel(dl, x, wDisplay, panelTop);
    noteRectAt("bar_display", ImVec2(x, panelTop), ImVec2(x + wDisplay, panelTop + kPanelH));
    float cx = x + kPanelPad;
    const std::string posText = menuparity::positionText(a, a.engine.position());
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    ImGui::InvisibleButton("##pos", ImVec2(posW, kBtn));
    if (ImGui::BeginPopupContextItem("##posmenu")) {
      menuparity::timeFormatMenu(a);
      ImGui::EndPopup();
    }
    ui::Lcd(dl, ImVec2(cx, btnTop), ImVec2(posW, kBtn), posText.c_str(), false, false);
    ui::Hint("Song position (right-click: bars and beats, minutes and seconds, or timecode)");
    cx += posW + kInner;
    static TempoMap map;
    static uint64_t mapRev = ~0ull;
    static const Song* mapSong = nullptr;
    if (mapRev != a.doc.revision() || mapSong != &s) {
      map.build(s);
      mapRev = a.doc.revision();
      mapSong = &s;
    }
    const bool automated = !map.constant() && a.engine.mode() == PlayMode::Song;
    double tempo = automated ? map.bpmAt(a.engine.position()) : s.tempo;
    const double shown = tempo;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (tempoLcd(a, &tempo, tempoW) && !(automated && tempo == shown)) a.doc.setParam(songParam(SongParam::Tempo), tempo, true);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    cx += tempoW + kInner;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    tapButton(a, kBtn);
    cx += kBtn + kInner;
    // K-179: the meter at the song position (song mode) or the current pattern's meter.
    char sig[16];
    int sigNum = s.timeSigNum, sigDen = s.timeSigDen;
    const bool songTime = a.engine.mode() != PlayMode::Pattern;
    const Tick at = static_cast<Tick>(a.engine.position());
    if (songTime) {
      meter::meterAt(s, at, sigNum, sigDen);
    } else if (const Pattern* cp = s.pattern(a.curPattern)) {
      sigNum = s.patternMeterNum(*cp);
      sigDen = s.patternMeterDen(*cp);
    }
    std::snprintf(sig, sizeof sig, "%d/%d", sigNum, sigDen);
    ui::Lcd(dl, ImVec2(cx, btnTop), ImVec2(sigW, kBtn), sig, false, false);
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (ImGui::InvisibleButton("##sig", ImVec2(sigW, kBtn))) meterui::openEditor(a, songTime ? meter::changeAt(s, at) : -1);
    ui::Hint(songTime ? "Meter at the song position (click to edit)" : "Meter of the pattern (click: the song's meter at the start)");
    x += wDisplay + kGap;
  }
  // Modes: metronome, count-in, pattern / song / live.
  if (showModes) {
    panel(dl, x, wModes, panelTop);
    float cx = x + kPanelPad;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (ui::ToggleIcon("metro", ui::Icon::Metronome, a.metronome, kBtn,
                       (std::string(i18n::tr("Metronome")) + (keys::label("options.metronome") ? std::string(" (") + keys::label("options.metronome") + ")" : "")).c_str()))
      a.metronome = !a.metronome;
    noteRect("metro");
    cx += kBtn + kInner;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    if (ui::ToggleIcon("countin", ui::Icon::LoopArrows, a.countInBars > 0, kBtn, i18n::N_("Count-in before recording")))
      a.countInBars = a.countInBars > 0 ? 0 : std::max(1, a.countInLength);
    cx += kBtn + theme::kS2;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    int mode = a.engine.mode() == PlayMode::Song ? 1 : a.engine.mode() == PlayMode::Live ? 2 : 0;
    const char* labels[] = {"PAT", "SONG", "LIVE"};
    const char* hints[] = {"Pattern mode: play the selected pattern", "Song mode: play the playlist arrangement", "Live mode: launch the clips of the live area by hand"};
    ImGui::PushFont(a.fontBold, theme::kFontCaption);
    if (ui::Segmented("mode", labels, 3, &mode, segW, kBtn, hints))
      a.engine.setMode(mode == 2 ? PlayMode::Live : mode == 1 ? PlayMode::Song : PlayMode::Pattern);
    ImGui::PopFont();
    x += wModes + kGap;
  }
  if (showPattern) {
    panel(dl, x, wPattern, panelTop);
    const Pattern* cur = s.pattern(a.curPattern);
    const ImVec2 p(x + kPanelPad, btnTop);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##patbtn", ImVec2(patW, kBtn));
    const bool hov = ImGui::IsItemHovered(), open = ImGui::IsPopupOpen("##patpop");
    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##patpop");
    dl->AddRectFilled(p, ImVec2(p.x + patW, p.y + kBtn), open ? P.surface700 : hov ? P.surface800 : P.surface950, theme::kRadius);
    dl->AddRect(p, ImVec2(p.x + patW, p.y + kBtn), open ? P.accent : P.line, theme::kRadius);
    if (cur) dl->AddRectFilled(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 12, p.y + kBtn - 8), theme::rgb(cur->color), 2);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontBody, ImVec2(p.x + 20, std::round(p.y + (kBtn - theme::kFontBody) * 0.5f)), patW - 20 - 24, P.text,
                        cur ? cur->name.c_str() : "No pattern");
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p.x + patW - 13, p.y + kBtn * 0.5f), 12, P.textDim);
    ui::Hint("Selected pattern: click to choose");
    ImGui::SetNextWindowPos(ImVec2(p.x, panelTop + kPanelH));
    ImGui::SetNextWindowSizeConstraints(ImVec2(patW, 0), ImVec2(320, 420));
    if (ImGui::BeginPopup("##patpop")) {
      patternList(a);
      ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + patW + kInner, btnTop));
    if (ui::IconButton("newpat", ui::Icon::Plus, false, kBtn, "New pattern")) {
      ObjectId id = a.doc.addPattern("Pattern " + std::to_string(s.patterns.size() + 1));
      if (id) a.selectPattern(id);
    }
  }
  // Right: the output monitor, then the lists pane toggle.
  float rx = wp.x + W - kPad - wLists;
  {
    panel(dl, rx, wLists, panelTop);
    ImGui::SetCursorScreenPos(ImVec2(rx + kPanelPad, btnTop));
    const char* key = keys::label("edit.history");
    const std::string hint = std::string(i18n::tr("Undo history")) + (key ? std::string(" (") + key + ")" : std::string());
    if (ui::ToggleIcon("t_lists", ui::Icon::ListBullets, a.showHistory, kBtn, hint.c_str())) a.showHistory = !a.showHistory;
    rx -= kGap;
  }
  static LevelMeter out;
  out.update(a.engine.takeMasterPeakL(), a.engine.takeMasterPeakR(), ImGui::GetIO().DeltaTime);
  if (showMonitor) {
    rx -= wMonitor;
    panel(dl, rx, wMonitor, panelTop);
    const ImVec2 p(rx + kPanelPad + 4, panelTop + 7);
    const float w = wMonitor - kPanelPad * 2 - 8, lf = theme::kFontLabel, labelW = 26.0f;
    dl->AddText(a.fontMono, lf, p, P.textFaint, "OUT");
    outMeter(dl, ImVec2(p.x + labelW, p.y + 1), ImVec2(w - labelW, 9.0f), out);
    const float load = std::clamp(a.engine.cpuLoad(), 0.0f, 1.0f);
    char cpu[16];
    std::snprintf(cpu, sizeof cpu, "%d%%", CpuMeter::displayPercent(a.engine.cpuLoad()));
    const float y2 = p.y + 14;
    const ImU32 col = load > 0.8f ? P.danger : P.textDim;
    dl->AddText(a.fontMono, lf, ImVec2(p.x, y2), P.textFaint, "CPU");
    const float tw = a.fontMono->CalcTextSizeA(lf, 1e9f, 0, cpu).x;
    dl->AddText(a.fontMono, lf, ImVec2(p.x + w - tw, y2), col, cpu);
    ImGui::SetCursorScreenPos(ImVec2(rx, panelTop));
    if (ImGui::InvisibleButton("##mon", ImVec2(wMonitor, kPanelH))) a.showPerformance = !a.showPerformance;
    ui::Hint("Master output level and audio processing load (click: Performance window)");
  }
  ImGui::End();
  return h;
}

// K-112c: the Live layout's bar. The menus on a row of their own (the system menu bar takes them
// on macOS), then one flat control row: the tempo group and the launch grid on the left; the
// position, the transport, Back to arrangement and the loop range in the middle; CPU, the typing
// keyboard and the Grid | Arrangement switch on the right. Every control runs the command Studio's
// bar runs; narrow windows drop the loop range first, then CPU and the keyboard.
// K-106: the Tracker's bar is its menu row alone, the output meter and load at its right end; the
// header band below holds every control (position, tempo, pattern, play, stop, record, metronome,
// master volume and pitch, the pages), so nothing shows twice.
static float drawTrackerMenuRow(App& a) {
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = std::round(layout::style(layout::Preset::Tracker).menuRowH);
  const float btnH = h - 4;
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface800));
  ImGui::Begin("##topbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();
  // A raised row: a light hairline on top, a dark one at the foot.
  dl->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + 1), P.lineStrong);
  dl->AddRectFilled(ImVec2(wp.x, wp.y + h - 1), ImVec2(wp.x + W, wp.y + h), P.surface950);
  const float my = std::round(wp.y + (h - btnH) * 0.5f);
  float x = wp.x + 4;
  if (!sysmenu::active()) {
    const menustyle::Menu* program = nullptr;
    const std::vector<const menustyle::Menu*> menus = barMenus(a, &program);
    ImGui::SetCursorScreenPos(ImVec2(x, my));
    ImGui::InvisibleButton("##mark", ImVec2(btnH, btnH));
    const bool mh = ImGui::IsItemHovered() || ImGui::IsPopupOpen("##mainmenu") || ImGui::IsPopupOpen("##programmenu");
    theme::drawMark(dl, ImVec2(x + 2, my + 2), btnH - 4, P.logo, P.surface800, mh ? 0.8f : 1.0f);
    ui::Hint("Home: recent projects and templates");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (program) ImGui::OpenPopup("##programmenu");
      else a.home.showHome(a.now);
    }
    menuAt(ImVec2(x, wp.y + h - 1));
    if (program && ImGui::BeginPopup("##programmenu")) {
      drawStyleMenu(a, *program);
      ImGui::EndPopup();
    }
    x += btnH + 4;
    static int openMenu = -1;
    bool anyOpen = false;
    for (size_t i = 0; i < menus.size(); ++i) {
      const char* name = i18n::tr(menus[i]->name);
      const ImVec2 ts = ImGui::CalcTextSize(name);
      const float mw = std::ceil(ts.x + kMenuPadXCompact * 2);
      const std::string pid = std::string("##menu_") + menus[i]->name;
      const ImVec2 bp(x, my);
      ImGui::SetCursorScreenPos(bp);
      ImGui::PushID(static_cast<int>(i));
      const bool clicked = ImGui::InvisibleButton("##m", ImVec2(mw, btnH));
      if (std::strcmp(menus[i]->name, "Tools") == 0) noteRect("menutools");
      const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
      ImGui::PopID();
      bool isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (clicked && !isOpen) ImGui::OpenPopup(pid.c_str());
      if (hov && openMenu >= 0 && openMenu != static_cast<int>(i) && !isOpen) ImGui::OpenPopup(pid.c_str());
      isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (isOpen) dl->AddRectFilled(bp, ImVec2(bp.x + mw, bp.y + btnH), P.surface950);
      else if (hov) dl->AddRectFilled(bp, ImVec2(bp.x + mw, bp.y + btnH), P.surface700);
      dl->AddText(ImVec2(std::round(bp.x + (mw - ts.x) * 0.5f), std::round(bp.y + (btnH - ts.y) * 0.5f)), isOpen ? P.hot : P.text, name);
      menuAt(ImVec2(bp.x, wp.y + h - 1));
      if (ImGui::BeginPopup(pid.c_str())) {
        anyOpen = true;
        openMenu = static_cast<int>(i);
        drawStyleMenu(a, *menus[i]);
        ImGui::EndPopup();
      }
      x += mw;
    }
    if (!anyOpen) openMenu = -1;
  }
  // ---- right end: the output meter and the processing load, in one sunken box ----
  static LevelMeter out;
  out.update(a.engine.takeMasterPeakL(), a.engine.takeMasterPeakR(), ImGui::GetIO().DeltaTime);
  ImFont* mono = a.fontMono ? a.fontMono : ImGui::GetFont();
  // K-106 cp22: the meter at the menu's own text size (it was the small label size, faint, and a 9 px
  // bar): "OUT", a left and a right row of lit blocks (green, then amber from -6 dB, red from 0 dB;
  // the unlit blocks faintly drawn, so the scale reads at rest), the peak hold, the clip lamp, and the
  // peak in dB in figures.
  const float lf = std::round(ImGui::GetFontSize() * 0.92f);
  char cpu[16];
  std::snprintf(cpu, sizeof cpu, "%3d%%", CpuMeter::displayPercent(a.engine.cpuLoad()));
  const float labelW = std::ceil(mono->CalcTextSizeA(lf, FLT_MAX, 0, "OUT").x) + 6;
  const float dbW = std::ceil(mono->CalcTextSizeA(lf, FLT_MAX, 0, "-00.0").x) + 6;
  // K-106 cp13: a hidden (scripted) run shows no load: its audio runs on the silent test driver beside
  // whatever else the machine is doing, so the reading (often 100%) says nothing about Gridline.
  const bool showCpu = !sysmenu::hiddenRun();
  const float cpuW = showCpu ? std::ceil(mono->CalcTextSizeA(lf, FLT_MAX, 0, "CPU 100%").x) : -10.0f;
  const float meterW = 112, lampW = 6, boxW = 6 + labelW + meterW + 3 + lampW + 6 + dbW + 8 + cpuW + 6;
  const ImVec2 b0(std::round(wp.x + W - 4 - boxW), my), b1(std::round(wp.x + W - 4), my + btnH);
  if (b0.x > x + 8) {
    dl->AddRectFilled(b0, b1, P.surface950);
    dl->AddRectFilled(ImVec2(b0.x, b1.y - 1), b1, P.lineStrong);
    dl->AddRectFilled(ImVec2(b1.x - 1, b0.y), b1, P.lineStrong);
    const float ty = std::round(b0.y + (btnH - lf) * 0.5f);
    dl->AddText(mono, lf, ImVec2(b0.x + 6, ty), P.textDim, "OUT");
    const float mx0 = b0.x + 6 + labelW, my0 = b0.y + 3, my1 = b1.y - 3;
    const float rowH = std::floor((my1 - my0 - 1) * 0.5f);
    constexpr float kSeg = 3.0f, kGap = 1.0f;
    constexpr int kMaxSegs = 64;
    const int segs = std::min(kMaxSegs, static_cast<int>((meterW + kGap) / (kSeg + kGap)));
    // Each block's dB at its end (the meter's scale is linear in dB, LevelMeter::toPos).
    float segDb[kMaxSegs];
    for (int k = 0; k < segs; ++k) {
      segDb[k] = LevelMeter::kFloorDb + (LevelMeter::kCeilDb - LevelMeter::kFloorDb) * static_cast<float>(k + 1) / static_cast<float>(segs);
    }
    auto segColour = [&](float db, bool lit) {
      const ImU32 c = db >= 0.0f ? P.meterHigh : db >= -6.0f ? P.meterMid : P.meterLow;
      return lit ? c : theme::withAlpha(c, 0.16f);
    };
    for (int ch = 0; ch < 2; ++ch) {
      const float y0 = my0 + static_cast<float>(ch) * (rowH + 1), y1 = y0 + rowH;
      const float lvPos = out.levelDb(ch) > LevelMeter::kFloorDb ? LevelMeter::toPos(out.levelDb(ch)) : -1.0f;
      const float hdPos = out.holdDb(ch) > LevelMeter::kFloorDb ? LevelMeter::toPos(out.holdDb(ch)) : -1.0f;
      const int holdSeg = hdPos < 0 ? -1 : std::min(segs - 1, static_cast<int>(hdPos * static_cast<float>(segs)));
      for (int k = 0; k < segs; ++k) {
        const float f0 = static_cast<float>(k) / static_cast<float>(segs);
        const bool lit = lvPos > f0 || k == holdSeg;
        const float sx = mx0 + static_cast<float>(k) * (kSeg + kGap);
        dl->AddRectFilled(ImVec2(sx, y0), ImVec2(sx + kSeg, y1), segColour(segDb[k], lit));
      }
    }
    const ImVec2 l0(mx0 + meterW + 3, my0), l1(mx0 + meterW + 3 + lampW, my1);
    dl->AddRectFilled(l0, l1, out.clipping() ? P.danger : theme::withAlpha(P.danger, 0.16f));
    {  // the louder side's peak hold, in dB
      const float hd = std::max(out.holdDb(0), out.holdDb(1));
      char db[16];
      if (hd > LevelMeter::kFloorDb) {
        std::snprintf(db, sizeof db, "%.1f", static_cast<double>(std::max(-99.9f, hd)));
      } else {
        std::snprintf(db, sizeof db, "%s", "-inf");
      }
      const float tw = mono->CalcTextSizeA(lf, FLT_MAX, 0, db).x;
      dl->AddText(mono, lf, ImVec2(l1.x + 6 + dbW - 6 - tw, ty), hd > 0.0f ? P.danger : hd > LevelMeter::kFloorDb ? P.text : P.textFaint, db);
    }
    const float load = std::clamp(a.engine.cpuLoad(), 0.0f, 1.0f);
    std::string c = std::string("CPU ") + cpu;
    if (showCpu) dl->AddText(mono, lf, ImVec2(b1.x - 6 - cpuW, ty), load > 0.8f ? P.danger : P.textDim, c.c_str());
    ImGui::SetCursorScreenPos(b0);
    if (ImGui::InvisibleButton("##mon", ImVec2(b1.x - b0.x, b1.y - b0.y))) a.showPerformance = !a.showPerformance;
    ui::Hint("Master output level and audio processing load (click: Performance window)");
  }
  barListOnEmpty(a, wp.y, wp.y + h);  // the bar list (its pinned commands) opens here too
  ImGui::End();
  return h;
}

static float drawTopBarLive(App& a) {
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const bool menuRow = !sysmenu::active();
  constexpr float kMenuRowH = 24.0f, kCtrlRowH = 46.0f, kH = 22.0f, kMenuBtnH = 20.0f;
  const float rowTop = menuRow ? kMenuRowH : 0.0f;
  const float h = rowTop + kCtrlRowH;
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.chrome));
  ImGui::Begin("##topbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Song& s = a.doc.song();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();

  // ---- menu row ----
  if (menuRow) {
    dl->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + kMenuRowH), P.surface950);
    const menustyle::Menu* program = nullptr;
    const std::vector<const menustyle::Menu*> menus = barMenus(a, &program);
    float x = wp.x + 6;
    const float my = wp.y + (kMenuRowH - kMenuBtnH) * 0.5f;
    if (program) {  // the program menu (settings, shortcuts, the welcome screen) behind a small mark
      ImGui::SetCursorScreenPos(ImVec2(x, my));
      ImGui::InvisibleButton("##mark", ImVec2(kMenuBtnH, kMenuBtnH));
      const bool mh = ImGui::IsItemHovered() || ImGui::IsPopupOpen("##programmenu");
      theme::drawMark(dl, ImVec2(x + 3, my + 3), kMenuBtnH - 6, P.logo, P.surface950, mh ? 0.8f : 1.0f);
      ui::Hint("Gridline: settings, shortcuts, the welcome screen");
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ImGui::OpenPopup("##programmenu");
      menuAt(ImVec2(x, wp.y + kMenuRowH - 1));
      if (ImGui::BeginPopup("##programmenu")) {
        drawStyleMenu(a, *program);
        ImGui::EndPopup();
      }
      x += kMenuBtnH + 2;
    }
    static int openMenu = -1;
    bool anyOpen = false;
    const float pad = W < 1100 ? kMenuPadXCompact : kMenuPadX - 1;
    for (size_t i = 0; i < menus.size(); ++i) {
      const char* name = i18n::tr(menus[i]->name);
      const float mw = std::ceil(ImGui::CalcTextSize(name).x + pad * 2);
      const std::string pid = std::string("##menu_") + menus[i]->name;
      const ImVec2 bp(x, my);
      ImGui::SetCursorScreenPos(bp);
      ImGui::PushID(static_cast<int>(i));
      const bool clicked = ImGui::InvisibleButton("##m", ImVec2(mw, kMenuBtnH));
      if (std::strcmp(menus[i]->name, "Tools") == 0) noteRect("menutools");
      const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
      ImGui::PopID();
      bool isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (clicked && !isOpen) ImGui::OpenPopup(pid.c_str());
      if (hov && openMenu >= 0 && openMenu != static_cast<int>(i) && !isOpen) ImGui::OpenPopup(pid.c_str());
      isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (isOpen || hov) dl->AddRectFilled(bp, ImVec2(bp.x + mw, bp.y + kMenuBtnH), isOpen ? P.surface800 : P.surface900, theme::kRadiusSm);
      const ImVec2 ts = ImGui::CalcTextSize(name);
      dl->AddText(ImVec2(std::round(bp.x + (mw - ts.x) * 0.5f), std::round(bp.y + (kMenuBtnH - ts.y) * 0.5f)), P.text, name);
      menuAt(ImVec2(bp.x, wp.y + kMenuRowH - 1));
      if (ImGui::BeginPopup(pid.c_str())) {
        anyOpen = true;
        openMenu = static_cast<int>(i);
        drawStyleMenu(a, *menus[i]);
        ImGui::EndPopup();
      }
      x += mw;
    }
    if (!anyOpen) openMenu = -1;
  }

  // ---- control row: flat fields and buttons ----
  const float cy = std::round(wp.y + rowTop + (kCtrlRowH - kH) * 0.5f);
  // K-112c: the readouts in the sans face, as the refs set a control bar's numbers.
  const float fsNum = theme::kFontCaption + 1;
  // A dark field with lit text (a readout); returns the click.
  auto field = [&](const char* id, const char* text, float x, float w, bool dim, const char* hint) {
    ImGui::SetCursorScreenPos(ImVec2(x, cy));
    const bool click = ImGui::InvisibleButton(id, ImVec2(w, kH));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(x, cy), ImVec2(x + w, cy + kH), hov ? theme::mix(P.field, P.surface800, 0.5f) : (P.dark ? P.field : P.surface950), theme::kRadiusSm);
    // K-112c: a 1 px rim a shade darker than the face, as the refs' control bars outline each field.
    dl->AddRect(ImVec2(x, cy), ImVec2(x + w, cy + kH), theme::mix(P.dark ? P.field : P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), theme::kRadiusSm);
    // K-112c (round 28): the readout in the text colour, as every dark ref's control bar (276, 280, 357);
    // the lit colour is kept for switches that are on.
    theme::centreText(dl, a.fontRegular, fsNum, ImVec2(x, cy), ImVec2(x + w, cy + kH), dim ? P.textDim : P.text, text);
    if (hint) ui::Hint(hint);
    return click;
  };
  // A flat button: lit (the hot role) while on.
  auto flat = [&](const char* id, float x, float w, bool on, const char* hint, bool press = false) {
    ImGui::SetCursorScreenPos(ImVec2(x, cy));
    bool click = ImGui::InvisibleButton(id, ImVec2(w, kH));
    if (press) click = ImGui::IsItemActivated();
    const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    // K-112c (round 31): every state answers the pointer: hover lifts, a press sinks, lit or not.
    const ImU32 face = on ? (held ? theme::mix(P.hot, IM_COL32(0, 0, 0, 255), 0.15f) : hov ? theme::mix(P.hot, P.text, 0.12f) : P.hot)
                          : held ? P.surface600 : hov ? P.surface700 : P.surface800;
    dl->AddRectFilled(ImVec2(x, cy), ImVec2(x + w, cy + kH), face, theme::kRadiusSm);
    dl->AddRect(ImVec2(x, cy), ImVec2(x + w, cy + kH), theme::mix(face, IM_COL32(0, 0, 0, 255), 0.35f), theme::kRadiusSm);  // K-112c: the rim
    if (hint) ui::Hint(hint);
    return click;
  };
  auto glyphCol = [&](bool on) { return on ? theme::contrastText(P.hot) : P.text; };
  auto keyHint = [](const char* text, const char* cmd) {
    const char* k = keys::label(cmd);
    return std::string(i18n::tr(text)) + (k ? std::string(" (") + k + ")" : std::string());
  };
  const MeterMap mm(s);
  // bars . beats . sixteenths of a song position, and of a length.
  auto bbt = [&](Tick t, bool length) {
    char b[64];  // (room for three 64-bit numbers)
    if (length) {
      const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.ppq);
      std::snprintf(b, sizeof b, "%lld . %lld . %lld", static_cast<long long>(t / bar), static_cast<long long>((t % bar) / beat),
                    static_cast<long long>((t % beat) * 4 / beat));
    } else {
      const MeterMap::Bbt v = mm.toBbt(std::max<Tick>(0, t));
      const Tick beatLen = std::max<Tick>(1, s.ppq * 4 / std::max(1, v.den));
      std::snprintf(b, sizeof b, "%lld . %d . %lld", static_cast<long long>(v.bar + 1), v.beat + 1, static_cast<long long>(v.tick * 4 / beatLen + 1));
    }
    return std::string(b);
  };

  // Widths, and what fits.
  // Checkpoint 57 (N56 part 6, 2): one spacing scale: kIn between the controls of a group, kGroup
  // between groups. Back to arrangement is an icon button, as the reference's (its name in the hint), so
  // the loop range fits at 1440 wide.
  // K-256: each part of the bar is an item (its width, the gap after it, how it draws) laid out in the
  // bar model's order inside its group; parts switched off in the bar list are left out, and on a narrow
  // window they leave by the model's leave order (the loop range, the key and scale, the CPU, draw and
  // typing switches, then loop and punch), as they always did; a part marked always shown stays.
  constexpr float kIn = 4, kGroup = 12;
  const float posW = 80, loopW = 64, backW = 26;
  const float segGridW = 68, segArrW = std::ceil(a.fontRegular->CalcTextSizeA(theme::kFontBody - 1, 1e9f, 0, i18n::tr("Arrangement")).x) + 32;
  const float keyW = 30, scaleW = 92;  // the key and scale group
  const toolbar::Bar& bar = toolbarui::model(a).bar;
  const std::vector<toolbar::Pin> pins = toolbarui::barPins(a);
  auto pinW = [&](const toolbar::Pin& pn) {
    return pn.icon ? 26.0f : std::ceil(a.fontRegular->CalcTextSizeA(fsNum, 1e9f, 0, toolbarui::pinLabel(pn).c_str()).x) + 16;
  };
  float pinsW = 0;
  for (const toolbar::Pin& pn : pins) pinsW += pinW(pn) + kIn;
  if (pinsW > 0) pinsW -= kIn;
  struct Item {
    const char* id;
    float w, gap;
  };
  // Checkpoint 55 (gap 6): Follow after Back to arrangement; the loop switch beside punch and the range.
  // Checkpoint 58 (N57-13): the system group ends a group gap before the view switch.
  const Item kItems[] = {
      {"browser", 26, kGroup}, {"tempo", 40 + kIn + 64, kIn}, {"meter", 44, kIn}, {"metronome", 50, kIn}, {"quantise", 54, kGroup},
      {"key", keyW + kIn + scaleW, kGroup},
      {"position", posW, kGroup}, {"transport", 26 * 3 + kIn * 2, kIn}, {"recording", 26 * 3 + kIn * 2, kIn},  // N59-01: + Re-enable automation
      {"capture", 26, kIn}, {"sessionRecord", 26, kGroup}, {"recordMenu", 26, kGroup}, {toolbar::kPins, pinsW, kGroup},
      {"back", backW, kIn}, {"follow", 26, kGroup}, {"loop", 26, kIn}, {"punch", 26, kIn}, {"range", loopW + kIn + loopW, 0},
      {"cpu", 66, kIn}, {"draw", 26, kIn}, {"typing", 40, kGroup}, {"view", segGridW + segArrW, 0},
  };
  auto item = [&](const std::string& id) -> const Item* {
    for (const Item& it : kItems)
      if (id == it.id) return &it;
    return nullptr;
  };
  std::set<std::string> gone;  // parts off, or left for want of room
  for (const toolbar::Part& part : bar.parts)
    if (!part.shown) gone.insert(part.id);
  if (pins.empty()) gone.insert(toolbar::kPins);
  auto sectionIds = [&](const char* sec) {
    std::vector<std::string> out;
    for (const toolbar::Part* part : toolbar::sectionParts(layout::Preset::Live, bar, sec))
      if (!gone.count(part->id) && item(part->id)) out.push_back(part->id);
    return out;
  };
  // 0.7.8: the gap between two neighbours of a group: the designed one (kIn inside a cluster such as tap,
  // tempo and meter) when the second is the part that follows the first by design (parts between them
  // switched off or moved away), else the group gap: a part moved next to another, or to another side,
  // keeps the bar's spacing and never touches its neighbour.
  auto gapBetween = [&](const std::string& p, const std::string& q, const std::vector<std::string>& ids) {
    const std::vector<toolbar::PartInfo>& all = toolbar::parts(layout::Preset::Live);
    size_t k = 0;
    while (k < all.size() && p != all[k].id) ++k;
    for (size_t j = k + 1; j < all.size() && std::strcmp(all[j].section, all[k].section) == 0; ++j) {
      if (std::find(ids.begin(), ids.end(), all[j].id) == ids.end()) continue;  // (not drawn in this group)
      return q == all[j].id ? item(p)->gap : kGroup;
    }
    return kGroup;
  };
  auto sectionW = [&](const char* sec) {
    const std::vector<std::string> ids = sectionIds(sec);
    float w = 0;
    for (size_t i = 0; i < ids.size(); ++i) w += item(ids[i])->w + (i + 1 < ids.size() ? gapBetween(ids[i], ids[i + 1], ids) : 0.0f);
    return w;
  };
  auto leftW = [&]() { return sectionW("left"); };
  auto centreW = [&]() { return sectionW("middle"); };
  auto rightW = [&]() { return sectionW("right"); };
  auto fits = [&]() { return 10 + leftW() + kGroup + centreW() + kGroup + rightW() + 8 <= W; };
  for (const std::vector<std::string>& level : toolbar::leaveOrder(layout::Preset::Live, bar)) {
    if (fits()) break;
    for (const std::string& id : level) gone.insert(id);
    if (gone.count("loop")) gone.insert("range");  // the range goes with the loop switch
  }

  // ---- the parts ----
  auto drawPart = [&](const std::string& id, float x) {
    if (id == "browser") {
      // K-297: the browser's show / hide toggle in the top-left corner, as the refs have it (before, the
      // browser could only be closed from the View menu)
      if (flat("##browserlive", x, 26, a.showBrowser, keyHint("Browser: show or hide it", "view.browser").c_str())) a.showBrowser = !a.showBrowser;
      ui::drawIconFitC(dl, ui::Icon::Browser, ImVec2(x + 13, cy + kH * 0.5f), ImVec2(14, 14), glyphCol(a.showBrowser));
      noteRect("browsertoggle");
    } else if (id == "tempo") {
      ImGui::SetCursorScreenPos(ImVec2(x, cy));
      ImGui::InvisibleButton("##taplive", ImVec2(40, kH));
      if (ImGui::IsItemActivated()) a.tapTempo();  // on the press: the moment of the beat
      if (ImGui::BeginPopupContextItem("##tapmenu")) {
        actionLinkMenuItems(a, RemoteAction::TapTempo);
        ImGui::EndPopup();
      }
      const bool tapHot = ImGui::IsItemHovered() || a.tapListening();
      dl->AddRectFilled(ImVec2(x, cy), ImVec2(x + 40, cy + kH), tapHot ? theme::mix(P.field, P.surface800, 0.5f) : (P.dark ? P.field : P.surface950), theme::kRadiusSm);
      dl->AddRect(ImVec2(x, cy), ImVec2(x + 40, cy + kH), theme::mix(P.dark ? P.field : P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), theme::kRadiusSm);
      theme::centreText(dl, a.fontRegular, fsNum, ImVec2(x, cy), ImVec2(x + 40, cy + kH), a.tapListening() ? P.hot : P.text, i18n::tr("Tap"));  // K-112c (round 28): lit while it counts
      ui::Hint(keyHint("Tap tempo: click on every beat. The tempo follows from the second tap, and a pause of over 2 seconds starts over.", "transport.tapTempo").c_str());
      noteRect("taptempo");
      x += 40 + kIn;
      static TempoMap map;
      static uint64_t mapRev = ~0ull;
      static const Song* mapSong = nullptr;
      if (mapRev != a.doc.revision() || mapSong != &s) {
        map.build(s);
        mapRev = a.doc.revision();
        mapSong = &s;
      }
      const bool automated = !map.constant() && a.engine.mode() == PlayMode::Song;
      double tempo = automated ? map.bpmAt(a.engine.position()) : s.tempo;
      const double shown = tempo;
      ImGui::SetCursorScreenPos(ImVec2(x, cy));
      if (tempoLcd(a, &tempo, 64, kH) && !(automated && tempo == shown)) a.doc.setParam(songParam(SongParam::Tempo), tempo, true);
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    } else if (id == "meter") {
      int sigNum = s.timeSigNum, sigDen = s.timeSigDen;
      const Tick at = static_cast<Tick>(a.engine.position());
      meter::meterAt(s, at, sigNum, sigDen);
      char sig[16];
      std::snprintf(sig, sizeof sig, "%d / %d", sigNum, sigDen);
      const bool sigClick = field("##siglive", sig, x, 44, false, "Meter at the song position (click to edit)");
      noteRect("clipssig");
      if (sigClick) meterui::openEditor(a, meter::changeAt(s, at));
    } else if (id == "metronome") {
      // 0.7.8 (user, three reference photos): one box as tall as its neighbours: two circles side by side
      // (the beat: one hollow, one filled) and a small arrow at its right. The box fills with the lit
      // colour while the metronome is on (the circles drawn dark on it). A click on the circles toggles
      // the metronome; the arrow (or a right-click) opens its menu. While the song plays the filled circle
      // swaps sides on every beat of the meter in effect; stopped, the right one is filled. (Playback
      // draws every frame already: the swap adds no redraw of its own.)
      constexpr float kMetroW = 50, kArrowW = 15;
      const float mainW = kMetroW - kArrowW;
      ImGui::SetCursorScreenPos(ImVec2(x, cy));
      const bool mainClick = ImGui::InvisibleButton("##metrolive", ImVec2(mainW, kH));
      const bool mainHov = ImGui::IsItemHovered(), mainHeld = ImGui::IsItemActive();
      noteRect("metro");
      ui::Hint(keyHint("Metronome", "options.metronome").c_str());
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##metromenulive");
      if (mainClick) a.metronome = !a.metronome;
      ImGui::SetCursorScreenPos(ImVec2(x + mainW, cy));
      const bool arrowClick = ImGui::InvisibleButton("##metroarrowlive", ImVec2(kArrowW, kH));
      const bool arrowHov = ImGui::IsItemHovered(), arrowHeld = ImGui::IsItemActive();
      noteRect("clipsmetroarrow");
      ui::Hint(i18n::tr("Metronome options: count-in and controller link"));
      if (arrowClick || ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##metromenulive");
      const bool menuOpen = ImGui::IsPopupOpen("##metromenulive");
      const bool on = a.metronome, hov = mainHov || arrowHov || menuOpen, held = mainHeld || arrowHeld;
      // The faces: off as the bar's other fields (its readouts), on in the lit colour; hover lifts, a press sinks.
      const ImU32 rest = P.dark ? P.field : P.surface950;
      // (lit: hover a lighter orange in either theme, a press darker)
      const ImU32 face = on ? (held ? theme::mix(P.hot, IM_COL32(0, 0, 0, 255), 0.15f) : hov ? theme::mix(P.hot, P.paper, 0.22f) : P.hot)
                            : held ? P.surface800 : hov ? theme::mix(P.field, P.surface800, 0.5f) : rest;
      const ImVec2 b0(x, cy), b1(x + kMetroW, cy + kH);
      dl->AddRectFilled(b0, b1, face, theme::kRadiusSm);
      dl->AddRect(b0, b1, theme::mix(on ? face : rest, IM_COL32(0, 0, 0, 255), 0.35f), theme::kRadiusSm);
      // The icon on the lit box is always dark ink (as the refs draw it), in the light theme too.
      const ImU32 ink = on ? IM_COL32(20, 22, 24, 255) : P.text;
      // The beat: the song's beats (the meter map's, as the transport counts them) while playing.
      bool left = false;
      if (a.engine.playing()) {
        const double beats = a.engine.mode() != PlayMode::Pattern ? mm.beatCount(a.engine.position())
                                                                  : a.engine.position() / static_cast<double>(std::max<Tick>(1, s.beatTicks()));
        const long long n = static_cast<long long>(std::floor(beats + 1e-9));
        left = (n % 2 + 2) % 2 == 1;
      }
      gClipsMetroLeft = left;
      const float r = 4.5f, cyc = std::round(cy + kH * 0.5f) + 0.5f, gap = 2.5f;  // (as big as the refs': near half the box's height)
      const float cxR = std::round(x + mainW * 0.5f + 2.0f + gap * 0.5f + r) + 0.5f, cxL = cxR - 2 * r - gap;
      dl->AddCircle(ImVec2(left ? cxR : cxL, cyc), r - 0.6f, ink, 20, 1.3f);
      dl->AddCircleFilled(ImVec2(left ? cxL : cxR, cyc), r, ink, 20);
      const float ax = std::round(x + mainW + kArrowW * 0.5f - 1.0f), ay = std::round(cy + kH * 0.5f) - 1.0f;
      dl->AddTriangleFilled(ImVec2(ax - 3, ay), ImVec2(ax + 3, ay), ImVec2(ax, ay + 3), ink);
      ImGui::SetNextWindowPos(ImVec2(x, cy + kH + 2));
      gClipsMetroMenu = false;
      if (ImGui::BeginPopup("##metromenulive")) {
        gClipsMetroMenu = true;
        if (ImGui::MenuItem(i18n::tr("Metronome"), keys::label("options.metronome"), a.metronome)) a.metronome = !a.metronome;
        ImGui::SeparatorText(i18n::tr("Count-in"));
        // (the count-in's switch, options.countIn, turns it on at the length picked last)
        const char* const kCountIn[] = {i18n::tr("None"), i18n::tr("1 bar"), i18n::tr("2 bars"), i18n::tr("4 bars")};
        const int kBars[] = {0, 1, 2, 4};
        for (int i = 0; i < 4; ++i) {
          const int bars = kBars[i];
          if (ImGui::MenuItem(kCountIn[i], nullptr, std::max(0, a.countInBars) == bars)) {
            a.countInBars = bars;
            if (bars > 0) a.countInLength = bars;
          }
        }
        ImGui::Separator();
        actionLinkMenuItems(a, RemoteAction::Metronome);
        ImGui::EndPopup();
      }
    } else if (id == "quantise") {
      // Launch grid of every track (one value when they agree).
      const LaunchGrid g0 = s.tracks.empty() ? LaunchGrid::FourBeats : s.tracks[0].launch;
      bool same = true;
      for (const PlaylistTrack& t : s.tracks) same = same && t.launch == g0;
      const char* gl = same ? livelook::gridName(g0) : "Mixed";
      const bool quantClick = field("##quantlive", gl, x, 54, false, "Launch quantisation: where a launched clip starts (every track; each track can have its own in the grid)");
      noteRect("clipsquant");
      if (quantClick) ImGui::OpenPopup("##quantpop");
      ImGui::SetNextWindowPos(ImVec2(x, cy + kH + 2));
      if (ImGui::BeginPopup("##quantpop")) {
        LaunchGrid g = g0;
        if (livelook::gridItems(same ? g0 : LaunchGrid::Count, g, false)) {  // grid-1118: None, 8 Bars ... 1/16
          a.doc.editSong("change launch grid", [g](Song& x2) {
            for (PlaylistTrack& t : x2.tracks) t.launch = g;
          });
        }
        ImGui::EndPopup();
      }
    } else if (id == "key") {
      // The key and scale (the notes editor's scale highlighting): root, then the scale.
      static const char* const kRoots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      const auto& sc = tools::scales();
      const bool on = a.piano.scale > 0 && a.piano.scale < static_cast<int>(sc.size());
      if (field("##rootlive", kRoots[std::clamp(a.piano.scaleRoot, 0, 11)], x, keyW, !on, "Key: the root of the scale the notes editor shows"))
        ImGui::OpenPopup("##rootpop");
      ImGui::SetNextWindowPos(ImVec2(x, cy + kH + 2));
      if (ImGui::BeginPopup("##rootpop")) {
        for (int k = 0; k < 12; ++k)
          if (ImGui::MenuItem(kRoots[k], nullptr, a.piano.scaleRoot == k)) a.piano.scaleRoot = k;
        ImGui::EndPopup();
      }
      x += keyW + kIn;
      const std::string sn = on ? sc[static_cast<size_t>(a.piano.scale)].name : i18n::tr("No scale");
      if (field("##scalelive", sn.c_str(), x, scaleW, !on, "Scale: rows outside it are shaded in the notes editor")) ImGui::OpenPopup("##scalepop");
      ImGui::SetNextWindowPos(ImVec2(x, cy + kH + 2));
      if (ImGui::BeginPopup("##scalepop")) {
        if (ImGui::MenuItem(i18n::tr("No scale"), nullptr, a.piano.scale == 0)) a.piano.scale = 0;
        for (size_t i = 1; i < sc.size(); ++i)
          if (ImGui::MenuItem(sc[i].name, nullptr, a.piano.scale == static_cast<int>(i))) a.piano.scale = static_cast<int>(i);
        ImGui::Separator();
        ImGui::MenuItem(i18n::tr("Snap notes to the scale"), nullptr, &a.piano.snapToScale);
        ImGui::EndPopup();
      }
    } else if (id == "view") {
      const float rx = x;
      const bool grid = livelook::gridView(a);
      // K-112c (round 31): the switch is as tall as every other control on the bar (it stood 2 px taller),
      // its labels centred by the font's capital band, as the other fields.
      ImGui::SetCursorScreenPos(ImVec2(rx, cy));
      if (ImGui::InvisibleButton("##vgrid", ImVec2(segGridW, kH)) && !grid) livelook::showView(a, true);
      const bool hg = ImGui::IsItemHovered(), pg = ImGui::IsItemActive();
      ui::Hint("Clip grid: the tracks as columns of clip slots, scenes as rows (Tab switches the view)");
      ImGui::SetCursorScreenPos(ImVec2(rx + segGridW, cy));
      if (ImGui::InvisibleButton("##varr", ImVec2(segArrW, kH)) && grid) livelook::showView(a, false);
      const bool ha = ImGui::IsItemHovered(), pa = ImGui::IsItemActive();
      ui::Hint("Arrangement: the tracks as lanes along the song's time (Tab switches the view)");
      const ImVec2 g0(rx, cy), g1(rx + segGridW, cy + kH), a1(rx + segGridW + segArrW, cy + kH);
      auto segFill = [&](bool on, bool hov, bool held) { return on ? P.hot : held ? P.surface600 : hov ? P.surface700 : P.surface800; };
      dl->AddRectFilled(g0, g1, segFill(grid, hg, pg), theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
      dl->AddRectFilled(ImVec2(g1.x, g0.y), a1, segFill(!grid, ha, pa), theme::kRadiusSm, ImDrawFlags_RoundCornersRight);
      dl->AddRect(g0, a1, theme::mix(P.surface800, IM_COL32(0, 0, 0, 255), 0.35f), theme::kRadiusSm);  // the rim, as the bar's buttons
      const float fsb = theme::kFontBody - 1;
      const char* tg = i18n::tr("Grid");
      const char* ta = i18n::tr("Arrangement");
      const float s1 = a.fontRegular->CalcTextSizeA(fsb, 1e9f, 0, tg).x, s2 = a.fontRegular->CalcTextSizeA(fsb, 1e9f, 0, ta).x;
      const float ty = theme::textTop(a.fontRegular, fsb, cy, kH);
      dl->AddText(a.fontRegular, fsb, ImVec2(std::round(g0.x + (segGridW - s1) * 0.5f), ty), glyphCol(grid), tg);
      dl->AddText(a.fontRegular, fsb, ImVec2(std::round(g1.x + (segArrW - s2) * 0.5f), ty), glyphCol(!grid), ta);
    } else if (id == "typing") {
      if (flat("##keylive", x, 40, a.typingKeyboard, keyHint("Computer keyboard as a piano keyboard", "options.typingKeyboard").c_str())) a.typingKeyboard = !a.typingKeyboard;
      const ImVec2 ks = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, "KEY");
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round(x + (40 - ks.x) * 0.5f), std::round(cy + (kH - ks.y) * 0.5f)), glyphCol(a.typingKeyboard), "KEY");
    } else if (id == "draw") {
      // K-112c: Draw mode: the editors' pencil on (lit) or off (the select tool).
      const bool drawOn = livelook::drawMode(a);
      if (flat("##drawlive", x, 26, drawOn, "Draw mode: the arrangement and the notes editor draw with the pencil (off: select)")) livelook::setDrawMode(a, !drawOn);
      noteRect("drawmode");
      ui::drawIconFit(dl, ui::Icon::ToolDraw, ImVec2(x + 6, cy + 4), ImVec2(x + 20, cy + kH - 4), glyphCol(drawOn));
    } else if (id == "cpu") {
      // hunt-clips2-1119: the readout keeps its place whatever the number: laid out for the widest one
      // ("CPU 100 %"), the number right-aligned in its slot (centred, "CPU 9 %" to "CPU 11 %" moved every part).
      char num[8];
      std::snprintf(num, sizeof num, "%d", CpuMeter::displayPercent(a.engine.cpuLoad()));
      if (field("##cpulive", "", x, 66, false, "Audio processing load (click: the Performance panel)")) {
        a.showPerformance = !a.showPerformance;
      }
      ImFont* f = a.fontRegular;
      const float wHead = f->CalcTextSizeA(fsNum, FLT_MAX, 0, "CPU ").x, wNum = f->CalcTextSizeA(fsNum, FLT_MAX, 0, "100").x;
      const float wAll = wHead + wNum + f->CalcTextSizeA(fsNum, FLT_MAX, 0, " %").x;
      const float x0 = std::round(x + (66 - wAll) * 0.5f), ty = theme::textTop(f, fsNum, cy, kH);
      const float numR = x0 + wHead + wNum;
      dl->AddText(f, fsNum, ImVec2(x0, ty), P.text, "CPU");
      dl->AddText(f, fsNum, ImVec2(std::round(numR - f->CalcTextSizeA(fsNum, FLT_MAX, 0, num).x), ty), P.text, num);
      dl->AddText(f, fsNum, ImVec2(numR, ty), P.text, " %");
    } else if (id == "position") {
      // C44-20: in the song, counted from the arrangement's start (the end of the clip grid's storage);
      // while clips play from the grid, the transport's own count.
      const Tick posOrigin = a.engine.mode() == PlayMode::Song ? livelook::arrangementOrigin(a) : 0;
      const std::string pos = bbt(std::max<Tick>(0, static_cast<Tick>(a.engine.position()) - posOrigin), false);
      field("##poslive", pos.c_str(), x, posW, false, "Song position: bars . beats . sixteenths");
    } else if (id == "transport") {
      float mx = x;
      const bool playing = a.engine.playing();
      menuparity::transportTick(a);
      const std::string playTip = menuparity::playButtonHint(a);
      // C44-29: the play triangle stays a triangle and turns the launch green while playing (Stop is
      // its own button); a press while playing changes nothing.
      if (flat("##playlive", mx, 26, false, playTip.empty() ? keyHint("Play", "transport.play").c_str() : playTip.c_str(), true) && !playing)
        (void)menuparity::playPressed(a);
      noteRect("play");
      ui::drawIconFit(dl, ui::Icon::Play, ImVec2(mx + 8, cy + 6), ImVec2(mx + 18, cy + kH - 6), playing ? P.launch : glyphCol(false));
      mx += 26 + kIn;
      if (flat("##stoplive", mx, 26, false, "Stop (right-click: options)", true)) menuparity::stopButtonPressed(a);
      noteRect("stop");
      if (ImGui::BeginPopupContextItem("##stopmenu")) {
        menuparity::stopMenuItems(a);
        ImGui::EndPopup();
      }
      dl->AddRectFilled(ImVec2(mx + 9, cy + 7), ImVec2(mx + 17, cy + kH - 7), P.text, 1.0f);
      mx += 26 + kIn;
      if (flat("##reclive", mx, 26, false, keyHint("Record (right-click: what to record, punch, count-in)", "transport.record").c_str(), true))
        livelook::toggleArrangementRecord(a), recordcard::recordClicked(a, ImGui::GetIO().KeyShift);  // K-123: the card
      noteRect("rec");
      if (ImGui::BeginPopupContextItem("##recmenu")) {
        recordOptionsMenu(a);
        ImGui::EndPopup();
      }
      dl->AddCircleFilled(ImVec2(mx + 13, cy + kH * 0.5f), 4.5f, a.recordArmed ? P.arm : theme::withAlpha(P.arm, 0.75f), 16);
      if (a.recordArmed) dl->AddCircle(ImVec2(mx + 13, cy + kH * 0.5f), 7.0f, P.arm, 16, 1.5f);
    } else if (id == "recording") {
      // What a recording takes besides audio: notes on top of the existing ones (overdub), and
      // controller moves (automation). The same switches as the record menu.
      float mx = x;
      RecordOptions& ro = a.recOptions;
      if (flat("##overdublive", mx, 26, ro.overdub, "Overdub: recorded notes join the existing ones (off: they replace them)")) ro.overdub = !ro.overdub;
      ui::drawIconFit(dl, ui::Icon::Plus, ImVec2(mx + 8, cy + 6), ImVec2(mx + 18, cy + kH - 6), glyphCol(ro.overdub));
      mx += 26 + kIn;
      if (flat("##autoarmlive", mx, 26, ro.automation, "Automation arm: knob and controller moves are recorded while recording (into the Arrangement with F9, into the clip with Session record)")) ro.automation = !ro.automation;
      ui::drawIconFit(dl, ui::Icon::ClipAutomation, ImVec2(mx + 7, cy + 5), ImVec2(mx + 19, cy + kH - 5), glyphCol(ro.automation));
      mx += 26 + kIn;
      // Checkpoint 60 (N59-01): Re-enable automation, beside the arm as the reference has it: lit while
      // a control moved by hand during playback holds its value instead of following its automation.
      const int held = automationOverrideCount();
      if (flat("##reenablelive", mx, 26, held > 0,
               held > 0 ? "Re-enable automation: the controls you moved by hand (marked) follow their automation again"
                        : "Re-enable automation: lights when you move an automated control during playback (it then holds your value)") &&
          held > 0) {
        reenableAutomation(a);
      }
      noteRect("reenable");
      ui::drawIconFit(dl, ui::Icon::Redo, ImVec2(mx + 7, cy + 5), ImVec2(mx + 19, cy + kH - 5), held > 0 ? glyphCol(true) : P.textFaint);
    } else if (id == "capture") {
      // K-112c: Capture: what was just played while nothing recorded becomes notes in the pattern.
      const bool kept = a.capture.size() > 0;
      if (flat("##capturelive", x, 26, false, "Capture: the notes you just played, while nothing recorded, into the clip the selected track plays, or a new clip in its slot") && kept)
        livelook::captureNow(a);
      noteRect("capture");
      ui::drawIconFit(dl, ui::Icon::Download, ImVec2(x + 7, cy + 5), ImVec2(x + 19, cy + kH - 5), kept ? P.text : P.textFaint);
    } else if (id == "sessionRecord") {
      // K-112c: Session record: records into the current pattern, looping it (the record button in
      // pattern mode); lit while that runs.
      const bool on = livelook::sessionRecording(a);
      if (flat("##sessreclive", x, 26, on, "Session record: records into the selected slot of the armed track, or adds to the clip it plays (click again to stop; the clip plays on)"))
        livelook::toggleSessionRecord(a);
      noteRect("sessrec");
      const ImVec2 c(x + 13, cy + kH * 0.5f);
      dl->AddRect(ImVec2(c.x - 6, c.y - 6), ImVec2(c.x + 6, c.y + 6), on ? glyphCol(true) : P.text, 2.0f, 1.2f, 0);
      dl->AddCircleFilled(c, 3.0f, on ? glyphCol(true) : P.arm, 12);
    } else if (id == "recordMenu") {
      if (flat("##recoptlive", x, 26, false, "Recording options: what to record, punch, count-in")) ImGui::OpenPopup("##recoptpop");
      ui::drawIconFit(dl, ui::Icon::More, ImVec2(x + 7, cy + kH * 0.5f - 1), ImVec2(x + 19, cy + kH * 0.5f + 2), P.text);
      ImGui::SetNextWindowPos(ImVec2(x, cy + kH + 2));
      if (ImGui::BeginPopup("##recoptpop")) {
        recordOptionsMenu(a);
        ImGui::EndPopup();
      }
    } else if (id == toolbar::kPins) {
      // K-256: the pinned commands, flat buttons with their glyph or their name.
      float px = x;
      for (size_t k = 0; k < pins.size(); ++k) {
        const toolbar::Pin& pn = pins[k];
        const float w = pinW(pn);
        const std::string label = toolbarui::pinLabel(pn);
        ImGui::PushID(static_cast<int>(k));
        if (flat("##pinlive", px, w, false, nullptr)) toolbarui::runPin(a, pn);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label.c_str());
        if (ImGui::BeginPopupContextItem("##pinmenu")) {
          toolbarui::pinMenu(a, pn);
          ImGui::EndPopup();
        }
        ImGui::PopID();
        if (pn.icon) ui::drawIconFitC(dl, toolbarui::pinIcon(pn), ImVec2(px + w * 0.5f, cy + kH * 0.5f), ImVec2(14, 14), glyphCol(false));
        else theme::centreText(dl, a.fontRegular, fsNum, ImVec2(px, cy), ImVec2(px + w, cy + kH), P.text, label.c_str());
        px += w + kIn;
      }
    } else if (id == "back") {
      // Back to arrangement: lit while any track plays a grid clip.
      // grid-1118: lit while the clip grid has taken a track from the arrangement (a clip launched there, even
      // one stopped since): every track plays its arrangement clips again, now.
      const bool anyGrid = a.engine.anyArrangementOverridden();
      if (flat("##backlive", x, backW, anyGrid, "Back to arrangement: every track stops its grid clip and the arrangement plays again")) {
        a.engine.backToArrangement();
        if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
      }
      noteRect("backtoarrangement");
      ui::drawIconFit(dl, ui::Icon::Undo, ImVec2(x + 7, cy + 5), ImVec2(x + 19, cy + kH - 5), glyphCol(anyGrid));
    } else if (id == "follow") {
      // Checkpoint 55 (gap 6): Follow: the Arrangement scrolls with the play position (the person's setting)
      const bool follow = a.settings.getBool("ui.autoScroll");
      if (flat("##followlive", x, 26, follow, "Follow: the Arrangement scrolls to keep the play position in view")) a.settings.set("ui.autoScroll", !follow);
      noteRect("follow");
      ui::drawIconFit(dl, ui::Icon::Follow, ImVec2(x + 6, cy + 5), ImVec2(x + 20, cy + kH - 5), glyphCol(follow));
    } else if (id == "loop") {
      // Checkpoint 56 (N55-04): the loop brace is its own, as the reference's: the song's cycle range (Smooth's
      // cycle reads the same fields), never the time selection. Cmd+L sets it from the selection.
      const bool on = livelook::loopOn(a);
      if (flat("##looplive", x, 26, on, keys::withKeys(i18n::N_("Loop: the Arrangement plays its loop brace over and over (click to switch it on or off; %s sets it from the time selection)"), {keys::cmdKey("playlist.loopMarkers", "Cmd+L")}))) {
        livelook::toggleLoop(a);
      }
      noteRect("loop");
      ui::drawIconFit(dl, ui::Icon::LoopArrows, ImVec2(x + 6, cy + 5), ImVec2(x + 20, cy + kH - 5), glyphCol(on));
    } else if (id == "punch") {
      // Punch in and out (off by default, as the reference's), beside the range.
      if (flat("##punchlive", x, 26, a.recPunch, "Punch in and out: recording keeps only what falls inside the loop brace")) {
        a.recPunch = !a.recPunch;
        a.settings.set("clips.punch", a.recPunch);
      }
      ui::drawIconFit(dl, ui::Icon::Trim, ImVec2(x + 6, cy + 5), ImVec2(x + 20, cy + kH - 5), glyphCol(a.recPunch));
    } else if (id == "range") {
      const bool on = livelook::loopOn(a), brace = s.cycleEnd > s.cycleStart;
      Tick songEnd = 0;
      for (const Clip& c : s.playlist) songEnd = std::max(songEnd, c.start + s.clipLength(c));
      // C44-20: counted from the arrangement's start (the clip grid before it is not part of the song here).
      const Tick org = livelook::arrangementOrigin(a);
      const Tick l0 = brace ? std::max(org, s.cycleStart) : org, l1 = brace ? std::max(l0, s.cycleEnd) : std::max(org, songEnd);
      const std::string ls = bbt(l0 - org, false), ll = bbt(std::max<Tick>(0, l1 - l0), true);
      field("##loopstart", ls.c_str(), x, loopW, !on, keys::withKeys(i18n::N_("Loop start: the loop brace (drag it in the ruler, or %s on a time selection)"), {keys::cmdKey("playlist.loopMarkers", "Cmd+L")}));
      field("##looplen", ll.c_str(), x + loopW + kIn, loopW, !on, "Loop length: bars . beats . sixteenths of the loop brace");
    }
  };
  auto drawSection = [&](const char* sec, float x) {
    const std::vector<std::string> ids = sectionIds(sec);
    for (size_t i = 0; i < ids.size(); ++i) {
      drawPart(ids[i], x);
      toolbarui::notePart(ids[i], sec, x, x + item(ids[i])->w);
      x += item(ids[i])->w + (i + 1 < ids.size() ? gapBetween(ids[i], ids[i + 1], ids) : 0.0f);
    }
  };

  // ---- left: tap, tempo, meter, metronome, launch grid ----
  toolbarui::noteBarEdges(wp.x + 10, wp.x + W - 8);
  drawSection("left", wp.x + 10);
  const float leftEnd = wp.x + 10 + leftW();
  // ---- right: CPU, typing keyboard, the view switch ----
  drawSection("right", wp.x + W - 8 - rightW());
  // ---- middle: position, transport, Back to arrangement, loop ----
  {
    const float cw = centreW();
    // Checkpoint 58 (N57-13): centred in the room between the left and right groups (on the window it left
    // 238 px on one side and 316 on the other at 1920).
    const float roomL = leftEnd + kGroup, roomR = wp.x + W - 8 - rightW() - kGroup;
    float mx = std::round((roomL + roomR - cw) * 0.5f);
    mx = std::min(std::max(mx, leftEnd + kGroup), wp.x + W - 8 - rightW() - kGroup - cw);
    drawSection("middle", mx);
  }
  barListOnEmpty(a, wp.y + rowTop, wp.y + h);  // K-256
  ImGui::End();
  return h;
}

// ------------------------------------------------- Logic control bar ----
// K-112b: the Logic layout's bars. A menu row (in the window where the system menu bar does not
// take the menus) above one control bar: the pane toggles on the left, the transport and the
// display centred, the cycle, count-in and click switches after it, the right pane's toggles
// at the right end with the project's save state. The commands are the same as everywhere.

namespace {

bool logicRegionShown(App& a, const char* region) {
  const layout::Preset p = layout::Preset::Logic;
  return layout::visible(layout::shell(p), layoutui::regionState(a, p), region);
}

// A rounded button of the control bar: an icon or a short label. While on, its glyph is lit (the
// pane toggles and switches); a strong one fills with the accent (play while playing).
bool gBigGlyphs = false;  // cp53: the transport's pills (16 px glyphs at any height)
bool gGrouped = false;  // the pills sit in a shared group container (drawn by barGroup): no face of their own at rest

// The rounded container a run of pills shares (the pane toggles, the transport, the switches).
// K-112b (close-range pass): a capsule (ends fully round), as 216 and 125 draw every group of the bar.
void barGroup(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const float r = std::floor((p1.y - p0.y + 6) * 0.5f);
  dl->AddRectFilled(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p1.x + 3, p1.y + 3), P.surface700, r);
  // R14-07: the outline lies inside the capsule (centred on its edge it stood half a pixel out, so at
  // 2x the groups measured 74 device px against the display's 72).
  dl->AddRect(ImVec2(p0.x - 2.5f, p0.y - 2.5f), ImVec2(p1.x + 2.5f, p1.y + 2.5f), P.lineStrong, r - 0.5f);
}

bool barPill(const char* id, ImVec2 p, ImVec2 size, ui::Icon icon, const char* label, bool on, const char* hint, ImU32 iconCol = 0, bool strong = false) {
  const auto& P = pal();
  ImGui::SetCursorScreenPos(p);
  const bool click = ImGui::InvisibleButton(id, size);
  if (std::strncmp(id, "##lb_", 5) == 0) {  // (dev: the pane buttons, for sdlat and devBarLit)
    noteRect(id + 2);
    gBarLit[id + 2] = {on, ImGui::GetFrameCount()};
  }
  const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q(p.x + size.x, p.y + size.y);
  const bool filled = on && strong;
  // K-112b (checkpoint 22): one quiet hover for the Tracks layout's own controls, the rest face 8 % toward the text (14 % open or held)
  const ImU32 face = filled ? P.hot : held ? theme::mix(P.surface700, P.text, 0.14f) : hov || on ? theme::mix(P.surface700, P.text, 0.08f) : P.surface700;
  // K-112b (close-range pass): inside a capsule a pill's face is round too (216, 125: the lit pane
  // button is a round highlight), so it follows the group's curve at its ends.
  if (!gGrouped || filled || held || hov || on) dl->AddRectFilled(p, q, face, gGrouped ? std::floor(size.y * 0.5f) : 6.0f);
  if (!gGrouped || filled) dl->AddRect(p, q, filled ? P.hot : on ? theme::withAlpha(P.hot, 0.55f) : P.lineStrong, gGrouped ? std::floor(size.y * 0.5f) : 6.0f);
  // Checkpoint 30: glyphs at rest a step under the text (126 and 150 draw them near white), full text on hover.
  const ImU32 ink = filled ? theme::contrastText(P.hot) : on ? P.hot : iconCol ? iconCol : hov ? P.text : theme::mix(P.textDim, P.text, 0.6f);
  if (label) {
    // K-112b (checkpoint 27): centred by the label's ink (the advance box put "1234" 2.5 px right of centre).
    theme::centreText(dl, ImGui::GetFont(), theme::kFontCaption, p, q, on ? ink : P.text, label);
  } else {
    // An icon's first draw rasterises it (a few ms each): a few new ones a frame, so the first
    // frame of this layout (a switch to it on a big project) stays quick; the rest follow.
    static std::set<int> drawn;
    static int newFrame = -1, newCount = 0;
    if (newFrame != ImGui::GetFrameCount()) newFrame = ImGui::GetFrameCount(), newCount = 0;
    const bool known = drawn.count(static_cast<int>(icon)) > 0;
    // Checkpoint 30: one optical size for every glyph in the bar (126 sets them alike): each icon's own
    // drawing fills its box differently, so each is scaled to about 16 px of ink at its longest side
    // (measured from the bar at 100 %: 11 to 19 px before, 14 to 16 now).
    float k = 1.0f;
    switch (icon) {
      case ui::Icon::ToolDraw: case ui::Icon::Keyboard: k = 0.89f; break;
      case ui::Icon::File: k = 0.92f; break;
      case ui::Icon::LoopArrows: k = 0.9f; break;
      case ui::Icon::Question: k = 1.2f; break;
      case ui::Icon::CatController: k = 1.45f; break;
      default: break;
    }
    const ImVec2 c(std::round(p.x + size.x * 0.5f), std::round(p.y + size.y * 0.5f));
    const float isz = std::round((gBigGlyphs || size.y >= 32 ? 16.0f : 14.0f) * k);  // transport 16, the pane pills 14
    if (icon == ui::Icon::ChevronLeft || icon == ui::Icon::Left || icon == ui::Icon::Right) {
      // Checkpoint 35: the start, back and forward buttons as filled transport glyphs (a bar and a
      // triangle, two triangles each way), as 216 and 125 draw them, at the play triangle's height
      // (they were thin 6 x 12 outline arrows beside 13 px filled glyphs).
      const float hh = std::round(isz * 0.375f), tw = std::round(isz * 0.44f);  // at 16: half height 6, triangle 7 wide
      const float cy = c.y;
      auto tri = [&](float tipX, float baseX) {  // clockwise either way (ImGui's anti-aliased fill wants it)
        const float s0 = tipX < baseX ? -hh : hh;
        dl->AddTriangleFilled(ImVec2(tipX, cy), ImVec2(baseX, cy + s0), ImVec2(baseX, cy - s0), ink);
      };
      if (icon == ui::Icon::ChevronLeft) {  // to the start: a bar and a triangle pointing at it
        const float x0 = c.x - std::round((tw + 4) * 0.5f);
        dl->AddRectFilled(ImVec2(x0, cy - hh), ImVec2(x0 + 2, cy + hh), ink);
        tri(x0 + 3, x0 + 3 + tw + 1);
      } else {  // back or forward: two triangles touching
        const float x0 = c.x - tw;
        if (icon == ui::Icon::Left) tri(x0, x0 + tw), tri(x0 + tw, x0 + 2 * tw);
        else tri(x0 + tw, x0), tri(x0 + 2 * tw, x0 + tw);
      }
    } else if (icon == ui::Icon::Info) {
      // The inspector: two switches stacked (a panel of settings), drawn here at the bar's optical size
      // (the bare "i" drew 2 px wide, the faintest glyph in the bar).
      const float w = std::round(isz * 0.9f), hh = std::round(isz * 0.36f), r = hh * 0.5f;
      for (int i = 0; i < 2; ++i) {
        const float y0 = std::round(c.y - isz * 0.5f + (i ? isz - hh : 0.0f)) + 0.5f, x0 = std::round(c.x - w * 0.5f) + 0.5f;
        dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w - 1, y0 + hh - 1), ink, r, 0, 1.5f);
        const float kx = i ? x0 + w - 1 - r : x0 + r;
        dl->AddCircleFilled(ImVec2(kx, y0 + (hh - 1) * 0.5f), r - 1.5f, ink, 12);
      }
    } else if (icon == ui::Icon::Plug) {
      // A1 (checkpoint 49): the Library as a stack of media (a card with a note on two cards behind
      // it), Gridline's own glyph; the plug read as "plugins".
      const float w = std::round(isz * 0.86f), h = std::round(isz * 0.62f);
      const float x0 = std::round(c.x - w * 0.5f) + 0.5f, y1 = std::round(c.y + isz * 0.5f) - 0.5f, y0 = y1 - h;
      dl->AddLine(ImVec2(x0 + 2, y0 - 3), ImVec2(x0 + w - 2, y0 - 3), ink, 1.5f);
      dl->AddLine(ImVec2(x0 + 4, y0 - 6), ImVec2(x0 + w - 4, y0 - 6), ink, 1.5f);
      dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y1), ink, 2.0f, 0, 1.5f);
      const ImVec2 nh(std::round(c.x - 1.5f), std::round(y1 - h * 0.32f));  // the note's head, its stem and flag
      dl->AddCircleFilled(nh, 1.9f, ink, 10);
      dl->AddLine(ImVec2(nh.x + 1.6f, nh.y), ImVec2(nh.x + 1.6f, y0 + 2.5f), ink, 1.3f);
      dl->AddLine(ImVec2(nh.x + 1.6f, y0 + 2.5f), ImVec2(nh.x + 4.0f, y0 + 4.5f), ink, 1.3f);
    } else if (known || newCount < 3) {
      if (!known) drawn.insert(static_cast<int>(icon)), ++newCount;
      ui::drawIcon(dl, icon, c, isz, ink);
    }
  }
  if (hint) ui::Hint(hint);
  return click;
}

std::string hintWithKey(const char* text, const char* cmd) {
  const char* k = cmd ? keys::label(cmd) : nullptr;
  return std::string(i18n::tr(text)) + (k ? std::string(" (") + k + ")" : std::string());
}

}  // namespace

// K-112b: cycle on or off. On without a range: the range cycle last used, else four bars from the bar
// the song position is in. Off: the range is kept for the next time.
void toggleCycle(App& a) {
  if (livelook::active(a)) {  // K-112c checkpoint 56 (N55-04): in Clips, its loop brace (the same song fields)
    livelook::toggleLoop(a);
    return;
  }
  PlaylistState& pst = a.playlist;
  const Song& s = a.doc.song();
  if (pst.selEnd > pst.selStart) {
    pst.selStart = pst.selEnd = 0;  // (the song keeps the range: logicCycleSync)
  } else if (s.cycleEnd > s.cycleStart) {  // R02-04: the range last used, from the project
    pst.selStart = s.cycleStart, pst.selEnd = s.cycleEnd;
  } else {
    const MeterMap mm(a.doc.song());
    const Tick from = mm.barStartAt(static_cast<Tick>(a.engine.position()));
    Tick to = from;
    for (int k = 0; k < 4; ++k) to = mm.nextBar(to);
    pst.selStart = from, pst.selEnd = to;
  }
}

void cycleToSelection(App& a) {
  const Song& s = a.doc.song();
  Tick lo = -1, hi = -1;
  for (const Clip& c : s.playlist)
    if (a.playlist.selected.count(c.id)) {
      lo = lo < 0 ? c.start : std::min(lo, c.start);
      hi = std::max(hi, c.start + s.clipLength(c));
    }
  if (lo < 0 || hi <= lo) {
    a.setHint(i18n::tr("Select regions first: the cycle then covers them"));
    return;
  }
  a.playlist.selStart = lo, a.playlist.selEnd = hi;
}

bool locators(const App& a, Tick& l, Tick& r) {
  const PlaylistState& pst = a.playlist;
  const Song& s = a.doc.song();
  if (pst.selEnd > pst.selStart) l = pst.selStart, r = pst.selEnd;
  else if (s.cycleEnd > s.cycleStart) l = s.cycleStart, r = s.cycleEnd;
  else return false;
  return true;
}

void gotoLocator(App& a, bool right) {
  Tick l = 0, r = 0;
  if (!locators(a, l, r)) {
    a.setHint(i18n::tr("No locators set: drag across the bar ruler to set them"));
    return;
  }
  if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
  a.engine.setPosition(right ? r : l);
}

void locatorsToSelection(App& a) {
  if (a.playlist.selEnd > a.playlist.selStart) return cycleToSelection(a);
  const Song& s = a.doc.song();
  Tick lo = -1, hi = -1;
  for (const Clip& c : s.playlist)
    if (a.playlist.selected.count(c.id)) {
      lo = lo < 0 ? c.start : std::min(lo, c.start);
      hi = std::max(hi, c.start + s.clipLength(c));
    }
  if (lo < 0 || hi <= lo) {
    a.setHint(i18n::tr("Select regions first: the locators then frame them"));
    return;
  }
  // Transport state (R04-10): no undo step; Cycle stays off, so the area shows grey.
  a.doc.patchSong([lo, hi](Song& x) { x.cycleStart = lo, x.cycleEnd = hi, x.cycleOn = false; });
}

void gotoMarker(App& a, int dir) {
  const Song& s = a.doc.song();
  const Tick pos = static_cast<Tick>(a.engine.position());
  Tick best = -1;
  for (const Marker& m : s.markers) {
    if (dir < 0 ? m.pos >= pos : m.pos <= pos) continue;
    if (best < 0 || (dir < 0 ? m.pos > best : m.pos < best)) best = m.pos;
  }
  if (best >= 0) a.engine.setPosition(best);
}

// "bar beat division tick", each from 1 ("9", "9 3", "9 3 2 1"); dots or spaces between.
void goToPositionPrompt(App& a) {
  menus::openPrompt("Go to position", "1 1 1 1", [&a](const std::string& t) {
    std::vector<long> v;
    std::string cur;
    for (const char ch : t + " ") {
      if (ch >= '0' && ch <= '9') cur += ch;
      else if (!cur.empty()) v.push_back(std::atol(cur.c_str())), cur.clear();
    }
    if (v.empty() || v[0] < 1) return;
    const Song& s = a.doc.song();
    const MeterMap mm(s);
    Tick at = mm.barStart(std::min<long>(v[0], 100000) - 1);  // (barStart counts from 0)
    const Tick beat = std::max<Tick>(1, s.ppq * 4 / std::max(1, s.timeSigDen));
    if (v.size() > 1) at += beat * std::max<long>(0, v[1] - 1);
    if (v.size() > 2) at += beat / 4 * std::max<long>(0, v[2] - 1);
    if (v.size() > 3) at += std::max<long>(0, v[3] - 1);
    a.engine.setPosition(at);
  });
}

namespace {
// K-314 (A-06): Smooth shows the song's cycle in the playlist's time selection, which is Studio's time
// selection and the Tracker's Loop block in their layouts; kept on entering Smooth, given back on leaving.
struct SmoothStash {
  bool held = false;
  Tick a = 0, b = 0;
  uint64_t epoch = 0;
  int frame = -10;  // the last frame Smooth synced its cycle
};
SmoothStash gSmoothStash;
}  // namespace

void cycleLayoutFrame(App& a) {
  if (!gSmoothStash.held || ImGui::GetFrameCount() - gSmoothStash.frame <= 1) {  // (Smooth drew its control bar this frame or the last)
    return;
  }
  if (gSmoothStash.epoch == a.doc.projectEpoch()) {  // (another project opened meanwhile: its own range stands)
    a.playlist.selStart = gSmoothStash.a, a.playlist.selEnd = gSmoothStash.b;
  }
  gSmoothStash.held = false;
}

bool playLoop(const App& a, Tick& from, Tick& to) {
  const Song& s = a.doc.song();
  const bool songLoop = s.cycleOn && s.cycleEnd > s.cycleStart;
  from = to = 0;
  if (!livelook::active(a) && a.playlist.selEnd > a.playlist.selStart) {
    from = a.playlist.selStart, to = a.playlist.selEnd;
  } else if (songLoop) {
    from = s.cycleStart, to = s.cycleEnd;
  }
  return to > from;
}

// R02-04: in the Tracks layout the cycle area is the arrangement's range (PlaylistState::selStart /
// selEnd) and is saved with the project (Song::cycleStart / cycleEnd / cycleOn).
// R13-02 (checkpoint 53): the song holds the cycle and the view follows it.
//  - The view takes the song's cycle whenever the song's changes, whatever changed it: a project
//    loaded, a collaborator's edit, joining a shared project, a menu command, a script.
//  - The song takes the view's only when the person here changed the view (a ruler drag, a click,
//    C), once the mouse is up. A frame where only the song moved never writes, so a peer never
//    sends back a cycle it was just sent, and a joining peer never writes its empty view over the host's.
// R04-10: the cycle is transport state, as the reference's: changing it is never an undo step and
// does not mark the project changed (it is saved with the next save). An undo or redo that brings
// back an older cycle with the rest of its step does not move it: the cycle shown stays.
void logicCycleSync(App& a) {
  PlaylistState& pst = a.playlist;
  const Song& s = a.doc.song();
  struct Seen {
    Tick a = 0, b = 0;  // the view's range (0, 0: cycle off)
    Tick songA = 0, songB = 0;  // the song's, as last synced
    bool songOn = false;
    uint64_t epoch = UINT64_MAX;
    unsigned long long moves = 0;
  };
  static Seen seen;
  // An undo / redo this frame (not a history the session cleared or rebuilt: a collaborator's
  // edits arrive without one, and their cycle is followed).
  const unsigned long long moves = undoui::historyMoves();
  const bool historyMoved = moves != seen.moves;
  seen.moves = moves;
  const auto takeSong = [&] {
    pst.selStart = seen.a = s.cycleOn ? s.cycleStart : 0;
    pst.selEnd = seen.b = s.cycleOn ? s.cycleEnd : 0;
    seen.songA = s.cycleStart, seen.songB = s.cycleEnd, seen.songOn = s.cycleOn;
  };
  // K-112c checkpoint 56 (N55-04): coming into Smooth from another layout brings the song's cycle in too
  // (another layout's time selection is not Smooth's cycle: Clips keeps its loop brace in these fields).
  static int lastFrame = -10;
  const bool entered = ImGui::GetFrameCount() - lastFrame > 1;
  lastFrame = ImGui::GetFrameCount();
  if (entered && !gSmoothStash.held) {  // K-314 (A-06): the other layout's time selection, back when Smooth is left
    gSmoothStash = {true, pst.selStart, pst.selEnd, a.doc.projectEpoch(), lastFrame};
  }
  gSmoothStash.frame = lastFrame;
  if (a.doc.projectEpoch() != seen.epoch || entered) {  // a project loaded: its cycle comes in
    seen.epoch = a.doc.projectEpoch();
    takeSong();
    return;
  }
  const bool songChanged = s.cycleStart != seen.songA || s.cycleEnd != seen.songB || s.cycleOn != seen.songOn;
  const Tick pa = pst.selEnd > pst.selStart ? pst.selStart : 0, pb = pst.selEnd > pst.selStart ? pst.selEnd : 0;
  const bool viewChanged = pa != seen.a || pb != seen.b;
  if (viewChanged && ImGui::IsAnyMouseDown()) return;  // (written when the drag ends)
  if (!viewChanged) {
    if (!songChanged) return;
    if (!historyMoved) {  // the song's cycle moved (not by this view): the view follows
      takeSong();
      return;
    }
    // An undo / redo brought back an older cycle: the song keeps the one it had (R04-10).
    const Tick ka = seen.songA, kb = seen.songB;
    const bool kon = seen.songOn;
    a.doc.patchSong([&](Song& x) { x.cycleStart = ka, x.cycleEnd = kb, x.cycleOn = kon; });
    return;
  }
  // The person here changed the cycle: the song takes it (off keeps the range for the next time).
  seen.a = pa, seen.b = pb;
  if (pb > pa ? !(s.cycleOn && s.cycleStart == pa && s.cycleEnd == pb) : s.cycleOn) {
    a.doc.patchSong([&](Song& x) {
      x.cycleOn = pb > pa;
      if (pb > pa) x.cycleStart = pa, x.cycleEnd = pb;
    });
  }
  seen.songA = s.cycleStart, seen.songB = s.cycleEnd, seen.songOn = s.cycleOn;
}

static float drawLogicControlBar(App& a) {
  logicCycleSync(a);
  const auto& P = pal();
  const layout::Style& ls = layout::style(layout::Preset::Logic);
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const bool menusHere = !sysmenu::active();
  const float rowH = menusHere ? ls.menuRowH : 0.0f, barH = ls.controlBarH;
  // R01-27: the Toolbar button shows the toolbar, a strip of labelled buttons under the control
  // bar (055: icons over their names), as the reference's does.
  const bool toolStrip = a.settings.getBool("ui.logicToolbar");
  const float stripH = toolStrip ? 50.0f : 0.0f;
  const float h = rowH + barH + stripH;
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface900));
  ImGui::Begin("##topbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Song& s = a.doc.song();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();

  // ---- the menu row: the program menu (its name in bold), then the layout's menus ----
  if (menusHere) {
    dl->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + rowH), P.chrome);
    dl->AddLine(ImVec2(wp.x, wp.y + rowH - 0.5f), ImVec2(wp.x + W, wp.y + rowH - 0.5f), P.line);
    const menustyle::Menu* program = nullptr;
    const std::vector<const menustyle::Menu*> menus = barMenus(a, &program);
    const float pad = W < 1100 ? kMenuPadXCompact : 8.0f;
    float x = wp.x + kPad;
    constexpr int kNone = -2;  // K-297: -1 is the program menu (hovering File from it switched nothing)
    static int openMenu = kNone;
    bool anyOpen = false;
    auto menuButtonAt = [&](int i, const char* name, ImFont* font, const menustyle::Menu* m, const char* pid) {
      const float mw = std::ceil(font->CalcTextSizeA(ImGui::GetFontSize(), 1e9f, 0, name).x + pad * 2);
      const ImVec2 bp(x, wp.y + 1);
      ImGui::SetCursorScreenPos(bp);
      ImGui::PushID(i);
      const bool clicked = ImGui::InvisibleButton("##m", ImVec2(mw, rowH - 2));
      if (m && m->name && std::strcmp(m->name, "Tools") == 0) noteRect("menutools");
      const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
      ImGui::PopID();
      bool isOpen = ImGui::IsPopupOpen(pid);
      if (clicked && !isOpen) ImGui::OpenPopup(pid);
      if (m && (m->name || i < 0) && !isOpen && devMenuTake(i < 0 ? "Gridline" : m->name)) ImGui::OpenPopup(pid);  // dev: "menu open <name>"
      if (hov && openMenu != kNone && openMenu != i && !isOpen) ImGui::OpenPopup(pid);
      isOpen = ImGui::IsPopupOpen(pid);
      if (isOpen || hov) dl->AddRectFilled(bp, ImVec2(bp.x + mw, bp.y + rowH - 2), isOpen ? P.surface800 : P.surface900, theme::kRadiusSm);
      const ImVec2 ts = font->CalcTextSizeA(ImGui::GetFontSize(), 1e9f, 0, name);
      dl->AddText(font, ImGui::GetFontSize(), ImVec2(std::round(bp.x + (mw - ts.x) * 0.5f), std::round(bp.y + (rowH - 2 - ts.y) * 0.5f)), P.text, name);
      menuAt(ImVec2(bp.x, wp.y + rowH - 1));
      if (ImGui::BeginPopup(pid)) {
        anyOpen = true;
        openMenu = i;
        if (m) drawStyleMenu(a, *m);
        ImGui::EndPopup();
      }
      x += mw;
    };
    if (program) menuButtonAt(-1, "Gridline", a.fontBold, program, "##programmenu");
    for (size_t i = 0; i < menus.size(); ++i) {
      const std::string pid = std::string("##menu_") + menus[i]->name;
      menuButtonAt(static_cast<int>(i), i18n::tr(menus[i]->name), a.fontRegular, menus[i], pid.c_str());
    }
    if (!anyOpen) openMenu = kNone;
    // The project and its save state, as the window's title: centred in the row when it fits,
    // else at the row's right end (the status bar's, which this layout has not).
    std::string proj = a.projectFile().empty() ? i18n::tr("Untitled project") : fs::path(a.projectFile()).stem().string();
    if (std::string shared = collabui::projectTitle(a); !shared.empty()) proj = shared;
    const float tfs = theme::kFontLabel + 1, menusEnd = x + 16, rowEnd = wp.x + W - kPad;  // A7: 12 px, dim, centred
    const float full = a.fontRegular->CalcTextSizeA(tfs, 1e9f, 0, proj.c_str()).x;
    const float tw = std::min(full, std::max(0.0f, rowEnd - menusEnd - 12));
    if (tw > 40) {
      float tx = std::round(wp.x + (W - tw) * 0.5f);
      if (tx < menusEnd + 12) tx = rowEnd - tw;
      const float ty = std::round(wp.y + (rowH - tfs) * 0.5f) - 1;
      theme::textEllipsis(dl, a.fontRegular, tfs, ImVec2(tx, ty), tw, P.textDim, proj.c_str());
      if (a.doc.modified()) dl->AddCircleFilled(ImVec2(tx - 8, ty + tfs * 0.5f + 1), 3.0f, P.text, 12);
      noteRectAt("logic_title", ImVec2(tx, ty), ImVec2(tx + tw, ty + tfs));
    }
  }
  const float top = wp.y + rowH;
  dl->AddLine(ImVec2(wp.x, wp.y + h - 0.5f), ImVec2(wp.x + W, wp.y + h - 0.5f), P.line);
  noteRectAt("logic_bar", ImVec2(wp.x, top), ImVec2(wp.x + W, top + barH));
  if (toolStrip) {  // R01-27: the toolbar strip
    const float sy0 = top + barH, sy1 = sy0 + stripH;
    dl->AddRectFilled(ImVec2(wp.x, sy0), ImVec2(wp.x + W, sy1), theme::mix(P.surface900, P.surface800, 0.5f));
    dl->AddLine(ImVec2(wp.x, sy0 + 0.5f), ImVec2(wp.x + W, sy0 + 0.5f), P.line);
    noteRectAt("logic_toolbar", ImVec2(wp.x, sy0), ImVec2(wp.x + W, sy1));
    struct Btn {
      const char* id;
      ui::Icon icon;
      const char* name;
      const char* hint;
      const char* cmd;  // its key, shown in the hint
      void (*run)(App&);
      bool (*enabled)(App&);
    };
    static const Btn btns[] = {
        {"##tb_loc", ui::Icon::LoopArrows, i18n::N_("Cycle"), i18n::N_("Set the cycle to the selected regions"), "transport.cycleSelection",
         [](App& x) { cycleToSelection(x); }, [](App& x) { return !x.playlist.selected.empty(); }},
        {"##tb_split", ui::Icon::ToolSlice, i18n::N_("Split"), i18n::N_("Split the selected regions at the playhead"), "playlist.splitAtSongPos",
         [](App& x) { tracksSplitAtPlayhead(x); }, [](App& x) { return !x.playlist.selected.empty(); }},
        {"##tb_join", ui::Icon::CirclesMerged, i18n::N_("Join"), i18n::N_("Join the selected regions into one"), "playlist.merge",
         [](App& x) { tracksJoin(x); },  // checkpoint 47: the Join tool's rules
         [](App& x) { return x.playlist.selected.size() >= 2; }},
        {"##tb_rep", ui::Icon::Clone, i18n::N_("Repeat"), i18n::N_("Repeat the selected regions after themselves"), "playlist.duplicate",
         [](App& x) { menus::editDuplicate(x); }, [](App& x) { return !x.playlist.selected.empty(); }},
        {"##tb_mute", ui::Icon::ToolMute, i18n::N_("Mute"), i18n::N_("Mute or unmute the selected regions"), "playlist.mute",
         [](App& x) {
           const auto sel = x.playlist.selected;
           x.doc.editClips("mute clips", [&](std::vector<Clip>& cs) {
             for (auto& c : cs)
               if (sel.count(c.id)) c.muted = !c.muted;
           });
         },
         [](App& x) { return !x.playlist.selected.empty(); }},
        {"##tb_mark", ui::Icon::Flag, i18n::N_("Marker"), i18n::N_("Add a marker at the playhead (its nearest bar)"), "playlist.marker",
         [](App& x) { tracksAddMarker(x); }, nullptr},
        {"##tb_auto", ui::Icon::ClipAutomation, i18n::N_("Automation"), i18n::N_("Show or hide the selected tracks' automation"), "view.automation",
         [](App&) { toggleAutomationLanes(); }, nullptr},
        {"##tb_tools", ui::Icon::ToolSelect, i18n::N_("Tools"), i18n::N_("The tracks area's tool menu"), "playlist.toolMenu",
         [](App&) { openToolMenu(); }, nullptr},
        {"##tb_zoom", ui::Icon::ToolZoom, i18n::N_("Zoom"), i18n::N_("Zoom the tracks area to fit the project"), "playlist.zoomFit",
         [](App& x) { x.playlist.fitRequest = true; }, nullptr},
    };
    const float bw = 76, bh = stripH - 6;
    float bx = wp.x + kPad + 3;
    for (const Btn& b : btns) {
      if (bx + bw > wp.x + W - kPad) break;  // a narrow window: the strip shows what fits
      const ImVec2 p0(bx, sy0 + 3), p1(bx + bw, sy0 + 3 + bh);
      const bool en = !b.enabled || b.enabled(a);
      ImGui::SetCursorScreenPos(p0);
      ImGui::BeginDisabled(!en);
      const bool click = ImGui::InvisibleButton(b.id, ImVec2(bw, bh));
      const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled), held = ImGui::IsItemActive();
      ImGui::EndDisabled();
      if (hov || held) dl->AddRectFilled(p0, p1, theme::mix(P.surface700, P.text, held ? 0.14f : 0.08f), 8.0f);
      const ImU32 ink = !en ? P.textFaint : hov ? P.text : theme::mix(P.textDim, P.text, 0.6f);
      ui::drawIcon(dl, b.icon, ImVec2(std::round((p0.x + p1.x) * 0.5f), p0.y + 13), 16, ink);
      theme::centreText(dl, a.fontRegular, theme::kFontCaption, ImVec2(p0.x, p0.y + 24), ImVec2(p1.x, p1.y - 2), ink, i18n::tr(b.name));
      if (hov) {
        const char* k = keys::label(b.cmd);
        ui::Hint(std::string(i18n::tr(b.hint)) + (k ? std::string(" (") + k + ")" : std::string()));
      }
      if (click && en) b.run(a);
      bx += bw + 2;
    }
  }

  // ---- left: the panes ----
  // Checkpoint 30: pills 34 wide on a 28 px capsule, about the spacing 126 gives its buttons (1.2 times the
  // capsule's height), each glyph about half the capsule's height.
  // cp53 (polish): every part of the bar is one height and stands on one centre line: the groups'
  // capsules (pills + their 3 px rim) and the display are all kGroupH tall, and neighbouring parts are
  // kGroupGap apart (rim to rim). Before, the pane groups were 34, the transport 40 and the display 50.
  constexpr float kGroupH = 36, kGroupGap = 10, kRim = 3;
  const float pillH = kGroupH - 2 * kRim, pillW = 34, gap = 6, gi = 2;  // gi: between the pills of one group
  const float py = std::round(top + (barH - pillH) * 0.5f);
  float x = wp.x + kPad + 3;
  {
    struct Pane {
      const char* id;
      ui::Icon icon;
      const char* region;
      const char* name;
      const char* cmd;
    };
    // K-112b (second pass): two groups, as 125, 216 and 195 all split them: the side panes and
    // bars (library, inspector, quick help, toolbar), then the lower pane's views (editors, smart
    // controls, mixer).
    const Pane panes[] = {{"##lb_lib", ui::Icon::Plug, "logic.library", i18n::N_("Library: instruments and effects"), "view.library"},
                          {"##lb_ins", ui::Icon::Info, "logic.inspector", i18n::N_("Inspector"), "view.inspector"},
                          {"##lb_help", ui::Icon::Question, nullptr, i18n::N_("Quick help: what the item under the pointer does"), "view.quickHelp"},
                          {"##lb_tb", ui::Icon::Download, nullptr, i18n::N_("Toolbar: a strip of editing buttons under the control bar"), "view.toolbar"},
                          {"##lb_ed", ui::Icon::ToolDraw, nullptr, i18n::N_("Editors"), "view.editors"},
                          {"##lb_sm", ui::Icon::CatController, nullptr, i18n::N_("Smart controls: the selected track's main controls"), "view.smartControls"},
                          {"##lb_mx", ui::Icon::Mixer, nullptr, i18n::N_("Mixer"), "view.mixer"}};
    constexpr int kFirstGroup = 4;
    barGroup(dl, ImVec2(x, py), ImVec2(x + pillW * kFirstGroup + gi * (kFirstGroup - 1), py + pillH));
    {
      const float x2 = x + pillW * kFirstGroup + gi * (kFirstGroup - 1) + kGroupGap + 2 * kRim;
      barGroup(dl, ImVec2(x2, py), ImVec2(x2 + pillW * 3 + gi * 2, py + pillH));
    }
    gGrouped = true;
    int paneIndex = 0;
    for (const Pane& pn : panes) {
      if (paneIndex++ == kFirstGroup) x += kGroupGap + 2 * kRim - gi;  // the second group's own capsule

      bool on = false;
      // K-112b (checkpoint 24): the toolbar a tray, the editors a pencil, as 126 and 216 draw them.
      if (pn.icon == ui::Icon::Download) {  // the toolbar switch (a per-user setting, never in the project)
        on = a.settings.getBool("ui.logicToolbar");
        if (barPill(pn.id, ImVec2(x, py), ImVec2(pillW, pillH), pn.icon, nullptr, on, hintWithKey(pn.name, pn.cmd).c_str())) a.settings.set("ui.logicToolbar", !on);
        x += pillW + gi;
        continue;
      }
      if (pn.icon == ui::Icon::Question) {  // R01-08: the floating Quick help (a per-user setting)
        on = a.settings.getBool("ui.logicQuickHelp");
        if (barPill(pn.id, ImVec2(x, py), ImVec2(pillW, pillH), pn.icon, nullptr, on, hintWithKey(pn.name, pn.cmd).c_str())) a.settings.set("ui.logicQuickHelp", !on);
        x += pillW + gi;
        continue;
      }
      // The lower pane's views (editors, Smart controls, mixer): lit while the pane shows that view; a click
      // shows it (the pane switching to it, or coming back with it), or closes the pane when it is shown.
      if (pn.region) on = logicRegionShown(a, pn.region);
      else on = layoutui::smoothViewShown(a, pn.cmd);
      if (barPill(pn.id, ImVec2(x, py), ImVec2(pillW, pillH), pn.icon, nullptr, on, hintWithKey(pn.name, pn.cmd).c_str())) {
        if (pn.region) layoutui::setRegionVisible(a, pn.region, !on);
        else layoutui::smoothViewToggle(a, pn.cmd);
      }
      x += pillW + gi;
    }
    gGrouped = false;
    x += gap - gi + 3;
  }
  const float leftEnd = x;

  // ---- centre: the transport and the display ----
  // K-112b: the display has two modes, picked with the chevron at its right end: beats and
  // project (the position large, the tempo, the meter over the key) and custom (two rows per
  // field: clock time over position, the cycle range, the tempo, the meter over the grid).
  // R05-16: six modes, as the reference's: beats and project (0), time and project (2), beats and
  // time (3), beats (4) or time (5) alone across the display, and custom (1).
  const int lcdMode = std::clamp(static_cast<int>(a.settings.getNumber("ui.lcdMode")), 0, 5);
  const bool lcdCustom = lcdMode == 1, lcdAlone = lcdMode == 4 || lcdMode == 5;
  const float tb = 34, tbH = pillH, lcdH = kGroupH, modesGap = kGroupGap + kRim;  // (cp53: one height, even gaps)
  float lcdW = lcdCustom ? std::clamp(std::round(W * 0.23f), 300.0f, 440.0f) : lcdMode == 0 || lcdMode == 3 ? 330.0f : 300.0f;
  // K-112b: the control bar's parts a right-click on the bar shows or hides (ref 070).
  // K-256: the bar model (layout.logic.toolbar; the older ui.cbar.* switches were read into it once).
  bool showNav = toolbarui::shown(a, "nav");
  const bool showMaster = toolbarui::shown(a, "master");
  // K-112b (checkpoint 25): cycle sits in the transport after record, as 216, 125 and 150 have it.
  const bool showCycle = toolbarui::shown(a, "cycle");
  const bool showStart = toolbarui::shown(a, "toStart");  // A3 (checkpoint 48): hidden by default
  auto transportCount = [&] { return (showNav ? (showStart ? 6 : 5) : 3) + (showCycle ? 1 : 0); };
  // cp54 (polish): on a narrow bar every part keeps the bar's 10 px rim-to-rim gap and nothing is
  // clipped: the switches after the display (count-in, click, typing) are planned in, so they no longer
  // vanish while the display keeps its full width (1024: the switches went and a 39 px hole stood
  // before the right group; the pane groups stood 19 px from the transport).
  const float rightPillsX = wp.x + W - kPad - 3 - (pillW * 3 + gi * 2);  // the right group's first pill
  const float gxMin = leftEnd - 6 + kRim + kGroupGap;                    // the transport's first pill (leftEnd: the panes' rim + 6)
  // K-256: the switches section in the bar model's order: the count-in ("1234", 40 px), click and typing
  // pills share one capsule; the pinned commands stand in a capsule of their own.
  std::vector<std::string> switchParts;
  for (const toolbar::Part* sp : toolbar::sectionParts(layout::Preset::Logic, toolbarui::model(a).bar, "switches"))
    if (sp->shown) switchParts.push_back(sp->id);
  const std::vector<toolbar::Pin> smoothPins = toolbarui::barPins(a);
  auto smoothPinW = [&](const toolbar::Pin& pn) {
    return pn.icon ? pillW : std::ceil(a.fontRegular->CalcTextSizeA(theme::kFontCaption + 1, 1e9f, 0, toolbarui::pinLabel(pn).c_str()).x) + 24;
  };
  float pinsTotalW = 0;
  for (const toolbar::Pin& pn : smoothPins) pinsTotalW += smoothPinW(pn) + gi;
  if (pinsTotalW > 0) pinsTotalW -= gi;
  float modesW = 0;  // the switches' pills (and the pins' capsule)
  {
    bool prevPill = false, any = false;
    for (const std::string& id : switchParts) {
      if (id == toolbar::kPins) {
        if (pinsTotalW <= 0) continue;
        modesW += (any ? kGroupGap + 2 * kRim : 0.0f) + pinsTotalW;
        any = true, prevPill = false;
        continue;
      }
      modesW += (prevPill ? gi : any ? kGroupGap + 2 * kRim : 0.0f) + (id == "countIn" ? 40.0f : pillW);
      any = prevPill = true;
    }
  }
  bool switchesAlways = false;
  for (const std::string& id : switchParts) switchesAlways = switchesAlways || toolbarui::always(a, id.c_str());
  // R14-05/06: the bar's middle is one cluster (transport, display, switches) between the pane groups
  // on the left and, on the right, the master volume's pill and the right group. Each part keeps a
  // 10 px rim-to-rim gap; the cluster stands in the middle of the room it has, so its two outer gaps
  // are equal (the switches used to hang off a window-centred transport and display: 157 | 59 at 1440).
  const float leftRim = gxMin - kRim - kGroupGap;   // the pane groups' rim end
  const float rightRim = rightPillsX - kRim;        // the right group's rim start
  constexpr float kMasterW = 128, kMasterCompactW = 64;  // the master pill (its slider 20 px shorter)
  float masterW = showMaster && !s.mixer.empty() ? kMasterW : 0.0f;
  const float lcdW0 = lcdW;
  auto clusterW = [&] {
    const int n = transportCount();
    const float tW = tb * static_cast<float>(n) + gi * static_cast<float>(n - 1);
    return kRim + tW + kRim + kGroupGap + lcdW + (modesW > 0 ? kGroupGap + 2 * kRim + modesW : 0.0f);
  };
  auto clusterRoom = [&] { return rightRim - kGroupGap - (masterW > 0 ? masterW + 2 * kRim + kGroupGap : 0.0f) - (leftRim + kGroupGap); };
  {
    // Checkpoint 33 / cp54 / R14-06: on a narrow bar, first the start, back and forward buttons go, then
    // the master volume takes its compact pill, then the display narrows (down to 220 px), then the
    // master volume goes, then the switches.
    auto fit = [&] {
      lcdW = lcdW0;
      if (clusterW() > clusterRoom()) {
        lcdW = std::max(220.0f, std::floor(lcdW - (clusterW() - clusterRoom())));
      }
    };
    // K-256: a part marked "always shown" in the bar list keeps its place.
    if (clusterW() > clusterRoom() && showNav && !toolbarui::always(a, "nav")) {
      showNav = false;
    }
    if (clusterW() > clusterRoom() && masterW > 0 && !toolbarui::always(a, "master")) {
      masterW = kMasterCompactW;
    }
    fit();
    if (clusterW() > clusterRoom() && masterW > 0 && !toolbarui::always(a, "master")) {
      masterW = 0;
      fit();
    }
    if (clusterW() > clusterRoom() && !switchesAlways) {
      modesW = 0;
      fit();
    }
  }
  const int transportN = transportCount();
  const float transportW = tb * static_cast<float>(transportN) + gi * static_cast<float>(transportN - 1);
  const float spare = std::max(0.0f, clusterRoom() - clusterW());
  float gx = std::floor(leftRim + kGroupGap + spare * 0.5f) + kRim;  // the transport's first pill
  {
    const float ty = std::round(top + (barH - tbH) * 0.5f);
    const Tick bar = std::max<Tick>(1, s.barTicks());
    const Tick pos = static_cast<Tick>(a.engine.position());
    const bool songBars = a.engine.mode() != PlayMode::Pattern;
    float cx = gx;
    barGroup(dl, ImVec2(cx, ty), ImVec2(cx + transportW, ty + tbH));
    gBigGlyphs = true;  // (the transport's glyphs stay 16 px in the lower capsule)
    gGrouped = true;
    if (showNav) {
    if (showStart) {
      if (barPill("##lb_start", ImVec2(cx, ty), ImVec2(tb, tbH), ui::Icon::ChevronLeft, nullptr, false, hintWithKey("Go to the start", "transport.toStart").c_str()))
        a.engine.setPosition(0);
      cx += tb + gi;
    }
    if (barPill("##lb_back", ImVec2(cx, ty), ImVec2(tb, tbH), ui::Icon::Left, nullptr, false, hintWithKey("Back one bar", "transport.rewindBar").c_str()))
      a.engine.setPosition(std::max<Tick>(0, songBars ? MeterMap(s).barStartAt(std::max<Tick>(0, pos - 1)) : (pos - 1) / bar * bar));
    cx += tb + gi;
    if (barPill("##lb_fwd", ImVec2(cx, ty), ImVec2(tb, tbH), ui::Icon::Right, nullptr, false, hintWithKey("Forward one bar", "transport.forwardBar").c_str()))
      a.engine.setPosition(songBars ? MeterMap(s).nextBar(pos) : (pos / bar + 1) * bar);
    cx += tb + gi;
    }
    menuparity::transportTick(a);
    const bool playing = a.engine.playing();
    const std::string playTip = menuparity::playButtonHint(a);
    // K-112b (checkpoint 25): stop, then play, as 216, 125 and 150 order them.
    if (barPill("stop", ImVec2(cx, ty), ImVec2(tb, tbH), ui::Icon::Stop, nullptr, false, i18n::tr("Stop (right-click: options)")))
      menuparity::stopButtonPressed(a);
    noteRect("stop");
    if (ImGui::BeginPopupContextItem("##stopmenu")) {
      menuparity::stopMenuItems(a);
      ImGui::EndPopup();
    }
    cx += tb + gi;
    ImGui::SetCursorScreenPos(ImVec2(cx, ty));
    // Checkpoint 31: a white triangle at rest that turns green while playing, as 070, 216 and 150 draw it
    // (it was green at rest and a filled orange pause while playing); a click still pauses.
    if (barPill("play", ImVec2(cx, ty), ImVec2(tb, tbH), ui::Icon::Play, nullptr, false,
                playTip.empty() ? hintWithKey("Play / pause", "transport.play").c_str() : playTip.c_str(), playing ? theme::rgb(0x34C759) : 0, false))
      playing ? a.engine.pause() : (void)menuparity::playPressed(a);
    // A2 (checkpoint 48): a bare green triangle while playing, no disc, as the reference's.
    noteRect("play");
    cx += tb + gi;
    if (barPill("rec", ImVec2(cx, ty), ImVec2(tb, tbH), ui::Icon::Record, nullptr, false,
                hintWithKey("Record (right-click: what to record, punch, count-in)", "transport.record").c_str(), a.recordArmed ? P.arm : theme::withAlpha(P.arm, 0.8f))) {
      // R14-01: as the R key and the other layouts' buttons: stopped, it records and plays at once (and
      // opens the Recording card when record.window says so); while playing it switches recording, which
      // with quick punch punches out and in (the card stays as it is).
      if (!a.recordArmed && !a.engine.playing()) {
        a.recordArmed = true;
        recordcard::recordClicked(a, ImGui::GetIO().KeyShift);
        (void)menuparity::playPressed(a);
      } else {
        livelook::toggleArrangementRecord(a);
        recordcard::recordClicked(a, ImGui::GetIO().KeyShift);
      }
    }
    if (a.recordArmed) dl->AddRect(ImVec2(cx, ty), ImVec2(cx + tb, ty + tbH), P.arm, std::floor(tbH * 0.5f), 0, 1.5f);
    noteRect("rec");
    if (ImGui::BeginPopupContextItem("##recmenu")) {
      recordOptionsMenu(a);
      ImGui::EndPopup();
    }
    if (showCycle) {
      cx += tb + gi;
      const bool cyc = a.playlist.selEnd > a.playlist.selStart;
      if (barPill("##lb_cycle", ImVec2(cx, ty), ImVec2(tb, tbH), ui::Icon::LoopArrows, nullptr, cyc,
                  hintWithKey("Cycle: playback loops inside the time selection (off: the whole song)", nullptr).c_str()))
        toggleCycle(a);
    }
    gGrouped = false;
    gBigGlyphs = false;
    cx += tb + kRim + kGroupGap;
    // The display: position, tempo, metre and key.
    const ImVec2 l0(cx, std::round(top + (barH - lcdH) * 0.5f)), l1(cx + lcdW, l0.y + lcdH);
    // K-112b: a dark display in three sections: the position large and bold (bar and beat, each
    // captioned), the tempo, then the meter over the key.
    // Checkpoint 52 (polish): a cool slate rather than near black, as 133, 146 and 101 show the display.
    // R14-07: a capsule as the groups are (its corners were 8 px beside their fully round 18).
    const float lcdR = std::floor(lcdH * 0.5f);
    dl->AddRectFilled(l0, l1, theme::mix(theme::mix(P.field, P.ink, 0.45f), P.accent, 0.07f), lcdR);
    // A5 (checkpoint 49): recessed, as the reference's: a soft inner shadow along the top, no stroke
    // (R14-07: it follows the capsule's curve).
    dl->PushClipRect(l0, ImVec2(l1.x, l0.y + 5), true);
    dl->AddRect(ImVec2(l0.x + 1.5f, l0.y + 1.5f), ImVec2(l1.x - 1.5f, l1.y - 1.5f), theme::withAlpha(P.ink, 0.26f), lcdR - 1.5f, 0, 1.5f);
    dl->PopClipRect();
    // Checkpoint 30: no outline (150, 205, 001: the display is a dark field on the bar, no stroke).
    noteRectAt("bar_display", l0, l1);
    const float chevW = 18;  // the mode chevron at the right end
    const float posW = lcdAlone ? lcdW - chevW : std::round(lcdW * (lcdCustom ? 0.27f : 0.48f)), locW = lcdCustom ? std::round(lcdW * 0.25f) : 0.0f;
    const float tempoW = std::round(lcdW * (lcdCustom ? 0.22f : 0.28f));
    const float third = posW + locW;  // where the tempo section starts
    for (const float sx : {lcdAlone ? -1.0f : posW, locW > 0 ? third : -1.0f, lcdAlone ? -1.0f : third + tempoW, lcdW - chevW})
      if (sx > 0) dl->AddLine(ImVec2(l0.x + sx, l0.y + 8), ImVec2(l0.x + sx, l1.y - 8), theme::withAlpha(P.line, 0.6f));  // A5: quieter
    // Two rows of a field (custom mode), right-aligned, each in its own colour.
    const float rowF = theme::kFontBody;
    auto bars = [&](double tick) {  // bars : beats : ticks without the bar's leading zeros ("1:01:000")
      std::string t = ops::formatSongTime(s, tick, ops::TimeFormat::BarBeatTick, a.settings.getBool("ui.timeSteps"));
      size_t k = 0;
      while (k + 1 < t.size() && t[k] == '0' && t[k + 1] != ':') ++k;
      return t.substr(k);
    };
    auto rows = [&](float x0, float w, const std::string& top, const std::string& bottom, ImU32 cTop, ImU32 cBottom) {
      const float gapY = 2, y0 = std::round(l0.y + (lcdH - 2 * rowF - gapY) * 0.5f);
      for (int r = 0; r < 2; ++r) {
        const std::string& t = r == 0 ? top : bottom;
        const float tw = a.fontMono->CalcTextSizeA(rowF, 1e9f, 0, t.c_str()).x;
        dl->AddText(a.fontMono, rowF, ImVec2(std::round(x0 + w - 10 - tw), y0 + r * (rowF + gapY)), r == 0 ? cTop : cBottom, t.c_str());
      }
    };
    const std::string posText = menuparity::positionText(a, a.engine.position());
    // Small captions under the values, centred on a span.
    // Checkpoint 30: in capitals, as 001 and 216 caption the display (BAR, BEAT, TEMPO).
    auto caption = [&](float cx0, const char* text) {
      const float cf = theme::kFontLabel - 1;
      std::string t = i18n::tr(text);
      for (char& ch : t)
        if (static_cast<unsigned char>(ch) < 128) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      const float w = a.fontRegular->CalcTextSizeA(cf, 1e9f, 0, t.c_str()).x;
      dl->AddText(a.fontRegular, cf, ImVec2(std::round(cx0 - w * 0.5f), std::round(l1.y - cf - 4)), P.textFaint, t.c_str());
    };
    {
      ImGui::SetCursorScreenPos(l0);
      ImGui::InvisibleButton("##lcdpos", ImVec2(posW, lcdH));
      noteRectAt("lcd_position", l0, ImVec2(l0.x + posW, l1.y));
      // R05-16: a double-click types a position (bar beat division tick, or h:mm:ss in the time modes).
      static char posBuf[96] = "";  // (room for the four numbers: GCC -Wformat-truncation)
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const MeterMap mm(s);
        const MeterMap::Bbt b = mm.toBbt(static_cast<Tick>(a.engine.position()));
        const Tick dv = std::max<Tick>(1, s.ppq / 4);
        std::snprintf(posBuf, sizeof posBuf, "%lld %d %lld %lld", static_cast<long long>(b.bar + 1), b.beat + 1,
                      static_cast<long long>(b.tick / dv + 1), static_cast<long long>(b.tick % dv + 1));
        ImGui::OpenPopup("##lcdposedit");
      }
      if (lcdCustom && ImGui::BeginPopupContextItem("##posmenu")) {
        menuparity::timeFormatMenu(a);
        ImGui::EndPopup();
      }
      ui::Hint(lcdCustom ? "Song position (double-click: type one; right-click: bars and beats, minutes and seconds, or timecode)"
                         : "Song position (double-click: type one, as bar beat division tick or h:mm:ss)");
      ImGui::SetNextWindowPos(ImVec2(l0.x, l1.y + 4));
      if (ImGui::BeginPopup("##lcdposedit")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(std::max(140.0f, posW));
        if (ImGui::InputText("##posin", posBuf, sizeof posBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
          if (const auto t = parseLcdPosition(s, posBuf)) a.engine.setPosition(*t);
          ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      // K-112b (checkpoint 25): larger position digits, as 216, 125 and 205 set them (about 0.6 of the display).
      // cp53 (polish): the display is the bar's height (36); its values share one centre line (lcdMid)
      // and its captions one row under them.
      const float fs = std::round(lcdH * 0.56f);
      // Checkpoint 28: in the regular weight, as 216, 205 and 150 set the position (it was the bold face).
      ImFont* pf = a.fontRegular;
      const float cy = std::round(l0.y + kLcdMid - fs * 0.55f);
      const bool barsBeats = static_cast<int>(a.settings.getNumber("ui.timeFormat")) == 0;
      const size_t c1 = posText.find(':'), c2 = c1 == std::string::npos ? c1 : posText.find(':', c1 + 1);
      const double at = a.engine.position();
      if (lcdCustom) {  // clock time over the position in bars
        rows(l0.x, posW, lcdClock(s, at), bars(at),  // R07-12: the clock as the other modes write it
             P.textDim, P.text);
      } else if (lcdMode == 3) {  // R05-16: beats and time: the position in bars over the clock time
        const Lbbdt q = lcdBbdt(s, at);
        char top[48];
        std::snprintf(top, sizeof top, "%lld %d %d %d", static_cast<long long>(q.bar), q.beat, q.div, q.tick);
        rows(l0.x, posW, top, lcdClock(s, at), P.text, P.textDim);
      } else if (lcdMode == 2 || lcdMode == 5) {  // R05-16: the clock time, large
        const std::string clk = lcdClock(s, at);
        const float f0 = lcdMode == 5 ? fs : std::round(fs * 0.8f);
        const float tw0 = pf->CalcTextSizeA(f0, 1e9f, 0, clk.c_str()).x;
        const float f = tw0 > posW - 16 ? f0 * (posW - 16) / tw0 : f0;
        const float w = pf->CalcTextSizeA(f, 1e9f, 0, clk.c_str()).x;
        dl->AddText(pf, f, ImVec2(std::round(l0.x + (posW - w) * 0.5f), std::round(l0.y + (lcdH - f) * 0.5f - lcdH * 0.08f)), P.text, clk.c_str());
        caption(l0.x + posW * 0.5f, i18n::N_("Time"));
      } else if (lcdMode == 4 || lcdMode == 0) {
        // R05-16: bar, beat, division and tick, each captioned (ref 300). In beats and project the
        // bar and beat stay large (ref 289) and the division and tick follow smaller.
        // A4 (checkpoint 49): beats and project shows the bar and the beat only, large, as the
        // reference's default display; division and tick are for Beats (4) and Custom.
        const bool sub = lcdMode == 4;
        const Lbbdt q = lcdBbdt(s, at);
        const std::string f1 = std::to_string(q.bar), f2 = std::to_string(q.beat), f3 = std::to_string(q.div), f4 = std::to_string(q.tick);
        const float big = fs, small = lcdMode == 4 ? fs : std::round(fs * 0.55f);
        const ImU32 dim = theme::mix(P.textFaint, theme::mix(P.field, P.ink, 0.55f), 0.6f);
        // Fixed fields so the digits do not jump: bar 3 digits (dim leading zeros), beat and division 1, tick 3.
        const float dig = pf->CalcTextSizeA(big, 1e9f, 0, "0").x, sdig = pf->CalcTextSizeA(small, 1e9f, 0, "0").x;
        const float gapBig = std::round(dig * 0.45f), gapSmall = std::round(sdig * 0.6f);
        const float wBar = dig * 3, wBeat = dig * std::max<size_t>(1, f2.size()), wDiv = sdig * std::max<size_t>(1, f3.size()), wTick = sdig * 3;
        const float total = wBar + gapBig + wBeat + (sub ? gapSmall * 2 + wDiv + wTick : 0.0f);
        float x = std::round(l0.x + (posW - total) * 0.5f);
        const float yBig = cy, ySmall = std::round(cy + big - small - big * 0.06f);
        auto field = [&](const std::string& t, float w, float size, float y, bool pad) {
          const float tw = pf->CalcTextSizeA(size, 1e9f, 0, t.c_str()).x;
          if (pad && t.size() < 3) {
            const std::string z(3 - t.size(), '0');
            dl->AddText(pf, size, ImVec2(x + w - tw - pf->CalcTextSizeA(size, 1e9f, 0, z.c_str()).x, y), dim, z.c_str());
          }
          dl->AddText(pf, size, ImVec2(x + w - tw, y), P.text, t.c_str());
          const float c = x + w * 0.5f;
          x += w;
          return c;
        };
        const float cBar = field(f1, wBar, big, yBig, true);
        x += gapBig;
        const float cBeat = field(f2, wBeat, big, yBig, false);
        caption(cBar, i18n::N_("Bar"));
        caption(cBeat, i18n::N_("Beat"));
        if (sub) {
          x += gapSmall * 2;
          const float cDiv = field(f3, wDiv, small, ySmall, false);
          x += gapSmall;
          const float cTick = field(f4, wTick, small, ySmall, false);
          caption(cDiv, i18n::N_("Div"));
          caption(cTick, i18n::N_("Tick"));
        }
      } else if (barsBeats && c1 != std::string::npos && c2 != std::string::npos) {  // "19.2": bar, beat
        const std::string barS = std::to_string(std::atoi(posText.substr(0, c1).c_str()));
        const std::string beatS = std::to_string(std::atoi(posText.substr(c1 + 1, c2 - c1 - 1).c_str()));
        const float bw = pf->CalcTextSizeA(fs, 1e9f, 0, barS.c_str()).x;
        const float dw = pf->CalcTextSizeA(fs, 1e9f, 0, ".").x;
        const float ew = pf->CalcTextSizeA(fs, 1e9f, 0, beatS.c_str()).x;
        const float dotX = std::round(l0.x + posW * 0.62f);  // the dot stays put as the bar grows
        // Checkpoint 30: the bar padded to three digits with dim zeros (001, 150, 205 show "001. 1"), so
        // the number sits in a field of fixed width instead of floating in the section.
        if (barS.size() < 3) {
          const std::string pad(3 - barS.size(), '0');
          const float zw = pf->CalcTextSizeA(fs, 1e9f, 0, pad.c_str()).x;
          dl->AddText(pf, fs, ImVec2(dotX - bw - zw, cy), theme::mix(P.textFaint, theme::mix(P.field, P.ink, 0.55f), 0.6f), pad.c_str());
        }
        dl->AddText(pf, fs, ImVec2(dotX - bw, cy), P.text, barS.c_str());
        dl->AddText(pf, fs, ImVec2(dotX, cy), P.text, ".");
        dl->AddText(pf, fs, ImVec2(dotX + dw, cy), P.text, beatS.c_str());
        // Each caption under its number, kept apart by at least a space (single digits are narrow).
        const float cf = theme::kFontLabel - 1;
        const float barW = a.fontRegular->CalcTextSizeA(cf, 1e9f, 0, i18n::tr("Bar")).x, beatW = a.fontRegular->CalcTextSizeA(cf, 1e9f, 0, i18n::tr("Beat")).x;
        float c1 = dotX - bw * 0.5f, c2 = dotX + dw + ew * 0.5f;
        const float need = (barW + beatW) * 0.5f + 8, have = c2 - c1;
        if (have < need) c1 -= (need - have) * 0.5f, c2 += (need - have) * 0.5f;
        c1 = std::min(c1, dotX - pf->CalcTextSizeA(fs, 1e9f, 0, "000").x * 0.5f);  // under the padded field
        caption(c1, i18n::N_("Bar"));
        caption(c2, i18n::N_("Beat"));
      } else {
        const float f0 = std::round(fs * 0.7f);
        const ImVec2 ts = pf->CalcTextSizeA(f0, 1e9f, 0, posText.c_str());
        const float f = ts.x > posW - 16 ? f0 * (posW - 16) / ts.x : f0;
        const float w = pf->CalcTextSizeA(f, 1e9f, 0, posText.c_str()).x;
        dl->AddText(pf, f, ImVec2(std::round(l0.x + (posW - w) * 0.5f), std::round(l0.y + (lcdH - f) * 0.5f - lcdH * 0.08f)), P.text,
                    posText.c_str());
        caption(l0.x + posW * 0.5f, i18n::N_("Position"));
      }
    }
    if (lcdCustom) {  // the locators: the left over the right (checkpoint 47: also with Cycle off, dimmer)
      Tick ll = 0, lr = 0;
      const bool has = locators(a, ll, lr), on = a.playlist.selEnd > a.playlist.selStart;
      ImGui::SetCursorScreenPos(ImVec2(l0.x + posW, l0.y));
      ImGui::InvisibleButton("##lcdloc", ImVec2(locW, lcdH));
      ui::Hint("Locators: the left over the right (click one to go there)");
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && has) gotoLocator(a, ImGui::GetIO().MousePos.y > l0.y + lcdH * 0.5f);
      noteRectAt("lcd_locators", ImVec2(l0.x + posW, l0.y), ImVec2(l0.x + third, l1.y));
      const ImU32 lc = on ? P.text : has ? P.textDim : P.textFaint;
      rows(l0.x + posW, locW, bars(static_cast<double>(ll)), bars(static_cast<double>(has ? lr : ll)), lc, lc);
    }
    if (!lcdAlone) caption(l0.x + third + tempoW * 0.5f, i18n::N_("Tempo"));
    if (!lcdAlone) {
      static TempoMap map;
      static uint64_t mapRev = ~0ull;
      static const Song* mapSong = nullptr;
      if (mapRev != a.doc.revision() || mapSong != &s) {
        map.build(s);
        mapRev = a.doc.revision();
        mapSong = &s;
      }
      const bool automated = !map.constant() && a.engine.mode() == PlayMode::Song;
      double tempo = automated ? map.bpmAt(a.engine.position()) : s.tempo;
      const double shown = tempo;
      const ImVec2 tp(std::round(l0.x + third + 2), l0.y);
      ImGui::SetCursorScreenPos(tp);
      if (tempoLcd(a, &tempo, tempoW - 4, lcdH, true) && !(automated && tempo == shown)) a.doc.setParam(songParam(SongParam::Tempo), tempo, true);
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    }
    if (!lcdAlone) {
      char sig[16];
      int sigNum = s.timeSigNum, sigDen = s.timeSigDen;
      const bool songTime = a.engine.mode() != PlayMode::Pattern;
      const Tick at = static_cast<Tick>(a.engine.position());
      if (songTime) meter::meterAt(s, at, sigNum, sigDen);
      else if (const Pattern* cp = s.pattern(a.curPattern)) sigNum = s.patternMeterNum(*cp), sigDen = s.patternMeterDen(*cp);
      std::snprintf(sig, sizeof sig, "%d/%d", sigNum, sigDen);
      const Marker* key = nullptr;
      for (const Marker& m : s.markers)
        if (m.isKey() && m.pos <= at && (!key || m.pos >= key->pos)) key = &m;
      const float x0 = l0.x + third + tempoW, w = l1.x - chevW - x0;
      ImGui::SetCursorScreenPos(ImVec2(x0, l0.y));
      if (ImGui::InvisibleButton("##sig", ImVec2(w, lcdH))) meterui::openEditor(a, songTime ? meter::changeAt(s, at) : -1);
      ui::Hint(songTime ? "Meter at the song position (click to edit)" : "Meter of the pattern (click: the song's meter at the start)");
      const float mf = theme::kFontTitle;  // a size the app already uses
      const float sw = a.fontBold->CalcTextSizeA(mf, 1e9f, 0, sig).x;
      std::string grid = a.playlist.effGrid > 0 ? timeline::gridName(s, a.playlist.effGrid) : std::string();
      if (lcdCustom && a.playlist.effGrid > 0 && (4 * static_cast<Tick>(s.ppq)) % a.playlist.effGrid == 0)  // R07-12: the division, "/16"
        grid = "/" + std::to_string((4 * static_cast<Tick>(s.ppq)) / a.playlist.effGrid);
      // Checkpoint 35: the key in short form under the meter ("Cmaj", "Bmin"), from the key marker's
      // root and scale, in the text colour a step under the meter's size, as 001, 216 and 158 set it
      // (it was the marker's own name, dim and small).
      auto shortKey = [](const Marker& m) {
        const auto& sc = tools::scales();
        const std::string sn = m.keyScale >= 0 && static_cast<size_t>(m.keyScale) < sc.size() ? sc[static_cast<size_t>(m.keyScale)].name : std::string();
        std::string mode = sn.find("minor") != std::string::npos ? "min" : sn == "Major" || sn == "Chromatic" || sn.empty() ? "maj" : sn.substr(0, 3);
        for (char& ch : mode) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return chord::noteName(m.keyRoot) + mode;
      };
      // R01-26: with no key marker the project's key is C major, shown as the reference always shows one.
      const std::string under = lcdCustom ? grid : (key ? shortKey(*key) : std::string("Cmaj"));
      const bool hasKey = !under.empty();
      // cp53 (polish): the meter on the values' line, the key (custom: the grid) on the captions' row,
      // a step brighter than a caption, so the display reads as two even rows.
      const float uf = theme::kFontLabel;
      const float my = std::round(l0.y + kLcdMid - mf * 0.55f);
      dl->AddText(a.fontBold, mf, ImVec2(std::round(x0 + (w - sw) * 0.5f), my), P.text, sig);
      if (hasKey) {
        const float kw = std::min(w - 8, a.fontRegular->CalcTextSizeA(uf, 1e9f, 0, under.c_str()).x);
        theme::textEllipsis(dl, a.fontRegular, uf, ImVec2(std::round(x0 + (w - kw) * 0.5f), std::round(l1.y - uf - 4)), w - 8, P.textDim, under.c_str());
      }
    }
    {  // the mode chevron
      const ImVec2 c0(l1.x - chevW, l0.y);
      ImGui::SetCursorScreenPos(c0);
      const bool click = ImGui::InvisibleButton("##lcdmode", ImVec2(chevW, lcdH));
      const bool hov = ImGui::IsItemHovered();
      ui::Hint("Display mode");
      ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(std::round(c0.x + chevW * 0.5f) - 1.0f, std::round(l0.y + lcdH * 0.5f)), 9, hov ? P.text : P.textDim);
      noteRectAt("lcd_mode", c0, ImVec2(l1.x, l1.y));
      if (click) ImGui::OpenPopup("##lcdmodes");
      // cp54 (polish): the menu hangs from the display's right edge, its right side flush with it (it began at
      // the chevron and stood about 140 px past the display, over the switches).
      ImGui::SetNextWindowPos(ImVec2(l1.x, l1.y + 4), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
      if (ImGui::BeginPopup("##lcdmodes")) {
        lcdModeItems(a);
        ImGui::EndPopup();
      }
    }
    gx = l1.x;
  }

  // ---- after the display: count-in, click, typing and the pinned commands (K-256: in the model's order) ----
  float mx = gx + modesGap;
  {
    // R01-24: where the count-in's beats do not fit, it narrows to a glyph pill before it goes.
    // cp54: as many as the plan above kept, their pills ending 10 px (rim to rim) before the right group.
    const float modeLimit = modesW > 0 ? mx + modesW + 0.5f : mx;  // R14-05: as many as the plan above kept
    struct Slot {
      std::string id;
      float x, w;
      int pin;  // a pinned command's index, -1 for a switch
    };
    std::vector<Slot> slots;
    std::vector<std::pair<float, float>> groups;  // each capsule's x range
    {
      float x = mx;
      bool prevPill = false, any = false, full = false;
      for (const std::string& id : switchParts) {
        if (full) break;
        if (id == toolbar::kPins) {
          if (smoothPins.empty()) continue;
          float px = x + (any ? kGroupGap + 2 * kRim : 0.0f);
          const float g0 = px;
          for (size_t k = 0; k < smoothPins.size(); ++k) {
            const float w = smoothPinW(smoothPins[k]);
            if (px + w > modeLimit) {
              full = true;
              break;
            }
            slots.push_back({id, px, w, static_cast<int>(k)});
            px += w + gi;
          }
          if (px > g0) groups.push_back({g0, px - gi}), x = px - gi, any = true, prevPill = false;
          continue;
        }
        const float at = x + (prevPill ? gi : any ? kGroupGap + 2 * kRim : 0.0f);
        const float w = id == "countIn" ? (at + 40.0f > modeLimit && at + pillW <= modeLimit ? pillW : 40.0f) : pillW;
        if (at + w > modeLimit) break;  // the right group stays
        if (!prevPill) groups.push_back({at, at + w});
        else groups.back().second = at + w;
        slots.push_back({id, at, w, -1});
        x = at + w;
        any = prevPill = true;
      }
    }
    for (const auto& g : groups) barGroup(dl, ImVec2(g.first, py), ImVec2(g.second, py + pillH));
    gGrouped = true;
    for (const Slot& sl : slots) {
      const float x = sl.x, w = sl.w;
      if (sl.pin >= 0) {  // K-256: a pinned command, as a pill with its name or its glyph
        const toolbar::Pin& pn = smoothPins[static_cast<size_t>(sl.pin)];
        const std::string label = toolbarui::pinLabel(pn);
        ImGui::PushID(sl.pin);
        if (barPill("##lb_pin", ImVec2(x, py), ImVec2(w, pillH), toolbarui::pinIcon(pn), pn.icon ? nullptr : label.c_str(), false, label.c_str()))
          toolbarui::runPin(a, pn);
        if (ImGui::BeginPopupContextItem("##lbpinmenu")) {
          toolbarui::pinMenu(a, pn);
          ImGui::EndPopup();
        }
        ImGui::PopID();
        continue;
      }
      if (sl.id == "countIn") {
        if (barPill("##lb_count", ImVec2(x, py), ImVec2(w, pillH), ui::Icon::Count, w > pillW ? "1234" : nullptr, a.countInBars > 0, i18n::tr("Count-in before recording")))
          a.countInBars = a.countInBars > 0 ? 0 : std::max(1, a.countInLength);
        if (a.engine.countingIn()) {
          // cp54 (recording UI): while the count-in runs, its pill pulses once a half second (nothing in
          // Smooth's bar showed it: the record button was armed and the display stood still), on the
          // pill's own rounded edge, as the Studio bar pulses its play button.
          const float ph = static_cast<float>(std::fmod(a.now, 0.5) / 0.5), grow = std::round(ph * 3.0f);
          dl->AddRect(ImVec2(x - grow, py - grow), ImVec2(x + w + grow, py + pillH + grow), theme::withAlpha(P.hot, 1.0f - ph),
                      std::floor(pillH * 0.5f) + grow, 0, 1.5f);
        }
      } else if (sl.id == "click") {
        if (barPill("##lb_click", ImVec2(x, py), ImVec2(w, pillH), ui::Icon::Metronome, nullptr, a.metronome, i18n::tr("Metronome"))) a.metronome = !a.metronome;
        noteRect("metro");
      } else if (sl.id == "typing") {
        // The typing keyboard: the computer keys play notes (its options on a right-click).
        if (barPill("##lb_typing", ImVec2(x, py), ImVec2(pillW, pillH), ui::Icon::Keyboard, nullptr, a.typingKeyboard,
                    hintWithKey("Typing keyboard to piano (right-click: options)", "options.typingKeyboard").c_str()))
          a.typingKeyboard = !a.typingKeyboard;
        noteRect("typing");
        if (ImGui::BeginPopupContextItem("##lbtypingmenu")) {
          typingKeyboardMenu(a);
          ImGui::EndPopup();
        }
      }
      mx = x + w + gi;
    }
    gGrouped = false;
    mx += gap - gi;
  }

  // ---- right: the right pane's panels, the save state ----
  {
    struct Side {
      const char* id;
      ui::Icon icon;
      const char* window;
      const char* name;
      const char* cmd;
    };
    // K-112b (checkpoint 24): lists, notes, then files, the order of 126, 216 and 125.
    const Side sides[] = {{"##lb_lists", ui::Icon::ListBullets, "Lists", i18n::N_("Lists"), "view.lists"},
                          {"##lb_notes", ui::Icon::File, "Notes", i18n::N_("Notes"), "view.notes"},
                          {"##lb_browser", ui::Icon::Browser, "Browser", i18n::N_("Browser"), "view.browser"}};
    float rx = wp.x + W - kPad - 3 - (pillW * 3 + gi * 2);
    float right0 = rx;
    barGroup(dl, ImVec2(rx, py), ImVec2(rx + pillW * 3 + gi * 2, py + pillH));
    gGrouped = true;
    for (const Side& sd : sides) {
      // Lit while the right pane shows it; a click shows it there (switching the open pane to it), or
      // closes the pane when it is what the pane shows.
      const bool on = layoutui::smoothViewShown(a, sd.cmd);
      if (barPill(sd.id, ImVec2(rx, py), ImVec2(pillW, pillH), sd.icon, nullptr, on, hintWithKey(sd.name, sd.cmd).c_str()))
        layoutui::smoothViewToggle(a, sd.cmd);
      rx += pillW + gi;
    }
    gGrouped = false;
    // The master volume: a slider over the master's level meter, left of the right group.
    // R14-06: in a pill of the bar's height, as every other part (it was a bare 15 px slider), 10 px
    // (rim to rim) before the right group; on a narrow bar it takes a compact pill before it goes.
    if (masterW > 0) {
      const ParamId pid = mixerParam(s.masterId(), MixerParam::Volume);
      const float px0 = rightRim - kGroupGap - kRim - masterW;  // the pill's face (its rim 3 px outside)
      barGroup(dl, ImVec2(px0, py), ImVec2(px0 + masterW, py + pillH));
      const float vw = masterW - 20, vx = px0 + 10, cy = std::round(py + pillH * 0.5f);
      // A6 (checkpoint 49): the slider's grey track, its meter in it, and a light knob.
      ImGui::SetCursorScreenPos(ImVec2(px0, py));
      ImGui::InvisibleButton("##lb_mvol", ImVec2(masterW, pillH));
      const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
      if (const auto d = describeParam(s, pid)) {
        const auto v = getParam(s, pid);
        double n = v ? d->toNormalized(*v) : 0.8;
        ImGuiIO& io = ImGui::GetIO();
        if (act && io.MouseDelta.x != 0) {
          n = std::clamp(n + io.MouseDelta.x / (vw - 10) * (io.KeyShift ? 0.15 : 1.0), 0.0, 1.0);
          a.doc.setParam(pid, d->fromNormalized(n), true);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && (!v || *v != d->def)) a.doc.setParam(pid, d->def, false);  // (R13-08)
        if (ImGui::IsItemDeactivated()) a.doc.endGesture();
        const float tx = std::round(vx + 5 + (vw - 10) * static_cast<float>(n));
        // Checkpoint 36: the meter is the slider's track: two bars (green, yellow over -6 dB, a red tick
        // once clipped) with the knob over them.
        const float bh = 4.0f;
        // flick-1119: the bars follow the shared meter ballistics (LevelMeter: instant rise, a timed
        // fall), as Smooth's strip meters and the other layouts' output meters do. They drew the raw
        // peak taken since the last frame, so every frame no audio buffer finished in (a large buffer,
        // a 120 Hz display) drew them empty: the pill flashed on and off while the song played.
        const mixmeters::Reading r = mixmeters::get(a, s.masterId());
        static LevelMeter meter;
        meter.update(r.peakL, r.peakR, ImGui::GetIO().DeltaTime);
        static float clipHold[2] = {0, 0};
        const float hot = LevelMeter::toPos(-6.0f);  // where yellow starts
        for (int k = 0; k < 2; ++k) {
          const float pk = k == 0 ? r.peakL : r.peakR;
          const float f = LevelMeter::toPos(meter.levelDb(k));
          const float my = k == 0 ? cy - 1.0f - bh : cy + 1.0f;
          dl->AddRectFilled(ImVec2(vx, my), ImVec2(vx + vw, my + bh), theme::mix(P.surface700, P.ink, 0.35f), 2.0f);
          if (f > 0) {
            dl->AddRectFilled(ImVec2(vx, my), ImVec2(vx + vw * std::min(f, hot), my + bh), theme::rgb(theme::role::kPlay), 2.0f);
            if (f > hot) {
              dl->AddRectFilled(ImVec2(vx + vw * hot, my), ImVec2(vx + vw * f, my + bh), theme::rgb(0xE8C33A), 2.0f);
            }
          }
          if (pk >= 1.0f) clipHold[k] = 1.0f;
          if (clipHold[k] > 0) dl->AddRectFilled(ImVec2(vx + vw + 2, my), ImVec2(vx + vw + 4, my + bh), P.danger);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) clipHold[0] = clipHold[1] = 0;
        const float kr = 7.0f;
        dl->AddCircleFilled(ImVec2(tx, cy + 0.5f), kr + 0.5f, theme::withAlpha(P.ink, 0.35f), 20);  // a soft shadow
        dl->AddCircleFilled(ImVec2(tx, cy), kr, hov || act ? theme::mix(P.text, P.surface700, 0.05f) : theme::mix(P.text, P.surface700, 0.16f), 20);
        if (hov || act) {
          const std::string tip = std::string(i18n::tr("Master volume")) + ": " + d->format(v.value_or(d->def));
          ui::Hint(tip.c_str());
        }
      }
      noteRectAt("logic_mvol", ImVec2(px0 - kRim, py - kRim), ImVec2(px0 + masterW + kRim, py + pillH + kRim));
      right0 = px0 - kRim - 12;
    }
    // Without the menu row (the system's menu bar holds the menus): the project and its save state here.
    std::string proj = a.projectFile().empty() ? i18n::tr("Untitled project") : fs::path(a.projectFile()).stem().string();
    if (std::string shared = collabui::projectTitle(a); !shared.empty()) proj = shared;
    const float room = right0 - 12 - (mx + 8);
    if (!menusHere && room > 60) {
      const float tw = std::min(a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, proj.c_str()).x, std::min(room - 14, 220.0f));
      const float tx = right0 - 12 - tw;
      const float ty = std::round(top + (barH - theme::kFontCaption) * 0.5f) - 1;
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(tx, ty), tw, P.textDim, proj.c_str());
      if (a.doc.modified()) dl->AddCircleFilled(ImVec2(tx - 8, ty + theme::kFontCaption * 0.5f + 1), 3.0f, P.text, 12);
    }
  }
  // K-112b / K-256: customise the control bar: a right-click on its empty space opens the bar list (its
  // parts on and off, their order, the pinned commands).
  barListOnEmpty(a, top, wp.y + h);
  ImGui::End();
  return h;
}

// ---------------------------------------------------------- status bar ----

static void drawNotifications(App& a, ImVec2 wp, float W, float fs);

void drawStatusBar(App& a) {
  if (livelook::active(a)) {  // K-112c: the view, the selection and the band toggles
    livelook::drawStatusBar(a);
    const ImGuiViewport* lvp = ImGui::GetMainViewport();
    drawNotifications(a, ImVec2(lvp->WorkPos.x, lvp->WorkPos.y + lvp->WorkSize.y - theme::kStatusBarH), lvp->WorkSize.x, theme::kFontCaption);
    return;
  }
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  // K-106 cp16: the Tracker has no app status bar and no cards for plain messages: they go to the
  // line the page in front shows them in (the pattern bar, a page's title), as the classic tracker
  // says things in its own text line. Errors keep their cards (a click copies their details).
  if (layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker)) {
    for (auto it = a.toasts.begin(); it != a.toasts.end();) {
      if (it->error) {
        ++it;
        continue;
      }
      trackerview::status(it->text);
      it = a.toasts.erase(it);
    }
  }
  if (layoutui::statusBarHeight() <= 0) {  // K-112: a layout without a status bar (Logic): only the cards
    drawNotifications(a, ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y), vp->WorkSize.x, theme::kFontCaption);
    return;
  }
  const float h = theme::kStatusBarH;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.chrome));
  // K-79: the live layout puts its bottom panel toggles in the status bar's corner.
  const bool corner = layoutui::active() == layout::Preset::Live;
  ImGui::Begin("##status", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | (corner ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_NoInputs));
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();
  const float fs = theme::kFontCaption;
  const float ty = std::round(wp.y + (h - fs) * 0.5f) - 1;
  dl->AddLine(wp, ImVec2(wp.x + W, wp.y), P.line);

  float rx = wp.x + W - theme::kS3;
  if (corner) {  // Editor, channel settings, Mixer (lit while shown)
    struct Tg {
      const char* id;
      const char* label;
      bool on;
      int what;
    };
    const Tg tg[] = {{"##sb_mixer", i18n::N_("Mixer"), a.showMixer, 2}, {"##sb_chan", i18n::N_("Channel"), a.settingsChannel != kNoId, 1},
                     {"##sb_editor", i18n::N_("Editor"), a.showPiano, 0}};
    for (const Tg& t : tg) {
      const char* text = i18n::tr(t.label);
      const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, text).x + 14;
      rx -= tw;
      const ImVec2 bp(rx, wp.y + 3);
      ImGui::SetCursorScreenPos(bp);
      const bool click = ImGui::InvisibleButton(t.id, ImVec2(tw, h - 6));
      const bool hov = ImGui::IsItemHovered();
      dl->AddRectFilled(bp, ImVec2(bp.x + tw, bp.y + h - 6), t.on ? theme::withAlpha(P.hot, 0.9f) : hov ? P.surface800 : P.surface900, theme::kRadiusSm);
      dl->AddText(a.fontRegular, fs, ImVec2(bp.x + 7, ty), t.on ? theme::contrastText(P.hot) : P.textDim, text);
      if (click) {
        if (t.what == 0) a.showPiano = !a.showPiano;
        if (t.what == 1) a.settingsChannel = a.settingsChannel != kNoId ? kNoId : a.selChannel;
        if (t.what == 2) a.showMixer = !a.showMixer;
      }
      rx -= theme::kS1;
    }
    rx -= theme::kS3;
  }
  // Right: audio format · project name · unsaved marker.
  std::string proj = a.projectFile().empty() ? "Untitled project" : fs::path(a.projectFile()).filename().string();
  if (std::string shared = collabui::projectTitle(a); !shared.empty()) proj = shared;  // J-111f
  char audio[48];
  std::snprintf(audio, sizeof audio, "%.1f kHz  \xC2\xB7  %d", a.engine.sampleRate() / 1000.0, a.audioBuffer);
  float aw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, audio).x;
  float pw = std::min(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, proj.c_str()).x, W * 0.3f);
  bool showAudio = W > 700;
  if (showAudio) {
    rx -= aw;
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(rx, ty + 1), P.textFaint, audio);
    rx -= theme::kS4;
  }
  rx -= pw;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(rx, ty), pw, P.textDim, proj.c_str());
  if (a.doc.modified()) {
    rx -= theme::kS3;
    dl->AddCircleFilled(ImVec2(rx + 3, ty + fs * 0.5f + 1), 3.0f, P.text, 12);
  }
  // J-30: samples still decoding in the background (their sounds play once ready).
  if (const int loading = sampleCache().pending(); loading > 0) {
    char lbuf[48];
    std::snprintf(lbuf, sizeof lbuf, "Loading %d sample%s", loading, loading == 1 ? "" : "s");
    const float lw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, lbuf).x;
    rx -= lw + 12 + theme::kS4;
    dl->AddCircleFilled(ImVec2(rx + 4, ty + fs * 0.5f + 1), 3.0f, P.hot, 12);
    dl->AddText(a.fontRegular, fs, ImVec2(rx + 12, ty), P.textDim, lbuf);
  }
  // MI-02: external sync lock lamp (green = locked to the master, dim = waiting).
  if (a.midi && a.midi->syncSource != SyncSource::Internal) {
    bool locked = false;
    {
      std::lock_guard<std::mutex> lk(a.midi->followerMutex());
      locked = a.midi->syncSource == SyncSource::Mtc ? a.midi->mtc().locked(MidiManager::clockNow())
                                                      : a.midi->follower().hasTempo() && a.midi->follower().running();
    }
    const char* lbl = a.midi->syncSource == SyncSource::Mtc ? "MTC" : "CLK";
    const float lw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl).x;
    rx -= lw + 14 + theme::kS4;
    ui::Led(dl, ImVec2(rx + 4, ty + fs * 0.5f + 1), 3.5f, locked);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(rx + 12, ty + 1), locked ? P.text : P.textFaint, lbl);
  }
  mcpui::statusLamp(a, dl, rx, ty, fs);  // SC-07: AI assistant server (lit: connected, hot: working)
  float leftMax = rx - wp.x - theme::kS3 * 2 - theme::kS4;

  // Left: the hint for whatever is hovered (notifications float above as cards).
  float lx = wp.x + theme::kS3;
  // K-112: the Info box homed in the status line (Logic, Tracker) shows the hint here, and keeps the
  // last one (dimmed) when nothing is hovered, as its own box does.
  static std::string lastHint;
  const bool infoBox = layoutparts::infoInBar(a);
  if (!a.hint.empty()) lastHint = a.hint;
  if (infoBox && a.hint.empty() && !lastHint.empty()) {
    ui::drawIcon(dl, ui::Icon::Help, ImVec2(lx + 6, ty + fs * 0.5f + 1), 11, P.textFaint);
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(lx + 16, ty), leftMax - 16, P.textFaint, lastHint.c_str());
  }
  if ((a.settings.getBool("ui.showHints") || infoBox) && !a.hint.empty()) {
    // Hint bar: the subject (name / value) bright, the help in parentheses dimmer.
    const std::string& h = a.hint;
    size_t cut = h.find("   (");
    std::string head = cut == std::string::npos ? h : h.substr(0, cut);
    ui::drawIcon(dl, ui::Icon::Help, ImVec2(lx + 6, ty + fs * 0.5f + 1), 11, P.hot);
    lx += 16;
    float hw = std::min(a.fontBold->CalcTextSizeA(fs, 1e9f, 0, head.c_str()).x, leftMax - 16);
    theme::textEllipsis(dl, a.fontBold, fs, ImVec2(lx, ty), leftMax - 16, P.text, head.c_str());
    if (cut != std::string::npos && leftMax - 16 - hw > 40)
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(lx + hw + 8, ty), leftMax - 16 - hw - 8, P.textFaint, h.c_str() + cut + 3);
  }
  ImGui::End();
  drawNotifications(a, wp, W, fs);
}

// Notifications: stacked cards over the bottom-right corner, newest at the
// bottom; a lamp (green, red for errors) and a quick fade before they expire.
// J-78: long messages wrap (up to four lines); a click copies the English
// message and its details to the clipboard ("Copied" for a second), the x
// dismisses the card, and a hovered card stays up.
static void drawNotifications(App& a, ImVec2 wp, float W, float fs);

void drawNotificationCards(App& a) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  drawNotifications(a, ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - theme::kS2), vp->WorkSize.x, theme::kFontCaption);
}

static void drawNotifications(App& a, ImVec2 wp, float W, float fs) {
  if (a.toasts.empty()) return;
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float fsz = fs + 1;
  const float cardMaxW = std::min(460.0f, vp->WorkSize.x * 0.45f);
  const float padL = 30, padR = 46;  // lamp | text | copy + close glyphs
  const float lineH = a.fontRegular->CalcTextSizeA(fsz, 1e9f, 0, "Ag").y;
  struct Card {
    Toast* t;
    ImVec2 c0, c1;
  };
  std::vector<Card> cards;
  float y = wp.y - theme::kS2;
  for (auto it = a.toasts.rbegin(); it != a.toasts.rend() && cards.size() < 3; ++it) {
    Toast& t = *it;
    const bool copied = t.copiedUntil > a.now;
    const char* shown = copied ? i18n::tr("Copied to clipboard") : t.text.c_str();
    const ImVec2 one = a.fontRegular->CalcTextSizeA(fsz, 1e9f, 0, shown);
    const float wrapW = cardMaxW - padL - padR;
    const float tw = std::min(one.x, wrapW);
    const float th = one.x <= wrapW ? lineH : std::min(a.fontRegular->CalcTextSizeA(fsz, 1e9f, wrapW, shown).y, lineH * 4);
    // Checkpoint 55: an offer's buttons on a row under its text, right-aligned.
    float aw = 0;
    for (const Toast::Action& ac : t.actions) {
      aw += a.fontRegular->CalcTextSizeA(fsz, 1e9f, 0, i18n::tr(ac.label.c_str())).x + 20 + 6;
    }
    const float bodyW = std::max(tw, std::min(aw, cardMaxW - padL - 12));
    const float cw = bodyW + padL + padR, ch = std::max(34.0f, th + 18) + (t.actions.empty() ? 0.0f : 28.0f);
    cards.push_back({&t, ImVec2(wp.x + W - theme::kS3 - cw, y - ch), ImVec2(wp.x + W - theme::kS3, y)});
    y -= ch + theme::kS1;
  }
  // Input: one borderless window over the cards, kept in front of every other window.
  ImVec2 bb0(1e9f, 1e9f), bb1(-1e9f, -1e9f);
  for (const auto& c : cards) {
    bb0 = ImVec2(std::min(bb0.x, c.c0.x), std::min(bb0.y, c.c0.y));
    bb1 = ImVec2(std::max(bb1.x, c.c1.x), std::max(bb1.y, c.c1.y));
  }
  const ImGuiWindowFlags cardFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoScrollWithMouse;
  // 0.7.8 (user report: plugin windows covered the notices): cards over a plugin window are each
  // drawn in a window of their own, which goes into a top-most OS window while it is over one
  // (viewports::nextWindowIsOverlay); square-cornered there (an OS window has no round corners).
  const bool lift = viewports::overCovers(bb0, bb1);
  ImGui::SetNextWindowPos(bb0);
  ImGui::SetNextWindowSize(ImVec2(bb1.x - bb0.x, bb1.y - bb0.y));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##notifications", nullptr, cardFlags | (lift ? ImGuiWindowFlags_NoInputs : 0));
  ImGui::PopStyleVar(3);
  ImGuiWindow* self = ImGui::GetCurrentWindow();
  ImGui::BringWindowToDisplayFront(self);
  // K-314 (C-06): in front of the panels, but behind a dialog the cards would cover (on a small
  // window the Render and Record windows' buttons sit where the cards are): the dialog wins.
  ImGuiWindow* under = nullptr;
  for (ImGuiWindow* w : GImGui->Windows) {  // display order, back to front: the lowest such dialog
    if (lift) break;  // (a lifted card is above every window of the main one)
    if (w == self || !w->WasActive || w->Hidden || w->ParentWindow || w->DockNodeAsHost || w->DockIsActive) {
      continue;
    }
    if ((w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildWindow)) || ((w->Flags & ImGuiWindowFlags_NoTitleBar) && !(w->Flags & ImGuiWindowFlags_Modal))) {
      continue;  // bars, overlays and menus keep the cards over them
    }
    if (w->Pos.x < bb1.x && w->Pos.x + w->Size.x > bb0.x && w->Pos.y < bb1.y && w->Pos.y + w->Size.y > bb0.y) {
      under = w;
      break;
    }
  }
  if (under) {
    ImGui::BringWindowToDisplayBehind(self, under);
  }
  ImDrawList* fg = under ? ImGui::GetWindowDrawList() : ImGui::GetForegroundDrawList(vp);
  if (under) {
    fg->PushClipRect(ImVec2(bb0.x - 4, bb0.y - 4), ImVec2(bb1.x + 4, bb1.y + 6), false);  // (the cards' shadow)
  }
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  if (lift) ImGui::End();  // (the cards' own windows take the input)
  for (const auto& c : cards) {
    Toast& t = *c.t;
    float radius = theme::kRadiusLg;
    if (lift) {
      ImGui::SetNextWindowPos(c.c0);
      ImGui::SetNextWindowSize(ImVec2(c.c1.x - c.c0.x, c.c1.y - c.c0.y));
      viewports::nextWindowIsOverlay();
      char nm[48];
      std::snprintf(nm, sizeof nm, "##notecard%llu", static_cast<unsigned long long>(t.serial));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
      ImGui::Begin(nm, nullptr, cardFlags);
      ImGui::PopStyleVar(3);
      ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
      fg = ImGui::GetWindowDrawList();
      if (viewports::windowLifted()) radius = 0;
    }
    ImGui::PushID(static_cast<int>(t.serial));
    ImGui::SetCursorScreenPos(c.c0);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(c.c1.x - c.c0.x, c.c1.y - c.c0.y));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 xc(c.c1.x - 14, c.c0.y + 14);  // close glyph centre
    const bool overClose = hov && std::fabs(mouse.x - xc.x) <= 10 && std::fabs(mouse.y - xc.y) <= 10;
    // Checkpoint 55: the offer's buttons (right to left from the card's right edge, on its last row).
    struct ActRect {
      ImVec2 p0, p1;
    };
    std::vector<ActRect> acts;
    {
      float ax = c.c1.x - 10;
      for (size_t k = t.actions.size(); k-- > 0;) {
        const float bw = a.fontRegular->CalcTextSizeA(fsz, 1e9f, 0, i18n::tr(t.actions[k].label.c_str())).x + 20;
        acts.insert(acts.begin(), ActRect{ImVec2(ax - bw, c.c1.y - 30), ImVec2(ax, c.c1.y - 8)});
        ax -= bw + 6;
      }
    }
    int overAct = -1;
    for (size_t k = 0; k < acts.size() && hov; ++k) {
      if (mouse.x >= acts[k].p0.x && mouse.x < acts[k].p1.x && mouse.y >= acts[k].p0.y && mouse.y < acts[k].p1.y) {
        overAct = static_cast<int>(k);
      }
    }
    if (hov) {
      t.until = std::max(t.until, a.now + 1.0);  // stays while the pointer is on it
      a.setHint(overClose ? i18n::tr("Dismiss this notification") : i18n::tr("Click to copy the message (with its details) to the clipboard"));
      ui::Tooltip(overClose ? "Dismiss" : "Click to copy");
    }
    if (clicked && overAct >= 0) {
      const std::function<void()> run = t.actions[static_cast<size_t>(overAct)].run;
      t.until = 0;
      t.actions.clear();
      if (run) {
        a.later(run);  // (next frame: the action may edit the song or open a window)
      }
    } else if (clicked && overClose) {
      t.until = 0;
    } else if (clicked) {
      ImGui::SetClipboardText(toast::clipboardText(t.english.empty() ? t.text : t.english, t.details, t.error).c_str());
      t.copiedUntil = a.now + toast::kCopiedFeedback;
      t.until = std::max(t.until, a.now + toast::kCopiedFeedback + 0.6);
    }
    ImGui::PopID();
    const bool copied = t.copiedUntil > a.now;
    const float life = static_cast<float>(t.until - a.now);
    const float alpha = std::clamp(life / 0.4f, 0.0f, 1.0f);
    const ImVec2 c0 = c.c0, c1 = c.c1;
    fg->AddRectFilled(ImVec2(c0.x + 2, c0.y + 3), ImVec2(c1.x + 2, c1.y + 3), theme::withAlpha(P.ink, 0.25f * alpha), radius);
    fg->AddRectFilled(c0, c1, theme::withAlpha(hov ? theme::mix(theme::popupBg(P), P.text, 0.06f) : theme::popupBg(P), alpha), radius);
    fg->AddRect(c0, c1, theme::withAlpha(P.dark ? P.ink : P.lineStrong, 0.8f * alpha), radius);
    const ImU32 lamp = copied ? P.led : t.error ? P.danger : P.led;
    fg->AddRectFilled(ImVec2(c0.x, c0.y), ImVec2(c0.x + 3, c1.y), theme::withAlpha(lamp, alpha), radius, ImDrawFlags_RoundCornersLeft);
    for (size_t k = 0; k < acts.size() && k < t.actions.size(); ++k) {
      const ImU32 bf = static_cast<int>(k) == overAct ? theme::mix(P.accent, P.text, 0.15f) : P.accent;
      fg->AddRectFilled(acts[k].p0, acts[k].p1, theme::withAlpha(bf, alpha), theme::kRadius);
      const char* bl = i18n::tr(t.actions[k].label.c_str());
      const ImVec2 bs = a.fontRegular->CalcTextSizeA(fsz, 1e9f, 0, bl);
      fg->AddText(a.fontRegular, fsz, ImVec2(std::round((acts[k].p0.x + acts[k].p1.x - bs.x) * 0.5f), std::round((acts[k].p0.y + acts[k].p1.y - bs.y) * 0.5f)),
                  theme::withAlpha(theme::contrastText(P.accent), alpha), bl);
    }
    const float lampH = t.actions.empty() ? 0.0f : 28.0f;  // (the lamp and the text keep to the card's first rows)
    const ImVec2 lc(c0.x + 18, (c0.y + c1.y - lampH) * 0.5f);
    if (copied) {  // a tick in place of the lamp
      ui::drawIconFit(fg, ui::Icon::Check, ImVec2(lc.x - 5.5f, lc.y - 4.5f), ImVec2(lc.x + 5.5f, lc.y + 4.5f), theme::withAlpha(P.led, alpha));
    } else {
      fg->AddCircleFilled(lc, 4.0f, theme::withAlpha(lamp, alpha), 12);
    }
    const char* shown = copied ? i18n::tr("Copied to clipboard") : t.text.c_str();
    const float textW = c1.x - c0.x - 30 - 46;
    const ImU32 tc = theme::withAlpha(copied ? P.text : t.error ? theme::mix(P.danger, P.text, 0.35f) : P.text, alpha);
    const float oneW = a.fontRegular->CalcTextSizeA(fsz, 1e9f, 0, shown).x;
    if (oneW <= textW + 0.5f) {
      theme::textEllipsis(fg, a.fontRegular, fsz, ImVec2(c0.x + 30, std::round((c0.y + c1.y - lampH - fsz) * 0.5f) - 1), textW + 1, tc, shown);
    } else {
      const ImVec4 clip(c0.x, c0.y + 6, c1.x, c1.y - 6 - lampH);
      fg->AddText(a.fontRegular, fsz, ImVec2(c0.x + 30, c0.y + 9), tc, shown, nullptr, textW, &clip);
    }
    // Copy glyph (hovered) and close glyph.
    const ImU32 glyph = theme::withAlpha(P.textDim, alpha * (hov ? 1.0f : 0.55f));
    if (hov && !copied) ui::drawIcon(fg, ui::Icon::Clone, ImVec2(c1.x - 34, xc.y), 11, glyph);
    ui::drawIcon(fg, ui::Icon::Close, xc, 10, overClose ? theme::withAlpha(P.text, alpha) : glyph);
    if (lift) ImGui::End();
  }
  if (lift) return;
  if (under) {
    fg->PopClipRect();
  }
  ImGui::End();
}

}  // namespace gl

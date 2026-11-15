// Gridline — top bar and bottom hint/status bar.
//
// The top bar follows FL Studio's main-window grouping (menus · transport ·
// tempo/position · pattern · master · monitor · window buttons), rendered as
// Gridline panels. It is responsive: every group knows its width up front, and
// when the window is too narrow the least important groups move into the "⋯"
// overflow popup instead of overlapping or being clipped.
#include "../core/MenuParity.h"
#include "../core/Timeline.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "CollabUi.h"
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
#include "../core/LayoutShell.h"
#include "../core/LayoutStyle.h"
#include "Theme.h"
#include "Widgets.h"
#include "MeterUi.h"
#include "MixerMeters.h"
#include "../core/MeterEdit.h"
#include "../core/NoteTools.h"
#include "Windows.h"
#include "SystemMenu.h"

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

namespace {

// ---- geometry (design tokens) ----
constexpr float kPad = theme::kS3;          // outer padding left/right
constexpr float kPanelH = 36.0f;            // panel height (6 px margin top/bottom)
constexpr float kPanelPad = theme::kS1;     // inner padding of a panel
constexpr float kGap = theme::kS2;          // gap between panels
constexpr float kBtn = theme::kIconBtn;     // 28 px icon buttons
constexpr float kInner = 2.0f;              // gap between buttons inside a panel
constexpr float kMenuPadX = 9.0f;
constexpr float kMenuPadXCompact = 6.0f;

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
    const float ty = std::round(p.y + (h - bt.size) * 0.5f - h * 0.08f);
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
DevRect gDevRects[] = {{"play", {}, {}}, {"stop", {}, {}}, {"rec", {}, {}}, {"metro", {}, {}}, {"brsearch", {}, {}}, {"brclear", {}, {}}, {"chroute", {}, {}}, {"chroutefree", {}, {}}, {"chroutelist", {}, {}}, {"tempoint", {}, {}}, {"tempofrac", {}, {}}, {"loupedrag", {}, {}}, {"taptempo", {}, {}}, {"tapmenu", {}, {}}, {"tapmenutap", {}, {}}, {"menutools", {}, {}}, {"bar_transport", {}, {}}, {"bar_tempo", {}, {}}, {"bar_pos", {}, {}}, {"bar_pattern", {}, {}}, {"bar_master", {}, {}}, {"bar_monitor", {}, {}}, {"bar_windows", {}, {}}, {"bar_more", {}, {}}, {"prstamp", {}, {}}, {"prstamplbl", {}, {}}, {"prstampchip", {}, {}}, {"brcollapse", {}, {}}, {"capture", {}, {}}, {"sessrec", {}, {}}, {"drawmode", {}, {}}, {"devswap", {}, {}}, {"devsave", {}, {}}, {"brchip", {}, {}}, {"devexpand", {}, {}}, {"devwindow", {}, {}}};
float gMetroMirror = 1.0f;
double gMetroBeat = 0;  // (dev: the song position, in beats, the glyph's pose was drawn for)
bool gMetroTicking = false;  // (dev: ... whether it ticked then, and from which beat)
double gMetroAnchor = 0;
float gDevFitWidth = 0;  // K-132: dev, the width the bar fits its groups into (0 = the window's)
void noteRectAt(const char* name, ImVec2 a, ImVec2 b) {
  for (DevRect& r : gDevRects)
    if (std::strcmp(r.name, name) == 0) r.a = a, r.b = b, r.frame = ImGui::GetFrameCount();
}
void noteRect(const char* name) { noteRectAt(name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }  // the last item's rectangle
}  // namespace

void devNoteRect(const char* name) { noteRect(name); }
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
bool topBarMetronomeTicking(double* anchorBeat) {
  if (anchorBeat) *anchorBeat = gMetroAnchor;
  return gMetroTicking;
}

static float drawTopBarLogic(App& a);
static float drawTopBarLive(App& a);
static float drawLogicControlBar(App& a);

float drawTopBar(App& a) {
  if (layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic)) return drawLogicControlBar(a);  // K-112b
  if (layoutui::active() == layout::Preset::Logic) return drawTopBarLogic(a);  // K-79: two rows
  if (livelook::active(a)) return drawTopBarLive(a);                          // K-112c: menu row and one flat control row
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
  enum G { Menus, Transport, Tempo, Pos, Pat, Master, Monitor, Windows, Count };
  bool show[Count];
  std::fill(show, show + Count, true);
  float widths[Count] = {};
  auto setWidths = [&]() {
    const float w[Count] = {sysMenus ? 0.0f : menusTotal + theme::kS2, wTransport, wTempo, wPos, wPattern, wMaster, wMonitor, wWindows};
    std::copy(w, w + Count, widths);
  };
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
    bool anyHidden = std::find(show, show + Count, false) != show + Count;
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
  // Menus stay visible as long as possible (a right click on the mark opens them too).
  const G hideOrder[] = {Monitor, Master, Windows, Pos, Pat, Menus, Tempo};
  for (G g : hideOrder)
    if (required() > fitW) show[g] = false;
  const bool overflow = std::find(show, show + Count, false) != show + Count;

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
    ImGui::SetNextWindowPos(ImVec2(x, wp.y + h - 2));
    if (ImGui::BeginPopup("##mainmenu")) {
      for (const menustyle::Menu* m : kMenus)
        if (ImGui::BeginMenu(i18n::tr(m->name))) {
          drawStyleMenu(a, *m);
          ImGui::EndMenu();
        }
      ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(ImVec2(x, wp.y + h - 2));
    if (program && ImGui::BeginPopup("##programmenu")) {
      drawStyleMenu(a, *program);
      ImGui::EndPopup();
    }
    x += kBtn + theme::kS2;
  }
  if (show[Menus] && !sysMenus) {
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
      ImGui::SetNextWindowPos(ImVec2(bp.x, wp.y + h - 2));
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
  }
  x += kGap;

  // ---- transport ----
  if (show[Transport]) {
    panel(dl, x, wTransport, panelTop);
    noteRectAt("bar_transport", ImVec2(x, panelTop), ImVec2(x + wTransport, panelTop + kPanelH));
    float cx = x + kPanelPad;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    int mode = a.engine.mode() == PlayMode::Song ? 1 : a.engine.mode() == PlayMode::Live ? 2 : 0;
    const char* labels[] = {"PAT", "SONG", "LIVE"};
    const char* hints[] = {"Pattern mode: play the selected pattern (L)", "Song mode: play the playlist arrangement (L)",
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
      a.recordArmed = !a.recordArmed;
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
  }

  // ---- tempo + position (one panel) ----
  if (show[Tempo]) {
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
  }

  // ---- pattern selector ----
  if (show[Pat]) {
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
  }

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
  if (show[Windows]) {
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
        *w.flag = !*w.flag;
        if (*w.flag) a.focusWindow = w.name;
      }
      cx += kBtn + kInner;
    }
    rx -= kGap;
  }
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
  if (show[Monitor]) {
    rx -= wMonitor;
    panel(dl, rx, wMonitor, panelTop);
    noteRectAt("bar_monitor", ImVec2(rx, panelTop), ImVec2(rx + wMonitor, panelTop + kPanelH));
    ImVec2 p(rx + kPanelPad + 4, panelTop + 7);
    monitor(p, wMonitor - kPanelPad * 2 - 8);
    ImGui::SetCursorScreenPos(ImVec2(rx, panelTop));
    if (ImGui::InvisibleButton("##mon", ImVec2(wMonitor, kPanelH))) a.showPerformance = !a.showPerformance;
    ui::Hint("Master output level and audio processing load (click: Performance window)");
    rx -= kGap;
  }
  auto masterControls = [&](bool inPopup) {
    if (!s.mixer.empty()) ui::ParamKnob("mvol", mixerParam(s.masterId(), MixerParam::Volume), 13.0f, inPopup ? "Volume" : nullptr);
    ImGui::SameLine(0, kInner);
    ui::ParamKnob("mpitch", songParam(SongParam::MasterPitch), 13.0f, inPopup ? "Pitch" : nullptr, true);
  };
  if (show[Master]) {
    rx -= wMaster;
    panel(dl, rx, wMaster, panelTop);
    noteRectAt("bar_master", ImVec2(rx, panelTop), ImVec2(rx + wMaster, panelTop + kPanelH));
    ImGui::SetCursorScreenPos(ImVec2(rx + kPanelPad, panelTop + (kPanelH - 26) * 0.5f));
    ImGui::BeginGroup();
    masterControls(false);
    ImGui::EndGroup();
    float cx = rx + kPanelPad + 26 + kInner + 26 + theme::kS2;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    // J-91 / J-97: while it clicks, the metronome snaps over on every beat (a hard tick-tock, no
    // in-between frames; Interface animations). Toggling it only shows the rest pose.
    const bool ticking = a.metronome && a.engine.playing() && !a.engine.countingIn() && scrollui::animations();
    const double beatTicks = static_cast<double>(std::max<Tick>(1, s.beatTicks()));  // the metronome's beat (time signature)
    static motion::MetronomeTicker ticker;
    // K-179: in song time the beats of the meter map (each section's own beat).
    const double beatNow = a.engine.mode() != PlayMode::Pattern ? MeterMap(s).beatCount(a.engine.position()) : a.engine.position() / beatTicks;
    const float mirror = static_cast<float>(ticker.update(beatNow, ticking));
    if (ui::ToggleIcon("metro", ui::Icon::Metronome, a.metronome, kBtn, "Metronome (Ctrl+M)", mirror)) a.metronome = !a.metronome;
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
    if (ui::ToggleIcon("typing", ui::Icon::Keyboard, a.typingKeyboard, kBtn, "Typing keyboard to piano (Ctrl+T, right-click: options)"))
      a.typingKeyboard = !a.typingKeyboard;
    if (ImGui::BeginPopupContextItem("##typingmenu")) {
      typingKeyboardMenu(a);
      ImGui::EndPopup();
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
    if (!show[Menus] && !sysMenus) {
      section("MENU");
      for (const menustyle::Menu* m : kMenus)
        if (ImGui::BeginMenu(i18n::tr(m->name))) {
          drawStyleMenu(a, *m);
          ImGui::EndMenu();
        }
    }
    if (!show[Tempo] || !show[Pos]) {
      section("TIME");
      if (!show[Tempo]) {
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
    if (!show[Pat] && ImGui::BeginMenu(i18n::tr("Pattern"))) {
      patternList(a);
      ImGui::EndMenu();
    }
    if (!show[Master]) {
      section("MASTER");
      masterControls(true);
      if (ImGui::MenuItem(i18n::tr("Metronome"), keys::label("options.metronome"), a.metronome)) a.metronome = !a.metronome;
      if (ImGui::MenuItem(i18n::tr("Typing keyboard"), keys::label("options.typingKeyboard"), a.typingKeyboard)) a.typingKeyboard = !a.typingKeyboard;
    }
    if (!show[Monitor]) {
      section("OUTPUT");
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(160, 26));
      dl = ImGui::GetWindowDrawList();
      monitor(p, 160);
    }
    if (!show[Windows]) {
      section("WINDOWS");
      if (ImGui::MenuItem(i18n::tr("Playlist"), keys::label("view.playlist"), a.showPlaylist)) a.showPlaylist = !a.showPlaylist;
      if (ImGui::MenuItem(i18n::tr("Channel rack"), keys::label("view.channelRack"), a.showRack)) a.showRack = !a.showRack;
      if (ImGui::MenuItem(i18n::tr("Piano roll"), keys::label("view.pianoRoll"), a.showPiano)) a.showPiano = !a.showPiano;
      if (ImGui::MenuItem(i18n::tr("Mixer"), keys::label("view.mixer"), a.showMixer)) a.showMixer = !a.showMixer;
      if (ImGui::MenuItem(i18n::tr("Browser"), keys::label("view.browser"), a.showBrowser)) a.showBrowser = !a.showBrowser;
    }
    ImGui::EndPopup();
  }

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
    ImGui::SetNextWindowPos(ImVec2(x, wp.y + kMenuRowH - 1));
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
      ImGui::SetNextWindowPos(ImVec2(bp.x, wp.y + kMenuRowH - 1));
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
      a.recordArmed = !a.recordArmed;
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
      a.countInBars = a.countInBars > 0 ? 0 : 1;
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
      ImGui::SetNextWindowPos(ImVec2(x, wp.y + kMenuRowH - 1));
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
      ImGui::SetNextWindowPos(ImVec2(bp.x, wp.y + kMenuRowH - 1));
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
  const float posW = 80, loopW = 80, backW = std::ceil(a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, i18n::tr("Back to arrangement")).x) + 30;
  const float segGridW = 68, segArrW = std::ceil(a.fontRegular->CalcTextSizeA(theme::kFontBody - 1, 1e9f, 0, i18n::tr("Arrangement")).x) + 32;
  const float keyW = 30, scaleW = 92;  // the key and scale group
  bool showLoop = true, showSys = true, showKey = true;
  auto leftW = [&]() { return 40 + 4 + 64 + 4 + 44 + 8 + 14 + 4 + 54 + (showKey ? 8 + keyW + 2 + scaleW : 0); };
  // The transport group: play, stop, record, then automation arm and overdub, then the record menu.
  auto centreW = [&]() { return posW + 4 + 26 * 6 + 28 * 2 + 10 + 6 + 4 + backW + (showLoop ? 8 + 26 + 4 + loopW + 4 + loopW : 0); };
  auto rightW = [&]() { return (showSys ? 66 + 4 + 26 + 4 + 40 + 14 : 0) + 26 + segGridW + segArrW; };
  if (10 + leftW() + 16 + centreW() + 16 + rightW() + 8 > W) showLoop = false;
  if (10 + leftW() + 16 + centreW() + 16 + rightW() + 8 > W) showKey = false;
  if (10 + leftW() + 16 + centreW() + 16 + rightW() + 8 > W) showSys = false;

  // ---- left: tap, tempo, meter, metronome, launch grid ----
  float x = wp.x + 10;
  {
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
    x += 44;
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
    x += 68;
    int sigNum = s.timeSigNum, sigDen = s.timeSigDen;
    const Tick at = static_cast<Tick>(a.engine.position());
    meter::meterAt(s, at, sigNum, sigDen);
    char sig[16];
    std::snprintf(sig, sizeof sig, "%d / %d", sigNum, sigDen);
    if (field("##siglive", sig, x, 44, false, "Meter at the song position (click to edit)")) meterui::openEditor(a, meter::changeAt(s, at));
    x += 52;
    // Metronome: a lamp.
    ImGui::SetCursorScreenPos(ImVec2(x, cy));
    if (ImGui::InvisibleButton("##metrolive", ImVec2(14, kH))) a.metronome = !a.metronome;
    noteRect("metro");
    ui::Hint(keyHint("Metronome", "options.metronome").c_str());
    dl->AddCircleFilled(ImVec2(x + 7, cy + kH * 0.5f), 4.0f, a.metronome ? P.led : P.surface600, 12);
    if (ImGui::IsItemHovered()) dl->AddCircle(ImVec2(x + 7, cy + kH * 0.5f), 6.0f, theme::withAlpha(P.text, 0.4f), 12);
    x += 18;
    // Launch grid of every track (one value when they agree).
    const LaunchGrid g0 = s.tracks.empty() ? LaunchGrid::FourBeats : s.tracks[0].launch;
    bool same = true;
    for (const PlaylistTrack& t : s.tracks) same = same && t.launch == g0;
    static const char* const kShort[] = {"None", "1/16", "1/8", "1/4", "1/2", "1 Bar", "Clip"};
    const char* gl = same ? kShort[std::min<int>(static_cast<int>(g0), 6)] : "Mixed";
    if (field("##quantlive", gl, x, 54, false, "Launch quantisation: where a launched clip starts (every track; each track can have its own in the grid)"))
      ImGui::OpenPopup("##quantpop");
    ImGui::SetNextWindowPos(ImVec2(x, cy + kH + 2));
    if (ImGui::BeginPopup("##quantpop")) {
      for (int i = 0; i < static_cast<int>(LaunchGrid::Count); ++i)
        if (ImGui::MenuItem(launchGridName(static_cast<LaunchGrid>(i)), nullptr, same && g0 == static_cast<LaunchGrid>(i)))
          a.doc.editSong("change launch grid", [i](Song& x2) {
            for (PlaylistTrack& t : x2.tracks) t.launch = static_cast<LaunchGrid>(i);
          });
      ImGui::EndPopup();
    }
    x += 54;
    // The key and scale (the notes editor's scale highlighting): root, then the scale.
    if (showKey) {
      x += 8;
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
      x += keyW + 2;
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
      x += scaleW;
    }
  }
  const float leftEnd = x;

  // ---- right: CPU, typing keyboard, the view switch ----
  float rx = wp.x + W - 8 - segArrW - segGridW;
  {
    const bool grid = livelook::gridView(a);
    // K-112c (round 31): the switch is as tall as every other control on the bar (it stood 2 px taller),
    // its labels centred by the font's capital band, as the other fields.
    ImGui::SetCursorScreenPos(ImVec2(rx, cy));
    if (ImGui::InvisibleButton("##vgrid", ImVec2(segGridW, kH)) && !grid) livelook::showView(a, true);
    const bool hg = ImGui::IsItemHovered(), pg = ImGui::IsItemActive();
    ui::Hint("Clip grid: the tracks as columns of clip slots, scenes as rows");
    ImGui::SetCursorScreenPos(ImVec2(rx + segGridW, cy));
    if (ImGui::InvisibleButton("##varr", ImVec2(segArrW, kH)) && grid) livelook::showView(a, false);
    const bool ha = ImGui::IsItemHovered(), pa = ImGui::IsItemActive();
    ui::Hint("Arrangement: the tracks as lanes along the song's time");
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
    const char* tab = "Tab";
    const ImVec2 st = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, tab);
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(rx - 8 - st.x, theme::textTop(a.fontRegular, theme::kFontLabel, cy, kH)), P.textFaint, tab);
    rx -= 26;
  }
  if (showSys) {
    rx -= 40;
    if (flat("##keylive", rx, 40, a.typingKeyboard, keyHint("Computer keyboard as a piano keyboard", "options.typingKeyboard").c_str())) a.typingKeyboard = !a.typingKeyboard;
    const ImVec2 ks = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, "KEY");
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round(rx + (40 - ks.x) * 0.5f), std::round(cy + (kH - ks.y) * 0.5f)), glyphCol(a.typingKeyboard), "KEY");
    // K-112c: Draw mode: the editors' pencil on (lit) or off (the select tool).
    rx -= 4 + 26;
    const bool drawOn = livelook::drawMode(a);
    if (flat("##drawlive", rx, 26, drawOn, "Draw mode: the arrangement and the notes editor draw with the pencil (off: select)")) livelook::setDrawMode(a, !drawOn);
    noteRect("drawmode");
    ui::drawIconFit(dl, ui::Icon::ToolDraw, ImVec2(rx + 6, cy + 4), ImVec2(rx + 20, cy + kH - 4), glyphCol(drawOn));
    rx -= 4 + 66;
    char cpu[24];
    std::snprintf(cpu, sizeof cpu, "CPU %d %%", CpuMeter::displayPercent(a.engine.cpuLoad()));
    if (field("##cpulive", cpu, rx, 66, false, "Audio processing load (click: the Performance panel)")) a.showPerformance = !a.showPerformance;
  }

  // ---- middle: position, transport, Back to arrangement, loop ----
  {
    const float cw = centreW();
    float mx = std::round(wp.x + (W - cw) * 0.5f);
    mx = std::max(mx, leftEnd + 16);
    const std::string pos = bbt(static_cast<Tick>(a.engine.position()), false);
    field("##poslive", pos.c_str(), mx, posW, false, "Song position: bars . beats . sixteenths");
    mx += posW + 4;
    const bool playing = a.engine.playing();
    menuparity::transportTick(a);
    const std::string playTip = menuparity::playButtonHint(a);
    if (flat("##playlive", mx, 26, playing, playTip.empty() ? keyHint("Play / pause", "transport.play").c_str() : playTip.c_str(), true))
      playing ? a.engine.pause() : (void)menuparity::playPressed(a);
    noteRect("play");
    ui::drawIconFit(dl, playing ? ui::Icon::Pause : ui::Icon::Play, ImVec2(mx + 8, cy + 6), ImVec2(mx + 18, cy + kH - 6), glyphCol(playing));
    mx += 28;
    if (flat("##stoplive", mx, 26, false, "Stop (right-click: options)", true)) menuparity::stopButtonPressed(a);
    noteRect("stop");
    if (ImGui::BeginPopupContextItem("##stopmenu")) {
      menuparity::stopMenuItems(a);
      ImGui::EndPopup();
    }
    dl->AddRectFilled(ImVec2(mx + 9, cy + 7), ImVec2(mx + 17, cy + kH - 7), P.text, 1.0f);
    mx += 28;
    if (flat("##reclive", mx, 26, false, keyHint("Record (right-click: what to record, punch, count-in)", "transport.record").c_str(), true))
      a.recordArmed = !a.recordArmed;
    noteRect("rec");
    if (ImGui::BeginPopupContextItem("##recmenu")) {
      recordOptionsMenu(a);
      ImGui::EndPopup();
    }
    dl->AddCircleFilled(ImVec2(mx + 13, cy + kH * 0.5f), 4.5f, a.recordArmed ? P.arm : theme::withAlpha(P.arm, 0.75f), 16);
    if (a.recordArmed) dl->AddCircle(ImVec2(mx + 13, cy + kH * 0.5f), 7.0f, P.arm, 16, 1.5f);
    mx += 28;
    // What a recording takes besides audio: notes on top of the existing ones (overdub), and
    // controller moves (automation). The same switches as the record menu.
    {
      RecordOptions& ro = a.recOptions;
      if (flat("##overdublive", mx, 26, ro.overdub, "Overdub: recorded notes join the existing ones (off: they replace them)")) ro.overdub = !ro.overdub;
      ui::drawIconFit(dl, ui::Icon::Plus, ImVec2(mx + 8, cy + 6), ImVec2(mx + 18, cy + kH - 6), glyphCol(ro.overdub));
      mx += 28;
      if (flat("##autoarmlive", mx, 26, ro.automation, "Record automation: controller and knob moves are recorded while recording")) ro.automation = !ro.automation;
      ui::drawIconFit(dl, ui::Icon::ClipAutomation, ImVec2(mx + 7, cy + 5), ImVec2(mx + 19, cy + kH - 5), glyphCol(ro.automation));
      mx += 28;
    }
    // K-112c: Capture: what was just played while nothing recorded becomes notes in the pattern.
    {
      const bool kept = a.capture.size() > 0;
      if (flat("##capturelive", mx, 26, false, "Capture: the notes you just played, while nothing recorded, into the current pattern") && kept)
        livelook::captureNow(a);
      noteRect("capture");
      ui::drawIconFit(dl, ui::Icon::Download, ImVec2(mx + 7, cy + 5), ImVec2(mx + 19, cy + kH - 5), kept ? P.text : P.textFaint);
      mx += 28;
    }
    // K-112c: Session record: records into the current pattern, looping it (the record button in
    // pattern mode); lit while that runs.
    {
      const bool on = livelook::sessionRecording(a);
      if (flat("##sessreclive", mx, 26, on, "Session record: records into the current pattern while it loops (click again to stop recording)"))
        livelook::toggleSessionRecord(a);
      noteRect("sessrec");
      const ImVec2 c(mx + 13, cy + kH * 0.5f);
      dl->AddRect(ImVec2(c.x - 6, c.y - 6), ImVec2(c.x + 6, c.y + 6), on ? glyphCol(true) : P.text, 2.0f, 1.2f, 0);
      dl->AddCircleFilled(c, 3.0f, on ? glyphCol(true) : P.arm, 12);
      mx += 28 + 4;
    }
    if (flat("##recoptlive", mx, 26, false, "Recording options: what to record, punch, count-in")) ImGui::OpenPopup("##recoptpop");
    ui::drawIconFit(dl, ui::Icon::More, ImVec2(mx + 7, cy + kH * 0.5f - 1), ImVec2(mx + 19, cy + kH * 0.5f + 2), P.text);
    ImGui::SetNextWindowPos(ImVec2(mx, cy + kH + 2));
    if (ImGui::BeginPopup("##recoptpop")) {
      recordOptionsMenu(a);
      ImGui::EndPopup();
    }
    mx += 26 + 6;
    // Back to arrangement: lit while any track plays a grid clip.
    bool anyGrid = false;
    if (a.engine.mode() == PlayMode::Live)
      for (int t = 0; t < static_cast<int>(s.tracks.size()) && !anyGrid; ++t) anyGrid = a.engine.liveActive(t) != kNoId;
    if (flat("##backlive", mx, backW, anyGrid, "Back to arrangement: every track stops its grid clip and the arrangement plays again")) {
      a.engine.stopAllLive();
      a.engine.setMode(PlayMode::Song);
    }
    ui::drawIconFit(dl, ui::Icon::Undo, ImVec2(mx + 7, cy + 6), ImVec2(mx + 17, cy + kH - 6), glyphCol(anyGrid));
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(mx + 22, theme::textTop(a.fontRegular, theme::kFontCaption, cy, kH)), glyphCol(anyGrid),
                i18n::tr("Back to arrangement"));
    mx += backW + 4;
    if (showLoop) {
      // The loop range: the playlist's time selection (the whole song while there is none).
      const bool sel = a.playlist.selEnd > a.playlist.selStart;
      Tick songEnd = 0;
      for (const Clip& c : s.playlist) songEnd = std::max(songEnd, c.start + std::max<Tick>(c.length, 0));
      const Tick l0 = sel ? a.playlist.selStart : 0, l1 = sel ? a.playlist.selEnd : songEnd;
      mx += 4;
      // Punch in and out at the time selection, beside the range it uses.
      if (flat("##punchlive", mx, 26, a.recPunch, "Punch in and out: recording keeps only what falls inside the time selection")) a.recPunch = !a.recPunch;
      ui::drawIconFit(dl, ui::Icon::Trim, ImVec2(mx + 6, cy + 5), ImVec2(mx + 20, cy + kH - 5), glyphCol(a.recPunch));
      mx += 26 + 4;
      const std::string ls = bbt(l0, false), ll = bbt(std::max<Tick>(0, l1 - l0), true);
      field("##loopstart", ls.c_str(), mx, loopW, !sel, "Loop start: the playlist's time selection (drag in the ruler to set it)");
      mx += loopW + 4;
      field("##looplen", ll.c_str(), mx, loopW, !sel, "Loop length: bars . beats . sixteenths of the time selection");
    }
  }
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
bool gGrouped = false;  // the pills sit in a shared group container (drawn by barGroup): no face of their own at rest

// The rounded container a run of pills shares (the pane toggles, the transport, the switches).
// K-112b (close-range pass): a capsule (ends fully round), as 216 and 125 draw every group of the bar.
void barGroup(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const float r = std::floor((p1.y - p0.y + 6) * 0.5f);
  dl->AddRectFilled(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p1.x + 3, p1.y + 3), P.surface700, r);
  dl->AddRect(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p1.x + 3, p1.y + 3), P.lineStrong, r);
}

bool barPill(const char* id, ImVec2 p, ImVec2 size, ui::Icon icon, const char* label, bool on, const char* hint, ImU32 iconCol = 0, bool strong = false) {
  const auto& P = pal();
  ImGui::SetCursorScreenPos(p);
  const bool click = ImGui::InvisibleButton(id, size);
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
  const ImU32 ink = filled ? theme::contrastText(P.hot) : on ? P.hot : iconCol ? iconCol : hov ? P.text : P.textDim;
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
    if (known || newCount < 3) {
      if (!known) drawn.insert(static_cast<int>(icon)), ++newCount;
      ui::drawIcon(dl, icon, ImVec2(std::round(p.x + size.x * 0.5f), std::round(p.y + size.y * 0.5f)), std::min(16.0f, size.y - 12), ink);
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
static void toggleCycle(App& a) {
  PlaylistState& pst = a.playlist;
  static Tick lastA = 0, lastB = 0;  // the range cycle last used (this run)
  if (pst.selEnd > pst.selStart) {
    lastA = pst.selStart, lastB = pst.selEnd;
    pst.selStart = pst.selEnd = 0;
  } else if (lastB > lastA) {
    pst.selStart = lastA, pst.selEnd = lastB;
  } else {
    const MeterMap mm(a.doc.song());
    const Tick from = mm.barStartAt(static_cast<Tick>(a.engine.position()));
    Tick to = from;
    for (int k = 0; k < 4; ++k) to = mm.nextBar(to);
    pst.selStart = from, pst.selEnd = to;
  }
}

static float drawLogicControlBar(App& a) {
  const auto& P = pal();
  const layout::Style& ls = layout::style(layout::Preset::Logic);
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const bool menusHere = !sysmenu::active();
  const float rowH = menusHere ? ls.menuRowH : 0.0f, barH = ls.controlBarH;
  const float h = rowH + barH;
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
    static int openMenu = -1;
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
      if (m && m->name && !isOpen && devMenuTake(i < 0 ? "Gridline" : m->name)) ImGui::OpenPopup(pid);  // dev: "menu open <name>"
      if (hov && openMenu >= 0 && openMenu != i && !isOpen) ImGui::OpenPopup(pid);
      isOpen = ImGui::IsPopupOpen(pid);
      if (isOpen || hov) dl->AddRectFilled(bp, ImVec2(bp.x + mw, bp.y + rowH - 2), isOpen ? P.surface800 : P.surface900, theme::kRadiusSm);
      const ImVec2 ts = font->CalcTextSizeA(ImGui::GetFontSize(), 1e9f, 0, name);
      dl->AddText(font, ImGui::GetFontSize(), ImVec2(std::round(bp.x + (mw - ts.x) * 0.5f), std::round(bp.y + (rowH - 2 - ts.y) * 0.5f)), P.text, name);
      ImGui::SetNextWindowPos(ImVec2(bp.x, wp.y + rowH - 1));
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
    if (!anyOpen) openMenu = -1;
    // The project and its save state, as the window's title: centred in the row when it fits,
    // else at the row's right end (the status bar's, which this layout has not).
    std::string proj = a.projectFile().empty() ? i18n::tr("Untitled project") : fs::path(a.projectFile()).stem().string();
    if (std::string shared = collabui::projectTitle(a); !shared.empty()) proj = shared;
    const float tfs = theme::kFontCaption, menusEnd = x + 16, rowEnd = wp.x + W - kPad;
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
  noteRectAt("logic_bar", ImVec2(wp.x, top), ImVec2(wp.x + W, wp.y + h));

  // ---- left: the panes ----
  const float pillH = 28, pillW = 30, gap = 6, gi = 2;  // gi: between the pills of one group
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
                          {"##lb_help", ui::Icon::Question, "logic.help", i18n::N_("Quick help"), "view.quickHelp"},
                          {"##lb_tb", ui::Icon::Download, nullptr, i18n::N_("Toolbar: the tracks area's tools, snap and arrangement"), "view.toolbar"},
                          {"##lb_ed", ui::Icon::ToolDraw, nullptr, i18n::N_("Editors"), "view.editors"},
                          {"##lb_sm", ui::Icon::CatController, nullptr, i18n::N_("Smart controls: the selected track's main controls"), nullptr},
                          {"##lb_mx", ui::Icon::Mixer, nullptr, i18n::N_("Mixer"), "view.mixer"}};
    constexpr int kFirstGroup = 4;
    barGroup(dl, ImVec2(x, py), ImVec2(x + pillW * kFirstGroup + gi * (kFirstGroup - 1), py + pillH));
    {
      const float x2 = x + pillW * kFirstGroup + gi * (kFirstGroup - 1) + gap + 4;
      barGroup(dl, ImVec2(x2, py), ImVec2(x2 + pillW * 3 + gi * 2, py + pillH));
    }
    gGrouped = true;
    int paneIndex = 0;
    for (const Pane& pn : panes) {
      if (paneIndex++ == kFirstGroup) x += gap + 4 - gi;  // the second group's own pill

      bool on = false;
      // K-112b (checkpoint 24): the toolbar a tray, the editors a pencil, as 126 and 216 draw them.
      if (pn.icon == ui::Icon::Download) {  // the toolbar switch (a per-user setting, never in the project)
        on = a.settings.getBool("ui.logicToolbar");
        if (barPill(pn.id, ImVec2(x, py), ImVec2(pillW, pillH), pn.icon, nullptr, on, hintWithKey(pn.name, pn.cmd).c_str())) a.settings.set("ui.logicToolbar", !on);
        x += pillW + gi;
        continue;
      }
      bool* smart = layoutui::windowOpenFlag(a, "Smart controls");
      if (pn.icon == ui::Icon::CatController) {  // K-112b: the Smart controls, in the lower pane
        on = smart && *smart && logicRegionShown(a, "logic.lower");
        if (barPill(pn.id, ImVec2(x, py), ImVec2(pillW, pillH), pn.icon, nullptr, on, hintWithKey(pn.name, pn.cmd).c_str()) && smart) {
          if (on) layoutui::setRegionVisible(a, "logic.lower", false);
          else layoutui::setRegionVisible(a, "logic.lower", true), *smart = true;
        }
        x += pillW + gi;
        continue;
      }
      const bool smartOn = smart && *smart;
      if (pn.region) on = logicRegionShown(a, pn.region);
      else if (pn.icon == ui::Icon::Mixer) on = a.showMixer && logicRegionShown(a, "logic.lower");
      else on = logicRegionShown(a, "logic.lower") && !a.showMixer && !smartOn;
      if (barPill(pn.id, ImVec2(x, py), ImVec2(pillW, pillH), pn.icon, nullptr, on, hintWithKey(pn.name, pn.cmd).c_str())) {
        if (pn.region) layoutui::setRegionVisible(a, pn.region, !on);
        else if (pn.icon == ui::Icon::Mixer) {
          if (on) layoutui::setRegionVisible(a, "logic.lower", false);
          else layoutui::setRegionVisible(a, "logic.lower", true), a.showMixer = true;
        } else {
          if (on) layoutui::setRegionVisible(a, "logic.lower", false);
          else {
            layoutui::setRegionVisible(a, "logic.lower", true);
            if ((!a.showPiano && !a.showRack) || smartOn) a.showPiano = true;
          }
        }
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
  const bool lcdCustom = a.settings.getNumber("ui.lcdMode") >= 1;
  const float tb = 34, lcdW = lcdCustom ? std::clamp(std::round(W * 0.23f), 300.0f, 440.0f) : 300.0f, lcdH = 50, modesGap = 20;
  // K-112b: the control bar's parts a right-click on the bar shows or hides (ref 070).
  const bool showNav = a.settings.getBool("ui.cbar.nav"), showMaster = a.settings.getBool("ui.cbar.master");
  // K-112b (checkpoint 25): cycle sits in the transport after record, as 216, 125 and 150 have it.
  const bool showCycle = a.settings.getBool("ui.cbar.cycle");
  const int transportN = (showNav ? 6 : 3) + (showCycle ? 1 : 0);
  const float transportW = tb * static_cast<float>(transportN) + gi * static_cast<float>(transportN - 1);
  const float groupW = transportW + 10 + lcdW;
  float gx = std::round(wp.x + (W - groupW) * 0.5f);
  {  // a narrow window: the group moves left so the switches after it still fit before the right group
    const float modesW = pillW * 2 + 40 + gap * 3;  // count-in ("1234"), click, typing (checkpoint 24: icon pills)
    gx = std::min(gx, std::round(wp.x + W - 200 - modesW - modesGap - groupW));
  }
  gx = std::max(gx, leftEnd + 16);
  {
    const float ty = std::round(top + (barH - tb) * 0.5f);
    const Tick bar = std::max<Tick>(1, s.barTicks());
    const Tick pos = static_cast<Tick>(a.engine.position());
    const bool songBars = a.engine.mode() != PlayMode::Pattern;
    float cx = gx;
    barGroup(dl, ImVec2(cx, ty), ImVec2(cx + transportW, ty + tb));
    gGrouped = true;
    if (showNav) {
    if (barPill("##lb_start", ImVec2(cx, ty), ImVec2(tb, tb), ui::Icon::ChevronLeft, nullptr, false, hintWithKey("Go to the start", "transport.toStart").c_str()))
      a.engine.setPosition(0);
    cx += tb + gi;
    if (barPill("##lb_back", ImVec2(cx, ty), ImVec2(tb, tb), ui::Icon::Left, nullptr, false, hintWithKey("Back one bar", "transport.rewindBar").c_str()))
      a.engine.setPosition(std::max<Tick>(0, songBars ? MeterMap(s).barStartAt(std::max<Tick>(0, pos - 1)) : (pos - 1) / bar * bar));
    cx += tb + gi;
    if (barPill("##lb_fwd", ImVec2(cx, ty), ImVec2(tb, tb), ui::Icon::Right, nullptr, false, hintWithKey("Forward one bar", "transport.forwardBar").c_str()))
      a.engine.setPosition(songBars ? MeterMap(s).nextBar(pos) : (pos / bar + 1) * bar);
    cx += tb + gi;
    }
    menuparity::transportTick(a);
    const bool playing = a.engine.playing();
    const std::string playTip = menuparity::playButtonHint(a);
    // K-112b (checkpoint 25): stop, then play, as 216, 125 and 150 order them.
    if (barPill("stop", ImVec2(cx, ty), ImVec2(tb, tb), ui::Icon::Stop, nullptr, false, i18n::tr("Stop (right-click: options)")))
      menuparity::stopButtonPressed(a);
    noteRect("stop");
    if (ImGui::BeginPopupContextItem("##stopmenu")) {
      menuparity::stopMenuItems(a);
      ImGui::EndPopup();
    }
    cx += tb + gi;
    ImGui::SetCursorScreenPos(ImVec2(cx, ty));
    if (barPill("play", ImVec2(cx, ty), ImVec2(tb, tb), playing ? ui::Icon::Pause : ui::Icon::Play, nullptr, playing,
                playTip.empty() ? hintWithKey("Play / pause", "transport.play").c_str() : playTip.c_str(), theme::rgb(theme::role::kPlay), true))
      playing ? a.engine.pause() : (void)menuparity::playPressed(a);
    noteRect("play");
    cx += tb + gi;
    if (barPill("rec", ImVec2(cx, ty), ImVec2(tb, tb), ui::Icon::Record, nullptr, false,
                hintWithKey("Record (right-click: what to record, punch, count-in)", "transport.record").c_str(), a.recordArmed ? P.arm : theme::withAlpha(P.arm, 0.8f)))
      a.recordArmed = !a.recordArmed;
    if (a.recordArmed) dl->AddRect(ImVec2(cx, ty), ImVec2(cx + tb, ty + tb), P.arm, 6.0f, 0, 1.5f);
    noteRect("rec");
    if (ImGui::BeginPopupContextItem("##recmenu")) {
      recordOptionsMenu(a);
      ImGui::EndPopup();
    }
    if (showCycle) {
      cx += tb + gi;
      const bool cyc = a.playlist.selEnd > a.playlist.selStart;
      if (barPill("##lb_cycle", ImVec2(cx, ty), ImVec2(tb, tb), ui::Icon::LoopArrows, nullptr, cyc,
                  hintWithKey("Cycle: playback loops inside the time selection (off: the whole song)", nullptr).c_str()))
        toggleCycle(a);
    }
    gGrouped = false;
    cx += tb + 10;
    // The display: position, tempo, metre and key.
    const ImVec2 l0(cx, std::round(top + (barH - lcdH) * 0.5f)), l1(cx + lcdW, l0.y + lcdH);
    // K-112b: a dark display in three sections: the position large and bold (bar and beat, each
    // captioned), the tempo, then the meter over the key.
    dl->AddRectFilled(l0, l1, theme::mix(P.field, P.ink, 0.55f), 8.0f);
    dl->AddRect(l0, l1, P.lineStrong, 8.0f);
    noteRectAt("bar_display", l0, l1);
    const float chevW = 18;  // the mode chevron at the right end
    const float posW = std::round(lcdW * (lcdCustom ? 0.27f : 0.44f)), locW = lcdCustom ? std::round(lcdW * 0.25f) : 0.0f;
    const float tempoW = std::round(lcdW * (lcdCustom ? 0.22f : 0.28f));
    const float third = posW + locW;  // where the tempo section starts
    for (const float sx : {posW, locW > 0 ? third : -1.0f, third + tempoW, lcdW - chevW})
      if (sx > 0) dl->AddLine(ImVec2(l0.x + sx, l0.y + 8), ImVec2(l0.x + sx, l1.y - 8), theme::withAlpha(P.line, 0.9f));
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
    auto caption = [&](float cx0, const char* text) {
      const float cf = theme::kFontLabel - 1;
      const char* t = i18n::tr(text);
      const float w = a.fontRegular->CalcTextSizeA(cf, 1e9f, 0, t).x;
      dl->AddText(a.fontRegular, cf, ImVec2(std::round(cx0 - w * 0.5f), std::round(l1.y - cf - 4)), P.textFaint, t);
    };
    {
      ImGui::SetCursorScreenPos(l0);
      ImGui::InvisibleButton("##lcdpos", ImVec2(posW, lcdH));
      if (ImGui::BeginPopupContextItem("##posmenu")) {
        menuparity::timeFormatMenu(a);
        ImGui::EndPopup();
      }
      ui::Hint("Song position (right-click: bars and beats, minutes and seconds, or timecode)");
      // K-112b (checkpoint 25): larger position digits, as 216, 125 and 205 set them (about 0.6 of the display).
      const float fs = std::round(lcdH * 0.62f);
      const float cy = std::round(l0.y + (lcdH - fs) * 0.5f - lcdH * 0.08f);
      const bool barsBeats = static_cast<int>(a.settings.getNumber("ui.timeFormat")) == 0;
      const size_t c1 = posText.find(':'), c2 = c1 == std::string::npos ? c1 : posText.find(':', c1 + 1);
      if (lcdCustom) {  // clock time over the position in bars
        const double at = a.engine.position();
        rows(l0.x, posW, ops::formatSongTime(s, at, ops::TimeFormat::MinSecCs), bars(at),
             P.textDim, P.text);
      } else if (barsBeats && c1 != std::string::npos && c2 != std::string::npos) {  // "19.2": bar, beat
        const std::string barS = std::to_string(std::atoi(posText.substr(0, c1).c_str()));
        const std::string beatS = std::to_string(std::atoi(posText.substr(c1 + 1, c2 - c1 - 1).c_str()));
        const float bw = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, barS.c_str()).x;
        const float dw = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, ".").x;
        const float ew = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, beatS.c_str()).x;
        const float dotX = std::round(l0.x + posW * 0.62f);  // the dot stays put as the bar grows
        dl->AddText(a.fontBold, fs, ImVec2(dotX - bw, cy), P.text, barS.c_str());
        dl->AddText(a.fontBold, fs, ImVec2(dotX, cy), P.text, ".");
        dl->AddText(a.fontBold, fs, ImVec2(dotX + dw, cy), P.text, beatS.c_str());
        // Each caption under its number, kept apart by at least a space (single digits are narrow).
        const float cf = theme::kFontLabel - 1;
        const float barW = a.fontRegular->CalcTextSizeA(cf, 1e9f, 0, i18n::tr("Bar")).x, beatW = a.fontRegular->CalcTextSizeA(cf, 1e9f, 0, i18n::tr("Beat")).x;
        float c1 = dotX - bw * 0.5f, c2 = dotX + dw + ew * 0.5f;
        const float need = (barW + beatW) * 0.5f + 8, have = c2 - c1;
        if (have < need) c1 -= (need - have) * 0.5f, c2 += (need - have) * 0.5f;
        caption(c1, i18n::N_("Bar"));
        caption(c2, i18n::N_("Beat"));
      } else {
        const float f0 = std::round(fs * 0.7f);
        const ImVec2 ts = a.fontBold->CalcTextSizeA(f0, 1e9f, 0, posText.c_str());
        const float f = ts.x > posW - 16 ? f0 * (posW - 16) / ts.x : f0;
        const float w = a.fontBold->CalcTextSizeA(f, 1e9f, 0, posText.c_str()).x;
        dl->AddText(a.fontBold, f, ImVec2(std::round(l0.x + (posW - w) * 0.5f), std::round(l0.y + (lcdH - f) * 0.5f - lcdH * 0.08f)), P.text,
                    posText.c_str());
        caption(l0.x + posW * 0.5f, i18n::N_("Position"));
      }
    }
    if (lcdCustom) {  // the cycle range: its start over its end (dim without one)
      const PlaylistState& ps = a.playlist;
      const bool has = ps.selEnd > ps.selStart;
      ImGui::SetCursorScreenPos(ImVec2(l0.x + posW, l0.y));
      ImGui::InvisibleButton("##lcdloc", ImVec2(locW, lcdH));
      ui::Hint("Cycle range: its start over its end");
      noteRectAt("lcd_locators", ImVec2(l0.x + posW, l0.y), ImVec2(l0.x + third, l1.y));
      rows(l0.x + posW, locW, bars(static_cast<double>(ps.selStart)),
           bars(static_cast<double>(has ? ps.selEnd : ps.selStart)), has ? P.text : P.textFaint,
           has ? P.text : P.textFaint);
    }
    caption(l0.x + third + tempoW * 0.5f, i18n::N_("Tempo"));
    {
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
    {
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
      const float mf = theme::kFontTitle, kf = theme::kFontCaption;  // sizes the app already uses
      const float sw = a.fontBold->CalcTextSizeA(mf, 1e9f, 0, sig).x;
      const std::string grid = a.playlist.effGrid > 0 ? timeline::gridName(s, a.playlist.effGrid) : std::string();
      const std::string under = lcdCustom ? grid : (key ? key->name : std::string());
      const bool hasKey = !under.empty();
      const float my = std::round(l0.y + (lcdH - (hasKey ? mf + 1 + kf : mf)) * 0.5f);  // the pair centred as one
      dl->AddText(a.fontBold, mf, ImVec2(std::round(x0 + (w - sw) * 0.5f), my), P.text, sig);
      if (hasKey) {
        const float kw = std::min(w - 8, a.fontBold->CalcTextSizeA(kf, 1e9f, 0, under.c_str()).x);
        theme::textEllipsis(dl, a.fontBold, kf, ImVec2(std::round(x0 + (w - kw) * 0.5f), std::round(my + mf + 1)), w - 8, P.textDim, under.c_str());
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
      ImGui::SetNextWindowPos(ImVec2(l1.x - chevW, l1.y + 4));
      if (ImGui::BeginPopup("##lcdmodes")) {
        if (ImGui::MenuItem(i18n::tr("Beats and project"), nullptr, !lcdCustom)) a.settings.set("ui.lcdMode", 0);
        if (ImGui::MenuItem(i18n::tr("Custom"), nullptr, lcdCustom)) a.settings.set("ui.lcdMode", 1);
        ImGui::EndPopup();
      }
    }
    gx = l1.x;
  }

  // ---- after the display: cycle, count-in, click ----
  float mx = gx + modesGap;
  {
    struct Mode {
      const char* id;
      const char* label;
      bool on;
      const char* hint;
      int what;
      const char* key;  // its show setting
      ui::Icon icon;    // K-112b (checkpoint 24): the switches as glyphs, as 216, 125 and 126 draw them
    };
    // The count-in shows its beats ("1234") in place of a glyph; cycle and click are glyphs.
    const Mode modes[] = {{"##lb_count", "1234", a.countInBars > 0, "Count-in before recording", 1, "ui.cbar.countIn", ui::Icon::Count},
                          {"##lb_click", i18n::N_("Click"), a.metronome, "Metronome", 2, "ui.cbar.click", ui::Icon::Metronome}};
    auto modeW = [&](const Mode& m) { return m.icon == ui::Icon::Count ? 40.0f : pillW; };
    const bool showTyping = a.settings.getBool("ui.cbar.typing");
    {  // the switches share one container (as many as fit before the right group)
      float gw = 0;
      for (const Mode& m : modes) {
        if (!a.settings.getBool(m.key)) continue;
        const float w = modeW(m);
        if (mx + gw + w > wp.x + W - 200) break;
        gw += w + gi;
      }
      if (showTyping && mx + gw + pillW <= wp.x + W - 200) gw += pillW + gi;
      if (gw > 0) barGroup(dl, ImVec2(mx, py), ImVec2(mx + gw - gi, py + pillH));
    }
    gGrouped = true;
    for (const Mode& m : modes) {
      if (!a.settings.getBool(m.key)) continue;
      const float w = modeW(m);
      if (mx + w > wp.x + W - 200) break;  // the right group stays
      if (barPill(m.id, ImVec2(mx, py), ImVec2(w, pillH), m.icon, m.icon == ui::Icon::Count ? m.label : nullptr, m.on, i18n::tr(m.hint))) {
        if (m.what == 1) a.countInBars = a.countInBars > 0 ? 0 : 1;
        else a.metronome = !a.metronome;
      }
      if (m.what == 2) noteRect("metro");
      mx += w + gi;
    }
    // The typing keyboard: the computer keys play notes (its options on a right-click).
    if (showTyping && mx + pillW <= wp.x + W - 200) {
      if (barPill("##lb_typing", ImVec2(mx, py), ImVec2(pillW, pillH), ui::Icon::Keyboard, nullptr, a.typingKeyboard,
                  hintWithKey("Typing keyboard to piano (right-click: options)", "options.typingKeyboard").c_str()))
        a.typingKeyboard = !a.typingKeyboard;
      noteRect("typing");
      if (ImGui::BeginPopupContextItem("##lbtypingmenu")) {
        typingKeyboardMenu(a);
        ImGui::EndPopup();
      }
      mx += pillW + gi;
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
      bool* f = layoutui::windowOpenFlag(a, sd.window);
      const bool on = f && *f && logicRegionShown(a, "logic.right");
      if (barPill(sd.id, ImVec2(rx, py), ImVec2(pillW, pillH), sd.icon, nullptr, on, hintWithKey(sd.name, sd.cmd).c_str()) && f) {
        if (on) layoutui::setRegionVisible(a, "logic.right", false);
        else {
          layoutui::setRegionVisible(a, "logic.right", true);
          *f = true;
        }
      }
      rx += pillW + gi;
    }
    gGrouped = false;
    // The master volume: a slider over the master's level meter, left of the right group.
    if (showMaster && !s.mixer.empty() && right0 - 16 - 130 > mx + 8) {
      const ParamId pid = mixerParam(s.masterId(), MixerParam::Volume);
      const float vw = 120, vx = right0 - 16 - vw, vy = std::round(top + barH * 0.5f) - 5;
      ImGui::SetCursorScreenPos(ImVec2(vx - 4, vy - 6));
      ImGui::InvisibleButton("##lb_mvol", ImVec2(vw + 8, 24));
      const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
      if (const auto d = describeParam(s, pid)) {
        const auto v = getParam(s, pid);
        double n = v ? d->toNormalized(*v) : 0.8;
        ImGuiIO& io = ImGui::GetIO();
        if (act && io.MouseDelta.x != 0) {
          n = std::clamp(n + io.MouseDelta.x / (vw - 10) * (io.KeyShift ? 0.15 : 1.0), 0.0, 1.0);
          a.doc.setParam(pid, d->fromNormalized(n), true);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.doc.setParam(pid, d->def, false);
        if (ImGui::IsItemDeactivated()) a.doc.endGesture();
        const float tx = std::round(vx + 5 + (vw - 10) * static_cast<float>(n));
        dl->AddRectFilled(ImVec2(vx, vy - 2), ImVec2(vx + vw, vy + 2), theme::mix(P.surface800, P.ink, 0.45f), 2.0f);
        dl->AddRectFilled(ImVec2(vx, vy - 2), ImVec2(tx, vy + 2), theme::withAlpha(P.textDim, 0.7f), 2.0f);
        // the meter: two thin bars (left, right) under the slider
        const mixmeters::Reading r = mixmeters::get(a, s.masterId());
        for (int k = 0; k < 2; ++k) {
          const float pk = k == 0 ? r.peakL : r.peakR;
          const float db = pk > 0.00001f ? 20.0f * std::log10(pk) : -60.0f;
          const float f = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
          const float my = vy + 8 + k * 4;
          dl->AddRectFilled(ImVec2(vx, my), ImVec2(vx + vw, my + 3), theme::mix(P.surface800, P.ink, 0.5f), 1.0f);
          if (f > 0) dl->AddRectFilled(ImVec2(vx, my), ImVec2(vx + vw * f, my + 3), pk >= 1.0f ? P.danger : theme::rgb(theme::role::kPlay), 1.0f);
        }
        dl->AddCircleFilled(ImVec2(tx, vy), 6.0f, hov || act ? P.text : theme::mix(P.text, P.surface800, 0.2f), 16);
        dl->AddCircle(ImVec2(tx, vy), 6.0f, theme::withAlpha(P.ink, 0.6f), 16, 1.0f);
        if (hov || act) {
          const std::string tip = std::string(i18n::tr("Master volume")) + ": " + d->format(v.value_or(d->def));
          ui::Hint(tip.c_str());
        }
      }
      noteRectAt("logic_mvol", ImVec2(vx, vy - 6), ImVec2(vx + vw, vy + 18));
      right0 = vx - 4;
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
  // K-112b: customise the control bar (ref 070): a right-click on its empty space lists its parts.
  {
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool onBar = mp.y >= top && mp.y < wp.y + h && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
    if ((onBar && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) || devMenuTake("Customise")) ImGui::OpenPopup("##cbcustom");
    if (ImGui::BeginPopup("##cbcustom")) {
      ImGui::SeparatorText(i18n::tr("Show in the control bar"));
      auto item = [&a](const char* label, const char* key) {
        const bool on = a.settings.getBool(key);
        if (ImGui::MenuItem(i18n::tr(label), nullptr, on)) a.settings.set(key, !on);
      };
      item(i18n::N_("Go to the start, back and forward"), "ui.cbar.nav");
      ImGui::Separator();
      item(i18n::N_("Cycle"), "ui.cbar.cycle");
      item(i18n::N_("Count-in"), "ui.cbar.countIn");
      item(i18n::N_("Click"), "ui.cbar.click");
      item(i18n::N_("Typing keyboard"), "ui.cbar.typing");
      ImGui::Separator();
      item(i18n::N_("Master volume"), "ui.cbar.master");
      ImGui::SeparatorText(i18n::tr("Display"));
      const bool custom = a.settings.getNumber("ui.lcdMode") >= 1;
      if (ImGui::MenuItem(i18n::tr("Beats and project"), nullptr, !custom)) a.settings.set("ui.lcdMode", 0);
      if (ImGui::MenuItem(i18n::tr("Custom"), nullptr, custom)) a.settings.set("ui.lcdMode", 1);
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Show everything"))) {
        for (const char* k : {"ui.cbar.nav", "ui.cbar.cycle", "ui.cbar.countIn", "ui.cbar.click", "ui.cbar.typing", "ui.cbar.master"}) a.settings.set(k, true);
        a.settings.set("ui.lcdMode", 0);
      }
      ImGui::EndPopup();
    }
  }
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
    const float cw = tw + padL + padR, ch = std::max(34.0f, th + 18);
    cards.push_back({&t, ImVec2(wp.x + W - theme::kS3 - cw, y - ch), ImVec2(wp.x + W - theme::kS3, y)});
    y -= ch + theme::kS1;
  }
  // Input: one borderless window over the cards, kept in front of every other window.
  ImVec2 bb0(1e9f, 1e9f), bb1(-1e9f, -1e9f);
  for (const auto& c : cards) {
    bb0 = ImVec2(std::min(bb0.x, c.c0.x), std::min(bb0.y, c.c0.y));
    bb1 = ImVec2(std::max(bb1.x, c.c1.x), std::max(bb1.y, c.c1.y));
  }
  ImGui::SetNextWindowPos(bb0);
  ImGui::SetNextWindowSize(ImVec2(bb1.x - bb0.x, bb1.y - bb0.y));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##notifications", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(3);
  ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
  ImDrawList* fg = ImGui::GetForegroundDrawList(vp);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  for (const auto& c : cards) {
    Toast& t = *c.t;
    ImGui::PushID(static_cast<int>(t.serial));
    ImGui::SetCursorScreenPos(c.c0);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(c.c1.x - c.c0.x, c.c1.y - c.c0.y));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 xc(c.c1.x - 14, c.c0.y + 14);  // close glyph centre
    const bool overClose = hov && std::fabs(mouse.x - xc.x) <= 10 && std::fabs(mouse.y - xc.y) <= 10;
    if (hov) {
      t.until = std::max(t.until, a.now + 1.0);  // stays while the pointer is on it
      a.setHint(overClose ? i18n::tr("Dismiss this notification") : i18n::tr("Click to copy the message (with its details) to the clipboard"));
      ui::Tooltip(overClose ? "Dismiss" : "Click to copy");
    }
    if (clicked && overClose) {
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
    fg->AddRectFilled(ImVec2(c0.x + 2, c0.y + 3), ImVec2(c1.x + 2, c1.y + 3), theme::withAlpha(P.ink, 0.25f * alpha), theme::kRadiusLg);
    fg->AddRectFilled(c0, c1, theme::withAlpha(hov ? theme::mix(theme::popupBg(P), P.text, 0.06f) : theme::popupBg(P), alpha), theme::kRadiusLg);
    fg->AddRect(c0, c1, theme::withAlpha(P.dark ? P.ink : P.lineStrong, 0.8f * alpha), theme::kRadiusLg);
    const ImU32 lamp = copied ? P.led : t.error ? P.danger : P.led;
    fg->AddRectFilled(ImVec2(c0.x, c0.y), ImVec2(c0.x + 3, c1.y), theme::withAlpha(lamp, alpha), theme::kRadiusLg, ImDrawFlags_RoundCornersLeft);
    const ImVec2 lc(c0.x + 18, (c0.y + c1.y) * 0.5f);
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
      theme::textEllipsis(fg, a.fontRegular, fsz, ImVec2(c0.x + 30, std::round((c0.y + c1.y - fsz) * 0.5f) - 1), textW + 1, tc, shown);
    } else {
      const ImVec4 clip(c0.x, c0.y + 6, c1.x, c1.y - 6);
      fg->AddText(a.fontRegular, fsz, ImVec2(c0.x + 30, c0.y + 9), tc, shown, nullptr, textW, &clip);
    }
    // Copy glyph (hovered) and close glyph.
    const ImU32 glyph = theme::withAlpha(P.textDim, alpha * (hov ? 1.0f : 0.55f));
    if (hov && !copied) ui::drawIcon(fg, ui::Icon::Clone, ImVec2(c1.x - 34, xc.y), 11, glyph);
    ui::drawIcon(fg, ui::Icon::Close, xc, 10, overClose ? theme::withAlpha(P.text, alpha) : glyph);
  }
  ImGui::End();
}

}  // namespace gl

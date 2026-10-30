// Gridline — top bar and bottom hint/status bar.
//
// The top bar follows FL Studio's main-window grouping (menus · transport ·
// tempo/position · pattern · master · monitor · window buttons), rendered as
// Gridline panels. It is responsive: every group knows its width up front, and
// when the window is too narrow the least important groups move into the "⋯"
// overflow popup instead of overlapping or being clipped.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
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
#include "../core/Automation.h"
#include "../core/LevelMeter.h"
#include "../core/Motion.h"
#include "SmoothScrollUi.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

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

struct MenuDef {
  const char* name;
  void (*fn)(App&);
};
// The bar hosts up to ~9 menus; add entries here when Menus.cpp grows.
const MenuDef kMenus[] = {{i18n::N_("File"), menuFile},     {i18n::N_("Edit"), menuEdit},       {i18n::N_("Add"), menuAdd}, {i18n::N_("Patterns"), menuPatterns},
                          {i18n::N_("View"), menuView},     {i18n::N_("Options"), menuOptions}, {i18n::N_("Tools"), menuTools},
                          {i18n::N_("Help"), menuHelp}};


void noteRectAt(const char* name, ImVec2 a, ImVec2 b);  // dev: an "sdlat" target (below)

// Tempo LCD: drag / wheel / double-click to type. Returns true when changed.
bool tempoLcd(App& a, double* v, float width) {
  ImGui::PushID("tempo");
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##md", ImVec2(width, kBtn));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool changed = false;
  ImGuiIO& io = ImGui::GetIO();
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", *v);
  // Two zones split at the decimal point: dragging the whole part steps whole BPM and keeps the
  // decimals (96.250 -> 97.250); dragging the decimals moves smoothly (0.25 BPM per pixel).
  const float dotX = ui::LcdCharX(p, ImVec2(width, kBtn), buf, static_cast<int>(std::strcspn(buf, ".")));
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
  std::snprintf(buf, sizeof buf, "%.3f", *v);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ui::Lcd(dl, p, ImVec2(width, kBtn), buf, hov, act);
  noteRectAt("tempoint", p, ImVec2(dotX, p.y + kBtn));
  noteRectAt("tempofrac", ImVec2(dotX, p.y), ImVec2(p.x + width, p.y + kBtn));
  const bool overWhole = act ? zone == Whole : io.MousePos.x < dotX;
  if (hov || act) {  // which digits a drag moves: a short accent bar under them
    const auto& P = pal();
    const float x0 = overWhole ? ui::LcdCharX(p, ImVec2(width, kBtn), buf, 0) : dotX;
    const float x1 = overWhole ? dotX : ui::LcdCharX(p, ImVec2(width, kBtn), buf, static_cast<int>(std::strlen(buf)));
    const float y = std::round(p.y + kBtn - 4.0f);
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

void panel(ImDrawList* dl, float x, float w, float top) {
  const auto& P = pal();
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
DevRect gDevRects[] = {{"play", {}, {}}, {"stop", {}, {}}, {"rec", {}, {}}, {"metro", {}, {}}, {"brsearch", {}, {}}, {"brclear", {}, {}}, {"chroute", {}, {}}, {"chroutefree", {}, {}}, {"chroutelist", {}, {}}, {"tempoint", {}, {}}, {"tempofrac", {}, {}}};
float gMetroMirror = 1.0f;
void noteRectAt(const char* name, ImVec2 a, ImVec2 b) {
  for (DevRect& r : gDevRects)
    if (std::strcmp(r.name, name) == 0) r.a = a, r.b = b, r.frame = ImGui::GetFrameCount();
}
void noteRect(const char* name) { noteRectAt(name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }  // the last item's rectangle
}  // namespace

void devNoteRect(const char* name) { noteRect(name); }
bool topBarButtonRect(const char* name, float& x0, float& y0, float& x1, float& y1) {
  for (const DevRect& r : gDevRects)
    if (std::strcmp(r.name, name) == 0 && r.frame >= ImGui::GetFrameCount() - 1) {
      x0 = r.a.x, y0 = r.a.y, x1 = r.b.x, y1 = r.b.y;
      return true;
    }
  return false;
}
float topBarMetronomeMirror() { return gMetroMirror; }

float drawTopBar(App& a) {
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = theme::kTopBarH;
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
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
  constexpr size_t kMenuCount = sizeof kMenus / sizeof kMenus[0];
  float menuW[kMenuCount];
  float menusTotal = 0;
  float segW = 50.0f, tempoW = 84.0f, posW = 118.0f, patW = 150.0f;
  float wTransport = 0, wTempo = 0, wPos = 0, wPattern = 0;
  const float wMaster = kPanelPad * 2 + 26 + kInner + 26 + theme::kS2 + kBtn * 2 + kInner;
  const float wMonitor = kPanelPad * 2 + 96.0f;
  const float wWindows = kPanelPad * 2 + kBtn * 5 + kInner * 4;
  const float wMark = kBtn;
  const float wMore = kBtn;
  auto measure = [&](bool compact) {
    menusTotal = 0;
    const float pad = compact ? kMenuPadXCompact : kMenuPadX;
    for (size_t i = 0; i < kMenuCount; ++i) {
      menuW[i] = std::ceil(ImGui::CalcTextSize(i18n::tr(kMenus[i].name)).x + pad * 2);
      menusTotal += menuW[i];
    }
    segW = compact ? 40.0f : 46.0f;
    tempoW = compact ? 72.0f : 84.0f;
    posW = compact ? 104.0f : 118.0f;
    patW = compact ? 120.0f : 150.0f;
    wTransport = kPanelPad * 2 + (segW * 3 + 4) + theme::kS2 + kBtn * 3 + kInner * 2;
    wTempo = kPanelPad * 2 + tempoW;
    wPos = posW + kInner;  // joins the tempo panel
    wPattern = kPanelPad * 2 + patW + kInner + kBtn;
  };

  // ---- fit: compact sizes first, then hide the lowest priority groups ----
  enum G { Menus, Transport, Tempo, Pos, Pat, Master, Monitor, Windows, Count };
  bool show[Count];
  std::fill(show, show + Count, true);
  float widths[Count] = {};
  auto setWidths = [&]() {
    const float w[Count] = {menusTotal + theme::kS2, wTransport, wTempo, wPos, wPattern, wMaster, wMonitor, wWindows};
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
    r += n * kGap + theme::kS6;  // gaps + breathing room between the left and right clusters
    if (anyHidden) r += wMore + kGap;
    return r;
  };
  if (required() > W) {
    measure(true);
    setWidths();
  }
  // Menus stay visible as long as possible (a right click on the mark opens them too).
  const G hideOrder[] = {Monitor, Master, Windows, Pos, Pat, Menus, Tempo};
  for (G g : hideOrder)
    if (required() > W) show[g] = false;
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
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) a.home.showHome(a.now);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##mainmenu");
    ImGui::SetNextWindowPos(ImVec2(x, wp.y + h - 2));
    if (ImGui::BeginPopup("##mainmenu")) {
      for (const auto& m : kMenus)
        if (ImGui::BeginMenu(i18n::tr(m.name))) {
          m.fn(a);
          ImGui::EndMenu();
        }
      ImGui::EndPopup();
    }
    x += kBtn + theme::kS2;
  }
  if (show[Menus]) {
    static int openMenu = -1;
    bool anyOpen = false;
    for (size_t i = 0; i < sizeof kMenus / sizeof kMenus[0]; ++i) {
      std::string pid = std::string("##menu_") + kMenus[i].name;
      ImVec2 bp(x, btnTop);
      ImGui::SetCursorScreenPos(bp);
      ImGui::PushID(static_cast<int>(i));
      bool clicked = ImGui::InvisibleButton("##m", ImVec2(menuW[i], kBtn));
      bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
      ImGui::PopID();
      bool isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (clicked && !isOpen) ImGui::OpenPopup(pid.c_str());
      // Menu-bar behaviour: while one menu is open, hovering another switches to it.
      if (hov && openMenu >= 0 && openMenu != static_cast<int>(i) && !isOpen) ImGui::OpenPopup(pid.c_str());
      isOpen = ImGui::IsPopupOpen(pid.c_str());
      if (isOpen || hov)
        dl->AddRectFilled(bp, ImVec2(bp.x + menuW[i], bp.y + kBtn), isOpen ? P.surface800 : P.surface900, theme::kRadius);
      ImVec2 ts = ImGui::CalcTextSize(i18n::tr(kMenus[i].name));
      dl->AddText(ImVec2(std::round(bp.x + (menuW[i] - ts.x) * 0.5f), std::round(bp.y + (kBtn - ts.y) * 0.5f)),
                  isOpen || hov ? P.text : P.textDim, i18n::tr(kMenus[i].name));
      ImGui::SetNextWindowPos(ImVec2(bp.x, wp.y + h - 2));
      if (ImGui::BeginPopup(pid.c_str())) {
        anyOpen = true;
        openMenu = static_cast<int>(i);
        kMenus[i].fn(a);
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
    float cx = x + kPanelPad;
    ImGui::SetCursorScreenPos(ImVec2(cx, btnTop));
    int mode = a.engine.mode() == PlayMode::Song ? 1 : a.engine.mode() == PlayMode::Live ? 2 : 0;
    const char* labels[] = {"PAT", "SONG", "LIVE"};
    const char* hints[] = {"Pattern mode: play the selected pattern (L)", "Song mode: play the playlist arrangement (L)",
                           "Live mode: launch the clips of the playlist's live area by hand (playlist or live pads)"};
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
    if (ui::IconButton("stop", ui::Icon::Stop, false, kBtn, "Stop and return to the set song position; press again to silence everything (right-click: options)", 0, 0, true))
      menuparity::stopPressed(a);
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
    if (show[Pos]) {
      const std::string pos = menuparity::positionText(a, a.engine.position());  // J-27: time formats
      ImVec2 p(x + kPanelPad + tempoW + kInner, btnTop);
      ImGui::SetCursorScreenPos(p);
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
    rx -= kGap;
  }
  if (show[Windows]) {
    rx -= wWindows;
    panel(dl, rx, wWindows, panelTop);
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
    const float mirror = static_cast<float>(ticker.update(a.engine.position() / beatTicks, ticking));
    if (ui::ToggleIcon("metro", ui::Icon::Metronome, a.metronome, kBtn, "Metronome (Ctrl+M)", mirror)) a.metronome = !a.metronome;
    noteRect("metro");
    gMetroMirror = mirror;
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
    if (!show[Menus]) {
      section("MENU");
      for (const auto& m : kMenus)
        if (ImGui::BeginMenu(i18n::tr(m.name))) {
          m.fn(a);
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

// ---------------------------------------------------------- status bar ----

static void drawNotifications(App& a, ImVec2 wp, float W, float fs);

void drawStatusBar(App& a) {
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = theme::kStatusBarH;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.chrome));
  ImGui::Begin("##status", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();
  const float fs = theme::kFontCaption;
  const float ty = std::round(wp.y + (h - fs) * 0.5f) - 1;
  dl->AddLine(wp, ImVec2(wp.x + W, wp.y), P.line);

  // Right: audio format · project name · unsaved marker.
  std::string proj = a.projectFile().empty() ? "Untitled project" : fs::path(a.projectFile()).filename().string();
  if (std::string shared = collabui::projectTitle(a); !shared.empty()) proj = shared;  // J-111f
  char audio[48];
  std::snprintf(audio, sizeof audio, "%.1f kHz  \xC2\xB7  %d", a.engine.sampleRate() / 1000.0, a.audioBuffer);
  float rx = wp.x + W - theme::kS3;
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
  if (a.settings.getBool("ui.showHints") && !a.hint.empty()) {
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

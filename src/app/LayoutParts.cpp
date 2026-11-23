// Gridline - the layout parts (K-112 R4). See LayoutParts.h.
#include "LayoutParts.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/MenuOpsExtra.h"
#include "../core/MixerOps.h"
#include "../core/MixerHide.h"
#include "../core/Automation.h"
#include "../core/Params.h"
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "../core/Project.h"
#include "../core/TrackModes.h"
#include "../engine/ClipOps.h"
#include "../engine/Dsp.h"
#include "../engine/EqDesign.h"
#include "../packs/PacksUi.h"
#include "App.h"
#include "BrowserUi.h"
#include "ClipEditor.h"
#include "ContextMenus.h"
#include "FrameProfile.h"
#include "HostedBodies.h"
#include "HostedUi.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LiveLook.h"
#include "LayoutStyleUi.h"
#include "LogicPlugins.h"
#include "LookGeom.h"
#include "MenuParityUi.h"
#include "../core/Meter.h"
#include "../core/LevelMeter.h"
#include "MixerMeters.h"
#include "MixerHideUi.h"
#include "SmoothLibrary.h"
#include "Theme.h"
#include "TrackIcons.h"
#include "TrackerEditors.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"
#include "../packs/samplers/patchplayer.h"

namespace gl::layoutparts {

namespace fs = std::filesystem;
using theme::pal;

namespace {

// K-112D: the filled slots' blue and the instrument's green, as the reference's strips (134, 158, 233).
constexpr uint32_t kSlotBlue = 0x2363B4, kSlotGreen = 0x2F8A47;

bool* flag(App& a, const char* window) { return layoutui::windowOpenFlag(a, window); }

// True while the part draws in a region without a tab bar (its window shows no name): it titles itself.
bool gOwnTitle = false;
bool ownTitle() { return gOwnTitle; }

// One part's window: docked in its region (the shell docks it), floating where it has none.
template <class F>
void part(App& a, const char* window, int index, ImVec2 firstSize, F&& body) {
  bool* open = flag(a, window);
  if (!open || !*open) return;
  const layout::Preset p = layoutui::active();
  // A part homed in a bar has no window there: the bar draws it (the Info box: the status line).
  if (layoutui::shellBuilt(a, p) && layout::windowHome(window, p).kind == layout::HomeKind::Bar) return;
  // A floating home in a region shell: along the centre region's lower edge, tiled with the others.
  ImVec2 fpos, fsize;
  bool retile = false;
  const bool floatHome = layoutui::floatHome(a, window, firstSize, &fpos, &fsize, &retile);
  if (!floatHome) {
    // Floating the first time: cascaded from the main window's upper left, so panels opened together
    // do not cover each other's title bars (a docked panel ignores it).
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float step = ImGui::GetFrameHeight() + theme::kS2;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.18f + step * static_cast<float>(index),
                                   vp->WorkPos.y + vp->WorkSize.y * 0.12f + step * static_cast<float>(index)),
                            ImGuiCond_FirstUseEver);
  }
  const bool vis = ui::BeginDockWindow(window, open, floatHome ? fsize : firstSize);
  // At its home each time it opens, and kept there while the floating parts are tiled again (the
  // centre changed size, a side region opened, another floating part opened or closed), until the
  // person moves or resizes it: then it stays where they put it until it is closed.
  if (floatHome && !ImGui::IsWindowDocked()) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    // Moved means the window itself left the place given to it last (its title bar or empty space
    // dragged, an edge or corner pulled); working a control inside it (a slider dragged) is not.
    struct Placed {
      bool moved = false;
      ImVec2 pos, size;
    };
    static std::map<std::string, Placed> placed;
    Placed& pl = placed[window];
    const auto differs = [](ImVec2 a, ImVec2 b) { return std::fabs(a.x - b.x) > 0.5f || std::fabs(a.y - b.y) > 0.5f; };
    if (ImGui::IsWindowAppearing()) pl.moved = false;
    // Only with the mouse held: the window settling its own size the frames after it appears is not a move.
    else if (!pl.moved && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (differs(w->Pos, pl.pos) || differs(w->SizeFull, pl.size)))
      pl.moved = true;
    const bool off = differs(w->Pos, fpos) || differs(w->SizeFull, fsize);
    if (ImGui::IsWindowAppearing() || (!pl.moved && (retile || off))) {
      ImGui::SetWindowPos(fpos);
      ImGui::SetWindowSize(fsize);
    }
    pl.pos = w->Pos;
    pl.size = w->SizeFull;
  }
  if (vis) {
    const ImGuiWindow* pw = ImGui::GetCurrentWindow();
    gOwnTitle = pw->DockIsActive && pw->DockNode && (pw->DockNode->LocalFlags & ImGuiDockNodeFlags_NoTabBar) != 0;
    // K-106: the tracker parts draw edge to edge (their own frames and boxes).
    // (cp24: the lower band too, the Device chain in the Tracker: its screens draw their own boxes; its
    // 8 px padding left a 16 px strip of empty band round the Channel frame.)
    const bool edge = std::strcmp(window, "Pattern editor") == 0 || std::strcmp(window, "Order list") == 0 ||
                      (p == layout::Preset::Tracker && std::strcmp(window, "Device chain") == 0);
    const float padPart = edge ? 2.0f : theme::kS2;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padPart, padPart));
    ImGui::BeginChild("##part", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    body();
    ImGui::EndChild();
  }
  ui::EndDockWindow();
}

std::string tickText(App& a, Tick t) {
  if (logicplug::active(a)) {
    // R01-26: the Tracks layout writes a position as the reference's lists and inspector do: bar,
    // beat, division (a sixteenth) and tick, each counted from 1 ("1 1 1 1" is the song's start).
    const Song& s = a.doc.song();
    const MeterMap mm(s);
    const MeterMap::Bbt b = mm.toBbt(std::max<Tick>(0, t));
    const Tick div = std::max<Tick>(1, static_cast<Tick>(s.ppq) / 4);
    char buf[96];  // room for three 64-bit fields, an int and spaces (GCC format-truncation)
    std::snprintf(buf, sizeof buf, "%lld %d %lld %lld", static_cast<long long>(b.bar + 1), b.beat + 1, static_cast<long long>(b.tick / div + 1),
                  static_cast<long long>(b.tick % div + 1));
    return buf;
  }
  return menuparity::positionText(a, static_cast<double>(t));
}

const MixerTrack* chainTrack(App& a, ObjectId channel) {
  const Song& s = a.doc.song();
  if (a.fxEditor.first != kNoId)
    if (const MixerTrack* t = s.mixerTrack(a.fxEditor.first)) return t;
  if (const Channel* c = s.channel(channel)) {
    if (const MixerTrack* t = s.mixerTrack(c->mixerTrack)) return t;
    return s.mixer.empty() ? nullptr : &s.mixer[0];  // routed to the master
  }
  if (const MixerTrack* t = s.mixerTrack(a.selMixer)) return t;
  return nullptr;
}

// ---------------------------------------------------------------- Inspector ----
// Cards (K-112b): the selected region (clip), its track and the track's chain, then two channel
// strips at the bottom: the sound's mixer track and where it goes. A linked track (an instrument
// or audio track) shows its own chain; a free track shows the chain of the selected region's
// sound, with a chooser when the region's pattern plays several channels (as in Studio, nothing
// is linked or split by looking).

// A card's heading (bold, the panel's own title when the window draws no tab).
void cardHeading(const std::string& text) {
  App& a = app();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = 22;
  theme::textEllipsis(ImGui::GetWindowDrawList(), a.fontBold, ImGui::GetFontSize(), ImVec2(p.x + 2, std::round(p.y + (h - ImGui::GetFontSize()) * 0.5f)), w - 4,
                      pal().text, text.c_str());
  ImGui::Dummy(ImVec2(w, h));
}

// K-112b (Tracks layout): an open card's rows sit in an inset rounded box, a thin line between
// rows, the labels in the text colour and the values in the body font, as the reference
// inspectors (110, 092) draw them. cardBegin / cardEnd bracket the rows.
struct CardBox {
  bool on = false;
  ImVec2 p0;
  float w = 0;
  int rows = 0;
};
CardBox gCard;
void cardBegin() {
  App& a = app();
  if (!logicplug::active(a)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  gCard = CardBox{true, ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, 0};
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::Indent(8);
  ImGui::PushItemWidth(-8);
}
void cardEnd() {
  if (!gCard.on) return;
  ImGui::PopItemWidth();
  ImGui::Unindent(8);
  ImGui::Dummy(ImVec2(0, 2));
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSetCurrent(0);
  const float y1 = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
  dl->AddRectFilled(gCard.p0, ImVec2(gCard.p0.x + gCard.w, y1), theme::mix(P.surface800, P.surface700, 0.5f), 8.0f);
  dl->ChannelsMerge();
  gCard = CardBox{};
}
// A row's top line inside a card (none above the first).
void cardRowLine(ImDrawList* dl, const ImVec2& p, float w) {
  if (!gCard.on) return;
  if (gCard.rows++ > 0) dl->AddLine(ImVec2(p.x, std::round(p.y - ImGui::GetStyle().ItemSpacing.y * 0.5f)), ImVec2(p.x + w, std::round(p.y - ImGui::GetStyle().ItemSpacing.y * 0.5f)),
                                    theme::withAlpha(pal().line, 0.7f));
}

// A card row: the label dim on the left, the value on the right. A clickable row returns its click.
bool cardRow(const char* id, const char* label, const std::string& value, const char* hint, bool clickable = false) {
  App& a = app();
  const auto& P = pal();
  const float rowH = std::max(18.0f, layoutui::style().inspectorRowH);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // D8: in a card the row keeps the card's 8 px inset on the right too (values ended 2 px from its edge)
  const float w = ImGui::GetContentRegionAvail().x - (gCard.on ? 8.0f : 0.0f);
  bool click = false, hov = false;
  if (clickable) {
    click = ImGui::InvisibleButton(id, ImVec2(w, rowH));
    hov = ImGui::IsItemHovered();
  } else {
    ImGui::Dummy(ImVec2(w, rowH));
    hov = ImGui::IsItemHovered();
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (clickable && hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), theme::withAlpha(P.paper, 0.05f), theme::kRadiusSm);
  const float fs = ImGui::GetFontSize(), ty = std::round(p.y + (rowH - fs) * 0.5f);
  cardRowLine(dl, p, w);
  ImFont* vf = gCard.on ? a.fontRegular : a.fontMono;  // in a card: the body font, as the references
  const float vw = std::min(vf->CalcTextSizeA(fs, 1e9f, 0, value.c_str()).x, w * 0.6f);
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(p.x + 2, ty), w - vw - 10, gCard.on ? P.text : P.textDim, i18n::tr(label));
  theme::textEllipsis(dl, vf, fs, ImVec2(p.x + w - vw - 2, ty), vw, clickable && hov ? P.hot : gCard.on ? P.textDim : P.text, value.c_str());
  if (hint && hov) ui::Hint(hint);
  return click;
}

std::string clipTitle(const Song& s, const Clip& c) {
  if (!c.name.empty() && logicplug::active(app())) return c.name;  // R08-06: the region's own name, as its name bar shows it
  if (c.kind == ClipKind::Pattern) {
    const Pattern* p = s.pattern(c.pattern);
    return p ? p->name : i18n::tr("Pattern");
  }
  if (c.kind == ClipKind::Audio) {
    const Channel* ch = s.channel(c.source);
    return ch ? ch->name : i18n::tr("Audio");
  }
  const AutomationClip* au = s.automation(c.source);
  return au ? au->name : i18n::tr("Automation");
}

std::string dbText(float gain) {
  if (gain <= 0.00001f) return "-inf";
  char b[24];
  double db = 20.0 * std::log10(static_cast<double>(gain));
  if (std::fabs(db) < 0.05) db = 0.0;  // R09-08: a level that rounds to zero reads "0.0", never "-0.0"
  std::snprintf(b, sizeof b, "%.1f", db);
  return b;
}

// D9 (R14-08): set by the inspector before its first heading: that row is the pane's header (see paneTitleY).
bool gHeadFirst = false;

// A card's foldable heading: a disclosure arrow and the bold title; a click folds or unfolds it.
bool cardFold(const char* id, const std::string& text, bool* open) {
  App& a = app();
  const auto& P = pal();
  // Checkpoint 37: in the Tracks layout a heading is a 34 px row with its line on the row's foot and the
  // next row straight under it, as 216 and 125 stack Region and Track (22 px with gaps around: a 44 px pitch).
  const bool lg = logicplug::active(a);
  // D9 (R14-08): the first one is the pane's header, in the arrange toolbar's band: its title on the
  // toolbar's centre line and its foot line on the toolbar's (it sat 8 px lower, below the pane's padding).
  const bool head = lg && gHeadFirst;
  gHeadFirst = false;
  if (head) {
    ImGui::SetCursorPosY(kPaneHeadTop);
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = lg ? (head ? kPaneHeadH : 34.0f) : 22.0f;
  if (ImGui::InvisibleButton(id, ImVec2(w, h))) *open = !*open;
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cy = std::round(p.y + h * 0.5f);
  if (lg) {  // K-112b: as the reference inspectors (110, 092)
    // (the header: its capitals centred on the toolbar's, a pixel above the row's middle: paneTitleY)
    const float fs = ImGui::GetFontSize(), ty = head ? paneTitleY(fs) - ImGui::GetWindowPos().y + p.y - kPaneHeadTop : std::round(p.y + (h - fs) * 0.5f);
    const size_t c = text.find(": ");
    const std::string lead = c == std::string::npos ? text : text.substr(0, c + 1), rest = c == std::string::npos ? std::string() : text.substr(c + 2);
    const float lw = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, lead.c_str()).x;
    dl->AddText(a.fontBold, fs, ImVec2(p.x + 2, ty), P.text, lead.c_str());
    if (!rest.empty()) theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(std::round(p.x + 2 + lw + 4), ty), w - lw - 26, P.textDim, rest.c_str());
    const float ax = std::round(p.x + w - 9);
    ui::drawIcon(dl, *open ? ui::Icon::ChevronDown : ui::Icon::Chevron, ImVec2(ax, cy), 9, hov ? P.text : P.textDim);
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h - 1), theme::withAlpha(P.text, 0.04f));
    dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f), pal().line);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    if (*open) ImGui::Dummy(ImVec2(0, 2));
    return *open;
  }
  if (*open) ui::drawIconFit(dl, ui::Icon::Down, ImVec2(p.x + 3, cy - 2), ImVec2(p.x + 10, cy + 3), hov ? P.text : P.textDim);
  else ui::drawIconFit(dl, ui::Icon::Right, ImVec2(p.x + 4, cy - 4), ImVec2(p.x + 9, cy + 4), hov ? P.text : P.textDim);
  theme::textEllipsis(dl, a.fontBold, ImGui::GetFontSize(), ImVec2(p.x + 15, std::round(p.y + (h - ImGui::GetFontSize()) * 0.5f)), w - 17, P.text,
                      text.c_str());
  return *open;
}

// A stepper row: the label dim on the left, the value on the right with up / down arrows. Drag the
// value up or down, click an arrow for one step, double-click for the default. True when changed.
bool cardStepper(const char* id, const char* label, int* v, int lo, int hi, int dflt, const std::string& shown, const char* hint) {
  App& a = app();
  const auto& P = pal();
  const float rowH = std::max(18.0f, layoutui::style().inspectorRowH);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x - (gCard.on ? 8.0f : 0.0f);  // D8: as cardRow
  ImGui::PushID(id);
  const float arrowW = 12;
  ImGui::InvisibleButton("##v", ImVec2(w - arrowW, rowH));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();  // the drag's edits are one undo step
  const int before = *v;
  static float acc = 0;
  if (ImGui::IsItemActivated()) acc = 0;
  if (act && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
    acc -= ImGui::GetIO().MouseDelta.y / 4.0f;
    const int steps = static_cast<int>(acc);
    if (steps != 0) *v = std::clamp(*v + steps, lo, hi), acc -= static_cast<float>(steps);
  }
  if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) *v = dflt, a.doc.endGesture();
  ImGui::SameLine(0, 0);
  const ImVec2 ap = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##up", ImVec2(arrowW, rowH * 0.5f));
  if (ImGui::IsItemClicked()) *v = std::min(hi, *v + 1), a.doc.endGesture();
  const bool hovUp = ImGui::IsItemHovered();
  ImGui::SetCursorScreenPos(ImVec2(ap.x, ap.y + rowH * 0.5f));
  ImGui::InvisibleButton("##dn", ImVec2(arrowW, rowH * 0.5f));
  if (ImGui::IsItemClicked()) *v = std::max(lo, *v - 1), a.doc.endGesture();
  const bool hovDn = ImGui::IsItemHovered();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov || act) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), theme::withAlpha(P.paper, 0.05f), theme::kRadiusSm);
  const float fs = ImGui::GetFontSize(), ty = std::round(p.y + (rowH - fs) * 0.5f);
  cardRowLine(dl, p, w);
  ImFont* vf = gCard.on ? a.fontRegular : a.fontMono;
  const float vw = std::min(vf->CalcTextSizeA(fs, 1e9f, 0, shown.c_str()).x, w * 0.55f);
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(p.x + 2, ty), w - vw - arrowW - 10, gCard.on ? P.text : P.textDim, i18n::tr(label));
  theme::textEllipsis(dl, vf, fs, ImVec2(p.x + w - arrowW - vw - 3, ty), vw, act ? P.hot : *v != dflt ? P.text : P.textDim, shown.c_str());
  const float ax = ap.x + arrowW * 0.5f, cy = p.y + rowH * 0.5f;
  ui::drawIconFit(dl, ui::Icon::Up, ImVec2(ax - 3, cy - 6), ImVec2(ax + 3, cy - 2), hovUp ? P.hot : P.textFaint);
  ui::drawIconFit(dl, ui::Icon::Down, ImVec2(ax - 3, cy + 2), ImVec2(ax + 3, cy + 6), hovDn ? P.hot : P.textFaint);
  if (hint && (hov || hovUp || hovDn)) ui::Hint(hint);
  return *v != before;
}

std::string signedText(long long v, const char* unit) {
  std::string t = v > 0 ? "+" + std::to_string(v) : std::to_string(v);
  if (unit && *unit) t += std::string(" ") + unit;
  return t;
}

// A strip slot: a small rounded bar with a device's name (empty: a faint bar); a click opens it.
// fill: a coloured slot (the instrument, as the reference strips colour it apart from the effects).
// The x where a slot's name starts: centred in [x, x + room] (every reference strip centres its
// slot names), or at x when it does not fit (then it is cut with an ellipsis).
float centredX(ImFont* f, float fs, const char* text, float x, float room) {
  const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, text).x;
  return tw < room ? std::round(x + (room - tw) * 0.5f) : x;
}

// Checkpoint 47 (R07-11): an untouched mixer track: no name, no effect, nothing plays or sends into
// it. A new reference project has no bus in use until one is picked, so these are not listed; New bus
// takes the first of them (else a new mixer track).
// R10-10: the bus the inspector's right strip shows for a track, after a send to it was made or
// clicked (the reference shows the send's aux there); with none, the strip the track goes out to.
std::map<ObjectId, ObjectId> gInspDest;

bool unusedBus(const Song& s, size_t k) {
  if (k == 0 || k >= s.mixer.size()) return false;
  const MixerTrack& m = s.mixer[k];
  if (!m.name.empty()) return false;
  for (const EffectSlot& e : m.slots)
    if (!e.plugin.type.empty()) return false;
  for (const Channel& c : s.channels)
    if (c.mixerTrack == m.id) return false;
  if (trackmodes::audioTrackOf(s, m.id) >= 0) return false;
  for (const MixerTrack& o : s.mixer)
    for (const Send& sd : o.sends)
      if (sd.to == m.id && o.id != m.id) return false;
  return true;
}

// R05-14: the Tracks layout's Sends and Output slots open a list of where the track can go, as the
// reference's: Sends add a send to a bus; Output moves the track's main route (one undo step).
void routeMenu(App& a, const char* popup, const MixerTrack& t, bool output) {
  if (!ImGui::BeginPopup(popup)) return;
  const Song& s = a.doc.song();
  ObjectId cur = kNoId;
  for (const Send& sd : t.sends)
    if (!sd.sidechain && cur == kNoId) cur = sd.to;
  ImGui::TextDisabled("%s", i18n::tr(output ? "Output" : "Send to bus"));
  for (size_t k = 0; k < s.mixer.size(); ++k) {
    const MixerTrack& m = s.mixer[k];
    if (m.id == t.id || (!output && k == 0) || unusedBus(s, k)) continue;
    // R06-02: every bus, used or not, as the reference lists them: a mixer track no channel plays
    // into directly (a channel's own strip is not a bus), that the track can reach without a loop.
    bool bus = k == 0;
    if (!bus) {
      bool fed = false;
      for (const Channel& c : s.channels) fed = fed || c.mixerTrack == m.id;
      fed = fed || trackmodes::audioTrackOf(s, m.id) >= 0;  // an audio track's own strip is not a bus either
      bus = !fed && (canRoute(s, t.id, m.id) || std::any_of(t.sends.begin(), t.sends.end(), [&](const Send& sd) { return sd.to == m.id; }));
    }
    if (!bus) continue;
    bool has = false;
    for (const Send& sd : t.sends) has = has || (sd.to == m.id && !sd.sidechain);
    // A send is any route but the main one (the output): the output's bus is not ticked as a send.
    const bool isOut = m.id == cur || (cur == kNoId && k == 0);
    const bool on = output ? isOut : has && !isOut;
    // R08-09 / R09-07: a bus made by New bus is listed by its bus number ("Bus 1"), its strip named
    // "Aux 1", as the reference's; the send slot and the send's automation read the same (busName).
    const std::string label = busName(s, k);
    ImGui::PushID(static_cast<int>(m.id));
    const bool picked = ImGui::MenuItem(label.c_str(), nullptr, on, output || (!has && !isOut));
    ImGui::PopID();
    if (picked) {
      if (output) {
        float lvl = 1.0f;
        for (const Send& sd : t.sends)
          if (sd.to == cur) lvl = sd.level;
        const ObjectId from = t.id, to = m.id, old = cur;
        a.doc.beginGroup("track output");
        if (old != kNoId && old != to) a.doc.setSend(from, old, 0.0f);
        if (!(k == 0 && old == kNoId)) a.doc.setSend(from, to, lvl);
        a.doc.endGroup(false);
        gInspDest.erase(t.id);
      } else {
        a.doc.setSend(t.id, m.id, 0.5f);
        gInspDest[t.id] = m.id;
      }
      break;  // the song changed: stop walking it
    }
  }
  // R06-02: a new bus, as choosing an unused bus in the reference creates its aux strip.
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("New bus"))) {
    const ObjectId from = t.id;
    a.later([&a, from, output] {
      a.doc.beginGroup(output ? "track output" : "add send");
      ObjectId bus = kNoId;
      for (size_t k = 1; k < a.doc.song().mixer.size() && bus == kNoId; ++k)
        if (unusedBus(a.doc.song(), k) && a.doc.song().mixer[k].id != from) bus = a.doc.song().mixer[k].id;
      if (bus == kNoId) bus = a.doc.addMixerTrack();
      if (bus != kNoId) {
        int aux = 1;  // R07-11: the new strip is "Aux N", as the reference names it
        for (const MixerTrack& m : a.doc.song().mixer)
          if (m.name.rfind("Aux ", 0) == 0) aux = std::max(aux, std::atoi(m.name.c_str() + 4) + 1);
        a.doc.editMixerTrack(bus, "name bus", [aux](MixerTrack& m) { m.name = "Aux " + std::to_string(aux); });
        if (output) {
          ObjectId old = kNoId;
          float lvl = 1.0f;
          if (const MixerTrack* f = a.doc.song().mixerTrack(from))
            for (const Send& sd : f->sends)
              if (!sd.sidechain && old == kNoId) old = sd.to, lvl = sd.level;
          if (old != kNoId) a.doc.setSend(from, old, 0.0f);
          a.doc.setSend(from, bus, lvl);
          gInspDest.erase(from);
        } else {
          a.doc.setSend(from, bus, 0.5f);
          gInspDest[from] = bus;
        }
      }
      a.doc.endGroup(false);
    });
  }
  ImGui::EndPopup();
}

// K-112D (audit F9): a plugin's name too long for its pill is shortened word by word, as the
// reference abbreviates its plugin names to fit ("Param EQ", "Comp"), before the ellipsis is needed.
// Written into `buf` (no allocation); the name as it is when it fits.
// D5 (R11-08, visual gap 7): after the table, the shortest-to-read form that fits is chosen among
// every way of dropping inner vowels from the longer words (4 letters and up; short words stay whole)
// and dropping trailing words (at least one stays): a vowel dropped costs 1, a run of four consonants
// or more 1 more, a word left with two letters 2 more; a trailing word dropped 2.9 ("Bus", "Aux" and the
// like 2.5, a word after "+" 4). So "Music Bus" reads "Msc Bus" and "Shaker + Ride" "Shkr + Ride", but
// "Graphite Mallet" reads "Graphite", not "Grpht Mallet". The answers are kept (a
// small table keyed by the name, the room and the size), so a strip measures its names once.
namespace {
struct FitMemo {
  uint64_t h = 0;
  float w = 0, fs = 0;
  const ImFont* f = nullptr;
  std::string name, out;
  bool same = false;
};
FitMemo gFitMemo[512];
uint64_t fitHash(const char* s, float w, float fs) {
  uint64_t h = 1469598103934665603ull;
  for (const char* c = s; *c; ++c) h = (h ^ static_cast<unsigned char>(*c)) * 1099511628211ull;
  h = (h ^ static_cast<uint64_t>(std::lround(w * 4))) * 1099511628211ull;
  return (h ^ static_cast<uint64_t>(std::lround(fs * 4))) * 1099511628211ull;
}
const char* copyOut(const std::string& s, char* buf, size_t n) {  // whole when it fits the buffer (64 or more)
  const size_t len = std::min(s.size(), n - 1);
  std::memcpy(buf, s.data(), len);
  buf[len] = 0;
  return buf;
}
bool fitVowel(char ch) { return ch == 'a' || ch == 'e' || ch == 'i' || ch == 'o' || ch == 'u'; }
bool genericWord(const std::string& w) {
  static const char* const kGeneric[] = {"Bus", "Aux", "Track", "Send", "Group", "FX", "Return", "Out"};
  for (const char* g : kGeneric)
    if (w == g) return true;
  return false;
}
// The best form of `words` that fits `w`, or "" when none does.
std::string bestFit(ImFont* f, float fs, const std::vector<std::string>& words, float w) {
  const int n = static_cast<int>(words.size());
  if (n == 0 || n > 6) return std::string();
  std::string best;
  float bestCost = 1e9f;
  for (int keep = n; keep >= 1; --keep) {
    float dropCost = 0;
    bool joined = false;  // a word after "+" or "&" names half of what the track holds: dearer to drop
    for (int k = 0; k < n; ++k) {
      const bool symbol = !words[static_cast<size_t>(k)].empty() && !std::isalnum(static_cast<unsigned char>(words[static_cast<size_t>(k)][0]));
      if (k >= keep) dropCost += symbol ? 0.0f : joined ? 4.0f : genericWord(words[static_cast<size_t>(k)]) ? 2.5f : 2.9f;
      joined = joined || symbol;
    }
    int kept = keep;  // a kept part never ends in a lone symbol ("Shaker +")
    while (kept > 1 && !std::isalnum(static_cast<unsigned char>(words[static_cast<size_t>(kept - 1)][0]))) --kept;
    std::vector<int> elig;
    for (int k = 0; k < kept; ++k) {
      const std::string& x = words[static_cast<size_t>(k)];
      int v = 0;
      for (size_t i = 1; i < x.size(); ++i) v += fitVowel(x[i]) ? 1 : 0;
      if (x.size() >= 4 && v > 0) elig.push_back(k);
    }
    for (int mask = 0; mask < (1 << elig.size()); ++mask) {
      float cost = dropCost;
      std::string s;
      for (int k = 0; k < kept; ++k) {
        const std::string& x = words[static_cast<size_t>(k)];
        bool strip = false;
        for (size_t e = 0; e < elig.size(); ++e) strip = strip || (elig[e] == k && ((mask >> e) & 1));
        if (!s.empty()) s += ' ';
        if (!strip) {
          s += x;
          continue;
        }
        int run = 0, longest = 0, len = 0;
        for (size_t i = 0; i < x.size(); ++i) {
          if (i > 0 && fitVowel(x[i])) {
            cost += 1.0f;
            continue;
          }
          s += x[i];
          ++len;
          const bool cons = std::isalpha(static_cast<unsigned char>(x[i])) && !fitVowel(static_cast<char>(std::tolower(static_cast<unsigned char>(x[i]))));
          run = cons ? run + 1 : 0;
          longest = std::max(longest, run);
        }
        if (longest >= 4) cost += 1.0f;
        if (len <= 2) cost += 2.0f;  // a word left as two letters (Ride: Rd) hardly reads
      }
      if (cost < bestCost && f->CalcTextSizeA(fs, 1e9f, 0, s.c_str()).x <= w) best = s, bestCost = cost;
    }
  }
  return best;
}
}  // namespace

const char* fitSlotName(ImFont* f, float fs, const char* name, float w, char* buf, size_t n) {
  const uint64_t h = fitHash(name, w, fs);
  FitMemo& memo = gFitMemo[h % 512];
  if (memo.h == h && memo.f == f && memo.w == w && memo.fs == fs && memo.name == name) {
    if (memo.same) return name;
    return copyOut(memo.out, buf, n);
  }
  memo.h = h, memo.f = f, memo.w = w, memo.fs = fs, memo.name = name, memo.same = false;
  if (f->CalcTextSizeA(fs, 1e9f, 0, name).x <= w) {
    memo.same = true;
    return name;
  }
  static const std::pair<const char*, const char*> kShort[] = {
      {"Parametric", "Param"}, {"Compressor", "Comp"}, {"Sidechain", "SC"},      {"Stereo", "St."},     {"Distortion", "Dist"},
      {"Expander", "Exp"},     {"Equalizer", "EQ"},    {"Saturation", "Sat"},    {"Saturator", "Sat"},  {"Multiband", "MB"},
      {"Transient", "Trans"},  {"Analog", "An."},      {"Reverb", "Verb"},       {"Delay", "Dly"},      {"Chorus", "Chor"},
      {"Imager", "Img"},       {"Filter", "Filt"},     {"Maximizer", "Max"},     {"Limiter", "Lim"},    {"Enhancer", "Enh"},
      {"Ducker", "Duck"}};
  std::vector<std::string> words;
  for (const char* c = name; *c;) {
    while (*c == ' ') ++c;
    const char* e = c;
    while (*e && *e != ' ') ++e;
    if (e == c) break;
    const size_t len = static_cast<size_t>(e - c);
    std::string word(c, len);
    for (const auto& [from, to] : kShort)
      if (std::strlen(from) == len && std::strncmp(c, from, len) == 0) word = to;
    words.push_back(std::move(word));
    c = e;
  }
  std::string out;
  for (const std::string& x : words) out += (out.empty() ? "" : " ") + x;
  if (f->CalcTextSizeA(fs, 1e9f, 0, out.c_str()).x > w) {
    std::string b = bestFit(f, fs, words, w);
    if (b.empty()) {  // nothing fits whole: the first word, its vowels gone (the ellipsis after it)
      std::string x = words.empty() ? std::string() : words[0];
      b.clear();
      for (size_t i = 0; i < x.size(); ++i)
        if (i == 0 || !fitVowel(x[i])) b += x[i];
    }
    out = b;
  }
  memo.out = out;
  return copyOut(out, buf, n);
}

// D5 (R11-08): room taken off a name's fit (it shortened every mixer name for a 64 px strip). D6 (R12-09):
// 0 everywhere now: names fit the strip they are on.
float gFitExcess = 0;

// D4 (R10-04): a std::string front for fitSlotName (the plates and slots that hold track names).
std::string fitName(ImFont* f, float fs, const std::string& name, float w) {
  char b[96];
  return fitSlotName(f, fs, name.c_str(), w, b, sizeof b);
}

// D9 (R14-11): a slot's name size in Smooth: a name a little too long for the slot steps down a pixel
// before it is shortened (as the name plates do), so "Parametric EQ" and "Analog Synth" read whole on a
// 74 px strip (2560 x 1440) instead of "Param EQ" and "An. Synth". Elsewhere the label size.
float slotNameSize(App& a, const char* name, float room) {
  const float fs = theme::kFontLabel;
  if (!logicplug::active(a) || a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, name).x <= room) {
    return fs;
  }
  return a.fontRegular->CalcTextSizeA(fs - 1, 1e9f, 0, name).x <= room ? fs - 1 : fs;
}

// D8: an empty slot's face. In Smooth a light veil over whatever is under it, so an empty slot shows the
// same on the selected strip (a step lighter) as on the others; the old fill (the slots' grey at 40 %)
// matched the selected strip's face and vanished there. Elsewhere the grey as before.
ImU32 emptyFace(bool hov) {
  const auto& P = pal();
  if (logicplug::active(app())) {
    return theme::withAlpha(P.text, hov ? 0.08f : 0.035f);
  }
  return hov ? P.surface700 : theme::withAlpha(P.surface700, 0.4f);
}

bool stripSlot(const char* id, const std::string& name, bool lit, bool empty, float w, const char* hint, ImU32 fill = 0) {
  App& a = app();
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 17;
  const bool click = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool coloured = fill != 0 && !empty;
  const ImU32 face = coloured ? (hov ? theme::mix(fill, pal().paper, 0.12f) : fill)
                              : empty ? emptyFace(hov)
                              : hov ? (layoutui::active() == layout::Preset::Logic ? theme::mix(P.surface700, P.text, 0.08f) : P.surface600)  // K-112b: the quiet hover
                                    : P.surface700;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), face, 4.0f);
  if (lit) dl->AddRect(p, ImVec2(p.x + w, p.y + h), P.hot, 4.0f);
  const float fs = theme::kFontLabel;
  char shortBuf[64];
  // D4 (R10-04): every named slot in Smooth (sends, outputs, groups too), not only the coloured ones
  const float nfs = empty ? fs : slotNameSize(a, name.c_str(), w - 10 - gFitExcess);  // D9 (R14-11)
  const char* shown = logicplug::active(a) && !empty ? fitSlotName(a.fontRegular, nfs, name.c_str(), w - 10 - gFitExcess, shortBuf, sizeof shortBuf) : name.c_str();
  theme::textEllipsis(dl, a.fontRegular, nfs, ImVec2(centredX(a.fontRegular, nfs, shown, p.x + 5, w - 10), std::round(p.y + (h - fs) * 0.5f) - 1), w - 10,
                      coloured ? pal().paper : empty ? P.textFaint : P.text, shown);
  if (hint && hov) ui::Hint(shown != name.c_str() ? name + "\n" + hint : std::string(hint));  // D4: a shortened name in full
  return click;
}

// K-112b: the strip's effect slots behave as the reference strips do. A click on a pill opens its
// window (again: closes it). Hovering shows a bypass switch at the left and a menu arrow at the
// right. Alt-click bypasses; a press on the bypass switch dragged across other pills sets them all
// the same. Dragging a pill moves the effect, onto another slot or strip; Alt (or Ctrl) copies.
// An empty slot offers the effect list.
struct StripSlotDrag {  // the mixer's slot payload ("GL_SLOT"), the same layout
  ObjectId track;
  int slot;
};
int gBypassPaint = -1;        // while a bypass press drags across pills: the state they all get
int gBypassPaintEnd = -1;     // the frame that press ended (its release opens nothing)

void openSlot(App& a, ObjectId track, int k) {
  if (a.fxEditor == std::make_pair(track, k)) {
    a.fxEditor = {kNoId, -1};
    return;
  }
  togglePluginWindow(a, PluginTarget{track, k});
}

void slotDrop(App& a, ObjectId track, int k) {
  if (!ImGui::BeginDragDropTarget()) return;
  if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_SLOT")) {
    const StripSlotDrag from = *static_cast<const StripSlotDrag*>(pl->Data);
    const bool copy = ImGui::GetIO().KeyAlt || ImGui::GetIO().KeyCtrl;
    if (!(from.track == track && from.slot == k) && !mixops::moveSlotTo(a.doc, from.track, from.slot, track, k, copy))
      a.notify("No room: every effect slot below that one is taken", true);
  }
  ImGui::EndDragDropTarget();
}

void fxPill(App& a, const MixerTrack& t, int k, float w) {
  const auto& P = pal();
  const EffectSlot& e = t.slots[static_cast<size_t>(k)];
  const PluginInfo* info = findPlugin(e.plugin.type);
  const std::string name = info ? info->name : e.plugin.type;
  const ImGuiIO& io = ImGui::GetIO();
  if (gBypassPaint >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) gBypassPaint = -1, gBypassPaintEnd = ImGui::GetFrameCount();
  ImGui::PushID(k);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 17;
  const bool click = ImGui::InvisibleButton("##fx", ImVec2(w, h));
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##slotmenu");  // R03-07: a right-click gives the slot's menu
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  const bool inBypass = hov && io.MousePos.x < p.x + 15, inMenu = hov && io.MousePos.x > p.x + w - 13;
  const ParamId en = slotEnableParam(t.id, k);
  if (ImGui::IsItemActivated() && inBypass) {  // the bypass switch: and every pill the drag crosses
    gBypassPaint = e.enabled ? 0 : 1;
    a.doc.setParam(en, gBypassPaint, false);
  } else if (gBypassPaint >= 0 && hov && (e.enabled ? 1 : 0) != gBypassPaint) {
    a.doc.setParam(en, gBypassPaint, false);
  }
  if (click && gBypassPaintEnd != ImGui::GetFrameCount() && !inBypass) {
    if (io.KeyAlt) a.doc.setParam(en, e.enabled ? 0 : 1, false);
    else if (inMenu) ImGui::OpenPopup("##slotmenu");
    else openSlot(a, t.id, k);
  }
  if (gBypassPaint < 0 && ImGui::BeginDragDropSource()) {
    const StripSlotDrag sd{t.id, k};
    ImGui::SetDragDropPayload("GL_SLOT", &sd, sizeof sd);
    ui::DragPreview(ui::Icon::Mixer, (name + "  (Alt: copy)").c_str());
    ImGui::EndDragDropSource();
  }
  slotDrop(a, t.id, k);
  if (ImGui::BeginPopup("##slotmenu")) {
    if (ImGui::BeginMenu(i18n::tr("Replace"))) {
      effectPickerList(a, t.id, k, e.plugin.type);
      ImGui::EndMenu();
    }
    ImGui::Separator();
    mixerSlotMenu(a, t.id, k);
    ImGui::EndPopup();
  }
  const bool lit = a.fxEditor == std::make_pair(t.id, k) || hostedEditorOpen(PluginTarget{t.id, k});
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // A filled slot reads as filled: a blue pill (lighter under the pointer); bypassed, a dim grey one.
  const ImU32 blue = theme::rgb(kSlotBlue);
  const ImU32 face = !e.enabled ? theme::withAlpha(P.surface700, 0.5f) : hov ? theme::mix(blue, pal().paper, 0.15f) : blue;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), face, 4.0f);
  if (lit) dl->AddRect(p, ImVec2(p.x + w, p.y + h), P.hot, 4.0f);
  const float fs = theme::kFontLabel;
  // D7 (R13-07): the name has the pill's full width (a 2 px inset each side), so "Parametric EQ" reads
  // whole on a 74 px strip; the hover icons take their room only while shown.
  float tx = p.x + 2, tw = w - 4;
  if (hov) {
    const ImU32 on = pal().paper, dim = theme::withAlpha(pal().paper, 170.0f / 255.0f);
    ui::drawIcon(dl, ui::Icon::Power, ImVec2(p.x + 8, p.y + h * 0.5f), 10, e.enabled ? (inBypass ? on : dim) : (inBypass ? P.text : P.textFaint));
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p.x + w - 7, p.y + h * 0.5f), 9, e.enabled ? (inMenu ? on : dim) : (inMenu ? P.text : P.textDim));
    tx = p.x + 16, tw = w - 30;
  }
  char shortBuf[64];  // K-112D (audit F9)
  const float nfs = slotNameSize(a, name.c_str(), tw - gFitExcess);  // D9 (R14-11)
  const char* shown = logicplug::active(a) ? fitSlotName(a.fontRegular, nfs, name.c_str(), tw - gFitExcess, shortBuf, sizeof shortBuf) : name.c_str();
  theme::textEllipsis(dl, a.fontRegular, nfs, ImVec2(centredX(a.fontRegular, nfs, shown, tx, tw), std::round(p.y + (h - fs) * 0.5f) - 1), tw,
                      e.enabled ? pal().paper : P.textFaint, shown);
  if (hov)
    ui::Hint(inBypass ? (e.enabled ? "Bypass (drag across slots to set them all)" : "Turn on (drag across slots to set them all)")
             : inMenu ? "Replace, remove and the slot's options"
                      : "Click: open or close its window · Alt-click: bypass · drag: move (Alt: copy)");
  ImGui::PopID();
}

void emptyPill(App& a, const MixerTrack& t, int k, float w) {
  const auto& P = pal();
  ImGui::PushID(1000 + k);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 17;
  if (ImGui::InvisibleButton("##fxadd", ImVec2(w, h)) && k >= 0) ImGui::OpenPopup("##fxpick");
  if (k >= 0 && ImGui::GetCurrentWindow()->ClipRect.Contains(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())))
    devNoteRect("fxslotempty");  // (dev: an "sdlat" target, the last free slot drawn whole)
  const bool hov = ImGui::IsItemHovered();
  slotDrop(a, t.id, k);
  effectPickerPopup(a, "##fxpick", p, ImVec2(p.x + w, p.y + h), t.id, k, "");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), emptyFace(hov), 4.0f);
  if (hov) {
    const float fs = theme::kFontLabel;
    const char* t = k >= 0 ? i18n::tr("Add an effect") : i18n::tr("No free slot");
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(centredX(a.fontRegular, fs, t, p.x + 5, w - 10), std::round(p.y + (h - fs) * 0.5f) - 1), w - 10, P.textFaint, t);
    ui::Hint(k >= 0 ? "Click: pick an effect for this slot (or drop one here)" : "Every effect slot of this track is taken");
  }
  ImGui::PopID();
}

int firstFree(const MixerTrack& t) {
  for (int k = 0; k < kEffectSlots; ++k)
    if (t.slots[static_cast<size_t>(k)].plugin.type.empty()) return k;
  return -1;
}

// K-112b: the EQ area at the top of a strip: the curve of the track's first EQ effect. A click opens
// it; with none, a click puts one in the first slot when that is free (Alt-click: at the top, the
// others move down); a double-click puts one in the first free slot.
void eqThumb(App& a, const MixerTrack& t, float w) {
  const auto& P = pal();
  int eq = -1;
  for (int k = 0; k < kEffectSlots && eq < 0; ++k)
    if (t.slots[static_cast<size_t>(k)].plugin.type == "gl.eq") eq = k;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 24;
  const bool click = ImGui::InvisibleButton("##eqthumb", ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered(), dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  auto insertAt = [&](int k, bool top) {
    const int free = firstFree(t);
    if (free < 0) {
      a.notify("No free effect slot for an EQ", true);
      return;
    }
    a.doc.beginGroup("insert EQ");
    bool ok = a.doc.setSlotPlugin(t.id, k >= 0 ? k : free, "gl.eq");
    const int at = k >= 0 ? k : free;
    if (ok && top && at != 0) ok = mixops::moveSlotTo(a.doc, t.id, at, t.id, 0, false);
    a.doc.endGroup();
    if (ok) a.fxEditor = {t.id, top ? 0 : at};
  };
  if (eq >= 0) {
    if (click) openSlot(a, t.id, eq);
  } else if (click && ImGui::GetIO().KeyAlt) {
    insertAt(-1, true);
  } else if (click && t.slots[0].plugin.type.empty()) {
    insertAt(0, false);
  } else if (dbl) {
    insertAt(-1, false);
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float mid = std::round(p.y + h * 0.5f);
  // R10-10: in Smooth a strip without an EQ shows the reference's empty EQ display (043, 134): a quiet
  // empty slot like the others with a faint "EQ" in it (110), not a dark well.
  const bool quietEmpty = eq < 0 && logicplug::active(a);
  if (quietEmpty) {
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), emptyFace(hov), 4.0f);
  } else {
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? theme::mix(P.field, P.surface700, 0.5f) : P.field, 3.0f);
    dl->AddLine(ImVec2(p.x + 2, mid), ImVec2(p.x + w - 2, mid), theme::withAlpha(P.line, 0.8f));
  }
  if (eq >= 0) {
    const EffectSlot& e = t.slots[static_cast<size_t>(eq)];
    // D5 (CPU): the curve (each band designed, then measured at every point) is worked out again only
    // when its EQ or the width changes, not every frame; kept as offsets from the thumbnail's corner.
    struct EqCurve {
      std::vector<float> params;
      float w = 0;
      int slot = -1;
      std::vector<ImVec2> pts;
    };
    static std::map<ObjectId, EqCurve> curves;
    EqCurve& cv = curves[t.id];
    if (cv.w != w || cv.slot != eq || cv.params != e.plugin.params) {
      cv.w = w, cv.slot = eq, cv.params = e.plugin.params;
      const double sr = 48000.0;
      EqBandFilters f[pp::kEqBands];
      for (int b = 0; b < pp::kEqBands; ++b) designEqBand(e.plugin, b, sr, f[b]);
      const int n = std::max(8, static_cast<int>(w / 3));
      cv.pts.assign(static_cast<size_t>(n), ImVec2(0, 0));
      for (int i = 0; i < n; ++i) {
        const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / (n - 1));
        double db = 0;
        for (int b = 0; b < pp::kEqBands; ++b)
          if (f[b].count > 0) db += dsp::gainToDb(eqBandMagnitude(f[b], hz, sr));
        cv.pts[static_cast<size_t>(i)] = ImVec2(2 + (w - 4) * static_cast<float>(i) / static_cast<float>(n - 1),
                                                h * 0.5f - static_cast<float>(std::clamp(db, -18.0, 18.0) / 18.0) * (h * 0.5f - 2));
      }
    }
    ImVec2 pts[512];
    const int n = std::min(512, static_cast<int>(cv.pts.size()));
    const float top = mid - h * 0.5f;
    for (int i = 0; i < n; ++i) pts[i] = ImVec2(p.x + cv.pts[static_cast<size_t>(i)].x, top + cv.pts[static_cast<size_t>(i)].y);
    dl->AddPolyline(pts, n, e.enabled ? theme::rgb(theme::role::kGraph) : P.textFaint, 0, 1.5f);
    if (hov) ui::Hint("The track's EQ: click to open it");
  } else {
    const float fs = theme::kFontLabel;
    const char* l = i18n::tr("EQ");
    const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, l).x;
    dl->AddText(a.fontRegular, fs, ImVec2(std::round(p.x + (w - tw) * 0.5f), std::round(mid - fs * 0.5f) - 1), P.textFaint, l);
    if (hov) ui::Hint("No EQ yet: click to put one in the first slot (Alt-click: at the top; double-click: the first free slot)");
  }
}

// K-112D (audit X3): numbers in the interface face with tabular figures, as the reference sets its
// read-outs: every digit takes the widest digit's advance, so values that change keep their place.
float tabWidth(ImFont* f, float fs, const char* t) {
  const float dw = f->CalcTextSizeA(fs, 1e9f, 0, "0").x;
  float w = 0;
  for (const char* c = t; *c; ++c) {
    if (*c >= '0' && *c <= '9') w += dw;
    else {
      const char one[2] = {*c, 0};
      w += f->CalcTextSizeA(fs, 1e9f, 0, one).x;
    }
  }
  return w;
}
void tabText(ImDrawList* dl, ImFont* f, float fs, ImVec2 pos, ImU32 col, const char* t) {
  const float dw = f->CalcTextSizeA(fs, 1e9f, 0, "0").x;
  float x = pos.x;
  for (const char* c = t; *c; ++c) {
    const char one[2] = {*c, 0};
    const float cw = f->CalcTextSizeA(fs, 1e9f, 0, one).x;
    const float adv = *c >= '0' && *c <= '9' ? dw : cw;
    dl->AddText(f, fs, ImVec2(std::round(x + (adv - cw) * 0.5f), pos.y), col, one);
    x += adv;
  }
}

// K-112D (audit E4, F5): Smooth's pan knob as the reference draws it: a dark round face on a faint
// rail, a green arc from the top (centre) to the value, and the value inside ("-16", "0", "+10", on the
// reference's -64 to +63 scale). Drawn into a fixed buffer: nothing allocated per frame.
ImFont* gPanFont = nullptr;
constexpr float kPiF = 3.14159265f;
void smoothPanFace(ImDrawList* dl, ImVec2 c, float r, float norm, bool, bool hov, bool act) {
  const auto& P = pal();
  const float th = 2.5f, ro = r - th * 0.5f;
  constexpr float kTop = -kPiF * 0.5f, kSpan = kPiF * 0.8f;  // 144 degrees each way
  dl->PathArcTo(c, ro, kTop - kSpan, kTop + kSpan, 32);
  dl->PathStroke(theme::mix(P.surface800, P.text, 0.14f), ImDrawFlags_None, th);
  const float an = kTop + (norm - 0.5f) * 2.0f * kSpan;
  const ImU32 green = theme::rgb(0x3FC456);
  if (std::fabs(an - kTop) > 0.02f) {
    dl->PathArcTo(c, ro, std::min(kTop, an), std::max(kTop, an), 24);
    dl->PathStroke(green, ImDrawFlags_None, th);
  } else {
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y - r + th + 2), green, 2.0f);  // centred: a short tick at the top
  }
  const ImU32 face = act ? theme::mix(P.surface900, P.text, 0.16f) : hov ? theme::mix(P.surface900, P.text, 0.10f) : theme::mix(P.surface950, P.text, 0.05f);
  dl->AddCircleFilled(c, r - th - 1.0f, face, 32);
  const int v = static_cast<int>(std::lround((norm - 0.5f) * 128.0f));
  char b[8];
  std::snprintf(b, sizeof b, v > 0 ? "+%d" : "%d", std::clamp(v, -64, 63));
  ImFont* f = gPanFont ? gPanFont : ImGui::GetFont();
  const float fs = r >= 14 ? 10.0f : 9.0f;
  const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0, b);
  dl->AddText(f, fs, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), v == 0 ? P.textDim : P.text, b);
}

// One channel strip, top to bottom as the inspector's strips read: the EQ area, the instrument slot
// (a channel's strip), its effects and an empty slot, sends and output, the pan knob, the level, the
// fader with its meter, mute and solo, and the name plate in the track's colour.
// The rows of a strip (screen y), for the mixer's legend column.
// K-112b (checkpoint 26): the mixer's fader keeps at least this height before the level, pan and a 32 px
// fader give way; the optional rows (icon, setting, EQ, sends) and spare effect rows step out first, so the
// fader stays the strip's main control, as in 134 and 043 (it was 48, about 55 px at 1440x900).
constexpr float kMixFaderMin = 88.0f;
// D4 (R10-04): the share of the mixer pane the fader aims for before the optional rows stay (134 and 043
// give the fader and meter the strip's lower third), and the most it takes on a very tall pane.
constexpr float kMixFaderShare = 0.34f, kMixFaderMax = 320.0f;
constexpr float kMarkGap = theme::kFontLabel + 4;  // the least room between two fader marks

// D4 (R10-04): one set of fader marks for the strips and the legend. The marks are taken by rank (0 dB
// first, then the top and bottom, then the round steps between) and a mark too close to one already
// taken is left out, so 0 always shows and the strips and the legend show the same marks.
// Returns how many, in `db` and `ys` from the top down.
int faderMarks(const ParamDesc& d, ImVec2 pos, ImVec2 size, float minGap, int* db, float* ys, int cap) {
  static constexpr int kRank[] = {0, 6, -60, -20, -10, -40, -6, -30, 3, -3, -15};
  // D5 (CPU): every strip asks for the same marks (one row, one height): the last answer is kept.
  static struct {
    float y = -1, h = -1, gap = -1;
    double max = 0;
    int cap = -1, n = 0;
    int db[16];
    float ys[16];
  } memo;
  if (memo.y == pos.y && memo.h == size.y && memo.gap == minGap && memo.max == d.max && memo.cap == cap && cap <= 16) {
    for (int k = 0; k < memo.n; ++k) db[k] = memo.db[k], ys[k] = memo.ys[k];
    return memo.n;
  }
  int n = 0;
  for (int v : kRank) {
    if (n >= cap) break;
    if (dsp::faderValueForDb(v, 0.8) > d.max + 1e-9) continue;
    const float y = std::round(ui::FaderCapY(pos, size, ui::FaderNormForDb(d, v, 0.8)));
    bool room = true;
    for (int k = 0; k < n && room; ++k) room = std::fabs(ys[k] - y) >= minGap;
    if (!room) continue;
    db[n] = v, ys[n] = y, ++n;
  }
  for (int i = 1; i < n; ++i)  // top down
    for (int j = i; j > 0 && ys[j] < ys[j - 1]; --j) std::swap(ys[j], ys[j - 1]), std::swap(db[j], db[j - 1]);
  if (cap <= 16) {
    memo.y = pos.y, memo.h = size.y, memo.gap = minGap, memo.max = d.max, memo.cap = cap, memo.n = n;
    for (int k = 0; k < n; ++k) memo.db[k] = db[k], memo.ys[k] = ys[k];
  }
  return n;
}
enum StripRow { RowSetting, RowEq, RowInst, RowFx, RowSends, RowOut, RowGroup, RowAuto, RowPan, RowLevel, RowFader, RowMuteSolo, RowName, RowCount };
// K-112b (checkpoint 20): rows the Tracks layout's mixer leaves out when its pane is short, so the
// strips fit without scrolling (dropped in this order: the icon, the settings slot, the EQ thumbnail,
// the sends slot).
// Checkpoint 22: on the smallest panes (150 % at 1440x900) the level readouts, then the pan knob go
// too, and the fader may shorten to 32 px; the effect rows are capped first (see logicMixer).
int gSendRows = 0;  // checkpoint 38: send rows each inspector strip of a pair keeps (so both line up)
enum StripHide { HideIcon = 1, HideSetting = 2, HideEq = 4, HideSends = 8, HideLevel = 16, HidePan = 32, HideShort = 64, HideMid = 128, HideGroupAuto = 256,
                 HideSendAdd = 512, IconSmall = 1024, PanSmall = 2048,
                 IconInline = 4096 };  // D6 (900 px pane): the icon beside the pan knob, on its row  // D5 (R11-08): the compact rows of a short pane  // HideMid: the fader down to 48 px (checkpoint 26)

// fxRows >= 0: a fixed layout (the mixer, where the strips line up with the legend): the instrument
// row is always there (blank without one) and the effects take exactly fxRows rows.
// padRows (K-112b, checkpoint 22): the inspector's pair of strips in the Tracks layout; both keep an
// instrument row and this many effect rows, so their knobs, faders and buttons line up across.
// K-112D (audit X5): Smooth's strip meter, as the reference's: two continuous bars a pixel apart, green
// up to -6 dB, then yellow, red over 0 dB, on a dark well, with a held peak line (the shared ballistics).
void smoothMeter(ImGuiID key, float peakL, float peakR, ImVec2 size) {
  static std::map<ImGuiID, LevelMeter> states;
  LevelMeter& m = states[key];
  m.update(peakL, peakR, ImGui::GetIO().DeltaTime);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  const float pos0 = LevelMeter::toPos(0.0f), pos6 = LevelMeter::toPos(-6.0f);
  const float bw = std::floor((size.x - 1) * 0.5f);
  const ImU32 green = theme::rgb(0x3FC84A), yellow = theme::rgb(0xE8C83A), red = theme::rgb(0xE5484D);
  for (int c = 0; c < 2; ++c) {
    const float x0 = p.x + static_cast<float>(c) * (bw + 1), x1 = x0 + bw, y1 = p.y + size.y;
    auto yAt = [&](float f) { return std::round(y1 - f * size.y); };
    dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, y1), theme::mix(P.field, P.ink, 0.3f));
    const float v = LevelMeter::toPos(LevelMeter::toDb(m.level(c)));
    if (v > 0.0f) {
      const float top = yAt(v);
      dl->AddRectFilledMultiColor(ImVec2(x0, std::max(top, yAt(pos6))), ImVec2(x1, y1), theme::mix(green, yellow, 0.15f), theme::mix(green, yellow, 0.15f), green, green);
      if (v > pos6) dl->AddRectFilledMultiColor(ImVec2(x0, std::max(top, yAt(pos0))), ImVec2(x1, yAt(pos6)), yellow, yellow, theme::mix(green, yellow, 0.15f), theme::mix(green, yellow, 0.15f));
      if (v > pos0) dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, yAt(pos0)), red);
    }
    const float h = m.hold(c);
    if (h > 1e-3f) {
      const float hp = LevelMeter::toPos(LevelMeter::toDb(h));
      const float hy = yAt(hp);
      dl->AddLine(ImVec2(x0, hy), ImVec2(x1, hy), hp > pos0 ? red : hp > pos6 ? yellow : green, 1.0f);
    }
  }
}

// D5 (CPU): what the mixer's strips ask of the whole song (each track's strip, the automation lanes'
// targets), worked out once per change of the song (its revision) and at most every half second
// otherwise (the bus fold can change without an edit); it was worked out per strip, per frame.
struct MixFacts {
  uint64_t rev = ~0ull;
  int frame = -1000;
  const Song* song = nullptr;
  std::vector<ObjectId> stripOf;                     // per arrange track: its strip (trackStrip), else kNoId
  std::vector<std::pair<ObjectId, uint32_t>> lanes;  // the automation lanes' targets
};
MixFacts gMixFacts;
const MixFacts& mixFacts(App& a) {
  const Song& s = a.doc.song();
  const int fr = ImGui::GetFrameCount();
  MixFacts& f = gMixFacts;
  if (f.rev == a.doc.revision() && f.song == &s && fr >= f.frame && fr - f.frame < 30) return f;
  f.rev = a.doc.revision(), f.song = &s, f.frame = fr;
  f.stripOf.assign(s.tracks.size(), kNoId);
  for (int tr = 0; tr < static_cast<int>(s.tracks.size()); ++tr)
    if (const MixerTrack* st = logicplug::trackStrip(s, tr)) f.stripOf[static_cast<size_t>(tr)] = st->id;
  f.lanes.clear();
  for (const AutomationLane& l : automationLanes(s)) f.lanes.emplace_back(l.owner, l.index);
  return f;
}

// K-112D: the strip's automation slot: the mode of the track whose strip it is (Off, Read, Touch, Latch,
// Write in the reference's colours: Read green). A click picks the mode for every track on this strip.
void autoSlot(App& a, const MixerTrack& t, float w) {
  static const char* const kNames[5] = {i18n::N_("Off"), i18n::N_("Read"), i18n::N_("Touch"), i18n::N_("Latch"), i18n::N_("Write")};
  static const uint32_t kCols[5] = {0x8A8A8A, 0x5CC26A, 0xE0B33A, 0xE08A3A, 0xA77BE0};
  const Song& s = a.doc.song();
  std::vector<int> tracks;
  const MixFacts& mf = mixFacts(a);  // D5
  for (int tr = 0; tr < static_cast<int>(mf.stripOf.size()) && tr < static_cast<int>(s.tracks.size()); ++tr)
    if (mf.stripOf[static_cast<size_t>(tr)] == t.id) tracks.push_back(tr);
  const bool master = s.mixerIndex(t.id) == 0;
  const int mode = tracks.empty() ? (master ? 1 : -1) : std::clamp(static_cast<int>(s.tracks[static_cast<size_t>(tracks[0])].autoMode), 0, 4);
  if (mode < 0) {
    ImGui::Dummy(ImVec2(w, 17));
    return;
  }
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 17;
  const bool click = ImGui::InvisibleButton("##auto", ImVec2(w, h)) && !tracks.empty();
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov && !tracks.empty() ? theme::mix(P.surface700, P.text, 0.08f) : P.surface700, 4.0f);
  const char* label = i18n::tr(kNames[mode]);
  const float fs = theme::kFontLabel;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(centredX(a.fontRegular, fs, label, p.x + 5, w - 10), std::round(p.y + (h - fs) * 0.5f) - 1), w - 10,
                      mode == 0 ? P.textDim : theme::rgb(kCols[mode]), label);
  if (hov) ui::Hint(tracks.empty() ? "The master's automation plays as written" : "The track's automation mode: click to choose Off, Read, Touch, Latch or Write");
  if (click) ImGui::OpenPopup("##automode");
  if (ImGui::BeginPopup("##automode")) {
    for (int m = 0; m < 5; ++m)
      if (ImGui::MenuItem(i18n::tr(kNames[m]), nullptr, m == mode) && m != mode) {
        a.doc.beginGroup("automation mode");
        for (int tr : tracks) a.doc.editPlaylistTrack(tr, "automation mode", [m](PlaylistTrack& x) { x.autoMode = static_cast<uint8_t>(m); });
        a.doc.endGroup();
      }
    ImGui::EndPopup();
  }
}

void strip(App& a, const MixerTrack& t, const Channel* ch, ImVec2 p, ImVec2 size, int fxRows = -1, float* rowY = nullptr, int hide = 0, int padRows = 0) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const size_t index = static_cast<size_t>(std::max(0, s.mixerIndex(t.id)));
  const float w = size.x - 6;
  ImGui::PushID(static_cast<int>(t.id));
  ImGui::SetCursorScreenPos(p);
  ImGui::BeginGroup();
  // K-112b (second pass): the slots stack at a 2 px gap, a 19 px pitch for the 17 px pills, as the
  // references' mixer and inspector strips do (134, 046, 216); the fader gets the height saved.
  // The cursor steps back instead of a pushed spacing, so the slots' menus keep their own.
  auto gap = [] { ImGui::SetCursorPosY(ImGui::GetCursorPosY() - std::max(0.0f, ImGui::GetStyle().ItemSpacing.y - 2.0f)); };
  auto mark = [&](int row) {
    if (rowY) rowY[row] = ImGui::GetCursorScreenPos().y;
  };
  if (!(hide & HideSetting)) {
    mark(RowSetting);  // the track's settings: its menu (the effect chain: copy, paste, save; options)
    if (stripSlot("##setting", i18n::tr("Setting"), false, true, w, "The track's settings: its effect chain (copy, paste, save) and options"))
      ImGui::OpenPopup("##stripset");
    gap();
  }
  if (devMenuTake("strip")) ImGui::OpenPopup("##stripset");  // dev: "menu open strip"
  if (ImGui::BeginPopup("##stripset")) {
    if (const int mi = a.doc.song().mixerIndex(t.id); mi >= 0) mixerTrackMenu(a, static_cast<size_t>(mi));
    ImGui::EndPopup();
  }
  if (!(hide & HideEq)) {
    mark(RowEq);
    eqThumb(a, t, w);
    gap();
  }
  mark(RowInst);
  if (ch) {
    const PluginInfo* info = findPlugin(ch->plugin.type);
    std::string name = info ? info->name : (ch->isLayer() ? i18n::tr("Layer") : i18n::tr("No instrument"));
    if (ch->plugin.type == packs::samplers::kPatchPlayerType) {  // K-333: the patch it plays
      const std::string patch = packs::samplers::parsePatchRef(ch->plugin.data).name;
      if (!patch.empty()) name = patch;
    }
    if (stripSlot("##inst", name, a.settingsChannel == ch->id, !info && !ch->isLayer(), w, "The channel's instrument: click to open it", theme::rgb(kSlotGreen)))
      a.settingsChannel = ch->id;
    gap();
  } else if (fxRows >= 0 || padRows > 0) {
    ImGui::Dummy(ImVec2(w, 17));
    gap();
  }
  mark(RowFx);
  int shown = 0;
  int fxCount = 0;
  for (const EffectSlot& e : t.slots) fxCount += e.plugin.type.empty() ? 0 : 1;
  // Checkpoint 28: a single effect row shows a lone effect itself (no free slot under it).
  const bool lone = fxRows == 1 && fxCount == 1;
  // D9 (R14-11): effects that fill every row show whole, with no free slot under them (it was the last
  // effect folded into a "+1": the same row, less said). The other strips keep their free slot.
  const bool full = fxRows > 1 && fxCount == fxRows;
  const int fxMax = fxRows >= 0 ? (lone || full ? fxRows : fxRows - 1) : 6;
  // K-112b (checkpoint 22): when the mixer caps its effect rows, the last row says how many more there
  // are (a click shows them in the detailed mixer) instead of leaving them out unseen.
  const bool more = fxRows >= 0 && fxCount > fxMax;
  // D6 (R12-09): with a count row, every row above it holds an effect (no free slot is shown then).
  for (int k = 0; k < kEffectSlots && shown < (more ? fxRows - 1 : fxMax); ++k) {
    if (t.slots[static_cast<size_t>(k)].plugin.type.empty()) continue;
    fxPill(a, t, k, w);
    gap();
    ++shown;
  }
  if (more) {
    const std::string label = "+" + std::to_string(fxCount - shown);
    if (stripSlot("##morefx", label, false, false, w, "More effects: click to show them all in the detailed mixer")) {
      a.selMixer = t.id;
      a.settings.set("ui.logicMixerDetail", true);
    }
  } else if (!lone && !full) {
    emptyPill(a, t, firstFree(t), w);
  }
  if (!lone && !full) {
    gap();
  }
  // The count row stands where the free slot would: both take the row after the effects shown.
  for (int k = shown + 1; k < (fxRows >= 0 ? fxRows : padRows); ++k) {
    ImGui::Dummy(ImVec2(w, 17));
    gap();
  }
  {
    if (!(hide & HideSends) && logicplug::active(a)) {
      // Checkpoint 38: the inspector's strips list each send as its own pill (the destination's name) with
      // its level knob beside it, then an empty Sends slot, as 216 and 110 draw them ("Sends 1" counted the
      // track's output route too). The pair keeps the same number of send rows so the strips line up.
      mark(RowSends);
      const Send* outSend = nullptr;
      for (const Send& sd : t.sends)
        if (!sd.sidechain && !outSend) outSend = &sd;
      int shownSends = 0;
      const int sendCap = fxRows >= 0 ? gSendRows : 1 << 20;  // K-112D: the mixer's rows line up
      for (const Send& sd : t.sends) {
        if (shownSends >= sendCap) break;
        if (&sd == outSend || sd.stageRoom) continue;
        const int di = s.mixerIndex(sd.to);
        if (di < 0) continue;
        ImGui::PushID(di);
        const float kw = 18.0f;
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        const std::string nm = (sd.sidechain ? std::string("SC ") : std::string()) + busName(s, static_cast<size_t>(di));  // R09-07
        const bool inInsp = fxRows < 0;
        if (stripSlot("##send", nm, false, false, w - kw,
                      inInsp ? "A send: click to show its bus in the strip beside; the knob sets its level" : "A send: click to show it in the mixer; the knob sets its level",
                      theme::rgb(kSlotBlue))) {  // blue as the effects (216)
          if (inInsp) {
            gInspDest[t.id] = sd.to;  // R10-10: the right strip shows the send's bus, as the reference's
          } else {
            a.selMixer = t.id;
            a.showMixer = true;
          }
        }
        ImGui::SetCursorScreenPos(ImVec2(sp.x + w - kw + 3, sp.y + 1.5f));
        ui::setAltClickResets(true);  // R09-04 (Smooth only: this row is Smooth's)
        ui::ParamKnob("##sendk", sendParam(t.id, di), 7.0f);
        ui::setAltClickResets(false);
        ImGui::SetCursorScreenPos(ImVec2(sp.x, sp.y + 17));
        ImGui::Dummy(ImVec2(w, 0));
        ImGui::PopID();
        gap();
        ++shownSends;
      }
      // R09 (visual gap 7): the rows without a send are the same quiet empty slots as the one that adds
      // a send, with no word in them, as 134 draws its send rows (a dim "Sends" read as a second label).
      // D5 (R11-08): in a short pane the add-a-send slot is the first empty send row (no row of its own).
      const bool addInRow = (hide & HideSendAdd) && fxRows >= 0;
      bool added = false;
      for (int k = shownSends; k < gSendRows; ++k) {
        if (addInRow && !added) {
          added = true;
          if (stripSlot("##sends", "", false, true, w, "Sends: click to send this track to a bus") || ImGui::IsItemClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##sendsmenu");
          gap();
          continue;
        }
        const ImVec2 ep = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, 17));
        dl->AddRectFilled(ep, ImVec2(ep.x + w, ep.y + 17), emptyFace(false), 4.0f);
        gap();
      }
      if (!addInRow && (stripSlot("##sends", "", false, true, w, "Sends: click to send this track to a bus") || ImGui::IsItemClicked(ImGuiMouseButton_Right)))
        ImGui::OpenPopup("##sendsmenu");
      routeMenu(a, "##sendsmenu", t, false);
      if (!addInRow) gap();
    } else if (!(hide & HideSends)) {
      mark(RowSends);
      std::string sends = t.sends.empty() ? std::string(i18n::tr("Sends")) : std::string(i18n::tr("Sends")) + " " + std::to_string(t.sends.size());
      if (stripSlot("##sends", sends, false, t.sends.empty(), w, "The track's sends: shows it in the mixer")) {
        a.selMixer = t.id;
        a.showMixer = true;
      }
      gap();
    }
    mark(RowOut);
    const MixerTrack* out = nullptr;
    for (const Send& sd : t.sends)
      if (!sd.sidechain && !out) out = s.mixerTrack(sd.to);
    const std::string outName = index == 0 ? std::string(i18n::tr("Device")) : out ? busName(s, static_cast<size_t>(std::max(0, s.mixerIndex(out->id)))) : std::string(i18n::tr("Master"));
    if (logicplug::active(a) && index > 0) {  // R05-14
      if (stripSlot("##out", outName, false, false, w, "Where the track goes: click to choose") || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##outmenu");
      routeMenu(a, "##outmenu", t, true);
    } else if (stripSlot("##out", outName, false, false, w, "Where the track goes: shows it in the mixer")) {
      a.selMixer = t.id;
      a.showMixer = true;
    }
    // Checkpoint 35: the inspector's strips end their slots with the track's group, as 216, 110 and 125
    // show a Group slot under the output. A click picks a group, a new one or none (mixops::setGroup,
    // one shared edit).
    const bool groupRow = logicplug::active(a) && !(hide & HideGroupAuto);
    if (groupRow && index == 0 && (padRows > 0 || fxRows >= 0)) {  // the master beside a track: an empty row, so both line up
      gap();
      mark(RowGroup);
      ImGui::Dummy(ImVec2(w, 17));
    } else if (groupRow && index > 0) {
      gap();
      mark(RowGroup);
      if (stripSlot("##group", t.group.empty() ? std::string(i18n::tr("Group")) : t.group, false, t.group.empty(), w,
                    "The track's group: click to put it in one or take it out"))
        ImGui::OpenPopup("##stripgroup");
      if (ImGui::BeginPopup("##stripgroup")) {
        const ObjectId id = t.id;
        if (ImGui::MenuItem(i18n::tr("Remove from group"), nullptr, false, !t.group.empty())) mixops::setGroup(a.doc, {id}, "");
        const auto groups = mixops::groupNames(s);
        if (!groups.empty()) ImGui::Separator();
        for (const auto& g : groups)
          if (ImGui::MenuItem(g.c_str(), nullptr, t.group == g) && t.group != g) mixops::setGroup(a.doc, {id}, g);
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Create group...")))
          menus::openPrompt("Group name", mixops::newGroupName(s), [&a, id](const std::string& n) {
            if (!n.empty()) {  // K-238: a new group starts linked
              mixops::createGroup(a.doc, {id}, n, true);
            }
          });
        ImGui::EndPopup();
      }
    }
    if (groupRow) {  // K-112D: the automation mode of the track this strip plays, as 134 and 216 show it under the group
      gap();
      mark(RowAuto);
      autoSlot(a, t, w);
    }
  }
  ImGui::EndGroup();
  const float topH = ImGui::GetItemRectSize().y + 6;
  // Pan, level, fader and meter, mute and solo, name.
  // K-112b: the inspector's strips (Tracks layout) as both references draw them (110, 216): a
  // larger pan knob, and the name as plain text under mute and solo (the mixer keeps its plates).
  const bool inspStrip = fxRows < 0 && logicplug::active(a);
  const bool mixStrip = fxRows >= 0;  // K-112b: the Tracks layout's mixer (strips in columns)
  const float knobR = inspStrip ? std::clamp(std::round(size.x * 0.17f), 11.0f, 16.0f) : mixStrip && logicplug::active(a) && !(hide & HideMid) ? (hide & PanSmall ? 12.0f : 15.0f) : 11.0f, plateH = 18, msH = inspStrip ? 20.0f : 18.0f, dbH = 16;
  // D8: every control centres on the slots' own column (p.x .. p.x + w); the centre was the strip's
  // box, 3 px right of the slots (the pan knob, the icon, the fader and meter sat off their axis).
  const float cx = std::round(p.x + w * 0.5f);
  float y = p.y + topH;
  // D6: in the shortest panes (900 px window) the icon sits left of the pan knob, on the pan's row, so
  // the fader keeps those 22 px.
  const bool inlineIcon = fxRows >= 0 && (hide & IconInline) && !(hide & HideIcon) && !(hide & HidePan);
  const float panX = inlineIcon ? cx + 9 : cx;
  if (fxRows >= 0 && !(hide & HideIcon)) {  // the mixer: the track's icon above its pan knob
    const int ic = t.icon > 0 ? t.icon : ch && ch->icon > 0 ? ch->icon : trackicons::guess(t.name, ch ? "Synth" : "Bus");
    // In the track's colour (dimmed toward the text colour so a dark colour still reads).
    const bool small = hide & IconSmall;  // D5 (R11-08)
    if (ic > 0)
      trackicons::draw(ImGui::GetWindowDrawList(), ic, inlineIcon ? ImVec2(cx - 17, std::round(y + knobR)) : ImVec2(cx, std::round(y + (small ? 10 : 13))),
                       inlineIcon ? 16 : small ? 18 : 24, theme::mix(theme::content(t.color), pal().text, 0.25f));
    if (!inlineIcon) y += small ? 22 : 30;
  }
  if (!(hide & HidePan)) {
    if (rowY) rowY[RowPan] = y;
    ImGui::SetCursorScreenPos(ImVec2(panX - knobR, y));
    if (logicplug::active(a)) gPanFont = a.fontRegular, ui::SetNextKnobFace(smoothPanFace);  // K-112D
    ui::setAltClickResets(logicplug::active(a));  // R09-04: Option-click centres the pan
    ui::ParamKnob("##pan", mixerParam(t.id, MixerParam::Pan), knobR, nullptr, true);
    ui::setAltClickResets(false);
    y += knobR * 2 + 4;
  }
  if (rowY && !(hide & HideLevel)) rowY[RowLevel] = y;
  // Two value boxes: the level set by the fader, and the loudest peak since it was reset (a click).
  const float fs = theme::kFontLabel;
  const mixmeters::Reading r = mixmeters::get(a, t.id);
  if (!(hide & HideLevel)) {
    static std::map<ObjectId, float> held;  // the peak hold per track, this run
    float& hp = held[t.id];
    hp = std::max(hp, std::max(r.peakL, r.peakR));
    // D8: the two boxes span the slots' width exactly (they sat 2 px in on the left, 1 px on the right), 4 px apart
    const float bw = std::floor((w - 4) * 0.5f), bh = dbH;
    const ImVec2 b0(p.x, y), b1(p.x + w - bw, y);
    // R04-03: the fader in dB by the mixer's own law (the lane and the engine read it so).
    double shownVol = t.volume;  // R07-08: in Read, the curve's value, as the fader shows it
    if (double lv = 0; ui::shownParamValue(mixerParam(t.id, MixerParam::Volume), lv)) shownVol = lv;
    const std::string db = dbText(static_cast<float>(dsp::faderGain(shownVol, 0.8))), pk = hp > 0.00001f ? dbText(hp) : std::string("-inf");
    const bool sm = logicplug::active(a);  // K-112D (audit X3, E6): the body font, the peak in the meter's green
    ImFont* nf = sm ? a.fontRegular : a.fontMono;
    for (int k = 0; k < 2; ++k) {
      const ImVec2 q = k == 0 ? b0 : b1;
      dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + bh), P.field, sm ? 4.0f : 3.0f);
      const std::string& v = k == 0 ? db : pk;
      const ImU32 col = k == 1 && hp >= 1.0f ? P.danger : k == 1 ? (sm ? (hp > 0.00001f ? theme::rgb(0x3FC84A) : P.textFaint) : theme::rgb(theme::role::kPlay)) : P.text;
      if (sm) {  // tabular figures (audit X3)
        const float tw = tabWidth(nf, fs, v.c_str());
        tabText(dl, nf, fs, ImVec2(std::round(q.x + (bw - tw) * 0.5f), std::round(q.y + (bh - fs) * 0.5f) - 1), col, v.c_str());
        continue;
      }
      const float tw = nf->CalcTextSizeA(fs, 1e9f, 0, v.c_str()).x;
      dl->AddText(nf, fs, ImVec2(std::round(q.x + (bw - tw) * 0.5f), std::round(q.y + (bh - fs) * 0.5f) - 1), col, v.c_str());
    }
    ImGui::SetCursorScreenPos(b1);
    if (ImGui::InvisibleButton("##peak", ImVec2(bw, bh))) hp = 0;
    if (ImGui::IsItemHovered()) ui::Hint("The loudest peak since it was reset: click to reset");
    y += dbH + 6;
  }
  if (rowY) rowY[RowFader] = y;
  const float fh = std::max(mixStrip ? (hide & HideShort ? 32.0f : hide & HideMid ? 48.0f : kMixFaderMin) : 40.0f, p.y + size.y - y - msH - plateH - 8);
  // K-112b (checkpoint 24): the Tracks layout's strips carry their own dB scale between the fader and
  // the meter, as every reference strip does (110, 134, 216, 043); it needs a fader of some height.
  const float faderW = 20, meterW = 12;
  // K-112D (audit E5, F6): Smooth's mixer strips (about 63 px at 1920) keep their scale too.
  const bool scaleCol = (inspStrip || mixStrip) && fh >= 64 && size.x >= (logicplug::active(a) ? 56.0f : 64.0f);
  const float scaleW = scaleCol ? 16.0f : 0.0f, fg = scaleCol ? 3.0f : 5.0f;
  const float x0 = std::round(cx - (faderW + fg * (scaleCol ? 2 : 1) + scaleW + meterW) * 0.5f);
  const ParamId volPid = mixerParam(t.id, MixerParam::Volume);
  // The fader's ruler ticks on the same marks as the scale's labels (checkpoint 25).
  // R09-03: one law for the cap and the scale (the fader's dB, unity 0.8), marks from +6 to -60 as the
  // reference's fader scale; a mark above the fader's top is left out.
  // D4: the ruler ticks on the labelled marks only (faderMarks), so every tick has its number.
  float tickN[12];
  int tickCount = 0;
  if (scaleCol)
    if (const auto d = describeParam(s, volPid)) {
      int mdb[12];
      float mys[12];
      tickCount = faderMarks(*d, ImVec2(x0, y), ImVec2(faderW, fh), kMarkGap, mdb, mys, 12);
      for (int k = 0; k < tickCount; ++k) tickN[k] = ui::FaderNormForDb(*d, mdb[k], 0.8);
    }
  ImGui::SetCursorScreenPos(ImVec2(x0, y));
  ui::setFaderTicks(tickN, tickCount);
  bool readLocked = false;  // R05-04: a track in Read with a curve on this fader: the curve moves it, a drag can't
  if (logicplug::active(a) && a.engine.playing()) {
    bool curve = false;  // a volume curve on this strip
    const MixFacts& mf = mixFacts(a);  // D5: worked out once, not per strip and frame
    for (const auto& l : mf.lanes) curve = curve || (l.first == volPid.owner && l.second == volPid.index);
    for (int tr = 0; curve && tr < static_cast<int>(s.tracks.size()) && !readLocked; ++tr) {
      const PlaylistTrack& pt = s.tracks[static_cast<size_t>(tr)];
      if (pt.autoMode != 1) continue;
      if (pt.laneSet) readLocked = pt.laneOwner == volPid.owner && pt.laneIndex == volPid.index;
      else if (static_cast<size_t>(tr) < mf.stripOf.size()) readLocked = mf.stripOf[static_cast<size_t>(tr)] == volPid.owner;
    }
  }
  ui::setFaderLocked(readLocked);
  ui::setFaderDb(logicplug::active(a) ? 0.8 : -1.0);  // R05-15: the Tracks layout's strips read in dB
  ui::setAltClickResets(logicplug::active(a));        // R09-04: Option-click sets 0 dB
  ui::ParamFader("##f", volPid, ImVec2(faderW, fh));
  ui::setAltClickResets(false);
  ui::setFaderDb(-1.0);
  ui::setFaderLocked(false);
  ui::setFaderTicks(nullptr, 0);
  const float meterX = x0 + faderW + fg + (scaleCol ? scaleW + fg : 0.0f);
  if (scaleCol) {
    if (const auto d = describeParam(s, volPid)) {
      const float sfs = theme::kFontLabel - 2, sx1 = x0 + faderW + fg + scaleW;
      // R09-03: each mark on the cap's own row for that level (the same law and geometry the fader draws
      // with). D4 (R10-04): taken by rank, so 0 always shows, and the same marks as the legend's.
      int mdb[12];
      float mys[12];
      const int mn = faderMarks(*d, ImVec2(x0, y), ImVec2(faderW, fh), kMarkGap, mdb, mys, 12);
      for (int mk = 0; mk < mn; ++mk) {
        const int db = mdb[mk];
        const float yy = mys[mk];
        char tx[8];
        std::snprintf(tx, sizeof tx, db > 0 ? "+%d" : "%d", db);  // R09-03: signed, as the reference's fader scale
        ImFont* const sf = logicplug::active(a) ? a.fontRegular : a.fontMono;  // K-112D (audit X3): the interface face
        const float tw = sf->CalcTextSizeA(sfs, 1e9f, 0, tx).x;
        dl->AddText(sf, sfs, ImVec2(std::round(sx1 - tw), std::round(yy - sfs * 0.5f) - 1), P.textDim, tx);
      }
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(meterX, y));
  if (logicplug::active(a)) smoothMeter(ImGui::GetID("m"), r.peakL, r.peakR, ImVec2(meterW, fh));  // K-112D (audit X5)
  else ui::Meter(ImGui::GetID("m"), r.peakL, r.peakR, ImVec2(meterW, fh));
  y += fh + 4;
  if (rowY) rowY[RowMuteSolo] = y;
  {  // mute and solo (the master has no solo)
    // K-112b: the references' inspector M and S are wider. Close-range pass: in the mixer the two fill
    // the strip's width, 4 px apart, as 043 and 134 have them (the master's one takes the left half).
    const float bw = inspStrip ? 28.0f : mixStrip ? std::floor((w - 4) * 0.5f) : 22.0f, bh = msH;  // D8: the slots' width
    auto btn = [&](const char* bid, const char* label, bool on, ImU32 lit, float bx) {
      ImGui::SetCursorScreenPos(ImVec2(bx, y));
      const bool c = ImGui::InvisibleButton(bid, ImVec2(bw, bh));
      const bool hv = ImGui::IsItemHovered();
      // K-112b (checkpoint 22): one quiet hover for the Tracks layout's own controls, the rest face 8 % toward the text (14 % open or held)
      const ImU32 restB = P.surface700, hovB = logicplug::active(a) ? theme::mix(restB, P.text, 0.08f) : P.surface600;
      const ImVec2 ts = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, label);
      if (logicplug::active(a)) {
        // D5 (visual gap 8): the reference's raised buttons (134, 043): unlit, a face a step lighter than
        // the strip, lit from the top (a hairline highlight, a darker foot) with light type; lit, the
        // button's own colour (mute blue, solo yellow) with dark type, a touch pressed in.
        const ImU32 face = on ? (hv ? theme::mix(lit, P.text, 0.12f) : lit) : hv ? theme::mix(P.surface700, P.text, 0.14f) : theme::mix(P.surface700, P.text, 0.07f);
        const ImU32 top = on ? theme::mix(face, P.text, 0.10f) : theme::mix(face, P.text, 0.05f), bot = theme::mix(face, P.ink, on ? 0.04f : 0.12f);
        const ImVec2 q0(bx, y), q1(bx + bw, y + bh);
        dl->AddRectFilled(ImVec2(q0.x, q0.y + 1), ImVec2(q1.x, q1.y + 1), theme::withAlpha(P.ink, 0.45f), 4.0f);  // the foot's shadow
        dl->AddRectFilled(q0, q1, bot, 4.0f);
        dl->AddRectFilledMultiColor(ImVec2(q0.x + 1, q0.y + 1), ImVec2(q1.x - 1, q1.y - 1), top, top, face, face);
        if (!on) dl->AddLine(ImVec2(q0.x + 3, q0.y + 0.5f), ImVec2(q1.x - 3, q0.y + 0.5f), theme::withAlpha(P.text, 0.16f));
        dl->AddText(a.fontBold, fs, ImVec2(std::round(bx + (bw - ts.x) * 0.5f), std::round(y + (bh - ts.y) * 0.5f)),
                    on ? theme::rgb(0x1A1A1C) : theme::mix(P.textDim, P.text, 0.65f), label);
        return c;
      }
      dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + bh), on ? lit : hv ? hovB : restB, 3.0f);
      dl->AddText(a.fontBold, fs, ImVec2(std::round(bx + (bw - ts.x) * 0.5f), std::round(y + (bh - ts.y) * 0.5f)), on ? P.ink : P.textDim, label);
      return c;
    };
    const bool twoBtn = index > 0;
    const float bx = mixStrip ? p.x : twoBtn ? std::round(cx - bw - 2) : std::round(cx - bw * 0.5f);
    // (Smooth's hot is its blue, as the reference's lit mute and the track headers')
    // D8: the click mutes again (a comment from D5 had swallowed the call, so M did nothing in Smooth's strips)
    if (btn("##m", "M", t.muted, P.hot, bx)) {
      a.doc.setParam(mixerParam(t.id, MixerParam::Mute), t.muted ? 0 : 1, false);
    }
    if (ImGui::IsItemHovered()) ui::Hint(t.muted ? "Track is muted: click to unmute" : "Track is on: click to mute");
    if (twoBtn) {
      if (btn("##s", "S", t.solo, logicplug::active(a) ? theme::rgb(0xE8C83A) : theme::rgb(theme::role::kStamp), mixStrip ? p.x + w - bw : bx + bw + 4))  // mixer: on the slots' right edge (D8)
        mixops::toggleSolo(a.doc, t.id);  // K-238: a linked group solos together
      if (ImGui::IsItemHovered()) ui::Hint("Solo (keeps tracks routed to and from it)");
    }
    y += bh + 4;
  }
  const std::string name0 = index == 0 ? std::string(i18n::tr("Master")) : mixerTrackName(s, index);
  if (rowY) rowY[RowName] = y;
  // K-112b (close-range pass): in the mixer the name bar spans the strip's column edge to edge and
  // runs down to its foot, square, as 043, 134 and 133 draw it (it was an inset plate with corners).
  // The column is the strip's area plus the mixer's 4 px margin left, 2 px right and 2 px below.
  const ImVec2 n0(mixStrip ? p.x - 4 : p.x, y), n1(mixStrip ? p.x + w + 4 : p.x + w, y + plateH + (mixStrip ? 2.0f : 0.0f));  // D8: on the slots' column
  const ImU32 plate = theme::mix(P.surface700, theme::content(t.color), 0.85f);  // the track colour, near full (as every reference mixer)
  if (!inspStrip) dl->AddRectFilled(n0, n1, plate, mixStrip ? 0.0f : 3.0f);
  float nfs = logicplug::active(a) ? theme::kFontCaption : fs;  // K-112D (D3, X6): the track's name at 12 px, as 134's plates
  std::string name = name0;
  if (logicplug::active(a)) {  // D4 (R10-04): a name too long steps down a pixel, then is shortened word by word
    const float room = n1.x - n0.x - 8 - gFitExcess;  // D5: the same at every width
    if (a.fontRegular->CalcTextSizeA(nfs, 1e9f, 0, name.c_str()).x > room) nfs -= 1;
    name = fitName(a.fontRegular, nfs, name, room);
  }
  if (name != name0 && ImGui::IsMouseHoveringRect(n0, n1) && ImGui::IsWindowHovered()) ui::Hint(name0);  // D4: the name in full
  // D9 (R14-11): a right-click on the name gives the track's settings menu (the Setting row's), which a
  // tall mixer may fold away for its fader
  if (mixStrip && ImGui::IsMouseHoveringRect(n0, n1) && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("##stripset");
  }
  const float nw = std::min(a.fontRegular->CalcTextSizeA(nfs, 1e9f, 0, name.c_str()).x, n1.x - n0.x - 8);
  const float nameY = mixStrip ? std::round(n0.y + (n1.y - n0.y - nfs) * 0.5f) - 1 : std::round(n0.y + (plateH - nfs) * 0.5f) - 1;
  theme::textEllipsis(dl, a.fontRegular, nfs, ImVec2(std::round((n0.x + n1.x - nw) * 0.5f), nameY), n1.x - n0.x - 8,
                      inspStrip ? P.text : theme::contrastText(plate), name.c_str());
  ImGui::PopID();
}

bool gRegionOpen = false;
void inspector(App& a) {
  const Song& s = a.doc.song();
  // The region: the first selected clip (in song order); edits apply to every selected one.
  const Clip* clip = nullptr;
  std::vector<ObjectId> selIds;
  for (const Clip& c : s.playlist)
    if (a.playlist.selected.count(c.id)) {
      if (!clip) clip = &c;
      selIds.push_back(c.id);
    }
  const int nSel = static_cast<int>(selIds.size());
  int track = clip ? clip->track : (!a.playlist.trackSel.empty() ? *a.playlist.trackSel.begin() : -1);
  if (track >= static_cast<int>(s.tracks.size())) track = -1;
  // R01-10: in the Tracks layout a track is always selected, as in the reference (the selected
  // channel's track, else the first), so the Track card and both strips never go away.
  if (track < 0 && logicplug::active(a)) track = logicplug::focusTrack(a);
  // Every selected region at once, one undo step (a drag on a value folds into one by its key).
  auto editSel = [&](const char* label, const std::function<void(Clip&)>& fn, uint64_t mergeKey = 0) {
    a.doc.editClips(label, [&selIds, &fn](std::vector<Clip>& cs) {
      for (Clip& k : cs)
        if (std::find(selIds.begin(), selIds.end(), k.id) != selIds.end()) fn(k);
    }, mergeKey);
  };
  bool& regionOpen = gRegionOpen;
  static bool trackOpen = false;  // both cards start folded: the strips below take the column
  gHeadFirst = true;  // D9 (R14-08): the first heading is the pane's header (Smooth only: see cardFold)
  // ---- Region card ----
  if (clip) {
    const std::string head = nSel > 1 ? std::string(i18n::tr("Regions")) + ": " + std::to_string(nSel) + " " + i18n::tr("selected")
                                      : std::string(i18n::tr("Region")) + ": " + clipTitle(s, *clip);
    if (cardFold("##rfold", head, &regionOpen)) {
      cardBegin();
      // R08-06: the region's own name, editable here as in the reference's region inspector (the
      // name bar and the Text tool show the same name; clearing it brings the pattern's or sound's back).
      if (nSel == 1 && logicplug::active(a) && cardRow("##rname", "Name", clipTitle(s, *clip), "Click: rename this region (its pattern and sound keep their names)", true)) {
        const ObjectId cid = clip->id;
        menus::openPrompt("Region name", clipTitle(s, *clip), [&a, cid](const std::string& n) {
          if (const Clip* k = a.doc.song().clip(cid); k && k->name != n) a.doc.editClip(cid, "rename region", [n](Clip& x) { x.name = n; });
        });
      }
      if (cardRow("##rmute", "Mute", clip->muted ? i18n::tr("on") : i18n::tr("off"), "Click: mute or unmute the region", true)) {
        const bool to = !clip->muted;
        editSel(to ? "mute clip" : "unmute clip", [to](Clip& k) { k.muted = to; });
      }
      if (cardRow("##rloop", "Loop", clip->loop ? i18n::tr("on") : i18n::tr("off"), "Click: the region repeats its content past its end, or plays it once", true)) {
        // PL-1118: the regions loop on to the next region, as the reference's Loop (it only set the
        // flag: nothing repeated, and an audio region's loop window was left unset).
        const bool to = !clip->loop;
        const std::set<ObjectId> lsel(selIds.begin(), selIds.end());
        a.doc.editSong(to ? "loop clip" : "stop looping clip", [&](Song& x) { loopClipsToNext(x, lsel, to); });
      }
      if (clip->kind == ClipKind::Pattern) {
        // Playback settings: the pattern stays as written, the region plays it changed.
        const Tick q = std::max<Tick>(1, s.ppq);
        struct QChoice {
          const char* name;
          Tick ticks;
        };
        const QChoice qs[] = {{i18n::N_("off"), 0},          {"1/4", q},           {"1/8", q / 2},  {"1/16", q / 4},
                              {"1/32", q / 8},               {"1/8 T", q / 3},     {"1/16 T", q / 6}};
        std::string qName = clip->playQuantize ? std::to_string(clip->playQuantize) + " t" : std::string(i18n::tr("off"));
        for (const QChoice& c : qs)
          if (c.ticks == clip->playQuantize) qName = i18n::tr(c.name);
        if (cardRow("##rquant", "Quantize", qName, "Click: snap the region's notes to a grid as it plays (the pattern stays as written)", true))
          ImGui::OpenPopup("##rquantmenu");
        if (ImGui::BeginPopup("##rquantmenu")) {
          for (const QChoice& c : qs)
            if (ImGui::MenuItem(i18n::tr(c.name), nullptr, c.ticks == clip->playQuantize)) {
              const Tick to = c.ticks;
              editSel("clip playback quantise", [to](Clip& k) { k.playQuantize = to; });
            }
          ImGui::EndPopup();
        }
        int tr = clip->playTranspose;
        if (cardStepper("##rtrans", "Transpose", &tr, -48, 48, 0, signedText(tr, "st"), "Plays the region's notes higher or lower, in semitones (double-click: 0)"))
          editSel("clip transpose", [tr](Clip& k) { k.playTranspose = tr; }, 0x112B0301);
        int vel = clip->playVelocity;
        if (cardStepper("##rvel", "Velocity", &vel, -127, 127, 0, signedText(vel, ""), "Plays the region's notes louder or softer (double-click: 0)"))
          editSel("clip velocity", [vel](Clip& k) { k.playVelocity = vel; }, 0x112B0302);
        int del = static_cast<int>(clip->playDelay);
        if (cardStepper("##rdelay", "Delay", &del, -static_cast<int>(q) * 4, static_cast<int>(q) * 4, 0, signedText(del, "t"),
                        "Plays the region earlier or later, in ticks (double-click: 0)"))
          editSel("clip delay", [del](Clip& k) { k.playDelay = del; }, 0x112B0303);
      }
      cardRow("##rstart", "Position", tickText(a, clip->start), nullptr);
      char len[32];
      const Tick clen = clip->length > 0 ? clip->length : (clip->kind == ClipKind::Pattern && s.pattern(clip->pattern) ? s.patternLength(*s.pattern(clip->pattern)) : 0);
      std::snprintf(len, sizeof len, "%.2f", static_cast<double>(clen) / static_cast<double>(std::max<Tick>(1, s.barTicks())));
      cardRow("##rlen", "Length (bars)", len, nullptr);
      if (clip->kind == ClipKind::Audio && logicplug::active(a)) {
        // 0.7.8: Smooth's audio region: its own settings (gain and pitch, fades, warp, the stretch
        // engine) as the card's rows, with every control the Audio clip window has for a clip; its
        // sample's and channel's settings are the audio editor's local inspector (the lower pane).
        clipeditor::drawInspectorRegion(a, clip->id);
      } else if (clip->kind == ClipKind::Audio) {
        // Gain in whole dB, the pitch as semitones and cents (the region's own, added to its channel's).
        int gdb = clip->gain > 0.00001f ? static_cast<int>(std::lround(20.0 * std::log10(static_cast<double>(clip->gain)))) : -60;
        if (cardStepper("##rgain", "Gain", &gdb, -60, 24, 0, dbText(clip->gain), "The region's level, in dB (double-click: 0 dB)")) {
          const float g = gdb <= -60 ? 0.0f : static_cast<float>(std::pow(10.0, gdb / 20.0));
          editSel("clip gain", [g](Clip& k) { k.gain = g; }, 0x112B0304);
        }
        const int cents = static_cast<int>(std::lround(clip->pitch));
        int semis = cents / 100, fine = cents % 100;
        if (cardStepper("##rpitch", "Transpose", &semis, -24, 24, 0, signedText(semis, "st"), "Plays the region higher or lower, in semitones (double-click: 0)")) {
          const float to = static_cast<float>(semis * 100 + fine);
          editSel("clip pitch", [to](Clip& k) { k.pitch = to; }, 0x112B0305);
        }
        if (cardStepper("##rfine", "Fine tune", &fine, -99, 99, 0, signedText(fine, "ct"), "Fine tuning, in cents (double-click: 0)")) {
          const float to = static_cast<float>(semis * 100 + fine);
          editSel("clip pitch", [to](Clip& k) { k.pitch = to; }, 0x112B0305);
        }
        // R01-13: the fades, as the reference's Region inspector lists them (062): each edge's length
        // and type. Stored on the clip as every layout stores them (Clip::fadeIn / fadeOut, seconds,
        // set by hand; the shape smooth or straight), so the other layouts show the same fades.
        auto msText = [](int ms) { return ms <= 0 ? std::string(i18n::tr("off")) : std::to_string(ms) + " ms"; };
        int fin = static_cast<int>(std::lround(clip->fadeIn * 1000.0)), fout = static_cast<int>(std::lround(clip->fadeOut * 1000.0));
        if (cardStepper("##rfadein", "Fade in", &fin, 0, 20000, 0, msText(fin), "The fade in's length, in milliseconds (double-click: off)")) {
          const double to = std::max(0, fin) / 1000.0;
          editSel("fade in", [to](Clip& k) { k.fadeIn = to, k.fadeInManual = true; }, 0x112B0306);
        }
        const char* shapes[2] = {i18n::N_("Smooth"), i18n::N_("Straight")};
        if (cardRow("##rfadeinty", "Fade in type", i18n::tr(shapes[clip->fadeInShape ? 1 : 0]), "Click: a smooth or a straight fade in", true)) {
          const uint8_t to = clip->fadeInShape ? 0 : 1;
          editSel("fade in type", [to](Clip& k) { k.fadeInShape = to; });
        }
        if (cardStepper("##rfadeout", "Fade out", &fout, 0, 20000, 0, msText(fout), "The fade out's length, in milliseconds (double-click: off)")) {
          const double to = std::max(0, fout) / 1000.0;
          editSel("fade out", [to](Clip& k) { k.fadeOut = to, k.fadeOutManual = true; }, 0x112B0307);
        }
        if (cardRow("##rfadeoutty", "Fade out type", i18n::tr(shapes[clip->fadeOutShape ? 1 : 0]), "Click: a smooth or a straight fade out", true)) {
          const uint8_t to = clip->fadeOutShape ? 0 : 1;
          editSel("fade out type", [to](Clip& k) { k.fadeOutShape = to; });
        }
      }
      cardEnd();
    }
    if (!logicplug::active(a)) ImGui::Dummy(ImVec2(0, 4));  // checkpoint 37: the Tracks layout's headings stack
  } else if (logicplug::active(a)) {
    // K-112D (audit E1): with no region selected the row stays, as "Region Defaults" (216, 110, 233):
    // what a new region on this track starts with (here: as the track's last one, nothing to set yet).
    static bool defOpen = false;
    if (cardFold("##rdeffold", std::string(i18n::tr("Region Defaults")), &defOpen)) {
      cardBegin();
      cardRow("##rdefq", "Quantize", i18n::tr("off"), "New regions play their notes as written");
      cardRow("##rdefl", "Loop", i18n::tr("off"), "New regions play once");
      cardEnd();
    }
  }
  // ---- Track card ----
  const Channel* ch = nullptr;
  const MixerTrack* mt = nullptr;
  if (track >= 0) {
    ch = trackmodes::linkedChannel(s, track);
    mt = trackmodes::linkedMixer(s, track);
    const bool linked = ch || mt;
    if (!linked && clip) {  // a free track: the region's sound
      if (clip->kind == ClipKind::Audio) ch = s.channel(clip->source);
      else if (clip->kind == ClipKind::Pattern)
        if (const Pattern* pat = s.pattern(clip->pattern)) {
          std::vector<const Channel*> chans;
          for (const Channel& c : s.channels)
            if (pat->notes.count(c.id) || pat->steps.count(c.id)) chans.push_back(&c);
          static ObjectId chosen = kNoId;
          for (const Channel* c : chans)
            if (c->id == chosen) ch = c;
          if (!ch && !chans.empty()) ch = chans.front();
          if (chans.size() > 1) {
            if (ui::BeginCombo("##rchan", ch ? ch->name.c_str() : "", ImGui::GetContentRegionAvail().x)) {
              for (const Channel* c : chans)
                if (ImGui::Selectable(c->name.c_str(), c == ch)) chosen = c->id;
              ImGui::EndCombo();
            }
            ui::Hint("The region plays several channels: pick the one whose strip shows here");
          }
        }
    }
    // R01-10: a free track's regions' sound. K-142: without a region of the track selected, the
    // per-song-change memo (the same answer; it was worked out every frame).
    if (!linked && !ch && logicplug::active(a))
      ch = clip && clip->track == track ? logicplug::trackSound(s, track, clip) : logicplug::trackSound(a, track);
    // R05-13: a free track's strip is the one its sounds play into (a kit's bus), not its first sound's.
    if (!linked && logicplug::active(a)) mt = logicplug::trackStrip(s, track);
    if (cardFold("##tfold", std::string(i18n::tr("Track")) + ": " + playlistTrackName(s, static_cast<size_t>(track)), &trackOpen)) {
      cardBegin();
      const TrackMode mode = trackmodes::modeOf(s, track);
      cardRow("##tkind", "Kind", mode == TrackMode::Instrument ? i18n::tr("Instrument") : mode == TrackMode::Audio ? i18n::tr("Audio") : i18n::tr("Free"),
              linked ? nullptr : "A free track holds any region; drag an instrument or a sound onto it to link it");
      if (ch && cardRow("##tchan", "Channel", ch->name, "Click: show the channel", true)) a.selectChannel(ch->id);
      if (cardRow("##tlock", "Locked", s.tracks[static_cast<size_t>(track)].locked ? i18n::tr("on") : i18n::tr("off"), "Click: lock or unlock the track's regions", true)) {
        const bool to = !s.tracks[static_cast<size_t>(track)].locked;
        a.doc.editPlaylistTrack(track, "lock playlist track", [to](PlaylistTrack& t) { t.locked = to; });
      }
      cardEnd();
    }
    if (!logicplug::active(a)) ImGui::Dummy(ImVec2(0, 4));  // checkpoint 37: the Tracks layout's headings stack
  } else if (const Channel* c = s.channel(a.selChannel)) {
    cardFold("##cfold", std::string(i18n::tr("Channel")) + ": " + c->name, &trackOpen);
    ch = c;
  }
  gHeadFirst = false;  // (when no heading was drawn, none later takes the header's place)
  if (ch && !mt) mt = s.mixerTrack(ch->mixerTrack);
  if (!mt && ch && !s.mixer.empty()) mt = &s.mixer[0];  // routed to the master
  if (!clip && track < 0 && !ch && !mt) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 90.0f)), "Nothing selected",
                   "Select a region, a track or a channel: its settings show here");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 90.0f)));
    return;
  }
  // ---- the two strips: the sound's mixer track and where it goes ----
  // Right under the cards, down to the column's foot (the fader takes what is left); a short
  // column scrolls rather than squeezing them.
  if (mt) {
    const ImVec2 av = ImGui::GetContentRegionAvail();
    // K-112b (checkpoint 24): the strips end 2 px above the column's foot; ending on it (and a 1 px
    // item after) overflowed the column by a pixel, so a full-height scrollbar showed beside them.
    const float sh = std::max(av.y - 8, 330.0f);
    {
      const MixerTrack* dest = nullptr;
      if (s.mixerIndex(mt->id) != 0) {
        for (const Send& sd : mt->sends)
          if (!sd.sidechain && !dest) dest = s.mixerTrack(sd.to);
        if (!dest && !s.mixer.empty()) dest = &s.mixer[0];
        // R10-10: after New bus or a click on a send, its bus (while the send is still there)
        if (auto it = gInspDest.find(mt->id); it != gInspDest.end() && logicplug::active(a)) {
          bool still = false;
          for (const Send& sd : mt->sends) still = still || (sd.to == it->second && !sd.stageRoom);
          if (const MixerTrack* b = still ? s.mixerTrack(it->second) : nullptr) dest = b;
        }
      }
      const ImVec2 base = ImGui::GetCursorScreenPos();
      const float y = base.y + 6;
      // D8: the pair spans the column exactly, from the cards' left edge to their right edge, 8 px apart
      // (the right strip ended 3 px short of the cards above it). A strip's slots are its size less 6.
      const float cw = std::floor((av.x - 8) * 0.5f), sw = cw + 6;
      if (!logicplug::active(a)) ImGui::GetWindowDrawList()->AddLine(ImVec2(base.x, y - 4), ImVec2(base.x + av.x, y - 4), pal().line);  // the Tracks layout's heading draws it
      // K-112b (checkpoint 22): in the Tracks layout the pair keeps the same rows (the taller one's
      // effects plus the free slot), so knobs, faders and buttons line up across.
      int pad = 0;
      if (dest && logicplug::active(a)) {
        auto used = [](const MixerTrack& m) {
          int n = 0;
          for (const EffectSlot& e : m.slots) n += e.plugin.type.empty() ? 0 : 1;
          return n;
        };
        pad = std::min(std::max(used(*mt), used(*dest)), 6) + 1;
        auto sendsOf = [&s](const MixerTrack& m) {  // checkpoint 38: the send pills (not the output route)
          int n = 0;
          bool outSeen = false;
          for (const Send& sd : m.sends) {
            if (!sd.sidechain && !outSeen) {
              outSeen = true;
              continue;
            }
            n += !sd.stageRoom && s.mixerIndex(sd.to) >= 0;
          }
          return n;
        };
        gSendRows = std::max(sendsOf(*mt), sendsOf(*dest));
      }
      strip(a, *mt, ch && s.mixerIndex(mt->id) != 0 ? ch : nullptr, ImVec2(base.x, y), ImVec2(sw, sh), -1, nullptr, 0, pad);
      if (dest) {
        strip(a, *dest, nullptr, ImVec2(base.x + av.x - cw, y), ImVec2(sw, sh), -1, nullptr, 0, pad);
      }
      gSendRows = 0;
      ImGui::SetCursorScreenPos(ImVec2(base.x, y + sh - 1));
      ImGui::Dummy(ImVec2(1, 1));
    }
  }
}

// ------------------------------------------------------------------ Library ----
// Sounds first (the built-in instruments' presets by kind of sound: bass, drum kit, keys, ...),
// then every instrument and effect, built-in and installed (the browser's plugin rows: drag one
// onto a track or a slot, double-click to add it), with a search field over both.
void library(App& a) {
  if (logicplug::active(a)) {  // K-112D: Smooth's own Library (SmoothLibrary.h)
    smoothlib::draw(a);
    return;
  }
  static char filter[64] = "";
  if (ownTitle()) cardHeading(i18n::tr("Library"));
  ImGui::SetNextItemWidth(-1);
  // Checkpoint 28: a short placeholder, as 111 and 158 have it (the long one was cut off in the column).
  ImGui::InputTextWithHint("##libfind", i18n::tr("Search"), filter, sizeof filter);
  ui::Hint("Search sounds, instruments and effects: type part of a name to narrow the list");
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::BeginChild("##liblist", ImVec2(0, 0), ImGuiChildFlags_None);
  browserui::drawSounds(a, filter);
  browserui::drawLibrary(a, filter);
  ImGui::EndChild();
}

// -------------------------------------------------------------------- Lists ----
// Checkpoint 28 (Tracks layout): the lists as 087, 128, 172 and 189 draw them: a centred segmented
// control (Marker, Tempo, Signature), then a table of 20 px rows under its header (hairlines between the
// header's columns only), and a footer line with the list's name and count.
void logicLists(App& a) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  static int tab = 0;
  const char* tabs[] = {i18n::tr("Marker"), i18n::tr("Tempo"), i18n::tr("Signature")};
  const char* hints[] = {i18n::tr("The song's markers"), i18n::tr("The song's tempo"), i18n::tr("The song's time signatures")};
  {
    // Checkpoint 29: an add button at the row's left (128 and 087 lead the list with one), the
    // segmented control centred in what is left of the row.
    const float fh = ImGui::GetFrameHeight(), x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    const float W = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(tab != 0);
    if (ui::IconButton("lgaddmk", ui::Icon::Plus, false, fh, "Add a marker at the song position"))
      ops::addMarker(a.doc, static_cast<Tick>(a.engine.position()), ops::nextMarkerName(s));
    ImGui::EndDisabled();
    float segW = std::min(84.0f, std::floor((W - fh * 2 - 16) / 3));
    // R02-21: a narrow pane (1024 x 640) still shows "Signature" whole: the segments take the room
    // the centring kept free before they cut a name.
    float need = 0;
    for (const char* tl : tabs) need = std::max(need, std::ceil(ImGui::CalcTextSize(tl).x) + 14);
    if (segW < need) segW = std::max(segW, std::min(need, std::floor((W - fh - 12) / 3)));
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPos(ImVec2(x0 + std::max(fh + 8, std::floor((W - segW * 3 - 4) * 0.5f)), y0));
    ui::Segmented("##lglists", tabs, 3, &tab, segW, 0, hints);
  }
  ImGui::Dummy(ImVec2(0, 2));
  struct Row {
    Tick pos;
    std::string a, b;
  };
  std::vector<Row> rows;
  const char* cols[3] = {i18n::tr("Position"), i18n::tr("Name"), nullptr};
  char t[32];
  if (tab == 0) {
    for (const Marker& m : s.markers)
      if (!m.isMeter()) rows.push_back({m.pos, m.name, i18n::tr(m.isKey() ? "Key" : "Marker")});
    cols[2] = i18n::tr("Kind");
  } else if (tab == 1) {
    std::snprintf(t, sizeof t, "%.4f", s.tempo);
    rows.push_back({0, t, ""});
    cols[1] = i18n::tr("Tempo");
  } else {
    std::snprintf(t, sizeof t, "%d/%d", s.timeSigNum, s.timeSigDen);
    rows.push_back({0, t, ""});
    for (const Marker& m : s.markers)
      if (m.isMeter()) {
        std::snprintf(t, sizeof t, "%d/%d", m.timeSigNum, m.timeSigDen);
        rows.push_back({m.pos, t, ""});
      }
    cols[1] = i18n::tr("Signature");
  }
  const int nCols = cols[2] ? 3 : 2;
  const float footH = 24;
  const ImVec2 av = ImGui::GetContentRegionAvail();
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 3));
  const float tableH = std::max(40.0f, av.y - footH - ImGui::GetStyle().ItemSpacing.y);
  if (ImGui::BeginTable("##lglist", nCols, ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(av.x, tableH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(cols[0], ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(cols[1], ImGuiTableColumnFlags_WidthStretch, 1.3f);
    if (nCols == 3) ImGui::TableSetupColumn(cols[2], ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableHeadersRow();
    {  // hairlines between the header's columns only, as 087 and 128 draw them (not down the rows)
      ImGuiTable* tb = ImGui::GetCurrentTable();
      const ImRect hr = ImGui::TableGetCellBgRect(tb, 0);
      for (int k = 1; k < nCols; ++k) {
        const float x = std::round(ImGui::TableGetCellBgRect(tb, k).Min.x) - 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, hr.Min.y + 4), ImVec2(x, hr.Max.y - 4), theme::withAlpha(P.text, 0.18f));
      }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, 20);
      if (i % 2) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, theme::withAlpha(P.text, 0.03f));
      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(tickText(a, r.pos).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) a.engine.setPosition(r.pos);
      ui::Hint("Click: move the song position here");
      ImGui::PopID();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.a.c_str());
      if (nCols == 3) {
        ImGui::TableNextColumn();
        ImGui::TextColored(theme::vec(P.textDim), "%s", r.b.c_str());
      }
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  // The footer: what the list holds, its count at the right.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = ImGui::GetCursorScreenPos();
  const float fw = ImGui::GetContentRegionAvail().x;
  dl->AddLine(ImVec2(f0.x, f0.y + 0.5f), ImVec2(f0.x + fw, f0.y + 0.5f), P.dark ? P.ink : P.line);
  const size_t n = rows.size();
  const std::string cnt = tab == 0 ? std::to_string(n) + " " + i18n::tr(n == 1 ? "marker" : "markers")
                          : tab == 1 ? std::string(t)
                                     : std::to_string(n) + " " + i18n::tr(n == 1 ? "signature" : "signatures");
  const float fs = theme::kFontLabel;
  const float ty = theme::textTop(a.fontRegular, fs, f0.y + 1, footH - 1);
  dl->AddText(a.fontRegular, fs, ImVec2(f0.x + 8, ty), P.textDim, tabs[tab]);
  const float cw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, cnt.c_str()).x;
  dl->AddText(a.fontRegular, fs, ImVec2(std::round(f0.x + fw - 8 - cw), ty), P.textDim, cnt.c_str());
  ImGui::Dummy(ImVec2(fw, footH));
}

void lists(App& a) {
  if (logicplug::active(a)) return logicLists(a);
  const Song& s = a.doc.song();
  static int tab = 0;
  const char* tabs[] = {"Markers", "Tempo and meter"};
  ui::TabStrip("##liststabs", tabs, 2, &tab);
  if (tab == 1) {
    ImGui::Text("%s %.2f", i18n::tr("Tempo"), s.tempo);
    ImGui::Text("%s %d/%d", i18n::tr("Meter"), s.timeSigNum, s.timeSigDen);
    for (const Marker& m : s.markers)
      if (m.isMeter()) ImGui::Text("%s  %d/%d", tickText(a, m.pos).c_str(), m.timeSigNum, m.timeSigDen);
    return;
  }
  if (s.markers.empty()) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 90.0f)), "No markers",
                   "Add markers in the playlist's time ruler: they list here");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 90.0f)));
    return;
  }
  if (!ImGui::BeginTable("##markers", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) return;
  ImGui::TableSetupColumn(i18n::tr("Position"));
  ImGui::TableSetupColumn(i18n::tr("Name"));
  ImGui::TableSetupColumn(i18n::tr("Kind"));
  ImGui::TableHeadersRow();
  for (size_t i = 0; i < s.markers.size(); ++i) {
    const Marker& m = s.markers[i];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(tickText(a, m.pos).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) a.engine.setPosition(m.pos);
    ui::Hint("Click: move the song position to this marker");
    ImGui::PopID();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(m.name.c_str());
    ImGui::TableNextColumn();
    ImGui::TextColored(theme::vec(pal().textDim), "%s", i18n::tr(m.isMeter() ? "Meter" : m.isKey() ? "Key" : "Marker"));
  }
  ImGui::EndTable();
}

// ------------------------------------------------------------- Device chain ----
// Which device the chain shows: the instrument, an effect slot or the master chain.
struct ChainPick {
  int kind = 0;  // 0 instrument, 1 effect slot, 2 master chain
  int slot = -1;
  ObjectId fxTrack = kNoId;  // the effect's mixer track (kind 1)
  ObjectId fxSeen = kNoId;
  int fxSlotSeen = -1;
  ObjectId settingsSeen = kNoId;
  bool masterSeen = false;
};
ChainPick gPick;
struct ChainDev {  // dev ("parts chain ..."): a chip click
  int kind = -1, slot = -1;
};
ChainDev gChainDev;

// The Clips device band holds a channel's settings itself when its instrument's home is the band (a
// sampler: the Audio clip window's band form in its card); every other editor opens in its window.
bool bandHostsChannel(const App& a, ObjectId channel, layout::Preset p) {
  const Channel* c = channel != kNoId ? a.doc.song().channel(channel) : nullptr;
  const layout::Item* it = c ? layout::findPlugin(c->plugin.type) : nullptr;
  if (!it) return false;
  const layout::Home& h = it->home[static_cast<int>(p)];
  return h.kind == layout::HomeKind::Host && std::string_view(h.region) == "live.device";
}

// R3, one host per body. Where the layout homes the instrument and effect windows in the chain
// (Host: Live, Tracker), the chain draws the device it shows and those windows stay closed.
// Elsewhere (Studio, Logic) the windows are the devices' home: the chain lists the devices and
// its chips open their windows, it never draws a second copy. The master chain always has its own
// window (its home), so its chip opens it. A Loupe effect shown in the pinned Loupe panel is
// drawn by the chain (the panel steps aside) where the chain hosts effects.
void deviceChain(App& a) {
  ChainPick& pick = gPick;
  const Song& s = a.doc.song();
  const layout::Preset p = layoutui::active();
  const bool hostsIns = layout::windowHome("###channelsettings", p).kind == layout::HomeKind::Host;
  const bool hostsFx = layout::windowHome("###fxeditor", p).kind == layout::HomeKind::Host;
  // A device opened elsewhere (the mixer, the rack, View) shows here.
  bool opened = false;  // K-112c: a device was opened since the last frame (the Live band unfolds it)
  if (a.fxEditor.first != kNoId && (a.fxEditor.first != pick.fxSeen || a.fxEditor.second != pick.fxSlotSeen))
    pick.kind = 1, pick.slot = a.fxEditor.second, pick.fxTrack = a.fxEditor.first, opened = true;
  if (a.settingsChannel != kNoId && a.settingsChannel != pick.settingsSeen) pick.kind = 0, opened = true;
  if (a.showMaster && !pick.masterSeen) pick.kind = 2, opened = true;
  pick.fxSeen = a.fxEditor.first, pick.fxSlotSeen = a.fxEditor.second, pick.settingsSeen = a.settingsChannel, pick.masterSeen = a.showMaster;

  const ObjectId channel = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
  const Channel* c = s.channel(channel);
  const MixerTrack* t = pick.kind == 1 && s.mixerTrack(pick.fxTrack) ? s.mixerTrack(pick.fxTrack) : chainTrack(a, channel);
  const bool onMaster = t && !s.mixer.empty() && t->id == s.mixer[0].id;
  // A chip click (or the dev command): the chain shows that device, or opens its window.
  auto choose = [&](int kind, int slot) {
    pick.kind = kind, pick.slot = slot;
    if (kind == 0 && c) {
      if (!hostsIns) a.settingsChannel = c->id, a.focusWindow = "###channelsettings";
    } else if (kind == 1 && t) {
      pick.fxTrack = t->id;
      if (!hostsFx) a.fxEditor = {t->id, slot}, a.focusWindow = "###fxeditor";
      pick.fxSeen = a.fxEditor.first, pick.fxSlotSeen = a.fxEditor.second;
    } else if (kind == 2) {
      a.showMaster = true, a.focusWindow = "Master chain";
      pick.masterSeen = true;
    }
  };
  if (gChainDev.kind >= 0) choose(std::exchange(gChainDev.kind, -1), gChainDev.slot), opened = true;
  if (livelook::active(a)) {  // K-112c: the Live device band: the same devices side by side, the picked one unfolded
    static livelook::BandPick band;
    band.kind = pick.kind, band.slot = pick.slot, band.fxTrack = pick.fxTrack;
    if (opened) band.open = true;
    livelook::deviceBand(a, band);
    pick.kind = band.kind, pick.slot = band.slot, pick.fxTrack = band.fxTrack;
    // The instrument and effect editors open in their floating windows; a sampler's settings stay in
    // its card (the plugin's home is the band) unless its window button asked for its window.
    if (bandHostsChannel(a, a.settingsChannel, p) && !band.insWindow) hostbody::claim(hostbody::Body::ChannelSettings, "Device chain");
    return;
  }
  // K-106 cp18: the Tracker's own screens for the instrument (the Sample and Instrument editors,
  // Seltmann's). K-391: only those: every plugin's editor and settings float in their windows, so
  // the band never draws a device; with none of its screens asked for it closes.
  if (p == layout::Preset::Tracker) {
    if (!c || !trackereditors::host(a, c->id))
      if (bool* f = flag(a, "Device chain")) *f = false;
    return;
  }
  // The chips: one per device, in signal order.
  if (c && ui::TextButton((std::string(findPlugin(c->plugin.type) ? findPlugin(c->plugin.type)->name : c->name) + "##devins").c_str(), pick.kind == 0, 0,
                          hostsIns ? "The channel's instrument" : "The channel's instrument: opens its window"))
    choose(0, -1);
  if (t)
    for (int k = 0; k < kEffectSlots; ++k) {
      const PluginInfo* fx = findPlugin(t->slots[static_cast<size_t>(k)].plugin.type);
      if (!fx) continue;
      ImGui::SameLine(0, 4);
      if (ui::TextButton((fx->name + "##devfx" + std::to_string(k)).c_str(), pick.kind == 1 && pick.slot == k, 0,
                         hostsFx ? "An effect on the track" : "An effect on the track: opens its window"))
        choose(1, k);
    }
  if (onMaster) {
    ImGui::SameLine(0, 4);
    if (ui::TextButton(i18n::tr("Master chain"), pick.kind == 2, 0, "The master chain's macros and effects: opens its window")) choose(2, -1);
  }
  ImGui::Separator();
  ImGui::BeginChild("##devbody", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  const float w = std::max(320.0f, ImGui::GetContentRegionAvail().x);
  // Where the device lives in its own window: the chain says so and offers to bring it up.
  auto elsewhere = [&](const std::string& name, const char* where, int kind, int slot) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    const float h = std::max(av.y, 60.0f);
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + h - ImGui::GetFrameHeight() - theme::kS2), name.c_str(), where);
    ImGui::Dummy(ImVec2(av.x, h - ImGui::GetFrameHeight() - theme::kS2));
    if (ui::TextButton((std::string(i18n::tr("Show its window")) + "##devshow").c_str(), false, 0, "Brings the device's window to the front"))
      choose(kind, slot);
  };
  const PluginInfo* fxInfo = pick.kind == 1 && t && pick.slot >= 0 && pick.slot < kEffectSlots ? findPlugin(t->slots[static_cast<size_t>(pick.slot)].plugin.type) : nullptr;
  if (pick.kind == 2 && onMaster) {
    elsewhere(i18n::tr("Master chain"), "The master chain has its own window", 2, -1);
  } else if (fxInfo && !hostsFx) {
    elsewhere(fxInfo->name, "This effect opens in its own window", 1, pick.slot);
  } else if (fxInfo) {
    const PluginTarget target{t->id, pick.slot};
    if (packs::showInLoupePanel(a, target)) {  // the pinned Loupe panel's session: the chain draws the panel's body
      drawLoupeBody(a, w, std::max(ImGui::GetContentRegionAvail().y, 200.0f));
      hostbody::claim(hostbody::Body::Loupe, "Device chain");
    } else {
      drawEffectEditorBody(a, t->id, pick.slot, BodyFrame{fxInfo->editorWidth > 0 ? fxInfo->editorWidth : 600.0f, false, "Device chain"});
    }
  } else if (c && !hostsIns) {
    const PluginInfo* info = findPlugin(c->plugin.type);
    elsewhere(info ? info->name : c->name, "The instrument opens in its own window", 0, -1);
  } else if (c) {
    const PluginInfo* info = findPlugin(c->plugin.type);
    drawChannelSettingsBody(a, c->id, BodyFrame{info && info->editorWidth > 0 ? info->editorWidth : 600.0f, false, "Device chain"});
  } else {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 60.0f)), "No device",
                   "Select a channel or a mixer track: its instrument and effects show here");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 60.0f)));
  }
  ImGui::EndChild();
  // Where this layout homes the instrument and effect windows in the chain, the chain is their
  // home while it is open: the windows stay closed (the chips pick which device shows).
  if (hostsIns) hostbody::claim(hostbody::Body::ChannelSettings, "Device chain");
  if (hostsFx) hostbody::claim(hostbody::Body::EffectEditor, "Device chain");
}

// --------------------------------------------------------------- Clip panel ----
void clipPanel(App& a) {
  const Song& s = a.doc.song();
  std::vector<const Clip*> sel;
  for (const Clip& c : s.playlist)
    if (a.playlist.selected.count(c.id)) sel.push_back(&c);
  if (sel.empty()) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 90.0f)), "No clip selected",
                   "Select clips in the playlist: their properties show here");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 90.0f)));
    return;
  }
  for (const Clip* c : sel) {
    ImGui::PushID(static_cast<int>(c->id));
    std::string src;
    if (c->kind == ClipKind::Pattern) {
      const Pattern* p = s.pattern(c->pattern);
      src = p ? p->name : i18n::tr("Pattern");
    } else if (c->kind == ClipKind::Audio) {
      const Channel* ch = s.channel(c->source);
      src = ch ? ch->name : i18n::tr("Audio");
    } else {
      const AutomationClip* au = s.automation(c->source);
      src = au ? au->name : i18n::tr("Automation");
    }
    if (ImGui::CollapsingHeader((src + "##clip").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Text("%s %d", i18n::tr("Track"), c->track + 1);
      ImGui::Text("%s %s", i18n::tr("Start"), tickText(a, c->start).c_str());
      ImGui::Text("%s %.2f %s", i18n::tr("Length"), static_cast<double>(c->length) / std::max<Tick>(1, s.barTicks()), i18n::tr("bars"));
      const ObjectId cid = c->id;
      bool loop = c->loop, muted = c->muted;
      if (ImGui::Checkbox(i18n::tr("Loop"), &loop))
        a.doc.editSong("loop clip", [cid, loop](Song& x) {
          for (Clip& k : x.playlist)
            if (k.id == cid) k.loop = loop;
        });
      ImGui::SameLine();
      if (ImGui::Checkbox(i18n::tr("Mute"), &muted))
        a.doc.editSong("mute clip", [cid, muted](Song& x) {
          for (Clip& k : x.playlist)
            if (k.id == cid) k.muted = muted;
        });
      if (c->kind == ClipKind::Pattern) {  // K-112b3: how the clip plays its pattern (the pattern stays as written)
        auto drag = [&](const char* id, const char* label, int v, int lo, int hi, const char* fmt, uint64_t key, auto set) {
          ImGui::SetNextItemWidth(std::min(160.0f, ImGui::GetContentRegionAvail().x * 0.6f));
          if (ImGui::DragInt(id, &v, 0.25f, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp))
            a.doc.editClip(cid, label, [v, set](Clip& k) { set(k, v); }, key);
          if (ImGui::IsItemDeactivated()) a.doc.endGesture();
        };
        drag("##ptrans", "clip transpose", c->playTranspose, -48, 48, i18n::tr("Transpose %+d st"), 0x112B0311, [](Clip& k, int v) { k.playTranspose = v; });
        ui::Hint("Plays the clip's notes higher or lower, in semitones");
        drag("##pvel", "clip velocity", c->playVelocity, -127, 127, i18n::tr("Velocity %+d"), 0x112B0312, [](Clip& k, int v) { k.playVelocity = v; });
        ui::Hint("Plays the clip's notes louder or softer");
        const int q = static_cast<int>(std::max<Tick>(1, s.ppq));
        drag("##pdelay", "clip delay", static_cast<int>(c->playDelay), -q * 4, q * 4, i18n::tr("Delay %+d ticks"), 0x112B0313,
             [](Clip& k, int v) { k.playDelay = v; });
        ui::Hint("Plays the clip earlier or later, in ticks");
        const Tick qs[] = {0, q, q / 2, q / 4, q / 8, q / 3, q / 6};
        const char* qn[] = {i18n::N_("Quantize: off"), "1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T"};
        int cur = 0;
        for (int i = 0; i < 7; ++i)
          if (qs[i] == c->playQuantize) cur = i;
        ImGui::SetNextItemWidth(std::min(160.0f, ImGui::GetContentRegionAvail().x * 0.6f));
        if (ImGui::BeginCombo("##pquant", i18n::tr(qn[cur]))) {
          for (int i = 0; i < 7; ++i)
            if (ImGui::Selectable(i18n::tr(qn[i]), i == cur)) {
              const Tick to = qs[i];
              a.doc.editClip(cid, "clip playback quantise", [to](Clip& k) { k.playQuantize = to; });
            }
          ImGui::EndCombo();
        }
        ui::Hint("Snaps the clip's notes to a grid as it plays");
      }
    }
    ImGui::PopID();
  }
}

// ------------------------------------------------------------- Track scopes ----
void trackScopes(App& a) {
  const Song& s = a.doc.song();
  ImGui::BeginChild("##scopes", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  const float h = std::max(40.0f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() - 4);
  for (size_t i = 0; i < s.mixer.size(); ++i) {
    const mixmeters::Reading r = mixmeters::get(a, s.mixer[i].id);
    ImGui::BeginGroup();
    ImGui::PushID(static_cast<int>(i));
    ui::Meter(ImGui::GetID("m"), r.peakL, r.peakR, ImVec2(12, h));
    ImGui::TextColored(theme::vec(pal().textFaint), "%s", i == 0 ? "M" : std::to_string(i).c_str());
    ImGui::PopID();
    ImGui::EndGroup();
    ui::Hint(i == 0 ? std::string(i18n::tr("Master")) : mixerTrackName(s, i));
    ImGui::SameLine(0, 6);
  }
  ImGui::NewLine();
  ImGui::EndChild();
}

// ----------------------------------------------------------------- Info box ----
void infoBox(App& a) {
  if (livelook::active(a)) return livelook::infoCorner(a);  // K-112c: a title and its text, the Live way
  static std::string last;
  if (!a.hint.empty()) last = a.hint;
  if (ownTitle()) cardHeading(i18n::tr("Quick help"));
  ImGui::PushTextWrapPos(0);
  if (last.empty()) ImGui::TextColored(theme::vec(pal().textDim), "%s", i18n::tr("Point at anything: what it does shows here"));
  else ImGui::TextUnformatted(last.c_str());
  ImGui::PopTextWrapPos();
}

// R01-08: the Tracks layout's Quick help: a small floating window (as the reference's), not a pane.
// It shows what the item under the pointer does and stays where the person drags it; its own close
// box, the control bar's "?" and Shift+/ turn it off.
void quickHelpFloat(App& a) {
  if (!logicplug::active(a) || !a.settings.getBool("ui.logicQuickHelp")) return;
  static std::string last;
  if (!a.hint.empty()) last = a.hint;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 300, vp->WorkPos.y + 140), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_Always);
  bool open = true;
  if (ImGui::Begin((std::string(i18n::tr("Quick help")) + "###lgquickhelp").c_str(), &open,
                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 244);
    if (last.empty()) ImGui::TextColored(theme::vec(pal().textDim), "%s", i18n::tr("Point at anything: what it does shows here"));
    else ImGui::TextUnformatted(last.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
  if (!open) a.settings.set("ui.logicQuickHelp", false);
}

// -------------------------------------------------------------------- Files ----
struct FileRow {
  std::string path, owner;
  bool missing = false;
};

void files(App& a) {
  const Song& s = a.doc.song();
  // The list is rebuilt when the project's files change (existence is checked then, not every frame).
  static std::vector<FileRow> rows;
  static std::string key;
  std::string now;
  std::vector<FileRow> fresh;
  for (const Channel& c : s.channels)
    if (!c.plugin.data.empty() && c.plugin.type == "gl.sampler") {
      now += c.plugin.data + "\n";
      fresh.push_back({c.plugin.data, c.name, false});
    }
  if (now != key) {
    key = now;
    std::error_code ec;
    for (FileRow& r : fresh) r.missing = !fs::exists(fs::path(r.path), ec);
    rows = std::move(fresh);
  }
  if (rows.empty()) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 90.0f)), "No audio files",
                   "Samples the project uses list here");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 90.0f)));
    return;
  }
  if (!ImGui::BeginTable("##files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) return;
  ImGui::TableSetupColumn(i18n::tr("File"));
  ImGui::TableSetupColumn(i18n::tr("Used by"));
  ImGui::TableHeadersRow();
  for (const FileRow& r : rows) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const std::string name = fs::path(r.path).filename().string();
    if (r.missing) ImGui::TextColored(theme::vec(pal().danger), "%s (%s)", name.c_str(), i18n::tr("missing"));
    else ImGui::TextUnformatted(name.c_str());
    ui::Hint(r.path);
    ImGui::TableNextColumn();
    ImGui::TextColored(theme::vec(pal().textDim), "%s", r.owner.c_str());
  }
  ImGui::EndTable();
}

// -------------------------------------------------------------------- Notes ----
struct NotesDev {
  bool focus = false, blur = false;
};

// The notes text grows as typed (no fixed buffer: long notes are never cut, nor a character split).
int growString(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* s = static_cast<std::string*>(d->UserData);
    s->resize(static_cast<size_t>(d->BufTextLen));
    d->Buf = s->data();
  }
  return 0;
}

NotesDev gNotesDev;

void notes(App& a) {
  const Song& s = a.doc.song();
  static std::string buf;
  static bool editing = false;
  if (!editing) buf = s.info.comments;  // a collaborator's edit shows at once
  if (gNotesDev.focus) ImGui::SetKeyboardFocusHere(), gNotesDev.focus = false;
  if (gNotesDev.blur && editing) ImGui::ClearActiveID(), gNotesDev.blur = false;
  // K-297 (round 6): Esc ends the typing and keeps the notes. The field put back the text it had
  // when the typing began (a text field's Esc), so everything typed since the click was lost.
  if (editing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::ClearActiveID();
  ImGui::InputTextMultiline("##notes", buf.data(), buf.capacity() + 1, ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_CallbackResize,
                            growString, &buf);
  editing = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit() && s.info.comments != buf) {  // one undo step per edit
    SongInfo info = s.info;
    info.comments = buf;
    a.doc.setInfo(info);
  }
  ui::Hint("The project's notes (saved with it, shared with collaborators)");
}

struct PartDef {
  const char* window;
  ImVec2 size;
  void (*draw)(App&);
};
const PartDef kParts[] = {
    {"Inspector", {280, 520}, inspector}, {"Library", {280, 560}, library},       {"Lists", {420, 360}, lists},
    {"Device chain", {900, 260}, deviceChain}, {"Clip panel", {320, 360}, clipPanel}, {"Track scopes", {520, 200}, trackScopes},
    {"Info box", {280, 120}, infoBox},    {"Files", {420, 360}, files},             {"Notes", {420, 320}, notes},
    {"Smart controls", {640, 240}, logicplug::smartControls},
    {"Pattern editor", {960, 560}, trackerview::patternEditor}, {"Order list", {300, 360}, trackerview::orderList},
    {"Seltmann", {1200, 420}, packs::seltmannPane},
    {"Chop", {1200, 640}, packs::seltmannChopPage},  // K-347: Seltmann's page in the Tracker
};
// K-112b: built for the Tracks layout so far; the other layouts' menus do not list it yet.
bool partListed(const char* window) {
  // N45-09: the Tracker's panels are the Tracker's; Clips does not list them.
  const bool tracker = std::strcmp(window, "Pattern editor") == 0 || std::strcmp(window, "Order list") == 0;
  if (tracker && layoutui::active() == layout::Preset::Live) return false;
  // 0.7.8: Seltmann's pane tab is Smooth's (elsewhere its editor is the instrument window or the device band)
  if (std::strcmp(window, "Seltmann") == 0) return layoutui::active() == layout::Preset::Logic;
  // K-347: the Chop page is the Tracker's (a page beside its others while the project has a Seltmann)
  if (std::strcmp(window, "Chop") == 0) return layoutui::active() == layout::Preset::Tracker;
  return std::strcmp(window, "Smart controls") != 0 || layoutui::active() == layout::Preset::Logic;
}

// D9 (R14-12): toggles (any number on) drawn as Smooth's segmented control: one dark capsule, each lit
// segment in the selection blue, the same height, insets and text as the view switch beside them.
// `names` and `hints` are untranslated (N_). True when a bit of *bits changed.
bool kindToggles(const char* id, const char* const* names, const char* const* hints, int count, int* bits, float segW) {
  const auto& P = pal();
  const float h = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + segW * static_cast<float>(count) + 4, p.y + h), theme::mix(P.surface900, P.ink, 0.25f), theme::kRadius + 1);
  const ImU32 blue = theme::rgb(0x2F7EDB);
  bool changed = false;
  ImGui::PushID(id);
  for (int i = 0; i < count; ++i) {
    ImGui::PushID(i);
    const ImVec2 q(p.x + 2 + segW * static_cast<float>(i), p.y + 2);
    ImGui::SetCursorScreenPos(q);
    if (ImGui::InvisibleButton("##kind", ImVec2(segW, h - 4))) {
      *bits ^= 1 << i;
      changed = true;
    }
    const bool hov = ImGui::IsItemHovered(), on = (*bits >> i) & 1;
    if (on) {
      dl->AddRectFilled(q, ImVec2(q.x + segW, q.y + h - 4), hov ? theme::mix(blue, P.paper, 0.10f) : blue, std::max(1.0f, theme::kRadius - 1));
    } else if (hov) {
      dl->AddRectFilled(q, ImVec2(q.x + segW, q.y + h - 4), theme::withAlpha(P.surface600, 0.6f), std::max(1.0f, theme::kRadius - 1));
    }
    theme::centreText(dl, ImGui::GetFont(), ImGui::GetFontSize(), q, ImVec2(q.x + segW, q.y + h - 4), on ? theme::contrastText(blue) : hov ? P.text : P.textDim,
                      i18n::tr(names[i]));
    if (hints && hints[i]) {
      ui::Hint(i18n::tr(hints[i]));
      ui::Tooltip(i18n::tr(hints[i]));
    }
    ImGui::PopID();
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(segW * static_cast<float>(count) + 4, h));
  return changed;
}

}  // namespace

// K-112b: the Logic layout's mixer: a legend column, then one strip per mixer track (the inspector's
// strips, lined up), a view filter above. The detailed mixer (slots, routing, stage) is one click away.
// GitHub #58: a slim column in Smooth's mixer, for a narrow track (its name, meter, mute and solo) or
// a folded group (its name and size, its loudest level, mute and solo for every member, an arrow that
// expands it). Square, as Smooth's strips; the track's colour bar along the foot as their name bars.
static void smoothSlimStrip(App& a, const mixhide::Entry& e, ImVec2 p, float w, float h) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const MixerTrack& t = s.mixer[e.index];
  const bool group = e.show == mixhide::Show::Group;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(group ? static_cast<int>(ImGui::GetID(e.group.c_str())) : static_cast<int>(t.id));
  ImGui::PushID("slim");
  bool on = !group && a.selMixer == t.id;
  if (group)
    for (const MixerTrack& m : s.mixer) on = on || (m.group == e.group && m.id == a.selMixer);
  dl->AddRectFilled(p, ImVec2(p.x + w - 1, p.y + h), on ? theme::mix(P.surface800, P.text, 0.07f) : P.surface800);
  if (group)  // a stack of strips: a second edge down the right
    dl->AddRectFilled(ImVec2(p.x + w - 4, p.y + 3), ImVec2(p.x + w - 2, p.y + h - 3), theme::mix(P.surface800, P.ink, 0.35f));
  const float cx = std::round(p.x + (w - 1) * 0.5f);
  // The top: a group's arrow (expands it), a track's number.
  const float topH = 20;
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 2));
  const bool topClick = ImGui::InvisibleButton("##top", ImVec2(w - 1, topH));
  const bool topHov = ImGui::IsItemHovered();
  if (group) {
    ui::drawIcon(dl, ui::Icon::Chevron, ImVec2(cx, p.y + 2 + topH * 0.5f), 10, topHov ? P.text : P.hot);
    if (topClick) mixhide::setGroupFolded(a.doc, e.group, false);
    if (topHov) ui::Hint("Expand the group");
  } else {
    const std::string num = std::to_string(e.index);
    const float nw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, num.c_str()).x;
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(cx - nw * 0.5f), std::round(p.y + 2 + (topH - theme::kFontLabel) * 0.5f)), on ? P.text : P.textDim,
                num.c_str());
    if (topClick) a.selMixer = t.id;
    if (topHov) ui::Hint("Narrow strip: click to select, double-click for the full strip, right-click for the track menu");
    if (topHov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) mixhide::setNarrow(a.doc, {t.id}, false);
  }
  // The foot: the colour bar, then M and S above it, the meter above them, the name above the meter.
  const float plateH = 22, bh = 18;
  const float plateY = p.y + h - plateH;
  dl->AddRectFilled(ImVec2(p.x, plateY), ImVec2(p.x + w - 1, p.y + h), theme::mix(P.surface700, theme::content(t.color), 0.85f));
  const bool muted = group ? mixhide::groupMuted(s, e.group) : t.muted;
  const bool soloed = group ? mixhide::groupSoloed(s, e.group) : t.solo;
  auto btn = [&](const char* id, const char* label, bool lit, ImU32 litCol, float y) {
    const float bw = w - 9, bx = p.x + 4;
    ImGui::SetCursorScreenPos(ImVec2(bx, y));
    const bool c = ImGui::InvisibleButton(id, ImVec2(bw, bh));
    const bool hv = ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + bh), lit ? litCol : hv ? theme::mix(P.surface700, P.text, 0.12f) : theme::mix(P.surface700, P.text, 0.05f), 4.0f);
    const ImVec2 ts = a.fontBold->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, label);
    dl->AddText(a.fontBold, theme::kFontLabel, ImVec2(std::round(bx + (bw - ts.x) * 0.5f), std::round(y + (bh - ts.y) * 0.5f)),
                lit ? theme::rgb(0x1A1A1C) : theme::mix(P.textDim, P.text, 0.65f), label);
    return c;
  };
  const float sY = plateY - 4 - bh, mY = sY - 4 - bh;
  if (btn("##m", "M", muted, P.hot, mY)) {
    if (group) mixhide::setGroupMute(a.doc, e.group, !muted);
    else a.doc.setParam(mixerParam(t.id, MixerParam::Mute), t.muted ? 0 : 1, false);
  }
  if (ImGui::IsItemHovered()) {
    if (group) ui::Hint(muted ? "Every track in the group is muted: click to unmute them" : "Click to mute every track in the group");
    else ui::Hint(muted ? "Track is muted: click to unmute" : "Track is on: click to mute");
  }
  if (btn("##s", "S", soloed, theme::rgb(0xE8C83A), sY)) {
    if (group) mixhide::setGroupSolo(a.doc, e.group, !soloed);
    else mixops::toggleSolo(a.doc, t.id);
  }
  if (ImGui::IsItemHovered()) ui::Hint(group ? "Solo every track in the group" : "Solo (keeps tracks routed to and from it)");
  float pl = 0, pr = 0;
  for (const MixerTrack& m : s.mixer)
    if (group ? m.group == e.group : m.id == t.id) {
      const mixmeters::Reading r = mixmeters::get(a, m.id);
      pl = std::max(pl, r.peakL), pr = std::max(pr, r.peakR);
    }
  const float meterH = std::round(std::clamp(h * 0.32f, 40.0f, 160.0f));
  const float my1 = mY - 8, my0 = my1 - meterH;
  ImGui::SetCursorScreenPos(ImVec2(cx - 6, my0));
  smoothMeter(ImGui::GetID("meter"), pl, pr, ImVec2(12, meterH));
  const float nameTop = p.y + 2 + topH + 6, nameBottom = my0 - 8;
  if (nameBottom - nameTop > 16) {
    const float fs = theme::kFontCaption;
    std::string name = group ? e.group + "  \xC2\xB7  " + std::to_string(e.members) : mixerTrackName(s, e.index);
    mixhideui::upwardText(dl, a.fontRegular, fs, ImVec2(std::round(cx - fs * 0.5f - 1), nameBottom), on ? P.text : P.textDim, name, nameBottom - nameTop);
  }
  // Right-click anywhere on the column: the track menu (a group: its own).
  if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w - 1, p.y + h)) && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    ImGui::OpenPopup("##slimmenu");
  if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w - 1, p.y + h)) && ImGui::IsWindowHovered() && group && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    mixhide::setGroupFolded(a.doc, e.group, false);
  if (ImGui::BeginPopup("##slimmenu")) {
    if (group) mixhideui::groupMenu(a, e.group);
    else mixerTrackMenu(a, e.index);
    ImGui::EndPopup();
  }
  ImGui::PopID();
  ImGui::PopID();
}

void logicMixer(App& a) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  if (s.mixer.empty()) return;
  static int view = 1;  // 0 the selected track and where it goes, 1 the tracks in use, 2 every track
  if (const int dv = mixhideui::devSmoothView(); dv >= 0) view = dv;  // (dev: "mixhide smoothview N")
  // K-112D (audit F2): the strip kinds shown (the reference's type buttons, 134): bit 0 instrument
  // strips, 1 audio strips, 2 buses (what other strips send into), 3 the master. All on at first.
  static int kindsOn = 15;
  {  // the local bar: Edit, Options and View on the left, the view in the middle, the kinds on the right (134)
    // D8: the editors' own bar, as the piano roll's: the same 36 px height, 8 px insets, ground and foot
    // line, and the three menus as one group. They were loose buttons flush with the pane's top left
    // corner, 7 px above the piano roll's and Smart Controls' bars.
    ui::BeginToolbar("##lgmixbar");
    const float y0 = ImGui::GetCursorPosY();
    {
      static const char* const names[] = {i18n::N_("Edit"), i18n::N_("Options"), i18n::N_("View")};
      static const char* const mhints[] = {i18n::N_("Undo and redo the mixer's changes"), i18n::N_("Groups and the selected strip"),
                                           i18n::N_("Which strips show, and the detailed mixer")};
      ui::MenuGroup("##mixmenus", names, mhints, 3, [&a, &s](int k) {
        if (k == 1) {
          if (ImGui::MenuItem(i18n::tr("Undo"))) {
            a.doc.undo();
          }
          if (ImGui::MenuItem(i18n::tr("Redo"))) {
            a.doc.redo();
          }
        } else if (k == 2) {
          const MixerTrack* selT = s.mixerTrack(a.selMixer);
          const bool canGroup = selT && s.mixerIndex(selT->id) > 0;
          if (ImGui::MenuItem(i18n::tr("Create group from the selected strip..."), nullptr, false, canGroup)) {
            const ObjectId id = selT->id;
            menus::openPrompt("Group name", mixops::newGroupName(s), [&a, id](const std::string& n) {
              if (!n.empty()) {
                mixops::createGroup(a.doc, {id}, n, true);
              }
            });
          }
          if (ImGui::MenuItem(i18n::tr("Remove the selected strip from its group"), nullptr, false, canGroup && !selT->group.empty())) {
            mixops::setGroup(a.doc, {selT->id}, "");
          }
        } else {
          const char* vn[] = {i18n::N_("Single"), i18n::N_("Tracks"), i18n::N_("All")};
          for (int k2 = 0; k2 < 3; ++k2) {
            if (ImGui::MenuItem(i18n::tr(vn[k2]), nullptr, view == k2)) {
              view = k2;
            }
          }
          ImGui::Separator();
          if (ImGui::MenuItem(i18n::tr("Detailed mixer"))) {
            a.settings.set("ui.logicMixerDetail", true);
          }
        }
      });
    }
    // GitHub #58: how many tracks have no strip of their own, beside the menus; the list shows them
    if (const std::string hl = mixhideui::hiddenLabel(s); !hl.empty()) {
      ImGui::SameLine(0, 8);
      ImGui::SetCursorPosY(y0);
      if (ui::TextButton(hl.c_str(), false, 0, "Tracks hidden from the mixer (they still play): click to show them")) ImGui::OpenPopup("##lghidden");
      devMenuItem("mixer hidden"), devNoteRect("mixhidden");
      if (ImGui::BeginPopup("##lghidden")) {
        mixhideui::hiddenMenu(a);
        ImGui::EndPopup();
      }
    }
    const char* views[] = {i18n::tr("Single"), i18n::tr("Tracks"), i18n::tr("All")};  // K-112D: the reference's names (134)
    const char* hints[] = {i18n::tr("The selected track and where it goes"), i18n::tr("The tracks that something plays through, and the master"),
                           i18n::tr("Every mixer track")};
    const float segW = 74;
    ImGui::SameLine();
    const float bx = ImGui::GetWindowContentRegionMin().x;
    ImGui::SetCursorPos(ImVec2(std::max(ImGui::GetCursorPosX() + 8, bx + (ImGui::GetContentRegionMax().x - bx - segW * 3) * 0.5f), y0));
    ui::Segmented("##lgmixview", views, 3, &view, segW, 0, hints);
    // The kinds at the right edge. D9 (R14-12): in the views' own style, one capsule whose segments light
    // in the selection blue (any number of them on), not bare text beside a segmented control.
    const char* kn[] = {i18n::N_("Inst"), i18n::N_("Audio"), i18n::N_("Bus"), i18n::N_("Master")};
    const char* kh[] = {i18n::N_("Instrument strips: show or hide them"), i18n::N_("Audio strips: show or hide them"),
                        i18n::N_("Buses (strips other strips send into): show or hide them"), i18n::N_("The master strip: show or hide it")};
    float kseg = 52;
    for (const char* k : kn) {
      kseg = std::max(kseg, std::ceil(ImGui::CalcTextSize(i18n::tr(k)).x) + 2 * ImGui::GetStyle().FramePadding.x);
    }
    const float kw = kseg * 4 + 4;  // D8: it ends on the bar's 8 px inset
    ImGui::SameLine();
    ImGui::SetCursorPos(ImVec2(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetWindowContentRegionMax().x - kw), y0));
    kindToggles("##lgmixkinds", kn, kh, 4, &kindsOn, kseg);
    ui::EndToolbar();
    // the strips start on the bar's foot line (no item gap: the strips keep the rows they had)
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
  }
  // Which tracks. D5 (CPU): worked out again only when the song, the view, the kinds or the selected
  // strip change (it went through every channel and send for every strip, every frame).
  struct OrderMemo {
    uint64_t rev = ~0ull;
    const Song* song = nullptr;
    int view = -1, kinds = -1;
    ObjectId sel = kNoId;
    std::vector<size_t> order;
    std::vector<mixhide::Entry> entries;  // GitHub #58: how each of `order` shows
    int fxRows = 2, sendRows = 0, fxMost = 0;  // fxMost: the most effects any strip shown has
  };
  static OrderMemo om;
  if (om.rev != a.doc.revision() || om.song != &s || om.view != view || om.kinds != kindsOn || om.sel != a.selMixer) {
    om.rev = a.doc.revision(), om.song = &s, om.view = view, om.kinds = kindsOn, om.sel = a.selMixer;
    std::vector<size_t>& order = om.order;
    order.clear();
    // D6 (R12-05): the Tracks view shows what something plays through, as the reference's mixer shows its
    // tracks: the strips of the sounds that play (on a track, or in a region of the arrangement), the
    // tracks' own strips and the strips automated there, and what those send into. A sound no track
    // plays (a template's spare channels) adds no strip. (View only: the song is not changed.)
    std::set<ObjectId> used, played;
    for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t) {
      if (const Channel* c = trackmodes::linkedChannel(s, t)) played.insert(c->id);
      if (const MixerTrack* lm = trackmodes::linkedMixer(s, t)) used.insert(lm->id);
      if (s.tracks[static_cast<size_t>(t)].sound != kNoId) played.insert(s.tracks[static_cast<size_t>(t)].sound);
    }
    std::set<ObjectId> pats;
    for (const Clip& k : s.playlist) {
      if (k.kind == ClipKind::Audio) {
        played.insert(k.source);
      } else if (k.kind == ClipKind::Pattern) {
        pats.insert(k.pattern);
      } else if (k.kind == ClipKind::Automation) {
        if (const AutomationClip* ac = s.automation(k.source); ac && s.mixerTrack(ac->owner)) used.insert(ac->owner);
      }
    }
    for (const ObjectId pid : pats) {
      const Pattern* p = s.pattern(pid);
      if (!p) continue;
      for (const Channel& c : s.channels) {
        if (p->notes.count(c.id) || p->steps.count(c.id)) played.insert(c.id);
      }
    }
    for (const Channel& c : s.channels) {
      if (played.count(c.id)) used.insert(c.mixerTrack);
    }
    for (bool grew = true; grew;) {  // and what they send into, down the chain
      grew = false;
      for (const MixerTrack& m : s.mixer) {
        if (!used.count(m.id)) continue;
        for (const Send& sd : m.sends) grew = used.insert(sd.to).second || grew;
      }
    }
    const MixerTrack* sel = s.mixerTrack(a.selMixer);
    const MixerTrack* selDest = nullptr;
    if (sel && s.mixerIndex(sel->id) != 0) {
      for (const Send& sd : sel->sends)
        if (!sd.sidechain && !selDest) selDest = s.mixerTrack(sd.to);
      if (!selDest) selDest = &s.mixer[0];
    }
    // K-112D: a strip's kind: an instrument strip (a sound plays into it), an audio strip (audio regions'
    // sounds), else a bus.
    auto kindOf = [&s](const MixerTrack& m) {
      bool inst = false, audio = false;
      for (const Channel& c : s.channels)
        if (c.mixerTrack == m.id) {
          const PluginInfo* pi = findPlugin(c.plugin.type);
          (pi && pi->usesSample && c.audioClip ? audio : inst) = true;
        }
      return inst ? 0 : audio ? 1 : 2;
    };
    for (size_t i = 1; i < s.mixer.size(); ++i) {
      const MixerTrack& m = s.mixer[i];
      if (!((kindsOn >> kindOf(m)) & 1) && !(view == 0 && sel && m.id == sel->id)) continue;
      if (view == 2 || (view == 1 && (used.count(m.id) || (sel && m.id == sel->id))) ||
          (view == 0 && sel && (m.id == sel->id || (selDest && m.id == selDest->id))))
        order.push_back(i);
    }
    if (kindsOn & 8) order.push_back(0);  // the master at the right end
    // GitHub #58: hidden tracks left out, a folded group as one slim column, narrow tracks slim
    om.entries = mixhide::arrange(s, order);
    order.clear();
    for (const mixhide::Entry& e : om.entries) order.push_back(e.index);
    om.fxRows = 2, om.fxMost = 0;
    for (size_t i : order) {
      int n = 0;
      for (const EffectSlot& e : s.mixer[i].slots) n += e.plugin.type.empty() ? 0 : 1;
      om.fxRows = std::max(om.fxRows, std::min(n, kEffectSlots) + 1);  // D6 (R12-09): every insert when there is room
      om.fxMost = std::max(om.fxMost, std::min(n, kEffectSlots));
    }
    // K-112D: each send its own row, as 134 lists them (the most any strip shown has, up to three), and
    // the empty Sends slot under them.
    om.sendRows = 0;
    for (size_t i : order) {
      int n = 0;
      bool out = false;
      for (const Send& sd : s.mixer[i].sends) {
        if (!sd.sidechain && !out) {
          out = true;
          continue;
        }
        if (!sd.stageRoom && s.mixerIndex(sd.to) >= 0) ++n;
      }
      om.sendRows = std::max(om.sendRows, std::min(n, 3));
    }
  }
  const std::vector<size_t>& order = om.order;
  if (order.empty()) return;
  int fxRows = om.fxRows, sendRows = om.sendRows;
  // The legend, then the strips.
  // K-112b (checkpoint 20): the strips fit the pane, as the references' mixers do at small sizes (they
  // scrolled at 1280x800). Sideways the strips narrow (84 px down to 64) before the pane scrolls;
  // downwards the optional rows go first (StripHide order), the fader keeping at least 48 px.
  float legendW = 84;
  const ImVec2 av = ImGui::GetContentRegionAvail();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  // GitHub #58: slim columns keep their own width; the full strips share the rest.
  constexpr float kSlimColW = 34.0f;
  int slimCount = 0;
  for (const mixhide::Entry& e : om.entries) slimCount += e.show != mixhide::Show::Strip;
  const int fullCount = std::max(1, static_cast<int>(order.size()) - slimCount);
  const float slimTotal = kSlimColW * static_cast<float>(slimCount);
  float stripW = std::clamp(std::floor((av.x - legendW - slimTotal) / static_cast<float>(fullCount)), 64.0f, 90.0f);  // K-112D (audit F9): room for "Parametric EQ"
  // D9 (R14-12): when the strips scroll, the view holds whole strips (at 1920 the last one was cut mid-strip):
  // as many as fit at 64 px, widened to share the room, the odd pixels left to the legend (its names are
  // right-aligned, so they stay beside the strips). The scroll then steps a strip at a time (below).
  const bool scrolls = stripW * static_cast<float>(fullCount) + slimTotal > av.x - legendW;
  if (scrolls) {
    const float room = av.x - legendW;
    const float fit = std::max(1.0f, std::floor(room / 64.0f));
    stripW = std::max(64.0f, std::floor(room / fit));
    legendW += std::max(0.0f, room - stripW * fit);
  }
  const float sh = std::max(av.y - 14, 120.0f);
  int hide = 0;
  {
    // The strip's height in rows (see strip()): settings 19, EQ 26, instrument 19, effects 19 each,
    // sends 19 each and the empty one, output 17 and 6 below, group and automation 19 each; icon 30,
    // pan 26, level 22, fader, mute and solo 22, name 20.
    // D4 (R10-04): the fader first aims for a third of the pane (134, 043: the fader and meter are the
    // strip's main part; at 1080 it had 90 px, 15 %); the optional rows step out for it in this order,
    // and only when they are all gone does the fader settle for less (down to kMixFaderMin).
    float faderT = std::clamp(std::round(sh * kMixFaderShare), kMixFaderMin, kMixFaderMax);
    auto need = [&](int h) {
      return 106.0f + 19.0f * static_cast<float>(fxRows) - (h & HideSetting ? 19.0f : 0.0f) - (h & HideEq ? 26.0f : 0.0f) -
             (h & HideSends ? 19.0f : -19.0f * static_cast<float>(sendRows) + (h & HideSendAdd ? 19.0f : 0.0f)) + (h & HideGroupAuto ? 0.0f : 38.0f) +
             (h & (HideIcon | IconInline) ? 0.0f : h & IconSmall ? 22.0f : 30.0f) + (h & HidePan ? 0.0f : h & PanSmall ? 28.0f : 32.0f) + (h & HideLevel ? 0.0f : 22.0f) +
             (h & HideShort ? 32.0f : h & HideMid ? 48.0f : faderT) + 22 + 20 + 8;
    };
    // D6 (R12-09): the inserts unfold when there is room: the fader first gives up its target down to a
    // quarter of the pane (it kept a third while effects folded into "+2" and "+3"), when that is enough
    // to show every row; otherwise the rows step out as before (a short pane keeps its fader).
    // D7 (R13-07): and short of that it still gives what it can (down to a fifth of the pane), so a tall
    // pane unfolds every insert it can before any folds into "+N" (at 2560x1440 they folded with a 260 px fader).
    // (Shorter panes keep the earlier rule: there the optional rows step out first and the fader stays.)
    // D9 (R14-11): a tall pane (2560 x 1440) gave the fader a fifth of it (about 114 px of 660) to keep rows
    // that were mostly empty. Now, while the fader would get less than 28 % of the pane, the quiet rows go
    // first, none of which hides an effect or a send: the add-a-send slot folds into the first empty send
    // row, the inserts' spare row goes (the fullest strip shows its effects whole, the others keep their
    // free slot), then the Setting row (its menu is also on the name's right-click).
    if (sh >= 560) {
      const float goal = std::max(kMixFaderMin, std::round(sh * 0.28f));
      auto shortOf = [&] { return faderT - (need(hide) - (sh - 6)) < goal; };
      if (sendRows > 0 && shortOf()) {
        hide |= HideSendAdd;
      }
      if (fxRows > std::max(2, om.fxMost) && shortOf()) {
        --fxRows;
      }
      if (shortOf()) {
        hide |= HideSetting;
      }
    }
    if (const float excess = need(hide) - (sh - 6); excess > 0) {
      if (faderT - excess >= std::max(kMixFaderMin, std::round(sh * 0.26f))) {
        faderT -= excess;
      } else if (sh >= 560) {
        faderT = std::max(std::max(kMixFaderMin, std::round(sh * 0.20f)), faderT - excess);
      }
    }
    while (sendRows > 1 && need(hide) > sh - 6) --sendRows;  // first fewer send rows (one at least)
    while (fxRows > 4 && need(hide) > sh - 6) --fxRows;      // (the rest of the effects behind a "+N" row)
    // K-112D (D3): the track icon row (134 keeps it) goes after the setting, group and automation and EQ rows
    for (int h : {HideSetting, HideGroupAuto, HideEq})
      if (need(hide) > sh - 6) {
        hide |= h;
      }
    while (fxRows > 3 && need(hide) > sh - 6) --fxRows;
    // D5 (R11-08): then the rows close up before the fader gives way (the 900 px window's lower pane, the
    // view a laptop shows first, kept its icon row): the add-a-send slot folds into the first empty send
    // row, the icon and the pan knob take their compact sizes.
    if (sendRows > 0 && need(hide) > sh - 6) {
      hide |= HideSendAdd;
    }
    for (int h : {IconSmall, PanSmall, IconInline}) {
      if (need(hide) > sh - 6) {
        hide |= h;
      }
    }
    if (need(hide) > sh - 6) {  // the fader gives way before the icon and sends rows
      faderT = std::max(kMixFaderMin, faderT - (need(hide) - (sh - 6)));
    }
    for (int h : {HideIcon, HideSends})
      if (need(hide) > sh - 6) {
        hide |= h;
      }
    // Checkpoint 22: then fewer effect rows (two at least: one effect or the count, and the free slot),
    // then the level readouts, the pan knob and a shorter fader, for the smallest panes.
    while (fxRows > 2 && need(hide) > sh - 6) --fxRows;
    // Checkpoint 28: then one effect row (the effect, the "+N" count or the free slot), so the fader keeps
    // its height at 150 % as 134 and 043 keep theirs (it fell to 48 px there).
    if (fxRows > 1 && need(hide) > sh - 6) {
      --fxRows;
    }
    for (int h : {HideMid, HideLevel, HidePan, HideShort})
      if (need(hide) > sh - 6) {
        hide |= h;
      }
  }
  float rows[RowCount] = {};
  const int sendRowsWas = gSendRows;
  gSendRows = sendRows;
  ImGui::SetCursorScreenPos(ImVec2(o.x + legendW, o.y));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
  ImGui::BeginChild("##lgmixstrips", ImVec2(av.x - legendW, av.y), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleColor();
  {
    const ImVec2 b = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = b.x;
    // D5 (CPU): only the strips in view are drawn (the pane scrolls sideways; the rest only take their room)
    const float vis0 = ImGui::GetWindowPos().x, vis1 = vis0 + ImGui::GetWindowWidth();
    bool rowsTaken = false;
    mixhideui::beginColumns();
    for (size_t oi = 0; oi < order.size(); ++oi) {
      const size_t i = order[oi];
      const bool slimCol = oi < om.entries.size() && om.entries[oi].show != mixhide::Show::Strip;
      const float colW = slimCol ? kSlimColW : stripW;
      mixhideui::noteColumn(i, x, x + colW);  // (dev: "mixhide state")
      if (x + colW < vis0 || x > vis1) {
        x += colW;
        continue;
      }
      if (slimCol) {  // GitHub #58
        smoothSlimStrip(a, om.entries[oi], ImVec2(x, b.y), colW, sh);
        x += colW;
        continue;
      }
      const MixerTrack& m = s.mixer[i];

      const Channel* ch = nullptr;
      if (i != 0)
        for (const Channel& c : s.channels)
          if (c.mixerTrack == m.id && !ch) ch = &c;
      const bool on = a.selMixer == m.id;
      // K-112b (close-range pass): square columns 1 px apart, as 134, 043 and 133 draw their strips
      // (they were cards with 4 px corners 2 px apart).
      dl->AddRectFilled(ImVec2(x, b.y), ImVec2(x + stripW - 1, b.y + sh), on ? theme::mix(P.surface800, P.text, 0.07f) : P.surface800);  // K-112D: the selected strip a step lighter (134), no orange cast
      ImGui::SetCursorScreenPos(ImVec2(x, b.y));
      ImGui::PushID(static_cast<int>(m.id));
      if (ImGui::InvisibleButton("##sel", ImVec2(stripW - 2, 4))) a.selMixer = m.id;
      ImGui::PopID();
      float ry[RowCount] = {};
      // D6 (R12-09): names fit the strip's real width (a wide strip shows them whole). The width depends
      // only on the window and the strips shown, so a name still reads the same frame to frame.
      gFitExcess = 0;
      // D8: the slots 4 px in from every column edge (they were 4 left, 5 right and 6 at the top)
      strip(a, m, ch, ImVec2(x + 4, b.y + 4), ImVec2(stripW - 3, sh - 6), fxRows, ry, hide);
      gFitExcess = 0;
      if (!rowsTaken || rows[RowName] == 0)  // the legend's rows: the first strip drawn
        for (int k = 0; k < RowCount; ++k) rows[k] = ry[k];
      rowsTaken = true;
      if (ImGui::IsMouseHoveringRect(ImVec2(x, b.y), ImVec2(x + stripW - 2, b.y + sh)) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
          !ImGui::IsAnyItemHovered())
        a.selMixer = m.id;
      x += stripW;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, b.y + sh));
    ImGui::Dummy(ImVec2(1, 1));
    // D9 (R14-12): the scroll comes to rest on a strip's edge, so the view never ends mid-strip: once a
    // wheel or trackpad scroll has stopped (a few frames still), it moves on to the strip edge in the
    // direction it went; a scroll bar drag settles the same way when let go.
    static float lastX = 0;
    static int lastMove = 0, dir = 0;
    const float sx = ImGui::GetScrollX();
    const int frame = ImGui::GetFrameCount();
    if (std::fabs(sx - lastX) > 0.5f) {
      dir = sx > lastX ? 1 : -1;
      lastMove = frame;
      lastX = sx;
    } else if (scrolls && dir != 0 && frame - lastMove >= 6 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const float steps = sx / stripW;
      const float snap = std::min((dir > 0 ? std::ceil(steps - 0.02f) : std::floor(steps + 0.02f)) * stripW, ImGui::GetScrollMaxX());
      dir = 0;
      if (std::fabs(snap - sx) > 0.5f) {
        ImGui::SetScrollX(snap);
      }
    }
  }
  ImGui::EndChild();
  gSendRows = sendRowsWas;
  // The legend: the rows' names, right-aligned beside the first strip's rows.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + legendW - 4, o.y + sh), P.surface900);
  // K-112D: the reference's own row names (134): Input for the instrument, Audio FX, dB for the readouts.
  const char* names[RowCount] = {i18n::N_("Setting"),    i18n::N_("EQ"),    i18n::N_("Input"), i18n::N_("Audio FX"), i18n::N_("Sends"),
                                 i18n::N_("Output"),     i18n::N_("Group"), i18n::N_("Automation"), i18n::N_("Pan"),  i18n::N_("dB"),
                                 nullptr,                i18n::N_("Mute / Solo"), i18n::N_("Name")};
  const float fs = theme::kFontLabel;
  // K-112b (second pass): each name centred on its row's own height (the EQ thumbnail, the pan knob
  // and the plates are taller than a slot), not hung from the row's top.
  const float rowH[RowCount] = {17, 24, 17, 17, 17, 17, 17, 17, 22, 16, 0, 18, 18};
  for (int k = 0; k < RowCount; ++k) {
    if (!names[k] || rows[k] <= 0) continue;
    const char* l = i18n::tr(names[k]);
    const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, l).x;
    dl->AddText(a.fontRegular, fs, ImVec2(std::round(o.x + legendW - 10 - tw), std::round(rows[k] + (rowH[k] - fs) * 0.5f) - 1), P.textDim, l);
  }
  if (rows[RowFader] > 0) {  // the fader scale: dB marks down the legend's right edge
    const float fh = rows[RowMuteSolo] - 4 - rows[RowFader];  // the strips' fader height (their fader row ends 4 px above M and S)
    // R09-03: the strips' own law and cap geometry (ui::FaderCapY), so each mark meets the caps at that
    // level. D4 (R10-04): the very marks the strips show (faderMarks), one shared set.
    const auto d = describeParam(s, mixerParam(s.mixer[0].id, MixerParam::Volume));
    int mdb[12];
    float mys[12];
    const int mn = d ? faderMarks(*d, ImVec2(0, rows[RowFader]), ImVec2(20, fh), kMarkGap, mdb, mys, 12) : 0;
    for (int mk = 0; mk < mn; ++mk) {
      const int db = mdb[mk];
      const float yy = mys[mk];
      char t[8];
      std::snprintf(t, sizeof t, db > 0 ? "+%d" : "%d", db);
      ImFont* const lf = logicplug::active(a) ? a.fontRegular : a.fontMono;  // K-112D (audit X3)
      const float tw = lf->CalcTextSizeA(fs - 1, 1e9f, 0, t).x;
      dl->AddText(lf, fs - 1, ImVec2(std::round(o.x + legendW - 14 - tw), yy - (fs - 1) * 0.5f), P.textFaint, t);
      dl->AddLine(ImVec2(o.x + legendW - 11, yy), ImVec2(o.x + legendW - 6, yy), P.textFaint);
    }
  }
}

// (a pixel above the row's middle: the capitals then sit on the toolbar's, 102 to 110 at 1920 x 1080)
void openRegionCard() { gRegionOpen = true; }

float paneTitleY(float fs) { return std::round(ImGui::GetWindowPos().y + kPaneHeadTop + (kPaneHeadH - fs) * 0.5f) - 1.0f; }

void draw(App& a) {
  smoothlib::frame(a);  // K-112D: the Library pane as the project saved it
  quickHelpFloat(a);  // R01-08
  // Where the layout homes the instrument or effect window in the device chain (Host), opening one
  // (from the rack, the mixer, a menu) opens the chain and brings it to the front; closing the
  // chain afterwards is respected until another one is opened.
  static ObjectId lastSettings = kNoId, lastFx = kNoId;
  static int lastSlot = -1;
  const layout::Preset p = layoutui::active();
  if (layoutui::shellBuilt(a, p)) {
    const bool newSettings = a.settingsChannel != kNoId && a.settingsChannel != lastSettings && !trackereditors::pluginWindowOpen(a) &&
                             (layout::windowHome("###channelsettings", p).kind == layout::HomeKind::Host || bandHostsChannel(a, a.settingsChannel, p));
    const bool newFx = a.fxEditor.first != kNoId && (a.fxEditor.first != lastFx || a.fxEditor.second != lastSlot) &&
                       layout::windowHome("###fxeditor", p).kind == layout::HomeKind::Host;
    if (newSettings || newFx)
      if (bool* f = flag(a, "Device chain")) {
        *f = true;
        a.focusWindow = "Device chain";
      }
  }
  // 0.7.8: in Smooth a Seltmann channel's settings open on its lower-pane tab
  if (p == layout::Preset::Logic && a.settingsChannel != kNoId && a.settingsChannel != lastSettings)
    if (const Channel* c = a.doc.song().channel(a.settingsChannel); c && c->plugin.type == "gl.seltmann")
      if (const layout::Item* it = layout::findPlugin(c->plugin.type); it && it->home[static_cast<int>(p)].kind == layout::HomeKind::Host)
        if (flag(a, "Seltmann")) {
          // (as a click on its tab: the audio editor holding the pane, an Audio clip there, gives it back)
          layoutui::showPaneWindow(a, "Seltmann");
          a.focusWindow = "Seltmann";
        }
  lastSettings = a.settingsChannel, lastFx = a.fxEditor.first, lastSlot = a.fxEditor.second;
  int i = 0;
  for (const PartDef& p : kParts) {
    prof::Scope ps(p.window);  // K-142: each part's cost per frame (--profile-frames)
    part(a, p.window, i++, p.size, [&] { p.draw(a); });
  }
}

bool infoInBar(App& a) {
  const layout::Preset p = layoutui::active();
  const bool* f = flag(a, "Info box");
  return f && *f && layoutui::shellBuilt(a, p) && layout::windowHome("Info box", p).kind == layout::HomeKind::Bar;
}

void devCommand(App& a, const std::vector<std::string>& args) {
  if (!args.empty() && args[0] == "lib") {  // K-112D: parts lib nav <n> | rows | load <row> | state
    smoothlib::devCommand(a, std::vector<std::string>(args.begin() + 1, args.end()));
    return;
  }
  if (!args.empty() && args[0] == "smart") {  // D5: parts smart focus|learn|cc|touch|state ...
    logicplug::smartDevCommand(a, std::vector<std::string>(args.begin() + 1, args.end()));
    return;
  }
  // parts chain instrument | fx <slot> | master: a Device chain chip click
  if (args.size() >= 2 && args[0] == "chain") {
    if (args[1] == "instrument") gChainDev = {0, -1};
    else if (args[1] == "fx" && args.size() > 2) gChainDev = {1, std::atoi(args[2].c_str())};
    else if (args[1] == "master") gChainDev = {2, -1};
    return;
  }
  // parts notes set <file> | dump <file> | focus | blur
  if (args.size() < 2 || args[0] != "notes") return;
  std::string path;  // the rest of the line (paths may have spaces)
  for (size_t i = 2; i < args.size(); ++i) path += (i > 2 ? " " : "") + args[i];
  if (args[1] == "focus") gNotesDev.focus = true;
  else if (args[1] == "blur") gNotesDev.blur = true;
  else if (args[1] == "set" && !path.empty()) {
    std::ifstream f(path, std::ios::binary);
    SongInfo info = a.doc.song().info;
    info.comments.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    a.doc.setInfo(info);
  } else if (args[1] == "dump" && !path.empty()) {
    writeFileAtomic(path, a.doc.song().info.comments);
  }
}

void panelsMenu(App& a, bool tracker) {
  static const char* const trackerOwn[] = {"Pattern editor", "Order list", "Track scopes", "Info box", "Files"};
  // Checkpoint 59 (N58-07): Clips' own browser, Clip View and Info View do what these Studio panels do;
  // there they would dock a second browser (or inspector) over Clips' own, so Clips does not list them.
  static const char* const clipsHas[] = {"Inspector", "Library", "Lists", "Clip panel", "Files"};
  const bool clips = livelook::active(a);
  for (const PartDef& p : kParts)
    if (tracker && std::any_of(std::begin(trackerOwn), std::end(trackerOwn), [&](const char* w) { return std::strcmp(w, p.window) == 0; })) continue;
    else if (clips && std::any_of(std::begin(clipsHas), std::end(clipsHas), [&](const char* w) { return std::strcmp(w, p.window) == 0; })) continue;
    else if (bool* f = flag(a, p.window); f && partListed(p.window)) ImGui::MenuItem(i18n::tr(p.window), nullptr, f);
}

}  // namespace gl::layoutparts

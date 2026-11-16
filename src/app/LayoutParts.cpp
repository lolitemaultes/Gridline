// Gridline - the layout parts (K-112 R4). See LayoutParts.h.
#include "LayoutParts.h"
#include <algorithm>
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
#include "../core/Automation.h"
#include "../core/Params.h"
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "../core/Project.h"
#include "../core/TrackModes.h"
#include "../engine/Dsp.h"
#include "../engine/EqDesign.h"
#include "../packs/PacksUi.h"
#include "App.h"
#include "BrowserUi.h"
#include "ContextMenus.h"
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
#include "SmoothLibrary.h"
#include "Theme.h"
#include "TrackIcons.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"

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
    const float padPart = std::strcmp(window, "Pattern editor") == 0 || std::strcmp(window, "Order list") == 0 ? 2.0f : theme::kS2;
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
  if (gCard.rows++ > 0) dl->AddLine(ImVec2(p.x, std::round(p.y - ImGui::GetStyle().ItemSpacing.y * 0.5f)), ImVec2(p.x + w - 8, std::round(p.y - ImGui::GetStyle().ItemSpacing.y * 0.5f)),
                                    theme::withAlpha(pal().line, 0.7f));
}

// A card row: the label dim on the left, the value on the right. A clickable row returns its click.
bool cardRow(const char* id, const char* label, const std::string& value, const char* hint, bool clickable = false) {
  App& a = app();
  const auto& P = pal();
  const float rowH = std::max(18.0f, layoutui::style().inspectorRowH);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
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
  std::snprintf(b, sizeof b, "%.1f", 20.0 * std::log10(static_cast<double>(gain)));
  return b;
}

// A card's foldable heading: a disclosure arrow and the bold title; a click folds or unfolds it.
bool cardFold(const char* id, const std::string& text, bool* open) {
  App& a = app();
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // Checkpoint 37: in the Tracks layout a heading is a 34 px row with its line on the row's foot and the
  // next row straight under it, as 216 and 125 stack Region and Track (22 px with gaps around: a 44 px pitch).
  const bool lg = logicplug::active(a);
  const float w = ImGui::GetContentRegionAvail().x, h = lg ? 34.0f : 22.0f;
  if (ImGui::InvisibleButton(id, ImVec2(w, h))) *open = !*open;
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cy = std::round(p.y + h * 0.5f);
  if (lg) {  // K-112b: as the reference inspectors (110, 092)
    const float fs = ImGui::GetFontSize(), ty = std::round(p.y + (h - fs) * 0.5f);
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
  const float w = ImGui::GetContentRegionAvail().x;
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
  int busNo = 0;  // R07-11: an unnamed bus reads "Bus N", as the reference numbers them
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
    std::string label = mixerTrackName(s, k);
    if (k > 0 && m.name.empty()) label = std::string(i18n::tr("Bus")) + " " + std::to_string(++busNo);
    // R08-09: a bus made by New bus is listed by its bus number ("Bus 1"), its strip named "Aux 1", as the reference's.
    else if (k > 0 && m.name.rfind("Aux ", 0) == 0 && std::atoi(m.name.c_str() + 4) > 0) label = std::string(i18n::tr("Bus")) + " " + m.name.substr(4);
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
      } else {
        a.doc.setSend(t.id, m.id, 0.5f);
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
        } else {
          a.doc.setSend(from, bus, 0.5f);
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
const char* fitSlotName(ImFont* f, float fs, const char* name, float w, char* buf, size_t n) {
  if (f->CalcTextSizeA(fs, 1e9f, 0, name).x <= w) return name;
  static const std::pair<const char*, const char*> kShort[] = {
      {"Parametric", "Param"}, {"Compressor", "Comp"}, {"Sidechain", "SC"},      {"Stereo", "St."},     {"Distortion", "Dist"},
      {"Expander", "Exp"},     {"Equalizer", "EQ"},    {"Saturation", "Sat"},    {"Saturator", "Sat"},  {"Multiband", "MB"},
      {"Transient", "Trans"},  {"Analog", "An."},      {"Reverb", "Verb"},       {"Delay", "Dly"},      {"Chorus", "Chor"},
      {"Imager", "Img"},       {"Filter", "Filt"},     {"Maximizer", "Max"},     {"Limiter", "Lim"},    {"Enhancer", "Enh"}};
  size_t o = 0;
  const char* c = name;
  while (*c && o + 1 < n) {
    const char* e = c;
    while (*e && *e != ' ') ++e;
    const size_t len = static_cast<size_t>(e - c);
    const char* rep = nullptr;
    for (const auto& [from, to] : kShort)
      if (std::strlen(from) == len && std::strncmp(c, from, len) == 0) rep = to;
    if (rep)
      for (const char* r = rep; *r && o + 1 < n; ++r) buf[o++] = *r;
    else
      for (const char* r = c; r < e && o + 1 < n; ++r) buf[o++] = *r;
    while (*e == ' ' && o + 1 < n) buf[o++] = *e++;
    c = e;
  }
  buf[o] = 0;
  return buf;
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
                              : empty ? (hov ? P.surface700 : theme::withAlpha(P.surface700, 0.4f))
                              : hov ? (layoutui::active() == layout::Preset::Logic ? theme::mix(P.surface700, P.text, 0.08f) : P.surface600)  // K-112b: the quiet hover
                                    : P.surface700;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), face, 4.0f);
  if (lit) dl->AddRect(p, ImVec2(p.x + w, p.y + h), P.hot, 4.0f);
  const float fs = theme::kFontLabel;
  char shortBuf[64];
  const char* shown = logicplug::active(a) && coloured ? fitSlotName(a.fontRegular, fs, name.c_str(), w - 10, shortBuf, sizeof shortBuf) : name.c_str();
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(centredX(a.fontRegular, fs, shown, p.x + 5, w - 10), std::round(p.y + (h - fs) * 0.5f) - 1), w - 10,
                      coloured ? pal().paper : empty ? P.textFaint : P.text, shown);
  if (hint && hov) ui::Hint(hint);
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
  float tx = p.x + 5, tw = w - 10;
  if (hov) {
    const ImU32 on = pal().paper, dim = theme::withAlpha(pal().paper, 170.0f / 255.0f);
    ui::drawIcon(dl, ui::Icon::Power, ImVec2(p.x + 8, p.y + h * 0.5f), 10, e.enabled ? (inBypass ? on : dim) : (inBypass ? P.text : P.textFaint));
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p.x + w - 7, p.y + h * 0.5f), 9, e.enabled ? (inMenu ? on : dim) : (inMenu ? P.text : P.textDim));
    tx = p.x + 16, tw = w - 30;
  }
  char shortBuf[64];  // K-112D (audit F9)
  const char* shown = logicplug::active(a) ? fitSlotName(a.fontRegular, fs, name.c_str(), tw, shortBuf, sizeof shortBuf) : name.c_str();
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(centredX(a.fontRegular, fs, shown, tx, tw), std::round(p.y + (h - fs) * 0.5f) - 1), tw,
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
  const bool hov = ImGui::IsItemHovered();
  slotDrop(a, t.id, k);
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(w, 200.0f), 0), ImVec2(320, 460));
  if (ImGui::BeginPopup("##fxpick")) {
    effectPickerList(a, t.id, k, "");
    ImGui::EndPopup();
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? P.surface700 : theme::withAlpha(P.surface700, 0.4f), 4.0f);
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
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? theme::mix(P.field, P.surface700, 0.5f) : P.field, 3.0f);
  const float mid = std::round(p.y + h * 0.5f);
  dl->AddLine(ImVec2(p.x + 2, mid), ImVec2(p.x + w - 2, mid), theme::withAlpha(P.line, 0.8f));
  if (eq >= 0) {
    const EffectSlot& e = t.slots[static_cast<size_t>(eq)];
    const double sr = 48000.0;
    EqBandFilters f[pp::kEqBands];
    for (int b = 0; b < pp::kEqBands; ++b) designEqBand(e.plugin, b, sr, f[b]);
    const int n = std::max(8, static_cast<int>(w / 3));
    std::vector<ImVec2> pts(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / (n - 1));
      double db = 0;
      for (int b = 0; b < pp::kEqBands; ++b)
        if (f[b].count > 0) db += dsp::gainToDb(eqBandMagnitude(f[b], hz, sr));
      const float y = mid - static_cast<float>(std::clamp(db, -18.0, 18.0) / 18.0) * (h * 0.5f - 2);
      pts[static_cast<size_t>(i)] = ImVec2(p.x + 2 + (w - 4) * static_cast<float>(i) / static_cast<float>(n - 1), y);
    }
    dl->AddPolyline(pts.data(), n, e.enabled ? theme::rgb(theme::role::kGraph) : P.textFaint, 0, 1.5f);
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
enum StripRow { RowSetting, RowEq, RowInst, RowFx, RowSends, RowOut, RowGroup, RowAuto, RowPan, RowLevel, RowFader, RowMuteSolo, RowName, RowCount };
// K-112b (checkpoint 20): rows the Tracks layout's mixer leaves out when its pane is short, so the
// strips fit without scrolling (dropped in this order: the icon, the settings slot, the EQ thumbnail,
// the sends slot).
// Checkpoint 22: on the smallest panes (150 % at 1440x900) the level readouts, then the pan knob go
// too, and the fader may shorten to 32 px; the effect rows are capped first (see logicMixer).
int gSendRows = 0;  // checkpoint 38: send rows each inspector strip of a pair keeps (so both line up)
enum StripHide { HideIcon = 1, HideSetting = 2, HideEq = 4, HideSends = 8, HideLevel = 16, HidePan = 32, HideShort = 64, HideMid = 128, HideGroupAuto = 256 };  // HideMid: the fader down to 48 px (checkpoint 26)

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

// K-112D: the strip's automation slot: the mode of the track whose strip it is (Off, Read, Touch, Latch,
// Write in the reference's colours: Read green). A click picks the mode for every track on this strip.
void autoSlot(App& a, const MixerTrack& t, float w) {
  static const char* const kNames[5] = {i18n::N_("Off"), i18n::N_("Read"), i18n::N_("Touch"), i18n::N_("Latch"), i18n::N_("Write")};
  static const uint32_t kCols[5] = {0x8A8A8A, 0x5CC26A, 0xE0B33A, 0xE08A3A, 0xA77BE0};
  const Song& s = a.doc.song();
  std::vector<int> tracks;
  for (int tr = 0; tr < static_cast<int>(s.tracks.size()); ++tr)
    if (const MixerTrack* st = logicplug::trackStrip(s, tr); st && st->id == t.id) tracks.push_back(tr);
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
    const std::string name = info ? info->name : (ch->isLayer() ? i18n::tr("Layer") : i18n::tr("No instrument"));
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
  const int fxMax = fxRows >= 0 ? (lone ? 1 : fxRows - 1) : 6;
  // K-112b (checkpoint 22): when the mixer caps its effect rows, the last row says how many more there
  // are (a click shows them in the detailed mixer) instead of leaving them out unseen.
  const bool more = fxRows >= 0 && fxCount > fxMax;
  for (int k = 0; k < kEffectSlots && shown < (more ? fxMax - 1 : fxMax); ++k) {
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
  } else if (!lone) {
    emptyPill(a, t, firstFree(t), w);
  }
  if (!lone) gap();
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
        const std::string nm = (sd.sidechain ? std::string("SC ") : std::string()) + mixerTrackName(s, static_cast<size_t>(di));
        if (stripSlot("##send", nm, false, false, w - kw, "A send: click to show it in the mixer; the knob sets its level", theme::rgb(kSlotBlue))) {  // blue as the effects (216)
          a.selMixer = t.id;
          a.showMixer = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(sp.x + w - kw + 3, sp.y + 1.5f));
        ui::ParamKnob("##sendk", sendParam(t.id, di), 7.0f);
        ImGui::SetCursorScreenPos(ImVec2(sp.x, sp.y + 17));
        ImGui::Dummy(ImVec2(w, 0));
        ImGui::PopID();
        gap();
        ++shownSends;
      }
      for (int k = shownSends; k < gSendRows; ++k) {
        ImGui::Dummy(ImVec2(w, 17));
        gap();
      }
      if (stripSlot("##sends", i18n::tr("Sends"), false, true, w, "The track's sends: click to send to a bus") || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##sendsmenu");
      routeMenu(a, "##sendsmenu", t, false);
      gap();
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
    const std::string outName = index == 0 ? std::string(i18n::tr("Device")) : out ? mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(out->id)))) : std::string(i18n::tr("Master"));
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
            if (!n.empty()) mixops::createGroup(a.doc, {id}, n, true);  // K-238: a new group starts linked
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
  const float knobR = inspStrip ? std::clamp(std::round(size.x * 0.17f), 11.0f, 16.0f) : mixStrip && logicplug::active(a) && !(hide & HideMid) ? 15.0f : 11.0f, plateH = 18, msH = inspStrip ? 20.0f : 18.0f, dbH = 16;
  const float cx = std::round(p.x + size.x * 0.5f);
  float y = p.y + topH;
  if (fxRows >= 0 && !(hide & HideIcon)) {  // the mixer: the track's icon above its pan knob
    const int ic = t.icon > 0 ? t.icon : ch && ch->icon > 0 ? ch->icon : trackicons::guess(t.name, ch ? "Synth" : "Bus");
    // In the track's colour (dimmed toward the text colour so a dark colour still reads).
    if (ic > 0) trackicons::draw(ImGui::GetWindowDrawList(), ic, ImVec2(cx, std::round(y + 13)), 24, theme::mix(theme::content(t.color), pal().text, 0.25f));
    y += 30;
  }
  if (!(hide & HidePan)) {
    if (rowY) rowY[RowPan] = y;
    ImGui::SetCursorScreenPos(ImVec2(cx - knobR, y));
    if (logicplug::active(a)) gPanFont = a.fontRegular, ui::SetNextKnobFace(smoothPanFace);  // K-112D
    ui::ParamKnob("##pan", mixerParam(t.id, MixerParam::Pan), knobR, nullptr, true);
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
    const float bw = std::floor((size.x - 12) * 0.5f), bh = dbH;
    const ImVec2 b0(p.x + 2, y), b1(p.x + 2 + bw + 4, y);
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
  float tickN[8];
  int tickCount = 0;
  if (scaleCol)
    if (const auto d = describeParam(s, volPid))
      for (int db : {6, 0, -6, -12, -18, -24, -36, -48}) {
        const double lin = std::pow(10.0, db / 20.0);
        if (lin <= d->max + 1e-9) tickN[tickCount++] = static_cast<float>(d->toNormalized(d->clamp(lin)));
      }
  ImGui::SetCursorScreenPos(ImVec2(x0, y));
  ui::setFaderTicks(tickN, tickCount);
  bool readLocked = false;  // R05-04: a track in Read with a curve on this fader: the curve moves it, a drag can't
  if (logicplug::active(a) && a.engine.playing()) {
    bool curve = false;  // a volume curve on this strip
    for (const AutomationLane& l : automationLanes(s)) curve = curve || l.targets(volPid.owner, volPid.index);
    for (int tr = 0; curve && tr < static_cast<int>(s.tracks.size()) && !readLocked; ++tr) {
      const PlaylistTrack& pt = s.tracks[static_cast<size_t>(tr)];
      if (pt.autoMode != 1) continue;
      if (pt.laneSet) readLocked = pt.laneOwner == volPid.owner && pt.laneIndex == volPid.index;
      else if (const MixerTrack* st = logicplug::trackStrip(s, tr)) readLocked = st->id == volPid.owner;
    }
  }
  ui::setFaderLocked(readLocked);
  ui::setFaderDb(logicplug::active(a) ? 0.8 : -1.0);  // R05-15: the Tracks layout's strips read in dB
  ui::ParamFader("##f", volPid, ImVec2(faderW, fh));
  ui::setFaderDb(-1.0);
  ui::setFaderLocked(false);
  ui::setFaderTicks(nullptr, 0);
  const float meterX = x0 + faderW + fg + (scaleCol ? scaleW + fg : 0.0f);
  if (scaleCol) {
    if (const auto d = describeParam(s, volPid)) {
      const look::geom::ClassicFader g = look::geom::classicFader(x0, y, faderW, fh, 0.0f);
      const float sfs = theme::kFontLabel - 2, sx1 = x0 + faderW + fg + scaleW;
      float lastY = -1e9f;  // a mark too close to the one above is left out, so labels never touch
      for (int db : {6, 0, -6, -12, -18, -24, -36, -48}) {
        const double lin = std::pow(10.0, db / 20.0);
        if (lin > d->max + 1e-9) continue;
        const float yy = g.tickY(static_cast<float>(d->toNormalized(d->clamp(lin))));
        if (yy - lastY < sfs + 2) continue;
        lastY = yy;
        char tx[8];
        std::snprintf(tx, sizeof tx, db > 0 ? "+%d" : "%d", db > 0 ? db : -db);  // below unity unsigned, as the references
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
    const float bw = inspStrip ? 28.0f : mixStrip ? std::floor((size.x - 5 - 4) * 0.5f) : 22.0f, bh = msH;
    auto btn = [&](const char* bid, const char* label, bool on, ImU32 lit, float bx) {
      ImGui::SetCursorScreenPos(ImVec2(bx, y));
      const bool c = ImGui::InvisibleButton(bid, ImVec2(bw, bh));
      const bool hv = ImGui::IsItemHovered();
      // K-112b (checkpoint 22): one quiet hover for the Tracks layout's own controls, the rest face 8 % toward the text (14 % open or held)
      const ImU32 restB = P.surface700, hovB = logicplug::active(a) ? theme::mix(restB, P.text, 0.08f) : P.surface600;
      dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + bh), on ? lit : hv ? hovB : restB, 3.0f);
      const ImVec2 ts = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, label);
      dl->AddText(a.fontBold, fs, ImVec2(std::round(bx + (bw - ts.x) * 0.5f), std::round(y + (bh - ts.y) * 0.5f)), on ? P.ink : P.textDim, label);
      return c;
    };
    const bool twoBtn = index > 0;
    const float bx = mixStrip ? p.x : twoBtn ? std::round(cx - bw - 2) : std::round(cx - bw * 0.5f);
    if (btn("##m", "M", t.muted, P.hot, bx)) a.doc.setParam(mixerParam(t.id, MixerParam::Mute), t.muted ? 0 : 1, false);
    if (ImGui::IsItemHovered()) ui::Hint(t.muted ? "Track is muted: click to unmute" : "Track is on: click to mute");
    if (twoBtn) {
      if (btn("##s", "S", t.solo, logicplug::active(a) ? theme::rgb(0xE8C83A) : theme::rgb(theme::role::kStamp), mixStrip ? p.x + size.x - 5 - bw : bx + bw + 4))  // mixer: 4 px from the column's edge
        mixops::toggleSolo(a.doc, t.id);  // K-238: a linked group solos together
      if (ImGui::IsItemHovered()) ui::Hint("Solo (keeps tracks routed to and from it)");
    }
    y += bh + 4;
  }
  const std::string name = index == 0 ? std::string(i18n::tr("Master")) : mixerTrackName(s, index);
  if (rowY) rowY[RowName] = y;
  // K-112b (close-range pass): in the mixer the name bar spans the strip's column edge to edge and
  // runs down to its foot, square, as 043, 134 and 133 draw it (it was an inset plate with corners).
  // The column is the strip's area plus the mixer's 4 px margin left, 2 px right and 2 px below.
  const ImVec2 n0(mixStrip ? p.x - 4 : p.x + 1, y), n1(mixStrip ? p.x + size.x - 1 : p.x + size.x - 5, y + plateH + (mixStrip ? 2.0f : 0.0f));
  const ImU32 plate = theme::mix(P.surface700, theme::content(t.color), 0.85f);  // the track colour, near full (as every reference mixer)
  if (!inspStrip) dl->AddRectFilled(n0, n1, plate, mixStrip ? 0.0f : 3.0f);
  const float nw = std::min(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, name.c_str()).x, n1.x - n0.x - 8);
  const float nameY = mixStrip ? std::round(n0.y + (n1.y - n0.y - fs) * 0.5f) - 1 : std::round(n0.y + (plateH - fs) * 0.5f) - 1;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(std::round((n0.x + n1.x - nw) * 0.5f), nameY), n1.x - n0.x - 8,
                      inspStrip ? P.text : theme::contrastText(plate), name.c_str());
  ImGui::PopID();
}

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
  static bool regionOpen = false, trackOpen = false;  // both cards start folded: the strips below take the column
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
        const bool to = !clip->loop;
        editSel("loop clip", [to](Clip& k) { k.loop = to; });
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
      if (clip->kind == ClipKind::Audio) {
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
    if (!linked && !ch && logicplug::active(a)) ch = logicplug::trackSound(s, track, clip);  // R01-10: a free track's regions' sound
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
      }
      const ImVec2 base = ImGui::GetCursorScreenPos();
      const float y = base.y + 6;
      const float sw = std::floor(av.x * 0.5f);
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
      if (dest) strip(a, *dest, nullptr, ImVec2(base.x + sw + 3, y), ImVec2(sw, sh), -1, nullptr, 0, pad);
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
    // A device asked for in a window (its title bar or menu) leaves its body to that window.
    if (hostsIns && !band.insWindow) hostbody::claim(hostbody::Body::ChannelSettings, "Device chain");
    if (hostsFx && !band.fxWindow) hostbody::claim(hostbody::Body::EffectEditor, "Device chain");
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
};
// K-112b: built for the Tracks layout so far; the other layouts' menus do not list it yet.
bool partListed(const char* window) {
  // N45-09: the Tracker's panels are the Tracker's; Clips does not list them.
  const bool tracker = std::strcmp(window, "Pattern editor") == 0 || std::strcmp(window, "Order list") == 0;
  if (tracker && layoutui::active() == layout::Preset::Live) return false;
  return std::strcmp(window, "Smart controls") != 0 || layoutui::active() == layout::Preset::Logic;
}

}  // namespace

// K-112b: the Logic layout's mixer: a legend column, then one strip per mixer track (the inspector's
// strips, lined up), a view filter above. The detailed mixer (slots, routing, stage) is one click away.
void logicMixer(App& a) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  if (s.mixer.empty()) return;
  static int view = 1;  // 0 the selected track and where it goes, 1 the tracks in use, 2 every track
  // K-112D (audit F2): the strip kinds shown (the reference's type buttons, 134): bit 0 instrument
  // strips, 1 audio strips, 2 buses (what other strips send into), 3 the master. All on at first.
  static int kindsOn = 15;
  {  // the local bar: Edit, Options and View on the left, the view in the middle, the kinds on the right (134)
    const float y0 = ImGui::GetCursorPosY();
    if (ui::TextButton("Edit", false, 0, "Undo and redo the mixer's changes")) ImGui::OpenPopup("##mixedit");
    ImGui::SameLine(0, 2);
    if (ui::TextButton("Options", false, 0, "Groups and the selected strip")) ImGui::OpenPopup("##mixopts");
    ImGui::SameLine(0, 2);
    if (ui::TextButton("View", false, 0, "Which strips show, and the detailed mixer")) ImGui::OpenPopup("##mixviewm");
    if (ImGui::BeginPopup("##mixedit")) {
      if (ImGui::MenuItem(i18n::tr("Undo"))) a.doc.undo();
      if (ImGui::MenuItem(i18n::tr("Redo"))) a.doc.redo();
      ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##mixopts")) {
      const MixerTrack* selT = s.mixerTrack(a.selMixer);
      const bool canGroup = selT && s.mixerIndex(selT->id) > 0;
      if (ImGui::MenuItem(i18n::tr("Create group from the selected strip..."), nullptr, false, canGroup)) {
        const ObjectId id = selT->id;
        menus::openPrompt("Group name", mixops::newGroupName(s), [&a, id](const std::string& n) {
          if (!n.empty()) mixops::createGroup(a.doc, {id}, n, true);
        });
      }
      if (ImGui::MenuItem(i18n::tr("Remove the selected strip from its group"), nullptr, false, canGroup && !selT->group.empty()))
        mixops::setGroup(a.doc, {selT->id}, "");
      ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##mixviewm")) {
      const char* vn[] = {i18n::N_("Single"), i18n::N_("Tracks"), i18n::N_("All")};
      for (int k = 0; k < 3; ++k)
        if (ImGui::MenuItem(i18n::tr(vn[k]), nullptr, view == k)) view = k;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Detailed mixer"))) a.settings.set("ui.logicMixerDetail", true);
      ImGui::EndPopup();
    }
    const char* views[] = {i18n::tr("Single"), i18n::tr("Tracks"), i18n::tr("All")};  // K-112D: the reference's names (134)
    const char* hints[] = {i18n::tr("The selected track and where it goes"), i18n::tr("The tracks that something plays through, and the master"),
                           i18n::tr("Every mixer track")};
    const float segW = 74;
    ImGui::SameLine();
    const float bx = ImGui::GetWindowContentRegionMin().x;
    ImGui::SetCursorPos(ImVec2(std::max(ImGui::GetCursorPosX() + 8, bx + (ImGui::GetContentRegionMax().x - bx - segW * 3) * 0.5f), y0));
    ui::Segmented("##lgmixview", views, 3, &view, segW, 0, hints);
    // The kinds, as one row of toggles at the right edge.
    const char* kn[] = {i18n::N_("Inst"), i18n::N_("Audio"), i18n::N_("Bus"), i18n::N_("Master")};
    const char* kh[] = {i18n::N_("Instrument strips: show or hide them"), i18n::N_("Audio strips: show or hide them"),
                        i18n::N_("Buses (strips other strips send into): show or hide them"), i18n::N_("The master strip: show or hide it")};
    float kw = 0;
    for (const char* k : kn) kw += ui::TextButtonWidth(i18n::tr(k)) + 2;
    ImGui::SameLine();
    ImGui::SetCursorPos(ImVec2(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetWindowContentRegionMax().x - kw), y0));
    for (int k = 0; k < 4; ++k) {
      if (k) ImGui::SameLine(0, 2);
      if (ui::TextButton(i18n::tr(kn[k]), (kindsOn >> k) & 1, 0, i18n::tr(kh[k]))) kindsOn ^= 1 << k;
    }
  }
  // Which tracks.
  std::vector<size_t> order;
  std::set<ObjectId> used;
  for (const Channel& c : s.channels) used.insert(c.mixerTrack);
  for (const MixerTrack& m : s.mixer)
    for (const Send& sd : m.sends) used.insert(sd.to);
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
    bool fx = false;
    for (const EffectSlot& e : m.slots) fx = fx || !e.plugin.type.empty();
    if (!((kindsOn >> kindOf(m)) & 1) && !(view == 0 && sel && m.id == sel->id)) continue;
    if (view == 2 || (view == 1 && (used.count(m.id) || fx)) || (view == 0 && sel && (m.id == sel->id || (selDest && m.id == selDest->id))))
      order.push_back(i);
  }
  if (kindsOn & 8) order.push_back(0);  // the master at the right end
  if (order.empty()) return;
  int fxRows = 2;
  for (size_t i : order) {
    int n = 0;
    for (const EffectSlot& e : s.mixer[i].slots) n += e.plugin.type.empty() ? 0 : 1;
    fxRows = std::max(fxRows, std::min(n, 6) + 1);
  }
  // The legend, then the strips.
  // K-112b (checkpoint 20): the strips fit the pane, as the references' mixers do at small sizes (they
  // scrolled at 1280x800). Sideways the strips narrow (84 px down to 64) before the pane scrolls;
  // downwards the optional rows go first (StripHide order), the fader keeping at least 48 px.
  const float legendW = 84;
  const ImVec2 av = ImGui::GetContentRegionAvail();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float stripW = std::clamp(std::floor((av.x - legendW) / static_cast<float>(order.size())), 64.0f, 90.0f);  // K-112D (audit F9): room for "Parametric EQ"
  const float sh = std::max(av.y - 14, 120.0f);
  int hide = 0;
  // K-112D: each send its own row, as 134 lists them (the most any strip shown has, up to three), and
  // the empty Sends slot under them.
  int sendRows = 0;
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
    sendRows = std::max(sendRows, std::min(n, 3));
  }
  {
    // The strip's height in rows (see strip()): settings 19, EQ 26, instrument 19, effects 19 each,
    // sends 19 each and the empty one, output 17 and 6 below, group and automation 19 each; icon 30,
    // pan 26, level 22, fader, mute and solo 22, name 20.
    auto need = [&](int h) {
      return 106.0f + 19.0f * static_cast<float>(fxRows) - (h & HideSetting ? 19.0f : 0.0f) - (h & HideEq ? 26.0f : 0.0f) -
             (h & HideSends ? 19.0f : -19.0f * static_cast<float>(sendRows)) + (h & HideGroupAuto ? 0.0f : 38.0f) + (h & HideIcon ? 0.0f : 30.0f) +
             (h & HidePan ? 0.0f : 32.0f) + (h & HideLevel ? 0.0f : 22.0f) + (h & HideShort ? 32.0f : h & HideMid ? 48.0f : kMixFaderMin) + 22 + 20 + 8;
    };
    while (sendRows > 1 && need(hide) > sh - 8) --sendRows;  // first fewer send rows (one at least)
    for (int h : {HideIcon, HideSetting, HideGroupAuto, HideEq, HideSends})
      if (need(hide) > sh - 8) hide |= h;
    // Checkpoint 22: then fewer effect rows (two at least: one effect or the count, and the free slot),
    // then the level readouts, the pan knob and a shorter fader, for the smallest panes.
    while (fxRows > 2 && need(hide) > sh - 8) --fxRows;
    // Checkpoint 28: then one effect row (the effect, the "+N" count or the free slot), so the fader keeps
    // its height at 150 % as 134 and 043 keep theirs (it fell to 48 px there).
    if (fxRows > 1 && need(hide) > sh - 8) --fxRows;
    for (int h : {HideMid, HideLevel, HidePan, HideShort})
      if (need(hide) > sh - 8) hide |= h;
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
    for (size_t i : order) {
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
      strip(a, m, ch, ImVec2(x + 4, b.y + 6), ImVec2(stripW - 4, sh - 8), fxRows, ry, hide);
      if (i == order.front() || rows[RowName] == 0)
        for (int k = 0; k < RowCount; ++k) rows[k] = ry[k];
      if (ImGui::IsMouseHoveringRect(ImVec2(x, b.y), ImVec2(x + stripW - 2, b.y + sh)) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
          !ImGui::IsAnyItemHovered())
        a.selMixer = m.id;
      x += stripW;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, b.y + sh));
    ImGui::Dummy(ImVec2(1, 1));
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
    const float top = rows[RowFader] + 9, bot = rows[RowMuteSolo] - 4 - 9;
    float lastY = -1e9f;  // K-112b (second pass): a mark too close to the one above is left out, so labels never overlap
    for (int db : {6, 0, -6, -12, -24, -48}) {
      // the fader's travel is linear in its normalised value; the marks follow the volume parameter
      const ParamId pid = mixerParam(s.mixer[0].id, MixerParam::Volume);
      const auto d = describeParam(s, pid);
      if (!d) break;
      const double lin = std::pow(10.0, db / 20.0);
      const double n = d->toNormalized(d->clamp(lin));
      if (lin > d->max + 1e-9) continue;
      const float yy = std::round(bot - static_cast<float>(n) * (bot - top));
      if (yy - lastY < fs + 1) continue;
      lastY = yy;
      char t[8];
      std::snprintf(t, sizeof t, "%d", db);
      ImFont* const lf = logicplug::active(a) ? a.fontRegular : a.fontMono;  // K-112D (audit X3)
      const float tw = lf->CalcTextSizeA(fs - 1, 1e9f, 0, t).x;
      dl->AddText(lf, fs - 1, ImVec2(std::round(o.x + legendW - 14 - tw), yy - (fs - 1) * 0.5f), P.textFaint, t);
      dl->AddLine(ImVec2(o.x + legendW - 11, yy), ImVec2(o.x + legendW - 6, yy), P.textFaint);
    }
  }
}

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
    const bool newSettings = a.settingsChannel != kNoId && a.settingsChannel != lastSettings &&
                             layout::windowHome("###channelsettings", p).kind == layout::HomeKind::Host;
    const bool newFx = a.fxEditor.first != kNoId && (a.fxEditor.first != lastFx || a.fxEditor.second != lastSlot) &&
                       layout::windowHome("###fxeditor", p).kind == layout::HomeKind::Host;
    if (newSettings || newFx)
      if (bool* f = flag(a, "Device chain")) {
        *f = true;
        a.focusWindow = "Device chain";
      }
  }
  lastSettings = a.settingsChannel, lastFx = a.fxEditor.first, lastSlot = a.fxEditor.second;
  int i = 0;
  for (const PartDef& p : kParts) part(a, p.window, i++, p.size, [&] { p.draw(a); });
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
  for (const PartDef& p : kParts)
    if (tracker && std::any_of(std::begin(trackerOwn), std::end(trackerOwn), [&](const char* w) { return std::strcmp(w, p.window) == 0; })) continue;
    else if (bool* f = flag(a, p.window); f && partListed(p.window)) ImGui::MenuItem(i18n::tr(p.window), nullptr, f);
}

}  // namespace gl::layoutparts

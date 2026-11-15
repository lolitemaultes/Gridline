// Gridline - the Live layout's own chrome (K-112 phase C). See LiveLook.h.
#include "LiveLook.h"
#include "LiveGrid.h"
#include "PixelGrid.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/LayoutShell.h"
#include "../core/Params.h"
#include "../core/PluginInfo.h"
#include "../core/Arrangements.h"
#include "../core/TrackModes.h"
#include "../core/TrackOrder.h"
#include "../packs/PacksUi.h"
#include "App.h"
#include "BrowserUi.h"
#include "CollabUi.h"
#include "CursorAnchor.h"
#include "DeviceViews.h"
#include "HostedUi.h"
#include "Keys.h"
#include "HostedBodies.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "MixerMeters.h"
#include "PluginUI.h"
#include "Theme.h"
#include "TrackModeUi.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::livelook {

using theme::pal;

bool active(const App& a) { return layoutui::active() == layout::Preset::Live && layoutui::shellBuilt(a, layout::Preset::Live); }

ImFont* numFont(const App& a) { return active(a) ? a.fontRegular : a.fontMono; }
ImU32 envelopeColour() { return theme::rgb(0xEE5A50); }

int captureNow(App& a) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(a.curPattern);
  if (!p) {
    a.notify(i18n::tr("Select a pattern first"), true);
    return 0;
  }
  const std::string name = p->name;
  const int n = a.capture.commit(a.doc, a.curPattern, s.tempo, 0, a.now);
  if (n > 0) a.notify(std::to_string(n) + " " + i18n::tr("notes captured into") + " " + name);
  else a.notify(i18n::tr("Nothing to capture: play some notes first"));
  return n;
}

bool sessionRecording(const App& a) { return a.recordArmed && a.engine.mode() == PlayMode::Pattern; }

void toggleSessionRecord(App& a) {
  if (sessionRecording(a)) {
    a.recordArmed = false;
    return;
  }
  a.engine.setMode(PlayMode::Pattern);
  a.recordArmed = true;
  if (!a.engine.playing()) a.startPlayback();
}

bool drawMode(const App& a) { return (a.editFocus == App::EditFocus::Piano ? a.piano.tool : a.playlist.tool) == Tool::Draw; }

void setDrawMode(App& a, bool on) { a.piano.tool = a.playlist.tool = on ? Tool::Draw : Tool::Select; }

// ------------------------------------------------------------ main view ----

bool gridView(const App& a) { return a.showLivePads && !a.showPlaylist; }

void showView(App& a, bool grid) {
  a.showLivePads = grid;
  a.showPlaylist = !grid;
  a.focusWindow = grid ? "Live pads" : "Playlist";
}

void viewFrame(App& a) {
  {  // the Live way with installed plugins' windows (Settings > Interface)
    PluginWindowPolicy p;
    p.managed = true;
    p.openOnLoad = a.settings.getBool("live.pluginWindowsOnLoad");
    p.several = a.settings.getBool("live.pluginWindowsSeveral");
    p.selectedOnly = a.settings.getBool("live.pluginWindowsSelectedOnly");
    const Song& s = a.doc.song();
    const Channel* ch = s.channel(a.selChannel);
    p.channel = ch ? ch->id : kNoId;
    p.mixer = ch ? ch->mixerTrack : a.selMixer;
    setPluginWindowPolicy(p);
  }
  {  // Round 45: selecting a clip shows the clip band in place of the device band (one detail band at a
     // time, as the reference's detail view turns to the clip view when a clip is selected).
    // Not while the button is held: the press that selects a clip also starts its drag, and the band
    // swap rebuilds the dock (the arrangement moves under the pointer and the drag was lost). The
    // band turns once the button is released.
    static std::set<ObjectId> lastSel;
    if (a.playlist.selected != lastSel && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      lastSel = a.playlist.selected;
      if (!lastSel.empty()) layoutui::setRegionVisible(a, "live.clip", true);
    }
  }
  static bool wasGrid = false;
  if (a.showLivePads && a.showPlaylist) showView(a, !wasGrid);  // one just opened: it wins
  else if (!a.showLivePads && !a.showPlaylist) showView(a, wasGrid);  // both closed: the last one comes back
  wasGrid = gridView(a);
}

// -------------------------------------------------------- track headers ----

namespace {

struct Cell {
  ImVec2 a, b;
  bool contains(ImVec2 m) const { return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y; }
};

// The parts of a header, left to right: name chip, activator, S, arm, volume (or Back to
// arrangement) and pan (on the second row only).
struct HeaderCells {
  Cell name, act, solo, arm, vol, pan;
  bool hasPan = false;
};

// The name block fills the header's left part in the track's colour; the mixer cells sit in a
// column on its right (activator, S, arm on top, the volume under them when the row is tall
// enough, beside them otherwise, with the pan beside the volume), and a level meter runs down the
// right edge.
constexpr float kHeadMeterW = 10.0f;
HeaderCells cellsOf(const HeaderDraw& h) {
  const float th = h.h1.y - h.h0.y;
  const float ch = std::min(16.0f, std::max(10.0f, th - 6.0f));
  const float y0 = std::round(h.h0.y + std::min(4.0f, std::max(2.0f, (th - ch) * 0.5f)));
  const float x1 = h.h1.x - kHeadMeterW - 6;
  const float sq = 18.0f, actW = 38.0f, gap = 3.0f, colW = actW + gap + sq + gap + sq;
  const bool twoRows = th >= ch * 2 + 10;
  HeaderCells c;
  const float rx0 = twoRows ? x1 - colW : x1 - colW - gap - 44.0f;
  c.act = {ImVec2(rx0, y0), ImVec2(rx0 + actW, y0 + ch)};
  c.solo = {ImVec2(c.act.b.x + gap, y0), ImVec2(c.act.b.x + gap + sq, y0 + ch)};
  c.arm = {ImVec2(c.solo.b.x + gap, y0), ImVec2(c.solo.b.x + gap + sq, y0 + ch)};
  // K-112c (round 28): on the second row the volume sits under the activator, as wide as it, and
  // the pan spans S and arm (280, 357, 141, 142).
  c.vol = twoRows ? Cell{ImVec2(rx0, y0 + ch + 4), ImVec2(c.act.b.x, y0 + ch * 2 + 4)} : Cell{ImVec2(c.arm.b.x + gap, y0), ImVec2(x1, y0 + ch)};
  c.hasPan = twoRows;
  if (twoRows) c.pan = {ImVec2(c.solo.a.x, y0 + ch + 4), ImVec2(c.arm.b.x, y0 + ch * 2 + 4)};
  // The name block fills its part of the row edge to edge: one line of the panel between tracks.
  c.name = {ImVec2(h.h0.x, h.h0.y), ImVec2(std::max(h.h0.x + 30, rx0 - 6), h.h1.y)};
  return c;
}

bool soloed(const Song& s, int track) {
  if (track < 0 || track >= static_cast<int>(s.tracks.size()) || s.tracks[static_cast<size_t>(track)].muted || s.tracks.size() < 2) return false;
  for (size_t k = 0; k < s.tracks.size(); ++k)
    if (static_cast<int>(k) != track && !s.tracks[k].muted) return false;
  return true;
}

void square(ImDrawList* dl, const Cell& c, ImU32 fill, bool hot) {
  const auto& P = pal();
  // K-112c (round 31): pressed (hovered with the button down) the face sinks a shade, lit or not.
  const bool down = hot && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  dl->AddRectFilled(c.a, c.b, down ? theme::mix(fill, IM_COL32(0, 0, 0, 255), 0.18f) : fill, theme::kRadiusSm);
  // K-112c: a 1 px rim a shade darker than the face (lit or not), as the refs outline these buttons;
  // hovered, a light one.
  const ImU32 rim = hot ? theme::withAlpha(P.text, 0.45f) : theme::mix(fill, IM_COL32(0, 0, 0, 255), 0.35f);
  dl->AddRect(c.a, c.b, rim, theme::kRadiusSm);
}

void centredText(ImDrawList* dl, ImFont* f, float fs, const Cell& c, ImU32 col, const char* t) {
  theme::centreText(dl, f, fs, c.a, c.b, col, t);  // by the font's ink and capital height (K-112)
}

// The volume (or pan) drag of a header's cell (the linked mixer track's fader or pan).
struct VolDrag {
  ObjectId mixer = kNoId;
  double start = 0;
  float y0 = 0;
  bool pan = false;
} gVol;

// A pan value as the header shows it: C, or 1 to 50 with L or R.
std::string panText(float pan) {
  const int v = static_cast<int>(std::lround(pan * 50.0f));
  if (v == 0) return "C";
  return std::to_string(std::abs(v)) + (v < 0 ? "L" : "R");
}

}  // namespace

namespace {
// K-112c (round 30): Main's title is the brightest block and a return's a light grey, as the dark refs'
// headers and strips (357, 280); in a light theme they turn dark (093).
ImU32 mainFill() { return pal().text; }
ImU32 returnFill() { return theme::mix(pal().text, pal().surface800, 0.42f); }
char returnLetter(const Song& s, int mixerIndex);

void busRow(App& a, ImDrawList* dl, size_t j, float x0, float x1, float y) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const MixerTrack& mt = s.mixer[j];
  const ImVec2 m = ImGui::GetIO().MousePos;
  const ImVec2 r0(x0 + 1, y), r1(x1 - 1, y + kBusRowH - 2);
  // K-112c (round 31): the name block ends where the tracks' name blocks end and the volume sits in
  // their activator column, so the rows' columns line up (357, 280, 093).
  HeaderDraw hd;
  hd.h0 = ImVec2(x0 + 1, y), hd.h1 = ImVec2(x1, y + 60.0f);  // a two-row header's columns
  const HeaderCells hc = cellsOf(hd);
  const float meterX = r1.x - 12, nameX1 = hc.name.b.x;
  const bool hov = m.x >= r0.x && m.x < r1.x && m.y >= r0.y && m.y < r1.y && ImGui::IsWindowHovered();
  const bool sel = a.selMixer == mt.id;
  const ImU32 base = j == 0 ? mainFill() : returnFill();
  const ImU32 fill = hov ? theme::mix(base, P.surface700, 0.15f) : base;
  dl->AddRectFilled(r0, r1, P.surface800);  // K-112c (round 32): the row's face, as a track header's, behind the fields
  dl->AddRectFilled(r0, ImVec2(nameX1, r1.y), fill, 0.0f);  // square, as the tracks' title blocks (141, 280)
  if (sel) dl->AddRect(ImVec2(r0.x + 0.75f, r0.y + 0.75f), ImVec2(nameX1 - 0.75f, r1.y - 0.75f), P.hot, 0.0f, 0, 1.5f);
  std::string name = j == 0 ? std::string(i18n::tr("Main")) : mt.name;
  if (const char lt = j == 0 ? 0 : returnLetter(s, static_cast<int>(j))) name = std::string(1, lt) + " " + name;  // as the strips' titles
  const ImVec4 clip(r0.x, r0.y, nameX1 - 2, r1.y);
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(r0.x + 5, theme::textTop(a.fontRegular, theme::kFontCaption, r0.y, kBusRowH - 2)),
              theme::contrastText(fill), name.c_str(), nullptr, 0, &clip);
  // Volume (dB) and the meter.
  const ImVec2 v0(hc.act.a.x, r0.y + 2), v1(hc.act.b.x, r1.y - 2);
  dl->AddRectFilled(v0, v1, P.dark ? P.field : P.surface950, theme::kRadiusSm);
  char vb[16];
  if (mt.volume > 1e-6f) std::snprintf(vb, sizeof vb, "%.1f", static_cast<double>(20.0f * std::log10(mt.volume)));
  else std::snprintf(vb, sizeof vb, "-\xE2\x88\x9E");  // minus infinity, as the refs write it
  const float tw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, vb).x;
  dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(v1.x - 4 - tw), theme::textTop(a.fontRegular, theme::kFontLabel, v0.y, v1.y - v0.y)), P.text, vb);
  const mixmeters::Reading rd = mixmeters::get(a, mt.id);
  ImGui::SetCursorScreenPos(ImVec2(meterX, r0.y + 1));
  ImGui::PushID(static_cast<int>(7000 + j));
  ui::Meter(ImGui::GetID("##busmeter"), rd.peakL, rd.peakR, ImVec2(10, kBusRowH - 4));
  ImGui::PopID();
  if (hov) {
    a.setHint(j == 0 ? i18n::tr("Main: the master track (click to select it in the mixer)")
                     : i18n::tr("Return track: other tracks send to it (click to select it in the mixer)"));
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) a.selMixer = mt.id;
  }
}
}  // namespace

void drawMainRow(App& a, ImDrawList* dl, float x0, float x1, float y) {
  const auto& P = pal();
  if (a.doc.song().mixer.empty()) return;
  dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(x1, y + kBusRowH), P.surface900);
  dl->AddLine(ImVec2(x0, y - 3.5f), ImVec2(x1, y - 3.5f), P.line);
  busRow(a, dl, 0, x0, x1, y);
}

namespace {
// Returns: mixer tracks that a send reaches (not the master), in mixer order.
std::vector<size_t> returnTracks(const Song& s) {
  std::vector<size_t> rows;
  for (size_t j = 1; j < s.mixer.size(); ++j) {
    bool target = false;
    for (const MixerTrack& m : s.mixer)
      for (const Send& sd : m.sends) target = target || (!sd.sidechain && sd.to == s.mixer[j].id);
    if (target) rows.push_back(j);
  }
  return rows;
}
// A return's letter (A, B, ... by its place among the returns), or 0.
char returnLetter(const Song& s, int mixerIndex) {
  const std::vector<size_t> r = returnTracks(s);
  for (size_t i = 0; i < r.size() && i < 26; ++i)
    if (static_cast<int>(r[i]) == mixerIndex) return static_cast<char>('A' + i);
  return 0;
}
}  // namespace

float drawBusRows(App& a, ImDrawList* dl, float x0, float x1, float top, float bottom) {
  const Song& s = a.doc.song();
  if (s.mixer.empty()) return 0;
  const std::vector<size_t> rows = returnTracks(s);
  float y = top + 6;  // a gap after the last track
  for (size_t j : rows) {
    if (y + kBusRowH >= bottom) break;
    busRow(a, dl, j, x0, x1, y);
    y += kBusRowH;
  }
  return y - top;
}

HeaderPart drawHeader(App& a, ImDrawList* dl, const HeaderDraw& h) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  if (h.track < 0 || h.track >= static_cast<int>(s.tracks.size())) return HeaderPart::None;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(h.track)];
  const HeaderCells c = cellsOf(h);
  const ImVec2 m = h.mouse;
  // The row: a raised face, lit while selected or moving.
  const ImU32 face = h.lit || h.selected ? P.surface700 : h.hovered ? theme::mix(P.surface800, P.surface700, 0.5f) : P.surface800;
  dl->AddRectFilled(h.h0, h.h1, face);
  if (h.lit) dl->AddRect(h.h0, h.h1, theme::withAlpha(P.hot, 0.7f), 0, 1.5f, 0);
  else if (h.selected) dl->AddRect(h.h0, h.h1, theme::withAlpha(P.hot, 0.45f));
  // Name block in the track's colour: number and name at its top, with Studio's mode glyph for linked tracks.
  const ImU32 tc = theme::content(t.color);
  const ImU32 chip = t.muted ? theme::mix(tc, P.surface800, 0.55f) : tc;
  dl->AddRectFilled(c.name.a, c.name.b, chip, 0.0f);  // K-112c (round 28): square, as every ref's title block
  const ImU32 ink = theme::contrastText(chip);
  const float fs = theme::kFontCaption + 1;
  const float ty = theme::textTop(a.fontRegular, fs, c.act.a.y, c.act.b.y - c.act.a.y);
  // K-112c (round 30): the name alone, as every ref (357, 280, 141, 093); the number is on the activator.
  const std::string label = playlistTrackName(s, static_cast<size_t>(h.track));
  dl->PushClipRect(c.name.a, c.name.b, true);
  float nx = c.name.a.x + 5;
  if (trackmodes::modeOf(s, h.track) != TrackMode::Unassigned) nx += trackui::drawNameGlyph(a, dl, h.track, ImVec2(nx, ty), theme::withAlpha(ink, 0.8f));  // K-112c (round 37): the name's ink, not a faint grey on the colour
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), c.name.b.x - nx - 4, ink, label.c_str());  // regular weight (280, 141, 142)
  dl->PopClipRect();
  // Activator (the track's mute, numbered), solo, arm.
  const bool on = !t.muted;
  char num[16];
  std::snprintf(num, sizeof num, "%d", h.track + 1);
  square(dl, c.act, on ? P.hot : P.surface900, h.hovered && c.act.contains(m));
  centredText(dl, a.fontRegular, theme::kFontLabel, c.act, on ? theme::contrastText(P.hot) : P.textFaint, num);
  const bool solo = soloed(s, h.track);
  square(dl, c.solo, solo ? P.solo : P.surface900, h.hovered && c.solo.contains(m));
  centredText(dl, a.fontBold, theme::kFontLabel, c.solo, solo ? theme::contrastText(P.solo) : P.textDim, "S");
  const MixerTrack* mx = trackmodes::linkedMixer(s, h.track);
  const bool audio = trackmodes::modeOf(s, h.track) == TrackMode::Audio;
  const bool armed = audio && mx && mx->armed;
  square(dl, c.arm, armed ? theme::withAlpha(P.arm, 0.85f) : P.surface900, audio && h.hovered && c.arm.contains(m));
  const ImVec2 ac((c.arm.a.x + c.arm.b.x) * 0.5f, (c.arm.a.y + c.arm.b.y) * 0.5f);
  dl->AddCircleFilled(ac, 3.5f, armed ? theme::contrastText(P.arm) : audio ? P.text : theme::withAlpha(P.textDim, 0.45f), 12);  // K-112c (round 19): a neutral dot until armed (142, 157, 143)
  // Volume (the linked mixer track's fader) or, while the grid plays a clip here, Back to arrangement.
  const bool onGrid = a.engine.mode() == PlayMode::Live && a.engine.liveActive(h.track) != kNoId;
  if (onGrid) {
    square(dl, c.vol, P.hot, h.hovered && c.vol.contains(m));
    ui::drawIconFit(dl, ui::Icon::Undo, ImVec2(c.vol.a.x + 13, c.vol.a.y + 3), ImVec2(c.vol.b.x - 13, c.vol.b.y - 3), theme::contrastText(P.hot));
  } else {
    dl->AddRectFilled(c.vol.a, c.vol.b, P.dark ? P.field : P.surface950, theme::kRadiusSm);
    if (h.hovered && c.vol.contains(m) && mx) dl->AddRect(c.vol.a, c.vol.b, theme::withAlpha(P.text, 0.35f), theme::kRadiusSm);
    char vb[16] = "-";
    if (mx) {
      const float g = mx->volume;
      if (g > 1e-6f) std::snprintf(vb, sizeof vb, "%.1f", static_cast<double>(20.0f * std::log10(g)));
      else std::snprintf(vb, sizeof vb, "-\xE2\x88\x9E");  // minus infinity, as the refs write it
    }
    const float vw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, vb).x;
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(c.vol.b.x - 5 - vw), theme::textTop(a.fontRegular, theme::kFontLabel, c.vol.a.y, c.vol.b.y - c.vol.a.y)),
                mx ? P.text : P.textFaint, vb);
  }
  if (c.hasPan) {  // the pan field (the linked mixer track's pan)
    dl->AddRectFilled(c.pan.a, c.pan.b, P.dark ? P.field : P.surface950, theme::kRadiusSm);
    if (h.hovered && c.pan.contains(m) && mx) dl->AddRect(c.pan.a, c.pan.b, theme::withAlpha(P.text, 0.35f), theme::kRadiusSm);
    centredText(dl, a.fontRegular, theme::kFontLabel, c.pan, mx ? P.text : P.textFaint, mx ? panText(mx->pan).c_str() : "-");
  }
  // The level meter down the right edge (the linked mixer track's).
  if (mx) {
    const mixmeters::Reading r = mixmeters::get(a, mx->id);
    const float mx0 = h.h1.x - kHeadMeterW - 3, my0 = h.h0.y + 3, my1 = h.h1.y - 3;
    dl->AddRectFilled(ImVec2(mx0, my0), ImVec2(mx0 + kHeadMeterW, my1), P.dark ? P.field : P.surface950, 1.0f);
    auto bar = [&](float x, float peak) {
      const float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -90.0f;
      const float u = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
      const float bw = (kHeadMeterW - 3) * 0.5f;
      if (u > 0) dl->AddRectFilled(ImVec2(x, my1 - (my1 - my0) * u), ImVec2(x + bw, my1), db > -0.1f ? P.meterHigh : db > -12 ? P.meterMid : P.meterLow);
    };
    bar(mx0 + 1, r.peakL);
    bar(mx0 + 1 + (kHeadMeterW - 2) * 0.5f, r.peakR);
  }
  // The volume drag in progress (moves the mixer track's volume, one undo step per drag).
  if (gVol.mixer != kNoId) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && gVol.pan) {
      const double dy = static_cast<double>(gVol.y0 - ImGui::GetIO().MousePos.y);
      a.doc.setParam(mixerParam(gVol.mixer, MixerParam::Pan), std::clamp(gVol.start + dy * (ImGui::GetIO().KeyShift ? 0.002 : 0.01), -1.0, 1.0), true);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const double dy = static_cast<double>(gVol.y0 - ImGui::GetIO().MousePos.y);
      const double db0 = gVol.start > 1e-6 ? 20.0 * std::log10(gVol.start) : -60.0;
      const double db = std::clamp(db0 + dy * (ImGui::GetIO().KeyShift ? 0.05 : 0.25), -60.0, 20.0 * std::log10(1.25));
      a.doc.setParam(mixerParam(gVol.mixer, MixerParam::Volume), db <= -59.9 ? 0.0 : std::pow(10.0, db / 20.0), true);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else {
      a.doc.endGesture();
      gVol = VolDrag{};
    }
  }
  if (!h.hovered) return HeaderPart::None;
  if (c.act.contains(m)) return HeaderPart::Activator;
  if (c.solo.contains(m)) return HeaderPart::Solo;
  if (c.arm.contains(m)) return audio ? HeaderPart::Arm : HeaderPart::None;
  if (c.vol.contains(m)) return onGrid ? HeaderPart::BackToArrangement : mx ? HeaderPart::Volume : HeaderPart::None;
  if (c.hasPan && c.pan.contains(m)) return mx ? HeaderPart::Pan : HeaderPart::None;
  if (c.name.contains(m)) return HeaderPart::Name;
  return HeaderPart::None;
}

bool clickHeader(App& a, int track, HeaderPart part, bool right) {
  const Song& s = a.doc.song();
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return false;
  switch (part) {
    case HeaderPart::Activator:
      if (!right) {
        a.doc.editPlaylistTrack(track, "mute playlist track", [](PlaylistTrack& t) { t.muted = !t.muted; });
        return true;
      }
      [[fallthrough]];  // right-click: solo, as Studio's switch
    case HeaderPart::Solo: {
      const bool solo = soloed(s, track);
      a.doc.editSong(solo ? "unsolo playlist track" : "solo playlist track", [track, solo](Song& x) {
        for (size_t k = 0; k < x.tracks.size(); ++k) x.tracks[k].muted = solo ? false : static_cast<int>(k) != track;
      });
      return true;
    }
    case HeaderPart::Arm: {
      const MixerTrack* mx = trackmodes::linkedMixer(s, track);
      const bool to = !(mx && mx->armed);
      a.doc.editSong(to ? "arm track" : "disarm track", [track, to](Song& x) { trackmodes::setArmed(x, track, to); });
      return true;
    }
    case HeaderPart::Volume:
      if (right) return false;
      if (const MixerTrack* mx = trackmodes::linkedMixer(s, track)) {
        gVol.mixer = mx->id;
        gVol.start = mx->volume;
        gVol.y0 = ImGui::GetIO().MousePos.y;
      }
      return true;
    case HeaderPart::Pan:
      if (const MixerTrack* mx = trackmodes::linkedMixer(s, track)) {
        if (right || ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) {  // back to the centre
          a.doc.setParam(mixerParam(mx->id, MixerParam::Pan), 0.0, false);
          return true;
        }
        gVol.mixer = mx->id;
        gVol.start = mx->pan;
        gVol.y0 = ImGui::GetIO().MousePos.y;
        gVol.pan = true;
      }
      return true;
    case HeaderPart::BackToArrangement:
      a.engine.stopLiveTrack(track);
      return true;
    default: return false;
  }
}

const char* headerHint(HeaderPart part) {
  switch (part) {
    case HeaderPart::Activator: return i18n::tr("Track on / off: click mutes or unmutes it, right-click solos it");
    case HeaderPart::Solo: return i18n::tr("Solo: only this track plays (click again for all)");
    case HeaderPart::Arm: return i18n::tr("Record arm: the track's mixer track records its input");
    case HeaderPart::Volume: return i18n::tr("Volume of the track's mixer track (dB): drag up or down, Shift for fine steps");
    case HeaderPart::Pan: return i18n::tr("Pan of the track's mixer track: drag up or down (Shift: fine), right-click centres it");
    case HeaderPart::BackToArrangement: return i18n::tr("Back to arrangement: the track stops its grid clip and plays the arrangement again");
    case HeaderPart::Name: return i18n::tr("Double-click the name to rename the track \xC2\xB7 right-click: track menu \xC2\xB7 drag to move it");
    default: return "";
  }
}

// ----------------------------------------------------------- status line ----

namespace {
std::string selectedTrackName(App& a) {
  const Song& s = a.doc.song();
  if (!a.playlist.trackSel.empty()) {
    const int t = *a.playlist.trackSel.begin();
    if (t >= 0 && t < static_cast<int>(s.tracks.size())) return std::to_string(t + 1) + ". " + playlistTrackName(s, static_cast<size_t>(t));
  }
  if (const Channel* c = s.channel(a.selChannel)) {
    const int t = trackmodes::instrumentTrackOf(s, c->id);
    return t >= 0 ? std::to_string(t + 1) + ". " + playlistTrackName(s, static_cast<size_t>(t)) : c->name;
  }
  return std::string();
}
}  // namespace

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
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();
  const float fs = theme::kFontCaption;
  const float ty = theme::textTop(a.fontRegular, fs, wp.y, h);
  // Right: the band toggles (lit while shown), then what is selected.
  const layout::Shell& sh = layout::shell(layout::Preset::Live);
  const layout::State st = layoutui::regionState(a, layout::Preset::Live);
  float rx = wp.x + W - 4;
  struct Tg {
    const char* id;
    const char* label;
    const char* region;
    const char* hint;
    const char* part;  // the window a band with nothing open in it opens
  };
  // K-297: the mixer band has its toggle here too, in the corner (as the refs keep it); before, it could
  // only be shown or hidden from the View menu.
  const Tg tg[] = {{"##sb_mix", i18n::N_("Mixer"), "live.mixer", i18n::N_("Mixer band: the tracks' levels, pans and sends under the main view"), "Mixer"},
                   {"##sb_dev", i18n::N_("Devices"), "live.device", i18n::N_("Device band: the selected track's instrument and the inserts of its mixer track"), "Device chain"},
                   {"##sb_clip", i18n::N_("Clip"), "live.clip", i18n::N_("Clip band: the selected clip's properties and editors"), "Piano roll"}};
  for (const Tg& t : tg) {
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(layoutui::regionNode(t.region));
    const bool on = layout::visible(sh, st, t.region) && node && node->Windows.Size > 0;  // shown, with something in it
    const char* text = i18n::tr(t.label);
    const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, text).x + 18;
    rx -= tw;
    const ImVec2 bp(rx, wp.y + 4);
    ImGui::SetCursorScreenPos(bp);
    const bool click = ImGui::InvisibleButton(t.id, ImVec2(tw, h - 7));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(bp, ImVec2(bp.x + tw, bp.y + h - 7), on ? P.hot : hov ? P.surface700 : P.surface800, theme::kRadiusSm);
    dl->AddText(a.fontRegular, fs, ImVec2(bp.x + 9, ty), on ? theme::contrastText(P.hot) : P.textDim, text);
    ui::Hint(t.hint);
    if (click) {
      if (on) {
        layoutui::setRegionVisible(a, t.region, false);
      } else {
        layoutui::setRegionVisible(a, t.region, true);
        // A band with nothing open in it opens its main part: the notes editor, the device chain, the mixer.
        if (bool* f = layoutui::windowOpenFlag(a, t.part)) *f = true;
      }
    }
    rx -= 4;
  }
  rx -= theme::kS3;
  std::string sel = selectedTrackName(a);
  if (const Channel* c = a.doc.song().channel(a.selChannel))
    if (const PluginInfo* pi = findPlugin(c->plugin.type)) sel += (sel.empty() ? "" : " \xC2\xB7 ") + pi->name;
  if (!sel.empty()) {
    const float sw = std::min(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, sel.c_str()).x, W * 0.3f);
    rx -= sw;
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(rx, ty), sw + 1, P.textDim, sel.c_str());
  }
  if (a.doc.modified()) {
    rx -= theme::kS3;
    dl->AddCircleFilled(ImVec2(rx + 3, ty + fs * 0.5f + 1), 3.0f, P.textDim, 12);
  }
  // Far left (as the refs have it): the button that shows or hides the info corner, lit while shown.
  float lx = wp.x + 4;
  {
    const float bw = h - 7;
    const bool on = layout::visible(sh, st, "live.info");
    const ImVec2 bp(lx, wp.y + 4);
    ImGui::SetCursorScreenPos(bp);
    const bool click = ImGui::InvisibleButton("##sb_info", ImVec2(bw, bw));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bw), on ? P.hot : hov ? P.surface700 : P.surface800, theme::kRadiusSm);
    ui::drawIconFitC(dl, ui::Icon::Info, ImVec2(bp.x + bw * 0.5f, bp.y + bw * 0.5f), ImVec2(bw - 8, bw - 8), on ? theme::contrastText(P.hot) : P.textDim);
    ui::Hint(i18n::tr("Info: shows or hides the corner that names what you point at and says what it does"));
    if (click) layoutui::setRegionVisible(a, "live.info", !on);
    lx += bw + theme::kS3;
  }
  // Left: the view and what it is for.
  const bool grid = gridView(a);
  const std::string view = grid ? i18n::tr("Clip grid") : i18n::tr("Arrangement");
  const char* what = grid ? i18n::tr("click a slot to launch its clip, the scene button launches the row")
                          : i18n::tr("the Grid switch at the top shows the clip grid");
  // K-112c (round 19): the message sits in a long recessed field from the info button to the right
  // end's items, as every ref's status bar (072, 093, 198, 276, 280).
  {
    const ImVec2 f0(lx - theme::kS1, wp.y + 4), f1(rx - theme::kS2, wp.y + h - 3);
    if (f1.x - f0.x > 60) {
      dl->AddRectFilled(f0, f1, P.dark ? P.field : P.surface950, 2.0f);
      dl->AddRect(f0, f1, theme::mix(P.dark ? P.field : P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), 2.0f);
    }
    lx += theme::kS1;
  }
  const float room = rx - lx - theme::kS4;
  const float vw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, view.c_str()).x;
  dl->AddText(a.fontRegular, fs, ImVec2(lx, ty), P.text, view.c_str());
  if (room > vw + 40) {
    const std::string rest = std::string(" \xC2\xB7 ") + what;
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(lx + vw, ty), room - vw, P.textDim, rest.c_str());
  }
  ImGui::End();
}

// ------------------------------------------------------------ info corner ----

void infoCorner(App& a) {
  const auto& P = pal();
  static std::string last;
  if (!a.hint.empty()) last = a.hint;
  std::string title, body;
  if (last.empty()) {
    title = i18n::tr("Info");
    body = i18n::tr("Point at anything: its name and what it does show here.");
  } else {
    // "Subject   (help)" or "Subject: help" -> a title and its text.
    size_t cut = last.find("   (");
    size_t skip = 4;
    if (cut == std::string::npos) cut = last.find(": "), skip = 2;
    if (cut != std::string::npos && cut > 0 && cut < 60) {
      title = last.substr(0, cut);
      body = last.substr(cut + skip);
      if (skip == 4 && !body.empty() && body.back() == ')') body.pop_back();
    } else {
      body = last;
    }
  }
  ImGui::Dummy(ImVec2(0, 2));
  if (!title.empty()) {
    ImGui::PushFont(a.fontBold, theme::kFontCaption + 1);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
  }
  if (!body.empty()) {
    if (!body.empty()) body[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(body[0])));
    ImGui::PushFont(a.fontRegular, theme::kFontCaption + 1);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::mix(P.textDim, P.text, 0.6f)));  // K-112c (round 19): nearer the text colour (072, 093)
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(body.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
}

// ------------------------------------------------------------ device band ----

namespace {

// K-112: a device's on / off lamp, as the refs draw it: a dot in the accent colour while on, an
// empty ring while off.
void deviceLamp(ImDrawList* dl, ImVec2 c, bool on) {
  const auto& P = pal();
  c = ImVec2(std::round(c.x), std::round(c.y));
  if (on) {  // K-112c: the lit lamp with a thin dark rim, as the refs draw it
    dl->AddCircleFilled(c, 5.0f, P.accent, 20);
    dl->AddCircle(c, 5.0f, IM_COL32(16, 18, 22, 150), 20, 1.0f);
  }
  else dl->AddCircle(c, 4.5f, P.textFaint, 20, 1.2f);
}

// One device of the chain: the instrument (slot -1) or an insert of the mixer track.
struct Device {
  PluginTarget target;
  const PluginInfo* info = nullptr;
  std::string name, sub;
  bool enabled = true;
  int knobs = 0, cols = 0;
  float visW = 0;       // the compact view's width (DeviceViews), 0 without one
  bool hosted = false;  // an installed plugin: its own window, never redrawn here
  bool above = false;   // the picture spans the card's top, one row of knobs under it
  std::vector<devview::Main> mains;  // its main controls (DeviceViews); empty: its first parameters
  // The k-th knob's parameter and label.
  int param(int k) const { return mains.empty() ? k : mains[static_cast<size_t>(k)].param; }
  std::string label(int k) const {
    const int p = param(k);
    if (!mains.empty() && !mains[static_cast<size_t>(k)].label.empty()) return mains[static_cast<size_t>(k)].label;
    return info && p >= 0 && p < static_cast<int>(info->params.size()) ? info->params[static_cast<size_t>(p)].name : std::string();
  }
};

constexpr float kCardTitleH = 20.0f, kCardPad = 8.0f, kKnobCellW = 60.0f, kFoldedW = 22.0f;
constexpr float kHostedRowsW = 236.0f, kHostedShutW = 132.0f;
constexpr int kAllParamsMax = 64;  // an installed plugin with more starts with an empty panel (Configure fills it)

// What the band remembers per device (by target key; moves keep it with the device).
struct BandUi {
  uint64_t selected = 0;        // the selected device (Delete removes it, P switches its A/B)
  std::set<uint64_t> folded;    // shown as a narrow strip
  std::set<uint64_t> expanded;  // its whole editor shows in the panel above the chain
  std::set<uint64_t> shut;      // an installed plugin's parameter panel is folded away
  std::set<uint64_t> configuring;
  std::map<uint64_t, int> seen;  // Configure: the last touched parameter when it was last looked at
  uint64_t rowSel = 0;           // Configure: the selected panel entry (target key) ...
  int rowSelParam = -1;          // ... and its parameter (Delete removes it)
  float expandH = 0;             // the panel's height (0: 45 % of the window)
  std::map<uint64_t, float> edSize;  // an expanded editor's width: a factor of its own width, or -1 (fill the panel)
  float plainH = 0;              // the band's height without the panel
  int boostFrame = -100;         // the last frame the band asked to grow (it stays grown a frame or two after)
};
BandUi& bandUi() {
  static BandUi u;
  return u;
}

// After moveSlot(owner, from, to) the devices' memory follows them.
void followMove(ObjectId owner, int from, int to) {
  BandUi& u = bandUi();
  auto moved = [&](int sl) {
    if (sl == from) return to;
    if (from < to && sl > from && sl <= to) return sl - 1;
    if (from > to && sl >= to && sl < from) return sl + 1;
    return sl;
  };
  auto remap = [&](uint64_t k) {
    for (int sl = 0; sl < kEffectSlots; ++sl)
      if (PluginTarget{owner, sl}.key() == k) return PluginTarget{owner, moved(sl)}.key();
    return k;
  };
  for (std::set<uint64_t>* s : {&u.folded, &u.expanded, &u.shut, &u.configuring}) {
    std::set<uint64_t> n;
    for (uint64_t k : *s) n.insert(remap(k));
    *s = std::move(n);
  }
  u.selected = remap(u.selected);
  u.rowSel = remap(u.rowSel);
}

// A parameter name cut to fit under its knob (the whole name shows in the knob's tooltip and hint).
std::string fitText(ImFont* f, float fs, const std::string& name, float w) {
  if (f->CalcTextSizeA(fs, 1e9f, 0, name.c_str()).x <= w) return name;
  std::string s = name;
  while (s.size() > 1 && f->CalcTextSizeA(fs, 1e9f, 0, (s + "\xE2\x80\xA6").c_str()).x > w) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s + "\xE2\x80\xA6";
}
std::string fitLabel(App& a, const std::string& name, float w) { return fitText(a.fontRegular, theme::kFontLabel, name, w); }

// The devices whose picture reads best wide: transfer curves and sweeps go above their knobs.
bool pictureAbove(const PluginInfo* info) {
  if (!info) return false;
  const std::string& ty = info->type;
  return ty == "gl.comp" || ty == "gl.clipper" || ty == "gl.dist" || ty == "gl.flanger" || ty == "gl.phaser" || ty == "gl.chorus";
}

// A band knob: its name above the dial and its value under it (the band's own order; the rack
// panels' knobs keep theirs). The dial is the panels' (drag, wheel, menu, typing a value).
float bandKnobRadius() { return (pui::knobCellHeight(pui::KnobSmall) - 32) * 0.5f; }
float bandKnobH() { return bandKnobRadius() * 2 + 30; }
void bandKnob(App& a, const PluginTarget& t, int param, ImVec2 pos, const std::string& label, float cell) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const ParamId pid = t.pid(param);
  const auto d = describeParam(s, pid);
  const auto v = getParam(s, pid);
  if (!d || !v) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const std::string lbl = fitLabel(a, label.empty() ? d->name : label, cell - 4);
  const ImVec2 ls = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl.c_str());
  dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(pos.x + (cell - ls.x) * 0.5f), pos.y), P.textDim, lbl.c_str());
  const float r = bandKnobRadius();
  const ImVec2 keep = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(ImVec2(std::round(pos.x + cell * 0.5f - r), pos.y + 14));
  pui::dial(a, t, param, r);
  const bool act = ImGui::IsItemActive();
  ImGui::SetCursorScreenPos(keep);
  const std::string vs = fitText(a.fontRegular, 11.0f, d->format(*v), cell - 2);
  const ImVec2 vz = a.fontRegular->CalcTextSizeA(11.0f, 1e9f, 0, vs.c_str());
  dl->AddText(a.fontRegular, 11.0f, ImVec2(std::round(pos.x + (cell - vz.x) * 0.5f), pos.y + 14 + r * 2 + 2), act ? P.paper : P.text, vs.c_str());
}

// Text reading upwards from `o` (a folded device's strip): drawn flat, then its vertices turned.
void upwardText(ImDrawList* dl, ImFont* f, float fs, ImVec2 o, ImU32 col, const std::string& text) {
  const int v0 = dl->VtxBuffer.Size;
  dl->PushClipRectFullScreen();
  dl->AddText(f, fs, o, col, text.c_str());
  dl->PopClipRect();
  for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float dx = v.pos.x - o.x, dy = v.pos.y - o.y;
    v.pos = ImVec2(o.x + dy, o.y - dx);
  }
}

// The installed plugin's panel entries: its chosen list, else every parameter up to kAllParamsMax.
std::vector<int> panelParams(const PluginState* st, const PluginInfo* info) {
  std::vector<int> out;
  const int n = info ? static_cast<int>(info->params.size()) : 0;
  if (st && !st->shown.empty()) {
    for (int k : st->shown)
      if (k >= 0 && k < n) out.push_back(k);
    return out;
  }
  if (n <= kAllParamsMax)
    for (int k = 0; k < n; ++k) out.push_back(k);
  return out;
}

// Writes the panel's list (one undo step; saved with the project and shared with collaborators).
void setShown(App& a, const PluginTarget& t, const std::vector<int>& v) {
  if (t.isChannel()) {
    a.doc.editChannel(t.owner, "device parameters", [&](Channel& c) { c.plugin.shown = v; });
  } else {
    a.doc.editMixerTrack(t.owner, "device parameters", [&](MixerTrack& m) { m.slots[static_cast<size_t>(t.slot)].plugin.shown = v; });
  }
}

float cardWidth(const Device& d) {
  const BandUi& u = bandUi();
  if (u.folded.count(d.target.key())) return kFoldedW;
  if (d.hosted) return u.shut.count(d.target.key()) ? kHostedShutW : kHostedRowsW;
  const float cell = kKnobCellW;
  if (d.above && d.visW > 0) return kCardPad * 2 + std::max(d.visW + 40, static_cast<float>(std::max(2, d.cols)) * cell);
  return kCardPad * 2 + (d.visW > 0 ? d.visW + 8 : 0.0f) + static_cast<float>(std::max(d.visW > 0 ? 1 : 3, d.cols)) * cell;
}

// The device's window is the floating one (Studio's), asked for from its title bar or menu.
bool inWindow(App& a, const BandPick& pick, const Device& d) {
  if (d.hosted) return pluginWindowShown(a, d.target);
  if (d.target.isChannel()) return pick.insWindow && a.settingsChannel == d.target.owner;
  return pick.fxWindow && a.fxEditor.first == d.target.owner && a.fxEditor.second == d.target.slot;
}
void toggleWindow(App& a, BandPick& pick, const Device& d) {
  if (d.hosted) {
    togglePluginWindow(a, d.target);
    return;
  }
  BandUi& u = bandUi();
  const bool shown = inWindow(a, pick, d);
  if (d.target.isChannel()) {
    pick.insWindow = !shown;
    a.settingsChannel = shown ? kNoId : d.target.owner;
    if (!shown) a.focusWindow = "###channelsettings";
  } else {
    pick.fxWindow = !shown;
    a.fxEditor = shown ? std::pair<ObjectId, int>{kNoId, -1} : std::pair<ObjectId, int>{d.target.owner, d.target.slot};
    if (!shown) a.focusWindow = "###fxeditor";
  }
  if (!shown) u.expanded.erase(d.target.key());  // one place at a time: the window or the panel
}

// A small glyph button of a device's title bar.
bool titleIcon(ImDrawList* dl, const char* id, ui::Icon ic, ImVec2 c0, float w, bool lit, const char* hint) {
  const auto& P = pal();
  ImGui::SetCursorScreenPos(c0);
  const bool click = ImGui::InvisibleButton(id, ImVec2(w, kCardTitleH - 4));
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 c1(c0.x + w, c0.y + kCardTitleH - 4);
  // K-112c: each title bar button sits in a thin ring (filled while hovered), as the refs draw them.
  const ImVec2 c(std::round((c0.x + c1.x) * 0.5f), std::round((c0.y + c1.y) * 0.5f));
  const float rr = 6.5f;
  if (hov) dl->AddCircleFilled(c, rr, P.surface600, 20);
  dl->AddCircle(c, rr, lit ? P.hot : theme::withAlpha(P.textDim, 0.75f), 20, 1.0f);
  ui::drawIcon(dl, ic, c, 7.0f, lit ? P.hot : hov ? P.text : P.textDim);
  ui::Hint(hint);
  return click;
}

// Dragging a device by its title moves it among the inserts; with Alt held at the drop it is copied.
struct DevDrag {
  ObjectId owner = kNoId;
  int slot = -1;
};
constexpr const char* kDevPayload = "GL_LIVEDEV";
constexpr const char* kRowPayload = "GL_LIVEPRM";

void dropOnInsert(App& a, ObjectId owner, int to) {
  if (!ImGui::BeginDragDropTarget()) return;
  if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kDevPayload)) {
    DevDrag dd;
    std::memcpy(&dd, pl->Data, sizeof dd);
    const Song& s = a.doc.song();
    const MixerTrack* mt = s.mixerTrack(owner);
    if (dd.owner == owner && mt && dd.slot >= 0 && dd.slot < kEffectSlots && to >= 0 && to < kEffectSlots) {
      if (ImGui::GetIO().KeyAlt) {  // a copy, in the first free slot, then moved to the drop
        int freeSlot = -1;
        for (int k = 0; k < kEffectSlots && freeSlot < 0; ++k)
          if (mt->slots[static_cast<size_t>(k)].plugin.type.empty()) freeSlot = k;
        if (freeSlot >= 0) {
          const PluginState copy = mt->slots[static_cast<size_t>(dd.slot)].plugin;
          a.doc.beginGroup("copy effect");
          if (a.doc.setSlotPlugin(owner, freeSlot, copy.type)) {
            setPluginState(a, PluginTarget{owner, freeSlot}, copy, "copy effect");
            if (to != freeSlot) {
              a.doc.moveSlot(owner, freeSlot, to);
              followMove(owner, freeSlot, to);
            }
          }
          a.doc.endGroup();
        } else {
          a.notify(i18n::tr("No free insert on this mixer track for the copy"), true);
        }
      } else if (dd.slot != to) {
        a.doc.moveSlot(owner, dd.slot, to);
        followMove(owner, dd.slot, to);
      }
    }
  }
  ImGui::EndDragDropTarget();
}

// K-112: the band's device clipboard: an effect's whole state (its type, parameters and data).
PluginState devClip;

void copyDevice(App& a, const Device& d) {
  if (d.target.isChannel()) return;
  if (const PluginState* st = d.target.state(a.doc.song())) devClip = *st;
}

// A copied effect goes right after `d` (after the instrument: the first insert) on the same mixer
// track, through the first free insert, as one undo step.
void pasteDevice(App& a, const Device& d) {
  if (devClip.type.empty()) return;
  const Song& s = a.doc.song();
  ObjectId owner = d.target.owner;
  int after = d.target.slot;
  if (d.target.isChannel()) {
    const Channel* c = s.channel(d.target.owner);
    if (!c || c->mixerTrack == kNoId) return;
    owner = c->mixerTrack;
    after = -1;
  }
  const MixerTrack* mt = s.mixerTrack(owner);
  if (!mt) return;
  int freeSlot = -1;
  for (int k = 0; k < kEffectSlots && freeSlot < 0; ++k)
    if (mt->slots[static_cast<size_t>(k)].plugin.type.empty()) freeSlot = k;
  if (freeSlot < 0) {
    a.notify(i18n::tr("No free insert on this mixer track for the pasted effect"), true);
    return;
  }
  const int to = freeSlot > after ? after + 1 : after;  // (erase-then-insert: past a freed earlier slot, one less)
  const PluginState copy = devClip;
  a.doc.beginGroup("paste effect");
  if (a.doc.setSlotPlugin(owner, freeSlot, copy.type)) {
    setPluginState(a, PluginTarget{owner, freeSlot}, copy, "paste effect");
    if (to != freeSlot && to >= 0) {
      a.doc.moveSlot(owner, freeSlot, to);
      followMove(owner, freeSlot, to);
    }
  }
  a.doc.endGroup();
}

// The device's menu (right-click on its title, or its menu button), grouped as the refs group a
// device's menu: cut, copy and paste; then removing and folding it and how its editor shows; then
// compare; then the plugin menu every window has (presets, state, replace, rename...).
void deviceMenu(App& a, BandPick& pick, const Device& d) {
  BandUi& u = bandUi();
  const uint64_t key = d.target.key();
  const bool folded = u.folded.count(key) > 0;
  const bool fx = !d.target.isChannel();
  if (ImGui::MenuItem(i18n::tr("Cut"), keys::label("rack.cut"), false, fx)) {
    copyDevice(a, d);
    a.doc.setSlotPlugin(d.target.owner, d.target.slot, "");
  }
  if (ImGui::MenuItem(i18n::tr("Copy"), keys::label("rack.copy"), false, fx)) copyDevice(a, d);
  if (ImGui::MenuItem(i18n::tr("Paste"), keys::label("rack.paste"), false, !devClip.type.empty())) pasteDevice(a, d);
  ImGui::Separator();
  if (fx && ImGui::MenuItem(i18n::tr("Delete"), "Del")) a.doc.setSlotPlugin(d.target.owner, d.target.slot, "");
  if (ImGui::MenuItem(folded ? i18n::tr("Unfold") : i18n::tr("Fold"))) {
    if (folded) u.folded.erase(key);
    else u.folded.insert(key);
  }
  if (!d.hosted && ImGui::MenuItem(i18n::tr("Show its whole editor above"), nullptr, u.expanded.count(key) > 0)) {
    if (!u.expanded.erase(key)) {
      u.expanded.insert(key);
      if (inWindow(a, pick, d)) toggleWindow(a, pick, d);
    }
  }
  if (ImGui::MenuItem(d.hosted ? i18n::tr("Plugin window") : i18n::tr("Open in a window"), nullptr, inWindow(a, pick, d))) toggleWindow(a, pick, d);
  if (d.hosted && ImGui::MenuItem(i18n::tr("Configure"), nullptr, u.configuring.count(key) > 0)) {
    if (!u.configuring.erase(key)) {
      u.configuring.insert(key);
      u.seen[key] = hostedLastTouched(d.target);
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(pui::compareOnB(d.target) ? i18n::tr("Compare: switch to A") : i18n::tr("Compare: switch to B"), "P")) pui::compareFlip(a, d.target);
  if (ImGui::MenuItem(i18n::tr("Compare: copy to the other side"))) pui::compareStore(a, d.target);
  ImGui::Separator();
  pluginWindowMenu(a, d.target.owner, d.target.slot);
}

}  // namespace

void devDeviceMenu(App& a) {
  const Song& s = a.doc.song();
  const Channel* ch = s.channel(a.selChannel);
  if (!ch && !s.channels.empty()) ch = &s.channels.front();
  if (!ch) return;
  Device d;
  d.target = PluginTarget{ch->id, -1};
  d.info = findPlugin(ch->plugin.type);
  d.hosted = isHostedType(ch->plugin.type);
  d.name = d.info ? d.info->name : ch->name;
  if (const MixerTrack* mt = s.mixerTrack(ch->mixerTrack))
    for (int k = 0; k < kEffectSlots; ++k) {
      const EffectSlot& sl = mt->slots[static_cast<size_t>(k)];
      if (sl.plugin.type.empty()) continue;
      d.target = PluginTarget{mt->id, k};
      d.info = findPlugin(sl.plugin.type);
      d.hosted = isHostedType(sl.plugin.type);
      d.name = d.info ? d.info->name : sl.plugin.type;
      d.enabled = sl.enabled;
      break;
    }
  static BandPick pick;
  deviceMenu(a, pick, d);
}

namespace {

// A built-in's factory presets, from its title bar.
void presetPopup(App& a, const Device& d) {
  const PluginState* st = d.target.state(a.doc.song());
  if (!st || !d.info) return;
  if (d.info->presets.empty()) ImGui::TextDisabled("%s", i18n::tr("No presets"));
  for (size_t i = 0; i < d.info->presets.size(); ++i)
    if (ImGui::MenuItem(d.info->presets[i].name.c_str())) {
      PluginState ns = presetState(*d.info, i);
      ns.data = st->data;
      ns.shown = st->shown;
      setPluginState(a, d.target, ns, "load preset");
      pui::presetLoaded(d.target, d.info->type, d.info->presets[i].name);
    }
}

// The installed plugin's parameter panel: Gridline's own rows (never the plugin's art).
void hostedRows(App& a, const Device& d, ImVec2 b0, ImVec2 b1) {
  const auto& P = pal();
  BandUi& u = bandUi();
  const uint64_t key = d.target.key();
  const PluginState* st = d.target.state(a.doc.song());
  std::vector<int> rows = panelParams(st, d.info);
  const bool configuring = u.configuring.count(key) > 0;
  const int touched = hostedLastTouched(d.target);
  if (configuring && touched >= 0 && touched != u.seen[key]) {  // a control touched in its window joins the panel
    u.seen[key] = touched;
    if (std::find(rows.begin(), rows.end(), touched) == rows.end()) {
      rows.push_back(touched);
      setShown(a, d.target, rows);
    }
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = b1.x - b0.x;
  if (configuring) {
    dl->AddRect(ImVec2(b0.x - 2, b0.y - 2), ImVec2(b1.x + 2, b1.y + 2), theme::withAlpha(P.hot, 0.8f), theme::kRadiusSm);
    dl->AddText(a.fontRegular, theme::kFontLabel, b0, P.hot, i18n::tr("Touch a control in its window to add it"));
    b0.y += 16;
  }
  // Outside Configure the control touched last shows too, for now, after the chosen ones.
  const bool temp = !configuring && touched >= 0 && std::find(rows.begin(), rows.end(), touched) == rows.end() && d.info &&
                    touched < static_cast<int>(d.info->params.size());
  if (rows.empty() && !temp) {
    // K-112c: an empty panel says how to fill it, centred in the card as the refs show it.
    const bool any = d.info && !d.info->params.empty();
    const char* msg = configuring ? "" : any ? i18n::tr("Click Configure, then move a control in the plugin's window to add it here")
                                             : i18n::tr("No parameters");
    if (*msg) {
      const float fs = theme::kFontLabel, ww = std::max(40.0f, w - 16);
      const ImVec2 ts = a.fontRegular->CalcTextSizeA(fs, 1e9f, ww, msg);
      const ImVec4 clip(b0.x, b0.y, b1.x, b1.y);
      // centred line by line: each wrapped line placed on its own
      float y = std::round(b0.y + std::max(2.0f, (b1.y - b0.y - ts.y) * 0.5f));
      const char* t = msg;
      const char* end = msg + std::strlen(msg);
      while (t < end) {
        const char* e = a.fontRegular->CalcWordWrapPosition(fs, t, end, ww);
        if (e == t) e = t + 1;
        const ImVec2 ls = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, t, e);
        dl->AddText(a.fontRegular, fs, ImVec2(std::round(b0.x + (w - ls.x) * 0.5f), y), P.textDim, t, e, 0, &clip);
        y += fs + 3;
        t = e;
        while (t < end && *t == ' ') ++t;
      }
    }
    return;
  }
  ImGui::SetCursorScreenPos(b0);
  ImGui::BeginChild("##rows", ImVec2(w, std::max(20.0f, b1.y - b0.y)), ImGuiChildFlags_None);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  const float gripW = configuring ? 12.0f : 0.0f, rw = ImGui::GetContentRegionAvail().x - gripW - 2;
  int moveFrom = -1, moveTo = -1;
  for (size_t r = 0; r < rows.size() + (temp ? 1 : 0); ++r) {
    const bool isTemp = r == rows.size();
    const int k = isTemp ? touched : rows[r];
    ImGui::PushID(static_cast<int>(r));
    const ImVec2 rp = ImGui::GetCursorScreenPos();
    if (configuring) {  // a grip to drag the entry (and select it: Delete removes it)
      ImGui::InvisibleButton("##grip", ImVec2(gripW, 22));
      const bool sel = u.rowSel == key && u.rowSelParam == k;
      if (ImGui::IsItemClicked()) u.rowSel = key, u.rowSelParam = k;
      ui::drawIcon(ImGui::GetWindowDrawList(), ui::Icon::GripDots, ImVec2(rp.x + gripW * 0.5f, rp.y + 11), 8.0f, sel ? P.hot : P.textFaint);
      ui::Hint("Drag to reorder; click, then Delete, to remove it from the panel");
      if (ImGui::BeginDragDropSource()) {
        const int from = static_cast<int>(r);
        ImGui::SetDragDropPayload(kRowPayload, &from, sizeof from);
        ImGui::TextUnformatted(d.info ? d.info->params[static_cast<size_t>(k)].name.c_str() : "");
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kRowPayload)) {
          std::memcpy(&moveFrom, pl->Data, sizeof moveFrom);
          moveTo = static_cast<int>(r);
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::SetCursorScreenPos(ImVec2(rp.x + gripW + 2, rp.y));
    }
    hostedParamRow(a, d.target, k, rw);
    if (isTemp) ImGui::GetWindowDrawList()->AddRect(ImVec2(rp.x + gripW + 2, rp.y), ImVec2(rp.x + gripW + 2 + rw, rp.y + 22), theme::withAlpha(P.textFaint, 0.7f),
                                                    4.0f);
    ImGui::PopID();
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo && moveFrom < static_cast<int>(rows.size()) && moveTo < static_cast<int>(rows.size())) {
    const int v = rows[static_cast<size_t>(moveFrom)];
    rows.erase(rows.begin() + moveFrom);
    rows.insert(rows.begin() + moveTo, v);
    setShown(a, d.target, rows);
  }
}

// Draws a device card at `p`, `h` tall: title bar, then its picture and main controls (a built-in)
// or its parameter panel (an installed plugin). A folded device is a narrow strip with its name.
void drawCard(App& a, ImDrawList* dl, BandPick& pick, const Device& d, ImVec2 p, float h) {
  const auto& P = pal();
  BandUi& u = bandUi();
  const uint64_t key = d.target.key();
  const float w = cardWidth(d);
  const ImVec2 p1(p.x + w, p.y + h);
  const bool selected = u.selected == key, folded = u.folded.count(key) > 0, expanded = u.expanded.count(key) > 0;
  const bool canBypass = !d.target.isChannel();
  dl->AddRectFilled(p, p1, P.surface800, theme::kRadiusLg);
  if (folded) {  // a strip: lamp, then the name reading upwards; a double-click (or click) unfolds it
    dl->AddRectFilled(p, p1, P.surface700, theme::kRadiusLg);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 2, p.y + 2));
    if (ImGui::InvisibleButton("##lamp", ImVec2(w - 4, 16)) && canBypass)
      a.doc.setParam(slotEnableParam(d.target.owner, d.target.slot), d.enabled ? 0.0 : 1.0, false);
    ui::Hint(canBypass ? "Turn the effect on or off (the mixer slot's switch)" : "The track's instrument");
    deviceLamp(dl, ImVec2(p.x + w * 0.5f, p.y + 10), d.enabled);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 20));
    ImGui::InvisibleButton("##strip", ImVec2(w, std::max(10.0f, h - 20)));
    if (ImGui::IsItemClicked()) u.selected = key;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) u.folded.erase(key);
    ui::Hint("Double-click to unfold the device");
    if (ImGui::BeginPopupContextItem("##menu")) {
      deviceMenu(a, pick, d);
      ImGui::EndPopup();
    }
    if (!d.target.isChannel()) dropOnInsert(a, d.target.owner, d.target.slot);
    const float fs = theme::kFontCaption;
    const std::string nm = fitText(a.fontBold, fs, d.name, h - 32);
    upwardText(dl, a.fontBold, fs, ImVec2(std::round(p.x + (w - fs) * 0.5f), p1.y - 8), d.enabled ? P.text : P.textDim, nm);
    if (selected) dl->AddRect(p, p1, P.hot, theme::kRadiusLg, 1.5f, 0);
    return;
  }
  dl->AddRectFilled(p, ImVec2(p1.x, p.y + kCardTitleH), selected ? theme::mix(P.surface700, P.surface600, 0.6f) : P.surface700, theme::kRadiusLg,
                    ImDrawFlags_RoundCornersTop);
  // Title bar, left to right: the activator, the expand arrow (a built-in: its whole editor above the
  // band) or the panel triangle (an installed plugin: its parameter panel), the name; at the right
  // the window button, Configure (installed plugins) or presets (built-ins), and the menu.
  const float ty0 = p.y + 2;
  ImGui::SetCursorScreenPos(ImVec2(p.x + 2, ty0));
  if (ImGui::InvisibleButton("##lamp", ImVec2(14, kCardTitleH - 4)) && canBypass)
    a.doc.setParam(slotEnableParam(d.target.owner, d.target.slot), d.enabled ? 0.0 : 1.0, false);
  ui::Hint(canBypass ? "Turn the effect on or off (the mixer slot's switch)" : "The track's instrument");
  deviceLamp(dl, ImVec2(p.x + 9, p.y + kCardTitleH * 0.5f), d.enabled);
  if (d.hosted) {
    const bool open = !u.shut.count(key);
    if (titleIcon(dl, "##panel", open ? ui::Icon::ChevronDown : ui::Icon::Right, ImVec2(p.x + 16, ty0), 14, false,
                  open ? "Fold the parameter panel away" : "Show the parameter panel")) {
      if (open) u.shut.insert(key);
      else u.shut.erase(key);
    }
  } else {
    const bool hit = titleIcon(dl, "##expand", expanded ? ui::Icon::ChevronDown : ui::Icon::Right, ImVec2(p.x + 16, ty0), 14, expanded,
                               expanded ? "Close the whole editor above the band" : "Show the device's whole editor above the band");
    devNoteRect("devexpand");
    if (hit && !u.expanded.erase(key)) {
      u.expanded.insert(key);
      if (inWindow(a, pick, d)) toggleWindow(a, pick, d);
    }
  }
  // Right end: menu, then (to its left) the window button and Configure / presets. K-112: an installed
  // plugin's window button sits left of its name, after the panel triangle, and Configure is a word
  // at the right end (an icon when the card is too narrow for it), as the refs draw a plugin's card.
  const float menuX = p1.x - 18, winX = d.hosted ? p.x + 31 : menuX - 16;
  float thirdX = d.hosted ? menuX - 16 : winX - 16;
  if (titleIcon(dl, "##menubtn", ui::Icon::More, ImVec2(menuX, ty0), 15, false, "The device's menu")) ImGui::OpenPopup("##menu");
  if (titleIcon(dl, "##window", ui::Icon::WindowPopout, ImVec2(winX, ty0), 15, inWindow(a, pick, d),
                d.hosted ? "Show or hide the plugin's own window" : "Open the device's editor in a window"))
    toggleWindow(a, pick, d);
  {  // the first built-in card's window button (an sdlat target)
    static int noted = -1;
    if (!d.hosted && noted != ImGui::GetFrameCount()) devNoteRect("devwindow"), noted = ImGui::GetFrameCount();
  }
  if (d.hosted) {
    const bool conf = u.configuring.count(key) > 0;
    const char* cw = i18n::tr("Configure");
    const float cfs = theme::kFontCaption;
    const float cwW = std::round(a.fontRegular->CalcTextSizeA(cfs, 1e9f, 0, cw).x + 12);
    const bool word = menuX - 4 - cwW - (p.x + 48) >= 48;  // room for the word and a readable name
    bool hit = false;
    const char* chint = conf ? "Done: stop adding controls to the panel" : "Configure: controls touched in the plugin's window join the panel";
    if (word) {
      thirdX = menuX - 4 - cwW;
      const ImVec2 c0(thirdX, ty0 + 1), c1(thirdX + cwW, ty0 + kCardTitleH - 5);
      ImGui::SetCursorScreenPos(c0);
      hit = ImGui::InvisibleButton("##configure", ImVec2(c1.x - c0.x, c1.y - c0.y));
      const bool hov = ImGui::IsItemHovered();
      dl->AddRectFilled(c0, c1, conf ? P.hot : hov ? P.surface700 : P.surface800, 2.0f);  // K-112c (round 26): one hover step, as the other Live buttons
      if (!conf) dl->AddRect(c0, c1, P.dark ? P.lineStrong : P.line, 2.0f);
      theme::centreText(dl, a.fontRegular, cfs, c0, c1, conf ? theme::contrastText(P.hot) : P.text, cw);
      ui::Hint(chint);
    } else {
      hit = titleIcon(dl, "##configure", ui::Icon::Settings, ImVec2(thirdX, ty0), 15, conf, chint);
    }
    if (hit) {
      if (!u.configuring.erase(key)) {
        u.configuring.insert(key);
        u.seen[key] = hostedLastTouched(d.target);
        u.shut.erase(key);
        if (!pluginWindowShown(a, d.target)) togglePluginWindow(a, d.target);
      }
    }
  } else if (titleIcon(dl, "##presets", ui::Icon::ListBullets, ImVec2(thirdX, ty0), 15, false, "Presets")) {
    ImGui::OpenPopup("##presetlist");
  }
  if (ImGui::BeginPopup("##presetlist")) {
    presetPopup(a, d);
    ImGui::EndPopup();
  }
  // K-112c: hot-swap (another device in its place) and save (its preset to a file), left of the
  // right-end buttons as the refs draw them, while the card still leaves the name room.
  if (thirdX - 32 - (d.hosted ? p.x + 48 : p.x + 32) >= 56) {
    thirdX -= 32;
    if (titleIcon(dl, "##swap", ui::Icon::SwapArrows, ImVec2(thirdX, ty0), 15, false, "Hot-swap: put another device in its place"))
      ImGui::OpenPopup("##swaplist");
    devNoteRect("devswap");
    if (titleIcon(dl, "##save", ui::Icon::Save, ImVec2(thirdX + 16, ty0), 15, false, "Save its settings as a preset file"))
      savePresetDialog(a, d.target.owner, d.target.slot);
    devNoteRect("devsave");
  }
  if (ImGui::BeginPopup("##swaplist")) {
    swapDeviceItems(a, d.target.owner, d.target.slot);
    ImGui::EndPopup();
  }
  // The name: a click selects the device, a double-click folds it; dragged, it moves (Alt: copies).
  const float nameX = d.hosted ? p.x + 48 : p.x + 32, nameW = std::max(10.0f, thirdX - 2 - nameX);
  ImGui::SetCursorScreenPos(ImVec2(nameX, p.y + 1));
  ImGui::InvisibleButton("##title", ImVec2(nameW, kCardTitleH - 2));
  if (ImGui::IsItemClicked()) u.selected = key;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) u.folded.insert(key);
  ui::Hint(d.target.isChannel() ? "Click to select, double-click to fold"
                                : "Click to select (Delete removes it), double-click to fold; drag to move it, with Alt to copy it");
  if (!d.target.isChannel() && ImGui::BeginDragDropSource()) {
    const DevDrag dd{d.target.owner, d.target.slot};
    ImGui::SetDragDropPayload(kDevPayload, &dd, sizeof dd);
    ImGui::TextUnformatted(d.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (!d.target.isChannel()) dropOnInsert(a, d.target.owner, d.target.slot);
  if (ImGui::BeginPopupContextItem("##menu")) {  // also the menu button's popup (same id)
    deviceMenu(a, pick, d);
    ImGui::EndPopup();
  }
  const float fs = theme::kFontCaption + 1;
  const float ty = theme::textTop(a.fontBold, fs, p.y, kCardTitleH);
  dl->PushClipRect(ImVec2(nameX, p.y), ImVec2(nameX + nameW, p.y + kCardTitleH), true);
  dl->AddText(a.fontBold, fs, ImVec2(nameX + 1, ty), d.enabled ? P.text : P.textDim, d.name.c_str());
  dl->PopClipRect();
  if (selected) dl->AddRect(p, p1, P.hot, theme::kRadiusLg, 1.5f, 0);
  const ImVec2 b0(p.x + kCardPad, p.y + kCardTitleH + 4);
  if (d.hosted) {
    if (!u.shut.count(key)) hostedRows(a, d, b0, ImVec2(p1.x - kCardPad, p1.y - 6));
    else dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(b0.x, b0.y + 2), P.textFaint, d.sub.c_str());
    return;
  }
  float kx = b0.x;
  const float cell = kKnobCellW, cellH = bandKnobH();
  float ky = b0.y;
  if (d.above && d.visW > 0) {  // the picture across the card's top, the knobs in a row under it
    const ImVec2 v0(b0.x, b0.y + 2), v1(p1.x - kCardPad, std::max(b0.y + 24, p1.y - 6 - cellH));
    if (devview::draw(a, d.target, v0, v1)) {
      u.expanded.insert(key);
      if (inWindow(a, pick, d)) toggleWindow(a, pick, d);
    }
    ky = v1.y + 4;
    kx = b0.x + std::max(0.0f, (p1.x - kCardPad - b0.x - static_cast<float>(d.cols) * cell) * 0.5f);
  } else if (d.visW > 0) {  // the device's picture, then its main knobs beside it
    const ImVec2 v0(b0.x, b0.y + 2), v1(b0.x + d.visW, p1.y - 8);
    if (devview::draw(a, d.target, v0, v1)) {  // double-clicked where no handle took it
      u.expanded.insert(key);
      if (inWindow(a, pick, d)) toggleWindow(a, pick, d);
    }
    kx += d.visW + 8;
  }
  for (int k = 0; k < d.knobs; ++k) {
    const int r = k / std::max(1, d.cols), col = k % std::max(1, d.cols);
    ImGui::PushID(k);
    bandKnob(a, d.target, d.param(k), ImVec2(kx + static_cast<float>(col) * cell, ky + static_cast<float>(r) * (cellH + 2)), d.label(k), cell);
    ImGui::PopID();
  }
}

// Which knobs a device shows: its main controls (DeviceViews), else its first parameters (the main
// ones come first in most plugins), as many as fit.
void pickKnobs(App& a, Device& d, float bandH) {
  if (!d.hosted) d.mains = devview::mainParams(a, d.target);
  const int n = !d.mains.empty() ? static_cast<int>(d.mains.size()) : d.info ? static_cast<int>(d.info->params.size()) : 0;
  const float cellH = bandKnobH() + 2;
  d.above = d.visW > 0 && pictureAbove(d.info);
  const int rows = d.above ? 1 : std::max(1, static_cast<int>((bandH - kCardTitleH - 4) / cellH));
  const int maxCols = d.above ? 4 : d.visW > 0 ? (d.target.isChannel() ? 3 : 2) : d.target.isChannel() ? 5 : 4;
  d.knobs = std::min(n, rows * maxCols);
  d.cols = std::min(maxCols, std::max(1, (d.knobs + rows - 1) / rows));
}

std::string slotLabel(int slot) { return std::string(i18n::tr("Insert")) + " " + std::to_string(slot + 1); }

// The whole editors of the expanded devices, side by side in the panel above the chain.
void expandedPanel(App& a, const std::vector<Device>& devs, ImVec2 size) {
  const auto& P = pal();
  BandUi& u = bandUi();
  ImGui::BeginChild("##expanded", size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = ImGui::GetContentRegionAvail().y;
  const float panelW = ImGui::GetContentRegionAvail().x;
  // One editor open fills the panel's width (the main view above has made room for it); several
  // sit side by side at their own widths. The size menu overrides either.
  int open = 0;
  for (const Device& d : devs) open += !d.hosted && u.expanded.count(d.target.key()) ? 1 : 0;
  const float byDefault = open == 1 ? -1.0f : 1.0f;
  bool first = true;
  for (const Device& d : devs) {
    if (d.hosted || !u.expanded.count(d.target.key())) continue;
    if (!first) ImGui::SameLine(0, 8);
    first = false;
    ImGui::PushID(static_cast<int>(d.target.key() & 0x7fffffff));
    // Its width: its own (the editor's design width) times the size chosen in its size menu, or the
    // panel's whole width; never narrower than 360 px.
    const float own = std::max(360.0f, d.info && d.info->editorWidth > 0 ? d.info->editorWidth : 600.0f);
    const auto sz = u.edSize.find(d.target.key());
    const float factor = sz != u.edSize.end() ? sz->second : byDefault;
    const float room = std::max(360.0f, panelW - 8);
    const float w = factor < 0 ? room : factor == 1.0f ? std::min(own, room) : std::max(360.0f, own * factor);
    ImGui::BeginGroup();
    const ImVec2 t0 = ImGui::GetCursorScreenPos();
    // K-112c: the editor's frame as the refs draw an expanded device: square, a title strip in
    // regular weight flush on the body, the whole framed by a dark 1 px rim.
    const float edH = std::max(40.0f, h - kCardTitleH - 4) + kCardTitleH;
    dl->AddRectFilled(t0, ImVec2(t0.x + w, t0.y + kCardTitleH), P.surface700, 0.0f);
    dl->AddLine(ImVec2(t0.x, t0.y + kCardTitleH - 0.5f), ImVec2(t0.x + w, t0.y + kCardTitleH - 0.5f), IM_COL32(16, 18, 22, 170));
    deviceLamp(dl, ImVec2(t0.x + 9, t0.y + kCardTitleH * 0.5f), d.enabled);
    dl->AddText(a.fontRegular, theme::kFontCaption + 1, ImVec2(t0.x + 18, theme::textTop(a.fontRegular, theme::kFontCaption + 1, t0.y, kCardTitleH)), P.text, d.name.c_str());
    if (titleIcon(dl, "##close", ui::Icon::Close, ImVec2(t0.x + w - 20, t0.y + 2), 16, false, "Close the whole editor (the device stays in the chain)"))
      u.expanded.erase(d.target.key());
    if (titleIcon(dl, "##size", ui::Icon::Zoom, ImVec2(t0.x + w - 38, t0.y + 2), 16, factor != byDefault, "Editor size: how wide the editor is above the band"))
      ImGui::OpenPopup("##sizemenu");
    if (ImGui::BeginPopup("##sizemenu")) {
      ImGui::TextDisabled("%s", i18n::tr("Editor size"));
      constexpr float kSizes[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
      for (float f : kSizes) {
        char lbl[32];
        std::snprintf(lbl, sizeof lbl, "%d %%", static_cast<int>(std::lround(f * 100)));
        if (ImGui::MenuItem(lbl, nullptr, factor == f)) {
          if (f == byDefault) u.edSize.erase(d.target.key());
          else u.edSize[d.target.key()] = f;
        }
      }
      if (ImGui::MenuItem(i18n::tr("The panel's whole width"), nullptr, factor < 0)) {
        if (byDefault < 0) u.edSize.erase(d.target.key());
        else u.edSize[d.target.key()] = -1.0f;
      }
      ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(t0.x, t0.y + kCardTitleH));
    ImGui::BeginChild("##body", ImVec2(w, edH - kCardTitleH), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    if (d.target.isChannel()) {
      drawChannelSettingsBody(a, d.target.owner, BodyFrame{w - 8, false, "Device chain"});
    } else if (packs::showInLoupePanel(a, d.target)) {  // the pinned Loupe panel's session: the band draws the panel's body
      drawLoupeBody(a, w - 8, std::max(ImGui::GetContentRegionAvail().y, 120.0f));
      hostbody::claim(hostbody::Body::Loupe, "Device chain");
    } else {
      drawEffectEditorBody(a, d.target.owner, d.target.slot, BodyFrame{w - 8, false, "Device chain"});
    }
    ImGui::EndChild();
    ImGui::EndGroup();
    dl->AddRect(ImVec2(t0.x + 0.5f, t0.y + 0.5f), ImVec2(t0.x + w - 0.5f, t0.y + edH - 0.5f), IM_COL32(16, 18, 22, 200));
    ImGui::PopID();
  }
  if (first) ImGui::Dummy(ImVec2(1, 1));
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().KeyShift && ImGui::GetIO().MouseWheel != 0)
    ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * 60.0f);
  ImGui::EndChild();
}

}  // namespace

void deviceBand(App& a, BandPick& pick) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  BandUi& u = bandUi();
  // A floating editor closed from its own window: the band hosts the device again.
  if (pick.fxWindow && a.fxEditor.first == kNoId) pick.fxWindow = false;
  if (pick.insWindow && a.settingsChannel == kNoId) pick.insWindow = false;
  const ObjectId chId = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
  const Channel* ch = s.channel(chId);
  const MixerTrack* mt = ch ? s.mixerTrack(ch->mixerTrack) : s.mixerTrack(a.selMixer);
  // An effect opened on another mixer track (from the mixer): the band shows that track's inserts.
  if (pick.kind == 1 && pick.fxTrack != kNoId && (!mt || mt->id != pick.fxTrack))
    if (const MixerTrack* fxt = s.mixerTrack(pick.fxTrack)) {
      mt = fxt;
      if (ch && ch->mixerTrack != fxt->id) ch = nullptr;
    }
  if (!ch && !mt) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 60.0f)), "No device",
                   "Select a track: its instrument and the inserts of its mixer track show here");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 60.0f)));
    return;
  }
  // A device opened elsewhere (the mixer, the rack, View) is selected and shows its whole editor
  // above the band, unless it was asked for in a window.
  if (pick.open && pick.kind != 0 && pick.kind != 1) pick.open = false;  // the master chain has its own window
  if (pick.open) {
    pick.open = false;
    const PluginTarget t = pick.kind == 0 && ch ? PluginTarget{ch->id, -1} : PluginTarget{pick.fxTrack, pick.slot};
    const bool floating = t.isChannel() ? pick.insWindow : pick.fxWindow;
    const PluginState* st = t.state(s);
    if (st && !isHostedType(st->type)) {
      u.selected = t.key();
      u.folded.erase(t.key());
      if (!floating) u.expanded.insert(t.key());
    }
  }
  // What the chain is (the track, then where the inserts live): the bracket label's hint.
  std::string chainHint = selectedTrackName(a);
  if (chainHint.empty() && ch) chainHint = ch->name;
  {
    const size_t mtIndex = mt ? static_cast<size_t>(mt - s.mixer.data()) : 0;
    chainHint += std::string(chainHint.empty() ? "" : ": ") +
                 (mt && mtIndex > 0 ? std::string(i18n::tr("chain: the instrument, then the inserts of mixer track")) + " " + std::to_string(mtIndex) +
                                          " (" + i18n::tr("the same slots the mixer shows") + ")"
                                    : std::string(i18n::tr("chain: the instrument, then the master's inserts")));
  }
  std::vector<Device> devs;
  if (ch) {
    Device d;
    d.target = PluginTarget{ch->id, -1};
    d.info = findPlugin(ch->plugin.type);
    d.hosted = isHostedType(ch->plugin.type);
    d.name = d.info ? d.info->name : ch->name;
    d.sub = i18n::tr("instrument");
    d.visW = d.hosted ? 0.0f : devview::width(a, d.target);
    devs.push_back(d);
  }
  int firstFree = -1;
  if (mt)
    for (int k = 0; k < kEffectSlots; ++k) {
      const EffectSlot& sl = mt->slots[static_cast<size_t>(k)];
      const PluginInfo* fx = findPlugin(sl.plugin.type);
      const bool hosted = isHostedType(sl.plugin.type);
      if (!fx && !hosted) {
        if (firstFree < 0) firstFree = k;
        continue;
      }
      Device d;
      d.target = PluginTarget{mt->id, k};
      d.info = fx;
      d.hosted = hosted;
      d.name = fx ? fx->name : sl.plugin.type;
      d.sub = slotLabel(k);
      d.enabled = sl.enabled;
      d.visW = hosted ? 0.0f : devview::width(a, d.target);
      devs.push_back(d);
    }
  // Forget devices that are gone (or the panel would open for nothing).
  {
    std::set<uint64_t> live;
    for (const Device& d : devs) live.insert(d.target.key());
    for (auto it = u.expanded.begin(); it != u.expanded.end();) it = live.count(*it) ? std::next(it) : u.expanded.erase(it);
    if (!live.count(u.selected)) u.selected = 0;
  }
  bool anyExpanded = false;
  for (const Device& d : devs) anyExpanded = anyExpanded || (!d.hosted && u.expanded.count(d.target.key()));

  // The panel above the chain: the band's region grows by its height while something is expanded.
  const float availH = ImGui::GetContentRegionAvail().y;
  const float vpH = ImGui::GetMainViewport()->Size.y;
  // The plain height is measured only once the band has shrunk back (a growth lasts a frame or two
  // after the last ask), so a device opened just after another closed still finds its room.
  const int frameNow = ImGui::GetFrameCount();
  if (u.plainH <= 0 || (!anyExpanded && frameNow - u.boostFrame > 3)) u.plainH = std::max(80.0f, availH);
  if (u.expandH <= 0) u.expandH = std::floor(vpH * 0.45f);
  u.expandH = std::clamp(u.expandH, 120.0f, std::max(120.0f, vpH * 0.6f - u.plainH));
  float chainH = availH;
  if (anyExpanded) {
    layoutui::setBandBoost("live.device", u.expandH + 6);
    u.boostFrame = frameNow;
    // (The first frame the band has not grown yet: the panel shows from the next one.)
    const float panelH = availH - u.plainH - 6;
    if (panelH >= 60) {
      // The panel's top edge drags its height (up: taller).
      const ImVec2 s0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("##panelsize", ImVec2(ImGui::GetContentRegionAvail().x, 5));
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsItemActive()) u.expandH -= ImGui::GetIO().MouseDelta.y;
      ui::Hint("Drag to make the editors above the band taller or shorter");
      ImGui::GetWindowDrawList()->AddLine(ImVec2(s0.x, s0.y + 2), ImVec2(s0.x + ImGui::GetContentRegionAvail().x, s0.y + 2),
                                          ImGui::IsItemActive() ? P.hot : P.line);
      expandedPanel(a, devs, ImVec2(0, panelH - 5));
      chainH = ImGui::GetContentRegionAvail().y;
    }
  }
  const float bandH = std::max(80.0f, chainH - 2);
  // The knobs fit the shortest card: the inserts' (under their bracket label).
  const float knobH = bandH - 16.0f;
  for (Device& d : devs) pickKnobs(a, d, knobH);

  // No scroll bar (its height would cost a row of knobs): the wheel scrolls the devices sideways.
  ImGui::BeginChild("##chain", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = ImGui::GetContentRegionAvail().y;
  const float chainW = ImGui::GetContentRegionAvail().x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float x0 = p.x;
  float bracket0 = -1;
  // K-112c (round 26): with a bracket in the band every card starts under its top line, so the
  // instrument's title row lines up with the inserts' (it sat 4 px higher).
  const bool bracketed = std::any_of(devs.begin(), devs.end(), [](const Device& x) { return !x.target.isChannel(); });
  for (size_t i = 0; i < devs.size(); ++i) {
    const Device& d = devs[i];
    if (!d.target.isChannel() && bracket0 < 0) {
      // The inserts sit inside a bracket: they are the mixer track's own slots.
      p.x += 6;
      bracket0 = p.x;
      p.x += 6;
    } else if (i > 0) {
      p.x += 8;
    }
    ImGui::PushID(static_cast<int>(i));
    drawCard(a, dl, pick, d, ImVec2(p.x, p.y + (bracketed ? 4.0f : 0.0f)), h - (bracket0 >= 0 ? 16.0f : bracketed ? 4.0f : 0.0f));
    p.x += cardWidth(d);
    ImGui::PopID();
    if (d.target.isChannel() && i + 1 < devs.size()) {  // signal flow arrow into the inserts
      ui::drawIconFit(dl, ui::Icon::Play, ImVec2(p.x + 6, p.y + h * 0.5f - 5), ImVec2(p.x + 14, p.y + h * 0.5f + 5), P.textFaint);
      p.x += 12;
    }
  }
  // The drop zone: the next free insert of the mixer track (a device dragged here moves to it). It
  // takes the room left before Out, so the chain always ends in a place to drop the next device.
  if (mt && firstFree >= 0) {
    if (bracket0 < 0) {
      p.x += 6;
      bracket0 = p.x;
      p.x += 6;
    } else {
      p.x += 8;
    }
    const float outW = 12.0f + 200.0f + 14.0f;  // the meter, Out and the bracket's end
    const float zw = std::max(120.0f, x0 + chainW - p.x - outW);
    const ImVec2 z0(p.x, p.y + 4), z1(p.x + zw, p.y + h - 12);
    ImGui::SetCursorScreenPos(z0);
    ImGui::InvisibleButton("##dropzone", ImVec2(z1.x - z0.x, z1.y - z0.y));
    {  // past the last device: a move goes to the end of the used inserts
      int lastUsed = -1;
      for (int k = 0; k < kEffectSlots; ++k)
        if (!mt->slots[static_cast<size_t>(k)].plugin.type.empty()) lastUsed = k;
      dropOnInsert(a, mt->id, std::max(0, lastUsed));
    }
    const bool dragging = ImGui::GetDragDropPayload() != nullptr;
    dl->AddRect(z0, z1, theme::withAlpha(dragging ? P.hot : P.textFaint, dragging ? 0.8f : 0.35f), theme::kRadiusLg, 1.0f, 0);
    const std::string dz = std::string(i18n::tr("Drop effects here")) + "\n" + slotLabel(firstFree);
    const ImVec2 ts = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, dz.c_str());
    // K-112c (round 33): centred in the part of the zone in view (the band scrolls), and only when it fits
    // there whole: it was centred on the whole zone and cut by the band's edge ("Drop ef").
    const float vx0 = std::max(z0.x, dl->GetClipRectMin().x + 20), vx1 = std::min(z1.x, dl->GetClipRectMax().x - 20);
    if (vx1 - vx0 >= ts.x + 16) {  // each line centred on its own
      float ly = std::round((z0.y + z1.y - ts.y) * 0.5f);
      for (size_t b = 0; b <= dz.size();) {
        const size_t e = std::min(dz.find('\n', b), dz.size());
        const float lw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, dz.c_str() + b, dz.c_str() + e).x;
        dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round((vx0 + vx1 - lw) * 0.5f), ly), P.textFaint, dz.c_str() + b, dz.c_str() + e);
        ly += ts.y / static_cast<float>(1 + std::count(dz.begin(), dz.end(), '\n'));
        b = e + 1;
      }
    }
    p.x = z1.x;
  }
  if (bracket0 >= 0 && mt) {
    // Dashed bracket around the inserts, named after the mixer track.
    const float bx0 = bracket0, bx1 = p.x + 6, by0 = p.y, by1 = p.y + h - 4;
    const ImU32 bc = theme::withAlpha(P.hot, 0.7f);
    for (float x = bx0; x < bx1; x += 7) {
      dl->AddLine(ImVec2(x, by0), ImVec2(std::min(x + 4, bx1), by0), bc);
      dl->AddLine(ImVec2(x, by1), ImVec2(std::min(x + 4, bx1), by1), bc);
    }
    for (float y = by0; y < by1; y += 7) {
      dl->AddLine(ImVec2(bx0, y), ImVec2(bx0, std::min(y + 4, by1)), bc);
      dl->AddLine(ImVec2(bx1, y), ImVec2(bx1, std::min(y + 4, by1)), bc);
    }
    const size_t mtIndex = static_cast<size_t>(mt - s.mixer.data());
    const std::string lbl = (mtIndex == 0 ? std::string(i18n::tr("Master")) : std::string(i18n::tr("Mixer track")) + " " + std::to_string(mtIndex)) +
                            " \xC2\xB7 " + i18n::tr("inserts");
    const ImVec2 ls = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl.c_str());
    dl->AddRectFilled(ImVec2(bx0 + 10, by1 - ls.y * 0.5f - 1), ImVec2(bx0 + 16 + ls.x, by1 + ls.y * 0.5f), P.surface900);
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(bx0 + 13, by1 - ls.y * 0.5f - 1), bc, lbl.c_str());
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(bx0 + 10, by1 - ls.y * 0.5f - 1), ImVec2(bx0 + 16 + ls.x, by1 + ls.y * 0.5f)))
      a.setHint(chainHint);
    p.x = bx1 + 8;
  }
  // Out: where the track goes, after a level meter of the chain's output.
  if (mt) {
    const mixmeters::Reading r = mixmeters::get(a, mt->id);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 4));
    ui::Meter(ImGui::GetID("##outmeter"), r.peakL, r.peakR, ImVec2(6, h - 12));
    ui::Hint("The chain's output level (the mixer track's meter)");
    p.x += 12;
    const float ow = 200.0f;
    const ImVec2 o0(p.x, p.y), o1(p.x + ow, p.y + h - 4);
    dl->AddRectFilled(o0, o1, P.surface800, theme::kRadiusLg);
    dl->AddText(a.fontBold, theme::kFontCaption, ImVec2(o0.x + 8, o0.y + 6), P.text, i18n::tr("Out"));
    std::string to = i18n::tr("to Main");
    for (const Send& sd : mt->sends)
      if (const MixerTrack* dst = s.mixerTrack(sd.to); dst && dst != &s.mixer[0]) {
        char lv[16];
        // K-112: no-break spaces keep a level with its % sign and "send" with the track's name when the line wraps.
        std::snprintf(lv, sizeof lv, "\xC2\xA0%d\xC2\xA0%%", static_cast<int>(std::lround(sd.level * 100)));
        to += std::string(" \xC2\xB7 ") + i18n::tr("send") + "\xC2\xA0" + dst->name + lv;
      }
    const ImVec4 clip(o0.x + 8, o0.y, o1.x - 8, o1.y - 4);
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(o0.x + 8, o0.y + 24), P.textDim, to.c_str(), nullptr, ow - 16, &clip);
    // The note goes under the routing line however many lines that wraps to.
    const float toH = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, ow - 16, to.c_str()).y;
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(o0.x + 8, std::max(o0.y + 52, std::round(o0.y + 24 + toH + 10))), P.textFaint,
                i18n::tr("Adding, moving or turning off a device here edits the mixer track: the mixer and every other layout show the change."),
                nullptr, ow - 16, &clip);
    p.x = o1.x;
  }
  ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
  ImGui::Dummy(ImVec2(p.x - x0 + 4, h - 4));
  // Keys while the band has focus: Delete removes the selected insert (or, in Configure, the
  // selected panel entry); P switches the selected device's A/B compare.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool plain = !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !io.KeyShift;
    const Device* sel = nullptr;
    for (const Device& d : devs)
      if (d.target.key() == u.selected) sel = &d;
    if (plain && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
      const Device* rowDev = nullptr;
      for (const Device& d : devs)
        if (d.target.key() == u.rowSel && u.configuring.count(u.rowSel)) rowDev = &d;
      if (rowDev && u.rowSelParam >= 0) {
        std::vector<int> rows = panelParams(rowDev->target.state(s), rowDev->info);
        rows.erase(std::remove(rows.begin(), rows.end(), u.rowSelParam), rows.end());
        if (rows.empty()) rows.push_back(-1);  // an emptied panel stays empty (no list at all means every parameter)
        setShown(a, rowDev->target, rows);
        u.rowSelParam = -1;
      } else if (sel && !sel->target.isChannel()) {
        a.doc.setSlotPlugin(sel->target.owner, sel->target.slot, "");
        u.selected = 0;
      }
    } else if (plain && sel && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
      pui::compareFlip(a, sel->target);
    } else if (sel && io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {  // the device menu's cut, copy and paste
      if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        copyDevice(a, *sel);
      } else if (ImGui::IsKeyPressed(ImGuiKey_X, false) && !sel->target.isChannel()) {
        copyDevice(a, *sel);
        a.doc.setSlotPlugin(sel->target.owner, sel->target.slot, "");
        u.selected = 0;
      } else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        pasteDevice(a, *sel);
      }
    }
  }
  // A plain wheel scrolls the chain sideways (over a plugin's parameter panel, the panel scrolls).
  if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0 && !ImGui::IsAnyItemActive())
    ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * 60.0f);
  {  // More devices past an edge: the edge fades and a small arrow says the wheel scrolls there.
    const ImVec2 w0 = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const ImU32 bg = P.surface900, clear = theme::withAlpha(P.surface900, 0.0f);
    ImDrawList* fdl = ImGui::GetWindowDrawList();
    auto edge = [&](bool right) {
      const float x0e = right ? w0.x + ws.x - 28 : w0.x, x1e = x0e + 28;
      fdl->AddRectFilledMultiColor(ImVec2(x0e, w0.y), ImVec2(x1e, w0.y + ws.y), right ? clear : bg, right ? bg : clear, right ? bg : clear, right ? clear : bg);
      ui::drawIcon(fdl, right ? ui::Icon::Right : ui::Icon::Left, ImVec2(right ? x1e - 9 : x0e + 9, w0.y + ws.y * 0.5f), 9.0f, P.textDim);
    };
    if (ImGui::GetScrollX() < ImGui::GetScrollMaxX() - 1) edge(true);
    if (ImGui::GetScrollX() > 1) edge(false);
  }
  ImGui::EndChild();
}

// ------------------------------------------------------------ clip column ----

namespace {

// The playlist track whose launch settings the clip column shows: the selected channel's track,
// else the first selected header.
int clipTrack(App& a) {
  const Song& s = a.doc.song();
  if (const Channel* c = s.channel(a.selChannel))
    if (const int t = trackmodes::instrumentTrackOf(s, c->id); t >= 0) return t;
  if (!a.playlist.trackSel.empty()) {
    const int t = *a.playlist.trackSel.begin();
    if (t >= 0 && t < static_cast<int>(s.tracks.size())) return t;
  }
  return -1;
}

// K-112c (round 21): one field of the column with its label above it, as the refs' clip view sets
// its fields (038, 041, 112): `cols` fields share the row side by side, `col` is this one's place.
// The cursor is left under the label, at the field's left; returns the field's width.
float columnField(App& a, const char* label, int col, int cols) {
  static ImVec2 row;
  static float rowW = 0;
  if (col == 0) row = ImGui::GetCursorScreenPos(), rowW = ImGui::GetContentRegionAvail().x;
  constexpr float kGap = 6.0f, kLabelH = 15.0f;
  const float w = std::floor((rowW - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
  const float x = row.x + static_cast<float>(col) * (w + kGap);
  ImGui::GetWindowDrawList()->AddText(a.fontRegular, theme::kFontLabel + 1, ImVec2(x, row.y), pal().textDim, label);
  ImGui::SetCursorScreenPos(ImVec2(x, row.y + kLabelH));
  return w;
}

// A section's title bar; `fill` (a colour) fills the bar, as the clip's title bar in its own colour.
// With a `foldId` the bar folds its section: a click on it shows or hides the rows under it (a
// chevron at its right: down while open, left while folded; remembered for the session). Returns
// whether the section is open.
bool columnHeading(App& a, const char* title, ImU32 chip, ImU32 fill = 0, const char* foldId = nullptr) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = 20.0f;
  bool open = true, hov = false;
  if (foldId) {
    const ImGuiID id = ImGui::GetID(foldId);
    ImGuiStorage* st = ImGui::GetStateStorage();
    open = st->GetBool(id, true);
    if (ImGui::InvisibleButton(foldId, ImVec2(w, h))) st->SetBool(id, open = !open);
    hov = ImGui::IsItemHovered();
    ui::Hint(open ? "Click to fold this section" : "Click to show this section");
    ImGui::SetCursorScreenPos(p);
  }
  // A clip's title band, square as the layout's other bars, in the clip's full colour as 038, 034 and 112
  // draw it (K-112c round 34; a 42 % tint read as a muddy brown beside the clip in the arrangement).
  const ImU32 bg = fill ? fill : P.surface700;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? theme::mix(bg, P.text, 0.08f) : bg, 0.0f);
  float tx = p.x + 6;
  // K-112c (round 35): no swatch on a band that is the colour itself (it showed as an empty check box), as 038.
  if (!fill && chip) {
    dl->AddRectFilled(ImVec2(tx, p.y + 6), ImVec2(tx + 8, p.y + 14), chip, 1.0f);
    tx += 13;
  }
  const ImU32 shown = bg;
  const ImU32 fg = fill ? theme::contrastText(shown) : P.text;
  const float right = foldId ? 20.0f : 6.0f;
  theme::textEllipsis(dl, a.fontBold, theme::kFontCaption, ImVec2(tx, theme::textTop(a.fontBold, theme::kFontCaption, p.y, h)), p.x + w - right - tx,
                      fg, title);
  if (foldId)
    ui::drawIcon(dl, open ? ui::Icon::ChevronDown : ui::Icon::ChevronLeft, ImVec2(p.x + w - 10, std::round(p.y + h * 0.5f)), 9,
                 theme::withAlpha(fg, hov ? 1.0f : 0.75f));
  ImGui::Dummy(ImVec2(w, h + 2));
  return open;
}

}  // namespace

bool columnSection(App& a, const char* title, ImU32 fill, const char* foldId) { return columnHeading(a, title, 0, fill, foldId); }
float columnRow(App& a, const char* label, int col, int cols) { return columnField(a, label, col, cols); }

void valueBox(App& a, const char* text, float w) {
  const auto& P = pal();
  const float h = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + w, p.y + h);
  ImGui::Dummy(ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, q, P.field, theme::kRadiusSm);
  dl->AddRect(p, q, ImGui::GetColorU32(ImGuiCol_Border), theme::kRadiusSm, 1.0f, 0);
  const float fs = ImGui::GetFontSize();
  dl->PushClipRect(p, q, true);
  dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + ImGui::GetStyle().FramePadding.x, theme::textTop(ImGui::GetFont(), fs, p.y, h)), P.text, text);
  dl->PopClipRect();
  (void)a;
}

void columnScrollEdges() {
  // More rows past an edge: the edge fades and a small arrow says the wheel scrolls there.
  const auto& P = pal();
  const ImVec2 w0 = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  const ImU32 bg = P.surface900, clear = theme::withAlpha(P.surface900, 0.0f);
  ImDrawList* fdl = ImGui::GetWindowDrawList();
  // K-112c (round 26): the arrow sits in a solid strip at the very edge, past a short fade, so it
  // never lands on a field's text (it crossed "Next 4 beats" at 100 % and the labels at 150 %).
  auto edge = [&](bool bottom) {
    const float y0 = bottom ? w0.y + ws.y - 24 : w0.y, y1 = y0 + 24, kSolid = 12.0f;
    const float s0 = bottom ? y1 - kSolid : y0, s1 = s0 + kSolid;   // the solid strip
    const float f0 = bottom ? y0 : s1, f1 = bottom ? s0 : y1;       // the fade
    fdl->AddRectFilledMultiColor(ImVec2(w0.x, f0), ImVec2(w0.x + ws.x, f1), bottom ? clear : bg, bottom ? clear : bg, bottom ? bg : clear,
                                 bottom ? bg : clear);
    fdl->AddRectFilled(ImVec2(w0.x, s0), ImVec2(w0.x + ws.x, s1), bg);
    ui::drawIcon(fdl, bottom ? ui::Icon::ChevronDown : ui::Icon::ChevronUp, ImVec2(w0.x + ws.x * 0.5f, std::round((s0 + s1) * 0.5f)), 9.0f, P.textDim);
  };
  if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1) edge(true);
  if (ImGui::GetScrollY() > 1) edge(false);
}

ObjectId focusedAudioClip(App& a) {
  if (a.playlist.selected.size() != 1) return kNoId;
  const Clip* c = a.doc.song().clip(*a.playlist.selected.begin());
  return c && c->kind == ClipKind::Audio && a.doc.song().channel(c->source) ? c->id : kNoId;
}

float clipColumn(App& a) {
  constexpr float kW = 188.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  if (av.x < kW + 360.0f || av.y < 80.0f) return 0.0f;  // a narrow or short window keeps its whole area for the notes
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Pattern* pat = s.pattern(a.curPattern);
  const int track = clipTrack(a);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));  // round 22: 8 px in from the panel, as the device band's cards below
  // A short band (1280 x 800) cannot show every row: the sections fold, and the column scrolls
  // with the wheel (a fade and an arrow at an edge say more is there).
  ImGui::BeginChild("##clipcol", ImVec2(kW - 2, av.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  // The clip: the pattern being edited (its name, length and meter are the pattern's own).
  // Its title bar: the clip's name in the clip's colour (the pattern's), or "Clip" when there is none.
  const bool clipOpen = pat ? columnHeading(a, pat->name.empty() ? i18n::tr("Clip") : pat->name.c_str(), 0, theme::content(pat->color), "##foldclip")
                            : columnHeading(a, i18n::tr("Clip"), 0, 0, "##foldclip");
  if (!clipOpen) {
    // folded: only the title bar
  } else if (!pat) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
    ImGui::TextWrapped("%s", i18n::tr("No pattern selected"));
    ImGui::PopStyleColor();
  } else {
    ImGui::PushID(static_cast<int>(pat->id));
    const float nameW = columnField(a, i18n::tr("Name"), 0, 1);
    static ObjectId nameFor = kNoId;
    static char name[128] = {};
    if (nameFor != pat->id || ImGui::GetActiveID() != ImGui::GetID("##name")) {  // follows the pattern unless being typed in
      std::snprintf(name, sizeof name, "%s", pat->name.c_str());
      nameFor = pat->id;
    }
    ImGui::SetNextItemWidth(nameW);
    ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemDeactivatedAfterEdit() && pat->name != name) a.doc.renamePattern(pat->id, name);
    ui::Hint("The pattern's name (the same name the channel rack and the playlist show)");
    const float lenW = columnField(a, i18n::tr("Length"), 0, 2);
    {
      const float bw = ImGui::GetFrameHeight();
      char len[32];
      std::snprintf(len, sizeof len, "%d %s", pat->bars, pat->bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
      if (ImGui::Button("-", ImVec2(bw, 0)) && pat->bars > 1) a.doc.setPatternLength(pat->id, pat->bars - 1, pat->stepsPerBar);
      ui::Hint("One bar shorter");
      ImGui::SameLine(0, 2);
      const ImVec2 lp = ImGui::GetCursorScreenPos();
      const float lw = std::max(20.0f, lenW - bw * 2 - 4);
      ImGui::Dummy(ImVec2(lw, bw));
      ImGui::GetWindowDrawList()->AddRectFilled(lp, ImVec2(lp.x + lw, lp.y + bw), P.field, theme::kRadiusSm);
      ImGui::GetWindowDrawList()->AddRect(lp, ImVec2(lp.x + lw, lp.y + bw), ImGui::GetColorU32(ImGuiCol_Border), theme::kRadiusSm, 1.0f, 0);  // round 23: the fields' rim
      theme::centreText(ImGui::GetWindowDrawList(), a.fontRegular, theme::kFontLabel + 1, lp, ImVec2(lp.x + lw, lp.y + bw), P.text, len);
      ImGui::SameLine(0, 2);
      if (ImGui::Button("+", ImVec2(bw, 0)) && pat->bars < 512) a.doc.setPatternLength(pat->id, pat->bars + 1, pat->stepsPerBar);
      ui::Hint("One bar longer");
    }
    const float meterW = columnField(a, i18n::tr("Meter"), 1, 2);
    {
      char m[32];
      if (pat->ownMeter()) std::snprintf(m, sizeof m, "%d / %d", pat->meterNum, pat->meterDen);
      else std::snprintf(m, sizeof m, "%s", i18n::tr("the song's"));
      valueBox(a, m, meterW);  // K-112c (round 23): boxed, as every ref's clip values (038, 041, 045, 112)
      ui::Hint("The pattern's own meter, or the song's (change it from the piano roll's toolbar)");
    }
    ImGui::PopID();
  }
  ImGui::Dummy(ImVec2(0, 4));
  // Launch: how the clip's track starts and ends its clips (the same settings as the track's menu).
  const bool launchOpen = columnHeading(a, i18n::tr("Launch"), 0, 0, "##foldlaunch");
  if (!launchOpen) {
    // folded: only the title bar
  } else if (track < 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
    ImGui::TextWrapped("%s", i18n::tr("Select a track to set how its clips launch"));
    ImGui::PopStyleColor();
  } else {
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
    auto edit = [&](const char* label, auto fn) { a.doc.editPlaylistTrack(track, label, fn); };
    ImGui::PushID(1000 + track);
    const float wpad = columnField(a, i18n::tr("Press"), 0, 2);
    if (ui::BeginCombo("##pad", padModeName(t.pad), wpad)) {
      for (int i = 0; i < static_cast<int>(PadMode::Count); ++i)
        if (ImGui::Selectable(padModeName(static_cast<PadMode>(i)), t.pad == static_cast<PadMode>(i)))
          edit("change pad mode", [i](PlaylistTrack& x) { x.pad = static_cast<PadMode>(i); });
      ImGui::EndCombo();
    }
    ui::Hint("What a press on a clip does");
    const float wfollow = columnField(a, i18n::tr("At its end"), 1, 2);
    if (ui::BeginCombo("##follow", followModeName(t.follow), wfollow)) {
      for (int i = 0; i < static_cast<int>(FollowMode::Count); ++i)
        if (ImGui::Selectable(followModeName(static_cast<FollowMode>(i)), t.follow == static_cast<FollowMode>(i)))
          edit("change clip end action", [i](PlaylistTrack& x) { x.follow = static_cast<FollowMode>(i); });
      ImGui::EndCombo();
    }
    ui::Hint("What happens when a launched clip reaches its end");
    const float wlaunch = columnField(a, i18n::tr("Starts on"), 0, 1);  // full width: its grids read in full ("Next 4 beats")
    if (ui::BeginCombo("##launch", launchGridName(t.launch), wlaunch)) {
      for (int i = 0; i < static_cast<int>(LaunchGrid::Count); ++i)
        if (ImGui::Selectable(launchGridName(static_cast<LaunchGrid>(i)), t.launch == static_cast<LaunchGrid>(i)))
          edit("change launch grid", [i](PlaylistTrack& x) { x.launch = static_cast<LaunchGrid>(i); });
      ImGui::EndCombo();
    }
    ui::Hint("The grid a launched clip waits for before it starts");
    const float wphase = columnField(a, i18n::tr("In phase"), 0, 1);
    if (ui::BeginCombo("##phase", t.syncStart == LaunchGrid::Now ? i18n::tr("Off") : launchGridName(t.syncStart), wphase)) {
      if (ImGui::Selectable(i18n::tr("Off"), t.syncStart == LaunchGrid::Now)) edit("change phase", [](PlaylistTrack& x) { x.syncStart = LaunchGrid::Now; });
      for (int i = 1; i < static_cast<int>(LaunchGrid::Count); ++i)
        if (ImGui::Selectable(launchGridName(static_cast<LaunchGrid>(i)), t.syncStart == static_cast<LaunchGrid>(i)))
          edit("change phase", [i](PlaylistTrack& x) { x.syncStart = static_cast<LaunchGrid>(i); });
      ImGui::EndCombo();
    }
    ui::Hint("Off: a launched clip starts from its start. A grid: it starts in phase with the song on that grid");
    ImGui::PopID();
  }
  columnScrollEdges();
  ImGui::EndChild();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x + kW - 1.5f, p0.y), ImVec2(p0.x + kW - 1.5f, p0.y + av.y), P.line);
  ImGui::SetCursorScreenPos(p0);
  return kW;
}

// -------------------------------------------------------------- clip grid ----

namespace {

// The tracks the grid shows as columns: every track up to the last one in use (clips, a link or a name).
// K-112c (round 39): the arrangement's rows in Clips are the same tracks (core/TrackModes.h).
int gridColumns(const Song& s) { return trackmodes::usedTrackCount(s); }

Rgb clipColour(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Pattern) {
    if (const Pattern* p = s.pattern(c.pattern)) return p->color;
  } else if (c.kind == ClipKind::Audio) {
    if (const Channel* ch = s.channel(c.source)) return ch->color;
  } else if (const AutomationClip* au = s.automation(c.source)) {
    return au->color;
  }
  return 0x50565E;
}

std::string clipLabel(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Pattern) {
    if (const Pattern* p = s.pattern(c.pattern)) return p->name;
  } else if (c.kind == ClipKind::Audio) {
    if (const Channel* ch = s.channel(c.source)) return ch->name;
  } else if (const AutomationClip* au = s.automation(c.source)) {
    return au->name;
  }
  return std::string();
}

// K-112c (round 44): a clip picked in the grid brings its channel along, so the piano roll's channel
// picker and the clip band name the same part: a pattern clip keeps the selected channel when it
// plays in the pattern, else takes the pattern's first channel that does; an audio clip its own.
void selectClipChannel(App& a, const Clip& c) {
  const Song& s = a.doc.song();
  if (c.kind == ClipKind::Audio) {
    if (s.channel(c.source)) a.selectChannel(c.source);
    return;
  }
  const Pattern* p = c.kind == ClipKind::Pattern ? s.pattern(c.pattern) : nullptr;
  if (!p) return;
  auto plays = [&](ObjectId ch) {
    if (auto it = p->notes.find(ch); it != p->notes.end() && !it->second.empty()) return true;
    if (auto it = p->steps.find(ch); it != p->steps.end())
      for (uint8_t v : it->second)
        if (v) return true;
    return false;
  };
  if (plays(a.selChannel)) return;
  for (const Channel& ch : s.channels)
    if (plays(ch.id)) {
      a.selectChannel(ch.id);
      return;
    }
}

void launch(App& a, ObjectId clip) {
  if (a.engine.mode() != PlayMode::Live) a.engine.setMode(PlayMode::Live);
  a.engine.launchClip(clip);
}

bool gGridAsked = false;  // K-287 (dev): the next drawn clip grid prints its slots

}  // namespace

void devGridAsk() { gGridAsked = true; }

namespace {

// K-112c (round 38): the session mixer's strips, shared by the clip grid's foot and the Clips mixer band.
struct StripRow {
  float y0 = 0, h = 0;     // the strips' top and height
  bool sendsRow = false;  // the sends row across their top
};
// A track's first two sends to a return (not the master, which every track reaches): (send index, mixer index).
std::vector<std::pair<size_t, int>> sendsOf(const Song& s, const MixerTrack* mx) {
  std::vector<std::pair<size_t, int>> v;
  if (!mx) return v;
  for (size_t k = 0; k < mx->sends.size() && v.size() < 2; ++k) {
    const Send& sd = mx->sends[k];
    if (sd.sidechain) continue;
    int dest = -1;
    for (size_t j = 0; j < s.mixer.size(); ++j)
      if (s.mixer[j].id == sd.to) dest = static_cast<int>(j);
    if (dest > 0) v.emplace_back(k, dest);
  }
  return v;
}
// K-112c (round 28): the sends get their own row across the top of every strip, above the volume
// and the meter, with "Sends" at the right and the knobs staggered, each with its return's letter
// in its open foot (143, 141, 112). The row is kept on every strip (blank where a track sends
// nowhere) so the faders line up, and dropped when the strips are too short for it.
constexpr float kSendsH = 34.0f;
bool anySends(const Song& s) {
  for (int t = 0; t < gridColumns(s); ++t)
    if (!sendsOf(s, trackmodes::playedMixer(s, t)).empty()) return true;
  return false;
}
void sessionStrip(App& a, ImDrawList* dl, const StripRow& g, int t, const MixerTrack* mx, float x, float w, bool master, char letter = 0) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const ImVec2 m = ImGui::GetIO().MousePos;
  const float my0 = g.y0, mh = g.h;
  const bool sendsRow = g.sendsRow;
  ImGui::PushID(master ? 9999 : letter ? 8000 + letter : 2000 + t);
  if (mx) {
    // K-112c (round 32): collaboration, as Studio's mixer: a peer's pointer on this mixer track (in any
    // layout) lands on this strip, and a peer's change to it flashes its outline.
    canchor::publishRect(canchor::MixerStrip, mx->id, 0, ImVec2(x, my0), ImVec2(x + w, my0 + mh));
    if (ImU32 fc = 0; const float fl = collabui::flash(a, "mixer/#" + std::to_string(mx->id), &fc))
      dl->AddRect(ImVec2(x + 1, my0 + 1), ImVec2(x + w - 1, my0 + mh - 1), theme::withAlpha(fc, fl), 0.0f, 0, 2.0f);
  }
  // The sends row (see above), then two columns. Left, top to bottom: the peak and the volume in
  // dB, pan, then the activator, S and arm stacked. Right: the fader beside a meter with its dB
  // scale, the rest of the strip's height.
  const float bottom = my0 + mh, top = my0 + 4 + (sendsRow ? kSendsH : 0.0f);
  const float colL = std::min(44.0f, std::floor(w * 0.48f));  // the left column's width
  const float rx = x + colL + 4;                               // the right column
  if (sendsRow && mx && !master) {
    const float sy = my0 + 4;
    const auto sends = sendsOf(s, mx);
    if (!sends.empty()) {
      const char* lab = i18n::tr("Sends");
      const float lw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lab).x;
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(x + w - 4 - lw), std::round(sy)), P.textDim, lab);
    }
    constexpr float kR = 10.0f;
    for (size_t i = 0; i < sends.size(); ++i) {
      const auto [k, dest] = sends[i];
      const ImVec2 kp(x + 4 + static_cast<float>(i) * (2 * kR + 6), sy + static_cast<float>(i) * 8.0f);
      ImGui::SetCursorScreenPos(kp);
      ImGui::PushID(static_cast<int>(k));
      ui::ParamKnob("##send", sendParam(mx->id, dest), kR, nullptr, false);
      if (ImGui::IsItemHovered()) a.setHint(std::string(i18n::tr("Send to")) + " " + s.mixer[static_cast<size_t>(dest)].name);
      if (const char lt = returnLetter(s, dest)) {  // the return's letter in the knob's open foot
        const char ls[2] = {lt, 0};
        const float fz = 9.0f, lw2 = a.fontRegular->CalcTextSizeA(fz, 1e9f, 0, ls).x;
        dl->AddText(a.fontRegular, fz, ImVec2(std::round(kp.x + kR - lw2 * 0.5f), std::round(kp.y + 2 * kR - fz + 1)), P.textDim, ls);
      }
      ImGui::PopID();
    }
  }
  // The peak since it was last cleared (click it to clear), then the volume (dB) under it, each
  // value centred, as the refs' strips (357, 280, 170, 143).
  // K-112c (round 44): a short strip (a small window or a large interface scale) keeps what fits,
  // from the most used down: the volume field at the top, the fader and meter, the activator; then
  // S, arm, the peak and pan as the height allows. Nothing is drawn over anything else.
  const bool compact = mh < 126.0f;
  if (mx && !compact) {
    static std::map<ObjectId, float> held;  // per mixer track, linear
    float& pk = held[mx->id];
    pk = std::max(pk, mixmeters::get(a, mx->id).peak());
    const ImVec2 k0(x + 2, top), k1(x + colL, top + 16);
    ImGui::SetCursorScreenPos(k0);
    if (ImGui::InvisibleButton("##peak", ImVec2(k1.x - k0.x, k1.y - k0.y))) pk = 0;
    ui::Hint("The highest level since this was last clicked (click to clear)");
    // K-112c (round 33): a filled capsule over the square volume field, as 357 and 170 draw them (round 32
    // squared it by mistake; the refs' peak is round, the volume square).
    const float kr = (k1.y - k0.y) * 0.5f;
    dl->AddRectFilled(k0, k1, P.dark ? P.field : P.surface950, kr);
    dl->AddRect(k0, k1, pk >= 1.0f ? P.arm : theme::mix(P.dark ? P.field : P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), kr, 0, 1.0f);
    char pb[16];
    if (pk > 1e-6f) std::snprintf(pb, sizeof pb, "%.1f", static_cast<double>(20.0f * std::log10(pk)));
    else std::snprintf(pb, sizeof pb, "-\xE2\x88\x9E");  // minus infinity, as the refs write it
    const float pw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, pb).x;
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round((k0.x + k1.x - pw) * 0.5f), theme::textTop(a.fontRegular, theme::kFontLabel, top, 16)),
                pk >= 1.0f ? P.arm : P.textDim, pb);
  }
  // Volume (dB). K-112c (round 44): a track with no mixer track yet has none (empty02, 069: an empty
  // MIDI track's strip is bare but for its meter and buttons).
  const float volTop = compact ? top : top + 18;
  if (mx) {
    const ImVec2 v0(x + 2, volTop), v1(x + colL, volTop + 16);
    dl->AddRectFilled(v0, v1, P.dark ? P.field : P.surface950, theme::kRadiusSm);
    char vb[16];
    if (mx->volume > 1e-6f) std::snprintf(vb, sizeof vb, "%.1f", static_cast<double>(20.0f * std::log10(mx->volume)));
    else std::snprintf(vb, sizeof vb, "-\xE2\x88\x9E");  // minus infinity, as the refs write it
    const float tw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, vb).x;
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round((v0.x + v1.x - tw) * 0.5f), theme::textTop(a.fontRegular, theme::kFontLabel, volTop, 16)),
                P.text, vb);
  }
  // The left column from the bottom up: arm, S, the activator, then pan.
  // K-112c (round 28): the strip ends in a bar of the track's colour (143, 146, 280, 141), so the
  // controls stand on it with a gap.
  constexpr float kFootH = 3.0f, kFootGap = 4.0f;
  const float base = bottom - kFootH - kFootGap;
  float armY = base - 15, soloY = armY - 17, actY = soloY - 21;
  const float panY = actY - 27;
  // In a short strip only the buttons that fit under the volume field are drawn, the activator
  // first, then S, then arm (all three stay in the arrangement's headers).
  const float minY = volTop + (mx ? 20.0f : 0.0f);
  int nb = 3;
  if (compact) {
    const float room = base - minY;
    nb = room >= 53 ? 3 : room >= 36 ? 2 : room >= 19 ? 1 : 0;
    if (nb == 2) soloY = base - 15, actY = soloY - 21;
    if (nb == 1) actY = base - 19;
  }
  const bool showPan = !compact && panY >= minY, fitSolo = nb >= 2, fitArm = nb >= 3;
  // Fader and meter of the mixer track, with the scale on the right.
  const float fy = top, fh = std::max(24.0f, bottom - kFootH - kFootGap - 1 - top);
  if (mx) {
    ImGui::SetCursorScreenPos(ImVec2(rx, fy));
    ui::setFaderPointer(true);  // K-112c: a pointer beside the meter, as the refs' strips
    ui::ParamFader("##fader", mixerParam(mx->id, MixerParam::Volume), ImVec2(14, fh));
    ui::setFaderPointer(false);
    const mixmeters::Reading r = mixmeters::get(a, mx->id);
    ImGui::SetCursorScreenPos(ImVec2(rx + 16, fy));
    ui::Meter(ImGui::GetID("meter"), r.peakL, r.peakR, ImVec2(10, fh));
    // The meter's dB scale (its range is -60 to +6 dB): a tick and a number every 12 dB, the
    // numbers only where there is room for them.
    const float fsz = 9.0f, sx = rx + 28;
    const int step = fh >= 90 ? 12 : 24;
    for (int db = 0; db >= -60; db -= step) {
      const float ty = std::round(fy + fh - (static_cast<float>(db) + 60.0f) / 66.0f * fh) + 0.5f;
      dl->AddLine(ImVec2(sx, ty), ImVec2(sx + 3, ty), P.textFaint);
      if (fh >= 48 && sx + 4 + 10 <= x + w + 2) {
        const std::string n = std::to_string(-db);
        dl->AddText(a.fontRegular, fsz, ImVec2(sx + 4, std::round(ty - fsz * 0.5f)), P.textFaint, n.c_str());
      }
    }
    // Pan: the mixer's own knob (undo, automation, links as there), as large as the refs' (143, 146, 280).
    const float kc = x + 2 + (colL - 2) * 0.5f;  // the left column's centre
    if (showPan) {
      ImGui::SetCursorScreenPos(ImVec2(kc - 11, panY));
      ui::ParamKnob("##pan", mixerParam(mx->id, MixerParam::Pan), 11.0f, nullptr, true);
      ui::Hint("Pan");
    }
  } else {
    // K-112c (round 44): no mixer track yet: a dotted, unlit meter where the fader and meter stand
    // (empty02, 069), and a hint saying how the track gets one.
    // A track whose clips play into several mixer tracks shows their loudest level there instead
    // (read only: each has its own fader in the mixer).
    const std::vector<ObjectId> outs = t >= 0 ? trackmodes::playedMixers(s, t) : std::vector<ObjectId>{};
    const float mx0 = rx + 16, my1 = fy + fh;
    if (outs.empty()) {
      for (float yy = fy + 2; yy + 2 <= my1; yy += 5) dl->AddRectFilled(ImVec2(mx0 + 3, yy), ImVec2(mx0 + 6, yy + 2), theme::withAlpha(P.textFaint, 0.55f));
    } else {
      float pl = 0, pr = 0;
      for (ObjectId id : outs) {
        const mixmeters::Reading r = mixmeters::get(a, id);
        pl = std::max(pl, r.peakL), pr = std::max(pr, r.peakR);
      }
      ImGui::SetCursorScreenPos(ImVec2(mx0, fy));
      ui::Meter(ImGui::GetID("meter"), pl, pr, ImVec2(10, fh));
    }
    ImGui::SetCursorScreenPos(ImVec2(rx, fy));
    ImGui::InvisibleButton("##nomixer", ImVec2(std::max(1.0f, x + w - rx), std::max(1.0f, fh)));
    ui::Hint(outs.empty() ? "No mixer track yet: put an instrument or an audio clip on this track to give it one"
                          : "This track plays into several mixer tracks: the loudest of them (each has its fader in the mixer)");
  }
  // Activator (number), S, arm, stacked: the same controls as the arrangement's headers.
  const float bx0 = x + 2, bx1 = x + colL;
  if (letter && nb >= 1) {  // K-112c (round 19): a return's activator is its letter (143, 141, 280)
    const float cxl = std::round((bx0 + bx1) * 0.5f);
    const Cell ca{ImVec2(cxl - 10, actY), ImVec2(cxl + 10, actY + 18)};  // K-112c (round 28): square, as S is wide
    const bool on = mx && !mx->muted;
    square(dl, ca, on ? P.hot : P.surface900, ImGui::IsWindowHovered() && ca.contains(m));
    const char ls[2] = {letter, 0};
    centredText(dl, a.fontRegular, theme::kFontLabel, ca, on ? theme::contrastText(P.hot) : P.textFaint, ls);
    ImGui::SetCursorScreenPos(ca.a);
    if (ImGui::InvisibleButton("##ract", ImVec2(ca.b.x - ca.a.x, ca.b.y - ca.a.y)) && mx)
      a.doc.setParam(mixerParam(mx->id, MixerParam::Mute), mx->muted ? 0.0 : 1.0, false);
    ui::Hint("Return track: turn it on or off");
  } else if (!master && !letter && nb >= 1) {
    const Song& ss = a.doc.song();
    const PlaylistTrack& tr = ss.tracks[static_cast<size_t>(t)];
    const bool hovAny = ImGui::IsWindowHovered();
    const float cxm = std::round((bx0 + bx1) * 0.5f);
    const Cell ca{ImVec2(cxm - 10, actY), ImVec2(cxm + 10, actY + 18)};  // K-112c (round 28): square, as S is wide (143, 146, 280, 141)
    const Cell cs{ImVec2(cxm - 10, soloY), ImVec2(cxm + 10, soloY + 14)};
    const Cell cr{ImVec2(cxm - 10, armY), ImVec2(cxm + 10, armY + 14)};
    char num[16];
    std::snprintf(num, sizeof num, "%d", t + 1);
    square(dl, ca, !tr.muted ? P.hot : P.surface900, hovAny && ca.contains(m));
    centredText(dl, a.fontRegular, theme::kFontLabel, ca, !tr.muted ? theme::contrastText(P.hot) : P.textFaint, num);
    const bool so = soloed(ss, t);
    if (fitSolo) {
      square(dl, cs, so ? P.solo : P.surface900, hovAny && cs.contains(m));
      centredText(dl, a.fontBold, theme::kFontLabel, cs, so ? theme::contrastText(P.solo) : P.textDim, "S");
    }
    const bool audio = trackmodes::modeOf(ss, t) == TrackMode::Audio, armed = audio && mx && mx->armed;
    if (fitArm) square(dl, cr, armed ? theme::withAlpha(P.arm, 0.85f) : P.surface900, audio && hovAny && cr.contains(m));
    if (fitArm) dl->AddCircleFilled(ImVec2((cr.a.x + cr.b.x) * 0.5f, (cr.a.y + cr.b.y) * 0.5f), 3.5f, armed ? theme::contrastText(P.arm) : audio ? P.text : theme::withAlpha(P.textDim, 0.45f), 12);  // K-112c (round 19): a neutral dot until armed (142, 157, 143)
    ImGui::SetCursorScreenPos(ca.a);
    if (ImGui::InvisibleButton("##act", ImVec2(ca.b.x - ca.a.x, ca.b.y - ca.a.y))) clickHeader(a, t, HeaderPart::Activator, false);
    ui::Hint(headerHint(HeaderPart::Activator));
    if (fitSolo) {
      ImGui::SetCursorScreenPos(cs.a);
      if (ImGui::InvisibleButton("##solo", ImVec2(cs.b.x - cs.a.x, cs.b.y - cs.a.y))) clickHeader(a, t, HeaderPart::Solo, false);
      ui::Hint(headerHint(HeaderPart::Solo));
    }
    if (fitArm) {
      ImGui::SetCursorScreenPos(cr.a);
      if (ImGui::InvisibleButton("##arm", ImVec2(cr.b.x - cr.a.x, cr.b.y - cr.a.y)) && audio) clickHeader(a, t, HeaderPart::Arm, false);
      ui::Hint(headerHint(HeaderPart::Arm));
    }
  }
  // K-112c (round 32): Main's strip no longer draws a "Main" box where the tracks have their
  // activator: it looked like a button and did nothing (its title already names it).
  {  // the colour bar along the foot
    const bool trk = !master && !letter && t >= 0 && t < static_cast<int>(s.tracks.size());
    const ImU32 cc = trk ? theme::content(s.tracks[static_cast<size_t>(t)].color) : mx ? theme::content(mx->color) : P.surface600;
    dl->AddRectFilled(ImVec2(x, std::round(bottom - kFootH)), ImVec2(x + w, std::round(bottom)), cc);
  }
  ImGui::PopID();
}

}  // namespace

void drawClipGrid(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImGui::BeginChild("##clipgrid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float dpp = pixelgrid::frameScale();  // K-276: every cell edge on a device pixel, at any interface scale
  const ImVec2 o(livegrid::snap(ImGui::GetCursorScreenPos().x, dpp), livegrid::snap(ImGui::GetCursorScreenPos().y, dpp));
  const ImVec2 av = ImGui::GetContentRegionAvail();
  const int nCols = gridColumns(s);
  // Track columns keep one width whatever the window's (spare width stays free between the last
  // column and Main, which sits at the right edge). K-112c (round 19): one gap everywhere: kGap
  // between columns, between slot rows, under the titles and above the stop row (18 px cells).
  // (LiveGrid.h holds the geometry, so a test measures the gaps.)
  using livegrid::kPad, livegrid::kTitleH, livegrid::kGap, livegrid::kSlotH, livegrid::kCellH;
  constexpr float kMainW = 140, colW = livegrid::kColW;
  const Tick sceneT = std::max<Tick>(1, s.barTicks()) * kSceneBars;
  // Slots: the live area's clips by scene.
  int nScenes = s.live.areaEnd > 0 ? static_cast<int>((s.live.areaEnd + sceneT - 1) / sceneT) : 0;
  std::vector<std::vector<std::vector<int>>> slot(static_cast<size_t>(nCols));
  for (int t = 0; t < nCols; ++t) {
    slot[static_cast<size_t>(t)].resize(static_cast<size_t>(std::max(nScenes, 0)));
    for (int ci : liveClips(s, t)) {
      const int k = static_cast<int>(s.playlist[static_cast<size_t>(ci)].start / sceneT);
      if (k >= 0 && k < nScenes) slot[static_cast<size_t>(t)][static_cast<size_t>(k)].push_back(ci);
    }
  }
  // The mixer at the foot of the grid: compact strips (volume, fader and meter, pan and sends,
  // then the activator, S and arm), 110 to 260 px (round 24: about 45 % of the panel, so the sends show at 1440 as in 143, 112, 093). The slots fill the panel with
  // empty rows down to the mixer (round 25: 276, 355), every scene in use always shown.
  // K-112c (round 44): with the mixer band open under the grid (the Mixer shown in Clips) the strips
  // are drawn there only, not twice; the slots take the grid's whole height.
  const bool footMixer = !(a.showMixer && !a.showStage);
  const float mixH = footMixer ? std::clamp(av.y * 0.45f, 110.0f, 260.0f) : 0.0f;
  const int rowsFit = std::max(1, static_cast<int>((av.y - kPad * 2 - kTitleH - kGap - mixH - kSlotH) / kSlotH));  // and the stop row
  const int rows = std::max(nScenes, rowsFit);  // K-112c (round 25): empty slot rows fill the panel down to the mixer, as 276 and 355
  const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 8.0f);
  static ObjectId held = kNoId;
  static std::pair<int, int> dragFrom{-1, -1};  // K-287: the slot (track, scene) a press started on
  static int titleDrag = -1;                     // K-112c (round 39): a column dragged by its title (moves the track)
  const ImVec2 m = ImGui::GetIO().MousePos;
  const float y0 = livegrid::titleY(o.y, dpp), yT1 = livegrid::titleBottom(o.y, dpp), titleH = yT1 - y0;
  const float fs = theme::kFontCaption + 1;
  auto column = [&](int t) { return livegrid::columnX(o.x, t, dpp); };
  const float mainX = std::max(column(nCols) + 4, livegrid::snap(o.x + av.x - kPad - kMainW + 4, dpp));
  // K-112c (round 36): the returns stand beside Main at the right, as 101, 112 and 357, not right
  // after the tracks with the spare width between them and Main. The shift is whole columns' worth
  // of snapped space, so each return keeps the K-276 column span and gaps.
  float retShift = 0.0f;
  int nFit = 0;
  {
    const std::vector<size_t> rets = returnTracks(s);
    while (nFit < static_cast<int>(rets.size()) && nFit < 26 && column(nCols + nFit) + colW - 4 <= mainX - 6) ++nFit;
    if (nFit > 0) retShift = std::max(0.0f, livegrid::snap(mainX - column(nCols + nFit), dpp));
  }
  auto retColumn = [&](int i) { return livegrid::column(o.x + retShift, nCols + i, dpp); };
  // K-112c (round 43): the free column between the last track and the returns (or Main), where
  // files and instruments dropped make new tracks (101, 069, 375, empty02).
  const float freeX0 = column(nCols), freeX1 = (nFit > 0 ? retColumn(0).x0 : mainX) - kGap;
  for (int t = 0; t < nCols; ++t) {
    const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
    const pixelgrid::Span cs = livegrid::column(o.x, t, dpp);
    const float x = cs.x0, w = cs.w();  // K-112c: columns kGap apart, square slots, as the refs
    ImGui::PushID(t);
    // Title: the track's colour and name (a click selects the track, as a header does).
    const ImU32 tc = theme::content(tr.color);
    const bool sel = a.playlist.trackSel.count(t) > 0;
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, yT1), tc, 0.0f);
    // K-112c (round 32): a peer's pointer on this track's header (arrangement, any layout) lands on its title here.
    canchor::publishRect(canchor::PlaylistHeader, static_cast<uint32_t>(t), 0, ImVec2(x, y0), ImVec2(x + w, yT1));
    if (sel) {  // K-276: the ring stroked wholly inside the title
      const float in = pixelgrid::strokeIn(1.5f);
      dl->AddRect(ImVec2(x + in, y0 + in), ImVec2(x + w - in, yT1 - in), P.text, 0.0f, 1.5f, 0);
    }
    const std::string name = playlistTrackName(s, static_cast<size_t>(t));  // K-112c (round 30): the name alone (357, 093, 143)
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x + 6, theme::textTop(a.fontRegular, fs, y0, titleH)), w - 10, theme::contrastText(tc), name.c_str());
    ImGui::SetCursorScreenPos(ImVec2(x, y0));
    if (ImGui::InvisibleButton("##title", ImVec2(w, titleH))) {
      a.playlist.trackSel = {t};
      if (const Channel* ch = trackmodes::linkedChannel(s, t)) a.selectChannel(ch->id);
    }
    if (ImGui::BeginPopupContextItem("##trlive")) {
      playlistTrackMenu(a, t);  // the track's whole menu (its launch settings under Live settings)
      ImGui::EndPopup();
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) titleDrag = t;  // K-112c (round 39)
    ui::Hint("The track: click selects it (its devices show below), drag to move it, right-click: the track's menu");
    const ObjectId act = a.engine.liveActive(t), que = a.engine.liveQueued(t);
    for (int k = 0; k < rows; ++k) {
      const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
      const float sy = rs.x0;
      const ImVec2 q0(x, sy), q1(x + w, rs.x1);
      ImGui::PushID(k);
      ImGui::SetCursorScreenPos(q0);
      ImGui::InvisibleButton("##slot", ImVec2(w, q1.y - q0.y));
      const bool hov = ImGui::IsItemHovered(), pressed = ImGui::IsItemActive();  // K-112c (round 31): a press sinks the slot
      const std::vector<int>* in = k < nScenes ? &slot[static_cast<size_t>(t)][static_cast<size_t>(k)] : nullptr;
      if (in && !in->empty()) {
        const Clip& c = s.playlist[static_cast<size_t>(in->front())];
        if (ImGui::IsItemActivated()) {
          launch(a, c.id);
          held = c.id;
          dragFrom = {t, k};  // K-287: dragged onto another slot, the clip moves there
          a.playlist.selected = {c.id};  // the clip band shows the clip (its notes, or an audio clip's sample)
          if (c.kind == ClipKind::Pattern) a.selectPattern(c.pattern);
          selectClipChannel(a, c);
        }
        if (ImGui::BeginPopupContextItem("##slotmenu")) {  // the clip's menu, as in the arrangement
          devArrangementClipMenu(a, c.id);
          ImGui::EndPopup();
        }
        const bool on = act == c.id, waiting = que == c.id;
        const ImU32 base = theme::content(clipColour(s, c));
        const ImU32 fill = pressed && !on ? theme::mix(base, IM_COL32(0, 0, 0, 255), 0.14f) : hov && !on ? theme::mix(base, P.paper, 0.12f) : base;
        dl->AddRectFilled(q0, q1, fill, 0.0f);
        // K-276: the 2 px ring stroked wholly inside the cell (centred on its edge it reached into the gap).
        const float ri = pixelgrid::strokeIn(2.0f);
        const ImVec2 r0(q0.x + ri, q0.y + ri), r1(q1.x - ri, q1.y - ri);
        if (on) dl->AddRect(r0, r1, P.launch, 0.0f, 2.0f, 0);
        // K-112c (round 32): a collaborator's change to the clip flashes its slot, as the arrangement's clips.
        if (ImU32 fc = 0; const float fl = collabui::flash(a, "playlist/#" + std::to_string(c.id), &fc))
          dl->AddRect(r0, r1, theme::withAlpha(fc, fl), 0.0f, 0, 2.0f);
        if (waiting) dl->AddRect(r0, r1, theme::withAlpha(P.launch, 0.35f + 0.65f * pulse), 0.0f, 2.0f, 0);
        const ImU32 ink = theme::contrastText(fill);
        const float cyy = (q0.y + q1.y) * 0.5f;
        // K-112c: the launch triangle in a cell of its own (a divider), drawn soft, as the refs do.
        ui::drawIconFit(dl, ui::Icon::Play, ImVec2(q0.x + 6, cyy - 4.5f), ImVec2(q0.x + 13, cyy + 4.5f), theme::withAlpha(ink, 0.6f));
        dl->AddLine(ImVec2(q0.x + 16.5f, q0.y), ImVec2(q0.x + 16.5f, q1.y), theme::withAlpha(IM_COL32(16, 18, 22, 255), 0.35f));
        std::string lbl = clipLabel(s, c);
        if (in->size() > 1) lbl += " +" + std::to_string(in->size() - 1);
        theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(q0.x + 19, theme::textTop(a.fontRegular, theme::kFontCaption, cyy, 0)), w - 24, ink, lbl.c_str());
        ui::Hint("Clip slot: press to launch the clip at the track's launch grid, right-click: the clip's menu");
      } else {
        // An empty slot stops the track.
        if (ImGui::IsItemClicked()) a.engine.stopLiveTrack(t);
        dl->AddRectFilled(q0, q1, pressed ? P.surface600 : hov ? P.surface700 : P.surface800, 0.0f);
        const float cyy = std::round((q0.y + q1.y) * 0.5f);
        // The stop square on the launch triangle's centre line (both centred 9 px in), so they line up down a column.
        dl->AddRectFilled(ImVec2(q0.x + 6, cyy - 3), ImVec2(q0.x + 12, cyy + 3), hov ? P.textDim : P.textFaint);
        ui::Hint("Empty slot: click to stop the track's clip");
      }
      ImGui::PopID();
    }
    ImGui::PopID();
  }
  // Main column: scenes (launch every track's slot in the row, stop the others), Stop all.
  {
    const float x = mainX, w = pixelgrid::whole(kMainW - 8, dpp);
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, yT1), mainFill(), 0.0f);  // K-112c: square, as the track titles
    dl->AddText(a.fontRegular, fs, ImVec2(x + 6, theme::textTop(a.fontRegular, fs, y0, titleH)), theme::contrastText(mainFill()), i18n::tr("Main"));
    for (int k = 0; k < rows; ++k) {
      const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
      const float sy = rs.x0;
      const ImVec2 q0(x, sy), q1(x + w, rs.x1);
      ImGui::PushID(1000 + k);
      ImGui::SetCursorScreenPos(q0);
      if (ImGui::InvisibleButton("##scene", ImVec2(w, q1.y - q0.y))) {
        for (int t = 0; t < nCols; ++t) {
          const std::vector<int>* in = k < nScenes ? &slot[static_cast<size_t>(t)][static_cast<size_t>(k)] : nullptr;
          if (in && !in->empty()) launch(a, s.playlist[static_cast<size_t>(in->front())].id);
          else a.engine.stopLiveTrack(t);
        }
      }
      const bool hov = ImGui::IsItemHovered();
      // K-112c: square scene cells, the launch triangle at the left and the scene's number at the
      // right end, as every ref draws the scene column. K-112c (round 43): in every row, an empty
      // set's too (empty01, empty02: eight numbered scenes before any clip). A row past the live
      // area is a scene of empty slots: launching it stops every track, as an empty slot does. It
      // is drawn, not stored: the song gains nothing until a clip lands in the row.
      dl->AddRectFilled(q0, q1, hov ? P.surface700 : P.surface800, 0.0f);
      const float cyy = (q0.y + q1.y) * 0.5f;
      ui::drawIconFit(dl, ui::Icon::Play, ImVec2(q0.x + 6, cyy - 4.5f), ImVec2(q0.x + 13, cyy + 4.5f), hov ? P.text : P.textDim);
      const std::string sn = std::to_string(k + 1);
      const float nw = numFont(a)->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, sn.c_str()).x;
      dl->AddText(numFont(a), theme::kFontCaption, ImVec2(std::round(q1.x - 7 - nw), theme::textTop(numFont(a), theme::kFontCaption, cyy, 0)), P.text, sn.c_str());
      ui::Hint("Scene: launches every track's clip in this row (tracks with an empty slot stop)");
      ImGui::PopID();
    }
  }
  float gridBottom = livegrid::rowY(o.y, rows, dpp);
  if (gGridAsked) {  // K-287 (dev): each slot's centre and its first clip (0: empty)
    gGridAsked = false;
    std::printf("livegrid cols %d scenes %d rows %d\n", nCols, nScenes, rows);
    for (int t = 0; t < nCols; ++t)  // K-112c (round 39): each column's title: its track's name and centre
      std::printf("livegrid title %d \"%s\" %.0f %.0f\n", t, playlistTrackName(s, static_cast<size_t>(t)).c_str(),
                  static_cast<double>(column(t) + (colW - 4) * 0.5f), static_cast<double>((y0 + yT1) * 0.5f));
    for (int k = 0; k < rows; ++k)  // K-112c (round 43): every slot row's centre, and the free column's
      std::printf("livegrid row %d y %.1f\n", k, static_cast<double>((livegrid::row(o.y, k, dpp).x0 + livegrid::row(o.y, k, dpp).x1) * 0.5f));
    if (freeX1 - freeX0 >= 40.0f) std::printf("livegrid free x %.1f w %.1f\n", static_cast<double>((freeX0 + freeX1) * 0.5f), static_cast<double>(freeX1 - freeX0));
    for (int t = 0; t < nCols; ++t)
      for (int k = 0; k < nScenes; ++k) {
        const livegrid::Cell q = livegrid::slot(o.x, o.y, t, k, dpp);
        const std::vector<int>& in = slot[static_cast<size_t>(t)][static_cast<size_t>(k)];
        std::printf("  slot %d %d x %.1f y %.1f clip %u\n", t, k, static_cast<double>((q.x0 + q.x1) * 0.5f), static_cast<double>((q.y0 + q.y1) * 0.5f),
                    in.empty() ? 0u : static_cast<unsigned>(s.playlist[static_cast<size_t>(in.front())].id));
      }
    std::fflush(stdout);
  }
  // The stop row under the slots: a stop button per track (lit while the track plays a grid clip),
  // and Stop all under the scenes. K-112c (round 42): always, as a new set's grid in the refs (no
  // live area yet is a grid of empty slots, not a message).
  {
    const float sy = gridBottom, sy1 = livegrid::row(o.y, rows, dpp).x1;  // the rows' own gap above it
    for (int t = 0; t < nCols; ++t) {
      const pixelgrid::Span cs = livegrid::column(o.x, t, dpp);
      const float x = cs.x0, w = cs.w();
      const ImVec2 q0(x, sy), q1(x + w, sy1);
      ImGui::PushID(3000 + t);
      ImGui::SetCursorScreenPos(q0);
      if (ImGui::InvisibleButton("##stoptrack", ImVec2(w, q1.y - q0.y))) a.engine.stopLiveTrack(t);
      const bool hov = ImGui::IsItemHovered();
      ui::Hint("Stop: this track's grid clip stops");
      ImGui::PopID();
      const bool live = a.engine.mode() == PlayMode::Live && a.engine.liveActive(t) != kNoId;
      // K-112c: the stop row is a row of cells like the slots, its square at the left under the
      // slots' squares (the refs line the stop buttons up with the launch column).
      dl->AddRectFilled(q0, q1, hov ? P.surface700 : P.surface800, 0.0f);
      const float cyy = (q0.y + q1.y) * 0.5f;
      dl->AddRectFilled(ImVec2(q0.x + 6, cyy - 4), ImVec2(q0.x + 14, cyy + 4), live || hov ? P.text : P.textDim, 1.0f);
    }
    const float x = mainX, w = pixelgrid::whole(kMainW - 8, dpp);
    const ImVec2 q0(x, sy), q1(x + w, sy1);
    ImGui::SetCursorScreenPos(q0);
    if (ImGui::InvisibleButton("##stopall", ImVec2(w, q1.y - q0.y))) a.engine.stopAllLive();
    const bool hov = ImGui::IsItemHovered();
    ui::Hint("Stop all: every track's grid clip stops");
    // K-112c: Stop all is a ringed square at the left of the scene column's stop cell (no word, as the refs).
    dl->AddRectFilled(q0, q1, hov ? P.surface700 : P.surface800, 0.0f);
    const float cyy = (q0.y + q1.y) * 0.5f;
    dl->AddRectFilled(ImVec2(q0.x + 5, cyy - 5), ImVec2(q0.x + 15, cyy + 5), hov ? P.text : P.textDim, 1.0f);
    dl->AddRect(ImVec2(q0.x + 4.5f, cyy - 5.5f), ImVec2(q0.x + 15.5f, cyy + 5.5f), IM_COL32(16, 18, 22, 200), 1.5f);
    gridBottom = livegrid::rowY(o.y, rows + 1, dpp);
  }
  // K-112c (round 43): the free column, and files or instruments dropped onto the grid. The free
  // column is a quiet panel with its hint, as the refs' (101, 069, 375, empty02); what drops there
  // makes new tracks after the shown ones. A slot (or a title, or the stop cell: the column's first
  // empty slot) puts the files in that track's slots, one per scene from there, through the same
  // drop as the arrangement's (one undo step, collaborators see it). Dropped audio opens nothing.
  {
    const float fy0 = y0, fy1 = livegrid::row(o.y, rows, dpp).x1;
    const bool freeCol = freeX1 - freeX0 >= 40.0f;
    if (freeCol) {
      dl->AddRectFilled(ImVec2(freeX0, fy0), ImVec2(freeX1, fy1), theme::withAlpha(P.surface800, 0.55f), 0.0f);
      const char* ht = i18n::tr("Drop files and instruments here");
      const float wrapW = freeX1 - freeX0 - 16.0f;  // a narrow column wraps it, a wide one keeps one line (101 / empty02)
      const ImVec2 ts = a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, wrapW, ht);
      const float midY = (livegrid::rowY(o.y, 0, dpp) + livegrid::row(o.y, std::min(rows, 8) - 1, dpp).x1) * 0.5f;
      // Each wrapped line centred on its own, as the refs set the hint.
      const char* line = ht;
      const char* htEnd = ht + std::strlen(ht);
      float ly = std::round(midY - ts.y * 0.5f);
      const float lh = a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, "Ag").y;
      // Round 45: a column narrower than the longest word shows no hint (no word is cut in two).
      float widest = 0.0f;
      for (const char* w = ht; w < htEnd;) {
        const char* we = w;
        while (we < htEnd && *we != ' ') ++we;
        widest = std::max(widest, a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, w, we).x);
        w = we < htEnd ? we + 1 : we;
      }
      if (widest > wrapW) line = htEnd;
      while (line < htEnd) {
        const char* end = a.fontRegular->CalcWordWrapPosition(theme::kFontCaption, line, htEnd, wrapW);
        if (end <= line) end = htEnd;
        const float lw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, line, end).x;
        dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round((freeX0 + freeX1 - lw) * 0.5f), ly), P.textFaint, line, end);
        ly += lh + 2;
        line = end;
        while (*line == ' ') ++line;
      }
    }
    const float dx1 = freeCol ? freeX1 : column(nCols) - kGap;
    if (nCols > 0 && dx1 > column(0) && ImGui::BeginDragDropTargetCustom(ImRect(column(0), fy0, dx1, fy1), ImGui::GetID("##griddrop"))) {
      int tt = -1;
      for (int t = 0; t < nCols && tt < 0; ++t) {
        const pixelgrid::Span cs = livegrid::column(o.x, t, dpp);
        if (m.x >= cs.x0 && m.x < cs.x1 + kGap) tt = t;
      }
      const bool inFree = tt < 0 && freeCol && m.x >= freeX0;
      int tk = -1;
      for (int k = 0; k < rows && tk < 0; ++k) {
        const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
        if (m.y >= rs.x0 - kGap && m.y < rs.x1) tk = k;
      }
      if (tt >= 0 && tk < 0) {  // the title or the stop cell: the column's first empty slot
        tk = 0;
        while (tk < nScenes && !slot[static_cast<size_t>(tt)][static_cast<size_t>(tk)].empty()) ++tk;
      }
      if (inFree) tt = nCols, tk = std::max(tk, 0);
      if (tt >= 0) {
        browserui::DropPlace place;
        place.track = tt;
        place.tick = static_cast<Tick>(tk) * sceneT;
        place.slotTicks = sceneT;
        place.newTracks = inFree;
        browserui::DropFeedback fb;
        fb.highlight = false;
        browserui::acceptFileDrop(a, browse::DropTarget::Playlist, place, &fb);
        browserui::DropFeedback pfb;
        pfb.highlight = false;
        browserui::acceptPluginDrop(a, browse::DropTarget::Playlist, place, &pfb);
        // Where it lands: the slots the files go to (in the free column, the new tracks' first
        // columns), or the whole column an instrument goes to.
        const float ri = pixelgrid::strokeIn(2.0f);
        dl->PushClipRect(ImVec2(column(0), fy0), ImVec2(std::max(dx1, column(0) + 1), fy1), true);
        if (const browserui::HoverPlan* hp = fb.hovering ? browserui::hoverPlan(browse::DropTarget::Playlist) : nullptr) {
          for (const xdrop::Step& sp : hp->plan.steps) {
            if (sp.action != browse::DropAction::AudioClip && sp.action != browse::DropAction::MidiPatternClip) continue;
            const int k = static_cast<int>(sp.tick / sceneT);
            const livegrid::Cell q = livegrid::slot(o.x, o.y, sp.track, k, dpp);
            const ImU32 col = theme::rgb(sp.action == browse::DropAction::AudioClip ? theme::role::kPaint : theme::role::kDraw);
            dl->AddRectFilled(ImVec2(q.x0, q.y0), ImVec2(q.x1, q.y1), theme::withAlpha(col, 0.28f), 0.0f);
            dl->AddRect(ImVec2(q.x0 + ri, q.y0 + ri), ImVec2(q.x1 - ri, q.y1 - ri), theme::withAlpha(col, 0.95f), 0.0f, 0, 2.0f);
            if (sp.item < hp->items.size())
              theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(q.x0 + 19, theme::textTop(a.fontRegular, theme::kFontCaption, (q.y0 + q.y1) * 0.5f, 0)),
                                  q.x1 - q.x0 - 24, P.text, xdrop::displayName(hp->items[sp.item]).c_str());
          }
        } else if (pfb.hovering) {
          const pixelgrid::Span cs = livegrid::column(o.x, tt, dpp);
          const ImU32 hot = theme::rgb(theme::role::kDraw);
          dl->AddRectFilled(ImVec2(cs.x0, fy0), ImVec2(cs.x1, fy1), theme::withAlpha(hot, 0.16f));
          dl->AddRect(ImVec2(cs.x0 + ri, fy0 + ri), ImVec2(cs.x1 - ri, fy1 - ri), theme::withAlpha(hot, 0.95f), 0.0f, 0, 2.0f);
        }
        dl->PopClipRect();
      }
      ImGui::EndDragDropTarget();
    }
  }
  // K-287: a slot's clips dragged onto an empty slot of the live area move there (that track, that
  // scene, the same place inside the scene): one undo step, as a move in the arrangement.
  if (dragFrom.first >= 0) {
    const auto [ft, fk] = dragFrom;
    int tt = -1, tk = -1;
    for (int t = 0; t < nCols && tt < 0; ++t) {
      const pixelgrid::Span cs = livegrid::column(o.x, t, dpp);
      if (m.x >= cs.x0 && m.x < cs.x1) tt = t;
    }
    for (int k = 0; k < nScenes && tk < 0; ++k) {
      const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
      if (m.y >= rs.x0 && m.y < rs.x1) tk = k;
    }
    const bool srcOk = ft < nCols && fk < nScenes && !slot[static_cast<size_t>(ft)][static_cast<size_t>(fk)].empty();
    bool ok = srcOk && tt >= 0 && tk >= 0 && (tt != ft || tk != fk) && slot[static_cast<size_t>(tt)][static_cast<size_t>(tk)].empty() &&
              !s.tracks[static_cast<size_t>(tt)].locked && !s.tracks[static_cast<size_t>(ft)].locked;
    if (ok)
      for (int ci : slot[static_cast<size_t>(ft)][static_cast<size_t>(fk)]) ok = ok && trackmodes::clipAllowed(s, s.playlist[static_cast<size_t>(ci)].kind, tt);
    const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (dragging && ok) {  // the slot it would land in
        const livegrid::Cell q = livegrid::slot(o.x, o.y, tt, tk, dpp);
        const float ri = pixelgrid::strokeIn(2.0f);
        dl->AddRect(ImVec2(q.x0 + ri, q.y0 + ri), ImVec2(q.x1 - ri, q.y1 - ri), P.hot, 0.0f, 0, 2.0f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      }
    } else {
      if (ok && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= 16.0f) {
        std::set<ObjectId> ids;
        for (int ci : slot[static_cast<size_t>(ft)][static_cast<size_t>(fk)]) ids.insert(s.playlist[static_cast<size_t>(ci)].id);
        const Tick dt = static_cast<Tick>(tk - fk) * sceneT;
        a.doc.editClips("move clips", [&](std::vector<Clip>& cs) {
          for (Clip& c : cs)
            if (ids.count(c.id)) c.start += dt, c.track = tt;
        });
      }
      dragFrom = {-1, -1};
    }
  }
  // K-112c (round 39): a title dragged along the row moves its track, as dragging an arrangement
  // header does (the same document edit: the arrangement's rows follow, and collaborators get it).
  if (titleDrag >= nCols) titleDrag = -1;
  if (titleDrag >= 0) {
    const int before = std::clamp(static_cast<int>(std::floor((m.x - (o.x + kPad)) / colW + 0.5f)), 0, nCols);
    const float lx = before < nCols ? column(before) - 3 : column(nCols - 1) + colW - 3;
    const pixelgrid::Span ls = pixelgrid::line(lx, dpp);
    dl->AddRectFilled(ImVec2(ls.x0 - 1, y0), ImVec2(ls.x1 + 1, yT1), P.hot);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (before != titleDrag && before != titleDrag + 1) {
        std::vector<int> now;
        if (trackorder::movePlaylistTracks(a.doc, {titleDrag}, before, &now) && !now.empty()) a.playlist.trackSel = {now.front()};
      }
      titleDrag = -1;
    }
  }
  if (held != kNoId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    a.engine.releaseClip(held);
    held = kNoId;
  }
  // ---- mixer controls under the columns (the linked mixer track of each column) ----
  const float my0 = std::max(gridBottom + 8, o.y + av.y - kPad - mixH);
  if (footMixer) dl->AddLine(ImVec2(o.x + kPad, my0 - 4), ImVec2(mainX + kMainW - 8, my0 - 4), P.line);
  const float mh = footMixer ? std::max(100.0f, o.y + av.y - kPad - my0) : 0.0f;
  const StripRow srow{my0, mh, anySends(s) && mh >= 165.0f};  // room for the sends row, the fields and the pan above the buttons  // room for the row, the fields and the pan above the buttons
  auto strip = [&](int t, const MixerTrack* mx, float x, float w, bool master, char letter = 0) {
    if (footMixer) sessionStrip(a, dl, srow, t, mx, x, w, master, letter);
  };
  for (int t = 0; t < nCols; ++t) strip(t, trackmodes::playedMixer(s, t), column(t), colW - 4, false);
  // K-112c (round 19): the return tracks as strips between the tracks and Main, each titled in the
  // grid's title row with its letter and name and no slots, as every ref's mixer (143, 141, 280, 093).
  // (Where they stand: retShift / nFit above.)
  int nRet = 0;
  {
    const std::vector<size_t> rets = returnTracks(s);
    for (size_t i = 0; i < rets.size() && i < 26; ++i) {
      const pixelgrid::Span cs = retColumn(static_cast<int>(i));  // K-276: as a track's title
      const float x = cs.x0;
      if (static_cast<int>(i) >= nFit) break;  // only the returns that fit before Main
      const MixerTrack& rt = s.mixer[rets[i]];
      const char lt = static_cast<char>('A' + i);
      dl->AddRectFilled(ImVec2(x, y0), ImVec2(cs.x1, yT1), returnFill(), 0.0f);
      const std::string nm = std::string(1, lt) + " " + rt.name;
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x + 6, theme::textTop(a.fontRegular, fs, y0, titleH)), cs.w() - 10, theme::contrastText(returnFill()), nm.c_str());
      ImGui::SetCursorScreenPos(ImVec2(x, y0));
      ImGui::PushID(static_cast<int>(8100 + i));
      if (ImGui::InvisibleButton("##rtitle", ImVec2(cs.w(), titleH))) a.selMixer = rt.id;
      ImGui::PopID();
      ui::Hint("Return track: other tracks send to it (click to select it in the mixer)");
      strip(-1, &rt, x, colW - 4, false, lt);
      ++nRet;
    }
  }
  // K-112c: a 1 px divider between strips (and before Main), as the refs' mixers draw them.
  for (int t = 1; footMixer && t <= nCols + nRet; ++t) {
    // A track's left edge, a return's (shifted) left edge, or Main's; plus the last track's right edge
    // when the returns stand apart from it.
    const float ex = t == nCols + nRet ? mainX : t >= nCols ? retColumn(t - nCols).x0 : column(t);
    const pixelgrid::Span lx = pixelgrid::line(ex - 3, dpp);  // one crisp device column
    dl->AddRectFilled(ImVec2(lx.x0, my0 + 2), ImVec2(lx.x1, my0 + mh - 2), P.line);
    if (t == nCols && nRet > 0 && retShift > 0.0f) {
      const pixelgrid::Span lt = pixelgrid::line(column(nCols) - 3, dpp);
      dl->AddRectFilled(ImVec2(lt.x0, my0 + 2), ImVec2(lt.x1, my0 + mh - 2), P.line);
    }
  }
  if (!s.mixer.empty()) strip(-1, &s.mixer[0], mainX, kMainW - 8, true);
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(mainX + kMainW - o.x + kPad - 4, std::max(av.y - 2, my0 + mh - o.y)));
  ImGui::EndChild();
}

void drawMixerBand(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImGui::BeginChild("##clipsmixer", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float dpp = pixelgrid::frameScale();
  const ImVec2 o(livegrid::snap(ImGui::GetCursorScreenPos().x, dpp), livegrid::snap(ImGui::GetCursorScreenPos().y, dpp));
  const ImVec2 av = ImGui::GetContentRegionAvail();
  const int nCols = gridColumns(s);
  using livegrid::kPad, livegrid::kGap;
  constexpr float kMainW = 140, colW = livegrid::kColW;
  const float fs = theme::kFontCaption + 1;
  // The titles, then the strips: the clip grid's columns, returns beside Main at the right (357).
  const float y0 = livegrid::titleY(o.y, dpp), yT1 = livegrid::titleBottom(o.y, dpp), titleH = yT1 - y0;
  auto column = [&](int t) { return livegrid::column(o.x, t, dpp); };
  const float mainX = std::max(column(nCols).x0 + 4, livegrid::snap(o.x + av.x - kPad - kMainW + 4, dpp));
  const float my0 = livegrid::snap(yT1 + kGap + 2, dpp);
  const float mh = std::max(100.0f, o.y + av.y - kPad - my0);
  const StripRow srow{my0, mh, anySends(s) && mh >= 165.0f};
  // A title: the fill and name of a column (a click selects its mixer track; a selected one is ringed).
  auto title = [&](float x0, float x1, ImU32 fill, const std::string& name, const MixerTrack* mx, int id) {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, yT1), fill, 0.0f);
    if (mx && a.selMixer == mx->id) {
      const float in = pixelgrid::strokeIn(1.5f);
      dl->AddRect(ImVec2(x0 + in, y0 + in), ImVec2(x1 - in, yT1 - in), P.hot, 0.0f, 0, 1.5f);
    }
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0 + 6, theme::textTop(a.fontRegular, fs, y0, titleH)), x1 - x0 - 10,
                        theme::contrastText(fill), name.c_str());
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::PushID(id);
    if (ImGui::InvisibleButton("##mtitle", ImVec2(x1 - x0, titleH)) && mx) a.selMixer = mx->id;
    ImGui::PopID();
    if (mx) ui::Hint("Click to select this mixer track");
  };
  for (int t = 0; t < nCols; ++t) {
    const pixelgrid::Span cs = column(t);
    const MixerTrack* mx = trackmodes::playedMixer(s, t);
    title(cs.x0, cs.x1, theme::content(s.tracks[static_cast<size_t>(t)].color), playlistTrackName(s, static_cast<size_t>(t)), mx, 3000 + t);
    sessionStrip(a, dl, srow, t, mx, cs.x0, colW - 4, false);
  }
  // The returns that fit before Main, right-aligned against it (as the clip grid, round 36).
  const std::vector<size_t> rets = returnTracks(s);
  int nFit = 0;
  while (nFit < static_cast<int>(rets.size()) && nFit < 26 && column(nCols + nFit).x0 + colW - 4 <= mainX - 6) ++nFit;
  const float retShift = nFit > 0 ? std::max(0.0f, livegrid::snap(mainX - column(nCols + nFit).x0, dpp)) : 0.0f;
  for (int i = 0; i < nFit; ++i) {
    const pixelgrid::Span cs = livegrid::column(o.x + retShift, nCols + i, dpp);
    const MixerTrack& rt = s.mixer[rets[static_cast<size_t>(i)]];
    const char lt = static_cast<char>('A' + i);
    title(cs.x0, cs.x1, returnFill(), std::string(1, lt) + " " + rt.name, &rt, 3500 + i);
    sessionStrip(a, dl, srow, -1, &rt, cs.x0, colW - 4, false, lt);
  }
  // 1 px dividers between the strips, as the clip grid's foot.
  auto divider = [&](float ex) {
    const pixelgrid::Span lx = pixelgrid::line(ex - 3, dpp);
    dl->AddRectFilled(ImVec2(lx.x0, my0 + 2), ImVec2(lx.x1, my0 + mh - 2), P.line);
  };
  for (int t = 1; t <= nCols; ++t) divider(column(t).x0);
  for (int i = 1; i < nFit; ++i) divider(livegrid::column(o.x + retShift, nCols + i, dpp).x0);
  if (nFit > 0 && retShift > 0.0f) divider(livegrid::column(o.x + retShift, nCols, dpp).x0);
  divider(mainX);
  if (!s.mixer.empty()) {
    const pixelgrid::Span ms{mainX, livegrid::snap(mainX + kMainW - 8, dpp)};
    title(ms.x0, ms.x1, mainFill(), i18n::tr("Main"), &s.mixer[0], 3999);
    sessionStrip(a, dl, srow, -1, &s.mixer[0], mainX, kMainW - 8, true);
  }
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(mainX + kMainW - o.x + kPad - 4, std::max(av.y - 2, my0 + mh - o.y)));
  ImGui::EndChild();
}

namespace {
bool gCentreAsked = false;
}

void devCentreAsk() { gCentreAsked = true; }

void devCentreFrame(App& a) {
  if (!gCentreAsked) return;
  gCentreAsked = false;
  const struct { const char* name; ImFont* f; } fonts[] = {{"regular", a.fontRegular}, {"bold", a.fontBold}, {"mono", a.fontMono}};
  std::printf("CENTRE BEGIN\n");
  for (const auto& fo : fonts)
    for (float sz : {theme::kFontLabel, theme::kFontCaption, theme::kFontBody}) {
      ImFontBaked* b = fo.f ? fo.f->GetFontBaked(sz) : nullptr;
      const ImFontGlyph* cap = b ? b->FindGlyph('H') : nullptr;
      if (!cap) continue;
      for (float h : {15.0f, 16.0f, 18.0f, 20.0f, 22.0f, 24.0f}) {
        const float older = std::round(0.0f + (h - sz) * 0.5f) - 1, plain = std::round((h - sz) * 0.5f), now = theme::textTop(fo.f, sz, 0.0f, h);
        const float mid = (cap->Y0 + cap->Y1) * 0.5f;  // the cap band's middle from the pen
        std::printf("CENTRE %s %.0f box %.0f cap %.1f..%.1f older %+.1f plain %+.1f now %+.1f\n", fo.name, sz, h, cap->Y0, cap->Y1,
                    older + mid - h * 0.5f, plain + mid - h * 0.5f, now + mid - h * 0.5f);
      }
    }
  std::printf("CENTRE END\n");
  std::fflush(stdout);
}

}  // namespace gl::livelook

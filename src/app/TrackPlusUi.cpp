// Gridline - K-12: the playlist track "+". See TrackPlusUi.h.
#include "TrackPlusUi.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/PluginInfo.h"
#include "../core/TrackModes.h"
#include "../core/TrackPlus.h"
#include "App.h"
#include "ContextMenus.h"
#include "PluginFavsUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::trackplusui {

using theme::pal;

namespace {

int gTrack = -1;      // track of the open menu
Tick gAt = 0;         // where a new pattern goes
ImVec2 gAnchor{0, 0};
bool gAppearing = false;
DevGeom gGeom;

const char* kSwitch = "ui.playlistTrackPlus";
const char* kStart = "ui.trackPlusPattern";

void row(const char* key) {
  const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
  gGeom.rows.push_back({key, (r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f});
}

// After an assignment / a new pattern: the pattern selected and open in the
// piano roll on the track's channel, else the channel selected.
void finish(App& a, const trackplus::Started& st, const std::string& why, bool newInstrument = false) {
  if (st.channel == kNoId) {
    if (!why.empty()) a.notify(why, true);
    return;
  }
  if (st.pattern != kNoId) {
    a.selectPattern(st.pattern);
    a.openPianoRoll(st.channel);
  } else {
    a.selectChannel(st.channel);
  }
  if (newInstrument) a.showNewInstrument(st.channel);  // K-125: in front of the piano roll
}

// The instrument of an instrument track replaced on its channel (one undo
// step); a track named after the old instrument takes the new name.
void replaceInstrument(App& a, int track, ObjectId ch, const std::string& type) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(ch);
  if (!c || c->plugin.type == type) return;
  const std::string oldName = c->name;
  const bool follow = track >= 0 && track < static_cast<int>(s.tracks.size()) && s.tracks[static_cast<size_t>(track)].name == oldName;
  a.doc.beginGroup("change track instrument");
  const bool ok = a.doc.setChannelPlugin(ch, type);
  if (ok && follow) {
    const Channel* nc = a.doc.song().channel(ch);
    const std::string nn = nc ? nc->name : oldName;
    if (nn != oldName) a.doc.editPlaylistTrack(track, "rename playlist track", [nn](PlaylistTrack& t) { t.name = nn; });
  }
  a.doc.endGroup();
  if (ok) {
    pui::pluginReplaced(PluginTarget{ch, -1});
    a.showNewInstrument(ch);  // K-125
  }
}

// The house menus' left gutter (check marks / icons) before the labels.
// (Selectable rows draw from their rect, which reaches half the item spacing left of the cursor.)
float gutterW() { return static_cast<float>(ImGui::GetCurrentWindow()->DC.MenuColumns.OffsetLabel) + std::floor(ImGui::GetStyle().ItemSpacing.x * 0.5f); }

// Favourite instruments, one row each (the star toggles, a click picks).
void favouriteRows(const std::string& current, const std::function<void(const std::string&)>& pick) {
  const auto favs = favui::favouritesOfKind(PluginKind::Generator);
  const float gutter = gutterW();  // on the menu items' label column
  ImGui::Indent(gutter);
  if (favs.empty()) ImGui::TextDisabled("%s", i18n::tr("Starred instruments are listed here"));
  for (const PluginInfo* p : favs) {
    const bool picked = favui::pickerRow(*p, p->type == current, isHostedType(p->type) ? nullptr : p->category.c_str());
    row(("fav:" + p->type).c_str());
    if (picked) pick(p->type);
  }
  ImGui::Unindent(gutter);
}

// Rack channels the track can play (generators not linked elsewhere).
void channelRows(App& a, int track, ObjectId current, const std::function<void(ObjectId)>& pick) {
  const Song& s = a.doc.song();
  int listed = 0;
  for (const auto& c : s.channels) {
    const bool cur = c.id == current;
    if (!cur && !trackmodes::channelLinkable(s, c.id, nullptr, track)) continue;
    ++listed;
    ImGui::PushID(static_cast<int>(c.id));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float lh = ImGui::GetTextLineHeight();
    // A swatch in the channel's colour in the menu's gutter (the track's own channel: dimmed, not pickable).
    const bool hit = ImGui::MenuItem(c.name.c_str(), nullptr, false, !cur);
    row(("chan:" + std::to_string(c.id)).c_str());
    const float sw = std::round(lh * 0.5f);
    const ImVec2 sc(std::round(at.x + std::max(sw, gutterW() * 0.5f)), std::round(at.y + lh * 0.5f + 1));
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(sc.x - sw * 0.5f, sc.y - sw * 0.5f), ImVec2(sc.x + sw * 0.5f, sc.y + sw * 0.5f),
                                              theme::content(c.color), theme::kRadiusSm);
    if (hit) pick(c.id);
    ImGui::PopID();
  }
  if (listed == 0) {
    ImGui::BeginDisabled();
    ImGui::MenuItem(i18n::tr("No free instrument channel"));
    ImGui::EndDisabled();
  }
}

// The menu's title: the track's colour and name.
void title(const Song& s, int track) {
  const auto& P = pal();
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  const float lh = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const ImU32 col = t.color != PlaylistTrack{}.color ? theme::content(t.color) : theme::rgb(theme::role::kStamp);
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + 2), ImVec2(at.x + 3, at.y + lh - 2), col, theme::kRadiusSm);
  ImGui::SetCursorScreenPos(ImVec2(at.x + theme::kS2 + 2, at.y));
  ImGui::TextColored(theme::vec(P.text), "%s", playlistTrackName(s, static_cast<size_t>(track)).c_str());
}

// A switch row (the menu stays open): the label, a lamp at the right end.
bool switchRow(const char* label, bool on) {
  const auto& P = pal();
  const float lh = ImGui::GetTextLineHeight();
  const float lampW = std::round(lh + theme::kS2);
  const float gutter = gutterW();
  const float minW = gutter + ImGui::CalcTextSize(label).x + lampW + theme::kS2;
  const bool clicked = ImGui::Selectable("##switch", false, ImGuiSelectableFlags_SpanAvailWidth, ImVec2(minW, 0));
  const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(r0.x + gutter, std::round((r0.y + r1.y - lh) * 0.5f)), on ? P.text : P.textDim, label);
  ui::Led(dl, ImVec2(std::round(r1.x - lampW * 0.5f), std::round((r0.y + r1.y) * 0.5f)), 3.5f, on);
  return clicked;
}

}  // namespace

bool enabled(const App& a) { return a.settings.getBool(kSwitch); }
void setEnabled(App& a, bool on) { a.settings.set(kSwitch, on); }

bool offeredOn(const Song& s, int track) {
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return false;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  return !t.locked && trackmodes::modeOf(s, track) != TrackMode::Audio;
}

void drawButton(App& a, ImDrawList* dl, int track, ImVec2 c, float alpha, bool hot) {
  (void)a;
  if (alpha <= 0.01f) return;
  const auto& P = pal();
  const bool open = gTrack == track && ImGui::IsPopupOpen("##trackplus");
  const ImVec2 b0(c.x - kHalf, c.y - kHalf), b1(c.x + kHalf, c.y + kHalf);
  const ImU32 well = hot || open ? theme::mix(P.surface700, P.hot, 0.18f) : theme::mix(P.ink, P.surface800, 0.45f);
  dl->AddRectFilled(b0, b1, theme::withAlpha(well, alpha), theme::kRadiusSm);
  dl->AddRect(b0, b1, theme::withAlpha(hot || open ? P.hot : P.line, alpha * (hot || open ? 0.9f : 0.7f)), theme::kRadiusSm, 0, 1.0f);
  ui::drawIconFitC(dl, ui::Icon::Plus, c, ImVec2(7, 7), theme::withAlpha(hot || open ? P.hot : P.textDim, alpha));
}

void open(App& a, int track, Tick at, ImVec2 anchor) {
  (void)a;
  gTrack = track;
  gAt = at;
  gAnchor = anchor;
  gAppearing = true;
  ImGui::OpenPopup("##trackplus");
}

int menuTrack() { return ImGui::IsPopupOpen("##trackplus") ? gTrack : -1; }

DevGeom& devGeom() { return gGeom; }

void menu(App& a) {
  gGeom.rows.clear();
  gGeom.menu = -1;
  if (gAppearing) {
    ImGui::SetNextWindowPos(gAnchor, ImGuiCond_Always);
    gAppearing = false;
  }
  if (!ImGui::BeginPopup("##trackplus")) return;
  const Song& s = a.doc.song();
  const int track = gTrack;
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  gGeom.menu = track;
  const TrackMode mode = trackmodes::modeOf(s, track);
  const Channel* ch = mode == TrackMode::Instrument ? trackmodes::linkedChannel(s, track) : nullptr;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kS2, theme::kS1 + 2));
  title(s, track);
  std::string why;
  if (ch) {
    const ObjectId cid = ch->id;
    const std::string ctype = ch->plugin.type;
    if (ImGui::MenuItem(i18n::tr("New pattern on this track"))) {
      const trackplus::Started st = trackplus::newPatternOnTrack(a.doc, track, gAt, &why);
      if (st.clip == kNoId && !why.empty()) a.notify(why, true);
      finish(a, st, "");
    }
    row("newpattern");
    ui::Hint("A new pattern for this instrument, placed on the track at the song position (or the view's left edge) and opened in the piano roll");
    if (ImGui::MenuItem(i18n::tr("Show the instrument"))) {
      a.selectChannel(cid);
      a.settingsChannel = cid;
    }
    row("showinst");
    ImGui::SeparatorText(i18n::tr("Change instrument"));
    favouriteRows(ctype, [&](const std::string& t) { replaceInstrument(a, track, cid, t); });
    if (ImGui::BeginMenu(i18n::tr("All instruments"))) {
      menus::instrumentItems([&](const std::string& t) { replaceInstrument(a, track, cid, t); }, ctype);
      ImGui::EndMenu();
    }
    row("all");
    if (ImGui::BeginMenu(i18n::tr("Play another rack channel"))) {
      channelRows(a, track, cid, [&](ObjectId c) { finish(a, trackplus::assignChannel(a.doc, track, c, false, gAt, &why), why); });
      ImGui::EndMenu();
    }
    row("rack");
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Make it an ordinary track"))) trackmodes::unassign(a.doc, track);
    row("unlink");
    ui::Hint("The track stops playing this instrument (its clips and the channel stay)");
  } else {
    const bool startPat = a.settings.getBool(kStart);
    ImGui::SeparatorText(i18n::tr("Give it an instrument"));
    favouriteRows("", [&](const std::string& t) { finish(a, trackplus::assignNewInstrument(a.doc, track, t, startPat, gAt, &why), why, true); });
    if (ImGui::BeginMenu(i18n::tr("All instruments"))) {
      menus::instrumentItems([&](const std::string& t) { finish(a, trackplus::assignNewInstrument(a.doc, track, t, startPat, gAt, &why), why, true); });
      ImGui::EndMenu();
    }
    row("all");
    if (ImGui::BeginMenu(i18n::tr("A rack channel"))) {
      channelRows(a, track, kNoId, [&](ObjectId c) { finish(a, trackplus::assignChannel(a.doc, track, c, startPat, gAt, &why), why); });
      ImGui::EndMenu();
    }
    row("rack");
    ImGui::Separator();
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    if (switchRow(i18n::tr("Start a pattern with it"), startPat)) a.settings.set(kStart, !startPat);
    ImGui::PopItemFlag();
    row("startpattern");
    ui::Hint("On an empty track, the instrument comes with a first pattern on the track, opened in the piano roll");
  }
  ImGui::PopStyleVar();
  ImGui::EndPopup();
}

}  // namespace gl::trackplusui

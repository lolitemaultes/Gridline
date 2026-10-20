// Gridline — arrangements menu / selector and live mode (live pads window,
// per-track live settings).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../core/I18n.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/Arrangements.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl {

namespace {

// Name field inside a popup/menu: Enter applies.
bool nameField(const char* id, const std::string& current, std::string& out) {
  static char buf[128];
  ImGui::PushID(id);
  if (ImGui::IsWindowAppearing()) std::snprintf(buf, sizeof buf, "%s", current.c_str());
  ImGui::SetNextItemWidth(200);
  bool ok = ImGui::InputText("##name", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
  ImGui::PopID();
  ImGui::TextDisabled("%s", i18n::tr("Enter to apply"));
  if (ok && buf[0]) out = buf;
  return ok && buf[0];
}

void switchTo(App& a, int index) {
  if (index == a.doc.song().currentArrangement && !a.doc.song().arrangements.empty()) return;
  a.engine.stopAllLive();
  a.doc.editSong("switch arrangement", [index](Song& s) { selectArrangement(s, index); });
  a.playlist.selected.clear();
}

}  // namespace

// ---------------------------------------------------------------------------
// Arrangements

void arrangementMenu(App& a) {
  const Song& s = a.doc.song();
  const int n = arrangementCount(s);
  const int cur = s.arrangements.empty() ? 0 : s.currentArrangement;
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i);
    std::string label = arrangementName(s, i);
    if (ImGui::MenuItem(label.c_str(), nullptr, i == cur)) switchTo(a, i);
    ImGui::PopID();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("New arrangement"))) {
    a.engine.stopAllLive();
    a.doc.editSong("new arrangement", [](Song& x) { selectArrangement(x, addArrangement(x, "", false)); });
    a.playlist.selected.clear();
  }
  if (ImGui::MenuItem(i18n::tr("Duplicate arrangement"))) {
    a.engine.stopAllLive();
    std::string nm = uniqueArrangementName(s, arrangementName(s, cur));
    a.doc.editSong("duplicate arrangement", [nm](Song& x) { selectArrangement(x, addArrangement(x, nm, true)); });
    a.playlist.selected.clear();
  }
  if (ImGui::BeginMenu(i18n::tr("Rename arrangement"))) {
    std::string nm;
    if (nameField("arrname", arrangementName(s, cur), nm)) {
      a.doc.editSong("rename arrangement", [cur, nm](Song& x) { renameArrangement(x, cur, nm); });
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Delete arrangement"), nullptr, false, n > 1)) {
    a.engine.stopAllLive();
    a.doc.editSong("delete arrangement", [cur](Song& x) { removeArrangement(x, cur); });
    a.playlist.selected.clear();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Previous arrangement"), nullptr, false, cur > 0)) switchTo(a, cur - 1);
  if (ImGui::MenuItem(i18n::tr("Next arrangement"), nullptr, false, cur + 1 < n)) switchTo(a, cur + 1);
}

void arrangementSelector(App& a, float width) {
  const Song& s = a.doc.song();
  const int cur = s.arrangements.empty() ? 0 : s.currentArrangement;
  std::string preview = arrangementName(s, cur);
  if (ui::BeginCombo("##arrangement", preview.c_str(), width)) {
    arrangementMenu(a);
    ImGui::EndCombo();
  }
  ui::Hint("Arrangement: the playlist being edited and played \xC2\xB7 new, duplicate, rename, delete");
}

// ---------------------------------------------------------------------------
// Live mode

void liveTrackMenu(App& a, int track) {
  const Song& s = a.doc.song();
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  auto edit = [&](const char* label, auto fn) { a.doc.editPlaylistTrack(track, label, fn); };
  ImGui::SeparatorText(i18n::tr("Pad press"));
  for (int i = 0; i < static_cast<int>(PadMode::Count); ++i)
    if (ImGui::MenuItem(padModeName(static_cast<PadMode>(i)), nullptr, t.pad == static_cast<PadMode>(i)))
      edit("change pad mode", [i](PlaylistTrack& x) { x.pad = static_cast<PadMode>(i); });
  ImGui::SeparatorText(i18n::tr("When the clip ends"));
  for (int i = 0; i < static_cast<int>(FollowMode::Count); ++i)
    if (ImGui::MenuItem(followModeName(static_cast<FollowMode>(i)), nullptr, t.follow == static_cast<FollowMode>(i)))
      edit("change clip end action", [i](PlaylistTrack& x) { x.follow = static_cast<FollowMode>(i); });
  if (ImGui::BeginMenu(i18n::tr("Launch on"))) {
    for (int i = 0; i < static_cast<int>(LaunchGrid::Count); ++i)
      if (ImGui::MenuItem(launchGridName(static_cast<LaunchGrid>(i)), nullptr, t.launch == static_cast<LaunchGrid>(i)))
        edit("change launch grid", [i](PlaylistTrack& x) { x.launch = static_cast<LaunchGrid>(i); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Keep in phase"))) {
    if (ImGui::MenuItem(i18n::tr("Off (start from the clip start)"), nullptr, t.syncStart == LaunchGrid::Now))
      edit("change phase", [](PlaylistTrack& x) { x.syncStart = LaunchGrid::Now; });
    for (int i = 1; i < static_cast<int>(LaunchGrid::Count); ++i) {
      std::string label = std::string("With ") + launchGridName(static_cast<LaunchGrid>(i)) + " grid";
      if (ImGui::MenuItem(label.c_str(), nullptr, t.syncStart == static_cast<LaunchGrid>(i)))
        edit("change phase", [i](PlaylistTrack& x) { x.syncStart = static_cast<LaunchGrid>(i); });
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Stop this track"), nullptr, false, a.engine.liveActive(track) != kNoId)) a.engine.stopLiveTrack(track);
}

void setLiveArea(App& a, Tick end) {
  a.doc.editSong(end > 0 ? "set live area" : "remove live area", [end](Song& s) { s.live.areaEnd = std::max<Tick>(0, end); });
}

void drawLivePads(App& a) {
  if (!a.showLivePads) return;
  if (!ui::BeginDockWindow("Live pads", &a.showLivePads, ImVec2(560, 300))) {
    ui::EndDockWindow();
    return;
  }
  const auto& P = theme::pal();
  const Song& s = a.doc.song();
  const bool live = a.engine.mode() == PlayMode::Live;
  ui::BeginToolbar("##livetb");
  {
    if (ui::TextButton(live ? "Live mode on" : "Live mode off", live, 0,
                       "Live mode: the transport runs freely and plays only the clips you launch"))
      a.engine.setMode(live ? PlayMode::Song : PlayMode::Live);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Stop all", false, 0, "Stop every track at its launch grid")) a.engine.stopAllLive();
    ImGui::SameLine(0, theme::kS2);
    arrangementSelector(a, 150);
  }
  ui::EndToolbar();
  if (s.live.areaEnd <= 0) {
    ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 80.0f)), "No live area",
                   "Playlist menu \xE2\x86\x92 Live area: clips placed before its end become pads");
    ImGui::Dummy(av);
    ui::EndDockWindow();
    return;
  }
  // Rows: playlist tracks with live clips; columns: their clips by position.
  ImGui::BeginChild("##pads");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rowH = 46.0f, nameW = 120.0f, padW = 92.0f;
  static ObjectId held = kNoId;
  const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 8.0f);
  for (int tr = 0; tr < static_cast<int>(s.tracks.size()); ++tr) {
    std::vector<int> clips = liveClips(s, tr);
    if (clips.empty()) continue;
    ImGui::PushID(tr);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    dl->AddRectFilled(p, ImVec2(p.x + nameW - 6, p.y + rowH - 6), P.surface800, theme::kRadiusSm);
    dl->AddRectFilled(p, ImVec2(p.x + 4, p.y + rowH - 6), theme::rgb(t.color), theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(p.x + 10, p.y + 6), nameW - 40, P.text,
                        playlistTrackName(s, static_cast<size_t>(tr)).c_str());
    std::string mode = std::string(padModeName(t.pad)) + " \xC2\xB7 " + followModeName(t.follow);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontLabel, ImVec2(p.x + 10, p.y + 24), nameW - 20, P.textFaint, mode.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x + nameW - 30, p.y + 4));
    if (ui::IconButton("stop", ui::Icon::Stop, false, 20, "Stop this track")) a.engine.stopLiveTrack(tr);
    if (ImGui::BeginPopupContextItem("##trlive")) {
      liveTrackMenu(a, tr);
      ImGui::EndPopup();
    }
    const ObjectId act = a.engine.liveActive(tr), que = a.engine.liveQueued(tr);
    for (size_t k = 0; k < clips.size(); ++k) {
      const Clip& c = s.playlist[static_cast<size_t>(clips[k])];
      ImGui::PushID(static_cast<int>(c.id));
      ImVec2 q(p.x + nameW + static_cast<float>(k) * padW, p.y);
      ImGui::SetCursorScreenPos(q);
      ImGui::InvisibleButton("##pad", ImVec2(padW - 6, rowH - 6));
      if (ImGui::IsItemActivated()) {
        if (!live) a.engine.setMode(PlayMode::Live);
        a.engine.launchClip(c.id);
        held = c.id;
      }
      bool hov = ImGui::IsItemHovered();
      Rgb rgb = 0x50565E;
      std::string nm;
      if (c.kind == ClipKind::Pattern) {
        if (const Pattern* pt = s.pattern(c.pattern)) rgb = pt->color, nm = pt->name;
      } else if (c.kind == ClipKind::Audio) {
        if (const Channel* ch = s.channel(c.source)) rgb = ch->color, nm = ch->name;
      } else if (const AutomationClip* au = s.automation(c.source)) {
        rgb = au->color, nm = au->name;
      }
      const ImVec2 q1(q.x + padW - 6, q.y + rowH - 6);
      const bool on = act == c.id, waiting = que == c.id;
      ImU32 fill = theme::mix(theme::rgb(rgb), P.surface900, on ? 0.15f : hov ? 0.5f : 0.68f);
      dl->AddRectFilled(q, q1, fill, theme::kRadiusSm);
      if (on) dl->AddRect(q, q1, P.accent, theme::kRadiusSm, 0, 2.0f);
      if (waiting) dl->AddRect(q, q1, theme::withAlpha(P.accent, 0.35f + 0.65f * pulse), theme::kRadiusSm, 0, 2.0f);
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + 6, q.y + 6), padW - 18, theme::contrastText(fill), nm.c_str());
      if (on) dl->AddTriangleFilled(ImVec2(q.x + 7, q1.y - 16), ImVec2(q.x + 7, q1.y - 6), ImVec2(q.x + 15, q1.y - 11), theme::contrastText(fill));
      ui::Hint("Press to launch (on the track's launch grid) \xC2\xB7 right-click the track's stop button for its live settings");
      ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(nameW + static_cast<float>(clips.size()) * padW, 0));
    ImGui::PopID();
  }
  if (held != kNoId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    a.engine.releaseClip(held);
    held = kNoId;
  }
  ImGui::EndChild();
  ui::EndDockWindow();
}

}  // namespace gl

// Gridline — arrangements menu / selector and live mode (live pads window,
// per-track live settings).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../core/I18n.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Arrangements.h"
#include "App.h"
#include "Keys.h"
#include "LiveLook.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "TrackerPages.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl {

namespace {

bool gDevOpenArrPopup = false;

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

bool arrangementPlaying(const App& a) {
  return a.engine.playing() && a.engine.mode() == PlayMode::Song;
}

}  // namespace

// ---------------------------------------------------------------------------
// Arrangements

void switchArrangement(App& a, int index) {
  const int n = arrangementCount(a.doc.song());
  if (index < 0 || index >= n) return;
  if (index == a.doc.song().currentArrangement && !a.doc.song().arrangements.empty()) return;
  a.engine.stopAllLive();
  a.doc.editSong("switch arrangement", [index](Song& s) { selectArrangement(s, index); });
  a.playlist.selected.clear();
}

void stepArrangement(App& a, int delta) {
  const Song& s = a.doc.song();
  const int n = arrangementCount(s);
  if (n <= 1) return;
  const int cur = s.arrangements.empty() ? 0 : s.currentArrangement;
  const int next = std::clamp(cur + delta, 0, n - 1);
  if (next != cur) switchArrangement(a, next);
}

void arrangementMenu(App& a) {
  const Song& s = a.doc.song();
  const int n = arrangementCount(s);
  const int cur = s.arrangements.empty() ? 0 : s.currentArrangement;
  const bool songPlay = arrangementPlaying(a);
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const auto& P = theme::pal();
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i);
    std::string label = arrangementName(s, i);
    const Tick len = arrangementLengthTicks(s, i);
    const int bars = static_cast<int>((len + bar - 1) / bar);
    char meta[48];
    if (i == cur && songPlay) {
      if (bars > 0) {
        std::snprintf(meta, sizeof meta, "%d %s \xC2\xB7 %s", bars, bars == 1 ? i18n::tr("bar") : i18n::tr("bars"), i18n::tr("playing"));
      } else {
        std::snprintf(meta, sizeof meta, "%s", i18n::tr("playing"));
      }
    } else if (bars > 0) {
      std::snprintf(meta, sizeof meta, "%d %s", bars, bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
    } else {
      std::snprintf(meta, sizeof meta, "%s", i18n::tr("empty"));
    }
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const bool picked = ImGui::MenuItem(label.c_str(), meta, i == cur);
    if (i == cur && songPlay) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float lh = ImGui::GetTextLineHeight();
      dl->AddCircleFilled(ImVec2(rowPos.x + 4.0f, std::round(rowPos.y + lh * 0.5f)), 3.0f, theme::rgb(theme::role::kPlay), 12);
    }
    if (picked) switchArrangement(a, i);
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
  if (n > 1) {
    if (ImGui::MenuItem(i18n::tr("Move arrangement up"), nullptr, false, cur > 0)) {
      a.doc.editSong("reorder arrangement", [cur](Song& x) { moveArrangement(x, cur, cur - 1); });
    }
    if (ImGui::MenuItem(i18n::tr("Move arrangement down"), nullptr, false, cur + 1 < n)) {
      a.doc.editSong("reorder arrangement", [cur](Song& x) { moveArrangement(x, cur, cur + 1); });
    }
  }
  if (ImGui::MenuItem(i18n::tr("Delete arrangement"), nullptr, false, n > 1)) {
    a.engine.stopAllLive();
    a.doc.editSong("delete arrangement", [cur](Song& x) { removeArrangement(x, cur); });
    a.playlist.selected.clear();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Previous arrangement"), keys::label("arrangement.previous"), false, cur > 0)) switchArrangement(a, cur - 1);
  if (ImGui::MenuItem(i18n::tr("Next arrangement"), keys::label("arrangement.next"), false, cur + 1 < n)) switchArrangement(a, cur + 1);
  (void)P;
}

void arrangementSelector(App& a, float width) {
  arrangementDropdown(a, "##arrangement", width, ImGui::GetFrameHeight(), ArrangementStyle::Studio);
}

void arrangementDropdown(App& a, const char* id, float width, float height, ArrangementStyle style) {
  const Song& s = a.doc.song();
  const auto& P = theme::pal();
  const int n = arrangementCount(s);
  const int cur = s.arrangements.empty() ? 0 : s.currentArrangement;
  const std::string name = arrangementName(s, cur);
  const bool songPlay = arrangementPlaying(a);
  const std::string popId = std::string(id) + "_pop";
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + width, p0.y + height);
  ImGui::InvisibleButton(id, ImVec2(width, height));
  const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  const bool hov = ImGui::IsItemHovered();
  const bool open = ImGui::IsPopupOpen(popId.c_str());
  if (clicked || gDevOpenArrPopup) {
    ImGui::OpenPopup(popId.c_str());
    gDevOpenArrPopup = false;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cy = std::round(p0.y + height * 0.5f);
  if (style == ArrangementStyle::Tracker) {
    trackerview::bevelBox(dl, p0, p1, open);
    if (hov && !open) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::withAlpha(P.paper, 0.06f));
  } else if (style == ArrangementStyle::Smooth) {
    const ImU32 face = theme::mix(P.surface700, P.text, open ? 0.14f : hov ? 0.10f : 0.06f);
    dl->AddRectFilled(p0, p1, face, 5.0f);
  } else if (style == ArrangementStyle::Clips) {
    const ImU32 face = open ? P.surface600 : hov ? P.surface700 : P.surface800;
    dl->AddRectFilled(p0, p1, face, 2.0f);
    dl->AddRect(p0, p1, theme::withAlpha(P.ink, P.dark ? 0.55f : 0.22f), 2.0f);
  } else {
    const ImU32 face = open ? theme::mix(P.field, P.surface600, 0.5f) : hov ? theme::mix(P.field, P.surface700, 0.45f) : P.field;
    dl->AddRectFilled(p0, p1, face, theme::kRadius);
    dl->AddRect(p0, p1, open ? P.hot : P.line, theme::kRadius);
  }
  float tx = p0.x + 7.0f;
  if (songPlay || n > 1) {
    const ImU32 dotCol = songPlay ? theme::rgb(theme::role::kPlay) : P.textFaint;
    if (style == ArrangementStyle::Tracker) {
      dl->AddRectFilled(ImVec2(tx, cy - 2.5f), ImVec2(tx + 5.0f, cy + 2.5f), dotCol);
    } else {
      dl->AddCircleFilled(ImVec2(tx + 2.5f, cy), 3.0f, dotCol, 12);
    }
    tx += 10.0f;
  }
  const float chevArea = style == ArrangementStyle::Smooth ? 16.0f : 15.0f;
  const float maxTw = std::max(8.0f, p1.x - chevArea - tx);
  const float fontSz = style == ArrangementStyle::Clips ? theme::kFontCaption + 1.0f : style == ArrangementStyle::Tracker ? ImGui::GetFontSize() : theme::kFontBody;
  ImFont* fnt = a.fontRegular;
  const float textH = fnt->CalcTextSizeA(fontSz, 1e9f, 0.0f, "Ag").y;
  theme::textEllipsis(dl, fnt, fontSz, ImVec2(tx, std::round(p0.y + (height - textH) * 0.5f)), maxTw,
                      hov || open ? P.text : (style == ArrangementStyle::Clips ? P.text : P.text), name.c_str());
  if (style == ArrangementStyle::Smooth) {
    const float cx = std::round(p1.x - 9.0f);
    dl->AddTriangleFilled(ImVec2(cx - 2.5f, cy - 1.5f), ImVec2(cx + 2.5f, cy - 1.5f), ImVec2(cx, cy - 4.5f), P.textDim);
    dl->AddTriangleFilled(ImVec2(cx + 2.5f, cy + 1.5f), ImVec2(cx - 2.5f, cy + 1.5f), ImVec2(cx, cy + 4.5f), P.textDim);
  } else {
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(std::round(p1.x - 8.5f), cy), 8.5f, hov || open ? P.text : P.textDim);
  }
  if (hov) {
    std::string tip = std::string(i18n::tr("Arrangement")) + ": " + name;
    if (songPlay) tip += std::string(" (") + i18n::tr("playing") + ")";
    tip += " \xC2\xB7 ";
    tip += i18n::tr("switch, add, duplicate, rename, reorder or delete arrangements");
    ui::Hint(tip);
  }
  ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + 2.0f));
  if (ImGui::BeginPopup(popId.c_str())) {
    arrangementMenu(a);
    ImGui::EndPopup();
  }
}

bool devArrangementCommand(App& a, const std::vector<std::string>& args, int frame) {
  if (args.empty()) return false;
  const std::string& op = args[0];
  const std::string p1 = args.size() > 1 ? args[1] : std::string();
  const std::string p2 = args.size() > 2 ? args[2] : std::string();
  if (op == "select") {
    switchArrangement(a, std::atoi(p1.c_str()));
  } else if (op == "next") {
    stepArrangement(a, 1);
  } else if (op == "prev") {
    stepArrangement(a, -1);
  } else if (op == "add") {
    a.engine.stopAllLive();
    a.doc.editSong("new arrangement", [p1](Song& x) { selectArrangement(x, addArrangement(x, p1, false)); });
    a.playlist.selected.clear();
  } else if (op == "dup") {
    a.engine.stopAllLive();
    const Song& s = a.doc.song();
    const int cur = s.arrangements.empty() ? 0 : s.currentArrangement;
    const std::string nm = p1.empty() ? uniqueArrangementName(s, arrangementName(s, cur)) : p1;
    a.doc.editSong("duplicate arrangement", [nm](Song& x) { selectArrangement(x, addArrangement(x, nm, true)); });
    a.playlist.selected.clear();
  } else if (op == "rename") {
    const int cur = a.doc.song().arrangements.empty() ? 0 : a.doc.song().currentArrangement;
    if (!p1.empty()) a.doc.editSong("rename arrangement", [cur, p1](Song& x) { renameArrangement(x, cur, p1); });
  } else if (op == "move") {
    const int from = std::atoi(p1.c_str()), to = std::atoi(p2.c_str());
    a.doc.editSong("reorder arrangement", [from, to](Song& x) { moveArrangement(x, from, to); });
  } else if (op == "delete") {
    const int cur = p1.empty() ? (a.doc.song().arrangements.empty() ? 0 : a.doc.song().currentArrangement) : std::atoi(p1.c_str());
    a.engine.stopAllLive();
    a.doc.editSong("delete arrangement", [cur](Song& x) { removeArrangement(x, cur); });
    a.playlist.selected.clear();
  } else if (op == "popup") {
    gDevOpenArrPopup = true;
  } else if (op != "list") {
    return false;
  }
  const Song& s = a.doc.song();
  const int n = arrangementCount(s);
  const int cur = s.arrangements.empty() ? 0 : s.currentArrangement;
  std::printf("arr frame %d: count %d cur %d playing %d undo %zu", frame, n, cur, arrangementPlaying(a) ? 1 : 0, a.doc.history().position());
  for (int i = 0; i < n; ++i) std::printf(" [%d:\"%s\" len=%lld]", i, arrangementName(s, i).c_str(), static_cast<long long>(arrangementLengthTicks(s, i)));
  std::printf("\n");
  std::fflush(stdout);
  return true;
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
    for (int i = 0; i < kLaunchGridsBasic; ++i)
      if (ImGui::MenuItem(launchGridName(static_cast<LaunchGrid>(i)), nullptr, t.launch == static_cast<LaunchGrid>(i)))
        edit("change launch grid", [i](PlaylistTrack& x) { x.launch = static_cast<LaunchGrid>(i); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Keep in phase"))) {
    if (ImGui::MenuItem(i18n::tr("Off (start from the clip start)"), nullptr, t.syncStart == LaunchGrid::Now))
      edit("change phase", [](PlaylistTrack& x) { x.syncStart = LaunchGrid::Now; });
    for (int i = 1; i < kLaunchGridsBasic; ++i) {
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
  static int shownFrames = 0;
  if (!a.showLivePads) {
    shownFrames = 0;
    return;
  }
  if (shownFrames == 0) {  // K-79: a first floating open goes over the middle of the window ...
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
  }
  const bool vis = ui::BeginDockWindow("Pads###Live pads", &a.showLivePads, ImVec2(560, 300));
  if (shownFrames < 3) {  // ... and in front of the floating channel rack (it used to hide the empty-state text)
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w->DockIsActive) ImGui::BringWindowToDisplayFront(w);
    ++shownFrames;
  }
  if (!vis) {
    ui::EndDockWindow();
    return;
  }
  if (trackerpages::active(a)) {  // K-106: the Tracker layout's Pads page (its look)
    trackerpages::pads(a);
    ui::EndDockWindow();
    return;
  }
  if (livelook::active(a)) {  // K-112c: in the Live layout the pads are the clip grid (tracks as columns, scenes as rows)
    livelook::drawClipGrid(a);
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
      if (on) ui::drawIconFit(dl, ui::Icon::Play, ImVec2(q.x + 7, q1.y - 16), ImVec2(q.x + 15, q1.y - 6), theme::contrastText(fill));
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

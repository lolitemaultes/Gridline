// Gridline - tracks from the playlist on the Stage (docs/stage-view.md, "Tracks from the playlist";
// core/StagePick.h): the Stage toolbar's "From playlist" menu (also in the floor's right-click menu and
// the mixer menu's Stage part), the "Tracks from the playlist" panel (command palette, "Between two
// markers..."), and the playlist's "Show on Stage" and "Move to Stage room" items (clips, tracks, markers).
// Every action is one undo step through core/StagePick.h; selecting is the mixer's own selection.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Meter.h"
#include "../core/StagePick.h"
#include "../core/StageRoomOps.h"
#include "App.h"
#include "ContextMenus.h"
#include "Keys.h"
#include "StageView.h"
#include "StageViewInternal.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::stageui {

using namespace detail;
namespace sp = stagepick;

namespace {

// What a menu or the panel acts on: the tracks found and where they came from.
struct Set {
  sp::Found found;
  std::string roomName;  // a new room's name (a marker's, or the area's bars; "" for a selection)
  bool selection = false;
  bool empty = true;      // nothing was chosen to look in (no selection, no area)
};

struct Panel {
  bool want = false;
  int source = 0;    // 0 the playlist selection, 1 a marker area
  int from = 0;      // the area's start: -1 the time selection, else a marker
  int to = -1;       // its end: -1 the next marker, else a marker
  int shownFrame = -10;  // the panel was drawn in this frame (dev checks)
  bool wantMenu = false;  // dev: open the toolbar's menu
};
Panel gPanel;

sp::Options options(App& a) {
  sp::Options o;
  o.buses = a.settings.getBool("ui.stage.pickBuses");
  o.muted = a.settings.getBool("ui.stage.pickMuted");
  return o;
}

Set selectionSet(App& a) {
  const Song& s = a.doc.song();
  Set r;
  r.selection = true;
  if (!a.playlist.selected.empty()) {
    r.found = sp::fromClips(s, std::vector<ObjectId>(a.playlist.selected.begin(), a.playlist.selected.end()), options(a));
    r.empty = false;
  } else if (!a.playlist.trackSel.empty()) {
    r.found = sp::fromTracks(s, std::vector<int>(a.playlist.trackSel.begin(), a.playlist.trackSel.end()), options(a));
    r.empty = false;
  }
  return r;
}

bool hasTimeSelection(App& a) { return a.playlist.selEnd > a.playlist.selStart; }

sp::Area timeSelection(App& a) {
  sp::Area ar;
  ar.start = a.playlist.selStart;
  ar.end = a.playlist.selEnd;
  return ar;
}

// "bars 9 to 16" for [start, end).
std::string barsText(const Song& s, Tick start, Tick end) {
  const MeterMap mm(s);
  const long b0 = static_cast<long>(mm.barIndex(start)) + 1;
  const long b1 = static_cast<long>(std::max(mm.barIndex(start), mm.barIndex(end - 1))) + 1;
  char b[96];
  if (b0 == b1) {
    std::snprintf(b, sizeof b, i18n::tr("bar %ld"), b0);
  } else {
    std::snprintf(b, sizeof b, i18n::tr("bars %ld to %ld"), b0, b1);
  }
  return b;
}

std::string markerLabel(const Song& s, int k) {
  const Marker& m = s.markers[static_cast<size_t>(k)];
  if (!m.name.empty()) {
    return m.name;
  }
  char b[48];
  std::snprintf(b, sizeof b, i18n::tr("Marker %d"), k + 1);
  return b;
}

// The area's label in a list: "Verse (bars 9 to 16)".
std::string areaLabel(const Song& s, const sp::Area& ar) {
  const std::string name = ar.marker >= 0 ? markerLabel(s, ar.marker) : std::string(i18n::tr("Time selection"));
  return name + "  (" + barsText(s, ar.start, ar.end) + ")";
}

// A new room's name from an area: its marker's name, else "Marker 3", else its bars.
std::string areaRoomName(const Song& s, const sp::Area& ar) {
  if (ar.marker >= 0 && ar.marker < static_cast<int>(s.markers.size())) {
    return markerLabel(s, ar.marker);
  }
  std::string b = barsText(s, ar.start, ar.end);
  if (!b.empty()) {
    b[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(b[0])));
  }
  return b;
}

Set areaSet(App& a, const sp::Area& ar) {
  Set r;
  r.empty = ar.end <= ar.start;
  r.found = sp::fromRange(a.doc.song(), ar.start, ar.end, options(a));
  r.roomName = areaRoomName(a.doc.song(), ar);
  return r;
}

// Where the found tracks are, from the Stage's point of view.
struct Where {
  std::vector<ObjectId> here;      // on the stage shown
  std::vector<ObjectId> notShown;  // in the room shown but not on its stage (not in use)
  std::vector<ObjectId> other;     // in another room
};

Where whereOf(App& a, const std::vector<ObjectId>& ids) {
  const Song& s = a.doc.song();
  Where w;
  const int room = shownRoom(a);
  const std::vector<size_t> shown = shownTracks(a);
  for (ObjectId id : ids) {
    const int i = s.mixerIndex(id);
    if (i <= 0) {
      continue;
    }
    if (std::find(shown.begin(), shown.end(), static_cast<size_t>(i)) != shown.end()) {
      w.here.push_back(id);
    } else if (roomops::trackRoom(s, id) == room) {
      w.notShown.push_back(id);
    } else {
      w.other.push_back(id);
    }
  }
  return w;
}

// The mixer's selection is exactly `ids` (the Stage highlights them).
void choose(App& a, const std::vector<ObjectId>& ids) {
  a.mixerView.sel = std::set<ObjectId>(ids.begin(), ids.end());
  a.selMixer = ids.empty() ? a.doc.song().masterId() : ids.front();
  a.editFocus = App::EditFocus::Mixer;
}

// Shows the Stage (the room holding most of them when none is in the room shown) and selects the
// tracks of `ids` its stage shows. Returns how many are selected.
int selectOn(App& a, const std::vector<ObjectId>& ids, bool say) {
  if (!active(a)) {
    setView(a, true);
  }
  const Song& s = a.doc.song();
  if (whereOf(a, ids).here.empty()) {
    std::vector<int> count(static_cast<size_t>(roomops::roomCount(s)), 0);
    for (ObjectId id : ids) {
      const int r = roomops::trackRoom(s, id);
      if (r >= 0 && r < static_cast<int>(count.size())) {
        ++count[static_cast<size_t>(r)];
      }
    }
    const auto best = std::max_element(count.begin(), count.end());
    if (best != count.end() && *best > 0) {
      showRoom(a, static_cast<int>(best - count.begin()));
    }
  }
  const Where w = whereOf(a, ids);
  choose(a, w.here);
  if (say) {
    std::string msg = i18n::plural(static_cast<long>(w.here.size()), "%d track selected on the Stage", "%d tracks selected on the Stage");
    if (!w.other.empty()) {
      msg += ". " + i18n::plural(static_cast<long>(w.other.size()), "%d more is in another room", "%d more are in other rooms");
    }
    if (!w.notShown.empty()) {
      msg += ". " + i18n::plural(static_cast<long>(w.notShown.size()), "%d is not on the stage yet (Add to the stage)",
                                 "%d are not on the stage yet (Add to the stage)");
    }
    a.notify(msg);
  }
  return static_cast<int>(w.here.size());
}

void addAction(App& a, const std::vector<ObjectId>& ids) {
  if (!active(a)) {
    setView(a, true);
  }
  const int n = sp::addToStage(a.doc, ids, shownRoom(a), a.settings.getNumber("ui.stage.tracks") >= 0.5);
  selectOn(a, ids, false);
  if (n > 0) {
    a.notify(i18n::plural(n, "Added %d track to the stage", "Added %d tracks to the stage"));
  } else {
    a.notify(i18n::tr("They are all on this stage already"));
  }
}

void moveAction(App& a, const std::vector<ObjectId>& ids, int room, const std::string& newName) {
  int to = room;
  const int n = sp::moveToRoom(a.doc, ids, room, newName, shownRoom(a), &to);
  if (to < 0) {
    return;
  }
  if (!active(a)) {
    setView(a, true);
  }
  showRoom(a, to);
  selectOn(a, ids, false);
  const std::string name = roomops::roomName(a.doc.song(), to);
  char b[200];
  if (n == 1) {
    std::snprintf(b, sizeof b, i18n::tr("Moved 1 track to %s"), name.c_str());
  } else if (n > 1) {
    std::snprintf(b, sizeof b, i18n::tr("Moved %d tracks to %s"), n, name.c_str());
  } else {
    std::snprintf(b, sizeof b, i18n::tr("They are all in %s already"), name.c_str());
  }
  a.notify(b);
}

void arrangeAction(App& a, const std::vector<ObjectId>& ids, sp::Arrange how) {
  selectOn(a, ids, false);
  const Where w = whereOf(a, ids);
  sp::arrangeTracks(a.doc, w.here, how);
  choose(a, w.here);
}

void newRoomPrompt(App& a, const std::vector<ObjectId>& ids, const std::string& name) {
  menus::openPrompt("Name the new room", name, [&a, ids](const std::string& n) { moveAction(a, ids, -1, n); });
}

// The summary of what was found: the count, the names, and where they stand.
void summary(App& a, const Set& set) {
  const Song& s = a.doc.song();
  const sp::Found& f = set.found;
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 24.0f * ImGui::GetFontSize());
  if (set.empty) {
    ImGui::TextDisabled("%s", set.selection ? i18n::tr("Select clips or tracks in the playlist first") : i18n::tr("This area is empty"));
    ImGui::PopTextWrapPos();
    return;
  }
  if (f.tracks.empty()) {
    ImGui::TextDisabled("%s", i18n::tr("No mixer tracks found"));
  } else {
    const std::string head = i18n::plural(static_cast<long>(f.tracks.size()), "%d track found", "%d tracks found");
    ImGui::TextUnformatted(head.c_str());
    std::string names;
    for (ObjectId id : f.tracks) {
      const int i = s.mixerIndex(id);
      if (i <= 0) {
        continue;
      }
      if (!names.empty()) {
        names += ", ";
      }
      names += mixerTrackName(s, static_cast<size_t>(i));
      if (std::find(f.buses.begin(), f.buses.end(), id) != f.buses.end()) {
        names += std::string(" ") + i18n::tr("(bus)");
      }
    }
    ImGui::TextDisabled("%s", names.c_str());
  }
  std::string notes = i18n::plural(f.clips, "From %d clip", "From %d clips");
  if (f.mutedLeftOut > 0) {
    notes += ", " + i18n::plural(f.mutedLeftOut, "%d muted clip left out", "%d muted clips left out");
  }
  if (f.toMaster > 0) {
    notes += ", " + i18n::plural(f.toMaster, "%d plays straight into the master", "%d play straight into the master");
  }
  ImGui::TextDisabled("%s", notes.c_str());
  if (!f.tracks.empty()) {
    const Where w = whereOf(a, f.tracks);
    std::string where;
    if (!w.other.empty()) {
      where = i18n::plural(static_cast<long>(w.other.size()), "%d is in another room", "%d are in other rooms");
    }
    if (!w.notShown.empty()) {
      where += (where.empty() ? "" : ", ") +
               i18n::plural(static_cast<long>(w.notShown.size()), "%d is not on the stage yet", "%d are not on the stage yet");
    }
    if (!where.empty()) {
      ImGui::TextDisabled("%s", where.c_str());
    }
  }
  ImGui::PopTextWrapPos();
}

// The arrange presets (core/StagePick.h Arrange), in its order.
const char* const kArrangeName[] = {i18n::N_("Spread left to right"), i18n::N_("Bring forward"), i18n::N_("Push back"),
                                    i18n::N_("Line up at the front"), i18n::N_("Gather around the centre"), i18n::N_("Move left"),
                                    i18n::N_("Move right")};
const char* const kArrangeHint[] = {i18n::N_("Evenly across the stage in the order they stand, each keeping its distance"),
                                    i18n::N_("One row nearer, all together (they keep their spacing)"),
                                    i18n::N_("One row further back, all together (they keep their spacing)"),
                                    i18n::N_("All in the front row, evenly spaced in the order they stand"),
                                    i18n::N_("The whole group slides so its middle is the centre of the stage"),
                                    i18n::N_("The whole group slides a step to the left"), i18n::N_("The whole group slides a step to the right")};
static_assert(sizeof(kArrangeName) / sizeof(kArrangeName[0]) == static_cast<size_t>(sp::Arrange::Count), "one name per preset");

// The actions on what was found (menu items: a menu, a popup or the panel).
void actions(App& a, const Set& set) {
  const Song& s = a.doc.song();
  const std::vector<ObjectId>& ids = set.found.tracks;
  summary(a, set);
  ImGui::Separator();
  const bool any = !ids.empty();
  const Where w = whereOf(a, ids);
  if (ImGui::MenuItem(i18n::tr("Select them on the Stage"), nullptr, false, any)) {
    selectOn(a, ids, true);
  }
  ui::Hint("Highlights them; drag any of them and they all move together");
  const size_t missing = w.other.size() + w.notShown.size();
  std::string add = i18n::tr("Add to the stage");
  if (missing > 0) {
    add += "  (" + std::to_string(missing) + ")";
  }
  if (ImGui::MenuItem(add.c_str(), nullptr, false, missing > 0)) {
    addAction(a, ids);
  }
  ui::Hint("The ones this stage does not show yet come into this room, in free places across the front");
  if (ImGui::BeginMenu(i18n::tr("Move to room"), any)) {
    for (int k = 0; k < roomops::roomCount(s); ++k) {
      bool all = true;
      for (ObjectId id : ids) {
        all = all && roomops::trackRoom(s, id) == k;
      }
      ImGui::PushID(k);
      if (ImGui::MenuItem(roomops::roomName(s, k).c_str(), nullptr, false, !all)) {
        moveAction(a, ids, k, "");
      }
      ImGui::PopID();
    }
    ImGui::Separator();
    if (!set.roomName.empty()) {
      char b[160];
      std::snprintf(b, sizeof b, i18n::tr("New room \"%s\""), set.roomName.c_str());
      if (ImGui::MenuItem(b)) {
        moveAction(a, ids, -1, set.roomName);
      }
      ui::Hint("A new room named after the marker, with the shown room's sound");
    }
    if (ImGui::MenuItem(i18n::tr("New room..."))) {
      newRoomPrompt(a, ids, set.roomName);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Move together"), any)) {
    if (ImGui::MenuItem(i18n::tr("Select them to drag as one"))) {
      selectOn(a, ids, true);
    }
    ui::Hint("Then drag any of them on the stage: they move as a group and keep their places to each other");
    ImGui::Separator();
    const bool here = !w.here.empty() || !w.other.empty();
    for (int k = 0; k < static_cast<int>(sp::Arrange::Count); ++k) {
      const auto how = static_cast<sp::Arrange>(k);
      if (how == sp::Arrange::Left) {
        ImGui::Separator();
      }
      if (ImGui::MenuItem(i18n::tr(kArrangeName[k]), nullptr, false, here)) {
        arrangeAction(a, ids, how);
      }
      ui::Hint(kArrangeHint[k]);
    }
    ImGui::EndMenu();
  }
}

void optionItems(App& a) {
  auto flag = [&](const char* label, const char* key, const char* hint) {
    if (ImGui::MenuItem(i18n::tr(label), nullptr, a.settings.getBool(key))) {
      a.settings.set(key, !a.settings.getBool(key));
    }
    ui::Hint(hint);
  };
  flag(i18n::N_("Include the buses they play into"), "ui.stage.pickBuses", i18n::N_("A drum bus or group the found tracks send into comes too"));
  flag(i18n::N_("Include muted clips"), "ui.stage.pickMuted",
       i18n::N_("Off: muted clips and clips on muted playlist tracks do not count, as they do not play"));
}

void comboMarkers(const Song& s, const char* id, int* v, bool withTime, bool withNext, int after) {
  auto label = [&](int k) -> std::string {
    if (k == -1) {
      return withTime ? std::string(i18n::tr("Time selection")) : std::string(i18n::tr("The next marker"));
    }
    return markerLabel(s, k);
  };
  if (ImGui::BeginCombo(id, label(*v).c_str())) {
    if (withTime || withNext) {
      if (ImGui::Selectable(label(-1).c_str(), *v == -1)) {
        *v = -1;
      }
    }
    for (int k = 0; k < static_cast<int>(s.markers.size()); ++k) {
      if (after >= 0 && s.markers[static_cast<size_t>(k)].pos <= s.markers[static_cast<size_t>(after)].pos) {
        continue;
      }
      ImGui::PushID(k);
      if (ImGui::Selectable(label(k).c_str(), *v == k)) {
        *v = k;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
}

// The panel: pick the source (and the area), see what is found, act.
void panelBody(App& a) {
  const Song& s = a.doc.song();
  Panel& p = gPanel;
  ImGui::TextUnformatted(i18n::tr("Tracks from the playlist"));
  ImGui::Separator();
  if (ImGui::RadioButton(i18n::tr("Playlist selection"), p.source == 0)) {
    p.source = 0;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(i18n::tr("Marker area"), p.source == 1)) {
    p.source = 1;
  }
  Set set;
  if (p.source == 0) {
    set = selectionSet(a);
  } else {
    const int n = static_cast<int>(s.markers.size());
    if (p.from >= n) {
      p.from = n > 0 ? 0 : -1;
    }
    if (p.from == -1 && !hasTimeSelection(a) && n > 0) {
      p.from = 0;
    }
    if (p.to >= n || (p.to >= 0 && p.from >= 0 && s.markers[static_cast<size_t>(p.to)].pos <= s.markers[static_cast<size_t>(p.from)].pos)) {
      p.to = -1;
    }
    if (n == 0 && !hasTimeSelection(a)) {
      ImGui::TextDisabled("%s", i18n::tr("The song has no markers and no time selection yet"));
      set.empty = true;
    } else {
      const float w = 13.0f * ImGui::GetFontSize();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(i18n::tr("From"));
      ImGui::SameLine(4.0f * ImGui::GetFontSize());
      ImGui::SetNextItemWidth(w);
      comboMarkers(s, "##pickfrom", &p.from, hasTimeSelection(a), false, -1);
      if (p.from >= 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(i18n::tr("To"));
        ImGui::SameLine(4.0f * ImGui::GetFontSize());
        ImGui::SetNextItemWidth(w);
        comboMarkers(s, "##pickto", &p.to, false, true, p.from);
      }
      const sp::Area ar = p.from < 0 ? timeSelection(a) : sp::betweenMarkers(s, p.from, p.to);
      ImGui::TextDisabled("%s", barsText(s, ar.start, ar.end).c_str());
      set = areaSet(a, ar);
    }
  }
  ImGui::Separator();
  actions(a, set);
  ImGui::Separator();
  optionItems(a);
}

}  // namespace

namespace detail {

// The "From playlist" menu (the toolbar's, the floor's right-click menu, the mixer menu's Stage part).
void pickMenu(App& a) {
  const Song& s = a.doc.song();
  if (ImGui::BeginMenu(i18n::tr("From the playlist selection"))) {
    actions(a, selectionSet(a));
    ImGui::EndMenu();
  }
  ui::Hint("The mixer tracks the selected clips or playlist tracks play through");
  if (ImGui::BeginMenu(i18n::tr("From a marker area"))) {
    bool any = false;
    if (hasTimeSelection(a)) {
      const sp::Area ar = timeSelection(a);
      if (ImGui::BeginMenu(areaLabel(s, ar).c_str())) {
        actions(a, areaSet(a, ar));
        ImGui::EndMenu();
      }
      any = true;
    }
    const std::vector<sp::Area> areas = sp::markerAreas(s);
    for (size_t k = 0; k < areas.size(); ++k) {
      ImGui::PushID(static_cast<int>(k));
      if (ImGui::BeginMenu(areaLabel(s, areas[k]).c_str())) {
        actions(a, areaSet(a, areas[k]));
        ImGui::EndMenu();
      }
      ImGui::PopID();
      any = true;
    }
    if (!any) {
      ImGui::TextDisabled("%s", i18n::tr("The song has no markers yet"));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Between two markers..."), nullptr, false, !s.markers.empty())) {
      openPickPanel(a, 1);
    }
    ImGui::EndMenu();
  }
  ui::Hint("The mixer tracks of every clip that plays between two markers (or in the time selection)");
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Tracks from the playlist..."))) {
    openPickPanel(a, gPanel.source);
  }
  ImGui::Separator();
  optionItems(a);
}

void openPickPanel(App& a, int source) {
  gPanel.want = true;
  gPanel.source = source;
  if (source == 1 && gPanel.from == -1 && !hasTimeSelection(a)) {
    gPanel.from = 0;
  }
  if (!active(a)) {
    setView(a, true);
  }
}

// Each frame in the Stage's body: the panel, and the command palette's two commands.
void pickPopups(App& a) {
  if (keys::pressed("stage.fromSelection")) {
    openPickPanel(a, 0);
  }
  if (keys::pressed("stage.fromMarkers")) {
    openPickPanel(a, 1);
  }
  if (gPanel.want) {
    gPanel.want = false;
    const ImVec2 c = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImGui::SetNextWindowPos(ImVec2(c.x + sz.x * 0.5f, c.y + 60.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::OpenPopup("##stagepick");
  }
  if (ImGui::BeginPopup("##stagepick")) {
    gPanel.shownFrame = ImGui::GetFrameCount();
    panelBody(a);
    ImGui::EndPopup();
  }
}

bool takeMenuRequest() {
  const bool w = gPanel.wantMenu;
  gPanel.wantMenu = false;
  return w;
}

bool pickPanelOpen() { return gPanel.shownFrame >= ImGui::GetFrameCount() - 1; }

}  // namespace detail

// ---------------------------------------------------------------- the playlist's items
namespace {
void playlistItems(App& a, const Set& set, const char* show) {
  ImGui::Separator();
  const std::vector<ObjectId>& ids = set.found.tracks;
  if (ImGui::MenuItem(i18n::tr(show), nullptr, false, !ids.empty())) {
    selectOn(a, ids, true);
  }
  {
    std::string hint = i18n::plural(static_cast<long>(ids.size()), "%d mixer track", "%d mixer tracks");
    if (!ids.empty()) {
      const Song& s = a.doc.song();
      hint += ": ";
      for (size_t k = 0; k < ids.size() && k < 8; ++k) {
        hint += (k ? ", " : "") + mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(ids[k]))));
      }
      if (ids.size() > 8) {
        hint += ", ...";
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", hint.c_str());
    }
  }
  if (ImGui::BeginMenu(i18n::tr("Move to Stage room"), !ids.empty())) {
    const Song& s = a.doc.song();
    for (int k = 0; k < roomops::roomCount(s); ++k) {
      ImGui::PushID(k);
      if (ImGui::MenuItem(roomops::roomName(s, k).c_str())) {
        moveAction(a, ids, k, "");
      }
      ImGui::PopID();
    }
    ImGui::Separator();
    if (!set.roomName.empty()) {
      char b[160];
      std::snprintf(b, sizeof b, i18n::tr("New room \"%s\""), set.roomName.c_str());
      if (ImGui::MenuItem(b)) {
        moveAction(a, ids, -1, set.roomName);
      }
    }
    if (ImGui::MenuItem(i18n::tr("New room..."))) {
      newRoomPrompt(a, ids, set.roomName);
    }
    ImGui::EndMenu();
  }
}
}  // namespace

void playlistClipItems(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  if (!s.clip(clip)) {
    return;
  }
  Set set;
  set.selection = true;
  set.empty = false;
  std::vector<ObjectId> clips(a.playlist.selected.begin(), a.playlist.selected.end());
  if (!a.playlist.selected.count(clip)) {
    clips = {clip};
  }
  set.found = sp::fromClips(s, clips, options(a));
  playlistItems(a, set, i18n::N_("Show on Stage"));
}

void playlistTrackItems(App& a, int track) {
  const Song& s = a.doc.song();
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) {
    return;
  }
  std::vector<int> lanes(a.playlist.trackSel.begin(), a.playlist.trackSel.end());
  if (!a.playlist.trackSel.count(track)) {
    lanes = {track};
  }
  Set set;
  set.selection = true;
  set.empty = false;
  set.found = sp::fromTracks(s, lanes, options(a));
  playlistItems(a, set, i18n::N_("Show on Stage"));
}

void playlistMarkerItems(App& a, int marker) {
  const Song& s = a.doc.song();
  if (marker < 0 || marker >= static_cast<int>(s.markers.size())) {
    return;
  }
  playlistItems(a, areaSet(a, sp::betweenMarkers(s, marker, -1)), i18n::N_("Show this part on Stage"));
}

void playlistRangeItems(App& a, Tick start, Tick end) {
  if (end <= start) {
    return;
  }
  sp::Area ar;
  ar.start = start;
  ar.end = end;
  playlistItems(a, areaSet(a, ar), i18n::N_("Show the time selection on Stage"));
}

// ---------------------------------------------------------------- dev
namespace detail {
// "pick" in the stage dev command: pick find sel|area <marker> [<to>]|time; pick select|add|forward|back|spread|
// lineup|centre|left|right sel|area ...; pick move <room|new> sel|area ...; pick panel <0|1>.
void devPick(App& a, const std::vector<std::string>& args) {
  const Song& s = a.doc.song();
  auto setOf = [&](size_t at) -> Set {
    const std::string src = args.size() > at ? args[at] : "sel";
    if (src == "area" && args.size() > at + 1) {
      const int from = std::atoi(args[at + 1].c_str());
      const int to = args.size() > at + 2 ? std::atoi(args[at + 2].c_str()) : -1;
      return areaSet(a, sp::betweenMarkers(s, from, to));
    }
    if (src == "time") {
      return areaSet(a, timeSelection(a));
    }
    return selectionSet(a);
  };
  const std::string c = args.size() > 1 ? args[1] : "find";
  if (c == "panel") {
    openPickPanel(a, args.size() > 2 ? std::atoi(args[2].c_str()) : 0);
    return;
  }
  if (c == "menu") {
    gPanel.wantMenu = true;
    return;
  }
  if (c == "move") {
    const Set set = setOf(3);
    const std::string room = args.size() > 2 ? args[2] : "new";
    moveAction(a, set.found.tracks, room == "new" ? -1 : std::atoi(room.c_str()), room == "new" ? set.roomName : std::string());
    return;
  }
  const Set set = setOf(2);
  if (c == "find") {
    std::string ids, buses;
    for (ObjectId id : set.found.tracks) {
      ids += (ids.empty() ? "" : ",") + std::to_string(s.mixerIndex(id));
    }
    for (ObjectId id : set.found.buses) {
      buses += (buses.empty() ? "" : ",") + std::to_string(s.mixerIndex(id));
    }
    const Where w = whereOf(a, set.found.tracks);
    std::printf("pick found %s buses %s clips %d muted %d master %d here %zu other %zu notshown %zu name [%s] undo %zu panel %d\n",
                ids.empty() ? "-" : ids.c_str(), buses.empty() ? "-" : buses.c_str(), set.found.clips, set.found.mutedLeftOut, set.found.toMaster,
                w.here.size(), w.other.size(), w.notShown.size(), set.roomName.c_str(), a.doc.history().position(), pickPanelOpen() ? 1 : 0);
    std::fflush(stdout);
  } else if (c == "select") {
    selectOn(a, set.found.tracks, true);
  } else if (c == "add") {
    addAction(a, set.found.tracks);
  } else {
    static const std::pair<const char*, sp::Arrange> kHow[] = {{"spread", sp::Arrange::SpreadLeftRight}, {"forward", sp::Arrange::Forward},
                                                               {"back", sp::Arrange::Back},             {"lineup", sp::Arrange::LineUpFront},
                                                               {"centre", sp::Arrange::Centre},         {"left", sp::Arrange::Left},
                                                               {"right", sp::Arrange::Right}};
    for (const auto& [name, how] : kHow) {
      if (c == name) {
        arrangeAction(a, set.found.tracks, how);
      }
    }
  }
}
}  // namespace detail

}  // namespace gl::stageui

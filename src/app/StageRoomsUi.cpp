// Gridline - Stage rooms on screen (docs/stage-view.md, "Rooms"): the rooms panel beside the stage, its
// menus, a new room and its templates, each room's headphones (solo, this computer only), and the Bring
// tracks in dialog. The rooms
// themselves are core/StageRoomOps.h; StageView.cpp draws the stage of the room shown.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/MixerOps.h"
#include "../core/Presets.h"
#include "../core/RackLayout.h"
#include "../core/StageRoomOps.h"
#include "../core/StageRooms.h"
#include "App.h"
#include "MixerMeters.h"
#include "SmoothScrollUi.h"
#include "StageView.h"
#include "StageViewInternal.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::stageui {
using theme::pal;
namespace detail {
namespace {

// K-270: the rows (even cards, a gap between them), the closed edge, the slim rail, the rows' widths, the
// width under which a drag gives the slim rail, what the stage always keeps, the tab, and the slide.
constexpr float kRowH = 60.0f, kRowGap = 6.0f, kRowPad = 8.0f, kHeadH = 40.0f, kWallGrip = 6.0f;
constexpr float kEdgeW = 10.0f, kSlimW = 56.0f, kFullMin = 240.0f, kFullDef = 264.0f, kFullMax = 360.0f, kSnapW = 150.0f;
constexpr float kPi = 3.14159265f;
constexpr float kStageMin = 440.0f, kFitW = 336.0f, kFitH = 160.0f, kTabW = 14.0f, kTabH = 44.0f, kSlideSec = 0.16f;
constexpr Rgb kSwatches[] = {0x73A6D8, 0xE07FA2, 0x6DBBA6, 0xE5B062, 0xA889E6, 0xE09466, 0x8FA0B2, 0xD9C45E};

struct Rail {
  std::vector<st::Rect> rows;  // each room's row on screen last frame (empty: not shown)
  std::vector<st::Rect> phones;  // K-270: each room's headphones last frame (zero: none)
  int renaming = -1;           // the row being renamed in place
  bool renameFocus = false;
  char name[96] = {};
  int menuRoom = -1;           // the row's menu
  int menuShown = -1;          // K-270: the room whose menu was drawn last frame (dev checks)
  bool openNew = false, openTemplates = false, openSave = false;
  int saveRoom = 0;
  char saveName[96] = {};
  std::vector<roomops::RoomTemplate> mine;  // the user's templates (listed when a menu opens)
  float anim = -1;             // K-270: the slide, 0 closed .. 1 open (-1: not drawn yet)
  int animFrame = -1;
  bool resizing = false;       // the wall is being dragged
  float live = 0, resizeFrom = 0, resizeAt = 0;
  st::Rect tab{0, 0, 0, 0};    // the tab on the wall last frame
  bool tabHot = false, tabDown = false;
};
Rail R;

struct Bring {
  bool open = false, want = false;
  int room = 0;     // where they go
  int pick = 0;     // 0 any track, 1 an arrangement, 2 a pattern, 3 chosen on the mixer
  int arr = 0;
  ObjectId pattern = kNoId;
  Rgb color = 0;    // only tracks of this colour (0: any)
  bool feeders = true;
  char search[64] = {};
  std::set<ObjectId> checked;
};
Bring B;

const Song& song(App& a) { return a.doc.song(); }

std::string countText(size_t n) {
  char b[48];
  if (n == 1) return i18n::tr("1 track");
  std::snprintf(b, sizeof b, i18n::tr("%zu tracks"), n);
  return b;
}

// The room's floor in small, with a dot for each of its tracks where it stands.
void thumbnail(App& a, ImDrawList* dl, int room, ImVec2 mn, ImVec2 sz, bool active) {
  const auto& P = pal();
  const Song& s = song(a);
  const StageSettings& st = roomops::roomAt(s, room);
  const ImU32 rc = theme::content(roomops::roomColor(s, room));
  const ImU32 floor = active ? theme::mix(P.surface800, rc, 0.22f) : theme::mix(P.surface900, rc, 0.12f);
  const float inset = sz.x * 0.2f;
  const ImVec2 tl(mn.x + inset, mn.y + 3), tr(mn.x + sz.x - inset, mn.y + 3), br(mn.x + sz.x - 1, mn.y + sz.y - 3), bl(mn.x + 1, mn.y + sz.y - 3);
  dl->AddQuadFilled(tl, tr, br, bl, floor);
  dl->AddQuad(tl, tr, br, bl, st.hasRoom() ? theme::withAlpha(rc, 0.9f) : theme::withAlpha(P.textFaint, 0.5f), 1.0f);
  for (size_t k : shownTracksOf(a, room)) {
    const MixerTrack* m = &s.mixer[k];
    const float d = std::clamp(m->depth, 0.0f, 1.0f);
    const float y = bl.y - 3 - d * (bl.y - tl.y - 6);
    const float half = (br.x - bl.x) * 0.5f - d * (inset - 1) - 3;
    const float x = mn.x + sz.x * 0.5f + std::clamp(m->pan, -1.0f, 1.0f) * half;
    dl->AddCircleFilled(ImVec2(x, y), 2.2f, m->muted ? P.textFaint : theme::content(m->color));
  }
}

void beginRename(App& a, int room) {
  R.renaming = room;
  R.renameFocus = true;
  std::snprintf(R.name, sizeof R.name, "%s", roomops::roomName(song(a), room).c_str());
}

}  // namespace

// K-270: why room `room` cannot be deleted, in words ("" when it can).
std::string deleteRefusal(App& a, int room) {
  size_t n = 0;
  switch (roomops::deleteBlock(song(a), room, &n)) {
    case roomops::DeleteBlock::None: return {};
    case roomops::DeleteBlock::NoSuchRoom: return i18n::tr("That room is gone");
    case roomops::DeleteBlock::LastRoom: return i18n::tr("The last room cannot be deleted");
    case roomops::DeleteBlock::HasTracks: break;
  }
  char b[160];
  if (n == 1) std::snprintf(b, sizeof b, "%s", i18n::tr("Move its tracks to another room first (1 track)"));
  else std::snprintf(b, sizeof b, i18n::tr("Move its tracks to another room first (%zu tracks)"), n);
  return b;
}

// K-270: every way of deleting a room comes here: refused with the reason while it has tracks (or is
// the last), else asked first with the in-app confirm.
void requestDeleteRoom(App& a, int room) {
  const std::string why = deleteRefusal(a, room);
  if (!why.empty()) {
    char t[200];
    std::snprintf(t, sizeof t, i18n::tr("%s cannot be deleted"), roomops::roomName(song(a), room).c_str());
    a.notify(t, true, why);
    return;
  }
  const ObjectId id = roomops::roomIdAt(song(a), room);
  char t[200];
  std::snprintf(t, sizeof t, i18n::tr("Delete the room '%s'?"), roomops::roomName(song(a), room).c_str());
  a.confirm = {};
  a.confirm.title = t;
  a.confirm.subtitle = i18n::tr("Its settings and its space track go with it. Undo brings it back.");
  a.confirm.okLabel = i18n::N_("Delete");
  a.confirm.action = [&a, id] {
    // By id: the rooms may have moved (or the room gained a track) while the question was open.
    const int i = roomops::roomIndex(a.doc.song(), id);
    if (i < 0) return;
    const std::string why = deleteRefusal(a, i);
    if (!why.empty()) {
      a.notify(i18n::tr("The room was not deleted"), true, why);
      return;
    }
    roomops::deleteRoom(a.doc, i);
  };
  a.confirm.open = true;
}

namespace {

// The templates as menu items (factory, then the user's own): picks one.
const roomops::RoomTemplate* templateItems(App& a) {
  const roomops::RoomTemplate* picked = nullptr;
  ImGui::TextDisabled("%s", i18n::tr("Templates"));
  for (const auto& t : roomops::factoryTemplates()) {
    if (ImGui::MenuItem(i18n::tr(t.name.c_str()))) picked = &t;
    ui::Hint(t.hint);
  }
  (void)a;
  if (!R.mine.empty()) {
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("My templates"));
    for (const auto& t : R.mine) {
      const std::string label = t.folder.empty() ? t.name : t.folder + " / " + t.name;
      if (ImGui::MenuItem(label.c_str())) picked = &t;
    }
  }
  return picked;
}

void refreshMine(App& a) { R.mine = roomops::listUserTemplates(presetRoot(a.configDir())); }

void startSave(App& a, int room) {
  R.saveRoom = room;
  std::snprintf(R.saveName, sizeof R.saveName, "%s", roomops::roomName(song(a), room).c_str());
  R.openSave = true;
}

void newRoom(App& a, int copyFrom, const roomops::RoomTemplate* t) {
  const int i = roomops::addRoom(a.doc, copyFrom, t);
  if (i < 0) return;
  showRoom(a, i);
  beginRename(a, i);
}

// K-270: the soloed rooms (View::soloRooms, room ids). In the list: heard; another room soloed:
// silent here; none: every room plays.
bool roomSoloed(App& a, int room) {
  const auto& v = view().soloRooms;
  return std::find(v.begin(), v.end(), roomops::roomIdAt(song(a), room)) != v.end();
}

// The headphones, as the channel rack's: a click adds the room to the rooms heard (or takes it out);
// right-click or Ctrl/Cmd+click hears this room only, and again: every room.
void phonesClick(App& a, int room, bool only) {
  auto& v = view().soloRooms;
  const ObjectId id = roomops::roomIdAt(song(a), room);
  const auto at = std::find(v.begin(), v.end(), id);
  if (only) {
    if (v.size() == 1 && at != v.end()) v.clear();
    else v.assign(1, id);
  } else if (at != v.end()) {
    v.erase(at);
  } else {
    v.push_back(id);
  }
  a.forceRebuild();
}

// How loud the room plays now (0..1, its loudest track's meter): the headphones glow with it.
float roomLevel(App& a, int room) {
  float pk = 0;
  for (size_t k : shownTracksOf(a, room)) {
    const mixmeters::Reading r = mixmeters::get(a, song(a).mixer[k].id);
    pk = std::max(pk, std::max(r.peakL, r.peakR));
  }
  return std::clamp(pk * 1.4f, 0.0f, 1.0f);
}

// The row's right-click menu.
void rowMenu(App& a, int room) {
  const Song& s = song(a);
  const int n = roomops::roomCount(s);
  ImGui::TextDisabled("%s", roomops::roomName(s, room).c_str());
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Rename"))) beginRename(a, room);
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    for (size_t k = 0; k < sizeof kSwatches / sizeof kSwatches[0]; ++k) {
      ImGui::PushID(static_cast<int>(k));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##sw", ImVec2(22, 22))) roomops::setRoomColor(a.doc, room, kSwatches[k]);
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + 20, p.y + 20), theme::content(kSwatches[k]), 4);
      if (roomops::roomColor(s, room) == kSwatches[k]) ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + 22, p.y + 22), pal().text, 5, 0, 1.5f);
      ImGui::PopID();
      if (k % 4 != 3) ImGui::SameLine(0, 2);
    }
    if (ImGui::MenuItem(i18n::tr("Automatic"))) roomops::setRoomColor(a.doc, room, 0);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Duplicate settings"))) {
    const std::string nm = roomops::roomName(s, room) + " 2";
    const int i = roomops::addRoom(a.doc, room, nullptr, nm, 0);
    if (i >= 0) showRoom(a, i);
  }
  ui::Hint("A new room with the same space and settings, without its tracks");
  if (ImGui::MenuItem(i18n::tr("Move up"), nullptr, false, room > 0)) moveRoomKeepShown(a, room, room - 1);
  if (ImGui::MenuItem(i18n::tr("Move down"), nullptr, false, room < n - 1)) moveRoomKeepShown(a, room, room + 1);
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Apply a template"))) {
    if (const roomops::RoomTemplate* t = templateItems(a)) roomops::applyTemplate(a.doc, room, *t);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Save as a template..."))) startSave(a, room);
  if (ImGui::MenuItem(i18n::tr("Bring tracks in..."))) {
    showRoom(a, room);
    openBring(a, room);
  }
  ui::Hint("Move tracks from other rooms into this one: by arrangement, pattern, colour or name");
  ImGui::Separator();
  // K-270: only an empty room can go (its tracks are moved to another room first), never the last one.
  const roomops::DeleteBlock why = roomops::deleteBlock(s, room);
  if (ImGui::MenuItem(i18n::tr("Delete room..."), "Del", false, why == roomops::DeleteBlock::None)) requestDeleteRoom(a, room);
  if (why != roomops::DeleteBlock::None) {  // greyed out, with the reason under it
    const std::string reason = deleteRefusal(a, room);
    ImGui::SetItemTooltip("%s", reason.c_str());
    ImGui::Indent(10);
    ImGui::PushFont(nullptr, theme::kFontCaption);
    ImGui::TextDisabled("%s", reason.c_str());
    ImGui::PopFont();
    ImGui::Unindent(10);
  }
}

// "+": a new room from the room shown, or from a template.
void newRoomMenu(App& a) {
  const Song& s = song(a);
  char b[96];
  std::snprintf(b, sizeof b, i18n::tr("Room %d starts from"), roomops::roomCount(s) + 1);
  ImGui::TextDisabled("%s", b);
  ImGui::Separator();
  const int cur = shownRoom(a);
  std::snprintf(b, sizeof b, i18n::tr("Same as %s"), roomops::roomName(s, cur).c_str());
  if (ImGui::MenuItem(b)) newRoom(a, cur, nullptr);
  ui::Hint("Its space, back of the stage, width and echoes. Never its tracks or places.");
  ImGui::Separator();
  if (const roomops::RoomTemplate* t = templateItems(a)) newRoom(a, cur, t);
  ImGui::Separator();
  std::snprintf(b, sizeof b, i18n::tr("Save %s as a template..."), roomops::roomName(s, cur).c_str());
  if (ImGui::MenuItem(b)) startSave(a, cur);
}

// The Templates button: apply, save, delete.
void templatesMenu(App& a) {
  const Song& s = song(a);
  const int cur = shownRoom(a);
  char b[96];
  std::snprintf(b, sizeof b, i18n::tr("Apply to %s"), roomops::roomName(s, cur).c_str());
  if (ImGui::BeginMenu(b)) {
    if (const roomops::RoomTemplate* t = templateItems(a)) roomops::applyTemplate(a.doc, cur, *t);
    ImGui::EndMenu();
  }
  ui::Hint("The template's sound settings. The room keeps its tracks and their places.");
  std::snprintf(b, sizeof b, i18n::tr("Save %s as a template..."), roomops::roomName(s, cur).c_str());
  if (ImGui::MenuItem(b)) startSave(a, cur);
  if (ImGui::BeginMenu(i18n::tr("Delete a template"), !R.mine.empty())) {
    for (size_t k = 0; k < R.mine.size(); ++k) {
      ImGui::PushID(static_cast<int>(k));
      const auto& t = R.mine[k];
      const std::string label = t.folder.empty() ? t.name : t.folder + " / " + t.name;
      if (ImGui::MenuItem(label.c_str())) confirmDeleteTemplate(a, t);
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }
}

void savePopup(App& a) {
  if (R.openSave) {
    ImGui::OpenPopup("##roomsave");
    R.openSave = false;
  }
  ImGui::SetNextWindowSize(ImVec2(360, 0));
  if (!ImGui::BeginPopup("##roomsave")) return;
  ImGui::TextUnformatted(i18n::tr("Save the room's settings as a template"));
  ImGui::TextDisabled("%s", i18n::tr("Its space, back of the stage, width and echoes: never its tracks"));
  ImGui::Spacing();
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(-1);
  const bool enter = ImGui::InputText("##tname", R.saveName, sizeof R.saveName, ImGuiInputTextFlags_EnterReturnsTrue);
  ui::Hint("A name; \"Folder/Name\" puts it in a folder");
  ImGui::Spacing();
  const bool ok = R.saveName[0] != 0;
  if (ui::PrimaryButton(i18n::tr("Save"), 100, nullptr, ok) || (enter && ok)) {
    std::string err, path;
    const roomops::RoomTemplate t = roomops::templateOf(song(a), R.saveRoom, R.saveName);
    if (roomops::saveUserTemplate(presetRoot(a.configDir()), t, R.saveName, &path, &err)) {
      a.notify(i18n::tr("Saved the room template"));
      refreshMine(a);
    } else {
      a.notify(i18n::tr("Could not save the room template"), true, err);
    }
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ui::TextButton(i18n::tr("Cancel"), false, 100)) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ---------------------------------------------------------------- Bring tracks in
bool matchesPick(App& a, ObjectId id, const std::vector<ObjectId>& picked) {
  if (B.pick == 0) return true;
  if (B.pick == 3) {
    const auto sel = mixerSelection(a);
    return std::find(sel.begin(), sel.end(), id) != sel.end();
  }
  return std::find(picked.begin(), picked.end(), id) != picked.end();
}

void bringDialog(App& a) {
  if (B.want) {
    ImGui::OpenPopup("##stagebring");
    B.want = false;
  }
  const ImVec2 vs = ImGui::GetMainViewport()->Size;
  ImGui::SetNextWindowSize(ImVec2(std::min(820.0f, vs.x - 40), std::min(640.0f, vs.y - 60)), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  B.open = ImGui::BeginPopupModal("##stagebring", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
  if (!B.open) return;
  const auto& P = pal();
  const Song& s = song(a);
  B.room = std::clamp(B.room, 0, roomops::roomCount(s) - 1);
  char b[160];
  std::snprintf(b, sizeof b, i18n::tr("Bring tracks into %s"), roomops::roomName(s, B.room).c_str());
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(b);
  ImGui::PopFont();
  ImGui::TextDisabled("%s", i18n::tr("They keep their places, effects and routing, and take on this room's sound."));
  ImGui::Separator();
  // What the pick is made of: an arrangement's or a pattern's tracks.
  std::vector<ObjectId> picked;
  if (B.pick == 1) picked = roomops::tracksOfArrangement(s, B.arr);
  if (B.pick == 2) picked = roomops::tracksOfPattern(s, B.pattern);
  const float leftW = 250, footH = ImGui::GetFrameHeight() + 16;
  ImGui::BeginChild("##bringleft", ImVec2(leftW, -footH), ImGuiChildFlags_None);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##bsearch", i18n::tr("Search tracks"), B.search, sizeof B.search);
  ImGui::Spacing();
  ImGui::TextDisabled("%s", i18n::tr("Pick by"));
  const char* picks[] = {i18n::N_("Any track"), i18n::N_("Used in an arrangement"), i18n::N_("Used in a pattern"), i18n::N_("Chosen on the mixer")};
  int was = B.pick;
  for (int k = 0; k < 4; ++k)
    if (ImGui::RadioButton(i18n::tr(picks[k]), B.pick == k)) B.pick = k;
  if (B.pick == 1) {
    const int n = std::max(1, static_cast<int>(s.arrangements.size()));
    B.arr = std::clamp(B.arr, 0, n - 1);
    const std::string cur = s.arrangements.empty() ? std::string(i18n::tr("Arrangement")) : s.arrangements[static_cast<size_t>(B.arr)].name;
    if (ui::BeginCombo("##barr", cur.c_str(), leftW - 16)) {
      for (int k = 0; k < n; ++k) {
        ImGui::PushID(k);
        const std::string nm = s.arrangements.empty() ? std::string(i18n::tr("Arrangement")) : s.arrangements[static_cast<size_t>(k)].name;
        if (ImGui::Selectable(nm.c_str(), k == B.arr)) B.arr = k, was = -1;
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
  }
  if (B.pick == 2) {
    const Pattern* p = s.pattern(B.pattern);
    if (!p && !s.patterns.empty()) B.pattern = s.patterns[0].id, p = &s.patterns[0];
    if (ui::BeginCombo("##bpat", p ? p->name.c_str() : "", leftW - 16)) {
      for (const auto& q : s.patterns) {
        ImGui::PushID(static_cast<int>(q.id));
        if (ImGui::Selectable(q.name.c_str(), q.id == B.pattern)) B.pattern = q.id, was = -1;
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
  }
  if (was != B.pick) {  // a new pick ticks what it finds (outside this room)
    std::vector<ObjectId> fresh;
    if (B.pick == 1) fresh = roomops::tracksOfArrangement(s, B.arr);
    if (B.pick == 2) fresh = roomops::tracksOfPattern(s, B.pattern);
    if (B.pick == 3) fresh = mixerSelection(a);
    B.checked.clear();
    for (ObjectId id : fresh)
      if (s.mixerTrack(id) && id != s.masterId() && !roomops::isSpaceTrack(s, id) && roomops::trackRoom(s, id) != B.room) B.checked.insert(id);
  }
  ImGui::Spacing();
  ImGui::TextDisabled("%s", i18n::tr("Colour"));
  {
    std::vector<Rgb> cols;
    for (size_t i = 1; i < s.mixer.size(); ++i)
      if (s.mixer[i].color != 0 && std::find(cols.begin(), cols.end(), s.mixer[i].color) == cols.end() && cols.size() < 16) cols.push_back(s.mixer[i].color);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t k = 0; k < cols.size(); ++k) {
      ImGui::PushID(static_cast<int>(k));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##bc", ImVec2(22, 22))) B.color = B.color == cols[k] ? 0 : cols[k];
      ui::Hint("Only tracks of this colour (click again: any colour)");
      dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + 20, p.y + 20), theme::content(cols[k]), 4);
      if (B.color == cols[k]) dl->AddRect(p, ImVec2(p.x + 22, p.y + 22), P.text, 5, 0, 1.5f);
      ImGui::PopID();
      if (k % 8 != 7 && k + 1 < cols.size()) ImGui::SameLine(0, 2);
    }
    if (cols.empty()) ImGui::TextDisabled("%s", i18n::tr("No coloured tracks"));
  }
  ImGui::Spacing();
  ImGui::TextDisabled("%s", i18n::tr("When they move"));
  ImGui::Checkbox(i18n::tr("Bring the tracks that feed them"), &B.feeders);
  ui::Hint("Tracks that send into a moved bus come along, so the bus keeps its sources");
  ImGui::EndChild();
  ImGui::SameLine(0, 12);
  // The tracks, grouped by the room they are in now.
  ImGui::BeginChild("##bringlist", ImVec2(0, -footH), ImGuiChildFlags_Borders);
  std::string q = B.search;
  std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  int shownRows = 0;
  for (int r = 0; r < roomops::roomCount(s); ++r) {
    if (r == B.room) continue;
    std::vector<ObjectId> rows;
    for (size_t k : shownTracksOf(a, r)) {  // the tracks its stage shows
      const ObjectId id = s.mixer[k].id;
      const int i = static_cast<int>(k);
      const MixerTrack& m = s.mixer[static_cast<size_t>(i)];
      std::string nm = mixerTrackName(s, static_cast<size_t>(i));
      std::transform(nm.begin(), nm.end(), nm.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (!q.empty() && nm.find(q) == std::string::npos) continue;
      if (B.color != 0 && m.color != B.color) continue;
      if (!matchesPick(a, id, picked)) continue;
      rows.push_back(id);
    }
    if (rows.empty()) continue;
    ImGui::PushID(r);
    // The room's heading: its colour, name and a box for all of its rows.
    bool all = true;
    for (ObjectId id : rows) all = all && B.checked.count(id);
    if (ImGui::Checkbox("##all", &all))
      for (ObjectId id : rows) {
        if (all) B.checked.insert(id);
        else B.checked.erase(id);
      }
    ImGui::SameLine();
    const ImVec2 hp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(hp.x, hp.y + 4), ImVec2(hp.x + 4, hp.y + ImGui::GetFontSize() + 2), theme::content(roomops::roomColor(s, r)), 1);
    ImGui::SetCursorScreenPos(ImVec2(hp.x + 10, hp.y));
    std::snprintf(b, sizeof b, "%s  \xC2\xB7  %s", roomops::roomName(s, r).c_str(), countText(rows.size()).c_str());
    ImGui::PushFont(a.fontBold, 0.0f);
    ImGui::TextUnformatted(b);
    ImGui::PopFont();
    for (ObjectId id : rows) {
      const int i = s.mixerIndex(id);
      const MixerTrack& m = s.mixer[static_cast<size_t>(i)];
      ImGui::PushID(static_cast<int>(id));
      ImGui::Indent(18);
      bool on = B.checked.count(id) > 0;
      if (ImGui::Checkbox("##t", &on)) {
        if (on) B.checked.insert(id);
        else B.checked.erase(id);
      }
      ImGui::SameLine();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float h = ImGui::GetFontSize() + 4;
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + h, p.y + h), theme::content(m.color ? m.color : 0x5E6A75), 4);
      ImGui::Dummy(ImVec2(h, h));
      ImGui::SameLine();
      const std::string nm = mixerTrackName(s, static_cast<size_t>(i));
      ImGui::TextUnformatted(nm.c_str());
      std::snprintf(b, sizeof b, i18n::tr("Insert %d"), i);
      if (nm != b) {  // a track with a name of its own: its insert number beside it
        ImGui::SameLine();
        ImGui::TextDisabled("%s", b);
      }
      if (B.pick == 1) {
        const auto other = roomops::otherArrangementsUsing(s, id, B.arr);
        if (!other.empty()) {
          std::string also = std::string(i18n::tr("also used in")) + " " + other.front();
          if (other.size() > 1) also += " +" + std::to_string(other.size() - 1);
          ImGui::SameLine();
          ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.hot), "%s", also.c_str());
          ui::Hint("A track is in one room: moving it moves it for every arrangement that uses it");
        }
      }
      ImGui::Unindent(18);
      ImGui::PopID();
      ++shownRows;
    }
    ImGui::Spacing();
    ImGui::PopID();
  }
  if (shownRows == 0) ImGui::TextDisabled("%s", i18n::tr("No tracks in other rooms match"));
  ImGui::EndChild();
  // Footer.
  std::vector<ObjectId> go(B.checked.begin(), B.checked.end());
  go.erase(std::remove_if(go.begin(), go.end(), [&](ObjectId id) { return !s.mixerTrack(id) || roomops::trackRoom(s, id) == B.room; }), go.end());
  size_t total = go.size();
  if (B.feeders)
    for (ObjectId f : roomops::feedersOf(s, go))
      if (roomops::trackRoom(s, f) != B.room) ++total;
  ImGui::Spacing();
  if (ui::TextButton(i18n::tr("Select none"), false, 0)) B.checked.clear();
  ImGui::SameLine();
  const float bw = 150;
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bw * 2 - 8);
  if (ui::TextButton(i18n::tr("Cancel"), false, bw) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  ImGui::SameLine(0, 8);
  if (total == 1) std::snprintf(b, sizeof b, "%s", i18n::tr("Move 1 track"));
  else std::snprintf(b, sizeof b, i18n::tr("Move %zu tracks"), total);
  if (ui::PrimaryButton(b, bw, nullptr, total > 0)) {
    a.doc.endGesture();
    const int moved = roomops::moveTracksToRoom(a.doc, go, B.room, B.feeders);
    a.doc.endGesture();
    std::snprintf(b, sizeof b, i18n::tr("Moved %d tracks to %s"), moved, roomops::roomName(song(a), B.room).c_str());
    a.notify(b);
    B.checked.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// The room's headphones (K-270): solo rooms, several at once, as in the channel rack.
void phonesButton(App& a, ImDrawList* dl, int i, ImVec2 b0, ImVec2 b1) {
  const View& v = view();
  ImGui::SetCursorScreenPos(b0);
  const bool soloed = roomSoloed(a, i), silenced = !v.soloRooms.empty() && !soloed;
  if (ImGui::InvisibleButton("##phones", ImVec2(b1.x - b0.x, b1.y - b0.y))) {
    const ImGuiIO& io = ImGui::GetIO();
    phonesClick(a, i, io.KeyCtrl || io.KeySuper);
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) phonesClick(a, i, true);
  const bool ph = ImGui::IsItemHovered();
  const bool pressed = ph && (ImGui::IsItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Right));
  ui::Hint(soloed     ? i18n::N_("Heard: only rooms with lit headphones play here \xC2\xB7 click: take it out \xC2\xB7 right-click: this room only, again: every room")
           : silenced ? i18n::N_("Silent here while other rooms are soloed \xC2\xB7 click: hear it too \xC2\xB7 right-click: hear this room only")
                      : i18n::N_("Click: solo this room (click others to add them) \xC2\xB7 right-click: hear this room only. Just for you: never saved or shared"));
  ui::PhonesFace(dl, b0, b1, roomLevel(a, i), silenced, soloed, ph, pressed);
  if (static_cast<size_t>(i) < R.phones.size()) R.phones[static_cast<size_t>(i)] = st::Rect{b0.x, b0.y, b1.x, b1.y};
}

// The row's right-click menu (opened after the rows: inside a row's PushID it would get another id).
void rowPopup(App& a, bool openMenu) {
  const int n = roomops::roomCount(song(a));
  if (openMenu) ImGui::OpenPopup("##roomrow");
  if (ImGui::BeginPopup("##roomrow")) {
    if (R.menuRoom >= 0 && R.menuRoom < n) rowMenu(a, R.menuRoom), R.menuShown = R.menuRoom;
    ImGui::EndPopup();
  }
}

// ---- the rooms column (K-270): closed it is a thin edge with a tab; open, rows or a slim rail ----

// The width the user gave it (the slim rail under kSnapW), before the mixer's own size is taken into account.
float storedWidth(App& a) {
  const float w = static_cast<float>(a.settings.getNumber("ui.stage.roomsWidth"));
  return w < kSnapW ? kSlimW : std::clamp(w, kFullMin, kFullMax);
}

// The open column: the slim rail, or rows between their minimum and what still leaves the stage its room.
float openWidth(App& a) {
  const float most = std::min(kFullMax, view().fullW - kStageMin);
  const float w = R.resizing ? R.live : storedWidth(a);
  if (w < kSnapW || most < kFullMin) return kSlimW;
  return std::clamp(w, kFullMin, most);
}

// The slide: 0 closed .. 1 open, moved once a frame (at once with the interface animations off).
float slideNow(App& a) {
  const float target = roomsPanelOpen(a) ? 1.0f : 0.0f;
  const int f = ImGui::GetFrameCount();
  if (R.anim < 0 || !scrollui::animations()) {
    R.anim = target;
  } else if (R.animFrame != f) {
    const float step = ImGui::GetIO().DeltaTime / kSlideSec;
    R.anim = target > R.anim ? std::min(target, R.anim + step) : std::max(target, R.anim - step);
  }
  R.animFrame = f;
  return R.anim;
}

float easeOut(float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

void setRoomsWidth(App& a, float w) { a.settings.set("ui.stage.roomsWidth", static_cast<double>(w < kSnapW ? kSlimW : std::clamp(w, kFullMin, kFullMax))); }

// The rows: the heading with "+", a card per room, and the foot.
void drawRows(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  View& v = view();
  const Song& s = song(a);
  const int n = roomops::roomCount(s), cur = shownRoom(a);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // The heading: Rooms, how many, and "+".
  dl->AddText(a.fontBold, theme::kFontBody, ImVec2(p0.x + 16, p0.y + 12), P.text, i18n::tr("Rooms"));
  {
    const float tw = a.fontBold->CalcTextSizeA(theme::kFontBody, 1e9f, 0, i18n::tr("Rooms")).x;
    char c[16];
    std::snprintf(c, sizeof c, "%d", n);
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x + 24 + tw, p0.y + 14), P.textFaint, c);
  }
  ImGui::SetCursorScreenPos(ImVec2(p1.x - 40, p0.y + 8));
  if (ui::IconButton("##roomnew", ui::Icon::Plus, ImGui::IsPopupOpen("##roomnewmenu"), 24.0f, "New room: from this room's settings or a template")) {
    refreshMine(a);
    ImGui::OpenPopup("##roomnewmenu");
  }
  ImGui::SetNextWindowPos(ImVec2(p1.x - 40, p0.y + 36));
  if (ImGui::BeginPopup("##roomnewmenu")) {
    newRoomMenu(a);
    ImGui::EndPopup();
  }
  // The rows: even cards with a gap between them, clear of the wall (its last pixels are the width handle).
  const float footH = 2 * (ImGui::GetFrameHeight() + 6) + 18;
  const ImVec2 l0(p0.x, p0.y + kHeadH), l1(p1.x - kWallGrip, p1.y - footH);
  R.rows.assign(static_cast<size_t>(n), st::Rect{0, 0, 0, 0});
  R.phones.assign(static_cast<size_t>(n), st::Rect{0, 0, 0, 0});
  ImGui::SetCursorScreenPos(l0);
  ImGui::BeginChild("##roomrows", ImVec2(l1.x - l0.x, std::max(kRowH, l1.y - l0.y)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImDrawList* rl = ImGui::GetWindowDrawList();
  bool openMenu = false;
  const ImVec2 clip0 = ImGui::GetWindowPos(), clip1(clip0.x + ImGui::GetWindowWidth(), clip0.y + ImGui::GetWindowHeight());
  const float top = clip0.y - ImGui::GetScrollY() + 2, left = clip0.x + kRowPad, right = clip1.x - 2;
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i);
    const ImVec2 r0(left, std::round(top + i * (kRowH + kRowGap))), r1(right, r0.y + kRowH);
    ImGui::SetCursorScreenPos(r0);
    ImGui::SetNextItemAllowOverlap();  // the headphones sit on top of the row
    ImGui::InvisibleButton("##row", ImVec2(r1.x - r0.x, kRowH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) showRoom(a, i);
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) beginRename(a, i);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      R.menuRoom = i;
      refreshMine(a);
      openMenu = true;  // opened after the loop: inside the row's PushID it would get another id than BeginPopup
    }
    // Drag a row up or down to reorder the rooms.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
      ImGui::SetDragDropPayload("GL_STAGE_ROOM", &i, sizeof i);
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_STAGE_ROOM"); pl && pl->DataSize == sizeof(int)) {
        const int from = *static_cast<const int*>(pl->Data);
        if (from != i) moveRoomKeepShown(a, from, i);
      }
      ImGui::EndDragDropTarget();
    }
    if (hov) ui::Hint("Click: show this room \xC2\xB7 double-click: rename \xC2\xB7 drag: reorder \xC2\xB7 right-click: more");
    // K-270: Delete over a row deletes that room (asked first; refused while it has tracks).
    if (hov && R.renaming < 0 && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) requestDeleteRoom(a, i);
    const bool active = i == cur, drop = v.dropRoom == i && i != cur;
    const ImU32 rc = theme::content(roomops::roomColor(s, i));
    if (drop) rl->AddRectFilled(r0, r1, theme::mix(P.surface800, P.accent, 0.22f), theme::kRadiusLg);
    else if (active) rl->AddRectFilled(r0, r1, v.stage, theme::kRadiusLg);
    else if (hov) rl->AddRectFilled(r0, r1, theme::withAlpha(P.surface700, 0.6f), theme::kRadiusLg);
    else rl->AddRectFilled(r0, r1, theme::withAlpha(P.surface800, 0.35f), theme::kRadiusLg);
    if (active || drop) rl->AddRectFilled(r0, ImVec2(r0.x + 3, r1.y), rc, 1.0f);
    if (drop) rl->AddRect(ImVec2(r0.x + 1, r0.y + 1), ImVec2(r1.x - 1, r1.y - 1), P.accent, theme::kRadiusLg, 0, 1.5f);
    thumbnail(a, rl, i, ImVec2(r0.x + 12, std::round(r0.y + (kRowH - 40) * 0.5f)), ImVec2(56, 40), active);
    // K-270: the room's headphones (two rooms or more): solo rooms, several at once, as in the channel rack.
    const bool phones = n > 1;
    const float pw = racklayout::kPhonesW + 2;
    const float tx = r0.x + 80, tw = r1.x - tx - 10 - (phones ? pw + 10 : 0);
    // Copied, not held: the rename below is a whole-song edit that replaces the room list.
    const uint8_t roomType = roomops::roomAt(s, i).roomType;
    if (R.renaming == i) {
      ImGui::SetCursorScreenPos(ImVec2(tx - 4, r0.y + 9));
      if (R.renameFocus) ImGui::SetKeyboardFocusHere(), R.renameFocus = false;
      ImGui::SetNextItemWidth(tw + 4);
      const bool done = ImGui::InputText("##rn", R.name, sizeof R.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) R.renaming = -1;
      else if (done || ImGui::IsItemDeactivated()) {
        if (R.name[0]) roomops::renameRoom(a.doc, i, R.name);
        R.renaming = -1;
      }
    } else {
      theme::textEllipsis(rl, a.fontBold, theme::kFontBody - 1, ImVec2(tx, r0.y + 13), tw, active || drop ? P.text : P.textDim, roomops::roomName(s, i).c_str());
    }
    std::string sub = countText(shownTracksOf(a, i).size()) + "  \xC2\xB7  " + i18n::tr(mixops::roomTypeName(roomType));
    theme::textEllipsis(rl, a.fontRegular, theme::kFontCaption, ImVec2(tx, r0.y + 33), tw, P.textFaint, sub.c_str());
    if (phones) {
      const ImVec2 b0(r1.x - 10 - pw, std::round(r0.y + kRowH * 0.5f - pw * 0.5f)), b1(b0.x + pw, b0.y + pw);
      phonesButton(a, rl, i, b0, b1);
    }
    // The row as a drop target for tracks dragged off the stage (StageView.cpp reads it while dragging).
    const float y0 = std::max(r0.y, clip0.y), y1 = std::min(r1.y, clip1.y);
    if (y1 > y0) R.rows[static_cast<size_t>(i)] = st::Rect{r0.x, y0, r1.x, y1};
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(ImVec2(left, top + n * (kRowH + kRowGap)));
  ImGui::Dummy(ImVec2(1, 1));  // the rows' height, for the scrollbar
  rowPopup(a, openMenu);
  ImGui::EndChild();
  // The foot: templates, bring tracks in (each room's headphones took "Hear this room only"'s place).
  float y = p1.y - footH + 10;
  const float bw = p1.x - p0.x - 16 - kRowPad - kWallGrip;
  dl->AddLine(ImVec2(p0.x + kRowPad, y - 5), ImVec2(p1.x - kWallGrip, y - 5), P.line);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + kRowPad + 4, y));
  if (ui::TextButton(i18n::tr("Templates"), ImGui::IsPopupOpen("##roomtemplates"), bw, "Apply a room template, or save this room's settings as one")) {
    refreshMine(a);
    ImGui::OpenPopup("##roomtemplates");
  }
  if (ImGui::BeginPopup("##roomtemplates")) {
    templatesMenu(a);
    ImGui::EndPopup();
  }
  y += ImGui::GetFrameHeight() + 6;
  ImGui::SetCursorScreenPos(ImVec2(p0.x + kRowPad + 4, y));
  if (ui::PrimaryButton(i18n::tr("Bring tracks in..."), bw, "Move tracks from other rooms into this one: by arrangement, pattern, colour or name")) openBring(a, cur);
}

// The slim rail: each room as its little floor in its colour, lit headphones on soloed rooms, and "+".
void drawSlim(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  View& v = view();
  const Song& s = song(a);
  const int n = roomops::roomCount(s), cur = shownRoom(a);
  // Each room: its floor, and under it (two rooms or more) its headphones, as in the full rows.
  const float pw = racklayout::kPhonesW + 2, cellH = n > 1 ? 30.0f + pw + 14.0f : 40.0f, gap = 6.0f;
  R.rows.assign(static_cast<size_t>(n), st::Rect{0, 0, 0, 0});
  R.phones.assign(static_cast<size_t>(n), st::Rect{0, 0, 0, 0});
  const ImVec2 l0(p0.x, p0.y + 8), l1(p1.x - kWallGrip, p1.y - 8);
  ImGui::SetCursorScreenPos(l0);
  ImGui::BeginChild("##roomslim", ImVec2(l1.x - l0.x, std::max(cellH, l1.y - l0.y)), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  ImDrawList* rl = ImGui::GetWindowDrawList();
  bool openMenu = false;
  const ImVec2 clip0 = ImGui::GetWindowPos(), clip1(clip0.x + ImGui::GetWindowWidth(), clip0.y + ImGui::GetWindowHeight());
  const float top = clip0.y - ImGui::GetScrollY(), left = clip0.x + 6, right = clip1.x - 2;
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i);
    const ImVec2 r0(left, std::round(top + i * (cellH + gap))), r1(right, r0.y + cellH);
    ImGui::SetCursorScreenPos(r0);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##cell", ImVec2(r1.x - r0.x, cellH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) showRoom(a, i);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      R.menuRoom = i;
      refreshMine(a);
      openMenu = true;
    }
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
      ImGui::SetDragDropPayload("GL_STAGE_ROOM", &i, sizeof i);
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_STAGE_ROOM"); pl && pl->DataSize == sizeof(int)) {
        const int from = *static_cast<const int*>(pl->Data);
        if (from != i) moveRoomKeepShown(a, from, i);
      }
      ImGui::EndDragDropTarget();
    }
    if (hov) {
      const std::string sub = countText(shownTracksOf(a, i).size()) + "  \xC2\xB7  " + i18n::tr(mixops::roomTypeName(roomops::roomAt(s, i).roomType));
      ImGui::SetTooltip("%s\n%s", roomops::roomName(s, i).c_str(), sub.c_str());
      ui::Hint("Click: show this room \xC2\xB7 drag: reorder \xC2\xB7 right-click: more");
      if (R.renaming < 0 && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) requestDeleteRoom(a, i);
    }
    const bool active = i == cur, drop = v.dropRoom == i && i != cur;
    const ImU32 rc = theme::content(roomops::roomColor(s, i));
    if (drop) rl->AddRectFilled(r0, r1, theme::mix(P.surface800, P.accent, 0.22f), theme::kRadiusLg);
    else if (active) rl->AddRectFilled(r0, r1, v.stage, theme::kRadiusLg);
    else if (hov) rl->AddRectFilled(r0, r1, theme::withAlpha(P.surface700, 0.6f), theme::kRadiusLg);
    if (active || drop) rl->AddRectFilled(r0, ImVec2(r0.x + 3, r1.y), rc, 1.0f);
    const float cx = std::round((r0.x + r1.x) * 0.5f + 1), fw = 32, fh = 24;
    thumbnail(a, rl, i, ImVec2(cx - fw * 0.5f, r0.y + 6), ImVec2(fw, fh), active);
    if (n > 1) {
      const ImVec2 b0(std::round(cx - pw * 0.5f), r0.y + 6 + fh + 6), b1(b0.x + pw, b0.y + pw);
      phonesButton(a, rl, i, b0, b1);
    }
    const float y0 = std::max(r0.y, clip0.y), y1 = std::min(r1.y, clip1.y);
    if (y1 > y0) R.rows[static_cast<size_t>(i)] = st::Rect{r0.x, y0, r1.x, y1};
    ImGui::PopID();
  }
  // "+" under the rooms.
  const float py = std::round(top + n * (cellH + gap) + 2);
  ImGui::SetCursorScreenPos(ImVec2(std::round((left + right) * 0.5f - 12), py));
  if (ui::IconButton("##roomnew", ui::Icon::Plus, ImGui::IsPopupOpen("##roomnewmenu"), 24.0f, "New room: from this room's settings or a template")) {
    refreshMine(a);
    ImGui::OpenPopup("##roomnewmenu");
  }
  ImGui::SetNextWindowPos(ImVec2(right + 8, py));
  if (ImGui::BeginPopup("##roomnewmenu")) {
    newRoomMenu(a);
    ImGui::EndPopup();
  }
  rowPopup(a, openMenu);
  // Rename from the menu: the slim rail has no room for the name, so it is asked in a small popup.
  if (R.renaming >= 0 && R.renameFocus) ImGui::OpenPopup("##slimrename");
  ImGui::SetNextWindowPos(ImVec2(right + 8, R.renaming >= 0 && R.renaming < n ? top + R.renaming * (cellH + gap) : py));
  if (ImGui::BeginPopup("##slimrename")) {
    ImGui::TextDisabled("%s", i18n::tr("Rename the room"));
    if (R.renameFocus) ImGui::SetKeyboardFocusHere(), R.renameFocus = false;
    ImGui::SetNextItemWidth(200);
    const bool done = ImGui::InputText("##rn", R.name, sizeof R.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      R.renaming = -1;
      ImGui::CloseCurrentPopup();
    } else if (done) {
      if (R.name[0] && R.renaming >= 0 && R.renaming < n) roomops::renameRoom(a.doc, R.renaming, R.name);
      R.renaming = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  } else if (R.renaming >= 0 && !R.renameFocus) {
    R.renaming = -1;  // closed by a click away
  }
  ImGui::EndChild();
}

}  // namespace

bool railFits(App& a) {
  (void)a;
  const View& v = view();
  // K-270: the rooms column is there in every project (a one-room project lists its room and "+"); only a
  // mixer too small to hold it beside a stage (a docked layout's short strip) leaves it out, edge and all.
  return v.fullW >= kFitW && v.fullH >= kFitH;
}

bool railShown(App& a) { return railFits(a) && roomsPanelOpen(a); }

bool railSlim(App& a) { return railShown(a) && openWidth(a) <= kSlimW; }

bool roomsPanelOpen(App& a) { return a.settings.getNumber("ui.stage.roomsList") >= 0.5; }

void setRoomsPanelOpen(App& a, bool open, bool slide) {
  a.settings.set("ui.stage.roomsList", open ? 1 : 0);
  if (!slide) R.anim = open ? 1.0f : 0.0f;
}

void setRailWidth(App& a, float w) { setRoomsWidth(a, w); }

float railWidth(App& a) {
  if (!railFits(a)) return 0.0f;
  return std::round(kEdgeW + (openWidth(a) - kEdgeW) * easeOut(slideNow(a)));
}

const std::vector<st::Rect>& railRows() { return R.rows; }

const std::vector<st::Rect>& railPhones() { return R.phones; }

st::Rect railTab() { return R.tab; }

float railAnim() { return R.anim; }

int railMenuRoom() { return R.menuShown; }

int railRowAt(ImVec2 m) {
  for (size_t k = 0; k < R.rows.size(); ++k)
    if (R.rows[k].contains(m.x, m.y)) return static_cast<int>(k);
  return -1;
}

void moveToRoom(App& a, const std::vector<ObjectId>& ids, int room) {
  const int n = roomops::moveTracksToRoom(a.doc, ids, room);
  if (n <= 0) return;
  char b[200];
  if (n == 1) std::snprintf(b, sizeof b, i18n::tr("Moved 1 track to %s"), roomops::roomName(song(a), room).c_str());
  else std::snprintf(b, sizeof b, i18n::tr("Moved %d tracks to %s"), n, roomops::roomName(song(a), room).c_str());
  a.notify(b);
  a.mixerView.sel.clear();  // they left this stage
  a.selMixer = song(a).masterId();
}

void openBring(App& a, int room) {
  B.want = true;
  B.room = room;
  B.pick = 0;
  B.checked.clear();
  B.search[0] = 0;
  (void)a;
}

// The column from p0 to its wall at p1.x (as wide as railWidth: the edge, the slide, or open), and its tab
// half way down the stage at tabY. The tab and the wall's width handle are taken before the stage's own
// button (they stick out over it); the tab is drawn by drawRailTab, after the stage.
void drawRail(App& a, ImVec2 p0, ImVec2 p1, float tabY) {
  const auto& P = pal();
  View& v = view();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float t = R.anim, full = openWidth(a);
  const bool open = roomsPanelOpen(a);
  R.menuShown = -1;
  dl->AddRectFilled(p0, p1, theme::mix(v.bg, P.surface950, 0.35f));
  // The tab first: over the wall, and over the stage beside it.
  const float tx0 = p1.x - 1, ty0 = std::round(tabY - kTabH * 0.5f);
  R.tab = st::Rect{tx0, ty0, tx0 + kTabW, ty0 + kTabH};
  ImGui::SetCursorScreenPos(ImVec2(tx0, ty0));
  if (ImGui::InvisibleButton("##roomstab", ImVec2(kTabW, kTabH))) setRoomsPanelOpen(a, !open);
  R.tabHot = ImGui::IsItemHovered();
  R.tabDown = ImGui::IsItemActive();
  if (R.tabHot) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ui::Hint(open ? "Close the rooms column" : "Open the rooms column: show, add, solo and manage rooms");
  }
  if (t > 0.001f) {
    // Open, or sliding: laid out at its open width and slid in from the left, cut at its wall.
    ImGui::PushClipRect(p0, p1, true);
    const ImVec2 q0(p1.x - full, p0.y), q1(p1.x, p1.y);
    if (full <= kSlimW) drawSlim(a, q0, q1);
    else drawRows(a, q0, q1);
    ImGui::PopClipRect();
  } else {
    R.rows.clear();
    R.phones.clear();
    // Closed: the thin edge opens it on a click too.
    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##roomsedge", ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)))) setRoomsPanelOpen(a, true);
    if (ImGui::IsItemHovered()) {
      dl->AddRectFilled(p0, p1, theme::withAlpha(P.accent, 0.10f));
      ui::Hint("Rooms: click to open the rooms column");
      R.tabHot = true;
    }
  }
  // The wall: drag it to set the width (narrow enough: the slim rail; double-click: as it started).
  bool wallHot = false;
  if (open && t >= 1.0f) {
    ImGui::SetCursorScreenPos(ImVec2(p1.x - kWallGrip, p0.y));
    ImGui::InvisibleButton("##roomswall", ImVec2(kWallGrip + 3, std::max(1.0f, p1.y - p0.y)));
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) {
      R.resizing = true;
      R.live = R.resizeFrom = full;
      R.resizeAt = io.MousePos.x;
    }
    if (R.resizing && ImGui::IsItemActive()) R.live = R.resizeFrom + (io.MousePos.x - R.resizeAt);
    if (R.resizing && !ImGui::IsItemActive()) {
      setRoomsWidth(a, R.live < kSnapW ? kSlimW : std::clamp(R.live, kFullMin, std::max(kFullMin, std::min(kFullMax, v.fullW - kStageMin))));
      R.resizing = false;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) setRoomsWidth(a, kFullDef), R.resizing = false;
    wallHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (wallHot) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      ui::Hint("Drag: the rooms column's width (narrow: a slim rail of rooms) \xC2\xB7 double-click: as it started");
    }
  } else {
    R.resizing = false;
  }
  const bool lit = wallHot || (t <= 0.001f && R.tabHot);
  dl->AddLine(ImVec2(p1.x - 0.5f, p0.y), ImVec2(p1.x - 0.5f, p1.y), lit ? theme::withAlpha(P.accent, 0.8f) : P.line, lit ? 2.0f : 1.0f);
}

// The tab on the column's wall: an arrow out (closed) or back (open), turned with the slide; drawn after the
// stage so it sits on top. A dot while rooms are soloed and the column is closed.
void drawRailTab(App& a) {
  if (!railFits(a) || R.tab.w() <= 0) return;
  const auto& P = pal();
  const View& v = view();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float x0 = R.tab.x0, y0 = R.tab.y0, x1 = R.tab.x1, y1 = R.tab.y1, r = 6.0f;
  const ImU32 base = theme::mix(v.bg, P.surface950, 0.35f);
  const ImU32 fill = R.tabDown ? theme::mix(base, P.text, 0.14f) : R.tabHot ? theme::mix(base, P.text, 0.08f) : base;
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill, r, ImDrawFlags_RoundCornersRight);
  // Its outline on three sides: the fourth is the column it belongs to.
  dl->PathLineTo(ImVec2(x0 + 0.5f, y0 + 0.5f));
  dl->PathArcTo(ImVec2(x1 - r - 0.5f, y0 + r + 0.5f), r, -kPi * 0.5f, 0.0f, 6);
  dl->PathArcTo(ImVec2(x1 - r - 0.5f, y1 - r - 0.5f), r, 0.0f, kPi * 0.5f, 6);
  dl->PathLineTo(ImVec2(x0 + 0.5f, y1 - 0.5f));
  dl->PathStroke(R.tabHot ? theme::withAlpha(P.accent, 0.8f) : P.line, ImDrawFlags_None, 1.0f);
  // The arrow: "›" closed, "‹" open, turned through the slide.
  const float d = std::cos(kPi * easeOut(std::clamp(R.anim, 0.0f, 1.0f)));
  const float cx = std::round((x0 + x1) * 0.5f) + 0.5f, cy = std::round((y0 + y1) * 0.5f) + 0.5f;
  const ImU32 ac = R.tabHot ? P.text : P.textDim;
  const ImVec2 pts[3] = {ImVec2(cx - 2.0f * d, cy - 4.5f), ImVec2(cx + 2.0f * d, cy), ImVec2(cx - 2.0f * d, cy + 4.5f)};
  dl->AddPolyline(pts, 3, ac, ImDrawFlags_None, 1.6f);
  if (R.anim < 0.5f && !v.soloRooms.empty()) dl->AddCircleFilled(ImVec2(cx, y1 - 7.0f), 2.2f, P.accent);
  if (R.anim < 0.5f && !v.soloRooms.empty() && R.tabHot) ui::Hint("Open the rooms column \xC2\xB7 some rooms are soloed: only they play here");
}

// Outside the rail too (popups opened from its menus, kept while it slides or closes).
void roomPopups(App& a) {
  savePopup(a);
  bringDialog(a);
}

}  // namespace detail

void applyRoomTemplate(App& a, const roomops::RoomTemplate& t) {
  roomops::applyTemplate(a.doc, detail::shownRoom(a), t);
  char b[160];
  std::snprintf(b, sizeof b, i18n::tr("%s now sounds like %s"), roomops::roomName(a.doc.song(), detail::shownRoom(a)).c_str(),
                t.factory ? i18n::tr(t.name.c_str()) : t.name.c_str());
  a.notify(b);
}

void newRoomFrom(App& a, const roomops::RoomTemplate& t) {
  setView(a, true);
  detail::newRoom(a, detail::shownRoom(a), &t);
}

void confirmDeleteTemplate(App& a, const roomops::RoomTemplate& t) {
  if (t.factory) return;
  std::vector<char> title(t.name.size() + 128);
  std::snprintf(title.data(), title.size(), i18n::tr("Delete the room template '%s'?"), t.name.c_str());
  a.confirm = {};
  a.confirm.title = title.data();
  a.confirm.subtitle = i18n::tr("Its file is deleted. Rooms that use its settings keep them.");
  a.confirm.okLabel = i18n::N_("Delete");
  a.confirm.action = [&a, t] {
    roomops::deleteUserTemplate(t);
    detail::refreshMine(a);
  };
  a.confirm.open = true;
}

void listenFilter(App& a, Song& s) {
  (void)a;
  auto& solo = detail::view().soloRooms;
  if (solo.empty()) return;
  // Soloed rooms that are gone (deleted here or by a collaborator) drop out; one room left: no solo.
  solo.erase(std::remove_if(solo.begin(), solo.end(), [&s](ObjectId id) { return id == kNoId || roomops::roomIndex(s, id) < 0; }), solo.end());
  if (roomops::roomCount(s) < 2) solo.clear();
  // Their tracks, their spaces and everything downstream play; the rest is muted in the engine's copy.
  roomops::hearOnly(s, solo);
}

}  // namespace gl::stageui

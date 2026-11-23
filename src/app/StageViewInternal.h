// Gridline - the Stage view's parts shared by app/StageView.cpp (the stage, its gestures, the toolbar)
// and app/StagePanel.cpp (the control panel, Arrange, the room and preset popovers, the first-run card).
// Not for other files: StageView.h is the public face.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/StageArrange.h"
#include "../core/StageCompact.h"
#include "../core/StageMap.h"
#include "../core/Types.h"

namespace gl {
class App;
struct MixerTrack;
struct Song;
struct StageSettings;
namespace stageui::detail {
namespace st = stage;

// One track on the stage this frame.
struct Tok {
  ObjectId id = kNoId;
  size_t index = 0;  // mixer index
  double pan = 0, depth = 0, sep = 0, focus = 0, volume = 0.8;  // shown (live while automation plays)
  double sPan = 0, sDepth = 0;                                    // stored
  bool live = false;
  int group = -1;  // its group (View::groups), -1 on its own
  int cell = -1;   // its cell in the group's tray (-1: behind "+N")
  st::Rect r;      // the token on screen (on its own: after the display separation; in a group: its cell's token,
                   // or with compact groups while the group is closed, its chip's face)
  st::Rect cellR;  // in a group: its cell's token in the tray (also while a compact group is closed)
  int icon = 0;    // the track's icon, else the automatic one (0: initials)
  std::string name;
  bool muted = false, solo = false, dim = false, selected = false;
};

// Tracks that share one spot: a tray of cells above the spot's ring.
struct Grp {
  std::vector<size_t> toks;  // View::toks indexes, mixer order
  st::Pos spot;
  st::Pt ring;               // the spot on the floor
  st::Tray tray;
  st::Chip chip;     // compact groups: the stacked chip at the spot while it is closed
  float open = 1.0f;  // compact groups: 0 the chip .. 1 the tray (always 1 without compact groups)
};

enum class PreviewKind { None, Spread, Sound, Preset };
// Where Spread out, Arrange by sound or a preset would put the tracks, before Apply.
struct Preview {
  PreviewKind kind = PreviewKind::None;
  int preset = -1;
  std::vector<ObjectId> ids;
  std::vector<st::Pos> to;
  std::vector<st::Guess> why;  // Arrange by sound only
  int unsure = 0;
  bool active() const { return kind != PreviewKind::None; }
};

struct View {
  st::Geo g;
  ImVec2 body0{0, 0}, body1{0, 0};  // the whole body under the toolbar
  ImVec2 panel0{0, 0}, panel1{0, 0};
  ImU32 bg = 0, stage = 0, tray = 0;  // colours of the frame
  std::vector<Tok> toks;
  std::vector<Grp> groups;
  std::map<ObjectId, ObjectId> prevGroup;  // groups' hysteresis: track -> its group's first member last frame
  ObjectId hovered = kNoId;
  Preview preview;       // shown with Cancel and Apply in the panel
  Preview hoverPreview;  // a preset row under the pointer (outlines only)
  int hoverPresetFrame = -10;
  bool roomOpen = false;     // the room popover is open: the stage gets an outline
  ObjectId dragId = kNoId;   // a track being dragged on the stage: the panel shows where the drop puts it
  double dragPan = 0, dragDepth = 0;
  bool cardOpen = false;     // the first-run card is up
  ObjectId room = kNoId;     // K-224: the room shown (StageSettings::id; kNoId: the first). This computer only
  std::vector<ObjectId> soloRooms;  // K-270: the rooms whose headphones are on (ids; empty: every room plays). This computer only, never saved
  float fullW = 0, fullH = 0, fullX = 0;  // K-224: the body before the rooms column takes its side
  bool trayOnFloor = false;  // K-270: a stage too short for its trays: they stay above the front edge
  int dropRoom = -1;         // K-224: the rooms list's row under tracks being dragged (-1: none)
  int dragCount = 0;
  ObjectId fxTrack = kNoId;  // the effects popover's track
  bool wantFx = false;       // open it this frame
  int frame = -1;
  bool open = false;
  bool compact = false;  // compact groups (ui.stage.compactGroups, on by default; GRIDLINE_STAGE_COMPACT=0 / 1 overrides it)
  // The Tracker layout's Stage page (StageTrackerUi.cpp): the Tracker look (square, bevelled, its console
  // column at the right). Set by drawBody each frame.
  bool trk = false;
  ImVec2 side0{0, 0}, side1{0, 0};  // the Tracker look: its console column (empty otherwise)
  ImVec2 spaceAt{0, 0};             // the Tracker look: under its space button (the room's hint shows there)
  int folded = 0;                   // the Tracker look: the column's boxes shown folded (bits: Arrange, Track, Place, Level)
  int groupRowsFit = 0;             // the Tracker look: the group rows its Groups list has room for
};
View& view();
// A corner radius as drawn: none in the Tracker look (its boxes are square), else `r`.
inline float corner(float r) { return view().trk ? 0.0f : r; }

// ---- shared with the mixer (App::selMixer + MixerViewState::sel) ----
std::vector<ObjectId> selection(App& a);
bool isSelected(App& a, ObjectId id);
void selectOnly(App& a, ObjectId id);
const Tok* tokOf(ObjectId id);
// The tracks the stage shows (mixer indexes, mixer order, the shown room's only, never the master or a
// space track).
std::vector<size_t> shownTracks(App& a);
std::vector<size_t> shownTracksOf(App& a, int room);  // K-224: the same for any room
// K-224: the room shown on the stage (an index, core/StageRoomOps.h) and its settings.
int shownRoom(App& a);
const StageSettings& shownStage(App& a);
void showRoom(App& a, int room);
// Moves room `from` to place `to` (one undo step); the room shown stays shown, wherever it ends up.
bool moveRoomKeepShown(App& a, int from, int to);

// ---- words (translated) ----
std::string panText(double pan);        // "Centre", "60% right"
std::string dbText(double gain, bool sign);  // "-12 dB" (gain is linear amplitude)
std::string levelText(double volume);   // the fader in dB

// ---- roles ----
st::Guess guessOf(App& a, size_t index);   // what plays on it, how it sounded, its name
int autoIcon(App& a, size_t index);        // the role's icon when the track has none of its own

// ---- arranging ----
void startPreview(App& a, PreviewKind kind, int preset = -1);
Preview presetPreview(App& a, int preset);
void applyPreview(App& a);
void cancelPreview();
// The analysis of what each track played (Arrange by sound): fed every frame while playing.
void feedAnalysis(App& a);

// ---- drawing helpers ----
void dashedRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float thick = 1.0f, float dash = 4.0f, float gap = 3.0f);
void drawFace(App& a, ImDrawList* dl, const Tok& t, ImVec2 mn, float side, float alpha);  // the tile and its icon
ImU32 faceOf(const MixerTrack& t, double depth, ImU32 stage);

// ---- StageRoomsUi.cpp (K-224, K-270) ----
// The rooms column down the stage's left: closed, a thin edge with a tab (an arrow out); open, rows or, dragged
// narrow, a slim rail of rooms. Open or closed and its width are this computer's (settings), never the song's.
bool railFits(App& a);    // the mixer is big enough for it beside a stage (else it is left out, edge and all)
bool railShown(App& a);   // open (rows or the slim rail), and it fits
bool railSlim(App& a);    // open as the slim rail
bool roomsPanelOpen(App& a);
void setRoomsPanelOpen(App& a, bool open, bool slide = true);  // slide: animated (false: at once, dev input)
void setRailWidth(App& a, float w);  // the open width asked for (under 150: the slim rail)
float railWidth(App& a);  // what it takes from the stage this frame: 0 (does not fit), the edge, sliding, or open
void drawRail(App& a, ImVec2 p0, ImVec2 p1, float tabY);
void drawRailTab(App& a);  // the tab, over the stage (after it is drawn)
st::Rect railTab();        // the tab last frame (dev checks)
float railAnim();          // the slide, 0 closed .. 1 open (dev checks)
int railRowAt(ImVec2 m);  // the room whose row is under `m` (-1: none)
const std::vector<st::Rect>& railRows();  // each room's row on screen last frame (dev checks)
const std::vector<st::Rect>& railPhones();  // K-270: each room's headphones last frame (dev checks)
int railMenuRoom();  // K-270: the room whose right-click menu is open (-1: none; dev checks)
void roomPopups(App& a);  // the template name and Bring tracks in popups
std::string deleteRefusal(App& a, int room);  // K-270: why it cannot be deleted ("": it can)
void requestDeleteRoom(App& a, int room);     // K-270: refused with the reason, or asked first
void openBring(App& a, int room);
// Moves tracks to another room (one step; inside a drag it joins the drag's step) and says so.
void moveToRoom(App& a, const std::vector<ObjectId>& ids, int room);

// ---- StagePickUi.cpp: tracks from the playlist (core/StagePick.h) ----
void pickMenu(App& a);                 // "From playlist": the two sources, their tracks and the actions
void openPickPanel(App& a, int source);  // the "Tracks from the playlist" panel (0 the selection, 1 a marker area)
void pickPopups(App& a);               // each frame in the body: the panel, the palette's commands
bool pickPanelOpen();
bool takeMenuRequest();  // dev: open the toolbar's From playlist menu
void devPick(App& a, const std::vector<std::string>& args);  // "stage pick ..." (dev input)

// ---- edits shared with the Tracker look (StageView.cpp; each one undo step, as the keys make them) ----
void nudgeParam(App& a, const std::vector<ObjectId>& ids, int mixerParam, double delta, double lo, double hi, const char* label);
void stepLevel(App& a, const std::vector<ObjectId>& ids, double notches);
void muteTracks(App& a, const std::vector<ObjectId>& ids);
void soloTracks(App& a, const std::vector<ObjectId>& ids);
// The toolbar's popovers asked for from elsewhere (the Tracker look's buttons): opened on this frame.
void openPresetsPopover();
void openRoomPopover();
void stageMenuItems(App& a);  // the stage menu (as the toolbar's menu button shows it)

// ---- StageTrackerUi.cpp: the Tracker layout's Stage page ----
// The Tracker look: on while the Tracker layout's pages show the stage.
bool trackerLook(App& a);
// The colours of the frame in the Tracker look (View::bg, stage, tray).
void trackerColours();
// The floor as the Tracker draws it (a sunken well, hard lines, the distance rows and the pan scale).
void trackerFloor(App& a, ImDrawList* dl);
void trackerListener(ImDrawList* dl);
// A raised bevel on a tile or chip (its edges only).
void trackerBevel(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool sunken = false);
// The console column at the right: Arrange (the stage's buttons), the selection's boxes, the groups.
void trackerSide(App& a);
// The popovers and the first-run card in the Tracker look (StagePanel.cpp calls them there).
void trackerPresets(App& a);
void trackerRoomPopover(App& a);
void trackerFirstRun(App& a);
// A footprint on the floor (a flattened diamond) and a width handle (a small square).
void trackerFoot(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 col);
void trackerFootDashed(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 col);
void trackerHandle(ImDrawList* dl, ImVec2 c, float r, ImU32 col, ImU32 ring);

// ---- StagePanel.cpp ----
void drawPanel(App& a);
void presetsMenu(App& a);
void roomPopover(App& a);
void effectsPopover(App& a, ObjectId id);
void firstRunCard(App& a);
void openEffects(App& a, ObjectId id);

}  // namespace stageui::detail
}  // namespace gl

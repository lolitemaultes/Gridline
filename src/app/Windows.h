// Gridline — window entry points (each draws one dockable window or bar).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Params.h"
#include "../core/Types.h"
#include "ClipHeaderHit.h"
#include "MidiUi.h"

namespace gl {

class App;

float drawTopBar(App& a);      // menu + transport; returns height used
// Dev checks (J-91): where the top bar drew a button last frame ("play", "stop", "rec", "metro"), in
// logical pixels; false when it is not shown. `mirror`: the metronome glyph's last horizontal scale.
bool topBarButtonRect(const char* name, float& x0, float& y0, float& x1, float& y1);
void devNoteRect(const char* name);  // the last item's rectangle as an "sdlat" target (browser: "brsearch", "brclear", "brcollapse"; channel window: "chroute", "chroutefree", "chroutelist"; piano roll: "prstamp", "prstamplbl", "prstampchip")
void devNoteRectAt(const char* name, float x0, float y0, float x1, float y1);  // K-237: a rectangle as an "sdlat" target ("ptabsrc", "ptabdst": a pane tab drag)
// Smooth's control bar: each pane button drawn last frame ("lb_lists", "lb_ed", ...) and whether it was lit.
std::string devBarLit();
void splitClipsAtPlayhead(App& a);  // K-112b: the clips across the song position split there (Dialogs.cpp)
void tracksNewSameSettings(App& a);
void tracksMuteSelected(App& a);  // R04-11: M, the selected tracks (or the focused one)
void tracksSoloSelected(App& a);  // R04-11: S  // K-112B: the Tracks layout's Track menu and keys (MenuStyles.cpp)
void tracksDeleteSelected(App& a);
// R04-09: the Tracks layout's Track menu: a stack over the selected tracks (one run of neighbours,
// none in a stack yet), freeze, bounce in place (tracks, regions), and the header's parts.
void tracksCreateStack(App& a);
bool tracksCanStack(App& a);
void tracksFreeze(App& a);
bool tracksFrozen(App& a);
bool tracksCanFreeze(App& a);
void tracksBounceInPlace(App& a);
void tracksRenderMenu(App& a);  // K-254: the selected track's instrument to audio (a submenu)
void regionsBounceInPlace(App& a);
// The header's parts the person hid (Configure track header): bits of ui.logicHeaderHide.
enum TrackHeaderPart : int { kHeadIcon = 1, kHeadPower = 2, kHeadMuteSolo = 4, kHeadRecord = 8, kHeadVolume = 16, kHeadPan = 32 };
bool trackHeaderShows(App& a, int part);
void configureTrackHeaderItems(App& a);  // the parts as checked menu items
void tracksSplitAtPlayhead(App& a);
void tracksAddMarker(App& a);  // at the nearest bar
void toggleCycle(App& a);
// K-314 (A-05): the range playback loops over: outside Clips the time selection (Studio's selection,
// Smooth's cycle, the Tracker's Loop block), else the song's saved loop while on (Clips: that only).
// False when none.
bool playLoop(const App& a, Tick& from, Tick& to);
// K-314 (A-06): each frame; gives the time selection Smooth borrowed for its cycle back once it is left.
void cycleLayoutFrame(App& a);
// R02-13: the cycle area over the selected regions (Cmd+U), Cycle on. Nothing selected: no change.
void cycleToSelection(App& a);
// Checkpoint 47: the Tracks layout's Join, for the tool, the menus, the key and the toolbar (Playlist.cpp).
// kind: the regions' kind to join (ClipKind::Automation: the first selected region's kind).
void tracksJoin(App& a, ClipKind kind = ClipKind::Automation);
// Checkpoint 47: the arrow keys (previous / next region and track), Option-arrow nudge and L (loop).
void tracksNavKeys(App& a);
// Checkpoint 47: the locators are the cycle's range while Cycle is on, else the range the project
// keeps (the grey area). False when none is set. The playhead to one of them, and the locators over
// the selected regions without turning Cycle on (on: the cycle moves there).
bool locators(const App& a, Tick& left, Tick& right);
void gotoLocator(App& a, bool right);
void locatorsToSelection(App& a);
// R02-15: the playhead to the previous / next marker (dir -1 / +1), and the Go to position prompt.
void gotoMarker(App& a, int dir);
void goToPositionPrompt(App& a);  // K-112b: cycle on or off (the Tracks control bar, Navigate menu and C key)
void devTopBarFitWidth(float w);  // K-132: dev, the top bar fits its groups into w px (0 = the window's width)
float topBarMetronomeMirror();
double topBarMetronomeBeat();  // dev: the beat that pose was drawn for
bool topBarMetronomeTicking(double* anchorBeat);  // dev: ... whether it ticked then, and the beat it started in
void clipsMetronomeState(bool* leftLit, bool* menuOpen);  // dev: the Clips metronome's beat pose (left circle filled) and its menu
void drawStatusBar(App& a);    // hint bar + notifications
void drawNotificationCards(App& a);  // J-141: the notification cards alone (the launcher has no status bar)
void drawBrowser(App& a);
void browserProjectFiles(App& a);  // K-106: recent files and the project folder, with the browser's rows (Tracker Files screen)
// K-106: the Tracker Files screen's file operations, as the browser's: rename ("" when done, else the
// error), move to the trash (asks first, in the dialog browserTrashDialog draws every frame).
// R7-02: Files has its own rule (any folder the user may write in, but shipped content, the root and
// home themselves, the open project and a shared project's files).
std::string filesRename(App& a, const std::string& path, const std::string& name);
void filesAskTrash(const std::string& path);
// Why Files may not rename or trash `path` ("" when it may); `asParent`: a new folder goes in it.
std::string filesLocked(App& a, const std::string& path, bool asParent);
// True once, on the frame after the trash prompt Files asked closed (moved or cancelled): the list reads again.
bool filesTrashMoved();
void browserTrashDialog(App& a);
void drawChannelRack(App& a);
// J-84 dev checks: --rack <spec> (comma separated: steps<N> pattern length,
// w<W> / h<H> floating rack size, dock = docked beside the browser, scroll<px>
// step area scroll) and the --dev-input "rack" dump (layout, scroll, values).
void rackDevSpec(const std::string& spec);
void rackDevBefore(App& a, int frame);  // right before drawChannelRack
void rackDevAfter(App& a, int frame);   // right after it
void rackDevDump(App& a, int frame);
// The last ImGui item's rectangle, listed as "rect <name>" by the "rack" dump (rack menus: scripted checks).
void devRackItem(const std::string& name);
void drawPianoRoll(App& a);
// K-262: the Vibrato tool's defaults (preset, depth, rate, sync) for the current pattern and channel, in a
// toolbar row; a change also sets the selected notes' vibrato. The Tracker's Roll page opens it in a popup.
void pianoVibratoStrip(App& a);
// Dev ("pianobar"): Studio's piano roll toolbar last frame: the Vibrato tool's icon (1 shown, 0 gave
// way to the zoom buttons, -1 no tool row) and the zoom buttons (1 shown, 0 not, -1 no tool row).
void pianoToolbarState(int& vibIcon, int& zoomButtons);
// Dev ("pianobar"): Studio's tool row folding: "row N more 0", or "row N more 1 at x0 y0 x1 y1 list
// x0 y0 x1 y1 pitch h hidden Name,Name" (the "..." button, its open list's first row and row pitch,
// the folded tools in the list's order; list -1 when closed).
std::string pianoToolbarFold();
// Q-4 dev check (--dev-input "lane"): the piano roll's event lane as drawn last
// frame, in logical pixels: x of tick t = x0 + t * pxPerTick; value v (0..1) at
// y = yBottom - v * (yBottom - yTop).
struct PianoLaneGeometry {
  float x0 = 0, pxPerTick = 0, yTop = 0, yBottom = 0;
  bool valid = false;
  float pitchY0 = 0, rowH = 0;  // K-20: the note grid: pitch p's row starts at pitchY0 + (127 - p) * rowH
  int preview = -1;  // K-21: the key the mouse is sounding (-1: none)
  // The view: scroll (px), the grid's box, and the scrollers: the horizontal
  // thumb's x span at height hBarY, the vertical thumb's y span at vBarX.
  float scrollX = 0, scrollY = 0, gridX0 = 0, gridY0 = 0, gridX1 = 0, gridY1 = 0;
  float hThumb0 = 0, hThumb1 = 0, hBarY = 0, vThumb0 = 0, vThumb1 = 0, vBarX = 0;
};
const PianoLaneGeometry& pianoLaneGeometry();
void drawPlaylist(App& a);
// J-118 dev check (--dev-input "plgeom"): the playlist as drawn last frame, in
// logical pixels: x of tick t = x0 + t * pxPerTick; track i spans
// [trackTop[i], trackTop[i] + trackH[i]); the picker's tabs and rows
// (centre points; rows of the shown tab: source id).
struct PlaylistGeometry {
  float x0 = 0, pxPerTick = 0;
  std::vector<float> trackTop, trackH;
  float tabX[3] = {0, 0, 0}, tabY = 0;
  struct Row {
    unsigned id;
    float x, y;
  };
  std::vector<Row> rows;
  // J-142: every clip drawn, with its title strip's menu arrow and name box.
  struct ClipHead {
    unsigned id;
    clipheader::Geom g;
  };
  std::vector<ClipHead> clips;
  // J-173: a point on each pattern automation curve drawn on a pattern clip (the middle of its visible part).
  struct Curve {
    unsigned clip, owner, index;
    float x, y;
  };
  std::vector<Curve> curves;
  // K-388: the divots of Smooth's looped regions drawn last frame: the seam's left x (a device-pixel
  // edge), the divot's centre x, the loop point's tick from the region start, and whether the top one shows.
  struct Divot {
    unsigned clip;
    float x, cx;
    long long at;
    bool top;
  };
  std::vector<Divot> divots;
  // J-208: the picker's "+" (new pattern) button centre (0 0 when not drawn) and the
  // track a dragged plugin would land on (-1 = none this frame).
  float plusX = 0, plusY = 0;
  int dropTrack = -1;
  int renaming = -1;  // the track whose name field is open (-1: none)
  float cloneX = 0, cloneY = 0;  // a pattern menu's Clone item, the last frame one was drawn (0 0: none yet)
  bool loopBadge = false;  // K-140: the loop badge drawn by the pointer last frame (its centre)
  float loopBadgeX = 0, loopBadgeY = 0;
  // K-24: the scrollers' thumbs (vertical: screen y of its top / bottom end; horizontal: x of its
  // left / right end) and the bars' centre lines (vertical: x, horizontal: y), the vertical
  // scroll, the base track height and the grid's rectangle (screen).
  float vThumb0 = 0, vThumb1 = 0, hThumb0 = 0, hThumb1 = 0, vBarX = 0, hBarY = 0;
  float scrollY = 0, trackHeight = 0, gridX0 = 0, gridY0 = 0, gridY1 = 0;
  float headX0 = 0, headX1 = 0;  // GitHub #46: the track headers' column (screen x)
  int dragBefore = -1;           // ... and where a header drag would drop now (-1: none)
  bool valid = false;
};
const PlaylistGeometry& playlistGeometry();
// 0.7.8 (dev): the time zoom sliders in Smooth's tracks area (0) and piano roll (1): the
// track's ends and centre line on screen, and the frame they were last drawn (-1: never).
struct ZoomSliderGeometry {
  float x0 = 0, x1 = 0, y = 0;
  int frame = -1;
};
ZoomSliderGeometry& zoomSliderGeometry(int which);
// 0.7.8: a time zoom slider's fine drag. Called right after the slider's button: f is the value it shows
// (0..1), x0 and span its line on screen, knobX the knob's centre. A press on the knob keeps the value; a
// press on the line beside it jumps there. While held the pointer's motion moves the value relative to the
// press, the whole range over kZoomSliderTravel px (Shift: a quarter of that); a wheel over it steps finely.
// Returns the new value while held or wheeled, else -1.
constexpr float kZoomSliderTravel = 250.0f;
float zoomSliderFine(float f, float x0, float span, float knobX);
// K-112b (dev, "autolane <track>"): the Tracks layout shows the automation lane under that playlist track.
void devShowAutomationLane(int track);
// R02-15: A / Mix > Show automation: the selected tracks' automation lanes (the focus track's when
// no header is selected) show, or hide when one of them shows. Runs in the next tracks frame.
void toggleAutomationLanes();
// R01-15: W in the Tracks layout: the audio editor in the lower pane (the selected audio region's
// sound, else the focus track's first audio region's); pressed while it shows, the pane closes.
void toggleTracksAudioEditor(App& a);
void openToolMenu();  // R01-18: T: the tracks area's tool menu opens (next frame)
bool automationLanesShown(const App& a);
// K-112c (dev, "plrows"): the next drawn playlist prints its rows (index, name, a point on the header's name: 24 px in, mid-row).
void devPlaylistRowsAsk();
// K-112b dev input "menu open <id>" (the same name and syntax as the other layouts'): asks the
// menu `id` to open on the next frame (the bar's menus by name, e.g. File; "track": the track
// header menu; "region": the region menu; "strip": a strip's Setting menu). devMenuTake: true
// once, where that menu lives, when it was asked for.
void devMenuOpen(const std::string& id);
bool devMenuTake(const char* id);
// K-112b: the playlist's track height each layout starts at until the person sets one (px).
float playlistDefaultTrackHeight(bool tracksLayout);
void notePatternMenuClone(float x, float y);  // dev (plgeom): where a pattern menu drew its Clone item
void drawMixer(App& a);
// J-83 dev check (--dev-input "mixer"): the strips pane's vertical scroll as
// drawn last frame, and whether the plain wheel scrolls the mixer there.
float mixerStripsScrollY();
float mixerStripsScrollX();  // K-176
bool mixerWheelScrolls();
// Dev check (--dev-input "mixer"): where the mixer's strip headers and some menu rows (mixer track,
// playlist track and pattern menus) were drawn in the last two frames.
void devMenuItem(const std::string& name);
// K-158: a clip's whole playlist menu (--menu arrclip; K-112: also Live's grid slots).
void devArrangementClipMenu(App& a, ObjectId clip);
void devMenuRects();
std::string mixerDragDev();  // GitHub #46: the strip drag ("mixer" dev dump)
void drawChannelSettings(App& a);
// Dev checks (--dev-input): "strip [on|off] [frames]", the channel strip of the
// channel settings window (ChannelStrip.h) as drawn this frame and the next
// `frames` frames: shown, shown fraction, heights, the eye button, window,
// keyboard and volume knob rects ("on" / "off" first show or hide it at once,
// a test set-up); "plugwin chan <index> | fx <mixer index> <slot> | close"
// opens or closes plugin windows. True when `cmd` was one of them.
bool pluginWindowDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame);
void drawChannelEnvelope(App& a, ObjectId channel);  // channel envelope / LFO / filter page (J-72)
void drawEffectEditor(App& a);
void drawDialogs(App& a);
void devShowPpqWarning(App& a, int ppq);  // J-54 layout check (--show ppqwarn): the lower-timebase confirmation
// CPU profiling (Performance.cpp): samples the engine counters into App::perf
// (per frame) and draws the "Performance" window.
void updatePerformance(App& a);
void drawPerformance(App& a);
// 0.7.8: the width the Tracker's Performance panel asks for (its content, the side panel's rail left
// out), from the interface's fonts and language; and the dev report of its last layout ("perfpanel").
float performanceTrackerWidth(App& a);
void performanceDevReport(App& a, int frame);
void drawPluginManager(App& a);  // Options > Plugin manager (PluginManager.cpp)
// Arrangements and live mode (Live.cpp).
void drawLivePads(App& a);                         // "Live pads" window
void drawMasterPanel(App& a);                      // RN-09 "Master chain" window
void arrangementMenu(App& a);                      // menu body: list, new, duplicate, rename, delete
void arrangementSelector(App& a, float width);     // toolbar combo
void liveTrackMenu(App& a, int track);             // menu body: a track's live settings
void setLiveArea(App& a, Tick end);                // 0 removes it      // file dialog, settings, info, export, history, about, prompts
void handleGlobalShortcuts(App& a);
void closeFrontWindow(bool dialogsOnly);  // K-297's shared close of the front window (Cmd+W; dialogsOnly: Escape's)
// One undo step per pointer gesture in every editor (Document::beginPointerGesture): opened
// right after ImGui::NewFrame when a mouse button is down or the wheel turns, closed by
// handleGlobalShortcuts after the frame of the release (a wheel burst: after 0.4 s and 8 frames
// without wheel).
void pointerGestureFrame(App& a);
// J-141: the frame-end half of it (handleGlobalShortcuts runs it; the launcher has no shortcuts pass).
void pointerGestureFrameEnd(App& a);
// Knob grid for a plugin's parameters (effects and instruments).
void pluginParamGrid(App& a, const PluginInfo& info, const std::function<ParamId(int)>& pidOf, float width);

// Shared menu fragments.
void menuFile(App& a);
void menuEdit(App& a);
void menuAdd(App& a);
void menuPatterns(App& a);
void menuView(App& a);
void menuOptions(App& a);
void menuHelp(App& a);
void menuTools(App& a);
// Per frame (from handleGlobalShortcuts): menu shortcuts, F2 prompt, edit focus.
// Returns true when it handled a key (the older global shortcuts then skip it).
bool menuShortcuts(App& a);
// ImGuiTabItemFlags for settings tab `index` (selects it once after Options > … settings).
int settingsTabFlags(App& a, int index);

// Context menu bodies (call between BeginPopup/EndPopup) — ContextMenus.cpp.
void channelContextMenu(App& a, ObjectId channel);
void rackOptionsMenu(App& a);      // channel rack menu: selection ops, sorting, colours
// GitHub #58: hide channels from the rack (one undo step; the selection leaves them), and the menu
// rows that show hidden ones again (each, or all).
void hideChannels(App& a, const std::vector<ObjectId>& ids);
void hiddenChannelItems(App& a);
// K-4: "Auto scrolling" menu item: the playlist's (ui.autoScroll) or, with `pattern`, the piano
// roll's and channel rack's (ui.autoScrollPattern).
void autoScrollItem(App& a, bool pattern);
void addChannelMenu(App& a);       // instruments by category
void patternContextMenu(App& a, ObjectId pattern);
// J-82 (Playlist.cpp): playlist picker source menus and deleting sources.
// Used sources ask first (App::confirm); unused ones are deleted at once.
void audioSourceMenu(App& a, ObjectId channel);
void automationSourceMenu(App& a, ObjectId automation);
void patternSourceMenu(App& a, ObjectId pattern);
void requestDeleteSources(App& a, ClipKind kind, std::vector<ObjectId> ids);
void playlistTrackMenu(App& a, int track);
// GitHub #46 (Playlist.cpp): the tracks a header gesture on `track` moves (the header selection when
// it holds `track`, else the track alone), and moving them one place (dir -1 up, 1 down; the
// selection follows them).
std::vector<int> playlistTrackSelection(App& a, int track);
bool stepPlaylistSelection(App& a, const std::vector<int>& tracks, int dir);
void clipContextMenu(App& a, ObjectId clip);
// K-112: with the clip's own rows (`clipRows`, true when it edited the song) placed in the menu by
// Live's order; Studio's order ignores them (its caller draws them above the menu).
void clipContextMenu(App& a, ObjectId clip, const std::function<bool()>& clipRows);
// N46-08: set while a Clips grid slot's menu draws the clip menu (the slot has its own edit block).
extern bool gClipMenuFromSlot;
void playlistOptionsMenu(App& a, int part = 0);  // part: 0 every menu, 1 edit, 2 functions, 3 view (K-112b)
void pianoRollMenu(App& a, int part = 0);  // part: 0 every menu, 1 edit, 2 functions, 3 view (K-112b)
void markerMenu(App& a, int index);  // playlist ruler: one marker (J-11)
void markerAddItems(App& a);         // playlist menu > Time markers: add at the selection, every N bars (J-11)
// J-85: a song loop marker at `pos` snapped to the playlist grid (one undo
// step; playback reaching the song end goes on from it).
void addLoopMarker(App& a, Tick pos);
void rulerMenu(App& a, Tick pos);    // right-click on the playlist ruler (J-85)
void mixerTrackMenu(App& a, size_t index);
void mixerSlotMenu(App& a, ObjectId track, int slot);
// The effect list a mixer slot offers (favourites, categories, installed): a pick puts the effect
// in (track, slot) and opens it (K-112b: the strips' empty slots and Replace).
void effectPickerList(App& a, ObjectId track, int slot, const std::string& current);
// The same list as a slot's popup `id` (opened with ImGui::OpenPopup(id) beside the slot's item):
// its own width (the list's widest row, never the slot's), below the slot or above it, flipped left
// at the window's right edge, always inside the window (a long list scrolls inside it).
void effectPickerPopup(App& a, const char* id, ImVec2 slotMin, ImVec2 slotMax, ObjectId track, int slot, const std::string& current);
std::string devSlotPickReport();  // dev "slotpick": the picker popup drawn last (its rectangle, scrollbar, the room it had)
// Mixer (MixerView.cpp, J-12): selected tracks in mixer order (the focused
// one when nothing else is selected), the send menu of a route arrow, the
// mixer options menu, the track latency menu, per-track stem renders.
std::vector<ObjectId> mixerSelection(App& a);
// Selects every insert track (never the master; the focus moves to the first
// insert when it was on the master), or back to just the focused track.
void mixerSelectAll(App& a);
void mixerDeselect(App& a);
// Mixer track menu Select items (also Alt+L and Ctrl+Alt+L on the focused track): the channels
// routed to `track` (shown in the channel rack), or the mixer tracks routed to it.
void mixerSelectRoutedChannels(App& a, ObjectId track);
void mixerSelectRoutedTo(App& a, ObjectId track);
// A reset of a mixer control covers the selected tracks when the track acted
// on is one of several selected. Pending: that applies and one of them is
// away from the default. Selected: does it (one undo step); false when it
// does not apply, and the caller resets the one control.
bool mixerResetPending(App& a, ParamId pid);
bool mixerResetSelected(App& a, ParamId pid);
// Items of the "Reset selected tracks" menu for these tracks.
void mixerResetItems(App& a, const std::vector<ObjectId>& tracks);
void mixerSendMenu(App& a, ObjectId from, ObjectId to);
void mixerOptionsMenu(App& a);
void mixerLatencyMenu(App& a, ObjectId track);
void renderMixerStems(App& a, const std::vector<ObjectId>& tracks);
void knobContextMenu(App& a, ParamId pid);
void pluginWindowMenu(App& a, ObjectId owner, int slot);  // slot < 0: channel instrument
// Checkpoint 59 (N58-06): the same menu for a device docked in Clips' device band, in Clips' words
// (track and device, no channel, mixer track, piano roll, editor size or window rows).
void pluginWindowMenu(App& a, ObjectId owner, int slot, bool clipsDevice);
// 0.7.8: the same menu in the Audio clip window's header (a sampler channel): no channel header,
// editor size or Close window rows (the window has its own).
void pluginWindowMenu(App& a, ObjectId owner, int slot, bool clipsDevice, bool clipWindow);
// The channel settings' note rows (ChannelSettings.cpp): `part` -1 all of them as the channel
// settings draw them; else one, without its heading, for a card of the Audio clip window's Channel
// tab (0 tune and time, 1 polyphony, 2 cut groups, 3 expression, 4 echo, 5 layer, group and MIDI).
void drawNoteSettingsPart(App& a, ObjectId id, int part);
// The same part laid on the Audio clip window's module grid: `place(kind, index, width)` gives the
// top-left (screen) of the part's index-th knob (kind 0: its name's row; the knob 16 px under it, its
// value 60 under it), toggle (1), list (2, its name in its preview) or note text (3), and sets the
// width to draw it at. Each control's edits are the same as above.
struct NoteGrid {
  std::function<ImVec2(int kind, int index, float& width)> place;
  float knobRadius = 20.0f;
  float knobOff = 16.0f, valueOff = 60.0f;  // the knob's and the value's top, from the name's
};
void drawNoteSettingsPart(App& a, ObjectId id, int part, const NoteGrid* grid);
// K-112c: the device title bar's save and hot-swap buttons. savePresetDialog: Save preset to file
// for that plugin; swapDeviceItems: menu rows of the instruments (slot < 0) or effects to put in its place.
void savePresetDialog(App& a, ObjectId owner, int slot);
void swapDeviceItems(App& a, ObjectId owner, int slot);
// Toolbar button (menu icon) that opens `body` as a popup.
void menuButton(const char* id, const char* hint, const std::function<void()>& body);
// Development: draws the named menu open (for --menu screenshots).
void drawDevMenu(App& a, const std::string& name);

}  // namespace gl

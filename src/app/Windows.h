// Gridline — window entry points (each draws one dockable window or bar).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../core/Params.h"
#include "ClipHeaderHit.h"
#include "MidiUi.h"

namespace gl {

class App;

float drawTopBar(App& a);      // menu + transport; returns height used
// Dev checks (J-91): where the top bar drew a button last frame ("play", "stop", "rec", "metro"), in
// logical pixels; false when it is not shown. `mirror`: the metronome glyph's last horizontal scale.
bool topBarButtonRect(const char* name, float& x0, float& y0, float& x1, float& y1);
float topBarMetronomeMirror();
void drawStatusBar(App& a);    // hint bar + notifications
void drawNotificationCards(App& a);  // J-141: the notification cards alone (the launcher has no status bar)
void drawBrowser(App& a);
void drawChannelRack(App& a);
// J-84 dev checks: --rack <spec> (comma separated: steps<N> pattern length,
// w<W> / h<H> floating rack size, dock = docked beside the browser, scroll<px>
// step area scroll) and the --dev-input "rack" dump (layout, scroll, values).
void rackDevSpec(const std::string& spec);
void rackDevBefore(App& a, int frame);  // right before drawChannelRack
void rackDevAfter(App& a, int frame);   // right after it
void rackDevDump(App& a, int frame);
void drawPianoRoll(App& a);
// Q-4 dev check (--dev-input "lane"): the piano roll's event lane as drawn last
// frame, in logical pixels: x of tick t = x0 + t * pxPerTick; value v (0..1) at
// y = yBottom - v * (yBottom - yTop).
struct PianoLaneGeometry {
  float x0 = 0, pxPerTick = 0, yTop = 0, yBottom = 0;
  bool valid = false;
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
  bool valid = false;
};
const PlaylistGeometry& playlistGeometry();
void drawMixer(App& a);
// J-83 dev check (--dev-input "mixer"): the strips pane's vertical scroll as
// drawn last frame, and whether the plain wheel scrolls the mixer there.
float mixerStripsScrollY();
bool mixerWheelScrolls();
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
void drawPluginManager(App& a);  // Options > Plugin manager (PluginManager.cpp)
// Arrangements and live mode (Live.cpp).
void drawLivePads(App& a);                         // "Live pads" window
void drawMasterPanel(App& a);                      // RN-09 "Master chain" window
void arrangementMenu(App& a);                      // menu body: list, new, duplicate, rename, delete
void arrangementSelector(App& a, float width);     // toolbar combo
void liveTrackMenu(App& a, int track);             // menu body: a track's live settings
void setLiveArea(App& a, Tick end);                // 0 removes it      // file dialog, settings, info, export, history, about, prompts
void handleGlobalShortcuts(App& a);
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
void addChannelMenu(App& a);       // instruments by category
void patternContextMenu(App& a, ObjectId pattern);
// J-82 (Playlist.cpp): playlist picker source menus and deleting sources.
// Used sources ask first (App::confirm); unused ones are deleted at once.
void audioSourceMenu(App& a, ObjectId channel);
void automationSourceMenu(App& a, ObjectId automation);
void patternSourceMenu(App& a, ObjectId pattern);
void requestDeleteSources(App& a, ClipKind kind, std::vector<ObjectId> ids);
void playlistTrackMenu(App& a, int track);
void clipContextMenu(App& a, ObjectId clip);
void playlistOptionsMenu(App& a);
void pianoRollMenu(App& a);
void markerMenu(App& a, int index);  // playlist ruler: one marker (J-11)
void markerAddItems(App& a);         // playlist menu > Time markers (J-11)
// J-85: a song loop marker at `pos` snapped to the playlist grid (one undo
// step; playback reaching the song end goes on from it).
void addLoopMarker(App& a, Tick pos);
void rulerMenu(App& a, Tick pos);    // right-click on the playlist ruler (J-85)
void mixerTrackMenu(App& a, size_t index);
void mixerSlotMenu(App& a, ObjectId track, int slot);
// Mixer (MixerView.cpp, J-12): selected tracks in mixer order (the focused
// one when nothing else is selected), the send menu of a route arrow, the
// mixer options menu, the track latency menu, per-track stem renders.
std::vector<ObjectId> mixerSelection(App& a);
void mixerSendMenu(App& a, ObjectId from, ObjectId to);
void mixerOptionsMenu(App& a);
void mixerLatencyMenu(App& a, ObjectId track);
void renderMixerStems(App& a, const std::vector<ObjectId>& tracks);
void knobContextMenu(App& a, ParamId pid);
void pluginWindowMenu(App& a, ObjectId owner, int slot);  // slot < 0: channel instrument
// Toolbar button (menu icon) that opens `body` as a popup.
void menuButton(const char* id, const char* hint, const std::function<void()>& body);
// Development: draws the named menu open (for --menu screenshots).
void drawDevMenu(App& a, const std::string& name);

}  // namespace gl

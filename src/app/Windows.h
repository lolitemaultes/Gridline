// Gridline — window entry points (each draws one dockable window or bar).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../core/Params.h"
#include "MidiUi.h"

namespace gl {

class App;

float drawTopBar(App& a);      // menu + transport; returns height used
void drawStatusBar(App& a);    // hint bar + notifications
void drawBrowser(App& a);
void drawChannelRack(App& a);
void drawPianoRoll(App& a);
void drawPlaylist(App& a);
void drawMixer(App& a);
void drawChannelSettings(App& a);
void drawEffectEditor(App& a);
void drawDialogs(App& a);
// CPU profiling (Performance.cpp): samples the engine counters into App::perf
// (per frame) and draws the "Performance" window.
void updatePerformance(App& a);
void drawPerformance(App& a);
void drawPluginManager(App& a);  // Options > Plugin manager (PluginManager.cpp)
// Arrangements and live mode (Live.cpp).
void drawLivePads(App& a);                         // "Live pads" window
void arrangementMenu(App& a);                      // menu body: list, new, duplicate, rename, delete
void arrangementSelector(App& a, float width);     // toolbar combo
void liveTrackMenu(App& a, int track);             // menu body: a track's live settings
void setLiveArea(App& a, Tick end);                // 0 removes it      // file dialog, settings, info, export, history, about, prompts
void handleGlobalShortcuts(App& a);
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
// Extra rows for the keyboard shortcuts window.
void menuShortcutTable(App& a);

// Context menu bodies (call between BeginPopup/EndPopup) — ContextMenus.cpp.
void channelContextMenu(App& a, ObjectId channel);
void rackOptionsMenu(App& a);      // channel rack menu: selection ops, sorting, colours
void addChannelMenu(App& a);       // instruments by category
void patternContextMenu(App& a, ObjectId pattern);
void playlistTrackMenu(App& a, int track);
void clipContextMenu(App& a, ObjectId clip);
void playlistOptionsMenu(App& a);
void pianoRollMenu(App& a);
void markerMenu(App& a, int index);  // playlist ruler: one marker (J-11)
void markerAddItems(App& a);         // playlist menu > Time markers (J-11)
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

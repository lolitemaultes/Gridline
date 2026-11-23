// Gridline: the "Audio clip" window's plan (no drawing): its tabs (each a few modules side by side
// under the waveform; the Clips band and Smooth's inspector and audio editor lay the same modules out
// in their own places) and what each changes,
// the tab each way in opens, which tabs a sampler channel without a clip shows, how cards wrap, and
// the checklist of every control of the two windows it replaced (the audio clip editor and the
// sampler channel's settings) with the place each one lives now. The window itself is
// app/ClipEditor.cpp; tests read this plan to check nothing was left behind.
#pragma once
#include <set>
#include <string>
#include <vector>

namespace gl::clipwin {

// The tabs, left to right: this clip, then its sample (every clip of it and the notes it plays),
// then its channel, then tools on the audio (Edit audio and Analysis: the tools row in the window,
// the Clips band's sample display and Smooth's audio editor). TabTime is the Warp tab.
enum Tab : int {
  TabClip = 0,
  TabTime,
  TabSample,
  TabPlayback,
  TabEnvelope,
  TabFilter,
  TabSampleFx,
  TabChannel,
  TabNoteFx,
  TabMidi,
  TabEdit,
  TabAnalysis,
  TabCount
};
enum class Scope { Clip, Sample, Channel, Tools };

Scope scopeOf(int tab);
// English tab name (translate with i18n::tr).
const char* tabName(int tab);
// The tab groups as drawn: {first tab, count}.
struct Group {
  int first, count;
};
const std::vector<Group>& groups();
// A tab of modules (not Edit audio or Analysis, which are tools).
bool hasModules(int tab);

// A sampler channel no clip plays opens the window without the clip's tabs (Clip, Warp).
bool tabShown(int tab, bool hasClip);
// The tab to show: `tab` when shown, else Playback.
int shownTab(int tab, bool hasClip);

// A channel of this plugin type has its settings in this window (no plugin window of its own).
bool ownsType(const std::string& type);

// The ways in, and the tab each opens on.
enum class Entry {
  DoubleClick,      // a double-click on an audio clip
  EditClip,         // the clip menu's Edit clip...
  ChannelSettings,  // Channel settings, the channel rack's button, any "open the channel's window"
  DoubleClickSetting,  // a double-click while Settings > Interface says "Playback tab"
};
int entryTab(Entry e);

// Cards in rows: as many as fit at their least width (gap between), each row then stretched to
// the width. Returns per card its row; widths gets each card's drawn width. `grow` (optional, one
// per card): each card's share of a row's spare width (a short list of toggles takes none, a
// keyboard or a graph takes most); without it, or a row whose cards all take none, they share it
// equally.
struct FlowRow {
  size_t first = 0, count = 0;
};
std::vector<FlowRow> flowRows(const std::vector<float>& minW, float avail, float gap, std::vector<float>* widths = nullptr,
                              const std::vector<float>* grow = nullptr);

// ---- the checklist of the old controls

enum Was : int { WasClipEditor = 1, WasChannelSettings = 2, WasBoth = 3 };
// Where a control is needed: always, only with a clip, only for a clip of the Clips layout's clip grid.
// NeedFullView: with a clip (the tools row: the window's, the Clips band's sample display, Smooth's
// audio editor).
enum Need : int { NeedAlways = 0, NeedClip, NeedClipsLaunch = 3, NeedFullView };
struct Item {
  const char* key;    // the mark the window sets where it draws the control
  const char* what;   // the old control (English)
  int was;            // Was
  const char* home;   // where it lives now (English)
  int tab;            // the tab it is on (-1: header, tools, waveform, overview or the header menu)
  int need;           // Need
};
const std::vector<Item>& inventory();
// The inventory keys not in `reached`, for a window (or a layout's own places) with or without a clip,
// on a clip of the Clips layout's clip grid or not.
std::vector<std::string> missing(const std::set<std::string>& reached, bool hasClip, bool clipsLaunch);

// 0.7.8: the window's Main view shows every control of the sampler channel's old window (its header,
// the channel strip, the sample row and the sampler's four pages: Sample, Envelope / filter, Misc,
// Sample FX) on one page: their inventory keys (the clip's own card's too, with a clip). The rest
// (the clip's waveform editor and tools, Warp, the channel's note settings, Edit audio, Analysis)
// is the Advanced view's.
const std::vector<std::string>& mainKeys(bool hasClip);
std::vector<std::string> missingMain(const std::set<std::string>& reached, bool hasClip);

}  // namespace gl::clipwin

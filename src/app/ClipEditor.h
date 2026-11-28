// Gridline: the "Audio clip" window: one window for an audio clip and its sampler channel (it
// replaced the audio clip editor and the sampler channel's settings window).
//
//   header      the clip's (or channel's) colour and name, its warp badge, the file, how many
//               clips share the sample, the selection count; the menu (presets, the channel's
//               commands, Audio tools), the mixer track box and a free one (as a plugin
//               window's header has them)
//   tool strip  pointer, warp, slip, fade, slice, pencil (volume line), scrub,
//               zoom; Wave / Spectrum; Bars / Time; snap; preview, loop, follow
//   waveform    the clip as it plays (warped, at its gain) inside the rest of
//               its sample (dimmed): trim edges, fades with their curves, the
//               volume line, warp markers, the time selection
//   overview    the whole sample: its blue start and end handles, the notes' loop and the
//               view's window
//   tabs        each a few modules side by side, Output pinned at the right: this clip: Clip,
//               Warp; the sample (every clip of it and its notes, orange): Sample, Playback,
//               Envelope, Filter, Sample FX; the channel: Channel, Note effects, MIDI and
//               macros (core/ClipWindow.h). Edit audio and Analysis are in the tool strip.
//
// A sampler channel no clip plays opens the same window without the clip's tabs, its whole
// sample in the waveform's place.
//
// 0.7.8: the window (Host::Window) has two views, Main | Advanced in its header (the app setting
// "ui.clipWindowView"): Main is the header, the whole sample and the sampler's own page's cards on
// one page (core/ClipWindow.h mainKeys); Advanced is the tool strip, waveform and tabs above.
//
// Hosts: Studio a window of its own (the tabs); Clips the clip band (the clip selected) and the
// device band (a sampler channel's device), both as the band's own view: the head card, the sample
// display and a card per module side by side in the band, scrolling sideways inside it; Smooth its
// own places: the inspector's Region card (the clip's settings as rows) and the lower pane's audio
// editor (a local inspector with the sample's and the channel's sections, the tools and the
// waveform); Tracker the Audio page (the tabs). Nothing opens over the views above a band or pane.
// A double-click on an audio clip, Edit clip..., and Channel settings of a sampler channel open
// it, on the tab each asks for.
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {
class App;

namespace clipeditor {

// (Device: the Clips layout's device band, a sampler channel's device there)
enum class Host { Window, Clips, Pane, Device };

// Opens the window on `clip` in the current layout's place for it, on `tab` (core/ClipWindow.h
// clipwin::Tab; -1: the tab it was on).
void open(App& a, ObjectId clip, int tab = -1);
// A sampler channel's settings: the window on its sample (with the clip shown or selected, else
// its first clip, else the channel alone), on `tab`. False for a channel that is not a sampler.
bool openChannel(App& a, ObjectId channel, int tab);
// The window shows this channel's settings (a sampler).
bool takesChannel(const App& a, ObjectId channel);
// The sampler channel's file dialog (the window's Replace sample...): the file and its name, in one step.
void sampleDialog(App& a, ObjectId channel);
// The window (or the pane) is open on this channel, or on a clip of it.
bool showsChannel(const App& a, ObjectId channel);
// Closes the window (or the pane) when it shows this channel or a clip of it.
void closeChannel(const App& a, ObjectId channel);
// A double-click on an audio clip: the window (on its Playback tab when the setting
// "ui.clipDoubleClick" says "channel"), or Loupe ("loupe").
void openFromDoubleClick(App& a, ObjectId clip);
// The Studio window (and the window of any layout whose place is not shown).
void drawWindow(App& a);
// The window's body in `size` at the cursor, for `host`: a clip's, or (clip kNoId) a channel's.
void drawBody(App& a, ObjectId clip, ImVec2 size, Host host);
void drawTargetBody(App& a, ObjectId clip, ObjectId channel, ImVec2 size, Host host);
// The Clips device band: a sampler channel's settings as the window's band form in `size` at the
// cursor (the tab bar when tall enough, else the chip row), with the channel's clip chosen in the
// playlist (else the one it showed, else its first; none: the channel alone).
void drawDeviceBand(App& a, ObjectId channel, ImVec2 size);
// The tab the device band shows next (Channel settings opened on the device).
void deviceBandTab(int tab);
// The Smooth lower pane / Tracker Audio page: draws the window when the pane shows a clip or a
// channel (true), else false (the pane shows Loupe).
bool drawPane(App& a, ImVec2 avail);
// Smooth's inspector: an audio region's own settings (gain and pitch, fades, warp and the stretch
// engine) as rows of its Region card, at the cursor, the card's width. Its sample's and channel's
// settings are the audio editor's local inspector (the lower pane).
void drawInspectorRegion(App& a, ObjectId clip);
// The inspector drew a region's rows this frame or the last.
bool inspectorShown();
// The pane shows Loupe again (Open in Loupe, Loupe opened another way).
void paneShowsLoupe();
ObjectId paneClip();
ObjectId paneChannel();
// A region picked while the pane shows the window: it follows it (true), else false.
bool followInPane(ObjectId clip);
// The window drew in `host` this frame or the last.
bool shown(Host host);
// The window's time selection on `clip` (song ticks), when one drawn lately shows the clip
// with one. False without.
bool selectionOf(ObjectId clip, double& t0, double& t1);
// The clip and the channel the window shows (kNoId: closed, or a channel alone).
ObjectId windowClip();
ObjectId windowChannel();
void closeWindow();
// The words of the current layout ("clip", "region", "sample clip").
const char* clipWord();
// ---- 0.7.8: the Tracker layout's Sample editor page (TrackerEditors.cpp) holds this window: every
// way in opens that page on the sampler (the pane's target and state: its tab, view, tools and
// selection), and the page draws the window's parts in its own frame, in the Tracker's look (its
// control style and bevelled boxes), between begin() and end().
namespace trackerpage {
// The page opened on a sampler channel (the band shows it for the channel).
void show(App& a, ObjectId channel);
// The band shows the page now.
bool showing(App& a);
// The Tracker's Audio page asked for on a selected Sampler, or while a way in had put a clip or a
// sampler in the window: the page opens on it instead (true); false: nothing there (the Audio page is Loupe's).
bool reopen(App& a);
// The page draws this sampler channel: the target a way in picked when it is of the channel, else the
// channel with its clip as Channel settings pick it. False: not a sampler channel.
bool take(App& a, ObjectId channel);
struct Info {
  bool ok = false;
  ObjectId clip = kNoId, channel = kNoId;
  std::string name, channelName, file, meta;  // the clip's (else the channel's) name, the file, its rate, channels and length
  uint32_t colour = 0;
  int shared = 0, moreSelected = 0, warpMarkers = 0;
};
Info info(App& a);
bool begin(App& a);  // false: nothing to draw (then no end())
void end(App& a);
void colour(App& a, ImVec2 at, float size);  // the channel's colour swatch
void menuItems(App& a);                       // the header menu's items, inside a popup the caller opened
bool takeMenuAsk();                           // (dev "clipeditor menu": open it)
std::string presetStep(App& a, int d);        // the sampler's previous / next preset (its name; empty: none)
// The channel's mixer track box `w` wide at `at`, then the free track's button `h` square after it
// (as the window's header has them); returns the width taken.
float route(App& a, ImVec2 at, float w, float h);
void tools(App& a, ImVec2 p0, float w, float h);   // the tools row (tools, views, snap, preview, analysis, Edit audio...)
void wave(App& a, ImVec2 p0, ImVec2 p1, float ovH);  // the clip's waveform with its SAMPLE strip (no clip: the whole sample)
void overview(App& a, ImVec2 p0, ImVec2 p1);  // the SAMPLE strip alone (the whole file, its handles and loop)
struct Module {
  const char* id;
  const char* title;  // English
  int tab;            // core/ClipWindow.h
  float w;
};
std::vector<Module> modules(App& a);  // the window's modules, Output last
float moduleHeight(bool compact);
// The shown tab's modules and Output as the window lays them in a panel `w` wide: their height
// (-1: they do not fit full), and drawn at p0, `hAvail` tall (scrolling up and down when taller;
// compact when they do not fit full); returns where each module went.
float tabHeight(App& a, float w, bool compact);
struct Laid {
  std::string id;
  ImVec2 a, b;
};
std::vector<Laid> tabPanel(App& a, ImVec2 p0, float w, float hAvail, bool compact);
void preview(App& a);  // the clip alone, as the tools row's play button (again: stops)
// Edit audio... (the audio operations, then send and split) in a popup at `at`, opened when `open`.
void editAudioMenu(App& a, bool open, ImVec2 at);
int tab();
void setTab(int tab);
bool reachAll();  // (dev) the checklist is on: every module is drawn
// K-391: the page's clip (kNoId: the channel alone) and its time selection (song ticks; empty when
// equal), so the Tracker's two Sample editor views keep one selection when switched.
ObjectId selection(double* t0, double* t1);
void setSelection(double t0, double t1);
}  // namespace trackerpage

// Dev: "clipeditor state|geom|tab <n>|tool <n>|spectrum <0/1>|select <t0> <t1>|op <name> [value]|
// open <clip> [tab]|channel <id> [tab]|dbl <clip>|measure|close|reach on|off|clear|checklist|menu|
// poke <key>|panel <px>|host clips|device|pane|window|auto".
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args);

}  // namespace clipeditor
}  // namespace gl

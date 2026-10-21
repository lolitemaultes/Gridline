// Gridline — J-75: files dragged into Gridline from other applications
// (Finder / Explorer / file managers, sample stores such as Tracklib, browsers,
// mail, photo apps).
//
// The window system layer (host/ui/ExternalDropMac.mm on macOS; SDL's drop
// events in app/main.cpp elsewhere) turns a drag session into Events posted to
// the queue below: Enter (what is dragged, where), Move, Leave, Drop, and for
// files that only exist after the drop — file promises, downloads of web
// links, raw audio data — Arrived / Failed / Progress. The interface
// (app/ExternalDragUi) consumes them once a frame, shows where the drop would
// land, and performs it.
//
// Pure logic, no UI: item kinds, the routing of a set of files onto a drop
// target (what happens, where, and the hint text), uniform type identifier →
// extension, web links, quick audio header reads. Unit tested
// (tests/test_external_drop.cpp).
//
// Platform layers:
//   macOS  — NSDraggingDestination on the main window: file URLs, file
//            promises (NSFilePromiseReceiver), http(s) links to audio, raw
//            audio data; hover position + modifier keys. Tested.
//   X11 / Wayland / Windows — SDL2 reports files only when they are dropped
//            (SDL_DROPBEGIN / SDL_DROPFILE / SDL_DROPTEXT / SDL_DROPCOMPLETE,
//            no hover position): main.cpp turns them into Enter + Drop at the
//            pointer. A native XDND / OLE IDropTarget layer plugs in by posting
//            the same events (see native::install). Untested on those systems.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "BrowserIndex.h"
#include "Types.h"

namespace gl::xdrop {

// One dragged thing.
struct Item {
  std::string path;  // the file on disk ("" while pending)
  std::string name;  // file name with extension (shown, decides the kind)
  std::string url;   // http(s) link it is downloaded from (on drop only)
  bool pending = false;  // file promise / download / raw data: not on disk yet
  bool dir = false;      // a folder
  bool guessed = false;  // pending item of unknown type: assumed to be audio
};

// Kind of an item (a folder → Kind::Folder; pending items by their name; a
// pending item of unknown type counts as audio, the common case for sample
// stores that promise files without a type).
browse::Kind kindOf(const Item& it);
// Display name ("Kick 01.wav").
std::string displayName(const Item& it);

struct Mods {
  bool shift = false;  // playlist: one after another on the track under the pointer; rack: always new channels
  bool alt = false;    // no snapping (the playlist inverts its snap, like every drag)
  bool ctrl = false;
};

// ---------------------------------------------------------------- events
enum class EventType { Enter, Move, Leave, Drop, Arrived, Failed, Progress };
struct Event {
  EventType type = EventType::Move;
  float x = 0, y = 0;  // window coordinates (points / SDL window units), top-left origin
  Mods mods;
  bool hasMods = false;     // the layer knows the modifier keys (macOS)
  std::vector<Item> items;  // Enter / Drop
  uint64_t token = 0;       // Drop: identifies pending items; Arrived / Failed / Progress: which drop
  size_t index = 0;         // Arrived / Failed / Progress: item index
  std::string path;         // Arrived: the file
  std::string error;        // Failed
  double progress = 0;      // Progress: 0..1, < 0 unknown
};
// Thread safe (promise / download completions come from background threads).
void post(const Event& e);
std::vector<Event> take();
uint64_t newToken();

// Where pending items are written: "<project folder>/Dropped", or the config
// folder's "Dropped" while the project is unsaved. Set by the interface.
void setDropFolder(const std::string& dir);
std::string dropFolder();
// A free path for `name` in `dir` ("Kick.wav", "Kick (2).wav", …); makes the folder.
std::string uniquePath(const std::string& dir, const std::string& name);
// Whether the last drawn frame found a target that takes the drag (the
// platform layer shows the copy cursor then).
void setHoverAccepted(bool on);
bool hoverAccepted();
// Set by the interface when a drag was cancelled (Escape): the platform
// layer refuses the drop until the next drag enters.
void setCancelled(bool on);
bool cancelled();

// ------------------------------------------------------------- formats
// File name extension (".wav") for a uniform type identifier, "" if unknown.
std::string extensionForUti(const std::string& uti);
// Audio uniform type identifiers (raw audio data on a pasteboard).
const std::vector<std::string>& audioUtis();
bool isWebUrl(const std::string& s);
// "file:///a%20b/c.wav" → "/a b/c.wav"; "" when not a file URL.
std::string pathFromFileUrl(const std::string& url);
// File name for a web link ("https://x.com/a/Kick%2001.wav?dl=1" → "Kick 01.wav").
std::string fileNameForUrl(const std::string& url);
// Length of an audio file from its header (WAV / FLAC), < 0 when unknown.
double audioHeaderSeconds(const std::string& path);

// ------------------------------------------------------------- planning
// Where the drag is: the drop target under the pointer and what is there.
struct Context {
  browse::DropTarget target = browse::DropTarget::Workspace;
  std::string targetType;         // plugin type of the target (channel row / plugin window / mixer slot)
  bool targetUsesSample = false;  // that plugin loads samples
  bool onChannel = false;         // Rack: over a channel row (replace its sample)
  std::string targetName;         // channel / plugin / mixer track name (hint text)
  int track = 0;                  // Playlist: track under the pointer (0-based)
  Tick tick = 0;                  // Playlist: snapped position
  Tick barTicks = 384;            // one bar
  int trackCount = 500;           // playlist tracks
  std::vector<Tick> lengths;      // per item: length in ticks (0 = unknown)
  Tick fallbackLength = 384;      // drawn length of clips of unknown length
};
// What dropping does with one item.
struct Step {
  size_t item = 0;
  browse::DropAction action = browse::DropAction::None;
  int track = 0;   // AudioClip / MidiPatternClip
  Tick tick = 0;
  Tick length = 0;
};
struct Plan {
  std::vector<Step> steps;
  std::vector<size_t> unsupported;  // items no target takes (not audio / MIDI / preset / project / folder)
  std::vector<size_t> skipped;      // usable items this target ignores
  std::string label;                // "Drop to add 3 audio clips on track 4 at bar 9"
  bool accepted() const { return !steps.empty(); }
};
// `kinds`: per item, what it is (presets decoded by the caller; see
// browserui::dropItemFor). FL behaviour: several samples dropped on the
// playlist go on consecutive tracks from the one under the pointer (stacked);
// Shift lays them one after another on that track. A channel row of a
// sample-based channel takes one sample (replace); Shift makes new channels.
// A project anywhere opens (the rest of the drop is ignored).
Plan plan(const std::vector<Item>& items, const std::vector<browse::DropItem>& kinds, const Context& ctx, const Mods& mods);

// Positions of several clips dropped at (track, tick): stacked on
// consecutive tracks (clamped to the last track) or one after another.
struct Place {
  int track = 0;
  Tick tick = 0;
};
std::vector<Place> layoutClips(const std::vector<Tick>& lengths, int track, Tick tick, bool sequential, int trackCount);

// ------------------------------------------------------------- platform
namespace native {
// Makes the main window (NSWindow*) a drop target for everything above
// (macOS). False where Gridline relies on SDL's drop events.
bool install(void* nativeWindow);
// Main loop, once a frame: download progress (macOS).
void poll();
// Starts downloading `url` into the drop folder; posts Progress, then
// Arrived / Failed with (token, index). Only ever called for an explicit drop.
void download(uint64_t token, size_t index, const std::string& url);
#if defined(__APPLE__)
// What an NSPasteboard* holds, as drag items, in order of preference: file
// URLs (Finder, Electron apps such as Tracklib / Splice), else file promises
// (browsers, Mail, Photos, sample stores), else http(s) links to audio /
// MIDI, else raw audio data. `resolve` (the drop): promises start arriving in
// the drop folder, links start downloading, raw data is written — Arrived /
// Failed events carry `token`. Tests call it with their own pasteboard.
std::vector<Item> readPasteboard(void* pasteboard, bool resolve, uint64_t token);
// The pasteboard's types per item ("public.file-url, …"; GRIDLINE_DND_LOG=1 prints them on drag enter).
std::string describePasteboard(void* pasteboard);
#endif
}  // namespace native

}  // namespace gl::xdrop

// Gridline — browser content in the app (J-16): the shared content index,
// drag & drop of browser files onto windows, double-click actions, sample
// preview with tempo sync, the plugin picker overlay and plugin thumbnails.
//
// Drag payloads: audio files travel as "GL_FILE" (path; every older sample
// target accepts it), other browser files (MIDI, presets, chains, projects,
// FL presets) as "GL_ITEM" (path). Plugins keep "GL_PLUGIN" (type).
#pragma once
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/BrowserIndex.h"
#include "../core/ExternalDrop.h"
#include "../core/Types.h"

namespace gl {

class App;
struct PluginTarget;

namespace browserui {

// The playlist picker's drag of a clip source ("GL_CLIPSRC": ClipKind, id).
// J-110: an audio clip source (a recorded take, a render, an edited or
// Seltmann loop in the picker) dropped on a plugin window or a channel row
// loads its file there like a browser audio file.
struct ClipSrcPayload {
  int kind;
  ObjectId id;
};

// ---- content index ----
browse::Index& index();
// Loads the cache and starts indexing once (no-op in thumbnail mode).
void ensureIndex(App& a);
// Walks the roots again (new / changed / removed files).
void refreshIndex(App& a);
// Per frame: re-walks the roots every few minutes while idle (file watching).
void tickIndex(App& a);
// Stops the indexing worker (main, before teardown). J-46: on a first run the
// worker is still analysing the shipped packs when a short run quits; left to
// the index's static destructor it kept running while the function-local
// statics it uses (tag vocabulary, sample cache …), built after the index,
// were already destroyed.
void shutdownIndex();
// FL Studio's shipped "Data" folder (read in place), or "".
std::string flDataDir();
// Gridline's own content folders under the config dir ("Recorded",
// "Rendered", "Sliced audio", "Clipboard files"); created on demand.
std::string contentDir(App& a, const char* name);

// ---- drag & drop ----
struct DropPlace {
  ObjectId channel = kNoId;   // Rack: channel row under the mouse (optional): its content is replaced (J-97)
  int insertAt = -1;          // Rack: between rows: new channels are inserted at this channel index (J-97)
  ObjectId mixer = kNoId;     // MixerTrack / MixerSlot
  int slot = -1;              // MixerSlot
  int track = 0;              // Playlist lane
  Tick tick = 0;              // Playlist position
  ObjectId owner = kNoId;     // PluginWindow: channel (pluginSlot < 0) or mixer track
  int pluginSlot = -1;
};
// Call right after a browser item: makes it draggable.
void fileDragSource(const std::string& path, const char* label = nullptr);
// What dropping `path` on `target` would do (FL presets are decoded, cached).
browse::DropAction dropActionFor(App& a, const std::string& path, browse::DropTarget target, const DropPlace& place);
// Between BeginDragDropTarget / EndDragDropTarget: accepts browser files for
// `target` (only those with an action there; shows the action while hovering).
bool acceptFileDrop(App& a, browse::DropTarget target, const DropPlace& place);
// J-97: the same with feedback for targets that draw their own highlight
// (the channel rack's rows): `hovering` is set while the drag hovers and would
// be taken; with `highlight` false the default target outline is left out.
struct DropFeedback {
  bool highlight = true;
  bool hovering = false;
};
bool acceptFileDrop(App& a, browse::DropTarget target, const DropPlace& place, DropFeedback* fb);
// J-97: plugins dragged from the browser / plugin picker ("GL_PLUGIN"):
//   Rack: a channel row -> its instrument is replaced (routing, volume, pan,
//         steps and notes stay; FL "Replace"); between rows -> a new channel
//         there; the free area -> a new channel (as before);
//   Playlist -> a new channel; MixerTrack -> the first free effect slot;
//   MixerSlot -> that slot; PluginWindow -> replaces the shown instrument /
//   effect of the same kind. Instruments only go where instruments live and
//   effects where effects live; elsewhere the drag is refused with a reason.
// Between BeginDragDropTarget / EndDragDropTarget. One undo step.
bool acceptPluginDrop(App& a, browse::DropTarget target, const DropPlace& place, DropFeedback* fb = nullptr);
// What dropping plugin `type` there does ("" = refused, `why` says why).
std::string pluginDropLabel(App& a, const std::string& type, browse::DropTarget target, const DropPlace& place, std::string* why = nullptr);
bool performPluginDrop(App& a, const std::string& type, browse::DropTarget target, const DropPlace& place);
// J-97: loads an audio file into a channel's sample (one undo step). The
// channel keeps its routing, volume, pan, steps and notes; it is renamed after
// the file only when its name was automatic (the instrument's name or the old
// sample's). Other instruments become a sampler holding the file.
// `keepSampleInstrument`: every instrument that holds a sample keeps it (plugin
// windows: Tercet's custom wave too); otherwise only the sample players do.
bool loadChannelSample(App& a, ObjectId channel, const std::string& path, const char* label = "replace sample", bool keepSampleInstrument = false);
// Plugin window drop zone (whole window): presets of the same plugin, samples
// for sample-based plugins. Call from the window header.
void pluginWindowDropTarget(App& a, const PluginTarget& t);
// Runs the action. Returns false (with a notification) when nothing happened.
bool performDrop(App& a, const std::string& path, browse::DropTarget target, const DropPlace& place);
// Double-click in the browser / file dropped onto the app window.
void openFile(App& a, const std::string& path);

// ---- several files at once (J-75: drags from other applications) ----
// Payload "GL_FILES": the items of the external drag (ExternalDragUi.h);
// one file that is already on disk travels as "GL_FILE" / "GL_ITEM" like a
// browser file, so every sample slot takes it too.
// What an item is (presets decoded, folders, pending items by name).
browse::DropItem dropItemFor(const xdrop::Item& it);
// Modifier keys as ImGui sees them (external drags feed theirs in).
xdrop::Mods currentMods();
// What dropping `items` on `target` at `place` does, with the hint text.
xdrop::Plan planFor(App& a, const std::vector<xdrop::Item>& items, browse::DropTarget target, const DropPlace& place, const xdrop::Mods& mods);
// Runs it as one undo step (clips stacked / one after another, unsupported
// files notified). Pending items: the drop waits until their files are here.
bool performItems(App& a, const std::vector<xdrop::Item>& items, browse::DropTarget target, const DropPlace& place, const xdrop::Mods& mods);
// Files dropped where no window takes them (each opened like a double click).
void openFiles(App& a, const std::vector<xdrop::Item>& items);
// The drop hovering `target` this frame (accepted), for drop previews such as
// the playlist's ghost clips; nullptr otherwise.
struct HoverPlan {
  browse::DropTarget target = browse::DropTarget::Workspace;
  xdrop::Plan plan;
  std::vector<xdrop::Item> items;
};
const HoverPlan* hoverPlan(browse::DropTarget target);
// Adds a folder to the browser's user folders (and indexes it).
void addBrowserFolder(App& a, const std::string& dir);
// Browser window drop zone (folders); call inside the browser window.
void browserDropTarget(App& a);

// ---- preview ----
// Plays a sample; with tempo sync on and a known tempo it is resampled to the song tempo.
void previewFile(App& a, const std::string& path);
bool& previewSync();

// ---- plugin picker (F8) ----
void drawPluginPicker(App& a);
void togglePluginPicker();
// Shows the browser on its current-project tab (View > Project picker).
void showProjectTab(App& a);
bool pluginPickerOpen();

// ---- in-house glyphs ----
void drawStar(ImDrawList* dl, ImVec2 center, float radius, ImU32 col, bool filled);
// Category glyph for a plugin (synth wave, drum pads, delay taps, filter
// curve, …), drawn as vectors in a size x size box centred on `center`.
void drawCategoryGlyph(ImDrawList* dl, const std::string& category, int pluginKind, ImVec2 center, float size, ImU32 col);
// Category glyph colour role (tile / icon tint).
uint32_t categoryRole(const std::string& category, int pluginKind);

// ---- thumbnails (<config>/thumbs/<type>.png) ----
// Texture of a built-in plugin's editor thumbnail (loaded lazily); 0 when none.
ImTextureID pluginThumb(App& a, const std::string& type, ImVec2* size);
std::string thumbPath(App& a, const std::string& type);
// Starts a child process that renders every editor once (--make-thumbs).
void startThumbnailRun(App& a);
bool thumbnailRunActive();
// --make-thumbs mode (main.cpp): draw the editor window, then capture it.
void beginThumbMode(App& a, const std::string& onlyType = "");
bool thumbMode();
// Q-7 --layout-audit <dir>: every built-in editor at widths x1 / x1.5 / x2;
// controls closer than inset::kMinInset to their panel's edge go to
// <dir>/layout_audit.tsv, editors (width x1, issues outlined) to <dir>/sheet_NN.png.
void beginAuditMode(App& a, const std::string& dir);
// J-92: with --layout-audit, also click every item of each editor (pages, tabs) for the binding audit.
void setBindingCrawl(bool on);
// Issues the finished audit found (main's exit status in audit mode).
int auditIssueCount();
void thumbDraw(App& a);
// After rendering: reads the editor from the framebuffer. False when done.
bool thumbCapture(App& a, int fbW, int fbH);

}  // namespace browserui
}  // namespace gl

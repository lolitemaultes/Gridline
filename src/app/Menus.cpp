// Gridline — main menu contents (File, Edit, Add, Patterns, View, Options,
// Tools, Help) as data (MenuDef.h) and the global shortcuts behind them. The bar that hosts the
// menus lives in TopBar.cpp; context menus live in ContextMenus.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include "FreezeUi.h"
#include "CollabUi.h"
#include "DisplayConfirmUi.h"
#include "J59Ui.h"
#include "ScriptUi.h"
#include "McpUi.h"
#include "../core/I18n.h"
#include "J31Ui.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Demo.h"
#include "../core/MenuOps.h"
#include "../core/TrackOrder.h"
#include "../core/MenuOpsExtra.h"
#include "ChannelRoute.h"
#include "App.h"
#include "EditTools.h"
#include "AutomationEditor.h"
#include "ContextMenus.h"
#include "ExportUi.h"
#include "HostedUi.h"
#include "LogicPlugins.h"
#include "FlpExportUi.h"
#include "Theme.h"
#include "UpdateUi.h"
#include "UrlImportUi.h"
#include "Widgets.h"
#include "MeterUi.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "TrackerView.h"
#include "WorkspacesUi.h"
#include "Windows.h"
#include "Keys.h"
#include "ThemeSettings.h"
#include "Viewports.h"
#include "BrowserUi.h"
#include "MenuParityUi.h"
#include "TrackReceiptUi.h"
#include "CommandPaletteUi.h"
#include "GrooveUi.h"
#include "LayoutParts.h"
#include "LogicNewTracks.h"
#include "LogicPlugins.h"
#include "MenuDef.h"
#include "LiveLook.h"
#include "../core/TrackModes.h"

namespace gl {

using theme::pal;
using namespace menus;
namespace fs = std::filesystem;

namespace {

void openSettings(App& a, int tab) {
  a.showSettings = true;
  a.settingsTab = tab;
  a.focusWindow = "Settings";
}

void newPattern(App& a) {
  ObjectId id = a.doc.addPattern(ops::nextPatternName(a.doc.song()));
  if (id) a.selectPattern(id);
}

void findEmptyPattern(App& a, bool first, bool askName) {
  ObjectId id = first ? ops::firstEmptyPattern(a.doc.song()) : ops::nextEmptyOrNewPattern(a.doc, a.curPattern);
  if (!id) id = ops::nextEmptyOrNewPattern(a.doc, a.curPattern);
  if (!id) return;
  a.selectPattern(id);
  if (askName) {
    const Pattern* p = a.doc.song().pattern(id);
    openPrompt("Name the pattern", p ? p->name : "", [&a, id](const std::string& n) { a.doc.renamePattern(id, n); });
  }
}

// Every window closes; in the Tracker also every screen (N-05: Ctrl+Alt+X, the classic main screen).
void closeAll(App& a) {
  a.closeAllWindows();
  if (layoutui::active() == layout::Preset::Tracker) trackerview::mainScreen(a);
}

void deletePattern(App& a) {
  if (a.doc.song().patterns.size() <= 1) return;
  // N-10: in the Tracker the key sits next to the pattern editor's clear keys: say what went and
  // how to bring it back.
  std::string name;
  if (const Pattern* p = a.doc.song().pattern(a.curPattern)) name = p->name;
  a.doc.removePattern(a.curPattern);
  if (layoutui::active() == layout::Preset::Tracker) a.notify(std::string(i18n::tr("Pattern deleted with its clips (Undo brings them back): ")) + name);
}

void movePattern(App& a, int dir) {
  int i = a.doc.song().patternIndex(a.curPattern);
  if (i >= 0) ops::movePattern(a.doc, a.curPattern, i + dir);
}

std::vector<ObjectId> selectedChannels(const App& a) {
  std::vector<ObjectId> out;
  for (const auto& c : a.doc.song().channels)
    if (a.selChannels.count(c.id) || c.id == a.selChannel) out.push_back(c.id);
  return out;
}

void stopSound(App& a) { menuparity::panicPressed(a); }  // J-109: stops the transport too

}  // namespace

// ---------------------------------------------------------------- menus ----
//
// The Studio menus as data (MenuDef.h): the in-window menu bars draw these definitions, the
// menu tree (palette, system menu bars, tests) is read from them and a chosen item runs from
// them. Providers list what only the project knows (templates, recent files, patterns) and the
// rows other modules keep with their own code.

namespace md = menudef;
using i18n::N_;

namespace {

bool notEmbedded(App& a) { return !a.embedded; }
// R05-18: Studio's own items the Tracks layout leaves out (its windows, playlist wording, repeats).
bool notTracks(App& a) { return !logicplug::active(a); }

void templateItems(App& a) {
  // Shipped and user templates by category (J-24).
  std::string cat;
  for (const auto& t : a.templates()) {
    // R02-22: the Tracks layout lists "Empty project" once (the item above makes the same project);
    // N49-12: Clips lists it once too (the row above).
    if ((logicplug::active(a) || livelook::active(a)) && t.name == "Empty project") continue;
    if (t.category != cat) {
      cat = t.category;
      ImGui::SeparatorText(cat.c_str());
    }
    std::string path = t.path;
    if (ImGui::MenuItem((t.name + "##" + path).c_str())) a.guardUnsaved([&a, path]() { a.newFromTemplate(path); });
    if (!t.description.empty()) ui::Hint(t.description);
  }
}

void recentItems(App& a) {
  for (const auto& r : a.recentFiles)
    if (ImGui::MenuItem((fs::path(r).filename().string() + "##" + r).c_str())) {
      std::string path = r;
      a.guardUnsaved([&a, path]() { a.openProject(path); });
    }
}

const md::Menu& fileDef() {
  // K-77: create, open, save, import / export, sharing, project, quit.
  static const md::Menu m = {
      md::item(N_("New"), "file.new", [](App& a) {
        a.guardUnsaved([&a]() {
          a.newProject();
          if (logicplug::active(a)) newtracks::open();  // R01-17
        });
      }),
      md::submenu(N_("New from template"),
                  {
                      md::item(N_("Browse templates..."), [](App& a) { a.showNewProject = true; }),
                      md::separator(),
                      md::item(N_("Empty project"), [](App& a) { a.guardUnsaved([&a]() { a.newFromTemplate(""); }); }),
                      md::item(N_("Starter kit (drums + synth)"), [](App& a) { a.guardUnsaved([&a]() { a.newFromTemplate("starter"); }); }),
                      md::item(N_("Demo song"), [](App& a) { a.guardUnsaved([&a]() { a.newFromTemplate("demo"); }); }),
                      md::provider(templateItems),
                  }),
      md::item(N_("New from loop starter..."), [](App& a) { j59::openLoopStarter(a); })  // J-59 (GEN-01)
          .tip(N_("Build a ready-to-play loop: style, tempo, key, mood and energy, with parts you can re-roll")),
      md::separator(),
      md::item(N_("Open..."), "file.open", [](App& a) { a.openDialog(); })
          .tip(N_("Gridline projects, FL Studio projects (.flp) and zipped projects (.zip)")),  // J-52
      md::submenu(N_("Open recent"),
                  {
                      md::provider(recentItems),
                      md::separator(),
                      md::item(N_("Clear list"), [](App& a) { a.clearRecents(); }),
                  })
          .when([](App& a) { return !a.recentFiles.empty(); }),
      md::item(N_("Welcome screen"), [](App& a) { a.home.showHome(a.now); })  // J-141
          .tip(N_("Recent projects, templates and the demo song (also a click on the Gridline logo)")),
      md::separator(),
      md::item(N_("Save"), "file.save", [](App& a) { a.save(); }),
      md::item(N_("Save as..."), "file.saveAs", [](App& a) { a.saveAs(); }),
      md::item(N_("Save new version"), "file.saveNewVersion", [](App& a) { a.saveNewVersion(); }),
      md::item(N_("Save as template..."), [](App& a) { a.showSaveTemplate = true; }),
      md::provider([](App& a) { menuparity::fileItems(a); }),  // J-27
      md::item(N_("Revert to last backup"), [](App& a) { a.revertToLastBackup(); }).when([](App& a) { return a.backups != nullptr; }),
      md::separator(),
      md::submenu(N_("Import"),
                  {
                      // J-52: FL Studio projects and zipped projects open through File > Open.
                      md::item(N_("MIDI file..."),
                               [](App& a) {
                                 fileDialog(a, "Import MIDI file", false, importMidiTypes()[0].exts, "",
                                            [&a](const std::string& p) { exportui::importMidiFile(a, p); });
                               }),
                      md::item(N_("DAWproject file..."),  // K-89: opens as a new project
                               [](App& a) {
                                 fileDialog(a, "Import DAWproject", false, {".dawproject"}, "",
                                            [&a](const std::string& p) { exportui::importDawProject(a, p); });
                               }),
                      md::item(N_("FL Studio presets..."),
                               [](App& a) {
                                 a.startPresetImport();
                                 a.showSettings = true;
                                 a.settingsTab = 2;  // Project page shows the progress
                               })
                          .when([](App& a) { return !a.presetImport.running; }),
                      md::separator(),
                      md::item(N_("Audio file into a sampler..."),
                               [](App& a) {
                                 fileDialog(a, "Import audio file", false, importAudioTypes()[0].exts, "",
                                            [&a](const std::string& p) { a.showNewInstrument(a.addInstrument("gl.sampler", p)); });
                               }),
                      md::item(N_("Multisample instrument (.sfz, .sf2)..."),  // K-144: a Multisampler channel playing it
                               [](App& a) {
                                 fileDialog(a, "Import multisample instrument", false, {".sfz", ".sf2"}, "",
                                            [&a](const std::string& p) { a.showNewInstrument(a.addInstrument("gl.multisampler", p)); });
                               }),
                      md::item(N_("Audio file as playlist clip..."),
                               [](App& a) {
                                 fileDialog(a, "Import audio clip", false, importAudioTypes()[0].exts, "",
                                            [&a](const std::string& p) { exportui::importAudioAsClip(a, p); });
                               }).only(notTracks),
                      md::item(N_("Audio file as a region..."),  // R05-18: the Tracks layout's wording
                               [](App& a) {
                                 fileDialog(a, "Import audio file", false, importAudioTypes()[0].exts, "",
                                            [&a](const std::string& p) { exportui::importAudioAsClip(a, p); });
                               }).only([](App& a) { return logicplug::active(a); }),
                      md::item(N_("Import from URL..."), "file.importUrl", [](App& a) { urlimportui::open(a); })  // J-192
                          .tip(N_("A link to a video or audio page: its best audio as a lossless WAV clip")),
                  }),
      md::submenu(N_("Export"),
                  {
                      md::heading(N_("Audio")),
                      md::item(N_("Render to file (WAV, FLAC, MP3, Ogg)..."), "file.render", [](App& a) { exportui::openRender(a, exportui::Preset::Audio); }),
                      md::item(N_("Mixer tracks (stems)..."), "file.renderStems", [](App& a) { exportui::openRender(a, exportui::Preset::Stems); }),
                      md::item(N_("All playlist tracks..."), [](App& a) { exportui::openRender(a, exportui::Preset::PlaylistTracks); }).only(notTracks),
                      md::item(N_("All tracks as audio files..."), [](App& a) { exportui::openRender(a, exportui::Preset::PlaylistTracks); })
                          .only([](App& a) { return logicplug::active(a); }),  // R05-18
                      md::item(N_("All playlist track groups..."), [](App& a) { exportui::openRender(a, exportui::Preset::TrackGroups); }),  // J-31
                      md::provider([](App& a) { menuparity::exportItems(a); }),  // J-27
                      md::heading(N_("MIDI")),
                      md::item(N_("Song as MIDI file..."), "file.exportMidi", [](App& a) { exportui::exportMidiDialog(a, 0); }),
                      md::item(N_("Pattern as MIDI file..."), [](App& a) { exportui::exportMidiDialog(a, 1); }),
                      md::item(N_("Time selection as MIDI file..."), [](App& a) { exportui::exportMidiDialog(a, 2); })
                          .when([](App& a) { return a.playlist.selEnd > a.playlist.selStart; }),
                      md::heading(N_("Project")),
                      md::item(N_("Project data files..."), [](App& a) { exportui::exportDataFilesDialog(a); }),
                      md::item(N_("Project bones..."), [](App& a) { exportui::exportBonesDialog(a); }),
                      md::item(N_("Gridline zipped project (.gridlinez)..."), [](App& a) { exportui::exportZipDialog(a); }),
                      md::item(N_("DAWproject (.dawproject)..."), [](App& a) { exportui::exportDawProjectDialog(a); }),  // K-89
                      md::provider([](App& a) { freezeui::exportItems(a); }),  // K-87
                      md::item(N_("FL Studio project (.flp)..."), [](App& a) { exportui::exportFlpDialog(a); }),  // J-34
                      md::item(N_("FL Studio zipped loop package (.zip)..."), [](App& a) { exportui::exportFlpZipDialog(a); }),  // J-52
                      md::item(N_("FL Studio export report..."), [](App&) { flpexport::openReport(); })  // J-80
                          .when([](App&) { return flpexport::hasReport(); }),
                  }),
      md::item(N_("Locate missing samples..."), [](App& a) { exportui::openMissingSamples(a); }),
      // The sharing items (none inside another host: no empty section).
      md::separator().only(notEmbedded),
      md::provider([](App& a) { collabui::fileMenu(a); }).only(notEmbedded),  // J-111
      md::separator(),
      md::item(N_("Project info..."), "file.projectInfo", [](App& a) { a.showInfo = true; }),
      // R04-09: the Tracks layout quits from its program menu (its File ends with Close and Bounce).
      md::separator().only([](App& a) { return !logicplug::active(a); }),
      md::item(N_("Quit"), "file.quit", [](App& a) { a.quitRequested = true; }).only([](App& a) { return !logicplug::active(a); }),
  };
  return m;
}

// Edit: the shortcut column shows the focused editor's own binding for the action.
const char* editPrefix(const App& a) {
  return a.editFocus == App::EditFocus::Playlist ? "playlist." : a.editFocus == App::EditFocus::Rack ? "rack."
         : a.editFocus == App::EditFocus::Mixer ? "mixer." : "piano.";
}
const char* editKey(const App& a, const char* op) { return keys::label((std::string(editPrefix(a)) + op).c_str()); }

const md::Menu& editDef() {
  static const md::Menu m = {
      md::item(nullptr, "edit.undo", [](App& a) { a.doc.undo(); })
          .text([](App& a) {
            const auto& h = a.doc.history();
            return h.canUndo() ? "Undo " + h.undoLabel() : std::string("Undo");
          })
          .when([](App& a) { return a.doc.history().canUndo(); }),
      md::item(nullptr, "edit.redo", [](App& a) { a.doc.redo(); })
          .text([](App& a) {
            const auto& h = a.doc.history();
            return h.canRedo() ? "Redo " + h.redoLabel() : std::string("Redo");
          })
          .when([](App& a) { return a.doc.history().canRedo(); }),
      md::item(N_("Undo history"), "edit.history", [](App& a) { a.showHistory = !a.showHistory; }).check([](App& a) { return a.showHistory; }),
      md::separator(),
      md::provider([](App& a) {
        std::string target = std::string("Acts on: ") + editTargetName(a) + " (" +
                             (a.editFocus == App::EditFocus::Playlist ? "playlist"
                              : a.editFocus == App::EditFocus::Rack   ? "channel rack"
                              : a.editFocus == App::EditFocus::Mixer  ? "mixer"
                                                                      : "piano roll") + ")";
        if (GImGuiGridlineMenuLive)  // K-112c (round 25): in Live it starts at the items' label column
          ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetCurrentWindow()->DC.MenuColumns.OffsetLabel);
        ImGui::TextDisabled("%s", target.c_str());
      }),
      md::item(N_("Cut"), [](App& a) { editCut(a); }).keyText([](App& a) { return editKey(a, "cut"); }),
      md::item(N_("Copy"), [](App& a) { editCopy(a); }).keyText([](App& a) { return editKey(a, "copy"); }),
      md::item(N_("Paste"), [](App& a) { editPaste(a); }).keyText([](App& a) { return editKey(a, "paste"); }).when([](App& a) { return canPaste(a); }),
      md::item(N_("Duplicate"), [](App& a) { editDuplicate(a); })
          .keyText([](App& a) { return editKey(a, "duplicate"); })
          .when([](App& a) { return a.editFocus != App::EditFocus::Mixer; }),
      md::item(N_("Delete"), [](App& a) { editDelete(a); }).keyText([](App& a) { return editKey(a, "delete"); }),
      md::separator(),
      md::item(N_("Select all"), [](App& a) { editSelectAll(a); }).keyText([](App& a) { return editKey(a, "selectAll"); }),
      md::item(N_("Deselect"), [](App& a) { editDeselect(a); }).keyText([](App& a) { return editKey(a, "deselect"); }),
      md::separator(),
      md::item(N_("Rename..."), "edit.rename", [](App& a) { renameFocused(a); }),
  };
  return m;
}

// K-112b: Edit in the Tracks layout's grouping (MAP.md 2.1): undo; the clipboard with Delete;
// Select as a submenu; Duplicate (repeat once); Rename.
const md::Menu& tracksEditDef() {
  static const md::Menu m = {
      md::item(nullptr, "edit.undo", [](App& a) { a.doc.undo(); })
          .text([](App& a) {
            const auto& h = a.doc.history();
            return h.canUndo() ? "Undo " + h.undoLabel() : std::string("Undo");
          })
          .when([](App& a) { return a.doc.history().canUndo(); }),
      md::item(nullptr, "edit.redo", [](App& a) { a.doc.redo(); })
          .text([](App& a) {
            const auto& h = a.doc.history();
            return h.canRedo() ? "Redo " + h.redoLabel() : std::string("Redo");
          })
          .when([](App& a) { return a.doc.history().canRedo(); }),
      md::item(N_("Undo history"), "edit.history", [](App& a) { a.showHistory = !a.showHistory; }).check([](App& a) { return a.showHistory; }),
      md::separator(),
      md::item(N_("Cut"), [](App& a) { editCut(a); }).keyText([](App& a) { return editKey(a, "cut"); }),
      md::item(N_("Copy"), [](App& a) { editCopy(a); }).keyText([](App& a) { return editKey(a, "copy"); }),
      md::item(N_("Paste"), [](App& a) { editPaste(a); }).keyText([](App& a) { return editKey(a, "paste"); }).when([](App& a) { return canPaste(a); }),
      md::item(N_("Delete"), [](App& a) { editDelete(a); }).keyText([](App& a) { return editKey(a, "delete"); }),
      md::separator(),
      md::submenu(N_("Select"),
                  {md::item(N_("Select all"), [](App& a) { editSelectAll(a); }).keyText([](App& a) { return editKey(a, "selectAll"); }),
                   md::item(N_("Deselect"), [](App& a) { editDeselect(a); }).keyText([](App& a) { return editKey(a, "deselect"); })}),
      md::item(N_("Duplicate"), [](App& a) { editDuplicate(a); })
          .keyText([](App& a) { return editKey(a, "duplicate"); })
          .when([](App& a) { return a.editFocus != App::EditFocus::Mixer; }),
      md::separator(),  // K-112B (checkpoint 39): split and join, as MAP.md 2.1 and 5.3 place them
      md::item(N_("Split"), "playlist.splitAtSongPos", [](App& a) { tracksSplitAtPlayhead(a); }).when([](App& a) { return !a.playlist.selected.empty(); }),
      md::item(N_("Join"), "playlist.merge",
               [](App& a) {
                 if (logicplug::active(a)) return tracksJoin(a);  // checkpoint 47: the Join tool's rules
                 const ObjectId nc = ops::mergeClips(a.doc, a.playlist.selected);
                 if (nc) a.playlist.selected = {nc};
               })
          .when([](App& a) { return a.playlist.selected.size() >= 2; }),
      md::separator(),
      md::item(N_("Rename..."), "edit.rename", [](App& a) { renameFocused(a); }),
  };
  return m;
}

const md::Menu& addDef() {
  // K-77: the things made most often first, then clips, then tracks.
  static const md::Menu m = {
      md::submenu(N_("Instrument channel"), {md::provider([](App& a) { addChannelMenu(a); })}),
      md::item(N_("Pattern"), "pattern.new", [](App& a) { newPattern(a); }),
      md::submenu(nullptr,
                  {md::provider([](App& a) {
                    const MixerTrack* mt = a.doc.song().mixerTrack(a.selMixer);
                    if (!mt) return;
                    ObjectId id = mt->id;
                    effectItems([&a, id](const std::string& type) {
                      const MixerTrack* m = a.doc.song().mixerTrack(id);
                      for (int k = 0; m && k < kEffectSlots; ++k)
                        if (m->slots[static_cast<size_t>(k)].plugin.empty()) {
                          a.doc.setSlotPlugin(id, k, type);
                          a.fxEditor = {id, k};
                          return;
                        }
                      a.notify("All effect slots of this track are in use", true);
                    });
                  })})
          .text([](App& a) {
            const Song& s = a.doc.song();
            const MixerTrack* mt = s.mixerTrack(a.selMixer);
            return "Effect on " + (mt ? mixerTrackName(s, static_cast<size_t>(s.mixerIndex(mt->id))) : std::string("mixer track"));
          })
          .when([](App& a) { return a.doc.song().mixerTrack(a.selMixer) != nullptr; }),
      md::provider([](App& a) { menuparity::addItems(a); }),  // J-27: automation clip for the last moved control
      md::separator(),
      md::item(N_("Clip of the current pattern at the song end"),
               [](App& a) {
                 const Song& s = a.doc.song();
                 int track = 0;
                 for (const auto& c : s.playlist)
                   if (c.pattern == a.curPattern) track = c.track;
                 const Tick at = MeterMap(s).roundUpToBar(s.lengthTicks());  // K-193: the next bar line of the song's meters
                 ObjectId id = a.doc.addClip(a.curPattern, track, at);
                 if (id) a.playlist.selected = {id};
               }).only(notTracks),
      md::separator(),
      md::item(N_("Mixer track"),
               [](App& a) {
                 ObjectId id = a.doc.addMixerTrack();
                 if (id != kNoId) a.selMixer = id;
               }),
      md::item(N_("8 playlist tracks"),
               [](App& a) {
                 if (livelook::active(a)) trackmodes::addUsedTrack(a.doc, 8);  // K-112c: Clips lists the tracks in use
                 else a.doc.setPlaylistTrackCount(static_cast<int>(a.doc.song().tracks.size()) + 8);
               })
          .when([](App& a) { return static_cast<int>(a.doc.song().tracks.size()) + 8 <= kMaxPlaylistTracks; })
          .only(notTracks),
  };
  return m;
}

bool hasPattern(App& a) { return a.doc.song().pattern(a.curPattern) != nullptr; }
int patternIdx(App& a) { return a.doc.song().patternIndex(a.curPattern); }

const md::Menu& patternsDef() {
  // K-77: create, go to, properties, arrange, render, delete (the pattern context menu keeps the same order).
  static const md::Menu m = {
      md::item(N_("New pattern"), "pattern.new", [](App& a) { newPattern(a); }),
      md::item(N_("Insert one before"), "pattern.insert",
               [](App& a) { a.selectPattern(ops::insertPattern(a.doc, a.curPattern, ops::nextPatternName(a.doc.song()))); }),
      md::item(N_("Clone"), "pattern.clone", [](App& a) { a.selectPattern(a.doc.clonePattern(a.curPattern)); }).when(hasPattern),
      md::item(N_("Find first empty"), "pattern.findFirstEmpty", [](App& a) { findEmptyPattern(a, true, true); }),
      md::item(N_("Find next empty"), "pattern.findNextEmpty", [](App& a) { findEmptyPattern(a, false, false); }),
      md::separator(),
      md::submenu(N_("Go to"), {md::provider([](App& a) {
                    for (const auto& p : a.doc.song().patterns) {
                      std::string label = p.name + "##" + std::to_string(p.id);
                      if (ImGui::MenuItem(label.c_str(), nullptr, p.id == a.curPattern)) a.selectPattern(p.id);
                    }
                  })}),
      md::item(N_("Next pattern"), "pattern.next", [](App& a) { a.selectPattern(a.doc.song().patterns[static_cast<size_t>(patternIdx(a) + 1)].id); })
          .when([](App& a) { return patternIdx(a) + 1 < static_cast<int>(a.doc.song().patterns.size()); }),
      md::item(N_("Previous pattern"), "pattern.previous", [](App& a) { a.selectPattern(a.doc.song().patterns[static_cast<size_t>(patternIdx(a) - 1)].id); })
          .when([](App& a) { return patternIdx(a) > 0; }),
      md::separator(),
      md::item(N_("Rename..."), "edit.rename",
               [](App& a) {
                 const ObjectId id = a.curPattern;
                 openPrompt("Rename pattern", a.doc.song().pattern(id)->name, [&a, id](const std::string& n) { a.doc.renamePattern(id, n); });
               })
          // K-112B: in the Tracks layout Shift-Return renames the track, so this item shows no key there.
          .keyText([](App& a) { return logicplug::active(a) ? nullptr : keys::label("edit.rename"); })
          .when(hasPattern),
      md::submenu(N_("Colour"), {md::provider([](App& a) {
                    uint32_t col;
                    if (colorGrid(&col)) ops::setPatternColor(a.doc, a.curPattern, col);
                  })})
          .when(hasPattern),
      md::item(N_("Random colour"), [](App& a) { ops::setPatternColor(a.doc, a.curPattern, randomColor()); }).when(hasPattern),
      md::submenu(N_("Length"), {md::provider([](App& a) {
                    const Pattern* cur = a.doc.song().pattern(a.curPattern);
                    if (!cur) return;
                    for (int b : {1, 2, 4, 8, 16}) {
                      std::string l = std::to_string(b) + (b == 1 ? " bar" : " bars");
                      if (ImGui::MenuItem(l.c_str(), nullptr, cur->bars == b)) a.doc.setPatternLength(a.curPattern, b, cur->stepsPerBar);
                    }
                  })})
          .when(hasPattern),
      md::provider([](App& a) { meterui::patternMeterMenu(a, a.curPattern); }),  // K-179
      md::submenu(N_("Transpose"), {md::provider([](App& a) {
                    for (int st : {12, 1, -1, -12}) {
                      std::string l = (st > 0 ? "+" : "") + std::to_string(st) + (std::abs(st) == 12 ? " (octave)" : " semitone");
                      if (ImGui::MenuItem(l.c_str())) ops::transposePattern(a.doc, a.curPattern, st);
                    }
                  })})
          .when(hasPattern),
      md::separator(),
      md::item(N_("Split by channel"),
               [](App& a) {
                 auto ids = ops::splitPatternByChannel(a.doc, a.curPattern);
                 a.notify(ids.empty() ? std::string("Nothing to split: the pattern is empty") : "Split into " + std::to_string(ids.size()) + " patterns");
               })
          .when(hasPattern),
      md::item(N_("Select in playlist"),
               [](App& a) {
                 a.playlist.selected.clear();
                 for (const auto& c : a.doc.song().playlist)
                   if (c.pattern == a.curPattern) a.playlist.selected.insert(c.id);
                 a.showPlaylist = true;
               })
          .when(hasPattern),
      md::provider([](App& a) { j31::patternsItems(a); }),  // J-31: pattern groups
      md::item(N_("Move up"), "pattern.moveUp", [](App& a) { movePattern(a, -1); }).when([](App& a) { return patternIdx(a) > 0; }),
      md::item(N_("Move down"), "pattern.moveDown", [](App& a) { movePattern(a, 1); }).when([](App& a) {
        const int idx = patternIdx(a);
        return idx >= 0 && idx + 1 < static_cast<int>(a.doc.song().patterns.size());
      }),
      md::separator(),
      md::item(N_("Render pattern to file..."), [](App& a) { exportui::openRender(a, exportui::Preset::PatternFile); }).when(hasPattern),
      md::item(N_("Render as audio clip..."), [](App& a) { exportui::openRender(a, exportui::Preset::AudioClip); }).when(hasPattern),
      md::item(N_("Render and replace..."), [](App& a) { exportui::openRender(a, exportui::Preset::PatternReplace); }).when(hasPattern),
      md::separator(),
      md::item(N_("Delete"), "pattern.delete", [](App& a) { deletePattern(a); }).when([](App& a) { return a.doc.song().patterns.size() > 1; }),
      md::item(N_("Delete unused patterns"),
               [](App& a) {
                 int n = ops::deleteUnusedPatterns(a.doc);
                 a.notify(n ? "Deleted " + std::to_string(n) + " unused pattern(s)" : std::string("No unused patterns"));
               }),
  };
  return m;
}

// ------------------------------------------------------- Clips' File and Edit (N47-13) ----
//
// Checkpoint 49: the Clips layout's own File and Edit, as the reference's (MAP.md 2.2, 2.3), in place
// of Studio's whole menus. The rows are Studio's (copied, so each does exactly what it does there),
// regrouped and worded for clips and tracks; Studio's pattern list rows (go to, next, move, find empty,
// pattern groups) have no place here: a MIDI clip is made, renamed, coloured, sized and deleted as a clip.

const md::Entry& rowOf(const md::Menu& m, const char* label) {
  for (const md::Entry& e : m)
    if (e.label && !std::strcmp(e.label, label)) return e;
  static const md::Entry none = md::separator();
  return none;
}

bool anySelected(App& a) { return !a.playlist.selected.empty(); }

// The one selected pattern clip whose length is not its notes' loop (Crop has work to do).
ObjectId cropTarget(App& a) {
  if (a.playlist.selected.size() != 1) return kNoId;
  const Song& s = a.doc.song();
  const Clip* c = s.clip(*a.playlist.selected.begin());
  const Pattern* p = c && c->kind == ClipKind::Pattern ? s.pattern(c->pattern) : nullptr;
  return p && c->length != 0 && c->length != s.patternContentLength(*p) ? c->id : kNoId;
}

const md::Menu& clipsFileDef() {
  // New, open and recent; the saves (a new project folder is the reference's collect and save);
  // the project's files; Export audio and Export MIDI clip; then Gridline's own rows (templates,
  // the loop starter, the welcome screen, backups, the other imports and exports in one submenu,
  // sharing); Quit.
  static const md::Menu m = [] {
    const md::Menu& f = fileDef();
    const md::Menu& imp = rowOf(f, "Import").children;
    const md::Menu& exp = rowOf(f, "Export").children;
    md::Menu out = {
        rowOf(f, "New"),
        md::item(N_("Open..."), "file.open", [](App& a) { a.openDialog(); }).tip(N_("Gridline projects and zipped projects")),
        rowOf(f, "Open recent"),
        md::separator(),
        rowOf(f, "Save"),
        rowOf(f, "Save as..."),
        rowOf(f, "Save new version"),
        md::provider([](App& a) { menuparity::fileItems(a); }),  // Save to a new project folder: every sample copied in
        rowOf(f, "Save as template..."),
        md::separator(),
        rowOf(f, "Locate missing samples..."),
        rowOf(f, "Project info..."),
        md::separator(),
        md::item(N_("Export audio..."), "file.render", [](App& a) { exportui::openRender(a, exportui::Preset::Audio); })
            .tip(N_("The song, or a part of it, as a WAV, FLAC, MP3 or Ogg file")),
        md::item(N_("Export MIDI clip..."), [](App& a) { exportui::exportMidiDialog(a, 1); })
            .when(hasPattern)
            .tip(N_("The selected MIDI clip's notes as a MIDI file")),
        md::item(N_("Export the arrangement as MIDI..."), "file.exportMidi", [](App& a) { exportui::exportMidiDialog(a, 0); }),
        md::separator(),
        rowOf(f, "New from template"),
        rowOf(f, "New from loop starter..."),
        rowOf(f, "Welcome screen"),
        rowOf(f, "Revert to last backup"),
        md::submenu(N_("Other imports and exports"),
                    {
                        md::heading(N_("Import")),
                        rowOf(imp, "MIDI file..."),
                        rowOf(imp, "Audio file into a sampler..."),
                        rowOf(imp, "Multisample instrument (.sfz, .sf2)..."),
                        rowOf(imp, "Import from URL..."),
                        rowOf(imp, "FL Studio presets..."),
                        md::heading(N_("Audio")),
                        // N50-09: no mixer-track stems row in Clips: "Each track..." is the reference's individual tracks
                        md::item(N_("Each track..."), [](App& a) { exportui::openRender(a, exportui::Preset::PlaylistTracks); }),
                        md::item(N_("Each track group..."), [](App& a) { exportui::openRender(a, exportui::Preset::TrackGroups); }),
                        md::provider([](App& a) { menuparity::exportItems(a); }),
                        md::heading(N_("MIDI")),
                        rowOf(exp, "Time selection as MIDI file..."),
                        md::heading(N_("Project")),
                        rowOf(exp, "Project data files..."),
                        rowOf(exp, "Project bones..."),
                        rowOf(exp, "Gridline zipped project (.gridlinez)..."),
                        md::provider([](App& a) { freezeui::exportItems(a); }),
                        rowOf(exp, "FL Studio project (.flp)..."),
                        rowOf(exp, "FL Studio zipped loop package (.zip)..."),
                        rowOf(exp, "FL Studio export report..."),
                    }),
        md::separator().only(notEmbedded),
        md::provider([](App& a) { collabui::fileMenu(a); }).only(notEmbedded),
        md::separator(),
        rowOf(f, "Quit"),
    };
    return out;
  }();
  return m;
}

const md::Menu& clipsEditDef() {
  // Undo; the clipboard; the clip edits (split, consolidate, crop); rename, select and deactivate;
  // the clip renders last (the reference's bounce rows).
  static const md::Menu m = [] {
    const md::Menu& e = editDef();
    md::Menu out = {
        e[0], e[1], e[2],  // Undo, Redo, Undo history
        md::separator(),
        rowOf(e, "Cut"),
        rowOf(e, "Copy"),
        rowOf(e, "Paste"),
        rowOf(e, "Duplicate"),
        rowOf(e, "Delete"),
        md::separator(),
        md::item(N_("Split"), "playlist.splitAtSongPos", [](App& a) { tracksSplitAtPlayhead(a); }).when(anySelected),
        md::item(N_("Consolidate"), "playlist.merge",
                 [](App& a) {
                   if (const ObjectId nc = ops::mergeClips(a.doc, a.playlist.selected)) a.playlist.selected = {nc};
                 })
            .when([](App& a) { return a.playlist.selected.size() >= 2; }),
        md::item(N_("Crop clip"), [](App& a) { a.doc.setClipLength(cropTarget(a), 0); }).when([](App& a) { return cropTarget(a) != kNoId; }),
        md::separator(),
        // Checkpoint 52 (N51-11): grouping in the Edit menu as well, as the reference's.
        md::item(N_("Group tracks"), "track.group", [](App& a) { livelook::groupTracks(a); }),
        md::item(N_("Ungroup tracks"), "track.ungroup", [](App& a) { livelook::ungroupTracks(a); }),
        md::separator(),
        rowOf(e, "Rename..."),
        rowOf(e, "Select all"),
        rowOf(e, "Deselect"),
        md::item(N_("Deactivate"), "playlist.mute",
                 [](App& a) {
                   const std::set<ObjectId> sel = a.playlist.selected;
                   bool all = true;
                   for (ObjectId id : sel)
                     if (const Clip* c = a.doc.song().clip(id)) all = all && c->muted;
                   a.doc.beginGroup(all ? "activate clips" : "deactivate clips");
                   for (ObjectId id : sel) a.doc.editClip(id, "deactivate clip", [all](Clip& x) { x.muted = !all; });
                   a.doc.endGroup();
                 })
            .when(anySelected)
            .check([](App& a) {
              bool any = false;
              for (ObjectId id : a.playlist.selected)
                if (const Clip* c = a.doc.song().clip(id)) {
                  if (!c->muted) return false;
                  any = true;
                }
              return any;
            }),
        md::separator(),
        md::item(N_("Render clip to audio..."), [](App& a) { exportui::openRender(a, exportui::Preset::AudioClip); })
            .when(hasPattern)
            .tip(N_("The selected MIDI clip rendered to an audio clip")),
        md::item(N_("Render clip in place..."), [](App& a) { exportui::openRender(a, exportui::Preset::PatternReplace); })
            .when(hasPattern)
            .tip(N_("The selected MIDI clip rendered to audio, in its place")),
    };
    return out;
  }();
  return m;
}

// View: zoom acts on the piano roll when it was the last editor used, otherwise on the playlist.
bool zoomPiano(App& a) { return a.editFocus == App::EditFocus::Piano; }
float& zoomRequest(App& a) { return zoomPiano(a) ? a.piano.zoomRequest : a.playlist.zoomRequest; }

// C47: Studio's windows that the Clips layout has no place for (its bands and views are in Navigate).
bool studioWindows(App& a) { return !livelook::active(a); }
bool studioNotTracks(App& a) { return studioWindows(a) && notTracks(a); }  // neither Clips nor Tracks

const md::Menu& viewDef() {
  static const md::Menu m = {
      md::item(N_("Command palette..."), "view.commandPalette", [](App& a) { cmdpalette::open(a); })  // K-93
          .tip(N_("Find and run any command, menu item, window, setting or plugin by typing part of its name")),
      md::heading(N_("Windows")),
      md::toggle(N_("Playlist"), "view.playlist", [](App& a) { return &a.showPlaylist; }).only(studioNotTracks),
      md::toggle(N_("Channel rack"), "view.channelRack", [](App& a) { return &a.showRack; }).only(studioNotTracks),
      md::toggle(N_("Piano roll"), "view.pianoRoll", [](App& a) { return &a.showPiano; }).only(studioWindows),
      md::toggle(N_("Mixer"), "view.mixer", [](App& a) { return &a.showMixer; }),  // K-77: with the other main editors
      // C47: the return tracks' strips and rows shown or folded away, as the reference's mixer controls.
      md::item(N_("Return tracks"), [](App& a) { a.settings.set("clips.returnTracks", !a.settings.getBool("clips.returnTracks")); })
          .check([](App& a) { return a.settings.getBool("clips.returnTracks"); })
          .tip(N_("Show the return tracks beside Main in the clip grid, the mixer and the arrangement"))
          .only([](App& a) { return livelook::active(a); }),
      md::toggle(N_("Browser"), "view.browser", [](App& a) { return &a.showBrowser; }),
      md::item(N_("Plugin picker"), "view.pluginPicker", [](App&) { browserui::togglePluginPicker(); }).check([](App&) { return browserui::pluginPickerOpen(); }).only(studioWindows),
      md::provider([](App& a) { menuparity::viewItems(a); }).only(studioNotTracks),  // J-27: project picker
      md::item(N_("Channel settings"), [](App& a) { a.settingsChannel = a.settingsChannel != kNoId ? kNoId : a.selChannel; })
          .check([](App& a) { return a.settingsChannel != kNoId; })
          .when([](App& a) { return a.doc.song().channel(a.selChannel) != nullptr; })
          .only(studioWindows),
      md::toggle(N_("Master chain"), nullptr, [](App& a) { return &a.showMaster; }).only(studioWindows),  // RN-09
      md::toggle(N_("Pads"), nullptr, [](App& a) { return &a.showLivePads; }).only(studioNotTracks),
      md::toggle(N_("Undo history"), "edit.history", [](App& a) { return &a.showHistory; }),
      md::toggle(N_("Performance"), nullptr, [](App& a) { return &a.showPerformance; }),
      md::toggle(N_("Groove editor"), nullptr, [](App& a) { return &a.showGroove; }).only(studioNotTracks),  // K-151
      md::toggle(N_("Stage"), "view.stage", [](App& a) { return &a.showStage; }).only(studioNotTracks),
      md::submenu(N_("Panels"), {md::provider([](App& a) { layoutparts::panelsMenu(a); })}),  // the layout parts, in every layout
      // R05-18: the Tracks layout's View has its own Zoom submenu (no repeat here).
      md::heading(nullptr).text([](App& a) { return std::string(zoomPiano(a) ? i18n::tr("Zoom (piano roll)") : i18n::tr("Zoom (playlist)")); }).only(notTracks),
      md::item(N_("Zoom in"), [](App& a) { zoomRequest(a) *= 1.5f; }).keyText([](App& a) { return keys::label(zoomPiano(a) ? "piano.zoomIn" : "playlist.zoomIn"); }).only(notTracks),
      md::item(N_("Zoom out"), [](App& a) { zoomRequest(a) /= 1.5f; }).keyText([](App& a) { return keys::label(zoomPiano(a) ? "piano.zoomOut" : "playlist.zoomOut"); }).only(notTracks),
      md::item(nullptr, [](App& a) { (zoomPiano(a) ? a.piano.fitRequest : a.playlist.fitRequest) = true; })
          .text([](App& a) { return std::string(zoomPiano(a) ? i18n::tr("Zoom to fit the pattern") : i18n::tr("Zoom to fit the song")); })
          .keyText([](App& a) { return keys::label(zoomPiano(a) ? "piano.zoomFit" : "playlist.zoomFit"); }).only(notTracks),
      md::item(N_("Zoom to ticks (snap turns free)"), [](App& a) { zoomRequest(a) = 1e9f; }).only(studioNotTracks),
      md::item(N_("Clear time selection"), [](App& a) { a.playlist.selStart = a.playlist.selEnd = 0; })
          .only([](App& a) { return !zoomPiano(a); })
          .when([](App& a) { return a.playlist.selEnd > a.playlist.selStart; }),
      md::heading(N_("Layout")),
      md::item(N_("Close plugin windows"), "view.closePlugins", [](App& a) { a.closePluginWindows(); }),
      md::item(N_("Close all windows"), "view.closeAll", [](App& a) { closeAll(a); }),
      md::item(N_("Bring all windows back into Gridline"), [](App&) { viewports::gatherAll(); })  // J-113
          .when([](App&) { return viewports::detachedCount() > 0; }),
      md::submenu(N_("Window layout"), {md::provider([](App& a) { layoutui::viewLayoutMenu(a); })}),  // K-79
      md::submenu(N_("Workspaces"), {md::provider([](App& a) { workspacesui::menu(a); })}),  // K-237
      md::item(N_("Reopen closed pane tab"), "view.reopenTab", [](App& a) { layoutui::reopenClosedTab(a); }),
      md::item(N_("Reset window layout"), "view.resetLayout", [](App& a) { ui::confirmLayoutReset(a); }),
      md::heading(N_("Appearance")),
      md::item(N_("Dark theme"),
               [](App& a) {
                 const bool dark = theme::isDark();
                 const dispconf::Snapshot before = dispconf::take(a.settings, dispconf::themeKeys());
                 theme::modeToSettings(dark ? theme::Mode::Light : theme::Mode::Dark, a.settings);
                 theme::applyFromSettings(a.settings);
                 ui::confirmTheme(a, before, "Theme");
               })
          .check([](App&) { return theme::isDark(); }),
      md::item(N_("White text and glyphs"),  // K-177
               [](App& a) {
                 theme::Tweaks t = theme::tweaksFromSettings(a.settings);
                 const dispconf::Snapshot before = dispconf::take(a.settings, dispconf::themeKeys());
                 t.whiteText = !t.whiteText;
                 theme::tweaksToSettings(t, a.settings);
                 theme::applyFromSettings(a.settings);
                 ui::confirmTheme(a, before, "White text");
               })
          .check([](App& a) { return theme::tweaksFromSettings(a.settings).whiteText; }),
      md::item(N_("Theme settings..."), [](App& a) { openSettings(a, 9); }),
      md::submenu(N_("Interface size"), {md::provider([](App& a) {
                    double cur = a.settings.getNumber("ui.scale");
                    for (int pct : {75, 90, 100, 110, 125, 150, 175, 200}) {
                      std::string l = std::to_string(pct) + "%";
                      if (ImGui::MenuItem(l.c_str(), nullptr, std::fabs(cur * 100 - pct) < 0.5)) ui::confirmScale(a, pct / 100.0);  // J-77: Keep / Revert
                    }
                  })}),
      md::submenu(N_("Mixer layout"), {md::provider([](App& a) {
                    const int cur = static_cast<int>(a.settings.getNumber("ui.mixerLayout"));
                    const char* names[] = {N_("Compact strips"), N_("Normal strips"), N_("Wide strips")};
                    for (int i = 0; i < 3; ++i)
                      if (ImGui::MenuItem(i18n::tr(names[i]), nullptr, cur == i)) a.settings.set("ui.mixerLayout", i);
                  })}),
      md::provider([](App& a) { menuparity::viewHintItems(a); }),  // J-27: played chords in the hint bar
  };
  return m;
}

const md::Menu& optionsDef() {
  // K-77: settings pages (in the Settings window's tab order), then the switches by subject.
  static const md::Menu m = {
      md::heading(N_("Settings")),
      md::item(N_("Audio settings..."), [](App& a) { openSettings(a, 0); }),
      md::item(N_("MIDI settings..."), "view.settings", [](App& a) { openSettings(a, 1); })
          .keyText([](App& a) { return logicplug::active(a) ? nullptr : keys::label("view.settings"); }),  // R02-22: Cmd+, is Settings... there
      md::item(N_("Project settings..."), [](App& a) { openSettings(a, 2); }),
      md::item(N_("Backup settings..."), [](App& a) { openSettings(a, 3); }),
      md::item(N_("Interface settings..."), [](App& a) { openSettings(a, 4); }),
      md::item(N_("General settings..."), [](App& a) { openSettings(a, 5); }),
      md::item(N_("File settings..."), [](App& a) { openSettings(a, 6); }),
      md::item(N_("Debug log and script console..."), [](App& a) { openSettings(a, 7); }),
      md::item(N_("Keyboard shortcuts..."), [](App& a) { openSettings(a, 8); }),
      md::item(N_("Theme..."), [](App& a) { openSettings(a, 9); }),
      md::item(N_("Plugin manager..."), [](App& a) { a.showPluginManager = true; }),
      md::heading(N_("Playback")),
      md::item(N_("Song mode"),
               [](App& a) { a.engine.setMode(a.engine.mode() == PlayMode::Song ? PlayMode::Pattern : PlayMode::Song); })
          .keyText([](App&) { return "L"; })
          .check([](App& a) { return a.engine.mode() == PlayMode::Song; })
          .only(studioNotTracks),  // N45-09: Clips has no play modes to pick; R05-18: the Tracks layout always plays its tracks
      md::item(N_("Live mode"), [](App& a) { a.engine.setMode(a.engine.mode() == PlayMode::Live ? PlayMode::Song : PlayMode::Live); })
          .check([](App& a) { return a.engine.mode() == PlayMode::Live; })
          .only(studioNotTracks),
      md::submenu(N_("Arrangements"), {md::provider([](App& a) { arrangementMenu(a); })}).only(studioWindows),  // N49-12: one Arrangement per Set
      md::provider([](App& a) { j31::optionsItems(a); }),  // J-31: song position jumps
      md::toggle(N_("Metronome"), "options.metronome", [](App& a) { return &a.metronome; }).only(studioWindows),  // N48-15: Clips: in Recording and metronome
      md::item(N_("Plugin delay compensation"),
               [](App& a) {
                 a.settings.set("audio.pdc", !a.settings.getBool("audio.pdc"));
                 a.syncEngine();
               })
          .check([](App& a) { return a.settings.getBool("audio.pdc"); })
          .tip(N_("Delays faster paths so tracks with latent effects (look-ahead, linear phase, oversampling) stay in time")),
      md::provider([](App& a) { freezeui::optionsItems(a); }),     // K-87: refreeze, hibernate
      md::provider([](App& a) { menuparity::optionsItems(a); }),  // J-27: MIDI clock out
      md::heading(N_("Recording")),
      md::toggle(N_("Record notes"), nullptr, [](App& a) { return &a.recordArmed; }).only(studioWindows),  // N48-15: Clips: Arrangement record
      // Clips: these rows are once, in Options > Recording and metronome (the same rows twice before).
      md::toggle(N_("Step record"), nullptr, [](App& a) { return &a.stepRecord; }).only(studioWindows),
      md::toggle(N_("Record automation"), nullptr, [](App&) { return &automationRecording(); }),
      md::submenu(N_("Recording options"), {md::provider([](App& a) { recordOptionsMenu(a); })}).only(studioWindows),  // the record button's menu (audio takes included)
      md::heading(N_("Keyboard and controllers")).only(studioWindows),
      md::toggle(N_("Typing keyboard to piano"), "options.typingKeyboard", [](App& a) { return &a.typingKeyboard; }).only(studioWindows),
      md::submenu(N_("Typing keyboard options"), {md::provider([](App& a) { typingKeyboardMenu(a); })}).only(studioWindows),
      md::toggle(N_("Multilink to controllers"), nullptr, [](App& a) { return &a.multilink; }).only(studioWindows),
      md::heading(N_("Editing")),
      md::provider([](App& a) {
        autoScrollItem(a, false);  // K-4: the playlist, and the pattern editors on their own
        autoScrollItem(a, true);
      }),
      md::item(N_("Smooth snapping"), [](App& a) { a.settings.set("ui.smoothSnap", !a.settings.getBool("ui.smoothSnap")); })  // J-37
          .check([](App& a) { return a.settings.getBool("ui.smoothSnap"); })
          .tip(N_("Clips and notes dragged on a snapped grid glide from cell to cell instead of jumping (positions still land exactly on the grid)")),
  };
  return m;
}

void tapperBody(App& a) {
  const ops::TapTempo& tapper = a.tapper;  // K-48: the top bar button's tapper
  ImGui::TextDisabled("%s", i18n::tr("Click Tap (or press T) on every beat"));
  bool tap = ImGui::Button(i18n::tr("Tap"), ImVec2(160, 36));
  devNoteRect("tapmenutap");
  tap = tap || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_T, false));
  if (tap) a.tapMeasure();
  const double bpm = tapper.bpm();
  if (bpm > 0) ImGui::Text(i18n::tr("%.1f BPM  (%d taps)"), bpm, static_cast<int>(tapper.taps.size()));
  else ImGui::TextDisabled("—");
  if (ImGui::Button(i18n::tr("Set the song tempo")) && bpm > 0) a.doc.setParam(songParam(SongParam::Tempo), std::round(bpm * 10.0) / 10.0, false);
  ImGui::SameLine();
  if (ImGui::Button(i18n::tr("Reset"))) a.tapReset();
}

const md::Menu& toolsDef() {
  // K-77: quick actions, composing, audio and recording, macros, clean-up (removals last), assistant.
  // K-158: each section keeps its most used rows; the rest sit in a submenu named after the section.
  static const md::Menu m = {
      md::item(N_("Stop all sound"), "tools.stopSound", [](App& a) { stopSound(a); }),
      md::submenu(N_("Tempo tapper"), {md::provider(tapperBody)}).note("tapmenu"),
      md::provider([](App& a) { menuparity::toolsControlItems(a); }),  // J-27: last moved controls
      md::separator(),
      md::item(N_("Loop starter..."), [](App& a) { j59::openLoopStarter(a); }),  // J-59 (GEN-01)
      md::submenu(N_("Swing presets"), {md::provider([](App& a) { grooveui::presetMenuItems(a); })}),  // K-151
      md::submenu(N_("Chord progression"), {md::provider([](App& a) { progressionItems(a, false); })}),
      md::submenu(N_("Riff from progression"), {md::provider([](App& a) { progressionItems(a, true); })}),
      md::submenu(N_("Piano roll tools"), {md::provider([](App& a) { pianoToolItems(a); })})
          .only(studioWindows),  // checkpoint 52 (N50-09): in Clips these are the note editor's
      md::submenu(N_("Composing"),
                  {
                      md::submenu(N_("Piano roll selection"), {md::provider([](App& a) { pianoSelectItems(a); })})
                          .only(studioWindows),  // N50-09 (checkpoint 53): the note editor's own in Clips
                      md::provider([](App& a) { menuparity::toolsNoteItems(a); }),  // J-27: last piano roll script, note clipboard
                      md::submenu(N_("Project scripts"), {md::provider([](App& a) { projectScriptsMenuItems(a); })}),  // J-57 (SC-02)
                  })
          .rect("toolsmenu composing"),  // K-158: scripted screenshots open it
      md::separator(),
      md::item(N_("Render channel to sampled instrument..."), [](App& a) { j59::openRenderSampler(a, a.selChannel); })  // J-59 (RN-08)
          .when([](App& a) { return a.doc.song().channel(a.selChannel) != nullptr; }),
      md::provider([](App& a) { j31::toolsItems(a); }),                 // J-31: audio logger, cancel recording
      md::provider([](App& a) { menuparity::toolsScoreLogItem(a); }),  // J-27
      md::separator(),
      md::item(N_("Route channels to free mixer tracks"), "rack.routeFree", [](App& a) { chanroute::routeToFree(a, selectedChannels(a)); })
          .only(studioWindows),  // N50-09: Studio's rack and mixer terms (Clips' tracks are their own strips)
      md::item(N_("Unmute all channels"),
               [](App& a) {
                 std::vector<ObjectId> all;
                 for (const auto& c : a.doc.song().channels) all.push_back(c.id);
                 ops::setChannelsMuted(a.doc, all, false);
               }),
      md::item(N_("Unsolo all mixer tracks"),
               [](App& a) {
                 a.doc.editSong("unsolo all", [](Song& s) {
                   for (auto& m : s.mixer) m.solo = false;
                 });
               })
          .only(studioWindows),
      md::item(N_("Unsolo all tracks"),  // N50-09: Clips' wording
               [](App& a) {
                 a.doc.editSong("unsolo all", [](Song& s) {
                   for (auto& m : s.mixer) m.solo = false;
                 });
               })
          .only([](App& a) { return !studioWindows(a); }),
      md::item(N_("Close plugin windows"), "view.closePlugins", [](App& a) { a.closePluginWindows(); }),
      md::submenu(N_("Macros"),
                  {
                      md::item(N_("Route every channel to its own mixer track"),
                               [](App& a) {
                                 std::vector<ObjectId> all;
                                 for (const auto& c : a.doc.song().channels)
                                   if (c.mixerTrack == a.doc.song().masterId()) all.push_back(c.id);
                                 auto t = ops::routeToFreeMixerTracks(a.doc, all);
                                 a.notify(t.empty() ? std::string("Every channel already has a mixer track") : "Routed " + std::to_string(t.size()) + " channel(s)");
                               })
                          .only(studioWindows),
                      md::item(N_("Select unused channels"),
                               [](App& a) {
                                 auto u = ops::unusedChannels(a.doc.song());
                                 if (u.empty()) {
                                   a.notify("Every channel is used in a pattern");
                                 } else {
                                   a.selChannels = std::set<ObjectId>(u.begin(), u.end());
                                   a.selChannel = u.front();
                                   a.showRack = true;
                                 }
                               }),
                      md::provider([](App& a) { menuparity::toolsMacroItems(a); }),  // J-27: track locks, live area, audio clip channels, declicking
                  })
          .rect("toolsmenu macros"),  // K-158: scripted screenshots open it
      md::submenu(N_("Freeze and hibernate"), {md::provider([](App& a) { freezeui::toolsMacroItems(a); })})  // K-87
          .only(studioWindows),
      md::submenu(N_("Freeze"), {md::provider([](App& a) { freezeui::toolsMacroItems(a); })})  // N50-09: Clips has no hibernation
          .only([](App& a) { return !studioWindows(a); })
          .rect("toolsmenu freeze"),
      md::separator(),
      md::submenu(N_("Clean up"),
                  {
                      md::item(N_("Trim unused mixer tracks"),
                               [](App& a) {
                                 int n = ops::trimUnusedMixerTracks(a.doc);
                                 a.notify(n ? "Removed " + std::to_string(n) + " unused mixer track(s)" : std::string("No unused mixer tracks at the end"));
                               })
                          .only(studioWindows),
                      md::provider([](App& a) { menuparity::toolsCleanupItems(a); }),  // J-27: unused automation clips
                      md::item(N_("Delete unused patterns"),
                               [](App& a) {
                                 int n = ops::deleteUnusedPatterns(a.doc);
                                 a.notify(n ? "Deleted " + std::to_string(n) + " unused pattern(s)" : std::string("No unused patterns"));
                               }),
                  })
          .rect("toolsmenu cleanup"),
      md::separator(),
      md::item(N_("AI assistant activity"), [](App&) { mcpui::showActivity(); }),  // SC-07
  };
  return m;
}

const md::Menu& helpDef() {
  static const md::Menu m = {
      md::item(N_("Keyboard shortcuts"), "help.shortcuts", [](App& a) { a.showShortcuts = true; }),
      md::item(N_("Load demo song"), [](App& a) { a.guardUnsaved([&a]() { a.newFromTemplate("demo"); }); }),
      md::item(N_("Track Receipt..."), [](App& a) { receiptui::open(a); }),  // J-219: parts of licensed samples used
      md::item(N_("Show settings folder"), [](App& a) { a.notify("Settings, presets and templates: " + a.configDir()); }),
      md::separator(),
      md::item(N_("Check for updates..."), [](App& a) { updui::checkNow(a); })  // J-89 (R02-22: the Tracks layout has it in the app menu)
          .only([](App& a) { return updui::started() && !logicplug::active(a); }),
      md::item(N_("About Gridline"), [](App& a) { a.showAbout = true; }).only(notTracks),  // R05-18: in the app menu there
  };
  return m;
}

}  // namespace

const menudef::Menu& menudef::tracksEdit() { return tracksEditDef(); }
const menudef::Menu& menudef::clipsFile() { return clipsFileDef(); }
const menudef::Menu& menudef::clipsEdit() { return clipsEditDef(); }

const menudef::Menu& menudef::studio(int which) {
  switch (which) {
    case 0: return fileDef();
    case 1: return editDef();
    case 2: return addDef();
    case 3: return patternsDef();
    case 4: return viewDef();
    case 5: return optionsDef();
    case 6: return toolsDef();
    default: return helpDef();
  }
}

void menuFile(App& a) { md::emit(a, fileDef()); }
void menuEdit(App& a) { md::emit(a, editDef()); }
void menuAdd(App& a) { md::emit(a, addDef()); }
void menuPatterns(App& a) { md::emit(a, patternsDef()); }
void menuView(App& a) { md::emit(a, viewDef()); }
void menuOptions(App& a) { md::emit(a, optionsDef()); }
void menuTools(App& a) { md::emit(a, toolsDef()); }
void menuHelp(App& a) { md::emit(a, helpDef()); }

int settingsTabFlags(App& a, int index) {
  if (a.settingsTab != index) return 0;
  a.settingsTab = -1;
  return ImGuiTabItemFlags_SetSelected;
}

// ------------------------------------------------------------ shortcuts ----

bool menuShortcuts(App& a) {
  drawPrompt();
  // Track which editor Edit-menu commands go to.
  if (ImGuiWindow* nw = GImGui->NavWindow) {
    const char* n = ui::stableWindowName(nw->RootWindow ? nw->RootWindow->Name : nw->Name);
    if (!std::strcmp(n, "Piano roll")) a.editFocus = App::EditFocus::Piano;
    else if (!std::strcmp(n, "Playlist")) a.editFocus = App::EditFocus::Playlist;
    else if (!std::strcmp(n, "Channel rack")) a.editFocus = App::EditFocus::Rack;
    else if (!std::strcmp(n, "Mixer")) a.editFocus = App::EditFocus::Mixer;
  }
  ImGuiIO& io = ImGui::GetIO();
  ImGuiWindow* nav = GImGui->NavWindow;
  const char* focused = nav ? (nav->RootWindow ? nav->RootWindow->Name : nav->Name) : "";
  const char* focusedId = ui::stableWindowName(focused);  // J-61: titles are translated, ids are English
  const bool inPiano = !std::strcmp(focusedId, "Piano roll"), inPlaylist = !std::strcmp(focusedId, "Playlist");
  const bool inRack = !std::strcmp(focusedId, "Channel rack"), inMixerWindow = !std::strcmp(focusedId, "Mixer");
  const bool inMixer = inMixerWindow && !a.showStage;
  const bool inAuto = std::strstr(focused, "###autoedit") != nullptr;
  const bool inStage = inMixerWindow && a.showStage;  // the mixer window's Stage view
  keys::setFocusContext(inPiano ? keys::Context::PianoRoll : inPlaylist ? keys::Context::Playlist : inRack ? keys::Context::ChannelRack
                        : inMixer ? keys::Context::Mixer : inAuto ? keys::Context::Automation : inStage ? keys::Context::Stage
                        : keys::Context::Global);
  if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
  bool used = false;  // a handled key stops handleGlobalShortcuts from also acting on it
  auto key = [&used](const char* id) { bool p = keys::pressed(id); used |= p; return p; };

  // File.
  if (key("file.saveNewVersion")) a.saveNewVersion();
  if (key("file.renderStems")) exportui::openRender(a, exportui::Preset::Stems);
  if (key("file.exportMidi")) exportui::exportMidiDialog(a, 0);
  if (key("file.quit")) a.quitRequested = true;
  if (key("tools.stopSound")) stopSound(a);
  // Patterns.
  if (key("edit.rename")) renameFocused(a);
  if (key("pattern.findFirstEmpty")) findEmptyPattern(a, true, true);
  if (key("pattern.findNextEmpty")) findEmptyPattern(a, false, false);
  if (!inPiano && key("pattern.clone")) a.selectPattern(a.doc.clonePattern(a.curPattern));
  if (key("pattern.insert")) a.selectPattern(ops::insertPattern(a.doc, a.curPattern, ops::nextPatternName(a.doc.song())));
  if (key("pattern.delete")) deletePattern(a);
  if (!inPiano && !inRack && key("pattern.moveUp")) movePattern(a, -1);
  if (!inPiano && !inRack && key("pattern.moveDown")) movePattern(a, 1);
  // View / Options.
  if (key("view.commandPalette")) cmdpalette::toggle(a);  // K-93
  if (!key("view.settings")) {
  } else if (layoutui::active() == layout::Preset::Tracker) {
    // R3-03: in the Tracker, Settings is the header's Config screen (as its button), whose "All
    // settings" opens the window.
    trackerview::setHeaderSwap(trackerview::headerSwap() == 3 ? 0 : 3);
  } else {
    a.showSettings = !a.showSettings;
    if (a.showSettings) a.settingsTab = 1;  // MIDI tab, like the menu entry
  }
  if (key("view.closeAll")) closeAll(a);
  if (key("view.closePlugins")) a.closePluginWindows();
  if (key("view.hidePlugins")) {  // K-112; R02-08: in the Tracks layout V hides every plugin window (the built-in ones too)
    toggleHostedEditorsHidden();
    // K-297: Smooth's built-in effect windows too. The key was taken here, so the Smooth branch of
    // handleGlobalShortcuts never saw it: V left them all open (K-301 R02-08).
    if (logicplug::active(a)) logicplug::toggleHidden(a);
  }
  if (key("view.resetLayout")) ui::confirmLayoutReset(a);
  if (key("view.paneNextTab")) layoutui::cyclePaneTab(a, 1);  // K-237
  if (key("view.panePrevTab")) layoutui::cyclePaneTab(a, -1);
  if (key("view.reopenTab")) layoutui::reopenClosedTab(a);
  workspacesui::frameKeys(a);

  // Channel rack.
  if (inRack) {
    auto sel = selectedChannels(a);
    if (key("rack.clone")) {
      auto ids = ops::cloneChannels(a.doc, sel);
      if (!ids.empty()) a.selChannel = ids.back(), a.selChannels = std::set<ObjectId>(ids.begin(), ids.end());
    }
    if (key("rack.randomColours")) {
      a.doc.beginGroup("random colours");
      for (ObjectId c : sel) ops::setChannelsColor(a.doc, {c}, randomColor());
      a.doc.endGroup();
    }
    if (key("rack.delete")) ops::removeChannels(a.doc, sel);
    if (key("rack.moveUp")) ops::moveChannels(a.doc, sel, -1);
    if (key("rack.moveDown")) ops::moveChannels(a.doc, sel, 1);
    if (key("rack.routeFree")) chanroute::routeToFree(a, sel);  // J-195: says where they went
    if (key("rack.zip"))
      for (ObjectId c : sel) a.rack.zipped.insert(c);
    if (key("rack.unzipAll")) a.rack.zipped.clear();
    if (key("rack.rotateLeft")) ops::rotateChannel(a.doc, a.curPattern, a.selChannel, -1);
    if (key("rack.rotateRight")) ops::rotateChannel(a.doc, a.curPattern, a.selChannel, 1);
    if (key("rack.copy")) editCopy(a);
    if (key("rack.paste")) editPaste(a);
    if (key("rack.cut")) editCut(a);
    if (key("rack.selectAll")) editSelectAll(a);
  }
  // Piano roll (keys the piano roll window does not handle itself).
  if (inPiano && pianoHasTarget(a)) {
    Tick step = std::max<Tick>(1, a.doc.song().ppq / 4);
    if (key("piano.legato")) pianoEdit(a, "legato", [](auto& n, auto& s) { ops::legato(n, s); });
    if (key("piano.chopSteps")) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, step); });
    if (key("piano.glue")) pianoEdit(a, "glue notes", [](auto& n, auto& s) { ops::glue(n, s); });
    if (key("piano.duplicate")) editDuplicate(a);
    // J-11: tool windows (live preview, one undo step).
    if (key("piano.quantize")) openNoteTool(a, NoteTool::Quantize);
    if (key("piano.randomize")) openNoteTool(a, NoteTool::Randomize);
    if (key("piano.chop")) openNoteTool(a, NoteTool::Chop);
    if (key("piano.strum")) openNoteTool(a, NoteTool::Strum);
    if (key("piano.arpeggiate")) openNoteTool(a, NoteTool::Arpeggiate);
    if (key("piano.articulate")) openNoteTool(a, NoteTool::Articulate);
    if (key("piano.limit")) openNoteTool(a, NoteTool::Limit);
    if (key("piano.lfo")) openNoteTool(a, NoteTool::Lfo);
    if (key("piano.scaleLevels")) openNoteTool(a, NoteTool::ScaleLevels);
    if (key("piano.flam")) openNoteTool(a, NoteTool::Flam);
    if (key("piano.properties")) openNoteTool(a, NoteTool::Properties);
    if (key("piano.humanize")) {
      uint32_t seed = static_cast<uint32_t>(a.now * 1000.0) + 7;
      pianoEdit(a, "humanize", [&](auto& n, auto& s) { ops::humanize(n, s, 12, std::max<Tick>(1, step / 8), seed); });
    }
    if (key("piano.invertSelection")) pianoEdit(a, "", [](auto&, auto& s) { ops::invertSelection(s); });
    if (key("piano.selectRandom")) {
      uint32_t seed = static_cast<uint32_t>(a.now * 1000.0) + 3;
      pianoEdit(a, "", [&](auto& n, auto& s) { ops::selectRandom(n, s, seed); });
    }
  }
  // Playlist.
  if (inPlaylist && a.playlist.selected.size() >= 2 && key("playlist.merge") && logicplug::active(a)) {
    tracksJoin(a);  // checkpoint 47: the Join tool's rules
  } else if (inPlaylist && a.playlist.selected.size() >= 2 && key("playlist.merge")) {
    ObjectId nc = ops::mergeClips(a.doc, a.playlist.selected);
    if (nc) a.playlist.selected = {nc};
  }
  if (inPlaylist && !a.playlist.trackSel.empty()) {  // GitHub #46: the selected track headers
    const std::vector<int> sel(a.playlist.trackSel.begin(), a.playlist.trackSel.end());
    if (key("playlist.moveTrackUp")) stepPlaylistSelection(a, sel, -1);
    if (key("playlist.moveTrackDown")) stepPlaylistSelection(a, sel, 1);
  }
  // Mixer.
  if (inMixer) {
    auto chans = selectedChannels(a);
    if (key("mixer.routeHere") && !chans.empty()) ops::routeChannels(a.doc, chans, a.selMixer, false);
    if (key("mixer.routeFromHere") && !chans.empty()) ops::routeChannels(a.doc, chans, a.selMixer, true);
    if (key("mixer.moveLeft")) trackorder::stepMixerTracks(a.doc, mixerSelection(a), -1);  // GitHub #46: the selection moves
    if (key("mixer.moveRight")) trackorder::stepMixerTracks(a.doc, mixerSelection(a), 1);
    if (!a.typingKeyboard && key("mixer.solo")) ops::soloMixerTrack(a.doc, a.selMixer);
    if (key("mixer.selectAll")) mixerSelectAll(a);
    if (key("mixer.deselect")) mixerDeselect(a);
    if (key("mixer.selectRoutedChannels")) mixerSelectRoutedChannels(a, a.selMixer);
    if (key("mixer.selectRoutedTo")) mixerSelectRoutedTo(a, a.selMixer);
  }
  return used;
}

}  // namespace gl

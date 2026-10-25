// Gridline — main menu contents (File, Edit, Add, Patterns, View, Options,
// Tools, Help) and the global shortcuts behind them. The bar that hosts the
// menus lives in TopBar.cpp; context menus live in ContextMenus.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
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
#include "../core/MenuOpsExtra.h"
#include "App.h"
#include "EditTools.h"
#include "AutomationEditor.h"
#include "ContextMenus.h"
#include "ExportUi.h"
#include "FlpExportUi.h"
#include "Theme.h"
#include "UpdateUi.h"
#include "Widgets.h"
#include "Windows.h"
#include "Keys.h"
#include "ThemeSettings.h"
#include "BrowserUi.h"
#include "MenuParityUi.h"

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

void deletePattern(App& a) {
  if (a.doc.song().patterns.size() > 1) a.doc.removePattern(a.curPattern);
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

void menuFile(App& a) {
  if (ImGui::MenuItem(i18n::tr("New"), keys::label("file.new"))) a.guardUnsaved([&a]() { a.newProject(); });
  if (ImGui::BeginMenu(i18n::tr("New from template"))) {
    if (ImGui::MenuItem(i18n::tr("Browse templates..."))) a.showNewProject = true;
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Empty project"))) a.guardUnsaved([&a]() { a.newFromTemplate(""); });
    if (ImGui::MenuItem(i18n::tr("Starter kit (drums + synth)"))) a.guardUnsaved([&a]() { a.newFromTemplate("starter"); });
    if (ImGui::MenuItem(i18n::tr("Demo song"))) a.guardUnsaved([&a]() { a.newFromTemplate("demo"); });
    // Shipped and user templates by category (J-24).
    std::string cat;
    for (const auto& t : a.templates()) {
      if (t.category != cat) {
        cat = t.category;
        ImGui::SeparatorText(cat.c_str());
      }
      std::string path = t.path;
      if (ImGui::MenuItem((t.name + "##" + path).c_str())) a.guardUnsaved([&a, path]() { a.newFromTemplate(path); });
      if (!t.description.empty()) ui::Hint(t.description);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("New from loop starter..."))) j59::openLoopStarter(a);  // J-59 (GEN-01)
  ui::Hint("Build a ready-to-play loop: style, tempo, key, mood and energy, with parts you can re-roll");
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Open..."), keys::label("file.open"))) a.openDialog();
  ui::Hint("Gridline projects, FL Studio projects (.flp) and zipped projects (.zip)");  // J-52
  if (ImGui::BeginMenu(i18n::tr("Open recent"), !a.recentFiles.empty())) {
    for (const auto& r : a.recentFiles)
      if (ImGui::MenuItem((fs::path(r).filename().string() + "##" + r).c_str())) {
        std::string path = r;
        a.guardUnsaved([&a, path]() { a.openProject(path); });
      }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Clear list"))) a.recentFiles.clear();
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Revert to last backup"), nullptr, false, a.backups != nullptr)) a.revertToLastBackup();
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Save"), keys::label("file.save"))) a.save();
  if (ImGui::MenuItem(i18n::tr("Save as..."), keys::label("file.saveAs"))) a.saveAs();
  if (ImGui::MenuItem(i18n::tr("Save new version"), keys::label("file.saveNewVersion"))) a.saveNewVersion();
  if (ImGui::MenuItem(i18n::tr("Save as template..."))) a.showSaveTemplate = true;
  menuparity::fileItems(a);  // J-27
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Import"))) {
    // J-52: FL Studio projects and zipped projects open through File > Open.
    if (ImGui::MenuItem(i18n::tr("MIDI file...")))
      fileDialog(a, "Import MIDI file", false, importMidiTypes()[0].exts, "", [&a](const std::string& p) { exportui::importMidiFile(a, p); });
    if (ImGui::MenuItem(i18n::tr("FL Studio presets..."), nullptr, false, !a.presetImport.running)) {
      a.startPresetImport();
      a.showSettings = true;
      a.settingsTab = 2;  // Project page shows the progress
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Audio file into a sampler...")))
      fileDialog(a, "Import audio file", false, importAudioTypes()[0].exts, "",
                 [&a](const std::string& p) { a.addInstrument("gl.sampler", p); });
    if (ImGui::MenuItem(i18n::tr("Audio file as playlist clip...")))
      fileDialog(a, "Import audio clip", false, importAudioTypes()[0].exts, "",
                 [&a](const std::string& p) { exportui::importAudioAsClip(a, p); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Export"))) {
    ImGui::SeparatorText(i18n::tr("Audio"));
    if (ImGui::MenuItem(i18n::tr("Render to file (WAV, FLAC, MP3, Ogg)..."), keys::label("file.render"))) exportui::openRender(a, exportui::Preset::Audio);
    if (ImGui::MenuItem(i18n::tr("Mixer tracks (stems)..."), keys::label("file.renderStems"))) exportui::openRender(a, exportui::Preset::Stems);
    if (ImGui::MenuItem(i18n::tr("All playlist tracks..."))) exportui::openRender(a, exportui::Preset::PlaylistTracks);
    if (ImGui::MenuItem(i18n::tr("All playlist track groups..."))) exportui::openRender(a, exportui::Preset::TrackGroups);  // J-31
    menuparity::exportItems(a);  // J-27
    ImGui::SeparatorText(i18n::tr("MIDI"));
    if (ImGui::MenuItem(i18n::tr("Song as MIDI file..."), keys::label("file.exportMidi"))) exportui::exportMidiDialog(a, 0);
    if (ImGui::MenuItem(i18n::tr("Pattern as MIDI file..."))) exportui::exportMidiDialog(a, 1);
    if (ImGui::MenuItem(i18n::tr("Time selection as MIDI file..."), nullptr, false, a.playlist.selEnd > a.playlist.selStart)) exportui::exportMidiDialog(a, 2);
    ImGui::SeparatorText(i18n::tr("Project"));
    if (ImGui::MenuItem(i18n::tr("Project data files..."))) exportui::exportDataFilesDialog(a);
    if (ImGui::MenuItem(i18n::tr("Project bones..."))) exportui::exportBonesDialog(a);
    if (ImGui::MenuItem(i18n::tr("Gridline zipped project (.gridline.zip)..."))) exportui::exportZipDialog(a);
    if (ImGui::MenuItem(i18n::tr("FL Studio project (.flp)..."))) exportui::exportFlpDialog(a);  // J-34
    if (ImGui::MenuItem(i18n::tr("FL Studio zipped loop package (.zip)..."))) exportui::exportFlpZipDialog(a);  // J-52
    if (ImGui::MenuItem(i18n::tr("FL Studio export report..."), nullptr, false, flpexport::hasReport())) flpexport::openReport();  // J-80
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Locate missing samples..."))) exportui::openMissingSamples(a);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Project info..."), keys::label("file.projectInfo"))) a.showInfo = true;
  if (ImGui::MenuItem(i18n::tr("Plugin manager..."))) a.showPluginManager = true;
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Quit"), keys::label("file.quit"))) a.quitRequested = true;
}

void menuEdit(App& a) {
  const auto& h = a.doc.history();
  std::string u = h.canUndo() ? "Undo " + h.undoLabel() : "Undo", r = h.canRedo() ? "Redo " + h.redoLabel() : "Redo";
  if (ImGui::MenuItem(u.c_str(), keys::label("edit.undo"), false, h.canUndo())) a.doc.undo();
  if (ImGui::MenuItem(r.c_str(), keys::label("edit.redo"), false, h.canRedo())) a.doc.redo();
  if (ImGui::MenuItem(i18n::tr("Undo history"), keys::label("edit.history"), a.showHistory)) a.showHistory = !a.showHistory;
  ImGui::Separator();
  std::string target = std::string("Acts on: ") + editTargetName(a) + " (" +
                       (a.editFocus == App::EditFocus::Playlist ? "playlist"
                        : a.editFocus == App::EditFocus::Rack   ? "channel rack"
                        : a.editFocus == App::EditFocus::Mixer  ? "mixer"
                                                                : "piano roll") + ")";
  ImGui::TextDisabled("%s", target.c_str());
  // Shortcut column: the focused editor's own binding for the action.
  const char* ctx = a.editFocus == App::EditFocus::Playlist ? "playlist." : a.editFocus == App::EditFocus::Rack ? "rack."
                    : a.editFocus == App::EditFocus::Mixer ? "mixer." : "piano.";
  auto ek = [ctx](const char* op) { return keys::label((std::string(ctx) + op).c_str()); };
  if (ImGui::MenuItem(i18n::tr("Cut"), ek("cut"))) editCut(a);
  if (ImGui::MenuItem(i18n::tr("Copy"), ek("copy"))) editCopy(a);
  if (ImGui::MenuItem(i18n::tr("Paste"), ek("paste"), false, canPaste(a))) editPaste(a);
  if (ImGui::MenuItem(i18n::tr("Duplicate"), ek("duplicate"), false, a.editFocus != App::EditFocus::Mixer)) editDuplicate(a);
  if (ImGui::MenuItem(i18n::tr("Delete"), ek("delete"))) editDelete(a);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Select all"), ek("selectAll"), false, a.editFocus != App::EditFocus::Mixer)) editSelectAll(a);
  if (ImGui::MenuItem(i18n::tr("Deselect"), ek("deselect"), false, a.editFocus != App::EditFocus::Mixer)) editDeselect(a);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Rename..."), keys::label("edit.rename"))) renameFocused(a);
}

void menuAdd(App& a) {
  if (ImGui::BeginMenu(i18n::tr("Instrument channel"))) {
    addChannelMenu(a);
    ImGui::EndMenu();
  }
  const Song& s = a.doc.song();
  const MixerTrack* mt = s.mixerTrack(a.selMixer);
  std::string fxLabel = "Effect on " + (mt ? mixerTrackName(s, static_cast<size_t>(s.mixerIndex(mt->id))) : std::string("mixer track"));
  if (ImGui::BeginMenu(fxLabel.c_str(), mt != nullptr)) {
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
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Pattern"), keys::label("pattern.new"))) newPattern(a);
  if (ImGui::MenuItem(i18n::tr("Mixer track"))) {
    ObjectId id = a.doc.addMixerTrack();
    if (id != kNoId) a.selMixer = id;
  }
  if (ImGui::MenuItem(i18n::tr("8 playlist tracks"), nullptr, false, static_cast<int>(s.tracks.size()) + 8 <= kMaxPlaylistTracks))
    a.doc.setPlaylistTrackCount(static_cast<int>(s.tracks.size()) + 8);
  if (ImGui::MenuItem(i18n::tr("Clip of the current pattern at the song end"))) {
    int track = 0;
    for (const auto& c : s.playlist)
      if (c.pattern == a.curPattern) track = c.track;
    Tick bar = std::max<Tick>(1, s.barTicks());
    Tick at = (s.lengthTicks() + bar - 1) / bar * bar;
    ObjectId id = a.doc.addClip(a.curPattern, track, at);
    if (id) a.playlist.selected = {id};
  }
  menuparity::addItems(a);  // J-27
}

void menuPatterns(App& a) {
  const Song& s = a.doc.song();
  const Pattern* cur = s.pattern(a.curPattern);
  ObjectId id = a.curPattern;
  if (ImGui::MenuItem(i18n::tr("New pattern"), keys::label("pattern.new"))) newPattern(a);
  if (ImGui::MenuItem(i18n::tr("Find first empty"), keys::label("pattern.findFirstEmpty"))) findEmptyPattern(a, true, true);
  if (ImGui::MenuItem(i18n::tr("Find next empty"), keys::label("pattern.findNextEmpty"))) findEmptyPattern(a, false, false);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Rename..."), keys::label("edit.rename"), false, cur != nullptr))
    openPrompt("Rename pattern", cur->name, [&a, id](const std::string& n) { a.doc.renamePattern(id, n); });
  if (ImGui::BeginMenu(i18n::tr("Colour"), cur != nullptr)) {
    uint32_t col;
    if (colorGrid(&col)) ops::setPatternColor(a.doc, id, col);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Random colour"), nullptr, false, cur != nullptr)) ops::setPatternColor(a.doc, id, randomColor());
  if (ImGui::MenuItem(i18n::tr("Select in playlist"), nullptr, false, cur != nullptr)) {
    a.playlist.selected.clear();
    for (const auto& c : s.playlist)
      if (c.pattern == id) a.playlist.selected.insert(c.id);
    a.showPlaylist = true;
  }
  if (ImGui::BeginMenu(i18n::tr("Transpose"), cur != nullptr)) {
    for (int st : {12, 1, -1, -12}) {
      std::string l = (st > 0 ? "+" : "") + std::to_string(st) + (std::abs(st) == 12 ? " (octave)" : " semitone");
      if (ImGui::MenuItem(l.c_str())) ops::transposePattern(a.doc, id, st);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Length"), cur != nullptr)) {
    for (int b : {1, 2, 4, 8, 16}) {
      std::string l = std::to_string(b) + (b == 1 ? " bar" : " bars");
      if (ImGui::MenuItem(l.c_str(), nullptr, cur->bars == b)) a.doc.setPatternLength(id, b, cur->stepsPerBar);
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Insert one"), keys::label("pattern.insert"))) a.selectPattern(ops::insertPattern(a.doc, id, ops::nextPatternName(s)));
  if (ImGui::MenuItem(i18n::tr("Clone"), keys::label("pattern.clone"), false, cur != nullptr)) a.selectPattern(a.doc.clonePattern(id));
  if (ImGui::MenuItem(i18n::tr("Delete"), keys::label("pattern.delete"), false, s.patterns.size() > 1)) deletePattern(a);
  j31::patternsItems(a);  // J-31: pattern groups
  int idx = s.patternIndex(id);
  if (ImGui::MenuItem(i18n::tr("Move up"), keys::label("pattern.moveUp"), false, idx > 0)) movePattern(a, -1);
  if (ImGui::MenuItem(i18n::tr("Move down"), keys::label("pattern.moveDown"), false, idx >= 0 && idx + 1 < static_cast<int>(s.patterns.size()))) movePattern(a, 1);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Split by channel"), nullptr, false, cur != nullptr)) {
    auto ids = ops::splitPatternByChannel(a.doc, id);
    a.notify(ids.empty() ? std::string("Nothing to split: the pattern is empty") : "Split into " + std::to_string(ids.size()) + " patterns");
  }
  if (ImGui::MenuItem(i18n::tr("Delete unused patterns"))) {
    int n = ops::deleteUnusedPatterns(a.doc);
    a.notify(n ? "Deleted " + std::to_string(n) + " unused pattern(s)" : std::string("No unused patterns"));
  }
  if (ImGui::MenuItem(i18n::tr("Render pattern to file..."), nullptr, false, cur != nullptr)) exportui::openRender(a, exportui::Preset::PatternFile);
  if (ImGui::MenuItem(i18n::tr("Render as audio clip..."), nullptr, false, cur != nullptr)) exportui::openRender(a, exportui::Preset::AudioClip);
  if (ImGui::MenuItem(i18n::tr("Render and replace..."), nullptr, false, cur != nullptr)) exportui::openRender(a, exportui::Preset::PatternReplace);
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Go to"))) {
    for (const auto& p : s.patterns) {
      std::string label = p.name + "##" + std::to_string(p.id);
      if (ImGui::MenuItem(label.c_str(), nullptr, p.id == a.curPattern)) a.selectPattern(p.id);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Next pattern"), keys::label("pattern.next"), false, idx + 1 < static_cast<int>(s.patterns.size())))
    a.selectPattern(s.patterns[static_cast<size_t>(idx + 1)].id);
  if (ImGui::MenuItem(i18n::tr("Previous pattern"), keys::label("pattern.previous"), false, idx > 0)) a.selectPattern(s.patterns[static_cast<size_t>(idx - 1)].id);
}

void menuView(App& a) {
  ImGui::SeparatorText(i18n::tr("Windows"));
  ImGui::MenuItem(i18n::tr("Playlist"), keys::label("view.playlist"), &a.showPlaylist);
  ImGui::MenuItem(i18n::tr("Channel rack"), keys::label("view.channelRack"), &a.showRack);
  ImGui::MenuItem(i18n::tr("Piano roll"), keys::label("view.pianoRoll"), &a.showPiano);
  ImGui::MenuItem(i18n::tr("Browser"), keys::label("view.browser"), &a.showBrowser);
  if (ImGui::MenuItem(i18n::tr("Plugin picker"), keys::label("view.pluginPicker"), browserui::pluginPickerOpen())) browserui::togglePluginPicker();
  menuparity::viewItems(a);  // J-27
  ImGui::MenuItem(i18n::tr("Mixer"), keys::label("view.mixer"), &a.showMixer);
  ImGui::MenuItem(i18n::tr("Undo history"), keys::label("edit.history"), &a.showHistory);
  ImGui::MenuItem(i18n::tr("Live pads"), nullptr, &a.showLivePads);
  ImGui::MenuItem(i18n::tr("Master chain"), nullptr, &a.showMaster);  // RN-09
  ImGui::MenuItem(i18n::tr("Performance"), nullptr, &a.showPerformance);
  if (ImGui::MenuItem(i18n::tr("Channel settings"), nullptr, a.settingsChannel != kNoId, a.doc.song().channel(a.selChannel) != nullptr))
    a.settingsChannel = a.settingsChannel != kNoId ? kNoId : a.selChannel;
  // Zoom acts on the piano roll when it was the last editor used, otherwise on the playlist.
  const bool piano = a.editFocus == App::EditFocus::Piano;
  ImGui::SeparatorText(piano ? i18n::tr("Zoom (piano roll)") : i18n::tr("Zoom (playlist)"));
  float& zr = piano ? a.piano.zoomRequest : a.playlist.zoomRequest;
  if (ImGui::MenuItem(i18n::tr("Zoom in"), keys::label(piano ? "piano.zoomIn" : "playlist.zoomIn"))) zr *= 1.5f;
  if (ImGui::MenuItem(i18n::tr("Zoom out"), keys::label(piano ? "piano.zoomOut" : "playlist.zoomOut"))) zr /= 1.5f;
  if (ImGui::MenuItem(piano ? i18n::tr("Zoom to fit the pattern") : i18n::tr("Zoom to fit the song"), keys::label(piano ? "piano.zoomFit" : "playlist.zoomFit"))) (piano ? a.piano.fitRequest : a.playlist.fitRequest) = true;
  if (ImGui::MenuItem(i18n::tr("Zoom to ticks (snap turns free)"))) zr = 1e9f;
  if (!piano && ImGui::MenuItem(i18n::tr("Clear time selection"), nullptr, false, a.playlist.selEnd > a.playlist.selStart))
    a.playlist.selStart = a.playlist.selEnd = 0;
  ImGui::SeparatorText(i18n::tr("Layout"));
  if (ImGui::MenuItem(i18n::tr("Close all windows"), keys::label("view.closeAll"))) a.closeAllWindows();
  if (ImGui::MenuItem(i18n::tr("Close plugin windows"), keys::label("view.closePlugins"))) a.closePluginWindows();
  if (ImGui::MenuItem(i18n::tr("Reset window layout"), keys::label("view.resetLayout"))) ui::confirmLayoutReset(a);
  ImGui::SeparatorText(i18n::tr("Appearance"));
  bool dark = theme::isDark();
  if (ImGui::MenuItem(i18n::tr("Dark theme"), nullptr, dark)) {
    const dispconf::Snapshot before = dispconf::take(a.settings, dispconf::themeKeys());
    theme::modeToSettings(dark ? theme::Mode::Light : theme::Mode::Dark, a.settings);
    theme::applyFromSettings(a.settings);
    ui::confirmTheme(a, before, "Theme");
  }
  if (ImGui::MenuItem(i18n::tr("Theme settings..."))) openSettings(a, 9);
  if (ImGui::BeginMenu(i18n::tr("Mixer layout"))) {
    const int cur = static_cast<int>(a.settings.getNumber("ui.mixerLayout"));
    const char* names[] = {i18n::N_("Compact strips"), i18n::N_("Normal strips"), i18n::N_("Wide strips")};
    for (int i = 0; i < 3; ++i)
      if (ImGui::MenuItem(i18n::tr(names[i]), nullptr, cur == i)) a.settings.set("ui.mixerLayout", i);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Interface size"))) {
    double cur = a.settings.getNumber("ui.scale");
    for (int pct : {75, 90, 100, 110, 125, 150, 175, 200}) {
      std::string l = std::to_string(pct) + "%";
      if (ImGui::MenuItem(l.c_str(), nullptr, std::fabs(cur * 100 - pct) < 0.5)) ui::confirmScale(a, pct / 100.0);  // J-77: Keep / Revert
    }
    ImGui::EndMenu();
  }
}

void menuOptions(App& a) {
  ImGui::SeparatorText(i18n::tr("Settings"));
  if (ImGui::MenuItem(i18n::tr("Audio settings..."))) openSettings(a, 0);
  if (ImGui::MenuItem(i18n::tr("MIDI settings..."), keys::label("view.settings"))) openSettings(a, 1);
  if (ImGui::MenuItem(i18n::tr("Project settings..."))) openSettings(a, 2);
  if (ImGui::MenuItem(i18n::tr("Backup settings..."))) openSettings(a, 3);
  if (ImGui::MenuItem(i18n::tr("Interface settings..."))) openSettings(a, 4);
  if (ImGui::MenuItem(i18n::tr("General settings..."))) openSettings(a, 5);
  if (ImGui::MenuItem(i18n::tr("File settings..."))) openSettings(a, 6);
  if (ImGui::MenuItem(i18n::tr("Debug log and script console..."))) openSettings(a, 7);
  if (ImGui::MenuItem(i18n::tr("Keyboard shortcuts..."))) openSettings(a, 8);
  if (ImGui::MenuItem(i18n::tr("Theme..."))) openSettings(a, 9);
  if (ImGui::MenuItem(i18n::tr("Project info..."), keys::label("file.projectInfo"))) a.showInfo = true;
  ImGui::SeparatorText(i18n::tr("Switches"));
  ImGui::MenuItem(i18n::tr("Metronome"), keys::label("options.metronome"), &a.metronome);
  ImGui::MenuItem(i18n::tr("Typing keyboard to piano"), keys::label("options.typingKeyboard"), &a.typingKeyboard);
  ImGui::MenuItem(i18n::tr("Record notes"), nullptr, &a.recordArmed);
  if (ImGui::BeginMenu(i18n::tr("Recording options"))) {
    recordOptionsMenu(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Typing keyboard options"))) {
    typingKeyboardMenu(a);
    ImGui::EndMenu();
  }
  ImGui::MenuItem(i18n::tr("Step record"), nullptr, &a.stepRecord);
  ImGui::MenuItem(i18n::tr("Multilink to controllers"), nullptr, &a.multilink);
  menuparity::optionsItems(a);  // J-27
  j31::optionsItems(a);         // J-31: quantized jumps, recording options
  {
    const bool pdc = a.settings.getBool("audio.pdc");
    if (ImGui::MenuItem(i18n::tr("Plugin delay compensation"), nullptr, pdc)) {
      a.settings.set("audio.pdc", !pdc);
      a.syncEngine();
    }
    ui::Hint("Delays faster paths so tracks with latent effects (look-ahead, linear phase, oversampling) stay in time");
  }
  ImGui::MenuItem(i18n::tr("Record automation"), nullptr, &automationRecording());
  {
    const bool follow = a.settings.getBool("ui.autoScroll");
    if (ImGui::MenuItem(i18n::tr("Auto scrolling"), nullptr, follow)) a.settings.set("ui.autoScroll", !follow);
    ui::Hint("The playlist follows the song position while playing");
  }
  {  // J-37
    const bool smoothSnap = a.settings.getBool("ui.smoothSnap");
    if (ImGui::MenuItem(i18n::tr("Smooth snapping"), nullptr, smoothSnap)) a.settings.set("ui.smoothSnap", !smoothSnap);
    ui::Hint("Clips and notes dragged on a snapped grid glide from cell to cell instead of jumping (positions still land exactly on the grid)");
  }
  bool song = a.engine.mode() == PlayMode::Song;
  if (ImGui::MenuItem(i18n::tr("Song mode"), "L", song)) a.engine.setMode(song ? PlayMode::Pattern : PlayMode::Song);
  bool liveOn = a.engine.mode() == PlayMode::Live;
  if (ImGui::MenuItem(i18n::tr("Live mode"), nullptr, liveOn)) a.engine.setMode(liveOn ? PlayMode::Song : PlayMode::Live);
  if (ImGui::BeginMenu(i18n::tr("Arrangements"))) {
    arrangementMenu(a);
    ImGui::EndMenu();
  }
}

void menuTools(App& a) {
  if (ImGui::BeginMenu(i18n::tr("Piano roll tools"))) {
    pianoToolItems(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Piano roll selection"))) {
    pianoSelectItems(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Chord progression"))) {
    progressionItems(a, false);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Riff from progression"))) {
    progressionItems(a, true);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Tempo tapper"))) {
    static ops::TapTempo tapper;
    ImGui::TextDisabled("%s", i18n::tr("Click Tap (or press T) on every beat"));
    bool tap = ImGui::Button(i18n::tr("Tap"), ImVec2(160, 36));
    tap = tap || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_T, false));
    if (tap) tapper.tap(ImGui::GetTime());
    const double bpm = tapper.bpm();
    if (bpm > 0) ImGui::Text(i18n::tr("%.1f BPM  (%d taps)"), bpm, static_cast<int>(tapper.taps.size()));
    else ImGui::TextDisabled("—");
    if (ImGui::Button(i18n::tr("Set the song tempo")) && bpm > 0) a.doc.setParam(songParam(SongParam::Tempo), std::round(bpm * 10.0) / 10.0, false);
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("Reset"))) tapper.reset();
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Loop starter..."))) j59::openLoopStarter(a);  // J-59 (GEN-01)
  if (ImGui::MenuItem(i18n::tr("Render channel to sampled instrument..."), nullptr, false, a.doc.song().channel(a.selChannel) != nullptr))
    j59::openRenderSampler(a, a.selChannel);  // J-59 (RN-08)
  if (ImGui::BeginMenu(i18n::tr("Project scripts"))) {  // J-57: Lua scripts over the whole project (SC-02)
    projectScriptsMenuItems(a);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("AI assistant activity"))) mcpui::showActivity();  // SC-07
  ImGui::SeparatorText(i18n::tr("Macros"));
  if (ImGui::MenuItem(i18n::tr("Stop all sound"), keys::label("tools.stopSound"))) stopSound(a);
  if (ImGui::MenuItem(i18n::tr("Route channels to free mixer tracks"), keys::label("rack.routeFree"))) {
    auto t = ops::routeToFreeMixerTracks(a.doc, selectedChannels(a));
    if (!t.empty()) a.notify("Routed to " + std::to_string(t.size()) + " mixer track(s)");
  }
  if (ImGui::MenuItem(i18n::tr("Route every channel to its own mixer track"))) {
    std::vector<ObjectId> all;
    for (const auto& c : a.doc.song().channels)
      if (c.mixerTrack == a.doc.song().masterId()) all.push_back(c.id);
    auto t = ops::routeToFreeMixerTracks(a.doc, all);
    a.notify(t.empty() ? std::string("Every channel already has a mixer track") : "Routed " + std::to_string(t.size()) + " channel(s)");
  }
  if (ImGui::MenuItem(i18n::tr("Select unused channels"))) {
    auto u = ops::unusedChannels(a.doc.song());
    if (u.empty()) {
      a.notify("Every channel is used in a pattern");
    } else {
      a.selChannels = std::set<ObjectId>(u.begin(), u.end());
      a.selChannel = u.front();
      a.showRack = true;
    }
  }
  if (ImGui::MenuItem(i18n::tr("Delete unused patterns"))) {
    int n = ops::deleteUnusedPatterns(a.doc);
    a.notify(n ? "Deleted " + std::to_string(n) + " unused pattern(s)" : std::string("No unused patterns"));
  }
  if (ImGui::MenuItem(i18n::tr("Trim unused mixer tracks"))) {
    int n = ops::trimUnusedMixerTracks(a.doc);
    a.notify(n ? "Removed " + std::to_string(n) + " unused mixer track(s)" : std::string("No unused mixer tracks at the end"));
  }
  if (ImGui::MenuItem(i18n::tr("Unmute all channels"))) {
    std::vector<ObjectId> all;
    for (const auto& c : a.doc.song().channels) all.push_back(c.id);
    ops::setChannelsMuted(a.doc, all, false);
  }
  if (ImGui::MenuItem(i18n::tr("Unsolo all mixer tracks"))) a.doc.editSong("unsolo all", [](Song& s) {
      for (auto& m : s.mixer) m.solo = false;
    });
  if (ImGui::MenuItem(i18n::tr("Close plugin windows"), keys::label("view.closePlugins"))) a.closePluginWindows();
  menuparity::toolsItems(a);  // J-27
  j31::toolsItems(a);         // J-31: audio logger, cancel recording
}

void menuHelp(App& a) {
  if (ImGui::MenuItem(i18n::tr("Keyboard shortcuts"), keys::label("help.shortcuts"))) a.showShortcuts = true;
  if (ImGui::MenuItem(i18n::tr("Load demo song"))) a.guardUnsaved([&a]() { a.newFromTemplate("demo"); });
  if (ImGui::MenuItem(i18n::tr("Show settings folder"))) a.notify("Settings, presets and templates: " + a.configDir());
  ImGui::Separator();
  if (updui::started() && ImGui::MenuItem(i18n::tr("Check for updates..."))) updui::checkNow(a);  // J-89
  if (ImGui::MenuItem(i18n::tr("About Gridline"))) a.showAbout = true;
}

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
  const bool inRack = !std::strcmp(focusedId, "Channel rack"), inMixer = !std::strcmp(focusedId, "Mixer");
  const bool inAuto = std::strstr(focused, "###autoedit") != nullptr;
  keys::setFocusContext(inPiano ? keys::Context::PianoRoll : inPlaylist ? keys::Context::Playlist : inRack ? keys::Context::ChannelRack
                        : inMixer ? keys::Context::Mixer : inAuto ? keys::Context::Automation : keys::Context::Global);
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
  if (key("view.settings")) {
    a.showSettings = !a.showSettings;
    if (a.showSettings) a.settingsTab = 1;  // MIDI tab, like the menu entry
  }
  if (key("view.closeAll")) a.closeAllWindows();
  if (key("view.closePlugins")) a.closePluginWindows();
  if (key("view.resetLayout")) ui::confirmLayoutReset(a);

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
    if (key("rack.routeFree")) {
      auto t = ops::routeToFreeMixerTracks(a.doc, sel);
      if (!t.empty()) a.selMixer = t.back();
    }
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
  if (inPlaylist && a.playlist.selected.size() >= 2 && key("playlist.merge")) {
    ObjectId nc = ops::mergeClips(a.doc, a.playlist.selected);
    if (nc) a.playlist.selected = {nc};
  }
  // Mixer.
  if (inMixer) {
    auto chans = selectedChannels(a);
    if (key("mixer.routeHere") && !chans.empty()) ops::routeChannels(a.doc, chans, a.selMixer, false);
    if (key("mixer.routeFromHere") && !chans.empty()) ops::routeChannels(a.doc, chans, a.selMixer, true);
    if (key("mixer.moveLeft")) ops::moveMixerTrack(a.doc, a.selMixer, -1);
    if (key("mixer.moveRight")) ops::moveMixerTrack(a.doc, a.selMixer, 1);
    if (!a.typingKeyboard && key("mixer.solo")) ops::soloMixerTrack(a.doc, a.selMixer);
  }
  return used;
}

}  // namespace gl

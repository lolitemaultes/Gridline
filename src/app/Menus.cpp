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
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
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

void stopSound(App& a) {
  a.engine.panic();
  a.notify("All sound stopped");
}

}  // namespace

// ---------------------------------------------------------------- menus ----

void menuFile(App& a) {
  if (ImGui::MenuItem("New", "Ctrl+N")) a.guardUnsaved([&a]() { a.newProject(); });
  if (ImGui::BeginMenu("New from template")) {
    if (ImGui::MenuItem("Browse templates...")) a.showNewProject = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Empty project")) a.guardUnsaved([&a]() { a.newFromTemplate(""); });
    if (ImGui::MenuItem("Starter kit (drums + synth)")) a.guardUnsaved([&a]() { a.newFromTemplate("starter"); });
    if (ImGui::MenuItem("Demo song")) a.guardUnsaved([&a]() { a.newFromTemplate("demo"); });
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
  ImGui::Separator();
  if (ImGui::MenuItem("Open...", "Ctrl+O")) a.openDialog();
  if (ImGui::BeginMenu("Open recent", !a.recentFiles.empty())) {
    for (const auto& r : a.recentFiles)
      if (ImGui::MenuItem((fs::path(r).filename().string() + "##" + r).c_str())) {
        std::string path = r;
        a.guardUnsaved([&a, path]() { a.openProject(path); });
      }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear list")) a.recentFiles.clear();
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Revert to last backup", nullptr, false, a.backups != nullptr)) a.revertToLastBackup();
  ImGui::Separator();
  if (ImGui::MenuItem("Save", "Ctrl+S")) a.save();
  if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S")) a.saveAs();
  if (ImGui::MenuItem("Save new version", "Ctrl+Alt+S")) a.saveNewVersion();
  if (ImGui::MenuItem("Save as template...")) a.showSaveTemplate = true;
  menuparity::fileItems(a);  // J-27
  ImGui::Separator();
  if (ImGui::BeginMenu("Import")) {
    if (ImGui::MenuItem("MIDI file..."))
      fileDialog(a, "Import MIDI file", false, {".mid", ".midi", ".rmi"}, "", [&a](const std::string& p) { exportui::importMidiFile(a, p); });
    if (ImGui::MenuItem("FL Studio project (.flp)..."))
      fileDialog(a, "Import FL Studio project", false, {".flp"}, "", [&a](const std::string& p) {
        a.guardUnsaved([&a, p]() { a.importFlpProject(p); });
      });
    if (ImGui::MenuItem("Zipped project (.zip)..."))
      fileDialog(a, "Open zipped project", false, {".zip"}, "", [&a](const std::string& p) {
        a.guardUnsaved([&a, p]() { exportui::openAny(a, p); });
      });
    if (ImGui::MenuItem("FL Studio presets...", nullptr, false, !a.presetImport.running)) {
      a.startPresetImport();
      a.showSettings = true;
      a.settingsTab = 2;  // Project page shows the progress
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Audio file into a sampler..."))
      fileDialog(a, "Import audio file", false, {".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff", ".wv"}, "",
                 [&a](const std::string& p) { a.addInstrument("gl.sampler", p); });
    if (ImGui::MenuItem("Audio file as playlist clip..."))
      fileDialog(a, "Import audio clip", false, {".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff", ".wv"}, "",
                 [&a](const std::string& p) { exportui::importAudioAsClip(a, p); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Export")) {
    ImGui::SeparatorText("Audio");
    if (ImGui::MenuItem("Render to file (WAV, FLAC, MP3, Ogg)...", "Ctrl+R")) exportui::openRender(a, exportui::Preset::Audio);
    if (ImGui::MenuItem("Mixer tracks (stems)...", "Ctrl+Shift+R")) exportui::openRender(a, exportui::Preset::Stems);
    if (ImGui::MenuItem("All playlist tracks...")) exportui::openRender(a, exportui::Preset::PlaylistTracks);
    if (ImGui::MenuItem("All playlist track groups...")) exportui::openRender(a, exportui::Preset::TrackGroups);  // J-31
    menuparity::exportItems(a);  // J-27
    ImGui::SeparatorText("MIDI");
    if (ImGui::MenuItem("Song as MIDI file...", "Ctrl+Shift+M")) exportui::exportMidiDialog(a, 0);
    if (ImGui::MenuItem("Pattern as MIDI file...")) exportui::exportMidiDialog(a, 1);
    if (ImGui::MenuItem("Time selection as MIDI file...", nullptr, false, a.playlist.selEnd > a.playlist.selStart)) exportui::exportMidiDialog(a, 2);
    ImGui::SeparatorText("Project");
    if (ImGui::MenuItem("Project data files...")) exportui::exportDataFilesDialog(a);
    if (ImGui::MenuItem("Project bones...")) exportui::exportBonesDialog(a);
    if (ImGui::MenuItem("Zipped project...")) exportui::exportZipDialog(a);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Locate missing samples...")) exportui::openMissingSamples(a);
  ImGui::Separator();
  if (ImGui::MenuItem("Project info...", "F11")) a.showInfo = true;
  if (ImGui::MenuItem("Plugin manager...")) a.showPluginManager = true;
  ImGui::Separator();
  if (ImGui::MenuItem("Quit", "Ctrl+Q")) a.quitRequested = true;
}

void menuEdit(App& a) {
  const auto& h = a.doc.history();
  std::string u = h.canUndo() ? "Undo " + h.undoLabel() : "Undo", r = h.canRedo() ? "Redo " + h.redoLabel() : "Redo";
  if (ImGui::MenuItem(u.c_str(), "Ctrl+Z", false, h.canUndo())) a.doc.undo();
  if (ImGui::MenuItem(r.c_str(), "Ctrl+Y", false, h.canRedo())) a.doc.redo();
  if (ImGui::MenuItem("Undo history", "Alt+Z", a.showHistory)) a.showHistory = !a.showHistory;
  ImGui::Separator();
  std::string target = std::string("Acts on: ") + editTargetName(a) + " (" +
                       (a.editFocus == App::EditFocus::Playlist ? "playlist"
                        : a.editFocus == App::EditFocus::Rack   ? "channel rack"
                        : a.editFocus == App::EditFocus::Mixer  ? "mixer"
                                                                : "piano roll") + ")";
  ImGui::TextDisabled("%s", target.c_str());
  if (ImGui::MenuItem("Cut", "Ctrl+X")) editCut(a);
  if (ImGui::MenuItem("Copy", "Ctrl+C")) editCopy(a);
  if (ImGui::MenuItem("Paste", "Ctrl+V", false, canPaste(a))) editPaste(a);
  if (ImGui::MenuItem("Duplicate", "Ctrl+B", false, a.editFocus != App::EditFocus::Mixer)) editDuplicate(a);
  if (ImGui::MenuItem("Delete", "Del")) editDelete(a);
  ImGui::Separator();
  if (ImGui::MenuItem("Select all", "Ctrl+A", false, a.editFocus != App::EditFocus::Mixer)) editSelectAll(a);
  if (ImGui::MenuItem("Deselect", "Ctrl+D", false, a.editFocus != App::EditFocus::Mixer)) editDeselect(a);
  ImGui::Separator();
  if (ImGui::MenuItem("Rename...", "F2")) renameFocused(a);
}

void menuAdd(App& a) {
  if (ImGui::BeginMenu("Instrument channel")) {
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
  if (ImGui::MenuItem("Pattern", "F4")) newPattern(a);
  if (ImGui::MenuItem("Mixer track")) {
    ObjectId id = a.doc.addMixerTrack();
    if (id != kNoId) a.selMixer = id;
  }
  if (ImGui::MenuItem("8 playlist tracks", nullptr, false, static_cast<int>(s.tracks.size()) + 8 <= kMaxPlaylistTracks))
    a.doc.setPlaylistTrackCount(static_cast<int>(s.tracks.size()) + 8);
  if (ImGui::MenuItem("Clip of the current pattern at the song end")) {
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
  if (ImGui::MenuItem("New pattern", "F4")) newPattern(a);
  if (ImGui::MenuItem("Find first empty", "Shift+F4")) findEmptyPattern(a, true, true);
  if (ImGui::MenuItem("Find next empty", "Ctrl+F4")) findEmptyPattern(a, false, false);
  ImGui::Separator();
  if (ImGui::MenuItem("Rename...", "F2", false, cur != nullptr))
    openPrompt("Rename pattern", cur->name, [&a, id](const std::string& n) { a.doc.renamePattern(id, n); });
  if (ImGui::BeginMenu("Colour", cur != nullptr)) {
    uint32_t col;
    if (colorGrid(&col)) ops::setPatternColor(a.doc, id, col);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Random colour", nullptr, false, cur != nullptr)) ops::setPatternColor(a.doc, id, randomColor());
  if (ImGui::MenuItem("Select in playlist", nullptr, false, cur != nullptr)) {
    a.playlist.selected.clear();
    for (const auto& c : s.playlist)
      if (c.pattern == id) a.playlist.selected.insert(c.id);
    a.showPlaylist = true;
  }
  if (ImGui::BeginMenu("Transpose", cur != nullptr)) {
    for (int st : {12, 1, -1, -12}) {
      std::string l = (st > 0 ? "+" : "") + std::to_string(st) + (std::abs(st) == 12 ? " (octave)" : " semitone");
      if (ImGui::MenuItem(l.c_str())) ops::transposePattern(a.doc, id, st);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Length", cur != nullptr)) {
    for (int b : {1, 2, 4, 8, 16}) {
      std::string l = std::to_string(b) + (b == 1 ? " bar" : " bars");
      if (ImGui::MenuItem(l.c_str(), nullptr, cur->bars == b)) a.doc.setPatternLength(id, b, cur->stepsPerBar);
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Insert one", "Ctrl+Shift+Ins")) a.selectPattern(ops::insertPattern(a.doc, id, ops::nextPatternName(s)));
  if (ImGui::MenuItem("Clone", "Ctrl+Shift+C", false, cur != nullptr)) a.selectPattern(a.doc.clonePattern(id));
  if (ImGui::MenuItem("Delete", "Ctrl+Shift+Del", false, s.patterns.size() > 1)) deletePattern(a);
  j31::patternsItems(a);  // J-31: pattern groups
  int idx = s.patternIndex(id);
  if (ImGui::MenuItem("Move up", "Ctrl+Shift+Up", false, idx > 0)) movePattern(a, -1);
  if (ImGui::MenuItem("Move down", "Ctrl+Shift+Down", false, idx >= 0 && idx + 1 < static_cast<int>(s.patterns.size()))) movePattern(a, 1);
  ImGui::Separator();
  if (ImGui::MenuItem("Split by channel", nullptr, false, cur != nullptr)) {
    auto ids = ops::splitPatternByChannel(a.doc, id);
    a.notify(ids.empty() ? std::string("Nothing to split: the pattern is empty") : "Split into " + std::to_string(ids.size()) + " patterns");
  }
  if (ImGui::MenuItem("Delete unused patterns")) {
    int n = ops::deleteUnusedPatterns(a.doc);
    a.notify(n ? "Deleted " + std::to_string(n) + " unused pattern(s)" : std::string("No unused patterns"));
  }
  if (ImGui::MenuItem("Render pattern to file...", nullptr, false, cur != nullptr)) exportui::openRender(a, exportui::Preset::PatternFile);
  if (ImGui::MenuItem("Render as audio clip...", nullptr, false, cur != nullptr)) exportui::openRender(a, exportui::Preset::AudioClip);
  if (ImGui::MenuItem("Render and replace...", nullptr, false, cur != nullptr)) exportui::openRender(a, exportui::Preset::PatternReplace);
  ImGui::Separator();
  if (ImGui::BeginMenu("Go to")) {
    for (const auto& p : s.patterns) {
      std::string label = p.name + "##" + std::to_string(p.id);
      if (ImGui::MenuItem(label.c_str(), nullptr, p.id == a.curPattern)) a.selectPattern(p.id);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Next pattern", "Num +", false, idx + 1 < static_cast<int>(s.patterns.size())))
    a.selectPattern(s.patterns[static_cast<size_t>(idx + 1)].id);
  if (ImGui::MenuItem("Previous pattern", "Num -", false, idx > 0)) a.selectPattern(s.patterns[static_cast<size_t>(idx - 1)].id);
}

void menuView(App& a) {
  ImGui::SeparatorText("Windows");
  ImGui::MenuItem("Playlist", "F5", &a.showPlaylist);
  ImGui::MenuItem("Channel rack", "F6", &a.showRack);
  ImGui::MenuItem("Piano roll", "F7", &a.showPiano);
  ImGui::MenuItem("Browser", "Alt+F8", &a.showBrowser);
  if (ImGui::MenuItem("Plugin picker", "F8", browserui::pluginPickerOpen())) browserui::togglePluginPicker();
  menuparity::viewItems(a);  // J-27
  ImGui::MenuItem("Mixer", "F9", &a.showMixer);
  ImGui::MenuItem("Undo history", "Alt+Z", &a.showHistory);
  ImGui::MenuItem("Live pads", nullptr, &a.showLivePads);
  ImGui::MenuItem("Performance", nullptr, &a.showPerformance);
  if (ImGui::MenuItem("Channel settings", nullptr, a.settingsChannel != kNoId, a.doc.song().channel(a.selChannel) != nullptr))
    a.settingsChannel = a.settingsChannel != kNoId ? kNoId : a.selChannel;
  // Zoom acts on the piano roll when it was the last editor used, otherwise on the playlist.
  const bool piano = a.editFocus == App::EditFocus::Piano;
  ImGui::SeparatorText(piano ? "Zoom (piano roll)" : "Zoom (playlist)");
  float& zr = piano ? a.piano.zoomRequest : a.playlist.zoomRequest;
  if (ImGui::MenuItem("Zoom in", "PgUp")) zr *= 1.5f;
  if (ImGui::MenuItem("Zoom out", "PgDn")) zr /= 1.5f;
  if (ImGui::MenuItem(piano ? "Zoom to fit the pattern" : "Zoom to fit the song", "Home")) (piano ? a.piano.fitRequest : a.playlist.fitRequest) = true;
  if (ImGui::MenuItem("Zoom to ticks (snap turns free)")) zr = 1e9f;
  if (!piano && ImGui::MenuItem("Clear time selection", nullptr, false, a.playlist.selEnd > a.playlist.selStart))
    a.playlist.selStart = a.playlist.selEnd = 0;
  ImGui::SeparatorText("Layout");
  if (ImGui::MenuItem("Close all windows", "F12")) a.closeAllWindows();
  if (ImGui::MenuItem("Close plugin windows", "Alt+F12")) a.closePluginWindows();
  if (ImGui::MenuItem("Reset window layout", "Ctrl+Shift+H")) a.resetLayout = true;
  ImGui::SeparatorText("Appearance");
  bool dark = theme::isDark();
  if (ImGui::MenuItem("Dark theme", nullptr, dark)) {
    theme::apply(!dark);
    a.settings.set("ui.darkTheme", !dark);
  }
  if (ImGui::BeginMenu("Mixer layout")) {
    const int cur = static_cast<int>(a.settings.getNumber("ui.mixerLayout"));
    const char* names[] = {"Compact strips", "Normal strips", "Wide strips"};
    for (int i = 0; i < 3; ++i)
      if (ImGui::MenuItem(names[i], nullptr, cur == i)) a.settings.set("ui.mixerLayout", i);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Interface size")) {
    double cur = a.settings.getNumber("ui.scale");
    for (int pct : {75, 90, 100, 110, 125, 150, 175, 200}) {
      std::string l = std::to_string(pct) + "%";
      if (ImGui::MenuItem(l.c_str(), nullptr, std::fabs(cur * 100 - pct) < 0.5)) a.settings.set("ui.scale", pct / 100.0);
    }
    ImGui::EndMenu();
  }
}

void menuOptions(App& a) {
  ImGui::SeparatorText("Settings");
  if (ImGui::MenuItem("Audio settings...")) openSettings(a, 0);
  if (ImGui::MenuItem("MIDI settings...", "F10")) openSettings(a, 1);
  if (ImGui::MenuItem("Project settings...")) openSettings(a, 2);
  if (ImGui::MenuItem("Backup settings...")) openSettings(a, 3);
  if (ImGui::MenuItem("Interface settings...")) openSettings(a, 4);
  if (ImGui::MenuItem("General settings...")) openSettings(a, 5);
  if (ImGui::MenuItem("File settings...")) openSettings(a, 6);
  if (ImGui::MenuItem("Debug log and script console...")) openSettings(a, 7);
  if (ImGui::MenuItem("Project info...", "F11")) a.showInfo = true;
  ImGui::SeparatorText("Switches");
  ImGui::MenuItem("Metronome", "Ctrl+M", &a.metronome);
  ImGui::MenuItem("Typing keyboard to piano", "Ctrl+T", &a.typingKeyboard);
  ImGui::MenuItem("Record notes", nullptr, &a.recordArmed);
  if (ImGui::BeginMenu("Recording options")) {
    recordOptionsMenu(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Typing keyboard options")) {
    typingKeyboardMenu(a);
    ImGui::EndMenu();
  }
  ImGui::MenuItem("Step record", nullptr, &a.stepRecord);
  ImGui::MenuItem("Multilink to controllers", nullptr, &a.multilink);
  menuparity::optionsItems(a);  // J-27
  j31::optionsItems(a);         // J-31: quantized jumps, recording options
  {
    const bool pdc = a.settings.getBool("audio.pdc");
    if (ImGui::MenuItem("Plugin delay compensation", nullptr, pdc)) {
      a.settings.set("audio.pdc", !pdc);
      a.syncEngine();
    }
    ui::Hint("Delays faster paths so tracks with latent effects (look-ahead, linear phase, oversampling) stay in time");
  }
  ImGui::MenuItem("Record automation", nullptr, &automationRecording());
  {
    const bool follow = a.settings.getBool("ui.autoScroll");
    if (ImGui::MenuItem("Auto scrolling", nullptr, follow)) a.settings.set("ui.autoScroll", !follow);
    ui::Hint("The playlist follows the song position while playing");
  }
  bool song = a.engine.mode() == PlayMode::Song;
  if (ImGui::MenuItem("Song mode", "L", song)) a.engine.setMode(song ? PlayMode::Pattern : PlayMode::Song);
  bool liveOn = a.engine.mode() == PlayMode::Live;
  if (ImGui::MenuItem("Live mode", nullptr, liveOn)) a.engine.setMode(liveOn ? PlayMode::Song : PlayMode::Live);
  if (ImGui::BeginMenu("Arrangements")) {
    arrangementMenu(a);
    ImGui::EndMenu();
  }
}

void menuTools(App& a) {
  if (ImGui::BeginMenu("Piano roll tools")) {
    pianoToolItems(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Piano roll selection")) {
    pianoSelectItems(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Chord progression")) {
    progressionItems(a, false);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Riff from progression")) {
    progressionItems(a, true);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Tempo tapper")) {
    static ops::TapTempo tapper;
    ImGui::TextDisabled("Click Tap (or press T) on every beat");
    bool tap = ImGui::Button("Tap", ImVec2(160, 36));
    tap = tap || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_T, false));
    if (tap) tapper.tap(ImGui::GetTime());
    const double bpm = tapper.bpm();
    if (bpm > 0) ImGui::Text("%.1f BPM  (%d taps)", bpm, static_cast<int>(tapper.taps.size()));
    else ImGui::TextDisabled("—");
    if (ImGui::Button("Set the song tempo") && bpm > 0) a.doc.setParam(songParam(SongParam::Tempo), std::round(bpm * 10.0) / 10.0, false);
    ImGui::SameLine();
    if (ImGui::Button("Reset")) tapper.reset();
    ImGui::EndMenu();
  }
  ImGui::SeparatorText("Macros");
  if (ImGui::MenuItem("Stop all sound", "Ctrl+H")) stopSound(a);
  if (ImGui::MenuItem("Route channels to free mixer tracks", "Ctrl+L")) {
    auto t = ops::routeToFreeMixerTracks(a.doc, selectedChannels(a));
    if (!t.empty()) a.notify("Routed to " + std::to_string(t.size()) + " mixer track(s)");
  }
  if (ImGui::MenuItem("Route every channel to its own mixer track")) {
    std::vector<ObjectId> all;
    for (const auto& c : a.doc.song().channels)
      if (c.mixerTrack == a.doc.song().masterId()) all.push_back(c.id);
    auto t = ops::routeToFreeMixerTracks(a.doc, all);
    a.notify(t.empty() ? std::string("Every channel already has a mixer track") : "Routed " + std::to_string(t.size()) + " channel(s)");
  }
  if (ImGui::MenuItem("Select unused channels")) {
    auto u = ops::unusedChannels(a.doc.song());
    if (u.empty()) {
      a.notify("Every channel is used in a pattern");
    } else {
      a.selChannels = std::set<ObjectId>(u.begin(), u.end());
      a.selChannel = u.front();
      a.showRack = true;
    }
  }
  if (ImGui::MenuItem("Delete unused patterns")) {
    int n = ops::deleteUnusedPatterns(a.doc);
    a.notify(n ? "Deleted " + std::to_string(n) + " unused pattern(s)" : std::string("No unused patterns"));
  }
  if (ImGui::MenuItem("Trim unused mixer tracks")) {
    int n = ops::trimUnusedMixerTracks(a.doc);
    a.notify(n ? "Removed " + std::to_string(n) + " unused mixer track(s)" : std::string("No unused mixer tracks at the end"));
  }
  if (ImGui::MenuItem("Unmute all channels")) {
    std::vector<ObjectId> all;
    for (const auto& c : a.doc.song().channels) all.push_back(c.id);
    ops::setChannelsMuted(a.doc, all, false);
  }
  if (ImGui::MenuItem("Unsolo all mixer tracks")) a.doc.editSong("unsolo all", [](Song& s) {
      for (auto& m : s.mixer) m.solo = false;
    });
  if (ImGui::MenuItem("Close plugin windows", "Alt+F12")) a.closePluginWindows();
  menuparity::toolsItems(a);  // J-27
  j31::toolsItems(a);         // J-31: audio logger, cancel recording
}

void menuHelp(App& a) {
  if (ImGui::MenuItem("Keyboard shortcuts", "F1")) a.showShortcuts = true;
  if (ImGui::MenuItem("Load demo song")) a.guardUnsaved([&a]() { a.newFromTemplate("demo"); });
  if (ImGui::MenuItem("Show settings folder")) a.notify("Settings, presets and templates: " + a.configDir());
  ImGui::Separator();
  if (ImGui::MenuItem("About Gridline")) a.showAbout = true;
}

int settingsTabFlags(App& a, int index) {
  if (a.settingsTab != index) return 0;
  a.settingsTab = -1;
  return ImGuiTabItemFlags_SetSelected;
}

// Shortcuts added with the menus (the original set is in handleGlobalShortcuts).
void menuShortcutTable(App& a) {
  const char* rows[][2] = {
      {"Menus", ""}, {"Ctrl+Alt+S", "Save new version"}, {"Ctrl+Shift+R", "Export mixer stems"},
      {"Ctrl+Shift+M", "Export song as MIDI"}, {"Ctrl+Q", "Quit"}, {"Ctrl+H", "Stop all sound"}, {"F2", "Rename (pattern, channel or mixer track)"},
      {"Shift+F4 / Ctrl+F4", "Find first / next empty pattern"}, {"Ctrl+Shift+C", "Clone pattern"}, {"Ctrl+Shift+Ins", "Insert pattern"},
      {"Ctrl+Shift+Del", "Delete pattern"}, {"Ctrl+Shift+Up / Down", "Move pattern"}, {"F12 / Alt+F12", "Close all windows / plugin windows"},
      {"Ctrl+Shift+H", "Reset window layout"},
      {"Channel rack", ""}, {"Alt+C", "Clone selected channels"}, {"Alt+Del", "Delete selected channels"}, {"Alt+Up / Down", "Move selected channels"},
      {"Ctrl+L", "Route selected to free mixer tracks"}, {"Ctrl+Shift+Left / Right", "Rotate steps"}, {"Alt+Z / Alt+U", "Zip selected / unzip all"},
      {"Ctrl+K", "Graph editor"}, {"Insert / Delete (over a step)", "Insert / remove a step"}, {"Ctrl+Alt+C", "Random colours"},
      {"Piano roll", ""}, {"Ctrl+L", "Legato"}, {"Ctrl+U", "Chop into steps"}, {"Ctrl+G", "Glue"}, {"Ctrl+B", "Duplicate"},
      {"Alt+Q", "Quantize..."}, {"Alt+R", "Randomize..."}, {"Alt+U", "Chop..."},
      {"Alt+S", "Strum..."}, {"Alt+A", "Arpeggiate..."}, {"Alt+L", "Articulate..."}, {"Alt+G", "Grace notes..."},
      {"Alt+K", "Limit key range..."}, {"Alt+O", "Note LFO..."}, {"Alt+X", "Scale levels..."}, {"Alt+N", "Note properties..."},
      {"Alt+Shift+R", "Humanize"}, {"T / Y / Z / U", "Mute / play / zoom / detune tool"}, {"Shift+I", "Invert selection"}, {"Shift+R", "Select random notes"},
      {"Playlist", ""}, {"Ctrl+G", "Merge selected clips"},
      {"Mixer", ""}, {"Ctrl+L / Ctrl+Shift+L", "Route selected channels here / from here on"}, {"Alt+Left / Right", "Move track"},
      {"S", "Solo selected track"}};
  ImGui::Spacing();
  if (ImGui::BeginTable("##menukeys", 2, ImGuiTableFlags_RowBg)) {
    for (auto& r : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (r[1][0] == 0) {
        ImGui::PushFont(a.fontBold, 0.0f);
        ImGui::TextUnformatted(r[0]);
        ImGui::PopFont();
        continue;
      }
      ImGui::PushFont(a.fontMono, 12.0f);
      ImGui::TextUnformatted(r[0]);
      ImGui::PopFont();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", r[1]);
    }
    ImGui::EndTable();
  }
}

// ------------------------------------------------------------ shortcuts ----

bool menuShortcuts(App& a) {
  drawPrompt();
  // Track which editor Edit-menu commands go to.
  if (ImGuiWindow* nw = GImGui->NavWindow) {
    const char* n = nw->RootWindow ? nw->RootWindow->Name : nw->Name;
    if (!std::strcmp(n, "Piano roll")) a.editFocus = App::EditFocus::Piano;
    else if (!std::strcmp(n, "Playlist")) a.editFocus = App::EditFocus::Playlist;
    else if (!std::strcmp(n, "Channel rack")) a.editFocus = App::EditFocus::Rack;
    else if (!std::strcmp(n, "Mixer")) a.editFocus = App::EditFocus::Mixer;
  }
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
  const bool ctrl = io.KeyCtrl || io.KeySuper, alt = io.KeyAlt, shift = io.KeyShift;
  bool used = false;  // a handled key stops handleGlobalShortcuts from also acting on it
  auto key = [&used](ImGuiKey k) { bool p = ImGui::IsKeyPressed(k, false); used |= p; return p; };
  auto rep = [&used](ImGuiKey k) { bool p = ImGui::IsKeyPressed(k, true); used |= p; return p; };
  ImGuiWindow* nav = GImGui->NavWindow;
  const char* focused = nav ? (nav->RootWindow ? nav->RootWindow->Name : nav->Name) : "";
  const bool inPiano = !std::strcmp(focused, "Piano roll"), inPlaylist = !std::strcmp(focused, "Playlist");
  const bool inRack = !std::strcmp(focused, "Channel rack"), inMixer = !std::strcmp(focused, "Mixer");

  // File.
  if (ctrl && alt && key(ImGuiKey_S)) a.saveNewVersion();
  if (ctrl && shift && key(ImGuiKey_R)) exportui::openRender(a, exportui::Preset::Stems);
  if (ctrl && shift && key(ImGuiKey_M)) exportui::exportMidiDialog(a, 0);
  if (ctrl && !shift && !alt && !inPiano && key(ImGuiKey_Q)) a.quitRequested = true;
  if (ctrl && !shift && key(ImGuiKey_H)) stopSound(a);
  // Patterns.
  if (key(ImGuiKey_F2)) renameFocused(a);
  if (shift && !ctrl && key(ImGuiKey_F4)) findEmptyPattern(a, true, true);
  if (ctrl && !shift && key(ImGuiKey_F4)) findEmptyPattern(a, false, false);
  if (ctrl && shift && !alt) {
    if (!inPiano && key(ImGuiKey_C)) a.selectPattern(a.doc.clonePattern(a.curPattern));
    if (key(ImGuiKey_Insert)) a.selectPattern(ops::insertPattern(a.doc, a.curPattern, ops::nextPatternName(a.doc.song())));
    if (key(ImGuiKey_Delete)) deletePattern(a);
    if (!inPiano && !inRack && rep(ImGuiKey_UpArrow)) movePattern(a, -1);
    if (!inPiano && !inRack && rep(ImGuiKey_DownArrow)) movePattern(a, 1);
  }
  // View / Options.
  if (!ctrl && !alt && !shift && key(ImGuiKey_F10)) {
    a.showSettings = !a.showSettings;
    if (a.showSettings) a.settingsTab = 1;  // MIDI tab, like the menu entry
  }
  if (key(ImGuiKey_F12)) alt ? a.closePluginWindows() : a.closeAllWindows();
  if (ctrl && shift && key(ImGuiKey_H)) a.resetLayout = true;

  // Channel rack.
  if (inRack) {
    auto sel = selectedChannels(a);
    if (alt && !ctrl && key(ImGuiKey_C)) {
      auto ids = ops::cloneChannels(a.doc, sel);
      if (!ids.empty()) a.selChannel = ids.back(), a.selChannels = std::set<ObjectId>(ids.begin(), ids.end());
    }
    if (ctrl && alt && key(ImGuiKey_C)) {
      a.doc.beginGroup("random colours");
      for (ObjectId c : sel) ops::setChannelsColor(a.doc, {c}, randomColor());
      a.doc.endGroup();
    }
    if (alt && key(ImGuiKey_Delete)) ops::removeChannels(a.doc, sel);
    if (alt && !ctrl && rep(ImGuiKey_UpArrow)) ops::moveChannels(a.doc, sel, -1);
    if (alt && !ctrl && rep(ImGuiKey_DownArrow)) ops::moveChannels(a.doc, sel, 1);
    if (ctrl && !shift && key(ImGuiKey_L)) {
      auto t = ops::routeToFreeMixerTracks(a.doc, sel);
      if (!t.empty()) a.selMixer = t.back();
    }
    if (alt && !ctrl && key(ImGuiKey_Z))
      for (ObjectId c : sel) a.rack.zipped.insert(c);
    if (alt && !ctrl && key(ImGuiKey_U)) a.rack.zipped.clear();
    if (ctrl && shift && rep(ImGuiKey_LeftArrow)) ops::rotateChannel(a.doc, a.curPattern, a.selChannel, -1);
    if (ctrl && shift && rep(ImGuiKey_RightArrow)) ops::rotateChannel(a.doc, a.curPattern, a.selChannel, 1);
    if (ctrl && !shift && key(ImGuiKey_C)) editCopy(a);
    if (ctrl && key(ImGuiKey_V)) editPaste(a);
    if (ctrl && key(ImGuiKey_X)) editCut(a);
    if (ctrl && key(ImGuiKey_A)) editSelectAll(a);
  }
  // Piano roll (keys the piano roll window does not handle itself).
  if (inPiano && pianoHasTarget(a)) {
    Tick step = std::max<Tick>(1, a.doc.song().ppq / 4);
    if (ctrl && key(ImGuiKey_L)) pianoEdit(a, "legato", [](auto& n, auto& s) { ops::legato(n, s); });
    if (ctrl && key(ImGuiKey_U)) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, step); });
    if (ctrl && key(ImGuiKey_G)) pianoEdit(a, "glue notes", [](auto& n, auto& s) { ops::glue(n, s); });
    if (ctrl && key(ImGuiKey_B)) editDuplicate(a);
    // J-11: tool windows (live preview, one undo step).
    if (alt && !ctrl && !shift) {
      if (key(ImGuiKey_Q)) openNoteTool(a, NoteTool::Quantize);
      if (key(ImGuiKey_R)) openNoteTool(a, NoteTool::Randomize);
      if (key(ImGuiKey_U)) openNoteTool(a, NoteTool::Chop);
      if (key(ImGuiKey_S)) openNoteTool(a, NoteTool::Strum);
      if (key(ImGuiKey_A)) openNoteTool(a, NoteTool::Arpeggiate);
      if (key(ImGuiKey_L)) openNoteTool(a, NoteTool::Articulate);
      if (key(ImGuiKey_K)) openNoteTool(a, NoteTool::Limit);
      if (key(ImGuiKey_O)) openNoteTool(a, NoteTool::Lfo);
      if (key(ImGuiKey_X)) openNoteTool(a, NoteTool::ScaleLevels);
      if (key(ImGuiKey_G)) openNoteTool(a, NoteTool::Flam);
      if (key(ImGuiKey_N)) openNoteTool(a, NoteTool::Properties);
    }
    if (alt && shift && !ctrl && key(ImGuiKey_R)) {
      uint32_t seed = static_cast<uint32_t>(a.now * 1000.0) + 7;
      pianoEdit(a, "humanize", [&](auto& n, auto& s) { ops::humanize(n, s, 12, std::max<Tick>(1, step / 8), seed); });
    }
    if (shift && !ctrl && !alt && key(ImGuiKey_I)) pianoEdit(a, "", [](auto&, auto& s) { ops::invertSelection(s); });
    if (shift && !ctrl && !alt && key(ImGuiKey_R)) {
      uint32_t seed = static_cast<uint32_t>(a.now * 1000.0) + 3;
      pianoEdit(a, "", [&](auto& n, auto& s) { ops::selectRandom(n, s, seed); });
    }
  }
  // Playlist.
  if (inPlaylist && ctrl && key(ImGuiKey_G) && a.playlist.selected.size() >= 2) {
    ObjectId nc = ops::mergeClips(a.doc, a.playlist.selected);
    if (nc) a.playlist.selected = {nc};
  }
  // Mixer.
  if (inMixer) {
    auto chans = selectedChannels(a);
    if (ctrl && key(ImGuiKey_L) && !chans.empty()) ops::routeChannels(a.doc, chans, a.selMixer, shift);
    if (alt && rep(ImGuiKey_LeftArrow)) ops::moveMixerTrack(a.doc, a.selMixer, -1);
    if (alt && rep(ImGuiKey_RightArrow)) ops::moveMixerTrack(a.doc, a.selMixer, 1);
    if (!ctrl && !alt && !shift && !a.typingKeyboard && key(ImGuiKey_S)) ops::soloMixerTrack(a.doc, a.selMixer);
  }
  return used;
}

}  // namespace gl

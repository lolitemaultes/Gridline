// Gridline — dialogs (file, settings, project info, export, history, about,
// shortcuts, unsaved changes) and global keyboard shortcuts.
#include "RecordCard.h"
#include "WindowRaise.h"
#include "LayoutShellUi.h"
#include "LiveIo.h"
#include "LiveLook.h"
#include "ChopUi.h"
#include "../core/ChromeOs.h"
#include "../core/LayoutShell.h"
#include "CollabUi.h"
#include "../core/RemoteFs.h"
#include "../core/TrackModes.h"
#include "../engine/ProjectLoad.h"
#include "DisplayConfirmUi.h"
#include "DrumKitUi.h"
#include "FileDialogUi.h"
#include "GridlineVersion.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ScriptUi.h"
#include "UndoUi.h"
#include "Keys.h"
#include "J31Ui.h"
#include "MenuParityUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Backup.h"
#include "../core/I18n.h"
#include "../core/EditorZoom.h"
#include "../core/UiScale.h"
#include "../core/flp/Flp.h"
#include "App.h"
#include "LogicPlugins.h"
#include "LogicNewTracks.h"
#include "LayoutPresets.h"
#include "TrackerLook.h"
#include "TrackerView.h"
#include "BrowserUi.h"
#include "../engine/PlaylistTools.h"
#include "AudioRecordUi.h"
#include "AsioUi.h"
#include "ExportUi.h"
#include "StemsUi.h"
#include "TrackReceiptUi.h"
#include "../core/ProjectInterop.h"
#include "Theme.h"
#include "MidiUi.h"
#include "SettingsPages.h"
#include "PluginFoldersUi.h"
#include "SmoothScrollUi.h"
#include "McpUi.h"
#include "OscUi.h"
#include "TemplateUi.h"
#include "FlpExportUi.h"
#include "J59Ui.h"
#include "SuiteUi.h"
#include "UpdateUi.h"
#include "UrlImportUi.h"
#include "Widgets.h"
#include "MeterUi.h"
#include "../core/MeterEdit.h"
#include "Windows.h"
#include "SpeechUi.h"
#include "GrooveUi.h"
#include "TrackerEditors.h"
#include "Viewports.h"
#include "../core/WheelRoute.h"
#include "../core/Platform.h"

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

// K-112b: the clips across the song position split there (the selected ones only, when there is a
// selection): the Tracks key set's Cmd+T and its Edit menu.
void splitClipsAtPlayhead(App& a) {
  const Song& s = a.doc.song();
  const Tick at = static_cast<Tick>(a.engine.position());
  std::vector<ObjectId> ids = pltools::clipsAcross(s, at, 0, static_cast<int>(s.tracks.size()) - 1);
  if (!a.playlist.selected.empty())
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&](ObjectId c) { return !a.playlist.selected.count(c); }), ids.end());
  if (!ids.empty()) a.doc.editSong("split clips", [&](Song& x) { pltools::sliceClips(x, ids, at, a.playlist.zeroCross); });
}

namespace {

// J-54: centred in the work area, size clamped to the screen (short screens / large interface scale).
void centerNext(ImVec2 size, ImVec2 minSize = ImVec2(0, 0)) { ui::PlaceNextDialog(size, ImGuiCond_Appearing, minSize); }

// Label column for settings-style forms: dim caption, control starts at `x`.
// J-54: a label longer than the column is fitted (ellipsis, full text on hover).
void formLabel(const char* label, float x = 168.0f) { ui::FormLabelFit(i18n::tr(label), x, pal().textDim); }

// Right-aligns the next `width` pixels of buttons on the current line.
void alignRight(float width) {
  float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
}

// K-106 cp21 (look audit): in the Tracker the two question boxes (unsaved changes, confirm) are the
// classic centred dialog: square, a raised bevel frame round a sunken well, the title in the bold
// face at the interface size, bevel buttons. Same popups, keys and actions; the other layouts are
// unchanged.
bool trackerDialogs() { return layoutui::active() == layout::Preset::Tracker; }
struct DialogLook {
  bool on = trackerDialogs();
  DialogLook() {
    if (on) {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    }
  }
  ~DialogLook() {
    if (on) ImGui::PopStyleVar(2);
  }
  DialogLook(const DialogLook&) = delete;
  DialogLook& operator=(const DialogLook&) = delete;
};
// The frame, inside the opened popup (drawn under its rows).
void dialogFrame() {
  if (!trackerDialogs()) return;
  const ImVec2 p0 = ImGui::GetWindowPos(), p1(p0.x + ImGui::GetWindowWidth(), p0.y + ImGui::GetWindowHeight());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  trackerview::bevelBox(dl, p0, p1, false);
  trackerview::bevelBox(dl, ImVec2(p0.x + 3, p0.y + 3), ImVec2(p1.x - 3, p1.y - 3), true);
}
// A dialog button: the shared text or primary button, or in the Tracker a bevel button.
bool dialogButton(App& a, const char* label, float w, bool primary) {
  if (!trackerDialogs()) return primary ? ui::PrimaryButton(label, w) : ui::TextButton(label, false, w);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = std::round(ImGui::GetFontSize() + 10);
  ImGui::PushID(label);
  const bool r = trackerview::bevelButton(a, "##tdb", label, p, ImVec2(p.x + w, p.y + h), false, nullptr);
  ImGui::PopID();
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(w, h));
  return r;
}

void dialogTitle(App& a, const char* title, const char* subtitle = nullptr) {
  ImGui::PushFont(a.fontBold, trackerDialogs() ? ImGui::GetFontSize() : theme::kFontTitle);  // (cp21: the Tracker's titles at its own size)
  ImGui::TextUnformatted(i18n::tr(title));
  ImGui::PopFont();
  if (subtitle) ImGui::TextColored(theme::vec(pal().textDim), "%s", i18n::tr(subtitle));
  ImGui::Dummy(ImVec2(0, theme::kS1));
}

ImVec2 gDevSettingsSize(0, 0);  // --settings-size (J-137)

}  // namespace

void setDevSettingsSize(float w, float h) { gDevSettingsSize = ImVec2(w, h); }

namespace {

// J-24 appended General, Files and Debug (tab indices 0..4 stay as they were), J-55 Shortcuts and Theme.
const char* const kSettingsPagesEn[] = {i18n::N_("Audio"), i18n::N_("MIDI"), i18n::N_("Project"), i18n::N_("Backup"), i18n::N_("Interface"), i18n::N_("General"), i18n::N_("Files"), i18n::N_("Debug"), i18n::N_("Shortcuts"), i18n::N_("Theme"), i18n::N_("Integrations"), i18n::N_("Collaboration"), i18n::N_("Plugins")};

// K-93: one settings page's controls (the settings window draws it; the command palette draws
// each page once, unseen, to find every setting). `page` may change (a button that opens another page).
void settingsPageBody(App& a, int& page, float fieldW) {
  const auto& P = pal();
  Settings& st = a.settings;
  (void)P;
  if (page == 0) {
    ui::BeginSection("INPUT / OUTPUT");
    const bool asioMode = asioui::backendRow(a, fieldW);  // K-6
    if (asioMode) asioui::deviceRows(a, fieldW);
    if (!asioMode) {
      formLabel("Output device");
      if (ui::BeginCombo("##dev", a.audioDevice.empty() ? "System default" : a.audioDevice.c_str(), fieldW)) {
        // J-77: device / rate / buffer changes apply at once and ask Keep / Revert (they can cut the sound).
        if (ImGui::Selectable(i18n::tr("System default"), a.audioDevice.empty())) ui::confirmAudioDevice(a, "");
        for (const auto& d : a.audioDevices)
          if (ImGui::Selectable(d.c_str(), d == a.audioDevice)) ui::confirmAudioDevice(a, d);
        ImGui::EndCombo();
      }
      // K-112b checkpoint 35: one option row each (a pop-up menu in the Smooth layout).
      const int rates[] = {44100, 48000, 88200, 96000};
      formLabel("Sample rate");
      {
        std::string names[4];
        const char* labels[4];
        int idx = -1;
        for (int i = 0; i < 4; ++i) {
          names[i] = std::to_string(rates[i]);
          labels[i] = names[i].c_str();
          if (a.audioSampleRate == rates[i]) idx = i;
        }
        if (ui::OptionRow("rate", labels, 4, &idx, 160.0f) && a.audioSampleRate != rates[idx]) {
          const dispconf::Snapshot before = dispconf::take(st, dispconf::audioKeys());
          st.set("audio.sampleRate", rates[idx]);
          ui::confirmAudio(a, before);
        }
      }
      const int bufs[] = {64, 128, 256, 512, 1024, 2048};
      formLabel("Buffer size");
      {
        std::string names[6];
        const char* labels[6];
        int idx = -1;
        for (int i = 0; i < 6; ++i) {
          names[i] = std::to_string(bufs[i]);
          labels[i] = names[i].c_str();
          if (a.audioBuffer == bufs[i]) idx = i;
        }
        if (ui::OptionRow("buf", labels, 6, &idx, 160.0f) && a.audioBuffer != bufs[idx]) {
          const dispconf::Snapshot before = dispconf::take(st, dispconf::audioKeys());
          st.set("audio.bufferFrames", bufs[idx]);
          ui::confirmAudio(a, before);
        }
      }
      // K-101 follow-up: extra output pairs of the system device (Hardware Insert sends, Voltage Out).
      formLabel("Output channels");
      {
        const int outs[] = {2, 4, 6, 8};
        const char* labels[] = {"2", "4", "6", "8"};
        const int cur = static_cast<int>(st.getNumber("audio.outputChannels"));
        int idx = -1;
        for (int i = 0; i < 4; ++i)
          if (cur == outs[i]) idx = i;
        if (ui::OptionRow("outch", labels, 4, &idx, 160.0f) && cur != outs[idx]) {
          st.set("audio.outputChannels", outs[idx]);
          a.audioReopen = true;
        }
        ui::Hint("Channels opened on the output device: 3-4, 5-6 and 7-8 are extra pairs for Hardware Insert sends and Voltage Out");
      }
      char lat[96];
      std::snprintf(lat, sizeof lat, "%.1f ms", 1000.0 * a.audioBuffer / std::max(1, a.audioSampleRate));
      formLabel("Output latency");
      ImGui::PushFont(livelook::numFont(a), theme::kFontNumber + 1);  // K-112c: sans in Live, as the refs' values
      ImGui::TextUnformatted(lat);
      ImGui::PopFont();
    }
    ui::EndSection();
    if (asioMode) {
      ui::BeginSection("ASIO CHANNELS");
      asioui::channelRows(a, fieldW);
      ui::EndSection();
    }
    ui::BeginSection("MIXER");
    // Mixer processing threads (the output is identical for any count).
    {
      const int cur = static_cast<int>(st.getNumber("audio.threads"));
      formLabel("Processing threads");
      const int counts[] = {0, 1, 2, 4, 6, 8};
      std::string names[6];
      const char* labels[6];
      const char* hints[6] = {"Half the hardware threads, at most 8", "Everything on the audio thread", nullptr, nullptr, nullptr, nullptr};
      int idx = -1;
      for (int i = 0; i < 6; ++i) {
        const int c = counts[i];
        names[i] = c == 0 ? std::string(i18n::tr("Auto")) + " (" + std::to_string(a.processingThreads()) + ")" : std::to_string(c);
        if (c == 0 && cur > 0 && !ui::OptionsAsMenus()) names[i] = i18n::tr("Auto");
        labels[i] = names[i].c_str();
        if (cur == c) idx = i;
      }
      if (ui::OptionRow("threads", labels, 6, &idx, 160.0f, hints)) {
        st.set("audio.threads", counts[idx]);
        a.syncEngine();
      }
    }
    formLabel("Delay compensation");
    {
      bool pdc = st.getBool("audio.pdc");
      if (ImGui::Checkbox(i18n::tr("Plugin delay compensation##pdc"), &pdc)) {
        st.set("audio.pdc", pdc);
        a.syncEngine();
      }
      ui::Hint("Delays faster paths so tracks with latent effects (look-ahead, linear phase, oversampling) stay in time");
      const int ol = a.engine.outputLatency();
      if (ol > 0) {
        ImGui::SameLine(0, theme::kS2);
        ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%d samples (%.1f ms)"), ol, 1000.0 * ol / std::max(1, a.audioSampleRate));
      }
    }
    ui::EndSection();
    ui::BeginSection("INPUT & RECORDING");
    audioInputSettings(a, fieldW);  // J-8
    ui::EndSection();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", a.audioStatus.c_str());
    ImGui::PopTextWrapPos();
  }
  if (page == 1) midiSettingsPage(a, fieldW);
  if (page == 2) {
    ui::BeginSection("NEW PROJECTS");
    int ppq = static_cast<int>(st.getNumber("project.defaultPpq"));
    formLabel("Timebase (PPQ)");
    if (ui::BeginCombo("##ppq", std::to_string(ppq).c_str(), 160)) {
      for (int p : kPpqChoices)
        if (ImGui::Selectable(std::to_string(p).c_str(), p == ppq)) st.set("project.defaultPpq", p);
      ImGui::EndCombo();
    }
    double tempo = st.getNumber("project.defaultTempo");
    formLabel("Tempo (BPM)");
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputDouble("##deftempo", &tempo, 1, 10, "%.1f")) st.set("project.defaultTempo", std::clamp(tempo, 10.0, 522.0));
    projectDefaultsPanel(a);  // J-24
    ui::EndSection();
    ui::BeginSection("FL STUDIO IMPORT");
    {
      static char flData[512];
      static bool init = false;
      if (!init) {
        std::snprintf(flData, sizeof flData, "%s", st.getString("import.flData").c_str());
        init = true;
      }
      formLabel("FL data folder");
      ImGui::SetNextItemWidth(fieldW);
      if (ImGui::InputTextWithHint("##fldata", i18n::tr("Detect automatically"), flData, sizeof flData)) st.set("import.flData", std::string(flData));
      ui::FocusRing();
      ui::Hint("Folder that %FLStudioFactoryData% points to (holds the samples FL projects use). Empty = detect.");
      std::string used = flData[0] ? std::string(flData) : flp::defaultFactoryData();
      formLabel("In use");
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::vec(pal().textDim), "%s", used.empty() ? "not found: samples from FL's library will be missing" : used.c_str());
      ImGui::PopTextWrapPos();
      // Convert FL's presets (plugin, channel and mixer presets) into the local preset library.
      formLabel("Presets");
      if (a.presetImport.running) {
        float fr = a.presetImport.total ? static_cast<float>(a.presetImport.done) / static_cast<float>(a.presetImport.total) : 0.0f;
        char lbl[64];
        std::snprintf(lbl, sizeof lbl, "%zu / %zu", a.presetImport.done.load(), a.presetImport.total.load());
        ImGui::ProgressBar(fr, ImVec2(fieldW - 80, 0), lbl);
        ImGui::SameLine(0, theme::kS1);
        if (ui::TextButton("Cancel", false, 72)) a.presetImport.cancel = true;
      } else {
        if (ui::TextButton("Import FL Studio presets", false, 0,
                           "Converts every preset of your FL Studio installation into Gridline presets on this computer"))
          a.startPresetImport();
        if (!a.presetImport.summary.empty()) {
          ImGui::PushTextWrapPos(0);
          ImGui::TextColored(theme::vec(pal().textDim), "%s", a.presetImport.summary.c_str());
          ImGui::PopTextWrapPos();
        }
      }
      ui::Hint("Presets appear in each plugin's preset menu under Library \xE2\x96\xB8 FL Studio, effect chains in the mixer track menu");
    }
    ui::EndSection();
    ui::BeginSection("FL STUDIO EXPORT");  // K-110
    {
      static const char* keys[] = {"suite+fallback", "suite", "render"};
      static const char* names[] = {i18n::N_("Gridline Suite plugins, with muted audio copies"), i18n::N_("Gridline Suite plugins only"),
                                    i18n::N_("Rendered to audio")};
      auto nameOf = [&](const std::string& k) {
        for (int i = 0; i < 3; ++i)
          if (k == keys[i]) return i18n::tr(names[i]);
        return i18n::tr(names[0]);
      };
      formLabel("Gridline plugins");
      const std::string cur = st.getString("export.flGridlinePlugins");
      if (ui::BeginCombo("##flgl", nameOf(cur), fieldW)) {
        for (int i = 0; i < 3; ++i)
          if (ImGui::Selectable(i18n::tr(names[i]), cur == keys[i])) st.set("export.flGridlinePlugins", std::string(keys[i]));
        ImGui::EndCombo();
      }
      ui::Hint("How Gridline plugins without an exact FL Studio counterpart go into FL Studio projects. Suite plugins open live where the Gridline Suite is installed; muted audio copies play the instruments everywhere else.");
      formLabel("This project");
      const std::string pk = a.doc.song().flExportGridline;
      if (pk.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(pal().textDim), "%s", i18n::tr("Uses the default"));
      } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(pal().text), "%s", nameOf(pk));
        ImGui::SameLine(0, theme::kS2);
        if (ui::TextButton(i18n::tr("Use the default"), false, 0, "This project's FL Studio exports follow the default above again"))
          a.doc.editSong("FL Studio export choice", [](Song& s) { s.flExportGridline.clear(); });
      }
    }
    ui::EndSection();
    ui::BeginSection("UNDO");
    // K-141: applied on Enter or when the field is left (typing "5" on the way to "500" must not
    // drop steps), always 10 to 10000.
    static int levels = 0;
    formLabel("Maximum levels");
    ImGui::SetNextItemWidth(96);
    if (ImGui::GetActiveID() != ImGui::GetID("##levels")) levels = static_cast<int>(st.getNumber("undo.maxLevels"));
    ImGui::InputInt("##levels", &levels, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      levels = std::clamp(levels, static_cast<int>(UndoStack::kMinLevels), static_cast<int>(UndoStack::kMaxLevels));
      st.set("undo.maxLevels", levels);
      a.doc.setMaxUndoLevels(static_cast<size_t>(levels));
    }
    ui::Hint("Steps kept from 10 to 10000, applied when you press Enter. The oldest steps also go once the history uses 256 MB.");
    bool skip = st.getBool("undo.skipEmpty");
    if (ImGui::Checkbox(i18n::tr("In a shared project, skip steps that have nothing left to undo"), &skip)) st.set("undo.skipEmpty", skip);
    ui::Hint("Off: each press undoes one of your steps, and tells you when a collaborator changed it so nothing was left.");
    ui::EndSection();
    collabui::projectSection(a);  // J-111
  }
  if (page == 3) {
    static const char* freq[] = {i18n::N_("Never (remind every 10 minutes)"), i18n::N_("Rarely (every 15 minutes)"), i18n::N_("Occasionally (every 10 minutes)"),
                                 i18n::N_("Regularly (every 5 minutes)"), i18n::N_("Frequently (5 minutes + before risky edits)"),
                                 i18n::N_("Very frequently (every minute + before risky edits)")};
    int f = std::clamp(static_cast<int>(st.getNumber("backup.frequency")), 0, 5);
    ui::BeginSection("HOW OFTEN");
    // K-112b checkpoint 35: a pop-up menu in the Smooth layout (its preferences pick such a choice from one).
    int picked = -1;
    if (ui::OptionsAsMenus()) {
      const char* labels[6];
      for (int i = 0; i < 6; ++i) labels[i] = i18n::tr(freq[i]);
      formLabel("Backups");
      int idx = f;
      if (ui::OptionRow("freq", labels, 6, &idx, fieldW)) picked = idx;
    } else
      for (int i = 0; i < 6; ++i)
        if (ImGui::RadioButton(i18n::tr(freq[i]), f == i)) picked = i;
    if (picked >= 0) {
      const int i = picked;
      st.set("backup.frequency", i);
      BackupPolicy p = a.backups->policy();
      p.frequency = static_cast<BackupFrequency>(i);
      a.backups = std::make_unique<BackupManager>(p);
    }
    ui::EndSection();
    ui::BeginSection("STORAGE");
    int maxCount = static_cast<int>(st.getNumber("backup.maxCount"));
    formLabel("Backups kept");
    ImGui::SetNextItemWidth(96);
    if (ImGui::InputInt("##maxcount", &maxCount, 0, 0)) {
      maxCount = std::clamp(maxCount, 1, 1000);
      st.set("backup.maxCount", maxCount);
      BackupPolicy p = a.backups->policy();
      p.maxCount = maxCount;
      a.backups = std::make_unique<BackupManager>(p);
    }
    bool inProject = st.getString("backup.location") == "project";
    if (ImGui::Checkbox(i18n::tr("Save backups next to the project once it has been saved"), &inProject)) {
      st.set("backup.location", inProject ? "project" : "global");
      BackupPolicy p = a.backups->policy();
      p.inProjectFolder = inProject;
      a.backups = std::make_unique<BackupManager>(p);
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("Global backup folder: %s"), a.backups->policy().globalFolder.c_str());
    ImGui::PopTextWrapPos();
    ui::EndSection();
    backupRestorePanel(a, fieldW);  // J-24
  }
  if (page == 4) {
    ui::BeginSection("LAYOUT");  // K-79: window layout presets
    ui::SectionFullWidth();      // K-112b: the cards use the whole width
    {
      // K-130: the welcome screen's cards at a fitting size, the two options, then the reset.
      const float w = ImGui::GetContentRegionAvail().x;
      ImGui::Dummy(ImVec2(0, theme::kS1));
      layoutui::cards(a, w, 232.0f);
      ImGui::Dummy(ImVec2(0, theme::kS2));
      layoutui::linkedOptions(a, std::min(w, 640.0f));
      ImGui::Dummy(ImVec2(0, theme::kS1));
      ImGui::Separator();
      ImGui::Dummy(ImVec2(0, theme::kS1));
      const char* resetLabel = i18n::tr("Reset this layout");
      const float resetW = ImGui::CalcTextSize(resetLabel).x + ImGui::GetStyle().FramePadding.x * 2;
      ImGui::AlignTextToFramePadding();
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
      ImGui::TextUnformatted(i18n::tr("Each layout remembers your own changes to it."));
      ImGui::PopStyleColor();
      ImGui::SameLine();
      alignRight(resetW);
      if (ui::TextButton("Reset this layout", false, 0, "Builds the default arrangement of the layout in use again")) ui::confirmLayoutReset(a);
    }
    ui::EndSection();
    ui::BeginSection("APPEARANCE");
    formLabel("Theme");
    if (ui::TextButton("Theme settings...", false, 0, "Dark, light or follow the system, accent colour, contrast")) page = 9;
    formLabel("Interface scale");
    {  // J-77: nothing changes while dragging; the value you let go of asks Keep / Revert.
      static dispconf::DeferredValue drag;
      const double committed = st.getNumber("ui.scale");
      float pct = static_cast<float>(std::round(drag.shown(committed) * 100.0));
      ImGui::SetNextItemWidth(fieldW);
      if (ImGui::SliderFloat("##scale", &pct, 75.0f, 200.0f, "%.0f%%")) drag.edit(std::round(pct / 5.0f) * 5.0 / 100.0);
      double to = 0;
      if (ImGui::IsItemDeactivated() && drag.release(committed, &to)) ui::confirmScale(a, to);
      ui::Hint("Scales the whole interface; asks to keep the new size once you let go");
    }
#if !defined(__APPLE__)
    {  // J-91: the desktop's display scale (Xft.dpi, GDK_SCALE, Wayland, Windows DPI) on top of the size above
      formLabel("");
      bool follow = st.getBool("ui.scaleAuto");
      char lbl[96];
      std::snprintf(lbl, sizeof lbl, i18n::tr("Follow the system's display scale (%d%%)"), static_cast<int>(std::lround(uiscale::detected() * 100.0f)));
      if (ImGui::Checkbox(lbl, &follow)) st.set("ui.scaleAuto", follow);
      ui::Hint("On: the interface size is multiplied by the scale set in your system's display settings. Off: the size above is used as it is");
    }
#endif
#if !defined(__APPLE__) && !defined(_WIN32)
    {  // K-153: X11 through XWayland (plugin interfaces inside their windows, detachable panels) or native Wayland
      static const char* const kSystems[] = {i18n::N_("Automatic (X11 when available)"), i18n::N_("Native Wayland")};
      const int cur = st.getString("ui.windowSystem") == "wayland" ? 1 : 0;
      formLabel("Window system");
      if (ui::BeginCombo("##windowsystem", i18n::tr(kSystems[cur]), fieldW)) {
        for (int i = 0; i < 2; ++i)
          if (ImGui::Selectable(i18n::tr(kSystems[i]), cur == i)) st.set("ui.windowSystem", i == 1 ? "wayland" : "auto");
        ImGui::EndCombo();
      }
      ui::Hint("Applies the next time Gridline starts. Automatic runs Gridline through XWayland on a Wayland desktop, so plugin "
               "interfaces sit inside their plugin windows and panels can be detached. Native Wayland: plugin interfaces open "
               "in windows of their own and panels stay inside the main window");
      formLabel("");
      ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("Now running on: %s"), a.windowSystem.empty() ? "-" : a.windowSystem.c_str());
    }
#endif
    // K-136: Gridline's own plugin windows, on top of the interface scale (core/EditorZoom.h).
    formLabel("Plugin window size");
    {
      float pct = std::round(edzoom::globalSize(st.getNumber("ui.pluginWindowSize")) * 100.0f);
      ImGui::SetNextItemWidth(fieldW);
      if (ImGui::SliderFloat("##pluginsize", &pct, edzoom::kGlobalMin * 100.0f, edzoom::kGlobalMax * 100.0f, "%.0f%%"))
        st.set("ui.pluginWindowSize", static_cast<double>(edzoom::globalSize(pct / 100.0)));
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        st.set("ui.pluginWindowSize", static_cast<double>(edzoom::kGlobalDefault));
      ui::Hint("Size of the built-in instrument and effect windows (right-click: default). "
               "A size picked in a plugin window's menu (Editor size) is kept for that plugin");
      formLabel("");
      bool fit = st.getBool("ui.pluginWindowFit");
      if (ImGui::Checkbox(i18n::tr("Open small plugin windows larger on big screens"), &fit)) st.set("ui.pluginWindowFit", fit);
      ui::Hint("On: a small plugin window grows to suit a large workspace. Plugins you install keep their own size");
    }
    formLabel("Confirm changes");
    bool autoRevert = st.getBool("ui.confirmAutoRevert");
    if (ImGui::Checkbox(i18n::tr("Revert size, theme, language and audio changes not kept within 15 s"), &autoRevert))
      st.set("ui.confirmAutoRevert", autoRevert);
    ui::Hint("A change that makes the interface hard to use or cuts the sound goes back by itself");
    bool hints = st.getBool("ui.showHints");
    formLabel("Status bar");
    if (ImGui::Checkbox(i18n::tr("Show hints for the control under the mouse"), &hints)) st.set("ui.showHints", hints);
    hintSettingsPanel(a, fieldW);  // J-24
    // J-113: windows dragged out of the main window become windows of their own.
    bool detach = st.getBool("ui.detachWindows") && viewports::available();
    formLabel("Windows");
    ImGui::BeginDisabled(!viewports::available());
    if (ImGui::Checkbox(i18n::tr("Allow windows outside the main window"), &detach)) st.set("ui.detachWindows", detach);
    ImGui::EndDisabled();
    ui::Hint("Drag a window by its title bar out of Gridline, onto another monitor if you like. "
             "Off: every window stays inside the main window");
    if (!viewports::available()) {
      formLabel("");
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr(viewports::unavailableReason().c_str()));
    }
    // K-112: the layouts with a menu row of their own (development layouts for now) can put their
    // menus in the system's menu bar. Studio keeps its menus in its top bar either way.
    if (layoutui::devLayouts(a) || layout::current(st) == layout::Preset::Tracker) {  // K-106: the Tracker has its own menu row in every build
      bool sysMenu = st.getBool("ui.systemMenuBar");
      formLabel("Menu bar");
      if (ImGui::Checkbox(i18n::tr("Put the menus in the system menu bar"), &sysMenu)) st.set("ui.systemMenuBar", sysMenu);
      ui::Hint("In the layouts with a menu row of their own, the menus move to the menu bar at the top of the screen (macOS) or "
               "the desktop's global menu (Linux) where there is one. Off: they stay in the window. Studio keeps its menus in its top bar");
    }
    // K-112: installed plugins' windows in the Live layout (shown as Clips).
    if (layoutui::devLayouts(a)) {
      formLabel("Plugin windows (Clips)");
      bool onLoad = st.getBool("live.pluginWindowsOnLoad"), several = st.getBool("live.pluginWindowsSeveral"),
           selOnly = st.getBool("live.pluginWindowsSelectedOnly");
      if (ImGui::Checkbox(i18n::tr("Open a plugin's window when it is put on a track"), &onLoad)) st.set("live.pluginWindowsOnLoad", onLoad);
      ui::Hint("Installed plugins you add by hand, one at a time. Opening a project, drum kits, scripts, AI tools, undo, redo and a collaborator's additions never open a window");
      formLabel("");
      if (ImGui::Checkbox(i18n::tr("Several plugin windows at once"), &several)) st.set("live.pluginWindowsSeveral", several);
      ui::Hint("Off: opening a plugin's window closes the other plugin windows");
      formLabel("");
      if (ImGui::Checkbox(i18n::tr("Show only the selected track's plugin windows"), &selOnly)) st.set("live.pluginWindowsSelectedOnly", selOnly);
      ui::Hint("Selecting another track hides the other tracks' plugin windows (they stay open) and shows its own again. "
               "Ctrl+Alt+P hides or shows every plugin window");
    }
    // The audio editor's auto-scroll (its own toggle, on the right of its waveform, sets the same thing).
    formLabel("Loupe");
    bool loupeFollow = st.getBool("loupe.autoScroll");
    if (ImGui::Checkbox(i18n::tr("Follow the playhead while playing"), &loupeFollow)) st.set("loupe.autoScroll", loupeFollow);
    ui::Hint("On: Loupe's waveform scrolls along with playback. Off: the view stays where you put it. "
             "The toggle beside Loupe's waveform changes the same setting");
    ui::EndSection();
    // J-67: motion (smooth snapping of drags, smooth scrolling, elastic edges).
    ui::BeginSection("MOTION");
    const bool reduced = scrollui::systemReducedMotion();
    ImGui::BeginDisabled(reduced);
    bool anim = st.getBool("ui.animations") && !reduced;
    formLabel("Animations");
    if (ImGui::Checkbox(i18n::tr("Interface animations"), &anim)) st.set("ui.animations", anim);
    ui::Hint("Decorative motion: the metronome ticking in tempo, key and pad lights fading, gliding drags, scrolling and zooming. "
             "Off: everything moves at once, which saves a little CPU");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(reduced || !anim);
    bool smoothSnap = st.getBool("ui.smoothSnap") && !reduced;
    bool smoothScroll = st.getBool("ui.smoothScroll") && !reduced, elastic = st.getBool("ui.elasticEdges") && !reduced;
    formLabel("Dragging");
    if (ImGui::Checkbox(i18n::tr("Smooth snapping"), &smoothSnap)) st.set("ui.smoothSnap", smoothSnap);
    ui::Hint("Snapped drags (clips, notes, selections, points, cut lines) glide from grid line to grid line");
    formLabel("Scrolling");
    if (ImGui::Checkbox(i18n::tr("Smooth scrolling"), &smoothScroll)) st.set("ui.smoothScroll", smoothScroll);
    ui::Hint("Mouse wheel steps glide to their row or step; trackpads follow the fingers directly");
    formLabel("");
    if (ImGui::Checkbox(i18n::tr("Elastic edges"), &elastic)) st.set("ui.elasticEdges", elastic);
    ui::Hint("Scrolling past the start or end of a view stretches a little and springs back");
    bool smoothZoom = st.getBool("ui.smoothZoom") && !reduced;  // J-188
    formLabel("Zooming");
    if (ImGui::Checkbox(i18n::tr("Smooth zooming"), &smoothZoom)) st.set("ui.smoothZoom", smoothZoom);
    ui::Hint("Zoom steps glide in the playlist, piano roll and automation editor, keeping the point under the pointer in place; "
             "trackpads follow the fingers directly");
    ImGui::EndDisabled();
    if (reduced) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Off while the system asks for reduced motion."));
      ImGui::PopTextWrapPos();
    }
    {  // J-83: what the plain wheel does over mixer faders and knobs; J-84: and over the channel rack's steps and knobs
      static const char* const kWheelModes[] = {i18n::N_("Adjust the control"), i18n::N_("Scroll the pane"),
                                                i18n::N_("Auto (scroll when the pane scrolls)")};
      const int cur = static_cast<int>(wheelroute::modeFromSetting(st.getNumber("ui.mixerWheel")));
      formLabel("Wheel over controls");
      if (ui::BeginCombo("##mixerwheel", i18n::tr(kWheelModes[cur]), fieldW)) {
        for (int i = 0; i < 3; ++i)
          if (ImGui::Selectable(i18n::tr(kWheelModes[i]), cur == i)) st.set("ui.mixerWheel", i);
        ImGui::EndCombo();
      }
      ui::Hint("Mouse wheel over controls in scrolling panes (narrow mixer, channel rack with a long pattern or many channels). "
               "Scrolling: the wheel scrolls the pane; drag, click then wheel, or hold Ctrl and wheel, to change a control or step");
    }
    ui::EndSection();
    // J-99: smooth freehand drawing (app/DrawSmoothUi.h).
    ui::BeginSection("DRAWING");
    {
      formLabel("Smooth drawing");
      float amt = static_cast<float>(std::clamp(st.getNumber("draw.smoothAmount"), 0.0, 1.0)) * 100.0f;
      ImGui::SetNextItemWidth(fieldW);
      if (ImGui::SliderFloat("##drawsmooth", &amt, 0.0f, 100.0f, amt <= 0 ? i18n::tr("Off") : "%.0f%%"))
        st.set("draw.smoothAmount", static_cast<double>(std::round(amt)) / 100.0);
      ui::Hint("How much strokes drawn into EQs, band levels, harmonics, waves and tables are smoothed. "
               "Each surface has its own on / off switch (the small wave in its corner); hold Alt while drawing for a raw stroke");
    }
    ui::EndSection();
    // J-48 interface sounds: moved to Settings > Theme > Sounds (J-133, SoundThemeUi.cpp).
  }
  if (page == 5) generalSettingsPage(a, fieldW);
  if (page == 8) shortcutSettingsPage(a, fieldW);
  if (page == 9) themeSettingsPage(a, fieldW);
  if (page == 6) fileSettingsPage(a, fieldW);
  if (page == 7) debugSettingsPage(a, fieldW);
  if (page == 10) mcpui::settingsPage(a, fieldW);  // SC-07
  if (page == 11) collabui::settingsPage(a, fieldW);  // J-111
  if (page == kPluginSettingsTab) pluginFoldersSettingsPage(a, fieldW);  // K-13
}

// K-112b: the Logic layout's settings tabs: a toolbar band of icon-over-label tiles, centred;
// the lit page is a raised tile with its icon and label in the accent colour.
void iconSettingsTabs(const char* const* labels, const ui::Icon* icons, int count, int* index, const char* const* hints = nullptr) {
  const auto& P = pal();
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  ImDrawList* dl = w->DrawList;
  const ImVec2 q0 = ImGui::GetCursorScreenPos();
  const float full = ImGui::GetContentRegionAvail().x, fs = theme::kFontCaption;
  const float tileH = 46.0f, bandPad = theme::kS2, minW = 56.0f, padX = theme::kS2 + 2, gap = 2.0f;
  const float gaps = gap * static_cast<float>(std::max(0, count - 1));
  ImGui::PushFont(nullptr, fs);
  std::vector<float> tw(static_cast<size_t>(count));
  float sum = 0;
  for (int i = 0; i < count; ++i) {
    tw[static_cast<size_t>(i)] = std::max(minW, std::ceil(ImGui::CalcTextSize(i18n::tr(labels[i])).x) + 2 * padX);
    sum += tw[static_cast<size_t>(i)];
  }
  if (sum + gaps > full) {  // too many tiles for the window: every tile narrows, labels fit with an ellipsis
    const float squeeze = std::max(0.3f, (full - gaps) / std::max(1.0f, sum));
    sum = 0;
    for (auto& v : tw) sum += (v = std::floor(v * squeeze));
  }
  sum += gaps;
  // The band runs edge to edge under the title bar; the tiles sit centred in it.
  const ImU32 bg = ImGui::GetColorU32(ImGuiCol_WindowBg);
  const float bandY0 = q0.y - ImGui::GetStyle().WindowPadding.y, bandY1 = q0.y + tileH + bandPad;
  dl->AddRectFilled(ImVec2(w->Pos.x, bandY0), ImVec2(w->Pos.x + w->Size.x, bandY1), theme::mix(bg, P.text, 0.045f));
  dl->AddLine(ImVec2(w->Pos.x, bandY1), ImVec2(w->Pos.x + w->Size.x, bandY1), P.dark ? theme::withAlpha(P.ink, 0.6f) : P.lineStrong);
  float x = std::round(q0.x + std::max(0.0f, (full - sum) * 0.5f));
  for (int i = 0; i < count; ++i) {
    const float wi = tw[static_cast<size_t>(i)];
    ImGui::SetCursorScreenPos(ImVec2(x, q0.y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##itab", ImVec2(wi, tileH))) *index = i;
    const bool hov = ImGui::IsItemHovered(), on = *index == i;
    ImGui::PopID();
    if (on || hov) dl->AddRectFilled(ImVec2(x, q0.y), ImVec2(x + wi, q0.y + tileH), theme::mix(bg, P.text, on ? 0.13f : 0.07f), 6.0f);
    const ImU32 col = on ? P.hot : hov ? P.text : P.textDim;
    ui::drawIconBox(dl, icons[i], ImVec2(std::round(x + wi * 0.5f), std::round(q0.y + 16.0f)), 20.0f, col);
    const char* t = i18n::tr(labels[i]);
    const char* shown = theme::fitText(t, wi - 4);
    const float lw = ImGui::CalcTextSize(shown).x;
    dl->AddText(ImVec2(std::round(x + (wi - lw) * 0.5f), std::round(q0.y + tileH - fs - 5.0f)), col, shown);
    if (hov && std::strcmp(shown, t) != 0) ui::Tooltip(labels[i]);
    else if (hov && hints) ui::Hint(hints[i]);  // checkpoint 28: what the page holds (the page itself no longer says)
    x += wi + gap;
  }
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(q0.x, bandY1 + theme::kS3));
  ImGui::Dummy(ImVec2(full, 0));
}

// K-112b: the Logic layout's settings: a row of segments under the tabs, one per section of the
// page (the references split each page into sub-tabs); a click scrolls to that section, and the
// segment of the section at the top is lit. Returns the y to scroll to, or -1.
float sectionJumpBar(const std::vector<ui::SectionMark>& marks, float scrollY) {
  if (marks.size() < 2) return -1;
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float full = ImGui::GetContentRegionAvail().x, h = 24.0f;
  float padX = theme::kS3, sum = 0;
  for (const auto& m : marks) sum += ImGui::CalcTextSize(m.heading.c_str()).x;
  const float n = static_cast<float>(marks.size());
  if (sum + 2 * padX * n + 4 > full) padX = std::floor((full - 4 - sum) / (2 * n));
  if (padX < theme::kS1) return -1;  // no room: the page scrolls as it is
  int cur = 0;
  for (size_t i = 0; i < marks.size(); ++i)
    if (marks[i].y <= scrollY + 8) cur = static_cast<int>(i);
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const float total = sum + 2 * padX * n + 4;
  float x = std::round(q.x + (full - total) * 0.5f);
  const ImU32 bg = ImGui::GetColorU32(ImGuiCol_WindowBg);
  dl->AddRectFilled(ImVec2(x, q.y), ImVec2(x + total, q.y + h), theme::mix(bg, P.text, 0.06f), 7.0f);
  x += 2;
  float jump = -1;
  for (size_t i = 0; i < marks.size(); ++i) {
    const char* t = marks[i].heading.c_str();
    const float tw = ImGui::CalcTextSize(t).x, sw = tw + 2 * padX;
    ImGui::SetCursorScreenPos(ImVec2(x, q.y));
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::InvisibleButton("##jump", ImVec2(sw, h))) jump = marks[i].y;
    const bool hov = ImGui::IsItemHovered(), on = static_cast<int>(i) == cur;
    ImGui::PopID();
    if (on || hov) dl->AddRectFilled(ImVec2(x, q.y + 2), ImVec2(x + sw, q.y + h - 2), theme::mix(bg, P.text, on ? 0.16f : 0.10f), 5.0f);
    dl->AddText(ImVec2(std::round(x + padX), std::round(q.y + (h - ImGui::GetFontSize()) * 0.5f)), on || hov ? P.text : P.textDim, t);
    x += sw;
  }
  ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + h + theme::kS2));
  ImGui::Dummy(ImVec2(full, 0));
  return jump;
}

void settingsWindow(App& a) {
  if (!a.showSettings) return;
  // K-112c (round 29): Live's settings window is a little larger, so the value column can sit about
  // a third across the page (427, 073, 428) and the longest rows still fit.
  centerNext(livelook::active(a) ? ImVec2(980, 600) : ImVec2(860, 560), ImVec2(660, 360));
  if (gDevSettingsSize.x > 0) ImGui::SetNextWindowSize(gDevSettingsSize, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin(ui::windowTitle("Settings").c_str(), &a.showSettings, layoutui::dockFlags("Settings", ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse));
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  const auto& P = pal();
  // Studio settings form: a tab strip across the top (the lit tab is the
  // page), then the page's controls grouped in recessed panels.
  static int page = 0;
  if (a.settingsTab >= 0) {
    page = a.settingsTab;
    a.settingsTab = -1;
  }
  // J-24 appended General, Files and Debug (tab indices 0..4 stay as they were).
  // J-55 appended Shortcuts and Theme.
  const char* blurbs[] = {i18n::N_("Output and input devices, sample rate, buffer and recording"), i18n::N_("Controllers, keyboards and MIDI out"),
                          i18n::N_("Defaults for new projects"), i18n::N_("Automatic backups, autosave and restoring a backup"), i18n::N_("Theme, size and hints"),
                          i18n::N_("Startup, default template, language and updates"), i18n::N_("Folders and scripting"), i18n::N_("Log and script console"),
                          i18n::N_("Every keyboard shortcut: search, change, reset"), i18n::N_("Appearance (dark or light, colours, contrast), control style and sounds"),
                          i18n::N_("Let AI assistants (MCP clients) inspect and edit the project"),
                          i18n::N_("Your name, contact and picture for collaborators, identity and connection"),
                          i18n::N_("Where each plugin format is searched, and scanning")};
  constexpr int kPages = 13;  // K-13 appended Plugins (kPluginSettingsTab)
  const char* pages[kPages];
  for (int i = 0; i < kPages; ++i) pages[i] = i18n::tr(kSettingsPagesEn[i]);
  page = std::clamp(page, 0, kPages - 1);
  // K-112b: the Logic layout's settings: icon tabs in a toolbar band, labels right-aligned to the controls.
  const bool iconTabs = layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic);
  if (livelook::active(a)) {
    // K-112: the Live way: the pages listed down the left, the open one on a lighter face, a line
    // between the list and the page.
    const float fs = theme::kFontBody, rowH = 30;
    float listW = 0;
    for (int i = 0; i < kPages; ++i) listW = std::max(listW, a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, pages[i]).x);
    listW = std::clamp(std::round(listW + 2 * theme::kS4), 140.0f, 220.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##pagelist", ImVec2(listW, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImDrawList* ldl = ImGui::GetWindowDrawList();
    for (int i = 0; i < kPages; ++i) {
      const ImVec2 q = ImGui::GetCursorScreenPos();
      ImGui::PushID(i);
      const bool click = ImGui::InvisibleButton("##pg", ImVec2(listW, rowH));
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      if (i == page) ldl->AddRectFilled(q, ImVec2(q.x + listW, q.y + rowH), P.surface700);
      else if (hov) ldl->AddRectFilled(q, ImVec2(q.x + listW, q.y + rowH), P.surface800);
      ImFont* lf = i == page ? a.fontBold : a.fontRegular;
      theme::textEllipsis(ldl, lf, fs, ImVec2(q.x + theme::kS4, theme::textTop(lf, fs, q.y, rowH)),
                          listW - 2 * theme::kS4 + 4, i == page || hov ? P.text : P.textDim, pages[i]);
      ui::Hint(blurbs[i]);
      if (click) page = i;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    const ImVec2 lmax = ImGui::GetItemRectMax(), lmin = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(lmax.x + 0.5f, lmin.y), ImVec2(lmax.x + 0.5f, lmax.y), P.dark ? P.ink : P.line);
    ImGui::SameLine(0, theme::kS4);
  } else if (iconTabs) {
    using I = ui::Icon;
    static const I kIcons[kPages] = {I::Wave, I::CatMidi, I::ProjectFile, I::Clock,     I::Monitor, I::Settings, I::Folder,
                                     I::Info, I::Keyboard, I::Paint,      I::LinkSlots, I::People,  I::Plug};
    iconSettingsTabs(kSettingsPagesEn, kIcons, kPages, &page, blurbs);
  } else {
    ui::TabStrip("##settingstabs", pages, kPages, &page);
  }
  static std::vector<ui::SectionMark> marks;  // the page's sections, as last drawn
  static int marksPage = -1;
  static float pageScroll = 0, jumpTo = -1;
  // Checkpoint 28 (Tracks layout): the page sits in an inset panel and the sub-tabs straddle its top
  // edge, as 023, 100 and 083 draw the settings pages (they stood on the window with no panel).
  ImDrawList* wdl = ImGui::GetWindowDrawList();
  const float barTop = ImGui::GetCursorScreenPos().y;
  if (iconTabs) wdl->ChannelsSplit(2), wdl->ChannelsSetCurrent(1);
  if (iconTabs && marksPage == page)
    if (const float y = sectionJumpBar(marks, pageScroll); y >= 0) jumpTo = y;
  bool panel = false;
  if (iconTabs) {
    const bool bar = ImGui::GetCursorScreenPos().y > barTop + 1;
    const ImVec2 c0(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, bar ? barTop + 12.0f : barTop);
    const ImVec2 c1(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x,
                    ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y);
    wdl->ChannelsSetCurrent(0);
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_WindowBg);
    wdl->AddRectFilled(c0, c1, theme::mix(bg, P.dark ? P.ink : P.text, P.dark ? 0.22f : 0.04f), 6.0f);
    wdl->AddRect(ImVec2(c0.x + 0.5f, c0.y + 0.5f), ImVec2(c1.x - 0.5f, c1.y - 0.5f), theme::mix(bg, P.text, 0.10f), 6.0f);
    wdl->ChannelsMerge();
    if (!bar) ImGui::Dummy(ImVec2(0, theme::kS2));
    panel = true;
  }
  if (panel) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, 0));
  ImGui::BeginChild("##page", ImVec2(0, panel ? -theme::kS2 : 0.0f), panel ? ImGuiChildFlags_AlwaysUseWindowPadding : ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  if (panel) ImGui::PopStyleVar();
  if (jumpTo >= 0) ImGui::SetScrollY(jumpTo), jumpTo = -1;
  pageScroll = ImGui::GetScrollY();
  // K-112: Live starts at the first group's name (the list rows say what each page holds); checkpoint 28: so does
  // the Tracks layout, whose icon tabs and sub-tabs name the page (no reference page has a description line).
  if (!livelook::active(a) && !iconTabs) {
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr(blurbs[page]));
    ImGui::Dummy(ImVec2(0, theme::kS1));
  }
  float fieldW = std::max(160.0f, std::min(320.0f, ImGui::GetContentRegionAvail().x - 176 - 2 * theme::kS2 - 12));
  float liveCol = 0;
  if (livelook::active(a)) {  // K-112c (round 28): values about halfway across the page (427, 073, 428)
    const float avail = ImGui::GetContentRegionAvail().x;
    liveCol = std::clamp(std::round(avail * 0.34f), 168.0f, 280.0f);
    fieldW = std::clamp(std::min(std::round(avail * 0.32f), avail - liveCol - 16.0f), 160.0f, 260.0f);  // choosers about a third of the page wide
  }
  ui::SetFormColumn(iconTabs ? 168.0f : 0.0f);  // formLabel's column
  ui::SetSettingsColumn(liveCol);
  ui::SectionMarksReset();
  settingsPageBody(a, page, fieldW);
  if (iconTabs) marks = ui::SectionMarks(), marksPage = page;
  ui::SetSettingsColumn(0);
  ui::SetFormColumn(0);
  ui::SectionMarksReset();
  ImGui::EndChild();
  ImGui::End();
}

int pendingPpq = 0;  // Project info: a lower timebase waiting for confirmation

void infoWindow(App& a) {
  if (!a.showInfo) return;
  // K-112b: in the Tracks layout this is the project's settings window, as the references have it
  // (098, 096, 132, 215): titled with the project's name, its pages under icon tabs.
  const bool lg = logicplug::active(a);
  centerNext(ImVec2(480, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  std::string title = ui::windowTitle("Project info");
  if (lg) {
    std::string proj = a.projectFile().empty() ? i18n::tr("Untitled project") : fs::path(a.projectFile()).stem().string();
    // Checkpoint 28: the project's name alone ("song", not "song.gridline"), as 098 and 096 title it.
    if (const size_t dot = proj.rfind(".gridline"); dot != std::string::npos && dot + 9 == proj.size() && dot > 0) proj.erase(dot);
    title = proj + " - " + i18n::tr("Project settings") + "###Project info";
    ImGui::SetNextWindowSizeConstraints(ImVec2(500, 0), ImVec2(900, 2000));
  }
  bool vis = ImGui::Begin(title.c_str(), &a.showInfo, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  const Song& s = a.doc.song();
  SongInfo info = s.info;
  static int page = 0;
  if (lg) {
    static const char* const kPagesEn[] = {i18n::N_("General"), i18n::N_("Timing"), i18n::N_("Contents")};
    static const ui::Icon kPageIcons[] = {ui::Icon::ProjectFile, ui::Icon::Metronome, ui::Icon::ListBullets};
    iconSettingsTabs(kPagesEn, kPageIcons, 3, &page);
  }
  const bool showGeneral = !lg || page == 0, showTiming = !lg || page == 1, showContents = !lg || page == 2;
  ui::SetFormColumn(lg ? 120.0f : 0.0f);  // K-112b: the Tracks layout's form (labels end at the controls)
  bool changed = false;
  auto field = [&](const char* label, std::string& v, bool multiline = false) {
    char buf[2048];
    std::snprintf(buf, sizeof buf, "%s", v.c_str());
    formLabel(label, 120);
    std::string id = std::string("##") + label;
    // K-112b (second pass): in the Tracks layout the fields run to the group's right edge, so the
    // form's right margin matches its left one.
    const float fw = lg ? std::floor(ImGui::GetContentRegionAvail().x) : 324.0f;
    bool ch = multiline ? ImGui::InputTextMultiline(id.c_str(), buf, sizeof buf, ImVec2(fw, 90))
                        : (ImGui::SetNextItemWidth(fw), ImGui::InputText(id.c_str(), buf, sizeof buf));
    ui::FocusRing();
    if (ch) v = buf, changed = true;
  };
  if (showGeneral) {
  ui::BeginSection("PROJECT", 464);
  collabui::projectNameRow(a, 120);  // J-111f: a collaborative project's shared name (Rename...)
  field("Title", info.title);
  field("Author", info.author);
  field("Genre", info.genre);
  field("URL", info.url);
  field("Comments", info.comments, true);
  if (changed) a.doc.setInfo(info);
  if (s.info.created > 0 || s.info.workTime > 0) {  // imported projects carry these (FL creation date, time spent)
    if (s.info.created > 0) {
      std::time_t t = static_cast<std::time_t>(s.info.created);
      char date[64] = "";
      if (const std::tm* tm = std::localtime(&t)) std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", tm);
      formLabel("Created", 120);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(date);
    }
    if (s.info.workTime > 0) {
      const long long m = static_cast<long long>(s.info.workTime / 60);
      formLabel("Time spent", 120);
      ImGui::AlignTextToFramePadding();
      ImGui::Text(i18n::tr("%lld h %02lld min"), m / 60, m % 60);
    }
  }
  ui::EndSection();
  }
  if (showTiming) {
  ui::BeginSection("TIMING", 464);
  formLabel("Meter at the start", 120);
  int num = s.timeSigNum, den = s.timeSigDen;
  static bool alsoPatterns = false;  // K-179: opt-in, off: existing patterns keep their meter and length
  if (ui::BeginCombo("##num", std::to_string(num).c_str(), 64)) {
    for (int n = 1; n <= 16; ++n)
      if (ImGui::Selectable(std::to_string(n).c_str(), n == num)) a.doc.setTimeSignature(n, den, alsoPatterns);
    ImGui::EndCombo();
  }
  ImGui::SameLine(0, theme::kS2);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textDim), "/");
  ImGui::SameLine(0, theme::kS2);
  const int dens[] = {2, 4, 8, 16};
  if (ui::BeginCombo("##den", std::to_string(den).c_str(), 64)) {
    for (int d : dens)
      if (ImGui::Selectable(std::to_string(d).c_str(), d == den)) a.doc.setTimeSignature(num, d, alsoPatterns);
    ImGui::EndCombo();
  }
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::SameLine(120);
  ImGui::Checkbox(i18n::tr("Also change the meter of existing patterns"), &alsoPatterns);
  ui::Hint("Off: existing patterns keep their meter and length when the meter at the start changes, so nothing moves");
  if (const int changes = meter::changeCount(s); changes > 0) {
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::SameLine(120);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(pal().textFaint), i18n::tr("Meter changes later in the song: %d (edit them in the playlist ruler)"), changes);
  }
  formLabel("Timebase", 120);
  if (ui::BeginCombo("##ppq", (std::to_string(s.ppq) + " PPQ").c_str(), 136)) {
    for (int p : kPpqChoices)
      if (ImGui::Selectable(std::to_string(p).c_str(), p == s.ppq)) {
        if (p < s.ppq && countPpqRoundings(s, p) > 0)
          pendingPpq = p, ImGui::OpenPopup("##ppqwarn");
        else
          a.doc.setPpq(p);
      }
    ImGui::EndCombo();
  }
  ui::Hint("Ticks per quarter note: the finest note position the project can store");
  if (pendingPpq) ImGui::OpenPopup("##ppqwarn");
  // J-54: laid out like the other confirmation prompts (title, dim body, Cancel + lit action on the right).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  const bool warn = ImGui::BeginPopupModal("##ppqwarn", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (warn) {
    char sub[160];
    std::snprintf(sub, sizeof sub, "%d note or clip position(s) move onto the coarser grid. You can undo this.", countPpqRoundings(s, pendingPpq));
    char title[64];
    std::snprintf(title, sizeof title, "Lower the timebase to %d PPQ?", pendingPpq);
    dialogTitle(a, title, sub);
    ImGui::Dummy(ImVec2(372, theme::kS2));
    alignRight(ui::ButtonRowWidth(2, 110));
    const bool cancel = ui::TextButton("Cancel", false, 110) || ::gl::keys::escapePressed();
    ImGui::SameLine(0, theme::kS2);
    if (ui::PrimaryButton("Change PPQ", 110) || ::gl::keys::defaultPressed()) {
      a.doc.setPpq(pendingPpq);
      pendingPpq = 0;
      ImGui::CloseCurrentPopup();
    } else if (cancel) {
      pendingPpq = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ui::EndSection();
  }
  size_t notes = 0;
  for (const auto& p : s.patterns)
    for (const auto& [c, ns] : p.notes) notes += ns.size();
  if (!lg) {
    ImGui::TextDisabled(i18n::tr("%zu channels · %zu patterns · %zu clips · %zu notes · %zu mixer tracks"), s.channels.size(),
                        s.patterns.size(), s.playlist.size(), notes, s.mixer.size());
  } else if (showContents) {
    // K-112b: what the project holds, one row a kind, the counts right-aligned (ref 215), in the
    // same section box as the other pages.
    const float x0 = ImGui::GetCursorPosX();
    ui::BeginSection("CONTENTS", 464);
    // The table spans the box: it starts at the box's padding, not at the form's control column.
    const float back = std::max(0.0f, ImGui::GetCursorPosX() - x0 - theme::kS2);
    if (back > 0) ImGui::Unindent(back);
    if (ImGui::BeginTable("##projcount", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("##kind", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##count", ImGuiTableColumnFlags_WidthFixed, 90);
    const std::pair<const char*, size_t> rows[] = {{i18n::N_("Channels"), s.channels.size()}, {i18n::N_("Patterns"), s.patterns.size()},
                                                   {i18n::N_("Clips"), s.playlist.size()},       {i18n::N_("Notes"), notes},
                                                   {i18n::N_("Mixer tracks"), s.mixer.size()},   {i18n::N_("Markers"), s.markers.size()}};
    for (const auto& [name, n] : rows) {
      ImGui::TableNextRow(ImGuiTableRowFlags_None, 24);
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(i18n::tr(name));
      ImGui::TableNextColumn();
      const std::string v = std::to_string(n);
      ImGui::AlignTextToFramePadding();
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(v.c_str()).x - 6);
      ImGui::TextUnformatted(v.c_str());
    }
    ImGui::EndTable();
    }
    if (back > 0) ImGui::Indent(back);
    ui::EndSection();
  }
  ui::SetFormColumn(0);
  ui::SectionMarksReset();  // only the settings window uses them
  ImGui::End();
}

void historyWindow(App& a) { undoui::historyWindow(a); }  // K-141

void aboutWindow(App& a) {
  if (!a.showAbout) return;
  // Logo guide §08: the lockup in Paper on surface-950 at 56 px, with at
  // least 24u (one fifth of the mark) of clear space on every side.
  const float lockH = 56.0f, clear = lockH * 0.2f;
  const float lockW = theme::lockupWidth(lockH, a.fontLarge);
  const float pad = theme::kS6 + theme::kS2;
  const float width = std::max(420.0f, lockW + 2 * (pad + clear));
  centerNext(ImVec2(width, 0));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, 2000));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::rgb(0x0B0B0B)));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, theme::vec(theme::rgb(0x0B0B0B)));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, theme::vec(theme::rgb(0x0B0B0B)));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::rgb(0xEDEDED)));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(theme::rgb(0x262626)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  bool vis = ImGui::Begin(ui::windowTitle("About Gridline").c_str(), &a.showAbout, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (vis) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    theme::drawLockup(dl, ImVec2(std::round(p.x + (cw - lockW) * 0.5f), p.y + clear), lockH, theme::rgb(0xFFFFFF), theme::rgb(0x0B0B0B),
                      a.fontLarge);
    ImGui::Dummy(ImVec2(cw, lockH + clear * 2));
    auto centred = [&](ImFont* f, float fs, ImU32 col, const char* t) {
      ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0, t);
      ImVec2 q = ImGui::GetCursorScreenPos();
      dl->AddText(f, fs, ImVec2(std::round(q.x + (cw - ts.x) * 0.5f), q.y), col, t);
      ImGui::Dummy(ImVec2(cw, fs + 4));
    };
    centred(a.fontMono, theme::kFontLabel, theme::rgb(0x8A8A8A), "THE GRID, CUT CLEAN");
    ImGui::Dummy(ImVec2(0, theme::kS3));
    centred(a.fontBold, theme::kFontBody, theme::rgb(0xEDEDED), "Version " GRIDLINE_VERSION);
    centred(a.fontRegular, theme::kFontCaption + 1, theme::rgb(0xA3A3A3), "A free, open, pattern-based DAW for everybody.");
    ImGui::Dummy(ImVec2(0, theme::kS3));
    ImVec2 lp = ImGui::GetCursorScreenPos();
    dl->AddLine(lp, ImVec2(lp.x + cw, lp.y), theme::rgb(0x262626));
    ImGui::Dummy(ImVec2(cw, theme::kS3));
    centred(a.fontRegular, theme::kFontCaption, theme::rgb(0x707070), "Dear ImGui (MIT) \xC2\xB7 SDL2 (zlib) \xC2\xB7 RtMidi (MIT) \xC2\xB7 dr_libs (MIT-0)");
    centred(a.fontRegular, theme::kFontCaption, theme::rgb(0x707070), "Signalsmith Stretch (MIT) \xC2\xB7 Rubber Band Library (GPL-2.0-or-later)");
    centred(a.fontRegular, theme::kFontCaption, theme::rgb(0x707070), "Inter and JetBrains Mono (SIL Open Font License)");
  }
  ImGui::End();
  ImGui::PopStyleColor(5);
}

void shortcutsWindow(App& a) {
  if (!a.showShortcuts) return;
  centerNext(ImVec2(560, 600), ImVec2(420, 300));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin(ui::windowTitle("Keyboard shortcuts").c_str(), &a.showShortcuts, layoutui::dockFlags("Keyboard shortcuts", ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse));
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  shortcutReference(a);  // J-55: generated from the keymap (always the current bindings)
  ImGui::End();
}

void unsavedPrompt(App& a) {
  if (a.confirmDiscard) {
    ImGui::OpenPopup("##unsaved");
    a.confirmDiscard = false;
  }
  centerNext(ImVec2(420, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  bool open = false;
  {
    const DialogLook look;
    open = ImGui::BeginPopupModal("##unsaved", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  }
  ImGui::PopStyleVar();
  if (open) {
    dialogFrame();
    dialogTitle(a, "Save changes to this project?", "Your changes will be lost if you don't save them.");
    ImGui::Dummy(ImVec2(372, theme::kS2));
    alignRight(110 + 90 + 100 + theme::kS2 * 2);
    if (dialogButton(a, i18n::N_("Don't save"), 110, false)) {
      ImGui::CloseCurrentPopup();
      if (a.afterDiscard) a.afterDiscard();
    }
    ImGui::SameLine(0, theme::kS2);
    if (dialogButton(a, "Cancel", 90, false) || ::gl::keys::escapePressed()) {
      a.afterDiscard = nullptr;
      a.quitRequested = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, theme::kS2);
    if (dialogButton(a, "Save", 100, true) || ::gl::keys::defaultPressed()) {
      ImGui::CloseCurrentPopup();
      auto then = a.afterDiscard;
      if (!a.projectPath.empty()) {
        a.saveProjectAs(a.projectPath, a.projectFormat, then);
      } else {
        a.saveAs(then);  // J-52: with the format list (an opened .flp asks too)
      }
    }
    ImGui::EndPopup();
  }
}

// J-82: one-question prompt in the unsaved-changes style (title, one dim
// line, Cancel + the action). Title and line come translated; the button
// label is English (marked N_), translated by TextButton.
void confirmPrompt(App& a) {
  auto& c = a.confirm;
  if (c.open) {
    ImGui::OpenPopup("##confirm");
    c.open = false;
  }
  centerNext(ImVec2(420, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  bool open = false;
  {
    const DialogLook look;
    open = ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  }
  ImGui::PopStyleVar();
  if (!open) return;
  dialogFrame();
  ImGui::PushFont(a.fontBold, trackerDialogs() ? ImGui::GetFontSize() : theme::kFontTitle);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 372);
  ImGui::TextUnformatted(c.title.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  if (!c.subtitle.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 372);
    ImGui::TextColored(theme::vec(pal().textDim), "%s", c.subtitle.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Dummy(ImVec2(372, theme::kS1 + theme::kS2));
  const bool third = !c.noLabel.empty();
  const float okW = third ? 120.0f : 100.0f;
  alignRight(90 + okW + theme::kS2 + (third ? okW + theme::kS2 : 0.0f));
  if (dialogButton(a, "Cancel", 90, false) || ::gl::keys::escapePressed()) {
    c.action = c.noAction = nullptr;
    c.noLabel.clear();
    ImGui::CloseCurrentPopup();
  }
  if (third) {
    ImGui::SameLine(0, theme::kS2);
    if (dialogButton(a, c.noLabel.c_str(), okW, false) || ImGui::IsKeyPressed(ImGuiKey_N, false)) {
      auto run = std::move(c.noAction);
      c.action = c.noAction = nullptr;
      c.noLabel.clear();
      ImGui::CloseCurrentPopup();
      if (run) run();
    }
  }
  ImGui::SameLine(0, theme::kS2);
  if (dialogButton(a, c.okLabel.empty() ? "OK" : c.okLabel.c_str(), okW, true) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
    auto run = std::move(c.action);
    c.action = c.noAction = nullptr;
    c.noLabel.clear();
    ImGui::CloseCurrentPopup();
    if (run) run();
  }
  ImGui::EndPopup();
}

}  // namespace

// Summary after importing an FL Studio project: what came across, what was
// approximated, what has no Gridline equivalent yet.
void importReportWindow(App& a) {
  if (!a.showImportReport) return;
  centerNext(ImVec2(560, 520), ImVec2(440, 300));
  bool vis = ImGui::Begin(ui::windowTitle("FL Studio import").c_str(), &a.showImportReport, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  if (vis) {
    const auto& P = pal();
    const auto& r = a.importReport;
    dialogTitle(a, r.source.c_str(), "Opened as a Gridline project. Save asks where to keep it, and in which format.");
    ImGui::BeginChild("##rep", ImVec2(0, -ui::DialogFooterHeight()));
    // J-54: one number column for both lists, as wide as the longest label
    // allows (labels beyond ~62% of the width are fitted, full text on hover).
    std::vector<const char*> labels;
    for (const auto& kv : r.counts) labels.push_back(kv.first.c_str());
    for (const auto& kv : r.unmapped) labels.push_back(kv.first.c_str());
    for (const auto& kv : r.mapped) labels.push_back(kv.first.c_str());
    const float sectionPad = 2 * theme::kS2;
    const float numX = ui::FormColumn(labels.data(), static_cast<int>(labels.size()), 168.0f,
                                      std::max(168.0f, (ImGui::GetContentRegionAvail().x - 2 * sectionPad) * 0.62f), theme::kS6);
    ui::BeginSection("IMPORTED");
    for (const auto& [k, v] : r.counts) {
      formLabel(k.c_str(), numX);
      ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
      ImGui::Text("%d", v);
      ImGui::PopFont();
    }
    ui::EndSection();
    if (!r.mapped.empty()) {  // J-94: which Gridline plugin each FL plugin became
      ui::BeginSection("PLUGINS");
      for (const auto& [k, v] : r.mapped) {
        formLabel(k.c_str(), numX);
        ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
        ImGui::Text("\xC3\x97%d", v);
        ImGui::PopFont();
      }
      ui::EndSection();
    }
    if (!r.unmapped.empty()) {
      ui::BeginSection("NO GRIDLINE EQUIVALENT YET");
      for (const auto& [k, v] : r.unmapped) {
        formLabel(k.c_str(), numX);
        ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
        ImGui::TextColored(theme::vec(P.danger), "\xC3\x97%d", v);
        ImGui::PopFont();
      }
      ui::EndSection();
    }
    if (const auto miss = missingFiles(a.doc.song()); !miss.empty()) {  // J-20: link to the files that did not come across
      ui::BeginSection("MISSING FILES");
      ImGui::TextColored(theme::vec(P.danger), i18n::tr("%zu sample(s) / file(s) could not be found"), miss.size());
      for (size_t k = 0; k < miss.size() && k < 6; ++k)
        ImGui::TextColored(theme::vec(P.textDim), "\xC2\xB7 %s", theme::fitText(miss[k].c_str(), ImGui::GetContentRegionAvail().x - 12));
      if (miss.size() > 6) ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("and %zu more"), miss.size() - 6);
      if (ui::TextButton("Locate missing samples...", false, 190)) exportui::openMissingSamples(a);
      ui::EndSection();
    }
    if (!r.warnings.empty()) {
      ui::BeginSection("NOTES");
      ImGui::PushTextWrapPos(0);
      for (const auto& w : r.warnings) ImGui::TextColored(theme::vec(P.textDim), "\xC2\xB7 %s", w.c_str());
      ImGui::PopTextWrapPos();
      ui::EndSection();
    }
    ImGui::EndChild();
    ui::DialogFooter(ui::ButtonRowWidth(2, 96));
    if (ui::TextButton("Close", false, 96)) a.showImportReport = false;
    ImGui::SameLine(0, theme::kS2);
    if (ui::PrimaryButton("Save as...", 96)) {
      a.saveAs();
      a.showImportReport = false;
    }
  }
  ImGui::End();
}

void devShowPpqWarning(App& a, int ppq) {
  a.showInfo = true;
  pendingPpq = ppq;
}

int settingsPageCount() { return static_cast<int>(sizeof kSettingsPagesEn / sizeof kSettingsPagesEn[0]); }
const char* settingsPageNameEn(int page) { return page >= 0 && page < settingsPageCount() ? kSettingsPagesEn[page] : ""; }
void drawSettingsPage(App& a, int page, float fieldW) {
  int p = page;
  settingsPageBody(a, p, fieldW);
}

void drawDialogs(App& a) {
  ImGui::GetStyle().HoverDelayNormal = static_cast<float>(std::clamp(a.settings.getNumber("ui.hintDelay"), 0.0, 2.0));
  drawTemplateDialogs(a);  // J-24: new project from template, save as template
  j59::drawDialogs(a);     // J-59: loop starter, render to sampled instrument
  flpexport::draw(a);      // J-80: FL Studio export progress and report
  updatePerformance(a);
  drawPerformance(a);
  grooveui::drawEditor(a);  // K-151
  meterui::drawEditor(a);   // K-179: meter change editor
  settingsWindow(a);
  infoWindow(a);
  exportui::draw(a);
  stemsui::draw(a);  // J-191: Split stems
  receiptui::draw(a);  // J-219: Track Receipt
  historyWindow(a);
  newtracks::draw(a);  // K-112b: the Tracks layout's New tracks dialog
  aboutWindow(a);
  shortcutsWindow(a);
  unsavedPrompt(a);
  confirmPrompt(a);           // J-82: delete clip sources, …
  speechui::draw(a);          // K-107: Speech
  ui::drawDisplayConfirm(a);  // J-77: Keep / Revert for size, theme, language, layout and audio changes
  updui::draw(a);             // J-89: update check (startup / Help), its popup
  suiteui::draw(a);           // J-131: Gridline Suite, first-launch question
  urlimportui::draw(a);       // J-192: Import from URL
  importReportWindow(a);
  drawPluginManager(a);
  drawRemoteDialog(a);
  j31::drawWindows(a);  // J-31: chord progression editor, audio logger, MIDI scale snap
  recordcard::draw(a);  // K-123: the Recording card
  drumkitui::draw(a);   // K-162: drum kits read on a worker, their import report
  drawProjectScriptWindow(a);  // J-57: project script settings / run
  mcpui::tick(a);                // SC-07: AI assistant server (opt-in, loopback only)
  oscui::tick(a);                // OSC server (opt-in, this computer only unless the network is switched on)
  mcpui::drawActivityWindow(a);
  filedlgui::draw(a);  // the Open / Save dialog (FileDialogUi.h)
}

namespace {
struct WheelGesture {
  bool open = false;  // the pointer gesture was opened by the wheel
  double at = 0;      // last wheel step (ImGui's frame clock: --fixed-dt makes it exact)
  int frame = 0;      // the frame of the last wheel step
};
WheelGesture& wheelGesture(const App& a) {
  static std::map<const App*, WheelGesture> m;  // (Gridline as a plugin: one App per instance)
  return m[&a];
}
// A wheel turn ends after both this long and this many frames without a wheel step. The frame
// count keeps one turn together on a slow or stalled frame rate (a frame that takes longer than
// the pause cannot show that the wheel stopped); the time keeps a real pause between turns.
constexpr double kWheelGestureIdle = 0.4;
constexpr int kWheelGestureIdleFrames = 8;
}  // namespace

void pointerGestureFrame(App& a) {
  const ImGuiIO& io = ImGui::GetIO();
  WheelGesture& w = wheelGesture(a);
  if (ImGui::IsAnyMouseDown()) {
    if (w.open) a.doc.endPointerGesture(), w.open = false;  // a press after a wheel turn is a gesture of its own
    if (!a.doc.inPointerGesture()) a.doc.beginPointerGesture();
  } else if (io.MouseWheel != 0 || io.MouseWheelH != 0) {
    if (!a.doc.inPointerGesture()) a.doc.beginPointerGesture(), w.open = true;
    if (w.open) w.at = ImGui::GetTime(), w.frame = ImGui::GetFrameCount();
  }
}

namespace {
// After the frame's editors ran: the release (or a pause of the wheel) ends the gesture.
void endPointerGesture(App& a) {
  if (ImGui::IsAnyMouseDown() || !a.doc.inPointerGesture()) return;
  WheelGesture& w = wheelGesture(a);
  if (w.open) {
    const double now = ImGui::GetTime();
    const int frames = ImGui::GetFrameCount() - w.frame;
    const bool idle = (now - w.at >= kWheelGestureIdle || now < w.at) && (frames >= kWheelGestureIdleFrames || frames < 0);
    if (!idle) return;
  }
  a.doc.endPointerGesture();
  w.open = false;
}
}  // namespace

void pointerGestureFrameEnd(App& a) { endPointerGesture(a); }

namespace {
// K-124: the window is one of the editors or plugin windows Esc leaves for the playlist.
bool editorWindow(const ImGuiWindow* w) {
  if (!w) return false;
  const char* full = w->RootWindow ? w->RootWindow->Name : w->Name;
  const char* id = ui::stableWindowName(full);
  for (const char* n : {"Piano roll", "Channel rack", "Mixer", "Loupe"})
    if (!std::strcmp(id, n)) return true;
  for (const char* tag : {"###autoedit", "###channelsettings", "###fxeditor", "###edison-standalone-"})
    if (std::strstr(full, tag)) return true;
  return false;
}
bool editorWindowFocused() { return editorWindow(GImGui->NavWindow); }

// K-297: the window a key closes: the focused floating window (not docked in a layout) that has a
// close button. Esc (dialogsOnly) closes dialogs and tool windows: one that never docks (About,
// Render, Project info, a tool's window), or one of the panels that only ever open over the
// layout; never an editor or plugin window (Esc there brings the playlist forward, K-124) and
// never a panel that is part of a layout (the playlist, mixer or browser floating). Cmd+W
// (window.close) closes any of them. It closes on the window's next Begin(), as its close
// button does (GRIDLINE PATCH K-297 in imgui.cpp).
bool closableWindow(const ImGuiWindow* w, bool dialogsOnly) {
  if (!w || !w->HasCloseButton || !(w->Active || w->WasActive) || w->Hidden || w->DockIsActive || w->Collapsed) return false;
  if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Modal)) return false;
  if (w->Name[0] == '#' && w->Name[1] == '#') return false;
  if (!dialogsOnly) return true;
  if (editorWindow(w) || std::strstr(w->Name, "###fxmore")) return false;
  if (w->Flags & ImGuiWindowFlags_NoDocking) return true;
  const char* id = ui::stableWindowName(w->Name);
  for (const char* n : {"Settings", "Keyboard shortcuts", "Plugin manager", "Project info", "Undo history", "Performance", "Sync review"})
    if (!std::strcmp(id, n)) return true;
  return false;
}

// K-297 (round 4, K-301 R04-07): Gridline's own chrome holds the focus (the top bar, a layout's
// toolbar or menu strip, the status bar: windows that draw their own title or none, "##..."), or
// nothing does: the key goes to the front-most window it may close, as the reference's Esc goes
// to the dialog in front. A click on the top bar's menu or transport after a dialog opened (the
// About item, Smooth's own menu strip) left Esc with nowhere to go.
bool chromeWindow(const ImGuiWindow* w) {
  return w && w->Name[0] == '#' && w->Name[1] == '#' && !(w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal | ImGuiWindowFlags_ChildMenu));
}

// K-297 (round 6): the bars themselves (the top bar, the status bar, the dock space behind the
// windows, the splash): never the place for the keys. (Other "##" windows, such as the command
// palette, a crash report or the launcher, are real places to type.)
bool barWindow(const ImGuiWindow* w) {
  if (!chromeWindow(w)) return false;
  for (const char* n : {"##topbar", "##status", "##dockhost", "##splash"})
    if (!std::strcmp(w->Name, n)) return true;
  return false;
}

ImGuiWindow* closableFocusedWindow(bool dialogsOnly) {
  ImGuiContext& g = *GImGui;
  const ImGuiWindow* nav = g.NavWindow;
  ImGuiWindow* w = nav ? nav->RootWindow : nullptr;
  while (w && (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Tooltip)) && w->ParentWindow)
    w = w->ParentWindow->RootWindow;  // (a popup of the window: the popup goes first, by itself)
  if (w && !w->Active && !w->WasActive) w = nullptr;  // the focus left on a window that closed (the project load card at start)
  if (!w || chromeWindow(w)) {
    for (int i = g.Windows.Size - 1; i >= 0; --i)  // (display order: the front one last)
      if (ImGuiWindow* o = g.Windows[i]; o == o->RootWindow && closableWindow(o, dialogsOnly)) return o;
    return nullptr;
  }
  return closableWindow(w, dialogsOnly) ? w : nullptr;
}

void closeFocusedWindow(bool dialogsOnly) {
  if (ImGuiWindow* w = closableFocusedWindow(dialogsOnly)) GImGui->CloseWindowRequest = w->ID;
}

// K-297 (round 4): the keyboard goes back to the main view when the window that had it closes (a
// dialog closed by Esc, Cmd+W or its close button; the project load card at start). ImGui handed
// the focus to the window focused before it, often the top bar or the status bar (the click that
// opened the dialog, or nothing at start): the keys of the main view (its context) then did nothing
// until a click in it. A focus that falls on another real window (an editor, a second dialog) stays
// there. (Round 6: a click on the bars gives the keys back to the window that had them.)
const char* shownMainView() {  // the layout's main view when it is on screen (nullptr otherwise)
  for (const char* n : {"Pattern editor", "Playlist", "Live pads"})
    if (layoutui::active() == layout::Preset::Tracker || std::strcmp(n, "Pattern editor"))
      if (const ImGuiWindow* m = ImGui::FindWindowByName(n); m && m->Active && !m->Hidden &&
          (!m->DockIsActive || !m->DockNode || m->DockNode->VisibleWindow == m))  // (not a tab behind another)
        return n;
  return nullptr;
}

void focusBackToMainView(App& a) {
  static ImGuiID prevId = 0;
  ImGuiContext& g = *GImGui;
  ImGuiWindow* cur = g.NavWindow ? g.NavWindow->RootWindow : nullptr;
  ImGuiWindow* prev = prevId ? ImGui::FindWindowByID(prevId) : nullptr;
  prevId = cur ? cur->ID : 0;
  // K-297 (round 6): every layout starts with the keys in its main view. Studio and Clips started
  // with them in the status bar (the last window to appear on the first frame after the splash takes
  // the focus, and the main view's focus asked for while the splash showed found no window), so the
  // main view's keys did nothing until a click in it. Once the main view is on screen, a focus still
  // on the bars, the splash or nowhere goes to it; a real window that took the focus keeps it.
  static bool started = false;
  if (!started && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {  // (a menu open at start keeps it)
    if (const char* n = shownMainView()) {
      started = true;
      if ((!cur || barWindow(cur)) && a.focusWindow.empty() && !ImGui::IsAnyMouseDown()) {
        ImGuiWindow* m = ImGui::FindWindowByName(n);
        ImGui::FocusWindow(m);  // (now, not next frame: a menu opened in between would close)
        prevId = m->ID;
        return;
      }
    }
  }
  // K-297 (round 5): the keyboard never stays with a tab hidden behind another one of its region.
  // Esc on a Tracker page (Settings, Keyboard shortcuts) brings the pattern editor forward and keeps
  // the page open behind it: the hidden page kept the keys and the pattern editor took none until
  // clicked. The tab shown in that region takes them once the hidden one has held them a few frames
  // (a window just opened into a region is shown a frame after it takes the focus).
  static int hiddenFrames = 0;
  ImGuiDockNode* node = cur && cur->DockIsActive ? cur->DockNode : nullptr;
  ImGuiWindow* shown = node ? node->VisibleWindow : nullptr;
  hiddenFrames = shown && shown != cur && !cur->DockTabIsVisible && shown->Active ? hiddenFrames + 1 : 0;
  if (hiddenFrames >= 3 && !ImGui::IsAnyMouseDown() && !g.IO.WantTextInput &&
      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
    hiddenFrames = 0;
    ImGui::FocusWindow(shown);
    prevId = shown->ID;
    return;
  }
  // K-297 (round 6): the bars never keep the keys. A click on the top bar or the status bar (a
  // transport button, a window button, the tempo) gave them the keyboard focus, and the keys of the
  // view in use did nothing until a click back in it. Once the click is over (no button held, no menu
  // or pop-up open, no text being typed) the keys go back to the window that had them, or the main view.
  static ImGuiID lastReal = 0;
  static int barFrames = 0;
  const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (cur && !chromeWindow(cur) && !(cur->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Tooltip)))
    lastReal = cur->ID;
  barFrames = cur && barWindow(cur) && !ImGui::IsAnyMouseDown() && !popupOpen && !g.IO.WantTextInput && a.focusWindow.empty() ? barFrames + 1 : 0;
  if (barFrames >= 2) {
    barFrames = 0;
    ImGuiWindow* back = lastReal ? ImGui::FindWindowByID(lastReal) : nullptr;
    if (back && back->Active && !back->Hidden && (!back->DockIsActive || !back->DockNode || back->DockNode->VisibleWindow == back)) {
      ImGui::FocusWindow(back);
      prevId = back->ID;
      return;
    }
    if (const char* n = shownMainView()) {
      a.focusWindow = n;
      return;
    }
  }
  if (!prev || prev == cur || prev->Active || chromeWindow(prev)) return;  // (nothing closed)
  if ((prev->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Tooltip)) && !(prev->Flags & ImGuiWindowFlags_Modal))
    return;  // (a menu or pop-up closed: the focus stays where it was opened from)
  if (cur && !chromeWindow(cur) && (cur->Active || cur->WasActive)) return;
  if (!a.focusWindow.empty() || ImGui::IsAnyMouseDown() || g.IO.WantTextInput ||
      ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    return;
  if (const char* n = shownMainView()) a.focusWindow = n;
}
// K-297 (round 6): a text field ended with Enter or Esc. Esc in a field of a small pop-up (the
// tempo typed in, a rename) closes the pop-up as well: the first Esc only put the old text back and
// a second one was needed. A field of the bars or the browser gives the keys to the main view once
// typing ends (the search box kept them, and the main view's keys did nothing until a click).
void textFieldEnded(App& a) {
  static ImGuiID textId = 0, textWin = 0;
  static int toMain = 0;
  ImGuiContext& g = *GImGui;
  const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
  if (textId && g.ActiveId != textId && (esc || enter)) {
    ImGuiWindow* w = ImGui::FindWindowByID(textWin);
    if (w && (w->Flags & ImGuiWindowFlags_Popup) && !(w->Flags & (ImGuiWindowFlags_Modal | ImGuiWindowFlags_ChildMenu))) {
      if (esc)
        for (int i = 0; i < g.OpenPopupStack.Size; ++i)
          if (g.OpenPopupStack[i].Window == w) {
            ImGui::ClosePopupToLevel(i, true);
            break;
          }
      w = w->ParentWindow;  // (the window the pop-up was opened from)
    }
    ImGuiWindow* root = w ? w->RootWindow : nullptr;
    auto named = [&](const char* n) {  // (a window's name, or its stable id after "###")
      const char* id = std::strstr(root->Name, "###");
      return !std::strcmp(id ? id + 3 : root->Name, n);
    };
    const bool panel = root && (barWindow(root) || named("Browser") || named("Library") || named("Notes") ||
                                (layoutui::active() == layout::Preset::Tracker && named("Order list")));  // (the Tracker's header band)
    if (panel) toMain = 3;
  }
  const bool typing = g.ActiveId && g.InputTextState.ID == g.ActiveId && g.ActiveIdWindow;
  // (a few frames later, unless the field took the keys again: a name refused shows why and stays
  // open for another try; a dialog over it keeps them too)
  if (typing || ImGui::GetTopMostPopupModal() || ImGui::IsAnyMouseDown()) toMain = 0;
  if (toMain > 0 && --toMain == 0)
    if (const char* n = shownMainView()) a.focusWindow = n;
  // A field opened again starts afresh: the whole text selected, as the first time. ImGui kept the
  // ended field's state while no other field took its place, and a rename opened a second time
  // put the cursor where the last typing had left it ("Drums" renamed again became "Drumsqq").
  if (!typing && g.InputTextState.ID && g.ActiveId != g.InputTextState.ID) g.InputTextState.ID = 0;
  textId = typing ? g.ActiveId : 0;
  textWin = typing ? g.ActiveIdWindow->ID : 0;
}
}  // namespace

void closeFrontWindow(bool dialogsOnly) { closeFocusedWindow(dialogsOnly); }

void handleGlobalShortcuts(App& a) {
  GImGui->CloseWindowRequest = 0;  // K-297: a close asked for last frame was taken by the window's Begin() (or it is gone)
  endPointerGesture(a);
  textFieldEnded(a);
  focusBackToMainView(a);
  if (menuShortcuts(a)) return;
  ImGuiIO& io = ImGui::GetIO();
  // K-297: Cmd+W in the sets that have it (K-301 R02-08). Round 8: also with the cursor in a text
  // field (the plugin picker's search, a dialog's field), and the plugin picker (an overlay without
  // a close button) closes too, as Esc closes it.
  if (keys::pressedOverText("window.close")) {
    const ImGuiWindow* nav = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : nullptr;
    if (browserui::pluginPickerOpen() && nav && std::strncmp(nav->Name, "Plugin picker##", 15) == 0) browserui::togglePluginPicker();
    else closeFocusedWindow(false);
  }
  if (io.WantTextInput) return;
  if (newtracks::holdsKeys()) return;  // R06-07: the New tracks dialog is modal
  using keys::pressed;
  // K-79: with the typing keyboard on, letters and digits play notes: a command on such a key
  // without Ctrl / Alt / Cmd (the tracks set: B, X, E, R, K, ...) waits until it is off.
  auto typed = [&a](const char* id) {
    if (a.typingKeyboard && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeySuper)
      for (const keys::Chord& c : keys::keymap().bindings(id)) {
        const bool typingKey = (c.key >= keys::Key::A && c.key <= keys::Key::D9) || (c.key >= keys::Key::Minus && c.key <= keys::Key::Grave);
        if (typingKey && keys::typingKeyPlays(c.key) && !(c.mods & (keys::kCtrl | keys::kAlt | keys::kCmd))) return false;  // R09-10
      }
    return keys::pressed(id);
  };
  if (pressed("transport.play")) {
    if (!a.engine.playing()) (void)menuparity::playPressed(a);
    else if (layoutui::active() == layout::Preset::Logic) menuparity::tracksStop(a);  // R02-11: Space stops in place
    else menuparity::stopPressed(a);
  }  // J-109: Stop releases everything; J-112: stops plugin playback first
  undoui::frame(a);  // K-141: Undo / Redo (not while a menu or dialog is open; waits for an open edit)
  if (pressed("edit.history")) a.showHistory = !a.showHistory;
  if (pressed("file.save")) a.save();
  if (pressed("file.saveAs")) a.saveAs();
  if (pressed("file.open")) a.openDialog();
  if (pressed("file.new")) a.guardUnsaved([&a]() {
      a.newProject();
      if (logicplug::active(a)) newtracks::open();  // R01-17: the Tracks layout asks for the first tracks, as the reference does
    });
  if (pressed("file.render")) a.showExport = true;
  if (pressed("file.importUrl")) urlimportui::open(a);  // J-192
  if (pressed("transport.tapTempo")) a.tapTempo();  // K-48
  for (const bool audio : {true, false})  // K-112c: a new track of the Clips set, selected
    if (pressed(audio ? "track.newAudio" : "track.newMidi"))
      livelook::newTrack(a, audio);  // N45-08: after the selected track, selected
  if (livelook::active(a)) {  // N47-11: the Clips set's return track, detail bands and Session record keys
    if (pressed("track.newReturn")) livelook::newReturnTrack(a);
    if (pressed("clips.insertMidiClip")) livelook::gridInsertMidiClip(a);  // N49-07
    // Checkpoint 52 (S10): not while the device chain has the keyboard (the reference groups devices there).
    const ImGuiWindow* nw = GImGui->NavWindow;
    const bool chainKeys = nw && (std::strstr(nw->Name, "##chain") || std::strstr(nw->Name, "##expanded") || std::strstr(nw->Name, "##body"));
    if (!chainKeys && pressed("track.group")) livelook::groupTracks(a);  // checkpoint 51
    if (!chainKeys && pressed("track.ungroup")) livelook::ungroupTracks(a);
    if (pressed("view.detail")) livelook::toggleDetail(a);
    if (pressed("view.io")) liveio::toggle(a);  // checkpoint 55 (gap 2): Cmd+Alt+I, the In/Out section
    if (pressed("view.quickHelp")) {  // checkpoint 55 (gap 9): Shift+?, the Info View
      const bool on = layout::visible(layout::shell(layout::Preset::Live), layoutui::regionState(a, layout::Preset::Live), "live.info");
      layoutui::setRegionVisible(a, "live.info", !on);
    }
    if (!a.typingKeyboard && pressed("view.automation")) livelook::toggleAutomationLanes(a);  // N52-10: A, the automation lanes
    if (pressed("transport.sessionRecord")) livelook::toggleSessionRecord(a);
  }
  if (pressed("options.typingKeyboard")) a.typingKeyboard = !a.typingKeyboard;
  if (typed("options.metronome")) a.metronome = !a.metronome;
  if (!a.typingKeyboard && pressed("transport.songMode"))
    a.engine.setMode(a.engine.mode() == PlayMode::Song ? PlayMode::Pattern : PlayMode::Song);
  if (pressed("help.shortcuts")) a.showShortcuts = !a.showShortcuts;
  if (pressed("pattern.new")) {
    ObjectId id = a.doc.addPattern("Pattern " + std::to_string(a.doc.song().patterns.size() + 1));
    if (id) a.selectPattern(id);
  }
  auto toggle = [&](bool& f, const char* name) {
    // K-297 (round 6): the key of a window open as a tab behind another one of its region brings it
    // forward (with the keys). It closed it: F5 with the piano roll in front of the playlist closed
    // the playlist nobody could see, and a second F5 was needed to show it.
    if (f && ui::bringTabForward(name)) return;
    f = !f;
    if (f) a.focusWindow = name;
  };
  if (layoutui::active() == layout::Preset::Tracker) {
    // K-106: the Tracker's screens. A page comes to the front (the pages stay open behind it);
    // Files and the pattern keys swap into the header band; the band hides and comes back.
    struct Pg {
      const char* id;
      const char* window;
    };
    static const Pg pages[] = {{"view.patternEditor", "Pattern editor"}, {"view.playlist", "Playlist"}, {"view.pianoRoll", "Piano roll"},
                               {"view.channelRack", "Channel rack"},     {"view.mixer", "Mixer"},      {"view.pads", "Live pads"}};
    for (const Pg& p : pages)
      if (typed(p.id)) trackerview::showPage(a, p.window);
    if (typed("view.files")) trackerview::setHeaderSwap(trackerview::headerSwap() == 2 ? 0 : 2);
    if (typed("help.trackerKeys")) trackerview::setHeaderSwap(trackerview::headerSwap() == 1 ? 0 : 1);
    if (typed("view.headerBand")) trackerview::setExtended(a, !trackerview::extended());
    if (typed("view.transposeBox")) trackerview::toggleScopeTool(a, 1);
    if (typed("view.cleanUp")) trackerview::toggleScopeTool(a, 2);
    if (typed("view.advancedEdit")) trackerview::toggleScopeTool(a, 3);
    if (typed("view.midiBox")) trackerview::toggleScopeTool(a, 4);
    if (typed("view.sampleTools")) trackerview::toggleScopeTool(a, 5);
    if (typed("view.zoomTracksIn")) trackerview::zoomFront(a, 1);
    if (typed("view.zoomTracksOut")) trackerview::zoomFront(a, -1);
    if (typed("view.zoomTracksFit")) trackerview::zoomFront(a, 0);
    if (typed("view.sampleEditor")) {  // cp17/cp18 (K-316 3.3): Ctrl+Alt+E, the current instrument's samples
      const Channel* ch = a.doc.song().channel(a.selChannel);
      if (ch && trackereditors::hasSampleEditor(a, ch->id)) trackereditors::toggle(a, trackereditors::Editor::Sample, true);
      else if (ch && ch->plugin.type == "gl.sampler") trackerview::toggleAudioPage(a);  // (a Sampler's sample: the Audio page)
      else if (ch) trackereditors::toggle(a, trackereditors::Editor::Instrument, true);  // (the Channel frame)
    }
    if (typed("view.channelSettings")) trackereditors::toggle(a, trackereditors::Editor::Instrument, true);  // cp18: Ctrl+Alt+I
    if (typed("view.audioEditor")) trackerview::toggleAudioPage(a);  // as the header's Audio editor button
  } else {
    if (typed("view.playlist")) {
      // K-112B: in the Tracks layout the key (Cmd+1) brings the main window to the front, as the
      // reference's Open Main Window does; the tracks area there never closes.
      if (layoutui::active() == layout::Preset::Logic) a.showPlaylist = true, a.focusWindow = "Playlist";
      else toggle(a.showPlaylist, "Playlist");
    }
    if (typed("view.channelRack")) toggle(a.showRack, "Channel rack");
    if (typed("view.pianoRoll")) toggle(a.showPiano, "Piano roll");
    if (typed("view.mixer")) toggle(a.showMixer, "Mixer");
    if (typed("view.patternEditor") && layoutui::active() != layout::Preset::Live)  // N46-09: not in Clips
      if (bool* f = layoutui::windowOpenFlag(a, "Pattern editor")) toggle(*f, "Pattern editor");
    if (typed("view.pads")) toggle(a.showLivePads, "Live pads");
  }
  if (typed("view.browser")) toggle(a.showBrowser, "Browser");
  if (typed("view.channelSettings") && layoutui::active() != layout::Preset::Tracker) a.settingsChannel = a.settingsChannel != kNoId ? kNoId : a.selChannel;
  if (typed("view.audioEditor") && layoutui::active() != layout::Preset::Tracker) {  // the Tracker's key opens its own page (above)
    if (layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic)) toggleTracksAudioEditor(a);  // R01-15
    else a.settings.set("ui.loupePinned", !a.settings.getBool("ui.loupePinned"));
  }
  if (typed("view.stage")) toggle(a.showStage, "Mixer");  // the mixer window's Stage view (stageui::sync opens the mixer)
  // K-124: Esc in an editor or plugin window brings the playlist (the main window) to the front,
  // once nothing else used the key this frame: a popup, menu or dialog, a drag, a text field, a tool.
  // K-297: in the Tracker a page docked in the centre goes as its Exit does (Esc, Cmd+W).
  const bool trackerOn = layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker);
  // R8-02: the instrument steps from anywhere in the Tracker (the pattern takes the key itself).
  if (trackerOn && !trackerview::hasKeyboard() && (io.KeyCtrl || io.KeySuper) && io.KeyShift && !io.KeyAlt &&
      (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
    trackerview::stepInstrument(a, ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 1);
  if (!(trackerOn && trackerview::closeFrontPageKey(a))) {
    // N45-01: in Clips Escape never changes the main view (only Tab swaps the grid and the arrangement).
    if (!livelook::active(a) && !keys::escapeBusy() && editorWindowFocused() && keys::pressedPastEditor("view.backToPlaylist")) layoutui::showPlaylist(a);
    // K-297: Esc in a dialog or tool window closes it (K-301 R02-19), once nothing else used the key.
    if (!keys::escapeBusy() && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        closableFocusedWindow(true)) {
      closeFocusedWindow(true);
      keys::claimEscape();
    }
    // (Cmd+W: at the top, also over a text field.)
  }
  if (pressed("file.projectInfo")) a.showInfo = !a.showInfo;
  // K-79: commands the layout presets' shortcut sets put on single keys.
  if (typed("transport.record")) {
    // R05-07: in the Tracks layout R is Record: armed and playing at once (with the count-in when it
    // is on); pressed while recording, it stops recording and playback goes on.
    if (logicplug::active(a) && !a.recordArmed && !a.engine.playing()) {
      a.recordArmed = true;
      (void)menuparity::playPressed(a);
    } else {
      livelook::toggleArrangementRecord(a);  // (outside Clips: the switch alone)
    }
  }
  if (typed("transport.toStart")) a.engine.setPosition(0);
  if (typed("transport.rewindBar") || typed("transport.forwardBar")) {
    const Tick bar = std::max<Tick>(1, a.doc.song().barTicks());
    const Tick pos = static_cast<Tick>(a.engine.position());
    const bool songBars = a.engine.mode() != PlayMode::Pattern;  // K-193: real bars of the song's meter map (as the top bar)
    const MeterMap mm = songBars ? MeterMap(a.doc.song()) : MeterMap();
    const Tick at = keys::pressed("transport.rewindBar")
                        ? std::max<Tick>(0, songBars ? mm.barStartAt(std::max<Tick>(0, pos - 1)) : (pos - 1) / bar * bar)
                        : (songBars ? mm.nextBar(pos) : (pos / bar + 1) * bar);
    a.engine.setPosition(at);
  }
  if (typed("options.countIn")) a.countInBars = a.countInBars > 0 ? 0 : 1;
  for (int o = 1; o <= 8; ++o)  // K-112: the typing keyboard's octave picked directly (the Tracker set: F1 to F8)
    if (typed(("typing.octave" + std::to_string(o)).c_str())) a.typing.octave = o;
  if (typed("view.toggleMain")) layoutui::toggleMainView(a);
  if (typed("view.editors")) {
    if (layoutui::active() == layout::Preset::Studio) toggle(a.showPiano, "Piano roll");
    else layoutui::toggleEditors(a);
  }
  if (layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic)) {  // K-112b: the layout's panes
    const layout::Preset lp = layout::Preset::Logic;
    auto shown = [&](const char* region) { return layout::visible(layout::shell(lp), layoutui::regionState(a, lp), region); };
    auto pane = [&](const char* cmd, const char* region) {
      if (typed(cmd)) layoutui::setRegionVisible(a, region, !shown(region));
    };
    if (typed("view.library")) {  // B: the library on the left; one browser at a time (R01-25)
      const bool on = !shown("logic.library");
      layoutui::setRegionVisible(a, "logic.library", on);
      if (on && a.showBrowser) a.showBrowser = false;  // the right pane's file browser closes (its pane with it)
    }
    pane("view.inspector", "logic.inspector");
    // R01-08: Quick help is a small floating window (LayoutParts.cpp quickHelpFloat), not a pane.
    if (typed("view.quickHelp")) a.settings.set("ui.logicQuickHelp", !a.settings.getBool("ui.logicQuickHelp"));
    if (typed("view.automation")) toggleAutomationLanes();  // R02-15: A, the selected tracks' lanes
    if (typed("view.smartControls"))  // R01-07: Y, the Smart controls in the lower pane
      if (bool* sm = layoutui::windowOpenFlag(a, "Smart controls")) {
        if (*sm && shown("logic.lower")) layoutui::setRegionVisible(a, "logic.lower", false);
        else layoutui::setRegionVisible(a, "logic.lower", true), *sm = true;
      }
    if (typed("view.toolbar")) a.settings.set("ui.logicToolbar", !a.settings.getBool("ui.logicToolbar"));
    auto side = [&](const char* cmd, const char* window) {  // a panel of the right pane: shows it there, or hides the pane
      if (!typed(cmd)) return;
      bool* f = layoutui::windowOpenFlag(a, window);
      if (!f) return;
      if (*f && shown("logic.right")) layoutui::setRegionVisible(a, "logic.right", false);
      else layoutui::setRegionVisible(a, "logic.right", true), *f = true;
    };
    side("view.lists", "Lists");
    side("view.notes", "Notes");
    side("view.files", "Files");  // O: the project's files
    // (V, view.hidePlugins: menuShortcuts takes it for every layout, K-297)
    if (typed("playlist.toolMenu")) openToolMenu();  // R01-18
    if (typed("view.globalTracks")) a.settings.set("ui.logicGlobalTracks", !a.settings.getBool("ui.logicGlobalTracks"));  // R01-15
    // K-112B (checkpoint 39): the Track and Navigate menu keys.
    if (pressed("track.newTracks")) newtracks::open();
    if (typed("transport.cycle")) toggleCycle(a);
    if (pressed("transport.cycleSelection")) cycleToSelection(a);  // R02-13
    if (keys::focusContext() == keys::Context::Playlist) tracksNavKeys(a);  // checkpoint 47
    if (pressed("transport.prevMarker")) gotoMarker(a, -1);       // R02-15
    if (pressed("transport.nextMarker")) gotoMarker(a, 1);
    if (typed("transport.goToPosition")) goToPositionPrompt(a);
    if (pressed("track.duplicate")) tracksNewSameSettings(a);
    if (keys::focusContext() == keys::Context::Playlist && typed("track.mute")) tracksMuteSelected(a);  // R04-11
    if (keys::focusContext() == keys::Context::Playlist && typed("track.solo")) tracksSoloSelected(a);
    if (pressed("track.delete")) tracksDeleteSelected(a);
  }
  if (keys::focusContext() == keys::Context::Playlist && typed("playlist.splitAtSongPos")) {
    if (layoutui::active() == layout::Preset::Logic) tracksSplitAtPlayhead(a);  // the Tracks layout: the selected clips only
    else if (livelook::active(a) && a.playlist.selEnd > a.playlist.selStart) chopui::splitAtSelection(a);  // N48-05: both edges
    else splitClipsAtPlayhead(a);
  }
  const auto& pats = a.doc.song().patterns;
  int pi = a.doc.song().patternIndex(a.curPattern);
  if (pressed("pattern.next") && pi + 1 < static_cast<int>(pats.size())) a.selectPattern(pats[static_cast<size_t>(pi + 1)].id);
  if (pressed("pattern.previous") && pi > 0) a.selectPattern(pats[static_cast<size_t>(pi - 1)].id);
}

}  // namespace gl

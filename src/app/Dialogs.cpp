// Gridline — dialogs (file, settings, project info, export, history, about,
// shortcuts, unsaved changes) and global keyboard shortcuts.
#include "DisplayConfirmUi.h"
#include "GridlineVersion.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "ScriptUi.h"
#include "Keys.h"
#include "J31Ui.h"
#include "MenuParityUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/Backup.h"
#include "../core/I18n.h"
#include "../core/UiScale.h"
#include "../core/flp/Flp.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "ExportUi.h"
#include "../core/ProjectInterop.h"
#include "Theme.h"
#include "MidiUi.h"
#include "SettingsPages.h"
#include "SmoothScrollUi.h"
#include "McpUi.h"
#include "TemplateUi.h"
#include "FlpExportUi.h"
#include "J59Ui.h"
#include "UpdateUi.h"
#include "Widgets.h"
#include "Windows.h"
#include "../core/WheelRoute.h"
#include "../core/Platform.h"

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool matches(const std::string& name, const std::vector<std::string>& exts) {
  std::string l = lower(name);
  for (const auto& e : exts)
    if (l.size() >= e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0) return true;
  return false;
}

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

void dialogTitle(App& a, const char* title, const char* subtitle = nullptr) {
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(i18n::tr(title));
  ImGui::PopFont();
  if (subtitle) ImGui::TextColored(theme::vec(pal().textDim), "%s", i18n::tr(subtitle));
  ImGui::Dummy(ImVec2(0, theme::kS1));
}

void fileDialog(App& a) {
  FileDialogState& fd = a.fileDialog;
  const auto& P = pal();
  if (fd.open) {
    ImGui::OpenPopup("##filedialog");
    fd.open = false;
  }
  {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 sz(std::min(680.0f, vp->WorkSize.x - 80), std::min(480.0f, vp->WorkSize.y - 80));
    centerNext(sz);
  }
  bool keep = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS4));
  bool open = ImGui::BeginPopupModal("##filedialog", &keep, ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (!open) return;
  const bool folderMode = fd.exts.size() == 1 && fd.exts[0] == "/";
  dialogTitle(a, fd.title.c_str());
  std::error_code ec;
  if (fd.dir.empty() || !fs::is_directory(fd.dir, ec)) fd.dir = gl::platform::homeEnv() ? gl::platform::homeEnv() : ".";
  const float fh = ImGui::GetFrameHeight();
  if (ui::IconButton("up", ui::Icon::Up, false, fh, "Parent folder")) fd.dir = fs::path(fd.dir).parent_path().string();
  ImGui::SameLine(0, theme::kS1);
  if (ui::IconButton("home", ui::Icon::Folder, false, fh, "Home folder")) fd.dir = gl::platform::homeEnv() ? gl::platform::homeEnv() : ".";
  ImGui::SameLine(0, theme::kS1);
  static char dirBuf[1024];
  std::snprintf(dirBuf, sizeof dirBuf, "%s", fd.dir.c_str());
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputText("##dir", dirBuf, sizeof dirBuf, ImGuiInputTextFlags_EnterReturnsTrue) && fs::is_directory(dirBuf, ec)) fd.dir = dirBuf;
  if (ImGui::CalcTextSize(fd.dir.c_str()).x > ImGui::GetItemRectSize().x - 2 * ImGui::GetStyle().FramePadding.x) ui::Tooltip(fd.dir.c_str());
  ui::FocusRing();

  // J-54: kS1, name field, kS1, buttons under the list; reserved exactly, so
  // the dialog itself never shows a scrollbar.
  const float footer = ui::DialogFooterHeight(2);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS1, theme::kS1));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##list", ImVec2(0, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleColor();
  std::vector<fs::path> dirs, files;
  for (fs::directory_iterator it(fd.dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::string n = it->path().filename().string();
    if (n.empty() || n[0] == '.') continue;
    std::error_code e2;
    if (it->is_directory(e2))
      dirs.push_back(it->path());
    else if (!folderMode && matches(n, fd.exts))
      files.push_back(it->path());
  }
  auto byName = [](const fs::path& x, const fs::path& y) { return lower(x.filename().string()) < lower(y.filename().string()); };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  std::string chosen;
  auto row = [&](const fs::path& f, bool dir) {
    std::string n = f.filename().string();
    ImGui::PushID(n.c_str());
    ImVec2 q = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    bool sel = !dir && fd.name == n;
    bool clicked = ImGui::InvisibleButton("##r", ImVec2(w, 24));
    bool hov = ImGui::IsItemHovered();
    bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sel || hov) dl->AddRectFilled(q, ImVec2(q.x + w, q.y + 24), sel ? theme::withAlpha(P.accent, 0.3f) : P.surface800, theme::kRadiusSm);
    ui::drawIcon(dl, dir ? ui::Icon::Folder : isAudioFile(f.string()) ? ui::Icon::Wave : ui::Icon::File, ImVec2(q.x + 14, q.y + 12), 12,
                 dir ? P.textDim : P.textFaint);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + 30, q.y + 5), w - 40, P.text, n.c_str());
    if (dir && dbl) fd.dir = f.string();
    if (!dir && clicked) {
      fd.name = n;
      if (isAudioFile(f.string())) a.previewSample(f.string());
    }
    if (!dir && dbl) chosen = f.string();
    ImGui::PopID();
  };
  for (const auto& d : dirs) row(d, true);
  for (const auto& f : files) row(f, false);
  if (dirs.empty() && files.empty()) {
    ImVec2 mn = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ui::EmptyState(ImGui::GetWindowDrawList(), mn, ImVec2(mn.x + sz.x, mn.y + sz.y), "This folder is empty",
                   folderMode ? "Choose it, or open a sub-folder" : "No matching files here");
  }
  ImGui::PopStyleVar();  // rows sit flush inside the list; the list itself keeps the normal gap below it
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (!folderMode) {
    static char nameBuf[512];
    std::snprintf(nameBuf, sizeof nameBuf, "%s", fd.name.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##name", i18n::tr("File name"), nameBuf, sizeof nameBuf)) fd.name = nameBuf;
    ui::FocusRing();
  } else {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitText(fd.dir.c_str(), ImGui::GetContentRegionAvail().x));
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  // Save dialogs with types: the name always carries the chosen type's extension.
  const std::string target = fd.name.empty() ? std::string()
                             : (fs::path(fd.dir) / (fd.save && fd.nameForType ? fd.nameForType(fd.name, fd.typeIndex) : fd.name)).string();
  if (!fd.confirmOverwrite.empty() && fd.confirmOverwrite != target) fd.confirmOverwrite.clear();
  const char* okLabel = !fd.confirmOverwrite.empty() ? "Replace" : fd.save ? "Save" : folderMode ? "Choose this folder" : "Open";
  float okW = std::max(96.0f, ImGui::CalcTextSize(okLabel).x + 24);
  // J-52: file types (Open: all projects / one format; Save as: the format written).
  if (!fd.types.empty()) {
    fd.typeIndex = std::clamp(fd.typeIndex, 0, static_cast<int>(fd.types.size()) - 1);
    const float typeW = std::max(120.0f, ImGui::GetContentRegionAvail().x - (96 + okW + theme::kS2 * 3));
    if (ui::BeginCombo("##types", i18n::tr(fd.types[static_cast<size_t>(fd.typeIndex)].label.c_str()), typeW)) {  // J-78: translated labels
      for (size_t i = 0; i < fd.types.size(); ++i)
        if (ImGui::Selectable(i18n::tr(fd.types[i].label.c_str()), static_cast<int>(i) == fd.typeIndex)) {
          fd.typeIndex = static_cast<int>(i);
          fd.exts = fd.types[i].exts;
          if (fd.save && fd.nameForType && !fd.name.empty()) fd.name = fd.nameForType(fd.name, fd.typeIndex);
        }
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, theme::kS2);
  } else if (!fd.confirmOverwrite.empty()) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.hot), "%s", "Already exists");
    ImGui::SameLine(0, theme::kS2);
  }
  alignRight(96 + theme::kS2 + okW);
  bool cancel = ui::TextButton("Cancel", false, 96) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SameLine(0, theme::kS2);
  bool ok = ui::TextButton(okLabel, true, okW);
  if (!fd.confirmOverwrite.empty()) ui::Hint((fd.name + " already exists. Replace it?").c_str());
  if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !fd.name.empty()) ok = true;
  if (ok && chosen.empty()) {
    if (folderMode)
      chosen = fd.dir;
    else if (!fd.name.empty())
      chosen = target;
  }
  // Saving over an existing file asks once (the button turns into Replace).
  if (fd.save && ok && !chosen.empty() && chosen != fd.confirmOverwrite && fs::exists(chosen, ec)) {
    fd.confirmOverwrite = chosen;
    fd.name = fs::path(chosen).filename().string();
    chosen.clear();
  }
  if (!chosen.empty() && (ok || !fd.save)) {
    auto cb = fd.onOk;
    ImGui::CloseCurrentPopup();
    a.stopPreview();
    if (cb) cb(chosen);
  } else if (cancel || !keep) {
    a.stopPreview();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void settingsWindow(App& a) {
  if (!a.showSettings) return;
  centerNext(ImVec2(860, 560), ImVec2(660, 360));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin(ui::windowTitle("Settings").c_str(), &a.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  const auto& P = pal();
  Settings& st = a.settings;
  // Studio settings form: a tab strip across the top (the lit tab is the
  // page), then the page's controls grouped in recessed panels.
  static int page = 0;
  if (a.settingsTab >= 0) {
    page = a.settingsTab;
    a.settingsTab = -1;
  }
  // J-24 appended General, Files and Debug (tab indices 0..4 stay as they were).
  // J-55 appended Shortcuts and Theme.
  const char* pagesEn[] = {i18n::N_("Audio"), i18n::N_("MIDI"), i18n::N_("Project"), i18n::N_("Backup"), i18n::N_("Interface"), i18n::N_("General"), i18n::N_("Files"), i18n::N_("Debug"), i18n::N_("Shortcuts"), i18n::N_("Theme"), i18n::N_("Integrations")};
  const char* blurbs[] = {i18n::N_("Output and input devices, sample rate, buffer and recording"), i18n::N_("Controllers, keyboards and MIDI out"),
                          i18n::N_("Defaults for new projects"), i18n::N_("Automatic backups, autosave and restoring a backup"), i18n::N_("Theme, size and hints"),
                          i18n::N_("Startup, default template, language and updates"), i18n::N_("Folders and scripting"), i18n::N_("Log and script console"),
                          i18n::N_("Every keyboard shortcut: search, change, reset"), i18n::N_("Dark, light or the system's choice, accent colour and contrast"),
                          i18n::N_("Let AI assistants (MCP clients) inspect and edit the project")};
  constexpr int kPages = 11;
  const char* pages[kPages];
  for (int i = 0; i < kPages; ++i) pages[i] = i18n::tr(pagesEn[i]);
  page = std::clamp(page, 0, kPages - 1);
  ui::TabStrip("##settingstabs", pages, kPages, &page);
  ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr(blurbs[page]));
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const float fieldW = std::max(160.0f, std::min(320.0f, ImGui::GetContentRegionAvail().x - 176 - 2 * theme::kS2 - 12));
  {
    if (page == 0) {
      ui::BeginSection("INPUT / OUTPUT");
      formLabel("Output device");
      if (ui::BeginCombo("##dev", a.audioDevice.empty() ? "System default" : a.audioDevice.c_str(), fieldW)) {
        // J-77: device / rate / buffer changes apply at once and ask Keep / Revert (they can cut the sound).
        if (ImGui::Selectable(i18n::tr("System default"), a.audioDevice.empty())) ui::confirmAudioDevice(a, "");
        for (const auto& d : a.audioDevices)
          if (ImGui::Selectable(d.c_str(), d == a.audioDevice)) ui::confirmAudioDevice(a, d);
        ImGui::EndCombo();
      }
      const int rates[] = {44100, 48000, 88200, 96000};
      formLabel("Sample rate");
      for (int r : rates) {
        if (r != rates[0]) ImGui::SameLine(0, theme::kS1);
        if (ui::TextButton(std::to_string(r).c_str(), a.audioSampleRate == r) && a.audioSampleRate != r) {
          const dispconf::Snapshot before = dispconf::take(st, dispconf::audioKeys());
          st.set("audio.sampleRate", r);
          ui::confirmAudio(a, before);
        }
      }
      const int bufs[] = {64, 128, 256, 512, 1024, 2048};
      formLabel("Buffer size");
      for (int b : bufs) {
        if (b != bufs[0]) ImGui::SameLine(0, theme::kS1);
        if (ui::TextButton(std::to_string(b).c_str(), a.audioBuffer == b) && a.audioBuffer != b) {
          const dispconf::Snapshot before = dispconf::take(st, dispconf::audioKeys());
          st.set("audio.bufferFrames", b);
          ui::confirmAudio(a, before);
        }
      }
      char lat[96];
      std::snprintf(lat, sizeof lat, "%.1f ms", 1000.0 * a.audioBuffer / std::max(1, a.audioSampleRate));
      formLabel("Output latency");
      ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
      ImGui::TextUnformatted(lat);
      ImGui::PopFont();
      ui::EndSection();
      ui::BeginSection("MIXER");
      // Mixer processing threads (the output is identical for any count).
      {
        const int cur = static_cast<int>(st.getNumber("audio.threads"));
        formLabel("Processing threads");
        const int counts[] = {0, 1, 2, 4, 6, 8};
        for (int c : counts) {
          if (c != counts[0]) ImGui::SameLine(0, theme::kS1);
          std::string l = c == 0 ? "Auto (" + std::to_string(a.processingThreads()) + ")" : std::to_string(c);
          if (c == 0 && cur > 0) l = "Auto";
          if (ui::TextButton(l.c_str(), cur == c, 0.0f,
                             c == 0 ? "Half the hardware threads, at most 8" : c == 1 ? "Everything on the audio thread" : nullptr)) {
            st.set("audio.threads", c);
            a.syncEngine();
          }
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
      ui::BeginSection("UNDO");
      int levels = static_cast<int>(st.getNumber("undo.maxLevels"));
      formLabel("Maximum levels");
      ImGui::SetNextItemWidth(96);
      if (ImGui::InputInt("##levels", &levels, 0, 0)) {
        levels = std::clamp(levels, 10, 10000);
        st.set("undo.maxLevels", levels);
        a.doc.setMaxUndoLevels(static_cast<size_t>(levels));
      }
      ui::EndSection();
    }
    if (page == 3) {
      static const char* freq[] = {i18n::N_("Never (remind every 10 minutes)"), i18n::N_("Rarely (every 15 minutes)"), i18n::N_("Occasionally (every 10 minutes)"),
                                   i18n::N_("Regularly (every 5 minutes)"), i18n::N_("Frequently (5 minutes + before risky edits)"),
                                   i18n::N_("Very frequently (every minute + before risky edits)")};
      int f = std::clamp(static_cast<int>(st.getNumber("backup.frequency")), 0, 5);
      ui::BeginSection("HOW OFTEN");
      for (int i = 0; i < 6; ++i)
        if (ImGui::RadioButton(i18n::tr(freq[i]), f == i)) {
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
      formLabel("Confirm changes");
      bool autoRevert = st.getBool("ui.confirmAutoRevert");
      if (ImGui::Checkbox(i18n::tr("Revert size, theme, language and audio changes not kept within 15 s"), &autoRevert))
        st.set("ui.confirmAutoRevert", autoRevert);
      ui::Hint("A change that makes the interface hard to use or cuts the sound goes back by itself");
      bool hints = st.getBool("ui.showHints");
      formLabel("Status bar");
      if (ImGui::Checkbox(i18n::tr("Show hints for the control under the mouse"), &hints)) st.set("ui.showHints", hints);
      hintSettingsPanel(a, fieldW);  // J-24
      ui::EndSection();
      // J-67: motion (smooth snapping of drags, smooth scrolling, elastic edges).
      ui::BeginSection("MOTION");
      const bool reduced = scrollui::systemReducedMotion();
      ImGui::BeginDisabled(reduced);
      bool anim = st.getBool("ui.animations") && !reduced;
      formLabel("Animations");
      if (ImGui::Checkbox(i18n::tr("Interface animations"), &anim)) st.set("ui.animations", anim);
      ui::Hint("Decorative motion: the metronome ticking in tempo, key and pad lights fading, gliding drags and scrolling. "
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
      // J-48: interface sounds.
      ui::BeginSection("INTERFACE SOUNDS");
      bool snd = st.getBool("sounds.enabled");
      formLabel("Interface sounds");
      if (ImGui::Checkbox(i18n::tr("Play short sounds for startup, saving, exports, imports and errors"), &snd)) st.set("sounds.enabled", snd);
      ui::Hint("Quiet while the song plays or records, except errors. Never recorded or exported.");
      ImGui::BeginDisabled(!snd);
      formLabel("Volume");
      float vol = static_cast<float>(st.getNumber("sounds.volume")) * 100.0f;
      ImGui::SetNextItemWidth(fieldW);
      if (ImGui::SliderFloat("##uisndvol", &vol, 0.0f, 100.0f, "%.0f%%")) st.set("sounds.volume", static_cast<double>(vol) / 100.0);
      // J-54: the Play column clears the widest sound name (checkbox square + gap included).
      float playX = fieldW + 140;
      for (int i = 0; i < uisound::kEventCount; ++i)
        playX = std::max(playX, 168 + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                    ImGui::CalcTextSize(uisound::eventLabel(static_cast<uisound::Event>(i))).x + theme::kS3);
      for (int i = 0; i < uisound::kEventCount; ++i) {
        const auto e = static_cast<uisound::Event>(i);
        bool on = st.getBool(uisound::settingKey(e));
        formLabel(i == 0 ? "Sounds" : "");
        ImGui::PushID(i);
        if (ImGui::Checkbox(uisound::eventLabel(e), &on)) st.set(uisound::settingKey(e), on);
        ImGui::SameLine(playX);
        ImGui::BeginDisabled(a.uiSoundsDisabled || !a.uiSounds.hasClip(e));
        if (ui::TextButton("Play", false, 48, "Preview this sound")) {
          a.uiSounds.setVolume(static_cast<float>(st.getNumber("sounds.volume")));
          a.uiSounds.trigger(e);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::EndDisabled();
      if (a.uiSoundsDisabled) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Interface sounds are turned off for this session (--no-ui-sounds or GRIDLINE_NO_UI_SOUNDS)."));
        ImGui::PopTextWrapPos();
      }
      ui::EndSection();
    }
    if (page == 5) generalSettingsPage(a, fieldW);
    if (page == 8) shortcutSettingsPage(a, fieldW);
    if (page == 9) themeSettingsPage(a, fieldW);
    if (page == 6) fileSettingsPage(a, fieldW);
    if (page == 7) debugSettingsPage(a, fieldW);
    if (page == 10) mcpui::settingsPage(a, fieldW);  // SC-07
  }
  ImGui::EndChild();
  ImGui::End();
}

int pendingPpq = 0;  // Project info: a lower timebase waiting for confirmation

void infoWindow(App& a) {
  if (!a.showInfo) return;
  centerNext(ImVec2(480, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin(ui::windowTitle("Project info").c_str(), &a.showInfo, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  const Song& s = a.doc.song();
  SongInfo info = s.info;
  bool changed = false;
  auto field = [&](const char* label, std::string& v, bool multiline = false) {
    char buf[2048];
    std::snprintf(buf, sizeof buf, "%s", v.c_str());
    formLabel(label, 120);
    std::string id = std::string("##") + label;
    bool ch = multiline ? ImGui::InputTextMultiline(id.c_str(), buf, sizeof buf, ImVec2(324, 90))
                        : (ImGui::SetNextItemWidth(324), ImGui::InputText(id.c_str(), buf, sizeof buf));
    ui::FocusRing();
    if (ch) v = buf, changed = true;
  };
  ui::BeginSection("PROJECT", 464);
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
  ui::BeginSection("TIMING", 464);
  formLabel("Time signature", 120);
  int num = s.timeSigNum, den = s.timeSigDen;
  if (ui::BeginCombo("##num", std::to_string(num).c_str(), 64)) {
    for (int n = 1; n <= 16; ++n)
      if (ImGui::Selectable(std::to_string(n).c_str(), n == num)) a.doc.setTimeSignature(n, den);
    ImGui::EndCombo();
  }
  ImGui::SameLine(0, theme::kS2);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textDim), "/");
  ImGui::SameLine(0, theme::kS2);
  const int dens[] = {2, 4, 8, 16};
  if (ui::BeginCombo("##den", std::to_string(den).c_str(), 64)) {
    for (int d : dens)
      if (ImGui::Selectable(std::to_string(d).c_str(), d == den)) a.doc.setTimeSignature(num, d);
    ImGui::EndCombo();
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
    const bool cancel = ui::TextButton("Cancel", false, 110) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Change PPQ", true, 110)) {
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
  size_t notes = 0;
  for (const auto& p : s.patterns)
    for (const auto& [c, ns] : p.notes) notes += ns.size();
  ImGui::TextDisabled(i18n::tr("%zu channels · %zu patterns · %zu clips · %zu notes · %zu mixer tracks"), s.channels.size(),
                      s.patterns.size(), s.playlist.size(), notes, s.mixer.size());
  ImGui::End();
}

void historyWindow(App& a) {
  static bool wasShown = false;
  if (!a.showHistory) {
    wasShown = false;
    return;
  }
  // J-40: opening it (menu, Alt+Z, --show history) brings it to the front,
  // also when it is docked behind another tab (the browser by default).
  const bool opened = !wasShown;
  wasShown = true;
  if (opened) ImGui::SetNextWindowFocus();
  ImGui::SetNextWindowSize(ImVec2(300, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(ui::windowTitle("Undo history").c_str(), &a.showHistory)) {
    ImGui::End();
    return;
  }
  const UndoStack& h = a.doc.history();
  auto labels = h.labels();
  if (ImGui::Selectable(i18n::tr("Start of history"), h.position() == 0)) a.doc.jumpTo(0);
  for (size_t i = 0; i < labels.size(); ++i) {
    bool applied = i < h.position();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(applied ? pal().text : pal().textFaint));
    std::string l = labels[i] + "##" + std::to_string(i);
    if (ImGui::Selectable(l.c_str(), i + 1 == h.position())) a.doc.jumpTo(i + 1);
    if (opened && i + 1 == h.position()) ImGui::SetScrollHereY(0.5f);  // show the current step
    ImGui::PopStyleColor();
  }
  if (!opened && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
  ImGui::End();
}

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
    centred(a.fontRegular, theme::kFontCaption, theme::rgb(0x707070), "Inter and JetBrains Mono (SIL Open Font License)");
  }
  ImGui::End();
  ImGui::PopStyleColor(5);
}

void shortcutsWindow(App& a) {
  if (!a.showShortcuts) return;
  centerNext(ImVec2(560, 600), ImVec2(420, 300));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin(ui::windowTitle("Keyboard shortcuts").c_str(), &a.showShortcuts, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
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
  bool open = ImGui::BeginPopupModal("##unsaved", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (open) {
    dialogTitle(a, "Save changes to this project?", "Your changes will be lost if you don't save them.");
    ImGui::Dummy(ImVec2(372, theme::kS2));
    alignRight(110 + 90 + 100 + theme::kS2 * 2);
    if (ui::TextButton("Don't save", false, 110)) {
      ImGui::CloseCurrentPopup();
      if (a.afterDiscard) a.afterDiscard();
    }
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Cancel", false, 90) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      a.afterDiscard = nullptr;
      a.quitRequested = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Save", true, 100)) {
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
  bool open = ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (!open) return;
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
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
  alignRight(90 + 100 + theme::kS2);
  if (ui::TextButton("Cancel", false, 90) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    c.action = nullptr;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(c.okLabel.empty() ? "OK" : c.okLabel.c_str(), true, 100) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
    auto run = std::move(c.action);
    c.action = nullptr;
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
    if (ui::TextButton("Save as...", true, 96)) {
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

void drawDialogs(App& a) {
  ImGui::GetStyle().HoverDelayNormal = static_cast<float>(std::clamp(a.settings.getNumber("ui.hintDelay"), 0.0, 2.0));
  drawTemplateDialogs(a);  // J-24: new project from template, save as template
  j59::drawDialogs(a);     // J-59: loop starter, render to sampled instrument
  flpexport::draw(a);      // J-80: FL Studio export progress and report
  updatePerformance(a);
  drawPerformance(a);
  settingsWindow(a);
  infoWindow(a);
  exportui::draw(a);
  historyWindow(a);
  aboutWindow(a);
  shortcutsWindow(a);
  unsavedPrompt(a);
  confirmPrompt(a);           // J-82: delete clip sources, …
  ui::drawDisplayConfirm(a);  // J-77: Keep / Revert for size, theme, language, layout and audio changes
  updui::draw(a);             // J-89: update check (startup / Help), its popup
  importReportWindow(a);
  drawPluginManager(a);
  drawRemoteDialog(a);
  j31::drawWindows(a);  // J-31: chord progression editor, audio logger, MIDI scale snap
  drawProjectScriptWindow(a);  // J-57: project script settings / run
  mcpui::tick(a);                // SC-07: AI assistant server (opt-in, loopback only)
  mcpui::drawActivityWindow(a);
  fileDialog(a);
}

void handleGlobalShortcuts(App& a) {
  if (menuShortcuts(a)) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;
  using keys::pressed;
  if (pressed("transport.play")) a.engine.playing() ? menuparity::stopPressed(a) : a.startPlayback();  // J-109: Stop releases everything
  if (pressed("edit.undo")) a.doc.undo();
  if (pressed("edit.redo")) a.doc.redo();
  if (pressed("edit.history")) a.showHistory = !a.showHistory;
  if (pressed("file.save")) a.save();
  if (pressed("file.saveAs")) a.saveAs();
  if (pressed("file.open")) a.openDialog();
  if (pressed("file.new")) a.guardUnsaved([&a]() { a.newProject(); });
  if (pressed("file.render")) a.showExport = true;
  if (pressed("options.typingKeyboard")) a.typingKeyboard = !a.typingKeyboard;
  if (pressed("options.metronome")) a.metronome = !a.metronome;
  if (!a.typingKeyboard && pressed("transport.songMode"))
    a.engine.setMode(a.engine.mode() == PlayMode::Song ? PlayMode::Pattern : PlayMode::Song);
  if (pressed("help.shortcuts")) a.showShortcuts = !a.showShortcuts;
  if (pressed("pattern.new")) {
    ObjectId id = a.doc.addPattern("Pattern " + std::to_string(a.doc.song().patterns.size() + 1));
    if (id) a.selectPattern(id);
  }
  auto toggle = [&](bool& f, const char* name) {
    f = !f;
    if (f) a.focusWindow = name;
  };
  if (pressed("view.playlist")) toggle(a.showPlaylist, "Playlist");
  if (pressed("view.channelRack")) toggle(a.showRack, "Channel rack");
  if (pressed("view.pianoRoll")) toggle(a.showPiano, "Piano roll");
  if (pressed("view.browser")) toggle(a.showBrowser, "Browser");
  if (pressed("view.mixer")) toggle(a.showMixer, "Mixer");
  if (pressed("file.projectInfo")) a.showInfo = !a.showInfo;
  const auto& pats = a.doc.song().patterns;
  int pi = a.doc.song().patternIndex(a.curPattern);
  if (pressed("pattern.next") && pi + 1 < static_cast<int>(pats.size())) a.selectPattern(pats[static_cast<size_t>(pi + 1)].id);
  if (pressed("pattern.previous") && pi > 0) a.selectPattern(pats[static_cast<size_t>(pi - 1)].id);
}

}  // namespace gl

// Gridline — dialogs (file, settings, project info, export, history, about,
// shortcuts, unsaved changes) and global keyboard shortcuts.
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
#include "../../third_party/imgui/imgui.h"
#include "../core/Backup.h"
#include "../core/I18n.h"
#include "../core/flp/Flp.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "ExportUi.h"
#include "../core/ProjectInterop.h"
#include "Theme.h"
#include "MidiUi.h"
#include "SettingsPages.h"
#include "TemplateUi.h"
#include "Widgets.h"
#include "Windows.h"

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

void centerNext(ImVec2 size) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (size.x > 0) ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
}

// Label column for settings-style forms: dim caption, control starts at `x`.
void formLabel(const char* label, float x = 168.0f) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textDim), "%s", label);
  ImGui::SameLine(x);
}

// Right-aligns the next `width` pixels of buttons on the current line.
void alignRight(float width) {
  float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
}

void dialogTitle(App& a, const char* title, const char* subtitle = nullptr) {
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (subtitle) ImGui::TextColored(theme::vec(pal().textDim), "%s", subtitle);
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
  if (fd.dir.empty() || !fs::is_directory(fd.dir, ec)) fd.dir = std::getenv("HOME") ? std::getenv("HOME") : ".";
  const float fh = ImGui::GetFrameHeight();
  if (ui::IconButton("up", ui::Icon::Up, false, fh, "Parent folder")) fd.dir = fs::path(fd.dir).parent_path().string();
  ImGui::SameLine(0, theme::kS1);
  if (ui::IconButton("home", ui::Icon::Folder, false, fh, "Home folder")) fd.dir = std::getenv("HOME") ? std::getenv("HOME") : ".";
  ImGui::SameLine(0, theme::kS1);
  static char dirBuf[1024];
  std::snprintf(dirBuf, sizeof dirBuf, "%s", fd.dir.c_str());
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputText("##dir", dirBuf, sizeof dirBuf, ImGuiInputTextFlags_EnterReturnsTrue) && fs::is_directory(dirBuf, ec)) fd.dir = dirBuf;
  ui::FocusRing();

  const float footer = fh * 2 + theme::kS2 * 3;
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
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (!folderMode) {
    static char nameBuf[512];
    std::snprintf(nameBuf, sizeof nameBuf, "%s", fd.name.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##name", "File name", nameBuf, sizeof nameBuf)) fd.name = nameBuf;
    ui::FocusRing();
  } else {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitText(fd.dir.c_str(), ImGui::GetContentRegionAvail().x));
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const char* okLabel = fd.save ? "Save" : folderMode ? "Choose this folder" : "Open";
  float okW = std::max(96.0f, ImGui::CalcTextSize(okLabel).x + 24);
  alignRight(96 + theme::kS2 + okW);
  bool cancel = ui::TextButton("Cancel", false, 96) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SameLine(0, theme::kS2);
  bool ok = ui::TextButton(okLabel, true, okW);
  if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !fd.name.empty()) ok = true;
  if (ok && chosen.empty()) {
    if (folderMode)
      chosen = fd.dir;
    else if (!fd.name.empty())
      chosen = (fs::path(fd.dir) / fd.name).string();
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
  centerNext(ImVec2(780, 500));
  ImGui::SetNextWindowSizeConstraints(ImVec2(660, 360), ImVec2(1200, 1000));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin("Settings", &a.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
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
  const char* pagesEn[] = {"Audio", "MIDI", "Project", "Backup", "Interface", "General", "Files", "Debug"};
  const char* blurbs[] = {"Output and input devices, sample rate, buffer and recording", "Controllers, keyboards and MIDI out",
                          "Defaults for new projects", "Automatic backups, autosave and restoring a backup", "Theme, size and hints",
                          "Startup, default template and language", "Folders and scripting", "Log and script console"};
  constexpr int kPages = 8;
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
        if (ImGui::Selectable("System default", a.audioDevice.empty())) a.audioDevice.clear(), a.audioReopen = true;
        for (const auto& d : a.audioDevices)
          if (ImGui::Selectable(d.c_str(), d == a.audioDevice)) a.audioDevice = d, a.audioReopen = true;
        ImGui::EndCombo();
      }
      const int rates[] = {44100, 48000, 88200, 96000};
      formLabel("Sample rate");
      for (int r : rates) {
        if (r != rates[0]) ImGui::SameLine(0, theme::kS1);
        if (ui::TextButton(std::to_string(r).c_str(), a.audioSampleRate == r)) {
          a.audioSampleRate = r;
          st.set("audio.sampleRate", r);
          a.audioReopen = true;
        }
      }
      const int bufs[] = {64, 128, 256, 512, 1024, 2048};
      formLabel("Buffer size");
      for (int b : bufs) {
        if (b != bufs[0]) ImGui::SameLine(0, theme::kS1);
        if (ui::TextButton(std::to_string(b).c_str(), a.audioBuffer == b)) {
          a.audioBuffer = b;
          st.set("audio.bufferFrames", b);
          a.audioReopen = true;
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
        if (ImGui::Checkbox("Plugin delay compensation##pdc", &pdc)) {
          st.set("audio.pdc", pdc);
          a.syncEngine();
        }
        ui::Hint("Delays faster paths so tracks with latent effects (look-ahead, linear phase, oversampling) stay in time");
        const int ol = a.engine.outputLatency();
        if (ol > 0) {
          ImGui::SameLine(0, theme::kS2);
          ImGui::TextColored(theme::vec(P.textDim), "%d samples (%.1f ms)", ol, 1000.0 * ol / std::max(1, a.audioSampleRate));
        }
      }
      ui::EndSection();
      ui::BeginSection("INPUT");
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
        if (ImGui::InputTextWithHint("##fldata", "Detect automatically", flData, sizeof flData)) st.set("import.flData", std::string(flData));
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
      static const char* freq[] = {"Never (remind every 10 minutes)", "Rarely (every 15 minutes)", "Occasionally (every 10 minutes)",
                                   "Regularly (every 5 minutes)", "Frequently (5 minutes + before risky edits)",
                                   "Very frequently (every minute + before risky edits)"};
      int f = std::clamp(static_cast<int>(st.getNumber("backup.frequency")), 0, 5);
      ui::BeginSection("HOW OFTEN");
      for (int i = 0; i < 6; ++i)
        if (ImGui::RadioButton(freq[i], f == i)) {
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
      if (ImGui::Checkbox("Save backups next to the project once it has been saved", &inProject)) {
        st.set("backup.location", inProject ? "project" : "global");
        BackupPolicy p = a.backups->policy();
        p.inProjectFolder = inProject;
        a.backups = std::make_unique<BackupManager>(p);
      }
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::vec(P.textFaint), "Global backup folder: %s", a.backups->policy().globalFolder.c_str());
      ImGui::PopTextWrapPos();
      ui::EndSection();
      backupRestorePanel(a, fieldW);  // J-24
    }
    if (page == 4) {
      ui::BeginSection("APPEARANCE");
      bool dark = theme::isDark();
      formLabel("Theme");
      int th = dark ? 0 : 1;
      const char* themes[] = {"Dark", "Light"};
      if (ui::Segmented("theme", themes, 2, &th, 72)) {
        theme::apply(th == 0);
        st.set("ui.darkTheme", th == 0);
      }
      formLabel("Interface scale");
      float scale = static_cast<float>(st.getNumber("ui.scale"));
      ImGui::SetNextItemWidth(fieldW);
      if (ImGui::SliderFloat("##scale", &scale, 0.75f, 2.0f, "%.2fx")) st.set("ui.scale", static_cast<double>(scale));
      ui::Hint("Scales the whole interface; applied as you drag");
      bool hints = st.getBool("ui.showHints");
      formLabel("Status bar");
      if (ImGui::Checkbox("Show hints for the control under the mouse", &hints)) st.set("ui.showHints", hints);
      hintSettingsPanel(a, fieldW);  // J-24
      ui::EndSection();
    }
    if (page == 5) generalSettingsPage(a, fieldW);
    if (page == 6) fileSettingsPage(a, fieldW);
    if (page == 7) debugSettingsPage(a, fieldW);
  }
  ImGui::EndChild();
  ImGui::End();
}

void infoWindow(App& a) {
  if (!a.showInfo) return;
  centerNext(ImVec2(480, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin("Project info", &a.showInfo, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
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
      ImGui::Text("%lld h %02lld min", m / 60, m % 60);
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
  static int pendingPpq = 0;
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
  if (ImGui::BeginPopupModal("##ppqwarn", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::Text("Lowering the timebase to %d PPQ moves %d note or clip", pendingPpq, countPpqRoundings(s, pendingPpq));
    ImGui::Text("positions onto the coarser grid. You can undo this.");
    if (ui::TextButton("Change PPQ", true)) {
      a.doc.setPpq(pendingPpq);
      pendingPpq = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::TextButton("Cancel")) {
      pendingPpq = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ui::EndSection();
  size_t notes = 0;
  for (const auto& p : s.patterns)
    for (const auto& [c, ns] : p.notes) notes += ns.size();
  ImGui::TextDisabled("%zu channels · %zu patterns · %zu clips · %zu notes · %zu mixer tracks", s.channels.size(),
                      s.patterns.size(), s.playlist.size(), notes, s.mixer.size());
  ImGui::End();
}

void historyWindow(App& a) {
  if (!a.showHistory) return;
  ImGui::SetNextWindowSize(ImVec2(300, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Undo history", &a.showHistory)) {
    ImGui::End();
    return;
  }
  const UndoStack& h = a.doc.history();
  auto labels = h.labels();
  if (ImGui::Selectable("Start of history", h.position() == 0)) a.doc.jumpTo(0);
  for (size_t i = 0; i < labels.size(); ++i) {
    bool applied = i < h.position();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(applied ? pal().text : pal().textFaint));
    std::string l = labels[i] + "##" + std::to_string(i);
    if (ImGui::Selectable(l.c_str(), i + 1 == h.position())) a.doc.jumpTo(i + 1);
    ImGui::PopStyleColor();
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
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
  bool vis = ImGui::Begin("About Gridline", &a.showAbout, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
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
    centred(a.fontBold, theme::kFontBody, theme::rgb(0xEDEDED), "Version 0.4.0");
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
  centerNext(ImVec2(560, 600));
  ImGui::SetNextWindowSizeConstraints(ImVec2(420, 300), ImVec2(1000, 1400));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin("Keyboard shortcuts", &a.showShortcuts, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  const char* rows[][2] = {
      {"General", ""}, {"Space", "Play / pause"}, {"L", "Switch pattern / song mode"}, {"Ctrl+N / O / S", "New / open / save project"},
      {"Ctrl+Shift+S", "Save as"}, {"Ctrl+R", "Export audio"}, {"Ctrl+Z / Ctrl+Y", "Undo / redo"}, {"Alt+Z", "Undo history"},
      {"F4", "New pattern"}, {"F5 F6 F7 F9", "Playlist, channel rack, piano roll, mixer"}, {"Alt+F8", "Browser"}, {"F8", "Plugin picker"},
      {"F10", "Settings"}, {"F11", "Project info"}, {"Ctrl+T", "Typing keyboard to piano"}, {"Ctrl+M", "Metronome"},
      {"Numpad + / -", "Next / previous pattern"},
      {"Channel rack", ""}, {"Ctrl+K", "Graph editor (step velocities)"},
      {"Piano roll", ""}, {"P B D E C", "Draw, paint, delete, select, slice tools"}, {"Ctrl+A / Ctrl+D", "Select all / none"},
      {"Ctrl+C / X / V", "Copy / cut / paste"}, {"Del", "Delete selection"}, {"Up / Down (+Shift)", "Transpose (octave)"},
      {"Left / Right", "Move by the snap grid"}, {"Ctrl+Q", "Quantize"}, {"Ctrl+wheel / Alt+wheel", "Zoom / row height"},
      {"Alt+drag", "Bypass snap"}, {"Right-click", "Delete note"},
      {"Playlist", ""}, {"Ctrl+B", "Duplicate selected clips"}, {"Ctrl+C / V", "Copy / paste clips"},
      {"Double-click clip", "Edit its pattern"},
      {"Typing keyboard", ""}, {"Z S X D C V G B H N J M", "Lower octave (C..B)"}, {"Q 2 W 3 E R 5 T 6 Y 7 U", "Upper octave"},
      {"Knobs", ""}, {"Drag / Shift+drag", "Change / fine"}, {"Double-click", "Reset"}, {"Right-click", "Type a value"}};
  const auto& P = pal();
  const float keyCol = std::min(168.0f, ImGui::GetContentRegionAvail().x * 0.4f);
  bool first = true;
  for (auto& r : rows) {
    if (r[1][0] == 0) {
      if (!first) ImGui::Dummy(ImVec2(0, theme::kS2));
      first = false;
      std::string up = r[0];
      for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      ui::Label(up.c_str());
      continue;
    }
    ImVec2 q = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* keys = theme::fitText(r[0], keyCol - 16);
    ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontNumber, 1e9f, 0, keys);
    dl->AddRectFilled(q, ImVec2(q.x + ts.x + 12, q.y + 20), P.surface800, theme::kRadiusSm);
    dl->AddRect(q, ImVec2(q.x + ts.x + 12, q.y + 20), P.line, theme::kRadiusSm);
    dl->AddText(a.fontMono, theme::kFontNumber, ImVec2(q.x + 6, q.y + 3), P.text, keys);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + keyCol, q.y + 3), ImGui::GetContentRegionAvail().x - keyCol,
                        P.textDim, r[1]);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 22));
  }
  menuShortcutTable(a);
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
        if (a.saveProject(a.projectPath) && then) then();
      } else {
        a.saveAs();
        a.fileDialog.onOk = [&a, then](const std::string& p) {
          if (a.saveProject(p) && then) then();
        };
      }
    }
    ImGui::EndPopup();
  }
}

}  // namespace

// Summary after importing an FL Studio project: what came across, what was
// approximated, what has no Gridline equivalent yet.
void importReportWindow(App& a) {
  if (!a.showImportReport) return;
  centerNext(ImVec2(560, 520));
  ImGui::SetNextWindowSizeConstraints(ImVec2(440, 300), ImVec2(900, 900));
  bool vis = ImGui::Begin("FL Studio import", &a.showImportReport, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  if (vis) {
    const auto& P = pal();
    const auto& r = a.importReport;
    dialogTitle(a, r.source.c_str(), "Converted to a new Gridline project. Save it to keep it.");
    ImGui::BeginChild("##rep", ImVec2(0, -ui::DialogFooterHeight()));
    ui::BeginSection("IMPORTED");
    for (const auto& [k, v] : r.counts) {
      formLabel(k.c_str(), 260);
      ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
      ImGui::Text("%d", v);
      ImGui::PopFont();
    }
    ui::EndSection();
    if (!r.unmapped.empty()) {
      ui::BeginSection("NO GRIDLINE EQUIVALENT YET");
      for (const auto& [k, v] : r.unmapped) {
        formLabel(k.c_str(), 260);
        ImGui::TextColored(theme::vec(P.danger), "\xC3\x97%d", v);
      }
      ui::EndSection();
    }
    if (const auto miss = missingFiles(a.doc.song()); !miss.empty()) {  // J-20: link to the files that did not come across
      ui::BeginSection("MISSING FILES");
      ImGui::TextColored(theme::vec(P.danger), "%zu sample(s) / file(s) could not be found", miss.size());
      for (size_t k = 0; k < miss.size() && k < 6; ++k)
        ImGui::TextColored(theme::vec(P.textDim), "\xC2\xB7 %s", theme::fitText(miss[k].c_str(), ImGui::GetContentRegionAvail().x - 12));
      if (miss.size() > 6) ImGui::TextColored(theme::vec(P.textFaint), "and %zu more", miss.size() - 6);
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
    ui::DialogFooter(260);
    if (ui::TextButton("Save as Gridline project...", true, 180)) {
      a.saveAs();
      a.showImportReport = false;
    }
    ImGui::SameLine(0, theme::kS1);
    if (ui::TextButton("Close", false, 72)) a.showImportReport = false;
  }
  ImGui::End();
}

void drawDialogs(App& a) {
  ImGui::GetStyle().HoverDelayNormal = static_cast<float>(std::clamp(a.settings.getNumber("ui.hintDelay"), 0.0, 2.0));
  drawTemplateDialogs(a);  // J-24: new project from template, save as template
  updatePerformance(a);
  drawPerformance(a);
  settingsWindow(a);
  infoWindow(a);
  exportui::draw(a);
  historyWindow(a);
  aboutWindow(a);
  shortcutsWindow(a);
  unsavedPrompt(a);
  importReportWindow(a);
  drawPluginManager(a);
  drawRemoteDialog(a);
  fileDialog(a);
}

void handleGlobalShortcuts(App& a) {
  if (menuShortcuts(a)) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;
  const bool ctrl = io.KeyCtrl || io.KeySuper;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
  if (key(ImGuiKey_Space) && !ctrl) a.engine.playing() ? a.engine.stop() : a.startPlayback();
  if (ctrl && key(ImGuiKey_Z) && !io.KeyShift) a.doc.undo();
  if (ctrl && (key(ImGuiKey_Y) || (key(ImGuiKey_Z) && io.KeyShift))) a.doc.redo();
  if (io.KeyAlt && key(ImGuiKey_Z)) a.showHistory = !a.showHistory;
  if (ctrl && key(ImGuiKey_S)) io.KeyShift ? a.saveAs() : a.save();
  if (ctrl && key(ImGuiKey_O)) a.openDialog();
  if (ctrl && key(ImGuiKey_N)) a.guardUnsaved([&a]() { a.newProject(); });
  if (ctrl && key(ImGuiKey_R)) a.showExport = true;
  if (ctrl && key(ImGuiKey_T)) a.typingKeyboard = !a.typingKeyboard;
  if (ctrl && key(ImGuiKey_M)) a.metronome = !a.metronome;
  if (!ctrl && !a.typingKeyboard && key(ImGuiKey_L))
    a.engine.setMode(a.engine.mode() == PlayMode::Song ? PlayMode::Pattern : PlayMode::Song);
  if (key(ImGuiKey_F1)) a.showShortcuts = !a.showShortcuts;
  if (key(ImGuiKey_F4)) {
    ObjectId id = a.doc.addPattern("Pattern " + std::to_string(a.doc.song().patterns.size() + 1));
    if (id) a.selectPattern(id);
  }
  auto toggle = [&](bool& f, const char* name) {
    f = !f;
    if (f) a.focusWindow = name;
  };
  if (key(ImGuiKey_F5)) toggle(a.showPlaylist, "Playlist");
  if (key(ImGuiKey_F6)) toggle(a.showRack, "Channel rack");
  if (key(ImGuiKey_F7)) toggle(a.showPiano, "Piano roll");
  if (key(ImGuiKey_F8) && io.KeyAlt) toggle(a.showBrowser, "Browser");
  if (key(ImGuiKey_F9)) toggle(a.showMixer, "Mixer");
  if (key(ImGuiKey_F10)) a.showSettings = !a.showSettings;
  if (key(ImGuiKey_F11)) a.showInfo = !a.showInfo;
  const auto& pats = a.doc.song().patterns;
  int pi = a.doc.song().patternIndex(a.curPattern);
  if (key(ImGuiKey_KeypadAdd) && pi + 1 < static_cast<int>(pats.size())) a.selectPattern(pats[static_cast<size_t>(pi + 1)].id);
  if (key(ImGuiKey_KeypadSubtract) && pi > 0) a.selectPattern(pats[static_cast<size_t>(pi - 1)].id);
}

}  // namespace gl

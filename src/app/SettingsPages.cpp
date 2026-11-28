// Gridline — J-24 settings pages (see SettingsPages.h).
#include "CacheUi.h"
#include "../core/ThreadJoin.h"
#include "../core/ThreadName.h"
#include "DisplayConfirmUi.h"
#include "SettingsPages.h"
#include "Keys.h"
#include "ContextMenus.h"
#include <cctype>
#include <cstdio>
#include <filesystem>
#include "../core/Crash.h"
#include "../core/DataFolder.h"
#include "../core/Platform.h"
#include <atomic>
#include <memory>
#include <thread>
#include "CollabUi.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Backup.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Project.h"
#include "App.h"
#include "ScriptUi.h"
#include "Theme.h"
#include "SuiteUi.h"
#include "UpdateUi.h"
#include "UrlImportUi.h"
#include "Widgets.h"

namespace gl {

using theme::pal;
using i18n::tr;
namespace fs = std::filesystem;

namespace {

void label(const char* text, float x = 168.0f) { ui::FormLabelFit(tr(text), x, pal().textDim); }  // J-54: fitted, never under the control

std::string langDir(const App& a) { return a.languageDir(); }

}  // namespace

// ------------------------------------------------------------------ General ----
void generalSettingsPage(App& a, float fieldW) {
  Settings& st = a.settings;
  ui::BeginSection(tr("ON STARTUP"));
  {  // J-141: the welcome launcher (recent projects, templates); also a click on the logo
    bool on = st.getBool("general.launcher");
    if (ImGui::Checkbox(tr("Show the welcome screen"), &on)) st.set("general.launcher", on);
    ui::Hint("Start on the welcome screen: recent projects, templates and the demo song. Projects opened from files go straight in.");
  }
  if (st.getBool("general.launcher")) ui::Label("WITHOUT THE WELCOME SCREEN");
  {
    static const char* modes[] = {"template", "empty", "last"};
    static const char* names[] = {"Open the default template", "Start with an empty project", "Reopen the last project"};
    std::string cur = st.getString("general.startup");
    if (ui::OptionsAsMenus()) {  // K-112b checkpoint 35: a pop-up menu in the Smooth layout
      const char* labels[3];
      int idx = 0;
      for (int i = 0; i < 3; ++i) {
        labels[i] = tr(names[i]);
        if (cur == modes[i]) idx = i;
      }
      label("Start");
      if (ui::OptionRow("startup", labels, 3, &idx, fieldW)) st.set("general.startup", std::string(modes[idx]));
    } else
      for (int i = 0; i < 3; ++i)
        if (ImGui::RadioButton(tr(names[i]), cur == modes[i])) st.set("general.startup", std::string(modes[i]));
  }
  label("Default template");
  {
    std::string key = st.getString("general.defaultTemplate");
    const TemplateInfo* t = key.empty() ? nullptr : a.findTemplate(key);
    std::string preview = key.empty() ? std::string(tr("Starter kit (built in)")) : t ? t->category + " \xE2\x96\xB8 " + t->name : key + " (missing)";
    if (ui::BeginCombo("##deftpl", preview.c_str(), fieldW)) {
      if (ImGui::Selectable(i18n::tr("Starter kit (built in)"), key.empty())) st.set("general.defaultTemplate", std::string());
      std::string cat;
      for (const auto& x : a.templates()) {
        if (x.category != cat) {
          cat = x.category;
          ImGui::SeparatorText(cat.c_str());
        }
        std::string k = x.category + "/" + x.name;
        if (ImGui::Selectable((x.name + "##" + k).c_str(), k == key)) st.set("general.defaultTemplate", k);
      }
      ImGui::EndCombo();
    }
    ui::Hint(keys::withKeys(i18n::N_("Used by File > New (%s) and at startup"), {keys::cmdKey("file.new", "Ctrl+N")}));
  }
  label("");
  if (ui::TextButton(tr("Browse templates..."), false, 0, "File > New from template")) a.showNewProject = true;
  ui::EndSection();

  ui::BeginSection(tr("LANGUAGE"));
  label("Display language");
  {
    auto langs = i18n::available(langDir(a));
    std::string cur = st.getString("general.language");
    std::string name = cur;
    for (const auto& l : langs)
      if (l.code == cur) name = l.name;
    if (ui::BeginCombo("##lang", name.c_str(), fieldW)) {
      for (const auto& l : langs)
        if (ImGui::Selectable((l.name + "##" + l.code).c_str(), l.code == cur))
          ui::confirmLanguage(a, l.code);  // J-77: applied at once, Keep / Revert in both languages
      ImGui::EndCombo();
    }
    ui::Hint("Translations are catalogues in assets/i18n (gettext .po or JSON); Pseudo shows which texts are translatable and where they clip");
  }
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s",
                     tr("Menus, dialogs, settings, hints and editor labels follow the language. To add one, copy assets/i18n/gridline.pot to a file named after the language's code (de.po for German) and translate it."));
  ImGui::PopTextWrapPos();
  ui::EndSection();

  ui::BeginSection(tr("GAME CONTROLLER"));  // K-199: Steam Deck Game mode and any game pad
  {
    static const char* ids[] = {"auto", "on", "off"};
    auto choice = [&](const char* key, const char* id, const char* const names[3], const char* hint) {
      const std::string cur = st.getString(key);
      int idx = 0;
      for (int i = 0; i < 3; ++i)
        if (cur == ids[i]) idx = i;
      if (ui::BeginCombo(id, tr(names[idx]), fieldW)) {
        for (int i = 0; i < 3; ++i)
          if (ImGui::Selectable(tr(names[i]), i == idx)) st.set(key, std::string(ids[i]));
        ImGui::EndCombo();
      }
      ui::Hint(hint);
    };
    static const char* const listen[] = {"Automatic", "On", "Off"};
    label("Controller");
    choice("input.gamepad", "##gamepad", listen,
           "Automatic: in Steam Deck Game mode, and as soon as a controller button is pressed. Its buttons are listed on the Shortcuts page");
    static const char* const dock[] = {"Automatic", "Always docked", "Floating"};
    label("Plugin windows");
    choice("input.dockPlugins", "##dockplugins", dock,
           "Automatic: built-in plugin windows open as tabs beside the main view in Game mode, which shows one window");
  }
  {  // K-422: piano roll note preview while transport plays
    label("Piano roll");
    bool pv = a.settings.getBool("piano.previewWhilePlaying");
    if (ImGui::Checkbox(tr("Preview notes while playing"), &pv)) a.settings.set("piano.previewWhilePlaying", pv);
    ui::Hint("When off (default), clicking or adding notes previews sound only while playback is stopped");
  }
  ui::EndSection();
  updui::settingsSection(a);  // J-89: automatic update check, beta versions, Check now
}

// -------------------------------------------------------------------- Files ----
// K-265: Settings > Files > Data folder. The copy runs on a worker thread; the choice takes effect
// at the next start (core/DataFolder.h).
struct DataFolderCopy {
  std::atomic<double> progress{0};
  std::atomic<bool> done{false}, ok{false}, cancel{false};
  std::string folder, error;  // error: written by the worker before done
  std::thread worker;
  ~DataFolderCopy() {
    cancel = true;
    gl::threads::safeJoin(worker);
  }
};

void dataFolderSection(App& a, float fieldW) {
  namespace df = datafolder;
  static std::unique_ptr<DataFolderCopy> job;
  static char buf[1024];
  static bool init = false;
  static bool justCopied = false;  // the Keep / Revert prompt after a copy
  const df::Info& in = platform::dataFolder();
  const std::string defRoot = platform::defaultConfigRoot();
  if (!init) std::snprintf(buf, sizeof buf, "%s", in.wanted.c_str()), init = true;
  const auto& P = pal();
  ui::BeginSection(tr("DATA FOLDER"));
  label("In use");
  {
    std::string now = in.folder.empty() ? std::string(tr("The default folders")) : in.folder;
    if (in.portable) now += std::string("  (") + tr("portable: beside the program") + ")";
    ui::TextFit(now.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), P.text, true);
  }
  if (in.envOverride) {
    label("");
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("GRIDLINE_CONFIG_HOME is set: the data folder setting is off for this run."));
  } else if (in.missing) {
    label("");
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.danger), "%s %s", tr("Not found, so the default folders are in use:"), in.wanted.c_str());
    ImGui::PopTextWrapPos();
  }
  const bool busy = job && !job->done;
  if (!in.envOverride && !in.portable) {
    label("Move to");
    ImGui::SetNextItemWidth(fieldW);
    ImGui::BeginDisabled(busy);
    ImGui::InputTextWithHint("##datafolder", tr("A full path, for example on an external drive"), buf, sizeof buf);
    ui::FocusRing();
    ImGui::EndDisabled();
    ui::Hint("Settings, caches and downloaded data move under this folder (config, cache, data). It takes effect at the next start");
    label("");
    const std::string want = buf;
    ImGui::BeginDisabled(busy || want.empty() || want == in.folder);
    if (ui::TextButton(tr("Copy my data there and use it"), false, 0)) {
      job = std::make_unique<DataFolderCopy>();
      job->folder = want;
      DataFolderCopy* j = job.get();
      const std::string cfg = platform::configRoot(), cache = platform::cacheRoot(), data = platform::dataRoot();
      j->worker = std::thread([j, cfg, cache, data, defRoot] { gl::threads::setName("gl-settings-job");
        std::string err;
        bool ok = df::copyInto(cfg, cache, data, j->folder,
                               [j](double f, const std::string&) {
                                 j->progress = f;
                                 return !j->cancel.load();
                               },
                               &err);
        if (ok) ok = df::writePointer(defRoot, j->folder, &err);
        j->error = err;
        j->ok = ok;
        j->done = true;
      });
    }
    ImGui::EndDisabled();
    if (!in.folder.empty() && !busy) {
      ImGui::SameLine();
      if (ui::TextButton(tr("Use the default folders"), false, 0, "From the next start; the files in the chosen folder stay where they are")) {
        std::string err;
        if (df::writePointer(defRoot, "", &err)) a.notify(tr("Gridline uses the default folders from the next start"));
        else a.notify(std::string(tr("Could not change the data folder: ")) + err, true);
      }
    }
  }
  if (busy) {
    label("Copying");
    ImGui::ProgressBar(static_cast<float>(job->progress.load()), ImVec2(std::min(fieldW, 260.0f), 0));
    ImGui::SameLine();
    if (ui::TextButton(tr("Cancel"), false, 0)) job->cancel = true;
  } else if (job && job->done) {
    if (job->worker.joinable()) job->worker.join();
    if (job->ok) justCopied = true;
    else a.notify(std::string(tr("The data folder was not changed: ")) + job->error, true);
    job.reset();
  }
  if (justCopied) {
    label("");
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.text), "%s", tr("Copied. Gridline uses the new folder from the next start; your files also stay where they were."));
    ImGui::PopTextWrapPos();
    label("");
    if (ui::TextButton(tr("Keep"), false, 0)) justCopied = false;
    ImGui::SameLine();
    if (ui::TextButton(tr("Revert"), false, 0, "Go on using the folders in use now")) {
      std::string err;
      if (in.folder.empty() ? df::writePointer(defRoot, "", &err) : df::writePointer(defRoot, in.folder, &err)) justCopied = false;
      else a.notify(std::string(tr("Could not change the data folder: ")) + err, true);
    }
  }
  ui::EndSection();
}

void fileSettingsPage(App& a, float fieldW) {
  Settings& st = a.settings;
  ui::BeginSection(tr("FOLDERS"));
  auto folder = [&](const char* what, const std::string& path) {
    label(what);
    ImGui::PushID(what);
    // J-54: paths share one fitted column, so the Show buttons line up (full path on hover).
    const float x0 = ImGui::GetCursorPosX(), pathW = std::max(80.0f, ImGui::GetContentRegionAvail().x - 60 - theme::kS2);
    ui::TextFit(path.c_str(), pathW, pal().text, true);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(x0 + pathW + theme::kS2);
    if (ui::TextButton(tr("Show"), false, 60, path.c_str())) {
      std::error_code ec;
      fs::create_directories(path, ec);
      a.notify(std::string(what) + ": " + path);
    }
    ImGui::PopID();
  };
  folder("User data", a.configDir());
  folder("Templates", a.templatesDir());
  folder("Piano roll scripts", userScriptsDir(a, "pianoroll"));
  folder("Controller scripts", userScriptsDir(a, "controllers"));
  folder("Backups", a.backups->policy().globalFolder);
  ui::EndSection();
  dataFolderSection(a, fieldW);  // K-265
  cacheui::settingsSection(a);  // K-45

  ui::BeginSection(tr("SCRIPTS"));
  label("Scripting");
  bool on = st.getBool("scripts.enabled");
  if (ImGui::Checkbox(tr("Run piano roll and controller scripts"), &on)) st.set("scripts.enabled", on);
  ui::Hint("Scripts run in a sandbox: no system or network access, files only inside their own folder");
  label("Transport");
  bool tr2 = st.getBool("scripts.allowTransport");
  if (ImGui::Checkbox(tr("Project scripts may start, stop and move playback"), &tr2)) st.set("scripts.allowTransport", tr2);
  ui::Hint("Off: project scripts and the console can only read the transport");
  label("Time limit");
  float secs = static_cast<float>(st.getNumber("scripts.maxSeconds"));
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::SliderFloat("##secs", &secs, 0.1f, 10.0f, "%.1f s per run")) st.set("scripts.maxSeconds", static_cast<double>(secs));
  ui::Hint("A script that runs longer is stopped (controller script hooks: at most 0.25 s)");
  label("Instruction limit");
  int mi = static_cast<int>(st.getNumber("scripts.maxMillionInstructions"));
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::SliderInt("##mi", &mi, 1, 500, "%d million")) st.set("scripts.maxMillionInstructions", mi);
  label("Script editor");
  {
    static char ed[512];
    static bool init = false;
    if (!init) std::snprintf(ed, sizeof ed, "%s", st.getString("file.scriptEditor").c_str()), init = true;
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::InputTextWithHint("##ed", i18n::tr("System default text editor"), ed, sizeof ed)) st.set("file.scriptEditor", std::string(ed));
    ui::FocusRing();
    ui::Hint("Command that opens a script for editing (the file path is added at the end)");
  }
  label("");
  if (ui::TextButton(tr("Rescan scripts"))) {
    pianoRollScripts(a, true);
    controllerScripts(a, true);
    shutdownControllerScripts();
    a.notify(std::to_string(pianoRollScripts(a).size()) + " piano roll and " + std::to_string(controllerScripts(a).size()) +
             " controller scripts");
  }
  ui::EndSection();
  suiteui::settingsSection(a);  // J-131: Gridline Suite in the plug-in folders
  urlimportui::settingsSection(a);  // J-192: yt-dlp and ffmpeg for Import from URL
}

// -------------------------------------------------------------------- Debug ----
void debugSettingsPage(App& a, float fieldW) {
  Settings& st = a.settings;
  const auto& P = pal();
  ui::BeginSection(tr("LOG"));
  label("Detail");
  {
    static const char* lv[] = {"Errors", "Warnings", "Info", "Verbose"};
    int cur = std::clamp(static_cast<int>(st.getNumber("debug.logLevel")), 0, 3);
    if (ui::Segmented("##lvl", lv, 4, &cur, 80)) {
      st.set("debug.logLevel", cur);
      a.applySettingsHooks();
    }
  }
  label("Log file");
  bool toFile = st.getBool("debug.logToFile");
  if (ImGui::Checkbox("##tofile", &toFile)) {
    st.set("debug.logToFile", toFile);
    a.applySettingsHooks();
  }
  ImGui::SameLine();
  ui::TextFit((fs::path(a.configDir()) / "gridline.log").string().c_str(), ImGui::GetContentRegionAvail().x, P.textFaint);
  auto entries = logging::entries();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##log", ImVec2(0, 150), ImGuiChildFlags_Borders);
  ImGui::PushFont(a.fontMono, theme::kFontNumber);
  for (const auto& e : entries) {
    ImU32 col = e.level == logging::Level::Error ? P.danger : e.level == logging::Level::Warning ? P.accent : e.level == logging::Level::Verbose ? P.textFaint : P.textDim;
    ImGui::TextColored(theme::vec(col), "%s", logging::format(e).c_str());
  }
  if (entries.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Nothing logged yet."));
  static uint64_t seen = 0;
  if (logging::serial() != seen) {
    seen = logging::serial();
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (ui::TextButton(tr("Copy all"))) {
    std::string all;
    for (const auto& e : entries) all += logging::format(e) + "\n";
    ImGui::SetClipboardText(all.c_str());
  }
  ImGui::SameLine();
  if (ui::TextButton(tr("Clear"))) logging::clear();
  ui::EndSection();
  (void)fieldW;
  ui::BeginSection(tr("CRASH REPORTS"));  // K-39
  ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Reports and recovery copies stay on this computer. Nothing is sent anywhere."));
  auto openFolder = [](const std::string& path) {
    std::string url = "file://";
    for (unsigned char ch : fs::absolute(fs::u8path(path)).generic_u8string()) {
      if (std::isalnum(ch) || ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
        url += static_cast<char>(ch);
      } else {
        char b[4];
        std::snprintf(b, sizeof b, "%%%02X", ch);
        url += b;
      }
    }
    collabui::openUrl(url);
  };
  if (ui::TextButton("Open reports folder")) openFolder(crash::reportsDir(a.configDir()));
  ImGui::SameLine();
  if (ui::TextButton("Open recovery folder")) openFolder(crash::recoveryDir(a.configDir()));
  ui::EndSection();
  ui::BeginSection(tr("SCRIPT CONSOLE"));
  ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Runs Lua against the open project with the controller-script API."));
  drawScriptConsole(a, 120);
  ui::EndSection();
}

// --------------------------------------------------------- panels on pages ----
void projectDefaultsPanel(App& a) {
  Settings& st = a.settings;
  label("Time signature");
  int num = static_cast<int>(st.getNumber("project.defaultNum")), den = static_cast<int>(st.getNumber("project.defaultDen"));
  if (ui::BeginCombo("##dnum", std::to_string(num).c_str(), 64)) {
    for (int n = 1; n <= 16; ++n)
      if (ImGui::Selectable(std::to_string(n).c_str(), n == num)) st.set("project.defaultNum", n);
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::TextColored(theme::vec(pal().textDim), "/");
  ImGui::SameLine();
  if (ui::BeginCombo("##dden", std::to_string(den).c_str(), 64)) {
    for (int d : {2, 4, 8, 16})
      if (ImGui::Selectable(std::to_string(d).c_str(), d == den)) st.set("project.defaultDen", d);
    ImGui::EndCombo();
  }
  label("Pattern length");
  int bars = static_cast<int>(st.getNumber("project.defaultPatternBars"));
  ImGui::SetNextItemWidth(160);
  if (ImGui::SliderInt("##dbars", &bars, 1, 16, bars == 1 ? "1 bar" : "%d bars")) st.set("project.defaultPatternBars", bars);
  label("Author");
  static char author[256];
  static bool init = false;
  if (!init) std::snprintf(author, sizeof author, "%s", st.getString("project.defaultAuthor").c_str()), init = true;
  ImGui::SetNextItemWidth(260);
  if (ImGui::InputTextWithHint("##dauthor", i18n::tr("Your name"), author, sizeof author)) st.set("project.defaultAuthor", std::string(author));
  ui::FocusRing();
  ui::Hint("Filled into Project info of new projects");
}

// GitHub #74: the colour new channels and patterns take (Settings > Project). A preference of this
// computer: it decides what new channels and patterns get, never changes existing ones.
namespace {
Document::NewColors newColorsFromSettings(const Settings& st) {
  Document::NewColors c;
  const std::string ch = st.getString("colors.newChannels"), pt = st.getString("colors.newPatterns");
  c.channels = ch == "one" ? Document::NewColors::One : Document::NewColors::Varied;
  c.patterns = pt == "one" ? Document::NewColors::One : pt == "channel" ? Document::NewColors::Channel : Document::NewColors::Varied;
  c.channel = static_cast<Rgb>(std::clamp(st.getNumber("colors.newChannelColor"), 0.0, 16777215.0)) & 0xFFFFFF;
  c.pattern = static_cast<Rgb>(std::clamp(st.getNumber("colors.newPatternColor"), 0.0, 16777215.0)) & 0xFFFFFF;
  return c;
}

// A colour chip that opens the palette grid; returns true when a colour was picked.
bool colorChip(const char* id, uint32_t* col, bool enabled) {
  const float h = ImGui::GetFrameHeight();
  ImGui::BeginDisabled(!enabled);
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const bool click = ImGui::InvisibleButton(id, ImVec2(h * 1.6f, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(q, ImVec2(q.x + h * 1.6f, q.y + h), theme::rgb(*col, enabled ? 1.0f : 0.35f), theme::kRadius);
  dl->AddRect(q, ImVec2(q.x + h * 1.6f, q.y + h), ImGui::IsItemHovered() ? pal().text : pal().lineStrong, theme::kRadius);
  ImGui::EndDisabled();
  const std::string pop = std::string(id) + "pop";
  if (click) ImGui::OpenPopup(pop.c_str());
  bool picked = false;
  if (ImGui::BeginPopup(pop.c_str())) {
    picked = menus::colorGrid(col);
    ImGui::EndPopup();
  }
  return picked;
}
}  // namespace

void publishNewColors(App& a) {
  Document::NewColors c = newColorsFromSettings(a.settings);
  if (const Channel* ch = a.doc.song().channel(a.selChannel)) c.channelHint = ch->color, c.hasHint = true;
  a.doc.setNewColors(c);
}

void newColorsPanel(App& a, float fieldW) {
  Settings& st = a.settings;
  const Document::NewColors c = newColorsFromSettings(st);
  {
    label("New channels");
    static const char* const ids[] = {"varied", "one"};
    const char* names[] = {tr("Varied colours"), tr("One colour")};
    const char* hints[] = {tr("Each new channel takes the next colour of the palette, so neighbours differ"),
                           tr("Every new channel takes the colour beside this choice")};
    int idx = c.channels == Document::NewColors::One ? 1 : 0;
    if (ui::OptionRow("newchcol", names, 2, &idx, std::max(120.0f, fieldW - 60), hints)) st.set("colors.newChannels", std::string(ids[idx]));
    ImGui::SameLine(0, 8);
    uint32_t col = c.channel;
    if (colorChip("##newchcolchip", &col, idx == 1)) st.set("colors.newChannelColor", static_cast<double>(col & 0xFFFFFF));
    ui::Hint("The colour every new channel takes");
  }
  {
    label("New patterns");
    static const char* const ids[] = {"varied", "one", "channel"};
    const char* names[] = {tr("Varied colours"), tr("One colour"), tr("The channel's colour")};
    const char* hints[] = {tr("Each new pattern takes the next colour of the palette"), tr("Every new pattern takes the colour beside this choice"),
                           tr("A new pattern takes the colour of the selected channel")};
    int idx = c.patterns == Document::NewColors::One ? 1 : c.patterns == Document::NewColors::Channel ? 2 : 0;
    if (ui::OptionRow("newptcol", names, 3, &idx, std::max(120.0f, fieldW - 60), hints)) st.set("colors.newPatterns", std::string(ids[idx]));
    ImGui::SameLine(0, 8);
    uint32_t col = c.pattern;
    if (colorChip("##newptcolchip", &col, idx == 1)) st.set("colors.newPatternColor", static_cast<double>(col & 0xFFFFFF));
    ui::Hint("The colour every new pattern takes");
  }
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", tr("Only new channels and patterns follow this; the ones you have keep their colours (right-click one to change it)."));
  ImGui::PopTextWrapPos();
}

void backupRestorePanel(App& a, float fieldW) {
  Settings& st = a.settings;
  const auto& P = pal();
  ui::BeginSection(tr("AUTOSAVE"));
  bool as = st.getBool("backup.autosave");
  if (ImGui::Checkbox(tr("Save the project file itself every"), &as)) st.set("backup.autosave", as);
  ImGui::SameLine();
  int mins = static_cast<int>(st.getNumber("backup.autosaveMinutes"));
  ImGui::SetNextItemWidth(140);
  if (ImGui::SliderInt("##asmin", &mins, 1, 60, mins == 1 ? "minute" : "%d minutes")) st.set("backup.autosaveMinutes", mins);
  ui::Hint("Only projects that have been saved once; backups keep the older states");
  ui::EndSection();

  ui::BeginSection(tr("RESTORE"));
  auto list = a.backups->list(a.projectPath);
  std::reverse(list.begin(), list.end());  // newest first
  static int pick = 0;
  pick = std::clamp(pick, 0, std::max(0, static_cast<int>(list.size()) - 1));
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     (std::to_string(list.size()) + (list.size() == 1 ? " backup of " : " backups of ") +
                      (a.projectPath.empty() ? std::string("this unsaved project") : fs::path(a.projectPath).filename().string()))
                         .c_str());
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##backups", ImVec2(std::max(fieldW, ImGui::GetContentRegionAvail().x), 110), ImGuiChildFlags_Borders);
  for (size_t i = 0; i < list.size(); ++i) {
    std::error_code ec;
    auto sz = fs::file_size(list[i], ec);
    char row[512];
    std::snprintf(row, sizeof row, "%s   %.0f KB##b%zu", fs::path(list[i]).filename().string().c_str(), ec ? 0.0 : static_cast<double>(sz) / 1024.0, i);
    if (ImGui::Selectable(row, pick == static_cast<int>(i))) pick = static_cast<int>(i);
  }
  if (list.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("No backups yet: they are written while you work."));
  ImGui::EndChild();
  ImGui::PopStyleColor();
  const bool any = !list.empty();
  if (ui::TextButton(tr("Restore"), false, 0, "Replace the project with the selected backup (undoable)") && any) {
    Song s;
    std::string err;
    if (loadProject(list[static_cast<size_t>(pick)], s, &err)) {
      a.engine.stopToStart();
      a.doc.editSong("restore backup", [&s](Song& x) { x = s; });
      a.notify("Restored " + fs::path(list[static_cast<size_t>(pick)]).filename().string());
    } else {
      a.notify("Could not read the backup: " + err, true);
    }
  }
  ImGui::SameLine();
  if (ui::TextButton(tr("Open as new project"), false, 0, "Opens the backup as a separate, unsaved project") && any) {
    std::string path = list[static_cast<size_t>(pick)];
    a.guardUnsaved([&a, path]() {
      Song s;
      std::string err;
      if (loadProject(path, s, &err))
        a.startProject(std::move(s));
      else
        a.notify("Could not read the backup: " + err, true);
    });
  }
  ImGui::SameLine();
  if (ui::TextButton(tr("Show folder"))) a.notify("Backups: " + a.backups->folderFor(a.projectPath));
  ui::EndSection();
}

void hintSettingsPanel(App& a, float fieldW) {
  Settings& st = a.settings;
  label("Tooltip delay");
  float d = static_cast<float>(st.getNumber("ui.hintDelay"));
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::SliderFloat("##hintdelay", &d, 0.0f, 2.0f, "%.2f s")) st.set("ui.hintDelay", static_cast<double>(d));
  ui::Hint("How long the mouse rests on a control before its tooltip appears (the hint bar updates at once)");
}

}  // namespace gl

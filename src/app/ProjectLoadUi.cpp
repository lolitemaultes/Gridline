// Gridline — J-167: project loads (App side) and the loading window (see ProjectLoadUi.h).
//
// App::openProject -> startProjectLoad: the old project's sound stops, the launcher steps
// aside and the loader starts (core of the work: engine/ProjectLoad.h). Each frame App::tick
// calls pollProjectLoad: a slice of work, then the swap once everything is prepared
// (completeProjectLoad, one frame: Document::reset, the prepared plugins handed to the
// builder, a build that finds them ready, the engine gets the new program atomically).
// Cancel drops the loader's work; the open project was never touched.
#include "CacheUi.h"
#include "Keys.h"
#include "ProjectLoadUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../collab/Session.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/ProjectInterop.h"
#include "App.h"
#include "CrashUi.h"
#include "../core/Crash.h"
#include "CollabUi.h"
#include "DevSignal.h"
#include "DialogLayout.h"
#include "ExportUi.h"
#include "LauncherUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

namespace fs = std::filesystem;
using projload::Level;
using projload::Phase;

namespace {

constexpr double kStepBudget = 0.012;  // seconds of loading work per frame (the rest draws the interface)

std::string fileNameOf(const std::string& path) {
  const std::string n = fs::u8path(path).filename().u8string();
  return n.empty() ? path : n;
}

}  // namespace

void App::startProjectLoad(const std::string& path, bool collab) {
  // The newest request wins (a startup "last project" followed by a command-line project).
  if (projectLoading())
    cancelProjectLoad();
  else if (projectLoadBlocking())
    dismissProjectLoad();
  load_ = LoadContext{};
  load_.shownAs = path;
  load_.collab = collab;
  load_.homeBefore = home;
  // The old project's sound stops cleanly (it stays exactly as it is until the swap: Cancel
  // returns to it, transport position included).
  engine.pause();
  engine.panic(true);
  if (home.visible()) home.projectOpened(now);  // the launcher gives way at once (back on Cancel)
  projload::Request r;
  r.path = path;
  r.flData = settings.getString("import.flData");
  r.sampleRate = engine.sampleRate();
  r.pdc = settings.getBool("audio.pdc");
  r.sketch = !embedded;
  if (const char* d = std::getenv("GRIDLINE_DEV_SLOW_LOAD_MS")) r.devDelayMs = std::clamp(std::atoi(d), 0, 60000);  // dev / tests
  if (const char* d = std::getenv("GRIDLINE_DEV_SLOW_READ_MS")) r.devReadDelayMs = std::clamp(std::atoi(d), 0, 60000);  // dev / tests
  if (const char* d = std::getenv("GRIDLINE_DEV_STALL_SECONDS")) r.stallSeconds = std::atof(d);  // dev / tests
  r.askProject = !embedded;
  r.collabProbe = [](const std::string& file) { return !collab::Session::folderOfProjectFile(file).empty(); };
  projectLoad.start(std::move(r));
  logging::info("app", "Opening " + path);
  crash::crumb("project open");
  for (const char* n : {"project-loaded", "project-load-cancelled", "project-load-failed", "project-load-preparing"}) devsig::forget(n);
  devsig::reach("project-load-started");
  if (embedded) finishProjectLoad();  // the host's thread waits (no frames of our own to draw it in)
}

void App::pollProjectLoad() {
  if (projectLoad.active()) {
    projectLoad.step(kStepBudget);
    if (projectLoad.phase() == Phase::Preparing) devsig::reach("project-load-preparing");
  }
  if (projectLoad.phase() == Phase::Ready) {
    completeProjectLoad();
  } else if (projectLoad.phase() == Phase::Failed && !load_.failReported) {
    load_.failReported = true;
    logging::error("app", "Could not open " + load_.shownAs + ": " + projectLoad.error());
    uiSound(uisound::Event::Error);
    devsig::reach("project-load-failed");
    if (embedded) {  // no window: a notification card
      notify(projectLoad.error(), true, load_.shownAs);
      dismissProjectLoad();
    }
  }
}

void App::finishProjectLoad() {
  if (projectLoad.active()) projectLoad.finish();
  pollProjectLoad();
}

void App::cancelProjectLoad() {
  if (!projectLoad.active()) return;
  projectLoad.cancel();
  logging::info("app", "Opening " + load_.shownAs + " cancelled");
  projectLoad.clear();
  home = load_.homeBefore;  // the launcher comes back if it was up
  devsig::reach("project-load-cancelled");
}

void App::dismissProjectLoad() {
  if (projectLoad.phase() != Phase::Failed) return;
  projectLoad.clear();
  home = load_.homeBefore;
}

void App::shutdownProjectLoad() {
  if (projectLoad.active()) home = load_.homeBefore;
  projectLoad.shutdown();
}

void App::completeProjectLoad() {
  projload::Result& r = projectLoad.result();
  const std::string shownAs = load_.shownAs;
  const std::string name = fileNameOf(shownAs);
  std::vector<std::string> warnings;
  for (const auto& l : projectLoad.log())
    if (l.level == Level::Warning) warnings.push_back(l.text());
  const int ms = static_cast<int>(projectLoad.elapsed() * 1000.0);
  crash::BusyScope busy("installing the opened project");
  crash::crumb("project loaded");
  engine.stopToStart();
  engine.panic(false);  // (the project is replaced: no fade over the new one)
  // The prepared plugins go to the builder: the build below (or the collaborative open's)
  // finds every one ready, so the swap itself starts none.
  builder.adoptPreloaded(std::move(projectLoad.prepared()));
  if (load_.collab || r.collabOnDrive) {  // J-111: the session opens the shared project (a quick read of the same file)
    const std::string file = r.projectFile;
    projectLoad.clear();
    bool opened = false;
    collabui::openIfCollab(*this, file, &opened);
    if (opened) {
      home.projectOpened(now);
      uiSound(uisound::Event::Open);
      devsig::reach("project-loaded");
    } else {
      home = load_.homeBefore;
      devsig::reach("project-load-failed");
    }
    return;
  }
  collabui::closeSession(*this);  // J-111
  doc.reset(std::move(r.song));
  cacheui::projectChanged(*this, std::move(r.copyUse));  // K-45: this copy stays claimed, the closed project's can go
  trimSamplesAt_ = now + 1.0;  // J-30: the old project's samples go
  // J-52: an FL Studio project is not a Gridline file yet: Save asks where and in which format,
  // so the .flp is never replaced unasked. A zip opens from its unpacked project; K-42: when that
  // copy is in the local cache (the archive is on a drive) it is no place to save to, so Save asks
  // too, and the archive itself is what stays in the recent list. K-45: every archive unpacks there.
  const bool saveAsks = r.fromFl || r.inCache;
  const std::string recent = saveAsks ? shownAs : r.projectFile;
  projectPath = saveAsks ? std::string() : r.projectFile;
  projectFormat = SaveFormat::Gridline;
  openedFrom = saveAsks ? shownAs : std::string();
  const bool recovered = crashui::isRecovering();
  if (recovered) crashui::recoveredLoaded(*this);  // K-39: Save goes to the project's real home; not a recent file
  else addRecent(recent);
  if (r.hasSketch && !recovered && !embedded && !recent.empty()) launcherui::rememberSketch(*this, recent, std::move(r.sketch));  // J-141
  curPattern = doc.song().patterns.empty() ? kNoId : doc.song().patterns[0].id;
  selChannel = doc.song().channels.empty() ? kNoId : doc.song().channels[0].id;
  selChannels = {selChannel};
  selMixer = doc.song().masterId();
  playlist.selected.clear();
  engine.setMode(doc.song().playlist.empty() ? PlayMode::Pattern : PlayMode::Song);
  rackReveal.quietSwitch(engine.mode());  // J-77: opening a project does not count as choosing PAT
  syncEngine();
  home.projectOpened(now);
  if (r.fromFl) {
    importReport = ImportReport{};
    importReport.source = name;
    for (const auto& [k, v] : r.report.counts) importReport.counts.emplace_back(k, v);
    for (const auto& [k, v] : r.report.unmapped) importReport.unmapped.emplace_back(k, v);
    for (const auto& [k, v] : r.report.mapped) importReport.mapped.emplace_back(k, v);
    importReport.warnings = r.report.warnings;
    showImportReport = true;
    notify("Opened " + importReport.source);
    // J-48: warnings or unmapped plugins: the report needs a look.
    uiSound(importReport.warnings.empty() && importReport.unmapped.empty() ? uisound::Event::Open : uisound::Event::Warning);
  } else if (!warnings.empty()) {
    // The warnings stay on a card (all of them in its details, copied with a click).
    std::string details;
    for (const auto& w : warnings) details += (details.empty() ? "" : "\n") + w;
    notify("Opened " + name + ": " + warnings.front() + (warnings.size() > 1 ? " (+" + std::to_string(warnings.size() - 1) + ")" : ""), false,
           details);
    uiSound(uisound::Event::Warning);
  } else {
    notify("Opened " + name);
    uiSound(uisound::Event::Open);
  }
  const bool zipped = r.fromZip;
  std::vector<std::string> missingRefs = std::move(r.missingRefs);  // (checked on the loader's worker)
  logging::info("app", "Opened " + shownAs + " in " + std::to_string(ms) + " ms, " + std::to_string(warnings.size()) + " warning(s)");
  projectLoad.clear();
  if (zipped && !missingRefs.empty()) exportui::openMissingSamples(*this, std::move(missingRefs));
  devsig::reach("project-loaded");
}

namespace projloadui {

namespace {

constexpr const char* kPopup = "##projectload";
constexpr float kWidth = 580.0f;

ImU32 levelColour(Level l) {
  const auto& P = theme::pal();
  switch (l) {
    case Level::Info: return P.textDim;
    case Level::Slow: return P.hot;
    case Level::Warning: return P.meterMid;
    case Level::Error: return P.danger;
  }
  return P.textDim;
}

// A log line in the interface language: the translated template with its argument and time.
std::string lineText(const projload::LogLine& l) {
  char b[640];
  std::snprintf(b, sizeof b, i18n::tr(l.fmt), l.arg.c_str());
  std::string s = b;
  if (l.ms >= 0) {
    char t[48];
    std::snprintf(t, sizeof t, i18n::tr("%d ms"), l.ms);
    s += std::string(" (") + t + ")";
  }
  return s;
}

std::string nowText(const projload::Now& n) {
  using K = projload::Now::Kind;
  char b[640];
  switch (n.kind) {
    case K::Idle: return {};
    case K::Reading: return i18n::tr("Reading the project file");
    case K::Unpacking:
      if (n.count > 0 && !n.name.empty()) {
        std::snprintf(b, sizeof b, i18n::tr("Unpacking %s (%d of %d)"), n.name.c_str(), n.index, n.count);
        return b;
      }
      return i18n::tr("Unpacking the zipped project");
    case K::Choosing: return i18n::tr("Which project in this archive should open?");
    case K::Converting: return i18n::tr("Converting the FL Studio project");
    case K::Listing: return i18n::tr("Listing samples and plugins");
    case K::Building: return i18n::tr("Building the mixer, automation and arrangement");
    case K::Failed: return i18n::tr("Could not open the project");
    case K::Plugin: std::snprintf(b, sizeof b, i18n::tr("Starting plugin %s (%d of %d)"), n.name.c_str(), n.index, n.count); return b;
    case K::Sample: std::snprintf(b, sizeof b, i18n::tr("Loading sample %s (%d of %d)"), n.name.c_str(), n.index, n.count); return b;
  }
  return {};
}

// Three dots that pulse in turn: the window is alive even while a step has no count.
void activityDots(ImDrawList* dl, ImVec2 c, float t, ImU32 col) {
  for (int i = 0; i < 3; ++i) {
    const float ph = std::fmod(t * 2.2f - static_cast<float>(i) * 0.22f, 1.0f);
    const float a = 0.25f + 0.75f * std::max(0.0f, std::sin(ph * 3.14159265f));
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(col);
    v.w *= a;
    dl->AddCircleFilled(ImVec2(c.x + static_cast<float>(i) * 7.0f, c.y), 2.2f, ImGui::ColorConvertFloat4ToU32(v));
  }
}

std::string sizeText(uint64_t bytes) {
  char b[48];
  const double v = static_cast<double>(bytes);
  if (v >= 1024.0 * 1024.0 * 1024.0) std::snprintf(b, sizeof b, i18n::tr("%.2f GB"), v / (1024.0 * 1024.0 * 1024.0));
  else if (v >= 1024.0 * 1024.0) std::snprintf(b, sizeof b, i18n::tr("%.1f MB"), v / (1024.0 * 1024.0));
  else std::snprintf(b, sizeof b, i18n::tr("%.0f KB"), v / 1024.0);
  return b;
}

// "1.20 GB of 3.08 GB, 42.0 MB/s, about 1 min left"
std::string bytesLine(const projload::Loader& L) {
  char b[200];
  std::snprintf(b, sizeof b, i18n::tr("%s of %s"), sizeText(L.bytesDone()).c_str(), sizeText(L.bytesTotal()).c_str());
  std::string s = b;
  const double rate = L.bytesPerSecond();
  if (rate > 1.0 && L.bytesDone() < L.bytesTotal()) {
    std::snprintf(b, sizeof b, i18n::tr("%s/s"), sizeText(static_cast<uint64_t>(rate)).c_str());
    s += std::string(", ") + b;
    const double left = static_cast<double>(L.bytesTotal() - L.bytesDone()) / rate;
    if (left >= 90.0) std::snprintf(b, sizeof b, i18n::tr("about %d min left"), static_cast<int>(left / 60.0 + 0.5));
    else std::snprintf(b, sizeof b, i18n::tr("about %d s left"), std::max(1, static_cast<int>(left + 0.5)));
    s += std::string(", ") + b;
  }
  return s;
}

// The progress bar: a recessed track with the accent fill.
void progressBar(float frac, float w) {
  const auto& P = theme::pal();
  const float h = 8.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), P.field, h * 0.5f);
  dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), P.line, h * 0.5f);
  const float fw = std::clamp(frac, 0.0f, 1.0f) * w;
  if (fw >= 1.0f) dl->AddRectFilled(p0, ImVec2(p0.x + std::max(fw, h), p0.y + h), P.accent, h * 0.5f);
  ImGui::Dummy(ImVec2(w, h));
}

bool gWarningsOnly = false;  // the log shows only warnings, errors and slow plugins

void drawLog(App& a, const std::vector<projload::LogLine>& all, float h) {
  const auto& P = theme::pal();
  static std::vector<const projload::LogLine*> lines;
  lines.clear();
  for (const auto& l : all)
    if (!gWarningsOnly || l.level != Level::Info) lines.push_back(&l);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.well));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, theme::kS1 + 2));
  ImGui::BeginChild("##loadlog", ImVec2(0, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  static size_t shown = 0;
  static bool filtered = false;
  const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
  ImGui::PushFont(a.fontMono, theme::kFontCaption);
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(lines.size()));
  while (clip.Step())
    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
      const auto& l = *lines[static_cast<size_t>(i)];
      char t[32];
      std::snprintf(t, sizeof t, "%6.2f", l.at);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", t);
      ImGui::SameLine(0, theme::kS2);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 3.0f, p.y + ImGui::GetTextLineHeight() * 0.5f), 3.0f,
                                                  l.level == Level::Info ? P.textFaint : levelColour(l.level));
      ImGui::SetCursorScreenPos(ImVec2(p.x + 12.0f, p.y));
      // Long lines (paths) are fitted with the ellipsis in the middle; hovering shows them whole.
      ui::PathFit(lineText(l).c_str(), ImGui::GetContentRegionAvail().x, l.level == Level::Info ? P.text : levelColour(l.level));
    }
  ImGui::PopFont();
  // Follows the newest line unless scrolled back (a filter switch starts at the end too).
  if ((lines.size() != shown && (atEnd || shown == 0)) || filtered != gWarningsOnly) ImGui::SetScrollHereY(1.0f);
  shown = lines.size();
  filtered = gWarningsOnly;
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

}  // namespace

// An archive holding several equal projects: the list, the first (the best guess) selected.
void drawChoice(App& a, float w) {
  projload::Loader& L = a.projectLoad;
  const auto& P = theme::pal();
  static int selected = 0;
  const std::vector<std::string> names = L.choices();
  if (selected >= static_cast<int>(names.size())) selected = 0;
  ui::TextFit(i18n::tr("This archive holds several projects. Which one should open?"), w, P.text);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const float rowH = ImGui::GetTextLineHeight() + theme::kS2;
  const float listH = std::min(static_cast<float>(names.size()) * rowH + 2 * theme::kS1 + 2.0f, 220.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.well));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS1, theme::kS1));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::BeginChild("##loadchoice", ImVec2(w, listH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  for (int i = 0; i < static_cast<int>(names.size()); ++i) {
    ImGui::PushID(i);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rw = ImGui::GetContentRegionAvail().x;
    const bool hit = ImGui::InvisibleButton("##r", ImVec2(rw, rowH));
    const bool hov = ImGui::IsItemHovered();
    if (hit) selected = i;
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) L.choose(i);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (i == selected || hov)
      dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rowH), i == selected ? theme::withAlpha(P.accent, 0.3f) : P.surface800, theme::kRadiusSm);
    ui::drawIcon(dl, ui::Icon::File, ImVec2(p.x + 12.0f, p.y + rowH * 0.5f), 12, P.textDim);
    theme::textEllipsis(dl, a.fontRegular, ImGui::GetFontSize(), ImVec2(p.x + 26.0f, p.y + theme::kS1), rw - 32.0f, P.text, names[static_cast<size_t>(i)].c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const float bw = 110.0f;
  const float y = ImGui::GetCursorPosY();
  ImGui::SetCursorPos(ImVec2(theme::kS6 + w - 2 * bw - theme::kS2, y));
  if (ui::TextButton("Cancel", false, bw, "Stop opening and go back to the project as it was") || ::gl::keys::escapePressed()) {
    selected = 0;
    a.cancelProjectLoad();
    return;
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Open", true, bw) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    L.choose(selected);
    selected = 0;
  }
}

void draw(App& a) {
  projload::Loader& L = a.projectLoad;
  const bool want = a.projectLoadBlocking();
  if (want && !ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);  // (closes any other popup or menu)
  if (!want && !ImGui::IsPopupOpen(kPopup)) return;
  const bool failed = L.phase() == Phase::Failed;
  // Input is blocked from the first frame; the window (and the dimming) shows after a moment.
  const float fade = failed ? 1.0f : std::clamp(static_cast<float>((L.elapsed() - kShowAfter) / 0.15), 0.0f, 1.0f);
  ImGui::GetCurrentContext()->DimBgRatio = want ? fade : 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, want ? fade : 0.0f);
  ui::PlaceNextDialog(ImVec2(kWidth, 0), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS4 + 2));
  const bool open = ImGui::BeginPopupModal(kPopup, nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoMove);
  ImGui::PopStyleVar();
  if (!open) {
    ImGui::PopStyleVar();
    return;
  }
  if (!want) {  // finished or cancelled: gone at once
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar();
    return;
  }
  const auto& P = theme::pal();
  const float w = kWidth - 2 * theme::kS6;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Title and elapsed time.
  {
    ImGui::PushFont(a.fontBold, theme::kFontTitle);
    ImGui::TextUnformatted(failed ? i18n::tr("Could not open the project") : i18n::tr("Opening project"));
    ImGui::PopFont();
    char t[32];
    std::snprintf(t, sizeof t, i18n::tr("%.1f s"), L.elapsed());
    ImGui::PushFont(a.fontMono, theme::kFontCaption);
    const float tw = ImGui::CalcTextSize(t).x;
    ImGui::SameLine(theme::kS6 + w - tw);
    if (!failed) ImGui::TextColored(theme::vec(P.textDim), "%s", t);
    else ImGui::NewLine();
    ImGui::PopFont();
  }
  ui::TextFit(fileNameOf(a.projectLoadName()).c_str(), w, P.textDim);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (L.awaitingChoice()) {
    drawChoice(a, w);
    if (!a.projectLoadBlocking()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar();
    return;
  }
  if (failed) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.danger));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(i18n::trMessage(L.error()).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  } else {
    // Progress over the real work units, then the step running now.
    const int total = L.unitsTotal(), done = L.unitsDone();
    progressBar(static_cast<float>(L.overall()), w);
    const std::string step = nowText(L.now());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    activityDots(dl, ImVec2(p.x + 3.0f, p.y + ImGui::GetTextLineHeight() * 0.5f + 2.0f), static_cast<float>(ImGui::GetTime()), P.accent);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 24.0f, p.y + 2.0f));
    char pc[48] = "";
    if (total > 0 || L.overall() > 0.0) std::snprintf(pc, sizeof pc, i18n::tr("%d%%"), static_cast<int>(std::floor(L.overall() * 100.0)));
    const float pw = ImGui::CalcTextSize(pc).x;
    ui::TextFit(step.c_str(), w - 24.0f - pw - theme::kS2, P.text);
    if (pc[0]) {
      ImGui::SameLine(theme::kS6 + w - pw);
      ImGui::TextColored(theme::vec(P.textDim), "%s", pc);
    }
    char counts[160] = "";
    if (total > 0)
      std::snprintf(counts, sizeof counts, i18n::tr("Samples %d of %d, plugins %d of %d"), L.samplesDone(), L.samplesTotal(), L.pluginsDone(),
                    L.pluginsTotal());
    const bool reading = L.phase() == Phase::Reading && L.bytesTotal() > 0;
    if (L.stalledFor() > 0.0) {  // K-42: a read or a sample on a drive has gone quiet: never a silent wait
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.meterMid));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
      ImGui::TextUnformatted(i18n::tr("The drive is not answering. It may be offline or still downloading. Cancel goes back to your project."));
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
    } else {
      ui::TextFit(reading ? bytesLine(L).c_str() : counts[0] ? counts : " ", w, P.textFaint);
    }
    (void)done;
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  drawLog(a, L.log(), 210.0f);
  // Footer: warnings so far, then Cancel (Esc) or Close.
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const float bw = 110.0f;
  const float y = ImGui::GetCursorPosY();
  if (L.log().size() <= 1) gWarningsOnly = false;  // a new load starts with the whole log
  if (L.warnings() > 0) {  // the count; a click shows only the warnings (and back)
    char b[64];
    std::snprintf(b, sizeof b, L.warnings() == 1 ? i18n::tr("%d warning") : i18n::tr("%d warnings"), L.warnings());
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.meterMid));
    if (ui::TextButton(b, gWarningsOnly, 0, gWarningsOnly ? "Show the whole log" : "Show only the warnings")) gWarningsOnly = !gWarningsOnly;
    ImGui::PopStyleColor();
  } else if (!failed && L.blockingNext()) {
    // A plugin's own start cannot be interrupted: Cancel takes effect right after it.
    ImGui::SetCursorPosY(y + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
    ui::TextFit(i18n::tr("Cancel takes effect once this plugin has started"), w - bw - theme::kS2, P.textFaint);
  }
  ImGui::SetCursorPos(ImVec2(theme::kS6 + w - bw, y));
  if (failed) {
    if (ui::TextButton("Close", true, bw) || ::gl::keys::escapePressed() || ImGui::IsKeyPressed(ImGuiKey_Enter)) a.dismissProjectLoad();
  } else if (ui::TextButton("Cancel", false, bw, "Stop opening and go back to the project as it was") || ::gl::keys::escapePressed()) {
    a.cancelProjectLoad();
  }
  if (!a.projectLoadBlocking()) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  ImGui::PopStyleVar();
}

void setWarningsOnly(bool on) { gWarningsOnly = on; }

void devReport(App& a, int frame) {
  const projload::Loader& L = a.projectLoad;
  static const char* kPhase[] = {"idle", "reading", "preparing", "ready", "failed", "cancelled"};
  ImGuiContext& g = *ImGui::GetCurrentContext();
  std::string popups;
  for (const auto& p : g.OpenPopupStack)
    if (p.Window) popups += (popups.empty() ? "" : ",") + std::string(p.Window->Name);
  const bool shown = a.projectLoadBlocking() && (L.phase() == Phase::Failed || L.elapsed() >= kShowAfter);
  int warn = 0, slow = 0, errors = 0;
  for (const auto& l : L.log()) {
    warn += l.level == Level::Warning;
    slow += l.level == Level::Slow;
    errors += l.level == Level::Error;
  }
  std::printf("loadstate frame %d: phase %s units %d/%d samples %d/%d plugins %d/%d log %zu warnings %d slow %d errors %d shown %d now \"%s\" popups [%s]\n",
              frame, kPhase[static_cast<int>(L.phase())], L.unitsDone(), L.unitsTotal(), L.samplesDone(), L.samplesTotal(), L.pluginsDone(),
              L.pluginsTotal(), L.log().size(), warn, slow, errors, shown ? 1 : 0, L.current().c_str(), popups.c_str());
  if (L.bytesTotal() > 0 || L.awaitingChoice())
    std::printf("  bytes %llu/%llu overall %.3f stalled %.0f choices %zu\n", static_cast<unsigned long long>(L.bytesDone()),
                static_cast<unsigned long long>(L.bytesTotal()), L.overall(), L.stalledFor(), L.choices().size());
  for (const auto& l : L.log()) std::printf("  log %.2f %d %s\n", l.at, static_cast<int>(l.level), l.text().c_str());
  if (!a.toasts.empty()) std::printf("  toast \"%s\" details \"%s\"\n", a.toasts.back().english.c_str(), a.toasts.back().details.c_str());
  std::fflush(stdout);
}

}  // namespace projloadui

}  // namespace gl

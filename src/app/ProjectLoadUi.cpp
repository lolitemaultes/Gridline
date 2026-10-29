// Gridline — J-167: project loads (App side) and the loading window (see ProjectLoadUi.h).
//
// App::openProject -> startProjectLoad: the old project's sound stops, the launcher steps
// aside and the loader starts (core of the work: engine/ProjectLoad.h). Each frame App::tick
// calls pollProjectLoad: a slice of work, then the swap once everything is prepared
// (completeProjectLoad, one frame: Document::reset, the prepared plugins handed to the
// builder, a build that finds them ready, the engine gets the new program atomically).
// Cancel drops the loader's work; the open project was never touched.
#include "ProjectLoadUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/ProjectInterop.h"
#include "App.h"
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
  projectLoad.start(std::move(r));
  logging::info("app", "Opening " + path);
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
  engine.stopToStart();
  engine.panic(false);  // (the project is replaced: no fade over the new one)
  // The prepared plugins go to the builder: the build below (or the collaborative open's)
  // finds every one ready, so the swap itself starts none.
  builder.adoptPreloaded(std::move(projectLoad.prepared()));
  if (load_.collab) {  // J-111: the session opens the shared project (a quick read of the same file)
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
  trimSamplesAt_ = now + 1.0;  // J-30: the old project's samples go
  // J-52: an FL Studio project is not a Gridline file yet: Save asks where and in which format,
  // so the .flp is never replaced unasked. A zip opens from its unpacked project.
  const std::string recent = r.fromFl ? shownAs : r.projectFile;
  projectPath = r.fromFl ? std::string() : r.projectFile;
  projectFormat = SaveFormat::Gridline;
  openedFrom = r.fromFl ? shownAs : std::string();
  addRecent(recent);
  if (r.hasSketch && !embedded && !recent.empty()) launcherui::rememberSketch(*this, recent, std::move(r.sketch));  // J-141
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
  logging::info("app", "Opened " + shownAs + " in " + std::to_string(ms) + " ms, " + std::to_string(warnings.size()) + " warning(s)");
  projectLoad.clear();
  if (zipped && !missingFiles(doc.song()).empty()) exportui::openMissingSamples(*this);
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
    case K::Unpacking: return i18n::tr("Unpacking the zipped project");
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
  if (failed) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.danger));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(i18n::trMessage(L.error()).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  } else {
    // Progress over the real work units, then the step running now.
    const int total = L.unitsTotal(), done = L.unitsDone();
    progressBar(static_cast<float>(L.progress()), w);
    const std::string step = nowText(L.now());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    activityDots(dl, ImVec2(p.x + 3.0f, p.y + ImGui::GetTextLineHeight() * 0.5f + 2.0f), static_cast<float>(ImGui::GetTime()), P.accent);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 24.0f, p.y + 2.0f));
    char pc[48] = "";
    if (total > 0) std::snprintf(pc, sizeof pc, i18n::tr("%d%%"), static_cast<int>(std::floor(L.progress() * 100.0)));
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
    ui::TextFit(counts[0] ? counts : " ", w, P.textFaint);
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
    if (ui::TextButton("Close", true, bw) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) a.dismissProjectLoad();
  } else if (ui::TextButton("Cancel", false, bw, "Stop opening and go back to the project as it was") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
  for (const auto& l : L.log()) std::printf("  log %.2f %d %s\n", l.at, static_cast<int>(l.level), l.text().c_str());
  if (!a.toasts.empty()) std::printf("  toast \"%s\" details \"%s\"\n", a.toasts.back().english.c_str(), a.toasts.back().details.c_str());
  std::fflush(stdout);
}

}  // namespace projloadui

}  // namespace gl

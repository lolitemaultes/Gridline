// Gridline — J-80: FL Studio export job, progress and report windows (see FlpExportUi.h).
#include "FlpExportUi.h"
#include "Keys.h"
#include "../core/Crash.h"
#include "../core/Platform.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Project.h"
#include "../core/flp/Flp.h"
#include "../core/flp/FlpExport.h"
#include "../core/flp/FlpPackage.h"
#include "../core/flp/FlpOrigin.h"
#include "../core/flp/FlpWrapper.h"
#include "App.h"
#include "DialogLayout.h"
#include "HostedUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::flpexport {

namespace fs = std::filesystem;

namespace {

bool startWith(App& a, const std::string& path, bool zip, std::function<void(bool)> done, flp::GridlinePlugins mode);

struct Job {
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> progress{0};
  std::mutex mu;
  // Guarded by mu.
  flp::ExportReport report;
  bool ok = false;
  std::string error;
  std::vector<std::string> missing;  // zipped package: files not packed
  // UI thread only.
  bool zip = false;
  std::string path;
  std::function<void(bool)> done;
  ~Job() {
    cancel = true;
    if (worker.joinable()) worker.join();
  }
} job;

struct Last {
  bool open = false;
  bool have = false;
  std::string file;
  flp::ExportReport report;
} last;

// K-110: the export options window (File > Export).
struct Ask {
  bool open = false, keep = false;
  std::string path;
  bool zip = false;
  flp::GridlinePlugins mode = flp::GridlinePlugins::SuiteWithFallback;
  std::function<void(bool)> done;
} ask;

flp::GridlinePlugins modeOf(const App& a) {
  flp::GridlinePlugins m = flp::GridlinePlugins::SuiteWithFallback;
  if (!flp::parseGridlinePlugins(a.doc.song().flExportGridline, m)) flp::parseGridlinePlugins(a.settings.getString("export.flGridlinePlugins"), m);
  return m;
}

// A Gridline plugin of the song that a Suite plugin can carry (not imported from FL).
bool hasSuiteCandidates(const Song& s) {
  auto fromFl = [&](ObjectId owner, int slot) { return s.flOrigin.blobs.count(flp::pluginKey(owner, slot)) > 0; };
  for (const auto& c : s.channels)
    if (!c.plugin.empty() && !flp::suiteVst3Type(c.plugin.type).empty() && !fromFl(c.id, -1)) return true;
  for (const auto& t : s.mixer)
    for (size_t k = 0; k < t.slots.size(); ++k)
      if (!t.slots[k].plugin.empty() && !flp::suiteVst3Type(t.slots[k].plugin.type).empty() && !fromFl(t.id, static_cast<int>(k))) return true;
  return false;
}

struct ModeText {
  flp::GridlinePlugins mode;
  const char* label;
  const char* hint;
};
const ModeText kModes[] = {
    {flp::GridlinePlugins::SuiteWithFallback, i18n::N_("Gridline Suite plugins, with muted audio copies"),
     i18n::N_("Plugins open live with the Gridline Suite installed. Each instrument also gets a muted copy playing it as audio, for computers without the Suite.")},
    {flp::GridlinePlugins::Suite, i18n::N_("Gridline Suite plugins only"),
     i18n::N_("Every Gridline plugin without an exact FL Studio counterpart opens live. The other computer needs the Gridline Suite.")},
    {flp::GridlinePlugins::Render, i18n::N_("Rendered to audio"),
     i18n::N_("Instruments play rendered samples and effects become the closest FL Studio effect. Plays anywhere; the Gridline plugins are not editable.")},
};

const char* modeLabel(flp::GridlinePlugins m) {
  for (const auto& t : kModes)
    if (t.mode == m) return i18n::tr(t.label);
  return "";
}

void askWindow(App& a) {
  if (!ask.open) return;
  const std::string title = std::string(i18n::tr("Export FL Studio project")) + "###flpexportask";
  ImGui::OpenPopup(title.c_str());
  ui::PlaceNextDialog(ImVec2(560, 0), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const bool vis = ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  if (!vis) return;
  const auto& P = theme::pal();
  const float w = 520, indent = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
  ui::TextFit(fs::path(ask.path).filename().string().c_str(), w, P.textDim);
  ImGui::Dummy(ImVec2(w, theme::kS1));
  ImGui::TextUnformatted(i18n::tr("Gridline plugins without an exact FL Studio counterpart"));
  ImGui::Dummy(ImVec2(0, theme::kS1));
  for (const auto& t : kModes) {
    ImGui::PushID(static_cast<int>(t.mode));
    if (ImGui::RadioButton(i18n::tr(t.label), ask.mode == t.mode)) ask.mode = t.mode;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - indent);
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr(t.hint));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::PopID();
  }
  ImGui::Checkbox(i18n::tr("Use this choice for this project from now on"), &ask.keep);
  {
    std::string dflt = std::string(i18n::tr("Default for every project: ")) + modeLabel([&] {
                         flp::GridlinePlugins m = flp::GridlinePlugins::SuiteWithFallback;
                         flp::parseGridlinePlugins(a.settings.getString("export.flGridlinePlugins"), m);
                         return m;
                       }()) +
                       i18n::tr(" (Settings, Project)");
    ui::TextFit(dflt.c_str(), w, P.textFaint);
  }
  const float bw = 96;
  ui::DialogFooter(ui::ButtonRowWidth(2, bw));
  bool go = false, cancel = false;
  const bool keys = !ImGui::IsWindowAppearing();  // (the key that closed the file dialog is not ours)
  if (ui::TextButton(i18n::tr("Cancel"), false, bw) || (keys && ::gl::keys::escapePressed())) cancel = true;
  ImGui::SameLine(0, theme::kS2);
  if (ui::PrimaryButton(i18n::tr("Export"), bw) || (keys && ImGui::IsKeyPressed(ImGuiKey_Enter))) go = true;
  if (go || cancel) {
    ask.open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  if (cancel) {
    auto done = std::move(ask.done);
    ask.done = nullptr;
    if (done) done(false);
  } else if (go) {
    const std::string key = flp::gridlinePluginsKey(ask.mode);
    if (ask.keep && a.doc.song().flExportGridline != key)
      a.doc.editSong("FL Studio export choice", [key](Song& s) { s.flExportGridline = key; });
    startWith(a, ask.path, ask.zip, std::move(ask.done), ask.mode);
    ask.done = nullptr;
  }
}

const char* actionLabel(const std::string& q) {
  if (q == "suite") return i18n::tr("Gridline Suite plugin");
  if (q == "rendered") return i18n::tr("Rendered to samples");
  if (q == "mapped") return i18n::tr("Mapped");
  if (q == "verbatim" || q == "exact") return i18n::tr("Exact");
  if (q == "approx") return i18n::tr("Approximated");
  if (q == "defaults") return i18n::tr("FL defaults");
  if (q == "placeholder") return i18n::tr("No FL counterpart");
  if (q == "unused") return i18n::tr("Not used");
  return q.c_str();
}

void finish(App& a) {
  if (!job.finished.exchange(false)) return;
  if (job.worker.joinable()) job.worker.join();
  bool ok;
  std::string err;
  std::vector<std::string> missing;
  {
    std::lock_guard<std::mutex> lk(job.mu);
    ok = job.ok, err = job.error, missing = job.missing;
    last.report = job.report;
  }
  last.have = true;
  last.file = fs::path(job.path).filename().string();
  const bool cancelled = last.report.cancelled || job.cancel.load();
  for (const auto& wv : last.report.warnings) logging::warning("FL export", wv);
  for (const auto& pl : last.report.plugins)
    if (pl.quality != "verbatim" && pl.quality != "exact")
      logging::info("FL export", pl.where + ": " + pl.glType + " -> " + pl.flName + " [" + pl.quality + "] " + pl.note);
  // J-78 cards: errors red with the path as details (a click copies them); a
  // success is one normal card whose details are the full report.
  if (cancelled) a.notify(i18n::tr("FL Studio export cancelled"));
  else if (!ok)
    a.notify((job.zip ? "Could not save the FL Studio zipped loop package: " : "Could not save the FL Studio project: ") + err, true, job.path);
  else {
    a.notify(flp::exportSummary(last.report, last.file), false, job.path + "\n" + flp::reportText(last.report));
    if (!missing.empty()) {
      std::string list;
      for (const auto& m : missing) list += (list.empty() ? "" : "\n") + m;
      a.notify("FL Studio zipped loop package written without " + std::to_string(missing.size()) +
                   " missing file(s); relink them with File > Locate missing samples",
               true, list);
    }
    a.uiSound(uisound::Event::Export);
  }
  auto done = std::move(job.done);
  job.done = nullptr;
  if (done) done(ok && !cancelled);
}

void progressWindow() {
  if (!job.running) return;
  ui::PlaceNextDialog(ImVec2(440, 0), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const std::string title = std::string(i18n::tr("Exporting FL Studio project")) + "###flpexportjob";
  ImGui::OpenPopup(title.c_str());
  if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    const float w = 400, bw = 96;
    ui::TextFit(fs::path(job.path).filename().string().c_str(), w, theme::pal().textDim);
    ImGui::TextUnformatted(i18n::tr("Rendering Gridline instruments to samples"));
    char pr[64];
    std::snprintf(pr, sizeof pr, i18n::tr("%.0f%%"), job.progress.load() * 100.0);
    ImGui::ProgressBar(static_cast<float>(job.progress.load()), ImVec2(w - bw - theme::kS2, 0), pr);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Cancel", false, bw, "Stop the export") || ::gl::keys::escapePressed()) job.cancel = true;
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

void reportWindow() {
  if (!last.open) return;
  ui::PlaceNextDialog(ImVec2(760, 460));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const std::string title = std::string(i18n::tr("FL Studio export report")) + "###flpexportreport";
  const bool vis = ImGui::Begin(title.c_str(), &last.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  const auto& P = theme::pal();
  if (!last.have) {
    ImGui::TextUnformatted(i18n::tr("No FL Studio export yet"));
    ImGui::End();
    return;
  }
  ImGui::TextUnformatted(flp::exportSummary(last.report, last.file).c_str());
  if (!last.report.files.empty()) {
    char b[160];
    std::snprintf(b, sizeof b, i18n::tr("%d sample files rendered next to the project"), static_cast<int>(last.report.files.size()));
    ui::TextFit(b, ImGui::GetContentRegionAvail().x, P.textDim);
  }
  ImGui::Spacing();
  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
  const float listH = ImGui::GetContentRegionAvail().y - ui::DialogFooterHeight(1);
  if (ImGui::BeginTable("##flprep", 4, tf, ImVec2(0, std::max(80.0f, listH)))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(i18n::tr("Where"), ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn(i18n::tr("Gridline plugin"), ImGuiTableColumnFlags_WidthFixed, 130);
    ImGui::TableSetupColumn(i18n::tr("Written as"), ImGuiTableColumnFlags_WidthFixed, 210);
    ImGui::TableSetupColumn(i18n::tr("Details"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& p : last.report.plugins) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ui::TextFit(p.where.c_str(), ImGui::GetContentRegionAvail().x, P.text);
      ImGui::TableNextColumn();
      const PluginInfo* pi = findPlugin(p.glType);
      ui::TextFit(pi ? pi->name.c_str() : p.glType.c_str(), ImGui::GetContentRegionAvail().x, P.text);
      ImGui::TableNextColumn();
      const std::string as = std::string(actionLabel(p.quality)) + (p.flName.empty() ? "" : ": " + p.flName);
      ui::TextFit(as.c_str(), ImGui::GetContentRegionAvail().x, p.quality == "placeholder" ? P.textDim : P.text);
      ImGui::TableNextColumn();
      ui::TextFit(p.note.c_str(), ImGui::GetContentRegionAvail().x, P.textDim);
    }
    for (const auto& wv : last.report.warnings) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ui::TextFit(i18n::tr("Note"), ImGui::GetContentRegionAvail().x, P.textDim);
      ImGui::TableNextColumn();
      ImGui::TableNextColumn();
      ImGui::TableNextColumn();
      ui::TextFit(wv.c_str(), ImGui::GetContentRegionAvail().x, P.textDim);
    }
    ImGui::EndTable();
  }
  ui::DialogFooter(ui::ButtonRowWidth(1, 96));
  if (ui::TextButton("Close", false, 96)) last.open = false;
  ImGui::End();
}

}  // namespace

bool running() { return job.running.load(); }
bool hasReport() { return last.have; }
void openReport() { last.open = true; }

std::string modeKey(const App& a) { return flp::gridlinePluginsKey(modeOf(a)); }

bool start(App& a, const std::string& path, bool zip, std::function<void(bool)> done) { return startWith(a, path, zip, std::move(done), modeOf(a)); }

void askAndStart(App& a, const std::string& path, bool zip, std::function<void(bool)> done) {
  if (!hasSuiteCandidates(a.doc.song())) {
    startWith(a, path, zip, std::move(done), modeOf(a));
    return;
  }
  ask.open = true;
  ask.keep = false;
  ask.path = path;
  ask.zip = zip;
  ask.mode = modeOf(a);
  ask.done = std::move(done);
}

namespace {

bool startWith(App& a, const std::string& path, bool zip, std::function<void(bool)> done, flp::GridlinePlugins mode) {
  if (job.running) {
    a.notify(i18n::tr("An FL Studio export is already running"), true);
    return false;
  }
  if (job.worker.joinable()) job.worker.join();
  {  // J-78: a folder or read-only file is refused before anything is rendered
    std::string why;
    const fs::path dir = fs::path(path).parent_path().empty() ? fs::path(".") : fs::path(path).parent_path();
    if (checkWritableTarget(path, &why) && !platform::writable(dir.string())) why = fileWriteError(path, errno);
    if (!why.empty()) {
      a.notify((zip ? "Could not save the FL Studio zipped loop package: " : "Could not save the FL Studio project: ") + why, true, path);
      return false;
    }
  }
  captureHostedStates(a);
  job.cancel = false, job.finished = false, job.progress = 0;
  job.path = path;
  job.zip = zip;
  job.done = std::move(done);
  job.running = true;
  Song song = a.doc.song();
  auto work = [song = std::move(song), path, zip, mode]() {
    flp::ExportOptions opt;
    opt.gridlinePlugins = mode;  // K-110
    opt.factoryData = flp::defaultFactoryData();
    opt.projectDir = fs::path(path).parent_path().string();
    opt.progress = [](double p) { job.progress = p; };
    opt.cancel = &job.cancel;
    flp::ExportReport rep;
    std::string err;
    std::vector<std::string> missing;
    bool ok = false;
    try {  // J-78: an unexpected failure is reported, never a crash
      if (zip) {
        flp::PackageOptions po;
        po.flp = opt;
        flp::PackageReport pr;
        ok = flp::exportFlpZip(song, path, fs::path(path).stem().string(), po, pr, &err);
        for (const auto& m : pr.missing) pr.flp.warn("missing file, not packed: " + m);
        missing = pr.missing;
        rep = std::move(pr.flp);
      } else {
        ok = flp::exportFlpFile(song, path, rep, opt, &err);
      }
    } catch (const std::exception& e) {
      ok = false;
      err = std::string("internal error (") + e.what() + "). Please report it; try another format meanwhile.";
      crash::reportCurrentException("Exporting an FL project");
    }
    {
      std::lock_guard<std::mutex> lk(job.mu);
      job.report = std::move(rep);
      job.ok = ok;
      job.error = err;
      job.missing = std::move(missing);
    }
    job.finished = true;
    job.running = false;
  };
  job.worker = std::thread(std::move(work));
  return true;
}

}  // namespace

void draw(App& a) {
  finish(a);
  askWindow(a);
  progressWindow();
  reportWindow();
}

void wait() {
  if (job.worker.joinable()) job.worker.join();
}

void shutdown() {
  job.cancel = true;
  if (job.worker.joinable()) job.worker.join();
}

}  // namespace gl::flpexport

// Gridline — J-80: FL Studio export job, progress and report windows (see FlpExportUi.h).
#include "FlpExportUi.h"
#include <unistd.h>
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
#include "App.h"
#include "DialogLayout.h"
#include "HostedUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::flpexport {

namespace fs = std::filesystem;

namespace {

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

const char* actionLabel(const std::string& q) {
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
    if (ui::TextButton("Cancel", false, bw, "Stop the export") || ImGui::IsKeyPressed(ImGuiKey_Escape)) job.cancel = true;
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

bool start(App& a, const std::string& path, bool zip, std::function<void(bool)> done) {
  if (job.running) {
    a.notify(i18n::tr("An FL Studio export is already running"), true);
    return false;
  }
  if (job.worker.joinable()) job.worker.join();
  {  // J-78: a folder or read-only file is refused before anything is rendered
    std::string why;
    const fs::path dir = fs::path(path).parent_path().empty() ? fs::path(".") : fs::path(path).parent_path();
    if (checkWritableTarget(path, &why) && ::access(dir.c_str(), W_OK) != 0) why = fileWriteError(path, errno);
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
  auto work = [song = std::move(song), path, zip]() {
    flp::ExportOptions opt;
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

void draw(App& a) {
  finish(a);
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

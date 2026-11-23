// Gridline - J-219: the Track Receipt window (see TrackReceiptUi.h): the shell (the window, the
// steps, the measuring job, the footer). The pages are in their own files (TrackReceiptUiKit.h).
#include "TrackReceiptUi.h"
#include "../core/ThreadName.h"
#include "LayoutShellUi.h"
#include "Keys.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/ReceiptHashes.h"
#include "../core/ReceiptOrigin.h"
#include "../core/TrackReceipt.h"
#include "../engine/TrackReceiptPass.h"
#include "../host/HostedPlugins.h"
#include "App.h"
#include "CollabUi.h"
#include "ContextMenus.h"
#include "DevSignal.h"
#include "DialogLayout.h"
#include "ProgressUi.h"
#include "Theme.h"
#include "TrackReceiptUiKit.h"
#include "Widgets.h"
#include "WorkerStall.h"

namespace fs = std::filesystem;

namespace gl::receiptui {

namespace {

using theme::pal;
using namespace gl::receipt;

struct Job {
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> progress{0.0};
  std::mutex mu;
  Measurement result;
  std::string error;  // K-133: why the measurement stopped (guarded by mu); shown to the person
  WorkerBeat beat;    // K-133: each progress step
  StallWatch watch;
  std::chrono::steady_clock::time_point started;  // K-319: for the time left (UI thread)
  void join() {
    if (worker.joinable()) worker.join();
  }
};
Job& J() {
  static Job j;
  return j;
}

std::string projectName(const App& a) {
  if (a.projectPath.empty()) return "Untitled";
  return fs::u8path(a.projectPath).stem().u8string();
}

void rebuild(App& a) {
  State& s = S();
  s.pick = build(s.scanned, s.saved, projectName(a));
  if (s.page == 2) s.report = build(s.measured, s.saved, projectName(a));
}

}  // namespace

State& S() {
  static State s;
  return s;
}

void commit(App& a) {
  State& s = S();
  const std::string json = savedToJson(s.saved);
  if (json != a.doc.song().trackReceipt) a.doc.editSong("Track Receipt", [&](Song& x) { x.trackReceipt = json; });
  rebuild(a);
}

namespace {

// ---- measuring

void startMeasure(App& a) {
  Job& j = J();
  if (j.running.load()) return;
  j.join();
  State& s = S();
  s.page = 1;
  s.demoProgress = -1;
  j.cancel = false;
  j.finished = false;
  j.progress = 0;
  j.started = std::chrono::steady_clock::now();
  workerstall::start(j.watch, j.beat);
  j.running = true;
  Song song = a.doc.song();
  const int rate = a.audioSampleRate > 0 ? a.audioSampleRate : 44100;
  j.worker = std::thread([&j, song, rate]() { gl::threads::setName("gl-receipt");
    host::UiThreadRenderScope scope;  // hosted plugins: as the render dialog does
    PassOptions o;
    o.sampleRate = rate;
    o.cancel = &j.cancel;
    o.hashes = &HashCache::shared();  // K-321: the project's trail is kept by file hash
    o.progress = [&j](double f) {
      j.beat.beat();
      j.progress = f;
    };
    Measurement m;
    std::string error;
    try {  // an exception must not leave this thread
      if (std::getenv("GRIDLINE_DEV_RECEIPT_FAIL")) throw std::runtime_error("a test failure");  // K-133 tests
      m = measure(song, o);
      HashCache::shared().save();
    } catch (const std::exception& e) {
      error = e.what();
    } catch (...) {
      error = "an internal error";
    }
    if (!error.empty()) logging::error("receipt", "the measurement stopped: " + error);
    {
      std::lock_guard<std::mutex> lk(j.mu);
      j.result = std::move(m);
      j.error = std::move(error);
    }
    j.running = false;
    j.finished = true;
  });
}

void finish(App& a) {
  Job& j = J();
  if (!j.finished.exchange(false)) return;
  j.join();
  State& s = S();
  Measurement m;
  std::string error;
  {
    std::lock_guard<std::mutex> lk(j.mu);
    m = std::move(j.result);
    error = std::move(j.error);
    j.error.clear();
  }
  if (!error.empty()) {  // K-133: told, not only logged; the files page is where it started
    s.page = 0;
    a.notify("The Track Receipt could not be calculated. Nothing was changed: try Calculate again.", true, error);
    devsig::reach("receipt-failed");
    return;
  }
  if (m.cancelled) {
    s.page = 0;
    devsig::reach("receipt-cancelled");
    return;
  }
  s.measured = std::move(m);
  s.page = 2;
  // K-321: what the scan learnt about the files (Tracklib names, origins) is kept in the project
  // by file hash, so the trail survives a lost sidecar or a rename. One edit, only when new.
  if (Saved next = s.saved; rememberOrigins(next, s.measured)) {
    s.saved = next;
    const std::string json = savedToJson(s.saved);
    a.doc.editSong("Track Receipt: remember sample origins", [&](Song& x) { x.trackReceipt = json; });
  }
  rebuild(a);
  devsig::reach("receipt-done");
}

// ---- outputs

bool writeText(const std::string& path, const std::string& text) {
  std::ofstream f(fs::u8path(path), std::ios::binary);
  f << text;
  return static_cast<bool>(f);
}

void openFile(const std::string& path) {
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
}

// Writes `name` into the project folder, or asks where while the project is unsaved.
void saveOutput(App& a, const std::string& name, const std::string& ext, std::function<std::string()> make, bool openAfter) {
  auto write = [&a, make, openAfter](const std::string& path) {
    if (!writeText(path, make())) {
      a.notify("Could not write " + fs::u8path(path).filename().u8string(), true);
      return;
    }
    S().lastFile = path;
    a.notify("Saved " + fs::u8path(path).filename().u8string(), false, fs::u8path(path).parent_path().u8string());
    if (openAfter && !std::getenv("GRIDLINE_TRACK_RECEIPT_DEMO")) openFile(path);
  };
  if (!a.projectPath.empty()) {
    write((fs::u8path(a.projectPath).parent_path() / fs::u8path(name)).u8string());
    return;
  }
  menus::fileDialog(a, i18n::tr("Save Track Receipt"), true, {ext}, name, write);
}

// ---- pages

void measuringPage(App& a) {
  (void)a;
  const auto& P = pal();
  const progui::Fake* fk = progui::fake("trackreceipt");  // (tests: a stand-in measurement)
  const double f = fk ? fk->frac : S().demoProgress >= 0 ? S().demoProgress : J().progress.load();
  ImGui::Dummy(ImVec2(kW, theme::kS6));
  progui::Line(i18n::tr("Playing the project through to measure what each sample voice reads..."), kW, P.text);
  ImGui::Spacing();
  ImGui::ProgressBar(static_cast<float>(f), ImVec2(kW, 0), "");
  // K-319: the time left, from the pace so far (once there is a pace to go by).
  const double elapsed = fk ? 600.0 * f : std::chrono::duration<double>(std::chrono::steady_clock::now() - J().started).count();
  std::string left;
  if ((fk || S().demoProgress < 0) && f >= 0.03 && f < 1.0 && elapsed >= 0.5) {
    const double rest = elapsed * (1.0 - f) / f;
    char b[96];
    if (rest < 60) {
      std::snprintf(b, sizeof b, i18n::tr("about %d s left"), std::max(1, static_cast<int>(std::ceil(rest))));
    } else {
      std::snprintf(b, sizeof b, i18n::tr("about %d min left"), static_cast<int>(std::ceil(rest / 60.0)));
    }
    left = std::string("  ·  ") + b;
  }
  // 0.7.8: the percent in a field as wide as "100%", the time left after it: nothing jumps.
  progui::Percent(f, P.textDim);
  ImGui::SameLine(0, 0);
  progui::Line(left.c_str(), kW - progui::PercentWidth(), P.textDim);
  ImGui::Dummy(ImVec2(kW, theme::kS6));
}

}  // namespace

void open(App& a) {
  State& s = S();
  s.open = true;
  s.focus = true;
  if (!J().running.load()) s.page = 0;
  s.saved = savedFromJson(a.doc.song().trackReceipt);
  s.refs = fileRefs(a.doc.song());
  s.scanned = scan(a.doc.song(), false);
  rebuild(a);
}

void close() { S().open = false; }

void draw(App& a) {
  finish(a);
  if (J().running.load()) workerstall::watch(a, J().watch, J().beat, "Track Receipt", "");
  State& s = S();
  // Screenshots: GRIDLINE_TRACK_RECEIPT_DEMO=picker|progress|results opens a page on the project given
  // (K-133: calculate also starts the measurement, as the Calculate button does).
  static int frames = 0;
  if (const char* env = std::getenv("GRIDLINE_TRACK_RECEIPT_DEMO"); env && *env && ++frames == 4) {
    open(a);
    const std::string mode = env;
    if (mode == "calculate") {
      startMeasure(a);
    } else if (mode == "progress") {
      s.page = 1;
      s.demoProgress = 0.42;
    } else if (mode == "results") {
      PassOptions o;
      o.sampleRate = 44100;
      s.measured = measure(a.doc.song(), o);
      s.page = 2;
      rebuild(a);
    }
  }
  static bool faked = false;  // (tests: a stand-in measurement; the window closes after it)
  if (progui::fake("trackreceipt")) s.open = true, s.page = 1;
  else if (faked) s.open = false, s.page = 0;
  faked = progui::fake("trackreceipt") != nullptr;
  if (!s.open) return;
  Job& j = J();
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ui::PlaceNextDialog(ImVec2(kW + 16 + 2 * ImGui::GetStyle().WindowPadding.x, 0));
  if (s.focus) ImGui::SetNextWindowFocus(), s.focus = false;
  bool open = true;
  const std::string title = std::string(i18n::tr("Track Receipt")) + "###trackreceipt";
  const bool vis = ImGui::Begin(title.c_str(), &open,  // K-112: docked in its region in a layout
                                layoutui::dockFlags(title.c_str(), ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking));
  ui::TrackWindowInViewport();
  progui::note("trackreceipt");
  if (vis) {
    {  // steps
      const auto& P = pal();
      const char* steps[3] = {i18n::tr("1  Files"), i18n::tr("2  Calculate"), i18n::tr("3  Results")};
      for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, theme::kS4);
        ImGui::TextColored(theme::vec(i == s.page ? P.accent : P.textFaint), "%s", steps[i]);
      }
      ImGui::Separator();
    }
    if (s.page == 0) filesPage(a);
    else if (s.page == 1) measuringPage(a);
    else resultsPage(a);
    const float bw = 150;
    if (s.page == 0) {
      ui::DialogFooter(ui::ButtonRowWidth(2, bw));
      if (ui::TextButton(i18n::tr("Close"), false, bw)) open = false;
      ImGui::SameLine(0, theme::kS2);
      if (ui::PrimaryButton(i18n::tr("Calculate"), bw, i18n::tr("Plays the project offline and measures the parts used"))) startMeasure(a);
    } else if (s.page == 1) {
      ui::DialogFooter(ui::ButtonRowWidth(1, bw));
      if (ui::TextButton(i18n::tr("Cancel"), false, bw) || ::gl::keys::escapePressed()) {
        j.cancel = true;
        if (s.demoProgress >= 0) s.page = 0, s.demoProgress = -1;
      }
    } else {
      ui::DialogFooter(ui::ButtonRowWidth(3, bw));
      if (ui::TextButton(i18n::tr("Back"), false, bw)) s.page = 0;
      ImGui::SameLine(0, theme::kS2);
      if (ui::TextButton(i18n::tr("Save Track Receipt"), false, bw, i18n::tr("A spreadsheet for your license: Licenses, Usage, Release, Splits"))) {
        const Report r = s.report;
        saveOutput(a, receiptFileName(projectName(a)), ".xml", [r]() { return spreadsheetXml(r); }, false);
      }
      ImGui::SameLine(0, theme::kS2);
      if (ui::PrimaryButton(i18n::tr("View results"), bw, i18n::tr("Saves the results as a PDF and opens it"))) {
        const Report r = s.report;
        saveOutput(a, resultsFileName(projectName(a)), ".pdf", [r]() { return resultsPdf(r); }, true);
      }
    }
  }
  ImGui::End();
  if (!open) {
    if (j.running.load()) j.cancel = true;
    s.open = false;
  }
}

void shutdown() {
  J().cancel = true;
  host::serveUiThreadTasksWhile(J().running);
  J().join();
}

}  // namespace gl::receiptui

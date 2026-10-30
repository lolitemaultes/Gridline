// Gridline - J-191: the Split stems window (see StemsUi.h).
#include "StemsUi.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "../core/I18n.h"
#include "../engine/Sample.h"
#include "../engine/stems/StemModels.h"
#include "../engine/stems/StemPlace.h"
#include "../host/net/Http.h"
#include "App.h"
#include "ContextMenus.h"
#include "DevSignal.h"
#include "Theme.h"
#include "Widgets.h"

namespace fs = std::filesystem;

namespace gl {

using theme::pal;

std::string stemsDir(const App& a) {
  if (!a.projectPath.empty()) return (fs::u8path(a.projectPath).parent_path() / "Stems").u8string();
  return (fs::u8path(a.configDir()) / "Stems").u8string();
}

namespace stemsui {

namespace {

namespace st = gl::stems;

using Clock = std::chrono::steady_clock;

struct Job {
  enum class Kind { None, Download, Split } kind = Kind::None;
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> progress{0.0};
  Clock::time_point started;
  std::mutex mu;
  bool ok = false;
  std::string err;
  std::vector<std::pair<std::string, std::string>> written;
  void join() {
    if (worker.joinable()) worker.join();
  }
  ~Job() {
    cancel = true;
    join();
  }
};

struct State {
  bool open = false, focus = false;
  ObjectId clip = kNoId;     // clip mode
  std::string path;          // the audio file to split
  std::string name;          // shown / used for file names
  double seconds = 0;
  int model = 0;             // index into the catalogue
  std::map<std::string, bool> keep;
  int place = 0;             // clip mode: 0 new tracks under the clip, 1 files only
  bool muteOriginal = true;
  std::string lastError;
  // the split that is running: what it was started for
  ObjectId jobClip = kNoId;
  int jobPlace = 0;
  bool jobMute = true;
  std::string jobName, jobDir;
};

State& S() {
  static State s;
  return s;
}
Job& J() {
  static Job j;
  return j;
}

bool trace() {
  static const bool on = std::getenv("GRIDLINE_STEMS_TRACE") != nullptr;
  return on;
}

const std::vector<st::ModelInfo>& models() { return st::catalogue(); }

const st::ModelInfo& currentModel() {
  State& s = S();
  s.model = std::clamp(s.model, 0, int(models().size()) - 1);
  return models()[size_t(s.model)];
}

std::string mb(int64_t bytes) {
  char b[32];
  std::snprintf(b, sizeof b, "%.0f MB", double(bytes) / 1e6);
  return b;
}

std::string timeLeft(double secs) {
  char b[64];
  if (secs < 60) std::snprintf(b, sizeof b, i18n::tr("about %d s left"), std::max(1, int(std::lround(secs))));
  else std::snprintf(b, sizeof b, i18n::tr("about %d min %02d s left"), int(secs) / 60, int(secs) % 60);
  return b;
}

const char* stemLabel(const std::string& id) {
  if (id == "vocals") return i18n::tr("Vocals");
  if (id == "drums") return i18n::tr("Drums");
  if (id == "bass") return i18n::tr("Bass");
  if (id == "other") return i18n::tr("Other");
  if (id == "guitar") return i18n::tr("Guitar");
  if (id == "piano") return i18n::tr("Piano");
  return id.c_str();
}

// The catalogue's English text (engine/stems/StemModels.cpp), translated where it is shown.
[[maybe_unused]] const char* const kCatalogueText[] = {
    i18n::N_("Standard"),
    i18n::N_("Vocals, drums, bass and other. Good quality, the fastest."),
    i18n::N_("High quality"),
    i18n::N_("Vocals, drums, bass and other with one tuned model per stem. About four times slower."),
    i18n::N_("Six stems"),
    i18n::N_("Adds guitar and piano to vocals, drums, bass and other. Piano separation is rough."),
    i18n::N_("The stem models are Hybrid Transformer Demucs by Meta AI Research. Their code is MIT-licensed; the trained "
             "model files are provided by Meta for research use and are not part of Gridline. They are downloaded from "
             "Meta's own server (dl.fbaipublicfiles.com) only when you ask, checked against a checksum, and kept on this "
             "computer. Check that your use of the results suits your purpose."),
};

void startJob(Job::Kind kind, std::function<bool(Job&)> body) {
  Job& j = J();
  j.join();
  j.kind = kind;
  j.cancel = false;
  j.finished = false;
  j.progress = 0.0;
  j.ok = false;
  j.err.clear();
  j.written.clear();
  j.started = Clock::now();
  j.running = true;
  j.worker = std::thread([&j, body] {
    const bool ok = body(j);
    {
      std::lock_guard<std::mutex> lk(j.mu);
      j.ok = ok;
    }
    j.running = false;
    j.finished = true;
  });
}

void startDownload(const st::ModelInfo& m) {
  const st::ModelInfo model = m;
  startJob(Job::Kind::Download, [model](Job& j) {
    std::string err;
    const bool ok = st::downloadModel(model, http::download, [&j](int64_t done, int64_t total) {
      if (total > 0) j.progress = double(done) / double(total);
    }, &j.cancel, &err);
    std::lock_guard<std::mutex> lk(j.mu);
    j.err = err;
    return ok;
  });
}

void startSplit(App& a) {
  State& s = S();
  const st::ModelInfo& m = currentModel();
  st::ExtractRequest req;
  req.input = s.path;
  req.modelId = m.id;
  for (const auto& src : m.sources)
    if (s.keep[src]) req.keep.push_back(src);
  req.outDir = stemsDir(a);
  req.baseName = s.name;
  s.jobClip = s.clip;
  s.jobPlace = s.clip != kNoId ? s.place : 1;
  s.jobMute = s.muteOriginal;
  s.jobName = s.name;
  s.jobDir = req.outDir;
  s.lastError.clear();
  startJob(Job::Kind::Split, [req](Job& j) {
    std::string err;
    std::vector<std::pair<std::string, std::string>> written;
    const bool ok = st::extractStems(req, [&j](double f) { j.progress = f; }, &j.cancel, &written, &err);
    std::lock_guard<std::mutex> lk(j.mu);
    j.err = err;
    j.written = written;
    return ok;
  });
}

// UI thread: a job that has ended.
void finish(App& a) {
  Job& j = J();
  if (!j.finished.exchange(false)) return;
  j.join();
  State& s = S();
  std::string err;
  bool ok;
  std::vector<std::pair<std::string, std::string>> written;
  {
    std::lock_guard<std::mutex> lk(j.mu);
    ok = j.ok;
    err = j.err;
    written = j.written;
  }
  const bool cancelled = j.cancel.load();
  if (j.kind == Job::Kind::Download) {
    if (ok) a.notify("Stem model downloaded");
    else if (!cancelled) s.lastError = err, a.notify("The model download failed", true, err);
    devsig::reach(ok ? "stems-model-ready" : "stems-model-failed");
    return;
  }
  if (!ok) {
    if (!cancelled) {
      s.lastError = err;
      a.notify("Splitting into stems failed", true, err);
    }
    devsig::reach(cancelled ? "stems-cancelled" : "stems-failed");
    return;
  }
  std::vector<st::StemFile> files;
  for (const auto& w : written) files.push_back({w.first, w.second});
  bool placed = false;
  if (s.jobPlace == 0 && s.jobClip != kNoId && a.doc.song().clip(s.jobClip)) {
    std::vector<ObjectId> made;
    a.doc.editSong("split stems", [&](Song& x) { made = st::placeStems(x, s.jobClip, files, s.jobName, s.jobMute); });
    if (!made.empty()) {
      a.playlist.selected.clear();
      for (ObjectId id : made) a.playlist.selected.insert(id);
      placed = true;
    }
  }
  char msg[160];
  if (placed) std::snprintf(msg, sizeof msg, i18n::tr("Split into %d stems on new tracks"), int(files.size()));
  else std::snprintf(msg, sizeof msg, i18n::tr("%d stems saved in the Stems folder"), int(files.size()));
  a.notify(msg, false, s.jobDir);
  s.open = false;
  devsig::reach("stems-done");
}

void openWith(const std::string& path, const std::string& name, ObjectId clip) {
  State& s = S();
  s.open = true;
  s.focus = true;
  s.clip = clip;
  s.path = path;
  s.name = name.empty() ? fs::u8path(path).stem().u8string() : name;
  s.seconds = 0;
  if (SamplePtr smp = sampleCache().get(path)) s.seconds = smp->seconds();
  s.lastError.clear();
  // a model the user already has comes first
  if (!st::modelInstalled(currentModel()))
    for (size_t i = 0; i < models().size(); ++i)
      if (st::modelInstalled(models()[i]) && models()[i].id != "test") {
        s.model = int(i);
        break;
      }
  if (const char* e = std::getenv("GRIDLINE_STEMS_TEST_MODEL"); e && *e)
    for (size_t i = 0; i < models().size(); ++i)
      if (models()[i].id == "test") s.model = int(i);
  for (const auto& m : models())
    for (const auto& src : m.sources)
      if (!s.keep.count(src)) s.keep[src] = true;
}

void modelSection(App& a, float w) {
  State& s = S();
  const auto& P = pal();
  const bool busy = J().running.load();
  ui::BeginSection(i18n::tr("MODEL"), w);
  for (size_t i = 0; i < models().size(); ++i) {
    const st::ModelInfo& m = models()[i];
    ImGui::PushID(int(i));
    ImGui::BeginDisabled(busy);
    if (ImGui::RadioButton(i18n::tr(m.title.c_str()), s.model == int(i))) s.model = int(i);
    ImGui::EndDisabled();
    const bool have = st::modelInstalled(m);
    ui::Tooltip(have ? i18n::tr("Right-click to remove the downloaded model") : i18n::tr("Downloaded the first time it is used"));
    if (have && !busy && m.id != "test" && ImGui::BeginPopupContextItem("##modelmenu")) {
      if (ImGui::MenuItem(i18n::tr("Remove the downloaded model"))) {
        std::string err;
        if (st::removeModel(m, &err)) a.notify("Stem model removed");
        else a.notify("The model could not be removed", true, err);
      }
      ImGui::EndPopup();
    }
    std::string status = have ? i18n::tr("downloaded") : mb(st::modelDownloadBytes(m)) + " " + i18n::tr("to download");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui::TextSizeExact(status.c_str()).x);  // the panel's inner edge
    ImGui::TextColored(theme::vec(have ? P.textDim : P.textFaint), "%s", status.c_str());
    ImGui::Indent(ImGui::GetFrameHeight() + theme::kS1);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr(m.summary.c_str()));
    ImGui::PopTextWrapPos();
    ImGui::Unindent(ImGui::GetFrameHeight() + theme::kS1);
    ImGui::PopID();
  }
  ui::EndSection();
  (void)a;
}

void downloadSection(App& a, float w) {
  const st::ModelInfo& m = currentModel();
  const auto& P = pal();
  Job& j = J();
  ui::BeginSection(i18n::tr("DOWNLOAD"), w);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr(st::modelLicenceNote()));
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (j.running && j.kind == Job::Kind::Download) {
    ImGui::ProgressBar(float(j.progress.load()), ImVec2(w - 96 - 3 * theme::kS2, 0));
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton(i18n::tr("Cancel"), false, 96)) j.cancel = true;
  } else {
    const std::string label = std::string(i18n::tr("Download")) + " (" + mb(st::modelDownloadBytes(m)) + ")";
    ImGui::BeginDisabled(j.running.load());
    if (ui::TextButton(label.c_str(), true, 0, i18n::tr("From Meta's server; nothing else is sent"))) startDownload(m);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton(i18n::tr("Import model file..."), false, 0, i18n::tr("A model file you already have (offline computers)")))
      menus::fileDialog(a, i18n::tr("Import a stem model file"), false, {".th"}, "", [&a](const std::string& p) {
        std::string id, err;
        if (st::importModelFile(p, &id, &err)) a.notify("Stem model file installed");
        else a.notify("Not a supported model file", true, err);
      });
    ImGui::EndDisabled();
  }
  ui::EndSection();
}

}  // namespace

void openForClip(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  const Channel* ch = c ? s.channel(c->source) : nullptr;
  if (!c || c->kind != ClipKind::Audio || !ch || ch->plugin.data.empty()) {
    a.notify("This clip has no audio file to split", true);
    return;
  }
  // named after the clip's channel (what the clip shows), else the file
  openWith(ch->plugin.data, !ch->name.empty() ? ch->name : fs::u8path(ch->plugin.data).stem().u8string(), clip);
}

void openForFile(App& a, const std::string& path, const std::string& name) {
  (void)a;
  openWith(path, name, kNoId);
}

void clipMenuItem(App& a, ObjectId clip) {
  const bool busy = J().running.load() && J().kind == Job::Kind::Split;
  if (ImGui::MenuItem(i18n::tr("Split stems..."), nullptr, false, !busy)) openForClip(a, clip);
  if (trace()) {
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    std::printf("stems: menu %d %d\n", int((mn.x + mx.x) * 0.5f), int((mn.y + mx.y) * 0.5f));
  }
  ui::Tooltip(i18n::tr("Separate vocals, drums, bass and other onto new tracks under the clip"));
}

void draw(App& a) {
  finish(a);
  State& s = S();
  Job& j = J();
  if (!s.open) return;
  const auto& P = pal();
  const float w = 470.0f;
  ui::PlaceNextDialog(ImVec2(w + 2 * ImGui::GetStyle().WindowPadding.x, 0));
  if (s.focus) ImGui::SetNextWindowFocus(), s.focus = false;
  bool open = true;
  const std::string title = std::string(i18n::tr("Split stems")) + "###stemswin";
  const bool vis = ImGui::Begin(title.c_str(), &open,
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
  ui::TrackWindowInViewport();
  const bool splitting = j.running.load() && j.kind == Job::Kind::Split;
  if (vis) {
    {  // what is split
      char line[512];
      if (s.seconds > 0)
        std::snprintf(line, sizeof line, "%s \xC2\xB7 %d:%02d", s.name.c_str(), int(s.seconds) / 60, int(s.seconds) % 60);
      else
        std::snprintf(line, sizeof line, "%s", s.name.c_str());
      ImGui::PushFont(a.fontMono, theme::kFontLabel);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(line, w));
      ImGui::PopFont();
    }
    ui::BeginDialogBody("##stemsbody");
    modelSection(a, w);
    const st::ModelInfo& m = currentModel();
    const bool have = st::modelInstalled(m);
    if (!have) downloadSection(a, w);
    ui::BeginSection(i18n::tr("STEMS"), w);
    {
      std::vector<std::string> order = m.sources;
      std::stable_sort(order.begin(), order.end(), [](const std::string& x, const std::string& y) { return st::stemOrder(x) < st::stemOrder(y); });
      ImGui::BeginDisabled(splitting);
      const float colW = (w - theme::kS2) / 3.0f;
      for (size_t i = 0; i < order.size(); ++i) {
        if (i % 3) ImGui::SameLine(theme::kS2 + colW * float(i % 3));
        bool on = s.keep[order[i]];
        if (ImGui::Checkbox(stemLabel(order[i]), &on)) s.keep[order[i]] = on;
      }
      ImGui::EndDisabled();
    }
    ui::EndSection();
    ui::BeginSection(i18n::tr("RESULT"), w);
    if (s.clip != kNoId) {
      ImGui::BeginDisabled(splitting);
      ImGui::RadioButton(i18n::tr("New tracks under the clip, lined up with it"), &s.place, 0);
      ImGui::RadioButton(i18n::tr("Files only, in the Stems folder"), &s.place, 1);
      ImGui::BeginDisabled(s.place != 0);
      ImGui::Checkbox(i18n::tr("Mute the original clip"), &s.muteOriginal);
      ImGui::EndDisabled();
      ImGui::EndDisabled();
    } else {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("The stems are saved as WAV files in the Stems folder (Browser, Stems)."));
      ImGui::PopTextWrapPos();
    }
    ui::EndSection();
    if (splitting) {
      const double f = j.progress.load();
      const double el = std::chrono::duration<double>(Clock::now() - j.started).count();
      std::string status = std::to_string(int(f * 100)) + "%";
      if (f > 0.02) status += "  \xC2\xB7  " + timeLeft(el / f * (1.0 - f));
      else status += "  \xC2\xB7  " + std::string(i18n::tr("starting..."));
      ImGui::TextColored(theme::vec(P.textDim), "%s", status.c_str());
      ImGui::ProgressBar(float(f), ImVec2(w, 0), "");
    } else if (!s.lastError.empty()) {
      ImGui::PushTextWrapPos(w - theme::kS2);
      ImGui::TextColored(theme::vec(P.danger), "%s", s.lastError.c_str());
      ImGui::PopTextWrapPos();
    }
    ui::EndDialogBody();
    int nKeep = 0;
    for (const auto& src : m.sources) nKeep += s.keep[src] ? 1 : 0;
    const float bw = 96.0f;
    ui::DialogFooter(ui::ButtonRowWidth(2, bw));
    if (splitting) {
      if (ui::TextButton(i18n::tr("Cancel"), false, bw) || ImGui::IsKeyPressed(ImGuiKey_Escape)) j.cancel = true;
      ImGui::SameLine(0, theme::kS2);
      ImGui::BeginDisabled(true);
      ui::TextButton(i18n::tr("Split"), true, bw);
      ImGui::EndDisabled();
    } else {
      if (ui::TextButton(i18n::tr("Close"), false, bw)) open = false;
      ImGui::SameLine(0, theme::kS2);
      const bool can = have && nKeep > 0 && !j.running.load();
      ImGui::BeginDisabled(!can);
      const bool go = ui::TextButton(i18n::tr("Split"), true, bw, i18n::tr("Runs in the background on this computer")) ||
                      (can && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter));
      ImGui::EndDisabled();
      if (trace()) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        std::printf("stems: split %d %d\nstems: model %s ready %d keep %d\n", int((mn.x + mx.x) * 0.5f), int((mn.y + mx.y) * 0.5f),
                    currentModel().id.c_str(), have ? 1 : 0, nKeep);
      }
      if (go && can) startSplit(a);
      if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
    }
  }
  ImGui::End();
  if (!open) {
    if (splitting) j.cancel = true;
    s.open = false;
  }
}

void shutdown() {
  Job& j = J();
  j.cancel = true;
  j.join();
}

}  // namespace stemsui
}  // namespace gl

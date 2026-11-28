// Gridline - J-191: the Split stems window (see StemsUi.h).
#include "StemsUi.h"
#include "../core/ThreadJoin.h"
#include "../core/ThreadName.h"
#include "Keys.h"
#include "../core/Crash.h"
#include <algorithm>
#include <cfloat>
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
#include "ProgressUi.h"
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
  void join() { gl::threads::safeJoin(worker); }
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
  // K-447: what is split. Clip mode: song ticks [t0, t1) of the clip (t1 <= t0: all of it);
  // file mode: seconds [fileStart, fileEnd) of the file (fileEnd <= fileStart: all of it).
  Tick t0 = 0, t1 = 0;
  double fileStart = 0, fileEnd = 0;
  bool partial = false;          // less than the whole file
  bool wholeClip = true;         // clip mode: the whole clip (else a part of it)
  double partFrom = 0, total = 0;  // where the part starts in the file and the file's length (seconds)
  int model = 0;             // index into the catalogue
  std::map<std::string, bool> keep;
  int place = 0;             // clip mode: 0 new tracks under the clip, 1 files only
  bool muteOriginal = true;
  std::string lastError;
  // the split that is running: what it was started for
  ObjectId jobClip = kNoId;
  st::StemPart jobPart;
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

// "1:02.5" (minutes, seconds and tenths)
void clockText(double secs, char* out, size_t n) {
  const long tenths = std::lround(std::max(0.0, secs) * 10.0);
  std::snprintf(out, n, "%ld:%02ld.%ld", tenths / 600, (tenths / 10) % 60, tenths % 10);
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
  j.worker = std::thread([&j, body] { gl::threads::setName("gl-stems");
    bool ok = false;
    try {
      ok = body(j);
    } catch (...) {  // K-39: reported; the job ends as failed
      crash::reportCurrentException("Splitting stems");
    }
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
  if (s.clip != kNoId) {  // K-447: the clip's part as it is now, from the audio the clip plays
    if (!st::clipStemPart(a.doc.song(), s.clip, s.t0, s.t1, s.jobPart)) {
      s.lastError = i18n::tr("This part of the clip has no audio to split");
      return;
    }
    req.sound = s.jobPart.sound;
    req.start = s.jobPart.startSeconds();
    req.end = s.jobPart.endSeconds();
  } else {
    req.start = s.fileStart;
    req.end = s.fileEnd;
  }
  s.jobClip = s.clip;
  s.jobPlace = s.clip != kNoId ? s.place : 1;
  s.jobMute = s.muteOriginal;
  s.jobName = s.name;
  s.jobDir = req.outDir;
  s.lastError.clear();
  s.jobPart.sound.reset();  // (the worker's request holds the audio; placing the stems needs none)
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
    a.doc.editSong("split stems", [&](Song& x) { made = st::placeStems(x, s.jobPart, files, s.jobName, s.jobMute); });
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
    if (ui::PrimaryButton(label.c_str(), 0, i18n::tr("From Meta's server; nothing else is sent"))) startDownload(m);
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

void openForClip(App& a, ObjectId clip, Tick t0, Tick t1) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  const Channel* ch = c ? s.channel(c->source) : nullptr;
  if (!c || c->kind != ClipKind::Audio || !ch || ch->plugin.data.empty()) {
    a.notify("This clip has no audio file to split", true);
    return;
  }
  // K-447: only the part the clip plays (inside [t0, t1) when given)
  st::StemPart part;
  if (!st::clipStemPart(s, clip, t0, t1, part)) {
    a.notify("This part of the clip has no audio to split", true);
    return;
  }
  // named after the clip's channel (what the clip shows), else the file
  openWith(ch->plugin.data, !ch->name.empty() ? ch->name : fs::u8path(ch->plugin.data).stem().u8string(), clip);
  State& st = S();
  st.t0 = t1 > t0 ? t0 : 0;
  st.t1 = t1 > t0 ? t1 : 0;
  st.fileStart = st.fileEnd = 0;
  st.seconds = part.seconds();
  st.total = double(part.frames) / part.rate;
  st.partFrom = double(part.heardFrom) / part.rate;
  st.partial = part.from > 0 || part.to < part.frames;  // (handles aside: less than the file)
  st.wholeClip = part.wholeClip;
  if (trace())
    std::printf("stems: part frames %lld %lld of %lld whole %d\n", static_cast<long long>(part.from), static_cast<long long>(part.to),
                static_cast<long long>(part.frames), part.wholeClip ? 1 : 0);
}

void openForFile(App& a, const std::string& path, const std::string& name, double start, double end) {
  (void)a;
  openWith(path, name, kNoId);
  State& s = S();
  s.t0 = s.t1 = 0;
  s.wholeClip = true;
  s.total = 0;
  if (SamplePtr smp = sampleCache().get(path)) s.total = smp->seconds();
  s.fileStart = end > start ? std::max(0.0, start) : 0.0;
  s.fileEnd = end > start ? std::min(end, s.total > 0 ? s.total : end) : 0.0;
  s.partial = s.fileEnd > s.fileStart && (s.fileStart > 0 || s.fileEnd < s.total);
  if (!s.partial) s.fileStart = s.fileEnd = 0;
  s.partFrom = s.fileStart;
  s.seconds = s.partial ? s.fileEnd - s.fileStart : s.total;
}

void clipMenuRange(const App& a, ObjectId clip, Tick& t0, Tick& t1) {
  t0 = t1 = 0;
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c) return;
  const Tick cs = c->start, ce = c->start + s.clipLength(*c);
  const auto& pl = a.playlist;
  Tick ra = 0, rb = 0;
  if (pl.selEnd > pl.selStart && (pl.selTracks.empty() || pl.selTracks.count(c->track))) {
    ra = pl.selStart, rb = pl.selEnd;
  } else if (pl.mqB > pl.mqA && c->track >= std::min(pl.mqT0, pl.mqT1) && c->track <= std::max(pl.mqT0, pl.mqT1)) {
    ra = pl.mqA, rb = pl.mqB;
  }
  // a selection that covers the clip only in part takes that part; one that covers all of it or none of it, the clip
  if (rb > ra && ra < ce && rb > cs && (ra > cs || rb < ce)) t0 = std::max(ra, cs), t1 = std::min(rb, ce);
}

void clipMenuItem(App& a, ObjectId clip) {
  const bool busy = J().running.load() && J().kind == Job::Kind::Split;
  Tick t0 = 0, t1 = 0;
  clipMenuRange(a, clip, t0, t1);
  if (ImGui::MenuItem(i18n::tr("Split stems..."), nullptr, false, !busy)) openForClip(a, clip, t0, t1);
  if (trace()) {
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    std::printf("stems: menu %d %d\n", int((mn.x + mx.x) * 0.5f), int((mn.y + mx.y) * 0.5f));
  }
  ui::Tooltip(t1 > t0 ? i18n::tr("Separate the part inside the time selection into vocals, drums, bass and other on new tracks under the clip")
                      : i18n::tr("Separate vocals, drums, bass and other onto new tracks under the clip"));
}

void draw(App& a) {
  finish(a);
  State& s = S();
  Job& j = J();
  const progui::Fake* fk = progui::fake("stems");  // (tests: a stand-in split; the window closes after it)
  static bool faked = false;
  if (fk && !s.open) s.name = fk->text, s.seconds = 245, s.partial = false, s.wholeClip = true;  // (a window already open keeps what it splits)
  if (fk) s.open = true;
  else if (faked) s.open = false;
  faked = fk != nullptr;
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
  progui::note("stems");
  const bool splitting = (j.running.load() && j.kind == Job::Kind::Split) || fk;
  if (vis) {
    {  // what is split: the name, then its length or the part (no separator before an empty name)
      char rest[256] = "";
      if (s.partial && s.seconds > 0) {  // K-447: the part, and where it lies in the file
        char from[32], to[32], len[32];
        clockText(s.partFrom, from, sizeof from);
        clockText(s.partFrom + s.seconds, to, sizeof to);
        clockText(s.seconds, len, sizeof len);
        std::snprintf(rest, sizeof rest, i18n::tr("%s, the part from %s to %s"), len, from, to);
      } else if (s.seconds > 0) {
        std::snprintf(rest, sizeof rest, "%d:%02d", int(s.seconds) / 60, int(s.seconds) % 60);
      }
      const std::string line = s.name.empty() ? std::string(rest) : !*rest ? s.name : s.name + " \xC2\xB7 " + rest;
      // (the window's own text size, so it reads as the rest of the window at every scale)
      ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitText(line.c_str(), w));
    }
    // The progress lives under the body in a row of its own (a line of text over the bar): while
    // nothing runs it says what a split will do. The body is as wide as the bar from the start, so
    // starting a split moves nothing. A body taller than the screen scrolls, and its visible part
    // ends on the edge of a section, never through a row of buttons.
    const float rowH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    static std::vector<float> edges, lastEdges;  // where each section ends in the body (this frame, the last)
    lastEdges.swap(edges);
    edges.clear();
    {
      ui::SettleDialogCentred();
      const float padY = ImGui::GetStyle().WindowPadding.y;
      const float room = std::max(120.0f, ui::DialogArea().y - ImGui::GetCursorPosY() - padY - ui::DialogFooterHeight(1) - rowH);
      float maxH = room;
      if (!lastEdges.empty() && lastEdges.back() > room) {  // it scrolls: show whole sections only
        float fit = 0;
        for (float e : lastEdges)
          if (e <= room) fit = e;
        if (fit > 0) maxH = fit;
      }
      ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxH));
      ImGui::BeginChild("##stemsbody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                        ImGuiWindowFlags_NoBackground);
    }
    auto sectionEnd = [&] {
      ui::EndSection();
      edges.push_back(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
    };
    ImGui::Dummy(ImVec2(w, 0));
    modelSection(a, w);
    edges.push_back(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
    const st::ModelInfo& m = currentModel();
    const bool have = st::modelInstalled(m);
    if (!have) {
      downloadSection(a, w);
      edges.push_back(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
    }
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
    sectionEnd();
    ui::BeginSection(i18n::tr("RESULT"), w);
    if (s.clip != kNoId) {
      ImGui::BeginDisabled(splitting);
      ImGui::RadioButton(s.wholeClip ? i18n::tr("New tracks under the clip, lined up with it")
                                     : i18n::tr("New tracks under the clip, lined up with the part"), &s.place, 0);
      ImGui::RadioButton(i18n::tr("Files only, in the Stems folder"), &s.place, 1);
      ImGui::BeginDisabled(s.place != 0);
      ImGui::Checkbox(s.wholeClip ? i18n::tr("Mute the original clip") : i18n::tr("Mute that part of the original clip"), &s.muteOriginal);
      ImGui::EndDisabled();
      ImGui::EndDisabled();
    } else {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("The stems are saved as WAV files in the Stems folder (Browser, Stems)."));
      ImGui::PopTextWrapPos();
    }
    sectionEnd();
    if (!splitting && !s.lastError.empty()) {
      ImGui::PushTextWrapPos(w - theme::kS2);
      ImGui::TextColored(theme::vec(P.danger), "%s", s.lastError.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    {  // the progress row: always there (a line of text over the bar), so the window keeps its size when a split starts
      if (splitting) {
        // 0.7.8: the percent in a field as wide as "100%", the time left after it: nothing jumps.
        const double f = fk ? fk->frac : j.progress.load();
        const double el = fk ? 60.0 : std::chrono::duration<double>(Clock::now() - j.started).count();
        const std::string rest = "  \xC2\xB7  " + (f > 0.02 ? timeLeft(el / f * (1.0 - f)) : std::string(i18n::tr("starting...")));
        progui::Percent(f, P.textDim);
        ImGui::SameLine(0, 0);
        progui::Line(rest.c_str(), w - progui::PercentWidth(), P.textDim);
        ImGui::ProgressBar(float(f), ImVec2(w, 0), "");
      } else {  // what a split will do, in the same two rows
        int n = 0;
        for (const auto& src : currentModel().sources) n += s.keep[src] ? 1 : 0;
        std::string what = std::string(i18n::tr(currentModel().title.c_str())) + "  \xC2\xB7  " +
                           (n == 0 ? std::string(i18n::tr("no stems chosen")) : i18n::plural(n, "%d stem", "%d stems"));
        if (!st::modelInstalled(currentModel())) what += std::string("  \xC2\xB7  ") + i18n::tr("download needed");
        const char* where = s.clip != kNoId && s.place == 0
                                ? (s.wholeClip ? i18n::tr("On new tracks under the clip, saved in the Stems folder")
                                               : i18n::tr("On new tracks under the part, saved in the Stems folder"))
                                : i18n::tr("WAV files in the Stems folder (Browser, Stems)");
        progui::Line(what.c_str(), w, P.textDim);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float fh = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + std::round((fh - ImGui::GetTextLineHeight()) * 0.5f)), P.textFaint,
                                            theme::fitText(where, w));
        ImGui::Dummy(ImVec2(w, fh));
      }
    }
    int nKeep = 0;
    for (const auto& src : m.sources) nKeep += s.keep[src] ? 1 : 0;
    const float bw = 96.0f;
    ui::DialogFooter(ui::ButtonRowWidth(2, bw));
    if (splitting) {
      if (ui::TextButton(i18n::tr("Cancel"), false, bw) || ::gl::keys::escapePressed()) j.cancel = true;
      ImGui::SameLine(0, theme::kS2);
      ImGui::BeginDisabled(true);
      ui::PrimaryButton(i18n::tr("Split"), bw);
      ImGui::EndDisabled();
    } else {
      if (ui::TextButton(i18n::tr("Close"), false, bw)) open = false;
      ImGui::SameLine(0, theme::kS2);
      const bool can = have && nKeep > 0 && !j.running.load();
      ImGui::BeginDisabled(!can);
      const bool go = ui::PrimaryButton(i18n::tr("Split"), bw, i18n::tr("Runs in the background on this computer")) ||
                      (can && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter));
      ImGui::EndDisabled();
      if (trace()) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        std::printf("stems: split %d %d\nstems: model %s ready %d keep %d\n", int((mn.x + mx.x) * 0.5f), int((mn.y + mx.y) * 0.5f),
                    currentModel().id.c_str(), have ? 1 : 0, nKeep);
      }
      if (go && can) startSplit(a);
      if (ImGui::IsWindowFocused() && ::gl::keys::escapePressed()) open = false;
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

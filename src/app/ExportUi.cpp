// Gridline — export & interop UI (J-20). See ExportUi.h.
//
// The render dialog follows the reference's render form: export type and tail
// at the top, file name and location, output formats (several at once), then
// quality and the miscellaneous switches (split mixer tracks, master / insert
// effects, tempo info, markers). Rendering runs on a worker thread with a
// progress window that can cancel; the summary lists the files written.
#include "../core/I18n.h"
#include "../core/ThreadJoin.h"
#include "../core/ThreadName.h"
#include "Keys.h"
#include "LiveLook.h"
#include "../engine/LiveRender.h"
#include "../core/TrackReceipt.h"
#include "../core/Meter.h"
#include "ExportUi.h"
#include "ProgressUi.h"
#include "LogicPlugins.h"
#include "../core/Crash.h"
#include "../core/ProjectArchive.h"
#include "../core/RemoteFs.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"  // (N58-11: the Render dialog's measuring frames)
#include "../core/Arrangements.h"
#include "../core/Automation.h"
#include "../core/Log.h"
#include "../core/flp/Flp.h"
#include "../core/flp/FlpExport.h"
#include "../core/flp/FlpPackage.h"
#include "../core/ProjectFiles.h"
#include "../core/DawProject.h"
#include "../core/MidiFile.h"
#include "../core/MidiImport.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"
#include "../engine/ClipOps.h"
#include "../engine/ClipRender.h"
#include "../engine/ExportJob.h"
#include "../engine/ExportChoices.h"
#include "../engine/MarkerAreas.h"
#include "../engine/Resample.h"
#include "../engine/Sample.h"
#include "../packs/PacksUi.h"
#include "FreezeUi.h"
#include "App.h"
#include "BrowserUi.h"
#include "CollabUi.h"
#include "ContextMenus.h"
#include "DevSignal.h"
#include "FlpExportUi.h"
#include "HostedUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "WorkerStall.h"
#include "../core/Platform.h"
#include "../host/HostedPlugins.h"
#include "../packs/samplers/patchplayer.h"

namespace fs = std::filesystem;

namespace gl::exportui {

using theme::pal;
using menus::fileDialog;

namespace {

// ---------------------------------------------------------------- state

enum class Dest { Files = 0, AudioClip, AudioEditor };

struct Dialog {
  bool open = false;
  bool init = false;
  ExportSettings s;
  int scope = 0;           // 0 song, 1 pattern, 2 time selection, 3 marker range
  int arrangement = -1;    // K-406: -1 = current, 0..N-1 = specific arrangement, -2 = all arrangements
  int markerA = 0, markerB = 1;
  bool fmt[6] = {true, false, false, false, false, false};  // WAV FLAC MP3 Ogg M4A Opus
  int wavBits = 1;         // 0 16, 1 24, 2 32f
  int flacBits = 1;        // 0 16, 1 24
  int mp3Rate = 3, oggRate = 2, m4aRate = 2, opusRate = 1;  // index in kBitrates
  int rateIdx = 2;         // index in kRates
  int dest = 0;            // Dest
  bool replacePattern = false;
  char folder[1024] = "";
  char filePattern[160] = "%P";  // the file's name as pieces (ExportChoices.h); "%P" = the project's name
  char pattern[128] = "%P - %N %T";
  bool lastSplit[3] = {true, false, false};  // the kinds ticked when Separate files was last on
  // Export marker areas: the dialog lists the arrangement's marker areas (ticked: exported), and
  // names each area's file with `areaPattern` (MarkerAreas.h). Ticks are kept by the area's start.
  bool areaMode = false;
  char areaPattern[160] = "%P - %M";
  std::map<Tick, bool> areaOn;
};
Dialog dlg;

// The dialog's ticks: an area not touched yet is ticked when something plays in it.
bool areaTicked(const MarkerArea& m) {
  const auto it = dlg.areaOn.find(m.start);
  return it == dlg.areaOn.end() ? m.hasContent : it->second;
}
// Dev: the area rows the dialog drew last (name, start, length, ticked), and its frame.
std::vector<std::string> devAreaRows;
int devAreaFrame = -1;

constexpr int kRates[] = {22050, 32000, 44100, 48000, 88200, 96000, 192000};
constexpr int kBitrates[] = {128, 192, 256, 320};

// Worker + progress.
struct Job {
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> progress{0};
  std::mutex mu;
  std::string status;  // guarded by mu
  ExportOutcome outcome;
  ExportSettings settings;
  Dest dest = Dest::Files;
  bool replacePattern = false;
  Tick start = 0;  // scope start (song ticks) for audio clips
  size_t areas = 0;  // marker areas exported (0: a plain render)
  bool showSummary = false;
  double started = 0, seconds = 0;
  WorkerBeat beat;   // K-133: the worker's sign of life (each progress step)
  StallWatch watch;  // the interface's view of it
  ~Job() {
    cancel = true;
    gl::threads::safeJoin(worker);
  }
};
Job job;
int devRendersFinished = 0, devRendersCancelled = 0, devRendersFailed = 0;  // since the start (devState)
std::vector<archive::CopyHold> renderHolds;  // K-47: the unpacked copies the running render reads

// K-47: the other file work that can take long (zip save, project data and bones, WAV of a clip or
// a sample, mixer tracks to wave files, saving into a new folder).
struct FileJob {
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> progress{0};
  std::mutex mu;
  std::string status;  // guarded by mu
  std::string title;
  FileJobDone done;    // written by the worker before `finished`
  std::vector<archive::CopyHold> holds;
  int finishedCount = 0, placed = 0;
  WorkerBeat beat;   // K-133: as for a render
  StallWatch watch;
  ~FileJob() {
    cancel = true;
    gl::threads::safeJoin(worker);
  }
};
FileJob fileJob;

// MIDI import options.
struct MidiDlg {
  bool open = false;
  std::string path;
  MidiFileData data;
  std::vector<bool> tracks;
  bool channels[16];
  MidiImportOptions o;
  bool patchPlayer = false;  // K-333: one Patch Player per track on the SF2 file `sf2`
  std::string sf2;
};
MidiDlg midiDlg;

// Missing samples.
struct Missing {
  bool open = false;
  std::vector<std::string> files;
  RelinkReport found;
  std::string folder;
  bool searched = false;
  // K-42: checks and searches run off the interface thread (the files or the folder can be on a
  // web drive). Each job owns its cell, so a stuck drive leaves only that thread waiting.
  struct Check {
    std::atomic<bool> done{false};
    std::vector<std::string> files;
  };
  struct Search {
    std::atomic<bool> done{false}, cancel{false};
    RelinkReport found;
    std::string folder;
  };
  std::shared_ptr<Check> check;
  std::shared_ptr<Search> search;
  double nextCheck = 0;
};
Missing missing;

void formLabel(const char* label, float x = 132.0f) { ui::FormLabelFit(i18n::tr(label), x, pal().textDim); }  // J-54: fitted

// J-54: centred in the work area, size clamped to the screen (short screens / large interface scale).
void centerNext(ImVec2 size, ImVec2 minSize = ImVec2(0, 0)) { ui::PlaceNextDialog(size, ImGuiCond_Appearing, minSize); }

bool combo(const char* id, int* idx, const char* const* items, int n, float w) {
  bool changed = false;
  if (ui::BeginCombo(id, items[std::clamp(*idx, 0, n - 1)], w)) {
    for (int k = 0; k < n; ++k)
      if (ImGui::Selectable(items[k], k == *idx)) {
        *idx = k;
        changed = true;
      }
    ImGui::EndCombo();
  }
  return changed;
}

std::string homeDir() {
  const char* h = gl::platform::homeEnv();
  return h ? h : ".";
}

std::string projectFolder(const App& a) {
  // R06-13: the Tracks layout bounces into the project's Bounces folder, as the reference does
  if (logicplug::active(a) && !a.projectPath.empty()) return (fs::path(a.projectPath).parent_path() / "Bounces").string();
  return a.projectPath.empty() ? homeDir() : fs::path(a.projectPath).parent_path().string();
}

// Rendered audio that becomes part of the project (audio clips, the audio editor).
std::string renderedFolder(const App& a) {
  return a.projectPath.empty() ? (fs::path(a.configDir()) / "Rendered").string() : (fs::path(a.projectPath).parent_path() / "Rendered").string();
}

std::string projectName(const App& a) {
  // J-111f: a collaborative project's shared name (what everyone calls it) names its exports
  if (std::string shared = collabui::projectTitle(a); !shared.empty()) return shared;
  const std::string t = a.doc.song().info.title;
  if (!t.empty() && t != "Untitled") return t;
  if (!a.projectFile().empty()) return projectStemOf(a.projectFile());
  return t.empty() ? "Untitled" : t;
}

int firstFreeTrack(const Song& s) {
  for (int t = 0; t < kMaxPlaylistTracks; ++t) {
    bool used = false;
    for (const auto& c : s.playlist) used = used || c.track == t;
    if (!used) return t;
  }
  return 0;
}

// Checkpoint 55 (N54-06): the next render in Clips is the clip grid's (the person chose "Export the
// Session" on the empty-Arrangement card): its launched clips, as Studio renders them.
bool gSessionExport = false;
// Checkpoint 56 (N55-08): what that render plays: the clips playing now, else the selected scene's
// (livelook::sessionExportClips), and the words its file name ends with.
std::vector<ObjectId> gSessionClips;
std::string gSessionLabel;
constexpr const char* kEmptyArrangement = "the arrangement is empty";

// Scope of the dialog → render job fields. False (with a reason) when empty.
bool applyScope(const App& a, RenderJob& r, std::string* why) {
  const Song& baseSong = a.doc.song();
  const int nArr = arrangementCount(baseSong);
  const int curArr = baseSong.arrangements.empty() ? 0 : baseSong.currentArrangement;
  const bool multiArr = dlg.scope != 1 && nArr > 1;
  const bool allArr = multiArr && dlg.arrangement == -2 && dlg.dest == static_cast<int>(Dest::Files);
  const int targetArr = multiArr && dlg.arrangement >= 0 && dlg.arrangement < nArr ? dlg.arrangement : curArr;
  Song chosenStorage;
  const Song& s = (multiArr && !allArr && targetArr != curArr) ? (chosenStorage = songWithArrangement(baseSong, targetArr)) : baseSong;
  r.scope = RenderScope::Song;
  // N48-10: Clips exports its arrangement, never the clip grid's slots (N55-08: a Session export is first
  // made an arrangement of the clips it plays: startJob).
  r.arrangementOnly = livelook::active(a);
  if (allArr && dlg.scope == 0 && !gSessionExport) {
    bool any = false;
    for (int i = 0; i < nArr; ++i) any = any || (arrangementLengthTicks(baseSong, i) > 0);
    if (!any) return why && (*why = "every arrangement is empty", false);
    return true;
  }
  if (r.arrangementOnly && dlg.scope == 0 && !gSessionExport) {
    bool any = false;
    for (const Clip& c : s.playlist) any = any || (c.start >= s.live.areaEnd && !c.muted && c.kind != ClipKind::Automation);
    if (!any) return why && (*why = kEmptyArrangement, false);
  }
  switch (dlg.scope) {
    case 1:
      r.scope = RenderScope::Pattern;
      r.pattern = a.curPattern;
      if (!s.pattern(a.curPattern)) return why && (*why = "no pattern selected", false);
      break;
    case 2:
      r.scope = RenderScope::Range;
      r.start = a.playlist.selStart;
      r.end = a.playlist.selEnd;
      if (r.end <= r.start) return why && (*why = livelook::active(a) ? "no time selection in the Arrangement" : "no time selection in the playlist", false);
      break;
    case 3: {
      r.scope = RenderScope::Range;
      const auto& m = s.markers;
      if (m.size() < 2) return why && (*why = livelook::active(a) ? "needs two markers" : "needs two playlist markers", false);
      const int A = std::clamp(dlg.markerA, 0, static_cast<int>(m.size()) - 1), B = std::clamp(dlg.markerB, 0, static_cast<int>(m.size()) - 1);
      r.start = m[static_cast<size_t>(A)].pos;
      r.end = m[static_cast<size_t>(B)].pos;
      if (r.end <= r.start) return why && (*why = "the end marker is not after the start marker", false);
      break;
    }
    default:
      if (s.lengthTicks() <= 0) return why && (*why = logicplug::active(a) ? "the project is empty" : livelook::active(a) ? "the Arrangement is empty" : "the playlist is empty", false);  // R06-13
      break;
  }
  return true;
}

// The length of a render job's scope (cached a few times a second per scope).
double scopeSecondsOf(const App& a, const RenderJob& r) {
  static double cached = 0, at = -1;
  static int scope = -1;
  static Tick s0 = -1, s1 = -1;
  if (at >= 0 && scope == static_cast<int>(r.scope) && s0 == r.start && s1 == r.end && a.now >= at && a.now - at < 0.25) return cached;
  at = a.now, scope = static_cast<int>(r.scope), s0 = r.start, s1 = r.end;
  Tick A = 0, B = 0;
  cached = renderScopeTicks(a.doc.song(), r, A, B) ? static_cast<double>(scopeFrames(a.doc.song(), r, A, B, 48000)) / 48000.0 : 0.0;
  return cached;
}

double scopeSeconds(const App& a) {
  // A few times a second: the song scope copies the whole project.
  static double cached = 0, at = -1;
  static int scope = -1, arr = -99;
  if (at >= 0 && scope == dlg.scope && arr == dlg.arrangement && a.now >= at && a.now - at < 0.25) return cached;
  at = a.now;
  scope = dlg.scope;
  arr = dlg.arrangement;
  cached = 0;
  RenderJob r;
  if (!applyScope(a, r, nullptr)) return 0;
  const Song& base = a.doc.song();
  const int nArr = arrangementCount(base);
  if (dlg.scope != 1 && nArr > 1 && dlg.arrangement == -2 && dlg.dest == static_cast<int>(Dest::Files)) {
    double total = 0;
    for (int i = 0; i < nArr; ++i) {
      Song tmp = songWithArrangement(base, i);
      Tick A = 0, B = 0;
      if (renderScopeTicks(tmp, r, A, B)) total += static_cast<double>(scopeFrames(tmp, r, A, B, 48000)) / 48000.0;
    }
    cached = total;
    return cached;
  }
  const int cur = base.arrangements.empty() ? 0 : base.currentArrangement;
  Song chosenStorage;
  const Song& s = (dlg.scope != 1 && dlg.arrangement >= 0 && dlg.arrangement < nArr && dlg.arrangement != cur)
                      ? (chosenStorage = songWithArrangement(base, dlg.arrangement))
                      : base;
  Tick A = 0, B = 0;
  if (!renderScopeTicks(s, r, A, B)) return 0;
  cached = static_cast<double>(scopeFrames(s, r, A, B, 48000)) / 48000.0;
  return cached;
}

// The encoder of a format as the dialog shows it: searched for a few times a second, not on every
// frame (on Windows a search looks in each PATH folder, some of which can be slow network drives).
struct EncoderLook {
  std::string name, status;
  double at = -1;
};
const EncoderLook& encoderLook(const App& a, ExportFormat f) {
  static EncoderLook looks[static_cast<int>(ExportFormat::Count)];
  EncoderLook& l = looks[static_cast<int>(f)];
  if (l.at < 0 || a.now < l.at || a.now - l.at >= 2.0) {
    l.name = encoderFor(f);
    l.status = encoderStatus(f);
    l.at = a.now;
  }
  return l;
}

// ---------------------------------------------------------------- worker

void startJob(App& a) {
  if (job.running) return;
  if (job.worker.joinable()) job.worker.join();
  captureHostedStates(a);  // offline instances load the plugins' current state
  ExportSettings s = dlg.s;
  std::string why;
  std::vector<MarkerArea> areas;  // Export marker areas: the ticked ones, in order
  if (dlg.areaMode) {
    dlg.dest = static_cast<int>(Dest::Files);
    const std::vector<MarkerArea> all = markerAreas(a.doc.song());
    for (const MarkerArea& m : all)
      if (areaTicked(m)) areas.push_back(m);
    if (areas.empty()) {
      a.notify(all.empty() ? "Nothing to render: the arrangement has no markers" : "Tick at least one marker area", true);
      return;
    }
  } else if (!applyScope(a, s.render, &why)) {
    if (why == kEmptyArrangement && livelook::active(a)) {
      // N54-06: Clips exports its Arrangement; an empty one says so and offers the Session's clips or a
      // recording of them into the Arrangement first.
      // N55-08: the clips playing now, else the selected scene's; never every scene back to back.
      std::string label;
      const std::vector<ObjectId> ids = livelook::sessionExportClips(a, label);
      std::vector<Toast::Action> acts;
      if (!ids.empty()) {
        acts.push_back({"Export " + label, [&a, ids, label] {
                          gSessionExport = true;
                          gSessionClips = ids;
                          gSessionLabel = label;
                          startJob(a);
                          gSessionExport = false;
                          gSessionClips.clear();
                          gSessionLabel.clear();
                        }});
      }
      acts.push_back({"Record it first", [&a] {
                        if (!a.recordArmed) {
                          livelook::toggleArrangementRecord(a);
                        }
                      }});
      a.notifyAction("Nothing in the Arrangement to export", std::move(acts), 20.0);
      return;
    }
    a.notify("Nothing to render: " + why, true);
    return;
  }
  s.formats.clear();
  const Dest dest = static_cast<Dest>(dlg.dest);
  if (dest == Dest::Files) {
    s.formats = exportchoice::tickedFormats(dlg.fmt, static_cast<int>(ExportFormat::Count));
    for (ExportFormat f : s.formats)
      if (exportFormatExternal(f) && encoderFor(f).empty()) {
        a.notify(std::string(exportFormatName(f)) + ": " + encoderStatus(f), true);
        return;
      }
    if (s.formats.empty() && !s.midi) {
      a.notify("Choose at least one output format", true);
      return;
    }
    s.folder = dlg.folder[0] ? std::string(dlg.folder) : projectFolder(a);
    s.baseName = expandNamePattern(dlg.filePattern[0] ? dlg.filePattern : "%P", {{'P', projectName(a)},
                                                                                {'R', std::to_string(kRates[dlg.rateIdx])},
                                                                                {'S', dlg.scope == 1 ? "Pattern" : dlg.scope == 0 ? "Song" : "Range"}});
    if (s.baseName.empty()) s.baseName = safeFileName(projectName(a));
    if (gSessionExport && !gSessionLabel.empty()) {
      s.baseName += " (" + gSessionLabel + ")";  // N55-08: says what it holds
    }
  } else {  // rendered into the project: one 32-bit float WAV, no splitting
    s.formats = {ExportFormat::Wav};
    s.wavBits = 32;
    s.midi = false;
    s.splitMixer = s.splitPlaylist = s.splitPlaylistGroups = false;
    s.channels = ChannelMode::Stereo;
    s.markers = false;
    s.folder = renderedFolder(a);
    const Pattern* p = a.doc.song().pattern(a.curPattern);
    s.baseName = (s.render.scope == RenderScope::Pattern && p ? p->name : projectName(a)) + " (rendered " +
                 std::to_string(static_cast<long long>(std::time(nullptr)) % 100000) + ")";
  }
  s.wavBits = dest == Dest::Files ? (dlg.wavBits == 0 ? 16 : dlg.wavBits == 1 ? 24 : 32) : 32;
  s.flacBits = dlg.flacBits == 0 ? 16 : 24;
  s.mp3Bitrate = kBitrates[dlg.mp3Rate];
  s.oggBitrate = kBitrates[dlg.oggRate];
  s.m4aBitrate = kBitrates[dlg.m4aRate];
  s.opusBitrate = kBitrates[dlg.opusRate];
  s.render.sampleRate = kRates[dlg.rateIdx];
  s.partPattern = dlg.pattern;
  {  // K-344: the subfolders when more than one kind of split is on, in this layout's words
    const bool tw = logicplug::active(a) || livelook::active(a);
    s.mixerFolder = i18n::tr(logicplug::active(a) ? i18n::N_("Channel strips") : i18n::N_("Mixer tracks"));
    s.playlistFolder = tw ? i18n::tr("Tracks") : i18n::tr("Playlist tracks");
    s.groupsFolder = i18n::tr("Track groups");
  }
  s.projectTitle = projectName(a);
  s.render.threads = std::max(1, a.processingThreads());
  // K-406: which arrangement renders (marker areas are always the current arrangement's).
  const int nArr = arrangementCount(a.doc.song());
  s.arrangement = (!dlg.areaMode && dlg.scope != 1 && nArr > 1)
                      ? ((dlg.arrangement == -2 && dest == Dest::Files) ? -2 : (dlg.arrangement >= 0 && dlg.arrangement < nArr ? dlg.arrangement : -1))
                      : -1;
  std::vector<std::string> names;
  if (!areas.empty()) names = markerAreaFileNames(areas, dlg.areaPattern, projectName(a), s.render.sampleRate);
  job.areas = areas.size();
  job.settings = s;
  job.dest = dest;
  job.replacePattern = dlg.replacePattern;
  job.start = s.render.scope == RenderScope::Range ? s.render.start : 0;
  job.cancel = false;
  job.finished = false;
  job.progress = 0;
  job.outcome = ExportOutcome{};
  job.showSummary = false;
  job.running = true;
  job.started = a.now;
  {
    std::lock_guard<std::mutex> lk(job.mu);
    job.status = "Preparing";
  }
  Song song = a.doc.song();
  if (gSessionExport) {
    prepareSessionExport(song, gSessionClips);  // N55-08: the chosen clips together, for the longest one's length
  }
  renderHolds = archive::holdCopiesOf(song);  // closing the project during the render takes nothing from it
  dlg.open = false;
  crash::crumb("export start");
  // A worker for every project: the window stays alive and Cancel works. The calls that must happen
  // on the interface thread for hosted plugins come back to it (host::UiThreadRenderScope).
  // The interface does not wait for this work, so its progress is no sign of the interface's life:
  // a heartbeat here would hide a frozen window. Frames beat by themselves; the calls that come back
  // are busy work (host::runUiThreadTasks).
  // K-133: but its progress is the worker's own sign of life: a worker that stops is reported.
  workerstall::start(job.watch, job.beat);
  job.worker = std::thread([song = std::move(song), s, areas = std::move(areas), names = std::move(names)]() { gl::threads::setName("gl-export");
    host::UiThreadRenderScope scope;
    ExportOutcome o;
    try {
      const auto step = [](double p, const std::string& what) {
        job.beat.beat();
        job.progress = p;
        std::lock_guard<std::mutex> lk(job.mu);
        job.status = what;
      };
      // reports what it catches (one report, one pop-up); marker areas: one export per area
      o = areas.empty() ? runExport(song, s, step, &job.cancel) : runMarkerAreaExport(song, s, areas, names, step, &job.cancel);
    } catch (...) {  // only what runExport could not catch itself
      crash::reportCurrentException("Exporting audio");
      o.errors.push_back("An internal error stopped the export.");
    }
    crash::crumb("export end");
    {
      std::lock_guard<std::mutex> lk(job.mu);
      job.outcome = std::move(o);
    }
    job.finished = true;
    job.running = false;
  });
}

// UI thread: what to do with the result.
void finishJob(App& a) {
  if (!job.finished.exchange(false)) return;
  if (job.worker.joinable()) job.worker.join();
  renderHolds.clear();
  devsig::reach("render-finished");
  job.seconds = a.now - job.started;
  ExportOutcome o;
  {
    std::lock_guard<std::mutex> lk(job.mu);
    o = job.outcome;
  }
  ++(o.cancelled ? devRendersCancelled : o.errors.empty() ? devRendersFinished : devRendersFailed);
  if (o.cancelled && job.areas > 0) {  // marker areas: the areas finished before Cancel stay
    if (o.files.empty()) {
      a.notify("Render cancelled");
    } else {
      a.notify("Render cancelled: the files of the finished marker areas are kept");
      job.showSummary = true;
    }
    return;
  }
  if (o.cancelled) {
    a.notify("Render cancelled");
    for (const auto& f : o.files) {
      std::error_code ec;
      fs::remove(f, ec);
      // K-344: a split's own subfolder goes too once empty (fs::remove leaves a folder with files)
      const fs::path dir = fs::path(f).parent_path();
      if (dir.lexically_normal() != fs::path(job.settings.folder).lexically_normal()) fs::remove(dir, ec);
    }
    return;
  }
  if (job.dest != Dest::Files && !o.files.empty()) {
    const std::string file = o.files[0];
    const Song& s = a.doc.song();
    const ObjectId pattern = job.settings.render.pattern;
    const bool patternScope = job.settings.render.scope == RenderScope::Pattern;
    const Pattern* p = s.pattern(pattern);
    const std::string chName = (patternScope && p ? p->name : projectName(a)) + " (rendered)";
    if (job.dest == Dest::AudioEditor) {
      ObjectId ch = a.addInstrument("gl.sampler", file);
      if (ch) a.doc.editChannel(ch, "rename", [&](Channel& c) { c.name = chName; });
      packs::openAudioEditor(a, file, ch);
      a.notify(logicplug::active(a) ? "Rendered into the audio editor" : "Rendered into Loupe");  // R06-13
      a.uiSound(uisound::Event::Export);
    } else {
      const Tick at = job.start;
      const bool replace = job.replacePattern && patternScope;
      a.doc.editSong(replace ? "render and replace" : "render to audio clip", [&](Song& x) {
        Channel c;
        c.id = x.allocateId();
        c.name = chName;
        c.color = 0x8A9BA8;
        c.plugin = defaultPluginState("gl.sampler");
        c.plugin.data = file;
        c.mixerTrack = x.masterId();
        x.channels.push_back(c);
        auto place = [&](int track, Tick start, Tick length) {
          Clip k;
          k.id = x.allocateId();
          k.kind = ClipKind::Audio;
          k.source = c.id;
          k.track = track;
          k.start = start;
          k.length = length;
          if (static_cast<int>(x.tracks.size()) <= track) x.tracks.resize(static_cast<size_t>(track) + 1);
          x.playlist.push_back(k);
        };
        if (replace) {
          std::vector<Clip> old;
          for (const auto& k : x.playlist)
            if (k.kind == ClipKind::Pattern && k.pattern == pattern) old.push_back(k);
          x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(),
                                          [&](const Clip& k) { return k.kind == ClipKind::Pattern && k.pattern == pattern; }),
                           x.playlist.end());
          for (const auto& k : old) place(k.track, k.start, x.clipLength(k) > 0 ? std::min(x.clipLength(k), audioClipNaturalLength(x, k)) : 0);
          if (old.empty()) place(firstFreeTrack(x), 0, 0);
        } else {
          place(firstFreeTrack(x), at, 0);
        }
        for (auto& k : x.playlist)  // natural length of the new clips
          if (k.kind == ClipKind::Audio && k.source == c.id && k.length == 0) k.length = audioClipNaturalLength(x, k);
      });
      a.showPlaylist = true;
      a.notify(replace ? "Pattern clips replaced by the rendered audio" : "Rendered as an audio clip");
      if (o.errors.empty()) a.uiSound(uisound::Event::Export);
    }
    for (const auto& e : o.errors) a.notify(e, true);
    return;
  }
  job.showSummary = true;
  if (!o.errors.empty())
    a.notify(o.errors[0], true);
  else {
    a.notify("Exported " + std::to_string(o.files.size()) + " file(s)" +
             (job.areas > 0 ? " from " + std::to_string(job.areas) + " marker area(s)" : std::string()));
    a.uiSound(uisound::Event::Export);
  }
}

// ---------------------------------------------------------------- render dialog

void initDialog(App& a) {
  if (dlg.init) return;
  dlg.init = true;
  dlg.s.render.tail = TailMode::Leave;
  dlg.s.render.maxTailSeconds = 20;
  dlg.s.render.fixedTailSeconds = 2;
  for (int k = 0; k < 7; ++k)
    if (kRates[k] == a.audioSampleRate) dlg.rateIdx = k;
}

// "1.1": an area's start as the ruler shows it (bar.beat).
std::string barBeat(const MeterMap& mm, Tick t) {
  const MeterMap::Bbt q = mm.toBbt(t);
  return std::to_string(q.bar + 1) + "." + std::to_string(q.beat + 1);
}
// "3:05.2": an area's length in time (the tempo map's, as the file will run without its tail).
std::string minSec(double sec) {
  char b[32];
  std::snprintf(b, sizeof b, "%d:%04.1f", static_cast<int>(sec / 60), std::fmod(sec, 60.0));
  return b;
}
double areaSeconds(const Song& song, const MarkerArea& m) {
  RenderJob r;
  r.scope = RenderScope::Range;
  r.start = m.start;
  r.end = m.end;
  return static_cast<double>(scopeFrames(song, r, m.start, m.end, 48000)) / 48000.0;
}

// ---- the Export audio dialog: "What to export" cards over three columns (what, its ending and
// where it goes | split, files and names | format, quality and processing), and a footer that
// sums up the render beside Cancel / Render.

constexpr float kColW = 370.0f;    // each column
constexpr float kRowX = 104.0f;    // where a row's control starts (its label to the left)
constexpr float kCardH = 44.0f;

void rowLabel(const char* label) { formLabel(label, kRowX); }

// Words that follow the layout.
const char* songWord(const App& a) { return livelook::active(a) ? i18n::N_("Arrangement") : i18n::N_("Whole song"); }
const char* patternWord(const App& a) {
  return livelook::active(a) ? i18n::N_("Current clip") : logicplug::active(a) ? i18n::N_("Current region") : i18n::N_("Current pattern");
}
const char* selectionWord(const App& a) { return logicplug::active(a) ? i18n::N_("Cycle area") : i18n::N_("Time selection"); }

// The card's glyph: 0 song (a waveform), 1 selection (brackets round a block), 2 marker areas
// (three flags), 3 pattern (a step grid).
void cardGlyph(ImDrawList* dl, int kind, ImVec2 c, ImU32 col) {
  const float s = 1.0f;
  switch (kind) {
    case 0: {
      const float h[] = {3, 7, 10, 5, 9, 4, 2};
      for (int i = 0; i < 7; ++i) {
        const float x = std::round(c.x - 9 + i * 3.0f) + 0.5f;
        dl->AddLine(ImVec2(x, c.y - h[i]), ImVec2(x, c.y + h[i]), col, 1.5f * s);
      }
      break;
    }
    case 1:
      dl->AddLine(ImVec2(c.x - 7, c.y - 8), ImVec2(c.x - 9, c.y - 8), col, 1.5f);
      dl->AddLine(ImVec2(c.x - 9, c.y - 8), ImVec2(c.x - 9, c.y + 8), col, 1.5f);
      dl->AddLine(ImVec2(c.x - 9, c.y + 8), ImVec2(c.x - 7, c.y + 8), col, 1.5f);
      dl->AddLine(ImVec2(c.x + 7, c.y - 8), ImVec2(c.x + 9, c.y - 8), col, 1.5f);
      dl->AddLine(ImVec2(c.x + 9, c.y - 8), ImVec2(c.x + 9, c.y + 8), col, 1.5f);
      dl->AddLine(ImVec2(c.x + 9, c.y + 8), ImVec2(c.x + 7, c.y + 8), col, 1.5f);
      dl->AddRectFilled(ImVec2(c.x - 4, c.y - 3), ImVec2(c.x + 4, c.y + 3), theme::withAlpha(col, 0.55f), 1.0f);
      break;
    case 2:
      for (int i = 0; i < 3; ++i) {
        const float x = std::round(c.x - 8 + i * 7.0f) + 0.5f;
        dl->AddLine(ImVec2(x, c.y + 8), ImVec2(x, c.y - 8), col, 1.5f);
        dl->AddTriangleFilled(ImVec2(x, c.y - 8), ImVec2(x + 4.5f, c.y - 6), ImVec2(x, c.y - 4), col);
      }
      break;
    default:
      for (int r = 0; r < 2; ++r)
        for (int k = 0; k < 4; ++k) {
          const bool on = (k + r) % 2 == 0;
          const ImVec2 p(c.x - 9 + k * 5.0f, c.y - 6 + r * 8.0f);
          dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + 4), on ? col : theme::withAlpha(col, 0.4f), 0.6f);
        }
      break;
  }
}

// One "What to export" card. True when clicked.
bool scopeCard(App& a, const char* id, int glyph, const char* title, const std::string& sub, bool on, float w) {
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, kCardH));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 q(p.x + w, p.y + kCardH);
  const ImU32 face = on ? theme::mix(P.field, P.hot, P.dark ? 0.14f : 0.10f)
                        : P.dark ? theme::mix(P.surface900, P.field, hov ? 0.20f : 0.38f) : theme::mix(P.surface900, P.surface800, hov ? 0.85f : 0.55f);
  dl->AddRectFilled(p, q, face, theme::kRadiusLg);
  dl->AddRect(p, q, on ? theme::withAlpha(P.hot, 0.65f) : P.dark ? theme::withAlpha(P.ink, 0.45f) : P.line, theme::kRadiusLg);
  cardGlyph(dl, glyph, ImVec2(p.x + 22, p.y + kCardH * 0.5f), on ? P.hot : P.textFaint);
  const float tx = p.x + 42;
  dl->AddText(a.fontBold, theme::kFontBody, ImVec2(tx, p.y + 5), P.text, i18n::tr(title));
  dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(tx, p.y + 24), on ? P.textDim : P.textFaint, sub.c_str());
  return clicked;
}

// Bottoms of the three columns line up: each column's last section takes the room the tallest
// column needs (measured on the frame before).
struct ColumnFit {
  float natural[3] = {0, 0, 0};
  float pad[3] = {0, 0, 0};
  int cols = 3;  // how many columns the measure is for
};
ColumnFit colFit;
void columnPad(int c) {
  if (colFit.pad[c] > 0.5f) ImGui::Dummy(ImVec2(0, colFit.pad[c]));
}

// Name pieces for a pattern (`buf`, the tokens it offers): the pieces in a field (a token is a
// lit chip, text is text; a chip's x removes it, text opens to be edited), and under it the
// pieces that can be added ("+ Marker", ...; More... for the rest, text, and the pattern as text).
// True when the pattern changed.
bool namePieces(App& a, const char* id, char* buf, size_t cap, const char* tokens, const char* offer, float w) {
  using namespace exportchoice;
  const auto& P = pal();
  bool changed = false;
  ImGui::PushID(id);
  const std::vector<NamePiece> pieces = parseNamePattern(buf, tokens);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fh = ImGui::GetFrameHeight(), chipH = 18.0f, padX = theme::kS2;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  // Lay the pieces out, wrapping to more lines when they do not fit (never cut off).
  struct Placed {
    ImVec2 a, b;
    size_t index;
    std::string text;
    bool token;
  };
  std::vector<Placed> placed;
  float x = p0.x + padX, y = p0.y + (fh - chipH) * 0.5f, lineH = fh;
  const float right = p0.x + w - padX;
  int lines = 1;
  for (size_t i = 0; i < pieces.size(); ++i) {
    const NamePiece& np = pieces[i];
    const std::string t = np.token ? i18n::tr(tokenLabel(np.token)) : np.text;
    const float tw = ImGui::CalcTextSize(t.c_str()).x + (np.token ? 16.0f : 0.0f);
    if (x + tw > right && x > p0.x + padX) {
      x = p0.x + padX;
      y += lineH - 4;
      ++lines;
    }
    placed.push_back({ImVec2(x, y), ImVec2(x + tw, y + chipH), i, t, np.token != 0});
    x += tw + (np.token ? 4.0f : 0.0f);
  }
  const float h = fh + static_cast<float>(lines - 1) * (lineH - 4);
  const ImVec2 p1(p0.x + w, p0.y + h);
  dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), theme::kRadius);
  dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), theme::kRadius);
  int editText = -1, removeAt = -1;
  for (const Placed& pl : placed) {
    ImGui::SetCursorScreenPos(pl.a);
    ImGui::PushID(static_cast<int>(pl.index));
    ImGui::InvisibleButton("##piece", ImVec2(std::max(4.0f, pl.b.x - pl.a.x), chipH));
    const bool hov = ImGui::IsItemHovered();
    if (pl.token) {
      dl->AddRectFilled(pl.a, pl.b, theme::mix(P.field, P.hot, P.dark ? 0.16f : 0.12f), chipH * 0.5f);
      dl->AddRect(pl.a, pl.b, theme::withAlpha(P.hot, hov ? 0.9f : 0.55f), chipH * 0.5f);
      const ImVec2 ts = ImGui::CalcTextSize(pl.text.c_str());
      dl->AddText(a.fontBold, theme::kFontCaption + 1, ImVec2(pl.a.x + 8, pl.a.y + (chipH - ts.y) * 0.5f), P.hot, pl.text.c_str());
      if (hov) ui::Tooltip(i18n::tr("Click to remove"));
      if (ImGui::IsItemClicked()) removeAt = static_cast<int>(pl.index);
    } else {
      const ImVec2 ts = ImGui::CalcTextSize(pl.text.c_str());
      dl->AddText(ImVec2(pl.a.x, pl.a.y + (chipH - ts.y) * 0.5f), P.text, pl.text.c_str());
      if (hov) ui::Tooltip(i18n::tr("Click to change the text"));
      if (ImGui::IsItemClicked()) editText = static_cast<int>(pl.index);
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(w, h));
  static std::string editBuf;
  static int editing = -1;
  static char textBuf[96];
  if (editText >= 0) {
    editing = editText;
    std::snprintf(textBuf, sizeof textBuf, "%s", pieces[static_cast<size_t>(editText)].text.c_str());
    ImGui::OpenPopup("##edittext");
  }
  if (removeAt >= 0) {
    std::snprintf(buf, cap, "%s", removePiece(buf, static_cast<size_t>(removeAt), tokens).c_str());
    changed = true;
  }
  // The pieces that can be added.
  ImGui::SetCursorScreenPos(ImVec2(p0.x, ImGui::GetCursorScreenPos().y));
  bool first = true;
  for (const char* o = offer; *o; ++o) {
    bool present = false;
    for (const NamePiece& np : pieces) present = present || np.token == *o;
    if (present) continue;
    if (!first) ImGui::SameLine(0, theme::kS1 + 2);
    first = false;
    const std::string l = std::string("+ ") + i18n::tr(tokenLabel(*o));
    if (ui::TextButton(l.c_str(), false, 0, nullptr)) {
      std::snprintf(buf, cap, "%s", appendToken(buf, *o, tokens).c_str());
      changed = true;
    }
  }
  if (!first) ImGui::SameLine(0, theme::kS1 + 2);
  if (ui::TextButton(i18n::tr("More..."), false, 0, nullptr)) ImGui::OpenPopup("##more");
  if (ImGui::BeginPopup("##more")) {
    for (const char* t = tokens; *t; ++t) {
      bool present = false;
      for (const NamePiece& np : pieces) present = present || np.token == *t;
      if (ImGui::MenuItem(i18n::tr(tokenLabel(*t)), nullptr, false, !present)) {
        std::snprintf(buf, cap, "%s", appendToken(buf, *t, tokens).c_str());
        changed = true;
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Text..."))) {
      editing = -2;  // new text
      textBuf[0] = 0;
      ImGui::CloseCurrentPopup();
      editText = -2;
    }
    if (ImGui::MenuItem(i18n::tr("Edit the whole name as text..."))) {
      editing = -3;
      std::snprintf(textBuf, sizeof textBuf, "%s", buf);
      editText = -3;
    }
    ImGui::EndPopup();
  }
  if (editText <= -2) ImGui::OpenPopup("##edittext");
  if (ImGui::BeginPopup("##edittext")) {
    const char* what = editing == -3 ? i18n::tr("The name as text: %P project, %M marker, %T track, %N number, %D date, %R sample rate")
                                     : i18n::tr("Text in the name");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 280);
    ImGui::TextColored(theme::vec(P.textDim), "%s", what);
    ImGui::PopTextWrapPos();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(280);
    const bool enter = ImGui::InputText("##t", textBuf, sizeof textBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter || ui::TextButton(i18n::tr("Done"), false, 80)) {
      if (editing == -3) {
        std::snprintf(buf, cap, "%s", textBuf);
      } else if (editing == -2) {
        std::snprintf(buf, cap, "%s", appendText(buf, textBuf, tokens).c_str());
      } else if (editing >= 0) {
        std::vector<NamePiece> ps = parseNamePattern(buf, tokens);
        if (static_cast<size_t>(editing) < ps.size()) ps[static_cast<size_t>(editing)].text = textBuf;
        std::snprintf(buf, cap, "%s", joinNamePattern(ps).c_str());
      }
      changed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

// The folder on one line (home as ~, the middle shortened, the last folder whole; the whole path
// in its tooltip). Click it to type a path. Browse... and Next to the project beside it when they
// fit, else under it.
void folderRow(App& a, float w) {
  const auto& P = pal();
  static bool editing = false;
  const float bw = ui::TextButtonWidth(i18n::tr("Browse...")) + theme::kS2 + ui::TextButtonWidth(i18n::tr("Next to the project"));
  const bool beside = w - bw - theme::kS2 >= 200.0f;
  const float fw = beside ? w - bw - theme::kS2 : w;
  const float x0 = ImGui::GetCursorPosX();
  if (editing) {
    ImGui::SetNextItemWidth(fw);
    if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##folder", dlg.folder, sizeof dlg.folder);
    if (ImGui::IsItemDeactivated()) editing = false;
  } else {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float fh = ImGui::GetFrameHeight();
    if (ImGui::InvisibleButton("##folderfield", ImVec2(fw, fh))) editing = true;
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + fw, p.y + fh), ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), theme::kRadius);
    dl->AddRect(p, ImVec2(p.x + fw, p.y + fh), ImGui::GetColorU32(ImGuiCol_Border), theme::kRadius);
    const float room = fw - 2 * ImGui::GetStyle().FramePadding.x;
    const std::string shown = exportchoice::shortFolder(dlg.folder, homeDir(), [room](const std::string& t) { return ImGui::CalcTextSize(t.c_str()).x <= room; });
    const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    dl->PushClipRect(p, ImVec2(p.x + fw - 2, p.y + fh), true);
    dl->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + (fh - ts.y) * 0.5f), P.text, shown.c_str());
    dl->PopClipRect();
    if (hov) ui::Tooltip(dlg.folder);
  }
  if (beside) ImGui::SameLine(0, theme::kS2);
  else ImGui::SetCursorPosX(x0);
  if (ui::TextButton(i18n::tr("Browse..."), false, 0))
    fileDialog(a, "Render into folder", false, {"/"}, "", [](const std::string& d) { std::snprintf(dlg.folder, sizeof dlg.folder, "%s", d.c_str()); });
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::tr("Next to the project"), false, 0, i18n::tr("Put the files in the project's folder")))
    std::snprintf(dlg.folder, sizeof dlg.folder, "%s", projectFolder(a).c_str());
}

// Export marker areas: the arrangement's areas (a tick, the name in full, where it starts, how
// long it is), All / None and how many are ticked.
void drawAreaList(App& a) {
  const auto& P = pal();
  const Song& song = a.doc.song();
  const std::vector<MarkerArea> areas = markerAreas(song);
  devAreaRows.clear();
  devAreaFrame = ImGui::GetFrameCount();
  const float w = ImGui::GetContentRegionAvail().x;
  if (areas.empty()) {
    const char* say = livelook::active(a) ? i18n::N_("Add markers to the Arrangement: each marker starts an area that runs to the next one.")
                                          : logicplug::active(a) ? i18n::N_("Add markers: each marker starts an area that runs to the next one.")
                                                                 : i18n::N_("Add playlist markers: each marker starts an area that runs to the next one.");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr(say));
    ImGui::PopTextWrapPos();
    return;
  }
  const MeterMap mm(song);
  const float rowH = ImGui::GetFrameHeight();
  const float startW = std::max(48.0f, ImGui::CalcTextSize(i18n::tr("Start")).x + theme::kS2);
  const float lenW = std::max(60.0f, ImGui::CalcTextSize(i18n::tr("Length")).x + theme::kS2);
  constexpr int kRowsShown = 8;  // a long set list scrolls
  ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
  ImVec2 size(w, 0);
  if (static_cast<int>(areas.size()) > kRowsShown) {
    flags |= ImGuiTableFlags_ScrollY;
    size.y = (rowH + 2 * ImGui::GetStyle().CellPadding.y) * static_cast<float>(kRowsShown + 1) + 2;
  }
  ImGui::PushStyleColor(ImGuiCol_TableRowBg, theme::vec(theme::withAlpha(P.field, 0.0f)));
  ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, theme::vec(theme::withAlpha(P.field, P.dark ? 0.55f : 0.45f)));
  ImGui::PushStyleColor(ImGuiCol_TableBorderLight, theme::vec(theme::withAlpha(P.line, 0.6f)));
  if (ImGui::BeginTable("##areas", 4, flags, size)) {
    ImGui::TableSetupColumn("##on", ImGuiTableColumnFlags_WidthFixed, ui::toggleFields() ? std::max(rowH, GImGuiGridlineToggleW) : rowH);
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##start", ImGuiTableColumnFlags_WidthFixed, startW);
    ImGui::TableSetupColumn("##len", ImGuiTableColumnFlags_WidthFixed, lenW);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, rowH);
    const auto rightText = [](const char* s, ImU32 col) {
      const float cw = ImGui::GetContentRegionAvail().x, tw = ImGui::CalcTextSize(s).x;
      if (tw < cw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cw - tw);
      ImGui::TextColored(theme::vec(col), "%s", s);
    };
    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Name"));
    ImGui::TableSetColumnIndex(2);
    ImGui::AlignTextToFramePadding();
    rightText(i18n::tr("Start"), P.textDim);
    ImGui::TableSetColumnIndex(3);
    ImGui::AlignTextToFramePadding();
    rightText(i18n::tr("Length"), P.textDim);
    for (size_t i = 0; i < areas.size(); ++i) {
      const MarkerArea& m = areas[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow(0, rowH);
      ImGui::TableSetColumnIndex(0);
      bool on = areaTicked(m);
      if (ImGui::Checkbox("##on", &on)) dlg.areaOn[m.start] = on;
      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      const ImU32 nameCol = !m.hasContent ? P.textFaint : on ? P.text : P.textDim;
      ImGui::PushTextWrapPos(0.0f);  // the name wraps in its column, never cut off
      ImGui::TextColored(theme::vec(nameCol), "%s", m.name.c_str());
      ImGui::PopTextWrapPos();
      if (!m.hasContent && ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("Nothing plays in this area"));
      const std::string at = barBeat(mm, m.start), len = minSec(areaSeconds(song, m));
      ImGui::TableSetColumnIndex(2);
      ImGui::AlignTextToFramePadding();
      ImGui::PushFont(livelook::numFont(a), theme::kFontNumber + 1);
      rightText(at.c_str(), on ? P.text : P.textDim);
      ImGui::PopFont();
      ImGui::TableSetColumnIndex(3);
      ImGui::AlignTextToFramePadding();
      ImGui::PushFont(livelook::numFont(a), theme::kFontNumber + 1);
      rightText(len.c_str(), on ? P.text : P.textDim);
      ImGui::PopFont();
      ImGui::PopID();
      devAreaRows.push_back(m.name + " | " + at + " | " + len + " | " + (on ? "on" : "off") + (m.hasContent ? "" : " | empty"));
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleColor(3);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
  if (ui::TextButton(i18n::tr("All"), false, 0, i18n::tr("Tick every area")))
    for (const MarkerArea& m : areas) dlg.areaOn[m.start] = true;
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::tr("None"), false, 0, i18n::tr("Untick every area")))
    for (const MarkerArea& m : areas) dlg.areaOn[m.start] = false;
  double total = 0;
  int ticked = 0;
  for (const MarkerArea& m : areas)
    if (areaTicked(m)) total += areaSeconds(song, m), ++ticked;
  char b[96];
  std::snprintf(b, sizeof b, i18n::tr("%d of %d, %s"), ticked, static_cast<int>(areas.size()), minSec(total).c_str());
  ImGui::SameLine();
  const float tw = ImGui::CalcTextSize(b).x;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - tw));
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textFaint), "%s", b);
}

// The scope's own rows: from / to / length, or the selection and its markers, or the pattern.
void drawScopeDetails(App& a) {
  const auto& P = pal();
  const Song& song = a.doc.song();
  const auto info = [&](const char* label, const std::string& value, const char* note) {
    rowLabel(label);
    ImGui::PushFont(livelook::numFont(a), theme::kFontNumber + 1);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopFont();
    if (note && *note) {
      ImGui::SameLine(0, theme::kS2);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr(note));
    }
  };
  if (dlg.areaMode) {
    drawAreaList(a);
    return;
  }
  // K-406: with more than one arrangement, which one renders (or each into its own files). Its
  // From / To are the chosen arrangement's; for every arrangement only the length in all shows.
  const int nArr = arrangementCount(song);
  const bool multiArr = dlg.scope != 1 && nArr > 1;
  const int curArr = song.arrangements.empty() ? 0 : song.currentArrangement;
  if (multiArr) {
    const bool files = dlg.dest == static_cast<int>(Dest::Files);
    if (dlg.arrangement >= nArr || (dlg.arrangement == -2 && !files)) dlg.arrangement = -1;
    const std::string curLbl = std::string(i18n::tr("Current")) + " (" + arrangementName(song, curArr) + ")";
    const std::string allLbl = std::string(i18n::tr("All arrangements")) + " (" + std::to_string(nArr) + ")";
    const std::string pv = dlg.arrangement == -1 ? curLbl : dlg.arrangement == -2 ? allLbl : arrangementName(song, dlg.arrangement);
    const float cw = ImGui::GetContentRegionAvail().x - kRowX;
    rowLabel("Arrangement");
    if (ui::BeginCombo("##exportarr", pv.c_str(), cw)) {
      if (ImGui::Selectable(curLbl.c_str(), dlg.arrangement == -1)) dlg.arrangement = -1;
      ImGui::Separator();
      for (int i = 0; i < nArr; ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(arrangementName(song, i).c_str(), dlg.arrangement == i)) dlg.arrangement = i;
        ImGui::PopID();
      }
      if (files) {
        ImGui::Separator();
        if (ImGui::Selectable(allLbl.c_str(), dlg.arrangement == -2)) dlg.arrangement = -2;
      }
      ImGui::EndCombo();
    }
    ui::Tooltip("Choose which arrangement to render, or render every arrangement into separate files");
  }
  RenderJob r;
  std::string why;
  const bool ok = applyScope(a, r, &why);
  Song chosen;
  const Song& scoped = multiArr && dlg.arrangement >= 0 && dlg.arrangement != curArr ? (chosen = songWithArrangement(song, dlg.arrangement)) : song;
  const MeterMap smm(scoped);
  Tick A = 0, B = 0;
  const bool ticks = ok && !(multiArr && dlg.arrangement == -2) && renderScopeTicks(scoped, r, A, B);
  if (dlg.scope == 2 || dlg.scope == 3) {
    int kind = dlg.scope == 3 ? 1 : 0;
    const char* kinds[] = {i18n::N_("Selection"), i18n::N_("Between markers")};
    rowLabel("Range");
    const char* tk[] = {logicplug::active(a) ? i18n::tr("Cycle area") : i18n::tr(kinds[0]), i18n::tr(kinds[1])};
    if (ui::Segmented("rangekind", tk, 2, &kind)) dlg.scope = kind == 1 ? 3 : 2;
    if (dlg.scope == 3) {
      std::vector<std::string> names;
      for (size_t k = 0; k < song.markers.size(); ++k)
        names.push_back(song.markers[k].name.empty() ? "Marker " + std::to_string(k + 1) : song.markers[k].name);
      std::vector<const char*> items;
      for (const auto& n : names) items.push_back(n.c_str());
      if (items.size() < 2) {
        rowLabel("");
        ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr(livelook::active(a) || logicplug::active(a) ? "Add two markers first" : "Add two playlist markers first"));
      } else {
        const float cw = ImGui::GetContentRegionAvail().x - kRowX;
        rowLabel("From marker");
        combo("##ma", &dlg.markerA, items.data(), static_cast<int>(items.size()), cw);
        rowLabel("To marker");
        combo("##mb", &dlg.markerB, items.data(), static_cast<int>(items.size()), cw);
      }
    }
  }
  if (dlg.scope == 1) {
    const Pattern* p = song.pattern(a.curPattern);
    rowLabel(livelook::active(a) ? "Clip" : logicplug::active(a) ? "Region" : "Pattern");
    ImGui::TextUnformatted(p ? p->name.c_str() : i18n::tr("(none)"));
  }
  if (ticks && dlg.scope != 1) {
    info("From", barBeat(smm, A), dlg.scope == 0 ? i18n::N_("the start of the song") : nullptr);
    info("To", barBeat(smm, B), dlg.scope == 0 ? i18n::N_("the end of the last clip") : nullptr);
  }
  const double sec = scopeSeconds(a);
  info("Length", minSec(sec), nullptr);
  if (!ok && !why.empty()) {
    rowLabel("");
    ImGui::PushTextWrapPos(0.0f);
    std::string say = why;
    if (!say.empty()) say[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(say[0])));
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr(say.c_str()));
    ImGui::PopTextWrapPos();
  } else if (livelook::active(a) && dlg.scope == 0 && song.live.areaEnd > 0 && !gSessionExport) {
    rowLabel("");
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("The Arrangement only, not the Session's clips"));
  }
}

// A choice of several on a row: a round box the size of the check boxes, a lit dot when chosen,
// centred on its text line. True when clicked.
bool choiceRow(const char* label, bool active) {
  const auto& P = pal();
  const float fh = ImGui::GetFrameHeight();
  const float box = std::min(fh, std::floor(ImGui::GetFontSize() * 1.15f));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float tw = ImGui::CalcTextSize(label).x;
  const bool clicked = ImGui::InvisibleButton(label, ImVec2(box + ImGui::GetStyle().ItemInnerSpacing.x + tw, fh));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p.x + box * 0.5f, p.y + fh * 0.5f);
  const float r = box * 0.5f;
  dl->AddCircleFilled(c, r, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : active ? ImGuiCol_CheckboxSelectedBg : ImGuiCol_FrameBg), 24);
  dl->AddCircle(c, r - 0.5f, ImGui::GetColorU32(ImGuiCol_Border), 24, 1.0f);
  if (active) dl->AddCircleFilled(c, std::max(2.0f, std::round(r * 0.42f)), P.hot, 16);
  dl->AddText(ImVec2(p.x + box + ImGui::GetStyle().ItemInnerSpacing.x, p.y + (fh - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
  return clicked;
}

// How the file ends: cut, ring out (up to N s), a fixed tail of N s, or folded into the start.
void drawEnding(App& a) {
  const auto& P = pal();
  ExportSettings& s = dlg.s;
  ImGui::PushFont(a.fontBold, theme::kFontBody);
  // Smooth draws its section headings at the panel's edge, a pad left of the rows: match them.
  if (logicplug::active(a)) ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 8.0f);
  ImGui::TextUnformatted(i18n::tr("Ending"));
  ImGui::PopFont();
  int tail = static_cast<int>(s.render.tail);
  const float indent = std::min(ImGui::GetFrameHeight(), std::floor(ImGui::GetFontSize() * 1.15f)) + ImGui::GetStyle().ItemInnerSpacing.x;
  if (choiceRow(i18n::tr("Cut exactly at the end"), tail == static_cast<int>(TailMode::Cut))) tail = static_cast<int>(TailMode::Cut);
  if (choiceRow(i18n::tr("Let sounds ring out, up to"), tail == static_cast<int>(TailMode::Leave))) tail = static_cast<int>(TailMode::Leave);
  ImGui::SameLine(0, theme::kS2);
  ImGui::SetNextItemWidth(64);
  float cap = static_cast<float>(s.render.maxTailSeconds);
  if (ImGui::DragFloat("##ringcap", &cap, 0.1f, 1.0f, 60.0f, "%.0f s", ImGuiSliderFlags_AlwaysClamp)) s.render.maxTailSeconds = cap, tail = static_cast<int>(TailMode::Leave);
  ui::Tooltip(i18n::tr("The longest the sound may ring on; it stops sooner once it goes quiet"));
  if (tail == static_cast<int>(TailMode::Leave)) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", dlg.areaMode ? i18n::tr("Stops once quiet; never the next area's notes") : i18n::tr("Stops once it goes quiet"));
  }
  if (choiceRow(i18n::tr("Add a fixed tail of"), tail == static_cast<int>(TailMode::Fixed))) tail = static_cast<int>(TailMode::Fixed);
  ImGui::SameLine(0, theme::kS2);
  ImGui::SetNextItemWidth(64);
  float fx = static_cast<float>(s.render.fixedTailSeconds);
  if (ImGui::DragFloat("##fixedtail", &fx, 0.05f, 0.0f, 60.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp)) s.render.fixedTailSeconds = fx, tail = static_cast<int>(TailMode::Fixed);
  ui::Tooltip(i18n::tr("Exactly this much after the end, loud or quiet"));
  if (choiceRow(i18n::tr("Fold the ring-out into the start (loops)"), tail == static_cast<int>(TailMode::Wrap))) tail = static_cast<int>(TailMode::Wrap);
  ui::Tooltip(i18n::tr("Sound still ringing at the end is mixed into the start, so the file loops without a gap"));
  s.render.tail = static_cast<TailMode>(tail);
}

// What the footer says: how many files, how long, which formats.
std::string summaryLine(App& a, int* filesOut = nullptr) {
  const ExportSettings& s = dlg.s;
  const Dest dest = static_cast<Dest>(dlg.dest);
  double sec = 0;
  int targets = 1;
  if (dlg.areaMode) {
    targets = 0;
    for (const MarkerArea& m : markerAreas(a.doc.song()))
      if (areaTicked(m)) sec += areaSeconds(a.doc.song(), m), ++targets;
  } else {
    sec = scopeSeconds(a) * std::max(1, s.render.loops);
  }
  std::string out;
  if (dest != Dest::Files) {
    out = dest == Dest::AudioClip ? (logicplug::active(a) ? i18n::tr("One audio region") : i18n::tr("One audio clip")) : i18n::tr("Opens in the audio editor");
    out += " \xC2\xB7 " + minSec(sec) + " \xC2\xB7 " + i18n::tr("32-bit float WAV");
    if (filesOut) *filesOut = 1;
    return out;
  }
  std::vector<std::string> fmts;
  const char* bits[] = {"16-bit", "24-bit", "32-bit float"};
  const int nf = static_cast<int>(exportchoice::tickedFormats(dlg.fmt, static_cast<int>(ExportFormat::Count)).size());
  for (ExportFormat f : exportchoice::tickedFormats(dlg.fmt, static_cast<int>(ExportFormat::Count))) {
    std::string t = exportFormatName(f);
    if (f == ExportFormat::Wav) t += std::string(" ") + bits[std::clamp(dlg.wavBits, 0, 2)];
    else if (f == ExportFormat::Flac) t += std::string(" ") + bits[std::clamp(dlg.flacBits, 0, 1)];
    else {
      const int idx = f == ExportFormat::Mp3 ? dlg.mp3Rate : f == ExportFormat::Ogg ? dlg.oggRate : f == ExportFormat::M4a ? dlg.m4aRate : dlg.opusRate;
      t += " " + std::to_string(kBitrates[std::clamp(idx, 0, 3)]) + " kbps";
    }
    fmts.push_back(t);
  }
  if (s.midi) fmts.push_back("MIDI");
  const bool split = s.splitMixer || s.splitPlaylist || s.splitPlaylistGroups;
  const int files = targets * (nf + (s.midi ? 1 : 0));
  char b[160];
  if (split) {
    std::snprintf(b, sizeof b, dlg.areaMode ? i18n::tr("%d areas, each with its parts") : i18n::tr("A file per part"), targets);
    out = b;
  } else {
    std::snprintf(b, sizeof b, files == 1 ? i18n::tr("%d file") : i18n::tr("%d files"), files);
    out = b;
  }
  if (filesOut) *filesOut = split ? -1 : files;
  out += " \xC2\xB7 " + minSec(sec) + (dlg.areaMode ? std::string(" ") + i18n::tr("in all") : std::string());
  for (size_t i = 0; i < fmts.size(); ++i) out += (i ? ", " : " \xC2\xB7 ") + fmts[i];
  char r[32];
  std::snprintf(r, sizeof r, " \xC2\xB7 %g kHz", kRates[dlg.rateIdx] / 1000.0);
  out += r;
  return out;
}
std::string devSummary;  // the footer's line, for dev dumps

// The example under the names: the first file as it will be written, and how many more.
std::string exampleName(App& a, std::string* more) {
  const ExportSettings& s = dlg.s;
  const std::string rate = std::to_string(kRates[dlg.rateIdx]);
  const std::string base = expandNamePattern(dlg.filePattern, {{'P', projectName(a)}, {'R', rate}, {'S', dlg.scope == 1 ? "Pattern" : dlg.scope == 0 ? "Song" : "Range"}});
  const ExportFormat f0 = [&] {
    const auto v = exportchoice::tickedFormats(dlg.fmt, static_cast<int>(ExportFormat::Count));
    return v.empty() ? ExportFormat::Wav : v[0];
  }();
  const std::string ext = exportFormatExt(f0);
  const bool split = s.splitMixer || s.splitPlaylist || s.splitPlaylistGroups;
  std::string part;
  if (split) {
    std::string track = "Kick";
    const Song& song = a.doc.song();
    if (s.splitMixer) {
      for (const auto& m : song.mixer)
        if (m.id != song.masterId() && !m.name.empty()) {
          track = m.name;
          break;
        }
    }
    part = expandNamePattern(dlg.pattern, {{'P', projectName(a)}, {'N', "01"}, {'T', track}, {'S', "Song"}, {'R', rate}});
  }
  if (dlg.areaMode) {
    std::vector<MarkerArea> ticked;
    for (const MarkerArea& m : markerAreas(a.doc.song()))
      if (areaTicked(m)) ticked.push_back(m);
    if (ticked.empty()) return {};
    const std::vector<std::string> names = markerAreaFileNames(ticked, dlg.areaPattern, projectName(a), kRates[dlg.rateIdx]);
    if (more) {
      char b[96];
      if (split) std::snprintf(b, sizeof b, "%s", i18n::tr("a folder per area, its mix and its parts"));
      else if (names.size() > 1) std::snprintf(b, sizeof b, i18n::tr("and %d more, one per area"), static_cast<int>(names.size() - 1));
      else b[0] = 0;
      *more = b;
    }
    return split ? names[0] + "/" + part + ext : names[0] + ext;
  }
  if (more) *more = !split ? std::string() : s.stemsIncludeMaster ? std::string(i18n::tr("and the other parts, plus the full mix")) : std::string(i18n::tr("and the other parts"));
  return split ? part + ext : base + ext;
}

void drawRenderDialog(App& a) {
  if (!dlg.open) return;
  initDialog(a);
  const auto& P = pal();
  const Song& song = a.doc.song();
  centerNext(ImVec2(0, 0));
  // Checkpoint 59 (N58-11): the auto-sized dialog takes a few frames to measure its columns; it stays
  // invisible until its size and its body's hold still (at most 12 frames), so it appears whole.
  constexpr int kShown = 12;
  static int settle = 0, still = 0, shownFrame = -2;
  static ImVec2 lastSize(0, 0), bodySize(0, 0), lastBody(0, 0);
  if (shownFrame != ImGui::GetFrameCount() - 1) {  // just opened
    settle = 0, still = 0;
    lastSize = bodySize = lastBody = ImVec2(0, 0);
    colFit = ColumnFit{};
  }
  shownFrame = ImGui::GetFrameCount();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS4));
  if (dlg.areaMode) dlg.dest = static_cast<int>(Dest::Files);  // areas go to files only
  bool vis = ImGui::Begin(i18n::tr("Export audio###render"), &dlg.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  {
    const ImVec2 sz = ImGui::GetWindowSize();
    const bool same = sz.x == lastSize.x && sz.y == lastSize.y && bodySize.x == lastBody.x && bodySize.y == lastBody.y;
    still = same && sz.x > 0 && bodySize.x > 0 ? still + 1 : 0;
    settle = settle >= 4 && still >= 2 ? kShown : settle + 1;
    lastSize = sz, lastBody = bodySize;
    if (ImGuiWindow* w = ImGui::GetCurrentWindow(); settle < kShown && w->HiddenFramesCannotSkipItems < 2) w->HiddenFramesCannotSkipItems = 2;
  }
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ExportSettings& s = dlg.s;
  const bool files = dlg.dest == static_cast<int>(Dest::Files);
  ui::SetFormColumn(logicplug::active(a) ? kRowX : 0.0f, false);  // Smooth: labels end at the controls, groups under bold headings
  ui::SetSectionBoldCaptions(true);
  ui::BeginDialogBody("##renderbody");
  bodySize = ImGui::GetWindowSize();
  // Three columns when the screen has room; two (format and quality under split and files) when it
  // is medium; one when narrow. Only the height scrolls, so the footer's buttons always show.
  const float room = std::min(ui::DialogArea().x, ui::DialogFitSize().x) - 2 * theme::kS4 - ImGui::GetStyle().ScrollbarSize;
  const int nc = room >= 3 * kColW + 2 * theme::kS4 ? 3 : room >= 2 * kColW + theme::kS4 ? 2 : 1;
  const float colW = nc == 1 ? std::clamp(std::floor(room), kColW - 60, kColW + 160) : kColW;
  const float fullW = nc * colW + static_cast<float>(nc - 1) * theme::kS4;
  if (nc != colFit.cols) colFit = ColumnFit{}, colFit.cols = nc;
  const float bodyX = ImGui::GetCursorScreenPos().x;  // the footer's line starts here too

  // ---- What to export
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float labelW = 96.0f;
    const bool labelAbove = nc == 1;   // narrow: the label sits over the cards
    const int perRow = nc == 3 ? 4 : 2;  // the cards wrap to two rows of two below three columns
    ImGui::PushTextWrapPos(labelAbove ? 0.0f : ImGui::GetCursorPosX() + labelW - theme::kS1);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 4));
    ImGui::PushFont(a.fontBold, theme::kFontBody);
    ImGui::TextUnformatted(i18n::tr("What to export"));
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    const float cardX = labelAbove ? p.x : p.x + labelW + theme::kS3;
    if (labelAbove) ImGui::SetCursorScreenPos(ImVec2(cardX, ImGui::GetCursorScreenPos().y + theme::kS1));
    else ImGui::SetCursorScreenPos(ImVec2(cardX, p.y));
    const float cw = std::floor((fullW - (cardX - p.x) - static_cast<float>(perRow - 1) * theme::kS2) / static_cast<float>(perRow));
    const MeterMap mm(song);
    // Each card says what it holds.
    std::string songSub, selSub, areaSub, patSub;
    {
      RenderJob r;
      r.scope = RenderScope::Song;
      r.arrangementOnly = livelook::active(a);
      Tick A = 0, B = 0;
      if (song.lengthTicks() > 0 && renderScopeTicks(song, r, A, B))
        songSub = barBeat(mm, A) + " " + i18n::tr("to") + " " + barBeat(mm, B) + " \xC2\xB7 " + minSec(scopeSecondsOf(a, r));
      else
        songSub = i18n::tr("Nothing to play yet");
      const PlaylistState& pst = a.playlist;
      selSub = pst.selEnd > pst.selStart ? barBeat(mm, pst.selStart) + " " + i18n::tr("to") + " " + barBeat(mm, pst.selEnd)
                                         : (logicplug::active(a) ? i18n::tr("Turn Cycle on first") : i18n::tr("None selected yet"));
      if (dlg.scope == 3) selSub = i18n::tr("Between two markers");
      const std::vector<MarkerArea> areas = markerAreas(song);
      char b[64];
      std::snprintf(b, sizeof b, areas.size() == 1 ? i18n::tr("%d area") : i18n::tr("%d areas"), static_cast<int>(areas.size()));
      double total = 0;
      for (const MarkerArea& m : areas) total += areaSeconds(song, m);
      areaSub = areas.empty() ? std::string(i18n::tr("No markers yet")) : std::string(b) + " \xC2\xB7 " + minSec(total);
      const Pattern* pt = song.pattern(a.curPattern);
      patSub = pt ? pt->name : std::string(i18n::tr("None selected"));
    }
    const int cur = dlg.areaMode ? 2 : dlg.scope == 0 ? 0 : dlg.scope == 1 ? 3 : 1;
    const char* titles[] = {songWord(a), selectionWord(a), i18n::N_("Marker areas"), patternWord(a)};
    const std::string subs[] = {songSub, selSub, areaSub, patSub};
    for (int k = 0; k < 4; ++k) {
      if (k % perRow) ImGui::SameLine(0, theme::kS2);
      else if (k) ImGui::SetCursorScreenPos(ImVec2(cardX, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y + theme::kS2));
      ImGui::PushID(k);
      if (scopeCard(a, "##card", k, titles[k], subs[k], cur == k, cw)) {
        dlg.areaMode = k == 2;
        if (k == 0) dlg.scope = 0;
        if (k == 1) dlg.scope = dlg.scope == 3 ? 3 : 2;
        if (k == 3) dlg.scope = 1;
        if (k == 2) dlg.dest = static_cast<int>(Dest::Files);
      }
      ImGui::PopID();
    }
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));

  float colTop = ImGui::GetCursorScreenPos().y;
  float colH[3] = {0, 0, 0};
  // ---- left: what, its ending, where it goes
  ImGui::BeginGroup();
  ui::BeginSection(dlg.areaMode ? "Marker areas" : dlg.scope == 1 ? patternWord(a) : dlg.scope == 0 ? songWord(a) : selectionWord(a), colW);
  drawScopeDetails(a);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  drawEnding(a);
  rowLabel("Play it");
  ImGui::SetNextItemWidth(64);
  if (livelook::active(a)) ImGui::DragInt("##loops", &s.render.loops, 0.1f, 1, 64);
  else ImGui::InputInt("##loops", &s.render.loops, 0, 0);
  s.render.loops = std::clamp(s.render.loops, 1, 64);
  ImGui::SameLine(0, theme::kS2);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", s.render.loops == 1 ? (dlg.areaMode ? i18n::tr("time (each area)") : i18n::tr("time"))
                                                                         : (dlg.areaMode ? i18n::tr("times (each area)") : i18n::tr("times")));
  {
    // Marker areas go to files only: the row stays (dimmed, Files lit) so the column keeps its shape.
    rowLabel("Render to");
    const char* dests[] = {i18n::tr("Files"), logicplug::active(a) ? i18n::tr("Audio region") : i18n::tr("Audio clip"), logicplug::active(a) ? i18n::tr("Audio editor") : "Loupe"};
    ImGui::BeginDisabled(dlg.areaMode);
    ui::Segmented("dest", dests, 3, &dlg.dest);
    ImGui::EndDisabled();
    if (dlg.areaMode && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", i18n::tr("Marker areas are written as files"));
    if (!dlg.areaMode && dlg.dest == 1 && dlg.scope == 1) {
      rowLabel("");
      ImGui::Checkbox(i18n::tr("Replace the pattern's clips"), &dlg.replacePattern);
    }
  }
  if (!song.mixer.empty() && song.mixer[0].muted) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("The master track is muted: the render will be silent"));
    ImGui::PopTextWrapPos();
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));  // room above the panel's soft bottom edge
  columnPad(0);
  ui::EndSection();
  ImGui::EndGroup();
  colH[0] = ImGui::GetItemRectSize().y;

  // ---- middle: split, files and names
  if (nc > 1) ImGui::SameLine(0, theme::kS4);
  ImGui::BeginGroup();
  const bool tracksWords = logicplug::active(a) || livelook::active(a);
  ui::BeginSection("Split", colW);
  {
    ImGui::BeginDisabled(!files);
    int sep = s.splitMixer || s.splitPlaylist || s.splitPlaylistGroups ? 1 : 0;
    const char* modes[] = {i18n::tr("One file, the full mix"), i18n::tr("Separate files")};
    if (ui::Segmented("splitmode", modes, 2, &sep)) {
      if (sep == 0) {
        dlg.lastSplit[0] = s.splitMixer, dlg.lastSplit[1] = s.splitPlaylist, dlg.lastSplit[2] = s.splitPlaylistGroups;
        s.splitMixer = s.splitPlaylist = s.splitPlaylistGroups = false;
      } else {
        s.splitMixer = dlg.lastSplit[0], s.splitPlaylist = dlg.lastSplit[1], s.splitPlaylistGroups = dlg.lastSplit[2];
        if (!s.splitMixer && !s.splitPlaylist && !s.splitPlaylistGroups) s.splitMixer = true;
      }
    }
    ImGui::BeginDisabled(sep == 0);
    ui::ToggleRow(logicplug::active(a) ? i18n::tr("A file per channel strip (stems)") : i18n::tr("A file per mixer track (stems)"), &s.splitMixer);
    if (s.splitMixer && sep) {
      ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
      ui::ToggleRow(i18n::tr("Through master effects"), &s.stemsMasterFx);
      ui::Tooltip(i18n::tr("Each part goes through the master track's effects, as it sounds in the mix"));
      ui::ToggleRow(i18n::tr("Plus the full mix"), &s.stemsIncludeMaster);
      ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    }
    ui::ToggleRow(tracksWords ? i18n::tr("A file per track") : i18n::tr("A file per playlist track"), &s.splitPlaylist);
    ui::ToggleRow(tracksWords ? i18n::tr("A file per track group") : i18n::tr("A file per playlist track group"), &s.splitPlaylistGroups);
    ImGui::EndDisabled();
    if (sep && !s.splitMixer && !s.splitPlaylist && !s.splitPlaylistGroups) s.splitMixer = true;  // Separate files needs one kind
    ImGui::EndDisabled();
    if (!files) {
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Splitting writes files only"));
    }
  }
  ui::EndSection();
  ui::BeginSection("Files and names", colW);
  if (!dlg.folder[0]) std::snprintf(dlg.folder, sizeof dlg.folder, "%s", projectFolder(a).c_str());
  if (files) {
    const float fw = ImGui::GetContentRegionAvail().x - kRowX;
    rowLabel("Folder");
    ImGui::BeginGroup();
    folderRow(a, fw);
    ImGui::EndGroup();
    rowLabel(dlg.areaMode ? "File names" : "File name");
    ImGui::BeginGroup();
    if (dlg.areaMode) namePieces(a, "areaname", dlg.areaPattern, sizeof dlg.areaPattern, exportchoice::kAreaTokens, "MN", fw);
    else namePieces(a, "filename", dlg.filePattern, sizeof dlg.filePattern, exportchoice::kFileTokens, "D", fw);
    ImGui::EndGroup();
    if (s.splitMixer || s.splitPlaylist || s.splitPlaylistGroups) {
      rowLabel("Each part");
      ImGui::BeginGroup();
      namePieces(a, "partname", dlg.pattern, sizeof dlg.pattern, exportchoice::kPartTokens, "NT", fw);
      ImGui::EndGroup();
    }
    std::string more;
    const std::string ex = exampleName(a, &more);
    if (!ex.empty()) {
      rowLabel("Example");
      ImGui::BeginGroup();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->ChannelsSplit(2);
      dl->ChannelsSetCurrent(1);
      ImGui::SetCursorScreenPos(ImVec2(p.x + theme::kS2, p.y + 5));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fw - 2 * theme::kS2);
      ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
      ImGui::TextUnformatted(ex.c_str());
      if (!more.empty()) ImGui::TextColored(theme::vec(P.textDim), "%s", more.c_str());
      ImGui::PopFont();
      ImGui::PopTextWrapPos();
      const float y1 = ImGui::GetItemRectMax().y + 5;
      dl->ChannelsSetCurrent(0);
      dl->AddRectFilled(p, ImVec2(p.x + fw, y1), ImGui::GetColorU32(ImGuiCol_FrameBg), theme::kRadius);
      dl->ChannelsMerge();
      ImGui::SetCursorScreenPos(ImVec2(p.x, y1));
      ImGui::Dummy(ImVec2(fw, 0));
      ImGui::EndGroup();
    }
  } else {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("A 32-bit float WAV in %s"), renderedFolder(a).c_str());
    ImGui::PopTextWrapPos();
  }
  if (nc == 3) ImGui::Dummy(ImVec2(0, theme::kS1)), columnPad(1);
  ui::EndSection();
  if (nc != 2) {  // two columns: format and quality carry on under files and names
    ImGui::EndGroup();
    colH[1] = ImGui::GetItemRectSize().y;
    if (nc == 3) ImGui::SameLine(0, theme::kS4);
    ImGui::BeginGroup();
  }

  // ---- right: format, quality and processing
  ui::BeginSection("Format", colW);
  {
    ImGui::BeginDisabled(!files);
    const char* names[] = {"WAV", "FLAC", "MP3", "Ogg", "AAC", "Opus"};
    const char* hints[] = {i18n::N_("Uncompressed"), i18n::N_("Lossless, smaller than WAV"), i18n::N_("Compressed (needs an encoder program)"),
                           i18n::N_("Compressed (needs an encoder program)"), i18n::N_("Compressed, .m4a (needs an encoder program)"),
                           i18n::N_("Compressed (needs an encoder program)")};
    for (int k = 0; k < 6; ++k) {
      if (k) ImGui::SameLine(0, theme::kS1 + 2);
      ImGui::PushID(k);
      if (ui::TextButton(names[k], dlg.fmt[k], 0, i18n::tr(hints[k]))) dlg.fmt[k] = !dlg.fmt[k];
      ImGui::PopID();
    }
    const char* wb[] = {"16-bit", "24-bit", "32-bit float"};
    const char* br[] = {"128 kbps", "192 kbps", "256 kbps", "320 kbps"};
    for (int k = 0; k < 6; ++k) {
      if (!dlg.fmt[k]) continue;
      ImGui::PushID(100 + k);
      rowLabel(names[k]);
      const bool ext = exportFormatExternal(static_cast<ExportFormat>(k));
      std::string note;
      bool have = true;
      if (ext) {
        const EncoderLook& enc = encoderLook(a, static_cast<ExportFormat>(k));
        have = !enc.name.empty();
        note = have ? std::string(i18n::tr("via")) + " " + enc.name : std::string(i18n::tr("no encoder"));
      }
      const float nw = note.empty() ? 0.0f : ImGui::CalcTextSize(note.c_str()).x + theme::kS2;
      const float cw = ImGui::GetContentRegionAvail().x - nw;
      int* idx = k == 0 ? &dlg.wavBits : k == 1 ? &dlg.flacBits : k == 2 ? &dlg.mp3Rate : k == 3 ? &dlg.oggRate : k == 4 ? &dlg.m4aRate : &dlg.opusRate;
      combo("##fmt", idx, k <= 1 ? wb : br, k == 0 ? 3 : k == 1 ? 2 : 4, cw);
      if (!note.empty()) {
        ImGui::SameLine(0, theme::kS2);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(have ? P.textFaint : P.danger), "%s", note.c_str());
        if (ImGui::IsItemHovered()) ui::Tooltip(encoderLook(a, static_cast<ExportFormat>(k)).status.c_str());
      }
      ImGui::PopID();
    }
    rowLabel("In WAV files");
    ImGui::BeginDisabled(!dlg.fmt[0]);
    ImGui::Checkbox(i18n::tr("Tempo"), &s.tempoInfo);
    ui::Tooltip(i18n::tr("The tempo and beats, so samplers and other programs play it in time"));
    ImGui::SameLine(0, theme::kS4);
    ImGui::Checkbox(i18n::tr("Markers"), &s.markers);
    ui::Tooltip(tracksWords ? i18n::tr("The markers as cue points") : i18n::tr("The playlist markers as cue points"));
    ImGui::EndDisabled();
    rowLabel("Channels");
    int cm = static_cast<int>(s.channels);
    const char* cms[] = {i18n::N_("Stereo"), i18n::N_("Mono (both sides mixed)"), i18n::N_("Mono (left only)"), i18n::N_("Mono (right only)")};
    if (combo("##chmode", &cm, cms, 4, ImGui::GetContentRegionAvail().x)) s.channels = static_cast<ChannelMode>(cm);
    rowLabel("");
    ImGui::Checkbox(i18n::tr("Also a MIDI file of the notes"), &s.midi);
    ImGui::EndDisabled();
  }
  ui::EndSection();
  ui::BeginSection("Quality and processing", colW);
  {
    const float cw = ImGui::GetContentRegionAvail().x - kRowX;
    rowLabel("Sample rate");
    const char* rates[] = {"22.05 kHz", "32 kHz", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "192 kHz"};
    combo("##rate", &dlg.rateIdx, rates, 7, cw);
    rowLabel("Dither");
    int d = static_cast<int>(s.dither);
    const char* ds[] = {i18n::N_("Off"), i18n::N_("Triangular"), i18n::N_("Noise-shaped")};
    if (combo("##dither", &d, ds, 3, cw)) s.dither = static_cast<DitherMode>(d);
    ui::Tooltip(i18n::tr("Applied when writing 16-bit and 24-bit files"));
    rowLabel("Resampling");
    int q = static_cast<int>(s.render.interp);
    const char* qs[] = {i18n::N_("Linear, as played"), i18n::N_("Hermite (4-point)"), "Sinc 16-point", "Sinc 32-point", "Sinc 64-point", "Sinc 128-point", "Sinc 256-point", "Sinc 512-point"};
    if (combo("##interp", &q, qs, 8, cw)) s.render.interp = static_cast<RenderInterp>(q);
    ui::Tooltip(i18n::tr("How sampler channels play their samples at other pitches"));
    rowLabel("Oversample");
    int os = s.render.oversample >= 4 ? 2 : s.render.oversample >= 2 ? 1 : 0;
    const char* oss[] = {i18n::tr("Off"), "2x", "4x"};
    if (ui::Segmented("os", oss, 3, &os)) s.render.oversample = os == 2 ? 4 : os == 1 ? 2 : 1;
    ui::Tooltip(i18n::tr("Renders at a multiple of the sample rate and filters down (less aliasing from synths and distortion)"));
    ImGui::Dummy(ImVec2(0, 2));
    const float half = std::floor(ImGui::GetContentRegionAvail().x * 0.5f);
    if (ui::toggleFields()) {
      ui::ToggleRow(i18n::tr("Master effects"), &s.render.masterEffects);
      ui::ToggleRow(logicplug::active(a) ? i18n::tr("Channel strip effects") : livelook::active(a) ? i18n::tr("Track effects") : i18n::tr("Mixer track effects"),
                    &s.render.insertEffects);
    } else {
      const float x0 = ImGui::GetCursorPosX();
      ImGui::Checkbox(i18n::tr("Master effects"), &s.render.masterEffects);
      ImGui::SameLine(0, 0);
      ImGui::SetCursorPosX(x0 + half);
      ImGui::Checkbox(logicplug::active(a) ? i18n::tr("Channel strip effects") : livelook::active(a) ? i18n::tr("Track effects") : i18n::tr("Mixer track effects"),
                      &s.render.insertEffects);
    }
    bool noPoly = !s.render.polyphonyLimits;
    if (ui::ToggleRow(i18n::tr("Every note plays (ignore voice limits)"), &noPoly)) s.render.polyphonyLimits = !noPoly;
    ui::Tooltip(i18n::tr("Channels play every note, even past their maximum polyphony"));
    ui::ToggleRow(i18n::tr("Normalize"), &s.normalize);
    if (s.normalize) {
      if (!ui::toggleFields()) ImGui::SameLine(0, theme::kS2);
      else ImGui::SetCursorPosX(ImGui::GetCursorPosX());
      int kind = s.normalizeLoudness ? 1 : 0;
      const char* kinds[] = {i18n::tr("Peak"), i18n::tr("Loudness")};
      if (ui::Segmented("nkind", kinds, 2, &kind)) {
        s.normalizeLoudness = kind == 1;
        if (s.normalizeLoudness && s.normalizeDb > -0.1) s.normalizeDb = -1.0;  // leave headroom for the peak ceiling
      }
      ImGui::SameLine(0, theme::kS2);
      if (s.normalizeLoudness) {
        ImGui::SetNextItemWidth(84);
        float lufs = static_cast<float>(s.normalizeLufs);
        if (ImGui::DragFloat("##nlufs", &lufs, 0.1f, -36.0f, -5.0f, "%.1f LUFS")) s.normalizeLufs = lufs;
        ui::Tooltip(i18n::tr("Integrated loudness the file is scaled to (streaming services play around -14 LUFS)"));
        ImGui::SameLine(0, theme::kS2);
      }
      ImGui::SetNextItemWidth(s.normalizeLoudness ? 84 : 80);
      float db = static_cast<float>(s.normalizeDb);
      if (ImGui::DragFloat("##ndb", &db, 0.05f, -24.0f, 0.0f, s.normalizeLoudness ? "max %.1f dB" : "%.1f dB")) s.normalizeDb = db;
      ui::Tooltip(s.normalizeLoudness ? i18n::tr("Peak ceiling: the gain stops where the sample peak would pass it") : i18n::tr("The loudest sample's level"));
    }
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));  // room above the panel's soft bottom edge
  columnPad(nc - 1 > 0 ? nc - 1 : 2);
  ui::EndSection();
  ImGui::EndGroup();
  colH[nc == 2 ? 1 : 2] = ImGui::GetItemRectSize().y;
  (void)colTop;
  ImGui::Dummy(ImVec2(0, theme::kS1));  // room for the sections' bottom edges
  if (nc > 1) {  // the columns' bottoms line up from the next frame on
    float tallest = 0;
    for (int c = 0; c < nc; ++c) colFit.natural[c] = colH[c] - colFit.pad[c], tallest = std::max(tallest, colFit.natural[c]);
    for (int c = 0; c < nc; ++c) colFit.pad[c] = std::max(0.0f, tallest - colFit.natural[c]);
  }
  ui::EndDialogBody();
  ui::SetFormColumn(0);
  ui::SetSectionBoldCaptions(false);
  ui::SectionMarksReset();
  // ---- footer: what the render makes, Cancel, Render
  const std::string sum = summaryLine(a);
  devSummary = sum;
  const float bw = ui::ButtonRowWidth(2, 96);
  ui::DialogFooter(bw);
  {
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float x0 = bodyX;
    const float room = bp.x - theme::kS4 - x0;
    const ImVec2 ts = ImGui::CalcTextSize(sum.c_str(), nullptr, false, room);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x0, bp.y + std::max(0.0f, (ImGui::GetFrameHeight() - ts.y) * 0.5f)), P.textDim,
                                        sum.c_str(), nullptr, room);
  }
  if (ui::TextButton(i18n::tr("Cancel"), false, 96)) dlg.open = false;
  ImGui::SameLine(0, theme::kS2);
  if (ui::PrimaryButton(i18n::tr("Render"), 96) || (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsAnyItemActive()))
    startJob(a);
  ImGui::End();
}

}  // namespace

bool closeRender() {
  const bool was = dlg.open;
  dlg.open = false;
  return was;
}


bool progressBody(const std::string& status, double progress, bool escapeCancels, const std::string* stalled) {
  // 0.7.8: one size for the window's whole life. Every row is kProgressW wide: the status cut to
  // it, the percent right-aligned in a field as wide as "100%", and the row of a stalled job's
  // warning there from the start (empty while the job moves on).
  const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
  progui::Line(i18n::trMessage(status).c_str(), kProgressW, text);
  // The percent stands beside the bar: on the fill it could not be read near the end.
  const float done = static_cast<float>(std::clamp(progress, 0.0, 1.0));
  const float percentW = progui::PercentWidth();
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::vec(pal().accent));
  ImGui::ProgressBar(done, ImVec2(kProgressW - percentW - theme::kS2, 0), "");
  ImGui::PopStyleColor();
  ImGui::SameLine(0, theme::kS2);
  ImGui::AlignTextToFramePadding();
  progui::Percent(done, text);
  if (stalled) progui::Line(stalled->c_str(), kProgressW, pal().danger);
  ui::DialogFooter(96);
  return ui::TextButton(i18n::tr("Cancel"), false, 96) || (escapeCancels && ::gl::keys::escapePressed());
}

namespace {

// K-133: the line under the bar of a worker that stopped moving on (empty while it moves).
std::string stalledText(int seconds) {
  char b[160];
  std::snprintf(b, sizeof b, i18n::tr("No progress for %d s. A plugin or the disk may not be responding."), seconds);
  return b;
}
std::string stalledText(const StallWatch& w) {
  return w.stalled() ? stalledText(static_cast<int>(w.silentMs(workerstall::nowMs()) / 1000)) : std::string();
}
// A stand-in job's line (tests): stalled for a long while, or moving on.
std::string stalledText(const progui::Fake& f) { return f.stalled ? stalledText(3600) : std::string(); }

std::string statusOf(std::mutex& mu, const std::string& status) {
  std::lock_guard<std::mutex> lk(mu);
  return status;
}

void drawProgress(App& a) {
  finishJob(a);
  if (job.running) workerstall::watch(a, job.watch, job.beat, "Rendering", statusOf(job.mu, job.status));
  const progui::Fake* fk = progui::fake("render");  // (tests: a stand-in render)
  if (!job.running && !job.showSummary && !fk) return;
  const auto& P = pal();
  centerNext(ImVec2(480, 0));
  bool open = true;
  const bool summary = !job.running && job.showSummary && !fk;
  bool vis = ImGui::Begin(summary ? "Render finished###renderprog" : "Rendering###renderprog", summary ? &open : nullptr,
                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  progui::note("render");
  if (vis) {
    if (!summary) {
      std::string st;
      if (fk) {
        st = fk->text;
      } else {
        std::lock_guard<std::mutex> lk(job.mu);
        st = job.status;
      }
      const std::string stall = fk ? stalledText(*fk) : stalledText(job.watch);
      if (progressBody(st, fk ? fk->frac : job.progress.load(), true, &stall) && !fk) job.cancel = true;
    } else {
      ExportOutcome o;
      {
        std::lock_guard<std::mutex> lk(job.mu);
        o = job.outcome;
      }
      ui::BeginSection("OUTPUT SUMMARY", 440);
      ImGui::Text(i18n::tr("%zu file(s) in %.1f s"), o.files.size(), job.seconds);
      if (o.frames > 0)
        ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%.2f s of audio, peak %.1f dBFS%s"), static_cast<double>(o.frames) / job.settings.render.sampleRate,
                           o.peak > 1e-9f ? 20.0 * std::log10(o.peak) : -180.0, o.peak > 1.0f && !job.settings.normalize ? "  (clips: try Normalize)" : "");
      if (o.frames > 0 && o.lufs > -100)  // RN-09: loudness of the file as written
        ImGui::TextColored(theme::vec(P.textDim), "Integrated loudness %.1f LUFS%s", o.lufs,
                           o.ceilingLimited ? "  (below the target: the peak ceiling held it back)" : "");
      ImGui::BeginChild("##files", ImVec2(420, std::min(220.0f, 20.0f * static_cast<float>(o.files.size() + o.errors.size()) + 8)));
      for (const auto& f : o.files) {  // K-344: a file in a split's subfolder shows that folder too
        const fs::path rel = fs::path(f).lexically_relative(job.settings.folder);
        ImGui::TextUnformatted((rel.empty() || *rel.begin() == ".." ? fs::path(f).filename() : rel).string().c_str());
      }
      for (const auto& e : o.errors) ImGui::TextColored(theme::vec(P.danger), "%s", e.c_str());
      ImGui::EndChild();
      if (!o.files.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(job.settings.folder.c_str(), 420));
      ui::EndSection();
      ui::DialogFooter(96);
      if (ui::PrimaryButton("OK", 96) || ImGui::IsKeyPressed(ImGuiKey_Enter)) open = false;
    }
  }
  ImGui::End();
  if (!open) job.showSummary = false;
}

// ---------------------------------------------------------------- file jobs (K-47)

void finishFileJob(App& a) {
  if (!fileJob.finished.exchange(false)) return;
  if (fileJob.worker.joinable()) fileJob.worker.join();
  fileJob.holds.clear();
  FileJobDone done = std::move(fileJob.done);
  fileJob.done = nullptr;
  // K-133: a job that stalled and then ended is reported as moving on again (its last beat came at the end).
  if (fileJob.watch.stalled()) workerstall::watch(a, fileJob.watch, fileJob.beat, fileJob.title, std::string());
  ++fileJob.finishedCount;
  devsig::reach("file-job-finished");
  if (done) done(a);
}

void drawFileJob(App& a) {
  finishFileJob(a);
  const progui::Fake* fk = progui::fake("filejob");  // (tests: a stand-in save)
  if (!fileJob.running && !fk) return;
  if (!fk) workerstall::watch(a, fileJob.watch, fileJob.beat, fileJob.title, statusOf(fileJob.mu, fileJob.status));
  centerNext(ImVec2(480, 0));
  if (job.running || flpexport::running()) {  // never on top of the other progress window
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f + 96.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
  }
  const std::string name = std::string(i18n::tr(fk ? "Saving the zipped project" : fileJob.title.c_str())) + "###filejobprog";
  const bool vis = ImGui::Begin(name.c_str(), nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  progui::note("filejob");
  if (vis) {
    std::string st;
    if (fk) {
      st = fk->text;
    } else {
      std::lock_guard<std::mutex> lk(fileJob.mu);
      st = fileJob.status;
    }
    const std::string stall = fk ? stalledText(*fk) : stalledText(fileJob.watch);
    if (progressBody(st, fk ? fk->frac : fileJob.progress.load(), true, &stall) && !fk) fileJob.cancel = true;
  }
  ImGui::End();
}

// ---------------------------------------------------------------- MIDI import dialog

void drawMidiImport(App& a) {
  if (!midiDlg.open) return;
  centerNext(ImVec2(460, 0));
  bool vis = ImGui::Begin(i18n::tr("Import MIDI data###midiimport"), &midiDlg.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  if (vis) {
    const auto& P = pal();
    const MidiFileData& m = midiDlg.data;
    ImGui::TextUnformatted(fs::path(midiDlg.path).filename().string().c_str());
    ImGui::TextColored(theme::vec(P.textDim), i18n::tr("Format %d · %d PPQ · %.2f BPM%s · %d/%d"), m.format, m.division, m.tempo,
                       m.tempos.size() > 1 ? " (tempo changes)" : "", m.timeSigNum, m.timeSigDen);
    ui::BeginDialogBody("##midibody");  // J-54: scrolls on a short screen, the footer stays visible
    ui::BeginSection("TRACKS", 430);
    ImGui::BeginChild("##trk", ImVec2(410, std::min(200.0f, 24.0f * static_cast<float>(m.tracks.size()) + 6)));
    for (size_t k = 0; k < m.tracks.size(); ++k) {
      const auto& t = m.tracks[k];
      std::string label = t.name.empty() ? (t.program >= 0 ? std::string(gmProgramName(t.program)) : "Track " + std::to_string(k + 1)) : t.name;
      label += "  (" + std::to_string(t.notes.size()) + " notes";
      if (t.channel >= 0) label += ", ch " + std::to_string(t.channel + 1);
      label += ")##t" + std::to_string(k);
      bool on = midiDlg.tracks[k];
      if (ImGui::Checkbox(label.c_str(), &on)) midiDlg.tracks[k] = on;
    }
    ImGui::EndChild();
    ui::EndSection();
    ui::BeginSection("MIDI CHANNELS", 430);
    for (int c = 0; c < 16; ++c) {
      ImGui::PushID(c);
      char l[8];
      std::snprintf(l, sizeof l, "%d", c + 1);
      ImGui::Checkbox(l, &midiDlg.channels[c]);
      ImGui::PopID();
      if (c % 8 != 7) ImGui::SameLine(0, 6);
    }
    ui::EndSection();
    ui::BeginSection("INSTRUMENTS", 430);  // K-333
    if (ImGui::RadioButton(i18n::tr("Gridline's synths and drum kit"), !midiDlg.patchPlayer)) midiDlg.patchPlayer = false;
    if (ImGui::RadioButton(i18n::tr("A Patch Player and this file:"), midiDlg.patchPlayer)) midiDlg.patchPlayer = true;
    ImGui::SameLine();
    {
      const std::string shown = midiDlg.sf2.empty() ? std::string(i18n::tr("Choose an SF2 file...")) : fs::u8path(midiDlg.sf2).filename().u8string();
      if (ui::TextButton((shown + "##midisf2").c_str(), midiDlg.patchPlayer, 200, midiDlg.sf2.empty() ? nullptr : midiDlg.sf2.c_str())) {
        a.fileDialog = {};
        a.fileDialog.open = true;
        a.fileDialog.title = i18n::tr("Choose an SF2 file");
        a.fileDialog.dir = midiDlg.sf2.empty() ? gl::platform::homeDir() : fs::u8path(midiDlg.sf2).parent_path().u8string();
        a.fileDialog.exts = {".sf2", ".sf3"};
        a.fileDialog.onOk = [](const std::string& p) {
          midiDlg.sf2 = p;
          midiDlg.patchPlayer = true;
        };
      }
    }
    ui::Hint("Each track gets its own Patch Player on its program; MIDI channel 10 plays the file's drum kits. A program the file lacks plays the nearest one.");
    ui::EndSection();
    ui::BeginSection("OPTIONS", 430);
    ui::ToggleRow(i18n::tr("Tempo (and tempo changes as automation)"), &midiDlg.o.tempo);
    ui::ToggleRow(i18n::tr("Time signature and meter changes"), &midiDlg.o.timeSignature);
    ui::ToggleRow(i18n::tr("Markers"), &midiDlg.o.markers);
    ui::ToggleRow(i18n::tr("Volume / pan controllers as pattern automation"), &midiDlg.o.controllers);
    ui::ToggleRow(i18n::tr("Name channels after GM programs"), &midiDlg.o.programNames);
    ui::ToggleRow(i18n::tr("Place the pattern in the playlist"), &midiDlg.o.placeInPlaylist);
    {  // K-179: one pattern per meter section, each in that section's meter
      const bool can = midiDlg.o.timeSignature && midiDlg.o.placeInPlaylist;
      ImGui::BeginDisabled(!can);
      ui::ToggleRow(i18n::tr("Split into patterns at meter changes"), &midiDlg.o.splitAtMeterChanges);
      ImGui::EndDisabled();
      ui::Hint("One pattern per meter section, each in its section's meter, placed one after another");
    }
    ui::EndSection();
    ui::EndDialogBody();
    ui::DialogFooter(96 * 2 + theme::kS2);
    if (ui::TextButton("Cancel", false, 96)) midiDlg.open = false;
    ImGui::SameLine(0, theme::kS2);
    if (ui::PrimaryButton("Accept", 96)) {
      MidiImportOptions o = midiDlg.o;
      o.tracks.clear();
      o.midiChannels.clear();
      bool allT = true, allC = true;
      for (size_t k = 0; k < midiDlg.tracks.size(); ++k) allT = allT && midiDlg.tracks[k];
      for (bool c : midiDlg.channels) allC = allC && c;
      if (!allT)
        for (size_t k = 0; k < midiDlg.tracks.size(); ++k)
          if (midiDlg.tracks[k]) o.tracks.insert(static_cast<int>(k));
      if (!allC)
        for (int c = 0; c < 16; ++c)
          if (midiDlg.channels[c]) o.midiChannels.insert(c);
      if (midiDlg.patchPlayer && !midiDlg.sf2.empty()) {  // K-333: one Patch Player per track, remembered for next time
        const std::string sf2 = midiDlg.sf2;
        o.instrument = o.drumInstrument = packs::samplers::kPatchPlayerType;
        o.setUpChannel = [sf2](Channel& c, int mch, int bank, int program) { packs::samplers::setPatchForMidiTrack(c.plugin, sf2, mch, bank, program); };
        // Later program changes switch the patch while the song plays (the Patch parameter's lane).
        o.programParam = [](const Channel& c) { return c.plugin.type == packs::samplers::kPatchPlayerType ? static_cast<int>(packs::samplers::PpPatch) : -1; };
      }
      a.settings.set("midiImport.patchPlayer", midiDlg.patchPlayer && !midiDlg.sf2.empty());
      a.settings.set("midiImport.sf2", midiDlg.sf2);
      MidiImportResult r;
      const std::string name = fs::path(midiDlg.path).stem().string();
      const MidiFileData& data = midiDlg.data;  // already split by channel (the indexes shown)
      const uint64_t serial0 = a.doc.history().stateSerial();
      a.doc.editSong("import MIDI", [&](Song& s) { r = importMidiIntoSong(s, data, name, o); });
      if (!r.error.empty()) {
        if (a.doc.history().stateSerial() != serial0 && a.doc.undo()) a.doc.discardStepsFrom(a.doc.history().position());  // K-141: only our own step
        a.notify("MIDI import: " + r.error, true);
      } else {
        a.selectPattern(r.pattern);
        if (!r.channels.empty()) a.selectChannel(r.channels[0]);
        a.syncEngine();
        std::string msg = "Imported " + std::to_string(r.channels.size()) + " channel(s), " + std::to_string(r.notes) + " notes";
        if (r.tempoChanges) msg += ", " + std::to_string(r.tempoChanges) + " tempo changes";
        if (r.skippedControllers) msg += " (" + std::to_string(r.skippedControllers) + " controller events without a target)";
        a.notify(msg);
        a.uiSound(uisound::Event::Import);
      }
      midiDlg.open = false;
    }
  }
  ImGui::End();
}

// ---------------------------------------------------------------- missing samples

void drawMissing(App& a) {
  if (!missing.open) return;
  centerNext(ImVec2(640, 420), ImVec2(480, 260));
  bool vis = ImGui::Begin(i18n::tr("Missing samples###missing"), &missing.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  if (vis) {
    const auto& P = pal();
    if (missing.check && missing.check->done.load(std::memory_order_acquire)) {
      missing.files = std::move(missing.check->files);
      missing.check.reset();
    }
    if (!missing.check && a.now >= missing.nextCheck) {
      missing.nextCheck = a.now + 2.0;
      missing.check = std::make_shared<Missing::Check>();
      std::thread([c = missing.check, refs = referencedFiles(a.doc.song())] { gl::threads::setName("gl-missing-chk");
        c->files = missingAmong(refs);
        c->done.store(true, std::memory_order_release);
      }).detach();
    }
    if (missing.search && missing.search->done.load(std::memory_order_acquire)) {
      missing.found = std::move(missing.search->found);
      missing.folder = missing.search->folder;
      missing.searched = true;
      missing.search.reset();
      a.notify("Found " + std::to_string(missing.found.found.size()) + " of " + std::to_string(missing.found.found.size() + missing.found.notFound.size()) +
               " missing file(s) (" + std::to_string(missing.found.indexed) + " files searched)" +
               (missing.found.shallow ? ". On a web or network drive only that folder is searched, not its sub-folders." : ""));
    }
    ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%zu file(s) this project refers to cannot be found."), missing.files.size());
    ImGui::BeginChild("##list", ImVec2(0, -ui::DialogFooterHeight()), ImGuiChildFlags_Borders);
    for (size_t k = 0; k < missing.files.size(); ++k) {
      const std::string& f = missing.files[k];
      ImGui::PushID(static_cast<int>(k));
      const std::string name = fs::path(f).filename().string();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(theme::fitText(name.c_str(), 260 - ImGui::GetCursorPosX() - theme::kS2));  // J-54: never under the next column
      if (ImGui::IsItemHovered()) ui::Tooltip(f.c_str());
      auto it = missing.found.found.find(f);
      ImGui::SameLine(260);
      if (it != missing.found.found.end())
        ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitText(it->second.c_str(), ImGui::GetContentRegionAvail().x - 90));
      else
        ImGui::TextColored(theme::vec(missing.searched ? P.danger : P.textFaint), "%s", missing.searched ? "not found" : "");
      ImGui::SameLine(ImGui::GetWindowWidth() - 92);
      if (ui::TextButton("Locate...", false, 80)) {
        const std::string from = f;
        std::string ext = fs::path(f).extension().string();
        fileDialog(a, ("Locate " + name).c_str(), false, {".wav", ".flac", ".ogg", ".mp3", ".aif", ".aiff", ".wv", ".m4a", ".aac", ".caf", ".rx2", ".rex", ".rcy", ".png", ".jpg", ".mid"}, "",
                   [&a, from](const std::string& p) {
                     a.doc.editSong("relink sample", [&](Song& s) { relinkFiles(s, {{from, p}}); });
                     sampleCache().clear();
                     a.syncEngine();
                     missing.files.erase(std::remove(missing.files.begin(), missing.files.end(), from), missing.files.end());
                     missing.nextCheck = 0;
                   });
      }
      ImGui::PopID();
    }
    if (missing.files.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Nothing is missing."));
    ImGui::EndChild();
    ui::DialogFooter(150 + 120 + theme::kS2);
    if (missing.search) {
      if (ui::TextButton("Stop searching", false, 150, "Stops the search in the folder")) {
        missing.search->cancel.store(true);
        missing.search.reset();
      }
    } else if (ui::TextButton("Search folder...", false, 150, "Looks for the files by name in a folder and its sub-folders")) {
      fileDialog(a, "Search for missing samples in", false, {"/"}, "", [](const std::string& d) {
        auto job = std::make_shared<Missing::Search>();
        job->folder = d;
        missing.search = job;
        std::thread([job, files = missing.files, d] { gl::threads::setName("gl-relink");
          RelinkOptions o;
          o.cancel = &job->cancel;
          job->found = findReplacements(files, d, o);
          job->done.store(true, std::memory_order_release);
        }).detach();
      });
    }
    ImGui::SameLine(0, theme::kS2);
    ImGui::BeginDisabled(missing.found.found.empty());
    if (ui::PrimaryButton(("Relink " + std::to_string(missing.found.found.size())).c_str(), 120)) {
      const auto map = missing.found.found;
      int n = 0;
      a.doc.editSong("relink samples", [&](Song& s) { n = relinkFiles(s, map); });
      sampleCache().clear();
      a.syncEngine();
      missing.found = {};
      missing.nextCheck = 0;
      a.notify("Relinked " + std::to_string(n) + " reference(s)");
    }
    ImGui::EndDisabled();
  }
  ImGui::End();
}

// ---------------------------------------------------------------- helpers

bool hasExt(const std::string& p, std::initializer_list<const char*> exts) {
  std::string l = fs::path(p).extension().string();
  for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* e : exts)
    if (l == e) return true;
  return false;
}

// What a WAV export makes. `writeTo` is a temporary name: the file takes its real name when it is
// whole, so a failure or a Cancel leaves what was there. `origin` is its Track Receipt sidecar, if any.
using SampleMaker = std::function<bool(const std::string& writeTo, std::string& err, const ExportControl& control, std::unique_ptr<receipt::Origin>& origin)>;

void writeSampleJob(App& a, const std::string& title, const Song& uses, const std::string& pathIn, const SampleMaker& make) {
  std::string path = pathIn;
  if (fs::path(path).extension().empty()) path += ".wav";
  startFileJob(a, title.c_str(), uses, [make, path](const ExportControl& control) -> FileJobDone {
      const std::string tmp = path + ".part.wav";
      std::string err;
      std::unique_ptr<receipt::Origin> origin;
      bool ok = false, cancelled = false;
      try {  // (out of memory on a long clip, a file system error)
        ok = make(tmp, err, control, origin);
        cancelled = control.cancelled();  // (read once: a Cancel after the file is in place comes too late)
        if (ok && !cancelled) ok = platform::replaceFile(tmp, path, &err);
      } catch (const std::exception& e) {
        crash::reportCurrentException("Exporting audio");
        ok = false;
        err = e.what();
      }
      if (ok && !cancelled) {
        if (origin) receipt::writeOrigin(path, *origin);
      } else {
        std::error_code ec;
        fs::remove(fs::u8path(tmp), ec);
      }
      return [ok, cancelled, err, path](App& app) {
        if (cancelled) {
          app.notify("Export cancelled");
        } else if (ok) {
          app.notify("Saved " + fs::path(path).filename().string());
          app.uiSound(uisound::Event::Export);
        } else {
          app.notify("Export failed: " + err, true);
        }
      };
  });
}

void writeSampleDialog(App& a, const std::string& title, const std::string& name, const Song& uses, const SampleMaker& make) {
  Song song = uses;
  fileDialog(a, title.c_str(), true, {".wav"}, safeFileName(name) + ".wav", [&a, make, song, title](const std::string& p) { writeSampleJob(a, title, song, p, make); });
}

}  // namespace

// ---------------------------------------------------------------- public

bool fileJobRunning() { return fileJob.running.load(); }

bool startFileJob(App& a, const char* title, const Song& uses, std::function<FileJobDone(const ExportControl&)> work) {
  if (fileJob.running) {
    a.notify("Another export is still running. Wait for it, or cancel it first.", true);
    return false;
  }
  if (fileJob.worker.joinable()) fileJob.worker.join();
  fileJob.holds = archive::holdCopiesOf(uses);
  fileJob.title = title;
  fileJob.cancel = false;
  fileJob.finished = false;
  fileJob.progress = 0;
  {
    std::lock_guard<std::mutex> lk(fileJob.mu);
    fileJob.status = i18n::N_("Preparing");
  }
  fileJob.done = nullptr;
  workerstall::start(fileJob.watch, fileJob.beat);  // K-133: its progress steps are its sign of life
  fileJob.running = true;
  crash::crumb("file job start", title);
  // As for a render: this thread's progress says nothing of the interface's life, so it does not
  // beat for it; calls that must come back to the interface are busy work there.
  fileJob.worker = std::thread([work = std::move(work), titleText = fileJob.title]() { gl::threads::setName("gl-file-job");
    host::UiThreadRenderScope scope;
    static const int delayMs = [] {  // tests: a job that takes its time
      const char* e = std::getenv("GRIDLINE_DEV_SLOW_FILE_JOB_MS");
      return e ? std::atoi(e) : 0;
    }();
    ExportControl control;
    control.cancel = &fileJob.cancel;
    control.progress = [](double p, const std::string& what) {
      fileJob.beat.beat();
      fileJob.progress = p;
      {
        std::lock_guard<std::mutex> lk(fileJob.mu);
        fileJob.status = what;
      }
      if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    };
    FileJobDone done;
    try {
      done = work(control);
    } catch (...) {
      crash::reportCurrentException(titleText.c_str());
      done = [](App& app) { app.notify("An internal error stopped the export.", true); };
    }
    fileJob.beat.beat();  // the end is a sign of life too
    crash::crumb("file job end");
    fileJob.done = std::move(done);
    fileJob.finished = true;
    fileJob.running = false;
  });
  return true;
}

void waitFileJob() {
  if (fileJob.worker.joinable()) {
    host::serveUiThreadTasksWhile(fileJob.running);
    fileJob.worker.join();
  }
}

void openRender(App& a, Preset p) {
  initDialog(a);
  dlg.open = true;
  a.showExport = false;
  dlg.s.splitMixer = p == Preset::Stems;
  dlg.s.splitPlaylist = p == Preset::PlaylistTracks;
  dlg.s.splitPlaylistGroups = p == Preset::TrackGroups;  // J-31
  dlg.dest = p == Preset::AudioClip ? static_cast<int>(Dest::AudioClip) : static_cast<int>(Dest::Files);
  if (p == Preset::Midi) {
    for (bool& f : dlg.fmt) f = false;
    dlg.s.midi = true;
  } else if (p != Preset::AudioClip && p != Preset::PatternReplace && !dlg.fmt[0] && !dlg.fmt[1] && !dlg.fmt[2] && !dlg.fmt[3] && !dlg.fmt[4] && !dlg.fmt[5]) {
    dlg.fmt[0] = true;
  }
  if (p == Preset::PatternReplace) dlg.dest = static_cast<int>(Dest::AudioClip);
  if (p == Preset::AudioClip || p == Preset::PatternReplace || p == Preset::PatternFile) dlg.scope = 1;
  if (p == Preset::Audio || p == Preset::Stems || p == Preset::PlaylistTracks || p == Preset::TrackGroups) dlg.scope = dlg.scope == 1 ? 0 : dlg.scope;
  // R05-11: the reference's Bounce starts and ends at the cycle area while Cycle is on; else the song.
  if (logicplug::active(a) && p != Preset::AudioClip && p != Preset::PatternReplace && p != Preset::PatternFile)
    dlg.scope = a.playlist.selEnd > a.playlist.selStart ? 2 : (dlg.scope == 2 ? 0 : dlg.scope);
  dlg.replacePattern = p == Preset::PatternReplace;
  dlg.areaMode = p == Preset::MarkerAreas;
  if (dlg.areaMode) {  // each opening starts with the areas that hold something ticked
    dlg.areaOn.clear();
    dlg.dest = static_cast<int>(Dest::Files);
  }
}

void openRenderAgain(App& a) {  // J-27: the dialog exactly as last used (formats, scope, splitting)
  initDialog(a);
  dlg.open = true;
  a.showExport = false;
  if (!dlg.fmt[0] && !dlg.fmt[1] && !dlg.fmt[2] && !dlg.fmt[3] && !dlg.fmt[4] && !dlg.fmt[5] && !dlg.s.midi) dlg.fmt[0] = true;
}

void startRenderTo(App& a, const std::string& folder) {
  initDialog(a);
  std::snprintf(dlg.folder, sizeof dlg.folder, "%s", folder.c_str());
  startJob(a);
}

void devStart(App& a, const DevRender& r) {
  initDialog(a);
  for (int k = 0; k < 6; ++k) dlg.fmt[k] = (r.formats >> k) & 1;
  dlg.scope = r.scope;
  dlg.dest = r.dest;
  dlg.rateIdx = std::clamp(r.sampleRate, 0, 6);
  dlg.s.splitMixer = r.split == 1;
  dlg.s.splitPlaylist = r.split == 2;
  dlg.s.splitPlaylistGroups = r.split == 3;
  std::snprintf(dlg.folder, sizeof dlg.folder, "%s", r.folder.c_str());
  startJob(a);
}

void devCancel() { job.cancel = true; }

void devMarkerAreas(App& a, const std::vector<std::string>& args, int frame) {
  const std::string what = args.empty() ? "dump" : args[0];
  if (what == "open") {
    openRender(a, Preset::MarkerAreas);
  } else if (what == "start" && args.size() > 1) {
    initDialog(a);
    dlg.areaMode = true;
    std::snprintf(dlg.folder, sizeof dlg.folder, "%s", args[1].c_str());
    startJob(a);
  } else if (what == "tick" && args.size() > 2) {
    const std::vector<MarkerArea> areas = markerAreas(a.doc.song());
    const size_t i = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
    if (i < areas.size()) dlg.areaOn[areas[i].start] = args[2] == "on";
  } else if (what == "pattern" && args.size() > 1) {
    std::snprintf(dlg.areaPattern, sizeof dlg.areaPattern, "%s", args[1].c_str());
  } else {
    const Song& s = a.doc.song();
    const std::vector<MarkerArea> areas = markerAreas(s);
    std::vector<MarkerArea> ticked;
    for (const MarkerArea& m : areas)
      if (areaTicked(m)) ticked.push_back(m);
    const std::vector<std::string> names = markerAreaFileNames(ticked, dlg.areaPattern, projectName(a), kRates[dlg.rateIdx]);
    std::printf("markerareas frame %d: open %d mode %d areas %zu ticked %zu drawn %zu (frame %d)\n", frame, dlg.open ? 1 : 0, dlg.areaMode ? 1 : 0,
                areas.size(), ticked.size(), devAreaRows.size(), devAreaFrame);
    for (const MarkerArea& m : areas)
      std::printf("  area %s: %lld..%lld %s%s\n", m.name.c_str(), static_cast<long long>(m.start), static_cast<long long>(m.end), areaTicked(m) ? "on" : "off",
                  m.hasContent ? "" : " empty");
    for (const std::string& r : devAreaRows) std::printf("  row %s\n", r.c_str());
    for (const std::string& n : names) std::printf("  file %s\n", n.c_str());
    std::fflush(stdout);
  }
}

void devExportDialog(App& a, const std::vector<std::string>& args, int frame) {
  initDialog(a);
  const std::string what = args.empty() ? "dump" : args[0];
  const std::string arg = args.size() > 1 ? args[1] : "";
  ExportSettings& s = dlg.s;
  if (what == "open") {
    const std::string p = arg;
    openRender(a, p == "stems" ? Preset::Stems : p == "areas" ? Preset::MarkerAreas : p == "pattern" ? Preset::PatternFile : p == "clip" ? Preset::AudioClip : Preset::Audio);
  } else if (what == "scope") {
    dlg.areaMode = arg == "areas";
    if (arg == "song") dlg.scope = 0;
    if (arg == "pattern") dlg.scope = 1;
    if (arg == "selection") dlg.scope = 2;
    if (arg == "markers") dlg.scope = 3;
    if (dlg.areaMode) dlg.dest = static_cast<int>(Dest::Files);
  } else if (what == "tail") {
    const double sec = args.size() > 2 ? std::atof(args[2].c_str()) : -1;
    if (arg == "cut") s.render.tail = TailMode::Cut;
    if (arg == "wrap") s.render.tail = TailMode::Wrap;
    if (arg == "ring") s.render.tail = TailMode::Leave, s.render.maxTailSeconds = sec > 0 ? sec : s.render.maxTailSeconds;
    if (arg == "fixed") s.render.tail = TailMode::Fixed, s.render.fixedTailSeconds = sec >= 0 ? sec : s.render.fixedTailSeconds;
  } else if (what == "name" && args.size() > 2) {
    char* buf = arg == "area" ? dlg.areaPattern : arg == "part" ? dlg.pattern : dlg.filePattern;
    const size_t cap = arg == "area" ? sizeof dlg.areaPattern : arg == "part" ? sizeof dlg.pattern : sizeof dlg.filePattern;
    const char* tokens = arg == "area" ? exportchoice::kAreaTokens : arg == "part" ? exportchoice::kPartTokens : exportchoice::kFileTokens;
    const std::string op = args[2], v = args.size() > 3 ? args[3] : "";
    std::string out = buf;
    if (op == "add" && !v.empty()) out = exportchoice::appendToken(buf, v[0], tokens);
    if (op == "remove") out = exportchoice::removePiece(buf, static_cast<size_t>(std::max(0, std::atoi(v.c_str()))), tokens);
    if (op == "text") out = exportchoice::appendText(buf, v, tokens);
    if (op == "set") out = v;
    std::snprintf(buf, cap, "%s", out.c_str());
  } else if (what == "split") {
    s.splitMixer = arg == "mixer";
    s.splitPlaylist = arg == "playlist";
    s.splitPlaylistGroups = arg == "groups";
  } else if (what == "dest") {
    dlg.dest = arg == "clip" ? static_cast<int>(Dest::AudioClip) : arg == "editor" ? static_cast<int>(Dest::AudioEditor) : static_cast<int>(Dest::Files);
  } else if (what == "format" && args.size() > 2) {
    const int k = std::atoi(arg.c_str());
    if (k >= 0 && k < 6) dlg.fmt[k] = args[2] == "on";
  } else if (what == "folder") {
    std::snprintf(dlg.folder, sizeof dlg.folder, "%s", arg.c_str());
  } else {
    const auto pieces = [](const char* pattern, const char* tokens) {
      std::string out;
      for (const exportchoice::NamePiece& p : exportchoice::parseNamePattern(pattern, tokens))
        out += p.token ? std::string("[") + exportchoice::tokenLabel(p.token) + "]" : "'" + p.text + "'";
      return out;
    };
    const char* tails[] = {"cut", "ring", "wrap", "fixed"};
    std::string more;
    const std::string ex = exampleName(a, &more);
    std::string fmts;
    for (ExportFormat f : exportchoice::tickedFormats(dlg.fmt, static_cast<int>(ExportFormat::Count))) fmts += std::string(fmts.empty() ? "" : ",") + exportFormatName(f);
    std::printf("exportdlg frame %d: open %d areas %d scope %d dest %d tail %s ring %.1f fixed %.1f loops %d split %d%d%d formats %s\n", frame, dlg.open ? 1 : 0,
                dlg.areaMode ? 1 : 0, dlg.scope, dlg.dest, tails[std::clamp(static_cast<int>(s.render.tail), 0, 3)], s.render.maxTailSeconds, s.render.fixedTailSeconds,
                s.render.loops, s.splitMixer ? 1 : 0, s.splitPlaylist ? 1 : 0, s.splitPlaylistGroups ? 1 : 0, fmts.c_str());
    std::printf("  file name %s = %s\n  area names %s = %s\n  part names %s = %s\n", dlg.filePattern, pieces(dlg.filePattern, exportchoice::kFileTokens).c_str(),
                dlg.areaPattern, pieces(dlg.areaPattern, exportchoice::kAreaTokens).c_str(), dlg.pattern, pieces(dlg.pattern, exportchoice::kPartTokens).c_str());
    std::printf("  example %s | %s\n  summary %s\n  drawn summary %s\n", ex.c_str(), more.c_str(), summaryLine(a).c_str(), devSummary.c_str());
    std::printf("  folder %s shown(40) %s\n", dlg.folder,
                exportchoice::shortFolder(dlg.folder, homeDir(), [](const std::string& t) { return t.size() <= 40; }).c_str());
    std::fflush(stdout);
  }
}

std::string devState() {
  std::lock_guard<std::mutex> lk(job.mu);
  char b[256];
  std::snprintf(b, sizeof b, "running %d finished %d progress %.3f files %zu errors %zu cancelled %d", job.running.load() ? 1 : 0, job.finished.load() ? 1 : 0, job.progress.load(),
                job.outcome.files.size(), job.outcome.errors.size(), job.outcome.cancelled ? 1 : 0);
  std::string out = b;
  out += " (renders so far: " + std::to_string(devRendersFinished) + " finished, " + std::to_string(devRendersCancelled) + " cancelled, " +
         std::to_string(devRendersFailed) + " with errors)";
  for (const auto& e : job.outcome.errors) out += "\n  error: " + e;
  return out;
}

void draw(App& a) {
  host::runUiThreadTasks();  // hosted plugin calls of a render on its worker
  if (a.showExport) openRender(a, Preset::Audio);  // Ctrl+R / older entry points
  drawRenderDialog(a);
  drawProgress(a);
  drawFileJob(a);
  drawMidiImport(a);
  drawMissing(a);
}

void importMidiFile(App& a, const std::string& path) {
  MidiFileData m;
  std::string err;
  if (!readMidiFile(path, m, &err)) {
    a.notify("MIDI import failed: " + err, true);
    return;
  }
  if (m.format == 0 || m.tracks.size() == 1) splitTracksByChannel(m);
  if (m.tracks.empty()) {
    a.notify("The MIDI file has no notes", true);
    return;
  }
  midiDlg.open = true;
  midiDlg.path = path;
  midiDlg.data = std::move(m);
  midiDlg.tracks.assign(midiDlg.data.tracks.size(), true);
  for (bool& c : midiDlg.channels) c = true;
  midiDlg.o = MidiImportOptions{};
  midiDlg.sf2 = a.settings.getString("midiImport.sf2");  // K-333: the instrument chosen last time
  midiDlg.patchPlayer = a.settings.getBool("midiImport.patchPlayer") && !midiDlg.sf2.empty();
}

void importMidiAtCursor(App& a, const std::string& path) {
  MidiFileData m;
  std::string err;
  if (!readMidiFile(path, m, &err)) {
    a.notify("MIDI import failed: " + err, true);
    return;
  }
  const Song& s = a.doc.song();
  const ObjectId ch = a.selChannel, pat = a.curPattern;
  if (!s.channel(ch) || !s.pattern(pat)) {
    a.notify("Select a channel and a pattern first", true);
    return;
  }
  // The piano roll's view start, on a bar line.
  const Tick bar = std::max<Tick>(1, s.patternBarTicks(*s.pattern(pat)));  // K-179: the pattern's own bar
  const Tick view = a.piano.pxPerTick > 0 ? static_cast<Tick>(std::max(0.0f, a.piano.scrollX) / a.piano.pxPerTick) : 0;
  const Tick at = view / bar * bar;
  int n = 0;
  a.doc.editSong("import MIDI into piano roll", [&](Song& x) { n = insertMidiNotes(x, pat, ch, at, m); });
  a.syncEngine();
  a.notify("Inserted " + std::to_string(n) + " notes at bar " + std::to_string(at / bar + 1));
}

void exportMidiDialog(App& a, int scope) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(a.curPattern);
  const std::string name = (scope == 1 && p ? p->name : projectName(a)) + ".mid";
  const char* title = scope == 1 ? "Export pattern as MIDI" : scope == 2 ? "Export time selection as MIDI" : "Export song as MIDI";
  fileDialog(a, title, true, {".mid", ".midi"}, name, [&a, scope](const std::string& path) {
    const Song& s2 = a.doc.song();
    MidiFileData m;
    if (scope == 1) {
      MidiExportOptions o;
      if (a.selChannels.size() > 1) o.channels = a.selChannels;
      m = patternToMidi(s2, a.curPattern, o);
    } else if (scope == 2) {
      if (a.playlist.selEnd <= a.playlist.selStart) {
        a.notify("No time selection in the playlist", true);
        return;
      }
      m = songRangeToMidi(s2, a.playlist.selStart, a.playlist.selEnd);
    } else {
      m = songToMidi(s2);
    }
    std::string p2 = path, err;
    if (fs::path(p2).extension().empty()) p2 += ".mid";
    if (m.tracks.empty())
      a.notify("Nothing to export: no notes", true);
    else if (writeMidiFile(p2, m, &err)) {
      a.notify("Exported MIDI " + fs::path(p2).filename().string() + " (" + std::to_string(m.tracks.size()) + " tracks)");
      a.uiSound(uisound::Event::Export);
    } else
      a.notify("MIDI export failed: " + err, true);
  });
}

namespace {

// Missing files of a package: one red card (the archive lacks them), the paths as its details.
void notifyMissing(App& a, const char* what, const std::vector<std::string>& missingFiles) {
  if (missingFiles.empty()) return;
  std::string list;
  for (const auto& m : missingFiles) list += (list.empty() ? "" : "\n") + m;
  a.notify(std::string(what) + " written without " + std::to_string(missingFiles.size()) +
               " missing file(s); relink them with File > Locate missing samples",
           true, list);
}

// What a package could not carry: every file by name, in the card's details.
void notifyPackReport(App& a, const char* what, const CollectReport& r) {
  notifyMissing(a, what, r.missing);
  auto listOf = [](const std::vector<std::string>& files) {
    std::string list;
    for (const auto& m : files) list += (list.empty() ? "" : "\n") + m;
    return list;
  };
  if (!r.unpacked.empty())
    a.notify(std::string(what) + " still points at " + std::to_string(r.unpacked.size()) +
                 " file(s) outside it; whoever receives it will not have them",
             true, listOf(r.unpacked));
  if (!r.insidePlugins.empty())
    a.notify(std::string(what) + " holds " + std::to_string(r.insidePlugins.size()) +
                 " file(s) that third-party plugins name in their own state; on another computer, point the plugin at them",
             false, listOf(r.insidePlugins));
}

}  // namespace

namespace {

// K-47: started from the dialogs and from dev scripts. The song's copy is what gets written; hosted
// plugins were asked for their state just before, here on the interface thread.
void startDataFiles(App& a, const std::string& dir) {
  captureHostedStates(a);
  const std::string name = safeFileName(projectName(a));
  const std::string folder = (fs::u8path(dir) / fs::u8path(name)).u8string();
  Song song = a.doc.song();
  startFileJob(a, i18n::N_("Exporting project data files"), song, [song, folder, name](const ExportControl& control) -> FileJobDone {
    CollectReport r = exportProjectDataFiles(song, folder, name, &control);
    return [r = std::move(r), folder](App& app) {
      if (r.cancelled) {
        app.notify("Export cancelled");
      } else if (!r.error.empty()) {
        app.notify("Export failed: " + r.error, true);
      } else {
        app.notify("Project + " + std::to_string(r.copied + r.reused) + " file(s) in " + folder);
        notifyPackReport(app, "Project data files", r);
        if (r.missing.empty() && r.unpacked.empty()) app.uiSound(uisound::Event::Export);
      }
    };
  });
}

void startBones(App& a, const std::string& dir) {
  captureHostedStates(a);
  const std::string name = safeFileName(projectName(a));
  const std::string folder = (fs::u8path(dir) / fs::u8path(name + " bones")).u8string();
  Song song = a.doc.song();
  startFileJob(a, i18n::N_("Exporting project bones"), song, [song, folder, name](const ExportControl& control) -> FileJobDone {
    BonesReport r = exportProjectBones(song, folder, name, &control);
    return [r = std::move(r)](App& app) {
      if (r.files.cancelled) {
        app.notify("Export cancelled");
      } else if (!r.files.error.empty()) {
        app.notify("Export failed: " + r.files.error, true);
      } else {
        app.notify("Bones: " + std::to_string(r.files.copied + r.files.reused) + " samples, " + std::to_string(r.midiFiles) + " MIDI files, " +
                   std::to_string(r.presets) + " presets, " + std::to_string(r.chains) + " effect chains");
        notifyPackReport(app, "Project bones", r.files);
        if (r.files.missing.empty() && r.files.unpacked.empty()) app.uiSound(uisound::Event::Export);
      }
    };
  });
}

}  // namespace

void exportDataFilesDialog(App& a) {
  fileDialog(a, "Export project data files into folder", false, {"/"}, "", [&a](const std::string& dir) { startDataFiles(a, dir); });
}

void exportBonesDialog(App& a) {
  fileDialog(a, "Export project bones into folder", false, {"/"}, "", [&a](const std::string& dir) { startBones(a, dir); });
}


// J-80: both run as a background job (FlpExportUi.h): Gridline-only
// instruments are rendered to samples first, with progress and cancel; the
// job notifies the success summary (J-78: never an error card) or the
// failure (red, the path as its details) and keeps the full report.
bool writeFlpFile(App& a, const std::string& path, std::function<void(bool)> done) {
  return flpexport::start(a, path, false, std::move(done));
}

bool writeFlpZip(App& a, const std::string& path, std::function<void(bool)> done) {
  return flpexport::start(a, path, true, std::move(done));
}

bool writeGridlineZip(App& a, const std::string& path, std::function<void(const std::string&)> done) {
  captureHostedStates(a);
  Song song = freezeui::zipSong(a, a.doc.song());  // K-87: "Leave out frozen audio"
  return startFileJob(a, i18n::N_("Saving the zipped project"), song, [song, path, done = std::move(done)](const ExportControl& control) -> FileJobDone {
    CollectReport r = exportZippedProject(song, path, projectStemOf(path), &control);
    return [r = std::move(r), path, done](App& app) {
      if (r.cancelled) {
        app.notify("Saving the zipped project was cancelled. The file on disk is as it was.");
      } else if (!r.error.empty()) {
        app.notify("Could not save the zipped project: " + r.error, true, path);
      } else {
        notifyPackReport(app, "Zipped project", r);
        if (done) done(r.copied ? " (" + std::to_string(r.copied) + " file(s))" : std::string());
      }
    };
  });
}

void exportFlpDialog(App& a) {
  fileDialog(a, "Export FL Studio project", true, {".flp"}, safeFileName(projectName(a)) + ".flp", [&a](const std::string& p) {
    flpexport::askAndStart(a, withSaveExtension(p, SaveFormat::Flp), false);  // K-110: how Gridline plugins go, first
  });
}

void exportFlpZipDialog(App& a) {
  fileDialog(a, "Export FL Studio zipped loop package", true, {".zip"}, safeFileName(projectName(a)) + ".zip", [&a](const std::string& p) {
    flpexport::askAndStart(a, withSaveExtension(p, SaveFormat::FlZip), true);
  });
}

void exportZipDialog(App& a) {
  // J-78: the writer of Save as > Gridline zipped project; the name always ends in .gridlinez (K-121),
  // or in .gridline.zip when that older name is typed.
  fileDialog(a, "Export Gridline zipped project", true, {kZippedProjectExt, kOldZippedProjectExt}, safeFileName(projectName(a)) + kZippedProjectExt,
             [&a](const std::string& p) {
               const std::string path = withSaveExtension(p, saveFormatOf(p) == SaveFormat::GridlineZipOld ? SaveFormat::GridlineZipOld : SaveFormat::GridlineZip);
               writeGridlineZip(a, path, [&a, path](const std::string& files) {
                 a.notify("Exported " + fs::path(path).filename().string() + files, false, path);
                 a.uiSound(uisound::Event::Export);
               });
             });
}

// K-89: DAWproject files. What could not be carried is shown after the export or import.
namespace {
void showReport(App& a, const std::string& title, const dawproject::Report& r) {
  if (r.lines.empty()) return;
  std::string body = i18n::tr("Not carried over exactly:");
  for (const auto& l : r.lines) body += "\n- " + l;
  a.confirm.title = title;
  a.confirm.subtitle = body;
  a.confirm.okLabel = i18n::tr("OK");
  a.confirm.action = nullptr;
  a.confirm.open = true;
}
}  // namespace

void exportDawProjectDialog(App& a) {
  fileDialog(a, "Export DAWproject", true, {".dawproject"}, safeFileName(projectName(a)) + ".dawproject", [&a](const std::string& p) {
    std::string path = p;
    if (!dawproject::isDawProject(path)) path += ".dawproject";
    dawproject::Report rep;
    std::string err;
    if (!dawproject::exportFile(a.doc.song(), path, &rep, &err)) {
      a.notify("Could not export " + fs::path(path).filename().string() + ": " + err, true);
      return;
    }
    a.notify("Exported " + fs::path(path).filename().string(), false, path);
    a.uiSound(uisound::Event::Export);
    showReport(a, i18n::tr("DAWproject exported"), rep);
  });
}

void importDawProject(App& a, const std::string& path) {
  a.guardUnsaved([&a, path] {
    Song s;
    dawproject::Report rep;
    std::string err;
    const std::string audio = (fs::path(browserui::contentDir(a, "Imported audio")) / fs::path(path).stem()).string();
    if (!dawproject::importFile(path, audio, s, &rep, &err)) {
      a.notify("Could not open " + fs::path(path).filename().string() + ": " + err, true);
      return;
    }
    a.startProject(std::move(s));
    a.notify("Opened " + fs::path(path).filename().string());
    showReport(a, i18n::tr("DAWproject opened"), rep);
  });
}

void openMissingSamples(App& a) {
  // K-42: from a menu: the list fills from a check off the interface thread.
  openMissingSamples(a, remotefs::isRemote(a.projectFile()) ? std::vector<std::string>{} : missingFiles(a.doc.song()));
}

void openMissingSamples(App&, std::vector<std::string> known) {
  missing.open = true;
  missing.found = {};
  missing.searched = false;
  missing.files = std::move(known);
  missing.nextCheck = 0;
}

void importAudioAsClip(App& a, const std::string& path) {
  SamplePtr smp = sampleCache().get(path);
  if (!smp) {
    a.notify("Cannot decode " + fs::path(path).filename().string(), true);
    return;
  }
  const Tick at = static_cast<Tick>(std::max(0.0, a.engine.mode() == PlayMode::Song ? a.engine.position() : 0.0));
  a.doc.editSong("import audio clip", [&](Song& x) {
    Channel c;
    c.id = x.allocateId();
    c.name = fs::path(path).stem().string();
    c.color = 0x8A9BA8;
    c.plugin = defaultPluginState("gl.sampler");
    c.plugin.data = path;
    c.mixerTrack = x.masterId();
    x.channels.push_back(c);
    Clip k;
    k.id = x.allocateId();
    k.kind = ClipKind::Audio;
    k.source = c.id;
    k.track = firstFreeTrack(x);
    k.start = at;
    if (static_cast<int>(x.tracks.size()) <= k.track) x.tracks.resize(static_cast<size_t>(k.track) + 1);
    x.playlist.push_back(k);
    x.playlist.back().length = audioClipNaturalLength(x, x.playlist.back());
  });
  a.showPlaylist = true;
  a.syncEngine();
  a.notify("Added " + fs::path(path).filename().string() + " as an audio clip");
  a.uiSound(uisound::Event::Import);
}

bool openAny(App& a, const std::string& path) {
  if (dawproject::isDawProject(path)) {  // K-89
    importDawProject(a, path);
    return true;
  }
  if (hasExt(path, {".mid", ".midi", ".rmi"})) {
    importMidiFile(a, path);
    return true;
  }
  if (isAudioFile(path) && !hasExt(path, {".json", ".flp"})) {
    importAudioAsClip(a, path);
    return true;
  }
  return false;
}

namespace {

SampleMaker channelSampleMaker(const std::string& src) {
  return [src](const std::string& writeTo, std::string& err, const ExportControl& control, std::unique_ptr<receipt::Origin>& origin) {
    if (control.progress) control.progress(0.1, i18n::N_("Reading the sample"));
    SamplePtr s = loadSample(src, &err);
    if (!s) return false;
    if (control.cancelled()) return false;
    AudioBuffer b(2, s->frames());
    std::copy(s->left.begin(), s->left.end(), b.plane(0));
    std::copy(s->right.begin(), s->right.end(), b.plane(1));
    AudioFileOptions o;
    o.bits = 24;
    o.cancel = control.cancel;
    if (control.progress) control.progress(0.6, i18n::N_("Writing the file"));
    if (!writeAudioFile(writeTo, b, static_cast<int>(std::lround(s->sampleRate)), o, &err)) return false;
    origin = std::make_unique<receipt::Origin>();  // J-219: a copy, second for second (Track Receipt)
    origin->source = src;
    origin->kind = "copy";
    return true;
  };
}

SampleMaker clipMaker(const Song& songIn, ObjectId clip, int rate) {
  return [song = songIn, clip, rate](const std::string& writeTo, std::string& err, const ExportControl& control, std::unique_ptr<receipt::Origin>& origin) {
    // K-250: the clip as it sounds (clip gain, pitch, stretch, fades) through its channels, master
    // neutral: the one clip render (engine/ClipRender.h) every clip render shares.
    if (!song.clip(clip)) return false;
    if (control.progress) control.progress(0, i18n::N_("Rendering the clip"));
    std::function<void(double)> prog;
    if (control.progress) prog = [&control](double p) { control.progress(0.9 * p, i18n::N_("Rendering the clip")); };
    RenderResult r = renderClipAlone(song, clip, rate, prog, control.cancel);
    if (r.cancelled || control.cancelled()) return false;
    if (!r.error.empty()) {
      err = r.error;
      return false;
    }
    AudioFileOptions o;
    o.bits = 24;
    o.cancel = control.cancel;
    if (control.progress) control.progress(0.92, i18n::N_("Writing the file"));
    if (!writeAudioFile(writeTo, r.audio, rate, o, &err)) return false;
    auto og = std::make_unique<receipt::Origin>();  // J-219: the clip's part of its file (Track Receipt)
    if (clipRenderOrigin(song, clip, static_cast<int64_t>(r.audio.frames), rate, *og)) origin = std::move(og);
    return true;
  };
}

}  // namespace

void exportChannelSample(App& a, ObjectId channel) {
  const Channel* c = a.doc.song().channel(channel);
  if (!c || c->plugin.data.empty()) return;
  const std::string src = c->plugin.data.substr(0, c->plugin.data.find('\n'));  // J-182: the file (first line)
  writeSampleDialog(a, "Export sample as WAV", c->name, a.doc.song(), channelSampleMaker(src));
}

void exportClipAudio(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c) return;
  const std::string name = c->kind == ClipKind::Audio && s.channel(c->source) ? s.channel(c->source)->name : "clip";
  const int rate = a.audioSampleRate > 0 ? a.audioSampleRate : 44100;
  captureHostedStates(a);  // a clip through a hosted effect: its current state, asked for here
  writeSampleDialog(a, "Export clip as WAV", name, a.doc.song(), clipMaker(a.doc.song(), clip, rate));
}

// K-251: each clip of `clips` to its own file in a folder the user picks (one job, one file per clip).
void exportClipsEach(App& a, const std::set<ObjectId>& clips) {
  const std::vector<ObjectId> ids = renderableClips(a.doc.song(), clips);
  if (ids.empty()) {
    a.notify(i18n::tr("Select the clips to render first"), true);
    return;
  }
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = ids.size() == 1 ? i18n::tr("Choose the folder for the clip's file") : i18n::tr("Choose the folder for the clips' files");
  const std::string last = a.settings.getString("export.clipsDir");
  const char* h = platform::homeEnv();
  std::error_code ec;
  a.fileDialog.dir = !last.empty() && fs::is_directory(fs::u8path(last), ec) ? last
                     : a.projectPath.empty()                                  ? std::string(h ? h : ".")
                                                                              : fs::path(a.projectPath).parent_path().string();
  a.fileDialog.exts = {"/"};
  const std::set<ObjectId> sel(ids.begin(), ids.end());
  a.fileDialog.onOk = [&a, sel](const std::string& dir) { exportClipsEachTo(a, sel, dir); };
}

void exportClipsEachTo(App& a, const std::set<ObjectId>& clips, const std::string& dir) {
  a.settings.set("export.clipsDir", dir);
  const int rate = a.audioSampleRate > 0 ? a.audioSampleRate : 44100;
  captureHostedStates(a);  // clips through a hosted effect: its current state, asked for here
  const Song song = a.doc.song();
  startFileJob(a, i18n::N_("Render clips to files"), song, [song, clips, dir, rate](const ExportControl& control) -> FileJobDone {
    std::string err;
    std::vector<ClipFile> files;
    try {
      files = renderClipsToFiles(song, clips, dir, rate, 24, &err,
                                 control.progress ? std::function<void(double)>([&control](double p) { control.progress(p, i18n::N_("Rendering the clips")); })
                                                  : std::function<void(double)>{},
                                 control.cancel);
    } catch (const std::exception& e) {
      crash::reportCurrentException("Rendering clips to files");
      err = e.what();
    }
    const bool cancelled = control.cancelled();
    const size_t n = files.size();
    return [n, err, cancelled, dir](App& app) {
      if (cancelled) {
        app.notify("Render cancelled");
      } else if (n == 0) {
        app.notify("Nothing rendered: " + err, true);
      } else {
        app.notify(i18n::plural(static_cast<long>(n), "%d clip rendered to its own file", "%d clips rendered, each to its own file") + ": " +
                   fs::path(dir).filename().u8string());
        app.uiSound(uisound::Event::Export);
      }
    };
  });
}

void devFileJob(App& a, const std::vector<std::string>& args) {
  const std::string what = args.empty() ? "state" : args[0];
  const std::string to = args.size() > 1 ? args[1] : "";
  if (what == "data") {
    startDataFiles(a, to);
  } else if (what == "bones") {
    startBones(a, to);
  } else if (what == "zip") {
    a.saveProject(to);
  } else if (what == "clip") {  // the first audio clip
    captureHostedStates(a);
    const Song& s = a.doc.song();
    for (const auto& c : s.playlist)
      if (c.kind == ClipKind::Audio) {
        writeSampleJob(a, "Export clip as WAV", s, to, clipMaker(s, c.id, a.audioSampleRate > 0 ? a.audioSampleRate : 44100));
        break;
      }
  } else if (what == "clips") {  // K-251: the selected clips (else every clip), each to its own file in <to>
    std::set<ObjectId> sel = a.playlist.selected;
    if (sel.empty())
      for (const auto& c : a.doc.song().playlist) sel.insert(c.id);
    exportClipsEachTo(a, sel, to);
  } else if (what == "sample") {  // the first channel with a sample file
    for (const auto& c : a.doc.song().channels) {
      const PluginInfo* pi = findPlugin(c.plugin.type);
      if (pi && pi->usesSample && !c.plugin.data.empty() && c.plugin.data[0] != '{') {
        writeSampleJob(a, "Export sample as WAV", a.doc.song(), to, channelSampleMaker(c.plugin.data.substr(0, c.plugin.data.find('\n'))));
        break;
      }
    }
  } else if (what == "cancel") {
    fileJob.cancel = true;
  } else if (what == "wait") {
    waitFileJob();
  } else {
    std::printf("filejob running %d finished %d count %d progress %.3f\n", fileJob.running.load() ? 1 : 0, fileJob.finished.load() ? 1 : 0, fileJob.finishedCount, fileJob.progress.load());
    std::fflush(stdout);
  }
}

void shutdown() {
  job.cancel = true;
  host::serveUiThreadTasksWhile(job.running);  // the render may be waiting for its plugins to close
  if (job.worker.joinable()) job.worker.join();
  fileJob.cancel = true;  // (it removes what it wrote; a save that was not done leaves the file as it was)
  host::serveUiThreadTasksWhile(fileJob.running);
  if (fileJob.worker.joinable()) fileJob.worker.join();
  fileJob.holds.clear();
  renderHolds.clear();
}

}  // namespace gl::exportui

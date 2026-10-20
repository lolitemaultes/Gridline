// Gridline — export & interop UI (J-20). See ExportUi.h.
//
// The render dialog follows the reference's render form: export type and tail
// at the top, file name and location, output formats (several at once), then
// quality and the miscellaneous switches (split mixer tracks, master / insert
// effects, tempo info, markers). Rendering runs on a worker thread with a
// progress window that can cancel; the summary lists the files written.
#include "ExportUi.h"
#include <algorithm>
#include <atomic>
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
#include "../core/Automation.h"
#include "../core/Log.h"
#include "../core/flp/Flp.h"
#include "../core/flp/FlpExport.h"
#include "../core/MidiFile.h"
#include "../core/MidiImport.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"
#include "../engine/ClipOps.h"
#include "../engine/ExportJob.h"
#include "../engine/Resample.h"
#include "../engine/Sample.h"
#include "../packs/PacksUi.h"
#include "App.h"
#include "ContextMenus.h"
#include "HostedUi.h"
#include "Theme.h"
#include "Widgets.h"

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
  int markerA = 0, markerB = 1;
  bool fmt[5] = {true, false, false, false, false};  // WAV FLAC MP3 Ogg M4A
  int wavBits = 1;         // 0 16, 1 24, 2 32f
  int flacBits = 1;        // 0 16, 1 24
  int mp3Rate = 3, oggRate = 2, m4aRate = 2;  // index in kBitrates
  int rateIdx = 2;         // index in kRates
  int dest = 0;            // Dest
  bool replacePattern = false;
  char folder[1024] = "";
  char name[256] = "";
  char pattern[128] = "%P - %N %T";
};
Dialog dlg;

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
  bool showSummary = false;
  double started = 0, seconds = 0;
  ~Job() {
    cancel = true;
    if (worker.joinable()) worker.join();
  }
};
Job job;

// MIDI import options.
struct MidiDlg {
  bool open = false;
  std::string path;
  MidiFileData data;
  std::vector<bool> tracks;
  bool channels[16];
  MidiImportOptions o;
};
MidiDlg midiDlg;

// Missing samples.
struct Missing {
  bool open = false;
  std::vector<std::string> files;
  RelinkReport found;
  std::string folder;
  bool searched = false;
};
Missing missing;

void formLabel(const char* label, float x = 132.0f) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textDim), "%s", label);
  ImGui::SameLine(x);
}

void centerNext(ImVec2 size) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (size.x > 0) ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
}

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
  const char* h = std::getenv("HOME");
  return h ? h : ".";
}

std::string projectFolder(const App& a) { return a.projectPath.empty() ? homeDir() : fs::path(a.projectPath).parent_path().string(); }

// Rendered audio that becomes part of the project (audio clips, the audio editor).
std::string renderedFolder(const App& a) {
  return a.projectPath.empty() ? (fs::path(a.configDir()) / "Rendered").string() : (fs::path(a.projectPath).parent_path() / "Rendered").string();
}

std::string projectName(const App& a) {
  const std::string t = a.doc.song().info.title;
  if (!t.empty() && t != "Untitled") return t;
  if (!a.projectPath.empty()) {
    std::string n = fs::path(a.projectPath).filename().string();
    if (auto p = n.find(".gridline.json"); p != std::string::npos) return n.substr(0, p);
    return fs::path(a.projectPath).stem().string();
  }
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

// Scope of the dialog → render job fields. False (with a reason) when empty.
bool applyScope(const App& a, RenderJob& r, std::string* why) {
  const Song& s = a.doc.song();
  r.scope = RenderScope::Song;
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
      if (r.end <= r.start) return why && (*why = "no time selection in the playlist", false);
      break;
    case 3: {
      r.scope = RenderScope::Range;
      const auto& m = s.markers;
      if (m.size() < 2) return why && (*why = "needs two playlist markers", false);
      const int A = std::clamp(dlg.markerA, 0, static_cast<int>(m.size()) - 1), B = std::clamp(dlg.markerB, 0, static_cast<int>(m.size()) - 1);
      r.start = m[static_cast<size_t>(A)].pos;
      r.end = m[static_cast<size_t>(B)].pos;
      if (r.end <= r.start) return why && (*why = "the end marker is not after the start marker", false);
      break;
    }
    default:
      if (s.lengthTicks() <= 0) return why && (*why = "the playlist is empty", false);
      break;
  }
  return true;
}

double scopeSeconds(const App& a) {
  RenderJob r;
  if (!applyScope(a, r, nullptr)) return 0;
  Tick A = 0, B = 0;
  if (!renderScopeTicks(a.doc.song(), r, A, B)) return 0;
  return static_cast<double>(scopeFrames(a.doc.song(), r, A, B, 48000)) / 48000.0;
}

// ---------------------------------------------------------------- worker

void startJob(App& a) {
  if (job.running) return;
  if (job.worker.joinable()) job.worker.join();
  captureHostedStates(a);  // offline instances load the plugins' current state
  ExportSettings s = dlg.s;
  std::string why;
  if (!applyScope(a, s.render, &why)) {
    a.notify("Nothing to render: " + why, true);
    return;
  }
  s.formats.clear();
  const Dest dest = static_cast<Dest>(dlg.dest);
  if (dest == Dest::Files) {
    for (int k = 0; k < 5; ++k)
      if (dlg.fmt[k]) s.formats.push_back(static_cast<ExportFormat>(k));
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
    s.baseName = dlg.name[0] ? std::string(dlg.name) : projectName(a);
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
  s.render.sampleRate = kRates[dlg.rateIdx];
  s.partPattern = dlg.pattern;
  s.projectTitle = projectName(a);
  s.render.threads = std::max(1, a.processingThreads());
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
  auto work = [song = std::move(song), s]() {
    ExportOutcome o = runExport(song, s, [](double p, const std::string& what) {
      job.progress = p;
      std::lock_guard<std::mutex> lk(job.mu);
      job.status = what;
    }, &job.cancel);
    {
      std::lock_guard<std::mutex> lk(job.mu);
      job.outcome = std::move(o);
    }
    job.finished = true;
    job.running = false;
  };
  // Hosted plugins create their offline instances where the render runs; some
  // formats only allow that on the main thread, so those projects render here
  // (the window waits) instead of on the worker.
  bool hosted = false;
  for (const auto& c : a.doc.song().channels) hosted = hosted || isHostedType(c.plugin.type);
  for (const auto& m : a.doc.song().mixer)
    for (const auto& sl : m.slots) hosted = hosted || isHostedType(sl.plugin.type);
  dlg.open = false;
  if (hosted)
    work();
  else
    job.worker = std::thread(std::move(work));
}

// UI thread: what to do with the result.
void finishJob(App& a) {
  if (!job.finished.exchange(false)) return;
  if (job.worker.joinable()) job.worker.join();
  job.seconds = a.now - job.started;
  ExportOutcome o;
  {
    std::lock_guard<std::mutex> lk(job.mu);
    o = job.outcome;
  }
  if (o.cancelled) {
    a.notify("Render cancelled");
    for (const auto& f : o.files) {
      std::error_code ec;
      fs::remove(f, ec);
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
      a.notify("Rendered into the audio editor");
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
    a.notify("Exported " + std::to_string(o.files.size()) + " file(s)");
    a.uiSound(uisound::Event::Export);
  }
}

// ---------------------------------------------------------------- render dialog

void initDialog(App& a) {
  if (dlg.init) return;
  dlg.init = true;
  dlg.s.render.tail = TailMode::Leave;
  dlg.s.render.maxTailSeconds = 20;
  for (int k = 0; k < 7; ++k)
    if (kRates[k] == a.audioSampleRate) dlg.rateIdx = k;
}

void drawRenderDialog(App& a) {
  if (!dlg.open) return;
  initDialog(a);
  const auto& P = pal();
  const Song& song = a.doc.song();
  centerNext(ImVec2(780, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin("Render###render", &dlg.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ExportSettings& s = dlg.s;
  const float colW = 360;
  const bool files = dlg.dest == static_cast<int>(Dest::Files);
  // ---- left column
  ImGui::BeginGroup();
  ui::BeginSection("EXPORT TYPE", colW);
  {
    formLabel("Render");
    const char* scopes[] = {"Song", "Pattern", "Time selection", "Marker range"};
    combo("##scope", &dlg.scope, scopes, 4, colW - 148);
    if (dlg.scope == 3) {
      std::vector<std::string> names;
      for (size_t k = 0; k < song.markers.size(); ++k)
        names.push_back(song.markers[k].name.empty() ? "Marker " + std::to_string(k + 1) : song.markers[k].name);
      std::vector<const char*> items;
      for (const auto& n : names) items.push_back(n.c_str());
      if (items.empty()) {
        formLabel("Markers");
        ImGui::TextColored(theme::vec(P.textFaint), "Add two playlist markers first");
      } else {
        formLabel("From marker");
        combo("##ma", &dlg.markerA, items.data(), static_cast<int>(items.size()), colW - 148);
        formLabel("To marker");
        combo("##mb", &dlg.markerB, items.data(), static_cast<int>(items.size()), colW - 148);
      }
    }
    if (dlg.scope == 1) {
      const Pattern* p = song.pattern(a.curPattern);
      formLabel("Pattern");
      ImGui::TextUnformatted(p ? p->name.c_str() : "(none)");
    }
    if (dlg.scope == 2 && a.playlist.selEnd <= a.playlist.selStart) {
      formLabel("");
      ImGui::TextColored(theme::vec(P.textFaint), "Select a time range in the playlist ruler");
    }
    formLabel("Tail");
    int tail = static_cast<int>(s.render.tail);
    const char* tails[] = {"Cut remainder", "Leave remainder", "Wrap remainder"};
    if (combo("##tail", &tail, tails, 3, colW - 148)) s.render.tail = static_cast<TailMode>(tail);
    ui::Tooltip("What happens to sound still ringing at the end: cut it, let it ring out, or mix it into the start (seamless loops)");
    formLabel("Loops");
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("##loops", &s.render.loops, 1, 1);
    s.render.loops = std::clamp(s.render.loops, 1, 64);
    const double sec = scopeSeconds(a) * s.render.loops;
    formLabel("Length");
    ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
    ImGui::Text("%d:%05.2f", static_cast<int>(sec / 60), std::fmod(sec, 60.0));
    ImGui::PopFont();
    if (!song.mixer.empty() && song.mixer[0].muted) {
      ImGui::TextColored(theme::vec(P.danger), "The master track is muted: the render will be silent");
    }
  }
  ui::EndSection();
  ui::BeginSection("DESTINATION", colW);
  {
    formLabel("Render to", 110);
    const char* dests[] = {"Files", "Audio clip", "Audio editor"};
    ui::Segmented("dest", dests, 3, &dlg.dest, 76);
    if (!files) {
      ImGui::TextColored(theme::vec(P.textFaint), "32-bit float WAV in %s", theme::fitText(renderedFolder(a).c_str(), colW - 140));
      if (dlg.dest == 1 && dlg.scope == 1) ImGui::Checkbox("Replace the pattern's clips (render and replace)", &dlg.replacePattern);
    }
  }
  ui::EndSection();
  if (files) {
    ui::BeginSection("FILE NAME AND LOCATION", colW);
    if (!dlg.folder[0]) std::snprintf(dlg.folder, sizeof dlg.folder, "%s", projectFolder(a).c_str());
    if (!dlg.name[0]) std::snprintf(dlg.name, sizeof dlg.name, "%s", projectName(a).c_str());
    formLabel("Folder");
    ImGui::SetNextItemWidth(colW - 148 - 70);
    ImGui::InputText("##folder", dlg.folder, sizeof dlg.folder);
    ImGui::SameLine(0, theme::kS1);
    if (ui::TextButton("Browse", false, 66))
      fileDialog(a, "Render into folder", false, {"/"}, "", [](const std::string& d) { std::snprintf(dlg.folder, sizeof dlg.folder, "%s", d.c_str()); });
    formLabel("");
    if (ui::TextButton("With project", false, 110, "Put the files next to the project file"))
      std::snprintf(dlg.folder, sizeof dlg.folder, "%s", projectFolder(a).c_str());
    formLabel("File name");
    ImGui::SetNextItemWidth(colW - 148);
    ImGui::InputText("##name", dlg.name, sizeof dlg.name);
    if (s.splitMixer || s.splitPlaylist || s.splitPlaylistGroups) {
      formLabel("Part names");
      ImGui::SetNextItemWidth(colW - 148);
      ImGui::InputText("##pattern", dlg.pattern, sizeof dlg.pattern);
      ui::Tooltip("%P project, %T track name, %N track number, %S scope, %R sample rate, %D date");
      std::map<char, std::string> t = {{'P', dlg.name}, {'N', "03"}, {'T', "Snare"}, {'S', "Song"}, {'R', std::to_string(kRates[dlg.rateIdx])}};
      formLabel("");
      ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(expandNamePattern(dlg.pattern, t).c_str(), colW - 148));
    }
    ui::EndSection();
    ui::BeginSection("OUTPUT FORMAT", colW);
    {
      const char* wb[] = {"16Bit int", "24Bit int", "32Bit float"};
      const char* fb[] = {"16Bit int", "24Bit int"};
      const char* br[] = {"128 kbps", "192 kbps", "256 kbps", "320 kbps"};
      auto row = [&](int k, const char* label, const char* const* items, int n, int* idx) {
        ImGui::Checkbox(label, &dlg.fmt[k]);
        ImGui::SameLine(132);
        ImGui::BeginDisabled(!dlg.fmt[k]);
        combo((std::string("##f") + label).c_str(), idx, items, n, 120);
        ImGui::EndDisabled();
        if (exportFormatExternal(static_cast<ExportFormat>(k))) {
          ImGui::SameLine();
          const bool have = !encoderFor(static_cast<ExportFormat>(k)).empty();
          ImGui::TextColored(theme::vec(have ? P.textFaint : P.danger), "%s", have ? ("via " + encoderFor(static_cast<ExportFormat>(k))).c_str() : "no encoder");
          if (ImGui::IsItemHovered()) ui::Tooltip(encoderStatus(static_cast<ExportFormat>(k)).c_str());
        }
      };
      row(0, "WAV", wb, 3, &dlg.wavBits);
      row(1, "FLAC", fb, 2, &dlg.flacBits);
      row(2, "MP3", br, 4, &dlg.mp3Rate);
      row(3, "Ogg Vorbis", br, 4, &dlg.oggRate);
      row(4, "AAC (.m4a)", br, 4, &dlg.m4aRate);
      ImGui::Checkbox("MIDI", &s.midi);
      formLabel("Channels");
      int cm = static_cast<int>(s.channels);
      const char* cms[] = {"Stereo", "Mono (merged)", "Mono (left only)", "Mono (right only)"};
      if (combo("##chmode", &cm, cms, 4, colW - 148)) s.channels = static_cast<ChannelMode>(cm);
    }
    ui::EndSection();
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, theme::kS3);
  // ---- right column
  ImGui::BeginGroup();
  ui::BeginSection("QUALITY", colW);
  {
    formLabel("Sample rate");
    const char* rates[] = {"22050 Hz", "32000 Hz", "44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz", "192000 Hz"};
    combo("##rate", &dlg.rateIdx, rates, 7, colW - 148);
    formLabel("Resampling");
    int q = static_cast<int>(s.render.interp);
    const char* qs[] = {"Linear (as played)", "Hermite (4-point)", "Sinc 16-point", "Sinc 32-point", "Sinc 64-point", "Sinc 128-point", "Sinc 256-point", "Sinc 512-point"};
    if (combo("##interp", &q, qs, 8, colW - 148)) s.render.interp = static_cast<RenderInterp>(q);
    ui::Tooltip("Interpolation of sampler channels while rendering");
    formLabel("Oversampling");
    int os = s.render.oversample >= 4 ? 2 : s.render.oversample >= 2 ? 1 : 0;
    const char* oss[] = {"Off", "2x", "4x"};
    if (ui::Segmented("os", oss, 3, &os)) s.render.oversample = os == 2 ? 4 : os == 1 ? 2 : 1;
    ui::Tooltip("Renders at a multiple of the sample rate and filters down (less aliasing from synths and distortion)");
    formLabel("Dither");
    int d = static_cast<int>(s.dither);
    const char* ds[] = {"Off", "Triangular (TPDF)", "Noise-shaped"};
    if (combo("##dither", &d, ds, 3, colW - 148)) s.dither = static_cast<DitherMode>(d);
    ui::Tooltip("Applied when writing 16 / 24-bit files");
    bool hq = true;
    ImGui::BeginDisabled();
    ImGui::Checkbox("HQ for all plugins (offline quality)", &hq);
    ImGui::EndDisabled();
    bool noPoly = !s.render.polyphonyLimits;
    if (ImGui::Checkbox("Disable maximum polyphony", &noPoly)) s.render.polyphonyLimits = !noPoly;
  }
  ui::EndSection();
  ui::BeginSection("MISCELLANEOUS", colW);
  {
    ImGui::BeginDisabled(!files);
    ImGui::Checkbox("Split mixer tracks", &s.splitMixer);
    ui::Tooltip("One file per mixer insert that carries sound (stems)");
    if (s.splitMixer) {
      ImGui::Indent(18);
      ImGui::Checkbox("Stems through the master effects", &s.stemsMasterFx);
      ImGui::Checkbox("Also render the full mix", &s.stemsIncludeMaster);
      ImGui::Unindent(18);
    }
    ImGui::Checkbox("Split playlist tracks", &s.splitPlaylist);
    ui::Tooltip("One file per playlist track that holds clips");
    ImGui::Checkbox("Split playlist track groups", &s.splitPlaylistGroups);  // J-31
    ui::Tooltip("One file per playlist track group (a group track and the tracks it holds)");
    ImGui::EndDisabled();
    ImGui::Checkbox("Enable master effects", &s.render.masterEffects);
    ImGui::Checkbox("Enable insert effects", &s.render.insertEffects);
    ImGui::BeginDisabled(!files);
    ImGui::Checkbox("Save tempo information (WAV)", &s.tempoInfo);
    ImGui::Checkbox("Playlist markers as WAV cue points", &s.markers);
    ImGui::EndDisabled();
    ImGui::Checkbox("Normalize", &s.normalize);
    if (s.normalize) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110);
      float db = static_cast<float>(s.normalizeDb);
      if (ImGui::DragFloat("##ndb", &db, 0.05f, -24.0f, 0.0f, "%.1f dBFS")) s.normalizeDb = db;
    }
  }
  ui::EndSection();
  ImGui::EndGroup();
  ui::DialogFooter(96 * 2 + theme::kS2);
  if (ui::TextButton("Cancel", false, 96)) dlg.open = false;
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Start", true, 96) || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) startJob(a);
  ImGui::End();
}

void drawProgress(App& a) {
  finishJob(a);
  if (!job.running && !job.showSummary) return;
  const auto& P = pal();
  centerNext(ImVec2(480, 0));
  bool open = true;
  const bool summary = !job.running && job.showSummary;
  bool vis = ImGui::Begin(summary ? "Render finished###renderprog" : "Rendering###renderprog", summary ? &open : nullptr,
                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  if (vis) {
    if (!summary) {
      std::string st;
      {
        std::lock_guard<std::mutex> lk(job.mu);
        st = job.status;
      }
      ImGui::TextUnformatted(st.c_str());
      ImGui::ProgressBar(static_cast<float>(job.progress.load()), ImVec2(440, 0));
      ui::DialogFooter(96);
      if (ui::TextButton("Cancel", false, 96) || ImGui::IsKeyPressed(ImGuiKey_Escape)) job.cancel = true;
    } else {
      ExportOutcome o;
      {
        std::lock_guard<std::mutex> lk(job.mu);
        o = job.outcome;
      }
      ui::BeginSection("OUTPUT SUMMARY", 440);
      ImGui::Text("%zu file(s) in %.1f s", o.files.size(), job.seconds);
      if (o.frames > 0)
        ImGui::TextColored(theme::vec(P.textDim), "%.2f s of audio, peak %.1f dBFS%s", static_cast<double>(o.frames) / job.settings.render.sampleRate,
                           o.peak > 1e-9f ? 20.0 * std::log10(o.peak) : -180.0, o.peak > 1.0f && !job.settings.normalize ? "  (clips: try Normalize)" : "");
      ImGui::BeginChild("##files", ImVec2(420, std::min(220.0f, 20.0f * static_cast<float>(o.files.size() + o.errors.size()) + 8)));
      for (const auto& f : o.files) ImGui::TextUnformatted(fs::path(f).filename().string().c_str());
      for (const auto& e : o.errors) ImGui::TextColored(theme::vec(P.danger), "%s", e.c_str());
      ImGui::EndChild();
      if (!o.files.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(fs::path(o.files[0]).parent_path().string().c_str(), 420));
      ui::EndSection();
      ui::DialogFooter(96);
      if (ui::TextButton("OK", true, 96) || ImGui::IsKeyPressed(ImGuiKey_Enter)) open = false;
    }
  }
  ImGui::End();
  if (!open) job.showSummary = false;
}

// ---------------------------------------------------------------- MIDI import dialog

void drawMidiImport(App& a) {
  if (!midiDlg.open) return;
  centerNext(ImVec2(460, 0));
  bool vis = ImGui::Begin("Import MIDI data###midiimport", &midiDlg.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  if (vis) {
    const auto& P = pal();
    const MidiFileData& m = midiDlg.data;
    ImGui::TextUnformatted(fs::path(midiDlg.path).filename().string().c_str());
    ImGui::TextColored(theme::vec(P.textDim), "Format %d · %d PPQ · %.2f BPM%s · %d/%d", m.format, m.division, m.tempo,
                       m.tempos.size() > 1 ? " (tempo changes)" : "", m.timeSigNum, m.timeSigDen);
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
    ui::BeginSection("OPTIONS", 430);
    ImGui::Checkbox("Tempo (and tempo changes as automation)", &midiDlg.o.tempo);
    ImGui::Checkbox("Time signature and meter changes", &midiDlg.o.timeSignature);
    ImGui::Checkbox("Markers", &midiDlg.o.markers);
    ImGui::Checkbox("Volume / pan controllers as pattern automation", &midiDlg.o.controllers);
    ImGui::Checkbox("Name channels after GM programs", &midiDlg.o.programNames);
    ImGui::Checkbox("Place the pattern in the playlist", &midiDlg.o.placeInPlaylist);
    ui::EndSection();
    ui::DialogFooter(96 * 2 + theme::kS2);
    if (ui::TextButton("Cancel", false, 96)) midiDlg.open = false;
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Accept", true, 96)) {
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
      MidiImportResult r;
      const std::string name = fs::path(midiDlg.path).stem().string();
      const MidiFileData& data = midiDlg.data;  // already split by channel (the indexes shown)
      a.doc.editSong("import MIDI", [&](Song& s) { r = importMidiIntoSong(s, data, name, o); });
      if (!r.error.empty()) {
        a.doc.undo();
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
  centerNext(ImVec2(640, 420));
  ImGui::SetNextWindowSizeConstraints(ImVec2(480, 260), ImVec2(1200, 1000));
  bool vis = ImGui::Begin("Missing samples###missing", &missing.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  if (vis) {
    const auto& P = pal();
    missing.files = missingFiles(a.doc.song());
    ImGui::TextColored(theme::vec(P.textDim), "%zu file(s) this project refers to cannot be found.", missing.files.size());
    ImGui::BeginChild("##list", ImVec2(0, -ui::DialogFooterHeight()), ImGuiChildFlags_Borders);
    for (size_t k = 0; k < missing.files.size(); ++k) {
      const std::string& f = missing.files[k];
      ImGui::PushID(static_cast<int>(k));
      const std::string name = fs::path(f).filename().string();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(name.c_str());
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
        fileDialog(a, ("Locate " + name).c_str(), false, {".wav", ".flac", ".ogg", ".mp3", ".aif", ".aiff", ".wv", ".png", ".jpg", ".mid"}, "",
                   [&a, from](const std::string& p) {
                     a.doc.editSong("relink sample", [&](Song& s) { relinkFiles(s, {{from, p}}); });
                     sampleCache().clear();
                     a.syncEngine();
                   });
      }
      ImGui::PopID();
    }
    if (missing.files.empty()) ImGui::TextColored(theme::vec(P.textFaint), "Nothing is missing.");
    ImGui::EndChild();
    ui::DialogFooter(150 + 120 + theme::kS2);
    if (ui::TextButton("Search folder...", false, 150, "Looks for the files by name in a folder and its sub-folders"))
      fileDialog(a, "Search for missing samples in", false, {"/"}, "", [&a](const std::string& d) {
        missing.folder = d;
        missing.found = findReplacements(missingFiles(a.doc.song()), d);
        missing.searched = true;
        a.notify("Found " + std::to_string(missing.found.found.size()) + " of " + std::to_string(missing.found.found.size() + missing.found.notFound.size()) +
                 " missing file(s) (" + std::to_string(missing.found.indexed) + " files searched)");
      });
    ImGui::SameLine(0, theme::kS2);
    ImGui::BeginDisabled(missing.found.found.empty());
    if (ui::TextButton(("Relink " + std::to_string(missing.found.found.size())).c_str(), true, 120)) {
      const auto map = missing.found.found;
      int n = 0;
      a.doc.editSong("relink samples", [&](Song& s) { n = relinkFiles(s, map); });
      sampleCache().clear();
      a.syncEngine();
      missing.found = {};
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

void writeSampleDialog(App& a, const std::string& title, const std::string& name, std::function<bool(const std::string&, std::string&)> make) {
  fileDialog(a, title.c_str(), true, {".wav"}, safeFileName(name) + ".wav", [&a, make](const std::string& p) {
    std::string path = p, err;
    if (fs::path(path).extension().empty()) path += ".wav";
    if (make(path, err)) {
      a.notify("Saved " + fs::path(path).filename().string());
      a.uiSound(uisound::Event::Export);
    } else {
      a.notify("Export failed: " + err, true);
    }
  });
}

}  // namespace

// ---------------------------------------------------------------- public

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
  } else if (p != Preset::AudioClip && p != Preset::PatternReplace && !dlg.fmt[0] && !dlg.fmt[1] && !dlg.fmt[2] && !dlg.fmt[3] && !dlg.fmt[4]) {
    dlg.fmt[0] = true;
  }
  if (p == Preset::PatternReplace) dlg.dest = static_cast<int>(Dest::AudioClip);
  if (p == Preset::AudioClip || p == Preset::PatternReplace || p == Preset::PatternFile) dlg.scope = 1;
  if (p == Preset::Audio || p == Preset::Stems || p == Preset::PlaylistTracks || p == Preset::TrackGroups) dlg.scope = dlg.scope == 1 ? 0 : dlg.scope;
  dlg.replacePattern = p == Preset::PatternReplace;
}

void openRenderAgain(App& a) {  // J-27: the dialog exactly as last used (formats, scope, splitting)
  initDialog(a);
  dlg.open = true;
  a.showExport = false;
  if (!dlg.fmt[0] && !dlg.fmt[1] && !dlg.fmt[2] && !dlg.fmt[3] && !dlg.fmt[4] && !dlg.s.midi) dlg.fmt[0] = true;
}

void startRenderTo(App& a, const std::string& folder) {
  initDialog(a);
  std::snprintf(dlg.folder, sizeof dlg.folder, "%s", folder.c_str());
  startJob(a);
}

void draw(App& a) {
  if (a.showExport) openRender(a, Preset::Audio);  // Ctrl+R / older entry points
  drawRenderDialog(a);
  drawProgress(a);
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
  const Tick bar = std::max<Tick>(1, s.barTicks());
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

void exportDataFilesDialog(App& a) {
  fileDialog(a, "Export project data files into folder", false, {"/"}, "", [&a](const std::string& dir) {
    captureHostedStates(a);
    const std::string folder = (fs::path(dir) / safeFileName(projectName(a))).string();
    CollectReport r = exportProjectDataFiles(a.doc.song(), folder, safeFileName(projectName(a)));
    if (!r.error.empty())
      a.notify("Export failed: " + r.error, true);
    else
      a.notify("Project + " + std::to_string(r.copied + r.reused) + " file(s) in " + folder +
                   (r.missing.empty() ? std::string() : " (" + std::to_string(r.missing.size()) + " missing)"),
               !r.missing.empty());
    if (r.error.empty() && r.missing.empty()) a.uiSound(uisound::Event::Export);
  });
}

void exportBonesDialog(App& a) {
  fileDialog(a, "Export project bones into folder", false, {"/"}, "", [&a](const std::string& dir) {
    captureHostedStates(a);
    const std::string folder = (fs::path(dir) / (safeFileName(projectName(a)) + " bones")).string();
    BonesReport r = exportProjectBones(a.doc.song(), folder, safeFileName(projectName(a)));
    if (!r.files.error.empty())
      a.notify("Export failed: " + r.files.error, true);
    else {
      a.notify("Bones: " + std::to_string(r.files.copied + r.files.reused) + " samples, " + std::to_string(r.midiFiles) + " MIDI files, " +
               std::to_string(r.presets) + " presets, " + std::to_string(r.chains) + " effect chains");
      a.uiSound(uisound::Event::Export);
    }
  });
}

void exportFlpDialog(App& a) {
  fileDialog(a, "Export FL Studio project", true, {".flp"}, safeFileName(projectName(a)) + ".flp", [&a](const std::string& p) {
    captureHostedStates(a);
    std::string path = p;
    if (fs::path(path).extension().empty()) path += ".flp";
    flp::ExportOptions opt;
    opt.factoryData = flp::defaultFactoryData();
    opt.projectDir = fs::path(path).parent_path().string();
    flp::ExportReport rep;
    std::string err;
    if (!flp::exportFlpFile(a.doc.song(), path, rep, opt, &err)) {
      a.notify("FL Studio export failed: " + err, true);
      return;
    }
    int notExact = 0;
    for (const auto& pl : rep.plugins) notExact += pl.quality != "verbatim" && pl.quality != "exact";
    a.notify("Exported FL Studio project" + (notExact ? " (" + std::to_string(notExact) + " plugin(s) approximated or placeholders; see the log)" : std::string()),
             notExact > 0);
    if (notExact == 0) a.uiSound(uisound::Event::Export);
    for (const auto& wv : rep.warnings) gl::logging::warning("FL export", wv);
    for (const auto& pl : rep.plugins)
      if (pl.quality != "verbatim" && pl.quality != "exact") gl::logging::warning("FL export", pl.where + ": " + pl.glType + " -> " + pl.flName + " [" + pl.quality + "] " + pl.note);
  });
}

void exportZipDialog(App& a) {
  fileDialog(a, "Export zipped project", true, {".zip"}, safeFileName(projectName(a)) + ".zip", [&a](const std::string& p) {
    captureHostedStates(a);
    std::string path = p;
    if (fs::path(path).extension().empty()) path += ".zip";
    CollectReport r = exportZippedProject(a.doc.song(), path, safeFileName(projectName(a)));
    if (!r.error.empty())
      a.notify("Zipped project failed: " + r.error, true);
    else
      a.notify("Zipped project with " + std::to_string(r.copied) + " file(s)" +
                   (r.missing.empty() ? std::string() : " (" + std::to_string(r.missing.size()) + " missing)"),
               !r.missing.empty());
    if (r.error.empty() && r.missing.empty()) a.uiSound(uisound::Event::Export);
  });
}

void openMissingSamples(App& a) {
  missing.open = true;
  missing.found = {};
  missing.searched = false;
  missing.files = missingFiles(a.doc.song());
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
  if (hasExt(path, {".mid", ".midi", ".rmi"})) {
    importMidiFile(a, path);
    return true;
  }
  if (hasExt(path, {".zip"})) {
    std::string proj, err;
    const std::string dir = (fs::path(path).parent_path() / fs::path(path).stem()).string();
    if (!unpackZippedProject(path, dir, proj, &err)) {
      a.notify("Cannot open the zipped project: " + err, true);
      return true;
    }
    a.openProject(proj);
    if (!missingFiles(a.doc.song()).empty()) openMissingSamples(a);
    return true;
  }
  if (isAudioFile(path) && !hasExt(path, {".json", ".flp"})) {
    importAudioAsClip(a, path);
    return true;
  }
  return false;
}

void exportChannelSample(App& a, ObjectId channel) {
  const Channel* c = a.doc.song().channel(channel);
  if (!c || c->plugin.data.empty()) return;
  const std::string src = c->plugin.data;
  writeSampleDialog(a, "Export sample as WAV", c->name, [src](const std::string& path, std::string& err) {
    SamplePtr s = loadSample(src, &err);
    if (!s) return false;
    AudioBuffer b(2, s->frames());
    std::copy(s->left.begin(), s->left.end(), b.plane(0));
    std::copy(s->right.begin(), s->right.end(), b.plane(1));
    AudioFileOptions o;
    o.bits = 24;
    return writeAudioFile(path, b, static_cast<int>(std::lround(s->sampleRate)), o, &err);
  });
}

void exportClipAudio(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c) return;
  const std::string name = c->kind == ClipKind::Audio && s.channel(c->source) ? s.channel(c->source)->name : "clip";
  const int rate = a.audioSampleRate > 0 ? a.audioSampleRate : 44100;
  Song song = s;
  writeSampleDialog(a, "Export clip as WAV", name, [song, clip, rate](const std::string& path, std::string& err) {
    // The clip as it sounds (clip gain, pitch, stretch, fades) through its channel, master neutral.
    const Clip* k = song.clip(clip);
    if (!k) return false;
    Song solo = stemSong(song, song.masterId(), false);
    solo.playlist.erase(std::remove_if(solo.playlist.begin(), solo.playlist.end(),
                                       [&](const Clip& x) { return x.id != clip && x.kind != ClipKind::Automation; }),
                        solo.playlist.end());
    for (auto& t : solo.tracks) t.muted = false;
    for (auto& x : solo.playlist) x.muted = x.id == clip ? false : x.muted;
    RenderJob j;
    j.sampleRate = rate;
    j.scope = RenderScope::Range;
    j.start = k->start;
    j.end = k->start + song.clipLength(*k);
    j.tail = TailMode::Cut;
    RenderResult r = renderAudio(solo, j);
    if (!r.error.empty()) {
      err = r.error;
      return false;
    }
    AudioFileOptions o;
    o.bits = 24;
    return writeAudioFile(path, r.audio, rate, o, &err);
  });
}

}  // namespace gl::exportui

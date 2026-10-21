// Gridline — "edison" pack: editor sessions — loading and saving the edited
// audio (worker thread), undo, transport, recording, the actions behind the
// toolbar and menus, the slot editor entry point and the standalone window
// opened from any sample or audio clip.
#include "../../app/AudioRecordUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/Params.h"
#include "../../core/PluginParams.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace fs = std::filesystem;

ImU32 C(uint32_t rgb, float a) { return skin::hex(rgb, a); }

namespace {

std::map<uint64_t, std::unique_ptr<Session>>& sessions() {
  static std::map<uint64_t, std::unique_ptr<Session>> m;
  return m;
}

uint64_t standaloneKey(ObjectId ch) { return 0xED15000000000000ull ^ static_cast<uint64_t>(ch); }

// Edited audio goes into a neutral "Edited audio" folder next to the project
// (the config folder while unsaved), like "Recorded" and "Rendered". Files
// saved earlier under "Audio/Edison" keep working: projects store their paths.
std::string audioDir(App& a) { return editedDir(a); }

std::string displayName(App& a, const Session& s) {
  if (!s.slot) {
    const Channel* c = a.doc.song().channel(s.channel);
    return c ? c->name : "Sample";
  }
  State st;
  if (const PluginState* ps = slotState(a, s)) decodeState(ps->data, st);
  if (!st.name.empty()) return st.name;
  if (!st.path.empty()) return fs::path(st.path).stem().string();
  return "Audio";
}

// Saves the version (worker thread): WAV 32-bit float, named by a hash of the
// audio and its markers, so every version has its own file and song undo can
// return to any of them. Returns path + hash ("" for an empty editor).
std::pair<std::string, std::string> saveVersion(const Content& c, const std::string& dir, const std::string& name) {
  if (c.frames() == 0) return {"", ""};
  uint64_t h = audioHash(c.a());
  for (const auto& m : c.markers) h = (h ^ static_cast<uint64_t>(m.pos)) * 1099511628211ull;
  for (const auto& g : c.regions) h = (h ^ static_cast<uint64_t>(g.start * 31 + g.end)) * 1099511628211ull;
  h = (h ^ static_cast<uint64_t>(c.loopStart * 7 + c.loopEnd)) * 1099511628211ull;
  const std::string path = renderPath(dir, name, h);
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) writeWavFile(path, c, 32, nullptr);
  publishSample(path, toSample(c.a(), path));
  return {path, hashHex(h)};
}

uint64_t mergeKey(const Session& s) { return s.key ^ 0x0ED150ED150ull; }

// UI thread: points the owner (slot state / channel sample) at the saved version.
void applySaved(App& a, Session& s, const std::string& path, const std::string& hash, const std::string& label) {
  const Content& c = s.cur();
  s.committed = path;
  if (s.slot) {
    const PluginState* ps = slotState(a, s);
    if (!ps) return;
    State old, st;
    decodeState(ps->data, old);
    st.path = path;
    st.hash = hash;
    st.name = old.name.empty() && !path.empty() ? std::string("Audio") : old.name;
    st.markers = c.markers;
    st.regions = c.regions;
    st.loopStart = c.loopStart;
    st.loopEnd = c.loopEnd;
    st.flVersion = old.flVersion;
    st.flExtra = old.flExtra;
    PluginState next = *ps;
    next.data = encodeState(st);
    if (next.data != ps->data) setPluginState(a, s.target, next, label, mergeKey(s));
  } else if (!path.empty()) {
    a.doc.editChannel(s.channel, label, [&](Channel& ch) { ch.plugin.data = path; }, mergeKey(s));
  }
}

void startCommit(App& a, Session& s, const std::string& label);

void launch(App& a, Session& s, const std::string& label, std::function<std::function<void()>()> work) {
  s.job.label = label;
  s.job.started = a.now;
  s.job.fut = std::async(std::launch::async, std::move(work));
}

void startCommit(App& a, Session& s, const std::string& label) {
  if (s.job.active()) {
    s.pendingCommit = true;
    s.commitLabel = label;
    return;
  }
  s.pendingCommit = false;
  Content c = s.cur();
  const std::string dir = audioDir(a), name = displayName(a, s);
  Session* sp = &s;
  App* ap = &a;
  launch(a, s, "Saving", [c, dir, name, sp, ap, label]() -> std::function<void()> {
    auto saved = saveVersion(c, dir, name);
    return [saved, sp, ap, label]() { applySaved(*ap, *sp, saved.first, saved.second, label); };
  });
}

// Loads `path` (worker) into the history; state markers override the file's.
void startLoad(App& a, Session& s, const State& st) {
  Session* sp = &s;
  const std::string path = st.path;
  launch(a, s, "Loading", [sp, st, path]() -> std::function<void()> {
    Content c = makeContent(Audio{});
    std::string err;
    bool ok = path.empty() || loadContent(path, c, &err);
    if (!st.markers.empty() || !st.regions.empty() || st.loopEnd > st.loopStart) {
      c.markers = st.markers;
      c.regions = st.regions;
      c.loopStart = st.loopStart;
      c.loopEnd = st.loopEnd;
    }
    return [sp, c, ok, err, path]() {
      sp->hist.reset(c);
      sp->loadedFrom = path;
      sp->committed = path;
      sp->sel = {};
      sp->cursor = 0;
      zoomAll(*sp);
      if (!ok) say(*sp, "Cannot open " + fs::path(path).filename().string() + (err.empty() ? "" : ": " + err));
    };
  });
}

// Per frame: finishes jobs, follows external state changes (song undo), pumps recording.
void pump(App& a, Session& s) {
  if (s.job.active() && s.job.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    std::function<void()> done = s.job.fut.get();
    s.job.fut = {};
    if (done) done();
    if (s.pendingCommit) startCommit(a, s, s.commitLabel);
  }
  // Follow the owner: a different file (song undo, preset, project load) reloads.
  if (!s.job.active()) {
    State st;
    if (s.slot) {
      if (const PluginState* ps = slotState(a, s)) decodeState(ps->data, st);
    } else if (const Channel* c = a.doc.song().channel(s.channel)) {
      st.path = c->plugin.data;
    }
    if (st.path != s.committed) startLoad(a, s, st);
  }
  // Recording: drain the slot's capture ring into the recorder.
  if (s.slot) {
    if (EdisonFx* fx = slotFx(a, s)) {
      const CaptureRing& ring = fx->capture();
      if (!s.ringStarted) {
        s.ringPos = ring.written();
        s.ringStarted = true;
      }
      static std::vector<float> L(16384), R(16384);
      static std::vector<uint8_t> P(16384);
      for (;;) {
        size_t n = ring.read(s.ringPos, L.size(), L.data(), R.data(), P.data());
        if (n == 0) break;
        s.rec.feed(L.data(), R.data(), P.data(), n);
      }
      s.inMeter = std::max(s.inMeter * 0.85f, fx->inputPeak());
      s.outMeter = std::max(s.outMeter * 0.85f, fx->outputPeak());
      if (s.rec.takeReady() && !s.job.active()) {
        Audio take = s.rec.take(2);
        if (take.frames() > 0) {
          const bool append = param(a, s, EdAppend, 0) >= 0.5 && s.frames() > 0;
          const int64_t at = s.frames();
          Content next = append ? insertAudio(s.cur(), at, take) : makeContent(take);
          applyNow(a, s, append ? "Record (append)" : "Record", next);
          s.sel = append ? Range{at, next.frames()} : Range{};
          zoomAll(s);
          char b[64];
          std::snprintf(b, sizeof b, "Recorded %.2f s", take.seconds());
          say(s, b);
        }
      }
    }
  }
  // Standalone preview playback ends on its own.
  if (s.pvPlaying && !a.engine.previewing()) s.pvPlaying = false;
}

}  // namespace

// ------------------------------------------------------------------ sessions
Session& sessionFor(App& a, const PluginTarget& t) {
  auto& m = sessions();
  auto it = m.find(t.key());
  if (it == m.end()) {
    auto s = std::make_unique<Session>();
    s->key = t.key();
    s->slot = true;
    s->target = t;
    s->committed = "\x01";  // forces the first load
    it = m.emplace(t.key(), std::move(s)).first;
    (void)a;
  }
  return *it->second;
}

EdisonFx* slotFx(App& a, const Session& s) {
  if (!s.slot) return nullptr;
  auto rt = a.builder.effectRuntime(s.target.owner, s.target.slot);
  return rt ? dynamic_cast<EdisonFx*>(rt->fx.get()) : nullptr;
}

const PluginState* slotState(App& a, const Session& s) {
  if (!s.slot) return nullptr;
  const PluginState* ps = s.target.state(a.doc.song());
  return ps && ps->type == kType ? ps : nullptr;
}

double param(App& a, const Session& s, int k, double dflt) {
  const PluginState* ps = slotState(a, s);
  return ps && k < static_cast<int>(ps->params.size()) ? ps->params[static_cast<size_t>(k)] : dflt;
}

void setParam(App& a, const Session& s, int k, double v) {
  if (s.slot) pui::setParamValue(a, s.target, k, v, false);
}

void say(Session& s, const std::string& text) {
  s.status = text;
  s.statusUntil = ImGui::GetTime() + 6.0;
}

Range target(const Session& s) { return rangeOrAll(s.cur(), s.sel); }

Audio& clipboard() {
  static Audio clip;
  return clip;
}

void runEdit(App& a, Session& s, const std::string& label, std::function<Content(const Content&)> op) {
  if (s.job.active()) {
    say(s, "Busy: " + s.job.label);
    return;
  }
  Content c = s.cur();
  const std::string dir = audioDir(a), name = displayName(a, s);
  Session* sp = &s;
  App* ap = &a;
  launch(a, s, label, [c, op, dir, name, sp, ap, label]() -> std::function<void()> {
    Content next = op(c);
    if (next.audio == c.audio && next.markers.size() == c.markers.size() && next.regions.size() == c.regions.size() &&
        next.loopStart == c.loopStart && next.loopEnd == c.loopEnd) {
      bool sameMarkers = true;
      for (size_t i = 0; i < next.markers.size(); ++i) sameMarkers = sameMarkers && next.markers[i].pos == c.markers[i].pos && next.markers[i].name == c.markers[i].name;
      if (sameMarkers) return [sp, label]() { say(*sp, label + ": nothing to change"); };
    }
    auto saved = saveVersion(next, dir, name);
    return [sp, ap, next, saved, label]() {
      sp->hist.push(label, next);
      sp->sel = clampRange(next, sp->sel);
      sp->cursor = std::min(sp->cursor, next.frames());
      if (sp->viewStart + sp->viewLen > static_cast<double>(next.frames()) * 1.05 || sp->viewLen <= 0) zoomAll(*sp);
      applySaved(*ap, *sp, saved.first, saved.second, "Audio editor: " + label);
    };
  });
}

void applyNow(App& a, Session& s, const std::string& label, const Content& next) {
  s.hist.push(label, next);
  startCommit(a, s, "Audio editor: " + label);
}

void undo(App& a, Session& s) {
  if (s.job.active() || !s.hist.canUndo()) return;
  std::string l = s.hist.undoLabel();
  s.hist.undo();
  s.sel = clampRange(s.cur(), s.sel);
  startCommit(a, s, "Audio editor: undo " + l);
  say(s, "Undo: " + l);
}

void redo(App& a, Session& s) {
  if (s.job.active() || !s.hist.canRedo()) return;
  std::string l = s.hist.redoLabel();
  s.hist.redo();
  s.sel = clampRange(s.cur(), s.sel);
  startCommit(a, s, "Audio editor: redo " + l);
  say(s, "Redo: " + l);
}

// ------------------------------------------------------------------ transport
void play(App& a, Session& s) {
  const int64_t n = s.frames();
  if (n == 0) return;
  int64_t from = s.cursor, to = n;
  if (!s.sel.empty()) from = s.sel.a, to = s.sel.b;
  else if (s.cur().hasLoop() && param(a, s, EdLoop, 0) >= 0.5) from = s.cur().loopStart, to = s.cur().loopEnd;
  if (from >= n) from = 0;
  if (EdisonFx* fx = slotFx(a, s); fx && fx->sourceFrames() == n) {
    fx->play(from, to, param(a, s, EdLoop, 0) >= 0.5);
    return;
  }
  // Standalone (or the engine has not picked the new version yet): preview.
  Audio part = extract(s.cur().a(), {from, to});
  s.pvSample = toSample(part, "");
  a.engine.preview(s.pvSample);
  s.pvPlaying = true;
  s.pvT0 = ImGui::GetTime();
  s.pvFrom = from;
  s.pvTo = to;
}

void stopPlay(App& a, Session& s) {
  if (EdisonFx* fx = slotFx(a, s)) fx->stop();
  if (s.pvPlaying) a.engine.preview(nullptr);
  s.pvPlaying = false;
}

bool isPlaying(App& a, Session& s) {
  if (EdisonFx* fx = slotFx(a, s); fx && fx->isPlaying()) return true;
  return s.pvPlaying;
}

double playPos(App& a, Session& s) {
  if (EdisonFx* fx = slotFx(a, s); fx && (fx->isPlaying() || s.scrubbing)) return fx->position();
  if (s.pvPlaying) {
    double p = static_cast<double>(s.pvFrom) + (ImGui::GetTime() - s.pvT0) * s.rate();
    if (p >= static_cast<double>(s.pvTo)) return -1;
    return p;
  }
  return -1;
}

void toggleRecord(App& a, Session& s) {
  if (!s.slot) {
    say(s, "Recording needs the editor in a mixer slot");
    return;
  }
  if (s.rec.armed() || s.rec.recording()) {
    s.rec.stop();
    if (!s.rec.takeReady()) say(s, "Recording disarmed");
    return;
  }
  Recorder::Config c;
  c.mode = static_cast<int>(param(a, s, EdRecMode, RecOnInput));
  c.rate = a.engine.sampleRate();
  c.thresholdDb = param(a, s, EdThreshold, -40);
  c.preRollSec = param(a, s, EdPreRoll, 0.5);
  c.maxSec = recMaxSeconds(static_cast<int>(param(a, s, EdRecMax, 4)));
  s.rec.arm(c);
  static const char* kWhen[] = {"recording", "armed: waiting for signal", "armed: recording while signal", "armed: waiting for playback"};
  say(s, std::string("Record ") + kWhen[std::clamp(c.mode, 0, 3)]);
}

// ------------------------------------------------------------------ actions
void saveAs(App& a, Session& s, FileFormat f) {
  Session* sp = &s;
  a.fileDialog = FileDialogState{};
  a.fileDialog.open = true;
  a.fileDialog.save = true;
  a.fileDialog.title = f == FileFormat::Flac ? "Save audio as FLAC" : "Save audio as WAV";
  a.fileDialog.dir = a.projectPath.empty() ? a.configDir() : fs::path(a.projectPath).parent_path().string();
  a.fileDialog.name = displayName(a, s) + (f == FileFormat::Flac ? ".flac" : ".wav");
  a.fileDialog.exts = {f == FileFormat::Flac ? ".flac" : ".wav"};
  App* ap = &a;
  a.fileDialog.onOk = [sp, ap, f](const std::string& path) {
    std::string err;
    if (exportFile(path, sp->cur(), {0, 0}, f, 24, &err))
      ap->notify("Saved " + fs::path(path).filename().string());
    else
      ap->notify(err, true);
  };
}

void exportSelection(App& a, Session& s) {
  Session* sp = &s;
  App* ap = &a;
  a.fileDialog = FileDialogState{};
  a.fileDialog.open = true;
  a.fileDialog.save = true;
  a.fileDialog.title = "Export selection";
  a.fileDialog.dir = a.projectPath.empty() ? a.configDir() : fs::path(a.projectPath).parent_path().string();
  a.fileDialog.name = displayName(a, s) + " selection.wav";
  a.fileDialog.exts = {".wav", ".flac"};
  const Range r = target(s);
  a.fileDialog.onOk = [sp, ap, r](const std::string& path) {
    const bool flac = fs::path(path).extension() == ".flac";
    std::string err;
    if (exportFile(path, sp->cur(), r, flac ? FileFormat::Flac : FileFormat::Wav, 24, &err))
      ap->notify("Exported " + fs::path(path).filename().string());
    else
      ap->notify(err, true);
  };
}

void openFile(App& a, Session& s) {
  Session* sp = &s;
  App* ap = &a;
  a.fileDialog = FileDialogState{};
  a.fileDialog.open = true;
  a.fileDialog.save = false;
  a.fileDialog.title = "Load audio into the editor";
  a.fileDialog.dir = a.projectPath.empty() ? a.configDir() : fs::path(a.projectPath).parent_path().string();
  a.fileDialog.exts = {".wav", ".flac", ".mp3", ".ogg", ".wv"};
  a.fileDialog.onOk = [sp, ap](const std::string& path) {
    runEdit(*ap, *sp, "Load " + fs::path(path).filename().string(), [path](const Content& c) {
      Content n;
      return loadContent(path, n) ? n : c;
    });
  };
}

namespace {
// The current version's file, saved now if needed (UI thread).
std::string currentFile(App& a, Session& s) {
  if (s.job.active()) return "";
  if (s.committed.empty() || s.committed[0] == '\x01' || !fs::exists(s.committed)) {
    auto saved = saveVersion(s.cur(), audioDir(a), displayName(a, s));
    applySaved(a, s, saved.first, saved.second, "Audio editor: save");
  }
  return s.committed;
}
}  // namespace

void sendToPlaylist(App& a, Session& s) {
  std::string path = currentFile(a, s);
  if (path.empty()) {
    say(s, s.frames() == 0 ? "Nothing to send" : "Busy");
    return;
  }
  const double secs = s.cur().a().seconds();
  a.doc.beginGroup("Send to playlist");
  ObjectId ch = a.addInstrument("gl.sampler", path);
  a.doc.editSong("Send to playlist", [&](Song& song) {
    int track = 0;
    for (const auto& c : song.playlist) track = std::max(track, c.track + 1);
    if (track >= static_cast<int>(song.tracks.size())) track = static_cast<int>(song.tracks.size()) - 1;
    Clip c;
    c.id = song.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch;
    c.track = std::max(0, track);
    c.start = 0;
    c.length = std::max<Tick>(1, static_cast<Tick>(std::llround(secs * song.tempo / 60.0 * song.ppq)));
    song.playlist.push_back(c);
  });
  a.doc.endGroup();
  a.showPlaylist = true;
  say(s, "Sent to the playlist as an audio clip");
}

void sendToSampler(App& a, Session& s, bool slicer) {
  std::string path = currentFile(a, s);
  if (path.empty()) {
    say(s, s.frames() == 0 ? "Nothing to send" : "Busy");
    return;
  }
  a.doc.beginGroup(slicer ? "Send to slicer" : "Send to sampler");
  ObjectId ch = a.addInstrument(slicer ? "gl.slicer" : "gl.sampler", path);
  if (slicer && ch != kNoId) {
    const Content& c = s.cur();
    std::vector<int64_t> starts;
    for (const auto& m : c.markers) starts.push_back(m.pos);
    if (starts.empty() || starts.front() > 0) starts.insert(starts.begin(), 0);
    const double n = static_cast<double>(std::max<int64_t>(1, c.frames()));
    a.doc.editChannel(ch, "", [&](Channel& chn) {
      auto& p = chn.plugin.params;
      if (p.size() > static_cast<size_t>(pp::SlMode)) p[pp::SlMode] = pp::SlModeManual;
      for (int k = 0; k < pp::kMaxSlices; ++k) {
        size_t idx = static_cast<size_t>(pp::slMarker(k));
        if (idx < p.size()) p[idx] = k < static_cast<int>(starts.size()) ? static_cast<float>(static_cast<double>(starts[static_cast<size_t>(k)]) / n) : -1.f;
      }
    });
  }
  a.doc.endGroup();
  a.settingsChannel = ch;
  say(s, slicer ? "Sent to a new slicer channel (markers = slices)" : "Sent to a new sampler channel");
}

void detectSlices(App& a, Session& s, double sensitivity) {
  const Range r = target(s);
  runEdit(a, s, "Detect beats", [r, sensitivity](const Content& c) {
    std::vector<int64_t> on = detectOnsets(c.a(), r, sensitivity);
    if (on.empty() || on.front() > r.a) on.insert(on.begin(), r.a);
    return setSliceMarkers(c, on);
  });
}

void addMarkerAtCursor(App& a, Session& s) {
  if (!s.sel.empty()) {
    char b[24];
    std::snprintf(b, sizeof b, "Region %zu", s.cur().regions.size() + 1);
    applyNow(a, s, "Add region", addRegion(s.cur(), s.sel, b));
  } else {
    char b[24];
    std::snprintf(b, sizeof b, "Marker %zu", s.cur().markers.size() + 1);
    applyNow(a, s, "Add marker", addMarker(s.cur(), s.cursor, b));
  }
}

// ------------------------------------------------------------------ view helpers
void zoomAll(Session& s) {
  s.viewStart = 0;
  s.viewLen = std::max<double>(64.0, static_cast<double>(s.frames()));
}

void zoomTo(Session& s, Range r) {
  if (r.empty()) return zoomAll(s);
  const double pad = r.len() * 0.04;
  s.viewStart = std::max(0.0, static_cast<double>(r.a) - pad);
  s.viewLen = std::max(32.0, static_cast<double>(r.len()) + 2 * pad);
}

void zoomBy(Session& s, double f, double centre) {
  const double n = std::max(64.0, static_cast<double>(s.frames()));
  double len = std::clamp(s.viewLen * f, 16.0, n * 1.02);
  double t = s.viewLen > 0 ? (centre - s.viewStart) / s.viewLen : 0.5;
  s.viewStart = std::clamp(centre - t * len, 0.0, std::max(0.0, n - len));
  s.viewLen = len;
}

std::string timeText(App& a, const Session& s, int64_t frame, bool precise) {
  char b[48];
  const double sec = static_cast<double>(frame) / std::max(1.0, s.rate());
  if (s.barsTime) {
    const Song& song = a.doc.song();
    const double beats = sec * song.tempo / 60.0;
    const int perBar = std::max(1, song.timeSigNum);
    const int bar = static_cast<int>(beats / perBar) + 1, beat = static_cast<int>(std::fmod(beats, perBar)) + 1;
    const int tick = static_cast<int>(std::fmod(beats, 1.0) * song.ppq);
    if (precise)
      std::snprintf(b, sizeof b, "%d:%d:%02d", bar, beat, tick);
    else
      std::snprintf(b, sizeof b, "%d.%d", bar, beat);
  } else {
    const int m = static_cast<int>(sec / 60);
    const double r = sec - m * 60;
    if (precise)
      std::snprintf(b, sizeof b, "%d:%06.3f", m, r);
    else
      std::snprintf(b, sizeof b, "%d:%04.1f", m, r);
  }
  return b;
}

// ------------------------------------------------------------------ entry points
namespace {
// Development / screenshots: GRIDLINE_EDISON_DEMO=1 fills an empty slot editor
// with a generated phrase (hits + a pitched tone + noise floor), markers, a
// region and loop points; GRIDLINE_EDISON_VIEW=spectral opens the spectrogram
// with a brush stroke.
Content demoContent(double rate) {
  Audio au = makeAudio(rate, 2, static_cast<int64_t>(rate * 4));
  uint32_t seed = 11;
  for (int64_t i = 0; i < au.frames(); ++i) {
    const double t = static_cast<double>(i) / rate;
    seed = seed * 1664525u + 1013904223u;
    const double nz = (static_cast<double>(seed >> 8) / 16777216.0 * 2 - 1);
    double beat = std::fmod(t, 0.5), v = 0.003 * nz;
    v += 0.85 * std::exp(-beat * 18) * std::sin(6.2832 * (55 + 90 * std::exp(-beat * 30)) * beat);
    if (std::fmod(t + 0.25, 0.5) < 0.12) v += 0.35 * nz * std::exp(-std::fmod(t + 0.25, 0.5) * 30);
    if (t > 1.0 && t < 3.2) v += 0.22 * std::sin(6.2832 * 440 * t) * std::min(1.0, (t - 1.0) * 8) * std::min(1.0, (3.2 - t) * 8);
    au.L[static_cast<size_t>(i)] = static_cast<float>(v);
    au.R[static_cast<size_t>(i)] = static_cast<float>(v * 0.92 + 0.02 * nz);
  }
  Content c = makeContent(std::move(au));
  std::vector<int64_t> on = detectOnsets(c.a(), {0, c.frames()}, 0.5);
  c = setSliceMarkers(c, on);
  c = addRegion(c, {static_cast<int64_t>(rate * 1.0), static_cast<int64_t>(rate * 3.2)}, "Tone");
  c = setLoop(c, {static_cast<int64_t>(rate * 2.0), static_cast<int64_t>(rate * 3.0)});
  return c;
}

void devDemo(App& a, Session& s) {
  static bool done = false;
  const char* env = std::getenv("GRIDLINE_EDISON_DEMO");
  if (done || !env || !*env || s.job.active() || s.frames() > 0 || s.committed != "") return;
  done = true;
  const double rate = a.engine.sampleRate();
  applyNow(a, s, "Demo audio", demoContent(rate));
  s.sel = {static_cast<int64_t>(rate * 1.5), static_cast<int64_t>(rate * 2.5)};
  s.cursor = s.sel.a;
  zoomAll(s);
  if (const char* v = std::getenv("GRIDLINE_EDISON_VIEW"); v && std::string(v) == "spectral") {
    s.spectral = true;
    s.brushMode = true;
    for (int k = 0; k < 14; ++k)
      s.brush.push_back({static_cast<int64_t>(rate * (1.6 + k * 0.05)), static_cast<int64_t>(rate * (1.65 + k * 0.05)), 380, 500});
  }
}
}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  Session& s = sessionFor(a, t);
  pump(a, s);
  devDemo(a, s);
  const float maxH = ImGui::GetMainViewport()->WorkSize.y - 170.0f;
  drawForm(a, s, std::max(width, 640.0f), maxH);
  return true;
}

// J-8: "record into the audio editor" from the transport's record filter.
int armTransportRecording(App& a, ObjectId track) {
  const MixerTrack* t = a.doc.song().mixerTrack(track);
  if (!t) return -1;
  int slot = -1;
  for (int k = 0; k < kEffectSlots && slot < 0; ++k)
    if (t->slots[static_cast<size_t>(k)].plugin.type == kType) slot = k;
  for (int k = 0; k < kEffectSlots && slot < 0; ++k)
    if (t->slots[static_cast<size_t>(k)].plugin.empty() && a.doc.setSlotPlugin(track, k, kType)) slot = k;
  if (slot < 0) return -1;
  Session& s = sessionFor(a, PluginTarget{track, slot});
  if (s.rec.armed() || s.rec.recording()) return slot;
  Recorder::Config c;
  c.mode = RecOnPlay;
  c.rate = a.engine.sampleRate();
  c.thresholdDb = param(a, s, EdThreshold, -40);
  c.preRollSec = 0;  // the take starts with the transport
  c.maxSec = 0;
  s.rec.arm(c);
  say(s, "Record armed: starts with playback");
  return slot;
}

bool openAudioEditor(App& a, const std::string& path, ObjectId channel) {
  if (path.empty() || channel == kNoId) return false;
  auto& m = sessions();
  const uint64_t k = standaloneKey(channel);
  auto it = m.find(k);
  if (it == m.end()) {
    auto s = std::make_unique<Session>();
    s->key = k;
    s->slot = false;
    s->channel = channel;
    s->committed = "\x01";
    it = m.emplace(k, std::move(s)).first;
  }
  it->second->open = true;
  a.focusWindow = "###edison-standalone-" + std::to_string(channel);
  return true;
}

void drawWindows(App& a) {
  // Development / screenshots: GRIDLINE_EDISON_STANDALONE=1 writes the demo
  // phrase to a file, puts it on a new sampler channel and opens the
  // standalone editor on it (what "Edit sample in audio editor" does).
  static bool devStandalone = false;
  if (const char* env = std::getenv("GRIDLINE_EDISON_STANDALONE"); env && *env && !devStandalone && ImGui::GetFrameCount() > 2) {
    devStandalone = true;
    Content c = demoContent(a.engine.sampleRate());
    const char* cache = std::getenv("GRIDLINE_CACHE_DIR");
    const std::string dir = cache && *cache ? std::string(cache) : (fs::path(a.configDir()) / "Edited audio").string();
    const std::string path = renderPath(dir, "demo phrase", audioHash(c.a()));
    if (writeWavFile(path, c, 24)) {
      ObjectId ch = a.addInstrument("gl.sampler", path);
      openAudioEditor(a, path, ch);
    }
  }
  for (auto& [k, sp] : sessions()) {
    Session& s = *sp;
    if (s.slot) {
      // Keep recording (and finishing jobs) while the slot's window is closed.
      if (!slotState(a, s)) continue;
      const bool shown = a.fxEditor.first == s.target.owner && a.fxEditor.second == s.target.slot;
      if (!shown) pump(a, s);
      continue;
    }
    if (!s.open) continue;
    const Channel* ch = a.doc.song().channel(s.channel);
    if (!ch) {
      s.open = false;
      continue;
    }
    pump(a, s);
    const std::string title = "Audio editor  ·  " + ch->name + "###edison-standalone-" + std::to_string(s.channel);
    ui::NextFloatingWindowSize(1100 + 24);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
      ui::KeepWindowInViewport();
      drawForm(a, s, 1100, vp->WorkSize.y - 120.0f);
    }
    ImGui::End();
    if (!open) {
      stopPlay(a, s);
      s.open = false;
    }
  }
}

}  // namespace gl::packs::edison

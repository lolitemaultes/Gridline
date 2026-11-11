// Gridline — "edison" pack: editor sessions — loading and saving the edited
// audio (worker thread), undo, transport, recording, the actions behind the
// toolbar and menus, the slot editor entry point and the standalone window
// opened from any sample or audio clip.
#include "../../core/PluginBrand.h"
#include "../../core/I18n.h"
#include "../../app/AudioRecordUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <thread>
#include <utility>
#include "../../app/DevSignal.h"
#include "../../app/StemsUi.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/Params.h"
#include "../../core/PluginParams.h"
#include "../../core/TrackReceipt.h"
#include "../../core/I18n.h"
#include "../../../third_party/imgui/imgui_internal.h"
#include "../../engine/KerfMarks.h"
#include "edison_dock.h"
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
  if (!s.file.empty()) return fs::path(s.file).stem().string();
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

// J-219: an edited version names the file it was made from (Track Receipt maps
// its uses back to the original; which part of it is not known after an edit).
void noteOrigin(const std::string& from, const std::string& path) {
  std::error_code ec;
  if (from.empty() || path.empty() || from == path || !fs::is_regular_file(from, ec)) return;
  receipt::Origin o;
  if (receipt::readOrigin(path, o)) return;  // the first source it was made from stays (undo does not rewrite it)
  o.source = from;
  o.rangeKnown = false;
  o.kind = "edit";
  receipt::writeOrigin(path, o);
}

void applySavedNow(App& a, Session& s, const std::string& path, const std::string& hash, const std::string& label);

// UI thread: points the owner (slot state / channel sample) at the saved version. K-141: a save
// that finishes while the mouse is held (an edit elsewhere) is its own step, never part of that
// edit; otherwise it merges with this editor's step as before.
void applySaved(App& a, Session& s, const std::string& path, const std::string& hash, const std::string& label) {
  if (!ImGui::IsAnyMouseDown()) return applySavedNow(a, s, path, hash, label);
  a.doc.beginGroup(label);
  applySavedNow(a, s, path, hash, label);
  a.doc.endGroup(false);
}

void applySavedNow(App& a, Session& s, const std::string& path, const std::string& hash, const std::string& label) {
  const Content& c = s.cur();
  s.committed = path;
  if (!s.file.empty()) {  // a file on its own: the versions stay in Loupe (drag out, send, save)
    noteOrigin(s.file, path);
    return;
  }
  if (!s.slot) {
    if (const Channel* ch = a.doc.song().channel(s.channel)) noteOrigin(kerfmarks::pathOf(ch->plugin.data), path);
  } else if (const PluginState* ps = slotState(a, s)) {
    State old;
    decodeState(ps->data, old);
    noteOrigin(old.path, path);
  }
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
    a.doc.editChannel(s.channel, label, [&](Channel& ch) {
      const size_t nl = ch.plugin.data.find('\n');  // a slicer's region names stay after the path
      ch.plugin.data = nl == std::string::npos ? path : path + ch.plugin.data.substr(nl);
    }, mergeKey(s));
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

// K-145: the live waveform of the take being recorded. Starts with the take (the pre-roll
// included); with "add after the existing audio" it holds the existing audio first, so the
// view shows the result as it will be. Ends when the take becomes the file (pump) or when
// recording stops without one.
void updateLive(App& a, Session& s) {
  const bool live = s.rec.recording() || s.rec.takeReady();
  if (!live) {
    s.fit.end();
    return;
  }
  const double rate = s.rec.config().rate > 0 ? s.rec.config().rate : a.engine.sampleRate();
  if (!s.fit.active) {
    const bool append = param(a, s, EdAppend, 0) >= 0.5 && s.frames() > 0;
    s.live.reset(rate, 2);
    s.liveSeed = 0;
    if (append) {
      const Audio& au = s.cur().a();
      s.live.add(au.L.data(), au.channels == 1 ? au.L.data() : au.R.data(), static_cast<size_t>(au.frames()));
      s.liveSeed = static_cast<size_t>(au.frames());
    }
    s.liveFed = 0;
    // The view's normal scale for a new take: ten seconds across the lanes (after the
    // existing audio when adding to it).
    s.fit.begin(static_cast<double>(s.liveSeed) + rate * 10.0, rate, static_cast<double>(s.liveSeed));
  }
  const size_t n = static_cast<size_t>(std::max<int64_t>(0, s.rec.recordedFrames()));
  if (n < s.liveFed) {  // the recorder dropped frames (cut at its maximum length): add again from a bucket boundary
    const size_t kept = s.live.truncate(s.liveSeed + n);
    s.liveFed = kept > s.liveSeed ? kept - s.liveSeed : 0;
  }
  if (n > s.liveFed) {
    s.live.add(s.rec.liveL() + s.liveFed, s.rec.liveR() + s.liveFed, n - s.liveFed);
    s.liveFed = n;
  }
  s.fit.step(static_cast<double>(s.live.frames()), ImGui::GetIO().DeltaTime);
}

// Development / tests: GRIDLINE_EDISON_SIM=<speed> stands in for the slot's input while the
// recorder is armed (never a microphone): a generated phrase (drum-like hits, a tone that
// swells, a quiet floor), engine rate x frame time x speed frames per frame, so a fixed frame
// time records the same take every run. Returns true while it feeds.
bool devSim(App& a, Session& s) {
  static const double speed = [] {
    const char* v = std::getenv("GRIDLINE_EDISON_SIM");
    return v && *v ? std::max(0.0, std::atof(v)) : 0.0;
  }();
  if (speed <= 0) return false;
  if (!s.rec.armed() && !s.rec.recording()) {
    s.simPos = 0;
    return true;
  }
  const double rate = s.rec.config().rate > 0 ? s.rec.config().rate : a.engine.sampleRate();
  const auto n = static_cast<size_t>(std::clamp(rate * static_cast<double>(ImGui::GetIO().DeltaTime) * speed, 1.0, rate * 120.0));
  static std::vector<float> L, R;
  L.resize(n), R.resize(n);
  for (size_t i = 0; i < n; ++i, ++s.simPos) {
    const double t = static_cast<double>(s.simPos) / rate;
    const double beat = std::fmod(t, 0.5), bar = std::fmod(t, 4.0);
    double v = 0.8 * std::exp(-beat * 16) * std::sin(6.2832 * (50 + 80 * std::exp(-beat * 28)) * beat);
    v += 0.25 * std::sin(6.2832 * 330 * t) * std::sin(3.1416 * bar / 4.0);
    v *= 0.55 + 0.45 * std::sin(6.2832 * t / 23.0);
    const double nz = 0.004 * std::sin(t * 9187.1) * std::sin(t * 3301.7);
    L[i] = static_cast<float>(v + nz);
    R[i] = static_cast<float>(v * 0.9 - nz);
  }
  s.rec.feed(L.data(), R.data(), nullptr, n);
  // GRIDLINE_EDISON_SIM_STOP=<seconds>: the take stops at that length (as the record button would).
  static const double stopAt = [] {
    const char* v = std::getenv("GRIDLINE_EDISON_SIM_STOP");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (stopAt > 0 && s.rec.recording() && static_cast<double>(s.rec.recordedFrames()) >= stopAt * rate) s.rec.stop();
  return true;
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
    } else if (!s.file.empty()) {
      st.path = s.committed == "\x01" ? s.file : s.committed;  // a file on its own: loaded once, then its edits
    } else if (const Channel* c = a.doc.song().channel(s.channel)) {
      st.path = kerfmarks::pathOf(c->plugin.data);  // a slicer's data keeps its region names after the path
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
      const bool sim = devSim(a, s);  // dev: a generated input instead of the slot's
      for (;;) {
        size_t n = ring.read(s.ringPos, L.size(), L.data(), R.data(), P.data());
        if (n == 0) break;
        if (!sim) s.rec.feed(L.data(), R.data(), P.data(), n);
      }
      updateLive(a, s);  // K-145: the take drawn as it records
      s.inMeter = std::max(s.inMeter * 0.85f, fx->inputPeak());
      s.outMeter = std::max(s.outMeter * 0.85f, fx->outputPeak());
      // K-145: the live view reaches the take's end before the take becomes the file, so the
      // last live frame and the normal view show the same take.
      const bool settled = !s.fit.active || s.fit.shown >= static_cast<double>(s.live.frames());
      if (s.rec.takeReady() && !s.job.active() && settled) {
        s.fit.end();  // the take becomes the file: the view's normal state from here
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
  // J-110: also when Stop (or another preview: the browser's) took the preview over.
  if (s.pvPlaying && !a.engine.previewingSample(s.pvSample.get())) s.pvPlaying = false;
  syncPlayRange(a, s);  // a new selection while playing plays at once
  // Scripted runs wait for the last play or jump to be heard ("waitfor loupe-settled").
  if (!s.settleSaid) {
    EdisonFx* fx = slotFx(a, s);
    if (s.settleWant == 0 || !fx || fx->handled() >= s.settleWant) {
      devsig::reach("loupe-settled");
      s.settleSaid = true;
    }
  }
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

bool loopOn(App& a, const Session& s) { return s.slot ? param(a, s, EdLoop, 0) >= 0.5 : s.loop; }

void setLoop(App& a, Session& s, bool on) {
  if (s.slot) setParam(a, s, EdLoop, on ? 1 : 0);
  else s.loop = on;
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
      applySaved(*ap, *sp, saved.first, saved.second, "Loupe: " + label);
    };
  });
}

void applyNow(App& a, Session& s, const std::string& label, const Content& next) {
  s.hist.push(label, next);
  startCommit(a, s, "Loupe: " + label);
}

void undo(App& a, Session& s) {
  if (s.job.active() || !s.hist.canUndo()) return;
  std::string l = s.hist.undoLabel();
  s.hist.undo();
  s.sel = clampRange(s.cur(), s.sel);
  startCommit(a, s, "Loupe: undo " + l);
  say(s, "Undo: " + l);
}

void redo(App& a, Session& s) {
  if (s.job.active() || !s.hist.canRedo()) return;
  std::string l = s.hist.redoLabel();
  s.hist.redo();
  s.sel = clampRange(s.cur(), s.sel);
  startCommit(a, s, "Loupe: redo " + l);
  say(s, "Redo: " + l);
}

// ------------------------------------------------------------------ transport
transport::PlayRange playRange(App& a, const Session& s) {
  const Content& c = s.cur();
  return transport::playRange(s.frames(), s.sel, c.hasLoop(), c.loopStart, c.loopEnd, loopOn(a, s), s.cursor);
}

namespace {
// The slot's player, when it holds the version shown (else the preview plays).
EdisonFx* player(App& a, Session& s) {
  EdisonFx* fx = slotFx(a, s);
  return fx && fx->sourceFrames() == s.frames() ? fx : nullptr;
}

// The preview plays [start, to), then, for a loop, [from, to) over and over with no gap.
void previewFrom(App& a, Session& s, int64_t start, int64_t from, int64_t to, bool loop) {
  const transport::PreviewLayout l = transport::previewLayout(start, from, to, loop);
  Audio part = extract(s.cur().a(), {l.headFrom, l.headTo});
  if (l.tailTo > l.tailFrom) {
    const Audio tail = extract(s.cur().a(), {l.tailFrom, l.tailTo});
    part.L.insert(part.L.end(), tail.L.begin(), tail.L.end());
    part.R.insert(part.R.end(), tail.R.begin(), tail.R.end());
  }
  s.pvSample = toSample(part, "");
  a.engine.preview(s.pvSample, true, l.loopFrom);  // J-112: Loupe's own Play (plugin playback)
  s.pvPlaying = true;
  s.pvT0 = ImGui::GetTime();
  s.pvFrom = l.headFrom;
  s.pvTo = to;
  s.pvLoopFrom = from;
  s.pvLoop = l.loopFrom >= 0;
}
}  // namespace

void play(App& a, Session& s) {
  const int64_t n = s.frames();
  if (n == 0) return;
  const transport::PlayRange r = playRange(a, s);
  s.playFrom = r.from;
  s.playTo = r.to;
  s.playLoop = r.loop;
  s.playStart = r.start;
  s.settleSaid = false;
  s.settleWant = 0;
  if (EdisonFx* fx = player(a, s)) {
    const uint32_t h = fx->handled();
    fx->play(r.from, r.to, r.loop, r.start);
    s.settleWant = h + 1;
    return;
  }
  // Standalone (or the engine has not picked the new version yet): preview.
  previewFrom(a, s, r.start, r.from, r.to, r.loop);
}

void stopPlay(App& a, Session& s) {
  if (EdisonFx* fx = slotFx(a, s)) fx->stop();
  if (s.pvPlaying) a.engine.preview(nullptr);
  s.pvPlaying = false;
}

bool isPlaying(App& a, Session& s) {
  if (EdisonFx* fx = slotFx(a, s); fx && fx->isPlaying()) return true;
  return s.pvPlaying && !s.scrubbing;
}

double playPos(App& a, Session& s) {
  if (EdisonFx* fx = slotFx(a, s); fx && (fx->isPlaying() || s.scrubbing)) return fx->position();
  if (s.scrubbing) return s.scrub.pos;
  if (s.pvPlaying) return transport::previewPosAt(s.pvFrom, s.pvLoop ? s.pvLoopFrom : s.pvFrom, s.pvTo, s.pvLoop, (ImGui::GetTime() - s.pvT0) * s.rate());
  return -1;
}

void syncPlayRange(App& a, Session& s) {
  if (s.dragging || s.scrubbing || s.frames() == 0) return;
  const transport::PlayRange r = playRange(a, s);
  EdisonFx* fx = slotFx(a, s);
  if (!isPlaying(a, s)) {
    if (fx) fx->setRange(r.from, r.to, r.start);  // a following song transport starts at the cursor
    return;
  }
  if (r.from == s.playFrom && r.to == s.playTo && r.loop == s.playLoop) return;
  s.playFrom = r.from;
  s.playTo = r.to;
  s.playLoop = r.loop;
  s.settleSaid = false;
  s.settleWant = 0;
  if (fx && fx->isPlaying()) {
    const uint32_t h = fx->handled();
    fx->setRange(r.from, r.to);
    s.settleWant = h + 1;
    s.playStart = -1;  // the player keeps its place when it is inside the new range
    return;
  }
  // The preview: on from where it is when that is inside the new range, else the new start.
  const double p = playPos(a, s);
  const auto at = static_cast<int64_t>(transport::keepOrJump(p < 0 ? static_cast<double>(r.from) : p, r.from, r.to));
  s.playStart = at;
  previewFrom(a, s, at, r.from, r.to, r.loop);
}

void scrubBegin(App& a, Session& s, double frame) {
  if (s.frames() < 2) return;
  s.scrubbing = true;
  if (EdisonFx* fx = player(a, s)) {
    fx->scrub(true, frame);
    return;
  }
  if (s.pvPlaying) a.engine.preview(nullptr);
  s.pvPlaying = false;
  s.scrub = {};
  s.scrub.pos = std::clamp(frame, 0.0, static_cast<double>(s.frames() - 1));
  s.scrubT = ImGui::GetTime();
}

void scrubMove(App& a, Session& s, double frame) {
  if (!s.scrubbing) return;
  if (EdisonFx* fx = player(a, s)) {
    fx->scrub(true, frame);
    return;
  }
  // Grains through the preview: each one starts where the last one has got to by now.
  const double now = ImGui::GetTime();
  const double dt = now - s.scrubT;
  s.scrubT = now;
  const double rate = s.rate();
  const bool wasAudible = s.pvPlaying;
  s.scrub.step(frame, dt, s.frames(), rate);
  if (!s.scrub.audible(rate)) {
    if (s.pvPlaying) a.engine.preview(nullptr);
    s.pvPlaying = false;
    return;
  }
  const Audio g = transport::scrubGrain(s.cur().a(), s.scrub.pos, s.scrub.vel / rate, static_cast<int64_t>(rate * 0.12), !wasAudible);
  if (g.frames() == 0) return;
  s.pvSample = toSample(g, "");
  a.engine.preview(s.pvSample, true);
  s.pvPlaying = true;
  s.pvT0 = now;
  s.pvFrom = static_cast<int64_t>(s.scrub.pos);
  s.pvTo = s.frames();
  s.pvLoop = false;
}

void scrubEnd(App& a, Session& s) {
  if (!s.scrubbing) return;
  double at = s.scrub.pos;
  if (EdisonFx* fx = player(a, s)) {
    at = fx->position();
    fx->scrub(false, at);
  } else if (s.pvPlaying) {
    a.engine.preview(nullptr);
    s.pvPlaying = false;
  }
  s.scrubbing = false;
  s.cursor = std::clamp<int64_t>(static_cast<int64_t>(std::llround(at)), 0, s.frames());  // the cursor stays where the scrub ended
}

bool autoScroll(App& a) { return a.settings.getBool("loupe.autoScroll"); }
void setAutoScroll(App& a, bool on) { a.settings.set("loupe.autoScroll", on); }

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
    applySaved(a, s, saved.first, saved.second, "Loupe: save");
  }
  return s.committed;
}
}  // namespace

void splitStems(App& a, Session& s) {
  std::string path = currentFile(a, s);
  if (path.empty()) {
    say(s, s.frames() == 0 ? "Nothing to split" : "Busy");
    return;
  }
  stemsui::openForFile(a, path, displayName(a, s));
}

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

std::string dragOutFile(App& a, Session& s) {
  if (s.job.active() || s.frames() == 0) return "";
  const Range sel = clampRange(s.cur(), s.sel);
  if (sel.empty() || (sel.a <= 0 && sel.b >= s.frames())) return currentFile(a, s);
  const std::string whole = currentFile(a, s);  // the version the selection comes from (its origin)
  const Content part = makeContent(extract(s.cur().a(), sel));
  const auto saved = saveVersion(part, audioDir(a), displayName(a, s) + " selection");
  noteOrigin(whole, saved.first);
  return saved.first;
}

void dragOutSource(App& a, Session& s) {
  if (!ImGui::BeginDragDropSource()) return;
  s.dragOutUsed = true;
  if (s.dragOut.empty()) s.dragOut = dragOutFile(a, s);
  if (!s.dragOut.empty()) {
    ImGui::SetDragDropPayload("GL_FILE", s.dragOut.data(), s.dragOut.size());
    const bool part = !clampRange(s.cur(), s.sel).empty() && s.sel.len() < s.frames();
    const std::string label = displayName(a, s) + (part ? std::string(" \xC2\xB7 ") + i18n::tr("selection") : std::string());
    ui::DragPreview(ui::Icon::Wave, label.c_str(), theme::rgb(theme::role::kPaint));
  }
  ImGui::EndDragDropSource();
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
    char b[48];
    std::snprintf(b, sizeof b, "Region %zu", s.cur().regions.size() + 1);
    applyNow(a, s, "Add region", addRegion(s.cur(), s.sel, b));
  } else {
    char b[48];
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

double viewRate(const Session& s) { return s.fit.active ? s.live.rate() : s.rate(); }

std::string timeText(App& a, const Session& s, int64_t frame, bool precise) {
  char b[48];
  const double sec = static_cast<double>(frame) / std::max(1.0, viewRate(s));
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
  // J-205: GRIDLINE_EDISON_TOOL=<1..18> opens that tool dialog (Tool enum order) for screenshots.
  if (const char* v = std::getenv("GRIDLINE_EDISON_TOOL"); v && *v) {
    const int k = std::atoi(v);
    if (k > 0 && k <= static_cast<int>(Tool::Script)) openTool(s, static_cast<Tool>(k));
  }
}

// J-207: GRIDLINE_EDISON_STATE=armed|rec|play|busy|nosel|menu<N> puts the editor in
// that state for screenshots (recording, playing, a running edit, no selection, menu N
// open), once, after the demo audio (if any) has been stored.
void devState(App& a, Session& s) {
  static bool done = false;
  const char* v = std::getenv("GRIDLINE_EDISON_STATE");
  if (done || !v || !*v || !s.slot || s.job.active()) return;
  const char* demo = std::getenv("GRIDLINE_EDISON_DEMO");
  if (demo && *demo && s.frames() == 0) return;
  done = true;
  const double rate = a.engine.sampleRate();
  {
    const std::string st(v);
    if (st == "armed" || st == "rec") {
      Recorder::Config c;
      c.mode = st == "rec" ? RecNow : RecOnInput;
      c.rate = rate;
      s.rec.arm(c);
    } else if (st == "play") {
      play(a, s);
    } else if (st == "busy") {
      runEdit(a, s, "Time stretch", [](const Content& c) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return c;
      });
    } else if (st == "nosel") {
      s.sel = {};
      s.cursor = static_cast<int64_t>(rate * 1.25);
    } else if (st.rfind("menu", 0) == 0) {
      s.devMenu = std::atoi(st.c_str() + 4);
    }
  }
}
}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  Session& s = sessionFor(a, t);
  pump(a, s);
  devDemo(a, s);
  devState(a, s);
  const float maxH = ImGui::GetMainViewport()->WorkSize.y - 170.0f;
  drawForm(a, s, std::max(width, 660.0f), maxH);
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

namespace {
uint64_t& panelKey();  // K-15 (below)
}  // namespace

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
  if (pinned(a)) {  // K-15: pinned into the layout: the panel shows it
    panelKey() = k;
    a.focusWindow = "Loupe";
    return true;
  }
  it->second->open = true;
  a.focusWindow = "###edison-standalone-" + std::to_string(channel);
  return true;
}

namespace {
uint64_t fileKey(const std::string& path) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char ch : path) h = (h ^ ch) * 1099511628211ull;
  return 0xF11E000000000000ull ^ (h & 0x0000FFFFFFFFFFFFull);
}
std::string fileWindowId(uint64_t key) {
  char b[48];
  std::snprintf(b, sizeof b, "###edison-file-%llx", static_cast<unsigned long long>(key));
  return b;
}
}  // namespace

bool openFileInLoupe(App& a, const std::string& path) {
  std::error_code ec;
  if (path.empty() || !fs::is_regular_file(path, ec)) return false;
  auto& m = sessions();
  const uint64_t k = fileKey(path);
  auto it = m.find(k);
  if (it == m.end()) {
    auto s = std::make_unique<Session>();
    s->key = k;
    s->slot = false;
    s->file = path;
    s->committed = "\x01";
    it = m.emplace(k, std::move(s)).first;
  }
  if (pinned(a)) {  // K-15: pinned into the layout: the panel shows it
    panelKey() = k;
    a.focusWindow = "Loupe";
    return true;
  }
  it->second->open = true;
  a.focusWindow = fileWindowId(k);
  return true;
}


// ---------------------------------------------------------------- K-15: pinned into the layout
namespace {

uint64_t& panelKey() {  // the session the panel shows (0 = none chosen)
  static uint64_t k = 0;
  return k;
}
int& pendingSpot() {  // where to dock the panel when it is next drawn (-1 = where it is)
  static int v = -1;
  return v;
}

Session* findSession(uint64_t k) {
  auto it = sessions().find(k);
  return it == sessions().end() ? nullptr : it->second.get();
}

// The session still has something to show (its slot holds Loupe, its channel exists).
bool alive(App& a, const Session& s) {
  if (s.slot) return slotState(a, s) != nullptr;
  return !s.file.empty() || a.doc.song().channel(s.channel) != nullptr;
}

// The panel's session: the one pinned last, else the project's first Loupe effect.
Session* panelSession(App& a) {
  if (Session* s = findSession(panelKey()); s && alive(a, *s)) return s;
  const Song& song = a.doc.song();
  for (const MixerTrack& t : song.mixer)
    for (int k = 0; k < kEffectSlots; ++k)
      if (t.slots[static_cast<size_t>(k)].plugin.type == kType) {
        Session& s = sessionFor(a, PluginTarget{t.id, k});
        panelKey() = s.key;
        return &s;
      }
  return nullptr;
}

ImGuiID dockOf(const char* window) {
  ImGuiWindow* w = ImGui::FindWindowByName(window);
  return w && w->DockIsActive ? w->DockId : 0;
}

// Puts the panel where the menu asked, before it begins (ImGui's docking keeps it there and
// saves it with the layout).
void placePanel(DockSpot spot) {
  ImGuiID node = 0;
  switch (spot) {
    case DockSpot::Tab: node = dockOf("Playlist"); break;
    case DockSpot::Left: node = dockOf("Browser"); break;
    case DockSpot::Right: node = dockOf("Mixer"); break;
    case DockSpot::Below:
      if (const ImGuiID pl = dockOf("Playlist")) {
        ImGuiID up = 0, down = 0;
        ImGui::DockBuilderSplitNode(pl, ImGuiDir_Down, 0.42f, &down, &up);
        node = down;
      }
      break;
    case DockSpot::Floating: break;
  }
  if (node != 0 || spot == DockSpot::Floating) ImGui::SetNextWindowDockID(node, ImGuiCond_Always);
  if (spot == DockSpot::Floating || node == 0) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 sz(std::min(1100.0f, vp->WorkSize.x - 80), std::min(560.0f, vp->WorkSize.y - 140));
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  }
}

void drawPanel(App& a) {
  if (!pinned(a)) return;
  const int spot = std::exchange(pendingSpot(), -1);
  if (spot >= 0) placePanel(static_cast<DockSpot>(spot));
  bool open = true;
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(C(col::PanelLo)));
  const bool vis = ui::BeginDockWindow("Loupe", &open, ImVec2(1100, 560));
  ImGui::PopStyleColor();
  if (vis) {
    Session* s = panelSession(a);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!s) {
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + avail.x, p0.y + avail.y), "Loupe",
                     "Edit a sample in Loupe, or open a Loupe effect: it shows here.");
      ImGui::Dummy(avail);
    } else {
      pump(a, *s);
      devDemo(a, *s);
      devState(a, *s);
      // A dock smaller than the form's minimum scrolls (never squeezed below it).
      const dock::Layout l = dock::layoutFor(avail.x, avail.y);
      const bool scroll = l.formW > avail.x + 0.5f || l.headH + l.waveH + l.pad > avail.y + 0.5f;
      if (scroll)
        ImGui::BeginChild("##loupescroll", avail, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
      const ImVec2 room = scroll ? ImGui::GetContentRegionAvail() : avail;
      dockDev() = {room.x, room.y, l.compact, l.scale, l.formW, l.headH, l.waveH};
      drawFormDocked(a, *s, room);
      if (scroll) ImGui::EndChild();
    }
  }
  ui::EndDockWindow();
  if (!open) unpin(a);
}

}  // namespace

bool pinned(App& a) { return a.settings.getBool("ui.loupePinned"); }

bool inPanel(const Session& s) { return panelKey() == s.key; }

void pinTo(App& a, Session& s, DockSpot spot) {
  a.settings.set("ui.loupePinned", true);
  panelKey() = s.key;
  pendingSpot() = static_cast<int>(spot);
  if (s.slot) {
    if (a.fxEditor == std::make_pair(s.target.owner, s.target.slot)) a.fxEditor = {kNoId, -1};
  } else {
    s.open = false;  // the panel shows it now
  }
  a.focusWindow = "Loupe";
}

void unpin(App& a) {
  a.settings.set("ui.loupePinned", false);
  if (Session* s = findSession(panelKey()); s && alive(a, *s)) {
    if (s->slot) a.fxEditor = {s->target.owner, s->target.slot};
    else s->open = true;
  }
  pendingSpot() = -1;
}

void dockMenuItems(App& a, Session& s) {
  const bool here = pinned(a) && inPanel(s);
  if (ImGui::BeginMenu(here ? i18n::tr("Move in the layout") : i18n::tr("Pin to the layout"))) {
    if (ImGui::MenuItem(i18n::tr("As a tab next to the playlist"))) pinTo(a, s, DockSpot::Tab);
    if (ImGui::MenuItem(i18n::tr("Below the playlist"))) pinTo(a, s, DockSpot::Below);
    if (ImGui::MenuItem(i18n::tr("Left, with the browser"))) pinTo(a, s, DockSpot::Left);
    if (ImGui::MenuItem(i18n::tr("Right, with the mixer"))) pinTo(a, s, DockSpot::Right);
    if (ImGui::MenuItem(i18n::tr("Floating panel"))) pinTo(a, s, DockSpot::Floating);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Unpin into its own window"), nullptr, false, here)) unpin(a);
}

bool claimEffectEditor(App& a, const PluginTarget& t) {
  if (!pinned(a)) return false;
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  Session& s = sessionFor(a, t);
  if (panelKey() != s.key) a.focusWindow = "Loupe";
  panelKey() = s.key;
  a.fxEditor = {kNoId, -1};
  return true;
}

DockDev& dockDev() {
  static DockDev d;
  return d;
}

bool dockDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args) {
  if (cmd != "loupe") return false;
  const std::string what = args.empty() ? "" : args[0];
  if (what == "state") {  // loupe state: every open Loupe (owner, length, cursor, selection, playback, lanes)
    for (auto& [k, sp] : sessions()) {
      Session& s = *sp;
      if (!alive(a, s)) continue;
      const char* kind = s.slot ? "slot" : !s.file.empty() ? "file" : "channel";
      const std::string committed = s.committed.empty() || s.committed[0] == '\x01' ? std::string("-") : fs::path(s.committed).filename().string();
      std::printf("loupe state: key %llx kind %s channel %llu frames %lld cursor %lld sel %lld %lld playing %d pos %.0f range %lld %lld scrubbing %d "
                  "autoscroll %d loop %d start %lld lanes %.1f %.1f %.1f %.1f ruler %.1f %.1f view %.0f %.0f committed %s\n",
                  static_cast<unsigned long long>(k), kind, static_cast<unsigned long long>(s.channel), static_cast<long long>(s.frames()),
                  static_cast<long long>(s.cursor), static_cast<long long>(s.sel.a), static_cast<long long>(s.sel.b), isPlaying(a, s) ? 1 : 0,
                  playPos(a, s), static_cast<long long>(s.playFrom), static_cast<long long>(s.playTo), s.scrubbing ? 1 : 0, autoScroll(a) ? 1 : 0, loopOn(a, s) ? 1 : 0, static_cast<long long>(s.playStart),
                  static_cast<double>(s.devLx0), static_cast<double>(s.devLx1), static_cast<double>(s.devLy0), static_cast<double>(s.devLy1),
                  static_cast<double>(s.devRy0), static_cast<double>(s.devRy1), s.devViewStart, s.devViewLen, committed.c_str());
    }
    std::fflush(stdout);
    return true;
  }
  if (what == "play" || what == "stop" || what == "loop" || what == "follow") {  // loupe play | stop | loop 0|1 | follow 0|1: the first open Loupe
    for (auto& [k, sp] : sessions()) {
      Session& s = *sp;
      if (!alive(a, s)) continue;
      const bool on = args.size() > 1 && args[1] == "1";
      if (what == "play") play(a, s);
      else if (what == "stop") stopPlay(a, s);
      else if (what == "loop") setLoop(a, s, on);
      else setParam(a, s, EdFollowHost, on ? 1 : 0);
      break;
    }
    return true;
  }
  if (what == "file" && args.size() > 1) {  // loupe file <path>: the browser's Edit in Loupe
    std::printf("loupe file: %s\n", openFileInLoupe(a, args[1]) ? "opened" : "not opened");
    std::fflush(stdout);
    return true;
  }
  if (what == "pin") {  // loupe pin tab|below|left|right|float: the slot session (first Loupe effect) or the open one
    Session* s = panelSession(a);
    if (!s)
      for (auto& [k, sp] : sessions())
        if (alive(a, *sp)) s = sp.get();
    if (!s) {
      std::printf("loupe: nothing to pin\n");
    } else {
      const std::string w = args.size() > 1 ? args[1] : "tab";
      pinTo(a, *s, w == "below" ? DockSpot::Below : w == "left" ? DockSpot::Left : w == "right" ? DockSpot::Right : w == "float" ? DockSpot::Floating : DockSpot::Tab);
    }
  } else if (what == "unpin") {
    unpin(a);
  } else {  // loupe: where the panel is and how its form is laid out, to stdout
    ImGuiWindow* w = ImGui::FindWindowByName("Loupe");
    const DockDev& d = dockDev();
    std::printf("loupe: pinned %d shown %d docked %d with \"%s\" pos %.0f,%.0f size %.0fx%.0f room %.0fx%.0f compact %d scale %.2f form %.0f head %.0f wave %.0f fx %d\n",
                pinned(a) ? 1 : 0, w && w->Active ? 1 : 0, w && w->DockIsActive ? 1 : 0,
                [&]() -> const char* {
                  if (!w || !w->DockIsActive || !w->DockNode) return "";
                  for (ImGuiWindow* o : w->DockNode->Windows)
                    if (o != w) return ui::stableWindowName(o->Name);
                  return "";
                }(),
                w ? static_cast<double>(w->Pos.x) : 0.0, w ? static_cast<double>(w->Pos.y) : 0.0, w ? static_cast<double>(w->Size.x) : 0.0,
                w ? static_cast<double>(w->Size.y) : 0.0, static_cast<double>(d.w), static_cast<double>(d.h), d.compact ? 1 : 0,
                static_cast<double>(d.scale), static_cast<double>(d.formW), static_cast<double>(d.headH), static_cast<double>(d.waveH),
                a.fxEditor.first != kNoId ? 1 : 0);
    std::fflush(stdout);
  }
  return true;
}

void drawWindows(App& a) {
  drawPanel(a);  // K-15
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
      const bool shown = (a.fxEditor.first == s.target.owner && a.fxEditor.second == s.target.slot) || (pinned(a) && inPanel(s));
      if (!shown) pump(a, s);
      continue;
    }
    if (!s.open || (pinned(a) && inPanel(s))) continue;
    const Channel* ch = s.file.empty() ? a.doc.song().channel(s.channel) : nullptr;
    if (!ch && s.file.empty()) {
      s.open = false;
      continue;
    }
    pump(a, s);
    const std::string title = s.file.empty()
                                  ? pluginDisplayName(kType) + "  \xC2\xB7  " + ch->name + "###edison-standalone-" + std::to_string(s.channel)
                                  : pluginDisplayName(kType) + "  \xC2\xB7  " + fs::path(s.file).filename().string() + fileWindowId(s.key);
    const int fitFlags = ui::NextFloatingWindowSize(1100 + 24);  // J-158: capped to the work area, the rest scrolls
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | fitFlags)) {
      ui::KeepWindowInViewport();
      drawForm(a, s, std::clamp(std::floor(vp->WorkSize.x - 48.0f), 660.0f, 1100.0f), vp->WorkSize.y - 120.0f);  // J-207: fits the work area
    }
    ImGui::End();
    if (!open) {
      stopPlay(a, s);
      s.open = false;
    }
  }
}

}  // namespace gl::packs::edison

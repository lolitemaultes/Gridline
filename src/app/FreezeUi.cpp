// Gridline - K-87: freezing and hibernating in the app (FreezeUi.h).
#include "FreezeUi.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include "../../third_party/imgui/imgui.h"
#include "../collab/Controller.h"
#include "../core/Crash.h"
#include "../core/I18n.h"
#include "../core/MenuOps.h"
#include "../core/TrackModes.h"
#include "../engine/Sample.h"
#include "../host/HostedPlugins.h"
#include "App.h"
#include "Windows.h"
#include "HostedUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::freezeui {

namespace fs = std::filesystem;
using theme::pal;
using i18n::tr;

namespace {

constexpr float kPi = 3.14159265f;

std::string fmt1(const char* f, const std::string& a) {
  std::string s = f;
  const size_t at = s.find("%s");
  if (at != std::string::npos) s.replace(at, 2, a);
  return s;
}

// ---------------------------------------------------------------- the render worker

struct Job {
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> progress{0};
  std::mutex mu;
  bool ok = false;           // guarded by mu
  FreezeRecord record;       // guarded by mu
  std::string error;         // guarded by mu
  Unit unit;
  bool automatic = false;    // a refreeze of an out-of-date unit (not an undo step)
  uint64_t epoch = 0;
  ~Job() {
    cancel = true;
    if (worker.joinable()) worker.join();
  }
};

struct Request {
  Unit unit;
  bool automatic = false;
};

struct State {
  uint64_t epoch = ~0ull;
  std::deque<Request> queue;
  std::unique_ptr<Job> job;
  // Out of date (frozen units only), checked after edits.
  std::map<Unit, bool> stale;
  uint64_t staleRev = ~0ull;
  double staleAt = -1;
  uint64_t lastRev = ~0ull;
  double lastEdit = -1;       // when the song last changed
  std::set<Unit> frozenHere;  // frozen (or refrozen) on this computer, this session
  std::set<Unit> failed;      // a refreeze failed: not tried again until the next edit
  // Live / awake (the current build's sets) and when each was last needed.
  std::set<Unit> live, awake;
  std::map<Unit, double> needed;
  uint64_t liveNotes = 0;
  double lastScan = -1;
  uint64_t scanRev = ~0ull;
  // The edit guard: what of each frozen unit's plugins was there before an edit.
  std::map<Unit, size_t> digests;
  uint64_t guardRev = ~0ull, guardPush = 0;
  double lastAutoHibernate = 0;
};

State& st() {
  static State s;
  return s;
}

std::string frozenDir(const App& a) {
  if (!a.projectPath.empty()) return (fs::u8path(a.projectPath).parent_path() / "Frozen").u8string();
  return (fs::u8path(a.configDir()) / "Frozen").u8string();
}

double sleepAfter(const App& a) { return a.settings.getNumber("freeze.sleepSeconds"); }  // <= 0: never

bool sharedAndNotMine(const App& a, Unit u) {
  return a.collab && a.collab->active() && !st().frozenHere.count(u);
}

std::vector<Unit> frozenUnits(const Song& s) {
  std::vector<Unit> out;
  for (const auto& c : s.channels)
    if (c.freeze.frozen()) out.push_back(freeze::channelUnit(c.id));
  for (const auto& m : s.mixer)
    if (m.freeze.frozen()) out.push_back(freeze::mixerUnit(m.id));
  return out;
}

void hashPlugin(size_t& h, const PluginState& p) {
  auto mixIn = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
  mixIn(std::hash<std::string>{}(p.type));
  mixIn(std::hash<std::string>{}(p.data));
  mixIn(std::hash<std::string>{}(p.ins));
  for (float v : p.params) mixIn(std::hash<float>{}(v));
}

// The plugins a frozen unit has unloaded (its own and those it covers): an edit of them is blocked.
size_t lockDigest(const Song& s, Unit u) {
  size_t h = u.mixer ? 1 : 2;
  auto channel = [&](ObjectId id) {
    if (const Channel* c = s.channel(id)) {
      hashPlugin(h, c->plugin);
      for (const auto& n : c->noteFx) hashPlugin(h, n);
    }
  };
  auto mixer = [&](ObjectId id) {
    if (const MixerTrack* m = s.mixerTrack(id))
      for (const auto& sl : m->slots) {
        hashPlugin(h, sl.plugin);
        h ^= (sl.enabled ? 3u : 5u) + std::hash<float>{}(sl.mix);
      }
  };
  if (u.mixer) {
    mixer(u.id);
    const freeze::Cover cv = freeze::covered(s, u.id);
    for (ObjectId c : cv.channels) channel(c);
    for (ObjectId m : cv.mixers) mixer(m);
  } else {
    channel(u.id);
  }
  return h;
}

// The frozen unit whose freeze holds `u`'s plugins (u itself, or the frozen track covering it).
bool lockingUnit(const Song& s, Unit u, Unit& out) {
  if (freeze::frozen(s, u)) {
    out = u;
    return true;
  }
  const ObjectId t = freeze::coveringTrack(s, u);
  if (t == kNoId) return false;
  out = freeze::mixerUnit(t);
  return true;
}

// Mixer tracks a channel's sound reaches (its track, aux outputs and their sends, onwards).
std::set<ObjectId> reachedMixers(const Song& s, const Channel& c) {
  std::set<ObjectId> seen;
  std::vector<ObjectId> todo{c.mixerTrack};
  for (ObjectId x : c.auxOuts) todo.push_back(x);
  while (!todo.empty()) {
    const ObjectId id = todo.back();
    todo.pop_back();
    const MixerTrack* m = s.mixerTrack(id);
    if (!m || !seen.insert(id).second) continue;
    for (const auto& sd : m->sends) todo.push_back(sd.to);
  }
  return seen;
}

bool hasNotesIn(const Pattern& p, ObjectId ch) {
  if (auto it = p.notes.find(ch); it != p.notes.end() && !it->second.empty()) return true;
  if (auto it = p.steps.find(ch); it != p.steps.end())
    for (uint8_t v : it->second)
      if (v) return true;
  return false;
}

void prompt(App& a, Unit lock, bool redo, std::function<void()> after) {
  const Song& s = a.doc.song();
  a.confirm = {};
  a.confirm.open = true;
  a.confirm.title = tr("Unfreeze to edit?");
  a.confirm.subtitle =
      fmt1(tr("Its plugins are unloaded while %s is frozen. Unfreezing loads them again; the render stays on disk."), freeze::unitName(s, lock));
  a.confirm.okLabel = tr("Unfreeze");
  a.confirm.action = [&a, lock, redo, after = std::move(after)] {
    if (redo) a.doc.redo();
    unfreezeUnits(a, {lock});
    if (after) after();
  };
}

// ---------------------------------------------------------------- jobs

void startJob(App& a, const Request& r) {
  State& g = st();
  const Song& s = a.doc.song();
  if (!freeze::whyNotFreeze(s, r.unit).empty()) return;
  captureHostedStates(a);
  g.job = std::make_unique<Job>();
  Job& job = *g.job;
  job.unit = r.unit;
  job.automatic = r.automatic;
  job.epoch = a.doc.projectEpoch();
  job.running = true;
  Song song = a.doc.song();
  const int rate = static_cast<int>(std::lround(a.engine.sampleRate()));
  const std::string dir = frozenDir(a);
  crash::crumb("freeze render start");
  Job* jp = &job;
  job.worker = std::thread([jp, song = std::move(song), rate, dir]() {
    host::UiThreadRenderScope scope;  // hosted plugins' main-thread calls come back to the interface
    FreezeRecord rec;
    std::string err;
    bool ok = false;
    try {
      freeze::RenderControl ctl;
      ctl.progress = [jp](double p) { jp->progress = p; };
      ctl.cancel = &jp->cancel;
      ok = freeze::render(song, jp->unit, rate, dir, rec, &err, &ctl);
    } catch (const std::exception& e) {
      crash::reportCurrentException("Freezing");
      err = std::string("The render stopped: ") + e.what();
    } catch (...) {
      crash::reportCurrentException("Freezing");
      err = "An internal error stopped the render.";
    }
    {
      std::lock_guard<std::mutex> lk(jp->mu);
      jp->ok = ok;
      jp->record = std::move(rec);
      jp->error = std::move(err);
    }
    jp->finished = true;
    jp->running = false;
  });
}

void finishJob(App& a) {
  State& g = st();
  if (!g.job || !g.job->finished) return;
  if (g.job->worker.joinable()) g.job->worker.join();
  Job& job = *g.job;
  bool ok;
  FreezeRecord rec;
  std::string err;
  {
    std::lock_guard<std::mutex> lk(job.mu);
    ok = job.ok;
    rec = job.record;
    err = job.error;
  }
  const Unit u = job.unit;
  const bool automatic = job.automatic;
  const bool sameProject = job.epoch == a.doc.projectEpoch();
  g.job.reset();
  if (!sameProject) return;
  const Song& s = a.doc.song();
  const std::string name = freeze::unitName(s, u);
  if (!ok) {
    if (err != "Cancelled") a.notify(fmt1(tr("Could not freeze %s"), name) + (err.empty() ? std::string() : ": " + err), true);
    if (automatic) g.failed.insert(u);
    return;
  }
  if (automatic) {
    // Not an undo step: the record follows the edits (undoing them refreezes again, from the file
    // already on disk).
    if (!freeze::frozen(s, u)) return;  // unfrozen meanwhile
    a.doc.patchSong([&](Song& x) { freeze::setRecord(x, u, rec); });
  } else {
    if (freeze::frozen(s, u) && freeze::record(s, u)->fingerprint == rec.fingerprint) return;
    a.doc.editSong(fmt1(tr("Freeze %s"), name), [&](Song& x) { freeze::setRecord(x, u, rec); });
    a.uiSound(uisound::Event::TaskDone);
  }
  g.frozenHere.insert(u);
  g.failed.erase(u);
  g.staleRev = ~0ull;  // check again
}

bool queued(Unit u) {
  const State& g = st();
  if (g.job && g.job->unit == u) return true;
  for (const auto& r : g.queue)
    if (r.unit == u) return true;
  return false;
}

// ---------------------------------------------------------------- out of date

void checkStale(App& a) {
  State& g = st();
  const uint64_t rev = a.doc.revision();
  if (rev == g.staleRev) return;
  if (a.doc.inPointerGesture()) return;  // not while a control is being dragged
  if (g.staleAt >= 0 && a.now - g.staleAt < 0.25) return;  // at most 4 times a second
  g.staleAt = a.now;
  g.staleRev = rev;
  std::map<Unit, bool> now;
  const Song& s = a.doc.song();
  for (Unit u : frozenUnits(s)) now[u] = !freeze::upToDate(s, u);
  g.stale.swap(now);
}

void autoRefreeze(App& a) {
  State& g = st();
  if (!a.settings.getBool("freeze.autoRefreeze")) return;
  if (g.staleRev != a.doc.revision()) return;  // not checked since the last edit yet
  if (a.doc.inPointerGesture() || g.lastEdit < 0 || a.now - g.lastEdit < 1.0) return;  // a second after the last edit
  for (const auto& [u, out] : g.stale) {
    if (!out || queued(u) || g.failed.count(u) || sharedAndNotMine(a, u)) continue;
    g.queue.push_back({u, true});
  }
}

// ---------------------------------------------------------------- live and awake

void markNeeded(Unit u, double now) { st().needed[u] = now; }

// The reasons a frozen unit plays live now or a hibernating one is awake (other than an out of
// date freeze): pattern / live mode, played by hand, the selected channel, the song about to play
// its notes, an editor open.
void scanNeeds(App& a) {
  State& g = st();
  const Song& s = a.doc.song();
  const double now = a.now;
  bool anyFrozen = false, anyAsleep = false;
  for (const auto& c : s.channels) anyFrozen |= c.freeze.frozen(), anyAsleep |= c.hibernate;
  for (const auto& m : s.mixer) anyFrozen |= m.freeze.frozen(), anyAsleep |= m.hibernate;
  // Played by hand (typing keyboard, MIDI, an editor's keys).
  const uint64_t notes = a.engine.liveNoteCount();
  const bool played = notes != g.liveNotes;
  g.liveNotes = notes;
  if (!anyFrozen && !anyAsleep) return;
  auto needChannel = [&](const Channel& c, bool withTracks) {
    markNeeded(freeze::channelUnit(c.id), now);
    if (withTracks)
      for (ObjectId m : reachedMixers(s, c)) markNeeded(freeze::mixerUnit(m), now);
  };
  if (played)
    if (const Channel* c = s.channel(a.engine.lastLiveNoteChannel())) needChannel(*c, true);
  // Editors open.
  if (const Channel* c = s.channel(a.settingsChannel)) markNeeded(freeze::channelUnit(c->id), now);
  if (a.fxEditor.first != kNoId) markNeeded(freeze::mixerUnit(a.fxEditor.first), now);
  // The selected channel (the typing keyboard plays it).
  if (anyAsleep)
    if (const Channel* c = s.channel(a.selChannel); c && c->hibernate) markNeeded(freeze::channelUnit(c->id), now);
  // Throttled: what the song or the pattern is about to play.
  if (now - g.lastScan < 0.2 && g.scanRev == a.doc.revision()) return;
  g.lastScan = now;
  g.scanRev = a.doc.revision();
  const PlayMode mode = a.engine.mode();
  if (mode == PlayMode::Live) {
    for (Unit u : frozenUnits(s)) markNeeded(u, now);
    for (const auto& c : s.channels)
      if (c.hibernate) needChannel(c, true);
    return;
  }
  if (mode == PlayMode::Pattern) {
    if (const Pattern* p = s.pattern(a.curPattern))
      for (const auto& c : s.channels)
        if (hasNotesIn(*p, c.id)) needChannel(c, true);
    return;
  }
  if (!anyAsleep) return;
  // Song mode: hibernating units wake a little before their notes (4 s ahead while playing; while
  // stopped, those within 2 s of the song position, so play starts with them loaded).
  const double tempo = std::max(10.0, s.tempo);
  const double ticksPerSec = tempo / 60.0 * std::max(1, s.ppq);
  const double pos = a.engine.position();
  const double from = pos - ticksPerSec * 0.5, to = pos + ticksPerSec * (a.engine.playing() ? 4.0 : 2.0);
  std::set<ObjectId> soon;
  for (const auto& clip : s.playlist) {
    if (clip.muted) continue;
    const double c0 = static_cast<double>(clip.start), c1 = c0 + static_cast<double>(s.clipLength(clip));
    if (c1 < from || c0 > to) continue;
    if (clip.kind == ClipKind::Audio) {
      soon.insert(clip.source);
    } else if (clip.kind == ClipKind::Pattern) {
      if (const Pattern* p = s.pattern(clip.pattern))
        for (const auto& [ch, list] : p->notes)
          if (!list.empty()) soon.insert(ch);
      if (const Pattern* p = s.pattern(clip.pattern))
        for (const auto& [ch, v] : p->steps)
          if (hasNotesIn(*p, ch)) soon.insert(ch);
    }
  }
  // Only hibernating units: a frozen one plays its render in song mode.
  for (ObjectId id : soon)
    if (const Channel* c = s.channel(id)) {
      if (c->hibernate) markNeeded(freeze::channelUnit(c->id), now);
      for (ObjectId m : reachedMixers(s, *c))
        if (const MixerTrack* t = s.mixerTrack(m); t && t->hibernate) markNeeded(freeze::mixerUnit(m), now);
    }
}

// The sets the next build should use.
void wantedSets(App& a, std::set<Unit>& live, std::set<Unit>& awake) {
  State& g = st();
  const Song& s = a.doc.song();
  const double keep = sleepAfter(a);
  auto recent = [&](Unit u) {
    auto it = g.needed.find(u);
    if (it == g.needed.end()) return false;
    return keep <= 0 || a.now - it->second <= keep;  // 0: never sleeps again
  };
  const bool replay = a.settings.getBool("freeze.autoRefreeze");
  for (Unit u : frozenUnits(s)) {
    auto it = g.stale.find(u);
    const bool out = it != g.stale.end() && it->second;
    if ((out && replay) || recent(u)) live.insert(u);
  }
  for (const auto& c : s.channels)
    if (c.hibernate && recent(freeze::channelUnit(c.id))) awake.insert(freeze::channelUnit(c.id));
  for (const auto& m : s.mixer)
    if (m.hibernate && recent(freeze::mixerUnit(m.id))) awake.insert(freeze::mixerUnit(m.id));
  // Forget what no longer needs remembering.
  for (auto it = g.needed.begin(); it != g.needed.end();)
    it = keep > 0 && a.now - it->second > keep * 4 + 60 ? g.needed.erase(it) : std::next(it);
}

// ---------------------------------------------------------------- the edit guard

void guardEdits(App& a) {
  State& g = st();
  const Song& s = a.doc.song();
  const uint64_t rev = a.doc.revision(), push = a.doc.pushCount();
  if (rev == g.guardRev) return;
  const bool local = push != g.guardPush && !a.doc.inPointerGesture();
  Unit blocked;
  bool block = false;
  if (local)
    for (const auto& [u, d] : g.digests)
      if (freeze::frozen(s, u) && lockDigest(s, u) != d) {
        blocked = u;
        block = true;
        break;
      }
  if (block) {
    a.doc.undo();
    prompt(a, blocked, true, nullptr);
  }
  g.digests.clear();
  const Song& now = a.doc.song();
  for (Unit u : frozenUnits(now)) g.digests[u] = lockDigest(now, u);
  g.guardRev = a.doc.revision();
  g.guardPush = a.doc.pushCount();
}

void guardEditors(App& a) {
  const Song& s = a.doc.song();
  Unit lock;
  if (a.settingsChannel != kNoId && lockingUnit(s, freeze::channelUnit(a.settingsChannel), lock)) {
    const ObjectId ch = a.settingsChannel;
    a.settingsChannel = kNoId;
    prompt(a, lock, false, [&a, ch] { a.settingsChannel = ch; });
  }
  if (a.fxEditor.first != kNoId && lockingUnit(s, freeze::mixerUnit(a.fxEditor.first), lock)) {
    const auto fx = a.fxEditor;
    a.fxEditor = {kNoId, -1};
    prompt(a, lock, false, [&a, fx] { a.fxEditor = fx; });
  }
}

void autoHibernate(App& a) {
  State& g = st();
  if (!a.settings.getBool("freeze.autoHibernate") || a.now - g.lastAutoHibernate < 5.0) return;
  g.lastAutoHibernate = a.now;
  hibernateUnused(a);
}

bool editorOpen(const App& a, Unit u) {
  if (u.mixer) return a.fxEditor.first == u.id;
  return a.settingsChannel == u.id;
}

}  // namespace

// ---------------------------------------------------------------- per frame

void tick(App& a) {
  State& g = st();
  if (g.epoch != a.doc.projectEpoch()) {  // another project: nothing carries over
    if (g.job) g.job->cancel = true;
    g.job.reset();
    g.queue.clear();
    g.stale.clear();
    g.frozenHere.clear();
    g.failed.clear();
    g.live.clear();
    g.awake.clear();
    g.needed.clear();
    g.digests.clear();
    g.staleRev = g.guardRev = g.lastRev = ~0ull;
    g.epoch = a.doc.projectEpoch();
  }
  if (a.doc.revision() != g.lastRev) {
    g.lastRev = a.doc.revision();
    g.lastEdit = a.now;
    g.failed.clear();
  }
  guardEdits(a);
  guardEditors(a);
  finishJob(a);
  checkStale(a);
  autoRefreeze(a);
  if (!g.job)
    while (!g.queue.empty()) {
      const Request r = g.queue.front();
      g.queue.pop_front();
      if (r.automatic && !freeze::frozen(a.doc.song(), r.unit)) continue;
      startJob(a, r);
      if (g.job) break;
    }
  scanNeeds(a);
  autoHibernate(a);
  std::set<Unit> live, awake;
  wantedSets(a, live, awake);
  if (live != g.live || awake != g.awake) {
    g.live.swap(live);
    g.awake.swap(awake);
    a.forceRebuild();
  }
}

Song playSong(App& a, const Song& s) {
  State& g = st();
  bool any = false;
  for (const auto& c : s.channels) any |= c.freeze.frozen() || c.hibernate;
  for (const auto& m : s.mixer) any |= m.freeze.frozen() || m.hibernate;
  if (!any) {
    a.builder.setFrozenAudio({}, {});
    a.builder.setUnloaded({}, {});
    return s;
  }
  std::set<Unit> live = g.live;
  // A render plays once it is decoded (in the background); until then, and when its file is
  // missing or at another rate than its record says, the unit plays live.
  auto file = [&](const FreezeRecord& r) -> SamplePtr {
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(r.render.data), ec)) return nullptr;
    return sampleCache().request(r.render.data);
  };
  std::map<ObjectId, SamplePtr> chans, mixers;
  for (const auto& m : s.mixer) {
    if (!m.freeze.frozen() || live.count(freeze::mixerUnit(m.id))) continue;
    if (SamplePtr p = file(m.freeze)) mixers[m.id] = p;
    else live.insert(freeze::mixerUnit(m.id));
  }
  for (const auto& c : s.channels) {
    if (!c.freeze.frozen() || live.count(freeze::channelUnit(c.id))) continue;
    if (SamplePtr p = file(c.freeze)) chans[c.id] = p;
    else live.insert(freeze::channelUnit(c.id));
  }
  const freeze::Plan plan = freeze::makePlan(s, live, g.awake);
  for (auto it = chans.begin(); it != chans.end();) it = plan.channelFiles.count(it->first) ? std::next(it) : chans.erase(it);
  for (auto it = mixers.begin(); it != mixers.end();) it = plan.mixerFiles.count(it->first) ? std::next(it) : mixers.erase(it);
  a.builder.setFrozenAudio(std::move(chans), std::move(mixers));
  // Notes played on an unloaded instrument wait for it (the engine plays them once a build loads
  // it); a hibernating one's song notes too (a frozen one's are in its render).
  std::set<ObjectId> asleepChannels;
  for (ObjectId id : plan.unloadChannels)
    if (const Channel* c = s.channel(id); c && c->hibernate && !c->freeze.frozen() && freeze::coveringTrack(s, freeze::channelUnit(id)) == kNoId)
      asleepChannels.insert(id);
  a.builder.setUnloaded(plan.unloadChannels, std::move(asleepChannels));
  return freeze::playView(s, plan);
}

// ---------------------------------------------------------------- actions

void freezeUnits(App& a, const std::vector<Unit>& units) {
  const Song& s = a.doc.song();
  for (Unit u : units) {
    if (freeze::frozen(s, u) || queued(u)) continue;
    if (std::string why = freeze::whyNotFreeze(s, u); !why.empty()) {
      a.notify(fmt1(tr("Cannot freeze %s"), freeze::unitName(s, u)) + ": " + why, true);
      continue;
    }
    if (freeze::hibernating(s, u))  // a frozen unit does not hibernate (its plugins are unloaded already)
      a.doc.patchSong([&](Song& x) { freeze::setHibernate(x, u, false); });
    st().queue.push_back({u, false});
  }
}

void refreezeUnits(App& a, const std::vector<Unit>& units) {
  const Song& s = a.doc.song();
  for (Unit u : units)
    if (freeze::frozen(s, u) && !queued(u)) st().queue.push_back({u, false});
}

void unfreezeUnits(App& a, const std::vector<Unit>& units) {
  State& g = st();
  const Song& s = a.doc.song();
  std::vector<Unit> todo;
  for (Unit u : units) {
    if (g.job && g.job->unit == u) g.job->cancel = true;
    g.queue.erase(std::remove_if(g.queue.begin(), g.queue.end(), [&](const Request& r) { return r.unit == u; }), g.queue.end());
    if (freeze::frozen(s, u)) todo.push_back(u);
  }
  if (todo.empty()) return;
  const std::string label = todo.size() == 1 ? fmt1(tr("Unfreeze %s"), freeze::unitName(s, todo[0])) : std::string(tr("Unfreeze"));
  a.doc.editSong(label, [&](Song& x) {
    for (Unit u : todo) freeze::unfreeze(x, u);
  });
  for (Unit u : todo) g.frozenHere.erase(u);
}

void setHibernate(App& a, const std::vector<Unit>& units, bool on) {
  const Song& s = a.doc.song();
  std::vector<Unit> todo;
  for (Unit u : units) {
    if (freeze::hibernating(s, u) == on) continue;
    if (on && (freeze::frozen(s, u) || !freeze::whyNotHibernate(s, u).empty())) continue;
    todo.push_back(u);
  }
  if (todo.empty()) return;
  // This computer's state, not an undo step (and never shared: collab masks it).
  a.doc.patchSong([&](Song& x) {
    for (Unit u : todo) freeze::setHibernate(x, u, on);
  });
  for (Unit u : todo) {
    if (on && !editorOpen(a, u)) st().needed.erase(u);
    if (!on) st().needed.erase(u);
  }
}

void unfreezeAll(App& a) { unfreezeUnits(a, frozenUnits(a.doc.song())); }

int hibernateUnused(App& a) {
  const Song& s = a.doc.song();
  std::vector<Unit> todo;
  const auto unused = ops::unusedChannels(s);
  std::set<ObjectId> idle(unused.begin(), unused.end());
  for (const auto& c : s.channels) {
    const Unit u = freeze::channelUnit(c.id);
    if (!idle.count(c.id) || c.hibernate || c.freeze.frozen() || c.id == a.selChannel || editorOpen(a, u)) continue;
    if (freeze::whyNotHibernate(s, u).empty()) todo.push_back(u);
  }
  // Mixer tracks with effects that nothing plays into.
  std::set<ObjectId> fed;
  for (const auto& c : s.channels)
    if (!idle.count(c.id))
      for (ObjectId m : reachedMixers(s, c)) fed.insert(m);
  for (const auto& m : s.mixer) {
    const Unit u = freeze::mixerUnit(m.id);
    if (m.id == s.masterId() || fed.count(m.id) || m.hibernate || m.freeze.frozen() || editorOpen(a, u)) continue;
    bool fx = false;
    for (const auto& sl : m.slots) fx |= !sl.plugin.empty();
    if (fx && freeze::whyNotHibernate(s, u).empty()) todo.push_back(u);
  }
  setHibernate(a, todo, true);
  return static_cast<int>(todo.size());
}

void wakeAll(App& a) {
  std::vector<Unit> all;
  for (const auto& c : a.doc.song().channels)
    if (c.hibernate) all.push_back(freeze::channelUnit(c.id));
  for (const auto& m : a.doc.song().mixer)
    if (m.hibernate) all.push_back(freeze::mixerUnit(m.id));
  setHibernate(a, all, false);
}

bool beforeEditorOpens(App& a, const PluginTarget& t) {
  const Song& s = a.doc.song();
  const Unit u = t.isChannel() ? freeze::channelUnit(t.owner) : freeze::mixerUnit(t.owner);
  Unit lock;
  if (lockingUnit(s, u, lock)) {
    prompt(a, lock, false, nullptr);
    return false;
  }
  if (freeze::hibernating(s, u) && !st().awake.count(u)) {  // wakes now: the editor needs the plugin
    markNeeded(u, a.now);
    st().awake.insert(u);
    a.forceRebuild();
    a.syncEngine();
  }
  return true;
}

// ---------------------------------------------------------------- state for the interface

bool rendering(const App& a, Unit u, float* progress) {
  (void)a;
  const State& g = st();
  if (g.job && g.job->unit == u) {
    if (progress) *progress = static_cast<float>(g.job->progress.load());
    return true;
  }
  for (const auto& r : g.queue)
    if (r.unit == u) {
      if (progress) *progress = 0;
      return true;
    }
  return false;
}

bool outOfDate(const App& a, Unit u) {
  if (!freeze::frozen(a.doc.song(), u)) return false;
  auto it = st().stale.find(u);
  return it != st().stale.end() && it->second;
}

bool asleep(const App& a, Unit u) { return freeze::hibernating(a.doc.song(), u) && !st().awake.count(u); }

bool coveredNow(const App& a, Unit u) {
  const ObjectId t = freeze::coveringTrack(a.doc.song(), u);
  return t != kNoId && !st().live.count(freeze::mixerUnit(t));
}

std::string statusText(const App& a, Unit u) {
  const Song& s = a.doc.song();
  if (rendering(a, u)) return freeze::frozen(s, u) ? tr("Frozen, rendering again") : tr("Freezing");
  if (freeze::frozen(s, u)) {
    if (outOfDate(a, u))
      return a.settings.getBool("freeze.autoRefreeze") ? tr("Frozen, out of date: plays live until it is rendered again")
                                                      : tr("Frozen, out of date: plays its old render");
    if (st().live.count(u)) return tr("Frozen, playing live for now");
    return tr("Frozen: plays its render, its plugins are unloaded");
  }
  if (const ObjectId t = freeze::coveringTrack(s, u); t != kNoId)
    return fmt1(tr("Its sound is in the render of %s: its plugins are unloaded"), freeze::unitName(s, freeze::mixerUnit(t)));
  if (freeze::hibernating(s, u)) return asleep(a, u) ? tr("Hibernating: its plugins are unloaded until it is needed") : tr("Hibernating, awake for now");
  return {};
}

bool trackUnit(const Song& s, int track, Unit& out) {
  const TrackMode m = trackmodes::modeOf(s, track);
  if (m == TrackMode::Instrument) {
    if (const Channel* c = trackmodes::linkedChannel(s, track)) {
      out = freeze::channelUnit(c->id);
      return true;
    }
  } else if (m == TrackMode::Audio) {
    if (const MixerTrack* t = trackmodes::linkedMixer(s, track); t && t->id != s.masterId()) {
      out = freeze::mixerUnit(t->id);
      return true;
    }
  }
  return false;
}

bool hasMarker(const App& a, Unit u) {
  const Song& s = a.doc.song();
  return freeze::frozen(s, u) || freeze::hibernating(s, u) || rendering(a, u);
}

bool drawMarker(App& a, ImDrawList* dl, Unit u, ImVec2 c, float size) {
  if (!hasMarker(a, u)) return false;
  const auto& P = pal();
  const Song& s = a.doc.song();
  const ImU32 ice = theme::litRole(theme::role::kPaint);
  const ImU32 amber = theme::litRole(theme::role::kDraw);
  float progress = 0;
  const bool busy = rendering(a, u, &progress);
  if (freeze::frozen(s, u) || busy) {
    const bool liveNow = st().live.count(u) > 0;
    ui::drawIcon(dl, ui::Icon::Snowflake, c, size, busy || liveNow ? theme::withAlpha(ice, 0.55f) : ice);
    if (busy) {  // progress ring
      const float r = size * 0.62f;
      dl->PathArcTo(c, r, -kPi * 0.5f, -kPi * 0.5f + kPi * 2.0f * std::clamp(progress, 0.03f, 1.0f), 24);
      dl->PathStroke(ice, 0, 1.5f);
    } else if (outOfDate(a, u)) {
      dl->AddCircleFilled(ImVec2(c.x + size * 0.42f, c.y - size * 0.42f), std::max(2.0f, size * 0.2f), amber);
    }
    return true;
  }
  ui::drawIcon(dl, ui::Icon::Moon, c, size, asleep(a, u) ? P.textDim : P.hot);
  return true;
}

void drawShade(App& a, ImDrawList* dl, Unit u, ImVec2 p0, ImVec2 p1) {
  if (!coveredNow(a, u)) return;
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, theme::withAlpha(P.surface900, 0.5f), theme::kRadiusSm);
}

// ---------------------------------------------------------------- menus

namespace {

void unitItems(App& a, const std::vector<Unit>& units, bool many) {
  const Song& s = a.doc.song();
  std::vector<Unit> canFreeze, frozenOnes, stale, canSleep, sleeping;
  std::string why;
  for (Unit u : units) {
    if (freeze::frozen(s, u)) {
      frozenOnes.push_back(u);
      if (outOfDate(a, u)) stale.push_back(u);
    } else if (!rendering(a, u)) {
      std::string w = freeze::whyNotFreeze(s, u);
      if (w.empty()) canFreeze.push_back(u);
      else if (why.empty()) why = w;
    }
    if (freeze::hibernating(s, u)) sleeping.push_back(u);
    else if (!freeze::frozen(s, u) && freeze::whyNotHibernate(s, u).empty() && freeze::coveringTrack(s, u) == kNoId) canSleep.push_back(u);
  }
  const bool allFrozen = !units.empty() && frozenOnes.size() == units.size();
  if (allFrozen) {
    // (Each action runs at the start of the next frame: these menus are drawn inside views that hold
    // references into the song while they draw, and an unfreeze replaces the song. App::later.)
    if (ImGui::MenuItem(many ? tr("Unfreeze selected") : tr("Unfreeze"))) a.later([&a, frozenOnes] { unfreezeUnits(a, frozenOnes); });
    devRackItem(many ? "menu unfreeze selected" : "menu unfreeze");
    ui::Hint("Loads its plugins again and plays them live; the render stays on disk");
    if (!stale.empty() && ImGui::MenuItem(tr("Render the freeze again"))) a.later([&a, stale] { refreezeUnits(a, stale); });
  } else {
    if (ImGui::MenuItem(many ? tr("Freeze selected") : tr("Freeze"), nullptr, false, !canFreeze.empty())) a.later([&a, canFreeze] { freezeUnits(a, canFreeze); });
    devRackItem(many ? "menu freeze selected" : "menu freeze");
    if (canFreeze.empty() && !why.empty()) ui::Hint(why);
    else ui::Hint("Renders its sound once and unloads its plugins; volume, pan and sends stay live");
  }
  const bool allSleeping = !units.empty() && sleeping.size() == units.size();
  if (ImGui::MenuItem(many ? tr("Hibernate selected") : tr("Hibernate"), nullptr, allSleeping, allSleeping || !canSleep.empty())) {
    std::vector<Unit> which = allSleeping ? sleeping : canSleep;
    a.later([&a, which, allSleeping] { setHibernate(a, which, !allSleeping); });
  }
  ui::Hint("Unloads its plugins on this computer until they are needed (an editor, a note, the song about to play it)");
}

}  // namespace

void channelMenuItems(App& a, ObjectId channel) { unitItems(a, {freeze::channelUnit(channel)}, false); }

void rackMenuItems(App& a, const std::vector<ObjectId>& channels) {
  std::vector<Unit> us;
  for (ObjectId c : channels) us.push_back(freeze::channelUnit(c));
  unitItems(a, us, true);
}

void mixerMenuItems(App& a, const std::vector<ObjectId>& tracks) {
  std::vector<Unit> us;
  for (ObjectId t : tracks)
    if (t != a.doc.song().masterId()) us.push_back(freeze::mixerUnit(t));
  if (us.empty()) return;
  unitItems(a, us, us.size() > 1);
}

void trackMenuItems(App& a, int track) {
  Unit u;
  if (!trackUnit(a.doc.song(), track, u)) return;
  unitItems(a, {u}, false);
}

void toolsMacroItems(App& a) {
  const Song& s = a.doc.song();
  bool anyFrozen = false, anySleeping = false;
  for (const auto& c : s.channels) anyFrozen |= c.freeze.frozen(), anySleeping |= c.hibernate;
  for (const auto& m : s.mixer) anyFrozen |= m.freeze.frozen(), anySleeping |= m.hibernate;
  if (ImGui::MenuItem(tr("Unfreeze all"), nullptr, false, anyFrozen)) unfreezeAll(a);
  if (ImGui::MenuItem(tr("Hibernate unused plugins"))) {
    const int n = hibernateUnused(a);
    a.notify(n ? "Hibernating " + std::to_string(n) + " unused plugin(s)" : std::string("No unused plugins to hibernate"));
  }
  ui::Hint("Channels without notes and mixer tracks nothing plays into unload their plugins until they are needed");
  if (ImGui::MenuItem(tr("Wake all"), nullptr, false, anySleeping)) wakeAll(a);
}

void optionsItems(App& a) {
  const bool re = a.settings.getBool("freeze.autoRefreeze");
  if (ImGui::MenuItem(tr("Refreeze out-of-date freezes"), nullptr, re)) a.settings.set("freeze.autoRefreeze", !re);
  ui::Hint("On: an edit a freeze depends on plays live and renders again a second later. Off: the old render plays on, marked amber");
  const bool hib = a.settings.getBool("freeze.autoHibernate");
  if (ImGui::MenuItem(tr("Hibernate unused plugins automatically"), nullptr, hib)) a.settings.set("freeze.autoHibernate", !hib);
  if (ImGui::BeginMenu(tr("Woken plugins sleep again after"))) {
    const double cur = sleepAfter(a);
    const struct {
      const char* label;
      double v;
    } opts[] = {{i18n::N_("10 seconds"), 10}, {i18n::N_("30 seconds"), 30}, {i18n::N_("2 minutes"), 120}, {i18n::N_("10 minutes"), 600},
                {i18n::N_("Never"), 0}};
    for (const auto& o : opts)
      if (ImGui::MenuItem(tr(o.label), nullptr, std::fabs(cur - o.v) < 0.5)) a.settings.set("freeze.sleepSeconds", o.v);
    ImGui::EndMenu();
  }
}

void exportItems(App& a) {
  const bool leave = a.settings.getBool("freeze.zipLeaveOut");
  if (ImGui::MenuItem(tr("Leave out frozen audio"), nullptr, leave)) a.settings.set("freeze.zipLeaveOut", !leave);
  ui::Hint("Zipped projects: frozen units go in unfrozen (smaller; their plugins load when it opens)");
}

Song zipSong(const App& a, Song s) {
  if (!a.settings.getBool("freeze.zipLeaveOut")) return s;
  for (auto& c : s.channels) c.freeze = {};
  for (auto& m : s.mixer) m.freeze = {};
  return s;
}

// ---------------------------------------------------------------- dev

bool devCommand(App& a, const std::vector<std::string>& args) {
  if (args.empty()) return false;
  const Song& s = a.doc.song();
  auto unitArg = [&](size_t i, Unit& u) {
    if (args.size() <= i + 1) return false;
    const int k = std::atoi(args[i + 1].c_str());
    if (args[i] == "c" && k >= 0 && k < static_cast<int>(s.channels.size())) u = freeze::channelUnit(s.channels[static_cast<size_t>(k)].id);
    else if (args[i] == "m" && k >= 0 && k < static_cast<int>(s.mixer.size())) u = freeze::mixerUnit(s.mixer[static_cast<size_t>(k)].id);
    else return false;
    return true;
  };
  Unit u;
  if (args[0] == "state") {
    std::printf("FREEZE STATE builds %llu\n", static_cast<unsigned long long>(a.builder.builds()));
    auto line = [&](Unit x) {
      const bool loaded = x.mixer ? [&] {
        const MixerTrack* m = s.mixerTrack(x.id);
        for (size_t k = 0; m && k < m->slots.size(); ++k)
          if (a.builder.effectRuntime(x.id, static_cast<int>(k))) return true;
        return false;
      }()
                                  : [&] {
                                      auto rt = a.builder.channelRuntime(x.id);
                                      const Channel* c = s.channel(x.id);
                                      return rt && c && rt->type == c->plugin.type;
                                    }();
      std::printf("  %s %s frozen %d stale %d live %d hibernate %d awake %d rendering %d loaded %d\n", x.mixer ? "m" : "c",
                  freeze::unitName(s, x).c_str(), freeze::frozen(s, x) ? 1 : 0, outOfDate(a, x) ? 1 : 0, st().live.count(x) ? 1 : 0,
                  freeze::hibernating(s, x) ? 1 : 0, st().awake.count(x) ? 1 : 0, rendering(a, x) ? 1 : 0, loaded ? 1 : 0);
    };
    for (const auto& c : s.channels) line(freeze::channelUnit(c.id));
    for (const auto& m : s.mixer)
      if (m.id != s.masterId()) line(freeze::mixerUnit(m.id));
    std::fflush(stdout);
    return true;
  }
  if (args[0] == "freeze" && unitArg(1, u)) return freezeUnits(a, {u}), true;
  if (args[0] == "unfreeze" && unitArg(1, u)) return unfreezeUnits(a, {u}), true;
  if (args[0] == "hibernate" && unitArg(1, u)) return setHibernate(a, {u}, args.size() < 4 || args[3] != "0"), true;
  if (args[0] == "wait") {  // blocks until the renders are done (scripted runs)
    for (int i = 0; i < 6000 && (st().job || !st().queue.empty()); ++i) {
      if (st().job && st().job->running) host::serveUiThreadTasksWhile(st().job->running);
      finishJob(a);
      if (!st().job && !st().queue.empty()) {
        const Request r = st().queue.front();
        st().queue.pop_front();
        startJob(a, r);
      }
    }
    std::printf("FREEZE WAIT done\n");
    std::fflush(stdout);
    return true;
  }
  return false;
}

}  // namespace gl::freezeui

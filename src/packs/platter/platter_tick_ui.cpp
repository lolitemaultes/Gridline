// Gridline — "platter" pack: per frame, for every setup in the project (its
// editor open or not): parameters the controller moved are written into the
// project (and recorded as automation while Gridline records, unless a pass
// holds them), cues a deck set itself are saved, recorded passes go into the
// pattern (J-159: a performance as notes and lanes, or a take in the setup and a
// note on its key; a pattern that holds one already is kept and a new one is
// made), the controller's LEDs
// follow both decks, and the devices bound to setups are claimed (their notes play no instrument, the app's templates and
// links leave them alone). UI thread.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include "../../app/App.h"
#include "../../app/AutomationEditor.h"
#include "../../app/DevSignal.h"
#include "../../app/Midi.h"
#include "../../app/PluginUI.h"
#include "../../core/I18n.h"
#include "../../core/PluginInfo.h"
#include "../../core/Song.h"
#include "../../core/TrackModes.h"
#include "../../engine/Program.h"
#include "platter.h"

namespace gl::packs::platter {

bool& learningNow() {  // an editor is learning a control this frame
  static bool v = false;
  return v;
}

namespace {

bool gLearning = false;  // last frame's learningNow()

struct DeckState {
  const Generator* gen = nullptr;
  std::array<uint32_t, PCount> mirSeen{};
  std::array<uint32_t, kDecks> marksSeen{};
  TakeBuilder builder;
  PerformanceBuilder perf;  // J-159
  bool perfMode = false;    // the pass being drained is a performance
  bool group = false;       // its undo group is open (the whole pass is one undo step)
  ObjectId passPat = kNoId;  // the pattern it records into
  bool passSong = false;
  double stopAt = -1;  // when the transport was seen stopped with the group open
  std::vector<CaptureEvent> buf;
  std::unordered_map<uint32_t, uint8_t> ledSent;  // (status << 8 | num) -> value sent
  std::string initFor;                             // device the init messages went to
  std::string data, path;
  bool hasModel = false;
  Model m;
};

std::unordered_map<ObjectId, DeckState>& decks() {
  static std::unordered_map<ObjectId, DeckState> d;
  return d;
}

void writeModel(App& a, ObjectId ch, DeckState& d, const Model& m, const char* label, uint64_t mergeKey = 0) {
  const PluginTarget t{ch, -1};
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  PluginState st = *sp;
  st.data = formatData(m.trackOf(0), m);
  setPluginState(a, t, st, i18n::tr(label), mergeKey);
  d.m = m;
  d.data = st.data;
}

bool takeKey(int pitch) { return pitch >= kTakeKeyFirst && pitch < kTakeKeyFirst + kTakeKeys; }

// The pattern a pass records into (J-159): the selected one (or the recorder's) while it
// holds no pass of this setup, else a new one, selected in pattern mode.
ObjectId choosePass(App& a, ObjectId channel, bool song) {
  const ObjectId want = a.recorder.active() ? a.recorder.pattern() : a.curPattern;
  const ObjectId target = passPattern(a.doc, want, channel, song);
  if (!song && target != a.curPattern && a.doc.song().pattern(target)) {
    a.recorder.retarget(target);
    a.selectPattern(target);
  }
  return target;
}

// Dev hooks (GRIDLINE_PLATTER_TRACE, J-159 checks): a line per recorded pass; with
// GRIDLINE_PLATTER_PASS_VIEW the piano roll opens on the pass with a lane, or the playlist.
void devPass(App& a, const Channel& c, ObjectId pattern) {
  devsig::reach("platter-pass");
  static const bool trace = std::getenv("GRIDLINE_PLATTER_TRACE") != nullptr;
  const Pattern* p = a.doc.song().pattern(pattern);
  if (trace && p) {
    size_t notes = 0, lanes = 0, fracs = 0;
    if (auto it = p->notes.find(c.id); it != p->notes.end()) {
      notes = it->second.size();
      for (const Note& n : it->second) fracs += n.frac != 0 || n.endFrac != 0 ? 1 : 0;
    }
    for (const auto& l : p->automation)
      if (l.owner == c.id) {
        ++lanes;
        for (const auto& q : l.points) fracs += q.frac != 0 ? 1 : 0;
      }
    int perfPatterns = 0;
    for (const auto& q : a.doc.song().patterns) perfPatterns += holdsPerformance(q, c.id) ? 1 : 0;
    std::string steps;  // the history's labels up to here
    const auto labels = a.doc.history().labels();
    for (size_t i = 0; i < std::min(labels.size(), a.doc.history().position()); ++i) steps += (i ? " | " : "") + labels[i];
    std::printf("platter: pass pattern \"%s\" notes %zu lanes %zu fractions %zu passes %d patterns %zu selected %d undo %zu [%s]\n", p->name.c_str(),
                notes, lanes, fracs, perfPatterns, a.doc.song().patterns.size(), a.curPattern == pattern ? 1 : 0, a.doc.history().position(),
                steps.c_str());
    std::fflush(stdout);
  }
  if (const char* v = std::getenv("GRIDLINE_PLATTER_PASS_VIEW"); v && *v) {  // "piano:<parameter>" or "playlist"
    a.selectChannel(c.id);
    a.showRack = false;  // (the floating rack would cover the view)
    if (std::string(v).rfind("piano:", 0) == 0) editPatternEvents(a, channelPluginParam(c.id, std::atoi(v + 6)));
    else a.focusWindow = "Playlist";
  }
}

void closePass(App& a, DeckState& d) {
  if (d.group) a.doc.endGroup(false);  // (a pass is its own undo step, whatever the mouse does)
  d.group = false;
  d.passPat = kNoId;
  d.stopAt = -1;
}

// A performance pass starts: one undo step from here to its end; its pattern now (in
// pattern mode the new one plays on from here).
void beginPass(App& a, const Channel& c, DeckState& d) {
  closePass(a, d);
  a.doc.beginGroup(i18n::tr("Record a deck performance"));
  d.group = true;
  d.passSong = a.engine.mode() == PlayMode::Song;
  d.passPat = choosePass(a, c.id, d.passSong);
}

// A performance pass ended: its notes and lanes into the pattern (song mode: through the
// clip at the place it was played, placed when none is there).
void endPass(App& a, const Channel& c, DeckState& d, const Performance& perf) {
  if (d.group && a.doc.song().pattern(d.passPat)) {
    double origin = 0;
    if (d.passSong) {
      const auto at = static_cast<Tick>(std::floor(perf.startTick));
      const auto len = std::max<Tick>(1, static_cast<Tick>(std::ceil(perf.endTick - perf.startTick)));
      origin = static_cast<double>(clipOrigin(a.doc, d.passPat, c.id, at, len));
    }
    writePerformance(a.doc, d.passPat, c.id, origin, perf, d.passSong);
    const ObjectId pat = d.passPat;
    closePass(a, d);
    devPass(a, c, pat);
  }
  closePass(a, d);
}

// A finished take: into the deck, and a note on its key at the place it was played.
void storeTake(App& a, const Channel& c, DeckState& d, Take take) {
  if (!d.hasModel || take.steps() == 0) return;
  const bool song = a.engine.mode() == PlayMode::Song;
  const ObjectId want = a.recorder.active() ? a.recorder.pattern() : a.curPattern;
  if (!a.doc.song().pattern(want)) return;
  const Tick len = std::max<Tick>(1, take.lengthTicks);
  // which take keys stay in use (a pass never replaces another: it goes into a new pattern)
  Model m = d.m;
  std::vector<int> used;
  for (const auto& p : a.doc.song().patterns)
    if (auto it = p.notes.find(c.id); it != p.notes.end())
      for (const Note& n : it->second)
        if (takeKey(n.pitch)) used.push_back(n.pitch);
  m.takes.erase(std::remove_if(m.takes.begin(), m.takes.end(),
                               [&](const Take& x) { return std::find(used.begin(), used.end(), x.key) == used.end(); }),
                m.takes.end());
  const int key = freeTakeKey(m.takes);
  if (key < 0) {
    a.notify(i18n::tr("All 24 take keys of this setup are used: delete a take to record another"), true);
    return;
  }
  int number = 1;
  for (const Take& x : d.m.takes) {
    int k = 0;
    if (std::sscanf(x.name.c_str(), "Take %d", &k) == 1) number = std::max(number, k + 1);
  }
  take.key = key;
  char nm[32];
  std::snprintf(nm, sizeof nm, "Take %d", number);
  take.name = nm;
  // (a stream whose file is gone plays nothing)
  for (auto& x : take.s)
    if (x.on && (x.file < 0 || x.file >= static_cast<int>(m.files.size()))) x = TakeStream{};
  if (take.steps() == 0) return;
  const Tick startTick = take.startTick;
  m.takes.push_back(std::move(take));
  a.doc.beginGroup(i18n::tr("Record a deck take"));
  const ObjectId pattern = choosePass(a, c.id, song);
  const Tick origin = song ? clipOrigin(a.doc, pattern, c.id, startTick, len) : 0;
  const Tick start = std::max<Tick>(0, startTick - origin);
  a.doc.editPattern(pattern, i18n::tr("Record a deck take"), [&](Pattern& p) {
    auto& notes = p.notes[c.id];
    notes.push_back(Note(start, len, key, 100));
    std::stable_sort(notes.begin(), notes.end(), [](const Note& x, const Note& y) { return x.start < y.start; });
  });
  writeModel(a, c.id, d, m, i18n::N_("Record a deck take"));
  a.doc.endGroup(false);
}

bool ledState(int control, const LiveView& v) {
  switch (control) {
    case CtlPlay: return v.playing;
    case CtlCue: return v.cueLit;
    case CtlSync: return v.sync;
    case CtlKeyLock: return v.keylock;
    case CtlSlip: return v.slip;
    case CtlVinyl: return v.vinyl;
    case CtlQuantize: return v.quantize;
    case CtlShift: return v.shift;
    case CtlLoaded: return v.loaded;
    case CtlLoopAuto:
    case CtlLoopIn:
    case CtlLoopOut: return v.loopOn;
    default:
      if (control >= CtlHot1 && control < CtlHot1 + kHotCues) return v.hot[static_cast<size_t>(control - CtlHot1)] >= 0;
      return false;
  }
}

}  // namespace

bool claimMidi(App& a, const std::string& device) {
  (void)a;
  return hub::claimed(hub::deviceId(device)) || gLearning;
}

void tick(App& a) {
  gLearning = learningNow();
  learningNow() = false;
  const Song& song = a.doc.song();
  const PluginInfo* info = findPlugin(kType);
  std::vector<uint32_t> claims;
  auto& all = decks();
  // forget decks that are gone
  for (auto it = all.begin(); it != all.end();) {
    const Channel* c = song.channel(it->first);
    if (c && c->plugin.type == kType) {
      ++it;
      continue;
    }
    closePass(a, it->second);
    it = all.erase(it);
  }
  for (const Channel& c : song.channels) {
    if (c.plugin.type != kType) continue;
    auto rt = a.builder.channelRuntime(c.id);
    if (!rt || !rt->gen) continue;
    Generator* g = rt->gen.get();
    DeckState& d = all[c.id];
    if (c.plugin.data != d.data) {
      d.data = c.plugin.data;
      Model m;
      parseData(c.plugin.data, d.path, m);  // (a bare path is a model with deck A's track)
      d.hasModel = !c.plugin.data.empty();
      d.m = std::move(m);
    }
    const PluginTarget t{c.id, -1};
    Mirror mr;
    mirror(g, mr);
    Marks mk[kDecks];
    for (int k = 0; k < kDecks; ++k) marks(g, k, mk[k]);
    LiveView lvA;
    liveView(g, 0, lvA);
    if (d.gen != g) {  // a new setup instance: start from what it has now
      d.gen = g;
      d.mirSeen = mr.seq;
      for (int k = 0; k < kDecks; ++k) d.marksSeen[static_cast<size_t>(k)] = mk[k].seq;
      d.builder = TakeBuilder{};
      d.perf = PerformanceBuilder{};
      d.perfMode = false;
      closePass(a, d);
      d.ledSent.clear();
      d.initFor.clear();
    }
    // controller moves -> parameters (and automation while recording)
    for (int k = 0; k < PCount; ++k) {
      const auto i = static_cast<size_t>(k);
      if (mr.seq[i] == d.mirSeen[i]) continue;
      d.mirSeen[i] = mr.seq[i];
      // (while a pass captures, the pass holds the move: no automation as well)
      automationRecordHold() = lvA.capturing;
      pui::setParamValue(a, t, k, mr.value[i], true);
      automationRecordHold() = false;
      if (info && a.recorder.active() && !automationRecording() && !lvA.capturing)
        a.recorder.controller(t.pid(k), info->params[i].toNormalized(mr.value[i]), a.engine.position());
    }
    // cues a deck set -> the model
    for (int k = 0; k < kDecks; ++k) {
      const auto ki = static_cast<size_t>(k);
      if (mk[k].seq == d.marksSeen[ki]) continue;
      if (lvA.replaying) continue;  // (a performance replays its own cues: saved once it is over)
      d.marksSeen[ki] = mk[k].seq;
      if (!d.hasModel || d.m.deck[ki].file < 0) continue;
      Model m = d.m;
      DeckModel& dm = m.deck[ki];
      dm.cue = mk[k].cue;
      dm.hot = mk[k].hot;
      dm.loopIn = mk[k].loopIn;
      dm.loopOut = mk[k].loopOut;
      if (!(m == d.m)) writeModel(a, c.id, d, m, i18n::N_("Set a cue on the deck"), (0x9A7710ull + ki) ^ c.id);
    }
    // recorded passes: performances (J-159) and takes
    if (d.buf.empty()) d.buf.resize(4096);
    for (int n; (n = drainCapture(g, d.buf.data(), static_cast<int>(d.buf.size()))) > 0;)
      for (int i = 0; i < n; ++i) {
        const CaptureEvent& e = d.buf[static_cast<size_t>(i)];
        if (e.kind == CaptureEvent::Start) {
          d.perfMode = e.snap[0] > 0;
          if (d.perfMode) beginPass(a, c, d);
        }
        if (!d.perfMode) {
          if (d.builder.feed(e)) storeTake(a, c, d, d.builder.take);
          continue;
        }
        if (d.perf.feed(e)) {
          endPass(a, c, d, d.perf.perf);
        } else if (e.kind == CaptureEvent::End) {
          if (d.perf.lost) a.notify(i18n::tr("A deck performance could not be recorded: the pass was too busy for the recorder"), true);
          closePass(a, d);
        }
      }
    // (a pass whose end never came: its edits stay one undo step)
    if (d.group) {
      if (a.engine.playing()) d.stopAt = -1;
      else if (d.stopAt < 0) d.stopAt = a.now;
      else if (a.now - d.stopAt > 2.0) closePass(a, d);
    }
    // controller: claim it, set it up, light it
    if (!d.hasModel || d.m.map.device.empty() || d.m.map.empty()) continue;
    if (static_cast<int>(claims.size()) < hub::kClaims) claims.push_back(hub::deviceId(d.m.map.device));
    if (!a.midi) continue;
    if (d.initFor != d.m.map.device) {
      d.initFor = d.m.map.device;
      d.ledSent.clear();
      for (const auto& msg : d.m.map.init) a.midi->sendTo(d.m.map.device, std::vector<unsigned char>(msg.begin(), msg.end()));
    }
    if (!d.m.map.feedback) continue;
    LiveView lv[kDecks];
    for (int k = 0; k < kDecks; ++k) liveView(g, k, lv[k]);
    for (const Led& l : d.m.map.leds) {
      const uint8_t v = ledState(l.control, lv[l.deck & 1]) ? l.on : l.off;
      const uint32_t key = static_cast<uint32_t>(l.status) << 8 | l.num;
      auto it = d.ledSent.find(key);
      if (it != d.ledSent.end() && it->second == v) continue;
      d.ledSent[key] = v;
      a.midi->sendTo(d.m.map.device, {l.status, l.num, v});
    }
  }
  hub::setClaimed(claims.data(), static_cast<int>(claims.size()));
}

}  // namespace gl::packs::platter

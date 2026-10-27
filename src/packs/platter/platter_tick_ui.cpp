// Gridline — "platter" pack: per frame, for every deck in the project (its
// editor open or not): parameters the controller moved are written into the
// project (and recorded as automation while Gridline records), cues the deck set
// itself are saved, recorded takes go into the deck and a note on the take's key
// into the pattern, the controller's LEDs follow the deck, and the devices bound
// to decks are claimed (their notes play no instrument, the app's templates and
// links leave them alone). UI thread.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include "../../app/App.h"
#include "../../app/AutomationEditor.h"
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
  uint32_t marksSeen = 0;
  TakeBuilder builder;
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
  st.data = formatData(d.path, m);
  setPluginState(a, t, st, i18n::tr(label), mergeKey);
  d.m = m;
  d.data = st.data;
}

// Song mode: the pattern-tick origin of the clip of `pattern` at song tick `tick` (a clip is placed when none covers it).
Tick songOrigin(App& a, ObjectId pattern, ObjectId channel, Tick tick, Tick length) {
  const Song& s = a.doc.song();
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Pattern && c.pattern == pattern && tick >= c.start && tick < c.start + s.clipLength(c)) return c.start - c.offset;
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const Tick start = tick / bar * bar;
  const Tick len = std::max<Tick>(bar, (tick + length - start + bar - 1) / bar * bar);
  int track = trackmodes::instrumentTrackOf(s, channel);
  if (track < 0) {
    for (track = 0; track < static_cast<int>(s.tracks.size()); ++track) {
      if (s.tracks[static_cast<size_t>(track)].mode != TrackMode::Unassigned) continue;
      bool busy = false;
      for (const auto& c : s.playlist)
        if (c.track == track && c.start < start + len && c.start + s.clipLength(c) > start) busy = true;
      if (!busy) break;
    }
  }
  if (track >= kMaxPlaylistTracks) track = 0;
  a.doc.addClip(pattern, track, start);
  return start;
}

bool takeKey(int pitch) { return pitch >= kTakeKeyFirst && pitch < kTakeKeyFirst + kTakeKeys; }

// A finished take: into the deck, and a note on its key at the place it was played.
void storeTake(App& a, const Channel& c, DeckState& d, Take take) {
  if (!d.hasModel || take.pts.empty()) return;
  const bool song = a.engine.mode() == PlayMode::Song;
  const ObjectId pattern = a.recorder.active() ? a.recorder.pattern() : a.curPattern;
  if (!a.doc.song().pattern(pattern)) return;
  const Tick len = std::max<Tick>(1, take.lengthTicks);
  const Tick origin = song ? songOrigin(a, pattern, c.id, take.startTick, len) : 0;
  const Tick start = std::max<Tick>(0, take.startTick - origin);
  // which take keys stay in use (a new pass over the same place replaces the old one)
  Model m = d.m;
  std::vector<int> used;
  for (const auto& p : a.doc.song().patterns)
    if (auto it = p.notes.find(c.id); it != p.notes.end())
      for (const Note& n : it->second) {
        if (!takeKey(n.pitch)) continue;
        const bool replaced = p.id == pattern && n.start < start + len && n.start + n.length > start;
        if (!replaced) used.push_back(n.pitch);
      }
  m.takes.erase(std::remove_if(m.takes.begin(), m.takes.end(),
                               [&](const Take& x) { return std::find(used.begin(), used.end(), x.key) == used.end(); }),
                m.takes.end());
  const int key = freeTakeKey(m.takes);
  if (key < 0) {
    a.notify(i18n::tr("All 24 take keys of this deck are used: delete a take to record another"), true);
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
  m.takes.push_back(std::move(take));
  a.doc.editPattern(pattern, i18n::tr("Record a deck take"), [&](Pattern& p) {
    auto& notes = p.notes[c.id];
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [&](const Note& n) { return takeKey(n.pitch) && n.start < start + len && n.start + n.length > start; }),
                notes.end());
    notes.push_back(Note(start, len, key, 100));
    std::sort(notes.begin(), notes.end(), [](const Note& x, const Note& y) { return x.start < y.start; });
  });
  writeModel(a, c.id, d, m, i18n::N_("Record a deck take"));
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
    it = c && c->plugin.type == kType ? std::next(it) : all.erase(it);
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
      d.hasModel = parseData(c.plugin.data, d.path, m);
      d.m = d.hasModel ? std::move(m) : Model{};
    }
    const PluginTarget t{c.id, -1};
    Mirror mr;
    mirror(g, mr);
    Marks mk;
    marks(g, mk);
    if (d.gen != g) {  // a new deck instance: start from what it has now
      d.gen = g;
      d.mirSeen = mr.seq;
      d.marksSeen = mk.seq;
      d.builder = TakeBuilder{};
      d.ledSent.clear();
      d.initFor.clear();
    }
    // controller moves -> parameters (and automation while recording)
    for (int k = 0; k < PCount; ++k) {
      const auto i = static_cast<size_t>(k);
      if (mr.seq[i] == d.mirSeen[i]) continue;
      d.mirSeen[i] = mr.seq[i];
      pui::setParamValue(a, t, k, mr.value[i], true);
      if (info && a.recorder.active() && !automationRecording())
        a.recorder.controller(t.pid(k), info->params[i].toNormalized(mr.value[i]), a.engine.position());
    }
    // cues the deck set -> the model
    if (mk.seq != d.marksSeen) {
      d.marksSeen = mk.seq;
      if (d.hasModel) {
        Model m = d.m;
        m.cue = mk.cue;
        m.hot = mk.hot;
        m.loopIn = mk.loopIn;
        m.loopOut = mk.loopOut;
        if (!(m == d.m)) writeModel(a, c.id, d, m, i18n::N_("Set a cue on the deck"), 0x9A7710ull ^ c.id);
      }
    }
    // takes
    if (d.buf.empty()) d.buf.resize(4096);
    for (int n; (n = drainCapture(g, d.buf.data(), static_cast<int>(d.buf.size()))) > 0;)
      for (int i = 0; i < n; ++i)
        if (d.builder.feed(d.buf[static_cast<size_t>(i)])) storeTake(a, c, d, d.builder.take);
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
    LiveView lv;
    if (!liveView(g, lv)) continue;
    for (const Led& l : d.m.map.leds) {
      const uint8_t v = ledState(l.control, lv) ? l.on : l.off;
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

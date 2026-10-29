// Gridline — "platter" pack: recorded performances (J-159, J-163). UI thread, no audio.
// (In the core library with the model and the runtime, so the tests use it too.)
//
// J-163: a pass of the setup, captured on the audio thread step by step, becomes automation:
// one lane per control it used. Each deck's state is a lane the deck follows wherever it
// plays: position (the record's place, exact), play (the motor), wheel touch, mouse hand,
// loop (on, in, out), reverse and pitch bend; beside them the hand's travel (wheel, mouse),
// nudges and every control that moved (faders, EQ, crossfader ...). Song mode: automation
// clips where the pass was played, in one clip group; pattern mode: lanes in the pass's
// pattern. The position lane keeps the read head's path: a point wherever the path bends
// (a steady run of the motor is one straight line), a jump as two points on one tick.
//
// J-159's form (still built, for projects recorded with it):
//   start   a note on the start key: the decks go back to how they were when the pass
//           began (the state is kept in the setup, the note's mod X / Y name it);
//   notes   the buttons on the command keys (play, cue, hot cues, loops, beat jumps,
//           censor, bends, the wheel touch, the mouse hand), for as long as they were held;
//   lanes   every parameter that moved (faders, EQ, filter, pitch, crossfader, modes ...)
//           and the performance lanes of each deck: record position (seeks), mouse travel,
//           wheel travel and nudge.
// Nothing is thinned or rounded: a lane holds one point wherever the value the decks used
// changed, half a step before the step it acted on, and a note sits where its button acted
// (sub-tick fractions kept), so playing the pattern back gives the decks the same values
// on the same samples as the pass.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include "../../core/Document.h"
#include "../../core/Params.h"
#include "../../core/PatternAutomation.h"
#include "../../core/PluginInfo.h"
#include "../../core/TrackModes.h"
#include "../../core/Types.h"
#include "platter.h"

namespace gl::packs::platter {

namespace {

bool takeKeyNote(int pitch) { return pitch >= kTakeKeyFirst && pitch < kTakeKeyFirst + kTakeKeys; }
bool perfNote(int pitch) {
  int i = -1;
  return takeKeyNote(pitch) || pitch == kStartKey || cmdKeyOf(pitch, i) >= 0;
}
bool platterLane(const PatternAutomation& l, ObjectId channel) {
  return l.owner == channel && l.index >= kChannelPluginBase && l.index < kChannelPluginBase + static_cast<uint32_t>(PCount);
}
// Splits an exact tick into a whole tick and its fraction.
void splitTick(double t, Tick& whole, float& frac) {
  t = std::max(0.0, t);
  double w = std::floor(t);
  double f = t - w;
  if (f >= 1.0) w += 1.0, f = 0.0;
  whole = static_cast<Tick>(w);
  frac = std::min(static_cast<float>(f), 0.99999994f);
}
// A parameter's default, normalized (J-163: a state lane that starts off is left out).
double normDefault(int p) {
  const PluginInfo* info = findPlugin(kType);
  if (!info || p < 0 || p >= static_cast<int>(info->params.size())) return 0.0;
  const ParamDesc& d = info->params[static_cast<size_t>(p)];
  return d.toNormalized(d.def);
}

}  // namespace

const PerfLane* Performance::lane(int param) const {
  for (const auto& l : lanes)
    if (l.param == param) return &l;
  return nullptr;
}
const PerfLane* Performance::clip(int param) const {
  for (const auto& l : clips)
    if (l.param == param) return &l;
  return nullptr;
}

// ------------------------------------------------------------------ builder
bool PerformanceBuilder::feed(const CaptureEvent& e) {
  switch (e.kind) {
    case CaptureEvent::Start:
      active = e.snap[0] > 0;  // (a pass recorded as a take has no start state)
      lost = false;
      perf = Performance{};
      for (auto& m : moves_) m.clear();
      for (auto& p : path_) p.clear();
      btns_.clear();
      if (!active) return false;
      for (int d = 0; d < kDecks; ++d) {
        startPos_[d] = e.pt[d].from;  // (where the read head is: a jump waiting is the first step's)
        if (!((e.streams >> d) & 1u)) continue;
        perf.decks |= 1 << d;
        perf.scale[static_cast<size_t>(d)] = PosScale(e.frames[d], e.srcRate[d]);
      }
      tick0_ = e.snap[1];
      // (a pass starts on a beat: a whole tick, give or take the clock's rounding)
      if (std::fabs(tick0_ - std::round(tick0_)) < 1e-6) tick0_ = std::round(tick0_);
      tps_ = e.sampleRate > 0 ? static_cast<double>(kStep) * e.tempo * e.ppq / (60.0 * e.sampleRate) : 0.0;
      snapN_ = static_cast<size_t>(e.snap[0]);
      perf.start.assign(snapN_, 0.0);
      perf.startTick = tick0_;
      return false;
    case CaptureEvent::Snap:
      if (active)
        for (size_t j = 0; j < static_cast<size_t>(kSnapPer); ++j) {
          const size_t i = static_cast<size_t>(e.step) * kSnapPer + j;
          if (i < snapN_) perf.start[i] = e.snap[j];
        }
      return false;
    case CaptureEvent::Move:
      if (active && e.param < PCount) moves_[e.param].push_back(Mv{e.step, e.value, e.flag});
      return false;
    case CaptureEvent::Point:
      if (active)
        for (int d = 0; d < kDecks; ++d)
          if ((perf.decks >> d) & 1) path_[d].push_back(Pt{e.pt[d].pos, e.pt[d].from, e.pt[d].flags});
      return false;
    case CaptureEvent::Button:
      if (active) btns_.push_back(Btn{e.step, e.deck, e.param, e.value > 0.5f});
      return false;
    case CaptureEvent::Lost:  // an end after dropped events: no performance
      if (active) lost = true;
      active = false;
      return false;
    case CaptureEvent::End:
      if (!active) return false;
      active = false;
      steps_ = e.step;
      {
        double end = e.tick > tick0_ ? e.tick : tick0_ + static_cast<double>(steps_) * tps_;  // (where the decks were when it ended)
        if (std::fabs(end - std::round(end)) < 1e-6) end = std::round(end);
        build(end);
        buildClips(end);
      }
      return true;
    default: return false;
  }
}

void PerformanceBuilder::build(double endTick) {
  perf.endTick = endTick;
  // Where step s acts: its start; what acts on it sits half a step before (the start note
  // and the first step's buttons on the pass's first sample, so playing from there plays them).
  const double sample = tps_ / kStep;
  const double startOn = tick0_;
  auto at = [&](uint32_t s) { return s == 0 ? startOn : tick0_ + (static_cast<double>(s) - 0.5) * tps_; };
  auto pointAt = [&](uint32_t s) { return s == 0 ? std::max(0.0, tick0_ - 0.5 * tps_) : at(s); };
  // lanes: parameters that changed during the pass (and the performance lanes that did)
  for (int p = 0; p < PCount; ++p) {
    const auto& mv = moves_[static_cast<size_t>(p)];
    if (mv.size() < 2 || stateParam(p)) continue;  // (J-163's state lanes are not J-159's)
    PerfLane l;
    l.param = p;
    for (const Mv& m : mv) l.points.push_back(PerfPoint{pointAt(m.step), m.value});
    perf.lanes.push_back(std::move(l));
  }
  // notes: the start, then each button from its press to its release
  PerfNote st;
  st.key = kStartKey;
  st.on = startOn;
  st.off = std::max(endTick, startOn + 1.0);
  perf.notes.push_back(st);
  std::stable_sort(btns_.begin(), btns_.end(), [](const Btn& a, const Btn& b) { return a.step < b.step; });
  std::vector<int> open(static_cast<size_t>(kDecks * kCmdKeys), -1);
  for (const Btn& b : btns_) {
    if (b.deck >= kDecks || b.key >= kCmdKeys) continue;
    int& o = open[static_cast<size_t>(b.deck * kCmdKeys + b.key)];
    const int pitch = cmdKeyPitch(b.deck, b.key);
    if (b.down) {
      if (o >= 0) perf.notes[static_cast<size_t>(o)].off = at(b.step);  // (no release seen: it ends here)
      PerfNote n;
      n.key = pitch;
      n.on = at(b.step);
      n.off = endTick;
      o = static_cast<int>(perf.notes.size());
      perf.notes.push_back(n);
    } else if (o >= 0) {
      perf.notes[static_cast<size_t>(o)].off = at(b.step);
      o = -1;
    } else {
      // held from before the pass: a note from the start, marked (its press is in the start state)
      PerfNote n;
      n.key = pitch;
      n.on = startOn;
      n.off = at(b.step);
      n.held = true;
      perf.notes.push_back(n);
    }
  }
  for (auto& n : perf.notes) n.off = std::max(n.off, n.on + 0.25 * sample);
}

namespace {

// The read head's path as few points as it takes: from each point the run goes on while one
// straight line from it passes within `tol` of every step on the way (a steady run of the motor
// is exactly straight: one line; a scratch keeps a point every few steps).
struct Node {
  double s, v, tol;
};
std::vector<size_t> simplify(const std::vector<Node>& n) {
  std::vector<size_t> keep{0};
  size_t a = 0;
  while (a + 1 < n.size()) {
    double lo = -1e300, hi = 1e300;
    size_t best = a + 1;
    for (size_t e = a + 1; e < n.size(); ++e) {
      const double ds = n[e].s - n[a].s;
      const double slope = (n[e].v - n[a].v) / ds;
      if (slope >= lo && slope <= hi) best = e;  // (a line to it passes every step between)
      lo = std::max(lo, (n[e].v - n[e].tol - n[a].v) / ds);
      hi = std::min(hi, (n[e].v + n[e].tol - n[a].v) / ds);
      if (lo > hi) break;
    }
    keep.push_back(best);
    a = best;
  }
  return keep;
}

}  // namespace

void PerformanceBuilder::buildClips(double endTick) {
  (void)endTick;
  // Step s's lane points: the pass's first step on its start, the others half a step before
  // theirs (a lane read on the step's own sample gives the step its value).
  const double h = 0.5 * tps_;
  auto at = [&](uint32_t s) { return s == 0 ? tick0_ : tick0_ + static_cast<double>(s) * tps_ - h; };
  for (int p = 0; p < PCount; ++p)
    if (!moves_[static_cast<size_t>(p)].empty()) perf.first[static_cast<size_t>(p)] = moves_[static_cast<size_t>(p)].front().value;
  auto holdLane = [&](int p) {
    PerfLane l;
    l.param = p;
    for (const Mv& m : moves_[static_cast<size_t>(p)]) l.points.push_back(PerfPoint{at(m.step), m.value, m.flag ? kInstantTension : 0.0f, AutoShape::Hold});
    return l;
  };
  auto changed = [&](int p) { return moves_[static_cast<size_t>(p)].size() >= 2; };
  auto offAtStart = [&](int p) {
    const auto& mv = moves_[static_cast<size_t>(p)];
    return mv.empty() || std::fabs(mv.front().value - normDefault(p)) < 1e-9;
  };
  for (int d = 0; d < kDecks; ++d) {
    if (!((perf.decks >> d) & 1)) continue;
    const PosScale& sc = perf.scale[static_cast<size_t>(d)];
    // position: the path in runs between jumps
    {
      PerfLane l;
      l.param = deckParam(d, DTrack);
      const auto& path = path_[d];
      std::vector<Node> run;
      bool firstRun = true;
      auto flush = [&]() {
        if (run.empty()) return;
        const std::vector<size_t> keep = simplify(run);
        for (size_t i = 0; i < keep.size(); ++i) {
          const Node& q = run[keep[i]];
          PerfPoint pp{at(static_cast<uint32_t>(q.s)), sc.toN(q.v), 0.0f, AutoShape::Curve};
          if (i == 0 && !firstRun) pp.shape = AutoShape::Hold;  // (a jump: on the step, after the run before it)
          l.points.push_back(pp);
        }
        firstRun = false;
        run.clear();
      };
      constexpr double kTolFree = 1.0 / 256, kTolHand = 1.0 / 256;  // frames
      double prev = startPos_[d];
      run.push_back(Node{0, prev, kTolFree});
      for (size_t s = 0; s < path.size(); ++s) {
        const Pt& q = path[s];
        if (q.flags & TfJump) {
          flush();
          run.push_back(Node{static_cast<double>(s), q.from, kTolFree});
        }
        run.push_back(Node{static_cast<double>(s + 1), q.pos, (q.flags & TfHand) ? kTolHand : kTolFree});
        prev = q.pos;
      }
      flush();
      (void)prev;
      perf.first[static_cast<size_t>(l.param)] = sc.toN(startPos_[d]);
      perf.clips.push_back(std::move(l));
    }
    const int pPlay = deckParam(d, DPlay);
    if (!moves_[static_cast<size_t>(pPlay)].empty()) perf.clips.push_back(holdLane(pPlay));
    // the hands: the wheel (its touch, its travel), the mouse (its hold, its travel), nudges
    for (int dp : {static_cast<int>(DTouch), static_cast<int>(DWheel), static_cast<int>(DHand), static_cast<int>(DMouse), static_cast<int>(DNudge)}) {
      const int p = deckParam(d, dp);
      const bool state = stateParam(p);
      if (changed(p) || (state && !offAtStart(p))) perf.clips.push_back(holdLane(p));
    }
    // the loop (its points with it), reverse, bend
    const int pLoop = deckParam(d, DLoop);
    if (changed(pLoop) || !offAtStart(pLoop)) {
      perf.clips.push_back(holdLane(pLoop));
      for (int dp : {static_cast<int>(DLoopIn), static_cast<int>(DLoopOut)})
        if (!moves_[static_cast<size_t>(deckParam(d, dp))].empty()) perf.clips.push_back(holdLane(deckParam(d, dp)));
    }
    for (int dp : {static_cast<int>(DReverse), static_cast<int>(DBend)}) {
      const int p = deckParam(d, dp);
      if (changed(p) || !offAtStart(p)) perf.clips.push_back(holdLane(p));
    }
  }
  // the controls that moved (faders, EQ, filter, pitch, crossfader ...), mixer ones last
  for (int pass = 0; pass < 3; ++pass)
    for (int p = 0; p < PCount; ++p) {
      if (perfParam(p) || !changed(p)) continue;
      int dp = -1;
      const int deck = deckOfParam(p, dp);
      if (deck != (pass < 2 ? pass : -1)) continue;
      perf.clips.push_back(holdLane(p));
    }
}

void performanceNotes(const Performance& perf, double origin, int startId, std::vector<Note>& out) {
  for (const PerfNote& pn : perf.notes) {
    Note n;
    float fr = 0;
    splitTick(pn.on - origin, n.start, fr);
    n.frac = fr;
    const double off = std::max(0.0, pn.off - origin);
    const auto end = static_cast<Tick>(std::floor(off));
    n.length = std::max<Tick>(1, end - n.start);
    n.endFrac = std::clamp(static_cast<float>(off - static_cast<double>(n.start + n.length)), -0.99999994f, 0.99999994f);
    n.pitch = pn.key;
    n.velocity = 100;
    if (pn.key == kStartKey) {
      n.modX = startId % 256;
      n.modY = startId / 256;
    } else if (pn.held) {
      n.modY = kHeldModY;
    }
    out.push_back(n);
  }
}

std::vector<AutoPoint> performancePoints(const PerfLane& lane, double origin) {
  std::vector<AutoPoint> pts;
  pts.reserve(lane.points.size());
  for (const PerfPoint& q : lane.points) {
    AutoPoint p;
    splitTick(q.tick - origin, p.pos, p.frac);
    p.value = static_cast<float>(std::clamp(q.value, 0.0, 1.0));
    p.tension = 0;
    p.shape = AutoShape::Hold;
    pts.push_back(p);
  }
  return pts;
}

std::vector<AutoPoint> passPoints(const PerfLane& lane, double origin, double vmin, double vmax) {
  std::vector<AutoPoint> pts;
  pts.reserve(lane.points.size());
  const double span = vmax - vmin;
  for (const PerfPoint& q : lane.points) {
    AutoPoint p;
    splitTick(q.tick - origin, p.pos, p.frac);
    // (exact: value + fine; a zoomed position lane's range is a power of two on a coarse grid)
    const double v = std::clamp(span > 0 ? (q.value - vmin) / span : 0.0, 0.0, 1.0);
    p.value = static_cast<float>(v);
    p.fine = v - static_cast<double>(p.value);
    p.tension = q.tension;
    p.shape = q.shape;
    pts.push_back(p);
  }
  return pts;
}

std::string laneName(const std::string& channel, int param) {
  const PluginInfo* info = findPlugin(kType);
  std::string n = info && param >= 0 && param < static_cast<int>(info->params.size()) ? info->params[static_cast<size_t>(param)].name : "";
  return n.empty() ? channel : channel + " " + n;
}

// ------------------------------------------------------------------ the pattern side
bool holdsPerformance(const Pattern& p, ObjectId channel) {
  if (auto it = p.notes.find(channel); it != p.notes.end())
    for (const Note& n : it->second)
      if (perfNote(n.pitch)) return true;
  for (const auto& l : p.automation)
    if (platterLane(l, channel) && !l.points.empty()) return true;
  return false;
}

ObjectId passPattern(Document& doc, ObjectId want, ObjectId channel, bool song) {
  const Song& s = doc.song();
  const Pattern* src = s.pattern(want);
  const Channel* ch = s.channel(channel);
  if (!ch) return want;
  if (src && !holdsPerformance(*src, channel)) return want;
  if (static_cast<int>(s.patterns.size()) >= kMaxPatterns) return want;
  // "<channel> take N": N counts the channel's performances
  int n = 1;
  for (const auto& p : s.patterns) n += holdsPerformance(p, channel) ? 1 : 0;
  auto taken = [&](const std::string& nm) {
    for (const auto& p : s.patterns)
      if (p.name == nm) return true;
    return false;
  };
  std::string name;
  for (;; ++n) {
    name = ch->name + " take " + std::to_string(n);
    if (!taken(name)) break;
  }
  const Pattern base = src ? *src : Pattern{};
  const int index = src ? s.patternIndex(want) + 1 : -1;
  const ObjectId id = doc.addPattern(name, song || !src ? 1 : base.bars, src ? base.stepsPerBar : 16, index);
  if (id == kNoId) return want;
  if (!src) return id;
  doc.editPattern(id, "new pattern for a deck performance", [&](Pattern& p) {
    p.color = base.color;
    if (song) return;
    p.steps = base.steps;
    p.stepProps = base.stepProps;
    p.stepLoop = base.stepLoop;
    p.loop = base.loop;
    p.notes = base.notes;
    if (auto it = p.notes.find(channel); it != p.notes.end()) {
      auto& v = it->second;
      v.erase(std::remove_if(v.begin(), v.end(), [](const Note& x) { return perfNote(x.pitch); }), v.end());
      if (v.empty()) p.notes.erase(it);
    }
    for (const auto& l : base.automation)
      if (!platterLane(l, channel)) p.automation.push_back(l);
  });
  return id;
}

Tick clipOrigin(Document& doc, ObjectId pattern, ObjectId channel, Tick tick, Tick length) {
  const Song& s = doc.song();
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Pattern && c.pattern == pattern && tick >= c.start && tick < c.start + s.clipLength(c)) return c.start - c.offset;
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const Tick start = tick / bar * bar;
  const Tick len = std::max<Tick>(bar, (tick + length - start + bar - 1) / bar * bar);
  auto busy = [&](int t) {
    for (const auto& c : s.playlist)
      if (c.track == t && c.start < start + len && c.start + s.clipLength(c) > start) return true;
    return false;
  };
  int track = trackmodes::instrumentTrackOf(s, channel);
  if (track >= 0 && busy(track)) track = -1;
  if (track < 0) {
    for (track = 0; track < static_cast<int>(s.tracks.size()); ++track) {
      if (s.tracks[static_cast<size_t>(track)].mode != TrackMode::Unassigned) continue;
      if (!busy(track)) break;
    }
  }
  if (track >= kMaxPlaylistTracks || track >= static_cast<int>(s.tracks.size())) track = 0;
  doc.addClip(pattern, track, start);
  return start;
}

void writePerformance(Document& doc, ObjectId pattern, ObjectId channel, double origin, const Performance& perf, bool song) {
  const Song& s = doc.song();
  const Channel* ch = s.channel(channel);
  if (!s.pattern(pattern) || !ch) return;
  // the start state: a new id in the setup's model (states no start note uses any more go)
  std::set<int> used;
  for (const auto& p : s.patterns)
    if (auto it = p.notes.find(channel); it != p.notes.end())
      for (const Note& n : it->second)
        if (n.pitch == kStartKey) used.insert(startIdOf(n.modX, n.modY));
  int id = 0;
  {
    std::string path;
    Model m;
    if (parseData(ch->plugin.data, path, m)) {
      int next = 1;
      for (const auto& st : m.starts) next = std::max(next, st.id + 1);
      if (next == startIdOf(kNoteModCentre, kNoteModCentre)) ++next;
      if (next > 65535) next = 1;
      id = next;
      doc.editChannel(channel, "Record a deck performance", [&](Channel& c) {
        std::string p0;
        Model mm;
        if (!parseData(c.plugin.data, p0, mm)) return;
        mm.starts.erase(
            std::remove_if(mm.starts.begin(), mm.starts.end(), [&](const PerfStart& q) { return !used.count(q.id) || q.id == id; }),
            mm.starts.end());
        PerfStart ps;
        ps.id = id;
        ps.v = perf.start;
        mm.starts.push_back(std::move(ps));
        c.plugin.data = formatData(p0, mm);
      });
    }
  }
  const Tick bar = std::max<Tick>(1, s.barTicks());
  doc.editPattern(pattern, "Record a deck performance", [&](Pattern& p) {
    auto& notes = p.notes[channel];
    performanceNotes(perf, origin, id, notes);
    std::stable_sort(notes.begin(), notes.end(), [](const Note& x, const Note& y) {
      return static_cast<double>(x.start) + static_cast<double>(x.frac) < static_cast<double>(y.start) + static_cast<double>(y.frac);
    });
    for (const PerfLane& l : perf.lanes)
      setPatternLane(p, channel, kChannelPluginBase + static_cast<uint32_t>(l.param), performancePoints(l, origin));
    if (song) {
      const auto end = static_cast<Tick>(std::ceil(perf.endTick - origin));
      const Tick need = (end + bar - 1) / bar;
      p.bars = static_cast<int>(std::clamp<Tick>(std::max<Tick>(p.bars, need), 1, kMaxPatternBars));
    }
  });
}

// ------------------------------------------------------------------ J-163: a pass as automation
namespace {

uint32_t laneIndex(int param) { return kChannelPluginBase + static_cast<uint32_t>(param); }
bool positionLane(uint32_t index) { return index == laneIndex(PTrackA) || index == laneIndex(PTrackB); }

// The start states the channel's passes still name: J-159 start notes, J-163 position lanes.
std::set<int> startsInUse(const Song& s, ObjectId channel) {
  std::set<int> used;
  auto lane = [&](uint32_t index, const std::vector<AutoPoint>& pts) {
    if (positionLane(index) && !pts.empty())
      if (const int id = startIdOfTag(pts.front().tension); id > 0) used.insert(id);
  };
  for (const auto& p : s.patterns) {
    if (auto it = p.notes.find(channel); it != p.notes.end())
      for (const Note& n : it->second)
        if (n.pitch == kStartKey) used.insert(startIdOf(n.modX, n.modY));
    for (const auto& l : p.automation)
      if (l.owner == channel) lane(l.index, l.points);
  }
  for (const auto& a : s.automations)
    if (a.owner == channel) lane(a.index, a.points);
  return used;
}

// An earlier clip (or a pattern clip's lane) of `channel` plays parameter `param` somewhere in [a, b).
bool laneDrives(const Song& s, ObjectId channel, int param, Tick a, Tick b) {
  const uint32_t index = laneIndex(param);
  for (const auto& c : s.playlist) {
    if (c.muted || c.start >= b || c.start + s.clipLength(c) <= a) continue;
    if (c.kind == ClipKind::Automation) {
      const AutomationClip* ac = s.automation(c.source);
      if (ac && ac->owner == channel && ac->index == index && !ac->points.empty()) return true;
    } else if (c.kind == ClipKind::Pattern) {
      const Pattern* p = s.pattern(c.pattern);
      const PatternAutomation* l = p ? findPatternLane(*p, channel, index) : nullptr;
      if (l && !l->points.empty()) return true;
    }
  }
  return false;
}

// The colour of a deck's clips (mixer: -1).
Rgb laneColor(int deck) { return deck == 0 ? 0x4FA3D9 : deck == 1 ? 0xE0913A : 0x8A93A0; }

}  // namespace

PassWritten writePass(Document& doc, ObjectId channel, const Performance& perf, ObjectId pattern) {
  PassWritten out;
  const Song& s = doc.song();
  const Channel* ch = s.channel(channel);
  const bool song = pattern == kNoId;
  if (!ch || (!song && !s.pattern(pattern))) return out;
  const std::string chName = ch->name;
  // the start state: a new id in the setup's model (states no pass names any more go)
  int id = 0;
  {
    std::string path;
    Model m;
    if (!perf.start.empty() && parseData(ch->plugin.data, path, m)) {
      const std::set<int> used = startsInUse(s, channel);
      int next = 1;
      for (const auto& st : m.starts) next = std::max(next, st.id + 1);
      if (!used.empty()) next = std::max(next, *used.rbegin() + 1);
      if (next >= (1 << 20)) next = 1;
      while (used.count(next) || next == startIdOf(kNoteModCentre, kNoteModCentre)) ++next;
      id = next;
      doc.editChannel(channel, "Record a deck performance", [&](Channel& c) {
        std::string p0;
        Model mm;
        if (!parseData(c.plugin.data, p0, mm)) return;
        mm.starts.erase(
            std::remove_if(mm.starts.begin(), mm.starts.end(), [&](const PerfStart& q) { return !used.count(q.id) || q.id == id; }),
            mm.starts.end());
        PerfStart ps;
        ps.id = id;
        ps.v = perf.start;
        mm.starts.push_back(std::move(ps));
        c.plugin.data = formatData(p0, mm);
      });
    }
  }
  std::vector<PerfLane> lanes = perf.clips;
  for (auto& l : lanes)  // (the position lanes name the start state)
    if ((l.param == PTrackA || l.param == PTrackB) && !l.points.empty() && id > 0) l.points.front().tension = startTagOf(id);
  if (!song) {
    doc.editPattern(pattern, "Record a deck performance", [&](Pattern& p) {
      for (const PerfLane& l : lanes) setPatternLane(p, channel, laneIndex(l.param), passPoints(l, 0.0));
    });
    out.lanes = static_cast<int>(lanes.size());
    return out;
  }
  const Tick start = std::max<Tick>(0, static_cast<Tick>(std::floor(perf.startTick)));
  const Tick len = std::max<Tick>(1, static_cast<Tick>(std::ceil(perf.endTick - static_cast<double>(start))));
  // a deck state an earlier pass plays over this time and this one does not hold: its own lane
  // (off), so the earlier pass's state does not carry into this one
  for (int d = 0; d < kDecks; ++d) {
    if (!((perf.decks >> d) & 1)) continue;
    for (int dp : {static_cast<int>(DTouch), static_cast<int>(DHand), static_cast<int>(DLoop), static_cast<int>(DReverse), static_cast<int>(DBend)}) {
      const int p = deckParam(d, dp);
      if (perf.clip(p) || !laneDrives(s, channel, p, start, start + len)) continue;
      PerfLane l;
      l.param = p;
      l.points.push_back(PerfPoint{perf.startTick, normDefault(p), 0.0f, AutoShape::Hold});
      lanes.push_back(std::move(l));
    }
  }
  if (lanes.empty()) return out;
  // adjacent playlist tracks free over the pass (plain tracks: not an instrument's or audio's)
  const int count = static_cast<int>(lanes.size());
  auto busy = [&](int t) {
    for (const auto& c : s.playlist)
      if (c.track == t && c.start < start + len && c.start + s.clipLength(c) > start) return true;
    return false;
  };
  auto usable = [&](int t) {
    if (t >= static_cast<int>(s.tracks.size())) return true;  // (added)
    return s.tracks[static_cast<size_t>(t)].mode == TrackMode::Unassigned && !busy(t);
  };
  int first = -1;
  for (int t = 0; t + count <= kMaxPlaylistTracks && first < 0; ++t) {
    bool ok = true;
    for (int k = t; k < t + count && ok; ++k) ok = usable(k);
    if (ok) first = t;
  }
  if (first < 0) first = std::max(0, kMaxPlaylistTracks - count);
  doc.editSong("Record a deck performance", [&](Song& so) {
    if (static_cast<int>(so.tracks.size()) < first + count) so.tracks.resize(static_cast<size_t>(std::min(kMaxPlaylistTracks, first + count)));
    out.group = so.allocateId();
    for (int i = 0; i < count; ++i) {
      const PerfLane& l = lanes[static_cast<size_t>(i)];
      AutomationClip ac;
      ac.id = so.allocateId();
      ac.name = laneName(chName, l.param);
      int dp = -1;
      ac.color = laneColor(deckOfParam(l.param, dp));
      ac.owner = channel;
      ac.index = laneIndex(l.param);
      double lo = 0, hi = 1;
      if ((l.param == PTrackA || l.param == PTrackB) && !l.points.empty()) {
        // zoomed on the record's travel (exact: a power-of-two range on a 1/1024 grid)
        double nmin = 1, nmax = 0;
        for (const PerfPoint& q : l.points) nmin = std::min(nmin, q.value), nmax = std::max(nmax, q.value);
        lo = std::floor(nmin * 1024.0) / 1024.0;
        double range = 1.0 / 1024.0;
        while (range < 1.0 && lo + range < nmax + 1.0 / 4096.0) range *= 2.0;
        if (lo + range > 1.0) lo = std::max(0.0, 1.0 - range);
        hi = lo + range;
      }
      ac.min = static_cast<float>(lo);
      ac.max = static_cast<float>(hi);
      ac.points = passPoints(l, static_cast<double>(start), lo, hi);
      Clip c;
      c.id = so.allocateId();
      c.kind = ClipKind::Automation;
      c.source = ac.id;
      c.track = std::min(first + i, kMaxPlaylistTracks - 1);
      c.start = start;
      c.length = len;
      c.group = out.group;
      out.clips.push_back(c.id);
      out.autos.push_back(ac.id);
      so.automations.push_back(std::move(ac));
      so.playlist.push_back(c);
    }
  });
  out.firstTrack = first;
  out.lanes = count;
  return out;
}

}  // namespace gl::packs::platter

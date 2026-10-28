// Gridline — "platter" pack: recorded performances (J-159). UI thread, no audio.
// (In the core library with the model and the runtime, so the tests use it too.)
//
// A pass of the setup, captured on the audio thread step by step, becomes pattern data
// the user can see and edit:
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

}  // namespace

const PerfLane* Performance::lane(int param) const {
  for (const auto& l : lanes)
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
      btns_.clear();
      if (!active) return false;
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
      if (active && e.param < PCount) moves_[e.param].emplace_back(e.step, e.value);
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
    if (mv.size() < 2) continue;
    PerfLane l;
    l.param = p;
    for (const auto& [s, v] : mv) l.points.push_back(PerfPoint{pointAt(s), v});
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
    p.value = std::clamp(q.value, 0.f, 1.f);
    p.tension = 0;
    p.shape = AutoShape::Hold;
    pts.push_back(p);
  }
  return pts;
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

}  // namespace gl::packs::platter

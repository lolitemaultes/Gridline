#include "MidiRecord.h"
#include "Meter.h"
#include "NoteBend.h"
#include "NoteExprEdit.h"
#include "Settings.h"
#include <algorithm>
#include <cmath>
#include "Automation.h"
#include "AutomationEdit.h"
#include "PatternAutomation.h"
#include "Timeline.h"
#include "TrackModes.h"

namespace gl {

namespace {
// K-171: recorded controller moves are thinned to this error (value units), as
// recording a knob's moves already is: inaudible, and far fewer points.
constexpr double kRecordThin = 0.004;
}  // namespace

const char* autoRecordTargetName(AutoRecordTarget t) {
  static const char* n[] = {"Automatic", "Pattern events", "Automation clips"};
  int i = static_cast<int>(t);
  return i >= 0 && i < static_cast<int>(AutoRecordTarget::Count) ? n[i] : "?";
}

Tick quantizeTick(Tick t, Tick grid) {
  if (grid <= 1) return t;
  if (t >= 0) return (t + grid / 2) / grid * grid;
  return -quantizeTick(-t, grid);
}

// ------------------------------------------------------------ record options ----

void loadRecordOptions(const Settings& s, RecordOptions& o, int& countInBars, bool& punch) {
  o.overdub = s.getBool("record.midi.overdub");
  o.quantize = s.getBool("record.midi.quantize");
  o.quantizeLength = s.getBool("record.midi.quantizeLength");
  o.takes = s.getBool("record.midi.takes");
  o.notes = s.getBool("record.midi.notes");
  o.automation = s.getBool("record.midi.automation");
  const double t = s.getNumber("record.midi.autoTarget");
  o.autoTarget = std::isfinite(t) && t >= 0 && t < static_cast<double>(AutoRecordTarget::Count)
                     ? static_cast<AutoRecordTarget>(static_cast<int>(t))
                     : AutoRecordTarget::Auto;
  const int bars = static_cast<int>(s.getNumber("record.countInBars"));
  countInBars = bars == 1 || bars == 2 || bars == 4 ? bars : 0;
  punch = s.getBool("record.punch");
}

void saveRecordOptions(Settings& s, const RecordOptions& o, int countInBars, bool punch) {
  s.set("record.midi.overdub", o.overdub);
  s.set("record.midi.quantize", o.quantize);
  s.set("record.midi.quantizeLength", o.quantizeLength);
  s.set("record.midi.takes", o.takes);
  s.set("record.midi.notes", o.notes);
  s.set("record.midi.automation", o.automation);
  s.set("record.midi.autoTarget", static_cast<int>(o.autoTarget));
  s.set("record.countInBars", countInBars);
  s.set("record.punch", punch);
}

// ------------------------------------------------------------ NoteRecorder ----

void NoteRecorder::beginPattern(Document& doc, ObjectId pattern, const RecordOptions& o, Tick loopLength, const RecordSetup& setup) {
  *this = NoteRecorder{};
  const Pattern* p = doc.song().pattern(pattern);
  if (!p) return;
  active_ = true;
  opt_ = o;
  setup_ = setup;
  base_ = pattern_ = pattern;
  baseContent_ = *p;
  loop_ = std::max<Tick>(1, loopLength);
  takeIds_ = {pattern};
}

void NoteRecorder::beginSong(Document& doc, ObjectId pattern, const RecordOptions& o, const RecordSetup& setup) {
  *this = NoteRecorder{};
  active_ = true;
  song_ = true;
  opt_ = o;
  setup_ = setup;
  base_ = pattern;
  if (const Pattern* p = doc.song().pattern(pattern)) baseContent_ = *p;
  pattern_ = kNoId;  // the take is made with the first note
}

void NoteRecorder::edit(Document& doc, const std::function<void()>& fn) {
  const uint64_t before = doc.history().stateSerial();
  if (step_ != 0 && before == step_ && doc.groupDepth() == 0 && doc.history().canAmend()) {
    doc.amendStep(step_, fn);  // into this recording's step
  } else {
    doc.beginGroup("record");
    fn();
    doc.endGroup(false);
  }
  const uint64_t after = doc.history().stateSerial();
  if (after != before && doc.groupDepth() == 0) step_ = after;
}

double NoteRecorder::fold(double tick) const {
  if (song_ || loop_ <= 0) return tick;
  const double l = static_cast<double>(loop_);
  return tick - std::floor(tick / l) * l;
}

void NoteRecorder::position(Document& doc, double tick) {
  if (!active_) return;
  if (!started_ && tick >= static_cast<double>(setup_.start) - 0.5) started_ = true;
  // A wrap (or a jump back): further back than a few events out of order could be.
  double back = std::max(1.0, doc.song().ppq / 2.0);
  if (!song_)
    back = std::min(back, std::max(0.5, static_cast<double>(loop_) / 2.0));
  else if (setup_.loopEnd > setup_.loopStart)
    back = std::min(back, std::max(0.5, static_cast<double>(setup_.loopEnd - setup_.loopStart) / 2.0));
  if (last_ > -1e17 && tick < last_ - back) {
    if (song_) {
      const bool loop = setup_.loopEnd > setup_.loopStart && last_ > static_cast<double>(setup_.loopStart) &&
                        tick >= static_cast<double>(setup_.loopStart) - back;
      wrapFrom_ = loop ? static_cast<double>(setup_.loopEnd) : last_;
      wrapTo_ = loop ? static_cast<double>(setup_.loopStart) : tick;
      clearReplaced(doc, wrapFrom_);
    } else {
      wrapFrom_ = static_cast<double>(loop_);
      wrapTo_ = 0;
      cutHeld(doc, wrapFrom_);  // a note held over the loop end ends there (the pattern keeps its length)
      clearReplaced(doc, wrapFrom_);
    }
    newPass(doc);
  }
  last_ = tick;
  if (!started_) return;  // the count-in
  if (song_ && setup_.punchOut > setup_.punchIn && tick >= static_cast<double>(setup_.punchOut) && !held_.empty())
    cutHeld(doc, static_cast<double>(setup_.punchOut));  // punched out
  clearReplaced(doc, song_ ? tick : fold(tick));
  growTake(doc, tick);
}

void NoteRecorder::cutHeld(Document& doc, double at) {
  auto held = std::move(held_);
  held_.clear();
  for (const auto& [key, h] : held) commitNote(doc, h, key.second, at);
}

void NoteRecorder::newPass(Document& doc) {
  commitAutomation(doc);
  ++pass_;
  replace_.clear();
  mine_.clear();
  if (!opt_.takes || song_) return;  // song mode: the next note makes the next take (ensureTake)
  const Song& s = doc.song();
  if (static_cast<int>(s.patterns.size()) >= kMaxPatterns) return;
  const std::string name = baseContent_.name + " take " + std::to_string(takeIds_.size() + 1);
  ObjectId id = kNoId;
  const Pattern base = baseContent_;
  edit(doc, [&] {
    id = doc.addPattern(name, base.bars, base.stepsPerBar);
    if (id == kNoId) return;
    doc.editPattern(id, "new take", [&base](Pattern& p) {
      p.color = base.color;
      p.meterNum = base.meterNum;
      p.meterDen = base.meterDen;
      p.steps = base.steps;
      p.notes = base.notes;
      p.automation = base.automation;
    });
  });
  if (id == kNoId) return;
  takeIds_.push_back(id);
  pattern_ = id;
}

bool NoteRecorder::ensureTake(Document& doc, double tick, ObjectId channel) {
  const Song& s = doc.song();
  const bool have = pattern_ != kNoId && s.pattern(pattern_) && s.clip(clip_);
  const bool nextTake = have && opt_.takes && takePass_ != pass_;  // loop recording: a take per pass
  if (have && !nextTake && tick >= static_cast<double>(origin_) - 0.5) return true;
  if (static_cast<int>(s.patterns.size()) >= kMaxPatterns) return false;
  const MeterMap mm(s);  // K-179: on a bar line of the meter map
  const Tick here = std::max<Tick>(0, static_cast<Tick>(std::floor(tick)));
  Tick at = here;  // played before the take (a jump back): where it was played
  if (have && tick >= static_cast<double>(origin_) - 0.5)
    at = origin_;  // the next take of a loop: where the last one starts
  else if (!have && takeIds_.empty())
    at = std::min(here, std::max<Tick>(0, setup_.punchOut > setup_.punchIn ? std::max(setup_.start, setup_.punchIn) : setup_.start));
  const Tick origin = mm.barStartAt(at);
  const Tick bar = std::max<Tick>(1, mm.barLenAt(origin));
  // The track: below the last take (a loop's passes stack), the channel's instrument track (PL-08), or
  // the first free unassigned track.
  auto freeAt = [&](int t) {
    if (t < static_cast<int>(s.tracks.size()) && s.tracks[static_cast<size_t>(t)].mode != TrackMode::Unassigned) return false;
    for (const auto& c : s.playlist)
      if (c.track == t && c.start < origin + bar && c.start + s.clipLength(c) > origin) return false;
    return true;
  };
  int track = -1;
  if (nextTake && track_ >= 0)
    for (int t = track_ + 1; t < kMaxPlaylistTracks && track < 0; ++t)
      if (freeAt(t)) track = t;
  if (track < 0 && !nextTake && setup_.track >= 0 && setup_.track < static_cast<int>(s.tracks.size())) track = setup_.track;  // R05-02
  if (track < 0 && !nextTake) track = trackmodes::instrumentTrackOf(s, channel);
  for (int t = 0; t < kMaxPlaylistTracks && track < 0; ++t)
    if (freeAt(t)) track = t;
  if (track < 0) track = 0;
  // The name: the channel's (or the selected pattern's) with the next free take number.
  std::string stem = baseContent_.name.empty() ? std::string("Pattern") : baseContent_.name;
  if (const Channel* ch = s.channel(channel)) stem = ch->name;
  int n = 1;
  for (const auto& p : s.patterns)
    if (p.name.rfind(stem + " take ", 0) == 0) ++n;
  const std::string name = stem + " take " + std::to_string(n);
  const int steps = baseContent_.stepsPerBar > 0 ? baseContent_.stepsPerBar : 16;
  const ObjectId prevClip = nextTake ? clip_ : kNoId;
  ObjectId id = kNoId, cid = kNoId;
  edit(doc, [&] {
    id = doc.addPattern(name, 1, steps);
    if (id == kNoId) return;
    cid = doc.addClip(id, track, origin);
    if (prevClip != kNoId) doc.editClip(prevClip, "mute earlier take", [](Clip& c) { c.muted = true; });  // the newest pass plays
  });
  if (id == kNoId || cid == kNoId) return false;
  pattern_ = id;
  clip_ = cid;
  origin_ = origin;
  track_ = track;
  takePass_ = pass_;
  takeIds_.push_back(id);
  takeClips_.push_back(cid);
  return true;
}

void NoteRecorder::growTake(Document& doc, double tick) {
  if (!song_ || clip_ == kNoId || (opt_.takes && takePass_ != pass_)) return;
  double upTo = tick;
  if (setup_.punchOut > setup_.punchIn) upTo = std::min(upTo, static_cast<double>(setup_.punchOut));
  if (setup_.loopEnd > setup_.loopStart && origin_ < setup_.loopEnd) upTo = std::min(upTo, static_cast<double>(setup_.loopEnd));
  const Song& s = doc.song();
  const Pattern* p = s.pattern(pattern_);
  if (!p) return;
  const Tick bar = std::max<Tick>(1, s.patternBarTicks(*p));
  const double len = upTo - static_cast<double>(origin_);
  if (len <= static_cast<double>(s.patternLength(*p))) return;
  const int bars = std::min(kMaxPatternBars, static_cast<int>(std::ceil(len / static_cast<double>(bar))));
  if (bars <= p->bars) return;
  const ObjectId id = pattern_;
  const int spb = p->stepsPerBar;
  edit(doc, [&] { doc.setPatternLength(id, bars, spb); });
}

void NoteRecorder::noteOn(Document& doc, ObjectId channel, int pitch, int velocity, double tick) {
  if (!active_ || !opt_.notes || !doc.song().channel(channel)) return;
  position(doc, tick);
  const double played = tick;
  if (!started_ && tick < static_cast<double>(setup_.start) - 0.5) {  // in the count-in
    if (tick < static_cast<double>(setup_.start - setup_.pickup) - 0.5) return;  // too early: not part of the take
    tick = static_cast<double>(setup_.start);                                   // a pickup: on the first beat
  }
  if (song_ && setup_.punchOut > setup_.punchIn &&
      (tick < static_cast<double>(setup_.punchIn) - 0.5 || tick >= static_cast<double>(setup_.punchOut)))
    return;  // outside the punch range
  auto key = std::make_pair(channel, pitch);
  if (auto it = held_.find(key); it != held_.end()) {  // retrigger: close the previous one
    const Held prev = it->second;
    held_.erase(it);
    commitNote(doc, prev, pitch, tick);
  }
  if (song_ && !ensureTake(doc, tick, channel)) return;
  Held h{channel, std::clamp(velocity, 1, 127), song_ ? tick : fold(tick), pass_, pattern_, song_ ? origin_ : 0, played};
  if (!opt_.overdub && !replace_.count(channel)) {
    Replace r;
    r.from = r.to = h.start;
    replace_[channel] = r;
  }
  held_[key] = h;
}

void NoteRecorder::noteOff(Document& doc, ObjectId channel, int pitch, double tick) {
  if (!active_) return;
  position(doc, tick);
  auto it = held_.find({channel, pitch});
  if (it == held_.end()) return;
  const Held h = it->second;
  held_.erase(it);
  commitNote(doc, h, pitch, tick);
}

void NoteRecorder::expression(ObjectId channel, int pitch, ExprDim dim, float value, double tick) {
  if (!active_) return;
  auto it = held_.find({channel, pitch});
  if (it == held_.end() || !std::isfinite(value)) return;
  auto& lane = heldExpr_[{channel, pitch}][static_cast<size_t>(dim) % 3];
  const double rel = std::max(0.0, tick - it->second.played);
  if (!lane.empty() && lane.back().first > rel) return;  // (a loop wrap: the note is cut there)
  if (!lane.empty() && rel - lane.back().first < 0.5) lane.back().second = value;  // one value per half tick
  else if (lane.size() < 4096) lane.emplace_back(rel, value);
}

void NoteRecorder::commitNote(Document& doc, const Held& h, int pitch, double endTick) {
  const Song& s = doc.song();
  std::array<std::vector<std::pair<double, float>>, 3> played;  // K-99: the note's expression as played
  if (auto e = heldExpr_.find({h.channel, pitch}); e != heldExpr_.end()) played = std::move(e->second), heldExpr_.erase(e);
  if (!s.pattern(h.pattern) || !s.channel(h.channel)) return;
  const Tick g = std::max<Tick>(1, opt_.grid);
  if (!started_ && endTick < h.start) endTick = h.start + std::max(1.0, endTick - h.played);  // a pickup let go in the count-in
  double start = h.start, end = endTick;
  Tick limit = 0;  // song mode: quantize keeps the start before this (pattern ticks; 0: no limit)
  if (song_) {
    const int wraps = pass_ - h.pass;
    if (wraps > 0) end = wrapFrom_ + (endTick - wrapTo_) + (wrapFrom_ - wrapTo_) * (wraps - 1);  // held over a loop wrap: its real length
    if (setup_.punchOut > setup_.punchIn) {
      end = std::min(end, static_cast<double>(setup_.punchOut));
      limit = setup_.punchOut - h.origin;
    } else if (setup_.loopEnd > setup_.loopStart && h.start >= static_cast<double>(setup_.loopStart) - 0.5 &&
               h.start < static_cast<double>(setup_.loopEnd)) {
      limit = setup_.loopEnd - h.origin;
    }
    start -= static_cast<double>(h.origin);
    end -= static_cast<double>(h.origin);
  } else {
    const double l = static_cast<double>(loop_);
    if (pass_ > h.pass || end < start) end = l;  // over the loop end: cut there
    if (start >= l) start = fold(start);          // the pattern got shorter meanwhile
    end = std::min(end, l);
  }
  const double len = end - start;
  Tick qs = static_cast<Tick>(std::llround(std::max(0.0, start)));
  if (opt_.quantize) qs = quantizeTick(qs, g);
  if (song_) {
    if (limit > 0 && qs >= limit) qs = std::max<Tick>(0, (limit - 1) / g * g);  // the last grid line inside
  } else if (loop_ > 0) {
    qs %= loop_;  // quantized onto the loop end: the start of the loop
  }
  Tick ql = std::max<Tick>(1, static_cast<Tick>(std::llround(len)));
  if (opt_.quantizeLength) ql = std::max(g, quantizeTick(ql, g));
  if (!song_ && loop_ > 0 && qs + ql > loop_) ql = std::max<Tick>(1, loop_ - qs);  // never past the loop end
  const ObjectId pat = h.pattern, ch = h.channel;
  Note note(qs, ql, pitch, h.velocity);
  // K-99: the expression played into the note's lanes, timed from where the key went down (a
  // quantized start moves the lanes with it, so they stay on the performance).
  const double shift = start - static_cast<double>(qs);
  for (int d = 0; d < 3; ++d) {
    if (played[static_cast<size_t>(d)].empty()) continue;
    const ExprDim dim = static_cast<ExprDim>(d);
    std::vector<BendPoint>& lane = exprlane::laneOf(note, dim);
    for (const auto& [rel, v] : played[static_cast<size_t>(d)]) {
      const double t = std::max(0.0, rel + shift);
      if (t > static_cast<double>(ql)) break;
      const float stored = dim == ExprDim::Pitch ? std::clamp(v, -bend::kMaxSemis, bend::kMaxSemis)
                                                 : exprlane::stored(dim, std::clamp(v, 0.0f, 1.0f));
      if (!lane.empty() && static_cast<double>(lane.back().tick) >= t) lane.back().semis = stored;
      else lane.emplace_back(static_cast<float>(t), stored);
    }
    exprlane::simplify(lane, dim == ExprDim::Pitch ? 0.02f : 0.004f, bend::kMaxPoints);
  }
  edit(doc, [&] { doc.addNote(pat, ch, note); });
  if (h.pass == pass_) mine_.push_back(Mine{pat, ch, qs, pitch});
  ++recorded_;
}

void NoteRecorder::clearReplaced(Document& doc, double upTo) {
  if (opt_.overdub || replace_.empty()) return;
  for (auto& [ch, r] : replace_) {
    double to = upTo;
    if (song_ && setup_.punchOut > setup_.punchIn) to = std::min(to, static_cast<double>(setup_.punchOut));
    if (!song_) to = std::min(to, static_cast<double>(loop_));
    if (to <= r.to) continue;
    r.to = to;
    const double from = r.from;
    const ObjectId channel = ch;
    auto mine = [&](ObjectId pat, const Note& n) {
      for (const Mine& m : mine_)
        if (m.pattern == pat && m.channel == channel && m.start == n.start && m.pitch == n.pitch) return true;
      return false;
    };
    // The notes of `pat` (its tick 0 at song tick `zero`) inside [from, to) and the window [lo, hi) go,
    // except the ones this pass recorded.
    auto clearIn = [&](ObjectId pat, double zero, double lo, double hi) {
      const Pattern* p = doc.song().pattern(pat);
      if (!p) return;
      auto it = p->notes.find(channel);
      if (it == p->notes.end()) return;
      std::vector<Note> keep;
      bool removed = false;
      for (const Note& n : it->second) {
        const double at = zero + static_cast<double>(n.start);
        if (at >= from - 0.5 && at < to && at >= lo && at < hi && !mine(pat, n))
          removed = true;
        else
          keep.push_back(n);
      }
      if (removed) edit(doc, [&] { doc.setNotes(pat, channel, std::move(keep), "record (replace)"); });
    };
    if (!song_) {
      clearIn(pattern_, 0, -1e18, 1e18);
      continue;
    }
    if (pattern_ != kNoId && !(opt_.takes && takePass_ != pass_)) clearIn(pattern_, static_cast<double>(origin_), -1e18, 1e18);  // earlier passes
    // What other clips play there (only a pattern played by that one clip: the others would lose it too).
    struct Hit {
      ObjectId pattern;
      double zero, lo, hi;
    };
    std::vector<Hit> hits;
    {
      const Song& s = doc.song();
      for (const auto& c : s.playlist) {
        if (c.kind != ClipKind::Pattern || c.muted || std::find(takeIds_.begin(), takeIds_.end(), c.pattern) != takeIds_.end()) continue;
        const double c0 = static_cast<double>(c.start), c1 = static_cast<double>(c.start + s.clipLength(c));
        if (c1 <= from || c0 >= to) continue;
        int uses = 0;
        for (const auto& o : s.playlist) uses += o.kind == ClipKind::Pattern && o.pattern == c.pattern ? 1 : 0;
        if (uses == 1) hits.push_back(Hit{c.pattern, c0 - static_cast<double>(c.offset), c0, c1});
      }
    }
    for (const Hit& hit : hits) clearIn(hit.pattern, hit.zero, hit.lo, hit.hi);
  }
}

void NoteRecorder::controller(ParamId target, double normalized, double tick) {
  if (!active_ || !opt_.automation) return;
  auto& v = auto_[target.packed()];
  if (!v.empty() && v.back().first == tick) {
    v.back().second = normalized;
    return;
  }
  if (v.size() < 100000) v.push_back({tick, std::clamp(normalized, 0.0, 1.0)});
}

void NoteRecorder::pedal(ObjectId channel, bool down, double tick) {
  if (!active_ || !opt_.notes || channel == kNoId) return;
  auto& v = auto_[channelParam(channel, ChannelParam::Sustain).packed()];
  if (!v.empty() && v.back().first == tick) {
    v.back().second = down ? 1.0 : 0.0;
    return;
  }
  if (!v.empty() && (v.back().second >= 0.5) == down) return;  // (no change)
  if (v.size() < 100000) v.push_back({tick, down ? 1.0 : 0.0});
}

void NoteRecorder::commitAutomation(Document& doc) {
  if (auto_.empty()) return;
  const bool clips = opt_.autoTarget == AutoRecordTarget::AutomationClips ||
                     (opt_.autoTarget == AutoRecordTarget::Auto && song_);
  auto pending = std::move(auto_);
  auto_.clear();
  edit(doc, [&] {
    for (auto& entry : pending) {
      auto& pts = entry.second;
      const uint64_t packed = entry.first;
      if (pts.empty()) continue;
      const ParamId pid = ParamId::unpack(packed);
      if (!describeParam(doc.song(), pid)) continue;
      // The sustain pedal: recorded events (each press and lift steps the pedal on its sample,
      // nothing is sent before the first), kept as played (no thinning into ramps).
      const bool pedal = pid.index == static_cast<uint32_t>(ChannelParam::Sustain) && doc.song().channel(pid.owner);
      std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      if (clips) {
        // A new automation clip from the bar of the first move.
        const Song& s = doc.song();
        const MeterMap mm(s);
        const Tick first = static_cast<Tick>(std::floor(pts.front().first));
        const Tick last = static_cast<Tick>(std::ceil(pts.back().first));
        const Tick start = std::max<Tick>(0, mm.barStartAt(std::max<Tick>(0, first)));  // K-179
        std::string name = paramDisplayName(s, pid);
        doc.editSong("record automation", [&](Song& so) {
          AutomationClip ac;
          ac.id = so.allocateId();
          ac.name = name;
          ac.owner = pid.owner;
          ac.index = pid.index;
          for (const auto& [t, v] : pts) {
            AutoPoint p;
            p.pos = std::max<Tick>(0, static_cast<Tick>(std::llround(t)) - start);
            p.value = static_cast<float>(v);
            p.shape = AutoShape::Hold;
            if (!ac.points.empty() && ac.points.back().pos == p.pos)
              ac.points.back() = p;
            else
              ac.points.push_back(p);
          }
          int track = 0;
          for (; track < static_cast<int>(so.tracks.size()); ++track) {
            if (!trackmodes::clipAllowed(so, ClipKind::Automation, track)) continue;  // PL-08: not on instrument tracks
            bool busy = false;
            for (const auto& c : so.playlist)
              if (c.track == track && c.start <= last && c.start + so.clipLength(c) > start) busy = true;
            if (!busy) break;
          }
          if (track >= static_cast<int>(so.tracks.size())) {
            if (static_cast<int>(so.tracks.size()) >= kMaxPlaylistTracks) return;
            so.tracks.resize(static_cast<size_t>(track) + 1);
          }
          Clip c;
          c.id = so.allocateId();
          c.kind = ClipKind::Automation;
          c.source = ac.id;
          c.track = track;
          c.start = start;
          c.length = mm.nextBar(std::max(start, last)) - start;  // K-193: to the bar line after the last move, in the meter in force
          if (!pedal) ac.points = autoedit::thin(ac.points, kRecordThin);  // K-171: a few points, within 0.4 %
          ac.events = pedal;
          so.automations.push_back(std::move(ac));
          so.playlist.push_back(c);
        });
      } else {
        // Pattern events: replace the lane's points inside the recorded span.
        Tick origin = 0;
        if (song_) {
          if (!ensureTake(doc, pts.front().first, kNoId)) continue;
          origin = origin_;
        }
        const Tick t0 = static_cast<Tick>(std::floor(pts.front().first)) - origin;
        const Tick t1 = static_cast<Tick>(std::ceil(pts.back().first)) - origin;
        doc.editPattern(pattern_, "record automation", [&](Pattern& p) {
          std::vector<AutoPoint> out, take;
          const PatternAutomation* lane = findPatternLane(p, pid.owner, pid.index);
          if (lane)
            for (const auto& q : lane->points)
              if (q.pos < t0 || q.pos > t1) out.push_back(q);
          for (const auto& [t, v] : pts) {
            AutoPoint q;
            q.pos = std::max<Tick>(0, static_cast<Tick>(std::llround(t)) - origin);
            q.value = static_cast<float>(lane ? paramToAutomation(*lane, v) : v);  // J-173: into the clip's range
            q.shape = AutoShape::Hold;
            if (!take.empty() && take.back().pos == q.pos)
              take.back() = q;  // one point per tick (the last value wins)
            else
              take.push_back(q);
          }
          if (!pedal) take = autoedit::thin(take, kRecordThin);  // K-171: a few points, within 0.4 %
          out.insert(out.end(), take.begin(), take.end());
          std::stable_sort(out.begin(), out.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.pos < b.pos; });
          // One point per tick (the last value wins).
          std::vector<AutoPoint> dedup;
          for (const auto& q : out) {
            if (!dedup.empty() && dedup.back().pos == q.pos)
              dedup.back() = q;
            else
              dedup.push_back(q);
          }
          setPatternLane(p, pid.owner, pid.index, std::move(dedup));
          if (PatternAutomation* l = pedal ? findPatternLane(p, pid.owner, pid.index) : nullptr) l->events = true;
        });
      }
    }
  });
}

void NoteRecorder::end(Document& doc, double tick) {
  if (!active_) return;
  position(doc, tick);
  cutHeld(doc, song_ ? tick : fold(tick));
  clearReplaced(doc, song_ ? tick : fold(tick));
  commitAutomation(doc);
  active_ = false;
}

// ------------------------------------------------------------ StepRecorder ----

void StepRecorder::press(int pitch, int velocity) {
  down_.insert(pitch);
  chord_[pitch] = std::clamp(velocity, 1, 127);
}

// ---- K-112c: capture ----

void NoteCapture::noteOn(ObjectId channel, int pitch, int velocity, double seconds) {
  if (played_.size() >= kMax) played_.erase(played_.begin(), played_.begin() + static_cast<std::ptrdiff_t>(played_.size() - kMax + 1));
  played_.push_back(Played{channel, pitch, std::clamp(velocity, 1, 127), seconds, -1.0});
}

void NoteCapture::noteOff(ObjectId channel, int pitch, double seconds) {
  for (auto it = played_.rbegin(); it != played_.rend(); ++it)
    if (it->channel == channel && it->pitch == pitch && it->off < 0) {
      it->off = std::max(seconds, it->on);
      return;
    }
}

int NoteCapture::commit(Document& doc, ObjectId pattern, double bpm, Tick grid, double now, double gapSeconds) {
  const Song& s = doc.song();
  const ObjectId ch = lastChannel();
  if (ch == kNoId || !s.pattern(pattern) || !s.channel(ch) || bpm <= 0) {
    clear();
    return 0;
  }
  std::vector<Played> mine;
  for (const Played& p : played_)
    if (p.channel == ch) mine.push_back(p);
  for (Played& p : mine)
    if (p.off < 0) p.off = std::max(now, p.on);  // still held: up to now
  // The last phrase: back from the last note while the gap before each note is short.
  size_t first = mine.size() - 1;
  double reach = mine[first].on;  // the earliest start in the phrase so far
  for (size_t i = mine.size() - 1; i-- > 0;) {
    if (reach - mine[i].off > gapSeconds) break;
    first = i;
    reach = std::min(reach, mine[i].on);
  }
  const double t0 = mine[first].on;
  const double ticksPerSecond = bpm / 60.0 * static_cast<double>(s.ppq);
  std::vector<Note> notes;
  if (auto it = s.pattern(pattern)->notes.find(ch); it != s.pattern(pattern)->notes.end()) notes = it->second;
  int added = 0;
  for (size_t i = first; i < mine.size(); ++i) {
    const Played& p = mine[i];
    Tick start = static_cast<Tick>(std::llround((p.on - t0) * ticksPerSecond));
    if (grid > 0) start = (start + grid / 2) / grid * grid;
    const Tick len = std::max<Tick>(1, static_cast<Tick>(std::llround((p.off - p.on) * ticksPerSecond)));
    notes.push_back(Note(std::max<Tick>(0, start), len, p.pitch, p.velocity));
    ++added;
  }
  std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  doc.setNotes(pattern, ch, std::move(notes), "capture");
  clear();
  return added;
}

bool StepRecorder::release(Document& doc, ObjectId pattern, ObjectId channel, int pitch) {
  down_.erase(pitch);
  if (!down_.empty() || chord_.empty()) return false;
  const Song& s = doc.song();
  if (!s.pattern(pattern) || !s.channel(channel)) {
    chord_.clear();
    return false;
  }
  std::vector<Note> notes;
  if (auto it = s.pattern(pattern)->notes.find(channel); it != s.pattern(pattern)->notes.end()) notes = it->second;
  const Tick len = lengthOverride > 0 ? lengthOverride : std::max<Tick>(1, step);
  for (const auto& cv : chord_) {
    const int p = cv.first;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Note& n) { return n.start == cursor && n.pitch == p; }),
                notes.end());
    notes.push_back(Note(cursor, len, p, cv.second));
  }
  std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
  chord_.clear();
  doc.setNotes(pattern, channel, std::move(notes), "step record");
  cursor += std::max<Tick>(1, step);
  return true;
}

void StepRecorder::backspace(Document& doc, ObjectId pattern, ObjectId channel) {
  cursor = std::max<Tick>(0, cursor - std::max<Tick>(1, step));
  const Pattern* p = doc.song().pattern(pattern);
  if (!p) return;
  auto it = p->notes.find(channel);
  if (it == p->notes.end()) return;
  std::vector<Note> notes = it->second;
  const Tick a = cursor, b = cursor + std::max<Tick>(1, step);
  size_t before = notes.size();
  notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Note& n) { return n.start >= a && n.start < b; }),
              notes.end());
  if (notes.size() != before) doc.setNotes(pattern, channel, std::move(notes), "step record: delete");
}

// ------------------------------------------------------------ TypingKeyboard ----

const char* chordTypeName(ChordType c) {
  static const char* n[] = {"Major", "Minor", "Dominant 7th", "Major 7th", "Minor 7th", "Sus 4", "Power (5th)", "Octave"};
  int i = static_cast<int>(c);
  return i >= 0 && i < static_cast<int>(ChordType::Count) ? n[i] : "?";
}

std::vector<int> chordIntervals(ChordType c) {
  switch (c) {
    case ChordType::Minor: return {0, 3, 7};
    case ChordType::Seventh: return {0, 4, 7, 10};
    case ChordType::Major7: return {0, 4, 7, 11};
    case ChordType::Minor7: return {0, 3, 7, 10};
    case ChordType::Sus4: return {0, 5, 7};
    case ChordType::Power: return {0, 7};
    case ChordType::Octave: return {0, 12};
    default: return {0, 4, 7};
  }
}

int TypingKeyboard::keyOffset(int key) {
  switch (key) {
    // lower row: from the base C
    case 'z': return 0;
    case 's': return 1;
    case 'x': return 2;
    case 'd': return 3;
    case 'c': return 4;
    case 'v': return 5;
    case 'g': return 6;
    case 'b': return 7;
    case 'h': return 8;
    case 'n': return 9;
    case 'j': return 10;
    case 'm': return 11;
    case ',': return 12;
    case 'l': return 13;
    case '.': return 14;
    case ';': return 15;
    case '/': return 16;
    // upper row: one octave up
    case 'q': return 12;
    case '2': return 13;
    case 'w': return 14;
    case '3': return 15;
    case 'e': return 16;
    case 'r': return 17;
    case '5': return 18;
    case 't': return 19;
    case '6': return 20;
    case 'y': return 21;
    case '7': return 22;
    case 'u': return 23;
    case 'i': return 24;
    case '9': return 25;
    case 'o': return 26;
    case '0': return 27;
    case 'p': return 28;
    default: return -1;
  }
}

bool TypingKeyboard::isControlKey(int key) {
  return key == '[' || key == ']' || key == '-' || key == '=' || key == '\\' || key == '\'';
}

namespace {
struct KeyPosition {
  int position, key;
};
// Each key's USB usage. Where one key code has two places the first is the one positionOfKey gives.
constexpr KeyPosition kKeyPositions[] = {
    {29, 'z'}, {22, 's'}, {27, 'x'}, {7, 'd'},  {6, 'c'},  {25, 'v'}, {10, 'g'}, {5, 'b'},  {11, 'h'}, {17, 'n'}, {13, 'j'}, {16, 'm'},
    {54, ','}, {15, 'l'}, {55, '.'}, {51, ';'}, {56, '/'}, {20, 'q'}, {31, '2'}, {26, 'w'}, {32, '3'}, {8, 'e'},  {21, 'r'}, {34, '5'},
    {23, 't'}, {35, '6'}, {28, 'y'}, {36, '7'}, {24, 'u'}, {12, 'i'}, {38, '9'}, {18, 'o'}, {39, '0'}, {19, 'p'},
    {47, '['}, {48, ']'}, {45, '-'}, {46, '='}, {49, '\\'}, {52, '\''},
    {50, '\\'},                                       // the same key on ISO keyboards
    {86, '['}, {87, ']'}, {84, '-'}, {85, '='},        // keypad - + change the octave, / * the velocity
};
}  // namespace

int TypingKeyboard::keyAtPosition(int position) {
  for (const KeyPosition& k : kKeyPositions)
    if (k.position == position) return k.key;
  return 0;
}

int TypingKeyboard::positionOfKey(int key) {
  for (const KeyPosition& k : kKeyPositions)
    if (k.key == key) return k.position;
  return 0;
}

std::string TypingKeyboard::keyLabel(int key, int typed) {
  const bool ascii = typed > 32 && typed < 127, latin1 = typed >= 0xA1 && typed <= 0xFF && typed != 0xAD;  // (0xAD: soft hyphen)
  int c = ascii || latin1 ? typed : key;
  if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 0x20;  // à..þ in capitals (ß and ÿ have none in Latin-1)
  if (c < 0x80) return std::string(1, static_cast<char>(c));
  return {static_cast<char>(0xC0 | (c >> 6)), static_cast<char>(0x80 | (c & 0x3F))};  // UTF-8
}

namespace {
// K-113: each layout's characters where they differ from US QWERTY, by USB usage (the typing keys only).
constexpr KeyPosition kQwertz[] = {{29, 'y'}, {28, 'z'}, {51, 0xF6}, {52, 0xE4}, {47, 0xFC}, {48, '+'},
                                   {45, 0xDF}, {46, 0xB4}, {49, '#'}, {50, '#'}, {56, '-'}};
constexpr KeyPosition kAzerty[] = {{20, 'a'}, {26, 'z'}, {29, 'w'}, {16, ','}, {54, ';'}, {55, ':'}, {56, '!'}, {51, 'm'},
                                   {52, 0xF9}, {47, '^'}, {48, '$'}, {45, ')'}, {46, '='}, {49, '*'}, {50, '*'}, {31, 0xE9},
                                   {32, '"'}, {34, '('}, {35, '-'}, {36, 0xE8}, {38, 0xE7}, {39, 0xE0}};
constexpr KeyPosition kDvorak[] = {{29, ';'}, {22, 'o'}, {27, 'q'}, {7, 'e'},  {6, 'j'},  {25, 'k'}, {10, 'i'}, {5, 'x'},
                                   {11, 'd'}, {17, 'b'}, {13, 'h'}, {54, 'w'}, {15, 'n'}, {55, 'v'}, {51, 's'}, {56, 'z'},
                                   {20, '\''}, {26, ','}, {8, '.'},  {21, 'p'}, {23, 'y'}, {28, 'f'}, {24, 'g'}, {12, 'c'},
                                   {18, 'r'}, {19, 'l'}, {47, '/'}, {48, '='}, {45, '['}, {46, ']'}, {52, '-'}};
constexpr KeyPosition kColemak[] = {{22, 'r'}, {7, 's'},  {10, 'd'}, {11, 'h'}, {17, 'k'}, {13, 'n'}, {15, 'i'}, {51, 'o'},
                                    {8, 'f'},  {21, 'p'}, {23, 'g'}, {28, 'j'}, {24, 'l'}, {12, 'u'}, {18, 'y'}, {19, ';'}};

template <size_t N>
int lookUp(const KeyPosition (&table)[N], int position) {
  for (const KeyPosition& k : table)
    if (k.position == position) return k.key;
  return 0;
}
}  // namespace

int TypingKeyboard::layoutChar(TypingLayout layout, int position) {
  int c = 0;
  switch (layout) {
    case TypingLayout::Standard: break;
    case TypingLayout::Qwertz: c = lookUp(kQwertz, position); break;
    case TypingLayout::Azerty: c = lookUp(kAzerty, position); break;
    case TypingLayout::Dvorak: c = lookUp(kDvorak, position); break;
    case TypingLayout::Colemak: c = lookUp(kColemak, position); break;
    default: return 0;  // Computer (the system's layout) or unknown
  }
  return c ? c : keyAtPosition(position);
}

namespace {
struct LayoutNames {
  const char* id;
  const char* name;
};
constexpr LayoutNames kLayoutNames[] = {{"standard", "Standard (QWERTY)"}, {"qwertz", "QWERTZ"}, {"azerty", "AZERTY"},
                                        {"dvorak", "Dvorak"},              {"colemak", "Colemak"}, {"computer", "Follow the computer"}};
static_assert(sizeof(kLayoutNames) / sizeof(kLayoutNames[0]) == static_cast<size_t>(TypingLayout::Count), "typing layout names");
}  // namespace

const char* typingLayoutName(TypingLayout l) {
  return l < TypingLayout::Count ? kLayoutNames[static_cast<size_t>(l)].name : kLayoutNames[0].name;
}
const char* typingLayoutId(TypingLayout l) { return l < TypingLayout::Count ? kLayoutNames[static_cast<size_t>(l)].id : kLayoutNames[0].id; }
TypingLayout typingLayoutFromId(const std::string& id) {
  for (size_t i = 0; i < static_cast<size_t>(TypingLayout::Count); ++i)
    if (id == kLayoutNames[i].id) return static_cast<TypingLayout>(i);
  return TypingLayout::Standard;
}

std::vector<int> TypingKeyboard::heldNotes() const {
  std::vector<int> notes;
  for (const auto& [key, on] : held_) notes.insert(notes.end(), on.begin(), on.end());
  return notes;
}

namespace {
int aRowOffset(int key) {
  static const char row[] = "awsedftgyhujkolp;'";
  for (int i = 0; row[i]; ++i)
    if (row[i] == key) return i;
  return -1;
}
}  // namespace

std::vector<int> TypingKeyboard::keyDown(int key) {
  if (aRow) {
    switch (key) {
      case 'z': octave = std::max(0, octave - 1); return {};
      case 'x': octave = std::min(9, octave + 1); return {};
      case 'c': velocity = std::max(1, velocity - 20); return {};
      case 'v': velocity = std::min(127, velocity + 20); return {};
      default: break;
    }
  } else {
    switch (key) {
    case '[': octave = std::max(0, octave - 1); return {};
    case ']': octave = std::min(9, octave + 1); return {};
    case '-': velocity = std::max(1, velocity - 10); return {};
    case '=': velocity = std::min(127, velocity + 10); return {};
    case '\\': chordMode = !chordMode; return {};
    case '\'':
      chord = static_cast<ChordType>((static_cast<int>(chord) + 1) % static_cast<int>(ChordType::Count));
      return {};
    default: break;
    }
  }
  const int off = aRow ? aRowOffset(key) : keyOffset(key);
  if (off < 0 || held_.count(key)) return {};
  std::vector<int> notes;
  const int root = baseNote() + off + (aRow ? 12 : 0);  // (the A row's A is middle C, 60, as the reference's)
  if (chordMode) {
    for (int iv : chordIntervals(chord))
      if (root + iv >= 0 && root + iv <= 127) notes.push_back(root + iv);
  } else if (root >= 0 && root <= 127) {
    notes.push_back(root);
  }
  // A note already sounding from another key is not started twice.
  for (const auto& [k, v] : held_)
    for (int n : v) notes.erase(std::remove(notes.begin(), notes.end(), n), notes.end());
  if (notes.empty()) return {};
  held_[key] = notes;
  return notes;
}

std::vector<int> TypingKeyboard::keyUp(int key) {
  auto it = held_.find(key);
  if (it == held_.end()) return {};
  std::vector<int> v = it->second;
  held_.erase(it);
  return v;
}

int TypingKeyboard::keyAt(int position) const {
  if (const int k = keyAtPosition(position)) return k;
  if (!aRow) return 0;
  return position == 4 ? 'a' : position == 9 ? 'f' : position == 14 ? 'k' : 0;  // N48-08: the A row's other keys
}

std::vector<int> TypingKeyboard::press(int position, bool repeat) {
  const int key = keyAt(position);
  if (key == 0 || repeat || !down_.insert(position).second || settling_) return {};
  const int before = octave;
  std::vector<int> on = keyDown(key);
  if (octave != before) ++octaveSteps_;
  return on;
}

std::vector<int> TypingKeyboard::release(int position) {
  down_.erase(position);
  const int key = keyAt(position);
  return key ? keyUp(key) : std::vector<int>{};
}

std::vector<int> TypingKeyboard::releaseAllKeys() {
  down_.clear();
  return releaseAll();
}

std::vector<int> TypingKeyboard::releaseAll() {
  std::vector<int> v;
  for (const auto& [k, n] : held_) v.insert(v.end(), n.begin(), n.end());
  held_.clear();
  return v;
}

}  // namespace gl

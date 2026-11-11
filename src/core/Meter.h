// Gridline — the song's meter map: the meter at the song start (Song::timeSigNum/Den) plus the
// meter changes later in the song (markers with a time signature, per arrangement).
//
// Ticks stay absolute quarter-note ticks: a meter is notation. It decides where bar and beat lines
// fall (grid, ruler, snap, position display, metronome, plugin bar info), never where a clip or a
// note sits or how long it is.
//
// A meter change always starts a new bar. When one sits inside a bar, that bar ends early at the
// change (a "short bar") and keeps its number; the next bar starts at the change.
#pragma once
#include <cstdint>
#include <vector>
#include "Song.h"

namespace gl {

// One section of constant meter, from `start` to the next section's start.
struct MeterSeg {
  Tick start = 0;
  int num = 4, den = 4;
  Tick bar = 384, beat = 96;  // bar and beat lengths in ticks
  int64_t firstBar = 0;       // 0-based index of the bar that starts at `start`
  int group = 0;              // compound meters (6/8, 9/8, 12/8 ...): beats per group (3), else 0
  bool operator==(const MeterSeg& o) const {
    return start == o.start && num == o.num && den == o.den && bar == o.bar && beat == o.beat && firstBar == o.firstBar &&
           group == o.group;
  }
};

// Beats per accent group for a meter: 3 for compound meters (an eighth or sixteenth note beat, the
// beat count a multiple of three, at least six), else 0.
int meterGroup(int num, int den);
// Clamps a meter to what the map can use: num 1..32, den the nearest power of two 1..32. Returns
// false when the beat would not be a whole number of ticks at `ppq` (such a change is skipped).
bool normalizeMeter(int& num, int& den, int ppq);
// Steps per bar that keep a step a sixteenth note in this meter (16 in 4/4, 12 in 6/8 and 3/4,
// 14 in 7/8), at least 1 and at most kMaxStepsPerBar.
int sixteenthStepsPerBar(int num, int den);

class MeterMap {
 public:
  MeterMap() { setConstant(4, 4, kDefaultPpq); }
  explicit MeterMap(const Song& s) { build(s); }

  // The song's base meter and its meter markers (the current arrangement's).
  void build(const Song& s);
  void build(int ppq, int baseNum, int baseDen, const std::vector<Marker>& markers);
  void setConstant(int num, int den, int ppq);
  // A pattern's own meter (constant; pattern time). Null: the song's meter at the start.
  static MeterMap forPattern(const Song& s, const Pattern* p) {
    MeterMap m;
    m.setConstant(p ? s.patternMeterNum(*p) : s.timeSigNum, p ? s.patternMeterDen(*p) : s.timeSigDen, s.ppq);
    return m;
  }

  const std::vector<MeterSeg>& segments() const { return segs_; }
  size_t segIndex(Tick t) const;           // the section that holds t (t < 0: the first)
  const MeterSeg& segAt(Tick t) const { return segs_[segIndex(t)]; }
  // True when the whole song is in one meter (no meter changes that matter).
  bool constant() const { return segs_.size() == 1; }
  int ppq() const { return ppq_; }
  // The song's base meter's bar and beat (the sizes Song::barTicks/beatTicks return).
  Tick baseBar() const { return baseBar_; }
  Tick baseBeat() const { return baseBeat_; }
  // Changes in the song, cheap to compare: equal maps give equal signatures.
  uint64_t signature() const;

  struct Bbt {
    int64_t bar = 0;  // 0-based
    int beat = 0;     // 0-based beat in the bar
    Tick tick = 0;    // ticks into the beat
    int num = 4, den = 4;
  };
  Bbt toBbt(Tick t) const;
  int64_t barIndex(Tick t) const;          // 0-based bar holding t (negative before the start)
  Tick barStart(int64_t bar) const;        // start of a 0-based bar (bars past the last change extend its meter)
  Tick barStartAt(Tick t) const;           // the bar line at or before t
  Tick nextBar(Tick t) const;              // the first bar line after t
  Tick nearestBar(Tick t) const;           // the closest bar line (ties: the later one)
  Tick beatStartAt(Tick t) const;          // the beat line at or before t
  Tick nextBeat(Tick t) const;             // the first beat line after t
  Tick nearestBeat(Tick t) const;
  Tick barLenAt(Tick t) const;             // length of the bar holding t (a short bar is shorter)
  Tick fullBarLenAt(Tick t) const { return segAt(t).bar; }
  Tick beatLenAt(Tick t) const { return segAt(t).beat; }
  // True when the bar holding t is cut short by a meter change.
  bool shortBar(Tick t) const;
  // n bars later (or earlier, n < 0), keeping the offset into the bar (clamped to the target bar).
  Tick addBars(Tick t, int64_t n) const;
  // Beats in the bar holding t, and the bar's position kind of a beat line (2 = bar start,
  // 1 = compound group start, 0 = other beat).
  int accentAt(Tick beatLine) const;
  // t rounded up to a bar line (t itself when it is on one).
  Tick roundUpToBar(Tick t) const {
    const Tick b = barStartAt(t);
    return b == t ? t : nextBar(t);
  }
  // Bars from the song start to t, fractional (the bar holding t counted by its own length).
  double barsAt(Tick t) const {
    const Tick b = barStartAt(t), len = barLenAt(t);
    return static_cast<double>(barIndex(t)) + static_cast<double>(t - b) / static_cast<double>(len > 0 ? len : 1);
  }
  // Beats from the song start to t (fractional), each section counting its own beats.
  double beatCount(double t) const;

 private:
  void finish();
  std::vector<MeterSeg> segs_;
  int ppq_ = kDefaultPpq;
  Tick baseBar_ = 384, baseBeat_ = 96;
};

}  // namespace gl

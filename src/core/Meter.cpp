// Gridline — the song's meter map (see Meter.h).
#include "Meter.h"
#include <algorithm>
#include <cstdlib>

namespace gl {

int meterGroup(int num, int den) { return (den == 8 || den == 16) && num >= 6 && num % 3 == 0 ? 3 : 0; }

bool normalizeMeter(int& num, int& den, int ppq) {
  if (num <= 0 || den <= 0 || ppq <= 0) return false;
  num = std::clamp(num, 1, 32);
  int best = 1;
  for (int p = 1; p <= 32; p *= 2)
    if (std::abs(p - den) <= std::abs(best - den)) best = p;  // ties: the larger note value number (6 -> 8)
  den = best;
  const Tick q = static_cast<Tick>(ppq) * 4;
  return q % den == 0 && q / den >= 1;
}

int sixteenthStepsPerBar(int num, int den) {
  if (num <= 0 || den <= 0) return 16;
  return std::clamp((num * 16 + den / 2) / den, 1, kMaxStepsPerBar);
}

namespace {
Tick floorDiv(Tick a, Tick b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

void MeterMap::setConstant(int num, int den, int ppq) {
  build(ppq, num, den, {});
}

void MeterMap::build(const Song& s) { build(s.ppq, s.timeSigNum, s.timeSigDen, s.markers); }

void MeterMap::build(int ppq, int baseNum, int baseDen, const std::vector<Marker>& markers) {
  ppq_ = std::max(1, ppq);
  if (!normalizeMeter(baseNum, baseDen, ppq_)) baseNum = baseDen = 4;
  baseBar_ = std::max<Tick>(1, static_cast<Tick>(ppq_) * 4 * baseNum / baseDen);
  baseBeat_ = std::max<Tick>(1, static_cast<Tick>(ppq_) * 4 / baseDen);
  struct Change {
    Tick pos;
    int num, den;
  };
  std::vector<Change> ch;
  for (const Marker& m : markers) {
    if (m.timeSigNum <= 0 || m.timeSigDen <= 0) continue;
    int n = m.timeSigNum, d = m.timeSigDen;
    if (!normalizeMeter(n, d, ppq_)) continue;
    ch.push_back({std::max<Tick>(0, m.pos), n, d});
  }
  // Sorted by position; at one tick the last one in list order wins.
  std::stable_sort(ch.begin(), ch.end(), [](const Change& a, const Change& b) { return a.pos < b.pos; });
  segs_.clear();
  MeterSeg base;
  base.start = 0;
  base.num = baseNum;
  base.den = baseDen;
  segs_.push_back(base);
  auto fill = [this](MeterSeg& g) {
    g.beat = std::max<Tick>(1, static_cast<Tick>(ppq_) * 4 / g.den);
    g.bar = g.beat * g.num;
    g.group = meterGroup(g.num, g.den);
  };
  fill(segs_[0]);
  for (const Change& c : ch) {
    MeterSeg& last = segs_.back();
    if (c.pos == last.start) {  // replaces the meter at that tick (a change at 0 overrides the base)
      last.num = c.num;
      last.den = c.den;
      fill(last);
      continue;
    }
    MeterSeg g;
    g.start = c.pos;
    g.num = c.num;
    g.den = c.den;
    fill(g);
    // A change to the same meter on one of its own bar lines changes nothing.
    if (g.num == last.num && g.den == last.den && (g.start - last.start) % last.bar == 0) continue;
    segs_.push_back(g);
  }
  // A same-meter section can be left behind when a later change at its tick replaced it; merge.
  std::vector<MeterSeg> merged;
  for (const MeterSeg& g : segs_) {
    if (!merged.empty()) {
      const MeterSeg& p = merged.back();
      if (g.num == p.num && g.den == p.den && (g.start - p.start) % p.bar == 0) continue;
    }
    merged.push_back(g);
  }
  segs_ = std::move(merged);
  finish();
}

void MeterMap::finish() {
  segs_[0].firstBar = 0;
  for (size_t i = 1; i < segs_.size(); ++i) {
    const MeterSeg& p = segs_[i - 1];
    const Tick len = segs_[i].start - p.start;
    segs_[i].firstBar = p.firstBar + (len + p.bar - 1) / p.bar;
  }
}

size_t MeterMap::segIndex(Tick t) const {
  auto it = std::upper_bound(segs_.begin(), segs_.end(), t, [](Tick v, const MeterSeg& g) { return v < g.start; });
  return it == segs_.begin() ? 0 : static_cast<size_t>(it - segs_.begin() - 1);
}

uint64_t MeterMap::signature() const {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(static_cast<uint64_t>(ppq_));
  for (const MeterSeg& g : segs_) {
    mix(static_cast<uint64_t>(g.start));
    mix(static_cast<uint64_t>(g.num) << 8 | static_cast<uint64_t>(g.den));
  }
  return h;
}

int64_t MeterMap::barIndex(Tick t) const {
  const MeterSeg& g = segAt(t);
  return g.firstBar + floorDiv(t - g.start, g.bar);
}

Tick MeterMap::barStart(int64_t bar) const {
  auto it = std::upper_bound(segs_.begin(), segs_.end(), bar, [](int64_t v, const MeterSeg& g) { return v < g.firstBar; });
  const MeterSeg& g = it == segs_.begin() ? segs_[0] : *(it - 1);
  return g.start + (bar - g.firstBar) * g.bar;
}

Tick MeterMap::barStartAt(Tick t) const { return barStart(barIndex(t)); }
Tick MeterMap::nextBar(Tick t) const { return barStart(barIndex(t) + 1); }
Tick MeterMap::nearestBar(Tick t) const {
  const Tick a = barStartAt(t), b = nextBar(t);
  return t - a < b - t ? a : b;
}
Tick MeterMap::barLenAt(Tick t) const { return nextBar(t) - barStartAt(t); }
bool MeterMap::shortBar(Tick t) const { return barLenAt(t) < segAt(t).bar; }

Tick MeterMap::beatStartAt(Tick t) const {
  const MeterSeg& g = segAt(t);
  return g.start + floorDiv(t - g.start, g.beat) * g.beat;
}
Tick MeterMap::nextBeat(Tick t) const {
  const size_t i = segIndex(t);
  const MeterSeg& g = segs_[i];
  const Tick b = g.start + (floorDiv(t - g.start, g.beat) + 1) * g.beat;
  if (i + 1 < segs_.size() && segs_[i + 1].start < b) return segs_[i + 1].start;
  return b;
}
Tick MeterMap::nearestBeat(Tick t) const {
  const Tick a = beatStartAt(t), b = nextBeat(t);
  return t - a < b - t ? a : b;
}

Tick MeterMap::addBars(Tick t, int64_t n) const {
  const int64_t b = barIndex(t);
  const Tick off = t - barStart(b);
  const Tick tb = barStart(b + n);
  const Tick len = barStart(b + n + 1) - tb;
  return tb + std::min(off, std::max<Tick>(0, len - 1));
}

MeterMap::Bbt MeterMap::toBbt(Tick t) const {
  Bbt r;
  const MeterSeg& g = segAt(t);
  r.bar = barIndex(t);
  const Tick into = t - barStart(r.bar);
  r.beat = static_cast<int>(into / g.beat);
  r.tick = into % g.beat;
  r.num = g.num;
  r.den = g.den;
  return r;
}

int MeterMap::accentAt(Tick t) const {
  if (t == barStartAt(t)) return 2;
  const MeterSeg& g = segAt(t);
  if (g.group > 0 && ((t - barStartAt(t)) / g.beat) % g.group == 0) return 1;
  return 0;
}

double MeterMap::beatCount(double t) const {
  double n = 0;
  for (size_t i = 0; i < segs_.size(); ++i) {
    const MeterSeg& g = segs_[i];
    const bool last = i + 1 == segs_.size() || t < static_cast<double>(segs_[i + 1].start);
    if (last) return n + (t - static_cast<double>(g.start)) / static_cast<double>(g.beat);
    // A short section counts its beats rounded up, so the next section starts on a whole beat.
    n += static_cast<double>((segs_[i + 1].start - g.start + g.beat - 1) / g.beat);
  }
  return n;
}

}  // namespace gl

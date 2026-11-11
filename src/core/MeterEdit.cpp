// Gridline — editing meter changes and pattern meters (see MeterEdit.h).
#include "MeterEdit.h"
#include <algorithm>

namespace gl::meter {

namespace {

void sortMarkers(Song& s) {
  std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
}

bool valid(int& num, int& den, int ppq) { return normalizeMeter(num, den, ppq); }

// Index of the marker equal to `m` after sorting (the last such one).
int find(const Song& s, const Marker& m) {
  for (size_t i = s.markers.size(); i-- > 0;)
    if (s.markers[i] == m) return static_cast<int>(i);
  return -1;
}

bool isMeterIndex(const Song& s, int index) {
  return index >= 0 && index < static_cast<int>(s.markers.size()) && s.markers[static_cast<size_t>(index)].isMeter();
}

}  // namespace

std::string text(int num, int den) { return std::to_string(num) + "/" + std::to_string(den); }

int changeAt(const Song& s, Tick t) {
  int r = -1;
  for (size_t i = 0; i < s.markers.size(); ++i) {
    const Marker& m = s.markers[i];
    if (m.pos > t) break;
    int n = m.timeSigNum, d = m.timeSigDen;
    if (m.isMeter() && valid(n, d, s.ppq)) r = static_cast<int>(i);
  }
  return r;
}

int changeExactlyAt(const Song& s, Tick t) {
  int r = -1;
  for (size_t i = 0; i < s.markers.size(); ++i)
    if (s.markers[i].pos == t && s.markers[i].isMeter()) r = static_cast<int>(i);
  return r;
}

int changeCount(const Song& s) {
  int n = 0;
  for (const Marker& m : s.markers) n += m.isMeter();
  return n;
}

void meterAt(const Song& s, Tick t, int& num, int& den) {
  const MeterSeg g = MeterMap(s).segAt(t);
  num = g.num;
  den = g.den;
}

int add(Document& d, Tick pos, int num, int den, bool free) {
  const Song& s = d.song();
  if (!valid(num, den, s.ppq)) return -1;
  const MeterMap map(s);
  Tick p = free ? pos : map.barStartAt(pos);
  p = std::max<Tick>(0, p);
  if (p == 0) {  // the song's meter at the start
    d.setTimeSignature(num, den);
    return -1;
  }
  if (const int at = changeExactlyAt(s, p); at >= 0) return set(d, at, num, den) ? at : -1;
  Marker m;
  m.pos = p;
  m.timeSigNum = num;
  m.timeSigDen = den;
  int index = -1;
  d.editSong("add meter change", [&](Song& x) {
    x.markers.push_back(m);
    sortMarkers(x);
    index = find(x, m);
  });
  return index;
}

bool set(Document& d, int index, int num, int den) {
  const Song& s = d.song();
  if (!isMeterIndex(s, index) || !valid(num, den, s.ppq)) return false;
  const Marker& m = s.markers[static_cast<size_t>(index)];
  if (m.timeSigNum == num && m.timeSigDen == den) return true;
  d.editSong("edit meter change", [&](Song& x) {
    x.markers[static_cast<size_t>(index)].timeSigNum = num;
    x.markers[static_cast<size_t>(index)].timeSigDen = den;
  });
  return true;
}

int move(Document& d, int index, Tick pos, bool free) {
  const Song& s = d.song();
  if (!isMeterIndex(s, index)) return -1;
  std::vector<Marker> others = s.markers;
  others.erase(others.begin() + index);
  MeterMap map;
  map.build(s.ppq, s.timeSigNum, s.timeSigDen, others);
  Tick p = free ? pos : map.nearestBar(pos);
  if (p <= 0) return -1;  // the start's meter is the song's meter
  Marker m = s.markers[static_cast<size_t>(index)];
  if (m.pos == p) return index;
  m.pos = p;
  int out = -1;
  d.editSong("move meter change", [&](Song& x) {
    x.markers[static_cast<size_t>(index)].pos = p;
    sortMarkers(x);
    out = find(x, m);
  });
  return out;
}

bool remove(Document& d, int index) {
  const Song& s = d.song();
  if (!isMeterIndex(s, index)) return false;
  const Marker& m = s.markers[static_cast<size_t>(index)];
  const bool keep = !m.name.empty() || m.action != MarkerAction::None || m.isKey();
  d.editSong("remove meter change", [&](Song& x) {
    if (keep) {
      Marker& k = x.markers[static_cast<size_t>(index)];
      k.timeSigNum = k.timeSigDen = 0;
      k.meterForPatterns = true;
    } else {
      x.markers.erase(x.markers.begin() + index);
    }
  });
  return true;
}

int removeDuplicates(Document& d) {
  const Song& s = d.song();
  std::vector<size_t> drop;
  for (size_t i = 0; i < s.markers.size(); ++i) {
    if (!s.markers[i].isMeter()) continue;
    for (size_t j = i + 1; j < s.markers.size() && s.markers[j].pos == s.markers[i].pos; ++j)
      if (s.markers[j].isMeter()) {
        drop.push_back(i);
        break;
      }
  }
  if (drop.empty()) return 0;
  d.editSong("remove duplicate meter changes", [&](Song& x) {
    for (size_t k = drop.size(); k-- > 0;) {
      Marker& m = x.markers[drop[k]];
      if (!m.name.empty() || m.action != MarkerAction::None || m.isKey()) m.timeSigNum = m.timeSigDen = 0;
      else x.markers.erase(x.markers.begin() + static_cast<std::ptrdiff_t>(drop[k]));
    }
  });
  return static_cast<int>(drop.size());
}

bool setForPatterns(Document& d, int index, bool on) {
  if (!isMeterIndex(d.song(), index)) return false;
  if (d.song().markers[static_cast<size_t>(index)].meterForPatterns == on) return true;
  d.editSong("meter for new patterns", [&](Song& x) { x.markers[static_cast<size_t>(index)].meterForPatterns = on; });
  return true;
}

void newPatternMeter(const Song& s, Tick pos, int& num, int& den, int& steps) {
  num = den = 0;
  const MeterSeg g = MeterMap(s).segAt(std::max<Tick>(0, pos));
  steps = sixteenthStepsPerBar(g.num, g.den);
  const int i = changeAt(s, pos);
  if (i >= 0 && !s.markers[static_cast<size_t>(i)].meterForPatterns) {
    steps = sixteenthStepsPerBar(s.timeSigNum, s.timeSigDen);
    return;
  }
  if (g.num == s.timeSigNum && g.den == s.timeSigDen) return;
  num = g.num;
  den = g.den;
}

bool setPatternMeter(Document& d, ObjectId pattern, int num, int den) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return false;
  if (num > 0 || den > 0) {
    if (!valid(num, den, s.ppq)) return false;
    if (num == s.timeSigNum && den == s.timeSigDen) num = den = 0;
  } else {
    num = den = 0;
  }
  if (p->meterNum == num && p->meterDen == den) return true;
  const int oldNum = s.patternMeterNum(*p), oldDen = s.patternMeterDen(*p);
  const int newNum = num ? num : s.timeSigNum, newDen = num ? den : s.timeSigDen;
  bool noSteps = true;
  for (const auto& [ch, row] : p->steps)
    for (uint8_t v : row) noSteps = noSteps && v == 0;
  const bool adaptSteps = noSteps && p->stepsPerBar == sixteenthStepsPerBar(oldNum, oldDen);
  return d.editPattern(pattern, "pattern meter", [&](Pattern& x) {
    x.meterNum = num;
    x.meterDen = den;
    if (adaptSteps) {
      x.stepsPerBar = sixteenthStepsPerBar(newNum, newDen);
      for (auto& [ch, row] : x.steps) row.assign(static_cast<size_t>(x.bars) * static_cast<size_t>(x.stepsPerBar), 0);
      x.stepProps.clear();
    }
  });
}

bool matchPatternMeter(Document& d, ObjectId pattern, Tick at, bool keepLength) {
  const Song& s = d.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return false;
  const MeterSeg g = MeterMap(s).segAt(std::max<Tick>(0, at));
  int bars = p->bars, spb = p->stepsPerBar;
  if (keepLength) {
    // The same ticks and the same step size, so no note or step moves.
    const Tick len = s.patternLength(*p), step = s.ticksPerStep(*p);
    if (g.bar <= 0 || len % g.bar != 0 || step <= 0 || g.bar % step != 0) return false;
    bars = static_cast<int>(len / g.bar);
    spb = static_cast<int>(g.bar / step);
    if (bars < 1 || bars > kMaxPatternBars || spb < 1 || spb > kMaxStepsPerBar) return false;
  }
  const int num = g.num == s.timeSigNum && g.den == s.timeSigDen ? 0 : g.num;
  const int den = num ? g.den : 0;
  if (p->meterNum == num && p->meterDen == den && p->bars == bars && p->stepsPerBar == spb) return true;
  return d.editPattern(pattern, "match pattern meter", [&](Pattern& x) {
    x.meterNum = num;
    x.meterDen = den;
    x.bars = bars;
    x.stepsPerBar = spb;
  });
}

}  // namespace gl::meter

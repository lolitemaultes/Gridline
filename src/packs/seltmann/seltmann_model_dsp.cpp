// Gridline — "seltmann" pack: the model. Text form (stored after the sample path
// in PluginState::data), the beat grid and its corrections, the region and the
// chopping, manual cut edits, and the pattern step language.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include "../../engine/TimeStretch.h"
#include "seltmann.h"

namespace gl::packs::seltmann {

bool Slice::operator==(const Slice& o) const {
  return start == o.start && end == o.end && pitch == o.pitch && gain == o.gain && reverse == o.reverse && fadeIn == o.fadeIn &&
         fadeOut == o.fadeOut && offset == o.offset && length == o.length && choke == o.choke;
}
bool Chop::operator==(const Chop& o) const {
  return mode == o.mode && division == o.division && sensitivity == o.sensitivity && count == o.count && grainMs == o.grainMs &&
         snap == o.snap && zeroCross == o.zeroCross && autoSize == o.autoSize && hitSnap == o.hitSnap && cellBeats == o.cellBeats;
}
bool Step::operator==(const Step& o) const {
  return src == o.src && slot == o.slot && ties == o.ties && reverse == o.reverse && pitch == o.pitch && gain == o.gain &&
         length == o.length && stutter == o.stutter && offset == o.offset;
}
bool Pattern::operator==(const Pattern& o) const {
  return name == o.name && unitBeats == o.unitBeats && steps == o.steps && slots == o.slots;
}
double Pattern::beats() const {
  double b = 0;
  for (const auto& s : steps) b += (1 + s.ties) * unitBeats;
  return b;
}

// ------------------------------------------------------------------ divisions
double divisionBeats(int div, int bpb) {
  const double bar = std::max(1, bpb);
  switch (div) {
    case Div8Bars: return 8 * bar;
    case Div4Bars: return 4 * bar;
    case Div2Bars: return 2 * bar;
    case Div1Bar: return bar;
    case DivHalf: return bar / 2;
    case DivBeat: return 1;
    case Div8th: return 0.5;
    case Div16th: return 0.25;
    case Div32nd: return 0.125;
    case Div64th: return 0.0625;
    case Div8T: return 1.0 / 3.0;
    case Div16T: return 1.0 / 6.0;
    default: return bar;
  }
}
double chopCellBeats(const Chop& c, int bpb) { return c.cellBeats > 0 ? c.cellBeats : divisionBeats(c.division, bpb); }
const char* divisionName(int div) {
  static const char* const n[DivCount] = {"8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/8 T", "1/16 T"};
  return div >= 0 && div < DivCount ? n[div] : "";
}
const char* slotName(int slot) {
  static const char* const n[kSlotCount] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                            "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
  return slot >= 0 && slot < kSlotCount ? n[slot] : "?";
}

// ------------------------------------------------------------------ grid
double beatFrames(const Analysis& g) { return g.bpm > 0 && g.sampleRate > 0 ? 60.0 * g.sampleRate / g.bpm : 22050.0; }

double beatAt(const Analysis& g, double frame) {
  const auto& b = g.beats;
  if (b.size() < 2) return b.empty() ? frame / beatFrames(g) : (frame - b[0]) / beatFrames(g);
  if (frame <= b.front()) return (frame - b.front()) / (b[1] - b[0]);
  if (frame >= b.back()) return static_cast<double>(b.size() - 1) + (frame - b.back()) / (b.back() - b[b.size() - 2]);
  const size_t i = static_cast<size_t>(std::upper_bound(b.begin(), b.end(), frame) - b.begin()) - 1;
  return static_cast<double>(i) + (frame - b[i]) / std::max(1e-9, b[i + 1] - b[i]);
}

double frameAt(const Analysis& g, double beat) {
  const auto& b = g.beats;
  if (b.size() < 2) return (b.empty() ? 0.0 : b[0]) + beat * beatFrames(g);
  if (beat <= 0) return b[0] + beat * (b[1] - b[0]);
  const double last = static_cast<double>(b.size() - 1);
  if (beat >= last) return b.back() + (beat - last) * (b.back() - b[b.size() - 2]);
  const auto i = static_cast<size_t>(beat);
  return b[i] + (beat - static_cast<double>(i)) * (b[i + 1] - b[i]);
}

Analysis flatGrid(const Sample& s, double bpm) {
  Analysis a;
  a.sampleRate = s.sampleRate;
  a.frames = static_cast<int64_t>(s.frames());
  a.bpm = bpm;
  const double p = 60.0 * s.sampleRate / bpm;
  for (double f = 0; f < static_cast<double>(a.frames); f += p) a.beats.push_back(f);
  if (a.beats.empty()) a.beats.push_back(0);
  a.beatsPerBar = 4;
  a.sections.push_back({0, static_cast<int>(a.beats.size()), 0});
  return a;
}

namespace {

// Spans (sections / loops) in frames, to map them back after a grid change.
struct SpanF {
  double a, b;
};
void spansToFrames(const Analysis& g, std::vector<SpanF>& sec, std::vector<SpanF>& lp) {
  sec.clear();
  lp.clear();
  for (const auto& s : g.sections) sec.push_back({frameAt(g, s.firstBeat), frameAt(g, s.endBeat)});
  for (const auto& l : g.loops) lp.push_back({frameAt(g, l.firstBeat), 0});
}
void spansFromFrames(Analysis& g, const std::vector<SpanF>& sec, const std::vector<SpanF>& lp) {
  const int nb = static_cast<int>(g.beats.size());
  for (size_t i = 0; i < g.sections.size() && i < sec.size(); ++i) {
    g.sections[i].firstBeat = std::clamp(static_cast<int>(std::lround(beatAt(g, sec[i].a))), 0, nb);
    g.sections[i].endBeat = std::clamp(static_cast<int>(std::lround(beatAt(g, sec[i].b))), g.sections[i].firstBeat, nb);
  }
  for (size_t i = 0; i < g.loops.size() && i < lp.size(); ++i)
    g.loops[i].firstBeat = std::clamp(static_cast<int>(std::lround(beatAt(g, lp[i].a))), 0, std::max(0, nb - 1));
}
void regressBpm(Analysis& g) {
  const size_t m = g.beats.size();
  if (m < 2 || g.sampleRate <= 0) return;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (size_t i = 0; i < m; ++i) {
    const double x = static_cast<double>(i), y = g.beats[i];
    sx += x, sy += y, sxx += x * x, sxy += x * y;
  }
  const double den = m * sxx - sx * sx;
  if (den > 0) {
    const double slope = (m * sxy - sx * sy) / den;
    if (slope > 0) g.bpm = 60.0 * g.sampleRate / slope;
  }
}
void clampBar1(Analysis& g) {
  if (g.beats.empty()) g.bar1 = 0;
  else g.bar1 = std::clamp(g.bar1, 0, static_cast<int>(g.beats.size()) - 1);
}

}  // namespace

void gridSetBpm(Analysis& g, double bpm, double anchor) {
  if (bpm < 20 || bpm > 400 || g.sampleRate <= 0) return;
  std::vector<SpanF> sec, lp;
  spansToFrames(g, sec, lp);
  const double p = 60.0 * g.sampleRate / bpm;
  const double n = static_cast<double>(g.frames);
  anchor = std::clamp(anchor, 0.0, std::max(0.0, n - 1));
  const double first = anchor - std::floor(anchor / p) * p;
  g.beats.clear();
  int bar1 = 0;
  for (double f = first; f < n; f += p) {
    if (std::fabs(f - anchor) < p * 0.5) bar1 = static_cast<int>(g.beats.size());
    g.beats.push_back(f);
  }
  if (g.beats.empty()) g.beats.push_back(anchor);
  g.bar1 = bar1;
  g.bpm = bpm;
  spansFromFrames(g, sec, lp);
}

void gridDouble(Analysis& g) {
  if (g.beats.size() < 2) return;
  std::vector<double> b;
  for (size_t i = 0; i + 1 < g.beats.size(); ++i) {
    b.push_back(g.beats[i]);
    b.push_back(0.5 * (g.beats[i] + g.beats[i + 1]));
  }
  b.push_back(g.beats.back());
  const double half = 0.5 * (g.beats.back() - g.beats[g.beats.size() - 2]);
  if (g.beats.back() + half < static_cast<double>(g.frames)) b.push_back(g.beats.back() + half);
  g.beats.swap(b);
  g.bar1 *= 2;
  g.bpm *= 2;
  for (auto& s : g.sections) s.firstBeat *= 2, s.endBeat *= 2;
  for (auto& l : g.loops) l.firstBeat *= 2, l.bars = std::max(1, l.bars / 2);
  clampBar1(g);
  for (auto& s : g.sections) s.endBeat = std::min(s.endBeat, static_cast<int>(g.beats.size()));
}

void gridHalve(Analysis& g) {
  if (g.beats.size() < 4) return;
  std::vector<double> b;
  const int par = g.bar1 % 2;
  for (size_t i = static_cast<size_t>(par); i < g.beats.size(); i += 2) b.push_back(g.beats[i]);
  g.beats.swap(b);
  g.bar1 /= 2;
  g.bpm /= 2;
  for (auto& s : g.sections) s.firstBeat /= 2, s.endBeat = (s.endBeat + 1) / 2;
  for (auto& l : g.loops) l.firstBeat /= 2, l.bars = std::min(16, l.bars * 2);
  clampBar1(g);
  for (auto& s : g.sections) s.endBeat = std::min(s.endBeat, static_cast<int>(g.beats.size()));
}

void gridNudge(Analysis& g, double frames) {
  for (auto& b : g.beats) b += frames;
  // keep the indices stable: beats leaving the file are clamped, not dropped
  for (auto& b : g.beats) b = std::clamp(b, 0.0, std::max(0.0, static_cast<double>(g.frames) - 1));
}

void gridSetBar1(Analysis& g, int beat) {
  g.bar1 = beat;
  clampBar1(g);
}

void gridMoveDownbeat(Analysis& g, double frame) {
  if (g.beats.empty()) return;
  clampBar1(g);
  gridNudge(g, frame - g.beats[static_cast<size_t>(g.bar1)]);
}

void gridStretchTo(Analysis& g, int j, double frame) {
  const int n = static_cast<int>(g.beats.size());
  if (n < 2 || j < 0 || j >= n) return;
  clampBar1(g);
  const int i = g.bar1;
  if (j == i) return gridMoveDownbeat(g, frame);
  const double bi = g.beats[static_cast<size_t>(i)], bj = g.beats[static_cast<size_t>(j)];
  // the stretched span keeps at least 16 frames per beat
  if (j > i) frame = std::clamp(frame, bi + (j - i) * 16.0, std::max(bi + (j - i) * 16.0, static_cast<double>(g.frames) - 1));
  else frame = std::clamp(frame, std::min(0.0, bi - (i - j) * 16.0), bi - (i - j) * 16.0);
  const double scale = (frame - bi) / (bj - bi);
  const double delta = frame - bj;
  for (int k = 0; k < n; ++k) {
    double& b = g.beats[static_cast<size_t>(k)];
    if (j > i) {
      if (k > i && k <= j) b = bi + (b - bi) * scale;
      else if (k > j) b += delta;
    } else {
      if (k < i && k >= j) b = bi + (b - bi) * scale;
      else if (k < j) b += delta;
    }
  }
  for (auto& b : g.beats) b = std::clamp(b, 0.0, std::max(0.0, static_cast<double>(g.frames) - 1));
  regressBpm(g);
}

void gridSetMeter(Analysis& g, int bpb) { g.beatsPerBar = std::clamp(bpb, 1, 16); }

double tapTempo(const std::vector<double>& taps) {
  if (taps.size() < 3) return 0;
  std::vector<double> d;
  for (size_t i = std::max<size_t>(1, taps.size() > 9 ? taps.size() - 8 : 1); i < taps.size(); ++i) d.push_back(taps[i] - taps[i - 1]);
  if (d.empty()) return 0;
  std::vector<double> s = d;
  std::sort(s.begin(), s.end());
  const double med = s[s.size() / 2];
  double sum = 0;
  int cnt = 0;
  for (double v : d)
    if (std::fabs(v - med) < med * 0.25) sum += v, ++cnt;
  if (!cnt || sum <= 0) return 0;
  return std::clamp(60.0 / (sum / cnt), 30.0, 300.0);
}

// ------------------------------------------------------------------ region / chop
int barCount(const Model& m) {
  const Analysis& g = m.grid;
  if (g.beats.empty()) return 0;
  const int bpb = std::max(1, g.beatsPerBar);
  const double lastBeat = beatAt(g, static_cast<double>(m.srcFrames));
  return std::max(0, static_cast<int>(std::floor((lastBeat - g.bar1) / bpb + 1e-6)));
}

double barStartFrame(const Model& m, int bar) {
  return frameAt(m.grid, m.grid.bar1 + static_cast<double>(bar) * std::max(1, m.grid.beatsPerBar));
}

void setRegionBars(Model& m, int firstBar, int bars) {
  const double a = barStartFrame(m, firstBar);
  const double b = barStartFrame(m, firstBar + std::max(1, bars));
  m.regionStart = std::clamp<int64_t>(static_cast<int64_t>(std::llround(a)), 0, std::max<int64_t>(0, m.srcFrames - 1));
  m.regionEnd = std::clamp<int64_t>(static_cast<int64_t>(std::llround(b)), m.regionStart + 1, m.srcFrames);
}

int64_t nearestZeroCrossing(const Sample& s, int64_t f, int64_t radius) {
  const int64_t n = static_cast<int64_t>(s.frames());
  if (n < 2) return f;
  auto v = [&](int64_t i) {
    const double l = s.left[static_cast<size_t>(i)];
    return s.mono() || s.right.size() != s.left.size() ? l : 0.5 * (l + s.right[static_cast<size_t>(i)]);
  };
  f = std::clamp<int64_t>(f, 0, n - 1);
  for (int64_t d = 0; d <= radius; ++d) {
    for (int64_t c : {f + d, f - d}) {
      if (c <= 0 || c >= n) continue;
      const double a = v(c - 1), b = v(c);
      if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) return std::fabs(a) < std::fabs(b) ? c - 1 : c;
    }
  }
  return f;
}

namespace {

std::vector<int64_t> onsetsNear(const Sample& s, int64_t a, int64_t b, double sensitivity) {
  const int64_t n = static_cast<int64_t>(s.frames());
  a = std::clamp<int64_t>(a, 0, n);
  b = std::clamp<int64_t>(b, a, n);
  if (b - a < 64) return {};
  std::vector<float> x(static_cast<size_t>(b - a));
  const bool st = !s.mono() && s.right.size() == s.left.size();
  for (int64_t i = a; i < b; ++i) x[static_cast<size_t>(i - a)] = st ? 0.5f * (s.left[static_cast<size_t>(i)] + s.right[static_cast<size_t>(i)]) : s.left[static_cast<size_t>(i)];
  std::vector<int64_t> on = stretch::detectOnsets(x.data(), b - a, s.sampleRate, sensitivity);
  for (auto& o : on) o += a;
  return on;
}

int64_t minSliceFrames(const Model& m) { return std::max<int64_t>(16, static_cast<int64_t>(m.srcRate * 0.004)); }

}  // namespace

double degenerateLimit(const Model& m) {
  const double beat = beatFrames(m.grid);
  switch (m.chop.mode) {
    case ChopGrid: return 0.5 * chopCellBeats(m.chop, m.grid.beatsPerBar) * beat;
    case ChopTransients: return std::max(0.04 * m.srcRate, beat / 16.0);
    case ChopSections: return 0.5 * beat * std::max(1, m.grid.beatsPerBar);
    case ChopEqual: return 0.5 * static_cast<double>(m.regionEnd - m.regionStart) / std::max(1, m.chop.count);
    case ChopGrains: return 0.5 * std::clamp(static_cast<double>(m.chop.grainMs), 5.0, 2000.0) * 0.001 * m.srcRate;
    default: return static_cast<double>(minSliceFrames(m));
  }
}

int64_t snapFrame(const Model& m, const Sample* s, int64_t f, int snap) {
  switch (snap) {
    case SnapBeat: {
      const double b = beatAt(m.grid, static_cast<double>(f));
      const double q = std::round(b * 4.0) / 4.0;  // sixteenths of the grid
      return static_cast<int64_t>(std::llround(frameAt(m.grid, q)));
    }
    case SnapTransient: {
      if (!s) return f;
      const int64_t r = static_cast<int64_t>(s->sampleRate * 0.12);
      auto on = onsetsNear(*s, f - r, f + r, 0.6);
      int64_t best = f, bd = r + 1;
      for (int64_t o : on)
        if (std::llabs(o - f) < bd) bd = std::llabs(o - f), best = o;
      return best;
    }
    case SnapZero:
      return s ? nearestZeroCrossing(*s, f, static_cast<int64_t>(s->sampleRate * 0.005)) : f;
    default: return f;
  }
}

double regionBars(const Model& m) {
  const int bpb = std::max(1, m.grid.beatsPerBar);
  return (beatAt(m.grid, static_cast<double>(m.regionEnd)) - beatAt(m.grid, static_cast<double>(m.regionStart))) / bpb;
}

void resolveAutoChop(Model& m) {
  const double bars = regionBars(m);
  m.chop.hitSnap = false;
  m.chop.mode = ChopGrid;
  switch (m.grid.kind) {
    case KindBreak:
      m.chop.division = bars <= 1.05 ? Div16th : bars <= 4.05 ? Div8th : DivBeat;
      m.chop.hitSnap = true;
      break;
    case KindLoop: m.chop.division = bars <= 0.55 ? Div8th : bars <= 1.05 ? DivBeat : bars <= 2.05 ? DivHalf : Div1Bar; break;
    case KindOneShot:
      m.chop.mode = ChopTransients;
      m.chop.sensitivity = 0.5f;
      break;
    case KindAmbient:
      m.chop.mode = ChopEqual;
      m.chop.count = 8;
      break;
    default: m.chop.division = Div1Bar; break;  // songs: bars of the phrase
  }
}

void rechop(Model& m, const Sample* s) {
  if (m.chop.autoSize) resolveAutoChop(m);
  int64_t a = std::clamp<int64_t>(m.regionStart, 0, std::max<int64_t>(0, m.srcFrames - 1));
  int64_t b = std::clamp<int64_t>(m.regionEnd, a + 1, m.srcFrames);
  std::vector<int64_t> cuts;
  const Analysis& g = m.grid;
  switch (m.chop.mode) {
    case ChopGrid: {
      const double div = chopCellBeats(m.chop, g.beatsPerBar);
      const double ba = beatAt(g, static_cast<double>(a)), bb = beatAt(g, static_cast<double>(b));
      // grid lines over the region (and one cell either side)
      std::vector<double> lines;
      const double k0 = std::floor((ba - g.bar1) / div) - 1;
      for (double k = k0; g.bar1 + k * div < bb + div && lines.size() < static_cast<size_t>(kMaxSlices) + 4; k += 1)
        lines.push_back(frameAt(g, g.bar1 + k * div));
      const double cell = div * beatFrames(g);
      // the region stays on the grid: a partial cell under half a cell at either end is dropped
      for (double f : lines)
        if (f > static_cast<double>(a) + 0.5) {
          if (f - static_cast<double>(a) < 0.5 * cell && f < static_cast<double>(b) - 0.5 * cell) a = static_cast<int64_t>(std::llround(f));
          break;
        }
      for (auto it = lines.rbegin(); it != lines.rend(); ++it)
        if (*it < static_cast<double>(b) - 0.5) {
          if (static_cast<double>(b) - *it < 0.5 * cell && *it > static_cast<double>(a) + 0.5 * cell) b = static_cast<int64_t>(std::llround(*it));
          break;
        }
      cuts.push_back(a);
      for (double f : lines)
        if (f > static_cast<double>(a) + 0.5 && f < static_cast<double>(b) - 0.5) cuts.push_back(static_cast<int64_t>(std::llround(f)));
      // breaks: each cut moves onto the nearest hit within a quarter of a cell and 50 ms (played,
      // not quantised; a hit further away belongs to another step)
      if (m.chop.hitSnap && s && cuts.size() > 1) {
        const int64_t r = static_cast<int64_t>(std::min(0.25 * cell, 0.050 * m.srcRate));
        const auto on = onsetsNear(*s, a - r, b, 0.5);
        for (size_t i = 0; i < cuts.size(); ++i) {
          int64_t best = cuts[i], bd = r + 1;
          for (int64_t o : on)
            if (std::llabs(o - cuts[i]) < bd) bd = std::llabs(o - cuts[i]), best = o;
          if (i == 0 && best < 0) best = 0;
          cuts[i] = best;
        }
        a = cuts.front();
      }
      break;
    }
    case ChopTransients: {
      cuts.push_back(a);
      if (s) {
        auto on = onsetsNear(*s, a, b, std::clamp(static_cast<double>(m.chop.sensitivity), 0.0, 1.0));
        for (int64_t o : on)
          if (o > a && o < b) cuts.push_back(o);
      }
      break;
    }
    case ChopSections: {
      cuts.push_back(a);
      for (const auto& sec : g.sections) {
        const int64_t f = static_cast<int64_t>(std::llround(frameAt(g, sec.firstBeat)));
        if (f > a && f < b) cuts.push_back(f);
      }
      break;
    }
    case ChopEqual: {
      const int n = std::clamp(m.chop.count, 1, kMaxSlices);
      for (int k = 0; k < n; ++k) cuts.push_back(a + (b - a) * k / n);
      break;
    }
    case ChopGrains: {
      const double len = std::clamp(static_cast<double>(m.chop.grainMs), 5.0, 2000.0) * 0.001 * m.srcRate;
      for (double f = static_cast<double>(a); f < static_cast<double>(b) && cuts.size() < static_cast<size_t>(kMaxSlices); f += len)
        cuts.push_back(static_cast<int64_t>(std::llround(f)));
      break;
    }
    default: {  // manual: keep the cuts inside the region
      cuts.push_back(a);
      for (const auto& sl : m.slices)
        if (sl.start > a && sl.start < b) cuts.push_back(sl.start);
      break;
    }
  }
  std::sort(cuts.begin(), cuts.end());
  if (m.chop.zeroCross && s && m.chop.mode != ChopManual) {
    const int64_t r = static_cast<int64_t>(s->sampleRate * 0.0015);
    for (auto& c : cuts) c = nearestZeroCrossing(*s, c, r);
    std::sort(cuts.begin(), cuts.end());
  }
  // bounds: the cuts inside [a, b), unique, capped
  std::vector<int64_t> bd;
  for (int64_t c : cuts) {
    if (c < 0 || c >= b) continue;
    if (!bd.empty() && c <= bd.back()) continue;
    bd.push_back(c);
    if (bd.size() >= static_cast<size_t>(kMaxSlices)) break;
  }
  if (bd.empty()) bd.push_back(a);
  bd.push_back(b);
  // no degenerate slices: a fragment under the chop's minimum merges into a neighbour
  // (the first one into the next, the others into the previous)
  {
    const double minLen = m.chop.mode == ChopManual ? static_cast<double>(minSliceFrames(m)) : std::max<double>(static_cast<double>(minSliceFrames(m)), degenerateLimit(m));
    for (bool again = true; again && bd.size() > 2;) {
      again = false;
      for (size_t i = 0; i + 1 < bd.size() && bd.size() > 2; ++i)
        if (static_cast<double>(bd[i + 1] - bd[i]) < minLen) {
          // grains: a short remainder at the end is left out (every grain keeps its length)
          if (m.chop.mode == ChopGrains && i + 2 == bd.size()) bd.pop_back();
          else bd.erase(bd.begin() + static_cast<std::ptrdiff_t>(i == 0 ? 1 : i));
          again = true;
          break;
        }
    }
  }
  m.regionStart = bd.front();
  m.regionEnd = bd.back();
  const size_t count = bd.size() - 1;
  std::vector<Slice> out(count);
  const bool sameCount = out.size() == m.slices.size();
  for (size_t i = 0; i < count; ++i) {
    Slice sl = sameCount ? m.slices[i] : Slice{};
    sl.start = bd[i];
    sl.end = bd[i + 1];
    out[i] = sl;
  }
  m.slices.swap(out);
  m.pattern.slots.assign(kSlotCount, -1);
  for (int k = 0; k < kSlotCount && k < static_cast<int>(m.slices.size()); ++k) m.pattern.slots[static_cast<size_t>(k)] = k;
}

int addCut(Model& m, int64_t f) {
  const int64_t minLen = minSliceFrames(m);
  for (size_t i = 0; i < m.slices.size(); ++i) {
    Slice& s = m.slices[i];
    if (f > s.start + minLen && f < s.end - minLen && m.slices.size() < static_cast<size_t>(kMaxSlices)) {
      Slice t = s;
      t.start = f;
      s.end = f;
      m.slices.insert(m.slices.begin() + static_cast<std::ptrdiff_t>(i + 1), t);
      for (auto& sl : m.pattern.slots)
        if (sl > static_cast<int>(i)) ++sl;
      m.chop.mode = ChopManual;
      return static_cast<int>(i + 1);
    }
  }
  return -1;
}

bool removeCut(Model& m, int k) {
  if (k <= 0 || k >= static_cast<int>(m.slices.size())) return false;
  m.slices[static_cast<size_t>(k - 1)].end = m.slices[static_cast<size_t>(k)].end;
  m.slices.erase(m.slices.begin() + k);
  for (auto& sl : m.pattern.slots)
    if (sl >= k) --sl;
  m.chop.mode = ChopManual;
  return true;
}

bool moveCut(Model& m, int k, int64_t f) {
  const int n = static_cast<int>(m.slices.size());
  if (k < 0 || k >= n) return false;
  const int64_t minLen = minSliceFrames(m);
  Slice& s = m.slices[static_cast<size_t>(k)];
  if (k == 0) {
    f = std::clamp<int64_t>(f, 0, s.end - minLen);
    s.start = f;
    m.regionStart = f;
  } else {
    Slice& p = m.slices[static_cast<size_t>(k - 1)];
    f = std::clamp<int64_t>(f, p.start + minLen, s.end - minLen);
    p.end = f;
    s.start = f;
  }
  m.chop.mode = ChopManual;
  return true;
}

// ------------------------------------------------------------------ regions (J-149)
std::string regionName(int k) {
  if (k < 0) return "-";
  if (k < 26) return std::string(1, static_cast<char>('A' + k));
  k -= 26;
  std::string s;
  s += static_cast<char>('A' + (k / 26) % 26);
  s += static_cast<char>('A' + k % 26);
  return s;
}

bool isNaturalOrder(const Model& m) {
  const int n = static_cast<int>(m.slices.size());
  int next = 0;
  for (const auto& st : m.pattern.steps) {
    if (st.src == kRest) return false;
    if (stepSlice(m.pattern, st, n) != next) return false;
    ++next;
  }
  return next == n;
}

namespace {

// Every step as the slice it plays (slot letters resolved), so region edits can renumber them.
void resolveSteps(Model& m) {
  const int n = static_cast<int>(m.slices.size());
  for (auto& st : m.pattern.steps) {
    if (st.src == kRest) continue;
    st.src = std::max(0, stepSlice(m.pattern, st, n));
    st.slot = false;
  }
}

void identitySlots(Model& m) {
  m.pattern.slots.assign(kSlotCount, -1);
  for (int k = 0; k < kSlotCount && k < static_cast<int>(m.slices.size()); ++k) m.pattern.slots[static_cast<size_t>(k)] = k;
}

// After a region edit: the chop is by hand now (later re-chops keep the cuts) and a
// straight pattern is "edited" (its step lengths follow the regions, not one unit).
void afterRegionEdit(Model& m) {
  m.chop.mode = ChopManual;
  m.chop.autoSize = false;
  identitySlots(m);
  if (m.preset.empty() && m.pattern.name == "Straight") m.pattern.name = kEditedName;
}

int64_t gcd64(int64_t a, int64_t b) {
  while (b) {
    const int64_t t = a % b;
    a = b, b = t;
  }
  return a;
}

}  // namespace

bool splitRegion(Model& m, int x, int parts) {
  const int n = static_cast<int>(m.slices.size());
  if (x < 0 || x >= n || parts < 2 || parts > kMaxSplit) return false;
  if (n + parts - 1 > kMaxSlices) return false;
  const Slice src = m.slices[static_cast<size_t>(x)];
  const int64_t len = src.end - src.start;
  if (len / parts < std::max<int64_t>(16, static_cast<int64_t>(m.srcRate * 0.004))) return false;
  resolveSteps(m);
  // the step grid: every step on x must divide into `parts` whole units
  int64_t f = 1;
  for (const auto& st : m.pattern.steps)
    if (st.src == x) {
      const int64_t L = 1 + st.ties;
      const int64_t need = parts / gcd64(L, parts);
      f = f / gcd64(f, need) * need;  // lcm
    }
  const bool refine = f > 1 && m.pattern.unitBeats / static_cast<double>(f) >= 1.0 / 64 - 1e-12;
  if (refine) {
    m.pattern.unitBeats /= static_cast<double>(f);
    for (auto& st : m.pattern.steps) st.ties = static_cast<int>((1 + st.ties) * f - 1);
  }
  // count the steps first (the pattern has a size limit)
  size_t total = 0;
  for (const auto& st : m.pattern.steps) total += st.src == x ? static_cast<size_t>(parts) : 1;
  if (total > static_cast<size_t>(kMaxSteps)) return false;
  // slices: x becomes `parts` equal pieces, later slices move up
  std::vector<Slice> pieces;
  for (int p = 0; p < parts; ++p) {
    Slice s = src;
    s.start = src.start + len * p / parts;
    s.end = p + 1 == parts ? src.end : src.start + len * (p + 1) / parts;
    s.offset = 0, s.length = 1;
    if (p > 0) s.fadeIn = 0;
    if (p + 1 < parts) s.fadeOut = 0;
    pieces.push_back(s);
  }
  m.slices.erase(m.slices.begin() + x);
  m.slices.insert(m.slices.begin() + x, pieces.begin(), pieces.end());
  // steps: every step on x becomes the pieces in order (backwards for a reversed step)
  std::vector<Step> out;
  out.reserve(total);
  for (const auto& st : m.pattern.steps) {
    if (st.src == kRest || st.src < x) {
      out.push_back(st);
      continue;
    }
    if (st.src > x) {
      Step s = st;
      s.src += parts - 1;
      out.push_back(s);
      continue;
    }
    const int L = 1 + st.ties;
    for (int p = 0; p < parts; ++p) {
      Step s = st;
      const int piece = st.reverse ? parts - 1 - p : p;
      s.src = x + piece;
      // whole units shared out (exact when the grid was refined; else as even as possible, at least one)
      const int u0 = L * p / parts, u1 = L * (p + 1) / parts;
      s.ties = std::max(1, u1 - u0) - 1;
      s.offset = p == 0 ? std::min(0.9f, st.offset * static_cast<float>(parts)) : 0.0f;
      if (s.offset >= 0.9f) s.offset = 0;
      out.push_back(s);
    }
  }
  m.pattern.steps.swap(out);
  afterRegionEdit(m);
  return true;
}

bool mergeRegion(Model& m, int x) {
  const int n = static_cast<int>(m.slices.size());
  if (x < 0 || x >= n || n < 2) return false;
  const int lo = x == 0 ? 0 : x - 1, hi = lo + 1;
  const int keep = x == 0 ? hi : lo;  // the neighbour's settings stay
  resolveSteps(m);
  const double lenLo = static_cast<double>(m.slices[static_cast<size_t>(lo)].end - m.slices[static_cast<size_t>(lo)].start);
  const double lenHi = static_cast<double>(m.slices[static_cast<size_t>(hi)].end - m.slices[static_cast<size_t>(hi)].start);
  const double tot = std::max(1.0, lenLo + lenHi);
  Slice merged = m.slices[static_cast<size_t>(keep)];
  merged.start = m.slices[static_cast<size_t>(lo)].start;
  merged.end = m.slices[static_cast<size_t>(hi)].end;
  merged.fadeIn = m.slices[static_cast<size_t>(lo)].fadeIn;
  merged.fadeOut = m.slices[static_cast<size_t>(hi)].fadeOut;
  m.slices[static_cast<size_t>(lo)] = merged;
  m.slices.erase(m.slices.begin() + hi);
  auto plain = [](const Step& s) { return !s.reverse && s.stutter == 1 && s.length >= 0.999f && s.offset < 0.001f; };
  std::vector<Step> out;
  out.reserve(m.pattern.steps.size());
  const auto& in = m.pattern.steps;
  for (size_t i = 0; i < in.size(); ++i) {
    Step s = in[i];
    if (s.src == kRest || s.src < lo) {
      out.push_back(s);
      continue;
    }
    if (s.src > hi) {
      --s.src;
      out.push_back(s);
      continue;
    }
    if (s.src == lo && i + 1 < in.size() && in[i + 1].src == hi && plain(s) && plain(in[i + 1]) && s.pitch == in[i + 1].pitch &&
        s.gain == in[i + 1].gain) {
      // the pair played in order: one step as long as both
      s.ties += 1 + in[i + 1].ties;
      out.push_back(s);
      ++i;
      continue;
    }
    if (s.src == hi) {
      // on its own: the merged region from where this part starts
      s.offset = static_cast<float>(std::min(0.9, (lenLo + s.offset * lenHi) / tot));
      s.src = lo;
    } else {
      s.offset = static_cast<float>(s.offset * lenLo / tot);
    }
    out.push_back(s);
  }
  m.pattern.steps.swap(out);
  afterRegionEdit(m);
  return true;
}

bool moveStep(Model& m, int from, int to) {
  const int n = static_cast<int>(m.pattern.steps.size());
  if (from < 0 || from >= n || to < 0 || to > n) return false;
  if (to == from || to == from + 1) return false;
  Step s = m.pattern.steps[static_cast<size_t>(from)];
  m.pattern.steps.erase(m.pattern.steps.begin() + from);
  if (to > from) --to;
  m.pattern.steps.insert(m.pattern.steps.begin() + to, s);
  if (m.preset.empty() && m.pattern.name == "Straight") m.pattern.name = kEditedName;
  return true;
}

void naturalPattern(Model& m) {
  if (m.chop.mode != ChopManual) {
    straightPattern(m);
    return;
  }
  // regions cut by hand: the steps follow their lengths (named as edited, so a later
  // re-chop of the same cuts keeps them instead of laying one unit per step)
  const double u = std::max(1.0 / 64, m.pattern.unitBeats);
  m.pattern.name = m.preset.empty() ? kEditedName : m.pattern.name;
  m.pattern.steps.clear();
  identitySlots(m);
  for (int k = 0; k < static_cast<int>(m.slices.size()) && k < kMaxSteps; ++k) {
    const auto& sl = m.slices[static_cast<size_t>(k)];
    const double beats = beatAt(m.grid, static_cast<double>(sl.end)) - beatAt(m.grid, static_cast<double>(sl.start));
    Step st;
    st.slot = false;
    st.src = k;
    st.ties = std::max(1, static_cast<int>(std::lround(beats / u))) - 1;
    m.pattern.steps.push_back(st);
  }
}

// ------------------------------------------------------------------ J-185 start, pitch, resize
int firstStepOf(const Model& m, int x) {
  const int n = static_cast<int>(m.slices.size());
  if (x < 0 || x >= n) return -1;
  for (int i = 0; i < static_cast<int>(m.pattern.steps.size()); ++i)
    if (stepSlice(m.pattern, m.pattern.steps[static_cast<size_t>(i)], n) == x) return i;
  return -1;
}

int sequenceStartRegion(const Model& m) {
  const int n = static_cast<int>(m.slices.size());
  for (const auto& st : m.pattern.steps) {
    const int k = stepSlice(m.pattern, st, n);
    if (k >= 0) return k;
  }
  return -1;
}

bool startSequenceAt(Model& m, int step, bool keepRest) {
  auto& steps = m.pattern.steps;
  const int ns = static_cast<int>(steps.size());
  if (step <= 0 || step >= ns) return false;
  if (keepRest) std::rotate(steps.begin(), steps.begin() + step, steps.end());
  else steps.erase(steps.begin(), steps.begin() + step);
  if (m.preset.empty() && m.pattern.name == "Straight") m.pattern.name = kEditedName;
  return true;
}

bool setSlicePitch(Model& m, int slice, float semis) {
  if (slice < 0 || slice >= static_cast<int>(m.slices.size()) || !std::isfinite(semis)) return false;
  const float v = std::clamp(std::round(semis * 100.0f) / 100.0f, -kSlicePitchRange, kSlicePitchRange);
  float& p = m.slices[static_cast<size_t>(slice)].pitch;
  if (v == p) return false;
  p = v;
  return true;
}

bool resizeCut(Model& m, int slice, int64_t frame) {
  const int n = static_cast<int>(m.slices.size());
  if (slice <= 0 || slice >= n) return false;
  const bool natural = isNaturalOrder(m);
  const int64_t before = m.slices[static_cast<size_t>(slice)].start;
  const int mode = m.chop.mode;
  if (!moveCut(m, slice, frame)) return false;
  if (m.slices[static_cast<size_t>(slice)].start == before) {
    m.chop.mode = mode;
    return false;
  }
  m.chop.autoSize = false;
  if (!natural) return true;
  // every region once, in order: the steps keep following the regions' lengths
  std::vector<double> beats(static_cast<size_t>(n));
  for (int k = 0; k < n; ++k) {
    const auto& sl = m.slices[static_cast<size_t>(k)];
    beats[static_cast<size_t>(k)] = beatAt(m.grid, static_cast<double>(sl.end)) - beatAt(m.grid, static_cast<double>(sl.start));
  }
  double u = std::max(1.0 / 64, m.pattern.unitBeats);
  auto fits = [&](double unit) {
    for (double b : beats) {
      const double q = b / unit;
      if (std::fabs(q - std::round(q)) > 0.02) return false;
    }
    return true;
  };
  while (!fits(u) && u / 2 >= 1.0 / 64 - 1e-12) u /= 2;
  m.pattern.unitBeats = u;
  for (int i = 0; i < static_cast<int>(m.pattern.steps.size()) && i < n; ++i)
    m.pattern.steps[static_cast<size_t>(i)].ties = std::max(1, static_cast<int>(std::lround(beats[static_cast<size_t>(i)] / u))) - 1;
  if (m.preset.empty() && m.pattern.name == "Straight") m.pattern.name = kEditedName;
  return true;
}

void straightPattern(Model& m) {
  const int bpb = std::max(1, m.grid.beatsPerBar);
  const int n = static_cast<int>(m.slices.size());
  m.pattern.name = "Straight";
  if (m.chop.mode == ChopGrid) {
    m.pattern.unitBeats = chopCellBeats(m.chop, bpb);
  } else {
    // the nearest power-of-two step to the mean slice length (sixteenths .. bars)
    const double beats = std::max(1e-3, regionBars(m) * bpb / std::max(1, n));
    double u = 0.25;
    while (u * 2 <= beats * 1.414 && u < bpb) u *= 2;
    m.pattern.unitBeats = u;
  }
  m.pattern.steps.clear();
  for (int k = 0; k < n && k < 64; ++k) {
    Step st;
    st.slot = false;
    st.src = k;
    m.pattern.steps.push_back(st);
  }
}

void foldSteps(Model& m) {
  const int n = std::max(1, static_cast<int>(m.slices.size()));
  for (auto& st : m.pattern.steps) {
    if (st.src == kRest) continue;
    if (!st.slot) {
      st.src %= n;
    } else {
      const int k = st.src >= 0 && st.src < static_cast<int>(m.pattern.slots.size()) ? m.pattern.slots[static_cast<size_t>(st.src)] : -1;
      if (k < 0 || k >= n) st.src %= std::min(n, kSlotCount);
    }
  }
}

void rechopKeepPattern(Model& m, const Sample* s) {
  const size_t before = m.slices.size();
  rechop(m, s);
  // J-149: a sequence edited by hand (split / joined regions, a new order) stays when the
  // re-chop keeps the region count (tempo and grid fixes); a new count starts the natural
  // order again. The straight pattern and the layouts behave as before.
  const bool edited = m.preset.empty() && m.pattern.name != "Straight";
  if (m.preset.empty() && !(edited && m.slices.size() == before)) straightPattern(m);
  else foldSteps(m);
}

Model defaultModel(const Sample& s, const Analysis& a) {
  Model m;
  m.srcFrames = static_cast<int64_t>(s.frames());
  m.srcRate = s.sampleRate;
  m.grid = a;
  m.grid.onsetEnv.clear();
  m.analysed = !a.beats.empty();
  if (m.grid.beats.empty()) m.grid = flatGrid(s);
  const int bpb = std::max(1, m.grid.beatsPerBar);
  const double beat = beatFrames(m.grid);
  const int64_t c0 = std::clamp<int64_t>(m.grid.contentStart, 0, std::max<int64_t>(0, m.srcFrames - 1));
  const int64_t c1 = m.grid.contentEnd > c0 ? std::min(m.grid.contentEnd, m.srcFrames) : m.srcFrames;
  const int bars = barCount(m);
  // the first bar that starts with the sound (leading silence and pickups stay outside)
  int firstBar = 0;
  while (firstBar + 1 < bars && barStartFrame(m, firstBar) < static_cast<double>(c0) - beat / 8) ++firstBar;
  const bool shortMaterial = static_cast<double>(m.srcFrames) / std::max(1.0, m.srcRate) < 45;
  if (m.grid.kind == KindOneShot || (m.grid.kind == KindAmbient && shortMaterial) || bars < 1) {
    // no bars to hold on to: the sound itself (from the first frame the grid has, or the first sound)
    const double b1 = frameAt(m.grid, m.grid.bar1);
    m.regionStart = m.grid.loopFit && b1 >= 0 && b1 < static_cast<double>(c1) ? static_cast<int64_t>(std::llround(b1)) : c0;
    m.regionEnd = m.grid.kind == KindOneShot || m.grid.kind == KindAmbient ? c1 : m.srcFrames;
    if (m.grid.kind == KindAmbient && !shortMaterial) m.regionEnd = std::min(m.srcFrames, m.regionStart + static_cast<int64_t>(8 * m.srcRate));
  } else if (shortMaterial || m.grid.loopFit) {
    // loops and breaks: every whole bar from bar 1 (a tail or a partial bar at the end stays outside)
    setRegionBars(m, firstBar, std::max(1, bars - firstBar));
  } else {
    // songs: the best 4-bar loop spot, else 4 bars from the first bar with sound
    int first = -1;
    for (const auto& l : m.grid.loops)
      if (l.bars == 4 && frameAt(m.grid, l.firstBeat) >= static_cast<double>(c0) - beat / 8) {
        first = l.firstBeat;
        break;
      }
    const int fb = first >= 0 ? static_cast<int>(std::floor(static_cast<double>(first - m.grid.bar1) / bpb + 1e-6)) : firstBar;
    setRegionBars(m, std::clamp(fb, 0, std::max(0, bars - 1)), std::min(4, std::max(1, bars - std::max(0, fb))));
  }
  m.chop = Chop{};
  m.chop.autoSize = true;
  rechop(m, &s);
  straightPattern(m);
  m.preset = "";
  return m;
}

// ------------------------------------------------------------------ pattern language
std::string stepToText(const Step& s) {
  std::string t;
  if (s.src == kRest) t = ".";
  else if (s.slot) t = slotName(s.src);
  else t = std::to_string(s.src + 1);
  char b[24];
  auto num = [&](float v) {
    if (std::fabs(v - std::round(v)) < 1e-4f) std::snprintf(b, sizeof b, "%+d", static_cast<int>(std::lround(v)));
    else std::snprintf(b, sizeof b, "%+.2f", static_cast<double>(v));
    return std::string(b);
  };
  std::string mods;
  if (s.reverse) mods += "r";
  if (s.pitch != 0) mods += (mods.empty() ? "" : ":") + std::string("p") + num(s.pitch);
  if (s.gain != 0) mods += (mods.empty() ? "" : ":") + std::string("g") + num(s.gain);
  if (s.length < 0.999f) mods += (mods.empty() ? "" : ":") + std::string("l") + std::to_string(static_cast<int>(std::lround(s.length * 100)));
  if (s.stutter > 1) mods += (mods.empty() ? "" : ":") + std::string("x") + std::to_string(s.stutter);
  if (s.offset > 0.001f) mods += (mods.empty() ? "" : ":") + std::string("o") + std::to_string(static_cast<int>(std::lround(s.offset * 100)));
  if (!mods.empty() && s.src != kRest) t += ":" + mods;
  for (int i = 0; i < s.ties; ++i) t += " ~";
  return t;
}

std::string patternToText(const Pattern& p) {
  std::string t;
  for (const auto& s : p.steps) {
    if (!t.empty()) t += ' ';
    t += stepToText(s);
  }
  return t;
}

bool patternFromText(const std::string& text, Pattern& p, std::string* err) {
  std::vector<Step> steps;
  std::istringstream in(text);
  std::string tok;
  while (in >> tok) {
    if (tok == "~") {
      if (steps.empty()) {
        Step r;
        r.src = kRest;
        steps.push_back(r);
      } else {
        ++steps.back().ties;
      }
      continue;
    }
    Step s;
    size_t i = 0;
    if (tok[0] == '.' || tok[0] == '_') {
      s.src = kRest;
      i = 1;
    } else if (std::isalpha(static_cast<unsigned char>(tok[0]))) {
      const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(tok[0])));
      s.slot = true;
      s.src = c - 'A';
      i = 1;
    } else if (std::isdigit(static_cast<unsigned char>(tok[0]))) {
      int v = 0;
      while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) v = v * 10 + (tok[i++] - '0');
      if (v < 1 || v > kMaxSlices) {
        if (err) *err = "slice number out of range: " + tok;
        return false;
      }
      s.slot = false;
      s.src = v - 1;
    } else {
      if (err) *err = "unknown step: " + tok;
      return false;
    }
    while (i < tok.size()) {
      const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(tok[i])));
      if (c == ':') {
        ++i;
        continue;
      }
      ++i;
      // number after the code
      size_t j = i;
      while (j < tok.size() && (std::isdigit(static_cast<unsigned char>(tok[j])) || tok[j] == '+' || tok[j] == '-' || tok[j] == '.')) ++j;
      const std::string ns = tok.substr(i, j - i);
      const double v = ns.empty() ? 0.0 : std::atof(ns.c_str());
      i = j;
      switch (c) {
        case 'r': s.reverse = true; break;
        case 'p': s.pitch = static_cast<float>(std::clamp(v, -48.0, 48.0)); break;
        case 'g': s.gain = static_cast<float>(std::clamp(v, -60.0, 12.0)); break;
        case 'l': s.length = static_cast<float>(std::clamp(v / 100.0, 0.05, 1.0)); break;
        case 'x': s.stutter = std::clamp(static_cast<int>(std::lround(v)), 1, 16); break;
        case 'o': s.offset = static_cast<float>(std::clamp(v / 100.0, 0.0, 0.9)); break;
        default:
          if (err) *err = std::string("unknown option '") + c + "' in " + tok;
          return false;
      }
    }
    if (steps.size() >= static_cast<size_t>(kMaxSteps)) break;
    steps.push_back(s);
  }
  if (steps.empty()) {
    if (err) *err = "empty pattern";
    return false;
  }
  p.steps.swap(steps);
  return true;
}

int stepSlice(const Pattern& p, const Step& s, int count) {
  if (s.src == kRest || count <= 0) return -1;
  if (!s.slot) return s.src % count;
  if (s.src >= 0 && s.src < static_cast<int>(p.slots.size())) {
    const int k = p.slots[static_cast<size_t>(s.src)];
    if (k >= 0 && k < count) return k;
  }
  return s.src % count;
}

namespace {
double unitStart(const Pattern& p, int i) {
  double u = 0;
  for (int k = 0; k < i && k < static_cast<int>(p.steps.size()); ++k) u += 1 + p.steps[static_cast<size_t>(k)].ties;
  return u;
}
}  // namespace

double stepStart(const Pattern& p, int i, double swing) {
  const double u = unitStart(p, i);
  double t = u * p.unitBeats;
  if (swing > 0 && p.unitBeats <= 1.0 + 1e-9 && static_cast<long long>(std::llround(u)) % 2 == 1) t += std::clamp(swing, 0.0, 1.0) * 0.5 * p.unitBeats;
  return t;
}

double stepBeats(const Pattern& p, int i) {
  return (1 + p.steps[static_cast<size_t>(i)].ties) * p.unitBeats;
}

// ------------------------------------------------------------------ text form
std::string modelToText(const Model& m) {
  std::ostringstream o;
  o << "seltmann 1\n";
  o << "src " << m.srcFrames << " " << m.srcRate << "\n";
  o << "analysed " << (m.analysed ? 1 : 0) << "\n";
  Analysis g = m.grid;
  g.onsetEnv.clear();
  o << analysisToText(g);
  o << "region " << m.regionStart << " " << m.regionEnd << "\n";
  o << "chop " << m.chop.mode << " " << m.chop.division << " " << m.chop.sensitivity << " " << m.chop.count << " " << m.chop.grainMs << " "
    << m.chop.snap << " " << (m.chop.zeroCross ? 1 : 0) << " " << (m.chop.autoSize ? 1 : 0) << " " << (m.chop.hitSnap ? 1 : 0) << "\n";
  o << "slices " << m.slices.size() << "\n";
  for (const auto& s : m.slices)
    o << "s " << s.start << " " << s.end << " " << s.pitch << " " << s.gain << " " << (s.reverse ? 1 : 0) << " " << s.fadeIn << " " << s.fadeOut
      << " " << s.offset << " " << s.length << " " << s.choke << "\n";
  o.precision(10);
  o << "pattern " << m.pattern.unitBeats << " " << m.pattern.name << "\n";
  o << "slots";
  for (int k : m.pattern.slots) o << " " << k;
  o << "\n";
  o << "steps " << patternToText(m.pattern) << "\n";
  o << "preset " << m.preset << "\n";
  return o.str();
}

bool modelFromText(const std::string& text, Model& m) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("seltmann ", 0) != 0) return false;
  Model r;
  std::string gridText;
  size_t nSlices = 0;
  while (std::getline(in, line)) {
    std::istringstream l(line);
    std::string k;
    l >> k;
    if (k == "src") l >> r.srcFrames >> r.srcRate;
    else if (k == "analysed") {
      int v = 0;
      l >> v;
      r.analysed = v != 0;
    } else if (k == "rate" || k == "tempo" || k == "meter" || k == "key" || k == "kind" || k == "beats" || k == "sections" || k == "loops")
      gridText += line + "\n";
    else if (k == "region") l >> r.regionStart >> r.regionEnd;
    else if (k == "chop") {
      int z = 1, au = 0, hs = 0;
      l >> r.chop.mode >> r.chop.division >> r.chop.sensitivity >> r.chop.count >> r.chop.grainMs >> r.chop.snap >> z;
      if (l >> au) l >> hs;
      r.chop.zeroCross = z != 0;
      r.chop.autoSize = au != 0;
      r.chop.hitSnap = hs != 0;
      r.chop.mode = std::clamp(r.chop.mode, 0, ChopModeCount - 1);
      r.chop.division = std::clamp(r.chop.division, 0, DivCount - 1);
    } else if (k == "slices") {
      l >> nSlices;
      nSlices = std::min<size_t>(nSlices, kMaxSlices);
    } else if (k == "s") {
      Slice s;
      int rv = 0;
      l >> s.start >> s.end >> s.pitch >> s.gain >> rv >> s.fadeIn >> s.fadeOut >> s.offset >> s.length >> s.choke;
      s.reverse = rv != 0;
      s.offset = std::clamp(s.offset, 0.0f, 0.9f);
      s.length = std::clamp(s.length, 0.05f, 1.0f);
      s.choke = std::clamp(s.choke, 0, 8);
      if (s.end > s.start && r.slices.size() < static_cast<size_t>(kMaxSlices)) r.slices.push_back(s);
    } else if (k == "pattern") {
      l >> r.pattern.unitBeats;
      std::string rest;
      std::getline(l, rest);
      while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
      r.pattern.name = rest;
      r.pattern.unitBeats = std::clamp(r.pattern.unitBeats, 1.0 / 64, 64.0);
    } else if (k == "slots") {
      r.pattern.slots.clear();
      int v;
      while (l >> v && r.pattern.slots.size() < static_cast<size_t>(kSlotCount)) r.pattern.slots.push_back(v);
    } else if (k == "steps") {
      std::string rest;
      std::getline(l, rest);
      Pattern p = r.pattern;
      if (patternFromText(rest, p)) r.pattern.steps = p.steps;
    } else if (k == "preset") {
      std::string rest;
      std::getline(l, rest);
      while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
      r.preset = rest;
    }
  }
  analysisFromText(gridText, r.grid);
  if (r.srcRate <= 0) r.srcRate = r.grid.sampleRate;
  r.grid.sampleRate = r.srcRate;
  r.grid.frames = r.srcFrames;
  r.pattern.slots.resize(kSlotCount, -1);
  m = std::move(r);
  return true;
}

std::string pathOf(const std::string& data) {
  const size_t nl = data.find('\n');
  return nl == std::string::npos ? data : data.substr(0, nl);
}

bool parseData(const std::string& data, std::string& path, Model& m) {
  const size_t nl = data.find('\n');
  path = nl == std::string::npos ? data : data.substr(0, nl);
  if (nl == std::string::npos) return false;
  return modelFromText(data.substr(nl + 1), m);
}

std::string formatData(const std::string& path, const Model& m) { return path + "\n" + modelToText(m); }

}  // namespace gl::packs::seltmann

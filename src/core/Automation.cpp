#include "Automation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include "AutomationEdit.h"
#include "FlCurves.h"
#include "Params.h"

namespace gl {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Single-curve families. All map [0,1] → [0,1]; positive tension = slow start
// (the segment eases in), negative = fast start; t and −t are mirror images
// (1 − f(−t, 1 − x) = f(t, x)), so a bend reads the same rising or falling.
//
// Power family: the exponent 2^(3.3|t|) makes |t| = 0.25 trace a quarter sine
// within ±0.016 — FL's own sine-shape automation presets are built from
// single-curve segments at tension ±0.25, so imported shapes stay round.
double powCurve(double t, double x) {
  double e = std::pow(2.0, 3.3 * std::fabs(t));
  return t >= 0 ? std::pow(x, e) : 1.0 - std::pow(1.0 - x, e);
}
// Exponential family: (e^(kx) − 1) / (e^k − 1).
double expCurve(double t, double x) {
  double k = 9.0 * t;
  if (std::fabs(k) < 1e-6) return x;
  return std::expm1(k * x) / std::expm1(k);
}
// Rational family (sharper knee): x / (x + c(1 − x)).
double ratCurve(double t, double x) {
  double c = std::pow(2.0, 7.0 * t);
  double d = x + c * (1.0 - x);
  return d <= 0 ? x : x / d;
}
// Two mirrored halves of a single curve (S shape for positive tension).
template <typename F>
double twoHalves(F f, double t, double x) {
  return x < 0.5 ? 0.5 * f(t, 2.0 * x) : 1.0 - 0.5 * f(t, 2.0 * (1.0 - x));
}
int stairCount(double t) { return 2 + static_cast<int>(std::lround((t + 1.0) * 7.0)); }  // 2 … 16
}  // namespace

double autoShape(AutoShape shape, double tension, double x) {
  x = std::clamp(x, 0.0, 1.0);
  const double t = std::clamp(tension, -1.0, 1.0);
  switch (shape) {
    case AutoShape::Curve: return powCurve(t, x);
    case AutoShape::Curve2: return expCurve(t, x);
    case AutoShape::Curve3: return ratCurve(t, x);
    case AutoShape::SCurve: return twoHalves(powCurve, t, x);
    case AutoShape::SCurve2: return twoHalves(expCurve, t, x);
    case AutoShape::SCurve3: return twoHalves(ratCurve, t, x);
    case AutoShape::Hold: return x >= 1.0 ? 1.0 : 0.0;
    case AutoShape::Stairs: {
      double n = stairCount(t);
      return std::min(1.0, std::floor(x * n) / (n - 1.0));
    }
    case AutoShape::SmoothStairs: {  // n steps; each riser eases over the last 35 % of its step
      double n = stairCount(t);
      double k = x * n, i = std::floor(k), r = std::clamp((k - i - 0.65) / 0.35, 0.0, 1.0);
      return std::min(1.0, (i + r * r * (3.0 - 2.0 * r)) / n);
    }
    case AutoShape::Smooth: return 0.5 - 0.5 * std::cos(kPi * powCurve(t, x));
    case AutoShape::HalfSine: {
      double base = 0.5 - 0.5 * std::cos(kPi * x);
      double side = t >= 0 ? 1.0 - std::cos(0.5 * kPi * x) : std::sin(0.5 * kPi * x);
      return base + (side - base) * std::fabs(t);
    }
    case AutoShape::Pulse: {
      double rate = 1.0 + std::round((t + 1.0) * 7.5);
      return std::fmod(x * rate, 1.0) < 0.5 ? 0.0 : 1.0;
    }
    case AutoShape::Wave: {
      double rate = 1.0 + std::round((t + 1.0) * 7.5);
      return 0.5 - 0.5 * std::cos(2.0 * kPi * x * rate);
    }
    default: return x;
  }
}

double flAutoShape(const std::vector<AutoPoint>& p, size_t k, double x) {
  const AutoPoint& b = p[k];
  const int mode = autoedit::flModeOf(b.shape);
  const double t = -static_cast<double>(b.tension);  // FL's tension sign
  if (mode != flcurve::kSmooth || k == 0) return flcurve::shape(mode, t, x);
  // Smooth: tangents from neighbouring smooth segments (FlCurves.h).
  const double T = static_cast<double>(p[k].pos - p[k - 1].pos), d = static_cast<double>(p[k].value) - p[k - 1].value;
  double ps = 0, pe = 0;
  if (T > 0 && std::fabs(d) >= 1.0 / 16777216.0) {
    const double slope = d / T;
    if (k >= 2 && p[k - 1].shape == AutoShape::Smooth && p[k - 1].pos > p[k - 2].pos) {
      const double Tp = static_cast<double>(p[k - 1].pos - p[k - 2].pos);
      ps = flcurve::smoothTangent((static_cast<double>(p[k - 1].value) - p[k - 2].value) / Tp / slope, Tp / T);
    }
    if (k + 1 < p.size() && p[k + 1].shape == AutoShape::Smooth && p[k + 1].pos > p[k].pos) {
      const double Tn = static_cast<double>(p[k + 1].pos - p[k].pos);
      pe = flcurve::smoothTangent((static_cast<double>(p[k + 1].value) - p[k].value) / Tn / slope, Tn / T);
    }
  }
  return flcurve::smooth(t, std::clamp(x, 0.0, 1.0), ps, pe);
}

double pointsValue(const std::vector<AutoPoint>& p, double t, bool fl) {
  if (p.empty()) return 0.0;
  if (t <= static_cast<double>(p.front().pos)) return p.front().value;
  if (t >= static_cast<double>(p.back().pos)) return p.back().value;
  auto it = std::upper_bound(p.begin(), p.end(), t, [](double v, const AutoPoint& q) { return v < static_cast<double>(q.pos); });
  const AutoPoint& b = *it;
  const AutoPoint& a0 = *(it - 1);
  double span = static_cast<double>(b.pos - a0.pos);
  if (span <= 0) return b.value;
  double x = (t - static_cast<double>(a0.pos)) / span;
  const double y = fl ? flAutoShape(p, static_cast<size_t>(it - p.begin()), x) : autoShape(b.shape, b.tension, x);
  return a0.value + (b.value - a0.value) * y;
}

double automationValue(const AutomationClip& a, double t) {
  if (a.points.empty()) return 0.0;
  if (a.loop > 0 && t >= static_cast<double>(a.loop)) t = std::fmod(t, static_cast<double>(a.loop));
  return pointsValue(a.points, t, a.flCurves);
}

int insertAutoPoint(AutomationClip& a, const AutoPoint& p) {
  auto it = std::upper_bound(a.points.begin(), a.points.end(), p.pos, [](Tick v, const AutoPoint& q) { return v < q.pos; });
  it = a.points.insert(it, p);
  return static_cast<int>(it - a.points.begin());
}

const char* autoShapeName(AutoShape s) {
  switch (s) {
    case AutoShape::Curve: return "Curve";
    case AutoShape::SCurve: return "S-curve";
    case AutoShape::Hold: return "Hold";
    case AutoShape::Stairs: return "Stairs";
    case AutoShape::Smooth: return "Smooth";
    case AutoShape::Pulse: return "Pulse";
    case AutoShape::Wave: return "Wave";
    case AutoShape::Curve2: return "Curve 2";
    case AutoShape::Curve3: return "Curve 3";
    case AutoShape::SCurve2: return "S-curve 2";
    case AutoShape::SCurve3: return "S-curve 3";
    case AutoShape::HalfSine: return "Half sine";
    case AutoShape::SmoothStairs: return "Smooth stairs";
    default: return "?";
  }
}

double AutomationLane::valueAt(double tick) const {
  if (inst.empty()) return 0.0;
  auto it = std::upper_bound(inst.begin(), inst.end(), tick, [](double v, const Instance& q) { return v < static_cast<double>(q.start); });
  const Instance& in = it == inst.begin() ? inst.front() : *(it - 1);
  double rel = it == inst.begin() ? 0.0 : std::min(tick, static_cast<double>(in.end)) - static_cast<double>(in.start);
  const AutomationClip& c = *in.clip;
  double v = automationValue(c, static_cast<double>(in.offset) + rel);
  return std::clamp(c.min + v * (c.max - c.min), 0.0, 1.0);
}

std::vector<AutomationLane> automationLanes(const Song& song) {
  std::map<ObjectId, std::shared_ptr<const AutomationClip>> copies;
  std::map<std::pair<ObjectId, uint32_t>, AutomationLane> lanes;
  for (const auto& c : song.playlist) {
    if (c.kind != ClipKind::Automation || c.muted || c.start < song.live.areaEnd) continue;  // live area: launched by hand
    if (c.track >= 0 && static_cast<size_t>(c.track) < song.tracks.size() && song.tracks[static_cast<size_t>(c.track)].muted) continue;
    const AutomationClip* a = song.automation(c.source);
    if (!a || a->points.empty() || a->owner == kNoId) continue;
    auto& cp = copies[a->id];
    if (!cp) cp = std::make_shared<const AutomationClip>(*a);
    AutomationLane& l = lanes[{a->owner, a->index}];
    l.owner = a->owner;
    l.index = a->index;
    l.inst.push_back({c.start, c.start + song.clipLength(c), c.offset, cp});
  }
  std::vector<AutomationLane> out;
  for (auto& [k, l] : lanes) {
    std::stable_sort(l.inst.begin(), l.inst.end(), [](const auto& x, const auto& y) { return x.start < y.start; });
    for (size_t i = 0; i + 1 < l.inst.size(); ++i) l.inst[i].end = std::min(l.inst[i].end, l.inst[i + 1].start);
    // Instances cut to nothing by a clip starting at the same tick go away.
    std::vector<AutomationLane::Instance> keep;
    for (size_t i = 0; i < l.inst.size(); ++i)
      if (l.inst[i].end > l.inst[i].start || i + 1 == l.inst.size()) keep.push_back(l.inst[i]);
    l.inst = std::move(keep);
    out.push_back(std::move(l));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Tempo map

void TempoMap::setConstant(double bpm, int ppq) {
  ppq_ = std::max(1, ppq);
  segs_.assign(1, Seg{0.0, std::clamp(bpm, 1.0, 1000.0), 0.0});
  finish();
}

void TempoMap::setSegments(const std::vector<std::pair<double, double>>& tickBpm, int ppq) {
  ppq_ = std::max(1, ppq);
  segs_.clear();
  for (const auto& [t, b] : tickBpm) {
    double bpm = std::clamp(b, 1.0, 1000.0);
    if (segs_.empty()) {
      segs_.push_back({0.0, bpm, 0.0});
    } else if (t > segs_.back().tick && bpm != segs_.back().bpm) {
      segs_.push_back({t, bpm, 0.0});
    }
  }
  if (segs_.empty()) segs_.push_back({0.0, 120.0, 0.0});
  finish();
}

void TempoMap::build(const Song& song) {
  const AutomationLane* lane = nullptr;
  auto lanes = automationLanes(song);
  for (const auto& l : lanes)
    if (l.targets(kSongId, static_cast<uint32_t>(SongParam::Tempo))) lane = &l;
  if (!lane) {
    setConstant(song.tempo, song.ppq);
    return;
  }
  auto desc = describeParam(song, songParam(SongParam::Tempo));
  Tick end = std::max(song.lengthTicks(), lane->inst.back().end) + song.ppq;
  Tick step = std::max<Tick>(1, song.ppq / 32);
  if (end / step > 200000) step = end / 200000 + 1;
  std::vector<std::pair<double, double>> pts;
  for (Tick t = 0; t < end; t += step) {
    double n = lane->valueAt(static_cast<double>(t) + static_cast<double>(step) * 0.5);
    pts.push_back({static_cast<double>(t), desc->clamp(desc->fromNormalized(n))});
  }
  setSegments(pts, song.ppq);
}

void TempoMap::finish() {
  uint64_t h = 1469598103934665603ull ^ static_cast<uint64_t>(ppq_);
  auto mix = [&](double v) {
    uint64_t b;
    static_assert(sizeof b == sizeof v, "double size");
    std::memcpy(&b, &v, sizeof b);
    h = (h ^ b) * 1099511628211ull;
  };
  double sec = 0;
  for (size_t i = 0; i < segs_.size(); ++i) {
    if (i > 0) sec += (segs_[i].tick - segs_[i - 1].tick) * 60.0 / (segs_[i - 1].bpm * ppq_);
    segs_[i].sec = sec;
    mix(segs_[i].tick);
    mix(segs_[i].bpm);
  }
  sig_ = h;
}

size_t TempoMap::segAt(double tick) const {
  auto it = std::upper_bound(segs_.begin(), segs_.end(), tick, [](double v, const Seg& s) { return v < s.tick; });
  return it == segs_.begin() ? 0 : static_cast<size_t>(it - segs_.begin()) - 1;
}

size_t TempoMap::segAtSec(double sec) const {
  auto it = std::upper_bound(segs_.begin(), segs_.end(), sec, [](double v, const Seg& s) { return v < s.sec; });
  return it == segs_.begin() ? 0 : static_cast<size_t>(it - segs_.begin()) - 1;
}

double TempoMap::secondsAt(double tick) const {
  const Seg& s = segs_[segAt(tick)];
  return s.sec + (tick - s.tick) * 60.0 / (s.bpm * ppq_);
}

double TempoMap::tickAtSeconds(double sec) const {
  const Seg& s = segs_[segAtSec(sec)];
  return s.tick + (sec - s.sec) * s.bpm * ppq_ / 60.0;
}

double TempoMap::samplesBetween(double t0, double t1, double sampleRate) const {
  size_t i = segAt(t0);
  if (segAt(t1) == i) return (t1 - t0) * (sampleRate * 60.0 / (segs_[i].bpm * ppq_));
  return (secondsAt(t1) - secondsAt(t0)) * sampleRate;
}

double TempoMap::tickAfterSamples(double t0, double samples, double sampleRate) const {
  size_t i = segAt(t0);
  double cand = t0 + samples / (sampleRate * 60.0 / (segs_[i].bpm * ppq_));
  if (segAt(cand) == i) return cand;
  return tickAtSeconds(secondsAt(t0) + samples / sampleRate);
}

}  // namespace gl

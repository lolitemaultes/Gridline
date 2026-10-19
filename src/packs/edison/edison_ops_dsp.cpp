// Gridline — "edison" pack: editing model (audio, content versions, undo
// history) and the time-domain edit operations. Every operation is a pure
// function of the current version, so it can run on a worker thread and its
// result becomes one undo step.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include "../../engine/Dsp.h"
#include "edison.h"
#include "edison_internal.h"

namespace gl::packs::edison {

using namespace gl::dsp;

// ------------------------------------------------------------------ basics
void Audio::resize(int64_t n) {
  L.resize(static_cast<size_t>(std::max<int64_t>(0, n)), 0.f);
  R.resize(static_cast<size_t>(std::max<int64_t>(0, n)), 0.f);
}

Audio makeAudio(double rate, int channels, int64_t frames) {
  Audio a;
  a.rate = rate;
  a.channels = channels == 1 ? 1 : 2;
  a.resize(frames);
  return a;
}

const Audio& Content::a() const {
  static const Audio empty;
  return audio ? *audio : empty;
}

Content makeContent(Audio a) {
  Content c;
  c.audio = std::make_shared<const Audio>(std::move(a));
  return c;
}

Range clampRange(const Content& c, Range r) {
  const int64_t n = c.frames();
  if (r.a > r.b) std::swap(r.a, r.b);
  r.a = std::clamp<int64_t>(r.a, 0, n);
  r.b = std::clamp<int64_t>(r.b, 0, n);
  return r;
}

Range rangeOrAll(const Content& c, Range r) {
  r = clampRange(c, r);
  return r.empty() ? Range{0, c.frames()} : r;
}

void fixMono(Audio& a) {
  if (a.channels == 1) a.R = a.L;
}

Content withAudio(const Content& c, Audio a) {
  Content out = c;
  fixMono(a);
  out.audio = std::make_shared<const Audio>(std::move(a));
  const int64_t n = out.frames();
  // Keep markers / regions / loop inside the (possibly shorter) audio.
  out.markers.erase(std::remove_if(out.markers.begin(), out.markers.end(), [n](const Marker& m) { return m.pos > n; }),
                    out.markers.end());
  for (auto& g : out.regions) g.end = std::min(g.end, n);
  out.regions.erase(std::remove_if(out.regions.begin(), out.regions.end(), [](const Region& g) { return g.end <= g.start; }),
                    out.regions.end());
  if (out.loopEnd > n) out.loopEnd = n;
  if (!out.hasLoop()) out.loopStart = out.loopEnd = -1;
  return out;
}

// ------------------------------------------------------------------ history
History::History() : History(makeContent(Audio{})) {}
History::History(Content c) : cur_(std::move(c)) {
  if (!cur_.audio) cur_.audio = std::make_shared<const Audio>();
}

void History::reset(Content c) {
  cur_ = std::move(c);
  if (!cur_.audio) cur_.audio = std::make_shared<const Audio>();
  undo_.clear();
  redo_.clear();
  ++serial_;
}

void History::push(const std::string& label, Content next) {
  if (!next.audio) next.audio = std::make_shared<const Audio>();
  undo_.emplace_back(label, std::move(cur_));
  if (undo_.size() > max_) undo_.erase(undo_.begin());
  cur_ = std::move(next);
  redo_.clear();
  ++serial_;
}

bool History::undo() {
  if (undo_.empty()) return false;
  redo_.emplace_back(undo_.back().first, std::move(cur_));
  cur_ = std::move(undo_.back().second);
  undo_.pop_back();
  ++serial_;
  return true;
}

bool History::redo() {
  if (redo_.empty()) return false;
  undo_.emplace_back(redo_.back().first, std::move(cur_));
  cur_ = std::move(redo_.back().second);
  redo_.pop_back();
  ++serial_;
  return true;
}

std::vector<std::string> History::labels() const {
  std::vector<std::string> v;
  for (const auto& u : undo_) v.push_back(u.first);
  return v;
}

// ------------------------------------------------------------------ marker bookkeeping
namespace {

// Positions after removing [a, b).
int64_t mapDeleted(int64_t p, Range r) { return p < r.a ? p : p < r.b ? r.a : p - r.len(); }

void shiftForDelete(Content& c, Range r) {
  std::vector<Marker> ms;
  for (const auto& m : c.markers)
    if (m.pos < r.a || m.pos >= r.b) ms.push_back({mapDeleted(m.pos, r), m.name});
  c.markers = std::move(ms);
  std::vector<Region> rs;
  for (const auto& g : c.regions) {
    Region n{mapDeleted(g.start, r), g.end <= r.a ? g.end : g.end <= r.b ? r.a : g.end - r.len(), g.name};
    if (n.end > n.start) rs.push_back(n);
  }
  c.regions = std::move(rs);
  if (c.hasLoop()) {
    int64_t s = mapDeleted(c.loopStart, r), e = c.loopEnd <= r.a ? c.loopEnd : c.loopEnd <= r.b ? r.a : c.loopEnd - r.len();
    if (e > s)
      c.loopStart = s, c.loopEnd = e;
    else
      c.loopStart = c.loopEnd = -1;
  }
}

void shiftForInsert(Content& c, int64_t at, int64_t n) {
  for (auto& m : c.markers)
    if (m.pos >= at) m.pos += n;
  for (auto& g : c.regions) {
    if (g.start >= at) g.start += n;
    if (g.end > at) g.end += n;
  }
  if (c.hasLoop()) {
    if (c.loopStart >= at) c.loopStart += n;
    if (c.loopEnd > at) c.loopEnd += n;
  }
}

// Clip converted to the content's rate and channel count.
Audio conform(const Audio& clip, const Audio& to) {
  Audio a = clip;
  if (std::fabs(a.rate - to.rate) > 1e-6 && a.frames() > 0) a = resampleAudio(a, to.rate, 1);
  a.rate = to.rate;
  if (to.channels == 1 && a.channels == 2) {
    for (size_t i = 0; i < a.L.size(); ++i) a.L[i] = 0.5f * (a.L[i] + a.R[i]);
    a.R = a.L;
  }
  a.channels = to.channels;
  fixMono(a);
  return a;
}

template <class F>
Content perSample(const Content& c, Range r, F f) {
  r = clampRange(c, r);
  Audio a = c.a();
  for (int ch = 0; ch < 2; ++ch) {
    float* x = a.ch(ch);
    for (int64_t i = r.a; i < r.b; ++i) x[i] = f(x[i], i - r.a, r.len());
  }
  return withAudio(c, std::move(a));
}

}  // namespace

// ------------------------------------------------------------------ cut / paste / trim
Audio extract(const Audio& a, Range r) {
  r.a = std::clamp<int64_t>(r.a, 0, a.frames());
  r.b = std::clamp<int64_t>(r.b, r.a, a.frames());
  Audio o;
  o.rate = a.rate;
  o.channels = a.channels;
  o.L.assign(a.L.begin() + r.a, a.L.begin() + r.b);
  o.R.assign(a.R.begin() + r.a, a.R.begin() + r.b);
  return o;
}

Content deleteRange(const Content& c, Range r) {
  r = clampRange(c, r);
  if (r.empty()) return c;
  Audio a = c.a();
  a.L.erase(a.L.begin() + r.a, a.L.begin() + r.b);
  a.R.erase(a.R.begin() + r.a, a.R.begin() + r.b);
  Content out = c;
  shiftForDelete(out, r);
  return withAudio(out, std::move(a));
}

Content insertAudio(const Content& c, int64_t at, const Audio& clipIn) {
  const Audio& base = c.a();
  // An empty editor takes the clip's own format.
  Audio clip = base.frames() == 0 && c.markers.empty() ? clipIn : conform(clipIn, base);
  at = std::clamp<int64_t>(at, 0, base.frames());
  Audio a = base.frames() == 0 ? makeAudio(clip.rate, clip.channels, 0) : base;
  a.L.insert(a.L.begin() + at, clip.L.begin(), clip.L.end());
  a.R.insert(a.R.begin() + at, clip.R.begin(), clip.R.end());
  Content out = c;
  shiftForInsert(out, at, clip.frames());
  return withAudio(out, std::move(a));
}

Content replaceRange(const Content& c, Range r, const Audio& clip) {
  r = clampRange(c, r);
  return insertAudio(deleteRange(c, r), r.a, clip);
}

Content insertSilence(const Content& c, int64_t at, int64_t frames) {
  Audio s = makeAudio(c.rate(), c.a().channels, std::max<int64_t>(0, frames));
  return insertAudio(c, at, s);
}

Content silenceRange(const Content& c, Range r) {
  return perSample(c, r, [](float, int64_t, int64_t) { return 0.f; });
}

Content cropTo(const Content& c, Range r) {
  r = clampRange(c, r);
  if (r.empty()) return c;
  Content out = deleteRange(c, {r.b, c.frames()});
  return deleteRange(out, {0, r.a});
}

Content trimSilence(const Content& c, double thresholdDb, Range* kept) {
  const Audio& a = c.a();
  const float thr = static_cast<float>(dbToGain(thresholdDb));
  int64_t s = 0, e = a.frames();
  auto loud = [&](int64_t i) { return std::fabs(a.L[static_cast<size_t>(i)]) >= thr || std::fabs(a.R[static_cast<size_t>(i)]) >= thr; };
  while (s < e && !loud(s)) ++s;
  while (e > s && !loud(e - 1)) --e;
  if (kept) *kept = {s, e};
  if (s == 0 && e == a.frames()) return c;
  if (e <= s) return deleteRange(c, {0, a.frames()});
  return cropTo(c, {s, e});
}

// ------------------------------------------------------------------ fades and levels
const char* curveName(Curve c) {
  switch (c) {
    case Curve::Linear: return "Linear";
    case Curve::Fast: return "Fast start";
    case Curve::Slow: return "Slow start";
    case Curve::SCurve: return "S-curve";
    case Curve::EqualPower: return "Equal power";
    default: return "?";
  }
}

double curveGain(Curve c, double x) {
  x = std::clamp(x, 0.0, 1.0);
  switch (c) {
    case Curve::Fast: return 1.0 - (1.0 - x) * (1.0 - x);
    case Curve::Slow: return x * x;
    case Curve::SCurve: return x * x * (3.0 - 2.0 * x);
    case Curve::EqualPower: return std::sin(x * kPi * 0.5);
    default: return x;
  }
}

Content fadeIn(const Content& c, Range r, Curve k) {
  return perSample(c, r, [k](float v, int64_t i, int64_t n) {
    double x = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 1.0;
    return static_cast<float>(v * curveGain(k, x));
  });
}

Content fadeOut(const Content& c, Range r, Curve k) {
  return perSample(c, r, [k](float v, int64_t i, int64_t n) {
    double x = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 1.0;
    return static_cast<float>(v * curveGain(k, 1.0 - x));
  });
}

Content crossfadeLoop(const Content& c, Range loop, int64_t fadeFrames) {
  loop = clampRange(c, loop);
  fadeFrames = std::min<int64_t>(fadeFrames, std::min(loop.a, loop.len() / 2));
  if (fadeFrames <= 0) return c;
  Audio a = c.a();
  // The last `fadeFrames` of the loop fade into the audio just before its start.
  for (int ch = 0; ch < 2; ++ch) {
    float* x = a.ch(ch);
    for (int64_t i = 0; i < fadeFrames; ++i) {
      double t = static_cast<double>(i + 1) / static_cast<double>(fadeFrames + 1);
      int64_t dst = loop.b - fadeFrames + i, src = loop.a - fadeFrames + i;
      x[dst] = static_cast<float>(x[dst] * std::cos(t * kPi * 0.5) + x[src] * std::sin(t * kPi * 0.5));
    }
  }
  return withAudio(c, std::move(a));
}

Content gain(const Content& c, Range r, double db) {
  const float g = static_cast<float>(dbToGain(db));
  return perSample(c, r, [g](float v, int64_t, int64_t) { return v * g; });
}

Content normalize(const Content& c, Range r, double targetDb) {
  r = clampRange(c, r);
  const Audio& a = c.a();
  float peak = 0;
  for (int64_t i = r.a; i < r.b; ++i)
    peak = std::max({peak, std::fabs(a.L[static_cast<size_t>(i)]), std::fabs(a.R[static_cast<size_t>(i)])});
  if (peak <= 0) return c;
  const double g = dbToGain(targetDb) / peak;
  return perSample(c, r, [g](float v, int64_t, int64_t) { return static_cast<float>(v * g); });
}

Content reverse(const Content& c, Range r) {
  r = clampRange(c, r);
  Audio a = c.a();
  std::reverse(a.L.begin() + r.a, a.L.begin() + r.b);
  std::reverse(a.R.begin() + r.a, a.R.begin() + r.b);
  return withAudio(c, std::move(a));
}

Content invert(const Content& c, Range r) {
  return perSample(c, r, [](float v, int64_t, int64_t) { return -v; });
}

Content removeDc(const Content& c, Range r) {
  r = clampRange(c, r);
  if (r.empty()) return c;
  Audio a = c.a();
  for (int ch = 0; ch < 2; ++ch) {
    float* x = a.ch(ch);
    double sum = 0;
    for (int64_t i = r.a; i < r.b; ++i) sum += x[i];
    const double mean = sum / static_cast<double>(r.len());
    for (int64_t i = r.a; i < r.b; ++i) x[i] = static_cast<float>(x[i] - mean);
  }
  return withAudio(c, std::move(a));
}

Content swapChannels(const Content& c) {
  Audio a = c.a();
  std::swap(a.L, a.R);
  return withAudio(c, std::move(a));
}

Content toMono(const Content& c) {
  Audio a = c.a();
  for (size_t i = 0; i < a.L.size(); ++i) a.L[i] = 0.5f * (a.L[i] + a.R[i]);
  a.channels = 1;
  return withAudio(c, std::move(a));
}

Content toStereo(const Content& c) {
  Audio a = c.a();
  a.channels = 2;
  a.R = a.L;
  return withAudio(c, std::move(a));
}

Content applyEnvelope(const Content& c, Range r, const std::vector<std::pair<double, double>>& pts) {
  if (pts.empty()) return c;
  std::vector<std::pair<double, double>> p = pts;
  std::sort(p.begin(), p.end());
  auto at = [&p](double x) {
    if (x <= p.front().first) return p.front().second;
    if (x >= p.back().first) return p.back().second;
    for (size_t k = 1; k < p.size(); ++k)
      if (x <= p[k].first) {
        double span = p[k].first - p[k - 1].first;
        double t = span > 0 ? (x - p[k - 1].first) / span : 1.0;
        return p[k - 1].second + (p[k].second - p[k - 1].second) * t;
      }
    return p.back().second;
  };
  return perSample(c, r, [&at](float v, int64_t i, int64_t n) {
    double x = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.0;
    return static_cast<float>(v * at(x));
  });
}

// ------------------------------------------------------------------ gate / declick
Content gate(const Content& c, Range r, double thresholdDb, double attackMs, double holdMs, double releaseMs, double floorDb) {
  r = clampRange(c, r);
  Audio a = c.a();
  const double sr = a.rate;
  const double thr = dbToGain(thresholdDb), floorG = floorDb <= -90 ? 0.0 : dbToGain(floorDb);
  const double att = attackMs > 0 ? 1.0 / (attackMs * 0.001 * sr) : 1.0;
  const double rel = releaseMs > 0 ? 1.0 / (releaseMs * 0.001 * sr) : 1.0;
  const int64_t hold = static_cast<int64_t>(holdMs * 0.001 * sr);
  // Look-ahead of the attack time so the gate is open when a transient starts.
  const int64_t look = static_cast<int64_t>(attackMs * 0.001 * sr);
  double g = 0;
  int64_t held = 0;
  std::vector<float> gains(static_cast<size_t>(r.len()));
  for (int64_t i = r.a; i < r.b; ++i) {
    int64_t j = std::min(r.b - 1, i + look);
    double lv = std::max(std::fabs(a.L[static_cast<size_t>(j)]), std::fabs(a.R[static_cast<size_t>(j)]));
    if (lv >= thr) held = hold + look;
    else if (held > 0) --held;
    const bool open = lv >= thr || held > 0;
    g = open ? std::min(1.0, g + att) : std::max(0.0, g - rel);
    gains[static_cast<size_t>(i - r.a)] = static_cast<float>(floorG + (1.0 - floorG) * g);
  }
  for (int ch = 0; ch < 2; ++ch) {
    float* x = a.ch(ch);
    for (int64_t i = r.a; i < r.b; ++i) x[i] *= gains[static_cast<size_t>(i - r.a)];
  }
  return withAudio(c, std::move(a));
}

Content declick(const Content& c, Range r, double sensitivity, int* fixedOut) {
  r = clampRange(c, r);
  Audio a = c.a();
  int fixed = 0;
  // A click is a sample far from the cubic prediction of its neighbours,
  // measured against the local median deviation; it and its neighbours are
  // re-drawn with a cubic through the samples around them.
  const double k = 4.0 + (1.0 - std::clamp(sensitivity, 0.0, 1.0)) * 16.0;
  const int win = 64;
  std::vector<double> dev;
  for (int ch = 0; ch < 2; ++ch) {
    float* x = a.ch(ch);
    const int64_t n = a.frames();
    std::vector<float> d(static_cast<size_t>(n), 0.f);
    for (int64_t i = std::max<int64_t>(r.a, 2); i < std::min(r.b, n - 2); ++i) {
      double pred = (-x[i - 2] + 4.0 * x[i - 1] + 4.0 * x[i + 1] - x[i + 2]) / 6.0;
      d[static_cast<size_t>(i)] = static_cast<float>(std::fabs(x[i] - pred));
    }
    for (int64_t i = std::max<int64_t>(r.a, 4); i < std::min(r.b, n - 4); ++i) {
      int64_t lo = std::max<int64_t>(0, i - win), hi = std::min<int64_t>(n, i + win);
      dev.assign(d.begin() + lo, d.begin() + hi);
      std::nth_element(dev.begin(), dev.begin() + static_cast<long>(dev.size() / 2), dev.end());
      double med = std::max(1e-6, dev[dev.size() / 2]);
      if (d[static_cast<size_t>(i)] > k * med && d[static_cast<size_t>(i)] > 1e-3) {
        // Neighbours of a click also miss their prediction: centre on the worst one.
        for (int64_t j = i + 1; j <= std::min(i + 4, std::min(r.b, n - 4) - 1); ++j)
          if (d[static_cast<size_t>(j)] > d[static_cast<size_t>(i)]) i = j;
        // Re-draw i-1..i+1 from x[i-3], x[i-2], x[i+2], x[i+3] (cubic Lagrange).
        const double t[4] = {-3, -2, 2, 3};
        const double v[4] = {x[i - 3], x[i - 2], x[i + 2], x[i + 3]};
        for (int64_t q = i - 1; q <= i + 1; ++q) {
          double s = static_cast<double>(q - i), y = 0;
          for (int m = 0; m < 4; ++m) {
            double w = 1;
            for (int o = 0; o < 4; ++o)
              if (o != m) w *= (s - t[o]) / (t[m] - t[o]);
            y += w * v[m];
          }
          x[q] = static_cast<float>(y);
        }
        for (int64_t q = i - 2; q <= i + 2; ++q) d[static_cast<size_t>(q)] = 0;
        if (ch == 0 || a.channels == 2) ++fixed;
        i += 2;
      }
    }
  }
  if (fixedOut) *fixedOut = fixed;
  return withAudio(c, std::move(a));
}

// ------------------------------------------------------------------ markers / regions / loop
Content addMarker(const Content& c, int64_t pos, const std::string& name) {
  Content o = c;
  pos = std::clamp<int64_t>(pos, 0, c.frames());
  auto it = std::upper_bound(o.markers.begin(), o.markers.end(), pos, [](int64_t p, const Marker& m) { return p < m.pos; });
  o.markers.insert(it, Marker{pos, name});
  return o;
}

Content removeMarker(const Content& c, size_t i) {
  Content o = c;
  if (i < o.markers.size()) o.markers.erase(o.markers.begin() + static_cast<long>(i));
  return o;
}

Content renameMarker(const Content& c, size_t i, const std::string& name) {
  Content o = c;
  if (i < o.markers.size()) o.markers[i].name = name;
  return o;
}

Content moveMarker(const Content& c, size_t i, int64_t pos) {
  if (i >= c.markers.size()) return c;
  std::string name = c.markers[i].name;
  return addMarker(removeMarker(c, i), pos, name);
}

Content addRegion(const Content& c, Range r, const std::string& name) {
  r = clampRange(c, r);
  if (r.empty()) return c;
  Content o = c;
  auto it = std::upper_bound(o.regions.begin(), o.regions.end(), r.a, [](int64_t p, const Region& g) { return p < g.start; });
  o.regions.insert(it, Region{r.a, r.b, name});
  return o;
}

Content removeRegion(const Content& c, size_t i) {
  Content o = c;
  if (i < o.regions.size()) o.regions.erase(o.regions.begin() + static_cast<long>(i));
  return o;
}

Content renameRegion(const Content& c, size_t i, const std::string& name) {
  Content o = c;
  if (i < o.regions.size()) o.regions[i].name = name;
  return o;
}

Content setLoop(const Content& c, Range r) {
  Content o = c;
  r = clampRange(c, r);
  if (r.empty())
    o.loopStart = o.loopEnd = -1;
  else
    o.loopStart = r.a, o.loopEnd = r.b;
  return o;
}

Content clearMarkers(const Content& c) {
  Content o = c;
  o.markers.clear();
  return o;
}

Content setSliceMarkers(const Content& c, const std::vector<int64_t>& onsets) {
  std::vector<int64_t> s = onsets;
  std::sort(s.begin(), s.end());
  s.erase(std::unique(s.begin(), s.end()), s.end());
  Content o = c;
  o.markers.clear();
  int k = 1;
  for (int64_t p : s) {
    if (p < 0 || p > c.frames()) continue;
    char name[32];
    std::snprintf(name, sizeof name, "Slice %d", k++);
    o.markers.push_back({p, name});
  }
  return o;
}

Content snapMarkersToZero(const Content& c, int64_t radius) {
  Content o = c;
  for (auto& m : o.markers) m.pos = nearestZeroCrossing(c.a(), m.pos, radius);
  std::stable_sort(o.markers.begin(), o.markers.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
  return o;
}

int64_t nearestZeroCrossing(const Audio& a, int64_t pos, int64_t radius) {
  const int64_t n = a.frames();
  if (n < 2) return std::clamp<int64_t>(pos, 0, n);
  pos = std::clamp<int64_t>(pos, 0, n - 1);
  auto mid = [&a](int64_t i) { return a.L[static_cast<size_t>(i)] + a.R[static_cast<size_t>(i)]; };
  // A crossing at i: sign change between i-1 and i (or an exact zero at i).
  auto crossing = [&](int64_t i) {
    if (i <= 0 || i >= n) return false;
    float p = mid(i - 1), q = mid(i);
    return q == 0.f || (p < 0.f) != (q < 0.f);
  };
  for (int64_t d = 0; d <= radius; ++d) {
    if (crossing(pos - d)) return pos - d;
    if (crossing(pos + d)) return pos + d;
  }
  return pos;
}

// ------------------------------------------------------------------ sample registry
namespace {
struct Registry {
  std::mutex mu;
  std::vector<std::pair<std::string, SamplePtr>> items;  // most recent last
  std::vector<std::string> failed;                        // paths that did not load
};
Registry& registry() {
  static Registry r;
  return r;
}
}  // namespace

void publishSample(const std::string& path, SamplePtr s) {
  if (path.empty() || !s) return;
  Registry& r = registry();
  std::lock_guard<std::mutex> lk(r.mu);
  r.items.erase(std::remove_if(r.items.begin(), r.items.end(), [&](const auto& p) { return p.first == path; }), r.items.end());
  r.failed.erase(std::remove(r.failed.begin(), r.failed.end(), path), r.failed.end());
  r.items.emplace_back(path, std::move(s));
  if (r.items.size() > 8) r.items.erase(r.items.begin());
}

SamplePtr sampleFor(const std::string& path) {
  if (path.empty()) return nullptr;
  Registry& r = registry();
  {
    std::lock_guard<std::mutex> lk(r.mu);
    for (auto it = r.items.rbegin(); it != r.items.rend(); ++it)
      if (it->first == path) return it->second;
    if (std::find(r.failed.begin(), r.failed.end(), path) != r.failed.end()) return nullptr;
  }
  SamplePtr s = loadSample(path);
  if (s) {
    publishSample(path, s);
  } else {
    std::lock_guard<std::mutex> lk(r.mu);
    r.failed.push_back(path);
  }
  return s;
}

SamplePtr toSample(const Audio& a, const std::string& path) {
  auto s = std::make_shared<Sample>();
  s->path = path;
  s->sampleRate = a.rate;
  s->left = a.L;
  s->right = a.R;
  return s;
}

Audio fromSample(const Sample& s) {
  Audio a;
  a.rate = s.sampleRate;
  a.L = s.left;
  a.R = s.right.size() == s.left.size() ? s.right : s.left;
  a.channels = a.L == a.R ? 1 : 2;
  return a;
}

}  // namespace gl::packs::edison

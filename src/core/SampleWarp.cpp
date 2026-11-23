// Gridline: warp markers on a sample. See SampleWarp.h.
#include "SampleWarp.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "Json.h"
#include "PluginInfo.h"

namespace gl {

namespace {

std::vector<WarpPoint> pointsOf(const std::vector<WarpMarker>& m) { return warpPoints(m, false, 0.0); }

int indexOf(const std::vector<WarpMarker>& m, uint32_t id) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m[i].id == id) return static_cast<int>(i);
  return -1;
}

// Inserts a marker at file second s keeping the current timing; its id (0: too close to another, full).
uint32_t insertAt(std::vector<WarpMarker>& m, double s) {
  for (const WarpMarker& k : m)
    if (std::fabs(k.at - s) < 0.001) return 0;
  if (m.size() >= kMaxWarpMarkers) return 0;
  const WarpMarker n{nextWarpId(m), s, warpTimeAt(pointsOf(m), s)};
  m.insert(std::upper_bound(m.begin(), m.end(), n, [](const WarpMarker& p, const WarpMarker& q) { return p.at < q.at; }), n);
  return n.id;
}

// The file's start is pinned by a marker at its first sample (so moving a marker never moves the start).
void pinStart(std::vector<WarpMarker>& m) {
  for (const WarpMarker& k : m)
    if (k.at <= 1e-9) return;
  insertAt(m, 0.0);
}

bool commit(std::vector<WarpMarker>& m, std::vector<WarpMarker> next, double fileSec) {
  normalizeWarp(next, std::max(0.0, fileSec));
  if (next == m) return false;
  m = std::move(next);
  return true;
}

}  // namespace

std::string sampleWarpToText(const SampleWarp& w) {
  if (!w.on && w.bpm <= 0 && w.markers.empty()) return std::string();
  Json j = Json::makeObject();
  if (w.on) {
    Json b;
    b.type = Json::Type::Bool;
    b.boolean = true;
    j.object["on"] = b;
  }
  if (w.bpm > 0) j.object["bpm"] = Json::makeNumber(w.bpm);
  if (!w.markers.empty()) {
    Json a = Json::makeArray();
    for (const WarpMarker& k : w.markers) {
      Json e = Json::makeArray();
      e.array.push_back(Json::makeNumber(k.id));
      e.array.push_back(Json::makeNumber(k.at));
      e.array.push_back(Json::makeNumber(k.to));
      a.array.push_back(std::move(e));
    }
    j.object["m"] = std::move(a);
  }
  return jsonCompact(j);
}

bool sampleWarpFromText(const std::string& text, SampleWarp& out) {
  out = SampleWarp{};
  if (text.empty()) return true;
  Json j;
  if (!jsonParse(text, j) || !j.is(Json::Type::Object)) return false;
  if (const Json* o = j.find("on")) out.on = o->is(Json::Type::Bool) ? o->boolean : o->is(Json::Type::Number) && o->number != 0;
  const double bpm = j.num("bpm", 0);
  out.bpm = std::isfinite(bpm) && bpm >= 20 && bpm <= 999 ? bpm : 0;
  if (const Json* a = j.find("m"); a && a->is(Json::Type::Array)) {
    for (const Json& e : a->array) {
      if (!e.is(Json::Type::Array) || e.array.size() < 3) continue;
      if (!e.array[0].is(Json::Type::Number) || !e.array[1].is(Json::Type::Number) || !e.array[2].is(Json::Type::Number)) continue;
      WarpMarker k;
      k.id = jsonInt<uint32_t>(e.array[0].number);
      k.at = e.array[1].number;
      k.to = e.array[2].number;
      out.markers.push_back(k);
      if (out.markers.size() > kMaxWarpMarkers) break;
    }
    normalizeWarp(out.markers);
  }
  return true;
}

SampleWarp sampleWarpOf(const PluginState& st) {
  SampleWarp w;
  sampleWarpFromText(st.warp, w);
  return w;
}

void setSampleWarp(PluginState& st, const SampleWarp& w) { st.warp = sampleWarpToText(w); }

bool sampleWarpSupported(const std::string& type) {
  return type == "gl.sampler" || type == "gl.slicer" || type == "gl.pads" || type == "gl.seltmann";
}

std::vector<WarpMarkerAt> sampleWarpViews(const std::vector<WarpMarker>& m) {
  std::vector<WarpMarkerAt> out;
  out.reserve(m.size());
  const double base = m.empty() ? 0.0 : warpTimeAt(pointsOf(m), 0.0);
  for (size_t i = 0; i < m.size(); ++i) {
    WarpMarkerAt v;
    v.id = m[i].id;
    v.w = m[i].to - base;
    if (i + 1 < m.size()) {
      const double dw = m[i + 1].to - m[i].to;
      v.speed = dw > 0 ? (m[i + 1].at - m[i].at) / dw : 1.0;
    }
    out.push_back(v);
  }
  return out;
}

double sampleWarpTimeOf(const std::vector<WarpMarker>& m, double s) { return warpTimeAt(pointsOf(m), s) - (m.empty() ? 0.0 : warpTimeAt(pointsOf(m), 0.0)); }

double sampleWarpSourceAt(const std::vector<WarpMarker>& m, double w) {
  const std::vector<WarpPoint> p = pointsOf(m);
  return warpSourceAt(p, w + (m.empty() ? 0.0 : warpTimeAt(p, 0.0)));
}

double sampleWarpLength(const std::vector<WarpMarker>& m, double fileSec) { return sampleWarpTimeOf(m, fileSec); }

uint32_t sampleWarpAdd(std::vector<WarpMarker>& m, double w, double fileSec, const std::vector<double>* onsets, double reach) {
  if (!(fileSec > 0) || !std::isfinite(w)) return 0;
  double s = sampleWarpSourceAt(m, w);
  if (onsets) {
    double best = std::numeric_limits<double>::infinity();
    for (double o : *onsets)
      if (std::fabs(o - s) <= reach && std::fabs(o - s) < std::fabs(best - s)) best = o;
    if (std::isfinite(best)) s = best;
  }
  if (s < -1e-9 || s > fileSec + 1e-9) return 0;
  s = std::clamp(s, 0.0, fileSec);
  std::vector<WarpMarker> next = m;
  const uint32_t id = insertAt(next, s);
  if (!id) return 0;
  commit(m, std::move(next), fileSec);
  return indexOf(m, id) >= 0 ? id : 0;
}

bool sampleWarpMove(std::vector<WarpMarker>& m, uint32_t id, double w, double fileSec) {
  std::vector<WarpMarker> x = m;
  int i = indexOf(x, id);
  if (i < 0 || x[static_cast<size_t>(i)].at <= 1e-9) return false;  // (the start's own marker stays)
  pinStart(x);
  i = indexOf(x, id);
  const double base = warpTimeAt(pointsOf(x), 0.0);  // warped time of the file's start
  const double target = w + base;
  const double slack = 1e-7;
  WarpMarker& k = x[static_cast<size_t>(i)];
  double lo = k.to - 1e5, hi = k.to + 1e5;
  if (i > 0) {
    const WarpMarker& p = x[static_cast<size_t>(i - 1)];
    const double ds = k.at - p.at;
    lo = std::max(lo, p.to + std::max(kWarpMinSection, ds / kWarpMaxSpeed) * (1 + slack));
    hi = std::min(hi, p.to + ds / kWarpMinSpeed * (1 - slack));
  }
  if (static_cast<size_t>(i) + 1 < x.size()) {
    const WarpMarker& n = x[static_cast<size_t>(i) + 1];
    const double ds = n.at - k.at;
    lo = std::max(lo, n.to - ds / kWarpMinSpeed * (1 - slack));
    hi = std::min(hi, n.to - std::max(kWarpMinSection, ds / kWarpMaxSpeed) * (1 + slack));
  }
  if (!(lo <= hi)) return false;
  k.to = std::clamp(target, lo, hi);
  return commit(m, std::move(x), fileSec);
}

bool sampleWarpSlide(std::vector<WarpMarker>& m, uint32_t id, double w, double fileSec) {
  std::vector<WarpMarker> x = m;
  const int i = indexOf(x, id);
  if (i < 0) return false;
  // The audio heard at w with this marker out of the way becomes the marker's.
  std::vector<WarpMarker> without = x;
  without.erase(without.begin() + i);
  const double base = warpTimeAt(pointsOf(x), 0.0);
  double s = warpSourceAt(pointsOf(without), w + base);
  const double lo = i > 0 ? x[static_cast<size_t>(i - 1)].at + 0.001 : 0.0;
  const double hi = static_cast<size_t>(i) + 1 < x.size() ? x[static_cast<size_t>(i) + 1].at - 0.001 : fileSec;
  if (!(lo <= hi)) return false;
  s = std::clamp(s, lo, hi);
  x[static_cast<size_t>(i)].at = s;
  return commit(m, std::move(x), fileSec);
}

bool sampleWarpRemove(std::vector<WarpMarker>& m, uint32_t id) {
  const int i = indexOf(m, id);
  if (i < 0) return false;
  m.erase(m.begin() + i);
  return true;
}

int sampleWarpAtOnsets(std::vector<WarpMarker>& m, const std::vector<double>& onsets, double fileSec) {
  std::vector<WarpMarker> x = m;
  int n = 0;
  for (double o : onsets) {
    if (!(o >= 0) || o > fileSec) continue;
    if (insertAt(x, o)) ++n;
  }
  commit(m, std::move(x), fileSec);
  return n;
}

int sampleWarpQuantize(std::vector<WarpMarker>& m, double step, double strength, double fileSec) {
  if (!(step > 0) || m.empty()) return 0;
  strength = std::clamp(strength, 0.0, 1.0);
  int moved = 0;
  // Left to right, each against the markers as already moved (as the playlist's quantize).
  std::vector<uint32_t> ids;
  for (const WarpMarker& k : m) ids.push_back(k.id);
  for (uint32_t id : ids) {
    const int i = indexOf(m, id);
    if (i < 0 || m[static_cast<size_t>(i)].at <= 1e-9) continue;
    const double base = warpTimeAt(pointsOf(m), 0.0);
    const double w = m[static_cast<size_t>(i)].to - base;
    const double line = std::round(w / step) * step;
    const double to = w + (line - w) * strength;
    if (std::fabs(to - w) < 1e-9) continue;
    if (sampleWarpMove(m, id, to, fileSec)) ++moved;
  }
  return moved;
}

double sampleWarpDragTarget(double markerW, double grabW, double pointerW, double step) {
  const double t = markerW + (pointerW - grabW);
  if (!(step > 0)) return t;
  return std::round(t / step) * step;
}

}  // namespace gl

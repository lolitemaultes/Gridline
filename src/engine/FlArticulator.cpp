// Gridline — FL-style articulator engine (see FlArticulator.h).
#include "FlArticulator.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include "../core/FlCurves.h"

namespace gl::flart {

namespace {

constexpr double kTwo32 = 4294967296.0;

int32_t rdI32(const uint8_t* p) {
  uint32_t u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
  return static_cast<int32_t>(u);
}
float rdF32(const uint8_t* p) {
  uint32_t u = static_cast<uint32_t>(rdI32(p));
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
double rdF64(const uint8_t* p) {
  uint64_t u = static_cast<uint32_t>(rdI32(p)) | (static_cast<uint64_t>(static_cast<uint32_t>(rdI32(p + 4))) << 32);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}
void wrU32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void wrI32(std::vector<uint8_t>& o, int32_t v) { wrU32(o, static_cast<uint32_t>(v)); }
void wrF32(std::vector<uint8_t>& o, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  wrU32(o, u);
}
void wrF64(std::vector<uint8_t>& o, double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  wrU32(o, static_cast<uint32_t>(u));
  wrU32(o, static_cast<uint32_t>(u >> 32));
}

bool sameBits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

double scaleFor(const EnvState& s, const Prepared& e, const Scales& sc) {
  if (s.released && e.sus >= 0) return sc.release;
  return s.state == EnvState::Attack ? sc.attack : sc.decay;
}

// Time units per segment k at the current scale; 0 = instant.
double rateFor(const EnvState& s, const Prepared& e, const Scales& sc, int k) {
  const double T = e.time[static_cast<size_t>(k)] * scaleFor(s, e, sc);
  return T > 0 ? 1.0 / T : std::numeric_limits<double>::infinity();
}

// Smooth-shape tangents of the segment ending at point k (see FlCurves.h).
void tangents(EnvState& s, const Prepared& e, double centre) {
  s.ps = s.pe = 0;
  const int k = s.k;
  if (e.shape[static_cast<size_t>(k)] != flcurve::kSmooth) return;
  const double T = e.time[static_cast<size_t>(k)];
  if (!(T > 0) || std::fabs(s.delta) < 1.0 / 16777216.0) return;
  const double slope = s.delta / T;
  const bool inLoop = !s.released && e.loop >= 0 && e.sus >= 0;
  int kp = k - 1;
  if (inLoop && k == e.loop + 1) kp = e.sus;
  if (kp >= 1 && e.shape[static_cast<size_t>(kp)] == flcurve::kSmooth && e.time[static_cast<size_t>(kp)] > 0) {
    const double d = pointValue(e, kp, s.o, centre) - pointValue(e, kp - 1, s.o, centre);
    s.ps = flcurve::smoothTangent(d / e.time[static_cast<size_t>(kp)] / slope, e.time[static_cast<size_t>(kp)] / T);
  }
  int kn = k + 1, from = k;
  if (inLoop && k == e.sus) kn = e.loop + 1;
  if (kn < e.n && kn >= 1 && e.shape[static_cast<size_t>(kn)] == flcurve::kSmooth && e.time[static_cast<size_t>(kn)] > 0) {
    const double d = pointValue(e, kn, s.o, centre) - pointValue(e, from, s.o, centre);
    s.pe = flcurve::smoothTangent(d / e.time[static_cast<size_t>(kn)] / slope, e.time[static_cast<size_t>(kn)] / T);
  }
}

double shapeAt(const EnvState& s, const Prepared& e, double x) {
  const int sh = e.shape[static_cast<size_t>(s.k)];
  const double t = e.tension[static_cast<size_t>(s.k)];
  if (sh == flcurve::kSmooth) return flcurve::smooth(t, std::clamp(x, 0.0, 1.0), s.ps, s.pe);
  return flcurve::shape(sh, t, x);
}

// The segment ending at point s.k has completed.
void advance(EnvState& s, const Prepared& e, const Scales& sc, double centre) {
  s.v0 = pointValue(e, s.k, s.o, centre);
  if (!s.released && s.k == e.sus) {
    if (e.loop >= 0 && !e.loopZero && sc.decay > 0) {
      s.k = e.loop + 1;
    } else {
      s.holding = true;
      s.delta = 0;
      s.out = s.v0;
      s.rate = 0;
      s.phi = 0;
      return;
    }
  } else {
    if (s.state == EnvState::Attack && e.decay >= 0 && s.k >= e.decay) {
      s.state = EnvState::Decay;
      s.o = sc.sustain;
    }
    s.k += 1;
    if (s.k >= e.n) {
      s.ended = true;
      s.k = e.n - 1;
      s.delta = 0;
      s.out = s.v0;
      return;
    }
  }
  s.delta = pointValue(e, s.k, s.o, centre) - s.v0;
  s.rate = rateFor(s, e, sc, s.k);
  tangents(s, e, centre);
}

// Runs completed segments (phi >= 1), carrying the overshoot (in time
// units) into the next segment. A segment without a rate (held at the
// sustain point, then released) carries a whole step (`step`); an instant
// segment passes the carry on unchanged (the plugin's tick loop).
void settle(EnvState& s, const Prepared& e, const Scales& sc, double centre, double step) {
  for (int guard = 0; s.phi >= 1.0 && !s.ended && !s.holding && guard < 4 * e.n + 16; ++guard) {
    const double r = s.rate;
    double left;
    if (r == 0) left = step;
    else if (!std::isfinite(r)) left = s.carry;
    else left = (s.phi - 1.0) / r;
    advance(s, e, sc, centre);
    if (s.ended || s.holding) break;
    if (std::isfinite(s.rate)) {
      s.phi = left * s.rate;
    } else {
      s.carry = left;
      s.phi = 1.0;
    }
  }
  if (s.phi >= 1.0 && !s.ended && !s.holding) s.phi = 1.0;  // guard hit: stay at the segment end
}

}  // namespace

bool Point::operator==(const Point& o) const {
  return sameBits(time, o.time) && sameBits(value, o.value) && sameBits(tension, o.tension) && shape == o.shape && wait == o.wait &&
         extra == o.extra && sign == o.sign;
}

bool Record::operator==(const Record& o) const {
  return kind == o.kind && hasKind == o.hasKind && version == o.version && pts == o.pts && flags == o.flags && enabled == o.enabled &&
         decay == o.decay && loop == o.loop && sustain == o.sustain && std::equal(extra, extra + 4, o.extra) && phase == o.phase;
}

Record defaultEnvelope() {
  Record r;
  r.kind = KindEnv;
  r.pts = {{0, 0, 0, 0, 0, 0, 0}, {0.75, 1, 0.2f, 0, 0, 0, 1}, {1.5, 0.5, 0.2f, 0, 0, 0, 1}, {0.25, 0, 0.2f, 0, 0, 0, 1}};
  r.decay = 1, r.loop = -1, r.sustain = 2;
  return r;
}

Record defaultLfo() {
  Record r;
  r.kind = KindLfo;
  r.pts = {{0, 0.5, 0, 0, 0, 0, 0}, {1, 0.5, 0, 0, 0, 0, 2}, {0.25, 1, 0, 0, 0, 0, 2}, {1, 0.5, 0, 0, 0, 0, 2}};
  r.decay = -1, r.loop = -1, r.sustain = 2;
  r.extra[0] = 40898, r.extra[1] = r.extra[2] = r.extra[3] = 0;
  return r;
}

Record defaultMap(double level) {
  Record r;
  r.kind = KindMap;
  r.pts = {{0, level, 0, 0, 0, 0, 0}, {1, level, 0, 0, 0, 0, 2}};
  r.decay = r.loop = r.sustain = -1;
  r.extra[0] = r.extra[1] = r.extra[2] = r.extra[3] = 0;
  return r;
}

size_t extraSize(int kind, int version) {
  if (kind == KindEnv) return 16;
  if (kind == KindLfo) return version >= 3 ? 20 : 16;
  return 0;
}

bool readRecord(const uint8_t* p, size_t n, size_t& at, int kind, Record& out) {
  auto has = [&](size_t k) { return at <= n && n - at >= k; };
  out = Record{};
  out.kind = kind;
  if (!has(8)) return false;
  const int32_t first = rdI32(p + at);
  at += 4;
  if (first == kind) {
    out.hasKind = true;
    out.version = rdI32(p + at);
    at += 4;
  } else {
    out.hasKind = false;
    out.version = first;
  }
  if (out.version < 0 || out.version >= 4 || !has(4)) return false;
  const int32_t np = rdI32(p + at);
  at += 4;
  const bool f64time = out.version >= 2 || kind == KindDeck;
  const size_t stride = (f64time ? 8 : 4) + 8 + 4 + (out.version >= 1 ? 4 : 0);
  if (np < 0 || np > 100000 || !has(static_cast<size_t>(np) * stride + 20)) return false;
  out.pts.resize(static_cast<size_t>(np));
  for (auto& pt : out.pts) {
    pt.time = f64time ? rdF64(p + at) : static_cast<double>(rdF32(p + at));
    at += f64time ? 8 : 4;
    pt.value = rdF64(p + at);
    pt.tension = rdF32(p + at + 8);
    at += 12;
    if (out.version >= 1) {
      pt.shape = p[at], pt.wait = p[at + 1], pt.extra = p[at + 2], pt.sign = static_cast<int8_t>(p[at + 3]);
      at += 4;
    }
  }
  out.flags = rdI32(p + at), out.enabled = rdI32(p + at + 4), out.decay = rdI32(p + at + 8), out.loop = rdI32(p + at + 12),
  out.sustain = rdI32(p + at + 16);
  at += 20;
  const size_t ex = extraSize(kind, out.version);
  if (!has(ex)) return false;
  if (ex >= 16)
    for (int i = 0; i < 4; ++i) out.extra[i] = rdI32(p + at + 4 * static_cast<size_t>(i));
  else
    std::fill(out.extra, out.extra + 4, 0);
  if (ex == 20) out.phase = static_cast<uint32_t>(rdI32(p + at + 16));
  at += ex;
  return true;
}

void writeRecord(std::vector<uint8_t>& o, const Record& r) {
  if (r.hasKind) wrI32(o, r.kind);
  wrI32(o, r.version);
  wrI32(o, static_cast<int32_t>(r.pts.size()));
  const bool f64time = r.version >= 2 || r.kind == KindDeck;
  for (const auto& pt : r.pts) {
    if (f64time) wrF64(o, pt.time);
    else wrF32(o, static_cast<float>(pt.time));
    wrF64(o, pt.value);
    wrF32(o, pt.tension);
    if (r.version >= 1) {
      o.push_back(pt.shape), o.push_back(pt.wait), o.push_back(pt.extra);
      o.push_back(static_cast<uint8_t>(pt.sign));
    }
  }
  wrI32(o, r.flags), wrI32(o, r.enabled), wrI32(o, r.decay), wrI32(o, r.loop), wrI32(o, r.sustain);
  const size_t ex = extraSize(r.kind, r.version);
  if (ex >= 16)
    for (int i = 0; i < 4; ++i) wrI32(o, r.extra[i]);
  if (ex == 20) wrU32(o, r.phase);
}

std::string writeText(const Record& r) {
  std::string s;
  char buf[96];
  std::snprintf(buf, sizeof buf, "%d %d %d %d %d %d %d %d %d %d %d %d %u %zu", r.kind, r.hasKind ? 1 : 0, r.version, r.flags, r.enabled,
                r.decay, r.loop, r.sustain, r.extra[0], r.extra[1], r.extra[2], r.extra[3], r.phase, r.pts.size());
  s += buf;
  for (const auto& pt : r.pts) {
    std::snprintf(buf, sizeof buf, " %.17g %.17g %.9g %d %d %d %d", pt.time, pt.value, static_cast<double>(pt.tension), pt.shape, pt.wait,
                  pt.extra, pt.sign);
    s += buf;
  }
  return s;
}

bool readText(const std::string& line, Record& out) {
  out = Record{};
  const char* p = line.c_str();
  char* end = nullptr;
  auto nextL = [&](long& v) {
    v = std::strtol(p, &end, 10);
    if (end == p) return false;
    p = end;
    return true;
  };
  auto nextD = [&](double& v) {
    v = std::strtod(p, &end);
    if (end == p) return false;
    p = end;
    return true;
  };
  long v[14];
  for (long& x : v)
    if (!nextL(x)) return false;
  out.kind = static_cast<int>(v[0]);
  out.hasKind = v[1] != 0;
  out.version = static_cast<int>(v[2]);
  out.flags = static_cast<int32_t>(v[3]), out.enabled = static_cast<int32_t>(v[4]), out.decay = static_cast<int32_t>(v[5]);
  out.loop = static_cast<int32_t>(v[6]), out.sustain = static_cast<int32_t>(v[7]);
  for (int i = 0; i < 4; ++i) out.extra[i] = static_cast<int32_t>(v[8 + i]);
  out.phase = static_cast<uint32_t>(v[12]);
  if (v[13] < 0 || v[13] > 100000) return false;
  out.pts.resize(static_cast<size_t>(v[13]));
  for (auto& pt : out.pts) {
    double t, val, ten;
    long a, b, c, d;
    if (!nextD(t) || !nextD(val) || !nextD(ten) || !nextL(a) || !nextL(b) || !nextL(c) || !nextL(d)) return false;
    pt.time = t, pt.value = val, pt.tension = static_cast<float>(ten);
    pt.shape = static_cast<uint8_t>(a), pt.wait = static_cast<uint8_t>(b), pt.extra = static_cast<uint8_t>(c);
    pt.sign = static_cast<int8_t>(d);
  }
  return out.version >= 0 && out.version < 4;
}

Prepared prepare(const Record& r, double neutral) {
  Prepared e;
  e.n = static_cast<int>(r.pts.size());
  e.enabled = r.on();
  e.flags = r.flags;
  e.tempo = (r.flags & kTempoTime) != 0;
  const size_t n = r.pts.size();
  e.time.resize(n), e.value.resize(n), e.weight.assign(n, 0.0), e.tension.resize(n), e.shape.resize(n), e.pos.resize(n);
  double prev = 0, acc = 0;
  for (size_t k = 0; k < n; ++k) {
    const Point& pt = r.pts[k];
    double t = std::isfinite(pt.time) ? std::max(0.0, pt.time) : 0.0;
    double v = std::isnan(pt.value) ? prev : pt.value;
    if (!std::isfinite(v)) v = prev;
    e.time[k] = t, e.value[k] = v, e.tension[k] = std::isfinite(pt.tension) ? std::clamp(pt.tension, -1.0f, 1.0f) : 0.0f;
    e.shape[k] = pt.shape;
    acc += t;
    e.pos[k] = acc;
    prev = v;
  }
  auto valid = [&](int32_t i) { return i >= 0 && i < e.n ? i : -1; };
  e.decay = r.kind == KindLfo ? -1 : valid(r.decay);
  e.loop = valid(r.loop);
  int sus = valid(r.sustain);
  if (sus < 0 && e.loop >= 0) sus = e.n - 1;
  e.sus = sus;
  if (e.loop >= 0 && (e.sus < 0 || e.loop >= e.sus)) e.loop = -1;
  if (e.loop >= 0) {
    for (int k = e.loop + 1; k <= e.sus; ++k) e.loopTime += e.time[static_cast<size_t>(k)];
    e.loopZero = !(e.loopTime > 0);
  }
  // Sustain-offset weights (spec §2.3; the plugin's precompute): rising
  // with time from the decay marker (0) to the loop start / sustain point (1),
  // 1 inside the loop, falling to 0 at the last point after the sustain point.
  // Without a sustain point the rise runs to the last point. (Points before
  // the decay marker never carry an offset: it is 0 until the marker.)
  if (e.n > 0) {
    const int d = std::max(0, e.decay);
    const int S = e.sus >= 0 ? e.sus : e.n - 1;
    const int A = e.loop >= 0 ? e.loop : S;
    const auto c = [&](int k) { return e.pos[static_cast<size_t>(k)]; };
    const double eps = 1.0 / 65536.0;
    for (int k = 0; k < e.n; ++k) {
      double w;
      if (k <= d) w = 0.0;
      else if (k <= A) w = (c(k) - c(d)) / std::max(eps, c(A) - c(d));
      else if (k <= S) w = 1.0;
      else w = (c(e.n - 1) - c(k)) / std::max(eps, c(e.n - 1) - c(S));
      e.weight[static_cast<size_t>(k)] = w;
    }
  }
  for (size_t k = 0; k < n; ++k) {
    if (e.value[k] != neutral) e.flat = false;
    if (e.value[k] != e.value[0]) e.constant = false;
  }
  return e;
}

double unitsPerTick(bool tempoBased, double tempo, int ppq) {
  const double q = 1.0 / std::max(1, ppq);
  return tempoBased ? q : q * 120.0 / std::max(1.0, tempo);
}

double pointValue(const Prepared& e, int k, double o, double centre) {
  if (k < 0 || k >= e.n) return 0.0 - centre;
  const size_t i = static_cast<size_t>(k);
  return std::clamp(e.value[i] + o * e.weight[i], 0.0, 1.0) - centre;
}

void envStart(EnvState& s, const Prepared& e, const Scales& sc, double centre) {
  s = EnvState{};
  if (e.n <= 0) {
    s.started = true;
    s.ended = true;
    s.out = 0.0 - centre;
    return;
  }
  s.started = true;
  s.ended = false;
  // The level offset starts at 0; it applies once the decay marker is passed.
  s.state = e.decay >= 0 ? EnvState::Attack : EnvState::Decay;
  s.o = 0.0;
  s.k = 0;
  s.v0 = pointValue(e, 0, s.o, centre);
  s.delta = 0;
  s.out = s.v0;
  s.phi = 0;
  s.carry = 0;
  // The pre-delay (point 0's time) runs unscaled.
  s.rate = e.time[0] > 0 ? 1.0 / e.time[0] : std::numeric_limits<double>::infinity();
  if (!std::isfinite(s.rate)) {  // no pre-delay: straight into segment 1
    s.phi = 1.0;
    settle(s, e, sc, centre, 0.0);
    if (!s.ended && !s.holding) s.out = s.v0 + s.delta * shapeAt(s, e, s.phi);
  }
}

void envTick(EnvState& s, const Prepared& e, const Scales& sc, double centre, double dt) {
  if (!s.started || s.ended || s.holding) return;
  if (std::isfinite(s.rate)) s.phi += dt * s.rate;
  else s.phi = 1.0, s.carry = dt;
  settle(s, e, sc, centre, dt);
  if (s.ended || s.holding) return;
  s.out = s.v0 + s.delta * shapeAt(s, e, s.phi);
}

void envRelease(EnvState& s, const Prepared& e, const Scales& sc, double centre) {
  (void)sc;
  if (!s.started || s.ended || s.released || e.sus < 0) return;
  const double cur = s.out;
  s.released = true;
  s.holding = false;
  s.k = e.sus;
  // The offset moves the sustain point's (unclamped) value onto the current output.
  const size_t i = static_cast<size_t>(e.sus);
  s.o += cur - (e.value[i] + s.o * e.weight[i] - centre);
  s.v0 = cur;
  s.delta = 0;
  s.out = cur;
  s.phi = 1.0;
}

void LfoShape::set(int tension, int skew, int pw, bool bipolar) {
  const double W = pw / 128.0, S = skew / 256.0;
  T = W <= 0 ? kTwo32 - 1.0 : (1.0 - W) * kTwo32 - 1.0;
  P = kTwo32 * ((1.0 - W) * 0.5 - (1.0 - std::fabs(W)) * S);
  P = std::clamp(P, 1.0, std::max(1.0, T - 1.0));
  Q = W >= 0 ? P : std::max(1.0, P + W * kTwo32);
  bipolarTension = bipolar;
  if (tension == 0) {
    tensionC = 0;
  } else {
    const double x = std::fabs(tension) / 128.0;
    const double c = std::pow(10.0, 18.0 * (std::pow(21.0, x) - 1.0) / 20.0) - 1.0;
    tensionC = tension < 0 ? -c : c;
  }
}

double LfoShape::value(uint32_t phase) const {
  const double ph = static_cast<double>(phase);
  double v;
  if (ph <= Q) v = 1.0 - 2.0 * ph / Q;
  else if (ph < P) v = -1.0;
  else if (ph <= T) v = T > P ? -1.0 + 2.0 * (ph - P) / (T - P) : 1.0;
  else v = 1.0;
  if (tensionC == 0) return v;
  if (!bipolarTension) return (v < 0 ? -1.0 : 1.0) * flcurve::curveKernel(tensionC, std::fabs(v));
  return 2.0 * flcurve::curveKernel(tensionC, (v + 1.0) * 0.5) - 1.0;
}

uint32_t LfoShape::startPhase(uint32_t offset) const {
  const double mid = P + 1.0 + (T - P) * 0.5;
  const uint64_t m = static_cast<uint64_t>(std::max(0.0, mid)) & 0xFFFFFFFFull;
  return static_cast<uint32_t>(m) + offset;
}

double lfoRate(int speed) { return (std::pow(10001.0, speed / 65536.0) - 1.0) / 625.0; }

uint32_t lfoIncrement(int speed, double upt) {
  double v = lfoRate(speed) * kTwo32 * upt;
  v = std::clamp(v, 0.0, 0.1875 * kTwo32);
  return static_cast<uint32_t>(std::nearbyint(v));
}

double mapValue(const Prepared& m, double x, double centre) {
  if (m.n <= 0) return 0.0;
  if (m.constant) return m.value[0] - centre;
  x = std::clamp(x, 0.0, 1.0);
  const size_t n = static_cast<size_t>(m.n);
  if (x <= m.pos[0]) return m.value[0] - centre;
  if (x >= m.pos[n - 1]) return m.value[n - 1] - centre;
  size_t k = static_cast<size_t>(std::upper_bound(m.pos.begin(), m.pos.end(), x) - m.pos.begin());
  if (k >= n) k = n - 1;
  while (k > 1 && m.pos[k - 1] >= x) --k;  // x lies in (pos[k-1], pos[k]]
  const double T = m.time[k];
  const double phi = T > 0 ? (x - m.pos[k - 1]) / T : 1.0;
  const double v0 = m.value[k - 1], d = m.value[k] - v0;
  double y;
  if (m.shape[k] == flcurve::kSmooth) {
    double ps = 0, pe = 0;
    if (T > 0 && std::fabs(d) >= 1.0 / 16777216.0) {
      const double slope = d / T;
      if (k >= 2 && m.shape[k - 1] == flcurve::kSmooth && m.time[k - 1] > 0)
        ps = flcurve::smoothTangent((m.value[k - 1] - m.value[k - 2]) / m.time[k - 1] / slope, m.time[k - 1] / T);
      if (k + 1 < n && m.shape[k + 1] == flcurve::kSmooth && m.time[k + 1] > 0)
        pe = flcurve::smoothTangent((m.value[k + 1] - m.value[k]) / m.time[k + 1] / slope, m.time[k + 1] / T);
    }
    y = flcurve::smooth(m.tension[k], std::clamp(phi, 0.0, 1.0), ps, pe);
  } else {
    y = flcurve::shape(m.shape[k], m.tension[k], phi);
  }
  return v0 + d * y - centre;
}

}  // namespace gl::flart

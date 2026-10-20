// Gridline — "synths" pack: Matrix FM reference model, articulator curves (mfmref_env.h).
#include <algorithm>
#include <cmath>
#include <cstring>
#include "mfmref_env.h"

namespace gl::packs::synths::mfmref::env {

namespace {

constexpr double kLn1e6 = 13.815511557963774;  // 6 ln 10
constexpr float kTwoPi = 6.28318548f;

inline uint32_t bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
inline bool negBit(float f) { return (bits(f) & 0x80000000u) != 0; }

int stepCount(const Pt& p) {
  if (!(p.shape >= 3 && p.shape <= 6)) return -1;
  const double d = std::exp(static_cast<double>(static_cast<float>(1.0 - static_cast<double>(static_cast<float>(std::fabs(p.tension))) * 0.59999999999999998)) * kLn1e6);
  int n = static_cast<int>(std::nearbyint(static_cast<double>(static_cast<float>(d - 1.0)) * 0.001));
  if (p.shape == 3 && p.tension >= 0.0f) n += 1;
  return n;
}

void curveConsts(float c, Pt& p) {  // curvature triple: c, 1/|c|, ln(|c| + 1)
  p.p10 = c;
  if (c != 0.0f) {
    p.p14 = 1.0f / std::fabs(c);
    p.p18 = static_cast<float>(std::log(static_cast<double>(std::fabs(c) + 1.0f)));
  }
}

inline double kernel(const Pt& p, double x) {
  if (bits(p.p10) == 0) return x;
  if (!negBit(p.p10)) return 1.0 - static_cast<double>(p.p14) * (std::exp(static_cast<double>(p.p18) * (1.0 - x)) - 1.0);
  return static_cast<double>(p.p14) * (std::exp(static_cast<double>(p.p18) * x) - 1.0);
}

inline long rnd(double x) { return static_cast<long>(std::nearbyint(x)); }

// Segment shape: progress x 0..1 → 0..1.
float shapeY(const Pt& p, double x) {
  if (p.sign == 2) return static_cast<float>(x);
  const double a = p.p10, b = p.p14, c = p.p18;
  switch (p.shape) {
    case 0:
      return static_cast<float>(kernel(p, x));
    case 1:
      if (x < 0.5) return static_cast<float>(kernel(p, x * 2.0) * 0.5);
      return static_cast<float>(1.0 - kernel(p, (1.0 - x) * 2.0) * 0.5);
    case 3:
      return static_cast<float>(static_cast<int>(rnd(a * x - c))) * p.p14;
    case 4: {
      const double d = a * x - 0.5;
      const double i = static_cast<double>(static_cast<int>(rnd(d - 0.5)));
      double f = (d - i) * 2.0 - 1.0;
      if (p.sign < 0) {
        const double g = 1.0 - std::fabs(f);
        f = (f < 0 ? -1.0 : 1.0) * (1.0 - g * g * g);
      } else {
        f = f * f * f;
      }
      return static_cast<float>(b * (f * 0.5 + i + 1.0));
    }
    case 5:
      return static_cast<float>(static_cast<uint32_t>(rnd(a * x - c)) & 1u);
    case 6:
      if (p.sign < 0) return static_cast<float>((std::sin(b * (a * x + 0.75)) + 1.0) * 0.5);
      {
        const double d = a * x - 0.5;
        return static_cast<float>(std::fabs((d - static_cast<double>(static_cast<int>(rnd(d - 0.5)))) * 2.0 - 1.0));
      }
    case 7: {
      const double u = p.sign > 0 ? 1.0 - x : x;
      const double u3 = u * u * u;
      return static_cast<float>(x - a * (u3 * u3 - u));
    }
    case 8:
      if (x < 0.5) {
        const double xx = x * 2.0;
        const double u = p.sign > 0 ? 1.0 - xx : xx;
        const double u3 = u * u * u;
        return static_cast<float>((xx - a * (u3 * u3 - u)) * 0.5);
      } else {
        const double d = (1.0 - x) * 2.0;
        const double u = p.sign > 0 ? 1.0 - d : d;
        const double u3 = u * u * u;
        return static_cast<float>(1.0 - (d - a * (u3 * u3 - u)) * 0.5);
      }
    case 9: {
      double q;
      if (p.sign < 1) q = 1.0 - std::cos(static_cast<double>(1.57079637f) * x);
      else q = std::sin(static_cast<double>(1.57079637f) * x);
      return static_cast<float>(x - a * (x - q));
    }
    case 10: {
      const double x2 = x * x, xm = x - 1.0;
      return static_cast<float>(c * ((b * x * xm * xm + a * x2 * xm + (3.0 * x2 - 2.0 * x2 * x)) - x) + x);
    }
    case 11:
      if (p.sign < 1) return static_cast<float>(b / (1.0 - a * x) - c);
      return static_cast<float>(b / (1.0 - (1.0 - x) * a) - c);
    case 12:
      if (x < 0.5) {
        if (p.sign < 1) return static_cast<float>(b / (1.0 - a * x * 2.0) - c);
        return static_cast<float>(b / (1.0 - (1.0 - x * 2.0) * a) - c);
      } else {
        const double d = (1.0 - x) * 2.0;
        if (p.sign < 1) return static_cast<float>(c + (1.0 - b / (1.0 - a * d)));
        return static_cast<float>(c + (1.0 - b / (1.0 - (1.0 - d) * a)));
      }
    default:
      return 0.0f;
  }
}

void genericStart(const Env& e, State& s) {
  s.phase = 9;
  if (!e.enabled) {
    s.st = 9, s.phase = 9, s.v0 = s.delta = s.out = 0;
    return;
  }
  s.k = 0;
  s.ofs = 0;
  s.st = e.decay < 0 ? 3 : 1;
  if (e.pts.empty() || e.pts[0].time == 0.0f) {
    s.rate = 1.0, s.phi = 1.0;
  } else {
    s.rate = e.speed / static_cast<double>(e.pts[0].time);
    s.phi = 0.0;
  }
  s.phase = s.st;
  s.v0 = static_cast<float>(pointValue(e, s.ofs, 0));
  s.delta = 0;
  s.out = s.v0;
  s.lfoOut = 0;
  s.loopHold = e.loopZero;
  s.waitStep = s.myIdx = s.count = -1;
  s.frozenV = 0;
  s.frozen = false;
}

// Walks a mapping curve's segments (no sustain, loop or scales).
void mapAdvance(const Env& m, State& s, double dt) {
  const int n = static_cast<int>(m.pts.size());
  s.phi = dt * s.rate + s.phi;
  while (s.phi >= 1.0) {
    const int k0 = s.k;
    if (s.rate == 0.0) s.phi = dt;
    else s.phi = (s.phi - 1.0) / s.rate;
    s.v0 = static_cast<float>(pointValue(m, 0, s.k));
    if (s.k < n - 1) s.k += 1;
    else s.st = 9;
    s.delta = static_cast<float>(pointValue(m, 0, s.k) - static_cast<double>(s.v0));
    if (k0 == s.k) {
      s.phi = 0, s.rate = 0;
    } else {
      const double T = static_cast<double>(m.pts[static_cast<size_t>(s.k)].time);
      if (T <= 0.0) {
        s.rate = 1.0;
        s.phi = s.phi + 1.0;
      } else {
        s.rate = 1.0 / T;
        s.phi = s.phi * s.rate;
      }
    }
  }
}

void mapBegin(const Env& m, State& s) {
  s = State{};
  s.k = 0, s.ofs = 0, s.st = 1;
  if (m.pts[0].time == 0.0f) s.rate = 1.0, s.phi = 1.0;
  else s.rate = 1.0 / static_cast<double>(m.pts[0].time), s.phi = 0.0;
  s.v0 = static_cast<float>(pointValue(m, 0, 0)), s.delta = 0, s.out = s.v0;
}

}  // namespace

Bend makeBend(float c) {
  Bend b;
  b.c = c;
  if (c != 0.0f) {
    b.k = 1.0f / std::fabs(c);
    b.l = static_cast<float>(std::log(static_cast<double>(std::fabs(c) + 1.0f)));
  }
  return b;
}

double bendAt(const Bend& b, double x) {
  if (bits(b.c) == 0) return x;
  if (!negBit(b.c)) return 1.0 - static_cast<double>(b.k) * (std::exp(static_cast<double>(b.l) * (1.0 - x)) - 1.0);
  return static_cast<double>(b.k) * (std::exp(static_cast<double>(b.l) * x) - 1.0);
}

void setLfoSpeed(Env& e, double speed) {
  e.speed = speed;
  const double sp = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(static_cast<double>(e.speedRaw) * 1.52587890625e-05)) * 9.2104403669765169) - 1.0));
  double inc = sp * 6871947.6736000003 * speed;
  if (inc > 805306368.0) inc = 805306368.0;
  e.inc = static_cast<double>(rnd(inc));
  e.incRaw = static_cast<double>(static_cast<uint32_t>(rnd(inc)));
  if (e.frozen) e.inc = 0;
}

Env prepare(const Record& r, int kind, double centre, double speed, bool clamp) {
  Env e;
  e.enabled = r.enabled != 0;
  e.flags = r.flags;
  e.centre = centre;
  e.speed = speed;
  e.clamp = clamp;
  float prev = 0;
  e.pts.reserve(r.pts.size());
  for (const auto& rp : r.pts) {
    Pt p;
    p.time = static_cast<float>(rp.time);
    double v = rp.value;
    if (std::isnan(v)) v = prev;
    p.value = static_cast<float>(v);
    prev = p.value;
    p.tension = rp.tension;
    p.shape = rp.shape;
    p.wait = rp.wait;
    e.pts.push_back(p);
  }
  const int n = static_cast<int>(e.pts.size());
  e.decay = kind == 2 || kind == 3 ? -1 : (r.decay >= 0 && r.decay < n ? r.decay : -1);
  e.loop = r.loop >= 0 && r.loop < n ? r.loop : -1;
  e.sus = r.sustain >= 0 && r.sustain < n ? r.sustain : -1;
  if (kind == 3) e.loop = e.sus = -1;
  e.end = e.sus;
  if (e.end < 0 && e.loop >= 0) e.end = n - 1;
  // per point constants + sustain-offset weights
  const int dmk = e.decay < 0 ? 0 : e.decay;
  const int c8 = e.sus < 0 ? n - 1 : e.sus;
  const int c4 = e.loop < 0 ? c8 : e.loop;
  double e0 = 0, d8 = 0, d0 = 0;
  for (int k = 0; k < n; ++k) {
    Pt& p = e.pts[static_cast<size_t>(k)];
    if (p.time < 0.0f) p.time = 0.0f;
    if (k <= dmk) e0 += static_cast<double>(p.time);
    else if (k <= c4) d8 += static_cast<double>(p.time);
    if (k > c8) d0 += static_cast<double>(p.time);
    if (k == 0) continue;
    p.sign = p.tension == 0.0f ? 0 : (negBit(p.tension) ? -1 : 1);
    if (p.shape < 2) {
      const float t = p.tension;
      const double ex = std::exp(static_cast<double>(std::fabs(t)) * kLn1e6);
      const float sg = t == 0.0f ? 0.0f : (negBit(t) ? -1.0f : 1.0f);
      curveConsts(sg * static_cast<float>(ex - 1.0), p);
    } else if (p.shape == 3) {
      const int sg = p.tension < 0.0f ? -1 : 1;
      p.p18 = static_cast<float>(sg) * 0.5f;
      p.p10 = static_cast<float>(static_cast<long>(stepCount(p)) + 1);
      p.p14 = 1.0f / (p.p10 - static_cast<float>(sg));
    } else if (p.shape == 4) {
      p.p10 = static_cast<float>(static_cast<long>(stepCount(p)) + 1);
      p.p14 = 1.0f / p.p10;
    } else if (p.shape == 5) {
      const int sg = p.tension < 0.0f ? -1 : 1;
      p.p18 = static_cast<float>(sg) * 0.5f;
      p.p10 = static_cast<float>((static_cast<long>(stepCount(p)) + 1) * 2);
      p.p14 = kTwoPi;
    } else if (p.shape == 6) {
      const int st = stepCount(p);
      p.p10 = static_cast<float>(st) + 0.5f;
      p.p14 = kTwoPi;
      if (st == 0 && p.sign > 0) p.sign = 2;
    } else if (p.shape == 7 || p.shape == 8) {
      p.p10 = p.tension;
    } else if (p.shape == 9) {
      p.p10 = std::fabs(p.tension);
    } else if (p.shape == 11 || p.shape == 12) {
      double a = static_cast<double>(std::fabs(p.tension)) * 0.90000000000000002;
      if (a <= 1.52587890625e-05) a = 1.52587890625e-05;
      a = 1.0 - a;
      p.p10 = static_cast<float>(1.0 - a * a * a);
      p.p14 = (1.0f - p.p10) / p.p10;
      p.p18 = p.p14;
      if (p.sign > 0) {
        p.p14 = -p.p14;
        p.p18 = p.p14 - 1.0f;
      }
      if (p.shape == 12) p.p14 *= 0.5f, p.p18 *= 0.5f;
    } else if (p.shape == 10) {
      // smooth: straight segment fallback (tangents are not modelled)
      p.p10 = 0, p.p14 = 0, p.p18 = 1.0f - std::fabs(p.tension);
    }
    const int sh = p.shape;
    if ((sh < 2 || (sh >= 7 && sh <= 9) || sh == 11 || sh == 12) && p.tension == 0.0f) p.sign = 2;
  }
  double acc = e0;
  for (int k = 0; k < n; ++k) {
    Pt& p = e.pts[static_cast<size_t>(k)];
    p.weight = 0.0f;
    if (dmk < k) {
      p.weight = 1.0f;
      if (c8 < k) {
        acc -= static_cast<double>(p.time);
        if ((e.flags & 0x80) == 0) p.weight = static_cast<float>(acc / std::max(1.52587890625e-05, d0));
      } else {
        acc += static_cast<double>(p.time);
        if (k <= c4) p.weight = static_cast<float>(acc / std::max(1.52587890625e-05, d8));
        if (k == c8) acc = d0;
      }
    } else if (dmk == k) {
      acc = 0.0;
    } else {
      acc -= static_cast<double>(p.time);
      p.weight = static_cast<float>(acc / std::max(1.52587890625e-05, e0));
    }
  }
  e.loopZero = false;
  if (e.end >= 0 && e.loop >= 0) {
    double a = 0, b = 0;
    for (int k = 0; k <= e.end; ++k) a += static_cast<double>(e.pts[static_cast<size_t>(k)].time);
    for (int k = 0; k <= e.loop; ++k) b += static_cast<double>(e.pts[static_cast<size_t>(k)].time);
    e.loopZero = a - b <= 0.0;
  }
  e.hasWait = false;
  for (int k = 0; k < n; ++k)
    if (e.pts[static_cast<size_t>(k)].wait && k <= e.sus) e.hasWait = true;
  if (e.loopZero) e.hasWait = false;
  if (kind == 1)
    for (int i = 0; i < 4; ++i) e.scale[i] = static_cast<float>(r.extra[i]) * 0.0078125f;
  if (kind == 2) {
    // oscillator: tension curvature, skew, pulse width
    const int ten = r.extra[1];
    const double x = static_cast<double>(static_cast<float>(std::abs(ten)) * 0.0078125f);
    const double d = std::exp(x * 3.044522437723423);
    const double c = std::exp(static_cast<double>(static_cast<float>(static_cast<double>(static_cast<float>(d - 1.0)) * 0.050000000000000003)) * 41.446531658199575);
    const float sg = ten == 0 ? 0.0f : (ten < 0 ? -1.0f : 1.0f);
    const float cc = static_cast<float>(c - 1.0) * sg;
    e.oscC = cc;
    if (cc != 0.0f) {
      e.oscCinv = 1.0f / std::fabs(cc);
      e.oscK = static_cast<float>(std::log(static_cast<double>(std::fabs(cc) + 1.0f)));
    }
    e.oscBipolar = (r.flags & 4) != 0;
    const float skew = static_cast<float>(r.extra[2]) * 0.00390625f, pw = static_cast<float>(r.extra[3]) * 0.0078125f;
    // cycle points on the 32-bit phase: T end of the wave, P peak, Q pulse edge
    uint32_t Tu = 0xFFFFFFFFu;
    if (0.0f < pw) Tu = static_cast<uint32_t>(static_cast<int64_t>(std::nearbyint((1.0f - pw) * 4294967296.0f - 1.0f)));
    double dd = static_cast<double>(((1.0f - pw) * 0.5f - (1.0f - std::fabs(pw)) * skew) * 4294967296.0f);
    if (static_cast<double>(static_cast<int64_t>(Tu) - 1) < dd) dd = static_cast<double>(static_cast<int64_t>(Tu) - 1);
    else if (dd < 1.0) dd = 1.0;
    const uint32_t Pu = static_cast<uint32_t>(static_cast<int64_t>(std::nearbyint(dd)));
    uint32_t Qu = Pu;
    if (pw < 0.0f) {
      Qu = static_cast<uint32_t>(static_cast<int64_t>(std::nearbyint(static_cast<double>(pw * 4294967296.0f) + dd)));
      if (Qu < 1u) Qu = 1u;
    }
    e.oscT = Tu, e.oscP = Pu, e.oscQ = Qu;
    e.oscQinv = static_cast<float>(2.0 / static_cast<double>(e.oscQ));
    e.oscRinv = static_cast<float>(2.0 / static_cast<double>(static_cast<int64_t>(static_cast<uint64_t>(e.oscT) - static_cast<uint64_t>(e.oscP))));
    e.phaseOfs = r.phase;
    e.speedRaw = r.extra[0];
    e.frozen = (r.flags & 8) != 0;
    e.global = (r.flags & 2) != 0;
    setLfoSpeed(e, speed);
  }
  return e;
}

double pointValue(const Env& e, float ofs, int k, bool clamp) {
  if (k < 0 || k >= static_cast<int>(e.pts.size())) return 0.0;
  const Pt& p = e.pts[static_cast<size_t>(k)];
  double v = static_cast<double>(p.value);
  if (ofs != 0.0f) {
    const double d = static_cast<double>(p.weight * ofs) + v;
    v = d;
    if (clamp) v = std::clamp(d, 0.0, 1.0);
  }
  return v - e.centre;
}

void start(const Env& e, State& s, const State* shared) {
  if (shared && (e.flags & 2)) {  // shared envelope: continues where the held voice is
    s = *shared;
    if (s.waitStep >= 0) {
      s.frozen = true;
      s.v0 = s.frozenV;
      s.delta = 0;
      s.out = s.v0;
    }
  } else {
    s = State{};
    genericStart(e, s);
  }
  for (int i = 0; i < 4; ++i) s.scale[i] = e.scale[i];
  if (!e.enabled) return;
  s.susOfs = e.sus >= 0 ? s.scale[2] : 0.0f;
  s.loopHold = !(s.loopHold == false && 0.0f < s.scale[1]);
  if (s.st >= 2) s.ofs = s.susOfs;
  s.out = static_cast<float>(pointValue(e, s.ofs, 0));
}

void tick(const Env& e, State& s, double dt) {
  const int n = static_cast<int>(e.pts.size());
  if (s.k < 0 || s.k >= n) return;
  s.phi = dt * s.rate + s.phi;
  while (s.phi >= 1.0) {
    if (s.rate == 0.0) s.phi = dt;
    else s.phi = (s.phi - 1.0) / s.rate;
    if (s.k >= n) {  // a loop starting on the last point: nothing to loop
      s.phi = 0, s.rate = 0;
      break;
    }
    s.v0 = static_cast<float>(pointValue(e, s.ofs, s.k));
    if (e.hasWait && s.st < 6 && e.pts[static_cast<size_t>(s.k)].wait) {
      if (s.st == 5) {
        s.waitStep = -1;
        release(e, s);
      } else if (s.count > 0) {
        const long base = s.waitStep < 0 ? 0 : s.waitStep;
        s.waitStep = static_cast<int>((static_cast<long>(s.count) + static_cast<long>(e.pts[static_cast<size_t>(s.k)].wait) + base - 2) % static_cast<long>(s.count));
        s.frozen = s.waitStep != s.myIdx;
        s.frozenV = s.v0;
      }
    }
    bool hold;
    if (s.k < e.end || e.end < 0 || s.st > 5) {
      if (e.decay >= 0 && e.decay <= s.k && s.phase < 3) {
        if (s.st < 3) s.st = 3;
        s.phase = 3;
        s.ofs = s.susOfs;
      }
      if (s.k < n - 1) {
        s.k += 1;
        hold = false;
      } else {
        s.st = 9;
        hold = true;
      }
    } else if (e.loop < 0) {
      hold = true;
    } else {
      s.phase = 4;
      s.k = e.loop + 1;
      hold = s.loopHold || s.k >= n;
    }
    if (!s.frozen) {
      s.delta = static_cast<float>(pointValue(e, s.ofs, s.k) - static_cast<double>(s.v0));
    } else {
      s.v0 = s.frozenV;
      s.delta = 0;
    }
    if (!hold) {
      const int rel = (s.st >= 6 && e.sus >= 0) ? 1 : 0;
      const double T = static_cast<double>(s.scale[rel * 2 + (s.st > 2 ? 1 : 0)]) * static_cast<double>(e.pts[static_cast<size_t>(s.k)].time);
      if (T <= 0.0) {
        s.rate = 1.0;
        s.phi = s.phi + 1.0;
      } else {
        s.rate = e.speed / T;
        s.phi = s.phi * s.rate;
      }
    } else {
      s.phi = 0, s.rate = 0;
    }
  }
  if (s.delta == 0.0f || s.k < 0 || s.k >= n) s.out = s.v0;  // (a loop on the last point leaves k past the end)
  else s.out = shapeY(e.pts[static_cast<size_t>(s.k)], s.phi) * s.delta + s.v0;
}

void release(const Env& e, State& s) {
  if (s.st > 5) return;
  if (s.waitStep >= 0 && !s.frozen && s.count > 1) {  // the active step voice plays on up to the next wait point
    const int hi = static_cast<int>(e.pts.size()) - 1;
    int k = s.k, top = k;
    bool found = false;
    if (k >= 0 && k <= hi) {
      do {
        if (e.pts[static_cast<size_t>(k)].wait) {
          found = true;
          break;
        }
        int nt;
        if (e.sus == k && e.sus >= 0 && e.loop >= 0) {
          k = e.loop + 1;
          nt = k <= top ? top : k;
        } else {
          k = k + 1;
          nt = top;
          if (hi < k) break;
        }
        top = nt;
      } while (top != k);
    }
    if (found) {
      s.st = 5;
      if (s.rate == 0.0) s.phi = 1.0;
      return;
    }
  }
  if (e.sus >= 0) {
    s.k = e.sus;
    const float o = s.ofs, cur = s.out;
    const double pv = pointValue(e, o, s.k, false);
    s.ofs = static_cast<float>((static_cast<double>(cur) - pv) + static_cast<double>(o));
    s.phi = 1.0;
    s.frozen = false;
    s.phase = 6;
  }
  s.st = 6;
  if (s.rate == 0.0) s.phi = 1.0;
}

void setStep(const Env& e, State& s, int idx, int count) {
  s.myIdx = idx;
  s.count = count;
  if (s.st == 5 && count < 1) release(e, s);
}

float oscValue(const Env& e, uint32_t ph) {
  float v;
  if (e.oscT < ph) {
    v = 1.0f;
  } else {
    float l;
    if (ph < e.oscQ) {
      l = 1.0f - static_cast<float>(ph) * e.oscQinv;
    } else {
      if (ph < e.oscP) return -1.0f;
      l = static_cast<float>(static_cast<int64_t>(static_cast<uint64_t>(ph) - static_cast<uint64_t>(e.oscP))) * e.oscRinv - 1.0f;
    }
    if (!e.oscBipolar) {
      double y = static_cast<double>(std::fabs(l));
      if (bits(e.oscC) != 0) {
        if (!negBit(e.oscC)) y = 1.0 - static_cast<double>(e.oscCinv) * (std::exp(static_cast<double>(e.oscK) * (1.0 - y)) - 1.0);
        else y = static_cast<double>(e.oscCinv) * (std::exp(static_cast<double>(e.oscK) * y) - 1.0);
      }
      const float fy = static_cast<float>(y);
      const uint32_t u = (bits(l) & 0x80000000u) | bits(fy);  // sign of l onto |y|
      std::memcpy(&v, &u, 4);
    } else {
      double y = static_cast<double>((l + 1.0f) * 0.5f);
      if (e.oscC != 0.0f) {
        if (!negBit(e.oscC)) y = 1.0 - static_cast<double>(e.oscCinv) * (std::exp(static_cast<double>(e.oscK) * (1.0 - y)) - 1.0);
        else y = static_cast<double>(e.oscCinv) * (std::exp(static_cast<double>(e.oscK) * y) - 1.0);
      }
      v = static_cast<float>(y * 2.0 - 1.0);
    }
  }
  return v;
}

void lfoStart(const Env& e, State& s, double globalTicks) {
  s = State{};
  s.scale[0] = s.scale[1] = 1.0f, s.scale[2] = 0.0f, s.scale[3] = 1.0f;
  s.susOfs = 0;
  genericStart(e, s);
  const uint32_t base = e.oscP + 1u + ((e.oscT - e.oscP) >> 1);
  if (e.global)  // one free-running phase for every voice (the start phase does not apply)
    s.lfoPhase = base + static_cast<uint32_t>(static_cast<int64_t>(rnd(globalTicks * e.incRaw)));
  else
    s.lfoPhase = base + e.phaseOfs;
}

void lfoTick(const Env& e, State& s, double dt) {
  auto inDelay = [&]() { return s.k == 0 && s.st < 9 && s.phi < 1.0; };
  double d = dt;
  if (inDelay()) {
    d = (s.phi - 1.0) / s.rate + dt;
    if (d <= 0.0) d = 0.0;
  }
  tick(e, s, dt);
  if (!inDelay()) {
    s.lfoPhase += static_cast<uint32_t>(static_cast<int64_t>(rnd(d * e.inc)));
    s.lfoOut = oscValue(e, s.lfoPhase) * s.out;
  }
}

bool mapFlat(const Env& m, double neutral) {
  for (const auto& p : m.pts)
    if (static_cast<double>(p.value) != neutral) return false;
  return true;
}

float mapAt(const Env& m, double x) {
  const int n = static_cast<int>(m.pts.size());
  if (n == 0) return 0.0f;
  bool constant = true;
  for (const auto& p : m.pts)
    if (p.value != m.pts[0].value) constant = false;
  if (constant) return static_cast<float>(static_cast<double>(m.pts[0].value) - m.centre);
  if (n == 2) {  // two straight points: direct interpolation
    const Pt& p1 = m.pts[1];
    const bool family = p1.shape < 2 || (p1.shape >= 7 && p1.shape <= 9) || p1.shape == 11 || p1.shape == 12;
    if (family && bits(p1.tension) == 0) {
      const double len = static_cast<double>(m.pts[0].time) + static_cast<double>(m.pts[1].time);
      const float t0 = static_cast<float>(static_cast<double>(m.pts[0].value) - m.centre), t1 = static_cast<float>(static_cast<double>(m.pts[1].value) - m.centre);
      const float sc = static_cast<float>(1.0 / len);
      const int i = static_cast<int>(rnd(static_cast<double>(sc) * x - 0.5));
      if (i < 0) return t0;
      if (i < 1) {
        const double f = static_cast<double>(sc) * x - static_cast<double>(i);
        return static_cast<float>(static_cast<double>(t1) * f + (1.0 - f) * static_cast<double>(t0));
      }
      return t1;
    }
  }
  State s;
  mapBegin(m, s);
  mapAdvance(m, s, x);
  if (s.delta == 0.0f) return s.v0;
  return shapeY(m.pts[static_cast<size_t>(s.k)], s.phi) * s.delta + s.v0;
}

float mapIterStep(const Env& m, State& s, double dt) {
  const int n = static_cast<int>(m.pts.size());
  if (s.k < 0 || s.k >= n) return s.out;
  mapAdvance(m, s, dt);
  s.out = s.delta == 0.0f ? s.v0 : shapeY(m.pts[static_cast<size_t>(s.k)], s.phi) * s.delta + s.v0;
  return s.out;
}

float mapIterStart(const Env& m, State& s) {
  s = State{};
  if (m.pts.empty()) return 0.0f;
  mapBegin(m, s);
  const float first = s.v0;
  mapIterStep(m, s, 0.0);
  return first;  // the iterator reports the first point before any step
}

}  // namespace gl::packs::synths::mfmref::env

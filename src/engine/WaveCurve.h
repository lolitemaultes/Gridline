// Gridline — Waveshaper free-point curve (PluginState::data "WS1|x,y,tension,mode ..."):
// x = input, y = output, both 0..1. Unipolar (mirror on): x = |input|, sign
// restored; bipolar: x 0..1 = input -1..1, y 0..1 = output -1..1. Segment
// shapes are menv modes (shape on the segment's END point). Without data the
// 17 evenly spaced points of the parameters are used (0.5 projects).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "MEnv.h"

namespace gl::wscurve {

inline constexpr int kPts = 256;
using Curve = menv::Env<kPts>;

inline bool has(const char* data) { return data && std::strncmp(data, "WS1", 3) == 0; }

// Parses the curve (no allocation). Returns false when data has no curve.
inline bool parse(const char* data, Curve& c) {
  c.clear();
  if (!has(data)) return false;
  const char* p = std::strchr(data, '|');
  if (!p) return false;
  ++p;
  char* end = nullptr;
  while (*p && *p != '\n') {
    double x = std::strtod(p, &end);
    if (end == p) break;
    p = end;
    double v[3] = {0, 0, 0};
    for (int k = 0; k < 3 && *p == ','; ++k) {
      ++p;
      v[k] = std::strtod(p, &end);
      p = end;
    }
    c.add(std::clamp(x, 0.0, 1.0), static_cast<float>(std::clamp(v[0], 0.0, 1.0)), static_cast<float>(std::clamp(v[1], -1.0, 1.0)),
          static_cast<uint8_t>(std::clamp(static_cast<int>(v[2]), 0, menv::ModeCount - 1)));
    while (*p == ' ') ++p;
  }
  c.prepare();
  return c.n > 0;
}

inline std::string serialize(const Curve& c) {
  std::string out = "WS1|";
  char buf[96];
  for (int i = 0; i < c.n; ++i) {
    std::snprintf(buf, sizeof buf, "%s%.9g,%.7g,%.5g,%d", i ? " " : "", c.x[i], static_cast<double>(c.y[i]), static_cast<double>(c.t[i]),
                  static_cast<int>(c.mode[i]));
    out += buf;
  }
  return out;
}

// Curve from the 17 legacy points (values -1..1 at inputs -1..1).
inline void fromPoints(const float* pts, int count, bool unipolar, Curve& c) {
  c.clear();
  for (int k = 0; k < count; ++k) {
    double u = -1.0 + 2.0 * k / (count - 1);
    if (unipolar) {
      if (u < -1e-9) continue;
      c.add(u, static_cast<float>(std::clamp(static_cast<double>(pts[k]), 0.0, 1.0)));
    } else {
      c.add((u + 1) * 0.5, static_cast<float>(std::clamp((pts[k] + 1.0) * 0.5, 0.0, 1.0)));
    }
  }
  // Drop interior points that lie on the straight line between their neighbours.
  for (int i = c.n - 2; i >= 1; --i) {
    double f = (c.x[i] - c.x[i - 1]) / std::max(1e-12, c.x[i + 1] - c.x[i - 1]);
    double lin = c.y[i - 1] + (c.y[i + 1] - c.y[i - 1]) * f;
    if (std::fabs(lin - c.y[i]) < 1e-5) c.erase(i);
  }
  c.prepare();
}

// Polarity switch (keeps the transfer function): unipolar → bipolar mirrors the
// curve to the negative half; bipolar → unipolar keeps the positive half.
inline void toBipolar(Curve& c) {
  Curve o = c;
  c.clear();
  for (int i = o.n - 1; i >= 0; --i) {
    uint8_t m = i + 1 < o.n ? o.mode[i + 1] : menv::SingleCurve;
    float t = i + 1 < o.n ? o.t[i + 1] : 0.f;
    bool curveFamily = m == menv::SingleCurve || m == menv::SingleCurve2 || m == menv::HalfSine || m == menv::SingleCurve3;
    if (i == o.n - 1) m = menv::SingleCurve, t = 0;
    c.add(0.5 - o.x[i] * 0.5, 0.5f - o.y[i] * 0.5f, curveFamily ? -t : t, m);
  }
  for (int i = 0; i < o.n; ++i) {
    if (i == 0 && o.x[0] == 0 && o.y[0] == 0) continue;
    c.add(o.x[i] * 0.5 + 0.5, o.y[i] * 0.5f + 0.5f, o.t[i], o.mode[i]);
  }
  c.prepare();
}
inline void toUnipolar(Curve& c) {
  Curve o = c;
  c.clear();
  c.add(0, 0.f);
  for (int i = 0; i < o.n; ++i)
    if (o.x[i] >= 0.5 && !(o.x[i] == 0.5 && c.n == 1 && false))
      c.add((o.x[i] - 0.5) * 2, std::clamp((o.y[i] - 0.5f) * 2.f, 0.f, 1.f), o.t[i], o.mode[i]);
  c.prepare();
}

}  // namespace gl::wscurve

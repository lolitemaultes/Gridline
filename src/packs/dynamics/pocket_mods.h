// Gridline — "dynamics" pack, J-42: Pocket Rack module engines (gl.pocketrack).
// Reference models of the module rack FL Studio ships as "FL Studio Mobile Rack
// FX" (behaviour study + the J-42 module notes;
// own code). Each module gets its normalised parameters (index 0 = enable) and
// processes a stereo float block in place. Module memory is allocated by
// prepare(sr) (UI thread through prepareHost, or once when the rate changes);
// process() never allocates.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "../../engine/FullReverbRef.h"

namespace gl::packs::dynamics::pocket {

inline constexpr int kParams = 31;  // PRMS[0..30] (0 = enable)
inline constexpr double kPi = 3.14159265358979323846;
inline float dbLin(float d) { return std::pow(10.0f, d / 20.0f); }
inline float clampf(float x, float a, float b) { return std::min(std::max(x, a), b); }
inline double fracd(double x) { return x - std::trunc(x); }
inline int roundAway(double x) { return static_cast<int>(x < 0 ? -std::floor(-x + 0.5) : std::floor(x + 0.5)); }

// Per-call host information.
struct Host {
  double pos = -1;   // song position in beats (< 0: none)
  double bpm = 120;  // tempo
  bool playing = false;
};

class Module {
 public:
  virtual ~Module() = default;
  // Applies changed parameters (index order; first call = every index).
  void set(const float* p) {
    for (int i = 0; i < kParams; ++i)
      if (!init_ || p[i] != v_[i]) {
        v_[i] = p[i];
        onParam(i, p[i]);
      }
    init_ = true;
  }
  virtual void prepare(double sr) = 0;
  // Returns false when the module left the block untouched.
  virtual void process(float* L, float* R, int n, const Host& h) = 0;
  virtual int latency() const { return 0; }
  double sr() const { return sr_; }

 protected:
  virtual void onParam(int i, float x) { (void)i, (void)x; }
  float P(int i) const { return v_[i]; }
  float v_[kParams] = {};
  bool init_ = false;
  double sr_ = 44100;
};

// ------------------------------------------------------------------ shared biquad / designer
struct Bq {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
struct BqState {
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  float run(const Bq& q, float x) {
    const float y = q.b0 * x + q.b1 * x1 + q.b2 * x2 - q.a1 * y1 - q.a2 * y2;
    x2 = x1, x1 = x, y2 = y1, y1 = y;
    return y;
  }
};
// Shelf / peak designer with V = 10^(G/20) in the cookbook formulas (type 0 low shelf, 1 peak, 2 high shelf).
inline Bq bandDesign(double sr, float G, float f, float bw, int type) {
  const float V = std::pow(10.0f, G / 20.0f);
  float w0 = static_cast<float>(2 * kPi * f / sr);
  w0 = clampf(w0, 0.f, 3.f);
  const float sn = std::sin(w0), cs = std::cos(w0);
  const float S = static_cast<float>(2.0 * std::sinh(0.34657359027997264 * bw * w0 / (sn != 0 ? sn : 1e-30f)));
  float b0, b1, b2, a0, a1, a2;
  if (type == 1) {
    const float al = 0.5f * sn * S;
    b0 = 1 + al * V, b1 = -2 * cs, b2 = 1 - al * V, a0 = 1 + al / V, a1 = -2 * cs, a2 = 1 - al / V;
  } else {
    const float K = sn * std::sqrt(V) * S;
    if (type == 0) {
      b0 = V * ((V + 1) - (V - 1) * cs + K), b1 = 2 * V * ((V - 1) - (V + 1) * cs), b2 = V * ((V + 1) - (V - 1) * cs - K);
      a0 = (V + 1) + (V - 1) * cs + K, a1 = -2 * ((V - 1) + (V + 1) * cs), a2 = (V + 1) + (V - 1) * cs - K;
    } else {
      b0 = V * ((V + 1) + (V - 1) * cs + K), b1 = -2 * V * ((V - 1) + (V + 1) * cs), b2 = V * ((V + 1) + (V - 1) * cs - K);
      a0 = (V + 1) - (V - 1) * cs + K, a1 = 2 * ((V - 1) - (V + 1) * cs), a2 = (V + 1) - (V - 1) * cs - K;
    }
  }
  Bq q;
  auto fl = [](float v) { return std::fabs(v) < 1e-9f ? 0.f : v; };
  q.b0 = fl(b0 / a0), q.b1 = fl(b1 / a0), q.b2 = fl(b2 / a0), q.a1 = fl(a1 / a0), q.a2 = fl(a2 / a0);
  return q;
}

// Integer sample delay line (fixed after prepare).
struct Delay {
  std::vector<float> b;
  int w = 0;
  void init(int n) { b.assign(static_cast<size_t>(std::max(1, n)), 0.f), w = 0; }
  void clear() { std::fill(b.begin(), b.end(), 0.f), w = 0; }
  float tick(float x) {  // returns the sample written size() samples ago
    if (b.empty()) return x;
    const float y = b[static_cast<size_t>(w)];
    b[static_cast<size_t>(w)] = x;
    w = w + 1 == static_cast<int>(b.size()) ? 0 : w + 1;
    return y;
  }
};

// Power-of-two ring (write, then read d samples back from the newest, linear).
struct Ring {
  std::vector<float> b;
  int w = 0, mask = 0;
  void init(int minSize) {
    int s = 1;
    while (s < minSize) s <<= 1;
    b.assign(static_cast<size_t>(s), 0.f), mask = s - 1, w = 0;
  }
  void clear() { std::fill(b.begin(), b.end(), 0.f), w = 0; }
  void write(float x) {
    b[static_cast<size_t>(w)] = x;
    w = (w + 1) & mask;
  }
  float read(float d) const {
    const int i = static_cast<int>(d);
    const float f = d - static_cast<float>(i);
    const float a = b[static_cast<size_t>((w - 1 - i) & mask)], c = b[static_cast<size_t>((w - 2 - i) & mask)];
    return (1 - f) * a + f * c;
  }
  float readAbs(float pos) const {
    const int i = static_cast<int>(pos);
    const float f = pos - static_cast<float>(i);
    return (1 - f) * b[static_cast<size_t>(i & mask)] + f * b[static_cast<size_t>((i + 1) & mask)];
  }
};

// Variable-speed tape line: samples recorded with a tape-time stamp; the play head
// is one tape unit behind, so the delay is 1/speed samples (2 … N).
struct TapeLine {
  struct Chunk {
    std::vector<float> s;
    std::vector<double> t;
    int count = 0, readIdx = 0;
  };
  int N = 4;
  double minStep = 0.25, target = 0.5, speed = -1, cursor = 0;
  float fb = 0, fbSign = 1;
  Chunk c[2];
  int rec = 0;  // record chunk index
  static constexpr int kPre = 3;
  void init(int size, bool invert = false) {
    N = size >= 5 ? size : 4;
    minStep = 1.0 / N;
    fbSign = invert ? -1.f : 1.f;
    target = 1.0 / (2.0 + (N - 2) * 0.5);
    for (auto& k : c) k.s.assign(static_cast<size_t>(N + 256 + kPre), 0.f), k.t.assign(static_cast<size_t>(N + 256 + kPre), 0.0);
    clear();
  }
  void clear() {
    cursor = 0, speed = -1, rec = 0;
    for (auto& k : c) {
      std::fill(k.s.begin(), k.s.end(), 0.f), std::fill(k.t.begin(), k.t.end(), 0.0);
      k.count = k.readIdx = 0;
    }
  }
  void setDelay(double pos) {
    if (!std::isfinite(pos) || pos < 0) pos = 0;
    const double a = 2.0 / N;
    target = 1.0 / (N * (a + (1 - a) * pos));
  }
  void append(Chunk& k, float s, double t) {
    if (k.count + kPre >= static_cast<int>(k.s.size())) return;
    k.s[static_cast<size_t>(kPre + k.count)] = s, k.t[static_cast<size_t>(kPre + k.count)] = t, ++k.count;
  }
  float tick(float x) {
    Chunk& R = c[1 - rec];
    float y = 0;
    if (R.count > 0) {
      int j = R.readIdx;
      while (j < R.count && R.t[static_cast<size_t>(kPre + j)] < cursor) ++j;
      R.readIdx = j;
      if (j < R.count) {
        const float t1 = static_cast<float>(R.t[static_cast<size_t>(kPre + j)]), t0 = static_cast<float>(R.t[static_cast<size_t>(kPre + j - 1)]);
        const float s1 = R.s[static_cast<size_t>(kPre + j)], s0 = R.s[static_cast<size_t>(kPre + j - 1)];
        y = s0 + (s1 - s0) * (t1 > t0 ? static_cast<float>((cursor - t0) / (t1 - t0)) : 0.f);
      }
    }
    if (std::fabs(y) < 1e-9f) y = 0;
    const float w = x + fbSign * fb * y;
    if (speed < 0) speed = target;
    const double step = std::max(minStep, std::min(0.9, speed));
    cursor += step;
    if (cursor >= 1) {
      Chunk& W = c[rec];
      append(W, w, cursor);
      // The record chunk becomes the play chunk; the last three entries of the old play chunk
      // move in front of it (times − 1).
      for (int q = 0; q < kPre; ++q) {
        const int src = R.count - kPre + q;
        const bool have = src >= 0;
        W.s[static_cast<size_t>(q)] = have ? R.s[static_cast<size_t>(kPre + src)] : 0.f;
        W.t[static_cast<size_t>(q)] = have ? R.t[static_cast<size_t>(kPre + src)] - 1.0 : -1.0;
      }
      W.readIdx = 0;
      R.count = R.readIdx = 0;
      rec = 1 - rec;
      cursor -= 1;
      append(c[rec], w, cursor);
    } else {
      append(c[rec], w, cursor);
    }
    speed += (target - speed) * 0.001;
    return y;
  }
};

// LFO with sine / saw / square / shaped-triangle outputs (phase in cycles).
struct Lfo4 {
  int shape = 0;
  float out = 0;
  double phase = 0, offset = 0, inc = 0, mul = 1, beats = 1, c = 0, m = 0, s = 0.5, w = 0;
  void advance(int n) {
    double p = std::fmod(phase + offset, 1.0);
    switch (shape) {
      case 0: out = static_cast<float>(std::sin(2 * kPi * p)); break;
      case 1: out = static_cast<float>(2 * p - 1); break;
      case 2: out = p > 0.5 ? 1.f : -1.f; break;
      case 3: {
        p = std::fmod(p, 1.0);
        const double tri = 2 * std::min(p / s, (1 - p) / (1 - s)) - 1;
        const double v = std::clamp((tri - w) / (1 - std::fabs(w)), -1.0, 1.0);
        const double e = std::exp2(4.0f * static_cast<float>(c));
        const double a = (v < 0 ? -1.0 : 1.0) * std::pow(std::fabs(v), e);
        out = static_cast<float>(m * std::sin(kPi * a / 2) + (1 - m) * a);
        break;
      }
      default: break;
    }
    phase = fracd(phase + n * inc);
  }
  void block(bool sync, double rateHz, double sr, const Host& h) {
    if (!sync) {
      inc = rateHz / sr;
      return;
    }
    const double period = beats * mul * 60.0 / std::max(1e-9, h.bpm);
    inc = period != 0 ? 1.0 / (sr * period) : 0;
    if (h.pos >= 0) phase = beats * mul > 0 ? fracd(h.pos / (beats * mul)) : fracd(phase);
  }
};

inline const double kTable9[9] = {0.25, 0.375, 0.5, 0.75, 1, 2, 4, 8, 16};
inline double table9(float x) {
  const int k = static_cast<int>(9 * x) - 1;
  return k >= 0 && k < 9 ? kTable9[k] : 0.125;
}
inline double table13(float x, double keep) {
  static const double t[13] = {0.25, 0.375, 0.5, 0.75, 1, 2, 4, 8, 16, 32, 64, 128, 256};
  const int k = static_cast<int>(12 * x);
  return k >= 0 && k < 13 ? t[k] : keep;
}
inline int shapeList(float x) { return std::clamp(roundAway(x * 2.0), 0, 2); }

// ================================================================== dynamics
class Compressor final : public Module {
  float thrDb = -20, thrLin = 0.1f, ratio = 1, attCoef = 1, relCoef = 0, slope = 1, autoMk = 1, makeup = 1, inLin = 1, levelX = 0.5f;
  bool autoOn = true;
  int hold = 0, holdCnt = 0;
  float env = 0, lvl = 0;
  void recalc() {
    if (ratio < 20) {
      const float out0 = thrDb + (0 - thrDb) / ratio;
      const float lin0 = out0 < -60 ? 0 : dbLin(out0);
      const float d = lin0 - thrLin;
      slope = d == 0 ? 1 : d / (1 - thrLin);
      autoMk = 1 / lin0;
    } else {
      slope = 0, autoMk = 1 / thrLin;
    }
  }
  void mk() { makeup = dbLin(40 * levelX + (autoOn ? -20.f : 0.f)); }
  void onParam(int i, float x) override {
    switch (i) {
      case 1: thrDb = -40 * (1 - x), thrLin = thrDb < -60 ? 0 : dbLin(thrDb), recalc(); break;
      case 2: ratio = 1 + 19 * x * x, recalc(); break;
      case 3: {
        const double t = 0.05 * x * x;
        attCoef = t < 1e-6 ? 1.f : static_cast<float>(1 - std::pow(0.1, 1 / (t * sr_)));
        break;
      }
      case 4: relCoef = static_cast<float>(std::pow(0.1, 1 / ((0.01 + 0.49 * x * x) * sr_))); break;
      case 5: levelX = x, mk(); break;
      case 6: hold = static_cast<int>(0.05 * x * x * sr_); break;
      case 7: autoOn = x >= 0.5f, mk(); break;
      case 8: inLin = dbLin(40 * x - 20); break;
      default: break;
    }
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    for (int i : {3, 4, 6}) onParam(i, v_[i]);
    holdCnt = 0, env = lvl = 0;
  }
  void process(float* L, float* R, int n, const Host&) override {
    for (int i = 0; i < n; ++i) {
      const float pk = std::max(std::fabs(L[i]) * inLin, std::fabs(R[i]) * inLin);
      if (pk > env) env = pk, holdCnt = hold;
      else if (holdCnt > 0) --holdCnt;
      else env *= relCoef;
      lvl += (env - lvl) * attCoef;
      float g = lvl >= thrLin && lvl > 0 ? (thrLin + (lvl - thrLin) * slope) / lvl : 1.f;
      g *= (autoOn ? autoMk : 1.f) * makeup;
      L[i] *= g, R[i] *= g;
    }
  }
};

class Limiter final : public Module {
  float inLin = 1, outLin = 1, relS = 0.1f, thrLin = 1, relPhase = 1, g0 = 1;
  std::vector<float> dl[2];
  int N = 1, wp = 0;
  void onParam(int i, float x) override {
    if (i == 1) inLin = dbLin(48 * x - 24);
    if (i == 2) outLin = dbLin(48 * x - 24);
    if (i == 3) relS = static_cast<float>(0.1 + 1.9 * std::pow(static_cast<double>(x), 4.0));
    if (i == 4) thrLin = dbLin(24 * x - 24);
  }

 public:
  int latency() const override { return N; }
  void prepare(double sr) override {
    sr_ = sr;
    N = std::max(1, static_cast<int>(0.002 * sr));
    for (auto& d : dl) d.assign(static_cast<size_t>(N), 0.f);
    wp = 0, relPhase = 1, g0 = 1;
  }
  void process(float* L, float* R, int n, const Host&) override {
    const float step = static_cast<float>(1.0 / (relS * sr_));
    for (int i = 0; i < n; ++i) {
      const float xl = L[i] * inLin, xr = R[i] * inLin;
      float w = 0;
      if (relPhase != 0) {
        w = static_cast<float>(std::sin(relPhase * kPi * 0.5));
        relPhase -= step;
        if (relPhase <= 0) relPhase = 0, w = 0;
      }
      const float g = g0 + (1 - w) * (1 - g0);
      const float pk = std::max(std::fabs(xl), std::fabs(xr));
      if (pk * g > thrLin) g0 = thrLin / pk, relPhase = 1;
      L[i] = dl[0][static_cast<size_t>(wp)] * g * outLin;
      R[i] = dl[1][static_cast<size_t>(wp)] * g * outLin;
      dl[0][static_cast<size_t>(wp)] = xl, dl[1][static_cast<size_t>(wp)] = xr;
      wp = (wp + 1) % N;
    }
  }
};

class Leveller final : public Module {
  float G = 1, gL = -1, gR = -1;
  void onParam(int i, float x) override {
    if (i == 1) {
      const float db = 72 * std::pow(clampf(x, 0, 1), 0.6f) - 60;
      G = db < -60 ? 0 : dbLin(db);
    }
  }

 public:
  void prepare(double sr) override { sr_ = sr, gL = gR = -1; }
  void process(float* L, float* R, int n, const Host&) override {
    const float p = P(2);
    const float tL = G * clampf(2 * (1 - p), 0, 1), tR = G * clampf(2 * p, 0, 1);
    if (gL < 0) gL = tL;
    if (gR < 0) gR = tR;
    const bool moving = gL != tL || gR != tR;
    for (int i = 0; i < n; ++i) {
      if (moving) gL += (tL - gL) * 0.01f, gR += (tR - gR) * 0.01f;
      L[i] *= gL, R[i] *= gR;
    }
  }
};

class Distortion final : public Module {
  float q = 1, gain = 1, b = 0, d[2] = {0, 0}, nn[2] = {1, 1}, held[2] = {0, 0}, dc[2] = {0, 0}, c = 0.005f;
  float hold = 0;
  int kind = 0;
  bool dirty = true;
  void onParam(int i, float x) override {
    if (i == 2) q = 1 + 256 * clampf(x * x * x * x, 0, 1);
    else dirty = true;
  }
  void update() {
    dirty = false;
    gain = dbLin(24 * P(3) - 24);
    kind = std::clamp(static_cast<int>(3 * P(5)), 0, 3);
    const float s = P(4);
    b = std::pow(2 * (P(6) - 0.5f), 5.f);
    const float D = P(1), w[2] = {std::min(1.f, 2 * s), std::min(1.f, 2 * (1 - s))};
    for (int k = 0; k < 2; ++k) {
      switch (kind) {
        case 0: d[k] = std::pow(10.f, 5 * D * w[k]) - 1, nn[k] = d[k] + 1; break;
        case 1: d[k] = std::pow(10.f, 4 * D * w[k]) - 0.99f, nn[k] = 1 / std::tanh(d[k]); break;
        case 2: d[k] = static_cast<float>(w[k] * (20.0 * D * D)), nn[k] = 1; break;
        default: d[k] = std::pow(10.f, 3 * D * D * w[k]), nn[k] = 1; break;
      }
    }
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    hold = 0, held[0] = held[1] = dc[0] = dc[1] = 0;
    c = static_cast<float>(220.5 / sr);
  }
  void process(float* L, float* R, int n, const Host&) override {
    if (dirty) update();
    for (int i = 0; i < n; ++i) {
      float y[2];
      float* io[2] = {&L[i], &R[i]};
      for (int k = 0; k < 2; ++k) {
        const float x = *io[k] + b;
        switch (kind) {
          case 0: y[k] = nn[k] * x / (1 + d[k] * std::fabs(x)); break;
          case 1: y[k] = nn[k] * std::tanh(d[k] * x); break;
          case 2: {
            const double ax = std::fabs(static_cast<double>(x));
            const double v = 1 - (1 - ax) * std::pow(10.0, -static_cast<double>(d[k]) * ax);
            y[k] = static_cast<float>(x < 0 ? -v : v);
            break;
          }
          default: y[k] = clampf(d[k] * x, -1, 1); break;
        }
      }
      if (hold <= 0) held[0] = y[0], held[1] = y[1], hold += q;
      hold -= 1;
      for (int k = 0; k < 2; ++k) {
        float v = held[k] * gain;
        if (b != 0) dc[k] += (v - dc[k]) * c, v -= dc[k];
        *io[k] = v;
      }
    }
  }
};

class Gate final : public Module {
  float thrDb = -60, hystDb = 0, cA = 0, cR = 0, flo = 0.001f, env = 0, gS = 0;
  double attStep = 1e9, relStep = 1e9;
  bool invert = false, open = false, holdArmed = false;
  int holdN = 0, holdCnt = 0, cnt = 0;
  double ms[2] = {0, 0};
  float buf[2][256] = {};
  float T1[1024], T2[1024];
  void onParam(int i, float x) override {
    switch (i) {
      case 1: thrDb = 66 * x - 60; break;
      case 2: hystDb = 66 * x * x; break;
      case 3: attStep = 1000.0 / (1000.0 * x * sr_); break;
      case 4: cA = 2 * x - 1; break;
      case 5: relStep = 1000.0 / (1000.0 * x * sr_); break;
      case 6: cR = 2 * x - 1; break;
      case 7: {
        const float m = 2 * x - 1;
        flo = std::pow(10.f, -60 * std::fabs(m) / 20);
        invert = m < 0;
        break;
      }
      case 8: holdN = static_cast<int>(x * sr_); break;
      default: break;
    }
  }
  static float cr(const float* T, float e) {
    const float p = 1023 * e;
    const int i = std::clamp(static_cast<int>(p), 0, 1023);
    const float f = p - static_cast<float>(i);
    const float a = i > 0 ? T[i] : 0.f, b = T[i], c1 = i < 1023 ? T[i + 1] : 1.f, d = i < 1022 ? T[i + 2] : 1.f;
    const float c3 = -0.5f * a + 1.5f * b - 1.5f * c1 + 0.5f * d, c2 = a - 2.5f * b + 2 * c1 - 0.5f * d, cc = 0.5f * (c1 - a);
    return ((c3 * f + c2) * f + cc) * f + b;
  }
  float curve(float e, float c) const { return c > 0 ? c * cr(T1, e) + (1 - c) * e : (1 + c) * e - c * cr(T2, e); }

 public:
  Gate() {
    for (int j = 0; j < 1024; ++j) {
      const double u = j / 1023.0;
      T1[j] = j == 0 ? 0.f : static_cast<float>(std::pow(1000.0, u - 1));
      T2[j] = j == 0 ? 0.f : static_cast<float>(1 + std::log10(u) / 3);
    }
  }
  int latency() const override { return 256; }
  void prepare(double sr) override {
    if (sr != sr_) {
      sr_ = sr;
      for (int i : {3, 5, 8}) onParam(i, v_[i]);
    }
  }
  void process(float* L, float* R, int n, const Host&) override {
    for (int i = 0; i < n; ++i) {
      if (holdCnt != 0) --holdCnt;
      const float oldL = buf[0][cnt], oldR = buf[1][cnt];
      buf[0][cnt] = L[i], buf[1][cnt] = R[i];
      ms[0] += static_cast<double>(L[i]) * L[i], ms[1] += static_cast<double>(R[i]) * R[i];
      if (++cnt == 256) {
        cnt = 0;
        const double dL = ms[0] > 0 ? 10 * std::log10(ms[0] / 256) : -60, dR = ms[1] > 0 ? 10 * std::log10(ms[1] / 256) : -60;
        ms[0] = ms[1] = 0;
        if (!open) open = dL > thrDb || dR > thrDb;
        if (open) {
          if (!holdArmed) holdCnt = holdN, holdArmed = true;
          if (holdCnt == 0) {
            const double cl = std::max(-60.0, static_cast<double>(thrDb - hystDb));
            if (dL < cl && dR < cl) open = false, holdArmed = false;
          }
        }
      }
      float c;
      if (open) {
        if (env < 1) env = std::min(1.f, static_cast<float>(env + attStep));
        c = cA;
      } else {
        if (env > 0) env = std::max(0.f, static_cast<float>(env - relStep));
        c = cR;
      }
      float g = curve(env, c);
      if (invert) g = 1 - g;
      const float gT = flo + (1 - flo) * g;
      gS = 0.1f * gT + 0.9f * gS;
      L[i] = oldL * gS, R[i] = oldR * gS;
    }
  }
};

class Autoduck final : public Module {
  double phase = 0;
  float gS = -1;
  float env(float ph) const {
    const float A = static_cast<float>(0.5 * P(2) + 0.001), Tl = P(4), H = P(6), C = P(3), S = P(5);
    const int k = static_cast<int>(5 * P(7));
    float t = ph - (S - 0.5f);
    if (t < 0) t += 1;
    else if (t > 1) t -= 1;
    float v;
    const bool att = t < A;
    if (att) {
      v = t / A;
    } else {
      const float T = std::max(Tl, 0.001f);
      if (t >= T) v = 0;
      else if (t < A + H * T) v = 1;
      else v = 1 - (t - A - H * T) / (T - A - H * T);
    }
    v = std::min(1.f, v);
    if (v <= 0) return 0;
    if (att) return std::pow(v, 0.5f * C + 0.5f);
    switch (k) {
      case 0: return std::pow(v, 0.5f * C + 0.5f);
      case 1: return std::pow(v, 0.9f * C + 0.1f);
      case 2: return std::pow(v, 1 + 2 * (1 - C));
      case 3: return std::pow(v, 1 + 5 * (1 - C));
      case 4:
      case 5: {
        const float nn = (k == 5 ? 5.f : 2.f) * (1 - C) + 1;
        const float w = v >= 0.5f ? 1 - v : v;
        const float s = 0.5f * std::pow(w, nn) / std::pow(2.f, -nn);
        return v >= 0.5f ? 1 - s : s;
      }
      default: return v;
    }
  }

 public:
  void prepare(double sr) override { sr_ = sr, phase = 0, gS = -1; }
  void process(float* L, float* R, int n, const Host& h) override {
    if (h.pos >= 0) phase = h.pos - std::trunc(h.pos);
    const double inc = h.bpm / (sr_ * 60);
    const float depth = std::sqrt(P(1));
    for (int i = 0; i < n; ++i) {
      const float e = env(static_cast<float>(phase));
      const float target = 1 - depth * e;
      phase += inc;
      if (phase >= 1) phase -= 1;
      gS = gS < 0 ? target : gS + (target - gS) * 0.01f;
      L[i] *= gS, R[i] *= gS;
    }
  }
};

// ================================================================== EQ / filter / stereo
// Eight shelf / peak bands in series, then the 8-sample skew of the reference's lane layout.
class EightBand : public Module {
 protected:
  Bq q[8];
  BqState z[8][2];
  Delay lat[2];
  bool dirty = true;
  virtual void design() = 0;
  void onParam(int, float) override { dirty = true; }

 public:
  int latency() const override { return 8; }
  void prepare(double sr) override {
    sr_ = sr, dirty = true;
    for (auto& b : z) b[0] = b[1] = BqState{};
    for (auto& d : lat) d.init(8);
  }
  void process(float* L, float* R, int n, const Host&) override {
    if (dirty) design(), dirty = false;
    for (int i = 0; i < n; ++i) {
      float x[2] = {L[i] + 1e-30f, R[i] + 1e-30f};
      for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < 8; ++k) x[c] = z[k][c].run(q[k], x[c]);
        x[c] = lat[c].tick(x[c]);
      }
      L[i] = x[0], R[i] = x[1];
    }
  }
};
class GraphicEq final : public EightBand {
  void design() override {
    static const float f[8] = {80, 160, 375, 750, 1500, 3000, 6000, 12000};
    for (int k = 0; k < 8; ++k) q[k] = bandDesign(sr_, 24 * P(1 + k) - 12, f[k], 1.f, k == 0 ? 0 : k == 7 ? 2 : 1);
  }
};
class TunedEq final : public EightBand {
  void design() override {
    const int key = static_cast<int>(11.0 * P(9) + 0.001f);
    const float x = P(10);
    const float bw = std::max(1e-4f, 0.1f + 10 * (x * x * x));
    for (int k = 0; k < 8; ++k) {
      const float f = static_cast<float>(110.0 * std::pow(2.0, k + (key - 9) / 12.0));
      q[k] = bandDesign(sr_, 24 * P(1 + k) - 12, std::max(0.f, f), bw, k == 0 ? 0 : k == 7 ? 2 : 1);
    }
  }
};

class ParamEq final : public Module {
  Bq q[2][4];
  BqState z[2][4];
  float Y[2][4] = {};
  bool dirty = true;
  void onParam(int, float) override { dirty = true; }
  void design() {
    for (int b = 0; b < 4; ++b) {
      const float x0 = P(4 * b + 1), x1 = P(4 * b + 2), x2 = P(4 * b + 3), x3 = P(4 * b + 4);
      const float G = ((x0 - 0.5f) * 2) * 12;
      const float f = static_cast<float>(std::exp2(5 + 9.25 * x1));
      const float bw = std::max(1e-4f, 0.25f + 7 * x2);
      const int kind = std::clamp(static_cast<int>(2.0 * x3), 0, 2);
      const int ch = std::clamp(static_cast<int>(4 * P(17 + b)), 0, 4);
      static const float mL[5] = {1, 1, 0, 1, -1}, mR[5] = {1, 0, 1, -1, 1};
      const double sr2 = static_cast<float>(2 * sr_);
      q[0][b] = bandDesign(sr2, G * mL[ch], f, bw, kind);
      q[1][b] = bandDesign(sr2, G * mR[ch], f, bw, kind);
    }
  }

 public:
  int latency() const override { return 1; }
  void prepare(double sr) override {
    sr_ = sr, dirty = true;
    for (auto& c : z)
      for (auto& s : c) s = BqState{};
    std::memset(Y, 0, sizeof Y);
  }
  void process(float* L, float* R, int n, const Host&) override {
    if (dirty) design(), dirty = false;
    for (int i = 0; i < n; ++i) {
      float* io[2] = {&L[i], &R[i]};
      for (int c = 0; c < 2; ++c) {
        for (int rep = 0; rep < 2; ++rep) {
          const float v = static_cast<float>(static_cast<double>(*io[c]) + 1e-30);
          const float X[4] = {Y[c][1], Y[c][2], Y[c][3], v};
          for (int k = 0; k < 4; ++k) Y[c][k] = z[c][k].run(q[c][k], X[k]);
        }
        *io[c] = Y[c][0];
      }
    }
  }
};

class FilterMod final : public Module {
  float g0 = 0, a1 = 0, a2 = 0, outG = 1;
  int mode = 0, blocks = 1;
  struct S {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  } z[8][2];
  Delay lat[2];
  bool dirty = true;
  void onParam(int, float) override { dirty = true; }
  void design() {
    const float f = static_cast<float>(std::exp2(4 + 10.3 * P(1)));
    const float q = 0.8f + 2 * P(2) * P(2);
    mode = std::clamp(static_cast<int>(2 * P(3)), 0, 2);
    const int sl = std::clamp(static_cast<int>(2 * P(4)), 0, 2);
    const int nb = sl == 0 ? 1 : sl == 1 ? 2 : 4;
    if (nb != blocks) {
      blocks = nb;
      for (auto& d : lat) d.init(blocks);
    }
    auto fl = [](float v) { return std::fabs(v) < 1e-9f ? 0.f : v; };
    if (mode == 1) {
      const double w = 2 * kPi * f / sr_, s = std::sin(w), c = std::cos(w), al = s / (2 * q);
      g0 = fl(static_cast<float>((s / 2) / (1 + al))), a1 = fl(static_cast<float>(2 * c / (1 + al))), a2 = fl(static_cast<float>((1 - al) / (1 + al)));
      outG = 1;
    } else {
      const float w = static_cast<float>(2 * kPi * f / sr_), s = std::sin(w), c = std::cos(w);
      g0 = fl(((mode == 0 ? 1 - c : 1 + c)) / (2 + s / q));
      a1 = fl(-2 * q * c / (s + q)), a2 = fl((q - s) / (s + q));
      outG = mode == 0 ? static_cast<float>(std::pow(0.8000000119, blocks)) : 1.f;
    }
  }

 public:
  int latency() const override { return blocks; }
  void prepare(double sr) override {
    sr_ = sr, dirty = true;
    for (auto& a : z) a[0] = a[1] = S{};
    for (auto& d : lat) d.init(blocks);
  }
  void process(float* L, float* R, int n, const Host&) override {
    if (dirty) design(), dirty = false;
    for (int i = 0; i < n; ++i) {
      float* io[2] = {&L[i], &R[i]};
      for (int c = 0; c < 2; ++c) {
        float x = static_cast<float>(static_cast<double>(*io[c]) + 1e-30);
        for (int k = 0; k < 2 * blocks; ++k) {
          S& s = z[k][c];
          float y;
          if (mode == 0) y = g0 * (x + 2 * s.x1 + s.x2) - a1 * s.y1 - a2 * s.y2;
          else if (mode == 2) y = g0 * (x - 2 * s.x1 + s.x2) - a1 * s.y1 - a2 * s.y2;
          else y = g0 * (x - s.x2) + a1 * s.y1 - a2 * s.y2;
          s.x2 = s.x1, s.x1 = x, s.y2 = s.y1, s.y1 = y;
          x = y;
        }
        *io[c] = lat[c].tick(x * outG);
      }
    }
  }
};

class Stereoizer final : public Module {
  std::vector<float> b1, bL, bR;
  int N1 = 1, N2 = 2, i0 = 0, w1 = 0, w2 = 0;
  float s = 0, D = 0, k = 0, dL = 0, dR = 0, tL = 1, tR = 1, gL = -1, gR = -1, z = 0;
  bool hp = false, panOn = false, dirty = true;
  void onParam(int, float) override { dirty = true; }
  void update() {
    dirty = false;
    s = P(1) * P(1);
    D = static_cast<float>(i0) + std::pow(P(2), 3.f) * static_cast<float>(N1 - 1 - i0);
    const float u = 2 * (1 - P(3)) - 1;
    hp = u < 0;
    const float c = hp ? -0.99f * u : 1 - 0.8f * u;
    k = c * c * c;
    const float v = std::pow(2 * P(4) - 1, 3.f);
    dL = v < 0 ? static_cast<float>(N2 - 1) * -v : 0, dR = v > 0 ? static_cast<float>(N2 - 1) * v : 0;
    tL = clampf(2 * (1 - P(5)), 0, 1), tR = clampf(2 * P(5), 0, 1);
    if (std::fabs(tL - 1) < 0.01f) tL = 1;
    if (std::fabs(tR - 1) < 0.01f) tR = 1;
    panOn = tL < 0.99f || tR < 0.99f;
    if (gL < 0) gL = tL;
    if (gR < 0) gR = tR;
  }
  static float interp(const std::vector<float>& b, int N, int w, float d) {
    float pos = static_cast<float>(w) - d;
    if (pos < 0) pos += static_cast<float>(N);
    const int i = static_cast<int>(pos);
    const int j = i + 1 >= N ? 0 : i + 1;
    const float fr = pos - static_cast<float>(i);
    return (1 - fr) * b[static_cast<size_t>(i)] + fr * b[static_cast<size_t>(j)];
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    N1 = std::max(1, static_cast<int>(0.05000000075 * sr));
    const int n2 = static_cast<int>(0.025000000373 * sr);
    N2 = n2 < 3 ? 2 : std::max(2, n2);
    i0 = static_cast<int>(0.0005 * sr);
    b1.assign(static_cast<size_t>(N1), 0.f), bL.assign(static_cast<size_t>(N2), 0.f), bR.assign(static_cast<size_t>(N2), 0.f);
    w1 = w2 = 0, z = 0, gL = gR = -1, dirty = true;
  }
  void process(float* L, float* R, int n, const Host&) override {
    if (dirty) update();
    const bool mvL = gL != tL, mvR = gR != tR;
    for (int i = 0; i < n; ++i) {
      const float inL = L[i], inR = R[i];
      float l = inL, r = inR;
      if (s > 0.001f) {
        const float d = interp(b1, N1, w1, D);
        z += (d - z) * k;
        const float e = hp ? d - z : z;
        l = inL + s * e, r = inR - s * e;
      }
      float oL = dL > 0.9f ? interp(bL, N2, w2, dL) : l, oR = dR > 0.9f ? interp(bR, N2, w2, dR) : r;
      b1[static_cast<size_t>(w1)] = inL + inR;
      w1 = w1 + 1 == N1 ? 0 : w1 + 1;
      bL[static_cast<size_t>(w2)] = l, bR[static_cast<size_t>(w2)] = r;
      w2 = w2 + 1 == N2 ? 0 : w2 + 1;
      if (panOn) {
        if (mvL) gL += (tL - gL) * 0.01f;
        if (mvR) gR += (tR - gR) * 0.01f;
        oL *= gL, oR *= gR;
      }
      L[i] = oL, R[i] = oR;
    }
    if (std::fabs(tL - gL) < 0.001f) gL = tL;
    if (std::fabs(tR - gR) < 0.001f) gR = tR;
  }
};

class Spacer final : public Module {
  Ring A, B, C, D;
  float ML = 1, MS = 1, p1 = 0.5f, p2 = 0.5f, p3 = 0.5f, p5 = 0.5f;

 public:
  void prepare(double sr) override {
    if (sr != sr_ || A.b.empty()) {
      sr_ = sr;
      ML = static_cast<float>(0.3 * sr), MS = static_cast<float>(0.1 * sr);
      A.init(static_cast<int>(std::ceil(ML))), B.init(static_cast<int>(std::ceil(ML)));
      C.init(static_cast<int>(std::ceil(MS))), D.init(static_cast<int>(std::ceil(MS)));
    } else {
      A.clear(), B.clear(), C.clear(), D.clear();
    }
  }
  void process(float* L, float* R, int n, const Host&) override {
    const int lr = std::clamp(roundAway(3.0 * P(4)), 0, 3), msi = std::clamp(roundAway(3.0 * P(6)), 0, 3), mode = std::clamp(roundAway(2.0 * P(7)), 0, 2);
    for (int i = 0; i < n; ++i) {
      p1 += (P(1) - p1) * 0.001f, p5 += (P(5) - p5) * 0.001f, p2 += (P(2) - p2) * 0.001f, p3 += (P(3) - p3) * 0.001f;
      const float dLR = std::pow(2 * (p1 - 0.5f), 3.f);
      A.write(L[i]), B.write(R[i]);
      float a = A.read(std::max(0.f, std::min(dLR * ML, ML))), b = B.read(std::max(0.f, std::min(-dLR * ML, ML)));
      if (lr & 1) a = -a;
      if (lr & 2) b = -b;
      if (p3 < 0.5f) {
        const float k = 0.5f - p3;
        a = a - k * b;
        b = b - k * a;
      } else {
        const float g = 2 - 2 * p3, M = 0.5f * (a + b), S = 0.5f * (a - b);
        a = M + g * S, b = M - g * S;
      }
      if (p2 != 0.5f) a *= std::min(1.f, 2 * (1 - p2)), b *= std::min(1.f, 2 * p2);
      float m, sd;
      if (mode != 2) m = 0.5f * (a + b), sd = 0.5f * (a - b);
      else m = a, sd = b;
      if (msi & 1) m = -m;
      if (msi & 2) sd = -sd;
      const float dMS = std::pow(2 * (p5 - 0.5f), 5.f);
      C.write(m), D.write(sd);
      const float m2 = C.read(std::max(0.f, std::min(dMS * MS, MS))), s2 = D.read(std::max(0.f, std::min(-dMS * MS, MS)));
      if (mode == 1) L[i] = m2, R[i] = s2;
      else L[i] = m2 + s2, R[i] = m2 - s2;
    }
  }
};

class Spreader final : public Module {
  float fs = 44100, pole = 0, w15 = 0, b15 = 0, r15 = 0;
  int Nd = 1, It = 1;
  float sm[5] = {};  // enable, width, separation, lowbypass, monobass (smoothed)
  float s1[2][2] = {}, s2[2][2] = {};
  std::vector<float> rA, rB;
  int wpos = 0;
  float phi = 0, step = 0, dA = 0, dB = 0;
  float lp1[2] = {}, hp1[2] = {}, x1[2] = {};

 public:
  void prepare(double sr) override {
    sr_ = sr;
    fs = static_cast<float>(std::min(192000.0, std::max(1.0, sr)));
    pole = std::exp(-500.f / fs);
    w15 = std::tan(15000.f * static_cast<float>(kPi) / fs);
    b15 = 1 / (1 / w15 + 1), r15 = 1 - 1 / w15;
    Nd = static_cast<int>(fs * 0.0063492064f);
    It = std::max(1, static_cast<int>(fs * 0.023219954f));
    rA.assign(2048, 0.f), rB.assign(2048, 0.f), wpos = 0;
    std::memset(sm, 0, sizeof sm), std::memset(s1, 0, sizeof s1), std::memset(s2, 0, sizeof s2);
    phi = step = dA = dB = 0;
    std::memset(lp1, 0, sizeof lp1), std::memset(hp1, 0, sizeof hp1), std::memset(x1, 0, sizeof x1);
  }
  void process(float* L, float* R, int n, const Host&) override {
    const float zone[5] = {1.f, 2 * P(1), P(4), static_cast<float>(10.0 * std::pow(1000.0, static_cast<double>(P(2)))), P(3)};
    const float k = std::sqrt(2.f);
    for (int i = 0; i < n; ++i) {
      for (int z = 0; z < 5; ++z) sm[z] = pole * sm[z] + (1 - pole) * zone[z];
      const float e = sm[0], wd = sm[1], sep = sm[2], fc = sm[3], mb = sm[4];
      const float g = std::tan(static_cast<float>(kPi) * fc / fs);
      float lp[2], hp[2];
      const float in[2] = {L[i], R[i]};
      for (int c = 0; c < 2; ++c) {
        const float x = e * in[c];
        auto svf = [&](int st, float v, float& bpOut) {
          const float h = g * (v - (g + k) * s1[c][st] - s2[c][st]) / (1 + g * (g + k));
          const float bp = h + s1[c][st];
          s1[c][st] = 2 * h + s1[c][st];
          const float t = g * bp;
          const float lpv = t + s2[c][st];
          s2[c][st] = 2 * t + s2[c][st];
          bpOut = bp;
          return lpv;
        };
        float bp1, bp2;
        const float l1 = svf(0, x, bp1);
        const float ap = x - 2 * k * bp1;
        const float l2 = svf(1, l1, bp2);
        lp[c] = l2, hp[c] = ap - l2;
      }
      const float HM = hp[0] + hp[1], HS = hp[0] - hp[1];
      rA[static_cast<size_t>(wpos)] = HM, rB[static_cast<size_t>(wpos)] = HS;
      const float d = std::trunc(static_cast<float>(Nd) * wd);
      const float pPrev = phi;
      float st = step;
      if (st != 0) st = (pPrev > 0 && pPrev < 1) ? st : 0;
      else if (pPrev == 0 && dA != d) st = 1.f / static_cast<float>(It);
      else if (pPrev == 1 && dB != d) st = -1.f / static_cast<float>(It);
      phi = clampf(pPrev + st, 0, 1);
      const float dAn = (pPrev >= 1 && dB != d) ? d : dA, dBn = (pPrev <= 0 && dA != d) ? d : dB;
      dA = dAn, dB = dBn, step = st;
      auto tap = [&](const std::vector<float>& r) {
        const int ia = static_cast<int>(std::min(static_cast<float>(Nd), std::max(0.f, dA)));
        const int ib = static_cast<int>(std::min(static_cast<float>(Nd), std::max(0.f, dB)));
        return r[static_cast<size_t>((wpos - ia) & 2047)] * (1 - phi) + r[static_cast<size_t>((wpos - ib) & 2047)] * phi;
      };
      wpos = (wpos + 1) & 2047;
      const float Dv = wd * tap(rA), Ev = wd * tap(rB);
      auto T = [&](int ch, float x) {
        const float lpv = b15 * (0.5f * (x + x1[ch]) - r15 * lp1[ch]);
        const float hpv = b15 * ((0.5f / w15) * (x - x1[ch]) - r15 * hp1[ch]);
        lp1[ch] = lpv, hp1[ch] = hpv, x1[ch] = x;
        return 2 * (lpv + 0.398107f * hpv);
      };
      const float tE = T(0, Ev), tD = T(1, Dv);
      const float midHi = 0.5f * HM + 0.5f * tE, sideHi = sep * (HS + tD);
      const float lowL = (1 - mb) * lp[0] + 0.5f * mb * (lp[0] + lp[1]), lowR = (1 - mb) * lp[1] + 0.5f * mb * (lp[0] + lp[1]);
      L[i] = (1 - e) * in[0] + lowL + midHi + sideHi;
      R[i] = (1 - e) * in[1] + lowR + midHi - sideHi;
    }
  }
};

class Comb final : public Module {
  TapeLine line[2];
  float fL = 0, fR = 0;
  void onParam(int i, float x) override {
    if (i == 10 || i == 11) {
      const int o = roundAway(7.0 * P(10)), nn = roundAway(11.0 * P(11));
      const int note = 12 * o + nn;
      v_[2] = note <= 0 ? 0.f : static_cast<float>(440.0 * std::pow(2.0, (note - 69) / 12.0));
    }
    (void)x;
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    const int N = std::max(4, static_cast<int>(sr));
    for (auto& l : line) l.init(N);
  }
  void process(float* L, float* R, int n, const Host&) override {
    const int mode = std::clamp(static_cast<int>(2 * P(1)), 0, 2);
    const bool snap = P(7) != 0;
    const int pol = std::clamp(static_cast<int>(3 * P(8)), 0, 3);
    const float a = (P(9) - 0.5f) * 0.1f, kk = 0.0001f + 0.1999f * (1 - P(6) * P(6)), fb = P(3), mix = P(4);
    const int oct = roundAway(4.0 * (P(5) - 0.5));
    line[0].fbSign = (pol & 1) ? -1.f : 1.f, line[1].fbSign = (pol & 2) ? -1.f : 1.f;
    line[0].fb = line[1].fb = fb;
    float f;
    if (mode == 0 && snap) {
      const int note = 12 * roundAway(7.0 * P(10)) + roundAway(11.0 * P(11));
      f = note <= 0 ? 0.f : static_cast<float>(440.0 * std::pow(2.0, (note - 69) / 12.0));
    } else if (mode == 0) {
      const float p = P(2) > 1 ? 1.f : P(2);
      f = 1 + 2499 * p * p * p * p;
    } else {  // note / pitch tracking (no note input here): the snapped note, offset by octaves
      const int note = 12 * roundAway(7.0 * P(10)) + roundAway(11.0 * P(11));
      f = (note <= 0 ? 0.f : static_cast<float>(440.0 * std::pow(2.0, (note - 69) / 12.0))) * std::pow(2.f, static_cast<float>(oct));
    }
    f = clampf(f, 1, 20000);
    for (int i = 0; i < n; ++i) {
      fL += kk * (f * (1 - a) - fL), fR += kk * (f * (1 + a) - fR);
      line[0].setDelay(fL > 0 ? 1.0 / fL : 0), line[1].setDelay(fR > 0 ? 1.0 / fR : 0);
      const float yl = line[0].tick(L[i]), yr = line[1].tick(R[i]);
      L[i] += mix * yl, R[i] += mix * yr;
    }
  }
};

// ================================================================== modulation / time
class Chorus final : public Module {
  std::vector<float> buf;
  int N = 1, w = 0;
  float phA = 0.62f, phB = 0.1f;

 public:
  void prepare(double sr) override {
    sr_ = sr;
    N = std::max(2, static_cast<int>(0.01 * sr));
    buf.assign(static_cast<size_t>(N), 0.f), w = 0, phA = 0.62f, phB = 0.1f;
  }
  void process(float* L, float* R, int n, const Host&) override {
    const float f = 4 * P(1) * P(1) + 0.2f, d = 0.4f * (0.9f * P(2) + 0.1f), s = P(3), m = P(4) * P(4);
    const float incA = static_cast<float>(f / sr_), incB = static_cast<float>((f + 0.1) / sr_);
    const float ND = static_cast<float>(N) * d;
    for (int i = 0; i < n; ++i) {
      if (w >= N) w = 0;
      buf[static_cast<size_t>(w)] = L[i] + R[i];
      float pA = static_cast<float>(0.5 + d * std::sin(2 * kPi * phA));
      pA = pA * pA * ND;
      const int iA = static_cast<int>(pA);
      const float fA = pA - static_cast<float>(iA);
      auto at = [&](int k) { return buf[static_cast<size_t>((w + k) % N)]; };
      const float tapA = (1 - fA) * at(iA) + fA * at(iA + 1);
      const float pB = static_cast<float>(0.5 + d * std::sin(2 * kPi * phB));
      const float tapB = (1 - pB) * at(0) + pB * at(1);
      phA += incA, phB += incB;
      phA -= std::trunc(phA), phB -= std::trunc(phB);
      L[i] += (tapB * (1 - s) + tapA) * m;
      R[i] += (tapA * (1 - s) + tapB) * m;
      ++w;
    }
  }
};

class Flanger final : public Module {
  Lfo4 lfo;
  TapeLine line[2];
  double syncBeats = 1;
  void onParam(int i, float x) override {
    if (i == 1) syncBeats = table13(x, syncBeats);
    if (i == 4) lfo.shape = shapeList(x);
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    const int N = static_cast<int>(0.005 * sr);
    for (auto& l : line) l.init(N);
  }
  void process(float* L, float* R, int n, const Host& h) override {
    lfo.beats = syncBeats;
    lfo.block(P(2) != 0, 0.1 + 9.9 * std::pow(static_cast<double>(P(3)), 3.0), sr_, h);
    const float fb = P(7) * (P(8) != 0 ? -1.f : 1.f), wet = P(10) * P(10) * (P(11) != 0 ? -1.f : 1.f);
    line[0].fb = line[1].fb = fb;
    const float half = 0.5f * P(6) * P(6), amp = P(5) * half, st = 0.5f * P(9);
    for (int i = 0; i < n; ++i) {
      float vL, vR;
      if (st == 0) {
        lfo.advance(1);
        vL = vR = lfo.out;
      } else {
        const double p0 = lfo.phase;
        lfo.advance(1);
        vL = lfo.out;
        const double p1 = lfo.phase;
        lfo.phase = fracd(p0 + st);
        lfo.advance(1);
        vR = lfo.out;
        lfo.phase = p1;
      }
      line[0].setDelay(clampf(0.5f * vL * amp + half, 0, 1)), line[1].setDelay(clampf(0.5f * vR * amp + half, 0, 1));
      const float yl = line[0].tick(L[i]), yr = line[1].tick(R[i]);
      L[i] += yl * wet, R[i] += yr * wet;
    }
  }
};

class Phaser final : public Module {
  Lfo4 lfo;
  double syncBeats = 1;
  float z[2][6] = {}, y[2] = {}, fLo = 0, fHi = 0.4f;
  void onParam(int i, float x) override {
    if (i == 1) syncBeats = table13(x, syncBeats);
    if (i == 8) lfo.shape = shapeList(x);
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    std::memset(z, 0, sizeof z), y[0] = y[1] = 0;
    fLo = static_cast<float>(50 / sr), fHi = std::min(0.499f, static_cast<float>(16000 / sr));
  }
  void process(float* L, float* R, int n, const Host& h) override {
    lfo.beats = syncBeats;
    lfo.block(P(6) != 0, 0.1 + 9.9 * std::pow(static_cast<double>(P(7)), 3.0), sr_, h);
    const float am = 0.25f * P(2) * P(2) * P(2), fb = 0.95f * P(3), fc = P(4) * P(4), mx = P(5) * P(5), st = 0.5f * P(9);
    auto coef = [&](float o) {
      const float f = clampf((o * am + fc) * (fHi - fLo) + fLo, fLo, fHi);
      return (1 - f) / (1 + f);
    };
    for (int i = 0; i < n; ++i) {
      float aL, aR;
      if (st == 0) {
        lfo.advance(1);
        aL = aR = coef(lfo.out);
      } else {
        const double p0 = lfo.phase;
        lfo.advance(1);
        aL = coef(lfo.out);
        const double p1 = lfo.phase;
        lfo.phase = fracd(p0 + st);
        lfo.advance(1);
        aR = coef(lfo.out);
        lfo.phase = p1;
      }
      float x[2] = {fb * y[0] + L[i] + 1e-30f, fb * y[1] + R[i] + 1e-30f};
      const float a[2] = {aL, aR};
      for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < 6; ++k) {
          const float yy = z[c][k] - a[c] * x[c];
          z[c][k] = x[c] + a[c] * yy;
          x[c] = yy;
        }
        y[c] = x[c];
      }
      L[i] += y[0] * mx, R[i] += y[1] * mx;
    }
  }
};

class Tremolo final : public Module {
  Lfo4 A, B;
  float rTarget = 1, rs = 0, ks = 0.95f, kr = 0.999f, smL = 0, smR = 0;
  double syncBeats = 0.125;
  bool sync = false;
  void onParam(int i, float x) override {
    auto lerp = [&](float a, float b) { return a + (b - a) * x; };
    switch (i) {
      case 1: rTarget = lerp(0.1f, 20.f); break;
      case 2:
        syncBeats = table9(x);
        for (Lfo4* l : {&A, &B}) l->out = 0, l->phase = l->offset = l->inc = 0, l->mul = 1, l->beats = syncBeats;
        break;
      case 7: sync = x > 0.5f; break;
      case 8: A.c = B.c = lerp(-1, 1); break;
      case 9: A.m = B.m = lerp(0.001f, 0.999f); break;
      case 10: A.s = B.s = lerp(0.001f, 0.999f); break;
      case 11: A.w = B.w = -(-0.999f + 1.998f * x); break;
      default: break;
    }
  }

 public:
  Tremolo() { A.shape = B.shape = 3; }
  void prepare(double sr) override {
    sr_ = sr;
    for (Lfo4* l : {&A, &B}) l->out = 0, l->phase = l->offset = l->inc = 0, l->mul = 1;
    kr = static_cast<float>(1 - 44.1 / sr);
    ks = static_cast<float>(std::exp(-2 * kPi / (sr * 0.003f)));
  }
  void process(float* L, float* R, int n, const Host& h) override {
    float hi = P(3), lo = P(4);
    if (hi < lo) hi = lo;
    A.beats = B.beats = syncBeats;
    A.block(sync, rs, sr_, h), B.block(sync, rs, sr_, h);
    A.offset = P(5), B.offset = P(5) + P(6);
    for (int i = 0; i < n; ++i) {
      float gA = (hi - lo) * (A.out + 1) * 0.5f * 0.99f + lo, gB = (hi - lo) * (B.out + 1) * 0.5f * 0.99f + lo;
      gA = gA < lo ? lo : std::min(gA, hi), gB = gB < lo ? lo : std::min(gB, hi);
      smL = (1 - ks) * gA + ks * smL, smR = (1 - ks) * gB + ks * smR;
      L[i] *= smL, R[i] *= smR;
      A.advance(1), B.advance(1);
      rs = (1 - kr) * rTarget + kr * rs;
    }
  }
};

class WowFlutter final : public Module {
  Lfo4 wow, flut;
  Ring rL, rR;
  float W = 0, F = 0, k = 0.999f, wa = 0, fa = 0, ra = 0, wowRate = 1, wowTarget = 1;
  double timer = 0, period = 2;
  uint32_t seed = 12345;
  float rnd(float a, float b) {
    seed = static_cast<uint32_t>((static_cast<uint64_t>(seed) * 48271u) % 2147483647u);
    return a + (b - a) * static_cast<float>((seed - 1) * (1.0 / 2147483648.0));
  }
  void step() {
    const float rw = 0.1f + 4.9f * P(1), r = P(10);
    const float lo = rw == 0.1f ? rw : rw + (0.1f - rw) * r;  // toward the range ends by the random amount
    const float hi = rw == 5.f ? rw : rw + (5.f - rw) * r;
    wowTarget = rnd(std::min(lo, hi), std::max(lo, hi));
    timer = 0;
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    const int sz = static_cast<int>((0.142 + 0.003) * sr);
    rL.init(sz), rR.init(sz);
    W = static_cast<float>(0.142 * sr), F = static_cast<float>(0.003 * sr), k = static_cast<float>(1 - 44.1 / sr);
    timer = 0;
  }
  void process(float* L, float* R, int n, const Host& h) override {
    const bool sync = P(7) > 0.5f;
    const float waT = static_cast<float>((std::exp(5.0 * P(3)) - 1) / 147.41316), faT = P(6), raT = P(10);
    const int k9 = static_cast<int>(9 * P(9)) - 1;
    const double rb = k9 >= 0 && k9 < 9 ? kTable9[k9] : 0.125;
    period = h.bpm >= 1 ? rb * 60.0 / static_cast<int>(h.bpm) : 0;
    wow.beats = table9(P(2)), flut.beats = table9(P(5));
    wow.block(sync, wowRate, sr_, h);
    flut.block(sync, 5 + 15 * P(4), sr_, h);
    for (int i = 0; i < n; ++i) {
      timer += 1.0 / sr_;
      if (timer >= period) step();
      rL.write(L[i]), rR.write(R[i]);
      wa = (1 - k) * waT + k * wa;
      const float d1 = clampf((wow.out + 1) * W * 0.5f * wa, 0, W);
      fa = (1 - k) * faT + k * fa;
      const float d = clampf(d1 + (flut.out + 1) * 0.5f * fa * F, d1, d1 + F);
      L[i] = rL.read(d), R[i] = rR.read(d);
      wow.advance(1), flut.advance(1);
      wowRate = (1 - k) * wowTarget + k * wowRate;
      ra = (1 - k) * raT + k * ra;
    }
  }
};

class TapeDelay final : public Module {
  TapeLine line[2];
  void update() {
    const float T = P(1) * P(1), a = P(3) - 0.5f;
    const float posR = (a > 0 ? 1 - a : 1) * T, posL = a < 0 ? (1 + a) * posR : T;
    line[0].setDelay(posL), line[1].setDelay(posR);
    line[0].fb = line[1].fb = P(2);
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    const int N = static_cast<int>(sr);
    for (auto& l : line) l.init(N);
    update();
    const long M = static_cast<long>(std::ceil(10 * sr));
    for (long i = 0; i < M; ++i) line[0].tick(0), line[1].tick(0);
  }
  void process(float* L, float* R, int n, const Host&) override {
    update();
    const float wet = P(4) * P(2);
    for (int i = 0; i < n; ++i) {
      const float yl = line[0].tick(L[i]), yr = line[1].tick(R[i]);
      L[i] += yl * wet, R[i] += yr * wet;
    }
  }
};

class TranceDelay final : public Module {
  std::vector<float> lineL, lineR, lineIn;
  int cap = 1, pL = 0, pR = 0, pI = 0;
  float lp[2] = {}, ps = -1;
  bool reset = false;
  void onParam(int i, float) override {
    if (i == 1 || i == 5) reset = true;
  }
  void clearAll() {
    std::fill(lineL.begin(), lineL.end(), 0.f), std::fill(lineR.begin(), lineR.end(), 0.f), std::fill(lineIn.begin(), lineIn.end(), 0.f);
    pL = pR = pI = 0, lp[0] = lp[1] = 0, ps = -1;
  }

 public:
  bool useGainedDry = false;
  void prepare(double sr) override {
    sr_ = sr;
    cap = std::max(1, static_cast<int>(2 * sr));
    lineL.assign(static_cast<size_t>(cap), 0.f), lineR.assign(static_cast<size_t>(cap), 0.f), lineIn.assign(static_cast<size_t>(cap), 0.f);
    clearAll();
    reset = false;
  }
  void process(float* L, float* R, int n, const Host& h) override {
    if (reset) clearAll(), reset = false;
    static const double kBeats[5] = {0.25, 0.375, 0.5, 0.75, 1.0};
    const double beats = kBeats[std::clamp(static_cast<int>(4 * P(1)), 0, 4)];
    const int D = static_cast<int>(sr_ * beats * 60 / std::max(1e-9, h.bpm));
    const float g = 0.99f * P(2), m = P(3) * P(3), f2 = 2 * P(4);
    const float a = f2 > 1 ? ((f2 - 1) * (f2 - 1)) * ((f2 - 1) * (f2 - 1)) : f2;
    const bool hp = f2 > 1;
    const int mode = std::clamp(static_cast<int>(2 * P(5) + 0.5f), 0, 2);
    auto filt = [&](int c, float d) {
      const float dd = static_cast<float>(static_cast<double>(d) + 1e-30);
      lp[c] += (dd - lp[c]) * a;
      return hp ? dd - lp[c] : lp[c];
    };
    if (mode == 1) {
      const int len = std::max(1, std::min(D, cap));
      const float pan = P(6);
      for (int i = 0; i < n; ++i) {
        if (ps < 0) ps = pan;
        if (ps != pan) {
          ps += (pan - ps) * 0.01f;
          if (std::fabs(ps - pan) < 0.001f) ps = pan;
        }
        const float gl = clampf(2 * (1 - ps), 0, 1), gr = clampf(2 * ps, 0, 1);
        const float iL = L[i] * gl, iR = R[i] * gr;
        if (pL >= len) pL = 0;
        if (pR >= len) pR = 0;
        const float fl = filt(0, lineL[static_cast<size_t>(pL)]), fr = filt(1, lineR[static_cast<size_t>(pR)]);
        const float dryL = useGainedDry ? iL : L[i], dryR = useGainedDry ? iR : R[i];
        lineR[static_cast<size_t>(pR)] = (iL + fl) * g;
        lineL[static_cast<size_t>(pL)] = (iR + fr) * g;
        L[i] = fl * m + dryL, R[i] = fr * m + dryR;
        ++pL, ++pR;
      }
      return;
    }
    const int len = std::max(1, std::min(mode == 2 ? 2 * D : D, cap)), lenIn = std::min(D, cap);
    for (int i = 0; i < n; ++i) {
      float inL = L[i];
      if (mode == 2 && lenIn > 0) {
        if (pI >= lenIn) pI = 0;
        lineIn[static_cast<size_t>(pI)] = inL;
        ++pI;
        if (pI >= lenIn) pI = 0;
        inL = lineIn[static_cast<size_t>(pI)];
      }
      const float in[2] = {inL, R[i]};
      std::vector<float>* ln[2] = {&lineL, &lineR};
      int* pp[2] = {&pL, &pR};
      float out[2];
      for (int c = 0; c < 2; ++c) {
        if (*pp[c] >= len) *pp[c] = 0;
        const float fv = filt(c, (*ln[c])[static_cast<size_t>(*pp[c])]);
        out[c] = m * fv + in[c];
        (*ln[c])[static_cast<size_t>(*pp[c])] = (fv + in[c]) * g;
        ++*pp[c];
      }
      L[i] = out[0], R[i] = out[1];
    }
  }
};

// Tape stop: steady state passes the input; Toggle Play edges run the speed ramps.
class TapeStop final : public Module {
  Ring rL, rR;
  float pos = 0, speed = 1, dSpeed = 0, target = 1, gF = 1, last[2] = {0, 0}, xfC = 0, xfStep = 0;
  int left = 0, xfLen = 0;
  bool on = true, init = false;

 public:
  void prepare(double sr) override {
    sr_ = sr;
    rL.init(static_cast<int>(5 * sr)), rR.init(static_cast<int>(5 * sr));
    pos = 0, speed = on ? 1.f : 0.f, left = 0;
  }
  void process(float* L, float* R, int n, const Host& h) override {
    if (!h.playing) return;
    const bool want = P(5) > 0.5f;
    if (!init) init = true, on = want, speed = on ? 1.f : 0.f;
    if (want != on) {
      on = want;
      const bool syncT = P(7) > 0.5f;
      const double freeT = std::max(0.005, 5.0 * P(on ? 2 : 1));
      const int k = roundAway(9.0 * P(on ? 4 : 3)) - 1;
      static const double kS[9] = {15, 22.5, 30, 45, 60, 120, 240, 480, 960};
      const double syncTsec = k >= 0 && k < 9 && h.bpm >= 1 ? std::max(0.005, kS[k] / static_cast<int>(h.bpm)) : 0.005;
      double T = syncT ? syncTsec : freeT;
      T *= on ? (1 - speed) : speed;
      if (left == 0) T = syncT ? syncTsec : freeT;
      left = std::max(0, static_cast<int>(T * sr_));
      target = on ? 1.f : 0.f;
      dSpeed = left > 0 ? (target - speed) / static_cast<float>(left) : 0;
      if (left == 0) speed = target;
      if (on) {
        gF = 0;
        xfLen = static_cast<int>(std::min(T, 0.1) * sr_);
        xfC = 0, xfStep = xfLen > 0 ? 1.f / static_cast<float>(xfLen) : 1.f;
      }
      pos = static_cast<float>(rL.w);
    }
    const float wv = 2 * P(6) - 1;
    const float wp = std::fabs(wv) < 0.005f ? 0.005f : 1 - std::pow(0.005f, -wv);
    const float r = 1 / (1 - wp);
    const float kf = static_cast<float>(std::exp(-2 * kPi / (sr_ * 0.005)));
    const float c = static_cast<float>(1 - 44.1 / sr_);
    for (int i = 0; i < n; ++i) {
      rL.write(L[i]), rR.write(R[i]);
      if (left > 0) {
        const float sp = speed;
        speed += dSpeed;
        --left;
        const float step = (std::pow(r, sp) - 1) * (1 / wp - 1);
        float y[2] = {rL.readAbs(pos), rR.readAbs(pos)};
        if (on) {
          gF = (1 - kf) * 1 + kf * gF;
          y[0] *= gF, y[1] *= gF;
          if (left <= static_cast<int>(0.1 * sr_)) {
            xfC = std::min(1.f, xfC + xfStep);
            y[0] = (1 - xfC) * y[0] + xfC * L[i], y[1] = (1 - xfC) * y[1] + xfC * R[i];
          }
        }
        last[0] = y[0], last[1] = y[1];
        L[i] = y[0], R[i] = y[1];
        pos += step;
        if (pos >= static_cast<float>(rL.b.size())) pos -= static_cast<float>(rL.b.size());
      } else if (on) {
        pos = static_cast<float>(rL.w);
      } else {
        last[0] *= c * c, last[1] *= c * c;
        L[i] = last[0], R[i] = last[1];
        pos = static_cast<float>(rL.w), gF = 0;
      }
    }
  }
};

// X / Y multi effect: the echo kind as the reference; the filter, panner and modulation
// kinds as Gridline approximations (not exact).
class MultiFx final : public Module {
  std::vector<float> bL, bR;
  int S = 1, D = -1, idx = 0;
  float g = 0, lp[2] = {}, hpS[2] = {};
  double ph = 0;
  float svf[2][2] = {};

 public:
  void prepare(double sr) override {
    sr_ = sr;
    S = std::max(2, static_cast<int>(1.5 * sr));
    bL.assign(static_cast<size_t>(S), 0.f), bR.assign(static_cast<size_t>(S), 0.f);
    D = -1, idx = 0, g = 0, ph = 0;
    std::memset(lp, 0, sizeof lp), std::memset(hpS, 0, sizeof hpS), std::memset(svf, 0, sizeof svf);
  }
  void process(float* L, float* R, int n, const Host& h) override {
    if (h.bpm < 1) return;
    static const double kRate[10] = {0.125, 0.25, 0.375, 0.5, 0.75, 1, 2, 4, 8, 16};
    const int kind = std::clamp(static_cast<int>(7 * P(1)), 0, 7);
    const float X = clampf(P(2), 0, 1), Y = clampf(P(3), 0, 1), amt = P(5) * std::fabs(P(5));
    const int shape = std::clamp(static_cast<int>(2 * P(6)), 0, 3);
    const double rate = kRate[std::clamp(static_cast<int>(9 * P(4)), 0, 9)];
    auto lfoAt = [&](double r) {
      const double inc = 1.0 / (sr_ * (r * 60 / h.bpm));
      double v;
      if (shape == 0) v = std::sin(2 * kPi * ph), ph = fracd(ph + inc);
      else if (shape == 1) ph = fracd(ph + inc), v = 1 - 2 * ph;
      else v = ph > 0.5 ? -1 : 1, ph = fracd(ph + inc);
      return static_cast<float>(v * amt);
    };
    if (kind == 0) {  // echo (reference)
      const double r2 = 2 * rate;
      if (h.pos >= 0) ph = fracd(h.pos / r2);
      double q = static_cast<float>((static_cast<float>(r2 * 60 / h.bpm) * 0.5) / 1.5);
      while (q > 1) q *= 0.5;
      const int Dn = static_cast<int>((S - 1) * q);
      if (Dn != D) {
        D = Dn;
        std::fill(bL.begin(), bL.end(), 0.f), std::fill(bR.begin(), bR.end(), 0.f);
        lp[0] = lp[1] = hpS[0] = hpS[1] = 0, idx = 0;
      }
      const int Dc = D < 2 ? 1 : std::min(D, S - 1);
      const float fbk = X * X, y2 = 2 * std::min(Y, 1.f), kLP = std::min(y2, 1.f) * 0.99f + 0.01f, kHP = y2 > 1 ? (y2 - 1) * 0.45f : 0;
      for (int i = 0; i < n; ++i) {
        const float p = lfoAt(r2);
        g += (1 - g) * 0.005f;
        if (idx >= Dc) idx = 0;
        float* bb[2] = {&bL[static_cast<size_t>(idx)], &bR[static_cast<size_t>(idx)]};
        float hpOut[2];
        for (int c = 0; c < 2; ++c) {
          const float d = *bb[c];
          const float lpn = lp[c] + (fbk * d - lp[c]) * kLP;
          hpS[c] = hpS[c] + (lp[c] - hpS[c]) * kHP;
          hpOut[c] = lp[c] - hpS[c];
          lp[c] = lpn;
        }
        *bb[0] = L[i] * g + hpOut[0], *bb[1] = R[i] * g + hpOut[1];
        ++idx;
        L[i] += (1 + p) * hpOut[0], R[i] += (1 - p) * hpOut[1];
      }
      return;
    }
    if (kind == 2 || kind == 3) {  // LP / HP filter (X frequency, Y resonance)
      const double f = 20.0 * std::pow(1000.0, static_cast<double>(X));
      const double gg = std::tan(kPi * std::min(f, 0.45 * sr_) / sr_), kq = 2.0 - 1.9 * Y;
      for (int i = 0; i < n; ++i) {
        float* io[2] = {&L[i], &R[i]};
        for (int c = 0; c < 2; ++c) {
          const double v = *io[c];
          const double hp = (v - (kq + gg) * svf[c][0] - svf[c][1]) / (1 + gg * (kq + gg));
          const double bp = gg * hp + svf[c][0];
          const double lpv = gg * bp + svf[c][1];
          svf[c][0] = static_cast<float>(gg * hp + bp), svf[c][1] = static_cast<float>(gg * bp + lpv);
          *io[c] = static_cast<float>(kind == 2 ? lpv : hp);
        }
      }
      return;
    }
    if (kind == 6) {  // panner
      for (int i = 0; i < n; ++i) {
        const float p = clampf(2 * X - 1 + lfoAt(rate), -1, 1), dist = 1 - 0.5f * Y;
        L[i] *= std::min(1.f, 1 - p) * dist, R[i] *= std::min(1.f, 1 + p) * dist;
      }
    }
  }
};


// ================================================================== multiband / waveshaper
class Multiband final : public Module {
  struct Bq2 {
    Bq q;
    BqState z[2];
  };
  struct Xover {
    Bq2 lp[2], hp[2];
    void design(double sr, double f) {
      const double w = 2 * kPi * f / sr, c = std::cos(w), al = std::sin(w) / (2 * 0.707f), a0 = 1 + al;
      Bq L, H;
      L.b0 = static_cast<float>((1 - c) / 2 / a0), L.b1 = static_cast<float>((1 - c) / a0), L.b2 = L.b0;
      H.b0 = static_cast<float>((1 + c) / 2 / a0), H.b1 = static_cast<float>(-(1 + c) / a0), H.b2 = H.b0;
      L.a1 = H.a1 = static_cast<float>(-2 * c / a0), L.a2 = H.a2 = static_cast<float>((1 - al) / a0);
      for (int k = 0; k < 2; ++k) lp[k].q = L, hp[k].q = H;
    }
    void clear() {
      for (auto* b : {&lp[0], &lp[1], &hp[0], &hp[1]}) b->z[0] = b->z[1] = BqState{};
    }
    void run(int c, float x, float& lo, float& hi) {
      lo = lp[1].z[c].run(lp[1].q, lp[0].z[c].run(lp[0].q, x));
      hi = hp[1].z[c].run(hp[1].q, hp[0].z[c].run(hp[0].q, x));
    }
  };
  struct Band {
    float env = 0, lvl = -1, eg = -1, ig = -1, pS = -1, wS = -1;
  };
  Xover x1, x2, x3;
  Band b[3];
  float master = -1, f1 = 0, f2 = 0;

 public:
  void prepare(double sr) override {
    sr_ = sr;
    f1 = f2 = -1;
    x1.clear(), x2.clear(), x3.clear();
    for (auto& q : b) q = Band{};
    master = -1;
  }
  void process(float* L, float* R, int n, const Host&) override {
    const float nf1 = static_cast<float>(20.0 * std::pow(2.0, 9.0 * P(2))), nf2 = static_cast<float>(20.0 * std::pow(2.0, 9.0 * P(3)));
    if (nf1 != f1) f1 = nf1, x1.design(sr_, f1);
    if (nf2 != f2) f2 = nf2, x2.design(sr_, f2), x3.design(sr_, f2);
    const float mT = static_cast<float>(std::pow(10.0, 24.0 * (2 * P(1) - 1) / 20.0)), pT = P(5) * P(5);
    const int stereo = static_cast<int>(P(30) + 0.5f);
    for (int i = 0; i < n; ++i) {
      float lo[2], hi[2], mid[2], high[2], low[2];
      const float in[2] = {L[i], R[i]};
      for (int c = 0; c < 2; ++c) {
        x1.run(c, in[c], lo[c], hi[c]);
        x2.run(c, hi[c], mid[c], high[c]);
        float a, h;
        x3.run(c, lo[c], a, h);
        low[c] = a + h;
      }
      float* bands[3] = {low, mid, high};
      float sum[2] = {0, 0};
      for (int k = 0; k < 3; ++k) {
        const int o = 6 + 7 * k;
        const int state = std::clamp(static_cast<int>(2 * P(o + 6)), 0, 2);
        float* y = bands[k];
        if (state == 1) {
          sum[0] += y[0], sum[1] += y[1];
          continue;
        }
        Band& B = b[k];
        const float igT = static_cast<float>(std::pow(10.0, 24.0 * (2 * P(o) - 1) / 20.0));
        const float thr = static_cast<float>(std::pow(10.0, -48.0 * (1 - P(o + 1)) / 20.0)), ratio = 1 + 30 * P(o + 2);
        const double ta = 0.05 * P(o + 3) * P(o + 3), tr = 0.01 + 0.99 * P(o + 4);
        const float att = ta < 1e-6 ? 1.f : static_cast<float>(1 - std::pow(0.1, 1 / (ta * 44100))), rel = static_cast<float>(1 - std::pow(0.1, 1 / (tr * 44100)));
        const float egT = state == 2 ? 0.f : static_cast<float>(std::pow(10.0, 24.0 * (2 * P(o + 5) - 1) / 20.0));
        B.eg = B.eg < 0 ? egT : B.eg;
        B.eg += (egT - B.eg) * 0.01f;
        B.ig = B.ig < 0 ? igT : B.ig;
        B.ig += (igT - B.ig) * 0.01f;
        const float xl = y[0] * B.ig, xr = y[1] * B.ig;
        const float det = std::max(std::fabs(xl), std::fabs(xr)) * B.ig;
        B.env = det > B.env ? det : B.env - B.env * rel;
        B.lvl = B.lvl < 0 ? B.env : B.lvl + (B.env - B.lvl) * att;
        const float g = B.lvl >= thr && B.lvl > 0 ? (thr + (B.lvl - thr) / ratio) / B.lvl : 1.f;
        float yl = xl * g + xl * B.pS, yr = xr * g + xr * B.pS;
        if (B.pS < 0) B.pS = pT;
        yl *= B.eg, yr *= B.eg;
        const float w = P(27 + k);
        float oL, oR;
        if (stereo == 0) {
          if (B.wS < 0) B.wS = w;
          if (B.wS != w) {
            B.wS += (w - B.wS) * 0.01f;
            if (std::fabs(B.wS - w) < 0.001f) B.wS = w;
          }
          const float mG = std::min(1.f, std::max(0.f, 2 * (1 - B.wS))), sG = std::min(1.f, std::max(0.f, 2 * B.wS));
          oL = 0.5f * (mG * (yl + yr) + sG * (yl - yr)), oR = 0.5f * (mG * (yl + yr) - sG * (yl - yr));
        } else {
          const float kk = 1 - w;
          if (kk < 0.5f) {
            const float d = 0.5f - kk;
            oL = yl - d * yr;
            oR = yr - d * oL;
          } else {
            const float M = (yl + yr) / 2, S = (yl - yr) / 2 * (2 - 2 * kk);
            oL = M + S, oR = M - S;
          }
        }
        B.pS += (pT - B.pS) * 0.01f;
        sum[0] += oL, sum[1] += oR;
      }
      master = master < 0 ? mT : master + (mT - master) * 0.01f;
      L[i] = sum[0] * master, R[i] = sum[1] * master;
    }
  }
};

inline constexpr int kMaxBlock = 2048;  // the rack runs its modules on chunks of at most this many frames
class Waveshaper final : public Module {
  // Smoothed controls (start at 0): drive, post dB, spread, mix, HC, LC, bass, mid f, mid g, treble, lfoX, lfoY.
  float sm[12] = {}, c = 0.999f;
  float a = 0, k = 0;  // shape controls (smoothed)
  int shape = 0;
  bool autoOn = false, first = true;
  double ph = 0;
  float lfoV = 0, R_ = 0.995f, kA = 0.99f, ag = 0, g2 = 0;
  struct Pre {
    float b0 = 0, a1 = 0, a2 = 0, f = -1, x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};
    bool lp = true;
    void design(double sr, float hz) {
      if (hz == f) return;
      f = hz;
      const float w = static_cast<float>(2 * kPi * hz / sr), s = std::sin(w), co = std::cos(w), Q = 0.7f;
      a1 = -2 * Q * co / (s + Q), a2 = (Q - s) / (Q + s);
      b0 = (lp ? 1 - co : 1 + co) / (2 + s / Q);
    }
    float run(int c, float x) {
      const float y = b0 * (x + (lp ? 2.f : -2.f) * x1[c] + x2[c]) - a1 * y1[c] - a2 * y2[c];
      x2[c] = x1[c], x1[c] = x, y2[c] = y1[c], y1[c] = std::fabs(y) < 1e-9f ? 0.f : y;
      return y1[c];
    }
  } hc, lc;
  Bq eq[3];
  BqState ez[3][2];
  float eqD[4] = {-999, -999, -999, -999};
  float dcIn[2] = {}, dcOut[2] = {};
  float sa = 0, sk = 0;  // shape a / k smoothed
  // Curve object nodes (Crossover / Rectify).
  struct Node {
    float x, y, c;
  };
  Node nodes[10];
  int nn = 0;
  float targetA() const {
    const float X = P(3), Y = P(4);
    (void)Y;
    switch (shape) {
      case 0: return 2 * X - 1;
      case 1: return -1 + 2 * X;
      case 3: return (2 - 4 * X) / std::max(1e-6f, sk);
      case 4: return 0.1f + 0.9f * X;
      case 6: return 0.6f + 0.4f * X;
      default: return X;
    }
  }
  float targetK() const {
    const float Y = P(4);
    switch (shape) {
      case 0: return clampf(1 + 9 * Y, 1, 10);
      case 1: return 1 + 9 * Y;
      case 3: return 1 + 7 * Y;
      case 4: return 0.1f + 0.6f * Y;
      case 6: return Y <= 0.5f ? -0.03f + (-4 + 0.03f) * (0.5f - Y) * 2 : 0.03f + (4 - 0.03f) * (Y - 0.5f) * 2;
      default: return Y;
    }
  }
  float evalCurve(float x) const {
    int it = 0;
    while (it < nn && nodes[it].x <= x) ++it;
    if (it == 0) return nodes[0].y;
    if (it >= nn) return nodes[nn - 1].y;
    const Node& p = nodes[it - 1];
    const Node& q = nodes[it];
    const float t = p.x == q.x ? p.x : (x - p.x) / (q.x - p.x);
    const float cc = clampf(q.c, -0.99f, 0.99f);
    const float tt = cc > 0 ? std::pow(t, 1 - cc) : 1 - std::pow(1 - t, 1 + cc);
    return p.y + (q.y - p.y) * tt;
  }
  void buildCurve() {
    const float X = sa, Y = sk;  // smoothed X / Y (stored in sa / sk for curve shapes)
    auto linc = [](float x, float i0, float i1, float o0, float o1) {
      const float v = o0 + (o1 - o0) * (x - i0) / (i1 - i0);
      return clampf(v, std::min(o0, o1), std::max(o0, o1));
    };
    if (shape == 2) {
      const float u = X > 0.5f ? 1 - X : X, e = Y >= 0.5f ? 1.f : 0.f;
      // stored order: Emin, 5, 0, 3, 6, 2, 1, 4, Emax
      nn = 9;
      nodes[0] = {-1, linc(Y, 0.5f, 0, -1, -0.63f), 0};
      nodes[1] = {-0.75f, linc(Y, 0.5f, 0, -0.75f, -0.5f), 0};
      nodes[2] = {linc(X, 1, 0, -0.13f, -0.25f), -0.1f, 0};
      nodes[3] = {linc(u, 0, 0.5f, -0.01f, -0.1f), linc(Y, 0.5f, e, -0.02f, -0.9f), 0.5f};
      nodes[4] = {0, 0, -0.3f};
      nodes[5] = {linc(u, 0, 0.5f, 0.01f, 0.1f), linc(Y, 0.5f, e, 0.02f, 0.9f), 0.3f};
      nodes[6] = {linc(X, 1, 0, 0.13f, 0.25f), 0.1f, -0.3f};
      nodes[7] = {0.75f, linc(Y, 0.5f, 0, 0.75f, 0.5f), 0};
      nodes[8] = {1, linc(Y, 0.5f, 0, 1, 0.63f), linc(Y, 0.5f, 0, 0, 0.16f)};
    } else {
      nn = 5;
      const float s1 = linc(X, 0, 0.5f, -1, 1);
      nodes[0] = {-1, s1, 0};
      nodes[1] = {linc(X, 0.5f, 1, -0.999f, 0), s1, 0};
      nodes[2] = {0, 0, clampf(0.8f * (Y - 0.5f) * 2, -1, 1)};
      nodes[3] = {0.999f, 1, clampf(-0.8f * (Y - 0.5f) * 2, -1, 1)};
      nodes[4] = {1, 1, 0};
    }
  }
  float shapeOf(float x) const {
    switch (shape) {
      case 0: {
        const float ap = std::fabs(a) < 1e-4f ? 1e-4f : a;
        const float sg = ap < 0 ? -1.f : 1.f;
        const float f5 = ((1 / std::fabs(ap) + 1) * sg / k) * k, f6 = f5 - ap, f3 = ap >= 0 ? k : -k;
        const float N = std::fabs(f3 / std::sqrt((f3 - ap) * (f3 - ap) + 1));
        const float u = x * k - ap;
        return ((x * k / std::sqrt(u * u + 1)) / N) / std::fabs((f5 / std::sqrt(f6 * f6 + 1)) / N);
      }
      case 1: {
        const float tk = std::tanh(k);
        return (std::tanh(k * x - a) / tk + std::tanh(a) / tk) / (std::tanh(std::fabs(a)) * (1 / tk) + 1 / tk);
      }
      case 3: return std::sin((x + a) * k);
      case 4: {
        const float p = ((x + 1) / 2) / k, f = p - std::trunc(p);
        return (f >= a ? 2 * (1 - f) / (1 - a) : 2 * f / a) - 1;
      }
      case 6: return static_cast<float>(x + std::exp(-std::exp(std::pow(20.0, 1.0 - a)) * x * x) * std::sin(2 * kPi * k * x));
      default: return evalCurve(x);
    }
  }
  void onParam(int i, float x) override {
    if (i == 1) shape = std::clamp(roundAway(6.0 * x), 0, 6);
    if (i == 2) {
      autoOn = x != 0;
      v_[6] = autoOn ? 0.5f : 1.f / 6.f;
    }
  }

 public:
  void prepare(double sr) override {
    sr_ = sr;
    c = static_cast<float>(1 - 44.1 / sr);
    R_ = static_cast<float>(std::exp(-2 * kPi * 35 / sr));
    kA = static_cast<float>(std::exp(-2 * kPi / (2.5 * sr)));
    first = true;
    hc.lp = true, lc.lp = false;
  }
  void process(float* L, float* R, int n, const Host& h) override {
    const float tgt[12] = {P(5), (P(6) - 0.5f) * 36, P(7) * P(7) * P(7) * P(7), P(8), static_cast<float>(20 * std::pow(1000.0, P(9))),
                           static_cast<float>(20 * std::pow(1000.0, P(10))), (P(11) - 0.5f) * 36, static_cast<float>(500 * std::pow(2.0, 2.0 * P(12))),
                           (P(13) - 0.5f) * 36, (P(14) - 0.5f) * 36, P(19), P(20)};
    static const double kSync[10] = {0.125, 0.25, 0.375, 0.5, 0.75, 1, 2, 4, 8, 16};
    const bool sync = P(16) != 0;
    double inc;
    if (sync) {
      const double beats = kSync[std::clamp(static_cast<int>(9 * P(17)), 0, 9)];
      inc = beats > 0 && h.bpm > 0 ? 1.0 / (sr_ * 60 * beats / h.bpm) : 0;
      if (h.pos >= 0) ph = fracd(h.pos / beats);
    } else {
      inc = (0.02 + 9.9 * std::pow(static_cast<double>(P(18)), 3.0)) / sr_;
    }
    const int lsh = std::clamp(roundAway(2.0 * P(15)), 0, 2);
    for (int i = 0; i < n; ++i) {
      sm[10] = c * sm[10] + (1 - c) * tgt[10], sm[11] = c * sm[11] + (1 - c) * tgt[11];
      lfoV = lsh == 0 ? static_cast<float>(std::sin(2 * kPi * ph)) : lsh == 1 ? static_cast<float>(2 * ph - 1) : (ph <= 0.5 ? -1.f : 1.f);
      ph = fracd(ph + inc);
    }
    const float Xe = clampf(P(3) + lfoV * sm[10] * 0.5f, 0, 1), Ye = clampf(P(4) + lfoV * sm[11] * 0.5f, 0, 1);
    (void)Xe, (void)Ye;
    double inS[2] = {0, 0};
    for (int i = 0; i < n; ++i) inS[0] += static_cast<double>(L[i]) * L[i], inS[1] += static_cast<double>(R[i]) * R[i];
    const float inRms = static_cast<float>(0.5 * (std::sqrt(inS[0] / n) + std::sqrt(inS[1] / n)));
    // Shaped path in place; the dry block is kept for the final mix (blocks are ≤ 2048 frames).
    double outS[2] = {0, 0};
    for (int i = 0; i < n; ++i) {
      dryCache_[i][0] = L[i], dryCache_[i][1] = R[i];
      for (int z = 0; z < 10; ++z) sm[z] = c * sm[z] + (1 - c) * tgt[z];
      sa = c * sa + (1 - c) * (shape == 2 || shape == 5 ? Xe : targetA());
      sk = c * sk + (1 - c) * (shape == 2 || shape == 5 ? Ye : targetK());
      a = sa, k = sk;
      if (shape == 2 || shape == 5) buildCurve();
      hc.design(sr_, sm[4]), lc.design(sr_, sm[5]);
      const float g = std::pow(10.f, 2 * sm[0]);
      float x[2] = {lc.run(0, hc.run(0, L[i])), lc.run(1, hc.run(1, R[i]))};
      x[0] = x[0] * g - sm[2], x[1] = x[1] * g + sm[2];
      for (int cc = 0; cc < 2; ++cc) {
        const float y = shapeOf(x[cc]);
        const float d = y - dcIn[cc] + R_ * dcOut[cc];
        dcIn[cc] = y, dcOut[cc] = d;
        x[cc] = d;
      }
      L[i] = x[0], R[i] = x[1];
      outS[0] += static_cast<double>(x[0]) * x[0], outS[1] += static_cast<double>(x[1]) * x[1];
    }
    const float outRms = std::max(static_cast<float>(0.5 * (std::sqrt(outS[0] / n) + std::sqrt(outS[1] / n))), 0.063095734f);
    const float ratio = inRms / outRms;
    if (first) ag = ratio, g2 = autoOn ? ratio : 1, first = false;
    for (int i = 0; i < n; ++i) {
      const float av = autoOn ? (ag = kA * ag + (1 - kA) * ratio) : 1.f;
      g2 = c * g2 + (1 - c) * av;
      float w[2] = {L[i] * g2, R[i] * g2};
      // post EQ (redesigned when a smoothed value moved more than 0.001)
      if (std::fabs(sm[6] - eqD[0]) > 0.001f) eqD[0] = sm[6], eq[0] = bandDesign(sr_, sm[6], 100, 3.75f, 1);
      if (std::fabs(sm[7] - eqD[1]) > 0.001f || std::fabs(sm[8] - eqD[2]) > 0.001f)
        eqD[1] = sm[7], eqD[2] = sm[8], eq[1] = bandDesign(sr_, sm[8], std::max(0.f, sm[7]), 3.75f, 1);
      if (std::fabs(sm[9] - eqD[3]) > 0.001f) eqD[3] = sm[9], eq[2] = bandDesign(sr_, sm[9], 3300, 1, 2);
      const float pg = std::pow(10.f, sm[1] / 20);
      const double mx = sm[3];
      for (int cc = 0; cc < 2; ++cc) {
        for (int e = 0; e < 3; ++e) w[cc] = ez[e][cc].run(eq[e], w[cc]);
        w[cc] *= pg;
        w[cc] = static_cast<float>(dryCache_[i][cc] * std::sqrt(1 - mx) + w[cc] * std::sqrt(mx));
      }
      L[i] = w[0], R[i] = w[1];
    }
  }

 private:
  float dryCache_[kMaxBlock][2] = {};
};

// Reverb module: the rack variant of the reference reverb network (two outputs:
// late + weighted reflections, raw reflections) mixed by the module's levels,
// separation and wet balance.
class Reverb2 final : public Module {
  fullrev::Engine eng;
  bool ready = false;
  std::vector<float> aL, aR, bL, bR;

 public:
  void prepare(double sr) override {
    sr_ = sr;
    const int sri = static_cast<int>(sr);
    eng.init(fullrev::Variant::Rack, sri, sri + 1);
    eng.resume();
    ready = true;
  }
  void process(float* L, float* R, int n, const Host&) override {
    if (!ready) return;
    auto tbl = [&](int i, float lo, float hi) { return static_cast<int>(std::floor(lo + (hi - lo) * clampf(P(i), 0, 1) + 0.5f)); };
    fullrev::Engine::RackSettings r;
    r.highCut = tbl(1, 5, 221), r.lowCut = tbl(2, 19, 3000), r.room = tbl(3, 1, 100), r.decay = tbl(4, 1, 200), r.diffusion = tbl(5, 0, 100);
    r.damp = tbl(6, 5, 221), r.predelayMs = tbl(11, 0, 1000), r.bass = tbl(14, 2, 30), r.cross = tbl(17, 25, 2000);
    r.modDepth = clampf(P(13), 0, 1), r.modSpeed = clampf(P(16), 0, 1);
    eng.setRack(r);
    const float dry = P(7), er = P(8), wet = P(9), sep = P(15), pan = P(18);
    const bool side = P(10) > 0.5f;
    const float gL = std::min(1.f, 2 * (1 - pan)), gR = std::min(1.f, 2 * pan);
    for (int i = 0; i < n; ++i) {
      float AL, AR, BL, BR;
      eng.tickRack(L[i], side ? -R[i] : R[i], AL, AR, BL, BR);
      float a = BL * er * 1.25f + AL * wet * 1.25f, b = BR * er * 1.25f + AR * wet * 1.25f;
      if (sep >= 0.5f) {
        const float M = (a + b) * 0.5f, S = (a - b) * 0.5f * (1 - (2 * sep - 1));
        a = S + M, b = M - S;
      } else {
        a = a - b * (0.5f - sep);
        b = b - a * (0.5f - sep);
      }
      L[i] = L[i] * dry * 1.25f + a * gL, R[i] = R[i] * dry * 1.25f + b * gR;
    }
  }
};

// Analysis modules (tuner, analyser) and the pitch corrector pass the audio.
class PassThrough final : public Module {
 public:
  void prepare(double sr) override { sr_ = sr; }
  void process(float*, float*, int, const Host&) override {}
};

// Module of a type id (nullptr = unknown type).
inline std::unique_ptr<Module> makeModule(int type) {
  switch (type) {
    case 3: return std::make_unique<ParamEq>();
    case 4: return std::make_unique<MultiFx>();
    case 6: return std::make_unique<Distortion>();
    case 7: return std::make_unique<GraphicEq>();
    case 8: return std::make_unique<Compressor>();
    case 9: return std::make_unique<FilterMod>();
    case 11: return std::make_unique<Chorus>();
    case 13: return std::make_unique<Flanger>();
    case 14: return std::make_unique<Phaser>();
    case 15: return std::make_unique<Stereoizer>();
    case 16: return std::make_unique<TranceDelay>();
    case 18: return std::make_unique<TapeDelay>();
    case 20: return std::make_unique<Leveller>();
    case 24: case 26: case 45: return std::make_unique<PassThrough>();
    case 27: return std::make_unique<Gate>();
    case 28: return std::make_unique<Limiter>();
    case 29: return std::make_unique<Autoduck>();
    case 30: return std::make_unique<TunedEq>();
    case 34: return std::make_unique<Spacer>();
    case 35: return std::make_unique<Reverb2>();
    case 36: return std::make_unique<Multiband>();
    case 40: return std::make_unique<Comb>();
    case 41: return std::make_unique<Waveshaper>();
    case 42: return std::make_unique<TapeStop>();
    case 43: return std::make_unique<WowFlutter>();
    case 44: return std::make_unique<Spreader>();
    case 46: return std::make_unique<Tremolo>();
    default: return nullptr;
  }
}
// Modules whose engine follows the reference exactly (the rest: approximations).
inline bool moduleExact(int type, const float* p) {
  switch (type) {
    case 35: return p[12] < 0.5f;             // reverb: tempo-synced pre-delay not modelled
    case 43: case 45: return false;           // wall-clock random drift, pitch corrector
    // J-46: clamped as floats first (decoded FL values can be huge: UB in the int conversion).
    case 4: return static_cast<int>(std::clamp(7 * p[1], 0.0f, 7.0f)) == 0;  // only the echo kind
    case 40: return static_cast<int>(std::clamp(2 * p[1], 0.0f, 2.0f)) == 0;  // manual pitch only
    case 42: return p[5] > 0.5f;  // steady play state passes the audio
    default: return makeModule(type) != nullptr;
  }
}

}  // namespace gl::packs::dynamics::pocket

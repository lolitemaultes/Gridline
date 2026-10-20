// Gridline — "synths" pack: Matrix FM reference model, effects and kernels (mfmref_fx.h).
//
// Several loops keep an extended-precision accumulator (long double): the
// model was measured on x86-64, where that is the 80-bit format; elsewhere
// long double is a double and results differ in the last bits only.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include "mfmref_fx.h"

namespace gl::packs::synths::mfmref::fx {

namespace {

using ld = long double;

int32_t rd32(const uint8_t* p) {
  return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                              (static_cast<uint32_t>(p[3]) << 24));
}
inline float flushDenorm(float x) { return std::fabs(x) < 5.96046448e-08f ? 0.0f : x; }
inline bool isZeroBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u == 0;
}
void flushTiny(double* s, int k) {
  for (int i = 0; i < k; ++i)
    if (std::fabs(static_cast<ld>(s[i])) < 5.96046448e-08L) s[i] = 0.0;
}
inline long rnd(double x) { return static_cast<long>(std::nearbyint(x)); }

// Chorus line detune factors.
const float kDetune[20] = {0.967f, 1.039f, 1.021f, 0.1013f, 0.977f, 1.033f, 0.997f, 0.971f, 1.061f, 0.947f,
                           1.031f, 0.1009f, 1.063f, 0.953f, 0.991f, 1.049f, 0.983f, 1.051f, 1.019f, 1.069f};
// Plate reverb line lengths and output taps (at 20000 units = full size).
const int kRvLen[12] = {142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
const int kRvTapA[7] = {266, 2974, 1913, 2496, 1690, 187, 1566};
const int kRvTapB[7] = {353, 3627, 1728, 3173, 1811, 335, 621};
const double kPi = 3.1415926535897931, kLnMilli = -6.9077552789821368;  // ln(0.001)
inline double pow4th(double x) { return std::exp(std::log(x) * 0.25); }
// Half-band decimator allpass coefficients (branch A: 0, 2, 4; branch B: 1, 3, 5).
const double kHalfband[6] = {0.14737711360104661, 0.039151597734460045, 0.48246854276970014,
                             0.30264684832849342, 0.88300502576937312, 0.6746159185469639};

bool quiet(const float* p, int count) {
  uint32_t thr;
  const float t = 9.99999997e-07f;
  std::memcpy(&thr, &t, 4);
  for (int i = 0; i < count; ++i) {
    uint32_t b;
    std::memcpy(&b, &p[i], 4);
    if (static_cast<uint32_t>(thr << 1) < static_cast<uint32_t>(b << 1)) return false;
  }
  return true;
}

inline ld ladderSat(ld x) {
  const ld a = std::fabs(x);
  const ld p = (3.0L + a) * a + 6.0L;
  return (x * p) / (a * p + 12.0L);
}

}  // namespace

void ramp(float target, float maxStep, float& inc, float& cur, float& start, int n) {
  start = cur;
  inc = target - start;
  if (inc != 0.0f) {
    cur = target;
    inc = inc / static_cast<float>(n);
    if (std::fabs(inc) > maxStep) {
      inc = std::copysign(maxStep, inc);
      cur = static_cast<float>(n) * inc + start;
      if (std::fabs(cur) < 5.96046448e-08f) {
        cur = 0;
        inc = -start / static_cast<float>(n);
      }
    }
  }
}

// ------------------------------------------------------------------ delay
void Delay::setup(const uint8_t raw[23], int sr, int blockFrames, double tempo, float lawVol) {
  const int32_t fb = rd32(raw), time = rd32(raw + 4), ofs = rd32(raw + 8);
  mode = rd32(raw + 16);
  const bool tempoOn = raw[20] != 0;
  on = raw[22] != 0;
  vol = lawVol;
  if (!on) return;
  const float ofsMs = static_cast<float>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(static_cast<float>(std::abs(ofs)) * 0.001953125f)) * 6.9087547793152204) - 1.0) * 50.0f);
  const int ofsFrames = static_cast<int>(rnd(static_cast<double>((static_cast<float>(sr) * ofsMs) / 100000.0f)));
  double len = static_cast<double>(static_cast<long>(sr) * static_cast<long>(time));
  if (!tempoOn) len = len / 384.0;
  else len = (len * 0.3125) / tempo;
  int d = static_cast<int>(rnd(len));
  if (d < 1) d = 1;
  const int total = blockFrames + ofsFrames + d;
  if (total != frames) {
    frames = total;
    buf.assign(static_cast<size_t>(frames) * 2, 0.f);
    rd = 0;
  }
  wr = (rd + d) % frames;
  ofsL = ofsFrames, ofsR = 0;
  if (ofs > 0) std::swap(ofsL, ofsR);
  fbTarget = static_cast<float>(fb) * 0.0078125f;
}

void Delay::process(const float* fx, bool writeInput, float* out, int n) {
  if (writeInput) {
    for (int c = 0; c < 2; ++c) {
      int p = wr + (c == 0 ? ofsL : ofsR);
      if (p >= frames) p -= frames;
      for (int i = 0; i < n; ++i) {
        float& b = buf[static_cast<size_t>(p) * 2 + static_cast<size_t>(c)];
        b = fx[2 * i + c] + b;
        if (++p >= frames) p = 0;
      }
    }
  }
  float inc, start;
  ramp(fbTarget, 0.001f, inc, fbCur, start, n);
  ld g = start;
  for (int i = 0; i < n; ++i) {
    float* r = &buf[static_cast<size_t>(rd) * 2];
    float* w = &buf[static_cast<size_t>(wr) * 2];
    const ld L = r[0], R = r[1];
    if (mode == 0) out[2 * i] = static_cast<float>(L), out[2 * i + 1] = static_cast<float>(R);
    else out[2 * i] = static_cast<float>(R), out[2 * i + 1] = static_cast<float>(L);
    r[0] = r[1] = 0.0f;
    if (mode == 0 || mode == 1) {
      w[0] = static_cast<float>(L * g + static_cast<ld>(w[0]));
      w[1] = static_cast<float>(R * g + static_cast<ld>(w[1]));
    } else {
      w[0] = static_cast<float>(R * g + static_cast<ld>(w[0]));
      w[1] = static_cast<float>(L * g + static_cast<ld>(w[1]));
    }
    w[0] = flushDenorm(w[0]);
    w[1] = flushDenorm(w[1]);
    g += static_cast<ld>(inc);
    if (++rd >= frames) rd = 0;
    if (++wr >= frames) wr = 0;
  }
}

// ------------------------------------------------------------------ biquad / EQ
void Biquad::design(double A, double alpha, double s, double c) {
  double n;
  switch (type) {
    case 1:  // low pass
      n = 1.0 / (1.0 + alpha);
      a1 = -2.0 * c * n, a2 = (1.0 - alpha) * n, b1 = (1.0 - c) * n, b0 = static_cast<double>(0.5f) * b1, b2 = b0;
      break;
    case 2:  // band pass
      n = 1.0 / (1.0 + alpha);
      a1 = -2.0 * c * n, a2 = (1.0 - alpha) * n, b1 = 0.0, b0 = n * alpha, b2 = -alpha * n;
      break;
    case 3:  // notch
      n = 1.0 / (1.0 + alpha);
      a1 = -2.0 * c * n, a2 = (1.0 - alpha) * n, b1 = a1, b0 = n, b2 = n;
      break;
    case 4: {  // high pass
      n = 1.0 / (1.0 + alpha);
      a1 = -2.0 * c * n, a2 = (1.0 - alpha) * n;
      const double d = -(1.0 + c) * n;
      b1 = d, b0 = static_cast<double>(0.5f) * -d, b2 = b0;
      break;
    }
    case 5:  // all pass
      n = 1.0 / (1.0 + alpha);
      a1 = -2.0 * c * n, a2 = (1.0 - alpha) * n, b1 = a1, b0 = a2, b2 = 1.0;
      break;
    case 6: {  // low shelf
      const double d2 = (A - 1.0) * c, be = alpha * s, d3 = 1.0 / (A + 1.0 + d2 + be);
      a1 = ((A + 1.0) * c + (A - 1.0)) * -2.0 * d3;
      a2 = ((A + 1.0 + d2) - be) * d3;
      b1 = ((A - 1.0) - (A + 1.0) * c) * 2.0 * A * d3;
      b0 = (((A + 1.0) - d2) + be) * A * d3;
      b2 = (((A + 1.0) - d2) - be) * A * d3;
      break;
    }
    case 7:  // peak
      n = 1.0 / (1.0 + alpha / A);
      a1 = -2.0 * c * n, a2 = (1.0 - alpha / A) * n, b1 = a1, b0 = (1.0 + alpha * A) * n, b2 = (1.0 - alpha * A) * n;
      break;
    case 8: {  // high shelf
      const double d3 = (A - 1.0) * c, be = alpha * s, d2 = 1.0 / (((A + 1.0) - d3) + be);
      a1 = ((A - 1.0) - (A + 1.0) * c) * 2.0 * d2;
      a2 = (((A + 1.0) - d3) - be) * d2;
      b1 = ((A + 1.0) * c + (A - 1.0)) * -2.0 * A * d2;
      b0 = (A + 1.0 + d3 + be) * A * d2;
      b2 = ((A + 1.0 + d3) - be) * A * d2;
      break;
    }
    default:
      break;
  }
}

void Biquad::process(float* b, int n) {
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < 2; ++c) {
      double t = x1[c] * b1 - y2[c] * a2;
      t = t + x2[c] * b2;
      t = t - y1[c] * a1;
      const double x = static_cast<double>(b[2 * i + c]);
      const double y = x * b0 + t;
      x2[c] = x1[c], x1[c] = x;
      y2[c] = y1[c], y1[c] = y;
      b[2 * i + c] = static_cast<float>(y);
    }
  for (double* p : {x1, x2, y1, y2})
    for (int c = 0; c < 2; ++c)
      if (std::fabs(p[c]) < static_cast<double>(5.96046448e-08f)) p[c] = 0.0;
}

void EqBand::setup(const int32_t raw[4], int sr, int tailFrames) {
  const int32_t gain = raw[0], freq = raw[1], bw = raw[2], type = raw[3];
  float hz = static_cast<float>(static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(static_cast<double>(freq) * 1.52587890625e-05)) * 4.6151205168412597) - 1.0)) * 159.9 + 10.0);
  const float bwf = static_cast<float>(static_cast<double>(bw) * 1.52587890625e-05);
  const float gdb = static_cast<float>(gain) * 0.00999999978f;
  countdown = gain == 0 ? tailFrames : -1;
  bq.type = type + 6;
  const float nyq = static_cast<float>(sr) * 0.4999f;
  if (nyq < hz) hz = nyq;
  const double w = static_cast<double>(hz) * (6.283185307179586 / static_cast<double>(sr));
  const double s = std::sin(w), c = std::cos(w);
  double A = 0, alpha;
  const int t = bq.type;
  if (t >= 1 && t <= 5) {
    alpha = s / (2.0 * (static_cast<double>(bwf) * 18.899999999999999 + 0.1));
  } else if (t == 7) {
    A = static_cast<double>(static_cast<float>(std::pow(10.0, static_cast<double>(static_cast<float>(static_cast<double>(gdb) * 0.025000000000000001)))));
    const float qq = static_cast<float>(std::exp(std::log(static_cast<double>(1000.0f + 1.0f)) * static_cast<double>(bwf)) - 1.0);
    alpha = s / (2.0 * (static_cast<double>(qq) * 0.099900000000000003 + 0.1));
  } else {
    A = static_cast<double>(static_cast<float>(std::pow(10.0, static_cast<double>(static_cast<float>(static_cast<double>(gdb) * 0.025000000000000001)))));
    const double S = static_cast<double>(bwf) * 1.8999999999999999 + 0.1;
    alpha = std::sqrt((A * A + 1.0) / S - (A - 1.0) * (A - 1.0));
  }
  bq.design(A, alpha, s, c);
}

// ------------------------------------------------------------------ chorus
void Chorus::setup(int ord, float depth, float speed, float delay, float spread, float cross, float vol, int sr, int blockFrames) {
  on = ord > 0;
  if (!on) return;
  const int order = std::max(1, ord);
  const float l78 = (1092.5f / 10.0f) * static_cast<float>(std::exp(static_cast<double>(depth) * 2.3978952727983707) - 1.0);
  const double d1 = std::exp(static_cast<double>(1.0f - speed) * 4.6151205168412597);
  const double l70 = (2147483648.0 / (static_cast<double>(static_cast<float>(d1 - 1.0)) * 75000.0 + 50000.0)) * 500.0;
  const float l68 = (4410.0f / 10.0f) * static_cast<float>(std::exp(static_cast<double>(delay) * 2.3978952727983707) - 1.0);
  const double d2 = std::exp(static_cast<double>(spread) * 6.9087547793152204);
  const float m = (static_cast<float>(d2 - 1.0) / 1000.0f) * 1.0f + 0.0f;
  float l74 = 0, l64 = 0;
  const int n2 = order * 2;
  lines.assign(static_cast<size_t>(n2), Line{});
  for (int i = 0; i < n2; ++i) {
    Line& L = lines[static_cast<size_t>(i)];
    float l3c;
    if (i < order) {
      l3c = static_cast<float>(i);
      L.chan = 0;
      L.phase0 = 0;
    } else {
      l3c = static_cast<float>(n2 - 1 - i);
      L.chan = 1;
      L.phase0 = INT32_MIN;
    }
    l3c = 1.0f - l3c / static_cast<float>(order);
    float l40 = kDetune[i % 20];
    l40 = static_cast<float>(static_cast<double>(1.0f - m) * 1.0 + static_cast<double>(m) * static_cast<double>(l40 * l40));
    L.gL = 1.0f;
    L.gR = cross * l3c;
    if (L.chan > 0) std::swap(L.gL, L.gR);
    l64 = std::fabs(L.gR) + std::fabs(L.gL) + l64;
    L.d0 = l78 * l3c + 10.0f;
    double l48 = l70 / static_cast<double>(10.0f + l78);
    const double l60 = (static_cast<double>(l40) * l70) / static_cast<double>(L.d0);
    l48 = static_cast<double>(1.0f - m) * l48 + static_cast<double>(m) * l60;
    const double d9 = static_cast<double>(1.0f - m) * static_cast<double>(l68) + static_cast<double>(m) * static_cast<double>(l68 * l3c * l40);
    L.base = static_cast<float>(d9);
    L.phase0 = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(rnd(static_cast<double>(l3c * m * 2147483648.0f * 0.00390625f))) << 8) +
                                    static_cast<uint32_t>(L.phase0));
    const float l58 = static_cast<float>(44100.0 / static_cast<double>(sr));
    L.inc = static_cast<int32_t>(rnd(static_cast<double>(l58) * l48));
    L.d0 = L.d0 / l58;
    float f7 = L.base / l58;
    if (f7 <= 8.0f) f7 = 8.0f;
    L.base = f7 + L.d0;
    f7 = L.base + L.d0;
    if (f7 < l74) f7 = l74;
    l74 = f7;
    const float f8 = std::sqrt(L.d0 * 2.0f * 2.0f);
    L.D = static_cast<float>(static_cast<double>(f8) * 4.6566128730773926e-10);
    L.F = f8;
    L.E = static_cast<float>(static_cast<double>(f8) * 0.5);
  }
  const double d3 = std::exp(static_cast<double>(1.0f - 1.0f / l64) * 6.9087547793152204);
  const float l38 = (1.0f - static_cast<float>(d3 - 1.0) / 1000.0f) * vol;
  for (auto& L : lines) L.gL *= l38, L.gR *= l38;
  delayFrames = static_cast<int>(rnd(static_cast<double>(l74))) + 8;
  frames = blockFrames + delayFrames;
  buf.assign(static_cast<size_t>(frames) * 2, 0.f);
  wr = delayFrames % frames;
  rd = wr;
  for (auto& L : lines) L.phase = L.phase0;
}

void Chorus::write(const float* fx, int n) {
  for (int i = 0; i < n; ++i) {
    buf[static_cast<size_t>(wr) * 2] = fx[2 * i];
    buf[static_cast<size_t>(wr) * 2 + 1] = fx[2 * i + 1];
    if (++wr >= frames) wr = 0;
  }
}

void Chorus::read(float* out, int n) {
  for (size_t li = 0; li < lines.size(); ++li) {
    Line& L = lines[li];
    const ld A = L.gL, B = L.gR, C = L.base, D = L.D;
    int pos = rd;
    for (int i = 0; i < n; ++i) {
      ld p = static_cast<ld>(L.phase) * D;
      p = std::fabs(p);
      L.phase = static_cast<int32_t>(static_cast<uint32_t>(L.phase) + static_cast<uint32_t>(L.inc));
      const ld t = p - static_cast<ld>(L.E);
      const ld g = static_cast<ld>(L.F) - std::fabs(t);
      const ld dl = t * g + C;
      const ld ri = std::trunc(dl);  // the line read runs with truncating rounding
      const int di = static_cast<int>(static_cast<long>(ri));
      const ld frac = dl - ri;
      int a = pos - di;
      while (a < 0) a += frames;
      int b = a - 1;
      if (b < 0) b += frames;
      const ld y0 = buf[static_cast<size_t>(a) * 2 + static_cast<size_t>(L.chan)], y1 = buf[static_cast<size_t>(b) * 2 + static_cast<size_t>(L.chan)];
      const ld y = y0 + (y1 - y0) * frac;
      if (li == 0) {
        out[2 * i] = static_cast<float>(y * A);
        out[2 * i + 1] = static_cast<float>(y * B);
      } else {
        out[2 * i] = static_cast<float>(y * A + static_cast<ld>(out[2 * i]));
        out[2 * i + 1] = static_cast<float>(y * B + static_cast<ld>(out[2 * i + 1]));
      }
      if (++pos >= frames) pos = 0;
    }
  }
  rd = (rd + n) % frames;
}

void Chorus::idle(int n) {
  for (auto& L : lines) L.phase = static_cast<int32_t>(static_cast<uint32_t>(L.phase) + static_cast<uint32_t>(n) * static_cast<uint32_t>(L.inc));
}

// ------------------------------------------------------------------ reverb
void Reverb::setup(const uint8_t raw[38], int sr, double tempo) {
  int32_t v[9];
  std::memcpy(v, raw, sizeof v);
  on = raw[36] != 0;
  if (!on) return;
  const bool sync = raw[37] != 0;
  const float lowcut = v[0] != 19 ? static_cast<float>(v[0]) : 0.f;
  const float highcut = v[1] != 221 ? static_cast<float>(v[1]) : 0.f;
  const float dampv = v[6] != 221 ? static_cast<float>(v[6]) : 0.f;
  wet = static_cast<float>(v[8]) * 0.00390625f;
  for (int k = 0; k < 12; ++k) line[k].assign(static_cast<size_t>(kRvLen[k]) * static_cast<size_t>(sr) / 20000 + 4, 0.f);
  line[12].assign(static_cast<size_t>(sr) * 2 + 4, 0.f);
  double pre = static_cast<double>(static_cast<long>(v[2]) * static_cast<long>(sr));
  pre = sync ? pre * 0.3125 / tempo : pre / 384.0;
  len[12] = std::max(1, std::min(static_cast<int>(line[12].size()), static_cast<int>(rnd(pre))));
  pos[12] = 0;
  const float srf = static_cast<float>(sr), inv = static_cast<float>(1.0 / static_cast<double>(sr));
  if (lowcut == 0.f) {
    hpC = 0.f;
  } else {
    const float f = (srf * 0.5f > lowcut) ? lowcut : srf * 0.5f;
    const float t = static_cast<float>(std::tan(static_cast<double>(inv) * (static_cast<double>(f) * kPi)));
    hpC = (t - 1.f) / (t + 1.f);
  }
  auto onePole = [&](float x) {
    if (x == 0.f) return 1.f;
    const float f = (srf * 0.5f > x) ? x : srf * 0.5f;
    const float c = static_cast<float>(2.0 - std::cos(static_cast<double>(inv) * (static_cast<double>(200.f * f) * kPi)));
    return 1.f - (c - std::sqrt(c * c - 1.f));
  };
  lpC = onePole(highcut);
  const int size = v[3];
  const long scale = static_cast<long>(sr) * static_cast<long>(size + 15) / 115;
  for (int k = 0; k < 12; ++k) {
    len[k] = std::max(1, static_cast<int>(static_cast<long>(kRvLen[k]) * scale / 20000));
    len[k] = std::min(len[k], static_cast<int>(line[k].size()));
    pos[k] = 0;
  }
  for (int k = 0; k < 7; ++k) {
    tapA[k] = static_cast<int>(static_cast<long>(kRvTapA[k]) * scale / 20000);
    tapB[k] = static_cast<int>(static_cast<long>(kRvTapB[k]) * scale / 20000);
  }
  const float D = static_cast<float>(static_cast<double>(v[4]) * 0.01);
  const float half = 0.5f * inv;
  auto dg = [&](int a, int b, double c) {
    return static_cast<float>(std::exp((static_cast<double>(static_cast<float>(static_cast<long>(len[a]) + static_cast<long>(len[b])) * half) * kLnMilli) /
                                       (static_cast<double>(D) * c)));
  };
  if (D == 0.f) {
    cIn = cAp1 = cAp2 = 0.f;
  } else {
    cIn = dg(1, 0, 0.07);
    cAp1 = dg(4, 8, 0.35);
    cAp2 = dg(6, 10, 0.51);
  }
  const long lsum = static_cast<long>(len[11]) + static_cast<long>(len[5]) + static_cast<long>(len[7]) + static_cast<long>(len[9]);
  const float T = static_cast<float>(static_cast<double>(static_cast<long>(v[5]) * static_cast<long>(sr)) / 10.0);
  g = static_cast<float>(pow4th(std::exp((static_cast<double>(lsum) * kLnMilli) / static_cast<double>(T))));
  const int col = v[7];
  float m = 1.f;
  if (col == 0) m = 0.2f;
  else if (col == 1) m = 0.6f;
  else if (col == 3) m = 1.5f;
  else if (col == 4) m = 3.f;
  float r = static_cast<float>(pow4th(std::exp((static_cast<double>(lsum) * kLnMilli) / static_cast<double>(T * m)))) / g;
  shH = (r - 1.f) * 0.5f;
  if (1.f < r) r = 1.f;
  const float t = static_cast<float>(std::tan(static_cast<double>(inv) * (static_cast<double>(500.f) * kPi)));
  shC = (t - r) / (t + r);
  dampC = onePole(dampv);
}

void Reverb::process(const float* in, float* out, int n) {
  const float w73 = 0.6f * (1.f - g) + g, w74 = 0.25f * (1.f - g) + g, w75 = 0.7f * (1.f - g) + g;
  const float w76 = 0.5f * (1.f - g) + g, w70 = (1.f - g) * 0.2f + g;
  const float tiny = 1.0000000031710769e-30f;
  auto adv = [&](int k) {
    int p = pos[k];
    if (p < 1) p = len[k];
    pos[k] = p - 1;
  };
  auto at = [&](int k, int idx) {
    if (idx >= len[k]) idx -= len[k];
    return line[k][static_cast<size_t>(idx)];
  };
  for (int i = 0; i < n; ++i) {
    float x = ((in[i] + tiny) - lp) * lpC + lp;
    lp = x;
    const float y = (x - hpS1) * hpC + hpS2;
    hpS1 = y;
    hpS2 = x;
    if (hpC != 0.f) x = (x - y) * 0.5f + tiny;
    for (int k = 0; k < 4; ++k) {
      float& b = line[k][static_cast<size_t>(pos[k])];
      const float vv = b - cIn * x;
      b = cIn * vv + x;
      adv(k);
      x = vv;
    }
    float& pb = line[12][static_cast<size_t>(pos[12])];
    const float a = pb;
    pb = x;
    adv(12);
    for (int tk = 0; tk < 2; ++tk) {
      const int L0 = tk ? 8 : 4;
      const float s = tk == 0 ? g * line[11][static_cast<size_t>(pos[11])] : g * line[7][static_cast<size_t>(pos[7])];
      const float z = (s - shOut[tk]) * shC + shIn[tk];
      shOut[tk] = z;
      shIn[tk] = s;
      const float vv = ((s + a + (z + s) * shH) - damp[tk]) * dampC + damp[tk];
      damp[tk] = vv;
      float& b4 = line[L0][static_cast<size_t>(pos[L0])];
      const float t1 = b4 - cAp1 * vv;
      b4 = cAp1 * t1 + vv;
      adv(L0);
      float& b5 = line[L0 + 1][static_cast<size_t>(pos[L0 + 1])];
      float o = b5;
      b5 = t1;
      adv(L0 + 1);
      o = o * g;
      float& b6 = line[L0 + 2][static_cast<size_t>(pos[L0 + 2])];
      const float t2 = b6 - cAp2 * o;
      b6 = cAp2 * t2 + o;
      adv(L0 + 2);
      line[L0 + 3][static_cast<size_t>(pos[L0 + 3])] = t2;
      adv(L0 + 3);
    }
    const float l5 = at(5, tapA[4] + pos[5]), l9a = at(9, tapA[0] + pos[9]), l11 = at(11, tapA[3] + pos[11]);
    const float l10 = at(10, tapA[2] + pos[10]), l9b = at(9, tapA[1] + pos[9]), l6 = at(6, tapA[5] + pos[6]);
    const float l7 = at(7, tapA[6] + pos[7]);
    out[2 * i] = ((l5 * w73 + l9a) - (l11 * w70 + l10 * w75 + l9b * w74)) + l6 + l7 * w76;
    const float r5b = at(5, tapB[1] + pos[5]), r6 = at(6, tapB[2] + pos[6]), r7 = at(7, tapB[3] + pos[7]);
    const float r9 = at(9, tapB[4] + pos[9]), r10 = at(10, tapB[5] + pos[10]), r5a = at(5, tapB[0] + pos[5]);
    const float r11 = at(11, tapB[6] + pos[11]);
    out[2 * i + 1] = (r5b * w74 + r5a + r10 + r11 * w76) - (r9 * w73 + r7 * w70 + r6 * w75);
  }
}

// ------------------------------------------------------------------ decimator
void Decimator::run(float* buf, int& n, int hi, int lo) {
  if (hi < lo) return;
  bool silent = quiet(buf, n * 2);
  for (int k = hi; silent && k >= lo; --k) silent = quiet(reinterpret_cast<const float*>(st[static_cast<size_t>(k)].s), 24);
  if (silent) {
    n >>= (hi - lo + 1);
    return;
  }
  for (int k = hi; k >= lo; --k) {
    n >>= 1;
    double(*s)[2] = st[static_cast<size_t>(k)].s;
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < 2; ++c) {
        const double xe = static_cast<double>(buf[4 * i + c]), xo = static_cast<double>(buf[4 * i + 2 + c]);
        double y;
        y = (xe - s[0][c]) * kHalfband[0] + s[6][c], s[6][c] = xe, s[0][c] = y;
        y = (xo - s[3][c]) * kHalfband[1] + s[9][c], s[9][c] = xo, s[3][c] = y;
        y = (s[0][c] - s[1][c]) * kHalfband[2] + s[7][c], s[7][c] = s[0][c], s[1][c] = y;
        y = (s[3][c] - s[4][c]) * kHalfband[3] + s[10][c], s[10][c] = s[3][c], s[4][c] = y;
        y = (s[1][c] - s[2][c]) * kHalfband[4] + s[8][c], s[8][c] = s[1][c], s[2][c] = y;
        y = (s[4][c] - s[5][c]) * kHalfband[5] + s[11][c], s[11][c] = s[4][c], s[5][c] = y;
        buf[2 * i + c] = static_cast<float>((s[5][c] + s[2][c]) * 0.5);
      }
    for (int r = 0; r < 6; ++r)
      for (double& x : st[static_cast<size_t>(k)].s[r])
        if (std::fabs(x) < static_cast<double>(5.96046448e-08f)) x = 0.0;
  }
}

// ------------------------------------------------------------------ kernels
void rampMulLanes(const float* src, float* dst, float start, float inc, int n) {
  const float inc4 = inc * 4.0f;
  static const float kL[4] = {0.0f, 0.25f, 0.5f, 0.75f};
  float lane[4];
  for (int j = 0; j < 4; ++j) lane[j] = start + kL[j] * inc4;
  const int body = (n >> 4) << 4;
  for (int i = 0; i < body; i += 4) {
    for (int j = 0; j < 4; ++j) dst[i + j] = src[i + j] * lane[j];
    for (int j = 0; j < 4; ++j) lane[j] += inc4;
  }
  float g = lane[0];
  for (int i = body; i < n; ++i) {
    dst[i] = src[i] * g;
    g += inc;
  }
}

void rampAdd(const float* src, float* dst, float start, float inc, int n) {
  ld g = start;
  for (int i = 0; i < n; ++i) {
    dst[i] = static_cast<float>(static_cast<ld>(src[i]) * g + static_cast<ld>(dst[i]));
    g += static_cast<ld>(inc);
  }
}

void mixSet(float gain, float inc, const float* src, float* dst, int n) {
  if (isZeroBits(inc)) {
    if (isZeroBits(gain)) {
      for (int i = 0; i < n; ++i) dst[i] = 0.f;
    } else if (gain == 1.0f) {
      if (src != dst) std::memcpy(dst, src, sizeof(float) * static_cast<size_t>(n));
    } else {
      for (int i = 0; i < n; ++i) dst[i] = src[i] * gain;
    }
  } else {
    rampMulLanes(src, dst, gain, inc, n);
  }
}

void mixAdd(float gain, float inc, const float* src, float* dst, int n) {
  if (isZeroBits(inc)) {
    if (isZeroBits(gain)) return;
    if (gain == 1.0f) {
      for (int i = 0; i < n; ++i) dst[i] = dst[i] + src[i];
    } else {
      for (int i = 0; i < n; ++i) dst[i] = dst[i] + src[i] * gain;
    }
  } else {
    rampAdd(src, dst, gain, inc, n);
  }
}

void svf(double* st, float f0, float q0, float finc, float qinc, bool ramp, const float* in, float* lo, float* bp, float* hp, int n) {
  ld low = st[0], band = st[2], f = f0, q = q0;
  for (int i = 0; i < n; ++i) {
    low = band * f + low;
    const ld x = in[i];
    lo[i] = static_cast<float>(low);
    const ld h = x - (band * q + low);
    hp[i] = static_cast<float>(h);
    band = h * f + band;
    bp[i] = static_cast<float>(band);
    if (ramp) {
      q += static_cast<ld>(qinc);
      f += static_cast<ld>(finc);
    }
  }
  st[2] = static_cast<double>(band);
  st[0] = static_cast<double>(low);
  flushTiny(st, 8);
}

void twoPole(double* st, float f0, float q0, float finc, float qinc, bool high, const float* in, float* out, int n) {
  ld y = st[0], v = st[2], f = f0, q = q0;
  for (int i = 0; i < n; ++i) {
    const ld x = in[i];
    v = (x - y) * f + v;
    y = y + v;
    v = v * q;
    out[i] = static_cast<float>(high ? x - y : y);
    f += static_cast<ld>(finc);
    q += static_cast<ld>(qinc);
  }
  auto flush = [](ld a) { return std::fabs(a) <= 5.96046448e-08L ? 0.0L : a; };
  st[2] = static_cast<double>(flush(v));
  st[0] = static_cast<double>(flush(y));
}

void ladder(float* z, double pd, float kf, double drive, float gain, const float* in, float* out, int n) {
  const ld p = pd, k = kf, d = drive, g = gain;
  ld s = z[4];
  for (int i = 0; i < n; ++i) {
    ld u = s * k + static_cast<ld>(in[i]);
    ld prev = 0;
    if (drive == 0.0) {
      for (int j = 0; j < 4; ++j) {
        const ld zj = z[j];
        const ld zn = (u - zj) * p + zj;
        z[j] = static_cast<float>(zn);
        u = zn;
        prev = zj;
      }
    } else {
      u = ladderSat(u * d);
      for (int j = 0; j < 4; ++j) {
        const ld zj = z[j];
        const ld zn = (u - ladderSat(zj * d)) * p + zj;
        z[j] = static_cast<float>(zn);
        prev = zj;
        u = j < 3 ? ladderSat(zn * d) : zn;
      }
    }
    s = prev + u;
    out[i] = static_cast<float>(g * s);
  }
  z[4] = static_cast<float>(s);
  for (int j = 0; j < 5; ++j)
    if (std::fabs(z[j]) < 5.96046448e-08f) z[j] = 0.f;
}

void biquadMono(double* st, const Biquad& q, const float* in, float* out, int n) {
  double x1 = st[0], x2 = st[2], y1 = st[4], y2 = st[6];
  for (int i = 0; i < n; ++i) {
    double t = x1 * q.b1;
    t = t - y2 * q.a2;
    t = t + x2 * q.b2;
    t = t - y1 * q.a1;
    x2 = x1, y2 = y1;
    const double x = static_cast<double>(in[i]);
    x1 = x;
    const double y = x * q.b0 + t;
    y1 = y;
    out[i] = static_cast<float>(y);
  }
  st[2] = x2, st[0] = x1, st[6] = y2, st[4] = y1;
  flushTiny(st, 8);
}

void allpassChain(float* s, int count, float a, float fb, float& yState, float* buf, int n) {
  const double A = a, F = fb;
  double y = yState;
  for (int i = 0; i < n; ++i) {
    double x = y * F + static_cast<double>(buf[i]);
    for (int j = 0; j < 2 * count; ++j) {
      const double t = x * -A + static_cast<double>(s[j]);
      s[j] = static_cast<float>(t * A + x);
      x = t;
    }
    y = x;
    buf[i] = static_cast<float>(y);
  }
  yState = static_cast<float>(y);
  for (int j = 0; j < 2 * count; ++j)
    if (std::fabs(s[j]) < 5.96046448e-08f) s[j] = 0.f;
}

void shaper(const float* in, float* out, int n, const int16_t* tbl, int L, float inGain, bool bip, bool interp) {
  const ld S = static_cast<ld>(1.0f * 3.0517578125e-05f);
  const ld b = 0.0L;
  if (!bip) {
    const ld K = static_cast<ld>(inGain) * static_cast<ld>(L);
    for (int i = 0; i < n; ++i) {
      const ld x = in[i];
      const ld y = x * K;
      ld v;
      if (!interp) {
        const long idx = static_cast<long>(std::nearbyint(y));
        if (idx >= 0) v = tbl[std::min<long>(idx, L)];
        else v = -static_cast<ld>(tbl[std::min<long>(-idx, L)]);
      } else {
        const ld ti = std::trunc(y);
        const ld fr = y - ti;
        long idx = static_cast<long>(ti);
        if (idx >= 0) {
          idx = std::min<long>(idx, L);
          const ld t0 = tbl[idx], t1 = tbl[idx + 1];
          v = (t1 - t0) * fr + t0;
        } else if (idx == static_cast<long>(INT32_MIN)) {
          v = 0;
        } else {
          idx = std::min<long>(-idx, L);
          const ld t0 = tbl[idx], t1 = tbl[idx + 1];
          v = -(t0 - (t1 - t0) * fr);
        }
      }
      out[i] = static_cast<float>(x * b + v * S);
    }
  } else {
    const ld H = static_cast<ld>(L) * 0.5L;
    const ld K = static_cast<ld>(static_cast<float>(static_cast<ld>(inGain) * static_cast<ld>(L))) * 0.5L;
    for (int i = 0; i < n; ++i) {
      const ld x = in[i];
      const ld y = x * K + H;
      ld v;
      if (!interp) {
        long idx = static_cast<long>(std::nearbyint(y));
        if (idx < 0) idx = 0;
        if (idx > L) idx = L;
        v = tbl[idx];
      } else {
        const ld ti = std::trunc(y);
        const ld fr = y - ti;
        long idx = static_cast<long>(ti);
        if (idx < -1) idx = -1;
        if (idx > L) idx = L;
        const ld t0 = tbl[idx], t1 = tbl[idx + 1];
        v = (t1 - t0) * fr + t0;
      }
      out[i] = static_cast<float>(x * b + v * S);
    }
  }
}

void upsample2(float* buf, int n) {
  for (int i = n - 1; i >= 0; --i) {
    const float x = buf[i];
    buf[2 * i + 1] = x;
    buf[2 * i] = x;
  }
}

void downsample2(float* buf, int n) {
  for (int i = 0; i < n; ++i) buf[i] = static_cast<float>((static_cast<ld>(buf[2 * i]) + static_cast<ld>(buf[2 * i + 1])) * 0.5L);
}

double powerD(double b, double e) {
  if (e == 0.0) return 1.0;
  if (b == 0.0 && e > 0.0) return 0.0;
  if (std::fabs(e) <= 2147483647.0 && std::trunc(e) == e) {
    long k = static_cast<long>(e);
    ld r = 1.0L, x = b;
    const bool neg = k < 0;
    if (neg) k = -k;
    while (k) {
      if (k & 1) r *= x;
      x *= x;
      k >>= 1;
    }
    return static_cast<double>(neg ? 1.0L / r : r);
  }
  return std::exp(std::log(b) * e);
}

float powerF(float b, float e) {
  if (e == 0.0f) return 1.0f;
  if (b == 0.0f && e > 0.0f) return 0.0f;
  if (std::trunc(e) == e) return static_cast<float>(powerD(static_cast<double>(b), static_cast<double>(static_cast<int>(std::nearbyint(e)))));
  return static_cast<float>(std::exp(std::log(static_cast<double>(b)) * static_cast<double>(e)));
}

}  // namespace gl::packs::synths::mfmref::fx

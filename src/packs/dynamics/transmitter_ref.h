// Gridline — J-45 reference engine for Transient Split (gl.transplit, model
// "Reference"): the transient/sustain splitter of the FL Studio effect it
// imports, rebuilt from its measured behaviour (clean-room spec in the J-45
// notes): an 8-band onset detector driving a hold + 4-pole envelope, a
// look-ahead delay split into transient (x·e) and sustain (x·(1−e)) paths,
// a one-pole tilt per path, one effect per path (saturation, bounce taps,
// diffusion all-passes, blur = none, delay, off), smoothed levels.
// Everything runs in float32 like the reference. Buffers are sized for one
// sample rate when the engine is built (UI thread, see TransplitFx).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gl::packs::dynamics::txref {

inline float exp10f_(float x) {
#if defined(__APPLE__)
  return __exp10f(x);
#elif defined(__GLIBC__)
  return ::exp10f(x);
#else
  return std::pow(10.0f, x);
#endif
}
inline size_t pow2At(double n) {
  size_t s = 1;
  while (static_cast<double>(s) < n) s <<= 1;
  return s;
}

// ------------------------------------------------------------------ detector
struct Svf {
  float g = 0, gk = 0, a = 0, a2 = 0, g2 = 0, s1 = 0, s2 = 0;
  void set(float w) {
    g = std::tan(w);
    gk = g + 1.4142135f;
    const float D = gk * g + 1.0f;
    a = g / D, a2 = a + a, g2 = g + g;
  }
  float tick(float u, float& bp) {
    const float e = u - (gk * s1 + s2);
    const float b = a * e + s1;
    s1 = a2 * e + s1;
    const float l = s2 + g * b;
    s2 = g2 * b + s2;
    bp = b;
    return l;
  }
};
struct Lr {  // LR4 split: returns low, writes high
  Svf a, b;
  float tick(float u, float& hi) {
    float bp1, bp2;
    const float l1 = a.tick(u, bp1), l2 = b.tick(l1, bp2);
    hi = (u - 2.828427f * bp1) - l2;
    return l2;
  }
};

struct Detector {
  float sr = 44100, kap = 0, c20 = 0, srq = 0, G = 0, G2 = 0;
  Lr sp[7];
  float st[8][4] = {}, lnprev[8] = {}, P = 0, mu[4] = {};
  int cnt = 0;
  float laS = 0, hS = 0, rS = 0;
  int hc[8] = {}, hv[8] = {};
  float env[4] = {};
  float mode = 1, sens = 0.75f, la = 0.002f, hold = 0.05f, rel = 0.05f;
  void init(float rate) {
    *this = Detector{};
    sr = std::clamp(rate, 1.0f, 192000.0f);
    const float inv = 1.0f / sr;
    static const float w[7] = {2010.6193f, 355.43063f, 149.44017f, 845.36127f, 11373.78f, 4782.0854f, 27051.56f};
    for (int i = 0; i < 7; ++i) sp[i].a.set(inv * w[i]), sp[i].b = sp[i].a;
    kap = inv * 14.444787f;
    c20 = std::exp(inv * -125.663704f);
    srq = sr * 0.125f;
    const float t = std::tan(inv * 62.831852f);
    G = t / (t + 1.0f), G2 = G + G;
  }
  float smooth(float& s, float tgt) const {
    const float e = tgt - s, v = G * e + s;
    s = G2 * e + s;
    return v;
  }
  void run(int n, const float* x, float* eT) {
    const float eps = 1.1920929e-07f;
    const float m = mode;
    const float retrig = m >= 2 ? 0.005f : (m >= 1 ? 0.02f : 0.05f), tau0 = m >= 2 ? 0.04f : (m >= 1 ? 0.05f : 0.1f);
    const float retrigN = retrig * sr - 1.0f;
    float cr[8];
    for (int k = 0; k < 8; ++k) cr[k] = std::exp(-kap / std::max(2 * tau0 - 0.01f * static_cast<float>(k), eps));
    const float om = 1.0f - sens, th1 = std::exp(om * om * 4.0f) - 1.0f;
    for (int i = 0; i < n; ++i) {
      const float u = x[i];
      float b[8], h;
      const float lo = sp[0].tick(u, h);
      const float hi = h;
      const float lo2 = sp[1].tick(lo, h);
      const float lohi = h;
      b[0] = sp[2].tick(lo2, h), b[1] = h;
      b[2] = sp[3].tick(lohi, h), b[3] = h;
      const float hi2 = sp[4].tick(hi, h);
      const float hihi = h;
      b[4] = sp[5].tick(hi2, h), b[5] = h;
      b[6] = sp[6].tick(hihi, h), b[7] = h;
      float dk[8];
      for (int k = 0; k < 8; ++k) {
        const float p = b[k] * b[k];
        const float q = p / std::max(st[k][3], eps);
        const float L = std::max(std::log(std::max(q, eps)), 1.0f);
        const float T = std::max(tau0 / (L * L), eps);
        const float ca = std::exp(-kap / T);
        float y = p;
        for (int j = 0; j < 4; ++j) {
          const float c = st[k][j] < y ? ca : cr[k];
          y = c * (st[k][j] - y) + y;
          st[k][j] = y;
        }
        const float lv = std::log(std::max(y, eps));
        const float dd = lv - lnprev[k];
        lnprev[k] = lv;
        dk[k] = dd > 0 ? dd : 0;
      }
      const float dsum = ((((((dk[7] + dk[3]) + dk[5]) + dk[1]) + dk[6]) + dk[2]) + dk[4]) + dk[0];
      P = u * u * (1.0f - c20) + P * c20;
      const float gg = std::max(P < 1e-6f ? 0.0f : 1.0f, eps);
      const float tm = std::max(3.0f / gg, eps), cm = std::exp(-kap / tm);
      float y = dsum;
      for (int j = 0; j < 4; ++j) {
        y = y * (1.0f - cm) + mu[j] * cm;
        mu[j] = y;
      }
      const int trig = (retrigN <= static_cast<float>(cnt)) && (th1 < (y * -7.0f + dsum) * 10.0f);
      cnt = trig ? 0 : cnt + 1;
      const float las = smooth(laS, la), hs = smooth(hS, hold);
      const float N = std::rint((hs + las) * srq);
      int v = trig;
      for (int j = 0; j < 8; ++j) {
        const bool held = !(N <= static_cast<float>(hc[j]));
        const bool keep = held && v < hv[j];
        hc[j] = keep ? hc[j] + 1 : 0;
        if (keep) v = hv[j];
        hv[j] = v;
      }
      const float rs = smooth(rS, rel);
      const float crel = std::exp(-kap / std::max(rs, eps)), catt = std::exp(-kap / std::max(las, eps));
      float e = static_cast<float>(v);
      for (int j = 0; j < 4; ++j) {
        const float c = env[j] < e ? catt : crel;
        e = (env[j] - e) * c + e;
        env[j] = e;
      }
      eT[i] = e;
    }
  }
};

// ------------------------------------------------------------------ diffusion
struct Diffusion {
  static constexpr int kSt = 16;
  struct Stage {
    std::vector<float> buf;
    uint32_t mask = 0;
    float latched = 0, dA = 0, dB = 0, D = 0, y = 0, sg = 0;
    int cnt = 0, cntPrev = 0;
  };
  float sr = 44100, k44 = 0, base[kSt] = {}, unit[kSt] = {};
  float gs = 0, xf = 0, step = 0;
  uint32_t w = 0;
  int32_t rng = 0;
  Stage st[2][kSt];
  float del = 0.25f, g = 0.5f, mix = 0;
  static constexpr float kT[kSt] = {0.270205f, 0.1976862f, 0.14463031f, 0.1058138f, 0.07741503f, 0.056638043f, 0.04143728f, 0.030316161f,
                                    0.022179777f, 0.016227072f, 0.011871979f, 0.008685726f, 0.0063546123f, 0.004649134f, 0.0034013789f, 0.002488502f};
  void init(float rate) {
    sr = std::clamp(rate, 1.0f, 192000.0f);
    k44 = 44.1f / sr;
    for (int k = 0; k < kSt; ++k) {
      base[k] = sr * kT[k];
      unit[k] = sr * (kT[k] * 4.656613e-10f);
      const size_t sz = pow2At(static_cast<double>(sr) * kT[k] * 10.0 + 4810.0);
      for (auto& ch : st) ch[k].buf.assign(sz, 0.f), ch[k].mask = static_cast<uint32_t>(sz - 1);
    }
  }
  void run(int n, float* L, float* R) {
    float d = std::clamp(del, 0.01f, 40.0f);
    if (del >= 40) d = 40;
    const float depth = std::max(d * -0.025f + 0.05f, 0.0f);
    float b[kSt];
    for (int k = 0; k < kSt; ++k) b[k] = base[k] * d;
    float* io[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      gs = gs * (1.0f - k44) + g * k44;
      float inc = std::clamp(gs * -5.0f + 1.0f, 0.0f, 1.0f);
      inc = 1.0f / (inc * 112.0f + 16.0f);
      const float stp = xf == 0.0f ? inc : (xf == 1.0f ? -inc : step);
      const float x2 = std::clamp(xf + stp, 0.0f, 1.0f);
      const float c = std::sqrt(1.0f - gs * gs);
      rng = static_cast<int32_t>(static_cast<uint32_t>(rng) * 1103515245u + 12345u);
      const float nz = static_cast<float>(rng) * depth * d;
      float cand[kSt];
      for (int k = 0; k < kSt; ++k) cand[k] = std::clamp(nz * unit[k], -4800.0f, 4800.0f) + b[k];
      for (int ch = 0; ch < 2; ++ch) {
        const float x = io[ch][i];
        float in = x;
        for (int k = 0; k < kSt; ++k) {
          Stage& s = st[ch][k];
          s.buf[w & s.mask] = in * c - s.D * gs;
          const float lat = (s.cntPrev <= s.cnt) ? s.latched : cand[k];
          s.latched = lat;
          const float t = lat - 1.0f;
          const float dB = (x2 != 1.0f) ? s.dB : t, dA = (x2 != 0.0f) ? s.dA : t;
          s.dB = dB, s.dA = dA;
          const float rb = std::min(dB > 0 ? dB : 0.0f, 1.92e6f), ra = std::min(dA > 0 ? dA : 0.0f, 1.92e6f);
          const float Dn = s.buf[(w - static_cast<uint32_t>(static_cast<int>(ra))) & s.mask] * x2 +
                           s.buf[(w - static_cast<uint32_t>(static_cast<int>(rb))) & s.mask] * (1.0f - x2);
          const float y = in * gs + s.D * c;
          const float sg = s.y != 0.0f ? s.y : s.sg;
          int nc = s.cnt + 1;
          if (sg * s.sg < 0) nc = 0;
          if (static_cast<float>(s.cnt) < b[k]) nc = s.cnt + 1;
          s.cntPrev = s.cnt, s.cnt = nc;
          s.sg = sg, s.y = y, s.D = Dn;
          in = y;
        }
        io[ch][i] = x * (1.0f - mix) + in * mix;
      }
      xf = x2, step = stp, ++w;
    }
  }
};

// ------------------------------------------------------------------ bounce
struct Bounce {
  static constexpr int kTaps = 65;
  float sr = 44100, k44 = 0, step20 = 0;
  std::vector<float> buf[2];
  uint32_t mask = 0, w = 0;
  float mixS = 0, ampS = 0, xf = 0, stp = 0;
  float posFac[kTaps] = {}, lastA = 0;
  bool have = false;
  float dA[kTaps] = {}, dB[kTaps] = {}, R[kTaps] = {};
  float amp = 0, dcurve = 0, mix = 0;
  void init(float rate) {
    sr = std::clamp(rate, 1.0f, 192000.0f);
    k44 = (1.0f / sr) * 44.1f, step20 = (1.0f / sr) * 20.0f;
    const size_t sz = pow2At(2.0 * sr + 4.0);
    for (auto& b : buf) b.assign(sz, 0.f);
    mask = static_cast<uint32_t>(sz - 1);
    for (int i = 0; i < kTaps; ++i) R[i] = static_cast<float>((std::exp(10.0 * (i / 64.0)) - 1.0) / (std::exp(10.0) - 1.0));
  }
  void run(int n, float* L, float* Rr) {
    float a = dcurve * -10.0f;
    if (a <= 0) {
      if (!(a < -1e-6f)) a = -1e-6f;
    } else if (!(a > 1e-6f)) {
      a = 1e-6f;
    }
    const float inv = 1.0f / (1.0f - std::exp(a));
    float fac[kTaps];
    for (int i = 0; i < kTaps; ++i) {
      const float e = std::exp(a * static_cast<float>(i / 64.0));
      fac[i] = (2.0f - (e + e)) * inv;
    }
    fac[64] = 2.0f;
    float* io[2] = {L, Rr};
    for (int s = 0; s < n; ++s) {
      for (int c = 0; c < 2; ++c) buf[c][w & mask] = io[c][s];
      mixS = mixS * (1.0f - k44) + mix * k44;
      ampS = (1.0f - k44) * ampS + k44 * amp;
      const float st = xf == 0.0f ? step20 : (xf == 1.0f ? -step20 : stp);
      const float x2 = std::clamp(xf + st, 0.0f, 1.0f);
      xf = x2, stp = st;
      const bool keep = have && lastA == a;
      for (int i = 0; i < kTaps; ++i) {
        if (!keep) posFac[i] = fac[i];
        const float d = posFac[i] * sr;
        if (x2 == 1.0f) dB[i] = d;
        if (x2 == 0.0f) dA[i] = d;
      }
      have = true, lastA = a;
      const float aa = std::fabs(ampS);
      for (int c = 0; c < 2; ++c) {
        float acc = 0;
        for (int i = c == 0 ? 1 : 2; i < kTaps; i += 2) {
          const float K = ampS > 0 ? R[i] : R[64 - i];
          const float g = ampS <= 0 ? aa * K + 1.0f + ampS : aa * K + (1.0f - ampS);
          const float ra = std::clamp(dA[i], 0.0f, 1.92e6f), rb = std::clamp(dB[i], 0.0f, 1.92e6f);
          const float v = buf[c][(w - static_cast<uint32_t>(static_cast<int>(ra))) & mask] * x2 +
                          buf[c][(w - static_cast<uint32_t>(static_cast<int>(rb))) & mask] * (1.0f - x2);
          acc += v * g;
        }
        io[c][s] = acc * mixS + io[c][s] * (1.0f - mixS);
      }
      ++w;
    }
  }
};

// ------------------------------------------------------------------ saturation
struct Saturation {
  float k = 0, pis = 0, f882 = 0, mixS = 0, wS = 0, fc = 0, gS = 0;
  float ap[2] = {}, drv[2] = {}, l1[2] = {}, l2[2] = {};
  float drive = 0, mix = 0, warm = 0;
  void init(float rate) {
    *this = Saturation{};
    const float sr = std::clamp(rate, 1.0f, 192000.0f), inv = 1.0f / sr;
    k = inv * 44.1f, pis = inv * 3.1415927f, f882 = inv * 882.0f;
  }
  static float knee(float y, float t) {
    const float r = 1.0f - t;
    if (std::fabs(y) > t) y = (y > t) ? std::tanh((y - t) / r) * r + t : std::tanh((y + t) / r) * r - t;
    return y;
  }
  void run(int n, float* L, float* R) {
    const float om = 1.0f - k;
    const float gT = exp10f_(drive * 0.05f) * k, wT = std::sqrt(warm) * k, mT = mix * k;
    float* io[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      mixS = mixS * om + mT;
      wS = wS * om + wT;
      const float w1 = 1.0f - wS;
      fc = std::exp2(w1 * 10.0f) * f882 + fc * om;
      const float G = std::tan(fc * pis), a = 1.0f / (G + 1.0f);
      gS = gS * om + gT;
      const float t = w1 * 0.9f;
      for (int c = 0; c < 2; ++c) {
        const float x = io[c][i];
        const float v = (x - ap[c]) * G * a;
        const float dry = (((v + ap[c]) + v) + ap[c]) - x;
        ap[c] = v + v + ap[c];
        const float u = gS * x;
        const float v2 = (u - drv[c]) * G * a;
        const float apu = (v2 + drv[c] + v2 + drv[c]) - u;
        drv[c] = v2 + v2 + drv[c];
        const float ue = u >= 0 ? (u > 1.1920929e-07f ? u : 1.1920929e-07f) : (u < -1.1920929e-07f ? u : -1.1920929e-07f);
        const float sh = std::tanh(ue) / ue;
        const float v3 = (sh - l1[c]) * G * a, n1 = v3 + v3 + l1[c];
        const float v4 = ((l1[c] - l2[c]) + v3) * G * a, n2 = v4 + v4 + l2[c];
        float y = (v4 + l2[c]) * apu;
        l1[c] = n1, l2[c] = n2;
        y = knee(y, t);
        io[c][i] = c == 0 ? y * mixS + dry * (1.0f - mixS) : dry * (1.0f - mixS) + y * mixS;
      }
    }
  }
};

// ------------------------------------------------------------------ delay
struct Delay {
  float a = 0, a2 = 0, dcK = 0, dcR = 0, pole = 0, gain = 0;
  int maxD = 0;
  std::vector<float> buf[2];
  uint32_t mask = 0, w = 0;
  float vprev[2] = {}, hp[2] = {}, lps[2] = {}, y[2] = {}, D = 0;
  float mix = 0, fbp = 0, ms = 0;
  void init(float rate) {
    const float sr = std::clamp(rate, 1.0f, 192000.0f);
    const float t = std::tan((1.0f / sr) * 31490.523f), wv = 62.831852f / sr;
    a = t / (t + 1.0f), a2 = a + a;
    dcK = 1.0f / (wv + 1.0f), dcR = 1.0f - wv;
    maxD = static_cast<int>(sr + sr) + 1;
    pole = std::exp((1.0f / sr) * -4.0f), gain = (1.0f - pole) * sr * 0.001f;
    const size_t sz = pow2At(maxD + 4.0);
    for (auto& b : buf) b.assign(sz, 0.f);
    mask = static_cast<uint32_t>(sz - 1);
  }
  void run(int n, float* L, float* R) {
    const float fb = fbp * 0.01f, tg = ms * gain;
    float* io[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      D = D * pole + tg;
      const int iD = static_cast<int>(D);
      const int i0 = std::min(iD > 0 ? iD : 0, maxD), i1 = std::min(iD < 0 ? 0 : iD + 1, maxD);
      const float fl = std::floor(D), fr = D - fl, fw = (1.0f - D) + fl;
      for (int c = 0; c < 2; ++c) {
        const float x = io[c][i];
        const float v = y[c] * fb + x;
        const float h = (hp[c] * dcR + (v - vprev[c])) * dcK;
        vprev[c] = v, hp[c] = h;
        const float e = h - lps[c];
        buf[c][w & mask] = e * a + lps[c];
        lps[c] = e * a2 + lps[c];
        const float yy = buf[c][(w - static_cast<uint32_t>(i1)) & mask] * fr + buf[c][(w - static_cast<uint32_t>(i0)) & mask] * fw;
        y[c] = yy;
        io[c][i] = yy * mix + x * (1.0f - mix);
      }
      ++w;
    }
  }
};

struct Tilt {
  float s[2] = {};
  void run(float sr, float gdb, int n, float* L, float* R) {
    const float g = exp10f_(gdb * 0.05f);
    float fc = 1000.0f * g;
    if (sr * 0.4995f <= fc) fc = sr * 0.4995f;
    const float tt = std::tan(fc * (3.1415927f / sr)), a = tt / (tt + 1.0f);
    float* io[2] = {L, R};
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < 2; ++c) {
        const float x = io[c][i], e = x - s[c], lp = e * a + s[c];
        s[c] = e * (a + a) + s[c];
        io[c][i] = (x - lp) * g + lp * (1.0f / g);
      }
  }
};

// Stored control values (the 42 tags, as the state keeps them) → engine units.
inline void engineValues(const float* raw, float* v) {
  auto u = [&](int t, double lo = 0, double hi = 10000) { return std::clamp((raw[t] - lo) / (hi - lo), 0.0, 1.0); };
  auto C = [](double x, double lo, double hi, double c) { return lo + (hi - lo) * std::pow(x, 1.0 / c); };
  auto f = [](double x) { return static_cast<float>(x); };
  v[0] = f(u(0));
  v[1] = f(C(u(1), 1, 250, 1)), v[2] = f(C(u(2), 1, 250, 1));
  v[3] = f(C(u(3, 0, 12800), -120, 12, 4)), v[4] = f(C(u(4, 0, 12800), -120, 12, 4));
  v[5] = f(C(u(5), -6, 6, 1)), v[6] = f(C(u(6), -6, 6, 1));
  for (int k = 0; k < 2; ++k) {
    v[7 + 3 * k] = f(u(7 + 3 * k)), v[8 + 3 * k] = f(0.99 * u(8 + 3 * k)), v[9 + 3 * k] = f(std::pow(10.0, 2 * u(9 + 3 * k) - 1));
    v[13 + 3 * k] = f(u(13 + 3 * k)), v[14 + 3 * k] = f(18 * u(14 + 3 * k)), v[15 + 3 * k] = f(u(15 + 3 * k));
    v[19 + 3 * k] = f(u(19 + 3 * k)), v[20 + 3 * k] = f(-1 + 2 * u(20 + 3 * k)), v[21 + 3 * k] = f(-1 + 2 * u(21 + 3 * k));
    const int b = 25 + 4 * k;
    v[b] = f(u(b)), v[b + 1] = f(0.001 + 1.999 * u(b + 1) * u(b + 1)), v[b + 2] = f(16 * u(b + 2, 1, 768)), v[b + 3] = f(u(b + 3));
  }
  for (int t = 33; t < 42; ++t) v[t] = raw[t];
}

// The whole effect for one sample rate. Blocks of at most kBlock frames.
struct Engine {
  static constexpr int kBlock = 1024;
  float sr = 44100, srRaw = 44100;
  Detector det[3];
  std::vector<float> ring[2];
  uint32_t rw[2] = {};
  float laSt[2] = {}, ea = 0, ea2 = 0;
  Tilt tilt[2];
  Saturation sat[2];
  Bounce bnc[2];
  Diffusion dif[2];
  Delay dly[2];
  float sm[4] = {1, 1, 0, 0}, pole = 0;
  float eT[2][kBlock] = {}, T[2][kBlock] = {}, S[2][kBlock] = {}, mono[kBlock] = {};
  explicit Engine(float rate) : srRaw(rate) {
    sr = std::clamp(rate, 1.0f, 192000.0f);
    for (auto& d : det) d.init(sr);
    for (auto& r : ring) r.assign(65536, 0.f);
    const float t = std::tan(62.831852f / sr);
    ea = t / (t + 1.0f), ea2 = ea + ea;
    for (int k = 0; k < 2; ++k) sat[k].init(sr), bnc[k].init(sr), dif[k].init(sr), dly[k].init(sr);
    const double wd = static_cast<double>(1.0f / rate) * 125.663706144;
    const float wf = static_cast<float>(wd);
    pole = 1.0f / (wf * wf * 0.5f + wf + 1.0f);
  }
  void fx(const float* p, int k, int n, float bpm) {
    float* L = T[0];
    float* R = T[1];
    if (k) L = S[0], R = S[1];
    switch (static_cast<int>(p[38 + k])) {
      case 0: sat[k].drive = p[14 + 3 * k], sat[k].mix = p[13 + 3 * k], sat[k].warm = p[15 + 3 * k], sat[k].run(n, L, R); break;
      case 1: bnc[k].amp = p[20 + 3 * k] * 0.95f, bnc[k].dcurve = p[21 + 3 * k], bnc[k].mix = p[19 + 3 * k], bnc[k].run(n, L, R); break;
      case 2: dif[k].del = p[9 + 3 * k], dif[k].g = p[8 + 3 * k], dif[k].mix = p[7 + 3 * k], dif[k].run(n, L, R); break;
      case 4: {
        const int b = 25 + 4 * k;
        dly[k].ms = p[40 + k] > 0.5f ? (p[b + 2] * 15000.0f) / bpm : p[b + 1] * 1000.0f;
        dly[k].mix = p[b], dly[k].fbp = p[b + 3] * 100.0f;
        dly[k].run(n, L, R);
        break;
      }
      default: break;  // blur and off: no processing
    }
  }
  // p: engine units (engineValues). sc: detection source (null = the input).
  // sendT / sendS (may be null): post-effect transient / sustain added in.
  void run(const float* p, float bpm, int n, float* L, float* R, const float* scL, const float* scR, float* tL, float* tR, float* sL,
           float* sR) {
    const float la = p[33] > 0.5f ? 0.002f : 0.0f;
    for (auto& d : det) d.mode = std::round(p[35]), d.sens = p[0], d.la = la, d.hold = p[1] * 0.001f, d.rel = p[2] * 0.001f;
    const float* dl = scL ? scL : L;
    const float* dr = scR ? scR : R;
    if (p[34] > 0.5f) {
      for (int i = 0; i < n; ++i) mono[i] = (dr[i] + dl[i]) * 0.5f;
      det[2].run(n, mono, eT[0]);
      std::memcpy(eT[1], eT[0], sizeof(float) * static_cast<size_t>(n));
    } else {
      det[0].run(n, dl, eT[0]);
      det[1].run(n, dr, eT[1]);
    }
    float* xs[2] = {L, R};
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < n; ++i) {
        ring[c][rw[c] & 0xffff] = xs[c][i];
        const float er = la - laSt[c], v = (er * ea + laSt[c]) * sr;
        laSt[c] = er * ea2 + laSt[c];
        float r = std::trunc(v + std::copysign(0.49999997f, v));
        r = std::clamp(r, 0.0f, 48000.0f);
        const float y = ring[c][(rw[c] - static_cast<uint32_t>(static_cast<int>(r))) & 0xffff];
        T[c][i] = eT[c][i] * y;
        S[c][i] = y * (1.0f - eT[c][i]);
        ++rw[c];
      }
    tilt[0].run(sr, p[5], n, T[0], T[1]);
    tilt[1].run(sr, p[6], n, S[0], S[1]);
    fx(p, 0, n, bpm);
    fx(p, 1, n, bpm);
    const float gT = std::exp(p[3] * 0.115129255f), gS = std::exp(p[4] * 0.115129255f);
    const float tg[4] = {p[36], p[37], gT, gS};
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < 4; ++j) sm[j] = pole * (sm[j] - tg[j]) + tg[j];
      const float ft = sm[2] * sm[0], fs = sm[3] * sm[1];
      L[i] = T[0][i] * ft + S[0][i] * fs;
      R[i] = S[1][i] * fs + T[1][i] * ft;
      if (tL && tR) tL[i] += T[0][i], tR[i] += T[1][i];
      if (sL && sR) sL[i] += S[0][i], sR[i] += S[1][i];
    }
  }
};

}  // namespace gl::packs::dynamics::txref

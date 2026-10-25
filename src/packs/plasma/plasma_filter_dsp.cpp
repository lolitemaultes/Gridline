// Gridline — "plasma" pack: the filter family used by the per-voice filters
// and the Filter effect (107 types). Plasma's own designs, grouped by engine:
// state-variable cascades (normal / multi-mode morphs / EQ), ZDF ladders
// (warm / ladder / diode / dirty), delay-line combs and flangers, allpass
// phasers, and the special types (ring mod, sample & hold, clusters, small
// room, screamers, formants, DJ mixer, pole-zero, exponential slopes).
#include <algorithm>
#include <cmath>
#include <cstring>
#include "plasma_dsp.h"

namespace gl::packs::plasma {

namespace {
enum Engine {
  EngSvf, EngLadder, EngComb, EngFlange, EngPhaser, EngEq, EngRing, EngSnh, EngCombs, EngAllpass, EngRoom, EngScream,
  EngBass, EngFormant, EngReject, EngDistComb, EngDj, EngPz, EngExp
};
int engineOf(int t) {
  if (t <= 3 || t == 84 || (t >= 99 && t <= 102)) return EngLadder;
  if (t <= 38) return EngSvf;
  if (t <= 46 || t == 104) return EngComb;
  if (t <= 54) return EngFlange;
  if (t <= 70) return EngPhaser;
  if (t <= 75) return EngEq;
  if (t <= 77) return EngRing;
  if (t <= 79) return EngSnh;
  if (t == 80) return EngCombs;
  if (t == 81 || t == 98) return EngAllpass;
  if (t == 82) return EngRoom;
  if (t == 83 || t == 94 || t == 95) return EngScream;
  if (t == 85) return EngBass;
  if ((t >= 86 && t <= 88) || t == 96) return EngFormant;
  if (t == 89) return EngReject;
  if (t >= 90 && t <= 93) return EngDistComb;
  if (t == 97) return EngDj;
  if (t == 103) return EngPz;
  return EngExp;
}

// TPT state-variable stage
struct SvfC {
  float g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
  void set(double sr, double hz, double kk) {
    g = static_cast<float>(std::tan(3.14159265358979 * std::clamp(hz, 5.0, sr * 0.49) / sr));
    k = static_cast<float>(kk);
    a1 = 1.f / (1.f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
};
struct SvfS {
  float ic1 = 0, ic2 = 0;
  void run(const SvfC& c, float v0, float& lp, float& bp, float& hp) {
    const float v3 = v0 - ic2;
    const float v1 = c.a1 * ic1 + c.a2 * v3;
    const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
    ic1 = 2 * v1 - ic1;
    ic2 = 2 * v2 - ic2;
    bp = v1;
    lp = v2;
    hp = v0 - c.k * v1 - v2;
  }
};
// TPT one-pole
struct OpC {
  float G = 0;
  void set(double sr, double hz) {
    const double g = std::tan(3.14159265358979 * std::clamp(hz, 5.0, sr * 0.49) / sr);
    G = static_cast<float>(g / (1 + g));
  }
};
struct OpS {
  float s = 0;
  float lp(const OpC& c, float x) {
    const float v = (x - s) * c.G;
    const float y = v + s;
    s = y + v;
    return y;
  }
};
// biquad (RBJ) for EQ types
struct Bq {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
struct BqS {
  float z1 = 0, z2 = 0;
  float run(const Bq& c, float x) {
    const float y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
  }
};
Bq rbj(int kind, double sr, double hz, double q, double db) {  // 0 low shelf, 1 bell, 2 high shelf, 3 bandpass
  const double A = std::pow(10.0, db / 40.0), w = kPi2 * std::clamp(hz, 10.0, sr * 0.49) / sr;
  const double cw = std::cos(w), sw = std::sin(w), al = sw / (2 * std::max(0.1, q));
  double b0, b1, b2, a0, a1, a2;
  if (kind == 1) {
    b0 = 1 + al * A, b1 = -2 * cw, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * cw, a2 = 1 - al / A;
  } else if (kind == 3) {
    b0 = al, b1 = 0, b2 = -al, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
  } else {
    const double sq = 2 * std::sqrt(A) * al;
    if (kind == 0) {
      b0 = A * ((A + 1) - (A - 1) * cw + sq), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - sq);
      a0 = (A + 1) + (A - 1) * cw + sq, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - sq;
    } else {
      b0 = A * ((A + 1) + (A - 1) * cw + sq), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - sq);
      a0 = (A + 1) - (A - 1) * cw + sq, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - sq;
    }
  }
  Bq c;
  c.b0 = static_cast<float>(b0 / a0), c.b1 = static_cast<float>(b1 / a0), c.b2 = static_cast<float>(b2 / a0);
  c.a1 = static_cast<float>(a1 / a0), c.a2 = static_cast<float>(a2 / a0);
  return c;
}
}  // namespace

struct Filter::Ch {
  // coefficients
  SvfC s1, s2, s3, f[3];
  OpC op, op2, damp;
  Bq bq;
  float g = 0, G = 0, k = 0, drv = 1, mix = 0, fb = 0, var = 0, outGain = 1;
  float delay = 100, delay2 = 100;     // samples
  float apA = 0;                        // first-order allpass coefficient
  float apSpread = 0;
  int stages = 4, poles = 4;
  float rmInc = 0, rmInc2 = 0, snhInc = 0;
  float fGain[3] = {1, 1, 1};
  int lens[8] = {};
  // state (cleared by clear())
  SvfS ss1, ss2, ss3, fs[3];
  OpS o1, o2, o3, o4, od, oe[8];
  BqS b1, b2;
  float lad[4] = {};
  float ap[16] = {};
  float fbState = 0;
  float rmPh = 0, rmPh2 = 0, snhPh = 0, snhHold = 0;
  int w = 0;
  int wi[8] = {};
  float* buf = nullptr;  // kBuf samples
  void clear() {
    ss1 = ss2 = ss3 = SvfS{};
    for (auto& v : fs) v = SvfS{};
    o1 = o2 = o3 = o4 = od = OpS{};
    for (auto& v : oe) v = OpS{};
    b1 = b2 = BqS{};
    for (float& v : lad) v = 0;
    for (float& v : ap) v = 0;
    fbState = rmPh = rmPh2 = snhPh = snhHold = 0;
    w = 0;
    for (int& v : wi) v = 0;
    std::fill(buf, buf + kBuf, 0.f);
  }
  float tapFrac(float d) const {
    d = std::clamp(d, 1.f, static_cast<float>(kBuf - 2));
    const int i = static_cast<int>(d);
    const float fr = d - static_cast<float>(i);
    const float a = buf[(w - i) & (kBuf - 1)], b = buf[(w - i - 1) & (kBuf - 1)];
    return a + (b - a) * fr;
  }
  void push(float x) {
    w = (w + 1) & (kBuf - 1);
    buf[w] = x;
  }
};

Filter::Filter() : ch_(new Ch[2]), mem_(new float[2 * kBuf]) {
  std::fill(mem_.get(), mem_.get() + 2 * kBuf, 0.f);
  ch_[0].buf = mem_.get();
  ch_[1].buf = mem_.get() + kBuf;
}
Filter::~Filter() = default;

void Filter::reset() {
  ch_[0].clear();
  ch_[1].clear();
}

void Filter::set(int type, double sr, double hz, double res, double drive, double var, double spreadSt) {
  type = std::clamp(type, 0, 106);
  if (type != type_) {
    type_ = type;
    reset();
  }
  sr_ = sr;
  res = std::clamp(res, 0.0, 1.0);
  drive = std::clamp(drive, 0.0, 1.0);
  var = std::clamp(var, 0.0, 1.0);
  const int eng = engineOf(type);
  for (int c = 0; c < 2; ++c) {
    Ch& h = ch_[c];
    const double f = std::clamp(hz * std::pow(2.0, (c == 0 ? -spreadSt : spreadSt) / 12.0), 8.0, sr * 0.49);
    h.drv = static_cast<float>(1.0 + drive * 9.0);
    h.var = static_cast<float>(var);
    h.outGain = 1;
    switch (eng) {
      case EngSvf: {
        const double kk = 2.0 * (1.0 - res * 0.985);
        h.s1.set(sr, f, kk);
        h.s2.set(sr, f, type == 13 || type == 15 || type == 17 || type == 32 || type == 34 || type == 36 || type == 38 ? kk : 1.4142);
        h.op.set(sr, f);
        h.mix = static_cast<float>(std::pow(10.0, (var - 0.5) * 36.0 / 20.0) - 1.0);  // peak gain (types 14 / 15)
        if (type == 28) h.s3.set(sr, f * (1 + 3 * var), kk);
        if (type == 30) h.s3.set(sr, f * (1 + 3 * var), 1.0);
        break;
      }
      case EngLadder: {
        const double g = std::tan(3.14159265358979 * f / sr);
        h.g = static_cast<float>(g);
        h.G = static_cast<float>(g / (1 + g));
        h.k = static_cast<float>(res * (type == 100 ? 4.4 : 4.05));
        h.poles = type <= 3 ? type + 1 : (type == 100 ? 3 : 4);
        break;
      }
      case EngComb:
      case EngDistComb: {
        h.delay = static_cast<float>(sr / f);
        if (type == 104) h.delay2 = static_cast<float>(sr / (f * 1.4983));
        h.fb = static_cast<float>(res * 0.97);
        const double cut = 200.0 * std::pow(100.0, var);  // damping filter in the loop
        h.damp.set(sr, (type == 43 || type == 44) ? 20 + 4000 * var : cut);
        if (eng == EngDistComb) h.damp.set(sr, (type == 90 || type == 92) ? 300 + 12000 * (1 - var) : 1000 + 3000 * var);
        break;
      }
      case EngFlange: {
        const double base = sr / f;
        h.delay = static_cast<float>(base * (c == 0 ? 1.0 : 1.0 + var * 0.5));
        h.fb = static_cast<float>(res * 0.95);
        h.damp.set(sr, (type == 51 || type == 52) ? 300 : 3000);
        break;
      }
      case EngPhaser: {
        const int base = (type - 55) / 2;  // 0 12, 1 24, 2 36, 3..6 48 variants, 7 flange-phase
        h.stages = base == 0 ? 4 : base == 1 ? 8 : base == 2 ? 12 : 16;
        if (type >= 69) h.stages = 8;
        const double w = std::tan(3.14159265358979 * f / sr);
        h.apA = static_cast<float>((w - 1) / (w + 1));
        h.apSpread = static_cast<float>(var);
        h.fb = static_cast<float>(res * 0.9);
        h.op.set(sr, f * 4);   // L6 / H6 variants
        h.delay = static_cast<float>(sr / f * 0.5);
        break;
      }
      case EngEq: {
        const double db = (var - 0.5) * 36.0;
        const double q = 0.5 + res * 6.0;
        if (type == 71 || type == 74) {  // 6 dB shelves: one-pole blend
          h.op.set(sr, f);
          h.mix = static_cast<float>(std::pow(10.0, db / 20.0));
        } else {
          h.bq = rbj(type == 72 ? 0 : type == 73 ? 1 : 2, sr, f, q, db);
        }
        break;
      }
      case EngRing:
        h.rmInc = static_cast<float>(f / sr);
        h.rmInc2 = static_cast<float>(f * (1.0 + 0.5 * var + (c ? 0.01 * var : 0)) * 1.5 / sr);
        h.s1.set(sr, std::min(sr * 0.45, f * 8), 2.0 * (1.0 - res * 0.9));
        break;
      case EngSnh:
        h.snhInc = static_cast<float>(f * 2 / sr);
        h.op.set(sr, 20 + 20000 * std::pow(1 - var, 2));
        h.s1.set(sr, std::min(sr * 0.45, f * 2), 2.0 * (1.0 - res * 0.95));
        break;
      case EngCombs:
      case EngAllpass:
      case EngRoom: {
        static const double r[8] = {1.0, 1.31, 1.73, 2.09, 2.47, 2.91, 3.37, 3.83};
        const double base = std::min(static_cast<double>(kBuf / 8 - 4), sr / f);
        for (int i = 0; i < 8; ++i) {
          const double sp = 1.0 + (r[i] - 1.0) * (eng == EngRoom ? (0.5 + var) : (0.2 + var * 1.6));
          h.lens[i] = std::clamp(static_cast<int>(base * sp * (c ? 1.07 : 1.0)), 2, kBuf / 8 - 2);
        }
        h.fb = static_cast<float>(eng == EngRoom ? 0.5 + res * 0.47 : res * 0.9);
        h.damp.set(sr, 1500 + 15000 * (1 - res * 0.5));
        break;
      }
      case EngScream: {
        const double kk = 2.0 * (1.0 - res * 0.9);
        h.s1.set(sr, f, std::max(0.05, kk));
        h.op.set(sr, 300 + 12000 * var);
        h.drv = static_cast<float>(1.0 + drive * 20.0 + res * 4.0);
        break;
      }
      case EngBass:
        h.s1.set(sr, f, 2.0 * (1.0 - res * 0.985));
        h.bq = rbj(0, sr, 80 + f * 0.1, 0.7, var * 18.0);
        break;
      case EngFormant: {
        static const double V[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
        const double pos = var * 4.0;
        const int a = std::min(3, static_cast<int>(pos));
        const double t = pos - a;
        const double shift = f / 425.0 * (type == 87 ? 1.3 : type == 88 ? 0.8 : 1.0);
        for (int i = 0; i < 3; ++i) {
          const double fr = (V[a][i] + (V[a + 1][i] - V[a][i]) * t) * shift;
          h.f[i].set(sr, std::clamp(fr, 50.0, sr * 0.45), 1.0 / (3.0 + res * 25.0));
          h.fGain[i] = i == 0 ? 1.f : i == 1 ? 0.7f : 0.45f;
        }
        h.op.set(sr, 6000);
        break;
      }
      case EngReject:
        h.s1.set(sr, f / (1 + var * 2), 2.0 * (1.0 - res * 0.9));
        h.s2.set(sr, f * (1 + var * 2), 2.0 * (1.0 - res * 0.9));
        break;
      case EngDj: {
        // cutoff below the middle = low-pass sweeping down, above = high-pass sweeping up
        const double n = cutoffNorm(hz);
        const double kk = 2.0 * (1.0 - res * 0.9);
        if (n < 0.5) {
          h.mix = 0;
          h.s1.set(sr, 20000.0 * std::pow(20.0 / 20000.0, (0.5 - n) * 2.0), kk);
        } else {
          h.mix = 1;
          h.s1.set(sr, 10.0 * std::pow(2000.0, (n - 0.5) * 2.0), kk);
        }
        h.s2 = h.s1;
        break;
      }
      case EngPz:
        h.s1.set(sr, f, 2.0 * (1.0 - res * 0.985));
        h.s2.set(sr, std::clamp(f * std::pow(4.0, var * 2 - 1), 8.0, sr * 0.49), 2.0);
        break;
      default: {  // exponential slopes: cascaded one-poles
        h.stages = 1 + static_cast<int>(std::lround(var * 7));
        h.op.set(sr, f);
        h.op2.set(sr, f * 0.5);
        h.fb = static_cast<float>(res * 0.6);
        break;
      }
    }
  }
}

void Filter::process(float& l, float& r) {
  const int t = type_;
  if (t < 0) return;
  const int eng = engineOf(t);
  float* io[2] = {&l, &r};
  for (int c = 0; c < 2; ++c) {
    Ch& h = ch_[c];
    float x = *io[c];
    float y = x;
    if (h.drv > 1.0001f && eng != EngScream && eng != EngLadder) x = fastTanh(x * h.drv) / std::sqrt(h.drv);
    switch (eng) {
      case EngSvf: {
        float lp, bp, hp, lp2, bp2, hp2;
        h.ss1.run(h.s1, x, lp, bp, hp);
        const float k1 = h.s1.k;
        const float notch = lp + hp, peak = lp - hp, band = bp * k1;
        auto second = [&](float v) {
          h.ss2.run(h.s2, v, lp2, bp2, hp2);
        };
        const float m = h.var;
        switch (t) {
          case 4: y = h.o1.lp(h.op, x); break;
          case 5: y = lp; break;
          case 6: y = h.o1.lp(h.op, lp); break;
          case 7: second(lp); y = lp2; break;
          case 8: y = x - h.o1.lp(h.op, x); break;
          case 9: y = hp; break;
          case 10: { const float a = h.o1.lp(h.op, hp); y = hp - a; break; }
          case 11: second(hp); y = hp2; break;
          case 12: y = band; break;
          case 13: second(band); y = bp2 * h.s2.k; break;
          case 14: y = x + band * h.mix; break;
          case 15: {
            const float v = x + band * h.mix;
            second(v);
            y = v + bp2 * h.s2.k * h.mix;
            break;
          }
          case 16: y = notch; break;
          case 17: second(notch); y = lp2 + hp2; break;
          case 18: { const float a = h.o1.lp(h.op, x); y = a + (x - a - a) * m; break; }
          case 19: y = lp + (hp - lp) * m; break;
          case 20: y = lp + (band - lp) * m; break;
          case 21: y = lp + (peak - lp) * m; break;
          case 22: y = lp + (notch - lp) * m; break;
          case 23: y = hp + (band - hp) * m; break;
          case 24: y = hp + (peak - hp) * m; break;
          case 25: y = hp + (notch - hp) * m; break;
          case 26: y = band + (peak - band) * m; break;
          case 27: y = band + (notch - band) * m; break;
          case 28: {
            float l3, b3, h3;
            h.ss3.run(h.s3, x, l3, b3, h3);
            y = peak * (1 - m) + (l3 - h3) * m;
            break;
          }
          case 29: y = peak + (notch - peak) * m; break;
          case 30: {
            float l3, b3, h3;
            h.ss3.run(h.s3, x, l3, b3, h3);
            y = notch * (1 - m) + (l3 + h3) * m;
            break;
          }
          default: {  // three-way morphs (31..38): A -> B -> C over var
            float A, B, C;
            switch (t) {
              case 31: case 32: A = lp, B = band, C = hp; break;
              case 33: case 34: A = lp, B = peak, C = hp; break;
              case 35: case 36: A = lp, B = notch, C = hp; break;
              default: A = band, B = peak, C = notch; break;
            }
            const float v = m < 0.5f ? A + (B - A) * (m * 2) : B + (C - B) * (m * 2 - 1);
            if (t == 32 || t == 34 || t == 36 || t == 38) {
              second(v);
              y = (m < 0.34f ? lp2 : m < 0.67f ? bp2 * h.s2.k : hp2) * 0.6f + v * 0.4f;
            } else {
              y = v;
            }
            break;
          }
        }
        break;
      }
      case EngLadder: {
        // ZDF ladder with a saturating input
        const float G = h.G, b = 1.f / (1.f + h.g);
        float S = 0;
        {
          const float g1 = G, g2 = G * G, g3 = g2 * G;
          S = (g3 * h.lad[0] + g2 * h.lad[1] + g1 * h.lad[2] + h.lad[3]) * b;
        }
        const float G4 = G * G * G * G;
        float u = (x * h.drv - h.k * S) / (1.f + h.k * G4);
        if (t == 102) u = fastTanh(u * 1.6f + 0.15f) - 0.15f;  // dirty: asymmetric
        else u = fastTanh(u);
        float in = u, out[4];
        for (int s = 0; s < 4; ++s) {
          const float v = (in - h.lad[s]) * G;
          const float o = v + h.lad[s];
          h.lad[s] = o + v;
          out[s] = o;
          in = (t == 84) ? fastTanh(o * 1.2f) : o;  // diode-style stage coupling
        }
        const int p = std::clamp(h.poles, 1, 4);
        y = out[p - 1];
        if (t == 101) y = out[3] * 0.6f + out[1] * 0.4f;            // bright: mixes an earlier tap
        y *= 1.f + h.k * 0.5f * h.var;                             // BODY / BASS: resonance gain compensation
        y /= h.drv > 1 ? std::sqrt(h.drv) : 1.f;
        break;
      }
      case EngComb: {
        float d = h.tapFrac(h.delay);
        if (t == 41 || t == 42) d = h.o1.lp(h.damp, d);
        else if (t == 43 || t == 44) d = d - h.o1.lp(h.damp, d);
        else if (t == 45 || t == 46) d = h.o2.lp(h.damp, d - h.o1.lp(h.op2, d));
        const float sgn = (t & 1) ? 1.f : -1.f;  // odd indices are the "+" types
        float in = x + d * h.fb * sgn;
        if (t == 104) in += h.tapFrac(h.delay2) * h.fb * 0.5f;
        h.push(std::clamp(in, -4.f, 4.f));
        y = (x + d * sgn) * 0.5f;
        if (t == 104) y = in * 0.6f;
        break;
      }
      case EngDistComb: {
        float d = h.tapFrac(h.delay);
        d = (t == 90 || t == 92) ? h.o1.lp(h.damp, d) : h.o1.lp(h.damp, d) - h.o2.lp(h.op2, d);
        const float in = fastTanh((x + d * h.fb) * (t >= 92 ? 2.5f : 1.5f));
        h.push(in);
        y = in * 0.7f;
        break;
      }
      case EngFlange: {
        float d = h.tapFrac(h.delay);
        if (t == 49 || t == 50) d = h.o1.lp(h.damp, d);
        else if (t == 51 || t == 52) d = d - h.o1.lp(h.damp, d);
        else if (t == 53 || t == 54) d = h.o2.lp(h.damp, d) - h.o1.lp(h.op2, d);
        const float sgn = (t & 1) ? 1.f : -1.f;
        h.push(std::clamp(x + d * h.fb * sgn, -4.f, 4.f));
        y = (x + d * sgn) * 0.5f;
        break;
      }
      case EngPhaser: {
        float in = x + h.fbState * h.fb * ((t & 1) ? 1.f : -1.f);
        const int n = h.stages;
        for (int s = 0; s < n; ++s) {
          // spread: stage coefficients fan out around the centre
          const float a = std::clamp(h.apA + (static_cast<float>(s) / n - 0.5f) * h.apSpread * 0.6f, -0.995f, 0.995f);
          const float o = a * in + h.ap[s];
          h.ap[s] = in - a * o;
          in = o;
        }
        h.fbState = in;
        if (t >= 69) {  // flange-phase: allpasses plus a short comb
          const float d = h.tapFrac(h.delay);
          h.push(x);
          in = in * 0.6f + d * 0.4f;
        }
        y = (x + in) * 0.5f;
        if (t == 63 || t == 64) y = h.o1.lp(h.op, y);
        else if (t == 65 || t == 66) y = y - h.o1.lp(h.op, y);
        else if (t == 67 || t == 68) y = h.o2.lp(h.op, y - h.o1.lp(h.op2, y));
        break;
      }
      case EngEq:
        if (t == 71) {
          const float a = h.o1.lp(h.op, x);
          y = x - a + a * h.mix;
        } else if (t == 74) {
          const float a = h.o1.lp(h.op, x);
          y = a + (x - a) * h.mix;
        } else {
          y = h.b1.run(h.bq, x);
        }
        break;
      case EngRing: {
        h.rmPh += h.rmInc;
        h.rmPh -= std::floor(h.rmPh);
        float m = std::sin(static_cast<float>(kPi2) * h.rmPh);
        if (t == 77) {
          h.rmPh2 += h.rmInc2;
          h.rmPh2 -= std::floor(h.rmPh2);
          m = m * 0.6f + std::sin(static_cast<float>(kPi2) * h.rmPh2) * 0.4f;
        }
        float lp, bp, hp;
        h.ss1.run(h.s1, x * m, lp, bp, hp);
        y = lp;
        break;
      }
      case EngSnh: {
        h.snhPh += h.snhInc;
        if (h.snhPh >= 1) {
          h.snhPh -= std::floor(h.snhPh);
          h.snhHold = x;
        }
        float v = h.snhHold;
        if (t == 79) v = h.o1.lp(h.op, v);
        float lp, bp, hp;
        h.ss1.run(h.s1, v, lp, bp, hp);
        y = (t == 79) ? lp : v * 0.7f + lp * 0.3f;
        break;
      }
      case EngCombs: {
        float acc = 0;
        const int seg = kBuf / 8;
        for (int i = 0; i < 4; ++i) {
          float* b = h.buf + i * seg;
          const int L = h.lens[i];
          int& wi = h.wi[i];
          wi = (wi + 1) % seg;
          const float d = b[(wi - L + seg) % seg];
          b[wi] = std::clamp(x + d * h.fb, -4.f, 4.f);
          acc += d;
        }
        y = (x + acc * 0.35f) * 0.6f;
        break;
      }
      case EngAllpass: {
        const int seg = kBuf / 8;
        float v = x;
        const int n = t == 98 ? 6 : 4;
        for (int i = 0; i < n; ++i) {
          float* b = h.buf + i * seg;
          const int L = h.lens[i];
          const int wi = h.w;
          const float d = b[(wi - L + seg) % seg];
          const float g = t == 98 ? 0.6f : h.fb;
          const float o = d - g * v;
          b[wi] = v + g * o;
          v = o;
        }
        h.w = (h.w + 1) % (kBuf / 8);
        y = t == 98 ? x * (1 - h.var) + v * h.var : (x + v) * 0.5f;
        break;
      }
      case EngRoom: {  // 4-line feedback delay network
        const int seg = kBuf / 8;
        float d[4];
        for (int i = 0; i < 4; ++i) d[i] = h.buf[i * seg + (h.w - h.lens[i] + seg) % seg];
        const float s = (d[0] + d[1] + d[2] + d[3]) * 0.5f;  // Householder
        for (int i = 0; i < 4; ++i) {
          const float v = (d[i] - s) * h.fb;
          h.buf[i * seg + h.w] = x * 0.5f + h.oe[i].lp(h.damp, v);
        }
        h.w = (h.w + 1) % seg;
        y = x * 0.6f + (d[0] - d[1] + d[2] - d[3]) * 0.45f;
        break;
      }
      case EngScream: {
        float lp, bp, hp;
        h.ss1.run(h.s1, fastTanh((x + h.fbState * 0.5f) * h.drv), lp, bp, hp);
        h.fbState = fastTanh(bp * 1.2f);
        y = t == 94 ? lp : bp;
        y = h.o1.lp(h.op, y) * 0.8f;
        break;
      }
      case EngBass: {
        float lp, bp, hp;
        h.ss1.run(h.s1, x, lp, bp, hp);
        y = h.b1.run(h.bq, lp);
        break;
      }
      case EngFormant: {
        float acc = 0;
        for (int i = 0; i < 3; ++i) {
          float lp, bp, hp;
          h.fs[i].run(h.f[i], x, lp, bp, hp);
          acc += bp * h.fGain[i] * h.f[i].k * 3.f;
        }
        if (t == 96) acc = acc * 0.7f + (x - h.o1.lp(h.op, x)) * 0.3f;  // whisper: breathy top
        y = acc;
        break;
      }
      case EngReject: {
        float lp, bp, hp, lp2, bp2, hp2;
        h.ss1.run(h.s1, x, lp, bp, hp);
        const float n1 = lp + hp;
        h.ss2.run(h.s2, n1, lp2, bp2, hp2);
        y = lp2 + hp2;
        break;
      }
      case EngDj: {
        float lp, bp, hp, lp2, bp2, hp2;
        h.ss1.run(h.s1, x, lp, bp, hp);
        h.ss2.run(h.s2, h.mix > 0.5f ? hp : lp, lp2, bp2, hp2);
        y = h.mix > 0.5f ? hp2 : lp2;
        break;
      }
      case EngPz: {
        float lp, bp, hp, lp2, bp2, hp2;
        h.ss1.run(h.s1, x, lp, bp, hp);
        h.ss2.run(h.s2, lp, lp2, bp2, hp2);
        y = lp + (lp - lp2) * (h.var * 2 - 1);  // zero moves around the pole
        break;
      }
      default: {  // exponential low / band
        float v = x + h.fbState * h.fb;
        for (int s = 0; s < h.stages && s < 8; ++s) v = h.oe[s].lp(h.op, v);
        h.fbState = v;
        y = t == 106 ? v - h.od.lp(h.op2, v) : v;
        break;
      }
    }
    if (!std::isfinite(y)) {
      reset();
      y = 0;
    }
    *io[c] = y;
  }
}

void Filter::responseCurve(int type, double sr, double cutoffHz, double res, double var, const float* hz, float* out, int n) {
  // Display helper: impulse response (4096 samples) -> spectrum, read at each frequency.
  constexpr int N = 4096;
  Filter f;
  f.set(type, sr, cutoffHz, res, 0.0, var, 0);
  std::vector<cpx> x(N);
  for (int i = 0; i < N; ++i) {
    float l = i == 0 ? 0.05f : 0.f, r = l;
    f.process(l, r);
    x[static_cast<size_t>(i)] = cpx(l / 0.05f, 0.f);
  }
  Fft fft;
  fft.init(N);
  fft.forward(x.data());
  for (int k = 0; k < n; ++k) {
    const double b = std::clamp(static_cast<double>(hz[k]) / sr * N, 0.0, N / 2.0 - 1.001);
    const int i = static_cast<int>(b);
    const double fr = b - i;
    out[k] = static_cast<float>(std::abs(x[static_cast<size_t>(i)]) * (1 - fr) + std::abs(x[static_cast<size_t>(i + 1)]) * fr);
  }
}

}  // namespace gl::packs::plasma

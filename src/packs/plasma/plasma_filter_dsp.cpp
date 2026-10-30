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

// measured formant tables (plasma_filter_formant_dsp.cpp): 5 x {centre Hz, bandwidth Hz, gain}
void formantMeasured(int type, double pos, double var, double res, double* out);

namespace {
// matched reverb memory: eight combs then four allpasses inside the kBuf samples of a channel
constexpr int kRvComb = Filter::kBuf / 32 * 3, kRvAp = Filter::kBuf / 16;
enum Engine {
  EngSvf, EngLadder, EngComb, EngFlange, EngPhaser, EngEq, EngRing, EngSnh, EngCombs, EngAllpass, EngRoom, EngScream,
  EngBass, EngFormant, EngReject, EngDistComb, EngDj, EngPz, EngExp,
  // matched voicing (imports, "Boost" drive mode): designs fitted to measured responses
  EngMSvf, EngMLad, EngMX, EngMPass, EngMRing, EngMDiff, EngMComb, EngMPhaser, EngMAllp, EngMCombs, EngMComb2, EngMScream, EngMEq, EngMReverb, EngMDj, EngMForm, EngMBass
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

// ---------------------------------------------------------------- matched voicing (J-203b)
// Laws fitted to white-noise transfer measurements of the import source (1/6-octave bands,
// cutoffs 88 Hz .. 10 kHz, resonance 0..100 %). Plasma's own designs, not copies.
int engineMatched(int t) {
  if (t >= 4 && t <= 38) return EngMSvf;
  if (t == 84 || (t >= 99 && t <= 102)) return EngMLad;
  if (t == 89 || t == 96 || t == 105 || t == 106) return EngMX;
  if (t == 103) return EngMPass;  // the pole-zero type measures as a straight wire at every setting
  if (t == 76) return EngMRing;
  if (t >= 39 && t <= 54) return EngMComb;  // combs and flangers (the same loop, measured mixes)
  if (t == 69 || t == 70) return EngMComb;   // flange-phase: the HL6 flanger with two allpasses in the loop
  if (t >= 55 && t <= 68) return EngMPhaser;
  if (t == 98) return EngMDiff;  // diffuser: no dry mix (the source's response stays flat at every VAR)
  if (t == 81) return EngMAllp;
  if (t == 80) return EngMCombs;
  if (t == 82) return EngMReverb;
  if (t == 97) return EngMDj;
  if (t == 85) return EngMBass;
  if (t >= 86 && t <= 88) return EngMForm;
  if (t == 104) return EngMComb2;
  if (t == 83 || t == 94 || t == 95) return EngMScream;
  if (t >= 71 && t <= 75) return EngMEq;
  return engineOf(t);
}
double interp(const double* R, const double* K, int n, double r) {
  r = std::clamp(r, R[0], R[n - 1]);
  int i = 0;
  while (i < n - 2 && r > R[i + 1]) ++i;
  return K[i] + (K[i + 1] - K[i]) * (r - R[i]) / (R[i + 1] - R[i]);
}
// Other state-variable designs (matched): damping against resonance per family.
//   Exp / ExpBPF (105 / 106): a gentle family, k from 3.2 at 0 % to almost 0 at 100 %
//   Wsp (96): k from 0.81 to almost 0
double mxK(int t, double r) {
  static const double R[] = {0, 0.1, 0.2, 0.3, 0.45, 0.6, 0.75, 0.9, 1.0};
  static const double E[] = {3.21, 2.51, 2.03, 1.66, 1.25, 0.93, 0.6, 0.28, 0.006};
  static const double W[] = {0.81, 0.64, 0.535, 0.42, 0.337, 0.24, 0.15, 0.075, 0.003};
  return interp(R, t == 96 ? W : E, 9, r);
}
// Band reject: a low-pass and a high-pass (state-variable, the normal damping) summed, their
// cutoffs VAR octaves apart around the cutoff (half-gap 0 / 1.4 / 2.7 / 4.0 / 5.0 octaves at
// VAR 0 / 25 / 50 / 75 / 100 %); at 0 % this is a notch, resonance fills it with two peaks.
double rejectHalfGap(double v) {
  static const double V[] = {0, 0.25, 0.5, 0.75, 1.0};
  static const double A[] = {0, 1.4, 2.69, 4.0, 5.0};
  return interp(V, A, 5, v);
}
// Matched ladders: a 4-pole cascade of equal one-poles (1 / ((1 + s)^4 + k)) or a diode-style
// coupled ladder (each stage loads the next: 1 / (s^4 + 7 s^3 + 15 s^2 + 10 s + 1 + k)), with the
// measured feedback amount k against resonance, cutoff scale, level and resonance make-up.
struct MLadDesc {
  bool diode;
  double fcMul;  // effective cutoff / nominal
  int kTable;    // 0 Mg, 1 A, 2 acid, 3 EMS, 4 dirty
  bool makeup;   // output x (1 + k): the passband keeps its level as the resonance rises
  double gainDb;
};
const MLadDesc& mlad(int t) {
  static const MLadDesc mg{false, 1.01, 0, true, -1.95}, za{false, 1.017, 1, false, 0.0},
      acid{true, 0.87, 2, true, -0.8}, ems{true, 0.87, 3, true, 1.0}, dirty{false, 0.845, 4, false, -1.0};
  return t == 84 ? za : t == 100 ? acid : t == 101 ? ems : t == 102 ? dirty : mg;
}
double mladK(int table, double r) {
  static const double R[] = {0, 0.05, 0.1, 0.15, 0.2, 0.3, 0.45, 0.6, 0.75, 0.9, 1.0};
  static const double K[5][11] = {
      {0, 0.18, 0.38, 0.57, 0.76, 1.15, 1.76, 2.33, 2.97, 3.54, 4.0},          // Mg: 3.95 r
      {0, 0.24, 0.46, 0.68, 1.0, 1.33, 2.12, 2.65, 3.4, 3.992, 4.6},          // A: about 4.45 r, oscillates at 100 %
      {0.09, 0.98, 1.83, 2.71, 3.45, 5.2, 7.77, 10.3, 12.8, 16.0, 17.8},       // acid (diode; limit 18.4)
      {-0.5, 0.43, 1.3, 2.2, 3.28, 5.0, 7.85, 10.2, 12.4, 15.9, 17.7},         // EMS (diode)
      {0.02, 0.02, 0.03, 0.046, 0.058, 0.19, 0.69, 1.66, 3.25, 5.2, 5.6},      // dirty: oscillates from 90 %
  };
  return interp(R, K[std::clamp(table, 0, 4)], 11, r);
}
// State-variable damping (k = 1/Q) against resonance: measured table, log-interpolated.
double svfDamping(double r) {
  static const double R[] = {0, 0.01, 0.025, 0.05, 0.075, 0.1, 0.125, 0.15, 0.175, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45,
                             0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95, 0.975, 0.99, 1.0};
  static const double K[] = {2.0, 1.387, 1.221, 1.067, 0.961, 0.882, 0.815, 0.754, 0.704, 0.66, 0.58, 0.515, 0.445, 0.395, 0.34,
                             0.3, 0.255, 0.225, 0.188, 0.16, 0.13, 0.1, 0.074, 0.046, 0.022, 0.0115, 0.0045, 0.0015};
  constexpr int n = static_cast<int>(sizeof(R) / sizeof(R[0]));
  r = std::clamp(r, 0.0, 1.0);
  int i = 0;
  while (i < n - 2 && r > R[i + 1]) ++i;
  const double t = (r - R[i]) / (R[i + 1] - R[i]);
  return std::exp(std::log(K[i]) + (std::log(K[i + 1]) - std::log(K[i])) * t);
}
// Matched state-variable family: per type the first section's response, an optional second
// section (24 dB types: same response with a gentler damping; dual types: another response at
// a cutoff the VAR knob moves +-5 octaves), or a three-way morph.
struct MSvfDesc {
  char a, b, c;  // responses: L low, H high, B band (unnormalised), P peak, N notch, l / h one-pole
  int form;      // 0 single, 1 single + one-pole (18 dB), 2 doubled (24 dB), 3 dual, 4 morph, 5 doubled morph
  bool level;    // resonance lowers the level (normal types)
};
const MSvfDesc& msvf(int t) {
  static const MSvfDesc d[] = {
      {'l', 0, 0, 0, false}, {'L', 0, 0, 0, true},   {'L', 'l', 0, 1, true},  {'L', 0, 0, 2, true},    // 4..7
      {'h', 0, 0, 0, false}, {'H', 0, 0, 0, true},   {'H', 'h', 0, 1, true},  {'H', 0, 0, 2, true},    // 8..11
      {'B', 0, 0, 0, true},  {'B', 0, 0, 2, true},   {'P', 0, 0, 0, true},    {'P', 0, 0, 2, true},    // 12..15
      {'N', 0, 0, 0, false}, {'N', 0, 0, 2, false},                                                     // 16, 17
      {'l', 'h', 0, 3, false}, {'L', 'H', 0, 3, false}, {'L', 'B', 0, 3, false}, {'L', 'P', 0, 3, false},  // 18..21
      {'L', 'N', 0, 3, false}, {'H', 'B', 0, 3, false}, {'H', 'P', 0, 3, false}, {'H', 'N', 0, 3, false},  // 22..25
      {'B', 'P', 0, 3, false}, {'B', 'N', 0, 3, false}, {'P', 'P', 0, 3, false}, {'P', 'N', 0, 3, false},  // 26..29
      {'N', 'N', 0, 3, false},                                                                          // 30
      {'L', 'B', 'H', 4, false}, {'L', 'B', 'H', 5, false}, {'L', 'P', 'H', 4, false}, {'L', 'P', 'H', 5, false},  // 31..34
      {'L', 'N', 'H', 4, false}, {'L', 'N', 'H', 5, false}, {'B', 'P', 'N', 4, false}, {'B', 'P', 'N', 5, false},  // 35..38
  };
  return d[std::clamp(t, 4, 38) - 4];
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
// the same stage in double precision (matched designs: high resonance at low cutoffs)
struct SvfCd {
  double a1 = 0, a2 = 0, a3 = 0, k = 2;
  void set(double sr, double hz, double kk) {
    const double g = std::tan(3.14159265358979 * std::clamp(hz, 5.0, sr * 0.49) / sr);
    k = kk;
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
};
struct SvfSd {
  double ic1 = 0, ic2 = 0;
  void run(const SvfCd& c, double v0, double& lp, double& bp, double& hp) {
    const double v3 = v0 - ic2;
    const double v1 = c.a1 * ic1 + c.a2 * v3;
    const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
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
  int eng = -1;  // engine in use (set() picks it; a change clears the state)
  // coefficients
  SvfC s1, s2, s3, f[3];
  OpC op, op2, damp;
  float w1 = 1, w2 = 0, w3 = 0;  // matched morph weights
  SvfCd d1, d2;
  SvfSd ds1, ds2;
  double lg = 0, lk = 0, la2 = 0, la3 = 0, la4 = 0, li2 = 0, li3 = 0, ld1 = 1;  // matched ladder
  Bq bq;
  float g = 0, G = 0, k = 0, drv = 1, head = 0, mix = 0, fb = 0, var = 0, outGain = 1;  // head > 0: boost drive (see set)
  float delay = 100, delay2 = 100;     // samples
  float apA = 0;                        // first-order allpass coefficient
  float apSpread = 0;
  int stages = 4, poles = 4;
  float rmInc = 0, rmInc2 = 0, snhInc = 0;
  float fGain[3] = {1, 1, 1};
  int lens[8] = {}, lens2[4] = {};
  SvfCd fc[5];
  double fg[5] = {};
  double mA = 0, mB = 0, mC = 0;  // matched loop filters: high-pass coefficient, low-pass pole, low-pass gain
  // state (cleared by clear())
  SvfS ss1, ss2, ss3, fs[3];
  OpS o1, o2, o3, o4, od, oe[8];
  BqS b1, b2;
  float lad[4] = {};
  double ls[4] = {};
  float ap[16] = {};
  double mh[8] = {}, ml[8] = {};  // matched loop filter states
  SvfSd fz[5];                    // matched formant band-passes
  float fbState = 0;
  float rmPh = 0, rmPh2 = 0, snhPh = 0, snhHold = 0;
  int w = 0;
  int wi[8] = {};
  float* buf = nullptr;  // kBuf samples
  void clear() {
    ss1 = ss2 = ss3 = SvfS{};
    ds1 = ds2 = SvfSd{};
    for (auto& v : fs) v = SvfS{};
    o1 = o2 = o3 = o4 = od = OpS{};
    for (auto& v : oe) v = OpS{};
    b1 = b2 = BqS{};
    for (float& v : lad) v = 0;
    for (double& v : ls) v = 0;
    for (float& v : ap) v = 0;
    for (double& v : mh) v = 0;
    for (double& v : ml) v = 0;
    for (auto& v : fz) v = SvfSd{};
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

void Filter::set(int type, double sr, double hz, double res, double drive, double var, double spreadSt, bool boost) {
  type = std::clamp(type, 0, 106);
  const int eng = boost ? engineMatched(type) : engineOf(type);
  if (type != type_ || eng != ch_[0].eng) {
    type_ = type;
    reset();
  }
  sr_ = sr;
  res = std::clamp(res, 0.0, 1.0);
  drive = std::clamp(drive, 0.0, 1.0);
  var = std::clamp(var, 0.0, 1.0);
  if (boost) {
    // J-203b, measured (L - R high-band level of a 12 dB filter at stereo 0 / 25 / 75 / 100 %): the
    // source's spread is cubic in the stereo offset, +-32 semitones at the ends and +-4 at 25 / 75 %
    // (the voice passes a linear +-24 semitones). J-203e: the same law holds for every type measured
    // (state-variable, ladder, comb, phaser, scream, EQ, DJ mixer: L / R peak frequencies at 75 / 100 %).
    const double u = std::clamp(spreadSt / 24.0, -1.0, 1.0);
    spreadSt = 32.0 * u * u * u;
  }
  for (int c = 0; c < 2; ++c) {
    Ch& h = ch_[c];
    h.eng = eng;
    const double f = std::clamp(hz * std::pow(2.0, (c == 0 ? -spreadSt : spreadSt) / 12.0), 8.0, sr * 0.49);
    h.drv = static_cast<float>(1.0 + drive * 9.0);
    h.head = 0;
    if (boost) {
      // J-201, measured on the import source with a saw at two input levels: small-signal gain
      // 24 dB x drive^0.8 (+7.7 / +14 / +19.4 / +24 dB at 25 / 50 / 75 / 100 %), into a soft clip
      // whose output ceiling sits 8.5 dB above Plasma's leveled one; the Mg and EMS ladders
      // barely clip, the acid ladder 2.5 dB later, the dirty ladder 5.5 dB earlier.
      h.drv = static_cast<float>(std::pow(10.0, 1.2 * std::pow(drive, 0.8)));
      h.head = type == 99 || type == 101 ? 16.f : type == 100 ? 3.5f : type == 102 ? 0.53f : 2.66f;
    }
    h.var = static_cast<float>(var);
    h.outGain = 1;
    switch (eng) {
      case EngMSvf: {
        const MSvfDesc& d = msvf(type);
        const double k1 = svfDamping(res);
        // normal types: the level falls with resonance, g = 1 / (1 + 4.04 r) (-6.9 dB at 30 %, -13.9 dB
        // at 100 %); VAR gives it back: g + VAR (1 - g) (0 dB at 100 %)
        if (d.level) {
          const double g = 1.0 / (1.0 + 4.04 * res);
          h.outGain = static_cast<float>(g + var * (1.0 - g));
        }
        h.d1.set(sr, f, k1);
        h.op.set(sr, f);
        if (d.form == 2 || d.form == 5) {
          // 24 dB: a second section at the cutoff, damping 1 + (k / 2)^2 (2 at 0 %, about 1 above 30 %)
          h.d2.set(sr, f, 1.0 + 0.25 * k1 * k1);
        } else if (d.form == 3) {
          // dual: VAR is the second response's own cutoff, 340 Hz x 2^(10.5 (VAR - 0.5)) (fitted
          // between 12 % and 100 %; it does not follow the cutoff)
          const double f2 = std::clamp(340.0 * std::exp2(10.5 * (var - 0.5)) * f / std::max(1.0, hz), 8.0, sr * 0.49);
          // a notch after another response stays wide (no resonance); every other second response resonates
          h.d2.set(sr, f2, d.b == 'N' && d.a != 'N' ? 2.0 : k1);
          h.op2.set(sr, f2);
        }
        // morph: A -> B over the first half of VAR, B -> C over the second
        const float m = static_cast<float>(var);
        h.w1 = m < 0.5f ? 1 - 2 * m : 0.f;
        h.w2 = m < 0.5f ? 2 * m : 2 - 2 * m;
        h.w3 = m < 0.5f ? 0.f : 2 * m - 1;
        break;
      }
      case EngMX: {
        if (type == 89) {
          const double k = svfDamping(res), a = std::exp2(rejectHalfGap(var));
          h.d1.set(sr, std::clamp(f / a, 8.0, sr * 0.49), k);
          h.d2.set(sr, std::clamp(f * a, 8.0, sr * 0.49), k);
          break;
        }
        const double k = mxK(type, res);
        const bool wsp = type == 96;
        h.d1.set(sr, std::clamp(f * (wsp ? 1.01 : 1.18), 8.0, sr * 0.49), k);
        // weights: low-pass, band-pass, high-pass (VAR crossfades low to high; measured levels)
        const double gl = std::pow(10.0, (wsp ? -0.75 : -0.7) / 20), gh = std::pow(10.0, (wsp ? -3.45 : -0.85) / 20);
        h.w1 = type == 106 ? 0.f : static_cast<float>(gl * (1 - var));
        h.w2 = type == 106 ? static_cast<float>(std::pow(10.0, -0.5 / 20)) : 0.f;
        h.w3 = type == 106 ? 0.f : static_cast<float>(gh * var);
        break;
      }
      case EngMPass:
        break;
      case EngMRing:
        h.rmInc = static_cast<float>(f / sr);
        break;
      case EngMLad: {
        const MLadDesc& d = mlad(type);
        // the dirty ladder's cutoff rises toward its nominal value above 75 % resonance
        const double fm = d.fcMul + (d.kTable == 4 ? 0.12 * std::max(0.0, (res - 0.75) / 0.25) : 0.0);
        const double g = std::tan(3.14159265358979 * std::clamp(f * fm, 8.0, sr * 0.49) / sr);
        const double k = mladK(d.kTable, res);
        h.lg = g;
        h.lk = k;
        if (d.diode) {
          // coupled stages: y4 = a4 y3 + b4, y3 = a3 y2 + b3, y2 = a2 y1 + b2 (b from the states)
          h.la4 = g / (1 + g);
          h.li3 = 1 / (1 + 2 * g - g * h.la4);
          h.la3 = g * h.li3;
          h.li2 = 1 / (1 + 2 * g - g * h.la3);
          h.la2 = g * h.li2;
          h.ld1 = 1 + 2 * g - g * h.la2;
        }
        h.outGain = static_cast<float>((d.makeup ? 1 + k : 1.0) * std::pow(10.0, d.gainDb / 20));
        break;
      }
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
      case EngMComb:
      case EngDistComb: {
        h.delay = static_cast<float>(sr / f);
        if (type == 104) h.delay2 = static_cast<float>(sr / (f * 1.4983));
        h.fb = static_cast<float>(res * 0.97);
        const double cut = 200.0 * std::pow(100.0, var);  // damping filter in the loop
        const int ct = type >= 69 ? type - 24 : type >= 47 && type <= 54 ? type - 8 : type;  // flangers: the comb variant
        h.damp.set(sr, (ct == 43 || ct == 44) ? 20 + 4000 * var : cut);
        if (eng == EngDistComb) h.damp.set(sr, (type == 90 || type == 92) ? 300 + 12000 * (1 - var) : 1000 + 3000 * var);
        if (eng == EngMComb) {
          // measured: feedback = resonance, the echo enters at fb (1 + fb), output 0.5 / (1 + fb)
          // (white noise 6 dB down and flat at 0 %, teeth +-fb at higher settings)
          h.fb = static_cast<float>(res);
          h.mix = static_cast<float>(type >= 47 ? 1 + res : res * (1 + res));  // flangers: 1 + fb
          if (type >= 69) {
            // flange-phase (fitted to the measured curves): the HL6 flanger with the 2-stage phaser's
            // allpasses (at 0.51 x cutoff) inside the delay path
            const double w = std::tan(3.14159265358979 * std::clamp(f * 0.51, 8.0, sr * 0.49) / sr);
            h.apA = static_cast<float>((w - 1) / (w + 1));
          }
          h.outGain = static_cast<float>(0.5 / (1 + res));
          // loop filters (one-pole) follow the comb pitch: L6 opens from f / 16 to 256 f, H6 closes from
          // f / 128 to 32 f, HL6 widens a band around f (fitted to where the teeth stop)
          if (ct == 41 || ct == 42) h.damp.set(sr, f * std::exp2(12 * var - 4));
          else if (ct == 43 || ct == 44) h.damp.set(sr, f * std::exp2(12 * var - 7));
          else if (ct == 45 || ct == 46) {
            h.damp.set(sr, f * std::exp2(10 * var));
            h.op2.set(sr, f * std::exp2(-10 * var));
          }
        }
        break;
      }
      case EngFlange: {
        const double base = sr / f;
        h.delay = static_cast<float>(base * (c == 0 ? 1.0 : 1.0 + var * 0.5));
        h.fb = static_cast<float>(res * 0.95);
        h.damp.set(sr, (type == 51 || type == 52) ? 300 : 3000);
        break;
      }
      case EngMPhaser: {
        // measured: 2 / 4 / 6 / 8 first-order allpasses at half the cutoff (0.51), feedback = resonance,
        // the chain mixed in at 1 + fb, output 0.5 / (1 + fb); VAR has no effect
        h.stages = type >= 61 ? 8 : 2 * (1 + (type - 55) / 2);
        // L6 / H6 / HL6 (63 .. 68, fitted to the measured curves): one-pole filters on the chain output
        // (heard in the mix and the feedback); L6 opens from f / 32 to 128 f, H6 closes from f / 64 to
        // 64 f, HL6 is a band from f / 16 .. f at VAR 0 that has opened fully by about VAR 50 %
        if (type == 63 || type == 64) h.damp.set(sr, std::min(sr * 0.45, f * std::exp2(12 * var - 5)));
        else if (type == 65 || type == 66) h.damp.set(sr, std::min(sr * 0.45, f * std::exp2(12 * var - 6)));
        else if (type == 67 || type == 68) {
          h.damp.set(sr, std::min(sr * 0.45, f * std::exp2(10 * var)));
          h.op2.set(sr, f * std::exp2(-4 - 10 * var));
        }
        const double w = std::tan(3.14159265358979 * std::clamp(f * 0.51, 8.0, sr * 0.49) / sr);
        h.apA = static_cast<float>((w - 1) / (w + 1));
        h.fb = static_cast<float>(res);
        h.mix = static_cast<float>(1 + res);
        h.outGain = static_cast<float>(0.5 / (1 + res));
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
      case EngMAllp: {
        // measured (impulse responses): four Schroeder allpasses in series, coefficient 0.989 x resonance,
        // delays 0.406 / 0.322 / 0.249 / 0.164 of the cutoff period; inside each loop a one-pole
        // high-pass (157 Hz) and a one-pole low-pass whose pole is VAR at 96 kHz (VAR 100 % = the
        // delayed path is gone and only -g^4 is left); output wet only
        static const double kR[4] = {0.406, 0.322, 0.249, 0.164};
        const double P = sr / f;
        for (int i = 0; i < 4; ++i) h.lens[i] = std::clamp(static_cast<int>(std::lround(P * kR[i])), 1, kBuf / 4 - 2);
        h.fb = static_cast<float>(std::min(0.989 * res, 0.999));
        const double hz = 157.0;
        h.mA = std::exp(-kPi2 * hz / sr);                    // one-pole high-pass pole
        h.mB = std::pow(var, 96000.0 / sr);                  // low-pass pole (VAR at 96 kHz)
        h.mC = 1 - h.mB;
        break;
      }
      case EngMCombs: {
        // measured: eight feedback combs in parallel, delays 0.812 .. 1.176 of the cutoff period plus one
        // sample (whole samples at 96 kHz; fitted from 27 to 1300 samples), each
        // echo at 1 / 4, feedback 0.98 x resonance through a one-pole low-pass whose pole is VAR at 96 kHz
        // (the source's loop filter also takes away a little low end at high VAR: not modelled)
        static const double kR[8] = {0.8119, 0.86492, 0.9291, 0.98632, 1.0346, 1.0849, 1.1341, 1.1757};
        const double P = sr / f + sr / 96000.0;
        for (int i = 0; i < 8; ++i) {
          const double L = std::round(std::clamp(P * kR[i], 1.0, kBuf / 8 - 3.0));  // whole samples at 96 kHz
          h.lens[i] = static_cast<int>(L);
        }
        h.fb = static_cast<float>(std::min(0.98 * res, 0.999));
        h.mB = std::pow(var, 96000.0 / sr);
        h.mC = 1 - h.mB;
        break;
      }
      case EngMForm: {
        // measured (impulse responses on an 11 x 11 grid of cutoff knob x VAR, resonance 60 %): five
        // parallel band-passes at unity peak whose centres and bandwidths follow measured tables
        const double pos = (12.0 * std::log2(f / 440.0) + 69.0) / 136.9282;
        double tab[15];
        formantMeasured(type, pos, var, res, tab);
        for (int i = 0; i < 5; ++i) {
          const double hz = std::clamp(tab[3 * i], 20.0, sr * 0.45), k = std::clamp(tab[3 * i + 1] / hz, 0.002, 4.0);
          h.fc[i].set(sr, hz, k);
          h.fg[i] = k * tab[3 * i + 2];
        }
        break;
      }
      case EngMBass: {
        // measured (impulse responses, single state-variable fits within 0.05 dB): the output is
        // -(1.035 - VAR / 2) low-pass + VAR / 2 high-pass + (VAR k / 2 + fc / 39000) band-pass, i.e. half
        // the dry signal comes in with VAR under an inverted low-pass; the section sits at
        // 847.5 Hz x 2^(9.13 (pos - 0.5)) (2 x cutoff at the middle of the knob)
        const double pos = (12.0 * std::log2(f / 440.0) + 69.0) / 136.9282;
        const double fc = std::min(sr * 0.45, 847.5 * std::exp2(9.13 * (pos - 0.5)));
        static const double R[] = {0, 0.3, 0.6, 0.9, 1.0};
        static const double K[] = {2.028, 0.522, 0.2403, 0.0522, 0.0031};
        const double k = interp(R, K, 5, res);
        h.d1.set(sr, fc, k);
        h.mA = -1.035 + 0.5 * var;
        h.mB = 0.5 * var * k + 2.56e-5 * fc;
        h.mC = 0.5 * var;
        break;
      }
      case EngMDj: {
        // measured (impulse responses): one knob, a 12 dB state-variable low-pass below the middle (cutoff
        // 79 Hz x 2^(16.6 pos), gone from about pos 0.43 on) and a high-pass above it (117 Hz x
        // 2^(14 (pos - 0.55)), up to 5.1 kHz); damping 1.35 at r 0 to 0.057 at r 90; VAR has no effect
        // (the source leaves a little dry signal under the high-pass above pos 0.85: not modelled)
        const double pos = (12.0 * std::log2(f / 440.0) + 69.0) / 136.9282;  // the cutoff knob (0 .. 1)
        static const double R[] = {0, 0.3, 0.6, 0.9, 1.0};
        static const double K[] = {1.35, 0.52, 0.242, 0.057, 0.01};
        const bool lp = pos < 0.5001;
        const double fc = lp ? 79.0 * std::exp2(16.6 * pos) : std::min(5126.0, 117.0 * std::exp2(14.0 * (pos - 0.55)));
        h.var = lp ? 0.f : 1.f;
        h.mix = (lp && pos > 0.425) || fc > sr * 0.45 ? 0.f : 1.f;  // 0: the low-pass is gone (a wire)
        h.d1.set(sr, std::min(fc, sr * 0.45), interp(R, K, 5, res));
        break;
      }
      case EngMReverb: {
        // measured (impulse responses): the Combs type's eight parallel combs (same delays, echo 1 / 4,
        // loop low-pass pole = VAR at 96 kHz, feedback 0.99 x resonance) followed by four allpasses in
        // series, coefficient 0.5, at the Allpasses type's delays (no loop filters); DC gain 2 at r 0
        const double P = sr / f + sr / 96000.0;
        static const double kR[8] = {0.8119, 0.86492, 0.9291, 0.98632, 1.0346, 1.0849, 1.1341, 1.1757};
        for (int i = 0; i < 8; ++i) h.lens[i] = static_cast<int>(std::round(std::clamp(P * kR[i], 1.0, kRvComb - 3.0)));
        static const double kA[4] = {0.406, 0.322, 0.249, 0.164};
        for (int i = 0; i < 4; ++i) h.lens2[i] = static_cast<int>(std::round(std::clamp(P * kA[i], 1.0, kRvAp - 3.0)));
        h.fb = static_cast<float>(std::min(0.99 * res, 0.999));
        h.mix = 0.5f;
        h.mB = std::pow(var, 96000.0 / sr);
        h.mC = 1 - h.mB;
        break;
      }
      case EngMComb2: {
        // measured: the dry signal plus a feedback comb at the cutoff period (feedback = resonance, echo
        // mix r (1 + r), output 0.5 / (1 + r)) heard after a pre-delay that VAR sets:
        // 122.3 ms x 2^(-10.66 VAR) + 0.012 ms (VAR 50 % = 3 ms)
        const double P = sr / f;
        const double pre = sr * (0.12232 * std::exp2(-10.66 * var) + 1.2e-5);
        h.delay = static_cast<float>(std::clamp(P - 1, 1.0, kBuf - 4.0));  // taps count from the last write
        h.delay2 = static_cast<float>(std::clamp(P + pre - 1, 1.0, kBuf - 4.0));
        h.fb = static_cast<float>(std::min(res, 0.999));
        h.mix = static_cast<float>(res * (1 + res));
        h.outGain = static_cast<float>(0.5 / (1 + res));
        break;
      }
      case EngMScream: {
        // measured (impulse responses, drive 0 .. 50 %): a state-variable low-pass (Scream, Scream3LP) or
        // band-pass (Scream3BP) at the cutoff; damping against resonance per family; level -12 / -13.2 dB
        // (band-pass: 0.2065 x the unnormalised band output); the drive is a clean gain of
        // 66 d - 23 d^2 dB (+6.6 dB at 10 %, +28 dB at 50 %, +43 dB at 100 %) that stays linear up to
        // high levels (soft ceiling well above the other types); on the 3-pole types VAR only turns the phase
        static const double R[] = {0, 0.15, 0.3, 0.45, 0.6, 0.75, 0.9, 1.0};
        static const double KS[] = {2.03, 0.755, 0.514, 0.354, 0.23, 0.128, 0.041, 0.003};
        static const double K3[] = {1.94, 0.925, 0.733, 0.605, 0.506, 0.424, 0.354, 0.312};
        double k = interp(R, type == 83 ? KS : K3, 8, res), fm = 1.0;
        if (type == 83) {  // Scream: VAR sharpens the resonance and pulls it down (measured to r 90 %)
          const double rv = res * res * var * var;
          k = std::max(0.0005, k - 0.2 * rv);
          fm = (1.0 - 0.02 * res) * (1.0 - 0.13 * rv);
        }
        h.d1.set(sr, f * fm, k);
        h.outGain = static_cast<float>(type == 83 ? 0.252 : type == 94 ? 0.22 : 0.2065);
        h.drv = static_cast<float>(std::pow(10.0, (66 * drive - 23 * drive * drive) / 20));
        h.head = 16.f;
        break;
      }
      case EngMEq: {
        // measured: a state-variable section (12 dB types) or a one-pole (6 dB) at the cutoff; the output
        // is the input plus (A - 1) x the low-pass / band-pass (unnormalised) / high-pass, where the gain
        // A = 1 + u^3 below the middle of VAR and 1 + 8 u^3 above it (u = 2 VAR - 1: -inf .. +19.1 dB);
        // the bell is that literal sum, so a cut at high resonance overshoots into a boost, as measured
        static const double R[] = {0, 0.15, 0.3, 0.45, 0.6, 0.75, 0.9, 1.0};
        static const double K[] = {2.014, 0.7574, 0.5209, 0.3624, 0.2401, 0.1391, 0.0523, 0.001};
        h.d1.set(sr, f, interp(R, K, 8, res));
        const double u = 2 * var - 1;
        h.mix = static_cast<float>(u * u * u * (u > 0 ? 8.0 : 1.0));  // A - 1
        h.mB = std::pow(0.16, 96000.0 / sr);
        h.mC = 1 - h.mB;
        h.lk = 1 - std::exp(-kPi2 * f / sr);
        break;
      }
      case EngCombs:
      case EngAllpass:
      case EngMDiff:
      case EngRoom: {
        static const double r[8] = {1.0, 1.31, 1.73, 2.09, 2.47, 2.91, 3.37, 3.83};
        const double base = std::min(static_cast<double>(kBuf / 8 - 4), sr / f);
        for (int i = 0; i < 8; ++i) {
          const double sp = 1.0 + (r[i] - 1.0) * (eng == EngRoom ? (0.5 + var) : (0.2 + var * 1.6));
          h.lens[i] = std::clamp(static_cast<int>(base * sp * (c && eng != EngMDiff ? 1.07 : 1.0)), 2, kBuf / 8 - 2);
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
  const int eng = ch_[0].eng;
  float* io[2] = {&l, &r};
  for (int c = 0; c < 2; ++c) {
    Ch& h = ch_[c];
    float x = *io[c];
    float y = x;
    if (h.head > 0) {
      if (eng != EngScream && eng != EngLadder && eng != EngMLad && eng != EngMScream) x = h.head * fastTanh(x * h.drv / h.head);
    } else if (h.drv > 1.0001f && eng != EngScream && eng != EngLadder && eng != EngMLad) {
      x = fastTanh(x * h.drv) / std::sqrt(h.drv);
    }
    switch (eng) {
      case EngMSvf: {
        const MSvfDesc& d = msvf(t);
        // one response of a state-variable section (or a one-pole for 'l' / 'h')
        auto resp = [](char m, double in, double lp, double bp, double hp, double op) {
          switch (m) {
            case 'L': return lp;
            case 'H': return hp;
            case 'B': return bp;
            case 'P': return lp + hp + bp;
            case 'N': return lp + hp;
            case 'l': return op;
            case 'h': return in - op;
            default: return in;
          }
        };
        double lp = 0, bp = 0, hp = 0;
        double op = 0;
        if (d.a == 'l' || d.a == 'h') op = h.o1.lp(h.op, x);
        else h.ds1.run(h.d1, x, lp, bp, hp);
        switch (d.form) {
          case 0: y = static_cast<float>(resp(d.a, x, lp, bp, hp, op)); break;
          case 1: {
            const float v = static_cast<float>(resp(d.a, x, lp, bp, hp, op));
            const float o = h.o1.lp(h.op, v);
            y = d.b == 'l' ? o : v - o;
            break;
          }
          case 2: {
            const double v = resp(d.a, x, lp, bp, hp, op);
            double l2, b2, h2;
            h.ds2.run(h.d2, v, l2, b2, h2);
            y = static_cast<float>(resp(d.a, v, l2, b2, h2, 0));
            break;
          }
          case 3: {
            const double v = resp(d.a, x, lp, bp, hp, op);
            double l2 = 0, b2 = 0, h2 = 0, o2 = 0;
            if (d.b == 'l' || d.b == 'h') o2 = h.o2.lp(h.op2, static_cast<float>(v));
            else h.ds2.run(h.d2, v, l2, b2, h2);
            y = static_cast<float>(resp(d.b, v, l2, b2, h2, o2));
            break;
          }
          default: {
            const double v = h.w1 * resp(d.a, x, lp, bp, hp, 0) + h.w2 * resp(d.b, x, lp, bp, hp, 0) + h.w3 * resp(d.c, x, lp, bp, hp, 0);
            if (d.form == 4) {
              y = static_cast<float>(v);
            } else {
              double l2, b2, h2;
              h.ds2.run(h.d2, v, l2, b2, h2);
              y = static_cast<float>(h.w1 * resp(d.a, v, l2, b2, h2, 0) + h.w2 * resp(d.b, v, l2, b2, h2, 0) + h.w3 * resp(d.c, v, l2, b2, h2, 0));
            }
            break;
          }
        }
        y *= h.outGain;
        break;
      }
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
      case EngMX: {
        double lp, bp, hp;
        if (t == 89) {
          double lp2, bp2, hp2;
          h.ds1.run(h.d1, x, lp, bp, hp);
          h.ds2.run(h.d2, x, lp2, bp2, hp2);
          y = static_cast<float>(lp + hp2);
        } else {
          h.ds1.run(h.d1, x, lp, bp, hp);
          y = static_cast<float>(h.w1 * lp + h.w2 * bp + h.w3 * hp);
        }
        break;
      }
      case EngMPass:
        y = x;
        break;
      case EngMRing: {  // ring modulation by a sine at the cutoff (white noise comes out 3 dB down, flat)
        h.rmPh += h.rmInc;
        h.rmPh -= std::floor(h.rmPh);
        y = x * std::sin(static_cast<float>(kPi2) * h.rmPh);
        break;
      }
      case EngMLad: {
        const double g = h.lg, k = h.lk, hd = h.head > 0 ? h.head : 1.0;
        const double xin = static_cast<double>(x) * h.drv;
        double* st = h.ls;
        double yo;
        if (mlad(t).diode) {
          const double b4 = st[3] / (1 + g);
          const double b3 = (g * b4 + st[2]) * h.li3;
          const double b2 = (g * b3 + st[1]) * h.li2;
          const double A = h.la4 * h.la3 * h.la2, B = h.la4 * (h.la3 * b2 + b3) + b4;
          double y1 = (g * (xin - k * B) + g * b2 + st[0]) / (h.ld1 + g * k * A);
          const double u = xin - k * (A * y1 + B);
          const double us = hd * fastTanh(static_cast<float>(u / hd));
          y1 = (g * us + g * b2 + st[0]) / h.ld1;
          const double y2 = h.la2 * y1 + b2, y3 = h.la3 * y2 + b3, y4 = h.la4 * y3 + b4;
          st[0] = 2 * y1 - st[0];
          st[1] = 2 * y2 - st[1];
          st[2] = 2 * y3 - st[2];
          st[3] = 2 * y4 - st[3];
          yo = y4;
        } else {
          const double G = g / (1 + g);
          const double S = (G * G * G * st[0] + G * G * st[1] + G * st[2] + st[3]) / (1 + g);
          const double u = (xin - k * S) / (1 + k * G * G * G * G);
          double in = hd * fastTanh(static_cast<float>(u / hd));
          for (int s = 0; s < 4; ++s) {
            const double v = (in - st[s]) * G;
            const double o = v + st[s];
            st[s] = o + v;
            in = o;
          }
          yo = in;
        }
        y = static_cast<float>(yo * h.outGain);
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
        const float hd = h.head > 0 ? h.head : 1.f;
        if (t == 102) u = hd * (fastTanh(u / hd * 1.6f + 0.15f) - 0.15f);  // dirty: asymmetric
        else u = hd * fastTanh(u / hd);
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
        // BODY: gives back the level the resonance takes; matched (Boost) Mg types: all of it at 100 %
        // (passband 1 / (1 + k) -> 1 + VAR k, measured), classic: half
        y *= 1.f + h.k * (h.head > 0 && t <= 3 ? 1.f : 0.5f) * h.var;
        if (h.head <= 0) y /= h.drv > 1 ? std::sqrt(h.drv) : 1.f;
        break;
      }
      case EngComb:
      case EngMComb: {
        float d = h.tapFrac(h.delay);
        const int ct = t >= 69 ? t - 24 : t >= 47 && t <= 54 ? t - 8 : t;
        if (t >= 69)
          for (int s = 0; s < 2; ++s) {
            const float o = h.apA * d + h.ap[s];
            h.ap[s] = d - h.apA * o;
            d = o;
          }
        if (ct == 41 || ct == 42) d = h.o1.lp(h.damp, d);
        else if (ct == 43 || ct == 44) d = d - h.o1.lp(h.damp, d);
        else if (ct == 45 || ct == 46) d = h.o2.lp(h.damp, d - h.o1.lp(h.op2, d));
        const float sgn = (t & 1) ? 1.f : -1.f;  // odd indices are the "+" types
        float in = x + d * h.fb * sgn;
        if (t == 104) in += h.tapFrac(h.delay2) * h.fb * 0.5f;
        h.push(std::clamp(in, -4.f, 4.f));
        y = eng == EngMComb ? (x + d * sgn * h.mix) * h.outGain : (x + d * sgn) * 0.5f;
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
      case EngMPhaser: {
        const float sg = (t & 1) ? 1.f : -1.f;
        float in = x + h.fbState * h.fb * sg;
        for (int s = 0; s < h.stages && s < 16; ++s) {
          const float o = h.apA * in + h.ap[s];
          h.ap[s] = in - h.apA * o;
          in = o;
        }
        if (t == 63 || t == 64) in = h.o1.lp(h.damp, in);
        else if (t == 65 || t == 66) in = in - h.o1.lp(h.damp, in);
        else if (t == 67 || t == 68) in = h.o2.lp(h.damp, in - h.o1.lp(h.op2, in));
        h.fbState = in;
        y = (x + sg * h.mix * in) * h.outGain;
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
      case EngMAllp: {
        const int seg = kBuf / 4;
        double v = x;
        const double g = h.fb;
        for (int i = 0; i < 4; ++i) {
          float* b = h.buf + i * seg;
          double d = b[(h.w - h.lens[i] + seg) % seg];
          // high-pass (x - lp) then low-pass, both one-pole
          h.mh[i] = d + h.mA * (h.mh[i] - d);
          d -= h.mh[i];
          h.ml[i] = h.mC * d + h.mB * h.ml[i];
          d = h.ml[i];
          const double w = v + g * d;
          b[h.w] = static_cast<float>(w);
          v = d - g * w;
        }
        h.w = (h.w + 1) % (kBuf / 4);
        y = static_cast<float>(v);
        break;
      }
      case EngMEq: {
        // the sections hear a softened input (one-pole, pole 0.16 at 96 kHz); the dry path does not, so a
        // full high-shelf cut leaves the difference, which comes back up toward the top as measured
        h.ml[0] = h.mC * x + h.mB * h.ml[0];
        const double xs = h.ml[0];
        double v;
        if (t == 71 || t == 74) {
          // one-pole low-pass of the previous input sample (the low shelf's measured zero)
          const double lp = h.ml[1];
          h.ml[1] = h.lk * xs + (1 - h.lk) * lp;
          v = t == 71 ? lp : xs - lp;
        } else {
          double lp, bp, hp;
          h.ds1.run(h.d1, xs, lp, bp, hp);
          v = t == 72 ? lp : t == 73 ? bp : hp;
        }
        y = static_cast<float>(x + h.mix * v);
        break;
      }
      case EngMScream: {
        double lp, bp, hp;
        h.ds1.run(h.d1, static_cast<double>(x) * h.drv, lp, bp, hp);
        const double v = (t == 95 ? bp : lp) * h.outGain;
        y = h.head * fastTanh(static_cast<float>(v / h.head));
        break;
      }
      case EngMCombs: {
        const int seg = kBuf / 8;
        double acc = 0;
        for (int i = 0; i < 8; ++i) {
          float* b = h.buf + i * seg;
          const double d = b[(h.w - h.lens[i] + seg) % seg];
          h.ml[i] = h.mC * d + h.mB * h.ml[i];
          b[h.w] = static_cast<float>(x + h.fb * h.ml[i]);
          acc += d;
        }
        h.w = (h.w + 1) % seg;
        y = static_cast<float>(0.25 * acc);
        break;
      }
      case EngMForm: {
        double acc = 0;
        for (int i = 0; i < 5; ++i) {
          double lp, bp, hp;
          h.fz[i].run(h.fc[i], x, lp, bp, hp);
          acc += h.fg[i] * bp;
        }
        y = static_cast<float>(acc);
        break;
      }
      case EngMBass: {
        double lp, bp, hp;
        h.ds1.run(h.d1, x, lp, bp, hp);
        y = static_cast<float>(h.mA * lp + h.mB * bp + h.mC * hp);
        break;
      }
      case EngMDj: {
        double lp, bp, hp;
        h.ds1.run(h.d1, x, lp, bp, hp);
        y = h.mix == 0.f ? x : static_cast<float>(h.var > 0.5f ? hp : lp);
        break;
      }
      case EngMReverb: {
        double acc = 0;
        for (int i = 0; i < 8; ++i) {
          float* b = h.buf + i * kRvComb;
          const double d = b[(h.w - h.lens[i] + kRvComb) % kRvComb];
          h.ml[i] = h.mC * d + h.mB * h.ml[i];
          b[h.w] = static_cast<float>(x + h.fb * h.ml[i]);
          acc += d;
        }
        h.w = (h.w + 1) % kRvComb;
        double v = 0.25 * acc;
        const double g = h.mix;
        const int wa = h.wi[0];
        for (int i = 0; i < 4; ++i) {
          float* b = h.buf + 8 * kRvComb + i * kRvAp;
          const double d = b[(wa - h.lens2[i] + kRvAp) % kRvAp];
          const double w = v + g * d;
          b[wa] = static_cast<float>(w);
          v = d - g * w;
        }
        h.wi[0] = (wa + 1) % kRvAp;
        y = static_cast<float>(v);
        break;
      }
      case EngMComb2: {
        const float d = h.tapFrac(h.delay), e = h.tapFrac(h.delay2);
        h.push(x + h.fb * d);
        y = (x + h.mix * e) * h.outGain;
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
      case EngAllpass:
      case EngMDiff: {
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
        y = eng == EngMDiff ? v : t == 98 ? x * (1 - h.var) + v * h.var : (x + v) * 0.5f;
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

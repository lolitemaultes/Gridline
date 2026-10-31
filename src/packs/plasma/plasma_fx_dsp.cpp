// Gridline — "plasma" pack: the effect modules (Plasma's own designs):
// distortion, flanger, phaser, chorus, delay, compressor (single / multiband
// with upward compression), reverb (plate / hall / vintage / abyss / space),
// two-band EQ, filter, hyper / dimension, frequency shifter, convolution,
// utility and the band splitters' crossovers. Memory is allocated on the UI
// thread (makeFxBank); processing never allocates.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "plasma_dsp.h"

namespace gl::packs::plasma {

// ------------------------------------------------------------------ LR4 crossover
void Lr4::set(double sr, double f) {
  const double w = std::tan(3.14159265358979 * std::clamp(f, 10.0, sr * 0.45) / sr);
  const double w2 = w * w, r = std::sqrt(2.0) * w, d = 1 + r + w2;
  b0 = w2 / d, b1 = 2 * w2 / d, b2 = w2 / d;
  hb0 = 1 / d, hb1 = -2 / d, hb2 = 1 / d;
  a1 = 2 * (w2 - 1) / d, a2 = (1 - r + w2) / d;
}
void Lr4::split(int c, double x, double& lo, double& hi) {
  auto bq = [&](double v, double (&z)[2], double c0, double c1, double c2) {
    const double y = c0 * v + z[0];
    z[0] = c1 * v - a1 * y + z[1];
    z[1] = c2 * v - a2 * y;
    return y;
  };
  lo = bq(bq(x, lz[0][c], b0, b1, b2), lz[1][c], b0, b1, b2);
  hi = bq(bq(x, hz[0][c], hb0, hb1, hb2), hz[1][c], hb0, hb1, hb2);
}

float fxDistTable(int mode, float drive, float x);  // plasma_fxdist_dsp.cpp

namespace {
double dbToGainF(double db) { return std::pow(10.0, db / 20.0); }
// compressor laws (J-203c, measured): threshold offset against the peak (dB), attack time scale
constexpr double kCompThrOfs = 8.4, kCompAtkScale = 0.33;
// multiband detector (J-203d): running-mean time constant (ms) and its reading offset (dB)
constexpr double kMbRmsMs = 3.0, kMbOfsDb = 9.5;
// hyper (J-203h, measured on the source with deterministic white noise, impulse responses per 21 ms window):
// voices per side for UNISON 0..7, the voice gain, the wet path's 2-pole low cut, the LFO rate law
constexpr int kHyperSide[8] = {0, 0, 1, 2, 3, 5, 5, 7};
constexpr float kHyperVoice = 0.97f;
constexpr double kHyperHpHz = 253.0, kHyperHpQ = 0.75;
// J-203k, measured per voice on the source (detune 10 %, voice delays tracked by cross-correlation every
// 40 ms, unison 2 .. 7): every voice starts at a fixed sweep phase and runs at its own rate multiple,
// different on the left and the right (that is where the width at small detune comes from); voices are
// added in this order as UNISON rises. Phase 0 is the longest delay.
constexpr double kHyperPh0[2][7] = {{0.051, 0.697, 0.114, 0.356, 0.063, 0.531, 0.199},
                                    {0.293, 0.528, 0.235, 0.494, 0.580, 0.315, 0.189}};
constexpr double kHyperRate[2][7] = {{0.94, 1.006, 0.862, 0.695, 1.24, 1.47, 0.58},
                                     {0.976, 0.874, 1.066, 0.737, 1.40, 1.59, 0.61}};

struct Line {  // fractional delay line inside the unit's pool
  float* b = nullptr;
  int n = 1, w = 0;
  void bind(float*& pool, int len) {
    b = pool;
    n = std::max(4, len);
    pool += n;
    w = 0;
  }
  void clear() {
    if (b) std::fill(b, b + n, 0.f);
    w = 0;
  }
  void push(float x) {
    w = w + 1 >= n ? 0 : w + 1;
    b[w] = x;
  }
  float tap(float d) const {  // linear
    d = std::clamp(d, 1.f, static_cast<float>(n - 3));
    int i = static_cast<int>(d);
    const float fr = d - static_cast<float>(i);
    int a = w - i;
    if (a < 0) a += n;
    int c = a - 1;
    if (c < 0) c += n;
    return b[a] + (b[c] - b[a]) * fr;
  }
  float tapCubic(float d) const {
    d = std::clamp(d, 2.f, static_cast<float>(n - 4));
    const int i = static_cast<int>(d);
    const float t = d - static_cast<float>(i);
    auto at = [&](int k) {
      int j = w - k;
      while (j < 0) j += n;
      return b[j];
    };
    const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
  }
  // first-order allpass (Thiran) interpolation: flat magnitude, so a recirculating line loses no highs
  float tapAp(float d, float& y1) const {
    d = std::clamp(d, 2.f, static_cast<float>(n - 4));
    const int i = static_cast<int>(d - 0.5f);
    const float D = d - static_cast<float>(i), a = (1.f - D) / (1.f + D);
    int j0 = w - i;
    if (j0 < 0) j0 += n;
    const int j1 = j0 > 0 ? j0 - 1 : n - 1;
    const float y = a * b[j0] + b[j1] - a * y1;
    y1 = y;
    return y;
  }
  float tapInt(int d) const {
    d = std::clamp(d, 1, n - 1);
    int a = w - d;
    if (a < 0) a += n;
    return b[a];
  }
};
struct Op1 {
  float z = 0, a = 0;
  void lp(double sr, double hz) { a = static_cast<float>(std::exp(-kPi2 * std::clamp(hz, 1.0, sr * 0.49) / sr)); }
  float run(float x) { return z = x + (z - x) * a; }
};
struct Bq2 {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2]{}, z2[2]{};
  void set(int kind, double sr, double hz, double q, double db) {  // 0 LS, 1 peak, 2 HS, 3 HP, 4 LP, 5 BP
    const double A = std::pow(10.0, db / 40.0), w = kPi2 * std::clamp(hz, 10.0, sr * 0.49) / sr;
    const double cw = std::cos(w), sw = std::sin(w), al = sw / (2 * std::max(0.05, q)), sq = 2 * std::sqrt(A) * al;
    double B0, B1, B2, A0, A1, A2;
    switch (kind) {
      case 0: B0 = A * ((A + 1) - (A - 1) * cw + sq), B1 = 2 * A * ((A - 1) - (A + 1) * cw), B2 = A * ((A + 1) - (A - 1) * cw - sq);
        A0 = (A + 1) + (A - 1) * cw + sq, A1 = -2 * ((A - 1) + (A + 1) * cw), A2 = (A + 1) + (A - 1) * cw - sq; break;
      case 2: B0 = A * ((A + 1) + (A - 1) * cw + sq), B1 = -2 * A * ((A - 1) + (A + 1) * cw), B2 = A * ((A + 1) + (A - 1) * cw - sq);
        A0 = (A + 1) - (A - 1) * cw + sq, A1 = 2 * ((A - 1) - (A + 1) * cw), A2 = (A + 1) - (A - 1) * cw - sq; break;
      case 3: B0 = (1 + cw) / 2, B1 = -(1 + cw), B2 = (1 + cw) / 2, A0 = 1 + al, A1 = -2 * cw, A2 = 1 - al; break;
      case 4: B0 = (1 - cw) / 2, B1 = 1 - cw, B2 = (1 - cw) / 2, A0 = 1 + al, A1 = -2 * cw, A2 = 1 - al; break;
      case 5: B0 = al, B1 = 0, B2 = -al, A0 = 1 + al, A1 = -2 * cw, A2 = 1 - al; break;
      default: B0 = 1 + al * A, B1 = -2 * cw, B2 = 1 - al * A, A0 = 1 + al / A, A1 = -2 * cw, A2 = 1 - al / A; break;
    }
    b0 = B0 / A0, b1 = B1 / A0, b2 = B2 / A0, a1 = A1 / A0, a2 = A2 / A0;
  }
  float run(int c, float x) {
    const double y = b0 * x + z1[c];
    z1[c] = b1 * x - a1 * y + z2[c];
    z2[c] = b2 * x - a2 * y;
    return static_cast<float>(y);
  }
  void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
};
struct Ap1 {  // first-order allpass
  float z = 0;
  float run(float x, float a) {
    const float y = a * x + z;
    z = x - a * y;
    return y;
  }
};
struct ApD {  // delay allpass (Schroeder)
  Line l;
  float run(float x, float g, float d) {
    const float v = l.tap(d);
    const float in = x + g * v;
    l.push(in);
    return v - g * in;
  }
};
// first-order high shelf: gain 1 at DC, G at high frequencies, centred on K = tan(pi fc / sr)
struct Shelf1 {
  float b0 = 1, b1 = 0, a1 = 0, z = 0;
  void set(double K, double G) {
    const double sg = std::sqrt(std::max(1e-6, G)), a0 = 1 / sg + K;
    b0 = static_cast<float>((sg + K) / a0), b1 = static_cast<float>((K - sg) / a0), a1 = static_cast<float>((K - 1 / sg) / a0);
  }
  float run(float x) {
    const float y = b0 * x + z;
    z = b1 * x - a1 * y;
    return y;
  }
};
// gain plus nine first-order high shelves an octave apart (63 Hz .. 16 kHz): a smooth measured tone
// curve (J-203f). db[0] is the gain below the first corner, db[k] the step at corner k.
struct ShelfEq {
  static constexpr int kN = 9;
  double g = 1, b0[kN]{}, b1[kN]{}, a1[kN]{}, z[kN][2]{};
  float last[kN + 1]{};
  bool valid = false;
  void set(double sr, const float* db) {
    if (valid && std::equal(db, db + kN + 1, last)) return;
    std::copy(db, db + kN + 1, last);
    valid = true;
    g = std::pow(10.0, db[0] / 20.0);
    for (int k = 0; k < kN; ++k) {
      const double fc = std::min(62.99 * std::exp2(k), sr * 0.45), K = std::tan(3.14159265358979 * fc / sr);
      const double sg = std::pow(10.0, db[k + 1] / 40.0), a0 = 1 / sg + K;
      b0[k] = (sg + K) / a0, b1[k] = (K - sg) / a0, a1[k] = (K - 1 / sg) / a0;
    }
  }
  float run(int c, float x) {
    double y = x * g;
    for (int k = 0; k < kN; ++k) {
      const double o = b0[k] * y + z[k][c];
      z[k][c] = b1[k] * y - a1[k] * o;
      y = o;
    }
    return static_cast<float>(y);
  }
  void clear() {
    for (auto& v : z) v[0] = v[1] = 0;
  }
};
#include "plasma_fx_reverb_tone.inc"
double lfoBeats(double norm) { return lfoRateDivisionBeats(static_cast<int>(std::lround(std::clamp(norm, 0.0, 1.0) * (lfoRateDivisionCount() - 1)))); }
float triLfo(double ph) {
  ph -= std::floor(ph);
  return static_cast<float>(ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4);
}
// Hilbert pair (two allpass chains, 90 degrees apart over the audio band)
struct Hilbert {  // two chains of allpasses in z^-2 whose outputs stay 90 degrees apart (about 20 Hz .. 20 kHz)
  struct Ap2 {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float run(float x, float c) {  // y = c (x + y[n-2]) - x[n-2]
      const float y = c * (x + y2) - x2;
      x2 = x1, x1 = x, y2 = y1, y1 = y;
      return y;
    }
  };
  Ap2 a[4], b[4];
  float prevB = 0;
  void run(float x, float& i, float& q) {
    static const float ca[4] = {0.6923878f, 0.9360654f, 0.9882295f, 0.9987488f};
    static const float cb[4] = {0.4021921f, 0.8561711f, 0.9722910f, 0.9952885f};
    float u = x, v = x;
    for (int k = 0; k < 4; ++k) {
      u = a[k].run(u, ca[k] * ca[k]);
      v = b[k].run(v, cb[k] * cb[k]);
    }
    i = u;
    q = prevB;
    prevB = v;
  }
};
}  // namespace

// ------------------------------------------------------------------ the unit
class FxUnit {
 public:
  double sr = 48000;
  std::vector<float> pool;
  int type = -1;
  float meter = 0;
  // generic modulation phase
  double lfoPh = 0;
  // distortion
  Filter* filt = nullptr;
  std::unique_ptr<Filter> filterMem;
  float shState[2]{}, dsHold[2]{};
  double dsPh = 0, dsv[2][2]{};
  Bq2 dfl;
  // modulation effects
  Line ml[4];
  Ap1 ph[2][32];
  float phFb[2]{};
  Op1 lpw[2], hpw[2];
  // delay
  Line dl[2];
  Bq2 dbp;
  Op1 dhp[2];  // the echo filter's low cut (one pole)
  float dfb[2]{};
  // compressor
  Lr4 xo1, xo2;
  float env[3][2]{}, gr[3]{}, mbEnv[3][3]{};
  Line look[2];
  // reverb
  Line rv[8], pre[2];
  ApD dif[4][2];
  Shelf1 rvLs[8], rvHs[8];
  float rvAp[8]{}, rvG[8]{};
  ShelfEq rvEq;
  double rvMod = 0;
  float rvSh[2]{};
  // eq / utility
  Bq2 eq1, eq2, uhp, uhp2, ulp;
  Lr4 ulf;
  // hyper / dimension
  double hyPh[16]{};
  Bq2 hyHp;
  bool hyInit = false;
  Bq2 rvBw[4];  // vintage reverb: the wet band ends at 16 kHz
  // bode
  Hilbert hb[2];
  double bodePh = 0;
  Line bd[2];
  ApD blur[2][2];
  // convolution (uniform partitions of kPart)
  static constexpr int kPart = 256, kFftN = 512;
  Fft cfft;
  int parts = 0;
  std::vector<cpx> irSpec[2];   // parts * (kFftN/2 + 1)
  std::vector<cpx> fdl[2];      // parts * (kFftN/2 + 1)
  std::vector<cpx> work;
  std::vector<float> inBuf[2], ovl[2], outBuf[2];
  std::vector<float> irHead[2], hist[2];  // first partition, run directly so the wet path has no latency
  int hpos = 0;
  int cpos = 0, fdlPos = 0;
  Line cpre[2];
  Op1 cdamp[2];
  Bq2 ctone;
  bool convReady = false;

  void clear() {
    std::fill(pool.begin(), pool.end(), 0.f);
    for (auto& l : ml) l.w = 0;
    for (auto& c : ph)
      for (auto& a : c) a.z = 0;
    phFb[0] = phFb[1] = 0;
    for (auto& l : dl) l.w = 0;
    dfb[0] = dfb[1] = 0;
    dbp.clear(), dfl.clear(), eq1.clear(), eq2.clear(), uhp.clear(), uhp2.clear(), ulp.clear(), hyHp.clear();
    for (auto& b : rvBw) b.clear();
    hyInit = false;
    for (auto& e : env) e[0] = e[1] = 0;
    for (auto& e : mbEnv) e[0] = e[1] = e[2] = 0;
    xo1 = Lr4{}, xo2 = Lr4{}, ulf = Lr4{};
    for (auto& f : rvLs) f.z = 0;
    for (auto& f : rvHs) f.z = 0;
    for (auto& v : rvAp) v = 0;
    rvEq.clear();
    lpw[0].z = lpw[1].z = hpw[0].z = hpw[1].z = 0;
    dhp[0].z = dhp[1].z = 0;
    shState[0] = shState[1] = dsHold[0] = dsHold[1] = 0;
    dsv[0][0] = dsv[0][1] = dsv[1][0] = dsv[1][1] = 0;
    hb[0] = Hilbert{}, hb[1] = Hilbert{};
    if (filterMem) filterMem->reset();
    for (int c = 0; c < 2; ++c) {
      if (!fdl[c].empty()) std::fill(fdl[c].begin(), fdl[c].end(), cpx(0, 0));
      if (!inBuf[c].empty()) std::fill(inBuf[c].begin(), inBuf[c].end(), 0.f);
      if (!hist[c].empty()) std::fill(hist[c].begin(), hist[c].end(), 0.f);
      if (!ovl[c].empty()) std::fill(ovl[c].begin(), ovl[c].end(), 0.f);
      if (!outBuf[c].empty()) std::fill(outBuf[c].begin(), outBuf[c].end(), 0.f);
    }
    cpos = fdlPos = 0;
  }
  void bindLines() {
    float* p = pool.data();
    const int s = static_cast<int>(sr);
    // modulation lines (chorus / flanger / hyper / dimension): 60 ms each
    for (auto& l : ml) l.bind(p, s * 60 / 1000 + 8);
    // delay: 2 s per channel
    for (auto& l : dl) l.bind(p, s * 2 + 8);
    // compressor lookahead 6 ms
    for (auto& l : look) l.bind(p, s / 100 + 8);
    // reverb: 8 lines of up to 0.35 s (size up to 2x shrinks to fit), predelay 0.6 s
    for (auto& l : rv) l.bind(p, s * 35 / 100 + 8);
    for (auto& l : pre) l.bind(p, s * 6 / 10 + 8);
    for (auto& d : dif)
      for (auto& c : d) c.l.bind(p, s / 40 + 8);
    // bode feedback delay 2 s, blur allpasses 30 ms
    for (auto& l : bd) l.bind(p, s * 2 + 8);
    for (auto& c : blur)
      for (auto& a : c) a.l.bind(p, s * 3 / 100 + 8);
    for (auto& l : cpre) l.bind(p, s + 8);
  }
  static size_t poolSize(double sr) {
    const size_t s = static_cast<size_t>(sr);
    return 4 * (s * 60 / 1000 + 8) + 2 * (s * 2 + 8) + 2 * (s / 100 + 8) + 8 * (s * 35 / 100 + 8) + 2 * (s * 6 / 10 + 8) +
           8 * (s / 40 + 8) + 2 * (s * 2 + 8) + 4 * (s * 3 / 100 + 8) + 2 * (s + 8) + 64;
  }
};

FxBank::~FxBank() = default;

namespace {
void buildConv(FxUnit& u, const Aux* aux, int slot, const PluginState& s) {
  // Impulse: file (aux) or a generated one, resampled to the engine rate, shaped by size.
  const int k = fxP(slot, X_P0 + fxParamIndex(FxConvolve, "kParamIr"));
  const int irChoice = k < static_cast<int>(s.params.size()) ? static_cast<int>(std::lround(fxPlain(FxConvolve, fxParamIndex(FxConvolve, "kParamIr"), s.params[static_cast<size_t>(k)]))) : 0;
  std::vector<float> L, R;
  double rate = 48000;
  if (irChoice == 0 && aux && aux->irFile[static_cast<size_t>(slot)]) {
    const Sample& smp = *aux->irFile[static_cast<size_t>(slot)];
    L = smp.left;
    R = smp.mono() ? smp.left : smp.right.vec();
    rate = smp.sampleRate;
  } else {
    const Impulse& ir = factoryImpulse(std::max(0, irChoice - 1));
    L = ir.L;
    R = ir.R.empty() ? ir.L : ir.R;
    rate = ir.rate;
  }
  const int ksz = fxParamIndex(FxConvolve, "kParamSize");
  const double size = ksz >= 0 && fxP(slot, X_P0 + ksz) < static_cast<int>(s.params.size())
                          ? fxPlain(FxConvolve, ksz, s.params[static_cast<size_t>(fxP(slot, X_P0 + ksz))]) / 100.0 : 1.0;
  const double ratio = rate / u.sr / std::clamp(size, 0.1, 10.0);
  const size_t maxLen = static_cast<size_t>(u.sr * 6);
  const size_t n = std::min(maxLen, static_cast<size_t>(static_cast<double>(L.size()) / ratio));
  std::vector<float> rl(n), rr(n);
  for (size_t i = 0; i < n; ++i) {
    const double x = static_cast<double>(i) * ratio;
    const size_t a = std::min(L.size() - 1, static_cast<size_t>(x)), b = std::min(L.size() - 1, a + 1);
    const double f = x - std::floor(x);
    rl[i] = static_cast<float>(L[a] + (L[b] - L[a]) * f);
    rr[i] = static_cast<float>(R[std::min(a, R.size() - 1)] + (R[std::min(b, R.size() - 1)] - R[std::min(a, R.size() - 1)]) * f);
  }
  // minimum phase (cepstral folding) when asked
  const int kmp = fxParamIndex(FxConvolve, "kParamMinPhase");
  if (kmp >= 0 && fxP(slot, X_P0 + kmp) < static_cast<int>(s.params.size()) && s.params[static_cast<size_t>(fxP(slot, X_P0 + kmp))] >= 0.5f && n > 16) {
    int N = 1;
    while (N < static_cast<int>(n) * 2) N <<= 1;
    Fft f;
    f.init(N);
    for (auto* v : {&rl, &rr}) {
      std::vector<cpx> X(static_cast<size_t>(N), cpx(0, 0));
      for (size_t i = 0; i < n; ++i) X[i] = cpx((*v)[i], 0);
      f.forward(X.data());
      for (auto& c : X) c = cpx(std::log(std::max(1e-9f, std::abs(c))), 0);
      f.inverse(X.data());
      for (int i = 1; i < N / 2; ++i) X[static_cast<size_t>(i)] *= 2.f;
      for (int i = N / 2 + 1; i < N; ++i) X[static_cast<size_t>(i)] = 0;
      f.forward(X.data());
      for (auto& c : X) c = std::exp(c);
      f.inverse(X.data());
      for (size_t i = 0; i < n; ++i) (*v)[i] = X[i].real();
    }
  }
  u.cfft.init(FxUnit::kFftN);
  u.parts = std::max(1, static_cast<int>((n + FxUnit::kPart - 1) / FxUnit::kPart));
  const size_t bins = FxUnit::kFftN / 2 + 1;
  u.work.assign(FxUnit::kFftN, cpx(0, 0));
  for (int c = 0; c < 2; ++c) {
    const auto& src = c == 0 ? rl : rr;
    u.irSpec[c].assign(static_cast<size_t>(u.parts) * bins, cpx(0, 0));
    for (int p = 0; p < u.parts; ++p) {
      std::fill(u.work.begin(), u.work.end(), cpx(0, 0));
      for (int i = 0; i < FxUnit::kPart; ++i) {
        const size_t j = static_cast<size_t>(p * FxUnit::kPart + i);
        if (j < n) u.work[static_cast<size_t>(i)] = cpx(src[j], 0);
      }
      u.cfft.forward(u.work.data());
      std::copy(u.work.begin(), u.work.begin() + static_cast<std::ptrdiff_t>(bins), u.irSpec[c].begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(p) * bins));
    }
    u.fdl[c].assign(static_cast<size_t>(u.parts) * bins, cpx(0, 0));
    u.inBuf[c].assign(FxUnit::kPart, 0.f);
    u.ovl[c].assign(FxUnit::kPart, 0.f);
    u.outBuf[c].assign(FxUnit::kPart, 0.f);
    u.irHead[c].assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(std::min(n, static_cast<size_t>(FxUnit::kPart))));
    u.hist[c].assign(FxUnit::kPart, 0.f);
  }
  u.hpos = 0;
  u.convReady = true;
}
}  // namespace

std::unique_ptr<FxBank> makeFxBank(const PluginState& s, const Aux* aux, double sr) {
  auto bank = std::make_unique<FxBank>();
  bank->sr = sr;
  for (int slot = 0; slot < kFxSlots; ++slot) {
    const int k = fxP(slot, X_Type);
    const int type = k < static_cast<int>(s.params.size()) ? std::clamp(static_cast<int>(std::lround(s.params[static_cast<size_t>(k)])), 0, FxTypeCount - 1) : 0;
    if (type == FxNone) continue;
    auto u = std::make_unique<FxUnit>();
    u->sr = sr;
    u->pool.assign(FxUnit::poolSize(sr), 0.f);
    u->bindLines();
    u->filterMem = std::make_unique<Filter>();
    u->filt = u->filterMem.get();
    if (type == FxConvolve) buildConv(*u, aux, slot, s);
    u->type = type;
    bank->unit[static_cast<size_t>(slot)] = std::move(u);
  }
  return bank;
}

// J-203c: the values Serum 2 uses for module parameters a preset does not store, measured on the
// source (a module with the parameter unset renders sample-identical to the module with this value).
bool fxSerumDefault(int type, int p, double& plain) {
  struct Def {
    int type;
    const char* key;
    double v;
  };
  static const Def kDefs[] = {
      {FxReverb, "kParamWet", 20},        {FxReverb, "kParamSize", 35},          {FxReverb, "kParamFeedback", 25},
      {FxReverb, "kParamFreq", 0},        {FxReverb, "kParamFreqB", 35},         {FxReverb, "kParamWidth", 20},
      {FxReverb, "kParamVintageScale", 70}, {FxReverb, "kParamType", 0},       {FxReverb, "kParamDelay", 31.25},
      {FxReverb, "kParamMode", 72},
      {FxDelay, "kParamWet", 30},         {FxDelay, "kParamFreq", 848.528},      {FxDelay, "kParamBW", 6.75},
      {FxDelay, "kParamFeedback", 40},    {FxDelay, "kParamMode", 0},            {FxDelay, "kParamOffsetL", 1},
      {FxDelay, "kParamOffsetR", 1},      {FxDelay, "kParamBeatSync", 1},
      {FxDistortion, "kParamWet", 100},   {FxDistortion, "kParamDrive", 25},     {FxDistortion, "kParamMode", 0},
      {FxDistortion, "kParamPrePost", 0}, {FxDistortion, "kParamFreq", 0.5},     {FxDistortion, "kParamLPHP", 0},     {FxDistortion, "kParamBW", 1.95},
      {FxComp, "kParamWet", 100},         {FxComp, "kParamThresh", 0.5},         {FxComp, "kParamRatio", 4},
      {FxComp, "kParamAttack", 90.091},   {FxComp, "kParamRelease", 90.091},     {FxComp, "kParamMakeup", 1},
      {FxComp, "kParamMultiband", 0},     {FxComp, "kParamRatio0", 1},           {FxComp, "kParamRatio1", 1},
      {FxComp, "kParamRatio2", 1},        {FxComp, "kParamRatioBelow0", 1},      {FxComp, "kParamRatioBelow1", 1},
      {FxComp, "kParamRatioBelow2", 1},   {FxComp, "kParamRatioBelow", 0},       {FxComp, "kParamXoverLow", 133.75},
      {FxComp, "kParamXoverHi", 2480.8},
      {FxFilter, "kParamWet", 100},       {FxFilter, "kParamFreq", 0.5},         {FxFilter, "kParamReso", 0},
      {FxFilter, "kParamDrive", 0},       {FxFilter, "kParamStereo", 50},        {FxFilter, "kParamVar", 0},
      {FxChorus, "kParamWet", 50},        {FxChorus, "kParamDelay", 5},          {FxChorus, "kParamDelay2", 0},
      {FxChorus, "kParamDepth", 26.25},   {FxChorus, "kParamFeedback", 9.5},     {FxChorus, "kParamFilt", 1000},
      {FxPhaser, "kParamWet", 100},       {FxPhaser, "kParamDepth", 50},         {FxPhaser, "kParamFreq", 600},
      {FxPhaser, "kParamWidth", 180},     {FxPhaser, "kParamNumPoles", 4},       {FxPhaser, "kParamDepth2", 0.5},
      {FxFlanger, "kParamWet", 100},      {FxFlanger, "kParamDepth", 100},       {FxFlanger, "kParamFeedback", 50},
      {FxFlanger, "kParamWidth", 180},
      {FxBode, "kParamWet", 100},         {FxBode, "kParamShift", 0},            {FxBode, "kParamRange", 22.3607},
      {FxBode, "kParamDelayTime", 0},     {FxBode, "kParamFeedback", 0},         {FxBode, "kParamOutputWidth", 100},
      {FxBode, "kParamOutputMix", 0},     {FxPhaser, "kParamFeedback", 80},      {FxConvolve, "kParamWet", 20},
      {FxChorus, "kParamRate", 0.078125}, {FxPhaser, "kParamRate", 0.078125},    {FxFlanger, "kParamRate", 0.078125},
      {FxUtility, "kParamHPF", 1},        {FxUtility, "kParamLPF", 20000},
  };
  const auto& d = fxParams(type);
  if (p < 0 || p >= static_cast<int>(d.size())) return false;
  for (const Def& e : kDefs)
    if (e.type == type && std::strcmp(e.key, d[static_cast<size_t>(p)].key) == 0) {
      plain = e.v;
      return true;
    }
  return false;
}

void fxReset(FxUnit& u) { u.clear(); }
float fxMeter(const FxUnit& u) { return u.meter; }

// ------------------------------------------------------------------ processing
void fxProcess(FxUnit& u, int type, const float* p, float* L, float* R, int n, const FxContext& c, const Aux* aux, int slot) {
  if (type != u.type) {
    u.clear();
    u.type = type;
  }
  const double sr = u.sr;
  auto P = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : fxPlain(type, i, p[i]);
  };
  auto N = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : static_cast<double>(p[i]);
  };
  const float wet = static_cast<float>(std::clamp(P("kParamWet") / 100.0, 0.0, 1.0));
  const float out = static_cast<float>(busGain(N("kParamLevelOut")));
  auto rateHz = [&](double norm, bool sync) {
    if (sync) return c.tempo / 60.0 / lfoBeats(norm);
    const int i = fxParamIndex(type, "kParamRate");
    return i < 0 ? 1.0 : fxPlain(type, i, norm);
  };
  float peak = 0;
  switch (type) {
    case FxDistortion: {
      // J-203d, measured on the source with held sines and saws (see plasma_fxdist_dsp.cpp for the
      // shapers): the module halves its output; the filter is a state-variable filter whose TYPE
      // knob fades low-pass -> band-pass (at 50 %, peak gain = Q) -> high-pass, cutoff on the voice
      // filter's law, Q = 0.71 e^(0.46 BW) (BW 0.075 / 1.95 / 7.5: Q 0.73 / 1.71 / 23); downsample
      // holds 1 + 96 d^2 samples at 48 kHz (d = drive 0..1; rates 9.6k / 1.92k / 495 Hz at 20 / 50 /
      // 100 %) and keeps the level; MIX m: wet sin^2(pi m / 2), dry 1 - wet^2 (dry -0.19 / -2.5 /
      // -11.3 dB at 25 / 50 / 75 %); the level knob acts after the mix.
      const int mode = std::clamp(static_cast<int>(std::lround(P("kParamMode"))), 0, 17);
      const float drive = static_cast<float>(std::clamp(P("kParamDrive") / 100.0, 0.0, 1.0));
      const int pp = static_cast<int>(std::lround(P("kParamPrePost")));
      const double fhz = std::clamp(cutoffHz(N("kParamFreq")), 5.0, sr * 0.45);
      const double q = 0.71 * std::exp(0.46 * std::clamp(P("kParamBW"), 0.0, 10.0));
      const double t = std::clamp(P("kParamLPHP") / 100.0, 0.0, 1.0);
      const float wl = static_cast<float>(t <= 0.5 ? 1 - 2 * t : 0), wb = static_cast<float>(t <= 0.5 ? 2 * t : 2 - 2 * t),
                  wh = static_cast<float>(t <= 0.5 ? 0 : 2 * t - 1);
      const double g = std::tan(3.14159265358979 * fhz / sr), k = 1.0 / q;
      const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
      auto svf = [&](int ch, float x) {  // TPT state-variable filter (Zavalishin / Simper form)
        double& s1 = u.dsv[ch][0];
        double& s2 = u.dsv[ch][1];
        const double v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2 * v1 - s1, s2 = 2 * v2 - s2;
        return static_cast<float>(wl * v2 + wb * v1 + wh * (x - k * v1 - v2));
      };
      const float* ca = aux ? aux->shapeA[static_cast<size_t>(slot)].data() : nullptr;
      const float* cb = aux ? aux->shapeB[static_cast<size_t>(slot)].data() : nullptr;
      const double hold = (1.0 + 96.0 * static_cast<double>(drive) * drive) * sr / 48000.0;
      const double dsStep = 1.0 / std::max(1.0, hold);
      const float wetG = static_cast<float>(std::pow(std::sin(1.5707963267948966 * wet), 2.0)), dryG = 1.f - wetG * wetG;
      const float odGain = static_cast<float>(0.68 * std::pow(74.0, static_cast<double>(drive)));
      constexpr float kFull = 1.657f;  // the source's full scale in Plasma's units (where its clippers sit)
      // X-Shaper (J-203k, fitted and verified against the hosted plugin's output with drawn straight and bent
      // curves at drive 0 / 50 / 100 %): the drive crossfades from the first curve (drive 0) to the second
      // (drive 100), it is not a gain. The symmetric mode reads the curve over the input's magnitude (0..1 of
      // full scale) and keeps the sign: the curve C is read at the square root of the magnitude and squared,
      // y = C(sqrt|x|)^2 (a straight rising line is the identity, a falling one stays quiet near the top); the
      // asymmetric mode reads it over -1..1.
      auto look = [](const float* c, float pos) {
        const float s = std::clamp(pos, 0.f, 1.f) * 256.f;
        const int j = std::min(255, static_cast<int>(s));
        return c[j] + (c[j + 1] - c[j]) * (s - static_cast<float>(j));
      };
      auto xcurve = [&](float v, bool asym) {
        if (!ca) return v;
        const float* b = cb ? cb : ca;
        if (asym) {
          const float pos = (v + 1.f) * 0.5f;
          return (look(ca, pos) + (look(b, pos) - look(ca, pos)) * drive) * 2.f - 1.f;
        }
        const float a = std::sqrt(std::fabs(v)), ya = look(ca, a) * look(ca, a), y = ya + (look(b, a) * look(b, a) - ya) * drive;
        return v < 0 ? -y : y;
      };
      for (int i = 0; i < n; ++i) {
        float io[2] = {L[i], R[i]}, x[2] = {L[i], R[i]};
        if (pp == 1) x[0] = svf(0, x[0]), x[1] = svf(1, x[1]);
        if (mode == 8) {  // downsample: sample and hold, level kept
          u.dsPh += dsStep;
          if (u.dsPh >= 1 || dsStep >= 1) u.dsPh -= std::floor(u.dsPh), u.dsHold[0] = x[0], u.dsHold[1] = x[1];
          x[0] = u.dsHold[0], x[1] = u.dsHold[1];
        } else {
          for (int ch = 0; ch < 2; ++ch) {
            if (mode == 11 || mode == 12) {  // X-Shaper
              const float v = std::clamp(x[ch] / kFull, -1.f, 1.f);
              x[ch] = 0.48f * kFull * xcurve(v, mode == 12);
            } else if (mode == 16) {  // overdrive: gain 0.68 * 74^d (-3.4 .. +34 dB), then a hard knee at 2.18
              const float v = x[ch] * odGain, a = std::fabs(v);
              constexpr float kTop = 2.18f, kKnee = 0.3f;
              const float y = a < kTop - kKnee ? a : a > kTop + kKnee ? kTop : a - (a - kTop + kKnee) * (a - kTop + kKnee) / (4 * kKnee);
              x[ch] = 0.5f * (v < 0 ? -y : y);
            } else {
              x[ch] = fxDistTable(mode, drive, x[ch]);
            }
          }
        }
        if (pp == 2) x[0] = svf(0, x[0]), x[1] = svf(1, x[1]);
        L[i] = (io[0] * dryG + x[0] * wetG) * out;
        R[i] = (io[1] * dryG + x[1] * wetG) * out;
      }
      break;
    }
    case FxFlanger:
    case FxPhaser:
    case FxChorus: {
      const bool sync = N("kParamBeatSync") >= 0.5;
      const double hz = rateHz(N("kParamRate"), sync);
      const double inc = hz / sr;
      const float depth = static_cast<float>(type == FxChorus ? P("kParamDepth") : P("kParamDepth") / 100.0);
      const float fb = static_cast<float>(P("kParamFeedback") / 100.0);
      const double phOff = (type == FxChorus ? 90.0 : P("kParamWidth")) / 360.0;
      // J-203d, measured on the source with a held saw and the modulation stopped (depth 0): all three share one
      // mix law. With v = the mix (chorus) or half the mix (flanger, phaser) and x = 3v^2 - 2v^3, the output is
      // (1 - x^2) * dry + x * wet: chorus mix 25 / 50 / 75 / 100 gives dry 0.98 / 0.75 / 0.27 / 0 and wet
      // 0.15 / 0.5 / 0.85 / 1; flanger and phaser at mix 100 give dry 0.75 and wet 0.5, at 50 dry 0.98, wet 0.15
      const double mv = type == FxChorus ? wet : wet * 0.5, mx = mv * mv * (3 - 2 * mv);
      const float gDry = static_cast<float>(1 - mx * mx), gWet = static_cast<float>(mx);
      if (type == FxChorus) {
        // taps sit 0.122 ms past their delay settings (5.122 ms and 0.122 ms at the unset 5 and 0)
        const double d1 = (P("kParamDelay") + 0.122) * sr / 1000.0, d2 = (P("kParamDelay2") + 0.122) * sr / 1000.0, dep = depth * sr / 1000.0;
        const double fc = P("kParamFilt");
        const bool hp = P("kParamFiltMode") >= 0.5;
        u.lpw[0].lp(sr, fc), u.lpw[1].lp(sr, fc);
        for (int i = 0; i < n; ++i) {
          u.lfoPh += inc;
          if (u.lfoPh >= 1) u.lfoPh -= 1;
          float io[2] = {L[i], R[i]};
          for (int ch = 0; ch < 2; ++ch) {
            const double m1 = 0.5 + 0.5 * triLfo(u.lfoPh + ch * 0.25), m2 = 0.5 + 0.5 * triLfo(u.lfoPh + 0.5 + ch * 0.25);
            const float a = u.ml[ch].tap(static_cast<float>(d1 + m1 * dep));
            const float b = u.ml[ch].tap(static_cast<float>(d2 + m2 * dep));
            float w = (a + b) * 0.5f;
            const float f = u.lpw[ch].run(w);
            w = hp ? w - f : f;
            u.ml[ch].push(io[ch] + w * fb * 0.9f);
            io[ch] = io[ch] * gDry + w * gWet;
          }
          L[i] = io[0] * out, R[i] = io[1] * out;
        }
      } else if (type == FxFlanger) {
        // shortest delay 0.136 ms (first notch 3.7 kHz at depth 0); feedback is positive, 0.98 * the setting
        // (feedback 50: +4.8 dB at the comb peaks' low end, -8 dB notches). J-203k, delay tracked over a held note
        // (feedback 0): the sweep spans 3.5 ms at full depth, left and right in opposite phase
        const double base = 0.000136 * sr, dep = 0.0035 * sr * depth;
        for (int i = 0; i < n; ++i) {
          u.lfoPh += inc;
          if (u.lfoPh >= 1) u.lfoPh -= 1;
          float io[2] = {L[i], R[i]};
          for (int ch = 0; ch < 2; ++ch) {
            const double m = 0.5 + 0.5 * triLfo(u.lfoPh + ch * phOff);
            const float d = u.ml[ch].tap(static_cast<float>(base + m * dep));
            u.ml[ch].push(std::clamp(io[ch] + d * fb * 0.98f, -4.f, 4.f));
            io[ch] = io[ch] * gDry + d * gWet;
          }
          L[i] = io[0] * out, R[i] = io[1] * out;
        }
      } else {  // phaser
        const double fc = P("kParamFreq");
        const int stages = std::clamp(static_cast<int>(P("kParamNumPoles")) * 2, 2, 32);
        // J-203d, measured with the sweep stopped: 2 * poles first-order allpasses (one notch per pole). With spread
        // (DEPTH 2) 0 they all sit at 1.882 * FREQ; spread 0.5 lays them out about geometrically, around a centre
        // and step that depend on the pole count (fits: 2 poles 3.62 * FREQ, 1.253 apart; 4 poles 2.15, 1.278,
        // FREQ 300 / 600 / 1200 giving 656 / 1292 / 2521 Hz; 6 poles 2.26, 1.140). Feedback is positive, 0.97 * it.
        const double dep2 = std::clamp(P("kParamDepth2"), 0.0, 1.0) * 2.0;
        const double np = std::clamp(stages * 0.5, 2.0, 6.0), t4 = np <= 4 ? (np - 2) / 2 : (np - 4) / 2;
        const double cN = np <= 4 ? 3.62 + (2.153 - 3.62) * t4 : 2.153 + (2.257 - 2.153) * t4;
        const double sN = np <= 4 ? 1.2526 + (1.278 - 1.2526) * t4 : 1.278 + (1.14 - 1.278) * t4;
        const double spr = 1.0 + (sN - 1.0) * dep2, ctr = 1.882 * std::pow(cN / 1.882, dep2);
        for (int i = 0; i < n; ++i) {
          u.lfoPh += inc;
          if (u.lfoPh >= 1) u.lfoPh -= 1;
          float io[2] = {L[i], R[i]};
          for (int ch = 0; ch < 2; ++ch) {
            const double m = triLfo(u.lfoPh + ch * phOff);
            // J-203h, measured (lowest notch tracked over time, feedback 0, FREQ 600): the sweep is linear in Hz and
            // only upward from the resting notches, by 15.2 x depth (depth 100: 0.87 -> 14 kHz, 50: -> 7.5 kHz)
            const double f = fc * ctr * (1.0 + 15.2 * depth * (0.5 + 0.5 * m));
            float x = io[ch] + u.phFb[ch] * fb * 0.97f;
            double fs = f * std::pow(spr, -0.5 * (stages - 1));
            for (int s = 0; s < stages; ++s, fs *= spr) {
              const double w = std::tan(3.14159265358979 * std::clamp(fs, 10.0, sr * 0.45) / sr);
              x = u.ph[ch][s].run(x, static_cast<float>((w - 1) / (w + 1)));
            }
            u.phFb[ch] = std::clamp(x, -4.f, 4.f);
            io[ch] = io[ch] * gDry + x * gWet;
          }
          L[i] = io[0] * out, R[i] = io[1] * out;
        }
      }
      break;
    }
    case FxDelay: {
      const bool sync = N("kParamBeatSync") >= 0.5;
      const bool link = N("kParamLink") >= 0.5;
      const int mode = static_cast<int>(P("kParamMode"));
      auto secs = [&](double v) {
        if (sync) {
          static const double beats[] = {0.0625, 0.125, 1.0 / 6, 0.1875, 0.25, 1.0 / 3, 0.375, 0.5, 2.0 / 3, 0.75, 1.0, 4.0 / 3, 1.5, 2.0, 3.0, 4.0};
          const int i = std::clamp(static_cast<int>(std::lround(v * 15)), 0, 15);
          return beats[i] * 60.0 / std::max(20.0, c.tempo);
        }
        return 0.001 + 1.999 * v * v;
      };
      const double tl = secs(N("kParamTimeL")) * P("kParamOffsetL");
      const double tr = (link ? secs(N("kParamTimeL")) : secs(N("kParamTimeR"))) * P("kParamOffsetR");
      const float fb = static_cast<float>(std::clamp(P("kParamFeedback") / 100.0, 0.0, 1.0));
      const bool hq = N("kParamHQ") >= 0.5;
      // J-201: the echo filter is a band around FREQ whose width follows Q (octaves): a one-pole
      // low cut and a two-pole high cut, unity in the band, so feedback below 100 % always decays
      // (it peaked at +5 dB before and could run away). Measured on the source with sines
      // around a 1 kHz centre at widths 1, 3 and 5.
      const double f0 = P("kParamFreq"), bw = std::max(0.0, P("kParamBW"));
      u.dbp.set(4, sr, f0 * std::exp2(0.69 + 0.5 * bw), 0.7071, 0);
      for (auto& h : u.dhp) h.lp(sr, f0 * std::exp2(-(0.69 + 0.75 * bw)));
      // dry / echo balance of the mix knob (measured: 25 % keeps the full dry signal, 50 % is
      // -3 dB dry / -6 dB echo, 75 % -12 dB / -2 dB, 100 % echo only)
      static const float kDry[5] = {1.f, 1.f, 0.707f, 0.25f, 0.f}, kWet[5] = {0.f, 0.141f, 0.5f, 0.79f, 1.f};
      const float wq = wet * 4.f;
      const int wi = std::min(3, static_cast<int>(wq));
      const float wf = wq - static_cast<float>(wi);
      const float gDry = kDry[wi] + (kDry[wi + 1] - kDry[wi]) * wf, gWet = kWet[wi] + (kWet[wi + 1] - kWet[wi]) * wf;
      const float dL = static_cast<float>(std::min(tl * sr, static_cast<double>(u.dl[0].n - 8)));
      const float dR = static_cast<float>(std::min(tr * sr, static_cast<double>(u.dl[1].n - 8)));
      for (int i = 0; i < n; ++i) {
        const float a = hq ? u.dl[0].tapCubic(dL) : u.dl[0].tap(dL);
        const float b = hq ? u.dl[1].tapCubic(dR) : u.dl[1].tap(dR);
        const float fa = u.dbp.run(0, a - u.dhp[0].run(a)), fbv = u.dbp.run(1, b - u.dhp[1].run(b));
        if (mode == 1) {  // ping-pong: mono input into L, L's echo into R, R's echo back into L
          u.dl[0].push((L[i] + R[i]) * 0.5f + fbv * fb);
          u.dl[1].push(fa);
        } else if (mode == 2) {  // tap -> delay: L tap feeds the R delay line
          u.dl[0].push(L[i] + fa * fb * 0.5f);
          u.dl[1].push(R[i] * 0.5f + fa + fbv * fb);
        } else {
          u.dl[0].push(L[i] + fa * fb);
          u.dl[1].push(R[i] + fbv * fb);
        }
        L[i] = (L[i] * gDry + fa * gWet) * out;
        R[i] = (R[i] * gDry + fbv * gWet) * out;
      }
      break;
    }
    case FxComp: {
      const bool multi = N("kParamMultiband") >= 0.5;
      // J-203c, measured on the source with a saw at -18.8 dB RMS: the limited level is 60 log10(1 - t)
      // - 2.1 dB RMS (-26.1 / -38.1 / -62.0 at t = 0.6 / 0.75 / 0.9) and bottoms out near -122 at t = 1.
      // Against this detector (peak, gain smoothed in dB) that is a threshold of 60 log10(1 - t) + 8.4.
      // Release time constant = release / 4. Ratios from 100 up act as a limiter that catches at once.
      // Makeup below 1 does nothing; in limiter mode it adds 5.8 (1 - m^-1.6) dB on top (makeup 1.5 / 2
      // / 3 / 4 lift the limited level by 6.2 / 9.8 / 14.3 / 17.2 dB). Latency compensation: 6 ms.
      const double thrDb = 60.0 * std::log10(std::max(0.01, 1.0 - P("kParamThresh"))) + kCompThrOfs;
      const double ratio = P("kParamRatio");
      const bool limiter = ratio >= 100;
      const double atk = P("kParamAttack") / 1000.0, rel = P("kParamRelease") / 1000.0;
      const double mk = std::max(1.0, P("kParamMakeup"));
      const float makeup = static_cast<float>(limiter && !multi ? mk * dbToGainF(5.8 * (1.0 - std::pow(mk, -1.6))) : mk);
      const float aA = limiter ? 0.f : static_cast<float>(std::exp(-1.0 / std::max(1.0, atk * kCompAtkScale * sr))),
                  aR = static_cast<float>(std::exp(-1.0 / std::max(1.0, rel * 0.25 * sr)));
      const bool look = N("kParamLimiterLatencyComp") >= 0.5;
      const int lookN = static_cast<int>(std::lround(sr * 0.00598));
      // e holds the smoothed gain change in dB (<= 0 for reduction, > 0 for upward gain)
      auto gain = [&](float& e, float x, double tdb, double rAbove, double rBelow, double dead) {
        const double db = 20.0 * std::log10(std::max(1e-6f, x));
        double g = 0;
        if (db > tdb) g = (tdb - db) * (1.0 - 1.0 / std::max(1.0, rAbove));
        else if (db < tdb - dead && db > -80) g = (tdb - dead - db) * (1.0 - 1.0 / std::max(1.0, rBelow)) * 0.5;  // upward
        g = std::clamp(g, -150.0, 24.0);
        const float tg = static_cast<float>(g);
        e = tg < e ? tg + (e - tg) * aA : tg + (e - tg) * aR;
        return static_cast<float>(std::pow(10.0, static_cast<double>(e) / 20.0));
      };
      if (!multi) {
        for (int i = 0; i < n; ++i) {
          const float det = std::max(std::fabs(L[i]), std::fabs(R[i]));
          const float g = gain(u.env[0][0], det, thrDb, limiter ? 1e9 : ratio, 1, 999);
          float l = L[i], r = R[i];
          if (look) {
            u.look[0].push(l), u.look[1].push(r);
            l = u.look[0].tapInt(lookN), r = u.look[1].tapInt(lookN);
          }
          float yl = l * g * makeup, yr = r * g * makeup;
          L[i] = (l + (yl - l) * wet) * out;
          R[i] = (r + (yr - r) * wet) * out;
          u.gr[0] = g;
        }
      } else {
        // J-203d, measured on the source with a held 32.7 Hz sine (deep in the low band; levels are sine RMS
        // in dB of the source's output, Plasma's units read 9.04 dB higher). Downward threshold T vs t:
        // -11.8 / -24.9 / -37.2 / -51.7 / -79.4 / -111.5 at 0.25 / 0.4 / 0.5 / 0.6 / 0.75 / 0.9, piecewise
        // linear in log10(1 - t). Above T the gain falls by r (1 - 1/G) dB per dB (G = global ratio: 1.5 /
        // 4 / 10 measured as 1.5 / 3.7 / 7.5; r = the band's ratio knob, unset 1, 0.5 halves it, 0 = off),
        // and the total change saturates near 40 dB (x / (1 + (x/40)^8)^(1/8): t 0.75 / 0.9 at full scale
        // -39.5 / -40.0). Upward, only when the global BELOW b is above 0: below Tu the gain rises by
        // b f(rb) dB per dB (b 0.25 / 0.5 / 0.75 / 1: 0.25 / 0.5 / 0.73 / 0.99), f = 2 rb - 1
        // of the band's BELOW knob rb (unset 1; 0.5 = flat, 0 = expands);
        // Tu = T - 2.1 - 0.357 D - 0.0306 D^2 (D = deadband: -39.3 / -43.3 / -50.3 at 0 / 7 / 14 with
        // T -37.2). A band's THRESH u scales both by (2 - u)^0.63 (u 0.5 / 1.5: -48.8 / -24.8). Band gains and
        // makeup are plain gains; the module has a one-pole low cut near 11 Hz (-0.5 dB at 32.7 Hz) and
        // its band sum is otherwise flat.
        u.xo1.set(sr, P("kParamXoverLow"));
        u.xo2.set(sr, std::max(P("kParamXoverLow") * 1.5, P("kParamXoverHi")));
        u.dhp[0].lp(sr, 11.4), u.dhp[1].lp(sr, 11.4);
        const double tk = std::clamp(P("kParamThresh"), 0.0, 1.0);
        double tRms;
        {
          static const double lx[] = {0, -0.1249, -0.2218, -0.3010, -0.3979, -0.6021, -1.0}, ty[] = {5.1, -11.8, -24.9, -37.2, -51.7, -79.4, -111.5};
          const double lt = std::log10(std::max(1e-4, 1.0 - tk));
          if (lt <= lx[6]) {
            tRms = ty[6] + 80.6 * (lt - lx[6]);
          } else {
            int j = 1;
            while (j < 6 && lt < lx[j]) ++j;
            tRms = ty[j - 1] + (ty[j] - ty[j - 1]) * (lt - lx[j - 1]) / (lx[j] - lx[j - 1]);
          }
        }
        const double slopeG = 1.0 - 1.0 / std::max(1.0, ratio);
        const double upB = std::clamp(P("kParamRatioBelow"), 0.0, 1.0);
        static const char* const kUd[3] = {"kParamThreshUD0", "kParamThreshUD1", "kParamThreshUD2"};
        static const char* const kDz[3] = {"kParamDeadband0", "kParamDeadband1", "kParamDeadband2"};
        static const char* const kRa[3] = {"kParamRatio0", "kParamRatio1", "kParamRatio2"};
        static const char* const kRb[3] = {"kParamRatioBelow0", "kParamRatioBelow1", "kParamRatioBelow2"};
        static const char* const kBg[3] = {"kParamGain0", "kParamGain1", "kParamGain2"};
        const float aAm = static_cast<float>(std::exp(-1.0 / (0.0005 * sr)));
        const float aRms = static_cast<float>(std::exp(-1.0 / (kMbRmsMs * 0.001 * sr)));
        const double kMbOfs = kMbOfsDb;  // source sine RMS dB -> this detector's reading in Plasma's units
        double mT[3], mTu[3], mS[3], mU[3];
        float bg[3];
        for (int b = 0; b < 3; ++b) {
          const double tb = tRms * std::pow(std::max(0.0, 2.0 - P(kUd[b]) / 100.0), 0.63);
          const double dz = std::max(0.0, P(kDz[b]));
          const double r = std::clamp(P(kRa[b]), 0.0, 1.0), rb = std::clamp(P(kRb[b]), 0.0, 1.0);
          mT[b] = tb + kMbOfs;
          mTu[b] = tb - 2.1 - 0.357 * dz - 0.0306 * dz * dz + kMbOfs;
          mS[b] = r * slopeG;
          mU[b] = upB * (2.0 * rb - 1.0);
          bg[b] = static_cast<float>(dbToGainF(P(kBg[b])));
        }
        // the band level is a short running mean of the power (two one-pole stages, so a held tone reads
        // its RMS and a note onset is caught within about 10 ms) that falls with the RELEASE time; the
        // source ignores ATTACK in this mode (10 / 100 / 1000 ms render alike). The gain is lightly smoothed.
        auto mgain = [&](float* e, float& gs, float pw, int b) {
          e[0] = pw + (e[0] - pw) * aRms;
          e[1] = e[0] + (e[1] - e[0]) * aRms;
          e[2] = e[1] > e[2] ? e[1] : e[1] + (e[2] - e[1]) * aR;
          const double db = 10.0 * std::log10(std::max(1e-12f, e[2]));
          double g = 0;
          if (db > mT[b]) g = (mT[b] - db) * mS[b];
          if (mU[b] != 0 && db > -110) {  // upward (or expanding), soft knee 6 dB wide around its threshold
            constexpr double kW = 6;
            const double over = mTu[b] - db;
            const double up = over >= kW / 2 ? over : over > -kW / 2 ? (over + kW / 2) * (over + kW / 2) / (2 * kW) : 0;
            g += up * mU[b];
          }
          const double q = g / 40.0, q2 = q * q;
          g /= std::pow(1.0 + q2 * q2 * q2 * q2, 1.0 / 8.0);  // saturates near +-40 dB
          const float tg = static_cast<float>(g);
          gs = tg + (gs - tg) * aAm;
          return static_cast<float>(std::pow(10.0, static_cast<double>(gs) / 20.0));
        };
        for (int i = 0; i < n; ++i) {
          float band[3][2];
          for (int ch = 0; ch < 2; ++ch) {
            double lo, rest, mid, hi;
            const float xin = ch ? R[i] : L[i];
            u.xo1.split(ch, xin - u.dhp[ch].run(xin), lo, rest);
            u.xo2.split(ch, rest, mid, hi);
            band[0][ch] = static_cast<float>(lo), band[1][ch] = static_cast<float>(mid), band[2][ch] = static_cast<float>(hi);
          }
          float yl = 0, yr = 0, dl = 0, dr = 0;
          for (int b = 0; b < 3; ++b) {
            const float det = std::max(band[b][0] * band[b][0], band[b][1] * band[b][1]);
            const float g = mgain(u.mbEnv[b], u.env[b][0], det, b) * bg[b];
            yl += band[b][0] * g, yr += band[b][1] * g;
            dl += band[b][0], dr += band[b][1];
            u.gr[b] = g;
          }
          yl *= makeup, yr *= makeup;
          // J-203f, measured (ratio 1, MIX 50 %): the source stays flat, so the dry part is the bands'
          // plain sum (the crossovers' phase), not the input, which would cancel around the crossovers
          L[i] = (dl + (yl - dl) * wet) * out;
          R[i] = (dr + (yr - dr) * wet) * out;
        }
      }
      break;
    }
    case FxReverb: {
      // J-203c: the decay of each type follows tables measured on the source (Schroeder RT60 in the
      // 125 Hz / 1 kHz / 8 kHz octaves of a held note's tail; wet 100, other values at their defaults).
      // Which knobs set the decay depends on the type: Plate = size (feedback darkens the tail), Hall =
      // size and the delay knob, Vintage = size, the A diffusion and the delay knob, Abyss = size and
      // shape, Space = size and the delay knob. Plate keeps the width knob; Hall, Vintage and Abyss are
      // wide whatever its value; Space is mono. Only the Plate waits before its tail (32 .. 128 ms with
      // size). Mix: dry 1 - m^2, wet m (wet 20 / 50: dry -0.2 / -2.6 dB on the source).
      const int rtype = std::clamp(static_cast<int>(P("kParamType")), 0, 4);
      const double sz = P("kParamSize"), dk = P("kParamDelay"), fbk = P("kParamFeedback");
      auto tab = [](const double* xs, const double* ys, int n, double x) {
        if (x <= xs[0]) return ys[0];
        for (int i = 1; i < n; ++i)
          if (x <= xs[i]) return ys[i - 1] * std::pow(ys[i] / ys[i - 1], (x - xs[i - 1]) / (xs[i] - xs[i - 1]));
        return ys[n - 1];
      };
      auto gLin = [](const double* xs, const double* ys, int n, double x) {  // linear in dB
        if (x <= xs[0]) return ys[0];
        for (int i = 1; i < n; ++i)
          if (x <= xs[i]) return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
        return ys[n - 1];
      };
      // wet level: the source keeps a hall's wet energy nearly level as it grows; fitted per type (dB)
      double rtM = 1, rtLo = 1, rtHi = 1, preS = 0, wetG = 1, width = 1;
      switch (rtype) {
        case 0: {  // plate
          static const double xs[] = {0, 15, 35, 50, 60, 100}, ys[] = {0.43, 0.77, 2.77, 6.96, 10.2, 10.5};
          static const double fx[] = {0, 25, 40, 50, 60, 75, 90, 100}, fm[] = {1.0, 1.0, 0.97, 0.95, 0.92, 0.77, 0.42, 0.18},
                              fh[] = {1.06, 0.9, 0.66, 0.54, 0.38, 0.28, 0.22, 0.21};
          const double t = tab(xs, ys, 6, sz);
          // J-203f: low / high octave decay against the mid one per size (noise bursts, Schroeder RT60)
          static const double rx[] = {0, 35, 70, 100}, rl[] = {1.04, 1.12, 0.98, 1.0}, rh[] = {1.27, 0.77, 0.55, 0.57};
          rtM = t * tab(fx, fm, 8, fbk), rtHi = t * tab(rx, rh, 4, sz) * tab(fx, fh, 8, fbk) / 0.9, rtLo = t * tab(rx, rl, 4, sz);
          static const double ox[] = {0, 35, 50, 60, 70}, oy[] = {0.0, 0.0, 0.02, 0.045, 0.06};
          preS = gLin(ox, oy, 5, sz);
          static const double gx[] = {0, 35, 70, 100}, gy[] = {-5.8, -5.6, -5.3, -4.7};
          width = P("kParamWidth") / 100.0, wetG = dbToGainF(gLin(gx, gy, 4, sz));
          break;
        }
        case 1: {  // hall: bilinear over size x delay knob (log RT)
          static const double sx[] = {0, 35, 70, 100}, dx[] = {0, 31.25, 100, 250};
          // J-203f: each octave (125 Hz, 1 kHz, 8 kHz) has its own size x delay table, measured with noise bursts:
          // the delay knob lengthens the lows far more than the highs on small halls
          static const double tl[4][4] = {{0.35, 2.30, 13.84, 9.20}, {1.12, 3.71, 15.10, 8.34}, {4.0, 4.27, 15.26, 8.22}, {11.25, 11.26, 13.54, 7.70}};
          static const double tm[4][4] = {{0.51, 1.74, 3.92, 4.88}, {1.25, 2.51, 8.50, 14.16}, {3.94, 4.22, 12.90, 12.13}, {11.01, 11.04, 13.70, 8.99}};
          static const double th[4][4] = {{0.50, 0.86, 1.15, 1.23}, {1.22, 1.92, 3.47, 4.16}, {4.0, 4.19, 8.58, 12.77}, {11.10, 11.12, 13.30, 12.27}};
          auto idx = [](const double* xs, double x, int& i, double& f) {
            i = 0;
            while (i < 2 && x > xs[i + 1]) ++i;
            f = std::clamp((x - xs[i]) / (xs[i + 1] - xs[i]), 0.0, 1.0);
          };
          int i, j;
          double fi, fj;
          idx(sx, sz, i, fi), idx(dx, dk, j, fj);
          auto bil = [&](const double (&t)[4][4]) {
            const double l00 = std::log(t[i][j]), l01 = std::log(t[i][j + 1]), l10 = std::log(t[i + 1][j]), l11 = std::log(t[i + 1][j + 1]);
            return std::exp((l00 * (1 - fj) + l01 * fj) * (1 - fi) + (l10 * (1 - fj) + l11 * fj) * fi);
          };
          rtM = bil(tm), rtLo = bil(tl), rtHi = bil(th);
          // wet energy of a noise burst against the dry one, per size x delay (dB, J-203f)
          static const double tg[4][4] = {{5.8, 4.4, 4.1, 5.2}, {2.0, 0.1, -1.4, -0.5}, {-3.9, -4.0, -6.1, -5.2}, {-8.5, -8.5, -8.7, -6.1}};
          const double gdb = (tg[i][j] * (1 - fj) + tg[i][j + 1] * fj) * (1 - fi) + (tg[i + 1][j] * (1 - fj) + tg[i + 1][j + 1] * fj) * fi;
          width = 0.85, wetG = dbToGainF(gdb);
          break;
        }
        case 2: {  // vintage
          static const double xs[] = {0, 15, 35, 50, 70, 85, 100}, ys[] = {0.69, 0.9, 1.41, 2.0, 3.66, 7.15, 11.0};
          static const double vx[] = {0, 25, 50, 70, 85, 100}, vy[] = {0.47, 0.46, 1.1, 1.0, 1.36, 2.14};
          rtM = tab(xs, ys, 7, sz) * tab(vx, vy, 6, P("kParamVintageScale")) * (dk < 15 ? 0.5 + dk / 30.0 : 1.0);
          static const double rx[] = {0, 35, 70, 100}, rl[] = {0.97, 1.01, 1.13, 0.6}, rh[] = {0.97, 0.59, 0.41, 0.23};
          rtHi = rtM * tab(rx, rh, 4, sz), rtLo = rtM * tab(rx, rl, 4, sz);
          static const double gx[] = {0, 35, 70, 100}, gy[] = {3.3, 1.4, -1.2, 2.0};
          width = 1.3, wetG = dbToGainF(gLin(gx, gy, 4, sz)), preS = 0.12;
          break;
        }
        case 3: {  // abyss
          static const double xs[] = {0, 15, 35, 50, 70, 85, 100}, ys[] = {5.06, 7.0, 10.6, 12.5, 15.3, 14.5, 13.6};
          static const double mx[] = {0, 10, 25, 35, 50, 72, 100}, my[] = {1.4, 1.65, 0.78, 0.78, 0.4, 1.0, 0.07};
          static const double dx[] = {0, 31.25, 60, 150, 250}, dy[] = {1.0, 1.0, 1.17, 2.3, 2.9};
          rtM = tab(xs, ys, 7, sz) * tab(mx, my, 7, P("kParamMode")) * tab(dx, dy, 5, dk);
          static const double rx[] = {0, 35, 70, 100}, rl[] = {0.75, 1.02, 0.85, 0.54}, rh[] = {0.61, 0.69, 0.72, 0.86};
          rtHi = rtM * tab(rx, rh, 4, sz), rtLo = rtM * tab(rx, rl, 4, sz);
          static const double gx[] = {0, 35, 70, 100}, gy[] = {-5.7, -8.25, -8.8, -7.9};
          width = 0.7, wetG = dbToGainF(gLin(gx, gy, 4, sz));
          break;
        }
        default: {  // space
          static const double xs[] = {0, 35, 50, 70, 85, 100}, ys[] = {0.61, 0.88, 1.12, 1.54, 1.9, 2.29};
          static const double dx[] = {0, 15, 31.25, 60, 150, 250}, dy[] = {0.65, 0.85, 1.0, 1.45, 3.3, 12.0};
          rtM = tab(xs, ys, 6, sz) * tab(dx, dy, 6, dk);
          static const double rx[] = {0, 35, 70, 100}, rl[] = {1.02, 1.03, 1.18, 1.0}, rh[] = {0.84, 0.73, 0.75, 0.67};
          rtHi = rtM * tab(rx, rh, 4, sz), rtLo = rtM * tab(rx, rl, 4, sz);
          static const double gx[] = {0, 35, 70, 100}, gy[] = {8.4, 7.3, 4.9, 3.8};
          width = 0, wetG = dbToGainF(gLin(gx, gy, 4, sz) - 0.04 * (dk - 31.25));
          break;
        }
      }
      if (rtype == 0 && N("kParamPreDelayBeatSync") >= 0.5) preS += P("kParamPreDelay") * 60.0 / std::max(20.0, c.tempo);
      const double size = rtype == 0 ? 0.6 + 0.8 * std::clamp(sz / 100.0, 0.0, 1.0) : 0.5;
      // J-203f: the wet tone (white noise through the source, 1/3-octave transfer of a held note at mix 100) as
      // a shelf curve per type: the curve at the unset cuts plus the LOW CUT and HIGH CUT changes, each
      // measured along its own knob (plasma_fx_reverb_tone.inc)
      {
        float db[ShelfEq::kN + 1];
        reverbTone(rtype, P("kParamFreq"), P("kParamFreqB"), db);
        u.rvEq.set(sr, db);
      }
      if (rtype == 2) {
        // J-203h, measured (1/3-octave transfer, mix 100): the vintage wet ends at 16 kHz like a reduced-rate
        // design, -14.4 dB in the 16 kHz band and -43 dB at 20 kHz (the hall: -2.1 / -2.5 dB): an 8-pole
        // Butterworth low-pass at 16 kHz
        static const double kBwQ[4] = {0.5098, 0.6013, 0.9000, 2.5629};
        for (int k = 0; k < 4; ++k) u.rvBw[k].set(4, sr, std::min(16000.0, sr * 0.45), kBwQ[k], 0);
      }
      const double diffA = 0.7, diffB = 0.7;
      const double spinR = 0.05 + 3.0 * P("kParamSpinRate") / 100.0, spinD = P("kParamSpinDepth") / 100.0 * (rtype == 4 ? 2.5 : 1.0);
      static const double base[5][8] = {
          {0.0297, 0.0371, 0.0411, 0.0437, 0.0533, 0.0617, 0.0713, 0.0797},  // plate: dense, short
          {0.0503, 0.0617, 0.0731, 0.0853, 0.0967, 0.1093, 0.1201, 0.1327},  // hall
          {0.0411, 0.0529, 0.0613, 0.0703, 0.0811, 0.0919, 0.1013, 0.1129},  // vintage
          {0.0897, 0.1013, 0.1201, 0.1327, 0.1499, 0.1621, 0.1777, 0.1913},  // abyss
          {0.0731, 0.0853, 0.0997, 0.1129, 0.1277, 0.1409, 0.1553, 0.1697},  // space
      };
      const double kRvKLo = std::tan(3.14159265358979 * 300.0 / sr), kRvKHi = std::tan(3.14159265358979 * std::min(3500.0, sr * 0.4) / sr);
      double len[8], g[8], gL[8], gH[8];
      for (int k = 0; k < 8; ++k) {
        len[k] = std::min(base[rtype][k] * size * sr, static_cast<double>(u.rv[k].n - 16));
        const double ts = len[k] / sr;
        g[k] = std::pow(10.0, -3.0 * ts / std::max(0.05, rtM));
        gL[k] = std::pow(10.0, -3.0 * ts / std::max(0.05, rtLo));
        gH[k] = std::pow(10.0, -3.0 * ts / std::max(0.05, rtHi));
        // J-203f: the loop gain per band as a product of shelves (lows below ~300 Hz at gL, highs above ~3.5 kHz
        // at gH): each band then decays at its own time, which the summed one-poles before could not hold
        u.rvG[k] = static_cast<float>(gL[k]);
        u.rvLs[k].set(kRvKLo, g[k] / gL[k]);
        u.rvHs[k].set(kRvKHi, gH[k] / g[k]);
      }
      const float wetF = static_cast<float>(wet * wetG), dryF = 1.f - wet * wet;
      const float preD = static_cast<float>(std::clamp(preS * sr, 1.0, static_cast<double>(u.pre[0].n - 8)));
      // early sound: the source's hall speaks at once when small and builds slowly when large; the
      // vintage builds over its first 100 ms or so; the abyss starts loud
      const float early = static_cast<float>(rtype == 1 ? 0.3 * (1 - sz / 100) * (1 - sz / 100) : rtype == 3 ? 1.0 : rtype == 4 ? 0.35 : 0.0);
      const double modInc = spinR / sr;
      for (int i = 0; i < n; ++i) {
        u.pre[0].push(L[i]), u.pre[1].push(R[i]);
        float in[2] = {u.pre[0].tap(preD), u.pre[1].tap(preD)};
        if (rtype == 2) {  // vintage: a touch of grit
          in[0] = std::round(in[0] * 2048.f) / 2048.f, in[1] = std::round(in[1] * 2048.f) / 2048.f;
        }
        // input diffusion
        for (int d = 0; d < 4; ++d)
          for (int ch = 0; ch < 2; ++ch) {
            const double dl = (0.0047 + 0.0031 * d + ch * 0.0007) * sr * (d < 2 ? 1.0 : 1.3);
            in[ch] = u.dif[d][ch].run(in[ch], static_cast<float>((d < 2 ? diffA : diffB) * 0.75), static_cast<float>(dl));
          }
        u.rvMod += modInc;
        if (u.rvMod >= 1) u.rvMod -= 1;
        float tap[8];
        for (int k = 0; k < 8; ++k) {
          const double m = spinD * 0.0008 * sr * std::sin(kPi2 * (u.rvMod + k * 0.125));
          const float x = u.rv[k].tapAp(static_cast<float>(len[k] + m), u.rvAp[k]);
          // three-band decay: highs at gH, mids at g, lows at gL
          tap[k] = u.rvHs[k].run(u.rvLs[k].run(x * u.rvG[k]));
        }
        // 8x8 Hadamard-style mix
        float h[8];
        for (int k = 0; k < 8; ++k) h[k] = tap[k];
        for (int s = 1; s < 8; s <<= 1)
          for (int k = 0; k < 8; k += s * 2)
            for (int j = k; j < k + s; ++j) {
              const float a = h[j], b = h[j + s];
              h[j] = a + b, h[j + s] = a - b;
            }
        for (int k = 0; k < 8; ++k) u.rv[k].push(std::clamp(h[k] * 0.35355339f + in[k & 1], -8.f, 8.f));
        float wl = (tap[0] + tap[2] + tap[4] + tap[6]) * 0.35f + in[0] * early, wr = (tap[1] + tap[3] + tap[5] + tap[7]) * 0.35f + in[1] * early;
        wl = u.rvEq.run(0, wl), wr = u.rvEq.run(1, wr);
        if (rtype == 2)
          for (auto& b : u.rvBw) wl = b.run(0, wl), wr = b.run(1, wr);
        const float mid = (wl + wr) * 0.5f, side = (wl - wr) * 0.5f * static_cast<float>(width);
        wl = mid + side, wr = mid - side;
        L[i] = (L[i] * dryF + wl * wetF) * out;
        R[i] = (R[i] * dryF + wr * wetF) * out;
      }
      break;
    }
    case FxEq: {
      static const int k1[3] = {0, 1, 3}, k2[3] = {2, 1, 4};
      const int t1 = std::clamp(static_cast<int>(P("kParamType1")), 0, 2), t2 = std::clamp(static_cast<int>(P("kParamType2")), 0, 2);
      // J-203c: Q = 0.1 + 18.9 * (reso / 100)^4 (measured on the source: the low-pass gain at the
      // cutoff is -20.0 / -17.7 / -4.7 / 8.1 / 17.9 / 25.6 dB at reso 0 / 20 / 40 / 60 / 80 / 100)
      // The shelves: Q = 0.4 + 1.73 * (reso / 100)^3 (shelf shapes matched at reso 0 / 25 / 50 / 75 / 90 / 100).
      auto eqQ = [](double reso, bool shelf) {
        const double r = std::clamp(reso / 100.0, 0.0, 1.0);
        return shelf ? 0.4 + 1.73 * r * r * r : 0.1 + 18.9 * r * r * r * r;
      };
      u.eq1.set(k1[t1], sr, P("kParamFreq1"), eqQ(P("kParamReso1"), t1 == 0), P("kParamGain1"));
      u.eq2.set(k2[t2], sr, P("kParamFreq2"), eqQ(P("kParamReso2"), t2 == 0), P("kParamGain2"));
      for (int i = 0; i < n; ++i) {
        L[i] = u.eq2.run(0, u.eq1.run(0, L[i])) * out;
        R[i] = u.eq2.run(1, u.eq1.run(1, R[i])) * out;
      }
      break;
    }
    case FxFilter: {
      const int ft = static_cast<int>(P("kParamType"));
      double hz = cutoffHz(std::clamp(N("kParamFreq") + (N("kParamX") - 0.0) * 0.25, 0.0, 1.0));
      const double res = std::clamp(P("kParamReso") / 100.0 + N("kParamY") * 0.25, 0.0, 1.0);
      const bool clean = N("kParamPad") >= 0.5;
      const double spread = (P("kParamStereo") / 100.0 - 0.5) * 48.0;
      // J-203f, measured (low-pass at 88 Hz, noise in): MIX follows the distortion's law, dry 1 - wet^2 with
      // wet sin^2(pi m / 2) (the dry part reads -0.2 / -2.5 / -11.3 dB at 25 / 50 / 75 %)
      const float fWet = static_cast<float>(std::pow(std::sin(1.5707963267948966 * wet), 2.0)), fDry = 1.f - fWet * fWet;
      const int sub = 32;
      for (int i0 = 0; i0 < n; i0 += sub) {
        const int m = std::min(sub, n - i0);
        // imports (boost): PAD keeps the drive's gain and lifts its clip; Plasma's own CLEAN drops the drive
        u.filt->set(ft, sr, hz, res, clean && !c.driveBoost ? 0.0 : P("kParamDrive") / 100.0, P("kParamVar") / 100.0, spread,
                    c.driveBoost, clean);
        for (int i = i0; i < i0 + m; ++i) {
          float l = L[i], r = R[i];
          u.filt->process(l, r);
          L[i] = (L[i] * fDry + l * fWet) * out;
          R[i] = (R[i] * fDry + r * fWet) * out;
        }
      }
      break;
    }
    case FxHyper: {
      // J-203h, measured on the source (deterministic white noise in, impulse responses per 21 ms window):
      // - each side carries its own voices (UNISON 1..7: 0 / 1 / 2 / 3 / 5 / 5 / 7 per side), each a plain
      //   delay read with a linear tap at gain 0.97; the voices' sum passes a 2-pole low cut (253 Hz, Q 0.75:
      //   the negative tail after every tap) and is not normalised (unison 4, detune 0: +10 dB near 500 Hz)
      // - every voice's delay runs a triangle between 1 sample and 1 + 0.5 * (0.0004 + 0.0004 * DETUNE (%)) * sr
      //   (J-203k: 10.6 samples at 48 kHz for detune 0, 20 ms at 100 %), at 1e-4 * RATE^2 Hz (rate 10: 100 s
      //   per cycle, 40: 6.3 s, 100: 1.0 s; rate 0 holds the delays), so the voices hold small constant pitch
      //   offsets that reverse at the ends; each voice has its own fixed start phase and rate multiple per
      //   side (kHyperPh0 / kHyperRate), so the sides differ even at detune 0
      // - MIX m: wet x = 3m^2 - 2m^3 (the chorus law), dry 1 - 5/9 x (0.444 at 100 %); unison 1 passes the
      //   input untouched at any mix
      const int nSide = kHyperSide[std::clamp(static_cast<int>(std::lround(P("kParamUnison"))), 0, 7)];
      const double det = std::clamp(P("kParamDetune"), 0.0, 100.0), rt = std::clamp(P("kParamRate"), 0.0, 100.0);
      const double dLo = 1.0, dHi = 1.0 + 0.5 * (0.0004 + 0.0004 * det) * sr;
      const double fHz = 1e-4 * rt * rt;
      const double mv = std::clamp(static_cast<double>(wet), 0.0, 1.0), mx = mv * mv * (3 - 2 * mv);
      const float gW = nSide > 0 ? static_cast<float>(mx) * kHyperVoice : 0.f, gD = nSide > 0 ? static_cast<float>(1 - mx * 5.0 / 9.0) : 1.f;
      if (!u.hyInit) {
        for (int ch = 0; ch < 2; ++ch)
          for (int v = 0; v < 7; ++v) u.hyPh[ch * 8 + v] = kHyperPh0[ch][v];
        u.hyInit = true;
      }
      u.hyHp.set(3, sr, kHyperHpHz, kHyperHpQ, 0);
      const float dimWet = static_cast<float>(P("kParamDimEWet") / 100.0);
      const double dimSize = P("kParamDimESize") / 100.0;
      const float dimOut = static_cast<float>(busGain(N("kParamDimELevelOut")));
      for (int i = 0; i < n; ++i) {
        u.ml[0].push(L[i]), u.ml[1].push(R[i]);
        float l = L[i], r = R[i];
        if (nSide > 0) {
          float hs[2] = {0, 0};
          for (int ch = 0; ch < 2; ++ch)
            for (int v = 0; v < nSide; ++v) {
              double& ph = u.hyPh[ch * 8 + v];
              ph += fHz * kHyperRate[ch][v] / sr;
              if (ph >= 1) ph -= 1;
              const double tri = 2 * std::fabs(ph - 0.5);  // 1 at phase 0: the longest delay
              hs[ch] += u.ml[ch].tap(static_cast<float>(dLo + (dHi - dLo) * tri));
            }
          l = L[i] * gD + u.hyHp.run(0, hs[0]) * gW;
          r = R[i] * gD + u.hyHp.run(1, hs[1]) * gW;
        }
        // dimension, measured on the source (held saw, impulse response from the harmonic transfer): the mid
        // passes untouched and a side is added, 0.465 * mix * (x(t - A) - x(t - B)) on the left and its
        // negative on the right, with A = 15.0 + 100 * size and B = 20.9 + 16.9 * size ms (size 0 / 50 / 100:
        // A 15.0 / 65.0 / 115.0, B 20.9 / 29.4 / 37.8 ms). Sine at key 60, size 50: left +4.5, right -4.6 dB.
        u.dl[0].push((l + r) * 0.5f);
        if (dimWet > 0) {
          const float a = u.dl[0].tap(static_cast<float>((0.015025 + 0.1 * dimSize) * sr));
          const float b = u.dl[0].tap(static_cast<float>((0.020907 + 0.01692 * dimSize) * sr));
          const float sd = 0.465f * dimWet * dimOut * (a - b);
          l += sd, r -= sd;
        }
        L[i] = l * out, R[i] = r * out;
      }
      break;
    }
    case FxBode: {
      const bool sync = N("kParamBeatSync") >= 0.5;
      double shiftHz = P("kParamShift") / 100.0 * P("kParamRange");
      if (sync) shiftHz = (P("kParamShift") / 100.0) * c.tempo / 60.0 * 4.0;
      const float fb = static_cast<float>(P("kParamFeedback") / 100.0);
      const float dt = static_cast<float>(std::clamp(P("kParamDelayTime") * sr, 1.0, static_cast<double>(u.bd[0].n - 8)));
      const float bal = static_cast<float>(P("kParamDelayBalance") / 100.0);
      const bool swap = N("kParamSwapAB") >= 0.5, mono = N("kParamMonoInput") >= 0.5;
      const float mixUD = static_cast<float>(P("kParamOutputMix") / 100.0);  // -1 down .. 1 up
      const float width = static_cast<float>(P("kParamOutputWidth") / 100.0);
      const float blurAmt = static_cast<float>(P("kParamBlur") / 100.0);
      const double inc = shiftHz / sr;
      for (int i = 0; i < n; ++i) {
        u.bodePh += inc;
        u.bodePh -= std::floor(u.bodePh);
        const float cs = static_cast<float>(std::cos(kPi2 * u.bodePh)), sn = static_cast<float>(std::sin(kPi2 * u.bodePh));
        float in[2] = {L[i], R[i]};
        if (mono) in[0] = in[1] = (L[i] + R[i]) * 0.5f;
        float y[2];
        for (int ch = 0; ch < 2; ++ch) {
          float x = in[ch] + u.bd[ch].tap(dt) * fb * (ch ? (1 + bal) : (1 - bal)) * 0.5f;
          if (blurAmt > 0) x = x * (1 - blurAmt) + u.blur[ch][1].run(u.blur[ch][0].run(x, 0.6f, 0.011f * static_cast<float>(sr)), 0.6f, 0.017f * static_cast<float>(sr)) * blurAmt;
          float I, Q;
          u.hb[ch].run(x, I, Q);
          float up = I * cs + Q * sn, down = I * cs - Q * sn;
          if (swap) std::swap(up, down);
          // J-203d, measured on the source with a held sine: the left side carries the down shift and
          // the right the up shift; DIR m moves the left towards up (m > 0) or the right towards down
          // (m < 0), so m = 0.5 gives left = half down + half up, right = up
          const float o = ch == 0 ? (mixUD >= 0 ? down * (1 - mixUD) + up * mixUD : down) : (mixUD >= 0 ? up : up * (1 + mixUD) - down * mixUD);
          u.bd[ch].push(o);
          y[ch] = o;
        }
        // WIDTH w: mid + w side (w 0: both sides carry half of each shift; 0.5: 0.75 / 0.25)
        const float mid = (y[0] + y[1]) * 0.5f, side = (y[0] - y[1]) * 0.5f * width;
        const float yl = mid + side, yr = mid - side;
        // MIX m crossfades: dry min(1, 2 (1 - m)), wet min(1, 2 m) (source: at 25 % the dry stays at
        // unity with the shift 6 dB down; at 50 % both at unity)
        const float wG = std::min(1.f, 2.f * wet), dG = std::min(1.f, 2.f * (1.f - wet));
        L[i] = (L[i] * dG + yl * wG) * out;
        R[i] = (R[i] * dG + yr * wG) * out;
      }
      break;
    }
    case FxConvolve: {
      if (!u.convReady) break;
      const float trim = static_cast<float>(dbToGainF(P("kParamIpTrim")));
      const double attack = P("kParamAttack"), dec = P("kParamDecay");
      const bool psync = N("kParamPredelayBeatSync") >= 0.5;
      const double pre = psync ? P("kParamPredelay") * 4.0 * 60.0 / std::max(20.0, c.tempo) : P("kParamPredelay");
      const float preD = static_cast<float>(std::clamp(pre * sr, 1.0, static_cast<double>(u.cpre[0].n - 8)));
      const double damp = P("kParamDamping") / 100.0;
      u.cdamp[0].lp(sr, 20000.0 * std::pow(0.05, damp)), u.cdamp[1].lp(sr, 20000.0 * std::pow(0.05, damp));
      const double tone = P("kParamTone") / 100.0;
      u.ctone.set(tone < 0 ? 2 : 0, sr, tone < 0 ? 3000 : 300, 0.7, -std::fabs(tone) * 12.0);
      const size_t bins = FxUnit::kFftN / 2 + 1;
      // J-203d, measured with the identity impulse and a held sine: mix 25 / 50 / 75 gives +1.0 / +1.9 / +1.0 dB,
      // the chorus law: x = 3w^2 - 2w^3, (1 - x^2) * dry + x * wet
      const double cx = static_cast<double>(wet) * wet * (3 - 2 * static_cast<double>(wet));
      const float cDry = static_cast<float>(1 - cx * cx), cWet = static_cast<float>(cx);
      // the source's wet path has no latency (identity impulse: in phase with the input), so the first partition
      // runs directly and the FFT partitions 1.. use the blocks already complete
      const bool noPre = pre * sr < 1.0;
      const float headG = attack > 0 ? 0.f : 1.f;
      const int hn = static_cast<int>(u.irHead[0].size());
      for (int i = 0; i < n; ++i) {
        u.cpre[0].push(L[i]), u.cpre[1].push(R[i]);
        const float in0 = noPre ? L[i] : u.cpre[0].tap(preD), in1 = noPre ? R[i] : u.cpre[1].tap(preD);
        u.inBuf[0][static_cast<size_t>(u.cpos)] = in0;
        u.inBuf[1][static_cast<size_t>(u.cpos)] = in1;
        u.hpos = u.hpos + 1 >= FxUnit::kPart ? 0 : u.hpos + 1;
        u.hist[0][static_cast<size_t>(u.hpos)] = in0, u.hist[1][static_cast<size_t>(u.hpos)] = in1;
        float h0 = 0, h1 = 0;
        if (headG > 0)
          for (int k = 0, j = u.hpos; k < hn; ++k, j = j > 0 ? j - 1 : FxUnit::kPart - 1)
            h0 += u.irHead[0][static_cast<size_t>(k)] * u.hist[0][static_cast<size_t>(j)], h1 += u.irHead[1][static_cast<size_t>(k)] * u.hist[1][static_cast<size_t>(j)];
        float yl = u.outBuf[0][static_cast<size_t>(u.cpos)] + h0, yr = u.outBuf[1][static_cast<size_t>(u.cpos)] + h1;
        if (++u.cpos >= FxUnit::kPart) {
          u.cpos = 0;
          u.fdlPos = (u.fdlPos + u.parts - 1) % u.parts;
          for (int ch = 0; ch < 2; ++ch) {
            std::fill(u.work.begin(), u.work.end(), cpx(0, 0));
            for (int k = 0; k < FxUnit::kPart; ++k) u.work[static_cast<size_t>(k)] = cpx(u.inBuf[ch][static_cast<size_t>(k)], 0);
            u.cfft.forward(u.work.data());
            std::copy(u.work.begin(), u.work.begin() + static_cast<std::ptrdiff_t>(bins), u.fdl[ch].begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(u.fdlPos) * bins));
            std::fill(u.work.begin(), u.work.end(), cpx(0, 0));
            for (int p2 = 1; p2 < u.parts; ++p2) {
              const double t = static_cast<double>(p2 * FxUnit::kPart) / sr;  // partition time
              float pg = 1;
              if (attack > 0) pg *= static_cast<float>(std::min(1.0, t / std::max(1e-3, attack * 2.0)));
              if (dec < 40) pg *= static_cast<float>(std::exp(-t * 6.9 / std::max(0.05, dec)));
              if (pg < 1e-5f) continue;
              const cpx* X = u.fdl[ch].data() + static_cast<size_t>((u.fdlPos + p2 - 1) % u.parts) * bins;
              const cpx* H = u.irSpec[ch].data() + static_cast<size_t>(p2) * bins;
              for (size_t b = 0; b < bins; ++b) u.work[b] += X[b] * H[b] * pg;
            }
            for (size_t b = 1; b + 1 < bins; ++b) u.work[FxUnit::kFftN - b] = std::conj(u.work[b]);
            u.cfft.inverse(u.work.data());
            for (int k = 0; k < FxUnit::kPart; ++k) {
              u.outBuf[ch][static_cast<size_t>(k)] = u.work[static_cast<size_t>(k)].real() + u.ovl[ch][static_cast<size_t>(k)];
              u.ovl[ch][static_cast<size_t>(k)] = u.work[static_cast<size_t>(k + FxUnit::kPart)].real();
            }
          }
        }
        yl = u.ctone.run(0, u.cdamp[0].run(yl * trim));
        yr = u.ctone.run(1, u.cdamp[1].run(yr * trim));
        L[i] = (L[i] * cDry + yl * cWet) * out;
        R[i] = (R[i] * cDry + yr * cWet) * out;
      }
      break;
    }
    case FxUtility: {
      const float width = static_cast<float>(P("kParamWidth") / 100.0);
      const float bal = static_cast<float>(P("kParamBalance") / 100.0);
      const bool mono = N("kParamLFMono") >= 0.5;
      u.ulf.set(sr, P("kParamLFXover"));
      const double hpf = P("kParamHPF"), lpf = P("kParamLPF");
      // J-203f, measured with stereo saws: the low cut is two 2-pole Butterworth sections in a row
      // (-6 dB at the knob, 24 dB/oct); the high cut stays 2nd order; MIX follows the distortion's
      // law (wet sin^2(pi m / 2), dry 1 - wet^2: +0.3 dB and -4.7 dB width at 50 % with width 0).
      u.uhp.set(3, sr, std::max(1.0, hpf), 0.707, 0);
      u.uhp2.set(3, sr, std::max(1.0, hpf), 0.707, 0);
      const float uWet = static_cast<float>(std::pow(std::sin(1.5707963267948966 * wet), 2.0)), uDry = 1.f - uWet * uWet;
      u.ulp.set(4, sr, lpf, 0.707, 0);
      const float pl = N("kParamPolarityL") >= 0.5 ? -1.f : 1.f, pr = N("kParamPolarityR") >= 0.5 ? -1.f : 1.f;
      const float gl = bal > 0 ? 1 - bal : 1, gr = bal < 0 ? 1 + bal : 1;
      for (int i = 0; i < n; ++i) {
        float l = L[i] * pl, r = R[i] * pr;
        const float m = (l + r) * 0.5f, s = (l - r) * 0.5f * width;
        l = m + s, r = m - s;
        if (mono) {
          double lo0, hi0, lo1, hi1;
          u.ulf.split(0, l, lo0, hi0);
          u.ulf.split(1, r, lo1, hi1);
          const float lm = static_cast<float>((lo0 + lo1) * 0.5);
          l = lm + static_cast<float>(hi0), r = lm + static_cast<float>(hi1);
        }
        if (hpf > 1.5) l = u.uhp2.run(0, u.uhp.run(0, l)), r = u.uhp2.run(1, u.uhp.run(1, r));
        if (lpf < 19900) l = u.ulp.run(0, l), r = u.ulp.run(1, r);
        l *= gl, r *= gr;
        L[i] = (L[i] * uDry + l * uWet) * out;
        R[i] = (R[i] * uDry + r * uWet) * out;
      }
      break;
    }
    default: {  // splitters: the generator splits; here only the level
      for (int i = 0; i < n; ++i) L[i] *= out, R[i] *= out;
      break;
    }
  }
  for (int i = 0; i < n; ++i) {
    if (!std::isfinite(L[i]) || !std::isfinite(R[i])) {
      u.clear();
      std::fill(L, L + n, 0.f);
      std::fill(R, R + n, 0.f);
      peak = 0;
      break;
    }
    peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
  }
  u.meter = std::max(peak, u.meter * 0.9f);
}

}  // namespace gl::packs::plasma

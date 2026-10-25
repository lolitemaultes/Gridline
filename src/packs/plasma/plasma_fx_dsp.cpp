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

namespace {
double dbToGainF(double db) { return std::pow(10.0, db / 20.0); }
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
double lfoBeats(double norm) { return lfoRateDivisionBeats(static_cast<int>(std::lround(std::clamp(norm, 0.0, 1.0) * (lfoRateDivisionCount() - 1)))); }
float triLfo(double ph) {
  ph -= std::floor(ph);
  return static_cast<float>(ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4);
}
// Hilbert pair (two allpass chains, 90 degrees apart over the audio band)
struct Hilbert {
  Ap1 a[4], b[4];
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
  double dsPh = 0;
  Bq2 dfl;
  // modulation effects
  Line ml[4];
  Ap1 ph[2][32];
  float phFb[2]{};
  Op1 lpw[2], hpw[2];
  // delay
  Line dl[2];
  Bq2 dbp;
  float dfb[2]{};
  // compressor
  Lr4 xo1, xo2;
  float env[3][2]{}, gr[3]{};
  Line look[2];
  // reverb
  Line rv[8], pre[2];
  ApD dif[4][2];
  Op1 rvDamp[8], rvLo[2], rvHi[2];
  double rvMod = 0;
  float rvSh[2]{};
  // eq / utility
  Bq2 eq1, eq2, uhp, ulp;
  Lr4 ulf;
  // hyper / dimension
  double hyPh[8]{};
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
    dbp.clear(), dfl.clear(), eq1.clear(), eq2.clear(), uhp.clear(), ulp.clear();
    for (auto& e : env) e[0] = e[1] = 0;
    xo1 = Lr4{}, xo2 = Lr4{}, ulf = Lr4{};
    for (auto& f : rvDamp) f.z = 0;
    rvLo[0].z = rvLo[1].z = rvHi[0].z = rvHi[1].z = 0;
    lpw[0].z = lpw[1].z = hpw[0].z = hpw[1].z = 0;
    shState[0] = shState[1] = dsHold[0] = dsHold[1] = 0;
    hb[0] = Hilbert{}, hb[1] = Hilbert{};
    if (filterMem) filterMem->reset();
    for (int c = 0; c < 2; ++c) {
      if (!fdl[c].empty()) std::fill(fdl[c].begin(), fdl[c].end(), cpx(0, 0));
      if (!inBuf[c].empty()) std::fill(inBuf[c].begin(), inBuf[c].end(), 0.f);
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
    // compressor lookahead 5 ms
    for (auto& l : look) l.bind(p, s / 200 + 8);
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
    return 4 * (s * 60 / 1000 + 8) + 2 * (s * 2 + 8) + 2 * (s / 200 + 8) + 8 * (s * 35 / 100 + 8) + 2 * (s * 6 / 10 + 8) +
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
  }
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
      const int mode = static_cast<int>(P("kParamMode"));
      const float drive = static_cast<float>(P("kParamDrive") / 100.0);
      const int pp = static_cast<int>(P("kParamPrePost"));
      const double fhz = cutoffHz(N("kParamFreq"));
      const double q = P("kParamBW");
      const double lphp = P("kParamLPHP") / 100.0;
      const int kind = lphp < 0.33 ? 4 : lphp < 0.67 ? 5 : 3;
      if (pp) u.dfl.set(kind, sr, fhz, q, 0);
      const int stages = std::clamp(static_cast<int>(P("kParamNumStages")), 1, 16);
      const float* ca = aux ? aux->shapeA[static_cast<size_t>(slot)].data() : nullptr;
      const float* cb = aux ? aux->shapeB[static_cast<size_t>(slot)].data() : nullptr;
      const double dsStep = 1.0 / (1.0 + drive * 63.0);
      for (int i = 0; i < n; ++i) {
        float io[2] = {L[i], R[i]};
        for (int ch = 0; ch < 2; ++ch) {
          float x = io[ch];
          if (pp == 1) x = u.dfl.run(ch, x);
          if (mode == 8) {  // downsample: sample-and-hold rate reduction
            if (ch == 0) {
              u.dsPh += dsStep;
              if (u.dsPh >= 1) u.dsPh -= 1, u.dsHold[0] = io[0], u.dsHold[1] = io[1];
            }
            x = u.dsHold[ch];
          } else {
            const int reps = (mode == 5 || mode == 6) ? stages : 1;
            for (int r = 0; r < reps; ++r) x = shapeSample(mode, x, r == 0 ? drive : 0.f, 0, ca, cb, u.shState[ch]);
          }
          if (pp == 2) x = u.dfl.run(ch, x);
          io[ch] = io[ch] + (x - io[ch]) * wet;
        }
        L[i] = io[0] * out;
        R[i] = io[1] * out;
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
      if (type == FxChorus) {
        const double d1 = P("kParamDelay") * sr / 1000.0, d2 = P("kParamDelay2") * sr / 1000.0, dep = depth * sr / 1000.0;
        const double fc = P("kParamFilt");
        const bool hp = P("kParamFiltMode") >= 0.5;
        u.lpw[0].lp(sr, fc), u.lpw[1].lp(sr, fc);
        for (int i = 0; i < n; ++i) {
          u.lfoPh += inc;
          if (u.lfoPh >= 1) u.lfoPh -= 1;
          float io[2] = {L[i], R[i]};
          for (int ch = 0; ch < 2; ++ch) {
            const double m1 = 0.5 + 0.5 * triLfo(u.lfoPh + ch * 0.25), m2 = 0.5 + 0.5 * triLfo(u.lfoPh + 0.5 + ch * 0.25);
            const float a = u.ml[ch].tap(static_cast<float>(d1 + 2 + m1 * dep));
            const float b = u.ml[ch].tap(static_cast<float>(d2 + 2 + m2 * dep));
            float w = (a + b) * 0.5f;
            const float f = u.lpw[ch].run(w);
            w = hp ? w - f : f;
            u.ml[ch].push(io[ch] + w * fb * 0.9f);
            io[ch] = io[ch] + (w - io[ch]) * wet;
          }
          L[i] = io[0] * out, R[i] = io[1] * out;
        }
      } else if (type == FxFlanger) {
        const double base = 0.0005 * sr, dep = 0.0045 * sr * depth;
        for (int i = 0; i < n; ++i) {
          u.lfoPh += inc;
          if (u.lfoPh >= 1) u.lfoPh -= 1;
          float io[2] = {L[i], R[i]};
          for (int ch = 0; ch < 2; ++ch) {
            const double m = 0.5 + 0.5 * triLfo(u.lfoPh + ch * phOff);
            const float d = u.ml[ch].tap(static_cast<float>(base + m * dep + 1));
            u.ml[ch].push(std::clamp(io[ch] + d * (fb * 1.9f - 0.95f), -4.f, 4.f));
            io[ch] = io[ch] + ((io[ch] + d) * 0.5f - io[ch]) * wet;
          }
          L[i] = io[0] * out, R[i] = io[1] * out;
        }
      } else {  // phaser
        const double fc = P("kParamFreq");
        const int stages = std::clamp(static_cast<int>(P("kParamNumPoles")) * 2, 2, 32);
        const double dep2 = N("kParamDepth2");
        for (int i = 0; i < n; ++i) {
          u.lfoPh += inc;
          if (u.lfoPh >= 1) u.lfoPh -= 1;
          float io[2] = {L[i], R[i]};
          for (int ch = 0; ch < 2; ++ch) {
            const double m = triLfo(u.lfoPh + ch * phOff);
            const double f = std::clamp(fc * std::pow(2.0, m * depth * 3.0), 20.0, sr * 0.45);
            const double w = std::tan(3.14159265358979 * f / sr);
            const float a = static_cast<float>((w - 1) / (w + 1));
            float x = io[ch] + u.phFb[ch] * (fb * 1.8f - 0.9f);
            for (int s = 0; s < stages; ++s) {
              const float as = static_cast<float>(std::clamp(a + (s & 1 ? 1 : -1) * dep2 * 0.2, -0.995, 0.995));
              x = u.ph[ch][s].run(x, as);
            }
            u.phFb[ch] = x;
            io[ch] = io[ch] + ((io[ch] + x) * 0.5f - io[ch]) * wet;
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
      const float fb = static_cast<float>(P("kParamFeedback") / 100.0);
      const bool hq = N("kParamHQ") >= 0.5;
      u.dbp.set(5, sr, P("kParamFreq"), P("kParamBW") * 0.5, 0);
      const float dL = static_cast<float>(std::min(tl * sr, static_cast<double>(u.dl[0].n - 8)));
      const float dR = static_cast<float>(std::min(tr * sr, static_cast<double>(u.dl[1].n - 8)));
      for (int i = 0; i < n; ++i) {
        const float a = hq ? u.dl[0].tapCubic(dL) : u.dl[0].tap(dL);
        const float b = hq ? u.dl[1].tapCubic(dR) : u.dl[1].tap(dR);
        const float fa = u.dbp.run(0, a) * 1.6f + a * 0.2f, fbv = u.dbp.run(1, b) * 1.6f + b * 0.2f;
        if (mode == 1) {  // ping-pong: mono input into L, crossed feedback
          u.dl[0].push((L[i] + R[i]) * 0.5f + fbv * fb);
          u.dl[1].push(fa * fb);
        } else if (mode == 2) {  // tap -> delay: L tap feeds the R delay line
          u.dl[0].push(L[i] + fa * fb * 0.5f);
          u.dl[1].push(R[i] * 0.5f + a + fbv * fb);
        } else {
          u.dl[0].push(L[i] + fa * fb);
          u.dl[1].push(R[i] + fbv * fb);
        }
        L[i] = (L[i] + a * wet) * out;
        R[i] = (R[i] + b * wet) * out;
      }
      break;
    }
    case FxComp: {
      const bool multi = N("kParamMultiband") >= 0.5;
      const double thrDb = -60.0 * P("kParamThresh");  // higher = lower threshold (0 dB .. -60 dB)
      const double ratio = P("kParamRatio");
      const double atk = P("kParamAttack") / 1000.0, rel = P("kParamRelease") / 1000.0;
      const float makeup = static_cast<float>(P("kParamMakeup"));
      const float aA = static_cast<float>(std::exp(-1.0 / std::max(1.0, atk * sr))), aR = static_cast<float>(std::exp(-1.0 / std::max(1.0, rel * sr)));
      const bool look = N("kParamLimiterLatencyComp") >= 0.5;
      auto gain = [&](float& e, float x, double tdb, double rAbove, double rBelow, double dead) {
        const float ax = std::fabs(x);
        e = ax > e ? ax + (e - ax) * aA : ax + (e - ax) * aR;
        const double db = 20.0 * std::log10(std::max(1e-6f, e));
        double g = 0;
        if (db > tdb) g = (tdb - db) * (1.0 - 1.0 / std::max(1.0, rAbove));
        else if (db < tdb - dead && db > -80) g = (tdb - dead - db) * (1.0 - 1.0 / std::max(1.0, rBelow)) * 0.5;  // upward
        return static_cast<float>(std::pow(10.0, std::clamp(g, -60.0, 24.0) / 20.0));
      };
      if (!multi) {
        for (int i = 0; i < n; ++i) {
          const float det = std::max(std::fabs(L[i]), std::fabs(R[i]));
          const float g = gain(u.env[0][0], det, thrDb, ratio >= 999999 ? 1e9 : ratio, 1, 999);
          float l = L[i], r = R[i];
          if (look) {
            u.look[0].push(l), u.look[1].push(r);
            l = u.look[0].tapInt(static_cast<int>(sr / 400)), r = u.look[1].tapInt(static_cast<int>(sr / 400));
          }
          float yl = l * g * makeup, yr = r * g * makeup;
          if (ratio >= 999999) yl = std::clamp(yl, -1.f, 1.f), yr = std::clamp(yr, -1.f, 1.f);
          L[i] = (l + (yl - l) * wet) * out;
          R[i] = (r + (yr - r) * wet) * out;
          u.gr[0] = g;
        }
      } else {
        u.xo1.set(sr, P("kParamXoverLow"));
        u.xo2.set(sr, std::max(P("kParamXoverLow") * 1.5, P("kParamXoverHi")));
        const double tdb[3] = {thrDb + 20 * std::log10(std::max(0.01, P("kParamThreshUD0") / 100.0)),
                               thrDb + 20 * std::log10(std::max(0.01, P("kParamThreshUD1") / 100.0)),
                               thrDb + 20 * std::log10(std::max(0.01, P("kParamThreshUD2") / 100.0))};
        auto rat = [&](const char* k) { return 1.0 + 19.0 * P(k) * P(k); };
        const double ra[3] = {rat("kParamRatio0") * (ratio / 4.0), rat("kParamRatio1") * (ratio / 4.0), rat("kParamRatio2") * (ratio / 4.0)};
        const double rbBase = P("kParamRatioBelow");
        const double rb[3] = {1 + 9 * P("kParamRatioBelow0") * rbBase * 2, 1 + 9 * P("kParamRatioBelow1") * rbBase * 2, 1 + 9 * P("kParamRatioBelow2") * rbBase * 2};
        const double dz[3] = {P("kParamDeadband0"), P("kParamDeadband1"), P("kParamDeadband2")};
        const float bg[3] = {static_cast<float>(dbToGainF(P("kParamGain0"))), static_cast<float>(dbToGainF(P("kParamGain1"))), static_cast<float>(dbToGainF(P("kParamGain2")))};
        for (int i = 0; i < n; ++i) {
          float band[3][2];
          for (int ch = 0; ch < 2; ++ch) {
            double lo, rest, mid, hi;
            u.xo1.split(ch, ch ? R[i] : L[i], lo, rest);
            u.xo2.split(ch, rest, mid, hi);
            band[0][ch] = static_cast<float>(lo), band[1][ch] = static_cast<float>(mid), band[2][ch] = static_cast<float>(hi);
          }
          float yl = 0, yr = 0;
          for (int b = 0; b < 3; ++b) {
            const float det = std::max(std::fabs(band[b][0]), std::fabs(band[b][1]));
            const float g = gain(u.env[b][0], det, tdb[b], ra[b], rb[b], dz[b]) * bg[b];
            yl += band[b][0] * g, yr += band[b][1] * g;
            u.gr[b] = g;
          }
          yl *= makeup, yr *= makeup;
          L[i] = (L[i] + (yl - L[i]) * wet) * out;
          R[i] = (R[i] + (yr - R[i]) * wet) * out;
        }
      }
      break;
    }
    case FxReverb: {
      const int rtype = static_cast<int>(P("kParamType"));
      const double size = 0.25 + 1.75 * P("kParamSize") / 100.0;
      const bool psync = N("kParamPreDelayBeatSync") >= 0.5;
      const double preS = psync ? P("kParamPreDelay") * 60.0 / std::max(20.0, c.tempo) : P("kParamDelay") / 1000.0;
      const double decay = P("kParamFeedback") / 100.0;
      const double rt60 = (rtype == 3 ? 2.0 : 0.6) * std::pow(rtype == 3 ? 30.0 : 25.0, decay);  // seconds
      const double damp = P("kParamFreqC") / 100.0;
      const double lo = 20.0 * std::pow(50.0, P("kParamFreq") / 100.0) * (P("kParamFreq") > 0.5 ? 1 : 0);
      const double hi = 20000.0 * std::pow(0.03, P("kParamFreqB") / 100.0);
      const float width = static_cast<float>(P("kParamWidth") / 100.0);
      const double diffA = P("kParamVintageScale") / 100.0, diffB = P("kParamVintageScaleB") / 100.0;
      const double spinR = 0.05 + 3.0 * P("kParamSpinRate") / 100.0, spinD = P("kParamSpinDepth") / 100.0 * (rtype == 4 ? 2.5 : 1.0);
      const double shape = P("kParamMode") / 100.0;
      static const double base[5][8] = {
          {0.0297, 0.0371, 0.0411, 0.0437, 0.0533, 0.0617, 0.0713, 0.0797},  // plate: dense, short
          {0.0503, 0.0617, 0.0731, 0.0853, 0.0967, 0.1093, 0.1201, 0.1327},  // hall
          {0.0411, 0.0529, 0.0613, 0.0703, 0.0811, 0.0919, 0.1013, 0.1129},  // vintage
          {0.0897, 0.1013, 0.1201, 0.1327, 0.1499, 0.1621, 0.1777, 0.1913},  // abyss
          {0.0731, 0.0853, 0.0997, 0.1129, 0.1277, 0.1409, 0.1553, 0.1697},  // space
      };
      double len[8], g[8];
      for (int k = 0; k < 8; ++k) {
        len[k] = std::min(base[rtype][k] * size * (1.0 + shape * (k & 1 ? 0.15 : -0.15)) * sr, static_cast<double>(u.rv[k].n - 16));
        g[k] = std::pow(10.0, -3.0 * (len[k] / sr) / rt60);
        u.rvDamp[k].lp(sr, 2000.0 + 18000.0 * (1.0 - damp) * (rtype == 2 ? 0.5 : 1.0));
      }
      for (int ch = 0; ch < 2; ++ch) {
        u.rvLo[ch].lp(sr, std::max(5.0, lo));
        u.rvHi[ch].lp(sr, std::max(200.0, hi));
      }
      const float preD = static_cast<float>(std::clamp(preS * sr, 1.0, static_cast<double>(u.pre[0].n - 8)));
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
          tap[k] = u.rvDamp[k].run(u.rv[k].tap(static_cast<float>(len[k] + m)));
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
        for (int k = 0; k < 8; ++k) u.rv[k].push(std::clamp(h[k] * 0.35355339f * static_cast<float>(g[k]) + in[k & 1], -8.f, 8.f));
        float wl = (tap[0] + tap[2] + tap[4] + tap[6]) * 0.35f, wr = (tap[1] + tap[3] + tap[5] + tap[7]) * 0.35f;
        wl = u.rvHi[0].run(wl), wr = u.rvHi[1].run(wr);
        if (lo > 5.0) wl -= u.rvLo[0].run(wl), wr -= u.rvLo[1].run(wr);
        const float mid = (wl + wr) * 0.5f, side = (wl - wr) * 0.5f * width;
        wl = mid + side, wr = mid - side;
        L[i] = (L[i] * (1 - wet) + wl * wet) * out;
        R[i] = (R[i] * (1 - wet) + wr * wet) * out;
      }
      break;
    }
    case FxEq: {
      static const int k1[3] = {0, 1, 3}, k2[3] = {2, 1, 4};
      const int t1 = std::clamp(static_cast<int>(P("kParamType1")), 0, 2), t2 = std::clamp(static_cast<int>(P("kParamType2")), 0, 2);
      u.eq1.set(k1[t1], sr, P("kParamFreq1"), 0.3 + P("kParamReso1") / 100.0 * 8.0, P("kParamGain1"));
      u.eq2.set(k2[t2], sr, P("kParamFreq2"), 0.3 + P("kParamReso2") / 100.0 * 8.0, P("kParamGain2"));
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
      const int sub = 32;
      for (int i0 = 0; i0 < n; i0 += sub) {
        const int m = std::min(sub, n - i0);
        u.filt->set(ft, sr, hz, res, clean ? 0.0 : P("kParamDrive") / 100.0, P("kParamVar") / 100.0, spread);
        for (int i = i0; i < i0 + m; ++i) {
          float l = L[i], r = R[i];
          u.filt->process(l, r);
          L[i] = (L[i] + (l - L[i]) * wet) * out;
          R[i] = (R[i] + (r - R[i]) * wet) * out;
        }
      }
      break;
    }
    case FxHyper: {
      const int voices = 1 + static_cast<int>(P("kParamUnison"));
      const double det = P("kParamDetune") / 100.0;
      const double rate = 0.1 + 6.0 * P("kParamRate") / 100.0;
      const float dimWet = static_cast<float>(P("kParamDimEWet") / 100.0);
      const double dimSize = P("kParamDimESize") / 100.0;
      const float dimOut = static_cast<float>(busGain(N("kParamDimELevelOut")));
      for (int i = 0; i < n; ++i) {
        u.ml[0].push(L[i]), u.ml[1].push(R[i]);
        float hl = 0, hr = 0;
        for (int v = 0; v < voices; ++v) {
          u.hyPh[v] += rate * (1.0 + v * 0.13) / sr;
          if (u.hyPh[v] >= 1) u.hyPh[v] -= 1;
          const double m = 0.5 + 0.5 * std::sin(kPi2 * u.hyPh[v]);
          const float d = static_cast<float>((0.004 + det * 0.018 * m + v * 0.0011) * sr);
          const float pan = voices > 1 ? static_cast<float>(v) / (voices - 1) : 0.5f;
          const float s = (v & 1) ? u.ml[1].tap(d) : u.ml[0].tap(d);
          hl += s * (1 - pan), hr += s * pan;
        }
        const float sc = 1.6f / static_cast<float>(voices);
        hl *= sc, hr *= sc;
        float l = L[i] + (hl - L[i]) * wet, r = R[i] + (hr - R[i]) * wet;
        // dimension: short cross-fed delays widen the image
        u.ml[2].push(l), u.ml[3].push(r);
        const float dl = u.ml[3].tap(static_cast<float>((0.007 + 0.02 * dimSize) * sr));
        const float dr = u.ml[2].tap(static_cast<float>((0.011 + 0.025 * dimSize) * sr));
        l = l + (l - dl * 0.7f - l) * dimWet * 0.6f + dl * dimWet * 0.4f * dimOut;
        r = r + (r - dr * 0.7f - r) * dimWet * 0.6f + dr * dimWet * 0.4f * dimOut;
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
          float up = I * cs - Q * sn, down = I * cs + Q * sn;
          if (swap) std::swap(up, down);
          const float o = up * (0.5f + 0.5f * mixUD) + down * (0.5f - 0.5f * mixUD);
          u.bd[ch].push(o);
          y[ch] = o;
        }
        const float mid = (y[0] + y[1]) * 0.5f;
        float side = (y[0] - y[1]) * 0.5f;
        side += (y[0] - mid) * width;  // width spreads up / down shifts across the image
        const float yl = mid + side, yr = mid - side;
        L[i] = (L[i] + (yl - L[i]) * wet) * out;
        R[i] = (R[i] + (yr - R[i]) * wet) * out;
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
      for (int i = 0; i < n; ++i) {
        u.cpre[0].push(L[i]), u.cpre[1].push(R[i]);
        u.inBuf[0][static_cast<size_t>(u.cpos)] = u.cpre[0].tap(preD);
        u.inBuf[1][static_cast<size_t>(u.cpos)] = u.cpre[1].tap(preD);
        float yl = u.outBuf[0][static_cast<size_t>(u.cpos)], yr = u.outBuf[1][static_cast<size_t>(u.cpos)];
        if (++u.cpos >= FxUnit::kPart) {
          u.cpos = 0;
          u.fdlPos = (u.fdlPos + u.parts - 1) % u.parts;
          for (int ch = 0; ch < 2; ++ch) {
            std::fill(u.work.begin(), u.work.end(), cpx(0, 0));
            for (int k = 0; k < FxUnit::kPart; ++k) u.work[static_cast<size_t>(k)] = cpx(u.inBuf[ch][static_cast<size_t>(k)], 0);
            u.cfft.forward(u.work.data());
            std::copy(u.work.begin(), u.work.begin() + static_cast<std::ptrdiff_t>(bins), u.fdl[ch].begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(u.fdlPos) * bins));
            std::fill(u.work.begin(), u.work.end(), cpx(0, 0));
            for (int p2 = 0; p2 < u.parts; ++p2) {
              const double t = static_cast<double>(p2 * FxUnit::kPart) / sr;  // partition time
              float pg = 1;
              if (attack > 0) pg *= static_cast<float>(std::min(1.0, t / std::max(1e-3, attack * 2.0)));
              if (dec < 40) pg *= static_cast<float>(std::exp(-t * 6.9 / std::max(0.05, dec)));
              if (pg < 1e-5f) continue;
              const cpx* X = u.fdl[ch].data() + static_cast<size_t>((u.fdlPos + p2) % u.parts) * bins;
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
        L[i] = (L[i] * (1 - wet) + yl * wet) * out;
        R[i] = (R[i] * (1 - wet) + yr * wet) * out;
      }
      break;
    }
    case FxUtility: {
      const float width = static_cast<float>(P("kParamWidth") / 100.0);
      const float bal = static_cast<float>(P("kParamBalance") / 100.0);
      const bool mono = N("kParamLFMono") >= 0.5;
      u.ulf.set(sr, P("kParamLFXover"));
      const double hpf = P("kParamHPF"), lpf = P("kParamLPF");
      u.uhp.set(3, sr, std::max(1.0, hpf), 0.707, 0);
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
        if (hpf > 1.5) l = u.uhp.run(0, l), r = u.uhp.run(1, r);
        if (lpf < 19900) l = u.ulp.run(0, l), r = u.ulp.run(1, r);
        l *= gl, r *= gr;
        L[i] = (L[i] + (l - L[i]) * wet) * out;
        R[i] = (R[i] + (r - R[i]) * wet) * out;
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

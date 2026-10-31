// Plasma reverb, the imported law: the five reverb models of the source (fitted and verified against the
// hosted plugin's output). Included by plasma_fx_dsp.cpp inside its anonymous namespace, after FxUnit.
// The knobs are read as their positions n (0..1); what each knob does depends on the type:
//   Plate   SIZE length and feedback, DECAY (Feedback) comb damping, LO CUT / HI CUT the input tone, WIDTH
//   Hall    SIZE all times, TAIL (Delay) the decay time, DECAY (Feedback) the modulation rate, WIDTH its depth
//   Vintage SIZE the decay, TAIL the damping, DIFF A / DIFF B the tank / input lengths, DAMP / SHAPE the input
//           diffusion, DECAY / WIDTH the modulation rate and depth (runs at 29761 Hz)
//   Abyss   SIZE the stage times, SHAPE the pattern, DAMP the all-pass colour, TAIL the feedback, DECAY / WIDTH
//           the wander speed and depth
//   Space   SIZE the times, TAIL the feedback, DECAY / WIDTH the modulation rate and depth (mono, 44.1 kHz)
// Hall, Vintage, Abyss and Space share a low shelf (LO CUT) and a high shelf (HI CUT) that cut up to 24 dB.

struct RvShelf {  // TPT state-variable shelf with Q 0.3; p = 0 flat, p = 1 cuts 24 dB
  double g = 0, h = 1, A = 1, s1[2]{}, s2[2]{};
  bool high = false;
  static constexpr double k = 3.3333;
  void set(bool hi, double p, double sr) {
    high = hi;
    A = std::pow(10.0, -0.6 * p * p);
    const double fc = hi ? 3330.0 - 1000.0 * p * p : 90.0 + 270.0 * p;
    const double t = std::tan(3.14159265358979 * std::min(fc, sr * 0.45) / sr);
    g = hi ? t * std::sqrt(A) : t / std::sqrt(A);
    h = 1.0 / (1.0 + g * (g + k));
  }
  double run(int c, double x) {
    const double v1 = (s1[c] + g * (x - s2[c])) * h, v2 = s2[c] + g * v1;
    s1[c] = 2 * v1 - s1[c], s2[c] = 2 * v2 - s2[c];
    return high ? A * A * x + k * A * (1 - A) * v1 + (1 - A * A) * v2 : x + k * (A - 1) * v1 + (A * A - 1) * v2;
  }
};

struct RvState {
  int type = -1;
  RvShelf lo, hi;
  // hall
  double hPh = 0, hT[8]{}, hFb[2]{};
  float hAp[8]{};
  bool hInit = false;
  // plate
  double pTone[4][2]{}, pComb[10]{}, pDen = 1e-18;
  // space / vintage: the internal-rate converter (mono in)
  double rAcc = 0, rIn[4]{}, rOut[2][4]{};
  Bq2 rAaIn[2], rAaOut[2][2];
  double sPh = 0, sD[3]{}, sPrev = 0;
  bool sInit = false;
  // vintage
  double vLp = 0, vDamp[2]{}, vPh = 0, vTank[2]{}, vScale = -1, vScaleB = -1;
  // abyss
  double aG = 0, aCur[32]{}, aTgt[32]{}, aHp[2]{}, aFb = 0;
  uint32_t aCnt = 0;
  int aTick = 0, aPat = -1;
};

int rvPrime(int v) {
  auto isPrime = [](int x) {
    if (x < 2) return false;
    for (int d = 2; d * d <= x; ++d)
      if (x % d == 0) return false;
    return true;
  };
  while (!isPrime(v)) ++v;
  return v;
}

// Schroeder all-pass on a line: v = x + g d; write v; y = d - g v (d read `delay` samples back)
// the same with a first-order all-pass fractional read (flat magnitude)
inline double rvApA(Line& l, double x, double g, double delay, float& st) {
  const double d = l.tapAp(static_cast<float>(delay), st);
  const double v = x + g * d;
  l.push(static_cast<float>(v));
  return d - g * v;
}
inline double rvAp(Line& l, double x, double g, double delay) {
  const double d = l.tap(static_cast<float>(delay));
  const double v = x + g * d;
  l.push(static_cast<float>(v));
  return d - g * v;
}

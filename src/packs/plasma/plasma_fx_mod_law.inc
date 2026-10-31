// Gridline — "plasma" pack: the imported laws of the modulation effects (phaser, flanger, frequency shifter,
// dimension) and of the multiband compressor. Included by plasma_fx_dsp.cpp inside its anonymous namespace;
// each law was fitted and verified against the hosted plugin's output. Plasma's own projects keep the
// Classic laws in plasma_fx_dsp.cpp.

// The effect LFO shared by the imported phaser and flanger: one phase per module, free at 20 n^4 Hz (n = the
// rate knob position; n = 0 holds it), synced locked to the song position at 2^round(i / 3) / 128 cycles per
// beat times 1, 4/3 or 3/4 (i = round(30 n): 128 beats .. 1/8 beat per cycle).
struct ModLfo {
  bool sync = false;
  double inc = 0, cpb = 0, bps = 0, beat = 0;
  void set(double rateHz, bool synced, const FxContext& c, double sr) {
    sync = synced;
    const double rn = std::pow(std::clamp(rateHz / 20.0, 0.0, 1.0), 0.25);
    inc = 20.0 * rn * rn * rn * rn / sr;
    if (sync) {
      const int i = static_cast<int>(30.0 * rn + 0.5);
      static const double kStep[3] = {1.0, 4.0 / 3.0, 0.75};
      cpb = std::exp2(std::round(i / 3.0)) / 128.0 * kStep[i % 3];
    }
    bps = std::max(20.0, c.tempo) / 60.0 / sr;
    beat = c.beat;
  }
  double next(double& ph, int i) {
    if (sync) {
      const double p = std::fabs(beat + i * bps) * cpb;
      return p - std::floor(p);
    }
    ph += inc;
    ph -= std::floor(ph);
    return ph;
  }
};
inline double triHalf(double x) {  // 0 .. 0.5 .. 0
  x -= std::floor(x);
  return x <= 0.5 ? x : 1.0 - x;
}
// pole positions of the imported phaser as offsets of the base frequency per unit of spread (DEPTH 2), in pole
// order (POLES n uses the first n); fitted on the resting notches of the hosted plugin
constexpr double kPhaserSpread[18] = {3.0, 1.0, -0.5, -0.75, 0.75, 0.5, -0.333, 0.6667, 0.717,
                                      -0.123, 0.2, -0.4, 0.1, -0.717, 1.3, -0.1, 0.8, -0.9};

// The phaser on the imported law:
// - base frequency 20 * 800^n of the FREQ position n (at most 10 kHz); pole k sits at it x (1 + s_k DEPTH 2); each pole is two equal
//   first-order allpasses with coefficient (1 - w) / (1 + w), w = 2 sin(pi f_k / sr) (1 + 16 pi DEPTH^3 tri),
//   so the sweep reaches 26 x at full depth; tri = the LFO triangle 0 .. 0.5, the right side PHASE later
// - positive feedback 0.99 FEEDBACK of the chain output into its input
// - mix: m' = 0.5 - 0.5 cos(pi mix / 2): wet m' (0.5 at 100 %), dry 1 - m'^2
void phaserImported(FxUnit& u, const float* p, float* L, float* R, int n, const FxContext& c, float out) {
  const int type = FxPhaser;
  const double sr = u.sr;
  auto P = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : fxPlain(type, i, p[i]);
  };
  ModLfo lfo;
  lfo.set(P("kParamRate"), P("kParamBeatSync") >= 0.5, c, sr);
  // FREQ is shown on a 20 Hz .. 18 kHz scale, but the poles follow 20 * 800^n of its position n (at most 10 kHz)
  const int fi = fxParamIndex(type, "kParamFreq");
  const double f0 = std::min(10000.0, 20.0 * std::pow(800.0, fi < 0 ? 0.5 : std::clamp(static_cast<double>(p[fi]), 0.0, 1.0)));
  const double spread = std::clamp(P("kParamDepth2"), 0.0, 1.0);
  const double dep = std::clamp(P("kParamDepth") / 100.0, 0.0, 1.0), swing = 16.0 * 3.14159265358979 * dep * dep * dep;
  const double fb = 0.99 * std::clamp(P("kParamFeedback") / 100.0, 0.0, 1.0);
  const double width = std::clamp(P("kParamWidth") / 360.0, 0.0, 1.0);
  const int poles = std::clamp(static_cast<int>(std::lround(P("kParamNumPoles"))), 1, 18);
  double wk[18];
  for (int k = 0; k < poles; ++k) wk[k] = 2.0 * std::sin(3.14159265358979 * std::clamp(f0 * (1.0 + kPhaserSpread[k] * spread), 0.0, sr * 0.25) / sr);
  const double m = std::clamp(P("kParamWet") / 100.0, 0.0, 1.0), mw = 0.5 - 0.5 * std::cos(1.5707963267948966 * m);
  const float gWet = static_cast<float>(mw), gDry = static_cast<float>(1 - mw * mw);
  for (int i = 0; i < n; ++i) {
    const double ph = lfo.next(u.lfoPh, i);
    float io[2] = {L[i], R[i]};
    for (int ch = 0; ch < 2; ++ch) {
      const double t = triHalf(ph + (ch ? width : 0.0)), mod = 1.0 + swing * t;
      float x = io[ch] + static_cast<float>(fb) * u.phFb[ch];
      for (int k = 0; k < poles; ++k) {
        const double w = wk[k] * mod;
        const float a = static_cast<float>((1.0 - w) / (1.0 + w));
        x = u.phI[ch][2 * k].run(x, -a);
        x = u.phI[ch][2 * k + 1].run(x, -a);
      }
      u.phFb[ch] = std::clamp(x, -64.f, 64.f);
      io[ch] = io[ch] * gDry + x * gWet;
    }
    L[i] = io[0] * out, R[i] = io[1] * out;
  }
}

// The flanger on the imported law:
// - the LFO triangle (0 .. 0.5, the right side PHASE later) smoothed by a one-pole (1/301 per sample at 44.1 kHz),
//   delay 0.1 ms + 7 ms DEPTH x it, gliding over 20 ms, cubic read; RATE 0 holds the sweep at DEPTH
// - the line input is softly limited at the source's full scale and low-passed at 8 kHz (a resonant
//   state-variable filter); positive feedback 0.99 FEEDBACK; the wet is the delayed signal
// - mix as the imported phaser (wet 0.5, dry 0.75 at 100 %)
void flangerImported(FxUnit& u, const float* p, float* L, float* R, int n, const FxContext& c, float out) {
  const int type = FxFlanger;
  const double sr = u.sr;
  auto P = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : fxPlain(type, i, p[i]);
  };
  const double rate = P("kParamRate");
  ModLfo lfo;
  lfo.set(rate, P("kParamBeatSync") >= 0.5, c, sr);
  const double dep = std::clamp(P("kParamDepth") / 100.0, 0.0, 1.0);
  const bool frozen = !lfo.sync && rate <= 0.0;
  const double fb = 0.99 * std::clamp(P("kParamFeedback") / 100.0, 0.0, 1.0);
  const double width = std::clamp(P("kParamWidth") / 360.0, 0.0, 1.0);
  const double k1 = 44100.0 / (300.0 * sr + 44100.0), k2 = 50.0 / sr;
  const double fs = std::sin(3.14159265358979 * std::min(0.25, 4000.0 / sr)), fg = 2 * fs;
  const double damp = std::min({1.0 / fs - fs, 2.0, 0.87532});
  const double m = std::clamp(P("kParamWet") / 100.0, 0.0, 1.0), mw = 0.5 - 0.5 * std::cos(1.5707963267948966 * m);
  const double gWet = mw * kSrcUnit, gDry = 1 - mw * mw;
  const double maxD = static_cast<double>(u.ml[0].n - 6);
  for (int i = 0; i < n; ++i) {
    double ph;
    if (frozen) ph = u.lfoPh = dep;
    else ph = lfo.next(u.lfoPh, i);
    const float io[2] = {L[i], R[i]};
    for (int ch = 0; ch < 2; ++ch) {
      const double t = triHalf(ph + (ch ? width : 0.0));
      if (u.flTri[ch] < 0) u.flTri[ch] = t, u.flD[ch] = t * 0.007 * dep * sr + 0.0001 * sr;
      u.flTri[ch] += (t - u.flTri[ch]) * k1;
      const double tgt = u.flTri[ch] * 0.007 * dep * sr + 0.0001 * sr;
      u.flD[ch] += (tgt - u.flD[ch]) * k2;
      const double rd = u.ml[ch].tapCubic(static_cast<float>(std::clamp(u.flD[ch], 2.0, maxD)));
      const double xs = io[ch] / kSrcUnit, v = std::fabs(xs + 1.5) - std::fabs(xs - 1.5);
      const double x = 0.5 * v - v * v * v / 54.0;
      double& lp = u.flLp[ch];
      double& bp = u.flBp[ch];
      for (int h = 0; h < 2; ++h) {
        lp += fg * bp;
        const double hp = x - damp * bp - lp;
        bp = bp - 0.15 * bp * bp * bp + fg * hp;
      }
      u.ml[ch].push(static_cast<float>(std::clamp(lp + fb * rd, -1.0, 1.0)));
      (ch ? R : L)[i] = static_cast<float>((io[ch] * gDry + rd * gWet) * out);
    }
  }
}

// The frequency shifter on the imported law:
// - shift = RANGE x SHIFT; each side split into I / Q (allpass pair), A = I cos + Q sin, B = I cos - Q sin
//   per side; SWAP exchanges A and B
// - DIR u: left runs from the left side's A to the right side's B, right from that B to that A (u 0: both the
//   left A, 0.5: left A / right B, 1: both the right B), WIDTH spreads them
// - delay: the mono A / B blend (BALANCE) into a line read sr (t - 0.27 ms) - 11 samples late, its return x
//   FEEDBACK added to both inputs; free time 0.5 ms + 5 s n^5 (stored as seconds), synced as the reverb's
// - BLUR: four nested allpass diffusers per side (lengths sr (BLUR / 2)^2 x a fixed spread, gliding), faded in
// - mix: wet min(1, 2 m), dry min(1, 2 - 2 m), the dry held 11 samples to line up with the shifter
constexpr double kBlurK[4][4] = {{0.02653, 0.02119, 0.01695, 0.0192},
                                 {0.01733, 0.01516, 0.01493, 0.0165},
                                 {0.01069, 0.01241, 0.0137, 0.01483},
                                 {0.00923, 0.00894, 0.01205, 0.01316}};
void bodeImported(FxUnit& u, const float* p, float* L, float* R, int n, const FxContext& c, float out) {
  const int type = FxBode;
  const double sr = u.sr;
  auto P = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : fxPlain(type, i, p[i]);
  };
  const double shiftHz = P("kParamRange") * P("kParamShift") / 100.0;
  const double fb = std::clamp(P("kParamFeedback") / 100.0, 0.0, 1.0);
  const double bal = std::clamp(0.5 + 0.5 * P("kParamDelayBalance") / 100.0, 0.0, 1.0);
  const double um = std::clamp(0.5 + 0.5 * P("kParamOutputMix") / 100.0, 0.0, 1.0);
  const double wid = std::clamp(P("kParamOutputWidth") / 100.0, 0.0, 1.0);
  const bool swap = P("kParamSwapAB") >= 0.5, mono = P("kParamMonoInput") >= 0.5;
  double secs = std::max(0.0005, P("kParamDelayTime"));
  if (P("kParamBeatSync") >= 0.5) {
    const double nrm = std::pow(std::clamp((secs - 0.0005) / 4.9995, 0.0, 1.0), 0.2);
    const int idx = static_cast<int>(28.0 * nrm + 0.5) + 13;
    static const double kStep[3] = {1.0, 4.0 / 3.0, 0.75};
    secs = std::exp2(std::round(idx / 3.0)) * 120.0 * kStep[idx % 3] / (4096.0 * std::max(20.0, c.tempo));
  }
  const float dly = static_cast<float>(std::clamp(sr * (secs - 0.00027) - 11.0, 2.0, static_cast<double>(u.bd[0].n - 8)));
  const double blur = std::clamp(P("kParamBlur") / 100.0, 0.0, 1.0), blurT = sr * 0.25 * blur * blur;
  if (u.blLen < 0) u.blLen = blurT;
  const double m = std::clamp(P("kParamWet") / 100.0, 0.0, 1.0);
  const float wG = static_cast<float>(std::min(1.0, 2 * m)), dG = static_cast<float>(std::min(1.0, 2 - 2 * m));
  const double inc = shiftHz / sr;
  for (int i = 0; i < n; ++i) {
    u.bodePh += inc;
    u.bodePh -= std::floor(u.bodePh);
    const double cs = std::cos(kPi2 * u.bodePh), sn = std::sin(kPi2 * u.bodePh);
    double in[2] = {L[i], R[i]};
    if (mono) in[0] = in[1] = 0.5 * (L[i] + R[i]);
    const double ret = u.bd[0].tap(dly) * fb;
    u.blLen += (blurT - u.blLen) * 0.000142455;
    u.blC = blur > 0 ? std::min(1.0, u.blC + 0.001) : std::max(0.0, u.blC - 0.001);
    double A = 0, B = 0, As[2], Bs[2];
    for (int ch = 0; ch < 2; ++ch) {
      double x = in[ch] + ret;
      if (u.blC > 0) {
        double y = x;
        for (int s = 0; s < 4; ++s) {
          Line& lp = u.bl[ch][2 * s];
          Line& ls = u.bl[ch][2 * s + 1];
          const float dP = static_cast<float>(std::max(1.5, u.blLen * kBlurK[s][ch])), dS = static_cast<float>(std::max(1.5, u.blLen * kBlurK[s][2 + ch]));
          const double rP = lp.tapAp(dP, u.blY[ch][2 * s]), rS = ls.tapAp(dS, u.blY[ch][2 * s + 1]);
          const double v = y + 0.25 * rP, w = v + 0.25 * rS;
          ls.push(static_cast<float>(w));
          lp.push(static_cast<float>(rS - 0.25 * w));
          y = rP - 0.25 * v;
        }
        x += (y - x) * u.blC;
      }
      float I, Q;
      u.hb[ch].runQuad(static_cast<float>(x), I, Q);
      As[ch] = I * cs - Q * sn;
      Bs[ch] = I * cs + Q * sn;
      if (swap) std::swap(As[ch], Bs[ch]);
      A += 0.5 * As[ch];
      B += 0.5 * Bs[ch];
    }
    u.bd[0].push(static_cast<float>(A + (B - A) * bal));
    const double Pl = As[0] + (Bs[1] - As[0]) * std::max(0.0, 2 * um - 1), Qr = Bs[1] + (As[0] - Bs[1]) * std::max(0.0, 1 - 2 * um);
    const double yl = 0.5 * (Pl + Qr) - 0.5 * (Qr - Pl) * wid, yr = 0.5 * (Pl + Qr) + 0.5 * (Qr - Pl) * wid;
    const int w = u.bDryW;
    const int r = (w + 32 - 11) & 31;
    u.bDry[0][w] = L[i], u.bDry[1][w] = R[i];
    u.bDryW = (w + 1) & 31;
    L[i] = static_cast<float>(yl * wG + u.bDry[0][r] * dG) * out;
    R[i] = static_cast<float>(yr * wG + u.bDry[1][r] * dG) * out;
  }
}

// The dimension stage on the imported law (after the hyper voices): the side-only output of four delays of
// the mono sum (L + R) at D, 1.18 D, 1.18^2 D and 1.18^3 D (D = 15 ms + 100 ms SIZE, gliding over 20 ms);
// two differences of them are turned by one slow sine (0.4545 Hz) and added to the left, taken from the right,
// at 0.25 DIM MIX x DIM LEVEL. The mid is untouched.
void dimensionRun(FxUnit& u, float* L, float* R, int n, double size, double wet, double level) {
  const double sr = u.sr;
  const double D = sr * (0.015 + 0.1 * std::clamp(size, 0.0, 1.0));
  const double tgt[4] = {D, 1.18 * D, 1.3924 * D, 1.643032 * D};
  const double k = 50.0 / sr, maxD = static_cast<double>(u.dl[0].n - 8);
  const double g = 0.25 * wet;
  for (int i = 0; i < n; ++i) {
    u.dl[0].push(L[i] + R[i]);
    double r[4];
    for (int j = 0; j < 4; ++j) {
      if (u.dimD[j] < 0) u.dimD[j] = tgt[j];
      u.dimD[j] += (tgt[j] - u.dimD[j]) * k;
      r[j] = u.dl[0].tap(static_cast<float>(std::clamp(u.dimD[j], 1.0, maxD)));
    }
    u.dimPh += 0.4545 / sr;
    u.dimPh -= std::floor(u.dimPh);
    const double a = r[0] - r[2], b = r[3] - r[1];
    const double w = (a * std::sin(kPi2 * u.dimPh) + b * std::cos(kPi2 * u.dimPh)) * g;
    L[i] = static_cast<float>((L[i] + w) * level);
    R[i] = static_cast<float>((R[i] - w) * level);
  }
}

// The multiband compressor on the imported law:
// - three bands from fourth-order crossovers (X-LOW, max(X-HIGH, 2 X-LOW)), the low band phase-matched
// - per band: the power of both sides (5 ms), an envelope that catches at once and falls with RELEASE
//   (low and mid x1, high x0.468; the limiter end of RATIO scales the times by 0.01)
// - above the band's threshold (48 u + -57.8 / -54.2 / -59.5 dB, all times 4 THRESH^2) the gain falls by
//   RATIO n x (1 - 1 / ratio) dB per dB (at most 40 dB); below it, past a gap of DEADBAND dB (at least 3), it rises
//   by (2 BELOW n - 1) x BELOW dB per dB (at most 36 dB; negative expands)
// - flags packed into LAW (Imported = 1 - flags / 256): bits 0..2 hold the bands' downward part, 3..5 their
//   upward part, bit 6 mixes the plain input as the dry (otherwise the bands' sum)
// - band gains, makeup, the clip at the source's full scale, linear MIX, a 12 Hz DC blocker and LEVEL
int compLawFlags(double lawNorm) {
  return std::clamp(static_cast<int>(std::lround((1.0 - std::clamp(lawNorm, 0.5, 1.0)) * 256.0)), 0, 127);
}
void mbImported(FxUnit& u, const float* p, float* L, float* R, int n, int flags) {
  const int type = FxComp;
  const double sr = u.sr;
  auto P = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : fxPlain(type, i, p[i]);
  };
  auto N = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : std::clamp(static_cast<double>(p[i]), 0.0, 1.0);
  };
  const double ratio = std::max(1.0, P("kParamRatio"));
  const double rn = std::min(1.0 - 1e-5, 1.0 - 1.0 / ratio);
  const bool lim = rn >= 0.981176;
  const double cr = std::clamp(1.000001 - rn, 1e-5, 1.0);
  double atk = std::max(0.1, P("kParamAttack"));
  double rel = std::max(0.1, P("kParamRelease"));
  if (lim) atk = 0.01 * atk + 0.1, rel = 0.01 * rel + 0.1;
  const double kA[3] = {2.13392, 1.0, 0.602678}, kR[3] = {1.0, 1.0, 0.468}, kC[3] = {-57.8, -54.2, -59.5};
  const double t = N("kParamThresh"), tScale = 4.0 * t * t;
  const double below = N("kParamRatioBelow");
  static const char* const kUd[3] = {"kParamThreshUD0", "kParamThreshUD1", "kParamThreshUD2"};
  static const char* const kDz[3] = {"kParamDeadband0", "kParamDeadband1", "kParamDeadband2"};
  static const char* const kRa[3] = {"kParamRatio0", "kParamRatio1", "kParamRatio2"};
  static const char* const kRb[3] = {"kParamRatioBelow0", "kParamRatioBelow1", "kParamRatioBelow2"};
  static const char* const kBg[3] = {"kParamGain0", "kParamGain1", "kParamGain2"};
  double up[3], lo[3], sA[3], sB[3], ea[3], eb[3];
  float bg[3];
  for (int b = 0; b < 3; ++b) {
    up[b] = (48.0 * std::clamp(P(kUd[b]) / 200.0, 0.0, 1.0) + kC[b]) * tScale;
    lo[b] = up[b] - std::clamp(P(kDz[b]), 3.0, 24.0);
    sA[b] = flags & (1 << b) ? 0.0 : std::clamp(P(kRa[b]), 0.0, 1.0) * (cr - 1.0);
    sB[b] = flags & (8 << b) ? 0.0 : (2.0 * std::clamp(P(kRb[b]), 0.0, 1.0) - 1.0) * below;
    ea[b] = std::exp(-1000.0 / (atk * kA[b] * sr));
    eb[b] = std::exp(-1000.0 / (rel * kR[b] * sr));
    bg[b] = static_cast<float>(std::pow(10.0, P(kBg[b]) / 20.0));
  }
  const double fLo = P("kParamXoverLow"), fHi = std::max(P("kParamXoverHi"), 2.0 * fLo);
  u.xo1.set(sr, fLo), u.xo2.set(sr, fHi), u.xo3.set(sr, fHi);
  const double pc = 1.0 - std::exp(-200.0 / sr);
  const double mk2 = std::max(0.0, P("kParamMakeup") - 1.0) / 30.0;
  const double mkT = lim ? 1.0 + 63.0 * mk2 : 1.0 + 31.0 * mk2;
  if (u.cMk < 0) u.cMk = mkT;
  const float w = static_cast<float>(N("kParamWet"));
  const float out = static_cast<float>(busGain(N("kParamLevelOut")));
  const bool plainDry = (flags & 64) != 0;
  const double dcC = 1.0 - std::exp(-75.398 / sr);
  constexpr double kU2 = kSrcUnit * kSrcUnit;
  for (int i = 0; i < n; ++i) {
    double band[3][2];
    for (int ch = 0; ch < 2; ++ch) {
      double l0, rest, mid, hi, la, lb;
      const double xin = ch ? R[i] : L[i];
      u.xo1.split(ch, xin, l0, rest);
      u.xo2.split(ch, rest, mid, hi);
      u.xo3.split(ch, l0, la, lb);  // the low band through the same phase as the others
      band[0][ch] = la + lb, band[1][ch] = mid, band[2][ch] = hi;
    }
    double yl = 0, yr = 0, dl = 0, dr = 0;
    for (int b = 0; b < 3; ++b) {
      u.mbP[b] += ((band[b][0] * band[b][0] + band[b][1] * band[b][1]) / kU2 + 1e-25 - u.mbP[b]) * pc;
      const double r = std::sqrt(u.mbP[b]);
      double& e = u.mbE[b];
      if (r > e) e += (r - e) * ea[b];
      else e *= eb[b];
      e = std::max(e, 1e-10);
      const double lev = 20.0 * std::log10(e);
      double gdb = 0;
      if (lev > up[b]) gdb = std::max(-40.0, (lev - up[b]) * sA[b]);
      else if (lev < lo[b] && lev > -496.0) gdb = std::min(36.0, (lo[b] - lev) * sB[b]);
      const double g = std::pow(10.0, gdb / 20.0) * bg[b];
      yl += band[b][0] * g, yr += band[b][1] * g;
      dl += band[b][0], dr += band[b][1];
      u.gr[b] = static_cast<float>(g);
    }
    if (plainDry) dl = L[i], dr = R[i];
    u.cMk = std::fabs(mkT - u.cMk) < 1e-5 ? mkT : u.cMk + (mkT - u.cMk) * 0.01;
    yl = std::clamp(yl * u.cMk, -kSrcUnit, kSrcUnit), yr = std::clamp(yr * u.cMk, -kSrcUnit, kSrcUnit);
    double ol = yl * w + dl * (1 - w), orr = yr * w + dr * (1 - w);
    ol -= u.cDc[0], u.cDc[0] += ol * dcC;
    orr -= u.cDc[1], u.cDc[1] += orr * dcC;
    L[i] = static_cast<float>(ol) * out, R[i] = static_cast<float>(orr) * out;
  }
}

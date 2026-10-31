// Plasma reverb, the imported law: the per-sample models (see plasma_fx_reverb_law.inc). Included by
// plasma_fx_dsp.cpp inside its anonymous namespace, after FxUnit.
void reverbImported(FxUnit& u, const float* p, float* L, float* R, int n, const FxContext& c, float out) {
  const double sr = u.sr;
  const int type = FxReverb;
  auto P = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : fxPlain(type, i, p[i]);
  };
  auto N = [&](const char* key) {
    const int i = fxParamIndex(type, key);
    return i < 0 ? 0.0 : std::clamp(static_cast<double>(p[i]), 0.0, 1.0);
  };
  RvState& s = u.rvs;
  const int rtype = std::clamp(static_cast<int>(std::lround(P("kParamType"))), 0, 4);
  if (s.type != rtype) {  // a new type starts from silence
    for (auto& l : u.rx) l.clear();
    s = RvState{};
    s.type = rtype;
  }
  const double size = N("kParamSize"), dly = N("kParamDelay"), fq = N("kParamFreq"), fb = N("kParamFeedback"), fqB = N("kParamFreqB");
  const double wid = N("kParamWidth"), fqC = N("kParamFreqC"), mode = N("kParamMode"), vs = N("kParamVintageScale"), vsB = N("kParamVintageScaleB");
  // the internal rate of the converted models
  const double ir = rtype == 2 ? 29761.0 : rtype == 4 ? 44100.0 : sr;
  s.lo.set(false, fq, ir), s.hi.set(true, fqB, ir);
  // pre-delay: free 2.5 n^5 s, synced 1/96 .. 6 beats
  double preS;
  {
    const double pd = N("kParamPreDelay");
    if (N("kParamPreDelayBeatSync") >= 0.5) {
      const int i = static_cast<int>(std::lround(28.0 * pd)) + 13;
      static const double kStep[3] = {1.0, 4.0 / 3.0, 0.75};
      preS = std::exp2(std::round(i / 3.0) - 11.0) * kStep[i % 3] * 60.0 / std::max(20.0, c.tempo);
    } else {
      preS = 2.5 * pd * pd * pd * pd * pd;
    }
    if (rtype == 2) preS += 0.12;  // the vintage tank speaks 120 ms after its input
  }
  const float preD = static_cast<float>(std::clamp(preS * sr, 1.0, static_cast<double>(u.pre[0].n - 8)));
  const double m = std::clamp(P("kParamWet") / 100.0, 0.0, 1.0), mw = 0.5 - 0.5 * std::cos(3.14159265358979 * m);
  const double gWet = mw * kSrcUnit, gDry = 1 - mw * mw, inv = 1.0 / kSrcUnit;

  // ---- per-type setup
  // hall
  static const double kHallT[8] = {77.05, 97.08, 122.31, 183.26, 68.65, 86.49, 108.97, 163.27};
  double hTgt[8]{}, hGain = 0, hDepth = 0, hInc = 0, hGlide = 0;
  if (rtype == 1) {
    const double sc = std::exp2(5.0 * size - 2.5);
    for (int k = 0; k < 8; ++k) hTgt[k] = kHallT[k] * sc;
    // decay time (fitted: 0.75 x 122.31 * 2^(10 n) ms reproduces the source's tails at TAIL 0 / 31.25 / 100)
    const double rt = 0.75 * 122.31 * std::exp2(10.0 * dly);
    hGain = std::pow(10.0, -3.0 * hTgt[3] / rt);
    hDepth = 2.0 * wid;  // ms
    hInc = 2.0 * fb / sr;
    hGlide = 44100.0 / (3000.0 * sr + 44100.0);
    if (!s.hInit) {
      for (int k = 0; k < 8; ++k) s.hT[k] = hTgt[k];
      s.hInit = true;
    }
  }
  // plate
  static const double kPlateB[11] = {42.60, 45.37, 48.32, 51.47, 54.81, 58.38, 62.18, 66.22, 70.53, 75.11, 80.00};
  int pIn = 0, pComb[10]{}, pAp[2][7]{};
  double pOff[10]{}, pS = 0, pD = 0, pc1 = 0, pc2 = 0;
  if (rtype == 0) {
    auto ms = [&](double v) { return rvPrime(static_cast<int>(std::ceil(v * sr / 1000.0))); };
    pIn = ms(15.0);
    pS = 1 - std::pow(1 - size, 6.0);
    for (int k = 0; k < 5; ++k) {
      pComb[k] = ms(kPlateB[k] * (k % 2 == 0 ? 1.005 : 1.0));
      pComb[5 + k] = ms(kPlateB[k] * (k % 2 == 1 ? 1.005 : 1.0));
      pOff[k] = std::floor((pComb[k] - 2) * (0.395 + 0.582 * pS) + 1);
      pOff[5 + k] = std::floor((pComb[5 + k] - 2) * (0.398 + 0.582 * pS) + 1);
    }
    pAp[0][0] = ms(kPlateB[3] * 0.21);
    for (int k = 0; k < 6; ++k) pAp[0][k + 1] = ms(kPlateB[4 + k] * 0.21), pAp[1][k + 1] = ms(kPlateB[4 + k] * 0.22);
    pD = 0.95 * fb * fb;
    auto coef = [&](double v) {
      const double q = std::pow(v * 44100.0 / std::max(sr, 44100.0), 2.0);
      return (3.6 - 1.6 * q) * q;
    };
    pc1 = coef(1 - fqB), pc2 = coef(0.05 + 0.95 * fq);
  }
  // abyss
  if (rtype == 3) {
    const int pat = std::min(4, static_cast<int>(5 * mode));
    static const double kPat[5][3][4] = {
        {{4.10, 3.55, 5.10, 16}, {2.45, 4.35, 4.25, 8}, {3.80, 3.05, 4.25, 8}},
        {{6.00, 5.00, 1.85, 16}, {5.75, 6.00, 0.40, 8}, {6.25, 4.00, 0.30, 8}},
        {{30.0, 3.50, 0.40, 16}, {45.3, 0.10, 0.10, 8}, {43.6, 3.70, 0.40, 8}},
        {{62.0, 90.05, 3.10, 6}, {58.95, 13.35, 2.90, 6}, {53.3, 13.05, 3.25, 6}},
        {{5.05, 0.25, 0.05, 8}, {5.15, 0.15, 0.05, 4}, {5.35, 0.50, 0.05, 4}}};
    const double scl = (size * size + 0.1) * sr / 1000.0;
    int st = 0;
    for (int ch = 0; ch < 3; ++ch) {
      double t = kPat[pat][ch][0], d = kPat[pat][ch][1];
      const double e = kPat[pat][ch][2];
      const int cnt = static_cast<int>(kPat[pat][ch][3]);
      for (int k = 0; k < (ch ? 8 : 16); ++k, ++st) {
        s.aTgt[st] = k < cnt ? std::min(32767.0, t * scl) : -1;  // -1: the stage is off in this pattern
        t += d, d += e;
      }
    }
    if (s.aPat != pat) {
      for (int k = 0; k < 32; ++k) s.aCur[k] = s.aTgt[k];
      if (s.aPat < 0) s.aG = std::pow(2 * fqC - 1, 3.0) >= 0 ? 0.91 + 0.07 * std::pow(2 * fqC - 1, 3.0) : -0.86 + 0.1 * std::pow(2 * fqC - 1, 3.0);
      s.aPat = pat;
    }
  }
  const double cA = std::pow(2 * fqC - 1, 3.0), aGt = cA >= 0 ? 0.91 + 0.07 * cA : -0.86 + 0.1 * cA;
  const uint32_t aInc = static_cast<uint32_t>(std::floor(sr * (0.00019022 + 161.685 * std::pow(fb, 12.0))));
  const double aDepth = (0.8 * std::pow(wid, 9.0) + 1e-6) * sr, aHpC = 1 - std::exp(-2 * 3.14159265358979 * 60.0 / sr);
  // the internal-rate converter's anti-alias filters (vintage only: space runs within 10 % of the rate)
  const bool conv = rtype == 2 || rtype == 4;
  if (rtype == 2) {
    const double fc = std::min(0.5 * ir, 0.45 * sr);
    static const double kQ[2] = {0.5412, 1.3066};
    for (int k = 0; k < 2; ++k) {
      s.rAaIn[k].set(4, sr, fc, kQ[k], 0);
      s.rAaOut[0][k].set(4, sr, fc, kQ[k], 0), s.rAaOut[1][k].set(4, sr, fc, kQ[k], 0);
    }
  }
  if (rtype == 2) {
    if (s.vScale < 0) s.vScale = vs, s.vScaleB = vsB;
  }

  // one internal-rate step of space / vintage: mono in, stereo out
  auto spaceStep = [&](double x, double& yl, double& yr) {
    const double S = 128.0 * size;
    s.sPh += 0.00025 * fb;
    if (s.sPh >= 2) s.sPh -= 2;
    const double tri = s.sPh < 1 ? s.sPh : 2 - s.sPh, mod = 2048.0 * 0.2 * wid * tri;
    const double tg[3] = {64 * S - mod, 29.625 * S + mod / 2, 18.375 * S + mod / 2};
    for (int k = 0; k < 3; ++k) {
      if (!s.sInit) s.sD[k] = tg[k];
      s.sD[k] += std::clamp(tg[k] - s.sD[k], -1.0 / 32, 1.0 / 32);
    }
    s.sInit = true;
    const double d1 = u.rx[0].tap(static_cast<float>(std::clamp(s.sD[0], 1.0, 8190.0)));
    u.rx[0].push(static_cast<float>(x + dly * s.sPrev));
    double y = d1;
    for (int k = 1; k < 3; ++k) {
      const double d = u.rx[k].tap(static_cast<float>(std::clamp(s.sD[k], 1.0, 4094.0)));
      const double v = std::clamp(y + 0.625 * d, -1.0, 1.0);
      u.rx[k].push(static_cast<float>(v));
      y = std::clamp(d - 0.625 * v, -1.0, 1.0);
    }
    y = s.lo.run(0, s.hi.run(0, y));
    s.sPrev = y;
    yl = yr = y;
  };
  // vintage: the figure-eight plate tank at 29761 Hz
  auto vintageStep = [&](double x, double& yl, double& yr) {
    s.vScale += (vs - s.vScale) * 0.001, s.vScaleB += (vsB - s.vScaleB) * 0.001;
    const double v = s.vScaleB, w = s.vScale;
    double a = s.lo.run(0, s.hi.run(0, x));
    static const double kDif[4] = {283, 213, 757, 553};
    for (int k = 0; k < 4; ++k) a = rvAp(u.rx[k], a, k < 2 ? fqC : mode, std::max(12.0, std::round(v * kDif[k])));
    const double decay = size, dd2 = std::clamp(size + 0.15, 0.25, 0.5);
    s.vPh += 16.0 * fb * fb / 44100.0;
    if (s.vPh >= 1) s.vPh -= 1;
    const double exc = 16.0 * wid, ang = 6.283185307179586 * s.vPh;
    static const double kDel1[2] = {8905, 8433}, kDif2[2] = {3599, 5311}, kDel2[2] = {7439, 6325}, kMod[2] = {672, 908};
    double outs[2];
    for (int h = 0; h < 2; ++h) {
      double t = a + decay * s.vTank[1 - h];
      // modulated decay diffuser (Line indices: 4 + h), coefficient -0.7 as in the classic figure-eight plate
      const double md = kMod[h] + exc * (h ? std::cos(ang) : std::sin(ang));
      const double d = u.rx[4 + h].tapCubic(static_cast<float>(md));
      const double vv = t - 0.7 * d;
      u.rx[4 + h].push(static_cast<float>(vv));
      t = d + 0.7 * vv;
      // delay 1, damping, decay, diffuser 2, delay 2
      u.rx[6 + h].push(static_cast<float>(t));
      double y = u.rx[6 + h].tap(static_cast<float>(std::max(12.0, w * kDel1[h])));
      s.vDamp[h] = (1 - dly) * y + dly * s.vDamp[h];
      y = s.vDamp[h] * decay;
      y = rvAp(u.rx[8 + h], y, dd2, std::max(12.0, w * kDif2[h]));
      u.rx[10 + h].push(static_cast<float>(y));
      outs[h] = u.rx[10 + h].tap(static_cast<float>(std::max(12.0, w * kDel2[h])));
    }
    s.vTank[0] = outs[0], s.vTank[1] = outs[1];
    auto at = [&](int line, double pos) { return static_cast<double>(u.rx[line].tap(static_cast<float>(std::max(1.0, pos * w)))); };
    // taps: line 6 / 8 / 10 = left half's delay 1 / diffuser 2 / delay 2, 7 / 9 / 11 the right half's
    yl = 0.6 * (at(7, 266) + at(7, 2974) + at(9, 1913) - at(11, 1996) - at(6, 1990) - at(8, 187) - at(10, 1066));
    yr = 0.6 * (at(6, 353) + at(6, 3627) + at(8, 1228) - at(10, 2673) - at(7, 2111) - at(9, 335) - at(11, 121));
  };

  for (int i = 0; i < n; ++i) {
    u.pre[0].push(L[i]), u.pre[1].push(R[i]);
    const double inL = u.pre[0].tap(preD) * inv, inR = u.pre[1].tap(preD) * inv;
    double wl = 0, wr = 0;
    switch (rtype) {
      case 0: {  // plate
        s.pDen = -s.pDen;
        const double x = inL + inR + s.pDen;
        const double y = rvAp(u.rx[0], x, 0.68, pIn);
        double cu = std::clamp(0.2 * (x + 0.2 * y), -1.0, 1.0);
        auto lp1 = [](double& x1y1a, double& x1y1b, double c1, double v) {  // y = (c/2)(x + x1) + (1 - c) y1
          const double yy = 0.5 * c1 * (v + x1y1a) + (1 - c1) * x1y1b;
          x1y1a = v, x1y1b = yy;
          return yy;
        };
        cu = lp1(s.pTone[0][0], s.pTone[0][1], pc1, cu);
        cu = std::clamp(lp1(s.pTone[1][0], s.pTone[1][1], pc1, cu), -1.0, 1.0);
        double hpv = lp1(s.pTone[2][0], s.pTone[2][1], pc2, cu);
        hpv = lp1(s.pTone[3][0], s.pTone[3][1], pc2, hpv);
        cu -= hpv;
        double side[2] = {0, 0};
        for (int k = 0; k < 10; ++k) {
          Line& l = u.rx[1 + k];
          const double d = l.tap(static_cast<float>(pOff[k]));
          s.pComb[k] = d + (s.pComb[k] - d) * pD;
          l.push(static_cast<float>(cu + 0.97 * pS * s.pComb[k]));
          side[k / 5] += d;
        }
        for (int ch = 0; ch < 2; ++ch)
          for (int k = ch ? 1 : 0; k < 7; ++k) side[ch] = rvAp(u.rx[11 + ch * 7 + k], side[ch], 0.68, pAp[ch][k]);
        const double a = 0.5 - 0.5 * wid, b = 0.5 + 0.5 * wid;
        wl = b * side[0] + a * side[1], wr = b * side[1] + a * side[0];
        break;
      }
      case 1: {  // hall
        s.hPh += hInc;
        if (s.hPh >= 1) s.hPh -= 1;
        for (int k = 0; k < 8; ++k) s.hT[k] += (hTgt[k] - s.hT[k]) * hGlide;
        const double msr = sr / 1000.0;
        double x[2] = {s.lo.run(0, s.hi.run(0, inL + s.hFb[1])), s.lo.run(1, s.hi.run(1, inR + s.hFb[0]))};
        for (int k = 0; k < 3; ++k) {
          const double sn = std::sin(6.283185307179586 * (s.hPh - k / 14.0));
          x[0] = rvApA(u.rx[k], x[0], 0.625, std::max(2.0, (s.hT[k] + hDepth * sn) * msr), s.hAp[k]);
          x[1] = rvApA(u.rx[3 + k], x[1], 0.625, std::max(2.0, (s.hT[4 + k] - hDepth * sn) * msr), s.hAp[3 + k]);
        }
        wl = x[0], wr = x[1];
        const double st = std::sin(6.283185307179586 * s.hPh - 1.3464);
        u.rx[6].push(static_cast<float>(x[0])), u.rx[7].push(static_cast<float>(x[1]));
        const double tl = u.rx[6].tapAp(static_cast<float>(std::max(2.0, (s.hT[3] + hDepth * st) * msr)), s.hAp[6]);
        const double tr = u.rx[7].tapAp(static_cast<float>(std::max(2.0, (s.hT[7] - hDepth * st) * msr)), s.hAp[7]);
        s.hFb[0] = std::clamp(tl * hGain, -1.0, 1.0), s.hFb[1] = std::clamp(tr * hGain, -1.0, 1.0);
        break;
      }
      case 3: {  // abyss
        s.aG += std::clamp(aGt - s.aG, -0.0001, 0.0001);
        if (++s.aTick >= 4) {
          s.aTick = 0;
          for (int k = 0; k < 32; ++k) {
            if (s.aTgt[k] < 0) continue;
            const double base = s.aTgt[k];
            double& cur = s.aCur[k];
            const double tgt = std::min(32767.0, base + aDepth * std::fabs(static_cast<double>(s.aCnt & 0xffffff) / 8388608.0 - 1.0));
            if (std::fabs(tgt - cur) <= 0.4) {
              cur = tgt;
              s.aCnt = (s.aCnt + aInc) & 0xffffff;
            } else {
              cur += tgt > cur ? 0.4 : -0.4;
            }
          }
        }
        auto chain = [&](int first, double v) {
          for (int k = first; k < first + 16; ++k)
            if (s.aTgt[k] >= 0) v = rvAp(u.rx[k], v, s.aG, std::max(1.0, s.aCur[k]));
          return v;
        };
        // stages 0..15 the shared chain, 16..23 the left, 24..31 the right (aTgt of the unused ones is -1)
        const double a = chain(0, 0.5 * (inL + inR) + dly * s.aFb);
        double l = a, r = a;
        for (int k = 16; k < 24; ++k)
          if (s.aTgt[k] >= 0) l = rvAp(u.rx[k], l, s.aG, std::max(1.0, s.aCur[k]));
        for (int k = 24; k < 32; ++k)
          if (s.aTgt[k] >= 0) r = rvAp(u.rx[k], r, s.aG, std::max(1.0, s.aCur[k]));
        s.aHp[0] += aHpC * (l - s.aHp[0]), s.aHp[1] += aHpC * (r - s.aHp[1]);
        l -= s.aHp[0], r -= s.aHp[1];
        s.aFb = 0.5 * (l + r);
        wl = s.lo.run(0, s.hi.run(0, l)), wr = s.lo.run(1, s.hi.run(1, r));
        break;
      }
      default: {  // vintage / space: through the internal-rate converter
        double x = 0.5 * (inL + inR);
        if (rtype == 2) {
          s.vLp += 0.85 * (x - s.vLp);  // the input low-pass runs at the project rate
          x = s.rAaIn[1].run(0, s.rAaIn[0].run(0, static_cast<float>(s.vLp)));
        }
        for (int k = 3; k > 0; --k) s.rIn[k] = s.rIn[k - 1];
        s.rIn[0] = x;
        s.rAcc += ir / sr;
        while (s.rAcc >= 1) {
          s.rAcc -= 1;
          // the internal sample sits rAcc internal periods before now: read the input there (cubic, 1 sample late)
          const double t = 1 + s.rAcc * sr / ir;
          const int i0 = std::min(2, static_cast<int>(t));
          const double f = t - i0, y0 = s.rIn[std::max(0, i0 - 1)], y1 = s.rIn[i0], y2 = s.rIn[std::min(3, i0 + 1)], y3 = s.rIn[std::min(3, i0 + 2)];
          const double xi = y1 + 0.5 * f * (y2 - y0 + f * (2 * y0 - 5 * y1 + 4 * y2 - y3 + f * (3 * (y1 - y2) + y3 - y0)));
          double yl = 0, yr = 0;
          if (rtype == 2) vintageStep(xi, yl, yr);
          else spaceStep(xi, yl, yr);
          for (int ch = 0; ch < 2; ++ch) {
            for (int k = 3; k > 0; --k) s.rOut[ch][k] = s.rOut[ch][k - 1];
            s.rOut[ch][0] = ch ? yr : yl;
          }
        }
        // output between the last internal samples (Catmull-Rom, two internal samples late)
        const double f = s.rAcc;
        double o[2];
        for (int ch = 0; ch < 2; ++ch) {
          const double* y = s.rOut[ch];
          const double y0 = y[3], y1 = y[2], y2 = y[1], y3 = y[0];
          o[ch] = y1 + 0.5 * f * (y2 - y0 + f * (2 * y0 - 5 * y1 + 4 * y2 - y3 + f * (3 * (y1 - y2) + y3 - y0)));
        }
        wl = o[0], wr = o[1];
        (void)conv;
        break;
      }
    }
    L[i] = static_cast<float>((L[i] * gDry + wl * gWet) * out);
    R[i] = static_cast<float>((R[i] * gDry + wr * gWet) * out);
  }
}

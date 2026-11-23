// The Shaper's engine as it was before K-257 phase 2 (0.7.7 plus the K-257 lane lengths and band
// split), kept verbatim as the reference: states that use none of the phase 2 controls must render
// bit-identical to it (tests/test_k257b.cpp). Included inside a namespace that provides param(),
// iparam(), coef() and SyncClock as engine/Effects4.cpp does.
inline double legacyLfoShape(int shape, double ph) {
  switch (shape) {
    case 1: return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
    case 2: return 2 * ph - 1;
    case 3: return ph < 0.5 ? 1 : -1;
    default: return std::sin(kTwoPi * ph);
  }
}

class LegacyShaper {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
    using namespace pp;
    const double sr = ctx.sampleRate;
    static const double beats[] = {1, 2, 4, 8, 16};
    static const double lfoBeats[] = {0.25, 0.5, 1, 2, 4, 8, 16};
    double len = beats[std::clamp(iparam(s, ShLength, 2), 0, 4)];
    int filt = std::clamp(iparam(s, ShFilter), 0, 8);
    double cmin = param(s, ShCutMin, 200), cmax = param(s, ShCutMax, 12000), reso = param(s, ShReso, 0.3f);
    double sm = coef(param(s, ShSmooth, 4) * 0.001, sr), mix = param(s, ShMix, 1);
    double tension = param(s, ShTension), depth = param(s, ShCutDepth, 1);
    double drive = dbToGain(param(s, ShDrive)), driveMix = param(s, ShDriveMix);
    int stages = std::clamp(iparam(s, ShSlope) + 1, 1, 3);
    double shelf = param(s, ShShelfGain);
    double follow = param(s, ShFollow), fAtt = coef(param(s, ShFollowAtt, 0.01f), sr), fRel = coef(param(s, ShFollowRel, 0.15f), sr);
    double lfoAmt = param(s, ShLfoAmt), lfoLen = lfoBeats[std::clamp(iparam(s, ShLfoRate, 2), 0, 6)];
    int lfoSh = iparam(s, ShLfoShape);
    double inG = dbToGain(param(s, ShInput)), outG = dbToGain(param(s, ShOutput));
    double spb = 60.0 / std::max(1.0, ctx.tempo) * sr;
    // K-257: each lane's own step count, and the band split.
    const int volN = std::clamp(iparam(s, ShVolSteps, kShaperPoints - 1) + 1, 1, kShaperPoints);
    const int cutN = std::clamp(iparam(s, ShCutSteps, kShaperPoints - 1) + 1, 1, kShaperPoints);
    const int panN = std::clamp(iparam(s, ShPanSteps, kShaperPoints - 1) + 1, 1, kShaperPoints);
    const int band = std::clamp(iparam(s, ShBand), 0, 2);
    if (band > 0) {
      const double bf = param(s, ShBandFreq, 250);
      if (bf != bandHz_ || sr != bandSr_) {
        bandHz_ = bf, bandSr_ = sr;
        for (int k = 0; k < 2; ++k)  // two in a row each: a 24 dB crossover whose bands add up to the full level
          for (int c = 0; c < 2; ++c) split_[k][c].set(Biquad::LowPass, sr, bf, 0.70710678, 0), splitHi_[k][c].set(Biquad::HighPass, sr, bf, 0.70710678, 0);
      }
    }
    const double stepLen = len / kShaperPoints;
    for (int i = 0; i < n; ++i) {
      double beat;
      if (ctx.playing) {
        beat = SyncClock::beat(ctx, i);
      } else {
        free_ += 1.0 / spb;
        if (free_ > 1e6) free_ = 0;
        beat = free_;
      }
      double ph = std::fmod(beat, len) / len;
      const double f16 = ph * kShaperPoints;
      // A lane of n steps loops every n steps (n = 16: the whole pattern, exactly as before).
      auto lane = [&](int base, float dflt, int steps) {
        double f = steps == kShaperPoints ? f16 : std::fmod(beat, stepLen * steps) / stepLen;
        int k = std::clamp(static_cast<int>(f), 0, steps - 1);
        double a = param(s, base + k, dflt);
        if (std::fabs(tension) < 1e-4) return a;  // steps
        double b = param(s, base + (k + 1) % steps, dflt);
        double fr = f - k, kk = std::pow(2.0, -tension * 3.0);
        return a + (b - a) * std::pow(fr, kk);
      };
      double vol = lane(ShVol, 1, volN), cut = lane(ShCut, 1, cutN), pan = lane(ShPan, 0, panN);
      vol_ = vol + (vol_ - vol) * sm;
      cut_ = cut + (cut_ - cut) * sm;
      pan_ = pan + (pan_ - pan) * sm;
      double l = L[i] * inG, r = R[i] * inG;
      double restL = 0, restR = 0;  // K-257: the band left untouched (it adds back in after the shaping)
      if (band > 0) {
        const double lowL = split_[1][0].process(split_[0][0].process(l)), lowR = split_[1][1].process(split_[0][1].process(r));
        const double highL = splitHi_[1][0].process(splitHi_[0][0].process(l)), highR = splitHi_[1][1].process(splitHi_[0][1].process(r));
        if (band == 1) restL = highL, restR = highR, l = lowL, r = lowR;
        else restL = lowL, restR = lowR, l = highL, r = highR;
      }
      double a = std::max(std::fabs(l), std::fabs(r));
      fol_ = a > fol_ ? a + (fol_ - a) * fAtt : a + (fol_ - a) * fRel;
      if (filt > 0) {
        if ((ctr_++ & 7) == 0) {
          double lfo = lfoAmt != 0 ? legacyLfoShape(lfoSh, std::fmod(beat, lfoLen) / lfoLen) * lfoAmt * 0.5 : 0;
          double env = depth >= 0 ? 1.0 - depth * (1.0 - cut_) : 1.0 + depth * cut_;
          double norm = std::clamp(env + follow * std::min(1.0, fol_ * 2) + lfo, 0.0, 1.0);
          double fc = cmin * std::pow(cmax / std::max(1.0, cmin), norm);
          for (auto& st : svf_)
            for (auto& sv : st) sv.set(sr, fc, reso);
          if (filt >= 5) {
            Biquad::Type t = filt == 5 ? Biquad::LowShelf : filt == 6 ? Biquad::Peak : Biquad::HighShelf;
            for (auto& st : bq_)
              for (auto& b : st) {
                double x1 = b.x1, x2 = b.x2, y1 = b.y1, y2 = b.y2;
                if (filt == 8) {  // all-pass (RBJ)
                  double qq = 0.5 + reso * 4, w = kTwoPi * std::min(fc, sr * 0.45) / sr, al = std::sin(w) / (2 * qq), a0 = 1 + al;
                  b.b0 = (1 - al) / a0, b.b1 = -2 * std::cos(w) / a0, b.b2 = 1.0, b.a1 = b.b1, b.a2 = b.b0;
                } else {
                  b.set(t, sr, fc, 0.5 + reso * 4, shelf / stages);
                }
                b.x1 = x1, b.x2 = x2, b.y1 = y1, b.y2 = y2;
              }
          }
        }
        for (int c = 0; c < 2; ++c) {
          double v = c ? r : l;
          for (int st = 0; st < stages; ++st) {
            if (filt >= 5) {
              v = bq_[st][c].process(v);
            } else {
              double band = 0, high = 0, low = svf_[st][c].process(v, &band, &high);
              v = filt == 1 ? low : filt == 2 ? band : filt == 3 ? high : low + high;
            }
          }
          (c ? r : l) = v;
        }
      }
      if (driveMix > 0) {
        double dl = std::tanh(l * drive), dr = std::tanh(r * drive);
        l = l * (1 - driveMix) + dl * driveMix;
        r = r * (1 - driveMix) + dr * driveMix;
      }
      double pl, pr;
      panGains(pan_, pl, pr);
      l *= vol_ * pl * outG, r *= vol_ * pr * outG;
      if (band > 0) l += restL, r += restR;  // the untouched band (after the input gain, like the rest)
      L[i] = static_cast<float>(L[i] * (1 - mix) + l * mix);
      R[i] = static_cast<float>(R[i] * (1 - mix) + r * mix);
    }
  }
  void reset() {
    for (int k = 0; k < 2; ++k)
      for (int c = 0; c < 2; ++c) split_[k][c].clear(), splitHi_[k][c].clear();
    free_ = 0, vol_ = cut_ = 1, pan_ = 0, fol_ = 0;
    for (auto& st : svf_)
      for (auto& sv : st) sv.reset();
    for (auto& st : bq_)
      for (auto& b : st) b.clear();
  }

 private:
  double free_ = 0, vol_ = 1, cut_ = 1, pan_ = 0, fol_ = 0;
  Svf svf_[3][2];
  Biquad bq_[3][2];
  Biquad split_[2][2], splitHi_[2][2];  // K-257: the band split's two low-pass and two high-pass stages, per side
  double bandHz_ = -1, bandSr_ = 0;
  uint32_t ctr_ = 0;
};


// Gridline — "spacefx" pack: J-1 reference models of the Bucket Chorus and
// the Retro Phaser (included by sfx_mod.h only). Original implementations of
// the behaviour documented in analysis/demos/specs/j1_vchorus.md / j1_vphaser.md.
#pragma once
#include "sfx_common.h"
#include "sfx_flcurves.h"
#include "sfx_resample.h"

namespace gl::packs::spacefx::detail {

// First-order bilinear high / low pass (tan pre-warped), double precision.
struct FirstOrder {
  double b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
  void setHp(double sr, double hz) {
    double k = std::tan(kPi * clampd(hz, 0.01, sr * 0.49) / sr);
    b0 = 1.0 / (1.0 + k), b1 = -b0, a1 = (k - 1.0) / (k + 1.0);
  }
  void setLp(double sr, double hz) {
    double k = std::tan(kPi * clampd(hz, 0.01, sr * 0.49) / sr);
    b0 = k / (1.0 + k), b1 = b0, a1 = (k - 1.0) / (k + 1.0);
  }
  double process(double x) {
    double y = b0 * x + b1 * x1 - a1 * y1;
    x1 = x;
    y1 = flushd(y);
    return y;
  }
  void clear() { x1 = y1 = 0; }
};

// Output gate of the vintage models: opens when either channel reaches the
// threshold, holds, then decays geometrically (hold / release times guessed).
// Starts closed, so hiss alone never opens it.
struct VintageGate {
  double g = 0;
  int hold = 0;
  void clear() { g = 0, hold = 0; }
  double next(double l, double r, double thr, int holdN, double rel) {
    if (std::max(std::fabs(l), std::fabs(r)) >= thr) {
      g = 1;
      hold = holdN;
    } else if (hold > 0) {
      --hold;
    } else {
      g = g * rel < 1e-7 ? 0.0 : g * rel;
    }
    return g;
  }
};

// LFO phase source: free running from the start phase, or locked to the song
// position while the transport runs (tempo sync).
struct RefLfoClock {
  double ph = 0;
  bool started = false;
  // Phase increment for this block and (synced + playing) the locked start.
  void begin(const ProcessContext& ctx, bool sync, double periodBeats, double rateHz, double start, double& inc) {
    const double sr = ctx.sampleRate;
    if (!started) {
      ph = start;
      started = true;
    }
    if (sync) {
      double beatsPerSample = std::max(1.0, ctx.tempo) / 60.0 / sr;
      inc = beatsPerSample / std::max(1e-6, periodBeats);
      if (ctx.playing) {
        double beat = ctx.tick / std::max(1, ctx.ppq);
        ph = beat / std::max(1e-6, periodBeats) + start;
      }
    } else {
      inc = rateHz / sr;
    }
  }
  double next(double inc) {
    double p = ph;
    ph += inc;
    if (ph >= 1e6) ph -= std::floor(ph);
    return p;
  }
  void clear() { started = false, ph = 0; }
};

// ============================================================ Bucket Chorus
// Reference model: two lines (L/R) written through a first-order high pass
// and a 4th-order 7 kHz low pass, read with Hermite interpolation at a delay
// swept between Time 1 and Time 2 (50 ms target smoothing), equal-power mix,
// output gain on dry + wet, hiss and an output gate.
class BbdRef {
 public:
  void init() {
    line_[0].init(static_cast<size_t>(0.04 * kMaxRate));
    line_[1].init(static_cast<size_t>(0.04 * kMaxRate));
    reset();
  }
  void reset() {
    for (int c = 0; c < 2; ++c) {
      line_[c].clear();
      hp_[c].clear();
      lp1_[c].clear();
      lp2_[c].clear();
      post_[c].clear();
      yPrev_[c] = 0;
      d_[c] = -1;
    }
    clock_.clear();
    gate_.clear();
    noise_.seed(0xB0Bu);
    mixS_ = gainS_ = -1;
    sr_ = 0;
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
    const double sr = ctx.sampleRate;
    if (sr != sr_) {
      sr_ = sr;
      for (int c = 0; c < 2; ++c) {
        lp1_[c].set(dsp::Biquad::LowPass, sr, std::min(7000.0, sr * 0.45), 0.7071, 0);
        lp2_[c].set(dsp::Biquad::LowPass, sr, std::min(7000.0, sr * 0.45), 0.7071, 0);
        post_[c].setLp(sr, std::min(7000.0, sr * 0.45));
      }
    }
    const int mode = std::clamp(pi(s, BcMode, 0), 0, 3);
    double rate, t1, t2, lr, mix, fb, hpHz, periodBeats = 1;
    int wave;
    bool invert = false, sync = false;
    if (mode < 3) {
      flc::BcFixed f = flc::bcFixedMode(mode);
      rate = f.rate, wave = f.wave, t1 = f.t1, t2 = f.t2, lr = f.lr;
      mix = 0.5, fb = 0, hpHz = 0.5;
    } else {
      rate = std::max(0.0, static_cast<double>(pv(s, BcRate, 0.5f)));
      wave = pi(s, BcWave, 1);
      t1 = pv(s, BcTime1, 1.7f), t2 = pv(s, BcTime2, 5.4f);
      lr = pv(s, BcPhaseLR, 0.5f);
      mix = clampd(pv(s, BcMix, 0.5f), 0, 1);
      fb = clampd(pv(s, BcFeedback, 0), 0, 0.95);
      hpHz = pv(s, BcHighPass, 50);
      invert = pb(s, BcInvertWet);
      sync = pb(s, BcSync);
      periodBeats = std::max(1.0, static_cast<double>(pv(s, BcSyncTicks, 192))) / 192.0;
    }
    for (int c = 0; c < 2; ++c) hp_[c].setHp(sr, hpHz);
    const bool mono = pb(s, BcMonoIn), gateOn = pb(s, BcNoiseGate);
    const double gain = dbg(pv(s, BcGain, 0));
    const double hiss = 0.2 * flc::bcHissLevel(pv(s, BcHiss, 0.5f));
    const double start = pv(s, BcStartPhase, 0);
    const double cS = 1.0 - std::pow(0.1, 1.0 / (0.05 * sr));  // 50 ms to −20 dB
    const double pol = invert ? -1.0 : 1.0;
    const int holdN = static_cast<int>(0.05 * sr);
    const double rel = std::exp(-1.0 / (0.1 * sr));
    double inc = 0;
    clock_.begin(ctx, sync, periodBeats, rate, start, inc);
    if (mixS_ < 0) mixS_ = mix, gainS_ = gain;
    const double maxD = static_cast<double>(line_[0].size() - 8);
    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i], R[i]};
      if (mono) x[0] = x[1] = 0.5 * (x[0] + x[1]);
      const double ph = clock_.next(inc);
      mixS_ += cS * (mix - mixS_);
      gainS_ += cS * (gain - gainS_);
      const double dg = std::sqrt(std::max(0.0, 1.0 - mixS_)), wg = std::sqrt(std::max(0.0, mixS_)) * pol;
      double out[2];
      for (int c = 0; c < 2; ++c) {
        double p = ph + (c ? lr : 0.0);
        double lfo = wave == 0 ? flc::refSine(p) : flc::refTri(p);
        double target = clampd((t1 + (t2 - t1) * lfo) * 0.001 * sr, 1.0, maxD);
        d_[c] = d_[c] < 0 ? target : d_[c] + cS * (target - d_[c]);
        double y = line_[c].read(d_[c]);
        double w = x[c] + fb * yPrev_[c];
        yPrev_[c] = y;
        line_[c].push(flush(static_cast<float>(lp2_[c].process(lp1_[c].process(hp_[c].process(w))))));
        double wet = y;
        if (hiss > 0) wet += noise_.next() * hiss;
        wet = post_[c].process(wet);
        out[c] = x[c] * dg + wet * wg;
      }
      double g = gainS_;
      if (gateOn && hiss > 0) g *= gate_.next(out[0], out[1], 8.0 * hiss, holdN, rel);
      L[i] = flush(static_cast<float>(out[0] * g));
      R[i] = flush(static_cast<float>(out[1] * g));
    }
  }

 private:
  Ring line_[2];
  FirstOrder hp_[2], post_[2];
  dsp::Biquad lp1_[2], lp2_[2];
  double yPrev_[2] = {}, d_[2] = {-1, -1};
  RefLfoClock clock_;
  VintageGate gate_;
  dsp::Noise noise_;
  double mixS_ = -1, gainS_ = -1, sr_ = 0;
};

// ============================================================== Retro Phaser
// Reference model: colour low shelf → first-order input high pass → drive →
// 4× / 8× oversampled core of four allpass stages (classic: linear first-order
// stages with a one-sample feedback; analog: zero-delay-feedback ladder with
// saturating stages and instantaneous feedback) → mix inside the core →
// downsample; LFO-tracked noise, output gate and a filtered echo.
class PhaserRef {
 public:
  void init() {
    coefs_.init();
    for (auto& ch : up_)
      for (auto& h : ch) h.c = &coefs_;
    for (auto& ch : dn_)
      for (auto& h : ch) h.c = &coefs_;
    echo_[0].init(static_cast<size_t>(4.2 * 96000.0));  // ≥ 4.2 s (8.4 s at 48 kHz)
    echo_[1].init(static_cast<size_t>(4.2 * 96000.0));
    reset();
  }
  void reset() {
    for (int c = 0; c < 2; ++c) {
      for (auto& h : up_[c]) h.clear();
      for (auto& h : dn_[c]) h.clear();
      color_[c].clear();
      inHp_[c].clear();
      fbHp_[c].clear();
      nzShelf_[c].clear();
      eLp_[c].clear();
      eHp_[c].clear();
      echo_[c].clear();
      std::memset(z_[c], 0, sizeof z_[c]);
      std::memset(s_[c], 0, sizeof s_[c]);
      hpS_[c] = yPrev_[c] = 0;
    }
    clock_.clear();
    gate_.clear();
    nz_[0].seed(0x51A5u);
    nz_[1].seed(0x7E11u);
    driveS_ = -1;
    eHeadA_ = eHeadB_ = eSm_ = -1;
    eFade_ = 1;
    nzCount_ = 0;
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
    const double sr = ctx.sampleRate;
    const int os = pb(s, VpHq) ? 8 : 4, stages = os == 8 ? 3 : 2;
    const double fs = sr * os;
    const bool analog = pi(s, VpEngine, 1) == 1;
    double fa = pv(s, VpFreqMin, 180), fb2 = pv(s, VpFreqMax, 2800);
    const double flo = std::max(0.5, std::min(fa, fb2)), fhi = std::max(0.5, std::max(fa, fb2));
    const double octs = std::log2(fhi / flo);
    const bool manual = pb(s, VpManual);
    const double fman = pv(s, VpManualFreq, 700);
    const double fb = clampd(pv(s, VpFeedback, 0.35f), 0, 0.95) * (pb(s, VpInvertFb) ? -1.0 : 1.0);
    const int wave = pi(s, VpWave, 0);
    const double ra = pv(s, VpRateLo, 0.025f), rb = pv(s, VpRateHi, 150);
    const double rlo = std::max(1e-4, std::min(ra, rb)), rhi = std::max(1e-4, std::max(ra, rb));
    const double rate = clampd(pv(s, VpRate, 0.35f), rlo, rhi);
    const double spd = rhi > rlo * 1.000001 ? clampd(std::log(rate / rlo) / std::log(rhi / rlo), 0, 1) : 0.0;
    const double tens = flc::vpTensionK(pv(s, VpTension, 0.5f));
    const double S = clampd(pv(s, VpDepthScale, 1), 0, 1), P = clampd(pv(s, VpDepthPull, 0), 0, 1);
    const double lr = pv(s, VpPhaseLR, 0.25f), start = pv(s, VpStartPhase, 0);
    const bool sync = pb(s, VpSync);
    const double periodBeats = std::max(1.0, static_cast<double>(pv(s, VpSyncTicks, 192))) / 192.0;
    const double mix = clampd(pv(s, VpMix, 0.5f), 0, 1);
    const bool mono = pb(s, VpMonoIn), gateOn = pb(s, VpNoiseGate);
    const double nAmp = flc::vpNoiseAmp(pv(s, VpNoise, 0));
    const double driveT = analog ? dbg(clampd(pv(s, VpDrive, 0), -15, 15)) : 1.0;
    const double cDrive = 1.0 - std::pow(2.0, -86.4 / sr);
    for (int c = 0; c < 2; ++c) {
      keepSet(color_[c], dsp::Biquad::LowShelf, sr, clampd(pv(s, VpColorFreq, 200), 10, sr * 0.45), 0.5, pv(s, VpColorGain, 0));
      inHp_[c].setHp(sr, pv(s, VpInputHp, 10));
    }
    const double fbHpHz = clampd(pv(s, VpFbHp, 10), 1, fs * 0.45);
    const double gh = std::tan(kPi * fbHpHz / fs), Gh = gh / (1.0 + gh);
    for (int c = 0; c < 2; ++c) fbHp_[c].setHp(fs, fbHpHz);
    // Echo.
    const bool echoOn = pb(s, VpDelayOn), keep = pb(s, VpDelayKeep);
    double eSec = pb(s, VpDelaySync) ? std::max(1.0, static_cast<double>(pv(s, VpDelayTicks, 48))) / 192.0 * 60.0 / std::max(1.0, ctx.tempo)
                                     : pv(s, VpDelayTime, 280) * 0.001;
    const double eT = clampd(eSec * sr, 1.0, static_cast<double>(echo_[0].size() - 8));
    const double efb = clampd(pv(s, VpDelayFb, 0.35f), 0, 1), emix = clampd(pv(s, VpDelayMix, 0.3f), 0, 1);
    const double est = clampd(pv(s, VpDelayStereo, 0.5f), 0, 1) * 2.0 - 1.0;
    for (int c = 0; c < 2; ++c) {
      eLp_[c].setLp(sr, pv(s, VpDelayLp, 7000));
      eHp_[c].setHp(sr, pv(s, VpDelayHp, 100));
    }
    const double cGlide = 1.0 - std::pow(2.0, -33.22 / sr);
    const double fadeInc = 1.0 / (0.1 * sr);
    const int holdN = static_cast<int>(0.05 * sr);
    const double rel = std::exp(-1.0 / (0.1 * sr));
    double inc = 0;
    clock_.begin(ctx, sync, periodBeats, rate, start, inc);
    if (driveS_ < 0) driveS_ = driveT;
    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i], R[i]};
      if (mono) x[0] = x[1] = 0.5 * (x[0] + x[1]);
      const double ph = clock_.next(inc);
      driveS_ += cDrive * (driveT - driveS_);
      double lfoY[2] = {0, 0};
      double out[2];
      for (int c = 0; c < 2; ++c) {
        double f;
        if (manual) {
          f = fman;
        } else {
          double p = ph + (c ? lr : 0.0);
          double v = wave == 0 ? flc::refSine(p) : wave == 1 ? flc::refTri(p) : phaserLfo(wave, p, 0.5);
          double y = flc::vpBend(v, tens);
          lfoY[c] = y;
          f = flo * std::pow(2.0, octs * flc::vpSweepPos(y, spd, S, P));
        }
        f = clampd(f, 0.5, fs * 0.49);
        double g = std::tan(kPi * f / fs);
        double in = inHp_[c].process(color_[c].process(x[c])) * driveS_;
        // Oversampled core.
        double buf[8], tmp[8];
        buf[0] = in;
        int cnt = 1;
        for (int st = 0; st < stages; ++st) {
          for (int k = 0; k < cnt; ++k) up_[c][st].up(buf[k], tmp[2 * k], tmp[2 * k + 1]);
          cnt *= 2;
          std::memcpy(buf, tmp, sizeof(double) * static_cast<size_t>(cnt));
        }
        for (int k = 0; k < cnt; ++k) {
          double xo = buf[k];
          double y = analog ? analogStep(c, xo, g, fb, Gh) : classicStep(c, xo, g, fb);
          buf[k] = xo * (1.0 - mix) + y * mix;
        }
        for (int st = stages - 1; st >= 0; --st) {
          cnt /= 2;
          for (int k = 0; k < cnt; ++k) buf[k] = dn_[c][st].down(buf[2 * k], buf[2 * k + 1]);
        }
        out[c] = buf[0] / driveS_;
      }
      if (nAmp > 0) {
        if (nzCount_-- <= 0) {
          nzCount_ = 15;
          for (int c = 0; c < 2; ++c) {
            double hz = 280.0 * std::pow(2.0, (c ? -3.215 : 3.215) * (lfoY[c] - 0.5));
            keepSet(nzShelf_[c], dsp::Biquad::LowShelf, sr, clampd(hz, 10, sr * 0.45), 0.5, 18.0);
          }
        }
        out[0] += nzShelf_[0].process(nz_[0].next()) * nAmp;
        out[1] += nzShelf_[1].process(nz_[1].next()) * nAmp;
        if (gateOn) {
          double g = gate_.next(out[0], out[1], 8.0 * nAmp, holdN, rel);
          out[0] *= g;
          out[1] *= g;
        }
      }
      if (echoOn) {
        double tap[2];
        if (keep) {
          if (eHeadA_ < 0) eHeadA_ = eHeadB_ = eT;
          if (eFade_ >= 1 && std::fabs(eT - eHeadA_) > 0.5) {
            eHeadB_ = eT;
            eFade_ = 0;
          }
          for (int c = 0; c < 2; ++c) {
            double a = echo_[c].readLinear(eHeadA_);
            tap[c] = eFade_ < 1 ? a + (echo_[c].readLinear(eHeadB_) - a) * eFade_ : a;
          }
          if (eFade_ < 1) {
            eFade_ += fadeInc;
            if (eFade_ >= 1) eFade_ = 1, eHeadA_ = eHeadB_;
          }
        } else {
          eSm_ = eSm_ < 0 ? eT : eSm_ + cGlide * (eT - eSm_);
          eHeadA_ = eSm_;
          tap[0] = echo_[0].readLinear(eSm_);
          tap[1] = echo_[1].readLinear(eSm_);
        }
        for (int c = 0; c < 2; ++c) {
          tap[c] = eHp_[c].process(eLp_[c].process(tap[c]));
          echo_[c].push(flush(static_cast<float>(out[c] + efb * tap[c])));
        }
        double m = 0.5 * (tap[0] + tap[1]), d = 0.5 * (tap[0] - tap[1]) * est;
        out[0] += (m + d) * emix;
        out[1] += (m - d) * emix;
      }
      L[i] = flush(static_cast<float>(out[0]));
      R[i] = flush(static_cast<float>(out[1]));
    }
  }

 private:
  static void keepSet(dsp::Biquad& b, dsp::Biquad::Type t, double sr, double hz, double q, double g) {
    double x1 = b.x1, x2 = b.x2, y1 = b.y1, y2 = b.y2;
    b.set(t, sr, hz, q, g);
    b.x1 = x1, b.x2 = x2, b.y1 = y1, b.y2 = y2;
  }
  // Classic engine: one-sample feedback through the high pass, four linear
  // first-order allpasses (a = (t − 1)/(t + 1)).
  double classicStep(int c, double x, double t, double fb) {
    const double a = (t - 1.0) / (t + 1.0);
    double u = fbHp_[c].process(x + fb * yPrev_[c]);
    for (int k = 0; k < 4; ++k) {
      double o = a * u + z_[c][k];
      z_[c][k] = flushd(u - a * o);
      u = o;
    }
    yPrev_[c] = u;
    return u;
  }
  // [7/6] Padé tanh, input clipped to ±5.
  static double pade(double v) {
    v = clampd(v, -5.0, 5.0);
    double q = v * v;
    return v * (((q + 378.0) * q + 17325.0) * q + 135135.0) / (((28.0 * q + 3150.0) * q + 62370.0) * q + 135135.0);
  }
  // Analog engine: TPT allpass ladder, feedback solved instantaneously from
  // the linearised loop, stages saturate on their state difference.
  double analogStep(int c, double x, double g, double fb, double Gh) {
    const double G = g / (1.0 + g), cc = 2.0 * G - 1.0;
    double* st = s_[c];
    const double k = 1.0 - G;
    // Linear prediction of the ladder output: y = cc^4·u + acc.
    double acc = 0, pw = 1;
    for (int j = 3; j >= 0; --j) {
      acc += pw * (1.0 - cc) * st[j];
      pw *= cc;
    }
    const double c4 = pw;  // cc^4
    const double hk = fb * (1.0 - Gh);
    double u = (x + hk * (acc - hpS_[c])) / (1.0 - hk * c4);
    for (int j = 0; j < 4; ++j) {
      double t = pade(u - st[j]);
      double w = G * t;
      double lp = st[j] + w;
      st[j] = flushd(lp + w);
      u = lp - k * t;
    }
    double y = u;
    double v5 = (y - hpS_[c]) * Gh;
    double lp5 = hpS_[c] + v5;
    hpS_[c] = flushd(lp5 + v5);
    return y;
  }
  HalfbandCoefs coefs_;
  Halfband2x up_[2][3], dn_[2][3];
  dsp::Biquad color_[2], nzShelf_[2];
  FirstOrder inHp_[2], fbHp_[2], eLp_[2], eHp_[2];
  double z_[2][4] = {}, s_[2][4] = {}, hpS_[2] = {}, yPrev_[2] = {};
  Ring echo_[2];
  RefLfoClock clock_;
  VintageGate gate_;
  dsp::Noise nz_[2];
  double driveS_ = -1, eHeadA_ = -1, eHeadB_ = -1, eSm_ = -1, eFade_ = 1;
  int nzCount_ = 0;
};

}  // namespace gl::packs::spacefx::detail

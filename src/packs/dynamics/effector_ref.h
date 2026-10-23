// Gridline — "dynamics" pack, J-26: reference engine of gl.effector
// (EfModel = 1). Behaviour study.
//
// Twelve modules in a fixed chain; the selected one gets the input (others
// with a tail fade their input out and ring on). All settings come from the
// twelve stored 0..1 values (kind, bypass, wet, X, Y, rate, tempo reduce,
// modulation amounts, shape, input, output). Buffers are sized in the
// constructor (up to 96 kHz at full length; longer delays clamp above that).
#pragma once
#include <array>
#include <cmath>
#include <vector>
#include "dyn_dsp.h"
#include "../../engine/FullReverbRef.h"

namespace gl::packs::dynamics::efref {

inline constexpr int kKinds = 12;
inline constexpr double kRateBeats[9] = {0.125, 0.25, 0.375, 0.5, 0.75, 1, 2, 4, 8};
inline int kindOf(double n) { return std::clamp(static_cast<int>(11 * n + 0.5), 0, 11); }
inline int rateOf(double n) { return std::clamp(static_cast<int>(8 * n + 0.5), 0, 8); }
inline int shapeOf(double n) { return std::clamp(static_cast<int>(2 * n + 0.5), 0, 2); }
inline double modAmount(double n) {
  double a = 2 * n - 1;
  return a * std::fabs(a);
}
inline double outGain(double n) { return std::pow(10.0, 2.4 * n - 1.2); }
// LFO length in beats (rate choice, tempo reduce 0..1).
inline double lengthBeats(double rateN, double reduceN) {
  double r = 1 - std::clamp(reduceN, 0.0, 1.0);
  return std::max(1.0 / 512, kRateBeats[rateOf(rateN)] * r * r);
}

// Variable-speed ("tape") delay: the delay glides toward its target through a
// smoothed playback speed; loop path = two one-poles (tone) and a gain.
struct TapeLine {
  DelayLine line{262144};
  double speed = 0, y1 = 0, y2 = 0;
  bool primed = false;
  void reset() {
    line.reset();
    y1 = y2 = 0;
    primed = false;
  }
  // Returns the loop output (tone-filtered read); writes in + out·gain.
  double tick(double in, double delay, double c1, double c2, double gain) {
    double target = 1.0 / std::max(2.0, delay);
    if (!primed) speed = target, primed = true;
    speed += (target - speed) * 0.005;
    double d = std::min(1.0 / speed, static_cast<double>(line.mask - 4));
    double r = line.frac(d);
    y1 += (r - y1) * c1;
    y2 += (y1 - y2) * c2;
    double out = y1 - y2;
    double w = in + out * gain;
    if (std::fabs(w) < 1e-9) w = 0;
    line.push(w);
    return out;
  }
};

class Effector {
 public:
  Effector() : stut_{std::vector<float>(kStutMax, 0.f), std::vector<float>(kStutMax, 0.f)} {
    for (auto& m : fade_) m = 0;
    rev_.init(fullrev::Variant::Multi, 44100, 44100 / 4);  // the module runs its reverb at 44.1 kHz
  }
  void reset() {
    for (int k = 0; k < kKinds; ++k) clearModule(k);
    primed_ = false;
    count_ = 0;
  }
  // v = the twelve stored values. posBeats = song position at the block start.
  void process(const float* v, double sr, double tempo, double posBeats, float* L, float* R, int n) {
    if (sr != sr_) {
      sr_ = sr;
      reset();
    }
    const int kind = kindOf(v[0]);
    const bool bypass = v[1] != 0;
    const double X = std::clamp<double>(v[3], 0, 1), Y = std::clamp<double>(v[4], 0, 1);
    const double aX = modAmount(v[7]), aY = modAmount(v[8]);
    const int shape = shapeOf(v[9]), rate = rateOf(v[5]);
    const double len = lengthBeats(v[5], v[6]);
    const double T = len * 60.0 / std::max(1.0, tempo);
    const double inc = 1.0 / (T * sr);
    if (!primed_ || rate != rate_) {  // phase resync to the song position
      double ph = posBeats / len;
      phase_ = ph - std::floor(ph);
    }
    rate_ = rate;
    const double inT = static_cast<double>(v[10]) * v[10], outT = outGain(v[11]), wT = std::clamp<double>(v[2], 0, 1);
    if (!primed_) gIn_ = inT, gOut_ = outT, wet_ = wT, primed_ = true;
    double XB = curX_, YB = curY_;
    for (int i = 0; i < n; ++i) {
      if ((count_++ & (kControl - 1)) == 0) {  // block-rate modulation, on a fixed 64-sample grid
        const double mB = lfo(shape, phase_, inc);
        XB = std::clamp(X + mB * aX, 0.0, 1.0), YB = std::clamp(Y + mB * aY, 0.0, 1.0);
        blockSetup(XB, YB, T);
        curX_ = XB, curY_ = YB;
      }
      gIn_ += (inT - gIn_) * 0.01;
      gOut_ += (outT - gOut_) * 0.005;
      wet_ += (wT - wet_) * 0.01;
      const double w = wet_;
      double l = L[i] * gIn_, r = R[i] * gIn_;
      const double m = lfo(shape, phase_, inc);
      for (int k = 0; k < kKinds; ++k) {
        const bool on = !bypass && k == kind;
        if (on) {
          if (fade_[k] <= 0 && k == 9) stutW_ = 0, stutR_ = 0;  // stutter records from activation
          fade_[k] = 1;
        } else if (fade_[k] > 0) {
          if (!hasTail(k)) {
            fade_[k] = 0;
            continue;
          }
          fade_[k] += (0 - fade_[k]) * 0.01;
          if (fade_[k] < 1e-4) {
            fade_[k] = 0;
            clearModule(k);
            continue;
          }
        } else {
          continue;
        }
        module(k, fade_[k], w, X, Y, XB, YB, m, aX, aY, l, r);
      }
      const double prev = phase_;
      phase_ += inc;
      if (phase_ >= 1) phase_ -= std::floor(phase_);
      if (phase_ < prev) stutR_ = 0;  // stutter replays from the slice start each cycle
      L[i] = static_cast<float>(l * gOut_);
      R[i] = static_cast<float>(r * gOut_);
    }
  }
  double curX() const { return curX_; }
  double curY() const { return curY_; }

 private:
  static constexpr size_t kStutMax = 144000;  // 1.5 s at 96 kHz
  static constexpr uint64_t kControl = 64;     // control-rate block (the original evaluates once per host block)
  static bool hasTail(int k) { return k == 2 || k == 3 || k == 4 || k == 5 || k == 6 || k == 7 || k == 10 || k == 11; }
  static double lfo(int shape, double ph, double inc) {
    if (shape == 0) return std::sin(2 * kPi * ph);
    if (shape == 1) {
      double f = ph + inc;
      return 1 - 2 * (f - std::floor(f));
    }
    return ph <= 0.5 ? 1.0 : -1.0;
  }
  void clearModule(int k) {
    for (int c = 0; c < 2; ++c) {
      switch (k) {
        case 0: dlp_[c] = 0; break;
        case 1: held_[c] = 0, crushC_ = 0; break;
        case 2: flg_[c].reset(); break;
        case 3:
          for (double& s : phs_[c]) s = 0;
          phY_[c] = 0;
          break;
        case 4:
          for (double& s : flt_[c]) s = 0;
          fltLast_[c] = 0;
          break;
        case 5: echo_[c].reset(); break;
        case 6: rev_.resume(); break;
        case 7: pan_[c].reset(); break;
        case 8: trance_ = 0; break;
        case 9: stutW_ = stutR_ = 0; break;
        case 10:
          for (auto& b : vow_[c]) b.reset();
          break;
        default: break;
      }
    }
  }
  // Settings evaluated once per block.
  void blockSetup(double XB, double YB, double T) {
    // Crusher
    crushHold_ = 128 * XB * XB * XB + 1;
    crushQ_ = 65536.0 / (1 + 2048 * YB * YB);
    // Filter
    double p = 0.8 * XB + 0.1, y2 = std::min(1.0, YB);
    bool hp = p > 0.5;
    if (hp != fltHp_) {
      for (int c = 0; c < 2; ++c) {
        for (double& s : flt_[c]) s = 0;
        fltLast_[c] = 0;
      }
      fltHp_ = hp;
    }
    if (!hp) {
      double c = 2 * p;
      fltG_ = c * c * c;
      double k = c > 0.845 ? (6.4516139 - 12.903228 * p) : 1.0;
      fltR_ = (c > 0.845 ? k * k : 1.0) * (0.7 * y2 + 0.2);
    } else {
      double a = 2 * p - 1;
      fltG_ = 0.5 * a * a * a;
      fltR_ = 6.2568 * y2;
    }
    // Echo: 2/3 of the LFO period, folded under one second, on a 1.5 s line.
    double t = T * 2.0 / 3.0;
    if (t < 0.001) t = 0.001;
    while (t > 1) t *= 0.5;
    const double N = std::floor(1.5 * sr_);
    echoDelay_ = 2 + (N - 2) * t;
    echoFb_ = XB * XB;
    if (YB <= 0.5) echoC1_ = 1.98 * YB + 0.01, echoC2_ = 0;
    else echoC1_ = 1, echoC2_ = 0.45 * YB - 0.225;
    // Reverb: the full reverb engine, re-set every control block (it runs at a fixed 44.1 kHz)
    {
      float rn[fullrev::MpCount];
      for (int k = 0; k < fullrev::MpCount; ++k) rn[k] = fullrev::kMultiDefaults[k];
      const float xf = static_cast<float>(XB), yf = static_cast<float>(YB), x3 = (xf * xf) * xf;
      rn[fullrev::MpDamp] = yf * yf;
      rn[fullrev::MpDecay] = 0.75f * x3;
      rn[fullrev::MpWet] = x3 * 0.225f + 0.5f;
      rn[fullrev::MpHighCut] = 1, rn[fullrev::MpLowCut] = 0.1f, rn[fullrev::MpDiffusion] = 1, rn[fullrev::MpRoom] = 1;
      rn[fullrev::MpPredelay] = 0, rn[fullrev::MpDry] = 0, rn[fullrev::MpMuteEr] = 1;
      rev_.setMulti(rn);
    }
    // Vowel
    double vv = 3 * XB;
    int vi = static_cast<int>(vv);
    double vt = vv - vi;
    if (vi >= 3) vi = 0;
    double wA = 0, wB = 0, wC = 0;
    if (vi == 0) wA = 1 - vt, wB = vt;
    else if (vi == 1) wB = 1 - vt, wC = vt;
    else wC = 1 - vt, wA = vt;
    static const double fA[8] = {250, 700, 2500, 3500, 4200, 8500, 10000, 12000};
    static const double fB[8] = {700, 1250, 2500, 3500, 4200, 8500, 10000, 12000};
    static const double fC[8] = {250, 2500, 2700, 3500, 4200, 8500, 10000, 12000};
    const double shift = std::pow(2.0, 1.86 * YB - 1);
    for (int k = 0; k < 8; ++k) {
      double f = (wA * fA[k] + wB * fB[k] + wC * fC[k]) * shift;
      for (int c = 0; c < 2; ++c) vow_[c][k].setBandSkirt(f, 12, sr_);
    }
    // Ring
    ringInc_ = std::pow(2.0, 8.7 * XB + 5) / sr_;
  }
  struct Bp {  // RBJ band-pass, constant skirt gain (peak gain = Q)
    double b0 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void setBandSkirt(double f, double q, double sr) {
      double w = 2 * kPi * std::clamp(f, 10.0, sr * 0.49) / sr, al = std::sin(w) / (2 * q), a0 = 1 + al;
      b0 = q * al / a0, b2 = -b0, a1 = -2 * std::cos(w) / a0, a2 = (1 - al) / a0;
    }
    double tick(double x) {
      double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1, x1 = x, y2 = y1, y1 = y;
      return y;
    }
    void reset() { x1 = x2 = y1 = y2 = 0; }
  };

  void module(int k, double f, double w, double X, double Y, double XB, double YB, double m, double aX, double aY, double& l,
              double& r) {
    double* io[2] = {&l, &r};
    switch (k) {
      case 0: {  // Disto: one-pole split, the band below goes through a soft-knee chain
        double d = std::clamp(0.95 * X + 0.05, 0.0, 1.0);
        d *= d;
        const double kk = 2048 * d, c = std::pow(0.9 * Y + 0.1, 4);
        auto chain = [kk](double s) {
          double t1 = s / (kk * std::fabs(s) + 1);
          double t2 = t1 / (kk * t1 * t1 + 1);
          double t3 = t2 / (kk * std::fabs(t2 * t2 * t2) + 1);
          return t3 / (kk * t3 * t3 * t3 * t3 + 1);
        };
        const double norm = 0.5 / chain(0.5);
        for (int ch = 0; ch < 2; ++ch) {
          double x = *io[ch];
          dlp_[ch] += (x - dlp_[ch]) * c;
          *io[ch] = x + w * (norm * chain(dlp_[ch]) - dlp_[ch]);
        }
        break;
      }
      case 1: {  // Crusher: sample-and-hold + truncating quantiser
        bool take = crushC_ <= 0;
        if (take) crushC_ += crushHold_ - 1;
        else crushC_ -= 1;
        for (int ch = 0; ch < 2; ++ch) {
          double x = *io[ch];
          if (take) held_[ch] = std::trunc(x * crushQ_) / crushQ_;
          *io[ch] = x + (held_[ch] - x) * w;
        }
        break;
      }
      case 2: {  // Flanger
        const double N = std::floor(0.005 * sr_);
        double D = std::clamp(X * X + m * aX, 0.0, 1.0);
        double Yp = std::clamp(Y + m * aY, 0.0, 1.0);
        double delay = 2 + (N - 2) * D;
        for (int ch = 0; ch < 2; ++ch) {
          double x = *io[ch];
          double o = flg_[ch].tick(-x * f, delay, 1, 0, -0.96 * Yp);
          *io[ch] = x + o * w;
        }
        break;
      }
      case 3: {  // Phaser: six first-order all-passes with feedback
        phB_ += (std::max(1e-5, X * X) - phB_) * 0.001;
        double Xp = std::min(1.0, X + m * aX), Yp = std::clamp(Y + m * aY, 0.0, 1.0);
        double fb = 0.98 * std::cbrt(0.6 * std::min(Yp, 1.0));
        double freq = phB_ * (1 + 0.5 * Xp) * 19950 + 50;
        double w0 = std::clamp(freq / sr_, 0.001, 0.499), a = (1 - w0) / (1 + w0);
        for (int ch = 0; ch < 2; ++ch) {
          double x = *io[ch];
          double u = x * f + fb * phY_[ch];  // (the original adds a -1e-6 denormal guard here; flushed below instead)
          for (double& s : phs_[ch]) {
            double vv = s - a * u;
            s = u + a * vv;
            u = vv;
          }
          if (std::fabs(u) < 1e-20) u = 0;
          for (double& st : phs_[ch])
            if (std::fabs(st) < 1e-20) st = 0;
          phY_[ch] = u;
          *io[ch] = x + u * w;
        }
        break;
      }
      case 4: {  // Filter: low pass = four 8-pole groups, each with feedback from its last
                 // pole and a make-up gain; high pass = 32 one-pole high-passes with feedback
                 // from the 16th pole's state. Keeps running (unfaded) while it fades out.
        (void)f;
        for (int ch = 0; ch < 2; ++ch) {
          double x = *io[ch], in = x;
          double* st = flt_[ch];
          if (!fltHp_) {
            for (int grp = 0; grp < 4; ++grp) {
              double* g8 = st + grp * 8;
              double s7 = g8[7];
              g8[0] += fltG_ * (in - fltR_ * s7 - g8[0]);
              for (int q = 1; q < 8; ++q) g8[q] += fltG_ * (g8[q - 1] - g8[q]);
              in = g8[7] * (1 + 0.6 * fltR_);
            }
          } else {
            in -= fltR_ * st[15];
            for (int q = 0; q < 32; ++q) {
              st[q] += fltG_ * (in - st[q]);
              in -= st[q];
            }
          }
          for (int q = 0; q < 32; ++q)
            if (std::fabs(st[q]) < 1e-25) st[q] = 0;
          *io[ch] = x + (in - x) * w;
        }
        break;
      }
      case 5: {  // Echo
        for (int ch = 0; ch < 2; ++ch) {
          double x = *io[ch];
          double e = echo_[ch].tick(x * f, echoDelay_, echoC1_, echoC2_, echoFb_);
          *io[ch] = x + e * w;
        }
        break;
      }
      case 6: {  // Reverb: the engine adds its output to the module's own buffers
        const float a = static_cast<float>(f * w);
        float ol = static_cast<float>(l), orr = static_cast<float>(r);
        rev_.tickMulti(static_cast<float>(l) * a, static_cast<float>(r) * a, ol, orr);
        l = ol, r = orr;
        break;
      }
      case 7: {  // Panner: Haas delay on one side + amplitude law
        const double N = std::floor(0.002 * sr_);
        double Xp = std::clamp(X + m * aX, 0.0, 1.0), Yp = std::clamp(Y + m * aY, 0.0, 1.0);
        double p = 2 * Xp - 1, q = p * 2 * Yp;
        double gL = std::clamp(2 - q, 0.0, 1.0), gR = std::clamp(2 + q, 0.0, 1.0);
        double a = f * w;
        double dl = 2 + (N - 2) * std::max(0.0, -p), dr = 2 + (N - 2) * std::max(0.0, p);
        double ol = pan_[0].tick(l * gL, dl, 1, 0, 0), orr = pan_[1].tick(r * gR, dr, 1, 0, 0);
        l = l * (1 - a) + ol * a;
        r = r * (1 - a) + orr * a;
        break;
      }
      case 8: {  // Trancer: a duck at every cycle start
        double kk = 1 + 50 * (1 - X) * (1 - X), depth = 1 - Y * Y;
        double tgt;
        if (phase_ < 0.01) {
          tgt = 100 * phase_;
        } else {
          double u = 1.010101 * phase_ - 0.010101;
          tgt = 1 - std::min(1.0, kk * u * u * u * u);
        }
        trance_ += (tgt - trance_) * 0.01;
        double g = 1 - trance_ * depth;
        l *= g;
        r *= g;
        break;
      }
      case 9: {  // Stutter: the first slice after activation, replayed every cycle
        const size_t N = std::min(kStutMax, static_cast<size_t>(1.5 * sr_));
        bool play = stutR_ < stutW_;
        for (int ch = 0; ch < 2; ++ch) {
          double x = *io[ch];
          if (stutW_ < N) stut_[ch][stutW_] = static_cast<float>(x);
          if (play) *io[ch] = x + (stut_[ch][stutR_] - x) * w;
        }
        if (stutW_ < N) ++stutW_;
        if (play) ++stutR_;
        break;
      }
      case 10: {  // Vowel: eight formant band-passes
        double a = f * w;
        for (int ch = 0; ch < 2; ++ch) {
          double x = *io[ch], sum = 0;
          for (int b = 0; b < 8; ++b) sum += vow_[ch][b].tick(x * f) * (1 - 0.125 * b);
          *io[ch] = x * (1 - a) + sum * a;
        }
        break;
      }
      default: {  // Ring
        double a = f * w, s = std::sin(2 * kPi * ringPh_);
        ringPh_ += ringInc_;
        ringPh_ -= std::floor(ringPh_);
        l = l * (1 - a) + l * s * a;
        r = r * (1 - a) + r * s * a;
        break;
      }
    }
    (void)XB, (void)YB;
  }

  uint64_t count_ = 0;
  double sr_ = 0, phase_ = 0, gIn_ = 1, gOut_ = 1, wet_ = 1, curX_ = 0.5, curY_ = 0.5;
  bool primed_ = false;
  int rate_ = -1;
  std::array<double, kKinds> fade_{};
  // module state
  double dlp_[2] = {}, held_[2] = {}, crushC_ = 0, crushHold_ = 1, crushQ_ = 65536;
  TapeLine flg_[2], echo_[2], pan_[2];
  double phs_[2][6] = {}, phY_[2] = {}, phB_ = 1e-5;
  double flt_[2][32] = {}, fltLast_[2] = {}, fltG_ = 0, fltR_ = 0;
  bool fltHp_ = false;
  double echoDelay_ = 2, echoFb_ = 0, echoC1_ = 1, echoC2_ = 0;
  fullrev::Engine rev_;
  double trance_ = 0;
  std::vector<float> stut_[2];
  size_t stutW_ = 0, stutR_ = 0;
  Bp vow_[2][8];
  double ringPh_ = 0, ringInc_ = 0;
};

}  // namespace gl::packs::dynamics::efref

// Gridline — "models" pack, J-33: Twin String reference engine (see twinref.h).
#include <algorithm>
#include <cmath>
#include "../synths/hex_fx.h"
#include "strings_laws.h"
#include "twinref.h"

namespace gl::packs::models::strs {

namespace {

constexpr int kVoices = 16;
constexpr int kBuf = 48000;
constexpr int kChunk = 48;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double c01(double x) { return std::clamp(x, 0.0, 1.0); }
double p5(double x) { return x * x * x * x * x; }

// The reference's 2-pole high pass (numerator α = s/2Q, denominator s/Q).
struct Hp2 {
  double b = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  void set(double fc, double Q, double rate) {
    const double w = 2.0 * kPi * fc / rate, s = std::sin(w), co = std::cos(w);
    b = (1.0 + co) / (2.0 + s / Q), a1 = -2.0 * Q * co / (Q + s), a2 = (Q - s) / (Q + s);
  }
  double run(double x) {
    const double y = b * (x - 2.0 * x1 + x2) - a1 * y1 - a2 * y2;
    x2 = x1, x1 = x, y2 = y1, y1 = y;
    return y;
  }
  void clear() { x1 = x2 = y1 = y2 = 0; }
};

}  // namespace

struct TwinRef::Voice {
  bool on = false, fresh = false;
  uint32_t id = 0;
  uint64_t start = 0;
  int key = 60;
  double vel = 0, t = 0, relT = -1, relSrc = 0;
  // Free envelope.
  int egStage = 4;  // 0 attack, 1 decay, 2 sustain, 3 release, 4 done
  double egPh = 0, egLevel = 0, egFrom = 0;
  // Amp / hi-cut envelopes.
  double amp = 0, ampT = 0, ampC = 0, hEnv = 0, hT = 0, hC = 0;
  // Exciter.
  double held = 0, thr = 1, pmix = 1, drive = 0, k2 = 1, lfb = 0;
  Hp2 lo;
  double s1 = 0, s2 = 0, s3 = 0, s4 = 0;
  // Strings: [string][lane] buffers, comb per string.
  std::vector<float> buf[2][2], comb[2];
  int len[2] = {1, 1}, clen[2] = {1, 1}, ptr[2] = {0, 0}, cptr[2] = {0, 0};
  double dc[2][2] = {}, q1[2][2] = {}, q2[2][2] = {}, c[2][2] = {}, k[2][2] = {};
  double G[2] = {0, 0}, Gs[2] = {0, 0};
  double gT[4] = {}, g[4] = {};  // 1L 1R 2L 2R
  double fbComb = 0, levComb = 0, sigma = 0;
  double fExact = 440, fInt = 440, ratio = 1, ph = 0;
  double prevL = 0, prevR = 0, curL = 0, curR = 0;
  bool sounded = false;
  int quiet = 0;
};

struct TwinRef::Fx {
  Hp2 dc[2];
  synths::hex::BbdLine resA[8], resB[8];
  bool resOn[8] = {};
  Hp2 resHp[2];
  synths::hex::BbdLine cho[3], dly[6];
  synths::hex::PlateReverb rev;
  double sr = 0;
};

TwinRef::TwinRef() : fx_(std::make_unique<Fx>()) {}
TwinRef::~TwinRef() = default;

void TwinRef::configure(const float* p, double sr, double bpm) {
  std::copy(p, p + TsCount, p_);
  bpm_ = bpm > 1 ? bpm : 120.0;
  if (v_.empty()) {
    v_.resize(kVoices);
    for (auto& v : v_) {
      for (int s = 0; s < 2; ++s) {
        v.buf[s][0].assign(kBuf, 0.f), v.buf[s][1].assign(kBuf, 0.f);
        v.comb[s].assign(kBuf, 0.f);
      }
    }
  }
  Fx& f = *fx_;
  if (sr != f.sr) {
    sr_ = sr > 0 ? sr : 44100.0;
    f.sr = sr_;
    step_ = kRate / sr_;
    bufL_.assign(static_cast<size_t>(kChunk * step_) + 8, 0.0);
    bufR_.assign(bufL_.size(), 0.0);
    pos_ = 0, chunkLeft_ = 0;
    for (auto& h : f.dc) h.set(32.0, 1.0, sr_), h.clear();
    for (auto& h : f.resHp) h.set(32.0, 1.0, sr_), h.clear();
    const size_t N = static_cast<size_t>(std::max(4.0, std::trunc(0.1 * sr_)));
    for (int b = 0; b < 8; ++b) {
      for (auto* l : {&f.resA[b], &f.resB[b]}) {
        l->reserve(N);
        l->configure(N, 0);
        l->setLoopLowpass(true);
        l->setMixNow(1.f);
      }
      f.resOn[b] = false;
    }
    const double t2 = std::trunc(sr_ * 0.02);
    for (auto& l : f.cho) l.reserve(static_cast<size_t>(sr_ * 0.03));
    f.cho[0].configure(static_cast<size_t>(std::round(t2 * 1.1)), 0);
    f.cho[1].configure(static_cast<size_t>(std::round(t2 * 0.854)), 0.467);
    f.cho[2].configure(static_cast<size_t>(sr_ * 0.02), 0.778);
    const double t = std::trunc(sr_ * 2.0);
    const double len[6] = {t, std::round(t * 0.63), std::round(t * 0.41), t, std::round(t * 0.76), std::round(t * 0.54)};
    const double st[6] = {0, 0.27, 0.4, 0.3, 0.62, 0.87};
    for (int k = 0; k < 6; ++k) f.dly[k].reserve(static_cast<size_t>(len[k])), f.dly[k].configure(static_cast<size_t>(len[k]), st[k]);
    f.rev.configure(sr_);
    f.rev.setParam(9, 1.0);  // tank damping off
    f.rev.setParam(5, 1.0);  // diffusion 100
  }
  // Resonators: switching a band on clears it.
  for (int b = 0; b < 8; ++b) {
    const bool on = p_[TsResOn1 + b] >= 0.5f;
    if (on && !f.resOn[b]) {
      f.resA[b].reset(), f.resB[b].reset();
      f.resA[b].setMixNow(1.f), f.resB[b].setMixNow(1.f);
    }
    f.resOn[b] = on;
    const double pr = 1.0 - std::pow(c01(p_[TsResFreq1 + b]), 0.1);
    f.resA[b].setTime(std::clamp(pr, 0.0, 0.99));
    f.resB[b].setTime(std::clamp(1.05 * pr, 0.0, 0.99));
    const float fb = static_cast<float>(0.9999 * std::sqrt(c01(p_[TsResFb1 + b])));
    f.resA[b].setFeedback(fb), f.resB[b].setFeedback(fb);
  }
  // Chorus.
  {
    const double rate = 5.0 * c01(p_[TsChoRate]) + 0.1;
    f.cho[0].setRate(rate * 0.896, sr_), f.cho[1].setRate(rate, sr_), f.cho[2].setRate(rate * 0.689, sr_);
    const double d = c01(p_[TsChoDepth]);
    for (auto& l : f.cho) l.setMix(static_cast<float>(c01(p_[TsChoWet]))), l.setDepth(d * d * d), l.setFeedback(0);
  }
  // Reverb.
  {
    const double x = c01(p_[TsRevMix]), d = c01(p_[TsRevDecay]), c = c01(p_[TsRevColor]);
    f.rev.setParam(12, 0.999 * x + 0.001);
    f.rev.setParam(8, 0.6 * d * d * d);
    f.rev.setParam(2, c * c * c);
  }
  // Delay.
  {
    const double tm = c01(p_[TsDlyTime]), rt = c01(p_[TsDlySpeed]), dp = c01(p_[TsDlyDepth]);
    double t = tm * tm + 0.01;
    const int sync = std::clamp(static_cast<int>(std::lround(p_[TsDlySync])), 0, 9);
    if (sync != 0) {
      static const double kDiv[10] = {1, 15, 30, 45, 60, 120, 180, 240, 480, 960};
      t = 0.5 / (bpm_ / kDiv[sync]);
    }
    const float fb = static_cast<float>(-0.99 * c01(p_[TsDlyFb]) * c01(p_[TsDlyFb]));
    const double w = c01(p_[TsDlyWet]), bl = c01(p_[TsDlyBlur]);
    const float mixes[3] = {static_cast<float>(w * w), static_cast<float>(w * w * bl), static_cast<float>((w * bl) * (w * bl))};
    for (int k = 0; k < 6; ++k) {
      // Lines L1 L2 L3 R1 R2 R3: rate ×1 for L1, L3, R2; ×1.1 for L2, R1, R3.
      const bool fast = k == 1 || k == 3 || k == 5;
      f.dly[k].setRate(20.0 * rt * rt * (fast ? 1.1 : 1.0), sr_);
      f.dly[k].setTime(t);
      f.dly[k].setFeedback(fb);
      f.dly[k].setDepth(dp * dp * dp * dp);
      f.dly[k].setMix(mixes[k % 3]);
    }
  }
}

void TwinRef::hardReset(Voice& v) {
  for (int s = 0; s < 2; ++s) {
    for (int l = 0; l < 2; ++l) {
      std::fill(v.buf[s][l].begin(), v.buf[s][l].end(), 0.f);
      v.dc[s][l] = v.q1[s][l] = v.q2[s][l] = 0;
    }
    std::fill(v.comb[s].begin(), v.comb[s].end(), 0.f);
    v.ptr[s] = v.cptr[s] = 0;
  }
  v.lo.clear();
  v.s1 = v.s2 = v.s3 = v.s4 = 0;
  v.amp = v.hEnv = 0;
  v.egStage = 4, v.egLevel = 0;
  v.held = 0;
  v.prevL = v.prevR = v.curL = v.curR = 0;
  v.ph = 0;
  v.sounded = false, v.quiet = 0;
  v.on = false;
}

void TwinRef::noteOn(uint32_t id, double pitch, float vel) {
  Voice* v = nullptr;
  for (auto& x : v_)
    if (!x.on) {
      v = &x;
      break;
    }
  if (!v) {
    v = &v_[0];
    for (auto& x : v_)
      if (x.start < v->start) v = &x;
    hardReset(*v);
  }
  const int key = static_cast<int>(std::lround(pitch));
  v->on = true, v->fresh = true;
  v->id = id, v->start = counter_++, v->key = key;
  v->vel = std::clamp(static_cast<double>(vel), 0.0, 1.0);
  static const double kBase[7] = {27.5, 55, 110, 220, 440, 880, 1760};
  const int tr = std::clamp(static_cast<int>(std::lround(p_[TsTranspose])), 0, 6);
  v->fExact = kBase[tr] * std::exp2(key / 12.0 - 4.75);
  const int L = std::clamp(static_cast<int>(std::trunc(kRate / v->fExact)), 1, kBuf);
  int L1 = L, L2 = L;
  switch (std::clamp(static_cast<int>(std::lround(p_[TsS2Ratio])), 0, 6)) {
    case 0: L1 = L - L % 4, L2 = L1 / 4; break;
    case 1: L1 = 3 * (L / 3), L2 = L / 3; break;
    case 2: L1 = L - L % 2, L2 = L1 / 2; break;
    case 3: L2 = L; break;
    case 4: L2 = 2 * L; break;
    case 5: L2 = 3 * L; break;
    default: L2 = 4 * L; break;
  }
  L1 = std::clamp(L1, 1, kBuf);
  L2 = L2 < 2 ? 1 : std::min(L2, kBuf);
  v->len[0] = L1, v->len[1] = L2;
  for (int s = 0; s < 2; ++s) {
    v->ptr[s] %= v->len[s];
    v->clen[s] = std::clamp(static_cast<int>(std::trunc(v->len[s] * static_cast<double>(p_[TsCombRatio]))), 1, kBuf);
    v->cptr[s] %= v->clen[s];
  }
  v->fInt = kRate / L1;
  v->t = 0, v->relT = -1, v->relSrc = 0;
  v->egFrom = v->egStage == 4 ? 0.0 : v->egLevel;
  v->egStage = 0, v->egPh = 0;
  v->lo.clear();
}

void TwinRef::noteOff(uint32_t id) {
  for (auto& v : v_)
    if (v.on && v.id == id && v.relT < 0) {
      v.relSrc = 1.0;
      v.relT = 0;
      if (v.egStage < 3) v.egFrom = v.egLevel, v.egStage = 3, v.egPh = 0;
    }
}

void TwinRef::allOff() {
  for (auto& v : v_)
    if (v.on) noteOff(v.id);
}

void TwinRef::kill() {
  for (auto& v : v_) hardReset(v);
  Fx& f = *fx_;
  for (int b = 0; b < 8; ++b) f.resA[b].reset(), f.resB[b].reset(), f.resA[b].setMixNow(1.f), f.resB[b].setMixNow(1.f);
  for (auto& l : f.cho) l.reset();
  for (auto& l : f.dly) l.reset();
  f.rev.reset();
  for (auto& h : f.dc) h.clear();
  for (auto& h : f.resHp) h.clear();
  tail_ = 0;
}

bool TwinRef::active() const {
  if (tail_ > 0) return true;
  for (const auto& v : v_)
    if (v.on) return true;
  return false;
}

double TwinRef::mod(const Voice& v, int dest) const {
  double sum = 0;
  bool any = false;
  for (int s = 0; s < kTsMods; ++s) {
    const int src = static_cast<int>(std::lround(p_[tsMod(s, TmSource)]));
    if (src <= 0 || src >= 9 || static_cast<int>(std::lround(p_[tsMod(s, TmDest)])) != dest) continue;
    any = true;
    double val = 0;
    switch (src) {
      case MsEnv: val = v.egLevel; break;
      case MsLfo: val = lfo_ * (dest == MdLfoDepth ? 1.0 : mod(v, MdLfoDepth)); break;
      case MsModWheel: val = mw_; break;
      case MsPitchWheel: val = pw_; break;
      case MsPressure: val = at_; break;
      case MsVelocity: val = v.vel; break;
      case MsKey: val = 0.0125 * v.key; break;
      default: val = v.relSrc; break;
    }
    const double a = c01(p_[tsMod(s, TmAmount)]);
    sum += (p_[tsMod(s, TmNegative)] >= 0.5f ? -a * a : a * a) * val;
  }
  if (!any) return dest == MdLfoDepth || dest == MdLevel ? 1.0 : 0.0;
  return sum;
}

void TwinRef::control(Voice& v, int n48) {
  const float* p = p_;
  // Amp envelope.
  const double A = 35.0 * p5(c01(p[TsAmpA])) + 0.001, D = 50.0 * p5(c01(p[TsAmpD])) + 0.001;
  const double S = c01(p[TsAmpS]) * c01(p[TsAmpS]), R = 10.0 * p5(c01(p[TsAmpR])) + 0.1;
  if (v.relT >= 0) {
    if (v.relT > R) {
      hardReset(v);
      return;
    }
    v.ampT = 0, v.ampC = 0.00018375 / R;
  } else if (v.t < A) {
    v.ampT = 5.0, v.ampC = 4.658742e-6 / A;
  } else {
    v.ampT = S, v.ampC = 0.00018375 / D;
  }
  // Pitch.
  const double tune = std::round(198.0 * p[TsTune] - 99.0) / 100.0;
  v.ratio = (v.fExact / v.fInt) * std::exp2(tune / 12.0 + mod(v, MdPitch) / 6.0);
  // Free envelope (linear, per control block).
  {
    const double eA = 10.0 * std::pow(c01(p[TsEgA]), 3.0) + 0.001, eD = 50.0 * p5(c01(p[TsEgD])) + 0.001;
    const double eS = c01(p[TsEgS]) * c01(p[TsEgS]), eR = 50.0 * p5(c01(p[TsEgR])) + 0.1;
    switch (v.egStage) {
      case 0:
        v.egPh += n48 / (std::max(eA, 0.001) * kRate);
        if (v.egPh >= 1) v.egStage = 1, v.egPh = 0, v.egLevel = 1;
        else v.egLevel = v.egFrom + (1.0 - v.egFrom) * v.egPh;
        break;
      case 1:
        v.egPh += n48 / (std::max(eD, 0.001) * kRate);
        if (v.egPh >= 1) {
          v.egLevel = eS;
          v.egStage = eS == 0 ? 4 : 2;
        } else {
          v.egLevel = 1.0 + (eS - 1.0) * v.egPh;
        }
        break;
      case 2: v.egLevel = eS; break;
      case 3:
        v.egPh += n48 / (std::max(eR, 0.001) * kRate);
        if (v.egPh >= 1) v.egStage = 4, v.egLevel = 0;
        else v.egLevel = v.egFrom * (1.0 - v.egPh);
        break;
      default: break;
    }
  }
  // Exciter.
  v.thr = 0.996 * std::pow(1.0 - c01(p[TsNoiseRate] + mod(v, MdNoiseRate)), 0.25);
  const double dr = c01(p[TsDrive]);
  v.drive = 1000.0 * dr * dr * dr * dr;
  v.pmix = 1.0 - c01(p[TsNoiseLevel]);
  if (v.relT >= 0) {
    v.hC = 0.002, v.hT = 0;
  } else {
    const double hA = c01(p[TsHiAtk] + mod(v, MdHiAtk));
    if (v.t <= 0.5 * hA * hA) {
      v.hT = 1.0, v.hC = 0.5 * hA * hA < 0.001 ? 1.0 : 0.0004 / (hA * hA);
    } else {
      const double hD = c01(p[TsHiDec] + mod(v, MdHiDec));
      v.hC = 0.0002 / (3.0 * hD * hD * hD + 0.001), v.hT = c01(p[TsHiSus]) * c01(p[TsHiSus]);
    }
  }
  {
    const double h = p[TsHiCut] + 0.5 * mod(v, MdHiCut);
    v.k2 = std::min(1.0, h * h);
    v.lfb = std::max(-4.0, (v.k2 - 1.0) * 10.0 * p[TsHiRes]);
  }
  v.lo.set(std::exp2(9.3 * c01(p[TsLoCut] + 0.5 * mod(v, MdLoCut)) + 5.0), 1.0 + 10.0 * p[TsLoRes], kRate);
  // Strings.
  for (int s = 0; s < 2; ++s) {
    const double dmp = c01((s ? p[TsS2Damp] : p[TsS1Damp]) + mod(v, MdBothDamp) + mod(v, s ? MdS2Damp : MdS1Damp));
    const double cRaw = std::exp2(14.5047104 - 5.0766486 * dmp * dmp * dmp) / 22050.0;
    const double o = s ? p[TsS2DampOfs] : p[TsS1DampOfs], sh = s ? p[TsS2Sharp] : p[TsS1Sharp];
    v.c[s][0] = std::min(1.0, cRaw);
    v.c[s][1] = std::min(1.0, (1.0 - o * o) * cRaw);
    for (int l = 0; l < 2; ++l) v.k[s][l] = (1.0 - v.c[s][l]) * (-0.5 * sh * sh);
    double G = std::pow(c01((s ? p[TsS2Decay] : p[TsS1Decay]) + mod(v, MdBothDecay) + mod(v, s ? MdS2Decay : MdS1Decay)), 0.05);
    if ((s ? p[TsS2Inv] : p[TsS1Inv]) >= 0.5f) G = -G;
    v.G[s] = G;
  }
  {
    const double sat = c01(p[TsSaturation]);
    v.sigma = 100.0 * sat * sat * sat * sat * sat * sat;
    const double lev = 0.05 * std::pow(10.0, 2.0 * mod(v, MdLevel));
    const double m = p[TsMix];
    const double pi1 = c01(p[TsStereo] + mod(v, MdPan)), pi2 = c01((1.0 - p[TsStereo]) + mod(v, MdPan));
    v.gT[0] = lev * (1 - m) * (1 - pi1), v.gT[1] = lev * (1 - m) * pi1;
    v.gT[2] = lev * m * (1 - pi2), v.gT[3] = lev * m * pi2;
    v.fbComb = std::sqrt(c01(p[TsCombFb])) * (p[TsCombFbInv] >= 0.5f ? -1.0 : 1.0);
    v.levComb = p[TsCombLevel] * (p[TsCombLevelInv] >= 0.5f ? -1.0 : 1.0);
  }
  if (v.fresh) {
    v.fresh = false;
    for (int s = 0; s < 2; ++s) v.Gs[s] = v.G[s];
    for (int i = 0; i < 4; ++i) v.g[i] = v.gT[i];
  }
  v.t += n48 / kRate;
  if (v.relT >= 0) v.relT += n48 / kRate;
}

void TwinRef::renderVoices(int k) {
  for (int i = 1; i <= k; ++i) bufL_[static_cast<size_t>(i)] = bufR_[static_cast<size_t>(i)] = 0;
  for (auto& v : v_) {
    if (!v.on) continue;
    control(v, k);
    if (!v.on) continue;
    for (int i = 1; i <= k; ++i) {
      // Exciter.
      const double u = lcg_ * 2.3283064365386963e-10;
      lcg_ = lcg_ * 0x0BB38435u + 0x3619636Bu;
      if (u > v.thr) v.held = u;
      const double e = v.held * (1.0 - v.pmix) * (1.0 - v.pmix) + v.pmix;
      const double y = v.lo.run(e);
      v.hEnv += (v.hT - v.hEnv) * v.hC;
      const double g = v.hEnv * v.k2;
      v.s1 += (y + v.lfb * v.s4 - v.s1) * g;
      v.s2 += (v.s1 - v.s2) * g;
      v.s3 += (v.s2 - v.s3) * g;
      v.s4 += (v.s3 - v.s4) * g;
      v.s4 = v.s4 / (1.0 + v.drive * v.s4 * v.s4);
      const double x = v.s4;
      // Strings at the fractional clock.
      v.ph += v.ratio;
      int steps = static_cast<int>(v.ph);
      v.ph -= steps;
      while (steps-- > 0) {
        v.prevL = v.curL, v.prevR = v.curR;
        double out[2];
        for (int s = 0; s < 2; ++s) {
          const int N = v.len[s];
          v.Gs[s] += (v.G[s] - v.Gs[s]) * 0.05;
          double r[2];
          for (int l = 0; l < 2; ++l) {
            float* B = v.buf[s][l].data();
            r[l] = B[v.ptr[s]];
            v.dc[s][l] += (x - v.dc[s][l]) / N;
            v.q1[s][l] += (r[l] + v.k[s][l] * v.q2[s][l] - v.q1[s][l]) * v.c[s][l];
            v.q2[s][l] += (v.q1[s][l] - v.q2[s][l]) * v.c[s][l];
            v.q2[s][l] = v.q2[s][l] / (1.0 + v.sigma * v.q2[s][l] * v.q2[s][l]);
            B[v.ptr[s]] = static_cast<float>(v.Gs[s] * v.q2[s][l] + (x - v.dc[s][l]));
          }
          if (++v.ptr[s] >= N) v.ptr[s] = 0;
          float* C = v.comb[s].data();
          const double cr = C[v.cptr[s]];
          C[v.cptr[s]] = static_cast<float>(v.fbComb * cr + r[1]);
          if (++v.cptr[s] >= v.clen[s]) v.cptr[s] = 0;
          out[s] = r[0] + v.levComb * cr;
        }
        for (int q = 0; q < 4; ++q) v.g[q] += (v.gT[q] - v.g[q]) * 0.001;
        v.curL = v.g[0] * out[0] + v.g[2] * out[1];
        v.curR = v.g[1] * out[0] + v.g[3] * out[1];
      }
      const double oL = (v.prevL + v.ph * (v.curL - v.prevL)) * v.amp;
      const double oR = (v.prevR + v.ph * (v.curR - v.prevR)) * v.amp;
      v.amp += (v.ampT - v.amp) * v.ampC;
      v.amp = std::min(v.amp, 1.0);
      if (std::fabs(oL) >= 1e-8 || std::fabs(oR) >= 1e-8) v.sounded = true, v.quiet = 0;
      else if (v.sounded) ++v.quiet;
      bufL_[static_cast<size_t>(i)] += oL;
      bufR_[static_cast<size_t>(i)] += oR;
      if (v.quiet > 1000 || !std::isfinite(oL) || !std::isfinite(oR)) {
        hardReset(v);
        break;
      }
    }
  }
}

void TwinRef::render(float* outL, float* outR, int total) {
  float L[256], R[256];
  for (int at = 0; at < total; at += 256) {
    const int n = std::min(256, total - at);
    renderPart(L, R, n);
    for (int i = 0; i < n; ++i) outL[at + i] += L[i], outR[at + i] += R[i];
  }
}

void TwinRef::renderPart(float* L, float* R, int n) {
  for (int i = 0; i < n; ++i) {
    if (chunkLeft_ == 0) {
      // New control chunk on the fixed 48-sample grid: global LFO, voices.
      lfo_ = std::cos(2.0 * kPi * lfoPh_);
      const double rate = 10.0 * std::pow(c01(p_[TsLfoRate]), 3.0) + 0.01;
      lfoPh_ += kChunk * rate / sr_;
      if (lfoPh_ > 1) lfoPh_ -= 1;
      const double end = pos_ + kChunk * step_;
      chunkK_ = static_cast<int>(std::trunc(end));
      if (end > chunkK_) ++chunkK_;
      renderVoices(chunkK_);
      chunkLeft_ = kChunk;
    }
    const int idx = static_cast<int>(pos_);
    const double t = pos_ - idx;
    L[i] = static_cast<float>(bufL_[static_cast<size_t>(idx)] + t * (bufL_[static_cast<size_t>(idx + 1)] - bufL_[static_cast<size_t>(idx)]));
    R[i] = static_cast<float>(bufR_[static_cast<size_t>(idx)] + t * (bufR_[static_cast<size_t>(idx + 1)] - bufR_[static_cast<size_t>(idx)]));
    pos_ += step_;
    if (--chunkLeft_ == 0) {
      pos_ -= chunkK_;
      bufL_[0] = bufL_[static_cast<size_t>(chunkK_)];
      bufR_[0] = bufR_[static_cast<size_t>(chunkK_)];
    }
  }
  fx(L, R, n);
  bool any = false;
  for (const auto& v : v_) any = any || v.on;
  const bool fxTail = p_[TsDlyOn] >= 0.5f || p_[TsRevOn] >= 0.5f || p_[TsChoOn] >= 0.5f;
  bool res = false;
  for (int b = 0; b < 8; ++b) res = res || fx_->resOn[b];
  if (any) tail_ = (fxTail || res) ? static_cast<int>(sr_ * 6) : static_cast<int>(sr_ * 0.05);
  else if (tail_ > 0) tail_ = std::max(0, tail_ - n);
}

void TwinRef::fx(float* L, float* R, int n) {
  Fx& f = *fx_;
  const bool cho = p_[TsChoOn] >= 0.5f, rev = p_[TsRevOn] >= 0.5f, dly = p_[TsDlyOn] >= 0.5f;
  const double G = std::pow(10.0, 4.8 * c01(p_[TsLevel]) - 1.8);
  for (int i = 0; i < n; ++i) {
    double l = f.dc[0].run(L[i]), r = f.dc[1].run(R[i]);
    const float u = static_cast<float>(l + r);
    double rl = 0, rr = 0;
    for (int b = 0; b < 8; ++b) {
      if (!f.resOn[b]) continue;
      const float ya = f.resA[b].tick(u);
      const float yb = f.resB[b].tick(-ya);
      if (b % 2 == 0) rl += 0.75 * yb, rr += 1.25 * yb;
      else rl += 1.25 * yb, rr += 0.75 * yb;
    }
    l += f.resHp[0].run(rl), r += f.resHp[1].run(rr);
    if (cho) {
      const float cu = static_cast<float>(l + r);
      const float c1 = f.cho[0].tick(cu), c3 = f.cho[2].tick(cu), c2 = f.cho[1].tick(cu);
      l += c1 + c2, r += c3 + c2;
    }
    L[i] = static_cast<float>(l), R[i] = static_cast<float>(r);
  }
  if (rev) f.rev.process(L, R, n);
  for (int i = 0; i < n; ++i) {
    if (dly) {
      float a = L[i];
      a = f.dly[0].tick(a) + a;
      a = f.dly[1].tick(a) + a;
      L[i] = f.dly[2].tick(a) + a;
      float b = R[i];
      b = f.dly[3].tick(b) + b;
      b = f.dly[4].tick(b) + b;
      R[i] = f.dly[5].tick(b) + b;
    }
    L[i] = static_cast<float>(L[i] * G), R[i] = static_cast<float>(R[i] * G);
  }
}

}  // namespace gl::packs::models::strs

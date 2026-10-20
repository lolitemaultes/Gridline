// Gridline — J-42: Time Gate reference engine (gl.timegate TgModel = 1), the
// beat-slot time / volume engine FL ships as Gross Beat (study notes: scratch
// j42/gross_beat/spec.md, own wording and code).
//
// Time path: a history ring read D = round(R + 0.5) + 15 samples behind the
// write head plus y·R (R = 8 beats in samples, y = time envelope), windowed-
// sinc reads (8 / 16 taps, anti-aliased by the head speed in HQ), a step-decay
// declick when the head jumps, bar-repeat hold that stops recording, scratch
// offset through three asymmetric one-poles; the dry path is delayed 15 samples
// while playing. Volume path: tension, mix, two asymmetric one-poles. Slot
// switches and holds happen at block starts on the integer song tick. The ring
// is sized for tempos down to 30 BPM (FL keeps 72 s: 10 BPM).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Generators.h"
#include "TimeGateSlots.h"

namespace gl::gbref {

inline constexpr double kPi = 3.14159265358979323846;

struct Settings {
  int timeSlot = 0, volSlot = 0;  // 1-based, 0 = none (flat)
  double timeMix = 1, volMix = 1, tension = 0;  // tension −1..1 (raw / 256)
  double attackRaw = 0, releaseRaw = 0;          // 0..10000
  double scratch = 0;                            // s = raw / 65536 − 1
  bool hold = false, dc = false, comp = false, hq = true;
  int declick = 1;  // 0 off, 1 low, 2 high
};

class Engine {
 public:
  void prepare(double sr) {
    sr_ = sr;
    cap_ = static_cast<int>(30.0 * sr) + 4096;
    ring_[0].assign(static_cast<size_t>(cap_), 0.f), ring_[1].assign(static_cast<size_t>(cap_), 0.f);
    off_.assign(8192, 0.0), spd_.assign(8192, 0.0), rawSpd_.assign(8192, 0.0);
    reset();
  }
  void reset() {
    for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.f);
    w_ = 0, base_ = 0, R_ = -1;
    for (auto& d : dry_) std::fill(std::begin(d), std::end(d), 0.f);
    dryW_ = 0;
    for (auto& b : bank_) b = Bank{};
    g1_ = g2_ = -1;
    for (auto& s : scr_) s = 0;
    scrPrimed_ = false;
    declOff_[0] = declOff_[1] = prevWet_[0] = prevWet_[1] = 0;
    lastOff_ = 0, spdS_ = 1;
    holding_ = false, holdTick_ = -1, holdSrc_ = 0, lastHoldParam_ = false;
    dcM_[0] = dcM_[1] = dcV_[0] = dcV_[1] = 0;
    wasPlaying_ = false, fadeLast_[0] = fadeLast_[1] = 0, fadeDone_ = true;
  }
  void note(int pitch, bool on) {
    const int b = pitch >= 60 && pitch <= 95 ? 0 : pitch >= 24 && pitch <= 59 ? 1 : -1;
    if (b < 0 || !on) return;  // key-held stacks: plain selects (shipped states store no note data)
    noteReq_[b] = pitch - (b == 0 ? 60 : 24) + 1;
  }

  void process(const Settings& S, const tg::Bank& bank, const ProcessContext& ctx, float* L, float* R, int n) {
    if (sr_ != ctx.sampleRate || ring_[0].empty()) prepare(ctx.sampleRate);
    if (n > static_cast<int>(off_.size())) n = static_cast<int>(off_.size());
    const int ppq = std::max(1, ctx.ppq);
    const double T = std::max(1.0, ctx.tempo);
    const double Bf = static_cast<double>(static_cast<float>(240.0 * sr_) / static_cast<float>(T));  // samples per bar
    const double Rn = 2 * Bf;
    const double s = 4.0 / Bf;  // beats per sample
    if (!ctx.playing) {
      // Stopped: input passes; the first stopped block fades from the last output.
      if (!fadeDone_)
        for (int i = 0; i < n; ++i) {
          const double w = (std::cos(kPi * i / std::max(n - 1, 1)) + 1) / 2;
          L[i] = static_cast<float>(w * fadeLast_[0] + (1 - w) * L[i]);
          R[i] = static_cast<float>(w * fadeLast_[1] + (1 - w) * R[i]);
        }
      fadeDone_ = true, fadeLast_[0] = fadeLast_[1] = 0;
      if (wasPlaying_) {  // stop: history cleared, slots unset
        for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.f);
        for (auto& b : bank_) b = Bank{};
        holding_ = false, holdTick_ = -1, holdSrc_ &= 1;
      }
      wasPlaying_ = false;
      return;
    }
    const long long now = std::llround(ctx.tick);
    if (!wasPlaying_ || ctx.jumped) {  // start: flush, then both banks re-select their slots
      for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.f);
      for (auto& d : dry_) std::fill(std::begin(d), std::end(d), 0.f);
      dcM_[0] = dcM_[1] = dcV_[0] = dcV_[1] = 0;
      g1_ = g2_ = -1, declOff_[0] = declOff_[1] = 0;
      bank_[0].cur = bank_[1].cur = -1;
      R_ = -1;
    }
    wasPlaying_ = true, fadeDone_ = false;
    if (R_ != Rn) {  // tempo / rate change: re-align the read base
      R_ = Rn;
      long long D = static_cast<long long>(std::nearbyint(Rn + 0.5)) + 15;
      D = std::clamp<long long>(D, 15, cap_ - 15);
      base_ = static_cast<double>(w_ - D);
      rho_ = std::nearbyint(Rn + 0.5) - Rn;
    }
    // ---- slot requests (block-granular, integer ticks)
    const long long syncT[6] = {0, ppq / 4, ppq / 2, ppq, 2LL * ppq, 4LL * ppq};
    const int want[2] = {S.timeSlot, S.volSlot};
    const double attS = sr_ * (std::pow(501.0, S.attackRaw / 10000.0) - 1.0) / 1000.0;
    for (int b = 0; b < 2; ++b) {
      Bank& k = bank_[b];
      int req = -1;
      if (want[b] != k.lastWant) req = want[b], k.lastWant = want[b];
      if (k.cur < 0) req = want[b];
      if (noteReq_[b] >= 0) req = noteReq_[b], noteReq_[b] = -1;
      if (b == 1 && req >= 0 && bank_[0].cur > 0 && slot(bank, 0, bank_[0].cur).link) req = -1;
      if (req >= 0) {
        const int trig = req > 0 ? std::clamp(slot(bank, b, req).trig < 0 ? 5 : slot(bank, b, req).trig, 0, 5) : 0;
        if (trig == 0 || k.cur < 0) select(bank, b, req, now, attS, syncT, s, ppq);
        else {
          const long long St = syncT[trig];
          k.pending = req, k.switchTick = (now + St - 1) / St * St;
        }
      }
      if (k.pending >= 0 && now >= k.switchTick) select(bank, b, k.pending, now, attS, syncT, s, ppq), k.pending = -1;
      if (k.oneShotTick >= 0 && now >= k.oneShotTick) k.oneShotTick = -1, select(bank, b, 1, now, attS, syncT, s, ppq);
    }
    // ---- hold
    if (S.hold != lastHoldParam_) {
      lastHoldParam_ = S.hold;
      holdSrc_ = (holdSrc_ & ~1) | (S.hold ? 1 : 0);
      if (holdTick_ < 0) holdTick_ = (now + 4LL * ppq - 1) / (4LL * ppq) * (4LL * ppq);
    }
    bool holdChanged = false;
    if (holdTick_ >= 0 && now >= holdTick_) {
      if (holdSrc_) {
        if (!holding_) savedBase_ = base_, loopStart_ = base_ - Bf, holding_ = true, holdChanged = true, holdStart_ = now;
        base_ = loopStart_, holdTick_ = now + 4LL * ppq;
      } else if (holding_) {
        base_ = savedBase_, holding_ = false, holdTick_ = -1, holdChanged = true;
      } else {
        holdTick_ = -1;
      }
    }
    // ---- envelopes (per sample), offsets, head speed
    const tg::Slot* ts = bank_[0].cur > 0 ? &slot(bank, 0, bank_[0].cur) : nullptr;
    const tg::Slot* vs = bank_[1].cur > 0 ? &slot(bank, 1, bank_[1].cur) : nullptr;
    const bool timeFlat = !ts || flatOne(*ts), volFlat = !vs || flatOne(*vs);
    const bool scrActive = S.scratch < 0 || scrMoving(S.scratch);
    const bool timeOn = (!timeFlat && S.timeMix != 0) || holding_ || scrActive || declOff_[0] != 0 || declOff_[1] != 0 || holdChanged;
    const double aUp = a(0.3 / std::sqrt(3.0) * sr_), aDn = a(0.2 / std::sqrt(3.0) * sr_);
    if (!scrPrimed_) scr_[0] = scr_[1] = scr_[2] = S.scratch, scrPrimed_ = true;
    for (int i = 0; i < n; ++i) {
      double y = ts ? ts->eval(std::fmod(bank_[0].phase, std::max(0.25, static_cast<double>(ts->len)))) : 1.0;
      bank_[0].phase += s;
      if (scrActive) {
        double x = S.scratch;
        for (double& st : scr_) st = x + (x > st ? aUp : aDn) * (st - x), x = st;
        y = std::max(0.0, y + x);
      }
      off_[static_cast<size_t>(i)] = y * Rn;
    }
    // The ring receives the whole block before it is read.
    if (!holding_)
      for (int i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c) ring_[c][static_cast<size_t>((w_ + i) % cap_)] = c == 0 ? L[i] : R[i];
    const double kS = a((S.declick == 2 ? 0.005 : 0.001) * sr_);
    double vmax = 0, vmin = 1e9;
    for (int i = 0; i < n; ++i) {
      const double prev = i ? off_[static_cast<size_t>(i - 1)] : lastOff_;
      rawSpd_[static_cast<size_t>(i)] = std::fabs(off_[static_cast<size_t>(i)] - prev + 1);
      spdS_ = rawSpd_[static_cast<size_t>(i)] + kS * (spdS_ - rawSpd_[static_cast<size_t>(i)]);
      spd_[static_cast<size_t>(i)] = spdS_;
      vmax = std::max(vmax, spdS_), vmin = std::min(vmin, spdS_);
    }
    if (n > 0) lastOff_ = off_[static_cast<size_t>(n - 1)];
    // HQ: 16 taps with an anti-alias cutoff when faster than 1x, 16 taps when slower, else 8.
    const bool fast = S.hq && vmax > 1 + 5.9604644775390625e-08;
    const int N = !S.hq ? 8 : (fast || vmin < 1 - 5.9604644775390625e-08) ? 16 : 8;
    const double J = 0.002 * sr_;
    const double cDec = S.declick == 0 ? 0.0 : std::exp(std::log(0.01) / (sr_ * (S.declick == 2 ? 0.035 : 0.025)));
    // Volume.
    const double tc = S.tension, cT = (tc < 0 ? -1.0 : 1.0) * (std::pow(1001.0, std::fabs(tc)) - 1.0), kT = std::log(1 + std::fabs(cT));
    const double nAtt = 0.70710678 * attS, nRel = 2 * 0.70710678 * sr_ * (std::pow(501.0, S.releaseRaw / 10000.0) - 1.0) / 1000.0;
    const double aAtt = a(nAtt), aRel = a(nRel);
    const double dcA = 2.2835e-5 * 44100.0 / sr_;
    for (int i = 0; i < n; ++i) {
      const float in[2] = {L[i], R[i]};
      // 15-sample dry delay.
      float dry[2];
      for (int c = 0; c < 2; ++c) dry[c] = dry_[c][dryW_], dry_[c][dryW_] = in[c];
      dryW_ = (dryW_ + 1) % 15;
      double out[2] = {dry[0], dry[1]};
      if (timeOn) {
        const double p = base_ + off_[static_cast<size_t>(i)] + rho_;
        double wet[2];
        read(p, N, fast ? kPi / std::max(spd_[static_cast<size_t>(i)], 1.0) : kPi, wet);
        if (S.declick != 0) {
          const bool trig = rawSpd_[static_cast<size_t>(i)] > J || (i == 0 && holdChanged);
          for (int c = 0; c < 2; ++c) {
            if (trig) declOff_[c] += prevWet_[c] - wet[c];
            prevWet_[c] = wet[c];
            wet[c] += declOff_[c];
            declOff_[c] *= cDec;
          }
        }
        const double m = ts ? S.timeMix : 1.0;
        for (int c = 0; c < 2; ++c) out[c] = m == 1 ? wet[c] : wet[c] * m + dry[c] * (1 - m);
      }
      // Volume envelope.
      double g = 1;
      double v = vs ? vs->eval(std::fmod(bank_[1].phase, std::max(0.25, static_cast<double>(vs->len)))) : 1.0;
      bank_[1].phase += s;
      if (!volFlat && S.volMix != 0) {
        v = static_cast<float>(v);
        if (cT < 0) v = (std::exp(kT * v) - 1) / std::fabs(cT);
        else if (cT > 0) v = 1 - (std::exp(kT * (1 - v)) - 1) / std::fabs(cT);
        g = 1 - S.volMix + S.volMix * v;
      }
      if (g1_ < 0) g1_ = g2_ = g;
      g1_ = g + (g1_ <= g ? aAtt : aRel) * (g1_ - g);
      g2_ = g1_ + (g2_ <= g1_ ? aAtt : aRel) * (g2_ - g1_);
      for (int c = 0; c < 2; ++c) {
        double x = out[c] * g2_;
        if (S.dc) {
          dcV_[c] += dcA * (x - dcM_[c]);
          dcM_[c] += dcV_[c];
          dcV_[c] *= 0.96;
          x -= dcM_[c];
        }
        out[c] = x;
      }
      L[i] = static_cast<float>(out[0]), R[i] = static_cast<float>(out[1]);
      base_ += 1;
    }
    if (!holding_) w_ += n;
    for (double& d : declOff_)
      if (std::fabs(d) < 5.9604644775390625e-08) d = 0;
    if (n > 0) fadeLast_[0] = L[n - 1], fadeLast_[1] = R[n - 1];
  }

 private:
  struct Bank {
    int cur = -1, lastWant = -1, pending = -1;
    long long switchTick = 0, oneShotTick = -1;
    double phase = 0;
  };
  static const tg::Slot& slot(const tg::Bank& b, int bank, int k) {
    k = std::clamp(k, 1, tg::kSlots);
    return bank == 0 ? b.time[k - 1] : b.vol[k - 1];
  }
  static bool flatOne(const tg::Slot& s) {
    for (int i = 0; i < s.env.n; ++i)
      if (s.env.y[i] != 1.f) return false;
    return true;
  }
  bool scrMoving(double x) const {
    for (double st : scr_)
      if (std::fabs(st - x) > 5.9604644775390625e-08) return true;
    return false;
  }
  static double a(double N) { return N <= 0 ? 0.0 : std::exp(std::log(0.01) / std::max(N, 1.0)); }
  void select(const tg::Bank& bank, int b, int k, long long now, double attS, const long long* syncT, double s, int ppq) {
    Bank& st = bank_[b];
    st.cur = k;
    st.oneShotTick = -1;
    if (k <= 0) {
      st.phase = 0;
      if (b == 0) holdSrc_ &= ~2;
      else holdSrc_ &= ~4;
      return;
    }
    const tg::Slot& sl = slot(bank, b, k);
    const int p = std::clamp(sl.pos, 0, 5);
    double d = 0;
    if (p > 0) {
      long long t = now;
      if (holding_) t = now - holdStart_ + 256LL * ppq;
      d = static_cast<double>(t % syncT[p]) / ppq;
    }
    st.phase = attS * s + d;
    const int bit = b == 0 ? 2 : 4;
    if (sl.force == tg::ForceLast) holdSrc_ |= bit, holdTick_ = now;
    else if (sl.force == tg::ForceNext) holdSrc_ |= bit, holdTick_ = now + 4LL * ppq;
    else holdSrc_ &= ~bit;
    if (sl.oneShot) st.oneShotTick = now + 4LL * ppq;
    if (b == 0) {  // link: the volume bank follows the time slot
      if (sl.link) select(bank, 1, k, now, attS, syncT, s, ppq);
    }
  }
  // Windowed-sinc read at absolute position p (2N taps, cutoff wc).
  void read(double p, int N, double wc, double* out) const {
    const double fl = std::floor(p);
    double x = std::max(p - fl, 1e-25);
    const long long i0 = static_cast<long long>(fl);
    double acc[2] = {0, 0};
    for (int k = -N + 1; k <= N; ++k) {
      const double d = k - x;
      const double th = kPi * d / N;
      const double W = 0.5 * (0.36 + std::cos(th) + 0.64 * std::cos(th) * std::cos(th));
      const double h = std::sin(wc * d) / (kPi * d) * W;
      long long idx = (i0 + k) % cap_;
      if (idx < 0) idx += cap_;
      for (int c = 0; c < 2; ++c) acc[c] += h * ring_[c][static_cast<size_t>(idx)];
    }
    out[0] = acc[0], out[1] = acc[1];
  }

  double sr_ = 0;
  int cap_ = 1;
  std::vector<float> ring_[2];
  std::vector<double> off_, spd_, rawSpd_;
  long long w_ = 0;
  double base_ = 0, R_ = -1, rho_ = 0, savedBase_ = 0, loopStart_ = 0;
  float dry_[2][15] = {};
  int dryW_ = 0;
  Bank bank_[2];
  int noteReq_[2] = {-1, -1};
  double g1_ = -1, g2_ = -1, scr_[3] = {0, 0, 0};
  bool scrPrimed_ = false;
  double declOff_[2] = {0, 0}, prevWet_[2] = {0, 0}, lastOff_ = 0, spdS_ = 1;
  bool holding_ = false, lastHoldParam_ = false, wasPlaying_ = false, fadeDone_ = true;
  long long holdTick_ = -1, holdStart_ = 0;
  int holdSrc_ = 0;
  double dcM_[2] = {0, 0}, dcV_[2] = {0, 0}, fadeLast_[2] = {0, 0};
};

}  // namespace gl::gbref

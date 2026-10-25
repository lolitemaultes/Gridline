// Gridline — κ3 effects: Time Gate (36-slot time / volume banks with
// per-slot trigger / position sync, force hold, link, key-held, one-shot,
// bar-repeat hold, windowed-sinc read-back). Audio thread only: buffers are
// sized when the sample rate changes, slot banks are parsed only when the
// state text changes (no allocation).
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "Effects.h"
#include "EffectsK3.h"
#include "GrossBeatRef.h"
#include "TimeGateSlots.h"

namespace gl {

using namespace dsp;

namespace {

float param(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}
int iparam(const PluginState& s, int i, int dflt = 0) { return static_cast<int>(std::lround(param(s, i, static_cast<float>(dflt)))); }
bool bparam(const PluginState& s, int i, bool dflt = false) { return param(s, i, dflt ? 1.f : 0.f) >= 0.5f; }

// Lanczos-windowed sinc kernels (8 and 16 taps), 512 fractional phases.
struct SincTable {
  static constexpr int kPhases = 512;
  std::vector<float> k8, k16;
  SincTable() {
    auto build = [](std::vector<float>& t, int taps) {
      t.assign(static_cast<size_t>((kPhases + 1) * taps), 0.f);
      const int half = taps / 2;
      for (int ph = 0; ph <= kPhases; ++ph) {
        double frac = static_cast<double>(ph) / kPhases;
        double sum = 0;
        for (int k = 0; k < taps; ++k) {
          double x = (k - (half - 1)) - frac;  // tap offsets -(half-1) .. half
          double v;
          if (std::fabs(x) < 1e-9) v = 1;
          else if (std::fabs(x) >= half) v = 0;
          else v = std::sin(kPi * x) / (kPi * x) * std::sin(kPi * x / half) / (kPi * x / half);
          t[static_cast<size_t>(ph * taps + k)] = static_cast<float>(v);
          sum += v;
        }
        for (int k = 0; k < taps; ++k) t[static_cast<size_t>(ph * taps + k)] = static_cast<float>(t[static_cast<size_t>(ph * taps + k)] / sum);
      }
    };
    build(k8, 8);
    build(k16, 16);
  }
};
const SincTable& sinc() {
  static const SincTable t;
  return t;
}

}  // namespace

// ======================================================================= Time Gate
// Gross-Beat-style engine (see TimeGateSlots.h for the slot model).
class TimeGateK3 final : public Effect {
 public:
  TimeGateK3() : factory_(std::make_unique<tg::Bank>()), bank_(std::make_unique<tg::Bank>()) {
    tg::factory(*factory_);
    *bank_ = *factory_;
    (void)sinc();
  }

  // Slot trigger from a note (MIDI trigger option): time slots 60..95, volume slots 24..59.
  void note(int pitch, bool on) {
    ref_.note(pitch, on);
    int b = pitch >= 60 && pitch <= 95 ? 0 : pitch >= 24 && pitch <= 59 ? 1 : -1;
    if (b < 0) return;
    int slot = pitch - (b == 0 ? 60 : 24) + 1;
    BankState& st = bs_[b];
    if (on) {
      const tg::Slot& s = slotOf(b, slot);
      if (s.keyHeld && st.stackN < 8) st.stack[st.stackN++] = st.cur;
      st.noteReq = slot;
    } else {
      const tg::Slot& s = slotOf(b, slot);
      if (!s.keyHeld) return;
      // Back to the most recent remaining entry.
      if (st.cur == slot && st.stackN > 0) st.noteReq = st.stack[--st.stackN];
    }
  }

  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    if (sr_ != ctx.sampleRate) {
      sr_ = ctx.sampleRate;
      size_t len = static_cast<size_t>(sr_ * 24) + 64;  // 8-beat range + bar loop + scratch at >= 50 BPM
      bl_.assign(len, 0.f);
      br_.assign(len, 0.f);
      wc_ = 0;
      for (auto& d : dc_) d.setFreq(sr_, 10.0);
    }
    size_t h = std::hash<std::string>{}(s.data);
    if (h != dataHash_) {
      dataHash_ = h;
      *bank_ = *factory_;
      tg::parse(s.data.c_str(), *bank_);
    }
    if (bparam(s, TgModel)) {  // J-42 reference engine
      gbref::Settings g;
      g.timeSlot = std::clamp(iparam(s, TgTimeSlot), 0, tg::kSlots), g.volSlot = std::clamp(iparam(s, TgVolSlot), 0, tg::kSlots);
      g.timeMix = param(s, TgTimeMix, 1), g.volMix = param(s, TgVolMix, 1), g.tension = param(s, TgTension);
      g.attackRaw = param(s, TgRAttack), g.releaseRaw = param(s, TgRRelease, 1000), g.scratch = param(s, TgRScratch);
      g.hold = bparam(s, TgHold), g.dc = bparam(s, TgDcRemove), g.comp = bparam(s, TgCompensate), g.hq = bparam(s, TgHq, true);
      g.declick = std::clamp(iparam(s, TgDeclick, 1), 0, 2);
      ref_.process(g, *bank_, ctx, L, R, n);
      return;
    }
    const int mode = iparam(s, TgMode);
    const bool gateOn = bparam(s, TgGateOn);
    const double sm = coefOf(param(s, TgSmooth, 3) * 0.001), mix = param(s, TgMix, 1);
    const double tmix = param(s, TgTimeMix, 1), vmix = param(s, TgVolMix, 1);
    const double attMs = param(s, TgVolAttack, 0.002f) * 1000.0, relMs = param(s, TgVolRelease, 0.0428f) * 1000.0;
    const double ca = coefOf(attMs * 0.001 * 0.70710678), cr = coefOf(relMs * 0.001 * 0.70710678);
    const double tens = param(s, TgTension);
    const bool holdParam = bparam(s, TgHold), freeRun = bparam(s, TgFreeRun);
    freeRun_ = freeRun;
    const int defTrig = std::clamp(iparam(s, TgTrigSync), 0, 5);
    const int declick = iparam(s, TgDeclick, 1);
    const double xfLen = declick == 0 ? 0 : (declick == 1 ? 0.002 : 0.012) * sr_;
    const double scratch = param(s, TgScratch);
    const bool dcOn = bparam(s, TgDcRemove), comp = bparam(s, TgCompensate), hq = bparam(s, TgHq, true);
    const double spb = 60.0 / std::max(1.0, ctx.tempo) * sr_;
    const size_t N = bl_.size();
    const int want[2] = {std::clamp(iparam(s, TgTimeSlot), 0, tg::kSlots), std::clamp(iparam(s, TgVolSlot), 0, tg::kSlots)};
    // Volume tension curve (exponential, ratio 1001 at full tension).
    const double tC = std::fabs(tens) * std::log(1001.0), tB = tC > 1e-9 ? 1.0 / (std::exp(tC) - 1.0) : 0;
    const bool running = ctx.playing || freeRun;
    if (!running) {
      // Stopped: dry output, declicked from the last output over the block.
      for (int i = 0; i < n; ++i) {
        double w = n > 1 ? (std::cos(kPi * i / (n - 1)) + 1) * 0.5 : 0.0;
        if (!fadeDone_) {
          float l = L[i], r = R[i];
          L[i] = static_cast<float>(lastL_ * w + l * (1 - w));
          R[i] = static_cast<float>(lastR_ * w + r * (1 - w));
        }
        bl_[wc_ % N] = L[i], br_[wc_ % N] = R[i];
        ++wc_;
      }
      fadeDone_ = true;
      lastL_ = n ? L[n - 1] : 0.f, lastR_ = n ? R[n - 1] : 0.f;
      for (auto& b : bs_) b.cur = 0, b.lastWant = -1, b.pending = -1, b.oneShotAt = -1;
      holdOn_ = false;
      phase_ = -1;
      return;
    }
    fadeDone_ = false;
    for (int i = 0; i < n; ++i) {
      double beat;
      if (ctx.playing) {
        // + 1e-9 beat (J-30): the position is block start + offset, which rounds a few ulps
        // differently for every block size; on-grid samples (exact .5 rounding ties of
        // the read head, slice and sync boundaries) must not flip with it.
        beat = ctx.tick / std::max(1, ctx.ppq) + i / spb + 1e-9;
      } else {
        free_ += 1.0 / spb;
        beat = free_;
      }
      // ---- slot requests (parameter changes, notes) and switching
      for (int b = 0; b < 2; ++b) {
        BankState& st = bs_[b];
        int req = -1;
        if (want[b] != st.lastWant) {
          st.lastWant = want[b];
          req = want[b];
        }
        if (st.noteReq >= 0) req = st.noteReq, st.noteReq = -1;
        if (req >= 0) request(b, req, beat, defTrig);
        if (st.pending >= 0 && beat >= st.switchAt - 1e-9) switchTo(b, st.pending, beat), st.pending = -1;
        if (st.oneShotAt >= 0 && beat >= st.oneShotAt - 1e-9) {
          st.oneShotAt = -1;
          request(b, 1, beat, defTrig);
        }
      }
      // ---- hold (bar repeat): parameter or a slot's force hold
      if (forceNextAt_ >= 0 && beat >= forceNextAt_ - 1e-9) forceNextAt_ = -1, forceHold_ = true;
      bool hold = holdParam || forceHold_;
      const double bar = 4 * spb;
      if (hold && !holdOn_) holdOn_ = true, holdT0_ = static_cast<double>(wc_);
      if (!hold) holdOn_ = false;
      double live = static_cast<double>(wc_);
      if (holdOn_) live = holdT0_ - bar + std::fmod(static_cast<double>(wc_) - holdT0_, bar);
      // ---- input into the history
      bl_[wc_ % N] = L[i];
      br_[wc_ % N] = R[i];
      const double dryL = L[i], dryR = R[i];
      double outL = dryL, outR = dryR;
      double tl = dryL, tr = dryR;
      bool timeActive = false;
      // ---- time envelope
      BankState& T = bs_[0];
      if (T.cur > 0) {
        const tg::Slot& sl = bank_->time[T.cur - 1];
        double len = std::max(0.25, static_cast<double>(sl.len));
        double pos = std::fmod(beat - T.start, len);
        if (pos < 0) pos += len;
        double y = sl.eval(pos);
        double D = tg::delayBeats(y) * spb;
        double yNext = sl.eval(std::fmod(pos + 1.0 / spb, len));
        double slope = tg::delayBeats(yNext) * spb - D;
        D += -scratch * spb;
        if (comp) D += attMs * 0.001 * sr_;
        double maxBack = static_cast<double>(N) - 32 - (static_cast<double>(wc_) - live);
        D = std::clamp(D, 0.0, std::max(0.0, maxBack));
        const bool flat = std::fabs(slope) < 1e-9 && (static_cast<double>(wc_) == live || holdOn_);
        double rp = live - D;
        if (flat) rp = std::round(rp);
        if (xfLen > 0 && std::fabs(rp - (lastRp_ + 1)) > 4.0 && xfLeft_ <= 0 && haveRp_) {  // jump: crossfade from the old head
          xfRp_ = lastRp_ + 1;
          xfLeft_ = xfLen;
        }
        read(rp, hq, tl, tr);
        if (xfLeft_ > 0) {
          double ol, orr;
          xfRp_ += 1;
          read(xfRp_, hq, ol, orr);
          double f = xfLeft_ / xfLen;
          double w = 0.5 - 0.5 * std::cos(kPi * f);  // raised cosine
          tl = tl * (1 - w) + ol * w, tr = tr * (1 - w) + orr * w;
          xfLeft_ -= 1;
        }
        lastRp_ = rp, haveRp_ = true;
        timeActive = true;
        phase_ = static_cast<float>(pos);
      } else if (holdOn_) {
        read(std::round(live), false, tl, tr);
        timeActive = true;
        haveRp_ = false;
      } else if (ctx.playing && mode > 0) {  // 0.5 quick modes
        double inBar = std::fmod(beat, 4.0);
        auto back = [&](double beatsAgo) { read(std::round(static_cast<double>(wc_) - beatsAgo * spb), false, tl, tr); };
        if (mode >= 1 && mode <= 4) {
          const double slice = mode == 1 ? 1.0 : mode == 2 ? 0.5 : mode == 3 ? 0.25 : 0.125;
          back(inBar - std::fmod(inBar, slice));
        } else if (mode == 5) {
          back(inBar * 0.5);
        } else if (mode == 6) {
          double x = inBar / 4.0, played = x - x * x * 0.5;
          back((x - played) * 4.0);
          double fade = std::min(1.0, (1 - x) * 8);
          tl *= fade, tr *= fade;
        } else if (mode == 7) {
          double inBeat = beat - std::floor(beat);
          back(2 * inBeat);
        }
        timeActive = true;
        haveRp_ = false;
      } else {
        haveRp_ = false;
        phase_ = -1;
      }
      if (timeActive) {
        double m = T.cur > 0 ? tmix : 1.0;
        outL = dryL * (1 - m) + tl * m;
        outR = dryR * (1 - m) + tr * m;
      }
      // ---- volume envelope
      double g = 1;
      BankState& V = bs_[1];
      if (V.cur > 0) {
        const tg::Slot& sl = bank_->vol[V.cur - 1];
        double len = std::max(0.25, static_cast<double>(sl.len));
        double pos = std::fmod(beat - V.start, len);
        if (pos < 0) pos += len;
        double v = std::clamp(sl.eval(pos), 0.0, 1.0);
        if (tC > 1e-9) v = tens > 0 ? 1.0 - tB * (std::exp(tC * (1.0 - v)) - 1.0) : tB * (std::exp(tC * v) - 1.0);
        double target = 1 - vmix + vmix * v;
        // Two cascaded one-pole followers (attack when rising, release when falling).
        v1_ = target + (v1_ - target) * (target > v1_ ? ca : cr);
        v2_ = v1_ + (v2_ - v1_) * (v1_ > v2_ ? ca : cr);
        g = v2_;
        gate_ = g;
        vphase_ = static_cast<float>(pos);
      } else {
        vphase_ = -1;
        if (gateOn && ctx.playing) {
          int step = static_cast<int>(std::fmod(beat, 4.0) * 4) % kGateSteps;
          g = bparam(s, TgGate + step, true) ? 1 : 0;
        }
        gate_ = g + (gate_ - g) * sm;
        g = gate_;
        v1_ = v2_ = g;
      }
      outL *= g, outR *= g;
      if (dcOn) outL = dc_[0].process(outL), outR = dc_[1].process(outR);
      L[i] = static_cast<float>(dryL * (1 - mix) + outL * mix);
      R[i] = static_cast<float>(dryR * (1 - mix) + outR * mix);
      ++wc_;
    }
    lastL_ = n ? L[n - 1] : 0.f, lastR_ = n ? R[n - 1] : 0.f;
  }

  // J-110: Stop. Stopped, a synced Shuttle is dry already (and drops its
  // slot / hold state); free-running it would keep repeating the bar it holds
  // and the slots' patterns from before the Stop: start over from silence.
  void stopSound() override {
    if (freeRun_) reset();
  }
  void reset() override {
    std::fill(bl_.begin(), bl_.end(), 0.f);
    std::fill(br_.begin(), br_.end(), 0.f);
    gate_ = v1_ = v2_ = 1;
    xfLeft_ = 0, free_ = 0, haveRp_ = false;
    for (auto& b : bs_) b = BankState{};
    holdOn_ = forceHold_ = false, forceNextAt_ = -1;
    dc_[0].reset(), dc_[1].reset();
  }
  int meters(float* out, int max) const override {
    if (max < 4) return 0;
    out[0] = phase_;
    out[1] = static_cast<float>(bs_[0].cur);
    out[2] = static_cast<float>(bs_[1].cur);
    out[3] = vphase_;
    return 4;
  }

 private:
  struct BankState {
    int cur = 0, lastWant = -1, pending = -1, noteReq = -1;
    double switchAt = 0, start = 0, oneShotAt = -1;
    int stack[8] = {};
    int stackN = 0;
  };
  const tg::Slot& slotOf(int b, int k) const {
    k = std::clamp(k, 1, tg::kSlots);
    return b == 0 ? bank_->time[k - 1] : bank_->vol[k - 1];
  }
  void request(int b, int k, double beat, int defTrig) {
    BankState& st = bs_[b];
    if (b == 1 && bs_[0].cur > 0 && slotOf(0, bs_[0].cur).link) return;  // volume follows the linked time slot
    int trig = k > 0 ? slotOf(b, k).trig : -1;
    if (trig < 0) trig = defTrig;
    double S = tg::kSyncBeats[std::clamp(trig, 0, 5)];
    if (S <= 0) {
      switchTo(b, k, beat);
      st.pending = -1;
    } else {
      st.pending = k;
      st.switchAt = std::ceil(beat / S - 1e-9) * S;
    }
  }
  void switchTo(int b, int k, double beat) {
    BankState& st = bs_[b];
    st.cur = k;
    st.oneShotAt = -1;
    if (k <= 0) {
      if (b == 0) forceHold_ = false, forceNextAt_ = -1;
      return;
    }
    const tg::Slot& sl = slotOf(b, k);
    double S = tg::kSyncBeats[std::clamp(sl.pos, 0, 5)];
    // Position sync: the envelope starts at the current sync-grid line (position = beat mod S);
    // off restarts it at the trigger.
    st.start = S > 0 ? beat - std::fmod(beat, S) : beat;
    // Force hold.
    if (sl.force == tg::ForceLast) forceHold_ = true, forceNextAt_ = -1;
    else if (sl.force == tg::ForceNext) forceNextAt_ = std::ceil(beat / 4.0 - 1e-9) * 4.0;
    else forceHold_ = false, forceNextAt_ = -1;
    if (sl.oneShot) st.oneShotAt = beat + 4.0;
    if (b == 0 && sl.link) {  // the volume bank follows
      bs_[1].cur = k;
      const tg::Slot& vs = slotOf(1, k);
      double Sv = tg::kSyncBeats[std::clamp(vs.pos, 0, 5)];
      bs_[1].start = Sv > 0 ? beat - std::fmod(beat, Sv) : beat;
    }
  }
  void read(double rp, bool hq, double& l, double& r) const {
    const size_t N = bl_.size();
    double fl = std::floor(rp);
    double frac = rp - fl;
    long long i0 = static_cast<long long>(fl);
    if (frac < 1e-9) {
      size_t ix = static_cast<size_t>((i0 % static_cast<long long>(N) + static_cast<long long>(N)) % static_cast<long long>(N));
      l = bl_[ix], r = br_[ix];
      return;
    }
    const SincTable& T = sinc();
    const int taps = hq ? 16 : 8, half = taps / 2;
    const std::vector<float>& tab = hq ? T.k16 : T.k8;
    int ph = static_cast<int>(std::lround(frac * SincTable::kPhases));
    const float* k = &tab[static_cast<size_t>(ph * taps)];
    double sl = 0, sr = 0;
    for (int j = 0; j < taps; ++j) {
      long long idx = i0 + j - (half - 1);
      // Never read samples not yet written (future): clamp to the newest.
      if (idx > static_cast<long long>(wc_)) idx = static_cast<long long>(wc_);
      size_t ix = static_cast<size_t>((idx % static_cast<long long>(N) + static_cast<long long>(N)) % static_cast<long long>(N));
      sl += bl_[ix] * k[j], sr += br_[ix] * k[j];
    }
    l = sl, r = sr;
  }
  double coefOf(double seconds) const { return seconds <= 0 ? 0.0 : std::exp(-1.0 / std::max(1e-9, seconds * sr_)); }

  std::unique_ptr<tg::Bank> factory_, bank_;
  std::vector<float> bl_, br_;
  unsigned long long wc_ = 0;
  size_t dataHash_ = 0;
  double sr_ = 0, gate_ = 1, v1_ = 1, v2_ = 1, xfRp_ = 0, xfLeft_ = 0, lastRp_ = 0, free_ = 0, holdT0_ = 0, forceNextAt_ = -1;
  bool haveRp_ = false, holdOn_ = false, forceHold_ = false, fadeDone_ = false;
  bool freeRun_ = false;  // J-110: free-running (plays on while the transport is stopped)
  float lastL_ = 0, lastR_ = 0, phase_ = -1, vphase_ = -1;
  BankState bs_[2];
  DcBlock dc_[2];
  gbref::Engine ref_;
};

void timeGateNote(Effect* fx, int pitch, bool on) {
  if (auto* g = dynamic_cast<TimeGateK3*>(fx)) g->note(pitch, on);
}

std::unique_ptr<Effect> createEffectK3(const std::string& type) {
  if (type == "gl.timegate") return std::make_unique<TimeGateK3>();
  return nullptr;
}

}  // namespace gl

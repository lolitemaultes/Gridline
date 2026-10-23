// Gridline — "tideline" pack: Tideline runtime. Gain follows the selected curve
// over a cycle that is locked to the song position (Tempo) or restarted by an
// audio onset / MIDI note (Audio / MIDI). Everything is per sample, so the
// result does not depend on the block size; the gain is smoothed (anti-click)
// and there is no latency.
#include "../Packs.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "../../engine/Dsp.h"
#include "tideline.h"

namespace gl::packs::tideline {

namespace {

using dsp::Biquad;
using dsp::gainToDb;

float par(const PluginState& s, int i, float def = 0) {
  return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : def;
}
int ipar(const PluginState& s, int i, int def = 0) { return static_cast<int>(std::lround(par(s, i, static_cast<float>(def)))); }

// 4th-order Linkwitz-Riley crossover: low and high bands (two cascaded
// Butterworth sections each) sum to an allpass, so the split is magnitude-flat.
struct Lr4 {
  Biquad la, lb, ha, hb;
  void set(double sr, double hz) {
    la.set(Biquad::LowPass, sr, hz, 0.70710678, 0);
    lb.set(Biquad::LowPass, sr, hz, 0.70710678, 0);
    ha.set(Biquad::HighPass, sr, hz, 0.70710678, 0);
    hb.set(Biquad::HighPass, sr, hz, 0.70710678, 0);
  }
  void split(double x, double& low, double& high) {
    low = lb.process(la.process(x));
    high = hb.process(ha.process(x));
  }
  void clear() {
    la.clear();
    lb.clear();
    ha.clear();
    hb.clear();
  }
};

class Tideline final : public Effect {
 public:
  void setSideIo(const SideIo& io) override { io_ = io; }

  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    const double amount = std::clamp(static_cast<double>(par(s, BwAmount, 1)), 0.0, 1.0);
    const int shape = std::clamp(ipar(s, BwShape), 0, kShapeCustom);
    const int rate = std::clamp(ipar(s, BwRate, kRateDefault), 0, kRateCount - 1);
    const int trig = std::clamp(ipar(s, BwTrigger), 0, 2);
    const bool loop = par(s, BwLoop) >= 0.5f;
    const double endAt = std::clamp(static_cast<double>(par(s, BwEnd, 1)), 0.0, 1.0);
    const double duration = par(s, BwDuration);
    const double shift = std::clamp(static_cast<double>(par(s, BwShift)), -0.5, 0.5);
    const double smoothMs = std::clamp(static_cast<double>(par(s, BwSmooth, 1)), 0.0, 50.0);
    const int band = std::clamp(ipar(s, BwBand), 0, 2);
    const double bandHz = std::clamp(static_cast<double>(par(s, BwBandFreq, 120)), 20.0, 5120.0);
    const bool listen = par(s, BwBandListen) >= 0.5f;
    const int stereo = std::clamp(ipar(s, BwStereo), 0, 2);
    const double tempo = ctx.tempo > 1 ? ctx.tempo : 120.0;
    const double cycleBeats = kRateBeats[rate];
    const double cycleSamples = cycleBeats * 60.0 / tempo * sr;
    if (shape == kShapeCustom) customFrom(s.params, custom_);

    // anti-click smoother (one pole; a floor keeps hard edges from clicking)
    const double tau = std::max(0.05, smoothMs) * 0.001;
    const double smA = std::exp(-1.0 / (tau * sr));

    if (band != BandFull && (bandHz != bandHz_ || sr != bandSr_)) {
      bandL_.set(sr, bandHz);
      bandR_.set(sr, bandHz);
      bandHz_ = bandHz;
      bandSr_ = sr;
    }
    if (sr != sr_) {
      sr_ = sr;
      detPrimed_ = false;
    }

    // ---- triggers that apply to the whole block (MIDI: block start)
    bool midiHit = false;
    if (trig == TrigMidi && ctx.ports) {
      const int port = ipar(s, BwMidiPort, -1), note = ipar(s, BwMidiNote, -1);
      std::array<uint64_t, 2> held{};
      for (int q = 0; q < MidiPortBus::kPorts; ++q) {
        if (port >= 0 ? q != port : !ctx.ports->used[static_cast<size_t>(q)].load(std::memory_order_relaxed)) continue;
        for (int k = (note >= 0 ? note : 0); k <= (note >= 0 ? note : 127); ++k)
          if (ctx.ports->get(q, k)) held[static_cast<size_t>(k >> 6)] |= uint64_t{1} << (k & 63);
      }
      // a note that was not held in the previous block starts the curve
      midiHit = ((held[0] & ~midiHeld_[0]) | (held[1] & ~midiHeld_[1])) != 0;
      midiHeld_ = held;
    } else {
      midiHeld_ = {};
    }

    // ---- audio trigger detector setup
    const float* scL = nullptr;
    const float* scR = nullptr;
    if (trig == TrigAudio) {
      scL = io_.scL ? io_.scL : L;
      scR = io_.scR ? io_.scR : (io_.scL ? io_.scL : R);
      const double lo = std::clamp(static_cast<double>(par(s, BwDetLow, 30)), 20.0, 2000.0);
      const double hi = std::clamp(static_cast<double>(par(s, BwDetHigh, 200)), 40.0, 20000.0);
      if (!detPrimed_ || lo != detLo_ || hi != detHi_) {
        detHp_.set(Biquad::HighPass, sr, lo, 0.70710678, 0);
        detLp_.set(Biquad::LowPass, sr, std::max(hi, lo * 1.2), 0.70710678, 0);
        detLo_ = lo;
        detHi_ = hi;
        detPrimed_ = true;
      }
    }
    const double thr = std::pow(10.0, std::clamp(static_cast<double>(par(s, BwThreshold, -24)), -60.0, 0.0) / 20.0);
    const double rearm = thr * 0.5;  // -6 dB hysteresis
    const double minGap = std::clamp(static_cast<double>(par(s, BwRetrigger, 60)), 5.0, 2000.0) * 0.001 * sr;
    const double envRel = std::exp(-1.0 / (0.025 * sr));

    const double beatsPerSample = tempo / 60.0 / sr;
    const double beat0 = ctx.tick / std::max(1, ctx.ppq);
    float peak = 0;
    for (int i = 0; i < n; ++i) {
      double target = 1.0;
      if (trig == TrigTempo) {
        if (ctx.playing) {
          const double beat = beat0 + i * beatsPerSample;
          double p = beat / cycleBeats + shift;
          p -= std::floor(p);
          pos_ = p;
          target = curveAt(shape, custom_, p, duration);
        } else {
          pos_ = -1;
        }
      } else {
        bool hit = false;
        if (trig == TrigMidi) hit = midiHit && i == 0;
        else {
          const double x = 0.5 * (static_cast<double>(scL[i]) + static_cast<double>(scR[i]));
          const double y = std::fabs(detLp_.process(detHp_.process(x)));
          env_ = y > env_ ? y : y + (env_ - y) * envRel;
          if (!armed_ && env_ < rearm) armed_ = true;
          if (armed_ && env_ >= thr && sinceTrig_ >= minGap) {
            hit = true;
            armed_ = false;
          }
        }
        if (hit) {
          elapsed_ = 0;
          running_ = true;
          sinceTrig_ = 0;
          flash_ = 1;
        }
        if (running_) {
          double p = elapsed_ / std::max(1.0, cycleSamples) + shift;
          if (p < 0) {
            target = 1.0;  // delayed start
            pos_ = -1;
          } else if (loop) {
            p -= std::floor(p);
            pos_ = p;
            target = curveAt(shape, custom_, p, duration);
          } else {
            p = std::min(p, endAt);  // one shot: hold at the end marker
            pos_ = p;
            target = curveAt(shape, custom_, p, duration);
          }
          elapsed_ += 1.0;
        } else {
          pos_ = -1;
        }
        sinceTrig_ += 1.0;
      }
      const double g1 = applyAmount(target, amount);
      gain_ = g1 + (gain_ - g1) * smA;
      const double g = gain_;
      double l = L[i], r = R[i];
      // Gain on the whole signal, or on one band of an LR4 split (the other band passes).
      // Listen (J-98) solos what is being ducked: the band (Lows / Highs) of the
      // ducked component (both channels, the mid or the side); everything that
      // passes untouched is muted. Full + Stereo ducks everything: no change.
      auto duck = [&](double v, Lr4& f) {
        if (band == BandFull) return v * g;
        double low, high;
        f.split(v, low, high);
        double& tgt = band == BandLows ? low : high;
        double& other = band == BandLows ? high : low;
        return listen ? tgt * g : tgt * g + other;
      };
      if (stereo == StStereo) {
        l = duck(l, bandL_);
        r = duck(r, bandR_);
      } else {
        double m = 0.5 * (l + r), sd = 0.5 * (l - r);
        if (stereo == StMid) {
          m = duck(m, bandL_);
          if (listen) sd = 0;
        } else {
          sd = duck(sd, bandL_);
          if (listen) m = 0;
        }
        l = m + sd;
        r = m - sd;
      }
      L[i] = static_cast<float>(l);
      R[i] = static_cast<float>(r);
      peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
    }
    flash_ *= std::exp(-static_cast<double>(n) / (0.12 * sr));
    peak_ = peak;
  }

  void reset() override {
    gain_ = 1;
    pos_ = -1;
    elapsed_ = 0;
    running_ = false;
    sinceTrig_ = 1e12;
    armed_ = true;
    env_ = 0;
    flash_ = 0;
    peak_ = 0;
    midiHeld_ = {};
    bandL_.clear();
    bandR_.clear();
    detHp_.clear();
    detLp_.clear();
  }

  float meter() const override { return static_cast<float>(gainToDb(std::max(gain_, 1e-6))); }

  int meters(float* out, int max) const override {
    const float v[kMeterCount] = {static_cast<float>(pos_), static_cast<float>(gain_), static_cast<float>(flash_), peak_};
    const int k = std::min(max, kMeterCount);
    for (int i = 0; i < k; ++i) out[i] = v[i];
    return k;
  }

 private:
  SideIo io_;
  CustomCurve custom_;
  double sr_ = 0;
  double gain_ = 1;
  double pos_ = -1;
  double elapsed_ = 0;
  bool running_ = false;
  double sinceTrig_ = 1e12;
  bool armed_ = true;
  double env_ = 0;
  double flash_ = 0;
  float peak_ = 0;
  std::array<uint64_t, 2> midiHeld_{};
  Lr4 bandL_, bandR_;
  double bandHz_ = -1, bandSr_ = 0;
  Biquad detHp_, detLp_;
  double detLo_ = -1, detHi_ = -1;
  bool detPrimed_ = false;
};

}  // namespace

std::unique_ptr<Generator> createGenerator(const std::string&) { return nullptr; }

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == "gl.tideline") {
    auto fx = std::make_unique<Tideline>();
    fx->reset();
    return fx;
  }
  return nullptr;
}

bool prepareAux(const PluginState&, ChannelAux&) { return false; }

}  // namespace gl::packs::tideline

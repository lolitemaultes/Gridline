// Gridline — "synths" pack, J-33 group "pocket": Pocket Synth (gl.pocketsynth)
// runtime, following the reference instrument's behaviour (x = 0..1 position):
//   voice mode int(2x) poly / mono / lead; transpose trunc(24x − 12) st;
//   slide: pitch += c·(note − pitch) per 32-sample block, c = 0.9(1 − x)^4 + 0.01;
//   oscillators: one wavetable (1024 band-limited 2048-sample levels), kind
//   int(9x) builds 1..8 phase-locked or detuned readers, jittered, no
//   interpolation; noise x²; mix (osc + noise)·0.2 + 0.07 through the amp
//   envelope BEFORE the filter;
//   filter: 4-pole ladder, cutoff 2^(4 + 10.3 x + key + vel ± LFO) Hz moving
//   linearly towards 2^(… + 10·(2x − 1)) with the filter envelope, peak
//   feedback −4.05·0.99 x, the continuous ladder solved per block (matrix
//   power) with a soft-clipped first state; output ×5 minus a slow DC tracker;
//   envelopes: per-sample one-poles chasing 4.0 (attack 4e-7 / f⁴, decay
//   4e-6 / f⁴, release 4e-5 / f₂⁴ per sample, f = 0.95x + 0.05);
//   LFO (one, global): period in beats from a 12-step table, locked to the
//   song position while playing; sine / falling saw / square; amount (2x − 1)²
//   signed, to cutoff (±4 oct), pitch (±12 st) or the oscillator mod;
//   effects: drive + decimator → tempo delay → phaser → chorus.
// The wavetable spectrum is the reference's own data file, read in place from
// the installed instrument (never shipped); without it a fitted saw spectrum
// stands in. Realtime-safe: tables are built on the UI thread (prepareAux).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../../core/flp/Flp.h"
#include "../Packs.h"
#include "pocket.h"

namespace gl::packs::synths {

namespace {

using namespace pocket;

constexpr int kBlock = 32;
constexpr int kVoices = 99;
constexpr int kTables = 1024, kLen = 2048;

struct PocketTables final : AuxData {
  std::vector<float> t;  // kTables × kLen; table s holds harmonics 1..1024 − s
  bool reference = false;
  const float* table(int s) const { return t.data() + static_cast<size_t>(s) * kLen; }
};

std::shared_ptr<const PocketTables> pocketTables() {
  static std::mutex mu;
  static std::weak_ptr<const PocketTables> cache;
  std::lock_guard<std::mutex> lk(mu);
  if (auto c = cache.lock()) return c;
  std::vector<float> amp(1024), ph(1024);
  bool ref = false;
  const std::string fl = flp::defaultFactoryData();
  if (!fl.empty()) {
    const std::string path = fl + "/Plugins/Fruity/Generators/MiniSynth/oscdata.dat";
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
      float buf[2048];
      if (std::fread(buf, sizeof(float), 2048, f) == 2048) {
        ref = true;
        for (int h = 0; h < 1024; ++h) amp[static_cast<size_t>(h)] = buf[h], ph[static_cast<size_t>(h)] = buf[1024 + h];
        for (int h = 0; h < 1024; ++h)
          if (!std::isfinite(amp[static_cast<size_t>(h)]) || !std::isfinite(ph[static_cast<size_t>(h)])) ref = false;
      }
      std::fclose(f);
    }
  }
  if (!ref)
    for (int h = 1; h <= 1024; ++h) {
      amp[static_cast<size_t>(h - 1)] = static_cast<float>((0.2994 / h) * (0.467 + 0.533 * std::exp(-(h - 1) / 3.7)));
      ph[static_cast<size_t>(h - 1)] = static_cast<float>(-M_PI / 2);
    }
  auto p = std::make_shared<PocketTables>();
  p->reference = ref;
  p->t.assign(static_cast<size_t>(kTables) * kLen, 0.f);
  std::vector<double> acc(kLen, 0.0);
  // Cumulative: after harmonic h the cycle holds 1..h = table 1024 − h.
  for (int h = 1; h <= 1024; ++h) {
    const double a = amp[static_cast<size_t>(h - 1)], phi = ph[static_cast<size_t>(h - 1)];
    if (a != 0.0)
      for (int i = 0; i < kLen; ++i) acc[static_cast<size_t>(i)] += a * std::cos(2.0 * M_PI * h * i / kLen + phi);
    float* dst = p->t.data() + static_cast<size_t>(1024 - h) * kLen;
    for (int i = 0; i < kLen; ++i) dst[i] = static_cast<float>(acc[static_cast<size_t>(i)]);
  }
  cache = p;
  return p;
}

// Envelope coefficient laws (per sample).
double aLaw(double x) {
  if (x < 0.001) return 1.0;
  const double f = 0.95 * x + 0.05;
  return 4e-7 / (f * f * f * f);
}
double dLaw(double x) {
  if (x < 0.001) return 1.0;
  const double f = 0.95 * x + 0.05;
  return 4e-6 / (f * f * f * f);
}
double rLaw(double x) {
  const double f = 0.95 * x + 0.05, f2 = 0.95 * f + 0.05;
  return 4e-5 / (f2 * f2 * f2 * f2);
}
constexpr double kPeriods[12] = {0.25, 0.375, 0.5, 0.75, 1, 2, 4, 8, 16, 64, 128, 256};

struct Lcg {
  uint32_t s;
  uint32_t mul, add;
  uint32_t next() { return s = s * mul + add; }
};

class PocketSynthGen final : public Generator {
 public:
  PocketSynthGen() {
    for (auto& v : v_) newPhases(v);
    for (auto& h : held_) h = 0;
  }
  bool active() const override {
    if (tail_ > 0) return true;
    for (const auto& v : v_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false, gated = false, firstGain = true;
    uint32_t id = 0;
    int note = 60;
    double pitch = 60, frac = 0;
    float vel = 1;
    uint64_t stamp = 0;
    double eAmp = 0, tAmp = 0, cAmp = 1, eFlt = 0, tFlt = 0, cFlt = 1;
    bool atkAmp = false, atkFlt = false;
    double gL = 0, gR = 0, tgL = 0, tgR = 0;
    double drift = 0;
    uint32_t ph[8] = {};
    uint32_t inc[8] = {};
    float lvl[8] = {};
    int nOsc = 1, table = 0;
    double s[4] = {};
    double M[4][5] = {};
    double dc = 0;
    Lcg rng{0x8AEu, 0x0BB38435u, 0x3619636Bu};  // jitter, noise and drift (per voice: block-size independent)
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    bpm_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    playing_ = ctx.playing;
    tick0_ = ctx.tick;
    ppq_ = ctx.ppq > 0 ? ctx.ppq : 96;
    blockPos_ = 0;
    tables_ = dynamic_cast<const PocketTables*>(aux.custom.get());
    for (int i = 0; i < PkCount; ++i) {
      const float v = i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : 0.f;
      x_[i] = std::clamp(std::isfinite(v) ? v : 0.f, 0.f, 1.f);
    }
    if (sr_ != lineSr_) {
      lineSr_ = sr_;
      delay_.assign(static_cast<size_t>(1.5 * sr_) + 1, 0.f);
      chorus_.assign(static_cast<size_t>(std::max(2.0, std::trunc(0.01 * sr_))), 0.f);
      for (auto& v : v_) v.on = false;
    }
    const bool dOn = x_[PkDelay] != 0.f, cOn = x_[PkChorus] != 0.f, pOn = x_[PkPhaser] != 0.f;
    if (dOn && !delayOn_) std::fill(delay_.begin(), delay_.end(), 0.f), dPos_ = 0, dz_ = 0;
    if (cOn && !chorusOn_) std::fill(chorus_.begin(), chorus_.end(), 0.f), cPos_ = 0;
    if (pOn && !phaserOn_) {
      for (auto& a : apS_)
        for (double& z : a) z = 0;
      apY_[0] = apY_[1] = 0;
      kappa_ = -1;
    }
    delayOn_ = dOn, chorusOn_ = cOn, phaserOn_ = pOn;
  }

  int mode() const { return std::min(2, static_cast<int>(2 * x_[PkVoiceMode])); }

  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    const int note = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    const int m = mode();
    bool others = false;
    for (int k = 0; k < 128; ++k) others = others || (held_[k] && k != note);
    held_[note] = 1;
    Voice* v = nullptr;
    if (m == 0) {
      for (int i = kVoices - 1; i >= 0 && !v; --i)
        if (!v_[i].on) v = &v_[i];
      if (!v) {
        v = &v_[0];
        for (auto& c : v_)
          if (c.stamp < v->stamp) v = &c;
      }
    } else {
      v = &v_[0];
    }
    const bool legato = m == 2 && v->on && v->gated;
    double start = lastNote_ >= 0 ? lastNote_ : note;
    if (m == 2 && !others) start = note;
    if (!legato || m != 2) v->pitch = start;
    v->id = id;
    v->note = note;
    v->frac = pitch - note;
    lastNote_ = note;
    v->stamp = ++clock_;
    if (!v->on) v->rng.s = 0x8AEu + static_cast<uint32_t>(clock_) * 0x9E3779B9u;
    v->gated = true;
    if (!legato) {
      v->vel = std::clamp(velocity, 0.f, 1.f);
      v->eFlt = 0;
      v->tAmp = v->tFlt = 4.0;
      v->cAmp = aLaw(x_[PkAmpA]);
      v->cFlt = aLaw(x_[PkFltA]);
      v->atkAmp = v->atkFlt = true;
      if (!v->on) v->firstGain = true;
    }
    const bool fresh = !v->on;
    v->on = true;
    if (fresh) setupVoice(*v, lam_, lfoDest_);  // a new voice sounds from its first sample
  }

  void releaseVoice(Voice& v) {
    v.gated = false;
    v.atkAmp = v.atkFlt = false;
    v.tAmp = 0, v.cAmp = rLaw(x_[PkAmpR]);
    v.tFlt = 0, v.cFlt = dLaw(x_[PkFltR]);
  }

  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) == 0xB0 && d1 == 1) wheel_ = d2 / 127.0;  // J-92: mod wheel
  }
  void onNoteOff(uint32_t id) override {
    int note = -1;
    for (auto& v : v_)
      if (v.on && v.id == id) note = v.note;
    if (note < 0) return;
    held_[note] = 0;
    for (auto& v : v_) {
      if (!v.on || v.id != id || !v.gated) continue;
      if (mode() == 2) {
        int to = -1;
        for (int k = note - 1; k >= 0 && to < 0; --k)
          if (held_[k]) to = k;
        for (int k = 127; k > note && to < 0; --k)
          if (held_[k]) to = k;
        if (to >= 0) {
          v.note = to;
          continue;
        }
      }
      releaseVoice(v);
    }
  }
  void onAllOff() override {
    for (auto& h : held_) h = 0;
    for (auto& v : v_)
      if (v.on && v.gated) releaseVoice(v);
  }
  void onKill() override {
    for (auto& v : v_) v.on = false;
    for (auto& h : held_) h = 0;
    tail_ = 0;
  }

  void newPhases(Voice& v) {
    for (auto& p : v.ph) {
      for (int k = 0; k < 32; ++k) rngP_.next();
      p = rngP_.s;
    }
  }

  static double uniform(Lcg& r) { return r.next() * (1.0 / 2147483648.0) - 1.0; }

  // Global LFO value Λ for this block.
  double lfoBlock(int n, double& phase, int rateParam) {
    const double period = kPeriods[std::min(11, static_cast<int>(11 * x_[rateParam]))];
    if (playing_) {
      const double beats = (tick0_ + blockPos_ * (bpm_ * ppq_ / 60.0) / sr_) / ppq_;
      phase = beats / period - std::floor(beats / period);
    } else {
      phase += bpm_ * n / (period * 60.0 * sr_);
      phase -= std::floor(phase);
    }
    return phase;
  }

  void setupVoice(Voice& v, double lam, int lfoDest) {
    // Drift, glide, pitch.
    v.drift += 0.01 * (uniform(v.rng) - v.drift);
    const double c = std::pow(1.0 - x_[PkSlide], 4) * 0.9 + 0.01;
    v.pitch += c * (v.note - v.pitch);
    const int transpose = static_cast<int>(24 * x_[PkTranspose] - 12);
    const double P = transpose + v.pitch + v.frac + 0.08 * v.drift + (lfoDest == 1 ? 12 * lam : 0.0);
    const double f = 220.0 * std::exp2((P - 57.0) / 12.0);
    // Oscillator kind.
    const int kind = std::min(9, static_cast<int>(9 * x_[PkOscKind]));
    double m = x_[PkOscMod] + (lfoDest == 2 ? 12 * lam : 0.0);
    const double mu = std::fabs(m);
    const double r = std::exp2(std::trunc((2 * m - 1) * 12) / 12.0 + 0.00625);
    double fr[8] = {f, f, f, f, f, f, f, f};
    float lv[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    double off[8] = {0};  // phase offsets relative to oscillator 0 (cycles), < 0 = free
    for (double& o : off) o = -1;
    int n = 1;
    switch (kind) {
      case 0: n = 3, lv[1] = -1, off[1] = 1 - mu, off[2] = 0; break;
      case 1: n = 2, fr[1] = r * f; break;
      case 2: n = 2, lv[1] = -1, off[1] = 1 - mu; break;
      case 3: n = 3, lv[1] = -1, off[1] = 0.5, fr[2] = r * f; break;
      case 4: n = 4, lv[1] = -1, lv[3] = -1, off[1] = 0.5625, fr[2] = fr[3] = r * f; break;
      case 5: {
        n = 8;
        static const double d[8] = {-0.05, -0.0375, -0.025, -0.0125, 0, 0.0125, 0.025, 0.0375};
        for (int k = 0; k < 8; ++k) fr[k] = f * (1 + m * d[k]), lv[k] = 0.5f;
        break;
      }
      case 6:
        n = 6;
        for (int k = 0; k < 6; ++k) lv[k] = 0.5f, off[k] = k == 0 ? -1 : k * mu / 2;
        break;
      case 7: n = 2, off[1] = 1 - mu; break;
      case 8: n = 3, fr[0] = (0.5 - 0.005 * m) * f, fr[2] = (2 + 0.02 * m) * f; break;
      case 9: n = 3, fr[0] = (1.5 - 0.015 * m) * f, fr[2] = (2 + 0.02 * m) * f; break;
      default: break;
    }
    v.nOsc = n;
    for (int k = 0; k < n; ++k) {
      v.inc[k] = static_cast<uint32_t>(std::max(0.0, fr[k]) * 4294967295.0 / sr_);
      v.lvl[k] = lv[k];
      if (k > 0 && off[k] >= 0) v.ph[k] = v.ph[0] + static_cast<uint32_t>(static_cast<int64_t>(std::llround(off[k] * 4294967296.0)) & 0xFFFFFFFFll);
    }
    if (fr[0] > sr_ / 2048.0) v.table = std::clamp(1023 - static_cast<int>(std::floor(sr_ / (2.0 * fr[0]))), 0, 1023);
    else v.table = 0;
    // Filter coefficients.
    const double e = 2.0 * x_[PkFltAmount] - 1.0;
    double base = 4 + 10.3 * (x_[PkCutoff] + 0.01 * v.drift) + x_[PkKeyTrack] * (P / 12.0 - 4) + 14 * (v.vel - 0.5) * x_[PkVelTrack];
    if (lfoDest == 0) base += 4 * lam;
    if (x_[PkWheelDest] < 1) base += 4.0 * wheel_ * x_[PkWheelAmount];  // J-92: mod wheel to cutoff (up to 4 octaves)
    const double lo = std::exp2(base), hi = std::exp2(base + 10 * e);
    const double fc = std::min(20000.0, lo + v.eFlt * (hi - lo));
    const double g = std::clamp(fc / (655.36 * sr_), 0.0, 1.0), q = 1 - g;
    const double K = -4.05 * std::clamp(0.99 * x_[PkPeak], 0.0, 1.0);
    double A[5][5] = {{q, 0, 0, g * K, g},
                      {g * q, q, 0, g * g * K, g * g},
                      {g * g * q, g * q, q, g * g * g * K, g * g * g},
                      {g * g * g * q, g * g * q, g * q, q + g * g * g * g * K, g * g * g * g},
                      {0, 0, 0, 0, 1}};
    for (int it = 0; it < 12; ++it) {
      double B[5][5] = {};
      for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
          double acc = 0;
          for (int k = 0; k < 5; ++k) acc += A[i][k] * A[k][j];
          B[i][j] = acc;
        }
      std::memcpy(A, B, sizeof A);
    }
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 5; ++j) v.M[i][j] = A[i][j];
    // Output gain targets.
    const bool dying = !v.gated && v.eAmp <= 1e-5;
    v.tgL = dying ? 0 : v.vel;
    v.tgR = dying ? 0 : v.vel;
    if (v.firstGain) v.gL = v.tgL, v.gR = v.tgR, v.firstGain = false;
  }

  void renderVoice(Voice& v, float* L, float* R, int n) {
    if (!tables_) return;
    const float* T = tables_->table(v.table);
    const double noise = x_[PkNoise] * x_[PkNoise];
    for (int i = 0; i < n; ++i) {
      const uint32_t J = v.rng.next() & 0x3FFFFFu;
      double S = 0;
      for (int k = 0; k < v.nOsc; ++k) {
        S += v.lvl[k] * T[(v.ph[k] + J) >> 21];
        v.ph[k] += v.inc[k];
      }
      const double N = noise * uniform(v.rng);
      // Envelopes.
      v.eAmp += v.cAmp * (v.tAmp - v.eAmp);
      if (v.atkAmp && v.eAmp > 1) v.eAmp = 1, v.tAmp = x_[PkAmpS], v.cAmp = dLaw(x_[PkAmpD]), v.atkAmp = false;
      v.eFlt += v.cFlt * (v.tFlt - v.eFlt);
      if (v.atkFlt && v.eFlt > 1) v.eFlt = 1, v.tFlt = x_[PkFltS], v.cFlt = dLaw(x_[PkFltD]), v.atkFlt = false;
      const double X = v.eAmp * ((S + N) * 0.2 + 0.07);
      double out[4];
      for (int r = 0; r < 4; ++r) out[r] = v.M[r][0] * v.s[0] + v.M[r][1] * v.s[1] + v.M[r][2] * v.s[2] + v.M[r][3] * v.s[3] + v.M[r][4] * X;
      v.s[0] = out[0] / (1 + out[0] * out[0]);
      v.s[1] = out[1], v.s[2] = out[2], v.s[3] = out[3];
      const double Y = 5 * out[3];
      v.dc += 0.005 * (Y - v.dc);
      const double o = Y - v.dc;
      v.gL += 0.005 * (v.tgL - v.gL);
      v.gR += 0.005 * (v.tgR - v.gR);
      L[i] += static_cast<float>(v.gL * o);
      R[i] += static_cast<float>(v.gR * o);
      if (!v.gated && v.eAmp <= 1e-5 && v.gL < 1e-5 && v.gR < 1e-5) break;  // ends at this sample (block-size invariant)
    }
    if (!v.gated && v.eAmp <= 1e-5 && v.gL < 1e-5 && v.gR < 1e-5) {
      v.on = false;
      newPhases(v);
      v.s[0] = v.s[1] = v.s[2] = v.s[3] = 0;
      v.dc = 0;
    }
  }

  void effects(float* L, float* R, int n) {
    if (x_[PkDist] != 0.f) {
      const double D = 20.0 * x_[PkDrive] * x_[PkDrive];
      const double G = 0.3 + 0.7 * std::pow(1 - D / 16, 8);
      auto shape = [&](double x) { return (x >= 0 ? std::pow(10.0, -D * x) * (x - 1) + 1 : std::pow(10.0, D * x) * (x + 1) - 1) * G; };
      const int H = static_cast<int>(std::min(static_cast<double>(x_[PkDecimate]) * x_[PkDecimate], 1.0) * 64);
      for (int i = 0; i < n; ++i) {
        L[i] = static_cast<float>(shape(L[i]));
        R[i] = static_cast<float>(shape(R[i]));
        if (H > 0) {
          if (decC_ < 1) decL_ = L[i], decR_ = R[i], decC_ += H;
          else L[i] = decL_, R[i] = decR_;
          decC_ -= 1;
        }
      }
    }
    if (delayOn_ && !delay_.empty()) {
      static const double kBeats[4] = {0.25, 0.5, 0.75, 1.0};
      double phi = kBeats[std::min(3, static_cast<int>(3 * x_[PkDelayTime]))] * 40.0 / bpm_;
      while (phi > 1) phi *= 0.5;
      const int Lb = static_cast<int>(delay_.size()) - 1;
      const int D = std::clamp(static_cast<int>((Lb - 1) * phi), 1, Lb);
      const double fb = x_[PkDelayFeedback], k = 0.99 * x_[PkDelayFilter] + 0.01, wet = x_[PkDelayMix] * x_[PkDelayMix];
      for (int i = 0; i < n; ++i) {
        if (dPos_ >= D) dPos_ = 0;
        const double r = delay_[static_cast<size_t>(dPos_)];
        dz_ += (fb * r + 1e-30 - dz_) * k;
        delay_[static_cast<size_t>(dPos_)] = static_cast<float>(L[i] + R[i] + dz_);
        L[i] += static_cast<float>(wet * dz_);
        R[i] += static_cast<float>(wet * dz_);
        if (std::fabs(dz_) > 1e-5) tail_ = static_cast<int64_t>(sr_ / 2) + i + 1;  // half a second after this sample
        dPos_ = (dPos_ + 1) % D;
      }
    }
    if (phaserOn_) {
      const double cs = phCos_;  // phaser LFO, stepped on the control grid
      const double tgt = std::max(static_cast<double>(x_[PkPhaserFreq]) * x_[PkPhaserFreq], 1e-5);
      if (kappa_ < 0) kappa_ = tgt;
      const double w = 0.5 * x_[PkPhaserWidth] * x_[PkPhaserWidth] * x_[PkPhaserWidth];
      const double lo = 50.0 / sr_, span = 20000.0 / sr_ - 50.0 / sr_;
      const double fb = 0.98 * std::cbrt(static_cast<double>(x_[PkPhaserFeedback]));
      const double mix = std::max(static_cast<double>(x_[PkPhaserMix]), 1e-5);
      for (int i = 0; i < n; ++i) {
        kappa_ += 0.001 * (tgt - kappa_);
        for (int ch = 0; ch < 2; ++ch) {
          const double om = std::clamp((kappa_ + w * (ch == 0 ? 1 - cs : 1 + cs)) * span + lo, 1e-5, 0.499);
          const double a = (1 - om) / (1 + om);
          float* io = ch == 0 ? L : R;
          double in = io[i] + fb * apY_[ch] + 1e-30;
          for (int k = 0; k < 6; ++k) {
            const double t = apS_[ch][k] - a * in;
            apS_[ch][k] = in + a * t;
            in = t;
          }
          apY_[ch] = in;
          io[i] += static_cast<float>(mix * in);
        }
      }
    }
    if (chorusOn_ && chorus_.size() > 1) {
      const int Nc = static_cast<int>(chorus_.size());
      const double rate = 4.0 * x_[PkChorusRate] * x_[PkChorusRate];
      const double d = 0.36 * x_[PkChorusDepth] + 0.04, wet = x_[PkChorusMix] * x_[PkChorusMix];
      auto read = [&](double off) {
        double p = cPos_ + off;
        p -= std::floor(p / Nc) * Nc;
        const int i0 = static_cast<int>(p) % Nc, i1 = (i0 + 1) % Nc;
        const double fr = p - std::floor(p);
        return chorus_[static_cast<size_t>(i0)] * (1 - fr) + chorus_[static_cast<size_t>(i1)] * fr;
      };
      for (int i = 0; i < n; ++i) {
        chorus_[static_cast<size_t>(cPos_)] = L[i] + R[i];
        const double sL = 0.5 + d * std::sin(2 * M_PI * chL_);
        const double tL = read(sL * sL * Nc * d);
        const double tR = read(0.5 + d * std::sin(2 * M_PI * chR_));
        chL_ += (rate + 0.20) / sr_, chR_ += (rate + 0.25) / sr_;
        chL_ -= std::floor(chL_), chR_ -= std::floor(chR_);
        L[i] += static_cast<float>(wet * tL);
        R[i] += static_cast<float>(wet * tR);
        cPos_ = (cPos_ + 1) % Nc;
      }
    }
  }

  double lfoBlockPhase_(int n) { return lfoBlock(n, phPhase_, PkPhaserRate); }

  void render(float* L, float* R, int n) override {
    int i = 0;
    while (i < n) {
      const int len = std::min(n - i, kBlock - grid_);
      if (grid_ == 0) {
        double ph = lfoBlock(kBlock, lfoPhase_, PkLfoRate);
        phCos_ = std::cos(2 * M_PI * lfoBlockPhase_(kBlock));
        const int shape = std::min(2, static_cast<int>(2 * x_[PkLfoShape]));
        const double wave = shape == 0 ? std::sin(2 * M_PI * ph) : (shape == 1 ? 1 - 2 * ph : (ph < 0.5 ? 1 : -1));
        // J-92: the mod wheel (CC 1) deepens the LFO when "Wheel to" is LFO (was never read).
        const double a0 = 2.0 * x_[PkLfoAmount] - 1.0;
        const double a = x_[PkWheelDest] >= 1 ? a0 + (a0 < 0 ? -1.0 : 1.0) * wheel_ * x_[PkWheelAmount] : a0, ac = std::clamp(a, -1.0, 1.0);
        lam_ = ac * ac * wave * (a < 0 ? -1 : 1);
        lfoDest_ = std::min(2, static_cast<int>(2 * x_[PkLfoDest]));
        bool on = false;
        for (auto& v : v_)
          if (v.on) setupVoice(v, lam_, lfoDest_), on = true;
        // The effects run while voices sound and for half a second after, decided on the grid.
        if (on) tail_ = static_cast<int64_t>(sr_ / 2);
        fxOn_ = on || tail_ > 0;
      }
      float bl[kBlock] = {}, br[kBlock] = {};
      bool any = false;
      for (auto& v : v_)
        if (v.on) renderVoice(v, bl, br, len), any = true;
      if (any && !fxOn_) fxOn_ = true, tail_ = static_cast<int64_t>(sr_ / 2);  // a note started mid-period
      if (fxOn_) effects(bl, br, len);
      tail_ -= len;
      for (int q = 0; q < len; ++q) L[i + q] += bl[q], R[i + q] += br[q];
      i += len;
      blockPos_ += len;
      grid_ = (grid_ + len) % kBlock;
    }
  }

 private:
  double sr_ = 44100, bpm_ = 120, lineSr_ = 0, tick0_ = 0;
  int ppq_ = 96;
  bool playing_ = false;
  int64_t blockPos_ = 0, tail_ = 0;
  const PocketTables* tables_ = nullptr;
  float x_[PkCount] = {};
  uint8_t held_[128];
  Voice v_[kVoices];
  int lastNote_ = -1, grid_ = 0;
  bool fxOn_ = false;
  double phCos_ = 1;
  uint64_t clock_ = 0;
  Lcg rngP_{0x12345u, 0x4D66B561u, 0x16C0A8E8u};
  double lfoPhase_ = 0, phPhase_ = 0, lam_ = 0;
  int lfoDest_ = 0;
  double wheel_ = 0;  // mod wheel (CC 1), 0..1
  // Effects.
  bool delayOn_ = false, chorusOn_ = false, phaserOn_ = false;
  std::vector<float> delay_, chorus_;
  int dPos_ = 0, cPos_ = 0;
  double dz_ = 0, chL_ = 0.62, chR_ = 0.10, kappa_ = -1;
  double apS_[2][6] = {}, apY_[2] = {};
  float decL_ = 0, decR_ = 0;
  double decC_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makePocketSynth() { return std::make_unique<PocketSynthGen>(); }

bool preparePocketAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != "gl.pocketsynth") return false;
  aux = ChannelAux{};
  aux.custom = pocketTables();
  return true;
}

// J-51: remembered per FL data folder — the FLP decoder asks on every decode
// (the export's solver decodes thousands of times) and the tables are only
// cached while an instrument holds them, so each question rebuilt them.
bool pocketReferenceTables() {
  static std::mutex mu;
  static std::string folder;
  static int known = -1;
  const std::string fl = flp::defaultFactoryData();
  {
    std::lock_guard<std::mutex> lk(mu);
    if (known >= 0 && folder == fl) return known != 0;
  }
  const bool ref = pocketTables()->reference;
  std::lock_guard<std::mutex> lk(mu);
  folder = fl, known = ref ? 1 : 0;
  return ref;
}

}  // namespace gl::packs::synths

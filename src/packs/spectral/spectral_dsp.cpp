// Gridline — "spectral" pack: runtimes.
//   gl.vocoder   channel vocoder (modulator left, carrier right or built-in)
//   gl.pitch     pitch shifter (delay-based "fast" mode, phase-vocoder mode)
//   gl.pitcher   monophonic pitch corrector + harmonizer (YIN + PSOLA)
//   gl.scratcher turntable scratch over a buffer of the input
//   gl.splitter  Linkwitz-Riley band splitter with per-band controls
//   gl.spread    stereo spreader / enhancer
// Everything is allocated in constructors; process() never allocates.
#include <array>
#include <atomic>
#include <memory>
#include <cstring>
#include <functional>
#include "../../engine/Dsp.h"
#include "../Packs.h"
#include "spectral.h"
#include "spectral_blocks.h"
#include "../dynamics/maximus_ref.h"

namespace gl::packs::spectral {

namespace {

using blk::kPi;

// Parameter reader with descriptor defaults for short vectors.
struct Params {
  const PluginInfo* info = nullptr;
  const PluginState* st = nullptr;
  float operator[](int k) const {
    size_t i = static_cast<size_t>(k);
    if (st && i < st->params.size()) return st->params[i];
    if (info && i < info->params.size()) return static_cast<float>(info->params[i].def);
    return 0.0f;
  }
  bool on(int k) const { return (*this)[k] >= 0.5f; }
  int choice(int k) const { return static_cast<int>((*this)[k] + 0.5f); }
};

inline double coefFor(double sr, double ms) { return std::exp(-1.0 / std::max(1.0, ms * 0.001 * sr)); }

// =================================================================== Vocoder
// Reference model data prepared on the UI thread (packs::prepareAux): the 13
// per-band curves sampled on a fine grid, pitch-follow flags, held keys.
struct VocMapsAux final : AuxData {
  static constexpr int kT = 1025;
  std::vector<float> sge;  // J-42: enhancer program (gl.enhance parameters), empty = Gridline's enhancer
  float t[kVxMaps][kT];
  bool pf[kVxMaps] = {};
  int keys[16] = {};
  int nkeys = 0;
  uint64_t hash = 0;
  void build(const VxData& d) {
    for (int m = 0; m < kVxMaps; ++m) {
      for (int i = 0; i < kT; ++i) t[m][i] = static_cast<float>(vxEval(d.maps[m], static_cast<double>(i) / (kT - 1)));
      pf[m] = d.pf[m];
    }
    nkeys = 0;
    for (int k : d.keys)
      if (nkeys < 16) keys[nkeys++] = k;
  }
  double at(int m, double x) const {
    double p = std::clamp(x, 0.0, 1.0) * (kT - 1);
    int i = std::min(kT - 2, static_cast<int>(p));
    double f = p - i;
    return t[m][i] + (t[m][i + 1] - t[m][i]) * f;
  }
};
const VocMapsAux& vocDefaultMaps() {
  static const VocMapsAux* d = [] {
    auto* a = new VocMapsAux;
    a->build(vxDefaultData());
    return a;
  }();
  return *d;
}

// Reference vocoder engine (J-1): band layout from the distribution curve on
// the reference frequency scale, 1..4 stagger-tuned resonators per band,
// hold / attack / release followers with curves and minimum times, the 13
// per-band curves, unison analysis copies, pass-through and noise filters,
// contours, a built-in carrier synth and an output enhancer.
class VocRef {
 public:
  void reset() {
    for (auto& b : b_) {
      for (auto& u : b.ana)
        for (auto& f : u) f.clear();
      for (auto& f : b.syn) f.clear();
      for (auto& e : b.env) e = 0;
      for (auto& h : b.hold) h = 0;
      b.cenv = 0;
    }
    clearBanks();
    modHp_.clear();
    carLp_.clear();
    noiseHp_.clear();
    sib_.clear();
    for (auto& o : osc_) o = 0;
    noise_.seed(0x51F15EEDu);
    hfEnv_ = totEnv_ = unv_ = cont_ = carEnv_ = 0;
    enhEnv_ = 0;
    enhHp_[0].clear();
    enhHp_[1].clear();
    sr_ = 0;
    key_[0] = -1;
  }

  bool skipEnhance = false;  // J-42: the enhancer stage runs on the mastering engine instead
  void process(const Params& p, const VocMapsAux* aux, const ProcessContext& ctx, const float* scL, const float* scR, float* L,
               float* R, int n) {
    const VocMapsAux& mp = aux ? *aux : vocDefaultMaps();
    const double sr = ctx.sampleRate;
    design(p, mp, sr);
    const int nb = nb_, ord = ord_, uni = uni_;
    const bool rms = p.choice(VxHoldMode) == 1, freeze = p.on(VcHold), denoise = p.on(VxDenoise);
    const double ca = std::clamp(static_cast<double>(p[VxAttCurve]), 1.0, 4.0), cr = std::clamp(static_cast<double>(p[VxRelCurve]), 1.0, 4.0);
    const bool lr = p.on(VxLrEncoding);
    const int carSrc = p.choice(VxCarSource);
    const bool scOk = scL && scR;
    const bool modSc = !lr && p.choice(VcModSource) == 1 && scOk;
    const int listen = p.choice(VcListen);
    const bool cMod = p.on(VxContourMod), cNoise = p.on(VxContourNoise), cCar = p.on(VxContourCar), cCons = p.on(VxContourCons);
    const double wet = blk::dbGain(p[VcWetLevel]);
    double modPass = blk::dbGain(p[VcModLevel]), carPass = blk::dbGain(p[VcCarLevel]);
    double nz = static_cast<double>(std::clamp(p[VxNoise], 0.0f, 1.0f));
    double noiseG = 0.5 * (std::pow(7.0, nz) - 1.0) / 6.0;
    if (cMod && cCar) modPass *= freeze ? 0.0 : 4.0;
    if (cNoise && cCar) noiseG *= 4.0;
    const bool modHpOn = p[VxModHp] > kVxOffLow, carLpOn = p[VxCarLp] < kVxOffHigh, noiseHpOn = p[VxNoiseHp] > kVxOffLow;
    const double enh = std::clamp(static_cast<double>(p[VxEnhance]), 0.0, 1.0);
    // Carrier synth keys: MIDI port notes, else the held keys, else root + chord.
    int keys[16];
    int nk = 0;
    const int port = static_cast<int>(std::lround(p[VxMidiPort]));
    if (ctx.ports && port >= 0 && port < MidiPortBus::kPorts)
      for (int k = 0; k < 128 && nk < 16; ++k)
        if (ctx.ports->get(port, k)) keys[nk++] = k;
    if (nk == 0 && mp.nkeys > 0)
      for (int k = 0; k < mp.nkeys; ++k) keys[nk++] = mp.keys[k];
    if (nk == 0) {
      static const int chords[8][4] = {{0, -99, -99, -99}, {0, 12, -99, -99}, {0, 7, -99, -99}, {0, 4, 7, -99},
                                       {0, 3, 7, -99},     {0, 5, 7, -99},    {0, 4, 7, 11},    {0, 3, 7, 10}};
      const int chord = std::clamp(p.choice(VcChord), 0, 7);
      for (int k = 0; k < 4; ++k)
        if (chords[chord][k] > -99) keys[nk++] = static_cast<int>(std::lround(p[VcRoot])) + chords[chord][k];
    }
    const int voices = std::clamp(static_cast<int>(std::lround(p[VxCarVoices])), 1, 4);
    const double detune = p[VxCarDetune];
    double dts[16][4];
    for (int k = 0; k < nk; ++k)
      for (int v = 0; v < voices; ++v) {
        double off = voices > 1 ? (static_cast<double>(v) / (voices - 1) * 2.0 - 1.0) * detune : 0.0;
        dts[k][v] = std::min(0.45, dsp::noteToHz(keys[k] + off / 100.0) / sr);
      }
    const int tone = std::clamp(p.choice(VxTone), 0, 8);
    const double synGain = 0.5 / std::sqrt(static_cast<double>(std::max(1, nk * voices)));
    const double hfA = coefFor(sr, 2.0), hfR = coefFor(sr, 25.0), contA = coefFor(sr, 3.0), contR = coefFor(sr, 60.0);
    const double eA = coefFor(sr, 5.0), eR = coefFor(sr, 120.0);
    const double invU = 1.0 / uni;

    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      double mod, car;
      if (lr) {
        mod = l;
        car = r;
      } else {
        mod = modSc ? 0.5 * (static_cast<double>(scL[i]) + scR[i]) : 0.5 * (l + r);
        if (carSrc == 1)
          car = 0.5 * (l + r);
        else if (carSrc == 2)
          car = scOk ? 0.5 * (static_cast<double>(scL[i]) + scR[i]) : 0.5 * (l + r);
        else
          car = synth(tone, nk, voices, dts) * synGain;
      }
      // Unvoiced (consonant) detector.
      double hf = sib_.process(mod), ah = std::fabs(hf), at = std::fabs(mod);
      hfEnv_ = ah > hfEnv_ ? ah + (hfEnv_ - ah) * hfA : ah + (hfEnv_ - ah) * hfR;
      totEnv_ = at > totEnv_ ? at + (totEnv_ - at) * hfA : at + (totEnv_ - at) * hfR;
      double u = std::clamp((hfEnv_ / (totEnv_ + 1e-6) - 0.3) / 0.35, 0.0, 1.0) * (totEnv_ > 1e-4 ? 1.0 : 0.0);
      unv_ += (u - unv_) * 0.002;
      // Contour: level of the carrier (or modulator), optionally gated by consonants.
      double ac = std::fabs(cCar ? car : mod);
      carEnv_ = ac > carEnv_ ? ac + (carEnv_ - ac) * contA : ac + (carEnv_ - ac) * contR;
      cont_ = std::min(1.0, carEnv_ * 3.0) * (cCons ? unv_ : 1.0);
      // Carrier noise (high-passed) feeds the filter bank.
      double nzs = noise_.next();
      if (noiseHpOn) nzs = noiseHp_.process(nzs);
      double carIn = car + nzs * noiseG * (cNoise ? cont_ : 1.0);
      // Resonator banks, band-parallel (J-30): structure-of-arrays stages the
      // compiler vectorizes across bands; per band the arithmetic is exactly
      // Bq::process's, so the output is unchanged.
      alignas(16) double cb[kVxMaxBands];
      alignas(16) double mb[kVxMaxUnison][kVxMaxBands];
      for (int b = 0; b < nb; ++b) cb[b] = carIn;
      for (int s = 0; s < ord; ++s) runBank(syn_[s], cb, nb);
      for (int v = 0; v < uni; ++v) {
        for (int b = 0; b < nb; ++b) mb[v][b] = mod;
        for (int s = 0; s < ord; ++s) runBank(ana_[v][s], mb[v], nb);
      }
      double wl = 0, wr = 0, pl = 0, pr = 0;
      for (int b = 0; b < nb; ++b) {
        Band& B = b_[b];
        const double c = cb[b];
        double e = 0, m0 = 0;
        for (int v = 0; v < uni; ++v) {
          const double m = mb[v][b];
          if (v == 0) m0 = m;
          double& env = B.env[v];
          if (!freeze) {
            double x = rms ? m * m : std::fabs(m);
            if (x > env) {
              env = follow(env, x, B.att, ca);
              B.hold[v] = B.holdN;
            } else if (B.hold[v] > 0) {
              --B.hold[v];
            } else {
              env = follow(env, x, B.rel, cr);
            }
          }
          double ev = rms ? std::sqrt(std::max(0.0, env)) : env;
          ev = denoise ? std::max(0.0, ev - B.floor) : (ev < B.floor ? 0.0 : ev);
          if (B.satMix > 0) ev += B.satMix * (mp.at(VmSatCurve, std::min(1.0, ev)) - ev);
          ev = std::max(0.0, ev + B.offset);
          double y = c * ev * B.gain * invU;
          wl += y * B.uL[v];
          wr += y * B.uR[v];
          e += ev;
        }
        double ps = m0 * B.pass;
        pl += ps * B.uL[0];
        pr += ps * B.uR[0];
        B.lvl = std::max(B.lvl * 0.9995, std::fabs(c * e * B.gain * invU));
        B.mlvl = std::max(B.mlvl * 0.9995, e * invU);
      }
      // Pass-throughs.
      double mp1 = modHpOn ? modHp_.process(mod) : mod;
      double cp1 = carLpOn ? carLp_.process(car) : car;
      double mpass = mp1 * modPass * (cMod ? cont_ : 1.0);
      double ol, orr;
      if (listen == 1) {
        ol = orr = mod;
      } else if (listen == 2) {
        ol = orr = car;
      } else {
        ol = (wl + pl) * wet + mpass + cp1 * carPass;
        orr = (wr + pr) * wet + mpass + cp1 * carPass;
      }
      if (enh > 0 && !skipEnhance) enhance(ol, orr, enh, eA, eR);
      L[i] = blk::flush(static_cast<float>(ol));
      R[i] = blk::flush(static_cast<float>(orr));
    }
    for (int b = 0; b < nb; ++b) {
      for (auto& e : b_[b].env)
        if (std::fabs(e) < 1e-15) e = 0;
    }
    if (hfEnv_ < 1e-15) hfEnv_ = 0;
    if (totEnv_ < 1e-15) totEnv_ = 0;
    if (carEnv_ < 1e-15) carEnv_ = 0;
    if (enhEnv_ < 1e-15) enhEnv_ = 0;
  }

  void publish(Telemetry& tel) const {
    tel.count.store(nb_, std::memory_order_relaxed);
    for (int b = 0; b < nb_; ++b) {
      tel.a[b].store(blk::toDb(b_[b].mlvl * 1.57), std::memory_order_relaxed);
      tel.b[b].store(blk::toDb(b_[b].lvl * 1.57), std::memory_order_relaxed);
    }
    tel.v[TvUnvoiced].store(static_cast<float>(unv_), std::memory_order_relaxed);
  }
  int bands() const { return nb_; }
  double bandHz(int b) const { return b_[static_cast<size_t>(b)].f; }

 private:
  struct Band {
    blk::Bq ana[kVxMaxUnison][kVxMaxOrder], syn[kVxMaxOrder];
    double env[kVxMaxUnison] = {}, cenv = 0, lvl = 0, mlvl = 0;
    int hold[kVxMaxUnison] = {};
    int holdN = 0;
    double att = 0, rel = 0;  // follower coefficients
    double gain = 1, floor = 0, offset = 0, pass = 0, satMix = 0, f = 1000;
    double uL[kVxMaxUnison] = {}, uR[kVxMaxUnison] = {};
  };

  // One-pole step towards x; curve c > 1 runs the step in the x^(1/c) domain.
  static double follow(double env, double x, double coef, double c) {
    if (c <= 1.0001) return x + (env - x) * coef;
    double ue = std::pow(env, 1.0 / c), ux = std::pow(x, 1.0 / c);
    double un = ux + (ue - ux) * coef;
    return std::pow(std::max(0.0, un), c);
  }

  double synth(int tone, int nk, int voices, const double (*dts)[4]) {
    double s = 0;
    if (tone == 8) return noise_.next() * 0.7;
    for (int k = 0; k < nk; ++k)
      for (int v = 0; v < voices; ++v) {
        double dt = dts[k][v];
        int slot = (k * 4 + v) * 3;
        auto saw = [&](int o, double d) {
          double& ph = osc_[slot + o];
          double y = 2.0 * ph - 1.0 - dsp::polyBlep(ph, d);
          ph += d;
          if (ph >= 1.0) ph -= 1.0;
          return y;
        };
        auto pulse = [&](int o, double width) {
          double& ph = osc_[slot + o];
          double y = (ph < width ? 1.0 : -1.0) + dsp::polyBlep(ph, dt) - dsp::polyBlep(std::fmod(ph + 1.0 - width, 1.0), dt);
          ph += dt;
          if (ph >= 1.0) ph -= 1.0;
          return y - (2.0 * width - 1.0);
        };
        switch (tone) {
          case 0: s += saw(0, dt); break;
          case 1: s += pulse(0, 0.5); break;
          case 2: s += pulse(0, 0.18); break;
          case 3: {
            double& ph = osc_[slot];
            s += 1.0 - 4.0 * std::fabs(ph - 0.5);
            ph += dt;
            if (ph >= 1.0) ph -= 1.0;
            break;
          }
          case 4: s += (saw(0, dt) + saw(1, dt * 1.0058) + saw(2, dt * 0.9942)) * 0.6; break;
          case 5: s += (saw(0, dt * 1.003) + saw(1, dt * 0.997) + 0.5 * saw(2, dt * 2.0)) * 0.55; break;
          case 6: {
            double& ph = osc_[slot];
            double w = 2.0 * blk::kPi * ph;
            s += std::sin(w) + 0.6 * std::sin(2 * w) + 0.4 * std::sin(3 * w) + 0.25 * std::sin(4 * w);
            ph += dt;
            if (ph >= 1.0) ph -= 1.0;
            break;
          }
          case 7: s += 0.7 * saw(0, dt) + 0.7 * pulse(1, 0.08); break;
          default: break;
        }
      }
    return s;
  }

  void enhance(double& l, double& r, double amt, double eA, double eR) {
    double a = std::max(std::fabs(l), std::fabs(r));
    enhEnv_ = a > enhEnv_ ? a + (enhEnv_ - a) * eA : a + (enhEnv_ - a) * eR;
    const double thr = 0.25, ratio = 1.0 + 4.0 * amt;
    double g = enhEnv_ > thr ? std::pow(thr / enhEnv_, 1.0 - 1.0 / ratio) : 1.0;
    double mk = 1.0 + amt * 0.6;
    double hl = enhHp_[0].process(l), hr = enhHp_[1].process(r);
    l = l * g * mk + amt * 0.3 * std::tanh(3.0 * hl);
    r = r * g * mk + amt * 0.3 * std::tanh(3.0 * hr);
  }

  // Noise-equivalent bandwidth of one band's resonator cascade relative to
  // f * width * ln 2 (constant-Q; measured on a 1 kHz prototype).
  double enbwRatio(double flat, double width) const {
    // White-noise power gain (impulse energy) of a prototype at sr / 8.
    width = std::clamp(width, 1e-3, 16.0);
    double off[kVxMaxOrder], ws[kVxMaxOrder];
    vxStagger(ord_, flat, width, off, ws);
    blk::Bq q[kVxMaxOrder];
    const double fsr = 48000.0, f0 = fsr / 8.0;
    for (int s = 0; s < ord_; ++s) q[s].bandpass(fsr, f0 * std::pow(2.0, off[s]), 1.0 / (2.0 * std::sinh(std::log(2.0) / 2.0 * ws[s])));
    double e = 0;
    for (int i = 0; i < 200000; ++i) {
      double x = i == 0 ? 1.0 : 0.0;
      for (int s = 0; s < ord_; ++s) x = q[s].process(x);
      e += x * x;
      if (i > 2000 && x * x < 1e-18 && (i & 255) == 0 && e > 0) {
        double tail = 0;
        for (int s = 0; s < ord_; ++s) tail += std::fabs(q[s].z1) + std::fabs(q[s].z2);
        if (tail < 1e-9) break;
      }
    }
    return e * (fsr / 2.0) / (f0 * width * std::log(2.0));
  }

  // Band x position (0..1) whose centre frequency is hz (distribution curve).
  double posOf(const VocMapsAux& mp, double hz) const {
    double lo = 0, hi = 1;
    bool inc = vxScaleHz(mp.at(VmDist, 1)) >= vxScaleHz(mp.at(VmDist, 0));
    for (int it = 0; it < 30; ++it) {
      double mid = 0.5 * (lo + hi);
      double f = vxScaleHz(mp.at(VmDist, mid));
      if ((f < hz) == inc)
        lo = mid;
      else
        hi = mid;
    }
    return 0.5 * (lo + hi);
  }

  void design(const Params& p, const VocMapsAux& mp, double sr) {
    float key[16] = {static_cast<float>(sr),     p[VxBands],     p[VxOrder],       p[VxFlatness], p[VxWidth],      p[VxModWidth],
                     p[VcFormant],               p[VxUnison],    p[VxUnisonShift], p[VxUnisonPan], p[VxHoldMs],    p[VxAttackMs],
                     p[VxReleaseMs],             p[VxHoldMode],  p[VxMinTimes],    static_cast<float>(mp.hash & 0xFFFFFF)};
    if (sr == sr_ && std::memcmp(key, key_, sizeof key) == 0 && &mp == lastMaps_) return;
    const bool topo = sr != sr_ || key[1] != key_[1] || key[2] != key_[2] || key[7] != key_[7];
    std::memcpy(key_, key, sizeof key);
    lastMaps_ = &mp;
    if (sr != sr_) {
      sr_ = sr;
      reset();
      sr_ = sr;
      std::memcpy(key_, key, sizeof key);
    } else if (topo) {
      for (auto& b : b_) {
        for (auto& u : b.ana)
          for (auto& f : u) f.clear();
        for (auto& f : b.syn) f.clear();
      }
      clearBanks();
    }
    nb_ = std::clamp(static_cast<int>(std::lround(p[VxBands])), 5, kVxMaxBands);
    ord_ = std::clamp(p.choice(VxOrder) + 1, 1, kVxMaxOrder);
    uni_ = std::clamp(static_cast<int>(std::lround(p[VxUnison])), 1, kVxMaxUnison);
    const double flat = std::clamp(static_cast<double>(p[VxFlatness]), 0.0, 1.0);
    const double bw = 0.5 * std::clamp(static_cast<double>(p[VxWidth]), 0.008, 4.0);
    const double modW = std::clamp(static_cast<double>(p[VxModWidth]), 0.25, 4.0);
    const double shiftOct = -p[VcFormant] / 12.0;
    const bool rms = p.choice(VxHoldMode) == 1, minT = p.on(VxMinTimes);
    double fs[kVxMaxBands], pa[kVxMaxBands], ps[kVxMaxBands], wsum = 0;
    for (int b = 0; b < nb_; ++b) fs[b] = vxScaleHz(mp.at(VmDist, static_cast<double>(b) / (nb_ - 1)));
    for (int b = 0; b < nb_; ++b) {
      Band& B = b_[b];
      const double x = static_cast<double>(b) / (nb_ - 1);
      const double f = fs[b];
      B.f = f;
      double dl = b > 0 ? std::fabs(std::log2(f / fs[b - 1])) : -1, dr = b + 1 < nb_ ? std::fabs(std::log2(fs[b + 1] / f)) : -1;
      double spacing = dl < 0 ? dr : (dr < 0 ? dl : 0.5 * (dl + dr));
      // Analysis (modulator) frequency and the curve positions that follow it.
      const double pitchOct = -2.0 * (mp.at(VmPitch, x) - 0.5);
      const double fa0 = f * std::pow(2.0, shiftOct + pitchOct);
      const double xf = std::fabs(shiftOct + pitchOct) > 1e-9 ? posOf(mp, fa0) : x;
      auto mapAt = [&](int m) { return mp.at(m, mp.pf[m] ? xf : x); };
      double w = std::clamp(spacing * bw * vxMapWidth(mapAt(VmWidth)), 1.0 / 60.0, 8.0);
      auto bank = [&](blk::Bq* fl, double fc, double width) {
        double off[kVxMaxOrder], ws[kVxMaxOrder];
        vxStagger(ord_, flat, width, off, ws);
        for (int s = 0; s < ord_; ++s) {
          double q = 1.0 / (2.0 * std::sinh(std::log(2.0) / 2.0 * ws[s]));
          fl[s].bandpass(sr, std::min(fc * std::pow(2.0, off[s]), 0.4 * sr), q);
        }
      };
      bank(B.syn, f, 2.0 * w);  // w is a half width (octaves)
      const double shift = std::clamp(static_cast<double>(p[VxUnisonShift]), 0.0, 1.0);
      const double upan = std::clamp(static_cast<double>(p[VxUnisonPan]), -1.0, 1.0);
      const double bpan = 2.0 * mapAt(VmPan) - 1.0;
      // J-42: the reference's unison detune table (octaves × shift).
      static const double kT[5][5] = {{0, 0, 0, 0, 0}, {-1, 1, 0, 0, 0}, {-1, 0, 1, 0, 0}, {-1, 1.0 / 3, -1.0 / 3, 1, 0}, {-1, 0.5, 0, -0.5, 1}};
      for (int v = 0; v < uni_; ++v) {
        const double s = uni_ > 1 ? kT[std::min(uni_ - 1, 4)][v] : 0.0;
        bank(B.ana[v], fa0 * std::pow(2.0, s * shift), 2.0 * w * modW);
        double ps = uni_ > 1 ? upan * ((static_cast<double>((b + v) % uni_) / (uni_ - 1)) * 2.0 - 1.0) : 0.0;
        double gl, gr;
        dsp::panGains(std::clamp(bpan + ps, -1.0, 1.0), gl, gr);
        B.uL[v] = gl;
        B.uR[v] = gr;
      }
      B.gain = vxMapGain(mapAt(VmGain));
      // Noise-equivalent share of the spectrum of the two resonator banks (for the level normalisation).
      ps[b] = 2.0 * w / 10.0;  // share of a 10-octave pink spectrum
      pa[b] = 2.0 * w * modW / 10.0;
      wsum += 2.0 * w;
      B.floor = vxMapNoise(mapAt(VmNoise));
      B.offset = vxMapOffset(mapAt(VmOffset));
      B.pass = 0.5 * vxMapGain(mapAt(VmPass));
      B.satMix = std::clamp(mapAt(VmSatMix), 0.0, 1.0);
      // Envelope times (ms) with the per-band multipliers and minimum times.
      double hold = p[VxHoldMs] * vxMapTime(mapAt(VmHold)), att = p[VxAttackMs] * vxMapTime(mapAt(VmAttack)),
             rel = p[VxReleaseMs] * vxMapTime(mapAt(VmRelease));
      if (minT) {
        double k = rms ? 2.0 : 1.0;
        double minAr = 1000.0 / std::clamp(f * std::pow(2.0, -w - 3.0), 40.0, 16000.0) * k;
        double minHold = 1000.0 / std::clamp(f, 40.0, 800.0) * k;
        att = std::max(att, minAr);
        rel = std::max(rel, minAr);
        hold = std::max(hold, minHold);
      }
      B.att = att <= 0 ? 0.0 : std::exp(-1.0 / std::max(1e-3, att * 0.001 * sr));
      B.rel = rel <= 0 ? 0.0 : std::exp(-1.0 / std::max(1e-3, rel * 0.001 * sr));
      B.holdN = static_cast<int>(hold * 0.001 * sr);
    }
    // Level normalisation (Gridline): pink (equal energy per octave)
    // modulator and carrier give about the same output level whatever the
    // band count, widths and order; RMS hold mode x1.5 (reference).
    const double wm = wsum / nb_, ra = enbwRatio(flat, wm * modW), rs = enbwRatio(flat, wm);
    double acc = 0;
    for (int b = 0; b < nb_; ++b) acc += pa[b] * ra * ps[b] * rs;
    // Staggered (flat) cascades overlap their neighbours, which sum
    // coherently: measured correction per order at full flatness.
    static const double kFlatCorr[kVxMaxOrder + 1] = {1.0, 1.0, 1.3, 0.25, 0.075};
    const double norm = 2.4 / std::sqrt(std::max(1e-12, acc)) * (rms ? 1.5 : 1.0) * std::pow(kFlatCorr[ord_], flat * flat);
    for (int b = 0; b < nb_; ++b) b_[b].gain *= norm;
    double mhp = std::clamp(static_cast<double>(p[VxModHp]), 40.0, 15000.0), clp = std::clamp(static_cast<double>(p[VxCarLp]), 40.0, 15000.0),
           nhp = std::clamp(static_cast<double>(p[VxNoiseHp]), 40.0, 15000.0);
    modHp_.highpass(sr, mhp, 0.7071);
    carLp_.lowpass(sr, clp, 0.7071);
    noiseHp_.highpass(sr, nhp, 0.7071);
    sib_.highpass(sr, 4500.0, 0.7071);
    enhHp_[0].highpass(sr, 3000.0, 0.7071);
    enhHp_[1].highpass(sr, 3000.0, 0.7071);
    syncBanks();
  }

  // One stage of every band's resonator (structure of arrays; see process()).
  struct BankSoA {
    alignas(16) double b0[kVxMaxBands] = {}, b1[kVxMaxBands] = {}, b2[kVxMaxBands] = {}, a1[kVxMaxBands] = {}, a2[kVxMaxBands] = {};
    alignas(16) double z1[kVxMaxBands] = {}, z2[kVxMaxBands] = {};
  };
  static void runBank(BankSoA& k, double* x, int n) {
    for (int b = 0; b < n; ++b) {  // Bq::process, band by band
      const double in = x[b];
      const double y = k.b0[b] * in + k.z1[b];
      k.z1[b] = k.b1[b] * in - k.a1[b] * y + k.z2[b];
      k.z2[b] = k.b2[b] * in - k.a2[b] * y;
      x[b] = y;
    }
  }
  static void loadStage(BankSoA& k, int b, const blk::Bq& q) {
    k.b0[b] = q.b0, k.b1[b] = q.b1, k.b2[b] = q.b2, k.a1[b] = q.a1, k.a2[b] = q.a2;
  }
  void syncBanks() {  // coefficients of the designed bands → the band-parallel banks
    for (int b = 0; b < kVxMaxBands; ++b) {
      for (int s = 0; s < kVxMaxOrder; ++s) {
        loadStage(syn_[s], b, b_[static_cast<size_t>(b)].syn[s]);
        for (int v = 0; v < kVxMaxUnison; ++v) loadStage(ana_[v][s], b, b_[static_cast<size_t>(b)].ana[v][s]);
      }
    }
  }
  void clearBanks() {
    for (auto& k : syn_) std::fill(std::begin(k.z1), std::end(k.z1), 0.0), std::fill(std::begin(k.z2), std::end(k.z2), 0.0);
    for (auto& u : ana_)
      for (auto& k : u) std::fill(std::begin(k.z1), std::end(k.z1), 0.0), std::fill(std::begin(k.z2), std::end(k.z2), 0.0);
  }
  BankSoA syn_[kVxMaxOrder];
  BankSoA ana_[kVxMaxUnison][kVxMaxOrder];

  std::array<Band, kVxMaxBands> b_{};
  int nb_ = 47, ord_ = 2, uni_ = 1;
  double sr_ = 0;
  float key_[16] = {};
  const VocMapsAux* lastMaps_ = nullptr;
  blk::Bq modHp_, carLp_, noiseHp_, sib_, enhHp_[2];
  double osc_[16 * 4 * 3] = {};
  dsp::Noise noise_;
  double hfEnv_ = 0, totEnv_ = 0, unv_ = 0, cont_ = 0, carEnv_ = 0, enhEnv_ = 0;
};

class VocoderFx final : public SpectralFx {
 public:
  VocoderFx() {
    info_ = findPlugin("gl.vocoder");
    sgState_ = defaultPluginState("gl.enhance");
    sgFx_ = createEffect("gl.enhance");
    reset();
  }
  void setSideIo(const SideIo& io) override {
    scL_ = io.scL;
    scR_ = io.scR;
  }
  void reset() override {
    for (auto& b : b_) {
      for (auto& f : b.ana) f.clear();
      for (auto& f : b.syn) f.clear();
      b.env = b.cenv = 0;
    }
    hfEnv_ = totEnv_ = unv_ = 0;
    hp_.clear();
    for (auto& o : osc_) o = dsp::Osc{};
    noise_.seed(0x2545F491u);
    tick_ = 0;
    sr_ = 0;
    ref_.reset();
  }
  void processAux(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) override {
    Params p{info_, &state};
    if (p.choice(VxModel) == 1) {
      const auto* maps = dynamic_cast<const VocMapsAux*>(aux.custom.get());
      const bool sg = maps && !maps->sge.empty() && sgState_.params.size() >= maps->sge.size();
      ref_.skipEnhance = sg;
      ref_.process(p, maps, ctx, scL_, scR_, L, R, n);
      ref_.publish(tel);
      if (sg && p[VxEnhance] > 0) {  // J-42: the enhancer stage on the mastering reference engine
        if (maps != sgFrom_) {
          std::copy(maps->sge.begin(), maps->sge.end(), sgState_.params.begin());
          sgFrom_ = maps;
        }
        sgState_.params[0] = p[VxEnhance];  // EnAmount: the program's band mix
        sgFx_->process(sgState_, ctx, L, R, n);
      }
      return;
    }
    process(state, ctx, L, R, n);
  }

  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override {
    Params p{info_, &state};
    if (p.choice(VxModel) == 1) {  // reference model without prepared curves: default curves
      ref_.process(p, nullptr, ctx, scL_, scR_, L, R, n);
      ref_.publish(tel);
      return;
    }
    const double sr = ctx.sampleRate;
    design(p, sr);
    const int nb = nb_;
    const int ord = p.choice(VcOrder) + 1;
    const double ca = coefFor(sr, p[VcAttack]), cr = coefFor(sr, p[VcRelease]);
    const double cca = coefFor(sr, 4.0), ccr = coefFor(sr, 60.0);
    const double flatten = std::clamp(static_cast<double>(p[VcFlatten]), 0.0, 1.0);
    const double makeup = 3.6 * std::pow(6.0, 1.0 - flatten) * (1.0 + 0.5 * flatten);
    const bool hold = p.on(VcHold), invert = p.on(VcInvert), swap = p.on(VcSwap);
    const int carrier = p.choice(VcCarrier), listen = p.choice(VcListen);
    const bool scMod = p.choice(VcModSource) == 1 && scL_ && scR_;
    const double sib = p[VcSibilance];
    const double wet = blk::dbGain(p[VcWetLevel]), modPass = blk::dbGain(p[VcModLevel]), carPass = blk::dbGain(p[VcCarLevel]);
    // Built-in carrier chord.
    static const int chords[8][4] = {{0, -99, -99, -99}, {0, 12, -99, -99}, {0, 7, -99, -99}, {0, 4, 7, -99},
                                     {0, 3, 7, -99},     {0, 5, 7, -99},    {0, 4, 7, 11},    {0, 3, 7, 10}};
    const int chord = std::clamp(p.choice(VcChord), 0, 7);
    double dt[4] = {0, 0, 0, 0};
    int voices = 0;
    for (int k = 0; k < 4; ++k)
      if (chords[chord][k] > -99) dt[voices++] = dsp::noteToHz(p[VcRoot] + chords[chord][k]) / sr;
    const double voiceGain = 0.5 / std::sqrt(static_cast<double>(std::max(1, voices)));
    const double hfA = coefFor(sr, 2.0), hfR = coefFor(sr, 25.0);

    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      double mod, car;
      if (scMod && carrier == 0) {  // modulator from the sidechain, carrier = the track's input
        mod = 0.5 * (static_cast<double>(scL_[i]) + scR_[i]);
        car = 0.5 * (l + r);
      } else if (scMod) {
        mod = 0.5 * (static_cast<double>(scL_[i]) + scR_[i]);
        car = 0;
        double s = 0;
        for (int v = 0; v < voices; ++v) {
          switch (carrier) {
            case 1: s += osc_[v].next(dsp::Shape::Saw, dt[v], noise_); break;
            case 2: s += osc_[v].next(dsp::Shape::Square, dt[v], noise_); break;
            case 4: s += osc_[v].next(dsp::Shape::Saw, dt[v], noise_) + 0.35 * noise_.next(); break;
            default: break;
          }
        }
        car = carrier == 3 ? noise_.next() * 0.5 : s * voiceGain;
      } else if (carrier == 0) {
        mod = swap ? r : l;
        car = swap ? l : r;
      } else {
        mod = 0.5 * (l + r);
        double s = 0;
        for (int v = 0; v < voices; ++v) {
          switch (carrier) {
            case 1: s += osc_[v].next(dsp::Shape::Saw, dt[v], noise_); break;
            case 2: s += osc_[v].next(dsp::Shape::Square, dt[v], noise_); break;
            case 4: s += osc_[v].next(dsp::Shape::Saw, dt[v], noise_) + 0.35 * noise_.next(); break;
            default: break;
          }
        }
        car = carrier == 3 ? noise_.next() * 0.5 : s * voiceGain;
      }
      // Unvoiced detector: share of high-frequency energy in the modulator.
      double hf = hp_.process(mod);
      double ah = std::fabs(hf), at = std::fabs(mod);
      hfEnv_ = ah > hfEnv_ ? ah + (hfEnv_ - ah) * hfA : ah + (hfEnv_ - ah) * hfR;
      totEnv_ = at > totEnv_ ? at + (totEnv_ - at) * hfA : at + (totEnv_ - at) * hfR;
      double ratio = hfEnv_ / (totEnv_ + 1e-6);
      double u = std::clamp((ratio - 0.3) / 0.35, 0.0, 1.0) * (totEnv_ > 1e-4 ? 1.0 : 0.0);
      unv_ += (u - unv_) * 0.002;
      double carIn = car + noise_.next() * sib * unv_ * 0.4;

      // Envelope followers on the analysis bands.
      if ((tick_++ & 15) == 0)
        for (int b = 0; b < nb; ++b) b_[b].cg = std::min(1000.0, std::pow(b_[b].cenv + 1e-4, -flatten));
      for (int b = 0; b < nb; ++b) {
        Band& B = b_[b];
        double m = mod;
        for (int s = 0; s < ord; ++s) m = B.ana[s].process(m);
        if (!hold) {
          double a = std::fabs(m);
          B.env = a > B.env ? a + (B.env - a) * ca : a + (B.env - a) * cr;
        }
      }
      double wl = 0, wr = 0;
      for (int b = 0; b < nb; ++b) {
        Band& B = b_[b];
        double c = carIn;
        for (int s = 0; s < ord; ++s) c = B.syn[s].process(c);
        double a = std::fabs(c);
        B.cenv = a > B.cenv ? a + (B.cenv - a) * cca : a + (B.cenv - a) * ccr;
        const Band& src = b_[invert ? nb - 1 - b : b];
        double y = B.active ? c * src.env * B.cg * B.contour : 0.0;
        wl += y * B.gL;
        wr += y * B.gR;
      }
      wl *= makeup;
      wr *= makeup;
      // Sibilants: let the modulator's top end through on unvoiced sounds.
      double sh = hf * sib * unv_ * 0.6;
      wl += sh;
      wr += sh;
      double ol, orr;
      if (listen == 1) {
        ol = orr = mod;
      } else if (listen == 2) {
        ol = orr = car;
      } else {
        ol = wl * wet + mod * modPass + car * carPass;
        orr = wr * wet + mod * modPass + car * carPass;
      }
      L[i] = blk::flush(static_cast<float>(ol));
      R[i] = blk::flush(static_cast<float>(orr));
    }
    for (int b = 0; b < nb; ++b) {
      if (std::fabs(b_[b].env) < 1e-15) b_[b].env = 0;
      if (std::fabs(b_[b].cenv) < 1e-15) b_[b].cenv = 0;
    }
    if (hfEnv_ < 1e-15) hfEnv_ = 0;
    if (totEnv_ < 1e-15) totEnv_ = 0;
    // Telemetry: modulator band levels and output band levels.
    tel.count.store(nb, std::memory_order_relaxed);
    for (int b = 0; b < nb; ++b) {
      const Band& B = b_[b];
      const Band& src = b_[invert ? nb - 1 - b : b];
      tel.a[b].store(blk::toDb(B.env * 1.57), std::memory_order_relaxed);
      tel.b[b].store(blk::toDb(B.active ? src.env * B.cenv * B.cg * B.contour * makeup * 1.57 * wet : 0.0), std::memory_order_relaxed);
    }
    tel.v[TvUnvoiced].store(static_cast<float>(unv_), std::memory_order_relaxed);
  }

 private:
  struct Band {
    blk::Bq ana[3], syn[3];
    double env = 0, cenv = 0, cg = 1;
    double gL = 1, gR = 1, contour = 1;
    bool active = true;
  };

  void design(const Params& p, double sr) {
    float key[8] = {static_cast<float>(sr), p[VcBands], p[VcLowFreq], p[VcHighFreq], p[VcSpacing], p[VcFormant], p[VcBandwidth], p[VcOrder]};
    if (sr != sr_ || std::memcmp(key, key_, sizeof key) != 0) {
      std::memcpy(key_, key, sizeof key);
      if (sr != sr_) reset();
      sr_ = sr;
      nb_ = vocBandCount(p[VcBands]);
      const int ord = p.choice(VcOrder) + 1;
      const double lo = p[VcLowFreq], hi = std::max(static_cast<double>(p[VcHighFreq]), lo * 1.1);
      const double fshift = std::pow(2.0, -p[VcFormant] / 12.0);
      // Per-stage widening so cascades keep the requested -3 dB width.
      const double stageWiden = ord == 1 ? 1.0 : (ord == 2 ? 1.554 : 1.961);
      for (int b = 0; b < nb_; ++b) {
        double f = vocBandFreq(b, nb_, lo, hi, p[VcSpacing]);
        double fn = vocBandFreq(std::min(b + 1, nb_ - 1), nb_, lo, hi, p[VcSpacing]);
        double fp = vocBandFreq(std::max(b - 1, 0), nb_, lo, hi, p[VcSpacing]);
        double spacingOct = nb_ > 1 ? std::log2(std::max(fn, f * 1.0001) / std::max(fp, 1.0)) / (b == 0 || b == nb_ - 1 ? 1.0 : 2.0) : 1.0;
        double bwOct = std::clamp(spacingOct * p[VcBandwidth] * stageWiden, 0.01, 4.0);
        double q = 1.0 / (2.0 * std::sinh(std::log(2.0) / 2.0 * bwOct));
        Band& B = b_[b];
        double fa = f * fshift;
        B.active = f < sr * 0.45;
        for (int s = 0; s < 3; ++s) {
          B.ana[s].bandpass(sr, std::min(fa, sr * 0.45), q);
          B.syn[s].bandpass(sr, std::min(f, sr * 0.45), q);
        }
      }
      hp_.highpass(sr, 4500.0, 0.7071);
    }
    // Cheap per-block values: contour and band panning.
    float pts[kVocContour];
    for (int k = 0; k < kVocContour; ++k) pts[k] = p[VcContour0 + k];
    const double spread = std::clamp(static_cast<double>(p[VcSpread]), 0.0, 1.0);
    for (int b = 0; b < nb_; ++b) {
      Band& B = b_[b];
      double t = nb_ > 1 ? static_cast<double>(b) / (nb_ - 1) : 0.5;
      B.contour = std::pow(10.0, vocContourDb(pts, t) / 20.0);
      if (b & 1) {
        B.gL = 1.0 - spread;
        B.gR = 1.0;
      } else {
        B.gL = 1.0;
        B.gR = 1.0 - spread;
      }
    }
  }

  const PluginInfo* info_ = nullptr;
  const float* scL_ = nullptr;  // sidechain input (VcModSource)
  const float* scR_ = nullptr;
  std::array<Band, kVocMaxBands> b_{};
  int nb_ = 32;
  double sr_ = 0;
  float key_[8] = {};
  blk::Bq hp_;
  double hfEnv_ = 0, totEnv_ = 0, unv_ = 0;
  dsp::Osc osc_[4];
  dsp::Noise noise_;
  uint32_t tick_ = 0;
  VocRef ref_;
  PluginState sgState_;
  std::unique_ptr<Effect> sgFx_;
  const VocMapsAux* sgFrom_ = nullptr;
};

// ============================================================ Pitch shifter
// Delay-line shifter: two taps sweep a window with complementary sin^2 gains.
struct DelayShifter {
  blk::Ring buf;
  int64_t w = 0;
  double phase = 0;
  void init(size_t n) { buf.init(n); }
  void clear() {
    buf.clear();
    phase = 0;
  }
  float process(float x, double ratio, double win) {
    buf.at(w) = x;
    phase += (1.0 - ratio) / win;
    phase -= std::floor(phase);
    double p2 = phase + 0.5;
    p2 -= std::floor(p2);
    double d1 = 2.0 + phase * win, d2 = 2.0 + p2 * win;
    double g1 = std::sin(kPi * phase), g2 = std::sin(kPi * p2);
    float y = static_cast<float>(buf.cubic(static_cast<double>(w) - d1) * g1 * g1 + buf.cubic(static_cast<double>(w) - d2) * g2 * g2);
    ++w;
    return y;
  }
};

// Phase-vocoder shifter with optional spectral-envelope (formant) correction.
class PhaseVocoder {
 public:
  static constexpr int N = 2048, H = 256, Half = N / 2;
  PhaseVocoder() : fft_(N) {
    in_.assign(N, 0.f);
    acc_.assign(2 * N, 0.f);
    re_.assign(N, 0.f);
    im_.assign(N, 0.f);
    win_.resize(N);
    for (int i = 0; i < N; ++i) win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / N));
    for (auto* v : {&mag_, &freq_, &lastPh_, &ph_, &outPh_, &env_, &tmp_}) v->assign(Half + 1, 0.f);
    peaks_.assign(Half + 1, 0);
  }
  void clear() {
    std::fill(in_.begin(), in_.end(), 0.f);
    std::fill(acc_.begin(), acc_.end(), 0.f);
    std::fill(lastPh_.begin(), lastPh_.end(), 0.f);
    std::fill(outPh_.begin(), outPh_.end(), 0.f);
    t_ = 0;
  }
  // ratio: pitch factor; formant: target envelope factor (1 = keep), keep: apply envelope correction.
  float process(float x, double ratio, double formant, bool correct) {
    in_[static_cast<size_t>(t_ % N)] = x;
    size_t ri = static_cast<size_t>(t_ % (2 * N));
    float y = acc_[ri];
    acc_[ri] = 0;
    ++t_;
    if (t_ % H == 0) frame(ratio, formant, correct);
    return y;
  }

 private:
  void frame(double ratio, double formant, bool correct) {
    for (int i = 0; i < N; ++i) {
      re_[static_cast<size_t>(i)] = in_[static_cast<size_t>((t_ + i) % N)] * win_[static_cast<size_t>(i)];  // t_-N+i mod N
      im_[static_cast<size_t>(i)] = 0;
    }
    fft_.run(re_.data(), im_.data(), false);
    const double expect = 2.0 * kPi * H / N;
    float maxMag = 0;
    for (int k = 0; k <= Half; ++k) {
      size_t K = static_cast<size_t>(k);
      double a = re_[K], b = im_[K];
      double m = std::sqrt(a * a + b * b), ph = std::atan2(b, a);
      double d = ph - lastPh_[K] - k * expect;
      lastPh_[K] = static_cast<float>(ph);
      d -= 2.0 * kPi * std::floor(d / (2.0 * kPi) + 0.5);
      mag_[K] = static_cast<float>(m);
      ph_[K] = static_cast<float>(ph);
      freq_[K] = static_cast<float>(k + d / expect);  // true frequency in bins
      maxMag = std::max(maxMag, mag_[K]);
    }
    if (correct) envelope();
    // Peaks and their regions (bounded by the lowest bin between neighbours).
    int np = 0;
    const float floor = maxMag * 1e-6f + 1e-12f;
    for (int k = 1; k < Half; ++k) {
      size_t K = static_cast<size_t>(k);
      if (mag_[K] > floor && mag_[K] > mag_[K - 1] && mag_[K] >= mag_[K + 1]) peaks_[static_cast<size_t>(np++)] = k;
    }
    std::fill(re_.begin(), re_.end(), 0.f);
    std::fill(im_.begin(), im_.end(), 0.f);
    // Rigid phase locking: every region moves with its peak, keeping the
    // lobe's shape and internal phase relations.
    int lo = 0;
    for (int q = 0; q < np; ++q) {
      int pk = peaks_[static_cast<size_t>(q)];
      int hi = Half;
      if (q + 1 < np) {
        int nx = peaks_[static_cast<size_t>(q + 1)];
        hi = pk;
        for (int k = pk; k <= nx; ++k)
          if (mag_[static_cast<size_t>(k)] < mag_[static_cast<size_t>(hi)]) hi = k;
        if (hi == nx) hi = nx - 1;
      }
      double wTrue = freq_[static_cast<size_t>(pk)], wOut = wTrue * ratio;
      int shift = static_cast<int>(std::lround(wOut - wTrue));
      int dest = pk + shift;
      if (dest > 0 && dest < Half) {
        double phOut = outPh_[static_cast<size_t>(dest)] + expect * wOut;
        double rot = phOut - ph_[static_cast<size_t>(pk)];
        for (int k = lo; k <= hi; ++k) {
          int j = k + shift;
          if (j < 0 || j > Half) continue;
          double m = mag_[static_cast<size_t>(k)];
          if (correct) {
            double have = sampleEnv(j / ratio), want = sampleEnv(j / formant);
            m *= std::min(want / std::max(have, 1e-9), 8.0);
          }
          double a = ph_[static_cast<size_t>(k)] + rot;
          re_[static_cast<size_t>(j)] += static_cast<float>(m * std::cos(a));
          im_[static_cast<size_t>(j)] += static_cast<float>(m * std::sin(a));
        }
      }
      lo = hi + 1;
    }
    for (int j = 0; j <= Half; ++j) {
      size_t J = static_cast<size_t>(j);
      outPh_[J] = std::atan2(im_[J], re_[J]);
    }
    for (int j = 1; j < Half; ++j) {
      re_[static_cast<size_t>(N - j)] = re_[static_cast<size_t>(j)];
      im_[static_cast<size_t>(N - j)] = -im_[static_cast<size_t>(j)];
    }
    fft_.run(re_.data(), im_.data(), true);
    // Hann analysis * Hann synthesis at 8x overlap sums to 3.
    const float scale = 1.0f / (N * 3.0f);
    for (int i = 0; i < N; ++i)
      acc_[static_cast<size_t>((t_ + i) % (2 * N))] += re_[static_cast<size_t>(i)] * win_[static_cast<size_t>(i)] * scale;
  }
  // Spectral envelope: running max over +-6 bins, then a +-6 bin average.
  void envelope() {
    const int W = 6;
    for (int k = 0; k <= Half; ++k) {
      float m = 0;
      for (int d = -W; d <= W; ++d) {
        int j = std::clamp(k + d, 0, Half);
        m = std::max(m, mag_[static_cast<size_t>(j)]);
      }
      tmp_[static_cast<size_t>(k)] = m;
    }
    float run = 0;
    int cnt = 0;
    for (int k = -W; k <= Half + W; ++k) {
      int add = k + W, rem = k - W - 1;
      if (add <= Half) {
        run += tmp_[static_cast<size_t>(add)];
        ++cnt;
      }
      if (rem >= 0) {
        run -= tmp_[static_cast<size_t>(rem)];
        --cnt;
      }
      if (k >= 0 && k <= Half) env_[static_cast<size_t>(k)] = cnt > 0 ? std::max(run / cnt, 0.0f) + 1e-9f : 1e-9f;
    }
  }
  double sampleEnv(double pos) const {
    if (pos >= Half) return env_[Half];
    if (pos <= 0) return env_[0];
    int i = static_cast<int>(pos);
    double f = pos - i;
    return env_[static_cast<size_t>(i)] * (1 - f) + env_[static_cast<size_t>(i + 1)] * f;
  }

  blk::Fft fft_;
  std::vector<float> in_, acc_, re_, im_, win_;
  std::vector<float> mag_, freq_, lastPh_, ph_, outPh_, env_, tmp_;
  std::vector<int> peaks_;
  int64_t t_ = 0;
};

class PitchFx final : public SpectralFx {
 public:
  PitchFx() {
    info_ = findPlugin("gl.pitch");
    for (auto& d : fast_) d.init(1 << 15);
    for (auto& d : delay_) d.init(1 << 18);
    for (auto& d : dry_) d.init(1 << 15);
    reset();
  }
  void reset() override {
    for (auto& d : fast_) d.clear();
    for (auto& d : delay_) d.clear();
    for (auto& d : dry_) d.clear();
    pv_[0].clear();
    pv_[1].clear();
    dw_ = 0;
    fb_[0] = fb_[1] = 0;
    mode_ = -1;
  }
  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override {
    Params p{info_, &state};
    const double sr = ctx.sampleRate;
    int mode = p.choice(PsMode);
    const int engine = std::clamp(p.choice(PsEngine), 0, 2);
    if (engine != kPsClassic) mode = 10 + engine * 2 + std::clamp(p.choice(PsDetector), 0, 1);
    if (mode != mode_ || sr != sr_) {
      reset();
      mode_ = mode;
      sr_ = sr;
      if (engine != kPsClassic) configureRef(sr, engine, p.choice(PsDetector));
    }
    if (engine != kPsClassic) {
      processRef(p, ctx, engine, L, R, n);
      return;
    }
    const double semis = p[PsPitch] + p[PsFine] / 100.0;
    const double det = p[PsDetune] / 1200.0;
    const double ratio[2] = {std::pow(2.0, semis / 12.0 - det), std::pow(2.0, semis / 12.0 + det)};
    const bool keep = p.on(PsPreserve);
    const double fShift = std::pow(2.0, p[PsFormant] / 12.0);
    // Envelope target: with "keep formants" the envelope stays put (then moves by the formant control);
    // without it the envelope follows the pitch, plus the formant control.
    const bool correct = keep || std::fabs(p[PsFormant]) > 0.01f;
    const double win = grainSamples(p, sr);
    const double delayS = std::min(p[PsDelay] * 0.001 * sr, static_cast<double>(delay_[0].size() - 8));
    const bool useDelay = delayS >= 1.0;
    const double fb = useDelay ? std::clamp(static_cast<double>(p[PsFeedback]), 0.0, 0.95) : 0.0;
    const double out = dsp::dbToGain(p[PsOutput]);
    const double mix = std::clamp(static_cast<double>(p[PsMix]), 0.0, 1.0);
    const bool blend = mix < 0.9999;
    // Dry path delayed by the shifter's latency so the blend does not comb.
    const double lat =
        std::min(mode == 0 ? win * 0.5 : static_cast<double>(PhaseVocoder::N), static_cast<double>(dry_[0].size() - 8));
    float* io[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      for (int c = 0; c < 2; ++c) {
        const float dryIn = io[c][i];
        float x = dryIn + static_cast<float>(fb_[c] * fb);
        float s;
        if (mode == 0) {
          s = fast_[c].process(x, ratio[c], win);
        } else {
          double formant = (keep ? 1.0 : ratio[c]) * fShift;
          s = pv_[c].process(x, ratio[c], formant, correct);
        }
        float y = s;
        if (useDelay) {
          delay_[c].at(dw_) = s;
          float d = delay_[c].linear(static_cast<double>(dw_) - delayS);
          fb_[c] = d;
          y = s + d;
        } else {
          fb_[c] = 0;
        }
        if (blend) {
          dry_[c].at(dw_) = dryIn;
          y = static_cast<float>(mix * y + (1.0 - mix) * dry_[c].linear(static_cast<double>(dw_) - lat));
        }
        io[c][i] = blk::flush(static_cast<float>(y * out));
      }
      ++dw_;
    }
    tel.v[TvLatency].store(static_cast<float>(latency(mode, win)), std::memory_order_relaxed);
  }
  int latencyFor(const PluginState& state, double sampleRate) const override {
    Params p{info_, &state};
    const int engine = std::clamp(p.choice(PsEngine), 0, 2);
    if (engine == kPsVoice) {  // PSOLA: 3 longest periods + 8 behind the input (see PsolaShifter)
      const double fmin = p.choice(PsDetector) == 0 ? 80.0 : 50.0;
      return 3 * (static_cast<int>(std::ceil(sampleRate / fmin)) + 2) + 8;
    }
    if (engine == kPsMusic) return 0;
    return latency(p.choice(PsMode), grainSamples(p, sampleRate));
  }

 private:
  // ---- J-1 reference engines: Voice (PSOLA) / Music (grains) + delay router.
  void configureRef(double sr, int engine, int detector) {
    const double fmin = detector == 0 ? 80.0 : 50.0;  // detector quality → analysis window
    det_.configure(sr, fmin);
    const int tmax = static_cast<int>(std::ceil(sr / fmin)) + 2;
    for (auto& s : ps_) s.configure(tmax);
    cloud_.clear();
    for (auto& t : tone_) {
      t.clear();
      t.design(sr);
    }
    dSm_ = -1;
    lat_ = engine == kPsVoice ? ps_[0].latency() : 0;
  }
  void processRef(const Params& p, const ProcessContext& ctx, int engine, float* L, float* R, int n) {
    const double sr = ctx.sampleRate;
    const double st = p[PsPitch] + p[PsFine] / 100.0, det = p[PsDetune] / 1200.0;
    const double ratio[2] = {std::pow(2.0, st / 12.0 - det), std::pow(2.0, st / 12.0 + det)};
    const double fSt = psFormantSt(st, p[PsFormant], std::clamp(static_cast<double>(p[PsCorrection]), 0.0, 1.0));
    const double g = std::pow(2.0, fSt / 12.0);
    const int pmode = std::clamp(p.choice(PsPitchMode), 0, 3);
    const int ana = std::clamp(p.choice(PsAnalysis), 0, 2);
    const double len = std::clamp(static_cast<double>(p[PsDuration]), 20.0, 150.0) * 0.001 * sr;
    const double dens = std::clamp(static_cast<double>(p[PsDensity]), 1.0, 5.0);
    const double jit = std::clamp(static_cast<double>(p[PsJitter]), 0.0, 1.0), rnd = std::clamp(static_cast<double>(p[PsRand]), 0.0, 1.0);
    const int dmode = std::clamp(p.choice(PsDelayMode), 0, 3);
    double dTarget = p.on(PsDelaySync) ? std::clamp(static_cast<double>(p[PsDelaySteps]), 0.0, 16.0) * 15.0 * sr / std::max(10.0, ctx.tempo)
                                       : std::clamp(static_cast<double>(p[PsDelay]), 0.0, 1000.0) * 0.001 * sr;
    dTarget = std::clamp(dTarget, 1.0, static_cast<double>(delay_[0].size() - 8));
    if (dSm_ < 0) dSm_ = dTarget;
    const double dA = std::exp(-1.0 / (0.05 * sr));
    const double fb = std::clamp(static_cast<double>(p[PsFeedback]), 0.0, 1.0);
    const double out = dsp::dbToGain(p[PsOutput]);
    const double mix = std::clamp(static_cast<double>(p[PsMix]), 0.0, 1.0);
    const double lat = std::min(static_cast<double>(lat_), static_cast<double>(dry_[0].size() - 8));
    blk::PsVoice v[2];
    for (int c = 0; c < 2; ++c) {
      v[c].ratio = ratio[c];
      v[c].absHop = pmode > 0 ? sr / (ratio[c] * kPsAbsHz[pmode - 1]) : 0.0;
      v[c].g = g;
      v[c].gain = 1.0;
    }
    for (int i = 0; i < n; ++i) {
      const float x[2] = {L[i], R[i]};
      dSm_ = dTarget + (dSm_ - dTarget) * dA;
      double e[2] = {0, 0}, in[2] = {x[0], x[1]};
      if (dmode != PsDlyOff) {
        for (int c = 0; c < 2; ++c) e[c] = delay_[c].linear(static_cast<double>(dw_) - dSm_);
        if (dmode == PsDlyPost) {
          for (int c = 0; c < 2; ++c) in[c] = x[c] + fb * tone_[c].process(e[c]);
        } else if (dmode == PsDlyPre) {
          for (int c = 0; c < 2; ++c) {
            delay_[c].at(dw_) = static_cast<float>(x[c] + fb * tone_[c].process(e[c]));
            in[c] = x[c] + e[c];
          }
        } else {
          for (int c = 0; c < 2; ++c) delay_[c].at(dw_) = static_cast<float>(x[c] + fb * tone_[c].process(e[c]));
        }
      }
      float s[2];
      if (engine == kPsVoice) {
        const float a = ana == 0 ? static_cast<float>(in[0]) : (ana == 1 ? static_cast<float>(in[1]) : static_cast<float>(0.5 * (in[0] + in[1])));
        det_.push(a);
        const bool voiced = det_.voiced();
        for (int c = 0; c < 2; ++c) s[c] = ps_[c].process(static_cast<float>(in[c]), a, det_.period(), voiced, &v[c], 1);
      } else {
        cloud_.process(static_cast<float>(in[0]), static_cast<float>(in[1]), s[0], s[1], ratio[0], len, dens, jit, rnd);
        if (det > 0) {  // stereo detune: the right channel's grains run at their own ratio
          float d0, d1;
          cloud2_.process(static_cast<float>(in[0]), static_cast<float>(in[1]), d0, d1, ratio[1], len, dens, jit, rnd);
          s[1] = d1;
        }
      }
      for (int c = 0; c < 2; ++c) {
        double y = s[c];
        if (dmode == PsDlyPost) delay_[c].at(dw_) = s[c];
        if (dmode == PsDlyPost || dmode == PsDlyNoShift) y += e[c];
        dry_[c].at(dw_) = x[c];
        double dry = lat > 0 ? dry_[c].linear(static_cast<double>(dw_) - lat) : x[c];
        y = mix * y + (1.0 - mix) * dry;
        (c ? R : L)[i] = blk::flush(static_cast<float>(y * out));
      }
      ++dw_;
    }
    tel.v[TvLatency].store(static_cast<float>(lat_), std::memory_order_relaxed);
    double hz = engine == kPsVoice ? det_.hz() : 0.0;
    tel.v[TvDetected].store(hz > 0 ? static_cast<float>(69.0 + 12.0 * std::log2(hz / 440.0)) : 0.f, std::memory_order_relaxed);
  }

  static double grainSamples(const Params& p, double sr) { return std::clamp(static_cast<double>(p[PsGrain]), 10.0, 120.0) * 0.001 * sr; }
  // Fast mode: at unison the shifter reads the input 2 + win/2 samples back
  // (the mean of its sweeping taps otherwise); phase vocoder: one frame (N).
  static int latency(int mode, double win) {
    return mode == 0 ? static_cast<int>(std::lround(2.0 + win * 0.5)) : PhaseVocoder::N;
  }
  const PluginInfo* info_ = nullptr;
  DelayShifter fast_[2];
  PhaseVocoder pv_[2];
  blk::Ring delay_[2];
  blk::Ring dry_[2];
  int64_t dw_ = 0;
  double fb_[2] = {0, 0};
  int mode_ = -1;
  double sr_ = 0;
  blk::AmdfDetector det_;
  blk::PsolaCh ps_[2];
  blk::GrainCloud cloud_, cloud2_;
  blk::FbTone tone_[2];
  double dSm_ = -1;
  int lat_ = 0;
};

// ========================================================== Pitch corrector
// YIN pitch detector on a decimated mono signal feeding a streaming TD-PSOLA
// resynthesis with up to four voices (lead + three harmonies).
// J-1 reference pitch corrector: per-channel filtered-AMDF detection, the
// reference glide law (no hysteresis), scale or MIDI-note targets with
// input-key bypass, harmonize voices from the MIDI port bus, and per channel
// either the period-jump shifter or (formant mode) TD-PSOLA, four voices each.
class PitcherRef {
 public:
  static constexpr int kV = 4;
  PitcherRef() {
    for (auto& d : dry_) d.init(1 << 16);
  }
  void reset() {
    sr_ = 0;
    range_ = -1;
  }
  void process(const Params& p, const ProcessContext& ctx, float* L, float* R, int n, Telemetry& tel) {
    const double sr = ctx.sampleRate;
    const int range = std::clamp(p.choice(PcRange6), 0, 5);
    if (sr != sr_ || range != range_) configure(sr, range);
    float pv[PcCount];
    for (int k = 0; k < PcCount; ++k) pv[k] = p[k];
    const uint16_t mask = pitcherMask(pv);
    const double a4 = std::clamp(static_cast<double>(p[PcReference]), 400.0, 480.0);
    const double alpha = pitcherGlideAlpha(p[PcCorrSpeed], sr);
    const double amount = std::clamp(static_cast<double>(p[PcAmount]), 0.0, 1.0);
    const double fine = std::clamp(static_cast<double>(p[PcFine]), -100.0, 100.0) / 100.0;
    const double stereo = std::clamp(static_cast<double>(p[PcStereo]), 0.0, 1.0);
    const int refCh = std::clamp(p.choice(PcRefChannel), 0, 2);
    const bool formant = p.on(PcPreserve);
    const double gender = std::pow(2.0, std::clamp(static_cast<double>(p[PcFormant]), -12.0, 12.0) / 12.0);
    const bool midiT = p.on(PcMidiTargets), oct = p.on(PcMidiOctaves), harm = p.on(PcHarmonize);
    const bool hmix = p.on(PcHarmMix), hvel = p.on(PcHarmVel), diatonic = p.on(PcDiatonic);
    const bool bypassAll = p.on(PcBypassAll);
    uint16_t kbypass = 0;
    for (int k = 0; k < 12; ++k)
      if (p.on(PcKeyBypass0 + k)) kbypass = static_cast<uint16_t>(kbypass | (1 << k));
    // Notes held on the MIDI port bus (-1 = every port).
    uint8_t held[128] = {};
    if ((midiT || harm) && ctx.ports) {
      const int port = static_cast<int>(std::lround(p[PcMidiPort]));
      for (int q = 0; q < MidiPortBus::kPorts; ++q) {
        if (port >= 0 ? q != port : !ctx.ports->used[static_cast<size_t>(q)].load(std::memory_order_relaxed)) continue;
        for (int k = 0; k < 128; ++k) held[k] = std::max(held[k], ctx.ports->get(q, k));
      }
    }
    // Allowed target notes: the scale over every octave, or the held notes.
    uint16_t heldPc = 0;
    for (int k = 0; k < 128; ++k)
      if (held[k]) heldPc = static_cast<uint16_t>(heldPc | (1 << (k % 12)));
    for (int k = 0; k < 128; ++k) allowed_[k] = midiT ? (oct ? held[k] > 0 : ((heldPc >> (k % 12)) & 1) != 0) : ((mask >> (k % 12)) & 1) != 0;
    // Harmony notes: up to four held notes, ascending (pitch classes once unless Octaves).
    int hn[kV] = {0, 0, 0, 0}, nh = 0;
    double hg[kV] = {0, 0, 0, 0};
    if (harm) {
      uint16_t used = 0;
      for (int k = 0; k < 128 && nh < kV; ++k) {
        if (!held[k]) continue;
        if (!oct) {
          if ((used >> (k % 12)) & 1) continue;
          used = static_cast<uint16_t>(used | (1 << (k % 12)));
        }
        double v = held[k] / 127.0;
        hg[nh] = hvel ? 2.0 * v * v : 1.0;
        hn[nh++] = k;
      }
    }
    // Per-voice level and pan (non-harmonize: lead + the three interval voices).
    double lvl[kV], pan[kV];
    int interval[kV] = {0, 0, 0, 0};
    lvl[0] = std::clamp(static_cast<double>(p[PcLeadLevel]), 0.0, 1.0);
    pan[0] = p[PcLeadPan];
    for (int k = 1; k < kV; ++k) {
      interval[k] = static_cast<int>(std::lround(p[PcVoice1 + (k - 1) * 3]));
      lvl[k] = std::clamp(static_cast<double>(p[PcVoice1Level + (k - 1) * 3]), 0.0, 1.0);
      pan[k] = p[PcVoice1Pan + (k - 1) * 3];
    }
    if (harm)
      for (int k = 0; k < kV; ++k) pan[k] = p[PcHarmPan1 + k];
    double pg[kV][2];
    for (int k = 0; k < kV; ++k) pitcherPanGains(std::clamp(pan[k], -1.0, 1.0), stereo, pg[k][0], pg[k][1]);
    const double gk = 1.0 - std::pow(1.0 - 0.007, 44100.0 / sr);
    const bool useDet[2] = {refCh != 2, refCh != 0};
    const int lat = formant ? ps_[0].latency() : 0;

    for (int i = 0; i < n; ++i) {
      const float x[2] = {L[i], R[i]};
      for (int d = 0; d < 2; ++d) {
        if (!useDet[d]) continue;
        if (det_[d].push(x[d])) update(d, a4, amount, midiT, kbypass, bypassAll);
        if (!det_[d].voiced()) sh_[d] = 0;
        else sh_[d] += (corr_[d] - sh_[d]) * alpha;
      }
      for (int c = 0; c < 2; ++c) {
        const int d = refCh == 1 ? c : (refCh == 0 ? 0 : 1);
        const bool voiced = det_[d].voiced();
        const double P = voiced ? det_[d].period() : 512.0;
        double want[kV], g[kV];
        for (int k = 0; k < kV; ++k) {
          want[k] = vs_[c][k];
          g[k] = 0;
        }
        if (!harm) {
          for (int k = 0; k < kV; ++k) {
            if (lvl[k] < 0.001) continue;
            int semis = interval[k];
            if (k > 0 && diatonic && voiced && target_[d] >= 0) semis = nearestInMask(target_[d] + interval[k], mask) - target_[d];
            want[k] = sh_[d] + fine + semis;
            g[k] = lvl[k] * pg[k][c];
          }
        } else {
          for (int k = 0; k < kV; ++k) {
            if (k < nh) {
              if (!voiced) continue;
              double dist = hn[k] - detMidi_[d];
              if (!oct) dist -= 12.0 * std::floor(dist / 12.0 + 0.5);
              want[k] = dist + fine;
              g[k] = hg[k] * pg[k][c];
            } else if (k == 0 && nh == 0 && !hmix) {
              want[k] = fine;  // replace mode, nothing held: the input passes
              g[k] = pg[0][c];
            }
          }
        }
        dry_[c].at(w_) = x[c];
        double y = 0;
        blk::PsVoice v[kV];
        for (int k = 0; k < kV; ++k) {
          want[k] = std::clamp(want[k], -12.0, 12.0);
          vs_[c][k] = harm ? vs_[c][k] + (want[k] - vs_[c][k]) * alpha : want[k];
          gs_[c][k] += (g[k] - gs_[c][k]) * gk;
          if (gs_[c][k] < 1e-6) gs_[c][k] = 0;
          v[k].ratio = std::exp2(vs_[c][k] / 12.0);
          v[k].g = gender;
          v[k].gain = gs_[c][k];
        }
        if (formant) {
          y = ps_[c].process(x[c], x[c], P, voiced, v, kV);
        } else {
          js_[c].push(x[c]);
          for (int k = 0; k < kV; ++k)
            if (v[k].gain > 0) y += v[k].gain * js_[c].voice(k, v[k].ratio, P);
        }
        if (harm && hmix) y += lat > 0 ? dry_[c].at(w_ - lat) : x[c];
        (c ? R : L)[i] = blk::flush(static_cast<float>(y));
      }
      ++w_;
    }
    // Telemetry.
    const int d0 = refCh == 2 ? 1 : 0;
    const bool vo = det_[d0].voiced();
    tel.v[TvLatency].store(static_cast<float>(formant ? lat : js_[0].delay(0)), std::memory_order_relaxed);
    tel.v[TvDetected].store(vo ? static_cast<float>(detMidi_[d0]) : 0.f, std::memory_order_relaxed);
    tel.v[TvTarget].store(vo && target_[d0] >= 0 ? static_cast<float>(target_[d0]) : 0.f, std::memory_order_relaxed);
    tel.v[TvOutput].store(vo ? static_cast<float>(detMidi_[d0] + sh_[d0] + fine) : 0.f, std::memory_order_relaxed);
    tel.v[TvConfidence].store(vo ? static_cast<float>(det_[d0].confidence()) : 0.f, std::memory_order_relaxed);
    tel.v[TvPcCorrection].store(static_cast<float>(sh_[d0] + fine), std::memory_order_relaxed);
    tel.v[TvPcHarm].store(harm ? 1.f : 0.f, std::memory_order_relaxed);
    for (int k = 0; k < 128; ++k) tel.wave[static_cast<size_t>(k)].store(held[k] / 127.0f, std::memory_order_relaxed);
    for (int k = 0; k < kV; ++k) {
      float vp = 0;
      if (gs_[d0][k] > 0.01 && vo) vp = static_cast<float>(harm && k < nh ? hn[k] + fine : detMidi_[d0] + vs_[d0][k]);
      tel.wave[static_cast<size_t>(128 + k)].store(vp, std::memory_order_relaxed);
    }
  }

 private:
  void configure(double sr, int range) {
    sr_ = sr;
    range_ = range;
    const double fmin = kPcRangeHz[range];
    for (auto& d : det_) d.configure(sr, fmin, 1000.0);
    const int tmax = static_cast<int>(std::ceil(sr / fmin)) + 2;
    for (auto& s : ps_) s.configure(tmax);
    for (auto& j : js_) j.clear();
    for (auto& d : dry_) d.clear();
    for (int d = 0; d < 2; ++d) {
      sh_[d] = corr_[d] = detMidi_[d] = 0;
      target_[d] = -1;
      for (int k = 0; k < kV; ++k) vs_[d][k] = gs_[d][k] = 0;
    }
    w_ = 0;
  }
  // New detector estimate on channel d: target note and correction.
  void update(int d, double a4, double amount, bool midiT, uint16_t kbypass, bool bypassAll) {
    if (!det_[d].voiced()) {
      target_[d] = -1;
      corr_[d] = 0;
      return;
    }
    const double note = 69.0 + 12.0 * std::log2(det_[d].hz() / a4);
    detMidi_[d] = note;
    const int c = std::clamp(static_cast<int>(std::lround(note)), 0, 127);
    int lo = -1, hi = -1;
    for (int k = c; k >= 0; --k)
      if (allowed_[k]) {
        lo = k;
        break;
      }
    for (int k = c; k < 128; ++k)
      if (allowed_[k]) {
        hi = k;
        break;
      }
    int t = lo < 0 ? hi : (hi < 0 ? lo : (note - lo < hi - note ? lo : hi));  // ties → upper
    target_[d] = t;
    double corr = t >= 0 ? (t - note) * amount : 0.0;
    const int pc = ((static_cast<int>(std::lround(note)) % 12) + 12) % 12;
    if (bypassAll || (!midiT && ((kbypass >> pc) & 1))) corr = 0;
    corr_[d] = std::clamp(corr, -12.0, 12.0);
  }

  blk::AmdfDetector det_[2];
  blk::PsolaCh ps_[2];
  blk::JumpShifter js_[2];
  blk::Ring dry_[2];
  bool allowed_[128] = {};
  double sr_ = 0, sh_[2] = {0, 0}, corr_[2] = {0, 0}, detMidi_[2] = {0, 0};
  double vs_[2][kV] = {}, gs_[2][kV] = {};
  int range_ = -1, target_[2] = {-1, -1};
  int64_t w_ = 0;
};

class PitcherFx final : public SpectralFx {
 public:
  static constexpr int kVoices = 4;
  static constexpr int kW = 512;  // YIN window (decimated samples)
  PitcherFx() {
    info_ = findPlugin("gl.pitcher");
    in_.init(1 << 15);
    dec_.init(1 << 13);
    outL_.init(1 << 15);
    outR_.init(1 << 15);
    diff_.assign(2048, 0.f);
    hann_.resize(kHann + 1);
    for (int i = 0; i <= kHann; ++i) hann_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / kHann));
    reset();
  }
  void reset() override {
    in_.clear();
    dec_.clear();
    outL_.clear();
    outR_.clear();
    w_ = 0;
    dw_ = 0;
    decPhase_ = 0;
    lp_[0].clear();
    lp_[1].clear();
    period_ = 0;
    voiced_ = false;
    conf_ = 0;
    detMidi_ = 0;
    target_ = -1;
    shift_ = 0;
    markCount_ = 0;
    lastMark_ = -1;
    for (auto& s : syn_) s = -1;
    sr_ = 0;
    tmax_ = 0;
    ref_.reset();
  }

  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override {
    Params p{info_, &state};
    if (p.choice(PcModel) == 1) {  // J-1 reference model
      if (!refActive_) ref_.reset();
      refActive_ = true;
      ref_.process(p, ctx, L, R, n, tel);
      return;
    }
    refActive_ = false;
    const double sr = ctx.sampleRate;
    const int lowest = std::clamp(p.choice(PcLowest), 0, 3);
    if (sr != sr_ || lowest != lowest_) configure(sr, lowest);
    float pv[PcCount];
    for (int k = 0; k < PcCount; ++k) pv[k] = p[k];
    const uint16_t mask = pitcherMask(pv);
    const double ref = std::clamp(static_cast<double>(p[PcReference]), 400.0, 480.0);
    const double amount = std::clamp(static_cast<double>(p[PcAmount]), 0.0, 1.0);
    const double glide = coefFor(sr, std::max(0.5f, p[PcSpeed]));
    const double formant = std::pow(2.0, p[PcFormant] / 12.0);
    const bool keep = p.on(PcPreserve);
    const bool diatonic = p.on(PcDiatonic);
    double level[kVoices], panL[kVoices], panR[kVoices];
    int interval[kVoices] = {0, 0, 0, 0};
    level[0] = p[PcLeadLevel];
    double pan = p[PcLeadPan];
    dsp::panGains(pan, panL[0], panR[0]);
    for (int v = 1; v < kVoices; ++v) {
      interval[v] = static_cast<int>(std::lround(p[PcVoice1 + (v - 1) * 3]));
      level[v] = p[PcVoice1Level + (v - 1) * 3];
      dsp::panGains(p[PcVoice1Pan + (v - 1) * 3], panL[v], panR[v]);
    }

    for (int i = 0; i < n; ++i) {
      float x = 0.5f * (L[i] + R[i]);
      in_.at(w_) = x;
      ++w_;
      // Decimate for the detector.
      double y = lp_[1].process(lp_[0].process(x));
      if (++decPhase_ >= decim_) {
        decPhase_ = 0;
        dec_.at(dw_) = static_cast<float>(y);
        ++dw_;
        if (dw_ % 128 == 0 && dw_ >= kW + tauMax_ + 2) detect(ref, mask);
      }
      // Pitch correction amount (semitones), glided.
      double want = 0;
      if (voiced_ && target_ >= 0) want = (target_ - detMidi_) * amount;
      shift_ = want + (shift_ - want) * glide;
      const double T = voiced_ ? period_ : tmax_ * 0.6;
      generateMarks(T);
      // Synthesis: each voice places grains ahead of the output position.
      const int64_t c = w_ - lat_;
      const double base = std::pow(2.0, shift_ / 12.0);
      for (int v = 0; v < kVoices; ++v) {
        if (level[v] < 0.001 || markCount_ == 0) {
          syn_[v] = static_cast<double>(c + tmax_ + 2);
          continue;
        }
        double ratio = base;
        if (v > 0) {
          int semis = interval[v];
          if (diatonic && voiced_ && target_ >= 0) semis = nearestInMask(target_ + interval[v], mask) - target_;
          ratio *= std::pow(2.0, semis / 12.0);
        }
        if (syn_[v] < static_cast<double>(c)) syn_[v] = static_cast<double>(c + tmax_ + 2);
        while (syn_[v] <= static_cast<double>(c + tmax_ + 2)) {
          int m = nearestMark(syn_[v]);
          double Tsrc = marks_[m].period;
          double g = keep ? formant : formant * ratio;
          double half = std::min(Tsrc / g, static_cast<double>(tmax_));
          double hop = Tsrc / ratio;
          double norm = std::min(1.5, hop / half);
          addGrain(marks_[m].pos, syn_[v], half, g, level[v] * norm, panL[v], panR[v]);
          syn_[v] += std::max(hop, 8.0);
        }
      }
      float ol = outL_.at(c), orr = outR_.at(c);
      outL_.at(c) = 0;
      outR_.at(c) = 0;
      L[i] = blk::flush(ol);
      R[i] = blk::flush(orr);
    }
    tel.v[TvLatency].store(static_cast<float>(lat_), std::memory_order_relaxed);
    tel.v[TvDetected].store(voiced_ ? static_cast<float>(detMidi_) : 0.f, std::memory_order_relaxed);
    tel.v[TvTarget].store(voiced_ && target_ >= 0 ? static_cast<float>(target_) : 0.f, std::memory_order_relaxed);
    tel.v[TvOutput].store(voiced_ ? static_cast<float>(detMidi_ + shift_) : 0.f, std::memory_order_relaxed);
    tel.v[TvConfidence].store(static_cast<float>(conf_), std::memory_order_relaxed);
  }

 private:
  struct Mark {
    int64_t pos = 0;
    double period = 100;
  };
  static constexpr int kMarks = 64;
  static constexpr int kHann = 4096;
  static constexpr double kFmin[4] = {55, 80, 110, 170};  // lowest detectable pitch per range
  // Longest period (samples) and the resynthesis delay behind the input.
  static int64_t maxPeriod(double sr, int lowest) { return static_cast<int64_t>(std::ceil(sr / kFmin[std::clamp(lowest, 0, 3)])) + 2; }
  static int64_t latency(double sr, int lowest) { return 3 * maxPeriod(sr, lowest) + 8; }

 public:
  // Grains are placed on the output timeline lat_ samples behind the input.
  int latencyFor(const PluginState& state, double sampleRate) const override {
    Params p{info_, &state};
    if (p.choice(PcModel) == 1) {  // reference: PSOLA voices in formant mode, zero-latency jump shifter otherwise
      if (!p.on(PcPreserve)) return 0;
      const double fmin = kPcRangeHz[std::clamp(p.choice(PcRange6), 0, 5)];
      return 3 * (static_cast<int>(std::ceil(sampleRate / fmin)) + 2) + 8;
    }
    return static_cast<int>(latency(sampleRate, std::clamp(p.choice(PcLowest), 0, 3)));
  }

 private:

  void configure(double sr, int lowest) {
    reset();
    sr_ = sr;
    lowest_ = lowest;
    decim_ = std::max(1, static_cast<int>(std::lround(sr / 22050.0)));
    const double dsr = sr / decim_;
    tauMin_ = std::max(2, static_cast<int>(dsr / 1100.0));
    tauMax_ = std::min(static_cast<int>(diff_.size()) - 2, static_cast<int>(dsr / kFmin[lowest]) + 2);
    tmax_ = maxPeriod(sr, lowest);
    lat_ = latency(sr, lowest);
    double cut = 0.42 * dsr;
    lp_[0].lowpass(sr, cut, 0.54119610014619698);
    lp_[1].lowpass(sr, cut, 1.3065629648763766);
  }

  void detect(double ref, uint16_t mask) {
    // Difference function over the newest window.
    const int64_t start = dw_ - kW - tauMax_;
    double energy = 0;
    for (int j = 0; j < kW; ++j) {
      float a = dec_.at(start + j);
      energy += a * a;
    }
    double rms = std::sqrt(energy / kW);
    diff_[0] = 1;
    double run = 0;
    int best = -1;
    double bestVal = 1;
    for (int tau = 1; tau <= tauMax_; ++tau) {
      double d = 0;
      for (int j = 0; j < kW; ++j) {
        float e = dec_.at(start + j) - dec_.at(start + j + tau);
        d += e * e;
      }
      run += d;
      double cm = run > 0 ? d * tau / run : 1.0;
      diff_[static_cast<size_t>(tau)] = static_cast<float>(cm);
    }
    for (int tau = tauMin_; tau < tauMax_; ++tau) {
      if (diff_[static_cast<size_t>(tau)] < 0.15f) {
        while (tau + 1 < tauMax_ && diff_[static_cast<size_t>(tau + 1)] < diff_[static_cast<size_t>(tau)]) ++tau;
        best = tau;
        bestVal = diff_[static_cast<size_t>(tau)];
        break;
      }
    }
    if (best < 0) {
      // No dip under the threshold: accept the global minimum when it is clear enough.
      for (int tau = tauMin_; tau < tauMax_; ++tau)
        if (diff_[static_cast<size_t>(tau)] < bestVal) {
          bestVal = diff_[static_cast<size_t>(tau)];
          best = tau;
        }
      if (bestVal > 0.3) best = -1;
    }
    if (best < 0 || rms < 3e-4) {
      voiced_ = false;
      conf_ = 0;
      return;
    }
    double t = best;
    if (best > 1 && best < tauMax_) {
      double a = diff_[static_cast<size_t>(best - 1)], b = diff_[static_cast<size_t>(best)], c = diff_[static_cast<size_t>(best + 1)];
      double den = a - 2 * b + c;
      if (std::fabs(den) > 1e-12) t += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    double f0 = sr_ / (t * decim_);
    period_ = std::min(sr_ / f0, static_cast<double>(tmax_ - 2));
    detMidi_ = 69.0 + 12.0 * std::log2(f0 / ref);
    conf_ = std::clamp(1.0 - bestVal, 0.0, 1.0);
    voiced_ = true;
    // Target note with hysteresis.
    bool keepTarget = target_ >= 0 && (mask & (1 << (target_ % 12))) && std::fabs(detMidi_ - target_) < 0.62;
    if (!keepTarget) target_ = nearestInMask(detMidi_, mask);
  }

  // Analysis pitch marks spaced one period apart, snapped to local peaks.
  void generateMarks(double T) {
    if (lastMark_ < 0) lastMark_ = w_ - lat_;
    while (true) {
      double pred = static_cast<double>(lastMark_) + T;
      int64_t reach = static_cast<int64_t>(pred + T * 0.3) + 2;
      if (reach >= w_) break;
      int64_t lo = static_cast<int64_t>(pred - T * 0.25), hi = static_cast<int64_t>(pred + T * 0.25);
      int64_t bestPos = static_cast<int64_t>(pred);
      if (voiced_) {
        float bv = -1e9f;
        for (int64_t k = lo; k <= hi; ++k) {
          float vv = in_.at(k);
          if (vv > bv) {
            bv = vv;
            bestPos = k;
          }
        }
      }
      if (bestPos <= lastMark_) bestPos = lastMark_ + 1;
      Mark& m = marks_[static_cast<size_t>(markHead_)];
      m.pos = bestPos;
      m.period = T;
      markHead_ = (markHead_ + 1) % kMarks;
      markCount_ = std::min(markCount_ + 1, kMarks);
      lastMark_ = bestPos;
    }
  }

  int nearestMark(double pos) const {
    int best = 0;
    double bd = 1e300;
    for (int k = 0; k < markCount_; ++k) {
      int idx = (markHead_ - 1 - k + kMarks) % kMarks;
      double d = std::fabs(static_cast<double>(marks_[static_cast<size_t>(idx)].pos) - pos);
      if (d < bd) {
        bd = d;
        best = idx;
      }
      if (static_cast<double>(marks_[static_cast<size_t>(idx)].pos) < pos - 4.0 * tmax_) break;
    }
    return best;
  }

  void addGrain(int64_t src, double centre, double half, double g, double gain, double pl, double pr) {
    int64_t c0 = static_cast<int64_t>(std::ceil(centre - half)), c1 = static_cast<int64_t>(std::floor(centre + half));
    const double inv = 1.0 / (2.0 * half);
    for (int64_t k = c0; k <= c1; ++k) {
      double off = static_cast<double>(k) - centre;  // -half..half
      double wpos = (off + half) * inv * kHann;
      int wi = std::clamp(static_cast<int>(wpos), 0, kHann);
      double s = in_.linear(static_cast<double>(src) + off * g) * hann_[static_cast<size_t>(wi)] * gain;
      outL_.at(k) += static_cast<float>(s * pl);
      outR_.at(k) += static_cast<float>(s * pr);
    }
  }

  const PluginInfo* info_ = nullptr;
  blk::Ring in_, dec_, outL_, outR_;
  std::vector<float> diff_, hann_;
  blk::Bq lp_[2];
  int64_t w_ = 0, dw_ = 0;
  int decPhase_ = 0, decim_ = 2, tauMin_ = 20, tauMax_ = 400, lowest_ = -1;
  int64_t tmax_ = 0, lat_ = 0;
  double sr_ = 0, period_ = 0, conf_ = 0, detMidi_ = 0, shift_ = 0;
  int target_ = -1;
  bool voiced_ = false;
  std::array<Mark, kMarks> marks_{};
  int markHead_ = 0, markCount_ = 0;
  int64_t lastMark_ = -1;
  double syn_[kVoices] = {-1, -1, -1, -1};
  PitcherRef ref_;
  bool refActive_ = false;
};

// ================================================================ Scratcher
class ScratchFx final : public SpectralFx {
 public:
  ScratchFx() {
    info_ = findPlugin("gl.scratcher");
    for (auto& b : buf_) b.init(static_cast<size_t>(kMaxSeconds * 96000));
    reset();
  }
  void reset() override {
    for (auto& b : buf_) b.clear();
    w_ = 0;
    lag_ = lagPrev_ = 0;
    s1_ = s2_ = 0;
    vel_ = 1;
    gain_ = 1;
    angle_ = 0;
    xfade_ = 0;
    wasHeld_ = false;
    for (auto& d : dc_) d = 0;
    for (auto& d : dcx_) d = 0;
    chunkPeak_ = 0;
    chunkN_ = 0;
    freePhase_ = 0;
    primed_ = false;
  }
  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override {
    Params p{info_, &state};
    const double sr = ctx.sampleRate;
    if (sr != sr_) {
      reset();
      sr_ = sr;
    }
    const double minLag = 4.0;
    const double maxLag = std::min(static_cast<double>(buf_[0].size()) - 16.0, kMaxSeconds * sr - 16.0);
    const double range = std::clamp(p[ScRange] * sr, 1.0, maxLag - minLag);
    const double smooth = coefFor(sr, p[ScSmooth]);
    const int pattern = p.choice(ScPattern);
    const bool held = p.on(ScGrab) || pattern > 0;
    const bool motor = p.on(ScMotor);
    const double accel = 1.0 / std::max(1.0, p[ScStart] * sr), brake = 1.0 / std::max(1.0, p[ScStop] * sr);
    static const double rateBeats[4] = {2.0, 1.0, 0.5, 0.25};
    const double cycle = rateBeats[std::clamp(p.choice(ScRate), 0, 3)];
    const double beatsPerSample = ctx.tempo / 60.0 / sr;
    // + 1e-9 beat (J-30): block start + offset rounds differently per block size; pattern
    // gates switching exactly on the grid must not flip with it.
    double beat = ctx.playing ? ctx.tick / std::max(1, ctx.ppq) + 1e-9 : freePhase_;
    const double fader = std::clamp(static_cast<double>(p[ScFader]), 0.0, 1.0);
    const int curve = p.choice(ScCurve);
    const double vol = dsp::dbToGain(p[ScVolume]);
    double pl, pr;
    dsp::panGains(p[ScPan], pl, pr);
    const double turn = 1.8 * sr;  // one platter revolution at 33 1/3 rpm
    const size_t chunk = static_cast<size_t>(std::max(1.0, kMaxSeconds * sr / 256.0));
    const double dcA = std::exp(-2.0 * kPi * 12.0 / sr);
    if (!primed_) {
      lag_ = lagPrev_ = s1_ = s2_ = minLag + p[ScHand] * range;
      primed_ = true;
    }

    for (int i = 0; i < n; ++i) {
      buf_[0].at(w_) = L[i];
      buf_[1].at(w_) = R[i];
      double gate = 1.0;
      if (held) {
        double h = p[ScHand];
        if (pattern > 0) h = patternPos(pattern, beat / cycle, gate);
        double target = minLag + std::clamp(h, 0.0, 1.0) * range;
        s1_ = target + (s1_ - target) * smooth;
        s2_ = s1_ + (s2_ - s1_) * smooth;
        lag_ = s2_;
        vel_ = 1.0 - (lag_ - lagPrev_);
      } else {
        if (wasHeld_ && !held) xfadeTo(minLag);
        double goal = motor ? 1.0 : 0.0;
        if (vel_ < goal)
          vel_ = std::min(goal, vel_ + accel);
        else if (vel_ > goal)
          vel_ = std::max(goal, vel_ - brake);
        lag_ += 1.0 - vel_;
        // Back at full speed after a stop: return to the live position.
        if (motor && vel_ >= 1.0 && lag_ > minLag + 0.002 * sr && xfade_ <= 0) xfadeTo(minLag);
        s1_ = s2_ = lag_;
      }
      wasHeld_ = held;
      lag_ = std::clamp(lag_, minLag, maxLag);
      lagPrev_ = lag_;
      angle_ += vel_ / turn;
      // Deck output: playback speed sets the level (moving-coil cartridge).
      double g = std::min(1.0, std::fabs(vel_));
      gain_ += (g - gain_) * 0.01;
      double pos = static_cast<double>(w_) - lag_;
      double dl = buf_[0].cubic(pos), dr = buf_[1].cubic(pos);
      if (xfade_ > 0) {
        double t = xfade_ / xfadeLen_;
        double op = static_cast<double>(w_) - xfadeFrom_;
        dl = dl * (1 - t) + buf_[0].cubic(op) * t;
        dr = dr * (1 - t) + buf_[1].cubic(op) * t;
        xfade_ -= 1;
      }
      dl *= gain_;
      dr *= gain_;
      // DC blocker on the deck (slow scratches produce offsets).
      double yl = dl - dcx_[0] + dcA * dc_[0];
      dcx_[0] = dl;
      dc_[0] = yl;
      double yr = dr - dcx_[1] + dcA * dc_[1];
      dcx_[1] = dr;
      dc_[1] = yr;
      // Crossfader: 0 = live input, 1 = deck.
      double f = fader * gate;
      double gd, gl;
      if (curve == 0) {
        gd = std::sin(f * kPi * 0.5);
        gl = std::cos(f * kPi * 0.5);
      } else if (curve == 1) {
        gd = std::min(1.0, f * 6.0);
        gl = std::min(1.0, (1.0 - f) * 6.0);
      } else {
        gd = f > 0.04 ? 1.0 : 0.0;
        gl = f < 0.96 ? 1.0 : 0.0;
      }
      double ol = (L[i] * gl + yl * gd) * vol * pl;
      double orr = (R[i] * gl + yr * gd) * vol * pr;
      // Overview of the recorded input.
      chunkPeak_ = std::max(chunkPeak_, std::fabs(0.5f * (L[i] + R[i])));
      if (++chunkN_ >= chunk) {
        int wp = tel.wavePos.load(std::memory_order_relaxed);
        tel.wave[static_cast<size_t>(wp & 255)].store(chunkPeak_, std::memory_order_relaxed);
        tel.wavePos.store(wp + 1, std::memory_order_relaxed);
        chunkPeak_ = 0;
        chunkN_ = 0;
      }
      L[i] = blk::flush(static_cast<float>(ol));
      R[i] = blk::flush(static_cast<float>(orr));
      ++w_;
      beat += beatsPerSample;
    }
    if (!ctx.playing) freePhase_ = std::fmod(beat, 64.0);
    if (std::fabs(dc_[0]) < 1e-20) dc_[0] = 0;
    if (std::fabs(dc_[1]) < 1e-20) dc_[1] = 0;
    tel.v[TvAngle].store(static_cast<float>(angle_ - std::floor(angle_)), std::memory_order_relaxed);
    tel.v[TvVelocity].store(static_cast<float>(vel_), std::memory_order_relaxed);
    tel.v[TvLag].store(static_cast<float>(lag_ / sr), std::memory_order_relaxed);
    tel.v[TvHandPos].store(static_cast<float>((lag_ - minLag) / range), std::memory_order_relaxed);
    tel.v[TvLatency].store(static_cast<float>(kMaxSeconds), std::memory_order_relaxed);  // overview span (s)
    angle_ -= std::floor(angle_);
  }

 private:
  static constexpr double kMaxSeconds = 6.0;
  // Hand position (0..1) and fader gate for an auto-scratch pattern at cycle phase ph.
  static double patternPos(int pattern, double ph, double& gate) {
    ph -= std::floor(ph);
    gate = 1.0;
    switch (pattern) {
      case 1:  // baby: smooth back and forth
        return 0.5 - 0.5 * std::cos(2 * kPi * ph);
      case 2:  // forward: fast push, fader closed on the way back
        if (ph < 0.25) return 1.0 - 0.5 * (1 - std::cos(kPi * ph / 0.25));
        gate = 0.0;
        return 0.5 * (1 - std::cos(kPi * (ph - 0.25) / 0.75));
      case 3: {  // chirp: fader closes around each turnaround
        double h = 0.5 - 0.5 * std::cos(2 * kPi * ph);
        double d = std::min(std::fabs(ph - 0.5), std::min(ph, 1 - ph));
        gate = d < 0.08 ? 0.0 : 1.0;
        return h;
      }
      case 4: {  // transform: slow stroke chopped by the fader
        double h = 0.5 - 0.5 * std::cos(2 * kPi * ph);
        gate = std::fmod(ph * 8.0, 1.0) < 0.5 ? 1.0 : 0.0;
        return h;
      }
      case 5: {  // tear: back stroke in two parts
        if (ph < 0.3) return 0.5 - 0.5 * std::cos(kPi * ph / 0.3);
        if (ph < 0.5) return 1.0 - 0.45 * (1 - std::cos(kPi * (ph - 0.3) / 0.2)) * 0.5;
        if (ph < 0.6) return 0.55;
        return 0.55 * (0.5 + 0.5 * std::cos(kPi * (ph - 0.6) / 0.4));
      }
      default: return 0.0;
    }
  }
  void xfadeTo(double newLag) {
    xfadeFrom_ = lag_;
    xfadeLen_ = std::max(1.0, 0.02 * sr_);
    xfade_ = xfadeLen_;
    lag_ = lagPrev_ = s1_ = s2_ = newLag;
  }

  const PluginInfo* info_ = nullptr;
  blk::Ring buf_[2];
  int64_t w_ = 0;
  double sr_ = 0;
  double lag_ = 0, lagPrev_ = 0, s1_ = 0, s2_ = 0, vel_ = 1, gain_ = 1, angle_ = 0;
  double xfade_ = 0, xfadeLen_ = 1, xfadeFrom_ = 0;
  bool wasHeld_ = false, primed_ = false;
  double dc_[2] = {0, 0}, dcx_[2] = {0, 0};
  float chunkPeak_ = 0;
  size_t chunkN_ = 0;
  double freePhase_ = 0;
};

// ============================================================ Band splitter
// Legacy model (0.5): LR2/4/8 crossovers chosen by SpSlope.
// Reference model (J-1): one slope per crossover (6..96 dB), main and send
// level / mute per band, band sends into other mixer inserts
// (SideIo::multiL/R, PluginInfo::sendParams) and a linear-phase FFT mode.
// J-42: the reference splitter's linear-phase mode (2 or 3 bands): FFT filter objects
// (zero-phase masks: −6 dB at the split, S dB per "6 dB octave", −120 dB floor) for a
// low pass at f1 and, with 3 bands, a high pass at f2; the remaining band is the input
// delayed 3N/4 minus the others, so the bands sum to that delay. Changes cross-fade two
// filter sets over the speed time (the first design fades in from the empty set).
struct FsLin {
  using LinEngine = gl::packs::dynamics::maxref::LinEngine;
  int N = 0, len = 0, count = 0, s = 0, hp = 0, dW = 0;
  bool dirty = true;
  LinEngine A[2], B[2];
  gl::packs::dynamics::maxref::FftD fft;
  std::vector<float> hist[2], dl[2], maskA, maskB;
  void alloc(int n) {
    N = n;
    fft.init(N);
    for (int k = 0; k < 2; ++k) A[k].init(N), B[k].init(N);
    for (int c = 0; c < 2; ++c) hist[c].assign(static_cast<size_t>(N), 0.f), dl[c].assign(static_cast<size_t>(3 * N / 4), 0.f);
    maskA.assign(static_cast<size_t>(N / 2 + 1), 0.f), maskB = maskA;
    s = 0, count = len = 0, hp = dW = 0;
  }
  static float mag(float f, float fc, float S, bool hpf) {
    if (f == 0) return hpf ? 0.f : 1.f;
    float x = gl::packs::dynamics::maxref::log2a(f * (1.0f / fc));
    if (hpf) x = -x;
    using gl::packs::dynamics::maxref::pow2a;
    if (x >= 0) {
      const float d = -6.0f - S * x;
      return d >= -120.0f ? pow2a(d / 6.0f) : 0.f;
    }
    const float d = -6.0f + S * x;
    return d >= -120.0f ? 1.0f - pow2a(d / 6.0f) : 1.f;
  }
  // One stereo frame → bands (low, [mid,] high).
  void run(const float* in, int nb, double fs, int q, float f1, float f2, float S1, float S2, double xfadeSec, double (*band)[4]) {
    const int want = gl::packs::dynamics::maxref::linSize(fs, q);
    if (dirty && count >= len) {
      if (want != N) alloc(want);
      for (int i = 0; i <= N / 2; ++i) {
        const float f = static_cast<float>(i) * static_cast<float>(fs / N);
        maskA[static_cast<size_t>(i)] = mag(f, f1, S1, false);
        maskB[static_cast<size_t>(i)] = mag(f, f2, S2, true);
      }
      A[s].clear(), B[s].clear();
      A[s].design(maskA, fft), B[s].design(maskB, fft);
      count = 0, len = static_cast<int>(xfadeSec * fs);
      float tmp[2], fr[2];
      for (int k = 0; k < N; ++k) {
        const size_t at = static_cast<size_t>((hp + k) % N);
        fr[0] = hist[0][at], fr[1] = hist[1][at];
        A[s].run(fr, tmp, fft);
        if (nb == 3) B[s].run(fr, tmp, fft);
      }
      s = 1 - s;
      dirty = false;
    }
    hist[0][static_cast<size_t>(hp)] = in[0], hist[1][static_cast<size_t>(hp)] = in[1];
    hp = hp + 1 == N ? 0 : hp + 1;
    float D[2];
    for (int c = 0; c < 2; ++c) D[c] = dl[c][static_cast<size_t>(dW)], dl[c][static_cast<size_t>(dW)] = in[c];
    dW = dW + 1 == static_cast<int>(dl[0].size()) ? 0 : dW + 1;
    const int nw = 1 - s, od = s;
    double b3[2][3][2];  // [set][band][channel]
    const int sets = count < len ? 2 : 1;
    for (int k = 0; k < sets; ++k) {
      const int set = k == 0 ? nw : od;
      float lo[2], hi[2] = {0, 0};
      A[set].run(in, lo, fft);
      if (nb == 3) B[set].run(in, hi, fft);
      for (int c = 0; c < 2; ++c) {
        b3[k][0][c] = lo[c];
        if (nb == 3) b3[k][2][c] = hi[c], b3[k][1][c] = D[c] - lo[c] - hi[c];
        else b3[k][1][c] = D[c] - lo[c];
      }
    }
    const double t = sets == 2 ? static_cast<double>(count) / len : 1.0;
    for (int b = 0; b < nb; ++b)
      for (int c = 0; c < 2; ++c) band[c][b] = sets == 2 ? b3[0][b][c] * t + b3[1][b][c] * (1 - t) : b3[0][b][c];
    if (count < len) ++count;
  }
};

class SplitterFx final : public SpectralFx {
 public:
  SplitterFx() {
    info_ = findPlugin("gl.splitter");
    reset();
  }
  ~SplitterFx() override {
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
  }
  void setSideIo(const SideIo& io) override {
    for (int k = 0; k < 4; ++k) {
      sendL_[k] = io.multiL[k];
      sendR_[k] = io.multiR[k];
    }
  }
  // UI thread (every Program build): allocate the linear-phase buffers for
  // the state's FFT size; the audio thread adopts them at its next block.
  void prepareHost(const PluginState& state, double sampleRate) override {
    delete retired_.exchange(nullptr);
    Params p{info_, &state};
    if (p.choice(SpModel) != 1 || !p.on(SpLinear)) return;
    int need = splitFftSize(p.choice(SpQuality), sampleRate);
    if (need <= capUi_) return;
    capUi_ = need;
    delete pending_.exchange(new LinBank(need));
  }
  int latencyFor(const PluginState& state, double sampleRate) const override {
    Params p{info_, &state};
    if (p.choice(SpModel) != 1 || !p.on(SpLinear)) return 0;
    if (std::clamp(p.choice(SpBands), 0, 2) + 2 <= 3) return 3 * dynamics::maxref::linSize(sampleRate, p.choice(SpQuality)) / 4;  // J-42
    return splitFftSize(p.choice(SpQuality), sampleRate) / 2;
  }
  void reset() override {
    for (int c = 0; c < 2; ++c) {
      for (auto& x : xo_[c]) x.clear();
      for (auto& row : ap_[c])
        for (auto& a : row) a.clear();
      for (auto& x : rx_[c]) x.clear();
      for (auto& row : rc_[c])
        for (auto& a : row) a.clear();
    }
    for (auto& g : gs_) g = {1, 1, 1, 1};
    for (auto& pk : peak_) pk = 0;
    for (auto& s : sendS_) s = 0;
    sr_ = 0;
    refKey_[0] = -1;
    if (lin_) lin_->clear();
    linN_ = 0;
  }

  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override {
    Params p{info_, &state};
    if (p.choice(SpModel) == 1) {
      processRef(p, ctx.sampleRate, L, R, n);
      return;
    }
    latency_.store(0, std::memory_order_relaxed);
    const double sr = ctx.sampleRate;
    const int nb = std::clamp(p.choice(SpBands), 0, 2) + 2;
    const int slope = std::clamp(p.choice(SpSlope), 0, 2);
    double f[3] = {p[SpFreq1], p[SpFreq2], p[SpFreq3]};
    f[0] = std::clamp(f[0], 20.0, sr * 0.45);
    for (int k = 1; k < 3; ++k) f[k] = std::clamp(std::max(f[k], f[k - 1] * 1.01), 20.0, sr * 0.45);
    float key[6] = {static_cast<float>(sr), static_cast<float>(nb), static_cast<float>(slope), static_cast<float>(f[0]),
                    static_cast<float>(f[1]), static_cast<float>(f[2])};
    if (std::memcmp(key, key_, sizeof key) != 0) {
      if (sr != sr_ || key[1] != key_[1] || key[2] != key_[2]) {
        for (int c = 0; c < 2; ++c) {
          for (auto& x : xo_[c]) x.clear();
          for (auto& row : ap_[c])
            for (auto& a : row) a.clear();
        }
      }
      std::memcpy(key_, key, sizeof key);
      sr_ = sr;
      for (int c = 0; c < 2; ++c)
        for (int x = 0; x < nb - 1; ++x) {
          xo_[c][x].design(sr, f[x], slope);
          for (int b = 0; b < 4; ++b) ap_[c][b][x].design(sr, f[x], slope);
        }
    }
    // Per-band targets.
    bool anySolo = false;
    for (int b = 0; b < nb; ++b) anySolo = anySolo || p.on(splitParam(b, SbSolo));
    std::array<double, 4> gT[4];
    for (int b = 0; b < 4; ++b) {
      bool audible = b < nb && !p.on(splitParam(b, SbMute)) && (!anySolo || p.on(splitParam(b, SbSolo)));
      double g = audible ? blk::dbGain(p[splitParam(b, SbGain)]) : 0.0;
      double pl, pr;
      dsp::panGains(p[splitParam(b, SbPan)], pl, pr);
      double width = std::clamp(static_cast<double>(p[splitParam(b, SbWidth)]), 0.0, 2.0);
      gT[b] = {g * pl, g * pr, width, 0};
    }
    const double sm = 1.0 - std::exp(-1.0 / (0.005 * sr));
    float pk[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; ++i) {
      double band[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
      for (int c = 0; c < 2; ++c) {
        double x = c ? R[i] : L[i];
        double rest = x;
        for (int k = 0; k < nb - 1; ++k) {
          double lo, hi;
          xo_[c][k].split(rest, lo, hi);
          // Phase compensation: band k still needs the all-passes of the later crossovers.
          for (int j = k + 1; j < nb - 1; ++j) lo = ap_[c][k][j].process(lo);
          band[c][k] = lo;
          rest = hi;
        }
        band[c][nb - 1] = rest;
      }
      double ol = 0, orr = 0;
      for (int b = 0; b < nb; ++b) {
        for (int q = 0; q < 3; ++q) gs_[b][q] += (gT[b][q] - gs_[b][q]) * sm;
        double l = band[0][b], r = band[1][b];
        double m = 0.5 * (l + r), s = 0.5 * (l - r) * gs_[b][2];
        l = (m + s) * gs_[b][0];
        r = (m - s) * gs_[b][1];
        ol += l;
        orr += r;
        pk[b] = std::max(pk[b], static_cast<float>(std::max(std::fabs(l), std::fabs(r))));
      }
      L[i] = blk::flush(static_cast<float>(ol));
      R[i] = blk::flush(static_cast<float>(orr));
    }
    publish(pk, nb, n, sr);
  }
  int meters(float* out, int max) const override {
    int k = std::min(max, 4);
    for (int b = 0; b < k; ++b) out[b] = blk::toDb(peak_[b]);
    return k;
  }

 private:
  // Linear-phase buffers (allocated on the UI thread, see prepareHost).
  struct LinBank {
    explicit LinBank(int cap) : fft(cap), cap(cap) {
      auto z = [cap](std::vector<float>& v) { v.assign(static_cast<size_t>(cap), 0.0f); };
      z(inRe), z(inIm), z(wRe), z(wIm), z(tRe), z(tIm);
      for (auto& set : kRe)
        for (auto& v : set) z(v);
      for (auto& set : kIm)
        for (auto& v : set) z(v);
      for (auto& v : oRe) z(v);
      for (auto& v : oIm) z(v);
    }
    void clear() {
      for (auto* v : {&inRe, &inIm})
        std::fill(v->begin(), v->end(), 0.0f);
      for (auto& v : oRe) std::fill(v.begin(), v.end(), 0.0f);
      for (auto& v : oIm) std::fill(v.begin(), v.end(), 0.0f);
    }
    blk::FftVar fft;
    int cap;
    std::vector<float> inRe, inIm, wRe, wIm, tRe, tIm;
    std::vector<float> kRe[2][4], kIm[2][4];  // two kernel sets (crossfade on redesign)
    std::vector<float> oRe[4], oIm[4];        // band outputs of the last hop
  };

  void publish(const float* pk, int nb, int n, double sr) {
    const double fall = std::exp(-static_cast<double>(n) / (0.3 * sr));
    tel.count.store(nb, std::memory_order_relaxed);
    for (int b = 0; b < 4; ++b) {
      peak_[b] = std::max(static_cast<double>(pk[b]), peak_[b] * fall);
      tel.a[b].store(blk::toDb(peak_[b]), std::memory_order_relaxed);
    }
    tel.v[TvLatency].store(static_cast<float>(latency_.load(std::memory_order_relaxed)), std::memory_order_relaxed);
  }

  // Crossover frequencies (sorted) and slopes of the reference model.
  int refLayout(const Params& p, double sr, double* f, int* sl) const {
    const int nb = std::clamp(p.choice(SpBands), 0, 2) + 2;
    double v[3] = {p[SpFreq1], p[SpFreq2], p[SpFreq3]};
    std::sort(v, v + (nb - 1));
    for (int k = 0; k < 3; ++k) f[k] = std::clamp(v[k], 10.0, sr * 0.49);
    sl[0] = std::clamp(p.choice(SpSlopeA), 0, kSplitSlopes - 1);
    sl[1] = std::clamp(p.choice(SpSlopeB), 0, kSplitSlopes - 1);
    sl[2] = std::clamp(p.choice(SpSlopeC), 0, kSplitSlopes - 1);
    return nb;
  }

  void processRef(const Params& p, double sr, float* L, float* R, int n) {
    // Adopt linear-phase buffers prepared on the UI thread (old ones are
    // handed back for deletion there).
    if (retired_.load(std::memory_order_acquire) == nullptr)
      if (LinBank* nbk = pending_.exchange(nullptr)) {
        retired_.store(lin_.release(), std::memory_order_release);
        lin_.reset(nbk);
        linN_ = 0;
      }
    double f[3];
    int sl[3];
    const int nb = refLayout(p, sr, f, sl);
    // J-42: 2 / 3-band linear phase runs the reference's FFT filter objects.
    const bool flLin = p.on(SpLinear) && nb <= 3;
    if (flLin) {
      if (!fs_) fs_ = std::make_unique<FsLin>();
      const float key[7] = {static_cast<float>(f[0]), static_cast<float>(f[1]), static_cast<float>(sl[0]), static_cast<float>(sl[1]),
                            static_cast<float>(nb), static_cast<float>(p.choice(SpQuality)), static_cast<float>(sr)};
      if (std::memcmp(key, fsKey_, sizeof key) != 0) std::memcpy(fsKey_, key, sizeof key), fs_->dirty = true;
      latency_.store(3 * dynamics::maxref::linSize(sr, p.choice(SpQuality)) / 4, std::memory_order_relaxed);
    }
    bool linear = !flLin && p.on(SpLinear) && lin_;
    int N = linear ? std::min(splitFftSize(p.choice(SpQuality), sr), lin_->cap) : 0;
    // Gains.
    bool anySolo = false;
    for (int b = 0; b < nb; ++b) anySolo = anySolo || p.on(splitParam(b, SbSolo));
    double gT[4][3], sT[4];
    for (int b = 0; b < 4; ++b) {
      bool audible = b < nb && !p.on(splitParam(b, SbMute)) && (!anySolo || p.on(splitParam(b, SbSolo)));
      double g = audible ? splitLevelGain(p[splitParam2(b, SxMain)]) * blk::dbGain(p[splitParam(b, SbGain)]) : 0.0;
      double pl, pr;
      dsp::panGains(p[splitParam(b, SbPan)], pl, pr);
      gT[b][0] = g * pl;
      gT[b][1] = g * pr;
      gT[b][2] = std::clamp(static_cast<double>(p[splitParam(b, SbWidth)]), 0.0, 2.0);
      sT[b] = b < nb && !p.on(splitParam2(b, SxSendMute)) ? splitLevelGain(p[splitParam2(b, SxSend)]) : 0.0;
    }
    const double sm = 1.0 - std::exp(-1.0 / (0.005 * sr));
    float pk[4] = {0, 0, 0, 0};
    double band[2][4];
    if (linear) {
      if (N != linN_) {  // (re)start the block convolver
        lin_->clear();
        linN_ = N;
        fill_ = 0;
        fadeHops_ = fadeAt_ = 0;
        designKernels(0, nb, f, sl, sr);
        cur_ = 0;
      }
      latency_.store(N / 2, std::memory_order_relaxed);
    } else if (!flLin) {
      linN_ = 0;
      latency_.store(0, std::memory_order_relaxed);
      float key[8] = {static_cast<float>(sr), static_cast<float>(nb), static_cast<float>(f[0]), static_cast<float>(f[1]),
                      static_cast<float>(f[2]), static_cast<float>(sl[0]), static_cast<float>(sl[1]), static_cast<float>(sl[2])};
      if (std::memcmp(key, refKey_, sizeof key) != 0) {
        bool topo = sr != sr_ || key[1] != refKey_[1] || key[5] != refKey_[5] || key[6] != refKey_[6] || key[7] != refKey_[7];
        if (topo)
          for (int c = 0; c < 2; ++c) {
            for (auto& x : rx_[c]) x.clear();
            for (auto& row : rc_[c])
              for (auto& a : row) a.clear();
          }
        std::memcpy(refKey_, key, sizeof key);
        sr_ = sr;
        for (int c = 0; c < 2; ++c)
          for (int x = 0; x < nb - 1; ++x) {
            rx_[c][x].design(sr, f[x], sl[x]);
            for (int b = 0; b < 3; ++b) rc_[c][b][x].design(sr, f[x], sl[x]);
          }
      }
    }
    for (int i = 0; i < n; ++i) {
      if (flLin) {
        const float in[2] = {L[i], R[i]};
        fs_->run(in, nb, sr, p.choice(SpQuality), static_cast<float>(p[SpFreq1]), static_cast<float>(p[SpFreq2]),
                 static_cast<float>(kSplitSlopeDb[sl[0]]), static_cast<float>(kSplitSlopeDb[sl[1]]), 0.001 * std::clamp(static_cast<double>(p[SpLinSpeed]), 2.0, 200.0), band);
      } else if (linear) {
        LinBank& lb = *lin_;
        const int H = linN_ / 4;
        lb.inRe[static_cast<size_t>(linN_ - H + fill_)] = L[i];
        lb.inIm[static_cast<size_t>(linN_ - H + fill_)] = R[i];
        for (int b = 0; b < nb; ++b) {
          band[0][b] = lb.oRe[b][static_cast<size_t>(fill_)];
          band[1][b] = lb.oIm[b][static_cast<size_t>(fill_)];
        }
        if (++fill_ == H) {
          fill_ = 0;
          hop(nb, f, sl, sr, p);
        }
      } else {
        for (int c = 0; c < 2; ++c) {
          double rest = c ? R[i] : L[i];
          for (int k = 0; k < nb - 1; ++k) {
            double lo, hi;
            rx_[c][k].split(rest, lo, hi);
            for (int j = k + 1; j < nb - 1; ++j) lo = rc_[c][k][j].sum(lo);  // later crossovers' sum response
            band[c][k] = lo;
            rest = hi;
          }
          band[c][nb - 1] = rest;
        }
      }
      double ol = 0, orr = 0;
      for (int b = 0; b < nb; ++b) {
        for (int q = 0; q < 3; ++q) gs_[b][q] += (gT[b][q] - gs_[b][q]) * sm;
        sendS_[b] += (sT[b] - sendS_[b]) * sm;
        double l = band[0][b], r = band[1][b];
        if (sendL_[b] && sendR_[b]) {
          sendL_[b][i] += static_cast<float>(l * sendS_[b]);
          sendR_[b][i] += static_cast<float>(r * sendS_[b]);
        }
        double m = 0.5 * (l + r), s = 0.5 * (l - r) * gs_[b][2];
        l = (m + s) * gs_[b][0];
        r = (m - s) * gs_[b][1];
        ol += l;
        orr += r;
        pk[b] = std::max(pk[b], static_cast<float>(std::max(std::fabs(l), std::fabs(r))));
      }
      L[i] = blk::flush(static_cast<float>(ol));
      R[i] = blk::flush(static_cast<float>(orr));
    }
    publish(pk, nb, n, sr);
  }

  // Linear-phase kernels for kernel set `set`: zero-phase band magnitudes
  // (the IIR crossovers' closed-form responses) windowed to N/2 + 1 taps,
  // centred at N/4.
  void designKernels(int set, int nb, const double* f, const int* sl, double sr) {
    LinBank& lb = *lin_;
    const int N = linN_, half = N / 2, D = N / 4;
    double tc[3];
    for (int k = 0; k < nb - 1; ++k) tc[k] = std::tan(kPi * f[k] / sr);
    for (int b = 0; b < nb; ++b) {
      for (int k = 0; k <= half; ++k) {
        double t = std::tan(std::min(kPi * k / N, kPi * 0.5 - 1e-9));
        double m = 1.0;
        for (int x = 0; x < nb - 1 && x <= b; ++x) {
          double lp, hp;
          blk::slopeMag(sl[x], t / tc[x], lp, hp);
          m *= x == b ? lp : hp;
        }
        lb.wRe[static_cast<size_t>(k)] = static_cast<float>(m);
        lb.wIm[static_cast<size_t>(k)] = 0;
        if (k > 0 && k < half) {
          lb.wRe[static_cast<size_t>(N - k)] = static_cast<float>(m);
          lb.wIm[static_cast<size_t>(N - k)] = 0;
        }
      }
      lb.fft.run(lb.wRe.data(), lb.wIm.data(), N, true);  // zero-phase impulse (scaled by N)
      std::vector<float>& kr = lb.kRe[set][b];
      std::vector<float>& ki = lb.kIm[set][b];
      std::fill(kr.begin(), kr.begin() + N, 0.0f);
      std::fill(ki.begin(), ki.begin() + N, 0.0f);
      const double inv = 1.0 / N;
      for (int t = -D; t <= D; ++t) {
        double w = 0.5 * (1.0 + std::cos(kPi * t / (D + 1)));
        kr[static_cast<size_t>(t + D)] = static_cast<float>(lb.wRe[static_cast<size_t>((t + N) % N)] * w * inv);
      }
      lb.fft.run(kr.data(), ki.data(), N, false);
    }
    std::memcpy(designed_, f, sizeof designed_);
    std::memcpy(designedSl_, sl, sizeof designedSl_);
    designedNb_ = nb;
  }

  // One hop of the block convolver (H = N/4 new samples; overlap-save).
  void hop(int nb, const double* f, const int* sl, double sr, const Params& p) {
    LinBank& lb = *lin_;
    const int N = linN_, H = N / 4;
    // Redesign on changes (crossfaded over the speed time).
    bool changed = nb != designedNb_ || std::memcmp(sl, designedSl_, sizeof designedSl_) != 0;
    for (int k = 0; k < nb - 1; ++k) changed = changed || std::fabs(f[k] - designed_[k]) > 1e-6 * f[k];
    if (changed && fadeHops_ == 0) {
      designKernels(1 - cur_, nb, f, sl, sr);
      double ms = std::clamp(static_cast<double>(p[SpLinSpeed]), 2.0, 200.0);
      fadeHops_ = std::max(1, static_cast<int>(std::ceil(ms * 0.001 * sr / H)));
      fadeAt_ = 0;
    }
    std::memcpy(lb.wRe.data(), lb.inRe.data(), sizeof(float) * static_cast<size_t>(N));
    std::memcpy(lb.wIm.data(), lb.inIm.data(), sizeof(float) * static_cast<size_t>(N));
    lb.fft.run(lb.wRe.data(), lb.wIm.data(), N, false);
    const int sets = fadeHops_ > 0 ? 2 : 1;
    for (int b = 0; b < nb; ++b)
      for (int s = 0; s < sets; ++s) {
        int set = s == 0 ? cur_ : 1 - cur_;
        const float* kr = lb.kRe[set][b].data();
        const float* ki = lb.kIm[set][b].data();
        for (int k = 0; k < N; ++k) {
          size_t u = static_cast<size_t>(k);
          lb.tRe[u] = lb.wRe[u] * kr[k] - lb.wIm[u] * ki[k];
          lb.tIm[u] = lb.wRe[u] * ki[k] + lb.wIm[u] * kr[k];
        }
        lb.fft.run(lb.tRe.data(), lb.tIm.data(), N, true);
        const float inv = 1.0f / static_cast<float>(N);
        for (int j = 0; j < H; ++j) {
          size_t u = static_cast<size_t>(N - H + j);
          float re = lb.tRe[u] * inv, im = lb.tIm[u] * inv;
          if (s == 0) {
            lb.oRe[b][static_cast<size_t>(j)] = re;
            lb.oIm[b][static_cast<size_t>(j)] = im;
          } else {
            float t = static_cast<float>((static_cast<double>(fadeAt_) * H + j + 1) / (static_cast<double>(fadeHops_) * H));
            lb.oRe[b][static_cast<size_t>(j)] += (re - lb.oRe[b][static_cast<size_t>(j)]) * t;
            lb.oIm[b][static_cast<size_t>(j)] += (im - lb.oIm[b][static_cast<size_t>(j)]) * t;
          }
        }
      }
    if (fadeHops_ > 0 && ++fadeAt_ >= fadeHops_) {
      cur_ = 1 - cur_;
      fadeHops_ = fadeAt_ = 0;
    }
    std::memmove(lb.inRe.data(), lb.inRe.data() + H, sizeof(float) * static_cast<size_t>(N - H));
    std::memmove(lb.inIm.data(), lb.inIm.data() + H, sizeof(float) * static_cast<size_t>(N - H));
  }

  const PluginInfo* info_ = nullptr;
  // Legacy model.
  blk::LrCrossover xo_[2][3];
  blk::LrAllpass ap_[2][4][3];
  float key_[6] = {};
  // Reference model (IIR).
  blk::XoverRef rx_[2][3];
  blk::XoverRef rc_[2][3][3];  // phase compensation copies [channel][band][crossover]
  float refKey_[8] = {};
  // Reference model (linear phase).
  std::unique_ptr<FsLin> fs_;
  float fsKey_[7] = {};
  std::unique_ptr<LinBank> lin_;
  std::atomic<LinBank*> pending_{nullptr};
  std::atomic<LinBank*> retired_{nullptr};
  int capUi_ = 0;  // UI thread: largest bank requested
  int linN_ = 0, fill_ = 0, cur_ = 0, fadeHops_ = 0, fadeAt_ = 0;
  double designed_[3] = {0, 0, 0};
  int designedSl_[3] = {-1, -1, -1};
  int designedNb_ = 0;
  std::atomic<int> latency_{0};
  // Shared.
  float* sendL_[4] = {};
  float* sendR_[4] = {};
  double sendS_[4] = {0, 0, 0, 0};
  std::array<double, 4> gs_[4];
  double peak_[4] = {0, 0, 0, 0};
  double sr_ = 0;
};

// ================================================================= Spreader
class SpreadFx final : public SpectralFx {
 public:
  SpreadFx() {
    info_ = findPlugin("gl.spread");
    for (auto& d : dl_) d.init(1 << 14);
    mid_.init(1 << 14);
    hiR_.init(1 << 14);
    reset();
  }
  void reset() override {
    for (auto& d : dl_) d.clear();
    mid_.clear();
    w_ = 0;
    for (auto& b : sideHp_) b.clear();
    for (auto& b : decHp_) b.clear();
    dcx_[0] = dcx_[1] = dcy_[0] = dcy_[1] = 0;
    lfo_ = 0;
    corrLR_ = corrLL_ = corrRR_ = 0;
    mRms_ = sRms_ = 0;
    sr_ = 0;
    scope_ = 0;
    rsr_ = 0;
    preLR_ = preLL_ = preRR_ = 0;
  }
  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override {
    Params p{info_, &state};
    if (p.choice(SrModel) == 1) {  // J-1 reference model
      processRef(p, ctx, L, R, n);
      return;
    }
    const double sr = ctx.sampleRate;
    const double bassF = std::clamp(static_cast<double>(p[SrBassFreq]), 20.0, 500.0);
    if (sr != sr_ || bassF != bassF_) {
      if (sr != sr_) reset();
      sr_ = sr;
      bassF_ = bassF;
      for (auto& b : sideHp_) b.highpass(sr, bassF, std::sqrt(0.5));
      for (auto& b : decHp_) b.highpass(sr, bassF, std::sqrt(0.5));
    }
    const int mode = p.choice(SrMode);
    const double spread = std::clamp(static_cast<double>(p[SrSpread]), 0.0, 1.0);
    const double sep = 1.0 + std::clamp(static_cast<double>(p[SrSeparation]), -1.0, 1.0);
    const bool bassMono = p.on(SrBassMono), sepAfter = p.choice(SrPosition) == 1;
    const double offset = std::clamp(static_cast<double>(p[SrOffset]), -20.0, 20.0) * 0.001 * sr;
    const int inv = p.choice(SrInvert);
    const double signL = (inv == 1 || inv == 3) ? -1.0 : 1.0, signR = (inv == 2 || inv == 3) ? -1.0 : 1.0;
    double pl, pr;
    dsp::panGains(p[SrPan], pl, pr);
    const double vol = dsp::dbToGain(p[SrVolume]);
    const bool mono = p.on(SrMonoCheck), dc = p.on(SrDcBlock);
    const double dcA = std::exp(-2.0 * kPi * 5.0 / sr);
    // Complementary decorrelation taps (ms, sign).
    static const double taps[4][2] = {{4.7, 1}, {9.1, -1}, {13.9, 1}, {21.3, -1}};
    const double lfoInc = 0.31 / sr;
    const double corrA = std::exp(-1.0 / (0.25 * sr));

    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      if (dc) {
        double yl = l - dcx_[0] + dcA * dcy_[0];
        dcx_[0] = l;
        dcy_[0] = yl;
        double yr = r - dcx_[1] + dcA * dcy_[1];
        dcx_[1] = r;
        dcy_[1] = yr;
        l = yl;
        r = yr;
      }
      l *= signL;
      r *= signR;
      double m = 0.5 * (l + r), s = 0.5 * (l - r);
      if (!sepAfter) s *= sep;
      mid_.at(w_) = static_cast<float>(m);
      switch (mode) {
        case 0: {  // complementary: add a decorrelated copy of the mid to the side only (mono-safe)
          double d = 0;
          for (const auto& t : taps) d += t[1] * mid_.linear(static_cast<double>(w_) - t[0] * 0.001 * sr);
          if (bassMono) d = decHp_[1].process(decHp_[0].process(d));
          s += d * spread * 0.55;
          break;
        }
        case 2: {  // chorus spread: modulated copies of the mid, opposite in the side
          double ph = lfo_ * 2 * kPi;
          double d1 = (11.0 + 4.0 * std::sin(ph)) * 0.001 * sr, d2 = (17.0 + 5.0 * std::sin(ph * 1.37 + 1.1)) * 0.001 * sr;
          double d = mid_.cubic(static_cast<double>(w_) - d1) - mid_.cubic(static_cast<double>(w_) - d2);
          if (bassMono) d = decHp_[1].process(decHp_[0].process(d));
          s += d * spread * 0.6;
          break;
        }
        case 3: s *= 1.0 + 2.0 * spread; break;  // mid/side
        default: break;                           // haas handled below
      }
      if (sepAfter) s *= sep;
      if (bassMono) s = sideHp_[1].process(sideHp_[0].process(s));
      l = m + s;
      r = m - s;
      // Time offsets: Haas widening and the L/R offset control.
      double dL = offset < 0 ? -offset : 0, dR = offset > 0 ? offset : 0;
      if (mode == 1) dR += spread * 0.025 * sr;
      dl_[0].at(w_) = static_cast<float>(l);
      dl_[1].at(w_) = static_cast<float>(r);
      if (dL > 0) l = dl_[0].linear(static_cast<double>(w_) - dL);
      if (dR > 0) r = dl_[1].linear(static_cast<double>(w_) - dR);
      l *= vol * pl;
      r *= vol * pr;
      if (mono) l = r = 0.5 * (l + r);
      // Meters.
      corrLR_ = l * r + (corrLR_ - l * r) * corrA;
      corrLL_ = l * l + (corrLL_ - l * l) * corrA;
      corrRR_ = r * r + (corrRR_ - r * r) * corrA;
      double mm = 0.5 * (l + r), ss = 0.5 * (l - r);
      mRms_ = mm * mm + (mRms_ - mm * mm) * corrA;
      sRms_ = ss * ss + (sRms_ - ss * ss) * corrA;
      if ((++scope_ & 7) == 0) {
        int wp = tel.wavePos.load(std::memory_order_relaxed);
        tel.wave[static_cast<size_t>((wp * 2) & 255)].store(static_cast<float>(l), std::memory_order_relaxed);
        tel.wave[static_cast<size_t>((wp * 2 + 1) & 255)].store(static_cast<float>(r), std::memory_order_relaxed);
        tel.wavePos.store(wp + 1, std::memory_order_relaxed);
      }
      L[i] = blk::flush(static_cast<float>(l));
      R[i] = blk::flush(static_cast<float>(r));
      ++w_;
      lfo_ += lfoInc;
      if (lfo_ >= 1) lfo_ -= 1;
    }
    for (double* v : {&corrLR_, &corrLL_, &corrRR_, &mRms_, &sRms_, &dcy_[0], &dcy_[1]})
      if (std::fabs(*v) < 1e-25) *v = 0;
    double den = std::sqrt(corrLL_ * corrRR_);
    tel.v[TvCorrelation].store(den > 1e-12 ? static_cast<float>(corrLR_ / den) : 1.f, std::memory_order_relaxed);
    tel.v[TvWidth].store(mRms_ > 1e-12 ? static_cast<float>(std::sqrt(sRms_ / mRms_)) : 0.f, std::memory_order_relaxed);
  }

 private:
  const PluginInfo* info_ = nullptr;
  blk::Ring dl_[2], mid_;
  int64_t w_ = 0;
  blk::Bq sideHp_[2], decHp_[2];
  double dcx_[2] = {0, 0}, dcy_[2] = {0, 0};
  double lfo_ = 0, sr_ = 0, bassF_ = 0;
  double corrLR_ = 0, corrLL_ = 0, corrRR_ = 0, mRms_ = 0, sRms_ = 0;
  unsigned scope_ = 0;

  // ---- J-1 reference model: LR4 split at the low-bypass frequency (lows keep
  // their stereo, or go mono with Mono bass), complementary single-tap
  // decorrelation of the high-band mid (0..6.35 ms, 23 ms tap crossfade, 15 kHz
  // smoothing) injected into the side with opposite polarity (mono-safe),
  // separation as side gain, smoothed enable crossfade.
  void processRef(const Params& p, const ProcessContext& ctx, float* L, float* R, int n) {
    const double sr = ctx.sampleRate;
    if (sr != rsr_) {
      for (auto& d : dl_) d.clear();
      mid_.clear();
      hiR_.clear();
      w_ = 0;
      rsr_ = sr;
      xf_ = -1;
      lp15_.lowpass1(sr, 15000.0);
      lp15_.clear();
      zA_ = std::exp(-500.0 / sr);
      fadeInc_ = 1.0 / (kSpreadFadeMs * 0.001 * sr);
      curD_ = oldD_ = 0;
      fading_ = false;
      sepS_ = 1.0 + std::clamp(static_cast<double>(p[SrSeparation]), -1.0, 1.0);
      enS_ = p.on(SrEnabled) ? 1.0 : 0.0;
      dcx_[0] = dcx_[1] = dcy_[0] = dcy_[1] = 0;
    }
    const double f = std::clamp(static_cast<double>(p[SrLowFreq]), 10.0, std::min(10000.0, 0.45 * sr));
    if (f != xf_) {
      xf_ = f;
      for (auto& x : xo_) {
        x.design(sr, f, 1);
        x.clear();
      }
    }
    const bool bassMono = p.on(SrBassMono), split = p.on(SrLowBypass) || bassMono;
    const int mode = p.choice(SrMode);
    const double spread = std::clamp(static_cast<double>(p[SrSpread]), 0.0, 1.0);
    const double sepT = 1.0 + std::clamp(static_cast<double>(p[SrSeparation]), -1.0, 1.0);
    const double enT = p.on(SrEnabled) ? 1.0 : 0.0;
    const bool sepAfter = p.choice(SrPosition) == 1;
    const int dT = static_cast<int>(std::lround(spread * kSpreadMaxDelayMs * 0.001 * sr));
    const double offset = std::clamp(static_cast<double>(p[SrOffset]), -20.0, 20.0) * 0.001 * sr;
    const int inv = p.choice(SrInvert);
    const double signL = (inv == 1 || inv == 3) ? -1.0 : 1.0, signR = (inv == 2 || inv == 3) ? -1.0 : 1.0;
    double pl, pr;
    dsp::panGains(p[SrPan], pl, pr);
    const double vol = dsp::dbToGain(p[SrVolume]);
    const bool mono = p.on(SrMonoCheck), dc = p.on(SrDcBlock);
    const double dcA = std::exp(-2.0 * kPi * 5.0 / sr);
    const double haas = mode == 1 ? spread * 0.025 * sr : 0.0;
    const double lfoInc = 0.31 / sr;
    const double corrA = std::exp(-1.0 / (0.25 * sr));
    for (int i = 0; i < n; ++i) {
      const double rl = L[i], rr = R[i];
      sepS_ = sepT + (sepS_ - sepT) * zA_;
      enS_ = enT + (enS_ - enT) * zA_;
      double l = rl, r = rr;
      if (dc) {
        double yl = l - dcx_[0] + dcA * dcy_[0];
        dcx_[0] = l;
        dcy_[0] = yl;
        double yr = r - dcx_[1] + dcA * dcy_[1];
        dcx_[1] = r;
        dcy_[1] = yr;
        l = yl;
        r = yr;
      }
      l *= signL;
      r *= signR;
      double lol = 0, lor = 0, hl = l, hr = r;
      if (split) {
        xo_[0].split(l, lol, hl);
        xo_[1].split(r, lor, hr);
        if (bassMono) lol = lor = 0.5 * (lol + lor);
      }
      if (haas > 0) {
        hiR_.at(w_) = static_cast<float>(hr);
        hr = hiR_.linear(static_cast<double>(w_) - haas);
      }
      double m = 0.5 * (hl + hr), s = 0.5 * (hl - hr);
      if (!sepAfter) s *= sepS_;
      mid_.at(w_) = static_cast<float>(m);
      switch (mode) {
        case 0: {  // complementary single-tap decorrelation
          if (fading_) {
            fadeT_ += fadeInc_;
            if (fadeT_ >= 1.0) fading_ = false;
          } else if (dT != curD_) {
            oldD_ = curD_;
            curD_ = dT;
            fadeT_ = 0;
            fading_ = true;
          }
          double tap = mid_.at(w_ - curD_);
          if (fading_) tap = (1.0 - fadeT_) * mid_.at(w_ - oldD_) + fadeT_ * tap;
          s += 0.398 * lp15_.process(m - tap);
          break;
        }
        case 2: {  // chorus spread (Gridline)
          double ph = lfo_ * 2 * kPi;
          double d1 = (11.0 + 4.0 * std::sin(ph)) * 0.001 * sr, d2 = (17.0 + 5.0 * std::sin(ph * 1.37 + 1.1)) * 0.001 * sr;
          s += (mid_.cubic(static_cast<double>(w_) - d1) - mid_.cubic(static_cast<double>(w_) - d2)) * spread * 0.6;
          break;
        }
        case 3: s *= 1.0 + 2.0 * spread; break;
        default: break;
      }
      if (sepAfter) s *= sepS_;
      l = lol + m + s;
      r = lor + m - s;
      double dL = offset < 0 ? -offset : 0, dR = offset > 0 ? offset : 0;
      dl_[0].at(w_) = static_cast<float>(l);
      dl_[1].at(w_) = static_cast<float>(r);
      if (dL > 0) l = dl_[0].linear(static_cast<double>(w_) - dL);
      if (dR > 0) r = dl_[1].linear(static_cast<double>(w_) - dR);
      l *= vol * pl;
      r *= vol * pr;
      if (mono) l = r = 0.5 * (l + r);
      l = rl + (l - rl) * enS_;
      r = rr + (r - rr) * enS_;
      corrLR_ = l * r + (corrLR_ - l * r) * corrA;
      corrLL_ = l * l + (corrLL_ - l * l) * corrA;
      corrRR_ = r * r + (corrRR_ - r * r) * corrA;
      preLR_ = rl * rr + (preLR_ - rl * rr) * corrA;
      preLL_ = rl * rl + (preLL_ - rl * rl) * corrA;
      preRR_ = rr * rr + (preRR_ - rr * rr) * corrA;
      double mm = 0.5 * (l + r), ss = 0.5 * (l - r);
      mRms_ = mm * mm + (mRms_ - mm * mm) * corrA;
      sRms_ = ss * ss + (sRms_ - ss * ss) * corrA;
      if ((++scope_ & 7) == 0) {
        int wp = tel.wavePos.load(std::memory_order_relaxed);
        tel.wave[static_cast<size_t>((wp * 2) & 255)].store(static_cast<float>(l), std::memory_order_relaxed);
        tel.wave[static_cast<size_t>((wp * 2 + 1) & 255)].store(static_cast<float>(r), std::memory_order_relaxed);
        tel.a[static_cast<size_t>(wp & 127)].store(static_cast<float>(rl), std::memory_order_relaxed);
        tel.b[static_cast<size_t>(wp & 127)].store(static_cast<float>(rr), std::memory_order_relaxed);
        tel.wavePos.store(wp + 1, std::memory_order_relaxed);
      }
      L[i] = blk::flush(static_cast<float>(l));
      R[i] = blk::flush(static_cast<float>(r));
      ++w_;
      lfo_ += lfoInc;
      if (lfo_ >= 1) lfo_ -= 1;
    }
    for (double* v : {&corrLR_, &corrLL_, &corrRR_, &mRms_, &sRms_, &dcy_[0], &dcy_[1], &preLR_, &preLL_, &preRR_})
      if (std::fabs(*v) < 1e-25) *v = 0;
    double den = std::sqrt(corrLL_ * corrRR_), pden = std::sqrt(preLL_ * preRR_);
    tel.v[TvCorrelation].store(den > 1e-12 ? static_cast<float>(corrLR_ / den) : 1.f, std::memory_order_relaxed);
    tel.v[TvSrPreCorr].store(pden > 1e-12 ? static_cast<float>(preLR_ / pden) : 1.f, std::memory_order_relaxed);
    tel.v[TvWidth].store(mRms_ > 1e-12 ? static_cast<float>(std::sqrt(sRms_ / mRms_)) : 0.f, std::memory_order_relaxed);
  }
  blk::LrCrossover xo_[2];
  blk::Bq lp15_;
  blk::Ring hiR_;
  double rsr_ = 0, xf_ = -1, zA_ = 0, fadeInc_ = 0, fadeT_ = 0, sepS_ = 1, enS_ = 1;
  double preLR_ = 0, preLL_ = 0, preRR_ = 0;
  int curD_ = 0, oldD_ = 0;
  bool fading_ = false;
};

}  // namespace

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  (void)type;
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == "gl.vocoder") return std::make_unique<VocoderFx>();
  if (type == "gl.pitch") return std::make_unique<PitchFx>();
  if (type == "gl.pitcher") return std::make_unique<PitcherFx>();
  if (type == "gl.scratcher") return std::make_unique<ScratchFx>();
  if (type == "gl.splitter") return std::make_unique<SplitterFx>();
  if (type == "gl.spread") return std::make_unique<SpreadFx>();
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.vocoder") {  // per-band curves + held keys (UI thread)
    if (state.data.empty()) return true;
    VxData d;
    vxParse(state.data, d);
    auto a = std::make_shared<VocMapsAux>();
    a->build(d);
    a->hash = std::hash<std::string>{}(state.data);
    if (size_t at = state.data.find("\nsge"); at != std::string::npos) {
      const char* q = state.data.c_str() + at + 4;
      for (;;) {
        char* end = nullptr;
        const float v = std::strtof(q, &end);
        if (end == q) break;
        a->sge.push_back(v);
        q = end;
      }
    }
    aux.custom = a;
    return true;
  }
  return false;
}

}  // namespace gl::packs::spectral

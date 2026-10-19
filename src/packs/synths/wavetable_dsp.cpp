// Gridline — "synths" pack: Wavetable runtime and the built-in tables.
// Tables are generated procedurally once (original shapes: analog morph, pulse
// sweep, vowel formants, additive build-up, sync sweep, FM, resonant sweep,
// odd/even, bit-reduced, drawbar organ) and stored as band-limited mip levels
// (level L keeps the lowest 1024 >> L harmonics), so playback never aliases
// except where a warp mode deliberately adds a discontinuity.
#include <complex>
#include "synths_common.h"

namespace gl::packs::synths {

namespace {

using cd = std::complex<double>;

void fft(std::vector<cd>& a) {
  size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    double ang = -2.0 * dsp::kPi / static_cast<double>(len);
    cd wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cd w(1, 0);
      for (size_t j = 0; j < len / 2; ++j) {
        cd u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

constexpr int kHarm = 1024;     // harmonics kept at level 0 (bins 1..1023)
constexpr int kTdSize = 16384;  // oversampled time-domain rendering for analytic shapes

double frac1(double x) { return x - std::floor(x); }

// Time-domain shapes (table, morph 0..1, phase 0..1).
double timeShape(int table, double m, double p) {
  switch (table) {
    case TblAnalog: {
      auto key = [&](int k) {
        switch (k) {
          case 0: return std::sin(dsp::kTwoPi * p);
          case 1: return 0.5 * (std::sin(dsp::kTwoPi * p) + (p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4));
          case 2: return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
          case 3: return 0.5 * ((p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4) + (1.0 - 2.0 * frac1(p + 0.5)));
          case 4: return 1.0 - 2.0 * frac1(p + 0.5);
          case 5: return 0.5 * ((1.0 - 2.0 * frac1(p + 0.5)) + (p < 0.5 ? 1.0 : -1.0));
          case 6: return p < 0.5 ? 1.0 : -1.0;
          default: return p < 0.2 ? 1.0 : -1.0;
        }
      };
      double x = m * 7.0;
      int k = std::min(6, static_cast<int>(x));
      double t = x - k;
      return key(k) * (1 - t) + key(k + 1) * t;
    }
    case TblPulse: {
      double w = 0.5 - 0.47 * m;
      return p < w ? 1.0 : -1.0;
    }
    case TblSync: {
      double r = 1.0 + 7.0 * m;
      double win = 0.5 - 0.5 * std::cos(dsp::kTwoPi * p);  // softens the cycle boundary
      return (2.0 * frac1(p * r) - 1.0) * (0.35 + 0.65 * win);
    }
    case TblFm: {
      double index = m * 5.0;
      return std::sin(dsp::kTwoPi * p + index * std::sin(dsp::kTwoPi * 2.0 * p));
    }
    case TblDigital: {
      double steps = std::round(64.0 - 56.0 * m);
      double levels = std::round(1.0 + (1.0 - m) * 14.0);
      double pq = std::floor(p * steps) / steps;
      return std::round(std::sin(dsp::kTwoPi * pq) * levels) / levels;
    }
    default: return 0.0;
  }
}

bool isTimeDomain(int t) { return t == TblAnalog || t == TblPulse || t == TblSync || t == TblFm || t == TblDigital; }

// Spectral shapes: complex coefficient of harmonic h (x(p) = Σ Re(c·e^{i2πhp})).
cd spectralShape(int table, double m, int h) {
  const cd sinPhase(0, -1);  // a·sin(2πhp)
  double hd = static_cast<double>(h);
  switch (table) {
    case TblFormant: {
      static const double vowels[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
      double x = m * 4.0;
      int k = std::min(3, static_cast<int>(x));
      double t = x - k;
      double f = hd * 110.0, w = 0.04;
      const double bw[3] = {90, 110, 160}, g[3] = {1.0, 0.7, 0.35};
      for (int i = 0; i < 3; ++i) {
        double F = vowels[k][i] * (1 - t) + vowels[k + 1][i] * t;
        double d = (f - F) / bw[i];
        w += g[i] / (1.0 + d * d);
      }
      return sinPhase * (w / std::sqrt(hd)) * (f > 6000 ? 0.0 : 1.0);
    }
    case TblHarmonic: {
      double n = 1.0 + m * 31.0;
      double a = hd <= std::floor(n) ? 1.0 : hd <= std::floor(n) + 1 ? n - std::floor(n) : 0.0;
      return sinPhase * (a / hd);
    }
    case TblReso: {
      double hc = 2.0 + m * 30.0;
      double d = (hd - hc) / 1.2;
      double a = (1.0 / hd) * (hd <= hc ? 1.0 : std::pow(hc / hd, 3.0)) * (1.0 + 6.0 / (1.0 + d * d));
      return sinPhase * a;
    }
    case TblOddEven: {
      double a = (h % 2 == 1 ? (1.0 - m) : m) / hd;
      return sinPhase * a;
    }
    case TblOrgan: {
      static const int bars[7] = {1, 2, 3, 4, 5, 6, 8};
      static const double reg[8][7] = {{8, 0, 0, 0, 0, 0, 0}, {8, 8, 0, 0, 0, 0, 0}, {8, 8, 8, 0, 0, 0, 0}, {8, 6, 8, 4, 0, 0, 0},
                                       {8, 8, 8, 8, 0, 0, 0}, {8, 8, 8, 8, 4, 4, 4}, {8, 6, 8, 8, 6, 6, 8}, {8, 8, 8, 8, 8, 8, 8}};
      double x = m * 7.0;
      int k = std::min(6, static_cast<int>(x));
      double t = x - k;
      for (int b = 0; b < 7; ++b)
        if (bars[b] == h) return sinPhase * ((reg[k][b] * (1 - t) + reg[k + 1][b] * t) / 8.0);
      return cd(0, 0);
    }
    default: return cd(0, 0);
  }
}

struct Tables {
  std::vector<float> data;
  Tables() : data(static_cast<size_t>(TblCount) * kWtFrames * kWtLevels * kWtSize, 0.f) {
    std::vector<cd> coef(kHarm), td(kTdSize), z(kWtSize);
    for (int t = 0; t < TblCount; ++t)
      for (int f = 0; f < kWtFrames; ++f) {
        double m = static_cast<double>(f) / (kWtFrames - 1);
        if (isTimeDomain(t)) {
          for (int i = 0; i < kTdSize; ++i) td[static_cast<size_t>(i)] = cd(timeShape(t, m, static_cast<double>(i) / kTdSize), 0);
          fft(td);
          for (int h = 1; h < kHarm; ++h) coef[static_cast<size_t>(h)] = td[static_cast<size_t>(h)] * (2.0 / kTdSize);
        } else {
          for (int h = 1; h < kHarm; ++h) coef[static_cast<size_t>(h)] = spectralShape(t, m, h);
        }
        coef[0] = 0;
        double gain = 1.0;
        for (int L = 0; L < kWtLevels; ++L) {
          int H = kHarm >> L;
          std::fill(z.begin(), z.end(), cd(0, 0));
          for (int h = 1; h < H && h < kHarm; ++h) z[static_cast<size_t>(h)] = std::conj(coef[static_cast<size_t>(h)]);
          if (H == 1) z[1] = std::conj(coef[1]);  // the top level keeps the fundamental
          fft(z);  // conj → forward FFT → conj is the inverse; we only need the real part
          float* out = &data[index(t, f, L)];
          if (L == 0) {
            double peak = 1e-9;
            for (int i = 0; i < kWtSize; ++i) peak = std::max(peak, std::fabs(z[static_cast<size_t>(i)].real()));
            gain = 0.9 / peak;
          }
          for (int i = 0; i < kWtSize; ++i) out[i] = static_cast<float>(z[static_cast<size_t>(i)].real() * gain);
        }
      }
  }
  static size_t index(int t, int f, int L) {
    return ((static_cast<size_t>(t) * kWtFrames + static_cast<size_t>(f)) * kWtLevels + static_cast<size_t>(L)) * kWtSize;
  }
};

const Tables& tables() {
  static const Tables t;
  return t;
}

}  // namespace

const float* wavetableFrame(int table, int frame, int level) {
  table = std::clamp(table, 0, TblCount - 1);
  frame = std::clamp(frame, 0, kWtFrames - 1);
  level = std::clamp(level, 0, kWtLevels - 1);
  return &tables().data[Tables::index(table, frame, level)];
}

const char* const* wavetableNames() {
  static const char* const names[TblCount] = {"Analog morph", "Pulse sweep", "Vowels", "Harmonic build", "Sync sweep",
                                              "FM sweep", "Resonant sweep", "Odd to even", "Bit steps", "Drawbars"};
  return names;
}

namespace {

using dsp::noteToHz;
using dsp::polyBlep;

constexpr int kMaxVoices = 32;
constexpr int kChunk = 256;
constexpr int kU = kWtMaxUnison;

int mipLevel(double dt) {
  double x = 2048.0 * dt;
  if (x <= 1.0) return 0;
  return std::min(kWtLevels - 1, static_cast<int>(std::ceil(std::log2(x))));
}

inline double readFrame(const float* a, const float* b, double mix, double p) {
  double x = p * kWtSize;
  int i = static_cast<int>(x);
  double t = x - i;
  i &= kWtSize - 1;
  int j = (i + 1) & (kWtSize - 1);
  double va = a[i] + (a[j] - a[i]) * t;
  double vb = b[i] + (b[j] - b[i]) * t;
  return va + (vb - va) * mix;
}

class WavetableGen final : public Generator {
 public:
  WavetableGen() { (void)wavetableFrame(0, 0, 0); }  // builds the tables off the audio thread

  bool active() const override {
    if (tail_ > 0) return true;
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct O {
    bool on = true, randPhase = true;
    int table = 0, warp = 0, uni = 1;
    double pos = 0.5, warpAmt = 0, tune = 0, level = 0.75, pan = 0, detune = 0.2, blend = 0.75, width = 0.8, phase = 0;
  };
  struct F {
    bool on = true;
    int type = FltLp24;
    double cutoff = 6000, reso = 0.15, drive = 0, keyTrack = 0.2;
  };
  struct OscState {
    double ph[kU] = {};
    double dt[kU] = {};
    double gl[kU] = {}, gr[kU] = {};
    const float* f0 = nullptr;
    const float* f1 = nullptr;
    double mix = 0, warp = 0, last = 0, level = 0;
  };
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    double pitch = 60, target = 60;
    float vel = 1;
    uint64_t age = 0;
    uint32_t ctr = 0;
    OscState osc[2];
    double subPh = 0, subDt = 0, subLvl = 0, noiseLvl = 0, noiseLp = 0;
    dsp::Adsr env[4];
    MultiFilter flt[2][2];
    dsp::Noise noise;
    LfoPhase lfo[4];
    double lfoInc[4] = {};
    double rnd = 0, gain = 0, panL = 1, panR = 1;
  };

  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo;
    volume_ = prm(s, WtVolume, 0.7f);
    transpose_ = prm(s, WtTranspose);
    maxVoices_ = std::clamp(prmi(s, WtVoices, 12), 1, kMaxVoices);
    playMode_ = std::clamp(prmi(s, WtPlayMode), 0, 2);
    glide_ = prm(s, WtGlide, 0.001f);
    velAmp_ = prm(s, WtVelAmp, 0.5f);
    for (int k = 0; k < 2; ++k) {
      O& o = o_[k];
      o.on = prmb(s, wtOsc(k, WoOn));
      o.table = std::clamp(prmi(s, wtOsc(k, WoTable)), 0, TblCount - 1);
      o.pos = prm(s, wtOsc(k, WoPos));
      o.warp = std::clamp(prmi(s, wtOsc(k, WoWarp)), 0, WarpCount - 1);
      o.warpAmt = prm(s, wtOsc(k, WoWarpAmt));
      o.tune = prm(s, wtOsc(k, WoCoarse)) + prm(s, wtOsc(k, WoFine)) / 100.0;
      o.level = prm(s, wtOsc(k, WoLevel), 0.75f);
      o.pan = prm(s, wtOsc(k, WoPan));
      o.uni = std::clamp(prmi(s, wtOsc(k, WoUni), 1), 1, kU);
      o.detune = prm(s, wtOsc(k, WoDetune));
      o.blend = prm(s, wtOsc(k, WoBlend), 0.75f);
      o.width = prm(s, wtOsc(k, WoWidth), 0.8f);
      o.phase = prm(s, wtOsc(k, WoPhase));
      o.randPhase = prmb(s, wtOsc(k, WoRandPhase));
    }
    subLevel_ = prm(s, WtSubLevel);
    subOct_ = prmi(s, WtSubOct) == 1 ? -24.0 : -12.0;
    subWave_ = std::clamp(prmi(s, WtSubWave), 0, 2);
    noise_ = prm(s, WtNoiseLevel);
    double c = prm(s, WtNoiseColor, 0.5f);
    noiseBright_ = c > 0.5 ? (c - 0.5) * 2.0 : 0.0;
    double fc = c < 0.5 ? 200.0 * std::pow(100.0, c * 2.0) : 1500.0;
    noiseCoef_ = std::exp(-dsp::kTwoPi * std::min(fc, sr_ * 0.45) / sr_);
    noiseDark_ = c < 0.5;
    for (int k = 0; k < 2; ++k) {
      F& f = f_[k];
      f.on = prmb(s, wtFlt(k, WfOn));
      f.type = std::clamp(prmi(s, wtFlt(k, WfType)), 0, FltModeCount - 1);
      f.cutoff = prm(s, wtFlt(k, WfCutoff), 6000);
      f.reso = prm(s, wtFlt(k, WfReso));
      f.drive = prm(s, wtFlt(k, WfDrive));
      f.keyTrack = prm(s, wtFlt(k, WfKeyTrack));
    }
    parallel_ = prmi(s, WtRouting) == RouteParallel;
    for (int l = 0; l < 4; ++l) {
      lfoShape_[l] = std::clamp(prmi(s, wtLfo(l, WlShape)), 0, LfoShapeCount - 1);
      lfoHz_[l] = lfoHz(prmb(s, wtLfo(l, WlSync)), prmi(s, wtLfo(l, WlDiv)), prm(s, wtLfo(l, WlRate), 1), tempo_);
      lfoRetrig_[l] = prmb(s, wtLfo(l, WlRetrig));
    }
    for (int r = 0; r < kWtMatrixRows; ++r) {
      mxSrc_[r] = std::clamp(prmi(s, wtMx(r, 0)), 0, WsSourceCount - 1);
      mxDst_[r] = std::clamp(prmi(s, wtMx(r, 1)), 0, WdDestCount - 1);
      mxAmt_[r] = prm(s, wtMx(r, 2));
    }
    for (int k = 0; k < 4; ++k) macro_[k] = prm(s, WtMacro1 + k);
    for (auto& v : voices_)
      for (int e = 0; e < 4; ++e) v.env[e].set(sr_, prm(s, wtEnv(e, 0)), prm(s, wtEnv(e, 1)), prm(s, wtEnv(e, 2)), prm(s, wtEnv(e, 3)));
    fx_.configure(s, WtFxDrive, sr_, tempo_);
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    ++clock_;
    if (playMode_ != PlayPoly) {
      bool hadHeld = stack_.count > 0;
      stack_.push(id, pitch, vel);
      Voice& v = voices_[0];
      bool legato = playMode_ == PlayLegato && hadHeld && v.on;
      bool glide = glide_ > 0.002 && v.on;
      v.id = id;
      v.target = pitch;
      if (!glide) v.pitch = pitch;
      v.vel = vel;
      v.held = true;
      v.age = clock_;
      if (!legato) trigger(v);
      lastPitch_ = pitch;
      return;
    }
    Voice* pick = nullptr;
    for (int i = 0; i < maxVoices_ && !pick; ++i)
      if (!voices_[static_cast<size_t>(i)].on) pick = &voices_[static_cast<size_t>(i)];
    if (!pick)
      for (int i = 0; i < maxVoices_; ++i) {
        Voice& v = voices_[static_cast<size_t>(i)];
        if (!pick || (!v.held && pick->held) || (v.held == pick->held && v.age < pick->age)) pick = &v;
      }
    Voice& v = *pick;
    v.id = id;
    v.target = pitch;
    v.pitch = glide_ > 0.002 && lastPitch_ >= 0 ? lastPitch_ : pitch;
    v.vel = vel;
    v.held = true;
    v.age = clock_;
    trigger(v);
    lastPitch_ = pitch;
  }

  void onNoteOff(uint32_t id) override {
    if (playMode_ != PlayPoly) {
      bool wasTop = stack_.top() && stack_.top()->id == id;
      stack_.remove(id);
      Voice& v = voices_[0];
      if (!wasTop || !v.on) return;
      if (const auto* t = stack_.top()) {
        v.id = t->id;
        v.target = t->pitch;
        if (glide_ <= 0.002) v.pitch = t->pitch;
        if (playMode_ == PlayMono) trigger(v);
      } else {
        release(v);
      }
      return;
    }
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) release(v);
  }
  void onAllOff() override {
    stack_.clear();
    for (auto& v : voices_) release(v);
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
    stack_.clear();
    lastPitch_ = -1;
    tail_ = 0;
  }

  void trigger(Voice& v) {
    bool fresh = !v.on || v.env[0].level <= 1e-4;
    if (fresh) {
      for (int k = 0; k < 2; ++k)
        for (int u = 0; u < kU; ++u)
          v.osc[k].ph[u] = o_[k].randPhase ? 0.5 + 0.5 * hashUnit(static_cast<uint32_t>(clock_ * 7919u + static_cast<uint64_t>(k * 37 + u)))
                                            : o_[k].phase;
      v.subPh = 0;
      for (auto& ff : v.flt)
        for (auto& f : ff) f.reset();
    }
    v.noise.seed(static_cast<uint32_t>(clock_ * 2654435761u + 7u));
    v.rnd = hashUnit(static_cast<uint32_t>(clock_ * 40503u + 99u));
    v.on = true;
    v.ctr = 0;
    for (int l = 0; l < 4; ++l)
      if (lfoRetrig_[l]) v.lfo[l].reset();
    for (auto& e : v.env) e.gate(true);
  }
  void release(Voice& v) {
    v.held = false;
    for (auto& e : v.env) e.gate(false);
  }

  void render(float* L, float* R, int n) override {
    int pos = 0;
    while (pos < n) {
      int m = std::min(n - pos, kChunk);
      std::fill(bufL_.begin(), bufL_.begin() + m, 0.f);
      std::fill(bufR_.begin(), bufR_.begin() + m, 0.f);
      bool any = false;
      for (auto& v : voices_)
        if (v.on) {
          renderVoice(v, m);
          any = true;
        }
      fx_.process(bufL_.data(), bufR_.data(), m);
      for (int i = 0; i < m; ++i) {
        L[pos + i] += bufL_[static_cast<size_t>(i)];
        R[pos + i] += bufR_[static_cast<size_t>(i)];
      }
      for (int l = 0; l < 4; ++l) globalLfo_[l].advance(lfoHz_[l] / sr_ * m);
      if (any)
        tail_ = fx_.tailActive() ? static_cast<int64_t>(sr_ * 6) : 0;
      else if (tail_ > 0)
        tail_ = fx_.tailActive() ? tail_ - m : 0;
      pos += m;
    }
  }

  void control(Voice& v, int i) {
    double src[WsSourceCount] = {};
    for (int e = 1; e < 4; ++e) src[WsEnv2 + e - 1] = v.env[e].level;
    for (int l = 0; l < 4; ++l) {
      if (lfoRetrig_[l]) {
        src[WsLfo1 + l] = lfoValue(lfoShape_[l], v.lfo[l].phase, v.lfo[l].cycle, static_cast<uint32_t>(l * 613 + 5) ^ v.id);
      } else {
        LfoPhase g = globalLfo_[l];
        g.advance(lfoHz_[l] / sr_ * i);
        src[WsLfo1 + l] = lfoValue(lfoShape_[l], g.phase, g.cycle, static_cast<uint32_t>(l * 613 + 5));
      }
    }
    src[WsVelocity] = v.vel;
    src[WsKey] = (v.target - 60.0) / 60.0;
    src[WsRandom] = v.rnd;
    for (int k = 0; k < 4; ++k) src[WsMacro1 + k] = macro_[k];
    double d[WdDestCount] = {};
    for (int r = 0; r < kWtMatrixRows; ++r)
      if (mxSrc_[r] != WsNone) d[mxDst_[r]] += mxAmt_[r] * src[mxSrc_[r]];
    for (int l = 0; l < 4; ++l) {
      double rm = l < 2 ? std::pow(2.0, std::clamp(d[WdLfo1Rate + l], -1.0, 1.0) * 3.0) : 1.0;
      v.lfoInc[l] = lfoHz_[l] * rm / sr_;
    }
    const double base = v.pitch + transpose_ + d[WdPitch] * 24.0;
    for (int k = 0; k < 2; ++k) {
      const O& o = o_[k];
      OscState& os = v.osc[k];
      if (!o.on) continue;
      double pk = base + o.tune + d[WdAPitch + k] * 24.0;
      double det = std::pow(std::clamp(o.detune + d[WdADetune + k], 0.0, 1.0), 1.5);
      int N = o.uni;
      double maxDt = 0;
      double wsum = 0;
      double w[kU];
      for (int u = 0; u < N; ++u) {
        double x = N > 1 ? 2.0 * u / (N - 1) - 1.0 : 0.0;
        os.dt[u] = std::min(0.45, noteToHz(pk + x * det) / sr_);
        maxDt = std::max(maxDt, os.dt[u]);
        bool centre = N == 1 || std::fabs(x) < 1.0 / (N - 1) + 1e-9;
        w[u] = centre ? 1.0 : o.blend;
        wsum += w[u] * w[u];
      }
      double norm = 1.0 / std::sqrt(std::max(wsum, 1e-9));
      for (int u = 0; u < N; ++u) {
        double x = N > 1 ? 2.0 * u / (N - 1) - 1.0 : 0.0;
        double p = std::clamp(o.pan + x * o.width, -1.0, 1.0);
        os.gl[u] = w[u] * norm * std::sqrt(1.0 - p);
        os.gr[u] = w[u] * norm * std::sqrt(1.0 + p);
      }
      os.warp = std::clamp(o.warpAmt + d[WdAWarp + k], 0.0, 1.0);
      double bw = o.warp == WarpSync ? 1.0 + 7.0 * os.warp : o.warp == WarpMirror ? 2.0 : 1.0;
      int lvl = mipLevel(maxDt * bw);
      double fpos = std::clamp(o.pos + d[WdAPos + k], 0.0, 1.0) * (kWtFrames - 1);
      int f0 = std::min(kWtFrames - 2, static_cast<int>(fpos));
      os.mix = fpos - f0;
      os.f0 = wavetableFrame(o.table, f0, lvl);
      os.f1 = wavetableFrame(o.table, f0 + 1, lvl);
      os.level = std::clamp(o.level + d[WdALevel + k], 0.0, 1.0);
    }
    v.subDt = std::min(0.45, noteToHz(base + subOct_) / sr_);
    v.subLvl = std::clamp(subLevel_ + d[WdSubLevel], 0.0, 1.0);
    v.noiseLvl = std::clamp(noise_ + d[WdNoiseLevel], 0.0, 1.0);
    for (int k = 0; k < 2; ++k) {
      const F& f = f_[k];
      if (!f.on) continue;
      double oct = f.keyTrack * (v.target - 60.0) / 12.0 + d[WdF1Cutoff + 2 * k] * 6.0;
      double cut = std::clamp(f.cutoff * std::pow(2.0, oct), 16.0, std::min(20000.0, sr_ * 0.45));
      double res = std::clamp(f.reso + d[WdF1Reso + 2 * k], 0.0, 1.0);
      v.flt[k][0].set(sr_, cut, res, f.type);
      v.flt[k][1].set(sr_, cut, res, f.type);
    }
    v.gain = volume_ * (1.0 - velAmp_ * (1.0 - v.vel)) * std::clamp(1.0 + d[WdVolume], 0.0, 2.0) * 0.5;
    double pan = std::clamp(d[WdPan], -1.0, 1.0);
    v.panL = pan > 0 ? 1.0 - pan : 1.0;
    v.panR = pan < 0 ? 1.0 + pan : 1.0;
  }

  double oscSample(OscState& os, const O& o, double other, double& outR) {
    double sl = 0, sr = 0;
    const int N = o.uni;
    const double a = os.warp;
    for (int u = 0; u < N; ++u) {
      double p = os.ph[u];
      os.ph[u] += os.dt[u];
      if (os.ph[u] >= 1.0) os.ph[u] -= 1.0;
      double y;
      switch (o.warp) {
        case WarpBendPlus:
        case WarpBendMinus: {
          double mid = o.warp == WarpBendPlus ? 0.5 - 0.45 * a : 0.5 + 0.45 * a;
          double q = p < mid ? 0.5 * p / mid : 0.5 + 0.5 * (p - mid) / (1.0 - mid);
          y = readFrame(os.f0, os.f1, os.mix, q);
          break;
        }
        case WarpSync: y = readFrame(os.f0, os.f1, os.mix, frac1(p * (1.0 + 7.0 * a))); break;
        case WarpMirror: {
          double q = p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p;
          y = (1.0 - a) * readFrame(os.f0, os.f1, os.mix, p) + a * readFrame(os.f0, os.f1, os.mix, std::min(q, 0.999999));
          break;
        }
        case WarpCrush: {
          y = readFrame(os.f0, os.f1, os.mix, p);
          double steps = std::pow(2.0, 1.0 + (1.0 - a) * 11.0);
          y = std::round(y * steps) / steps;
          break;
        }
        case WarpFm: y = readFrame(os.f0, os.f1, os.mix, frac1(p + a * other)); break;
        default: y = readFrame(os.f0, os.f1, os.mix, p); break;
      }
      sl += y * os.gl[u];
      sr += y * os.gr[u];
    }
    os.last = (sl + sr) * 0.5;
    outR = sr * os.level;
    return sl * os.level;
  }

  void renderVoice(Voice& v, int n) {
    const double glideCoef = std::pow(std::exp(-1.0 / std::max(1.0, glide_ * sr_ * 0.3)), 16.0);
    for (int i = 0; i < n; ++i) {
      if ((v.ctr++ & 15u) == 0) {
        v.pitch = v.target + (v.pitch - v.target) * glideCoef;
        control(v, i);
      }
      double sl = 0, sr = 0;
      if (o_[0].on) {
        double r = 0;
        sl += oscSample(v.osc[0], o_[0], v.osc[1].last, r);
        sr += r;
      }
      if (o_[1].on) {
        double r = 0;
        sl += oscSample(v.osc[1], o_[1], v.osc[0].last, r);
        sr += r;
      }
      double mono = 0;
      if (v.subLvl > 0) {
        double p = v.subPh, dt = v.subDt, s;
        if (subWave_ == 0)
          s = std::sin(dsp::kTwoPi * p);
        else if (subWave_ == 1)
          s = 1.0 - 4.0 * std::fabs(p - 0.5) + 4.0 * dt * (polyBlamp(p, dt) - polyBlamp(std::fmod(p + 0.5, 1.0), dt));
        else
          s = (p < 0.5 ? 1.0 : -1.0) + polyBlep(p, dt) - polyBlep(std::fmod(p + 0.5, 1.0), dt);
        mono += s * v.subLvl;
        v.subPh += dt;
        if (v.subPh >= 1.0) v.subPh -= 1.0;
      }
      if (v.noiseLvl > 0) {
        double x = v.noise.next();
        v.noiseLp = x + (v.noiseLp - x) * noiseCoef_;
        double y = noiseDark_ ? v.noiseLp : x + (x - v.noiseLp) * noiseBright_;
        mono += y * v.noiseLvl * 0.7;
      }
      sl += mono;
      sr += mono;
      double ol = sl, orr = sr;
      if (parallel_) {
        double al = 0, ar = 0;
        int cnt = 0;
        for (int k = 0; k < 2; ++k) {
          if (!f_[k].on) continue;
          double g = 1.0 + f_[k].drive * 8.0, mk = 2.0 / (1.0 + f_[k].drive * 2.0);
          double xl = f_[k].drive > 0.001 ? softClip(sl * g) * mk : sl, xr = f_[k].drive > 0.001 ? softClip(sr * g) * mk : sr;
          al += v.flt[k][0].process(xl);
          ar += v.flt[k][1].process(xr);
          ++cnt;
        }
        if (cnt) ol = al / cnt, orr = ar / cnt;
      } else {
        for (int k = 0; k < 2; ++k) {
          if (!f_[k].on) continue;
          if (f_[k].drive > 0.001) {
            double g = 1.0 + f_[k].drive * 8.0, mk = 2.0 / (1.0 + f_[k].drive * 2.0);
            ol = softClip(ol * g) * mk;
            orr = softClip(orr * g) * mk;
          }
          ol = v.flt[k][0].process(ol);
          orr = v.flt[k][1].process(orr);
        }
      }
      double a = v.env[0].next() * v.gain;
      for (int e = 1; e < 4; ++e) v.env[e].next();
      bufL_[static_cast<size_t>(i)] += static_cast<float>(ol * a * v.panL);
      bufR_[static_cast<size_t>(i)] += static_cast<float>(orr * a * v.panR);
      for (int l = 0; l < 4; ++l)
        if (lfoRetrig_[l]) v.lfo[l].advance(v.lfoInc[l]);
      if (!v.env[0].active()) {
        v.on = false;
        v.held = false;
        break;
      }
    }
  }

 private:
  std::array<Voice, kMaxVoices> voices_{};
  std::array<float, kChunk> bufL_{}, bufR_{};
  FxRack fx_;
  NoteStack stack_;
  LfoPhase globalLfo_[4];
  O o_[2];
  F f_[2];
  bool parallel_ = false, noiseDark_ = false;
  double sr_ = 44100, tempo_ = 140, volume_ = 0.7, transpose_ = 0, glide_ = 0.001, velAmp_ = 0.5;
  int maxVoices_ = 12, playMode_ = PlayPoly, subWave_ = 0;
  double subLevel_ = 0, subOct_ = -12, noise_ = 0, noiseBright_ = 0, noiseCoef_ = 0.5;
  int lfoShape_[4] = {}, mxSrc_[kWtMatrixRows] = {}, mxDst_[kWtMatrixRows] = {};
  double lfoHz_[4] = {1, 1, 1, 1}, mxAmt_[kWtMatrixRows] = {}, macro_[4] = {};
  bool lfoRetrig_[4] = {};
  uint64_t clock_ = 0;
  double lastPitch_ = -1;
  int64_t tail_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeWavetable() { return std::make_unique<WavetableGen>(); }

}  // namespace gl::packs::synths

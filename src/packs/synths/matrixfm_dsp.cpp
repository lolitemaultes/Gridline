// Gridline — "synths" pack: Matrix FM runtime (gl.matrixfm).
//
// Per voice: six table operators (harmonic tables built on the UI thread,
// band-limited mip levels) and three multimode filters wired by the 9 × 9
// modulation matrix (phase modulation + ring modulation layer), every unit
// shaped by its articulators (multipoint envelope + drawn LFO + mapping
// curves), evaluated at control rate. Voices sum into a dry bus and an
// effects bus (chorus → 3 delays → reverb); a 3-band EQ sits on the selected
// bus. No allocation after construction.
//
// MModel = 1 (J-50) plays the reference model instead (mfmref.h): its engine
// is built on the UI thread in prepareHost and handed over through an atomic
// pointer; the live controls (volume, matrix, operator volumes, filter
// controls, mod X / Y) follow the parameters every block.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include "matrixfm.h"
#include "matrixfm_data.h"
#include "mfmref.h"
#include "synths_common.h"

namespace gl::packs::synths {

namespace {

using namespace mfm;

constexpr int kTab = 2048;          // samples per table cycle
constexpr int kLevels = 9;          // mip levels: 256, 128, … 1 harmonics
constexpr int kCtl = 16;            // control-rate period (samples)
constexpr int kMaxVoices = 64;      // voice pool (unison voices included)
constexpr int kPluck = 2048;        // pluck delay line per operator
constexpr int kChunk = 64;

// ---------------------------------------------------------------- tables
struct MipTable {
  std::vector<float> t;  // kLevels × (kTab + 1)
  const float* level(int l) const { return t.data() + static_cast<size_t>(l) * (kTab + 1); }
};

void buildMip(MipTable& m, const float* lv, const float* ph, int nh, bool gibbs) {
  m.t.assign(static_cast<size_t>(kLevels) * (kTab + 1), 0.f);
  std::vector<double> acc(kTab);
  double peak = 0;
  for (int l = 0; l < kLevels; ++l) {
    const int maxH = std::min(nh, 256 >> l);
    std::fill(acc.begin(), acc.end(), 0.0);
    for (int h = 0; h < maxH; ++h) {
      double a = lv[h];
      if (a == 0) continue;
      if (gibbs) {
        const double x = dsp::kPi * (h + 1) / (maxH + 1);
        a *= std::sin(x) / x;
      }
      const double p0 = ph ? ph[h] : 0.0;
      for (int i = 0; i < kTab; ++i) acc[static_cast<size_t>(i)] += a * std::sin(dsp::kTwoPi * ((h + 1) * (i / double(kTab)) + p0));
    }
    float* dst = m.t.data() + static_cast<size_t>(l) * (kTab + 1);
    for (int i = 0; i < kTab; ++i) dst[i] = static_cast<float>(acc[static_cast<size_t>(i)]);
    dst[kTab] = dst[0];
    if (l == 0)
      for (double v : acc) peak = std::max(peak, std::fabs(v));
  }
  if (peak > 1.0)
    for (float& v : m.t) v = static_cast<float>(v / peak);
}

const MipTable& sawTable() {
  static const MipTable t = [] {
    MipTable m;
    float lv[256], ph[256];
    for (int h = 0; h < 256; ++h) lv[h] = static_cast<float>(0.55 / (h + 1)), ph[h] = 0.5f;  // -sin: rising ramp
    buildMip(m, lv, ph, 256, false);
    return m;
  }();
  return t;
}
const MipTable& squareTable() {
  static const MipTable t = [] {
    MipTable m;
    float lv[256], ph[256];
    for (int h = 0; h < 256; ++h) lv[h] = (h % 2 == 0) ? static_cast<float>(0.9 / (h + 1)) : 0.f, ph[h] = 0.f;
    buildMip(m, lv, ph, 256, false);
    return m;
  }();
  return t;
}

inline float lookup(const float* t, double p) {
  const double x = p * kTab;
  const int i = static_cast<int>(x);
  const float f = static_cast<float>(x - i);
  return t[i] + (t[i + 1] - t[i]) * f;
}

}  // namespace

namespace mfm {

struct MfmAux final : AuxData {
  PatchData d;
  std::array<MipTable, kOps> tab;  // empty = pure sine
  std::array<bool, kOps> sine{};
  MfmAux() { sine.fill(true); }
};

std::shared_ptr<const AuxData> buildMatrixFmAux(const PluginState& s) {
  auto a = std::make_shared<MfmAux>();
  parsePatch(s.data, a->d);
  const bool gibbs = prmb(s, MGibbs);
  for (int k = 0; k < kOps; ++k) {
    const auto& lv = a->d.level[static_cast<size_t>(k)];
    const auto& ph = a->d.phase[static_cast<size_t>(k)];
    bool pure = lv[0] == 1.f && ph[0] == 0.f;
    for (int h = 1; h < kHarmonics && pure; ++h) pure = lv[static_cast<size_t>(h)] == 0.f;
    a->sine[static_cast<size_t>(k)] = pure;
    if (!pure) buildMip(a->tab[static_cast<size_t>(k)], lv.data(), ph.data(), kHarmonics, gibbs);
  }
  return a;
}

}  // namespace mfm

namespace {

// Envelope time scale factor of a 0..2 knob (1 = as drawn).
inline double timeScale(double v) { return std::exp2((std::clamp(v, 0.0, 2.0) - 1.0) * 8.0); }

// ------------------------------------------------------------ articulators
struct ArtCfg {
  bool env = false, lfo = false, any = false, bip = false;
  float amount = 1, lfoAmt = 1, sus = 0, skew = 0;
  double att = 1, dec = 1, rel = 1;
  double envT = 1;      // tempo envelopes: seconds per drawn unit (drawn at 120 BPM)
  double lfoPhase = 0;  // LFO start phase (cycles)
  double lfoHz = 2;
  bool global = false;
  const Curve* envC = nullptr;
  const Curve* lfoC = nullptr;
  double lfoLen = 1;
  int peak = 0;  // index of the attack peak point
  std::array<const Curve*, kMaps> maps{};
};

struct EnvRun {
  int seg = 1;      // segment ends at point seg
  double t = 0;     // time in segment
  double from = 0;  // start value of the segment
  double val = 0;
  bool done = false;
};

struct EnvShape {
  const Curve* c;
  const ArtCfg* a;
  double scaleT;  // unison envelope variation
};

double segDur(const ArtCfg& a, const Curve& c, int seg, double scaleT) {
  const double dt = c.pts[static_cast<size_t>(seg)].dt;
  double f = seg <= a.peak ? a.att : (c.sustain < 0 || seg <= c.sustain) ? a.dec : a.rel;
  return dt * f * scaleT * a.envT;
}
double segTarget(const ArtCfg& a, const Curve& c, int seg) {
  double y = c.pts[static_cast<size_t>(seg)].y;
  if (seg == c.sustain) y = std::clamp(y + a.sus, 0.0, 1.0);
  return y;
}

void envStart(EnvRun& r, const Curve& c) {
  r = EnvRun{};
  r.val = r.from = c.pts.empty() ? 0.0 : c.pts[0].y;
  r.seg = 1;
  r.done = c.pts.size() < 2;
}

// Advances by dt seconds; returns the value.
double envStep(EnvRun& r, const ArtCfg& a, const Curve& c, double dt, bool released, double scaleT) {
  const int n = static_cast<int>(c.pts.size());
  if (r.done || n < 2) return r.val;
  if (r.seg >= n) {  // the curve changed under a running voice
    r.done = true;
    return r.val;
  }
  // Release: leave the sustain / loop section.
  if (released && c.sustain >= 0 && r.seg <= c.sustain) {
    r.seg = c.sustain + 1;
    r.t = 0;
    r.from = r.val;
    if (r.seg >= n) {
      r.done = true;
      return r.val;
    }
  }
  r.t += dt;
  for (int guard = 0; guard < 64; ++guard) {
    if (!released && c.sustain >= 0 && r.seg > c.sustain) {
      // Holding at the sustain point (or looping back to the loop start).
      if (c.loopStart >= 0 && c.loopStart < c.sustain) {
        r.seg = c.loopStart + 1;
        r.t = 0;
        r.from = r.val;
        continue;
      }
      r.val = segTarget(a, c, c.sustain);
      r.t = 0;
      return r.val;
    }
    const double d = segDur(a, c, r.seg, scaleT);
    const double target = segTarget(a, c, r.seg);
    if (r.t < d) {
      r.val = r.from + (target - r.from) * tensionShape(r.t / d, c.pts[static_cast<size_t>(r.seg)].tension);
      return r.val;
    }
    r.t -= d;
    r.val = r.from = target;
    if (++r.seg >= n) {
      r.done = true;
      return r.val;
    }
  }
  return r.val;
}

double lfoAt(const ArtCfg& a, double phase) {
  double p = phase - std::floor(phase);
  if (a.skew != 0) {
    const double m = 0.5 + 0.45 * a.skew;
    p = p < m ? 0.5 * p / m : 0.5 + 0.5 * (p - m) / (1 - m);
  }
  return curveAt(*a.lfoC, p * a.lfoLen);
}

// ---------------------------------------------------------------- filter
struct FltCfg {
  int mode = FmX1, type = FtLp;
  float cut = 0.5f, env = 1, res = 0, low = 1, band = 0, high = 0, wsAmp = 1, wsMix = 1, next = 0;
  bool flat = false, ws = false, wsBip = false;
  const Curve* wsC = nullptr;
};

struct FltState {
  dsp::Svf s[3];
  double ap[4] = {0, 0, 0, 0}, fb = 0;
  void reset() {
    for (auto& v : s) v.reset();
    for (double& v : ap) v = 0;
    fb = 0;
  }
};

double runFilter(FltState& st, const FltCfg& c, double sr, double x, double hz, double res, double low, double band, double high) {
  if (c.mode == FmOff) return x;
  const int stages = c.mode == FmX1 ? 1 : (c.mode == FmX2 || c.mode == FmAlt2) ? 2 : 3;
  const bool alt = c.mode == FmAlt2 || c.mode == FmAlt3;
  res = std::clamp(res, 0.0, 1.0);
  if (c.flat) res *= 0.6;
  if (c.type == FtPhaser) {
    const double t = std::tan(dsp::kPi * std::clamp(hz, 20.0, sr * 0.45) / sr), a = (t - 1) / (t + 1);
    double v = x + st.fb * res * 0.85;
    const int n = 2 * stages;
    for (int i = 0; i < n && i < 4; ++i) {
      const double y = a * v + st.ap[i];
      st.ap[i] = v - a * y;
      v = y;
    }
    st.fb = v;
    return 0.5 * (x + v);
  }
  double v = x;
  for (int i = 0; i < stages; ++i) {
    const double h = alt && (i % 2 == 1) ? hz * 1.5 : hz;
    const bool last = i == stages - 1;
    st.s[i].set(sr, h, last ? res : res * 0.3);
    double bp = 0, hp = 0;
    double lp = st.s[i].process(c.type == FtLp3 ? softClip(v * (1 + 4 * low)) / (1 + 2 * low) : v, &bp, &hp);
    const double k = st.s[i].k;
    const double g = std::pow(10.0, (low - 0.5) * 36.0 / 20.0);  // shelf / peak gain from LOW
    switch (c.type) {
      case FtSvf: v = lp * low + bp * band + hp * high; break;
      case FtLp: case FtLp2: case FtLp3: v = lp; break;
      case FtHp: case FtHp2: v = hp; break;
      case FtBp: v = bp * k; break;
      case FtBs: v = lp + hp; break;
      case FtAp: v = lp - k * bp + hp; break;
      case FtLs: v = v + (g - 1) * lp; break;
      case FtPk: v = v + (g - 1) * k * bp; break;
      case FtHs: v = v + (g - 1) * hp; break;
      default: v = lp; break;
    }
    if ((c.type == FtLp2 || c.type == FtHp2) && last) v *= 1.0;  // 12 dB/oct per stage (same core)
  }
  return v;
}

double waveshape(const FltCfg& c, double x) {
  // The transfer curve covers full scale; louder input stays at its end (the
  // default straight line clips at ±1, so the pre-amp drives into it).
  if (!c.wsC || c.wsC->empty()) return std::clamp(x, -1.0, 1.0);
  if (c.wsBip) {
    const double u = std::clamp((x + 1) * 0.5, 0.0, 1.0);
    return curveAt(*c.wsC, u * curveLength(*c.wsC)) * 2 - 1;
  }
  const double u = std::clamp(std::fabs(x), 0.0, 1.0);
  const double y = curveAt(*c.wsC, u * curveLength(*c.wsC));
  return x < 0 ? -y : y;
}

// ---------------------------------------------------------------- voice
struct OpCfg {
  float phase = 0, vol = 1, range = 12, ratio = 1, freq = 0, noise = 0, shape = 0, tension = 0, skew = 0, sine = 0, filter = 1;
  bool half = false, even = false, absv = false, global = false, center = false, declick = false, pluck = false, bandLimit = true;
};

struct Voice {
  bool on = false, released = false, gate = false;
  uint32_t id = 0;
  uint64_t order = 0;
  int uni = 0;
  double pitch = 60, uniSemis = 0;
  float vel = 1, x = 0.5f, y = 0.5f, rnd = 0.5f, uniPos = 0, panOfs = 0, gain = 1;
  double envScaleT = 1;
  PitchGlide glide;
  double ph[kOps] = {}, inc[kOps] = {};
  float prev[kOps] = {}, amp[kOps] = {}, ampTo[kOps] = {}, modAmt[kOps] = {}, phOfs[kOps] = {}, opPan[kOps] = {};
  double lp[kOps] = {}, lpA[kOps] = {}, dcX[kOps] = {}, dcY[kOps] = {};
  uint32_t cyc[kOps] = {}, noise = 1;
  float* pk = nullptr;  // kOps × kPluck
  int pkLen[kOps] = {}, pkPos[kOps] = {};
  int pkFill[kOps] = {};  // pluck: excitation samples left (one period of the waveform)
  double pkG[kOps] = {}, pkPrev[kOps] = {};
  FltState flt[kFilters];
  double fHz[kFilters] = {}, fRes[kFilters] = {}, fLow[kFilters] = {}, fBand[kFilters] = {}, fHigh[kFilters] = {};
  float fVol[kFilters] = {}, fPan[kFilters] = {}, fWs[kFilters] = {};
  float yF[kFilters] = {};
  EnvRun env[kUnits][kFltArts];
  double lfoPh[kUnits][kFltArts] = {};
  float art[kUnits][kFltArts] = {};
  float mainVol = 1, mainVolTo = 1, mainPan = 0;
  double age = 0, attackFade = 0;
  int ctl = 0;
  double releaseGate = 1;  // fade for voices without a main volume envelope
};

class MatrixFmGen final : public Generator {
 public:
  MatrixFmGen() {
    for (int c = 0; c < 2; ++c) chorusBuf_[c].assign(kChorusLen, 0.f);
    (void)sawTable();
    (void)squareTable();
  }
  ~MatrixFmGen() override {
    delete pending_.exchange(nullptr);
    delete specPending_.exchange(nullptr);
    for (auto& r : retired_) delete r.exchange(nullptr);
    for (auto& r : retiredSpec_) delete r.exchange(nullptr);
    delete retiredLeak_;
    delete spec_;
    if (ref_) mfmref::EngineDeleter{}(ref_);
  }

  // J-30: the delay lines, the reverb and the pluck lines (~11 MB) are
  // allocated here (UI thread, every program build) only once the patch uses
  // them; a section whose memory is not there yet is skipped.
  void prepareHost(const PluginState& s, double sampleRate) override {
    prepared_.store(true, std::memory_order_release);
    for (auto& r : retired_) delete r.exchange(nullptr);
    for (auto& r : retiredSpec_) delete r.exchange(nullptr);
    if (prmb(s, MModel)) {
      prepareRef(s, static_cast<int>(std::lround(sampleRate)));
      return;
    }
    for (int i = 0; i < 3; ++i)
      if (prmb(s, dly(i, 0))) dlyMem_[i].ensure(2 * static_cast<size_t>(kDelayLen));
    if (prmb(s, RvOn)) rvMem_.ensure(kRvFloats);
    for (int k = 0; k < kOps; ++k)
      if (prmb(s, op(k, OpPluck))) {
        pluckMem_.ensure(static_cast<size_t>(kMaxVoices) * kOps * kPluck);
        break;
      }
  }

  bool supportsGlide() const override { return true; }

  bool active() const override {
    if (refOn_) return ref_ && mfmref::active(*ref_);
    for (const Voice& v : voices_)
      if (v.on) return true;
    return tailLeft_ > 0;
  }

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo;
    lastTempo_.store(ctx.tempo, std::memory_order_relaxed);
    const bool ref = prmb(s, MModel);
    if (ref != refOn_) {  // model switch: the other engine's voices stop
      if (ref_) mfmref::kill(*ref_);
      for (Voice& v : voices_) v.on = false;
      refOn_ = ref;
    }
    if (refOn_) {
      configureRef(s, ctx);
      return;
    }
    if (!prepared_.load(std::memory_order_acquire)) {  // used outside the engine (never prepared): everything now
      for (auto& d : dlyMem_) d.ensure(2 * static_cast<size_t>(kDelayLen));
      rvMem_.ensure(kRvFloats);
      pluckMem_.ensure(static_cast<size_t>(kMaxVoices) * kOps * kPluck);
    }
    bindMemory();
    aux_ = dynamic_cast<const mfm::MfmAux*>(aux.custom.get());
    if (!aux_) {
      if (!fallback_) fallback_ = std::make_shared<mfm::MfmAux>();
      aux_ = fallback_.get();
    }
    const PatchData& d = aux_->d;
    for (int r = 0; r < kRows; ++r)
      for (int c = 0; c < kCols; ++c) {
        fm_[r][c] = prm(s, fm(r, c));
        rm_[r][c] = r < kOps && c < kOps ? prm(s, rm(r, c)) : 0.f;
      }
    // Rows that reach the output (directly or through other rows).
    bool used[kRows] = {};
    for (int r = 0; r < kRows; ++r) used[r] = fm_[r][kColOut] != 0 || fm_[r][kColFx] != 0;
    for (int pass = 0; pass < kRows; ++pass)
      for (int r = 0; r < kRows; ++r)
        if (used[r]) {
          for (int c = 0; c < kOps; ++c)
            if (fm_[r][c] != 0 || rm_[r][c] != 0) used[c] = true;
          if (r >= kOps && r < kRows - 1 && prm(s, flt(r - kOps, FlNext)) != 0) used[r + 1] = true;
          if (r > kOps && prm(s, flt(r - kOps - 1, FlNext)) != 0) used[r - 1] = true;
        }
    for (int r = 0; r < kRows; ++r) used_[r] = used[r];
    for (int k = 0; k < kOps; ++k) {
      OpCfg& o = op_[k];
      o.phase = prm(s, op(k, OpPhase)), o.vol = prm(s, op(k, OpVol), 1), o.range = prm(s, op(k, OpPitchRange), 12);
      o.ratio = prm(s, op(k, OpRatio), 1), o.freq = prm(s, op(k, OpFreq)), o.noise = prm(s, op(k, OpNoise));
      o.shape = prm(s, op(k, OpShape)), o.tension = prm(s, op(k, OpTension)), o.skew = prm(s, op(k, OpSkew));
      o.sine = prm(s, op(k, OpSine)), o.filter = prm(s, op(k, OpFilter), 1);
      o.half = prmb(s, op(k, OpHalf)), o.even = prmb(s, op(k, OpEven)), o.absv = prmb(s, op(k, OpAbs));
      o.global = prmb(s, op(k, OpGlobal)), o.center = prmb(s, op(k, OpCenter)), o.declick = prmb(s, op(k, OpDeclick));
      o.pluck = prmb(s, op(k, OpPluck)), o.bandLimit = prm(s, op(k, OpBandLimit), 1) >= 0.5f;
      nzFm_[k] = 0, nzRm_[k] = 0;
      for (int j = 0; j < kOps; ++j) {
        if (fm_[k][j] != 0) fmList_[k][nzFm_[k]++] = j;
        if (rm_[k][j] != 0) rmList_[k][nzRm_[k]++] = j;
      }
    }
    for (int j = 0; j < kFilters; ++j) {
      FltCfg& f = flt_[j];
      f.mode = std::clamp(prmi(s, flt(j, FlMode), FmX1), 0, kFltModes - 1);
      f.type = std::clamp(prmi(s, flt(j, FlType), FtLp), 0, kFltTypes - 1);
      f.env = prm(s, flt(j, FlEnv), 1), f.cut = prm(s, flt(j, FlCut), 0.5f), f.res = prm(s, flt(j, FlRes));
      f.low = prm(s, flt(j, FlLow), 1), f.band = prm(s, flt(j, FlBand)), f.high = prm(s, flt(j, FlHigh));
      f.wsAmp = prm(s, flt(j, FlWsAmp), 1), f.wsMix = prm(s, flt(j, FlWsMix), 1), f.next = prm(s, flt(j, FlNext));
      f.flat = prmb(s, flt(j, FlFlat)), f.ws = prmb(s, flt(j, FlWsOn)), f.wsBip = prmb(s, flt(j, FlWsBipolar));
      f.wsC = &d.ws[static_cast<size_t>(j)];
    }
    const double vA = prm(s, MVolA, 1), vD = prm(s, MVolD, 1), vR = prm(s, MVolR, 1), vS = prm(s, MVolS);
    const double fA = prm(s, MFltA, 1), fD = prm(s, MFltD, 1), fR = prm(s, MFltR, 1), fS = prm(s, MFltS);
    lfoLevel_ = prm(s, MLfoLevel, 1);
    for (int u = 0; u < kUnits; ++u)
      for (int a = 0; a < unitArts(u); ++a) {
        ArtCfg& c = art_[u][a];
        const ArtData& ad = d.arts[static_cast<size_t>(u)][static_cast<size_t>(a)];
        c.env = prmb(s, art(u, a, AfEnvOn)) && ad.env.pts.size() >= 2;
        c.lfo = prmb(s, art(u, a, AfLfoOn)) && !ad.lfo.empty();
        c.bip = artBipolar(u, a);
        c.amount = prm(s, art(u, a, AfAmount), 1);
        c.lfoAmt = prm(s, art(u, a, AfLfoAmt), 1) * lfoLevel_;
        const bool isVol = (u < kOps && a == OaVol) || (u >= kOps && u < kOps + kFilters && a == FaVol) || (u == kUnits - 1 && a == MaVol);
        const bool isCut = u >= kOps && u < kOps + kFilters && a == FaCut;
        c.att = timeScale(prm(s, art(u, a, AfAtt), 1)) * (isVol ? timeScale(vA) : isCut ? timeScale(fA) : 1.0);
        c.dec = timeScale(prm(s, art(u, a, AfDec), 1)) * (isVol ? timeScale(vD) : isCut ? timeScale(fD) : 1.0);
        c.rel = timeScale(prm(s, art(u, a, AfRel), 1)) * (isVol ? timeScale(vR) : isCut ? timeScale(fR) : 1.0);
        c.sus = static_cast<float>(prm(s, art(u, a, AfSus)) + (isVol ? vS : isCut ? fS : 0.0));
        c.skew = prm(s, art(u, a, AfLfoSkew));
        c.lfoHz = prm(s, art(u, a, AfLfoSpeed), 2);
        if (prmb(s, art(u, a, AfTempo))) c.lfoHz = c.lfoHz * std::max(1.0, tempo_) / 60.0;
        c.global = prmb(s, art(u, a, AfLfoGlobal));
        c.envT = ad.env.tempo ? 120.0 / std::max(20.0, tempo_) : 1.0;
        c.lfoPhase = ad.lfoPhase;
        c.envC = &ad.env;
        c.lfoC = &ad.lfo;
        c.lfoLen = std::max(1e-6, curveLength(ad.lfo));
        c.peak = 0;
        if (c.env && ad.env.decay >= 0) {
          c.peak = ad.env.decay;  // explicit decay point: the attack part ends there
        } else if (c.env) {
          const int lim = ad.env.sustain >= 0 ? ad.env.sustain : static_cast<int>(ad.env.pts.size()) - 1;
          float best = -1;
          for (int i = 1; i <= lim; ++i)
            if (ad.env.pts[static_cast<size_t>(i)].y > best) best = ad.env.pts[static_cast<size_t>(i)].y, c.peak = i;
        }
        bool anyMap = false;
        for (int m = 0; m < kMaps; ++m) {
          const Curve& mc = ad.maps[static_cast<size_t>(m)];
          c.maps[static_cast<size_t>(m)] = mc.empty() ? nullptr : &mc;
          anyMap = anyMap || !mc.empty();
        }
        c.any = c.env || c.lfo || anyMap || c.amount != 1;
      }
    mainVol_ = prm(s, MVolume, 1);
    mainPitch_ = prm(s, MPitch);
    uniN_ = std::clamp(prmi(s, MUniOrder, 1), 1, 9);
    uniPan_ = prm(s, MUniPan, 1), uniVol_ = prm(s, MUniVol, 0.78f), uniPitch_ = prm(s, MUniPitch, 0.5f);
    uniSub_ = prm(s, MUniSub), uniPhase_ = prm(s, MUniPhase, 0.125f), uniEnv_ = prm(s, MUniEnv);
    modX_ = prm(s, MModX, 0.5f), modY_ = prm(s, MModY, 0.5f);
    porta_ = prmb(s, MPorta), portaTime_ = prm(s, MPortaTime, 0.08f);
    poly_ = std::clamp(prmi(s, MVoices, 16), 1, 32);
    monoKey_ = prmb(s, MMonoKey), globalPitch_ = prmb(s, MGlobalPitch);
    soften_ = prm(s, MSoften), center_ = prmb(s, MCenter), random_ = prm(s, MRandom, 1) >= 0.5f, smooth_ = prmb(s, MSmooth);
    // Effects.
    fxPan_ = prm(s, FxPan);
    chN_ = std::clamp(prmi(s, ChOrder), 0, 9);
    chDepth_ = prm(s, ChDepth, 0.5f), chSpeed_ = prm(s, ChSpeed, 0.75f), chDelay_ = prm(s, ChDelay, 0.0625f);
    chSpread_ = prm(s, ChSpread, 1), chCross_ = prm(s, ChCross, -0.25f), chVol_ = prm(s, ChVol, 1);
    dlSerial_ = prmb(s, DlSerial);
    for (int i = 0; i < 3; ++i) {
      Dly& D = dly_[i];
      const bool on = prmb(s, dly(i, 0));
      if (on && !D.on) D.clear();
      D.on = on;
      D.mode = std::clamp(prmi(s, dly(i, 1)), 0, 2);
      D.fb = prm(s, dly(i, 2), 0.5f);
      const double t = prm(s, dly(i, 3), 0.75f);
      const double secs = prmb(s, dly(i, 6)) ? t * 60.0 / std::max(20.0, tempo_) : t * 0.5;
      const double ofs = prm(s, dly(i, 4));
      D.tL = std::clamp(secs * sr_, 1.0, kDelayLen - 4.0);
      D.tR = std::clamp(secs * (1.0 + 0.5 * ofs) * sr_, 1.0, kDelayLen - 4.0);
      D.vol = prm(s, dly(i, 5), 0.5f);
    }
    const bool rv = prmb(s, RvOn);
    if (rv && !rvOn_) clearReverb();
    rvOn_ = rv;
    rvWet_ = prm(s, RvVol, 0.2f);
    const double room = 0.35 + 0.95 * prm(s, RvRoom, 0.4f), decay = std::max(0.05f, prm(s, RvDecay, 1.5f));
    const double scale = sr_ / 44100.0 * room;
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 8; ++i) {
        comb_[c][i].len = std::clamp(static_cast<int>((kComb[i] + (c ? 23 : 0)) * scale), 2, kCombMax - 1);
        comb_[c][i].fb = std::pow(10.0, -3.0 * comb_[c][i].len / (sr_ * decay));
      }
      for (int i = 0; i < 4; ++i) ap_[c][i].len = std::clamp(static_cast<int>((kAp[i] + (c ? 23 : 0)) * sr_ / 44100.0), 2, kApMax - 1);
    }
    rvDiff_ = 0.5 * prm(s, RvDiff, 1);
    const int color = std::clamp(prmi(s, RvColor, 2), 0, 4);
    const double hd = prm(s, RvHighDamp, 4000) * std::pow(1.6, 2 - color);
    rvDamp_ = std::exp(-dsp::kTwoPi * std::clamp(hd, 200.0, sr_ * 0.45) / sr_);
    double pre = prm(s, RvPreDelay);
    if (prmb(s, RvTempo)) pre = pre * 4 * 60.0 / std::max(20.0, tempo_) * 0.25;
    rvPre_ = std::clamp(static_cast<int>(pre * sr_), 0, kPreLen - 1);
    for (int c = 0; c < 2; ++c) {
      rvHc_[c].setFreq(sr_, prm(s, RvHighCut, 4000));
      rvLc_[c].setFreq(sr_, prm(s, RvLowCut, 75));
    }
    // EQ.
    eqTarget_ = std::clamp(prmi(s, MEqTarget), 0, 3);
    for (int b = 0; b < 3; ++b) {
      const int ty = std::clamp(prmi(s, eq(b, 0), b), 0, 2);
      const double hz = std::clamp(static_cast<double>(prm(s, eq(b, 1), 1000)), 20.0, sr_ * 0.45);
      const double bw = std::max(0.1f, prm(s, eq(b, 2), 1));
      const double q = std::sqrt(std::exp2(bw)) / (std::exp2(bw) - 1.0);
      const double g = prm(s, eq(b, 3));
      for (int c = 0; c < 2; ++c)
        eqf_[b][c].set(ty == 0 ? dsp::Biquad::LowShelf : ty == 1 ? dsp::Biquad::Peak : dsp::Biquad::HighShelf, sr_, hz, q, g);
      eqOn_[b] = std::fabs(g) > 0.01;
    }
    dcL_.setFreq(sr_), dcR_.setFreq(sr_);
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override { onNoteOnEx(id, pitch, vel, NoteProps{}); }
  void onNoteOnEx(uint32_t id, double pitch, float vel, const NoteProps& p) override {
    if (refOn_) {
      if (ref_) {
        // The host's note: velocity v/128 of the instrument's scale, volume (11^(v/100) − 1) / 10.
        const double v128 = std::nearbyint(static_cast<double>(vel) * 128.0);
        const double vol = (std::pow(11.0, v128 / 100.0) - 1.0) / 10.0;
        const float velF = static_cast<float>(static_cast<double>(static_cast<float>(static_cast<float>(1.0 / 2.397895336151123) *
                                                                                     std::log(vol * 10.0 + 1.0))) * 0.78125);
        mfmref::noteOn(*ref_, id, (pitch - 60.0) * 100.0, vol, p.pan, (p.modX - 0.5) * 2.0, (p.modY - 0.5) * 2.0, velF, p.color);
      }
      return;
    }
    start(id, pitch, vel, p);
  }
  bool handlesNotePan() const override { return true; }

  void onNoteOff(uint32_t id) override {
    if (refOn_) {
      if (ref_) mfmref::noteOff(*ref_, id);
      return;
    }
    if (poly_ == 1) {
      stack_.remove(id);
      if (id == monoId_) {
        if (const auto* top = stack_.top()) {
          // Back to the previous held note.
          for (Voice& v : voices_)
            if (v.on && v.id == id) {
              v.id = top->id;
              if (porta_) v.glide.start(v.pitch, top->pitch, portaTime_, sr_);
              else v.pitch = top->pitch;
            }
          monoId_ = top->id;
          return;
        }
      }
    }
    for (Voice& v : voices_)
      if (v.on && v.id == id) v.released = true;
  }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (Voice& v : voices_)
      if (v.on && v.id == id) v.glide.start(v.pitch, pitch, seconds, sr_);
  }
  void onAllOff() override {
    if (ref_) mfmref::allOff(*ref_);
    for (Voice& v : voices_) v.released = true;
    stack_.clear();
  }
  void onKill() override {
    if (ref_) mfmref::kill(*ref_);
    for (Voice& v : voices_) v.on = false;
    stack_.clear();
    clearFx();
  }

  void render(float* L, float* R, int n) override {
    if (refOn_) {
      if (ref_) mfmref::render(*ref_, L, R, n);
      return;
    }
    // Chunks sit on the absolute sample grid (J-30: block-size independent smoothing).
    for (int i = 0; i < n;) {
      const int k = std::min(n - i, kChunk - static_cast<int>(static_cast<int64_t>(clock_) % kChunk));
      renderChunk(L + i, R + i, k);
      i += k;
    }
  }

 private:
  // ------------------------------------------------------------- reference model
  // UI thread: (re)builds the engine when the patch's structure or the rate changed.
  void prepareRef(const PluginState& s, int rate) {
    mfmref::Patch patch = mfmref::patchFromState(s);
    auto spec = std::make_unique<mfmref::LiveSpec>(mfmref::liveSpec(s));
    if (built_ && builtRate_ == rate && mfmref::sameStructure(*built_, patch)) {
      delete specPending_.exchange(spec.release());
      return;
    }
    double tempo = lastTempo_.load(std::memory_order_relaxed);
    if (!(tempo > 1)) tempo = 120;
    mfmref::EnginePtr e = mfmref::makeEngine(patch, rate, tempo);
    built_ = std::make_unique<mfmref::Patch>(std::move(patch));
    builtRate_ = rate;
    delete specPending_.exchange(spec.release());
    delete pending_.exchange(new Pack{std::move(e)});
  }
  // Audio thread: adopts a new engine / live map, then the live values.
  void configureRef(const PluginState& s, const ProcessContext& ctx) {
    if (Pack* pk = pending_.exchange(nullptr)) {
      mfmref::Engine* old = ref_;
      ref_ = pk->engine.release();
      pk->engine.reset(old);  // retired with its pack (freed on the UI thread)
      retire(pk);
    }
    if (mfmref::LiveSpec* sp = specPending_.exchange(nullptr)) {
      std::swap(sp, spec_);
      retireSpec(sp);
    }
    if (!ref_ && !prepared_.load(std::memory_order_acquire)) {  // used outside the engine (never prepared): build now
      prepareRef(s, static_cast<int>(std::lround(ctx.sampleRate)));
      configureRef(s, ctx);
      return;
    }
    if (!ref_) return;
    mfmref::setTempo(*ref_, ctx.tempo);
    if (spec_) {
      mfmref::liveFromParams(*spec_, s.params.data(), s.params.size(), live_);
      mfmref::applyLive(*ref_, live_);
    }
  }
  struct Pack {
    mfmref::EnginePtr engine;
  };
  void retire(Pack* p) {
    if (!p) return;
    for (auto& r : retired_) {
      Pack* expect = nullptr;
      if (r.compare_exchange_strong(expect, p)) return;
    }
    retiredLeak_ = p;  // every slot taken (never seen): kept until the instance goes
  }
  void retireSpec(mfmref::LiveSpec* sp) {
    if (!sp) return;
    for (auto& r : retiredSpec_) {
      mfmref::LiveSpec* expect = nullptr;
      if (r.compare_exchange_strong(expect, sp)) return;
    }
  }

  // ------------------------------------------------------------- notes
  void start(uint32_t id, double pitch, float vel, const NoteProps& p) {
    const float px = p.modX, py = p.modY;
    if (poly_ == 1) {
      stack_.push(id, pitch, vel);
      bool any = false;
      for (Voice& v : voices_)
        if (v.on && !v.released) any = true;
      if (any && porta_) {
        for (Voice& v : voices_)
          if (v.on && !v.released) {
            v.id = id;
            v.glide.start(v.pitch, pitch, portaTime_, sr_);
          }
        monoId_ = id;
        return;
      }
      for (Voice& v : voices_)
        if (v.on) v.on = false;
      monoId_ = id;
    } else {
      if (monoKey_)
        for (Voice& v : voices_)
          if (v.on && std::fabs(v.pitch - pitch) < 0.01) v.on = false;
      // Polyphony: release the oldest notes beyond the limit.
      int notes = 0;
      for (const Voice& v : voices_)
        if (v.on && !v.released && v.uni == 0) ++notes;
      if (notes >= poly_) {
        uint64_t oldest = UINT64_MAX;
        uint32_t oid = 0;
        for (const Voice& v : voices_)
          if (v.on && !v.released && v.order < oldest) oldest = v.order, oid = v.id;
        for (Voice& v : voices_)
          if (v.on && v.id == oid) v.released = true;
      }
    }
    const float rnd = random_ ? static_cast<float>(0.5 + 0.5 * hashUnit(static_cast<uint32_t>(++noteCount_) * 2654435761u)) : 0.5f;
    double gsum = 0;
    for (int u = 0; u < uniN_; ++u) gsum += uniGain(u) * uniGain(u);
    const float norm = static_cast<float>(1.0 / std::sqrt(std::max(1e-9, gsum)));
    for (int u = 0; u < uniN_; ++u) {
      Voice& v = alloc();
      init(v, id, pitch, vel, u, rnd, px, py);
      v.panOfs += p.pan;
      v.gain = static_cast<float>(uniGain(u)) * norm;
    }
  }

  double uniPosOf(int u) const { return uniN_ == 1 ? 0.0 : (u / double(uniN_ - 1)) * 2.0 - 1.0; }
  double uniGain(int u) const {
    if (uniN_ == 1) return 1.0;
    if (uniSub_ > 0 && u == uniN_ - 1) return uniSub_;
    return std::fabs(uniPosOf(u)) < 0.34 ? 1.0 : uniVol_;
  }

  Voice& alloc() {
    for (Voice& v : voices_)
      if (!v.on) return v;
    Voice* best = &voices_[0];
    for (Voice& v : voices_)
      if ((v.released && !best->released) || (v.released == best->released && v.order < best->order)) best = &v;
    return *best;
  }

  void init(Voice& v, uint32_t id, double pitch, float vel, int u, float rnd, float px, float py) {
    v.on = true, v.released = false, v.id = id, v.order = ++orderCount_, v.uni = u;
    v.pitch = pitch, v.vel = vel, v.rnd = rnd, v.x = px, v.y = py;
    v.glide = PitchGlide{};
    const double pos = uniPosOf(u);
    v.uniPos = static_cast<float>(pos);
    v.uniSemis = pos * uniPitch_ * 0.6;
    if (uniSub_ > 0 && uniN_ > 1 && u == uniN_ - 1) v.uniSemis = -12;
    v.panOfs = static_cast<float>(pos * uniPan_);
    v.envScaleT = 1.0 + pos * uniEnv_ * 0.25;
    v.age = 0, v.ctl = 0, v.releaseGate = 1;
    v.attackFade = soften_ > 0 ? soften_ * (1.0 - vel) * 0.06 : 0.0;
    v.noise = 0x9E3779B9u ^ (static_cast<uint32_t>(v.order) * 747796405u);
    for (int k = 0; k < kOps; ++k) {
      const OpCfg& o = op_[k];
      double ph0 = o.phase + pos * uniPhase_ * 0.5;
      if (o.global) ph0 += std::fmod(clock_ * dsp::noteToHz(pitch) * o.ratio / sr_, 1.0);
      v.ph[k] = ph0 - std::floor(ph0);
      v.prev[k] = 0, v.lp[k] = 0, v.dcX[k] = v.dcY[k] = 0, v.cyc[k] = 0, v.amp[k] = 0, v.ampTo[k] = 0;
      v.pkPos[k] = 0, v.pkPrev[k] = 0;
      if (v.pk) std::fill(v.pk + static_cast<size_t>(k) * kPluck, v.pk + static_cast<size_t>(k + 1) * kPluck, 0.f);
    }
    for (auto& f : v.flt) f.reset();
    for (float& y : v.yF) y = 0;
    for (int un = 0; un < kUnits; ++un)
      for (int a = 0; a < unitArts(un); ++a) {
        envStart(v.env[un][a], *art_[un][a].envC);
        v.lfoPh[un][a] = art_[un][a].lfoPhase;
      }
    v.mainVol = 0;
    control(v, 0.0);
    v.mainVol = v.attackFade > 0 ? 0.f : v.mainVolTo;
    for (int k = 0; k < kOps; ++k) {
      v.amp[k] = op_[k].declick ? 0.f : v.ampTo[k];
      v.pkFill[k] = op_[k].pluck ? v.pkLen[k] : 0;
    }
  }

  // Articulator value of unit u, target a (neutral 1, or 0.5 when bipolar).
  float artValue(Voice& v, int u, int a, double dt) {
    const ArtCfg& c = art_[u][a];
    const double neutral = c.bip ? 0.5 : 1.0;
    if (!c.any) return static_cast<float>(neutral);
    double b = neutral;
    if (c.env) b = envStep(v.env[u][a], c, *c.envC, dt, v.released, v.envScaleT);
    if (c.lfo) {
      double ph;
      if (c.global) ph = clock_ / sr_ * c.lfoHz + c.lfoPhase;
      else ph = v.lfoPh[u][a] += dt * c.lfoHz;
      b += c.lfoAmt * (lfoAt(c, ph) - 0.5);
    }
    const double mx = v.x != 0.5f ? v.x : sx_, my = v.y != 0.5f ? v.y : sy_;
    const double xs[kMaps] = {v.pitch / 127.0, v.vel, mx, my, v.rnd, 0.5 + 0.5 * v.uniPos};
    for (int m = 0; m < kMaps; ++m) {
      const Curve* mc = c.maps[static_cast<size_t>(m)];
      if (!mc) continue;
      const double mv = curveAt(*mc, std::clamp(xs[m], 0.0, 1.0) * curveLength(*mc));
      if (c.bip) b += mv - 0.5;
      else b *= mv;
    }
    double out;
    if (c.bip) out = 0.5 + c.amount * (b - 0.5);
    else out = c.amount >= 0 ? 1.0 - c.amount + c.amount * b : 1.0 + c.amount * b;
    return static_cast<float>(out);
  }

  // Control-rate update (articulators → per-voice targets).
  void control(Voice& v, double dt) {
    for (int u = 0; u < kUnits; ++u)
      for (int a = 0; a < unitArts(u); ++a) v.art[u][a] = artValue(v, u, a, dt);
    const double base = v.pitch;
    const double gpitch = globalPitch_ ? (v.art[0][OaPitch] - 0.5) * 2.0 * op_[0].range : 0.0;
    const float vel = 0.15f + 0.85f * v.vel;
    for (int k = 0; k < kOps; ++k) {
      const OpCfg& o = op_[k];
      const double p = base;
      const double semis = (k == 0 && globalPitch_) ? 0.0 : (v.art[k][OaPitch] - 0.5) * 2.0 * o.range;
      const double hz = dsp::noteToHz(p + mainPitch_ + v.uniSemis + semis + gpitch) * o.ratio + o.freq;
      v.inc[k] = std::clamp(hz / sr_, -0.49, 0.49);
      v.ampTo[k] = o.vol * v.art[k][OaVol];
      v.modAmt[k] = v.art[k][OaMod];
      v.phOfs[k] = static_cast<float>(v.art[k][OaPhase] - 0.5);
      v.opPan[k] = static_cast<float>((v.art[k][OaPan] - 0.5) * 2.0);
      const double fc = std::fabs(hz) * std::exp2(o.filter * 8.0 + (v.art[k][OaDamp] - 1.0) * 8.0);
      v.lpA[k] = o.filter >= 0.999f && v.art[k][OaDamp] >= 0.999f ? 0.0 : std::exp(-dsp::kTwoPi * std::clamp(fc, 10.0, sr_ * 0.45) / sr_);
      if (o.pluck) {
        v.pkLen[k] = std::clamp(static_cast<int>(sr_ / std::max(1.0, std::fabs(hz))), 2, kPluck - 1);
        v.pkG[k] = 0.990 + 0.0099 * std::clamp(static_cast<double>(v.art[k][OaDamp]), 0.0, 1.0);
      }
    }
    for (int j = 0; j < kFilters; ++j) {
      const int u = kOps + j;
      const FltCfg& f = flt_[j];
      const double cutPos = std::clamp(f.cut + f.env * (v.art[u][FaCut] - 0.5), 0.0, 1.0);
      v.fHz[j] = 20.0 * std::pow(1000.0, cutPos);
      v.fRes[j] = std::clamp(f.res * 2.0 * v.art[u][FaRes], 0.0, 1.0);
      v.fLow[j] = f.low * v.art[u][FaLow];
      v.fBand[j] = f.band * v.art[u][FaBand];
      v.fHigh[j] = f.high * v.art[u][FaHigh];
      v.fVol[j] = v.art[u][FaVol];
      v.fPan[j] = static_cast<float>((v.art[u][FaPan] - 0.5) * 2.0);
      v.fWs[j] = f.wsMix * v.art[u][FaWsMix];
    }
    const int M = kUnits - 1;
    v.mainVolTo = v.art[M][MaVol] * vel * v.gain;
    v.mainPan = static_cast<float>((v.art[M][MaPan] - 0.5) * 2.0);
  }

  bool voiceDone(const Voice& v) const {
    const int M = kUnits - 1;
    if (art_[M][MaVol].env) return v.env[M][MaVol].done && (v.mainVol < 1e-4f || v.releaseGate <= 0.0);
    return v.released && v.releaseGate <= 0.0;
  }

  float opSample(Voice& v, int k, double phase) {
    const OpCfg& o = op_[k];
    double p = phase - std::floor(phase);
    if (o.skew > 0) {
      const double m = 0.5 + 0.48 * o.skew;
      p = p < m ? 0.5 * p / m : 0.5 + 0.5 * (p - m) / (1 - m);
    }
    if (o.tension != 0) p = std::pow(p, std::exp2(o.tension * 2.0));
    const double inc = std::fabs(v.inc[k]);
    int lvl = 0;
    if (o.bandLimit)
      while (lvl < kLevels - 1 && (256 >> lvl) * inc > 0.45) ++lvl;
    double w;
    if (aux_->sine[static_cast<size_t>(k)]) w = std::sin(dsp::kTwoPi * p);
    else w = lookup(aux_->tab[static_cast<size_t>(k)].level(lvl), p);
    if (o.shape > 0) {
      const double s1 = std::min(1.0f, o.shape), s2 = std::max(0.0f, o.shape - 1);
      const double saw = lookup(sawTable().level(lvl), p);
      w = w + (saw - w) * s1;
      if (s2 > 0) w = w + (lookup(squareTable().level(lvl), p) - w) * s2;
    }
    if (o.sine > 0) w = w + (std::sin(w * dsp::kPi * 0.5 * (1.0 + 3.0 * o.sine)) - w) * o.sine;
    if (o.half && p >= 0.5) w = 0;
    if (o.absv) w = std::fabs(w) * 2.0 - 1.0;
    if (o.even && (v.cyc[k] & 1u)) w = 0;
    if (o.noise > 0) {
      v.noise = v.noise * 1664525u + 1013904223u;
      const double nz = static_cast<double>(v.noise >> 8) / 8388608.0 - 1.0;
      w = w * (1.0 - o.noise) + nz * o.noise;
    }
    return static_cast<float>(w);
  }

  void renderVoice(Voice& v, float* dL, float* dR, float* fL, float* fR, int n) {
    float y[kRows] = {};
    for (int i = 0; i < n; ++i) {
      if (v.ctl <= 0) {
        control(v, kCtl / sr_);
        v.ctl = kCtl;
      }
      --v.ctl;
      if (v.glide.active()) v.glide.advance(v.pitch);
      // Operators.
      for (int k = 0; k < kOps; ++k) {
        if (!used_[k]) {
          y[k] = 0;
          continue;
        }
        v.amp[k] += (v.ampTo[k] - v.amp[k]) * (op_[k].declick && v.age < 0.004 ? 0.02f : 0.25f);
        double mod = 0;
        for (int q = 0; q < nzFm_[k]; ++q) {
          const int j = fmList_[k][q];
          mod += fm_[k][j] * (j < k ? y[j] : v.prev[j]);
        }
        double w = opSample(v, k, v.ph[k] + mod * v.modAmt[k] + v.phOfs[k]);
        if (nzRm_[k]) {
          double ring = 0, rs = 0;
          for (int q = 0; q < nzRm_[k]; ++q) {
            const int j = rmList_[k][q];
            ring += rm_[k][j] * (j < k ? y[j] : v.prev[j]);
            rs += std::fabs(rm_[k][j]);
          }
          w = w * (1.0 - std::min(1.0, rs)) + w * ring;
        }
        if (v.lpA[k] > 0) w = v.lp[k] = w + (v.lp[k] - w) * v.lpA[k];
        if (op_[k].pluck && v.pk) {
          float* buf = v.pk + static_cast<size_t>(k) * kPluck;
          int& pos = v.pkPos[k];
          const int len = v.pkLen[k];
          int rd = pos - len;
          if (rd < 0) rd += kPluck;
          const double d = buf[rd];
          // Plucked mode: one period of the waveform excites a string loop
          // (delay of one period, averaging low-pass, loss per pass) that
          // then rings on its own. Exciting it continuously instead piles up
          // energy at the fundamental (tens of times the waveform's level).
          double out;
          if (v.pkFill[k] > 0) {
            --v.pkFill[k];
            out = w;
          } else {
            out = 0.5 * (d + v.pkPrev[k]) * v.pkG[k];
          }
          v.pkPrev[k] = d;
          buf[pos] = static_cast<float>(out);
          pos = (pos + 1) % kPluck;
          w = out;
        }
        if (op_[k].center) {
          const double o = w - v.dcX[k] + 0.995 * v.dcY[k];
          v.dcX[k] = w, v.dcY[k] = o;
          w = o;
        }
        y[k] = static_cast<float>(w * v.amp[k]);
        v.ph[k] += v.inc[k];
        if (v.ph[k] >= 1.0) v.ph[k] -= 1.0, ++v.cyc[k];
        else if (v.ph[k] < 0.0) v.ph[k] += 1.0;
      }
      for (int k = 0; k < kOps; ++k) v.prev[k] = y[k];
      // Filters.
      for (int j = 0; j < kFilters; ++j) {
        const int r = kOps + j;
        if (!used_[r]) {
          y[r] = 0;
          continue;
        }
        double in = 0;
        for (int k = 0; k < kOps; ++k) in += fm_[r][k] * y[k];
        if (j > 0) in += flt_[j - 1].next * y[r - 1];
        double o = runFilter(v.flt[j], flt_[j], sr_, in, v.fHz[j], v.fRes[j], v.fLow[j], v.fBand[j], v.fHigh[j]);
        if (flt_[j].ws && v.fWs[j] > 0) o = o + (waveshape(flt_[j], o * flt_[j].wsAmp) - o) * v.fWs[j];
        if (!std::isfinite(o)) o = 0, v.flt[j].reset();
        y[r] = static_cast<float>(o * v.fVol[j]);
      }
      // Main volume (with soften fade and release gate).
      double mv = v.mainVolTo;
      if (v.attackFade > 0 && v.age < v.attackFade) mv *= v.age / v.attackFade;
      v.mainVol += static_cast<float>((mv - v.mainVol) * 0.05);
      if (v.released && (!art_[kUnits - 1][MaVol].env || v.env[kUnits - 1][MaVol].done)) v.releaseGate -= 1.0 / (0.01 * sr_);
      const float g = v.mainVol * static_cast<float>(std::max(0.0, std::min(1.0, v.releaseGate)));
      float sl = 0, sr = 0, xl = 0, xr = 0;
      for (int r = 0; r < kRows; ++r) {
        if (y[r] == 0) continue;
        const float out = fm_[r][kColOut], fx = fm_[r][kColFx];
        if (out == 0 && fx == 0) continue;
        float pan = fm_[r][kColPan] + (r < kOps ? v.opPan[r] : v.fPan[r - kOps]) + v.panOfs + v.mainPan;
        pan = std::clamp(pan, -1.f, 1.f);
        const float gl = std::min(1.f, 1.f - pan), gr = std::min(1.f, 1.f + pan);
        sl += y[r] * out * gl, sr += y[r] * out * gr;
        xl += y[r] * fx * gl, xr += y[r] * fx * gr;
      }
      dL[i] += sl * g, dR[i] += sr * g;
      fL[i] += xl * g, fR[i] += xr * g;
      v.age += 1.0 / sr_;
      if (voiceDone(v)) {  // on the voice's own clock, not at a block end (J-30)
        v.on = false;
        break;
      }
    }
  }

  // ------------------------------------------------------------ effects
  static constexpr int kDelayLen = 1 << 18;
  static constexpr int kChorusLen = 8192;
  static constexpr int kPreLen = 1 << 17;
  static constexpr int kCombMax = 12000, kApMax = 3000;
  static constexpr int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
  static constexpr int kAp[4] = {556, 441, 341, 225};

  struct Dly {
    float* buf[2] = {nullptr, nullptr};  // kDelayLen each (null until allocated, see prepareHost)
    int w = 0;
    bool on = false;
    int mode = 0;
    double fb = 0.5, tL = 1000, tR = 1000, vol = 0.5, lpL = 0, lpR = 0;
    void clear() {
      if (buf[0]) std::fill(buf[0], buf[0] + kDelayLen, 0.f), std::fill(buf[1], buf[1] + kDelayLen, 0.f);
      lpL = lpR = 0;
    }
    float read(int c, double t) const {
      double rp = w - t;
      while (rp < 0) rp += kDelayLen;
      const int i0 = static_cast<int>(rp);
      const double f = rp - i0;
      const auto& b = buf[c];
      return static_cast<float>(b[static_cast<size_t>(i0 & (kDelayLen - 1))] * (1 - f) + b[static_cast<size_t>((i0 + 1) & (kDelayLen - 1))] * f);
    }
    void run(float& l, float& r) {
      const double dl = read(0, tL), dr = read(1, tR);
      lpL = dl + (lpL - dl) * 0.3, lpR = dr + (lpR - dr) * 0.3;
      const double s = mode == 1 ? -fb : fb;
      if (mode == 2) {
        buf[0][static_cast<size_t>(w)] = static_cast<float>((l + r) * 0.5 + lpR * s);
        buf[1][static_cast<size_t>(w)] = static_cast<float>(lpL * s);
      } else {
        buf[0][static_cast<size_t>(w)] = static_cast<float>(l + lpL * s);
        buf[1][static_cast<size_t>(w)] = static_cast<float>(r + lpR * s);
      }
      w = (w + 1) & (kDelayLen - 1);
      l = static_cast<float>(dl * vol), r = static_cast<float>(dr * vol);
    }
  };
  struct Comb {
    float* buf = nullptr;  // kCombMax
    int len = 2, pos = 0;
    double fb = 0.8, store = 0;
    double run(double in, double damp) {
      if (pos >= len) pos = 0;
      const double y = buf[static_cast<size_t>(pos)];
      store = y * (1 - damp) + store * damp;
      if (std::fabs(store) < 1e-20) store = 0;
      buf[static_cast<size_t>(pos)] = static_cast<float>(in + store * fb);
      if (++pos >= len) pos = 0;
      return y;
    }
  };
  struct Ap {
    float* buf = nullptr;  // kApMax
    int len = 2, pos = 0;
    double run(double in, double g) {
      if (pos >= len) pos = 0;
      const double b = buf[static_cast<size_t>(pos)];
      const double out = b - in * g;
      buf[static_cast<size_t>(pos)] = static_cast<float>(in + b * g);
      if (++pos >= len) pos = 0;
      return out;
    }
  };

  void clearReverb() {
    for (auto& ch : comb_)
      for (auto& c : ch) {
        if (c.buf) std::fill(c.buf, c.buf + kCombMax, 0.f);
        c.store = 0;
      }
    for (auto& ch : ap_)
      for (auto& a : ch)
        if (a.buf) std::fill(a.buf, a.buf + kApMax, 0.f);
    for (auto& b : preBuf_)
      if (b) std::fill(b, b + kPreLen, 0.f);
  }
  // Lazily allocated memory (J-30): allocated on the UI thread (prepareHost),
  // published through an atomic pointer, never freed before the instance.
  struct LazyMem {
    std::unique_ptr<float[]> mem;
    std::atomic<float*> ptr{nullptr};
    void ensure(size_t n) {
      if (mem) return;
      mem.reset(new float[n]());
      ptr.store(mem.get(), std::memory_order_release);
    }
    float* get() const { return ptr.load(std::memory_order_acquire); }
  };
  static constexpr size_t kRvFloats = 2 * (8 * static_cast<size_t>(kCombMax) + 4 * static_cast<size_t>(kApMax) + static_cast<size_t>(kPreLen));
  // Audio thread: points the sections at the memory allocated so far.
  void bindMemory() {
    for (int i = 0; i < 3; ++i) {
      float* m = dlyMem_[i].get();
      dly_[i].buf[0] = m;
      dly_[i].buf[1] = m ? m + kDelayLen : nullptr;
    }
    float* r = rvMem_.get();
    for (int c = 0; c < 2; ++c) {
      for (int k = 0; k < 8; ++k) comb_[c][k].buf = r ? r + (static_cast<size_t>(c) * 8 + static_cast<size_t>(k)) * kCombMax : nullptr;
      float* aps = r ? r + 16 * static_cast<size_t>(kCombMax) : nullptr;
      for (int k = 0; k < 4; ++k) ap_[c][k].buf = aps ? aps + (static_cast<size_t>(c) * 4 + static_cast<size_t>(k)) * kApMax : nullptr;
      preBuf_[c] = r ? r + 16 * static_cast<size_t>(kCombMax) + 8 * static_cast<size_t>(kApMax) + static_cast<size_t>(c) * kPreLen : nullptr;
    }
    float* pk = pluckMem_.get();
    for (int i = 0; i < kMaxVoices; ++i)
      voices_[static_cast<size_t>(i)].pk = pk ? pk + static_cast<size_t>(i) * kOps * kPluck : nullptr;
  }
  void clearFx() {
    for (auto& d : dly_) d.clear();
    for (auto& b : chorusBuf_) std::fill(b.begin(), b.end(), 0.f);
    clearReverb();
    tailLeft_ = 0;
  }

  void runFx(float* L, float* R, int n) {
    if (chN_ > 0) {
      const double base = (0.001 + chDelay_ * 0.03) * sr_, depth = chDepth_ * 0.006 * sr_;
      const double inc = (0.05 + chSpeed_ * 3.0) / sr_;
      for (int i = 0; i < n; ++i) {
        chorusBuf_[0][static_cast<size_t>(chW_)] = L[i];
        chorusBuf_[1][static_cast<size_t>(chW_)] = R[i];
        double wl = 0, wr = 0;
        for (int k = 0; k < chN_; ++k) {
          const double ph = chPh_ + k / double(chN_);
          const double d0 = base + depth * (0.5 + 0.5 * std::sin(dsp::kTwoPi * ph));
          const double d1 = base + depth * (0.5 + 0.5 * std::sin(dsp::kTwoPi * (ph + 0.25 * chSpread_)));
          wl += readChorus(0, d0), wr += readChorus(1, d1);
        }
        wl /= chN_, wr /= chN_;
        const double cl = wl + chCross_ * wr, cr = wr + chCross_ * wl;
        L[i] = static_cast<float>(L[i] + cl * chVol_);
        R[i] = static_cast<float>(R[i] + cr * chVol_);
        chW_ = (chW_ + 1) & (kChorusLen - 1);
        chPh_ += inc;
        if (chPh_ >= 1) chPh_ -= 1;
      }
    }
    bool anyD = false;
    for (const auto& d : dly_) anyD = anyD || (d.on && d.buf[0]);
    if (anyD)
      for (int i = 0; i < n; ++i) {
        float l = L[i], r = R[i];
        if (dlSerial_) {
          for (auto& d : dly_)
            if (d.on && d.buf[0]) {
              float a = l, b = r;
              d.run(a, b);
              l += a, r += b;
            }
          L[i] = l, R[i] = r;
        } else {
          float al = 0, ar = 0;
          for (auto& d : dly_)
            if (d.on && d.buf[0]) {
              float a = l, b = r;
              d.run(a, b);
              al += a, ar += b;
            }
          L[i] = l + al, R[i] = r + ar;
        }
      }
    if (rvOn_ && preBuf_[0])
      for (int i = 0; i < n; ++i) {
        preBuf_[0][static_cast<size_t>(preW_)] = L[i];
        preBuf_[1][static_cast<size_t>(preW_)] = R[i];
        int rp = preW_ - rvPre_;
        if (rp < 0) rp += kPreLen;
        const double in = (preBuf_[0][static_cast<size_t>(rp)] + preBuf_[1][static_cast<size_t>(rp)]) * 0.015;
        preW_ = (preW_ + 1) & (kPreLen - 1);
        double out[2] = {0, 0};
        for (int c = 0; c < 2; ++c) {
          for (int k = 0; k < 8; ++k) out[c] += comb_[c][k].run(in, rvDamp_);
          for (int k = 0; k < 4; ++k) out[c] = ap_[c][k].run(out[c], rvDiff_);
          out[c] = rvHc_[c].process(out[c]);
          out[c] -= rvLcz_[c] = rvLc_[c].process(out[c]);
        }
        L[i] += static_cast<float>(out[0] * rvWet_ * 1.4);
        R[i] += static_cast<float>(out[1] * rvWet_ * 1.4);
      }
  }
  double readChorus(int c, double d) const {
    double rp = chW_ - d;
    while (rp < 0) rp += kChorusLen;
    const int i0 = static_cast<int>(rp);
    const double f = rp - i0;
    const auto& b = chorusBuf_[c];
    return b[static_cast<size_t>(i0 & (kChorusLen - 1))] * (1 - f) + b[static_cast<size_t>((i0 + 1) & (kChorusLen - 1))] * f;
  }
  void runEq(float* L, float* R, int n) {
    for (int b = 0; b < 3; ++b) {
      if (!eqOn_[b]) continue;
      for (int i = 0; i < n; ++i) {
        L[i] = static_cast<float>(eqf_[b][0].process(L[i]));
        R[i] = static_cast<float>(eqf_[b][1].process(R[i]));
      }
    }
  }

  void renderChunk(float* L, float* R, int n) {
    float dL[kChunk] = {}, dR[kChunk] = {}, fL[kChunk] = {}, fR[kChunk] = {};
    if (smooth_) {
      const float k = static_cast<float>(1.0 - std::exp(-n / (0.03 * sr_)));
      sx_ += (modX_ - sx_) * k, sy_ += (modY_ - sy_) * k;
    } else {
      sx_ = modX_, sy_ = modY_;
    }
    bool any = false;
    for (Voice& v : voices_)
      if (v.on) {
        renderVoice(v, dL, dR, fL, fR, n);
        any = true;
      }
    const bool fxActive = chN_ > 0 || rvOn_ || dly_[0].on || dly_[1].on || dly_[2].on;
    if (any) tailLeft_ = static_cast<int64_t>(sr_ * 8);
    else if (tailLeft_ > 0) tailLeft_ -= n;
    if (!fxActive && !any) tailLeft_ = 0;
    if (fxActive) {
      if (eqTarget_ == 3) runEq(fL, fR, n);
      runFx(fL, fR, n);
      if (fxPan_ != 0) {
        const float gl = std::min(1.f, 1.f - fxPan_), gr = std::min(1.f, 1.f + fxPan_);
        for (int i = 0; i < n; ++i) fL[i] *= gl, fR[i] *= gr;
      }
    }
    if (eqTarget_ == 2) runEq(dL, dR, n);
    for (int i = 0; i < n; ++i) dL[i] += fL[i], dR[i] += fR[i];
    if (eqTarget_ == 1) runEq(dL, dR, n);
    const float g = mainVol_ * 0.5f;
    for (int i = 0; i < n; ++i) {
      double l = dL[i] * g, r = dR[i] * g;
      if (center_) l = dcL_.process(l), r = dcR_.process(r);
      L[i] += static_cast<float>(l);
      R[i] += static_cast<float>(r);
    }
    clock_ += n;
  }

  // ------------------------------------------------------------ state
  double sr_ = 44100, tempo_ = 140;
  const mfm::MfmAux* aux_ = nullptr;
  std::shared_ptr<mfm::MfmAux> fallback_;
  float fm_[kRows][kCols] = {}, rm_[kRows][kCols] = {};
  bool used_[kRows] = {};
  int fmList_[kOps][kOps] = {}, rmList_[kOps][kOps] = {}, nzFm_[kOps] = {}, nzRm_[kOps] = {};
  OpCfg op_[kOps];
  FltCfg flt_[kFilters];
  ArtCfg art_[kUnits][kFltArts];
  float mainVol_ = 1, mainPitch_ = 0, lfoLevel_ = 1;
  int uniN_ = 1;
  float uniPan_ = 1, uniVol_ = 0.78f, uniPitch_ = 0.5f, uniSub_ = 0, uniPhase_ = 0.125f, uniEnv_ = 0;
  float modX_ = 0.5f, modY_ = 0.5f, sx_ = 0.5f, sy_ = 0.5f;
  bool porta_ = false, monoKey_ = false, globalPitch_ = false, center_ = false, random_ = true, smooth_ = false;
  float portaTime_ = 0.08f, soften_ = 0;
  int poly_ = 16;
  uint32_t monoId_ = 0;
  NoteStack stack_;
  uint64_t noteCount_ = 0, orderCount_ = 0;
  double clock_ = 0;
  std::array<Voice, kMaxVoices> voices_{};
  LazyMem pluckMem_, dlyMem_[3], rvMem_;  // see prepareHost / bindMemory
  std::atomic<bool> prepared_{false};
  // Reference model (J-50).
  bool refOn_ = false;
  mfmref::Engine* ref_ = nullptr;  // audio thread
  mfmref::LiveSpec* spec_ = nullptr;
  mfmref::Live live_;
  std::atomic<Pack*> pending_{nullptr};
  std::atomic<mfmref::LiveSpec*> specPending_{nullptr};
  std::atomic<Pack*> retired_[4] = {};
  std::atomic<mfmref::LiveSpec*> retiredSpec_[4] = {};
  Pack* retiredLeak_ = nullptr;
  std::unique_ptr<mfmref::Patch> built_;  // UI thread
  int builtRate_ = 0;
  std::atomic<double> lastTempo_{120.0};
  int64_t tailLeft_ = 0;
  // effects
  float fxPan_ = 0;
  int chN_ = 0, chW_ = 0;
  float chDepth_ = 0.5f, chSpeed_ = 0.75f, chDelay_ = 0.06f, chSpread_ = 1, chCross_ = -0.25f, chVol_ = 1;
  double chPh_ = 0;
  std::vector<float> chorusBuf_[2];
  float* preBuf_[2] = {nullptr, nullptr};  // kPreLen each, in rvMem_
  bool dlSerial_ = false;
  Dly dly_[3];
  bool rvOn_ = false;
  double rvWet_ = 0.2, rvDamp_ = 0.3, rvDiff_ = 0.5, rvLcz_[2] = {0, 0};
  int rvPre_ = 0, preW_ = 0;
  Comb comb_[2][8];
  Ap ap_[2][4];
  dsp::OnePole rvHc_[2], rvLc_[2];
  int eqTarget_ = 0;
  bool eqOn_[3] = {};
  dsp::Biquad eqf_[3][2];
  dsp::DcBlock dcL_, dcR_;
};

}  // namespace

std::unique_ptr<Generator> makeMatrixFm() { return std::make_unique<MatrixFmGen>(); }

}  // namespace gl::packs::synths

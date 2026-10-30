// Gridline — "plasma" pack: the Plasma generator. Notes (poly / mono /
// legato / glide, voice priority, stealing), four envelopes, ten LFOs (drawn,
// path, chaos, sample & hold), the 64-slot modulation matrix, oscillator /
// filter routing with buses, the three effect racks with splitters, the
// arpeggiator and clip player, MPE / MIDI, tuning, oversampling, and the
// prepared data (aux) the runtime reads. Nothing here allocates on the audio
// thread: memory is made in the constructor and in prepareHost (UI thread).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include "../../core/Json.h"
#include "../Packs.h"
#include "plasma_dsp.h"

namespace gl::packs::plasma {

// ================================================================== parameter maps
namespace {
struct PMap {
  float lo = 0, hi = 1;
  uint8_t kind = 0;  // 0 linear, 1 log, 2 stepped (choice / toggle)
};
const std::vector<PMap>& pmaps() {
  static const std::vector<PMap> v = [] {
    std::vector<PMap> m(kParamCount);
    const PluginInfo* info = findPlugin(kType);
    if (!info) return m;
    for (int k = 0; k < kParamCount && k < static_cast<int>(info->params.size()); ++k) {
      const ParamDesc& d = info->params[static_cast<size_t>(k)];
      PMap& p = m[static_cast<size_t>(k)];
      p.lo = static_cast<float>(d.min);
      p.hi = static_cast<float>(d.kind == ParamKind::Choice ? static_cast<double>(d.choices.size() - 1) : d.max);
      p.kind = d.kind != ParamKind::Continuous ? 2 : d.scale == ParamScale::Log && d.min > 0 ? 1 : 0;
    }
    return m;
  }();
  return v;
}
inline float toNorm(const PMap& p, float v) {
  if (p.hi <= p.lo) return 0;
  if (p.kind == 1) return std::log(std::max(v, p.lo) / p.lo) / std::log(p.hi / p.lo);
  return (v - p.lo) / (p.hi - p.lo);
}
inline float fromNorm(const PMap& p, float n) {
  n = std::clamp(n, 0.f, 1.f);
  if (p.kind == 1) return p.lo * std::pow(p.hi / p.lo, n);
  const float v = p.lo + n * (p.hi - p.lo);
  return p.kind == 2 ? std::round(v) : v;
}

// ================================================================== aux cache
std::mutex& auxMu() {
  static std::mutex m;
  return m;
}
std::map<std::string, std::weak_ptr<const Aux>>& auxCache() {
  static std::map<std::string, std::weak_ptr<const Aux>> c;
  return c;
}
void sampleCurve(const Curve& c, float* out, int n, bool identity) {
  for (int i = 0; i < n; ++i) {
    const double x = static_cast<double>(i) / (n - 1);
    out[i] = c.empty() ? (identity ? static_cast<float>(x) : 0.f) : static_cast<float>(curveAt(c, x));
  }
}
std::array<double, 128> parseTun(const std::string& text, bool& ok) {
  // AnaMark .tun: [Exact Tuning] note N=cents (absolute from 8.1758 Hz) or [Tuning] note N=cents
  std::array<double, 128> t{};
  for (int i = 0; i < 128; ++i) t[static_cast<size_t>(i)] = i;
  ok = false;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    const size_t a = line.find("note");
    const size_t eq = line.find('=');
    if (a == std::string::npos || eq == std::string::npos || line[0] == ';') continue;
    const int note = std::atoi(line.c_str() + a + 4);
    const double cents = std::atof(line.c_str() + eq + 1);
    if (note < 0 || note > 127) continue;
    t[static_cast<size_t>(note)] = cents / 100.0;  // cents from MIDI note 0
    ok = true;
  }
  return t;
}
}  // namespace

FxInputHook gFxInputHook = nullptr;

std::shared_ptr<const Aux> buildAux(const PluginState& state) {
  (void)pmaps();
  (void)factoryMips();
  (void)factoryNoises();
  (void)fft2048();
  std::string key = state.data;
  {
    std::lock_guard<std::mutex> lk(auxMu());
    auto it = auxCache().find(key);
    if (it != auxCache().end())
      if (auto sp = it->second.lock()) return sp;
  }
  auto a = std::make_shared<Aux>();
  Model m;
  modelFromData(state.data, m);
  for (int o = 0; o < kOscs; ++o) {
    const OscData& d = m.osc[static_cast<size_t>(o)];
    OscAux& x = a->osc[static_cast<size_t>(o)];
    if (d.table == "custom" && !d.frames.empty()) {
      x.mip = buildMip(tableFromFrames(d.frames, d.tableName.empty() ? "Custom" : d.tableName));
    } else if (d.table.rfind("file:", 0) == 0) {
      auto t = loadTableFile(d.table.substr(5));
      if (t) x.mip = buildMip(t);
    }
    if (const std::string path = mediaFile(m, d.sample); !path.empty()) {
      x.sample = sampleCache().get(path);
      if (!x.sample) x.sample = loadSample(path);
      if (x.sample) {
        for (float sl : d.slices) x.slices.push_back(static_cast<double>(sl) * static_cast<double>(x.sample->frames()));
        // spectral analysis (limited to 30 s)
        auto sp = std::make_shared<SpecSrc>();
        const Sample& s = *x.sample;
        const size_t n = std::min<size_t>(s.frames(), static_cast<size_t>(s.sampleRate * 30));
        sp->rate = s.sampleRate;
        sp->frames = n > SpecSrc::kN ? static_cast<int>((n - SpecSrc::kN) / SpecSrc::kHop) + 1 : 1;
        sp->mag.assign(static_cast<size_t>(sp->frames) * SpecSrc::kBins, 0.f);
        std::vector<cpx> buf(SpecSrc::kN);
        float peak = 1e-9f;
        for (int f = 0; f < sp->frames; ++f) {
          for (int i = 0; i < SpecSrc::kN; ++i) {
            const size_t j = static_cast<size_t>(f) * SpecSrc::kHop + static_cast<size_t>(i);
            const float v = j < n ? (s.mono() ? s.left[j] : 0.5f * (s.left[j] + s.right[j])) : 0.f;
            const float w = 0.5f - 0.5f * std::cos(static_cast<float>(kPi2) * i / SpecSrc::kN);
            buf[static_cast<size_t>(i)] = cpx(v * w, 0);
          }
          fft2048().forward(buf.data());
          for (int b = 0; b < SpecSrc::kBins; ++b) {
            const float mg = std::abs(buf[static_cast<size_t>(b)]) * (4.0f / SpecSrc::kN);
            sp->mag[static_cast<size_t>(f) * SpecSrc::kBins + static_cast<size_t>(b)] = mg;
            peak = std::max(peak, mg);
          }
        }
        x.spec = sp;
      }
    }
    if (const std::string path = mediaFile(m, d.sfz); !path.empty()) parseSfz(path, x.zones, nullptr);
    for (int r = 0; r < 4; ++r) {
      const Curve& c = d.remap[static_cast<size_t>(r)];
      if (c.empty()) {
        // defaults: 1 = S-curve, 2 = mirror ramp, 3 = soft curve, 4 = ramp
        for (int i = 0; i <= 256; ++i) {
          const double t = i / 256.0;
          double v = t;
          if (r == 0) v = t * t * (3 - 2 * t);
          else if (r == 1) v = t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
          else if (r == 2) v = std::sin(t * 1.5707963);
          x.remap[static_cast<size_t>(r)][static_cast<size_t>(i)] = static_cast<float>(v);
        }
      } else {
        sampleCurve(c, x.remap[static_cast<size_t>(r)].data(), 257, true);
      }
    }
  }
  for (int l = 0; l < kLfos; ++l) {
    const LfoData& d = m.lfo[static_cast<size_t>(l)];
    LfoAux& x = a->lfo[static_cast<size_t>(l)];
    x.pts.assign(d.pts.begin(), d.pts.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(d.pts.size(), 128)));
    x.loop = d.loop;
    x.path.assign(d.path.begin(), d.path.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(d.path.size(), 128)));
    x.pathX.assign(d.pathX.begin(), d.pathX.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(d.pathX.size(), 128)));
  }
  a->factoryShapes.resize(lfoShapeNames().size());
  for (size_t i = 1; i < lfoShapeNames().size(); ++i) a->factoryShapes[i] = lfoFactoryShape(static_cast<int>(i));
  sampleCurve(m.velo, a->velo.data(), 129, true);
  sampleCurve(m.note, a->note.data(), 129, true);
  for (int s = 0; s < kModSlots; ++s) {
    a->hasModCurve[static_cast<size_t>(s)] = !m.modCurve[static_cast<size_t>(s)].empty();
    a->hasAuxCurve[static_cast<size_t>(s)] = !m.auxCurve[static_cast<size_t>(s)].empty();
    sampleCurve(m.modCurve[static_cast<size_t>(s)], a->modCurve[static_cast<size_t>(s)].data(), 65, true);
    sampleCurve(m.auxCurve[static_cast<size_t>(s)], a->auxCurve[static_cast<size_t>(s)].data(), 65, true);
  }
  a->busLinks = m.busLinks;
  for (int s = 0; s < kFxSlots; ++s) {
    const FxExtra& e = m.fxExtra[static_cast<size_t>(s)];
    Curve da = e.shapeA.empty() ? Curve{{0, 0, 0}, {0.5f, 0.5f, 0}, {1, 1, 0.5f}} : e.shapeA;
    Curve db = e.shapeB.empty() ? da : e.shapeB;
    sampleCurve(da, a->shapeA[static_cast<size_t>(s)].data(), 257, true);
    sampleCurve(db, a->shapeB[static_cast<size_t>(s)].data(), 257, true);
    if (!e.ir.empty() && e.ir.rfind("factory:", 0) != 0)
      if (const std::string path = mediaFile(m, e.ir); !path.empty()) a->irFile[static_cast<size_t>(s)] = loadSample(path);
  }
  if (const std::string path = mediaFile(m, m.noiseFile); !path.empty()) a->noiseFile = loadSample(path);
  if (!m.tuning.empty()) a->tuning = parseTun(m.tuning, a->hasTuning);
  else
    for (int i = 0; i < 128; ++i) a->tuning[static_cast<size_t>(i)] = i;
  a->model = std::move(m);
  std::lock_guard<std::mutex> lk(auxMu());
  auto& c = auxCache();
  for (auto it = c.begin(); it != c.end();) it = it->second.expired() ? c.erase(it) : std::next(it);
  c[key] = a;
  return a;
}

// ================================================================== the generator
namespace {

constexpr int kCtl = 16;             // control period (base-rate samples)
constexpr int kVoices = kMaxVoices + 8;  // extra slots for voices fading out after a steal
constexpr int kMaxOs = 4;

// Alternating unison mix (J-201, measured per voice on held notes against the import source):
// level per channel against a single voice, by blend in 0.1 steps, before the voice-count
// level. The centre voice fades as the blend rises; the side voices rise from silence and
// overtake it above about 0.6 blend.
constexpr float kAltCentre[11] = {1.f, 1.f, 0.977f, 0.955f, 0.912f, 0.861f, 0.804f, 0.733f, 0.646f, 0.550f, 0.447f};
constexpr float kAltSide[11] = {0.f, 0.11f, 0.234f, 0.385f, 0.572f, 0.794f, 1.023f, 1.245f, 1.413f, 1.496f, 1.479f};
constexpr float kAltPair = 2.53f;  // an even count's two middle voices (+8 dB over an odd centre)
float uniTab(const float (&t)[11], float blend) {
  const float x = std::clamp(blend, 0.f, 1.f) * 10.f;
  const int i = std::min(9, static_cast<int>(x));
  return t[i] + (t[i + 1] - t[i]) * (x - static_cast<float>(i));
}

// 2x decimator (31-tap halfband)
struct Decim {
  static constexpr int L = 31;
  float z[2 * L] = {};
  int p = 0;
  static const float* taps() {
    static const float* h = [] {
      static float t[L];
      double s = 0;
      for (int i = 0; i < L; ++i) {
        const int k = i - (L - 1) / 2;
        const double sinc = k == 0 ? 0.5 : std::sin(3.14159265358979 * k * 0.5) / (3.14159265358979 * k);
        const double w = 0.42 - 0.5 * std::cos(kPi2 * i / (L - 1)) + 0.08 * std::cos(2 * kPi2 * i / (L - 1));
        t[i] = static_cast<float>(sinc * w);
        s += t[i];
      }
      for (float& v : t) v = static_cast<float>(v / s);
      return t;
    }();
    return h;
  }
  float push2(float a, float b) {  // two input samples -> one output
    const float* h = taps();
    z[p] = z[p + L] = a;
    p = (p + 1) % L;
    z[p] = z[p + L] = b;
    p = (p + 1) % L;
    float acc = 0;
    for (int i = 0; i < L; ++i) acc += h[i] * z[p + i];
    return acc;
  }
  void reset() {
    std::fill(z, z + 2 * L, 0.f);
    p = 0;
  }
};

struct EnvState {
  int stage = 0;  // 0 idle, 1 attack, 2 hold, 3 decay, 4 sustain, 5 release
  double level = 0, from = 0, to = 0, t = 0, dt = 0, k = 0, ek = 1, mul = 1, norm = 1;
  double secs = 0;  // time in stage
};
struct EnvSet {
  double a, h, d, s, r, ca, cd, cr, start, end;
  bool steep;  // the source's curve law (T_EnvLaw)
};
// Stage curve constant from a curve knob (0..100, 50 = straight). Plasma's own law is k = 8 c (c = -1..1).
// J-203a, measured on the source (0.3 s attack, decay to zero and release, curves 0..100 in steps of
// 10, fitted with the same exponential stage shape): k = 9.5 atanh(c) for all three stages (fitted
// k at curve 60 / 70 / 80 / 90: 2.0 / 4.0 / 6.5 / 10.3; at 40 / 30 / 20 / 10: -2.2 / -4.2 / -6.7 / -10.9;
// at the ends about +-40 to 50, nearly a step). Its knob default is 60, not Plasma's 66.6.
inline double stageK(double curve, bool steep) {
  const double c = (curve - 50) / 50.0;
  if (!steep) return c * 8.0;
  return 9.5 * std::atanh(std::clamp(c, -0.9998, 0.9998));
}
void envStage(EnvState& e, int stage, const EnvSet& p, double srv) {
  e.stage = stage;
  e.t = 0;
  e.secs = 0;
  e.from = e.level;
  double time = 0, k = 0;
  switch (stage) {
    case 1: time = p.a, k = stageK(p.ca, p.steep), e.to = 1; break;
    case 2: time = p.h, k = 0, e.to = 1; break;
    case 3: time = p.d, k = stageK(p.cd, p.steep), e.to = p.s; break;
    case 4: e.to = p.s; e.dt = 0; return;
    case 5: time = p.r, k = stageK(p.cr, p.steep), e.to = p.end; break;
    default: e.to = 0, e.dt = 0; return;
  }
  e.dt = time <= 1e-5 ? 1.0 : 1.0 / (time * srv);
  e.k = k;
  if (std::fabs(e.k) < 1e-4) {
    e.k = 0;
    e.mul = 1;
    e.ek = 1;
    e.norm = 1;
  } else {
    e.ek = 1;
    e.mul = std::exp(-e.k * e.dt);
    e.norm = 1.0 / (1.0 - std::exp(-e.k));
  }
}
inline void envTick(EnvState& e, const EnvSet& p, double srv, bool held) {
  if (e.stage == 0 || e.stage == 4) {
    if (e.stage == 4) e.level = p.s;
    return;
  }
  e.t += e.dt;
  e.secs += 1.0 / srv;
  double shape;
  if (e.k == 0) {
    shape = std::min(1.0, e.t);
  } else {
    e.ek *= e.mul;
    shape = std::min(1.0, (1.0 - e.ek) * e.norm);
  }
  e.level = e.from + (e.to - e.from) * shape;
  if (e.t >= 1.0) {
    e.level = e.to;
    if (e.stage == 1) envStage(e, p.h > 1e-5 ? 2 : 3, p, srv);
    else if (e.stage == 2) envStage(e, 3, p, srv);
    else if (e.stage == 3) envStage(e, held ? 4 : 5, p, srv);
    else if (e.stage == 5) e.stage = 0, e.level = p.end;
  }
}

struct LfoState {
  double phase = 0, t = 0, out = 0, y = 0, sm = 0, smY = 0, sh = 0;
  double lx = 0.1, ly = 0, lz = 0;  // chaos
  bool done = false;
  int cycles = 0;
  uint32_t rng = 1;
};

// Settings of a note for one control period (computed on the control grid).
struct Ctl {
  OscSet os[kOscs];
  float lev[5] = {}, pl[5] = {}, pr[5] = {};
  bool subOn = false, noiseOn = false;
  int subShape = 0;
  double subInc = 0;
  const NoiseSrc* nsrc = nullptr;
  const Sample* nfile = nullptr;
  double nRate = 1;
  bool nOne = false;
  float nColor = 0;
  bool nTone = false;
  float nTk[5] = {};  // noise tone filter b0 b1 b2 a1 a2
  int dest[kRoutes] = {};
  float bal2[kRoutes] = {}, b1[kRoutes] = {}, b2[kRoutes] = {};
  bool fOn[kFilters] = {};
  float fMix[kFilters] = {}, fLev[kFilters] = {};
  int serial = 0;
  float amp = 0;
  EnvSet es[kEnvs] = {};
  bool menvOn[kOscs] = {};
};

struct Note {
  Ctl ctl;
  bool hasCtl = false;
  bool active = false, held = false, stealing = false;
  uint32_t id = 0;
  int key = 60, chan = 0;
  double pitch = 60, glideFrom = 60, glideTarget = 60, glideLeft = 0, glideTotal = 0;
  float vel = 0.8f, relVel = 0.5f;
  NoteProps props;
  uint64_t age = 0;
  int index = 0;
  float rnd1 = 0, rnd2 = 0, rndDisc = 0, alt1 = 0, alt2 = 0;
  VoiceStep vc{};
  float rndPan = 0, rndDet = 0, rndCut = 0, rndEnv = 0;
  float uniRand[kOscs][kMaxUnison] = {};
  EnvState env[kEnvs];
  LfoState lfo[kLfos];
  OscVoice osc[kOscs];
  SubVoice sub;
  NoiseVoice noise;
  std::unique_ptr<Filter> f1, f2;
  float off[kModSlots] = {};    // modulation offset per destination slot (normalized)
  float slew[kModSlots] = {};   // per matrix slot smoothing
  float modOut[kModSlots] = {};
  float last[8] = {};           // previous outputs: osc A, B, C, noise, sub, F1, F2, (unused)
  float fade = 1, fadeStep = 0;
  float ampPrev = 0;
  double scan[kOscs] = {};
  float menv[kOscs] = {};       // multisample envelope level
  int menvStage[kOscs] = {};
  double menvT[kOscs] = {};     // seconds in the current delay / hold stage
  float polyAT = 0;
  bool legatoStart = false;
};

class PlasmaGen final : public Generator {
 public:
  PlasmaGen() {
    for (auto& n : notes_) {
      n.f1 = std::make_unique<Filter>();
      n.f2 = std::make_unique<Filter>();
    }
    for (auto& b : vbuf_) b.assign(static_cast<size_t>(kCtl * kMaxOs), 0.f);
    base_.assign(kParamCount, 0.f);
    destOf_.assign(kParamCount, -1);
    for (auto& g : goff_) g = 0;
    scaleBuf_.reserve(16);
    held_.reserve(80);
    fxOrder_.reserve(kFxSlots + 4);
    for (auto& r : retired_) r.store(nullptr);
    pending_.store(nullptr);
    for (auto& l : globalLfo_) l.rng = 0x51ed27u;
    // free-running phases before the first note ends: unison voices already apart
    // (all at zero, a wide unison would start as one loud click)
    for (int o = 0; o < kOscs; ++o)
      for (int u = 0; u < kMaxUnison; ++u) {
        const double x = static_cast<double>(u) * 0.6180339887498949 + static_cast<double>(o) * 0.2360679774997897;
        phaseMem_[o][u] = u == 0 ? 0.0 : x - std::floor(x);
      }
  }
  ~PlasmaGen() override {
    delete pending_.exchange(nullptr);
    for (auto& r : retired_) delete r.exchange(nullptr);
    delete bank_;
    delete parkedBank_;
  }

  bool active() const override {
    for (const auto& n : notes_)
      if (n.active) return true;
    return tailLeft_ > 0 || arp_.held() || clip_.active();
  }
  bool supportsGlide() const override { return true; }
  bool handlesNotePan() const override { return true; }
  int displayStep() const override { return live_.arpStep.load(std::memory_order_relaxed); }
  void setMidiOut(MidiOutRing*) override {}

  void prepareHost(const PluginState& state, double sampleRate) override {
    for (auto& r : retired_) delete r.exchange(nullptr);
    auto aux = buildAux(state);
    // signature: rate + slot types + convolve inputs
    std::string sig = std::to_string(static_cast<int>(sampleRate)) + "|" + std::to_string(reinterpret_cast<uintptr_t>(aux.get()));
    for (int s = 0; s < kFxSlots; ++s) {
      const int k = fxP(s, X_Type);
      const int t = k < static_cast<int>(state.params.size()) ? static_cast<int>(std::lround(state.params[static_cast<size_t>(k)])) : 0;
      sig += "," + std::to_string(t);
      if (t == FxConvolve) {
        const int ki = fxParamIndex(FxConvolve, "kParamIr"), ks = fxParamIndex(FxConvolve, "kParamSize"), km = fxParamIndex(FxConvolve, "kParamMinPhase");
        for (int q : {ki, ks, km}) {
          const int kk = fxP(s, X_P0 + q);
          sig += ":" + std::to_string(kk < static_cast<int>(state.params.size()) ? state.params[static_cast<size_t>(kk)] : 0.f);
        }
      }
    }
    if (sig == bankSig_) return;
    bankSig_ = sig;
    auto bank = makeFxBank(state, aux.get(), sampleRate);
    delete pending_.exchange(bank.release());
    hostAux_ = aux;  // keeps the IR / curves alive for the bank
  }

  void* liveData() { return &live_; }
  struct Live {
    std::atomic<int> voices{0}, arpStep{-1};
    std::atomic<float> oscPos[kOscs], env[kEnvs], envTime[kEnvs], lfo[kLfos], lfoPh[kLfos], fx[kFxSlots], outL{0}, outR{0}, mod[kModSlots], cut[kFilters];
    std::atomic<int> envStage[kEnvs], oscOn[kOscs];
    std::atomic<double> clipPos{-1};
    std::atomic<float> src[SourceCount];  // every modulation source's output (lead note / global)
    Live() {
      for (auto& v : src) v.store(0);
      for (auto& v : oscPos) v.store(0);
      for (auto& v : env) v.store(0);
      for (auto& v : envTime) v.store(0);
      for (auto& v : lfo) v.store(0);
      for (auto& v : lfoPh) v.store(0);
      for (auto& v : fx) v.store(0);
      for (auto& v : mod) v.store(0);
      for (auto& v : cut) v.store(0);
      for (auto& v : envStage) v.store(0);
      for (auto& v : oscOn) v.store(0);
    }
  };
  const Live& live() const { return live_; }
  std::atomic<int> previewKey_{-1}, previewOff_{-1};
  // clip recording (editor): single-producer ring read by the UI
  std::atomic<bool> recording_{false};
  static constexpr uint32_t kRecRing = 256;
  std::array<RecordedEvent, kRecRing> recRing_{};
  std::atomic<uint32_t> recWrite_{0};
  void recPush(int key, float vel, bool on) {
    const uint32_t w = recWrite_.load(std::memory_order_relaxed);
    recRing_[w % kRecRing] = {key, vel, on, recClock_, beat_, playing_};
    recWrite_.store(w + 1, std::memory_order_release);
  }

 protected:
  void onNoteOn(uint32_t id, double pitch, float velocity) override { noteIn(id, pitch, velocity, NoteProps{}, true); }
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& props) override { noteIn(id, pitch, velocity, props, true); }
  void onNoteOff(uint32_t id) override { noteOutId(id); }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& n : notes_)
      if (n.active && n.id == id) startGlide(n, pitch, seconds);
  }
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    const int type = status & 0xF0, ch = status & 0x0F;
    if (type == 0xE0) {
      const float v = (static_cast<float>((d2 << 7) | d1) - 8192.f) / 8192.f;
      if (mpe() && ch != 0) chanBend_[ch] = v;
      else wheel_ = v;
    } else if (type == 0xB0) {
      if (d1 == 1) modWheel_ = d2 / 127.f;
      else if (d1 == 74) chanY_[ch] = d2 / 127.f;
      else if (d1 == 64) sustain_ = d2 >= 64;
      else if (d1 == 123 || d1 == 120) onAllOff();
    } else if (type == 0xD0) {
      if (mpe() && ch != 0) chanZ_[ch] = d1 / 127.f;
      else pressure_ = d1 / 127.f;
    } else if (type == 0xA0) {
      for (auto& n : notes_)
        if (n.active && n.key == d1) n.polyAT = d2 / 127.f;
    }
  }
  void onAllOff() override {
    for (auto& n : notes_)
      if (n.active) release(n);
    held_.clear();
    arp_.allOff();
    clip_.reset();
    for (auto& h : idKeys_) h.first = 0;
    heldExternal_ = 0;
    recIds_ = {};
  }
  void onKill() override {
    for (auto& n : notes_) n.active = false;
    held_.clear();
    arp_.reset();
    clip_.reset();
    for (auto& h : idKeys_) h.first = 0;
    heldExternal_ = 0;
    recIds_ = {};
    tailLeft_ = 0;
    for (auto& d : decim_) d.reset();
    if (bank_)
      for (auto& u : bank_->unit)
        if (u) fxReset(*u);
  }

  void configure(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx) override {
    aux_ = dynamic_cast<const Aux*>(aux.custom.get());
    if (aux_ && (aux_->model.arpActive != lastArpActive_ || aux_->model.clipActive != lastClipActive_)) {
      // the editor picked another slot: it wins over the trigger octave
      lastArpActive_ = aux_->model.arpActive, lastClipActive_ = aux_->model.clipActive;
      arpSel_ = clipSel_ = -1;
    }
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo > 1 ? ctx.tempo : 120;
    playing_ = ctx.playing;
    beat_ = ctx.ppq > 0 ? ctx.tick / ctx.ppq : 0;
    if (FxBank* b = pending_.exchange(nullptr)) {
      FxBank* old = bank_;
      bank_ = b;
      if (old) {
        bool parked = false;
        for (auto& r : retired_) {
          FxBank* expected = nullptr;
          if (r.compare_exchange_strong(expected, old)) {
            parked = true;
            break;
          }
        }
        if (!parked) parkedBank_ = old;  // freed with the generator
      }
    }
    const auto& p = state.params;
    const size_t n = std::min(p.size(), static_cast<size_t>(kParamCount));
    std::copy(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(n), base_.begin());
    os_ = G(G_Quality) >= 1.5f ? 4 : G(G_Quality) >= 0.5f ? 2 : 1;
    if (os_ != prevOs_) {
      for (auto& d : decim_) d.reset();
      prevOs_ = os_;
    }
    srv_ = sr_ * os_;
    buildMatrix();
    // scale for the arp / porta (Key + Scale)
    scaleBuf_.clear();
    for (int s : scaleSteps(static_cast<int>(G(G_Scale)))) scaleBuf_.push_back(s);
    // editor previews
    const int pk = previewKey_.exchange(-1);
    if (pk >= 0) noteIn(0xFFFF0000u | static_cast<uint32_t>(pk), pk, 0.8f, NoteProps{}, true);
    const int po = previewOff_.exchange(-1);
    if (po >= 0) noteOutId(0xFFFF0000u | static_cast<uint32_t>(po));
  }

  void render(float* L, float* R, int n) override {
    if (!aux_) return;
    // control periods on an absolute grid: the result does not depend on the host block size
    for (int pos = 0; pos < n;) {
      const bool grid = ctlLeft_ <= 0;
      if (grid) {
        ctlLeft_ = kCtl;
        runSequencers(kCtl);  // sequencer events land on the control grid
      }
      const int m = std::min(ctlLeft_, n - pos);
      renderChunk(L + pos, R + pos, m, grid, ctlLeft_);
      beat_ += m / sr_ * tempo_ / 60.0;
      recClock_ += m / sr_ * tempo_ / 60.0;
      ctlLeft_ -= m;
      pos += m;
    }
    publishLive();
  }

  int takeMidiOut(MidiOutEvent* out, int max) override {
    const int c = std::min(midiCount_, max);
    for (int i = 0; i < c; ++i) out[i] = midiOut_[i];
    midiCount_ = 0;
    return c;
  }

 private:
  // ---------------------------------------------------------------- params
  float G(int k) const { return base_[static_cast<size_t>(k)]; }
  bool mpe() const { return G(G_MpeOn) >= 0.5f; }
  // value of k with the note's modulation
  float V(const Note* nt, int k) const {
    const int d = destOf_[static_cast<size_t>(k)];
    const float b = base_[static_cast<size_t>(k)];
    if (d < 0) return b;
    const float off = nt ? nt->off[d] : goff_[d];
    if (off == 0) return b;
    const PMap& pm = pmaps()[static_cast<size_t>(k)];
    return fromNorm(pm, toNorm(pm, b) + off);
  }
  float GV(int k) const { return V(lead_, k); }

  void buildMatrix() {
    std::fill(destOf_.begin(), destOf_.end(), -1);
    slotCount_ = 0;
    destCount_ = 0;
    audioSrc_ = false;
    for (int s = 0; s < kModSlots; ++s) {
      const int src = static_cast<int>(std::lround(G(modP(s, M_Source))));
      const int dst = static_cast<int>(std::lround(G(modP(s, M_Dest)))) - 1;
      if (src <= SrcNone || src >= SourceCount || dst < 0 || dst >= kParamCount || G(modP(s, M_Bypass)) >= 0.5f) continue;
      if (!modulatable(dst)) continue;
      int d = destOf_[static_cast<size_t>(dst)];
      if (d < 0) {
        d = destCount_++;
        destOf_[static_cast<size_t>(dst)] = d;
        destParam_[d] = dst;
      }
      Slot& sl = slots_[slotCount_++];
      sl.index = s;
      sl.src = src;
      sl.aux = static_cast<int>(std::lround(G(modP(s, M_Aux))));
      sl.dest = d;
      if ((src >= SrcNoise && src <= SrcSub) || src == SrcFilter1 || src == SrcFilter2) audioSrc_ = true;
    }
  }

  // ---------------------------------------------------------------- notes
  int activeCount() const {
    int c = 0;
    for (const auto& n : notes_)
      if (n.active && !n.stealing) ++c;
    return c;
  }
  double notePitch(double pitch) const {
    if (!aux_ || !aux_->hasTuning) return pitch;
    const int k = std::clamp(static_cast<int>(std::floor(pitch)), 0, 126);
    const double f = pitch - k;
    return aux_->tuning[static_cast<size_t>(k)] + (aux_->tuning[static_cast<size_t>(k + 1)] - aux_->tuning[static_cast<size_t>(k)]) * f;
  }
  void startGlide(Note& n, double target, double secs) {
    n.glideFrom = n.pitch;
    n.glideTarget = target;
    n.glideTotal = n.glideLeft = std::max(0.0, secs);
    if (secs <= 0) n.pitch = target;
  }
  double portaSecs(double from, double to) const {
    double t = portaTime(GV(G_PortaTime));
    if (G(G_PortaScaled) >= 0.5f) t *= std::fabs(to - from) / 12.0;
    return t;
  }

  // note input: through the clip player and the arpeggiator when they are on
  void noteIn(uint32_t id, double pitch, float vel, const NoteProps& props, bool external) {
    const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    if (external && recording_.load(std::memory_order_relaxed)) {
      // recording a clip: the note sounds directly and is reported to the editor
      for (auto& h : recIds_)
        if (h.first == 0) {
          h = {id, key};
          break;
        }
      recPush(key, vel, true);
      playNote(id, pitch, vel, props);
      return;
    }
    if (external) {
      const bool seq = (G(G_ClipOn) >= 0.5f && aux_) || G(G_ArpOn) >= 0.5f;
      // trigger octave: its twelve keys pick the clip / arp slot instead of playing
      const int trig = static_cast<int>(G(G_TrigOctave));
      if (seq && trig >= 1 && key >= (trig - 1) * 12 && key < trig * 12) {
        arpSel_ = clipSel_ = key - (trig - 1) * 12;
        for (auto& h : idKeys_)
          if (h.first == 0) {
            h = {id, 1000};  // a trigger key: its note-off is swallowed
            break;
          }
        return;
      }
      if (seq) {  // remember the key of this note id (note-offs arrive by id)
        for (auto& h : idKeys_)
          if (h.first == 0) {
            h = {id, key};
            break;
          }
      }
      if (G(G_ClipOn) >= 0.5f && aux_) {
        clip_.noteOn(key, vel);
        return;
      }
      if (G(G_ArpOn) >= 0.5f) {
        arp_.noteOn(key, vel);
        if (!aux_ || !aux_->model.arp[static_cast<size_t>(activeArp())].thru) return;
      }
      if (G(G_NoteLatch) >= 0.5f && heldExternal_ == 0)
        for (auto& n : notes_)
          if (n.active && !n.stealing) release(n);
      ++heldExternal_;
    }
    playNote(id, pitch, vel, props);
  }
  void noteOutId(uint32_t id) {
    bool found = false;
    for (auto& n : notes_)
      if (n.active && n.id == id) found = true;
    for (auto& h : recIds_)
      if (h.first == id && id != 0) {
        recPush(h.second, 0, false);
        h.first = 0;
        if (found) stopNote(id);
        return;
      }
    // external notes into the clip player / arp are keyed by pitch
    if (G(G_ClipOn) >= 0.5f || G(G_ArpOn) >= 0.5f) {
      for (auto& h : idKeys_)
        if (h.first == id && h.second >= 1000) {
          h.first = 0;
          return;
        }
      for (auto& h : idKeys_)
        if (h.first == id) {
          if (G(G_ClipOn) >= 0.5f) clip_.noteOff(h.second);
          if (G(G_ArpOn) >= 0.5f) arp_.noteOff(h.second);
          h.first = 0;
        }
    }
    if (heldExternal_ > 0) --heldExternal_;
    if (G(G_NoteLatch) >= 0.5f) return;
    if (!found) {
      // J-203a: in mono a held key whose voice was taken over by a later key is still remembered
      // for the return; its release must forget it, or the last release glides back to it instead of
      // ending the note (a mono chord then never releases and later notes join a silent legato voice)
      for (size_t i = 0; i < held_.size(); ++i)
        if (held_[i].id == id) {
          held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
          break;
        }
      return;
    }
    stopNote(id);
  }
  void stopNote(uint32_t id) {
    // mono: return to the previous held key
    for (size_t i = 0; i < held_.size(); ++i)
      if (held_[i].id == id) {
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    if (mono()) {
      Note* n = monoNote();
      if (n && n->id == id) {
        if (!held_.empty()) {
          const HeldKey& h = held_.back();
          n->id = h.id;
          n->key = h.key;
          startGlide(*n, h.pitch, portaSecs(n->pitch, h.pitch));
          return;
        }
        if (!sustain_) release(*n);
        return;
      }
    }
    for (auto& n : notes_)
      if (n.active && n.id == id && !sustain_) release(n);
  }
  bool mono() const { return G(G_Mono) >= 0.5f; }
  Note* monoNote() {
    for (auto& n : notes_)
      if (n.active && !n.stealing) return &n;
    return nullptr;
  }
  void release(Note& n) {
    if (!n.held) return;
    n.held = false;
    for (int e = 0; e < kEnvs; ++e) {
      EnvSet p = envSet(&n, e);
      if (n.env[e].stage != 0) envStage(n.env[e], 5, p, srv_);
    }
    for (auto& o : n.osc) o.release();
  }
  void steal(Note& n) {
    n.stealing = true;
    n.fade = 1;
    n.fadeStep = static_cast<float>(1.0 / std::max(1.0, 0.003 * srv_));
  }
  Note* allocate(int key, float vel) {
    const int poly = std::clamp(static_cast<int>(std::lround(G(G_Poly))), 1, kMaxVoices);
    if (G(G_SameNote) >= 0.5f)
      for (auto& n : notes_)
        if (n.active && !n.stealing && n.key == key) steal(n);
    int count = activeCount();
    while (count >= poly) {
      Note* victim = nullptr;
      const int prio = static_cast<int>(G(G_Priority));
      for (auto& n : notes_) {
        if (!n.active || n.stealing) continue;
        if (!victim) {
          victim = &n;
          continue;
        }
        bool better = false;
        if (!n.held && victim->held) better = true;  // released voices go first
        else if (n.held == victim->held) {
          switch (prio) {
            case 1: better = n.age > victim->age; break;                 // oldest kept: steal the newest
            case 2: better = n.key < victim->key; break;                 // highest kept
            case 3: better = n.key > victim->key; break;                 // lowest kept
            case 4: better = n.vel < victim->vel; break;                 // loudest kept
            default: better = n.age < victim->age; break;                // newest kept
          }
        }
        if (better) victim = &n;
      }
      if (!victim) break;
      if (G(G_Priority) >= 0.5f && G(G_Priority) < 1.5f && victim->held) return nullptr;  // "oldest": the new note is dropped
      steal(*victim);
      --count;
    }
    (void)vel;
    for (auto& n : notes_)
      if (!n.active) return &n;
    // all slots busy: reuse the quietest fading one
    Note* q = &notes_[0];
    for (auto& n : notes_)
      if (n.stealing && n.fade < q->fade) q = &n;
    return q;
  }

  EnvSet envSet(const Note* n, int e) const {
    EnvSet p;
    const double scale = G(G_EnvScale) * (n ? (1.0 + n->rndEnv + n->vc.env * 0.5) : 1.0);
    const double beat = 60.0 / tempo_;
    const bool bpm = V(n, envP(e, E_Bpm)) >= 0.5f;
    auto tm = [&](int k) {
      double s = envTime(V(n, envP(e, k)));
      if (bpm) s = s * 2.0 * beat;  // knob seconds read as beats
      return std::max(0.0, s * scale);
    };
    p.a = tm(E_Attack), p.h = tm(E_Hold), p.d = tm(E_Decay), p.r = tm(E_Release);
    p.s = levelGain(V(n, envP(e, E_Sustain)));
    p.ca = V(n, envP(e, E_CurveA)), p.cd = V(n, envP(e, E_CurveD)), p.cr = V(n, envP(e, E_CurveR));
    p.start = V(n, envP(e, E_Start)), p.end = V(n, envP(e, E_End));
    p.steep = G(tailP(T_EnvLaw)) >= 0.5f;
    return p;
  }

  void playNote(uint32_t id, double pitch, float vel, const NoteProps& props) {
    const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    const double target = notePitch(pitch) + G(G_Transpose);
    if (mono()) {
      HeldKey h{id, key, target};
      held_.push_back(h);
      if (held_.size() > 64) held_.erase(held_.begin());
      Note* n = monoNote();
      const bool legato = G(G_Legato) >= 0.5f && n && n->held;
      if (n && (legato || G(G_PortaAlways) >= 0.5f || n->held)) {
        // legato / glide: keep the note, move its pitch
        const double from = n->pitch;
        n->id = id;
        n->key = key;
        n->vel = vel;
        startGlide(*n, target, legato || G(G_PortaAlways) >= 0.5f ? portaSecs(from, target) : 0.0);
        if (!legato) {
          retrigger(*n, false);
        } else {
          legatoRetrig(*n);
        }
        lastPitch_ = target;
        return;
      }
      if (n) steal(*n);
    }
    Note* n = allocate(key, vel);
    if (!n) return;
    const double from = lastPitch_;
    n->active = true;
    n->hasCtl = false;
    n->held = true;
    n->stealing = false;
    n->fade = 1;
    n->id = id;
    n->key = key;
    n->chan = props.color;
    n->vel = vel;
    n->relVel = props.release;
    n->props = props;
    n->age = ++ageCounter_;
    n->pitch = target;
    n->glideLeft = 0;
    const double pt = portaTime(G(G_PortaTime));
    if (pt > 0 && (G(G_PortaAlways) >= 0.5f || activeCount() > 1) && lastPitch_ >= 0) startGlide(*n, target, portaSecs(from, target)), n->pitch = from;
    lastPitch_ = target;
    // per-note randoms and voice control
    n->rnd1 = rngUni(rng_);
    n->rnd2 = rngUni(rng_);
    n->rndDisc = std::floor(rngUni(rng_) * 8.f) / 7.f;
    alt_ = !alt_;
    n->alt1 = alt_ ? 1.f : 0.f;
    alt2_ = (alt2_ + 1) & 3;
    n->alt2 = alt2_ >= 2 ? 1.f : 0.f;
    const int steps = std::clamp(static_cast<int>(std::lround(G(G_VcSteps))), 1, 8);
    vcStep_ = (vcStep_ + 1) % steps;
    n->vc = aux_ ? aux_->model.voiceSeq[static_cast<size_t>(vcStep_)] : VoiceStep{};
    n->rndPan = rngBi(rng_) * G(G_RndPan);
    n->rndDet = rngBi(rng_) * G(G_RndDetune);
    n->rndCut = rngBi(rng_) * G(G_RndCutoff);
    n->rndEnv = rngBi(rng_) * G(G_RndEnv) * 0.5f;
    for (auto& o : n->uniRand)
      for (float& v : o) v = rngBi(rng_);
    int idx = 0;
    for (auto& x : notes_)
      if (&x == n) break;
      else ++idx;
    n->index = idx;
    n->polyAT = 0;
    for (float& v : n->slew) v = 0;
    for (float& v : n->off) v = 0;
    for (float& v : n->last) v = 0;
    for (auto& s : n->scan) s = 0;
    n->ampPrev = 0;
    n->f1->reset();
    n->f2->reset();
    retrigger(*n, true);
  }

  void legatoRetrig(Note& n) {
    // legato note: modules retrigger by their probability (and inverted envelopes)
    auto chance = [&](int r) { const float p = G(retrigP(r)); return p > 0 && (p >= 1 || rngUni(rng_) < p); };
    if (chance(0)) {
      retrigger(n, false);
      return;
    }
    for (int e = 0; e < kEnvs; ++e) {
      const bool inv = V(&n, envP(e, E_LegatoInv)) >= 0.5f;
      if (inv || (e > 0 && chance(5 + e))) startEnv(n, e);
    }
    for (int l = 0; l < kLfos; ++l)
      if (chance(9 + l)) startLfo(n, l);
    for (int o = 0; o < kOscs; ++o)
      if (chance(1 + o)) startOsc(n, o);
    if (chance(4)) n.sub.ph = V(&n, subP(S_Phase));
    if (chance(5)) startNoise(n);
  }

  void startEnv(Note& n, int e) {
    EnvSet p = envSet(&n, e);
    EnvState& s = n.env[e];
    const bool fromZero = V(&n, envP(e, E_Steal)) >= 0.5f;
    if (fromZero || s.stage == 0) s.level = p.start;
    envStage(s, 1, p, srv_);
  }
  // Start phase: PHASE, snapped to the horizontal grid when "snap to grid" is on.
  double lfoStartPhase(const Note* n, int l) const {
    double ph = V(n, lfoP(l, L_Phase));
    if (V(n, lfoP(l, L_PhaseSnap)) >= 0.5f) {
      const double g = std::max(1.0, std::round(static_cast<double>(V(n, lfoP(l, L_GridX)))));
      ph = std::round(ph * g) / g;
    }
    return ph - std::floor(ph);
  }
  void startLfo(Note& n, int l) {
    LfoState& s = n.lfo[l];
    const int mode = static_cast<int>(V(&n, lfoP(l, L_Mode)));
    if (mode == LfoFree) {
      s.phase = globalLfo_[l].phase;
    } else {
      s.phase = lfoStartPhase(&n, l);
    }
    s.t = 0;
    s.done = false;
    s.cycles = 0;
    s.sm = s.smY = 0;
    s.rng = 0x9e3779b9u ^ static_cast<uint32_t>(n.age * 2654435761u + static_cast<uint32_t>(l));
    s.sh = rngUni(s.rng);
    s.lx = 0.1 + rngUni(s.rng) * 0.1, s.ly = 0, s.lz = 0;
  }
  void startOsc(Note& n, int o) {
    OscSet s;
    oscSettings(n, o, s);
    const int mode = static_cast<int>(V(&n, oscP(o, O_PhaseMemory)));
    n.osc[o].start(s, V(&n, oscP(o, O_Phase)), V(&n, oscP(o, O_RandPhase)), V(&n, oscP(o, O_UniRandStart)), mode,
                   mode > 0 ? phaseMem_[o] : nullptr, rngNext(rng_), n.key, n.vel);
    n.scan[o] = 0;
    n.menv[o] = 0;
    n.menvStage[o] = 1;
    n.menvT[o] = 0;
  }
  void startNoise(Note& n) {
    const int t = static_cast<int>(V(&n, noiseP(N_Type)));
    const auto& fn = factoryNoises();
    const NoiseSrc* src = t >= 1 && t - 1 < static_cast<int>(fn.size()) ? &fn[static_cast<size_t>(t - 1)] : nullptr;
    n.noise.start(src, t == 0 && aux_ ? aux_->noiseFile.get() : nullptr, V(&n, noiseP(N_Phase)), V(&n, noiseP(N_RandPhase)), rngNext(rng_));
  }
  void retrigger(Note& n, bool fresh) {
    for (int e = 0; e < kEnvs; ++e) startEnv(n, e);
    for (int l = 0; l < kLfos; ++l) {
      const int mode = static_cast<int>(V(&n, lfoP(l, L_Mode)));
      if (fresh || mode != LfoFree) startLfo(n, l);
    }
    for (int o = 0; o < kOscs; ++o) startOsc(n, o);
    if (fresh && V(&n, subP(S_ContPhase)) < 0.5f) n.sub.ph = V(&n, subP(S_Phase));
    else if (fresh) n.sub.ph = subPhase_;
    startNoise(n);
  }

  // ---------------------------------------------------------------- modulation
  struct Slot {
    int index = 0, src = 0, aux = 0, dest = 0;
  };
  float sourceValue(const Note* n, int s) const {
    switch (s) {
      case SrcNone: return 0;
      case SrcVelo: return n ? curve129(aux_->velo, n->vel) : 0;
      case SrcNote: return n ? curve129(aux_->note, static_cast<float>(std::clamp(n->pitch, 0.0, 127.0) / 127.0)) : 0;
      case SrcActiveVoices: return std::min(1.f, static_cast<float>(activeCount()) / std::max(1.f, G(G_Poly)));
      case SrcAlt1: return n ? n->alt1 : 0;
      case SrcAlt2: return n ? n->alt2 : 0;
      case SrcRandDiscrete: return n ? n->rndDisc : 0;
      case SrcRand1: return n ? n->rnd1 : 0;
      case SrcRand2: return n ? n->rnd2 : 0;
      case SrcReleaseVelo: return n ? n->relVel : 0;
      case SrcVoiceIndex: return n ? static_cast<float>(n->index) / std::max(1.f, G(G_Poly) - 1) : 0;
      case SrcVoiceMod1: return n ? n->vc.mod1 * 0.5f + 0.5f : 0.5f;
      case SrcVoiceMod2: return n ? n->vc.mod2 * 0.5f + 0.5f : 0.5f;
      case SrcNoise: return n ? n->last[3] : 0;
      case SrcOscA: return n ? n->last[0] : 0;
      case SrcOscB: return n ? n->last[1] : 0;
      case SrcOscC: return n ? n->last[2] : 0;
      case SrcSub: return n ? n->last[4] : 0;
      case SrcExprX:
        if (!n) return 0.5f;
        return mpe() ? chanBend_[n->chan & 15] * 0.5f + 0.5f : n->props.pan * 0.5f + 0.5f;
      case SrcExprY:
        if (!n) return 0;
        {
          const float y = mpe() ? chanY_[n->chan & 15] : n->props.modX;
          return G(G_MpeYBipolar) >= 0.5f ? y * 2.f - 1.f : y;  // bipolar: centred on the middle position
        }
      case SrcExprZ:
        if (!n) return 0;
        return mpe() ? chanZ_[n->chan & 15] : n->props.modY;
      case SrcFilter1: return n ? n->last[5] : 0;
      case SrcFilter2: return n ? n->last[6] : 0;
      case SrcModWheel: return std::max(G(G_ModWheel), modWheel_);
      case SrcAftertouch: return pressure_;
      case SrcPolyAT: return n ? n->polyAT : 0;
      case SrcPitchBend: return std::clamp(G(G_PitchWheel) + wheel_, -1.f, 1.f);
      case SrcFixed: return 1;
      default: break;
    }
    if (s >= SrcEnv1 && s <= SrcEnv4) return n ? static_cast<float>(n->env[s - SrcEnv1].level) : 0;
    if (s >= SrcLfo1 && s <= SrcLfo10) {
      const int l = s - SrcLfo1;
      if (G(lfoP(l, L_Mono)) >= 0.5f || !n) return static_cast<float>(globalLfo_[l].out);
      return static_cast<float>(n->lfo[l].out);
    }
    if (s >= SrcLfo1Y && s <= SrcLfo10Y) {
      const int l = s - SrcLfo1Y;
      if (G(lfoP(l, L_Mono)) >= 0.5f || !n) return static_cast<float>(globalLfo_[l].y);
      return static_cast<float>(n->lfo[l].y);
    }
    if (s >= SrcMacro1 && s <= SrcMacro8) return G(macroP(s - SrcMacro1));
    return 0;
  }
  // Per-note properties that stay meaningful after the note ends (velocity, key, ...).
  static bool heldNoteSource(int s) {
    return s == SrcVelo || s == SrcNote || s == SrcReleaseVelo || s == SrcAlt1 || s == SrcAlt2 || s == SrcRandDiscrete || s == SrcRand1 ||
           s == SrcRand2 || s == SrcVoiceIndex;
  }
  static bool bipolarSource(int s) {
    return s == SrcPitchBend || (s >= SrcNoise && s <= SrcSub) || s == SrcFilter1 || s == SrcFilter2;
  }
  static float curve129(const std::array<float, 129>& c, float x) {
    const float u = std::clamp(x, 0.f, 1.f) * 128.f;
    const int i = std::min(127, static_cast<int>(u));
    return c[static_cast<size_t>(i)] + (c[static_cast<size_t>(i + 1)] - c[static_cast<size_t>(i)]) * (u - static_cast<float>(i));
  }
  float shapeMod(int slot, float v, bool auxCurve) const {
    const auto& has = auxCurve ? aux_->hasAuxCurve : aux_->hasModCurve;
    if (has[static_cast<size_t>(slot)]) {
      const auto& c = auxCurve ? aux_->auxCurve[static_cast<size_t>(slot)] : aux_->modCurve[static_cast<size_t>(slot)];
      const float u = std::clamp(v, 0.f, 1.f) * 64.f;
      const int i = std::min(63, static_cast<int>(u));
      return c[static_cast<size_t>(i)] + (c[static_cast<size_t>(i + 1)] - c[static_cast<size_t>(i)]) * (u - static_cast<float>(i));
    }
    const float b = G(modP(slot, auxCurve ? M_AuxCurve : M_Curve));
    if (std::fabs(b) < 1e-4f) return v;
    const float sgn = v < 0 ? -1.f : 1.f;
    return sgn * static_cast<float>(bendCurve(std::fabs(v), b));
  }
  // Evaluates the matrix for a note (or the global context when n is null).
  void evalMatrix(Note* n, float* off, float dtSec) {
    for (int d = 0; d < destCount_; ++d) off[d] = 0;
    for (int i = 0; i < slotCount_; ++i) {
      const Slot& s = slots_[i];
      float v = sourceValue(n, s.src);
      const bool bi = G(modP(s.index, M_Bipolar)) >= 0.5f;
      if (bipolarSource(s.src)) {
        v = shapeMod(s.index, v, false);
      } else {
        v = shapeMod(s.index, v, false);
        if (bi) v = v * 2.f - 1.f;
      }
      if (s.aux > SrcNone && s.aux < SourceCount) {
        float a = shapeMod(s.index, std::clamp(sourceValue(n, s.aux), 0.f, 1.f), true);
        if (G(modP(s.index, M_AuxInv)) >= 0.5f) a = 1.f - a;
        v *= a;
      }
      const float amt = n ? V(n, modP(s.index, M_Amount)) : GV(modP(s.index, M_Amount));
      const float outv = n ? V(n, modP(s.index, M_Out)) : GV(modP(s.index, M_Out));
      float target = v * amt * outv;
      // smoothing (rise / fall)
      const float rise = G(modP(s.index, M_Rise)), fall = G(modP(s.index, M_Fall));
      float* sl = n ? &n->slew[s.index] : &gslew_[s.index];
      const float tc = target > *sl ? rise : fall;
      if (tc > 0) {
        const float c = 1.f - std::exp(-dtSec / (tc * tc * 2.f + 1e-4f));
        *sl += (target - *sl) * c;
        target = *sl;
      } else {
        *sl = target;
      }
      off[s.dest] += target;
      if (n) n->modOut[s.index] = target;
      else gmodOut_[s.index] = target;
    }
  }

  // ---------------------------------------------------------------- LFOs
  float lfoShapeAt(const Note* n, int l, double x) const {
    const LfoAux& la = aux_->lfo[static_cast<size_t>(l)];
    const std::vector<CurvePt>* pts = &la.pts;
    if (pts->empty()) {
      const int sh = std::clamp(static_cast<int>(G(lfoP(l, L_Shape))), 1, static_cast<int>(aux_->factoryShapes.size()) - 1);
      pts = &aux_->factoryShapes[static_cast<size_t>(sh)];
    }
    if (aux_->busLinks.empty()) return static_cast<float>(curveAt(*pts, x));
    // point modulation: copy, move, evaluate
    CurvePt tmp[128];
    const int cnt = std::min(128, static_cast<int>(pts->size()));
    for (int i = 0; i < cnt; ++i) tmp[i] = (*pts)[static_cast<size_t>(i)];
    for (const auto& b : aux_->busLinks) {
      if (b.lfo != l || b.point >= cnt) continue;
      const float bus = V(n, busP(b.bus)) * b.depth;
      CurvePt& p = tmp[b.point];
      if (b.axis == 0) {
        const float lo = b.point > 0 ? tmp[b.point - 1].x : 0.f, hi = b.point + 1 < cnt ? tmp[b.point + 1].x : 1.f;
        p.x = std::clamp(p.x + bus * 0.5f, lo, hi);
      } else {
        p.y = std::clamp(p.y + bus * 0.5f, 0.f, 1.f);
      }
    }
    if (x <= tmp[0].x) return tmp[0].y;
    for (int i = 1; i < cnt; ++i)
      if (x <= tmp[i].x) {
        const double w = tmp[i].x - tmp[i - 1].x;
        if (w <= 1e-9) return tmp[i].y;
        return static_cast<float>(tmp[i - 1].y + (tmp[i].y - tmp[i - 1].y) * bendCurve((x - tmp[i - 1].x) / w, tmp[i].c));
      }
    return tmp[cnt - 1].y;
  }
  double lfoRateHz(const Note* n, int l) const {
    double hz;
    if (V(n, lfoP(l, L_Bpm)) >= 0.5f) {
      const int i = std::clamp(static_cast<int>(std::lround(V(n, lfoP(l, L_Rate)) * (lfoRateDivisionCount() - 1))), 0, lfoRateDivisionCount() - 1);
      double beats = lfoRateDivisionBeats(i);
      if (V(n, lfoP(l, L_Dot)) >= 0.5f) beats *= 1.5;
      if (V(n, lfoP(l, L_Trip)) >= 0.5f) beats *= 2.0 / 3.0;
      hz = tempo_ / 60.0 / beats;
    } else {
      hz = lfoHz(V(n, lfoP(l, L_Rate)), V(n, lfoP(l, L_Rate10x)) >= 0.5f);
    }
    const double sc = std::max(0.01f, G(G_LfoScale));
    return G(G_LfoScaleRate) >= 0.5f ? hz * sc : hz / sc;
  }
  double divSeconds(double v) const {  // rise / delay: 0 = off, else a tempo division
    const int i = static_cast<int>(std::lround(v * lfoRateDivisionCount()));
    if (i <= 0) return 0;
    return lfoRateDivisionBeats(lfoRateDivisionCount() - i) * 60.0 / tempo_;
  }
  void stepLfo(const Note* n, LfoState& s, int l, double dt, bool held, bool global) {
    const int type = static_cast<int>(V(n, lfoP(l, L_Type)));
    const int mode = static_cast<int>(V(n, lfoP(l, L_Mode)));
    const double hz = lfoRateHz(n, l);
    const double delay = divSeconds(V(n, lfoP(l, L_Delay)));
    const double rise = divSeconds(V(n, lfoP(l, L_Rise)));
    s.t += dt;
    double y = 0, y2 = 0;
    if (s.t >= delay) {
      // phase
      if (global || mode == LfoFree) {
        if (V(n, lfoP(l, L_Anchor)) >= 0.5f && playing_) {
          const double cyc = beat_ * hz * 60.0 / tempo_ + lfoStartPhase(n, l);
          s.phase = cyc - std::floor(cyc);
        } else {
          s.phase += hz * dt;
        }
      } else if (!s.done) {
        s.phase += hz * dt;
      }
      if (mode == LfoEnvelope && !global) {
        const int loop = aux_->lfo[static_cast<size_t>(l)].loop;
        if (s.phase >= 1.0) {
          if (loop >= 0 && held && loop < static_cast<int>(aux_->lfo[static_cast<size_t>(l)].pts.size())) {
            const double lx = aux_->lfo[static_cast<size_t>(l)].pts[static_cast<size_t>(loop)].x;
            s.phase = lx + std::fmod(s.phase - 1.0, std::max(1e-3, 1.0 - lx));
          } else {
            s.phase = 1.0;
            s.done = true;
          }
        }
      } else if (s.phase >= 1.0) {
        const double c = std::floor(s.phase);
        s.phase -= c;
        s.cycles += static_cast<int>(c);
        if (type == LfoSH) s.sh = rngUni(s.rng);
      }
      double x = s.phase;
      // swing: alternate cycles stretch / squeeze
      if (V(n, lfoP(l, L_Swing)) >= 0.5f) {
        const double sw = std::clamp(static_cast<double>(G(G_Swing)), 0.05, 0.95);
        x = (s.cycles & 1) ? (x < 1 - sw ? x * 0.5 / (1 - sw) : 0.5 + (x - (1 - sw)) * 0.5 / sw) : (x < sw ? x * 0.5 / sw : 0.5 + (x - sw) * 0.5 / (1 - sw));
      }
      const int dir = static_cast<int>(V(n, lfoP(l, L_Dir)));
      if (dir == 1) x = 1.0 - x;
      else if (dir == 2) x = (s.cycles & 1) ? 1.0 - x : x;
      switch (type) {
        case LfoPath: {
          const LfoAux& la = aux_->lfo[static_cast<size_t>(l)];
          if (!la.path.empty()) {
            y = curveAt(la.path, x);
            if (!la.pathX.empty()) {
              const double fi = x * (la.pathX.size() - 1);
              const size_t i0 = static_cast<size_t>(fi), i1 = std::min(la.pathX.size() - 1, i0 + 1);
              y2 = la.pathX[i0] + (la.pathX[i1] - la.pathX[i0]) * (fi - i0);
            } else {
              y2 = x;
            }
          } else {
            y = 0.5 + 0.5 * std::sin(kPi2 * x);
            y2 = 0.5 + 0.5 * std::cos(kPi2 * x);
          }
          break;
        }
        case LfoLorenz:
        case LfoRossler: {
          const int steps = std::clamp(static_cast<int>(hz * dt * 200.0) + 1, 1, 64);
          const double h = std::min(0.02, hz * dt * 2.0 / steps);
          for (int i = 0; i < steps; ++i) {
            double dx, dy, dz;
            if (type == LfoLorenz) {
              dx = 10.0 * (s.ly - s.lx), dy = s.lx * (28.0 - s.lz) - s.ly, dz = s.lx * s.ly - 8.0 / 3.0 * s.lz;
            } else {
              dx = -s.ly - s.lz, dy = s.lx + 0.2 * s.ly, dz = 0.2 + s.lz * (s.lx - 5.7);
            }
            s.lx += dx * h, s.ly += dy * h, s.lz += dz * h;
            if (!std::isfinite(s.lx) || !std::isfinite(s.ly) || !std::isfinite(s.lz)) s.lx = 0.1, s.ly = s.lz = 0;
          }
          const double sc = type == LfoLorenz ? 25.0 : 12.0;
          y = std::clamp(0.5 + s.lx / (2 * sc), 0.0, 1.0);
          y2 = std::clamp(0.5 + s.ly / (2 * sc), 0.0, 1.0);
          break;
        }
        case LfoSH:
          y = s.sh;
          y2 = x;
          break;
        default:
          y = lfoShapeAt(n, l, x);
          y2 = x;
          break;
      }
      if (rise > 0 && s.t - delay < rise) {
        const double r = (s.t - delay) / rise;
        y *= r, y2 *= r;
      }
    }
    // smooth
    const double sm = V(n, lfoP(l, L_Smooth));
    if (sm > 0) {
      const double tau = sm * sm * 0.5 / std::max(0.05, hz) + 1e-4;
      const double c = 1.0 - std::exp(-dt / tau);
      s.sm += (y - s.sm) * c;
      s.smY += (y2 - s.smY) * c;
      y = s.sm, y2 = s.smY;
    } else {
      s.sm = y, s.smY = y2;
    }
    s.out = y;
    s.y = y2;
  }

  // ---------------------------------------------------------------- oscillator settings
  double oscPitch(const Note& n, int o) const {
    const bool track = V(&n, oscP(o, O_PitchTrack)) >= 0.5f;
    double p = track ? n.pitch : 60.0;
    const int mode = static_cast<int>(V(&n, oscP(o, O_PitchMode)));
    double semi = std::round(V(&n, oscP(o, O_Semi)));
    if (mode == 1 && !scaleBuf_.empty()) {  // steps of the scale
      const int st = static_cast<int>(std::lround(semi));
      const int len = static_cast<int>(scaleBuf_.size());
      const int oct = st >= 0 ? st / len : -((-st + len - 1) / len);
      const int idx = ((st % len) + len) % len;
      semi = oct * 12 + scaleBuf_[static_cast<size_t>(idx)];
    }
    p += std::round(V(&n, oscP(o, O_Octave))) * 12.0 + semi + V(&n, oscP(o, O_Fine)) / 100.0 + V(&n, oscP(o, O_Coarse));
    p += bendSemis(n) + G(G_MasterTune) / 100.0 + 12.0 * std::log2(std::max(1.f, G(G_ConcertA)) / 440.0);
    const bool vc = G(G_VcA + o) >= 0.5f;
    if (vc) p += n.vc.detune * 0.5 + n.rndDet / 100.0;
    return p;
  }
  double bendSemis(const Note& n) const {
    const float w = std::clamp(G(G_PitchWheel) + wheel_, -1.f, 1.f);
    double s = w >= 0 ? w * G(G_BendUp) : -w * G(G_BendDown);
    if (mpe()) s += chanBend_[n.chan & 15] * G(G_MpeBend);
    return s;
  }
  void panGains(float pan, float& l, float& r) const {
    // centre at unity; the near side rises and the far side falls slightly bowed,
    // so a hard pan is a little louder in total than the centre (+1.4 dB)
    const float a = std::min(std::fabs(pan), 1.f);
    const float nearG = std::sqrt(1.f + 1.76f * a), farG = (1.f - a) * (1.f + 0.15f * a);
    l = pan > 0 ? farG : nearG;
    r = pan > 0 ? nearG : farG;
  }
  void oscSettings(const Note& n, int o, OscSet& s) const {
    s.on = V(&n, oscP(o, O_Enable)) >= 0.5f;
    s.silent = false;
    double fold = 0;  // semitones (FOLD: keys outside the zone move into it by octaves)
    if (s.on) {
      const float kmin = std::round(V(&n, oscP(o, O_KeyMin))), kmax = std::round(V(&n, oscP(o, O_KeyMax)));
      const float vmin = V(&n, oscP(o, O_VelMin)), vmax = V(&n, oscP(o, O_VelMax));
      const float v127 = n.vel * 127.f;
      float key = static_cast<float>(n.key);
      if ((key < kmin || key > kmax) && V(&n, oscP(o, O_MapFold)) >= 0.5f && kmax >= kmin) {
        while (key < kmin) key += 12, fold += 12;
        while (key > kmax) key -= 12, fold -= 12;
        if (key < kmin) key = static_cast<float>(n.key), fold = 0;  // zone narrower than an octave: no fold fits
      }
      if (key < kmin || key > kmax || v127 < vmin - 0.5f || v127 > vmax + 0.5f) {
        if (V(&n, oscP(o, O_MapWarp)) >= 0.5f) s.silent = true;
        else s.on = false;
      }
    }
    s.type = static_cast<int>(V(&n, oscP(o, O_Type)));
    s.src = &aux_->osc[static_cast<size_t>(o)];
    s.mip = s.src->mip.get();
    if (!s.mip) {
      const int t = static_cast<int>(V(&n, oscP(o, O_Table)));
      const auto& fm = factoryMips();
      s.mip = fm[static_cast<size_t>(std::clamp(t - 1, 0, static_cast<int>(fm.size()) - 1))].get();
    }
    s.srv = srv_;
    s.pitch = oscPitch(n, o) + fold;
    double hz = 440.0 * std::exp2((s.pitch - 69.0) / 12.0);
    const int mode = static_cast<int>(V(&n, oscP(o, O_PitchMode)));
    const double ratio = V(&n, oscP(o, O_Ratio));
    if (mode == 3) hz *= ratio;
    else if (mode == 2) hz *= ratio >= 1 ? std::round(ratio) : 1.0 / std::round(1.0 / std::max(0.01, ratio));
    hz += V(&n, oscP(o, O_HzOffset));
    s.hz = hz;
    s.pitch = 69.0 + 12.0 * std::log2(std::max(0.01, std::fabs(hz)) / 440.0);
    const int U = std::clamp(static_cast<int>(std::lround(V(&n, oscP(o, O_Unison)))), 1, kMaxUnison);
    s.unison = U;
    const float detune = V(&n, oscP(o, O_UniDetune));
    const float range = V(&n, oscP(o, O_UniRange));
    const float spread = detune * detune * range;
    const float blend = V(&n, oscP(o, O_UniBlend)), width = V(&n, oscP(o, O_UniWidth)), span = V(&n, oscP(o, O_UniSpan));
    const int dmode = static_cast<int>(V(&n, oscP(o, O_DetuneMode)));
    const int stack = static_cast<int>(V(&n, oscP(o, O_UniStack)));
    const int rot = static_cast<int>(std::lround(V(&n, oscP(o, O_UniRotate)) * U));
    const bool vc = G(G_VcA + o) >= 0.5f;
    const float pan = std::clamp(V(&n, oscP(o, O_Pan)) + (vc ? n.vc.pan + n.rndPan : 0.f), -1.f, 1.f);
    float pl0, pr0;
    panGains(pan, pl0, pr0);
    float gsum = 0;
    const bool altMix = V(&n, tailP(T_UniMix)) >= 0.5f;
    // Alternating: each voice drops 0.25 dB per voice from -6.45 dB; two voices sit at -2 dB
    const float uniLevel = std::pow(10.f, (-6.45f - 0.25f * static_cast<float>(U)) / 20.f), altTwo = 0.794f;
    for (int u = 0; u < U; ++u) {
      const int ui = (u + rot) % U;
      float t = U == 1 ? 0.f : -1.f + 2.f * static_cast<float>(ui) / static_cast<float>(U - 1);
      float td = t;
      switch (dmode) {
        case 1: td = (t < 0 ? -1.f : 1.f) * std::pow(std::fabs(t), 0.7f); break;
        case 2: td = (t < 0 ? -1.f : 1.f) * (std::exp2(std::fabs(t) * 3.f) - 1.f) / 7.f; break;
        case 3: td = t == 0 ? 0.f : (t < 0 ? -1.f : 1.f) * (1.f - std::fabs(t) * 0.5f); break;
        case 4: td = U == 1 ? 0.f : n.uniRand[o][u]; break;
        default: break;
      }
      float semis = spread * (td + span) / (1.f + std::fabs(span));
      // stacking
      switch (stack) {
        case 1: semis += (u & 1) ? 12.f : 0.f; break;
        case 2: semis += static_cast<float>(u % 3) * 12.f; break;
        case 3: semis += static_cast<float>(u % 4) * 12.f; break;
        case 4: semis += (u & 1) ? 19.f : 0.f; break;
        case 5: { static const float st[4] = {0, 7, 12, 19}; semis += st[u % 4]; break; }
        case 6: { static const float st[6] = {0, 7, 12, 19, 24, 31}; semis += st[u % 6]; break; }
        case 7: if (U > 1 && std::fabs(t) < 1e-3f) semis -= 12.f; else if (U > 1 && u == U / 2) semis -= 12.f; break;
        case 8: if (U > 1 && u == U / 2) semis -= 24.f; break;
        default: break;
      }
      s.det[u] = semis;
      s.inc[u] = hz * std::exp2(semis / 12.0) / srv_;
      s.uw[u] = t;
      const bool center = U == 1 || (U & 1 ? u == U / 2 : (u == U / 2 || u == U / 2 - 1));
      if (altMix && U > 1) {
        // Alternating mix: voices below the middle go right, left, right... from the lowest,
        // voices above it left, right, left... from the highest; an odd count's centre voice
        // plays in both channels. Width moves the side voices in from hard left/right.
        const bool mid = (U & 1) && ui == U / 2, pair = !(U & 1) && (ui == U / 2 || ui == U / 2 - 1);
        const bool upper = 2 * ui > U - 1;
        const int j = upper ? U - 1 - ui : ui;
        const bool left = upper ? (j % 2 == 0) : (j % 2 == 1);
        float g;
        if (U == 2) g = altTwo;
        else g = uniLevel * (mid ? uniTab(kAltCentre, blend) : pair ? kAltPair * uniTab(kAltCentre, blend) : uniTab(kAltSide, blend));
        float l = 1.f, r = 1.f;
        if (!mid) {
          const float w = std::min(std::fabs(width), 1.f);
          const float nearG = 0.398f + 0.602f * std::pow(w, 1.2f), farG = 0.398f * std::pow(1.f - w, 1.25f);
          const bool toL = width < 0 ? !left : left;
          l = toL ? nearG : farG;
          r = toL ? farG : nearG;
        }
        s.pl[u] = g * l * pl0;
        s.pr[u] = g * r * pr0;
        continue;
      }
      const float g = center ? 1.f - blend * 0.5f : blend;
      // unison spread: linear balance (the main pan's curve would lift a wide unison)
      const float up = std::clamp(t * width, -1.f, 1.f);
      const float l = up > 0 ? 1.f - up : 1.f, r = up < 0 ? 1.f + up : 1.f;
      s.pl[u] = g * l * pl0;
      s.pr[u] = g * r * pr0;
      gsum += g * g;
    }
    const float norm = altMix && U > 1 ? 1.f : gsum > 0 ? 1.f / std::sqrt(gsum) : 1.f;
    for (int u = 0; u < U; ++u) s.pl[u] *= norm, s.pr[u] *= norm;
    s.wtPos = V(&n, oscP(o, O_WtPos));
    s.uniWt = V(&n, oscP(o, O_UniWtPos));
    s.uniW1 = V(&n, oscP(o, O_UniWarp1));
    s.uniW2 = V(&n, oscP(o, O_UniWarp2));
    s.w1 = static_cast<int>(V(&n, oscP(o, O_Warp1Mode)));
    s.w2 = static_cast<int>(V(&n, oscP(o, O_Warp2Mode)));
    if (!warpAllowed(s.type, s.w1)) s.w1 = 0;
    if (!warpAllowed(s.type, s.w2)) s.w2 = 0;
    s.a1 = V(&n, oscP(o, O_Warp1));
    s.a2 = V(&n, oscP(o, O_Warp2));
    s.v1 = V(&n, oscP(o, O_Warp1Var));
    s.v2 = V(&n, oscP(o, O_Warp2Var));
    s.smooth = V(&n, oscP(o, O_Smooth)) >= 0.5f;
    s.s1 = G(G_S1Mode) >= 0.5f;
    s.start = V(&n, oscP(o, O_Start));
    s.end = V(&n, oscP(o, O_End));
    s.reverse = V(&n, oscP(o, O_Reverse)) >= 0.5f;
    double scan = V(&n, oscP(o, O_ScanRate));
    if (V(&n, oscP(o, O_ScanSync)) >= 0.5f && s.src && s.src->sample) {
      // tempo scan: the start..end range passes once per division (the rate knob sets direction and multiplies)
      const double secs = s.src->sample->seconds() * std::max(0.01, static_cast<double>(V(&n, oscP(o, O_End)) - V(&n, oscP(o, O_Start))));
      const double divSec = divisionBeats(static_cast<int>(V(&n, oscP(o, O_ScanBpm)))) * 60.0 / tempo_;
      scan *= secs / std::max(1e-3, divSec);
    }
    if (V(&n, oscP(o, O_ScanKey)) >= 0.5f) scan *= std::exp2((n.pitch - 60.0) / 12.0);
    s.scan = static_cast<float>(scan);
    s.position = std::clamp(V(&n, oscP(o, O_Position)) + static_cast<float>(n.scan[o]), 0.f, 1.f);
    s.loopMode = static_cast<int>(V(&n, oscP(o, O_LoopMode)));
    s.loopStart = V(&n, oscP(o, O_LoopStart));
    s.loopEnd = V(&n, oscP(o, O_LoopEnd));
    s.loopXfade = V(&n, oscP(o, O_LoopXfade));
    s.relLoop = V(&n, oscP(o, O_RelLoop)) >= 0.5f;
    s.slice = static_cast<int>(std::lround(V(&n, oscP(o, O_Slice))));
    s.sliceMode = static_cast<int>(V(&n, oscP(o, O_SliceMode)));
    // granular density / length modes
    const int dm = static_cast<int>(V(&n, oscP(o, O_GDensMode)));
    const int lm = static_cast<int>(V(&n, oscP(o, O_GLenMode)));
    const float dv = V(&n, oscP(o, O_GDensity)), lv = V(&n, oscP(o, O_GLength));
    s.lengthMs = static_cast<float>(lm == 1 ? divisionBeats(static_cast<int>(std::lround((1 - lv) * 15))) * 60000.0 / tempo_
                                    : lm == 2 ? std::max(2.0, lv * (s.src && s.src->sample ? s.src->sample->seconds() * 1000.0 : 500.0))
                                              : grainLengthMs(lv));
    s.density = static_cast<float>(dm == 1 ? tempo_ / 60.0 / divisionBeats(static_cast<int>(std::lround((1 - dv) * 15)))
                                   : dm == 2 ? (1.0 + dv * 15.0) / (s.lengthMs * 0.001) : grainDensity(dv));
    s.grReverse = V(&n, oscP(o, O_GReverse));
    s.winShape = static_cast<int>(V(&n, oscP(o, O_GWinShape)));
    s.winSkew = V(&n, oscP(o, O_GWinSkew));
    s.winParam = V(&n, oscP(o, O_GWinParam));
    for (int i = 0; i < 10; ++i) s.rnd[i] = V(&n, oscP(o, O_GRndOffset + i));
    s.maxGrains = static_cast<int>(std::lround(V(&n, oscP(o, O_GMaxGrains))));
    s.grTrig = static_cast<int>(V(&n, oscP(o, O_GTrig)));
    s.sCut = V(&n, oscP(o, O_SCut));
    s.sMix = V(&n, oscP(o, O_SMix));
    s.sLo = V(&n, oscP(o, O_SLo));
    s.sHi = V(&n, oscP(o, O_SHi));
    s.phaseLock = V(&n, oscP(o, O_SPhaseLock)) >= 0.5f;
    s.transients = V(&n, oscP(o, O_STransients)) >= 0.5f;
    s.timbre = V(&n, oscP(o, O_MTimbre));
    s.velTrack = V(&n, oscP(o, O_MVelTrack));
    s.msLaw = G(tailP(T_MsLaw)) >= 0.5f;
    s.msOv = s.msLaw && V(&n, oscP(o, O_MEnvOn)) >= 0.5f;
    if (s.msOv) {
      s.ovDelay = static_cast<float>(envTime(V(&n, oscP(o, O_MDelay)))), s.ovAttack = static_cast<float>(envTime(V(&n, oscP(o, O_MAttack))));
      s.ovHold = static_cast<float>(envTime(V(&n, oscP(o, O_MHold)))), s.ovDecay = static_cast<float>(envTime(V(&n, oscP(o, O_MDecay))));
      s.ovSustain = V(&n, oscP(o, O_MSustain)), s.ovRelease = static_cast<float>(envTime(V(&n, oscP(o, O_MRelease))));
    }
  }

  // ---------------------------------------------------------------- one note, one control period
  // Control part: LFOs, glide, matrix, oscillator / filter / routing settings for `period` samples.
  void controlNote(Note& n, int period) {
    n.hasCtl = true;
    Ctl& c = n.ctl;
    const double dtSec = static_cast<double>(period) / sr_;
    // LFOs
    for (int l = 0; l < kLfos; ++l)
      if (G(lfoP(l, L_Mono)) < 0.5f) stepLfo(&n, n.lfo[l], l, dtSec, n.held, false);
    // glide
    if (n.glideLeft > 0) {
      n.glideLeft = std::max(0.0, n.glideLeft - dtSec);
      double t = n.glideTotal > 0 ? 1.0 - n.glideLeft / n.glideTotal : 1.0;
      const float c = G(G_PortaCurve);
      if (std::fabs(c) > 1e-3f) t = bendCurve(t, c);
      n.pitch = n.glideFrom + (n.glideTarget - n.glideFrom) * t;
    }
    // matrix
    evalMatrix(&n, n.off, static_cast<float>(dtSec));
    // settings
    OscSet* os = c.os;
    for (int o = 0; o < kOscs; ++o) {
      oscSettings(n, o, os[o]);
      // granular / spectral scan through the source
      if (os[o].type == OscGranular) n.scan[o] += os[o].scan * dtSec * 0.1;
      if (n.scan[o] > 1) n.scan[o] -= 1;
      if (n.scan[o] < -1) n.scan[o] += 1;
    }
    float* lev = c.lev;
    float* pl = c.pl;
    float* pr = c.pr;
    for (int o = 0; o < kOscs; ++o) {
      float g = static_cast<float>(levelGain(V(&n, oscP(o, O_Level))));
      if (os[o].type == OscMultisample && !os[o].msLaw) g *= 1.f - os[o].velTrack + os[o].velTrack * n.vel;
      lev[o] = g;
      pl[o] = pr[o] = 1;
    }
    c.subOn = V(&n, subP(S_Enable)) >= 0.5f;
    c.noiseOn = V(&n, noiseP(N_Enable)) >= 0.5f;
    lev[3] = static_cast<float>(levelGain(V(&n, noiseP(N_Level))));
    lev[4] = static_cast<float>(levelGain(V(&n, subP(S_Level))));
    panGains(V(&n, noiseP(N_Pan)) + (G(G_VcN) >= 0.5f ? n.vc.pan + n.rndPan : 0.f), pl[3], pr[3]);
    panGains(V(&n, subP(S_Pan)) + (G(G_VcS) >= 0.5f ? n.vc.pan + n.rndPan : 0.f), pl[4], pr[4]);
    c.subShape = static_cast<int>(V(&n, subP(S_Shape)));
    const double subPitch = (V(&n, subP(S_PitchTrack)) >= 0.5f ? n.pitch : 60.0) + std::round(V(&n, subP(S_Octave))) * 12.0 + V(&n, subP(S_Coarse)) + bendSemis(n) +
                            G(G_MasterTune) / 100.0 + (G(G_VcS) >= 0.5f ? n.vc.detune * 0.5 + n.rndDet / 100.0 : 0.0);
    c.subInc = 440.0 * std::exp2((subPitch - 69.0) / 12.0) / srv_;
    const int nType = static_cast<int>(V(&n, noiseP(N_Type)));
    const auto& fnz = factoryNoises();
    c.nsrc = nType >= 1 && nType - 1 < static_cast<int>(fnz.size()) ? &fnz[static_cast<size_t>(nType - 1)] : nullptr;
    c.nfile = nType == 0 && aux_->noiseFile ? aux_->noiseFile.get() : nullptr;
    const double nRoot = c.nfile ? c.nfile->sampleRate : 44100.0;
    const double nPitch = (V(&n, noiseP(N_PitchTrack)) >= 0.5f ? n.pitch - 60.0 : 0.0) + V(&n, noiseP(N_Pitch)) + V(&n, noiseP(N_Fine));
    c.nRate = std::exp2(nPitch / 12.0) * nRoot / srv_;
    c.nOne = V(&n, noiseP(N_OneShot)) >= 0.5f;
    c.nColor = V(&n, noiseP(N_Color));
    {
      // J-201 noise tone: a 2-pole Butterworth low-pass (tone < 0) or high-pass (tone > 0); the
      // corner follows the source's tone knob law: low-pass 2000 Hz * 2^(21.5 (x - 0.3)),
      // high-pass 430 Hz * 2^(21.5 (x - 0.7)), x = 0.5 + tone / 2 (measured on white noise)
      const float tone = std::clamp(V(&n, tailP(T_NoiseTone)), -1.f, 1.f);
      c.nTone = std::fabs(tone) > 1e-4f;
      if (c.nTone) {
        const double x = 0.5 + 0.5 * tone;
        const bool hp = tone > 0;
        const double fc = hp ? 430.0 * std::exp2(21.5 * (x - 0.7)) : 2000.0 * std::exp2(21.5 * (x - 0.3));
        const double w = 2.0 * 3.14159265358979323846 * std::clamp(fc, 5.0, 0.45 * srv_) / srv_;
        const double cw = std::cos(w), al = std::sin(w) / (2.0 * 0.70710678118654752), a0 = 1.0 + al;
        const double b0 = hp ? (1.0 + cw) * 0.5 : (1.0 - cw) * 0.5, b1 = hp ? -(1.0 + cw) : 1.0 - cw;
        c.nTk[0] = static_cast<float>(b0 / a0), c.nTk[1] = static_cast<float>(b1 / a0), c.nTk[2] = static_cast<float>(b0 / a0);
        c.nTk[3] = static_cast<float>(-2.0 * cw / a0), c.nTk[4] = static_cast<float>((1.0 - al) / a0);
      }
    }
    // routing
    int* dest = c.dest;
    float* bal2 = c.bal2;
    float* b1 = c.b1;
    float* b2 = c.b2;
    for (int r = 0; r < kRoutes; ++r) {
      dest[r] = static_cast<int>(V(&n, routeP(r, R_Dest)));
      bal2[r] = (V(&n, routeP(r, R_Balance)) + 1.f) * 0.5f;
      b1[r] = V(&n, routeP(r, R_Bus1));
      b2[r] = V(&n, routeP(r, R_Bus2));
    }
    // filters
    bool* fOn = c.fOn;
    float* fMix = c.fMix;
    float* fLev = c.fLev;
    for (int f = 0; f < kFilters; ++f) {
      fOn[f] = V(&n, filtP(f, F_Enable)) >= 0.5f;
      const int type = static_cast<int>(V(&n, filtP(f, F_Type)));
      double cutN = V(&n, filtP(f, F_Freq)) + V(&n, filtP(f, F_X)) * 0.25f;
      double hz = cutoffHz(std::clamp(cutN, 0.0, 1.0));
      if (V(&n, filtP(f, F_KeyTrack)) >= 0.5f) hz *= std::exp2((n.pitch - (G(tailP(T_KeyCentre)) >= 0.5f ? 32.0 : 60.0)) / 12.0);
      hz *= std::exp2((n.rndCut + n.vc.cutoff) * 2.0);
      hz = std::clamp(hz, 8.0, srv_ * 0.48);
      const double res = std::clamp(V(&n, filtP(f, F_Res)) + V(&n, filtP(f, F_Y)) * 0.25f, 0.f, 1.f);
      const double spread = (V(&n, filtP(f, F_Stereo)) - 0.5) * 48.0;
      Filter& flt = f == 0 ? *n.f1 : *n.f2;
      if (fOn[f]) flt.set(type, srv_, hz, res, V(&n, filtP(f, F_Drive)), V(&n, filtP(f, F_Var)), spread, G(tailP(T_DriveMode)) >= 0.5f);
      fMix[f] = V(&n, filtP(f, F_Wet));
      fLev[f] = static_cast<float>(busGain(V(&n, filtP(f, F_Level))));
      if (&n == lead_) live_.cut[f].store(static_cast<float>(hz), std::memory_order_relaxed);
    }
    c.serial = 0;  // 1: F1 -> F2, 2: F2 -> F1
    if (dest[RtF1] == 0) c.serial = 1;
    else if (dest[RtF2] == 0) c.serial = 2;
    c.amp = static_cast<float>(busGain(V(&n, G_Amp)));
    for (int e = 0; e < kEnvs; ++e) c.es[e] = envSet(&n, e);
    // multisample own envelope
    for (int o = 0; o < kOscs; ++o) c.menvOn[o] = V(&n, oscP(o, O_MEnvOn)) >= 0.5f;
  }

  // ---------------------------------------------------------------- one note, one chunk of audio
  void renderNote(Note& n, int frames, bool grid, int period) {
    if (grid || !n.hasCtl) controlNote(n, period);
    Ctl& c = n.ctl;
    OscSet* os = c.os;
    const float* lev = c.lev;
    const float* pl = c.pl;
    const float* pr = c.pr;
    const bool subOn = c.subOn, noiseOn = c.noiseOn;
    const int subShape = c.subShape;
    const double subInc = c.subInc;
    const NoiseSrc* nsrc = c.nsrc;
    const Sample* nfile = c.nfile;
    const double nRate = c.nRate;
    const bool nOne = c.nOne;
    const float nColor = c.nColor;
    const bool nTone = c.nTone;
    const float* nTk = c.nTk;
    const int* dest = c.dest;
    const float* bal2 = c.bal2;
    const float* b1 = c.b1;
    const float* b2 = c.b2;
    const bool* fOn = c.fOn;
    const float* fMix = c.fMix;
    const float* fLev = c.fLev;
    const int serial = c.serial;
    const EnvSet* es = c.es;
    const bool* menvOn = c.menvOn;
    const int vframes = frames * os_;
    float* outs[8] = {vbuf_[0].data(), vbuf_[1].data(), vbuf_[2].data(), vbuf_[3].data(), vbuf_[4].data(), vbuf_[5].data(), vbuf_[6].data(), vbuf_[7].data()};
    const float ampTarget = c.amp;
    const float ampSlew = G(G_NoSmooth) >= 0.5f ? 1.f : 0.01f;  // "Disable Smoothing": gain changes apply at once
    for (int i = 0; i < vframes; ++i) {
      for (int e = 0; e < kEnvs; ++e) envTick(n.env[e], es[e], srv_, n.held);
      // fm sources: previous sample outputs
      float sl[5] = {}, sr[5] = {}, mono[5] = {};
      for (int o = 0; o < kOscs; ++o) {
        float fm[6];
        const int a = o == 0 ? 1 : 0, b = o == 2 ? 1 : 2;
        fm[0] = n.last[a], fm[1] = n.last[b], fm[2] = n.last[3], fm[3] = n.last[4], fm[4] = n.last[5], fm[5] = n.last[6];
        if (!os[o].on) {
          n.last[o] = 0;
          continue;
        }
        n.osc[o].tick(os[o], fm, sl[o], sr[o], mono[o]);
        if (os[o].silent) {  // outside its zone: a warp source only
          n.last[o] = mono[o];
          sl[o] = sr[o] = 0;
          continue;
        }
        float g = lev[o];
        if (menvOn[o] && (os[o].type == OscSample || (os[o].type == OscMultisample && !os[o].msLaw))) g *= msEnv(n, o, frames);
        sl[o] *= g, sr[o] *= g;
        n.last[o] = mono[o];
      }
      if (noiseOn) {
        float a, b;
        n.noise.tick(nsrc, nfile, nRate, nOne, nColor, a, b);
        if (nTone) a = n.noise.tone(0, a, nTk), b = n.noise.tone(1, b, nTk);
        sl[3] = a * lev[3] * pl[3], sr[3] = b * lev[3] * pr[3];
        n.last[3] = (a + b) * 0.5f;
      }
      if (subOn) {
        const float x = n.sub.tick(subShape, subInc);
        sl[4] = x * lev[4] * pl[4], sr[4] = x * lev[4] * pr[4];
        n.last[4] = x;
      }
      float f1l = 0, f1r = 0, f2l = 0, f2r = 0, ml = 0, mr = 0, dl = 0, dr = 0, b1l = 0, b1r = 0, b2l = 0, b2r = 0;
      for (int r = 0; r < 5; ++r) {
        const float xl = sl[r], xr = sr[r];
        if (xl == 0 && xr == 0) continue;
        switch (dest[r]) {
          case RouteFilter: f1l += xl * (1 - bal2[r]), f1r += xr * (1 - bal2[r]), f2l += xl * bal2[r], f2r += xr * bal2[r]; break;
          case RouteMain: ml += xl, mr += xr; break;
          case RouteDirect: dl += xl, dr += xr; break;
          default: break;
        }
        b1l += xl * b1[r], b1r += xr * b1[r], b2l += xl * b2[r], b2r += xr * b2[r];
      }
      auto runF = [&](int f, float& l, float& r) {
        if (!fOn[f]) return;
        float a = l, b = r;
        (f == 0 ? *n.f1 : *n.f2).process(a, b);
        l = (l + (a - l) * fMix[f]) * fLev[f];
        r = (r + (b - r) * fMix[f]) * fLev[f];
      };
      auto routeF = [&](int f, float l, float r) {
        const int rt = f == 0 ? RtF1 : RtF2;
        switch (dest[rt]) {
          case 1: ml += l, mr += r; break;
          case 2: dl += l, dr += r; break;
          default: break;
        }
        b1l += l * b1[rt], b1r += r * b1[rt], b2l += l * b2[rt], b2r += r * b2[rt];
      };
      if (serial == 1) {
        runF(0, f1l, f1r);
        n.last[5] = (f1l + f1r) * 0.5f;
        f2l += f1l, f2r += f1r;
        b1l += f1l * b1[RtF1], b1r += f1r * b1[RtF1], b2l += f1l * b2[RtF1], b2r += f1r * b2[RtF1];
        runF(1, f2l, f2r);
        n.last[6] = (f2l + f2r) * 0.5f;
        routeF(1, f2l, f2r);
      } else if (serial == 2) {
        runF(1, f2l, f2r);
        n.last[6] = (f2l + f2r) * 0.5f;
        f1l += f2l, f1r += f2r;
        b1l += f2l * b1[RtF2], b1r += f2r * b1[RtF2], b2l += f2l * b2[RtF2], b2r += f2r * b2[RtF2];
        runF(0, f1l, f1r);
        n.last[5] = (f1l + f1r) * 0.5f;
        routeF(0, f1l, f1r);
      } else {
        runF(0, f1l, f1r);
        runF(1, f2l, f2r);
        n.last[5] = (f1l + f1r) * 0.5f;
        n.last[6] = (f2l + f2r) * 0.5f;
        routeF(0, f1l, f1r);
        routeF(1, f2l, f2r);
      }
      // amp: ENV 1 x amp, steal fade, smoothed per sample
      n.ampPrev += (ampTarget - n.ampPrev) * ampSlew;
      float g = static_cast<float>(n.env[0].level) * n.ampPrev;
      if (n.stealing) {
        n.fade -= n.fadeStep;
        if (n.fade <= 0) {
          n.fade = 0;
          n.active = false;
        }
        g *= std::max(0.f, n.fade);
      }
      outs[0][i] += ml * g, outs[1][i] += mr * g, outs[2][i] += dl * g, outs[3][i] += dr * g;
      outs[4][i] += b1l * g, outs[5][i] += b1r * g, outs[6][i] += b2l * g, outs[7][i] += b2r * g;
    }
    // a non-finite value (extreme settings) must not stay in the note's state
    float chk = 0;
    for (float v : n.last) chk += v;
    if (!std::isfinite(chk)) {
      n.f1->reset();
      n.f2->reset();
      for (float& v : n.last) v = 0;
    }
    // end of the note: ENV 1 finished, or every source done
    if (!n.held && n.env[0].stage == 0) n.active = false;
    if (n.active && n.env[0].stage == 0 && n.env[0].level <= 0 && !n.held) n.active = false;
    if (!n.active)
      for (int o = 0; o < kOscs; ++o)
        if (static_cast<int>(G(oscP(o, O_PhaseMemory))) > 0) n.osc[o].savePhases(phaseMem_[o]);
    if (!n.active) subPhase_ = n.sub.ph;
  }
  float msEnv(Note& n, int o, int frames) {
    // multisample / sample own envelope (D A H D S R), advanced per sample at the voice rate
    (void)frames;
    const double srv = srv_;
    float& lv = n.menv[o];
    int& st = n.menvStage[o];
    const double tD = envTime(V(&n, oscP(o, O_MDelay))), tA = envTime(V(&n, oscP(o, O_MAttack))), tH = envTime(V(&n, oscP(o, O_MHold)));
    const double tDe = envTime(V(&n, oscP(o, O_MDecay))), sus = levelGain(V(&n, oscP(o, O_MSustain))), tR = envTime(V(&n, oscP(o, O_MRelease)));
    double& tt = n.menvT[o];
    if (!n.held && st < 6) st = 6;
    switch (st) {
      case 1:  // delay
        lv = 0;
        tt += 1.0 / srv;
        if (tt >= tD) st = 2, tt = 0;
        break;
      case 2: lv += static_cast<float>(1.0 / std::max(1.0, tA * srv)); if (lv >= 1) lv = 1, st = 3, tt = 0; break;
      case 3:  // hold
        tt += 1.0 / srv;
        if (tt >= tH) st = 4;
        break;
      case 4: lv -= static_cast<float>((1.0 - sus) / std::max(1.0, tDe * srv)); if (lv <= sus) lv = static_cast<float>(sus), st = 5; break;
      case 5: lv = static_cast<float>(sus); break;
      default: lv -= static_cast<float>(std::max(1e-6, static_cast<double>(lv)) / std::max(1.0, tR * srv) * 4.0); if (lv < 0) lv = 0; break;
    }
    return lv;
  }

  // ---------------------------------------------------------------- sequencers
  void runSequencers(int m) {
    const bool arpOn = G(G_ArpOn) >= 0.5f, clipOn = G(G_ClipOn) >= 0.5f;
    if (!aux_ || (!arpOn && !clipOn)) {
      if (arpWas_) arp_.allOff(), flushSeq(seqOut_);
      arpWas_ = false;
      return;
    }
    arpWas_ = true;
    seqOut_.n = 0;
    const int midiOut = static_cast<int>(G(G_MidiOut));
    if (clipOn) {
      const ClipSlot& c = aux_->model.clip[static_cast<size_t>(activeClip())];
      float offs[3] = {GV(G_ClipTranspose), GV(G_ClipRate), GV(G_ClipOffset)};
      clipSeq_.n = 0;
      clip_.run(c, offs, m, sr_, tempo_, beat_, playing_, G(G_Swing), static_cast<int>(G(G_SwingDiv)), clipSeq_, laneVal_, 32);
      // clip lanes: automation values override their target parameters
      for (int l = 0; l < std::min(32, static_cast<int>(c.lanes.size())); ++l) {
        const int t = c.lanes[static_cast<size_t>(l)].target;
        if (t >= 0 && t < kParamCount && laneVal_[l] >= 0) base_[static_cast<size_t>(t)] = fromNorm(pmaps()[static_cast<size_t>(t)], laneVal_[l]);
      }
      for (int i = 0; i < clipSeq_.n; ++i) {
        const SeqNote& e = clipSeq_.ev[i];
        if (midiOut >= 1) midi(e);
        if (arpOn) {
          if (e.on) arp_.noteOn(e.key, e.vel);
          else arp_.noteOff(e.key);
        } else {
          seqOut_.push(e);
        }
      }
      live_.clipPos.store(clip_.position(), std::memory_order_relaxed);
    }
    if (arpOn) {
      const ArpSlot& s = aux_->model.arp[static_cast<size_t>(activeArp())];
      float offs[14];
      for (int i = 0; i < 14; ++i) offs[i] = GV(G_ArpRate + i);
      arp_.run(s, offs, m, sr_, tempo_, beat_, playing_, G(G_Swing), static_cast<int>(G(G_SwingDiv)), scaleBuf_, static_cast<int>(G(G_Key)), seqOut_);
      live_.arpStep.store(arp_.step(), std::memory_order_relaxed);
      if (midiOut == 2)
        for (int i = 0; i < seqOut_.n; ++i) midi(seqOut_.ev[i]);
    }
    flushSeq(seqOut_);
  }
  void midi(const SeqNote& e) {
    if (midiCount_ >= 256) return;
    midiOut_[midiCount_++] = {e.offset, static_cast<uint8_t>(e.on ? 0x90 : 0x80), static_cast<uint8_t>(e.key), static_cast<uint8_t>(e.on ? std::clamp(static_cast<int>(e.vel * 127), 1, 127) : 0)};
  }
  void flushSeq(SeqOut& o) {
    for (int i = 0; i < o.n; ++i) {
      const SeqNote& e = o.ev[i];
      const uint32_t id = 0xA0000000u | static_cast<uint32_t>(e.key);
      if (e.on) playNote(id, e.key, e.vel, NoteProps{});
      else stopNote(id);
    }
    o.n = 0;
  }

  // ---------------------------------------------------------------- chunk
  void renderChunk(float* L, float* R, int m, bool grid, int period) {
    const int vframes = m * os_;
    for (auto& b : vbuf_) std::fill(b.begin(), b.begin() + vframes, 0.f);
    // global LFOs (mono LFOs, free-running phases, the global matrix context): on the control grid
    const double dt = period / sr_;
    if (grid)
      for (int l = 0; l < kLfos; ++l) stepLfo(nullptr, globalLfo_[l], l, dt, true, true);
    // lead note: newest sounding note
    lead_ = nullptr;
    for (auto& n : notes_)
      if (n.active && !n.stealing && (!lead_ || n.age > lead_->age)) lead_ = &n;
    for (auto& n : notes_)
      if (n.active) renderNote(n, m, grid, period);
    if (grid) {
      if (lead_ && lead_->active) {
        for (int d = 0; d < destCount_; ++d) goff_[d] = lead_->off[d];
      } else {
        evalMatrix(nullptr, goff_, static_cast<float>(dt));
      }
    }
    if (lead_ && !lead_->active) lead_ = nullptr;
    // decimate the voice buses to the base rate
    float bus[8][kCtl];
    for (int c = 0; c < 8; ++c) {
      const float* src = vbuf_[static_cast<size_t>(c)].data();
      if (os_ == 1) {
        std::copy(src, src + m, bus[c]);
      } else if (os_ == 2) {
        for (int i = 0; i < m; ++i) bus[c][i] = decim_[c].push2(src[2 * i], src[2 * i + 1]);
      } else {
        for (int i = 0; i < m; ++i) {
          const float a = decim_[8 + c].push2(src[4 * i], src[4 * i + 1]);
          const float b = decim_[8 + c].push2(src[4 * i + 2], src[4 * i + 3]);
          bus[c][i] = decim_[c].push2(a, b);
        }
      }
    }
    if (gFxInputHook) {
      float* const bp[8] = {bus[0], bus[1], bus[2], bus[3], bus[4], bus[5], bus[6], bus[7]};
      gFxInputHook(bp, m);
    }
    // effect racks
    FxContext fc;
    fc.sr = sr_, fc.tempo = tempo_, fc.beat = beat_, fc.playing = playing_, fc.driveBoost = G(tailP(T_DriveMode)) >= 0.5f;
    float b1v = static_cast<float>(busGain(GV(G_Bus1Vol))), b2v = static_cast<float>(busGain(GV(G_Bus2Vol)));
    runRack(1, bus[4], bus[5], m, fc);
    runRack(2, bus[6], bus[7], m, fc);
    for (int i = 0; i < m; ++i) {
      if (G(G_Bus1Dest) < 0.5f) bus[0][i] += bus[4][i] * b1v, bus[1][i] += bus[5][i] * b1v;
      else bus[2][i] += bus[4][i] * b1v, bus[3][i] += bus[5][i] * b1v;
      if (G(G_Bus2Dest) < 0.5f) bus[0][i] += bus[6][i] * b2v, bus[1][i] += bus[7][i] * b2v;
      else bus[2][i] += bus[6][i] * b2v, bus[3][i] += bus[7][i] * b2v;
    }
    runRack(0, bus[0], bus[1], m, fc);
    const float dv = static_cast<float>(busGain(GV(G_DirectVol)));
    const float mv = static_cast<float>(masterGain(GV(G_MasterVol)));
    float pk = 0;
    for (int i = 0; i < m; ++i) {
      float l = (bus[0][i] + bus[2][i] * dv) * mv;
      float r = (bus[1][i] + bus[3][i] * dv) * mv;
      if (!std::isfinite(l) || !std::isfinite(r)) l = r = 0;
      L[i] += l;
      R[i] += r;
      pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
    }
    outPeak_ = std::max(pk, outPeak_ * 0.95f);
    // effect tails keep the instrument alive after the last note
    if (pk > 1e-6f || activeCount() > 0) tailLeft_ = static_cast<int>(sr_ * 0.25);
    else if (tailLeft_ > 0) tailLeft_ = std::max(0, tailLeft_ - m);
  }
  float fxParam(int slot, int p) const { return std::clamp(GV(fxP(slot, X_P0 + p)), 0.f, 1.f); }
  int fxType(int slot) const { return std::clamp(static_cast<int>(std::lround(G(fxP(slot, X_Type)))), 0, FxTypeCount - 1); }
  void runRack(int rack, float* L, float* R, int m, const FxContext& fc) {
    if (!bank_) return;
    for (const auto& node : aux_->fx.empty() ? fxOrder_ : aux_->fx) (void)node;
    const std::vector<FxNode>& order = rackOrder();
    for (size_t i = 0; i < order.size(); ++i) {
      const FxNode& nd = order[i];
      if (nd.rack != rack || nd.parent >= 0) continue;
      runNode(nd, order, L, R, m, fc);
    }
  }
  const std::vector<FxNode>& rackOrder() {
    // resolved order (model order + typed slots not listed)
    fxOrder_.clear();
    std::array<bool, kFxSlots> seen{};
    for (const auto& nd : aux_->model.fx) {
      if (nd.slot < 0 || nd.slot >= kFxSlots || seen[static_cast<size_t>(nd.slot)] || fxType(nd.slot) == FxNone) continue;
      seen[static_cast<size_t>(nd.slot)] = true;
      fxOrder_.push_back(nd);
    }
    for (int s = 0; s < kFxSlots; ++s)
      if (!seen[static_cast<size_t>(s)] && fxType(s) != FxNone) fxOrder_.push_back({s, 0, -1, 0});
    return fxOrder_;
  }
  void runNode(const FxNode& nd, const std::vector<FxNode>& order, float* L, float* R, int m, const FxContext& fc) {
    const int type = fxType(nd.slot);
    if (type == FxNone || G(fxP(nd.slot, X_On)) < 0.5f) return;
    FxUnit* u = bank_->unit[static_cast<size_t>(nd.slot)].get();
    float p[kFxParams];
    for (int q = 0; q < kFxParams; ++q) p[q] = fxParam(nd.slot, q);
    if (type >= FxSplitLH) {
      const int bands = type == FxSplitLMH ? 3 : 2;
      float bl[3][kCtl], br[3][kCtl];
      if (type == FxSplitMS) {
        for (int i = 0; i < m; ++i) {
          bl[0][i] = br[0][i] = (L[i] + R[i]) * 0.5f;
          bl[1][i] = (L[i] - R[i]) * 0.5f, br[1][i] = -bl[1][i];
        }
      } else {
        Lr4& x1 = split_[nd.slot][0];
        Lr4& x2 = split_[nd.slot][1];
        x1.set(sr_, fxPlain(type, 0, p[0]));
        if (bands == 3) x2.set(sr_, std::max(fxPlain(type, 0, p[0]) * 1.2, fxPlain(type, 1, p[1])));
        for (int i = 0; i < m; ++i) {
          double lo, hi, lo2, hi2;
          x1.split(0, L[i], lo, hi);
          x1.split(1, R[i], lo2, hi2);
          bl[0][i] = static_cast<float>(lo), br[0][i] = static_cast<float>(lo2);
          if (bands == 3) {
            double mlo, mhi, mlo2, mhi2;
            x2.split(0, hi, mlo, mhi);
            x2.split(1, hi2, mlo2, mhi2);
            bl[1][i] = static_cast<float>(mlo), br[1][i] = static_cast<float>(mlo2);
            bl[2][i] = static_cast<float>(mhi), br[2][i] = static_cast<float>(mhi2);
          } else {
            bl[1][i] = static_cast<float>(hi), br[1][i] = static_cast<float>(hi2);
          }
        }
      }
      const int byp0 = fxParamIndex(type, "kParamBand1Bypass");
      for (int b = 0; b < bands; ++b) {
        const bool bypass = byp0 >= 0 && fxPlain(type, byp0 + b, p[byp0 + b]) >= 0.5;
        if (bypass) continue;
        for (const auto& c : order)
          if (c.parent == nd.slot && c.band == b && c.rack == nd.rack) runNode(c, order, bl[b], br[b], m, fc);
      }
      for (int i = 0; i < m; ++i) {
        if (type == FxSplitMS) {
          L[i] = bl[0][i] + bl[1][i];
          R[i] = br[0][i] + br[1][i];
        } else {
          L[i] = bl[0][i] + bl[1][i] + (bands == 3 ? bl[2][i] : 0.f);
          R[i] = br[0][i] + br[1][i] + (bands == 3 ? br[2][i] : 0.f);
        }
      }
      if (u) fxProcess(*u, type, p, L, R, m, fc, aux_, nd.slot);  // level
      return;
    }
    if (!u) return;
    fxProcess(*u, type, p, L, R, m, fc, aux_, nd.slot);
  }

  void publishLive() {
    live_.voices.store(activeCount(), std::memory_order_relaxed);
    const Note* n = lead_;
    for (int o = 0; o < kOscs; ++o) {
      live_.oscPos[o].store(n ? static_cast<float>(n->osc[o].displayPos()) : GV(oscP(o, O_WtPos)), std::memory_order_relaxed);
      live_.oscOn[o].store(n ? 1 : 0, std::memory_order_relaxed);
    }
    for (int e = 0; e < kEnvs; ++e) {
      live_.env[e].store(n ? static_cast<float>(n->env[e].level) : 0.f, std::memory_order_relaxed);
      live_.envStage[e].store(n ? n->env[e].stage : 0, std::memory_order_relaxed);
      live_.envTime[e].store(n ? static_cast<float>(n->env[e].secs) : 0.f, std::memory_order_relaxed);
    }
    for (int l = 0; l < kLfos; ++l) {
      const bool mono = G(lfoP(l, L_Mono)) >= 0.5f || !n;
      const LfoState& s = mono ? globalLfo_[l] : n->lfo[l];
      live_.lfo[l].store(static_cast<float>(s.out), std::memory_order_relaxed);
      live_.lfoPh[l].store(static_cast<float>(s.phase), std::memory_order_relaxed);
    }
    for (int s = 0; s < kModSlots; ++s) live_.mod[s].store(n ? n->modOut[s] : gmodOut_[s], std::memory_order_relaxed);
    // every source as the matrix sees it for the lead note (the source meters on the tabs);
    // with no note sounding, per-note properties keep the last note's value
    for (int s = SrcNone + 1; s < SourceCount; ++s) {
      if (!n && heldNoteSource(s)) continue;
      live_.src[s].store(sourceValue(n, s), std::memory_order_relaxed);
    }
    if (bank_)
      for (int s = 0; s < kFxSlots; ++s)
        live_.fx[s].store(bank_->unit[static_cast<size_t>(s)] ? fxMeter(*bank_->unit[static_cast<size_t>(s)]) : 0.f, std::memory_order_relaxed);
    live_.outL.store(outPeak_, std::memory_order_relaxed);
  }

  struct HeldKey {
    uint32_t id;
    int key;
    double pitch;
  };

  const Aux* aux_ = nullptr;
  int lastArpActive_ = 0, lastClipActive_ = 0;
  int arpSel_ = -1, clipSel_ = -1;  // slot chosen by the trigger octave (-1: the model's active slot)
  int activeArp() const { return arpSel_ >= 0 ? std::clamp(arpSel_, 0, kArpSlots - 1) : aux_->model.arpActive; }
  int activeClip() const { return clipSel_ >= 0 ? std::clamp(clipSel_, 0, kClipSlots - 1) : aux_->model.clipActive; }
  std::shared_ptr<const Aux> hostAux_;
  double sr_ = 48000, srv_ = 96000, tempo_ = 120, beat_ = 0;
  bool playing_ = false;
  int os_ = 2, prevOs_ = 2;
  std::vector<float> base_;
  std::vector<int> destOf_;
  int destParam_[kModSlots] = {};
  Slot slots_[kModSlots];
  int slotCount_ = 0, destCount_ = 0;
  bool audioSrc_ = false;
  float goff_[kModSlots];
  float gslew_[kModSlots] = {};
  float gmodOut_[kModSlots] = {};
  std::array<Note, kVoices> notes_;
  Note* lead_ = nullptr;
  LfoState globalLfo_[kLfos];
  std::array<std::vector<float>, 8> vbuf_;
  Decim decim_[16];
  std::vector<HeldKey> held_ = std::vector<HeldKey>(0);
  std::array<std::pair<uint32_t, int>, 128> idKeys_{};
  int heldExternal_ = 0;
  double recClock_ = 0;
  std::array<std::pair<uint32_t, int>, 32> recIds_{};  // recorded notes held (id, key)
  double lastPitch_ = -1;
  uint64_t ageCounter_ = 0;
  uint32_t rng_ = 0x2468ACEu;
  bool alt_ = false;
  int alt2_ = 0, vcStep_ = -1;
  double phaseMem_[kOscs][kMaxUnison] = {};
  double subPhase_ = 0;
  float wheel_ = 0, modWheel_ = 0, pressure_ = 0;
  float chanBend_[16] = {}, chanY_[16] = {}, chanZ_[16] = {};
  bool sustain_ = false;
  std::vector<int> scaleBuf_;
  Arp arp_;
  ClipPlayer clip_;
  bool arpWas_ = false;
  SeqOut seqOut_, clipSeq_;
  float laneVal_[32] = {};
  MidiOutEvent midiOut_[256];
  int midiCount_ = 0;
  // effects
  FxBank* bank_ = nullptr;
  FxBank* parkedBank_ = nullptr;
  std::atomic<FxBank*> pending_;
  std::array<std::atomic<FxBank*>, 4> retired_;
  std::string bankSig_;
  std::vector<FxNode> fxOrder_ = std::vector<FxNode>(0);
  Lr4 split_[kFxSlots][2];
  int tailLeft_ = 0, ctlLeft_ = 0;
  float outPeak_ = 0;
  Live live_;
};

}  // namespace

// ================================================================== entry points
bool liveView(const Generator* g, LiveView& out) {
  const auto* p = dynamic_cast<const PlasmaGen*>(g);
  if (!p) return false;
  const auto& l = p->live();
  out.voices = l.voices.load(std::memory_order_relaxed);
  for (int o = 0; o < kOscs; ++o) out.oscPos[o] = l.oscPos[o].load(std::memory_order_relaxed), out.oscOn[o] = l.oscOn[o].load(std::memory_order_relaxed) != 0;
  for (int e = 0; e < kEnvs; ++e) {
    out.env[e] = l.env[e].load(std::memory_order_relaxed);
    out.envStage[e] = l.envStage[e].load(std::memory_order_relaxed);
    out.envTime[e] = l.envTime[e].load(std::memory_order_relaxed);
  }
  for (int i = 0; i < kLfos; ++i) out.lfo[i] = l.lfo[i].load(std::memory_order_relaxed), out.lfoPhase[i] = l.lfoPh[i].load(std::memory_order_relaxed);
  for (int s = 0; s < kFxSlots; ++s) out.fxMeter[s] = l.fx[s].load(std::memory_order_relaxed);
  for (int s = 0; s < kModSlots; ++s) out.mod[s] = l.mod[s].load(std::memory_order_relaxed);
  for (int f = 0; f < kFilters; ++f) out.filterCut[f] = l.cut[f].load(std::memory_order_relaxed);
  out.outL = out.outR = l.outL.load(std::memory_order_relaxed);
  out.arpStep = l.arpStep.load(std::memory_order_relaxed);
  out.clipPos = l.clipPos.load(std::memory_order_relaxed);
  for (int s = 0; s < SourceCount; ++s) out.source[s] = l.src[s].load(std::memory_order_relaxed);
  return true;
}

float sourceMeterValue(int src, float raw, bool& bipolar) {
  if (!std::isfinite(raw)) raw = 0;
  // already signed: pitch bend and the audio-rate sources
  if (src == SrcPitchBend || (src >= SrcNoise && src <= SrcSub) || src == SrcFilter1 || src == SrcFilter2) {
    bipolar = true;
    return std::clamp(raw, -1.f, 1.f);
  }
  // 0..1 outputs that swing around the middle: LFOs (X and Y), the voice spread, MPE X
  if ((src >= SrcLfo1 && src <= SrcLfo10Y) || src == SrcVoiceMod1 || src == SrcVoiceMod2 || src == SrcExprX) {
    bipolar = true;
    return std::clamp(raw * 2.f - 1.f, -1.f, 1.f);
  }
  bipolar = false;
  return std::clamp(raw, 0.f, 1.f);
}

void previewNote(Generator* g, int key, bool on) {
  auto* p = dynamic_cast<PlasmaGen*>(g);
  if (!p) return;
  if (on) p->previewKey_.store(std::clamp(key, 0, 127));
  else p->previewOff_.store(std::clamp(key, 0, 127));
}

void setClipRecording(Generator* g, bool on) {
  if (auto* p = dynamic_cast<PlasmaGen*>(g)) p->recording_.store(on, std::memory_order_relaxed);
}

int recordedEvents(const Generator* g, uint32_t& readIndex, RecordedEvent* out, int max) {
  const auto* p = dynamic_cast<const PlasmaGen*>(g);
  if (!p) return 0;
  const uint32_t w = p->recWrite_.load(std::memory_order_acquire);
  if (w - readIndex > PlasmaGen::kRecRing) readIndex = w - PlasmaGen::kRecRing;  // overrun: keep the newest
  int n = 0;
  while (readIndex != w && n < max) out[n++] = p->recRing_[readIndex++ % PlasmaGen::kRecRing];
  return n;
}

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type != kType) return nullptr;
  (void)pmaps();
  return std::make_unique<PlasmaGen>();
}

std::unique_ptr<Effect> createEffect(const std::string&) { return nullptr; }

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != kType) return false;
  aux.samples.clear();
  aux.custom = buildAux(state);
  return true;
}

}  // namespace gl::packs::plasma

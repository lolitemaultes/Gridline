// Gridline — Resynth (gl.harmor) runtime.
//
// Per voice and part: a bank of sine partials (up to 256 including unison
// copies) whose levels and frequencies are recomputed every 32 samples from
// the timbre / image layer, the shaping curves and the articulators, then
// ramped per sample. Curves and images are prepared on the UI thread
// (packs::prepareAux) into flat per-harmonic tables.
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include "../../engine/Sample.h"
#include "harmor.h"
#include "models_rt.h"

namespace gl::packs::models {

namespace {

using namespace dsp;
using namespace hm;
constexpr int kCtl = 32;
constexpr int kVoices = 12;
constexpr int kMaxUni = 9;
constexpr int kEqPts = 128;  // frequency-axis tables (20 Hz .. 20 kHz, log)

double l11(double y) { return 0.1 * (std::pow(11.0, std::clamp(y, 0.0, 1.0)) - 1.0); }

// ------------------------------------------------------------ prepared data
struct ArtPrep {
  bool bip = false, envOn = false, lfoOn = false, any = false;
  Curve env, lfo;
  double envLen = 0, lfoLen = 1, susX = -1, loopX = -1;
  float att = 1, dec = 1, sus = 0, rel = 1, lfoSpeed = 0.5f, lfoPhase = 0, smooth = 0;
  bool lfoTempo = false, envTempo = false, lfoGlobal = false;
  Curve maps[kMaps];
  bool mapFlat[kMaps] = {};
  float mapConst[kMaps] = {};
};
struct PartPrep {
  std::array<ArtPrep, kArts> arts;
  std::array<std::array<float, kHarm>, kShapes> sh{};  // per-harmonic shape values
  std::array<float, kEqPts> localEq{}, globalEq{};
  Curve filterCurve[2], resCurve[2], phaserCurve;
  // Image (gain 0..1, freq offset in octaves), column-major.
  int cols = 0, rows = 0, loop = 1;
  float colSec = 0.01f;
  std::vector<float> gain, freq;
};
struct HarmorAux final : AuxData {
  PartPrep part[kParts];
};

void prepCurveTables(const PartData& d, PartPrep& p) {
  for (int s = 0; s < kShapes; ++s)
    for (int k = 1; k <= kHarm; ++k)
      p.sh[static_cast<size_t>(s)][static_cast<size_t>(k - 1)] =
          static_cast<float>(shapeAtHarmonic(d.shapes[static_cast<size_t>(s)], k, kHarm, neutralShape(s)));
  for (int i = 0; i < kEqPts; ++i) {
    const double hz = 20.0 * std::pow(1000.0, i / (kEqPts - 1.0));
    p.localEq[static_cast<size_t>(i)] = static_cast<float>(shapeAtHz(d.shapes[ShLocalEq], hz, 0.25f));
    p.globalEq[static_cast<size_t>(i)] = static_cast<float>(shapeAtHz(d.shapes[ShGlobalEq], hz, 0.25f));
  }
  p.filterCurve[0] = d.shapes[ShFilter1], p.filterCurve[1] = d.shapes[ShFilter2];
  p.resCurve[0] = d.shapes[ShRes1], p.resCurve[1] = d.shapes[ShRes2];
  p.phaserCurve = d.shapes[ShPhaser];
  for (int a = 0; a < kArts; ++a) {
    const ArtData& s = d.arts[static_cast<size_t>(a)];
    ArtPrep& r = p.arts[static_cast<size_t>(a)];
    r.bip = s.bipolar;
    r.env = s.env, r.lfo = s.lfo;
    r.envOn = s.env.enabled && !s.env.pts.empty();
    r.lfoOn = s.lfo.enabled && !s.lfo.pts.empty();
    r.envLen = curveLength(s.env);
    r.lfoLen = std::max(1e-6, curveLength(s.lfo));
    auto xOf = [&](int idx) {
      double x = 0;
      for (int i = 1; i <= idx && i < static_cast<int>(s.env.pts.size()); ++i) x += s.env.pts[static_cast<size_t>(i)].dt;
      return x;
    };
    r.susX = s.env.sustain >= 0 ? xOf(s.env.sustain) : -1;
    r.loopX = s.env.loopStart >= 0 && s.env.sustain >= 0 ? xOf(s.env.loopStart) : -1;
    r.att = s.att, r.dec = s.dec, r.sus = s.sus, r.rel = s.rel;
    r.lfoSpeed = s.lfoSpeed, r.lfoPhase = s.lfoPhase, r.smooth = s.smooth;
    r.envTempo = s.env.tempo, r.lfoTempo = s.lfo.tempo, r.lfoGlobal = s.lfo.global;
    const float neutral = r.bip ? 0.5f : 1.0f;
    bool anyMap = false;
    for (int m = 0; m < kMaps; ++m) {
      const Curve& c = s.maps[static_cast<size_t>(m)];
      r.maps[m] = c;
      bool flat = true;
      for (const auto& pt : c.pts) flat = flat && std::fabs(pt.y - (c.pts.empty() ? neutral : c.pts[0].y)) < 1e-5f;
      r.mapFlat[m] = flat;
      r.mapConst[m] = c.pts.empty() ? neutral : c.pts[0].y;
      anyMap = anyMap || !flat || std::fabs(r.mapConst[m] - neutral) > 1e-5f;
    }
    r.any = r.envOn || r.lfoOn || anyMap || a == ArVol;
  }
}

std::mutex& audioMutex() {
  static std::mutex m;
  return m;
}

// Image from the data, or re-analysed from its source file (cached by path).
void prepImage(const ImageData& im, PartPrep& p) {
  ImageData src = im;
  if (src.gain.empty() && !src.source.empty()) {
    static std::map<std::string, ImageData> cache;
    std::lock_guard<std::mutex> lk(audioMutex());
    auto it = cache.find(src.source);
    if (it != cache.end()) {
      src = it->second;
    } else {
      ImageData out;
      if (SamplePtr smp = sampleCache().get(src.source)) {
        std::vector<float> mono(smp->left);
        for (size_t i = 0; i < mono.size() && i < smp->right.size(); ++i) mono[i] = 0.5f * (mono[i] + smp->right[i]);
        analyzeAudio(mono, smp->sampleRate, 255, 0.01, out);
      } else {
        imageFromPicture(src.source, 128, out);
      }
      if (cache.size() > 64) cache.clear();
      cache[src.source] = out;
      src = out;
    }
  }
  if (src.gain.empty()) return;
  p.cols = src.cols, p.rows = std::min(src.rows, kHarm), p.loop = src.loop, p.colSec = src.colSec;
  p.gain.assign(static_cast<size_t>(p.cols) * static_cast<size_t>(p.rows), 0.0f);
  p.freq.clear();
  if (!src.freq.empty()) p.freq.assign(p.gain.size(), 0.0f);
  for (int c = 0; c < p.cols; ++c)
    for (int r = 0; r < p.rows; ++r) {
      const size_t s = static_cast<size_t>(c) * static_cast<size_t>(src.rows) + static_cast<size_t>(r);
      const size_t d = static_cast<size_t>(c) * static_cast<size_t>(p.rows) + static_cast<size_t>(r);
      p.gain[d] = src.gain[s] / 255.0f;
      if (!p.freq.empty()) p.freq[d] = src.freq[s] / 64.0f;
    }
}

// ------------------------------------------------------------ part settings
struct PartCfg {
  bool on = false;
  float phase = 0.5f, phaseRand = 0, timbreMix = 0, sub[3] = {0, 0, 0}, protect = 0, clip = 1, fxMix = 0, vol = 1, envVol = 1, autoGain = 0;
  int blend = 0, clipMode = 0, uni = 1, uniType = 0, phType = 0, phScale = 0, fType[2] = {4, 17}, rType[2] = {0, 0}, imgLoop = 1;
  bool velAtt = false, velAttLen = false, velRel = false, prismMul = false, uniAlt = false, portaLog = true;
  float trem = 0, tremHz = 4, tremGap = 0.5f, blurMix = 1, tAtt = 0, tDec = 0, hBlur = 1, prism = 0;
  float hzMix = 0, hzWidth = 0.5f, hzStrength = 0, hzShift = -1;
  float uniPan = 1, uniPitch = 11, uniPhase = 0.5f, freqRatio = 1, detRatio = 1, pitchArt = 1200, vib = 0, vibHz = 5.66f, porta = 0.08f,
        portaLimit = 1200;
  float cut[2] = {1, 1}, width[2] = {0.5f, 0.5f}, key[2] = {0, 0}, env[2] = {1, 1}, res[2] = {0, 0}, resWidth[2] = {0.5f, 0.5f},
        resOfs[2] = {0, 0}, osc[2] = {0, 0};
  float fMix = 1, pluck = 1, phMix = 0, phWidth = 0.5f, phOfs = 0.5f, phMotion = 3, phKey = 0;
  float imgMix = 1, imgScale = 0.5f, imgTime = 0, imgSpeed = 1, predelay = 0, relMul = 1;
  void read(const PluginState& s, int p) {
    auto P = [&](int f, float d) { return prmf(s, pf(p, f), d); };
    auto I = [&](int f, int d) { return prmi(s, pf(p, f), d); };
    on = I(PfEnabled, p == 0 ? 1 : 0) != 0;
    phase = P(PfPhase, 0.5f), phaseRand = P(PfPhaseRand, 0), timbreMix = P(PfTimbreMix, 0), blend = I(PfBlend, 0);
    sub[0] = P(PfSub1, 0), sub[1] = P(PfSub3, 0), sub[2] = P(PfSub4, 0);
    protect = P(PfProtect, 0), clip = P(PfClip, 1), clipMode = I(PfClipMode, 0), fxMix = P(PfFxMix, 0);
    vol = P(PfVolume, 1), envVol = P(PfEnvVolume, 1), autoGain = P(PfAutoGain, 0);
    velAtt = I(PfVelAttTime, 0) != 0, velAttLen = I(PfVelAttLen, 0) != 0, velRel = I(PfVelRelLen, 0) != 0;
    trem = P(PfTremolo, 0), tremHz = P(PfTremHz, 4), tremGap = P(PfTremGap, 0.5f);
    blurMix = P(PfBlurMix, 1), tAtt = P(PfTBlurAtt, 0), tDec = P(PfTBlurDec, 0), hBlur = P(PfHBlur, 1);
    prism = P(PfPrism, 0), prismMul = I(PfPrismMul, 0) != 0;
    hzMix = P(PfHzMix, 0), hzWidth = P(PfHzWidth, 0.5f), hzStrength = P(PfHzStrength, 0), hzShift = P(PfHzShift, -1);
    uni = std::clamp(I(PfUniOrder, 1), 1, kMaxUni), uniType = I(PfUniType, 0), uniAlt = I(PfUniAlt, 0) != 0;
    uniPan = P(PfUniPan, 1), uniPitch = P(PfUniPitch, 11), uniPhase = P(PfUniPhase, 0.5f);
    freqRatio = P(PfFreqMul, 1) / std::max(1.0f, P(PfFreqDiv, 1));
    detRatio = P(PfDetMul, 1) / std::max(1.0f, P(PfDetDiv, 1));
    pitchArt = P(PfPitchArt, 1200), vib = P(PfVibrato, 0), vibHz = P(PfVibHz, 5.66f);
    porta = P(PfPortaTime, 0.08f), portaLimit = P(PfPortaLimit, 1200), portaLog = I(PfPortaLog, 1) != 0;
    for (int k = 0; k < 2; ++k) {
      auto F = [&](int f, float d) { return prmf(s, flt(p, k, f), d); };
      fType[k] = std::clamp(static_cast<int>(std::lround(F(PfF1Type, k == 0 ? 4.f : 17.f))), 0, kFltShapes - 1);
      rType[k] = std::clamp(static_cast<int>(std::lround(F(PfF1ResType, 0))), 0, kResShapes - 1);
      cut[k] = F(PfF1Cut, 1), width[k] = F(PfF1Width, 0.5f), key[k] = F(PfF1Key, 0), env[k] = F(PfF1Env, 1);
      res[k] = F(PfF1Res, 0), resWidth[k] = F(PfF1ResWidth, 0.5f), resOfs[k] = F(PfF1ResOfs, 0), osc[k] = F(PfF1Osc, 0);
    }
    fMix = P(PfFMix, 1), pluck = P(PfPluck, 1);
    phType = I(PfPhType, 0), phMix = P(PfPhMix, 0), phWidth = P(PfPhWidth, 0.5f), phOfs = P(PfPhOfs, 0.5f);
    phMotion = P(PfPhMotion, 3), phKey = P(PfPhKey, 0), phScale = I(PfPhScale, 0);
    imgMix = P(PfImgGainMix, 1), imgScale = P(PfImgGainScale, 0.5f), imgTime = P(PfImgTime, 0), imgLoop = I(PfImgLoop, 1);
    imgSpeed = P(PfImgSpeed, 1) * std::pow(2.0f, P(PfImgSpeedOct, 0));
    predelay = P(PfPredelay, 0), relMul = P(PfRelMul, 1);
  }
};

// Filter shapes 0..16 follow Additive's harmonic-domain set (index + 1); 17 = off.
int fltShapeOf(int t) { return t >= 17 ? AfOff : t + 1; }
int resShapeOf(int t) { return t == 8 ? ArBump : t == 9 ? ArNoise : std::clamp(t, 0, ArCount - 1); }

// ------------------------------------------------------------ generator
class HarmorGen final : public Generator {
 public:
  HarmorGen() : voices_(new Voice[kVoices]) {
    (void)sineTable();
    last_.reserve(HmParamCount + 8);
  }
  bool active() const override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on) return true;
    return fx_.ringing() || distTail_ > 0;
  }

 protected:
  struct Osc {
    uint32_t ph = 0, inc = 0;
    float amp = 0, damp = 0, smooth = 0;
  };
  struct ArtState {
    double x = 0;        // envelope position
    double lfoPh = 0;    // LFO phase (cycles)
    float relFrom = -1;  // level at release when released before the sustain point
    float out = 0;       // smoothed output
    float mapBase = 0;   // combined note-time maps (unipolar product / bipolar sum of offsets)
  };
  struct PartVoice {
    Osc osc[kHarm];
    Osc sub[4];  // 3 half-step partials + self-oscillation
    ArtState art[kArts];
    double imgPos = 0;
    float gate = 1;  // fallback amp when the volume envelope is off
    bool done = false;
    int stride = kHarm, uni = 1;  // oscillators per unison copy, copies
    float panL[kMaxUni] = {}, panR[kMaxUni] = {}, trem = 0;
  };
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0, serial = 0;
    uint64_t age = 0;
    double note = 60, cur = 60, from = 60, glideT = 1, glideLen = 0;
    float vel = 1, rnd = 0;
    double t = 0;
    bool released = false;
    PartVoice part[kParts];
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo > 0 ? ctx.tempo : 120;
    prep_ = dynamic_cast<const HarmorAux*>(aux.custom.get());
    if (!prep_) prep_ = &defaultPrep();
    if (s.params != last_) {
      last_ = s.params;
      for (int p = 0; p < kParts; ++p) cfg_[p].read(s, p);
    }
    modX_ = prmf(s, GmModX, 0.5f), modY_ = prmf(s, GmModY, 0.5f), modZ_ = prmf(s, GmModZ, 0.5f);
    legato_ = prmi(s, GmLegato, 0) != 0, porta_ = prmi(s, GmPorta, 0) != 0, velPorta_ = prmi(s, GmVelPorta, 0) != 0;
    partMix_ = prmf(s, GmPartMix, 0.5f), volume_ = prmf(s, GmVolume, 1), post_ = prmf(s, GmPostVolume, 1);
    velVol_ = prmi(s, GmVelVol, 1) != 0, pitch_ = prmf(s, GmPitch, 0) / 100.0f, mono_ = prmi(s, GmMono, 0);
    maxVoices_ = std::clamp(prmi(s, GmVoices, 12), 1, kVoices);
    // Effects.
    distType_ = prmi(s, FxDistType, 0), distDrive_ = prmf(s, FxDistDrive, 0.5f), distAsym_ = prmf(s, FxDistAsym, 0);
    distOut_ = prmf(s, FxDistOut, 1), distMix_ = prmf(s, FxDistMix, 0);
    const float chorus = prmi(s, FxChOrder, 0) > 0 ? std::clamp(0.5f + 0.5f * prmf(s, FxChMix, 0.5f), 0.0f, 1.0f) : 0.0f;
    const float delay = prmi(s, FxDlOn, 0) ? std::clamp(0.5f + 0.5f * prmf(s, FxDlVol, 0.5f), 0.0f, 1.0f) : 0.0f;
    const float reverb = prmi(s, FxRvOn, 0) ? std::clamp(0.5f + 0.5f * prmf(s, FxRvMix, 0.5f), 0.0f, 1.0f) * 0.8f : 0.0f;
    fx_.setDirect(sr_, chorus, delay, prmf(s, FxDlTime, 1.5f) * 60.0 / tempo_, prmf(s, FxDlFeedback, 0.3f), reverb,
                  std::clamp(prmf(s, FxRvDecay, 3.9f) / 9.0f + 0.3f * prmf(s, FxRvRoom, 0.4f), 0.0f, 1.0f));
    fx_.setKinds(0, prmi(s, FxDlMode, 1), 0, tempo_);
    // J-92: chorus speed / depth / delay were never read (neutral at their defaults 0.75 / 0.5 / 0.0625).
    fx_.scaleChorus(static_cast<float>(std::exp2((prmf(s, FxChSpeed, 0.75f) - 0.75f) * 4.0f)), prmf(s, FxChDepth, 0.5f) / 0.5f,
                    (prmf(s, FxChDelay, 0.0625f) + 0.0625f) / 0.125f);
    compType_ = prmi(s, FxCompType, 0), comp_ = prmf(s, FxCompAmount, 0.5f);
    toneA_ = static_cast<float>(1.0 - std::exp(-kTwoPi * prmf(s, FxDistTone, 12000) / sr_));
  }

  static const HarmorAux& defaultPrep() {
    static const HarmorAux a = [] {
      HarmorAux x;
      PatchData d;
      for (int p = 0; p < kParts; ++p) prepCurveTables(d.part[static_cast<size_t>(p)], x.part[p]);
      return x;
    }();
    return a;
  }

  // ---- notes
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    const double prev = lastNote_;
    lastNote_ = pitch;
    ++held_;
    if (mono_ > 0 || legato_) {
      for (int i = 0; i < kVoices; ++i) {
        Voice& v = voices_[i];
        if (v.on && v.held) {
          if (legato_) {
            startGlide(v, v.cur, pitch, vel);
            v.note = pitch;
            v.id = id;
            return;
          }
          if (mono_ >= 2 || std::fabs(v.note - pitch) < 0.01) release(v);
        }
      }
    }
    Voice* pick = nullptr;
    for (int i = 0; i < maxVoices_; ++i)
      if (!voices_[i].on) {
        pick = &voices_[i];
        break;
      }
    if (!pick)
      for (int i = 0; i < maxVoices_; ++i)
        if (!pick || voices_[i].age < pick->age) pick = &voices_[i];
    Voice& v = *pick;
    v.on = v.held = true;
    v.released = false;
    v.id = id;
    v.serial = ++serial_;
    v.note = pitch;
    v.vel = vel;
    v.age = ++clock_;
    v.t = 0;
    v.rnd = hash(v.serial, 3);
    if (porta_ && prev >= 0) startGlide(v, prev, pitch, vel);
    else v.cur = pitch, v.glideT = 1;
    for (int p = 0; p < kParts; ++p) startPart(v, p);
    control(v, true);
  }
  static float hash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
  }
  void startGlide(Voice& v, double from, double to, float vel) {
    const PartCfg& c = cfg_[0];
    double len = c.porta * (velPorta_ ? 1.5 - vel : 1.0);
    if (std::fabs(to - from) * 100 > c.portaLimit && c.portaLimit < 1199) from = to - std::copysign(c.portaLimit / 100.0, to - from);
    v.from = from;
    v.cur = from;
    v.glideLen = len;
    v.glideT = len > 1e-4 ? 0 : 1;
  }
  void startPart(Voice& v, int p) {
    PartVoice& pv = v.part[p];
    const PartCfg& c = cfg_[p];
    const PartPrep& pp = prep_->part[p];
    pv.done = !c.on;
    pv.gate = 1;
    pv.imgPos = c.imgTime * std::max(0, pp.cols - 1);
    for (int a = 0; a < kArts; ++a) {
      const ArtPrep& ap = pp.arts[static_cast<size_t>(a)];
      ArtState& st = pv.art[a];
      st = ArtState{};
      st.lfoPh = ap.lfoPhase;
      // Note-time maps: key, velocity, random, unison (centre), held, X speed (centre).
      const double xs[kMaps] = {v.note / 127.0, v.vel, modX_, modY_, modZ_, v.rnd, 0.5, std::min(1.0, held_ / 16.0), 0.5};
      float acc = ap.bip ? 0.0f : 1.0f;
      for (int m = 0; m < kMaps; ++m) {
        if (m == MkX || m == MkY || m == MkZ) continue;
        const double y = ap.mapFlat[m] ? ap.mapConst[m] : curveAt(ap.maps[m], xs[m] * std::max(1e-9, curveLength(ap.maps[m])));
        if (ap.bip) acc += static_cast<float>(y - 0.5);
        else acc *= static_cast<float>(std::max(0.0, y));
      }
      st.mapBase = acc;
      st.out = ap.bip ? 0.5f : 1.0f;
    }
    for (int k = 0; k < kHarm; ++k) {
      Osc& o = pv.osc[k];
      o = Osc{};
      double ph = c.phase + (pp.sh[ShPhase][static_cast<size_t>(k % kHarm)] - 0.5);
      if (c.phaseRand != 0) ph += std::fabs(c.phaseRand) * hash(v.serial * 977u + static_cast<uint32_t>(p), static_cast<uint32_t>(k));
      o.ph = static_cast<uint32_t>((ph - std::floor(ph)) * 4294967296.0);
    }
    for (auto& s : pv.sub) s = Osc{};
  }
  void release(Voice& v) {
    if (!v.held) return;
    v.held = false;
    v.released = true;
    if (held_ > 0) --held_;
    for (int p = 0; p < kParts; ++p) {
      PartVoice& pv = v.part[p];
      const PartPrep& pp = prep_->part[p];
      for (int a = 0; a < kArts; ++a) {
        const ArtPrep& ap = pp.arts[static_cast<size_t>(a)];
        ArtState& st = pv.art[a];
        if (ap.envOn && ap.susX >= 0 && st.x < ap.susX) {
          st.relFrom = static_cast<float>(curveAt(ap.env, st.x));
          st.x = ap.susX;
        }
      }
    }
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on && voices_[i].held && voices_[i].id == id) release(voices_[i]);
  }
  void onAllOff() override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on) release(voices_[i]);
  }
  void onKill() override {
    for (int i = 0; i < kVoices; ++i) voices_[i].on = false;
    held_ = 0;
    fx_.reset();
    distTail_ = 0;
  }

  // ---- articulators
  // Advances and evaluates articulator a of a part (value: unipolar factor or bipolar 0..1 around 0.5).
  float art(const Voice& v, PartVoice& pv, const PartPrep& pp, int a, double dt) {
    const ArtPrep& ap = pp.arts[static_cast<size_t>(a)];
    ArtState& st = pv.art[a];
    if (!ap.any) return ap.bip ? 0.5f : 1.0f;
    const double tScale = ap.envTempo ? tempo_ / 120.0 : 1.0;  // tempo envelopes: x in beats at 120 BPM
    float e = ap.bip ? 0.5f : 1.0f;
    if (ap.envOn) {
      const bool beforeDecay = ap.susX < 0 || st.x < ap.susX;
      const double scale = v.released && !beforeDecay ? std::max(0.01f, ap.rel) : st.x < ap.susX * 0.5 ? std::max(0.01f, ap.att) : std::max(0.01f, ap.dec);
      double nx = st.x + dt * tScale / scale;
      if (!v.released && ap.susX >= 0 && nx >= ap.susX) {
        if (ap.loopX >= 0 && ap.loopX < ap.susX) nx = ap.loopX + std::fmod(nx - ap.susX, std::max(1e-6, ap.susX - ap.loopX));
        else nx = ap.susX;
      }
      st.x = std::min(nx, ap.envLen + 1.0);
      double y = curveAt(ap.env, st.x);
      if (st.relFrom >= 0 && ap.susX >= 0) {
        // Released before the sustain point: continue from the level reached.
        const double ys = curveAt(ap.env, ap.susX);
        y = ys > 1e-6 ? y * st.relFrom / ys : y;
      }
      if (!ap.bip && (a == ArVol)) y = std::clamp(y + (ap.sus * (!v.released ? 1.0 : 0.0)), 0.0, 1.0);
      e = static_cast<float>(y);
    } else if (a == ArVol) {
      // No volume envelope: a gate with a short release.
      pv.gate = v.released ? std::max(0.0f, pv.gate - static_cast<float>(dt / std::max(0.005f, 0.02f * cfg_[0].relMul))) : 1.0f;
      e = pv.gate;
    }
    float l = ap.bip ? 0.5f : 1.0f;
    if (ap.lfoOn) {
      const double hz = ap.lfoSpeed * (ap.lfoTempo ? tempo_ / 60.0 : 1.0);
      double& ph = ap.lfoGlobal ? globalLfo_[a] : st.lfoPh;
      if (!ap.lfoGlobal) ph += hz * dt;
      ph -= std::floor(ph);
      l = static_cast<float>(curveAt(ap.lfo, ph * ap.lfoLen));
    }
    float val;
    if (ap.bip) {
      val = 0.5f + (e - 0.5f) + (l - 0.5f) + st.mapBase;
      for (int m : {MkX, MkY, MkZ}) {
        const double x = m == MkX ? modX_ : m == MkY ? modY_ : modZ_;
        val += static_cast<float>((ap.mapFlat[m] ? ap.mapConst[m] : curveAt(ap.maps[m], x * std::max(1e-9, curveLength(ap.maps[m])))) - 0.5);
      }
    } else {
      val = e * l * st.mapBase;
      for (int m : {MkX, MkY, MkZ}) {
        const double x = m == MkX ? modX_ : m == MkY ? modY_ : modZ_;
        val *= static_cast<float>(std::max(0.0, ap.mapFlat[m] ? ap.mapConst[m] : curveAt(ap.maps[m], x * std::max(1e-9, curveLength(ap.maps[m])))));
      }
    }
    if (ap.smooth > 0.0005f) st.out += (val - st.out) * static_cast<float>(1.0 - std::exp(-dt / ap.smooth));
    else st.out = val;
    return st.out;
  }
  bool volumeDone(const Voice& v, const PartVoice& pv, const PartPrep& pp) const {
    const ArtPrep& ap = pp.arts[ArVol];
    if (!v.released) return false;
    if (!ap.envOn) return pv.gate <= 0.0f;
    return pv.art[ArVol].x >= ap.envLen && pv.art[ArVol].out < 1e-4f;
  }

  // ---- control
  void control(Voice& v, bool fresh) {
    const double dt = fresh ? 0.0 : kCtl / sr_;
    if (!fresh) {
      v.t += dt;
      if (v.glideT < 1) {
        v.glideT = std::min(1.0, v.glideT + dt / std::max(1e-4, v.glideLen));
        const double u = cfg_[0].portaLog ? 1.0 - std::pow(1.0 - v.glideT, 3.0) : v.glideT;
        v.cur = v.from + (v.note - v.from) * u;
      } else {
        v.cur = v.note;
      }
    }
    bool anyOn = false;
    for (int p = 0; p < kParts; ++p) {
      PartVoice& pv = v.part[p];
      if (pv.done) continue;
      partControl(v, p, dt, fresh);
      if (volumeDone(v, pv, prep_->part[p])) pv.done = true;
      anyOn = anyOn || !pv.done;
    }
    if (!anyOn) v.on = false;
  }

  void partControl(Voice& v, int p, double dt, bool fresh) {
    PartVoice& pv = v.part[p];
    const PartCfg& c = cfg_[p];
    const PartPrep& pp = prep_->part[p];
    auto A = [&](int a) { return art(v, pv, pp, a, dt); };
    const float aPan = A(ArPan), aVol = A(ArVol), aTim = A(ArTimbreMix);
    const float aF[2] = {A(ArF1Freq), A(ArF2Freq)}, aW[2] = {A(ArF1Width), A(ArF2Width)}, aR[2] = {A(ArF1Res), A(ArF2Res)};
    const float aRW[2] = {A(ArF1ResWidth), A(ArF2ResWidth)}, aRO[2] = {A(ArF1ResOfs), A(ArF2ResOfs)}, aFE[2] = {A(ArF1Env), A(ArF2Env)};
    const float aFMix = A(ArFMix), aPluck = A(ArPluck), aPhMix = A(ArPhMix), aPhW = A(ArPhWidth), aPhO = A(ArPhOfs);
    const float aHzMix = A(ArHzMix), aHzW = A(ArHzWidth), aClip = A(ArClip), aBlur = A(ArBlur), aPrism = A(ArPrism);
    const float aPitch = A(ArPitch), aVib = A(ArVibrato), aUni = A(ArUniPitch), aImgT = A(ArImgTime), aImgS = A(ArImgSpeed);
    (void)A(ArFxMix);
    // Pitch.
    const double vibC = c.vib * aVib * std::sin(kTwoPi * c.vibHz * v.t);
    const double cents = (aPitch - 0.5) * 2.0 * c.pitchArt + vibC + pitch_ * 100.0;
    const double f0 = noteToHz(v.cur + cents / 100.0) * c.freqRatio;
    const double nyq = sr_ * 0.47;
    // Volume.
    const float velG = velVol_ ? (0.25f + 0.75f * v.vel) : 1.0f;
    const float partG = p == 0 ? std::min(1.0f, 2.0f * (1.0f - partMix_)) : std::min(1.0f, 2.0f * partMix_);
    const float gain = aVol * c.vol * c.envVol * velG * partG * 0.18f;
    // Image position.
    double imgCol = -1;
    if (pp.cols > 0) {
      pv.imgPos += dt / std::max(0.0005f, pp.colSec) * c.imgSpeed * aImgS;
      double pos = pv.imgPos + (aImgT - 0.5) * 2.0 * pp.cols;
      if (pp.cols > 1) {
        if (c.imgLoop == 1) pos = std::fmod(std::fmod(pos, pp.cols) + pp.cols, pp.cols);
        else if (c.imgLoop == 2) {
          const double per = 2.0 * (pp.cols - 1);
          pos = std::fmod(std::fmod(pos, per) + per, per);
          if (pos > pp.cols - 1) pos = per - pos;
        } else pos = std::clamp(pos, 0.0, pp.cols - 1.0);
      } else {
        pos = 0;
      }
      imgCol = pos;
    }
    // Unison.
    const int uni = c.uni;
    const int stride = kHarm / uni;
    const int K = std::min(stride, std::max(1, static_cast<int>(nyq / std::max(1.0, f0))));
    if (pv.uni != uni) {
      for (auto& o : pv.osc) o.amp = o.damp = o.smooth = 0;
      pv.uni = uni;
    }
    pv.stride = stride;
    // Levels (shared by the unison copies).
    float lvl[kHarm], pre[kHarm], tmp[kHarm];
    double ratio[kHarm];
    const float mixT = std::clamp(c.timbreMix * (artOk(pp, ArTimbreMix) ? aTim : 1.0f), 0.0f, 1.0f);
    for (int k = 1; k <= K; ++k) {
      const size_t i = static_cast<size_t>(k - 1);
      const float saw = 1.0f / static_cast<float>(k);
      const float t1 = saw * sq(pp.sh[ShTimbre1][i] / 0.25f), t2 = saw * sq(pp.sh[ShTimbre2][i] / 0.25f);
      float g;
      switch (c.blend) {
        case 1: g = std::max(0.0f, t1 - t2 * mixT); break;
        case 2: g = t1 * (1 - mixT) + t1 * t2 * k * mixT; break;
        case 3: g = std::max(t1, t2 * mixT); break;
        case 4: g = t1 * (1 - mixT) + std::min(t1, t2) * mixT; break;
        case 5: g = t2 + (t1 - t2) * static_cast<float>(std::exp(-v.t * 6.0 * mixT)); break;
        default: g = t1 + (t2 - t1) * mixT; break;
      }
      if (imgCol >= 0 && static_cast<int>(i) < pp.rows) {
        const int c0 = static_cast<int>(imgCol), c1 = std::min(pp.cols - 1, c0 + 1);
        const float fr = static_cast<float>(imgCol - c0);
        const float ig = pp.gain[static_cast<size_t>(c0 * pp.rows) + i] * (1 - fr) + pp.gain[static_cast<size_t>(c1 * pp.rows) + i] * fr;
        g = g * (1 - c.imgMix) + ig * c.imgMix * (0.5f + c.imgScale);
      } else if (imgCol >= 0) {
        g *= 1 - c.imgMix;
      }
      // Randomness shape: per-voice random level offsets.
      const float rnd = (pp.sh[ShRandom][i] - 0.5f) * 2.0f;
      if (rnd != 0) g *= std::max(0.0f, 1.0f + rnd * (hash(v.serial, static_cast<uint32_t>(k)) * 2 - 1));
      // Partial frequency: detune ratio, prism, image freq plane.
      double r = k * std::pow(static_cast<double>(c.detRatio), std::log2(static_cast<double>(k)));
      const double prismAmt = c.prism * aPrism;
      if (prismAmt != 0) {
        const double s = (pp.sh[ShPrism][i] - 0.5) * 2.0;
        r = c.prismMul ? r * std::pow(2.0, prismAmt * s * 0.5) : r + prismAmt * s * 0.5 * k;
      }
      if (imgCol >= 0 && !pp.freq.empty() && static_cast<int>(i) < pp.rows) {
        const int c0 = static_cast<int>(imgCol);
        r *= std::pow(2.0, pp.freq[static_cast<size_t>(c0 * pp.rows) + i] * 0.5);
      }
      ratio[i] = std::max(0.01, r);
      // Local EQ.
      g *= sq(eqAt(pp.localEq, f0 * ratio[i]) / 0.25f);
      lvl[i] = g;
    }
    // Harmonic blur (across neighbours).
    if (c.hBlur > 1.01f && c.blurMix > 0) {
      const int rad = std::min(32, static_cast<int>(c.hBlur * aBlur));
      if (rad >= 1) {
        for (int k = 0; k < K; ++k) tmp[k] = lvl[k];
        for (int k = 0; k < K; ++k) {
          float s = 0;
          int n = 0;
          for (int j = std::max(0, k - rad); j <= std::min(K - 1, k + rad); ++j) s += tmp[j], ++n;
          const float m = pp.sh[ShMaskBlur][static_cast<size_t>(k)] * c.blurMix;
          lvl[k] = tmp[k] + (s / n - tmp[k]) * m;
        }
      }
    }
    for (int k = 0; k < K; ++k) pre[k] = lvl[k];
    // Filters.
    const double keyOct = (v.cur - 60.0) / 12.0;
    double cutHz[2];
    for (int f = 0; f < 2; ++f) {
      const double envOct = (aF[f] - 0.5) * 2.0 * c.env[f] * aFE[f] * 5.0;
      cutHz[f] = std::clamp(16.35 * std::pow(2.0, 10.3 * c.cut[f] + c.key[f] * keyOct + envOct), 10.0, 40000.0);
    }
    const float fm = std::clamp(c.fMix + (aFMix - 0.5f) * 2.0f, -1.0f, 1.0f);
    for (int k = 0; k < K; ++k) {
      const double hz = f0 * ratio[k];
      float g12[2];
      for (int f = 0; f < 2; ++f) {
        double g = 1.0;
        if (c.fType[f] != 17) {
          const double wOct = std::pow(2.0, 4.0 * c.width[f] * aW[f] - 1.0);
          const double oct = std::log2(std::max(1.0, hz) / cutHz[f]);
          if (c.fType[f] >= kFltCustom1) {
            const Curve& cv = pp.filterCurve[c.fType[f] - kFltCustom1];
            const double t = std::clamp(0.5 + oct * 1024.0 / wOct / 2047.0, 0.0, 1.0);
            g = cv.pts.empty() ? 1.0 : curveAt(cv, t * curveLength(cv));
          } else {
            g = filterShape(fltShapeOf(c.fType[f]), oct, wOct);
          }
          const double ra = l11(c.res[f] * aR[f]) * 0.25;
          if (ra > 0) {
            const double rc = cutHz[f] * std::pow(2.0, (c.resOfs[f] + (aRO[f] - 0.5) * 4800.0) / 1200.0);
            const double ro = std::log2(std::max(1.0, hz) / rc);
            const double rw = std::pow(2.0, 4.0 * c.resWidth[f] * aRW[f] - 2.0);
            double rv;
            if (c.rType[f] >= kResCustom1) {
              const Curve& cv = pp.resCurve[c.rType[f] - kResCustom1];
              const double t = 0.5 + ro * 1024.0 / rw / 1023.0;
              rv = t < 0 || t > 1 || cv.pts.empty() ? 0.0 : (curveAt(cv, t * curveLength(cv)) - 0.5) * 14.0;
            } else {
              rv = resonanceShape(resShapeOf(c.rType[f]), ro, rw, v.serial, k);
            }
            g += ra * rv * pp.sh[f == 0 ? ShMaskRes1 : ShMaskRes2][static_cast<size_t>(k)];
          }
          const float mask = pp.sh[f == 0 ? ShMaskF1 : ShMaskF2][static_cast<size_t>(k)];
          g = 1.0 + (std::max(0.0, g) - 1.0) * mask;
        }
        g12[f] = static_cast<float>(g);
      }
      const float ser = g12[0] * g12[1], par = 0.5f * (g12[0] + g12[1]);
      lvl[k] *= par + (ser - par) * (fm + 1) * 0.5f;
    }
    // Phaser.
    const float phMix = c.phMix * aPhMix;
    if (phMix > 0 && c.phType != ApFreq) {
      const double dens = 32.0 * std::pow(2.0, 8.0 * (c.phWidth * aPhW - 1.0));
      const double off = c.phOfs + (aPhO - 0.5) * 2.0 + c.phMotion * 0.01 * v.t + c.phKey * keyOct;
      for (int k = 0; k < K; ++k) {
        const double hz = f0 * ratio[k];
        double ph = c.phScale == 1 ? hz / 1000.0 * dens * 0.5 + off : c.phScale == 2 ? ratio[k] * dens * 0.25 + off
                                                                                     : std::log2(std::max(hz, 1.0) / 20.0) * dens + off;
        double val;
        if (c.phType == 10 && !pp.phaserCurve.pts.empty()) {
          ph -= std::floor(ph);
          val = curveAt(pp.phaserCurve, ph * curveLength(pp.phaserCurve)) * 2.0;
        } else {
          val = phaserShape(std::min(c.phType, ApCount - 1), ph);
        }
        const float m = phMix * pp.sh[ShMaskPhaser][static_cast<size_t>(k)];
        lvl[k] *= static_cast<float>(m * val + (1 - m));
      }
    }
    // Pluck.
    if (c.pluck < 0.999f) {
      const double d = 0.11975 * (std::pow(1001.0, std::clamp(static_cast<double>(c.pluck), 0.0, 1.0)) - 1.0);
      const float amt = std::clamp(aPluck, 0.0f, 1.0f);
      for (int k = 0; k < K; ++k) {
        const double rate = 0.6 / (d + 0.05) * std::pow(f0 * ratio[k] / 200.0, 0.8) * pp.sh[ShPluck][static_cast<size_t>(k)];
        lvl[k] *= static_cast<float>(1.0 - amt + amt * std::exp(-v.t * rate));
      }
    }
    // Harmonizer (octave copies with a shift).
    const float hzMix = c.hzMix * aHzMix;
    if (hzMix > 0) {
      for (int k = 0; k < K; ++k) tmp[k] = lvl[k];
      const double strength = 1.331 * std::pow(2.0, 4.0 * c.hzStrength);
      const double w = c.hzWidth * aHzW;
      double sum = 0;
      for (int cpy = 1; cpy < 16; ++cpy) {
        const double r = 1.0 - cpy / (5.0 * w + 1.0);
        if (r <= 0) break;
        const double g = std::min(1.0, r * r * r * strength) * hzMix * hzMix;
        sum += g;
        const int step = 1 << cpy;
        for (int src = 1; src <= K; ++src) {
          const long t = static_cast<long>(src) * step + static_cast<long>(std::lround(c.hzShift)) + 1;
          if (t < 1 || t > K) break;
          lvl[t - 1] += static_cast<float>(g * tmp[src - 1] * pp.sh[ShMaskHarm][static_cast<size_t>(t - 1)]);
        }
      }
      for (int k = 0; k < K; ++k) lvl[k] *= static_cast<float>(1.0 / (1.0 + sum));
    }
    // Clipping and protection.
    float peak = 1e-9f;
    for (int k = 0; k < K; ++k) peak = std::max(peak, lvl[k]);
    const float thr = std::clamp(c.clip * aClip, 0.0f, 1.0f) * peak;
    if (c.clip < 0.999f || aClip < 0.999f)
      for (int k = 0; k < K; ++k) {
        const float w = pp.sh[ShClip][static_cast<size_t>(k)] * 2.0f;
        float g = lvl[k];
        switch (c.clipMode) {
          case 0: g = std::min(g, thr); break;
          case 1: g = g < thr ? g * g / std::max(1e-9f, thr) : g; break;
          case 2: g = g < thr ? g * 0.1f : g; break;
          case 3: g = g < thr ? 0.0f : g; break;
          default: g = std::max(0.0f, g - thr); break;
        }
        lvl[k] += (g - lvl[k]) * std::clamp(w, 0.0f, 1.0f);
      }
    if (c.protect > 0)
      for (int k = 0; k < std::min(K, 8); ++k) lvl[k] += (pre[k] - lvl[k]) * c.protect * pp.sh[ShProtect][static_cast<size_t>(k)];
    // Global EQ and auto-gain.
    double e = 0;
    for (int k = 0; k < K; ++k) {
      lvl[k] *= sq(eqAt(pp.globalEq, f0 * ratio[k]) / 0.25f);
      e += static_cast<double>(lvl[k]) * lvl[k];
    }
    if (c.autoGain > 0 && e > 1e-12) {
      const float ag = static_cast<float>(std::pow(1.0 / std::sqrt(e) * 0.8, c.autoGain));
      for (int k = 0; k < K; ++k) lvl[k] *= ag;
    }
    // Oscillators (unison copies).
    const double uniC = c.uniPitch * aUni;
    const float tr = c.trem > 0 ? c.trem * static_cast<float>(0.5 + 0.5 * std::sin(kTwoPi * c.tremHz * v.t)) : 0.0f;
    pv.trem = tr;
    const float norm = gain / std::sqrt(static_cast<float>(uni));
    const float rise = c.tAtt > 0 ? static_cast<float>(1.0 - std::exp(-dt / c.tAtt)) : 1.0f;
    const float fall = c.tDec > 0 ? static_cast<float>(1.0 - std::exp(-dt / c.tDec)) : 1.0f;
    for (int u = 0; u < uni; ++u) {
      double x = uni > 1 ? 2.0 * u / (uni - 1) - 1.0 : 0.0;
      if (c.uniAlt && u % 2) x = -x;
      if (c.uniType == 1 && u > 0) x *= 0.5 + hash(v.serial + 5u, static_cast<uint32_t>(u));
      if (c.uniType == 3 && u > 0) x = hash(v.serial + 7u, static_cast<uint32_t>(u)) * 2 - 1;
      const double detMul = std::pow(2.0, x * uniC / 1200.0);
      const double hzAdd = c.uniType == 4 ? x * uniC * 0.1 : 0.0;  // "Hz" spreads by a fixed amount
      double pan = std::clamp(x * std::min(1.0f, c.uniPan) + (aPan - 0.5) * 2.0, -1.0, 1.0), pl, pr;
      panGains(pan, pl, pr);
      pv.panL[u] = static_cast<float>(pl);
      pv.panR[u] = static_cast<float>(pr);
      for (int k = 0; k < stride; ++k) {
        Osc& o = pv.osc[u * stride + k];
        const double hz = k < K ? f0 * ratio[k] * detMul + hzAdd : 0.0;
        float target = 0;
        if (k < K && hz > 0 && hz < nyq) {
          target = lvl[k] * norm;
          o.inc = phaseInc(hz, sr_);
        }
        if (!fresh && (rise < 1 || fall < 1)) o.smooth += (target - o.smooth) * (target > o.smooth ? rise : fall);
        else o.smooth = target;
        o.damp = (o.smooth - o.amp) / kCtl;
      }
    }
    // Half-step partials and self-oscillation.
    static const double kSubR[3] = {0.5, 1.5, 2.5};
    for (int i = 0; i < 3; ++i) {
      Osc& s = pv.sub[i];
      s.inc = phaseInc(f0 * kSubR[i], sr_);
      const float t = c.sub[i] * (K > 0 ? pre[0] : 0.0f) * norm;
      s.damp = (t - s.amp) / kCtl;
    }
    {
      Osc& s = pv.sub[3];
      const double rc = cutHz[0] * std::pow(2.0, c.resOfs[0] / 1200.0);
      s.inc = phaseInc(std::clamp(rc, 20.0, nyq), sr_);
      const float t = c.osc[0] > 0 && c.fType[0] != 17 ? c.osc[0] * 2.0f * norm : 0.0f;
      s.damp = (t - s.amp) / kCtl;
    }
  }
  static float sq(float x) { return x * x; }
  static bool artOk(const PartPrep& pp, int a) { return pp.arts[static_cast<size_t>(a)].any; }
  static float eqAt(const std::array<float, kEqPts>& t, double hz) {
    const double x = std::clamp(std::log(std::max(hz, 20.0) / 20.0) / std::log(1000.0), 0.0, 1.0) * (kEqPts - 1);
    const int i = std::min(kEqPts - 2, static_cast<int>(x));
    const float f = static_cast<float>(x - i);
    return t[static_cast<size_t>(i)] + (t[static_cast<size_t>(i + 1)] - t[static_cast<size_t>(i)]) * f;
  }

  void renderVoices(float* bl, float* br, int n) {
    const SineTable& st = sineTable();
    for (int i = 0; i < kVoices; ++i) {
      Voice& v = voices_[i];
      if (!v.on) continue;
      for (int p = 0; p < kParts; ++p) {
        PartVoice& pv = v.part[p];
        if (pv.done) continue;
        const int K = pv.stride, uni = pv.uni;
        for (int u = 0; u < uni; ++u) {
          float mono[kCtl] = {};
          for (int k = 0; k < K; ++k) {
            Osc& o = pv.osc[u * K + k];
            if (o.amp == 0 && o.damp == 0) {
              o.ph += o.inc * static_cast<uint32_t>(n);
              continue;
            }
            uint32_t ph = o.ph;
            float a = o.amp;
            for (int j = 0; j < n; ++j) {
              mono[j] += st.at(ph) * a;
              ph += o.inc;
              a += o.damp;
            }
            o.ph = ph;
            o.amp = std::fabs(a) < 1e-9f ? 0.0f : a;
          }
          if (u == 0)
            for (Osc& s : pv.sub) {
              if (s.amp == 0 && s.damp == 0) continue;
              for (int j = 0; j < n; ++j) {
                mono[j] += st.at(s.ph) * s.amp;
                s.ph += s.inc;
                s.amp += s.damp;
              }
              if (std::fabs(s.amp) < 1e-9f) s.amp = 0;
            }
          const float tr = pv.trem, gl = pv.panL[u] * (1 - tr * std::min(1.0f, cfg_[p].tremGap * 2)), gr = pv.panR[u] * (1 - tr);
          for (int j = 0; j < n; ++j) {
            bl[j] += mono[j] * gl;
            br[j] += mono[j] * gr;
          }
        }
      }
    }
  }

  void distort(float* L, float* R, int n) {
    if (distType_ <= 0) return;
    const float drive = 1.0f + distDrive_ * distDrive_ * 30.0f, wet = std::clamp(0.5f + 0.5f * distMix_, 0.0f, 1.0f) * 2.0f;
    bool loud = false;
    for (int i = 0; i < n; ++i) {
      float io[2] = {L[i], R[i]};
      for (int c = 0; c < 2; ++c) {
        const float x = io[c] * drive + distAsym_;
        float y;
        switch (distType_) {
          case 2: y = x / (1 + std::fabs(x)); break;
          case 3: y = std::clamp(x, -1.0f, 1.0f); break;
          case 4: y = std::clamp(x - x * x * x / 3, -0.67f, 0.67f) * 1.5f; break;
          case 5: y = std::copysign(std::log1p(std::fabs(x)), x) * 0.6f; break;
          case 6: y = std::tanh(x * 1.5f); break;
          case 7: y = std::sin(std::clamp(x, -1.57f, 1.57f)); break;
          case 8: y = x / std::sqrt(1 + x * x); break;
          case 9: y = std::sin(x); break;
          case 10: y = std::round(x * 8) / 8; break;
          case 11: y = std::fmod(x * 3, 2.0f) - (x > 0 ? 1.0f : -1.0f); break;
          default: y = std::tanh(x); break;
        }
        y -= std::tanh(distAsym_) * (distType_ == 1 ? 1.0f : 0.0f);
        tone_[c] += (y - tone_[c]) * toneA_;
        const float o = io[c] + (tone_[c] * distOut_ * 0.5f - io[c]) * std::min(1.0f, wet);
        io[c] = o;
        if (std::fabs(o) > 1e-6f) loud = true;
      }
      L[i] = io[0], R[i] = io[1];
    }
    distTail_ = loud ? static_cast<int>(sr_ * 0.1) : std::max(0, distTail_ - n);
  }
  void compress(float* L, float* R, int n) {
    if (compType_ <= 0) return;
    const float drive = 0.5f + comp_ * 3.0f;
    for (int i = 0; i < n; ++i) {
      float* io[2] = {&L[i], &R[i]};
      for (float* x : io) {
        switch (compType_) {
          case 1: *x = std::clamp(*x * drive, -0.95f, 0.95f); break;
          case 2: *x = std::tanh(*x * drive) / std::tanh(drive); break;
          case 3: *x = std::tanh(*x * drive * 1.8f) * 0.8f; break;
          default: *x = std::tanh(*x * drive * 3.0f) * 0.7f; break;
        }
      }
    }
  }

  void render(float* L, float* R, int n) override {
    int pos = 0;
    while (pos < n) {
      if (ctr_ == 0) {
        for (int a = 0; a < kArts; ++a) {
          const ArtPrep& ap = prep_->part[0].arts[static_cast<size_t>(a)];
          if (ap.lfoOn && ap.lfoGlobal) globalLfo_[a] += ap.lfoSpeed * (ap.lfoTempo ? tempo_ / 60.0 : 1.0) * kCtl / sr_;
        }
        for (int i = 0; i < kVoices; ++i)
          if (voices_[i].on) control(voices_[i], false);
        ctr_ = kCtl;
      }
      const int len = std::min(ctr_, n - pos);
      float bl[kCtl] = {}, br[kCtl] = {};
      renderVoices(bl, br, len);
      for (int j = 0; j < len; ++j) bl[j] *= volume_, br[j] *= volume_;
      distort(bl, br, len);
      fx_.process(bl, br, len);
      compress(bl, br, len);
      for (int j = 0; j < len; ++j) {
        L[pos + j] += bl[j] * post_;
        R[pos + j] += br[j] * post_;
      }
      ctr_ -= len;
      pos += len;
    }
  }

 private:
  std::unique_ptr<Voice[]> voices_;
  const HarmorAux* prep_ = nullptr;
  std::vector<float> last_;
  PartCfg cfg_[kParts];
  double sr_ = 44100, tempo_ = 120, lastNote_ = -1, globalLfo_[kArts] = {};
  float modX_ = 0.5f, modY_ = 0.5f, modZ_ = 0.5f, partMix_ = 0.5f, volume_ = 1, post_ = 1, pitch_ = 0;
  bool legato_ = false, porta_ = false, velPorta_ = false, velVol_ = true;
  int mono_ = 0, maxVoices_ = kVoices, ctr_ = 0, held_ = 0;
  int distType_ = 0, compType_ = 0, distTail_ = 0;
  float distDrive_ = 0.5f, distAsym_ = 0, distOut_ = 1, distMix_ = 0, comp_ = 0.5f, toneA_ = 1, tone_[2] = {0, 0};
  uint64_t clock_ = 0;
  uint32_t serial_ = 0;
  ModelFx fx_;
};

}  // namespace

std::unique_ptr<Generator> makeHarmor() { return std::make_unique<HarmorGen>(); }

std::shared_ptr<const AuxData> buildHarmorAux(const PluginState& s) {
  PatchData d;
  if (!s.data.empty()) parsePatch(s.data, d);
  auto a = std::make_shared<HarmorAux>();
  for (int p = 0; p < kParts; ++p) {
    prepCurveTables(d.part[static_cast<size_t>(p)], a->part[p]);
    prepImage(d.part[static_cast<size_t>(p)].image, a->part[p]);
  }
  return a;
}

}  // namespace gl::packs::models

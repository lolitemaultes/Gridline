// Gridline: the Multisampler's SFZ player (K-144). The UI thread reads the map (core/SfzMap) and
// loads its samples into SfzData; the audio thread picks regions and plays voices from fixed
// arrays (no allocation, no locks). Included by samplers_rt.h inside its anonymous namespace.
#pragma once
#include <atomic>
#include <filesystem>
#include "sfz_fx.h"
#include "../../engine/RegionActivity.h"

namespace sfzp {

using namespace gl::sfz;
// the sampler pack has its own names for some of these
using LoopMode = gl::sfz::LoopMode;
using LoopType = gl::sfz::LoopType;
using Region = gl::sfz::Region;
using Filter = gl::sfz::Filter;
using Trigger = gl::sfz::Trigger;
using OffMode = gl::sfz::OffMode;
using Builtin = gl::sfz::Builtin;
using Lfo = gl::sfz::Lfo;
using Eq = gl::sfz::Eq;
using Target = gl::sfz::Target;
namespace fx = gl::packs::samplers::sfzfx;

constexpr int kVoices = 96;
constexpr int kMods = 32;  // controller modulations followed per voice
constexpr int kLfos = 8, kEgs = 8;
constexpr int kCtl = 16;   // control period (frames)
constexpr int kEgPts = 16;  // flexible envelope points whose time / level controllers are followed

// ------------------------------------------------------------------ prepared data (UI thread)
struct RegionRt {
  const Region* r = nullptr;
  SamplePtr smp;
  int64_t n = 0, start = 0, end = 0, ls = 0, le = 0;
  bool hasLoop = false;
  float baseDb = 0, baseAmp = 1;
  int index = 0;
};
struct FxSlot {
  fx::Def def;
  std::vector<std::unique_ptr<fx::State>> pool;  // one per playing instance
};
struct SfzData {
  Instrument ins;
  std::vector<RegionRt> regions;
  std::array<std::vector<int>, 128> attack, release;  // regions by key (attack-type and release-type)
  std::vector<int> ccTriggered;                       // regions with on_locc / on_hicc
  std::vector<FxSlot> fx;
  mutable std::array<std::atomic<bool>, 4> fxTaken{};  // pool slots in use
  std::string reportLine;
  int interp = 1;  // sample_quality
  bool anySend = false;
};

inline float dbToGain(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// Builds the player data for an .sfz (UI thread). Samples come from the sample cache.
inline std::shared_ptr<SfzData> buildSfz(const std::string& path) {
  auto d = std::make_shared<SfzData>();
  load(path, d->ins);
  d->reportLine = reportLine(d->ins.report);
  const int q = d->ins.sampleQuality;
  d->interp = q < 0 ? InterpSinc : q <= 2 ? InterpLinear : q <= 5 ? InterpCubic : InterpSinc;
  d->regions.reserve(d->ins.regions.size());
  for (size_t i = 0; i < d->ins.regions.size(); ++i) {
    const Region& r = d->ins.regions[i];
    RegionRt rt;
    rt.r = &r;
    rt.index = static_cast<int>(i);
    if (r.builtin == Builtin::None) {
      rt.smp = gl::sampleCache().get(r.file);
      if (!rt.smp || rt.smp->frames() < 2) continue;
      rt.n = static_cast<int64_t>(rt.smp->frames());
      int64_t off = r.offsetPercent ? static_cast<int64_t>(static_cast<double>(rt.n) * static_cast<double>(r.offset) / 100.0) : r.offset;
      rt.start = std::clamp<int64_t>(off, 0, rt.n - 2);
      rt.end = r.end > 0 ? std::clamp<int64_t>(r.end + 1, rt.start + 2, rt.n) : rt.n;
      rt.ls = r.loopStart >= 0 ? std::clamp<int64_t>(r.loopStart, 0, rt.end - 2) : 0;
      rt.le = r.loopEnd > 0 ? std::clamp<int64_t>(r.loopEnd + 1, rt.ls + 2, rt.end) : rt.end;
      rt.hasLoop = (r.loopMode == LoopMode::Continuous || r.loopMode == LoopMode::Sustain) && rt.le - rt.ls >= 2;
      if (!r.md5.empty() && r.md5 != "null" && r.md5.size() != 32)
        d->ins.report.warnings.push_back("md5 of " + r.sample + " is not a 32 digit sum");
    } else {
      rt.hasLoop = false;
    }
    rt.baseDb = r.volume + r.globalVolume + r.masterVolume + r.groupVolume;
    rt.baseAmp = r.globalAmplitude / 100.f * r.masterAmplitude / 100.f * r.groupAmplitude / 100.f;  // the region's own amplitude adds its controllers
    d->regions.push_back(std::move(rt));
  }
  for (size_t k = 0; k < d->regions.size(); ++k) {
    const Region& r = *d->regions[k].r;
    if (!r.onCc.empty()) d->ccTriggered.push_back(static_cast<int>(k));
    const bool rel = r.trigger == Trigger::Release || r.trigger == Trigger::ReleaseKey;
    for (int key = r.onCc.empty() ? std::max(0, r.lokey) : 128; key <= std::min(127, r.hikey); ++key) (rel ? d->release : d->attack)[static_cast<size_t>(key)].push_back(static_cast<int>(k));  // started by its controller only
    for (float s : r.effectSend)
      if (s > 0) d->anySend = true;
  }
  for (const auto& e : d->ins.effects) {
    FxSlot s;
    s.def = fx::makeDef(e);
    if (s.def.type == fx::TTdfir) {
      auto it = e.params.find("tdfir_impulse");
      if (it != e.params.end()) {
        std::filesystem::path p = std::filesystem::u8path(it->second);
        if (!p.is_absolute()) p = std::filesystem::u8path(path).parent_path() / p;
        if (SamplePtr ir = gl::sampleCache().get(p.u8string())) {
          const size_t n = std::min<size_t>(ir->frames(), 2048);
          s.def.ir.assign(ir->left.begin(), ir->left.begin() + static_cast<long>(n));
        } else {
          d->ins.report.missingSamples.push_back(p.u8string());
        }
      }
    }
    if (s.def.type == fx::TNone) {
      if (!e.type.empty()) ++d->ins.report.reported["effect type " + e.type];
      d->reportLine = reportLine(d->ins.report);
      continue;
    }
    for (int k = 0; k < 4; ++k) {
      auto st = std::make_unique<fx::State>();
      st->prepare(s.def);
      s.pool.push_back(std::move(st));
    }
    d->fx.push_back(std::move(s));
  }
  if (d->regions.size() < d->ins.regions.size() && d->ins.report.missingSamples.empty())
    d->ins.report.warnings.push_back(std::to_string(d->ins.regions.size() - d->regions.size()) + " regions have samples that could not be read");
  return d;
}

// ------------------------------------------------------------------ voice pieces
// Classic envelope (delay, start, attack, hold, decay, sustain, release), shapes as in ARIA.
struct ClassicState {
  enum St { Off, Delay, Attack, Hold, Decay, Sustain, Release, Done } st = Off;
  double lvl = 0, t = 0, from = 0;
  double delay = 0, start = 0, attack = 0, hold = 0, decay = 0, sustain = 1, release = 0;
  float aShape = 0, dShape = 0, rShape = 0;
  bool decayZero = true, releaseZero = false;
  static double shaped(double x, float shape, bool expo) {
    if (shape == 0) return expo ? (1.0 - std::exp(-6.9 * x)) / (1.0 - std::exp(-6.9)) : x;
    const double s = shape;
    return (std::exp(s * x) - 1.0) / (std::exp(s) - 1.0);
  }
  void begin() {
    st = delay > 0 ? Delay : Attack;
    t = 0;
    lvl = start;
    from = start;
  }
  void off() {
    if (st == Off || st == Done || st == Release) return;
    st = Release;
    t = 0;
    from = lvl;
  }
  // Advances dt seconds; returns the level.
  double next(double dt) {
    switch (st) {
      case Off: return 0;
      case Done: return 0;
      case Delay:
        if ((t += dt) >= delay) st = Attack, t = 0;
        return lvl = start;
      case Attack:
        if (attack <= 0) {
          st = hold > 0 ? Hold : Decay, t = 0, lvl = 1;
          return lvl;
        }
        t += dt;
        lvl = start + (1.0 - start) * shaped(std::min(1.0, t / attack), aShape, false);
        if (t >= attack) st = hold > 0 ? Hold : Decay, t = 0, lvl = 1;
        return lvl;
      case Hold:
        if ((t += dt) >= hold) st = Decay, t = 0;
        return lvl = 1;
      case Decay: {
        if (decay <= 0) return st = Sustain, lvl = sustain;
        t += dt;
        // decay_zero: the time to fall from full to zero (the sustain level stops it earlier)
        const double span = decayZero ? decay * (1.0 - sustain) : decay;
        const double x = std::min(1.0, t / std::max(1e-6, decayZero ? decay : span));
        const double y = 1.0 - shaped(x, dShape, true);
        lvl = decayZero ? std::max(sustain, y) : sustain + (1.0 - sustain) * y;
        if (lvl <= sustain + 1e-6 || x >= 1.0) st = Sustain, lvl = sustain;
        return lvl;
      }
      case Sustain: return lvl = sustain;
      case Release: {
        if (release <= 0) return st = Done, lvl = 0;
        t += dt;
        const double x = std::min(1.0, t / release);
        const double y = 1.0 - shaped(x, rShape, true);
        lvl = releaseZero ? std::min(from, y) : from * y;
        if (x >= 1.0 || lvl < 1e-5) st = Done, lvl = 0;
        return lvl;
      }
    }
    return lvl;
  }
};

// Flexible envelope (egN_).
struct FlexState {
  int seg = 0, loops = 0;
  double t = 0, lvl = 0, from = 0;
  bool released = false, done = false, used = false;
  void begin(const FlexEg& e) {
    used = true;
    seg = 0, t = 0, loops = 0, released = false, done = false;
    lvl = from = e.points.empty() ? 0.0 : e.points[0].level;
  }
  // mod: per point, time (2 i) and level (2 i + 1) added by controllers (egN_timeX_onccY, levelX).
  double next(const FlexEg& e, const Instrument& ins, double dt, const float* mod) {
    if (!used || e.points.size() < 2) return lvl;
    if (done) return lvl;
    const int last = static_cast<int>(e.points.size()) - 1;
    if (seg >= last) {
      done = true;
      return lvl;
    }
    const auto& to = e.points[static_cast<size_t>(seg + 1)];
    if (!released && e.sustain > 0 && seg >= e.sustain) return lvl;  // holds at the sustain point
    const int pi = seg + 1;
    const double toTime = std::max(0.0, static_cast<double>(to.time) + (pi < kEgPts ? mod[2 * pi] : 0.f));
    const double toLevel = static_cast<double>(to.level) + (pi < kEgPts ? mod[2 * pi + 1] : 0.f);
    t += dt;
    const double x = toTime > 0 ? std::min(1.0, t / toTime) : 1.0;
    double y = x;
    if (to.shape <= -1000.f) y = curveAt(ins, static_cast<int>(-(to.shape + 1000.f)), static_cast<float>(x));
    else if (to.shape != 0) {
      const double sh = std::clamp(static_cast<double>(to.shape), -30.0, 30.0);
      y = (std::exp(sh * x) - 1.0) / (std::exp(sh) - 1.0);
    }
    lvl = from + (toLevel - from) * y;
    if (x >= 1.0) {
      ++seg;
      t = 0;
      from = toLevel;
      lvl = toLevel;
      if (seg == last && e.loop >= 0 && e.loop < last && !released && (e.loopCount <= 0 || loops < e.loopCount)) {
        ++loops;
        seg = e.loop;
        from = lvl;
      }
      if (seg == e.sustain && !released) return lvl;
    }
    return lvl;
  }
  void release(const FlexEg& e) {
    if (!used) return;
    released = true;
    if (seg < e.sustain) seg = std::max(seg, e.sustain), t = 0, from = lvl;
  }
};

struct LfoState {
  double ph = 0, t = 0, out = 0, sh = 0, sm = 0;
  int cycles = 0;
  std::array<double, 8> sub{};
  bool used = false;
};

inline double waveAt(int w, double ph, double& sh, uint32_t& rng, bool wrapped) {
  ph -= std::floor(ph);
  switch (w) {
    case 0: return 1.0 - 4.0 * std::fabs(ph - 0.5);                                  // triangle
    case 1: return std::sin(gl::dsp::kTwoPi * ph);                                   // sine
    case 2: return ph < 0.75 ? 1.0 : -1.0;                                           // pulse 75 %
    case 3: return ph < 0.5 ? 1.0 : -1.0;                                            // square
    case 4: return ph < 0.25 ? 1.0 : -1.0;                                           // pulse 25 %
    case 5: return ph < 0.125 ? 1.0 : -1.0;                                          // pulse 12.5 %
    case 6: return 2.0 * ph - 1.0;                                                   // ramp up
    case 7: return 1.0 - 2.0 * ph;                                                   // ramp down
    case 12:
      if (wrapped) sh = static_cast<double>(fx::rnd(rng) & 0xFFFF) / 32767.5 - 1.0;  // sample and hold
      return sh;
    default: return std::sin(gl::dsp::kTwoPi * ph);
  }
}

// One filter (up to three cascaded stages), stereo.
struct FilterState {
  gl::dsp::Svf s[2][3];
  gl::dsp::Biquad b[2];
  gl::dsp::OnePole p[2];
  gl::dsp::Allpass1 a[2];
  float comb[2][512] = {};
  int combW = 0;
  double combD = 1, combFb = 0;
  int type = -1;
  void set(int t, double sr, double hz, double resDb, double gainDb) {
    if (!std::isfinite(hz)) hz = sr * 0.45;
    resDb = std::isfinite(resDb) ? std::clamp(resDb, -48.0, 48.0) : 0.0;
    gainDb = std::isfinite(gainDb) ? std::clamp(gainDb, -48.0, 48.0) : 0.0;
    type = t;
    hz = std::clamp(hz, 10.0, sr * 0.45);
    const double q = std::pow(10.0, resDb / 20.0) * 0.7071;
    const double r01 = std::clamp((2.0 - 1.0 / std::max(0.5, q)) / 1.98, 0.0, 0.995);
    switch (t) {
      case 0: case 1: case 6: case 7:
        for (auto& x : p) x.setFreq(sr, hz);
        for (auto& c : s)
          for (auto& x : c) x.set(sr, hz, 0.0);
        break;
      case 8:
        for (auto& x : a) x.setFreq(sr, hz);
        break;
      case 17: case 20: case 23:
        for (auto& x : b) x.set(gl::dsp::Biquad::Peak, sr, hz, std::max(0.3, q), gainDb);
        break;
      case 18:
        for (auto& x : b) x.set(gl::dsp::Biquad::LowShelf, sr, hz, 0.7071, gainDb);
        break;
      case 19:
        for (auto& x : b) x.set(gl::dsp::Biquad::HighShelf, sr, hz, 0.7071, gainDb);
        break;
      case 21:
        combD = std::clamp(sr / hz, 1.0, 510.0);
        combFb = std::clamp(r01, 0.0, 0.98);
        break;
      default:
        for (auto& c : s)
          for (auto& x : c) x.set(sr, hz, r01);
        break;
    }
  }
  float process(int c, float in) {
    double x = in, bp, hp;
    switch (type) {
      case -1: return in;
      case 0: return static_cast<float>(p[c].process(x));                           // lpf_1p
      case 1: return static_cast<float>(x - p[c].process(x));                       // hpf_1p
      case 6: {                                                                      // bpf_1p
        const double lo = s[c][0].process(x, &bp, &hp);
        (void)lo;
        return static_cast<float>(bp);
      }
      case 7: {                                                                      // brf_1p
        const double lo = s[c][0].process(x, &bp, &hp);
        return static_cast<float>(lo + hp);
      }
      case 8: return static_cast<float>(a[c].process(x));                           // apf_1p
      case 2: case 9: return static_cast<float>(s[c][0].process(x, &bp, &hp));     // lpf_2p
      case 3: case 10: s[c][0].process(x, &bp, &hp); return static_cast<float>(hp); // hpf_2p
      case 4: case 11: s[c][0].process(x, &bp, &hp); return static_cast<float>(bp); // bpf_2p
      case 5: case 12: {                                                             // brf_2p
        const double lo = s[c][0].process(x, &bp, &hp);
        return static_cast<float>(lo + hp);
      }
      case 13: return static_cast<float>(s[c][1].process(s[c][0].process(x, &bp, &hp), &bp, &hp));
      case 14: {
        s[c][0].process(x, &bp, &hp);
        const double h1 = hp;
        s[c][1].process(h1, &bp, &hp);
        return static_cast<float>(hp);
      }
      case 15: return static_cast<float>(s[c][2].process(s[c][1].process(s[c][0].process(x, &bp, &hp), &bp, &hp), &bp, &hp));
      case 16: {
        double y = x;
        for (int k = 0; k < 3; ++k) {
          s[c][k].process(y, &bp, &hp);
          y = hp;
        }
        return static_cast<float>(y);
      }
      case 17: case 18: case 19: case 20: case 23: return static_cast<float>(b[c].process(x));
      case 21: {
        const int di = static_cast<int>(combD);
        const float fr = static_cast<float>(combD - di);
        const float t0 = comb[c][(combW - di + 512) & 511], t1 = comb[c][(combW - di - 1 + 512) & 511];
        const float y = in + static_cast<float>(combFb) * (t0 + (t1 - t0) * fr);
        comb[c][combW] = y;
        if (c == 1) combW = (combW + 1) & 511;
        return y * 0.5f;
      }
      case 22: return static_cast<float>(p[c].process(x) * 0.5 + x * 0.5);         // pink-ish tilt
      default: return static_cast<float>(s[c][0].process(x, &bp, &hp));
    }
  }
};

struct ModSmooth {
  float v = -1;
};

struct Voice {
  bool on = false, held = false, sustained = false, sostenuto = false, released = false, fading = false, looping = false, releaseTrig = false;
  bool reverse = false, firstCtl = true;
  uint32_t id = 0;
  int key = 60, chan = 1, dir = 1, ctl = 0, loopsLeft = -1, countLeft = 0, ri = 0;
  float vel = 1, relVel = 0.5f;
  uint64_t age = 0;
  const RegionRt* rr = nullptr;
  double pos = 0, rate = 1, notePitch = 60, osc[9] = {}, oscFreq = 0, gainCur = 0, gainL = 0, gainR = 0;
  int64_t wait = 0, life = 0, stopAt = -1, fadeLen = 0, fadeLeft = 0;
  double fadeFrom = 1;
  PitchGlide slide;
  // fixed at the start
  double pitchStatic = 0, randPitch = 0, randCut[2] = {0, 0}, randRes[2] = {0, 0}, randAmpDb = 0, randPan = 0, randPos = 0;
  double velGain = 1, xfGain = 1, rtGain = 1, keyGainDb = 0;
  double delayStart = 0;
  ClassicState amp, fil, pit;
  std::array<FlexState, kEgs> egs{};
  float egMod[kEgs][kEgPts * 2] = {};  // controller offsets of the flexible envelopes' points
  std::array<LfoState, kLfos + 3> lfo{};  // v2 LFOs, then amp / fil / pitch v1
  FilterState f[2];
  gl::dsp::Biquad eq[3][2];
  std::array<ModSmooth, kMods> sm{};
  double bendSm = 0;
  // noise and lofi
  uint32_t rng = 1;
  double noiseHoldL = 0, noiseHoldR = 0, noiseT = 0, noiseLpL = 0, noiseLpR = 0, holdL = 0, holdR = 0, holdT = 0, ringPh = 0;
  // control-rate results
  double ampMul = 1, panV = 0, widthV = 1, posV = 0, noiseGain = 0, noiseStep = 0, noiseTone = 1, decim = 0, bitred = 0, rectify = 0, ring = 0, drive = 0;
  double oscDetune = 0, oscMod = 0, loopTuneMul = 1;
  float send[4] = {0, 0, 0, 0};
};

// ------------------------------------------------------------------ the player
class Player {
 public:
  void reset() {
    for (auto& v : voices_) v = Voice{};
    for (auto& k : keyHeld_) k = false;
    lastSwitch_ = -1, prevKey_ = -1, prevVel_ = 0;
  }
  // A new map (or none). Called from configure with the map's data.
  void setData(const SfzData* d, double sr) {
    if (d != data_) {
      for (auto& v : voices_) v.on = false;  // the old map's regions are gone
      releaseFx();
      data_ = d;
      seq_.fill(0);
      groupTimer_.fill(-1e9);
      timerSeen_ = groupTimer_;
      if (d) {
        for (int k = 0; k < kCcCount; ++k) cc_[static_cast<size_t>(k)] = d->ins.setCc[static_cast<size_t>(k)] >= 0 ? d->ins.setCc[static_cast<size_t>(k)] : 0.f;
        if (d->ins.setCc[7] < 0) cc_[7] = 100.f / 127.f;
        if (d->ins.setCc[10] < 0) cc_[10] = 0.5f;
        if (d->ins.setCc[11] < 0) cc_[11] = 1.f;
        lastSwitch_ = d->ins.swDefault;
        claimFx();
      }
    }
    sr_ = sr;
  }
  ~Player() { releaseFx(); }
  const SfzData* data() const { return data_; }
  // Which regions sound, for the editor's lit map (the first RegionActivity::kSlots voices).
  const gl::RegionActivity& activity() const { return act_; }
  bool active() const {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }
  void setTransport(double tempo, double beat, bool playing) {
    tempo_ = tempo > 0 ? tempo : 120;
    beat_ = beat;
    playing_ = playing;
  }
  void setProgramVolume(double gain, double transpose) {
    progGain_ = gain;
    transpose_ = transpose;
  }
  // a user-set controller from the editor knobs (0..1)
  void setCc(int cc, float v) { controller(cc, v); }
  float cc(int k) const { return k >= 0 && k < kCcCount ? cc_[static_cast<size_t>(k)] : 0.f; }
  int lastSwitch() const { return lastSwitch_; }
  // Lit regions for the editor (bit per region index, first 4096).
  void litRegions(std::vector<uint8_t>& out) const {
    for (const auto& v : voices_)
      if (v.on && v.rr && v.rr->index < static_cast<int>(out.size())) out[static_cast<size_t>(v.rr->index)] = 1;
  }

  void noteOn(uint32_t id, double pitch, float vel, int chan, float relVel = 0.5f) {
    if (!data_) return;
    const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    keyRel_[static_cast<size_t>(key)] = relVel;
    const double now = clock_;
    const bool othersHeld = anyHeld();
    keyVel_[static_cast<size_t>(key)] = vel;
    keyOn_[static_cast<size_t>(key)] = now;
    keyHeld_[static_cast<size_t>(key)] = true;
    cc_[kCcVelocity] = vel;
    cc_[kCcKey] = static_cast<float>(key) / 127.f;
    cc_[kCcGate] = 1.f;
    cc_[kCcRandUni] = fx::rndf(rng_);
    cc_[kCcRandBi] = fx::rndf(rng_);
    cc_[kCcAlternate] = cc_[kCcAlternate] > 0.5f ? 0.f : 1.f;
    if (prevKey_ >= 0) {
      cc_[kCcKeyDelta] = static_cast<float>(key - prevKey_ + 127) / 254.f;
      cc_[kCcAbsKeyDelta] = static_cast<float>(std::abs(key - prevKey_)) / 127.f;
    }
    const Instrument& ins = data_->ins;
    if (key >= ins.swLokey && key <= ins.swHikey) lastSwitch_ = key;
    const float rnd = fx::rndf(rng_);
    timerSeen_ = groupTimer_;  // every region of this note sees the timers as they were before it
    for (int ri : data_->attack[static_cast<size_t>(key)]) {
      const RegionRt& rr = data_->regions[static_cast<size_t>(ri)];
      const Region& r = *rr.r;
      if (r.trigger == Trigger::First && othersHeld) continue;
      if (r.trigger == Trigger::Legato && !othersHeld) continue;
      const float v = r.swVelPrevious ? prevVel_ : vel;
      if (!matches(r, key, v, chan, rnd)) continue;
      if (!seqOk(ri, r)) continue;
      start(id, key, pitch, v, chan, rr, false, 1.0);
    }
    prevKey_ = key;
    prevVel_ = vel;
  }
  void noteOff(uint32_t id) {
    if (!data_) return;
    float relVel = 0.5f;
    for (const auto& v : voices_)
      if (v.on && v.id == id && !v.releaseTrig) relVel = keyRel_[static_cast<size_t>(v.key)];
    int key = -1;
    for (auto& v : voices_)
      if (v.on && v.id == id && !v.releaseTrig) {
        key = v.key;
        v.held = false;
        v.relVel = relVel;
        const Region& r = *v.rr->r;
        if (r.sustainSw && cc_[static_cast<size_t>(std::clamp(r.sustainCc, 0, kCcCount - 1))] >= r.sustainLo) v.sustained = true;
        else if (!v.sostenuto) releaseVoice(v);
      }
    if (key < 0) {
      for (int k = 0; k < 128; ++k)
        if (keyId_[static_cast<size_t>(k)] == id) key = k;
      if (key < 0) return;
    }
    keyHeld_[static_cast<size_t>(key)] = false;
    cc_[kCcRelVelocity] = relVel;
    cc_[kCcGate] = anyHeld() ? 1.f : 0.f;
    releaseTriggers(key, relVel, false);
  }
  void cut(uint32_t id) {
    for (auto& v : voices_)
      if (v.on && v.id == id) fadeVoice(v, 0.004);
  }
  void allOff() {
    for (auto& v : voices_)
      if (v.on) v.held = false, v.sustained = false, releaseVoice(v);
    for (auto& k : keyHeld_) k = false;
  }
  void glide(uint32_t id, double pitch, double seconds) {
    for (auto& v : voices_)
      if (v.on && v.id == id) v.slide.start(v.notePitch, pitch, seconds, sr_);
  }
  void midi(uint8_t st, uint8_t d1, uint8_t d2) {
    const int kind = st & 0xF0;
    if (kind == 0xB0) controller(d1, static_cast<float>(d2) / 127.f);
    else if (kind == 0xE0) {
      const int raw = (d2 << 7 | d1) - 8192;
      bend_ = static_cast<float>(raw) / 8192.f;
      bendRaw_ = raw;
      cc_[kCcPitchBend] = (bend_ + 1.f) * 0.5f;
    } else if (kind == 0xD0) {
      cc_[kCcChanAft] = static_cast<float>(d1) / 127.f;
      chanAft_ = d1;
    } else if (kind == 0xA0) {
      polyAft_[d1 & 127] = d2;
      cc_[kCcPolyAft] = static_cast<float>(d2) / 127.f;
    } else if (kind == 0xC0) {
      program_ = d1;
    }
  }

  void render(float* L, float* R, int n) {
    if (!data_) return;
    for (int off = 0; off < n; off += kChunk) {
      const int m = std::min(kChunk, n - off);
      float mainL[kChunk] = {}, mainR[kChunk] = {};
      float fxL[4][kChunk] = {}, fxR[4][kChunk] = {};
      for (auto& v : voices_)
        if (v.on) renderVoice(v, mainL, mainR, fxL, fxR, m);
      // effects: fx buses first, then main
      bool fxUsed[4] = {false, false, false, false};
      for (size_t k = 0; k < data_->fx.size(); ++k) {
        const auto& s = data_->fx[k];
        if (fxSlot_ < 0 || s.def.bus < 1 || s.def.bus > 4) continue;
        const int b = s.def.bus - 1;
        fxUsed[b] = true;
        fx::process(s.def, *s.pool[static_cast<size_t>(fxSlot_)], fxL[b], fxR[b], m, sr_, tempo_, cc_.data());
      }
      double toMain[4] = {0, 0, 0, 0}, direct = 1;
      for (const auto& s : data_->fx)
        if (s.def.bus == 0) {
          direct = s.def.directToMain / 100.0;
          for (int b = 0; b < 4; ++b) toMain[b] = s.def.fxToMain[static_cast<size_t>(b)] / 100.0;
        }
      bool anyMainDef = false;
      for (const auto& s : data_->fx) anyMainDef |= s.def.bus == 0;
      for (int b = 0; b < 4; ++b)
        if (!anyMainDef && fxUsed[b]) toMain[b] = 1.0;  // a send bus without a main header still reaches the output
      for (int i = 0; i < m; ++i) {
        double l = mainL[i] * direct, r = mainR[i] * direct;
        for (int b = 0; b < 4; ++b) l += fxL[b][i] * toMain[b], r += fxR[b][i] * toMain[b];
        mainL[i] = static_cast<float>(l), mainR[i] = static_cast<float>(r);
      }
      for (size_t k = 0; k < data_->fx.size(); ++k) {
        const auto& s = data_->fx[k];
        if (fxSlot_ < 0 || s.def.bus != 0) continue;
        fx::process(s.def, *s.pool[static_cast<size_t>(fxSlot_)], mainL, mainR, m, sr_, tempo_, cc_.data());
      }
      for (int i = 0; i < m; ++i) L[off + i] += mainL[i], R[off + i] += mainR[i];
      for (int k = 0; k < gl::RegionActivity::kSlots; ++k) {
        const Voice& v = voices_[static_cast<size_t>(k)];
        if (v.on && v.rr) act_.set(k, v.ri, static_cast<int64_t>(v.pos), static_cast<float>(v.amp.lvl), v.held);
        else act_.off(k);
      }
      clock_ += static_cast<double>(m) / sr_;
      beat_ += static_cast<double>(m) / sr_ * tempo_ / 60.0;
    }
  }

 private:
  // ---- selection
  bool anyHeld() const {
    for (bool k : keyHeld_)
      if (k) return true;
    return false;
  }
  bool matches(const Region& r, int key, float vel, int chan, float rnd) const {
    const int v127 = std::clamp(static_cast<int>(std::lround(vel * 127.f)), 0, 127);
    if (key < r.lokey || key > r.hikey || v127 < r.lovel || v127 > r.hivel) return false;
    if (chan < r.lochan || chan > r.hichan) return false;
    if (bendRaw_ < r.lobend || bendRaw_ > r.hibend) return false;
    if (chanAft_ < r.lochanaft || chanAft_ > r.hichanaft) return false;
    if (polyAft_[key] < r.lopolyaft || polyAft_[key] > r.hipolyaft) return false;
    if (program_ < r.loprog || program_ > r.hiprog) return false;
    if (tempo_ < r.lobpm || tempo_ >= r.hibpm) return false;
    if (rnd < r.lorand || rnd >= r.hirand) {
      if (!(r.hirand >= 1.f && rnd >= r.lorand)) return false;
    }
    for (const auto& c : r.ccRange) {
      const float x = cc_[static_cast<size_t>(std::clamp(c.cc, 0, kCcCount - 1))] * 127.f;
      if (x < c.lo - 1e-3f || x > c.hi + 1e-3f) return false;
    }
    for (const auto& c : r.reverseCc) (void)c;
    // keyswitches
    if (r.swLast >= 0 && lastSwitch_ != r.swLast) return false;
    if (r.swLolast >= 0 && (lastSwitch_ < r.swLolast || lastSwitch_ > (r.swHilast >= 0 ? r.swHilast : r.swLolast))) return false;
    if (r.swDown >= 0 && !keyHeld_[static_cast<size_t>(std::clamp(r.swDown, 0, 127))]) return false;
    if (r.swUp >= 0 && keyHeld_[static_cast<size_t>(std::clamp(r.swUp, 0, 127))]) return false;
    if (r.swPrevious >= 0 && prevKey_ != r.swPrevious) return false;
    // timers: the time since a region of the same group last played
    if (r.lotimer > 0 || r.hitimer < 1e8f) {
      const double last = timerSeen_[static_cast<size_t>(static_cast<uint64_t>(r.group) & 63)];
      const bool never = last < -1e8;  // the group has not played yet: as long ago as can be
      if (never ? r.hitimer < 1e8f : (clock_ - last < r.lotimer || clock_ - last > r.hitimer)) return false;
    }
    return true;
  }
  bool seqOk(int ri, const Region& r) {
    if (r.seqLength <= 1) return true;
    const uint32_t c = seq_[static_cast<size_t>(ri) & 4095]++;
    return static_cast<int>(c % static_cast<uint32_t>(r.seqLength)) == r.seqPosition - 1;
  }

  void releaseTriggers(int key, float relVel, bool fromPedal) {
    const double held = clock_ - keyOn_[static_cast<size_t>(key)];
    const float vel = keyVel_[static_cast<size_t>(key)];
    bool attackAlive = false;
    for (const auto& v : voices_)
      if (v.on && v.key == key && !v.releaseTrig && !v.fading) attackAlive = true;
    const float rnd = fx::rndf(rng_);
    timerSeen_ = groupTimer_;
    for (int ri : data_->release[static_cast<size_t>(key)]) {
      const RegionRt& rr = data_->regions[static_cast<size_t>(ri)];
      const Region& r = *rr.r;
      if (r.trigger == Trigger::Release && !fromPedal && r.sustainSw && cc_[static_cast<size_t>(std::clamp(r.sustainCc, 0, kCcCount - 1))] >= r.sustainLo) {
        pendingRelease_[static_cast<size_t>(key)] = true;  // plays when the pedal comes up
        continue;
      }
      if (!r.rtDead && !attackAlive && r.trigger == Trigger::Release) continue;
      if (!matches(r, key, vel, 1, rnd)) continue;
      if (!seqOk(ri, r)) continue;
      // rt_decay: quieter the longer the key was held
      double db = -r.rtDecay * held;
      if (!r.rtDecayStages.empty()) {
        db = 0;
        double t = held;
        for (const auto& [amount, time] : r.rtDecayStages) {
          if (t <= 0) break;
          const double seg = std::min(t, static_cast<double>(std::max(1e-3f, time)));
          db -= amount * seg / std::max(1e-3f, time);
          t -= seg;
        }
      }
      start(keyId_[static_cast<size_t>(key)], key, key, vel, 1, rr, true, std::pow(10.0, db / 20.0));
    }
    (void)relVel;
  }

  void controller(int cc, float v) {
    if (cc < 0 || cc >= kCcCount) return;
    const float old = cc_[static_cast<size_t>(cc)];
    cc_[static_cast<size_t>(cc)] = v;
    if (!data_) return;
    // pedals
    for (auto& vo : voices_) {
      if (!vo.on) continue;
      const Region& r = *vo.rr->r;
      if (cc == r.sustainCc && r.sustainSw && v < r.sustainLo && vo.sustained) {
        vo.sustained = false;
        if (!vo.held && !vo.sostenuto) releaseVoice(vo);
      }
      if (cc == r.sostenutoCc && r.sostenutoSw) {
        if (v >= r.sostenutoLo && old < r.sostenutoLo && vo.held) vo.sostenuto = true;
        if (v < r.sostenutoLo && vo.sostenuto) {
          vo.sostenuto = false;
          if (!vo.held && !vo.sustained) releaseVoice(vo);
        }
      }
      for (const auto& s : r.stopCc)
        if (s.cc == cc && v * 127.f >= s.lo && v * 127.f <= s.hi) fadeVoice(vo, 0.004);
    }
    if (cc == 64 && v < 0.5f)
      for (int k = 0; k < 128; ++k)
        if (pendingRelease_[static_cast<size_t>(k)]) {
          pendingRelease_[static_cast<size_t>(k)] = false;
          releaseTriggers(k, 0.5f, true);
        }
    // controller-triggered regions
    for (int ri : data_->ccTriggered) {
      const RegionRt& rr = data_->regions[static_cast<size_t>(ri)];
      for (const auto& s : rr.r->onCc)
        if (s.cc == cc) {
          const float x = v * 127.f, ox = old * 127.f;
          const bool inNow = x >= s.lo && x <= s.hi, inBefore = ox >= s.lo && ox <= s.hi;
          if (inNow && !inBefore) {
            const int key = std::clamp(rr.r->pitchKeycenter >= 0 ? rr.r->pitchKeycenter : 60, 0, 127);
            start(0xFFFF0000u + static_cast<uint32_t>(ri), key, key, std::max(0.01f, v), 1, rr, false, 1.0);
          }
        }
    }
  }

  // ---- voices
  Voice* freeVoice() {
    for (auto& v : voices_)
      if (!v.on) return &v;
    Voice* best = nullptr;
    for (auto& v : voices_)
      if (v.fading && (!best || v.fadeLeft < best->fadeLeft)) best = &v;
    if (!best)
      for (auto& v : voices_)
        if (!best || v.age < best->age) best = &v;
    return best;
  }
  void releaseVoice(Voice& v) {
    if (!v.on || v.released) return;
    const Region& r = *v.rr->r;
    v.released = true;
    if (r.loopMode == LoopMode::OneShot && !v.releaseTrig) return;
    v.amp.off();
    v.fil.off();
    v.pit.off();
    for (size_t k = 0; k < r.egs.size() && k < kEgs; ++k) v.egs[k].release(r.egs[k]);
    if (r.loopMode == LoopMode::Sustain) v.looping = false;
  }
  void fadeVoice(Voice& v, double seconds) {
    if (!v.on || v.fading) return;
    v.fading = true;
    v.fadeLen = v.fadeLeft = std::max<int64_t>(1, static_cast<int64_t>(seconds * sr_));
    v.fadeFrom = 1;
  }
  void offBy(const Region& r, Voice& self) {
    if (r.group == 0) return;
    for (auto& v : voices_) {
      if (!v.on || &v == &self) continue;
      const Region& o = *v.rr->r;
      if (o.offBy != r.group) continue;
      switch (o.offMode) {
        case OffMode::Normal: releaseVoice(v); break;
        case OffMode::Time: fadeVoice(v, std::max(0.001f, o.offTime)); break;
        default: fadeVoice(v, 0.006); break;
      }
    }
  }
  void polyphony(const Region& r, int key, float vel) {
    if (r.polyphony > 0) {
      int count = 0;
      Voice* oldest = nullptr;
      for (auto& v : voices_)
        if (v.on && !v.fading && v.rr->r->group == r.group) {
          ++count;
          if (!oldest || v.age < oldest->age) oldest = &v;
        }
      if (count >= r.polyphony && oldest) fadeVoice(*oldest, 0.006);
    }
    if (r.notePolyphony > 0) {
      int count = 0;
      Voice* oldest = nullptr;
      for (auto& v : voices_)
        if (v.on && !v.fading && v.key == key && v.rr->r->group == r.group) {
          if (r.noteSelfmask && v.vel > vel) continue;  // a softer note does not cut a louder one
          ++count;
          if (!oldest || v.age < oldest->age) oldest = &v;
        }
      if (count >= r.notePolyphony && oldest) fadeVoice(*oldest, 0.006);
    }
  }

  void start(uint32_t id, int key, double pitch, float vel, int chan, const RegionRt& rr, bool releaseTrig, double rtGain) {
    const Region& r = *rr.r;
    polyphony(r, key, vel);
    Voice& v = *freeVoice();
    v = Voice{};
    v.on = true;
    v.held = !releaseTrig;
    v.releaseTrig = releaseTrig;
    v.id = id;
    if (!releaseTrig) keyId_[static_cast<size_t>(key)] = id;
    v.key = key;
    v.chan = chan;
    v.vel = vel;
    v.age = ++age_;
    v.rr = &rr;
    v.ri = rr.index;
    v.notePitch = pitch;
    v.rtGain = rtGain;
    act_.started(static_cast<int>(&v - voices_.data()), rr.index);
    v.rng = 0x9E3779B9u ^ static_cast<uint32_t>(age_ * 2654435761u);
    groupTimer_[static_cast<size_t>(static_cast<uint64_t>(r.group) & 63)] = clock_;
    offBy(r, v);
    // random draws
    auto bip = [&]() { return fx::rndf(v.rng) * 2.0 - 1.0; };
    v.randPitch = bip() * r.pitchRandom;
    for (int k = 0; k < 2; ++k) v.randCut[k] = bip() * r.filter[static_cast<size_t>(k)].random, v.randRes[k] = fx::rndf(v.rng) * r.filter[static_cast<size_t>(k)].resonanceRandom;
    v.randAmpDb = fx::rndf(v.rng) * r.ampRandom;
    v.randPan = bip() * r.panRandom;
    v.randPos = bip() * r.positionRandom;
    // velocity
    double velCurve = static_cast<double>(vel) * vel;
    if (!r.velcurve.empty()) {
      const double x = vel * 127.0;
      double x0 = 0, y0 = 0, x1 = 127, y1 = 1;
      for (const auto& [px, py] : r.velcurve) {
        if (px <= x) x0 = px, y0 = py;
        if (px >= x) {
          x1 = px, y1 = py;
          break;
        }
      }
      velCurve = x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y0;
    }
    double vt = r.ampVeltrack / 100.0 + bip() * r.ampVeltrackRandom / 100.0;
    for (const auto& m : r.cc)
      if (m.target == Target::AmpegStage && m.index == 100) vt += m.depth / 100.0 * ccVal(m.curve, m.cc);
    velCurve = std::clamp(velCurve, 0.0, 1.0);
    vt = std::clamp(vt, -1.0, 1.0);  // beyond 100% the track would turn the gain negative
    v.velGain = vt >= 0 ? 1.0 - vt + vt * velCurve : 1.0 + vt * velCurve;
    v.keyGainDb = r.ampKeytrack * (key - r.ampKeycenter);
    v.xfGain = crossfade(r, key, vel);
    // pitch (cents, fixed part)
    const int kc = r.pitchKeycenter >= 0 ? r.pitchKeycenter : 60;
    v.pitchStatic = r.transpose * 100.0 + r.tune + r.globalTune + r.masterTune + r.groupTune + r.pitchVeltrack * vel + v.randPitch;
    (void)kc;
    // sample position and direction
    v.reverse = r.reverse;
    for (const auto& c : r.reverseCc) {
      const float x = cc_[static_cast<size_t>(std::clamp(c.cc, 0, kCcCount - 1))] * 127.f;
      if (x >= c.lo && x <= c.hi) v.reverse = true;
    }
    int64_t off = rr.start;
    if (r.offsetRandom > 0) off += static_cast<int64_t>(fx::rndf(v.rng) * static_cast<float>(r.offsetRandom));
    for (const auto& m : r.cc)
      if (m.target == Target::Offset) off += static_cast<int64_t>(m.depth * ccVal(m.curve, m.cc));
    off = rr.n > 2 ? std::clamp<int64_t>(off, 0, rr.end - 2) : 0;
    v.dir = v.reverse ? -1 : 1;
    v.pos = v.reverse ? static_cast<double>(rr.end - 1) : static_cast<double>(off);
    v.looping = rr.hasLoop;
    v.loopsLeft = r.loopCount > 0 ? r.loopCount : -1;
    v.countLeft = r.count > 1 ? static_cast<int>(r.count) - 1 : 0;
    if (r.oscillator) {
      const double ph = r.oscPhase < 0 ? fx::rndf(v.rng) : r.oscPhase;
      for (double& p : v.osc) p = ph;
    }
    // delay
    double delay = r.delay + fx::rndf(v.rng) * r.delayRandom + (r.delayBeats + fx::rndf(v.rng) * r.delayBeatsRandom) * 60.0 / tempo_;
    int64_t delaySamples = r.delaySamples;
    for (const auto& m : r.cc)
      if (m.target == Target::Delay) {
        const double x = m.depth * ccVal(m.curve, m.cc);
        if (m.index == 0) delay += x;
        else if (m.index == 1) delaySamples += static_cast<int64_t>(x);
        else delay += x * 60.0 / tempo_;
      }
    if (r.syncBeats > 0 && playing_) {  // wait for the next multiple of sync_beats (plus the offset)
      const double b = beat_ - r.syncOffset;
      const double next = std::ceil(b / r.syncBeats) * r.syncBeats;
      delay += (next - b) * 60.0 / tempo_;
    }
    v.wait = static_cast<int64_t>(std::max(0.0, delay) * sr_) + std::max<int64_t>(0, delaySamples);
    v.stopAt = r.stopBeats > 0 ? static_cast<int64_t>(r.stopBeats * 60.0 / tempo_ * sr_) : -1;
    // envelopes
    setClassic(v.amp, r.ampeg, vel, true);
    setClassic(v.fil, r.fileg, vel, false);
    setClassic(v.pit, r.pitcheg, vel, false);
    v.amp.begin();
    v.fil.begin();
    v.pit.begin();
    for (size_t k = 0; k < r.egs.size() && k < kEgs; ++k) v.egs[k].begin(r.egs[k]);
    for (size_t k = 0; k < r.lfos.size() && k < kLfos; ++k) startLfo(v.lfo[k], r.lfos[k], v);
    startLfo(v.lfo[kLfos], r.amplfo, v);
    startLfo(v.lfo[kLfos + 1], r.fillfo, v);
    startLfo(v.lfo[kLfos + 2], r.pitchlfo, v);
    for (int k = 0; k < 4; ++k) v.send[k] = r.effectSend[static_cast<size_t>(k)] / 100.f;
    v.ctl = 0;
    v.firstCtl = true;
  }
  void startLfo(LfoState& s, const Lfo& l, Voice& v) {
    s = LfoState{};
    s.used = l.used;
    s.ph = l.phase;
    (void)v;
  }
  void setClassic(ClassicState& s, const ClassicEg& e, float vel, bool amp) {
    const ClassicEg def;
    const ClassicEg& x = e;
    auto ccAdd = [&](int stage) {
      double a = 0;
      for (const auto& c : x.cc)
        if (c.stage == stage) a += c.amount * ccVal(c.curve, c.cc);
      return a;
    };
    s.delay = std::max(0.0, x.delay + x.vel2delay * vel + ccAdd(0));
    s.start = std::clamp((x.start + ccAdd(1)) / 100.0, 0.0, 1.0);
    s.attack = std::max(0.0, x.attack + x.vel2attack * vel + ccAdd(2));
    s.hold = std::max(0.0, x.hold + x.vel2hold * vel + ccAdd(3));
    s.decay = std::max(0.0, x.decay + x.vel2decay * vel + ccAdd(4));
    s.sustain = std::clamp((x.sustain + x.vel2sustain * vel + ccAdd(5)) / 100.0, 0.0, 1.0);
    s.release = std::max(0.0, x.release + x.vel2release * vel + ccAdd(6));
    if (amp && !x.used) s.release = 0.001, s.attack = 0, s.sustain = 1;
    if (amp && s.release < 0.001) s.release = 0.001;  // no click on the way out
    s.aShape = std::clamp(x.attackShape, -30.f, 30.f), s.dShape = std::clamp(x.decayShape, -30.f, 30.f), s.rShape = std::clamp(x.releaseShape, -30.f, 30.f);
    s.decayZero = x.decayZero, s.releaseZero = x.releaseZero;
    (void)def;
  }
  double ccVal(int curve, int cc) const {
    const float x = cc_[static_cast<size_t>(std::clamp(cc, 0, kCcCount - 1))];
    return curve == 0 ? x : curveAt(data_->ins, curve, x);
  }
  double crossfade(const Region& r, int key, float vel) const {
    auto fade = [](double x, double lo, double hi, bool in, bool power) {
      double g = 1;
      if (hi > lo) {
        double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
        if (!in) t = 1.0 - t;
        g = power ? std::sqrt(t) : t;
      } else if (in ? x < lo : x > hi) {
        g = 0;
      }
      return g;
    };
    double g = 1;
    if (r.xfinHikey > r.xfinLokey) g *= fade(key, r.xfinLokey, r.xfinHikey, true, r.xfKeyPower);
    if (r.xfoutHikey > r.xfoutLokey && r.xfoutLokey < 127) g *= fade(key, r.xfoutLokey, r.xfoutHikey, false, r.xfKeyPower);
    const double v127 = vel * 127.0;
    if (r.xfinHivel > r.xfinLovel) g *= fade(v127, r.xfinLovel, r.xfinHivel, true, r.xfVelPower);
    if (r.xfoutHivel > r.xfoutLovel && r.xfoutLovel < 127) g *= fade(v127, r.xfoutLovel, r.xfoutHivel, false, r.xfVelPower);
    for (const auto& x : r.xfCc) {
      const double c = cc_[static_cast<size_t>(std::clamp(x.cc, 0, kCcCount - 1))] * 127.0;
      g *= fade(c, x.lo, x.hi, x.in, r.xfCcPower);
    }
    return g;
  }

  // Controller modulation value for one CcMod (smoothed per voice).
  double modVal(Voice& v, const CcMod& m, int slot) {
    double x = ccVal(m.curve, m.cc);
    if (m.smooth > 0 && slot >= 0 && slot < kMods) {
      ModSmooth& s = v.sm[static_cast<size_t>(slot)];
      if (s.v < 0) s.v = static_cast<float>(x);
      const double a = std::exp(-static_cast<double>(kCtl) / std::max(1.0, m.smooth / 1000.0 * sr_));
      s.v = static_cast<float>(x + (s.v - x) * a);
      x = s.v;
    }
    double y = m.depth * x;
    if (m.step > 0) y = std::round(y / m.step) * m.step;
    return y;
  }

  // Everything that moves, every kCtl frames.
  void control(Voice& v) {
    const RegionRt& rr = *v.rr;
    const Region& r = *rr.r;
    const double dt = static_cast<double>(kCtl) / sr_;
    const Instrument& ins = data_->ins;
    // accumulators per target
    double volDb = rr.baseDb + v.keyGainDb + v.randAmpDb + 20.0 * std::log10(std::max(1e-6, static_cast<double>(rr.baseAmp)));
    double ampPct = r.amplitude, pan = r.pan + v.randPan + r.panKeytrack * (v.key - r.panKeycenter) + r.panVeltrack * v.vel;
    double width = r.width, position = r.position + v.randPos + r.positionKeytrack * (v.key - r.positionKeycenter) + r.positionVeltrack * v.vel;
    double cents = v.pitchStatic;
    double cut[2] = {0, 0}, res[2] = {0, 0}, fgain[2] = {0, 0};
    double eqF[3] = {0, 0, 0}, eqB[3] = {0, 0, 0}, eqG[3] = {0, 0, 0};
    double noise = r.noiseLevel, nStep = r.noiseStep, nTone = r.noiseTone, decim = r.decim, bitred = r.bitred, rect = r.rectify, ring = r.ringmod, drive = r.drive;
    double oscDet = r.oscDetune, oscMod = r.oscModDepth;
    auto add = [&](Target t, int idx, double x) {
      switch (t) {
        case Target::Volume: volDb += x; break;
        case Target::Amplitude: ampPct += x; break;
        case Target::Pan: pan += x; break;
        case Target::Width: width += x; break;
        case Target::Position: position += x; break;
        case Target::Pitch: cents += x; break;
        case Target::Cutoff: cut[std::clamp(idx, 0, 1)] += x; break;
        case Target::Resonance: res[std::clamp(idx, 0, 1)] += x; break;
        case Target::FilGain: fgain[std::clamp(idx, 0, 1)] += x; break;
        case Target::EqFreq: eqF[std::clamp(idx, 0, 2)] += x; break;
        case Target::EqBw: eqB[std::clamp(idx, 0, 2)] += x; break;
        case Target::EqGain: eqG[std::clamp(idx, 0, 2)] += x; break;
        case Target::NoiseLevel: noise += x; break;
        case Target::NoiseStep: nStep += x; break;
        case Target::NoiseTone: nTone += x; break;
        case Target::Decim: decim += x; break;
        case Target::Bitred: bitred += x; break;
        case Target::Rectify: rect += x; break;
        case Target::RingMod: ring += x; break;
        case Target::Drive: drive += x; break;
        case Target::Oscillator: (idx == 0 ? oscDet : oscMod) += x; break;
        default: break;
      }
    };
    // controllers
    int slot = 0;
    for (const auto& m : r.cc) {
      const int s = slot++;
      if (m.target == Target::Offset || m.target == Target::Delay || m.target == Target::AmpegStage || m.target == Target::LoopStart || m.target == Target::LoopLength) continue;
      add(m.target, m.index, modVal(v, m, s));
    }
    // ARIA variables
    for (const auto& var : r.vars) {
      double x = var.mod == 1 ? 1.0 : 0.0;
      for (const auto& [c, amt] : var.cc) {
        const double val = ccVal(0, c) * amt;
        x = var.mod == 1 ? x * val : x + val;
      }
      for (const auto& [t, d] : var.dest) add(t, 0, d * x);
    }
    // v1 LFOs
    auto lfoStep = [&](LfoState& s, const Lfo& l, double freqAdd, double depthMul) -> double {
      if (!s.used) return 0;
      s.t += dt;
      if (s.t < l.delay) return 0;
      double freq = (l.freq + freqAdd) * l.ratio;
      for (const auto& c : l.cc)
        if (c.target == Target::LfoFreq && c.index <= 0) freq += c.depth * ccVal(c.curve, c.cc);
      const double fade = l.fade > 0 ? std::min(1.0, (s.t - l.delay) / l.fade) : 1.0;
      const double before = s.ph;
      if (l.count <= 0 || s.cycles < l.count) s.ph += freq * dt;
      const bool wrapped = std::floor(s.ph) != std::floor(before) || v.firstCtl;
      if (wrapped && std::floor(s.ph) != std::floor(before)) ++s.cycles;
      double y;
      if (!l.steps.empty()) {
        const double p = s.ph - std::floor(s.ph);
        y = l.steps[std::min(l.steps.size() - 1, static_cast<size_t>(p * static_cast<double>(l.steps.size())))];
      } else {
        y = waveAt(l.v1 ? 1 : l.wave, s.ph, s.sh, v.rng, wrapped);
      }
      for (size_t k = 0; k < l.subWaves.size(); ++k) {  // sub waveforms
        const double ratio = k < l.subRatios.size() ? l.subRatios[k] : 1.0;
        const double ofs = k < l.subOffsets.size() ? l.subOffsets[k] : 0.0;
        const double sc = k < l.subScales.size() ? l.subScales[k] : 1.0;
        y += sc * waveAt(l.subWaves[k], s.ph * ratio + ofs, s.sub[k], v.rng, wrapped);
      }
      y = y * l.scale * fade * depthMul + l.offset;
      if (l.smooth > 0) {
        const double a = std::exp(-dt / (l.smooth / 1000.0));
        y = s.sm = y + (s.sm - y) * a;
      }
      return s.out = y;
    };
    // v2 LFOs (with LFO-to-LFO frequency and depth)
    double lfoOut[kLfos] = {};
    for (size_t k = 0; k < r.lfos.size() && k < kLfos; ++k) {
      const Lfo& l = r.lfos[k];
      double fAdd = 0, dMul = 1, dAdd = 0;
      for (size_t j = 0; j < r.lfos.size() && j < kLfos; ++j)
        for (const auto& d : r.lfos[j].dest)
          if (d.index == static_cast<int>(k) + 1) {
            if (d.target == Target::LfoFreq) fAdd += d.depth * lfoOut[j];
            if (d.target == Target::LfoDepth) dMul *= 1.0 + d.depth * lfoOut[j];
            if (d.target == Target::LfoDepthAdd) dAdd += d.depth * lfoOut[j];
          }
      const double y = lfoStep(v.lfo[k], l, fAdd, dMul) + dAdd;
      lfoOut[k] = y;
      for (size_t di = 0; di < l.dest.size(); ++di) {
        const auto& d = l.dest[di];
        if (d.target == Target::LfoFreq || d.target == Target::LfoDepth || d.target == Target::LfoDepthAdd) continue;
        double depth = d.depth;
        for (const auto& c : l.cc)
          if (c.index == static_cast<int>(di) && c.target == d.target) depth += c.depth * ccVal(c.curve, c.cc);
        add(d.target, d.index, depth * y);
      }
      for (const auto& c : l.cc)  // controllers on a target that has no static depth
        if (c.index >= 0 && c.index < static_cast<int>(l.dest.size()) && l.dest[static_cast<size_t>(c.index)].depth == 0 && c.target != l.dest[static_cast<size_t>(c.index)].target)
          add(c.target, l.dest[static_cast<size_t>(c.index)].index, c.depth * ccVal(c.curve, c.cc) * y);
    }
    // v1 LFOs
    const Lfo* v1[3] = {&r.amplfo, &r.fillfo, &r.pitchlfo};
    const Target v1t[3] = {Target::Volume, Target::Cutoff, Target::Pitch};
    for (int k = 0; k < 3; ++k) {
      const Lfo& l = *v1[k];
      if (!l.used) continue;
      const double y = lfoStep(v.lfo[static_cast<size_t>(kLfos + k)], l, 0, 1);
      double depth = l.dest.empty() ? 0.0 : l.dest[0].depth;
      for (const auto& c : l.cc)
        if (c.target == v1t[k]) depth += c.depth * ccVal(c.curve, c.cc);
      add(v1t[k], 0, depth * y);
    }
    // dynamic classic envelopes follow their controllers while they play
    if (r.ampeg.dynamic) setClassic(v.amp, r.ampeg, v.vel, true);
    if (r.fileg.dynamic) setClassic(v.fil, r.fileg, v.vel, false);
    if (r.pitcheg.dynamic) setClassic(v.pit, r.pitcheg, v.vel, false);
    // classic envelopes: pitch and filter
    {
      double pd = r.pitcheg.depth + r.pitcheg.vel2depth * v.vel;
      for (const auto& c : r.pitcheg.cc)
        if (c.stage == 7) pd += c.amount * ccVal(c.curve, c.cc);
      if (r.pitcheg.used) cents += pd * v.pit.lvl;
      double fd = r.fileg.depth + r.fileg.vel2depth * v.vel;
      for (const auto& c : r.fileg.cc)
        if (c.stage == 7) fd += c.amount * ccVal(c.curve, c.cc);
      if (r.fileg.used) cut[0] += fd * v.fil.lvl, cut[1] += fd * v.fil.lvl;
      for (int k = 0; k < kCtl; ++k) v.fil.next(1.0 / sr_), v.pit.next(1.0 / sr_);
    }
    // flex envelopes
    double flexAmp = -1;
    for (size_t k = 0; k < r.egs.size() && k < kEgs; ++k) {
      const FlexEg& e = r.egs[k];
      if (v.firstCtl || e.dynamic) {  // point controllers: read at the start, or all along when dynamic
        float* mod = v.egMod[k];
        std::fill(mod, mod + kEgPts * 2, 0.f);
        for (const auto& c : e.pointCc)
          if (c.index >= 0 && c.index < kEgPts * 2) mod[c.index] += static_cast<float>(c.depth * ccVal(c.curve, c.cc));
      }
      const double y = v.egs[k].next(e, ins, dt, v.egMod[k]);
      for (size_t di = 0; di < e.dest.size(); ++di) {
        const auto& d = e.dest[di];
        double depth = d.depth;
        for (const auto& c : e.depthCc)
          if (c.index == static_cast<int>(di)) depth += c.depth * ccVal(c.curve, c.cc);
        if (d.target == Target::LfoFreq || d.target == Target::LfoDepth || d.target == Target::LfoDepthAdd) continue;
        add(d.target, d.index, depth * y);
      }
      if (e.ampeg) flexAmp = std::max(0.0, y);
    }
    // pitch bend (smoothed, stepped)
    {
      double b = bend_;
      if (r.bendSmooth > 0) {
        const double a = std::exp(-dt / (r.bendSmooth / 1000.0));
        b = v.bendSm = b + (v.bendSm - b) * a;
      }
      double bc = b >= 0 ? b * r.bendUp : -b * r.bendDown;
      const double step = b >= 0 ? (r.bendStepUp > 0 ? r.bendStepUp : r.bendStep) : (r.bendStepDown > 0 ? r.bendStepDown : r.bendStep);
      if (step > 1) bc = std::round(bc / step) * step;
      cents += bc;
    }
    // pitch: rate
    const int kc = r.pitchKeycenter >= 0 ? r.pitchKeycenter : 60;
    double semis = (v.notePitch + transpose_ - kc) * r.pitchKeytrack / 100.0 + cents / 100.0 + (v.looping && r.loopTune != 0 ? r.loopTune / 100.0 : 0.0);
    semis = std::isfinite(semis) ? std::clamp(semis, -72.0, 72.0) : 0.0;  // six octaves either way
    if (r.builtin != Builtin::None || r.oscillator) {
      v.oscFreq = std::min(sr_ * 0.45, 440.0 * std::exp2((kc + semis - 69.0) / 12.0));
    }
    if (rr.smp) v.rate = std::exp2(semis / 12.0) * rr.smp->sampleRate / sr_;
    // filters
    for (int k = 0; k < 2; ++k) {
      const Filter& f = r.filter[static_cast<size_t>(k)];
      if (f.type < 0 || f.cutoff < 0) {
        v.f[k].type = -1;
        continue;
      }
      const double c = cut[k] + f.keytrack * (v.key - f.keycenter) + f.veltrack * v.vel + v.randCut[k];
      const double hz = f.mode == 1 ? f.cutoff + c : f.cutoff * std::exp2(c / 1200.0);
      v.f[k].set(f.type, sr_, hz, f.resonance + res[k] + v.randRes[k], f.gain + fgain[k]);
    }
    for (int k = 0; k < 3; ++k) {
      const Eq& e = r.eq[static_cast<size_t>(k)];
      if (!e.used) continue;
      double hz = e.freq + eqF[k] + e.vel2freq * v.vel, bw = e.bw + eqB[k], gdb = e.gain + eqG[k] + e.vel2gain * v.vel;
      hz = std::isfinite(hz) ? std::clamp(hz, 20.0, sr_ * 0.45) : 1000.0;
      bw = std::isfinite(bw) ? std::clamp(bw, 0.05, 4.0) : 1.0;
      gdb = std::isfinite(gdb) ? std::clamp(gdb, -48.0, 48.0) : 0.0;
      const double q = std::sqrt(std::exp2(bw)) / (std::exp2(bw) - 1.0);
      const gl::dsp::Biquad::Type t = e.type == 1 ? gl::dsp::Biquad::LowShelf : e.type == 2 ? gl::dsp::Biquad::HighShelf : gl::dsp::Biquad::Peak;
      for (int c = 0; c < 2; ++c) v.eq[k][c].set(t, sr_, hz, q, gdb);
    }
    // amplifier
    volDb = std::isfinite(volDb) ? std::min(volDb, 48.0) : -144.0;
    ampPct = std::isfinite(ampPct) ? std::clamp(ampPct, 0.0, 10000.0) : 0.0;
    const double gain = std::pow(10.0, volDb / 20.0) * (ampPct / 100.0) * v.velGain * v.xfGain * v.rtGain * progGain_ * (flexAmp >= 0 ? flexAmp : 1.0);
    v.ampMul = r.phaseInvert ? -gain : gain;
    v.panV = std::clamp(pan / 100.0, -1.0, 1.0);
    v.widthV = std::clamp(width / 100.0, -1.0, 1.0);
    v.posV = std::clamp(position / 100.0, -1.0, 1.0);
    v.noiseGain = noise > -96 ? std::pow(10.0, std::min(noise, 24.0) / 20.0) : 0.0;
    v.noiseStep = std::clamp(nStep, 0.0, 100.0);
    v.noiseTone = std::clamp(nTone, 0.0, 100.0);
    v.decim = std::clamp(decim, 0.0, 100.0), v.bitred = std::clamp(bitred, 0.0, 100.0);
    v.rectify = std::clamp(rect / 100.0, 0.0, 1.0), v.ring = std::clamp(ring / 100.0, 0.0, 1.0), v.drive = std::clamp(drive / 100.0, 0.0, 1.0);
    v.oscDetune = oscDet, v.oscMod = oscMod / 100.0;
    v.firstCtl = false;
  }

  // One sample of the source (sample, built-in or oscillator).
  void source(Voice& v, float& l, float& r) {
    const RegionRt& rr = *v.rr;
    const Region& reg = *rr.r;
    if (reg.builtin != Builtin::None) {
      const double inc = v.oscFreq / sr_;
      double& p = v.osc[0];
      double y = 0;
      switch (reg.builtin) {
        case Builtin::Sine: y = std::sin(gl::dsp::kTwoPi * p); break;
        case Builtin::Triangle: y = 1.0 - 4.0 * std::fabs(p - 0.5); break;
        case Builtin::Square: y = p < 0.5 ? 1.0 : -1.0; break;
        case Builtin::Saw: y = 2.0 * p - 1.0; break;
        case Builtin::Noise: y = fx::rndf(v.rng) * 2.0 - 1.0; break;
        default: y = 0; break;
      }
      p += inc;
      p -= std::floor(p);
      l = r = static_cast<float>(y);
      return;
    }
    const Sample& s = *rr.smp;
    if (reg.oscillator) {  // the sample is one cycle of a wavetable
      const double len = static_cast<double>(rr.n);
      const int voices = std::clamp(reg.oscMulti, 1, 9);
      double accL = 0, accR = 0;
      const int pairs = reg.oscMode > 0 ? std::max(1, voices / 2) : voices;
      for (int k = 0; k < pairs; ++k) {
        const double det = voices > 1 ? (static_cast<double>(k) / std::max(1, pairs - 1) - 0.5) * v.oscDetune : 0.0;
        const double inc = std::min(0.45, v.oscFreq * std::exp2(std::clamp(det, -2400.0, 2400.0) / 1200.0) / sr_);
        double& p = v.osc[k];
        auto at = [&](const float* d, double ph) {
          ph -= std::floor(ph);
          const double x = ph * len;
          const int64_t i = static_cast<int64_t>(x);
          const float fr = static_cast<float>(x - static_cast<double>(i));
          const float a = d[i % rr.n], b = d[(i + 1) % rr.n];
          return a + (b - a) * fr;
        };
        if (reg.oscMode == 0) {
          accL += at(s.left.data(), p), accR += at(s.right.data(), p);
        } else {
          double& q = v.osc[std::min(8, k + pairs)];
          const double qinc = inc * 1.0;
          const float mod = at(s.left.data(), q);
          q += qinc;
          q -= std::floor(q);
          if (reg.oscMode == 1) accL += at(s.left.data(), p) * mod, accR += at(s.right.data(), p) * mod;           // ring
          else accL += at(s.left.data(), p + mod * v.oscMod), accR += at(s.right.data(), p + mod * v.oscMod);     // FM
        }
        p += inc;
        p -= std::floor(p);
      }
      l = static_cast<float>(accL / pairs), r = static_cast<float>(accR / pairs);
      return;
    }
    Reader rl{s.left.data(), rr.n, rr.ls, rr.le, false};
    Reader rrd{s.right.data(), rr.n, rr.ls, rr.le, false};
    l = rl.read(v.pos, data_->interp, v.rate);
    r = rrd.read(v.pos, data_->interp, v.rate);
    // loop crossfade: the end of the loop blends into its start
    const double xf = reg.loopCrossfade * rr.smp->sampleRate;
    if (v.looping && xf >= 2 && v.dir > 0 && reg.loopType == LoopType::Forward) {
      const double from = static_cast<double>(rr.le) - std::min(xf, static_cast<double>(rr.le - rr.ls) - 1);
      if (v.pos >= from) {
        const double w = (v.pos - from) / (static_cast<double>(rr.le) - from);
        const double back = v.pos - static_cast<double>(rr.le - rr.ls);
        const float c0 = static_cast<float>(std::cos(w * kPi * 0.5)), c1 = static_cast<float>(std::sin(w * kPi * 0.5));
        l = l * c0 + rl.read(std::max(0.0, back), data_->interp, v.rate) * c1;
        r = r * c0 + rrd.read(std::max(0.0, back), data_->interp, v.rate) * c1;
      }
    }
  }
  // Advances the play head; false when the voice ends.
  bool advance(Voice& v) {
    const RegionRt& rr = *v.rr;
    const Region& reg = *rr.r;
    if (reg.builtin != Builtin::None || reg.oscillator) return true;
    v.pos += v.rate * v.dir;
    const double ls = static_cast<double>(rr.ls), le = static_cast<double>(rr.le);
    if (v.looping) {
      if (reg.loopType == LoopType::Alternate) {
        if (v.dir > 0 && v.pos >= le - 1) v.pos = le - 1 - (v.pos - (le - 1)), v.dir = -1;
        else if (v.dir < 0 && v.pos < ls) {
          v.pos = ls + (ls - v.pos), v.dir = 1;
          if (v.loopsLeft > 0 && --v.loopsLeft == 0) v.looping = false;
        }
        return true;
      }
      if (reg.loopType == LoopType::Backward) {
        if (v.dir > 0 && v.pos >= le) v.pos = le - 1, v.dir = -1;  // the first pass reaches the loop end, then runs back
        else if (v.dir < 0 && v.pos < ls) {
          v.pos = le - (ls - v.pos);
          if (v.loopsLeft > 0 && --v.loopsLeft == 0) v.looping = false, v.dir = 1, v.pos = le;
        }
        return true;
      }
      if (v.dir > 0 && v.pos >= le) {
        v.pos = ls + std::fmod(v.pos - ls, le - ls);
        if (v.loopsLeft > 0 && --v.loopsLeft == 0) v.looping = false;
      } else if (v.dir < 0 && v.pos < ls) {
        v.pos = le - std::fmod(ls - v.pos, le - ls);
        if (v.loopsLeft > 0 && --v.loopsLeft == 0) v.looping = false;
      }
      return true;
    }
    const bool out = (v.dir > 0 && v.pos >= static_cast<double>(rr.end - 1)) || (v.dir < 0 && v.pos < static_cast<double>(rr.start));
    if (!out) return true;
    if (v.countLeft > 0) {  // count: the sample plays again from its start
      --v.countLeft;
      v.pos = v.dir > 0 ? static_cast<double>(rr.start) : static_cast<double>(rr.end - 1);
      return true;
    }
    return false;
  }

  void renderVoice(Voice& v, float* L, float* R, float (*fxL)[kChunk], float (*fxR)[kChunk], int m) {
    const RegionRt& rr = *v.rr;
    const Region& reg = *rr.r;
    const double dt = 1.0 / sr_;
    for (int i = 0; i < m; ++i) {
      if (v.wait > 0) {
        --v.wait;
        continue;
      }
      if (v.ctl-- <= 0) {
        v.ctl = kCtl - 1;
        control(v);
      }
      if (v.slide.active()) v.slide.advance(v.notePitch);
      ++v.life;
      if (v.stopAt >= 0 && v.life >= v.stopAt && !v.fading) fadeVoice(v, 0.006);
      const double a = v.amp.next(dt);
      if (v.amp.st == ClassicState::Done) {
        v.on = false;
        return;
      }
      float l, r;
      source(v, l, r);
      // lofi
      if (v.decim > 0) {
        const double hold = 1.0 + v.decim * 0.63;
        if ((v.holdT += 1.0) >= hold) v.holdT -= hold, v.holdL = l, v.holdR = r;
        l = static_cast<float>(v.holdL), r = static_cast<float>(v.holdR);
      }
      if (v.bitred > 0) {
        const double q = std::exp2(16.0 - 0.15 * v.bitred - 1.0);
        l = static_cast<float>(std::round(l * q) / q), r = static_cast<float>(std::round(r * q) / q);
      }
      if (v.rectify > 0) l = static_cast<float>(l + (std::fabs(l) - l) * v.rectify), r = static_cast<float>(r + (std::fabs(r) - r) * v.rectify);
      if (v.ring > 0) {
        const double s = std::sin(gl::dsp::kTwoPi * v.ringPh);
        v.ringPh += 440.0 / sr_;
        v.ringPh -= std::floor(v.ringPh);
        l = static_cast<float>(l * (1.0 - v.ring + v.ring * s)), r = static_cast<float>(r * (1.0 - v.ring + v.ring * s));
      }
      if (v.drive > 0) {
        const double d = 1.0 + 20.0 * v.drive;
        l = static_cast<float>(std::tanh(l * d) / std::tanh(d)), r = static_cast<float>(std::tanh(r * d) / std::tanh(d));
      }
      // noise source
      if (v.noiseGain > 0) {
        const double hold = 1.0 + v.noiseStep;
        if ((v.noiseT += 1.0) >= hold) {
          v.noiseT -= hold;
          v.noiseHoldL = fx::rndf(v.rng) * 2.0 - 1.0;
          v.noiseHoldR = reg.noiseStereo ? fx::rndf(v.rng) * 2.0 - 1.0 : v.noiseHoldL;
        }
        const double c = v.noiseTone > 0 ? std::exp(-gl::dsp::kTwoPi * std::min(sr_ * 0.45, 20000.0 * std::exp2(-v.noiseTone * 0.066)) / sr_) : 0.0;  // 0 white, 100 dark
        v.noiseLpL = v.noiseHoldL + (v.noiseLpL - v.noiseHoldL) * c;
        v.noiseLpR = v.noiseHoldR + (v.noiseLpR - v.noiseHoldR) * c;
        if (reg.noiseFilter != 0) l += static_cast<float>(v.noiseLpL * v.noiseGain), r += static_cast<float>(v.noiseLpR * v.noiseGain);
      }
      // filters and EQ
      for (int k = 0; k < 2; ++k)
        if (v.f[k].type >= 0) l = v.f[k].process(0, l), r = v.f[k].process(1, r);
      if (v.noiseGain > 0 && reg.noiseFilter == 0) l += static_cast<float>(v.noiseLpL * v.noiseGain), r += static_cast<float>(v.noiseLpR * v.noiseGain);
      for (int k = 0; k < 3; ++k)
        if (reg.eq[static_cast<size_t>(k)].used) l = static_cast<float>(v.eq[k][0].process(l)), r = static_cast<float>(v.eq[k][1].process(r));
      // fades
      double fade = 1;
      if (v.fading) {
        if (--v.fadeLeft <= 0) {
          v.on = false;
          return;
        }
        const double x = static_cast<double>(v.fadeLeft) / static_cast<double>(std::max<int64_t>(1, v.fadeLen));
        fade = reg.offCurve >= 0 ? curveAt(data_->ins, reg.offCurve, static_cast<float>(x)) : reg.offShape != 0 ? (std::exp(std::clamp(reg.offShape, -30, 30) * x) - 1.0) / (std::exp(static_cast<double>(std::clamp(reg.offShape, -30, 30))) - 1.0) : x;
      }
      if (reg.sampleFadeout > 0 && rr.smp && !v.looping) {
        const double left = (v.dir > 0 ? static_cast<double>(rr.end) - v.pos : v.pos - static_cast<double>(rr.start)) / std::max(1e-9, v.rate) / sr_;
        if (left < reg.sampleFadeout) fade *= std::max(0.0, left / reg.sampleFadeout);
      }
      if (rr.smp && !v.looping && reg.builtin == Builtin::None && !reg.oscillator) {  // no click at the sample end
        const double left = (v.dir > 0 ? static_cast<double>(rr.end - 1) - v.pos : v.pos - static_cast<double>(rr.start)) / std::max(1e-9, v.rate);
        if (left < 32 && v.countLeft == 0) fade *= std::max(0.0, left / 32.0);
      }
      // width, position, pan
      const double mid = (l + r) * 0.5, side = (l - r) * 0.5 * v.widthV;
      double ol = mid + side, orr = mid - side;
      const double bal = std::clamp(v.posV + v.panV, -1.0, 1.0);
      double gl, gr;
      if (reg.panLaw == 1) gl = std::sqrt(0.5 * (1.0 - bal)) * 1.4142, gr = std::sqrt(0.5 * (1.0 + bal)) * 1.4142;
      else gl = std::min(1.0, 1.0 - bal), gr = std::min(1.0, 1.0 + bal);
      const double g = a * v.ampMul * fade;
      ol *= g * gl, orr *= g * gr;
      L[i] += static_cast<float>(ol);
      R[i] += static_cast<float>(orr);
      if (data_->anySend)
        for (int b = 0; b < 4; ++b)
          if (v.send[b] > 0) fxL[b][i] += static_cast<float>(ol * v.send[b]), fxR[b][i] += static_cast<float>(orr * v.send[b]);
      if (!advance(v)) {
        v.on = false;
        return;
      }
    }
  }

  void claimFx() {
    fxSlot_ = -1;
    if (!data_ || data_->fx.empty()) return;
    for (int k = 0; k < 4; ++k) {
      bool expected = false;
      if (data_->fxTaken[static_cast<size_t>(k)].compare_exchange_strong(expected, true)) {
        fxSlot_ = k;
        for (const auto& s : data_->fx) s.pool[static_cast<size_t>(k)]->reset();
        return;
      }
    }
  }
  void releaseFx() {
    if (data_ && fxSlot_ >= 0) data_->fxTaken[static_cast<size_t>(fxSlot_)].store(false);
    fxSlot_ = -1;
  }

  const SfzData* data_ = nullptr;
  gl::RegionActivity act_;
  std::array<Voice, kVoices> voices_{};
  std::array<float, kCcCount> cc_{};
  std::array<uint32_t, 4096> seq_{};
  std::array<double, 64> groupTimer_{}, timerSeen_{};
  std::array<bool, 128> keyHeld_{}, pendingRelease_{};
  std::array<float, 128> keyVel_{}, keyRel_{};
  std::array<double, 128> keyOn_{};
  std::array<uint32_t, 128> keyId_{};
  std::array<int, 128> polyAft_{};
  double sr_ = 44100, clock_ = 0, tempo_ = 120, beat_ = 0, progGain_ = 1, transpose_ = 0;
  bool playing_ = false;
  float bend_ = 0, prevVel_ = 0;
  int bendRaw_ = 0, chanAft_ = 0, program_ = 0, lastSwitch_ = -1, prevKey_ = -1, fxSlot_ = -1;
  uint64_t age_ = 0;
  uint32_t rng_ = 0x51ED270Bu;
};

}  // namespace sfzp

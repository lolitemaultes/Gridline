// Gridline - "chater" pack: the Chater engine (gl.chater).
//
// Signal flow (per sample, control values per 32-sample block aligned to the absolute frame):
//   input -> Catch (rolling capture, or a frozen loop while Hold is on)
//         -> breaks (Repeat, Reverse, Tape, Scrub, Jump, Skip) = the main signal; Soft edges
//            round and darken them, Wash sends them into the Room and Echo instead of cutting
//   main  -> dry path (delayed by the Halo window so both paths line up)
//   main + sparkle (grains, shimmer, crystal) -> Halo -> Grit -> Haze (warmth, damp)
//         -> Memory (wow, flutter, ensemble, tape age, hiss, crackle) -> Spray
//         -> Room (diffuse, modulated, shimmer, freeze) / Echo (diffuse) sends -> Tone
//   out = dry * (1 - mix) + wet * mix, Output gain, safety limiter.
// Breaks sit before the dry/wet split: a Skip or a Repeat always cuts the beat (washed
// breaks keep part of the input under them).
//
// When breaks fire: on the tempo grid (each Grid step fires with the chance Density, shaped
// by Lean, plus the Glitch macro) and/or on the input's own hits (an onset detector). Every
// random choice hashes the Seed with the grid step's song index (or the hit's number since
// play), so renders repeat exactly and an offline bounce matches playback.
//
// Audio thread: no allocation, locks or I/O. Every buffer is allocated in the constructor.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include "../../engine/Dsp.h"
#include "../Packs.h"
#include "chater.h"

namespace gl::packs::chater {

namespace {

constexpr int kCatchBits = 20, kCatch = 1 << kCatchBits, kCatchMask = kCatch - 1;
constexpr int kCB = 32;                  // control block (frames)
constexpr int kMaxGrains = 64;
constexpr int kCombLen = 16384;          // crystal lines
constexpr int kRoomLen = 32768;          // room lines
constexpr int kEchoLen = 1 << 19;        // echo lines
constexpr int kShimLen = 1 << 14;        // shimmer pitch-shift lines
constexpr int kHaloMax = 1024;
constexpr int kDryLen = 2048;            // dry-path latency line (>= kHaloMax)
constexpr int kTestLog = 4096;
constexpr int kMemLen = 16384;           // Memory: the wow / flutter / ensemble line
constexpr int kApLen = 1024;             // room input diffusers (each)
constexpr int kEcApLen = 2048;           // echo diffusers (each)
constexpr int kRShLen = 8192;            // room shimmer pitch-shift lines
constexpr double kPi = 3.14159265358979323846;

inline uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
// A uniform number in [0, 1) from (seed, key, salt): the same on every run.
inline double h01(uint32_t seed, int64_t key, uint32_t salt) {
  uint32_t x = mix32(seed * 0x9E3779B9U + 0x632BE5ABU);
  x = mix32(x ^ static_cast<uint32_t>(key) ^ mix32(static_cast<uint32_t>(static_cast<uint64_t>(key) >> 32) + salt * 0x85EBCA6BU));
  return (x >> 8) * (1.0 / 16777216.0);
}
struct Rng {
  uint32_t s = 1;
  void seed(uint32_t v) { s = mix32(v * 0x9E3779B9U + 1) | 1u; }
  double next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (s >> 8) * (1.0 / 16777216.0);
  }
};
inline float hermite(float y0, float y1, float y2, float y3, float t) {
  const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
  return ((c3 * t + c2) * t + c1) * t + y1;
}
inline float flush(float v) { return std::fabs(v) < 1e-15f ? 0.0f : v; }
inline double flushd(double v) { return std::fabs(v) < 1e-15 ? 0.0 : v; }
// One Schroeder allpass on a power-of-two line: w = x + g * d, y = d - g * w.
inline float allpass(float* line, int mask, int w, int len, float x, float g) {
  const float d = line[(w - len) & mask];
  const float v = x + g * d;
  line[w & mask] = flush(v);
  return d - g * v;
}
inline double softLimit(double x, double t) {  // transparent below t, approaches 1 above
  const double a = std::fabs(x);
  if (a <= t) return x;
  const double k = 1.0 - t;
  return std::copysign(t + k * std::tanh((a - t) / k), x);
}

// ------------------------------------------------------------------ FFT (radix 2, in place)
struct Fft {
  int n = 0;
  std::vector<float> cs, sn;
  std::vector<int> rev;
  void init(int size) {
    n = size;
    cs.assign(static_cast<size_t>(n / 2), 0);
    sn.assign(static_cast<size_t>(n / 2), 0);
    rev.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n / 2; ++i) cs[static_cast<size_t>(i)] = static_cast<float>(std::cos(2 * kPi * i / n)), sn[static_cast<size_t>(i)] = static_cast<float>(-std::sin(2 * kPi * i / n));
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
      rev[static_cast<size_t>(i)] = r;
    }
  }
  // size m (power of two, m <= n); inverse uses the conjugate twiddles (no scaling)
  void run(float* re, float* im, int m, bool inverse) const {
    const int shift = [&] {
      int s = 0;
      while ((m << s) < n) ++s;
      return s;
    }();
    for (int i = 0; i < m; ++i) {
      const int j = rev[static_cast<size_t>(i << shift)];
      if (j > i) std::swap(re[i], re[j]), std::swap(im[i], im[j]);
    }
    for (int len = 2; len <= m; len <<= 1) {
      const int half = len >> 1, step = (n / len);
      for (int i = 0; i < m; i += len)
        for (int j = 0; j < half; ++j) {
          const float wr = cs[static_cast<size_t>(j * step)], wi = inverse ? -sn[static_cast<size_t>(j * step)] : sn[static_cast<size_t>(j * step)];
          const int a = i + j, b = a + half;
          const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
          re[b] = re[a] - tr, im[b] = im[a] - ti;
          re[a] += tr, im[a] += ti;
        }
    }
  }
};

// ------------------------------------------------------------------ live data (audio -> UI)
struct Tel {
  std::atomic<double> beat{0}, tempo{120}, barOrigin{0}, barBeats{4};
  std::atomic<int> playing{0}, active{0};
  std::atomic<float> peak[kTapeCols];
  std::atomic<uint8_t> type[kTapeCols];
  struct Ev {
    std::atomic<uint32_t> seq{0};
    std::atomic<double> beat{0}, beats{0};
    std::atomic<int> type{0}, repeats{0}, hit{0};
  } ev[kEventRing];
  std::atomic<uint32_t> evCount{0};
  std::atomic<float> follower{0}, swA{0}, swB{0}, outL{0}, outR{0}, wow{0}, room{0};
  std::atomic<float> crystal[6];
  std::atomic<float> macros[8];
  struct G {
    std::atomic<float> pan{0}, semis{0}, life{0}, level{0};
  } grains[kViewGrains];
  Tel() {
    for (auto& p : peak) p.store(0, std::memory_order_relaxed);
    for (auto& t : type) t.store(0, std::memory_order_relaxed);
    for (auto& c : crystal) c.store(0, std::memory_order_relaxed);
    for (auto& m : macros) m.store(0, std::memory_order_relaxed);
  }
};

struct Grain {
  bool on = false;
  double pos = 0, step = 1;
  int len = 0, age = 0, md = 0;
  float gl = 0, gr = 0, semis = 0, pan = 0;
  int win = WinBell;
};

struct BreakState {
  bool on = false;
  int type = BrNone;
  int64_t start = 0;  // catch index of the sample the break started on
  int len = 0, el = 0, fade = 1;
  // Repeat
  double slice = 0, slicePos = 0, shrink = 1, ramp = 0, gate = 1;
  int rep = 0;
  // Tape
  double tapeLen = 1, curve = 0.6, pos = 0;
  bool rise = false;
  // Scrub
  double depth = 0, period = 1;
  // Jump
  int64_t offset = 0;
  float panL = 1, panR = 1;
  // soft edges: the break's own low-pass (darker on each repeat, with tape speed) and wash
  double soft = 0, wash = 0, lpL = 0, lpR = 0;
};

class ChaterFx final : public Effect {
 public:
  ChaterFx() {
    info_ = findPlugin(kType);
    catchL_.assign(kCatch, 0), catchR_.assign(kCatch, 0);
    combL_.assign(6 * kCombLen, 0), combR_.assign(6 * kCombLen, 0);
    room_.assign(8 * kRoomLen, 0);
    echoL_.assign(kEchoLen, 0), echoR_.assign(kEchoLen, 0);
    shimL_.assign(kShimLen, 0), shimR_.assign(kShimLen, 0);
    dryL_.assign(kDryLen, 0), dryR_.assign(kDryLen, 0);
    for (auto* v : {&haIn_, &haOla_}) v->assign(2 * kHaloMax, 0);
    for (auto* v : {&haRe_, &haIm_, &haWin_, &haMag_, &haSmear_, &haFrz_, &haFrzPh_, &haTwPh_, &haTwRate_}) v->assign(kHaloMax, 0);
    haOutQ_.assign(kHaloMax, 0);
    haDelay_.assign(2 * kDryLen, 0);
    memL_.assign(kMemLen, 0), memR_.assign(kMemLen, 0);
    ap_.assign(8 * kApLen, 0);
    ecAp_.assign(4 * kEcApLen, 0);
    rsh_.assign(kRShLen, 0);
    fft_.init(kHaloMax);
    testLog_.assign(kTestLog, FiredBreak{});
    page_.assign(PCount, 0), base_.assign(PCount, 0), E_.assign(PCount, 0);
    // warm the model's lazy tables on this (UI) thread
    double mz[8] = {};
    for (int k = 0; k < PCount; ++k) (void)applyMacros(k, 0, mz);
    (void)wobbleCents(mz);
    for (int k = 0; k < kHaloMax; ++k) haTwRate_[static_cast<size_t>(k)] = static_cast<float>(4.0 + 16.0 * h01(77, k, 3));
    reset();
  }

  void reset() override {
    std::fill(catchL_.begin(), catchL_.end(), 0.f), std::fill(catchR_.begin(), catchR_.end(), 0.f);
    clearCrystal(), clearRoom(), clearEcho(), clearShimmer(), clearMemory();
    std::fill(dryL_.begin(), dryL_.end(), 0.f), std::fill(dryR_.begin(), dryR_.end(), 0.f);
    clearHalo();
    for (auto& g : grains_) g.on = false;
    br_ = BreakState{};
    w_ = 0;
    frame_ = 0;
    quiet_ = 0;
    synced_ = false;
    wasPlaying_ = false;
    hits_ = 0;
    gapCount_ = 1 << 30;
    fastEnv_ = slowEnv_ = folEnv_ = duckEnv_ = 0;
    folVal_ = 0;
    sw_[0] = sw_[1] = 0;
    breakSrc_ = 0;
    grng_.seed(1);
    spawnAcc_ = 0;
    gL_.reset(), gR_.reset();
    for (auto& b : toneL_) b.clear();
    for (auto& b : toneR_) b.clear();
    for (auto& b : gritPreL_) b.clear();
    for (auto& b : gritPreR_) b.clear();
    gritPostL_.clear(), gritPostR_.clear();
    gritPh_ = 1, holdL_ = holdR_ = 0, softL_ = softR_ = 0;
    holdOn_ = false;
    haloAmt_ = 0;
    dampOn_ = false, dampL_[0] = dampL_[1] = dampR_[0] = dampR_[1] = 0;
    actEnv_ = 0, hissLpL_ = hissLpR_ = 0, crkL_ = crkR_ = 0;
    ageOn_ = false, ageHpL_ = ageHpR_ = ageX1L_ = ageX1R_ = 0, ageLp_[0] = ageLp_[1] = ageLp_[2] = ageLp_[3] = 0, ageEnv_ = 0;
    wowCents_ = 0, memPrevD_ = -1;
    roomLevel_ = 0;
    smInit_ = false;
    lastCol_ = -1, colPeak_ = 0;
    testCount_ = 0;
  }

  int latencyFor(const PluginState& state, double) const override { return haloLatency(state); }

  static int haloLatency(const PluginState& state) {
    const float v = state.params.size() > static_cast<size_t>(PHaloEngine) ? state.params[PHaloEngine] : static_cast<float>(HaloLow);
    const int e = static_cast<int>(std::lround(std::clamp(v, 0.0f, 2.0f)));
    return e == HaloOff ? 0 : e == HaloLow ? 512 : 1024;
  }

  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override;

  Tel tel;
  const FiredBreak* testLog() const { return testLog_.data(); }
  int testCount() const { return testCount_; }
  bool isIdle() const { return idle_; }

 private:
  // ---- setup
  void setRate(double sr);
  void readParams(const PluginState& s);
  void control(const ProcessContext& ctx);
  // ---- per-sample parts
  void onSlot(int64_t slot);
  void fire(int64_t key, bool hit, double atBeat);
  void breakSample(float inL, float inR, float& mL, float& mR, float& wL, float& wR);
  void hazeSample(float& l, float& r);
  void memorySample(int64_t fi, float inL, float inR, float& l, float& r);
  void noiseSample(int64_t fi, float& l, float& r);
  float readCatch(const std::vector<float>& b, double idx) const {
    const int64_t i = static_cast<int64_t>(std::floor(idx));
    const float t = static_cast<float>(idx - static_cast<double>(i));
    const float* d = b.data();
    return hermite(d[(i - 1) & kCatchMask], d[i & kCatchMask], d[(i + 1) & kCatchMask], d[(i + 2) & kCatchMask], t);
  }
  void spawnGrain(bool real);
  void grainsSample(float& oL, float& oR);
  void shimmerSample(float inL, float inR, float& oL, float& oR);
  void crystalSample(float sL, float sR, float& oL, float& oR);
  void haloSample(float inL, float inR, float& oL, float& oR);
  void haloHop();
  void gritSample(float& l, float& r);
  void roomSample(float inL, float inR, float& oL, float& oR);
  void echoSample(float inL, float inR, float& oL, float& oR);
  void logFire(const FiredBreak& f);
  void clearCrystal() {
    std::fill(combL_.begin(), combL_.end(), 0.f), std::fill(combR_.begin(), combR_.end(), 0.f);
    for (auto& v : combLp_) v = 0;
    combW_ = 0;
    crystalClean_ = true;
  }
  void clearRoom() {
    std::fill(room_.begin(), room_.end(), 0.f);
    std::fill(ap_.begin(), ap_.end(), 0.f);
    std::fill(rsh_.begin(), rsh_.end(), 0.f);
    for (auto& v : roomLp_) v = 0;
    roomW_ = 0, apW_ = 0, rshW_ = 0, rshPh_ = 0, rshPh2_ = 0, rshLp_ = 0, rshFb_ = 0;
    roomClean_ = true;
  }
  void clearEcho() {
    std::fill(echoL_.begin(), echoL_.end(), 0.f), std::fill(echoR_.begin(), echoR_.end(), 0.f);
    std::fill(ecAp_.begin(), ecAp_.end(), 0.f);
    echoW_ = 0, ecApW_ = 0;
    ecLpL_ = ecLpR_ = 0;
    echoClean_ = true;
  }
  void clearShimmer() {
    std::fill(shimL_.begin(), shimL_.end(), 0.f), std::fill(shimR_.begin(), shimR_.end(), 0.f);
    shimW_ = 0, shimPh_ = 0, shFbL_ = shFbR_ = 0, shLpL_ = shLpR_ = 0;
    shimClean_ = true;
  }
  void clearMemory() {
    std::fill(memL_.begin(), memL_.end(), 0.f), std::fill(memR_.begin(), memR_.end(), 0.f);
    memW_ = 0, memPrevD_ = -1, wowCents_ = 0;
    memClean_ = true;
  }
  void clearHalo() {
    std::fill(haIn_.begin(), haIn_.end(), 0.f), std::fill(haOla_.begin(), haOla_.end(), 0.f);
    std::fill(haOutQ_.begin(), haOutQ_.end(), 0.f), std::fill(haDelay_.begin(), haDelay_.end(), 0.f);
    std::fill(haSmear_.begin(), haSmear_.end(), 0.f);
    haCount_ = 0, haDW_ = 0, haFrozen_ = false;
  }
  double P(int k) const { return E_[static_cast<size_t>(k)]; }
  int Pi(int k) const { return static_cast<int>(std::lround(E_[static_cast<size_t>(k)])); }

  const PluginInfo* info_ = nullptr;
  double sr_ = 0;
  // capture
  std::vector<float> catchL_, catchR_;
  int64_t w_ = 0;       // next catch write index
  int64_t frame_ = 0;   // frames processed (control blocks align to it)
  bool holdOn_ = false;
  int holdLen_ = 1;
  // params
  std::vector<double> page_, base_, E_;
  double macro_[8] = {};
  uint32_t seed_ = 1;
  // clock
  double beat_ = 0, dBeat_ = 0, spb_ = 22050, barOrigin_ = 0, barBeats_ = 4, tempo_ = 120;
  bool synced_ = false, wasPlaying_ = false, playing_ = false;
  int64_t lastSlot_ = 0;
  double grid_ = 0.25;
  // breaks
  BreakState br_;
  int64_t hits_ = 0;
  int gapCount_ = 0;
  double fastEnv_ = 0, slowEnv_ = 0, fastA_ = 0, fastR_ = 0, slowA_ = 0, slowR_ = 0;
  // modulation
  double duckEnv_ = 0, folEnv_ = 0, folA_ = 0, folR_ = 0, folVal_ = 0;
  double sw_[2] = {};
  double breakSrc_ = 0;
  // smoothers
  bool smInit_ = false;
  double smA_ = 0;
  double sMix_ = 1, sOut_ = 1, sGr_ = 0, sCr_ = 0, sHa_ = 1, sGt_ = 1, sRm_ = 0, sEc_ = 0, sSh_ = 0, sDuck_ = 0;
  // grains
  Grain grains_[kMaxGrains];
  Rng grng_;
  double spawnAcc_ = 0;
  dsp::DcBlock gL_, gR_;
  // shimmer
  std::vector<float> shimL_, shimR_;
  int shimW_ = 0;
  double shimPh_ = 0, shFbL_ = 0, shFbR_ = 0, shLpL_ = 0, shLpR_ = 0, shLpA_ = 0;
  bool shimClean_ = true;
  // crystal
  std::vector<float> combL_, combR_;
  double combLp_[12] = {};
  int combW_ = 0;
  double combD_[12] = {}, combG_[12] = {}, combDamp_ = 0, combPanL_[6] = {}, combPanR_[6] = {};
  float strikePrevL_ = 0, strikePrevR_ = 0;
  float crystalLevel_[6] = {};
  bool crystalClean_ = true;
  // halo
  Fft fft_;
  std::vector<float> haIn_, haOla_, haOutQ_, haDelay_, haRe_, haIm_, haWin_, haMag_, haSmear_, haFrz_, haFrzPh_, haTwPh_, haTwRate_;
  int haN_ = 512, haHop_ = 128, haCount_ = 0, haDW_ = 0, haEngine_ = HaloLow;
  bool haFrozen_ = false;
  double haloAmt_ = 0;
  // grit
  dsp::Biquad gritPreL_[2], gritPreR_[2], gritPostL_, gritPostR_;
  double gritPh_ = 1, holdL_ = 0, holdR_ = 0, softL_ = 0, softR_ = 0, gritRate_ = -1;
  Rng jrng_;
  // room
  std::vector<float> room_;
  int roomW_ = 0, roomLen_[8] = {};
  double roomG_[8] = {}, roomLp_[8] = {}, roomDamp_ = 0, roomKey_[4] = {-1, -1, -1, -1};
  bool roomClean_ = true;
  // room diffusers (8 allpasses: four per side), modulation, shimmer, freeze
  std::vector<float> ap_;
  int apW_ = 0, apLen_[8] = {};
  double modOff_[8] = {};
  std::vector<float> rsh_;
  int rshW_ = 0;
  double rshPh_ = 0, rshPh2_ = 0, rshLp_ = 0, rshFb_ = 0;
  double sRmFrz_ = 0, sRmShim_ = 0;
  double roomLevel_ = 0;
  // echo diffusers (two per side)
  std::vector<float> ecAp_;
  int ecApW_ = 0, ecApLen_[2] = {};
  // haze
  bool dampOn_ = false;
  double dampA_ = 0, dampL_[2] = {}, dampR_[2] = {};
  // memory
  std::vector<float> memL_, memR_;
  int memW_ = 0;
  bool memClean_ = true;
  double sMem_ = 0, memPrevD_ = -1, wowCents_ = 0;
  bool ageOn_ = false;
  double ageHpA_ = 0, ageLpA_ = 0, ageHpL_ = 0, ageHpR_ = 0, ageX1L_ = 0, ageX1R_ = 0, ageLp_[4] = {}, ageEnv_ = 0, ageEnvA_ = 0, ageEnvR_ = 0;
  double actEnv_ = 0, actR_ = 0, hissLpL_ = 0, hissLpR_ = 0, crkL_ = 0, crkR_ = 0;
  // per-block values of the haze, memory and space stages
  double warm_ = 0, warmMk_ = 1, mWow_ = 0, mFlut_ = 0, mEns_ = 0, mRate_ = 0.5, mAge_ = 0, mHiss_ = 0, mCrk_ = 0, ecDiff_ = 0;
  double rshWin_ = 2048;
  // echo
  std::vector<float> echoL_, echoR_;
  int echoW_ = 0;
  double ecLpL_ = 0, ecLpR_ = 0, ecLpA_ = 0;
  bool echoClean_ = true;
  // tone
  dsp::Biquad toneL_[4], toneR_[4];
  double toneKey_[3] = {-1, -1, -99};
  bool lowOn_ = false, highOn_ = false, tiltOn_ = false;
  // dry latency
  std::vector<float> dryL_, dryR_;
  int dryW_ = 0, latency_ = 0;
  // tape view
  int lastCol_ = -1;
  float colPeak_ = 0;
  // idle
  int64_t quiet_ = 0;
  bool idle_ = false;
  // tests
  std::vector<FiredBreak> testLog_;
  int testCount_ = 0;
  uint32_t evSeq_ = 0;
};

// ------------------------------------------------------------------ setup

void ChaterFx::setRate(double sr) {
  sr_ = sr;
  smA_ = std::exp(-1.0 / (0.010 * sr));
  fastA_ = std::exp(-1.0 / (0.0005 * sr)), fastR_ = std::exp(-1.0 / (0.015 * sr));
  slowA_ = std::exp(-1.0 / (0.030 * sr)), slowR_ = std::exp(-1.0 / (0.250 * sr));
  gL_.setFreq(sr, 8), gR_.setFreq(sr, 8);
  gritRate_ = -1;
  roomKey_[0] = -1;
  toneKey_[0] = -1;
  synced_ = false;
  static const int apBase[8] = {142, 151, 107, 113, 379, 397, 277, 263};
  for (int i = 0; i < 8; ++i) apLen_[i] = std::clamp(static_cast<int>(apBase[i] * sr / 48000.0), 8, kApLen - 2);
  ecApLen_[0] = std::clamp(static_cast<int>(223 * sr / 48000.0), 8, kEcApLen - 2);
  ecApLen_[1] = std::clamp(static_cast<int>(337 * sr / 48000.0), 8, kEcApLen - 2);
  actR_ = std::exp(-1.0 / (1.5 * sr));
  ageEnvA_ = std::exp(-1.0 / (0.005 * sr)), ageEnvR_ = std::exp(-1.0 / (0.15 * sr));
  actR_ = std::exp(-1.0 / (0.6 * sr));
  rshWin_ = std::min(0.06 * sr, kRShLen - 8.0);
}

void ChaterFx::readParams(const PluginState& s) {
  float raw[PCount];
  for (int k = 0; k < PCount; ++k)
    raw[k] = static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : static_cast<float>(info_->params[static_cast<size_t>(k)].def);
  if (s.params.size() >= static_cast<size_t>(PCount)) migrateV1(raw, PCount);  // a state saved before version 2
  for (int k = 0; k < PCount; ++k) page_[static_cast<size_t>(k)] = info_->params[static_cast<size_t>(k)].clamp(raw[k]);
  seed_ = static_cast<uint32_t>(std::lround(page_[PSeed]));
}

// Control values for the next block: sways, follower, links, macros.
void ChaterFx::control(const ProcessContext& ctx) {
  (void)ctx;
  // Sways: their phase follows the clock, so they repeat with the song.
  for (int s = 0; s < 2; ++s) {
    const int shape = static_cast<int>(std::lround(page_[static_cast<size_t>(swayParam(s, SwShape))]));
    const double len = swayBeats(static_cast<int>(std::lround(page_[static_cast<size_t>(swayParam(s, SwRate))])));
    const double ph0 = page_[static_cast<size_t>(swayParam(s, SwPhase))] / 360.0;
    const double depth = page_[static_cast<size_t>(swayParam(s, SwDepth))];
    const double smooth = page_[static_cast<size_t>(swayParam(s, SwSmooth))];
    const double pos = beat_ / len + ph0;
    const double ph = pos - std::floor(pos);
    const auto cyc = static_cast<int64_t>(std::floor(pos));
    double v = 0;
    switch (shape) {
      case ShSine: v = std::sin(2 * kPi * ph); break;
      case ShTri: v = ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4; break;
      case ShRamp: v = ph * 2 - 1; break;
      case ShSteps: {
        const auto step = static_cast<int64_t>(std::floor(pos * 4));
        v = h01(seed_ + 11 + static_cast<uint32_t>(s), step, 5) * 2 - 1;
        break;
      }
      default: {  // Drift: smooth random between cycle points
        const double a = h01(seed_ + 21 + static_cast<uint32_t>(s), cyc, 6) * 2 - 1, b = h01(seed_ + 21 + static_cast<uint32_t>(s), cyc + 1, 6) * 2 - 1;
        const double t = ph * ph * (3 - 2 * ph);
        v = a + (b - a) * t;
        break;
      }
    }
    v *= depth;
    const double a = smooth <= 0 ? 0 : std::exp(-1.0 / (smooth * 0.25 * sr_ / kCB));
    sw_[s] = v + (sw_[s] - v) * a;
  }
  // Follower: the input's loudness over its floor (0..1).
  {
    const double db = 20 * std::log10(std::max(1e-6, folEnv_)) + page_[PFoGain];
    const double fl = page_[PFoFloor];
    folVal_ = std::clamp((db - fl) / -fl, 0.0, 1.0);
  }
  // Breaks as a source: 1 while a break plays (eased).
  breakSrc_ += ((br_.on ? 1.0 : 0.0) - breakSrc_) * 0.25;
  // Links: offsets on the normalised travel of their targets.
  base_ = page_;
  double linkOff[kLinks] = {};
  int linkTarget[kLinks] = {};
  int nl = 0;
  for (int l = 0; l < kLinks; ++l) {
    const int src = static_cast<int>(std::lround(page_[static_cast<size_t>(linkParam(l, LkSource))]));
    const int tgt = static_cast<int>(std::lround(page_[static_cast<size_t>(linkParam(l, LkTarget))]));
    const double depth = page_[static_cast<size_t>(linkParam(l, LkDepth))];
    if (src == SrcOff || depth == 0 || tgt < 0 || tgt >= PCount) continue;
    const ParamDesc& d = info_->params[static_cast<size_t>(tgt)];
    if (d.kind != ParamKind::Continuous || tgt >= PQuality) continue;
    const double v = src == SrcSwayA ? sw_[0] : src == SrcSwayB ? sw_[1] : src == SrcFollower ? folVal_ : breakSrc_;
    linkOff[nl] = v * depth;
    linkTarget[nl++] = tgt;
  }
  for (int i = 0; i < nl; ++i) {
    const ParamDesc& d = info_->params[static_cast<size_t>(linkTarget[i])];
    double& b = base_[static_cast<size_t>(linkTarget[i])];
    b = d.fromNormalized(std::clamp(d.toNormalized(b) + linkOff[i], 0.0, 1.0));
  }
  for (int m = 0; m < 8; ++m) macro_[m] = std::clamp(base_[static_cast<size_t>(m)], 0.0, 1.0);
  for (int k = 0; k < PCount; ++k) E_[static_cast<size_t>(k)] = applyMacros(k, base_[static_cast<size_t>(k)], macro_);
  E_[PMix] = macro_[PMix];
}

// ------------------------------------------------------------------ breaks

void ChaterFx::onSlot(int64_t slot) {
  const int mode = Pi(PTrigMode);
  if (mode == TrigHits) return;
  if (br_.on && br_.len - br_.el > 2) return;  // breaks never overlap (one ending on this step gives way)
  double chance = std::clamp(P(PDensity), 0.0, 1.0);
  if (chance <= 0) return;
  // Lean: where in the bar the chance sits
  const double at = static_cast<double>(slot) * grid_;
  double inBar = std::fmod(at - barOrigin_, barBeats_);
  if (inBar < 0) inBar += barBeats_;
  const double inBeat = inBar - std::floor(inBar + 1e-9);
  const bool onBeat = inBeat < 1e-6 || inBeat > 1 - 1e-6;
  switch (Pi(PLean)) {
    case LeanDown: chance *= onBeat ? 1.6 : 0.5; break;
    case LeanOff: chance *= onBeat ? 0.25 : 1.6; break;
    case LeanBarEnd: {
      const double f = inBar / barBeats_;
      chance *= 0.25 + 2.0 * f * f;
      break;
    }
    default: break;
  }
  chance = std::min(1.0, chance);
  if (h01(seed_, slot, 1) >= chance) return;
  fire(slot, false, at);
}

void ChaterFx::fire(int64_t key, bool hit, double atBeat) {
  const uint32_t sd = seed_ + (hit ? 0x5bd1e995U : 0u);
  // which break
  double odds[6];
  double sum = 0;
  for (int i = 0; i < 6; ++i) odds[i] = std::max(0.0, P(POddRepeat + i)), sum += odds[i];
  if (sum <= 0) return;
  const double variation = std::clamp(P(PVariation), 0.0, 1.0);
  double r = h01(sd, key, 2) * sum;
  int type = BrRepeat;
  for (int i = 0; i < 6; ++i) {
    if (r < odds[i]) {
      type = BrRepeat + i;
      break;
    }
    r -= odds[i];
  }
  // Variation lets the type wander to any break with a chance
  if (h01(sd, key, 3) < variation * 0.35) {
    double r2 = h01(sd, key, 4) * sum;
    for (int i = 0; i < 6; ++i) {
      if (r2 < odds[i] || i == 5) {
        if (odds[i] > 0) type = BrRepeat + i;
        break;
      }
      r2 -= odds[i];
    }
  }
  BreakState b;
  b.on = true;
  b.type = type;
  b.start = w_ - 1;  // the sample just written
  double beats = spanSteps(Pi(PSpan)) * grid_;
  int division = Pi(PDivision);
  double ramp = P(PRamp);
  // Variation: the length and shape wander
  if (h01(sd, key, 5) < variation) {
    const double m = h01(sd, key, 6);
    if (m < 0.3) beats *= 2;
    else if (m < 0.5) beats = std::max(grid_, beats * 0.5);
    else if (m < 0.7) division = std::clamp(division + (h01(sd, key, 7) < 0.5 ? 1 : -1), 0, kDivisionStraight - 1);
    else if (m < 0.85) ramp = -ramp;
    else b.rise = true;
  }
  // a grid break ends exactly on the step it reaches; a hit's lasts its span from here
  b.len = hit ? static_cast<int>(std::lround(beats * spb_)) : static_cast<int>(std::ceil((atBeat + beats - beat_) * spb_ - 1e-6));
  b.len = std::max(64, b.len);
  b.soft = std::clamp(P(PSoftness), 0.0, 1.0);
  b.wash = std::clamp(P(PWash), 0.0, 1.0);
  // soft edges: the crossfade grows from the Catch smooth time up to 60 ms
  b.fade = std::max(1, static_cast<int>(std::lround(std::max(P(PSmooth) * 0.001, b.soft * 0.06) * sr_)));
  b.fade = std::min(b.fade, b.len / 2);
  const int64_t lenCap = std::min<int64_t>(static_cast<int64_t>(lengthBars(Pi(PLength)) * barBeats_ * spb_),
                                           static_cast<int64_t>(captureBars(Pi(PCaptureUpTo)) * barBeats_ * spb_));
  const int64_t capMax = std::max<int64_t>(64, std::min<int64_t>(lenCap, kCatch - 8 * static_cast<int64_t>(sr_) / 4 - 64));
  b.len = static_cast<int>(std::min<int64_t>(b.len, capMax));
  switch (type) {
    case BrRepeat:
      b.slice = std::max(32.0, divisionBeats(division) * spb_);
      b.shrink = std::clamp(1.0 - P(PAccel) * 0.5, 0.3, 1.7);
      b.ramp = ramp;
      b.gate = std::clamp(P(PGate), 0.1, 1.0);
      break;
    case BrTape:
      b.tapeLen = std::max(64.0, std::min(static_cast<double>(b.len), tapeBeats(Pi(PTapeTime)) * spb_));
      if (b.rise) b.tapeLen = std::max(64.0, b.len * 0.5);
      b.curve = std::clamp(P(PTapeCurve), 0.0, 1.0);
      break;
    case BrScrub:
      b.depth = std::clamp(P(PScrubDepth), 0.0, 1.0) * static_cast<double>(std::min<int64_t>(b.len, capMax));
      b.period = std::max(64.0, scrubBeats(Pi(PScrubRate)) * spb_);
      break;
    case BrJump: {
      const int64_t q = std::max<int64_t>(16, static_cast<int64_t>(quantiseBeats(Pi(PQuantise)) * spb_));
      // never further back than the capture holds
      const int64_t steps = std::max<int64_t>(1, std::min<int64_t>(capMax, w_ - 1) / q);
      b.offset = q * (1 + static_cast<int64_t>(h01(sd, key, 8) * static_cast<double>(steps)) % steps);
      break;
    }
    default: break;
  }
  const double ps = std::clamp(P(PPanScatter), 0.0, 1.0);
  if (ps > 0) {
    const double pan = (h01(sd, key, 9) * 2 - 1) * ps * 0.7;
    b.panL = static_cast<float>(std::sqrt(2.0) * std::cos((pan + 1) * kPi / 4));
    b.panR = static_cast<float>(std::sqrt(2.0) * std::sin((pan + 1) * kPi / 4));
  }
  br_ = b;
  FiredBreak f;
  f.beat = atBeat;
  f.beats = b.len / spb_;
  f.type = type;
  f.repeats = type == BrRepeat ? std::max(1, static_cast<int>(std::lround(b.len / b.slice))) : 0;
  f.hit = hit;
  f.frame = w_ - 1;
  logFire(f);
}

void ChaterFx::logFire(const FiredBreak& f) {
  FiredBreak& t = testLog_[static_cast<size_t>(testCount_ % kTestLog)];
  t = f;
  t.seq = static_cast<uint32_t>(testCount_ + 1);
  ++testCount_;
  auto& e = tel.ev[evSeq_ % kEventRing];
  ++evSeq_;
  e.seq.store(0, std::memory_order_release);
  e.beat.store(f.beat, std::memory_order_relaxed);
  e.beats.store(f.beats, std::memory_order_relaxed);
  e.type.store(f.type, std::memory_order_relaxed);
  e.repeats.store(f.repeats, std::memory_order_relaxed);
  e.hit.store(f.hit ? 1 : 0, std::memory_order_relaxed);
  e.seq.store(evSeq_, std::memory_order_release);
  tel.evCount.store(evSeq_, std::memory_order_release);
}

// The main signal for one sample: the live input, or the break, crossfaded at its edges.
void ChaterFx::breakSample(float inL, float inR, float& mL, float& mR, float& wL, float& wR) {
  wL = wR = 0;
  if (!br_.on) {
    mL = inL, mR = inR;
    return;
  }
  BreakState& b = br_;
  float bl = 0, bR = 0;
  const int el = b.el;
  double fc = 20000, softGain = 1;  // the break's own low-pass and level (soft edges)
  switch (b.type) {
    case BrRepeat: {
      double env = 1;
      double edge = std::max(1.0, std::min(b.fade * 0.5, b.slice * 0.25));
      if (b.soft > 0) {
        edge = std::max(edge, b.soft * b.slice * 0.3);                    // rounded slices
        softGain = std::pow(1.0 - b.soft * 0.15, b.rep);                  // each repeat a little quieter
        fc = 18000.0 * std::pow(1.0 - 0.75 * b.soft, 1.0 + 0.5 * b.rep);  // and darker
      }
      if (b.rep == 0) {
        bl = inL, bR = inR;  // the first slice plays live while it is caught
        env = std::min(1.0, (b.slice - b.slicePos) / edge);
      } else {
        const double idx = static_cast<double>(b.start) + b.slicePos;
        bl = readCatch(catchL_, idx), bR = readCatch(catchR_, idx);
        env = std::min({1.0, b.slicePos / edge, (b.slice - b.slicePos) / edge});
        const double g = b.gate * b.slice;
        if (b.gate < 0.999) env *= std::clamp((g - b.slicePos) / edge, 0.0, 1.0);
      }
      env = std::max(0.0, env);
      bl *= static_cast<float>(env), bR *= static_cast<float>(env);
      const double speed = b.rep == 0 ? 1.0 : std::exp2(b.ramp * (static_cast<double>(el) / b.len) / 12.0);
      b.slicePos += speed;
      if (b.slicePos >= b.slice) {
        b.slicePos -= b.slice;
        if (b.rep == 0) b.slicePos = 0;
        ++b.rep;
        b.slice = std::max(16.0, b.slice * b.shrink);
        if (b.slicePos >= b.slice) b.slicePos = 0;
      }
      break;
    }
    case BrReverse: {
      const int64_t idx = b.start - 1 - el;
      bl = catchL_[static_cast<size_t>(idx & kCatchMask)], bR = catchR_[static_cast<size_t>(idx & kCatchMask)];
      if (b.soft > 0) {
        softGain = std::pow((el + 1.0) / b.len, b.soft * 1.5);  // a reverse swell
        fc = 18000.0 * (1.0 - 0.6 * b.soft);
      }
      break;
    }
    case BrTape: {
      const double u = el / b.tapeLen;
      double v;
      if (u < 1) {
        const double lin = 1 - u, ex = lin * lin * lin;
        v = lin + (ex - lin) * b.curve;
      } else if (b.rise) {
        const double u2 = std::min(1.0, (el - b.tapeLen) / std::max(1.0, b.len - b.tapeLen));
        v = u2 * u2;
      } else {
        v = 0;
      }
      if (u < 1 || b.rise) {
        // two samples behind the write point, so the interpolation never reads ahead of the input
        const double idx = static_cast<double>(b.start) - 2 + b.pos;
        bl = readCatch(catchL_, idx), bR = readCatch(catchR_, idx);
        if (u >= 1 && !b.rise) bl = bR = 0;
      }
      b.pos += v;
      if (b.soft > 0) fc = 300.0 + 17700.0 * (1.0 - 0.7 * b.soft) * std::clamp(v, 0.0, 1.0);  // the tape darkens as it slows
      break;
    }
    case BrScrub: {
      const double idx = static_cast<double>(b.start) - b.depth * 0.5 + b.depth * 0.5 * std::sin(2 * kPi * el / b.period);
      bl = readCatch(catchL_, idx), bR = readCatch(catchR_, idx);
      break;
    }
    case BrJump: {
      const int64_t idx = b.start - b.offset + el;
      bl = catchL_[static_cast<size_t>(idx & kCatchMask)], bR = catchR_[static_cast<size_t>(idx & kCatchMask)];
      break;
    }
    default:  // Skip
      bl = bR = 0;
      if (b.soft > 0) fc = 18000.0 * (1.0 - 0.75 * b.soft);
      break;
  }
  if (b.soft > 0) {
    if (el == 0) b.lpL = inL, b.lpR = inR;
    const double a = std::clamp(2 * kPi * std::min(fc, sr_ * 0.45) / sr_, 0.0, 1.0);
    b.lpL += (bl * softGain - b.lpL) * a, b.lpR += (bR * softGain - b.lpR) * a;
    bl = static_cast<float>(b.lpL), bR = static_cast<float>(b.lpR);
  }
  bl *= b.panL, bR *= b.panR;
  const float g = static_cast<float>(std::min({1.0, (el + 1.0) / b.fade, static_cast<double>(b.len - el) / b.fade}));
  // Wash: the break replaces less of the input and goes into the space instead
  const float into = g * static_cast<float>(1.0 - 0.5 * b.wash);
  mL = inL + (bl - inL) * into;
  mR = inR + (bR - inR) * into;
  wL = bl * g * static_cast<float>(b.wash), wR = bR * g * static_cast<float>(b.wash);
  if (++b.el >= b.len) b.on = false;
}

// ------------------------------------------------------------------ glitter

void ChaterFx::spawnGrain(bool real) {
  // Every random draw happens whether or not the grain is used, so the sequence stays the
  // same through silent stretches (renders repeat at any block size).
  const double rPick = grng_.next(), rDet = grng_.next(), rRev = grng_.next(), rScat = grng_.next(), rPan = grng_.next(), rMd = grng_.next();
  if (!real) return;
  const int quality = Pi(PQuality);
  const int cap = quality == QLight ? 24 : quality == QNormal ? 48 : kMaxGrains;
  Grain* g = nullptr;
  for (int i = 0; i < cap; ++i)
    if (!grains_[i].on) {
      g = &grains_[i];
      break;
    }
  if (!g) return;
  // pitch: the lit chips, weighted toward the low ones unless Reach is high
  const int mask = static_cast<int>(std::lround(P(PIntervals)));
  const double reach = std::clamp(P(PReach), 0.0, 1.0);
  double wsum = 0, wts[kChips] = {};
  int lit = 0;
  for (int i = 0; i < kChips; ++i)
    if (mask & (1 << i)) wts[i] = std::pow(reach, lit++), wsum += wts[i];
  double semis = 0;
  if (wsum > 0) {
    double r = rPick * wsum;
    for (int i = 0; i < kChips; ++i) {
      if (wts[i] <= 0) continue;
      if (r < wts[i]) {
        semis = kChipSemis[static_cast<size_t>(i)];
        break;
      }
      r -= wts[i];
      semis = kChipSemis[static_cast<size_t>(i)];
    }
  }
  semis += (rDet * 2 - 1) * P(PDetune) / 100.0 + sw_[0] * wobbleCents(macro_) / 100.0;
  const int scale = Pi(PScale);
  if (scale > 0) {
    const int sm = scaleMask(scale);
    double best = semis, bestD = 1e9;
    for (int o = -3; o <= 4; ++o)
      for (int d = 0; d < 12; ++d)
        if (sm & (1 << d)) {
          const double c = o * 12 + d;
          if (std::fabs(c - semis) < bestD) bestD = std::fabs(c - semis), best = c;
        }
    semis += (best - semis) * std::clamp(P(PSnap), 0.0, 1.0);
  }
  const double ratio = std::exp2(semis / 12.0);
  const double size = std::clamp(P(PGrSize), 0.003, 0.3);
  const int len = std::max(16, static_cast<int>(size * sr_));
  const bool rev = rRev < P(PGrReverse);
  const double scatter = std::clamp(P(PGrScatter), 0.0, 1.0) * rScat * 0.5 * sr_;
  double start;
  if (rev) start = static_cast<double>(w_) - 4 - scatter;
  else start = static_cast<double>(w_) - 4 - scatter - std::max(0.0, ratio - 1.0) * len;
  g->on = true;
  g->pos = start;
  g->step = rev ? -ratio : ratio;
  g->len = len, g->age = 0;
  g->win = Pi(PGrWindow);
  const double pan = (rPan * 2 - 1) * std::clamp(P(PPanScatter), 0.0, 1.0);
  g->gl = static_cast<float>(std::cos((pan + 1) * kPi / 4) * std::sqrt(2.0));
  g->gr = static_cast<float>(std::sin((pan + 1) * kPi / 4) * std::sqrt(2.0));
  g->md = static_cast<int>(rMd * P(PMicroDelay) * 0.001 * sr_);
  g->semis = static_cast<float>(semis);
  g->pan = static_cast<float>(pan);
}

void ChaterFx::grainsSample(float& oL, float& oR) {
  float sl = 0, sr = 0;
  for (auto& g : grains_) {
    if (!g.on) continue;
    const double t = static_cast<double>(g.age) / g.len;
    double w;
    switch (g.win) {
      case WinSoft: w = 0.5 - 0.5 * std::cos(2 * kPi * t); break;
      case WinSharp: w = t < 0.08 ? t / 0.08 : std::exp(-(t - 0.08) * 5.0) * (1 - t) / 0.92 * 1.2; break;
      case WinClick: w = std::min({1.0, t * g.len / (0.001 * sr_), (1 - t) * g.len / (0.001 * sr_)}); break;
      case WinSwell: {  // a slow rise, a quicker fall: grains that fade in like a breath
        const double u = t < 0.75 ? std::sin(0.5 * kPi * t / 0.75) : std::cos(0.5 * kPi * (t - 0.75) / 0.25);
        w = u * u;
        break;
      }
      default: {
        const double x = (t - 0.5) / 0.2;
        w = std::exp(-0.5 * x * x) * std::sin(kPi * t);
        break;
      }
    }
    const float a = readCatch(catchL_, g.pos), b = readCatch(catchR_, g.pos - g.md);
    sl += static_cast<float>(w) * a * g.gl;
    sr += static_cast<float>(w) * b * g.gr;
    g.pos += g.step;
    if (++g.age >= g.len) g.on = false;
  }
  oL = sl, oR = sr;
}

// Pitched feedback: a two-tap crossfading delay-line shifter in the loop.
void ChaterFx::shimmerSample(float inL, float inR, float& oL, float& oR) {
  const double ratio = std::exp2(shimmerSemis(Pi(PShInterval)) / 12.0);
  const double win = 0.06 * sr_;
  const double spread = std::clamp(P(PShSpread), 0.0, 1.0);
  const double fbL = shFbL_ * (1 - spread * 0.5) + shFbR_ * spread * 0.5, fbR = shFbR_ * (1 - spread * 0.5) + shFbL_ * spread * 0.5;
  shimL_[static_cast<size_t>(shimW_)] = flush(static_cast<float>(inL + fbL));
  shimR_[static_cast<size_t>(shimW_)] = flush(static_cast<float>(inR + fbR));
  shimPh_ += (1.0 - ratio) / win;
  shimPh_ -= std::floor(shimPh_);
  auto tap = [&](const std::vector<float>& b, double d) {
    d = std::clamp(d, 2.0, static_cast<double>(kShimLen - 4));
    const double idx = shimW_ - d;
    const int i = static_cast<int>(std::floor(idx));
    const float t = static_cast<float>(idx - i);
    const float* p = b.data();
    return hermite(p[(i - 1) & (kShimLen - 1)], p[i & (kShimLen - 1)], p[(i + 1) & (kShimLen - 1)], p[(i + 2) & (kShimLen - 1)], t);
  };
  const double p1 = shimPh_, p2 = std::fmod(shimPh_ + 0.5, 1.0);
  const float g1 = static_cast<float>(std::sin(kPi * p1)), g2 = static_cast<float>(std::sin(kPi * p2));
  const float yl = tap(shimL_, 4 + p1 * win) * g1 + tap(shimL_, 4 + p2 * win) * g2;
  const float yr = tap(shimR_, 4 + p1 * win) * g1 + tap(shimR_, 4 + p2 * win) * g2;
  shimW_ = (shimW_ + 1) & (kShimLen - 1);
  shLpL_ = yl + (shLpL_ - yl) * shLpA_;
  shLpR_ = yr + (shLpR_ - yr) * shLpA_;
  shFbL_ = std::tanh(shLpL_ * sSh_);
  shFbR_ = std::tanh(shLpR_ * sSh_);
  oL = static_cast<float>(shLpL_), oR = static_cast<float>(shLpR_);
}

// Six tuned combs per side, struck by the main signal's transients and the grains.
void ChaterFx::crystalSample(float sL, float sR, float& oL, float& oR) {
  const float strike = static_cast<float>(std::clamp(P(PCrStrike), 0.0, 1.0));
  const float dl = sL - strikePrevL_, dr = sR - strikePrevR_;
  strikePrevL_ = sL, strikePrevR_ = sR;
  const float inL = (dl * 1.5f + sL * 0.15f) * strike * 0.12f, inR = (dr * 1.5f + sR * 0.15f) * strike * 0.12f;
  float yl = 0, yr = 0;
  for (int v = 0; v < 6; ++v) {
    for (int side = 0; side < 2; ++side) {
      const int c = v * 2 + side;
      std::vector<float>& buf = side ? combR_ : combL_;
      float* line = buf.data() + v * kCombLen;
      const double idx = combW_ - combD_[c];
      const int i = static_cast<int>(std::floor(idx));
      const float t = static_cast<float>(idx - i);
      const float d = line[i & (kCombLen - 1)] * (1 - t) + line[(i + 1) & (kCombLen - 1)] * t;
      combLp_[c] = d + (combLp_[c] - d) * combDamp_;
      const float y = flush(static_cast<float>((side ? inR : inL) + combLp_[c] * combG_[c]));
      line[combW_] = y;
      if (side) yr += y * static_cast<float>(combPanR_[v]);
      else yl += y * static_cast<float>(combPanL_[v]);
      if (!side) crystalLevel_[v] = std::max(crystalLevel_[v] * 0.9995f, std::fabs(y));
    }
  }
  combW_ = (combW_ + 1) & (kCombLen - 1);
  oL = yl * 0.4f, oR = yr * 0.4f;
}

// ------------------------------------------------------------------ halo (STFT, latency haN_)

void ChaterFx::haloHop() {
  const int N = haN_;
  float* re = haRe_.data();
  float* im = haIm_.data();
  for (int i = 0; i < N; ++i) re[i] = haIn_[static_cast<size_t>(i)] * haWin_[static_cast<size_t>(i)], im[i] = 0;
  fft_.run(re, im, N, false);
  const int half = N / 2;
  const int k0 = std::clamp(static_cast<int>(P(PHaFrom) / sr_ * N), 1, half);
  const double lift = std::clamp(P(PHaLift), 0.0, 1.0), twinkle = std::clamp(P(PHaTwinkle), 0.0, 1.0), smear = std::clamp(P(PHaSmear), 0.0, 1.0);
  const bool freeze = P(PHaFreeze) >= 0.5;
  for (int k = std::max(0, k0 - 2); k <= half; ++k) haMag_[static_cast<size_t>(k)] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
  if (freeze && !haFrozen_) {
    for (int k = k0; k <= half; ++k) haFrz_[static_cast<size_t>(k)] = haMag_[static_cast<size_t>(k)], haFrzPh_[static_cast<size_t>(k)] = std::atan2(im[k], re[k]);
    haFrozen_ = true;
  } else if (!freeze) {
    haFrozen_ = false;
  }
  const float tdt = static_cast<float>(2 * kPi * haHop_ / sr_);
  const float smearA = static_cast<float>(smear * 0.92);
  for (int k = k0; k <= half; ++k) {
    const size_t K = static_cast<size_t>(k);
    const float m = haMag_[K];
    const float mPrev = haMag_[static_cast<size_t>(std::max(0, k - 1))], mNext = k < half ? haMag_[static_cast<size_t>(k + 1)] : 0;
    const bool peak = m > mPrev && m >= mNext;
    const bool near = !peak && (mPrev > haMag_[static_cast<size_t>(std::max(0, k - 2))] || mNext > (k + 2 <= half ? haMag_[static_cast<size_t>(k + 2)] : 0));
    double gain = 1;
    if (lift > 0) gain *= peak ? 1 + lift * 2 : near ? 1 + lift * 1.2 : 1 + lift * 0.25;
    haTwPh_[K] += tdt * haTwRate_[K];
    if (haTwPh_[K] > 6.2831853f) haTwPh_[K] -= 6.2831853f;
    if (twinkle > 0) gain *= std::pow(10.0, twinkle * (peak ? 9.0 : 4.0) * std::sin(haTwPh_[K]) / 20.0);
    float target = m;
    haSmear_[K] = target + (haSmear_[K] - target) * smearA;
    float mag = smear > 0 ? haSmear_[K] : m;
    if (freeze) {
      haFrzPh_[K] += static_cast<float>(2 * kPi * k * haHop_ / N);
      if (haFrzPh_[K] > 6.2831853f) haFrzPh_[K] = std::fmod(haFrzPh_[K], 6.2831853f);
      const float fm = haFrz_[K] * static_cast<float>(gain);
      re[k] = fm * std::cos(haFrzPh_[K]), im[k] = fm * std::sin(haFrzPh_[K]);
    } else {
      const float s = m > 1e-12f ? static_cast<float>(gain) * mag / m : 0.0f;
      re[k] *= s, im[k] *= s;
    }
  }
  // mirror for the inverse transform of a real signal
  for (int k = 1; k < half; ++k) re[N - k] = re[k], im[N - k] = -im[k];
  fft_.run(re, im, N, true);
  const float norm = 1.0f / (static_cast<float>(N) * 1.5f);
  for (int i = 0; i < N; ++i) haOla_[static_cast<size_t>(i)] += re[i] * haWin_[static_cast<size_t>(i)] * norm;
}

// haIn_ and haOla_ are shared by both channels as interleaved halves: L in [0, N), R in [N, 2N).
void ChaterFx::haloSample(float inL, float inR, float& oL, float& oR) {
  const int N = haN_, hop = haHop_;
  // delayed input (the reference the halo's change is added to)
  haDelay_[static_cast<size_t>(haDW_)] = inL;
  haDelay_[static_cast<size_t>(kDryLen + haDW_)] = inR;
  const int rd = (haDW_ - N + kDryLen) & (kDryLen - 1);
  const float dL = haDelay_[static_cast<size_t>(rd)], dR = haDelay_[static_cast<size_t>(kDryLen + rd)];
  haDW_ = (haDW_ + 1) & (kDryLen - 1);
  oL = dL, oR = dR;
  if (haloAmt_ < 1e-6) {
    haCount_ = (haCount_ + 1) % hop;
    return;
  }
  // processed (mono mid, the halo is a lift on top): out = d + amt * (y - d_mid)
  const float x = 0.5f * (inL + inR);
  haIn_[static_cast<size_t>(N - hop + haCount_)] = x;
  const float y = haOutQ_[static_cast<size_t>(haCount_)];
  const float dm = 0.5f * (dL + dR);
  const float add = static_cast<float>(haloAmt_) * (y - dm);
  oL = dL + add, oR = dR + add;
  if (++haCount_ >= hop) {
    haCount_ = 0;
    haloHop();
    for (int i = 0; i < hop; ++i) haOutQ_[static_cast<size_t>(i)] = haOla_[static_cast<size_t>(i)];
    std::memmove(haOla_.data(), haOla_.data() + hop, sizeof(float) * static_cast<size_t>(N - hop));
    std::fill(haOla_.begin() + (N - hop), haOla_.begin() + N, 0.0f);
    std::memmove(haIn_.data(), haIn_.data() + hop, sizeof(float) * static_cast<size_t>(N - hop));
  }
}

// ------------------------------------------------------------------ grit

void ChaterFx::gritSample(float& l, float& r) {
  const double bits = P(PGtBits), rate = P(PGtRate), drive = P(PGtDrive), jitter = P(PGtJitter);
  const int smooth = Pi(PGtSmooth);
  double xl = l, xr = r;
  if (drive > 0.01) {
    const double g = dsp::dbToGain(drive);
    const double nrm = std::pow(g, 0.6);
    xl = std::tanh(xl * g) / nrm, xr = std::tanh(xr * g) / nrm;
  }
  const bool down = rate < sr_ * 0.98;
  if (down) {
    if (smooth == GsClean)
      for (int i = 0; i < 2; ++i) xl = gritPreL_[i].process(xl), xr = gritPreR_[i].process(xr);
    double inc = rate / sr_;
    if (jitter > 0) inc *= 1.0 + (jrng_.next() * 2 - 1) * jitter * 0.9;
    gritPh_ += inc;
    if (gritPh_ >= 1) {
      gritPh_ -= std::floor(gritPh_);
      holdL_ = xl, holdR_ = xr;
    }
    xl = holdL_, xr = holdR_;
  }
  if (bits < 23.9) {
    const double q = std::exp2(bits - 1);
    xl = std::round(xl * q) / q, xr = std::round(xr * q) / q;
  }
  if (down) {
    if (smooth == GsSoft) {
      const double a = std::exp(-2 * kPi * rate * 0.5 / sr_);
      softL_ = xl + (softL_ - xl) * a, softR_ = xr + (softR_ - xr) * a;
      xl = softL_, xr = softR_;
    } else if (smooth == GsClean) {
      xl = gritPostL_.process(xl), xr = gritPostR_.process(xr);
    }
  }
  const float m = static_cast<float>(sGt_);
  l = l + (static_cast<float>(xl) - l) * m;
  r = r + (static_cast<float>(xr) - r) * m;
}

// ------------------------------------------------------------------ room and echo

void ChaterFx::roomSample(float inL, float inR, float& oL, float& oR) {
  const double frz = sRmFrz_, keep = 1.0 - frz;
  // Freeze: no new input, lossless lines, no damping and no modulation
  float xl = inL * static_cast<float>(keep), xr = inR * static_cast<float>(keep);
  if (rshFb_ != 0) {
    const float sh = static_cast<float>(rshFb_ * keep);
    xl += sh, xr += sh;
  }
  // input diffusion: four allpasses per side smear the attacks into a cloud
  constexpr int am = kApLen - 1;
  for (int k = 0; k < 4; ++k) {
    xl = allpass(ap_.data() + (2 * k) * kApLen, am, apW_, apLen_[2 * k], xl, 0.62f);
    xr = allpass(ap_.data() + (2 * k + 1) * kApLen, am, apW_, apLen_[2 * k + 1], xr, 0.62f);
  }
  apW_ = (apW_ + 1) & am;
  float y[8];
  const double dampA = roomDamp_ * keep;
  for (int i = 0; i < 8; ++i) {
    const float* line = room_.data() + i * kRoomLen;
    const double rd = roomLen_[i] + modOff_[i] * keep;
    const int ri = static_cast<int>(rd);
    const float fr = static_cast<float>(rd - ri);
    const float a = line[(roomW_ - ri) & (kRoomLen - 1)], b = line[(roomW_ - ri - 1) & (kRoomLen - 1)];
    const float d = fr == 0.0f ? a : a + (b - a) * fr;
    roomLp_[i] = flushd(d + (roomLp_[i] - d) * dampA);
    y[i] = static_cast<float>(roomLp_[i]);
  }
  oL = (y[0] + y[2] + y[4] + y[6]) * 0.5f;
  oR = (y[1] + y[3] + y[5] + y[7]) * 0.5f;
  // Hadamard mix (fast, orthonormal)
  float h[8];
  for (int i = 0; i < 8; ++i) h[i] = y[i];
  for (int len = 1; len < 8; len <<= 1)
    for (int i = 0; i < 8; i += len * 2)
      for (int j = i; j < i + len; ++j) {
        const float a = h[j], b = h[j + len];
        h[j] = a + b, h[j + len] = a - b;
      }
  const float sc = 0.35355339f;
  for (int i = 0; i < 8; ++i) {
    const float in = (i & 1) ? xr : xl;
    // shimmer feeds energy back in: the lines give a little up for it, so the tail never grows
    const double g = roomG_[i] * (1.0 - 0.2 * sRmShim_) + (1.0 - roomG_[i] * (1.0 - 0.2 * sRmShim_)) * frz;
    room_[static_cast<size_t>(i * kRoomLen + roomW_)] = flush(h[i] * sc * static_cast<float>(g) + in * 0.3f);
  }
  roomW_ = (roomW_ + 1) & (kRoomLen - 1);
  // shimmer: the tail pitched an octave and a fifth up, back into the room
  if (sRmShim_ > 1e-6) {
    constexpr int sm = kRShLen - 1;
    rsh_[static_cast<size_t>(rshW_)] = flush(0.5f * (oL + oR));
    const double W = rshWin_;
    auto tap = [&](double ph) {
      const double d = 2.0 + W * (1.0 - ph);
      const int di = static_cast<int>(d);
      const double f = d - di;
      const float a = rsh_[static_cast<size_t>((rshW_ - di) & sm)], b = rsh_[static_cast<size_t>((rshW_ - di - 1) & sm)];
      const double w = std::sin(kPi * ph);
      return (a + (b - a) * f) * w * w;
    };
    auto frac = [](double v) { return v - std::floor(v); };
    const double oct = tap(rshPh_) + tap(frac(rshPh_ + 0.5));
    const double fifth = tap(rshPh2_) + tap(frac(rshPh2_ + 0.5));
    rshPh_ = frac(rshPh_ + 1.0 / W), rshPh2_ = frac(rshPh2_ + 0.5 / W);
    const double sh = 0.7 * oct + 0.3 * fifth;
    rshLp_ = flushd(rshLp_ + (sh - rshLp_) * 0.45);
    rshFb_ = std::tanh(rshLp_ * sRmShim_ * 0.4);
    rshW_ = (rshW_ + 1) & sm;
  } else {
    rshFb_ = 0;
  }
  const double lv = std::max(std::fabs(oL), std::fabs(oR));
  roomLevel_ = lv > roomLevel_ ? lv : roomLevel_ * 0.9997;
}

void ChaterFx::echoSample(float inL, float inR, float& oL, float& oR) {
  const double t = std::clamp(echoBeats(Pi(PEcTime)) * spb_, 16.0, static_cast<double>(kEchoLen - 8));
  const int ti = static_cast<int>(t);
  const float dl = echoL_[static_cast<size_t>((echoW_ - ti) & (kEchoLen - 1))], dr = echoR_[static_cast<size_t>((echoW_ - ti) & (kEchoLen - 1))];
  ecLpL_ = dl + (ecLpL_ - dl) * ecLpA_;
  ecLpR_ = dr + (ecLpR_ - dr) * ecLpA_;
  const double fb = std::clamp(P(PEcFeedback), 0.0, 0.95);
  float fl = static_cast<float>(std::tanh(ecLpL_ * fb)), fr = static_cast<float>(std::tanh(ecLpR_ * fb));
  if (ecDiff_ > 0.01) {  // Diffuse: each repeat a little more blurred than the last
    constexpr int em = kEcApLen - 1;
    const float g = static_cast<float>(ecDiff_ * 0.65);
    fl = allpass(ecAp_.data(), em, ecApW_, ecApLen_[0], fl, g);
    fl = allpass(ecAp_.data() + kEcApLen, em, ecApW_, ecApLen_[1], fl, g);
    fr = allpass(ecAp_.data() + 2 * kEcApLen, em, ecApW_, ecApLen_[1], fr, g);
    fr = allpass(ecAp_.data() + 3 * kEcApLen, em, ecApW_, ecApLen_[0], fr, g);
    ecApW_ = (ecApW_ + 1) & em;
  }
  float wl, wr;
  if (P(PEcPingPong) >= 0.5) {
    wl = 0.5f * (inL + inR) + fr;
    wr = fl;
  } else {
    wl = inL + fl;
    wr = inR + fr;
  }
  echoL_[static_cast<size_t>(echoW_)] = flush(wl);
  echoR_[static_cast<size_t>(echoW_)] = flush(wr);
  echoW_ = (echoW_ + 1) & (kEchoLen - 1);
  oL = dl, oR = dr;
}

// ------------------------------------------------------------------ haze and memory

void ChaterFx::hazeSample(float& l, float& r) {
  if (warm_ > 0.001) {  // soft saturation, level-matched around -12 dBFS
    const double g = 1.0 + 3.0 * warm_;
    l = static_cast<float>(std::tanh(g * l) / g * warmMk_), r = static_cast<float>(std::tanh(g * r) / g * warmMk_);
  }
  if (dampOn_) {  // two one-poles: a 12 dB per octave high cut
    dampL_[0] = flushd(dampL_[0] + (l - dampL_[0]) * dampA_), dampL_[1] = flushd(dampL_[1] + (dampL_[0] - dampL_[1]) * dampA_);
    dampR_[0] = flushd(dampR_[0] + (r - dampR_[0]) * dampA_), dampR_[1] = flushd(dampR_[1] + (dampR_[0] - dampR_[1]) * dampA_);
    l = static_cast<float>(dampL_[1]), r = static_cast<float>(dampR_[1]);
  }
}

namespace {
inline float memRead(const std::vector<float>& b, int w, double d) {
  constexpr int m = kMemLen - 1;
  const double pos = w - d;
  const int i = static_cast<int>(std::floor(pos));
  const float t = static_cast<float>(pos - i);
  return hermite(b[static_cast<size_t>((i - 1) & m)], b[static_cast<size_t>(i & m)], b[static_cast<size_t>((i + 1) & m)], b[static_cast<size_t>((i + 2) & m)], t);
}
}  // namespace

// The wet path through a worn tape: wow (a slow sine plus a seeded slow drift), flutter,
// a two-voice ensemble, then tape age (roll-off and light squash). Every modulator runs
// from the absolute frame, so renders repeat and do not depend on the block size.
void ChaterFx::memorySample(int64_t fi, float inL, float inR, float& l, float& r) {
  float oL = inL, oR = inR;
  if (!memClean_) {
    constexpr int m = kMemLen - 1;
    memL_[static_cast<size_t>(memW_ & m)] = inL, memR_[static_cast<size_t>(memW_ & m)] = inR;
    const double t = static_cast<double>(fi) / sr_;
    const double base = 24.0 * sr_ / 48000.0;
    const double ensBase = 0.017 * sr_, ensA = 0.0022 * sr_ * mEns_;
    const double room = kMemLen - 16 - base - ensBase - 2 * ensA;
    // wow: depth in samples for a pitch swing of up to about 20 cents at the wow rate
    const double aw = std::min(mWow_ * 0.0115 * sr_ / (2 * kPi * mRate_), room / 3.4);
    const double af = mFlut_ * 0.0022 * sr_ / (2 * kPi * 6.5);
    double drift = 0;
    if (aw > 0) {
      const double seg = 2.3 * sr_;
      const double pos = static_cast<double>(fi) / seg;
      const auto k = static_cast<int64_t>(std::floor(pos));
      const double u = pos - static_cast<double>(k), sm = u * u * (3 - 2 * u);
      const double a = h01(seed_ + 0x51ED27U, k, 9), b = h01(seed_ + 0x51ED27U, k + 1, 9);
      drift = a + (b - a) * sm;
    }
    const double d = base + aw * (1 + std::sin(2 * kPi * mRate_ * t)) + aw * 1.2 * drift + af * (1 + std::sin(2 * kPi * 6.5 * t)) +
                     af * 0.4 * (1 + std::sin(2 * kPi * 11.3 * t + 0.7));
    if (memPrevD_ >= 0) wowCents_ += (1731.0 * (memPrevD_ - d) - wowCents_) * 0.002;
    memPrevD_ = d;
    float yl = memRead(memL_, memW_, d), yr = memRead(memR_, memW_, d);
    if (mEns_ > 0.001) {
      const double da = d + ensBase * 0.65 + ensA * (1 + std::sin(2 * kPi * 0.37 * t));
      const double db = d + ensBase + ensA * (1 + std::sin(2 * kPi * 0.53 * t + 1.7));
      const double dc = d + ensBase * 0.65 + ensA * (1 + std::sin(2 * kPi * 0.37 * t + kPi));
      const double dd = d + ensBase + ensA * (1 + std::sin(2 * kPi * 0.53 * t + 1.7 + kPi));
      const float e = static_cast<float>(mEns_);
      const float vl = 0.5f * (memRead(memL_, memW_, da) + memRead(memR_, memW_, db));
      const float vr = 0.5f * (memRead(memR_, memW_, dc) + memRead(memL_, memW_, dd));
      yl = yl * (1 - 0.25f * e) + vl * 0.55f * e, yr = yr * (1 - 0.25f * e) + vr * 0.55f * e;
    }
    memW_ = (memW_ + 1) & m;
    const float g = static_cast<float>(sMem_);
    oL = inL + (yl - inL) * g, oR = inR + (yr - inR) * g;
  }
  if (ageOn_) {
    // worn tape: lows and highs roll off, loud parts squash a little
    const double hl = ageHpA_ * (ageHpL_ + oL - ageX1L_), hr = ageHpA_ * (ageHpR_ + oR - ageX1R_);
    ageX1L_ = oL, ageX1R_ = oR, ageHpL_ = flushd(hl), ageHpR_ = flushd(hr);
    ageLp_[0] = flushd(ageLp_[0] + (hl - ageLp_[0]) * ageLpA_), ageLp_[1] = flushd(ageLp_[1] + (ageLp_[0] - ageLp_[1]) * ageLpA_);
    ageLp_[2] = flushd(ageLp_[2] + (hr - ageLp_[2]) * ageLpA_), ageLp_[3] = flushd(ageLp_[3] + (ageLp_[2] - ageLp_[3]) * ageLpA_);
    const double ax = std::max(std::fabs(ageLp_[1]), std::fabs(ageLp_[3]));
    ageEnv_ = flushd(ax + (ageEnv_ - ax) * (ax > ageEnv_ ? ageEnvA_ : ageEnvR_));
    const double gc = 1.0 / (1.0 + mAge_ * 2.0 * std::max(0.0, ageEnv_ - 0.08));
    oL = static_cast<float>(ageLp_[1] * gc), oR = static_cast<float>(ageLp_[3] * gc);
  }
  l = oL, r = oR;
}

// Hiss and crackle: quiet, and only while the input has been playing (silence stays silent).
void ChaterFx::noiseSample(int64_t fi, float& l, float& r) {
  const double gate = std::clamp((actEnv_ - 0.001) * 40.0, 0.0, 1.0);
  if (gate <= 0) {
    hissLpL_ = hissLpR_ = crkL_ = crkR_ = 0;
    return;
  }
  if (mHiss_ > 0.001) {
    const double nl = h01(seed_ + 0x3C6EF372U, fi, 21) * 2 - 1, nr = h01(seed_ + 0x3C6EF372U, fi, 22) * 2 - 1;
    hissLpL_ += (nl - hissLpL_) * 0.55, hissLpR_ += (nr - hissLpR_) * 0.55;
    const double a = mHiss_ * 0.0085 * gate;
    l += static_cast<float>(hissLpL_ * a), r += static_cast<float>(hissLpR_ * a);
  }
  if (mCrk_ > 0.001) {
    double il = 0, ir = 0;
    if (h01(seed_ + 0x7F4A7C15U, fi, 31) < mCrk_ * 0.0006) {
      const double amp = (0.3 + 0.7 * h01(seed_ + 0x7F4A7C15U, fi, 32)) * 0.07 * mCrk_ * gate;
      const double side = h01(seed_ + 0x7F4A7C15U, fi, 33);
      il = amp * (side < 0.7 ? 1 : 0.3), ir = amp * (side > 0.3 ? 1 : 0.3);
      if (h01(seed_ + 0x7F4A7C15U, fi, 34) < 0.5) il = -il, ir = -ir;
    }
    crkL_ = flushd(crkL_ * -0.55 + il), crkR_ = flushd(crkR_ * -0.55 + ir);
    l += static_cast<float>(crkL_), r += static_cast<float>(crkR_);
  }
}

// ------------------------------------------------------------------ process

void ChaterFx::process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) {
  if (!info_ || n <= 0) return;
  if (ctx.sampleRate != sr_) setRate(ctx.sampleRate);
  readParams(state);
  // latency follows the Halo engine setting (constant while it does not change)
  const int engine = static_cast<int>(std::lround(page_[PHaloEngine]));
  const int lat = engine == HaloOff ? 0 : engine == HaloLow ? 512 : 1024;
  if (engine != haEngine_ || lat != latency_) {
    haEngine_ = engine;
    latency_ = lat;
    haN_ = engine == HaloFull ? 1024 : 512;
    haHop_ = haN_ / 4;
    for (int i = 0; i < haN_; ++i) haWin_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / haN_));
    clearHalo();
  }
  // transport and clock
  const bool freeTempo = page_[PTempoMode] >= 0.5;
  tempo_ = freeTempo ? page_[PFreeBpm] : std::max(1.0, ctx.tempo);
  spb_ = 60.0 / tempo_ * sr_;
  dBeat_ = 1.0 / spb_;
  barBeats_ = std::max(0.25, ctx.tsNum * 4.0 / std::max(1, ctx.tsDen));
  const bool playStart = ctx.playing && (!wasPlaying_ || ctx.jumped);
  if (playStart && page_[PRestart] >= 0.5) {
    grng_.seed(seed_);
    jrng_.seed(seed_ + 3);
    spawnAcc_ = 0;
    hits_ = 0;
    br_.on = false;
    if (freeTempo) beat_ = 0, synced_ = false;
  }
  playing_ = ctx.playing;
  wasPlaying_ = ctx.playing;
  if (ctx.playing && !freeTempo) {
    const double target = ctx.tick / std::max(1, ctx.ppq);
    if (!synced_ || std::fabs(target - beat_) > 1e-3) {
      beat_ = target;
      synced_ = false;
    }
    barOrigin_ = ctx.barStart >= 0 ? ctx.barStart / std::max(1, ctx.ppq) : 0;
  } else if (!ctx.playing) {
    barOrigin_ = 0;
  }
  const double grid = gridBeats(static_cast<int>(std::lround(page_[PGrid])));
  if (!synced_ || grid != grid_) {
    grid_ = grid;
    lastSlot_ = static_cast<int64_t>(std::ceil(beat_ / grid_ - 1e-9)) - 1;  // a step exactly here fires now
    synced_ = true;
  }
  if (!smInit_) {
    control(ctx);
    sMix_ = P(PMix), sOut_ = dsp::dbToGain(P(POutput)), sGr_ = 0, sCr_ = 0, sHa_ = P(PHaMix), sGt_ = P(PGtMix);
    sRm_ = sEc_ = sSh_ = 0, sDuck_ = P(PDuck);
    smInit_ = true;
  }
  // idle: silent input, silent output for a while and nothing playing inside
  bool silentIn = true;
  for (int i = 0; i < n && silentIn; ++i) silentIn = L[i] == 0.0f && R[i] == 0.0f;
  bool grainsAlive = false;
  for (const auto& g : grains_) grainsAlive = grainsAlive || g.on;
  // grains read at most a few seconds back: once the capture has been silent that long they are silent too
  const int64_t settle = static_cast<int64_t>(latency_) + 8192 + (grainsAlive ? static_cast<int64_t>(3 * sr_) : 0);
  idle_ = silentIn && quiet_ > settle && !br_.on && page_[PHold] < 0.5;
  float peakL = 0, peakR = 0;
  int pos = 0;
  while (pos < n) {
    const int len = std::min(n - pos, kCB - static_cast<int>(frame_ % kCB));
    if (frame_ % kCB == 0) control(ctx);  // aligned to the absolute frame: any block size gives the same values
    // per-block derived values
    const double mixT = std::clamp(P(PMix), 0.0, 1.0);
    const double outT = dsp::dbToGain(P(POutput));
    const double grT = std::pow(std::clamp(P(PGrLevel), 0.0, 1.0), 2.0);
    const double crT = std::pow(std::clamp(P(PCrMix), 0.0, 1.0), 2.0);
    const double rmT = std::pow(std::clamp(P(PRmMix), 0.0, 1.0), 2.0);
    const double ecT = std::pow(std::clamp(P(PEcMix), 0.0, 1.0), 2.0);
    const double shT = std::clamp(P(PShFeedback), 0.0, 0.95);
    const double washT = br_.on ? br_.wash : 0.0;
    const bool haloOn = engine != HaloOff && (P(PHaLift) > 0 || P(PHaTwinkle) > 0 || P(PHaSmear) > 0 || P(PHaFreeze) >= 0.5) && P(PHaMix) > 0;
    const double haT = haloOn ? std::clamp(P(PHaMix), 0.0, 1.0) : 0.0;
    const bool gritOn = P(PGtBits) < 23.9 || P(PGtRate) < std::min(47900.0, sr_ * 0.98) || P(PGtDrive) > 0.01;
    const bool holdNow = page_[PHold] >= 0.5;
    if (holdNow && !holdOn_) holdLen_ = static_cast<int>(std::clamp(lengthBars(Pi(PLength)) * barBeats_ * spb_, 64.0, kCatch * 0.5));
    holdOn_ = holdNow;
    const double density = P(PGrDensity);
    // crystal tuning
    if (crT > 0 || sCr_ > 1e-6) {
      const double root = 60 + Pi(PRoot);
      const int chord = Pi(PCrChord);
      const double spread = std::clamp(P(PCrSpread), 0.0, 1.0);
      const double decay = std::max(0.05, P(PCrDecay));
      for (int v = 0; v < 6; ++v) {
        const double f = dsp::noteToHz(root + chordSemis(chord, v));
        for (int side = 0; side < 2; ++side) {
          const double cents = (side ? 1 : -1) * spread * 8.0 * (v % 2 ? 1 : -1);
          const double fs = f * std::exp2(cents / 1200.0);
          const double D = std::clamp(sr_ / fs, 4.0, static_cast<double>(kCombLen - 4));
          combD_[v * 2 + side] = D;
          combG_[v * 2 + side] = std::pow(10.0, -3.0 * D / (decay * sr_));
        }
        const double pan = spread * (v % 2 ? 0.8 : -0.8) * ((v / 2) % 2 ? 0.6 : 1.0);
        combPanL_[v] = std::cos((pan + 1) * kPi / 4) * std::sqrt(2.0);
        combPanR_[v] = std::sin((pan + 1) * kPi / 4) * std::sqrt(2.0);
      }
      combDamp_ = (1.0 - std::clamp(P(PCrBright), 0.0, 1.0)) * 0.75;
    }
    if (shT > 0 || sSh_ > 1e-6) shLpA_ = std::exp(-2 * kPi * std::clamp(P(PShTone), 200.0, sr_ * 0.45) / sr_);
    if (gritOn) {
      const double rate = P(PGtRate);
      if (std::fabs(rate - gritRate_) > 1e-6) {
        gritRate_ = rate;
        const double fc = std::min(rate * 0.45, sr_ * 0.45);
        for (int i = 0; i < 2; ++i) gritPreL_[i].set(dsp::Biquad::LowPass, sr_, fc, 0.707, 0), gritPreR_[i].set(dsp::Biquad::LowPass, sr_, fc, 0.707, 0);
        gritPostL_.set(dsp::Biquad::LowPass, sr_, fc, 0.707, 0), gritPostR_.set(dsp::Biquad::LowPass, sr_, fc, 0.707, 0);
      }
    }
    if (rmT > 0 || sRm_ > 1e-6 || washT > 0 || !roomClean_) {
      const double size = std::clamp(P(PRmSize), 0.0, 1.0), decay = std::max(0.2, P(PRmDecay)), damp = std::clamp(P(PRmDamping), 0.0, 1.0);
      if (size != roomKey_[0] || decay != roomKey_[1] || damp != roomKey_[2]) {
        roomKey_[0] = size, roomKey_[1] = decay, roomKey_[2] = damp;
        static const int base[8] = {1031, 1327, 1523, 1871, 2053, 2311, 2539, 2789};
        for (int i = 0; i < 8; ++i) {
          roomLen_[i] = std::clamp(static_cast<int>(base[i] * (0.35 + size * 2.4) * sr_ / 48000.0), 16, kRoomLen - 4 - static_cast<int>(40 * sr_ / 48000.0));
          roomG_[i] = std::pow(10.0, -3.0 * roomLen_[i] / (decay * sr_));
        }
        roomDamp_ = damp * 0.7;
      }
    }
    if (ecT > 0 || sEc_ > 1e-6) ecLpA_ = std::exp(-2 * kPi * std::clamp(P(PEcTone), 200.0, sr_ * 0.45) / sr_);
    ecDiff_ = std::clamp(P(PEcDiffuse), 0.0, 1.0);
    // haze
    warm_ = std::clamp(P(PWarmth), 0.0, 1.0);
    if (warm_ > 0.001) {
      const double g = 1.0 + 3.0 * warm_;
      warmMk_ = 0.25 * g / std::tanh(0.25 * g);
    }
    {
      const double hz = P(PDamp);
      const bool on = hz < 19900 && hz < sr_ * 0.45;
      if (on && !dampOn_) dampL_[0] = dampL_[1] = dampR_[0] = dampR_[1] = 0;
      dampOn_ = on;
      if (on) dampA_ = 1.0 - std::exp(-2 * kPi * hz / sr_);
    }
    const bool hazeOn = warm_ > 0.001 || dampOn_;
    // memory
    mWow_ = std::clamp(P(PWow), 0.0, 1.0), mFlut_ = std::clamp(P(PFlutter), 0.0, 1.0), mEns_ = std::clamp(P(PEnsemble), 0.0, 1.0);
    mRate_ = std::clamp(P(PWowRate), 0.05, 4.0), mAge_ = std::clamp(P(PAge), 0.0, 1.0);
    mHiss_ = std::clamp(P(PHiss), 0.0, 1.0), mCrk_ = std::clamp(P(PCrackle), 0.0, 1.0);
    const bool memMod = mWow_ > 0.001 || mFlut_ > 0.001 || mEns_ > 0.001;
    const double memT = memMod ? 1.0 : 0.0;
    if (!memMod && sMem_ < 1e-5 && !memClean_) clearMemory(), sMem_ = 0;
    if (memMod) memClean_ = false;
    {
      const bool on = mAge_ > 0.001;
      if (on && !ageOn_) ageHpL_ = ageHpR_ = ageX1L_ = ageX1R_ = ageEnv_ = 0, ageLp_[0] = ageLp_[1] = ageLp_[2] = ageLp_[3] = 0;
      ageOn_ = on;
      if (on) {
        ageHpA_ = std::exp(-2 * kPi * (25.0 + 150.0 * mAge_) / sr_);
        ageLpA_ = 1.0 - std::exp(-2 * kPi * std::min(18000.0 * std::pow(0.2, mAge_), sr_ * 0.45) / sr_);
      }
    }
    const bool noiseOn = mHiss_ > 0.001 || mCrk_ > 0.001;
    const bool memOn = !memClean_ || ageOn_ || noiseOn;
    // space: freeze, shimmer, modulation (per control block from the absolute frame)
    const double frzT = P(PRmFreeze) >= 0.5 ? 1.0 : 0.0;
    const double rshT = std::clamp(P(PRmShimmer), 0.0, 1.0);
    {
      const double depth = std::clamp(P(PRmMod), 0.0, 1.0) * 16.0 * sr_ / 48000.0;
      static const double fr[8] = {0.31, 0.43, 0.37, 0.53, 0.47, 0.61, 0.29, 0.67};
      const double tb = static_cast<double>(frame_ - frame_ % kCB) / sr_;
      for (int k = 0; k < 8; ++k) modOff_[k] = depth * (1 + std::sin(2 * kPi * fr[k] * tb + k * 0.9));
    }
    {
      const double lo = P(PLowCut), hi = P(PHighCut), tilt = P(PTilt);
      if (lo != toneKey_[0] || hi != toneKey_[1] || tilt != toneKey_[2]) {
        const bool wasLow = lowOn_, wasHigh = highOn_, wasTilt = tiltOn_;
        toneKey_[0] = lo, toneKey_[1] = hi, toneKey_[2] = tilt;
        lowOn_ = lo > 20.5, highOn_ = hi < 19900 && hi < sr_ * 0.45, tiltOn_ = std::fabs(tilt) > 0.05;
        toneL_[0].set(dsp::Biquad::HighPass, sr_, lo, 0.707, 0), toneR_[0].set(dsp::Biquad::HighPass, sr_, lo, 0.707, 0);
        toneL_[1].set(dsp::Biquad::LowPass, sr_, std::min(hi, sr_ * 0.45), 0.707, 0), toneR_[1].set(dsp::Biquad::LowPass, sr_, std::min(hi, sr_ * 0.45), 0.707, 0);
        toneL_[2].set(dsp::Biquad::LowShelf, sr_, 1000, 0.6, -tilt * 0.5), toneR_[2].set(dsp::Biquad::LowShelf, sr_, 1000, 0.6, -tilt * 0.5);
        toneL_[3].set(dsp::Biquad::HighShelf, sr_, 1000, 0.6, tilt * 0.5), toneR_[3].set(dsp::Biquad::HighShelf, sr_, 1000, 0.6, tilt * 0.5);
        if (!wasLow) toneL_[0].clear(), toneR_[0].clear();
        if (!wasHigh) toneL_[1].clear(), toneR_[1].clear();
        if (!wasTilt) toneL_[2].clear(), toneR_[2].clear(), toneL_[3].clear(), toneR_[3].clear();
      }
    }
    const double width = std::clamp(P(PWidth), 0.0, 2.0);
    const double orbitLen = orbitBeats(Pi(POrbit));
    const int mode = Pi(PTrigMode);
    const double sens = std::clamp(P(PSensitivity), 0.0, 1.0);
    const double thr = 1.4 + (1.0 - sens) * 4.5;
    const int gapS = static_cast<int>(P(PHitGap) * 0.001 * sr_);
    const double hitChance = std::clamp(P(PHitChance), 0.0, 1.0);
    folA_ = std::exp(-1.0 / (std::max(0.0001, P(PFoAttack) * 0.001) * sr_));
    folR_ = std::exp(-1.0 / (std::max(0.001, P(PFoRelease) * 0.001) * sr_));
    const double duckT = std::clamp(P(PDuck), 0.0, 1.0);
    const bool safety = page_[PSafety] >= 0.5;
    const double smA = smA_;
    const bool sparkleOn = grT > 0 || sGr_ > 1e-6;
    const bool shimOn = sparkleOn && (shT > 0 || sSh_ > 1e-6);
    const bool crystalOn = crT > 0 || sCr_ > 1e-6;
    // the room runs while it is fed (mix, wash, freeze) or its tail still rings
    if (!roomClean_ && (silentIn || (rmT == 0 && sRm_ < 1e-6 && washT == 0)) && roomLevel_ < 1e-6 && sRmFrz_ < 1e-3 && frzT == 0) clearRoom(), roomLevel_ = 0;
    const bool roomOn = rmT > 0 || sRm_ > 1e-6 || washT > 0 || !roomClean_;
    const bool echoOn = ecT > 0 || sEc_ > 1e-6;
    if (!sparkleOn) for (auto& g : grains_) g.on = false;
    if (!shimOn && !shimClean_) clearShimmer();
    if (!crystalOn && !crystalClean_) clearCrystal();
    if (!echoOn && !echoClean_) clearEcho();
    if (shimOn) shimClean_ = false;
    if (crystalOn) crystalClean_ = false;
    if (roomOn) roomClean_ = false;
    if (echoOn) echoClean_ = false;

    for (int i = pos; i < pos + len; ++i) {
      float xl = L[i], xr = R[i];
      // ---- detectors on the real input
      const double ax = std::max(std::fabs(xl), std::fabs(xr));
      fastEnv_ = ax + (fastEnv_ - ax) * (ax > fastEnv_ ? fastA_ : fastR_);
      slowEnv_ = ax + (slowEnv_ - ax) * (ax > slowEnv_ ? slowA_ : slowR_);
      folEnv_ = ax + (folEnv_ - ax) * (ax > folEnv_ ? folA_ : folR_);
      duckEnv_ = ax + (duckEnv_ - ax) * (ax > duckEnv_ ? fastA_ : slowR_);
      actEnv_ = ax > actEnv_ ? ax : flushd(actEnv_ * actR_);
      bool onset = false;
      if (gapCount_ < (1 << 30)) ++gapCount_;
      if (fastEnv_ > 0.003 && fastEnv_ > slowEnv_ * thr && gapCount_ >= gapS) {
        onset = true;
        gapCount_ = 0;
      }
      // ---- catch: the input, or the frozen loop while held
      if (holdOn_) {
        xl = catchL_[static_cast<size_t>((w_ - holdLen_) & kCatchMask)];
        xr = catchR_[static_cast<size_t>((w_ - holdLen_) & kCatchMask)];
      }
      catchL_[static_cast<size_t>(w_ & kCatchMask)] = xl;
      catchR_[static_cast<size_t>(w_ & kCatchMask)] = xr;
      ++w_;
      // ---- when breaks fire
      const auto slot = static_cast<int64_t>(std::floor(beat_ / grid_ + 1e-9));
      if (slot != lastSlot_) {
        if (slot > lastSlot_) onSlot(slot);
        lastSlot_ = slot;
      }
      if (onset) {
        const int64_t hk = hits_++;
        if ((mode == TrigHits || mode == TrigBoth) && !br_.on && h01(seed_ + 0x2545F491U, hk, 1) < hitChance) fire(hk, true, beat_);
      }
      beat_ += dBeat_;
      // ---- the main signal
      float mL, mR, washL, washR;
      breakSample(xl, xr, mL, mR, washL, washR);
      // ---- smoothers
      sMix_ = mixT + (sMix_ - mixT) * smA;
      sOut_ = outT + (sOut_ - outT) * smA;
      sGr_ = grT + (sGr_ - grT) * smA;
      sCr_ = crT + (sCr_ - crT) * smA;
      sHa_ = haT + (sHa_ - haT) * smA;
      sGt_ = P(PGtMix) + (sGt_ - P(PGtMix)) * smA;
      sRm_ = rmT + (sRm_ - rmT) * smA;
      sEc_ = ecT + (sEc_ - ecT) * smA;
      sSh_ = shT + (sSh_ - shT) * smA;
      sDuck_ = duckT + (sDuck_ - duckT) * smA;
      sMem_ = memT + (sMem_ - memT) * smA;
      sRmFrz_ = frzT + (sRmFrz_ - frzT) * smA;
      sRmShim_ = rshT + (sRmShim_ - rshT) * smA;
      const float duckG = static_cast<float>(1.0 - sDuck_ * std::min(1.0, duckEnv_ * 3.0));
      // ---- dry path (aligned with the halo)
      float dL = mL, dR = mR;
      if (latency_ > 0) {
        dryL_[static_cast<size_t>(dryW_)] = mL, dryR_[static_cast<size_t>(dryW_)] = mR;
        const int rd = (dryW_ - latency_ + kDryLen) & (kDryLen - 1);
        dL = dryL_[static_cast<size_t>(rd)], dR = dryR_[static_cast<size_t>(rd)];
        dryW_ = (dryW_ + 1) & (kDryLen - 1);
      }
      if (idle_) {
        // the clock and the breaks advanced above; grains draw their randoms
        spawnAcc_ += density / sr_;
        while (spawnAcc_ >= 1) spawnAcc_ -= 1, spawnGrain(false);
        if (latency_ > 0) haloSample(0, 0, mL, mR);
        L[i] = R[i] = 0;
        continue;
      }
      // ---- sparkle
      float layL = 0, layR = 0;
      spawnAcc_ += density / sr_;
      while (spawnAcc_ >= 1) spawnAcc_ -= 1, spawnGrain(sparkleOn && grT > 0);
      if (sparkleOn) {
        float gl, gr;
        grainsSample(gl, gr);
        const float gn = static_cast<float>(sGr_ / std::sqrt(std::max(1.0, density * std::clamp(P(PGrSize), 0.003, 0.3) * 0.6)));
        gl = static_cast<float>(gL_.process(gl * gn)), gr = static_cast<float>(gR_.process(gr * gn));
        layL += gl, layR += gr;
        if (shimOn) {
          float sl, sr;
          shimmerSample(gl, gr, sl, sr);
          const float sa = static_cast<float>(std::min(1.0, sSh_ * 1.6));
          layL += sl * sa, layR += sr * sa;
        }
      }
      if (crystalOn) {
        float cl, cr;
        crystalSample(mL + layL * 0.5f, mR + layR * 0.5f, cl, cr);
        layL += cl * static_cast<float>(sCr_), layR += cr * static_cast<float>(sCr_);
      }
      if (orbitLen > 0 && (layL != 0 || layR != 0)) {
        const double ph = beat_ / orbitLen;
        const double pan = std::sin(2 * kPi * (ph - std::floor(ph))) * 0.75;
        const float ol = static_cast<float>(std::cos((pan + 1) * kPi / 4) * std::sqrt(2.0)), orr = static_cast<float>(std::sin((pan + 1) * kPi / 4) * std::sqrt(2.0));
        const float m = 0.5f * (layL + layR);
        layL = m * ol + (layL - m), layR = m * orr + (layR - m);
      }
      layL *= duckG, layR *= duckG;
      float wl = mL + layL, wr = mR + layR;
      // ---- halo (always delays by the engine's window)
      haloAmt_ = sHa_;
      if (latency_ > 0) haloSample(wl, wr, wl, wr);
      // ---- grit
      if (gritOn) gritSample(wl, wr);
      // ---- haze and memory
      if (hazeOn) hazeSample(wl, wr);
      if (memOn) {
        const int64_t fi = frame_ + (i - pos);
        memorySample(fi, wl, wr, wl, wr);
        if (noiseOn) noiseSample(fi, wl, wr);
      }
      // ---- spray width
      if (width != 1.0) {
        const float m = 0.5f * (wl + wr), s = 0.5f * (wl - wr) * static_cast<float>(width);
        wl = m + s, wr = m - s;
      }
      // ---- room and echo
      if (roomOn || echoOn) {
        float al = 0, ar = 0;
        if (roomOn) {
          float rl, rr;
          const float rg = static_cast<float>(sRm_);
          roomSample(wl * rg + washL * 0.35f, wr * rg + washR * 0.35f, rl, rr);
          al += rl, ar += rr;
        }
        if (echoOn) {
          float el, er;
          echoSample(wl + washL, wr + washR, el, er);
          al += el * static_cast<float>(sEc_), ar += er * static_cast<float>(sEc_);
        }
        wl += al * duckG, wr += ar * duckG;
      }
      // ---- tone
      double tl = wl, tr = wr;
      if (lowOn_) tl = toneL_[0].process(tl), tr = toneR_[0].process(tr);
      if (highOn_) tl = toneL_[1].process(tl), tr = toneR_[1].process(tr);
      if (tiltOn_) tl = toneL_[3].process(toneL_[2].process(tl)), tr = toneR_[3].process(toneR_[2].process(tr));
      // ---- out
      double ol = dL + (tl - dL) * sMix_, orr = dR + (tr - dR) * sMix_;
      ol *= sOut_, orr *= sOut_;
      if (safety) ol = softLimit(ol, 0.966), orr = softLimit(orr, 0.966);
      if (!std::isfinite(ol) || !std::isfinite(orr)) ol = orr = 0;
      L[i] = static_cast<float>(ol), R[i] = static_cast<float>(orr);
      peakL = std::max(peakL, std::fabs(L[i])), peakR = std::max(peakR, std::fabs(R[i]));
      // ---- the tape view: one column per kTapeCols-th of four bars
      const double win = 4 * barBeats_;
      double inWin = std::fmod(beat_ - barOrigin_, win);
      if (inWin < 0) inWin += win;
      const int col = std::clamp(static_cast<int>(inWin / win * kTapeCols), 0, kTapeCols - 1);
      colPeak_ = std::max(colPeak_, std::max(std::fabs(mL), std::fabs(mR)));
      if (col != lastCol_) {
        if (lastCol_ >= 0) {
          tel.peak[lastCol_].store(colPeak_, std::memory_order_relaxed);
          tel.type[lastCol_].store(static_cast<uint8_t>(br_.on ? br_.type : BrNone), std::memory_order_relaxed);
        }
        lastCol_ = col;
        colPeak_ = 0;
      }
    }
    frame_ += len;
    pos += len;
  }
  const bool silentOut = peakL == 0 && peakR == 0;
  quiet_ = silentIn && silentOut ? quiet_ + n : 0;
  // live data for the editor
  tel.beat.store(beat_, std::memory_order_relaxed);
  tel.tempo.store(tempo_, std::memory_order_relaxed);
  tel.barOrigin.store(barOrigin_, std::memory_order_relaxed);
  tel.barBeats.store(barBeats_, std::memory_order_relaxed);
  tel.playing.store(ctx.playing ? 1 : 0, std::memory_order_relaxed);
  tel.active.store(br_.on ? br_.type : BrNone, std::memory_order_relaxed);
  tel.follower.store(static_cast<float>(folVal_), std::memory_order_relaxed);
  tel.swA.store(static_cast<float>(sw_[0]), std::memory_order_relaxed);
  tel.swB.store(static_cast<float>(sw_[1]), std::memory_order_relaxed);
  tel.outL.store(std::max(peakL, tel.outL.load(std::memory_order_relaxed) * 0.8f), std::memory_order_relaxed);
  tel.outR.store(std::max(peakR, tel.outR.load(std::memory_order_relaxed) * 0.8f), std::memory_order_relaxed);
  tel.wow.store(static_cast<float>(wowCents_), std::memory_order_relaxed);
  tel.room.store(static_cast<float>(roomLevel_), std::memory_order_relaxed);
  for (int v = 0; v < 6; ++v) tel.crystal[v].store(crystalLevel_[v], std::memory_order_relaxed);
  for (int m = 0; m < 8; ++m) tel.macros[m].store(static_cast<float>(macro_[m]), std::memory_order_relaxed);
  for (int g = 0; g < kViewGrains; ++g) {
    const Grain& gr = grains_[g];
    tel.grains[g].life.store(gr.on ? static_cast<float>(gr.age) / static_cast<float>(std::max(1, gr.len)) : -1.0f, std::memory_order_relaxed);
    tel.grains[g].pan.store(gr.pan, std::memory_order_relaxed);
    tel.grains[g].semis.store(gr.semis, std::memory_order_relaxed);
  }
}

}  // namespace

// ------------------------------------------------------------------ pack entry points

std::unique_ptr<Generator> createGenerator(const std::string&) { return nullptr; }

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == kType) return std::make_unique<ChaterFx>();
  return nullptr;
}

bool prepareAux(const PluginState&, ChannelAux&) { return false; }

bool liveView(const Effect* fx, LiveView& out) {
  const auto* c = dynamic_cast<const ChaterFx*>(fx);
  if (!c) return false;
  const Tel& t = c->tel;
  out.beat = t.beat.load(std::memory_order_relaxed);
  out.tempo = t.tempo.load(std::memory_order_relaxed);
  out.barOrigin = t.barOrigin.load(std::memory_order_relaxed);
  out.barBeats = t.barBeats.load(std::memory_order_relaxed);
  out.playing = t.playing.load(std::memory_order_relaxed) != 0;
  out.activeType = t.active.load(std::memory_order_relaxed);
  for (int i = 0; i < kTapeCols; ++i) out.peak[i] = t.peak[i].load(std::memory_order_relaxed), out.type[i] = t.type[i].load(std::memory_order_relaxed);
  for (int i = 0; i < kEventRing; ++i) {
    const auto& e = t.ev[i];
    const uint32_t s0 = e.seq.load(std::memory_order_acquire);
    FiredBreak f;
    f.beat = e.beat.load(std::memory_order_relaxed);
    f.beats = e.beats.load(std::memory_order_relaxed);
    f.type = e.type.load(std::memory_order_relaxed);
    f.repeats = e.repeats.load(std::memory_order_relaxed);
    f.hit = e.hit.load(std::memory_order_relaxed) != 0;
    f.seq = e.seq.load(std::memory_order_acquire) == s0 ? s0 : 0;
    out.events[i] = f;
  }
  out.follower = t.follower.load(std::memory_order_relaxed);
  out.swayA = t.swA.load(std::memory_order_relaxed);
  out.swayB = t.swB.load(std::memory_order_relaxed);
  out.outL = t.outL.load(std::memory_order_relaxed);
  out.outR = t.outR.load(std::memory_order_relaxed);
  out.wow = t.wow.load(std::memory_order_relaxed);
  out.room = t.room.load(std::memory_order_relaxed);
  for (int v = 0; v < 6; ++v) out.crystal[v] = t.crystal[v].load(std::memory_order_relaxed);
  for (int m = 0; m < 8; ++m) out.macros[m] = t.macros[m].load(std::memory_order_relaxed);
  for (int g = 0; g < kViewGrains; ++g) {
    const float life = t.grains[g].life.load(std::memory_order_relaxed);
    out.grains[g].alive = life >= 0;
    out.grains[g].life = life;
    out.grains[g].pan = t.grains[g].pan.load(std::memory_order_relaxed);
    out.grains[g].semis = t.grains[g].semis.load(std::memory_order_relaxed);
  }
  return true;
}

int firedBreaks(const Effect* fx, FiredBreak* out, int max) {
  const auto* c = dynamic_cast<const ChaterFx*>(fx);
  if (!c) return 0;
  const int total = c->testCount();
  const int n = std::min({max, total, kTestLog});
  for (int i = 0; i < n; ++i) out[i] = c->testLog()[static_cast<size_t>((total - n + i) % kTestLog)];
  return n;
}

bool idle(const Effect* fx) {
  const auto* c = dynamic_cast<const ChaterFx*>(fx);
  return c && c->isIdle();
}

}  // namespace gl::packs::chater

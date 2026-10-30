// Gridline — "plasma" pack: runtime internals shared by the DSP files
// (wavetables, filters, effects, sequencers, generator). Not used by the editor
// except for the display helpers at the end.
#pragma once
#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/Generators.h"
#include "../../engine/Sample.h"
#include "plasma.h"

namespace gl::packs::plasma {

using cpx = std::complex<float>;
inline constexpr double kPi2 = 6.283185307179586;

// ------------------------------------------------------------------ FFT (radix 2, in place)
class Fft {
 public:
  void init(int n);  // power of two; allocates (UI thread / constructor)
  int size() const { return n_; }
  void forward(cpx* x) const;
  void inverse(cpx* x) const;  // scaled by 1/n
 private:
  void run(cpx* x, bool inv) const;
  int n_ = 0;
  std::vector<cpx> tw_;
  std::vector<int> rev_;
};
const Fft& fft2048();  // shared, built on first use (UI thread first)

// ------------------------------------------------------------------ band-limited tables
struct MipTable {
  static constexpr int kLevels = 11;   // level l keeps harmonics 1 .. 1024 >> l
  static constexpr int kHarm = 1024;
  int frames = 1;
  std::array<int, kLevels> len{};      // samples per frame (power of two)
  std::array<std::vector<float>, kLevels> data;  // frames * (len + 1) (one guard sample)
  std::vector<float> mag, phs;         // per frame harmonic magnitude / phase (frames * kHarm), for spectral mode & display
  std::shared_ptr<const Table> raw;    // original frames (granular reads them as audio)
  std::string name;
  const float* frame(int level, int f) const {
    return data[static_cast<size_t>(level)].data() + static_cast<size_t>(f) * static_cast<size_t>(len[static_cast<size_t>(level)] + 1);
  }
};
std::shared_ptr<const MipTable> buildMip(const std::shared_ptr<const Table>& t);
// Factory tables, band-limited (all built on the first call: UI thread).
const std::vector<std::shared_ptr<const MipTable>>& factoryMips();
inline int mipLevel(double phaseInc) {  // phase increment per sample (cycles)
  const double allowed = 0.5 / std::max(1e-9, std::fabs(phaseInc));  // harmonics below Nyquist
  if (allowed >= 1024) return 0;
  if (allowed < 1) return MipTable::kLevels - 1;
  int l = static_cast<int>(std::ceil(std::log2(1024.0 / allowed)));
  return l < 0 ? 0 : (l >= MipTable::kLevels ? MipTable::kLevels - 1 : l);
}
// Reads frame position fpos (0..frames-1) at phase ph (0..1).
inline float mipRead(const MipTable& t, int level, double fpos, bool smooth, double ph) {
  const int L = t.len[static_cast<size_t>(level)];
  const double x = ph * L;
  int i = static_cast<int>(x);
  const float fr = static_cast<float>(x - i);
  i &= L - 1;
  const int last = t.frames - 1;
  if (fpos <= 0 || last <= 0) {
    const float* a = t.frame(level, 0);
    return a[i] + (a[i + 1] - a[i]) * fr;
  }
  if (fpos >= last) {
    const float* a = t.frame(level, last);
    return a[i] + (a[i + 1] - a[i]) * fr;
  }
  int f0 = static_cast<int>(fpos);
  const float* a = t.frame(level, f0);
  const float va = a[i] + (a[i + 1] - a[i]) * fr;
  if (!smooth) {
    if (fpos - f0 >= 0.5) {
      const float* b = t.frame(level, f0 + 1);
      return b[i] + (b[i + 1] - b[i]) * fr;
    }
    return va;
  }
  const float* b = t.frame(level, f0 + 1);
  const float vb = b[i] + (b[i + 1] - b[i]) * fr;
  return va + (vb - va) * static_cast<float>(fpos - f0);
}

// ------------------------------------------------------------------ factory noises / impulses
struct NoiseSrc {
  std::string name;
  std::vector<float> L, R;  // generated audio at 44.1 kHz
  bool loop = true;
};
const std::vector<NoiseSrc>& factoryNoises();  // index = N_Type - 1 (built on first call)
// Factory impulse responses for the Convolve module (index = choice - 1), 48 kHz stereo.
struct Impulse {
  std::vector<float> L, R;
  double rate = 48000;
};
const Impulse& factoryImpulse(int index);

// ------------------------------------------------------------------ multisample zones
struct Zone {
  SamplePtr s;
  int loKey = 0, hiKey = 127, loVel = 0, hiVel = 127, center = 60;
  double tune = 0;     // cents
  double gainDb = 0;
  int loopMode = 0;    // 0 none, 1 continuous, 2 sustain
  int64_t loopStart = 0, loopEnd = 0;
  int64_t start = 0, end = -1;
  // J-203a: the map's velocity tracking (0..1) and amplitude envelope (seconds; sustain 0..1).
  // Used with the "Mapped" multisample level law; the release default is the source's (measured).
  double velTrack = 1.0;
  double egDelay = 0, egAttack = 0, egHold = 0, egDecay = 0, egSustain = 1, egRelease = 0.5;
};
bool parseSfz(const std::string& path, std::vector<Zone>& zones, std::string* err);

// ------------------------------------------------------------------ prepared data (aux)
struct SpecSrc {           // STFT analysis of a sample for the spectral oscillator
  static constexpr int kN = 2048, kBins = 1024, kHop = 512;
  int frames = 0;
  double rate = 44100;     // sample rate of the analysed audio
  std::vector<float> mag;  // frames * kBins (magnitude, peak-normalised)
};
struct OscAux {
  std::shared_ptr<const MipTable> mip;  // custom / file table (null = factory from O_Table)
  SamplePtr sample;                     // sample / granular / spectral source
  std::vector<double> slices;           // slice start frames
  std::vector<Zone> zones;              // multisample
  std::shared_ptr<const SpecSrc> spec;  // sample analysis for the spectral oscillator
  std::array<std::array<float, 257>, 4> remap{};  // remap warps 1-4 sampled 0..1
};
struct LfoAux {
  std::vector<CurvePt> pts;  // drawn shape (empty = factory from L_Shape)
  int loop = -1;
  std::vector<CurvePt> path;
  std::vector<float> pathX;
};
struct Aux : AuxData {
  std::array<OscAux, kOscs> osc;
  std::array<LfoAux, kLfos> lfo;
  std::vector<std::vector<CurvePt>> factoryShapes;  // index = L_Shape (0 unused)
  std::array<float, 129> velo{}, note{};            // scalar graphs sampled 0..1
  std::array<std::array<float, 65>, kModSlots> modCurve{}, auxCurve{};
  std::array<bool, kModSlots> hasModCurve{}, hasAuxCurve{};
  std::vector<BusLink> busLinks;
  std::vector<FxNode> fx;              // rack layout (resolved at build with the build's params)
  std::array<std::array<float, 257>, kFxSlots> shapeA{}, shapeB{};  // distortion X-Shaper curves
  std::array<int, kFxSlots> irIndex{};  // convolve: 0 = file, 1.. = factory (from data), -1 = param
  std::array<std::shared_ptr<const Sample>, kFxSlots> irFile;
  SamplePtr noiseFile;
  std::array<double, 128> tuning{};    // MIDI note -> note number (fractional); identity when no .tun
  bool hasTuning = false;
  Model model;                         // the rest (arp / clip banks, voice sequence, macro names)
};
std::shared_ptr<const Aux> buildAux(const PluginState& state);  // cached by data

// ------------------------------------------------------------------ filters (per voice and FX)
class Filter {
 public:
  Filter();
  ~Filter();
  Filter(const Filter&) = delete;
  Filter& operator=(const Filter&) = delete;
  void reset();
  // Coefficients for the next samples (call at control rate). hz: cutoff,
  // res / drive / var 0..1, spread: stereo cutoff offset in semitones (L -, R +).
  // boost: the drive raises the level (gain up to +24 dB into a soft clip with headroom) instead of
  // keeping it about even.
  void set(int type, double sr, double hz, double res, double drive, double var, double spreadSt, bool boost = false);
  void process(float& l, float& r);
  int type() const { return type_; }
  // Display: magnitude response at n frequencies (UI thread; allocates).
  static void responseCurve(int type, double sr, double cutoffHz, double res, double var, const float* hz, float* out, int n);
  static constexpr int kBuf = 16384;  // per channel (delay-based types; 170 ms at the 96 kHz voice rate)
 private:
  struct Ch;
  int type_ = -1;
  double sr_ = 48000;
  std::unique_ptr<Ch[]> ch_;  // 2 channels
  std::unique_ptr<float[]> mem_;
};

// ------------------------------------------------------------------ effects
struct FxContext {
  double sr = 48000, tempo = 120, beat = 0;  // beat = song position (quarter notes) at block start
  bool playing = false;
  float sideL = 0, sideR = 0;
  bool driveBoost = false;  // filter effect: boost drive (Filter Drive Mode)
};
class FxUnit;  // one effect slot (any type), plasma_fx_dsp.cpp
struct FxBank {
  double sr = 0;
  std::array<std::unique_ptr<FxUnit>, kFxSlots> unit;  // null = slot without memory
  ~FxBank();
};
// Measurement hook (offline comparison tools only; null in the app): called once per control chunk
// with the 8 voice buses (main L/R, direct L/R, bus 1 L/R, bus 2 L/R, m samples each) just before
// the effect racks, so a tool can feed the racks a known signal.
using FxInputHook = void (*)(float* const* bus, int m);
extern FxInputHook gFxInputHook;
// Builds the effect memory for a state (UI thread): slots that have a type.
std::unique_ptr<FxBank> makeFxBank(const PluginState& s, const Aux* aux, double sr);
void fxReset(FxUnit& u);
// Runs one slot over n samples in place. p: the slot's 28 normalized (modulated) params.
void fxProcess(FxUnit& u, int type, const float* p, float* L, float* R, int n, const FxContext& c, const Aux* aux, int slot);
// Meter of the last block (0..1 peak), for the editor.
float fxMeter(const FxUnit& u);
// LR4 band splitter used by the splitter modules and the multiband compressor.
struct Lr4 {
  double a1 = 0, a2 = 0, b0 = 0, b1 = 0, b2 = 0, hb0 = 0, hb1 = 0, hb2 = 0;
  double lz[2][2][2]{}, hz[2][2][2]{};  // [stage][channel][state]
  void set(double sr, double hz);
  void split(int c, double x, double& lo, double& hi);
};

// ------------------------------------------------------------------ sequencer (arp / clip)
struct SeqNote {
  int key = 60;
  float vel = 0.8f;
  bool on = true;
  int offset = 0;  // sample offset in the block
};
struct SeqOut {
  SeqNote ev[256];
  int n = 0;
  void push(const SeqNote& e) {
    if (n < 256) ev[n++] = e;
  }
};
class Arp {
 public:
  Arp();
  void reset();
  void noteOn(int key, float vel);
  void noteOff(int key);
  void allOff();
  bool held() const { return heldCount_ > 0 || latched_; }
  // Advances one block: emits note on / off events (offsets inside the block).
  void run(const ArpSlot& s, const float* globalOffsets, int n, double sr, double tempo, double beat, bool playing,
           double swing, int swingDiv, const std::vector<int>& scale, int key, SeqOut& out);
  int step() const { return step_; }
 private:
  std::array<int, 128> heldKey_{};
  std::array<float, 128> heldVel_{};
  std::array<int, 128> order_{};  // played order
  int heldCount_ = 0;
  bool latched_ = false, latchArmed_ = false;
  std::vector<int> sorted_, cycle_, played_;
  int pos_ = 0, step_ = 0, repeat_ = 0, transStep_ = 0;
  double phase_ = 0, gateLeft_ = -1;
  double next_ = 0;  // grid index (steps) of the next step
  int sounding_[16];
  int soundingN_ = 0;
  float veloCur_ = 1;
  uint32_t rng_ = 0x9e3779b9u;
  bool first_ = true;
  bool newNote_ = false, chanceHold_ = true;
  int transDir_ = 1, cycles_ = 0;
  double retrigAcc_ = 0;
};
class ClipPlayer {
 public:
  void reset();
  void noteOn(int key, float vel);
  void noteOff(int key);
  bool active() const { return running_; }
  void run(const ClipSlot& c, const float* offsets, int n, double sr, double tempo, double beat, bool playing, double swing,
           int swingDiv, SeqOut& out, float* laneValues, int laneCount);
  double position() const { return pos_; }
 private:
  bool running_ = false;
  int trigKey_ = 60, held_ = 0;
  float trigVel_ = 0.8f;
  double pos_ = 0;
  int dir_ = 1;
  uint32_t rng_ = 0x1234567u;
  std::array<int8_t, 128> on_{};
  bool finishing_ = false;   // keys released without note gate: stop at the clip end
  double waitBeats_ = -1;    // launch quantize wait
};


// ------------------------------------------------------------------ oscillators (per note)
// Settings of one oscillator for the next control period (modulation applied).
struct OscSet {
  bool on = false;
  bool silent = false;             // outside its key / velocity zone: runs only as a warp source
  int type = OscWavetable;
  const MipTable* mip = nullptr;   // wavetable (also the granular / spectral source without a sample)
  const OscAux* src = nullptr;     // sample / zones / spectral analysis / remap curves
  double srv = 96000;              // voice sample rate
  double pitch = 60;               // MIDI pitch of the oscillator (all offsets applied)
  double hz = 261.6;               // frequency (after Hz offset, ratio)
  double hzOffset = 0;
  int unison = 1;
  float det[kMaxUnison] = {};      // semitone offset per unison voice
  double inc[kMaxUnison] = {};     // phase increment per unison voice (cycles / sample)
  float pl[kMaxUnison] = {}, pr[kMaxUnison] = {};  // pan gains
  float uw[kMaxUnison] = {};       // spread position -1..1 (WT pos / warp spread)
  float wtPos = 0;                 // 0..1
  float uniWt = 0, uniW1 = 0, uniW2 = 0;
  int w1 = 0, w2 = 0;              // warp modes
  float a1 = 0, a2 = 0, v1 = 0, v2 = 0;
  bool smooth = true;
  // sample / granular / spectral / multisample
  float start = 0, end = 1, position = 0, scan = 1;
  bool reverse = false;
  int loopMode = 0;
  float loopStart = 0, loopEnd = 1, loopXfade = 0;
  bool relLoop = false;
  int slice = 1, sliceMode = 0;
  float density = 10, lengthMs = 100, grReverse = 0;
  int winShape = 0;
  float winSkew = 0, winParam = 0.5f;
  float rnd[10] = {};
  int maxGrains = 24, grTrig = 0;
  float sCut = 0, sMix = 1, sLo = 0, sHi = 1;
  bool phaseLock = false, transients = true;
  float timbre = 0, velTrack = 1;
  bool msLaw = false;   // multisample "Mapped" level law (zone velocity tracking, envelope, gain)
  bool msOv = false;    // the oscillator's own envelope replaces the zones' (seconds below)
  float ovDelay = 0, ovAttack = 0, ovHold = 0, ovDecay = 0, ovSustain = 1, ovRelease = 0.5f;
  bool s1 = false;
};
class OscVoice {
 public:
  OscVoice();
  // Note start: phases (memory: contiguous phases of the previous note, or null).
  void start(const OscSet& s, float phase, float randPhase, float randStart, int phaseMode, const double* memory, uint32_t seed,
             int key, float vel);
  // One sample. fm[0..5]: previous outputs of (other osc 1, other osc 2, noise, sub, filter 1, filter 2).
  void tick(const OscSet& s, const float* fm, float& l, float& r, float& mono);
  void release() { released_ = true; }
  bool finished() const { return finished_; }
  void savePhases(double* out) const;
  float lastMono() const { return lastMono_; }
  double displayPos() const { return dispPos_; }
 private:
  float wavetableSample(const OscSet& s, int u, double& ph, double inc, const float* fm);
  void samplePlay(const OscSet& s, float& l, float& r);
  void granular(const OscSet& s, float& l, float& r);
  void spectral(const OscSet& s, const float* fm, float& l, float& r);
  void spectralHop(const OscSet& s);
  double phase_[kMaxUnison] = {};
  double play_[kMaxUnison] = {};
  int dirU_[kMaxUnison] = {};
  float selfFb_[kMaxUnison] = {};
  float postZ_[2][2] = {};          // LP / HP warp state for sample-based types
  uint32_t rng_ = 1;
  bool released_ = false, finished_ = false;
  float lastMono_ = 0;
  double dispPos_ = 0;
  int key_ = 60;
  float vel_ = 0.8f;
  int zone_ = -1;
  // zone amplitude envelope (Mapped law): stage 0 delay .. 4 sustain, 5 release, 6 done
  int zeStage_ = 0;
  double zeT_ = 0, zeLev_ = 0, zeMul_ = 1;
  float zoneEnv(const OscSet& s, const Zone& z);
  // granular
  struct Grain {
    double pos = 0, inc = 0;
    int left = 0, len = 1;
    float pl = 1, pr = 1, gain = 1, skew = 0, param = 0.5f;
    float warp[2] = {0, 0};  // random warp offsets of this grain
    int win = 0;
    bool active = false;
  };
  Grain grains_[64];
  double spawnAcc_[kMaxUnison] = {};
  float grainWarp_[2] = {0, 0};  // window-weighted warp offsets of the sounding grains
  // spectral
  std::unique_ptr<float[]> ola_;     // 2 x 2048
  std::unique_ptr<float[]> sphase_;  // 8 x 1024
  std::unique_ptr<float[]> prevMag_; // 1024
  std::unique_ptr<cpx[]> work_;      // 2 x 2048
  std::unique_ptr<float[]> fmRing_;  // 2048 (mask / vocode source)
  std::unique_ptr<float[]> smag_, sfrq_, stmp_;  // 1024 each (partials scratch)
  std::unique_ptr<cpx[]> sy_;        // 2048 (mask / vocode analysis)
  int olaPos_ = 512, fmPos_ = 0;
  double specPos_ = 0;
  bool specStarted_ = false;
};
// sub / noise
struct SubVoice {
  double ph = 0;
  float tick(int shape, double inc);
};
struct NoiseVoice {
  double pos = 0;
  float lp = 0;
  float tz[2][2] = {};  // tone filter state (L / R)
  bool done = false;
  // tone filter (coefficients b0 b1 b2 a1 a2 from the control block), transposed direct form II
  float tone(int ch, float x, const float* k) {
    float* z = tz[ch];
    const float y = k[0] * x + z[0];
    z[0] = k[1] * x - k[3] * y + z[1];
    z[1] = k[2] * x - k[4] * y;
    return y;
  }
  void start(const NoiseSrc* n, const Sample* file, float phase, float rnd, uint32_t seed);
  void tick(const NoiseSrc* n, const Sample* file, double rate, bool oneShot, float color, float& l, float& r);
};

// ------------------------------------------------------------------ helpers
inline float fastTanh(float x) {
  if (x > 4.97f) return 1.f;
  if (x < -4.97f) return -1.f;
  const float x2 = x * x;
  return x * (27.f + x2) / (27.f + 9.f * x2);
}
inline uint32_t rngNext(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
inline float rngUni(uint32_t& s) { return static_cast<float>(rngNext(s) >> 8) * (1.0f / 16777216.0f); }  // 0..1
inline float rngBi(uint32_t& s) { return rngUni(s) * 2.f - 1.f; }
// Distortion shapers shared by the warp menu and the Distortion module (mode = index in the 18-mode list).
float shapeSample(int mode, float x, float drive, float bias, const float* curveA, const float* curveB, float& state);
// Index into the Distortion module's 18 modes for warp distortion entries (warp 19..32).
int warpDistMode(int warp);

}  // namespace gl::packs::plasma

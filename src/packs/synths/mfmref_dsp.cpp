// Gridline — "synths" pack: Matrix FM reference model, engine (mfmref.h).
//
// Structure: a fixed voice pool (unison voices included), per-voice operator
// / filter / articulator state, and the engine buses. Wave tables, filter and
// shaper tables, effect lines and the band-limited table slots are built in
// makeEngine; voices render at the oversampled rate in control-period chunks
// (articulators tick once per chunk), then the decimators, EQ, chorus,
// delays and reverb run on the 256-frame block.
//
// Behavioural constants (level laws, filter / unison / chorus tables, the
// half-band coefficients) are numbers the engine was measured to use; the
// implementation is Gridline's own.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include "mfmref.h"
#include "mfmref_env.h"
#include "mfmref_fx.h"

namespace gl::packs::synths::mfmref {

namespace {

using cplx = std::complex<double>;
using ld = long double;

constexpr double kLn2 = 0.69314718055994530942;
constexpr double kPi = 3.14159265358979323846;
const double kCentsLn = kLn2 / 1200.0;
const float kLn11f = 2.39789534f;  // ln 11 (float)

inline bool isZeroBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u == 0;
}

// Level law: x = v / den; |x| == 1 → 1, else (11^|x| − 1) / 10, sign kept when `sign`.
float levelLaw(int32_t v, float den, bool sign) {
  const float x = static_cast<float>(v) * den;
  const float ax = std::fabs(x);
  float l;
  if ((sign ? ax : x) == 1.0f) l = 1.0f;
  else l = static_cast<float>(0.1f * (std::exp(static_cast<double>((sign ? ax : x) * kLn11f)) - 1.0));
  if (!sign) return l;
  return x == 0 ? 0.0f : (x < 0 ? -l : l);
}

// Radix-2 complex FFT (in place) with the twiddles of one size precomputed.
struct Fft {
  int n = 0, sign = -1;
  std::vector<cplx> tw;  // stage by stage: len / 2 factors each
  void init(int size, int sg) {
    n = size, sign = sg;
    tw.clear();
    for (int len = 2; len <= n; len <<= 1) {
      const double ang = 2 * kPi / static_cast<double>(len) * sign;
      for (int k = 0; k < len / 2; ++k) tw.push_back(std::polar(1.0, ang * static_cast<double>(k)));
    }
  }
  void run(cplx* a) const {
    const size_t N = static_cast<size_t>(n);
    for (size_t i = 1, j = 0; i < N; ++i) {
      size_t bit = N >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) std::swap(a[i], a[j]);
    }
    size_t base = 0;
    for (size_t len = 2; len <= N; len <<= 1) {
      const cplx* w = tw.data() + base;
      for (size_t i = 0; i < N; i += len)
        for (size_t k = 0; k < len / 2; ++k) {
          const cplx u = a[i + k], v = a[i + k + len / 2] * w[k];
          a[i + k] = u + v, a[i + k + len / 2] = u - v;
        }
      base += len / 2;
    }
  }
};

template <typename T, int N>
struct Small {
  T v[N];
  int n = 0;
  void clear() { n = 0; }
  void push(const T& x) {
    if (n < N) v[n++] = x;
  }
  bool empty() const { return n == 0; }
  T* begin() { return v; }
  T* end() { return v + n; }
  const T* begin() const { return v; }
  const T* end() const { return v + n; }
};

// ------------------------------------------------------------------ engine data
struct BandSlots {  // band-limited tables of one operator (shared by its voices)
  static constexpr int kSlots = 48;
  std::vector<float> mem;  // kSlots × (kBandLen + 1)
  int h1[kSlots], h2[kSlots], refs[kSlots];
  uint64_t used[kSlots];
  bool on = false;
};

struct OpTables {
  std::vector<float> base;  // kTableLen + 1
  std::vector<cplx> spectrum;
  bool gibbs = false;
};

struct ArtRt {
  int mode = 0;  // 0 multiply, 1 add
  float base = 0, envAmt = 1, lfoAmt = 1;
  env::Env env, lfo, maps[6];
  bool envOn = false, lfoOn = false;
  bool mapFlat[6] = {};
  bool envTempo = false, lfoTempo = false;
  std::vector<float> tableX, tableY;  // 513-entry mod X / Y tables (empty when flat)
};

struct ArtState {
  env::State env, lfo;
  float stat = 0;
  float v = 0;
};

struct UnitRt {
  float pan = 0, fx = 0, out = 0;
  float vol = 1;
  float pitchScale = 0;
  float phase = 0;
  float freqOfs = 0;
  double ratioInc = 0;
  std::vector<ArtRt> arts;
  OpTables tables;
  bool hasTables = false;
  bool bandLimit = false, pluck = false;
  int ftype = 0, stagesRaw = 0;
  bool hq = false, norm = false;
  float cutBase = 0, resBase = 0, mix[3] = {}, toNext = 0, wsMix = 1;
  int gainRaw = 0;
  float wsGain = 1;
  bool wsOn = false, wsBip = false;
  int wsLen = 0;
  std::vector<int16_t> wsTable;
};

struct OpVoice {
  double phase = 0;
  float pmIn = 0, rmAcc = 0, inc = 0;
  float freqBase = 0, pitchMul = 1, freq = 0, phaseInc = 0, prevPhaseArt = 0;
  float vol = 0, volInc = 0, mod = 0, modInc = 0;
  float noiseAmt = 0, noiseOfs = 0, noiseKeep = 1;
  const float* table = nullptr;
  int slot = -1;  // band-limited table slot held
  int len = 0, mask = 0;
  float invLen = 0;
  bool hasMod = false;
  bool pluck = false;
  float* pbuf = nullptr;
  int plen = 0, ppos = 0;
  float pc1 = 0, pc2 = 0, pz = 0, pDamp = 0, pLast = 2147483648.0f, pComp = 0;
  uint32_t pFrac = 0, pFracInc = 0, pIntInc = 0, pExtra = 1;
  float pY = 0, pPrev = 0;
  bool active = false;
  float* out = nullptr;
  bool center = false;
  float cf = 0, cq = 0;
  double cy = 0, cv = 0;
  int smoothLeft = -1;
  float smoothCoef = 0, smoothInc = 0, smoothY = 0;
  bool smooth = false;
  struct Fm {
    float* dst;
    float amt;
  };
  struct Rm {
    float* dst;
    float amt, c;
  };
  Small<Fm, kOps> fm;
  Small<Rm, kOps> rm;
};

struct FltStage {
  float f = 0, q = 0, finc = 0, qinc = 0, fcur = 0.5f, qcur = 1;
  double st[8] = {};
  fx::Biquad bq;
  float lz[5] = {};
  double lp = 0, ldrive = 0;
  float lk = 0, lg = 0;
};

struct FltVoice {
  bool active = false;
  struct Src {
    int unit;
    float gain;
  };
  Small<Src, 9> src;
  float volStart = 0, volInc = 0, volCur = 0;
  float gStart[3] = {}, gInc[3] = {}, gCur[3] = {};
  int last = -1;
  bool spread = false;
  float expo = 1, normRef = 1, bwScale = 1, gainA = 0;
  float wsTarget = 0, wsStart = 0, wsInc = 0, wsCur = 0;
  int apCount = 0;
  float apA = 0, apFb = 0, apY = 0;
  float apS[64] = {};
  FltStage stg[8];
};

struct UnitVoice {
  std::array<ArtState, 8> arts;
  std::array<float, 8> cur{};
  Small<int, 8> activeArts;
  float gL = 0, gR = 0, cL = 0, cR = 0;
  float fL = 0, fR = 0, fcL = 0, fcR = 0;
  OpVoice op;
  FltVoice flt;
};

struct Voice {
  uint32_t id = 0;
  int state = 11;  // 1 playing, 5 released, 9 releasing, 10 silent, 11 free, 12 left-over unison voice
  int untilTick = 0;
  bool firstBlock = true;
  float pitch = 0;
  double key = 60;
  float vel = 0;
  float flVol = 1, flPan = 0, flCut = 0, flRes = 0;
  float gain = 1;
  float uniPan = 0.5f, uniVol = 1, uniPitch = 1, uniPos = 0.5f;
  float envVar = 0.0078125f, phaseOfs = 0, uniGain = 1;
  int role = 0, bufOfs = 0;
  Small<Voice*, 18> chain;
  int idxX = 256, idxY = 256;
  float modX = 0, modY = 0;
  int keyCents = 0;      // the note's key (step envelopes order the held keys)
  bool chained = false;  // an extra unison voice
  std::array<UnitVoice, kUnits> units;
  Small<int, kOps> modOps, plainOps;
  Small<int, 9> outList, fxList;
  float* pluckMem[kOps] = {};  // string-loop buffers (string-loop operators only)
};

struct VoiceInit {
  uint32_t id = 0;
  float flVol = 1, flPan = 0, flCut = 0, flRes = 0, pitch = 0, key = 60, vel = 0;
  float uniPan = 0.5f, uniVol = 1, uniPitch = 1, uniPos = 0.5f, envVar = 0.0078125f, phaseOfs = 0, uniGain = 1;
  int role = 0, bufOfs = 0;
  bool porta = false;
  float portaFrom = 0;
  int keyCents = 0;
  bool chained = false;
};

constexpr int kPluckCap = 16384;

}  // namespace

struct Engine {
  Patch patch;
  int sr = 44100, hostSr = 44100, os = 0, ratio = 1, fxRatio = 1, fxStages = 0, fxSr = 44100;
  int tick = 96;
  double tickRatio = 1;
  double tempo = 120;
  float e8 = 0, mainVol = 1, lfoLevel = 1;
  int masterPitch = 0;
  float modXg = 0, modYg = 0;
  float modSmooth = 0;
  float maxStepMix = 0.001f, maxStepUnit = 0.01f;
  fx::Decimator decDry, decFx;
  UnitRt units[kUnits];
  std::vector<Voice> voices;
  int order[kMaxVoices] = {};  // live voices, creation order
  int nOrder = 0;
  uint64_t serial = 0;
  uint32_t noiseSeed = 0, rnd = 0;
  std::vector<float> bufs[16];
  std::vector<float> dry, fxb, tmp, mono, out;
  fx::Delay delays[3];
  fx::Chorus chorus;
  fx::Reverb reverb;
  bool fxToFx = false;
  fx::EqBand eq[3];
  int eqMode = 0;
  float fxPan = 0;
  bool fxVoices = false;
  int fxCount = 0, fxTail = 100;
  double samplesTotal = 0;
  std::vector<float> svfTable[2];
  int softenInt = 0;
  double dcY[2] = {}, dcV[2] = {};
  float declick = 96;
  bool hasLast = false;
  float lastPitch = 0;
  bool waitArts = false, stepDirty = false;  // step envelopes present / held-key steps to refresh
  int mainVoices = 0;
  int stepKeys[kMaxVoices] = {};
  BandSlots band[kOps];
  Fft bandFft;
  std::vector<cplx> bandY;
  std::vector<double> bandW;
  std::vector<float> pluckPool;
  int64_t clock = 0;
  int readPos = kBlock;  // frames of `out` already handed to the host
  int64_t quiet = 0;
  bool anyFx = false;
};

void EngineDeleter::operator()(Engine* e) const { delete e; }

namespace {

// ------------------------------------------------------------------ tables
using Bend = env::Bend;

void buildTables(OpTables& t, const Unit& U, bool ringing) {
  const int N = kTableLen;
  const int shape = U.tail[0], tension = U.tail[1], skew = U.tail[2], sine = U.tail[3];
  const bool half = U.flags[0] != 0, even = U.flags[1] != 0;
  const bool pure = shape == 0 && skew == 0 && sine == 0 && !half && !even;
  std::vector<float> table(static_cast<size_t>(N) + 1, 0.f);
  if (pure) {
    for (int h = 0; h < 256; ++h) {
      const float lv = U.lv[static_cast<size_t>(h)];
      if (lv == 0) continue;
      double ph = static_cast<double>(U.ph[static_cast<size_t>(h)] * 2.0f) + 1.5;
      if (ph >= 2.0) ph -= 2.0;
      const float phase = static_cast<float>(ph * kPi);
      const float rel = static_cast<float>(static_cast<double>((h + 1) * 2) / static_cast<double>(N) * 0.5);
      for (int i = 0; i < N; ++i)
        table[static_cast<size_t>(i)] +=
            static_cast<float>(static_cast<double>(lv) * std::cos(2 * kPi * static_cast<double>(rel) * static_cast<double>(i) + static_cast<double>(phase)));
    }
  } else {
    // shaped partials, evaluated per sample from a phase running 0..2 per cycle
    float sh = static_cast<float>(shape) * 0.0078125f, w0 = 1.0f, w1 = 0.0f, w2 = 0.0f;
    if (1.0f < sh) {
      if (sh <= 2.0f) {
        w0 = 2.0f - sh;
      } else {
        w0 = 0.0f;
        if (sh <= 3.0f) w1 = sh - 2.0f;
        else w1 = 1.0f, w2 = sh - 3.0f;
      }
      sh = 1.0f;
    }
    const float k = static_cast<float>(static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(skew) * 0.0078125f) * 3.044522437723423) - 1.0)) * 0.050000000000000003);
    const Bend skewC = env::makeBend(static_cast<float>(std::exp(static_cast<double>(k) * 20.72326583694641) - 1.0));
    const double sineK = sine > 0 ? static_cast<double>(static_cast<float>(static_cast<double>(sine) * 3.1415926535897931 * 0.01171875)) : 0.0;
    auto wave = [&](double x) -> double {
      if (w2 <= 0.0f) {
        double v = std::sin(x * 3.1415926535897931);
        if (0.0f < sh) {
          double d = static_cast<double>(w0 * 0.5f) + x;
          while (2.0 < d) d -= 2.0;
          double t;
          if (static_cast<double>(w0) <= d) {
            t = 1.0 - static_cast<double>(2.0f / (2.0f - w0)) * (d - static_cast<double>(w0));
            if (w1 != 0.0f) {
              if (1.0 <= d) t = static_cast<double>(1.0f - w1) * t + static_cast<double>(-w1);
              else t = static_cast<double>(1.0f - w1) * t + static_cast<double>(w1);
            }
          } else {
            t = (d / static_cast<double>(w0)) * 2.0 - 1.0;
          }
          v = static_cast<double>(sh) * t + static_cast<double>(1.0f - sh) * v;
        }
        return v;
      }
      const double d = static_cast<double>(1.0f - w2 * 0.75f);
      return d <= x ? -1.0 : 1.0;
    };
    for (int h = 0; h < 256; ++h) {
      const double lv = static_cast<double>(U.lv[static_cast<size_t>(h)]);
      if (lv == 0.0) continue;
      double x = static_cast<double>(U.ph[static_cast<size_t>(h)] * 2.0f);
      double step = static_cast<double>((h + 1) * 2) / static_cast<double>(N), period = 2.0;
      if (half) step = step * 0.5, period = 2.0 * 0.5;
      const double lim = period;
      if (even) period = period * 2.0, step = step * 2.0;
      if (!std::isfinite(x)) x = 0.0;
      if (x > period) x = std::fmod(x, period);
      for (int i = 0; i < N; ++i) {
        while (period < x) x -= period;
        double a;
        if (x <= lim) {
          if (skew < 1) {
            a = x;
          } else {
            double b = x <= 1.0 ? 1.0 - x : x - 1.0;
            b = std::fabs(b);
            const double v = env::bendAt(skewC, b);
            a = x <= 1.0 ? 1.0 - v : v + 1.0;
          }
          a = wave(a);
        } else {
          a = 0.0;
        }
        if (sine > 0) a = std::sin(sineK * a);
        table[static_cast<size_t>(i)] = static_cast<float>(a * lv + static_cast<double>(table[static_cast<size_t>(i)]));
        x += step;
      }
    }
  }
  float peak = 0;
  for (int i = 0; i < N; ++i) peak = std::max(peak, std::fabs(table[static_cast<size_t>(i)]));
  if (peak > 5.96046448e-08f) {
    const float s = 1.0f / peak;
    for (int i = 0; i < N; ++i) table[static_cast<size_t>(i)] *= s;
  }
  if (tension != 0) {
    const int at = tension < 0 ? -tension : tension;
    const float c0 = static_cast<float>(static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(at) * 0.0078125f) * 3.044522437723423) - 1.0)) * 0.050000000000000003);
    const float sg = tension < 0 ? -1.0f : 1.0f;
    const Bend tc = env::makeBend(static_cast<float>(std::exp(static_cast<double>(c0) * 41.446531658199575) - 1.0) * sg);
    for (int i = 0; i < N; ++i) {
      const float o = table[static_cast<size_t>(i)];
      float y = static_cast<float>(env::bendAt(tc, static_cast<double>(std::fabs(o))));
      if (o < 0.0f) y = -y;
      table[static_cast<size_t>(i)] = y;
    }
  }
  if (U.flags[2])
    for (int i = 0; i < N; ++i) table[static_cast<size_t>(i)] = std::fabs(table[static_cast<size_t>(i)]);
  // Pre-filter: harmonics from the cutoff up removed (+ ringing attenuation).
  Fft fwd, inv;
  fwd.init(N, -1);
  std::vector<cplx> X(static_cast<size_t>(N));
  for (int i = 0; i < N; ++i) X[static_cast<size_t>(i)] = table[static_cast<size_t>(i)];
  fwd.run(X.data());
  const float f = static_cast<float>(U.tail[4]) * 0.0078125f;
  const float cutf = static_cast<float>(static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(static_cast<double>(f) * 0.35 + 0.65)) * 23.025850929940457) - 1.0)) * 1.6384e-06);
  const int cut = static_cast<int>(std::nearbyint(cutf));
  t.spectrum.assign(static_cast<size_t>(N) / 2 + 1, {});
  for (int k = 0; k <= N / 2; ++k) t.spectrum[static_cast<size_t>(k)] = X[static_cast<size_t>(k)] / static_cast<double>(N);
  if (cut <= N / 2) {
    for (int k = std::max(cut, 0); k <= N / 2; ++k) t.spectrum[static_cast<size_t>(k)] = 0;
    inv.init(N, +1);
    std::vector<cplx> Y(static_cast<size_t>(N));
    for (int k = 0; k <= N / 2; ++k) {
      double g = 1.0;
      if (ringing && cut - 1 > 0 && k >= 1 && k <= cut - 1) {
        const double c = cut - 1;
        g = std::sin(k * (kPi / c)) * (c / kPi) / k;
      }
      Y[static_cast<size_t>(k)] = t.spectrum[static_cast<size_t>(k)] * g;
      if (k > 0 && k < N / 2) Y[static_cast<size_t>(N - k)] = std::conj(Y[static_cast<size_t>(k)]);
    }
    inv.run(Y.data());
    for (int i = 0; i < N; ++i) table[static_cast<size_t>(i)] = static_cast<float>(Y[static_cast<size_t>(i)].real());
  }
  for (int i = 0; i < N; ++i)
    if (std::fabs(table[static_cast<size_t>(i)]) < 5.96046448e-08f) table[static_cast<size_t>(i)] = 0;
  table[static_cast<size_t>(N)] = table[0];
  t.base = std::move(table);
  t.gibbs = ringing;
}

// ------------------------------------------------------------------ articulators
double unitsPerTick(const Engine& e, bool tempoBased) {
  const double secPerTick = e.tickRatio * 96.0 / 44100.0;
  return tempoBased ? secPerTick * e.tempo / 60.0 : secPerTick * 2.0;
}

ArtRt prepareArt(const Engine& e, const Art& a, int mode, bool clamp) {
  ArtRt r;
  r.mode = mode;
  r.base = a.base, r.envAmt = a.envAmt, r.lfoAmt = e.lfoLevel;  // the global LFO level replaces every LFO amount
  const double centre = mode == 1 ? 0.5 : 0.0;
  r.envTempo = (a.env.flags & 1) != 0, r.lfoTempo = (a.lfo.flags & 1) != 0;
  r.env = env::prepare(a.env, 1, centre, unitsPerTick(e, r.envTempo), clamp);
  r.lfo = env::prepare(a.lfo, 2, 0.5, unitsPerTick(e, r.lfoTempo), clamp);
  for (int m = 0; m < 6; ++m) {
    r.maps[m] = env::prepare(a.maps[static_cast<size_t>(m)], 3, centre, 1.0, clamp);
    r.mapFlat[m] = env::mapFlat(r.maps[m], mode == 1 ? 0.5 : 1.0);
  }
  r.envOn = r.env.enabled;
  r.lfoOn = r.lfo.enabled;
  for (int m : {2, 3}) {
    if (r.mapFlat[m]) continue;
    auto& t = m == 2 ? r.tableX : r.tableY;
    t.resize(513);
    for (int i = 0; i <= 512; ++i) t[static_cast<size_t>(i)] = env::mapAt(r.maps[m], static_cast<double>(static_cast<float>(i) * 0.001953125f));
  }
  return r;
}

inline float combine(int mode, float a, float b) { return mode == 1 ? a + b : (mode == 0 ? a * b : a * b * 2.0f); }

void unitLive(Engine& e, int u) {
  const Patch& p = e.patch;
  UnitRt& R = e.units[u];
  const Unit& U = p.units[u];
  auto cell = [&](int layer, int row, int col) -> int32_t {
    if (p.mutes[layer * 81 + row * 9 + col]) return 0;
    return p.main[3 + layer * 81 + row * 9 + col];
  };
  if (u < 9) {
    R.pan = static_cast<float>(static_cast<double>(cell(0, u, 6)) * 0.001953125);
    R.fx = levelLaw(cell(0, u, 7), 0.00390625f, true);
    R.out = levelLaw(cell(0, u, 8), 0.00390625f, true);
  }
  if (u < kOps) R.vol = static_cast<float>(U.u[1]) * 0.00390625f;
  if (u >= kOps && u < 9) {
    R.cutBase = static_cast<float>(U.u[2]) * 0.00390625f, R.resBase = static_cast<float>(U.u[3]) * 0.00390625f;
    for (int i = 0; i < 3; ++i) R.mix[i] = levelLaw(U.u[4 + i], 0.0078125f, false);
    R.toNext = levelLaw(U.u[9], 0.00390625f, true);
    R.wsMix = static_cast<float>(U.u[8]) * 0.0078125f;
    R.gainRaw = U.u[4];
    R.wsGain = static_cast<float>(U.u[7]) * 0.0078125f;
  }
}

// ------------------------------------------------------------------ band-limited tables
void releaseSlot(Engine& e, int u, OpVoice& o) {
  if (o.slot >= 0) {
    BandSlots& B = e.band[u];
    if (B.refs[o.slot] > 0) --B.refs[o.slot];
    o.slot = -1;
  }
}

const float* selectTable(Engine& e, int u, float freqCycles, int& len, int& slotOut) {
  UnitRt& R = e.units[u];
  OpTables& T = R.tables;
  slotOut = -1;
  int h1 = 0x7fffffff, h2 = 0x7fffffff;
  if (!R.pluck && R.bandLimit) {
    float hz = static_cast<float>(e.sr) * freqCycles;
    if (hz <= 0.1f) hz = 0.1f;
    const float inv = 1.0f / hz;
    const double ny = e.patch.opt[2] == 0 ? static_cast<double>(static_cast<float>(e.sr) * 0.5f) : 22050.0;
    const double d = 0.25 * (ny - 18000.0);
    h1 = static_cast<int>(static_cast<long>((18000.0 + d) * static_cast<double>(inv))) + 1;
    h2 = static_cast<int>(static_cast<long>((d * 3.0 + 18000.0) * static_cast<double>(inv))) + 1;
  }
  const int M = kBandLen;
  BandSlots& B = e.band[u];
  if (h1 > M / 2 || !B.on) {
    len = kTableLen;
    return T.base.data();
  }
  if (h2 > M / 2) h2 = M / 2;
  len = M;
  const uint64_t now = ++e.serial;
  int pick = -1;
  for (int s = 0; s < BandSlots::kSlots; ++s)
    if (B.h1[s] == h1 && B.h2[s] == h2) {
      pick = s;
      break;
    }
  if (pick < 0) {
    for (int s = 0; s < BandSlots::kSlots; ++s)
      if (B.refs[s] == 0 && (pick < 0 || B.used[s] < B.used[pick])) pick = s;
    if (pick < 0) {  // every slot in use: the full table (inexact, never seen in practice)
      len = kTableLen;
      return T.base.data();
    }
    std::vector<double>& w = e.bandW;
    std::fill(w.begin(), w.end(), 0.0);
    for (int k = 0; k < h1 && k <= M / 2; ++k) w[static_cast<size_t>(k)] = 1.0;
    if (h1 <= h2) {
      const int cnt = h2 - h1 + 1;
      const float slope = static_cast<float>(-1.0 / static_cast<double>(cnt));
      const double off = static_cast<double>(1.0f + slope);
      for (int i = 0; i < cnt && h1 + i <= M / 2; ++i) w[static_cast<size_t>(h1 + i)] = static_cast<float>(off + static_cast<double>(slope) * static_cast<double>(i));
    }
    if (T.gibbs && h2 > 0) {
      const float a = static_cast<float>(static_cast<double>(h2) * 0.31830988618379069), b = static_cast<float>(kPi / static_cast<double>(h2));
      for (int k = 1; k <= h2 && k <= M / 2; ++k)
        w[static_cast<size_t>(k)] = static_cast<float>((std::sin(static_cast<double>(static_cast<float>(k) * b)) * static_cast<double>(static_cast<float>(w[static_cast<size_t>(k)] * a))) / static_cast<double>(k));
    }
    cplx* Y = e.bandY.data();
    std::fill(e.bandY.begin(), e.bandY.end(), cplx{});
    for (int k = 0; k <= M / 2; ++k) {
      Y[k] = T.spectrum[static_cast<size_t>(k)] * w[static_cast<size_t>(k)];
      if (k > 0 && k < M / 2) Y[M - k] = std::conj(Y[k]);
    }
    e.bandFft.run(Y);
    float* tab = B.mem.data() + static_cast<size_t>(pick) * static_cast<size_t>(M + 1);
    for (int i = 0; i < M; ++i) {
      float x = static_cast<float>(Y[i].real());
      if (std::fabs(x) < 5.96046448e-08f) x = 0;
      tab[i] = x;
    }
    tab[M] = tab[0];
    B.h1[pick] = h1, B.h2[pick] = h2;
  }
  B.used[pick] = now;
  ++B.refs[pick];
  slotOut = pick;
  return B.mem.data() + static_cast<size_t>(pick) * static_cast<size_t>(M + 1);
}

// ------------------------------------------------------------------ voice helpers
void artStart(const Engine& e, const ArtRt& r, ArtState& s, const env::State* shared) {
  env::start(r.env, s.env, shared);
  // global LFOs share one free-running phase: control ticks since the engine started
  env::lfoStart(r.lfo, s.lfo, e.samplesTotal / static_cast<double>(e.tick));
  s.v = r.base;
  if (r.envOn) s.v = s.env.out * r.envAmt + s.v;
  if (r.lfoOn) {
    if (r.mode == 1) s.v = s.lfo.lfoOut * r.lfoAmt + s.v;
    else s.v = (s.lfo.lfoOut * r.lfoAmt * 2.0f + 1.0f) * s.v;
  }
}

void artTick(const ArtRt& r, ArtState& s, double dtFlag) {
  if (r.envOn) {
    env::tick(r.env, s.env, dtFlag);
    if (r.mode == 1) s.v = s.env.out * r.envAmt + s.v;
    else s.v = s.v * s.env.out * r.envAmt;
  }
  if (r.lfoOn) {
    env::lfoTick(r.lfo, s.lfo, dtFlag);
    if (r.mode == 1) s.v = s.lfo.lfoOut * r.lfoAmt + s.v;
    else s.v = (s.lfo.lfoOut * r.lfoAmt * 2.0f + 1.0f) * s.v;
  }
}

void rampTo(float target, float maxStep, float& inc, float& cur, int n) {
  inc = target - cur;
  if (inc != 0.0f) {
    inc = inc / static_cast<float>(n);
    if (std::fabs(inc) > maxStep) inc = std::copysign(maxStep, inc);
  }
}

// Subtract-with-borrow + congruential uniform generator (the string loop's exciter noise).
void uniformNoise(float* out, int n, float lo, float hi, uint32_t seed) {
  const double scale = static_cast<double>(hi - lo) * 2.3283064365387e-10, ofs = static_cast<double>(hi + lo) * 0.5;
  int32_t A = static_cast<int32_t>(seed);
  int32_t B = static_cast<int32_t>(static_cast<uint32_t>(A) * 69069u + 0x3c6ef373u);
  int32_t C = static_cast<int32_t>(static_cast<uint32_t>(B) * 69069u + 0x3c6ef373u);
  int32_t br = -1;
  uint32_t L = 0x436cbae9u;
  int i = 0;
  auto sub = [](int32_t a, int32_t b, int32_t c) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b) + static_cast<uint32_t>(c)); };
  for (; i + 1 < n; i += 2) {
    const int32_t t1 = sub(B, C, br), b1 = t1 >> 31, x1 = t1 - (b1 & 18);
    const uint32_t L1 = L * 69069u + 0x3c6ef373u;
    const int32_t t2 = sub(A, B, b1), b2 = t2 >> 31, x2 = t2 - (b2 & 18);
    const uint32_t L2 = L1 * 69069u + 0x3c6ef373u;
    out[i] = static_cast<float>(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(x1) + L1)) * scale + ofs);
    out[i + 1] = static_cast<float>(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(x2) + L2)) * scale + ofs);
    C = A, B = x1, A = x2, br = b2, L = L2;
  }
  if (i < n) {
    const int32_t t1 = sub(B, C, br), b1 = t1 >> 31, x1 = t1 - (b1 & 18);
    const uint32_t L1 = L * 69069u + 0x3c6ef373u;
    out[i] = static_cast<float>(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(x1) + L1)) * scale + ofs);
  }
}

// String loop: damping, loop-delay compensation and read speed from the current frequency.
void pluckTune(OpVoice& o) {
  o.pc1 = static_cast<float>(static_cast<double>(o.pDamp) * 0.98999999999999999 + 0.01);
  o.pc2 = 1.0f - o.pc1;
  if (o.pLast != o.pc2) {
    o.pLast = o.pc2;
    const double a = 1.0 / static_cast<double>(o.plen);
    const double sn = std::sin(a), cs = std::cos(a);
    o.pComp = static_cast<float>(((static_cast<double>(o.pLast) * sn) / (1.0 - static_cast<double>(o.pLast) * cs) + 1.0) * static_cast<double>(o.plen));
  }
  float f = o.freq;
  if (!(f > 0.0f)) f = 0.0f;
  const ld x = static_cast<ld>(static_cast<double>(f * o.pComp)) * static_cast<ld>(4294967296.0f);
  int64_t q;
  if (!(x < 9.2e18L)) q = INT64_MIN;  // out of range: the converter's "indefinite" value
  else q = static_cast<int64_t>(std::nearbyint(x));
  o.pIntInc = static_cast<uint32_t>(static_cast<uint64_t>(q) >> 32);
  o.pFracInc = static_cast<uint32_t>(static_cast<uint64_t>(q));
  if (o.pIntInc > static_cast<uint32_t>(o.plen) * 4u) o.pIntInc = static_cast<uint32_t>(o.plen) * 4u;  // bounded work per sample
}

void pluckStart(Engine& e, int u, OpVoice& o, float art5, float* mem) {
  const Unit& U = e.patch.units[u];
  long len = static_cast<long>(std::nearbyint(88200.0f / (static_cast<float>(e.sr) * o.freqBase)));
  if (len < 2) len = 2;
  if (len > kPluckCap) len = kPluckCap;
  o.plen = static_cast<int>(len);
  o.pLast = 2147483648.0f;
  o.pDamp = art5;
  o.pbuf = mem;
  pluckTune(o);
  std::fill(o.pbuf, o.pbuf + o.plen, 0.f);
  float keep = 0.0f, mix = 0.0f;
  if (U.noise != 0) {
    const float lvl = levelLaw(U.noise, 0.0078125f, false);
    keep = 1.0f - lvl;
    mix = lvl * 1.5f;
  }
  const ld ph = static_cast<ld>(o.phase) * 4294967296.0L;
  const uint32_t pos0 = std::isfinite(static_cast<double>(ph)) ? static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(std::nearbyint(ph)))) : 0u;
  if (mix != 0.0f) uniformNoise(o.pbuf, o.plen, -1.0f, 1.0f, pos0);
  const float* tbl = e.units[u].tables.base.data();
  const float step = 4294967296.0f / o.pComp;
  const uint32_t istep = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(std::nearbyint(step))));
  uint32_t pos = pos0;
  float sum = 0.0f;
  for (int i = 0; i < o.plen; ++i) {
    const float t0 = tbl[pos >> 18];
    const float t1 = tbl[static_cast<uint32_t>(pos + 0x40000u) >> 18];
    float f = static_cast<float>(static_cast<double>(static_cast<float>(pos & 0x3ffffu) * (t1 - t0)) * 3.814697265625e-06 + static_cast<double>(t0));
    if (mix != 0.0f) f = o.pbuf[i] * mix + f * keep;
    o.pbuf[i] = f;
    sum = sum + f;
    pos += istep;
  }
  const float dc = sum / static_cast<float>(o.plen);
  for (int i = 0; i < o.plen; ++i) o.pbuf[i] = o.pbuf[i] - dc;
  o.pz = o.pbuf[o.plen - 1];
  o.pFrac = 0, o.pExtra = 1, o.pY = 0, o.pPrev = 0, o.ppos = 0;
}

void freeVoice(Engine& e, Voice& v) {
  for (int u = 0; u < kOps; ++u) releaseSlot(e, u, v.units[u].op);
  if (v.state < 11 && !v.chained && e.mainVoices > 0) --e.mainVoices;
  v.state = 11;
}

int voiceIndex(const Engine& e, const Voice* v) { return static_cast<int>(v - e.voices.data()); }

// Removes free voices from the creation-order list.
void compactOrder(Engine& e) {
  int w = 0;
  for (int i = 0; i < e.nOrder; ++i)
    if (e.voices[static_cast<size_t>(e.order[i])].state != 11) e.order[w++] = e.order[i];
  e.nOrder = w;
}

Voice* allocVoice(Engine& e) {
  for (auto& v : e.voices)
    if (v.state == 11) return &v;
  // Left-over unison voices of finished notes: the instrument keeps them in its
  // list (they keep the effects running) until their slot is needed.
  for (int i = 0; i < e.nOrder; ++i) {
    Voice& v = e.voices[static_cast<size_t>(e.order[i])];
    if (v.state != 12) continue;
    v.state = 11;
    compactOrder(e);
    return &v;
  }
  // pool full: the oldest main voice (and its unison chain) goes
  for (int i = 0; i < e.nOrder; ++i) {
    Voice& v = e.voices[static_cast<size_t>(e.order[i])];
    if (v.role >= 1) continue;
    if (v.role < 0)
      for (Voice* sv : v.chain) freeVoice(e, *sv);
    freeVoice(e, v);
    compactOrder(e);
    return &v;
  }
  return nullptr;
}

Voice* spawnVoice(Engine& e, const VoiceInit& vi) {
  Voice* vp = allocVoice(e);
  if (!vp) return nullptr;
  Voice& v = *vp;
  float* pluckMem[kOps];
  std::memcpy(pluckMem, v.pluckMem, sizeof pluckMem);
  v.~Voice();
  new (&v) Voice();
  std::memcpy(v.pluckMem, pluckMem, sizeof pluckMem);
  v.state = 1;
  v.id = vi.id;
  v.flVol = vi.flVol, v.flPan = vi.flPan, v.flCut = vi.flCut, v.flRes = vi.flRes;
  v.pitch = vi.pitch;
  v.key = vi.key;
  v.vel = vi.vel;
  v.uniPan = vi.uniPan, v.uniVol = vi.uniVol, v.uniPitch = vi.uniPitch, v.uniPos = vi.uniPos;
  v.envVar = vi.envVar, v.phaseOfs = vi.phaseOfs, v.uniGain = vi.uniGain;
  v.role = vi.role, v.bufOfs = vi.bufOfs;
  v.untilTick = -e.tick;
  v.keyCents = vi.keyCents;
  v.chained = vi.chained;
  v.gain = v.flVol * e.mainVol;
  v.modX = e.modXg, v.modY = e.modYg;
  v.idxX = std::clamp(static_cast<int>(std::lrint(v.flCut * 256.0f + v.modX)), 0, 512);
  v.idxY = std::clamp(static_cast<int>(std::lrint(v.flRes * 256.0f + v.modY)), 0, 512);
  // articulators
  for (int u = 0; u < kUnits; ++u) {
    UnitRt& R = e.units[u];
    UnitVoice& UV = v.units[u];
    for (size_t a = 0; a < R.arts.size(); ++a) {
      const ArtRt& r = R.arts[a];
      ArtState& s = UV.arts[a];
      // shared envelope: starts from the first held voice's state (and control-tick phase)
      const env::State* shared = nullptr;
      if (r.envOn && (r.env.flags & 2))
        for (int i = 0; i < e.nOrder; ++i) {
          const Voice& o = e.voices[static_cast<size_t>(e.order[i])];
          if (o.state < 5) {
            shared = &o.units[u].arts[a].env;
            v.untilTick = -std::abs(o.untilTick);
            break;
          }
        }
      artStart(e, r, s, shared);
      if (r.env.hasWait || r.lfo.hasWait) e.waitArts = true;
      // envelope macros (volume group: every unit's VOL articulator; cut group: the filters' CUT / RES)
      int grp = -1;
      if (a == 1) grp = 0;
      else if ((a == 2 || a == 3) && u >= kOps && u < 9) grp = 1;
      if (grp >= 0 && r.envOn && !shared) {
        const float factor = (r.env.flags & 2) ? 0.0078125f : v.envVar;
        float* sc = s.env.scale;
        const int32_t* m = &e.patch.main[165 + 4 * grp];
        sc[0] = static_cast<float>(m[0]) * sc[0] * factor;
        sc[1] = static_cast<float>((0.015625 - static_cast<double>(factor)) * static_cast<double>(static_cast<float>(m[1]) * sc[1]));
        sc[2] = static_cast<float>(m[2]) * 0.0078125f + sc[2];
        sc[3] = static_cast<float>(m[3]) * sc[3] * 0.0078125f;
        s.env.susOfs = r.env.sus >= 0 ? sc[2] : 0.0f;
        s.env.loopHold = !(s.env.loopHold == false && 0.0f < sc[1]);
        if (s.env.st >= 2) s.env.ofs = s.env.susOfs;
        s.env.out = static_cast<float>(env::pointValue(r.env, s.env.ofs, 0));  // (the articulator value keeps its start-time value)
      }
      // velocity options: bit 0 scales the attack by (2 − 2 vel), bit 1 starts further into the attack
      if (r.envOn && e.patch.opt[9] != 0) {
        float* sc = s.env.scale;
        if (e.patch.opt[9] & 1) {
          sc[0] = sc[0] * (2.0f - v.vel * 2.0f);
          s.env.susOfs = r.env.sus >= 0 ? sc[2] : 0.0f;
          s.env.loopHold = !(s.env.loopHold == false && 0.0f < sc[1]);
          if (s.env.st >= 2) s.env.ofs = s.env.susOfs;
          s.env.out = static_cast<float>(env::pointValue(r.env, s.env.ofs, 0));
        }
        if ((e.patch.opt[9] & 2) && r.env.decay >= 0) {
          double T = 0.0;
          for (int i = 0; i <= r.env.decay && i < static_cast<int>(r.env.pts.size()); ++i) T = static_cast<double>(r.env.pts[static_cast<size_t>(i)].time) + T;
          env::tick(r.env, s.env, ((static_cast<double>(sc[0]) * T) * static_cast<double>(v.vel)) / r.env.speed);
        }
      }
      // portamento: the pitch envelope starts offset by the previous note's pitch
      if (vi.porta && a == 3 && u < kOps && r.envOn && R.pitchScale != 0.0f && !shared) {
        float d = vi.portaFrom - v.pitch;
        const int fold = e.patch.opt[7];
        if (fold != 0) {
          if (fold < 2) {
            const float step = d == 0.0f ? 0.0f : (d < 0.0f ? -1200.0f : 1200.0f);
            while (1250.0f < std::fabs(d)) d = d - step;
          } else {
            const float lim = static_cast<float>((6 - static_cast<long>(fold)) * 100);
            const float mm = std::fabs(d) < lim ? std::fabs(d) : lim;
            d = (d == 0.0f ? 0.0f : (d < 0.0f ? -1.0f : 1.0f)) * mm;
          }
        }
        if (d != 0.0f) {
          float* sc = s.env.scale;
          auto setOfs = [&](float x) {
            s.env.ofs = s.env.st < 2 ? x : s.env.susOfs;
            s.env.out = static_cast<float>(env::pointValue(r.env, s.env.ofs, 0));
          };
          setOfs(static_cast<float>(static_cast<double>(d) * 0.00041666666666666669));
          if (e.patch.opt[5])
            sc[0] = static_cast<float>(static_cast<double>(static_cast<float>(e.patch.units[u].u[2]) * std::fabs(d) * sc[0]) * 6.9444444444444448e-07);
          s.env.susOfs = r.env.sus >= 0 ? sc[2] : 0.0f;
          s.env.loopHold = !(s.env.loopHold == false && 0.0f < sc[1]);
          setOfs(s.env.ofs);
        }
      }
      const bool xy = !r.tableX.empty() || !r.tableY.empty();
      if (r.envOn || r.lfoOn || xy) UV.activeArts.push(static_cast<int>(a));
      float st = combine(r.mode, env::mapAt(r.maps[0], v.key * 0.0083333333333333332), env::mapAt(r.maps[1], v.vel));
      // random map: the instrument's generator is seeded per session, so any sequence is valid
      if (((e.patch.main[197] >> 8) & 0xff) != 0 && !r.mapFlat[4]) {
        e.rnd = e.rnd * 0x8088405u + 1u;
        st = combine(r.mode, st, env::mapAt(r.maps[4], static_cast<double>(e.rnd) * 2.3283064365386963e-10));
      }
      st = combine(r.mode, st, env::mapAt(r.maps[5], v.uniPos));
      s.stat = st;
      if (!r.envOn && !r.lfoOn) s.v = st;
      else s.v = combine(r.mode, s.v, st);
      if (!r.tableX.empty()) s.v = combine(r.mode, s.v, r.tableX[static_cast<size_t>(v.idxX)]);
      if (!r.tableY.empty()) s.v = combine(r.mode, s.v, r.tableY[static_cast<size_t>(v.idxY)]);
      float c = s.v;
      if (a == 2 || a == 5) c = std::clamp(c, 0.0f, 1.0f);
      UV.cur[a] = c;
    }
  }
  // operators
  for (int u = 0; u < kOps; ++u) {
    UnitRt& R = e.units[u];
    UnitVoice& UV = v.units[u];
    OpVoice& o = UV.op;
    o.vol = UV.cur[1] * R.vol;
    o.mod = UV.cur[2];
    o.rmAcc = o.mod;  // the ring accumulator starts at the MOD value
    o.len = 0;
    o.table = nullptr;
    o.slot = -1;
    o.freqBase = std::max(0.0f, static_cast<float>(static_cast<double>(R.freqOfs) + static_cast<double>(v.uniPitch) * std::exp(static_cast<double>(v.pitch) * kCentsLn) * R.ratioInc));
    o.pitchMul = static_cast<float>(std::exp(static_cast<double>(UV.cur[3] * R.pitchScale)));
    o.freq = o.freqBase * o.pitchMul;
    if (u > 0 && ((e.patch.main[197] >> 16) & 0xff) != 0) o.freq = o.freq * v.units[0].op.pitchMul;
    double ph = static_cast<double>(R.phase + v.phaseOfs + UV.cur[4]);
    if (e.patch.units[u].flags[3]) ph = static_cast<double>(static_cast<float>(static_cast<int>(static_cast<long>(e.samplesTotal))) * o.freqBase) + ph;
    ph = static_cast<double>(64 - static_cast<long>(std::nearbyint(ph))) + ph;
    o.noiseAmt = 0, o.noiseOfs = 0, o.noiseKeep = 1;
    if (!e.patch.units[u].flags[6] && e.patch.units[u].noise != 0) {
      const float a = levelLaw(e.patch.units[u].noise, 0.0078125f, false);
      o.noiseKeep = 1.0f - a;
      const float a2 = static_cast<float>(std::sqrt(static_cast<double>(e.sr) / 44100.0) * static_cast<double>(a));
      o.noiseOfs = a2;
      o.noiseAmt = static_cast<float>(static_cast<double>(a2) * 4.6566128730773926e-10);
    }
    o.center = e.patch.units[u].flags[4] != 0;
    o.cf = static_cast<float>(2.2834999999999997e-05 * (44100.0 / static_cast<double>(e.sr)));
    o.cq = 0.959999979f;
    o.cy = o.cv = 0;
    o.smooth = e.patch.units[u].flags[5] != 0 || e.softenInt > 0;
    {
      float T;
      if (e.softenInt < 1) {
        T = 0.0f;
      } else {
        T = static_cast<float>(static_cast<double>(e.softenInt) * (0.77500000000000002 - static_cast<double>(v.vel)));
        float t2 = e.declick * T;
        if (t2 <= 0.0f) t2 = 0.0f;
        T = t2;
      }
      if (e.patch.units[u].flags[5]) T = T + e.declick;
      o.smoothLeft = static_cast<int>(static_cast<long>(std::nearbyint(T)));
      if (o.smoothLeft < 1) {
        o.smoothLeft = -1;
      } else {
        o.smoothInc = static_cast<float>(1.0 / static_cast<double>(o.smoothLeft));
        o.smoothCoef = 0;
        o.smoothY = 0;
      }
    }
    o.phase = ph;
    o.prevPhaseArt = UV.cur[4];
    o.pluck = e.patch.units[u].flags[6] != 0 && v.pluckMem[u];
    if (o.pluck) pluckStart(e, u, o, UV.cur[5], v.pluckMem[u]);
  }
  // routing (matrix)
  auto cell = [&](int layer, int row, int col) -> int32_t {
    if (e.patch.mutes[layer * 81 + row * 9 + col]) return 0;
    return e.patch.main[3 + layer * 81 + row * 9 + col];
  };
  for (int s = 0; s < kOps; ++s) {
    OpVoice& o = v.units[s].op;
    o.fm.clear(), o.rm.clear();
    o.active = e.units[s].out != 0 || e.units[s].fx != 0;
    for (int t = 0; t < kOps; ++t) {
      if (int32_t c = cell(0, t, s)) {
        o.active = true;
        const float amt = levelLaw(c, 0.00390625f, true);
        o.fm.push({&v.units[t].op.pmIn, s == t ? amt * 35.0f * 0.0078125f : amt * 4.0f});
        o.hasMod = true;
        v.units[t].op.hasMod = true;
      }
      if (int32_t c = cell(1, t, s)) {
        o.active = true;
        const float amt = levelLaw(c, 0.00390625f, true);
        o.rm.push({&v.units[t].op.rmAcc, amt, 1.0f - std::fabs(amt)});
        o.hasMod = true;
        v.units[t].op.hasMod = true;
      }
    }
    if (e.units[s].hasTables && (e.patch.units[s].noise != 0 || e.units[s].pluck)) o.hasMod = true;
  }
  for (int t = kOps; t < 9; ++t) {
    FltVoice& F = v.units[t].flt;
    const UnitRt& R = e.units[t];
    F.src.clear();
    for (int s = 0; s < t; ++s) {
      float g;
      if (s < kOps) g = levelLaw(cell(0, t, s), 0.00390625f, true);
      else g = s + 1 == t ? e.units[s].toNext : 0.0f;
      if (g != 0.0f && (s < kOps || v.units[s].flt.active)) {
        F.src.push({s, g});
        if (s < kOps) v.units[s].op.active = true;
      }
    }
    F.active = !F.src.empty();
    F.volCur = v.units[t].cur[1];
    for (int i = 0; i < 3; ++i) F.gCur[i] = v.units[t].cur[4 + i];
    const float qInit = R.ftype == 0 ? 1.0f : (R.ftype == 1 || R.ftype == 2) ? 0.1f : 1.0f;
    F.last = R.stagesRaw - 1;
    F.bwScale = static_cast<float>(F.last + 1);
    if (F.last >= 0) {
      F.normRef = static_cast<float>(44100.0 / static_cast<double>(e.sr));
      if (R.hq) F.normRef *= 0.5f;
      if (R.ftype == 5) F.bwScale = 1.0f / F.bwScale;
      if (R.ftype == 12) {
        F.apCount = F.last + 1 + (R.norm ? 5 : 0);
        F.apY = 0;
        for (float& x : F.apS) x = 0;
      }
      F.spread = R.ftype != 12 && F.last > 2;
      if (F.spread) F.last -= 2;
      if (F.last > 7) F.last = 7;
      F.expo = R.ftype == 11 ? static_cast<float>(1.0 / (static_cast<double>(F.last) * 0.25 + 1.0)) : static_cast<float>(1.0 / static_cast<double>(static_cast<long>(F.last) + 1));
      for (int k = 0; k < 8; ++k) {
        F.stg[k] = FltStage{};
        if (k >= 1 && k <= F.last) F.stg[k].q = qInit;
        if (k < 3) F.stg[k].fcur = 0.5f, F.stg[k].qcur = qInit;
      }
    }
    F.bwScale = static_cast<float>(static_cast<double>(F.bwScale) * 0.34657359027997264);
  }
  for (int u = 0; u < 9; ++u)
    if (e.units[u].out != 0 && (u < kOps ? v.units[u].op.active : v.units[u].flt.active)) v.outList.push(u);
  for (int u = 0; u < 9; ++u)
    if (e.units[u].fx != 0 && (u < kOps ? v.units[u].op.active : v.units[u].flt.active)) v.fxList.push(u);
  for (int u = 0; u < kOps; ++u)
    if (v.units[u].op.active && v.units[u].op.hasMod) v.modOps.push(u);
  for (int u = 0; u < kOps; ++u)
    if (v.units[u].op.active && !v.units[u].op.hasMod) v.plainOps.push(u);
  e.fxVoices = !v.fxList.empty();
  if (e.nOrder < kMaxVoices) e.order[e.nOrder++] = voiceIndex(e, &v);
  return &v;
}

const float kUniDetune[4][9] = {
    {0.876999974f, 1.06900001f, 0.929000020f, 1.18099999f, 1.03100002f, 1.12899995f, 0.887000024f, 1.11699998f, 0.976999998f},
    {0.876999974f, 1.06900001f, 0.929000020f, 1.18099999f, 1.03100002f, 1.12899995f, 0.887000024f, 1.11699998f, 0.976999998f},
    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {0.890471935f, 1.07642627f, 0.911577046f, 1.10253584f, 1.03412616f, 1.16686118f, 0.853970945f, 1.20627260f, 1.0f}};
const float kUniEnvVar[9] = {0.987699986f, 1.00689995f, 0.992900014f, 1.01810002f, 1.00310004f, 1.01289999f, 0.988699973f, 1.01170003f, 0.997699976f};

// ------------------------------------------------------------------ rendering
void controlTick(Engine& e, Voice& v, int n, double dtFlag) {
  if (v.state > 4) v.state = 9;
  for (int u = 0; u < kUnits; ++u) {
    UnitRt& R = e.units[u];
    UnitVoice& UV = v.units[u];
    for (int a : UV.activeArts) {
      const ArtRt& r = R.arts[static_cast<size_t>(a)];
      ArtState& s = UV.arts[static_cast<size_t>(a)];
      s.v = s.stat;
      if (!r.tableX.empty()) s.v = combine(r.mode, s.v, r.tableX[static_cast<size_t>(v.idxX)]);
      if (!r.tableY.empty()) s.v = combine(r.mode, s.v, r.tableY[static_cast<size_t>(v.idxY)]);
      artTick(r, s, dtFlag);
      if (v.state > 4) v.state = std::min(v.state, std::min(s.env.st, s.lfo.st));
    }
    if (u < kOps && !UV.activeArts.empty()) {
      OpVoice& o = UV.op;
      for (int a : UV.activeArts) {
        if (a == 2) rampTo(std::clamp(UV.arts[2].v, 0.0f, 1.0f), e.maxStepUnit, o.modInc, o.mod, n);
        if (a == 3) o.pitchMul = static_cast<float>(std::exp(static_cast<double>(UV.arts[3].v * R.pitchScale)));
        if (a == 4) {
          o.phaseInc = (UV.arts[4].v - o.prevPhaseArt) / static_cast<float>(n);
          o.prevPhaseArt = UV.arts[4].v;
        }
        if (a == 5 && o.pluck) {
          o.pDamp = std::clamp(UV.arts[5].v, 0.0f, 1.0f);
          pluckTune(o);
        }
      }
    }
  }
  for (int u = 0; u < kOps; ++u) {
    OpVoice& o = v.units[u].op;
    if (!o.active) continue;
    rampTo(v.units[u].arts[1].v * e.units[u].vol, e.maxStepUnit, o.volInc, o.vol, n);
  }
  for (int u : v.outList) {
    UnitVoice& UV = v.units[u];
    if (v.state < 9) {
      float pan = (v.flPan * 0.5f + v.uniPan + e.units[u].pan + UV.arts[0].v) * 2.0f - 1.0f;
      pan = std::clamp(pan, -1.0f, 1.0f);
      const float vol = v.gain * v.uniVol * e.units[u].out;
      const ld a = (1.0L + static_cast<ld>(pan)) * static_cast<ld>(0.78539816339744828);
      UV.gL = static_cast<float>(std::cos(a) * static_cast<ld>(vol));
      UV.gR = static_cast<float>(std::sin(a) * static_cast<ld>(vol));
    } else {
      UV.gL = UV.gR = 0;
    }
    if (v.firstBlock) UV.cL = UV.gL, UV.cR = UV.gR;
  }
  for (int u : v.fxList) {
    UnitVoice& UV = v.units[u];
    if (v.state < 9) {
      float pan = (v.flPan * 0.5f + v.uniPan + e.units[u].pan + e.fxPan + UV.arts[0].v + v.units[kMain].arts[0].v) * 2.0f - 1.0f;
      pan = std::clamp(pan, -1.0f, 1.0f);
      const float vol = v.gain * v.uniVol * e.units[u].fx * v.units[kMain].arts[1].v;
      const ld a = (1.0L + static_cast<ld>(pan)) * static_cast<ld>(0.78539816339744828);
      UV.fL = static_cast<float>(std::cos(a) * static_cast<ld>(vol));
      UV.fR = static_cast<float>(std::sin(a) * static_cast<ld>(vol));
    } else {
      UV.fL = UV.fR = 0;
    }
    if (v.firstBlock) UV.fcL = UV.fL, UV.fcR = UV.fR;
  }
}

void renderOps(Engine& e, Voice& v, int n) {
  for (int u = 0; u < kOps; ++u) {
    OpVoice& o = v.units[u].op;
    if (!o.active) continue;
    {
      float nf = o.freqBase * o.pitchMul;
      if (u > 0 && ((e.patch.main[197] >> 16) & 0xff) != 0) nf = o.freqBase * o.pitchMul * v.units[0].op.pitchMul;
      if (nf != o.freq) {
        o.freq = nf;
        if (o.pluck) pluckTune(o);
      }
    }
    if (!o.table) {
      o.table = selectTable(e, u, o.freq, o.len, o.slot);
      o.mask = o.len - 1;
      o.invLen = static_cast<float>(1.0 / static_cast<double>(o.len));
      o.phase = static_cast<double>(o.len) * o.phase;
      // FM amounts into this operator scale with its table length
      for (int s = 0; s < kOps; ++s)
        for (auto& f : v.units[s].op.fm)
          if (f.dst == &o.pmIn) f.amt = static_cast<float>(o.len) * f.amt;
    }
    o.out = e.bufs[static_cast<size_t>(u + v.bufOfs)].data();
    float inc = static_cast<float>(o.len) * (o.freq + o.phaseInc);
    if (static_cast<float>(o.len >> 1) <= inc) inc = static_cast<float>(o.len >> 1);
    o.inc = inc;
    if (!o.hasMod) o.phase = static_cast<double>((1 - static_cast<long>(std::nearbyint(static_cast<double>(o.invLen) * o.phase))) * static_cast<long>(o.len)) + o.phase;
    else o.phase = static_cast<double>((64 - static_cast<long>(std::nearbyint(static_cast<double>(o.invLen) * o.phase))) * static_cast<long>(o.len)) + o.phase;
  }
  // per-sample loop (operators in a modulation path), extended precision
  if (!v.modOps.empty()) {
    for (int i = 0; i < n; ++i) {
      for (int u : v.modOps) {
        OpVoice& o = v.units[u].op;
        ld w;
        if (o.pluck) {
          const uint64_t fs = static_cast<uint64_t>(o.pFrac) + static_cast<uint64_t>(o.pFracInc);
          o.pFrac = static_cast<uint32_t>(fs);
          uint32_t steps = o.pIntInc + o.pExtra + static_cast<uint32_t>(fs >> 32);
          ld y = o.pY;
          if (steps != 0) {
            const ld c1 = o.pc1, c2 = o.pc2;
            for (; steps; --steps) {
              o.pPrev = static_cast<float>(y);
              const float b = o.pbuf[o.ppos];
              const ld nv = static_cast<ld>(b) * c1 + static_cast<ld>(o.pz) * c2;
              o.pbuf[o.ppos] = static_cast<float>(nv);
              o.pz = static_cast<float>(nv);
              y = b;
              if (++o.ppos >= o.plen) o.ppos = 0;
            }
            o.pY = static_cast<float>(y);
            o.pExtra = 0;
          }
          const ld prev = o.pPrev;
          w = (y - prev) * (static_cast<ld>(o.pFrac) * static_cast<ld>(2.3283064365386963e-10f)) + prev;
        } else {
          const ld pm = static_cast<ld>(o.pmIn) * static_cast<ld>(o.mod);
          const ld old = static_cast<ld>(o.phase);
          o.phase = static_cast<double>(static_cast<ld>(o.inc) + old);
          const ld x = old + pm;
          o.pmIn = 0;
          const ld r = std::nearbyint(x - 0.5L);
          const ld frac = x - r;
          const int idx = static_cast<int>(static_cast<long>(r)) & o.mask;
          const ld y0 = o.table[idx], y1 = o.table[idx + 1];
          w = y0 + frac * (y1 - y0);
        }
        if (!o.pluck && o.noiseAmt != 0) {
          e.noiseSeed = e.noiseSeed * 0x8088405u + 1u;  // one generator for the module, starts at 0
          w = (static_cast<ld>(e.noiseSeed) * static_cast<ld>(o.noiseAmt) - static_cast<ld>(o.noiseOfs)) + w * static_cast<ld>(o.noiseKeep);
        }
        const ld X = o.mod;
        const ld t = (1.0L + static_cast<ld>(o.rmAcc)) - X;
        o.rmAcc = static_cast<float>(X);
        o.mod = static_cast<float>(X + static_cast<ld>(o.modInc));
        const ld vv = w * t;
        for (auto& r2 : o.rm) *r2.dst = static_cast<float>((static_cast<ld>(r2.amt) * vv + static_cast<ld>(r2.c)) * static_cast<ld>(*r2.dst));
        const ld vol = static_cast<ld>(o.vol) + static_cast<ld>(o.volInc);
        const ld outx = vv * vol;
        o.vol = static_cast<float>(vol);
        o.out[i] = static_cast<float>(outx);
        for (auto& f : o.fm) *f.dst = static_cast<float>(static_cast<ld>(f.amt) * outx + static_cast<ld>(*f.dst));
      }
    }
  }
  // block path (plain operators): four float lanes
  for (int u : v.plainOps) {
    OpVoice& o = v.units[u].op;
    const double ph0 = o.phase;
    o.phase = static_cast<double>(n) * static_cast<double>(o.inc) + ph0;
    const float inc4 = o.inc * 4.0f;
    const float p0 = static_cast<float>(ph0);
    float lane[4];
    static const float kL[4] = {0.0f, 0.25f, 0.5f, 0.75f};
    for (int j = 0; j < 4; ++j) lane[j] = p0 + kL[j] * inc4;
    float vol = o.vol;
    for (int i = 0; i < n; ++i) {
      const float p = lane[i & 3];
      const int ip = static_cast<int>(p);
      const float fr = p - static_cast<float>(ip);
      const int idx = ip & o.mask;
      vol += o.volInc;
      const float y0 = o.table[idx], y1 = o.table[idx + 1];
      o.out[i] = (y0 + (y1 - y0) * fr) * vol;
      if ((i & 3) == 3)
        for (int j = 0; j < 4; ++j) lane[j] += inc4;
    }
    o.vol = vol;
  }
}

void opPost(Engine& e, Voice& v, int n) {
  for (int u = 0; u < kOps; ++u) {
    OpVoice& o = v.units[u].op;
    if (!o.active || !o.center) continue;
    float* b = e.bufs[static_cast<size_t>(u)].data();
    ld y = o.cy, w = o.cv;
    const ld f = o.cf, q = o.cq;
    for (int i = 0; i < n; ++i) {
      const ld x = b[i];
      w = (x - y) * f + w;
      y = y + w;
      w = w * q;
      b[i] = static_cast<float>(x - y);
    }
    auto flush = [](ld a) { return std::fabs(a) <= 5.96046448e-08L ? 0.0L : a; };
    o.cv = static_cast<double>(flush(w));
    o.cy = static_cast<double>(flush(y));
  }
  for (int u = 0; u < kOps; ++u) {
    OpVoice& o = v.units[u].op;
    if (!o.active || !o.smooth || o.smoothLeft <= 0) continue;
    o.smoothLeft -= n;
    int frames = n;
    if (o.smoothLeft < 0) frames += o.smoothLeft;
    float* b = e.bufs[static_cast<size_t>(u)].data();
    float y = o.smoothY, c = o.smoothCoef;
    for (int i = 0; i < frames; ++i) {
      y = y + (b[i] - y) * c;
      b[i] = y;
      c = c + o.smoothInc;
    }
    o.smoothY = y;
    o.smoothCoef = static_cast<float>(frames) * o.smoothInc + o.smoothCoef;
  }
}

const float kSpreadExp[4] = {0.99769998f, 0.5f, 0.75f, 0.0f};

void filterCoefs(Engine& e, const UnitRt& R, FltVoice& F, float cut, float res) {
  if (R.ftype == 0) {
    double c = 0.0;
    if (0.0 < static_cast<double>(cut)) c = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(cut) * 3.2580965380214821) - 1.0)) * 0.039599999999999996;
    const auto& T = e.svfTable[R.hq ? 1 : 0];
    for (int k = F.last; k >= 0; --k) {
      long idx = static_cast<long>(std::nearbyint(c * 2048.0));
      const long hi = static_cast<long>(T.size()) - 1;
      if (hi < idx) idx = hi;
      if (idx < 0) idx = 0;
      F.stg[k].f = T[static_cast<size_t>(idx)];
      if (F.spread) c = fx::powerD(c, static_cast<double>(kSpreadExp[std::min(F.last, 3)]));
    }
    double d = static_cast<double>(res);
    if (0.0 < d) d = d < 1.0 ? 1.0 - d * 0.80000000000000004 : 0.20000000000000001;
    else d = 1.0;
    if (R.norm) d = d * d;
    F.stg[0].q = static_cast<float>(d);
    return;
  }
  if (R.ftype == 1 || R.ftype == 2) {
    double c = static_cast<double>(cut), d = static_cast<double>(res);
    if (R.norm) d = static_cast<double>(0.5f) * (c + 1.0) * d;
    if (0.0 < c) {
      c = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(cut) * 4.3307333402863311) - 1.0)) * 0.01324;
      if (1.6000000000000001 < c) c = 1.6000000000000001;
    } else {
      c = 0.0;
    }
    if (0.0 < d) d = d < 1.0 ? static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(d)) * 6.9087547793152204) - 1.0)) * 0.049000000000000002 + 1.0 : 50.0;
    else d = 1.0;
    if (0 < F.last) d = fx::powerD(d, static_cast<double>(F.expo));
    const double w = static_cast<double>(F.normRef) * 3.1415926535897931 * 0.5;
    for (int st = F.last; st >= 0; --st) {
      double a = (0.0070000000000000001 + c) * w;
      a = a <= 1.2566370614359172 ? std::cos(a) : 0.30901699437494745;
      FltStage& S = F.stg[st];
      S.f = static_cast<float>(2.0 - 2.0 * a);
      const double m = a - 1.0;
      S.q = static_cast<float>((static_cast<double>(1.41421354f) * std::sqrt(-m * m * m)) / (d * m) + 1.0);
      if (static_cast<double>(S.q) < 0.10000000000000001) S.q = 0.1f;
      if (F.spread) c = fx::powerD(c, static_cast<double>(kSpreadExp[std::min(F.last, 3)]));
    }
    return;
  }
  if (R.ftype == 12) {
    double c = static_cast<double>(cut), d = static_cast<double>(res);
    if (0.0 < c) c = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(cut) * 5.0172798368149243) - 1.0)) * 0.0066660000000000001;
    else c = 0.0;
    if (0.0 < d) {
      if (d < 1.0) d = static_cast<double>(static_cast<float>(std::log(static_cast<double>(static_cast<float>(d * 10.0) + 1.0f)) / 2.3978952727983707)) * 0.90000000000000002;
      else d = 0.90000000000000002;
    } else {
      d = 0.0;
    }
    double a = static_cast<double>(F.normRef) * (0.0001 + c);
    if (1.0 < a) a = 1.0;
    F.apFb = static_cast<float>(d);
    F.apA = static_cast<float>((1.0 - a) / (1.0 + a));
    if (std::fabs(F.apA) < 5.96046448e-08f) F.apA = 0;
    return;
  }
  if (R.ftype == 11) {
    double c = static_cast<double>(cut), d = static_cast<double>(res);
    if (0.0 < c) {
      c = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(cut) * 5.3033049080590757) - 1.0)) * 0.014950000000000001;
      if (3.0 < c) c = 3.0;
    } else {
      c = 0.0;
    }
    const double dd = std::exp(static_cast<double>(F.gainA) * 5.5254529391317835);
    const double l78 = static_cast<double>(static_cast<float>(dd - 1.0)) * 0.12 + 0.0;
    const bool drive = 0.001 <= l78;
    double lb8 = 0.0;
    if (drive) lb8 = 1.0 / l78;
    double fb;
    if (0.0 < d) {
      float f7 = static_cast<float>(l78);
      if (1.0f <= f7) f7 = 1.0f;
      fb = static_cast<double>(f7) * 0.59999999999999998 + 1.8999999999999999;
      if (d < 1.0) fb = (fb - 0.0) * d + 0.0;
    } else {
      fb = 0.0;
    }
    if (0 < F.last) fb = fx::powerD(fb + 1.0, static_cast<double>(F.expo)) - 1.0;
    fb = -fb;
    const double w = static_cast<double>(F.normRef * 0.5f);
    for (int st = F.last; st >= 0; --st) {
      FltStage& S = F.stg[st];
      double a = (0.01 + c) * w;
      if (0.65000000000000002 < a) a = 0.65000000000000002;
      const double a2 = a * a;
      const double poly = ((0.4955 * a2 + 1.873 * a * a2) - 0.64900000000000002 * a) + 0.99880000000000002;
      S.lp = 1.0 - std::exp(-3.1415926535897931 * poly * a);
      S.lk = static_cast<float>((1.8409 * a + -3.9363999999999999 * a2 + 0.99680000000000002) * fb);
      if (drive) {
        S.ldrive = 2.0 * l78;
        S.lp = S.lp * lb8;
        const double x = 4.0 * lb8;
        double th;
        if (x > 5678.22249441322) th = 1.0;
        else if (x < -5678.22249441322) th = -1.0;
        else {
          const double ex = std::exp(-2.0 * x);
          th = (1.0 - ex) / (1.0 + ex);
        }
        S.lg = static_cast<float>(th);
        if (R.norm) S.lk = S.lk * S.lg;
        S.lg = (1.0f - S.lk) * (1.0f / S.lg) * 0.5f;
      } else {
        S.lg = (1.0f - S.lk) * 0.5f;
        S.ldrive = 0.0;
      }
      if (F.spread) c = fx::powerD(c, static_cast<double>(kSpreadExp[std::min(F.last, 3)]));
    }
    return;
  }
  if (R.ftype < 3 || R.ftype > 10) return;
  static const int kCode[11] = {0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
  double c = static_cast<double>(cut), d = static_cast<double>(res);
  if (R.norm) d = static_cast<double>(0.5f) * (c + 1.0) * d;
  const bool t3 = R.ftype == 3;
  if (0.0 < c) {
    c = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(cut) * 7.3138868316334618) - 1.0)) * (t3 ? 0.0006646666666666667 : 0.00039993333333333331);
    if (!t3) {
      const double lim = R.ftype == 6 ? 0.60009999999999997 : 1.0001;
      if (lim < c) c = lim;
    }
  } else {
    c = 0.0;
  }
  const double w = static_cast<double>(F.normRef) * 3.1415926535897931 * 2.0;
  const double lo = t3 ? 0.0030000000000000001 : 0.0001;
  double A = 0.0, k = 0.0;
  int mode = 0;  // 0: alpha = sin k, 1: alpha = sinh(k a / sin) sin, 2: alpha = k (shelf form), 3: alpha = sin / (2 k)
  switch (R.ftype) {
    case 3:
    case 6:
    case 7:
      if (0.0 < d) d = d < 1.0 ? static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(d)) * 7.6014023345837334) - 1.0)) * 0.02495 + 0.10000000000000001 : 50.0;
      else d = 0.10000000000000001;
      if (0 < F.last) d = fx::powerD(d, static_cast<double>(F.expo));
      k = 0.5 / d;
      mode = 0;
      break;
    case 4:
    case 5:
      if (0.0 < d) d = d < 1.0 ? static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(d)) * 4.6151205168412597) - 1.0)) * 0.11990000000000001 + 0.01 : 12.0;
      else d = 0.01;
      k = static_cast<double>(F.bwScale) * d;
      mode = 1;
      break;
    case 8:
    case 10: {
      d = 1.0 - d;
      if (0.0 < d) d = d < 1.0 ? static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(d)) * 1.0986122886681098) - 1.0)) * 0.45000000000000001 + 0.10000000000000001 : 1.0;
      else d = 0.10000000000000001;
      A = static_cast<double>(F.gainA);
      if (0 < F.last) A = fx::powerD(A, static_cast<double>(F.expo));
      k = std::sqrt((1.0 / d - 1.0) * (1.0 / A + A) + 2.0) * std::sqrt(A);
      mode = 2;
      break;
    }
    case 9:
      d = 1.0 - d;
      if (0.0 < d) d = d < 1.0 ? static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(d)) * 7.6014023345837334) - 1.0)) * 0.049950000000000001 + 0.10000000000000001 : 100.0;
      else d = 0.10000000000000001;
      A = static_cast<double>(F.gainA);
      if (0 < F.last) A = fx::powerD(A, static_cast<double>(F.expo));
      k = d;
      mode = 3;
      break;
    default:
      break;
  }
  for (int st = F.last; st >= 0; --st) {
    double a = (lo + c) * w;
    if (a <= 2.849517146113191) {
      if (a < 0.0) a = 0.0;
    } else {
      a = 2.849517146113191;
    }
    const double sn = std::sin(a), cs = std::cos(a);
    double alpha;
    if (mode == 0) alpha = sn * k;
    else if (mode == 1) {
      const double x = (k * a) / sn;
      alpha = ((std::exp(x) - std::exp(-x)) * 0.5) * sn;
    } else if (mode == 2) alpha = k;
    else alpha = sn / (2.0 * k);
    fx::Biquad& B = F.stg[st].bq;
    B.type = kCode[R.ftype];
    B.design(A, alpha, sn, cs);
    if (F.spread) c = fx::powerD(c, static_cast<double>(kSpreadExp[std::min(F.last, 3)]));
  }
}

void renderFilters(Engine& e, Voice& v, int n) {
  float* band = e.bufs[9].data();
  float* high = e.bufs[10].data();
  for (int t = kOps; t < 9; ++t) {
    UnitVoice& UV = v.units[t];
    FltVoice& F = UV.flt;
    if (!F.active) continue;
    const UnitRt& R = e.units[t];
    float* buf = e.bufs[static_cast<size_t>(t)].data();
    bool first = true;
    for (auto& sg : F.src) {
      const float* src = e.bufs[static_cast<size_t>(sg.unit)].data();
      if (first) {
        for (int i = 0; i < n; ++i) buf[i] = src[i] * sg.gain;
        first = false;
      } else {
        for (int i = 0; i < n; ++i) buf[i] = buf[i] + src[i] * sg.gain;
      }
    }
    fx::ramp(UV.arts[1].v, e.maxStepUnit, F.volInc, F.volCur, F.volStart, n);
    if (isZeroBits(F.volInc)) {
      if (F.volStart != 1.0f)
        for (int i = 0; i < n; ++i) buf[i] = buf[i] * F.volStart;
    } else {
      fx::rampMulLanes(buf, buf, F.volStart, F.volInc, n);
    }
    if (F.last >= 0) {
      const int nn = R.hq ? 2 * n : n;
      const float cut = R.cutBase + UV.arts[2].v, res = R.resBase + UV.arts[3].v;
      if (R.hq) fx::upsample2(buf, n);
      if (R.ftype == 12) {
        filterCoefs(e, R, F, cut, res);
        fx::allpassChain(F.apS, std::min(F.apCount, 32), F.apA, F.apFb, F.apY, buf, nn);
      } else if (R.ftype == 11) {
        F.gainA = static_cast<float>(R.gainRaw) * 0.0078125f * UV.arts[4].v;
        filterCoefs(e, R, F, cut, res);
        for (int k = 0; k <= F.last; ++k) {
          FltStage& S = F.stg[k];
          fx::ladder(S.lz, S.lp, S.lk, S.ldrive, S.lg, buf, buf, nn);
        }
      } else if (R.ftype == 1 || R.ftype == 2) {
        filterCoefs(e, R, F, cut, res);
        if (v.firstBlock)
          for (int k = 0; k <= F.last; ++k) F.stg[k].fcur = F.stg[k].f, F.stg[k].qcur = F.stg[k].q;
        for (int k = 0; k <= F.last; ++k) {
          FltStage& S = F.stg[k];
          fx::ramp(S.f, e.maxStepUnit, S.finc, S.fcur, S.f, nn);
          fx::ramp(S.q, e.maxStepUnit, S.qinc, S.qcur, S.q, nn);
          fx::twoPole(S.st, S.f, S.q, S.finc, S.qinc, R.ftype == 2, buf, buf, nn);
        }
      } else if (R.ftype >= 3 && R.ftype <= 10) {
        if (R.ftype >= 7) {
          const float x = static_cast<float>(static_cast<double>(static_cast<float>(R.gainRaw) * UV.arts[4].v) * 0.015625);
          F.gainA = static_cast<float>(std::exp(static_cast<double>(x) * 2.0794415416798357) - 1.0) * 0.125f + 0.125f;
        }
        filterCoefs(e, R, F, cut, res);
        for (int k = 0; k <= F.last; ++k) fx::biquadMono(F.stg[k].st, F.stg[k].bq, buf, buf, nn);
      } else if (R.ftype == 0) {
        float g[3];
        for (int i = 0; i < 3; ++i) {
          g[i] = R.mix[i] * UV.arts[4 + i].v;
          if (F.last > 0) {
            int32_t bits;
            std::memcpy(&bits, &g[i], 4);
            g[i] = bits < 1 ? 0.0f : fx::powerF(g[i], F.expo);
          }
        }
        filterCoefs(e, R, F, cut, res);
        if (v.firstBlock) {
          for (int i = 0; i < 3; ++i) F.gCur[i] = g[i];
          for (int k = 0; k <= F.last; ++k) F.stg[k].fcur = F.stg[k].f, F.stg[k].qcur = F.stg[k].q;
        }
        for (int i = 0; i < 3; ++i) fx::ramp(g[i], e.maxStepUnit, F.gInc[i], F.gCur[i], F.gStart[i], nn);
        for (int k = 0; k <= F.last; ++k) {
          FltStage& S = F.stg[k];
          fx::ramp(S.f, e.maxStepUnit, S.finc, S.fcur, S.f, nn);
        }
        fx::ramp(F.stg[0].q, e.maxStepUnit, F.stg[0].qinc, F.stg[0].qcur, F.stg[0].q, nn);
        if (R.norm)
          for (int k = 0; k <= F.last; ++k) {
            FltStage& S = F.stg[k];
            const float a = S.fcur < S.f ? S.f : S.fcur;
            S.q = k > 0 ? F.normRef / a : S.q * F.normRef / a;
          }
        for (int k = 0; k <= F.last; ++k) {
          FltStage& S = F.stg[k];
          const bool ramp = !isZeroBits(S.finc) || !isZeroBits(S.qinc);
          fx::svf(S.st, S.f, S.q, S.finc, S.qinc, ramp, buf, buf, band, high, nn);
          fx::mixSet(F.gStart[0], F.gInc[0], buf, buf, nn);
          fx::mixAdd(F.gStart[1], F.gInc[1], band, buf, nn);
          fx::mixAdd(F.gStart[2], F.gInc[2], high, buf, nn);
        }
      }
      if (R.hq) fx::downsample2(buf, n);
    }
    if (R.wsOn) {
      fx::shaper(buf, band, n, R.wsTable.data() + 1, R.wsLen, R.wsGain, R.wsBip, e.patch.b5f28 != 0);
      float mix = UV.arts[7].v * R.wsMix;
      if (!(0.0f <= mix)) mix = 0.0f;
      else if (1.0f < mix) mix = 1.0f;
      F.wsTarget = mix;
      if (v.firstBlock) F.wsCur = mix;
      fx::ramp(F.wsTarget, e.maxStepUnit, F.wsInc, F.wsCur, F.wsStart, n);
      if (isZeroBits(F.wsInc)) {
        if (!isZeroBits(F.wsStart)) {
          if (F.wsStart == 1.0f) {
            std::memcpy(buf, band, sizeof(float) * static_cast<size_t>(n));
          } else {
            const float k = 1.0f - F.wsStart;
            for (int i = 0; i < n; ++i) buf[i] = buf[i] * k;
            for (int i = 0; i < n; ++i) buf[i] = buf[i] + band[i] * F.wsStart;
          }
        }
      } else {
        fx::rampMulLanes(buf, buf, 1.0f - F.wsStart, -F.wsInc, n);
        fx::rampAdd(band, buf, F.wsStart, F.wsInc, n);
      }
    }
  }
}

void mixUnit(float tL, float tR, float maxStep, float& cL, float& cR, const float* src, float* dst, int n) {
  float incL, incR, startL, startR;
  fx::ramp(tL, maxStep, incL, cL, startL, n);
  fx::ramp(tR, maxStep, incR, cR, startR, n);
  if (incL == 0 && incR == 0 && startL == 0 && startR == 0) return;
  ld l = startL, r = startR;
  for (int i = 0; i < n; ++i) {
    const ld x = src[i];
    dst[2 * i] = static_cast<float>(x * l + static_cast<ld>(dst[2 * i]));
    dst[2 * i + 1] = static_cast<float>(x * r + static_cast<ld>(dst[2 * i + 1]));
    l += incL, r += incR;
  }
}

void mixOut(Engine& e, Voice& v, int n, float* dry, float* fxBus) {
  const float maxStep = e.maxStepMix * v.uniVol;
  for (int u : v.fxList) mixUnit(v.units[u].fL, v.units[u].fR, maxStep, v.units[u].fcL, v.units[u].fcR, e.bufs[static_cast<size_t>(u)].data(), fxBus, n);
  for (int u : v.outList) {
    UnitVoice& UV = v.units[u];
    float incL, incR, startL, startR;
    fx::ramp(UV.gL, maxStep, incL, UV.cL, startL, n);
    fx::ramp(UV.gR, maxStep, incR, UV.cR, startR, n);
    const float* src = e.bufs[static_cast<size_t>(u)].data();
    ld l = startL, r = startR;
    for (int i = 0; i < n; ++i) {
      const ld x = src[i];
      dry[2 * i] = static_cast<float>(x * l + static_cast<ld>(dry[2 * i]));
      dry[2 * i + 1] = static_cast<float>(x * r + static_cast<ld>(dry[2 * i + 1]));
      l += incL, r += incR;
    }
  }
}

void voiceOps(Engine& e, Voice& v, int k) {
  renderOps(e, v, k);
  if (v.bufOfs > 0) {
    for (int u : v.modOps) {
      float* d = e.bufs[static_cast<size_t>(u)].data();
      const float* s2 = e.bufs[static_cast<size_t>(u + v.bufOfs)].data();
      for (int i = 0; i < k; ++i) d[i] = d[i] + s2[i] * v.uniGain;
    }
    for (int u : v.plainOps) {
      float* d = e.bufs[static_cast<size_t>(u)].data();
      const float* s2 = e.bufs[static_cast<size_t>(u + v.bufOfs)].data();
      for (int i = 0; i < k; ++i) d[i] = d[i] + s2[i] * v.uniGain;
    }
  } else if (v.role >= 1) {
    for (int u : v.modOps) {
      float* d = e.bufs[static_cast<size_t>(u)].data();
      for (int i = 0; i < k; ++i) d[i] = d[i] * v.uniGain;
    }
    for (int u : v.plainOps) {
      float* d = e.bufs[static_cast<size_t>(u)].data();
      for (int i = 0; i < k; ++i) d[i] = d[i] * v.uniGain;
    }
  }
}

// One host block (≤ kBlock frames), adding into interleaved `out`.
void renderBlock(Engine& e, float* out, int hostFrames) {
  int n = hostFrames << e.os;
  std::fill(e.dry.begin(), e.dry.begin() + 2 * n, 0.f);
  std::fill(e.fxb.begin(), e.fxb.begin() + 2 * n, 0.f);
  std::fill(e.tmp.begin(), e.tmp.begin() + 2 * n, 0.f);
  // effects activity: reset while voices with sends exist, then a short tail
  int live = 0;
  for (int i = 0; i < e.nOrder; ++i)
    if (e.voices[static_cast<size_t>(e.order[i])].state != 11) ++live;  // left-over unison voices count
  if (!e.fxVoices) e.fxCount = 0;
  else if (live < 1) {
    if (e.fxCount > 0) e.fxCount -= n;
  } else {
    e.fxCount = e.fxTail;
  }
  if (e.stepDirty) {  // step envelopes: each voice's key position among the held keys
    e.stepDirty = false;
    if (e.mainVoices > 0) {
      int cnt = 0;
      for (int i = 0; i < e.nOrder; ++i) {
        const Voice& v = e.voices[static_cast<size_t>(e.order[i])];
        if (v.state >= 5 || v.chained) continue;
        int at = 0;
        bool dup = false;
        for (; at < cnt; ++at) {
          if (e.stepKeys[at] == v.keyCents) {
            dup = true;
            break;
          }
          if ((v.keyCents < e.stepKeys[at]) != (e.patch.opt[3] != 0)) break;
        }
        if (dup || cnt >= kMaxVoices) continue;
        for (int j = cnt; j > at; --j) e.stepKeys[j] = e.stepKeys[j - 1];
        e.stepKeys[at] = v.keyCents;
        ++cnt;
      }
      for (int i = 0; i < e.nOrder; ++i) {
        Voice& v = e.voices[static_cast<size_t>(e.order[i])];
        if (v.state >= 6) continue;
        int idx = 0;
        while (idx < cnt && e.stepKeys[idx] != v.keyCents) ++idx;
        for (int u = 0; u < kUnits; ++u)
          for (int a : v.units[u].activeArts) {
            const ArtRt& r = e.units[u].arts[static_cast<size_t>(a)];
            env::setStep(r.env, v.units[u].arts[static_cast<size_t>(a)].env, idx, cnt);
            env::setStep(r.lfo, v.units[u].arts[static_cast<size_t>(a)].lfo, idx, cnt);
          }
      }
    }
  }
  bool died = false;
  for (int oi = e.nOrder; oi-- > 0;) {
    Voice& v = e.voices[static_cast<size_t>(e.order[oi])];
    if (v.state >= 10 || v.role >= 1) continue;
    const int state0 = v.state;
    int done = 0;
    while (done < n) {
      double dtFlag = 1.0;
      bool tick = false;
      if (v.untilTick < 1) {
        if (v.untilTick < 0) {
          v.untilTick = -v.untilTick;
          dtFlag = 0.0;
        } else {
          v.untilTick = e.tick;
        }
        tick = true;
        controlTick(e, v, v.untilTick, dtFlag);
      }
      const int k = std::min(v.untilTick, n - done);
      if (v.role < 0)
        for (Voice* sv : v.chain) {
          if (tick) controlTick(e, *sv, v.untilTick, dtFlag);
          voiceOps(e, *sv, k);
          sv->firstBlock = false;
        }
      voiceOps(e, v, k);
      opPost(e, v, k);
      renderFilters(e, v, k);
      mixOut(e, v, k, e.dry.data() + 2 * done, e.fxb.data() + 2 * done);
      v.firstBlock = false;
      v.untilTick -= k;
      done += k;
    }
    if (v.state >= 9) {
      bool silent = true;
      for (int u : v.outList)
        if (v.units[u].cL != 0 || v.units[u].cR != 0) silent = false;
      for (int u : v.fxList)
        if (v.units[u].fcL != 0 || v.units[u].fcR != 0) silent = false;
      if (silent) v.state = 10;
    }
    if (v.state != state0) e.stepDirty = e.waitArts;
  }
  // the host frees finished voices (with their unison chains)
  for (int i = 0; i < e.nOrder; ++i) {
    Voice& v = e.voices[static_cast<size_t>(e.order[i])];
    if (v.state != 10 || v.role >= 1) continue;
    if (v.role < 0)  // shared-filter unison: the extra voices are left over (see allocVoice)
      for (Voice* sv : v.chain) {
        freeVoice(e, *sv);
        sv->state = 12;
      }
    freeVoice(e, v);
    died = true;
  }
  if (died) compactOrder(e);
  e.samplesTotal += n;
  if (e.os > e.fxStages) {
    int m = n;
    e.decDry.run(e.dry.data(), m, e.os - 1, e.fxStages);
    e.decFx.run(e.fxb.data(), n, e.os - 1, e.fxStages);
    n = m;
  }
  // effects
  if (e.eqMode > 1) {
    float* b = e.eqMode == 3 ? e.fxb.data() : e.dry.data();
    for (auto& bd : e.eq)
      if (bd.countdown != 0) {
        bd.bq.process(b, n);
        if (bd.countdown > 0 && (bd.countdown -= n) < 1) bd.countdown = 0;
      }
  }
  if (e.chorus.on) {
    if (e.fxCount < 1) {
      e.chorus.idle(n);
    } else {
      e.chorus.write(e.fxb.data(), n);
      e.chorus.read(e.tmp.data(), n);
      for (int j = 0; j < 2 * n; ++j) e.dry[static_cast<size_t>(j)] += e.tmp[static_cast<size_t>(j)];
      if (e.fxToFx)
        for (int j = 0; j < 2 * n; ++j) e.fxb[static_cast<size_t>(j)] += e.tmp[static_cast<size_t>(j)];
    }
  }
  int fxCount = e.fxCount;
  for (int i = 0; i < 3; ++i) {
    fx::Delay& d = e.delays[i];
    if (!d.on) continue;
    d.process(e.fxb.data(), fxCount > 0, e.tmp.data(), n);
    float* dst = d.serial ? e.fxb.data() : e.dry.data();
    if (d.serial) fxCount = e.fxTail;
    for (int j = 0; j < 2 * n; ++j) dst[j] = d.vol == 1.0f ? dst[j] + e.tmp[static_cast<size_t>(j)] : dst[j] + d.vol * e.tmp[static_cast<size_t>(j)];
  }
  if (e.reverb.on) {
    for (int j = 0; j < n; ++j) e.mono[static_cast<size_t>(j)] = e.fxb[static_cast<size_t>(2 * j)] + e.fxb[static_cast<size_t>(2 * j + 1)];
    e.reverb.process(e.mono.data(), e.fxb.data(), n);
    for (int j = 0; j < 2 * n; ++j) e.dry[static_cast<size_t>(j)] += e.fxb[static_cast<size_t>(j)] * e.reverb.wet;
  }
  if (e.eqMode == 1)
    for (auto& bd : e.eq)
      if (bd.countdown != 0) {
        bd.bq.process(e.dry.data(), n);
        if (bd.countdown > 0 && (bd.countdown -= n) < 1) bd.countdown = 0;
      }
  if (e.fxStages > 0) e.decDry.run(e.dry.data(), n, e.fxStages - 1, 0);
  if (e.patch.opt[1]) {  // output centring (DC) filter
    const ld f = static_cast<float>(1.0070234999999998 / static_cast<double>(e.hostSr)), q = 0.959999979f;
    for (int c = 0; c < 2; ++c) {
      ld y = e.dcY[c], w = e.dcV[c];
      for (int i = 0; i < n; ++i) {
        const ld x = e.dry[static_cast<size_t>(2 * i + c)];
        w = (x - y) * f + w;
        y = y + w;
        w = w * q;
        e.dry[static_cast<size_t>(2 * i + c)] = static_cast<float>(x - y);
      }
      auto flush = [](ld a) { return std::fabs(a) <= 5.96046448e-08L ? 0.0L : a; };
      e.dcV[c] = static_cast<double>(flush(w));
      e.dcY[c] = static_cast<double>(flush(y));
    }
  }
  for (int j = 0; j < 2 * n; ++j) out[j] += e.dry[static_cast<size_t>(j)];
}

}  // namespace

// ------------------------------------------------------------------ public
EnginePtr makeEngine(const Patch& p, int sampleRate, double tempo) {
  EnginePtr E(new Engine);
  Engine& e = *E;
  e.patch = p;
  e.tempo = tempo > 1 ? tempo : 120.0;
  // oversampling: voices at sr << os, effects at sr × min(2^os, 4); half-band decimators between
  e.hostSr = sampleRate;
  e.os = std::max(0, std::min(p.main[195], 4));
  e.ratio = 1 << e.os;
  e.fxRatio = std::min(e.ratio, 4);
  e.fxStages = std::min(e.os, 2);
  e.sr = sampleRate * e.ratio;
  e.fxSr = sampleRate * e.fxRatio;
  e.maxStepMix = static_cast<float>(0.001 * (44100.0 / static_cast<double>(e.sr)));
  e.maxStepUnit = static_cast<float>(0.01 * (44100.0 / static_cast<double>(e.sr)));
  // control period (envelope accuracy)
  static const int kAcc[2] = {96, 8};
  const int acc = kAcc[std::clamp(p.main[193], 0, 1)];
  const double d3 = static_cast<double>(static_cast<long>(e.sr) * acc) / 44100.0;
  e.tick = std::max(4, static_cast<int>(std::nearbyint(d3 * 0.25)) * 4);
  e.tickRatio = static_cast<double>(static_cast<long>(e.tick) * acc) / (d3 * 96.0);
  e.modSmooth = static_cast<float>(std::exp(-4.6051701859880909 * e.tickRatio / 96.0));
  e.e8 = static_cast<float>(523.25113060119702 / static_cast<double>(e.sr));
  e.mainVol = levelLaw(p.main[0], 0.0078125f, false);
  e.lfoLevel = levelLaw(p.main[1], 0.0078125f, true);
  e.masterPitch = p.main[2] * 100;
  e.modXg = static_cast<float>(p.main[190]), e.modYg = static_cast<float>(p.main[191]);
  int plucks = 0;
  for (int u = 0; u < kUnits; ++u) {
    UnitRt& R = e.units[u];
    const Unit& U = p.units[u];
    unitLive(e, u);
    if (u < kOps) {
      R.pitchScale = static_cast<float>(static_cast<double>(static_cast<long>(U.u[2]) << 1) * kCentsLn);
      R.phase = static_cast<float>(U.u[0]) * 0.00390625f;
      R.ratioInc = static_cast<double>((static_cast<float>(U.ratio) * e.e8) / 20000.0f);
      R.freqOfs = U.freqOfs == 0 ? 0.0f : static_cast<float>(U.freqOfs) / static_cast<float>(static_cast<long>(e.sr) * 100);
      buildTables(R.tables, U, p.opt[2] != 0);
      R.hasTables = true;
      R.pluck = U.flags[6] != 0;
      R.bandLimit = U.flags[7] != 0;
      plucks += R.pluck ? 1 : 0;
      BandSlots& B = e.band[u];
      B.on = R.bandLimit && !R.pluck;
      if (B.on) {
        B.mem.assign(static_cast<size_t>(BandSlots::kSlots) * (kBandLen + 1), 0.f);
        for (int s = 0; s < BandSlots::kSlots; ++s) B.h1[s] = B.h2[s] = -1, B.refs[s] = 0, B.used[s] = 0;
      }
    }
    if (u >= kOps && u < 9) {
      R.ftype = U.fb[11] & 0xff;
      R.stagesRaw = std::clamp(U.u[0], 0, 8);
      R.hq = ((U.fb[0] & 0xff) != 0 && e.sr <= 176399) || (R.ftype == 11 && e.sr <= 88199);
      R.norm = ((U.fb[0] >> 8) & 0xff) != 0;
      R.wsOn = (U.fb[10] & 0xff) != 0;
      R.wsBip = U.wsBip != 0;
      if (R.wsOn) {
        const int bip = R.wsBip ? 1 : 0;
        const int L = (bip + 1) * 0x4000 - 1;
        R.wsLen = L;
        R.wsTable.assign(static_cast<size_t>(L) + 3, 0);
        const env::Env m = env::prepare(U.ws, 3, R.wsBip ? 0.5 : 0.0, 1.0, true);
        const float scale = static_cast<float>(static_cast<long>(static_cast<long>(bip + 1) * 0x8000 - 1));
        const double step = static_cast<double>(static_cast<float>(1.0 / static_cast<double>(L)));
        env::State it;
        float val = env::mapIterStart(m, it);
        for (int i = 0; i <= L; ++i) {
          if (i > 0) val = env::mapIterStep(m, it, step);
          long q = std::isfinite(val) ? static_cast<long>(std::nearbyint(val * scale)) : 0;  // float product, then rounded
          if (q < -0x7fff) q = -0x8000;
          if (0x7fff < q) q = 0x7fff;
          R.wsTable[static_cast<size_t>(i) + 1] = static_cast<int16_t>(q);
        }
        R.wsTable[0] = R.wsTable[1];
        R.wsTable[static_cast<size_t>(L) + 2] = R.wsTable[static_cast<size_t>(L) + 1];
      }
    }
    static const int kOpModes[6] = {1, 0, 0, 1, 1, 0};
    static const int kFltModes[8] = {1, 0, 1, 1, 0, 0, 0, 0};
    static const int kMainModes[2] = {1, 0};
    for (size_t a = 0; a < U.arts.size(); ++a) {
      const int mode = u < kOps ? kOpModes[a % 6] : u < 9 ? kFltModes[a % 8] : kMainModes[a % 2];
      const bool clamp = !(u < kOps && a == 3);
      R.arts.push_back(prepareArt(e, U.arts[a], mode, clamp));
      if (u >= kOps && u < 9 && a == 2) {  // the filter's cut articulator takes its depth from the unit
        R.arts.back().envAmt = static_cast<float>(U.u[1]) * 0.0078125f;
        R.arts.back().lfoAmt = R.arts.back().envAmt * e.lfoLevel;
      }
    }
  }
  const size_t maxN = static_cast<size_t>(kBlock) << e.os;
  for (auto& b : e.bufs) b.assign(maxN * 2 + 8, 0.f);
  e.dry.assign(maxN * 2, 0.f), e.fxb.assign(maxN * 2, 0.f), e.tmp.assign(maxN * 2, 0.f), e.mono.assign(maxN, 0.f);
  e.out.assign(static_cast<size_t>(kBlock) * 2, 0.f);
  {
    const double d = std::exp(static_cast<double>(static_cast<float>(p.soften) * 0.0078125f) * 4.6151205168412597);
    e.softenInt = static_cast<int>(static_cast<long>(std::nearbyint(static_cast<ld>(static_cast<float>(d - 1.0) * 10.0f))));
    const double r = 44100.0 / static_cast<double>(e.sr);
    e.declick = static_cast<float>(96.0 / (r * r));
  }
  {
    double scale = 44100.0 / static_cast<double>(e.sr);
    for (int set = 0; set < 2; ++set) {
      auto& T = e.svfTable[set];
      T.assign(4096, 0.f);
      for (int i = 0; i < 4096; ++i) {
        const double d = static_cast<double>(i) * 0.00048828125 + 0.01;
        float f = static_cast<float>(0.5 * d);
        if (1.0f <= f) f = 1.0f;
        const double x = static_cast<double>(f);
        const float g = static_cast<float>(std::atan2(x, std::sqrt((1.0 - x) * (1.0 + x))));
        const double t = std::sin(static_cast<double>(g) * scale) * 2.0;
        if (1.175 <= t) {
          T[static_cast<size_t>(i)] = 1.17499995f;
          T.resize(static_cast<size_t>(i) + 1);
          break;
        }
        T[static_cast<size_t>(i)] = static_cast<float>(t);
      }
      scale *= 0.5;
    }
  }
  e.fxPan = static_cast<float>(p.main[180]) * 0.00390625f;
  e.fxTail = static_cast<int>((static_cast<long>(e.sr) * 100) / 44100);
  e.decDry.init(e.os), e.decFx.init(e.os);
  const int fxBlock = kBlock * e.fxRatio;
  for (int i = 0; i < 3; ++i) {
    const uint8_t* d = p.delays[i];
    const int32_t vol = static_cast<int32_t>(static_cast<uint32_t>(d[12]) | (static_cast<uint32_t>(d[13]) << 8) | (static_cast<uint32_t>(d[14]) << 16) | (static_cast<uint32_t>(d[15]) << 24));
    e.delays[i].setup(d, e.fxSr, fxBlock, e.tempo, levelLaw(vol, 0.0078125f, true));
  }
  {
    int last = -1;
    for (int i = 0; i < 3; ++i)
      if (e.delays[i].on) last = i;
    e.reverb.setup(p.reverb, e.fxSr, e.tempo);
    const bool reverbOn = e.reverb.on;
    const int lim = reverbOn ? 3 : last;
    e.fxToFx = last >= 0 || reverbOn || p.main[189] != -1;
    for (int i = 0; i < 3; ++i) e.delays[i].serial = p.delays[i][21] != 0 && i < lim;
  }
  e.chorus.setup(p.main[181], static_cast<float>(static_cast<double>(p.main[182]) * 0.0009765625), static_cast<float>(static_cast<double>(p.main[183]) * 0.0009765625),
                 static_cast<float>(p.main[184]) * 0.0078125f, static_cast<float>(p.main[185]) * 0.0078125f, static_cast<float>(p.main[186]) * 0.0078125f,
                 static_cast<float>(p.main[187]) * 0.0078125f, e.fxSr, fxBlock);
  if (e.chorus.on) e.fxTail += static_cast<int>(static_cast<long>(e.chorus.frames) * static_cast<long>(e.ratio) / static_cast<long>(e.fxRatio));
  e.eqMode = p.eqTarget;
  for (int b = 0; b < 3; ++b) e.eq[b].setup(p.eq[b], e.fxSr, e.fxSr / 5);
  e.anyFx = e.chorus.on || e.reverb.on || e.delays[0].on || e.delays[1].on || e.delays[2].on;
  // voices, band-limited table scratch, string-loop memory
  e.voices.resize(kMaxVoices);
  e.bandFft.init(kBandLen, +1);
  e.bandY.assign(kBandLen, cplx{});
  e.bandW.assign(kBandLen / 2 + 1, 0.0);
  if (plucks > 0) {
    e.pluckPool.assign(static_cast<size_t>(kMaxVoices) * static_cast<size_t>(plucks) * kPluckCap, 0.f);
    float* m = e.pluckPool.data();
    for (auto& v : e.voices)
      for (int u = 0; u < kOps; ++u)
        if (e.units[u].pluck) {
          v.pluckMem[u] = m;
          m += kPluckCap;
        }
  }
  return E;
}

Live liveOf(const Patch& p) {
  Live l;
  l.mainVol = p.main[0], l.modX = p.main[190], l.modY = p.main[191];
  std::memcpy(l.matrix, p.main + 3, sizeof l.matrix);
  std::memcpy(l.mutes, p.mutes, sizeof l.mutes);
  for (int u = 0; u < kOps; ++u) l.opVol[u] = p.units[u].u[1];
  for (int j = 0; j < kFilters; ++j)
    for (int i = 0; i < 10; ++i) l.flt[j][i] = p.units[kOps + j].u[i];
  return l;
}

namespace {
void putLive(Patch& p, const Live& l) {
  p.main[0] = l.mainVol, p.main[190] = l.modX, p.main[191] = l.modY;
  std::memcpy(p.main + 3, l.matrix, sizeof l.matrix);
  std::memcpy(p.mutes, l.mutes, sizeof l.mutes);
  for (int u = 0; u < kOps; ++u) p.units[u].u[1] = l.opVol[u];
  for (int j = 0; j < kFilters; ++j)
    for (int i = 0; i < 10; ++i)
      if (i != 0 && i != 1) p.units[kOps + j].u[i] = l.flt[j][i];  // stage count and cut depth shape the voices: structural
}
}  // namespace

bool sameStructure(const Patch& a, const Patch& b) {
  Patch x = a, y = b;
  const Live l = liveOf(a);
  putLive(x, l), putLive(y, l);
  return serialize(x, {}) == serialize(y, {});
}

void applyLive(Engine& e, const Live& l) {
  putLive(e.patch, l);
  e.mainVol = levelLaw(e.patch.main[0], 0.0078125f, false);
  e.modXg = static_cast<float>(e.patch.main[190]), e.modYg = static_cast<float>(e.patch.main[191]);
  for (int u = 0; u < kUnits; ++u) unitLive(e, u);
}

void setTempo(Engine& e, double bpm) {
  if (!(bpm > 1) || bpm == e.tempo) return;
  e.tempo = bpm;
  for (auto& R : e.units)
    for (auto& r : R.arts) {
      if (r.envTempo) r.env.speed = unitsPerTick(e, true);
      if (r.lfoTempo) {
        r.lfo.speed = unitsPerTick(e, true);
        env::setLfoSpeed(r.lfo, r.lfo.speed);
      }
    }
}

void noteOn(Engine& e, uint32_t id, double pitchCents, double vol, double pan, double modX, double modY, double velocity, int color) {
  const int32_t* M = e.patch.main;
  VoiceInit base;
  base.id = id;
  base.flVol = static_cast<float>(vol), base.flPan = static_cast<float>(pan), base.flCut = static_cast<float>(modX), base.flRes = static_cast<float>(modY);
  base.pitch = static_cast<float>(e.masterPitch) + static_cast<float>(pitchCents);
  base.porta = ((e.patch.opt[4] != 0) != (color == 14)) && e.hasLast;
  base.keyCents = static_cast<int>(std::lrint(static_cast<float>(pitchCents)));
  if (color == 15) base.keyCents -= 100000;
  e.waitArts = false;
  base.portaFrom = e.lastPitch;
  float key = (base.porta && e.patch.opt[6] ? e.lastPitch : base.pitch) * 0.01f + 60.0f;
  if (key <= 0.0f) key = 0.0f;
  base.key = key;
  base.vel = static_cast<float>(velocity);
  // unison (order, pan, volume spread, detune, sub layer, phase spread, envelope variation)
  const int count = std::clamp(M[173], 1, 9);
  const int mode = e.patch.opt[11];
  const int modeIdx = std::clamp(mode, 0, 3);
  double dd;
  if (mode == 0) dd = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(M[176]) * 0.00390625f) * 2.3978952727983707) - 1.0)) * 5.0 * kCentsLn;
  else dd = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(static_cast<float>(M[176]) * 0.00390625f) * 3.713572066704308) - 1.0)) * 1.875 * kCentsLn;
  const float detune = static_cast<float>(dd);
  const float phSpread = static_cast<float>(M[178]) * 0.0078125f, volSpread = static_cast<float>(M[175]) * 0.0078125f;
  float norm = static_cast<float>(1.0 / static_cast<double>(count));
  if (1 < count) norm = 1.0f - static_cast<float>(std::exp(static_cast<double>(1.0f - norm) * 1.0986122886681098) - 1.0) * 0.5f;
  float layerGain[2] = {norm, 0};
  if (M[177] != 0) {
    const float s1 = static_cast<float>(M[177]) * 0.0078125f;
    layerGain[0] = (1.0f - s1) * norm;
    layerGain[1] = norm * s1;
  }
  const bool shared = e.patch.opt[0] != 0 && !(count < 2 && M[177] < 1);
  const int layers = M[177] != 0 ? 2 : 1;
  Voice* first = nullptr;
  Voice* prev = nullptr;
  for (int layer = 0; layer < layers; ++layer)
    for (int k = 0; k < count; ++k) {
      VoiceInit vi = base;
      const bool isFirst = prev == nullptr;
      vi.role = 0, vi.bufOfs = 0;
      if (shared) {
        vi.role = isFirst ? -1 : 1;
        if (first != prev || isFirst) vi.bufOfs = 6;
      }
      const float g54 = layerGain[layer];
      if (count == 1) {
        vi.uniPan = 0.5f, vi.uniVol = g54, vi.uniPitch = 1.0f, vi.phaseOfs = 0.0f, vi.uniPos = 0.5f, vi.envVar = 0.0078125f;
      } else {
        vi.phaseOfs = (static_cast<float>(k) / static_cast<float>(count)) * phSpread;
        vi.uniPos = static_cast<float>(k) / static_cast<float>(count - 1);
        float c = vi.uniPos * 2.0f - 1.0f;
        const float pos50 = vi.role == 0 ? c * 0.00390625f : 0.0f;
        vi.uniPan = static_cast<float>(M[174]) * pos50 + 0.5f;
        c = std::fabs(c);
        const float g58 = (mode == 0 || count != 2) ? (1.0f - c) * g54 * 2.0f : g54;
        vi.uniVol = (1.0f - volSpread) * g58 + g54 * volSpread;
        if (k & 1) c = -c;
        if (mode != 0 && (count & 1) && static_cast<long>(count) < static_cast<long>(k) * 2) c = -c;
        const int idx = (layer + k) % 9;
        const float f = kUniDetune[modeIdx][idx];
        vi.uniPitch = static_cast<float>(std::exp(static_cast<double>(c * f * detune)));
        if (mode == 3) {
          double d = static_cast<double>(phSpread * phSpread) * static_cast<double>(phSpread * phSpread);
          d = static_cast<double>(100000.0f * vi.uniPitch) * d * d;
          if (layer > 0) d = static_cast<double>(0.5f) * d;
          vi.phaseOfs = static_cast<float>(d - std::trunc(d));
        }
        vi.envVar = (static_cast<float>(M[179]) * (std::fabs(c) - 0.5f) * kUniEnvVar[idx] * 0.0078125f + 1.0f) * 0.0078125f;
      }
      vi.uniGain = vi.uniVol;
      if (vi.role < 0) vi.uniVol = 1.0f;
      if (layer > 0) {
        vi.uniPitch = vi.uniPitch * 0.5f;
        vi.key = vi.key - 12.0f;
        if (vi.key <= 0.0f) vi.key = 0.0f;
      }
      vi.chained = !isFirst;
      Voice* v = spawnVoice(e, vi);
      if (!v) continue;
      if (isFirst) ++e.mainVoices;
      if (isFirst) first = v;
      else if (first) first->chain.push(v);
      prev = v;
    }
  e.stepDirty = e.waitArts;
  e.hasLast = true;
  e.lastPitch = base.pitch;
}

void noteOff(Engine& e, uint32_t id) {
  e.stepDirty = e.waitArts;
  for (int i = 0; i < e.nOrder; ++i) {
    Voice& v = e.voices[static_cast<size_t>(e.order[i])];
    if (v.id != id || v.state >= 5) continue;
    v.state = 5;
    for (int u = 0; u < kUnits; ++u)
      for (size_t a = 0; a < e.units[u].arts.size(); ++a) {
        const ArtRt& r = e.units[u].arts[a];
        ArtState& s = v.units[u].arts[a];
        if (r.envOn) env::release(r.env, s.env);
        if (r.lfoOn) env::release(r.lfo, s.lfo);
      }
  }
}

void allOff(Engine& e) {
  for (int i = 0; i < e.nOrder; ++i) noteOff(e, e.voices[static_cast<size_t>(e.order[i])].id);
}

void kill(Engine& e) {
  for (auto& v : e.voices)
    if (v.state != 11) freeVoice(e, v);
  e.nOrder = 0;
  e.mainVoices = 0;
  for (auto& d : e.delays) std::fill(d.buf.begin(), d.buf.end(), 0.f);
  std::fill(e.chorus.buf.begin(), e.chorus.buf.end(), 0.f);
  for (auto& l : e.reverb.line) std::fill(l.begin(), l.end(), 0.f);
  e.fxCount = 0;
  e.quiet = INT64_MAX / 2;
}

void render(Engine& e, float* L, float* R, int n) {
  for (int i = 0; i < n;) {
    if (e.readPos >= kBlock) {  // next 256-frame block (events since the last one apply from its start)
      float* out = e.out.data();
      std::fill(out, out + 2 * kBlock, 0.f);
      renderBlock(e, out, kBlock);
      float peak = 0;
      for (int j = 0; j < 2 * kBlock; ++j) peak = std::max(peak, std::fabs(out[j]));
      e.quiet = peak > 1e-6f ? 0 : e.quiet + kBlock;
      e.clock += kBlock;
      e.readPos = 0;
    }
    const int k = std::min(n - i, kBlock - e.readPos);
    const float* out = e.out.data() + 2 * e.readPos;
    for (int j = 0; j < k; ++j) {
      L[i + j] += out[2 * j];
      R[i + j] += out[2 * j + 1];
    }
    e.readPos += k;
    i += k;
  }
}

bool active(const Engine& e) {
  for (int i = 0; i < e.nOrder; ++i)
    if (e.voices[static_cast<size_t>(e.order[i])].state < 11) return true;
  return e.anyFx && e.quiet < static_cast<int64_t>(e.hostSr) * 2;
}

}  // namespace gl::packs::synths::mfmref

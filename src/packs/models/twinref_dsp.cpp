// Gridline — "models" pack: Twin String reference engine (see twinref.h).
// Arithmetic order, precision (float / double) and the approximate reciprocal
// follow the reference's measured behaviour; keep them when editing.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "../synths/hex_fx.h"
#include "twinref.h"
#if (defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)) && !defined(GL_TWREF_NORCP)
#include <xmmintrin.h>
#define GL_TWREF_RCP 1
#endif

namespace gl::packs::models::strs {

namespace {

constexpr int kAllVoices = 99;   // the reference's voice table (inactive ones fill render groups)
constexpr int kLineVoices = 32;  // voices with string memory (= polyphony)
constexpr int kLine = 48000;     // samples per string / comb line
constexpr int kBlock = 256;      // engine block
constexpr int kQueue = 256;      // pending note events per block

inline float exp10f_(float x) {
#if defined(__APPLE__)
  return __exp10f(x);
#elif defined(__GLIBC__)
  return ::exp10f(x);
#else
  return std::pow(10.0f, x);
#endif
}
inline float rcp(float x) {
#ifdef GL_TWREF_RCP
  return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
#else
  return 1.0f / x;
#endif
}
// y / den with one Newton step on the approximate reciprocal.
inline float nr(float y, float den) {
  const float r = rcp(den);
  const float t = y * r;
  return (y - den * t) * r + t;
}

constexpr float kTiny = 9.99999972e-10f, kNegTiny = -9.99999972e-10f;
inline float flushTiny(float x) { return (x < kTiny && kNegTiny < x) ? 0.0f : x; }

// ------------------------------------------------------------------ free envelope (doubles)
struct Env {
  double sr = 0, peak = 0, x0 = 0, start = 0, rA = 0, rD = 0, sus = 0, rR = 0;
  uint8_t stage = 0;
  double ph = 0;
  uint8_t idle = 1;
  double out = 0, relFrom = 0;
};
void envInit(Env& e, double a, double d, double s, double sr) {
  e.sr = sr;
  e.peak = 1.0;
  e.x0 = 0;
  e.start = e.idle ? 0.0 : e.out;
  e.idle = 0;
  e.stage = 0;
  e.ph = 0;
  e.sus = s;
  e.rA = 1.0 / (sr * std::max(a, 0.001));
  e.rD = 1.0 / (sr * std::max(d, 0.001));
  e.rR = 1.0 / (0.001 * sr);
}
void envRelease(Env& e, double r) {
  if (e.stage != 2) {
    e.rR = 1.0 / (std::max(r, 0.001) * e.sr);
    e.relFrom = e.out;
    e.stage = 2;
    e.ph = 0;
  }
}
float envTick(Env& e, int n) {
  e.out = 0;
  double v;
  if (e.stage == 2) {
    const double ph = static_cast<double>(n) * e.rR + e.ph;
    e.ph = ph;
    if (1.0 <= ph) {
      e.idle = 1;
      e.ph = 1.0;
      v = e.x0;
    } else {
      v = ph * (e.x0 - e.relFrom) + e.relFrom;
    }
  } else if (e.stage == 1) {
    const double ph = static_cast<double>(n) * e.rD + e.ph;
    e.ph = ph;
    if (ph < 1.0) {
      v = ph * (e.sus - e.peak) + e.peak;
    } else {
      v = e.sus;
      if (v == 0.0 && e.x0 == 0.0) e.stage = 2, e.idle = 1;
      e.ph = 1.0;
    }
  } else if (e.stage == 0) {
    const double ph = static_cast<double>(n) * e.rA + e.ph;
    e.ph = ph;
    if (ph < 1.0) {
      v = ph * (e.peak - e.start) + e.start;
    } else {
      e.stage = 1;
      e.ph = 0;
      v = e.peak;
    }
  } else {
    return 0.0f;
  }
  e.out = v;
  if (v < 0.0) e.out = 0, v = 0;
  return static_cast<float>(v);
}

// ------------------------------------------------------------------ 2-pole high pass (float)
struct Bq {
  float f = 100, q = 1, sr = 44100, b0 = 0.992876351f, a1 = -1.97170603f, a2 = 0.971906066f;
  float x2 = 0, x1 = 0, y2 = 0, y1 = 0;
  void set(float fc, float qq, float rate) {
    if (fc == f && qq == q && rate == sr) return;
    f = fc, q = qq, sr = rate;
    const float wf = static_cast<float>((static_cast<double>(fc) * 6.2831853071795862) / static_cast<double>(rate));
    const float s = std::sin(wf), c = std::cos(wf);
    b0 = (c + 1.0f) / (s / qq + 2.0f);
    a1 = static_cast<float>((static_cast<double>(qq) * -2.0 * static_cast<double>(c)) / static_cast<double>(s + qq));
    a2 = (qq - s) / (s + qq);
    b0 = flushTiny(b0), a1 = flushTiny(a1), a2 = flushTiny(a2);
  }
  float run(float x) {
    x = flushTiny(x);
    const float t = (x - (x1 + x1)) + x2;
    const float y = flushTiny(t * b0 - (y1 * a1 + y2 * a2));
    x2 = x1, x1 = x, y2 = y1, y1 = y;
    return y;
  }
  void clear() { x1 = x2 = y1 = y2 = 0; }
};

// ------------------------------------------------------------------ clocked lines
// Samples are stored with their clock time; the read side is the previous
// clock period, interpolated linearly at the current clock.
struct Chunk {
  std::vector<float> buf;
  std::vector<double> t;
  int count = 0, readIdx = 0;
};
struct Clocked {
  Chunk c[2];
  int w = 0, r = 1;
  long n = 4;
  double clock = 0, minRate = 0;
  void alloc(long cap) {
    n = cap < 4 ? 4 : cap;
    for (auto& ch : c) {
      ch.buf.assign(static_cast<size_t>(n) + 256, 0.0f);
      ch.t.assign(static_cast<size_t>(n) + 256, 0.0);
      ch.count = 0, ch.readIdx = 0;
    }
    w = 0, r = 1;
    minRate = 1.0 / static_cast<double>(n);
  }
  void clearBufs() {
    for (auto& ch : c) {
      std::fill(ch.buf.begin(), ch.buf.end(), 0.0f);
      std::fill(ch.t.begin(), ch.t.end(), 0.0);
      ch.count = 0, ch.readIdx = 0;
    }
  }
  float read() {
    Chunk& R = c[r];
    const int cnt = R.count;
    if (cnt != 0 && R.readIdx <= cnt) {
      int i = R.readIdx + 10, nx = R.readIdx + 1;
      for (;;) {
        const double ti = R.t[static_cast<size_t>(i)];
        if (clock <= ti) {
          const float hi = static_cast<float>(ti), lo = static_cast<float>(R.t[static_cast<size_t>(i) - 1]);
          double fr = 0.0;
          if (lo < hi) fr = (clock - static_cast<double>(lo)) / static_cast<double>(hi - lo);
          const float b0 = R.buf[static_cast<size_t>(i) - 1];
          return static_cast<float>(static_cast<double>(R.buf[static_cast<size_t>(i)] - b0) * fr + static_cast<double>(b0));
        }
        R.readIdx = nx;
        ++nx, ++i;
        if (nx == cnt + 2) break;
      }
    }
    return 0.0f;
  }
  // Advances the clock by `rate` and stores x.
  void write(float x, double rate) {
    clock = clock + rate;
    if (1.0 <= clock) {
      Chunk& R = c[r];
      Chunk& W = c[w];
      const size_t rc = static_cast<size_t>(R.count);
      for (size_t k = 9; k >= 7; --k) W.buf[k] = R.buf[rc + k], W.t[k] = R.t[rc + k] + -1.0;
      W.buf[static_cast<size_t>(W.count) + 10] = x;
      W.t[static_cast<size_t>(W.count) + 10] = clock;
      W.readIdx = 0;
      clock = clock + -1.0;
      std::swap(w, r);
      Chunk& N = c[w];
      N.buf[10] = x;
      N.t[10] = clock;
      N.count = 1;
    } else {
      Chunk& W = c[w];
      W.buf[static_cast<size_t>(W.count) + 10] = x;
      W.t[static_cast<size_t>(W.count) + 10] = clock;
      ++W.count;
    }
  }
};

// Resonator line: fixed rate per sample, one-pole smoothed feedback.
struct ResLine : Clocked {
  float smooth = 0;
  double rate = 0;
  void init(long cap) {
    alloc(cap);
    double rt = 1.0 / ((0.5 + minRate) * static_cast<double>(n));
    if (0.2 <= rt) rt = 0.2;
    if (rt <= minRate) rt = minRate;
    rate = rt;
    smooth = 0;
    clock = 0;
  }
  void clear() {
    smooth = 0;
    clock = 0;
    clearBufs();
  }
};

// Swept line (chorus / delay): a triangle LFO speeds the clock up, smoothed level.
struct ModLine : Clocked {
  double ph = 0, inc = 0;
  float lvl = 0;
  double base = 0, rateS = 0, clock0 = 0;
  float fb = 0, lvlT = 0, depth = 0;
  void init(double c0, long cap) {
    alloc(cap);
    clock0 = c0;
    base = 1.0 / ((minRate + 0.5) * static_cast<double>(static_cast<unsigned long>(n)));
    inc = 0, ph = 0, lvl = 0;
    clock = clock0;
    rateS = base;
    fb = 0, lvlT = 1.0f, depth = 0;
  }
  void reset() {
    lvl = 0;
    clock = clock0;
    rateS = base;
    ph = 0;
    clearBufs();
  }
  void setTime(double t) {
    if (0.99 <= t) t = 0.99;
    const double d = 0.0 <= t ? t : 0.0;
    const double N = static_cast<double>(static_cast<unsigned long>(n));
    const double r0 = 2.0 / N;
    base = 1.0 / (((1.0 - r0) * d + r0) * N);
  }
  void setLfo(double hz, double sr) { inc = (0.0 <= hz ? hz : 0.0) / sr; }
  void setDepth(double v) {
    if (1.0 <= v) v = 1.0;
    depth = static_cast<float>(0.0 <= v ? v : 0.0);
  }
  float run(float x) {
    const float xin = flushTiny(x);
    const double p0 = ph;
    const double tri = p0 < 0.5 ? p0 + p0 : 2.0 - (p0 + p0);
    double m = tri * tri * static_cast<double>(depth) * (3.0 - (tri + tri));
    double p1 = p0 + inc;
    if (!(p1 < 1.0)) p1 = p1 - std::trunc(p1);
    ph = p1;
    const float y = flushTiny(read());
    m = m + 1.0;
    if (m <= 1.0) m = 1.0;
    const float lvlOld = lvl;
    double rt = m * rateS;
    if (0.2 <= rt) rt = 0.2;
    if (rt <= minRate) rt = minRate;
    write(fb * y + xin, rt);
    lvl = flushTiny(static_cast<float>(static_cast<double>(lvl) + static_cast<double>(lvlT - lvl) * 0.01));
    rateS = (base - rateS) * 0.005 + rateS;
    return lvlOld * y;
  }
};

// ------------------------------------------------------------------ voice
struct Voice {
  int order = 0;
  bool gate = false, active = false;
  float egv = 0, lfo = 0, vel = 0, rel = 0;
  double keyD = 0;
  int key = 0;
  uint32_t id = 0;
  Env env;
  float noiseThr = 0, drive = 0, ampRate = 0, ampTarget = 0, amp = 0;
  double frac = 0, rate = 0, freq2 = 0, lenRate = 0;
  float prevA = 0, curA = 0, prevB = 0, curB = 0;
  double t = 0, tRel = -1;
  float lad[4] = {};
  float hcRate = 0, hcTarget = 0, hcEnv = 0, hcCut = 0, hcRes = 0;
  float noiseGain = 0, noiseOfs = 0;
  float lc[4] = {};  // x2, x1, y2, y1
  float lcB0 = 0, lcA1 = 0, lcA2 = 0;
  float* line[6] = {};  // string 1 lanes, string 2 lanes, comb 1, comb 2 (nullptr: no string memory)
  int pos[6] = {}, end[6] = {};
  float combLevel = 0, combFb = 0, sat = 0;
  float dm1[4] = {}, dm2[4] = {}, dc[4] = {}, damp[4] = {}, sharp[4] = {}, decT[4] = {}, dec[4] = {}, gainT[4] = {}, gain[4] = {};
  float noise = 0, dcK1 = 0, dcK2 = 0;
  int quiet = -1;
};

void resetVoice(Voice& v) {
  v.active = false;
  v.gate = false;
  v.env.idle = 1;
  v.frac = 0;
  v.amp = 0;
  v.ampRate = v.ampTarget = 0;
  v.prevA = v.curA = v.prevB = v.curB = 0;
  for (int j = 0; j < 4; ++j) v.lad[j] = v.lc[j] = 0;
  v.hcRate = v.hcTarget = v.hcEnv = v.hcCut = v.hcRes = 0;
  v.lcB0 = v.lcA1 = v.lcA2 = 0;
  for (int k = 0; k < 6; ++k) {
    if (v.line[k]) std::fill(v.line[k], v.line[k] + kLine, 0.0f);
    v.pos[k] = 0;
  }
  v.drive = 0;
  for (int j = 0; j < 4; ++j) v.dm1[j] = v.dm2[j] = v.dc[j] = v.damp[j] = v.sharp[j] = v.decT[j] = v.gain[j] = v.gainT[j] = 0;
  v.quiet = -1;
  v.rate = 0;
  v.egv = v.lfo = 0;
}

struct LaneOut {
  float a = 0, b = 0;
  bool on = false;
};

inline void exciter(Voice& v, float r) {
  if (v.noiseThr < r) v.noise = r;
  const float x = v.noise * v.noiseGain + v.noiseOfs;
  const float x2 = v.lc[0], x1 = v.lc[1], y2 = v.lc[2], y1 = v.lc[3];
  const float y = (y1 * -v.lcA1 - y2 * v.lcA2) + ((x2 - (x1 + x1)) + x) * v.lcB0;
  v.lc[0] = x1, v.lc[1] = x, v.lc[2] = y1, v.lc[3] = y;
  v.hcEnv = v.hcEnv + (v.hcTarget - v.hcEnv) * v.hcRate;
  const float g = v.hcEnv * v.hcCut;
  v.lad[0] = v.lad[0] + ((v.lad[3] * v.hcRes - v.lad[0]) + y) * g;
  v.lad[1] = v.lad[1] + (v.lad[0] - v.lad[1]) * g;
  v.lad[2] = v.lad[2] + (v.lad[1] - v.lad[2]) * g;
  v.lad[3] = (v.lad[2] - v.lad[3]) * g + v.lad[3];
  v.lad[3] = nr(v.lad[3], v.lad[3] * v.lad[3] * v.drive + 1.0f);
}

inline LaneOut strings(Voice& v) {
  LaneOut o;
  if (!v.active) return o;
  o.on = true;
  const double dpos = v.frac + v.rate;
  int steps = static_cast<int>(dpos);
  const double frac = dpos - std::trunc(dpos);
  v.frac = frac;
  float curA = v.curA, curB = v.curB;
  const float e = v.lad[3];
  float* const* L = v.line;
  while (steps != 0) {
    v.prevA = curA;
    v.prevB = curB;
    for (int k = 0; k < 6; ++k)
      if (v.pos[k] >= v.end[k]) v.pos[k] = 0;
    v.dc[0] = v.dc[0] + (e - v.dc[0]) * v.dcK1;
    v.dc[1] = v.dc[1] + (e - v.dc[1]) * v.dcK1;
    v.dc[2] = v.dc[2] + (e - v.dc[2]) * v.dcK2;
    v.dc[3] = v.dc[3] + (e - v.dc[3]) * v.dcK2;
    const float in[4] = {L[0][v.pos[0]], L[1][v.pos[1]], L[2][v.pos[2]], L[3][v.pos[3]]};
    float out[4];
    for (int j = 0; j < 4; ++j) {
      v.dm1[j] = v.dm1[j] + ((v.dm2[j] * v.sharp[j] - v.dm1[j]) + in[j]) * v.damp[j];
      v.dm2[j] = (v.dm1[j] - v.dm2[j]) * v.damp[j] + v.dm2[j];
      v.dm2[j] = nr(v.dm2[j], v.dm2[j] * v.dm2[j] * v.sat + 1.0f);
      out[j] = v.dm2[j] * v.dec[j] + (e - v.dc[j]);
    }
    for (int j = 0; j < 4; ++j) L[j][v.pos[j]] = out[j];
    const float c0 = in[0] + v.combLevel * L[4][v.pos[4]];
    const float c1 = in[2] + v.combLevel * L[5][v.pos[5]];
    const float oa = v.gain[0] * c0 + v.gain[2] * c1;
    const float ob = v.gain[3] * c1 + v.gain[1] * c0;
    L[4][v.pos[4]] = v.combFb * L[4][v.pos[4]] + in[1];
    L[5][v.pos[5]] = v.combFb * L[5][v.pos[5]] + in[3];
    for (int k = 0; k < 6; ++k) ++v.pos[k];
    for (int j = 0; j < 4; ++j) v.dec[j] = v.dec[j] + (v.decT[j] - v.dec[j]) * 0.0500000007f;
    for (int j = 0; j < 4; ++j) v.gain[j] = v.gain[j] + (v.gainT[j] - v.gain[j]) * 0.00100000005f;
    v.curA = oa, v.curB = ob;
    curA = oa, curB = ob;
    --steps;
  }
  const float dB = curB - v.prevB, dA = curA - v.prevA;
  const float amp = v.amp;
  o.b = static_cast<float>(static_cast<double>(amp) * (frac * static_cast<double>(dB) + static_cast<double>(v.prevB)));
  o.a = static_cast<float>(static_cast<double>(amp) * (frac * static_cast<double>(dA) + static_cast<double>(v.prevA)));
  if (std::fabs(o.b) < 9.99999994e-09f && std::fabs(o.a) < 9.99999994e-09f) {
    if (v.quiet > -1) ++v.quiet;
  } else {
    v.quiet = 0;
  }
  float na = (v.ampTarget - amp) * v.ampRate + amp;
  if (1.0f <= na) na = 1.0f;
  v.amp = na;
  return o;
}

struct Event {
  enum Kind : uint8_t { On, Off, AllOff } kind = On;
  int key = 0;
  uint32_t id = 0;
  float vel = 0, l = 0, r = 0, pitch = 0;
};

}  // namespace

// ------------------------------------------------------------------ engine
struct TwinRef::Engine {
  float tag[TsCount] = {};
  bool tagSet = false;
  // converted controls
  float level = 0, tune = 0, transpose = 0;
  float s1Decay = 0, s1DampAmt = 0, s1Sharp = 0, s1DampOfs = 0;
  float s2Decay = 0, s2DampAmt = 0, s2Sharp = 0, s2DampOfs = 0;
  bool s1Inv = false, s2Inv = false;
  int ratio = 0;
  float satur = 0, mix = 0, stereo = 0, combRatio = 0, combLevel = 0, combFb = 0;
  bool combLevelInv = false, combFbInv = false;
  float hcA = 0, hcD = 0, hcS = 0, hcCut = 0, hcRes = 0, lcCut = 0, lcRes = 0;
  float ampA = 0, ampD = 0, ampS = 0, ampR = 0, egA = 0, egD = 0, egS = 0, egR = 0;
  float lfoRate = 0, noiseLvl = 0, noiseRate = 0, drive = 0;
  float lfoPh = 0, lfoCos = 1;
  float modWheel = 0, bend = 0, pressure = 0;
  float keyL[128] = {}, keyR[128] = {}, keyPitch[128] = {};
  bool keyDown[128] = {};
  Voice voices[kAllVoices];
  Voice dummy;
  std::vector<float> lineMem;
  int counter = 0;
  double sr = 0, bpm = 120;
  double pos = 0;
  int need = 0;
  float bufL[256] = {}, bufR[256] = {};
  uint32_t seed = 0;
  Bq dcL, dcR;
  struct {
    float frq[8] = {}, fb[8] = {}, on[8] = {};
    ResLine a[8], b[8];
    Bq hpL, hpR;
    bool ready = false;
  } res;
  struct {
    bool on = false, ready = false;
    float depthP = 0, levelP = 0, rateP = 0;
    ModLine u[3];
  } cho;
  struct {
    bool on = false;
    float time = 0, sync = 0, fb = 0, speed = 0, depth = 0, blur = 0, wet = 0;
    ModLine u[6];
  } dly;
  bool revOn = false;
  synths::hex::PlateReverb rev;
  // host side
  Event queue[kQueue];
  int nQueue = 0;
  struct IdKey {
    uint32_t id;
    int key;
  } held[128] = {};
  int nHeld = 0;
  float out[2 * kBlock] = {};
  int readPos = kBlock;
  int64_t quiet = INT64_MAX / 2;  // frames since the output was last audible

  Engine() {
    lineMem.assign(static_cast<size_t>(kLineVoices) * 6 * kLine, 0.0f);
    for (int i = 0; i < kAllVoices; ++i) {
      if (i < kLineVoices)
        for (int k = 0; k < 6; ++k) voices[i].line[k] = lineMem.data() + (static_cast<size_t>(i) * 6 + static_cast<size_t>(k)) * kLine;
      resetVoice(voices[i]);
    }
    resetVoice(dummy);
  }

  void prepare(double rate) {
    sr = rate;
    const long cap = static_cast<long>(0.1 * rate);
    for (int k = 0; k < 8; ++k) res.a[k].init(cap), res.b[k].init(cap);
    res.hpL.set(32.0f, 1.0f, static_cast<float>(rate));
    res.hpR.set(32.0f, 1.0f, static_cast<float>(rate));
    res.ready = true;
    cho.u[0].init(0.0, static_cast<long>(static_cast<unsigned long>(0.021999999508261682 * rate)));
    cho.u[1].init(0.467, static_cast<long>(static_cast<unsigned long>(0.017079999618232249 * rate)));
    cho.u[2].init(0.778, static_cast<long>(static_cast<unsigned long>(rate * 0.019999999552965164)));
    {
      const float lv = cho.levelP;
      cho.levelP = -lv;
      for (auto& u : cho.u) u.lvlT = lv;
    }
    choLfo(cho.rateP);
    for (auto& u : cho.u) u.setDepth(static_cast<double>(cho.depthP * cho.depthP * cho.depthP));
    for (auto& u : cho.u) u.reset();
    cho.ready = true;
    rev.configure(rate);
    rev.setParam(9, 1.0);
    rev.setParam(5, 1.0);
    {
      const unsigned long c0 = static_cast<unsigned long>(rate + rate);
      const double dc = static_cast<double>(c0);
      dly.u[0].init(0.0, static_cast<long>(c0));
      dly.u[1].init(0.27, static_cast<long>(static_cast<unsigned long>(0.63 * dc)));
      dly.u[2].init(0.4, static_cast<long>(static_cast<unsigned long>(0.41 * dc)));
      dly.u[3].init(0.3, static_cast<long>(c0));
      dly.u[4].init(0.62, static_cast<long>(static_cast<unsigned long>(0.76 * dc)));
      dly.u[5].init(0.87, static_cast<long>(static_cast<unsigned long>(dc * 0.54)));
      for (auto& u : dly.u) u.reset();
    }
    dcL.set(32.0f, 1.0f, static_cast<float>(rate));
    dcR.set(32.0f, 1.0f, static_cast<float>(rate));
  }

  void choLfo(float p) {
    const double lr = static_cast<double>(p) * 5.0 + 0.1;
    cho.u[0].setLfo(0.896 * lr, sr);
    cho.u[1].setLfo(lr, sr);
    cho.u[2].setLfo(lr * 0.689, sr);
  }

  void setParam(int id, float p) {
    tag[id] = p;
    const float p2 = p * p, p4 = p2 * p2, p5 = p4 * p;
    switch (id) {
      case TsLevel: level = exp10f_(p * 4.80000019f + -1.80000007f); break;
      case TsTune: tune = static_cast<float>(static_cast<double>(std::clamp(static_cast<int>(p * 198.0f + -99.0f), -99, 99)) * 0.01); break;
      case TsAmpA: ampA = static_cast<float>(static_cast<double>(p5) * 35.0 + 0.001); break;
      case TsAmpD: ampD = static_cast<float>(static_cast<double>(p5) * 50.0 + 0.001); break;
      case TsAmpS: ampS = p * p; break;
      case TsAmpR: ampR = static_cast<float>(static_cast<double>(p5) * 10.0 + 0.1); break;
      case TsS1Decay: s1Decay = p; break;
      case TsS1Inv: s1Inv = p != 0.0f; break;
      case TsS1DampOfs: s1DampOfs = p * p; break;
      case TsS1Damp: s1DampAmt = p; break;
      case TsS1Sharp: s1Sharp = p * p; break;
      case TsS2Decay: s2Decay = p; break;
      case TsS2Inv: s2Inv = p != 0.0f; break;
      case TsS2DampOfs: s2DampOfs = p * p; break;
      case TsS2Damp: s2DampAmt = p; break;
      case TsS2Sharp: s2Sharp = p * p; break;
      case TsSaturation: satur = p * p; break;
      case TsMix: mix = p; break;
      case TsStereo: stereo = p; break;
      case TsCombRatio: combRatio = p; break;
      case TsCombFb: combFb = std::sqrt(p); break;
      case TsCombFbInv: combFbInv = p != 0.0f; break;
      case TsCombLevel: combLevel = p; break;
      case TsCombLevelInv: combLevelInv = p != 0.0f; break;
      case TsLoCut: lcCut = p; break;
      case TsLoRes: lcRes = p; break;
      case TsHiAtk: hcA = p; break;
      case TsHiDec: hcD = p; break;
      case TsHiSus: hcS = p; break;
      case TsHiCut: hcCut = p; break;
      case TsHiRes: hcRes = p; break;
      case TsNoiseLevel: noiseLvl = 1.0f - p; break;
      case TsNoiseRate: noiseRate = p; break;
      case TsDrive: drive = p4 * 1000.0f; break;
      case TsEgA: egA = static_cast<float>(static_cast<double>(p * p * p) * 10.0 + 0.001); break;
      case TsEgD: egD = static_cast<float>(static_cast<double>(p5) * 50.0 + 0.001); break;
      case TsEgS: egS = p * p; break;
      case TsEgR: egR = static_cast<float>(static_cast<double>(p5) * 50.0 + 0.1); break;
      case TsLfoRate: lfoRate = static_cast<float>(static_cast<double>(p * p * p) * 10.0 + 0.01); break;
      case TsDlyOn: dly.on = p != 0.0f; break;
      case TsDlyTime: dly.time = p; break;
      case TsDlyFb: dly.fb = p; break;
      case TsDlySpeed: dly.speed = p; break;
      case TsDlyDepth: dly.depth = p; break;
      case TsDlyBlur: dly.blur = p; break;
      case TsDlyWet: dly.wet = p; break;
      case TsDlySync: dly.sync = p; break;
      case TsChoOn: cho.on = p != 0.0f; break;
      case TsChoRate:
        cho.rateP = p;
        if (cho.ready) choLfo(p);
        break;
      case TsChoDepth:
        cho.depthP = p;
        if (cho.ready)
          for (auto& u : cho.u) u.setDepth(static_cast<double>(p * p * p));
        break;
      case TsChoWet:
        cho.levelP = -p;
        if (cho.ready)
          for (auto& u : cho.u) u.lvlT = p;
        break;
      case TsRevOn: revOn = p != 0.0f; break;
      case TsRevMix: rev.setParam(12, p * 0.999000013f + 0.00100000005f); break;
      case TsRevDecay: rev.setParam(8, p * p * 0.600000024f * p); break;
      case TsRevColor: rev.setParam(2, p * p * p); break;
      case TsS2Ratio: ratio = static_cast<int>(p); break;
      case TsTranspose: transpose = p; break;
      default:
        if (id >= TsResFreq1 && id < TsResFreq1 + kTsRes) res.frq[id - TsResFreq1] = 1.0f - std::pow(p, 0.100000001f);
        else if (id >= TsResFb1 && id < TsResFb1 + kTsRes) res.fb[id - TsResFb1] = static_cast<float>(static_cast<double>(std::sqrt(p)) * 0.9999);
        else if (id >= TsResOn1 && id < TsResOn1 + kTsRes) {
          const int k = id - TsResOn1;
          res.on[k] = p != 0.0f ? 1.0f : 0.0f;
          if (p != 0.0f && res.ready) res.a[k].clear(), res.b[k].clear();
        }
        break;
    }
  }

  float mod(const Voice& v, int dest) const {
    bool any = false;
    float sum = 0.0f;
    for (int s = 0; s < kTsMods; ++s) {
      const float src = tag[tsMod(s, TmSource)], amt = tag[tsMod(s, TmAmount)], neg = tag[tsMod(s, TmNegative)];
      if (static_cast<int>(tag[tsMod(s, TmDest)]) != dest) continue;
      const int k = static_cast<int>(src);
      if (k != 0) any = true;
      if (static_cast<unsigned>(k - 1) < 8u) {
        float m = amt * amt;
        if (neg != 0.0f) m = -m;
        float val = 0;
        switch (k) {
          case MsEnv: val = v.egv; break;
          case MsLfo: val = v.lfo; break;
          case MsModWheel: val = modWheel; break;
          case MsPitchWheel: val = bend; break;
          case MsPressure: val = pressure; break;
          case MsVelocity: val = v.vel; break;
          case MsKey: sum = static_cast<float>(static_cast<double>(sum) + static_cast<double>(m) * 0.0125 * v.keyD); continue;
          case MsRelease: val = v.rel; break;
          default: break;
        }
        sum = sum + val * m;
      }
    }
    if ((dest == MdLevel || dest == MdLfoDepth) && !any) return 1.0f;
    return sum;
  }

  void start(Voice& v, float vel, int key) {
    v.key = key;
    v.keyD = static_cast<double>(key);
    const double f = std::exp2(static_cast<double>(key) * 0.083333333333333329 + -4.75);
    double fr = 220.0 * f;
    v.tRel = -1.0;
    v.t = 0;
    v.rel = 0;
    v.vel = vel;
    v.gate = true;
    v.freq2 = fr;
    const unsigned tr = static_cast<unsigned>(static_cast<double>(transpose) * 6.0);
    static const double kBase[7] = {27.5, 55, 110, 0, 440, 880, 1760};
    if (tr < 7 && ((0x77u >> tr) & 1u)) {
      fr = f * kBase[tr];
      v.freq2 = fr;
    }
    int len = static_cast<int>(static_cast<unsigned>(48000.0 / fr));
    int len2 = len;
    switch (ratio) {
      case 0: len2 = (len < 0 ? len + 3 : len) >> 2, len = len2 * 4; break;
      case 1: len2 = len / 3, len = len2 * 3; break;
      case 2: len2 = len / 2, len = len & ~1; break;
      case 4: len2 = len * 2; break;
      case 5: len2 = len * 3; break;
      case 6: len2 = len * 4; break;
      default: break;
    }
    if (len2 < 2) len2 = 1;
    const int l1 = std::min(len, kLine), l2 = std::min(len2, kLine);
    v.lenRate = 48000.0 / static_cast<double>(l1);
    v.end[0] = v.end[1] = l1;
    v.end[2] = v.end[3] = l2;
    v.dcK1 = static_cast<float>(1.0 / static_cast<double>(l1));
    v.dcK2 = static_cast<float>(1.0 / static_cast<double>(l2));
    int c1 = static_cast<int>(static_cast<float>(l1) * combRatio);
    c1 = c1 < 2 ? 1 : std::min(c1, kLine);
    int c2 = static_cast<int>(static_cast<float>(l2) * combRatio);
    c2 = c2 < 2 ? 1 : std::min(c2, kLine);
    v.end[4] = c1, v.end[5] = c2;
    envInit(v.env, egA, egD, egS, 48000.0);
    v.lc[0] = v.lc[1] = v.lc[2] = v.lc[3] = 0;
    v.active = true;
  }

  void noteOn(int key, float vel, uint32_t id) {
    Voice* pick = nullptr;
    for (int i = 0; i < kLineVoices; ++i)
      if (!voices[i].active) {
        pick = &voices[i];
        break;
      }
    if (!pick) {  // the oldest; later voices win ties
      for (int i = 0; i < kLineVoices; ++i)
        if (!pick || static_cast<unsigned>(voices[i].order) <= static_cast<unsigned>(pick->order)) pick = &voices[i];
      resetVoice(*pick);
    }
    start(*pick, vel, key);
    pick->id = id;
    pick->order = counter++;
  }

  void noteOff(int key) {
    for (int i = 0; i < kLineVoices; ++i) {
      Voice& v = voices[i];
      if (v.key == key && v.active) {
        v.rel = 1.0f;
        envRelease(v.env, static_cast<double>(egR));
        v.tRel = 0;
      }
    }
  }

  void control(Voice& v, int n) {
    if (!v.active) return;
    v.sat = satur * satur * satur * 100.0f;
    v.rate = v.freq2 / v.lenRate;
    {
      const float a = (tune + keyPitch[v.key]) * 0.0833333358f + mod(v, MdPitch) * 0.166666672f;
      v.rate = std::exp2(static_cast<double>(a)) * v.rate;
    }
    v.egv = envTick(v.env, n);
    v.lfo = mod(v, MdLfoDepth) * lfoCos;
    {
      const float f = std::clamp(mod(v, MdNoiseRate) + noiseRate, 0.0f, 1.0f);
      v.noiseThr = static_cast<float>(std::sqrt(std::sqrt(1.0 - static_cast<double>(f))) * 0.996);
    }
    v.drive = drive;
    v.noiseGain = noiseLvl * noiseLvl;
    v.noiseOfs = 1.0f - noiseLvl;
    {
      const float ma = mod(v, MdHiAtk), md = mod(v, MdHiDec);
      if (v.tRel <= -1.0) {
        const float aa = std::clamp(ma + hcA, 0.0f, 1.0f);
        const float t = 0.5f * aa * aa;
        if (v.t <= static_cast<double>(t)) {
          v.hcRate = 0.00100000005f <= t ? 0.00039999999f / (aa * aa) : 1.0f;
          v.hcTarget = 1.0f;
        } else {
          const float dd = std::clamp(md + hcD, 0.0f, 1.0f);
          v.hcRate = static_cast<float>(0.0002 / static_cast<double>(static_cast<float>(static_cast<double>(dd * dd * dd) * 3.0 + 0.001)));
          v.hcTarget = hcS * hcS;
        }
      } else {
        v.hcRate = 0.00199999986f;
        v.hcTarget = 0.0f;
      }
    }
    {
      float c = static_cast<float>(static_cast<double>(mod(v, MdHiCut)) * 0.5 + static_cast<double>(hcCut));
      c = c * c;
      if (1.0f <= c) c = 1.0f;
      v.hcCut = c;
      float r = static_cast<float>((static_cast<double>(c) - 1.0) * static_cast<double>(hcRes) * 10.0);
      if (r <= -4.0f) r = -4.0f;
      v.hcRes = r;
    }
    {
      float f = static_cast<float>(static_cast<double>(mod(v, MdLoCut)) * 0.5 + static_cast<double>(lcCut));
      f = std::clamp(f, 0.0f, 1.0f);
      const double w = std::exp2(static_cast<double>(f) * 9.3 + 5.0);
      const double q = static_cast<double>(static_cast<float>(static_cast<double>(lcRes) * 10.0 + 1.0));
      const double x = w * 0.0001308996938995747;
      const double s = std::sin(x), co = std::cos(x);
      v.lcB0 = static_cast<float>((co + 1.0) / (2.0 + s / q));
      v.lcA1 = static_cast<float>((-2.0 * q * co) / (q + s));
      v.lcA2 = static_cast<float>((q - s) / (q + s));
    }
    auto damping = [&](float sharpP, float amt, float ofs, int mB, int j0) {
      const float k = sharpP * -0.5f;
      const float f = std::clamp(mod(v, MdBothDamp) + mod(v, mB) + amt, 0.0f, 1.0f);
      const double e = std::exp2(14.50471040772104 - static_cast<double>(f * f * f) * 5.0766486427023638);
      const float g = static_cast<float>(e) * 4.53514731e-05f;
      const float a = std::min(g, 1.0f);
      v.damp[j0] = a;
      v.sharp[j0] = (1.0f - a) * k;
      const float b = std::min((1.0f - ofs) * g, 1.0f);
      v.damp[j0 + 1] = b;
      v.sharp[j0 + 1] = (1.0f - b) * k;
    };
    auto decay = [&](float base, int mB, bool inv, int j0) {
      const float f = std::clamp(mod(v, MdBothDecay) + mod(v, mB) + base, 0.0f, 1.0f);
      float g = std::pow(f, 0.0500000007f);
      if (inv) g = -g;
      v.decT[j0] = v.decT[j0 + 1] = g;
    };
    damping(s1Sharp, s1DampAmt, s1DampOfs, MdS1Damp, 0);
    decay(s1Decay, MdS1Decay, s1Inv, 0);
    damping(s2Sharp, s2DampAmt, s2DampOfs, MdS2Damp, 2);
    decay(s2Decay, MdS2Decay, s2Inv, 2);
    {
      const float pan = std::clamp(mod(v, MdPan) + stereo, 0.0f, 1.0f);
      const float m1 = mod(v, MdLevel);
      const float g = exp10f_(m1 + m1) * 0.0500000007f * (1.0f - mix);
      v.gainT[0] = static_cast<float>(static_cast<double>(keyL[v.key]) * static_cast<double>(g) * (1.0 - static_cast<double>(pan)));
      v.gainT[1] = g * pan * keyR[v.key];
      const float pan2 = std::clamp(mod(v, MdPan) + (1.0f - stereo), 0.0f, 1.0f);
      const float m1b = mod(v, MdLevel);
      const float g2 = exp10f_(m1b + m1b) * 0.0500000007f * mix;
      v.gainT[2] = static_cast<float>(static_cast<double>(keyL[v.key]) * static_cast<double>(g2) * (1.0 - static_cast<double>(pan2)));
      v.gainT[3] = g2 * pan2 * keyR[v.key];
    }
    v.combFb = combFbInv ? -combFb : combFb;
    v.combLevel = combLevelInv ? -combLevel : combLevel;
    const double dt = static_cast<double>(n) * 2.0833333333333333e-05;
    if (v.tRel <= -1.0) {
      if (static_cast<double>(ampA) <= v.t) {
        v.ampTarget = ampS;
        v.ampRate = static_cast<float>(0.00018375 / static_cast<double>(ampD));
      } else {
        v.ampTarget = 5.0f;
        v.ampRate = static_cast<float>(4.6587423677440092e-06 / static_cast<double>(ampA));
      }
    } else {
      if (static_cast<double>(ampR) < v.tRel) {
        resetVoice(v);
        return;
      }
      v.ampTarget = 0.0f;
      v.ampRate = static_cast<float>(0.00018375 / static_cast<double>(ampR));
      v.tRel = v.tRel + dt;
    }
    if (v.gate)
      for (int j = 0; j < 4; ++j) v.dec[j] = v.decT[j], v.gain[j] = v.gainT[j];
    v.t = dt + v.t;
  }

  void render4(Voice* const* lane, float* outL, float* outR, int n, bool first) {
    for (int i = 0; i < n; ++i) {
      const uint32_t s0 = seed;
      const uint32_t s1 = s0 * 0xbb38435u + 0x3619636bu;
      const uint32_t s2 = s1 * 0xbb38435u + 0x3619636bu;
      const uint32_t s3 = s2 * 0xbb38435u + 0x3619636bu;
      seed = s3 * 0xbb38435u + 0x3619636bu;
      const uint32_t sv[4] = {s0, s1, s2, s3};
      for (int k = 0; k < 4; ++k) exciter(*lane[k], static_cast<float>(sv[k]) * 2.32830644e-10f);
      float sumA = 0, sumB = 0;
      bool any = false;
      for (int k = 0; k < 4; ++k) {
        const LaneOut o = strings(*lane[k]);
        if (!o.on) continue;
        if (!any) sumA = o.a, sumB = o.b, any = true;
        else sumB = sumB + o.b, sumA = sumA + o.a;
      }
      if (first) outL[i] = sumA, outR[i] = sumB;
      else outL[i] = sumA + outL[i], outR[i] = sumB + outR[i];
    }
  }

  void mixVoices(float* L, float* R, int n) {
    for (Voice& v : voices) control(v, n);
    Voice* list[2 * kAllVoices + 4];
    int nList = 0, nAct = 0;
    for (int i = 0; i < kLineVoices; ++i)
      if (voices[i].active) list[nList++] = &voices[i], ++nAct;
    for (int i = 0; i < kAllVoices; ++i)
      if (!voices[i].active) list[nList++] = &voices[i];
    list[nList++] = &dummy, list[nList++] = &dummy, list[nList++] = &dummy;
    if (nAct < 1) return;
    auto group = [&](int g, bool first) {
      render4(list + g, L, R, n, first);
      for (int k = 0; k < 4; ++k)
        if (list[g + k]->quiet > 1000) resetVoice(*list[g + k]);
    };
    group(0, true);
    if (nAct > 4) {
      int u = nAct + 4, g = 0;
      do {
        g += 4;
        group(g, false);
        u -= 4;
      } while (8 < u);
    }
  }

  void resonators(float* L, float* R, int n) {
    for (int i = 0; i < n; ++i) {
      const float in = flushTiny(R[i] + L[i]);
      float accL = 0, accR = 0;
      for (int k = 0; k < 8; ++k) {
        if (res.on[k] == 0.0f) continue;
        ResLine& A = res.a[k];
        ResLine& B = res.b[k];
        const double f = static_cast<double>(res.frq[k]);
        auto rateOf = [](double d, const ResLine& l) {
          if (0.99 <= d) d = 0.99;
          if (d <= 0.0) d = 0.0;
          const double N = static_cast<double>(l.n);
          const double r0 = 2.0 / N;
          double rt = 1.0 / (((1.0 - r0) * d + r0) * N);
          if (0.2 <= rt) rt = 0.2;
          if (rt < l.minRate) rt = l.minRate;
          return rt;
        };
        A.rate = rateOf(f, A);
        B.rate = rateOf(f * 1.05, B);
        const float fb = res.fb[k];
        const float ra = flushTiny(A.read());
        A.smooth = (ra - A.smooth) * 0.5f + A.smooth;
        A.write(A.smooth * fb + in, A.rate);
        const float inB = flushTiny(-ra);
        const float rb = flushTiny(B.read());
        B.smooth = (rb - B.smooth) * 0.5f + B.smooth;
        B.write(B.smooth * fb + inB, B.rate);
        const double wl = static_cast<double>(k & 1) * 0.5 + 0.75, wr = static_cast<double>((k & 1) == 0 ? 1 : 0) * 0.5 + 0.75;
        accL = static_cast<float>(static_cast<double>(rb) * wl + static_cast<double>(accL));
        accR = static_cast<float>(static_cast<double>(rb) * wr + static_cast<double>(accR));
      }
      L[i] = res.hpL.run(accL) + L[i];
      R[i] = res.hpR.run(accR) + R[i];
    }
  }

  void delay(float* outL, float* outR, int n) {
    const float sp = dly.speed * dly.speed * 20.0f;
    const float d2 = dly.depth * dly.depth;
    const float dp = d2 * d2;
    const float tm = static_cast<float>(static_cast<double>(dly.time * dly.time) + 0.01);
    const double rA = static_cast<double>(sp), rB = 1.1 * rA;
    const double fbv = static_cast<double>(dly.fb * dly.fb) * -0.99;
    for (int k = 0; k < 6; ++k) {
      dly.u[k].setLfo((k & 1) ? rB : rA, sr);
      dly.u[k].setTime(static_cast<double>(tm));
      dly.u[k].fb = static_cast<float>(fbv);
      dly.u[k].setDepth(static_cast<double>(dp));
    }
    const float w2 = dly.wet * dly.wet, bw = dly.blur * dly.wet;
    dly.u[0].lvlT = w2;
    dly.u[1].lvlT = w2 * dly.blur;
    dly.u[2].lvlT = bw * bw;
    dly.u[3].lvlT = w2;
    dly.u[4].lvlT = w2 * dly.blur;
    dly.u[5].lvlT = bw * bw;
    const long syn = static_cast<long>(dly.sync);
    if (syn >= 1 && syn <= 9) {
      static const double kPer[10] = {0, 0.066666666666666666, 0.033333333333333333, 0.022222222222222223, 0.016666666666666666,
                                      0.0083333333333333332, 0.0055555555555555558, 0.0041666666666666666, 0.0020833333333333333,
                                      0.0010416666666666667};
      const double v = kPer[syn] * bpm;
      const float x = static_cast<float>(0.5 / v);
      for (auto& u : dly.u) u.setTime(static_cast<double>(x));
    } else if (syn != 0) {
      const float x = static_cast<float>(0.5 / 1.0);
      for (auto& u : dly.u) u.setTime(static_cast<double>(x));
    }
    for (int i = 0; i < n; ++i) {
      float a = outL[i];
      a = dly.u[0].run(a) + a;
      a = dly.u[1].run(a) + a;
      a = dly.u[2].run(a) + a;
      outL[i] = a;
      float b = outR[i];
      b = dly.u[3].run(b) + b;
      b = dly.u[4].run(b) + b;
      b = dly.u[5].run(b) + b;
      outR[i] = b;
    }
  }

  void process(float* outL, float* outR, int n) {
    lfoCos = static_cast<float>(std::cos(static_cast<double>(lfoPh) * 6.283185306));
    {
      const double inc = static_cast<double>(static_cast<float>(n) * lfoRate) / sr;
      float ph = static_cast<float>(static_cast<double>(lfoPh) + inc);
      if (1.0f < ph) ph = ph + -1.0f;
      lfoPh = ph;
    }
    const double step = 48000.0 / sr;
    double p = pos;
    int left = n;
    float* L = outL;
    float* R = outR;
    float* bL = bufL;  // bL[0] = previous chunk's last sample
    float* bR = bufR;
    while (left != 0) {
      int c = std::min(48, left);
      left -= c;
      if (!(p >= 0.0)) {
        const double nd = static_cast<double>(need);
        int i = 0;
        for (;;) {
          const double x = p + nd;
          const int idx = static_cast<int>(x);
          const float fr = static_cast<float>(x - std::trunc(x));
          L[i] = (bL[idx + 1] - bL[idx]) * fr + bL[idx];
          R[i] = (bR[idx + 1] - bR[idx]) * fr + bR[idx];
          p += step;
          ++i;
          if (p >= 0.0 || i == c) break;
        }
        pos = p;
        L += i, R += i;
        if (i == c) continue;
        c -= i;
      }
      const double x = static_cast<double>(c) * step + p;
      const int cnt = static_cast<int>(x) + (x > std::trunc(x) ? 1 : 0);
      need = cnt;
      std::memset(bL + 1, 0, 255 * sizeof(float));
      std::memset(bR + 1, 0, 255 * sizeof(float));
      mixVoices(bL + 1, bR + 1, cnt);
      p = pos;
      for (int i = 0; i < c; ++i) {
        const int idx = static_cast<int>(p);
        const float fr = static_cast<float>(p - std::trunc(p));
        L[i] = (bL[idx + 1] - bL[idx]) * fr + bL[idx];
        R[i] = (bR[idx + 1] - bR[idx]) * fr + bR[idx];
        p += step;
      }
      p = p - static_cast<double>(need);
      pos = p;
      bL[0] = bL[need];
      bR[0] = bR[need];
      L += c, R += c;
    }
    for (int i = 0; i < n; ++i) outL[i] = dcL.run(outL[i]), outR[i] = dcR.run(outR[i]);
    resonators(outL, outR, n);
    if (cho.on)
      for (int i = 0; i < n; ++i) {
        const float x = outR[i] + outL[i];
        const float y0 = cho.u[0].run(x);
        const float y2 = cho.u[2].run(x);
        const float y1 = cho.u[1].run(x);
        outL[i] = (y0 + y1) + outL[i];
        outR[i] = (y2 + y1) + outR[i];
      }
    if (revOn) rev.process(outL, outR, n);
    if (dly.on) delay(outL, outR, n);
    const float g = std::max(level, 0.0f);
    for (int i = 0; i < n; ++i) outL[i] = outL[i] * g, outR[i] = outR[i] * g;
  }

  void applyEvents() {
    for (int q = 0; q < nQueue; ++q) {
      const Event& ev = queue[q];
      if (ev.kind == Event::On) {
        // A key struck again while down is released first (the host side keeps one velocity per key).
        if (keyDown[ev.key]) noteOff(ev.key);
        keyDown[ev.key] = true;
        keyL[ev.key] = ev.l, keyR[ev.key] = ev.r, keyPitch[ev.key] = ev.pitch;
        noteOn(ev.key, ev.vel, ev.id);
      } else if (ev.kind == Event::Off) {
        keyDown[ev.key] = false;
        noteOff(ev.key);
      } else {
        for (int k = 0; k < 128; ++k)
          if (keyDown[k]) keyDown[k] = false, noteOff(k);
      }
    }
    nQueue = 0;
  }
  void push(const Event& ev) {
    if (nQueue < kQueue) queue[nQueue++] = ev;
  }
};

// ------------------------------------------------------------------ host side
TwinRef::TwinRef() : e_(std::make_unique<Engine>()) {}
TwinRef::~TwinRef() = default;

void TwinRef::configure(const float* p, double sr, double bpm) {
  Engine& e = *e_;
  if (sr <= 0) sr = 44100.0;
  e.bpm = bpm > 1 ? bpm : 120.0;
  const bool fresh = sr != e.sr;
  if (fresh) e.prepare(sr);
  for (int i = 0; i < TsCount; ++i) {
    float v = p[i];
    // The octave wheel is stored as its step 0..6; the engine reads a knob position.
    if (i == TsTranspose) v = (static_cast<float>(std::clamp(static_cast<int>(std::lround(v)), 0, 6)) + 0.5f) / 6.0f;
    if (fresh || !e.tagSet || std::memcmp(&v, &e.tag[i], sizeof v) != 0) e.setParam(i, v);
  }
  e.tagSet = true;
}

void TwinRef::noteOn(uint32_t id, double pitch, float vel, float pan) {
  Engine& e = *e_;
  // The host's note: velocity v/128, voice volume (11^(v/100) − 1) / 10,
  // constant-power pan; the instrument reads back a 7-bit velocity.
  const double v128 = std::nearbyint(static_cast<double>(vel) * 128.0);
  const float vol = static_cast<float>((std::pow(11.0, v128 / 100.0) - 1.0) / 10.0);
  const float fv = std::log(vol * 10.0f + 1.0f) * 41.7142601f;
  const int iv = std::min(fv >= 1.0f ? static_cast<int>(fv) : 0, 127);
  if (iv == 0) return;
  const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);  // the note's key; the rest is fine pitch
  Event ev;
  ev.kind = Event::On;
  ev.key = key;
  ev.id = id;
  ev.vel = static_cast<float>(iv) * 0.00787401572f;
  const long double a = (1.0L + static_cast<long double>(std::clamp(pan, -1.0f, 1.0f))) * static_cast<long double>(0.7853981633974483);
  ev.l = static_cast<float>(std::cos(a) * static_cast<long double>(vol));
  ev.r = static_cast<float>(std::sin(a) * static_cast<long double>(vol));
  ev.pitch = static_cast<float>(pitch - static_cast<double>(key));
  e.push(ev);
  if (e.nHeld < 128) e.held[e.nHeld++] = {id, key};
}

void TwinRef::noteOff(uint32_t id) {
  Engine& e = *e_;
  for (int i = 0; i < e.nHeld; ++i)
    if (e.held[i].id == id) {
      Event ev;
      ev.kind = Event::Off;
      ev.key = e.held[i].key;
      e.push(ev);
      e.held[i] = e.held[--e.nHeld];
      return;
    }
}

void TwinRef::midi(double modWheel, double bend, double pressure) {
  e_->modWheel = static_cast<float>(modWheel);
  e_->bend = static_cast<float>(bend);
  e_->pressure = static_cast<float>(pressure);
}

void TwinRef::allOff() {
  Event ev;
  ev.kind = Event::AllOff;
  e_->push(ev);
  e_->nHeld = 0;
}

void TwinRef::kill() {
  Engine& e = *e_;
  for (Voice& v : e.voices) resetVoice(v);
  std::fill(std::begin(e.keyDown), std::end(e.keyDown), false);
  e.nQueue = 0, e.nHeld = 0;
  for (int k = 0; k < 8; ++k) e.res.a[k].clear(), e.res.b[k].clear();
  e.res.hpL.clear(), e.res.hpR.clear();
  for (auto& u : e.cho.u) u.reset();
  for (auto& u : e.dly.u) u.reset();
  e.rev.reset();
  e.dcL.clear(), e.dcR.clear();
  std::fill(std::begin(e.bufL), std::end(e.bufL), 0.0f);
  std::fill(std::begin(e.bufR), std::end(e.bufR), 0.0f);
  std::fill(std::begin(e.out), std::end(e.out), 0.0f);
  e.readPos = kBlock;
  e.quiet = INT64_MAX / 2;
}

bool TwinRef::active() const {
  const Engine& e = *e_;
  if (e.nQueue > 0) return true;
  for (int i = 0; i < kLineVoices; ++i)
    if (e.voices[i].active) return true;
  return e.quiet < static_cast<int64_t>(e.sr * 4.0);
}

void TwinRef::render(float* L, float* R, int n) {
  Engine& e = *e_;
  for (int i = 0; i < n;) {
    if (e.readPos >= kBlock) {
      e.applyEvents();
      float* oL = e.out;
      float* oR = e.out + kBlock;
      std::fill(e.out, e.out + 2 * kBlock, 0.0f);
      e.process(oL, oR, kBlock);
      float peak = 0;
      for (int j = 0; j < 2 * kBlock; ++j) peak = std::max(peak, std::fabs(e.out[j]));
      e.quiet = peak > 1e-7f ? 0 : e.quiet + kBlock;
      e.readPos = 0;
    }
    const int k = std::min(n - i, kBlock - e.readPos);
    for (int j = 0; j < k; ++j) {
      L[i + j] += e.out[e.readPos + j];
      R[i + j] += e.out[kBlock + e.readPos + j];
    }
    e.readPos += k;
    i += k;
  }
}

}  // namespace gl::packs::models::strs

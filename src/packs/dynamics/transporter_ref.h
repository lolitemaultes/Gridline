// Gridline — J-45 reference engine for the transient loop slicer that FL's
// "Transporter" effect imports map to. Clean-room rebuild from the J-45 spec:
// an 8-band LR4 onset detector (fast/slow envelope pair per band, log-flux
// vote), two loopers (A, B) that replay the slice between the last two
// accepted triggers from 20 s capture rings, with bounce / adaptive / tempo
// lengths, reverse and octave (read-rate 2 or 0.5, no interpolation) chosen
// on each trigger either from an LCG (probabilistic) or from the write
// position (deterministic), linear pan / sine auto-pan, look-ahead declick,
// and the host-side flush fader. Everything runs in float32 in the reference
// evaluation order (build with -ffp-contract=off). All memory is allocated in
// the constructor; run() never allocates.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#if (defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)) && !defined(GL_TPREF_NORCP)
#include <xmmintrin.h>
#define GL_TPREF_RCP 1
#endif

namespace gl::packs::dynamics::tpref {

inline float exp10f_(float x) {
#if defined(__APPLE__)
  return __exp10f(x);
#elif defined(__GLIBC__)
  return ::exp10f(x);
#else
  return std::pow(10.0f, x);
#endif
}

// Reference computes two constants with an approximate reciprocal plus one
// Newton step: k / x ≈ (k - x·(r·k))·r + r·k, r = rcp(x).
inline float nrDiv(float k, float x) {
#ifdef GL_TPREF_RCP
  const float r = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
#else
  const float r = 1.0f / x;
#endif
  const float rk = r * k;
  return (k - x * rk) * r + rk;
}

// Note-length table used by tempo length and tempo trigger (combo index → whole notes).
inline constexpr float kNote[21] = {1.0f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 1.5f, 0.75f, 0.375f, 0.1875f, 0.09375f,
                                    1.0f / 3.0f, 1.0f / 6.0f, 1.0f / 12.0f, 1.0f / 24.0f, 1.0f / 48.0f,
                                    0.2f, 0.1f, 0.05f, 0.025f, 0.0125f};

// Stored control ints (state params 0..21) → engine units.
//  0..2 levels dB, 3..5 biases 0..1, 6 spread -1..1, 7 min length s, 8 A/B ratio,
//  9 sensitivity, 10 bounce -1..1, 11 freeze, 12 flush, 13 loop mode, 14 bypass,
//  15..17 update A / B / A+B, 18 deterministic, 19 tempo length idx, 20 tempo trigger idx (21 = off), 21 ignore.
inline void engineValues(const int32_t* raw, float* v) {
  static const int lo[22] = {0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  static const int hi[22] = {12800, 12800, 12800, 10000, 10000, 10000, 1024, 2000, 36, 1024, 1024, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10000};
  for (int i = 0; i < 22; ++i) {
    if (hi[i] == 0) {
      v[i] = static_cast<float>(raw[i]);
      continue;
    }
    double d = static_cast<double>(static_cast<int64_t>(raw[i]) - lo[i]) * (1.0 / static_cast<double>(hi[i] - lo[i]));
    d = d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
    const float n = static_cast<float>(d);
    if (i <= 2) {
      float lin;
      if (n == 1.0f) {
        lin = n;
      } else {
        const double e = std::exp(static_cast<double>(n * 2.3978953f)) - 1.0;
        lin = static_cast<float>(static_cast<double>(0.1f) * e);
      }
      v[i] = lin > 0.0f ? static_cast<float>(std::log10(static_cast<double>(lin)) * 20.0) : -INFINITY;
    } else if (i == 6 || i == 10) {
      v[i] = (n - 0.5f) * 2.0f;
    } else if (i == 7) {
      v[i] = static_cast<float>(static_cast<double>(n) * 1.98 + 0.02);
    } else if (i == 8) {
      v[i] = n * 9.0f + 1.0f;
    } else {
      v[i] = n;
    }
  }
}

namespace detail {
struct Svf {  // one TPT state-variable section, damping √2
  float g = 0, gk = 0, a = 0, a2 = 0, g2 = 0, s1 = 0, s2 = 0;
  void set(float w) {
    g = std::tan(w);
    gk = g + 1.4142135f;
    const float D = gk * g + 1.0f;
    a = g / D, a2 = a + a, g2 = g + g;
  }
  // e is the already-formed high-pass numerator; returns low-pass, writes band-pass
  float step(float e, float& bp) {
    const float b = a * e + s1;
    s1 = a2 * e + s1;
    const float l = s2 + b * g;
    s2 = g2 * b + s2;
    bp = b;
    return l;
  }
  float feed(float u) const { return u - (gk * s1 + s2); }
};
struct Cross {  // LR4 low-pass built from two sections; bp1 kept for the complement
  Svf x, y;
  float bp1 = 0;
  float lowFromE(float e1) {
    float b2;
    const float l1 = x.step(e1, bp1);
    return y.step(l1 - (y.s2 + y.s1 * y.gk), b2);
  }
  float low(float u) { return lowFromE(x.feed(u)); }
};
struct Band {  // fast (A) and slow (B) 4-pole power envelopes
  float a[4] = {}, b[4] = {}, rB = 0, lg = 0;
};
}  // namespace detail

struct Engine {
  static constexpr int kChunk = 1024;
  float srRaw = 44100, fs = 44100;
  // constants
  float G10 = 0, G10x2 = 0, G20 = 0, G20x2 = 0, laOn = 0, fs20 = 0, onB = 0, onBm = 0, refr = 0;
  float cRel = 0, k14 = 0, cAtk = 0, twoFs = 0, k20 = 0, outC = 0;
  int ringLen = 0;
  // parameters (engine zones)
  bool lookahead = true;
  float zBypass = 0, zFlush = 0, zMode = 0, zMinLen = 0.5f, zSens = 0.5f, zSource = 0, zTrigA = 0, zTrigAB = 0, zTrigB = 0;
  float zFreeze = 0, zMult = 21.0f, zDet = 0, zIgnore = 0, zBounce = 0, zRev = 0, zOct = 0, zDU = 0, zSpread = 0, zStereo = 0, zRatio = 3;
  float zDry = 0, zLA = 0, zLB = 0;
  float pPrev[3] = {}, flushPrev = 0;
  bool latch = false, bypassFlag = false;
  float fade[2] = {};
  // state
  detail::Cross x640, x113, x47, x269, x3620, x1522, x8611;
  detail::Band band[8];
  int first = 0, firstPrev = 0, iota = 0, wp = 0, cntOn = 0;
  float wetS = 0, dryS = 0, thrS = 0, rate[4] = {}, trA = 0, trAB = 0, trB = 0, tfr = 0, panPh = 0, panS = 0;
  int32_t lcg = 0;
  struct Looper {
    int cnt = 0, cntT = 0, bCnt = 0, done = 0, rev = 0;
    float frz = 0, start = 0, trig = 0, bLen = 0, len = 0, ph = 0, rate = 0, dcl = 0, env[4] = {}, gS = 0;
  } A, B;
  float dlyL[1024] = {}, dlyR[1024] = {}, dA[1024] = {}, dB[1024] = {};
  std::vector<float> buf1, buf2;
  float tab[65536];
  float io[6][kChunk];

  explicit Engine(float sampleRate) : srRaw(sampleRate) {
    int si = static_cast<int>(sampleRate);
    fs = static_cast<float>(std::max(1, si));
    if (192000.0f <= fs) fs = 192000.0f;
    const float inv = 1.0f / fs;
    float t = std::tan(inv * 31.415926f);
    G10 = t / (t + 1.0f), G10x2 = G10 + G10;
    laOn = fs * 0.005f;
    t = std::tan(inv * 62.831852f);
    G20 = t / (t + 1.0f), G20x2 = G20 + G20;
    fs20 = fs * 20.0f;
    onB = 1.0f / (inv * 1.8055984f + 1.0f), onBm = 1.0f - onB;
    refr = fs * 0.02f + -1.0f;
    x640.x.set(inv * 2010.6193f), x113.x.set(inv * 355.43063f), x47.x.set(inv * 149.44017f);
    x269.x.set(inv * 845.36127f), x3620.x.set(inv * 11373.78f), x1522.x.set(inv * 4782.0854f), x8611.x.set(inv * 27051.56f);
    cRel = nrDiv(1.0f, inv * 288.89572f + 1.0f);
    k14 = nrDiv(14.444787f, fs);
    cAtk = 1.0f / (inv * 14444.787f + 1.0f);
    twoFs = fs + fs;
    k20 = inv * 20.0f;
    const float m = std::max(sampleRate, 1.0f);
    outC = std::exp(static_cast<float>(static_cast<double>(1.0f / m) * -94.24777960769379));
    ringLen = static_cast<int>(fs20) + 2;
    buf1.assign(static_cast<size_t>(ringLen), 0.f), buf2.assign(static_cast<size_t>(ringLen), 0.f);
    for (int i = 0; i < 65536; ++i) tab[i] = std::sin(9.58738e-05f * static_cast<float>(i));
    clear();
  }
  int latency() const { return lookahead ? static_cast<int>(static_cast<int64_t>(srRaw * 0.005f)) : 0; }

  // Engine cleared: all DSP state, LCG, write position, rings zeroed, output fader at 0
  // (identical to a freshly constructed engine).
  void clear() {
    resetState();
    std::fill(buf1.begin(), buf1.end(), 0.f), std::fill(buf2.begin(), buf2.end(), 0.f);
    fade[0] = fade[1] = 0, latch = false;
  }
  // The host flush gesture (flush button, stop with flush-on-stop, host flush call): state is
  // cleared now (rings kept), the main output fades out; at the first block start where the
  // fader is below 1e-7 the rings are wiped, that block runs with the write position held at 0,
  // and the output fades back in.
  void flush() {
    resetState();
    latch = true;
  }

  // v: 22 engine values (engineValues). bpm: host tempo. source 0 L / 1 R / 2 L+R; stereo 0 mono / 1 stereo.
  // tempoIn: per-block 4·frac(beats/4). scL/scR: detection source (null = input).
  // sendAL / sendBR (may be null): loop A / loop B send signals, OVERWRITTEN (FL writes, not adds).
  void run(const float* v, float bpm, int source, int stereo, float tempoIn, int n, float* L, float* R, const float* scL,
           const float* scR, float* sendAL, float* sendBR) {
    setParams(v, bpm, source, stereo);
    for (int off = 0; off < n; off += kChunk) {
      const int c = std::min(kChunk, n - off);
      const float* sl = scL ? scL + off : L + off;
      const float* sr = scR ? scR + off : R + off;
      std::memcpy(io[0], sl, sizeof(float) * static_cast<size_t>(c));
      std::memcpy(io[1], sr, sizeof(float) * static_cast<size_t>(c));
      block(tempoIn, c, L + off, R + off, io[0], io[1], sendAL ? sendAL + off : io[2], sendBR ? sendBR + off : io[3]);
    }
  }

 private:
  void resetState() {
    detail::Cross* xs[7] = {&x640, &x113, &x47, &x269, &x3620, &x1522, &x8611};
    for (auto* c : xs) {
      c->x.s1 = c->x.s2 = 0, c->bp1 = 0;
      c->y = c->x;
    }
    for (auto& b : band) b = detail::Band{};
    first = firstPrev = iota = wp = cntOn = 0;
    wetS = dryS = thrS = trA = trAB = trB = tfr = panPh = panS = 0;
    std::fill(rate, rate + 4, 0.f);
    lcg = 0;
    A = Looper{}, B = Looper{};
    std::memset(dlyL, 0, sizeof dlyL), std::memset(dlyR, 0, sizeof dlyR), std::memset(dA, 0, sizeof dA), std::memset(dB, 0, sizeof dB);
  }
  static int roundAway(float x) { return static_cast<int>(std::trunc(x + std::copysign(0.5f, x))); }
  void setParams(const float* v, float bpm, int source, int stereo) {
    zDry = v[0], zLA = v[1], zLB = v[2], zRev = v[3], zOct = v[4], zDU = v[5], zSpread = v[6];
    zRatio = v[8], zSens = v[9], zFreeze = v[11], zIgnore = v[21];
    zBypass = v[14], bypassFlag = 0.5f < v[14];
    zDet = 0.5f < v[18] ? 1.0f : 0.0f;
    const int mode = roundAway(v[13]);
    if (mode == 3) {
      zMode = 1.0f, zBounce = 0.0f;
      const int li = std::clamp(roundAway(v[19]), 0, 20);
      zMinLen = kNote[li] * 240.0f / bpm;
    } else {
      zMode = static_cast<float>(mode), zBounce = v[10], zMinLen = v[7];
    }
    const int ti = roundAway(v[20]);
    zMult = (ti == 21 || ti < 0 || ti > 21) ? 0.0f : 0.25f / kNote[ti];
    float* z[3] = {&zTrigA, &zTrigB, &zTrigAB};
    for (int k = 0; k < 3; ++k) {
      *z[k] = pPrev[k] < v[15 + k] ? 1.0f : 0.0f;
      pPrev[k] = v[15 + k];
    }
    if (!latch && 0.5f < v[12] && !(0.5f < flushPrev)) flush();
    flushPrev = v[12];
    zSource = static_cast<float>(source), zStereo = 0.5f < static_cast<float>(stereo) ? 1.0f : 0.0f;
  }

  static float u01(int32_t r) { return static_cast<float>(r) * 2.3283064e-10f + 0.5f; }
  static int32_t lcgMix(int32_t s, uint32_t m, uint32_t c) { return static_cast<int32_t>(static_cast<uint32_t>(s) * m + c); }

  float bandStep(detail::Band& b, float x, float thr) {
    const float eps = 1.1920929e-07f;
    const float p = x * x;
    float in = p;
    for (int j = 0; j < 4; ++j) {
      const float c = b.a[j] < in ? cAtk : cRel;
      in = (b.a[j] - in) * c + in;
      b.a[j] = in;
    }
    float d = b.rB;
    if (d <= eps) d = eps;
    float q = std::sqrt(in) / d;
    if (q <= 0.001f) q = 0.001f;
    q = std::log(q);
    const float cAd = 1.0f / (k14 / (thr < q ? eps : 0.05f) + 1.0f);
    in = p;
    for (int j = 0; j < 4; ++j) {
      const float c = in <= b.b[j] ? cRel : cAd;
      in = (b.b[j] - in) * c + in;
      b.b[j] = in;
    }
    float r = std::sqrt(in);
    b.rB = r;
    if (r <= 0.001f) r = 0.001f;
    const float lg = std::log(r);
    const int fire = 0.5f < lg - b.lg;
    b.lg = lg;
    return static_cast<float>(fire);
  }

  // one looper's trigger / length / phase / read-position update; returns read position
  float loop(Looper& S, bool isA, float lenBase, float bounceReset, int onset, int edge, int manual, int keep, float wpf, float L,
             float bf, int det, float d, int32_t s0, int32_t s1, float revD2, float octD2, float duD2, float frz) {
    // length gate for onsets and tempo edges
    const float len1 = lenBase - 1.0f;
    int on = (len1 <= static_cast<float>(S.cnt)) & onset;
    S.cnt = on ? 0 : S.cnt + 1;
    S.frz = on ? frz : S.frz;
    const int te = (len1 <= static_cast<float>(S.cntT)) & edge;
    S.cntT = te ? 0 : S.cntT + 1;
    if (static_cast<int>(S.frz) != 0) on = 0;
    const int acc = keep & (on | te | manual);
    const float start = acc ? S.trig : S.start;
    S.start = start;
    float trig = S.trig;
    float bl;
    int bc;
    if (!acc) {
      if (S.done == 0) {
        bc = S.bCnt + 1, bl = S.bLen;
      } else {
        bl = S.bLen * bf, bc = 0;
      }
    } else {
      trig = wpf;
      bl = bounceReset, bc = 0;
      if (S.done == 0) bc = S.bCnt + 1;
    }
    S.trig = trig;
    if (L <= bl) bl = L;
    S.done = bl <= static_cast<float>(bc);
    S.bCnt = bc, S.bLen = bl;
    float len = bl;
    if (zMode != 1.0f) len = std::fmod(std::fmod(trig - start, L) + L, L);
    if (len <= 48.0f) len = 48.0f;
    const float phPrev = S.ph;
    const float ph = std::fmod((1.0f - static_cast<float>(acc)) * phPrev + 1.0f, len);
    const float lenPrev = S.len;
    S.len = len, S.ph = ph;
    // reverse / octave choice on accept
    if (acc) {
      const bool octD = d < octD2, duD = d < duD2;
      if (det) {
        S.rev = d < revD2;
        S.rate = octD ? (duD ? 2.0f : 0.5f) : 1.0f;
      } else if (isA) {
        S.rev = u01(lcgMix(s0, 0xCFDDDF21u, 0xAF5A7D38u)) < zRev;
        if (zOct <= u01(lcgMix(s0, 0xD3DC57F9u, 0x3EAD32C2u)))
          S.rate = 1.0f;
        else
          S.rate = u01(lcgMix(s0, 0xEE067F11u, 0x0DAA66BCu)) < zDU ? 2.0f : 0.5f;
      } else {
        S.rev = u01(lcgMix(s0, 0x9B355305u, 0xCD1D9EDFu)) < zRev;
        if (zOct <= u01(lcgMix(s0, 0xEBA1483Du, 0xC21EEC51u)))
          S.rate = 1.0f;
        else
          S.rate = u01(lcgMix(s0, 0x807DBCB5u, 0xD664EBF3u)) < zDU ? 2.0f : 0.5f;
      }
    }
    (void)s1;
    const float rp = S.rate * ph;
    const float pos = S.rev ? std::fmod(std::fmod(trig - rp, L) + L, L) : std::fmod(rp + start, L);
    const bool disc = std::fabs(wpf - pos) < 2.0f || ph - phPrev < 0.0f || len != lenPrev;
    S.dcl = (1.0f - static_cast<float>(disc)) * S.dcl + 1.0f;
    return pos;
  }
  void envStep(Looper& S, float la, float q, float qm) {
    float e = S.dcl <= la ? 0.0f : 1.0f;
    for (int j = 0; j < 4; ++j) {
      e = e * qm + S.env[j] * q;
      S.env[j] = e;
    }
  }

  void block(float tempoIn, int n, float* outL, float* outR, const float* scL, const float* scR, float* sA, float* sB) {
    // host-side flush latch (evaluated per block)
    zFlush = 0.0f;
    if (latch && fade[0] < 1.0000000116860974e-07f) {
      std::fill(buf1.begin(), buf1.end(), 0.f), std::fill(buf2.begin(), buf2.end(), 0.f);
      zFlush = 1.0f;
      latch = false;
    }
    const float tgt = static_cast<float>((latch ? 0 : 1) | (bypassFlag ? 1 : 0));

    const float eps = 1.1920929e-07f;
    const float la = lookahead ? laOn : eps;
    const int laI = static_cast<int>(la);
    const float gDry = exp10f_(zDry * 0.05f);
    const int wrap = zFlush <= 0.5f ? static_cast<int>(fs20) : 1;
    const float mode = zMode, minLen = zMinLen;
    const float thrK = (1.0f - zSens) * (1.0f - zSens) * 6.5f;
    const float bf = std::exp(zBounce * -0.5f);
    const float gA = exp10f_(zLA * 0.05f), gB = exp10f_(zLB * 0.05f);
    const float spread = zSpread, ratio = zRatio, L = fs20;
    const float octD2 = zOct * twoFs;
    const float q = 1.0f / (14.444787f / la + 1.0f), qm = 1.0f - q;
    const float lenB0 = minLen / ratio, bResetB = lenB0 * fs, bResetA = fs * minLen;
    const int det = static_cast<int>(zDet);
    const bool srcMono = zSource < 2.0f;
    const float keepD2 = (1.0f - zIgnore) * twoFs, revD2 = zRev * twoFs, duD2 = twoFs * zDU;
    const float panK = spread >= 0.0f ? spread * 0.5f : 0.5f;
    for (int i = 0; i < n; ++i) {
      first = 1;
      // bypass crossfade (10 Hz), squared later
      const float eW = 1.0f - (zBypass + wetS);
      const float wetNext = G10x2 * eW + wetS;
      // dry path (look-ahead delay)
      const float inL = outL[i], inR = outR[i];
      dlyL[iota & 1023] = inL;
      const int rd = (iota - laI) & 1023;
      const float dryL = dlyL[rd];
      const float dPrev = dryS;
      dryS = G20x2 * (gDry - dPrev) + dPrev;
      // write position
      wp = (wp + firstPrev) % wrap;
      const float wpf = static_cast<float>(wp);
      // detector source
      const float sL = scL[i], sR = scR[i];
      const float sc = srcMono ? (1.0f <= zSource ? sR : sL) : (sR + sL) * 0.70710677f;
      // crossover tree
      const float l640 = x640.low(sc);
      const float l113 = x113.low(l640);
      const float l47 = x47.low(l113);
      float bnd[8];
      bnd[0] = l47;
      bnd[1] = (x47.bp1 * -2.828427f - l47) + l113;
      const float h113 = l640 - x113.bp1 * 2.828427f;
      const float l269 = x269.lowFromE(h113 - ((x269.x.gk * x269.x.s1 + l113) + x269.x.s2));
      bnd[2] = l269;
      bnd[3] = (h113 + x269.bp1 * -2.828427f) - (l113 + l269);
      const float scH = sc - x640.bp1 * 2.828427f;
      const float l3620 = x3620.lowFromE(scH - ((x3620.x.gk * x3620.x.s1 + l640) + x3620.x.s2));
      const float l1522 = x1522.low(l3620);
      bnd[4] = l1522;
      bnd[5] = (x1522.bp1 * -2.828427f - l1522) + l3620;
      const float h3620 = (scH - l640) + x3620.bp1 * -2.828427f;
      const float l8611 = x8611.lowFromE(h3620 - ((x8611.x.gk * x8611.x.s1 + l3620) + x8611.x.s2));
      bnd[6] = l8611;
      bnd[7] = (h3620 + x8611.bp1 * -2.828427f) - (l3620 + l8611);
      // threshold (20 Hz smoothed), band votes
      const float eT = (1.5f - thrS) + thrK;
      const float thr = eT * G20 + thrS;
      thrS = G20x2 * eT + thrS;
      float fires[8];
      for (int k = 0; k < 8; ++k) fires[k] = bandStep(band[k], bnd[k], thr);
      int votes = 0;
      for (int k = 0; k < 8; ++k) votes += static_cast<int>(fires[k]);
      const int onset = votes & static_cast<int>(refr <= static_cast<float>(cntOn));
      cntOn = onset ? 0 : cntOn + 1;
      // manual triggers (rising edges)
      auto edgeOf = [](float now, float prev) {
        const float df = now - prev;
        return static_cast<int>((0.0f < df ? 1.0f : 0.0f) * df);
      };
      const int eA = edgeOf(zTrigA, trA), eAB = edgeOf(zTrigAB, trAB), eB = edgeOf(zTrigB, trB);
      trA = zTrigA, trAB = zTrigAB, trB = zTrigB;
      const int manA = eA | eAB, manB = eAB | eB;
      // onset rate → period → adaptive proximity
      float r = static_cast<float>(manA | eB | onset) * onBm + rate[0] * onB;
      rate[0] = r;
      for (int j = 1; j < 4; ++j) {
        r = r * onBm + rate[j] * onB;
        rate[j] = r;
      }
      float rf = r * fs;
      if (rf <= eps) rf = eps;
      float prox = std::fabs(minLen - 1.0f / rf);
      if (2.0f <= prox) prox = 2.0f;
      const float lenA = (mode == 2.0f ? prox : minLen) * fs;
      // tempo edge (bouncing engine mode only)
      const float tv = tempoIn * zMult;
      const float fr = tv - std::floor(tv);
      const int edge = mode == 1.0f && fr - tfr < 0.0f;
      tfr = fr;
      // random / deterministic sources
      const int32_t s0 = lcg;
      const int32_t s1 = lcgMix(s0, 0xCFDDDF21u, 0xAF5A7D38u);
      const float d = std::fmod(wpf, twoFs);
      const int keepA = det ? d < keepD2 : zIgnore < u01(lcgMix(s0, 0xC2A29A69u, 0xA703F7A6u));
      const int keepB = det ? d < keepD2 : zIgnore < u01(lcgMix(s0, 0x41C64E6Du, 0xD3DBE645u));
      // loop A
      const float posA = loop(A, true, lenA, bResetA, onset, edge, manA, keepA, wpf, L, bf, det, d, s0, s1, revD2, octD2, duD2, zFreeze);
      envStep(A, la, q, qm);
      const float aPrev = A.gS;
      A.gS = G20x2 * (gA - aPrev) + aPrev;
      // pan
      float ph = 0.0f;
      if (firstPrev == 1) ph = panPh - k20 * spread;
      ph = ph - std::floor(ph);
      panPh = ph;
      const float pan = spread < 0.0f ? tab[static_cast<int>(ph * 65536.0f)] : 1.0f;
      const float pPrev2 = panS;
      panS = G20x2 * (pan - pPrev2) + pPrev2;
      const float f = ((pan - pPrev2) * G20 + pPrev2) * panK;
      const float fA = f + 0.5f, fB = 0.5f - f;
      // record + read A
      dlyR[iota & 1023] = inR;
      const float mono = (inR + inL) * 0.70710677f;
      buf1[static_cast<size_t>(wp)] = zStereo != 0.0f ? inL : mono;
      dA[iota & 1023] = buf1[static_cast<size_t>(static_cast<int>(posA))];
      const float aOut = dA[rd];
      // loop B
      const float proxB = prox * (1.0f / ratio);
      const float lenB = (mode == 2.0f ? proxB : lenB0) * fs;
      const float posB = loop(B, false, lenB, bResetB, onset, edge, manB, keepB, wpf, L, bf, det, d, s0, s1, revD2, octD2, duD2, zFreeze);
      envStep(B, la, q, qm);
      const float bPrev = B.gS;
      B.gS = G20x2 * (gB - bPrev) + bPrev;
      buf2[static_cast<size_t>(wp)] = zStereo != 0.0f ? inR : mono;
      dB[iota & 1023] = buf2[static_cast<size_t>(static_cast<int>(posB))];
      const float bOut = dB[rd];
      // mix
      const float gD = G20 * (gDry - dPrev) + dPrev;
      float wet = eW * G10 + wetS;
      wet = wet * wet;
      const float aSig = A.env[3] * (G20 * (gA - aPrev) + aPrev) * aOut;
      const float bSig = B.env[3] * ((gB - bPrev) * G20 + bPrev) * bOut;
      const float dryR = dlyR[rd];
      float oL = (bSig * fB + (aSig * fA + dryL * gD)) * wet + (1.0f - wet) * dryL;
      float oR = (bSig * fA + (aSig * fB + gD * dryR)) * wet + (1.0f - wet) * dryR;
      sA[i] = aOut * wet * A.env[3];
      sB[i] = bOut * wet * B.env[3];
      lcg = s1;
      wetS = wetNext;
      firstPrev = first;
      ++iota;
      // host-side output fader
      fade[0] = (fade[0] - tgt) * outC + tgt;
      oL = fade[0] * fade[0] * oL;
      fade[1] = (fade[1] - tgt) * outC + tgt;
      oR = fade[1] * fade[1] * oR;
      outL[i] = oL, outR[i] = oR;
    }
  }
};

}  // namespace gl::packs::dynamics::tpref

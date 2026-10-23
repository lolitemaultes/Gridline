// Gridline — "dynamics" pack, J-35: reference engine of gl.emphasis
// (EmModel = 1; behaviour study).
//
// A four-stage look-ahead limiter: optional DC block → (modes 0 / 1) a
// low / high band analysis that sets a detector knee → input gain, optional
// mid / side coding, linked detector with the knee → stage A (adaptive
// two-speed release on a 50 ms hold, 1–4 smoothing stages) → stage B
// (peak limiter at 1–16× through polyphase IIR half-band resamplers, window
// from the hardness) → mid / side decode and stage C (M/S only) → stage D (4 ms,
// optional 4× true-peak detector) → ceiling, level matching, delta, bypass
// and the start / reconfiguration gate. Audio, envelopes and states are
// float, as the reference.
//
// The per-block analysis of modes 0 / 1 is latched on a fixed grid of
// kAnalysisBlock samples (the reference latches once per host block), so the
// result does not depend on Gridline's block size. Buffers are sized in the
// constructor; process() never allocates.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gl::packs::dynamics::emphref {

inline constexpr double kTwoPi = 6.283185307179586;
inline constexpr float kEps = 1.19e-7f;
inline constexpr int kAnalysisBlock = 512;

// ---------------------------------------------------------------- laws
// Ceiling knob n (0..1) → linear gain as the plugin sends it: (11^n − 1) / 10.
inline double ceilingLin(double n) { return n <= 0 ? 0.0 : (std::pow(11.0, std::min(1.0, n)) - 1.0) / 10.0; }
inline double ceilingDb(double n) { return n <= 0 ? -1000.0 : 20.0 * std::log10(ceilingLin(n)); }
// Inverse (dB → knob), used by editors and encoders.
inline double ceilingKnob(double db) { return db <= -999 ? 0.0 : std::log(10.0 * std::pow(10.0, db / 20.0) + 1.0) / std::log(11.0); }
// Look-ahead window of hardness h (seconds).
inline float window(int h) { return 0.032f * std::exp2(static_cast<float>(-std::clamp(h, 0, 9))); }
inline float lin(float db) { return std::exp(db * 0.115129255f); }
inline float dB(float x) { return 8.685889f * std::log(std::max(x, kEps)); }

struct Settings {
  bool bypass = false, delta = false, truePeak = false, ms = false, levelMatch = false;
  bool lowLatency = false, instant = true, dcBlock = true;
  int order = 1;      // 1..4 envelope stages
  int mode = 0;       // 0 versatile, 1 loud, 2 transient, 3 steady
  int os = 2;         // 0..4 → 1×..16×
  int hardness = 7;   // 0..9
  double inDb = 0;    // 0..24
  double link = 1;    // 0..1
  double amount = 1;  // 0..1
  double ceilN = 1;   // 0..1 knob (law above)
};

// Total latency (samples) the reference reports for these settings.
inline int latency(const Settings& s, double fs) {
  const float f = static_cast<float>(fs), w = window(s.hardness), wC = std::min(0.008f, w);
  int L = static_cast<int>(std::floor(f * w));
  if (!s.lowLatency || s.instant) L += static_cast<int>(std::floor(f * 0.05f)) - static_cast<int>(std::floor(f * w));
  if (s.ms) L += static_cast<int>(f * wC);
  L += static_cast<int>(f * 0.004f);
  return L;
}

// ---------------------------------------------------------------- building blocks
struct OnePole {
  float a = 0, y = 0;
  void set(double fs, double fc) { a = static_cast<float>(std::exp(-kTwoPi * fc / fs)); }
  float run(float x) { return y = x + (y - x) * a; }
};

struct HighPass1 {
  float a = 0, s = 0;
  void set(double fs) { a = static_cast<float>(std::exp(-kTwoPi * 5.0 / fs)); }
  float run(float x) {
    s = x + (s - x) * a;
    return x - s;
  }
};

inline float coefD(double fs, double t) { return t <= 0 ? 0.f : static_cast<float>(std::exp(-kTwoPi / (fs * t))); }

// 1..8 rise / fall smoothing stages (n = 1: double-precision coefficients).
struct AsymSmoother {
  int n = 1;
  double fs = 44100, tAtt = 0, tRel = 0;
  float w = 0, ca = 0, cr = 0;
  float s[8] = {};
  bool fixed8 = false;
  void setN(int nn) {
    nn = std::clamp(nn, 1, 8);
    if (nn != n)
      for (int k = 1; k < 8; ++k) s[k] = s[0];
    n = nn;
    recalcW();
    setAtt(tAtt), setRel(tRel);
  }
  void prepare(double rate) {
    fs = rate;
    recalcW();
    setAtt(tAtt), setRel(tRel);
  }
  void recalcW() {
    if (fixed8) w = static_cast<float>(20.885122 / fs);
    else w = static_cast<float>(kTwoPi / std::sqrt(std::exp2(1.0 / n) - 1.0) / fs);
  }
  float coef(double t) const {
    if (t <= 0) return 0.f;
    if (n == 1 && !fixed8) return coefD(fs, t);
    return std::exp(-w / static_cast<float>(t));
  }
  void setAtt(double t) { tAtt = t, ca = coef(t); }
  void setRel(double t) { tRel = t, cr = coef(t); }
  float run(float in) {
    for (int k = 0; k < n; ++k) {
      s[k] = in + (s[k] - in) * (s[k] < in ? ca : cr);
      in = s[k];
    }
    return in;
  }
};

struct HoldCell {
  int c = 0;
  float v = 0;
  float run(float u, float H) {
    const int old = c;
    c += 1;
    if (v <= u || H <= static_cast<float>(old)) c = 0, v = u;
    return v;
  }
};
struct PeakHold8 {
  HoldCell cell[8];
  float H = 0;
  void set(double fs, double T) { H = static_cast<float>(T * fs / 8.0); }
  float run(float u) {
    for (auto& c : cell) u = c.run(u, H);
    return u;
  }
  void clear() {
    for (auto& c : cell) c = HoldCell{};
  }
};

struct DelayInt {
  std::vector<float> buf;
  uint32_t mask = 0, w = 0, D = 0;
  explicit DelayInt(uint32_t size = 1u << 16) : buf(size, 0.f), mask(size - 1) {}
  void set(double fs, double T) { D = std::min(mask, static_cast<uint32_t>(static_cast<float>(T * fs))); }
  void setSamples(int d) { D = std::min(mask, static_cast<uint32_t>(std::max(0, d))); }
  float run(float x) {
    buf[w & mask] = x;
    const float y = buf[(w - D) & mask];
    ++w;
    return y;
  }
  void clear() {
    std::fill(buf.begin(), buf.end(), 0.f);
    w = 0;
  }
};

// Fractional delay that glides (5 Hz) toward its target; starts at 0.
struct DelayGlide {
  std::vector<float> buf;
  uint32_t mask = 0, w = 0;
  float d = 0, D = 0, k = 0;
  explicit DelayGlide(uint32_t size = 1u << 17) : buf(size, 0.f), mask(size - 1) {}
  void prepare(double fs) { k = static_cast<float>(std::exp(-10.0 * 3.141592653589793 / fs)); }
  void target(float t) { D = std::min(t, static_cast<float>(mask - 2)); }
  float run(float x) {
    const float t = (d - D) * k;
    d = t + D;
    const float dd = std::fabs(t) < 0.001f * D ? std::round(D) : d;
    const int n = static_cast<int>(dd);
    const float f = dd - static_cast<float>(n);
    buf[w & mask] = x;
    const float a = buf[(w - static_cast<uint32_t>(n)) & mask], b = buf[(w - static_cast<uint32_t>(n) + 1) & mask];
    ++w;
    return a + (b - a) * f;
  }
  void clear() {
    std::fill(buf.begin(), buf.end(), 0.f);
    w = 0;
  }
};

// Polyphase IIR half-band (two all-pass chains, the second one sample late).
struct Halfband {
  struct Sec {
    float a = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float run(float x) {
      const float y = x2 + a * (x - y2);
      x2 = x1, x1 = x, y2 = y1, y1 = y;
      return y;
    }
  };
  Sec A[6], B[6];
  int nA = 0;
  float bPrev = 0;
  void design(int order) {
    static const float o12a[6] = {.016774667f, .139021486f, .332501113f, .537661073f, .721418381f, .882185817f};
    static const float o12b[6] = {.065013193f, .230941296f, .436494231f, .632960975f, .803780854f, .959968746f};
    static const float o6a[3] = {.060297392f, .412590712f, .772715628f}, o6b[3] = {.215971440f, .604358613f, .923886120f};
    static const float o4a[2] = {.079866424f, .545353651f}, o4b[2] = {.283829331f, .834411919f};
    const float* a = order == 12 ? o12a : order == 6 ? o6a : o4a;
    const float* b = order == 12 ? o12b : order == 6 ? o6b : o4b;
    nA = order / 2;
    for (int k = 0; k < nA; ++k) A[k].a = a[k], B[k].a = b[k];
  }
  float run(float x) {
    float ya = x, yb = x;
    for (int k = 0; k < nA; ++k) ya = A[k].run(ya), yb = B[k].run(yb);
    const float out = 0.5f * (ya + bPrev);
    bPrev = yb;
    return out;
  }
  void clearStates() {  // reset keeps bPrev, as the reference
    for (int k = 0; k < 6; ++k) A[k].x1 = A[k].x2 = A[k].y1 = A[k].y2 = B[k].x1 = B[k].x2 = B[k].y1 = B[k].y2 = 0;
  }
};

// Cascaded 2× stages (orders from the base rate up: 12, 6, 4, 4).
struct Resampler {
  Halfband st[4];
  int stages = 0;
  Resampler() {
    static const int ord[4] = {12, 6, 4, 4};
    for (int k = 0; k < 4; ++k) st[k].design(ord[k]);
  }
  // x → out[0..F-1]
  void up(float x, float* out, int nStages) {
    stages = nStages;
    out[0] = x * static_cast<float>(1 << nStages);
    int len = 1;
    for (int s = 0; s < nStages; ++s) {  // each sample becomes (H(v), H(0)), in time order
      float tmp[16];
      for (int i = 0; i < len; ++i) tmp[2 * i] = st[s].run(out[i]), tmp[2 * i + 1] = st[s].run(0.f);
      len *= 2;
      for (int i = 0; i < len; ++i) out[i] = tmp[i];
    }
  }
  float down(float* in, int nStages) {
    int len = 1 << nStages;
    for (int s = nStages - 1; s >= 0; --s) {
      for (int i = 0; i < len / 2; ++i) {
        const float a = st[s].run(in[2 * i]);
        st[s].run(in[2 * i + 1]);
        in[i] = a;
      }
      len /= 2;
    }
    return in[0];
  }
  void clearStates() {
    for (auto& s : st) s.clearStates();
  }
};

// 4× interpolating true-peak detector (385-tap half-sine-windowed sinc).
struct TruePeak {
  static constexpr int kTaps = 385, kHist = 97;
  float hist[kHist] = {};
  int pos = 0;
  static const std::array<float, kTaps>& taps() {
    static const std::array<float, kTaps> h = [] {
      std::array<float, kTaps> t{};
      for (int k = 0; k < kTaps; ++k) {
        const double win = std::max(0.0, std::sin(3.141592653589793 * k / (kTaps - 1)));
        const double x = static_cast<float>(k / 4.0 - 48.0);
        const double sc = std::fabs(x) < 1e-6 ? 1.0 : std::sin(3.141592653589793 * x) / (3.141592653589793 * x);
        t[static_cast<size_t>(k)] = static_cast<float>(win * sc);
      }
      return t;
    }();
    return h;
  }
  float run(float x) {
    const auto& h = taps();
    hist[pos] = x;
    float best = 0;
    for (int j = 0; j < 4; ++j) {
      float acc = 0;
      for (int m = 0; 4 * m + j < kTaps; ++m) acc += hist[(pos - m + kHist) % kHist] * h[static_cast<size_t>(4 * m + j)];
      best = std::max(best, std::fabs(acc));
    }
    pos = (pos + 1) % kHist;
    return best;
  }
  void clear() {
    std::fill(std::begin(hist), std::end(hist), 0.f);
    pos = 0;
  }
};

// Hold → floor at 1 → smoother → gain; busy statistics for the analysis.
struct GRStatic {
  PeakHold8 hold;
  AsymSmoother sm;
  HoldCell busyHold;
  float busyH = 0;
  int busyN = 0, total = 0;
  void set(double fs, double tAtt, double tHold, double tRel, bool eightStage) {
    hold.set(fs, tHold);
    sm.fixed8 = eightStage;
    sm.n = eightStage ? 8 : 1;
    sm.fs = fs;
    sm.recalcW();
    sm.setAtt(tAtt), sm.setRel(tRel);
    busyH = static_cast<float>(0.025 * fs);
  }
  float run(float d) {
    if (busyHold.run(d, busyH) >= 1.0018674f) ++busyN;
    ++total;
    const float y = sm.run(std::max(hold.run(d), 1.0f));
    return std::min(1.0f, 1.0f / std::max(y, kEps));
  }
  bool busy() const { return total > 0 && static_cast<float>(busyN) / static_cast<float>(total) > 0.9f; }
  void startBlock() { busyN = total = 0; }
};

// ---------------------------------------------------------------- the limiter
class Limiter {
 public:
  Limiter() {
    for (int c = 0; c < 2; ++c) {
      dl1_[c] = DelayGlide(1u << 17), dl2_[c] = DelayGlide(1u << 17);
      dC_[c] = DelayInt(1u << 15), dD_[c] = DelayInt(1u << 15), dry_[c] = DelayInt(1u << 16);
    }
  }
  bool ready() const { return fs_ > 0; }
  int latencySamples() const { return lat_; }
  float gainReductionDb() const { return grDb_; }

  void prepare(double fs, const Settings& s) {
    fs_ = fs;
    for (int c = 0; c < 2; ++c) {
      dc_[c].set(fs);
      lp320_[c].set(fs, 320);
      rmsLo_[c].set(fs, 5), rmsHi_[c].set(fs, 5);
      A_[c].hold.set(fs, 0.05);
      A_[c].lev.set(fs, 20);
      A_[c].sm.fs = fs;
      dl1_[c].prepare(fs);
      grD_[c].set(fs, 0.004, 0.004, 0.004, true);
      dD_[c].set(fs, 0.004);
      lmIn_[c].set(fs, 0.125), lmOut_[c].set(fs, 0.125);
    }
    gIn_.set(fs, 20), ceilR_.set(fs, 20), byp_.set(fs, 20);
    lp1000_.set(fs, 1000);
    loud_.n = 1, loud_.fs = fs, loud_.recalcW(), loud_.setAtt(0.05), loud_.setRel(0.05);
    fadeA_.set(fs, 100), fadeB_.set(fs, 100);
    cur_ = s;
    active_ = s;
    applyAll();
  }

  // Back to a fresh instance (no allocation): lines cleared, every state zeroed;
  // prepare() must follow.
  void reset() {
    clearLines();
    for (int c = 0; c < 2; ++c) {
      dc_[c] = HighPass1{}, lp320_[c] = OnePole{}, rmsLo_[c] = OnePole{}, rmsHi_[c] = OnePole{};
      lmIn_[c] = OnePole{}, lmOut_[c] = OnePole{};
      A_[c] = StageA{}, grB_[c] = GRStatic{}, grC_[c] = GRStatic{}, grD_[c] = GRStatic{};
      dl1_[c].d = dl2_[c].d = 0;
      upB_[c] = Resampler{}, dnB_[c] = Resampler{}, dryUp_[c] = Resampler{}, dryDn_[c] = Resampler{};
    }
    lp1000_ = gIn_ = ceilR_ = byp_ = fadeA_ = fadeB_ = OnePole{};
    loud_ = AsymSmoother{};
    pending_ = false, ready_ = true, busy_ = false, anValid_ = false;
    cnt_ = 0, count_ = 0, gate_ = 0, fs_ = 0;
  }

  void process(const Settings& s, float* L, float* R, int n) {
    if (fs_ <= 0) return;
    update(s);
    const int F = 1 << active_.os;
    const float fsf = static_cast<float>(fs_);
    float minG = 1;
    for (int i = 0; i < n; ++i) {
      if ((count_++ % kAnalysisBlock) == 0) latchAnalysis();
      float in[2] = {L[i], R[i]};
      if (cur_.dcBlock)
        for (int c = 0; c < 2; ++c) in[c] = dc_[c].run(in[c]);
      if (analysisOn_) analyse(in);
      // dry path
      float dry[2];
      for (int c = 0; c < 2; ++c) {
        dry[c] = dry_[c].run(in[c]);
        if (F > 1 && cur_.delta) {
          float buf[16];
          dryUp_[c].up(dry[c], buf, active_.os);
          dry[c] = dryDn_[c].down(buf, active_.os);
        }
      }
      // input gain, mid / side, detector, link, knee
      float a = in[0], b = in[1], gT = inLin_;
      if (active_.ms) a = in[0] + in[1], b = in[0] - in[1], gT = 0.5f * inLin_;
      const float g = gIn_.run(gT);
      float x[2] = {a * g, b * g};
      float d[2] = {std::fabs(x[0]), std::fabs(x[1])};
      const float mx = std::max(d[0], d[1]), lk = static_cast<float>(cur_.link);
      for (int c = 0; c < 2; ++c) {
        d[c] += (mx - d[c]) * lk;
        if (d[c] > d4_) d[c] = d4_ + (d[c] - d4_) * d8_;
      }
      // stage A
      for (int c = 0; c < 2; ++c) {
        const float gA = stageA(c, d[c]);
        if (!active_.lowLatency || active_.instant) x[c] = dl1_[c].run(x[c]);
        x[c] *= gA;
        minG = std::min(minG, gA);
      }
      // stage B
      for (int c = 0; c < 2; ++c) {
        if (F == 1) {
          const float gB = grB_[c].run(std::fabs(x[c]));
          x[c] = dl2_[c].run(x[c]) * gB;
          minG = std::min(minG, gB);
        } else {
          float u[16];
          upB_[c].up(x[c], u, active_.os);
          for (int k = 0; k < F; ++k) {
            const float gB = grB_[c].run(std::fabs(u[k]));
            u[k] = dl2_[c].run(u[k]) * gB;
            minG = std::min(minG, gB);
          }
          x[c] = dnB_[c].down(u, active_.os);
        }
      }
      // mid / side decode + stage C
      if (active_.ms) {
        const float l = x[0] + x[1], r = x[0] - x[1];
        x[0] = l, x[1] = r;
        for (int c = 0; c < 2; ++c) {
          const float gC = grC_[c].run(std::fabs(x[c]));
          x[c] = dC_[c].run(x[c]) * gC;
        }
      }
      // stage D + ceiling
      for (int c = 0; c < 2; ++c) {
        const float det = active_.truePeak ? tp_[c].run(x[c]) : std::fabs(x[c]);
        const float gD = grD_[c].run(det);
        x[c] = dD_[c].run(x[c]) * gD;
        minG = std::min(minG, gD);
      }
      const float cg = ceilR_.run(ceilGain_);
      for (float& v : x) v *= cg;
      // level matching
      if (cur_.levelMatch)
        for (int c = 0; c < 2; ++c) {
          const float pin = lmIn_[c].run(in[c] * in[c]), pout = lmOut_[c].run(x[c] * x[c]);
          x[c] *= pout <= 0 ? 2.f : std::min(2.f, std::sqrt(pin) / std::sqrt(pout));
        }
      if (active_.delta)
        for (int c = 0; c < 2; ++c) x[c] = dry[c] - x[c];
      // bypass cross-fade and gate
      const float sb = byp_.run(cur_.bypass ? 1.f : 0.f);
      for (int c = 0; c < 2; ++c) x[c] += (dry[c] - x[c]) * sb;
      if (!cur_.bypass) {
        cnt_ = ready_ ? std::min(cnt_ + 1, 1 << 30) : 0;
        const float g1 = fadeA_.run(static_cast<float>(cnt_) >= 0.1f * fsf ? 1.f : 0.f);
        gate_ = fadeB_.run(g1);
        x[0] *= gate_, x[1] *= gate_;
      }
      L[i] = x[0], R[i] = x[1];
    }
    grDb_ = dB(minG);
    // deferred changes wait for the closed gate
    if (gate_ < kEps && pending_) {
      clearLines();
      active_ = want_;
      applyAll();
      pending_ = false;
      ready_ = true;
    }
  }

 private:
  struct StageA {
    AsymSmoother sm;
    OnePole lev;
    PeakHold8 hold;
    float a0 = 0, b0 = 0, s2 = 0;
  };

  // ---- parameter application (in the reference's tag order)
  void update(const Settings& s) {
    Settings o = cur_;
    cur_.bypass = s.bypass, cur_.link = s.link, cur_.dcBlock = s.dcBlock;
    if (s.delta != o.delta) cur_.delta = s.delta, applyCeiling();
    if (s.order != o.order) cur_.order = s.order, applyOrder();
    if (s.mode != o.mode) cur_.mode = s.mode, applyMode();
    if (s.inDb != o.inDb) cur_.inDb = s.inDb, applyInput();
    if (s.amount != o.amount) cur_.amount = s.amount, applyAmount();
    if (s.ceilN != o.ceilN) cur_.ceilN = s.ceilN, applyCeiling();
    if (s.levelMatch != o.levelMatch) cur_.levelMatch = s.levelMatch, applyCeiling();
    if (s.hardness != o.hardness) cur_.hardness = s.hardness, applyHardness();
    // deferred: delta (output path), true peak, M/S, oversampling, low latency, instant recovery
    want_ = active_;
    want_.delta = s.delta, want_.truePeak = s.truePeak, want_.ms = s.ms, want_.os = s.os;
    want_.lowLatency = s.lowLatency, want_.instant = s.instant;
    const bool differs = want_.delta != active_.delta || want_.truePeak != active_.truePeak || want_.ms != active_.ms ||
                         want_.os != active_.os || want_.lowLatency != active_.lowLatency || want_.instant != active_.instant;
    if (differs && !pending_) {
      pending_ = true;
      ready_ = false;
    }
  }
  void applyAll() {
    // prepare order: 0..12, 25, 27, 26 (deferred values from active_)
    applyCeiling();
    applyOrder();
    applyMode();
    applyInput();
    applyAmount();
    applyCeiling();
    applyHardness();
    applyOs();
    applyInstant();
  }
  void applyOrder() {
    const int n = std::clamp(cur_.order, 1, 4);
    for (auto& a : A_) a.sm.setN(n);
    applyInstant();
  }
  void applyInstant() {
    irk_ = 10.f / static_cast<float>(std::clamp(cur_.order, 1, 4));
    applyAmount();
  }
  void applyMode() {
    analysisOn_ = cur_.mode <= 1;
    applyAmount();
    applyInput();
  }
  void applyInput() {
    inDb_ = static_cast<float>(cur_.inDb);
    inLin_ = lin(inDb_);
    invIn_ = 1.f / inLin_;
    applyCeiling();
  }
  void setZ(float z) {
    const float G = std::log(10.f * z + 1.f) / std::log(11.f);
    d8_ = 1.f - G;
    d4_ = (inLin_ - 1.f) * d8_ + 1.f;
  }
  void applyAmount() {
    static const float T[4] = {1.f, 0.41421357f, 0.25992104f, 0.18920712f};
    auto E = [](float u) { return (std::pow(10.f, u) - 1.f) / 9.f; };
    const int o = std::clamp(cur_.order, 1, 4) - 1;
    const float a = static_cast<float>(cur_.amount);
    float z = 0, R = 0, c4 = 0, c = 1;
    switch (cur_.mode) {
      case 0: R = 10.f * T[o] + 0.05f; break;
      case 2:
        R = 10.f * T[o] * E(0.8f * a + 0.2f) + 0.05f;
        c4 = (1 - a) * (1 - a) / 2;
        break;
      case 1:
        z = a;
        R = 10.f * T[o] * E(0.2f) + 0.05f;
        break;
      default:
        z = a;
        R = 10.f * T[o] * E(0.2f) + 0.05f;
        c4 = (1 - a) * (1 - a) / 2;
        break;
    }
    if (cur_.mode != 0) c = 1 + 10 * c4 * c4;
    setZ(z);
    attack_ = R;
    for (auto& s : A_) s.sm.setAtt(R);
    pFast_ = 0.15f * c, pSlow_ = 0.6f * c * c;
  }
  void applyCeiling() {
    if (cur_.delta) {
      ceilGain_ = cur_.levelMatch ? 1.f : 1.f / inLin_;
    } else if (cur_.ceilN <= 0) {
      ceilGain_ = 0;
    } else {
      const float db = static_cast<float>(ceilingDb(cur_.ceilN));
      ceilGain_ = std::min(lin(db), 0.99966854f);
    }
  }
  void applyHardness() {
    static const float HF[10] = {3.f, 2.f, 1.5f, 1.25f, 1.125f, 1.0625f, 1.03125f, 1.01562f, 1.00781f, 1.00391f};
    const int h = std::clamp(cur_.hardness, 0, 9);
    const float fsf = static_cast<float>(fs_), w = window(h), wC = std::min(0.008f, w);
    const int F = 1 << active_.os;
    for (int c = 0; c < 2; ++c) {
      dl1_[c].target(std::floor(fsf * 0.05f) - std::floor(fsf * w));
      dl2_[c].prepare(fs_ * F);
      dl2_[c].target(std::floor(fsf * w) * static_cast<float>(F));
      grB_[c].set(fs_ * F, HF[h] * w, w, w, true);
      grC_[c].set(fs_, wC, wC, wC, false);
      dC_[c].set(fs_, wC);
    }
    hard_ = h;
    updateLatency();
  }
  void applyOs() { applyHardness(); }
  void updateLatency() {
    lat_ = latency(withActive(), fs_);
    for (auto& d : dry_) d.setSamples(lat_);
  }
  Settings withActive() const {
    Settings s = cur_;
    s.ms = active_.ms, s.lowLatency = active_.lowLatency, s.instant = active_.instant, s.os = active_.os;
    return s;
  }

  // ---- stage A (adaptive release)
  float stageA(int c, float d) {
    StageA& A = A_[c];
    const float p = A.hold.run(d);
    float rel;
    if (active_.instant) {
      rel = 0.005f;
    } else {
      const float lev = std::max(A.lev.run(p * invIn_), kEps);
      const float D1 = dB(lev / std::max(A.a0, kEps)), D2 = dB(A.a0);
      const float t = std::sqrt(std::fabs(D2 + 0.33f * inDb_)) * irk_ + 1.f;
      rel = ((6.f - inDb_ < D1) && (A.s2 < A.b0) ? pFast_ : pSlow_) * ((D1 < -0.5f * inDb_ || D2 < -0.33f * inDb_) ? t : 1.f);
    }
    A.sm.setRel(rel);
    const float y = A.sm.run(std::max(p, 1.f));
    const float gA = std::min(1.f, 1.f / std::max(y, kEps));
    const float bNew = A.sm.n == 1 ? gA : std::min(1.f, 1.f / std::max(A.sm.s[0], kEps));
    A.s2 = A.b0, A.a0 = gA, A.b0 = bNew;
    return gA;
  }

  // ---- analysis (modes 0 / 1)
  static float tanh7(float x) {
    const float x2 = x * x, x4 = x2 * x2, x6 = x4 * x2;
    return x * (135135.f + 17325.f * x2 + 378.f * x4 + x6) / (135135.f + 62370.f * x2 + 3150.f * x4 + 28.f * x6);
  }
  void analyse(const float* in) {
    float q[2];
    for (int c = 0; c < 2; ++c) {
      const float lo = lp320_[c].run(in[c]), hi = in[c] - lo;
      const float rl = std::sqrt(std::max(0.f, rmsLo_[c].run(lo * lo))), rh = std::sqrt(std::max(0.f, rmsHi_[c].run(hi * hi)));
      q[c] = tanh7(rl / std::max(rh, kEps));
    }
    const float s = cur_.mode == 0 ? 10.f : 40.f;
    const float m = std::min(tanh7(s * (1 - q[0])), tanh7(s * (1 - q[1])));
    anM_ = m;
    anRel_ = lp1000_.run(busy_ ? std::max(std::sqrt(m) * attack_, active_.instant ? 0.05f : 0.5f) : attack_);
    anLz_ = loud_.run(busy_ ? std::max(m, 1e-6f) : 1.f);
    anValid_ = true;
  }
  void latchAnalysis() {
    busy_ = grB_[0].busy() && grB_[1].busy();
    grB_[0].startBlock(), grB_[1].startBlock();
    if (!analysisOn_ || !anValid_) return;
    if (cur_.mode == 0) {
      for (auto& a : A_) a.sm.setAtt(anRel_);
      const float k = std::min(1.f, 1.f - inDb_ / 24.f);
      setZ(std::sqrt(std::max(0.f, std::sqrt(std::max(0.f, anM_)) * k)));
    } else {
      setZ(std::sqrt(std::sqrt(std::max(anLz_, 0.f))));
    }
  }

  void clearLines() {
    for (int c = 0; c < 2; ++c) {
      dl1_[c].clear(), dl2_[c].clear(), dC_[c].clear(), dD_[c].clear(), dry_[c].clear();
      upB_[c].clearStates(), dnB_[c].clearStates(), dryUp_[c].clearStates(), dryDn_[c].clearStates();
      tp_[c].clear();
    }
  }

  double fs_ = 0;
  Settings cur_, active_, want_;
  bool pending_ = false, ready_ = true, analysisOn_ = true, busy_ = false, anValid_ = false;
  int cnt_ = 0, lat_ = 0, hard_ = 7;
  uint64_t count_ = 0;
  float gate_ = 0, grDb_ = 0;
  float inDb_ = 0, inLin_ = 1, invIn_ = 1, d4_ = 1, d8_ = 1, attack_ = 0, pFast_ = 0.15f, pSlow_ = 0.6f, irk_ = 10, ceilGain_ = 1;
  float anM_ = 0, anRel_ = 0, anLz_ = 1;
  HighPass1 dc_[2];
  OnePole lp320_[2], rmsLo_[2], rmsHi_[2], lp1000_, gIn_, ceilR_, byp_, lmIn_[2], lmOut_[2], fadeA_, fadeB_;
  AsymSmoother loud_;
  StageA A_[2];
  DelayGlide dl1_[2], dl2_[2];
  GRStatic grB_[2], grC_[2], grD_[2];
  DelayInt dC_[2], dD_[2], dry_[2];
  Resampler upB_[2], dnB_[2], dryUp_[2], dryDn_[2];
  TruePeak tp_[2];
};

}  // namespace gl::packs::dynamics::emphref

// Gridline — "synths" pack, J-33 group "hex": Hex Synth's plate reverb, the
// reference instrument's engine as analysed (figure-of-eight tank):
//   mono input → one-pole high cut → first-order low cut ((x − AP) / 2) →
//   4 input diffusers (the 2nd and 4th read with g2 but feed back with g1) →
//   predelay → added into both tank halves after their bass shelves; each half:
//   ×g, bass shelf, damping one-pole, all-pass, delay, ×g, all-pass, delay.
//   The early reflections tap the first diffuser's output (one tap per side).
//   Output: 14 signed taps weighted g + c(1 − g); width as mid / side.
// Room size, diffusion, bass and levels keep the engine's default program; the
// synth drives high cut, decay, damping and the tail level (laws in the slot).
// Lengths: line k = T[k]·S / 20000 samples with S = (size + 15)·sr / 115.
#include <algorithm>
#include <cmath>
#include <vector>
#include "hex_fx.h"

namespace gl::packs::synths::hex {

namespace {

constexpr int kT[12] = {142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
constexpr int kU[14] = {266, 2974, 1913, 2496, 1690, 187, 1566, 353, 3627, 1728, 3173, 1811, 335, 621};
constexpr int kTapLine[14] = {9, 9, 10, 11, 5, 6, 7, 5, 5, 6, 7, 9, 10, 11};
constexpr int kEL0 = 1102, kER0 = 837;

int paramInt(int idx, float v) {
  static const float A[18] = {421, 2981, 216, 250, 99, 100, 28, 1975, 199, 216, 421, 421, 421, 200, 1, 1, 1, 1};
  static const float B[18] = {-360.5f, 19.5f, 5.5f, 0.5f, 1.5f, 0.5f, 2.5f, 25.5f, 1.5f, 5.5f, -360.5f, -360.5f, -360.5f,
                              0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
  return static_cast<int>(std::floor(v * A[idx] + B[idx]));
}

}  // namespace

struct PlateReverb::Impl {
  float sr = 44100.f;
  int sri = 44100;
  int p[18];
  bool dirty = true;
  // Buffers.
  std::vector<float> line[12];
  int len[12] = {}, idx[12] = {};
  std::vector<float> pre, er;
  int preLen = 1, preIdx = 0, erN = 2, erIdx = 0;
  int tap[14] = {};
  int tl0 = 1, tr0 = 1;
  float gl0 = 0, gr0 = 0;
  int lastSize = 0;
  // Coefficients.
  float preFb = 0;
  bool dw = false;
  double dwDamp = 0.5, dwDiff = 0.75, dwRt = 1;
  float gin = 1, k = 0, ahc = 1, admp = 1, gt1 = 0, gt2 = 0, g1 = 0, g2 = 0, g = 0, csh = 0, ash = 0, dry = 1, erl = 0, tgt = 0;
  // State.
  float hc = 0, ax = 0, ay = 0, dA = 0, dB = 0, sAx = 0, sAy = 0, sBx = 0, sBy = 0, tg = -1000.f;

  Impl() {
    static const float def[18] = {0.85748219f, 0.018785642f, 0.16203703f, 0.0f, 0.49494949f, 1.0f, 0.2857143f, 0.24050634f,
                                  0.070351757f, 0.16203703f, 0.85748219f, 0.77434677f, 0.57244658f, 0.5f, 0, 0, 0, 0};
    for (int i = 0; i < 18; ++i) p[i] = paramInt(i, def[i]);
    allocate(44100.f);
  }

  void allocate(float rate) {
    sr = rate;
    sri = static_cast<int>(rate);
    for (int i = 0; i < 12; ++i) {
      const int cap = std::max(1, static_cast<int>(static_cast<int64_t>(kT[i]) * sri / 20000));
      line[i].assign(static_cast<size_t>(cap) + 1, 0.f);
      len[i] = cap, idx[i] = 0;
    }
    pre.assign(static_cast<size_t>(std::max(1, sri / 4)) + 1, 0.f);
    preIdx = 0;
    const int x = static_cast<int>(static_cast<double>(sri) * 0.3163265306122449);
    int nb = 0;
    for (int v = x; v > 0; v >>= 1) ++nb;
    erN = 2 << nb;
    er.assign(static_cast<size_t>(erN), 0.f);
    erIdx = 0;
    dirty = true;
  }

  void updateDw() {
    dirty = false;
    gin = 1.f;
    {
      const float t = std::tan(296.880524f / sr);
      k = (t - 1.f) / (t + 1.f);
    }
    auto onePoleHz = [&](double hz) {
      const float c = std::cos(static_cast<float>(hz) * 6.2831855f / sr);
      return (c - 1.f) + std::sqrt((2.f - c) * (2.f - c) - 1.f);
    };
    ahc = onePoleHz(22000.0);
    admp = onePoleHz((216.0 * dwDamp + 5.0) * 100.0);
    const int S = (p[4] + 15) * sri / 115;
    for (int i = 0; i < 12; ++i) {
      const int64_t ts = static_cast<int64_t>(kT[i]) * S;
      int L = ts < 20000 ? 1 : static_cast<int>(ts / 20000);
      L = std::min(L, static_cast<int>(line[i].size()) - 1);
      len[i] = std::max(1, L);
      idx[i] %= len[i];
    }
    preLen = 1;
    preIdx = 0;
    for (int j = 0; j < 14; ++j) tap[j] = static_cast<int>(static_cast<int64_t>(kU[j]) * S / 20000);
    if (p[4] != lastSize) {
      lastSize = p[4];
      for (int i = 0; i < 12; ++i) std::fill(line[i].begin(), line[i].begin() + len[i], 0.f);
    }
    if (dwDiff <= 0) {
      gt1 = gt2 = g1 = g2 = 0;
    } else {
      const float v = static_cast<float>(dwDiff);
      gt1 = std::exp((static_cast<float>(len[4] + len[8]) * 0.5f / sr) * -6.9077554f / (0.35f * v));
      gt2 = std::exp((static_cast<float>(len[6] + len[10]) * 0.5f / sr) * -6.9077554f / (0.51f * v));
      g1 = std::exp((static_cast<float>(len[0] + len[1]) * 0.5f / sr) * -6.9077554f / (0.07f * v));
      g2 = g1;  // this build feeds every diffuser with the same gain
    }
    const float X = static_cast<float>(len[5] + len[7] + len[9] + len[11]) * -6.9077554f / sr;
    g = std::pow(std::exp(X / static_cast<float>(dwRt)), 0.25f);
    csh = 0, ash = 0;
    dry = 0, erl = 0, tgt = 0.5f, tg = 0.5f;
  }

  void update() {
    if (dw) return updateDw();
    dirty = false;
    gin = p[14] != 0 ? 0.f : (p[0] < -360 ? 0.f : std::pow(10.f, static_cast<float>(p[0]) * 0.005f));
    if (p[1] < 20) k = 0;
    else {
      const float fc = std::min(0.5f * sr, static_cast<float>(p[1]));
      const float t = std::tan(fc * 3.1415927f / sr);
      k = (t - 1.f) / (t + 1.f);
    }
    auto onePole = [&](int pv) {
      if (pv > 220) return 1.f;
      const float c = std::cos(static_cast<float>(pv) * 628.31854f / sr);
      return (c - 1.f) + std::sqrt((2.f - c) * (2.f - c) - 1.f);
    };
    ahc = onePole(p[2]);
    admp = onePole(p[9]);
    const int S = (p[4] + 15) * sri / 115;
    for (int i = 0; i < 12; ++i) {
      const int64_t ts = static_cast<int64_t>(kT[i]) * S;
      int L = ts < 20000 ? 1 : static_cast<int>(ts / 20000);
      L = std::min(L, static_cast<int>(line[i].size()) - 1);
      len[i] = std::max(1, L);
      idx[i] %= len[i];
    }
    preLen = std::max(1, static_cast<int>(static_cast<int64_t>(p[3]) * sri / 1000));
    preLen = std::min(preLen, static_cast<int>(pre.size()) - 1);
    preIdx %= preLen;
    for (int j = 0; j < 14; ++j) tap[j] = static_cast<int>(static_cast<int64_t>(kU[j]) * S / 20000);
    const float s = static_cast<float>((p[4] + 3) * sri / 103) * 3.4013603e-05f;
    tl0 = std::max(1, static_cast<int>(kEL0 * s));
    tr0 = std::max(1, static_cast<int>(kER0 * s));
    const int m = std::min(tl0, tr0);
    gl0 = static_cast<float>(m) * 0.5f / static_cast<float>(tl0);
    gr0 = static_cast<float>(m) * 0.36f / static_cast<float>(tr0);
    if (p[4] != lastSize) {
      lastSize = p[4];
      for (int i = 0; i < 12; ++i) std::fill(line[i].begin(), line[i].begin() + len[i], 0.f);
      std::fill(er.begin(), er.end(), 0.f);
    }
    if (static_cast<float>(p[5]) * 0.01f == 0.f) {
      gt1 = gt2 = g1 = g2 = 0;
    } else {
      const float D = static_cast<float>(p[5]) * sr;
      gt1 = std::exp(static_cast<float>(len[4] + len[8]) * -3.4538777f / (D * 0.0035f));
      gt2 = std::exp(static_cast<float>(len[6] + len[10]) * -3.4538777f / (D * 0.0051f));
      g1 = std::exp(static_cast<float>(len[0] + len[1]) * -3.4538777f / (D * 0.0007f));
      g2 = std::exp(static_cast<float>(len[2] + len[3]) * -3.4538777f / (D * 0.0011f));
    }
    const float X = static_cast<float>(len[5] + len[7] + len[9] + len[11]) * -6.9077554f * (1.f / sr);
    const float E = std::exp(X / (0.1f * static_cast<float>(p[8])));
    const float q = std::sqrt(E);
    g = std::fabs(q) < 1.17549435e-38f ? 0.f : std::sqrt(q);
    const float Eb = std::exp(X / ((static_cast<float>(p[8]) * 0.010000001f) * static_cast<float>(p[6])));
    const float gb = (q > 0.f) ? std::sqrt(std::sqrt(Eb)) / std::sqrt(q) : 0.f;
    csh = gb * 0.5f - 0.5f;
    const float gbc = std::min(gb, 1.f);
    const float t = std::tan(static_cast<float>(p[7]) * 3.1415927f * (1.f / sr));
    ash = (t - gbc) / (gbc + t);
    dry = p[15] != 0 ? 0.f : (p[10] < -360 ? 0.f : std::pow(10.f, static_cast<float>(p[10]) * 0.005f));
    erl = p[16] != 0 ? 0.f : (p[11] < -360 ? 0.f : 0.5f * std::pow(10.f, static_cast<float>(p[11]) * 0.005f));
    tgt = p[17] != 0 ? 0.f : (p[12] < -360 ? 0.f : 0.3f * std::pow(10.f, static_cast<float>(p[12]) * 0.005f));
  }

  void reset() {
    for (auto& l : line) std::fill(l.begin(), l.end(), 1e-30f);
    std::fill(pre.begin(), pre.end(), 1e-30f);
    std::fill(er.begin(), er.end(), 1e-30f);
    hc = ax = ay = dA = dB = sAx = sAy = sBx = sBy = 0;
    tg = -1000.f;
  }

  void process(float* L, float* R, int n) {
    if (dirty) update();
    const float d = 1.f - g;
    const float w6 = d * 0.6f + g, w25 = d * 0.25f + g, w2 = d * 0.2f + g, w7 = d * 0.7f + g, w5 = d * 0.5f + g;
    const float wid = static_cast<float>(p[13]) * 0.01f;
    float ERl = 0, ERr = 0;
    float* b[12];
    for (int i = 0; i < 12; ++i) b[i] = line[i].data();
    auto wrap = [&](int i) {
      if (idx[i] <= 0) idx[i] = len[i];
    };
    auto tapRead = [&](int j, int lineIdx, int base) {
      int a = base + tap[j];
      const int Ln = len[lineIdx];
      while (a >= Ln) a -= Ln;
      while (a < 0) a += Ln;
      return b[lineIdx][a];
    };
    for (int s = 0; s < n; ++s) {
      const float xL = gin * L[s], xR = gin * R[s];
      hc = ((xR + xL) * 0.5f + 1e-30f - hc) * ahc + hc;
      const float ap = (hc - ay) * k + ax;
      ay = ap, ax = hc;
      const float u = k != 0.f ? (hc - ap) * 0.5f + 1e-30f : hc;
      if (erl != 0.f) {
        ERl = gl0 * er[static_cast<size_t>((erIdx + tl0) & (erN - 1))];
        ERr = gr0 * er[static_cast<size_t>((erIdx + tr0) & (erN - 1))];
      }
      const float v1 = b[0][idx[0]] - g1 * u;
      b[0][idx[0]] = g1 * v1 + u;
      wrap(0);
      const float v2 = b[1][idx[1]] - g2 * v1;
      b[1][idx[1]] = g1 * v2 + v1;
      wrap(1);
      er[static_cast<size_t>(erIdx)] = v1;
      if (erIdx <= 0) erIdx = erN;
      const float v3 = b[2][idx[2]] - g1 * v2;
      b[2][idx[2]] = g1 * v3 + v2;
      wrap(2);
      const float v4 = b[3][idx[3]] - g2 * v3;
      b[3][idx[3]] = g1 * v4 + v3;
      wrap(3);
      float pd = pre[static_cast<size_t>(preIdx)];
      pre[static_cast<size_t>(preIdx)] = v4 + preFb * pd;
      if (preIdx <= 0) preIdx = preLen;
      if (!dw) pd += 1e-20f;
      // Tank half A.
      const float a = g * b[11][idx[11]];
      const float sh = (a - sAy) * ash + sAx;
      sAy = sh, sAx = a;
      const float y = (a + pd) + (sh + a) * csh;
      dA = (y - dA) * admp + dA;
      const float w = b[4][idx[4]] - gt1 * dA;
      b[4][idx[4]] = gt1 * w + dA;
      wrap(4);
      const float o5 = b[5][idx[5]];
      b[5][idx[5]] = w;
      wrap(5);
      const float bb = o5 * g;
      const float w2v = b[6][idx[6]] - gt2 * bb;
      b[6][idx[6]] = gt2 * w2v + bb;
      wrap(6);
      b[7][idx[7]] = w2v;
      wrap(7);
      // Tank half B.
      const float c = g * b[7][idx[7] - 1];
      const float sh2 = (c - sBy) * ash + sBx;
      sBy = sh2, sBx = c;
      const float y2 = (c + pd) + (sh2 + c) * csh;
      dB = (y2 - dB) * admp + dB;
      const float w3 = b[8][idx[8]] - gt1 * dB;
      b[8][idx[8]] = gt1 * w3 + dB;
      wrap(8);
      const float o9 = b[9][idx[9]];
      b[9][idx[9]] = w3;
      wrap(9);
      const float e2 = o9 * g;
      const float w4 = b[10][idx[10]] - gt2 * e2;
      b[10][idx[10]] = gt2 * w4 + e2;
      wrap(10);
      for (int i : {5, 6, 7, 9, 10}) --idx[i];
      b[11][idx[11]] = w4;
      wrap(11);
      // Output taps.
      float A[14];
      for (int j = 0; j < 14; ++j) {
        const int ln = kTapLine[j];
        A[j] = tapRead(j, ln, ln == 11 ? idx[11] - 1 : idx[ln]);
      }
      const float P = (w2 * A[3] + w25 * A[1]) + w7 * A[2];
      const float tailL = w5 * A[6] + (A[5] + ((A[0] + w6 * A[4]) - P));
      const float Q = (w7 * A[9] + w2 * A[10]) + w6 * A[11];
      const float tailR = (w5 * A[13] + (A[12] + (A[7] + w25 * A[8]))) - Q;
      if (dw) {
        tg = 0.5f;
      } else {
        if (tg < -99.f) tg = tgt;
        tg = (tgt - tg) * 0.001f + tg;
      }
      const float Lw = erl * ERl + tg * tailL, Rw = erl * ERr + tg * tailR;
      const float M = Lw + Rw, Sd = (Lw - Rw) * wid;
      L[s] = (dry * xL + M) + Sd;
      R[s] = dry * xR + (M - Sd);
      for (int i : {0, 1, 2, 3, 4, 8, 11}) --idx[i];
      --erIdx;
      --preIdx;
    }
  }
};

PlateReverb::PlateReverb() : d_(std::make_unique<Impl>()) {}
PlateReverb::~PlateReverb() = default;
void PlateReverb::configure(double sr) {
  if (static_cast<float>(sr) != d_->sr) d_->allocate(static_cast<float>(sr));
  d_->reset();
}
void PlateReverb::reset() { d_->reset(); }
void PlateReverb::setHighCut(double v) { d_->p[2] = paramInt(2, static_cast<float>(v)), d_->dirty = true; }
void PlateReverb::setDecay(double v) { d_->p[8] = paramInt(8, static_cast<float>(v)), d_->dirty = true; }
void PlateReverb::setDamping(double v) { d_->p[9] = paramInt(9, static_cast<float>(v)), d_->dirty = true; }
void PlateReverb::setWet(double v) { d_->p[12] = paramInt(12, static_cast<float>(v)), d_->dirty = true; }
void PlateReverb::setParam(int idx, double v) {
  if (idx < 0 || idx >= 18) return;
  const int pv = paramInt(idx, static_cast<float>(v));
  if (pv != d_->p[idx]) d_->p[idx] = pv, d_->dirty = true;
}
void PlateReverb::setPredelayFeedback(double fb) { d_->preFb = static_cast<float>(fb); }
void PlateReverb::setSamplerMode(double room, double damping, double diffusion, double decay) {
  d_->dw = true;
  d_->p[4] = static_cast<int>(99.0 * room + 1.0);
  d_->p[13] = 100;  // width 100 %
  d_->dwDamp = damping, d_->dwDiff = diffusion;
  d_->dwRt = (199.0 * decay * decay + 1.0) / 10.0;
  d_->dirty = true;
}
void PlateReverb::process(float* L, float* R, int n) { d_->process(L, R, n); }

}  // namespace gl::packs::synths::hex

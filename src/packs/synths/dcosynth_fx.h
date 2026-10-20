// Gridline — "synths" pack, J-22: DCO Synth effect blocks (realtime-safe:
// every buffer is sized in the constructor for sample rates up to 192 kHz).
//   BbdChorus   two-line bucket-brigade style chorus with modes I / II / I+II
//               and optional hiss (gated by note activity when asked)
//   ThreeBandEq low shelf / peak / high shelf
//   StereoDelay left time + right tap, feedback filter (LP / HP / BP / off),
//               cross-feedback (stereo), blur (diffusion) and tone
//   Plate       8-line feedback delay network with predelay (+ feedback),
//               early reflections, size, diffusion, decay, damping, low / high
//               cut, width, dry / wet
//   Ensemble    2 or 4 modulated lines with drift ("movement"), feedback,
//               low / high pass on the wet signal and line phase spread
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../../engine/Dsp.h"

namespace gl::packs::synths::dcofx {

inline constexpr double kMaxRate = 192000.0;

// Fractional read from a circular buffer (linear interpolation).
inline float readFrac(const std::vector<float>& b, int w, double delay) {
  const int len = static_cast<int>(b.size());
  double rp = w - delay;
  while (rp < 0) rp += len;
  const int i0 = static_cast<int>(rp) % len;
  const int i1 = (i0 + 1) % len;
  const double f = rp - std::floor(rp);
  return static_cast<float>(b[static_cast<size_t>(i0)] + (b[static_cast<size_t>(i1)] - b[static_cast<size_t>(i0)]) * f);
}

// One-pole low / high pass helper.
struct OnePole {
  double a = 1, z = 0;
  void set(double sr, double hz) { a = 1.0 - std::exp(-dsp::kTwoPi * std::clamp(hz, 5.0, sr * 0.49) / sr); }
  double lp(double x) { return z += a * (x - z); }
  double hp(double x) { return x - lp(x); }
};

// ------------------------------------------------------------- BBD chorus
struct BbdChorus {
  std::vector<float> bufL, bufR;
  int w = 0;
  double sr = 44100, ph1 = 0, ph2 = 0.25;
  int mode = 0;  // 0 off, 1 I, 2 II, 3 I+II
  double noise = 0;
  bool gate = false;
  bool ref = false;  // reference modes: I 0.4 Hz / II 0.6 Hz over 1.08–5.23 ms, I+II 8 Hz over 2.85–3.37 ms
  uint32_t rnd = 12345;
  BbdChorus() : bufL(static_cast<size_t>(0.05 * kMaxRate) + 4, 0.f), bufR(static_cast<size_t>(0.05 * kMaxRate) + 4, 0.f) {}
  void configure(double s, int m, double noiseAmt, bool g) {
    sr = s;
    mode = m;
    noise = noiseAmt;
    gate = g;
  }
  void clear() {
    std::fill(bufL.begin(), bufL.end(), 0.f);
    std::fill(bufR.begin(), bufR.end(), 0.f);
  }
  // In-place on a stereo block; `active` = any voice sounding (hiss gate).
  void process(float* L, float* R, int n, bool active) {
    if (mode == 0) return;
    // Rates / depths of the classic modes (Hz, ms).
    const double rate = ref ? (mode == 1 ? 0.4 : mode == 2 ? 0.6 : 8.0) : (mode == 1 ? 0.513 : mode == 2 ? 0.863 : 9.75);
    const double depthMs = ref ? (mode == 3 ? 0.26 : 2.075) : (mode == 3 ? 0.35 : 1.6);
    const double baseMs = ref ? (mode == 3 ? 3.11 : 3.155) : (mode == 3 ? 3.2 : 3.5);
    const int len = static_cast<int>(bufL.size());
    const double hiss = (gate && !active) ? 0.0 : (ref ? 0.2 * noise * noise * noise : noise * noise * 0.004);
    for (int i = 0; i < n; ++i) {
      rnd = rnd * 1664525u + 1013904223u;
      const double nz = (static_cast<double>(rnd >> 9) / 4194304.0 - 1.0) * hiss;
      const double in = 0.5 * (L[i] + R[i]) + nz;
      bufL[static_cast<size_t>(w)] = static_cast<float>(in);
      ph1 += rate / sr;
      if (ph1 >= 1.0) ph1 -= 1.0;
      const double tri = ph1 < 0.5 ? 4.0 * ph1 - 1.0 : 3.0 - 4.0 * ph1;
      const double dL = (baseMs + depthMs * tri) * 0.001 * sr;
      const double dR = (baseMs - depthMs * tri) * 0.001 * sr;
      const double wl = readFrac(bufL, w, dL), wr = readFrac(bufL, w, dR);
      L[i] = static_cast<float>(L[i] + wl);
      R[i] = static_cast<float>(R[i] + wr);
      w = (w + 1) % len;
    }
  }
};

// ----------------------------------------------------------------- 3-band EQ
struct ThreeBandEq {
  dsp::Biquad b[3][2];
  bool on = false;
  void configure(double sr, bool enable, const double gainDb[3], const double hz[3], const double q[3]) {
    on = enable;
    for (int c = 0; c < 2; ++c) {
      b[0][c].set(dsp::Biquad::LowShelf, sr, hz[0], q[0], gainDb[0]);
      b[1][c].set(dsp::Biquad::Peak, sr, hz[1], q[1], gainDb[1]);
      b[2][c].set(dsp::Biquad::HighShelf, sr, hz[2], q[2], gainDb[2]);
    }
  }
  void process(float* L, float* R, int n) {
    if (!on) return;
    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      for (int k = 0; k < 3; ++k) {
        l = b[k][0].process(l);
        r = b[k][1].process(r);
      }
      L[i] = static_cast<float>(l);
      R[i] = static_cast<float>(r);
    }
  }
  void clear() {
    for (auto& bb : b)
      for (auto& x : bb) x.clear();
  }
};

// ------------------------------------------------------------- stereo delay
struct StereoDelay {
  std::vector<float> bl, br;
  int w = 0;
  double sr = 44100;
  bool on = false;
  double input = 0.5, tL = 0.3, tR = 0.3, fb = 0.4, stereo = 0, blur = 0, tone = 0.5;
  int fbType = 0;
  double fbHz = 4000;
  dsp::Svf fL, fR;
  dsp::Allpass1 apL, apR;
  OnePole toneL, toneR;
  void configure(double s) {
    sr = s;
    apL.setFreq(sr, 900.0);
    apR.setFreq(sr, 1300.0);
  }
  double smoothL = 0, smoothR = 0;
  StereoDelay() : bl(static_cast<size_t>(4.1 * kMaxRate), 0.f), br(static_cast<size_t>(4.1 * kMaxRate), 0.f) {}
  void clear() {
    std::fill(bl.begin(), bl.end(), 0.f);
    std::fill(br.begin(), br.end(), 0.f);
  }
  void process(float* L, float* R, int n) {
    if (!on) return;
    const int len = static_cast<int>(bl.size());
    const double maxD = len - 4;
    fL.set(sr, fbHz, 0.3);
    fR.set(sr, fbHz, 0.3);
    toneL.set(sr, 400.0 * std::pow(40.0, tone));
    toneR.set(sr, 400.0 * std::pow(40.0, tone));
    for (int i = 0; i < n; ++i) {
      smoothL += 0.0005 * (std::min(maxD, tL * sr) - smoothL);
      smoothR += 0.0005 * (std::min(maxD, tR * sr) - smoothR);
      double dl = readFrac(bl, w, std::max(1.0, smoothL));
      double dr = readFrac(br, w, std::max(1.0, smoothR));
      auto filt = [&](dsp::Svf& f, double x) {
        double bp = 0, hp = 0;
        const double lp = f.process(x, &bp, &hp);
        switch (fbType) {
          case 0: return lp;
          case 1: return hp;
          case 2: return bp;
          default: return x;
        }
      };
      double fl = filt(fL, dl), fr = filt(fR, dr);
      if (blur > 0) {
        fl = fl + blur * (apL.process(fl) - fl);
        fr = fr + blur * (apR.process(fr) - fr);
      }
      const double inL = L[i] * input, inR = R[i] * input;
      const double mixL = (1.0 - stereo) * fl + stereo * fr, mixR = (1.0 - stereo) * fr + stereo * fl;
      bl[static_cast<size_t>(w)] = static_cast<float>(inL + fb * mixL);
      br[static_cast<size_t>(w)] = static_cast<float>(inR + fb * mixR);
      L[i] = static_cast<float>(L[i] + toneL.lp(dl));
      R[i] = static_cast<float>(R[i] + toneR.lp(dr));
      w = (w + 1) % len;
    }
  }
};

// --------------------------------------------------------------- plate reverb
struct Plate {
  static constexpr int kLines = 8;
  std::array<std::vector<float>, kLines> line;
  std::array<int, kLines> pos{}, len{};
  std::array<double, kLines> damp{};
  std::vector<float> pre;
  int prePos = 0;
  double sr = 44100;
  bool on = false;
  double lowCut = 20, highCut = 16000, predelay = 0, preFb = 0, size = 0.5, diffusion = 0.5, decay = 2, dampAmt = 0.3,
         width = 1, dry = 1, early = 0, wet = 0.3;
  OnePole lcL, lcR, hcL, hcR;
  dsp::Allpass1 diff[4];
  Plate() : pre(static_cast<size_t>(0.6 * kMaxRate), 0.f) {
    for (auto& l : line) l.assign(static_cast<size_t>(0.25 * kMaxRate), 0.f);
  }
  void clear() {
    for (auto& l : line) std::fill(l.begin(), l.end(), 0.f);
    std::fill(pre.begin(), pre.end(), 0.f);
    damp.fill(0);
  }
  void configure(double s) {
    sr = s;
    static const double base[kLines] = {0.0297, 0.0371, 0.0411, 0.0437, 0.0533, 0.0617, 0.0713, 0.0797};
    for (int k = 0; k < kLines; ++k)
      len[static_cast<size_t>(k)] = std::clamp(static_cast<int>(base[k] * (0.4 + 1.6 * size) * sr), 16,
                                               static_cast<int>(line[static_cast<size_t>(k)].size()) - 1);
    lcL.set(sr, lowCut);
    lcR.set(sr, lowCut);
    hcL.set(sr, highCut);
    hcR.set(sr, highCut);
    for (int k = 0; k < 4; ++k) diff[k].setFreq(sr, 300.0 + 900.0 * k);
  }
  void process(float* L, float* R, int n) {
    if (!on) return;
    const int pl = static_cast<int>(pre.size());
    const int pd = std::clamp(static_cast<int>(predelay * sr), 0, pl - 2);
    for (int i = 0; i < n; ++i) {
      const double inMono = 0.5 * (L[i] + R[i]);
      const double delayed = pre[static_cast<size_t>((prePos - pd + pl) % pl)];
      pre[static_cast<size_t>(prePos)] = static_cast<float>(inMono + preFb * delayed);
      prePos = (prePos + 1) % pl;
      double x = lcL.hp(delayed);
      for (int k = 0; k < 4; ++k) x = x + diffusion * (diff[k].process(x) - x);
      double outs[kLines];
      double sum = 0;
      for (int k = 0; k < kLines; ++k) {
        const auto& b = line[static_cast<size_t>(k)];
        const int l = len[static_cast<size_t>(k)];
        int rp = pos[static_cast<size_t>(k)] - l;
        if (rp < 0) rp += static_cast<int>(b.size());
        outs[k] = b[static_cast<size_t>(rp)];
        sum += outs[k];
      }
      const double hh = sum * (2.0 / kLines);  // Householder mix
      double oL = 0, oR = 0;
      for (int k = 0; k < kLines; ++k) {
        const int l = len[static_cast<size_t>(k)];
        const double g = std::pow(10.0, -3.0 * l / (std::max(0.1, decay) * sr));
        double y = (outs[k] - hh) * g;
        damp[static_cast<size_t>(k)] += (1.0 - dampAmt * 0.9) * (y - damp[static_cast<size_t>(k)]);
        y = damp[static_cast<size_t>(k)];
        auto& b = line[static_cast<size_t>(k)];
        b[static_cast<size_t>(pos[static_cast<size_t>(k)])] = static_cast<float>(y + x * 0.35);
        pos[static_cast<size_t>(k)] = (pos[static_cast<size_t>(k)] + 1) % static_cast<int>(b.size());
        (k & 1 ? oR : oL) += outs[k];
      }
      oL = hcL.lp(oL) * 0.5 + early * delayed * 0.5;
      oR = hcR.lp(oR) * 0.5 + early * delayed * 0.5;
      const double mid = 0.5 * (oL + oR), side = 0.5 * (oL - oR) * width;
      L[i] = static_cast<float>(L[i] * dry + (mid + side) * wet);
      R[i] = static_cast<float>(R[i] * dry + (mid - side) * wet);
    }
  }
};

// --------------------------------------------------------------- ensemble
struct Ensemble {
  std::vector<float> bl, br;
  int w = 0;
  double sr = 44100;
  bool on = false;
  double delayMs = 8, fb = 0, rate = 0.5, depth = 0.5, move = 0, moveRate = 0.5, mix = 0.5, phase = 0.5;
  int lines = 2;
  double ph = 0, drift[4] = {0, 0, 0, 0}, driftT[4] = {0, 0, 0, 0}, mph = 0;
  OnePole lpL, lpR, hpL, hpR;
  double lpHz = 18000, hpHz = 20;
  uint32_t rnd = 777;
  double fbL = 0, fbR = 0;
  Ensemble() : bl(static_cast<size_t>(0.1 * kMaxRate), 0.f), br(static_cast<size_t>(0.1 * kMaxRate), 0.f) {}
  void clear() {
    std::fill(bl.begin(), bl.end(), 0.f);
    std::fill(br.begin(), br.end(), 0.f);
    fbL = fbR = 0;
  }
  void process(float* L, float* R, int n) {
    if (!on) return;
    const int len = static_cast<int>(bl.size());
    lpL.set(sr, lpHz);
    lpR.set(sr, lpHz);
    hpL.set(sr, hpHz);
    hpR.set(sr, hpHz);
    for (int i = 0; i < n; ++i) {
      bl[static_cast<size_t>(w)] = static_cast<float>(L[i] + fb * fbL);
      br[static_cast<size_t>(w)] = static_cast<float>(R[i] + fb * fbR);
      ph += rate / sr;
      if (ph >= 1.0) ph -= 1.0;
      mph += moveRate / sr;
      if (mph >= 1.0) {
        mph -= 1.0;
        for (int k = 0; k < 4; ++k) {
          rnd = rnd * 1664525u + 1013904223u;
          driftT[k] = static_cast<double>(rnd >> 9) / 4194304.0 - 1.0;
        }
      }
      double wl = 0, wr = 0;
      for (int k = 0; k < lines; ++k) {
        drift[k] += 0.0005 * (driftT[k] - drift[k]);
        const double p = ph + phase * k / lines;
        const double m = std::sin(dsp::kTwoPi * p) * depth + drift[k] * move;
        const double d = std::clamp((delayMs + 4.0 * m) * 0.001 * sr, 2.0, static_cast<double>(len - 4));
        (k & 1 ? wr : wl) += readFrac(k & 1 ? br : bl, w, d);
      }
      wl = hpL.hp(lpL.lp(wl * 2.0 / lines));
      wr = hpR.hp(lpR.lp(wr * 2.0 / lines));
      fbL = wl;
      fbR = wr;
      L[i] = static_cast<float>(L[i] * (1.0 - mix) + wl * mix);
      R[i] = static_cast<float>(R[i] * (1.0 - mix) + wr * mix);
      w = (w + 1) % len;
    }
  }
};

}  // namespace gl::packs::synths::dcofx

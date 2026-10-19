// Gridline — "models" pack: runtime building blocks shared by the generators
// (sine table, effect block, parameter access). Audio-thread safe: everything
// is allocated in constructors.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include "../../engine/Dsp.h"
#include "../../engine/Generators.h"
#include "models.h"

namespace gl::packs::models {

std::unique_ptr<Generator> makeAdditive();
std::unique_ptr<Generator> makeHarmor();
// Resynth's prepared curves / images (UI thread).
std::shared_ptr<const AuxData> buildHarmorAux(const PluginState& s);
// Additive's custom timbre table (UI thread; null when the state has none).
std::shared_ptr<const AuxData> buildAdditiveAux(const PluginState& s);
std::unique_ptr<Generator> makeImageSynth();
std::unique_ptr<Generator> makeString();
std::unique_ptr<Generator> makeDrumKit();

inline float prmf(const PluginState& s, int i, float d = 0.f) {
  return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : d;
}
inline int prmi(const PluginState& s, int i, int d = 0) { return static_cast<int>(std::lround(prmf(s, i, static_cast<float>(d)))); }

// 4096-point sine table with a guard point, indexed by a 32-bit phase.
struct SineTable {
  static constexpr int kBits = 12;
  static constexpr int kSize = 1 << kBits;
  float t[kSize + 1];
  SineTable() {
    for (int i = 0; i <= kSize; ++i) t[i] = static_cast<float>(std::sin(dsp::kTwoPi * i / kSize));
  }
  // Linear interpolation from the top 12 bits, fraction from the next 16.
  inline float at(uint32_t ph) const {
    uint32_t i = ph >> (32 - kBits);
    float f = static_cast<float>((ph >> (32 - kBits - 16)) & 0xFFFF) * (1.0f / 65536.0f);
    return t[i] + (t[i + 1] - t[i]) * f;
  }
};
inline const SineTable& sineTable() {
  static const SineTable tab;
  return tab;
}
// Phase increment for a frequency (cycles per sample * 2^32).
inline uint32_t phaseInc(double hz, double sr) {
  double c = hz / sr;
  if (c <= 0 || c >= 0.5) return 0;
  return static_cast<uint32_t>(c * 4294967296.0);
}

// Simple LFO: sine, triangle, saw, square, sample & hold (0..1 phase).
inline float lfoShape(int shape, double ph, float held) {
  switch (shape) {
    case 1: return static_cast<float>(ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph);
    case 2: return static_cast<float>(2 * ph - 1);
    case 3: return ph < 0.5 ? 1.0f : -1.0f;
    case 4: return held;
    default: return static_cast<float>(std::sin(dsp::kTwoPi * ph));
  }
}

// Chorus + delay + reverb on a generator's own output (in place, adds wet).
class ModelFx {
 public:
  ModelFx() {
    const double maxSr = 192000.0, k = maxSr / 44100.0 * 1.6 + 1;
    chor_[0].assign(static_cast<size_t>(0.04 * maxSr) + 4, 0.0f);
    chor_[1] = chor_[0];
    del_[0].assign(static_cast<size_t>(2.0 * maxSr) + 4, 0.0f);
    del_[1] = del_[0];
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i) comb_[c][i].buf.assign(static_cast<size_t>(kComb[i] * k) + 32, 0.0f);
      for (int i = 0; i < 2; ++i) ap_[c][i].buf.assign(static_cast<size_t>(kAp[i] * k) + 32, 0.0f);
    }
  }
  void set(const PluginState& s, int base, double sr) {
    sr_ = sr;
    chorus_ = prmf(s, base + FxChorus, 0);
    delay_ = prmf(s, base + FxDelay, 0);
    delayTime_ = std::clamp(static_cast<double>(prmf(s, base + FxDelayTime, 0.3f)), 0.01, 1.9);
    fb_ = std::clamp(prmf(s, base + FxDelayFeedback, 0.35f), 0.0f, 0.95f);
    reverb_ = prmf(s, base + FxReverb, 0);
    size_ = prmf(s, base + FxRoomSize, 0.6f);
    resize();
  }
  // Same with explicit values (instruments with their own effect controls).
  void setDirect(double sr, float chorus, float delay, double delaySec, float feedback, float reverb, float room) {
    sr_ = sr;
    chorus_ = chorus;
    delay_ = delay;
    delayTime_ = std::clamp(delaySec, 0.01, 1.9);
    fb_ = std::clamp(feedback, 0.0f, 0.95f);
    reverb_ = reverb;
    size_ = std::clamp(room, 0.0f, 1.0f);
    resize();
  }
  void resize() {
    const float size = std::clamp(size_ + revSize_, 0.0f, 1.0f);
    double scale = sr_ / 44100.0 * (0.6 + size);
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i)
        comb_[c][i].len = std::max<size_t>(1, std::min(comb_[c][i].buf.size() - 1, static_cast<size_t>((kComb[i] + c * 23) * scale)));
      for (int i = 0; i < 2; ++i)
        ap_[c][i].len = std::max<size_t>(1, std::min(ap_[c][i].buf.size() - 1, static_cast<size_t>((kAp[i] + c * 13) * scale)));
    }
    roomFb_ = std::clamp(0.72f + 0.26f * size + revFb_, 0.5f, 0.985f);
  }
  // Effect characters (Additive): chorus kind 0..8, delay kind 0 straight / 1 ping-pong /
  // 2 wide, reverb kind 0..5. Defaults (0, 1, 0) are the original voicing.
  void setKinds(int chorus, int delay, int reverb, double tempo) {
    (void)tempo;
    static const float base[9] = {0.012f, 0.020f, 0.025f, 0.0025f, 0.0035f, 0.015f, 0.008f, 0.003f, 0.005f};
    static const float depth[9] = {0.004f, 0.006f, 0.010f, 0.0018f, 0.0028f, 0.0012f, 0.003f, 0.0022f, 0.0025f};
    static const float rate[9] = {0.55f, 0.35f, 0.25f, 0.2f, 0.12f, 0.1f, 1.8f, 0.18f, 5.5f};
    static const float fb[9] = {0, 0, 0, 0.6f, -0.6f, 0, 0, 0.55f, 0};
    const int c = std::clamp(chorus, 0, 8);
    chBase_ = base[c], chDepth_ = depth[c], chRate_ = rate[c], chFb_ = fb[c];
    chInvert_ = c == 7;
    chDry_ = c == 8 ? 0.0f : 1.0f;
    delayKind_ = std::clamp(delay, 0, 2);
    static const float revSize[6] = {0.0f, 0.25f, 0.4f, 0.1f, -0.3f, -0.45f};
    static const float revFb[6] = {0.0f, 0.04f, 0.07f, 0.0f, -0.12f, -0.2f};
    const int r = std::clamp(reverb, 0, 5);
    if (r != revKind_) {
      revKind_ = r;
      revSize_ = revSize[r];
      revFb_ = revFb[r];
      if (sr_ > 0) resize();
    }
  }
  bool enabled() const { return chorus_ > 0 || delay_ > 0 || reverb_ > 0; }
  // True while a tail may still be sounding after the input went silent.
  bool ringing() const { return enabled() && quiet_ < static_cast<int64_t>(sr_ * (0.3 + (delay_ > 0 ? delayTime_ * 8 : 0) + (reverb_ > 0 ? 3 : 0))); }
  void reset() {
    for (int c = 0; c < 2; ++c) {
      std::fill(chor_[c].begin(), chor_[c].end(), 0.0f);
      std::fill(del_[c].begin(), del_[c].end(), 0.0f);
      for (auto& cb : comb_[c]) std::fill(cb.buf.begin(), cb.buf.end(), 0.0f), cb.z = 0;
      for (auto& a : ap_[c]) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
    }
    quiet_ = 1 << 30;
  }
  void process(float* L, float* R, int n) {
    if (!enabled()) return;
    bool loud = false;
    const size_t cn = chor_[0].size(), dn = del_[0].size();
    const double dSamp = delayTime_ * sr_;
    for (int i = 0; i < n; ++i) {
      float x[2] = {L[i], R[i]};
      if (std::fabs(x[0]) + std::fabs(x[1]) > 1e-6f) loud = true;
      float y[2] = {x[0], x[1]};
      if (chorus_ > 0) {
        chPh_ += chRate_ / sr_;
        if (chPh_ >= 1) chPh_ -= 1;
        for (int c = 0; c < 2; ++c) {
          double d = (chBase_ + chDepth_ * std::sin(dsp::kTwoPi * (chPh_ + c * 0.25))) * sr_;
          double rp = static_cast<double>(cw_) - d;
          while (rp < 0) rp += static_cast<double>(cn);
          size_t i0 = static_cast<size_t>(rp) % cn, i1 = (i0 + 1) % cn;
          float f = static_cast<float>(rp - std::floor(rp));
          float w = chor_[c][i0] + (chor_[c][i1] - chor_[c][i0]) * f;
          chor_[c][cw_] = x[c] + w * chFb_;
          if (chInvert_ && c == 1) w = -w;
          y[c] = x[c] * (1 - (1 - chDry_ * 0.65f) * chorus_) + w * 0.7f * chorus_;
        }
        cw_ = (cw_ + 1) % cn;
      }
      float out[2] = {y[0], y[1]};
      if (delay_ > 0) {
        size_t rp = (dw_ + dn - static_cast<size_t>(dSamp)) % dn;
        size_t rp1 = delayKind_ == 2 ? (dw_ + dn - std::min(dn - 1, static_cast<size_t>(dSamp * 1.5))) % dn : rp;
        float d0 = del_[0][rp], d1 = del_[1][rp1];
        if (delayKind_ == 1) {
          dlp_[0] += (d1 - dlp_[0]) * 0.6f;  // cross-feed (ping-pong) with gentle damping
          dlp_[1] += (d0 - dlp_[1]) * 0.6f;
          del_[0][dw_] = y[0] * 0.7f + dlp_[0] * fb_;
          del_[1][dw_] = y[1] * 0.3f + dlp_[1] * fb_;
        } else {
          dlp_[0] += (d0 - dlp_[0]) * 0.6f;
          dlp_[1] += (d1 - dlp_[1]) * 0.6f;
          del_[0][dw_] = y[0] * 0.5f + dlp_[0] * fb_;
          del_[1][dw_] = y[1] * 0.5f + dlp_[1] * fb_;
        }
        dw_ = (dw_ + 1) % dn;
        out[0] += d0 * delay_;
        out[1] += d1 * delay_;
      }
      if (reverb_ > 0) {
        float in = (y[0] + y[1]) * 0.015f;
        for (int c = 0; c < 2; ++c) {
          float acc = 0;
          for (auto& cb : comb_[c]) {
            float v = cb.buf[cb.pos];
            cb.z = v * 0.7f + cb.z * 0.3f;
            cb.buf[cb.pos] = in + cb.z * roomFb_;
            if (++cb.pos >= cb.len) cb.pos = 0;
            acc += v;
          }
          for (auto& a : ap_[c]) {
            float b = a.buf[a.pos];
            a.buf[a.pos] = acc + b * 0.5f;
            acc = b - acc;
            if (++a.pos >= a.len) a.pos = 0;
          }
          out[c] += acc * reverb_;
        }
      }
      if (std::fabs(out[0] - x[0]) + std::fabs(out[1] - x[1]) > 1e-6f) loud = true;
      L[i] = out[0];
      R[i] = out[1];
    }
    quiet_ = loud ? 0 : quiet_ + n;
  }

 private:
  static constexpr int kComb[4] = {1123, 1201, 1289, 1373};
  static constexpr int kAp[2] = {563, 439};
  struct Comb {
    std::vector<float> buf;
    size_t len = 1, pos = 0;
    float z = 0;
  };
  struct Ap {
    std::vector<float> buf;
    size_t len = 1, pos = 0;
  };
  double sr_ = 0, chPh_ = 0, delayTime_ = 0.3;
  float size_ = 0.6f, revSize_ = 0, revFb_ = 0, chBase_ = 0.012f, chDepth_ = 0.004f, chRate_ = 0.55f, chFb_ = 0, chDry_ = 1;
  bool chInvert_ = false;
  int delayKind_ = 1, revKind_ = 0;
  float chorus_ = 0, delay_ = 0, fb_ = 0.35f, reverb_ = 0, roomFb_ = 0.85f, dlp_[2] = {0, 0};
  std::vector<float> chor_[2], del_[2];
  size_t cw_ = 0, dw_ = 0;
  Comb comb_[2][4];
  Ap ap_[2][2];
  int64_t quiet_ = 1 << 30;
};

}  // namespace gl::packs::models

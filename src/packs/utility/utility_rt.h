// Gridline — "utility" pack: runtime pieces shared by the DSP, the editors
// and the tests: a stereo history ring, loudness (ITU-R BS.1770 / EBU R128),
// true-peak detection, and the published state that visual plugins expose to
// their editors (audio thread writes atomics; the UI only reads, plus a few
// request flags it may set).
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "../../engine/Effects.h"
#include "utility.h"

namespace gl::packs::utility {

inline constexpr float kNoLoudness = -144.0f;  // "not measured yet"

// --------------------------------------------------------------- stereo ring

// Lock-free stereo history (audio thread writes, UI copies). Torn reads only
// affect what is drawn.
class StereoRing {
 public:
  static constexpr size_t kSize = 1u << 15;  // ~0.7 s at 48 kHz
  void write(const float* L, const float* R, int n) {
    uint64_t w = pos_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
      size_t k = static_cast<size_t>((w + static_cast<uint64_t>(i)) & (kSize - 1));
      l_[k] = L[i];
      r_[k] = R[i];
    }
    pos_.store(w + static_cast<uint64_t>(n), std::memory_order_release);
  }
  uint64_t written() const { return pos_.load(std::memory_order_acquire); }
  // Copies frames [end - n, end). Returns false if they are no longer (or not yet) held.
  bool readAt(uint64_t end, float* L, float* R, size_t n) const {
    uint64_t w = written();
    if (n > kSize || end > w || end < n || w - (end - n) > kSize) return false;
    for (size_t i = 0; i < n; ++i) {
      size_t k = static_cast<size_t>((end - n + i) & (kSize - 1));
      if (L) L[i] = l_[k];
      if (R) R[i] = r_[k];
    }
    return true;
  }
  // Newest n frames (zero-filled before anything was written).
  void readLatest(float* L, float* R, size_t n) const {
    uint64_t w = written();
    if (w < n) {
      size_t pad = static_cast<size_t>(n - w);
      if (L) std::memset(L, 0, pad * sizeof(float));
      if (R) std::memset(R, 0, pad * sizeof(float));
      if (w) readAt(w, L ? L + pad : nullptr, R ? R + pad : nullptr, static_cast<size_t>(w));
      return;
    }
    readAt(w, L, R, n);
  }

 private:
  std::array<float, kSize> l_{}, r_{};
  std::atomic<uint64_t> pos_{0};
};

// ------------------------------------------------------------------ loudness

// K-weighted loudness: momentary (400 ms), short-term (3 s), gated integrated
// and loudness range, updated every 100 ms. Fixed memory; realtime-safe.
class LoudnessMeter {
 public:
  void reset();
  void setSampleRate(double sr);
  void process(const float* L, const float* R, int n);
  float momentary() const { return momentary_; }
  float shortTerm() const { return shortTerm_; }
  float integrated() const { return integrated_; }
  float range() const { return lra_; }
  float maxMomentary() const { return maxMomentary_; }

 private:
  struct Bq {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) {
      double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  static constexpr int kBins = 1000;  // 0.1 LU bins from -70 LUFS
  void addSubBlock(double energy);
  void updateIntegrated();
  static float toLufs(double e) { return e > 1e-20 ? static_cast<float>(-0.691 + 10.0 * std::log10(e)) : kNoLoudness; }

  double sr_ = 0;
  Bq shelf_[2], hp_[2];
  int subLen_ = 4410, subPos_ = 0;
  double subSum_ = 0;
  double subs_[30] = {};
  int subCount_ = 0, subHead_ = 0;
  // Integrated: gating blocks (400 ms, 75% overlap) by loudness bin.
  uint32_t gCount_[kBins] = {};
  double gEnergy_[kBins] = {};
  // Loudness range: short-term values by loudness bin.
  uint32_t sCount_[kBins] = {};
  double sEnergy_[kBins] = {};
  float momentary_ = kNoLoudness, shortTerm_ = kNoLoudness, integrated_ = kNoLoudness, lra_ = 0, maxMomentary_ = kNoLoudness;
};

// ----------------------------------------------------------------- true peak

// 4x oversampled peak detector (polyphase windowed-sinc interpolator).
class TruePeak {
 public:
  static constexpr int kPhases = 4, kTaps = 12;  // taps per phase
  TruePeak();
  void reset() {
    std::memset(hist_, 0, sizeof hist_);
    pos_ = 0;
  }
  // Returns the largest |x| of the oversampled block.
  float process(const float* x, int n);

 private:
  float h_[kPhases][kTaps];
  float hist_[kTaps * 2] = {};
  int pos_ = 0;
};

// ------------------------------------------------------------ published state

// Levels a metering plugin publishes for its editor. "Since read" values are
// maxima the UI consumes with exchange(0).
struct LevelState {
  std::atomic<float> peakL{0}, peakR{0};          // max |x| since read
  std::atomic<float> truePeakL{0}, truePeakR{0};  // max true peak since read
  std::atomic<float> rmsL{0}, rmsR{0};            // 300 ms RMS (linear)
  std::atomic<float> vuL{0}, vuR{0};              // VU ballistics (linear RMS-like)
  std::atomic<float> maxPeak{0}, maxTruePeak{0};  // since reset
  std::atomic<float> correlation{0};              // -1..1 (300 ms)
  std::atomic<float> momentary{kNoLoudness}, shortTerm{kNoLoudness}, integrated{kNoLoudness}, range{0}, maxMomentary{kNoLoudness};
  std::atomic<bool> resetRequest{false};          // UI asks the audio thread to clear integrations
};

// Transport as seen by the plugin (for the clock).
struct TransportState {
  std::atomic<double> tick{0}, tempo{140}, sampleRate{44100};
  std::atomic<int> ppq{96};
  std::atomic<bool> playing{false};
  std::atomic<int64_t> playedSamples{0};  // samples since the transport last started
};

// Per-band input energy of the Morph EQ, captured while the editor's analyse
// button is held (UI sets capture / reset; the audio thread accumulates).
struct BandTelemetry {
  std::atomic<bool> capture{false}, reset{false};
  std::atomic<float> energy[kBqBands] = {};  // sum of squared band outputs (mid)
  std::atomic<int64_t> frames{0};
};

// Base of every utility effect: lets the editors reach the live data of the
// instance in a mixer slot (EffectRuntime::fx) with a dynamic_cast.
class UtilityFx : public Effect {
 public:
  virtual const StereoRing* ring() const { return nullptr; }
  virtual LevelState* levels() { return nullptr; }
  virtual const TransportState* transport() const { return nullptr; }
  virtual BandTelemetry* bandTelemetry() { return nullptr; }
};

// Morph EQ runtime (utility_bankeq_dsp.cpp).
std::unique_ptr<Effect> createBankEq();

}  // namespace gl::packs::utility

// Gridline — "synths" pack, J-22 group "va": note handling shared by Twin VA
// and Saw Swarm — held-note list, arpeggiator and the reference control laws
// (normalized knob position → time / frequency / ratio).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "../../engine/Dsp.h"

namespace gl::packs::synths::va {

// ------------------------------------------------------------ control laws
// Envelope times: exponential in the knob position, 1 ms .. `maxS` seconds.
inline double expTime(double v, double minS, double maxS) {
  v = std::clamp(v, 0.0, 1.0);
  return minS * std::pow(maxS / minS, v);
}
// Filter cutoff 0..1 → 16 Hz .. 20 kHz (ten octaves, exponential).
inline double cutoffHz(double v) { return 16.0 * std::pow(1250.0, std::clamp(v, 0.0, 1.0)); }
// LFO rate 0..1 → 0.02 .. 40 Hz.
inline double lfoRateHz(double v) { return 0.02 * std::pow(2000.0, std::clamp(v, 0.0, 1.0)); }

// ----------------------------------------------------------- held notes
struct Held {
  struct N {
    uint32_t id;
    double pitch;
    float vel;
  };
  std::array<N, 32> n{};
  int count = 0;
  void add(uint32_t id, double p, float v) {
    remove(id);
    if (count == static_cast<int>(n.size())) {
      for (int i = 1; i < count; ++i) n[static_cast<size_t>(i - 1)] = n[static_cast<size_t>(i)];
      --count;
    }
    n[static_cast<size_t>(count++)] = {id, p, v};
  }
  bool remove(uint32_t id) {
    for (int i = 0; i < count; ++i)
      if (n[static_cast<size_t>(i)].id == id) {
        for (int j = i + 1; j < count; ++j) n[static_cast<size_t>(j - 1)] = n[static_cast<size_t>(j)];
        --count;
        return true;
      }
    return false;
  }
  const N* last() const { return count ? &n[static_cast<size_t>(count - 1)] : nullptr; }
};

// ------------------------------------------------------------ arpeggiator
// Plays the held notes (sorted by pitch) over `octaves` octaves; one step per
// `stepBeats` beats; mode 0 up, 1 down, 2 up-down, 3 random. Steps are emitted
// as (pitch, velocity) through stepNote()/advance().
struct Arp {
  int stepIdx = 0, dir = 1;
  double phase = 1.0;  // fraction of the current step done (1 = next step due)
  uint32_t rnd = 0x9E3779B9u;
  bool playing = false;
  double curPitch = 0;
  float curVel = 0;

  void reset() {
    stepIdx = 0;
    dir = 1;
    phase = 1.0;
    playing = false;
  }
  // Sorted pitch list of `h` expanded over octaves into `out`; returns length.
  static int build(const Held& h, int octaves, std::array<double, 128>& out, std::array<float, 128>& vel) {
    std::array<Held::N, 32> s{};
    int c = h.count;
    for (int i = 0; i < c; ++i) s[static_cast<size_t>(i)] = h.n[static_cast<size_t>(i)];
    std::sort(s.begin(), s.begin() + c, [](const Held::N& a, const Held::N& b) { return a.pitch < b.pitch; });
    int k = 0;
    for (int o = 0; o < std::max(1, octaves); ++o)
      for (int i = 0; i < c && k < 128; ++i) {
        out[static_cast<size_t>(k)] = s[static_cast<size_t>(i)].pitch + 12.0 * o;
        vel[static_cast<size_t>(k++)] = s[static_cast<size_t>(i)].vel;
      }
    return k;
  }
  // Picks the next step's note. Returns false when nothing is held.
  bool next(const Held& h, int mode, int octaves) {
    std::array<double, 128> p{};
    std::array<float, 128> v{};
    const int len = build(h, octaves, p, v);
    if (len == 0) return false;
    int i = 0;
    switch (mode) {
      case 1:  // down
        i = len - 1 - (stepIdx % len);
        ++stepIdx;
        break;
      case 2: {  // up-down (ends not repeated)
        const int period = std::max(1, 2 * len - 2);
        const int s = stepIdx % period;
        i = s < len ? s : period - s;
        ++stepIdx;
        break;
      }
      case 3:  // random
        rnd ^= rnd << 13;
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        i = static_cast<int>(rnd % static_cast<uint32_t>(len));
        break;
      default:  // up
        i = stepIdx % len;
        ++stepIdx;
        break;
    }
    curPitch = p[static_cast<size_t>(i)];
    curVel = v[static_cast<size_t>(i)];
    return true;
  }
};

}  // namespace gl::packs::synths::va

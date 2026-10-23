// Gridline — J-26: the classic tempo delay (gl.delay DlyModel = 2; studies
// behaviour study notes). A stereo line whose
// write slot is D samples ahead of the read slot: the (matrixed) input is added
// into the write slot (one side optionally O samples later), each read slot
// is emptied, fed back (straight or crossed) into the write slot and the slot
// then goes through a one-pole low-pass, so the first echo is filtered too.
// Outputs: straight, swapped or ping-pong; dry adds on top.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl::cdelay {

// Volume law of the classic controls: 1 at 128, else (11^(v/128) - 1)/10.
inline double volume(double raw) { return raw == 128 ? 1.0 : (std::pow(11.0, raw / 128.0) - 1) / 10.0; }
// Feedback filter coefficient (raw 0..1 position n): (21^n - 1) / 20.
inline double coefOf(double n) { return (std::pow(21.0, std::clamp(n, 0.0, 1.0)) - 1) * 0.05; }
// Offset in ms of the signed raw value (-512..512).
inline double offsetMs(double raw) { return (std::pow(1001.0, std::fabs(std::clamp(raw, -512.0, 512.0)) / 512.0) - 1) * 0.5; }

struct Settings {
  double ticks = 144;      // 1/192 beat
  bool stepRound = false;  // length = (ticks / 48) · round(sr · 15 / bpm) (whole sixteenths rounded first)
  double bpm = 0;          // 0 = host tempo
  double coef = 1;         // feedback filter coefficient (at 44.1 kHz when scaled)
  bool scaleSr = false;    // coefficient × 44100 / sr (capped at 1)
  double feedback = 0.5;   // linear
  int mode = 0;            // 0 normal, 1 swapped outputs, 2 ping-pong (crossed feedback + swapped outputs)
  double offsetMs = 0;     // < 0: left input later, > 0: right
  double inLL = 1, inRR = 1, inRL = 0, inLR = 0;  // input matrix (R→L, L→R)
  double dry = 1;
};

class Line {
 public:
  void init(double sr) {
    sr_ = sr;
    size_t n = static_cast<size_t>(sr * 12.0) + 16;  // 768 ticks down to 20 BPM, + 500 ms offset
    buf_[0].assign(n, 0.f), buf_[1].assign(n, 0.f);
    read_ = 0, s_[0] = s_[1] = 0;
  }
  void reset() {
    for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.f);
    read_ = 0, s_[0] = s_[1] = 0;
  }
  bool ready() const { return sr_ > 0; }
  size_t delaySamples(const Settings& s, double hostTempo) const {
    const double bpm = s.bpm > 0 ? s.bpm : std::max(1.0, hostTempo);
    double d = s.stepRound ? (s.ticks / 48.0) * std::nearbyint(sr_ * 15.0 / bpm) : std::nearbyint(sr_ * s.ticks * 0.3125 / bpm);
    return static_cast<size_t>(std::clamp(d, 1.0, static_cast<double>(buf_[0].size() / 2)));
  }
  void process(const Settings& s, double hostTempo, float* L, float* R, int n) {
    const size_t N = buf_[0].size();
    const size_t D = delaySamples(s, hostTempo);
    const size_t O = static_cast<size_t>(std::min(std::nearbyint(sr_ * std::fabs(s.offsetMs) / 1000.0), static_cast<double>(N / 2 - 2)));
    const size_t oL = s.offsetMs < 0 ? O : 0, oR = s.offsetMs > 0 ? O : 0;
    const double a = s.scaleSr ? std::min(1.0, s.coef * 44100.0 / sr_) : s.coef;
    const bool cross = s.mode == 2, swap = s.mode != 0;
    for (int i = 0; i < n; ++i) {
      const double xL = L[i], xR = R[i];
      const size_t w = (read_ + D) % N;
      buf_[0][(w + oL) % N] += static_cast<float>(s.inLL * xL + s.inRL * xR);
      buf_[1][(w + oR) % N] += static_cast<float>(s.inRR * xR + s.inLR * xL);
      const double rL = buf_[0][read_], rR = buf_[1][read_];
      buf_[0][read_] = buf_[1][read_] = 0;
      const double srcL = cross ? rR : rL, srcR = cross ? rL : rR;
      double slot[2] = {buf_[0][w] + s.feedback * srcL, buf_[1][w] + s.feedback * srcR};
      for (int c = 0; c < 2; ++c) {
        s_[c] += a * (slot[c] - s_[c]);
        buf_[c][w] = std::fabs(s_[c]) < 5.96e-8 ? 0.f : static_cast<float>(s_[c]);
      }
      L[i] = static_cast<float>((swap ? rR : rL) + s.dry * xL);
      R[i] = static_cast<float>((swap ? rL : rR) + s.dry * xR);
      read_ = (read_ + 1) % N;
    }
  }

 private:
  double sr_ = 0;
  std::vector<float> buf_[2];
  size_t read_ = 0;
  double s_[2] = {};
};

}  // namespace gl::cdelay

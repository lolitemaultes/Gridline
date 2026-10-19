// Gridline — "edison" pack: helpers shared by the pack's DSP files.
#pragma once
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>
#include "edison.h"

namespace gl::packs::edison {

// Mono audio keeps R equal to L.
void fixMono(Audio& a);
// Same markers / regions / loop with new audio (clipped to its length).
Content withAudio(const Content& c, Audio a);

using cplx = std::complex<double>;

// Radix-2 complex FFT of a fixed power-of-two size.
class Fft {
 public:
  explicit Fft(int n);
  int size() const { return n_; }
  void forward(std::vector<cplx>& a) const { run(a, false); }
  void inverse(std::vector<cplx>& a) const { run(a, true); }  // scaled by 1/n

 private:
  void run(std::vector<cplx>& a, bool inv) const;
  int n_;
  std::vector<cplx> tw_;
  std::vector<int> rev_;
};

inline int nextPow2(int64_t v) {
  int n = 1;
  while (n < v && n < (1 << 30)) n <<= 1;
  return n;
}

// Periodic Hann window (sums to a constant at 50 % / 75 % overlap).
std::vector<double> hannWindow(int n);

// Short-time Fourier processing of one channel: frames of `fft` samples every
// `hop`, Hann analysis and synthesis windows, overlap-add normalised so an
// unmodified spectrum reproduces the input. `fn(frameIndex, spectrum)` edits
// the full spectrum (bins 0..fft-1; keep it Hermitian). Frame k is centred on
// sample k * hop.
template <class F>
std::vector<float> stftProcess(const float* x, int64_t n, int fft, int hop, F fn) {
  const Fft F_(fft);
  const std::vector<double> w = hannWindow(fft);
  std::vector<double> acc(static_cast<size_t>(n + fft), 0.0), norm(static_cast<size_t>(n + fft), 0.0);
  std::vector<cplx> buf(static_cast<size_t>(fft));
  const int half = fft / 2;
  const int64_t frames = n / hop + 2;
  for (int64_t k = 0; k < frames; ++k) {
    const int64_t c0 = k * hop - half;  // first sample of the frame
    for (int i = 0; i < fft; ++i) {
      int64_t s = c0 + i;
      double v = s >= 0 && s < n ? x[s] : 0.0;
      buf[static_cast<size_t>(i)] = cplx(v * w[static_cast<size_t>(i)], 0.0);
    }
    F_.forward(buf);
    fn(k, buf);
    F_.inverse(buf);
    for (int i = 0; i < fft; ++i) {
      int64_t s = c0 + i;
      if (s < 0 || s >= n) continue;
      acc[static_cast<size_t>(s)] += buf[static_cast<size_t>(i)].real() * w[static_cast<size_t>(i)];
      norm[static_cast<size_t>(s)] += w[static_cast<size_t>(i)] * w[static_cast<size_t>(i)];
    }
  }
  std::vector<float> out(static_cast<size_t>(n));
  for (int64_t s = 0; s < n; ++s) out[static_cast<size_t>(s)] = static_cast<float>(norm[static_cast<size_t>(s)] > 1e-9 ? acc[static_cast<size_t>(s)] / norm[static_cast<size_t>(s)] : 0.0);
  return out;
}

// Linear-phase FIR from a magnitude response (gain per bin of an `n`-point
// grid, 0..n/2), Kaiser-windowed; length n - 1 (odd), delay (n - 2) / 2.
std::vector<double> firFromMagnitude(const std::vector<double>& mag, int n);
// Convolution of x with a short kernel (FFT overlap-add); output len = n + k - 1.
std::vector<double> convolveReal(const float* x, int64_t n, const std::vector<double>& k);

// K-weighting filter (BS.1770) for a sample rate: two biquads.
struct KWeight {
  double b[2][3], a[2][3];
  double z[2][2] = {{0, 0}, {0, 0}};
  explicit KWeight(double rate);
  double run(double x);
};

}  // namespace gl::packs::edison

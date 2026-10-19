// Gridline — "spacefx" pack: 2× half-band FIR up/down sampler (47 taps,
// Kaiser window), cascadable for 4× / 8×. Allocation-free; coefficients are
// computed by init() (constructor / UI thread).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gl::packs::spacefx::detail {

struct HalfbandCoefs {
  static constexpr int kTaps = 24;  // non-centre taps (even full-rate indices)
  double h[kTaps] = {};
  void init() {
    auto bessel0 = [](double x) {
      double s = 1, t = 1;
      for (int k = 1; k < 30; ++k) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
      }
      return s;
    };
    const double beta = 8.0, pi = 3.141592653589793;
    double sum = 0;
    for (int i = 0; i < kTaps; ++i) {
      double m = 2.0 * i - 23.0;  // offset from the centre (odd)
      double r = m / 24.0;
      double w = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(beta);
      h[i] = std::sin(pi * m / 2.0) / (pi * m) * w;
      sum += h[i];
    }
    for (double& v : h) v *= 0.5 / sum;
  }
};

// One channel, one 2× stage.
struct Halfband2x {
  const HalfbandCoefs* c = nullptr;
  double ux[HalfbandCoefs::kTaps] = {};  // upsampler input history (newest first)
  double de[12] = {}, dout[HalfbandCoefs::kTaps] = {};  // downsampler even / odd streams
  void clear() {
    std::memset(ux, 0, sizeof ux);
    std::memset(de, 0, sizeof de);
    std::memset(dout, 0, sizeof dout);
  }
  void up(double x, double& y0, double& y1) {
    std::memmove(ux + 1, ux, sizeof(double) * (HalfbandCoefs::kTaps - 1));
    ux[0] = x;
    double a = 0;
    for (int i = 0; i < HalfbandCoefs::kTaps; ++i) a += c->h[i] * ux[i];
    y0 = 2.0 * a;
    y1 = ux[11];
  }
  double down(double z0, double z1) {
    std::memmove(de + 1, de, sizeof(double) * 11);
    de[0] = z0;
    std::memmove(dout + 1, dout, sizeof(double) * (HalfbandCoefs::kTaps - 1));
    dout[0] = z1;
    double a = 0.5 * de[11];
    for (int i = 0; i < HalfbandCoefs::kTaps; ++i) a += c->h[i] * dout[i];
    return a;
  }
};

}  // namespace gl::packs::spacefx::detail

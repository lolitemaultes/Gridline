// K-U: the Plasma distortion module's static shapers: finite everywhere, silent at x = 0, the right
// symmetry, no jumps in the input or the drive, and the levels each shape stands for.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include "../Check.h"

namespace gl::packs::plasma {
float fxDistTable(int mode, float drive, float x);
}
using gl::packs::plasma::fxDistTable;

namespace {
constexpr int kModes[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 13, 14, 15, 17};
constexpr int kOddModes[] = {0, 1, 2, 3, 5, 6, 7, 13, 14, 15, 17};
constexpr float kDrives[] = {0.f, 0.03f, 0.1f, 0.125f, 0.17f, 0.25f, 0.5f, 0.77f, 0.95f, 1.f};
constexpr float kCeiling = 0.8286f;  // the clipping modes' top, the output half included

std::string named(const char* what, int mode) { return std::string(what) + " (mode " + std::to_string(mode) + ")"; }

// Narrows [a, b] to the half that changes most: a continuous curve's change shrinks with the interval, a
// jump's does not. Returns the change left when the interval is down to `eps`.
template <class F>
double jumpAcross(const F& f, double a, double b, double eps) {
  double fa = f(a), fb = f(b);
  while (b - a > eps) {
    const double m = 0.5 * (a + b), fm = f(m);
    if (std::fabs(fm - fa) >= std::fabs(fb - fm)) b = m, fb = fm;
    else a = m, fa = fm;
  }
  return std::fabs(fb - fa);
}
}  // namespace

int main() {
  const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();

  // finite for any input and any drive
  for (int m : kModes) {
    bool finite = true;
    for (float d : {-1.f, 0.f, 0.001f, 0.0999f, 0.1f, 0.3f, 0.5f, 0.999f, 1.f, 7.f, inf, -inf, nan})
      for (float x : {0.f, 1e-30f, 1e-6f, 0.01f, 1.f, 1.6572f, 10.5f, 11.f, 100.f, 1e4f, 1e6f, 1e20f, 3e38f, inf, nan}) {
        finite = finite && std::isfinite(fxDistTable(m, d, x)) && std::isfinite(fxDistTable(m, d, -x));
      }
    for (float d : kDrives)
      for (int i = -40000; i <= 40000; ++i) finite = finite && std::isfinite(fxDistTable(m, d, static_cast<float>(i) * 0.0005f));
    expect(finite, named("finite over a wide range of input and drive", m).c_str());
  }

  // silent at x = 0
  for (int m : kModes) {
    bool zero = true;
    for (int i = 0; i <= 100; ++i) zero = zero && std::fabs(fxDistTable(m, static_cast<float>(i) * 0.01f, 0.f)) < 1e-6f;
    expect(zero, named("no output at zero input at any drive", m).c_str());
  }

  // symmetry
  for (int m : kOddModes) {
    bool odd = true;
    for (float d : kDrives)
      for (int i = 1; i <= 3000; ++i) {
        const float x = static_cast<float>(i) * 0.004f;
        odd = odd && std::fabs(fxDistTable(m, d, x) + fxDistTable(m, d, -x)) < 1e-6f;
      }
    expect(odd, named("odd symmetry", m).c_str());
  }
  expect(fxDistTable(4, 1.f, 5.f) > 0.4f && fxDistTable(4, 1.f, -5.f) < -0.5f, "diode 2 settles at a different height on each side");
  expect(std::fabs(fxDistTable(9, 0.f, 5.f)) < std::fabs(fxDistTable(9, 0.f, -5.f)), "asymmetric droops on the positive side only");
  expect(fxDistTable(10, 0.5f, 0.5f) < 0.f && fxDistTable(10, 0.5f, 0.5f) == fxDistTable(10, 0.5f, -0.5f), "rectify is even and negative");

  // no jumps in the input
  for (int m : kModes) {
    double worst = 0;
    for (float d : kDrives) {
      const auto curve = [&](double x) { return static_cast<double>(fxDistTable(m, d, static_cast<float>(x))); };
      for (int i = -3000; i < 3000; ++i) {
        const double a = i * 0.004, b = (i + 1) * 0.004;
        if (std::fabs(curve(b) - curve(a)) > 0.01) worst = std::max(worst, jumpAcross(curve, a, b, 4e-6));
      }
    }
    expect(worst < 0.02, named("continuous in the input", m).c_str());
  }

  // no jumps in the drive
  for (int m : kModes) {
    double worst = 0;
    for (float x : {0.001f, 0.01f, 0.1f, 0.5f, 1.f, 1.6572f, 2.5f, 4.f, 7.f, 10.5f, 20.f}) {
      for (float s : {1.f, -1.f}) {
        const auto curve = [&](double d) { return static_cast<double>(fxDistTable(m, static_cast<float>(d), s * x)); };
        for (int i = 0; i < 1000; ++i) {
          const double a = i * 0.001, b = (i + 1) * 0.001;
          if (std::fabs(curve(b) - curve(a)) > 0.01) worst = std::max(worst, jumpAcross(curve, a, b, 1e-6));
        }
      }
    }
    expect(worst < 0.02, named("continuous in the drive", m).c_str());
  }

  // what each shape stands for
  for (int m : {0, 2, 3, 14, 15}) expect(std::fabs(fxDistTable(m, 0.f, 0.5f) - 0.25f) < 1e-4f, named("straight and halved at drive 0, small input", m).c_str());
  for (int m : {0, 2, 17}) expect(std::fabs(fxDistTable(m, 0.6f, 50.f) - kCeiling) < 1e-3f, named("stops at the ceiling", m).c_str());
  expect(std::fabs(fxDistTable(4, 0.f, 1.f)) < 1e-6f, "diode 2 is silent at drive 0");
  expect(std::fabs(fxDistTable(4, 0.05f, 10.f) - 0.5f * fxDistTable(4, 0.1f, 10.f)) < 1e-5f, "diode 2 rises in level over the first tenth");
  expect(std::fabs(fxDistTable(2, 1.f, 0.1f) - 0.5f * 4.98f * 0.1f) < 1e-3f, "hard clip gains about 14 dB at full drive");
  expect(fxDistTable(1, 1.f, 100.f) > kCeiling * 1.4f, "soft clip keeps rising past the ceiling of the hard modes");
  expect(fxDistTable(7, 1.f, 0.05f) > 0.7f, "zero-square jumps close to full level at full drive");
  expect(fxDistTable(14, 0.5f, 1.6572f) == fxDistTable(14, 1.f, 1.6572f), "stomp box's lines all cross at full scale");
  for (int m : {5, 6, 13}) {
    bool bounded = true;
    for (float d : {0.f, 0.3f, 0.75f, 1.f})
      for (int i = -20000; i <= 20000; ++i) bounded = bounded && std::fabs(fxDistTable(m, d, static_cast<float>(i) * 0.002f)) < kCeiling * 1.02f;
    for (float x : {50.f, 1e3f, 1e6f, 1e20f}) bounded = bounded && std::fabs(fxDistTable(m, 1.f, x)) < kCeiling * 1.02f;
    expect(bounded, named("folds stay within the ceiling at any input", m).c_str());
  }
  // a fold really folds: back through zero past full scale at drive 0
  expect(fxDistTable(5, 0.f, 1.f) > 0.45f && fxDistTable(5, 0.f, 1.6572f) > 0.8f && fxDistTable(5, 0.f, 3.5f) < fxDistTable(5, 0.f, 1.6572f) - 0.5f, "linear fold turns back past full scale");
  for (int m : {8, 11, 12, 16}) expect(std::fabs(fxDistTable(m, 0.5f, 0.7f) - 0.35f) < 1e-6f, named("modes shaped elsewhere pass through halved", m).c_str());

  return finish();
}

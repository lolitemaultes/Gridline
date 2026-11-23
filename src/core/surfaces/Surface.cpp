// Gridline - control surface profiles: shared LED bookkeeping and colour matching (Surface.h).
#include "Surface.h"
#include <algorithm>
#include <cmath>

namespace gl::surfaces {

namespace {
struct Lab {
  double l, a, b;
};
double lin(double c) {
  c /= 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
// CIE L*a*b* (D65): close distances look alike, so matching keeps hue and lightness apart.
Lab lab(Rgb c) {
  const double r = lin((c >> 16) & 0xFF), g = lin((c >> 8) & 0xFF), b = lin(c & 0xFF);
  const double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
  const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  const double z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
  auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
  const double fx = f(x), fy = f(y), fz = f(z);
  return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}
}  // namespace

int nearestColour(const std::array<Rgb, 128>& palette, Rgb c, int first) {
  const Lab t = lab(c);
  int best = std::clamp(first, 0, 127);
  double bestD = 1e18;
  for (int i = std::clamp(first, 0, 127); i < 128; ++i) {
    const Lab p = lab(palette[static_cast<size_t>(i)]);
    // Hue and saturation weigh more than lightness: an LED's brightness reads differently from a
    // screen's anyway, its colour must not.
    const double dl = (t.l - p.l) * 0.7, da = t.a - p.a, db = t.b - p.b;
    const double d = dl * dl + da * da + db * db;
    if (d < bestD) bestD = d, best = i;
  }
  return best;
}

Rgb dim(Rgb c, float f) {
  f = std::clamp(f, 0.0f, 1.0f);
  auto ch = [&](int s) { return static_cast<Rgb>(std::lround(static_cast<float>((c >> s) & 0xFF) * f)) << s; };
  return ch(16) | ch(8) | ch(0);
}

std::vector<int> LedCache::take(int max) {
  std::vector<int> out;
  for (size_t i = 0; i < 128 && static_cast<int>(out.size()) < max; ++i)
    if (used_[i] && (!known_[i] || sent_[i] != want_[i])) {
      known_[i] = true;
      sent_[i] = want_[i];
      out.push_back(static_cast<int>(i));
    }
  return out;
}

int LedCache::pending() const {
  int n = 0;
  for (size_t i = 0; i < 128; ++i) n += (used_[i] && (!known_[i] || sent_[i] != want_[i])) ? 1 : 0;
  return n;
}

}  // namespace gl::surfaces

// Gridline: "edison" pack: the spectrogram image (edison_specimage.h), shared by Loupe's
// spectrogram view and the audio clip editor's spectrum.
#include "edison_specimage.h"
#include <SDL_opengl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>
#include "../../app/GlEpoch.h"
#include "../../app/UiWake.h"
#include "../../core/ThreadName.h"

namespace gl::packs::edison {

namespace {

struct Lab {
  double L, a, b;
};
double toLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double toSrgb(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }
Lab oklab(uint32_t rgb) {
  const double r = toLinear(((rgb >> 16) & 255) / 255.0), g = toLinear(((rgb >> 8) & 255) / 255.0), b = toLinear((rgb & 255) / 255.0);
  const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
          0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}
ImU32 fromLab(const Lab& c, int alpha) {
  const double l = std::pow(c.L + 0.3963377774 * c.a + 0.2158037573 * c.b, 3), m = std::pow(c.L - 0.1055613458 * c.a - 0.0638541728 * c.b, 3),
               s = std::pow(c.L - 0.0894841775 * c.a - 1.2914855480 * c.b, 3);
  const double r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
               b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
  auto q = [](double v) { return static_cast<int>(std::lround(std::clamp(toSrgb(std::clamp(v, 0.0, 1.0)), 0.0, 1.0) * 255.0)); };
  return IM_COL32(q(r), q(g), q(b), alpha);
}

}  // namespace

SpecRamp makeSpecRamp(const std::pair<double, uint32_t>* stops, size_t n, double fadeTo) {
  SpecRamp t{};
  for (int i = 0; i < kSpecLut; ++i) {
    const double x = static_cast<double>(i) / (kSpecLut - 1);
    size_t k = 1;
    while (k + 1 < n && x > stops[k].first) ++k;
    const double u = std::clamp((x - stops[k - 1].first) / std::max(1e-9, stops[k].first - stops[k - 1].first), 0.0, 1.0);
    const Lab A = oklab(stops[k - 1].second), B = oklab(stops[k].second);
    int alpha = 255;
    if (fadeTo > 0) {
      const double f = std::clamp(x / fadeTo, 0.0, 1.0);
      alpha = static_cast<int>(std::lround(255.0 * f * f * (3 - 2 * f)));
    }
    t[static_cast<size_t>(i)] = fromLab({A.L + (B.L - A.L) * u, A.a + (B.a - A.a) * u, A.b + (B.b - A.b) * u}, alpha);
  }
  return t;
}

const SpecRamp& loupeSpecRamp() {
  // The floor is Loupe's lanes' own background (col::WaveBg); lightness rises steadily to white.
  static const SpecRamp lut = [] {
    const std::pair<double, uint32_t> stops[] = {{0.00, 0x1C2124}, {0.16, 0x1D3237}, {0.36, 0x1E5A4B}, {0.55, 0x3E9466},
                                                 {0.71, 0x79C48C}, {0.85, 0xE8C46A}, {0.95, 0xFBE6B4}, {1.00, 0xFFFFFF}};
    return makeSpecRamp(stops, std::size(stops));
  }();
  return lut;
}

void specUpload(SpecLayer& L, int range, const SpecRamp& lut, int look) {
  if (L.tex && L.texEpoch != glepoch::current()) L.tex = 0, L.texW = L.texH = 0;  // K-47: a driver reset took it
  if (L.img.empty()) return;
  const int key = range + look * 1000;
  if (!L.dirty && L.texLook == key && L.tex) return;
  L.dirty = false;
  L.texLook = key;
  ++L.uploads;
  const int W = L.img.req.cols, H = L.img.req.rows;
  std::vector<uint32_t> px(static_cast<size_t>(W) * static_cast<size_t>(H));
  const float lo = -static_cast<float>(range), k = (kSpecLut - 1) / static_cast<float>(range);
  for (size_t i = 0; i < px.size(); ++i) {
    const float t = (L.img.db[i] - lo) * k;
    px[i] = lut[static_cast<size_t>(std::clamp(static_cast<int>(t + 0.5f), 0, kSpecLut - 1))];
  }
  GLint last = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
  if (!L.tex) {
    GLuint t = 0;
    glGenTextures(1, &t);
    L.tex = t;
    L.texEpoch = glepoch::current();
    L.texW = L.texH = 0;
  }
  glBindTexture(GL_TEXTURE_2D, L.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  if (L.texW == W && L.texH == H)
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
  L.texW = W, L.texH = H;
}

void specStep(SpecLayer& L, const SpecViewRequest& want, uint64_t serial, bool start, const SpecMaker& make, const char* threadName) {
  if (L.job.valid() && L.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    SpecView v = L.job.get();
    if (!v.empty()) {
      L.img = std::move(v);
      L.imgSerial = L.askedSerial;
      L.dirty = true;
    } else {
      L.askedSerial = ~0ull;  // stopped early: ask again
    }
  }
  const bool fresh = L.askedSerial == serial && L.asked == want;
  if (L.job.valid()) {
    if (!fresh && L.cancel) L.cancel->store(true);  // it is out of date: stop it, the next frame asks again
    return;
  }
  if (fresh || !start || !make || want.cols <= 0 || want.rows <= 1) return;
  L.asked = want;
  L.askedSerial = serial;
  L.cancel = std::make_shared<std::atomic<bool>>(false);
  ++L.made;
  auto cancel = L.cancel;
  L.job = std::async(std::launch::async, [make, want, cancel, threadName]() {
    gl::threads::setName(threadName);
    SpecView v = make(want, cancel.get());
    uiwake::poke();  // the interface takes its next turn now (it may be idle)
    return v;
  });
}

}  // namespace gl::packs::edison

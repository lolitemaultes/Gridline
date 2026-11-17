// K-112D (R09-03): one law for a mixer fader's cap and the dB scale beside it. The scale's mark for a
// level sits on the very pixel row the cap's centre is drawn on when the fader is at that level, in the
// Classic drawing and in every control style; 0 dB is the fader's unity value (0.8), and every mark
// reads back as its own level through the engine's fader law.
#include <cmath>
#include <cstdio>
#include "Check.h"
#include "app/LookGeom.h"
#include "core/Params.h"
#include "core/Song.h"
#include "engine/Dsp.h"

using namespace gl;

namespace {

constexpr double kUnity = 0.8;
constexpr int kMarks[] = {6, 3, 0, -3, -6, -10, -15, -20, -30, -40, -60};

// What the strips do (LayoutParts.cpp, ui::FaderNormForDb / ui::FaderCapY): a level's position.
float normFor(const ParamDesc& d, double db) { return static_cast<float>(d.toNormalized(d.clamp(dsp::faderValueForDb(db, kUnity)))); }

}  // namespace

int main() {
  Song s = makeEmptySong();
  const ParamId vol = mixerParam(s.mixer[0].id, MixerParam::Volume);
  const auto d = describeParam(s, vol);
  expect(d.has_value(), "the master's volume is described");
  if (!d) return finish();

  // The law: each mark's fader value reads back as that level (0.01 dB), and 0 dB is the unity value.
  expect(std::fabs(dsp::faderValueForDb(0, kUnity) - kUnity) < 1e-12, "0 dB is the fader's unity value");
  for (int db : kMarks) {
    const double v = dsp::faderValueForDb(db, kUnity);
    if (v > d->max + 1e-9) continue;
    const double back = dsp::gainToDb(dsp::faderGain(v, kUnity));
    char n[64];
    std::snprintf(n, sizeof n, "the %+d dB mark's value reads %+.3f dB", db, back);
    expect(std::fabs(back - db) < 0.01, n);
  }
  // The fader's top is above +6 dB, so the scale has positive marks (the tester saw none).
  expect(dsp::faderValueForDb(6, kUnity) <= d->max, "+6 dB is on the fader");

  // The geometry: a fader at 0.0 dB (value 0.8) has its cap's centre on the 0 mark's row, in Classic
  // and in each control style, for the mixer's and the inspector's fader heights.
  const look::geom::FaderSpec* specs[] = {nullptr, &look::geom::kSlateFader, &look::geom::kGraphiteFader, &look::geom::kHairlineFader,
                                          &look::geom::kLumenFader};
  for (const look::geom::FaderSpec* sp : specs)
    for (float h : {88.0f, 120.0f, 160.0f, 233.0f}) {
      const float x = 400, y = 300.5f, w = 20;
      for (int db : kMarks) {
        if (dsp::faderValueForDb(db, kUnity) > d->max + 1e-9) continue;
        const float mark = std::round(look::geom::faderCapY(x, y, w, h, normFor(*d, db), sp));
        // the cap: the fader set to the level as a person sets it (the value the engine reads)
        const double v = dsp::faderValueForDb(db, kUnity);
        const float cap = std::round(look::geom::faderCapY(x, y, w, h, static_cast<float>(d->toNormalized(v)), sp));
        char n[96];
        std::snprintf(n, sizeof n, "%s fader %.0f px: the %+d dB mark on the cap's row (%.1f / %.1f)", sp ? "styled" : "classic", h, db, mark, cap);
        expect(mark == cap, n);
      }
      // The old scale put 0 dB at a gain of 1 on a linear axis (13 px off at 88 px): never again.
      const float old0 = std::round(look::geom::faderCapY(x, y, w, h, static_cast<float>(d->toNormalized(d->clamp(1.0))), sp));
      const float cap0 = std::round(look::geom::faderCapY(x, y, w, h, static_cast<float>(d->toNormalized(kUnity)), sp));
      expect(old0 != cap0, "the unity value is not the old linear 0 dB mark");
    }
  // Marks go down the travel in order (top +6, bottom -60), at least a pixel apart on an 88 px fader.
  float last = -1e9f;
  for (int db : kMarks) {
    const float yy = look::geom::faderCapY(0, 0, 20, 88, normFor(*d, db), nullptr);
    expect(yy > last, "the marks run down the fader in order");
    last = yy;
  }
  return finish();
}

// Gridline K-383: the Clips layout's track meters (core/ClipsMeter.h): the dB to height mapping
// (0 dB at the top, -60 dB at the bottom, weighted to the top), its inverse and tick positions, the
// colour zones, and the ballistics (instant rise, a 20 dB/s fall at any frame rate, a short hold, at
// rest in silence), with the shared LevelMeter left as it was.
#include <cmath>
#include <cstdio>
#include "Check.h"
#include "Hermetic.h"
#include "core/ClipsMeter.h"
#include "core/LevelMeter.h"

using namespace gl;

namespace {

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
float gain(float db) { return std::pow(10.0f, db / 20.0f); }

void run(LevelMeter& m, float peak, double fps, double seconds) {
  const int frames = static_cast<int>(std::lround(seconds * fps));
  for (int i = 0; i < frames; ++i) m.update(peak, peak, static_cast<float>(1.0 / fps));
}

void testMapping() {
  using namespace clipsmeter;
  expect(pos(0.0f) == 1.0f && pos(3.0f) == 1.0f, "map: 0 dBFS and overs at the top");
  expect(pos(-60.0f) == 0.0f && pos(-90.0f) == 0.0f && pos(NAN) == 0.0f, "map: -60 dB and under at the bottom");
  expect(gainPos(0.0f) == 0.0f && gainPos(1.0f) == 1.0f && gainPos(INFINITY) == 1.0f, "map: silence at the bottom, full scale at the top");
  // Known levels -> heights (x = (dB + 60) / 60, 0.35 x + 0.65 x^2).
  const struct {
    float db, want;
  } k[] = {{-3, 0.9188f}, {-6, 0.8415f}, {-12, 0.696f}, {-18, 0.5635f}, {-24, 0.444f}, {-36, 0.244f}, {-48, 0.096f}};
  for (const auto& e : k) {
    char what[96];
    std::snprintf(what, sizeof what, "map: %.0f dB at %.3f (got %.4f)", static_cast<double>(e.db), static_cast<double>(e.want), static_cast<double>(pos(e.db)));
    expect(near(pos(e.db), e.want, 0.002f), what);
  }
  // Weighted to the top: the top 12 dB take more height than the bottom 24 dB, and -18 dBFS (a
  // typical track peak) sits a little over the middle, not near the top.
  expect(pos(0.0f) - pos(-12.0f) > pos(-36.0f) - pos(-60.0f), "map: the top 12 dB are taller than the bottom 24 dB");
  expect(pos(-18.0f) > 0.5f && pos(-18.0f) < 0.62f, "map: -18 dBFS just over the middle");
  expect(pos(-18.0f) < LevelMeter::toPos(-18.0f) - 0.05f, "map: -18 dBFS sits lower than on the shared -60..+6 dB scale");
  // Every dB step up is a step up (monotonic), and steeper near the top than at the bottom.
  bool mono = true;
  for (float d = -60.0f; d < 0.0f; d += 0.25f) mono = mono && pos(d + 0.25f) > pos(d);
  expect(mono, "map: rises with every quarter dB");
  expect(pos(0.0f) - pos(-1.0f) > 2.5f * (pos(-59.0f) - pos(-60.0f)), "map: more height per dB at the top than at the bottom");
  // The inverse, and the scale's ticks where the bars reach.
  bool inv = true;
  for (float d = -59.5f; d < 0.0f; d += 0.5f) inv = inv && near(dbAt(pos(d)), d, 0.01f);
  expect(inv, "map: dbAt inverts pos");
  expect(dbAt(0.0f) == kFloorDb && dbAt(1.0f) == 0.0f, "map: dbAt ends");
  for (int db = 0; db >= -60; db -= kTickStepDb) {
    char what[64];
    std::snprintf(what, sizeof what, "ticks: the %d tick is where a %d dB bar ends", -db, db);
    expect(near(gainPos(gain(static_cast<float>(db))), pos(static_cast<float>(db)), 1e-4f), what);
  }
  expect(kTickStepDb == 12, "ticks: 0, 12, 24, 36, 48, 60");
  // Colour zones: green to -6 dB, yellow to 0 dB, red only over 0 dBFS.
  expect(kYellowDb == -6.0f && pos(kYellowDb) > 0.8f && pos(kYellowDb) < 0.87f, "colours: yellow from -6 dB (the top sixth)");
}

void testBallistics() {
  {
    LevelMeter m = clipsmeter::makeMeter();
    run(m, 0.0f, 60, 1.0);
    expect(m.atRest() && m.levelDb(0) == LevelMeter::kFloorDb, "ballistics: silence rests at the floor");
  }
  {  // Instant rise.
    LevelMeter m = clipsmeter::makeMeter();
    m.update(gain(-12.0f), gain(-12.0f), 1.0f / 60.0f);
    expect(near(m.levelDb(0), -12.0f, 0.01f) && near(m.holdDb(1), -12.0f, 0.01f), "ballistics: one frame reaches the level");
  }
  for (const double fps : {30.0, 60.0, 144.0}) {  // A 20 dB/s fall, the same at any frame rate.
    LevelMeter m = clipsmeter::makeMeter();
    run(m, gain(-6.0f), fps, 0.5);
    run(m, 0.0f, fps, 1.0);
    char what[96];
    std::snprintf(what, sizeof what, "ballistics: %.0f fps falls 20 dB in a second (-6 -> %.2f dB)", fps, static_cast<double>(m.levelDb(0)));
    expect(near(m.levelDb(0), -26.0f, 0.2f), what);
  }
  {  // Release rate measured over a quarter second.
    LevelMeter m = clipsmeter::makeMeter();
    run(m, 1.0f, 60, 0.2);
    run(m, 0.0f, 60, 0.25);
    const float a = m.levelDb(0);
    run(m, 0.0f, 60, 0.25);
    const float rate = (a - m.levelDb(0)) / 0.25f;
    char what[64];
    std::snprintf(what, sizeof what, "ballistics: release %.1f dB/s", static_cast<double>(rate));
    expect(near(rate, clipsmeter::kReleaseDbPerSec, 0.5f) && clipsmeter::kReleaseDbPerSec >= 15.0f && clipsmeter::kReleaseDbPerSec <= 26.0f, what);
  }
  {  // A short hold: the peak line stays 0.6 s, then falls at the release rate; the meter rests in about 3.6 s.
    LevelMeter m = clipsmeter::makeMeter();
    run(m, gain(-3.0f), 60, 0.1);
    run(m, 0.0f, 60, 0.5);
    expect(near(m.holdDb(0), -3.0f, 0.01f), "hold: still at the peak after 0.5 s");
    run(m, 0.0f, 60, 0.3);
    expect(m.holdDb(0) < -3.5f, "hold: falling by 0.8 s");
    expect(clipsmeter::kHoldSeconds <= 1.0f, "hold: short (1 s or less)");
    run(m, 0.0f, 60, 3.0);
    expect(m.atRest(), "rest: silent again within about 3.6 s (no redraws needed after)");
  }
  {  // Overs light red (the clip lamp), a full-scale sample does not.
    LevelMeter m = clipsmeter::makeMeter();
    m.update(1.0f, 1.0f, 0.016f);
    expect(!m.clipping(), "overs: full scale is not an over");
    m.update(1.2f, 0.5f, 0.016f);
    expect(m.clipping() && m.levelDb(0) > 0.0f, "overs: over 0 dBFS is");
  }
  {  // reset keeps the meter's own ballistics.
    LevelMeter m = clipsmeter::makeMeter();
    m.update(1.0f, 1.0f, 0.016f);
    m.reset();
    run(m, gain(-6.0f), 60, 0.1);
    run(m, 0.0f, 60, 1.0);
    expect(near(m.levelDb(0), -26.0f, 0.2f), "reset: keeps the 20 dB/s fall");
  }
  {  // The shared meter (Studio, Smooth, the top bar) is unchanged: 24 dB/s, 1.5 s hold.
    LevelMeter m;
    run(m, gain(-6.0f), 60, 0.5);
    run(m, 0.0f, 60, 1.0);
    expect(near(m.levelDb(0), -30.0f, 0.2f) && near(m.holdDb(0), -6.0f, 0.01f), "shared: 24 dB/s and the 1.5 s hold as before");
  }
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k383");
  testMapping();
  testBallistics();
  return finish();
}

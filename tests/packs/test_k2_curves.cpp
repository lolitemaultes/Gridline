// K-34 (K-AD): the corrected curve modes. Saved curves keep the old numbers and their
// sound; FL imports get the measured ones and export them back under FL's own numbers.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/engine/MEnv.h"
#include "../../src/engine/WaveCurve.h"
#include "../Check.h"

using namespace gl;

namespace {

bool near(double a, double b, double e = 1e-5) { return std::fabs(a - b) <= e; }

struct Pt {
  double x, y;
  float t;
  uint32_t mode;
};

std::vector<uint8_t> waveShaper(const std::vector<Pt>& pts) {
  std::vector<uint8_t> b;
  auto u32 = [&](uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(v >> (8 * k)));
  };
  auto f64 = [&](double v) {
    uint8_t r[8];
    std::memcpy(r, &v, 8);
    b.insert(b.end(), r, r + 8);
  };
  u32(0x00040003), u32(128), u32(128), u32(128);
  b.push_back(0), b.push_back(0);
  u32(0);
  u32(3), u32(3), u32(static_cast<uint32_t>(pts.size()));
  double prev = 0;
  for (const Pt& p : pts) {
    f64(p.x - prev), f64(p.y);
    prev = p.x;
    uint32_t t;
    std::memcpy(&t, &p.t, 4);
    u32(t), u32(p.mode);
  }
  u32(0);
  for (int k = 0; k < 4; ++k) u32(0xffffffffu);
  u32(0);
  b.push_back(0), b.push_back(0);
  return b;
}

template <class E>
void build(E& e, uint8_t mode) {
  e.add(0, 0.f, 0.f, menv::SingleCurve);
  e.add(0.1, 0.3f, 0.f, mode);
  e.add(0.25, 0.35f, 0.f, mode);
  e.add(0.8, 0.7f, 0.f, mode);
  e.add(1, 1.f, 0.f, mode);
  e.prepare();
}

}  // namespace

int main() {
  using namespace menv;
  // ---------------------------------------------------------- Old numbers untouched
  expect(Smooth == 10 && SingleCurve3 == 11 && DoubleCurve3 == 12 && Smooth2 == 13 && DoubleCurve3b == 14, "the saved numbers stay");
  expect(ModeCount == 15, "two new modes");
  {
    Env<8> old, now;
    build(old, Smooth);
    build(now, Smooth2);
    // Tangent at x = 0.25 between segments of slope 0.5/... : weights differ on unequal spacing.
    const double h0 = 0.15, h1 = 0.55, d0 = 0.05 / h0, d1 = 0.35 / h1;
    const double wrong = 3 * d0 * d1 * (h0 + h1) / ((2 * h1 + h0) * d0 + (h1 + 2 * h0) * d1);
    const double right = 3 * d0 * d1 * (h0 + h1) / ((2 * h1 + h0) * d1 + (h1 + 2 * h0) * d0);
    expect(near(old.tanE[2] * d0, wrong, 1e-5), "old law keeps its tangent");
    expect(near(now.tanE[2] * d0, right, 1e-5), "new law: Fritsch-Butland weights");
    // Next to a non-smooth segment the old law uses the secant slope, the new one a flat end.
    Env<8> beside, beside2;
    beside.add(0, 0.f, 0.f, SingleCurve), beside.add(0.5, 0.2f, 0.f, Smooth), beside.add(1, 1.f, 0.f, SingleCurve);
    beside.prepare();
    beside2.add(0, 0.f, 0.f, SingleCurve), beside2.add(0.5, 0.2f, 0.f, Smooth2), beside2.add(1, 1.f, 0.f, SingleCurve);
    beside2.prepare();
    expect(near(beside.tanS[1], 1) && near(beside.tanE[1], 1), "old law: secant at the ends");
    expect(beside2.tanS[1] == 0.f && beside2.tanE[1] == 0.f, "new law: flat ends");
    expect(near(beside.eval(0.25), 0.1, 1e-5), "old law: straight when both ends are the secant");
    expect(near(beside2.eval(0.25), 0.1, 1e-5) && beside2.eval(0.125) < 0.05 - 1e-3, "new law: an S between the points");
  }
  {
    // Double curve 3 is two mirrored Single curve 3 halves; the old mode halves the bend once more.
    const float t = 0.5f;
    const Coef s = coef(SingleCurve3, t), b = coef(DoubleCurve3b, t), o = coef(DoubleCurve3, t);
    for (double f : {0.1, 0.3, 0.45}) expect(near(shape(DoubleCurve3b, b, f), 0.5 * shape(SingleCurve3, s, 2 * f), 1e-6), "new double curve: two halves");
    expect(near(shape(DoubleCurve3b, b, 0.75), 1.0 - 0.5 * shape(SingleCurve3, s, 0.5), 1e-6), "new double curve: mirrored");
    expect(near(shape(DoubleCurve3, o, 0.1), 0.25 * shape(SingleCurve3, s, 0.2) + 0.25 * 0, 0.2), "old double curve: still its own");
    expect(!near(shape(DoubleCurve3, o, 0.1), shape(DoubleCurve3b, b, 0.1), 1e-3), "the laws differ");
  }
  // ---------------------------------------------------------- Import and export
  {
    const std::vector<Pt> pts = {{0, 0, 0, SingleCurve}, {0.3, 0.2, 0, Smooth}, {0.6, 0.5, 0, Smooth}, {1, 1, 0.5f, DoubleCurve3}};
    const std::vector<uint8_t> st = waveShaper(pts);
    flp::enc::EncodeContext c;
    flp::PluginMapping m;
    expect(flp::enc::decodeState("Fruity WaveShaper", st, m, c), "WaveShaper decodes");
    wscurve::Curve cv;
    expect(wscurve::parse(m.state.data.c_str(), cv) && cv.n == 4, "curve imported");
    expect(cv.n == 4 && cv.mode[1] == Smooth2 && cv.mode[2] == Smooth2 && cv.mode[3] == DoubleCurve3b, "FL's modes import as the measured laws");
    normalizePluginState(m.state);
    expect(flp::enc::encode(m.state, "Fruity WaveShaper", &st, c).quality == flp::enc::Result::Verbatim, "unchanged import stays verbatim");
    PluginState native = m.state;
    const auto out = flp::enc::encode(native, "", nullptr, c);
    flp::PluginMapping back;
    expect(out.hasState && flp::enc::decodeState("Fruity WaveShaper", out.state, back, c), "export decodes");
    wscurve::Curve cb;
    expect(wscurve::parse(back.state.data.c_str(), cb) && cb.n == 4 && cb.mode[1] == Smooth2 && cb.mode[3] == DoubleCurve3b, "export writes FL's numbers back");
    // A curve saved before: the text keeps 10 and 12.
    wscurve::Curve saved;
    expect(wscurve::parse("WS1|0,0,0,0 0.5,0.5,0,10 1,1,0,12", saved) && saved.n == 3 && saved.mode[1] == Smooth && saved.mode[2] == DoubleCurve3, "saved curves stay on the old laws");
  }
  return finish();
}

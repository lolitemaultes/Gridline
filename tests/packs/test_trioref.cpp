// Gridline — J-65 tests: Trio Synth "Reference" oscillators (packs/synths/trioref.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/flp/FlpEncode.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/packs/Packs.h"
#include "../Check.h"

using namespace gl;
using namespace gl::pp;

namespace {

struct Out {
  std::vector<float> L, R;
};

// Renders `total` frames of one note (C5 = 60) held for `hold` frames, in blocks of `block`.
Out render(const PluginState& s, double pitch, float vel, int hold, int total, int block = 256, float pan = 0.f) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.f), o.R.assign(static_cast<size_t>(total), 0.f);
  auto g = createGenerator(s.type);
  ChannelAux aux;
  prepareChannelAux(s, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100, ctx.tempo = 120;
  NoteProps pr;
  pr.pan = pan;
  for (int pos = 0; pos < total; pos += block) {
    const int k = std::min(block, total - pos);
    if (pos == 0) g->noteOn(1, pitch, vel, 0, pr);
    if (hold >= pos && hold < pos + k) g->noteOff(1, hold - pos);
    g->process(s, aux, ctx, o.L.data() + pos, o.R.data() + pos, k);
  }
  return o;
}

// Least-squares amplitude of a sine at f over [a, b).
double amp(const std::vector<float>& x, double f, size_t a, size_t b) {
  double ss = 0, sc = 0, s2 = 0, c2 = 0, sxc = 0;
  for (size_t i = a; i < b; ++i) {
    const double w = 2 * M_PI * f * static_cast<double>(i) / 44100.0, s = std::sin(w), c = std::cos(w);
    ss += x[i] * s, sc += x[i] * c, s2 += s * s, c2 += c * c, sxc += s * c;
  }
  (void)sxc;
  return std::hypot(ss / s2, sc / c2);
}

PluginState fromFl(std::vector<int32_t> vals, uint8_t hq) {
  // Version-13 state: 21 values, invert ×3 + AM bytes, HQ byte.
  std::vector<uint8_t> b(4 + 84 + 5, 0);
  const int32_t ver = 13;
  std::memcpy(b.data(), &ver, 4);
  vals.resize(21, 0);
  std::memcpy(b.data() + 4, vals.data(), 84);
  b[92] = hq;
  flp::PluginMapping m;
  flp::enc::EncodeContext c;
  if (!flp::enc::decodeState("3x Osc", b, m, c)) return {};
  return m.state;
}

void testReference() {
  // The reference instrument's default patch: sines at C6 / C5 / C4 (osc 2 −12, osc 3 −24), mix 64 / 32.
  std::vector<int32_t> def = {0, 0, 0, 0, 0, 0, 64, 0, 0, -12, 0, 0, 0, 32, 0, 0, -24, 0, 0, 0, 0};
  PluginState s = fromFl(def, 0);
  expect(s.type == "gl.synth" && s.params.size() > static_cast<size_t>(SynModel) && s.params[SynModel] == 1.f, "trio ref: the import selects the reference oscillators");
  if (s.type != "gl.synth") return;
  const Out o = render(s, 60, 100.f / 128.f, 13230, 22050);
  // Velocity 100 = note volume 1; centre pan 0.7071; the oscillators' own pan 0.7071; levels 0.375 / 0.375 / 0.25.
  const double a1 = amp(o.L, 523.2511306, 0, 13000), a2 = amp(o.L, 261.6255653, 0, 13000), a3 = amp(o.L, 130.8127827, 0, 13000);
  std::printf("# trio ref amplitudes %.6f %.6f %.6f\n", a1, a2, a3);
  expect(std::fabs(a1 - 0.1875) < 1e-3 && std::fabs(a2 - 0.1875) < 1e-3 && std::fabs(a3 - 0.125) < 1e-3,
         "trio ref: C5 sounds at 523.25 Hz; level law (1 − m2)(1 − m3), m2 (1 − m3), m3");
  // Note end: a 10 ms fade, then silence.
  float tail = 0;
  for (size_t i = 13230 + 441; i < o.L.size(); ++i) tail = std::max(tail, std::fabs(o.L[i]));
  expect(tail == 0.f && std::fabs(o.L[13230 + 5]) > 1e-3f, "trio ref: the note ends with a 10 ms declick");
  // Block size does not change the output.
  const Out b = render(s, 60, 100.f / 128.f, 13230, 22050, 37), c = render(s, 60, 100.f / 128.f, 13230, 22050, 1024);
  bool same = true;
  for (size_t i = 0; i < o.L.size(); ++i) same = same && o.L[i] == b.L[i] && o.L[i] == c.L[i] && o.R[i] == b.R[i];
  expect(same, "trio ref: block-size invariant");
  // Hard-left note: the right channel stays silent.
  const Out l = render(s, 60, 100.f / 128.f, 13230, 13230, 256, -1.f);
  float r = 0;
  for (float x : l.R) r = std::max(r, std::fabs(x));
  expect(r == 0.f, "trio ref: note pan (constant power) — hard left");
  // High-quality saw: band-limited (C5 sits on a table boundary: partials up to the 45th).
  std::vector<int32_t> saw = def;
  saw[1] = 3, saw[6] = 0, saw[13] = 0;
  PluginState h = fromFl(saw, 1);
  const Out hs = render(h, 60, 100.f / 128.f, 44100, 8192);
  const double h45 = amp(hs.L, 523.2511306 * 45, 512, 8192), h46 = amp(hs.L, 523.2511306 * 46, 512, 8192), h1 = amp(hs.L, 523.2511306, 512, 8192);
  std::printf("# trio ref hq saw h1 %.5f h45 %.6f h46 %.2g\n", h1, h45, h46);
  expect(h1 > 0.1 && h45 > h1 / 60 && h46 < h45 / 20, "trio ref: HQ saw keeps harmonics up to the quarter-octave table's limit");
  // Old states (no model parameter) keep the Trio oscillators.
  PluginState old = defaultPluginState("gl.synth");
  old.params.resize(SynModel);
  normalizePluginState(old);
  expect(old.params[SynModel] == 0.f, "trio ref: states saved before the model parameter keep the Trio oscillators");
}

}  // namespace

int main() {
  testReference();
  return finish();
}

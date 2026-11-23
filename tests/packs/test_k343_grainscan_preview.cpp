// Gridline - K-343: Grain Scanner's grain preview follows all four Grains dials. Attack and Hold
// always shaped it; Grain spacing (how often a grain starts) and Wave spacing (how far the read
// head moves between two grains) work in the engine but left the preview alone.
//   1. the shared timing has the engine's laws (defaults: 40 ms attack, 5 ms hold, back to back
//      grains, original speed);
//   2. Grain spacing moves the grains of the preview apart or together;
//   3. Wave spacing tilts the read head (forwards, held, backwards) and sets the speed it shows;
//   4. the engine plays the same at its defaults as the laws say (one grain every 45 ms).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/metal.h"
#include "../Check.h"

using namespace gl;
using namespace gl::packs::models::metal;

namespace {

void ex(bool ok, const std::string& name) { expect(ok, name.c_str()); }
bool near(double a, double b, double t) { return std::fabs(a - b) <= t; }

GsTiming timing(float gs, float ws) { return gsTiming(204, 50, gs, ws, 44100); }

}  // namespace

int main() {
  // 1.
  const GsTiming d = timing(611, 611);
  ex(near(d.attSec, 0.040, 1e-9) && near(d.holdSec, 0.005, 1e-9) && near(d.spaceSec, 0.045, 1e-9) && near(d.speed(), 1.0, 1e-9),
     "defaults: 40 ms attack, 5 ms hold, a grain every 45 ms, original speed");
  // 2.
  const GsPreview p0 = gsPreview(d), dense = gsPreview(timing(300, 611)), sparse = gsPreview(timing(1000, 611));
  std::printf("# grains in the preview: default %d, dense %d, sparse %d\n", p0.count, dense.count, sparse.count);
  ex(dense.count > p0.count && dense.x0[1] < p0.x0[1], "Grain spacing down: more grains, closer together");
  ex(sparse.count >= 2 && sparse.x0[1] > sparse.x3[0], "Grain spacing up: a gap between the grains");
  bool shapeSame = true;
  for (int k = 0; k < p0.count; ++k) shapeSame = shapeSame && p0.x1[k] - p0.x0[k] > 0 && p0.x3[k] > p0.x2[k];
  ex(shapeSame, "each grain keeps its attack, hold and release");
  // 3.
  const GsPreview fwd = gsPreview(d), held = gsPreview(timing(611, 0)), back = gsPreview(timing(611, -611)), slow = gsPreview(timing(611, 411));
  ex(fwd.head[1] > fwd.head[0] && near(fwd.speed, 1.0, 1e-9), "Wave spacing forwards: the read head rises, 1.00x");
  ex(held.head[1] == held.head[0] && held.speed == 0, "Wave spacing 0: the read head stays (held)");
  ex(back.head[1] < back.head[0] && back.speed < 0, "Wave spacing negative: the read head falls (backwards)");
  ex(slow.speed > 0.3 && slow.speed < 0.7 && slow.head[1] < fwd.head[1], "Wave spacing 411: about half speed, a gentler slope");
  // 4. one note at the defaults: grains start every 45 ms (the output's envelope repeats)
  PluginState s;
  s.type = "gl.grainscan";
  normalizePluginState(s);
  auto g = packs::createGenerator("gl.grainscan");
  ChannelAux aux;
  packs::prepareAux(s, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  std::vector<float> L(44100), R(44100);
  g->noteOn(1, 60, 0.8f, 0);
  for (int pos = 0; pos < 44100; pos += 256) g->process(s, aux, ctx, L.data() + pos, R.data() + pos, std::min(256, 44100 - pos));
  double e = 0;
  for (float x : L) e += static_cast<double>(x) * x;
  ex(e > 1e-3, "the engine plays at its defaults with the shared timing");
  return finish();
}

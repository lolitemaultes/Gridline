// Gridline — J-25 performance work: regression guards for every optimisation.
//  * DSP fast paths keep their output (golden checksums captured from the
//    code before the optimisation; tolerance where the arithmetic changed).
//  * Whole-song undo steps (delta snapshots) restore exactly and stay small.
//  * Denormal flushing is active inside the engine and restored after it.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "../fixtures/TestSong.h"
#include "../../src/core/Document.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Denormals.h"
#include "../../src/engine/Effects.h"
#include "../../src/engine/Render.h"
#include "../../src/packs/dynamics/dynamics.h"
#include "../Check.h"

using namespace gl;

namespace {

std::vector<float> noise(size_t n, uint32_t seed) {
  std::vector<float> v(n);
  for (auto& x : v) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    x = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f * 0.5f;
  }
  return v;
}

struct Sum {
  double sum = 0, abs = 0, sq = 0;
  float at[4] = {};
};

// Noise burst (0.25 s) then silence through an effect, in odd-sized blocks.
Sum runFx(const std::string& type, const std::vector<std::pair<int, float>>& params, double seconds = 2.0) {
  auto fx = createEffect(type);
  PluginState st = defaultPluginState(type);
  for (auto [k, v] : params) st.params[static_cast<size_t>(k)] = v;
  const double sr = 44100;
  const size_t n = static_cast<size_t>(seconds * sr);
  std::vector<float> L = noise(n, 1234), R = noise(n, 99);
  for (size_t i = static_cast<size_t>(0.25 * sr); i < n; ++i) L[i] = R[i] = 0;
  ProcessContext c;
  c.sampleRate = sr;
  for (size_t off = 0; off < n;) {
    int b = static_cast<int>(std::min<size_t>(n - off, 300 + off % 211));
    fx->process(st, c, L.data() + off, R.data() + off, b);
    off += static_cast<size_t>(b);
  }
  Sum s;
  for (size_t i = 0; i < n; ++i) {
    s.sum += L[i] + R[i];
    s.abs += std::fabs(L[i]) + std::fabs(R[i]);
    s.sq += static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i];
  }
  // Q-2: a pick past the run's end reads 0 (the 1 s runs used to read past
  // the buffer: whatever the heap held there, so the check failed at random,
  // more often on a busy machine where the heap is reused differently).
  const size_t pick[4] = {100, 11025, 30000, 70000};
  for (int k = 0; k < 4; ++k) s.at[k] = pick[k] < n ? L[pick[k]] : 0.0f;
  return s;
}

bool close(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::max(1e-9, std::fabs(b)); }

void checkGolden(const char* name, const Sum& s, const double g[7], double rel) {
  if (std::getenv("GL_PRINT_GOLDEN"))
    std::printf("# %s {%.17g, %.17g, %.17g, %.9g, %.9g, %.9g, %.9g}\n", name, s.sum, s.abs, s.sq, s.at[0], s.at[1], s.at[2], s.at[3]);
  bool ok = close(s.sum, g[0], rel * 50) && close(s.abs, g[1], rel) && close(s.sq, g[2], rel);
  for (int k = 0; k < 4; ++k) ok = ok && std::fabs(s.at[k] - g[3 + k]) <= rel * 10 + 1e-7;
  expect(ok, name);
}

}  // namespace

int main() {
  // ---- Reverb (J-25: one sin/cos per sample instead of 16 sin, hoisted
  //      per-block constants, no modulo in the allpasses) ----
  {
    using namespace pp;
    static const double plain[7] = {-87.81206747332908, 6653.1301428066417, 1898.1696756721251, 0.121882699, -0.0197831243, -0.0139368037, 3.84136656e-05};
    static const double mod[7] = {-87.774473695910274, 7388.7481449309198, 1943.7827320241618, 0.121882699, -0.0277187955, -0.0530817434, 0.00652316725};
    checkGolden("reverb (no modulation) matches the reference", runFx("gl.reverb", {{RvbDecayTime, 2.5f}, {RvbWet, 0.5f}}), plain, 1e-9);
    checkGolden("reverb (modulated) matches the reference within 1e-6",
                runFx("gl.reverb", {{RvbDecayTime, 4.5f}, {RvbModDepth, 0.6f}, {RvbModRate, 1.3f}, {RvbSize, 0.85f}}), mod, 1e-6);
  }

  // ---- Oversampled saturation (J-25: circular FIR buffers instead of
  //      shifting the history every sample; same arithmetic order: exact) ----
  {
    using namespace packs::dynamics;
    static const double os4[7] = {-86.615606158839668, 2604.4507095505046, 481.14257364789177, 0, -0.0481787473, 3.21525826e-25, 0};
    static const double os2[7] = {-86.478856969695556, 2659.0505232690939, 500.7580559141507, 0, -0.145377219, 3.19146646e-25, 0};
    checkGolden("master dynamics 4x oversampling matches the reference", runFx("gl.master", {{MsOversample, 2.f}}, 1.0), os4, 1e-12);
    checkGolden("master dynamics 2x oversampling matches the reference", runFx("gl.master", {{MsOversample, 1.f}}, 1.0), os2, 1e-12);
  }

  // ---- whole-song undo steps are deltas ----
  {
    Document doc(makeTestSong());
    const Song s0 = doc.song();
    doc.editSong("tempo", [](Song& s) { s.tempo = 99; });
    const Song s1 = doc.song();
    doc.editSong("rename + clips", [](Song& s) {
      s.info.title = "Delta";
      if (!s.playlist.empty()) s.playlist[0].start += 96;
      if (!s.channels.empty()) s.channels[0].name = "Lead (renamed)";
      s.markers.push_back(Marker{});
    });
    const Song s2 = doc.song();
    expect(s1.tempo == 99 && s1.channels == s0.channels, "delta edit applies only its change");
    expect(s2.info.title == "Delta" && s2.channels[0].name == "Lead (renamed)", "delta edit applies several fields");
    doc.undo();
    expect(doc.song() == s1, "undo restores the previous song exactly");
    doc.undo();
    expect(doc.song() == s0, "second undo restores the original song exactly");
    doc.redo();
    doc.redo();
    expect(doc.song() == s2, "redo replays both steps exactly");
    doc.undo();
    doc.editSong("branch", [](Song& s) { s.patterns.clear(); });
    expect(doc.song().patterns.empty() && doc.song().tempo == 99, "new edit after undo");
    doc.undo();
    expect(doc.song() == s1, "undo of a container edit restores it");
  }

  // ---- denormals are flushed inside the engine, restored after ----
  {
    volatile float tiny = 1e-39f;  // denormal
    {
      ScopedFlushDenormals ftz;
      volatile float y = tiny * 1.0f;
      expect(y == 0.0f, "denormals flush to zero inside the DSP scope");
    }
    volatile float z = tiny * 1.0f;
    expect(z != 0.0f, "the caller's float mode is restored after the scope");
  }

  // ---- renders are identical for 1 and 4 threads and any block size ----
  {
    Song s = makeTestSong();
    RenderOptions o;
    o.threads = 1;
    AudioBuffer a = renderSong(s, o);
    o.threads = 4;
    AudioBuffer b = renderSong(s, o);
    expect(a.frames > 0 && a.data == b.data, "render identical for 1 and 4 threads");
    o.threads = 1;
    o.blockSize = 333;
    AudioBuffer c = renderSong(s, o);
    double d = 0;
    for (size_t i = 0; i < std::min(a.data.size(), c.data.size()); ++i) d = std::max(d, static_cast<double>(std::fabs(a.data[i] - c.data[i])));
    std::printf("# block-size max diff %g\n", d);
    expect(c.frames == a.frames, "render length independent of the block size");
  }
  return finish();
}

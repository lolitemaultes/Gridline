// Gridline - K-133 sound: the Pitch Shifter's Music engine (grain read point and level law), the
// vocoder's output enhancer level, and the vintage LFOs' start phase at transport start.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "fl_corpus.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/spacefx/spacefx.h"
#include "../../src/packs/spectral/spectral.h"

using namespace gl;
namespace sp = gl::packs::spectral;
namespace sx = gl::packs::spacefx;

namespace {

constexpr double kTau = 6.283185307179586;

void expectf(bool ok, const char* fmt, double a, double b = 0) {
  char buf[256];
  std::snprintf(buf, sizeof buf, fmt, a, b);
  expect(ok, buf);
}

// The same with the plugin type in front.
void named(bool ok, const char* type, const char* fmt, double a, double b = 0) {
  char buf[200], line[260];
  std::snprintf(buf, sizeof buf, fmt, a, b);
  std::snprintf(line, sizeof line, "%s: %s", type, buf);
  expect(ok, line);
}

struct Run {
  std::vector<float> L, R;
};

// The input through the effect as the engine runs it (prepared data, the transport as given per block).
Run render(const PluginState& st, std::vector<float> L, std::vector<float> R, int block = 512, int playFrom = 0) {
  auto fx = createEffect(st.type);
  fx->prepareHost(st, 44100);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  const int n = static_cast<int>(L.size());
  for (int i = 0; i < n;) {
    const int k = std::min(block, n - i);
    ctx.playing = playFrom >= 0 && i >= playFrom;
    ctx.tick = ctx.playing ? (i - playFrom) * ctx.tempo * ctx.ppq / (60.0 * 44100) : 0;
    fx->processAux(st, aux, ctx, L.data() + i, R.data() + i, k);
    i += k;
  }
  return {L, R};
}

PluginState musicPitch(int law, double density) {
  PluginState st = defaultPluginState("gl.pitch");
  st.params[sp::PsEngine] = sp::kPsMusic;
  st.params[sp::PsPitch] = 0;
  st.params[sp::PsDensity] = static_cast<float>(density);
  st.params[sp::PsJitter] = 0;
  st.params[sp::PsRand] = 0;
  st.params[sp::PsMix] = 1;
  st.params[sp::PsMusicLaw] = static_cast<float>(law);
  return st;
}

// Music engine: the grain tap sits 4 samples behind the input (was 5), and the grains sum into a
// soft clip (was scaled by 2 / density): an impulse train reads tanh of the summed windows.
void testPitchMusic() {
  expect(defaultPluginState("gl.pitch").params[sp::PsMusicLaw] == 1, "pitch: a new instance uses grain law 2");
  {
    PluginState old = defaultPluginState("gl.pitch");
    old.params.resize(sp::PsMusicLaw);  // saved before the parameter existed
    normalizePluginState(old);
    expect(old.params[sp::PsMusicLaw] == 0, "pitch: a state saved before keeps grain law 1");
  }
  for (int law = 0; law < 2; ++law) {
    const int at = 20000, n = 44100;
    std::vector<float> L(n, 0.f), R(n, 0.f);
    L[at] = R[at] = 0.5f;
    Run r = render(musicPitch(law, 2), L, R);
    int first = -1;
    for (int i = at; i < n && first < 0; ++i)
      if (std::fabs(r.L[static_cast<size_t>(i)]) > 1e-6f) first = i - at;
    const int want = law ? 4 : 5;
    expectf(first == want, "pitch: Music engine grain tap at %.0f samples (law %.0f)", first, law + 1);
  }
  // Impulse train (step 7): the wet tap of every impulse reads the summed grain windows S. Law 2: tanh(S)
  // (S = 1 at density 2, 1.5 at 3, 2.5 at 5); law 1 read 2 / density x S = 1 whatever the density.
  const double want[3] = {std::tanh(1.0), std::tanh(1.5), std::tanh(2.5)};
  const double dens[3] = {2, 3, 5};
  for (int d = 0; d < 3; ++d) {
    const int n = 44100;
    std::vector<float> L(n, 0.f), R(n, 0.f);
    for (int i = 0; i < n; i += 7) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = 1.f;
    Run r = render(musicPitch(1, dens[d]), L, R);
    double sum = 0;
    int cnt = 0;
    for (int i = 22050; i + 4 < n; i += 7, ++cnt) sum += r.L[static_cast<size_t>(i + 4)];
    const double mean = sum / cnt;
    expectf(std::fabs(mean - want[d]) < 0.03, "pitch: summed grain level %.3f (reference %.3f)", mean, want[d]);
  }
  // A unison tone: denser grains play louder, as the reference does (law 1 kept the level).
  double rms[2] = {0, 0};
  for (int d = 0; d < 2; ++d) {
    const int n = 44100;
    std::vector<float> L(n), R(n);
    for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(i * 0.031));
    Run r = render(musicPitch(1, d ? 5 : 2), L, R);
    double e = 0;
    for (int i = 8192; i < n; ++i) e += r.L[static_cast<size_t>(i)] * r.L[static_cast<size_t>(i)];
    rms[d] = std::sqrt(e / (n - 8192));
    expect(std::isfinite(rms[d]), "pitch: finite output");
  }
  expectf(rms[1] > rms[0] * 1.6, "pitch: density 5 plays louder than density 2 (%.3f / %.3f)", rms[1], rms[0]);
  // Silence in, silence out; extremes finite.
  {
    std::vector<float> L(20000, 0.f), R(20000, 0.f);
    Run r = render(musicPitch(1, 3), L, R);
    double pk = 0;
    for (float v : r.L) pk = std::max(pk, static_cast<double>(std::fabs(v)));
    expect(pk == 0, "pitch: silence in, silence out");
    PluginState st = musicPitch(1, 5);
    st.params[sp::PsPitch] = 24;
    st.params[sp::PsJitter] = 1;
    st.params[sp::PsRand] = 1;
    std::vector<float> A(20000), B(20000);
    for (int i = 0; i < 20000; ++i) A[static_cast<size_t>(i)] = B[static_cast<size_t>(i)] = (i % 50) < 25 ? 1.f : -1.f;
    r = render(st, A, B);
    bool fin = true;
    for (float v : r.L) fin = fin && std::isfinite(v) && std::fabs(v) <= 1.f;
    expect(fin, "pitch: extreme settings stay finite and inside the soft clip");
  }
}

// Bucket Chorus and Retro Phaser: while the transport stands still the LFO starts at phase 0, and the
// start phase applies when the transport starts.
void testStartPhase() {
  struct Case {
    const char* type;
    int model, start;
  } cases[2] = {{"gl.bbdchorus", sx::BcModel, sx::BcStartPhase}, {"gl.vphaser", sx::VpModel, sx::VpStartPhase}};
  for (const Case& c : cases) {
    const int n = 44100;
    std::vector<float> L(n), R(n);
    for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(i * 0.05) + 0.1 * std::sin(i * 0.31));
    PluginState a = defaultPluginState(c.type);
    a.params[static_cast<size_t>(c.model)] = 1;
    PluginState b = a;
    a.params[static_cast<size_t>(c.start)] = 0;
    b.params[static_cast<size_t>(c.start)] = 0.5f;
    auto diff = [&](const Run& x, const Run& y, int from, int to) {
      double e = 0;
      for (int i = from; i < to; ++i) e = std::max(e, static_cast<double>(std::fabs(x.L[static_cast<size_t>(i)] - y.L[static_cast<size_t>(i)])));
      return e;
    };
    const Run sa = render(a, L, R, 512, -1), sb = render(b, L, R, 512, -1);
    named(diff(sa, sb, 0, n) < 1e-6, c.type, "stopped, the start phase does not move the LFO (max diff %.2g)", diff(sa, sb, 0, n));
    const Run pa = render(a, L, R, 512, 0), pb = render(b, L, R, 512, 0);
    named(diff(pa, pb, 0, n) > 1e-3, c.type, "playing from the start, the start phase applies (max diff %.2g)", diff(pa, pb, 0, n));
    // Stopped for a while, then the transport starts: from there the start phase applies.
    const Run la = render(a, L, R, 512, 10240), lb = render(b, L, R, 512, 10240);
    named(diff(la, lb, 0, 10240) < 1e-6 && diff(la, lb, 10240, n) > 1e-3, c.type, "and it applies when the transport starts (%.2g / %.2g)",
            diff(la, lb, 0, 10240), diff(la, lb, 10240, n));
  }
}

// Vocodex imports: the enhancer program keeps its level whatever the enhancer amount (the amount used to be
// the program's band mix: at 100 % the output came out 2.4 times as loud as at 15 %).
void testVocoderEnhancer() {
  {
    PluginState old = defaultPluginState("gl.vocoder");
    old.params.resize(sp::VxEnhanceLaw);
    normalizePluginState(old);
    expect(old.params[sp::VxEnhanceLaw] == 0, "vocoder: a state saved before keeps enhancer law 1");
  }
  if (!testfl::flInstalled()) return;
  flp::PresetResult pr;
  const std::string path = testfl::flData() + "/Data/Patches/Plugin presets/Effects/Vocodex/Configuration/Stereo interlacing.fst";
  if (!flp::importFlPreset(path, pr, testfl::flData()) || pr.kind != flp::PresetResult::Plugin) {
    expect(false, "vocoder: the shipped interlacing preset decodes");
    return;
  }
  expect(pr.state.params[sp::VxEnhanceLaw] == 1, "vocoder: an import uses enhancer law 2");
  // Left: a voiced modulator in syllables, right: a three-saw carrier.
  const int n = 2 * 44100;
  std::vector<float> L(n), R(n);
  uint32_t seed = 7;
  double ph[3] = {0, 0, 0};
  for (int i = 0; i < n; ++i) {
    const double t = i / 44100.0, syl = std::pow(std::max(0.0, std::sin(kTau * 3 * t)), 2.0);
    const double f0 = 140 + 20 * std::sin(kTau * 0.7 * t);
    double buzz = 0;
    for (int k = 1; k < 12; ++k) buzz += std::sin(kTau * f0 * k * t) / k * (k >= 2 && k <= 6 ? 1.0 : 0.4);
    seed = seed * 1664525u + 1013904223u;
    const double nz = (seed >> 8) / 8388608.0 - 1.0;
    double car = 0;
    const double fr[3] = {110, 165, 220};
    for (int j = 0; j < 3; ++j) {
      ph[j] = std::fmod(ph[j] + fr[j] / 44100.0, 1.0);
      car += 0.15 * (2 * ph[j] - 1);
    }
    L[static_cast<size_t>(i)] = static_cast<float>(2.1 * (0.075 * syl * buzz + 0.05 * nz * (1 - syl)));
    R[static_cast<size_t>(i)] = static_cast<float>(2.1 * car);
  }
  auto rmsAt = [&](PluginState st, double amount) {
    st.params[sp::VxEnhance] = static_cast<float>(amount);
    Run r = render(st, L, R);
    double e = 0;
    for (int i = 8192; i < n; ++i) e += 0.5 * (r.L[static_cast<size_t>(i)] * r.L[static_cast<size_t>(i)] + r.R[static_cast<size_t>(i)] * r.R[static_cast<size_t>(i)]);
    return std::sqrt(e / (n - 8192));
  };
  const double lo = rmsAt(pr.state, 0.15), hi = rmsAt(pr.state, 1.0);
  expectf(lo > 1e-3 && hi / lo < 1.1, "vocoder: enhancer at 100 %% against 15 %%: %.3f times the level (reference 0.98)", hi / lo);
}

}  // namespace

int main() {
  testPitchMusic();
  testVocoderEnhancer();
  testStartPhase();
  return finish();
}

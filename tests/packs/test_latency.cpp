// Gridline — plugin delay compensation: every built-in effect's latencyFor()
// matches where an impulse (or a burst) actually lands in its output, for
// the settings that make it latent, at 44.1 and 48 kHz; and the reported
// value is a deterministic, sane function of (state, sample rate) for every
// catalogue effect and preset.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/engine/Effects.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/dynamics/dynamics.h"
#include "../../src/packs/modulation/modulation.h"
#include "../../src/packs/modulation/modulation_dsp.h"
#include "../../src/packs/spectral/spectral.h"
#include "../../src/packs/utility/utility.h"
#include "../Check.h"

using namespace gl;
namespace dyn = gl::packs::dynamics;
namespace spc = gl::packs::spectral;
namespace utl = gl::packs::utility;
namespace mod = gl::packs::modulation;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kT0 = 8192;  // impulse position (after silence)

PluginState st(const char* type, std::vector<std::pair<int, float>> v = {}) {
  PluginState s = defaultPluginState(type);
  for (auto [k, x] : v) s.params[static_cast<size_t>(k)] = x;
  return s;
}

// Runs `in` (both channels) through a fresh instance in 256-frame blocks; returns the left output.
std::vector<float> run(const PluginState& s, double sr, const std::vector<float>& in) {
  auto fx = createEffect(s.type);
  if (!fx) return {};
  ChannelAux aux;
  packs::prepareAux(s, aux);
  fx->reset();
  ProcessContext c;
  c.sampleRate = sr;
  std::vector<float> L = in, R = in;
  for (size_t off = 0; off < L.size(); off += 256) {
    int n = static_cast<int>(std::min<size_t>(256, L.size() - off));
    fx->processAux(s, aux, c, L.data() + off, R.data() + off, n);
  }
  return L;
}

// Position of the output peak of a unit impulse at kT0, relative to kT0.
int impulseLag(const PluginState& s, double sr, int expect, int shift = 0) {
  const int t0 = kT0 + shift;
  const size_t len = static_cast<size_t>(t0 + std::max(expect, 0) + 8192);
  std::vector<float> in(len, 0.f);
  in[static_cast<size_t>(t0)] = 0.5f;
  auto out = run(s, sr, in);
  int best = -t0;
  double mx = -1;
  for (size_t i = 0; i < out.size(); ++i)
    if (std::fabs(out[i]) > mx) mx = std::fabs(out[i]), best = static_cast<int>(i) - t0;
  return best;
}

// Lag (0..maxLag) that best aligns the output with a Hann-windowed sine burst
// (for effects an impulse does not pass through, e.g. the phase vocoder).
int burstLag(const PluginState& s, double sr, int maxLag) {
  const int B = 2048;
  const size_t len = static_cast<size_t>(kT0 + B + maxLag + 4096);
  std::vector<float> in(len, 0.f);
  for (int i = 0; i < B; ++i)
    in[static_cast<size_t>(kT0 + i)] =
        static_cast<float>(0.5 * (0.5 - 0.5 * std::cos(2 * kPi * i / B)) * std::sin(2 * kPi * 1000.0 * i / sr));
  auto out = run(s, sr, in);
  int best = 0;
  double bv = -1e300;
  for (int lag = 0; lag <= maxLag; ++lag) {
    double acc = 0;
    for (int i = 0; i < B; ++i) acc += static_cast<double>(in[static_cast<size_t>(kT0 + i)]) * out[static_cast<size_t>(kT0 + i + lag)];
    if (acc > bv) bv = acc, best = lag;
  }
  return best;
}

// Delay of the envelope of a steady tone (`hz`) through a short amplitude dip:
// centroid of the dip in a one-period moving RMS, output minus input.
double dipLag(const PluginState& s, double sr, double hz, int maxLag) {
  const double tc = sr * 1.0, sigma = 0.004 * sr;
  const size_t len = static_cast<size_t>(tc + 10 * sigma + maxLag + 4096);
  std::vector<float> in(len);
  for (size_t i = 0; i < len; ++i) {
    double u = (static_cast<double>(i) - tc) / sigma;
    in[i] = static_cast<float>(0.4 * std::sin(2 * kPi * hz * static_cast<double>(i) / sr) * (1.0 - 0.9 * std::exp(-0.5 * u * u)));
  }
  auto out = run(s, sr, in);
  const int P = static_cast<int>(std::lround(sr / hz));
  auto centroid = [&](const std::vector<float>& x, size_t from, size_t to) {
    std::vector<double> env(len, 0.0);
    double acc = 0;
    for (size_t i = 0; i < len; ++i) {
      acc += static_cast<double>(x[i]) * x[i];
      if (i >= static_cast<size_t>(P)) acc -= static_cast<double>(x[i - static_cast<size_t>(P)]) * x[i - static_cast<size_t>(P)];
      env[i] = std::sqrt(std::max(0.0, acc) / P);
    }
    double base = 0;
    for (size_t i = from; i < from + static_cast<size_t>(4 * sigma); ++i) base = std::max(base, env[i]);
    double w = 0, m = 0;
    for (size_t i = from; i < to; ++i) {
      double d = std::max(0.0, base - env[i]);
      d *= d;
      w += d, m += d * static_cast<double>(i);
    }
    return w > 0 ? m / w : 0.0;
  };
  const size_t a = static_cast<size_t>(tc - 8 * sigma), b = static_cast<size_t>(tc + 8 * sigma);
  return centroid(out, a + static_cast<size_t>(maxLag - 2000), b + static_cast<size_t>(maxLag - 2000) + 4000) - centroid(in, a, b);
}

int reported(const PluginState& s, double sr) {
  auto fx = createEffect(s.type);
  return fx ? fx->latencyFor(s, sr) : -1;
}

// The measured lag of `s` (minus the lag of `ref`, when given: removes filter
// smearing common to both) equals latencyFor() within `tol` samples.
void check(const char* what, const PluginState& s, int tol, const PluginState* ref = nullptr) {
  for (double sr : {44100.0, 48000.0}) {
    const int lat = reported(s, sr);
    int lag = impulseLag(s, sr, lat);
    int refLat = 0;
    if (ref) refLat = reported(*ref, sr), lag -= impulseLag(*ref, sr, refLat) - refLat;
    char name[200];
    std::snprintf(name, sizeof name, "%s @%.0f: reported %d, measured %d", what, sr, lat, lag);
    expect(lat >= 0 && std::abs(lag - lat) <= tol, name);
  }
}

void latentEffects() {
  // Parametric EQ: linear phase = 1.5 × taps; minimum phase = 0.
  for (int lin = 1; lin <= 6; ++lin) {
    char what[64];
    std::snprintf(what, sizeof what, "gl.eq linear phase %d", lin);
    check(what, st("gl.eq", {{pp::EqLinear, static_cast<float>(lin)}}), 0);
  }
  {
    // A mild boost: the symmetric FIR's peak stays at its centre.
    PluginState s = st("gl.eq", {{pp::EqLinear, 2.f}, {pp::eqParam(3, pp::EqGain), 3.f}});
    check("gl.eq linear phase + 3 dB band", s, 1);
  }
  check("gl.drawneq linear phase", st("gl.drawneq", {{utl::CePhase, 1.f}}), 0);
  check("gl.multiband linear-phase FIR", st("gl.multiband", {{pp::MbFilterType, 1.f}}), 0);
  // Look-ahead dynamics: the audio is delayed by exactly the look-ahead.
  check("gl.gate look-ahead 5 ms", st("gl.gate", {{pp::GtLookahead, 5.f}}), 0);
  check("gl.gate look-ahead 0.7 ms", st("gl.gate", {{pp::GtLookahead, 0.7f}}), 0);
  {
    PluginState ref = st("gl.maximizer", {{pp::MxLookahead, 0.f}});
    check("gl.maximizer look-ahead 10 ms", st("gl.maximizer", {{pp::MxLookahead, 10.f}}), 0, &ref);
    check("gl.maximizer look-ahead 3.3 ms", st("gl.maximizer", {{pp::MxLookahead, 3.3f}}), 0, &ref);
  }
  check("gl.limiter default attack", st("gl.limiter"), 0);
  check("gl.limiter attack 7.3 ms", st("gl.limiter", {{pp::LimAttack, 0.0073f}}), 0);
  check("gl.limiter attack 0", st("gl.limiter", {{pp::LimAttack, 0.f}}), 0);
  check("gl.comp default (64-sample output limiter)", st("gl.comp"), 0);
  check("gl.comp look-ahead 10 ms", st("gl.comp", {{pp::CmpLookahead, 10.f}}), 0);
  check("gl.comp limiter off", st("gl.comp", {{pp::CmpLimiter, 0.f}}), 0);
  check("gl.comp limiter off, look-ahead 4 ms", st("gl.comp", {{pp::CmpLimiter, 0.f}, {pp::CmpLookahead, 4.f}}), 0);
  // Oversampled shapers (dry path aligned internally; half-band delay ~ x.5 samples).
  for (int os = 0; os <= 3; ++os) {
    char what[64];
    std::snprintf(what, sizeof what, "gl.waveshaper oversampling %d", os);
    check(what, st("gl.waveshaper", {{pp::WsOversample, static_cast<float>(os)}}), 1);
    std::snprintf(what, sizeof what, "gl.dist oversampling %d", os);
    check(what, st("gl.dist", {{pp::DstOversample, static_cast<float>(os)}}), 1);
    if (os <= 2) {
      std::snprintf(what, sizeof what, "gl.clipper oversampling %d", os);
      check(what, st("gl.clipper", {{pp::ClpOversample, static_cast<float>(os)}}), 1);
    }
  }
  for (int os = 0; os <= 2; ++os) {
    char what[64];
    std::snprintf(what, sizeof what, "gl.overdrive oversampling %d", os);
    check(what, st("gl.overdrive", {{dyn::OdOversample, static_cast<float>(os)}}), 1);
    PluginState ref = st("gl.amprack", {{dyn::RkOversample, 0.f}});
    std::snprintf(what, sizeof what, "gl.amprack oversampling %d", os);
    check(what, st("gl.amprack", {{dyn::RkOversample, static_cast<float>(os)}}), 1, &ref);
    std::snprintf(what, sizeof what, "gl.emphasis oversampling %d", os);
    check(what, st("gl.emphasis", {{dyn::EmOversample, static_cast<float>(os)}}), 1);
  }
  for (int ch = 0; ch < 4; ++ch) {
    char what[64];
    std::snprintf(what, sizeof what, "gl.emphasis character %d", ch);
    check(what, st("gl.emphasis", {{dyn::EmCharacter, static_cast<float>(ch)}}), 1);
  }
  check("gl.fastdist", st("gl.fastdist"), 1);
  check("gl.effector distortion", st("gl.effector", {{dyn::EfKind, 0.f}}), 2);
  // Mastering engine: two look-ahead stages + oversampled saturators.
  check("gl.master default", st("gl.master"), 2);
  check("gl.master look-ahead 10 ms", st("gl.master", {{dyn::MsLookahead, 10.f}}), 2);
  for (int os = 0; os <= 2; ++os) {
    char what[64];
    std::snprintf(what, sizeof what, "gl.master oversampling %d", os);
    check(what, st("gl.master", {{dyn::MsOversample, static_cast<float>(os)}}), 2);
  }
  check("gl.enhance default", st("gl.enhance"), 2);
  check("gl.enhance custom program", st("gl.enhance", {{dyn::EnCustom, 1.f}}), 2);
  // Pitch shifter: fast mode reads 2 + window/2 back; the phase vocoder one frame.
  check("gl.pitch fast mode", st("gl.pitch", {{spc::PsEngine, 0.f}, {spc::PsMode, 0.f}}), 1);
  check("gl.pitch fast mode, 20 ms window", st("gl.pitch", {{spc::PsEngine, 0.f}, {spc::PsMode, 0.f}, {spc::PsGrain, 20.f}}), 1);
  for (double sr : {44100.0, 48000.0}) {
    PluginState s = st("gl.pitch", {{spc::PsEngine, 0.f}, {spc::PsMode, 1.f}});
    const int lat = reported(s, sr), lag = burstLag(s, sr, 6000);
    char name[160];
    std::snprintf(name, sizeof name, "gl.pitch spectral mode @%.0f: reported %d, measured %d", sr, lat, lag);
    expect(lat > 0 && std::abs(lag - lat) <= 2, name);
  }
  // Pitch corrector (PSOLA): grains copy the input around the pitch mark
  // nearest their output position, so single samples move by up to half a
  // period; the envelope of an in-tune tone is delayed by the reported value
  // within half a period.
  for (int r = 0; r < 4; ++r)
    for (double sr : {44100.0, 48000.0}) {
      PluginState s = st("gl.pitcher", {{spc::PcLowest, static_cast<float>(r)}});
      const int lat = reported(s, sr);
      const double lag = dipLag(s, sr, 220.0, lat + 2000);
      char name[160];
      std::snprintf(name, sizeof name, "gl.pitcher range %d @%.0f: reported %d, envelope delay %.1f", r, sr, lat, lag);
      expect(lat > 0 && std::fabs(lag - lat) <= 0.5 * sr / 220.0, name);
    }
  // Patch host: serial stages add up (linear EQ, then a look-ahead limiter).
  {
    mod::PatchGraph g;
    mod::PatchNode a, b;
    a.plugin = st("gl.eq", {{pp::EqLinear, 1.f}});
    b.plugin = st("gl.limiter");
    g.nodes = {a, b};
    PluginState s = st("gl.patcherfx");
    s.data = mod::serializePatchGraph(g);
    check("gl.patcherfx linear EQ -> limiter", s, 1);
    for (double sr : {44100.0, 48000.0})
      expect(reported(s, sr) == reported(a.plugin, sr) + reported(b.plugin, sr), "gl.patcherfx sums serial stages");
    g.nodes[1].parallel = true;  // parallel branch: the stage takes its longest node
    s.data = mod::serializePatchGraph(g);
    expect(reported(s, 48000) == std::max(reported(a.plugin, 48000), reported(b.plugin, 48000)), "gl.patcherfx parallel stage = max");
  }
}

// Effects that report 0 really put the impulse at sample 0.
void zeroLatency() {
  const char* types[] = {"gl.eq",       "gl.filter",    "gl.delay",   "gl.reverb",  "gl.chorus",   "gl.convolver",
                         "gl.banddelay", "gl.freqshift", "gl.stereo",  "gl.utility", "gl.geq",      "gl.splitter",
                         "gl.spread",    "gl.transient", "gl.squeeze", "gl.phaser",  "gl.multidelay", "gl.drawneq",
                         "gl.hall",      "gl.transplit", "gl.stereomatrix", "gl.patcherfx"};
  for (const char* t : types) {
    PluginState s = st(t);
    for (double sr : {44100.0, 48000.0}) {
      const int lat = reported(s, sr), lag = impulseLag(s, sr, 0);
      char name[160];
      std::snprintf(name, sizeof name, "%s @%.0f zero latency (reported %d, measured %d)", t, sr, lat, lag);
      // The reference band splitter's IIR crossover sum is an all-pass whose
      // impulse peak lands one sample late; that is phase, not latency.
      const int tol = std::string(t) == "gl.splitter" ? 1 : 0;
      expect(lat == 0 && lag >= 0 && lag <= tol, name);
    }
  }
}

// Every catalogue effect (defaults and presets): sane, deterministic, pure.
void wholeCatalogue() {
  int effects = 0, bad = 0, latent = 0;
  for (const PluginInfo& info : pluginCatalog()) {
    if (info.kind != PluginKind::Effect) continue;
    ++effects;
    std::vector<PluginState> states = {defaultPluginState(info.type)};
    for (const auto& pr : info.presets) {
      PluginState s = defaultPluginState(info.type);
      for (auto [k, v] : pr.values)
        if (k >= 0 && static_cast<size_t>(k) < s.params.size()) s.params[static_cast<size_t>(k)] = v;
      states.push_back(s);
    }
    for (const PluginState& s : states)
      for (double sr : {44100.0, 48000.0}) {
        auto a = createEffect(info.type), b = createEffect(info.type);
        if (!a || !b) {
          ++bad;
          std::printf("# %s: no instance\n", info.type.c_str());
          continue;
        }
        const int la = a->latencyFor(s, sr), lb = b->latencyFor(s, sr), le = effectLatency(s, sr);
        // Purity: an instance that has processed audio (at another rate) reports the same.
        ChannelAux aux;
        packs::prepareAux(s, aux);
        ProcessContext c;
        c.sampleRate = sr == 48000.0 ? 44100.0 : 48000.0;
        std::vector<float> L(512, 0.1f), R(512, -0.1f);
        a->processAux(s, aux, c, L.data(), R.data(), 256);
        a->processAux(s, aux, c, L.data() + 256, R.data() + 256, 256);
        const int lc = a->latencyFor(s, sr);
        if (la < 0 || la >= (1 << 20) || la != lb || la != le || la != lc) {
          ++bad;
          std::printf("# %s @%.0f: %d %d %d %d\n", info.type.c_str(), sr, la, lb, le, lc);
        }
        if (la > 0) ++latent;
      }
  }
  char name[120];
  std::snprintf(name, sizeof name, "catalogue: %d effects, latencyFor sane/deterministic/pure (%d latent states)", effects, latent);
  expect(effects > 50 && bad == 0, name);
  // Linear phase toggles the EQ's latency.
  PluginState eq = defaultPluginState("gl.eq");
  const int off = effectLatency(eq, 48000);
  eq.params[pp::EqLinear] = 1.f;
  const int on = effectLatency(eq, 48000);
  expect(off == 0 && on > 0, "gl.eq: linear phase switches latency from 0 to > 0");
  expect(effectLatency(defaultPluginState("gl.analyzer"), 48000) == 0, "analyzer: no latency");
}

}  // namespace

int main() {
  latentEffects();
  zeroLatency();
  wholeCatalogue();
  return finish();
}

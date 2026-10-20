// Gridline — J-35: effects exactness tier E (the hard leftovers). Reference
// engines and exact decoders for the loudness limiter, the full reverb, the
// mastering dynamics, the module rack, the pedal board and the mobile rack
// (studies: analysis/demos/specs/j35_*.md). FL preset checks run only when an
// FL Studio installation is present.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Song.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpPresets.h"
#include "../src/engine/Effects.h"
#include "../src/engine/ClassicDelay.h"
#include "../src/engine/ClassicModFx.h"
#include "../src/engine/FlFilterRef.h"
#include "../src/engine/FreqShiftRef.h"
#include "../src/engine/FullReverbRef.h"
#include "../src/engine/Delay3Ref.h"
#include "../src/engine/LowLifterRef.h"
#include "../src/engine/PlateReverb.h"
#include "../src/packs/dynamics/dynamics.h"
#include "../src/packs/dynamics/effector_ref.h"
#include "../src/packs/dynamics/emphasis_ref.h"
#include "../src/packs/dynamics/maximus_ref.h"
#include "../src/packs/dynamics/modrack_ref.h"
#include "../src/packs/dynamics/pedal_ref.h"
#include "../src/packs/spacefx/hyperchorus_ref.h"
#include "../src/packs/spacefx/spacefx.h"
#include "../src/packs/spacefx/sfx_flcurves.h"
#include "../src/packs/utility/utility.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;
namespace dy = gl::packs::dynamics;
namespace ut = gl::packs::utility;

namespace {
constexpr double kSr = 48000;
const char* kFlFx = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Plugin presets/Effects";
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool haveFl() { return fs::exists(kFlFx); }

// ------------------------------------------------------------------ FLP event builder (decoder input)
struct Builder {
  std::vector<uint8_t> ev;
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  std::vector<uint8_t> file() const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, 96, 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};
bool decodeFx(const std::string& flName, const std::vector<uint8_t>& state, flp::PluginMapping& m) {
  static Builder B;
  B.ev.clear();
  B.text(201, flName);
  B.data(213, state);
  static flp::File f;
  std::string err;
  if (!flp::readBytes(B.file(), f, &err)) return false;
  static Song song;
  static flp::Report rep;
  flp::Context c(f, song, rep);
  auto it = flp::decoders().find(flName);
  if (it == flp::decoders().end()) return false;
  m = flp::PluginMapping{};
  m.flName = flName;
  if (!it->second(c, f.events, m) || m.glType.empty()) return false;
  normalizePluginState(m.state);
  return true;
}
struct Blob {
  std::vector<uint8_t> b;
  Blob& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
  Blob& u8(uint8_t v) { return b.push_back(v), *this; }
  Blob& f32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return i32(static_cast<int32_t>(u));
  }
  Blob& ints(std::initializer_list<int32_t> v) {
    for (int32_t x : v) i32(x);
    return *this;
  }
};
double P(const flp::PluginMapping& m, int i) { return i >= 0 && static_cast<size_t>(i) < m.state.params.size() ? m.state.params[static_cast<size_t>(i)] : -1e9; }
double P(const PluginState& s, int i) { return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : -1e9; }

// ------------------------------------------------------------------ audio helpers
struct Buf {
  std::vector<float> L, R;
};
Buf sine(double hz, double sec, double amp = 0.5) {
  Buf b;
  size_t n = static_cast<size_t>(sec * kSr);
  b.L.resize(n), b.R.resize(n);
  for (size_t i = 0; i < n; ++i) b.L[i] = b.R[i] = static_cast<float>(amp * std::sin(2 * 3.14159265358979 * hz * static_cast<double>(i) / kSr));
  return b;
}
Buf noise(double sec, double amp, uint32_t seed = 7) {
  Buf b;
  size_t n = static_cast<size_t>(sec * kSr);
  b.L.resize(n), b.R.resize(n);
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    b.L[i] = static_cast<float>(amp * ((seed >> 8) / 8388608.0 - 1.0));
    seed = seed * 1664525u + 1013904223u;
    b.R[i] = static_cast<float>(amp * ((seed >> 8) / 8388608.0 - 1.0));
  }
  return b;
}
Buf run(const PluginState& s, Buf b, int block = 256, double tempo = 120) {
  auto fx = createEffect(s.type);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = tempo;
  for (size_t i = 0; i < b.L.size(); i += static_cast<size_t>(block)) {
    int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), b.L.size() - i));
    fx->process(s, ctx, b.L.data() + i, b.R.data() + i, n);
  }
  return b;
}
bool finite(const Buf& b) {
  for (size_t i = 0; i < b.L.size(); ++i)
    if (!std::isfinite(b.L[i]) || !std::isfinite(b.R[i])) return false;
  return true;
}
double peak(const Buf& b, size_t from = 0) {
  double p = 0;
  for (size_t i = from; i < b.L.size(); ++i) p = std::max(p, static_cast<double>(std::max(std::fabs(b.L[i]), std::fabs(b.R[i]))));
  return p;
}
[[maybe_unused]] double rms(const std::vector<float>& v, size_t from = 0, size_t to = 0) {
  if (to == 0 || to > v.size()) to = v.size();
  double a = 0;
  for (size_t i = from; i < to; ++i) a += static_cast<double>(v[i]) * v[i];
  return to > from ? std::sqrt(a / static_cast<double>(to - from)) : 0.0;
}
double maxDiff(const Buf& a, const Buf& b) {
  double d = 0;
  for (size_t i = 0; i < a.L.size(); ++i) d = std::max(d, static_cast<double>(std::max(std::fabs(a.L[i] - b.L[i]), std::fabs(a.R[i] - b.R[i]))));
  return d;
}
// Amplitude of the `hz` component (Goertzel over the given range).
double tone(const std::vector<float>& v, double hz, size_t from, size_t to) {
  double w = 2 * 3.14159265358979 * hz / kSr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = from; i < to; ++i) {
    double s0 = v[i] + c * s1 - s2;
    s2 = s1, s1 = s0;
  }
  double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
  return 2 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}
[[maybe_unused]] double gainDb(const PluginState& s, double hz) {
  Buf b = run(s, sine(hz, 1.0, 0.25));
  size_t from = b.L.size() / 2;
  return 20 * std::log10(std::max(1e-9, tone(b.L, hz, from, b.L.size()) / 0.25));
}
PluginState state(const std::string& type, std::initializer_list<std::pair<int, float>> v) {
  PluginState s = defaultPluginState(type);
  for (auto [k, x] : v) s.params[static_cast<size_t>(k)] = x;
  return s;
}
// Every preset of an FL folder (recursively): converted, of the given type, exact.
void presetsExact(const std::string& folder, const std::string& type) {
  if (!haveFl()) return;
  int n = 0, ok = 0;
  for (const auto& e : fs::recursive_directory_iterator(fs::path(kFlFx) / folder)) {
    if (e.path().extension() != ".fst") continue;
    ++n;
    flp::PresetResult r;
    if (flp::importFlPreset(e.path().string(), r) && r.kind == flp::PresetResult::Plugin && r.state.type == type && r.exact) ++ok;
    else std::printf("# %s: not exact\n", e.path().filename().string().c_str());
  }
  std::printf("# %s: %d/%d presets exact\n", folder.c_str(), ok, n);
  expect(n > 0 && ok == n, (folder + ": every shipped preset decodes exactly").c_str());
}
// A state saved before the model parameter existed keeps the legacy engine.
void legacyKept(const std::string& type, int modelParam) {
  PluginState s;
  s.type = type;
  s.params.assign(static_cast<size_t>(modelParam), 0.f);
  PluginState d = defaultPluginState(type);
  for (int k = 0; k < modelParam; ++k) s.params[static_cast<size_t>(k)] = d.params[static_cast<size_t>(k)];
  normalizePluginState(s);
  expect(P(s, modelParam) == 0 && P(defaultPluginState(type), modelParam) == 1, (type + ": old states keep the legacy engine, new ones use the reference").c_str());
}

// Impulse response lag (samples) after the start-up gate has opened.
int impulseLag(const PluginState& s, double at = 0.5) {
  Buf b;
  size_t n = static_cast<size_t>(1.0 * kSr);
  b.L.assign(n, 0.f), b.R.assign(n, 0.f);
  size_t pos = static_cast<size_t>(at * kSr);
  b.L[pos] = b.R[pos] = 0.25f;
  Buf o = run(s, b, 256);
  size_t best = 0;
  float bv = 0;
  for (size_t i = pos; i < n; ++i)
    if (std::fabs(o.L[i]) > bv) bv = std::fabs(o.L[i]), best = i;
  return static_cast<int>(best - pos);
}
}  // namespace

// ===================================================================== Emphasis → gl.emphasis reference
void testEmphasis() {
  namespace er = dy::emphref;
  // Laws: ceiling knob, look-ahead window, reported latency (spec §4).
  expect(near(er::ceilingLin(1), 1, 1e-12) && er::ceilingLin(0) == 0 && near(er::ceilingDb(0.9912), -0.20, 0.01) &&
             near(er::ceilingKnob(er::ceilingDb(0.37)), 0.37, 1e-9),
         "emphasis: ceiling law (11^n - 1)/10 and its inverse");
  expect(near(er::window(0), 0.032, 1e-8) && near(er::window(9), 0.0000625, 1e-10), "emphasis: hardness window 32 ms * 2^-h");
  er::Settings s;
  s.instant = false;
  bool lat = er::latency(s, 44100) == 2381 && er::latency(s, 48000) == 2592 && er::latency(s, 96000) == 5184;
  static const int msLat[10] = {2733, 2733, 2733, 2557, 2469, 2425, 2403, 2392, 2386, 2383};
  static const int lowLat[10] = {1587, 881, 528, 352, 264, 220, 198, 187, 181, 178};
  static const int lowLatMs[10] = {1939, 1233, 880, 528, 352, 264, 220, 198, 186, 180};
  for (int h = 0; h < 10; ++h) {
    er::Settings a = s;
    a.hardness = h, a.ms = true;
    lat = lat && er::latency(a, 44100) == msLat[h];
    a.ms = false, a.lowLatency = true;
    lat = lat && er::latency(a, 44100) == lowLat[h];
    a.ms = true;
    lat = lat && er::latency(a, 44100) == lowLatMs[h];
    a.instant = true;  // instant recovery keeps the 50 ms look-ahead
    lat = lat && er::latency(a, 44100) == msLat[h];
  }
  expect(lat, "emphasis: reported latency for every hardness, M/S and low-latency setting");

  // Decoder: every tag at its place, version < 8 order offset, trailing bytes.
  flp::PluginMapping m;
  Blob e;  // DEDZII - Night Terror (version 8)
  e.ints({8, 0, 0, 1, 0, 1, 0, 2500, 7480, 10000, 9900, 0, 1, 8, 0, 0});
  expect(decodeFx("Emphasis", e.b, m) && m.exact && P(m, dy::EmModel) == 1 && near(P(m, dy::EmInput), 6.0, 1e-6) &&
             P(m, dy::EmStereoMode) == 1 && near(P(m, dy::EmLink), 0.748, 1e-6) && P(m, dy::EmOs5) == 1 && P(m, dy::EmHardness) == 8 &&
             P(m, dy::EmOrder) == 0 && P(m, dy::EmInstant) == 0 && P(m, dy::EmDcBlock) == 0 && P(m, dy::EmLowLatency) == 0 &&
             near(P(m, dy::EmCeilKnob), 0.99, 1e-6),
         "emphasis: version 8 state decodes exactly (options off)");
  Blob e7;  // render test: version 7, order stored 0-based
  e7.ints({7, 0, 0, 0, 0, 0, 0, 0, 10000, 10000, 9912, 0, 2, 7, 0, 0});
  expect(decodeFx("Emphasis", e7.b, m) && m.exact && P(m, dy::EmOrder) == 0 && P(m, dy::EmOs5) == 2 && P(m, dy::EmHardness) == 7,
         "emphasis: version 7 envelope order is 0-based");
  Blob e10;  // version 10, order 4, steady mode, true peak, options bytes
  e10.ints({10, 0, 0, 4, 1, 0, 3, 887, 10000, 10000, 9870, 0, 4, 6, 0, 0}).u8(1).u8(1).u8(1);
  expect(decodeFx("Emphasis", e10.b, m) && m.exact && P(m, dy::EmOrder) == 3 && P(m, dy::EmTruePeak) == 1 && P(m, dy::EmMode) == 3 &&
             P(m, dy::EmOs5) == 4 && P(m, dy::EmLowLatency) == 1 && P(m, dy::EmInstant) == 1 && P(m, dy::EmDcBlock) == 1,
         "emphasis: version 10 state with option bytes");
  presetsExact("Emphasis", "gl.emphasis");

  // Runtime: the start-up gate (0.1 s of silence, then a fade-in).
  PluginState d = state("gl.emphasis", {{dy::EmDcBlock, 0.f}});
  Buf in = sine(220, 0.6, 0.2);
  Buf o = run(d, in);
  expect(finite(o) && peak(Buf{std::vector<float>(o.L.begin(), o.L.begin() + 4700), std::vector<float>(o.R.begin(), o.R.begin() + 4700)}) == 0 &&
             peak(o, 24000) > 0.1,
         "emphasis: a fresh instance is silent for 0.1 s, then fades in");
  // Quiet material: delayed by the reported latency, scaled by the ceiling gain.
  for (int h : {0, 7, 9})
    for (int ms = 0; ms < 2; ++ms) {
      PluginState q = state("gl.emphasis", {{dy::EmDcBlock, 0.f}, {dy::EmHardness, static_cast<float>(h)}, {dy::EmOs5, 0.f},
                                            {dy::EmStereoMode, static_cast<float>(ms)}, {dy::EmInstant, 0.f}});
      auto fx = createEffect("gl.emphasis");
      const int rep = fx->latencyFor(q, kSr), lag = impulseLag(q);
      char name[128];
      std::snprintf(name, sizeof name, "emphasis: hardness %d %s: impulse lag %d = reported latency %d", h, ms ? "M/S" : "L/R", lag, rep);
      expect(std::abs(lag - rep) <= 1, name);
    }
  // Loud material is held under the ceiling (true peak, 4x).
  for (int mode = 0; mode < 4; ++mode) {
    PluginState hot = state("gl.emphasis", {{dy::EmMode, static_cast<float>(mode)}, {dy::EmInput, 18.f}, {dy::EmTruePeak, 1.f}});
    Buf ho = run(hot, noise(1.2, 0.4));
    const double ceil = er::ceilingLin(0.9912);
    char name[96];
    std::snprintf(name, sizeof name, "emphasis: mode %d +18 dB stays under the ceiling (peak %.4f)", mode, peak(ho, 24000));
    expect(finite(ho) && peak(ho, 24000) <= ceil * 1.01 && peak(ho, 24000) > 0.5 * ceil, name);
  }
  // Block-size invariance (the mode 0 / 1 analysis is latched on a fixed grid).
  for (int mode : {0, 1, 2}) {
    PluginState v = state("gl.emphasis", {{dy::EmMode, static_cast<float>(mode)}, {dy::EmInput, 9.f}, {dy::EmAmount, 0.6f}, {dy::EmOs5, 1.f}});
    Buf n = noise(0.8, 0.3);
    expect(maxDiff(run(v, n, 64), run(v, n, 1000)) < 1e-7, ("emphasis: block-size invariant, mode " + std::to_string(mode)).c_str());
  }
  // Delta: what the limiter removes (nothing on quiet input).
  PluginState dq = state("gl.emphasis", {{dy::EmDcBlock, 0.f}, {dy::EmDelta, 1.f}, {dy::EmOs5, 0.f}});
  Buf dd = run(dq, sine(330, 1.0, 0.05));
  expect(peak(dd, 36000) < 0.002, "emphasis: delta is silent when nothing is limited");
  legacyKept("gl.emphasis", dy::EmModel);
}

// ===================================================================== full reverb (Effector module, Reeverb 2)
// Energy (dB) of an impulse response in [a, b) seconds.
double irDb(const PluginState& s, double a, double b) {
  Buf in;
  size_t n = static_cast<size_t>(3.0 * kSr);
  in.L.assign(n, 0.f), in.R.assign(n, 0.f);
  in.L[100] = in.R[100] = 0.5f;
  Buf o = run(s, in);
  double e = 0;
  for (size_t i = static_cast<size_t>(a * kSr); i < static_cast<size_t>(b * kSr) && i < n; ++i) e += o.L[i] * o.L[i] + o.R[i] * o.R[i];
  return 10 * std::log10(e + 1e-30);
}
void testFullReverb() {
  namespace fr = gl::fullrev;
  // Room scaling (spec §1.2 examples).
  auto lens = [](int room, int sr) {
    const int64_t S = fr::roomScale(room, sr);
    std::vector<int> v;
    for (int c : fr::kInConst) v.push_back(fr::lineLen(c, S));
    for (int c : fr::kLeftConst) v.push_back(fr::lineLen(c, S));
    for (int c : fr::kRightConst) v.push_back(fr::lineLen(c, S));
    return v;
  };
  expect(lens(100, 44100) == std::vector<int>{313, 235, 835, 610, 1481, 9818, 3969, 8202, 2002, 9298, 5856, 6974} &&
             lens(50, 44100) == std::vector<int>{176, 133, 472, 345, 837, 5549, 2243, 4636, 1131, 5255, 3310, 3942} &&
             lens(50, 48000) == std::vector<int>{192, 145, 514, 375, 911, 6040, 2441, 5046, 1231, 5720, 3602, 4290},
         "full reverb: line lengths follow the room scale");
  expect(fr::levelDb10(0.8574822f) == 0 && fr::lowCutHz(0.1f) == 317 && fr::cut100(1) == 221 && fr::roomOf(1) == 100 &&
             fr::levelDb10(0) == -361 && fr::decay10(0.07035176f) == 15 && fr::crossHz(0.24050634f) == 500,
         "full reverb: multi-variant control laws (default program)");
  expect(near(fr::levelCurve(1), 1, 1e-7) && near(fr::levelCurve(0.5), (std::sqrt(11.0) - 1) / 10, 1e-6) &&
             fr::predelaySamples(384, 44100, false, 120) == 44100 && fr::predelaySamples(192, 48000, true, 120) == 24000,
         "full reverb: standalone level curve and pre-delay");

  // Effector reverb module: decoded exactly, a decaying tail, block-size invariant.
  flp::PluginMapping m;
  Blob b;
  b.b = {'S', 'S', 'L', 'F'};
  b.i32(308);
  for (char c : std::string("10XF")) b.u8(static_cast<uint8_t>(c));
  while (b.b.size() < 316 - 48) b.u8(0);
  for (float f : {0.5455f, 0.f, 1.f, 0.8984f, 0.3827f, 0.1111f, 0.f, 0.5f, 0.5f, 0.f, 1.f, 0.5f}) b.f32(f);
  expect(decodeFx("Effector", b.b, m) && m.exact && P(m, dy::EfModel) == 1, "effector: reverb kind decodes exactly");
  PluginState ef = m.state;
  ef.params[static_cast<size_t>(dy::EfRawX)] = 0.5f;  // decay floor(149.25 / 8 + 1.5) / 10 = 2.0 s
  const double early = irDb(ef, 0.05, 0.3), late = irDb(ef, 1.5, 2.5);
  char name[128];
  std::snprintf(name, sizeof name, "effector reverb: tail decays (%.1f dB → %.1f dB)", early, late);
  expect(early > -40 && late < early - 12, name);
  Buf nz = noise(0.6, 0.3);
  expect(maxDiff(run(ef, nz, 64), run(ef, nz, 1000)) < 1e-6, "effector reverb: block-size invariant");

  // Reeverb 2 → gl.reverb full model, knobs as stored.
  Blob r;
  r.ints({10001, 120, 60, 38, 80, 90, 25, 70, 150, 700, -20, 128, 100, 140, 900, 300}).u8(0).u8(1);
  expect(decodeFx("Fruity Reeverb 2", r.b, m) && m.exact && P(m, pp::RvbModel) == 2 && P(m, pp::RvbFLowCut) == 120 &&
             P(m, pp::RvbFDecay) == 25 && P(m, pp::RvbFSep) == -20 && P(m, pp::RvbFModDepth) == 300 && P(m, pp::RvbInputMode) == 1,
         "reeverb 2: decodes exactly into the full model");
  Blob r0;  // oldest layout: no version word, 13 knobs
  r0.ints({75, 40, 0, 50, 100, 15, 40, 100, 500, 0, 128, 128, 128}).u8(0);
  expect(decodeFx("Fruity Reeverb 2", r0.b, m) && m.exact && P(m, pp::RvbFModSpeed) == 684 && P(m, pp::RvbFModDepth) == 0,
         "reeverb 2: unversioned state, modulation at its defaults");
  presetsExact("Fruity Reeverb 2", "gl.reverb");
  // Dry only: wet and ER at 0 → the input scaled by the dry curve.
  PluginState dryOnly = state("gl.reverb", {{pp::RvbModel, 2.f}, {pp::RvbFEr, 0.f}, {pp::RvbFWet, 0.f}, {pp::RvbFDry, 64.f}});
  Buf sn = sine(500, 0.3, 0.4);
  Buf od = run(dryOnly, sn);
  double worst = 0;
  for (size_t i = 0; i < sn.L.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(od.L[i] - sn.L[i] * fr::levelCurve(0.5))));
  expect(worst < 1e-6, "reeverb 2 full model: dry path follows the level curve");
  // Decay knob = 60 dB per loop-delay time: the all-passes lengthen the loop by
  // ~39 %, so the tail falls ~21.6 dB in half the decay time (late part, no ER).
  for (int dec : {10, 30}) {
    PluginState rv = state("gl.reverb", {{pp::RvbModel, 2.f}, {pp::RvbFDry, 0.f}, {pp::RvbFEr, 0.f}, {pp::RvbFDecay, static_cast<float>(dec)},
                                         {pp::RvbFDamp, 221.f}, {pp::RvbFHighCut, 221.f}});
    const double T = dec / 10.0, e1 = irDb(rv, 0.3, 0.4), e2 = irDb(rv, 0.3 + T / 2, 0.4 + T / 2);
    std::snprintf(name, sizeof name, "reeverb 2 full model: decay %.1f s → %.1f dB over T/2 (≈ -21.6)", T, e2 - e1);
    expect(e2 - e1 < -18 && e2 - e1 > -26, name);
  }
  PluginState rm = state("gl.reverb", {{pp::RvbModel, 2.f}, {pp::RvbFModDepth, 1500.f}, {pp::RvbFSep, 40.f}, {pp::RvbFPredelay, 30.f}});
  Buf ro1 = run(rm, nz, 64), ro2 = run(rm, nz, 1000);
  expect(finite(ro1) && maxDiff(ro1, ro2) < 1e-6 && peak(ro1) < 4, "reeverb 2 full model: modulated, block-size invariant, bounded");
}

// ===================================================================== Delay 3 → gl.delay stereo reference
PluginState d3state(std::initializer_list<std::pair<int, float>> v) {
  PluginState s = defaultPluginState("gl.delay");
  s.params[static_cast<size_t>(pp::DlyModel)] = 3;
  for (auto [k, x] : v) s.params[static_cast<size_t>(pp::DlyD3Raw0 + k)] = x;
  return s;
}
void testDelay3() {
  namespace d3 = gl::d3ref;
  expect(near(d3::freeTimeSec(1), 1.0, 1e-3) && near(d3::freeTimeSec(0), 0.001, 1e-4) && near(d3::threshDb(1), -28.5, 1e-5) &&
             near(d3::makeUp(0), 1, 1e-6) && near(d3::toNorm(d3::PTone, 0), 0.5, 1e-12) && near(d3::fromNorm(d3::PTime, d3::toNorm(d3::PTime, 193)), 193, 1e-9),
         "delay 3: time, threshold and normalisation laws");
  // Decoder: 26 ints after the version word, every value kept.
  Blob b;
  b.i32(1);
  const int v[26] = {6000, 2, 1, 0, 96, 3000, 500, 6000, 1000, 1200, 0, 2000, 3000, 1, 4000, 0, 4500, 1500, 1000, 4000, 3000, 60000, 6000, 4800, 600, 6000};
  for (int x : v) b.i32(x);
  flp::PluginMapping m;
  bool ok = decodeFx("Fruity Delay 3", b.b, m) && m.exact && P(m, pp::DlyModel) == 3;
  for (int k = 0; k < 26 && ok; ++k) ok = near(P(m, pp::DlyD3Raw0 + k), d3::toNorm(k, v[k]), 1e-6);
  expect(ok, "delay 3: decodes exactly into the stereo reference (26 controls)");
  presetsExact("Fruity Delay 3", "gl.delay");
  // A single echo T samples after the impulse (fake stereo, free time, no feedback, no diffusion).
  for (float tn : {0.3f, 0.7f}) {
    PluginState s = d3state({{d3::PSync, 0.f}, {d3::PTime, tn}, {d3::PSmooth, 0.f}, {d3::PFeedback, 0.f}, {d3::PDifLevel, 0.f}, {d3::PDry, 0.f},
                             {d3::PModTime, 0.f}, {d3::PFilter, 1.f}, {d3::PCutoff, 0.f}, {d3::PRate, 1.f}, {d3::PBits, 1.f},
                             {d3::PTone, 0.5f}, {d3::PThresh, 0.f}, {d3::PDistType, 1.f}, {d3::PHard, 1.f}});
    Buf in;
    in.L.assign(96000, 0.f), in.R.assign(96000, 0.f);
    in.L[1000] = in.R[1000] = 0.5f;
    Buf o = run(s, in);
    size_t best = 0;
    float bv = 0;
    for (size_t i = 0; i < o.L.size(); ++i)
      if (std::fabs(o.L[i]) > bv) bv = std::fabs(o.L[i]), best = i;
    const int T = static_cast<int>(d3::freeTimeSec(tn) * kSr);
    char name[128];
    std::snprintf(name, sizeof name, "delay 3: echo at %d samples (expected %d)", static_cast<int>(best) - 1000, T);
    expect(std::abs(static_cast<int>(best) - 1000 - T) <= 2, name);
  }
  // Every stereo type: bounded, finite, block-size invariant (modulated, saturating, crushed).
  for (int type = 0; type < 4; ++type) {
    PluginState s = d3state({{d3::PType, type / 3.f}, {d3::PFeedback, 0.7f}, {d3::PModTime, 0.6f}, {d3::PDistType, 1.f}, {d3::PThresh, 0.5f},
                             {d3::PRate, 0.6f}, {d3::PBits, 0.7f}, {d3::PDifLevel, 0.8f}, {d3::PKeepPitch, 1.f}, {d3::PTone, 0.3f}});
    Buf nz = noise(0.5, 0.3);
    Buf a = run(s, nz, 64), c = run(s, nz, 1000);
    char name[96];
    std::snprintf(name, sizeof name, "delay 3 type %d: finite, bounded, block-size invariant", type);
    expect(finite(a) && peak(a) < 8 && maxDiff(a, c) < 1e-6, name);
  }
}

// ===================================================================== Maximus → gl.master reference, Soundgoodizer
const char* kSgz = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Plugins/Fruity/Effects/Soundgoodizer/Data/Presets";
bool loadFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}
void testMaximus() {
  namespace mr = dy::maxref;
  // Laws: time, gains, tension shapes.
  expect(near(mr::timeMs(0), 0, 1e-12) && near(mr::timeMs(10000), 1000, 1e-9) && mr::gain7(1000) == 1 && near(mr::gain7(2000), 8, 1e-12) &&
             near(mr::splitHz(19800), 30 * std::pow(2.0, 19800 * 0.000138227748), 1e-9),
         "maximus: time, gain and split laws");
  bool shapes = true;
  for (double tau : {-1.0, -0.4, 0.0, 0.3, 1.0})
    for (int type : {0, 1}) shapes = shapes && near(mr::segShape(0, tau, type), 0, 1e-9) && near(mr::segShape(1, tau, type), 1, 1e-9);
  shapes = shapes && mr::segShape(0.5, 0.5, 0) > 0.9 && mr::segShape(0.5, -0.5, 0) < 0.1 && near(mr::segShape(0.5, 0.7, 1), 0.5, 1e-9) &&
           mr::segShape(0.7, 0, 2) == 0;
  expect(shapes, "maximus: segment shapes (tension law, double curve, hold)");
  presetsExact("Maximus", "gl.master");
  if (!haveFl()) return;
  // Every Soundgoodizer program decodes exactly into the reference block.
  for (const char* prog : {"a", "b", "c", "d"}) {
    std::vector<uint8_t> st;
    flp::PluginMapping m;
    expect(loadFile(std::string(kSgz) + "/" + prog + ".dat", st) && decodeFx("Maximus", st, m) && m.exact && P(m, dy::MsRef0 + dy::MrModel) == 1,
           (std::string("soundgoodizer program ") + prog + ": exact on the reference engine").c_str());
  }
  std::vector<uint8_t> a;
  flp::PluginMapping ma;
  loadFile(std::string(kSgz) + "/a.dat", a);
  decodeFx("Maximus", a, ma);
  PluginState mas = ma.state;
  // Loud noise: held under the curve's ceiling (0.975 safety + post / saturation), finite.
  Buf hot = noise(1.5, 0.9);
  Buf o = run(mas, hot);
  char name[160];
  std::snprintf(name, sizeof name, "maximus program A: loud noise is limited (peak %.3f), finite", peak(o, 24000));
  expect(finite(o) && peak(o, 24000) < 1.3 && peak(o, 24000) > 0.3, name);
  // Block-size invariance.
  Buf nz = noise(0.6, 0.5);
  expect(maxDiff(run(mas, nz, 64), run(mas, nz, 1000)) < 1e-6, "maximus reference: block-size invariant");
  // The Soundgoodizer form (gl.enhance EnCustom 3, amount = band mix) equals gl.master with that mix.
  PluginState en = defaultPluginState("gl.enhance");
  en.params[static_cast<size_t>(dy::EnCustom)] = 3;
  en.params[static_cast<size_t>(dy::EnAmount)] = 0.6f;
  for (int k = 0; k < dy::kMsCount; ++k) en.params[static_cast<size_t>(dy::EnProg0 + k)] = mas.params[static_cast<size_t>(k)];
  for (int k = 0; k < dy::kMrCount; ++k) en.params[static_cast<size_t>(dy::EnRef0 + k)] = mas.params[static_cast<size_t>(dy::MsRef0 + k)];
  PluginState mm = mas;
  mm.params[static_cast<size_t>(dy::MsRef0 + dy::MrRaw0 + 5)] = 600;
  expect(maxDiff(run(en, nz), run(mm, nz)) < 1e-7, "soundgoodizer: amount drives the program's band mix");
  // Stage order (spec j35_maximus.md §5 steps 7–8): the post gain is applied before the
  // saturation, so a saturating band never exceeds its ceiling (a demo's master band:
  // post +1.96 dB, saturation A −16, ceiling 988).
  {
    PluginState sm = mas;
    auto R = [&](int band, int tag, float v) { sm.params[static_cast<size_t>(dy::MsRef0 + dy::MrRaw0 + mr::rawIndex(band, tag))] = v; };
    R(0, mr::TMode, 3), R(2, mr::TMode, 3);  // low / high merged into the mid band
    R(1, mr::TMode, 1), R(1, mr::TPost, 1000), R(1, mr::TSatThr, 0), R(1, mr::TSep, 0);
    R(3, mr::TMode, 0), R(3, mr::TPre, 1000), R(3, mr::TPost, 1101), R(3, mr::TSatThr, -16), R(3, mr::TSatCeil, 988);
    Buf ho = run(sm, noise(1.0, 0.95));
    const double ceil = mr::gain7(988);
    char nm[160];
    std::snprintf(nm, sizeof nm, "maximus: post gain before the saturation ceiling (peak %.4f, ceiling %.4f)", peak(ho, 4800), ceil);
    expect(finite(ho) && peak(ho, 4800) <= ceil + 1e-6 && peak(ho, 4800) > 0.5 * ceil, nm);
  }
  // Reported latency = the delay of a quiet impulse through every band.
  {
    auto fx = createEffect("gl.master");
    const int rep = fx->latencyFor(mas, kSr);
    Buf b;
    b.L.assign(48000, 0.f), b.R.assign(48000, 0.f);
    b.L[20000] = b.R[20000] = 0.01f;
    Buf io = run(mas, b);
    size_t best = 0;
    float bv = 0;
    for (size_t i = 0; i < io.L.size(); ++i)
      if (std::fabs(io.L[i]) > bv) bv = std::fabs(io.L[i]), best = i;
    std::snprintf(name, sizeof name, "maximus reference: reported latency %d, impulse peak at +%d", rep, static_cast<int>(best) - 20000);
    expect(rep > 0 && std::abs(static_cast<int>(best) - 20000 - rep) <= 3, name);
  }
}

// ===================================================================== Distructor → module rack, Hardcore → pedal board
// Every shipped preset of a folder: decodes to `type`, renders finite / bounded / block-size invariant.
void renderPresets(const std::string& folder, const char* label, double bound) {
  if (!haveFl()) return;
  int n = 0, ok = 0, exact = 0;
  Buf nz = noise(0.4, 0.3, 11);
  for (const auto& e : fs::recursive_directory_iterator(fs::path(kFlFx) / folder)) {
    if (e.path().extension() != ".fst") continue;
    flp::PresetResult r;
    if (!flp::importFlPreset(e.path().string(), r) || r.kind != flp::PresetResult::Plugin || r.state.type != "gl.amprack") continue;
    ++n;
    exact += r.exact;
    Buf a = run(r.state, nz, 64), b = run(r.state, nz, 1000);
    if (finite(a) && peak(a) < bound && maxDiff(a, b) < 1e-6) ++ok;
    else std::printf("# %s: finite %d peak %.3f diff %.2g\n", e.path().filename().string().c_str(), finite(a), peak(a), maxDiff(a, b));
  }
  std::printf("# %s: %d presets (%d exact), %d finite / bounded / block-size invariant\n", folder.c_str(), n, exact, ok);
  expect(n > 0 && ok == n, label);
}
void testModuleRack() {
  namespace dr = dy::drack;
  dr::ModuleState d;
  dr::moduleDefaults(d);
  expect(d.p[36] == 5000 && d.p[37] == 10000 && d.p[43] == 108 && dr::level128(128) == 1.f && near(dr::level128(0), 0, 1e-7),
         "module rack: defaults and level law");
  // Distructor state builder (host v5, one module per call).
  auto build = [](std::initializer_list<dr::ModuleState> ms, bool soft) {
    Blob b;
    b.i32(5).i32(static_cast<int32_t>(ms.size()));
    for (const auto& m : ms) {
      b.i32(4).i32(m.kind).i32(m.algo).i32(m.os).u8(static_cast<uint8_t>(m.removeDc)).u8(static_cast<uint8_t>(m.disabled)).i32(m.ftype);
      b.u8(static_cast<uint8_t>(m.fopt)).i32(m.ir);
      for (int v : m.p) b.i32(v);
      b.i32(0).i32(m.chorusMode);
    }
    b.u8(soft ? 1 : 0).i32(4).i32(1).i32(0).i32(2).i32(3).i32(0).u8(1);
    return b;
  };
  dr::ModuleState fast = d, filt = d, ch = d, spk = d;
  fast.algo = 1, fast.p[16] = 191, fast.p[17] = 9, fast.p[18] = 114, fast.p[20] = 1;
  filt.kind = dr::KFilter, filt.ftype = 0, filt.p[39] = 7501;
  ch.kind = dr::KChorus, ch.chorusMode = 2;
  spk.kind = dr::KSpeaker, spk.ir = 1;
  flp::PluginMapping m;
  Blob b = build({fast, filt, ch}, true);
  expect(decodeFx("Distructor", b.b, m) && m.exact && P(m, dy::RkModel) == 1 && P(m, dy::DrCount) == 3 && P(m, dy::drParam(0, dy::DmP0 + 16)) == 191 &&
             P(m, dy::drParam(2, dy::DmChorusMode)) == 2 && P(m, dy::DrSlot0) == 1 && P(m, dy::DrSoftClip) == 1,
         "distructor: modules, slots and soft clip decode exactly");
  expect(m.param && m.param(dy::kDrParams * 1 + 16) == dy::drParam(0, dy::DmP0 + 16), "distructor: automation follows the module's block");
  Blob bs = build({spk}, false);
  expect(decodeFx("Distructor", bs.b, m) && !m.exact, "distructor: a speaker module stays approximate (FL impulse files)");
  // Runtime: a low-pass module darkens, the soft clip holds the output under 1.
  PluginState lp = m.state;
  lp.params[static_cast<size_t>(dy::DrCount)] = 1;
  for (int k = 0; k < dy::DmP0; ++k) lp.params[static_cast<size_t>(dy::drParam(0, k))] = 0;
  lp.params[static_cast<size_t>(dy::drParam(0, dy::DmKind))] = dr::KFilter;
  lp.params[static_cast<size_t>(dy::drParam(0, dy::DmFilterType))] = 2;
  lp.params[static_cast<size_t>(dy::drParam(0, dy::DmP0 + 39))] = 5000;  // 20·2^5 = 640 Hz
  lp.params[static_cast<size_t>(dy::DrSoftClip)] = 0;
  expect(gainDb(lp, 200) > -1.5 && gainDb(lp, 5000) < -25, "module rack: LP 12 at 640 Hz");
  for (int algo = 0; algo < 18; ++algo) {
    PluginState ds = lp;
    ds.params[static_cast<size_t>(dy::drParam(0, dy::DmKind))] = dr::KDist;
    ds.params[static_cast<size_t>(dy::drParam(0, dy::DmAlgo))] = static_cast<float>(algo);
    ds.params[static_cast<size_t>(dy::drParam(0, dy::DmOs))] = 1;
    ds.params[static_cast<size_t>(dy::DrSoftClip)] = 1;
    Buf in = sine(220, 0.4, 0.8);
    Buf o1 = run(ds, in, 64), o2 = run(ds, in, 777);
    char name[96];
    std::snprintf(name, sizeof name, "module rack: algorithm %d finite, under the soft clip, block-size invariant", algo);
    expect(finite(o1) && peak(o1) <= 1.0 && maxDiff(o1, o2) < 1e-6, name);
  }
  for (int type = 0; type < 23; ++type) {
    PluginState fsx = lp;
    fsx.params[static_cast<size_t>(dy::drParam(0, dy::DmFilterType))] = static_cast<float>(type);
    fsx.params[static_cast<size_t>(dy::drParam(0, dy::DmP0 + 40))] = 6000;
    fsx.params[static_cast<size_t>(dy::drParam(0, dy::DmFilterOpt))] = 1;
    Buf in = noise(0.3, 0.3);
    Buf o1 = run(fsx, in, 64), o2 = run(fsx, in, 1000);
    char name[96];
    std::snprintf(name, sizeof name, "module rack: filter type %d finite, bounded, block-size invariant", type);
    expect(finite(o1) && peak(o1) < 20 && maxDiff(o1, o2) < 1e-6, name);
  }
  renderPresets("Distructor", "distructor: every shipped preset renders finite, bounded and block-size invariant", 1.0001);
}
void testPedalBoard() {
  namespace pb = dy::pedal;
  expect(pb::boxType(5.9f) == 5 && pb::boxType(0.5f) == 0 && near(pb::levelGain(0.5), 1, 1e-12) && near(20 * std::log10(pb::levelGain(1)), 24, 1e-9),
         "pedal board: type and level laws");
  // Treben-style state: one distortion box, no cabinet → exact.
  Blob b;
  for (char c : std::string("1RCH")) b.u8(static_cast<uint8_t>(c));
  float v[50];
  for (float& x : v) x = 0.5f;
  v[2] = 0, v[11] = 0;
  v[13] = 0.5f, v[14] = 0.3475f, v[15] = 0.3375f, v[16] = 0.5f;
  v[39] = 1, v[40] = 0, v[41] = 0, v[42] = 1, v[43] = 1;
  v[45] = 1, v[46] = v[47] = v[48] = v[49] = 0;
  for (float x : v) b.f32(x);
  flp::PluginMapping m;
  expect(decodeFx("Hardcore", b.b, m) && m.exact && P(m, dy::RkModel) == 2 && near(P(m, dy::PbValue0 + 14), 0.3475, 1e-6),
         "hardcore: decodes exactly (no cabinet)");
  PluginState st = m.state;
  Buf in = sine(330, 0.5, 0.3);
  Buf o1 = run(st, in, 64), o2 = run(st, in, 1000);
  expect(finite(o1) && peak(o1) > 0.05 && maxDiff(o1, o2) < 1e-6, "pedal board: distortion box sounds, block-size invariant");
  st.params[static_cast<size_t>(dy::PbValue0 + 11)] = 4;
  expect(decodeFx("Hardcore", b.b, m), "hardcore: decodes again");
  for (int t = 1; t < pb::TCount; ++t) {
    PluginState s2 = m.state;
    s2.params[static_cast<size_t>(dy::PbValue0 + 45)] = static_cast<float>(t);
    s2.params[static_cast<size_t>(dy::PbValue0 + 2)] = 1;
    s2.params[static_cast<size_t>(dy::PbValue0 + 11)] = 3;
    Buf nz = noise(0.4, 0.3);
    Buf a = run(s2, nz, 64), c = run(s2, nz, 1000);
    char name[96];
    std::snprintf(name, sizeof name, "pedal board: pedal %d finite, bounded, block-size invariant", t);
    expect(finite(a) && peak(a) < 20 && maxDiff(a, c) < 1e-6, name);
  }
  // Every shipped .hdprg program renders.
  const fs::path dir("/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Patches/Plugin presets/Effects/Hardcore");
  if (fs::exists(dir)) {
    int n = 0, ok = 0, exact = 0;
    Buf nz = noise(0.3, 0.3, 5);
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
      if (e.path().extension() != ".hdprg") continue;
      std::vector<uint8_t> raw;
      FILE* f = std::fopen(e.path().string().c_str(), "rb");
      if (!f) continue;
      uint8_t buf[4096];
      size_t k;
      while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) raw.insert(raw.end(), buf, buf + k);
      std::fclose(f);
      ++n;
      flp::PluginMapping pm;
      if (!decodeFx("Hardcore", raw, pm) || P(pm, dy::RkModel) != 2) continue;
      exact += pm.exact;
      Buf a = run(pm.state, nz, 64), c = run(pm.state, nz, 1000);
      if (finite(a) && peak(a) < 20 && maxDiff(a, c) < 1e-6) ++ok;
      else std::printf("# %s: peak %.3f diff %.2g\n", e.path().filename().string().c_str(), peak(a), maxDiff(a, c));
    }
    std::printf("# Hardcore programs: %d (%d exact), %d render cleanly\n", n, exact, ok);
    expect(n > 0 && ok == n, "hardcore: every shipped program decodes and renders cleanly");
  }
}

// ===================================================================== FL Studio Mobile Rack FX
void testMobileRack() {
  if (!haveFl()) return;
  int n = 0, exact = 0, reverb = 0, multi = 0;
  for (const auto& e : fs::recursive_directory_iterator(fs::path(kFlFx) / "FL Studio Mobile Rack FX")) {
    if (e.path().extension() != ".fst") continue;
    flp::PresetResult r;
    if (!flp::importFlPreset(e.path().string(), r) || r.kind != flp::PresetResult::Plugin) continue;
    ++n;
    exact += r.exact;
    // J-42: every rack decodes into the Pocket Rack with its modules' stored parameters.
    if (r.state.type == "gl.pocketrack" && P(r.state, dy::pkParam(0, dy::PkType)) == 35) {
      ++reverb;
      expect(std::fabs(P(r.state, dy::pkParam(0, dy::PkP0 + 1)) - 0.22561) < 1e-4 && P(r.state, dy::pkParam(0, dy::PkMix)) == 1,
             ("mobile rack reverb module: parameters kept (" + e.path().filename().string() + ")").c_str());
    }
    if (r.state.type == "gl.pocketrack" && P(r.state, dy::pkParam(0, dy::PkType)) == 4) ++multi;
  }
  std::printf("# FL Studio Mobile Rack FX: %d presets, %d exact\n", n, exact);
  expect(n == 31 && exact >= 29 && reverb == 2 && multi == 1, "mobile rack: module racks decoded (29 of 31 exact: reference module engines)");
}

// ===================================================================== Delay Bank laws, Flangus contour
void testDelayBankFlangus() {
  namespace sf = gl::packs::spacefx;
  double m[4];
  sf::flc::mdPanMatrix(0, 0.7, 0, false, m);
  bool ok = m[0] == 0.7 && m[1] == 0.7 && m[2] == 0 && m[3] == 0;
  sf::flc::mdPanMatrix(1, 1, 0, false, m);  // hard right: the left side folds into the right
  ok = ok && std::fabs(m[0]) < 1e-6 && std::fabs(m[2]) < 1e-6 && near(m[1], 0.70710678, 1e-6) && near(m[3], 0.70710678, 1e-6);
  sf::flc::mdPanMatrix(0, 1, 1, true, m);  // full separation: mono feedback
  ok = ok && near(m[0], 0.5, 1e-6) && near(m[1], 0.5, 1e-6) && near(m[2], 0.5, 1e-6) && near(m[3], 0.5, 1e-6);
  expect(ok, "delay bank: pan / separation matrices (constant power × √2 with fold)");
  expect(near(sf::flc::mdCutHz(1, 0), 44100 * 0.003, 1e-9), "delay bank: filter scale 44100 Hz per unit");
  if (haveFl()) {  // J-42: the 2x oversampled preset too (reference resampler)
    int n = 0, ex = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(kFlFx) / "Fruity Delay Bank")) {
      if (e.path().extension() != ".fst") continue;
      flp::PresetResult r;
      ++n;
      if (flp::importFlPreset(e.path().string(), r) && r.exact) ++ex;
    }
    expect(n == 21 && ex == 21, "delay bank: all 21 shipped presets exact (J-42: oversampling)");
  }
  // Flangus: decodes exactly; the first echo of an impulse comes at the minimum delay.
  Blob b;
  b.ints({1, 512, 768, 8, 0, 0, 0, 128});  // one voice per side, no spread, dry 0, wet 1
  flp::PluginMapping fm;
  expect(decodeFx("Fruity Flangus", b.b, fm) && fm.exact && P(fm, sf::SwModel) == 1, "flangus: decodes exactly");
  Buf in;
  in.L.assign(24000, 0.f), in.R.assign(24000, 0.f);
  in.L[0] = in.R[0] = 1.f;
  Buf o = run(fm.state, in);
  size_t first = 0;
  for (size_t i = 0; i < o.L.size() && !first; ++i)
    if (std::fabs(o.L[i]) > 1e-4) first = i;
  // min delay = max(441·(11^(8/128) − 1)·48000/44100, 8) samples at 48 kHz
  const double minD = std::max(441.0 * (std::pow(11.0, 8.0 / 128) - 1) * 48000 / 44100, 8.0);
  char name[128];
  std::snprintf(name, sizeof name, "flangus: the contour starts at the minimum delay (%zu vs %.1f samples)", first, minD);
  expect(first > 0 && std::fabs(static_cast<double>(first) - minD) <= 2, name);
  Buf nz = noise(0.5, 0.3);
  expect(maxDiff(run(fm.state, nz, 64), run(fm.state, nz, 1000)) < 1e-6, "flangus: block-size invariant");
  presetsExact("Fruity Flangus", "gl.swirl");
}

int main() {
  testEmphasis();
  testFullReverb();
  testDelay3();
  testMaximus();
  testModuleRack();
  testPedalBoard();
  testMobileRack();
  testDelayBankFlangus();
  return finish();
}

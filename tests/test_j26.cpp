// Gridline — J-26: effects exactness tier D. Reference engines and exact
// decoders for the classic reverb, frequency shifter, squeeze, multi effect,
// overdrive, multi-tap filter, quick low-pass, free filter and bass boost
// (studies: analysis/demos/specs/j26_*.md). FL preset checks run only when an
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
#include "../src/engine/LowLifterRef.h"
#include "../src/engine/PlateReverb.h"
#include "../src/packs/dynamics/dynamics.h"
#include "../src/packs/dynamics/effector_ref.h"
#include "../src/packs/spacefx/hyperchorus_ref.h"
#include "../src/packs/spacefx/spacefx.h"
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
double rms(const std::vector<float>& v, size_t from = 0, size_t to = 0) {
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
double gainDb(const PluginState& s, double hz) {
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
}  // namespace

// ===================================================================== Reeverb v1 → plate
void testReeverb1() {
  flp::PluginMapping m;
  Blob b;
  b.ints({1, 1231, 10682, 0, 26345, 65536, 20, 4614, 10879, 65536, 13238});
  expect(decodeFx("Fruity Reeverb", b.b, m) && m.glType == "gl.reverb" && m.exact, "reeverb v1: decodes exactly");
  expect(P(m, pp::RvbModel) == 1 && near(P(m, pp::RvbLowCut), 74, 1e-6) && near(P(m, pp::RvbHighCut), 4000, 1e-6) &&
             near(P(m, pp::RvbSize), 0.40, 1e-6) && near(P(m, pp::RvbDecayTime), 1.5, 1e-5) && near(P(m, pp::RvbDampHz), 4000, 1e-6),
         "reeverb v1: low cut 74 Hz, high cut 4 kHz, size 40, decay 1.5 s, damping 4 kHz");
  expect(near(P(m, pp::RvbBassMult), 1.0, 1e-9) && near(P(m, pp::RvbWet), 13238 / 65536.0, 1e-6) && near(P(m, pp::RvbDry), 1, 1e-6),
         "reeverb v1: flat colour, linear wet / dry");
  expect(plate::colourStep(5) == 0 && plate::colourStep(15) == 2 && plate::colourStep(25) == 2 && plate::colourStep(35) == 4,
         "reeverb v1: colour steps round half to even");
  Blob f;
  for (int k = 0; k < 10; ++k) f.f32(0.5f);  // a state FL ignores (first word above version 1)
  expect(decodeFx("Fruity Reeverb", f.b, m) && m.exact && near(P(m, pp::RvbSize), plate::sizeStep(26000) / 100.0, 1e-6),
         "reeverb v1: unreadable versions load the plugin defaults");
  // Plate DSP: an impulse rings out finitely, longer decay → longer tail.
  auto tail = [](float decay) {
    PluginState s = state("gl.reverb", {{pp::RvbModel, 1.f}, {pp::RvbDecayTime, decay}, {pp::RvbDry, 0.f}, {pp::RvbWet, 1.f}});
    Buf b;
    b.L.assign(static_cast<size_t>(3 * kSr), 0.f), b.R = b.L;
    b.L[0] = b.R[0] = 1;
    Buf o = run(s, b);
    return std::make_pair(finite(o), rms(o.L, static_cast<size_t>(1.0 * kSr), static_cast<size_t>(1.5 * kSr)));
  };
  auto shortT = tail(0.4f), longT = tail(5.f);
  expect(shortT.first && longT.first && longT.second > 20 * shortT.second && longT.second > 1e-5, "plate: decay time sets the tail");
  PluginState s = state("gl.reverb", {{pp::RvbModel, 1.f}});
  Buf in = noise(0.5, 0.3);
  expect(maxDiff(run(s, in, 64), run(s, in, 1000)) < 1e-6, "plate: block-size invariant");
  expect(peak(run(s, Buf{std::vector<float>(48000, 0.f), std::vector<float>(48000, 0.f)})) < 1e-6, "plate: silence in, silence out");
  presetsExact("Fruity Reeverb", "gl.reverb");
}

// ===================================================================== Frequency Shifter
void testFreqShift() {
  expect(near(fsref::shiftHz((51542 + 70000) / 140000.0, 0, 1, 120), 800.25, 0.05) &&
             near(fsref::shiftHz((-51537 + 70000) / 140000.0, 0, 1, 120), -799.55, 0.05),
         "freq shift: wide range law ±0.1·200000^x (+800 / -800 Hz presets)");
  expect(near(fsref::shiftHz(1.0, 0, 0, 120), 200, 1e-6) && fsref::shiftHz(0.5, 0, 1, 120) == 0, "freq shift: narrow range tops at 200 Hz, centre is 0 Hz");
  expect(near(fsref::tempoPeriodBeats(2 * std::fabs((29990 + 70000) / 140000.0 - 0.5)), 4.0, 1e-9), "freq shift: tempo period steps (4 beats)");
  flp::PluginMapping m;
  Blob b;
  b.ints({4, 10000, 0, 51542, 0, 10000, 10000, 5000, 10000, 0, 5000, 0, 0});
  expect(decodeFx("Frequency Shifter", b.b, m) && m.exact && P(m, pp::FsModel) == 1 && P(m, pp::FsType) == 1 && P(m, pp::FsRange) == 1,
         "freq shift: decodes exactly (FL HQ → HQ, FL wide → wide)");
  // A 1 kHz tone shifted up by 200 Hz (narrow range, knob at the top, wet only).
  PluginState s = state("gl.freqshift", {{pp::FsModel, 1.f}, {pp::FsType, 0.f}, {pp::FsRange, 0.f}, {pp::FsFreqPos, 1.f}, {pp::FsMixRef, 1.f}});
  Buf o = run(s, sine(1000, 1.0, 0.4));
  size_t from = o.L.size() / 2;
  double up = tone(o.L, 1200, from, o.L.size()), at = tone(o.L, 1000, from, o.L.size()), down = tone(o.L, 800, from, o.L.size());
  std::printf("# shifted: 1200 Hz %.3f, 1000 Hz %.3f, 800 Hz %.3f\n", up, at, down);
  expect(finite(o) && up > 0.3 && at < 0.02 && down < 0.03, "freq shift: one sideband, shifted by the law's amount");
  PluginState hq = s;
  hq.params[pp::FsType] = 1;
  Buf oh = run(hq, sine(1000, 1.0, 0.4));
  expect(finite(oh) && tone(oh.L, 1200, from, oh.L.size()) > 0.3, "freq shift: HQ type shifts too");
  PluginState ring = state("gl.freqshift", {{pp::FsModel, 1.f}, {pp::FsType, 2.f}, {pp::FsRange, 0.f}, {pp::FsFreqPos, 1.f}, {pp::FsMixRef, 1.f}});
  Buf orr = run(ring, sine(1000, 1.0, 0.4));
  expect(tone(orr.L, 1200, from, orr.L.size()) > 0.1 && tone(orr.L, 800, from, orr.L.size()) > 0.1, "freq shift: ring modulator makes both sidebands");
  Buf in = noise(0.5, 0.3);
  expect(maxDiff(run(s, in, 64), run(s, in, 777)) < 1e-6, "freq shift: block-size invariant");
  legacyKept("gl.freqshift", pp::FsModel);
  presetsExact("Frequency Shifter", "gl.freqshift");
}

// ===================================================================== Squeeze
void testSqueeze() {
  expect(near(dy::sqStepsOfRaw(28417), 3.98, 0.01) && near(dy::sqStepsOfRaw(12144), 695, 2) && near(dy::sqMsOfRaw(7500), 0.117, 0.002) &&
             near(dy::sqMsOfRaw(15948), 1.0616, 0.001),
         "squeeze: step and length laws");
  flp::PluginMapping m;
  Blob b;
  b.ints({0, 93, 108, 28417, 1, 4, 46592, 23552, 1, 0, 2892, 4684, 0});
  expect(decodeFx("Fruity Squeeze", b.b, m) && m.exact && P(m, dy::SqModel) == 1 && P(m, dy::SqFilterPos) == 1 &&
             near(P(m, dy::SqGainRef), 93 / 127.0, 1e-6) && near(P(m, dy::SqMix), 108 / 128.0, 1e-6),
         "squeeze: decodes exactly (post filter stays post, linear gain)");
  PluginState d = defaultPluginState("gl.squeeze");
  Buf in = sine(220, 0.3, 0.4);
  expect(maxDiff(run(d, in), in) < 1e-6, "squeeze: default is transparent");
  PluginState q = state("gl.squeeze", {{dy::SqStepsRef, 2.f}, {dy::SqMix, 1.f}});
  Buf o = run(q, in);
  bool levels = true;
  for (float v : o.L) levels = levels && near(v * 2, std::nearbyint(v * 2), 1e-6);
  expect(levels, "squeeze: quantiser rounds to 1/S");
  PluginState g = state("gl.squeeze", {{dy::SqGainRef, 0.f}, {dy::SqMix, 0.f}});
  expect(peak(run(g, in)) < 1e-9, "squeeze: gain acts after the mix (dry included)");
  PluginState gate = state("gl.squeeze", {{dy::SqImpactMs, 5.f}, {dy::SqPreserveMs, 5.f}, {dy::SqAmountRef, 0.f}, {dy::SqMix, 1.f}});
  Buf og = run(gate, Buf{std::vector<float>(48000, 0.5f), std::vector<float>(48000, 0.5f)});
  size_t zeros = 0;
  for (float v : og.L) zeros += v == 0.f;
  expect(zeros > 18000 && zeros < 30000, "squeeze: puncher gates periodically (impact at the amount, preserve at unity)");
  legacyKept("gl.squeeze", dy::SqModel);
  presetsExact("Fruity Squeeze", "gl.squeeze");
}

// ===================================================================== Effector
void testEffector() {
  expect(near(dy::efref::lengthBeats(1.0 / 8, 0), 0.25, 1e-12) && near(dy::efref::lengthBeats(1, 0.5), 2, 1e-12) &&
             dy::efref::kindOf(0.5455) == 6 && dy::efref::kindOf(0.9091) == 10 && near(dy::efref::outGain(0.5), 1, 1e-12) &&
             near(dy::efref::modAmount(0.25), -0.25, 1e-12),
         "effector: rate, tempo reduce, kind, output and modulation laws");
  flp::PluginMapping m;
  Blob b;
  b.b = {'S', 'S', 'L', 'F'};
  b.i32(8 + 48).b.insert(b.b.end(), {'1', '0', 'X', 'F', 0, 0, 0, 0});
  const float st[12] = {0.7273f, 0, 1, 0.4453f, 0.4531f, 0.625f, 0, 0.5f, 0.5f, 0, 1, 0.5f};
  for (float v : st) b.f32(v);
  expect(decodeFx("Effector", b.b, m) && m.exact && P(m, dy::EfModel) == 1, "effector: decodes exactly");
  bool same = true;
  for (int k = 0; k < 12; ++k) same = same && near(P(m, dy::EfRawKind + k), st[k], 1e-6);
  expect(same, "effector: the twelve stored values kept verbatim");
  bool ok = true, audible = true, inv = true;
  Buf in = noise(0.6, 0.3), tone1 = sine(330, 0.6, 0.3);
  for (int kind = 0; kind < 12; ++kind) {
    PluginState s = state("gl.effector", {{dy::EfRawKind, static_cast<float>(kind / 11.0)}, {dy::EfRawX, 0.7f}, {dy::EfRawY, 0.4f},
                                          {dy::EfRawModX, 0.8f}, {dy::EfRawRate, 0.25f}});
    Buf o = run(s, in, 256);
    ok = ok && finite(o) && peak(o) < 8;
    Buf ot = run(s, tone1, 256);
    bool aud = maxDiff(ot, tone1) > 0.005;
    if (!aud) std::printf("# effector kind %d inaudible\n", kind);
    audible = audible && aud;
    inv = inv && maxDiff(o, run(s, in, 1000)) < 1e-5;
  }
  expect(ok, "effector: all twelve modules finite and bounded");
  expect(audible, "effector: all twelve modules change the sound");
  expect(inv, "effector: block-size invariant (64-sample control grid)");
  PluginState by = state("gl.effector", {{dy::EfRawBypass, 1.f}, {dy::EfRawIn, 1.f}, {dy::EfRawOut, 0.5f}});
  expect(maxDiff(run(by, in), in) < 1e-6, "effector: bypass with unity in / out is transparent");
  PluginState z = defaultPluginState("gl.effector");
  expect(peak(run(z, Buf{std::vector<float>(24000, 0.f), std::vector<float>(24000, 0.f)})) < 1e-6, "effector: silence in, silence out");
  legacyKept("gl.effector", dy::EfModel);
}

// ===================================================================== Blood Overdrive
void testOverdrive() {
  flp::PluginMapping m;
  Blob b;
  b.ints({3, 8500, 5000, 10000, 0, 5000, 5000, 2, 0});
  expect(decodeFx("Fruity Blood Overdrive", b.b, m) && m.exact && P(m, dy::OdModel) == 1 && near(P(m, dy::OdPostGainK), 0.5, 1e-6) &&
             near(P(m, dy::OdPreBand), 0.85, 1e-6),
         "overdrive: decodes exactly (post gain as the knob position, theme ignored)");
  // Small signal, no drive: three bands summed through sin(πx/2) ≈ (π/2)·x, then the post low-passes.
  PluginState s = state("gl.overdrive", {{dy::OdPreAmp, 0.f}, {dy::OdPreBand, 0.f}, {dy::OdPostFilter, 0.f}, {dy::OdPostGainK, 1.f}});
  Buf o = run(s, sine(60, 1.0, 0.05));
  double ratio = rms(o.L, 24000) / (0.05 / std::sqrt(2.0));
  std::printf("# overdrive small-signal gain %.3f\n", ratio);
  expect(near(ratio, 1.5708, 0.08), "overdrive: quarter-sine shaper small-signal gain π/2");
  PluginState hot = state("gl.overdrive", {{dy::OdPreAmp, 1.f}, {dy::OdBoost, 1.f}, {dy::OdPostGainK, 1.f}});
  Buf oh = run(hot, noise(0.5, 0.9));
  expect(finite(oh) && peak(oh) <= 0.99995 + 1e-6, "overdrive: output limited to ±0.99995");
  PluginState mute = state("gl.overdrive", {{dy::OdPostGainK, 0.f}});
  expect(peak(run(mute, noise(0.5, 0.5)), 24000) < 1e-3, "overdrive: post gain 0 is silence (n² law)");
  legacyKept("gl.overdrive", dy::OdModel);
  presetsExact("Fruity Blood Overdrive", "gl.overdrive");
}

// ===================================================================== Fruity Filter → classic model
void testFilter() {
  expect(near(flref::filterCutCoef(1), 1, 1e-6) && near(flref::filterLevel(0.5), 0.2317, 1e-4) && near(flref::filterDamping(1), 0.2, 1e-6),
         "filter: cutoff, level and damping laws");
  flp::PluginMapping m;
  Blob b;
  b.ints({2, 1024, 205, 1024, 0, 512, 0}).u8(1);
  expect(decodeFx("Fruity Filter", b.b, m) && m.exact && P(m, pp::FltModel) == 1 && P(m, pp::FltDcBlock) == 1 && P(m, pp::FltSlope) == 0 &&
             near(P(m, pp::FltResK), 205 / 1024.0, 1e-6),
         "filter: decodes exactly (centre byte, x2 off)");
  Blob v1;
  v1.ints({1, 480, 1024, 0, 0, 1024, 1});
  expect(decodeFx("Fruity Filter", v1.b, m) && m.exact && P(m, pp::FltSlope) == 1 && P(m, pp::FltDcBlock) == 0, "filter: version 1, x2 on");
  PluginState lp = state("gl.filter", {{pp::FltModel, 1.f}, {pp::FltCutK, 0.3f}, {pp::FltResK, 0.f}, {pp::FltLPLevel, 1.f}, {pp::FltBPLevel, 0.f},
                                       {pp::FltHPLevel, 0.f}, {pp::FltDcBlock, 0.f}});
  double lo = gainDb(lp, 100), hi = gainDb(lp, 8000);
  std::printf("# classic LP: 100 Hz %.2f dB, 8 kHz %.2f dB\n", lo, hi);
  expect(near(lo, 0, 0.5) && hi < -20, "filter: classic low pass");
  PluginState dc = lp;
  dc.params[pp::FltCutK] = 1, dc.params[pp::FltDcBlock] = 1;
  Buf o = run(dc, Buf{std::vector<float>(96000, 0.5f), std::vector<float>(96000, 0.5f)});
  expect(std::fabs(o.L.back()) < 1e-3, "filter: centre removes DC");
  presetsExact("Fruity Filter", "gl.filter");
}

// ===================================================================== Fast LP / Free Filter / Bass Boost
void testSmallFilters() {
  // Fast LP
  expect(flref::quickLpCoef(1) == 1.f && flref::quickLpRes(0) == 0.f && near(flref::quickLpRes(1), 0.98330, 1e-4), "fast LP: coefficient laws");
  PluginState open = defaultPluginState("gl.fastlp");
  Buf in = noise(0.3, 0.4);
  expect(maxDiff(run(open, in), in) < 1e-6, "fast LP: fully open is transparent");
  PluginState lp = state("gl.fastlp", {{ut::QlCutK, 0.5f}});
  expect(gainDb(lp, 8000) < gainDb(lp, 100) - 15, "fast LP: lower cutoff damps highs");
  flp::PluginMapping m;
  Blob b;
  b.ints({1, 137, 138});
  expect(decodeFx("Fruity Fast LP", b.b, m) && m.exact && near(P(m, ut::QlCutK), 137 / 1024.0, 1e-6), "fast LP: decodes exactly");
  legacyKept("gl.fastlp", ut::QlModel);
  // Free Filter
  expect(flref::freeFilterType((250 - 100) / 600.0) == 1 && flref::freeFilterType((375 - 100) / 600.0) == 3 &&
             flref::freeFilterType((300 - 100) / 600.0) == 2 && flref::freeFilterType(1) == 6,
         "free filter: discrete type law (250 band pass, 300 high pass, 375 notch)");
  expect(near(flref::freeFilterHz(0.5), 1457, 2) && near(flref::freeFilterDb(676 / 1024.0), 5.77, 0.01), "free filter: frequency and gain laws");
  PluginState ff = state("gl.freefilter", {{ut::FfType, 0.f}, {ut::FfFreq, 1000.f}, {ut::FfQK, 0.032f}, {ut::FfDc, 0.f}});
  // The design assumes 44.1 kHz: at 48 kHz the corner lands at 1000 · 48000 / 44100 Hz.
  expect(near(gainDb(ff, 1000 * 48000.0 / 44100.0), -3.0, 0.5) && gainDb(ff, 10000) < -30, "free filter: fixed 44.1 kHz design");
  legacyKept("gl.freefilter", ut::FfModel);
  // Bass Boost
  expect(flref::bassBoostHz(0.5) == 60 && flref::bassBoostHz(1) == 100 && near(flref::bassBoostGain(0.75), 2.52, 0.01), "bass boost: frequency and gain laws");
  PluginState bb = state("gl.bassboost", {{ut::BbFreq, 60.f}, {ut::BbAmount, 1.f}});
  expect(near(gainDb(bb, 15), 20 * std::log10(6.0), 0.6) && near(gainDb(bb, 5000), 0, 0.2), "bass boost: lows lifted by 1 + G, highs untouched");
  legacyKept("gl.bassboost", ut::BbModel);
  presetsExact("Fruity Free Filter", "gl.freefilter");
  presetsExact("Fruity Bass Boost", "gl.bassboost");
}


// ===================================================================== Phaser / Flanger
void testPhaserFlanger() {
  expect(near(classic::phaserRateHz(0.3, false), 0.08657, 1e-4) && classic::phaserStages(11) == 12 && classic::phaserStages(0) == 3,
         "phaser: rate law (small range) and stage count");
  flp::PluginMapping m;
  Blob b;
  b.ints({1, 1500, 0, 900, 0, 512, 11, 693, 0, 2500});
  expect(decodeFx("Fruity Phaser", b.b, m) && m.exact && P(m, pp::PhsModel) == 1 && P(m, pp::PhsStagesN) == 12 && near(P(m, pp::PhsDryN), 0, 1e-9),
         "phaser: decodes exactly");
  // Fixed coefficient (min = max): a static all-pass chain has unity magnitude → wet at the gain law.
  PluginState ap = state("gl.phaser", {{pp::PhsDepthMin, 0.5f}, {pp::PhsDepthMax, 0.5f}, {pp::PhsFeedbackN, 0.f}, {pp::PhsDryN, 0.f}, {pp::PhsGainN, 0.5f}});
  expect(near(gainDb(ap, 1000), 0, 0.2), "phaser: all-pass chain at unity gain (G = 2 n)");
  PluginState mix = ap;
  mix.params[pp::PhsDryN] = 0.5f;
  double gmin = 99, gmax = -99;
  for (double f = 200; f < 16000; f *= 1.12) gmin = std::min(gmin, gainDb(mix, f)), gmax = std::max(gmax, gainDb(mix, f));
  std::printf("# phaser dry + wet: %.1f .. %.1f dB\n", gmin, gmax);
  expect(gmax - gmin > 12, "phaser: dry + wet makes notches");
  Buf in = noise(0.5, 0.3);
  PluginState d = defaultPluginState("gl.phaser");
  expect(finite(run(d, in)) && maxDiff(run(d, in, 64), run(d, in, 999)) < 1e-6, "phaser: finite, block-size invariant");
  legacyKept("gl.phaser", pp::PhsModel);

  expect(near(classic::flangerMs(1), 20, 1e-9) && near(classic::flangerMs(0.5), 3.3333, 1e-3) && near(classic::flangerRateHz(1), 5, 1e-9),
         "flanger: delay / depth / rate curves");
  Blob f;
  f.ints({1, 0, 2000, 2000, 118, 0, 0, 23, 1024, 1024, 3500, 3000, 2500});
  expect(decodeFx("Fruity Flanger", f.b, m) && m.exact && P(m, pp::FlgModel) == 1 && P(m, pp::FlgInvFb) == 1 && P(m, pp::FlgInvertWet) == 1,
         "flanger: decodes exactly");
  PluginState dry = state("gl.flanger", {{pp::FlgDryN, 1.f}, {pp::FlgWetN, 0.f}, {pp::FlgCrossN, 0.f}});
  expect(maxDiff(run(dry, in), in) < 1e-6, "flanger: dry only is transparent");
  PluginState wet = state("gl.flanger", {{pp::FlgDryN, 0.f}, {pp::FlgWetN, 1.f}, {pp::FlgCrossN, 0.f}, {pp::FlgInvertWet, 0.f}, {pp::FlgFeedN, 0.f},
                                         {pp::FlgDelayN, 0.5f}, {pp::FlgDepthN, 0.f}});
  Buf imp;
  imp.L.assign(4800, 0.f), imp.R = imp.L;
  imp.L[0] = imp.R[0] = 1;
  Buf o = run(wet, imp);
  size_t at = 0;
  for (size_t i = 0; i < o.L.size(); ++i)
    if (std::fabs(o.L[i]) > std::fabs(o.L[at])) at = i;
  std::printf("# flanger wet impulse at %zu samples (expected %.1f)\n", at, 48000 * 3.3333 / 1000);
  expect(at == 160, "flanger: wet path delayed by the delay curve (3.33 ms at 48 kHz)");
  expect(maxDiff(run(defaultPluginState("gl.flanger"), in, 64), run(defaultPluginState("gl.flanger"), in, 1000)) < 1e-6, "flanger: block-size invariant");
  legacyKept("gl.flanger", pp::FlgModel);
  presetsExact("Fruity Phaser", "gl.phaser");
  presetsExact("Fruity Flanger", "gl.flanger");
}

// ===================================================================== Parametric EQ v1 → classic band model
void testPeq1() {
  flp::PluginMapping m;
  Blob b;
  b.ints({0, 0, 0, 0, 0, 0, 0, 5777, 13024, 19904, 33145, 42327, 49307, 55825, 17500, 17500, 17500, 17500, 24000, 24000, 17500,
          5, 6, 6, 6, 6, 6, 7, 128});
  expect(decodeFx("Fruity Parametric EQ", b.b, m) && m.exact && P(m, pp::EqModel) == 2 && near(P(m, pp::EqMainLin), 1, 1e-9) &&
             near(P(m, pp::eqParam(0, pp::EqFreq)), 90.3, 0.1),
         "peq v1: decodes exactly (classic model, band 1 at 90.3 Hz)");
  Blob s;
  s.ints({0, 0, 0, 1200, 0, 0, 0, 5777, 13024, 19904, 32768, 42327, 49307, 55825, 17500, 17500, 17500, 17500, 24000, 24000, 17500,
          5, 6, 6, 6, 6, 6, 7, 128});
  expect(decodeFx("Fruity Parametric EQ", s.b, m), "peq v1: boosted band decodes");
  const double hz = flref::freeFilterHz(32768 / 65536.0);
  std::printf("# peq v1 band 4 at %.1f Hz: %.2f dB\n", hz, gainDb(m.state, hz));
  expect(near(gainDb(m.state, hz), 12, 0.3) && near(gainDb(m.state, 40), 0, 0.3), "peq v1: +12 dB peak at the band frequency");
  PluginState silent = m.state;
  silent.params[pp::EqMainLin] = 0;
  expect(peak(run(silent, noise(0.2, 0.5))) < 1e-9, "peq v1: main level 0 is silence");
  presetsExact("Fruity Parametric EQ", "gl.eq");
}


// ===================================================================== Fruity Delay / Delay 2 → classic tempo delay
void testClassicDelay() {
  flp::PluginMapping m;
  Blob b1;
  b1.ints({1, 338, 512, 1024, 0, 3, 0});
  expect(decodeFx("Fruity Delay", b1.b, m) && m.exact && P(m, pp::DlyModel) == 2 && near(P(m, pp::DlyTicks), 144, 1e-9) &&
             near(P(m, pp::DlyClsFb), 0.99 * 0.5, 1e-6) && near(P(m, pp::DlyClsCoef), 1, 1e-6) && P(m, pp::DlyClsBpm) == 0,
         "delay v1: decodes exactly (3 sixteenths, host tempo, open filter)");
  Blob b1b;
  b1b.ints({1, 642, 240, 720, 1024, 1, 0});
  expect(decodeFx("Fruity Delay", b1b.b, m) && P(m, pp::DlyClsBpm) == 280 && near(P(m, pp::DlyClsCoef), cdelay::coefOf(720 / 1024.0), 1e-6),
         "delay v1: own tempo round(220 n + 60), coefficient law");
  Blob b2;
  b2.ints({106, 103, 128, 23, 6, -394, 2, 128});
  expect(decodeFx("Fruity Delay 2", b2.b, m) && m.exact && P(m, pp::DlyModel) == 2 && P(m, pp::DlyClsMode) == 2 &&
             near(P(m, pp::DlyClsIn), (std::pow(11.0, 103 / 128.0) - 1) / 10, 1e-5) && near(P(m, pp::DlyClsDry), 1, 1e-9),
         "delay 2: decodes exactly (volume law, ping-pong)");
  auto impulse = [](const PluginState& s, double tempo) {
    Buf b;
    b.L.assign(48000, 0.f), b.R = b.L;
    b.L[0] = 1;
    return run(s, b, 256, tempo);
  };
  PluginState d1 = state("gl.delay", {{pp::DlyModel, 2.f}, {pp::DlyTicks, 48.f}, {pp::DlyClsStepRound, 1.f}, {pp::DlyClsFb, 0.5f},
                                       {pp::DlyClsCoef, 1.f}, {pp::DlyClsScaleSr, 0.f}, {pp::DlyClsDry, 1.f}, {pp::DlyClsIn, 1.f}});
  Buf o = impulse(d1, 120);
  expect(near(o.L[6000], 1, 1e-6) && near(o.L[12000], 0.5, 1e-6) && near(o.L[0], 1, 1e-6) && std::fabs(o.L[5999]) < 1e-9,
         "classic delay: one sixteenth at 120 BPM = 6000 samples, feedback halves each repeat");
  PluginState pp2 = d1;
  pp2.params[pp::DlyClsMode] = 2;
  Buf op = impulse(pp2, 120);
  expect(near(op.R[6000], 1, 1e-6) && std::fabs(op.L[6000]) < 1e-9 && near(op.L[12000], 0.5, 1e-6), "classic delay: ping-pong alternates sides");
  PluginState lp = d1;
  lp.params[pp::DlyClsCoef] = 0.1f;
  Buf ol = impulse(lp, 120);
  expect(near(ol.L[6000], 0.1, 1e-6), "classic delay: the filter colours the first echo (input goes through it)");
  PluginState d2 = d1;
  d2.params[pp::DlyClsStepRound] = 0, d2.params[pp::DlyTicks] = 144;
  Buf o2 = impulse(d2, 120);
  expect(near(o2.L[18000], 1, 1e-6), "classic delay: ticks at the host tempo (144 ticks at 120 BPM = 18000 samples)");
  presetsExact("Fruity Delay", "gl.delay");
  presetsExact("Fruity Delay 2", "gl.delay");
}


// ===================================================================== Low Lifter
void testLowLifter() {
  expect(near(llref::lowDb(0.4999), 0.0, 0.02) && near(llref::cutHz(0.892), 99.5, 0.2) && near(llref::slopeDb(0.6993), -6.53, 0.02) &&
             llref::lowDb(0) < -1e8,
         "low lifter: knob laws (0 dB, 99.5 Hz, -6.53 dB; raw 0 = off)");
  flp::PluginMapping m;
  Blob b;
  b.ints({6, 1, 10000, 4999, 6993, 1, 0, 4998, 0, 5000, 10000, 8920, 0, 0, 1, 1});
  expect(decodeFx("Low Lifter", b.b, m) && m.exact && P(m, pp::LlModel) == 1 && near(P(m, pp::LlPreDb), 9.0, 0.01) &&
             near(P(m, pp::LlXoverHz), 99.5, 0.2) && P(m, pp::LlLookahead) == 1,
         "low lifter: decodes exactly");
  PluginState d = m.state;
  auto fx = createEffect("gl.lowlift");
  expect(fx->latencyFor(d, 48000) == 480, "low lifter: 10 ms look-ahead reported as latency");
  Buf loud = run(d, sine(50, 1.0, 1.5));
  expect(finite(loud) && peak(loud, 24000) < 2.5, "low lifter: loud lows stay bounded (envelope normalisation)");
  Buf z;
  z.L.assign(24000, 0.f), z.R = z.L;
  expect(peak(run(d, z)) < 1e-9, "low lifter: silence in, silence out");
  PluginState dry = d;
  dry.params[pp::LlMix] = 0;
  Buf in = sine(1000, 0.5, 0.3), o = run(dry, in);
  const double ratio = rms(o.L, 4000) / rms(in.L, 4000);
  size_t first = 0;
  for (size_t i = 0; i < o.L.size() && first == 0; ++i)
    if (std::fabs(o.L[i]) > 1e-4) first = i;
  std::printf("# low lifter dry path: level ratio %.4f, first output at %zu\n", ratio, first);
  expect(near(ratio, 1, 0.01) && first >= 480, "low lifter: dry path is the input (all-pass aligned), delayed by the look-ahead");
  Buf o64 = run(d, in, 64), o999 = run(d, in, 999);
  expect(maxDiff(o64, o999) < 1e-6, "low lifter: block-size invariant");
  legacyKept("gl.lowlift", pp::LlModel);
  presetsExact("Low Lifter", "gl.lowlift");
}


// ===================================================================== Emphasizer / Emphasis
void testEmphasizer() {
  flp::PluginMapping m;
  Blob b;
  b.ints({2, 0, 5013, 0, 0, 0, 10000}).u8(0);
  expect(decodeFx("Emphasizer", b.b, m) && m.exact && P(m, dy::EnCustom) == 2 && near(P(m, dy::EnPeakEmph), 0.5013, 1e-6) &&
             near(P(m, dy::EnPeakOut), 1, 1e-9),
         "emphasizer: version 2 decodes exactly onto the peak emphasis model");
  Blob v1;
  v1.ints({1, 0, 5013, 0, 0, 0}).u8(0);
  expect(decodeFx("Emphasizer", v1.b, m) && !m.exact, "emphasizer: version 1 (output default unverified) stays approximate");
  PluginState s = state("gl.enhance", {{dy::EnCustom, 2.f}, {dy::EnPeakIn, 18.f}, {dy::EnPeakEmph, 0.f}});
  Buf o = run(s, sine(200, 1.0, 0.5));
  std::printf("# emphasizer: peak %.4f with +18 dB in\n", peak(o, 24000));
  expect(finite(o) && peak(o, 24000) <= 1.0 + 1e-6 && peak(o, 24000) > 0.9, "emphasizer: limits to the ceiling / clipper");
  PluginState by = state("gl.enhance", {{dy::EnCustom, 2.f}, {dy::EnPeakIn, 18.f}, {dy::EnPeakBypass, 1.f}});
  Buf in = sine(200, 0.5, 0.3), ob = run(by, in);
  double err = 0;
  for (size_t i = 200; i < ob.L.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(ob.L[i] - 0.9999f * in.L[i - 96])));
  expect(err < 1e-4, "emphasizer: bypass passes the 2 ms delayed input");
  auto fx = createEffect("gl.enhance");
  expect(fx->latencyFor(s, 48000) == 96, "emphasizer: 2 ms look-ahead latency");
  // Emphasis: the decoder reads every tag at its real position.
  Blob e;
  e.ints({8, 0, 0, 0, 0, 1, 0, 2500, 7480, 10000, 9900, 0, 1, 8, 0, 0});
  expect(decodeFx("Emphasis", e.b, m) && near(P(m, dy::EmInput), 6.0, 1e-6) && P(m, dy::EmStereoMode) == 1 && near(P(m, dy::EmLink), 0.748, 1e-6) &&
             P(m, dy::EmOversample) == 1,
         "emphasis: input, M/S, link and oversampling from their own tags");
}


// ===================================================================== Hyper Chorus
void testHyperChorus() {
  namespace sf = gl::packs::spacefx;
  expect(near(sf::hcref::lowPassHz(1), 22000, 1) && near(sf::hcref::lowPassHz(0), 714, 1e-6) && sf::hcref::highPassHz(0) == 0 &&
             near(sf::hcref::highPassHz(1), 1090, 1) && near(sf::hcref::wetShare(0.5), 0.5, 1e-12) && near(sf::hcref::wetShare(0.25), 0.125, 1e-12),
         "hyper chorus: filter and mix laws");
  flp::PluginMapping m;
  Blob b;
  b.ints({2, 476, 644, 273, 689, 1000, 1000, 0, 22000, 1974, 900, 500, 0});
  expect(decodeFx("Hyper Chorus", b.b, m) && m.exact && P(m, sf::EnModel) == 1 && near(P(m, sf::EnK8), (1974 - 120) / 10780.0, 1e-6) &&
             near(P(m, sf::EnK7), 1, 1e-6) && near(P(m, sf::EnK9), 0.9, 1e-6),
         "hyper chorus: decodes exactly (filter values are knob positions)");
  PluginState dry = state("gl.ensemble", {{sf::EnK9, 0.f}});
  Buf in = noise(0.3, 0.3);
  expect(maxDiff(run(dry, in), in) < 1e-6, "hyper chorus: mix 0 is dry");
  PluginState d = defaultPluginState("gl.ensemble");
  Buf o = run(d, in);
  expect(finite(o) && peak(o) < 2 && maxDiff(o, in) > 0.01, "hyper chorus: default chorus is audible and bounded");
  expect(maxDiff(run(d, in, 64), run(d, in, 1000)) < 1e-6, "hyper chorus: block-size invariant");
  legacyKept("gl.ensemble", sf::EnModel);
  presetsExact("Hyper Chorus", "gl.ensemble");
}


// ===================================================================== Distructor (structural, approximate)
void testDistructor() {
  if (!haveFl()) return;
  int n = 0, ok = 0, slots = 0;
  for (const auto& e : fs::recursive_directory_iterator(fs::path(kFlFx) / "Distructor")) {
    if (e.path().extension() != ".fst") continue;
    ++n;
    flp::PresetResult r;
    if (!flp::importFlPreset(e.path().string(), r) || r.state.type != "gl.amprack") continue;
    int used = 0;
    for (int k = 0; k < dy::kRackSlots; ++k) used += P(r.state, dy::rkParam(k, dy::RsType)) != dy::RmEmpty;
    Buf o = run(r.state, noise(0.2, 0.3));
    if (finite(o) && peak(o) < 16) ++ok;
    slots += used;
  }
  std::printf("# Distructor: %d/%d presets render, %d module slots mapped\n", ok, n, slots);
  expect(n > 0 && ok == n && slots >= n, "distructor: every preset maps its modules onto Amp Rack slots and renders");
}

int main() {
  testReeverb1();
  testFreqShift();
  testSqueeze();
  testEffector();
  testOverdrive();
  testFilter();
  testSmallFilters();
  testPhaserFlanger();
  testPeq1();
  testClassicDelay();
  testLowLifter();
  testEmphasizer();
  testHyperChorus();
  testDistructor();
  return finish();
}

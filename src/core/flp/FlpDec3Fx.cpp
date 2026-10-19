// Gridline — FLP import, third wave: effects and controllers whose shipped
// presets had no decoder yet.
//   Fruity Peak Controller  → gl.peakctl     Fruity PanOMatic   → gl.autopan
//   Fruity Bass Boost       → gl.bassboost   Fruity Blood Overdrive → gl.overdrive
//   Fruity Center           → gl.utility     Fruity Spectroman  → gl.spectro
//   Tuner                   → gl.tuner       Fruity 7 Band EQ   → gl.geq
//   Transmitter             → gl.transplit   Multiband Delay    → gl.banddelay
// Stored layouts (observed in the shipped presets and supplied projects):
// see the comment above each decoder. FL parameter index = control tag.
#include <algorithm>
#include <cmath>
#include "../PluginParams.h"
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/spacefx/spacefx.h"
#include "../../packs/utility/utility.h"
#include "FlpDec3.h"

namespace gl::flp::dec3 {

namespace {

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }
// Logarithmic map of v in lo..hi onto a..b.
double logMap(double v, double lo, double hi, double a, double b) {
  double u = clampd((v - lo) / (hi - lo), 0, 1);
  return a * std::pow(b / a, u);
}
// `count` i32 values from byte `at` (missing: 0).
std::vector<double> ints(const Blob& b, size_t at, size_t count) {
  std::vector<double> v(count, 0.0);
  for (size_t i = 0; i < count && b.has(at + 4 * i, 4); ++i) v[i] = b.i32(at + 4 * i);
  return v;
}

// ------------------------------------------------------- Fruity Peak Controller
// 10 × i32 (tags 0..9): peak base 0..65536, peak amount −256..256, peak
// tension −128..128, peak decay 5..128, LFO base 0..65536, LFO amount
// −256..256, LFO tension −128..128, LFO speed 200..65536, LFO shape 0..4
// (sine, triangle, square, saw, random), LFO phase 0..65535; then u8 mute
// output, u8 ramp (smooth) controllers.
namespace mod = gl::packs::modulation;
double peakDecay(double v) { return logMap(v, 5, 128, 0.01, 5.0); }
double lfoHz(double v) { return logMap(v, 200, 65536, 0.01, 20.0); }

bool decodePeakController(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 40) return false;
  gen2::begin(m, "gl.peakctl");
  static const std::vector<Row> rows = {
      {0, 0, 65536, mod::PkBase, [](double v) { return v / 65536.0; }},
      {1, -256, 256, mod::PkAmount, [](double v) { return v / 256.0; }},
      {2, -128, 128, mod::PkTension, [](double v) { return v / 128.0; }},
      {3, 5, 128, mod::PkDecay, peakDecay},
      {4, 0, 65536, mod::LfBase, [](double v) { return v / 65536.0; }},
      {5, -256, 256, mod::LfAmount, [](double v) { return v / 256.0; }},
      {6, -128, 128, mod::LfTension, [](double v) { return v / 128.0; }},
      {7, 200, 65536, mod::LfRate, lfoHz},
      {8, 0, 4, mod::LfShape, [](double v) { return clampd(std::round(v), 0, 4); }},
      {9, 0, 65535, mod::LfPhase, [](double v) { return v / 65536.0; }},
  };
  applyRows(m, rows, ints(b, 0, 10));
  gen2::set(m, mod::LfSync, 0);  // FL's LFO runs at a free rate
  gen2::set(m, mod::PkMute, b.u8(40) ? 1 : 0);
  gen2::set(m, mod::PkRamp, b.u8(41) ? 1 : 0);
  m.exact = false;  // decay / LFO speed curves estimated; FL has no attack control
  return true;
}

// Fruity PanOMatic, Bass Boost and Blood Overdrive are decoded by FlpFx2*.cpp (second wave).
namespace ut = gl::packs::utility;
namespace dyn = gl::packs::dynamics;

// ---------------------------------------------------------------- Fruity Center
// u32 version 1, i32 status (1 = removing DC offset).
bool decodeCenter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 8) return false;
  gen2::begin(m, "gl.utility");
  applyRows(m, {{0, 0, 1, UtlDcRemove, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }}}, {static_cast<double>(b.i32(4))});
  m.exact = true;
  return true;
}

// ------------------------------------------------------------ Fruity Spectroman
// i32 100, i32 style, i32 amplification 0..128 (64 = unity, tag 1), i32
// frequency scale 0..128 (tag 2), u8 peaks, u8 stereo, u8 spare.
bool decodeSpectroman(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 16) return false;
  gen2::begin(m, "gl.spectro");
  std::vector<double> v = {0, static_cast<double>(b.i32(8)), static_cast<double>(b.i32(12))};
  applyRows(m,
            {{1, 0, 128, ut::SgAmp, [](double x) { return clampd((x - 64) / 64.0 * 24.0, -24, 24); }},
             {2, 0, 128, ut::SgScale, [](double x) { return clampd(x / 128.0, 0, 1); }}},
            v);
  if (b.has(16, 2)) {
    gen2::set(m, ut::SgPeaks, b.u8(16) ? 1 : 0);
    gen2::set(m, ut::SgStereo, b.u8(17) ? 1 : 0);
  }
  m.exact = false;  // display style and scale curve estimated
  return true;
}

// ------------------------------------------------------------------------ Tuner
// u32 version 1, i32 reactivity 0..6000 (tag 1), i32 averaging (0/1), i32
// bass mode (tag 2), i32 display (1 horizontal, 2 circular).
bool decodeTuner(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 20) return false;
  gen2::begin(m, "gl.tuner");
  std::vector<double> v = {0, static_cast<double>(b.i32(4)), static_cast<double>(b.i32(12))};
  applyRows(m,
            {{1, 0, 6000, TnReactivity, [](double x) { return clampd(x / 6000.0, 0, 1); }},
             {2, 0, 1, TnBassMode, [](double x) { return x >= 0.5 ? 1.0 : 0.0; }}},
            v);
  gen2::set(m, TnDisplay, b.i32(16) == 2 ? 2 : 1);
  gen2::set(m, TnReference, 440);
  m.exact = b.i32(8) != 0;  // the averaging option has no Gridline counterpart
  return true;
}

// ------------------------------------------------------------- Fruity 7 Band EQ
// u32 version 1, 7 × i32 band levels −1800..1800 (hundredths of a dB, tags
// 0..6) at the classic band centres.
bool decodeSevenBand(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 32) return false;
  gen2::begin(m, "gl.geq");
  std::vector<Row> rows;
  for (int k = 0; k < kGeqBands; ++k) rows.push_back({k, -1800, 1800, k, [](double v) { return clampd(v / 100.0, -18, 18); }});
  applyRows(m, rows, ints(b, 4, kGeqBands));
  gen2::set(m, GeqLayout, 1);
  gen2::set(m, GeqOutput, 0);
  gen2::set(m, GeqMix, 1);
  m.exact = true;
  return true;
}

// ------------------------------------------------------------------ Transmitter
// u32 version 3, then i32 per tag 0..41: 0 sensitivity, 1 hold time, 2
// release time (0..10000), 3 transient level, 4 sustain level (0..12800), 5 /
// 6 transient / sustain tilt (5000 = flat), the effect knobs of both slots
// (0..10000; transient / sustain): diffuser 7..9 / 10..12 (mix, depth,
// delay), saturation 13..15 / 16..18 (mix, drive, warmth), bounce 19..21 /
// 22..24 (mix, amp curve, time curve), delay 25..28 / 29..32 (mix, time,
// synced time 1..768, feedback); 33 lookahead, 34 stereo link, 35 detection
// mode, 36 / 37 transient / sustain enabled, 38 / 39 transient / sustain
// effect (0 saturation, 1 bounce, 2 diffusion, 3 blur, 4 delay, 5 off), 40 /
// 41 delay tempo sync. Every stored setting has a Transient Split parameter;
// the blur slot shares the diffuser knobs. Only the selected effect's knobs
// are imported (and linked for automation) per slot.
constexpr double kTmNeutral = 8743;  // level of the plugin's default state
double tmLevel(double v) {
  return v >= kTmNeutral ? clampd((v - kTmNeutral) / (12800 - kTmNeutral), 0, 1) : clampd((v - kTmNeutral) / kTmNeutral, -1, 0);
}

bool decodeTransmitter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 4 + 40 * 4) return false;
  gen2::begin(m, "gl.transplit");
  std::vector<double> v = ints(b, 4, 42);
  auto u = [](double x) { return clampd(x / 10000.0, 0, 1); };
  auto tilt = [](double x) { return clampd((x - 5000.0) / 5000.0, -1, 1); };
  std::vector<Row> rows = {
      {0, 0, 10000, dyn::TsSensitivity, u},
      {1, 0, 10000, dyn::TsHold, [](double x) { return clampd(x / 10000.0, 0, 1) * 50.0; }},
      {2, 0, 10000, dyn::TsReleaseShape, [](double x) { return x < 3333 ? 0.0 : (x < 6667 ? 1.0 : 2.0); }},
      {3, 0, 12800, dyn::TsAttack, tmLevel},
      {4, 0, 12800, dyn::TsRelease, tmLevel},
      {5, 0, 10000, dyn::TsTransTilt, tilt},
      {6, 0, 10000, dyn::TsSusTilt, tilt},
      {33, 0, 1, dyn::TsLookahead},
      {34, 0, 1, dyn::TsStereoLink},
      {35, 0, 2, dyn::TsAttackShape, [](double x) { return clampd(std::round(x), 0, 2); }},
      {36, 0, 1, dyn::TsTransOn},
      {37, 0, 1, dyn::TsSusOn},
      {40, 0, 1, dyn::TsTransSync},
      {41, 0, 1, dyn::TsSusSync},
  };
  for (int side = 0; side < 2; ++side) {
    const int o = side * dyn::kTsSlotStride;
    const int fx = static_cast<int>(clampd(v[static_cast<size_t>(38 + side)], 0, 5));
    rows.push_back({38 + side, 0, 5, dyn::TsTransFx + o, [](double x) { return clampd(std::round(x), 0, 5); }});
    int mix = -1, a = -1, bb = -1;
    switch (fx) {
      case dyn::TsFxSaturate: mix = 13 + 3 * side, a = 14 + 3 * side, bb = 15 + 3 * side; break;
      case dyn::TsFxBounce: mix = 19 + 3 * side, a = 21 + 3 * side, bb = 20 + 3 * side; break;
      case dyn::TsFxDiffuse:
      case dyn::TsFxBlur: mix = 7 + 3 * side, a = 8 + 3 * side, bb = 9 + 3 * side; break;
      case dyn::TsFxEcho: {
        const int base = 25 + 4 * side;
        const bool sync = v[static_cast<size_t>(40 + side)] >= 0.5;
        mix = base, a = base + 3, bb = sync ? base + 2 : base + 1;
        break;
      }
      default: break;
    }
    if (mix < 0) continue;
    rows.push_back({mix, 0, 10000, dyn::TsTransFxMix + o, u});
    rows.push_back({a, 0, 10000, dyn::TsTransFxA + o, u});
    if (fx == dyn::TsFxEcho && v[static_cast<size_t>(40 + side)] >= 0.5)
      rows.push_back({bb, 1, 768, dyn::TsTransFxB + o, [](double x) { return clampd((x - 1.0) / 767.0, 0, 1); }});
    else
      rows.push_back({bb, 0, 10000, dyn::TsTransFxB + o, u});
  }
  applyRows(m, rows, v);
  gen2::set(m, dyn::TsDrive, 0.0);
  gen2::set(m, dyn::TsMix, 1.0);
  gen2::set(m, dyn::TsGain, 0.0);
  gen2::set(m, dyn::TsSplitFreq, 20.0);
  gen2::set(m, dyn::TsBalance, 0.0);
  // Settings all map, but the level / time / effect laws are Gridline's own
  // (the reference DSP was not measured) and the envelope send outputs are
  // not stored in the state: approximate.
  m.exact = false;
  return true;
}

// -------------------------------------------------------------- Multiband Delay
// u32 version 4, u32 n (393), n × i32: 8 banks × {delay, volume, pan} × 16
// bands (index bank·48 + target·16 + band; value / 2^30 = 0..1, pan 0.5 =
// centre), then 384 delay scale −6000..6000, 385 ×0.1, 386 feedback 0..6000,
// 387 smoothing 0..6000, 388 filter mode, 389 keep pitch, 390 bank morph
// 0..7000, 391 wet/dry −128..128, 392 harmonic scale; then extra options.
namespace sfx = gl::packs::spacefx;
double mbdMaxTime(double scale, bool tenth) { return clampd(0.5 * std::pow(2.0, scale / 2000.0) * (tenth ? 0.1 : 1.0), 0.05, 2.0); }

bool decodeMultibandDelay(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 8) return false;
  const uint32_t n = b.u32(4);
  if (n < 392 || !b.has(8, static_cast<size_t>(n) * 4)) return false;
  gen2::begin(m, "gl.banddelay");
  auto val = [&](size_t i) { return static_cast<double>(b.i32(8 + 4 * i)); };
  auto band = [&](int bank, int target, double pos) {  // pos 0..15, interpolated
    int k = std::clamp(static_cast<int>(pos), 0, 14);
    double f = clampd(pos - k, 0, 1);
    size_t at = static_cast<size_t>(bank * 48 + target * 16 + k);
    double a = val(at) / 1073741824.0, c = val(at + 1) / 1073741824.0;
    return clampd(a + (c - a) * f, 0, 1);
  };
  const double morph = clampd(val(390) / 1000.0, 0, 7);
  const int bankA = std::min(6, static_cast<int>(morph));
  const double frac = morph - bankA;
  sfx::BandCurves cv{};
  for (int g = 0; g < 2; ++g) {
    int bank = bankA + g;
    for (int i = 0; i < sfx::kBdPoints; ++i) {
      double pos = 15.0 * i / (sfx::kBdPoints - 1);
      cv.v[g][sfx::BtDelay][i] = static_cast<float>(band(bank, 0, pos));
      cv.v[g][sfx::BtLevel][i] = static_cast<float>(band(bank, 1, pos));
      cv.v[g][sfx::BtPan][i] = static_cast<float>(band(bank, 2, pos));
      cv.v[g][sfx::BtFeedback][i] = 1.0f;  // FL's feedback is global
    }
  }
  m.state.data = sfx::formatBandCurves(cv);
  gen2::set(m, sfx::BdMaxTime, mbdMaxTime(val(384), val(385) != 0));
  gen2::set(m, sfx::BdSync, 0);
  gen2::set(m, sfx::BdMorph, frac);
  const double mix = clampd(val(391), -128, 128);
  gen2::set(m, sfx::BdDry, mix > 0 ? 1.0 - mix / 128.0 : 1.0);
  gen2::set(m, sfx::BdWet, mix < 0 ? 1.0 + mix / 128.0 : 1.0);
  std::vector<double> byTag(393, 0.0);
  for (size_t i = 384; i < 393; ++i) byTag[i] = val(i);
  applyRows(m,
            {{386, 0, 6000, sfx::BdFeedback, [](double x) { return clampd(x / 6000.0, 0, 1); }},
             {387, 0, 6000, sfx::BdSmooth, [](double x) { return clampd(x / 10000.0, 0.001, 2); }},
             {389, 0, 1, sfx::BdKeepPitch, [](double x) { return x >= 0.5 ? 1.0 : 0.0; }},
             {384, -6000, 6000, sfx::BdMaxTime, [](double x) { return mbdMaxTime(x, false); }}},
            byTag);
  gen2::set(m, sfx::BdMaxTime, mbdMaxTime(val(384), val(385) != 0));
  m.exact = false;  // 8 banks → 2 (the morph position's pair), 16 bands resampled, time scale estimated
  return true;
}

}  // namespace

void registerFx(std::map<std::string, PluginDecoder>& d) {
  add(d, "Fruity Peak Controller", decodePeakController);
  add(d, "Fruity Center", decodeCenter);
  add(d, "Fruity Spectroman", decodeSpectroman);
  add(d, "Tuner", decodeTuner);
  add(d, "Fruity 7 Band EQ", decodeSevenBand);
  add(d, "Transmitter", decodeTransmitter);
  add(d, "Multiband Delay", decodeMultibandDelay);
}

}  // namespace gl::flp::dec3

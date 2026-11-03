// Gridline — FLP import: decoders for FL reverb, delays, chorus / flanger family.
//
// State layouts (all little endian, observed in saved projects and presets):
//   Fruity Reeverb 2   u32 version 10001, i32 × 15 (tags 0..14), u8 tempo pre-delay, u8 mid/side
//                      (older: i32 × 13 (tags 0..12) + u8, no version word)
//   Fruity Delay 3     u32 version 1, i32 × 26 (tags 0..25)
//   Fruity Delay 2     i32 × 8 (tags 0..7)
//   Fruity Delay Bank  u32 version (15 / 16), i32 × 4 (tags 0..3), 8 units × i32 × 16 (unit fields),
//                      i32 oversampling, then 8 units × 32-byte records (see decodeDelayBank)
//   Fruity Chorus      u32 version 1, i32 × 12 (tags 0..11)
//   Hyper Chorus       u32 version 2, i32 × 11 (tags 0..10), i32 spare
//   Fruity Flanger     u32 version 1, i32 × 12 (tags 0..11)
//   Fruity Flangus     i32 × 8: order, then tags 1..7
// Value curves: see format notes (Time family).
#include <algorithm>
#include <cmath>
#include "../../engine/ClassicDelay.h"
#include "../../engine/Delay3Ref.h"
#include "FlpFx.h"
#include "../PluginInfo.h"
#include "../PluginParams.h"
#include "../../packs/spacefx/sfx_flcurves.h"
#include "../../packs/spacefx/spacefx.h"
#include "../DelayCurves.h"

namespace gl::flp::fx {

namespace {

namespace sfx = gl::packs::spacefx;
namespace flc = gl::packs::spacefx::flc;

// Reads `count` i32 values starting at byte `at` (missing values: `def`).
std::vector<double> ints(const Event* st, size_t at, size_t count, double def = 0) {
  std::vector<double> v(count, def);
  Reader r(st);
  r.skip(at);
  for (size_t i = 0; i < count && r.has(4); ++i) v[i] = r.i32();
  return v;
}

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

// Presets saved by FL 3 store 12 normalized floats (tags 0..11) and no
// version word; scaled to the integer ranges of the current state.
std::vector<double> oldFloats(const Event* st, const double (&range)[12]) {
  std::vector<double> v(12, 0);
  Reader r(st);
  for (size_t i = 0; i < 12 && r.has(4); ++i) v[i] = std::round(clampd(r.f32(), 0, 1) * range[i]);
  return v;
}


// Shared helper: a table + the FL values by tag → mapping.
void finish(PluginMapping& m, const std::shared_ptr<const KnobTable>& t, const std::vector<double>& byTag) {
  applyTable(m, *t, byTag);
  bindTable(m, t);
}

// ---------------------------------------------------------------- Fruity Reeverb 2

// Cut / damping wheels (5..221) → Hz (×100 Hz; 221 = off → 22.1 kHz).
double rv2CutHz(double v) { return clampd(v, 5, 221) * 100.0; }
// Level curve of the reference: x → (11^x − 1) / 10 (x = 1 → unity).
double rv2Curve(double x) { return (std::pow(11.0, x) - 1.0) / 10.0; }
// Dry (0..160, x = v/128) → linear gain.
double rv2Dry(double v) { return clampd(rv2Curve(clampd(v, 0, 160) / 128.0), 0, 1); }
// Early reflections / wet (x = v/256 into the output matrix) → gain relative
// to the 128 default.
double rv2Send(double v) { return clampd(rv2Curve(clampd(v, 0, 160) / 256.0) / rv2Curve(0.5), 0, 1); }
// Modulation speed (0..2048) → Hz (fraction mapped onto Gridline's range); depth → 0..1.
double rv2ModHz(double v) { return 0.05 * std::pow(100.0, clampd(v, 0, 2048) / 2048.0); }

std::shared_ptr<const KnobTable> reeverb2Table() {
  using namespace pp;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 19, 3000, RvbLowCut, [](double v) { return clampd(v, 20, 3000); }},  // 19 = off
      {1, 5, 221, RvbHighCut, rv2CutHz},
      {2, 0, 384, RvbPreDelay, [](double v) { return clampd(v * 1000.0 / 384.0, 0, 384); }},  // v / 384 s
      {3, 1, 100, RvbSize, scale(0.01)},
      {4, 0, 100, RvbDiffusion, scale(0.01)},
      {5, 1, 200, RvbDecayTime, scale(0.1)},
      {6, 5, 221, RvbDampHz, rv2CutHz},
      {7, 20, 300, RvbBassMult, scale(0.01)},
      {8, 25, 2000, RvbCrossover, nullptr},
      {9, -64, 64, RvbWidth, [](double v) { return clampd(1.0 + v / 64.0, -1, 1); }},
      {10, 0, 160, RvbDry, rv2Dry},
      {11, 0, 160, RvbEarly, rv2Send},
      {12, 0, 160, RvbWet, rv2Send},
      {13, 0, 2048, RvbModRate, rv2ModHz},
      {14, 0, 2048, RvbModDepth, [](double v) { return clampd(v, 0, 2048) / 2048.0; }},
  });
  return t;
}

// J-35: the full reference model (RvbModel = 2) takes the knobs as stored
// (tag k → RvbFLowCut + k); automation drives the same integers.
std::shared_ptr<const KnobTable> reeverb2RawTable() {
  using namespace pp;
  static const auto t = [] {
    static const double lo[15] = {19, 5, 0, 1, 0, 1, 5, 20, 25, -64, 0, 0, 0, 0, 0};
    static const double hi[15] = {3000, 221, 384, 100, 100, 200, 221, 300, 2000, 64, 160, 160, 160, 2048, 2048};
    KnobTable k;
    for (int i = 0; i < 15; ++i) {
      const double a = lo[i], b = hi[i];
      k.push_back({i, a, b, RvbFLowCut + i, [a, b](double v) { return clampd(std::round(v), a, b); }});
    }
    return std::make_shared<const KnobTable>(k);
  }();
  return t;
}

bool decodeReeverb2(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  if (!st || st->size < 4) return false;
  begin(m, "gl.reverb");
  // K-2: the knobs as the reference plugin itself reads the state (see
  // reeverb2Read), then mapped onto the full model's ranges with the same sound.
  const Reeverb2Knobs r = reeverb2Read(st->data, st->size);
  std::vector<double> v(r.knob, r.knob + 15);
  const bool exact = reeverb2Fit(v);
  applyTable(m, *reeverb2Table(), v);  // Gridline-unit mirrors (hall / plate models)
  finish(m, reeverb2RawTable(), v);
  set(m, RvbModel, 2);
  set(m, RvbFRev, 1);  // the model as measured against the reference's renders
  set(m, RvbDamp, 0);  // damping comes from RvbDampHz
  set(m, RvbPreSync, r.tempo ? 1 : 0);
  set(m, RvbInputMode, r.midSide ? 1 : 0);
  // J-35: the full reference reverb runs the stored knobs (spec j35_fullreverb.md).
  m.exact = exact;  // false: values outside the ranges (old layouts) approximated
  return true;
}

// ---------------------------------------------------------------- delays (shared)

double logMap(double v, double vmax, double lo, double hi) { return lo * std::pow(hi / lo, clampd(v / vmax, 0, 1)); }

// ---------------------------------------------------------------- Fruity Delay 3

double u6000(double v) { return clampd(v / 6000.0, 0, 1); }

// Tag → Gridline parameter, reference delay model (curves: core/DelayCurves.h).
std::shared_ptr<const KnobTable> delay3Table() {
  using namespace pp;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 6000, DlyInput, u6000},
      {1, 0, 3, DlyType3, [](double v) { return clampd(std::round(v), 0, 3); }},
      {2, 0, 1, DlySync, nullptr},
      {3, 0, 1, DlyKeepPitch, nullptr},
      {5, 0, 6000, DlyTimeR, [](double v) { return clampd(v / 3000.0 - 1.0, -1, 1); }},
      {6, 0, 1000, DlyTimeSmooth, nullptr},  // fraction of 1 s → ms
      {7, 0, 6000, DlySpread3, u6000},
      {8, 0, 6000, DlyModRate3, [](double v) { return dly::modRateHz(u6000(v) * 0.99); }},
      {9, 0, 6000, DlyModDepth, u6000},
      {10, 0, 6000, DlyModCutoff, u6000},
      {11, 0, 6000, DlyDiffusion, u6000},
      {12, 0, 6000, DlyDiffSpread, u6000},
      {13, 0, 1, DlyDistType, [](double v) { return v > 0.5 ? 2.0 : 1.0; }},
      {14, 0, 6000, DlyFeedback, [](double v) { return u6000(v) * 1.25; }},
      {15, 0, 3, DlyFilter3, [](double v) { return clampd(std::round(v), 0, 3); }},
      {17, 0, 6000, DlyResonance, u6000},
      {18, 0, 6000, DlyDistThresh, [](double v) { return dly::distThreshDb(u6000(v)); }},
      {19, 0, 6000, DlyDistHardness, u6000},
      {20, 0, 6000, DlyDistSymmetry, u6000},
      {21, 0, 60000, DlyRate3, [](double v) { return v >= 60000 ? 48000.0 : clampd(dly::rateHz(v / 60000.0), 150, 48000); }},
      {22, 0, 6000, DlyBits, [](double v) { return dly::bits(u6000(v)); }},
      {23, 0, 6000, DlyWet, u6000},
      {24, -3000, 3000, DlyTone, [](double v) { return clampd(v / 3000.0, -1, 1); }},
      {25, 0, 6000, DlyDry, u6000},
  });
  return t;
}

// J-35: tag k → DlyD3Raw0 + k as the reference's normalised value (v − lo) / (hi − lo).
std::shared_ptr<const KnobTable> delay3RawTable() {
  using namespace pp;
  static const auto t = [] {
    KnobTable k;
    for (int i = 0; i < 26; ++i)
      k.push_back({i, static_cast<double>(d3ref::kLo[i]), static_cast<double>(d3ref::kHi[i]), DlyD3Raw0 + i,
                   [i](double v) { return d3ref::toNorm(i, std::round(v)); }});
    return std::make_shared<const KnobTable>(k);
  }();
  return t;
}

bool decodeDelay3(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  if (!st || st->size < 4 + 26 * 4) return false;
  begin(m, "gl.delay");
  std::vector<double> v = ints(st, 4, 26);
  applyTable(m, *delay3Table(), v);  // Gridline-unit mirrors (reference model 1)
  finish(m, delay3RawTable(), v);
  set(m, DlyModel, 3);
  // Time: synced = ticks of 1/192 beat (1..768); free = the time curve (1 ms .. 1 s).
  set(m, DlyTicks, clampd(v[4], 1, 768));
  set(m, DlyTimeMs, clampd(dly::freeTimeSec((clampd(v[4], 1, 768) - 1.0) / 767.0) * 1000.0, 1, 3000));
  // Cutoff depends on the filter mode's table.
  set(m, DlyCutoff3, dly::cutoffHz(static_cast<int>(clampd(v[15], 0, 3)), u6000(v[16])));
  // J-35: the stereo delay reference (DlyModel = 3, spec j35_delay3.md) runs the
  // 26 stored controls.
  m.exact = true;
  return true;
}

// ---------------------------------------------------------------- Fruity Delay 2

// J-26: classic tempo delay (DlyModel = 2, spec j26_delay2.md): 8 raw ints, no
// version. Input / dry volume (11^(v/128) - 1)/10 (1 at 128), fold pan v/128,
// feedback v/128, time in ticks at the host tempo, input-side offset
// (1001^(|v|/512) - 1)/2 ms, modes normal / swapped / ping-pong, coefficient
// (21^(v/128) - 1)/20 scaled by 44.1 kHz / sr.
std::shared_ptr<const KnobTable> delay2Table() {
  using namespace pp;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, -128, 128, DlyClsPan, [](double v) { return clampd(v / 128.0, -1, 1); }},
      {1, 0, 160, DlyClsIn, [](double v) { return cdelay::volume(std::nearbyint(clampd(v, 0, 160))); }},
      {2, 0, 160, DlyClsDry, [](double v) { return cdelay::volume(std::nearbyint(clampd(v, 0, 160))); }},
      {3, 0, 127, DlyClsFb, [](double v) { return clampd(v, 0, 127) / 128.0; }},
      {4, 1, 768, DlyTicks, [](double v) { return clampd(std::nearbyint(v), 1, 768); }},
      {5, -512, 512, DlyClsOffset, [](double v) { return (v < 0 ? -1.0 : 1.0) * cdelay::offsetMs(v); }},
      {6, 0, 2, DlyClsMode, [](double v) { return clampd(std::nearbyint(v), 0, 2); }},
      {7, 0, 128, DlyClsCoef, [](double v) { return cdelay::coefOf(clampd(v, 0, 128) / 128.0); }},
  });
  return t;
}

bool decodeDelay2(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  if (!st || st->size < 32) return false;
  begin(m, "gl.delay");
  set(m, DlyModel, 2);
  set(m, DlyClsStepRound, 0);
  set(m, DlyClsScaleSr, 1);
  set(m, DlyClsBpm, 0);
  std::vector<double> v = ints(st, 0, 8);
  finish(m, delay2Table(), v);
  m.exact = true;
  return true;
}

// ---------------------------------------------------------------- Fruity Delay Bank
// State: version (15 / 16), 133 ints (tags 0..131: dry, wet, in, feedback,
// then 8 units × 16 fields; #132 = oversampling), then per unit a 32-byte
// record (input / feedback filter type and cascade, POST, feedback mode, grain
// division / shape, enabled, leave first echo, tempo time). Version 15 time
// fields are in 10× coarser units. Levels follow (11^x − 1)/10; curves:
// sfx_flcurves.h (md*). Imports run the reference model.

std::shared_ptr<const KnobTable> delayBankTable() {
  using namespace sfx;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 128, MdDry, [](double v) { return flc::mdGain(v / 128.0); }},
      {1, 0, 128, MdWet, [](double v) { return flc::mdGain(v / 128.0); }},
      {2, 0, 128, MdInput, [](double v) { return flc::mdGain(v / 128.0); }},
      {3, 0, 128, MdFbScale, scale(1.0 / 128)},
  });
  return t;
}

// K-2: automation of the unit controls (tags 4 + 16 k + field). The time and
// cutoff laws depend on the unit's tempo mode and filter types, so the table is
// built per instance from the decoded state.
struct DelayBankUnitModes {
  bool tempo[sfx::kTaps] = {};
  int inType[sfx::kTaps] = {}, fbType[sfx::kTaps] = {};
  double timeScale = 1;  // version 15 time fields are 10x coarser
};

std::shared_ptr<const KnobTable> delayBankAutoTable(const DelayBankUnitModes& md) {
  using namespace sfx;
  KnobTable k = *delayBankTable();
  auto pan = [](double v) { return std::clamp(v / 128.0, -1.0, 1.0); };
  auto lvl = [](double v) { return flc::mdGain(std::clamp(v, -160.0, 160.0) / 128.0); };
  auto res = [](double v) { return std::clamp(v / 65536.0, 0.0, 1.0); };
  const double ts = md.timeScale;
  for (int u = 0; u < kTaps; ++u) {
    const int b = 4 + 16 * u;
    const int it = md.inType[u] == MfOff ? MfLowPass : md.inType[u], ft = md.fbType[u] == MfOff ? MfLowPass : md.fbType[u];
    k.push_back({b + 0, -128, 128, mdParam(u, MdInPan), pan});
    k.push_back({b + 1, -160, 160, mdParam(u, MdInLevel), lvl});
    k.push_back({b + 2, 0, 65536, mdParam(u, MdInCut), [it](double v) { return flc::mdCutHz(it, v / 65536.0); }});
    k.push_back({b + 3, 0, 65536, mdParam(u, MdInRes), res});
    k.push_back({b + 5, -128, 128, mdParam(u, MdFbPan), pan});
    k.push_back({b + 6, -128, 128, mdParam(u, MdFeedback), pan});
    k.push_back({b + 7, -128, 128, mdParam(u, MdSep), pan});
    if (md.tempo[u])
      k.push_back({b + 8, 0, 7680, mdParam(u, MdSteps), [ts](double v) { return std::clamp(v * ts / 480.0, 0.002, 64.0); }});
    else
      k.push_back({b + 8, 0, 7680, mdParam(u, MdTimeMs), [ts](double v) { return std::clamp(v * ts / 3.84, 0.02, 5000.0); }});
    k.push_back({b + 9, -512, 512, mdParam(u, MdOffset), [](double v) { return flc::mdOffsetMs(v); }});
    k.push_back({b + 10, 0, 65536, mdParam(u, MdFbCut), [ft](double v) { return flc::mdCutHz(ft, v / 65536.0); }});
    k.push_back({b + 11, 0, 65536, mdParam(u, MdFbRes), res});
    k.push_back({b + 13, -128, 128, mdParam(u, MdOutPan), pan});
    k.push_back({b + 14, -160, 160, mdParam(u, MdOutLevel), lvl});
    if (u < kTaps - 1) k.push_back({b + 15, -160, 160, mdParam(u, MdToNext), lvl});
  }
  return std::make_shared<const KnobTable>(std::move(k));
}

bool decodeDelayBank(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace sfx;
  const Event* st = pluginState(evs);
  const size_t flags = 4 + (4 + 8 * 16) * 4;  // per-unit option records follow the values
  if (!st || st->size < flags) return false;
  const int version = st->i32(0);
  begin(m, "gl.multidelay");
  std::vector<double> v = ints(st, 4, 4 + 8 * 16);
  finish(m, delayBankTable(), v);
  set(m, MdModel, 1);
  set(m, MdSync, 1);
  set(m, MdOversample, st->i32(flags) >= 1 ? 1 : 0);
  set(m, MdGateLaw, 1);
  static const int kType[8] = {MfOff, MfLowPass, MfBandPass, MfNotch, MfHighPass, MfLowShelf, MfPeak, MfHighShelf};  // FL order → Gridline
  static const int kMode[4] = {MfbOff, MfbNormal, MfbInvert, MfbPingPong};
  bool exact = true;
  DelayBankUnitModes modes;
  modes.timeScale = version <= 15 ? 10.0 : 1.0;
  for (int k = 0; k < kTaps; ++k) {
    const double* u = v.data() + 4 + 16 * k;  // tags 4..19 of unit k
    auto P = [&](int f, double x) { set(m, mdParam(k, f), x); };
    auto X = [&](int e, double x) { set(m, mdExt(k, e), x); };
    const size_t rec = flags + 4 + 32 * static_cast<size_t>(k);  // 32-byte unit record (after the oversampling int)
    const bool on = st->u8(rec + 29) != 0;
    const int inType = kType[std::clamp(st->i32(rec + 0), 0, 7)], fbType = kType[std::clamp(st->i32(rec + 8), 0, 7)];
    const double time = u[8] * (version <= 15 ? 10.0 : 1.0);
    const bool tempo = st->u8(rec + 31) != 0;
    modes.tempo[k] = tempo, modes.inType[k] = inType, modes.fbType[k] = fbType;
    P(MdOn, on ? 1 : 0);
    P(MdInPan, clampd(u[0] / 128.0, -1, 1));
    P(MdInLevel, flc::mdGain(clampd(u[1], -160, 160) / 128.0));
    P(MdInFType, inType);
    P(MdInCut, flc::mdCutHz(inType == MfOff ? MfLowPass : inType, u[2] / 65536.0));
    P(MdInRes, clampd(u[3] / 65536.0, 0, 1));
    X(MdxInGain, clampd(flc::mdFilterA(clampd(u[4], 0, 256)), 0.1, 2));
    X(MdxInStages, std::clamp(st->i32(rec + 4), 0, 2) + 1);
    X(MdxInPost, st->u8(rec + 16) ? 1 : 0);
    P(MdFbPan, clampd(u[5] / 128.0, -1, 1));
    P(MdFeedback, clampd(u[6] / 128.0, -1, 1));
    P(MdFbMode, kMode[std::clamp(static_cast<int>(st->i32(rec + 17)), 0, 3)]);
    P(MdSep, clampd(u[7] / 128.0, -1, 1));
    X(MdxTempo, tempo ? 1 : 0);
    if (tempo)
      P(MdSteps, clampd(time / 480.0, 0.002, 64));  // 1/1920 beat → sixteenth steps
    else
      P(MdTimeMs, clampd(time / 3.84, 0.02, 5000));
    P(MdOffset, flc::mdOffsetMs(u[9]));
    P(MdFbFType, fbType);
    P(MdFbCut, flc::mdCutHz(fbType == MfOff ? MfLowPass : fbType, u[10] / 65536.0));
    P(MdFbRes, clampd(u[11] / 65536.0, 0, 1));
    X(MdxFbGain, clampd(flc::mdFilterA(clampd(u[12], 0, 256)), 0.1, 2));
    X(MdxFbStages, std::clamp(st->i32(rec + 12), 0, 2) + 1);
    P(MdOutPan, clampd(u[13] / 128.0, -1, 1));
    P(MdOutLevel, flc::mdGain(clampd(u[14], -160, 160) / 128.0));
    P(MdToNext, k < kTaps - 1 ? flc::mdGain(clampd(u[15], -160, 160) / 128.0) : 0.0);
    P(MdGateDiv, std::clamp(st->i32(rec + 21), 0, 16));
    P(MdGateShape, clampd(st->i32(rec + 25) / 128.0, -1, 1));
    X(MdxLeaveFirst, st->u8(rec + 30) ? 1 : 0);
    P(MdModDepth, 0);
    P(MdDiffuse, 0);
    // J-35: filter scale, pan laws, cascades, grain window and the minimum time
    // follow the reference (spec j35_delaybank.md); only times beyond Gridline's
    // line (5 s) stay approximate.
    if (on && (tempo ? time / 480.0 > 64 : time / 3.84 > 5000)) exact = false;
    (void)inType, (void)fbType;
  }
  // J-42: the 2x resampler (polyphase IIR half-bands) is the reference's own.
  // The export's solver decodes a state per candidate it tries and reads only the parameters:
  // building the unit table (over 100 controls) each time took a Delay Bank to about 2.7x its export cost (K-X).
  if (!c.paramsOnly) bindTable(m, delayBankAutoTable(modes));
  m.exact = exact;
  return true;
}

// ---------------------------------------------------------------- Fruity Chorus

double chorusRate(double v) { return logMap(v, 5000, 0.05, 8); }

std::shared_ptr<const KnobTable> chorusTable() {
  using namespace pp;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 1024, ChoDelay, [](double v) { return 2.0 + 28.0 * clampd(v / 1024.0, 0, 1); }},
      {1, 0, 5000, ChoDepth, scale(1.0 / 5000)},
      {2, 0, 1024, ChoStereo, scale(1.0 / 1024)},
      {3, 0, 5000, ChoRate, chorusRate},
      {4, 0, 5000, ChoRate2, chorusRate},
      {5, 0, 5000, ChoRate3, chorusRate},
      {6, 0, 1024, ChoShape, scale(1.0 / 1024)},
      {10, 0, 5000, ChoCrossFreq, [](double v) { return logMap(v, 5000, 20, 20000); }},
      {11, 0, 1024, ChoWetOnly, [](double v) { return v >= 512 ? 1.0 : 0.0; }},
  });
  return t;
}

bool decodeChorus(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  static const double kRange[12] = {1024, 5000, 1024, 5000, 5000, 5000, 1024, 1024, 1024, 1024, 5000, 1024};
  if (!st || (st->size < 52 && st->size != 48)) return false;
  begin(m, "gl.chorus");
  std::vector<double> v = st->size == 48 ? oldFloats(st, kRange) : ints(st, 4, 12);
  finish(m, chorusTable(), v);
  set(m, ChoVoices, 2);  // three LFO voices
  set(m, ChoCrossMode, v[9] > 0 ? 1 : 0);
  set(m, ChoMix, 0.5);
  m.exact = false;  // per-voice LFO waves and the cross type amount approximated
  return true;
}

// ---------------------------------------------------------------- Hyper Chorus


// J-26: reference engine (EnModel = 1, spec j26_hyperchorus.md): every control
// as its knob position n = (raw - min) / (max - min); the low / high pass
// values are knob positions too (raw is not Hz).
std::shared_ptr<const KnobTable> hyperChorusTable() {
  using namespace sfx;
  static const auto t = [] {
    static const double lo[11] = {0, 0, 0, 0, 0, 0, 0, 7140, 120, 0, 0}, hi[11] = {1000, 1000, 1000, 1000, 1000, 1000, 1, 22000, 10900, 1000, 1000};
    KnobTable k;
    for (int i = 0; i < 11; ++i) {
      const double a = lo[i], b = hi[i];
      k.push_back({i, a, b, EnK0 + i, [a, b](double v) { return clampd((v - a) / (b - a), 0, 1); }});
    }
    return std::make_shared<const KnobTable>(k);
  }();
  return t;
}

bool decodeHyperChorus(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  const Event* st = pluginState(evs);
  if (!st || st->size < 48) return false;
  begin(m, "gl.ensemble");
  set(m, sfx::EnModel, 1);
  finish(m, hyperChorusTable(), ints(st, 4, 11));
  m.exact = true;  // every control law; the starting LFO phases are random in the original
  return true;
}

// ---------------------------------------------------------------- Fruity Flanger

// J-26: reference engine (FlgModel = 1, spec j26_flanger.md): knob positions
// n = raw / max; FL 3 era states hold the twelve n as floats.
bool decodeFlanger(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  static const double kRange[12] = {1024, 5000, 5000, 360, 5000, 1024, 100, 1024, 1024, 5000, 5000, 5000};
  if (!st || (st->size < 52 && st->size != 48)) return false;
  begin(m, "gl.flanger");
  std::vector<double> v = st->size == 48 ? oldFloats(st, kRange) : ints(st, 4, 12);
  set(m, FlgModel, 1);
  auto n = [](double mx) { return [mx](double x) { return clampd(x / mx, 0, 1); }; };
  auto sw = [](double x) { return x / 1024.0 > 0.5 ? 1.0 : 0.0; };
  static const auto table = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 1024, FlgDelayN, n(1024)},  {1, 0, 5000, FlgDepthN, n(5000)}, {2, 0, 5000, FlgRateN, n(5000)},
      {3, 0, 360, FlgPhase, [](double x) { return clampd(x, 0, 360); }},  {4, 0, 5000, FlgDampN, n(5000)},
      {5, 0, 1024, FlgShape, n(1024)},  {6, 0, 100, FlgFeedN, n(100)},     {7, 0, 1024, FlgInvFb, sw},
      {8, 0, 1024, FlgInvertWet, sw},    {9, 0, 5000, FlgDryN, n(5000)},    {10, 0, 5000, FlgWetN, n(5000)},
      {11, 0, 5000, FlgCrossN, n(5000)}});
  finish(m, table, v);
  m.exact = true;
  return true;
}

// ---------------------------------------------------------------- Fruity Flangus

std::shared_ptr<const KnobTable> flangusTable() {
  using namespace sfx;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 1, 9, SwOrder, [](double v) { return clampd(v, 1, 9); }},
      {1, 0, 1024, SwDepth, scale(1.0 / 1024)},
      {2, 0, 1024, SwSpeed, [](double v) { return logMap(v, 1024, 0.02, 10); }},
      {3, 0, 128, SwDelay, [](double v) { return 0.1 + 24.9 * clampd(v / 128.0, 0, 1); }},
      {4, 0, 128, SwSpread, scale(1.0 / 128)},
      {5, -128, 128, SwCross, [](double v) { return 0.95 * clampd(v / 128.0, -1, 1); }},
      {6, -128, 128, SwDry, scale(1.0 / 128)},
      {7, -128, 128, SwWet, scale(1.0 / 128)},
  });
  return t;
}

bool decodeFlangus(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  const Event* st = pluginState(evs);
  if (!st || st->size < 32) return false;
  begin(m, "gl.swirl");
  finish(m, flangusTable(), ints(st, 0, 8));
  m.exact = false;  // control set 1:1; speed / delay curves reasoned
  return true;
}

}  // namespace

// K-2: observed by loading crafted states into the reference plugin and reading
// its controls back (behaviour only).
Reeverb2Knobs reeverb2Read(const uint8_t* p, size_t n) {
  Reeverb2Knobs r;
  if (!p || n < 4) return r;
  Reader rd(p, n);
  const uint32_t ver = rd.u32();
  if (ver > 10001) return r;  // unknown version: the plugin keeps its defaults
  const bool versioned = ver == 10000 || ver == 10001;
  const size_t knobs = ver == 10001 ? 15 : 13;
  for (size_t k = 0; k < knobs && rd.left() > 0; ++k) {
    if (rd.has(4)) {
      r.knob[k] = rd.i32();
    } else {  // a short last value: the bytes there, zero-padded
      uint32_t v = 0;
      for (int b = 0; rd.left() > 0; ++b) v |= static_cast<uint32_t>(rd.u8()) << (8 * b);
      r.knob[k] = static_cast<int32_t>(v);
    }
  }
  if (versioned) {
    if (rd.left() > 0) r.tempo = rd.u8() != 0;
    if (rd.left() > 0) r.midSide = rd.u8() != 0;
  }
  r.current = ver == 10001 && n >= 4 + 15 * 4 + 2;
  return r;
}

bool reeverb2Fit(std::vector<double>& v) {
  if (v.size() < 15) v.resize(15, 0);
  static const double lo[15] = {19, 5, 0, 1, 0, 1, 5, 20, 25, -64, 0, 0, 0, 0, 0};
  static const double hi[15] = {3000, 221, 384, 100, 100, 200, 221, 300, 2000, 64, 160, 160, 160, 2048, 2048};
  bool exact = true;
  // Equivalent values first (the plugin's own laws give the same sound).
  for (int k : {1, 6})
    if (v[static_cast<size_t>(k)] == 0) v[static_cast<size_t>(k)] = 221;  // cut 0 = off, as 221
  if (v[8] <= 0) {  // crossover at or below 0 Hz: the bass shelf has no effect (multiplier 100 %)
    if (v[8] < 0) exact = false;
    v[8] = 25, v[7] = 100;
  }
  if (v[9] > 64) v[9] = 64;  // separation saturates at fully merged
  for (size_t k = 0; k < 15; ++k) {
    const double c = std::clamp(v[k], lo[k], hi[k]);
    if (c != v[k] && !(k == 0 && v[k] < 19 && v[k] >= 0)) exact = false;  // low cut under 19 Hz: inaudible
    v[k] = c;
  }
  return exact;
}

void reeverb2Mirrors(PluginState& s) {
  using namespace pp;
  if (s.params.size() <= static_cast<size_t>(RvbFModDepth)) return;
  PluginMapping m;
  m.state = s;
  std::vector<double> v(15);
  for (size_t k = 0; k < 15; ++k) v[k] = s.params[static_cast<size_t>(RvbFLowCut) + k];
  applyTable(m, *reeverb2Table(), v);
  if (const PluginInfo* info = findPlugin(s.type))
    for (size_t k = 0; k < m.state.params.size() && k < info->params.size(); ++k) m.state.params[k] = static_cast<float>(info->params[k].clamp(m.state.params[k]));
  s = m.state;
}

void registerFxTime(std::map<std::string, PluginDecoder>& d) {
  d["Fruity Reeverb 2"] = decodeReeverb2;
  d["Fruity Delay 3"] = decodeDelay3;
  d["Fruity Delay 2"] = decodeDelay2;
  d["Fruity Delay Bank"] = decodeDelayBank;
  d["Fruity Chorus"] = decodeChorus;
  d["Hyper Chorus"] = decodeHyperChorus;
  d["Fruity Flanger"] = decodeFlanger;
  d["Fruity Flangus"] = decodeFlangus;
}

}  // namespace gl::flp::fx

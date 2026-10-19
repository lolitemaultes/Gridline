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
// Value curves: see analysis/demos/notes_gamma.md (Time family).
#include <algorithm>
#include <cmath>
#include "FlpFx.h"
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

// Tempo of the project being imported (FL tempo-synced times).
double tempoOf(const Context& c) { return c.song.tempo > 1 ? c.song.tempo : 120.0; }

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

bool decodeReeverb2(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  if (!st || st->size < 52) return false;
  begin(m, "gl.reverb");
  // Version word 10000 (13 knobs) or 10001 (15 knobs, with modulation); the
  // oldest states have no version word (13 knobs).
  const uint32_t ver = st->u32(0);
  const bool versioned = ver == 10000 || ver == 10001;
  const size_t knobs = ver == 10001 && st->size >= 64 ? 15 : 13;
  std::vector<double> v = versioned ? ints(st, 4, knobs) : ints(st, 0, 13);
  if (v.size() < 15) v.resize(15), v[13] = 684, v[14] = 0;  // no modulation in old states
  const size_t flags = (versioned ? 4 : 0) + knobs * 4;
  finish(m, reeverb2Table(), v);
  set(m, RvbDamp, 0);  // damping comes from RvbDampHz
  set(m, RvbPreSync, st->u8(flags) ? 1 : 0);
  set(m, RvbInputMode, st->u8(flags + 1) ? 1 : 0);
  // Decay, size, diffusion, pre-delay (v / 384 s, tempo: v / 192 beat), cuts,
  // damping, bass, crossover and the level curves follow the plugin; the
  // reverb network itself is Gridline's own.
  m.exact = false;
  return true;
}

// ---------------------------------------------------------------- delays (shared)

double logMap(double v, double vmax, double lo, double hi) { return lo * std::pow(hi / lo, clampd(v / vmax, 0, 1)); }

// FL tempo-based delay time in ticks (192 per beat) → gl.delay: the matching
// synced division, or a free time at the project tempo.
void setDelayTime(PluginMapping& m, double ticks, double tempo) {
  using namespace pp;
  const double beats = std::max(1.0, ticks) / 192.0;
  for (int i = 0; i < kDelayBeatCount; ++i)
    if (std::fabs(kDelayBeats[i] - beats) < 1e-6) {
      set(m, DlySync, 1);
      set(m, DlyTime, i);
      return;
    }
  set(m, DlySync, 0);
  set(m, DlyTimeMs, clampd(beats * 60000.0 / tempo, 1, 3000));
}

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

bool decodeDelay3(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  if (!st || st->size < 4 + 26 * 4) return false;
  begin(m, "gl.delay");
  std::vector<double> v = ints(st, 4, 26);
  finish(m, delay3Table(), v);
  set(m, DlyModel, 1);
  // Time: synced = ticks of 1/192 beat (1..768); free = the time curve (1 ms .. 1 s).
  set(m, DlyTicks, clampd(v[4], 1, 768));
  set(m, DlyTimeMs, clampd(dly::freeTimeSec((clampd(v[4], 1, 768) - 1.0) / 767.0) * 1000.0, 1, 3000));
  // Cutoff depends on the filter mode's table.
  set(m, DlyCutoff3, dly::cutoffHz(static_cast<int>(clampd(v[15], 0, 3)), u6000(v[16])));
  m.exact = false;  // curves exact (see DelayCurves.h); diffusion / spread networks are Gridline's own
  return true;
}

// ---------------------------------------------------------------- Fruity Delay 2

std::shared_ptr<const KnobTable> delay2Table() {
  using namespace pp;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, -128, 128, DlyInputPan, scale(1.0 / 128)},
      {1, 0, 160, DlyInput, scale(1.0 / 128)},
      {2, 0, 160, DlyDry, [](double v) { return clampd(v / 128.0, 0, 1); }},
      {3, 0, 127, DlyFeedback, scale(1.0 / 128)},
      // Offset: (1001^(|v|/512) − 1) / 2 ms, negative = left side later (up to 500 ms).
      {5, -512, 512, DlyOffset, [](double v) { return (v < 0 ? -1.0 : 1.0) * (std::pow(1001.0, std::fabs(clampd(v, -512, 512)) / 512.0) - 1.0) * 0.5; }},
      {7, 0, 128, DlyCutoff, [](double v) { return clampd(logMap(v, 128, 20, 20000), 300, 20000); }},
  });
  return t;
}

bool decodeDelay2(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  if (!st || st->size < 32) return false;
  begin(m, "gl.delay");
  set(m, DlyModel, 0);  // Delay 2 maps onto the legacy model (input pan, inverted feedback, ms offset)
  std::vector<double> v = ints(st, 0, 8);
  finish(m, delay2Table(), v);
  setDelayTime(m, v[4], tempoOf(c));
  static const int modes[3] = {0 /* normal */, 2 /* invert */, 1 /* ping-pong */};
  set(m, DlyMode, modes[static_cast<int>(clampd(v[6], 0, 2))]);
  set(m, DlyWet, 1);
  set(m, DlyFilterType, 0);
  set(m, DlyLowCut, 20);
  set(m, DlyDistType, 0);
  m.exact = false;
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

bool decodeDelayBank(Context&, const std::vector<Event>& evs, PluginMapping& m) {
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
  static const int kType[8] = {MfOff, MfLowPass, MfBandPass, MfNotch, MfHighPass, MfLowShelf, MfPeak, MfHighShelf};  // FL order → Gridline
  static const int kMode[4] = {MfbOff, MfbNormal, MfbInvert, MfbPingPong};
  bool exact = true;
  for (int k = 0; k < kTaps; ++k) {
    const double* u = v.data() + 4 + 16 * k;  // tags 4..19 of unit k
    auto P = [&](int f, double x) { set(m, mdParam(k, f), x); };
    auto X = [&](int e, double x) { set(m, mdExt(k, e), x); };
    const size_t rec = flags + 4 + 32 * static_cast<size_t>(k);  // 32-byte unit record (after the oversampling int)
    const bool on = st->u8(rec + 29) != 0;
    const int inType = kType[std::clamp(st->i32(rec + 0), 0, 7)], fbType = kType[std::clamp(st->i32(rec + 8), 0, 7)];
    const double time = u[8] * (version <= 15 ? 10.0 : 1.0);
    const bool tempo = st->u8(rec + 31) != 0;
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
      P(MdSteps, clampd(time / 480.0, 0.25, 64));  // 1/1920 beat → sixteenth steps
    else
      P(MdTimeMs, clampd(time / 3.84, 1, 5000));
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
    // Exact unless an enabled unit relies on a still-assumed piece: the
    // filter frequency scale (22050 Hz per unit) or the pan law; or a time
    // outside Gridline's ranges.
    if (on) {
      if (inType != MfOff || fbType != MfOff) exact = false;
      if (u[0] != 0 || u[13] != 0 || u[5] != 0) exact = false;
      if (tempo ? time < 120 : time < 3.84) exact = false;
    }
  }
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

double u1000(double v) { return clampd(v / 1000.0, 0, 1); }

std::shared_ptr<const KnobTable> hyperChorusTable() {
  using namespace sfx;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 1000, EnDelay, [](double v) { return 1.0 + 39.0 * u1000(v); }},
      {1, 0, 1000, EnFeedback, [](double v) { return 0.9 * u1000(v); }},
      {2, 0, 1000, EnModRate, [](double v) { return logMap(v, 1000, 0.05, 10); }},
      {3, 0, 1000, EnModAmount, u1000},
      {4, 0, 1000, EnMoveAmount, u1000},
      {5, 0, 1000, EnMoveRate, [](double v) { return logMap(v, 1000, 0.02, 4); }},
      {6, 0, 1, EnLines, [](double v) { return v > 0 ? 1.0 : 0.0; }},
      {7, 7140, 22000, EnLowPass, [](double v) { return clampd(v, 1500, 22000); }},
      {8, 120, 10900, EnHighPass, [](double v) { return clampd(v, 20, 10900); }},
      {9, 0, 1000, EnMix, u1000},
      {10, 0, 1000, EnPhase, u1000},
  });
  return t;
}

bool decodeHyperChorus(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  const Event* st = pluginState(evs);
  if (!st || st->size < 48) return false;
  begin(m, "gl.ensemble");
  finish(m, hyperChorusTable(), ints(st, 4, 11));
  set(m, sfx::EnSpread, 1);
  m.exact = false;  // filter frequencies direct; time / rate curves reasoned
  return true;
}

// ---------------------------------------------------------------- Fruity Flanger

std::shared_ptr<const KnobTable> flangerTable() {
  using namespace pp;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 1024, FlgDelay, [](double v) { return 0.1 + 9.9 * clampd(v / 1024.0, 0, 1); }},
      {1, 0, 5000, FlgDepth, scale(1.0 / 5000)},
      {2, 0, 5000, FlgRate, [](double v) { return logMap(v, 5000, 0.02, 5); }},
      {3, 0, 360, FlgPhase, nullptr},
      {4, 0, 5000, FlgDamp, [](double v) { return logMap(v, 5000, 20000, 500); }},
      {5, 0, 1024, FlgShape, scale(1.0 / 1024)},
      {8, 0, 1024, FlgInvertWet, [](double v) { return v >= 512 ? 1.0 : 0.0; }},
      {11, 0, 5000, FlgCross, scale(1.0 / 5000)},
  });
  return t;
}

bool decodeFlanger(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  const Event* st = pluginState(evs);
  static const double kRange[12] = {1024, 5000, 5000, 360, 5000, 1024, 100, 1024, 1024, 5000, 5000, 5000};
  if (!st || (st->size < 52 && st->size != 48)) return false;
  begin(m, "gl.flanger");
  std::vector<double> v = st->size == 48 ? oldFloats(st, kRange) : ints(st, 4, 12);
  finish(m, flangerTable(), v);
  double fb = clampd(v[6] / 100.0, 0, 1) * 0.95;
  set(m, FlgFeedback, v[7] >= 512 ? -fb : fb);
  double dry = clampd(v[9], 0, 5000), wet = clampd(v[10], 0, 5000);
  set(m, FlgMix, dry + wet > 0 ? wet / (dry + wet) : 0.5);
  m.exact = false;  // separate dry / wet levels folded into one mix
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

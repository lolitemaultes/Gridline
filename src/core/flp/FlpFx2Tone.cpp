// Gridline — FLP import, second wave: filters, EQs, distortion, loudness and
// band splitting.
//
// State layouts (ints = int32 in tag order):
//   Fruity Blood Overdrive  version 3, pre band, colour, pre amp (0..10000),
//                           x100 (0/1), post filter, post gain (0..10000), 2 spare.
//   Fruity Fast LP          version 1, cutoff, resonance (0..1024).
//   Fruity Bass Boost       version 1, frequency, amount (0..1024).
//   Fruity Squeeze          version 0, gain (0..160, 127 = unity), mix (0..128),
//                           steps (1..32768), filter on, filter type, resonance,
//                           cutoff (0..65536), filter position, relation, impact,
//                           preserve (samples), amount (0..65536).
//   Fruity Parametric EQ    no version; levels 0-6 (±1800 = ±18 dB), freq 7-13
//                           (0..65536), width 14-20, type 21-27, main 28 (0..256).
//   EQUO                    version 3/4, current curves + band modes, 9 banks of
//                           31 × 3 curve values, then shift, mix, morph, smoothing,
//                           volume, … (full layout at decodeEquo)
//   Frequency Splitter      version 4, low/mid freq, mid/high freq (0..65536),
//                           slopes (index into 6..96 dB), linear phase, band
//                           mode (0 = 2 bands), quality, lin. speed, send levels
//                           8-10 and main levels 11-13 (-1800..1800 amplitude
//                           curve), send targets, mute bits (word 20).
//   Emphasis                version, delta, ?, envelope order, true peak, M/S,
//                           mode, input gain, linking, amount, ceiling (0..10000),
//                           level match, oversampling, hardness, …
//   Emphasizer              version, input gain, emphasis, clipping, level match,
//                           bypass, output gain (0..10000).
//   Low Lifter              version 6, ?, dry/wet, low gain, slope, lookahead,
//                           invert, pre gain, diffuser mix, diffuser coefficient,
//                           high gain, crossover, low cut (0..10000), multiband,
//                           high band, low band.
//   Distructor, Hardcore    module racks (not decoded: default Amp Rack).
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/spectral/spectral.h"
#include "../../packs/utility/utility.h"
#include "../PluginParams.h"
#include "FlpFx2.h"

namespace gl::flp::fx2 {

namespace {

namespace pp = gl::pp;
namespace dy = gl::packs::dynamics;
namespace ut = gl::packs::utility;
namespace sp = gl::packs::spectral;

// ------------------------------------------------------------------ Blood Overdrive → Overdrive
double odPostDb(double v) { return clampd(dbOf(v / 10000.0, -36), -36, 6); }

bool decodeBloodOverdrive(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 6 * 4) return false;
  r.i32();  // version
  std::vector<double> t = ints(r, 6);
  if (t.size() < 6) return false;
  fx::begin(m, "gl.overdrive");
  auto lvl = [](double v) { return clampd(v / 10000.0, 0, 1); };
  KnobTable tab = {{0, 0, 10000, dy::OdPreBand, lvl},    {1, 0, 10000, dy::OdColor, lvl},
                   {2, 0, 10000, dy::OdPreAmp, lvl},     {3, 0, 1, dy::OdBoost, nullptr},
                   {4, 0, 10000, dy::OdPostFilter, lvl}, {5, 0, 10000, dy::OdPostGain, odPostDb}};
  fx::applyTable(m, tab, t);
  m.exact = false;  // knob curves follow the control ranges; the transfer curve is Gridline's
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Fast LP → Quick Lowpass
// The plugin turns the cutoff knob c (0..1) into a one-pole style coefficient
// a = (601^x - 1) / 600 with x = (c + 1/63) · 63/64 (bypassed at the top);
// Hz here assumes a = 1 - e^(-2π f / 44100).
double fastLpHz(double v) {
  double c = clampd(v / 1024.0, 0, 1);
  if (c >= 1.0) return 20000.0;
  double x = (c + 1.0 / 63.0) * 0.984375;
  double a = clampd((std::pow(601.0, x) - 1.0) / 600.0, 1e-6, 0.999);
  return clampd(-44100.0 * std::log(1.0 - a) / (2.0 * 3.14159265358979323846), 20.0, 20000.0);
}

bool decodeFastLp(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 12) return false;
  r.i32();  // version
  std::vector<double> t = ints(r, 2);
  if (t.size() < 2) return false;
  fx::begin(m, "gl.fastlp");
  KnobTable tab = {{0, 0, 1024, ut::QlCutoff, fastLpHz}, {1, 0, 1024, ut::QlReso, [](double v) { return clampd(v / 1024.0, 0, 1); }}};
  fx::applyTable(m, tab, t);
  m.exact = t[0] >= 1024 && t[1] == 0;  // fully open = transparent in both
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Bass Boost → Bass Boost
bool decodeBassBoost(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 12) return false;
  r.i32();
  std::vector<double> t = ints(r, 2);
  if (t.size() < 2) return false;
  fx::begin(m, "gl.bassboost");
  KnobTable tab = {{0, 0, 1024, ut::BbFreq, [](double v) { return expMap(v / 1024.0, 20, 500); }},
                   {1, 0, 1024, ut::BbAmount, [](double v) { return clampd(v / 1024.0, 0, 1); }}};
  fx::applyTable(m, tab, t);
  m.exact = false;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Squeeze → Squeeze
// J-26: reference engine (SqModel = 1), laws in analysis/demos/specs/j26_squeeze.md.
bool decodeSqueeze(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 12 * 4) return false;
  r.i32();
  std::vector<double> t = ints(r, 12);
  if (t.size() < 12) return false;
  fx::begin(m, "gl.squeeze");
  fx::set(m, dy::SqModel, 1);
  auto w = [](double v) { return clampd(v / 65536.0, 0, 1); };
  auto ms = [](double v) { return dy::sqMsOfRaw(v); };
  KnobTable tab = {
      {0, 0, 160, dy::SqGainRef, [](double v) { return clampd(v / 127.0, 0, 160.0 / 127.0); }},
      {1, 0, 128, dy::SqMix, [](double v) { return clampd(v / 128.0, 0, 1); }},
      {2, 1, 32768, dy::SqStepsRef, [](double v) { return dy::sqStepsOfRaw(v); }},
      {3, 0, 1, dy::SqFilterOn, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }},
      {4, 0, 4, dy::SqFilterType, [](double v) { return clampd(std::round(v), 0, 4); }},
      {5, 0, 65536, dy::SqResonance, w},
      {6, 0, 65536, dy::SqCutoffPos, w},
      {7, 0, 1, dy::SqFilterPos, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }},
      {9, 0, 44100, dy::SqImpactMs, ms},
      {10, 0, 44100, dy::SqPreserveMs, ms},
      {11, 0, 65536, dy::SqAmountRef, w},
  };
  fx::applyTable(m, tab, t);
  fx::set(m, dy::SqRelation, clampd(t[8], 0, 3));  // a link between the two lengths in the editor only
  m.exact = true;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Parametric EQ (v1) → Parametric EQ
int eqType(int fl) {
  static const int map[8] = {pp::EqOff, pp::EqHighCut, pp::EqBandPass, pp::EqLowCut, pp::EqNotch, pp::EqLowShelf, pp::EqPeak, pp::EqHighShelf};
  return map[std::clamp(fl, 0, 7)];
}

bool decodeParamEq1(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  std::vector<double> t = ints(r, 29);
  if (t.size() < 29) return false;
  fx::begin(m, "gl.eq");
  KnobTable tab;
  for (int b = 0; b < 7; ++b) {
    tab.push_back({b, -1800, 1800, pp::eqParam(b, pp::EqGain), [](double v) { return clampd(v / 100.0, -18, 18); }});
    tab.push_back({7 + b, 0, 65536, pp::eqParam(b, pp::EqFreq), [](double v) { return clampd(fx::flEqFreqHz(v), 20, 20000); }});
    tab.push_back({14 + b, 0, 65536, pp::eqParam(b, pp::EqQ), [](double v) { return clampd(fx::flInsertEqQ(v), 0.1, 18); }});
    tab.push_back({21 + b, 0, 7, pp::eqParam(b, pp::EqType), [](double v) { return eqType(static_cast<int>(std::lround(v))); }});
  }
  tab.push_back({28, 0, 256, pp::EqMain, [](double v) { return clampd(dbOf(v / 128.0, -18), -18, 18); }});
  fx::applyTable(m, tab, t);
  m.exact = false;  // band width → Q curve approximate (levels, types, frequencies follow FL's tables)
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ EQUO → Morph EQ
// State (v3 4139 bytes, v4 4143): u32 version; 31 × 3 × {f32 current value,
// i32 band mode} (band-major); 9 × 31 × 3 f32 banks (bank-major, then band,
// then curve; bank 8 = master); i32 shift (cents), mix (±128), morph (0..7000),
// smoothing (0..1024), volume (0..256), oversample, bandwidth (80..500),
// send track (-1 none), edited curve, edited bank; u8 send only, add mode,
// master; v4: u32 flags (editor panel).
double equoShift(double v) { return clampd(v / 1200.0, -10, 10); }
double equoMix(double v) { return clampd(v / 128.0, -1, 1); }
double equoMorph(double v) { return clampd(v / 1000.0, 0, 7); }
double equoSmooth(double v) { return clampd(v / 1024.0, 0, 1); }
double equoBand(double v) { return clampd(v / 536870912.0, 0, 2); }  // automation: value × 2^29

bool decodeEquo(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  if (b.size() < 4139) return false;
  Reader r(b.data(), b.size());
  const uint32_t version = r.u32();
  if (version < 3) return false;
  fx::begin(m, "gl.bankeq");
  for (int band = 0; band < ut::kBqBands; ++band)
    for (int t = 0; t < ut::kBqTargets; ++t) {
      r.f32();  // current (morphed + mastered) value: follows from the banks
      int mode = static_cast<int>(static_cast<int32_t>(r.u32()));
      fx::set(m, ut::bqMode(t, band), std::clamp(mode, 0, 2));
    }
  for (int k = 0; k <= ut::kBqBanks; ++k)
    for (int band = 0; band < ut::kBqBands; ++band)
      for (int t = 0; t < ut::kBqTargets; ++t) {
        float v = r.f32();
        fx::set(m, ut::bqValue(k, t, band), std::isfinite(v) ? clampd(v, 0, 2) : 1.0);
      }
  std::vector<double> t = ints(r, 10);
  if (t.size() < 10) return false;
  const int sendOnly = r.u8(), add = r.u8(), master = r.u8();
  fx::set(m, ut::BqShift, equoShift(t[0]));
  fx::set(m, ut::BqMix, equoMix(t[1]));
  fx::set(m, ut::BqMorph, equoMorph(t[2]));
  fx::set(m, ut::BqSmooth, equoSmooth(t[3]));
  fx::set(m, ut::BqOutput, ut::bqOutputDbOfRaw(clampd(t[4], 0, 256)));
  fx::set(m, ut::BqOversample, t[5] > 0 ? 1 : 0);
  fx::set(m, ut::BqBandwidth, clampd(t[6], 80, 500) / 300.0);
  const int send = static_cast<int>(t[7]);
  fx::set(m, ut::BqSendTo, send < 0 ? -1 : std::min(send + 1, 125));  // FL insert index (shown + 1) → mixer index
  fx::set(m, ut::BqEditTarget, std::clamp(static_cast<int>(t[8]), 0, 2));
  fx::set(m, ut::BqEditBank, std::clamp(static_cast<int>(t[9]), 0, 7));
  fx::set(m, ut::BqSendOnly, sendOnly ? 1 : 0);
  fx::set(m, ut::BqAddMode, add ? 1 : 0);
  fx::set(m, ut::BqEditMaster, master ? 1 : 0);
  if (send >= 0) c.report.warn("EQUO: send track routed to the mixer insert with the same number (numbering unverified)");
  m.exact = send < 0;  // every control maps by its reference law; only the send destination numbering is unverified
  static const KnobTable tab = [] {
    KnobTable k = {{0, -12000, 12000, ut::BqShift, equoShift},
                   {1, -128, 128, ut::BqMix, equoMix},
                   {2, 0, 7000, ut::BqMorph, equoMorph},
                   {3, 0, 1024, ut::BqSmooth, equoSmooth},
                   {4, 0, 256, ut::BqOutput, [](double v) { return ut::bqOutputDbOfRaw(clampd(v, 0, 256)); }}};
    // Band values 5..841: index − 5 = band + 31 · (curve + 3 · bank).
    for (int i = 0; i < ut::kBqValues; ++i) {
      int band = i % ut::kBqBands, curve = (i / ut::kBqBands) % ut::kBqTargets, bank = i / (ut::kBqBands * ut::kBqTargets);
      k.push_back({5 + i, 0, 1073741824.0, ut::bqValue(bank, curve, band), equoBand});
    }
    return k;
  }();
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Frequency Splitter → Band Splitter
double splitHz(double v) { return expMap(v / 65536.0, 30.0, 16000.0); }

// State (version 4, 113 bytes): tags 0-13, 3 send targets (FL insert number,
// 0 = none), monitor ×2, view bits, mute bits (word 20: bits 0-2 send mutes,
// 3-5 main mutes; 1 = muted), analyser options. 2-band mode uses the low and
// high bands (indices 0 and 2).
bool decodeFreqSplitter(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 21 * 4) return false;
  const int version = r.i32();
  std::vector<double> t = ints(r, 21);
  if (t.size() < 21) return false;
  fx::begin(m, "gl.splitter");
  const bool three = t[5] != 0;
  const int bits = static_cast<int>(t[20]);
  fx::set(m, sp::SpModel, 1);
  fx::set(m, sp::SpBands, three ? 1 : 0);
  fx::set(m, sp::SpFreq1, splitHz(std::min(t[0], t[1])));
  fx::set(m, sp::SpFreq2, splitHz(std::max(t[0], t[1])));
  fx::set(m, sp::SpSlopeA, std::clamp(static_cast<int>(t[2]), 0, 7));
  fx::set(m, sp::SpSlopeB, std::clamp(static_cast<int>(t[3]), 0, 7));
  fx::set(m, sp::SpLinear, t[4] != 0 ? 1 : 0);
  fx::set(m, sp::SpQuality, std::clamp(static_cast<int>(t[6]), 0, 5));
  fx::set(m, sp::SpLinSpeed, sp::splitLinSpeedMs(clampd(t[7], 0, 65536)));
  bool routed = false;
  for (int band = 0; band < (three ? 3 : 2); ++band) {
    const int k = three || band == 0 ? band : 2;  // FL band index
    fx::set(m, sp::splitParam2(band, sp::SxMain), sp::splitLevelDb(t[static_cast<size_t>(11 + k)]));
    fx::set(m, sp::splitParam2(band, sp::SxSend), sp::splitLevelDb(t[static_cast<size_t>(8 + k)]));
    fx::set(m, sp::splitParam(band, sp::SbMute), (bits >> (3 + k)) & 1);
    fx::set(m, sp::splitParam2(band, sp::SxSendMute), (bits >> k) & 1);
    const int to = static_cast<int>(t[static_cast<size_t>(14 + k)]);
    fx::set(m, sp::splitParam2(band, sp::SxSendTo), to > 0 ? std::min(to, 500) : -1);  // FL insert number = mixer index
    routed = routed || to > 0;
  }
  if (version > 4) c.report.warn("Frequency Splitter: newer state version, read as version 4");
  if (routed) c.report.warn("Frequency Splitter: band sends go to the mixer inserts with the same numbers");
  // Exact for the IIR model (the linear-phase FIR designer differs in detail).
  m.exact = version <= 4 && t[4] == 0 && !routed;  // send-target numbering unverified
  KnobTable tab = {{0, 0, 65536, sp::SpFreq1, splitHz},
                   {1, 0, 65536, sp::SpFreq2, splitHz},
                   {2, 0, 7, sp::SpSlopeA, [](double v) { return clampd(std::round(v), 0, 7); }},
                   {3, 0, 7, sp::SpSlopeB, [](double v) { return clampd(std::round(v), 0, 7); }},
                   {4, 0, 1, sp::SpLinear, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }},
                   {6, 0, 5, sp::SpQuality, [](double v) { return clampd(std::round(v), 0, 5); }},
                   {7, 0, 65536, sp::SpLinSpeed, [](double v) { return sp::splitLinSpeedMs(clampd(v, 0, 65536)); }}};
  for (int k = 0; k < 3; ++k) {
    if (!three && k == 1) continue;
    const int band = three || k == 0 ? k : 1;
    auto lvl = [](double v) { return sp::splitLevelDb(v); };
    auto mute = [](double v) { return v >= 0.5 ? 1.0 : 0.0; };
    tab.push_back({8 + k, -1800, 1800, sp::splitParam2(band, sp::SxSend), lvl});
    tab.push_back({11 + k, -1800, 1800, sp::splitParam2(band, sp::SxMain), lvl});
    tab.push_back({14 + k, 0, 1, sp::splitParam2(band, sp::SxSendMute), mute});
    tab.push_back({17 + k, 0, 1, sp::splitParam(band, sp::SbMute), mute});
  }
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Emphasis → Loudness
bool decodeEmphasis(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 13 * 4) return false;
  r.i32();
  std::vector<double> t = ints(r, 13);
  if (t.size() < 13) return false;
  fx::begin(m, "gl.emphasis");
  KnobTable tab = {
      {1, 0, 1, dy::EmDelta, nullptr},
      {3, 0, 1, dy::EmStereoMode, nullptr},
      {5, 0, 10000, dy::EmInput, [](double v) { return clampd(v / 10000.0 * 24.0, 0, 24); }},
      {6, 0, 10000, dy::EmLink, [](double v) { return clampd(v / 10000.0, 0, 1); }},
      {8, 0, 10000, dy::EmCeiling, [](double v) { return clampd(-(1.0 - v / 10000.0) * 12.0, -12, 0); }},
      {9, 0, 1, dy::EmLevelMatch, nullptr},
      {10, 0, 4, dy::EmOversample, [](double v) { return clampd(v, 0, 2); }},
  };
  fx::applyTable(m, tab, t);
  fx::set(m, dy::EmCharacter, clampd(t[4], 0, 3));
  fx::set(m, dy::EmClipHard, t[2] != 0 ? 1 : 0);
  m.exact = false;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Emphasizer → Enhance
bool decodeEmphasizer(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 5 * 4) return false;
  r.i32();
  std::vector<double> t = ints(r, 5);
  if (t.size() < 5) return false;
  fx::begin(m, "gl.enhance");
  KnobTable tab = {{1, 0, 10000, dy::EnAmount, [](double v) { return clampd(v / 10000.0, 0, 1); }}};
  fx::applyTable(m, tab, t);
  m.exact = false;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Low Lifter → Low Lifter
bool decodeLowLifter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 15 * 4) return false;
  r.i32();
  std::vector<double> t = ints(r, 15);
  if (t.size() < 15) return false;
  fx::begin(m, "gl.lowlift");
  auto lvl = [](double v) { return clampd(v / 10000.0, 0, 1); };
  KnobTable tab = {
      {1, 0, 10000, pp::LlMix, lvl},
      {2, 0, 10000, pp::LlLowGain, [](double v) { return clampd((v - 5000.0) / 5000.0 * 12.0, -12, 12); }},
      {3, 0, 10000, pp::LlSlope, lvl},
      {5, 0, 1, pp::LlInvertHarm, nullptr},
      {6, 0, 10000, pp::LlDrive, [](double v) { return clampd(v / 10000.0 * 24.0, 0, 24); }},
      {7, 0, 10000, pp::LlDiffuse, lvl},
      {8, 0, 10000, pp::LlDiffuseAmt, lvl},
      {9, 0, 10000, pp::LlHighGain, [](double v) { return clampd(dbOf(v / 10000.0, -12), -12, 12); }},
      {10, 0, 10000, pp::LlCrossover, [](double v) { return expMap(v / 10000.0, 40, 400); }},
      {11, 0, 10000, pp::LlLowCut, [](double v) { return expMap(v / 10000.0, 10, 80); }},
      {12, 0, 1, pp::LlMultiband, nullptr},
      {13, 0, 1, pp::LlHighOn, nullptr},
      {14, 0, 1, pp::LlLowOn, nullptr},
  };
  fx::applyTable(m, tab, t);
  m.exact = false;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Distructor / Hardcore → Amp Rack
bool decodeRack(Context& c, const std::vector<Event>& evs, PluginMapping& m, const char* what) {
  if (!pluginState(evs)) return false;
  fx::begin(m, "gl.amprack");
  m.exact = false;
  c.report.warn(std::string(what) + ": module rack settings not decoded; default Amp Rack used");
  return true;
}
bool decodeDistructor(Context& c, const std::vector<Event>& evs, PluginMapping& m) { return decodeRack(c, evs, m, "Distructor"); }
bool decodeHardcore(Context& c, const std::vector<Event>& evs, PluginMapping& m) { return decodeRack(c, evs, m, "Hardcore"); }

}  // namespace

void registerFx2Tone(std::map<std::string, PluginDecoder>& d) {
  d["Fruity Blood Overdrive"] = decodeBloodOverdrive;
  d["Fruity Fast LP"] = decodeFastLp;
  d["Fruity Bass Boost"] = decodeBassBoost;
  d["Fruity Squeeze"] = decodeSqueeze;
  d["Fruity Parametric EQ"] = decodeParamEq1;
  d["EQUO"] = decodeEquo;
  d["Frequency Splitter"] = decodeFreqSplitter;
  d["Emphasis"] = decodeEmphasis;
  d["Emphasizer"] = decodeEmphasizer;
  d["Low Lifter"] = decodeLowLifter;
  d["Distructor"] = decodeDistructor;
  d["Hardcore"] = decodeHardcore;
}

}  // namespace gl::flp::fx2

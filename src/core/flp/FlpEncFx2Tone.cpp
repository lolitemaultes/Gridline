// Gridline — FLP export: plugin state encoders for FlpFx2Tone.cpp (Distructor, EQUO, Emphasis, Emphasizer, Frequency Splitter, Bass Boost, Blood Overdrive, Fast LP, Parametric EQ, Squeeze, Hardcore, Low Lifter).
// The reverse of the decoders in those files; see FlpEncode.h for the model.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/dynamics/modrack_ref.h"
#include "../../packs/spectral/spectral.h"
#include "../../packs/utility/utility.h"
#include "../PluginParams.h"
#include "FlpEncode.h"

namespace gl::flp::enc {

namespace {

namespace dy = gl::packs::dynamics;
namespace ut = gl::packs::utility;
namespace sp = gl::packs::spectral;

float paramOf(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.0f; }

// A state of `version` (i32) followed by i32 values.
Bytes intState(int version, std::initializer_list<int> values) {
  ByteWriter w;
  w.i32(version);
  for (int v : values) w.i32(v);
  return w.b;
}
// Field of the i32 at tag `tag` after a version word.
Field tagField(int gl, int tag, double lo, double hi) { return i32(gl, 4 + 4 * static_cast<size_t>(tag), lo, hi); }

// ------------------------------------------------------------------ Blood Overdrive → gl.overdrive
// version 3, pre band, colour, pre amp, x100, post filter, post gain, 2 spare.
Encoder bloodOverdrive() {
  Encoder e;
  e.flName = "Fruity Blood Overdrive";
  e.glType = "gl.overdrive";
  e.nativeRank = 0;
  e.defaults = [] {
    const int preBand = 0, colour = 5000, preAmp = 0, x100 = 0, postFilter = 0, postGain = 10000, spare = 0;
    return intState(3, {preBand, colour, preAmp, x100, postFilter, postGain, spare, spare});
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{tagField(dy::OdPreBand, 0, 0, 10000),    tagField(dy::OdColor, 1, 0, 10000),
                              tagField(dy::OdPreAmp, 2, 0, 10000),     tagField(dy::OdBoost, 3, 0, 1),
                              tagField(dy::OdPostFilter, 4, 0, 10000), tagField(dy::OdPostGainK, 5, 0, 10000),
                              tagField(dy::OdPostGain, 5, 0, 10000)};
  };
  return e;
}

// ------------------------------------------------------------------ Fast LP → gl.fastlp
Encoder fastLp() {
  Encoder e;
  e.flName = "Fruity Fast LP";
  e.glType = "gl.fastlp";
  e.nativeRank = 0;
  e.defaults = [] {
    const int cutoff = 1024, resonance = 0;
    return intState(1, {cutoff, resonance});
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{tagField(ut::QlCutK, 0, 0, 1024), tagField(ut::QlResK, 1, 0, 1024), tagField(ut::QlCutoff, 0, 0, 1024),
                              tagField(ut::QlReso, 1, 0, 1024)};
  };
  return e;
}

// ------------------------------------------------------------------ Bass Boost → gl.bassboost
Encoder bassBoost() {
  Encoder e;
  e.flName = "Fruity Bass Boost";
  e.glType = "gl.bassboost";
  e.nativeRank = 0;
  e.defaults = [] {
    const int frequency = 512, amount = 768;
    return intState(1, {frequency, amount});
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{tagField(ut::BbFreq, 0, 0, 1024), tagField(ut::BbAmount, 1, 0, 1024)};
  };
  return e;
}

// ------------------------------------------------------------------ Squeeze → gl.squeeze
// version 0, then 12 i32 (tags 0..11).
Encoder squeeze() {
  Encoder e;
  e.flName = "Fruity Squeeze";
  e.glType = "gl.squeeze";
  e.nativeRank = 0;
  e.defaults = [] {
    const int gain = 127, mix = 64, steps = 1, filterOn = 0, filterType = 0, resonance = 32768, cutoff = 32768,
              filterPos = 0, relation = 0, impact = 0, preserve = 7500, amount = 0;
    return intState(0, {gain, mix, steps, filterOn, filterType, resonance, cutoff, filterPos, relation, impact, preserve, amount});
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{tagField(dy::SqGainRef, 0, 0, 160),       tagField(dy::SqMix, 1, 0, 128),
                              tagField(dy::SqStepsRef, 2, 1, 32768),    tagField(dy::SqFilterOn, 3, 0, 1),
                              tagField(dy::SqFilterType, 4, 0, 4),      tagField(dy::SqResonance, 5, 0, 65536),
                              tagField(dy::SqCutoffPos, 6, 0, 65536),   tagField(dy::SqFilterPos, 7, 0, 1),
                              tagField(dy::SqRelation, 8, 0, 3),        tagField(dy::SqImpactMs, 9, 0, 44100),
                              tagField(dy::SqPreserveMs, 10, 0, 44100), tagField(dy::SqAmountRef, 11, 0, 65536)};
  };
  return e;
}

// ------------------------------------------------------------------ Parametric EQ (v1) → gl.eq
// No version word: 7 levels, 7 frequencies, 7 widths, 7 types, main level.
Encoder paramEq1() {
  Encoder e;
  e.flName = "Fruity Parametric EQ";
  e.glType = "gl.eq";
  e.nativeRank = 0;  // the classic band model (EqModel = 2) is this plugin's
  e.accepts = [](const PluginState& s) { return std::lround(paramOf(s, pp::EqModel)) == 2; };
  e.defaults = [] {
    const int level = 0;
    const int freq[7] = {5777, 13024, 19904, 33145, 42327, 49307, 55825};
    const int width[7] = {17500, 17500, 17500, 17500, 24000, 24000, 17500};
    const int type[7] = {5, 6, 6, 6, 6, 6, 7};  // low shelf, 5 × peaking, high shelf
    const int main = 128;
    ByteWriter w;
    for (int b = 0; b < 7; ++b) w.i32(level);
    for (int v : freq) w.i32(v);
    for (int v : width) w.i32(v);
    for (int v : type) w.i32(v);
    w.i32(main);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int b = 0; b < 7; ++b) {
      const size_t at = 4 * static_cast<size_t>(b);
      f.push_back(i32(pp::eqParam(b, pp::EqGain), at, -1800, 1800));
      f.push_back(i32(pp::eqParam(b, pp::EqFreq), at + 28, 0, 65536));
      f.push_back(i32(pp::eqFl(b, pp::EqWidth), at + 56, 0, 65536));
      f.push_back(i32(pp::eqParam(b, pp::EqType), at + 84, 0, 7));
    }
    f.push_back(i32(pp::EqMainLin, 112, 0, 256));
    f.push_back(i32(pp::EqMain, 112, 0, 256));
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ EQUO → gl.bankeq
// u32 version; 31 bands × 3 curves × {f32 current value, i32 band mode};
// 9 banks × 31 × 3 f32 (bank 8 = master); i32 shift, mix, morph, smoothing,
// volume, oversample, bandwidth, send track, edited curve, edited bank; u8
// send only, add mode, master; v4: u32 editor flags.
constexpr size_t kEquoBanks = 4 + 31 * 3 * 8;            // 748
constexpr size_t kEquoInts = kEquoBanks + 9 * 31 * 3 * 4;  // 4096

Encoder equo() {
  Encoder e;
  e.flName = "EQUO";
  e.glType = "gl.bankeq";
  e.nativeRank = 0;
  e.defaults = [] {
    const float unity = 1.0f;  // curve value 1 = 0 dB / centre
    const int bandMode = 0;
    const int shift = 0, mix = 128, morph = 0, smoothing = 170, volume = 128, oversample = 0, bandwidth = 150, sendTrack = -1,
              editCurve = 0, editBank = 0;
    ByteWriter w;
    w.u32(4);  // version
    for (int i = 0; i < ut::kBqBands * ut::kBqTargets; ++i) {
      w.f32(unity);  // current value
      w.i32(bandMode);
    }
    for (int i = 0; i < (ut::kBqBanks + 1) * ut::kBqBands * ut::kBqTargets; ++i) w.f32(unity);
    for (int v : {shift, mix, morph, smoothing, volume, oversample, bandwidth, sendTrack, editCurve, editBank}) w.i32(v);
    w.u8(0);  // send only
    w.u8(0);  // add mode
    w.u8(0);  // master edited
    w.u32(1); // editor flags
    return w.b;
  };
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int band = 0; band < ut::kBqBands; ++band)
      for (int t = 0; t < ut::kBqTargets; ++t)
        f.push_back(i32(ut::bqMode(t, band), 4 + static_cast<size_t>(band * ut::kBqTargets + t) * 8 + 4, 0, 2));
    for (int k = 0; k <= ut::kBqBanks; ++k)
      for (int band = 0; band < ut::kBqBands; ++band)
        for (int t = 0; t < ut::kBqTargets; ++t) {
          const size_t at = kEquoBanks + static_cast<size_t>((k * ut::kBqBands + band) * ut::kBqTargets + t) * 4;
          f.push_back(Field{ut::bqValue(k, t, band), at, Type::F32, 0, 2, [](double v) { return v; }});
        }
    const size_t a = kEquoInts;
    f.push_back(i32(ut::BqShift, a, -12000, 12000));
    f.push_back(i32(ut::BqMix, a + 4, -128, 128));
    f.push_back(i32(ut::BqMorph, a + 8, 0, 7000));
    f.push_back(i32(ut::BqSmooth, a + 12, 0, 1024));
    f.push_back(i32(ut::BqOutput, a + 16, 0, 256));
    f.push_back(i32(ut::BqOversample, a + 20, 0, 1));
    f.push_back(i32(ut::BqBandwidth, a + 24, 80, 500));
    f.push_back(i32(ut::BqSendTo, a + 28, -1, 125));
    f.push_back(i32(ut::BqEditTarget, a + 32, 0, 2));
    f.push_back(i32(ut::BqEditBank, a + 36, 0, 7));
    f.push_back(field(ut::BqSendOnly, a + 40, Type::U8, 0, 1));
    f.push_back(field(ut::BqAddMode, a + 41, Type::U8, 0, 1));
    f.push_back(field(ut::BqEditMaster, a + 42, Type::U8, 0, 1));
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Frequency Splitter → gl.splitter
// version 4, i32 tags 0..20 (word 20 = mute bits: 0-2 send mutes, 3-5 main
// mutes), then analyser display settings. 2-band mode uses FL bands 0 and 2.
Encoder freqSplitter() {
  Encoder e;
  e.flName = "Frequency Splitter";
  e.glType = "gl.splitter";
  e.nativeRank = 0;
  e.defaults = [] {
    const int lowMid = 19800, midHigh = 48065, slopeA = 3, slopeB = 3, linear = 0, threeBands = 1, quality = 2,
              linSpeed = 45820, level = 0, sendTarget = 0, monitor = 2, viewBits = 12, muteBits = 0;
    ByteWriter w;
    w.i32(4);  // version
    for (int v : {lowMid, midHigh, slopeA, slopeB, linear, threeBands, quality, linSpeed}) w.i32(v);
    for (int k = 0; k < 6; ++k) w.i32(level);  // send levels, main levels
    for (int k = 0; k < 3; ++k) w.i32(sendTarget);
    for (int v : {monitor, monitor, viewBits, muteBits}) w.i32(v);
    // Analyser display settings (not decoded).
    const int anaSpeed = 274, anaRange = 768, anaFloor = 256, anaColour = 512;
    for (int v : {anaSpeed, anaRange, anaFloor, anaColour, anaColour, anaColour}) w.i32(v);
    w.u8(0);
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    const bool three = getU32(b, 4 + 5 * 4) != 0;
    std::vector<Field> f = {tagField(sp::SpBands, 5, 0, 1),        tagField(sp::SpFreq1, 0, 0, 65536),
                            tagField(sp::SpFreq2, 1, 0, 65536),    tagField(sp::SpSlopeA, 2, 0, 7),
                            tagField(sp::SpSlopeB, 3, 0, 7),       tagField(sp::SpLinear, 4, 0, 1),
                            tagField(sp::SpQuality, 6, 0, 5),      tagField(sp::SpLinSpeed, 7, 0, 65536)};
    for (int band = 0; band < (three ? 3 : 2); ++band) {
      const int k = three || band == 0 ? band : 2;
      f.push_back(tagField(sp::splitParam2(band, sp::SxMain), 11 + k, -1800, 1800));
      f.push_back(tagField(sp::splitParam2(band, sp::SxSend), 8 + k, -1800, 1800));
      f.push_back(tagField(sp::splitParam2(band, sp::SxSendTo), 14 + k, 0, 500));
    }
    return f;
  };
  // Band count (it decides the band mapping) and the mute bits (one word for six switches).
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    const bool three = paramOf(g, sp::SpBands) != 0;
    putI32(b, 4 + 5 * 4, three ? 1 : 0);
    uint32_t bits = getU32(b, 4 + 20 * 4);
    for (int band = 0; band < (three ? 3 : 2); ++band) {
      const int k = three || band == 0 ? band : 2;
      auto setBit = [&](int bit, bool on) { bits = on ? (bits | (1u << bit)) : (bits & ~(1u << bit)); };
      setBit(3 + k, paramOf(g, sp::splitParam(band, sp::SbMute)) != 0);
      setBit(k, paramOf(g, sp::splitParam2(band, sp::SxSendMute)) != 0);
    }
    putU32(b, 4 + 20 * 4, bits);
  };
  return e;
}

// ------------------------------------------------------------------ Emphasis → gl.emphasis
// version 10, i32 tags 0..12, then 3 option bytes.
Encoder emphasis() {
  Encoder e;
  e.flName = "Emphasis";
  e.glType = "gl.emphasis";
  e.nativeRank = 0;
  e.defaults = [] {
    const int delta = 0, unknown1 = 0, envOrder = 1, truePeak = 0, midSide = 0, mode = 0, inputGain = 0, linking = 10000,
              amount = 10000, ceiling = 9912, levelMatch = 0, oversampling = 2, hardness = 7, tail14 = 0, tail15 = 1;
    ByteWriter w;
    w.i32(10);  // version
    for (int v : {delta, unknown1, envOrder, truePeak, midSide, mode, inputGain, linking, amount, ceiling, levelMatch,
                  oversampling, hardness, tail14, tail15})
      w.i32(v);
    // Option bytes (not decoded).
    w.u8(0);
    w.u8(1);
    w.u8(1);
    return w.b;
  };
  // Tags 0..12 after the version; v10: the DC-block byte at 66.
  e.fields = [](const Bytes& b) {
    std::vector<Field> f = {tagField(dy::EmDelta, 1, 0, 1),        tagField(dy::EmStereoMode, 4, 0, 1),
                            tagField(dy::EmCharacter, 5, 0, 3),    tagField(dy::EmInput, 6, 0, 10000),
                            tagField(dy::EmLink, 7, 0, 10000),     tagField(dy::EmCeiling, 9, 0, 10000),
                            tagField(dy::EmLevelMatch, 10, 0, 1),  tagField(dy::EmOversample, 11, 0, 4),
                            tagField(dy::EmClipHard, 12, 0, 9)};
    // J-35: the reference controls (stored as the plugin keeps them).
    for (const Field& x : {tagField(dy::EmBypass, 0, 0, 1), tagField(dy::EmOrder, 2, 0, 4), tagField(dy::EmTruePeak, 3, 0, 1),
                           tagField(dy::EmMode, 5, 0, 3), tagField(dy::EmAmount, 8, 0, 10000), tagField(dy::EmCeilKnob, 9, 0, 10000),
                           tagField(dy::EmOs5, 11, 0, 4), tagField(dy::EmHardness, 12, 0, 9)})
      f.push_back(x);
    const int ver = static_cast<int32_t>(getU32(b, 0));
    if (ver >= 9 && b.size() >= 65) f.push_back(field(dy::EmLowLatency, 64, Type::U8, 0, 1));
    if (ver >= 10 && b.size() >= 66) f.push_back(field(dy::EmInstant, 65, Type::U8, 0, 1));
    if (ver >= 10 && b.size() >= 67) f.push_back(field(dy::EmDcBlock, 66, Type::U8, 0, 1));
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Emphasizer → gl.enhance
// version 2: input gain, emphasis, clipping, level match, bypass, output gain, u8 option.
Encoder emphasizer() {
  Encoder e;
  e.flName = "Emphasizer";
  e.glType = "gl.enhance";
  e.nativeRank = 0;  // the peak-emphasis mode (EnCustom = 2) is this plugin's
  e.accepts = [](const PluginState& s) { return std::lround(paramOf(s, dy::EnCustom)) == 2; };
  e.defaults = [] {
    const int inputGain = 0, emphasis = 5013, clipping = 0, levelMatch = 0, bypass = 0, outputGain = 10000;
    Bytes b = intState(2, {inputGain, emphasis, clipping, levelMatch, bypass, outputGain});
    b.push_back(1);  // option byte (not decoded)
    return b;
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f = {tagField(dy::EnPeakIn, 0, 0, 10000), tagField(dy::EnPeakEmph, 1, 0, 10000),
                            tagField(dy::EnPeakSoft, 2, 0, 1),   tagField(dy::EnPeakMatch, 3, 0, 1),
                            tagField(dy::EnPeakBypass, 4, 0, 1)};
    if (static_cast<int32_t>(getU32(b, 0)) >= 2) f.push_back(tagField(dy::EnPeakOut, 5, 0, 10000));
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Low Lifter → gl.lowlift
// version 6, i32 tags 0..14, then display settings.
Encoder lowLifter() {
  Encoder e;
  e.flName = "Low Lifter";
  e.glType = "gl.lowlift";
  e.nativeRank = 0;
  e.defaults = [] {
    const int unknown0 = 1, dryWet = 10000, lowGain = 4999, slope = 6993, lookahead = 1, invert = 0, preGain = 4998,
              diffMix = 0, diffCoef = 5000, highGain = 10000, crossover = 8920, lowCut = 0, multiband = 0, highBand = 1,
              lowBand = 1;
    Bytes b = intState(6, {unknown0, dryWet, lowGain, slope, lookahead, invert, preGain, diffMix, diffCoef, highGain, crossover,
                           lowCut, multiband, highBand, lowBand});
    // Display settings (not decoded): two flags, four view words.
    ByteWriter w;
    w.u8(1);
    w.u8(0);
    for (int v : {3, 3, 2, 2}) w.i32(v);
    b.insert(b.end(), w.b.begin(), w.b.end());
    return b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{tagField(pp::LlOn, 0, 0, 1),           tagField(pp::LlMix, 1, 0, 10000),
                              tagField(pp::LlLowDb, 2, 0, 10000),     tagField(pp::LlSlopeDb, 3, 0, 10000),
                              tagField(pp::LlLookahead, 4, 0, 1),     tagField(pp::LlInvertHarm, 5, 0, 1),
                              tagField(pp::LlPreDb, 6, 0, 10000),     tagField(pp::LlDiffuse, 7, 0, 10000),
                              tagField(pp::LlDiffCoef, 8, 0, 10000),  tagField(pp::LlHighDb, 9, 0, 10000),
                              tagField(pp::LlXoverHz, 10, 0, 10000),  tagField(pp::LlLowCutHz, 11, 0, 10000),
                              tagField(pp::LlMultiband, 12, 0, 1),    tagField(pp::LlHighOn, 13, 0, 1),
                              tagField(pp::LlLowOn, 14, 0, 1)};
  };
  return e;
}

// ------------------------------------------------------------------ Distructor / Hardcore → gl.amprack
// Hardcore is not decoded: no state is built (FL loads its defaults).
// J-39: Distructor written from the module rack (gl.amprack RkModel 1, the J-35
// decoder's raw fields): host u32 version 5, u32 module count, per module u32
// version 4, kind, algorithm, oversampling, u8 remove DC, u8 disabled, filter
// type, u8 filter option, impulse, 49 parameters, spare (0), chorus mode; then
// u8 soft clip, u32 4 + the four modules' automation blocks, u32 0 (spare), u8
// 1 (meters). A guitar rack (RkModel 0) has no FL counterpart: its slots are
// approximated by modules (the default rack is written without a state — FL's
// own defaults import as it).
namespace distructor {

namespace dr = gl::packs::dynamics::drack;

void putModule(ByteWriter& w, const dr::ModuleState& m) {
  w.u32(4);
  w.i32(m.kind), w.i32(m.algo), w.i32(m.os);
  w.u8(static_cast<uint32_t>(m.removeDc)), w.u8(static_cast<uint32_t>(m.disabled));
  w.i32(m.ftype);
  w.u8(static_cast<uint32_t>(m.fopt));
  w.i32(m.ir);
  for (int v : m.p) w.i32(v);
  w.i32(0);  // spare
  w.i32(m.chorusMode);
}
Bytes build(const std::vector<dr::ModuleState>& ms, bool softClip, const int (&slot)[dy::kDrModules]) {
  ByteWriter w;
  w.u32(5);
  w.u32(static_cast<uint32_t>(ms.size()));
  for (const auto& m : ms) putModule(w, m);
  w.u8(softClip ? 1 : 0);
  w.u32(static_cast<uint32_t>(dy::kDrModules));
  for (int s : slot) w.i32(s);
  w.u32(0);
  w.u8(1);
  return w.b;
}
int ip(const PluginState& g, int k) { return static_cast<int>(std::lround(paramOf(g, k))); }

// Module rack parameters → modules.
Bytes fromModules(const PluginState& g) {
  const int count = std::clamp(ip(g, dy::DrCount), 0, dy::kDrModules);
  std::vector<dr::ModuleState> ms;
  for (int k = 0; k < count; ++k) {
    dr::ModuleState m;
    int* f[dy::DmP0] = {&m.kind, &m.algo, &m.os, &m.removeDc, &m.disabled, &m.ftype, &m.fopt, &m.ir, &m.chorusMode};
    for (int q = 0; q < dy::DmP0; ++q) *f[q] = ip(g, dy::drParam(k, q));
    for (int q = 0; q < dy::kDrParams; ++q) m.p[q] = ip(g, dy::drParam(k, dy::DmP0 + q));
    ms.push_back(m);
  }
  int slot[dy::kDrModules];
  for (int k = 0; k < dy::kDrModules; ++k) slot[k] = std::clamp(ip(g, dy::DrSlot0 + k), 0, 3);
  return build(ms, paramOf(g, dy::DrSoftClip) >= 0.5f, slot);
}

int32_t r10k(double v) { return static_cast<int32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 10000.0)); }

// Guitar rack slots → the nearest modules (an approximation: the guitar
// rack's effects are Gridline's own).
Bytes fromSlots(const PluginState& g) {
  std::vector<dr::ModuleState> ms;
  for (int s = 0; s < dy::kRackSlots && static_cast<int>(ms.size()) < dy::kDrModules; ++s) {
    auto P = [&](int f) { return static_cast<double>(paramOf(g, dy::rkParam(s, f))); };
    dr::ModuleState m;
    dr::moduleDefaults(m);
    m.os = 1, m.removeDc = 1, m.ftype = 2, m.ir = 1;
    m.disabled = P(dy::RsOn) < 0.5 ? 1 : 0;
    const double k0 = P(dy::RsK0), k1 = P(dy::RsK1), k3 = P(dy::RsK3), k5 = P(dy::RsK5);
    switch (static_cast<int>(std::lround(P(dy::RsType)))) {
      case dy::RmOverdrive: m.kind = 0, m.algo = 0, m.p[2] = r10k(k0), m.p[1] = r10k(k1), m.p[3] = r10k(k5 / 0.85); break;
      case dy::RmFuzz: m.kind = 0, m.algo = 3, m.p[23] = static_cast<int>(std::lround(std::clamp(k0, 0.0, 1.0) * 1000.0)); break;
      case dy::RmWah: m.kind = 1, m.ftype = 12, m.p[39] = r10k(k0), m.p[40] = r10k(k3); break;
      case dy::RmChorus: m.kind = 2; break;
      default: continue;  // no module for this slot type
    }
    m.p[37] = r10k(P(dy::RsMix));
    ms.push_back(m);
  }
  const int slot[dy::kDrModules] = {0, 1, 2, 3};
  return build(ms, paramOf(g, dy::RkSoftClip) >= 0.5f, slot);
}

Encoder encoder() {
  Encoder e;
  e.flName = "Distructor";
  e.glType = "gl.amprack";
  e.nativeRank = 0;  // the module rack's FL counterpart (and the guitar rack's nearest)
  e.accepts = [](const PluginState& s) { return ip(s, dy::RkModel) != 2; };
  e.defaults = [] {
    const int slot[dy::kDrModules] = {0, 1, 2, 3};
    return build({}, true, slot);
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginMapping m;
    if (decodeState("Distructor", b, m, c) && m.state.params == g.params) return;
    const int model = ip(g, dy::RkModel);
    if (model == 1) return void(b = fromModules(g));
    // The default guitar rack is what FL's own Distructor defaults import as: no state.
    if (g.params == defaultPluginState("gl.amprack").params) return b.clear();
    b = fromSlots(g);
  };
  return e;
}

}  // namespace distructor

// J-39: Hardcore written from the pedal board (gl.amprack RkModel 2): "SSLF" |
// u32 size | "1RCH" | 50 × f32 (the board values as the decoder reads them) |
// 256-byte preset name (empty).
namespace hardcore {

Bytes build(const PluginState& g) {
  ByteWriter w;
  w.bytes(reinterpret_cast<const uint8_t*>("SSLF"), 4);
  w.u32(4 + 200 + 256);
  w.bytes(reinterpret_cast<const uint8_t*>("1RCH"), 4);
  for (int k = 0; k < 50; ++k) w.f32(paramOf(g, dy::PbValue0 + k));
  w.zeros(256);
  return w.b;
}

Encoder encoder() {
  Encoder e;
  e.flName = "Hardcore";
  e.glType = "gl.amprack";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return distructor::ip(s, dy::RkModel) == 2; };
  e.defaults = [] {
    PluginState g = defaultPluginState("gl.amprack");
    return build(g);
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginMapping m;
    if (decodeState("Hardcore", b, m, c) && m.state.params == g.params) return;
    if (b.size() >= 12 + 200 && std::memcmp(b.data() + 8, "1RCH", 4) == 0) {  // in place (the name kept)
      for (int k = 0; k < 50; ++k) putF32(b, 12 + 4 * static_cast<size_t>(k), paramOf(g, dy::PbValue0 + k));
      return;
    }
    b = build(g);
  };
  return e;
}

}  // namespace hardcore


}  // namespace

void registerFx2Tone(std::vector<Encoder>& v) {
  v.push_back(bloodOverdrive());
  v.push_back(fastLp());
  v.push_back(bassBoost());
  v.push_back(squeeze());
  v.push_back(paramEq1());
  v.push_back(equo());
  v.push_back(freqSplitter());
  v.push_back(emphasis());
  v.push_back(emphasizer());
  v.push_back(lowLifter());
  v.push_back(distructor::encoder());  // J-39: the module rack (and the guitar rack, approximated)
  v.push_back(hardcore::encoder());    // J-39: the pedal board
}

}  // namespace gl::flp::enc

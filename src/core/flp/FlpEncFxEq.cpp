// Gridline — FLP export: plugin state encoders for FlpFxEq.cpp (Parametric EQ
// 2, Balance, Soft Clipper, Fast Dist, Stereo Shaper / Enhancer, Free Filter,
// Filter, WaveShaper). The reverse of the decoders in that file; see
// FlpEncode.h for the model. (Fast Dist and WaveShaper: FlpEncFxK3.cpp, whose
// decoders override these.)
#include <algorithm>
#include <cmath>
#include "FlpEncode.h"
#include "../PluginParams.h"
#include "../../packs/utility/utility.h"

namespace gl::flp::fx {
double flEq2FreqHz(double v);      // FlpFxEq.cpp: the decoder's laws
double flEq2BandwidthQ(double v);
}  // namespace gl::flp::fx

namespace gl::flp::enc {

namespace {

namespace ut = gl::packs::utility;

// Int32 knob k of a state whose knobs follow a version word.
constexpr size_t knob(int k) { return 4 + 4 * static_cast<size_t>(k); }

// Balance: i32 pan (-128..128), i32 volume (0..320, 256 = unity).
Encoder balance() {
  Encoder e;
  e.flName = "Fruity Balance";
  e.glType = "gl.utility";
  e.nativeRank = 10;
  e.defaults = [] {
    ByteWriter w;
    w.i32(0);
    w.i32(256);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(pp::UtlPan, 0, -128, 128), i32(pp::UtlGain, 4, 0, 320)};
  };
  return e;
}

// ------------------------------------------------------------------ Parametric EQ 2
// Version 8 (FL 25): 36 knob words by tag (levels 0-6, frequencies 7-13,
// widths 14-20, types 21-27, orders 28-34, main level 35), the same 36 for the
// A/B spare state, then the editor options.
constexpr int kEq2Bands = 7;
constexpr int kEq2Knobs = 36;

void eq2Knobs(ByteWriter& w) {
  for (int b = 0; b < kEq2Bands; ++b) w.i32(0);  // levels: 0 dB
  // Frequencies: evenly spread over the log scale, band b at (b + 1.5) / 9 of
  // the 0..65536 range (≈ 43 Hz .. 6.5 kHz).
  for (int b = 0; b < kEq2Bands; ++b) w.i32(static_cast<int32_t>(std::lround(65536.0 * (b + 1.5) / 9.0)));
  // Widths: the shelves (bands 1 and 7) steeper than the peaks.
  const int32_t shelfWidth = 40124, peakWidth = 25412;
  for (int b = 0; b < kEq2Bands; ++b) w.i32(b == 0 || b == kEq2Bands - 1 ? shelfWidth : peakWidth);
  // Types (FL numbering): 5 low shelf, 6 peaking, 7 high shelf.
  for (int b = 0; b < kEq2Bands; ++b) w.i32(b == 0 ? 5 : b == kEq2Bands - 1 ? 7 : 6);
  for (int b = 0; b < kEq2Bands; ++b) w.i32(0);  // orders: 0 = the type's standard slope
  w.i32(0);                                       // main level: 0 dB
}

Encoder eq2() {
  Encoder e;
  e.flName = "Fruity Parametric EQ 2";
  e.glType = "gl.eq";
  e.nativeRank = 0;  // the band model Gridline's EQ follows (EqModel 1)
  e.defaults = [] {
    ByteWriter w;
    w.u32(8);     // version (FL 25)
    eq2Knobs(w);  // current state
    eq2Knobs(w);  // A/B spare state
    // Editor options (monitor, display, HQ, band tokens, …): not read by the
    // decoder; the values a fresh FL 25 instance stores.
    static const uint8_t kOptions[] = {3, 0, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0,
                                       0, 0, 0, 0, 64, 127, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0,
                                       1, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0};
    w.bytes(kOptions, sizeof kOptions);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    // J-51: with the decoder's laws (FlpFxEq.cpp decodeEq2) for the solver.
    auto level = [](double r, const Bytes&) { return std::clamp(r / 100.0, -18.0, 18.0); };
    for (int b = 0; b < kEq2Bands; ++b) {
      f.push_back(i32(pp::eqParam(b, pp::EqType), knob(21 + b), 0, 7));
      f.push_back(lawField(i32(pp::eqParam(b, pp::EqGain), knob(b), -1800, 1800), level));
      f.push_back(lawField(i32(pp::eqParam(b, pp::EqFreq), knob(7 + b), 0, 65536),
                           [](double r, const Bytes&) { return std::clamp(fx::flEq2FreqHz(r), 20.0, 20000.0); }));
      // One width word: the band's width and (derived) Q.
      f.push_back(lawField(i32(pp::eqFl(b, pp::EqWidth), knob(14 + b), 0, 65536), [](double r, const Bytes&) { return std::clamp(r / 65536.0, 0.0, 1.0); }));
      f.push_back(lawField(i32(pp::eqParam(b, pp::EqQ), knob(14 + b), 0, 65536),
                           [](double r, const Bytes&) { return std::clamp(fx::flEq2BandwidthQ(r), 0.1, 18.0); }));
      f.push_back(i32(pp::eqFl(b, pp::EqFlOrder), knob(28 + b), -3, 3));
    }
    f.push_back(lawField(i32(pp::EqMain, knob(35), -1800, 1800), level));
    return f;
  };
  static_assert(kEq2Knobs == 5 * kEq2Bands + 1, "EQ 2 knob layout");
  return e;
}

// ------------------------------------------------------------------ Soft Clipper
// No version word: i32 threshold (1..127), i32 post gain (0..160, 128 = unity).
Encoder softClipper() {
  Encoder e;
  e.flName = "Fruity Soft Clipper";
  e.glType = "gl.clipper";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.i32(100);  // threshold
    w.i32(128);  // post gain: unity
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(pp::ClpThreshold, 0, 1, 127), i32(pp::ClpPostGain, 4, 0, 160)};
  };
  return e;
}

// ------------------------------------------------------------------ Stereo Shaper
// Version 0: R→L, L, R, L→R (±25600, 12800 = unity), delay, dephase (±4096),
// difference output, effects position.
Encoder stereoShaper() {
  Encoder e;
  e.flName = "Fruity Stereo Shaper";
  e.glType = "gl.stereomatrix";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(0);      // version
    w.i32(0);      // R → L
    w.i32(12800);  // L → L: unity
    w.i32(12800);  // R → R: unity
    w.i32(0);      // L → R
    w.i32(0);      // delay
    w.i32(0);      // dephase
    w.i32(0);      // difference output: off
    w.i32(1);      // effects position: post
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(ut::SmRL, knob(0), -25600, 25600),    i32(ut::SmLL, knob(1), -25600, 25600),
                              i32(ut::SmRR, knob(2), -25600, 25600),    i32(ut::SmLR, knob(3), -25600, 25600),
                              i32(ut::SmDelayK, knob(4), -4096, 4096),  i32(ut::SmDephaseK, knob(5), -4096, 4096),
                              i32(ut::SmFxPos, knob(7), 0, 1),         i32(ut::SmDiffTo, knob(6), 0, 500)};
  };
  return e;
}

// ------------------------------------------------------------------ Stereo Enhancer
// No version word: pan (±128), volume (0..320, 256 = unity), separation
// (±96), phase offset (±512), effects position, phase inversion (0..2).
Encoder stereoEnhancer() {
  Encoder e;
  e.flName = "Fruity Stereo Enhancer";
  e.glType = "gl.stereo";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.i32(0);    // pan
    w.i32(256);  // volume: unity
    w.i32(0);    // separation
    w.i32(0);    // phase offset
    w.i32(1);    // effects position: post
    w.i32(0);    // phase inversion: none
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(pp::StPan, 0, -128, 128),     i32(pp::StVolK, 4, 0, 320), i32(pp::StSep, 8, -96, 96),
                              i32(pp::StPhaseK, 12, -512, 512), i32(pp::StFxPos, 16, 0, 1), i32(pp::StInvert, 20, 0, 2)};
  };
  return e;
}

// ------------------------------------------------------------------ Free Filter
// Version 2: type (100..700), frequency, Q, gain (0..1024), u8 DC remover.
Encoder freeFilter() {
  Encoder e;
  e.flName = "Fruity Free Filter";
  e.glType = "gl.freefilter";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(2);    // version
    w.i32(100);  // type: low pass
    w.i32(512);  // frequency: mid scale
    w.i32(169);  // Q
    w.i32(676);  // gain: 0 dB
    w.u8(1);     // DC remover: on
    return w.b;
  };
  e.fields = [](const Bytes& s) {
    std::vector<Field> f{i32(ut::FfType, knob(0), 100, 700), i32(ut::FfFreq, knob(1), 0, 1024), i32(ut::FfQK, knob(2), 0, 1024),
                         i32(ut::FfGain, knob(3), 0, 1024)};
    if (s.size() > knob(4) && getU32(s, 0) >= 2) f.push_back(field(ut::FfDc, knob(4), Type::U8, 0, 1));  // DC remover
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Filter
// Version 2: cutoff, resonance, low / band / high levels (0..1024), x2 slope, u8 DC remover.
Encoder fruityFilter() {
  Encoder e;
  e.flName = "Fruity Filter";
  e.glType = "gl.filter";
  e.nativeRank = 0;  // Gridline's multi-mode blend (FltBlend) follows this plugin
  e.defaults = [] {
    ByteWriter w;
    w.u32(2);     // version
    w.i32(1024);  // cutoff: fully open
    w.i32(205);   // resonance: 20 %
    w.i32(1024);  // low pass level: full
    w.i32(0);     // band pass level
    w.i32(512);   // high pass level
    w.i32(0);     // x2 slope: off
    w.u8(1);      // DC remover: on
    return w.b;
  };
  e.fields = [](const Bytes& s) {
    // The cutoff word gives both the classic knob (FltCutK) and its Hz view (FltCutoff).
    std::vector<Field> f{i32(pp::FltCutK, knob(0), 0, 1024),    i32(pp::FltCutoff, knob(0), 0, 1024), i32(pp::FltResK, knob(1), 0, 1024),
                         i32(pp::FltLPLevel, knob(2), 0, 1024), i32(pp::FltBPLevel, knob(3), 0, 1024), i32(pp::FltHPLevel, knob(4), 0, 1024),
                         i32(pp::FltSlope, knob(5), 0, 1)};
    if (s.size() > knob(6) && getU32(s, 0) >= 2) f.push_back(field(pp::FltDcBlock, knob(6), Type::U8, 0, 1));  // DC remover
    return f;
  };
  return e;
}

}  // namespace

void registerFxEq(std::vector<Encoder>& v) {
  v.push_back(balance());
  v.push_back(eq2());
  v.push_back(softClipper());
  v.push_back(stereoShaper());
  v.push_back(stereoEnhancer());
  v.push_back(freeFilter());
  v.push_back(fruityFilter());
}

}  // namespace gl::flp::enc

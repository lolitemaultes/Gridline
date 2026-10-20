// Gridline — FLP export: plugin state encoders for FlpFxTime.cpp (Reeverb 2,
// Delay 2 / 3, Delay Bank, Hyper Chorus, Flanger). Fruity Chorus and Fruity
// Flangus are decoded by FlpFxK3.cpp (registered later), so their encoders
// live in FlpEncFxK3.cpp. The reverse of the decoders; see FlpEncode.h.
#include <algorithm>
#include <cmath>
#include "FlpEncode.h"
#include "../PluginParams.h"
#include "../../packs/spacefx/spacefx.h"

namespace gl::flp::enc {

namespace {

namespace sfx = gl::packs::spacefx;

// One i32 knob stored by tag: offset = base + 4 × tag.
struct Tag {
  int gl;
  int tag;
  double lo, hi;
};
std::vector<Field> tagFields(size_t base, std::initializer_list<Tag> tags) {
  std::vector<Field> v;
  for (const Tag& t : tags) v.push_back(i32(t.gl, base + 4 * static_cast<size_t>(t.tag), t.lo, t.hi));
  return v;
}

float param(const PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.f; }

// ---------------------------------------------------------------- Fruity Reeverb 2
// u32 version 10001, i32 × 15 knobs (tags 0..14), u8 tempo-based pre-delay, u8 mid/side input.
constexpr uint32_t kRv2Version = 10001;
Encoder reeverb2() {
  using namespace pp;
  Encoder e;
  e.flName = "Fruity Reeverb 2";
  e.glType = "gl.reverb";
  e.nativeRank = 0;  // the plugin Gridline's reverb is modelled on
  e.accepts = [](const PluginState& s) { return param(s, RvbModel) < 0.5f; };  // hall; the plate model comes from Fruity Reeverb
  e.defaults = [] {
    ByteWriter w;
    w.u32(kRv2Version);
    w.i32(75);    // 0 low cut (Hz; 19 = off)
    w.i32(40);    // 1 high cut (× 100 Hz)
    w.i32(0);     // 2 pre-delay (1/384 s)
    w.i32(50);    // 3 room size
    w.i32(100);   // 4 diffusion
    w.i32(15);    // 5 decay (1/10 s)
    w.i32(40);    // 6 high damping (× 100 Hz)
    w.i32(100);   // 7 bass multiplier (%)
    w.i32(500);   // 8 crossover (Hz)
    w.i32(0);     // 9 stereo separation
    w.i32(128);   // 10 dry
    w.i32(64);    // 11 early reflections
    w.i32(64);    // 12 wet
    w.i32(684);   // 13 modulation speed
    w.i32(0);     // 14 modulation depth
    w.u8(0);      // tempo-based pre-delay
    w.u8(0);      // mid/side input
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    // Older states (version 10000 / none) are read with 13 knobs; written as found.
    const uint32_t ver = getU32(b, 0);
    const bool versioned = ver == 10000 || ver == 10001;
    const size_t base = versioned ? 4 : 0;
    const size_t knobs = ver == 10001 && b.size() >= 64 ? 15 : 13;
    std::vector<Field> v = tagFields(base, {{RvbLowCut, 0, 19, 3000},  {RvbHighCut, 1, 5, 221},  {RvbPreDelay, 2, 0, 384},
                                            {RvbSize, 3, 1, 100},      {RvbDiffusion, 4, 0, 100}, {RvbDecayTime, 5, 1, 200},
                                            {RvbDampHz, 6, 5, 221},    {RvbBassMult, 7, 20, 300}, {RvbCrossover, 8, 25, 2000},
                                            {RvbWidth, 9, -64, 64},    {RvbDry, 10, 0, 160},      {RvbEarly, 11, 0, 160},
                                            {RvbWet, 12, 0, 160}});
    if (knobs == 15) {
      for (const Field& f : tagFields(base, {{RvbModRate, 13, 0, 2048}, {RvbModDepth, 14, 0, 2048}})) v.push_back(f);
    }
    const size_t flags = base + knobs * 4;
    v.push_back(field(RvbPreSync, flags, Type::U8, 0, 1));
    v.push_back(field(RvbInputMode, flags + 1, Type::U8, 0, 1));
    return v;
  };
  return e;
}

// ---------------------------------------------------------------- Fruity Delay 3
// u32 version 1, i32 × 26 (tags 0..25).
Encoder delay3() {
  using namespace pp;
  Encoder e;
  e.flName = "Fruity Delay 3";
  e.glType = "gl.delay";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return std::lround(param(s, DlyModel)) == 1; };  // reference delay model
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);       // version
    w.i32(6000);    // 0 input
    w.i32(1);       // 1 delay type (stereo)
    w.i32(1);       // 2 tempo sync
    w.i32(0);       // 3 keep pitch
    w.i32(192);     // 4 time (1/192 beat when synced)
    w.i32(3000);    // 5 R/L time offset (centre)
    w.i32(500);     // 6 time smoothing (ms)
    w.i32(6000);    // 7 stereo spread
    w.i32(3000);    // 8 modulation rate
    w.i32(0);       // 9 modulation time
    w.i32(0);       // 10 modulation cutoff
    w.i32(0);       // 11 diffusion level
    w.i32(0);       // 12 diffusion spread
    w.i32(1);       // 13 distortion type
    w.i32(3000);    // 14 feedback
    w.i32(0);       // 15 filter mode
    w.i32(6000);    // 16 feedback cutoff
    w.i32(0);       // 17 feedback resonance
    w.i32(0);       // 18 distortion threshold
    w.i32(0);       // 19 distortion hardness
    w.i32(0);       // 20 distortion symmetry
    w.i32(60000);   // 21 rate reduction (off)
    w.i32(6000);    // 22 bit reduction (off)
    w.i32(6000);    // 23 output wet
    w.i32(0);       // 24 output tone
    w.i32(6000);    // 25 output dry
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return tagFields(4, {{DlyInput, 0, 0, 6000},       {DlyType3, 1, 0, 3},           {DlySync, 2, 0, 1},
                         {DlyKeepPitch, 3, 0, 1},      {DlyTicks, 4, 1, 768},         {DlyTimeMs, 4, 1, 768},
                         {DlyTimeR, 5, 0, 6000},       {DlyTimeSmooth, 6, 0, 1000},   {DlySpread3, 7, 0, 6000},
                         {DlyModRate3, 8, 0, 6000},    {DlyModDepth, 9, 0, 6000},     {DlyModCutoff, 10, 0, 6000},
                         {DlyDiffusion, 11, 0, 6000},  {DlyDiffSpread, 12, 0, 6000},  {DlyDistType, 13, 0, 1},
                         {DlyFeedback, 14, 0, 6000},   {DlyFilter3, 15, 0, 3},        {DlyCutoff3, 16, 0, 6000},
                         {DlyResonance, 17, 0, 6000},  {DlyDistThresh, 18, 0, 6000},  {DlyDistHardness, 19, 0, 6000},
                         {DlyDistSymmetry, 20, 0, 6000}, {DlyRate3, 21, 0, 60000},    {DlyBits, 22, 0, 6000},
                         {DlyWet, 23, 0, 6000},        {DlyTone, 24, -3000, 3000},    {DlyDry, 25, 0, 6000}});
  };
  return e;
}

// ---------------------------------------------------------------- Fruity Delay 2
// i32 × 8: input pan, input, dry, feedback, time (1/192 beat), offset, mode, cutoff.
Encoder delay2() {
  using namespace pp;
  Encoder e;
  e.flName = "Fruity Delay 2";
  e.glType = "gl.delay";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return std::lround(param(s, DlyModel)) == 2; };  // classic tempo delay model
  e.defaults = [] {
    ByteWriter w;
    w.i32(0);    // 0 input pan
    w.i32(128);  // 1 input level
    w.i32(128);  // 2 dry
    w.i32(64);   // 3 feedback
    w.i32(144);  // 4 time (1/192 beat: 3/4 beat)
    w.i32(0);    // 5 stereo offset
    w.i32(0);    // 6 mode (normal)
    w.i32(128);  // 7 feedback cutoff (open)
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return tagFields(0, {{DlyClsPan, 0, -128, 128}, {DlyClsIn, 1, 0, 160},  {DlyClsDry, 2, 0, 160}, {DlyClsFb, 3, 0, 127},
                         {DlyTicks, 4, 1, 768},     {DlyClsOffset, 5, -512, 512}, {DlyClsMode, 6, 0, 2}, {DlyClsCoef, 7, 0, 128}});
  };
  return e;
}

// ---------------------------------------------------------------- Fruity Delay Bank
// u32 version 16, i32 × 132 (tags 0..3 dry / wet / input / feedback, then 8
// units × 16 knobs), i32 oversampling, then per unit a 32-byte record: i32
// input filter type, i32 input stages, i32 feedback filter type, i32 feedback
// stages, u8 input filter post, i32 feedback mode, i32 grain division, i32
// grain shape, u8 enabled, u8 leave first echo, u8 tempo-based time.
constexpr size_t kDbFlags = 4 + 132 * 4;  // oversampling int
size_t dbRecord(int unit) { return kDbFlags + 4 + 32 * static_cast<size_t>(unit); }

Encoder delayBank() {
  using namespace sfx;
  Encoder e;
  e.flName = "Fruity Delay Bank";
  e.glType = "gl.multidelay";
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return param(s, MdModel) >= 0.5f; };  // reference model
  e.defaults = [] {
    ByteWriter w;
    w.u32(16);  // version
    w.i32(128);  // dry
    w.i32(128);  // wet
    w.i32(128);  // input
    w.i32(128);  // feedback scale
    for (int k = 0; k < kTaps; ++k) {
      w.i32(0);       // input pan
      w.i32(128);     // input level
      w.i32(33145);   // input filter cutoff
      w.i32(17500);   // input filter resonance
      w.i32(0);       // input filter gain
      w.i32(0);       // feedback pan
      w.i32(64);      // feedback level
      w.i32(0);       // stereo separation
      w.i32(1440);    // time (1/1920 beat: 3/4 beat)
      w.i32(0);       // stereo time offset
      w.i32(33145);   // feedback filter cutoff
      w.i32(17500);   // feedback filter resonance
      w.i32(0);       // feedback filter gain
      w.i32(0);       // output pan
      w.i32(128);     // output level
      w.i32(k < kTaps - 1 ? 0 : -1);  // to next unit (none after the last)
    }
    w.i32(0);  // oversampling off
    for (int k = 0; k < kTaps; ++k) {
      w.i32(0);  // input filter type (off)
      w.i32(0);  // input filter stages
      w.i32(0);  // feedback filter type (off)
      w.i32(0);  // feedback filter stages
      w.u8(0);   // input filter post
      w.i32(1);  // feedback mode (normal)
      w.i32(0);  // grain division
      w.i32(0);  // grain shape
      w.u8(k == 0 ? 1 : 0);  // enabled (first unit)
      w.u8(0);   // leave first echo
      w.u8(1);   // tempo-based time
    }
    return w.b;
  };
  e.fields = [](const Bytes&) {
    std::vector<Field> v = tagFields(4, {{MdDry, 0, 0, 128}, {MdWet, 1, 0, 128}, {MdInput, 2, 0, 128}, {MdFbScale, 3, 0, 128}});
    v.push_back(i32(MdOversample, kDbFlags, 0, 1));
    for (int k = 0; k < kTaps; ++k) {
      auto P = [k](int f) { return mdParam(k, f); };
      auto X = [k](int x) { return mdExt(k, x); };
      const size_t u = 4 + 4 * (4 + 16 * static_cast<size_t>(k)), r = dbRecord(k);
      auto U = [u](int gl, int f, double lo, double hi) { return i32(gl, u + 4 * static_cast<size_t>(f), lo, hi); };
      v.push_back(field(P(MdOn), r + 29, Type::U8, 0, 1));
      v.push_back(U(P(MdInPan), 0, -128, 128));
      v.push_back(U(P(MdInLevel), 1, -160, 160));
      v.push_back(i32(P(MdInFType), r + 0, 0, 7));
      v.push_back(U(P(MdInCut), 2, 0, 65536));
      v.push_back(U(P(MdInRes), 3, 0, 65536));
      v.push_back(U(X(MdxInGain), 4, 0, 256));
      v.push_back(i32(X(MdxInStages), r + 4, 0, 2));
      v.push_back(field(X(MdxInPost), r + 16, Type::U8, 0, 1));
      v.push_back(U(P(MdFbPan), 5, -128, 128));
      v.push_back(U(P(MdFeedback), 6, -128, 128));
      v.push_back(i32(P(MdFbMode), r + 17, 0, 3));
      v.push_back(U(P(MdSep), 7, -128, 128));
      v.push_back(field(X(MdxTempo), r + 31, Type::U8, 0, 1));
      v.push_back(U(P(MdSteps), 8, 0, 7680));
      v.push_back(U(P(MdTimeMs), 8, 0, 7680));
      v.push_back(U(P(MdOffset), 9, -512, 512));
      v.push_back(i32(P(MdFbFType), r + 8, 0, 7));
      v.push_back(U(P(MdFbCut), 10, 0, 65536));
      v.push_back(U(P(MdFbRes), 11, 0, 65536));
      v.push_back(U(X(MdxFbGain), 12, 0, 256));
      v.push_back(i32(X(MdxFbStages), r + 12, 0, 2));
      v.push_back(U(P(MdOutPan), 13, -128, 128));
      v.push_back(U(P(MdOutLevel), 14, -160, 160));
      if (k < kTaps - 1) v.push_back(U(P(MdToNext), 15, -160, 160));
      v.push_back(i32(P(MdGateDiv), r + 21, 0, 16));
      v.push_back(i32(P(MdGateShape), r + 25, -128, 128));
      v.push_back(field(X(MdxLeaveFirst), r + 30, Type::U8, 0, 1));
    }
    return v;
  };
  return e;
}

// ---------------------------------------------------------------- Hyper Chorus
// u32 version 2, i32 × 11 (tags 0..10), i32 spare.
Encoder hyperChorus() {
  using namespace sfx;
  Encoder e;
  e.flName = "Hyper Chorus";
  e.glType = "gl.ensemble";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(2);       // version
    w.i32(500);     // 0 delay
    w.i32(500);     // 1 feedback
    w.i32(500);     // 2 modulation rate
    w.i32(500);     // 3 modulation amount
    w.i32(500);     // 4 movement
    w.i32(500);     // 5 movement speed
    w.i32(0);       // 6 delay lines
    w.i32(22000);   // 7 low pass (Hz)
    w.i32(120);     // 8 high pass (Hz)
    w.i32(500);     // 9 dry / wet
    w.i32(500);     // 10 LFO phase difference
    w.i32(0);       // spare
    return w.b;
  };
  e.accepts = [](const PluginState& s) { return param(s, EnModel) >= 0.5f; };  // reference engine
  e.fields = [](const Bytes&) {
    static const double lo[11] = {0, 0, 0, 0, 0, 0, 0, 7140, 120, 0, 0}, hi[11] = {1000, 1000, 1000, 1000, 1000, 1000, 1, 22000, 10900, 1000, 1000};
    std::vector<Field> v;
    for (int i = 0; i < 11; ++i) v.push_back(i32(EnK0 + i, 4 + 4 * static_cast<size_t>(i), lo[i], hi[i]));
    return v;
  };
  return e;
}

// ---------------------------------------------------------------- Fruity Flanger
// u32 version 1, i32 × 12: delay, depth, rate, phase, damp, shape, feedback,
// invert feedback, invert wet, dry, wet, cross.
Encoder flanger() {
  using namespace pp;
  Encoder e;
  e.flName = "Fruity Flanger";
  e.glType = "gl.flanger";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);      // version
    w.i32(0);      // 0 delay
    w.i32(2000);   // 1 depth
    w.i32(2000);   // 2 rate
    w.i32(118);    // 3 phase (degrees)
    w.i32(0);      // 4 damp
    w.i32(0);      // 5 shape
    w.i32(23);     // 6 feedback (%)
    w.i32(1024);   // 7 invert feedback
    w.i32(1024);   // 8 invert wet
    w.i32(3500);   // 9 dry
    w.i32(3000);   // 10 wet
    w.i32(2500);   // 11 cross
    return w.b;
  };
  e.accepts = [](const PluginState& s) { return param(s, FlgModel) >= 0.5f; };  // reference engine
  e.fields = [](const Bytes&) {
    return tagFields(4, {{FlgDelayN, 0, 0, 1024}, {FlgDepthN, 1, 0, 5000}, {FlgRateN, 2, 0, 5000},  {FlgPhase, 3, 0, 360},
                         {FlgDampN, 4, 0, 5000},  {FlgShape, 5, 0, 1024}, {FlgFeedN, 6, 0, 100},   {FlgInvFb, 7, 0, 1024},
                         {FlgInvertWet, 8, 0, 1024}, {FlgDryN, 9, 0, 5000}, {FlgWetN, 10, 0, 5000}, {FlgCrossN, 11, 0, 5000}});
  };
  return e;
}

}  // namespace

void registerFxTime(std::vector<Encoder>& v) {
  v.push_back(reeverb2());
  v.push_back(delay3());
  v.push_back(delay2());
  v.push_back(delayBank());
  v.push_back(hyperChorus());
  v.push_back(flanger());
}

}  // namespace gl::flp::enc

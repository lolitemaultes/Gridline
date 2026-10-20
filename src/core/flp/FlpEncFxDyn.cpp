// Gridline — FLP export: plugin state encoders for FlpFxDyn.cpp (Compressor, Limiter, Multiband Compressor, Maximus, Soundgoodizer, Transient Processor).
// The reverse of the decoders in those files; see FlpEncode.h for the model.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include "FlpEncode.h"
#include "../PluginParams.h"
#include "../../packs/dynamics/dynamics.h"

namespace gl::flp::enc {

namespace {

namespace dy = gl::packs::dynamics;

// Int32 knob k of a state whose knobs follow a version word.
constexpr size_t knob(int k) { return 4 + 4 * static_cast<size_t>(k); }

// ------------------------------------------------------------------ Fruity Limiter
// Version 7 (FL 25, 169 bytes): 18 knobs by tag, the same 18 for the A/B
// spare state, i32 scroll speed, i32 view, 4 × u8 display toggles, i32 ?,
// i32 sidechain input, u8 flags (bit 0: 0.2 dB safety margin).
constexpr int kLimKnobs = 18;
constexpr size_t kLimTail = 4 + 2 * 4 * kLimKnobs;  // after the spare state

void limiterKnobs(ByteWriter& w) {
  w.i32(1000);  // gain: 0 dB
  w.i32(1000);  // saturation threshold: 0 dB
  w.i32(1000);  // ceiling: 0 dB
  w.i32(1115);  // attack
  w.i32(3);     // attack curve 3
  w.i32(5000);  // release
  w.i32(3);     // release curve 3
  w.i32(2882);  // sustain (hold)
  w.i32(1000);  // compressor threshold: 0 dB
  w.i32(0);     // ratio 1:1
  w.i32(0);     // knee shape
  w.i32(0);     // compressor attack
  w.i32(6666);  // compressor release
  w.i32(1);     // compressor curve 1
  w.i32(650);   // RMS window
  w.i32(1000);  // noise gate floor: 0 dB
  w.i32(0);     // noise gate threshold: off
  w.i32(4187);  // noise gate release
}

Encoder limiter() {
  Encoder e;
  e.flName = "Fruity Limiter";
  e.glType = "gl.limiter";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(7);          // version (FL 25)
    limiterKnobs(w);   // current state
    limiterKnobs(w);   // A/B spare state
    w.i32(50);         // scroll speed
    w.i32(0);          // view: limiter page
    for (int i = 0; i < 4; ++i) w.u8(1);  // display toggles: on
    w.i32(0);          // reserved
    w.i32(0);          // sidechain input: own signal
    w.u8(0);           // flags: no safety margin
    return w.b;
  };
  e.fields = [](const Bytes& s) {
    static const int gl[kLimKnobs] = {pp::LimGain,       pp::LimSaturation,  pp::LimCeiling,    pp::LimAttack,    pp::LimAtkCurve,
                                      pp::LimRelease,    pp::LimRelCurve,    pp::LimHold,       pp::LimCompThresh, pp::LimCompRatio,
                                      pp::LimCompShape,  pp::LimCompAttack,  pp::LimCompRelease, pp::LimCompCurve, pp::LimCompRms,
                                      pp::LimGateFloor,  pp::LimGateThresh,  pp::LimGateRelease};
    static const int lo[kLimKnobs] = {0, 0, 0, 0, 1, 0, 1, 0, 0, -1000, -1000, 0, 0, 1, 0, 0, 0, 0};
    static const int hi[kLimKnobs] = {2000, 1000, 2000, 10000, 8, 10000, 8, 10000, 1000, 1000, 1000, 10000, 10000, 8, 10000, 1000, 1000, 10000};
    std::vector<Field> f;
    for (int k = 0; k < kLimKnobs; ++k) f.push_back(i32(gl[k], knob(k), lo[k], hi[k]));
    if (s.size() >= kLimTail + 8) f.push_back(i32(pp::LimView, kLimTail + 4, 0, 1));
    if (s.size() >= kLimTail + 20) f.push_back(i32(pp::LimSidechain, kLimTail + 16, 0, 1));
    if (s.size() >= kLimTail + 21 && s.size() >= 4 && getU32(s, 0) >= 7) f.push_back(field(pp::LimSafety, kLimTail + 20, Type::U8, 0, 1));
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Fruity Compressor
// Version 3 (FL 25, 36 bytes): threshold, ratio, gain, attack, release, type
// (0-7), i32 option flags, i32 sidechain input.
Encoder compressor() {
  Encoder e;
  e.flName = "Fruity Compressor";
  e.glType = "gl.comp";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(3);    // version (FL 25)
    w.i32(0);    // threshold: 0 dB
    w.i32(10);   // ratio 1:1
    w.i32(0);    // gain: 0 dB
    w.i32(150);  // attack 15 ms
    w.i32(200);  // release 200 ms
    w.i32(0);    // type: hard knee
    w.u32(1);    // option flags
    w.i32(0);    // sidechain input: own signal
    return w.b;
  };
  // The type word carries three Gridline params (knee, vintage character for
  // type % 4 == 2, fast release for types 4-7): set it from all of them.
  e.write = [](const PluginState& g, Bytes& s, const EncodeContext&) {
    if (s.size() < knob(6) || g.params.size() <= static_cast<size_t>(pp::CmpFastRelease)) return;
    const double knee = g.params[pp::CmpKnee];
    int idx = knee <= 3 ? 0 : knee <= 6.5 ? 1 : knee <= 11 ? 2 : 3;  // knee {0, 6, 7, 15} dB
    if (g.params[pp::CmpCharacter] >= 0.5) idx = 2;
    putI32(s, knob(5), idx + (g.params[pp::CmpFastRelease] >= 0.5 ? 4 : 0));
  };
  e.fields = [](const Bytes& s) {
    std::vector<Field> f{i32(pp::CmpThreshold, knob(0), -600, 0), i32(pp::CmpRatio, knob(1), 4, 300),  i32(pp::CmpOutput, knob(2), -300, 300),
                         i32(pp::CmpAttack, knob(3), 0, 4000),    i32(pp::CmpRelease, knob(4), 1, 4000), i32(pp::CmpKnee, knob(5), 0, 7)};
    if (s.size() >= 36 && getU32(s, 0) > 2) f.push_back(i32(pp::CmpSidechain, 32, 0, 1));
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Fruity Multiband Compressor
// Version 0 (124 bytes): 28 knobs by tag, i32 ?, i32 scroll speed.
Encoder multiband() {
  Encoder e;
  e.flName = "Fruity Multiband Compressor";
  e.glType = "gl.multiband";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(0);       // version
    w.i32(32768);   // master output gain: unity
    w.i32(0);       // limiter: off
    w.i32(0);       // filter: IIR
    // Bands high, mid, low: state (0 compress), edge(s), gain (unity),
    // threshold, ratio, attack 10 ms, release 100 ms, hardness.
    const int32_t gain = 21845, ratio = 32768, attack = 100, release = 1000, hard = 52429;
    w.i32(0), w.i32(17000), w.i32(gain), w.i32(45875), w.i32(ratio), w.i32(attack), w.i32(release), w.i32(hard);  // high
    w.i32(0), w.i32(17000), w.i32(6500), w.i32(gain), w.i32(48875), w.i32(ratio), w.i32(attack), w.i32(release), w.i32(hard);  // mid
    w.i32(0), w.i32(6500), w.i32(gain), w.i32(45875), w.i32(ratio), w.i32(attack), w.i32(release), w.i32(hard);  // low
    w.i32(0);   // reserved
    w.i32(50);  // scroll speed
    return w.b;
  };
  e.fields = [](const Bytes&) {
    static const int gl[28] = {pp::MbOutput,    pp::MbLimiter,     pp::MbFilterType,  pp::MbHighState, pp::MbHighXover, pp::MbHighGain,
                               pp::MbHighThresh, pp::MbHighRatio,  pp::MbHighAtk,     pp::MbHighRel,   pp::MbHighHard,  pp::MbMidState,
                               pp::MbMidHighEdge, pp::MbMidLowEdge, pp::MbMidGain,    pp::MbMidThresh, pp::MbMidRatio,  pp::MbMidAtk,
                               pp::MbMidRel,     pp::MbMidHard,     pp::MbLowState,    pp::MbLowXover,  pp::MbLowGain,   pp::MbLowThresh,
                               pp::MbLowRatio,   pp::MbLowAtk,      pp::MbLowRel,      pp::MbLowHard};
    static const int lo[28] = {0, 0, 0, 0, 20, 0, 0, 0, 1, 100, 0, 0, 20, 20, 0, 0, 0, 1, 100, 0, 0, 20, 0, 0, 0, 1, 100, 0};
    static const int hi[28] = {65536, 1,     1,     2,  20000, 65536, 65536, 65536, 1000, 10000, 65536, 2, 20000, 20000,
                               65536, 65536, 65536, 1000, 10000, 65536, 2,     20000, 65536, 65536, 65536, 1000, 10000, 65536};
    std::vector<Field> f;
    for (int k = 0; k < 28; ++k) f.push_back(i32(gl[k], knob(k), lo[k], hi[k]));
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Maximus
// Version 3.15 (FL 25, 1375 bytes): u16 3, u16 15, 7 globals (tags 0-6), 4 ×
// 13 band words (low, mid, high, master; tags 7-19), the same 59 words for the
// A/B spare state, the editor options, then two envelope records per band:
// {u32 3, u32 3, u32 n, n × {f64 dx, f64 y, f32 tension, 3 bytes, u8 segment},
// u32 0, 4 × i32 -1}; the first record of each pair is the band's
// compression curve (x = log9(2·in + 1), out = (9^y − 1)/2).
enum { BMode, BPre, BPost, BSep, BSatThres, BSatCeil, BAtt, BRel2, BAttCurve, BRel, BRelCurve, BWindow, BEnvMode, BCount };
constexpr int kMaxGlobals = 7, kMaxBands = 4, kMaxRecords = 2 * kMaxBands;
// Version 2 states have six globals (no low cut), version 3 on seven.
int maxGlobals(const Bytes& s) { return s.size() >= 2 && getU16(s, 0) < 3 ? 6 : kMaxGlobals; }
size_t bandWord(const Bytes& s, int band, int f) {
  return 4 + 4 * static_cast<size_t>(maxGlobals(s)) + 4 * static_cast<size_t>(band * BCount + f);
}
constexpr size_t kRecHead = 12, kRecPoint = 24, kRecTail = 20;

void maximusKnobs(ByteWriter& w) {
  w.i32(19800);  // low band frequency
  w.i32(1);      // low band slope: 24 dB/oct
  w.i32(48065);  // high band frequency
  w.i32(1);      // high band slope: 24 dB/oct
  w.i32(1115);   // LMH look-ahead
  w.i32(1000);   // LMH mix: full
  w.i32(32768);  // low cut
  for (int b = 0; b < kMaxBands; ++b) {
    w.i32(0);     // mode: active
    w.i32(1000);  // pre gain: 0 dB
    w.i32(1000);  // post gain: 0 dB
    w.i32(0);     // stereo separation
    w.i32(0);     // saturation: off
    w.i32(1000);  // saturation ceiling: 0 dB
    w.i32(1115);  // attack
    w.i32(0);     // release 2: off
    w.i32(2);     // attack / release 2 curve 2
    w.i32(5000);  // release
    w.i32(3);     // release curve 3
    w.i32(2882);  // sustain window
    w.i32(0);     // sustain mode: peak
  }
}

struct CurvePt {
  double x, y;
  float t;
};

// One envelope record: header, points (first one at x = 0 without a
// segment), tail.
void maximusRecord(ByteWriter& w, const std::vector<CurvePt>& pts) {
  w.u32(3);  // record kind
  w.u32(3);  // record format
  w.u32(static_cast<uint32_t>(pts.size()));
  double prev = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    w.f64(pts[i].x - prev);
    w.f64(pts[i].y);
    w.f32(pts[i].t);
    w.zeros(3);
    w.u8(i == 0 ? 0 : pts[i].t == 0 ? 2 : 1);  // segment: 2 straight, 1 curved
    prev = pts[i].x;
  }
  w.u32(0);
  for (int i = 0; i < 4; ++i) w.i32(-1);
}

// Offsets of the 8 envelope records: the chain of records that ends exactly
// at the end of the state (the option block before them varies by version).
std::vector<size_t> maximusRecords(const Bytes& s) {
  const size_t from = 4 + 2 * 4 * static_cast<size_t>(maxGlobals(s) + kMaxBands * BCount);
  for (size_t at = from; at + kRecHead <= s.size(); ++at) {
    std::vector<size_t> offs;
    size_t q = at;
    while (offs.size() < static_cast<size_t>(kMaxRecords) && q + kRecHead <= s.size()) {
      const uint32_t n = getU32(s, q + 8);
      if (n < 1 || n > 64) break;
      offs.push_back(q);
      q += kRecHead + kRecPoint * n + kRecTail;
    }
    if (offs.size() == static_cast<size_t>(kMaxRecords) && q == s.size()) return offs;
  }
  return {};
}

Encoder maximus() {
  Encoder e;
  e.flName = "Maximus";
  e.glType = "gl.master";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u16(3), w.u16(15);  // version (FL 25)
    maximusKnobs(w);      // current state
    maximusKnobs(w);      // A/B spare state
    w.i32(50);            // monitor scroll speed
    w.i32(3);             // selected band: master
    // Editor options (display toggles, monitor and view settings): not read
    // by the decoder; the values a fresh FL 25 instance stores.
    static const uint8_t kOptions[] = {0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0,
                                       0, 0, 0, 0, 6, 3, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0};
    w.bytes(kOptions, sizeof kOptions);
    // Per band: compression curve = limiter at 0 dB (unity up to the knee
    // at the middle, flat above), then the second envelope (unused, same shape).
    const std::vector<CurvePt> limit{{0, 0, 0}, {0.5, 0.5, 0}, {1, 0.5, 0}};
    for (int r = 0; r < kMaxRecords; ++r) maximusRecord(w, limit);
    return w.b;
  };
  // Curves (variable size) and the saturation mode (the sign of the
  // saturation word); its amount is solved as a field afterwards.
  e.write = [](const PluginState& g, Bytes& s, const EncodeContext&) {
    if (s.size() < bandWord(s, kMaxBands, 0) || g.params.size() < static_cast<size_t>(dy::kMsCount)) return;
    auto P = [&](int k) { return static_cast<double>(g.params[static_cast<size_t>(k)]); };
    for (int b = 0; b < kMaxBands; ++b) {
      const int mode = static_cast<int>(std::lround(P(dy::msExt(b, dy::MxSatMode))));
      const size_t at = bandWord(s, b, BSatThres);
      const int32_t sat = static_cast<int32_t>(getU32(s, at));
      if (mode == 0) putI32(s, at, 0);
      else if (mode == 1 && sat >= 0) putI32(s, at, -500);
      else if (mode == 2 && sat <= 0) putI32(s, at, 500);
    }
    for (int b = 0; b < kMaxBands; ++b) {
      std::vector<size_t> offs = maximusRecords(s);
      if (offs.empty()) return;
      const size_t at = offs[static_cast<size_t>(2 * b)];
      const size_t n = getU32(s, at + 8);
      // The curve the decoder reads now (≤ 8 points: the first ones and the last).
      std::vector<CurvePt> cur;
      double x = 0;
      for (size_t i = 0; i < n; ++i) {
        const size_t q = at + kRecHead + kRecPoint * i;
        x += getF64(s, q);
        cur.push_back({std::min(1.0, x), std::clamp(getF64(s, q + 8), 0.0, 1.0), std::clamp(getF32(s, q + 16), -1.0f, 1.0f)});
      }
      const int want = std::clamp(static_cast<int>(std::lround(P(dy::msExt(b, dy::MxPoints)))), 2, dy::kMsCurvePts);
      std::vector<CurvePt> pts;
      for (int i = 0; i < want; ++i) {
        const int k = dy::msExt(b, dy::MxPt0 + 3 * i);
        pts.push_back({P(k), P(k + 1), static_cast<float>(P(k + 2))});
      }
      bool same = cur.size() >= 2 && std::min(cur.size(), static_cast<size_t>(dy::kMsCurvePts)) == pts.size();
      for (size_t i = 0; same && i < pts.size(); ++i) {
        const CurvePt& c = cur[i == pts.size() - 1 ? cur.size() - 1 : i];
        same = static_cast<float>(c.x) == static_cast<float>(pts[i].x) && static_cast<float>(c.y) == static_cast<float>(pts[i].y) &&
               c.t == pts[i].t;
      }
      if (same) continue;
      ByteWriter w;
      maximusRecord(w, pts);
      const size_t end = at + kRecHead + kRecPoint * n + kRecTail;
      s.erase(s.begin() + static_cast<std::ptrdiff_t>(at), s.begin() + static_cast<std::ptrdiff_t>(end));
      s.insert(s.begin() + static_cast<std::ptrdiff_t>(at), w.b.begin(), w.b.end());
    }
  };
  e.fields = [](const Bytes& s) {
    std::vector<Field> f{i32(dy::MsLowFreq, 4, 0, 65536),    i32(dy::MsLowSlope, 8, 0, 1),     i32(dy::MsHighFreq, 12, 0, 65536),
                         i32(dy::MsHighSlope, 16, 0, 1),     i32(dy::MsLookahead, 20, 0, 10000), i32(dy::MsBandMix, 24, 0, 1000),
                         };
    if (maxGlobals(s) == kMaxGlobals) f.push_back(i32(dy::MsLowCut, 28, -1, 65536));
    if (s.size() < bandWord(s, kMaxBands, 0)) return f;
    for (int b = 0; b < kMaxBands; ++b) {
      auto P = [&](int k) { return dy::msParam(b, k); };
      auto X = [&](int k) { return dy::msExt(b, k); };
      f.push_back(i32(P(dy::MbMode), bandWord(s, b, BMode), 0, 3));
      f.push_back(i32(P(dy::MbPre), bandWord(s, b, BPre), 0, 2000));
      f.push_back(i32(P(dy::MbPost), bandWord(s, b, BPost), 0, 2000));
      f.push_back(i32(P(dy::MbStereo), bandWord(s, b, BSep), -1000, 1000));
      f.push_back(i32(P(dy::MbSatCeil), bandWord(s, b, BSatCeil), 0, 2000));
      // Saturation amount on the side of zero `write` chose (its sign is the mode).
      const int32_t sat = static_cast<int32_t>(getU32(s, bandWord(s, b, BSatThres)));
      if (sat != 0) f.push_back(i32(P(dy::MbSatThresh), bandWord(s, b, BSatThres), sat < 0 ? -1000 : 1, sat < 0 ? -1 : 1000));
      f.push_back(i32(P(dy::MbAttack), bandWord(s, b, BAtt), 0, 10000));
      f.push_back(i32(X(dy::MxRelease2), bandWord(s, b, BRel2), 0, 10000));
      f.push_back(i32(X(dy::MxAtkCurve), bandWord(s, b, BAttCurve), 1, 8));
      f.push_back(i32(P(dy::MbRelease), bandWord(s, b, BRel), 0, 10000));
      f.push_back(i32(X(dy::MxRelCurve), bandWord(s, b, BRelCurve), 1, 8));
      f.push_back(i32(P(dy::MbWindow), bandWord(s, b, BWindow), 0, 10000));
      f.push_back(i32(X(dy::MxEnvMode), bandWord(s, b, BEnvMode), 0, 1));
    }
    return f;
  };
  return e;
}

// ------------------------------------------------------------------ Soundgoodizer
// Version 3: i32 mode (A-D = 0-3), i32 amount (0..1000). The decoder reads
// the mode's Maximus program from FL's data folder.
Encoder soundgoodizer() {
  Encoder e;
  e.flName = "Soundgoodizer";
  e.glType = "gl.enhance";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(3);    // version
    w.i32(0);    // mode A
    w.i32(600);  // amount 60 %
    return w.b;
  };
  e.fields = [](const Bytes&) { return std::vector<Field>{i32(dy::EnMode, 4, 0, 3), i32(dy::EnAmount, 8, 0, 1000)}; };
  return e;
}

// ------------------------------------------------------------------ Transient Processor
// Version 1 (49 bytes): 9 knobs by tag (0..160 knobs centred at 80, speeds
// 0-2, on/off), i32 ?, i32 ?, u8.
Encoder transient() {
  Encoder e;
  e.flName = "Transient Processor";
  e.glType = "gl.transient";
  e.nativeRank = 0;
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);     // version
    w.i32(80);    // attack: neutral
    w.i32(0);     // drive
    w.i32(80);    // release: neutral
    w.i32(80);    // gain: 0 dB
    w.i32(1);     // attack speed: medium
    w.i32(1);     // release speed: medium
    w.i32(80);    // split frequency
    w.i32(80);    // balance: centre
    w.i32(1);     // effect: on
    w.i32(5);     // editor setting (not read by the decoder)
    w.i32(12800); // editor setting (not read by the decoder)
    w.u8(0);
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(pp::TrAttack, knob(0), 0, 160),     i32(pp::TrDrive, knob(1), 0, 160),
                              i32(pp::TrSustain, knob(2), 0, 160),    i32(pp::TrOutput, knob(3), 0, 160),
                              i32(pp::TrAttackSpeed, knob(4), 0, 2),  i32(pp::TrReleaseSpeed, knob(5), 0, 2),
                              i32(pp::TrSplit, knob(6), 0, 160),      i32(pp::TrBalance, knob(7), 0, 160),
                              i32(pp::TrEnabled, knob(8), 0, 1)};
  };
  return e;
}

}  // namespace

void registerFxDyn(std::vector<Encoder>& v) {
  v.push_back(limiter());
  v.push_back(compressor());
  v.push_back(multiband());
  v.push_back(maximus());
  v.push_back(soundgoodizer());
  v.push_back(transient());
}

}  // namespace gl::flp::enc

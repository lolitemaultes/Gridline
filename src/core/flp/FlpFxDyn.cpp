// Gridline — FLP import: decoders for FL dynamics: compressors, limiters,
// maximizers, transients.
//
// State layouts (observed in saved projects and the plugins' preset files):
//   Fruity Limiter (169 B): u32 version 7, 18 × i32 knobs in tag order, the
//     same 18 for the A/B spare state, i32 scroll speed, i32 view (0 limit,
//     1 comp), 4 × u8 display toggles, i32 ?, i32 sidechain input (0 = own
//     signal, n = n-th sidechain route), u8 ?.
//   Fruity Compressor (32 B): i32 version 2, 6 × i32 knobs (tag order), i32 ?.
//   Fruity Multiband Compressor (124 B): i32 version 0, 28 × i32 (tag order), i32 ?, i32 scroll speed.
//   Maximus (1375 B): u16 version, u16 ?, 7 global i32 (tags 0-6), then 4 × 13 i32
//     band blocks (low, mid, high, master; tags 7-19 each), the spare state,
//     then two envelope records per band (the first = compression curve).
//   Soundgoodizer (12 B): i32 version 3, i32 mode (A-D), i32 amount 0..1000.
//     The modes are Maximus programs shipped with FL (amount = their band mix):
//     the importer reads them from the FL data folder when it is available.
//   Transient Processor (49 B): i32 version 1, 9 × i32 (tag order), i32 ?, i32 ?, u8.
// κ2: knob laws re-derived from the plugins (exact where stated per decoder).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../../packs/dynamics/dynamics.h"
#include "../PluginParams.h"
#include "FlpFx.h"

namespace gl::flp::fx {

namespace {
namespace pp = gl::pp;


std::vector<int32_t> ints(Reader& r, size_t count) {
  std::vector<int32_t> v;
  for (size_t i = 0; i < count && r.has(4); ++i) v.push_back(r.i32());
  return v;
}
std::vector<double> asDouble(const std::vector<int32_t>& v) { return std::vector<double>(v.begin(), v.end()); }

// ---------------------------------------------------------------- Fruity Limiter
// Knob laws (κ2, exact): amplitude knobs a = (b^x - 1)/(b - 1), x = v/1000, with
// b = 7 for gain / noise / comp threshold and b = 3 for ceiling / saturation;
// times T(v) = 2·(501^(v/10000) - 1) ms, releases shown and used as 2·T.
double limAmp7(double v) { return v == 1000 ? 1.0 : (std::pow(7.0, v / 1000.0) - 1.0) / 6.0; }
double limAmp3(double v) { return v == 1000 ? 1.0 : (std::pow(3.0, v / 1000.0) - 1.0) / 2.0; }
double limMs(double v) { return 2.0 * (std::pow(501.0, v / 10000.0) - 1.0); }
double limDb(double a, double lo) { return a <= 0 ? lo : std::max(lo, 20.0 * std::log10(a)); }
double limRatio(double v) {  // FL ratio knob -1000..1000 → Gridline ratio (x:1; < 1 = expansion)
  double x = std::fabs(v) / 1000.0, e = std::exp(x * 3.9318256327243257) - 1.0;
  return v > 0 ? 1.0 + 0.38 * e : 1.0 / (1.0 + 0.06 * e);
}

std::shared_ptr<const KnobTable> limiterTable() {
  static auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 2000, pp::LimGain, [](double v) { return std::clamp(limDb(limAmp7(v), -36), -36.0, 24.0); }},
      {1, 0, 1000, pp::LimSaturation, [](double v) { return std::clamp(limDb(limAmp3(v), -36), -36.0, 0.0); }},
      {2, 0, 2000, pp::LimCeiling, [](double v) { return std::clamp(limDb(limAmp3(v), -36), -36.0, 12.0); }},
      {3, 0, 10000, pp::LimAttack, [](double v) { return std::clamp(limMs(v) * 0.001, 0.0001, 1.0); }},
      {4, 1, 8, pp::LimAtkCurve, [](double v) { return std::clamp(v - 1, 0.0, 7.0); }},
      {5, 0, 10000, pp::LimRelease, [](double v) { return std::clamp(2.0 * limMs(v) * 0.001, 0.001, 2.0); }},
      {6, 1, 8, pp::LimRelCurve, [](double v) { return std::clamp(v - 1, 0.0, 7.0); }},
      {7, 0, 10000, pp::LimHold, [](double v) { return std::clamp(limMs(v) * 0.001, 0.0001, 1.0); }},
      {8, 0, 1000, pp::LimCompThresh, [](double v) { return v >= 1000 ? 0.0 : std::clamp(limDb(limAmp7(v), -60), -60.0, -0.02); }},
      {9, -1000, 1000, pp::LimCompRatio, [](double v) { return std::clamp(limRatio(v), 0.25, 20.0); }},
      {10, -1000, 1000, pp::LimCompShape, [](double v) { return std::clamp(v / 1000.0, -1.0, 1.0); }},
      {11, 0, 10000, pp::LimCompAttack, [](double v) { return std::clamp(limMs(v) * 0.001, 0.0001, 1.0); }},
      {12, 0, 10000, pp::LimCompRelease, [](double v) { return std::clamp(2.0 * limMs(v) * 0.001, 0.001, 2.0); }},
      {13, 1, 8, pp::LimCompCurve, [](double v) { return std::clamp(v - 1, 0.0, 7.0); }},
      {14, 0, 10000, pp::LimCompRms, [](double v) { return std::clamp(limMs(v) * 0.001, 0.0001, 1.0); }},
      {15, 0, 1000, pp::LimGateFloor, [](double v) { return std::clamp(limDb(limAmp7(v), -80), -80.0, 0.0); }},
      {16, 0, 1000, pp::LimGateThresh, [](double v) { return std::clamp(limDb(limAmp7(v), -80), -80.0, 0.0); }},
      {17, 0, 10000, pp::LimGateRelease, [](double v) { return std::clamp(2.0 * limMs(v) * 0.001, 0.001, 2.0); }},
  });
  return t;
}

bool decodeLimiter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 76) return false;
  const int version = r.i32();
  std::vector<int32_t> v = ints(r, 18);
  begin(m, "gl.limiter");
  auto t = limiterTable();
  applyTable(m, *t, asDouble(v));
  bindTable(m, t);
  set(m, pp::LimCompKnee, 0.0);  // the reference has no dB soft knee
  // Tail: spare state (18), i32 ?, i32 view, 4 x u8 display toggles, i32 ?,
  // i32 sidechain input (0 = own signal); version >= 7: u8 flags (bit 0 = 0.2 dB safety).
  if (r.has(18 * 4 + 8)) {
    r.skip(18 * 4 + 4);
    set(m, pp::LimView, r.i32() == 1 ? 1 : 0);
    if (r.has(12)) {
      r.skip(4 + 4);
      set(m, pp::LimSidechain, r.i32() > 0 ? 1 : 0);
      if (version >= 7 && r.has(1)) set(m, pp::LimSafety, (r.u8() & 1) ? 1 : 0);
    }
  }
  m.exact = true;
  return true;
}

// ---------------------------------------------------------------- Fruity Compressor
// Exact (κ2): threshold v/10 dB, ratio v/10 : 1, gain v/10 dB, attack v/10 ms,
// release v ms; type 0-7 = knee {0, 6, 7, 15} dB by type % 4, "vintage" curve
// for type % 4 == 2, fast program-dependent release for types 4-7.
std::shared_ptr<const KnobTable> compTable() {
  static auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, -600, 0, pp::CmpThreshold, [](double v) { return std::clamp(v / 10.0, -60.0, 0.0); }},
      {1, 4, 300, pp::CmpRatio, [](double v) { return std::clamp(v / 10.0, 0.4, 30.0); }},
      {2, -300, 300, pp::CmpOutput, [](double v) { return std::clamp(v / 10.0, -30.0, 30.0); }},
      {3, 0, 4000, pp::CmpAttack, [](double v) { return std::clamp(v / 10.0 * 0.001, 0.0001, 0.4); }},
      {4, 1, 4000, pp::CmpRelease, [](double v) { return std::clamp(v * 0.001, 0.001, 4.0); }},
      {5, 0, 7, pp::CmpKnee, [](double v) {
         static const double knee[4] = {0, 6, 7, 15};
         return knee[std::clamp(static_cast<int>(v), 0, 7) % 4];
       }},
  });
  return t;
}

bool decodeCompressor(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 28) return false;
  const int version = r.i32();
  std::vector<int32_t> v = ints(r, 6);
  begin(m, "gl.comp");
  auto t = compTable();
  applyTable(m, *t, asDouble(v));
  bindTable(m, t);
  const int type = v.size() > 5 ? std::clamp(static_cast<int>(v[5]), 0, 7) : 0;
  set(m, pp::CmpMakeup, 0);
  set(m, pp::CmpCharacter, type % 4 == 2 ? 1 : 0);
  set(m, pp::CmpFastRelease, type >= 4 ? 1 : 0);
  set(m, pp::CmpDetector, 0);
  set(m, pp::CmpLookahead, 0);
  set(m, pp::CmpMix, 1);
  set(m, pp::CmpKeyHpf, 20);
  set(m, pp::CmpLimiter, 1);
  if (r.has(4)) r.u32();  // option flags
  if (version > 2 && r.has(4)) set(m, pp::CmpSidechain, r.i32() > 0 ? 1 : 0);
  m.exact = true;
  return true;
}

// ---------------------------------------------------------------- Fruity Multiband Compressor
// Exact (κ2): the plugin normalizes each knob to n = (v - min) / (max - min) for
// its engine: band edges 19981^sqrt(n) + 19 Hz, threshold 60·n - 60 dB, ratio
// 1 / (1 - n), band gain 2^(3n) - 1, master 4^n - 1, attack / release v / 10 ms,
// knee hardness n; band state 0 compress, 1 muted, 2 bypass; filter 0 IIR, 1 FIR.
double mbFreq(double v) { return std::pow(19981.0, std::sqrt(std::clamp((v - 20.0) / 19980.0, 0.0, 1.0))) + 19.0; }
double mbThresh(double v) { return std::clamp(60.0 * v / 65536.0 - 60.0, -60.0, 0.0); }
double mbRatio(double v) { return v >= 65536 ? 1000.0 : std::clamp(1.0 / (1.0 - v / 65536.0), 1.0, 1000.0); }
double mbOut(double v) {
  double g = std::pow(2.0, 3.0 * v / 65536.0) - 1.0;
  return std::clamp(g <= 0 ? -24.0 : 20.0 * std::log10(g), -24.0, 16.9);
}
double mbState(double v) { return v == 1 ? 2.0 : v == 2 ? 1.0 : 0.0; }

std::shared_ptr<const KnobTable> multibandTable() {
  static auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 65536, pp::MbOutput, [](double v) {
         double g = std::pow(4.0, v / 65536.0) - 1.0;
         return std::clamp(g <= 0 ? -24.0 : 20.0 * std::log10(g), -24.0, 9.54);
       }},
      {1, 0, 1, pp::MbLimiter, nullptr},
      {2, 0, 1, pp::MbFilterType, nullptr},
      {3, 0, 2, pp::MbHighState, mbState},
      {4, 20, 20000, pp::MbHighXover, mbFreq},
      {5, 0, 65536, pp::MbHighGain, mbOut},
      {6, 0, 65536, pp::MbHighThresh, mbThresh},
      {7, 0, 65536, pp::MbHighRatio, mbRatio},
      {8, 1, 1000, pp::MbHighAtk, [](double v) { return std::clamp(v / 10000.0, 0.0001, 0.1); }},
      {9, 100, 10000, pp::MbHighRel, [](double v) { return std::clamp(v / 10000.0, 0.01, 1.0); }},
      {10, 0, 65536, pp::MbHighHard, [](double v) { return std::clamp(v / 65536.0, 0.0, 1.0); }},
      {11, 0, 2, pp::MbMidState, mbState},
      {12, 20, 20000, pp::MbMidHighEdge, mbFreq},
      {13, 20, 20000, pp::MbMidLowEdge, mbFreq},
      {14, 0, 65536, pp::MbMidGain, mbOut},
      {15, 0, 65536, pp::MbMidThresh, mbThresh},
      {16, 0, 65536, pp::MbMidRatio, mbRatio},
      {17, 1, 1000, pp::MbMidAtk, [](double v) { return std::clamp(v / 10000.0, 0.0001, 0.1); }},
      {18, 100, 10000, pp::MbMidRel, [](double v) { return std::clamp(v / 10000.0, 0.01, 1.0); }},
      {19, 0, 65536, pp::MbMidHard, [](double v) { return std::clamp(v / 65536.0, 0.0, 1.0); }},
      {20, 0, 2, pp::MbLowState, mbState},
      {21, 20, 20000, pp::MbLowXover, mbFreq},
      {22, 0, 65536, pp::MbLowGain, mbOut},
      {23, 0, 65536, pp::MbLowThresh, mbThresh},
      {24, 0, 65536, pp::MbLowRatio, mbRatio},
      {25, 1, 1000, pp::MbLowAtk, [](double v) { return std::clamp(v / 10000.0, 0.0001, 0.1); }},
      {26, 100, 10000, pp::MbLowRel, [](double v) { return std::clamp(v / 10000.0, 0.01, 1.0); }},
      {27, 0, 65536, pp::MbLowHard, [](double v) { return std::clamp(v / 65536.0, 0.0, 1.0); }},
  });
  return t;
}

bool decodeMultiband(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 28 * 4) return false;
  r.i32();  // version
  std::vector<int32_t> v = ints(r, 28);
  begin(m, "gl.multiband");
  auto t = multibandTable();
  applyTable(m, *t, asDouble(v));
  bindTable(m, t);
  set(m, pp::MbEdgeMode, 1);  // the reference's mid band has its own edges
  m.exact = true;
  return true;
}

// ---------------------------------------------------------------- Maximus
// κ2: Maximus maps to Mastering Dynamics (gl.master) with its band blocks and
// drawn curves (engine "Drawn curve"). Knob laws (exact): band edges
// 30·2^(v·0.000138227748) Hz, low cut 5·2^(v/16384) Hz (−1 = off), pre / post /
// saturation ceiling (7^x − 1)/6 with x = v/1000, attack / sustain window
// T(v) = 2·(501^(v/10000) − 1) ms, release / release 2 shown as 2·T(v), LMH
// look-ahead T(v), LMH mix v/1000, stereo separation v/1000, saturation
// −1000..1000 (0 off, < 0 mode A, > 0 mode B, |v|/1000 amount). Curves: x =
// log9(2·in + 1), out = (9^y − 1)/2 (shared with the runtime).
// State: u16 version, u16 ?, 7 globals (tags 0-6), 4 × 13 band ints (tags 7-19,
// bands low, mid, high, master), spare state, options, then two envelope
// records per band (u32 kind, u32 format, u32 n, n × {f64 dx, f64 y, f32 tension,
// u32}, u32, 4 × i32); the first of each pair is the band's compression curve.
enum { BMode, BPre, BPost, BSep, BSatThres, BSatCeil, BAtt, BRel2, BAttCurve, BRel, BRelCurve, BWindow, BEnvMode, BCount };
namespace dy = gl::packs::dynamics;

double maxF7(double v) { return v == 1000 ? 1.0 : (std::pow(7.0, v / 1000.0) - 1.0) / 6.0; }
double maxDb(double v, double lo, double hi) {
  double g = maxF7(v);
  return std::clamp(g <= 0 ? lo : 20.0 * std::log10(g), lo, hi);
}
double maxT(double v) { return 2.0 * (std::pow(501.0, v / 10000.0) - 1.0); }

struct MaxCurve {
  int n = 0;
  double x[64], y[64], t[64];
};

// Reads the envelope records after the band blocks. They are byte-packed after
// the spare state and options, so the start is found as the offset from which
// eight consecutive valid records end exactly at the end of the state (every
// shipped preset and demo state, versions 2-10); otherwise a forward scan.
std::vector<MaxCurve> maximusCurves(const Reader& r, size_t from) {
  std::vector<MaxCurve> out;
  auto rd32 = [&](size_t o) {
    return static_cast<uint32_t>(r.p[o]) | (static_cast<uint32_t>(r.p[o + 1]) << 8) | (static_cast<uint32_t>(r.p[o + 2]) << 16) |
           (static_cast<uint32_t>(r.p[o + 3]) << 24);
  };
  auto rdf64 = [&](size_t o) {
    uint64_t u = static_cast<uint64_t>(rd32(o)) | (static_cast<uint64_t>(rd32(o + 4)) << 32);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
  };
  auto rdf32 = [&](size_t o) {
    uint32_t u = rd32(o);
    float f;
    std::memcpy(&f, &u, 4);
    return static_cast<double>(f);
  };
  // End of a valid record at `o` (0 = none).
  auto recEnd = [&](size_t o) -> size_t {
    if (o + 12 > r.n) return 0;
    const uint32_t kind = rd32(o), fmt = rd32(o + 4), n = rd32(o + 8);
    if (!((fmt == 2 || fmt == 3) && kind <= 16 && n >= 1 && n <= 64 && o + 12 + 24ull * n + 20 <= r.n)) return 0;
    for (uint32_t i = 0; i < n; ++i) {
      const double dx = rdf64(o + 12 + 24ull * i), y = rdf64(o + 20 + 24ull * i);
      if (!(dx >= 0 && dx <= 1.0001 && y >= -0.01 && y <= 1.01)) return 0;
    }
    return o + 12 + 24ull * n + 20;
  };
  size_t o = from;
  for (size_t s = from; s + 12 <= r.n; ++s) {
    size_t p = s;
    int k = 0;
    for (; k < 8; ++k) {
      const size_t q = recEnd(p);
      if (!q) break;
      p = q;
    }
    if (k == 8 && p == r.n) {
      o = s;
      break;
    }
  }
  while (o + 12 <= r.n && out.size() < 8) {
    uint32_t kind = rd32(o), fmt = rd32(o + 4), n = rd32(o + 8);
    if ((fmt == 2 || fmt == 3) && kind <= 16 && n >= 1 && n <= 64 && o + 12 + 24ull * n + 20 <= r.n) {
      MaxCurve c;
      double x = 0;
      bool ok = true;
      for (uint32_t i = 0; i < n; ++i) {
        size_t q = o + 12 + 24ull * i;
        double dx = rdf64(q), y = rdf64(q + 8), t = rdf32(q + 16);
        if (!(dx >= 0 && dx <= 1.0001 && y >= -0.01 && y <= 1.01 && std::isfinite(t))) {
          ok = false;
          break;
        }
        x += dx;
        c.x[c.n] = std::min(1.0, x), c.y[c.n] = std::clamp(y, 0.0, 1.0), c.t[c.n] = std::clamp(t, -1.0, 1.0);
        ++c.n;
      }
      if (ok) {
        out.push_back(c);
        o += 12 + 24ull * n + 20;
        continue;
      }
    }
    ++o;
  }
  return out;
}

// Decodes a Maximus state into gl.master parameters, written through `set`
// at `offset` (0 for gl.master, EnProg0 for gl.enhance's embedded program).
bool maximusToMaster(Reader& r, PluginMapping& m, int offset) {
  if (r.n < 4 + 7 * 4 + 4 * BCount * 4) return false;
  const int version = r.u16();
  r.u16();
  // Version 2 states have no low cut (six globals); from version 3 on, seven.
  std::vector<int32_t> g = ints(r, version >= 3 ? 7 : 6);
  if (g.size() < 7) g.resize(7, -1);
  std::vector<int32_t> b = ints(r, 4 * BCount);
  if (b.size() < 4 * BCount) return false;
  auto put = [&](int k, double v) { set(m, offset + k, v); };
  auto band = [&](int k, int f) { return static_cast<double>(b[static_cast<size_t>(k * BCount + f)]); };
  auto freq = [](double v) { return 30.0 * std::pow(2.0, v * 0.000138227748); };
  put(dy::MsLowFreq, std::clamp(freq(g[0]), 40.0, 1000.0));
  put(dy::MsHighFreq, std::clamp(freq(g[2]), 800.0, 16000.0));
  put(dy::MsLowSlope, g[1] > 0 ? 2 : 1);   // 12 / 24 dB per octave
  put(dy::MsHighSlope, g[3] > 0 ? 2 : 1);
  put(dy::MsLookahead, std::clamp(maxT(g[4]), 0.0, 10.0));
  put(dy::MsBandMix, std::clamp(g[5] / 1000.0, 0.0, 1.0));
  double lc = g[6] < 0 ? 10.0 : 5.0 * std::pow(2.0, g[6] / 16384.0);
  put(dy::MsLowCut, lc <= 10.5 ? 10.0 : std::clamp(lc, 10.0, 300.0));
  put(dy::MsSolo, 0);
  // The reference saturates at the base rate: no oversampling, so a stage's
  // output never exceeds its ceiling (no reconstruction overshoot).
  put(dy::MsOversample, 0);
  std::vector<MaxCurve> curves = maximusCurves(r, r.pos);
  for (int k = 0; k < 4; ++k) {
    auto P = [&](int f) { return offset + dy::msParam(k, f); };
    auto X = [&](int f) { return offset + dy::msExt(k, f); };
    // Reference band modes: 0 compress, 1 compression off (pre / post /
    // separation / saturation still apply), 2 muted, 3 band off: a low / high
    // band merges into the mid band (its split goes away), the master stage
    // passes through.
    const double md = band(k, BMode);
    set(m, P(dy::MbMode), md == 0 ? 1 : md == 1 ? 3 : md == 2 ? 2 : (md == 3 && (k == 0 || k == 2)) ? 4 : 0);
    set(m, P(dy::MbPre), maxDb(band(k, BPre), -24, 24));
    set(m, P(dy::MbPost), maxDb(band(k, BPost), -24, 24));
    set(m, P(dy::MbStereo), std::clamp(band(k, BSep) / 1000.0, -1.0, 1.0));
    const double sat = band(k, BSatThres);
    set(m, X(dy::MxSatMode), sat == 0 ? 0 : sat < 0 ? 1 : 2);
    // Saturation amount → threshold below the ceiling (approximation: the
    // reference's amount-to-threshold law is not decoded).
    double ceilDb = maxDb(band(k, BSatCeil), -24, 12);
    set(m, P(dy::MbSatCeil), ceilDb);
    set(m, P(dy::MbSatThresh), std::clamp(ceilDb + 20.0 * std::log10(std::max(0.05, 1.0 - std::fabs(sat) / 1000.0)), -24.0, 0.0));
    set(m, P(dy::MbAttack), std::clamp(maxT(band(k, BAtt)), 0.1, 300.0));
    set(m, X(dy::MxRelease2), std::clamp(2.0 * maxT(band(k, BRel2)), 0.0, 2000.0));
    set(m, X(dy::MxAtkCurve), std::clamp(band(k, BAttCurve) - 1, 0.0, 7.0));
    set(m, P(dy::MbRelease), std::clamp(2.0 * maxT(band(k, BRel)), 5.0, 3000.0));
    set(m, X(dy::MxRelCurve), std::clamp(band(k, BRelCurve) - 1, 0.0, 7.0));
    set(m, P(dy::MbWindow), std::clamp(maxT(band(k, BWindow)), 0.0, 1000.0));
    set(m, X(dy::MxEnvMode), band(k, BEnvMode) > 0 ? 1 : 0);
    set(m, X(dy::MxCurveMode), 1);
    // Curve: the first record of the band's pair (default: limiter at 0 dB).
    MaxCurve c;
    if (curves.size() >= static_cast<size_t>(2 * k + 1)) c = curves[static_cast<size_t>(2 * k)];
    if (c.n < 2) {
      c.n = 3;
      c.x[0] = 0, c.y[0] = 0, c.t[0] = 0, c.x[1] = 0.5, c.y[1] = 0.5, c.t[1] = 0, c.x[2] = 1, c.y[2] = 0.5, c.t[2] = 0;
    }
    int n = std::min(c.n, dy::kMsCurvePts);
    for (int i = 0; i < n; ++i) {  // more than 8 points: keep the first ones and the last
      int src = i == n - 1 ? c.n - 1 : i;
      set(m, X(dy::MxPt0 + i * 3), c.x[src]);
      set(m, X(dy::MxPt0 + i * 3 + 1), c.y[src]);
      set(m, X(dy::MxPt0 + i * 3 + 2), c.t[src]);
    }
    set(m, X(dy::MxPoints), n);
  }
  return true;
}

bool decodeMaximus(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 7 * 4 + 4 * BCount * 4) return false;
  begin(m, "gl.master");
  if (!maximusToMaster(r, m, 0)) return false;
  // Automation: the global controls (the band page controls follow the
  // selected band and are not mapped).
  static auto table = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 65536, dy::MsLowFreq, [](double v) { return std::clamp(30.0 * std::pow(2.0, v * 0.000138227748), 40.0, 1000.0); }},
      {2, 0, 65536, dy::MsHighFreq, [](double v) { return std::clamp(30.0 * std::pow(2.0, v * 0.000138227748), 800.0, 16000.0); }},
      {5, 0, 1000, dy::MsBandMix, [](double v) { return std::clamp(v / 1000.0, 0.0, 1.0); }},
  });
  bindTable(m, table);
  m.exact = false;  // knob laws and curves exact; band envelope / saturation details approximated
  return true;
}

// ---------------------------------------------------------------- Soundgoodizer
// A Maximus engine running one of four programs; the amount knob sets the
// program's LMH (band) mix. κ2: imports as Enhance (gl.enhance) with the
// program embedded when the FL Studio data folder provides it.
bool decodeSoundgoodizer(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 12) return false;
  r.i32();  // version
  int mode = std::clamp(r.i32(), 0, 3);
  double amount = std::clamp(r.i32() / 1000.0, 0.0, 1.0);
  begin(m, "gl.enhance");
  set(m, dy::EnMode, mode);
  set(m, dy::EnAmount, amount);
  bool fromProgram = false;
  if (!c.factoryData.empty()) {
    static const char* names[4] = {"a", "b", "c", "d"};
    // Raw program data shipped with the plugin, else the preset copies.
    std::string dat = c.factoryData + "/Plugins/Fruity/Effects/Soundgoodizer/Data/Presets/" + names[mode] + ".dat";
    if (FILE* fp = std::fopen(dat.c_str(), "rb")) {
      std::vector<uint8_t> bytes;
      uint8_t buf[4096];
      size_t got;
      while ((got = std::fread(buf, 1, sizeof buf, fp)) > 0) bytes.insert(bytes.end(), buf, buf + got);
      std::fclose(fp);
      Reader pr(bytes.data(), bytes.size());
      fromProgram = maximusToMaster(pr, m, dy::EnProg0);
    }
    if (!fromProgram) {
      std::string path = c.factoryData + "/Data/Patches/Plugin presets/Effects/Maximus/Soundgoodizer presets/" + names[mode] + ".fst";
      File f;
      if (readFile(path, f, nullptr)) {
        Reader pr(pluginState(f.events));
        fromProgram = maximusToMaster(pr, m, dy::EnProg0);
      }
    }
  }
  set(m, dy::EnCustom, fromProgram ? 1 : 0);
  // Automation: the amount knob (stored 0..1000).
  m.param = [](int fl) { return fl == 0 ? static_cast<int>(dy::EnAmount) : -1; };
  m.value = [](int, double n) { return std::clamp(n, 0.0, 1.0); };
  setRawRanges(m, {{0, 0, 1000}});
  m.exact = false;
  return true;
}

// ---------------------------------------------------------------- Transient Processor
// Exact (κ2): knobs 0..160 → x = v / 160 for the engine: attack / release
// amount 2x - 1, drive x, gain 18x - 9 dB, shapes 0-2, split 995x² + 5 Hz,
// balance 2x - 1; tag 8 switches the effect.
std::shared_ptr<const KnobTable> transientTable() {
  static auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 0, 160, pp::TrAttack, [](double v) { return std::clamp(v / 80.0 - 1.0, -1.0, 1.0); }},
      {1, 0, 160, pp::TrDrive, [](double v) { return std::clamp(v / 160.0, 0.0, 1.0); }},
      {2, 0, 160, pp::TrSustain, [](double v) { return std::clamp(v / 80.0 - 1.0, -1.0, 1.0); }},
      {3, 0, 160, pp::TrOutput, [](double v) { return std::clamp(18.0 * v / 160.0 - 9.0, -9.0, 9.0); }},
      {4, 0, 2, pp::TrAttackSpeed, nullptr},
      {5, 0, 2, pp::TrReleaseSpeed, nullptr},
      {6, 0, 160, pp::TrSplit, [](double v) {
         double x = v / 160.0;
         return std::clamp(995.0 * x * x + 5.0, 5.0, 1000.0);
       }},
      {7, 0, 160, pp::TrBalance, [](double v) { return std::clamp(v / 80.0 - 1.0, -1.0, 1.0); }},
      {8, 0, 1, pp::TrEnabled, nullptr},
  });
  return t;
}

bool decodeTransient(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 9 * 4) return false;
  r.i32();  // version
  std::vector<int32_t> v = ints(r, 9);
  begin(m, "gl.transient");
  auto t = transientTable();
  applyTable(m, *t, asDouble(v));
  bindTable(m, t);
  set(m, pp::TrMix, 1.0);
  m.exact = true;
  return true;
}

}  // namespace

void registerFxDyn(std::map<std::string, PluginDecoder>& d) {
  d["Fruity Limiter"] = decodeLimiter;
  d["Fruity Compressor"] = decodeCompressor;
  d["Fruity Multiband Compressor"] = decodeMultiband;
  d["Maximus"] = decodeMaximus;
  d["Soundgoodizer"] = decodeSoundgoodizer;
  d["Transient Processor"] = decodeTransient;
}

}  // namespace gl::flp::fx

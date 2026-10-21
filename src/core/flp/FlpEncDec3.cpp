// Gridline — FLP export: plugin state encoders for FlpDec3Fx/Inst/Kepler/Vfx.cpp, FlpDec4.cpp, FlpDecoders3.cpp.
// The reverse of the decoders in those files; see FlpEncode.h for the model.
//
// Default states are built field by field from the plugins' published
// controls and the defaults FL's plugin database records (no FL bytes are
// copied). Plugins whose decoder reads nothing (Level Scaler, Sequencer,
// Mute 2, Wasp, FL Slayer, LSD, Slicer 2) or whose layout is too large to
// build (VFX Envelope, Soundfont Player, Mobile Rack FX) have no
// default state: FL loads them with their own defaults, which is what the
// decoder produces for an empty state.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/spacefx/sfx_mbd.h"
#include "../../packs/spacefx/spacefx.h"
#include "../../packs/utility/utility.h"
#include "../PluginParams.h"
#include "FlpEncode.h"

namespace gl::flp::enc {

namespace {

namespace mod = gl::packs::modulation;
namespace dyn = gl::packs::dynamics;
namespace sfx = gl::packs::spacefx;
namespace ut = gl::packs::utility;

int32_t rdI32(const Bytes& b, size_t at) { return at + 4 <= b.size() ? static_cast<int32_t>(getU32(b, at)) : 0; }
uint32_t rdU32(const Bytes& b, size_t at) { return at + 4 <= b.size() ? getU32(b, at) : 0; }
double rdF64(const Bytes& b, size_t at) {
  double d = 0;
  if (at + 8 <= b.size()) std::memcpy(&d, b.data() + at, 8);
  return d;
}
float prm(const PluginState& g, int k) { return k >= 0 && static_cast<size_t>(k) < g.params.size() ? g.params[static_cast<size_t>(k)] : 0.0f; }

// The state decoded with the plugin's own decoder (false when rejected).
bool decoded(const std::string& flName, const Bytes& b, const EncodeContext& c, PluginState& out) {
  PluginMapping m;
  if (!decodeState(flName, b, m, c)) return false;
  out = m.state;
  return true;
}
// Parameters `pick` selects are equal in both states.
template <class Pick>
bool sameParams(const PluginState& a, const PluginState& b, Pick pick) {
  const size_t n = std::min(a.params.size(), b.params.size());
  for (size_t k = 0; k < n; ++k)
    if (pick(static_cast<int>(k)) && a.params[k] != b.params[k]) return false;
  return a.params.size() == b.params.size();
}

Encoder make(const char* flName, const char* glType, int rank, bool generator = false) {
  Encoder e;
  e.flName = flName;
  e.glType = glType;
  e.nativeRank = rank;
  e.generator = generator;
  return e;
}

// ======================================================= FlpDec3Fx.cpp

// Fruity Peak Controller: 10 × i32 knobs (tags 0..9), u8 mute, u8 ramp.
Encoder peakController() {
  Encoder e = make("Fruity Peak Controller", "gl.peakctl", 0);
  e.defaults = [] {
    ByteWriter w;
    w.i32(0);      // 0 peak base
    w.i32(179);    // 1 peak amount (FL's saved default)
    w.i32(0);      // 2 peak tension
    w.i32(66);     // 3 peak decay
    w.i32(0);      // 4 LFO base
    w.i32(0);      // 5 LFO amount
    w.i32(0);      // 6 LFO tension
    w.i32(32950);  // 7 LFO speed
    w.i32(0);      // 8 LFO shape: sine
    w.i32(0);      // 9 LFO phase
    w.u8(0);       // mute output
    w.u8(0);       // ramp controllers
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{
        // J-42: the reference model keeps the stored integers.
        i32(mod::PkRBase, 0, 0, 65536),     i32(mod::PkRAmount, 4, -256, 256), i32(mod::PkRTension, 8, -128, 128),
        i32(mod::PkRDecay, 12, 5, 128),     i32(mod::LfRBase, 16, 0, 65536),   i32(mod::LfRAmount, 20, -256, 256),
        i32(mod::LfRTension, 24, -128, 128), i32(mod::LfRSpeed, 28, 200, 65536), i32(mod::LfShape, 32, 0, 4),
        i32(mod::LfRPhase, 36, 0, 65535),   field(mod::PkMute, 40, Type::U8, 0, 1), field(mod::PkRamp, 41, Type::U8, 0, 1)};
  };
  return e;
}

// Fruity Center: u32 version 1, i32 status (1 = removing DC offset).
// Native home for a utility that only removes DC (Balance cannot).
Encoder center() {
  Encoder e = make("Fruity Center", "gl.utility", 20);
  e.accepts = [](const PluginState& s) {
    const PluginState d = defaultPluginState("gl.utility");
    return prm(s, pp::UtlDcRemove) >= 0.5f && sameParams(s, d, [](int k) { return k != pp::UtlDcRemove; });
  };
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);  // version
    w.i32(1);  // status: on
    return w.b;
  };
  e.fields = [](const Bytes&) { return std::vector<Field>{i32(pp::UtlDcRemove, 4, 0, 1)}; };
  return e;
}

// Fruity Spectroman: i32 100, i32 style, i32 amplification 0..128, i32
// frequency scale 0..128, u8 peaks, u8 stereo, u8 spare.
Encoder spectroman() {
  Encoder e = make("Fruity Spectroman", "gl.spectro", 0);
  e.defaults = [] {
    ByteWriter w;
    w.i32(100);  // format word
    w.i32(1);    // display style
    w.i32(64);   // amplification: unity
    w.i32(128);  // frequency scale
    w.u8(1);     // show peaks
    w.u8(1);     // stereo
    w.u8(0);     // spare
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(ut::SgAmp, 8, 0, 128), i32(ut::SgScale, 12, 0, 128), field(ut::SgPeaks, 16, Type::U8, 0, 1),
                              field(ut::SgStereo, 17, Type::U8, 0, 1)};
  };
  return e;
}

// Tuner: u32 version 1, i32 reactivity 0..6000, i32 averaging, i32 bass
// mode, i32 display (1 horizontal, 2 circular).
Encoder tuner() {
  Encoder e = make("Tuner", "gl.tuner", 0);
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);     // version
    w.i32(3000);  // reactivity
    w.i32(1);     // averaging
    w.i32(0);     // bass mode
    w.i32(2);     // display: circular
    return w.b;
  };
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32(pp::TnReactivity, 4, 0, 6000), i32(pp::TnAverage, 8, 0, 1), i32(pp::TnBassMode, 12, 0, 1), i32(pp::TnDisplay, 16, 1, 2)};
  };
  return e;
}

// Fruity 7 Band EQ: u32 version 1, 7 × i32 band levels −1800..1800.
Encoder sevenBand() {
  Encoder e = make("Fruity 7 Band EQ", "gl.geq", 0);
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);  // version
    for (int k = 0; k < pp::kGeqBands; ++k) w.i32(0);  // flat
    return w.b;
  };
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int k = 0; k < pp::kGeqBands; ++k) f.push_back(i32(k, 4 + 4 * static_cast<size_t>(k), -1800, 1800));
    return f;
  };
  return e;
}

// Transmitter: u32 version 3, i32 per tag 0..41, then 20 bytes of options.
constexpr size_t tmTag(int tag) { return 4 + 4 * static_cast<size_t>(tag); }

Encoder transmitter() {
  Encoder e = make("Transmitter", "gl.transplit", 0);
  e.defaults = [] {
    ByteWriter w;
    w.u32(3);  // version
    const int32_t tags[42] = {
        7475, 1968, 1968,         // sensitivity, hold, release
        8743, 8743,               // transient / sustain level (neutral)
        5000, 5000,               // transient / sustain tilt (flat)
        10000, 9112, 0,           // transient diffuser: mix, depth, delay
        5000, 5051, 5000,         // sustain diffuser
        10000, 3333, 0,           // transient saturation: mix, drive, warmth
        10000, 3342, 0,           // sustain saturation
        10000, 5320, 5275,        // transient bounce: mix, amp curve, time curve
        10000, 4443, 4642,        // sustain bounce
        10000, 3531, 144, 0,      // transient delay: mix, time, synced time, feedback
        10000, 3156, 240, 0,      // sustain delay
        1, 1, 1,                  // lookahead, stereo link, detection mode
        1, 1,                     // transient / sustain enabled
        0, 2,                     // transient effect saturation, sustain effect diffusion
        0, 0};                    // delay tempo sync
    for (int32_t v : tags) w.i32(v);
    // Options after the parameters (not decoded): three zero words, 90, and
    // two set flag bytes, two spare.
    w.i32(0);
    w.i32(0);
    w.i32(0);
    w.i32(90);
    w.u8(1);
    w.u8(1);
    w.u8(0);
    w.u8(0);
    return w.b;
  };
  // J-45: the reference engine keeps every stored control (tsRaw), then the
  // two send routes and the detection source route.
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int k = 0; k < 42; ++k) f.push_back(i32(dyn::tsRaw(k), tmTag(k), dyn::kTsRawMin[k], dyn::kTsRawMax[k]));
    f.push_back(i32(dyn::TsSendT, tmTag(42), 0, 500));
    f.push_back(i32(dyn::TsSendS, tmTag(43), 0, 500));
    f.push_back(i32(dyn::TsScIn, tmTag(44), 0, 500));
    return f;
  };
  return e;
}

// Multiband Delay: u32 version 4, u32 n (393), n × i32 (8 banks × {delay,
// volume, pan} × 16 bands as value · 2^30, then the global knobs), then
// options. Gridline's two curve groups are the morph position's bank pair;
// its 32 curve points are linear interpolations of the 16 bands, so write()
// fits the bands to the points (the decoder's interpolation, checked through
// the printed curve text) and the morph position picks the bank pair.
constexpr int kMbdN = 393;
constexpr double kMbdOne = 1073741824.0;  // 2^30 = stored 1.0
constexpr size_t mbdAt(int i) { return 8 + 4 * static_cast<size_t>(i); }
constexpr int kMbdTargets[3] = {sfx::BtDelay, sfx::BtLevel, sfx::BtPan};  // FL target order

std::string curveText(float v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%.4g", static_cast<double>(v));
  return buf;
}
// Bands from the printed points by interval propagation: each point's text
// stands for an interval of values, each point is (1 − f)·band k + f·band
// k + 1, so the band intervals are narrowed along the chain until stable,
// then bands are picked left to right inside what the previous pick allows.
// `band` is left as it is when the intervals do not meet.
void mbdIntervals(const std::string* target, double* band) {
  struct Pt {
    int k;
    double f, lo, hi;
  };
  std::vector<Pt> pts;
  for (int i = 0; i < sfx::kBdPoints; ++i) {
    const double pos = 15.0 * i / (sfx::kBdPoints - 1);
    const int k = std::clamp(static_cast<int>(pos), 0, 14);
    const double d = std::strtod(target[i].c_str(), nullptr);
    double lo, hi;
    if (d <= 0) {
      lo = hi = 0;  // "0": the value clamps to 0
    } else {
      const double e = std::floor(std::log10(d));
      const double u = std::pow(10.0, e - 3);
      const bool decade = std::fabs(d - std::pow(10.0, e)) < u * 1e-6;
      lo = d - (decade ? 0.05 : 0.5) * u * 0.98;
      hi = d + 0.5 * u * 0.98;
      if (d >= 1) hi = 2;  // clamps to 1
    }
    pts.push_back({k, std::clamp(pos - k, 0.0, 1.0), lo, hi});
  }
  double L[16], U[16];
  for (int k = 0; k < 16; ++k) L[k] = 0, U[k] = 1;
  auto narrow = [&](const Pt& p) {
    const int a = p.k, b = p.k + 1;
    if (p.f < 1) L[a] = std::max(L[a], (p.lo - p.f * U[b]) / (1 - p.f)), U[a] = std::min(U[a], (p.hi - p.f * L[b]) / (1 - p.f));
    if (p.f > 0) L[b] = std::max(L[b], (p.lo - (1 - p.f) * U[a]) / p.f), U[b] = std::min(U[b], (p.hi - (1 - p.f) * L[a]) / p.f);
  };
  auto settle = [&] {
    for (int it = 0; it < 100; ++it)
      for (const Pt& p : pts) narrow(p);
    for (int k = 0; k < 16; ++k)
      if (L[k] > U[k] + 1e-12) return false;
    return true;
  };
  if (!settle()) return;
  double v[16];
  for (int k = 0; k < 16; ++k) {
    // Fix band k inside its interval (middle first) so that the rest still meets.
    double keepL[16], keepU[16];
    std::copy(L, L + 16, keepL);
    std::copy(U, U + 16, keepU);
    bool ok = false;
    for (int j = 0; j <= 40 && !ok; ++j) {
      const double t = 0.5 + (j % 2 ? 1 : -1) * ((j + 1) / 2) / 41.0;
      std::copy(keepL, keepL + 16, L);
      std::copy(keepU, keepU + 16, U);
      v[k] = keepL[k] + (keepU[k] - keepL[k]) * t;
      L[k] = U[k] = v[k];
      ok = settle();
    }
    if (!ok) return;
  }
  for (int k = 0; k < 16; ++k) band[k] = v[k];
}

// J-51: the floats that print as one curve text. "%.4g" rounds the value,
// which is monotonic, so they form one interval in value order: found by
// bisection over the float order, the per-candidate check is then two
// comparisons instead of a formatted print (the fit tries ~10^5 candidates).
struct PrintRange {
  float lo = 1, hi = 0;  // empty: compare the text
  int zero = 0;          // 1: the text of +0 ("0"), −1: of −0 (only that float prints so)
  std::string text;
  bool has(float v) const {
    if (zero) return v == 0 && std::signbit(v) == (zero < 0);
    return lo <= hi ? (v >= lo && v <= hi) : curveText(v) == text;
  }
};
int64_t floatKey(float f) {  // order-preserving integer key (−0 and +0 share 0)
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return (u & 0x80000000u) ? -static_cast<int64_t>(u & 0x7FFFFFFFu) : static_cast<int64_t>(u);
}
float keyFloat(int64_t k) {
  const uint32_t u = k < 0 ? (static_cast<uint32_t>(-k) | 0x80000000u) : static_cast<uint32_t>(k);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
PrintRange printRange(float want) {
  PrintRange r;
  r.text = curveText(want);
  // Only finite non-zero values (−0 prints differently from +0, which shares
  // its key): zero and the rest fall back to comparing the text.
  if (want == 0) {
    r.zero = std::signbit(want) ? -1 : 1;
    return r;
  }
  if (!std::isfinite(want)) return r;
  const int64_t w = floatKey(want), kMax = floatKey(std::numeric_limits<float>::max());
  auto ok = [&](int64_t k) { return k != 0 && curveText(keyFloat(k)) == r.text; };
  int64_t a = -kMax, b = w;  // lowest match in [a, b]: ok(b), first true
  if (ok(a)) b = a;
  while (b - a > 1) {
    const int64_t m = a + (b - a) / 2;
    (ok(m) ? b : a) = m;
  }
  const int64_t lo = b;
  a = w, b = kMax;  // highest match: ok(a), last true
  if (ok(b)) a = b;
  while (b - a > 1) {
    const int64_t m = a + (b - a) / 2;
    (ok(m) ? a : b) = m;
  }
  // The interval must not reach zero (whose two signs print differently).
  if ((lo <= 0 && a >= 0)) return r;
  r.lo = keyFloat(lo), r.hi = keyFloat(a);
  return r;
}

// 16 stored bands whose interpolated points print as `want`.
void mbdFit(const float* want, int32_t* s) {
  std::string target[sfx::kBdPoints];
  PrintRange prints[sfx::kBdPoints];
  for (int i = 0; i < sfx::kBdPoints; ++i) prints[i] = printRange(want[i]), target[i] = prints[i].text;
  // Start: band 0 = first point; each next band through the segment's last point.
  double band[16];
  band[0] = want[0];
  for (int k = 0; k < 15; ++k) {
    int best = -1;
    double bf = 0;
    for (int i = 0; i < sfx::kBdPoints; ++i) {
      const double pos = 15.0 * i / (sfx::kBdPoints - 1);
      const int kk = std::clamp(static_cast<int>(pos), 0, 14);
      const double f = pos - kk;
      if (kk == k && f > bf) bf = f, best = i;
    }
    band[k + 1] = best < 0 ? band[k] : (want[best] - (1 - bf) * band[k]) / bf;
  }
  mbdIntervals(target, band);
  for (int k = 0; k < 16; ++k) s[k] = static_cast<int32_t>(std::lround(std::clamp(band[k], 0.0, 1.0) * kMbdOne));
  // Points that depend on band k (J-51: listed once, in point order).
  // Each point's segment and position in it (the decoder's interpolation).
  int dep[16][sfx::kBdPoints], deps[16] = {}, segOf[sfx::kBdPoints];
  double fracOf[sfx::kBdPoints];
  for (int i = 0; i < sfx::kBdPoints; ++i) {
    const double pos = 15.0 * i / (sfx::kBdPoints - 1);
    const int kk = std::clamp(static_cast<int>(pos), 0, 14);
    segOf[i] = kk, fracOf[i] = std::clamp(pos - kk, 0.0, 1.0);
    for (int k = 0; k < 16; ++k)
      if (kk == k || kk + 1 == k) dep[k][deps[k]++] = i;
  }
  auto cost = [&](int k) {
    int bad = 0;
    double err = 0;
    for (int j = 0; j < deps[k]; ++j) {
      const int i = dep[k][j];
      const double a = s[segOf[i]] / kMbdOne, c = s[segOf[i] + 1] / kMbdOne;
      const float v = static_cast<float>(std::clamp(a + (c - a) * fracOf[i], 0.0, 1.0));  // the decoder's point i
      if (!prints[i].has(v)) ++bad;
      err += std::fabs(static_cast<double>(v) - want[i]);
    }
    return bad * 1e3 + err;
  };
  // Refine each band: the values that make one of its points exact (with the
  // neighbour band as it is), the range ends, and a local search.
  for (int pass = 0; pass < 20; ++pass) {
    bool changed = false;
    for (int k = 0; k < 16; ++k) {
      const int32_t keep = s[k];
      int32_t best = keep;
      double bc = cost(k);
      auto tryValue = [&](double v) {
        if (!(v >= 0 && v <= 1)) return;
        const int32_t r = static_cast<int32_t>(std::lround(v * kMbdOne));
        for (int32_t d = -2; d <= 2; ++d) {
          s[k] = r + d;
          if (s[k] < 0 || s[k] > static_cast<int32_t>(kMbdOne)) continue;
          const double c = cost(k);
          if (c < bc) bc = c, best = s[k];
        }
      };
      tryValue(0);
      tryValue(1);
      for (int i = 0; i < sfx::kBdPoints; ++i) {
        const double pos = 15.0 * i / (sfx::kBdPoints - 1);
        const int kk = std::clamp(static_cast<int>(pos), 0, 14);
        const double f = std::clamp(pos - kk, 0.0, 1.0);
        if (kk == k && f < 1) tryValue((want[i] - f * (s[k + 1] / kMbdOne)) / (1 - f));
        if (kk + 1 == k && f > 0) tryValue((want[i] - (1 - f) * (s[kk] / kMbdOne)) / f);
      }
      for (double step : {1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8}) {
        const double centre = best / kMbdOne;
        for (int j = -12; j <= 12; ++j) tryValue(centre + j * step);
      }
      s[k] = best;
      changed = changed || best != keep;
    }
    if (!changed) break;
  }
  // Points still off: move neighbouring bands together (band k over its
  // candidates, band k + 1 re-solved for each).
  auto bad = [&](int k) { return cost(k) >= 1e3; };
  auto pointValues = [&](int k, std::vector<double>& out) {  // band-k values making one of its points exact
    out.clear();
    out.push_back(0), out.push_back(1);
    for (int j = 0; j < deps[k]; ++j) {  // (the points of segments k − 1 and k, in point order)
      const int i = dep[k][j], kk = segOf[i];
      const double f = fracOf[i];
      if (kk == k && f < 1) out.push_back((want[i] - f * (s[k + 1] / kMbdOne)) / (1 - f));
      if (kk + 1 == k && f > 0) out.push_back((want[i] - (1 - f) * (s[kk] / kMbdOne)) / f);
    }
  };
  std::vector<double> candA, candB;
  for (int pass = 0; pass < 4; ++pass) {
    bool any = false;
    for (int k = 0; k + 1 < 16; ++k) {
      if (!bad(k) && !bad(k + 1)) continue;
      any = true;
      const int32_t keepA = s[k], keepB = s[k + 1];
      int32_t bestA = keepA, bestB = keepB;
      double bc = cost(k) + cost(k + 1);
      pointValues(k, candA);
      for (double step : {1e-4, 1e-5, 1e-6, 1e-7})
        for (int j = -12; j <= 12; ++j) candA.push_back(keepA / kMbdOne + j * step);
      for (double va : candA) {
        if (!(va >= 0 && va <= 1)) continue;
        for (int32_t da = -1; da <= 1; ++da) {
          s[k] = std::clamp(static_cast<int32_t>(std::lround(va * kMbdOne)) + da, 0, static_cast<int32_t>(kMbdOne));
          pointValues(k + 1, candB);
          candB.push_back(keepB / kMbdOne);
          for (double vb : candB) {
            if (!(vb >= 0 && vb <= 1)) continue;
            for (int32_t db = -1; db <= 1; ++db) {
              s[k + 1] = std::clamp(static_cast<int32_t>(std::lround(vb * kMbdOne)) + db, 0, static_cast<int32_t>(kMbdOne));
              const double c = cost(k) + cost(k + 1);
              if (c < bc) bc = c, bestA = s[k], bestB = s[k + 1];
            }
          }
        }
      }
      s[k] = bestA, s[k + 1] = bestB;
    }
    if (!any) break;
  }
}

Encoder multibandDelay() {
  Encoder e = make("Multiband Delay", "gl.banddelay", 0);
  e.defaults = [] {
    ByteWriter w;
    w.u32(4);      // version
    w.u32(kMbdN);  // stored values
    for (int bank = 0; bank < 8; ++bank) {
      for (int k = 0; k < 16; ++k) w.i32(static_cast<int32_t>(static_cast<double>(static_cast<float>(k / 15.0)) * kMbdOne));  // delay ramp
      for (int k = 0; k < 16; ++k) w.i32(static_cast<int32_t>(kMbdOne));       // volume 1
      for (int k = 0; k < 16; ++k) w.i32(static_cast<int32_t>(kMbdOne / 2));   // pan centre
    }
    w.i32(1800);  // 384 delay scale
    w.i32(0);     // 385 ×0.1
    w.i32(0);     // 386 feedback
    w.i32(1500);  // 387 smoothing
    w.i32(0);     // 388 filter mode
    w.i32(0);     // 389 keep pitch
    w.i32(0);     // 390 bank morph
    w.i32(128);   // 391 wet / dry: wet
    w.i32(0);     // 392 harmonic scale
    // Extra options (not decoded): two zero words, a set word, reserved zeros.
    w.i32(0);
    w.i32(0);
    w.i32(1);
    w.zeros(189);
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    if (b.size() < 8 || rdU32(b, 4) < 392 || b.size() < mbdAt(static_cast<int>(rdU32(b, 4)))) return;
    PluginState cur;
    if (!decoded("Multiband Delay", b, c, cur)) return;
    if (prm(g, sfx::BdModel) >= 0.5f) {  // J-42 band-delay model: the 384 band values verbatim
      if (cur.data == g.data) return;
      const size_t at = g.data.find("mbd8");
      if (at == std::string::npos) return;
      const char* p = g.data.c_str() + at + 4;
      for (int i = 0; i < 384; ++i) {  // doubles: the stored integers survive the text round trip
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) break;
        p = end;
        putI32(b, mbdAt(i), static_cast<int32_t>(std::lround(std::clamp(v, 0.0, 1.0) * kMbdOne)));
      }
      return;
    }
    // Maximum time: the ×0.1 range and the scale together (the scale alone
    // is a field). Each range is tried with its closest scale steps.
    const float wantTime = prm(g, sfx::BdMaxTime);
    if (prm(cur, sfx::BdMaxTime) != wantTime) {
      int32_t bestTenth = rdI32(b, mbdAt(385)), bestScale = rdI32(b, mbdAt(384));
      double bestErr = HUGE_VAL;
      for (int tenth = 0; tenth < 2; ++tenth) {
        const double s0 = 2000.0 * std::log2(std::max(1e-6, static_cast<double>(wantTime)) / (tenth ? 0.05 : 0.5));
        for (int d = -2; d <= 2; ++d) {
          const int32_t sc = std::clamp(static_cast<int32_t>(std::lround(s0)) + d, -6000, 6000);
          putI32(b, mbdAt(385), tenth);
          putI32(b, mbdAt(384), sc);
          PluginState t;
          if (!decoded("Multiband Delay", b, c, t)) continue;
          const double err = std::fabs(static_cast<double>(prm(t, sfx::BdMaxTime)) - wantTime);
          if (err < bestErr) bestErr = err, bestTenth = tenth, bestScale = sc;
        }
      }
      putI32(b, mbdAt(385), bestTenth);
      putI32(b, mbdAt(384), bestScale);
    }
    const double wantMorph = prm(g, sfx::BdMorph);
    if (cur.data == g.data && prm(cur, sfx::BdMorph) == static_cast<float>(wantMorph)) return;
    // Bank pair: keep the stored one; a full morph (1) needs the last pair.
    int morph = std::clamp(rdI32(b, mbdAt(390)), 0, 7000);
    int bankA = std::min(6, morph / 1000);
    if (wantMorph >= 0.9995) {
      bankA = 6;
      morph = 7000;
    } else {
      morph = bankA * 1000 + std::clamp(static_cast<int>(std::lround(wantMorph * 1000)), 0, 999);
    }
    putI32(b, mbdAt(390), morph);
    const sfx::BandCurves cv = sfx::parseBandCurves(g.data);
    // J-51: the six curve fits are independent: on the solver's threads.
    int32_t fit[6][16];
    parallelFor(6, [&](size_t i) { mbdFit(cv.v[i / 3][kMbdTargets[i % 3]], fit[i]); });
    for (int grp = 0; grp < 2; ++grp)
      for (int t = 0; t < 3; ++t)
        for (int k = 0; k < 16; ++k) putI32(b, mbdAt((bankA + grp) * 48 + t * 16 + k), fit[grp * 3 + t][k]);
  };
  e.fields = [](const Bytes& b) {
    // J-42: the band-delay model's engine values (decoded states always carry it).
    const int n = static_cast<int>(rdU32(b, 4));
    std::vector<Field> f = {i32(sfx::BdRScale, mbdAt(384), -6000, 6000), i32(sfx::BdRTenth, mbdAt(385), 0, 1),
                            i32(sfx::BdRFeedback, mbdAt(386), 0, 6000),   i32(sfx::BdRSmooth, mbdAt(387), 0, 6000),
                            i32(sfx::BdRMode, mbdAt(388), 0, 2),          i32(sfx::BdRKeep, mbdAt(389), 0, 1),
                            i32(sfx::BdRMorph, mbdAt(390), 0, 7000),      i32(sfx::BdRMix, mbdAt(391), -128, 128),
                            i32(sfx::BdRHarmonic, mbdAt(392), 0, 1)};
    if (n > 393) f.push_back(i32(sfx::BdRWeight, mbdAt(393), 0, 1));
    if (n > 394) f.push_back(i32(sfx::BdRHp, mbdAt(394), 1000, 100000));
    if (n > 395) f.push_back(i32(sfx::BdRSat, mbdAt(395), 0, 2));
    return f;
  };
  return e;
}

// ======================================================= FlpDec3Vfx.cpp

// VFX Key Mapper: i32 version 2, enable, base key, offset, multi mode, pitch
// slide; then 12 records (key r of the octave from the base key): u32 count,
// count × i16 target. write() rewrites only the records whose mapping
// differs (chords and other FL-only detail of unchanged keys stay).
Encoder keyMapper() {
  Encoder e = make("VFX Key Mapper", "gl.keymap", 0);
  e.defaults = [] {
    ByteWriter w;
    w.i32(2);  // version
    w.i32(1);  // enabled
    w.i32(0);  // base key C
    w.i32(0);  // offset
    w.i32(0);  // multi mode
    w.i32(0);  // pitch slide
    for (int r = 0; r < 12; ++r) {
      w.u32(1);  // one target: the key itself
      w.u16(static_cast<uint32_t>(r));
    }
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (b.size() < 24 || !decoded("VFX Key Mapper", b, c, cur)) return;
    if (sameParams(cur, g, [](int k) { return k != mod::KmTranspose; })) return;
    const bool wasOn = rdI32(b, 4) != 0;
    const int base = ((rdI32(b, 8) % 12) + 12) % 12;
    std::vector<std::vector<int>> rec(12);
    size_t p = 24;
    for (int r = 0; r < 12; ++r) {
      const uint32_t n = rdU32(b, p);
      p += 4;
      if (n > 64 || p + 2 * n > b.size()) return;
      for (uint32_t k = 0; k < n; ++k) rec[static_cast<size_t>(r)].push_back(static_cast<int16_t>(getU16(b, p + 2 * k)));
      p += 2 * n;
    }
    const Bytes tail(b.begin() + static_cast<std::ptrdiff_t>(p), b.end());
    const bool multi = prm(g, mod::KmMulti) >= 0.5f;
    for (int pc = 0; pc < 12; ++pc) {
      const int r = ((pc - base) % 12 + 12) % 12;
      const int want = static_cast<int>(std::lround(prm(g, mod::KmMap + pc)));
      const bool wantBoth = multi && want != 0 && want > -13;
      auto& t = rec[static_cast<size_t>(r)];
      if (wasOn && prm(cur, mod::KmMap + pc) == static_cast<float>(want)) {
        // Same target: keep the record unless the "both" state must change.
        const bool hasSelf = std::find(t.begin(), t.end(), r) != t.end();
        const bool both = t.size() >= 2 && hasSelf;
        if (want <= -13 || want == 0 || both == wantBoth) continue;
      }
      if (want <= -13) t.clear();
      else if (wantBoth) t = {r, r + want};
      else t = {r + want};
    }
    ByteWriter w;
    w.bytes(b.data(), 24);
    for (const auto& t : rec) {
      w.u32(static_cast<uint32_t>(t.size()));
      for (int v : t) w.u16(static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(v))));
    }
    w.bytes(tail);
    b = w.b;
    putI32(b, 4, 1);  // enabled
  };
  e.fields = [](const Bytes&) { return std::vector<Field>{i32(mod::KmTranspose, 12, -12, 12)}; };
  return e;
}

// VFX Keyboard Splitter: u32 3, then 16 zones: key envelope, velocity
// envelope, i32 transpose, u8 absolute velocity, two spare bytes. Envelope:
// u32 3, u32 3, u32 N, N × {f64 Δx, f64 level, f32 tension, u32 shape}, then
// u32 0 and four i32 −1. Key envelopes span keys 0..132 over x 0..1,
// velocity envelopes 0..127.
constexpr uint32_t kShapeLinear = 0x02000000;  // segment shape word of every point after the first

void writeEnv(ByteWriter& w, const std::vector<std::pair<double, double>>& pts) {
  w.u32(3);
  w.u32(3);
  w.u32(static_cast<uint32_t>(pts.size()));
  double x = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    w.f64(pts[i].first - x);
    w.f64(pts[i].second);
    w.f32(0.0f);  // tension
    w.u32(i == 0 ? 0 : kShapeLinear);
    x = pts[i].first;
  }
  w.u32(0);
  for (int k = 0; k < 4; ++k) w.i32(-1);
}
// Envelope that is 1 over lo..hi (whole steps of 1/span) and 0 elsewhere,
// with FL's one-step ramps.
std::vector<std::pair<double, double>> rangeEnv(int lo, int hi, double span) {
  std::vector<std::pair<double, double>> p;
  if (lo > 0) {
    p.push_back({0, 0});
    if (lo > 1) p.push_back({(lo - 1) / span, 0});
  }
  p.push_back({lo / span, 1});
  if (hi > lo) p.push_back({hi / span, 1});
  if (hi + 1 < span) {
    p.push_back({(hi + 1) / span, 0});
    p.push_back({1, 0});
  } else {
    p.push_back({1, 1});
  }
  return p;
}

// Byte offset of each zone's transpose word (empty when malformed).
std::vector<size_t> splitTransposeAt(const Bytes& b, std::vector<bool>* plays = nullptr) {
  std::vector<size_t> out;
  size_t p = 0;
  for (size_t h : {4, 5})
    if (rdU32(b, h) == 3 && rdU32(b, h + 4) == 3) {
      p = h;
      break;
    }
  if (p == 0) return out;
  for (int z = 0; z < 16; ++z) {
    bool any[2] = {false, false};
    for (int e = 0; e < 2; ++e) {
      const uint32_t n = rdU32(b, p + 8);
      if (rdU32(b, p) != 3 || n == 0 || n > 512 || p + 12 + n * 24 + 20 > b.size()) return out;
      // Envelope levels at whole keys / velocities (the decoder's sampling).
      std::vector<double> xs, ys;
      double x = 0;
      for (uint32_t k = 0; k < n; ++k) {
        x += rdF64(b, p + 12 + 24 * k);
        xs.push_back(x);
        ys.push_back(rdF64(b, p + 20 + 24 * k));
      }
      const int count = e == 0 ? 132 : 128;
      const double span = e == 0 ? 132.0 : 127.0;
      for (int k = 0; k < count && !any[e]; ++k) {
        const double q = k / span;
        double y = ys.back();
        if (q <= xs.front()) {
          y = ys.front();
        } else {
          for (size_t i = 0; i + 1 < xs.size(); ++i) {
            if (q > xs[i + 1] || xs[i + 1] <= xs[i] || q < xs[i]) continue;
            y = ys[i] + (ys[i + 1] - ys[i]) * (q - xs[i]) / (xs[i + 1] - xs[i]);
            break;
          }
        }
        any[e] = y >= 0.5;
      }
      p += 12 + n * 24 + 20;
    }
    if (p + 7 > b.size()) return out;
    out.push_back(p);
    if (plays) plays->push_back(any[0] && any[1]);
    p += 7;
  }
  return out;
}

Encoder keyboardSplitter() {
  Encoder e = make("VFX Keyboard Splitter", "gl.split", 0);
  auto build = [](const PluginState* g) {
    ByteWriter w;
    w.u32(3);  // format
    int z = 0;
    for (int k = 0; k < 16; ++k) {
      // Next Gridline zone that is on (zones are imported in order).
      while (g && z < mod::kSplitZones && prm(*g, mod::splitParam(z, mod::SpOn)) < 0.5f) ++z;
      if (g && z < mod::kSplitZones) {
        const int lo = std::clamp(static_cast<int>(std::lround(prm(*g, mod::splitParam(z, mod::SpLow)))), 0, 127);
        const int hi = std::clamp(static_cast<int>(std::lround(prm(*g, mod::splitParam(z, mod::SpHigh)))), lo, 127);
        const int vlo = std::clamp(static_cast<int>(std::lround(prm(*g, mod::splitParam(z, mod::SpVelLow)) * 127.0)), 0, 127);
        const int vhi = std::clamp(static_cast<int>(std::lround(prm(*g, mod::splitParam(z, mod::SpVelHigh)) * 127.0)), vlo, 127);
        writeEnv(w, rangeEnv(lo, hi, 132.0));
        writeEnv(w, rangeEnv(vlo, vhi, 127.0));
        w.i32(static_cast<int32_t>(std::lround(prm(*g, mod::splitParam(z, mod::SpTranspose)))));
        ++z;
      } else if (!g && k == 0) {  // default: the first zone plays everything
        writeEnv(w, {{0, 1}, {1, 1}});
        writeEnv(w, {{0, 1}, {1, 1}});
        w.i32(0);
      } else {  // empty zone
        writeEnv(w, {{0, 0}, {1, 0}});
        writeEnv(w, {{0, 1}, {1, 1}});
        w.i32(0);
      }
      w.u8(0);  // absolute velocity: off
      w.u8(0);
      w.u8(0);
    }
    return w.b;
  };
  e.defaults = [build] { return build(nullptr); };
  e.write = [build](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    // Only the zone ranges need a rebuild; transposes are fields.
    auto notTranspose = [](int k) { return k >= mod::SpMode || k % mod::SpFields != mod::SpTranspose; };
    if (decoded("VFX Keyboard Splitter", b, c, cur) && sameParams(cur, g, notTranspose)) return;
    b = build(&g);
  };
  e.fields = [](const Bytes& b) {
    std::vector<bool> plays;
    std::vector<size_t> at = splitTransposeAt(b, &plays);
    std::vector<Field> f;
    int zi = 0;
    for (size_t z = 0; z < at.size() && zi < mod::kSplitZones; ++z)
      if (plays[z]) f.push_back(i32(mod::splitParam(zi++, mod::SpTranspose), at[z], -48, 48));
    return f;
  };
  return e;
}

// VFX Script: u32 length, UTF-8 INI text ([Script] Source=<lines joined by
// "<linebreak>">, [Parameters] name=value). A Gridline script imported from
// FL keeps the FL source and parameters as comments, which write() turns
// back into the INI; knobs A..C are stored as value × 2^18.
constexpr const char* kScriptHead = "# Converted from an FL Studio VFX script.\n";
constexpr double kVfxKnob = 262144.0;

Bytes scriptState(const std::vector<std::string>& source, const std::vector<std::pair<std::string, std::string>>& params) {
  std::string t = "\xEF\xBB\xBF[Script]\r\nSource=";
  for (size_t i = 0; i < source.size(); ++i) t += (i ? "<linebreak>" : "") + source[i];
  t += "\r\n";
  if (!params.empty()) {
    t += "[Parameters]\r\n";
    for (const auto& [k, v] : params) t += k + "=" + v + "\r\n";
  }
  t += "\r\n";
  ByteWriter w;
  w.u32(static_cast<uint32_t>(t.size()));
  w.bytes(t.data(), t.size());
  return w.b;
}

// An original pass-through script: every incoming voice is replayed as is.
std::vector<std::string> passThroughSource() {
  return {"# Gridline: notes pass through unchanged.",
          "import flvfx as vfx",
          "",
          "class Follower(vfx.Voice):",
          "  source = None",
          "",
          "def onTriggerVoice(incomingVoice):",
          "  out = Follower(incomingVoice)",
          "  out.source = incomingVoice",
          "  out.trigger()",
          "",
          "def onTick():",
          "  for out in vfx.context.voices:",
          "    out.copyFrom(out.source)",
          "",
          "def onReleaseVoice(incomingVoice):",
          "  for out in vfx.context.voices:",
          "    if out.source == incomingVoice:",
          "      out.release()"};
}

Encoder vfxScript() {
  Encoder e = make("VFX Script", "gl.notescript", 0);
  e.defaults = [] { return scriptState(passThroughSource(), {}); };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (decoded("VFX Script", b, c, cur) && cur == g) return;
    if (g.data.compare(0, std::strlen(kScriptHead), kScriptHead) != 0) return;  // a Gridline script: no FL source
    // The comment sections the decoder wrote: parameters, then the source.
    std::vector<std::string> source;
    std::vector<std::pair<std::string, std::string>> params;
    int section = 0;  // 1 parameters, 2 script
    size_t p = 0;
    while (p < g.data.size()) {
      size_t q = g.data.find('\n', p);
      const std::string ln = g.data.substr(p, q == std::string::npos ? std::string::npos : q - p);
      p = q == std::string::npos ? g.data.size() : q + 1;
      if (ln == "# --- parameters ---") {
        section = 1;
      } else if (ln == "# --- script ---") {
        section = 2;
      } else if (section == 1 && ln.rfind("# ", 0) == 0) {
        const size_t eq = ln.find(" = ", 2);
        if (eq != std::string::npos) params.emplace_back(ln.substr(2, eq - 2), ln.substr(eq + 3));
      } else if (section == 2) {
        if (ln.rfind("# ", 0) != 0) break;  // the Gridline formulas follow the source
        source.push_back(ln.substr(2));
      }
    }
    // Knobs A..C from the Gridline values (unchanged text when they still match).
    const std::pair<const char*, int> knobs[] = {{"Parameters: A", mod::NsA}, {"Parameters: B", mod::NsB}, {"Parameters: C", mod::NsC}};
    for (auto& [k, v] : params)
      for (const auto& [name, gl] : knobs) {
        if (k != name) continue;
        const double old = std::clamp(std::strtod(v.c_str(), nullptr) / kVfxKnob, 0.0, 1.0);
        if (static_cast<float>(old) != prm(g, gl)) v = std::to_string(std::lround(prm(g, gl) * kVfxKnob));
      }
    b = scriptState(source, params);
  };
  return e;
}

// VFX Envelope: the point lists ({i32 3, f32 ×3, i32 1, i32 3, i32 N, N ×
// {f64 Δbeats, f64 level, f32 tension, u32 shape}}) found as the decoder
// finds them; list 5·a = articulator a's first target. Point times and levels
// are fields; the layout around them is not built (no default state).
Encoder vfxEnvelope() {
  Encoder e = make("VFX Envelope", "gl.envctl", -1);
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    if (b.size() < 0x100 || getU16(b, 0) != 3) return f;
    int list = 0;
    for (size_t p = 0x40; p + 28 <= b.size();) {
      const int n = rdI32(b, p + 24);
      if (rdI32(b, p) == 3 && rdI32(b, p + 16) == 1 && rdI32(b, p + 20) == 3 && n >= 1 && n <= 64 && p + 28 + 24 * static_cast<size_t>(n) <= b.size()) {
        const int a = list / 5;
        if (list % 5 == 0 && a < mod::kEnvArts && n >= 2)
          for (int k = 0; k < std::min(n, mod::kEnvPoints); ++k) {
            const size_t at = p + 28 + 24 * static_cast<size_t>(k);
            f.push_back(field(mod::envPointTime(a, k), at, Type::F64, 0, 1000));
            f.push_back(field(mod::envPointLevel(a, k), at + 8, Type::F64, 0, 1));
          }
        ++list;
        p += 28 + 24 * static_cast<size_t>(n);
      } else {
        p += 4;
      }
    }
    return f;
  };
  return e;
}

// ======================================================= FlpDec4.cpp

// Fruity Big Clock: one byte, the display colour scheme.
Encoder bigClock() {
  Encoder e = make("Fruity Big Clock", "gl.clock", 0);
  e.defaults = [] { return Bytes{1}; };  // colour scheme 1
  return e;
}

// VFX Color Mapper: i32 version 0, 16 × i32 output port per note colour,
// 16 × i32 MIDI channel per input channel.
Encoder colorMapper() {
  Encoder e = make("VFX Color Mapper", "gl.colormap", 0);
  e.defaults = [] {
    ByteWriter w;
    w.i32(0);                                // version
    for (int k = 0; k < 16; ++k) w.i32(k);   // colour k → port k
    for (int k = 0; k < 16; ++k) w.i32(k);   // MIDI channel k → k
    return w.b;
  };
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int k = 0; k < mod::kColorGroups; ++k) f.push_back(i32(mod::CmMap + k, 4 + 4 * static_cast<size_t>(k), 0, 7));
    return f;
  };
  return e;
}

// VFX Level Scaler (J-45): u32 1, chunk 1002 (u8 editor tab), chunk 1001
// (6 targets × centre, multiply, offset, tension, humanize, mode as i32).
Encoder levelScaler() {
  Encoder e = make("VFX Level Scaler", "gl.velscale", 0);
  e.defaults = [] {
    ByteWriter w;
    w.u32(1);
    w.u32(1002), w.u32(1), w.u32(0), w.u8(0);
    w.u32(1001), w.u32(144), w.u32(0);
    for (int t = 0; t < mod::kVsTargets; ++t)
      for (int v : {t < 2 ? 0 : 4096, 8192, 0, 0, 0, 1}) w.i32(v);
    return w.b;
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    if (b.size() < 29 + 144) return f;
    for (int t = 0; t < mod::kVsTargets; ++t) {
      const size_t a = 29 + 24 * static_cast<size_t>(t);
      f.push_back(i32(mod::vsRef(t, mod::VfCenter), a, 0, 8192));
      f.push_back(i32(mod::vsRef(t, mod::VfMultiply), a + 4, -16384, 16384));
      f.push_back(i32(mod::vsRef(t, mod::VfOffset), a + 8, -8192, 8192));
      f.push_back(i32(mod::vsRef(t, mod::VfTension), a + 12, -8192, 8192));
      f.push_back(i32(mod::vsRef(t, mod::VfHumanize), a + 16, 0, 16384));
      f.push_back(i32(mod::vsRef(t, mod::VfMode), a + 20, 0, 2));
    }
    return f;
  };
  return e;
}

// VFX Sequencer (J-45): the FL installation's default bank (plugin database),
// with the current program's steps, length, sync and transpose as fields
// (see parseSeqBank).
Encoder sequencer() {
  Encoder e = make("VFX Sequencer", "gl.notesq", 0);
  e.defaultsIn = [](const EncodeContext& c) {
    if (c.factoryData.empty()) return Bytes{};
    File f;
    if (!readFile(c.factoryData + "/Data/System/Plugin databases/Plugin database (full)/Generators/Utility/VFX Sequencer.fst", f, nullptr))
      return Bytes{};
    const Event* s = pluginState(f.events);
    return s && parseSeqBank(s->data, s->size).ok ? Bytes(s->data, s->data + s->size) : Bytes{};
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const SeqBank s = parseSeqBank(b.data(), b.size());
    if (!s.ok) return f;
    f.push_back(field(mod::SqLength, s.prgp + 1, Type::U8, 1, 64));
    f.push_back(field(mod::SqRate, s.prgp + 3, Type::U8, 0, 12));
    f.push_back(field(mod::SqTranspose, s.prgp + 12, Type::I8, -2, 2));
    for (int i = 0; i < mod::kSeqSteps; ++i) {
      const size_t k = static_cast<size_t>(i);
      f.push_back(field(mod::seqParam(i, mod::SqOn), s.lanes[2] + k, Type::U8, 0, 5));
      f.push_back(field(mod::seqParam(i, mod::SqTie), s.lanes[2] + k, Type::U8, 0, 5));
      f.push_back(field(mod::seqParam(i, mod::SqNote), s.lanes[3] + k, Type::U8, 0, 255));
      f.push_back(field(mod::seqParam(i, mod::SqOctave), s.lanes[1] + k, Type::U8, 0, 127));
      f.push_back(field(mod::seqParam(i, mod::SqVelocity), s.lanes[4] + k, Type::U8, 0, 127));
      f.push_back(field(mod::seqParam(i, mod::SqGate), s.lanes[5] + k, Type::I8, -3, 5));
    }
    return f;
  };
  return e;
}

// Dashboard (J-45): header, value list, controller links, panel block (see
// parseDashboard). A state whose controls differ is rebuilt from the controls
// with the panel's default artwork (buildDashboard); the values are fields.
std::vector<std::pair<int, std::string>> surfaceControls(const PluginState& g) {
  std::vector<std::pair<int, std::string>> out;
  size_t at = 0;
  while (at < g.data.size() && out.size() < static_cast<size_t>(mod::kSurfaceControls)) {
    size_t e = g.data.find('\n', at);
    if (e == std::string::npos) e = g.data.size();
    const std::string line = g.data.substr(at, e - at);
    at = e + 1;
    const size_t sp = line.find(' ');
    if (line.empty()) continue;
    const int k = std::atoi(line.c_str());
    const std::string name = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    out.push_back({k == mod::SkSlider ? 1 : (k == mod::SkButton || k == mod::SkToggle) ? 4 : 2, name});
  }
  return out;
}
Encoder dashboard() {
  Encoder e = make("Dashboard", "gl.surface", -1, true);  // an FL instrument (J-81: never in a mixer slot)
  e.defaults = [] { return buildDashboard({}, {}); };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (decoded("Dashboard", b, c, cur) && cur.data == g.data) return;  // same controls: the values are fields
    const auto ctl = surfaceControls(g);
    std::vector<double> v;
    for (size_t k = 0; k < ctl.size(); ++k) v.push_back(prm(g, static_cast<int>(k)));
    b = buildDashboard(ctl, v);
  };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const DashPanel d = parseDashboard(b.data(), b.size());
    if (!d.ok) return f;
    for (size_t k = 0; k < d.links.size() && k < static_cast<size_t>(mod::kSurfaceControls); ++k)
      if (d.links[k].valueAt) f.push_back(field(static_cast<int>(k), d.links[k].valueAt, Type::U32, d.links[k].lo, d.links[k].hi));
    return f;
  };
  return e;
}

// Soundfont Player: the .sf2 path is a byte-length-prefixed string in the
// state; write() replaces it (the rest of the state stays).
Encoder soundfont() {
  Encoder e = make("Soundfont Player", "gl.multisampler", -1, true);
  // J-44: without the FL origin, the state starts from the FL installation's
  // plugin database preset (its bank / program / envelope / effect settings
  // are FL's defaults; their layout is not modelled here); write() puts the
  // instance's SoundFont in.
  e.defaultsIn = [](const EncodeContext& c) {
    if (c.factoryData.empty()) return Bytes{};
    File f;
    if (!readFile(c.factoryData + "/Data/Patches/Plugin database/Generators/Misc/SoundFont Player.fst", f, nullptr)) return Bytes{};
    const Event* s = pluginState(f.events);
    return s && s->size >= 16 ? Bytes(s->data, s->data + s->size) : Bytes{};
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (decoded("Soundfont Player", b, c, cur) && cur.data == g.data) return;
    const std::string pre = "#gl-zones 1\nsf2 0 0 - | ";
    if (g.data.compare(0, pre.size(), pre) != 0) return;
    std::string path = g.data.substr(pre.size());
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    if (path.find('\n') != std::string::npos) return;  // more than one zone
    path = flPath(path, c);
    if (path.empty() || path.size() > 255) return;
    for (size_t at = 0; at + 4 <= b.size(); ++at) {
      auto lower = [&](size_t i) { return static_cast<char>(std::tolower(b[i])); };
      if (!(lower(at) == '.' && lower(at + 1) == 's' && lower(at + 2) == 'f' && b[at + 3] == '2')) continue;
      // the string's start: where the length byte before it matches (J-44: a
      // length of 0x20 or more is itself a printable byte)
      size_t s = at, len = 0;
      for (; s >= 1 && b[s] >= 0x20; --s)
        if (static_cast<size_t>(b[s - 1]) == at + 4 - s) {
          len = at + 4 - s;
          break;
        }
      if (len == 0) return;  // not a length-prefixed string: leave it
      Bytes out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(s - 1));
      out.push_back(static_cast<uint8_t>(path.size()));
      out.insert(out.end(), path.begin(), path.end());
      out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(at + 4), b.end());
      b = out;
      return;
    }
  };
  return e;
}

// Fruity Phase Inverter: i32 version, i32 State 0..1024 (no shipped state:
// the version word is unknown, so no default state).
Encoder phaseInverter() {
  Encoder e = make("Fruity Phase Inverter", "gl.utility", -1);
  e.fields = [](const Bytes&) { return std::vector<Field>{i32(pp::UtlInvert, 4, 0, 1024)}; };
  return e;
}

// FL Studio Mobile Rack FX (J-42) ↔ gl.pocketrack: "SSLF" | u32 size | "10LF",
// HEAD (f64 tempo), RACK (u32 0, "10KR", RHED, RPRM, FOUT, then one RMOd per
// module: u32 size, u32 type, u32 id, HEAD, DESC, PRST, ADD1, PRMS (f32 × n),
// SMPR (3 × f32), optional CSTM). write() rebuilds the module chunks from the
// slots; a stored module of the same type keeps its own chunks (names, custom data).
namespace rack {
float rackParam(const gl::PluginState& s, int k) { return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : 0.0f; }
void chunk(ByteWriter& w, const char* tag, const Bytes& data) {
  w.bytes(tag, 4);
  w.u32(static_cast<uint32_t>(data.size()));
  w.bytes(data.data(), data.size());
}
struct Mod {
  size_t at = 0, end = 0;  // RMOd chunk (tag offset, end)
  uint32_t type = 0;
};
// Offsets of the RMOd chunks inside the RACK chunk; rackAt = RACK tag offset.
bool scan(const Bytes& b, size_t& rackAt, std::vector<Mod>& mods, size_t& modsFrom) {
  mods.clear();
  rackAt = std::string::npos;
  for (size_t q = 12; q + 8 <= b.size();) {
    const uint32_t sz = rdU32(b, q + 4);
    if (std::memcmp(b.data() + q, "RACK", 4) == 0) {
      rackAt = q;
      break;
    }
    q += 8 + sz;
  }
  if (rackAt == std::string::npos) return false;
  const size_t rEnd = std::min(b.size(), rackAt + 8 + rdU32(b, rackAt + 4));
  size_t q = rackAt + 8 + 8;  // u32 0, "10KR"
  modsFrom = rEnd;
  while (q + 8 <= rEnd) {
    const uint32_t sz = rdU32(b, q + 4);
    if (std::memcmp(b.data() + q, "RMOd", 4) == 0) {
      if (modsFrom == rEnd) modsFrom = q;
      mods.push_back({q, std::min(rEnd, q + 8 + sz), rdU32(b, q + 8)});
    }
    q += 8 + sz;
  }
  return true;
}
Bytes floats(const float* v, int n) {
  ByteWriter w;
  for (int i = 0; i < n; ++i) w.f32(v[i]);
  return w.b;
}
// A module chunk for slot s (template = the stored chunk of the same type, or none).
Bytes module(const gl::PluginState& g, int s, const Bytes* tpl, size_t tAt, size_t tEnd) {
  namespace dy = gl::packs::dynamics;
  auto P = [&](int f) { return rackParam(g, dy::pkParam(s, f)); };
  const int type = static_cast<int>(std::lround(P(dy::PkType)));
  float prm[dy::kPkParams];
  for (int i = 0; i < dy::kPkParams; ++i) prm[i] = P(dy::PkP0 + i);
  const float smpr[3] = {P(dy::PkPost), P(dy::PkMix), P(dy::PkPan)};
  int count = dy::pocketModuleParamCount(type);
  ByteWriter body;
  body.u32(static_cast<uint32_t>(type));
  if (tpl) {
    body.u32(rdU32(*tpl, tAt + 12));
    for (size_t q = tAt + 16; q + 8 <= tEnd;) {
      const uint32_t sz = rdU32(*tpl, q + 4);
      const Bytes data(tpl->begin() + static_cast<std::ptrdiff_t>(q + 8), tpl->begin() + static_cast<std::ptrdiff_t>(std::min(tEnd, q + 8 + sz)));
      if (std::memcmp(tpl->data() + q, "PRMS", 4) == 0) {
        count = std::max(count, static_cast<int>(sz / 4));
        chunk(body, "PRMS", floats(prm, std::min(count, dy::kPkParams)));
      } else if (std::memcmp(tpl->data() + q, "SMPR", 4) == 0) {
        chunk(body, "SMPR", floats(smpr, 3));
      } else {
        char tag[4];
        std::memcpy(tag, tpl->data() + q, 4);
        body.bytes(tag, 4);
        body.u32(sz);
        body.bytes(data.data(), data.size());
      }
      q += 8 + sz;
    }
  } else {
    body.u32(static_cast<uint32_t>(s + 1));
    ByteWriter h;
    h.u32(static_cast<uint32_t>(type)), h.u32(0), h.u8(1);
    chunk(body, "HEAD", h.b);
    Bytes desc(516, 0);
    std::memcpy(desc.data(), "Default", 7), std::memcpy(desc.data() + 256, "Default", 7);
    chunk(body, "DESC", desc);
    ByteWriter pr;
    const std::string path = "Default\\Default";
    pr.u32(0), pr.u32(static_cast<uint32_t>(path.size())), pr.bytes(path.data(), path.size());
    chunk(body, "PRST", pr.b);
    chunk(body, "ADD1", Bytes{0, 0, 0x7f});
    chunk(body, "PRMS", floats(prm, count));
    chunk(body, "SMPR", floats(smpr, 3));
  }
  ByteWriter w;
  chunk(w, "RMOd", body.b);
  return w.b;
}
}  // namespace rack

Encoder mobileRack() {
  namespace dy = gl::packs::dynamics;
  Encoder e = make("FL Studio Mobile Rack FX", "gl.pocketrack", 0);
  e.defaults = [] {
    ByteWriter rk;
    rk.u32(0), rk.bytes("10KR", 4);
    ByteWriter rh;
    rh.u32(0xFFFFFFFFu), rh.u32(0), rh.u32(0), rh.u32(0xFFFFFFFFu);
    rack::chunk(rk, "RHED", rh.b);
    const float rp[6] = {0.85312f, 0.5f, 0, 0, 0.5f, 0};
    rack::chunk(rk, "RPRM", rack::floats(rp, 6));
    ByteWriter fo;
    fo.u32(0xFFFFFFFFu), fo.u32(0);
    rack::chunk(rk, "FOUT", fo.b);
    ByteWriter body;
    body.bytes("10LF", 4);
    ByteWriter hd;
    hd.f64(140.0);
    rack::chunk(body, "HEAD", hd.b);
    rack::chunk(body, "RACK", rk.b);
    ByteWriter w;
    w.bytes("SSLF", 4), w.u32(static_cast<uint32_t>(body.b.size())), w.bytes(body.b.data(), body.b.size());
    return w.b;
  };
  e.write = [](const gl::PluginState& g, Bytes& b, const EncodeContext& c) {
    gl::PluginState cur;
    if (decoded("FL Studio Mobile Rack FX", b, c, cur) && cur.params == g.params) return;
    size_t rackAt = 0, modsFrom = 0;
    std::vector<rack::Mod> mods;
    if (b.size() < 12 || std::memcmp(b.data(), "SSLF", 4) != 0 || !rack::scan(b, rackAt, mods, modsFrom)) return;
    const size_t rEnd = std::min(b.size(), rackAt + 8 + rdU32(b, rackAt + 4));
    ByteWriter mw;
    for (int s = 0, k = 0; s < dy::kPkSlots; ++s) {
      const int type = static_cast<int>(std::lround(rack::rackParam(g, dy::pkParam(s, dy::PkType))));
      if (type <= 0) continue;
      const rack::Mod* t = k < static_cast<int>(mods.size()) && mods[static_cast<size_t>(k)].type == static_cast<uint32_t>(type) ? &mods[static_cast<size_t>(k)] : nullptr;
      const Bytes m = rack::module(g, s, t ? &b : nullptr, t ? t->at : 0, t ? t->end : 0);
      mw.bytes(m.data(), m.size());
      ++k;
    }
    for (size_t k = static_cast<size_t>(dy::kPkSlots); k < mods.size(); ++k)  // modules beyond the slots stay
      mw.bytes(b.data() + mods[k].at, mods[k].end - mods[k].at);
    // Rack payload before the modules + new modules (+ whatever followed the last module).
    Bytes rackBody(b.begin() + static_cast<std::ptrdiff_t>(rackAt + 8), b.begin() + static_cast<std::ptrdiff_t>(modsFrom));
    rackBody.insert(rackBody.end(), mw.b.begin(), mw.b.end());
    const size_t tailFrom = mods.empty() ? modsFrom : mods.back().end;
    rackBody.insert(rackBody.end(), b.begin() + static_cast<std::ptrdiff_t>(tailFrom), b.begin() + static_cast<std::ptrdiff_t>(rEnd));
    Bytes out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(rackAt));
    ByteWriter rw;
    rack::chunk(rw, "RACK", rackBody);
    out.insert(out.end(), rw.b.begin(), rw.b.end());
    out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(rEnd), b.end());
    const uint32_t body = static_cast<uint32_t>(out.size() - 8);
    for (int i = 0; i < 4; ++i) out[static_cast<size_t>(4 + i)] = static_cast<uint8_t>(body >> (8 * i));
    b = out;
  };
  return e;
}

}  // namespace

void registerDec3(std::vector<Encoder>& v) {
  // FlpDec3Fx.cpp
  v.push_back(peakController());
  v.push_back(center());
  v.push_back(spectroman());
  v.push_back(tuner());
  v.push_back(sevenBand());
  v.push_back(transmitter());
  v.push_back(multibandDelay());
  // FlpDec3Vfx.cpp
  v.push_back(keyMapper());
  v.push_back(keyboardSplitter());
  v.push_back(vfxScript());
  v.push_back(vfxEnvelope());
  // FlpDec3Inst.cpp: Slicer 2's decoder reads nothing (an empty slicer).
  v.push_back(make("Fruity Slicer 2", "gl.slicer", -1, true));
  // FlpDec4.cpp
  v.push_back(bigClock());
  v.push_back(colorMapper());
  v.push_back(levelScaler());
  v.push_back(sequencer());
  v.push_back(dashboard());
  v.push_back(soundfont());
  v.push_back(mobileRack());
  v.push_back(make("Fruity Mute 2", "gl.mute", 0));  // no stored setting decoded: FL defaults
  v.push_back(phaseInverter());
  v.push_back(make("Wasp", "gl.analog", -1, true));
  v.push_back(make("Wasp XT", "gl.analog", -1, true));
  v.push_back(make("FL Slayer", "gl.string", -1, true));
  v.push_back(make("Fruity LSD", "gl.midiout", -1, true));
}

}  // namespace gl::flp::enc

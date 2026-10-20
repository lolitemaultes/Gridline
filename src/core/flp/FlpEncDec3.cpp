// Gridline — FLP export: plugin state encoders for FlpDec3Fx/Inst/Kepler/Vfx.cpp, FlpDec4.cpp, FlpDecoders3.cpp.
// The reverse of the decoders in those files; see FlpEncode.h for the model.
//
// Default states are built field by field from the plugins' published
// controls and the defaults FL's plugin database records (no FL bytes are
// copied). Plugins whose decoder reads nothing (Level Scaler, Sequencer,
// Mute 2, Wasp, FL Slayer, LSD, Slicer 2) or whose layout is too large to
// build (VFX Envelope, Dashboard, Soundfont Player, Mobile Rack FX) have no
// default state: FL loads them with their own defaults, which is what the
// decoder produces for an empty state.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/modulation/modulation.h"
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
        i32(mod::PkBase, 0, 0, 65536),     i32(mod::PkAmount, 4, -256, 256), i32(mod::PkTension, 8, -128, 128),
        i32(mod::PkDecay, 12, 5, 128),     i32(mod::LfBase, 16, 0, 65536),   i32(mod::LfAmount, 20, -256, 256),
        i32(mod::LfTension, 24, -128, 128), i32(mod::LfRate, 28, 200, 65536), i32(mod::LfShape, 32, 0, 4),
        i32(mod::LfPhase, 36, 0, 65535),   field(mod::PkMute, 40, Type::U8, 0, 1), field(mod::PkRamp, 41, Type::U8, 0, 1)};
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
    return std::vector<Field>{i32(pp::TnReactivity, 4, 0, 6000), i32(pp::TnBassMode, 12, 0, 1), i32(pp::TnDisplay, 16, 1, 2)};
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
  e.fields = [](const Bytes& b) {
    std::vector<Field> f = {
        i32(dyn::TsSensitivity, tmTag(0), 0, 10000), i32(dyn::TsHold, tmTag(1), 0, 10000), i32(dyn::TsReleaseShape, tmTag(2), 0, 10000),
        i32(dyn::TsAttack, tmTag(3), 0, 12800),      i32(dyn::TsRelease, tmTag(4), 0, 12800), i32(dyn::TsTransTilt, tmTag(5), 0, 10000),
        i32(dyn::TsSusTilt, tmTag(6), 0, 10000),     i32(dyn::TsLookahead, tmTag(33), 0, 1),  i32(dyn::TsStereoLink, tmTag(34), 0, 1),
        i32(dyn::TsAttackShape, tmTag(35), 0, 2),    i32(dyn::TsTransOn, tmTag(36), 0, 1),    i32(dyn::TsSusOn, tmTag(37), 0, 1),
        i32(dyn::TsTransSync, tmTag(40), 0, 1),      i32(dyn::TsSusSync, tmTag(41), 0, 1)};
    // Effect slots: the kind first, then the knobs of the stored kind.
    for (int side = 0; side < 2; ++side) {
      const int o = side * dyn::kTsSlotStride;
      f.push_back(i32(dyn::TsTransFx + o, tmTag(38 + side), 0, 5));
      const int fx = std::clamp(rdI32(b, tmTag(38 + side)), 0, 5);
      const bool sync = rdI32(b, tmTag(40 + side)) >= 1;
      int mix = -1, a = -1, bb = -1;
      switch (fx) {
        case dyn::TsFxSaturate: mix = 13 + 3 * side, a = 14 + 3 * side, bb = 15 + 3 * side; break;
        case dyn::TsFxBounce: mix = 19 + 3 * side, a = 21 + 3 * side, bb = 20 + 3 * side; break;
        case dyn::TsFxDiffuse:
        case dyn::TsFxBlur: mix = 7 + 3 * side, a = 8 + 3 * side, bb = 9 + 3 * side; break;
        case dyn::TsFxEcho: {
          const int base = 25 + 4 * side;
          mix = base, a = base + 3, bb = sync ? base + 2 : base + 1;
          break;
        }
        default: break;
      }
      if (mix < 0) continue;
      f.push_back(i32(dyn::TsTransFxMix + o, tmTag(mix), 0, 10000));
      f.push_back(i32(dyn::TsTransFxA + o, tmTag(a), 0, 10000));
      if (fx == dyn::TsFxEcho && sync) f.push_back(i32(dyn::TsTransFxB + o, tmTag(bb), 1, 768));
      else f.push_back(i32(dyn::TsTransFxB + o, tmTag(bb), 0, 10000));
    }
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
// Point i of a curve from 16 stored bands, as the decoder interpolates it.
float mbdPoint(const int32_t* s, int i) {
  const double pos = 15.0 * i / (sfx::kBdPoints - 1);
  const int k = std::clamp(static_cast<int>(pos), 0, 14);
  const double f = std::clamp(pos - k, 0.0, 1.0);
  const double a = s[k] / kMbdOne, c = s[k + 1] / kMbdOne;
  return static_cast<float>(std::clamp(a + (c - a) * f, 0.0, 1.0));
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

// 16 stored bands whose interpolated points print as `want`.
void mbdFit(const float* want, int32_t* s) {
  std::string target[sfx::kBdPoints];
  for (int i = 0; i < sfx::kBdPoints; ++i) target[i] = curveText(want[i]);
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
  // Points that depend on band k.
  auto cost = [&](int k) {
    int bad = 0;
    double err = 0;
    for (int i = 0; i < sfx::kBdPoints; ++i) {
      const double pos = 15.0 * i / (sfx::kBdPoints - 1);
      const int kk = std::clamp(static_cast<int>(pos), 0, 14);
      if (kk != k && kk + 1 != k) continue;
      const float v = mbdPoint(s, i);
      if (curveText(v) != target[i]) ++bad;
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
    out = {0, 1};
    for (int i = 0; i < sfx::kBdPoints; ++i) {
      const double pos = 15.0 * i / (sfx::kBdPoints - 1);
      const int kk = std::clamp(static_cast<int>(pos), 0, 14);
      const double f = std::clamp(pos - kk, 0.0, 1.0);
      if (kk == k && f < 1) out.push_back((want[i] - f * (s[k + 1] / kMbdOne)) / (1 - f));
      if (kk + 1 == k && f > 0) out.push_back((want[i] - (1 - f) * (s[kk] / kMbdOne)) / f);
    }
  };
  for (int pass = 0; pass < 4; ++pass) {
    bool any = false;
    for (int k = 0; k + 1 < 16; ++k) {
      if (!bad(k) && !bad(k + 1)) continue;
      any = true;
      const int32_t keepA = s[k], keepB = s[k + 1];
      int32_t bestA = keepA, bestB = keepB;
      double bc = cost(k) + cost(k + 1);
      std::vector<double> candA;
      pointValues(k, candA);
      for (double step : {1e-4, 1e-5, 1e-6, 1e-7})
        for (int j = -12; j <= 12; ++j) candA.push_back(keepA / kMbdOne + j * step);
      for (double va : candA) {
        if (!(va >= 0 && va <= 1)) continue;
        for (int32_t da = -1; da <= 1; ++da) {
          s[k] = std::clamp(static_cast<int32_t>(std::lround(va * kMbdOne)) + da, 0, static_cast<int32_t>(kMbdOne));
          std::vector<double> candB;
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
    for (int grp = 0; grp < 2; ++grp)
      for (int t = 0; t < 3; ++t) {
        int32_t s[16];
        mbdFit(cv.v[grp][kMbdTargets[t]], s);
        for (int k = 0; k < 16; ++k) putI32(b, mbdAt((bankA + grp) * 48 + t * 16 + k), s[k]);
      }
  };
  e.fields = [](const Bytes& b) {
    const int bankA = std::min(6, std::clamp(rdI32(b, mbdAt(390)), 0, 7000) / 1000);
    return std::vector<Field>{
        i32(sfx::BdMaxTime, mbdAt(384), -6000, 6000),
        i32(sfx::BdFeedback, mbdAt(386), 0, 6000),
        i32(sfx::BdSmooth, mbdAt(387), 0, 6000),
        i32(sfx::BdKeepPitch, mbdAt(389), 0, 1),
        i32(sfx::BdMorph, mbdAt(390), bankA * 1000, bankA * 1000 + 999),
        i32(sfx::BdDry, mbdAt(391), -128, 128),
        i32(sfx::BdWet, mbdAt(391), -128, 128)};
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

// Dashboard: i32 version, …, @28 u32 N, N × u32 values (value 0 belongs to
// the panel); the control records are not built (no default state).
Encoder dashboard() {
  Encoder e = make("Dashboard", "gl.surface", -1);
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const uint32_t n = rdU32(b, 28);
    if (b.size() < 32 || n > 4096 || 32 + 4 * static_cast<size_t>(n) > b.size()) return f;
    for (uint32_t k = 1; k < n && static_cast<int>(k - 1) < mod::kSurfaceControls; ++k)
      f.push_back(field(static_cast<int>(k - 1), 32 + 4 * static_cast<size_t>(k), Type::U32, 0, 65536));
    return f;
  };
  return e;
}

// Soundfont Player: the .sf2 path is a byte-length-prefixed string in the
// state; write() replaces it (the rest of the state stays).
Encoder soundfont() {
  Encoder e = make("Soundfont Player", "gl.multisampler", -1, true);
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
      size_t s = at;
      while (s >= 1 && b[s - 1] >= 0x20) --s;
      if (s == 0) return;
      const size_t len = at + 4 - s;
      if (static_cast<size_t>(b[s - 1]) != len) return;  // not a length-prefixed string: leave it
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
  v.push_back(make("VFX Level Scaler", "gl.velscale", 0));  // settings not decoded: FL defaults
  v.push_back(make("VFX Sequencer", "gl.notesq", 0));       // pattern bank not decoded: FL defaults
  v.push_back(dashboard());
  v.push_back(soundfont());
  {
    // The rack's Gridline type is its first module's: any type (no settings decoded).
    Encoder e = make("FL Studio Mobile Rack FX", "", -1);
    v.push_back(e);
  }
  v.push_back(make("Fruity Mute 2", "gl.mute", 0));  // no stored setting decoded: FL defaults
  v.push_back(phaseInverter());
  v.push_back(make("Wasp", "gl.analog", -1, true));
  v.push_back(make("Wasp XT", "gl.analog", -1, true));
  v.push_back(make("FL Slayer", "gl.string", -1, true));
  v.push_back(make("Fruity LSD", "gl.midiout", -1, true));
}

}  // namespace gl::flp::enc

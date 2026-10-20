// Gridline — FLP import: decoders for FL EQ, filters, stereo, utility and
// distortion effects (Parametric EQ 2, Free Filter, Filter, Balance, Stereo
// Enhancer, Stereo Shaper, Soft Clipper, Fast Dist, WaveShaper).
//
// State layouts (observed in saved projects and shipped presets; see
// analysis/demos/notes_gamma.md). "ints" = little-endian int32:
//   Parametric EQ 2  ver(6|8), 36 ints by tag: level 0-6 (±1800 = ±18 dB),
//                    freq 7-13 (0..65536), width 14-20 (0..65536), type 21-27,
//                    order 28-34 (-3..3), main level 35; then the A/B spare
//                    state (36 ints) and option words.
//   Free Filter      ver 2, type 100..700, freq, Q, gain (0..1024) + u8.
//   Filter           ver 2, cutoff, reso, low, band, high (0..1024), x2 + u8.
//   Balance          pan (-128..128), volume (0..320, 256 = unity).
//   Stereo Enhancer  pan, volume, separation (-96..96), phase offset
//                    (-512..512), effect position, inversion.
//   Stereo Shaper    ver 0, R→L, L, R, L→R (±25600, 12800 = unity), delay,
//                    dephase (±4096), split mode, flag.
//   Soft Clipper     threshold (1..127), post gain (0..160, 128 = unity).
//   Fast Dist        pre amp (64..192), threshold (1..10), type, mix (0..128),
//                    post gain (0..128).
//   WaveShaper       ver, pre amp, mix, post gain, 3 flag bytes (bipolar,
//                    remove DC, …), 2 words, point count, points
//                    (ver 1: f32 dx, y, tension; ver ≥ 3: f64 dx, f64 y,
//                    f32 tension, 3 bytes, u8 segment type).
#include <algorithm>
#include <array>
#include <cmath>
#include "../PluginParams.h"
#include "../../packs/dynamics/dynamics.h"
#include "../../packs/utility/utility.h"
#include "../../engine/FlFilterRef.h"
#include "FlpFx.h"

namespace gl::flp::fx {

namespace {

namespace pp = gl::pp;

double ratioDb(double v, double unity, double floorDb) {
  return v <= 0 ? floorDb : std::max(floorDb, 20.0 * std::log10(v / unity));
}

// Reads `count` int32 values after an optional version word.
std::vector<double> ints(Reader& r, size_t count) {
  std::vector<double> v;
  for (size_t i = 0; i < count && r.has(4); ++i) v.push_back(r.i32());
  return v;
}

// ------------------------------------------------------------------ Parametric EQ 2

int eq2Type(int fl) {
  static const int map[8] = {pp::EqOff, pp::EqHighCut, pp::EqBandPass, pp::EqLowCut, pp::EqNotch, pp::EqLowShelf, pp::EqPeak, pp::EqHighShelf};
  return map[std::clamp(fl, 0, 7)];
}

}  // namespace

// Parametric EQ 2 band frequency word → Hz: logarithmic 20 Hz..20 kHz (the
// plugin's own note-snap code converts Hz back as log(f / 20) scaled to 0..65536).
double flEq2FreqHz(double v) { return 20.0 * std::pow(1000.0, std::clamp(v / 65536.0, 0.0, 1.0)); }

// Bandwidth word → Q of a peak band (FL Parametric EQ 2: bandwidth
// 0.1 + 4.9 · v/65536 octaves). Kept for callers that want a Q.
double flEq2BandwidthQ(double v) {
  double bw = 0.1 + 4.9 * std::clamp(v / 65536.0, 0.0, 1.0);
  return 1.0 / (2.0 * std::sinh(0.5 * std::log(2.0) * bw));
}

namespace {

bool decodeEq2(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 36 * 4) return false;
  r.i32();  // version
  std::vector<double> t = ints(r, 36);
  begin(m, "gl.eq");
  // Exact: Gridline's reference band model uses the plugin's own width / order
  // semantics (types, levels, frequencies, widths, orders map 1:1).
  m.exact = true;
  set(m, pp::EqModel, 1);
  for (int b = 0; b < 7; ++b) {
    int type = static_cast<int>(t[static_cast<size_t>(21 + b)]);
    int order = static_cast<int>(t[static_cast<size_t>(28 + b)]);
    set(m, pp::eqParam(b, pp::EqType), eq2Type(type));
    set(m, pp::eqParam(b, pp::EqGain), std::clamp(t[static_cast<size_t>(b)] / 100.0, -18.0, 18.0));
    set(m, pp::eqParam(b, pp::EqFreq), std::clamp(flEq2FreqHz(t[static_cast<size_t>(7 + b)]), 20.0, 20000.0));
    set(m, pp::eqParam(b, pp::EqQ), std::clamp(flEq2BandwidthQ(t[static_cast<size_t>(14 + b)]), 0.1, 18.0));
    set(m, pp::eqFl(b, pp::EqWidth), std::clamp(t[static_cast<size_t>(14 + b)] / 65536.0, 0.0, 1.0));
    set(m, pp::eqFl(b, pp::EqFlOrder), std::clamp(order, -3, 3) + 3);
  }
  set(m, pp::EqMain, std::clamp(t[35] / 100.0, -18.0, 18.0));
  // Automation: FL tags.
  auto table = std::make_shared<KnobTable>();
  for (int b = 0; b < 7; ++b) {
    table->push_back({b, -1800, 1800, pp::eqParam(b, pp::EqGain), [](double v) { return v / 100.0; }});
    table->push_back({7 + b, 0, 65536, pp::eqParam(b, pp::EqFreq), [](double v) { return flEq2FreqHz(v); }});
    table->push_back({14 + b, 0, 65536, pp::eqFl(b, pp::EqWidth), [](double v) { return std::clamp(v / 65536.0, 0.0, 1.0); }});
    table->push_back({21 + b, 0, 7, pp::eqParam(b, pp::EqType), [](double v) { return eq2Type(static_cast<int>(std::lround(v))); }});
    table->push_back({28 + b, -3, 3, pp::eqFl(b, pp::EqFlOrder), [](double v) { return std::clamp(std::lround(v), -3L, 3L) + 3.0; }});
  }
  table->push_back({35, -1800, 1800, pp::EqMain, [](double v) { return v / 100.0; }});
  bindTable(m, table);
  return true;
}

// ------------------------------------------------------------------ simple knob plugins
//
// One decoder for plugins whose state is a list of int32 knob values in tag
// order (after an optional version word).
struct SimplePlugin {
  std::string glType;
  bool versioned = false;      // a version word precedes the knobs
  bool provisional = true;     // value curves not yet confirmed against the FL plugin: mapped approximately
  size_t knobs = 0;            // number of int32 knob values
  KnobTable table;
  std::function<void(PluginMapping&, const std::vector<double>&)> extra;  // non-table settings
};

PluginDecoder simpleDecoder(std::shared_ptr<const SimplePlugin> sp) {
  return [sp](Context&, const std::vector<Event>& evs, PluginMapping& m) {
    Reader r(pluginState(evs));
    if (sp->versioned) r.i32();
    std::vector<double> v = ints(r, sp->knobs);
    if (v.size() < sp->knobs) return false;
    begin(m, sp->glType);
    m.exact = !sp->provisional;
    applyTable(m, sp->table, v);
    if (sp->extra) sp->extra(m, v);
    bindTable(m, std::shared_ptr<const KnobTable>(sp, &sp->table));
    return true;
  };
}

// --- Balance → Utility
std::shared_ptr<const SimplePlugin> balance() {
  auto sp = std::make_shared<SimplePlugin>();
  sp->glType = "gl.utility";
  sp->knobs = 2;
  sp->provisional = false;  // linear pan / volume ratio (256 = unity)
  sp->table = {{0, -128, 128, pp::UtlPan, [](double v) { return v / 128.0; }},
               {1, 0, 320, pp::UtlGain, [](double v) { return v >= 256 && v <= 256 ? 0.0 : std::clamp(20.0 * std::log10(std::max(1e-9, (std::pow(11.0, v / 256.0) - 1.0) / 10.0)), -36.0, 24.0); }}};
  sp->extra = [](PluginMapping& m, const std::vector<double>&) { set(m, pp::UtlPanLaw, 2); };  // reference folded balance law
  return sp;
}

// --- Soft Clipper → Soft Clipper (κ2, exact): threshold amplitude v/128 with an
// exponential approach to full scale above it; post gain (11^(v/128) - 1) / 10.
std::shared_ptr<const SimplePlugin> softClipper() {
  auto sp = std::make_shared<SimplePlugin>();
  sp->glType = "gl.clipper";
  sp->knobs = 2;
  sp->provisional = false;
  sp->table = {{0, 1, 127, pp::ClpThreshold, [](double v) { return std::clamp(ratioDb(v, 128, -42.2), -42.2, 0.0); }},
               {1, 0, 160, pp::ClpPostGain, [](double v) {
                  double g = v == 128 ? 1.0 : (std::pow(11.0, v / 128.0) - 1.0) / 10.0;
                  return std::clamp(g <= 0 ? -60.0 : 20.0 * std::log10(g), -60.0, 12.0);
                }}};
  sp->extra = [](PluginMapping& m, const std::vector<double>&) {
    set(m, pp::ClpCeilingMode, 1);
    set(m, pp::ClpOversample, 0);
    set(m, pp::ClpInput, 0);
    set(m, pp::ClpMix, 1);
  };
  return sp;
}

// --- Fast Dist → Clip Dist
std::shared_ptr<const SimplePlugin> fastDist() {
  namespace dy = gl::packs::dynamics;
  auto sp = std::make_shared<SimplePlugin>();
  sp->glType = "gl.fastdist";
  sp->knobs = 5;
  sp->table = {{0, 64, 192, dy::FdPreAmp, [](double v) { return std::clamp(ratioDb(v, 128, -12), -12.0, 24.0); }},
               {1, 1, 10, dy::FdThreshold, [](double v) { return v / 10.0; }},
               {2, 0, 1, dy::FdType, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }},
               {3, 0, 128, dy::FdMix, [](double v) { return v / 128.0; }},
               {4, 0, 128, dy::FdPost, [](double v) { return std::clamp(ratioDb(v, 128, -24), -24.0, 12.0); }}};
  return sp;
}

// --- Stereo Shaper → Stereo Matrix
std::shared_ptr<const SimplePlugin> stereoShaper() {
  namespace ut = gl::packs::utility;
  auto sp = std::make_shared<SimplePlugin>();
  sp->glType = "gl.stereomatrix";
  sp->versioned = true;
  sp->provisional = false;
  sp->knobs = 6;
  auto g = [](double v) { return std::clamp(v / 12800.0, -2.0, 2.0); };
  sp->table = {{0, -25600, 25600, ut::SmRL, g}, {1, -25600, 25600, ut::SmLL, g}, {2, -25600, 25600, ut::SmRR, g},
               {3, -25600, 25600, ut::SmLR, g}, {4, -4096, 4096, ut::SmDelayK, [](double v) { return std::clamp(v / 4096.0, -1.0, 1.0); }},
               {5, -4096, 4096, ut::SmDephaseK, [](double v) { return std::clamp(v / 4096.0, -1.0, 1.0); }}};
  sp->knobs = 8;  // + difference output, effects position
  sp->extra = [](PluginMapping& m, const std::vector<double>& v) {
    set(m, ut::SmClassic, 1);  // reference gain law, delay and dephase curves
    set(m, ut::SmFxPos, v[7] >= 0.5 ? 1 : 0);
    set(m, ut::SmDiffTo, std::clamp(v[6], 0.0, 500.0));  // J-45: FL route index of the difference output
  };
  return sp;
}

// --- Stereo Enhancer → Stereo Shaper
std::shared_ptr<const SimplePlugin> stereoEnhancer() {
  auto sp = std::make_shared<SimplePlugin>();
  sp->glType = "gl.stereo";
  sp->knobs = 6;
  sp->provisional = false;
  sp->table = {{0, -128, 128, pp::StPan, [](double v) { return v / 128.0; }},
               {1, 0, 320, pp::StVolK, [](double v) { return std::clamp(v / 256.0, 0.0, 1.25); }},
               {2, -96, 96, pp::StSep, [](double v) { return std::clamp(v / 96.0, -1.0, 1.0); }},
               {3, -512, 512, pp::StPhaseK, [](double v) { return std::clamp(v / 512.0, -1.0, 1.0); }},
               {4, 0, 1, pp::StFxPos, [](double v) { return v >= 0.5 ? 1.0 : 0.0; }},
               {5, 0, 2, pp::StInvert, [](double v) { return std::clamp(std::round(v), 0.0, 2.0); }}};
  return sp;
}

// --- Free Filter → Free Filter
// J-26: reference model (FfModel = 1, spec j26_freefilter.md). State: version,
// type 100..700 (older states store a smaller type code), frequency, Q, gain
// (0..1024), then (version 2) 1 byte: DC remover.
bool decodeFreeFilter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  namespace ut = gl::packs::utility;
  Reader r(pluginState(evs));
  if (r.n < 4 + 4 * 4) return false;
  const int version = r.i32();
  std::vector<double> t;
  for (int k = 0; k < 4; ++k) t.push_back(r.i32());
  if (t[0] < 100) t[0] = 100 + std::nearbyint(600.0 * (std::floor(t[0] / 10.0) - 0.5) / 7.0);  // older type code
  const bool center = version >= 2 && r.left() >= 1 ? (r.u8() & 1) != 0 : false;
  begin(m, "gl.freefilter");
  set(m, ut::FfModel, 1);
  auto n = [](double v) { return std::clamp(v / 1024.0, 0.0, 1.0); };
  KnobTable tab = {{0, 100, 700, ut::FfType, [](double v) { return static_cast<double>(flref::freeFilterType((v - 100.0) / 600.0)); }},
                   {1, 0, 1024, ut::FfFreq, [n](double v) { return flref::freeFilterHz(n(v)); }},
                   {2, 0, 1024, ut::FfQK, n},
                   {3, 0, 1024, ut::FfGain, [n](double v) { return flref::freeFilterDb(n(v)); }}};
  applyTable(m, tab, t);
  set(m, ut::FfDc, center ? 1 : 0);
  m.exact = true;
  bindTable(m, std::make_shared<const KnobTable>(tab));
  return true;
}

// --- Filter → Filter
// J-26: classic model (FltModel = 1, spec j26_filter.md). State: version 1
// (6 ints) or 2 (+ 1 byte: DC remover); x2 is on only for the raw value 1.
bool decodeFruityFilter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4 + 6 * 4) return false;
  const int version = r.i32();
  std::vector<double> t;
  for (int k = 0; k < 6; ++k) t.push_back(r.i32());
  const bool center = version >= 2 && r.left() >= 1 ? (r.u8() & 1) != 0 : false;
  begin(m, "gl.filter");
  set(m, pp::FltModel, 1);
  auto lvl = [](double v) { return std::clamp(v / 1024.0, 0.0, 1.0); };
  KnobTable tab = {{0, 0, 1024, pp::FltCutK, lvl},
                   {1, 0, 1024, pp::FltResK, lvl},
                   {2, 0, 1024, pp::FltLPLevel, lvl},
                   {3, 0, 1024, pp::FltBPLevel, lvl},
                   {4, 0, 1024, pp::FltHPLevel, lvl},
                   {5, 0, 1, pp::FltSlope, [](double v) { return v == 1 ? 1.0 : 0.0; }}};
  applyTable(m, tab, t);
  set(m, pp::FltSlope, t[5] == 1 ? 1 : 0);
  set(m, pp::FltDcBlock, center ? 1 : 0);
  set(m, pp::FltCutoff, std::clamp(flref::filterCutHz(lvl(t[0]), 44100.0), 20.0, 20000.0));  // the modern model's view
  set(m, pp::FltBlend, 1);
  m.exact = true;
  bindTable(m, std::make_shared<const KnobTable>(tab));
  return true;
}

// --- WaveShaper → Waveshaper
bool decodeWaveShaper(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 34) return false;
  int ver = static_cast<int>(r.u16());
  r.u16();
  double pre = r.i32(), mix = r.i32(), post = r.i32();
  uint8_t bipolar = r.u8();
  r.u8(), r.u8();  // remove DC / smoothing flags
  r.skip(3);
  r.i32(), r.i32();
  uint32_t count = r.u32();
  struct Pt {
    double x, y;
  };
  std::vector<Pt> pts;
  double x = 0;
  for (uint32_t i = 0; i < count && i < 4096; ++i) {
    double dx, y;
    if (ver >= 3) {
      if (!r.has(24)) break;
      dx = r.f64(), y = r.f64();
      r.skip(8);
    } else {
      if (!r.has(12)) break;
      dx = r.f32(), y = r.f32();
      r.skip(4);
    }
    x += dx;
    pts.push_back({std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)});
  }
  if (pts.empty()) return false;
  auto curve = [&](double u) {  // piecewise linear through the points
    if (u <= pts.front().x) return pts.front().y;
    for (size_t i = 1; i < pts.size(); ++i)
      if (u <= pts[i].x) {
        double w = pts[i].x - pts[i - 1].x;
        return w <= 0 ? pts[i].y : pts[i - 1].y + (pts[i].y - pts[i - 1].y) * (u - pts[i - 1].x) / w;
      }
    return pts.back().y;
  };
  begin(m, "gl.waveshaper");
  m.exact = false;  // FL breakpoints are resampled onto Gridline's evenly spaced curve points
  const int n = pp::kShapePoints;
  set(m, pp::WsBipolar, bipolar ? 0 : 1);
  for (int k = 0; k < n; ++k) {
    double u = -1.0 + 2.0 * k / (n - 1);  // Gridline point position (input)
    double y;
    if (bipolar) {
      y = curve((u + 1.0) * 0.5) * 2.0 - 1.0;
    } else {
      y = u >= 0 ? curve(u) : -curve(-u);
    }
    set(m, k, std::clamp(y, -1.0, 1.0));
  }
  set(m, pp::WsPreGain, std::clamp(ratioDb(pre, 128, -12), -12.0, 36.0));
  set(m, pp::WsMix, std::clamp(mix / 128.0, 0.0, 1.0));
  set(m, pp::WsPostGain, std::clamp(ratioDb(post, 128, -36), -36.0, 12.0));
  set(m, pp::WsCenter, 0);
  set(m, pp::WsInterp, 0);
  auto table = std::make_shared<KnobTable>(KnobTable{
      {0, 64, 192, pp::WsPreGain, [](double v) { return std::clamp(ratioDb(v, 128, -12), -12.0, 36.0); }},
      {1, 0, 128, pp::WsMix, [](double v) { return v / 128.0; }},
      {2, 0, 128, pp::WsPostGain, [](double v) { return std::clamp(ratioDb(v, 128, -36), -36.0, 12.0); }}});
  bindTable(m, table);
  return true;
}

}  // namespace

void registerFxEq(std::map<std::string, PluginDecoder>& d) {
  d["Fruity Parametric EQ 2"] = decodeEq2;
  d["Fruity Balance"] = simpleDecoder(balance());
  d["Fruity Soft Clipper"] = simpleDecoder(softClipper());
  d["Fruity Fast Dist"] = simpleDecoder(fastDist());
  d["Fruity Stereo Shaper"] = [](Context& c, const std::vector<Event>& evs, PluginMapping& m) {
    static const PluginDecoder base = simpleDecoder(stereoShaper());
    if (!base(c, evs, m)) return false;
    const auto k = static_cast<size_t>(gl::packs::utility::SmDiffTo);
    const int n = k < m.state.params.size() ? static_cast<int>(m.state.params[k]) : 0;
    if (!flRouteOutputKept(c, n)) {
      m.exact = false;
      c.report.warn("Stereo Shaper: the difference output's route is not among the imported routes");
    }
    return true;
  };
  d["Fruity Stereo Enhancer"] = simpleDecoder(stereoEnhancer());
  d["Fruity Free Filter"] = decodeFreeFilter;
  d["Fruity Filter"] = decodeFruityFilter;
  d["Fruity WaveShaper"] = decodeWaveShaper;
}

}  // namespace gl::flp::fx

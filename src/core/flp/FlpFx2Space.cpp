// Gridline — FLP import, second wave: reverbs, convolver, delay, chorus and
// phasers.
//
// State layouts (ints = int32 in tag order):
//   LuxeVerb          u32 version (7..9), u32 sidechain input number, 5 bytes
//                     (pre-delay sync, feedback-delay sync, three display flags),
//                     then one int per tag (38 / 39 / 49 for v7 / v8 / v9; table
//                     in decodeLuxeVerb), v9: two feed-forward delay sync bytes.
//                     Older versions load with the limiter on (v<8) and default
//                     feed-forward settings (v<9).
//   Fruity Convolver  u32 version (11, 14, or 14/15 in the high word), dry,
//                     wet (0..1000), input / wet separation (±1000), u8 "file"
//                     flag, impulse path (u8 length or 0xFF + u32), embedded
//                     RIFF/WAVE when the flag is 0 (u32 size + data), f64 ×4
//                     editor selection / view, f32, five point envelopes (four
//                     time envelopes, then the EQ curve), 4 bytes (… [3] =
//                     normalize), v15: i32 latency, then i32 editor tab,
//                     pre-delay, self-convolution, stretch, [v ≥ 11] EQ mix.
//   Fruity Reeverb    version 1, low cut, high cut, pre-delay (0..250), room
//                     size, diffusion, colour (0..40), decay, high damping, dry,
//                     reverb (0..65536 unless noted).
//   Fruity Delay      version 1, input, feedback, cutoff, tempo (0..1024),
//                     steps (1..16), mode (0..1024).
//   Fruity Phaser     version 1, sweep frequency (0..5000), min depth, max depth
//                     (0..1000), frequency range, stereo (0..1024), stages
//                     (0..22), feedback (0..1000), dry-wet (0..1024), output
//                     gain (0..5000, 4000 = unity).
//   Vintage Chorus    version 1, then 17 ints in tag order (see decodeVintageChorus).
//   Vintage Phaser    version 1..3, then 32 / 36 ints (see decodeVintagePhaser).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include "../../packs/spacefx/sfx_flcurves.h"
#include "../../packs/spacefx/spacefx.h"
#include "../PluginParams.h"
#include "../../engine/ClassicDelay.h"
#include "../../engine/ClassicModFx.h"
#include "../../engine/PlateReverb.h"
#include "FlpFx2.h"

namespace gl::flp::fx2 {

namespace {

namespace pp = gl::pp;
namespace sx = gl::packs::spacefx;

// ------------------------------------------------------------------ LuxeVerb → Hall
// Knob curves: a control's raw value is normalized over its range (n) and
// shaped as lo + (hi - lo) · n^(1/c).
double luxeShape(double v, double rmin, double rmax, double lo, double hi, double c) {
  double n = clampd((v - rmin) / (rmax - rmin), 0, 1);
  return lo + (hi - lo) * std::pow(n, 1.0 / c);
}
// Input / dry / wet level slider (-600..1500) → dB: -60 + 75·√n (746 = 0 dB).
double luxeLevelDb(double v) { return luxeShape(v, -600, 1500, -60, 15, 2.0); }

bool decodeLuxeVerb(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  if (b.size() < 13 + 38 * 4) return false;
  Reader r(b.data(), b.size());
  const uint32_t version = r.u32();
  const int scIn = static_cast<int>(std::min<uint32_t>(r.u32(), 500));  // J-45: FL input route index of the hosting insert
  uint8_t hdr[5];  // pre-delay sync, feedback-delay sync, 3 display flags (UI only)
  for (uint8_t& h : hdr) h = r.u8();
  const size_t nTags = version <= 7 ? 38 : version == 8 ? 39 : 49;
  std::vector<double> t = ints(r, nTags);
  if (t.size() < 38) return false;
  uint8_t ffSync[2] = {0, 0};
  if (nTags == 49 && t.size() == 49 && r.has(2)) ffSync[0] = r.u8(), ffSync[1] = r.u8();
  // Load fix-ups for older versions (limiter on before v8; feed-forward defaults before v9).
  if (t.size() < 39) t.resize(39, 1);
  if (t.size() < 49) {
    static const double ffDef[10] = {0, 0, 24, 20, 1000, 20, 1000, 192, 1000, 192};
    for (size_t k = t.size(); k < 49; ++k) t.push_back(ffDef[k - 39]);
  }
  fx::begin(m, "gl.hall");
  auto n = [](double rmin, double rmax) { return [rmin, rmax](double v) { return clampd((v - rmin) / (rmax - rmin), 0, 1); }; };
  auto ms = [](double rmin, double rmax) { return [rmin, rmax](double v) { return 1.0 + 499.0 * clampd((v - rmin) / (rmax - rmin), 0, 1); }; };
  auto cut = [](double rmin, double rmax, double lo, double hi) {
    return [=](double v) { return luxeShape(v, rmin, rmax, lo, hi, 0.5); };
  };
  KnobTable tab = {
      {0, -600, 1500, sx::HvInGain, luxeLevelDb},
      {1, 40, 20000, sx::HvInHighCut, [](double v) { return luxeShape(v, 40, 20000, 40, 20000, 1.5); }},
      {2, 20, 5000, sx::HvInLowCut, cut(20, 5000, 20, 5000)},
      {3, 0, 1, sx::HvOn, nullptr},
      {4, 250, 20000, sx::HvDecay, [](double v) { return luxeShape(v, 250, 20000, 0.25, 20, 0.4); }},
      {5, 0, 5000, sx::HvPreDelay, cut(0, 5000, 0, 500)},
      {6, 1, 768, sx::HvPreTicks, nullptr},
      {7, 0, 1000, sx::HvBright, n(0, 1000)},
      {8, 0, 100, sx::HvCharacter, n(0, 100)},
      {9, 1, 100, sx::HvSize, n(1, 100)},
      {10, 0, 1000, sx::HvModFreq, n(0, 1000)},
      {11, 0, 100, sx::HvModDepth, n(0, 100)},
      {12, 0, 1000, sx::HvDiffusion, n(0, 1000)},
      {13, 0, 2, sx::HvFreeze, [](double v) { int k = static_cast<int>(std::lround(clampd(v, 0, 2))); return k == 2 ? double(sx::HfSustain) : double(k); }},
      {14, 0, 1, sx::HvHQ, nullptr},
      {15, 0, 1, sx::HvFbOn, nullptr},
      {16, -120, 120, sx::HvFbPitch, [](double v) { return (clampd((v + 120.0) / 240.0, 0, 1) - 0.5) * 24.0; }},
      {17, 10, 5000, sx::HvFbDelay, ms(10, 5000)},
      {18, 1, 768, sx::HvFbTicks, nullptr},
      {19, 0, 1000, sx::HvFbGain, n(0, 1000)},
      {20, 20, 5000, sx::HvFbHighCut, cut(20, 5000, 20, 5000)},
      {21, 20, 5000, sx::HvFbLowCut, cut(20, 5000, 20, 5000)},
      {22, 0, 1000, sx::HvFbMix, n(0, 1000)},
      {23, -1000, 1000, sx::HvEnvScale, [](double v) { return 2.0 * clampd((v + 1000.0) / 2000.0, 0, 1) - 1.0; }},
      {24, -6000, 0, sx::HvEnvThresh, [](double v) { return v * 0.01; }},
      {25, 50, 1000, sx::HvEnvAttack, nullptr},
      {26, 50, 1000, sx::HvEnvRelease, nullptr},
      {27, 0, 100, sx::HvEnvOffset, [](double v) { return clampd(v / 100.0, 0, 1) - 0.5; }},
      {28, 0, 2, sx::HvEnvRoute, nullptr},
      {29, 0, 1, sx::HvEnvSidechain, nullptr},
      {30, 20, 2000, sx::HvEnvLowCut, cut(20, 2000, 20, 2000)},
      {31, 10, 1000, sx::HvEnvSmooth, [](double v) { return 1.0 + 99.0 * clampd((v - 10.0) / 990.0, 0, 1); }},
      {32, -600, 1500, sx::HvDry, luxeLevelDb},
      {33, -600, 1500, sx::HvWet, luxeLevelDb},
      {34, 40, 10000, sx::HvEqFreq, cut(40, 10000, 40, 10000)},
      {35, -1800, 1800, sx::HvEqGain, [](double v) { return v * 0.01; }},
      {36, 25, 5000, sx::HvEqQ, [](double v) { return 0.25 + 4.75 * clampd((v - 25.0) / 4975.0, 0, 1); }},
      {37, 0, 200, sx::HvWidth, [](double v) { return v * 0.01; }},
      {38, 0, 1, sx::HvLimiter, nullptr},
      {39, 0, 1, sx::HvFbMode, nullptr},
      {40, 0, 1000, sx::HvFfDirect, n(0, 1000)},
      {41, -24, 24, sx::HvFfPitch1, nullptr},
      {42, -24, 24, sx::HvFfPitch2, nullptr},
      {43, 0, 1000, sx::HvFfAmp1, n(0, 1000)},
      {44, 0, 1000, sx::HvFfAmp2, n(0, 1000)},
      {45, 10, 5000, sx::HvFfDelay1, ms(10, 5000)},
      {46, 1, 768, sx::HvFfTicks1, nullptr},
      {47, 0, 1000, sx::HvFfDelay2, ms(0, 1000)},
      {48, 1, 768, sx::HvFfTicks2, nullptr},
  };
  t[43] = clampd(t[43], 0, 1000);  // v<9 default 1719 lies outside the control's range
  fx::applyTable(m, tab, t);
  fx::set(m, sx::HvModel, 1);
  fx::set(m, sx::HvPreSync, hdr[0] ? 1 : 0);
  fx::set(m, sx::HvFbSync, hdr[1] ? 1 : 0);
  fx::set(m, sx::HvFfSync1, ffSync[0] ? 1 : 0);
  fx::set(m, sx::HvFfSync2, ffSync[1] ? 1 : 0);
  normalizePluginState(m.state);
  // Every control maps by its exact law. J-45: the detector reads the hosting
  // track's input route n (resolved by the engine; 0 = its own input).
  fx::set(m, sx::HvScIn, scIn);
  m.exact = flRouteInputKept(c, scIn) && !(scIn > 0 && t[29] == 0);
  if (!m.exact) c.report.warn("LuxeVerb: the sidechain source is not among the imported routes");
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Convolver → Convolver
// Folder for impulse responses stored inside plugin states (written once, reused).
std::string irCacheDir() {
  namespace fs = std::filesystem;
  if (const char* e = std::getenv("GRIDLINE_CACHE_DIR"); e && *e) return std::string(e) + "/flp-ir";
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/gridline/flp-ir";
  if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/gridline/flp-ir";
  return (fs::temp_directory_path() / "gridline-flp-ir").string();
}

// Writes an embedded RIFF/WAVE to the cache, named by a content hash; "" on failure.
std::string storeEmbeddedIr(const uint8_t* p, size_t n) {
  namespace fs = std::filesystem;
  uint64_t h = 1469598103934665603ull;  // FNV-1a 64
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  char name[40];
  std::snprintf(name, sizeof name, "%016llx.wav", static_cast<unsigned long long>(h));
  std::string dir = irCacheDir(), path = dir + "/" + name;
  std::error_code ec;
  if (fs::is_regular_file(path, ec) && fs::file_size(path, ec) == n) return path;
  fs::create_directories(dir, ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return "";
  f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
  return f ? path : "";
}

// File name (lower case) → path of every impulse under FL's Data/Patches/Impulses.
const std::map<std::string, std::string>& impulseIndex(const std::string& factory) {
  namespace fs = std::filesystem;
  static std::mutex mu;
  static std::map<std::string, std::map<std::string, std::string>> cache;
  std::lock_guard<std::mutex> lk(mu);
  auto it = cache.find(factory);
  if (it != cache.end()) return it->second;
  std::map<std::string, std::string>& idx = cache[factory];
  std::error_code ec;
  fs::path root = fs::path(factory) / "Data" / "Patches" / "Impulses";
  if (!factory.empty() && fs::is_directory(root, ec))
    for (fs::recursive_directory_iterator d(root, ec), end; !ec && d != end; d.increment(ec)) {
      if (!d->is_regular_file(ec)) continue;
      std::string fn = d->path().filename().string();
      std::transform(fn.begin(), fn.end(), fn.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      idx.emplace(fn, d->path().string());
    }
  return idx;
}

// FL impulse path → local file: macros (%FLStudioData%, %FLStudioFactoryData%,
// %USERPROFILE%), then the file name under FL's impulse folders, then next to the project.
std::string resolveImpulse(Context& c, std::string p) {
  namespace fs = std::filesystem;
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  const std::string data = "%FLStudioData%", user = "%USERPROFILE%";
  if (p.rfind(data, 0) == 0) p = "%FLStudioFactoryData%/Data" + p.substr(data.size());
  if (p.rfind(user, 0) == 0) {
    const char* home = std::getenv("HOME");
    p = std::string(home ? home : "") + p.substr(user.size());
  }
  std::string r = c.resolvePath(p);
  std::error_code ec;
  if (!r.empty() && fs::exists(r, ec)) return r;
  std::string fn = fs::path(r).filename().string();
  std::string key = fn;
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  const auto& idx = impulseIndex(c.factoryData);
  if (auto it = idx.find(key); it != idx.end()) return it->second;
  if (!c.projectDir.empty() && !fn.empty()) {
    std::string alt = c.projectDir + "/" + fn;
    if (fs::exists(alt, ec)) return alt;
  }
  return r;
}

// One point envelope of the IR editor: u32 kind, u32 format, u32 count,
// count × (f64 Δposition, f64 value, f32 tension, 4 flag bytes), 20 bytes of markers.
bool readConvEnvelope(Reader& r, std::vector<sx::EnvPoint>& out) {
  if (!r.has(12)) return false;
  r.u32();
  r.u32();
  uint32_t n = r.u32();
  if (n > 100000 || !r.has(static_cast<size_t>(n) * 24 + 20)) return false;
  double t = 0;
  out.clear();
  for (uint32_t i = 0; i < n; ++i) {
    double dt = r.f64(), v = r.f64();
    float k = r.f32();
    const uint8_t kind = r.u8();
    r.skip(3);
    t += std::isfinite(dt) ? std::max(0.0, dt) : 0.0;
    sx::EnvPoint q;
    q.t = t;
    q.v = clampd(std::isfinite(v) ? v : 0.5, 0, 1);
    q.k = clampd(std::isfinite(k) ? k : 0.0, -1, 1);
    q.c = kind <= 12 ? kind : 0;
    out.push_back(q);
  }
  r.skip(20);
  return true;
}
bool flatEnvelope(const std::vector<sx::EnvPoint>& e) {
  for (const sx::EnvPoint& q : e)
    if (std::fabs(q.v - 0.5) > 1e-6) return false;
  return true;
}

bool decodeConvolver(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  if (b.size() < 22) return false;
  Reader r(b.data(), b.size());
  uint32_t version = r.u32();
  if (version >= 0x10000) version >>= 16;
  std::vector<double> t = ints(r, 4);
  if (t.size() < 4) return false;
  fx::begin(m, "gl.convolver");
  KnobTable tab = {{0, 0, 1000, sx::CvDry, [](double v) { return sx::convDryGain(clampd(v / 1000.0, 0, 1)); }},
                   {1, 0, 1000, sx::CvWet, [](double v) { return sx::convWetGain(clampd(v / 1000.0, 0, 1)); }},
                   {2, -1000, 1000, sx::CvInWidth, [](double v) { return clampd(1.0 - v / 1000.0, 0, 2); }},
                   {3, -1000, 1000, sx::CvWidth, [](double v) { return clampd(1.0 - v / 1000.0, 0, 2); }}};
  fx::applyTable(m, tab, t);
  // FL plays the impulse as loaded: no Gridline trims, fades or tone shaping.
  fx::set(m, sx::CvStart, 0);
  fx::set(m, sx::CvFadeIn, 0);
  fx::set(m, sx::CvFadeOut, 0);
  fx::set(m, sx::CvTighten, 0);
  fx::set(m, sx::CvLength, 60);
  fx::set(m, sx::CvReverse, 0);
  fx::set(m, sx::CvStereo, 0);
  fx::set(m, sx::CvNormalize, 1);
  fx::set(m, sx::CvSource, sx::SrcFile);
  const bool isFile = r.u8() != 0;
  size_t len = r.u8();
  if (len == 0xFF) len = r.u32();
  std::string path;
  if (r.has(len)) path = latin1(b.data() + r.pos, len), r.skip(len);
  sx::ConvData cd;
  bool resolved = false;
  if (isFile) {
    if (!path.empty()) {
      cd.path = resolveImpulse(c, path);
      std::error_code ec;
      resolved = std::filesystem::exists(cd.path, ec);
    }
  } else if (r.has(4)) {
    uint32_t size = r.u32();
    if (r.has(size) && size >= 12 && std::memcmp(b.data() + r.pos, "RIFF", 4) == 0) {
      cd.path = storeEmbeddedIr(b.data() + r.pos, size);
      resolved = !cd.path.empty();
    }
    r.skip(size);
  }
  if (!resolved) c.report.warn("Fruity Convolver: impulse response not found (" + (path.empty() ? std::string("embedded") : path) + ")");
  // Editor selection / view (UI only), one float, then the five envelopes.
  r.skip(4 * 8 + 4);
  std::vector<sx::EnvPoint> env[5];
  bool envOk = true;
  for (auto& e : env) envOk = envOk && readConvEnvelope(r, e);
  bool kindsOk = true;
  if (envOk) {
    // J-42 reference build: 0 pan, 1 volume, 2 stereo separation (3 = scratch, not used by
    // the Convolver), 4 = EQ curve; neutral (all 0.5) envelopes are left out.
    if (!flatEnvelope(env[0])) cd.pan = env[0];
    if (!flatEnvelope(env[1])) cd.env = env[1];
    if (!flatEnvelope(env[2])) cd.sep = env[2];
    if (!flatEnvelope(env[4])) cd.eq = env[4];
    fx::set(m, sx::CvEnvOn, cd.env.empty() && cd.pan.empty() && cd.sep.empty() ? 0 : 1);
    for (int k : {0, 1, 2, 4})
      for (const sx::EnvPoint& q : env[k]) kindsOk = kindsOk && q.c <= 12;
  }
  fx::set(m, sx::CvModel, 1);
  double eqMix = 1;
  if (envOk && r.has(4)) {
    uint8_t flags[4];
    for (uint8_t& f : flags) f = r.u8();
    fx::set(m, sx::CvNormalize, flags[3] ? 1 : 0);
    if (version >= 15) fx::set(m, sx::CvLatency, clampd(r.i32(), 0, 65536));
    std::vector<double> u = ints(r, version >= 11 ? 5 : 4);
    if (u.size() >= 4) {
      fx::set(m, sx::CvPreDelay, sx::convPreDelayMs(clampd(u[1] / 1000.0, 0, 1)));
      fx::set(m, sx::CvSelfConv, clampd(u[2] / 1000.0, 0, 1));
      fx::set(m, sx::CvStretch, sx::convStretch(clampd(u[3] / 1000.0, -1, 1)));
    }
    if (u.size() >= 5) eqMix = clampd(u[4] / 1000.0, 0, 1);
    fx::set(m, sx::CvEqMix, eqMix);
  }
  m.state.data = sx::formatConvData(cd);
  normalizePluginState(m.state);
  // J-42: the reference build reproduces normalise, envelopes, self-convolution,
  // stretch, EQ curve and pre-delay (spec j35_convolver.md).
  m.exact = resolved && envOk && kindsOk;
  (void)eqMix;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Fruity Reeverb → Reverb
// J-26: plate reference (gl.reverb RvbModel = 1), laws in analysis/demos/specs/j26_reeverb1.md.
// FL ignores states whose first word is above version 1 (the 40-byte float
// "send" preset), so those load the plugin defaults.
double rv1LowCut(double v) { return plate::lowCutStep(v); }  // <= 20 = off in the plate
double rv1Cut100(double v) { return plate::cutStep(v) * 100.0; }  // >= 22100 = off
double rv1Size(double v) { return plate::sizeStep(v) / 100.0; }
double rv1Bass(double v) { return plate::kColourBass[plate::colourStep(v)]; }

bool decodeReeverb1(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 4) return false;
  std::vector<double> t = {1250, 10800, 0, 26000, 65536, 20, 4700, 10800, 65536, 13120};  // plugin defaults
  const uint32_t version = r.u32();
  if (version <= 1) {
    if (r.n < 44) return false;
    t = ints(r, 10);
    if (t.size() < 10) return false;
  }
  fx::begin(m, "gl.reverb");
  fx::set(m, pp::RvbModel, 1);
  auto w = [](double v) { return clampd(v / 65536.0, 0, 1); };
  KnobTable tab = {
      {0, 0, 65536, pp::RvbLowCut, [](double v) { return clampd(rv1LowCut(v), 20, 3000); }},
      {1, 0, 65536, pp::RvbHighCut, rv1Cut100},
      {2, 0, 250, pp::RvbPreDelay, [](double v) { return clampd(std::round(v), 0, 250); }},
      {3, 0, 65536, pp::RvbSize, rv1Size},
      {4, 0, 65536, pp::RvbDiffusion, [](double v) { return plate::diffusionStep(v); }},
      {5, 0, 40, pp::RvbBassMult, rv1Bass},
      {6, 0, 65536, pp::RvbDecayTime, [](double v) { return plate::decayStep(v); }},
      {7, 0, 65536, pp::RvbDampHz, rv1Cut100},
      {8, 0, 65536, pp::RvbDry, w},
      {9, 0, 65536, pp::RvbWet, w},
  };
  fx::applyTable(m, tab, t);
  fx::set(m, pp::RvbCrossover, 500);
  fx::set(m, pp::RvbWidth, 1);
  fx::set(m, pp::RvbEarly, 0);
  m.exact = rv1LowCut(t[0]) != 20;  // a 20 Hz low cut and "off" share one value in Gridline
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Fruity Delay → Delay
// J-26: classic tempo delay (DlyModel = 2, spec j26_delay1.md): input gain n,
// dry unity, feedback 0.99 n, coefficient (21^n - 1)/20 (not rate-scaled),
// own tempo round(220 n + 60) (0 = host), steps of a sixteenth, three modes.
bool decodeDelay1(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 28) return false;
  r.i32();
  std::vector<double> t = ints(r, 6);
  if (t.size() < 6) return false;
  fx::begin(m, "gl.delay");
  fx::set(m, pp::DlyModel, 2);
  fx::set(m, pp::DlyClsStepRound, 1);
  fx::set(m, pp::DlyClsScaleSr, 0);
  fx::set(m, pp::DlyClsDry, 1);
  fx::set(m, pp::DlyClsPan, 0);
  auto n = [](double v) { return clampd(v / 1024.0, 0, 1); };
  KnobTable tab = {{0, 0, 1024, pp::DlyClsIn, n},
                   {1, 0, 1024, pp::DlyClsFb, [n](double v) { return 0.99 * n(v); }},
                   {2, 0, 1024, pp::DlyClsCoef, [n](double v) { return cdelay::coefOf(n(v)); }},
                   {3, 0, 1024, pp::DlyClsBpm, [n](double v) { return v <= 0 ? 0.0 : std::nearbyint(n(v) * 220 + 60); }},
                   {4, 1, 16, pp::DlyTicks, [](double v) { return 48.0 * clampd(std::nearbyint(v), 1, 16); }},
                   {5, 0, 1024, pp::DlyClsMode, [n](double v) { return clampd(std::nearbyint(n(v) * 3 - 0.5), 0, 2); }}};
  fx::applyTable(m, tab, t);
  m.exact = true;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Fruity Phaser → Phaser
// J-26: reference engine (PhsModel = 1, spec j26_phaser.md): every control as
// its knob position n = raw / max (stages: the raw count + 1, 3..23).
bool decodePhaser1(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Reader r(pluginState(evs));
  if (r.n < 40) return false;
  r.i32();
  std::vector<double> t = ints(r, 9);
  if (t.size() < 9) return false;
  fx::begin(m, "gl.phaser");
  fx::set(m, pp::PhsModel, 1);
  auto n = [](double mx) { return [mx](double v) { return clampd(v / mx, 0, 1); }; };
  KnobTable tab = {
      {0, 0, 5000, pp::PhsSweepN, n(5000)},
      {1, 0, 1000, pp::PhsDepthMin, n(1000)},
      {2, 0, 1000, pp::PhsDepthMax, n(1000)},
      {3, 0, 1024, pp::PhsRangeLarge, [](double v) { return v / 1024.0 > 0.5 ? 1.0 : 0.0; }},
      {4, 0, 1024, pp::PhsStereoN, n(1024)},
      {5, 0, 22, pp::PhsStagesN, [](double v) { return static_cast<double>(classic::phaserStages(v)); }},
      {6, 0, 1000, pp::PhsFeedbackN, n(1000)},
      {7, 0, 1024, pp::PhsDryN, n(1024)},
      {8, 0, 5000, pp::PhsGainN, n(5000)},
  };
  fx::applyTable(m, tab, t);
  m.exact = true;
  bindKnobs(m, tab);
  return true;
}

// ------------------------------------------------------------------ Vintage Chorus → Bucket Chorus
// State: version 1, then 17 ints in tag order: mode (0 I, 1 II, 2 custom),
// I+II link, speed (1..245760), LFO type (0 triangle, 1 sine), L/R phase,
// time 1, time 2, mix, mono input, invert wet, feedback, high pass, gain,
// noise, noise gate, tempo sync, start phase (0..6000 unless noted). The
// plugin sends each control's clamped linear position n to its engine;
// curves: sfx_flcurves.h (bc*). Imports run the reference model (exact).
double vcN(double v) { return clampd(v / 6000.0, 0, 1); }
double vcSpeedN(double v) { return clampd((v - 1.0) / 245759.0, 0, 1); }

std::shared_ptr<const KnobTable> vintageChorusTable() {
  namespace fc = gl::packs::spacefx::flc;
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {2, 1, 245760, sx::BcRate, [](double v) { return fc::bcSpeedHz(vcSpeedN(v)); }},
      {4, 0, 6000, sx::BcPhaseLR, vcN},
      {5, 0, 6000, sx::BcTime1, [](double v) { return fc::bcTimeMs(vcN(v)); }},
      {6, 0, 6000, sx::BcTime2, [](double v) { return fc::bcTimeMs(vcN(v)); }},
      {7, 0, 6000, sx::BcMix, vcN},
      {10, 0, 6000, sx::BcFeedback, [](double v) { return fc::bcFeedback(vcN(v)); }},
      {11, 0, 6000, sx::BcHighPass, [](double v) { return fc::bcHighPassHz(vcN(v)); }},
      {12, 0, 6000, sx::BcGain, [](double v) { return fc::bcGainDb(vcN(v)); }},
      {13, 0, 6000, sx::BcHiss, vcN},
      {16, 0, 6000, sx::BcStartPhase, vcN},
  });
  return t;
}

bool decodeVintageChorus(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  namespace fc = gl::packs::spacefx::flc;
  Reader r(pluginState(evs));
  if (r.n < 4 + 17 * 4) return false;
  if (r.i32() >= 2) return false;  // the plugin loads version < 2 only
  std::vector<double> t = ints(r, 17);
  if (t.size() < 17) return false;
  fx::begin(m, "gl.bbdchorus");
  auto on = [](double v) { return v >= 1 ? 1.0 : 0.0; };  // switches: n = clamp(v, 0, 1) ≥ 0.5
  const auto tab = vintageChorusTable();
  fx::applyTable(m, *tab, t);
  fx::set(m, sx::BcModel, 1);
  const int model = static_cast<int>(std::floor(3.0 * 0.9999 * clampd(t[0] / 2.0, 0, 1)));  // 0 I, 1 II, 2 custom
  fx::set(m, sx::BcMode, model == 2 ? 3 : (t[1] >= 1 ? 2 : model));
  fx::set(m, sx::BcWave, t[3] >= 1 ? 0 : 1);  // FL 0 = triangle, 1 = sine
  fx::set(m, sx::BcMonoIn, on(t[8]));
  fx::set(m, sx::BcInvertWet, on(t[9]));
  fx::set(m, sx::BcNoiseGate, on(t[14]));
  fx::set(m, sx::BcSync, on(t[15]));
  fx::set(m, sx::BcSyncTicks, fc::bcSyncTicks(vcSpeedN(t[2])));
  m.exact = true;  // every control by its exact law (hiss colour / gate timing are the model's guesses)
  fx::bindTable(m, tab);
  return true;
}

// ------------------------------------------------------------------ Vintage Phaser → Retro Phaser
// State: version, then the controls in tag order — version 3: 36 ints + a
// UI byte; version 2: 36 ints; version 1: 32 ints in an older order
// (remapped below). Versions < 3 have no engine / drive / inverted-feedback
// controls. Every control sends its clamped linear position n (raw ranges
// below) to the engine; curves: sfx_flcurves.h (vp*). Imports run the
// reference model.
struct VpRange {
  double lo, hi;
};
constexpr VpRange kVpRange[36] = {
    {125, 2048000}, {125, 2048000}, {0, 6000}, {0, 1},    {125, 2048000}, {0, 1},     {1, 245760}, {0, 1},    {0, 1},
    {0, 6000},      {0, 6000},      {0, 6000}, {0, 1},    {0, 1},         {0, 6000},  {0, 6000},   {1, 768},  {0, 1},
    {0, 6000},      {3200, 204800}, {2000, 204800}, {0, 100}, {0, 1},     {0, 6000},  {0, 1},      {0, 6000}, {5000, 650000},
    {2000, 80000},  {2000, 80000},  {25, 150000}, {25, 150000}, {0, 6000}, {0, 6000}, {0, 1},      {5000, 650000}, {0, 1}};
double vpN(int tag, double v) { return clampd((v - kVpRange[tag].lo) / (kVpRange[tag].hi - kVpRange[tag].lo), 0, 1); }

std::shared_ptr<const KnobTable> vintagePhaserTable() {
  namespace fc = gl::packs::spacefx::flc;
  auto N = [](int tag) { return [tag](double v) { return vpN(tag, v); }; };
  static const auto t = std::make_shared<const KnobTable>(KnobTable{
      {0, 125, 2048000, sx::VpFreqMin, [](double v) { return fc::vpFreqHz(vpN(0, v)); }},
      {1, 125, 2048000, sx::VpFreqMax, [](double v) { return fc::vpFreqHz(vpN(1, v)); }},
      {2, 0, 6000, sx::VpFeedback, [](double v) { return 0.9 * vpN(2, v); }},
      {4, 125, 2048000, sx::VpManualFreq, [](double v) { return fc::vpFreqHz(vpN(4, v)); }},
      {9, 0, 6000, sx::VpTension, N(9)},
      {10, 0, 6000, sx::VpStartPhase, N(10)},
      {11, 0, 6000, sx::VpPhaseLR, N(11)},
      {14, 0, 6000, sx::VpDelayMix, N(14)},
      {15, 0, 6000, sx::VpDelayStereo, N(15)},
      {16, 1, 768, sx::VpDelayTime, [](double v) { return 1000.0 * vpN(16, v); }},
      {18, 0, 6000, sx::VpDelayFb, N(18)},
      {19, 3200, 204800, sx::VpDelayLp, [](double v) { return fc::vpEchoLpHz(vpN(19, v)); }},
      {20, 2000, 204800, sx::VpDelayHp, [](double v) { return fc::vpEchoHpHz(vpN(20, v)); }},
      {21, 0, 100, sx::VpMix, N(21)},
      {23, 0, 6000, sx::VpNoise, N(23)},
      {25, 0, 6000, sx::VpColorGain, [](double v) { return 30.0 * vpN(25, v); }},
      {26, 5000, 650000, sx::VpColorFreq, [](double v) { return 600.0 * vpN(26, v) + 50.0; }},
      {27, 2000, 80000, sx::VpInputHp, [](double v) { return fc::vpHpHz(vpN(27, v)); }},
      {28, 2000, 80000, sx::VpFbHp, [](double v) { return fc::vpHpHz(vpN(28, v)); }},
      {29, 25, 150000, sx::VpRateLo, [](double v) { return fc::vpRateRangeHz(vpN(29, v)); }},
      {30, 25, 150000, sx::VpRateHi, [](double v) { return fc::vpRateRangeHz(vpN(30, v)); }},
      {31, 0, 6000, sx::VpDepthScale, N(31)},
      {32, 0, 6000, sx::VpDepthPull, N(32)},
      {34, 5000, 650000, sx::VpDrive, [](double v) { return 30.0 * vpN(34, v) - 15.0; }},
  });
  return t;
}

bool decodeVintagePhaser(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  namespace fc = gl::packs::spacefx::flc;
  Reader r(pluginState(evs));
  if (r.n < 4 + 32 * 4) return false;
  const int version = r.i32();
  if (version < 1 || version > 3) return false;
  std::vector<double> t(36, 0.0);
  if (version == 1) {
    static const int remap[32] = {0, 1, 2, 3, 4, 6, 8, 11, 7, 10, 21, 22, 23, 5, 12, 13, 14, 15, 16, 17, 18, 19, 20, 25, 26, 27, 28, 29, 30, 31, 32, 9};
    std::vector<double> v = ints(r, 32);
    if (v.size() < 32) return false;
    for (int i = 0; i < 32; ++i) t[static_cast<size_t>(remap[i])] = v[static_cast<size_t>(i)];
    t[24] = 1;  // noise gate: the control's default
  } else {
    std::vector<double> v = ints(r, 36);
    if (v.size() < 36) return false;
    t = v;
  }
  if (version < 3) t[33] = t[34] = t[35] = 0;
  fx::begin(m, "gl.vphaser");
  const auto tab = vintagePhaserTable();
  fx::applyTable(m, *tab, t);
  auto on = [](double v) { return v >= 1 ? 1.0 : 0.0; };
  fx::set(m, sx::VpModel, 1);
  fx::set(m, sx::VpStages, 0);
  fx::set(m, sx::VpInvertFb, on(t[35]));
  fx::set(m, sx::VpManual, on(t[3]));
  fx::set(m, sx::VpHq, on(t[5]));
  // Speed: position n between the (sorted) rate-range ends.
  double lo = fc::vpRateRangeHz(vpN(29, t[29])), hi = fc::vpRateRangeHz(vpN(30, t[30]));
  if (lo > hi) std::swap(lo, hi);
  const double n6 = vpN(6, t[6]);
  fx::set(m, sx::VpRateLo, lo);
  fx::set(m, sx::VpRateHi, hi);
  fx::set(m, sx::VpRate, lo * std::pow(hi / lo, n6));
  fx::set(m, sx::VpSync, on(t[7]));
  fx::set(m, sx::VpSyncTicks, fc::vpSyncTicks(n6));
  fx::set(m, sx::VpWave, t[8] >= 1 ? 0 : 1);  // FL 0 = triangle, 1 = sine
  fx::set(m, sx::VpDelayOn, on(t[12]));
  fx::set(m, sx::VpDelayKeep, on(t[13]));
  fx::set(m, sx::VpDelaySync, on(t[17]));
  fx::set(m, sx::VpDelayTicks, std::floor(767.0 * vpN(16, t[16]) + 1e-6) + 1.0);
  fx::set(m, sx::VpMonoIn, on(t[22]));
  fx::set(m, sx::VpNoiseGate, on(t[24]));
  fx::set(m, sx::VpEngine, on(t[33]));
  // Out-of-range sweep ends (sorted by the engine) stay inside Gridline's range.
  m.exact = true;
  fx::bindTable(m, tab);
  return true;
}

}  // namespace

void registerFx2Space(std::map<std::string, PluginDecoder>& d) {
  d["LuxeVerb"] = decodeLuxeVerb;
  d["Fruity Convolver"] = decodeConvolver;
  d["Fruity Reeverb"] = decodeReeverb1;
  d["Fruity Delay"] = decodeDelay1;
  d["Fruity Phaser"] = decodePhaser1;
  d["Vintage Chorus"] = decodeVintageChorus;
  d["Vintage Phaser"] = decodeVintagePhaser;
}

}  // namespace gl::flp::fx2

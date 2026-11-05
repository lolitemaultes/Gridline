// Gridline: K-62, the Convolver against FL's state and engine. Every impulse here is synthetic.
// States decoded from FL carry a marker ("#fit") and follow FL's full law; a state without it (Gridline's
// own curves, projects saved before K-62) keeps its sound.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "../src/core/Automation.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpEncode.h"
#include "../src/core/flp/FlpFx2.h"
#include "../src/engine/Effects.h"
#include "../src/engine/Export.h"
#include "../src/engine/Sample.h"
#include "../src/engine/SlicexEngine.h"
#include "../src/packs/Packs.h"
#include "../src/packs/spacefx/sfx_convolver.h"
#include "../src/packs/spacefx/spacefx.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;
namespace sx = gl::packs::spacefx;
namespace det = gl::packs::spacefx::detail;
namespace rf = gl::packs::spacefx::detail::ref;

namespace {

void expect(bool ok, const std::string& name) { ::expect(ok, name.c_str()); }
bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

fs::path scratch() {
  const fs::path p = fs::temp_directory_path() / ("gl_k62_" + std::to_string(static_cast<long>(std::hash<std::string>{}(fs::current_path().string()) % 100000)));
  fs::create_directories(p);
  return p;
}

// ---------------------------------------------------------- a Fruity Convolver state, built by hand
struct Blob {
  std::vector<uint8_t> b;
  Blob& u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Blob& i32(int32_t v) { return u32(static_cast<uint32_t>(v)); }
  Blob& u8(uint8_t v) {
    b.push_back(v);
    return *this;
  }
  Blob& f32(float v) {
    uint32_t x;
    std::memcpy(&x, &v, 4);
    return u32(x);
  }
  Blob& f64(double v) {
    uint64_t x;
    std::memcpy(&x, &v, 8);
    u32(static_cast<uint32_t>(x));
    return u32(static_cast<uint32_t>(x >> 32));
  }
};

struct Pt {
  double t, v;
};

void envelope(Blob& s, uint32_t kind, const std::vector<Pt>& pts) {
  s.u32(kind).u32(3).u32(static_cast<uint32_t>(pts.size()));
  double prev = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    s.f64(pts[i].t - prev).f64(pts[i].v).f32(0.f);
    prev = pts[i].t;
    s.u8(0).u8(0).u8(0).u8(i + 1 == pts.size() ? 2 : 0);
  }
  s.u32(0).i32(-1).i32(-1).i32(-1).i32(-1);
}

struct FlState {
  std::string path;
  int dry = 0, wet = 1000, inSep = 0, wetSep = 0;
  int pre = 0, self = 0, stretch = 0, eqMix = 1000;
  bool normalize = false;
  std::vector<Pt> pan = {{0, 0.5}, {1, 0.5}}, vol = {{0, 0.5}, {1, 0.5}}, sep = {{0, 0.5}, {1, 0.5}};
};

std::vector<uint8_t> flState(const FlState& f) {
  Blob s;
  s.u32((15u << 16) | 3u).i32(f.dry).i32(f.wet).i32(f.inSep).i32(f.wetSep).u8(1).u8(static_cast<uint8_t>(f.path.size()));
  for (char c : f.path) s.u8(static_cast<uint8_t>(c));
  s.f64(-1).f64(-1).f64(0).f64(1).f32(0.02f);
  envelope(s, 6, f.pan);
  envelope(s, 6, f.vol);
  envelope(s, 6, f.sep);
  envelope(s, 6, {{0, 0.5}, {1, 0.5}});
  envelope(s, 3, {{0, 0.5}, {1, 0.5}});
  s.u8(0).u8(0).u8(1).u8(f.normalize ? 1 : 0).i32(0);
  s.i32(0).i32(f.pre).i32(f.self).i32(f.stretch).i32(f.eqMix);
  return s.b;
}

bool decode(const std::vector<uint8_t>& st, flp::PluginMapping& m) {
  flp::File file;
  Song song;
  flp::Report rep;
  flp::Context c(file, song, rep);
  return flp::fx2::decodeEmbedded(c, "Fruity Convolver", st.data(), st.size(), m);
}

float P(const PluginState& s, int i) { return s.params[static_cast<size_t>(i)]; }

// One impulse of `amp` at frame `at`, stereo.
std::string impulseFile(const std::string& name, size_t frames, size_t at, float amp, int rate = 44100) {
  AudioBuffer b(2, frames);
  b.at(0, at) = b.at(1, at) = amp;
  const std::string p = (scratch() / name).string();
  writeWavFile(p, b, rate, 32);
  sampleCache().clear();
  return p;
}

std::vector<float> renderL(const PluginState& st, double sr, std::vector<float> in, int block = 256) {
  std::vector<float> r = in;
  auto fx = createEffect(st.type);
  fx->prepareHost(st, sr);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 120;
  float z0 = 0, z1 = 0;
  fx->processAux(st, aux, ctx, &z0, &z1, 0);
  for (size_t i = 0; i < in.size(); i += static_cast<size_t>(block)) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.size() - i));
    fx->processAux(st, aux, ctx, in.data() + i, r.data() + i, n);
  }
  return in;
}

float peakOf(const std::vector<float>& v) {
  float p = 0;
  for (float x : v) p = std::max(p, std::fabs(x));
  return p;
}

// ---------------------------------------------------------------------------------- envelopes
void testEnvelopeSpread() {
  det::IrBuf ir;
  ir.rate = 1000;
  ir.paths = 2;
  ir.p[0].assign(1000, 1.f);
  ir.p[1].assign(1000, 1.f);
  det::IrBuf own = ir;
  sx::ConvData cd;
  sx::EnvPoint a, b, c;
  a.t = 0, a.v = 0;
  b.t = 4, b.v = 0.5;  // the middle of a curve 8 long
  c.t = 8, c.v = 1;
  cd.env = {a, b, c};
  cd.envFit = true;
  rf::envelopes(ir, cd);
  expect(near(ir.p[0][499], 1.0, 1e-5), "a volume curve from FL spreads over the whole source: its middle falls on the source's middle");
  expect(near(ir.p[0][0], 2.0 * 0.001, 1e-5), "FL steps the curve before the first frame");
  cd.envFit = false;
  rf::envelopes(own, cd);
  expect(near(own.p[0][500], 2.0 * 0.0625, 1e-5), "a curve without the marker keeps seconds of the source");
  expect(near(own.p[0][0], 0.0, 1e-6), "a curve without the marker starts at its first point");

  // Each envelope has its own span.
  det::IrBuf both;
  both.rate = 1000;
  both.paths = 2;
  both.p[0].assign(1000, 1.f);
  both.p[1].assign(1000, 1.f);
  sx::ConvData two;
  two.envFit = true;
  sx::EnvPoint p0, p1, p2;
  p0.t = 0, p0.v = 0.5;
  p1.t = 1, p1.v = 1;  // hard right at the middle of a curve 2 long
  p2.t = 2, p2.v = 0.5;
  two.pan = {p0, p1, p2};
  sx::EnvPoint v0, v1;
  v0.t = 0, v0.v = 0.5;
  v1.t = 20, v1.v = 0.5;  // flat, 20 long: says nothing about the pan curve's span
  two.env = {v0, v1};
  rf::envelopes(both, two);
  expect(near(both.p[0][499], 0.0, 1e-3) && both.p[1][499] > 1.2f, "a pan curve spreads by its own length, not the volume curve's");
}

void testMarkerText() {
  sx::ConvData d;
  d.path = "/x/y.wav";
  expect(sx::formatConvData(d) == "/x/y.wav", "a state without the marker keeps its old text");
  expect(!sx::parseConvData("/x/y.wav").envFit, "old text parses without the marker");
  d.envFit = true;
  const sx::ConvData back = sx::parseConvData(sx::formatConvData(d));
  expect(back.envFit && back.path == d.path, "the marker survives a save");
}

// --------------------------------------------------------------------------------- the decoder
void testDecoder() {
  FlState f;
  f.path = "/nowhere.wav";
  f.inSep = 500;
  f.wetSep = -500;
  flp::PluginMapping m;
  expect(decode(flState(f), m), "a state decodes");
  expect(near(P(m.state, sx::CvInWidth), 1.0 - (std::pow(5.0, 0.5) - 1.0) * 0.25, 1e-5), "separation above centre: cross mix (5^x - 1) / 4");
  expect(near(P(m.state, sx::CvWidth), 1.5, 1e-5), "separation below centre: cross mix = the knob");
  expect(sx::parseConvData(m.state.data).envFit, "a decoded state carries the marker");
  f.inSep = 1000;
  flp::PluginMapping full;
  decode(flState(f), full);
  expect(near(P(full.state, sx::CvInWidth), 0.0, 1e-5), "separation at the top: mono");
}

// ------------------------------------------------------------------------- the impulse's region
void appendRegions(const std::string& path, uint32_t start, uint32_t length) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  slicex::Region r;
  r.start = start;
  r.length = length > 0 ? static_cast<int64_t>(length) : -1;
  const std::vector<uint8_t> o = slicex::writeWavRegions(b, {r});
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
}

void testRegion() {
  const std::string p = impulseFile("region.wav", 4000, 1500, 0.5f);
  appendRegions(p, 1000, 2000);
  size_t from = 0, to = 0;
  expect(rf::firstRegion(p, 4000, from, to) && from == 1000 && to == 3000, "the first region of an impulse file is read");
  PluginState s = defaultPluginState("gl.convolver");
  s.params[sx::CvSource] = sx::SrcFile;
  s.params[sx::CvModel] = 1;
  s.params[sx::CvNormalize] = 0;
  sx::ConvData cd;
  cd.path = p;
  cd.envFit = true;
  s.data = sx::formatConvData(cd);
  det::IrBuf ir;
  ir.rate = 44100;
  ir.paths = 2;
  ir.p[0].assign(4000, 0.f);
  ir.p[1].assign(4000, 0.f);
  ir.p[0][1500] = ir.p[1][1500] = 0.5f;
  rf::buildSource(ir, s);
  expect(ir.len() == 2000 && near(ir.p[0][500], 0.5, 1e-6), "a state from FL plays the file's first region");
  cd.envFit = false;
  s.data = sx::formatConvData(cd);
  det::IrBuf whole;
  whole.rate = 44100;
  whole.paths = 2;
  whole.p[0].assign(4000, 0.f);
  whole.p[1].assign(4000, 0.f);
  rf::buildSource(whole, s);
  expect(whole.len() == 4000, "a state without the marker plays the whole file");
}

// ------------------------------------------------------------------------- the impulse's conversion
double taps(double x, double cut) {
  const double pi = 3.14159265358979323846;
  const double c = std::cos(pi * x / 256.0);
  const double si = std::fabs(x) < 1e-9 ? cut : std::sin(pi * x * cut) / (pi * x);
  return std::fabs(x) >= 256.0 ? 0.0 : si * (0.5 + 0.5 * c) * (0.36 + 0.64 * c);
}

void testConversion() {
  std::vector<float> in(6000, 0.f);
  in[3000] = 1.f;
  const std::vector<float> up = rf::irConvert(in, 44100, 48000);
  expect(up.size() == 6531, "the converted length is frames x ratio, rounded");
  double worst = 0;
  for (size_t i = 0; i < up.size(); ++i) worst = std::max(worst, std::fabs(up[i] - taps(static_cast<double>(i) * 44100.0 / 48000.0 - 3000.0, 1.0)));
  expect(worst < 2e-6, "upward conversion: 256 half taps of a sinc under the window");
  const std::vector<float> down = rf::irConvert(in, 44100, 22050);
  worst = 0;
  for (size_t i = 0; i < down.size(); ++i) worst = std::max(worst, std::fabs(down[i] - taps(static_cast<double>(i) * 2.0 - 3000.0, 0.5)));
  expect(worst < 2e-6, "downward conversion: the sinc cuts at the lower rate");
  // The table of phases and the direct way agree.
  const std::vector<float> off = rf::irConvert(in, 44100.0, 48000.0 + 1e-7);
  worst = 0;
  for (size_t i = 0; i < std::min(up.size(), off.size()); ++i) worst = std::max(worst, static_cast<double>(std::fabs(up[i] - off[i])));
  expect(worst < 1e-4, "whole-number rates through a table match the direct way");
  std::vector<float> dc(8000, 1.f);
  const std::vector<float> dcOut = rf::irConvert(dc, 96000, 44100);
  expect(near(dcOut[dcOut.size() / 2], 1.0, 1e-3), "a steady signal keeps its level going down");
}

void testPreDelayFrame() {
  det::IrBuf none;
  none.rate = 44100;
  none.paths = 2;
  none.p[0].assign(5000, 0.f);
  none.p[1].assign(5000, 0.f);
  none.p[0][0] = 1.f;
  expect(rf::preDelayExtraFrame(none, 100), "a pre-delay with no room in the impulse's silence holds one more frame");
  det::IrBuf lead = none;
  lead.p[0][0] = 0.f;
  lead.p[0][1000] = 1.f;
  expect(!rf::preDelayExtraFrame(lead, 100), "a pre-delay that fits the silence holds no more");
}

// ------------------------------------------------------------------------ levels and gain moves
void testRatesAndLevels() {
  const std::string p = impulseFile("level.wav", 2000, 100, 0.5f);
  FlState f;
  f.path = p;
  flp::PluginMapping m;
  expect(decode(flState(f), m), "decodes with a file");
  normalizePluginState(m.state);
  std::vector<float> in(20000, 0.f);
  in[2000] = 1.f;
  const float at44 = peakOf(renderL(m.state, 44100, in));
  const float at88 = peakOf(renderL(m.state, 88200, in));
  expect(near(at44, 0.5, 0.02), "wet level at 44.1 kHz");
  expect(near(at88 / at44, 0.5, 0.02), "FL's wet level scales with 44100 / rate");

  PluginState old = m.state;
  sx::ConvData cd = sx::parseConvData(old.data);
  cd.envFit = false;
  old.data = sx::formatConvData(cd);
  const float oldAt44 = peakOf(renderL(old, 44100, in));
  std::vector<float> oldRender = renderL(old, 88200, in);
  expect(near(peakOf(oldRender) / oldAt44, 1.0, 0.1), "a state without the marker keeps its wet level at any rate");
}

void testGainMoves() {
  const std::string p = impulseFile("move.wav", 200, 0, 0.0f);
  FlState f;
  f.path = p;
  f.dry = 0;
  f.wet = 0;
  flp::PluginMapping m;
  decode(flState(f), m);
  normalizePluginState(m.state);
  PluginState st = m.state;
  st.params[sx::CvDry] = 0.f;
  auto fx = createEffect(st.type);
  fx->prepareHost(st, 44100);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  std::vector<float> l(256, 1.f), r(256, 1.f);
  fx->processAux(st, aux, ctx, l.data(), r.data(), 256);
  st.params[sx::CvDry] = 1.f;
  std::fill(l.begin(), l.end(), 1.f), std::fill(r.begin(), r.end(), 1.f);
  fx->processAux(st, aux, ctx, l.data(), r.data(), 256);
  expect(near(l[0], 0.0, 1e-6) && near(l[100], 0.1, 1e-5) && near(l[255], 0.255, 1e-5), "dry moves at FL's limit of 0.001 a sample");
  std::fill(l.begin(), l.end(), 1.f), std::fill(r.begin(), r.end(), 1.f);
  fx->processAux(st, aux, ctx, l.data(), r.data(), 256);
  expect(near(l[0], 0.256, 1e-5), "the move goes on where it stopped");
}

void testKernelRate() {
  const std::string p = impulseFile("rate.wav", 2000, 100, 0.5f);
  FlState f;
  f.path = p;
  flp::PluginMapping m;
  decode(flState(f), m);
  normalizePluginState(m.state);
  sx::setConvolverRateHint(44100);
  auto fx = createEffect(m.state.type);
  fx->prepareHost(m.state, 48000);
  ChannelAux aux;
  packs::prepareAux(m.state, aux);
  SamplePtr k = aux.sample(0);
  expect(k && near(k->sampleRate, 48000, 1e-9), "a kernel is built for the rate of the engine being built, not an earlier one");
  sx::setConvolverRateHint(44100);
}

// The app's builds never wait for a decode: the first one finds the impulse still loading. That answer must
// not stick, or the Convolver stays without its impulse (wet silent) for the whole session.
void testLoadingImpulse() {
  const std::string p = impulseFile("loading.wav", 2000, 100, 0.5f);
  FlState f;
  f.path = p;
  flp::PluginMapping m;
  decode(flState(f), m);
  normalizePluginState(m.state);
  sx::setConvolverRateHint(44100);
  sampleCache().clear();
  sampleCache().pauseBackground(true);
  ChannelAux first;
  {
    SampleCache::NonBlocking nb;
    packs::prepareAux(m.state, first);
  }
  expect(!first.sample(0), "a build that finds the impulse still loading has no kernel yet");
  sampleCache().pauseBackground(false);
  sampleCache().waitIdle();
  ChannelAux second;
  {
    SampleCache::NonBlocking nb;
    packs::prepareAux(m.state, second);
  }
  expect(static_cast<bool>(second.sample(0)), "the next build, once the impulse has loaded, gets the kernel");
  std::vector<float> in(20000, 0.f);
  in[2000] = 1.f;
  expect(near(peakOf(renderL(m.state, 44100, in)), 0.5, 0.02), "and the Convolver sounds");
}

// ----------------------------------------------------------------------------------- automation
void testKnobLaw() {
  AutoPoint a, b;
  a.pos = 0, a.value = 0.33f;
  b.pos = 960, b.value = 0.5f;
  const auto law = [](double x) { return (std::pow(1001.0, x) - 1.0) / 1000.0; };
  const std::vector<AutoPoint> fl = {a, b};
  std::vector<AutoPoint> out = fl;
  for (auto& p : out) p.value = static_cast<float>(law(p.value));
  followFlKnobLaw(fl, out, law);
  expect(out.size() > 2, "a law that bends gets more points");
  bool sorted = true;
  for (size_t i = 1; i < out.size(); ++i) sorted = sorted && out[i].pos >= out[i - 1].pos;
  expect(sorted && near(out.back().value, law(0.5), 1e-5), "the points stay in order and end on the target");
  double worst = 0;
  for (int i = 0; i <= 960; i += 7) {
    const double u = i / 960.0;
    const double want = law(0.33 + (0.5 - 0.33) * u);
    double got = out.back().value;
    for (size_t k = 1; k < out.size(); ++k) {
      const double p0 = static_cast<double>(out[k - 1].pos) + out[k - 1].frac, p1 = static_cast<double>(out[k].pos) + out[k].frac;
      if (i <= p1) {
        got = out[k - 1].value + (out[k].value - out[k - 1].value) * (p1 > p0 ? (i - p0) / (p1 - p0) : 1.0);
        break;
      }
    }
    worst = std::max(worst, std::fabs(20 * std::log10(std::max(got, 1e-9) / want)));
  }
  expect(worst < 0.2, "the straight pieces follow FL's values within 0.2 dB");
  // A straight law keeps its two points.
  std::vector<AutoPoint> linear = fl;
  followFlKnobLaw(fl, linear, [](double x) { return x; });
  expect(linear.size() == 2, "a straight law keeps its points");
}

// ------------------------------------------------------------------------------- export to FL
void testExport() {
  const std::string p = impulseFile("export.wav", 44100, 100, 0.5f);
  flp::enc::EncodeContext ctx;
  {  // A state from FL with a curve, moved knobs: back out and in.
    FlState f;
    f.path = p;
    f.wetSep = 500;
    f.stretch = 500;
    f.pre = 300;
    f.vol = {{0, 0.2}, {3.0, 0.9}, {10.0, 0.1}};
    flp::PluginMapping m;
    decode(flState(f), m);
    normalizePluginState(m.state);
    PluginState edited = m.state;
    edited.params[sx::CvInWidth] = static_cast<float>(1.0 - (std::pow(5.0, 0.25) - 1.0) * 0.25);
    const flp::enc::Result r = flp::enc::encode(edited, "", nullptr, ctx);
    expect(r.quality == flp::enc::Result::Exact, std::string("an edited FL state exports exactly (") + flp::enc::qualityName(r.quality) + ")");
    flp::PluginMapping back;
    expect(decode(r.state, back), "the export decodes");
    normalizePluginState(back.state);
    const sx::ConvData cd = sx::parseConvData(back.state.data);
    expect(cd.envFit && cd.env.size() == 3 && near(cd.env.back().t, 10.0, 1e-9), "the curve keeps its own length through export");
    expect(near(P(back.state, sx::CvInWidth), P(edited, sx::CvInWidth), 1e-3), "the separation law inverts on export");
    expect(near(P(back.state, sx::CvStretch), std::pow(2.0, 0.5), 1e-5), "stretch through export");
  }
  {  // Gridline's own curve in seconds: FL spreads it, so it goes out ending at the impulse's length.
    PluginState s = defaultPluginState("gl.convolver");
    s.params[sx::CvSource] = sx::SrcFile;
    s.params[sx::CvModel] = 1;
    s.params[sx::CvEnvOn] = 1;
    sx::ConvData cd;
    cd.path = p;
    sx::EnvPoint a, b;
    a.t = 0, a.v = 0.8;
    b.t = 0.5, b.v = 0.2;  // ends before the impulse's second
    cd.env = {a, b};
    s.data = sx::formatConvData(cd);
    normalizePluginState(s);
    const flp::enc::Result r = flp::enc::encode(s, "", nullptr, ctx);
    flp::PluginMapping back;
    expect(r.hasState && decode(r.state, back), "Gridline's own curve exports");
    const sx::ConvData c2 = sx::parseConvData(back.state.data);
      expect(c2.env.size() == 3 && near(c2.env[1].t, 0.5, 1e-9) && near(c2.env.back().t, 1.0, 1e-9) && near(c2.env.back().v, 0.2, 1e-9),
           "a curve shorter than the impulse holds to its end, so its times stay true in FL");
  }
}

// ----------------------------------------------------------------- old projects keep their sound
void testOldStates() {
  // Model 1 without the marker: the same kernel as before K-62 (shared offline kernel).
  std::vector<float> in(4000, 0.f);
  in[1000] = 1.f;
  PluginState s = defaultPluginState("gl.convolver");
  s.params[sx::CvModel] = 1;
  s.params[sx::CvNormalize] = 0;
  det::IrBuf ir;
  ir.rate = 44100;
  ir.paths = 2;
  ir.p[0] = in;
  ir.p[1] = in;
  const std::vector<float> legacy = resampleChannel(in.data(), in.size(), 44100, 48000, 64);
  rf::buildForRate(ir, s, 48000);
  bool same = !ir.p[0].empty() && ir.p[0].size() <= legacy.size();  // the tail is trimmed afterwards
  for (size_t i = 0; same && i < ir.p[0].size(); ++i) same = std::fabs(ir.p[0][i] - legacy[i]) < 1e-3f;
  expect(same, "a state without the marker keeps the shared conversion kernel");
  // The editing model never reads a marker.
  PluginState own = defaultPluginState("gl.convolver");
  expect(P(own, sx::CvModel) == 0.f, "new instances stay on the editing model");
}

}  // namespace

int main() {
  testEnvelopeSpread();
  testMarkerText();
  testDecoder();
  testRegion();
  testConversion();
  testPreDelayFrame();
  testRatesAndLevels();
  testGainMoves();
  testKernelRate();
  testLoadingImpulse();
  testKnobLaw();
  testExport();
  testOldStates();
  std::error_code ec;
  fs::remove_all(scratch(), ec);
  return finish();
}

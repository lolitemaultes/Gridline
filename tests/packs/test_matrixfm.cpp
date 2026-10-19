// Gridline — "synths" pack tests: Matrix FM (gl.matrixfm).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpGen.h"
#include "../../src/core/flp/FlpPresets.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/synths/matrixfm.h"
#include "../../src/packs/synths/matrixfm_data.h"
#include "../Check.h"

using namespace gl;
using namespace gl::packs::synths::mfm;

namespace {

struct Ev {
  int at;
  bool on;
  uint32_t id;
  double pitch;
  float vel;
};

struct Out {
  std::vector<float> L, R;
  bool finite = true;
  float peak = 0;
  bool activeAtEnd = false;
};

Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block, double sr = 44100.0) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.f);
  o.R.assign(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) {
    o.finite = false;
    return o;
  }
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 120;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on) g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else g->noteOff(e.id, e.at - pos);
      }
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
  }
  o.activeAtEnd = g->active();
  return o;
}

float peakIn(const Out& o, int a, int b) {
  float p = 0;
  for (int i = std::max(0, a); i < std::min(b, static_cast<int>(o.L.size())); ++i)
    p = std::max({p, std::fabs(o.L[static_cast<size_t>(i)]), std::fabs(o.R[static_cast<size_t>(i)])});
  return p;
}

double tone(const std::vector<float>& x, int a, int b, double f, double sr = 44100) {
  const double w = 2 * 3.14159265358979 * f / sr, c = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (int i = a; i < b; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * (i - a) / (b - a));
    const double s0 = x[static_cast<size_t>(i)] * win + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (b - a);
}

// Zero crossings per second (pitch estimate) over [a, b).
double zcHz(const std::vector<float>& x, int a, int b, double sr = 44100) {
  int z = 0;
  for (int i = a + 1; i < b; ++i)
    if (x[static_cast<size_t>(i - 1)] <= 0 && x[static_cast<size_t>(i)] > 0) ++z;
  return z * sr / (b - a);
}

PluginState base() { return defaultPluginState("gl.matrixfm"); }
void set(PluginState& s, int i, float v) { s.params[static_cast<size_t>(i)] = v; }
float get(const PluginState& s, int i) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : -999.f; }

// ---- synthetic Sytrus state (version 25), written field by field ----------
struct SyCurve {
  std::vector<std::array<double, 3>> pts;  // dx, y, tension
  int flags = 0, enabled = 0, decay = -1, loop = -1, sustain = -1;
};
struct SyArt {
  SyCurve env, lfo;
  int adsr[4] = {128, 128, 0, 128};
  int lfo5[5] = {40000, 0, 0, 0, 0};  // speed, tension, skew, pulse width, phase (u32)
};
struct SyState {
  int32_t main[240] = {};
  uint8_t mute[162] = {};  // fm layer then ring layer, row × 9 + col (overlays main bytes 800..)
  int soften = 0;
  uint8_t gibbs = 0;
  int rev[9] = {75, 40, 0, 40, 100, 15, 40, 2, 25};
  uint8_t revOn = 0, revTempo = 0;
  SyArt arts[kUnits][kFltArts];
};
struct Wr {
  std::vector<uint8_t> b;
  void i32(int32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k)));
  }
  void u8(int v) { b.push_back(static_cast<uint8_t>(v)); }
  void f32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    i32(static_cast<int32_t>(u));
  }
  void f64(double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int k = 0; k < 8; ++k) b.push_back(static_cast<uint8_t>(u >> (8 * k)));
  }
  void curve(int kind, const SyCurve& c, bool tail5) {
    i32(kind), i32(2), i32(static_cast<int>(c.pts.size()));
    for (const auto& p : c.pts) f64(p[0]), f64(p[1]), f32(static_cast<float>(p[2])), i32(0);
    i32(c.flags), i32(c.enabled), i32(c.decay), i32(c.loop);
    if (tail5) i32(c.sustain);
  }
};
std::vector<uint8_t> sytrusState(const SyState& S) {
  Wr w;
  const int ver = 25;
  std::vector<uint8_t> mainBytes(962, 0);
  for (int i = 0; i < 240; ++i)
    for (int k = 0; k < 4; ++k) mainBytes[static_cast<size_t>(i * 4 + k)] = static_cast<uint8_t>(static_cast<uint32_t>(S.main[i]) >> (8 * k));
  for (int i = 0; i < 162; ++i) mainBytes[static_cast<size_t>(800 + i)] = S.mute[i];
  w.b = mainBytes;
  w.i32(S.soften), w.u8(0), w.u8(0), w.u8(S.gibbs);
  for (int i = 0; i < 9; ++i) w.u8(0);  // later option bytes
  for (int d = 0; d < 3; ++d) {
    w.i32(0), w.i32(96), w.i32(0), w.i32(64), w.i32(0);
    w.u8(1), w.u8(0), w.u8(0);
  }
  for (int v : S.rev) w.i32(v);
  w.u8(S.revOn), w.u8(S.revTempo);
  w.i32(0);
  for (int b = 0; b < 3; ++b) w.i32(0), w.i32(30000), w.i32(17500), w.i32(1);
  w.u8(0);  // info text (UTF-16, empty)
  for (int u = 0; u < kUnits; ++u) {
    for (int i = 0; i < 10; ++i) w.i32(u < kOps && i == 1 ? 256 : 0);
    if (u < kOps) {
      w.i32(20000), w.i32(0), w.i32(0);
      for (int h = 0; h < 256; ++h) w.f32(h == 0 ? 1.f : 0.f);
      for (int h = 0; h < 256; ++h) w.f32(0.f);
      w.i32(0), w.i32(0), w.i32(0), w.i32(0), w.i32(128);
      for (int i = 0; i < 8; ++i) w.u8(0);
    } else if (u < kOps + kFilters) {
      for (int i = 0; i < 12; ++i) w.i32(0);
      SyCurve ws;
      ws.pts = {{0, 0, 0}, {1, 1, 0}};
      w.curve(3, ws, true);
      w.u8(0);
    }
    for (int a = 0; a < unitArts(u); ++a) {
      const SyArt& A = S.arts[u][a];
      w.i32(3), w.f32(0), w.f32(0), w.f32(0);
      w.curve(1, A.env, true);
      for (int v : A.adsr) w.i32(v);
      w.curve(2, A.lfo, true);
      for (int v : A.lfo5) w.i32(v);
      const double neutral = artBipolar(u, a) ? 0.5 : 1.0;
      for (int m = 0; m < kMaps; ++m) {
        SyCurve mc;
        mc.pts = {{0, neutral, 0}, {1, neutral, 0}};
        w.curve(3, mc, true);
      }
    }
  }
  w.u8(0), w.u8(0);
  // Header + zlib stream made of stored blocks.
  std::vector<uint8_t> out;
  const uint32_t hdr = (static_cast<uint32_t>(ver) << 16) | 3u;
  for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(hdr >> (8 * k)));
  out.push_back(0x78), out.push_back(0x01);
  size_t at = 0;
  while (at < w.b.size()) {
    const size_t n = std::min<size_t>(65535, w.b.size() - at);
    out.push_back(at + n == w.b.size() ? 1 : 0);
    out.push_back(static_cast<uint8_t>(n)), out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(~n)), out.push_back(static_cast<uint8_t>((~n) >> 8));
    out.insert(out.end(), w.b.begin() + static_cast<long>(at), w.b.begin() + static_cast<long>(at + n));
    at += n;
  }
  uint32_t s1 = 1, s2 = 0;
  for (uint8_t c : w.b) s1 = (s1 + c) % 65521, s2 = (s2 + s1) % 65521;
  const uint32_t ad = (s2 << 16) | s1;
  for (int k = 3; k >= 0; --k) out.push_back(static_cast<uint8_t>(ad >> (8 * k)));
  return out;
}
bool decodeSy(const SyState& S, PluginState& st, PatchData& pd) {
  std::vector<uint8_t> bytes = sytrusState(S);
  flp::File f;
  Song song;
  flp::Report rep;
  flp::Context c(f, song, rep);
  flp::Event e;
  e.id = 213;
  e.data = bytes.data();
  e.size = static_cast<uint32_t>(bytes.size());
  flp::PluginMapping m;
  if (!flp::gen::decodeSytrusMatrix(c, {e}, m)) return false;
  st = m.state;
  normalizePluginState(st);
  return parsePatch(st.data, pd);
}

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.matrixfm");
  expect(info != nullptr, "catalogue entry");
  if (!info) return finish();
  expect(static_cast<int>(info->params.size()) == kParamCount, "parameter count");
  expect(info->presets.size() >= 2 && info->presets.size() <= 6, "2-6 factory presets");
  bool named = true;
  for (const auto& p : info->params) named = named && !p.name.empty() && p.name != "Reserved";
  expect(named, "every parameter is named");
  for (const auto& p : info->params)
    if (p.def < p.min || p.def > p.max) {
      std::printf("# default out of range: %s\n", p.name.c_str());
      named = false;
    }
  expect(named, "defaults inside ranges");

  // Short / old states normalize.
  {
    PluginState s;
    s.type = "gl.matrixfm";
    s.params = {0.5f, 0.f};
    normalizePluginState(s);
    expect(static_cast<int>(s.params.size()) == kParamCount, "normalizePluginState pads");
  }

  const std::vector<Ev> one = {{0, true, 1, 69, 0.9f}, {22050, false, 1, 0, 0}};
  // Default: a sine at the note's pitch.
  {
    Out o = render(base(), one, 44100 * 2, 256);
    expect(o.finite && o.peak > 0.05f, "default patch sounds");
    const double f440 = tone(o.L, 4410, 15000, 440), f880 = tone(o.L, 4410, 15000, 880);
    expect(f440 > 20 * f880, "default operator is a sine at the note pitch");
    expect(peakIn(o, 44100 + 22050, 88200) < 1e-4f, "release ends in silence");
    expect(!o.activeAtEnd, "voice finishes after the release");
  }
  // Block-size invariance.
  {
    PluginState s = presetState(*info, 0);
    Out a = render(s, one, 30000, 1), b = render(s, one, 30000, 64), c = render(s, one, 30000, 999);
    float d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max({d, std::fabs(a.L[i] - b.L[i]), std::fabs(a.L[i] - c.L[i])});
    expect(d < 1e-4f, "block-size invariant");
  }
  // Frequency modulation adds sidebands.
  {
    PluginState s = base();
    set(s, fm(0, 1), 0.3f);
    Out o = render(s, one, 20000, 256);
    const double f880 = tone(o.L, 4410, 15000, 880), ref = tone(render(base(), one, 20000, 256).L, 4410, 15000, 880);
    expect(f880 > 50 * std::max(ref, 1e-9) && f880 > 1e-3, "operator 2 modulates operator 1 (sidebands)");
  }
  // Operator ratio.
  {
    PluginState s = base();
    set(s, op(0, OpRatio), 2);
    Out o = render(s, one, 20000, 256);
    expect(tone(o.L, 4410, 15000, 880) > 20 * tone(o.L, 4410, 15000, 440), "ratio 2 plays an octave up");
  }
  // Harmonic table from the data.
  {
    PluginState s = base();
    s.data = "mfm 1\nh 0 2:1:0\n";  // third harmonic only
    Out o = render(s, one, 20000, 256);
    expect(tone(o.L, 4410, 15000, 1320) > 20 * tone(o.L, 4410, 15000, 440), "drawn harmonics shape the operator");
  }
  // Saw shape through filter 1: the cutoff removes upper harmonics.
  {
    auto cut = [&](float c) {
      PluginState s = base();
      set(s, op(0, OpShape), 1);
      set(s, fm(0, kColOut), 0);
      set(s, fm(6, 0), 1);
      set(s, fm(6, kColOut), 1);
      set(s, flt(0, FlCut), c);
      Out o = render(s, {{0, true, 1, 45, 1.f}}, 20000, 256);
      return tone(o.L, 4410, 15000, 110 * 15) / std::max(1e-9, tone(o.L, 4410, 15000, 110));
    };
    expect(cut(0.35f) < 0.1 * cut(1.0f), "filter cutoff");
  }
  // Every filter type / mode produces finite output.
  {
    bool ok = true;
    for (int t = 0; t < kFltTypes; ++t)
      for (int m = 0; m < kFltModes; ++m) {
        PluginState s = base();
        set(s, op(0, OpShape), 1.5f);
        set(s, fm(0, kColOut), 0), set(s, fm(6, 0), 1), set(s, fm(6, kColOut), 1);
        set(s, flt(0, FlType), static_cast<float>(t)), set(s, flt(0, FlMode), static_cast<float>(m)), set(s, flt(0, FlRes), 1);
        set(s, flt(0, FlWsOn), 1);
        s.data = "mfm 1\nw 0 0,0,0;0.5,1,0.3;0.5,0.4,0\n";
        Out o = render(s, one, 8000, 128);
        ok = ok && o.finite && o.peak < 8;
      }
    expect(ok, "filter types x modes finite (with waveshaper)");
  }
  // Ring modulation.
  {
    PluginState s = base();
    set(s, rm(0, 1), 1);
    set(s, op(1, OpRatio), 0.5f);
    Out o = render(s, one, 20000, 256);
    // sin(a)·sin(a/2): energy moves to 220 and 660 Hz.
    expect(tone(o.L, 4410, 15000, 660) > 5 * tone(o.L, 4410, 15000, 440), "ring modulation layer");
  }
  // Envelope from the data with a sustain point; release time scale.
  {
    PluginState s = base();
    s.data = "mfm 1\ne 9 1 1 -1 -1 0,0,0;0.001,1,0;0.05,0.5,0;0.05,0,0\n";
    Out o = render(s, one, 44100, 256);
    expect(peakIn(o, 6000, 20000) > 0.1f && peakIn(o, 30000, 44100) < 1e-4f, "drawn envelope: sustain then release");
    set(s, art(kUnits - 1, MaVol, AfRel), 2);  // 256 x release
    Out r = render(s, one, 44100, 256);
    expect(peakIn(r, 30000, 44100) > 0.01f, "release scale lengthens the release");
  }
  // Pitch LFO (articulator).
  {
    PluginState s = base();
    set(s, art(0, OaPitch, AfLfoOn), 1);
    set(s, art(0, OaPitch, AfLfoSpeed), 3);
    set(s, op(0, OpPitchRange), 12);
    s.data = "mfm 1\nl 0 3 0,1,0;0.5,1,0;0,0,0;0.5,0,0\n";  // square LFO: ±0.5 → ±6 semitones
    Out o = render(s, {{0, true, 1, 69, 1}}, 44100, 256);
    const double a = zcHz(o.L, 2000, 6000), b = zcHz(o.L, 9000, 13000);
    expect((a > 550 && b < 350) || (a < 350 && b > 550), "pitch LFO swings the operator pitch");
  }
  // Key map on pitch (bipolar offset): a flat 0.75 curve adds half the range (+6 st).
  {
    PluginState s = base();
    s.data = "mfm 1\nm 0 3 0 0,0.75,0;1,0.75,0\n";
    Out o = render(s, one, 20000, 256);
    expect(std::fabs(zcHz(o.L, 4000, 16000) - 440 * std::sqrt(2.0)) < 6, "pitch key map offsets the pitch");
  }
  // Unison widens the stereo image.
  {
    PluginState s = base();
    set(s, MUniOrder, 5);
    Out o = render(s, one, 20000, 256);
    double diff = 0;
    for (size_t i = 4000; i < 16000; ++i) diff += std::fabs(o.L[i] - o.R[i]);
    expect(o.finite && diff > 1, "unison spreads voices");
  }
  // Reverb on the effects send keeps a tail after the release.
  {
    PluginState s = base();
    set(s, fm(0, kColFx), 1);
    set(s, RvOn, 1);
    set(s, RvVol, 1);
    set(s, RvDecay, 3);
    Out o = render(s, one, 44100 + 10000, 256);
    expect(peakIn(o, 44100, 44100 + 10000) > 1e-3f, "reverb tail on the effects bus");
    PluginState d = s;
    set(d, Dl1On, 1), set(d, RvOn, 0), set(d, ChOrder, 3);
    Out e = render(d, one, 60000, 256);
    expect(e.finite && peakIn(e, 30000, 60000) > 1e-3f, "delay + chorus echoes");
  }
  // EQ on the output.
  {
    PluginState s = base();
    set(s, MEqTarget, 1);
    set(s, eq(1, 1), 440), set(s, eq(1, 3), -18);
    Out o = render(s, one, 20000, 256), r = render(base(), one, 20000, 256);
    expect(tone(o.L, 4410, 15000, 440) < 0.3 * tone(r.L, 4410, 15000, 440), "EQ cut at 440 Hz");
  }
  // Mono + portamento glides.
  {
    PluginState s = base();
    set(s, MVoices, 1), set(s, MPorta, 1), set(s, MPortaTime, 0.2f);
    Out o = render(s, {{0, true, 1, 57, 1}, {11025, true, 2, 69, 1}}, 44100, 256);
    const double mid = zcHz(o.L, 11025 + 2000, 11025 + 5000), end = zcHz(o.L, 30000, 40000);
    expect(mid > 230 && mid < 420 && std::fabs(end - 440) < 10, "mono portamento glide");
  }
  // Presets: sound, finite, sane level.
  for (size_t i = 0; i < info->presets.size(); ++i) {
    Out o = render(presetState(*info, i), {{0, true, 1, 60, 0.8f}, {0, true, 2, 64, 0.8f}, {20000, false, 1, 0, 0}, {20000, false, 2, 0, 0}},
                   60000, 512);
    char name[96];
    std::snprintf(name, sizeof name, "preset '%s' sounds (peak %.2f)", info->presets[i].name.c_str(), o.peak);
    expect(o.finite && o.peak > 0.02f && o.peak < 4.f, name);
  }
  // Parameter extremes.
  {
    PluginState mn = base(), mx = base();
    for (size_t i = 0; i < info->params.size(); ++i) {
      mn.params[i] = static_cast<float>(info->params[i].min);
      mx.params[i] = static_cast<float>(info->params[i].max);
    }
    Out a = render(mn, one, 30000, 256), b = render(mx, one, 30000, 256);
    expect(a.finite && b.finite && b.peak < 1e4f, "parameter extremes finite");
  }
  // 96 kHz.
  {
    Out o = render(presetState(*info, 1), one, 40000, 300, 96000);
    expect(o.finite && o.peak > 0.02f, "96 kHz");
  }
  // Data round trip.
  {
    PatchData d;
    d.level[2][5] = 0.25f;
    d.phase[2][5] = 0.5f;
    d.arts[7][FaCut].env.pts = {{0, 0.2f, 0}, {0.5f, 1, 0.3f}, {1, 0, -0.2f}};
    d.arts[7][FaCut].env.sustain = 1;
    d.arts[1][OaVol].maps[MapVel].pts = {{0, 0, 0}, {1, 1, 0}};
    d.ws[1].pts = {{0, 0, 0}, {1, 0.7f, 0.5f}};
    PatchData e;
    const bool ok = parsePatch(serializePatch(d), e);
    expect(ok && e.level[2][5] == 0.25f && e.phase[2][5] == 0.5f && e.arts[7][FaCut].env.sustain == 1 &&
               e.arts[7][FaCut].env.pts.size() == 3 && e.arts[1][OaVol].maps[MapVel].pts.size() == 2 && e.ws[1].pts.size() == 2 &&
               e.level[0][0] == 1.f,
           "patch data round trip");
    expect(serializePatch(PatchData()) == "mfm 1\n", "default data serializes empty");
    PatchData junk;
    expect(!parsePatch("hello", junk) && parsePatch("mfm 1\nh 9 1:1\ne 99 0 0 0 0 x\nm 1 1 9 0,0,0\n", junk), "malformed data ignored");
  }
  // ---- J-3b: articulator records decoded field by field (synthetic Sytrus state) ----
  {
    SyState S;
    S.main[0] = 128;
    S.main[3 + 0 * 9 + kColOut] = 256;  // operator 1 → output
    S.main[3 + 1 * 9 + kColOut] = 200;  // operator 2 → output, muted below
    S.mute[1 * 9 + kColOut] = 1;
    S.main[191] = 256;
    S.soften = 64, S.gibbs = 1;
    S.rev[2] = 96, S.revOn = 1;
    // Operator 1 volume envelope: 6 points, decay point 2, loop 3 → sustain 4, tempo-synced.
    SyArt& V = S.arts[0][OaVol];
    V.env.pts = {{0, 0, 0}, {0.01, 1, 0}, {0.05, 0.8, 0}, {0.1, 0.2, 0}, {0.1, 0.9, 0.5}, {0.2, 0, 0}};
    V.env.enabled = -1, V.env.decay = 2, V.env.loop = 3, V.env.sustain = 4, V.env.flags = 1;
    V.adsr[0] = 64;
    // Operator 1 pitch LFO: tempo + global, speed / tension / skew / width / phase.
    SyArt& P = S.arts[0][OaPitch];
    P.lfo.pts = {{0, 0.5, 0}, {0.5, 1, 0}, {0.5, 0, 0}};
    P.lfo.enabled = -1, P.lfo.flags = 3, P.lfo.sustain = 2;
    P.lfo5[0] = 32768, P.lfo5[1] = 32, P.lfo5[2] = -64, P.lfo5[3] = 16, P.lfo5[4] = static_cast<int>(0x40000000u);  // phase 1/4
    // Filter 1 cutoff envelope without decay point.
    SyArt& F = S.arts[6][FaCut];
    F.env.pts = {{0, 0.5, 0}, {0.1, 1, 0.4}, {0.3, 0.5, 0}};
    F.env.enabled = -1, F.env.sustain = 2;
    PluginState st;
    PatchData pd;
    const bool ok = decodeSy(S, st, pd);
    expect(ok, "synthetic Sytrus state walks and decodes");
    if (ok) {
      const Curve& e = pd.arts[0][OaVol].env;
      expect(e.sustain == 4, "sustain point = p1[0] (loop end), not decay + 1");
      expect(e.decay == 2, "decay point = envelope tail[2]");
      expect(e.loopStart == 3 && e.loopEnd == 4, "loop start = envelope tail[3], loops up to the sustain point");
      expect(e.tempo && get(st, art(0, OaVol, AfEnvOn)) == 1, "envelope tempo flag (tail[0] bit 0) and enable");
      expect(std::fabs(get(st, art(0, OaVol, AfAtt)) - 0.5f) < 1e-6f, "envelope A/D/S/R scales follow the sustain index");
      expect(pd.arts[6][FaCut].env.decay == 0 && pd.arts[6][FaCut].env.sustain == 2, "no decay point → no attack part");
      expect(pd.arts[6][FaCut].env.pts[1].tension < -0.39f, "FL tension (positive = fast start) maps to Matrix FM's sign");
      expect(get(st, art(0, OaPitch, AfTempo)) == 1 && get(st, art(0, OaPitch, AfLfoGlobal)) == 1, "LFO tempo / global flags");
      expect(std::fabs(get(st, art(0, OaPitch, AfLfoSkew)) + 0.5f) < 1e-6f, "LFO skew = p2[3] / 128");
      expect(std::fabs(pd.arts[0][OaPitch].lfoTension - 0.25f) < 1e-6f && std::fabs(pd.arts[0][OaPitch].lfoPw - 0.125f) < 1e-6f,
             "LFO tension = p2[2], pulse width = p2[4]");
      expect(std::fabs(pd.arts[0][OaPitch].lfoPhase - 0.25f) < 1e-6f, "LFO phase = p2[5] / 2^32");
      const double hz = (std::pow(10001.0, 0.5) - 1) / 625;  // speed 32768
      expect(std::fabs(get(st, art(0, OaPitch, AfLfoSpeed)) - hz * 0.5) < 1e-4, "LFO speed (tempo: cycles per beat at 120 BPM)");
      expect(get(st, fm(0, kColOut)) == 1 && get(st, fm(1, kColOut)) == 0, "muted matrix cell decodes as 0");
      expect(std::fabs(get(st, MSoften) - 0.5f) < 1e-6f && get(st, MGibbs) == 1, "soften dword and ringing (Gibbs) byte");
      expect(std::fabs(get(st, RvPreDelay) - 0.25f) < 1e-6f, "reverb predelay: 1/384 s units");
      PatchData rt;
      expect(parsePatch(serializePatch(pd), rt) && rt.arts[0][OaVol].env.decay == 2 && rt.arts[0][OaVol].env.tempo &&
                 std::fabs(rt.arts[0][OaPitch].lfoPhase - 0.25f) < 1e-6f && std::fabs(rt.arts[0][OaPitch].lfoPw - 0.125f) < 1e-6f,
             "decay / tempo / LFO extras survive the data round trip");
    }
  }
  // Engine: the decay point splits attack and decay time scales.
  {
    auto level = [&](int decay, float att) {
      PluginState s = base();
      char buf[160];
      std::snprintf(buf, sizeof buf, "mfm 1\ne 9 1 3 -1 -1 0,0,0;0.05,1,0;0.05,0.6,0;0.05,0.3,0 dec=%d\n", decay);
      s.data = buf;
      set(s, art(kUnits - 1, MaVol, AfAtt), att);
      Out o = render(s, {{0, true, 1, 69, 1.f}}, 16000, 256);
      return peakIn(o, 12800, 14000);  // ~0.3 s: past segment 1 (stretched to 0.2 s by attack 1.25)
    };
    // Decay point 2: the attack scale also stretches segment 2; decay point 1 leaves it alone.
    expect(level(2, 1.25f) > 1.8f * level(1, 1.25f), "decay point sets which segments the attack scale stretches");
  }
  // Engine: tempo envelopes follow the song tempo (drawn at 120 BPM).
  {
    PluginState s = base();
    s.data = "mfm 1\ne 9 1 -1 -1 -1 0,1,0;0.5,1,0;0.01,0,0 tempo=1\n";
    auto endAt = [&](double tempo) {
      auto g = packs::createGenerator(s.type);
      ChannelAux aux;
      packs::prepareAux(s, aux);
      ProcessContext ctx;
      ctx.sampleRate = 44100, ctx.tempo = tempo;
      std::vector<float> L(441), R(441);
      g->noteOn(1, 69, 1, 0);
      for (int blk = 0; blk < 200; ++blk) {
        std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
        g->process(s, aux, ctx, L.data(), R.data(), 441);
        float p = 0;
        for (float x : L) p = std::max(p, std::fabs(x));
        if (blk > 2 && p < 1e-3f) return blk * 0.01;
      }
      return 2.0;
    };
    const double a = endAt(120), b = endAt(240);
    expect(std::fabs(a - 0.52) < 0.04 && std::fabs(b - 0.27) < 0.04, "tempo envelope: half as long at double tempo");
  }
  // Engine: a loop between loop start and sustain keeps cycling while held.
  {
    PluginState s = base();
    s.data = "mfm 1\ne 9 1 4 2 4 0,0,0;0.005,1,0;0.005,1,0;0.05,0,0;0.05,1,0;0.05,0,0\n";
    Out o = render(s, {{0, true, 1, 69, 1.f}}, 44100, 256);
    int lows = 0;
    for (int w = 4410; w + 441 <= 44100; w += 441) lows += peakIn(o, w, w + 441) < 0.15f;
    expect(lows >= 6 && peakIn(o, 30000, 44100) > 0.2f, "looping envelope repeats while the note is held");
  }
  // Engine: LFO start phase.
  {
    auto first = [&](const char* data) {
      PluginState s = base();
      set(s, art(kUnits - 1, MaVol, AfLfoOn), 1);
      set(s, art(kUnits - 1, MaVol, AfLfoSpeed), 0.5f);
      s.data = data;
      Out o = render(s, {{0, true, 1, 69, 1.f}}, 4410, 64);
      return peakIn(o, 2000, 4410);
    };
    // Square LFO on the main volume: phase 0 starts high, phase 0.5 starts low.
    const float hi = first("mfm 1\nl 9 1 0,1,0;0.5,1,0;0,0,0;0.5,0,0\n"), lo = first("mfm 1\nl 9 1 0,1,0;0.5,1,0;0,0,0;0.5,0,0 ph=0.5\n");
    expect(hi > 3 * lo, "LFO start phase");
  }
  // FL Studio's shipped Sytrus presets (when installed): full decode → Matrix FM, every one renders.
  {
    namespace fs = std::filesystem;
    const std::string fl = "/Applications/FL Studio 2026.app/Contents/Resources/FL";
    const fs::path dir = fs::path(fl) / "Data/Patches/Plugin presets/Generators/Sytrus";
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
      int total = 0, matrix = 0, sounding = 0, finite = 0;
      int loops = 0, tempoEnvs = 0, decayPts = 0, susNotDecayPlus1 = 0, tempoLfos = 0;
      std::vector<std::pair<std::string, int>> gates;  // presets whose operator volume is a looping on/off gate
      std::vector<fs::path> files;
      for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".fst") files.push_back(it->path());
      std::sort(files.begin(), files.end());
      for (const fs::path& f : files) {
        ++total;
        flp::PresetResult r;
        if (!flp::importFlPreset(f.string(), r, fl) || r.state.type != "gl.matrixfm") {
          std::printf("# not decoded: %s\n", f.filename().string().c_str());
          continue;
        }
        ++matrix;
        PatchData pd;
        parsePatch(r.state.data, pd);
        for (int u = 0; u < kUnits; ++u)
          for (int a = 0; a < unitArts(u); ++a) {
            if (get(r.state, art(u, a, AfEnvOn)) != 1) continue;
            const Curve& e = pd.arts[static_cast<size_t>(u)][static_cast<size_t>(a)].env;
            loops += e.loopStart >= 0;
            tempoEnvs += e.tempo;
            decayPts += e.decay > 0;
            susNotDecayPlus1 += e.sustain >= 0 && e.sustain != e.decay + 1;
            tempoLfos += get(r.state, art(u, a, AfLfoOn)) == 1 && get(r.state, art(u, a, AfTempo)) == 1;
            bool gate = u < kOps && a == OaVol && e.loopStart >= 0 && get(r.state, fm(u, kColOut)) != 0;
            for (const Pt& p : e.pts) gate = gate && (p.y == 0.f || p.y == 1.f);
            if (gate) gates.push_back({f.string(), u});
          }
        Out o = render(r.state, {{0, true, 1, 60, 0.8f}, {6000, false, 1, 0, 0}}, 11025, 512);
        finite += o.finite && o.peak < 50;
        sounding += o.peak > 1e-3f;
        if (!o.finite || o.peak >= 50) std::printf("# unstable: %s (peak %g)\n", f.filename().string().c_str(), o.peak);
        else if (o.peak <= 1e-3f) std::printf("# silent: %s\n", f.filename().string().c_str());
      }
      std::printf("# Sytrus presets: %d files, %d -> Matrix FM, %d finite, %d sounding\n", total, matrix, finite, sounding);
      std::printf("# enabled envelopes: %d looping, %d tempo, %d with a decay point, %d sustain != decay + 1; %d tempo LFOs\n", loops,
                  tempoEnvs, decayPts, susNotDecayPlus1, tempoLfos);
      expect(total > 0 && matrix == total, "every shipped Sytrus preset decodes to Matrix FM");
      expect(finite == matrix, "every decoded preset renders finite");
      expect(sounding >= matrix - 20, "decoded presets sound (only X/Y-gated ones stay quiet at the default X/Y)");
      expect(loops >= 50 && tempoEnvs >= 20 && susNotDecayPlus1 >= 100 && tempoLfos >= 20,
             "shipped envelopes keep their loops, tempo flags and sustain points");
      // A shipped preset whose operator volume is a looping on/off gate keeps gating while the note is held.
      if (!gates.empty()) {
        flp::PresetResult r;
        flp::importFlPreset(gates.front().first, r, fl);
        PluginState s = r.state;
        for (int k = 0; k < kOps; ++k)  // listen to the gated operator alone
          if (k != gates.front().second) set(s, fm(k, kColOut), 0);
        for (int j = 0; j < kFilters; ++j) set(s, fm(kOps + j, kColOut), 0), set(s, fm(kOps + j, kColFx), 0);
        Out o = render(s, {{0, true, 1, 60, 0.8f}}, 44100 * 3, 512);
        int on = 0, off = 0, flips = 0;
        bool prev = false;
        const float top = peakIn(o, 44100, 44100 * 3);  // steady part: after the first second
        for (int w = 44100; w + 441 <= 44100 * 3; w += 441) {
          const float p = peakIn(o, w, w + 441);
          const bool hi = p > 0.5f * top;
          on += hi, off += p < 0.35f * top;
          flips += w > 44100 && hi != prev;
          prev = hi;
        }
        std::printf("# looping gate check: %s (operator %d): %d on / %d off windows, %d flips\n",
                    fs::path(gates.front().first).filename().string().c_str(), gates.front().second + 1, on, off, flips);
        expect(on >= 10 && off >= 10 && flips >= 6, "shipped looping envelope keeps gating after its first pass");
      }
    } else {
      std::printf("# FL Studio not installed: Sytrus preset checks skipped\n");
    }
  }
  return finish();
}

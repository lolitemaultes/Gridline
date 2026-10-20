// Gridline — J-32: level laws and loudness fixes found by the QA sweep.
//   * Matrix FM: plucked operators ring from one excitation period (no energy
//     pile-up), the default waveshaper transfer stops at full scale.
//   * Maximus import: version-2 layout (six globals), band modes (compression
//     off keeps gains + saturation, a band switched off merges into the mid
//     band / passes the master stage), curve records found as the chain that
//     ends the state, no oversampling (the ceiling holds).
//   * Mastering Dynamics engine: "no compression" and "merged" band modes.
//   * Live mode (performance zone): offline renders play the launched clips
//     from the song start / the arrangement after the live area, or the live
//     area as arranged when nothing is launched; launch state import + JSON.
//   * FL paths: %FLStudioData% expands to the installation's Data folder.
// All inputs are synthetic (no FL Studio content needed).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/Json.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Project.h"
#include "../src/core/flp/Flp.h"
#include "../src/engine/Effects.h"
#include "../src/engine/Generators.h"
#include "../src/engine/LiveRender.h"
#include "../src/engine/Render.h"
#include "../src/packs/Packs.h"
#include "../src/packs/dynamics/dynamics.h"
#include "../src/packs/synths/matrixfm.h"
#include "Check.h"

using namespace gl;
namespace mf = gl::packs::synths::mfm;
namespace dy = gl::packs::dynamics;

namespace {

// ------------------------------------------------------------------ helpers
float prm(const PluginState& s, int i) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : -999.f; }
void setp(PluginState& s, int i, float v) {
  if (i >= static_cast<int>(s.params.size())) s.params.resize(static_cast<size_t>(i) + 1, 0.f);
  s.params[static_cast<size_t>(i)] = v;
}

struct Lv {
  float peak = 0, early = 0, late = 0;
  bool finite = true;
};
// One note (C5) for 1.5 s at full velocity; peak overall, in the first and the last 0.25 s.
Lv playNote(const PluginState& st) {
  Lv o;
  auto g = packs::createGenerator(st.type);
  if (!g) return {-1, -1, -1, false};
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  const int total = 44100 * 3 / 2, block = 256;
  std::vector<float> L(block), R(block);
  for (int pos = 0; pos < total; pos += block) {
    std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
    if (pos == 0) g->noteOn(1, 60, 1.0f, 0);
    g->process(st, aux, ctx, L.data(), R.data(), block);
    for (int i = 0; i < block; ++i) {
      const float a = std::max(std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)]));
      if (!std::isfinite(a)) o.finite = false;
      o.peak = std::max(o.peak, a);
      if (pos + i < 11025) o.early = std::max(o.early, a);
      if (pos + i >= total - 11025) o.late = std::max(o.late, a);
    }
  }
  return o;
}

// Peak of a test signal (decaying 55 Hz kick + noise, `db` relative to full
// scale) through one effect state, after the first 0.2 s.
double fxPeak(const PluginState& st, double db, double sineHz = 0) {
  auto fx = createEffect(st.type);
  if (!fx) return -1;
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 120;
  const int block = 256;
  std::vector<float> L(block), R(block);
  const double amp = std::pow(10.0, db / 20.0);
  uint32_t rnd = 1;
  double peak = 0;
  for (int b = 0; b < 44100 * 2 / block; ++b) {
    for (int i = 0; i < block; ++i) {
      const double t = (b * block + i) / 44100.0;
      double x;
      if (sineHz > 0) {
        x = amp * std::sin(2 * 3.14159265358979 * sineHz * t);
      } else {
        rnd = rnd * 1664525u + 1013904223u;
        const double kick = std::exp(-std::fmod(t, 0.5) * 12) * std::sin(2 * 3.14159265358979 * 55 * t);
        x = amp * (0.6 * kick + 0.4 * ((rnd >> 8) / 8388608.0 - 1.0));
      }
      L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(x);
    }
    fx->process(st, ctx, L.data(), R.data(), block);
    if (b * block < 8820) continue;
    for (int i = 0; i < block; ++i) peak = std::max(peak, static_cast<double>(std::max(std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)]))));
  }
  return peak;
}

// Little-endian byte builder.
struct Bin {
  std::vector<uint8_t> b;
  Bin& u8(uint8_t v) { return b.push_back(v), *this; }
  Bin& u16(uint16_t v) { return u8(static_cast<uint8_t>(v & 0xFF)).u8(static_cast<uint8_t>(v >> 8)); }
  Bin& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
  Bin& f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return i32(static_cast<int32_t>(u));
  }
  Bin& f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(u >> (8 * i)));
    return *this;
  }
  Bin& zeros(size_t n) { return b.insert(b.end(), n, 0), *this; }
};

// Maximus state: version, globals (6 for version 2, else 7), four band blocks
// of 13 ints, filler (spare state / options), then eight curve records.
struct MaxBand {
  int v[13];
};
std::vector<uint8_t> maximusState(int version, const std::vector<int>& globals, const MaxBand bands[4], bool decoy,
                                  const std::vector<std::vector<double>>& band0Curve) {
  Bin s;
  s.u16(static_cast<uint16_t>(version)).u16(15);
  for (int g : globals) s.i32(g);
  for (int k = 0; k < 4; ++k)
    for (int f = 0; f < 13; ++f) s.i32(bands[k].v[f]);
  s.zeros(120);
  if (decoy) {  // looks like a 2-point record whose points overlap the real first record
    s.i32(1).i32(2).i32(2);
    s.zeros(4);
  }
  for (int r = 0; r < 8; ++r) {
    const std::vector<std::vector<double>> def = {{0, 0}, {0.5, 0.5}, {0.5, 0.5}};  // limiter at 0 dB (dx, y)
    const auto& pts = r == 0 && !band0Curve.empty() ? band0Curve : def;
    s.i32(3).i32(3).i32(static_cast<int32_t>(pts.size()));
    for (const auto& p : pts) s.f64(p[0]).f64(p[1]).f32(0).i32(0);
    s.i32(0).i32(-1).i32(-1).i32(-1).i32(-1);
  }
  return s.b;
}
std::string name16(const std::string& s) {
  std::string o;
  for (char ch : s) o += ch, o += '\0';
  return o + std::string(2, '\0');
}

bool decode(const std::string& fl, const std::vector<uint8_t>& state, flp::PluginMapping& m) {
  static flp::File file;
  static Song song;
  static flp::Report rep;
  static flp::Context ctx{file, song, rep};
  const std::string n = name16(fl);
  std::vector<flp::Event> evs(2);
  evs[0].id = 201, evs[0].data = reinterpret_cast<const uint8_t*>(n.data()), evs[0].size = static_cast<uint32_t>(n.size());
  evs[1].id = 213, evs[1].data = state.data(), evs[1].size = static_cast<uint32_t>(state.size());
  auto it = flp::decoders().find(fl);
  if (it == flp::decoders().end() || !it->second(ctx, evs, m)) return false;
  normalizePluginState(m.state);
  return true;
}

// ---------------------------------------------------------------- live songs
// One Matrix FM channel playing a held C5 in every pattern; clips as given
// (track, start, length, launched); live area end `zone`.
struct ClipSpec {
  int track;
  Tick start, length;
  bool launched;
};
Song liveSong(Tick zone, const std::vector<ClipSpec>& clips) {
  Song s;
  s.tempo = 120;
  s.ppq = 96;
  ensureMixer(s);
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Tone";
  ch.plugin = defaultPluginState("gl.matrixfm");
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  Pattern p;
  p.id = s.allocateId();
  Note n;
  n.start = 0, n.length = 384, n.pitch = 72, n.velocity = 100;
  p.notes[ch.id].push_back(n);
  s.patterns.push_back(p);
  for (const auto& c : clips) {
    Clip k;
    k.id = s.allocateId(), k.pattern = p.id, k.track = c.track, k.start = c.start, k.length = c.length, k.launched = c.launched;
    s.playlist.push_back(k);
  }
  while (s.tracks.size() < 4) s.tracks.push_back(PlaylistTrack{});
  s.live.areaEnd = zone;
  return s;
}
double bufPeak(const AudioBuffer& b) {
  double p = 0;
  for (float v : b.data) p = std::max(p, static_cast<double>(std::fabs(v)));
  return p;
}
double secondsOf(const AudioBuffer& b) { return static_cast<double>(b.frames) / 44100.0; }

// Synthetic FLP builder (event stream).
struct Flp {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(v >> 8); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int k = 0; k < 4; ++k) ev.push_back(static_cast<uint8_t>(v >> (8 * k)));
  }
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  std::vector<uint8_t> file(int ppq) const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(ppq), 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int k = 0; k < 4; ++k) f.push_back(static_cast<uint8_t>(n >> (8 * k)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};
std::vector<uint8_t> playlistItem(uint32_t pos, uint32_t len, int track, bool muted) {
  std::vector<uint8_t> it(60, 0);
  auto put32 = [&](size_t at, uint32_t v) {
    for (int k = 0; k < 4; ++k) it[at + static_cast<size_t>(k)] = static_cast<uint8_t>(v >> (8 * k));
  };
  put32(0, pos);
  it[4] = 0x00, it[5] = 0x50;  // pattern base
  it[6] = 0x01, it[7] = 0x50;  // pattern 1
  put32(8, len);
  const uint16_t trk = static_cast<uint16_t>(499 - track);
  it[12] = static_cast<uint8_t>(trk & 0xFF), it[13] = static_cast<uint8_t>(trk >> 8);
  if (muted) it[19] = 0x20;  // flags 0x2000
  return it;
}

}  // namespace

int main() {
  // ================================================================ Matrix FM
  {
    PluginState plain = defaultPluginState("gl.matrixfm");  // operator 1 (sine) to the output
    PluginState pluck = plain;
    setp(pluck, mf::op(0, mf::OpPluck), 1);
    const Lv a = playNote(plain), b = playNote(pluck);
    expect(a.finite && b.finite && a.peak > 0.05f, "matrix fm: plain and plucked operators sound");
    expect(b.peak <= a.peak * 1.05f, "matrix fm: a plucked operator is no louder than its waveform (one excitation period)");
    expect(b.early > 0.05f && b.late < b.early * 0.8f, "matrix fm: the pluck rings down");

    // Four operators summed into filter 1 (wide open), waveshaper on with the
    // default straight curve and the pre-amp at its maximum.
    PluginState ws = plain;
    setp(ws, mf::fm(0, mf::kColOut), 0);
    for (int k = 0; k < 4; ++k) setp(ws, mf::fm(6, k), 1), setp(ws, mf::op(k, mf::OpVol), 1);
    setp(ws, mf::fm(6, mf::kColOut), 1);
    setp(ws, mf::flt(0, mf::FlMode), mf::FmX1);
    setp(ws, mf::flt(0, mf::FlType), mf::FtLp);
    setp(ws, mf::flt(0, mf::FlCut), 1);
    setp(ws, mf::flt(0, mf::FlRes), 0);
    PluginState wsOff = ws;
    setp(ws, mf::flt(0, mf::FlWsOn), 1);
    setp(ws, mf::flt(0, mf::FlWsAmp), 1.5f);
    setp(ws, mf::flt(0, mf::FlWsMix), 1);
    const Lv on = playNote(ws), off = playNote(wsOff);
    expect(off.peak > 1.0f, "matrix fm: four summed operators exceed full scale before the shaper");
    expect(on.finite && on.peak > 0.2f && on.peak <= 0.51f, "matrix fm: the default waveshaper curve stops at full scale (pre-amp drives into it)");
  }

  // ================================================================== Maximus
  {
    // Band blocks: mode, pre, post, separation, sat threshold, sat ceiling, attack,
    // release 2, attack curve, release, release curve, sustain window, env mode.
    const MaxBand bands[4] = {{{3, 1000, 1000, 0, 0, 1000, 1115, 0, 2, 5000, 3, 2882, 0}},     // low: off → merged
                              {{0, 1480, 852, 0, 0, 1000, 1115, 0, 2, 5000, 2, 2015, 1}},      // mid: compress
                              {{1, 1404, 852, 0, 0, 1000, 1115, 0, 2, 5000, 2, 1115, 1}},      // high: compression off
                              {{1, 1000, 1000, 0, -12, 976, 1115, 8096, 2, 1120, 3, 1115, 0}}};  // master: compression off + saturation
    const std::vector<std::vector<double>> curve = {{0, 0}, {0.4, 0.4}, {0.6, 0.3}};
    flp::PluginMapping m;
    const auto st3 = maximusState(3, {19800, 1, 48065, 1, 1115, 1000, 32768}, bands, true, curve);
    expect(decode("Maximus", st3, m) && m.glType == "gl.master", "maximus v3 decodes to gl.master");
    const PluginState& s = m.state;
    expect(prm(s, dy::msParam(0, dy::MbMode)) == 4 && prm(s, dy::msParam(1, dy::MbMode)) == 1 && prm(s, dy::msParam(2, dy::MbMode)) == 3 &&
               prm(s, dy::msParam(3, dy::MbMode)) == 3,
           "maximus band modes: off low → merged, compress → active, compression off → no compression (master too)");
    expect(prm(s, dy::MsOversample) == 0, "maximus: no oversampling (saturation at the base rate)");
    expect(std::fabs(prm(s, dy::msParam(3, dy::MbSatCeil)) - 20 * std::log10((std::pow(7.0, 0.976) - 1) / 6)) < 0.01, "maximus: master ceiling law");
    expect(prm(s, dy::msExt(0, dy::MxPoints)) == 3 && std::fabs(prm(s, dy::msExt(0, dy::MxPt0 + 3)) - 0.4f) < 1e-4 &&
               std::fabs(prm(s, dy::msExt(0, dy::MxPt0 + 7)) - 0.3f) < 1e-4,
           "maximus: curve records found as the chain ending the state (decoy before them ignored)");
    expect(prm(s, dy::msExt(1, dy::MxPoints)) == 3 && std::fabs(prm(s, dy::msExt(1, dy::MxPt0 + 4)) - 0.5f) < 1e-4, "maximus: the next band's curve follows");
    // The master stage holds its ceiling with compression off (saturation only).
    const double ceil = std::pow(10.0, prm(s, dy::msParam(3, dy::MbSatCeil)) / 20.0);
    const double p0 = fxPeak(s, 0), p12 = fxPeak(s, 12);
    expect(p0 > 0.3 && p12 <= ceil * 1.001, "maximus → gl.master: output never above the master ceiling (+12 dB in)");

    // Version 2: six globals (no low cut), bands follow directly.
    flp::PluginMapping m2;
    const MaxBand b2[4] = {{{3, 1000, 1000, 0, 0, 1000, 1115, 0, 2, 5000, 3, 2882, 0}},
                           {{1, 1000, 1000, 0, 0, 1000, 1115, 0, 2, 5000, 3, 2882, 0}},
                           {{3, 1000, 1000, 0, 0, 1000, 1115, 0, 2, 5000, 3, 2882, 0}},
                           {{0, 1520, 1000, 0, 0, 1000, 1115, 6304, 2, 6424, 4, 2882, 0}}};
    const auto st2 = maximusState(2, {19800, 1, 48065, 1, 1115, 1000}, b2, false, {});
    expect(decode("Maximus", st2, m2), "maximus v2 decodes");
    expect(prm(m2.state, dy::msParam(0, dy::MbMode)) == 4 && prm(m2.state, dy::msParam(1, dy::MbMode)) == 3 &&
               prm(m2.state, dy::msParam(2, dy::MbMode)) == 4 && prm(m2.state, dy::msParam(3, dy::MbMode)) == 1,
           "maximus v2: six globals, band modes in place (one-band limiter: master active)");
    expect(prm(m2.state, dy::MsLowCut) <= 10.5f, "maximus v2: no low cut");
    expect(std::fabs(prm(m2.state, dy::msParam(3, dy::MbPre)) - 20 * std::log10((std::pow(7.0, 1.52) - 1) / 6)) < 0.01,
           "maximus v2: master pre gain read from its own field");

    // Master band off (reference mode 3) passes through.
    MaxBand b3[4] = {bands[0], bands[1], bands[2], bands[3]};
    b3[3].v[0] = 3;
    flp::PluginMapping m3;
    expect(decode("Maximus", maximusState(3, {19800, 1, 48065, 1, 1115, 1000, 32768}, b3, false, {}), m3) &&
               prm(m3.state, dy::msParam(3, dy::MbMode)) == 0,
           "maximus: master stage switched off → bypass");
  }

  // ============================================== Mastering Dynamics band modes
  {
    PluginState st = defaultPluginState("gl.master");
    for (int b = 0; b < 4; ++b) setp(st, dy::msParam(b, dy::MbMode), 0);
    setp(st, dy::MsLowFreq, 200), setp(st, dy::MsHighFreq, 3000);
    // Low merged into a muted mid band: a 50 Hz tone disappears.
    setp(st, dy::msParam(0, dy::MbMode), 4);
    setp(st, dy::msParam(1, dy::MbMode), 2);
    const double merged = fxPeak(st, -6, 50);
    setp(st, dy::msParam(0, dy::MbMode), 0);
    const double alone = fxPeak(st, -6, 50);
    expect(merged < 0.02 && alone > 0.3, "gl.master: a merged low band follows the mid band (muted mid mutes it)");
    // No compression: the gain stages still apply (+6 dB post on the master stage).
    PluginState nc = defaultPluginState("gl.master");
    for (int b = 0; b < 3; ++b) setp(nc, dy::msParam(b, dy::MbMode), 0);
    setp(nc, dy::msParam(3, dy::MbMode), 3);
    setp(nc, dy::msParam(3, dy::MbPost), 6);
    setp(nc, dy::msParam(3, dy::MbSatCeil), 12);
    setp(nc, dy::msParam(3, dy::MbSatThresh), 0);
    setp(nc, dy::MsOversample, 0);
    const double p = fxPeak(nc, -20, 1000);
    expect(std::fabs(p - 0.1 * std::pow(10.0, 6.0 / 20.0)) < 0.01, "gl.master: no-compression band applies its gains, no gain reduction");
  }

  // =================================================================== live
  {
    RenderOptions o;
    o.tail = 0;
    // Pure performance set, one launched clip: plays from the live area's end for its length.
    Song a = liveSong(768, {{0, 0, 384, true}, {0, 384, 384, false}, {1, 0, 384, false}});
    Song ta = a;
    const Tick startA = prepareOfflineLive(ta);
    size_t launchedCopies = 0;
    for (const Clip& c : ta.playlist) launchedCopies += c.start >= 768 && c.track == 0;
    expect(startA == 768 && ta.live.areaEnd == 0 && launchedCopies == 2 && ta.lengthTicks() == 1536,
           "live: launched clip loops from the live area's end for the live area's length");
    const AudioBuffer ba = renderSong(a, o);
    expect(bufPeak(ba) > 0.05 && std::fabs(secondsOf(ba) - 4.0) < 0.1, "live: offline render of a performance set with a launched clip is not silent");
    // Nothing launched: the live area plays as arranged.
    Song b = liveSong(768, {{0, 0, 384, false}, {0, 384, 384, false}});
    Song tb = b;
    expect(prepareOfflineLive(tb) == 0 && tb.live.areaEnd == 0 && tb.playlist.size() == 2, "live: nothing launched → the live area plays as arranged");
    expect(bufPeak(renderSong(b, o)) > 0.05, "live: such a render is not silent");
    // Arrangement after the live area + a launched clip: render starts at the live area's end.
    Song c = liveSong(768, {{0, 0, 384, true}, {1, 768, 768, false}, {1, 0, 384, false}});
    Song tc = c;
    expect(prepareOfflineLive(tc) == 768 && tc.lengthTicks() == 1536, "live: arranged song after the live area keeps its length");
    RenderJob job;
    job.tail = TailMode::Cut;
    const RenderResult rc = renderAudio(c, job);
    expect(rc.error.empty() && rc.start == 768 && std::fabs(static_cast<double>(rc.audio.frames) / 44100.0 - 4.0) < 0.05, "live: renderAudio starts at the song start marker");
    Tick s0 = 0, e0 = 0;
    expect(renderScopeTicks(c, job, s0, e0) && s0 == 768 && e0 == 1536, "live: render scope (export dialog) matches");
    double pk = 0;
    for (size_t k = 0; k < rc.audio.frames; ++k) pk = std::max(pk, static_cast<double>(std::fabs(rc.audio.plane(0)[k])));
    expect(pk > 0.05, "live: arranged + launched clips sound");
    // Songs without a live area are unchanged.
    Song d = liveSong(0, {{0, 0, 384, false}});
    Song td = d;
    expect(prepareOfflineLive(td) == 0 && td.playlist == d.playlist, "live: no live area → untouched");
    // Follow mode "once": the launched clip plays one time.
    Song e = liveSong(768, {{0, 0, 384, true}});
    e.tracks[0].follow = FollowMode::Once;
    Song te = e;
    prepareOfflineLive(te);
    expect(te.playlist.size() == 1 && te.playlist[0].start == 768, "live: follow mode once → one pass");
    // Next: cycles through the track's live clips.
    Song f = liveSong(1152, {{0, 0, 384, true}, {0, 384, 384, false}, {0, 768, 384, false}});
    f.tracks[0].follow = FollowMode::Next;
    Song tf = f;
    prepareOfflineLive(tf);
    bool order = tf.playlist.size() == 3;
    for (size_t k = 0; order && k < 3; ++k) order = tf.playlist[k].start == 1152 + static_cast<Tick>(k) * 384;
    expect(order, "live: follow mode next → the track's clips in order");

    // JSON round trip of the launch state.
    Song j1;
    expect(songFromJson(songToJson(a), j1) && j1.playlist.size() == a.playlist.size() && j1.playlist[0].launched && !j1.playlist[1].launched &&
               j1.live.areaEnd == 768,
           "live: launch state survives save / load");
  }

  // ============================================== FL import: performance zone
  {
    Flp B;
    B.text(199, "25.2.3.5164");
    B.d(156, 120000);
    B.w(65, 1);  // pattern 1
    std::vector<uint8_t> note(24, 0);
    note[4] = 0x00, note[5] = 0x40, note[8] = 96, note[12] = 60, note[16] = 120, note[18] = 64, note[20] = 64, note[21] = 100, note[22] = 128, note[23] = 128;
    B.data(224, note);
    B.w(64, 0), B.b(21, 2), B.text(192, "Lead");
    B.w(99, 0);  // arrangement 0
    std::vector<uint8_t> items;
    for (auto it : {playlistItem(0, 384, 0, false), playlistItem(384, 384, 0, true), playlistItem(0, 384, 1, true)}) items.insert(items.end(), it.begin(), it.end());
    B.data(233, items);
    B.d(148, (5u << 24) | 768u);  // song start marker → performance zone [0, 768)
    const std::string path = "build_j32_live.flp";
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
      auto bytes = B.file(96);
      std::fwrite(bytes.data(), 1, bytes.size(), fp);
      std::fclose(fp);
    }
    Song s;
    flp::Report rep;
    std::string err;
    const bool ok = importFlp(path, s, rep, &err, "/nonexistent");
    std::remove(path.c_str());
    int launched = 0, muted = 0;
    for (const Clip& c : s.playlist) launched += c.launched, muted += c.muted;
    expect(ok && s.live.areaEnd == 768 && s.playlist.size() == 3, "fl import: performance zone → live area");
    expect(launched == 1 && muted == 0 && s.playlist.size() == 3 && [&] {
      for (const Clip& c : s.playlist)
        if (c.launched) return c.start == 0 && c.track == 0;
      return false;
    }(),
           "fl import: the unmuted zone clip is the launched one, zone mutes become launch state");
  }

  // ======================= FL import: default sends 100–103 without stored levels
  {
    // Old-layout project (127 inserts written): every insert routes to the master
    // and to the default send inserts 100–103, levels not stored (FL's render tests).
    Flp B;
    B.text(199, "20.8.0.2115");
    B.d(156, 120000);
    B.w(65, 1);
    std::vector<uint8_t> note(24, 0);
    note[4] = 0x00, note[5] = 0x40, note[8] = 96, note[12] = 60, note[16] = 120, note[18] = 64, note[20] = 64, note[21] = 100, note[22] = 128, note[23] = 128;
    B.data(224, note);
    B.w(64, 0), B.b(21, 2), B.text(192, "Lead"), B.b(22, 1);
    for (int k = 0; k < 127; ++k) {
      B.data(236, std::vector<uint8_t>(12, 0));
      for (int sl = 0; sl < 10; ++sl) B.w(98, static_cast<uint16_t>(sl));
      std::vector<uint8_t> route(127, 0);
      if (k > 0) {
        route[0] = 1;
        for (int d = 100; d <= 103; ++d)
          if (d != k) route[static_cast<size_t>(d)] = 1;
      }
      B.data(235, route);
      B.d(147, 0);
    }
    const std::string path = "build_j32_sends.flp";
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
      auto bytes = B.file(96);
      std::fwrite(bytes.data(), 1, bytes.size(), fp);
      std::fclose(fp);
    }
    Song s;
    flp::Report rep;
    std::string err;
    const bool ok = importFlp(path, s, rep, &err, "/nonexistent");
    std::remove(path.c_str());
    const MixerTrack* t1 = s.mixer.size() > 1 ? &s.mixer[1] : nullptr;
    int audible = 0;
    if (t1)
      for (const auto& sd : t1->sends) audible += sd.level > 0 && !sd.sidechain;
    expect(ok && t1 && audible == 1, "fl import: default sends 100–103 without stored levels carry nothing (insert feeds the master once)");
  }

  // ================================================ FL paths: %FLStudioData%
  {
    flp::File file;
    Song song;
    flp::Report rep;
    flp::Context ctx{file, song, rep};
    ctx.factoryData = "/opt/fl";
    ctx.projectDir = "/home/u/proj";
    expect(ctx.resolvePath("%FLStudioData%\\Patches\\Misc\\a.wav") == "/opt/fl/Data/Patches/Misc/a.wav", "fl paths: %FLStudioData% → <FL>/Data");
    expect(ctx.resolvePath("%FLStudioFactoryData%/x.wav") == "/opt/fl/x.wav" && ctx.resolvePath("rel/y.wav") == "/home/u/proj/rel/y.wav",
           "fl paths: factory data and relative paths unchanged");
  }
  return finish();
}

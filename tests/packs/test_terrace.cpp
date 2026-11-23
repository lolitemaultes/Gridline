// Gridline — Terrace (gl.terrace), the multiband rhythmic gate: the crossover's bands add up flat
// (3 and 4 bands, every slope, the dry path, the whole effect at rest); lanes play on their steps;
// chance repeats exactly; the sidechain (level and notes, in the effect and through the project's
// routing); routes; patterns, the chain and note picks; the step bank text and the project save
// and load; block sizes; extreme settings stay finite.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/AudioBuffer.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "engine/Effects.h"
#include "engine/Generators.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "packs/terrace/terrace.h"

using namespace gl;
namespace tr = gl::packs::terrace;

namespace {

constexpr double kSr = 48000, kTempo = 120;  // one beat = 24000 samples; a 1/16 step = 6000
constexpr long kBeat = 24000;

struct Out {
  std::vector<float> L, R;
};

struct RunOpts {
  bool playing = true;
  int block = 256;
  std::function<void(size_t, int, NoteHitBus&)> hits;  // the block's notes
  const std::vector<float>* sc = nullptr;              // the sidechain input
};

Out render(const PluginState& st, const std::vector<float>& in, const RunOpts& o = {}) {
  auto fx = createEffect(tr::kType);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = kTempo;
  ctx.playing = o.playing;
  NoteHitBus bus;
  Out out{in, in};
  for (size_t at = 0; at < in.size(); at += static_cast<size_t>(o.block)) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(o.block), in.size() - at));
    ctx.tick = static_cast<double>(at) / (kSr * 60.0 / kTempo) * ctx.ppq;
    ctx.jumped = at == 0;
    bus.clear();
    if (o.hits) o.hits(at, n, bus);
    ctx.hits = &bus;
    SideIo io;
    if (o.sc) io.scL = io.scR = o.sc->data() + at;
    fx->setSideIo(io);
    fx->process(st, ctx, out.L.data() + at, out.R.data() + at, n);
  }
  return out;
}

std::vector<float> noise(size_t n, uint32_t seed = 1) {
  std::vector<float> v(n);
  for (float& x : v) seed = seed * 1664525u + 1013904223u, x = static_cast<float>(static_cast<int32_t>(seed)) / 4.3e9f;
  return v;
}
uint32_t rnd(uint32_t& r) { return r = r * 1664525u + 1013904223u, r >> 8; }

// |H(f)| in dB of an impulse response.
double magDb(const std::vector<double>& h, double f) {
  std::complex<double> s = 0;
  const double w = -2.0 * 3.14159265358979323846 * f / kSr;
  for (size_t i = 0; i < h.size(); ++i) s += h[i] * std::polar(1.0, w * static_cast<double>(i));
  return 20.0 * std::log10(std::max(1e-12, std::abs(s)));
}

PluginState gate() {
  PluginState st = defaultPluginState(tr::kType);
  st.params[tr::TrSmooth] = 0.5f;  // levels settle within a few ms
  return st;
}

void editBank(PluginState& st, const std::function<void(tr::Bank&)>& edit) {
  auto b = std::make_unique<tr::Bank>();
  tr::parse(st.data.c_str(), *b);
  edit(*b);
  tr::finish(*b);
  st.data = tr::withBank(st.data, *b);
}

double at(const Out& o, long i) { return o.L[static_cast<size_t>(i)]; }

// ------------------------------------------------------------------ 1. the crossover

void testCrossover() {
  const double splits[3] = {120, 900, 5000};
  const char* slopeName[3] = {"12 dB", "24 dB", "48 dB"};
  for (int bands : {3, 4})
    for (int slope = 0; slope < 3; ++slope) {
      tr::Crossover xo, dryXo;
      xo.set(kSr, bands, slope, splits);
      dryXo.set(kSr, bands, slope, splits);
      const size_t n = 16384;
      std::vector<double> sum(n), dry(n), band0(n), bandLast(n);
      double out[4];
      double worstDiff = 0;
      for (size_t i = 0; i < n; ++i) {
        const double x = i == 0 ? 1.0 : 0.0;
        xo.split(x, out);
        double s = 0;
        for (int b = 0; b < bands; ++b) s += out[b];
        sum[i] = s, dry[i] = dryXo.dry(x), band0[i] = out[0], bandLast[i] = out[bands - 1];
        worstDiff = std::max(worstDiff, std::fabs(sum[i] - dry[i]));
      }
      double worst = 0;
      for (double f = 20; f < 20000; f *= 1.07) worst = std::max(worst, std::fabs(magDb(sum, f)));
      char msg[160];
      std::snprintf(msg, sizeof msg, "crossover %d bands %s: the bands add up flat (worst %.4f dB)", bands, slopeName[slope], worst);
      expect(worst < 0.01, msg);
      std::snprintf(msg, sizeof msg, "crossover %d bands %s: the dry path is the bands' sum (worst %.2g)", bands, slopeName[slope], worstDiff);
      expect(worstDiff < 1e-9, msg);
      // the bands do split: the low band passes 30 Hz and not 18 kHz, the top band the other way
      const double lo40 = magDb(band0, 30), lo10k = magDb(band0, 18000), hi40 = magDb(bandLast, 30), hi10k = magDb(bandLast, 18000);
      std::snprintf(msg, sizeof msg, "crossover %d bands %s: low band %.1f / %.1f dB, top band %.1f / %.1f dB", bands, slopeName[slope], lo40, lo10k, hi40, hi10k);
      expect(lo40 > -1 && lo10k < -30 && hi40 < -30 && hi10k > -1, msg);
      // at the split each side is -3 dB (12 dB) or -6 dB (24, 48 dB)
      const double at120 = magDb(band0, 120);
      std::snprintf(msg, sizeof msg, "crossover %s: the low band is %.2f dB at its split", slopeName[slope], at120);
      expect(std::fabs(at120 + 6.02) < 0.3, msg);  // Linkwitz-Riley: each side -6 dB at its split
    }
}

// The whole effect at rest: flat, the mix flat too, and the slope steeper as it says.
void testEffectFlat() {
  for (int bands : {0, 1})
    for (int slope = 0; slope < 3; ++slope)
      for (double mix : {1.0, 0.5, 0.0}) {
        PluginState st = defaultPluginState(tr::kType);
        st.params[tr::TrBands] = static_cast<float>(bands);
        st.params[tr::TrSlope] = static_cast<float>(slope);
        st.params[tr::TrMix] = static_cast<float>(mix);
        std::vector<float> imp(16384, 0.f);
        imp[0] = 1;
        const Out o = render(st, imp);
        std::vector<double> h(o.L.begin(), o.L.end());
        double worst = 0;
        for (double f = 20; f < 20000; f *= 1.1) worst = std::max(worst, std::fabs(magDb(h, f)));
        char msg[128];
        std::snprintf(msg, sizeof msg, "the effect at rest is flat: %d bands, slope %d, mix %.1f (worst %.4f dB)", bands ? 4 : 3, slope, mix, worst);
        expect(worst < 0.01, msg);
      }
  // the default state's output equals its dry path sample for sample (mix 1 against mix 0)
  PluginState a = defaultPluginState(tr::kType), b = a;
  b.params[tr::TrMix] = 0;
  const std::vector<float> x = noise(48000);
  const Out oa = render(a, x), ob = render(b, x);
  double d = 0;
  for (size_t i = 0; i < x.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(oa.L[i] - ob.L[i])));
  expect(d < 1e-5, "at rest the wet and the dry are the same signal");
}

// ------------------------------------------------------------------ 2. lanes

void testLaneTiming() {
  PluginState st = gate();
  editBank(st, [](tr::Bank& b) {
    for (int k = 0; k < 16; ++k) b.val[0][0][tr::LnVol][k] = k % 2 ? 0.f : 1.f;
  });
  const std::vector<float> dc(kBeat * 8, 0.5f);
  const Out o = render(st, dc);
  expect(std::fabs(at(o, 3000) - 0.5) < 0.01 && std::fabs(at(o, 9000)) < 0.01 && std::fabs(at(o, 15000) - 0.5) < 0.01,
         "lane: the low band's volume steps play (on, off, on)");
  long edge = -1;
  for (long i = 5000; i < 8000; ++i)
    if (at(o, i) < 0.25) {
      edge = i;
      break;
    }
  char msg[96];
  std::snprintf(msg, sizeof msg, "lane: step 2 starts on its sample (6000, the level halves at %ld)", edge);
  expect(edge >= 6000 && edge <= 6000 + 48, msg);
  // the 16 steps loop each bar: step 0 again at 96000
  expect(std::fabs(at(o, 96000 + 3000) - 0.5) < 0.01 && std::fabs(at(o, 96000 + 9000)) < 0.01, "lane: 16 steps of 1/16 loop each bar");

  // 3 steps: the lane loops every 3 steps (18000 samples)
  PluginState s3 = st;
  s3.params[tr::laneParam(0, tr::LnVol, tr::LpSteps)] = 2;
  const Out o3 = render(s3, dc);
  expect(std::fabs(at(o3, 3000) - 0.5) < 0.01 && std::fabs(at(o3, 9000)) < 0.01 && std::fabs(at(o3, 15000) - 0.5) < 0.01 &&
             std::fabs(at(o3, 21000) - 0.5) < 0.01 && std::fabs(at(o3, 27000)) < 0.01,
         "lane: a 3 step lane loops on its own length");
  // step size 1/8: the steps last 12000 samples
  PluginState s8 = st;
  s8.params[tr::laneParam(0, tr::LnVol, tr::LpSize)] = 5;  // 1/8
  const Out o8 = render(s8, dc);
  expect(std::fabs(at(o8, 6000) - 0.5) < 0.01 && std::fabs(at(o8, 18000)) < 0.01, "lane: step size 1/8");
  // swing pushes the odd steps later
  PluginState sw = st;
  sw.params[tr::laneParam(0, tr::LnVol, tr::LpSwing)] = 0.6f;
  const Out os = render(sw, dc);
  long swEdge = -1;
  for (long i = 5000; i < 11000; ++i)
    if (at(os, i) < 0.25) {
      swEdge = i;
      break;
    }
  std::snprintf(msg, sizeof msg, "lane: swing moves the off step later (%ld)", swEdge);
  expect(swEdge > 6000 + 500, msg);
  // depth 0: the lane does nothing
  PluginState d0 = st;
  d0.params[tr::laneParam(0, tr::LnVol, tr::LpDepth)] = 0;
  const Out od = render(d0, dc);
  expect(std::fabs(at(od, 9000) - 0.5) < 0.01, "lane: depth 0 leaves the band as it is");
  // half depth: the off step plays at half level
  d0.params[tr::laneParam(0, tr::LnVol, tr::LpDepth)] = 0.5f;
  const Out oh = render(d0, dc);
  expect(std::fabs(at(oh, 9000) - 0.25) < 0.01, "lane: depth 0.5 halves the gate");
  // pan lane: hard left on step 0 takes the right side away
  PluginState pn = gate();
  editBank(pn, [](tr::Bank& b) { b.val[0][0][tr::LnPan][0] = -1.f; });
  const Out op = render(pn, dc);
  expect(std::fabs(op.L[3000] - 0.5) < 0.01 && std::fabs(op.R[3000]) < 0.01 && std::fabs(op.R[9000] - 0.5) < 0.01, "lane: pan steps move the band");
  // cutoff lane: a closed step takes the top band's noise down, an open step leaves it
  PluginState ct = gate();
  editBank(ct, [](tr::Bank& b) { b.val[0][3][tr::LnCut][1] = 0.f; });
  const std::vector<float> nz = noise(kBeat * 2, 3);
  const Out oc = render(ct, nz), ref = render(gate(), nz);
  double e0 = 0, e1 = 0, r1 = 0;
  for (long i = 1000; i < 5000; ++i) e0 += std::pow(at(oc, i) - at(ref, i), 2);
  for (long i = 7000; i < 11000; ++i) e1 += std::pow(at(oc, i), 2), r1 += std::pow(at(ref, i), 2);
  expect(e0 < 1e-9 && e1 < r1 * 0.85, "lane: a closed cutoff step filters its band, open steps are no filter");
  // solo and mute
  PluginState so = gate();
  so.params[tr::bandParam(3, tr::BpSolo)] = 1;
  const Out oso = render(so, dc);
  expect(std::fabs(at(oso, 20000)) < 0.001, "solo the top band: the low band's DC is gone");
  PluginState mu = gate();
  mu.params[tr::bandParam(0, tr::BpMute)] = 1;
  expect(std::fabs(at(render(mu, dc), 20000)) < 0.001, "mute the low band");
  PluginState gn = gate();
  gn.params[tr::bandParam(0, tr::BpGain)] = -6;
  expect(std::fabs(at(render(gn, dc), 20000) - 0.5 * std::pow(10.0, -6.0 / 20)) < 0.003, "band gain");
}

// ------------------------------------------------------------------ 3. chance

std::vector<int> stepsPlayed(const Out& o, int steps) {
  std::vector<int> v;
  for (int k = 0; k < steps; ++k) v.push_back(at(o, k * 6000 + 3000) > 0.25 ? 1 : 0);
  return v;
}

void testChance() {
  PluginState st = gate();
  editBank(st, [](tr::Bank& b) {
    for (int k = 0; k < 16; ++k) b.chance[0][0][tr::LnVol][k] = 50;
  });
  const std::vector<float> dc(kBeat * 16, 0.5f);
  RunOpts a, b;
  a.block = 256, b.block = 999;
  const std::vector<int> pa = stepsPlayed(render(st, dc, a), 64), pb = stepsPlayed(render(st, dc, b), 64);
  int on = 0;
  for (int x : pa) on += x;
  char msg[96];
  std::snprintf(msg, sizeof msg, "chance 50%%: %d of 64 steps play", on);
  expect(on > 16 && on < 48, msg);
  expect(pa == pb, "chance: the same steps play again (fresh instance, other block size)");
  // the dice change from bar to bar (not the same 16 every bar)
  bool differs = false;
  for (int k = 0; k < 16; ++k) differs = differs || pa[static_cast<size_t>(k)] != pa[static_cast<size_t>(k + 16)];
  expect(differs, "chance: each bar rolls its own dice");
  // the dice of band 1 differ from band 0 (seeded per band and lane)
  PluginState s1 = gate();
  editBank(s1, [](tr::Bank& bk) {
    for (int k = 0; k < 16; ++k) bk.chance[0][0][tr::LnVol][k] = 50, bk.chance[0][1][tr::LnVol][k] = 50;
  });
  expect(stepsPlayed(render(s1, dc), 64) == pa, "chance: another band's dice leave this band's alone");
  // repeats: 2 hits split the step, each hit holding the level for its first half
  PluginState rp = gate();
  editBank(rp, [](tr::Bank& bk) { bk.hits[0][0][tr::LnVol][0] = 2; });
  const Out orp = render(rp, dc);
  expect(std::fabs(at(orp, 750) - 0.5) < 0.01 && std::fabs(at(orp, 2250)) < 0.01 && std::fabs(at(orp, 3750) - 0.5) < 0.01 &&
             std::fabs(at(orp, 5250)) < 0.01 && std::fabs(at(orp, 9000) - 0.5) < 0.01,
         "repeats: a step of 2 hits");
}

// ------------------------------------------------------------------ 4. sidechain

void testSidechainDsp() {
  PluginState st = gate();
  st.params[tr::routeParam(0, tr::RtSource)] = tr::SrcSidechain;
  st.params[tr::routeParam(0, tr::RtTarget)] = tr::TgOutput;
  st.params[tr::routeParam(0, tr::RtAmount)] = -1;
  st.params[tr::TrScRelease] = 5;
  const std::vector<float> dc(kBeat * 3, 0.5f);
  std::vector<float> sc(dc.size(), 0.f);
  for (long i = kBeat; i < 2 * kBeat; ++i) sc[static_cast<size_t>(i)] = 1.f;
  RunOpts o;
  o.sc = &sc;
  const Out out = render(st, dc, o);
  const double duck = 0.5 * std::pow(10.0, -24.0 / 20);
  expect(std::fabs(at(out, kBeat / 2) - 0.5) < 0.005 && std::fabs(at(out, kBeat * 3 / 2) - duck) < 0.005 && std::fabs(at(out, kBeat * 5 / 2) - 0.5) < 0.005,
         "sidechain level: a loud key ducks the output by the route (-24 dB), then lets go");
  // under the threshold nothing moves
  std::vector<float> quiet(dc.size(), 0.f);
  for (long i = kBeat; i < 2 * kBeat; ++i) quiet[static_cast<size_t>(i)] = 0.03f;  // -30 dB
  o.sc = &quiet;
  expect(std::fabs(at(render(st, dc, o), kBeat * 3 / 2) - 0.5) < 0.005, "sidechain level: under the threshold, no duck");

  // notes: from any MIDI Out port, each one sets the source full and it falls with the release
  PluginState nt = st;
  nt.params[tr::TrScListen] = tr::LsNotes;
  nt.params[tr::TrNoteSrc] = tr::NsPorts;
  nt.params[tr::TrNotePort] = -1;
  nt.params[tr::TrScRelease] = 50;
  RunOpts on;
  on.hits = [](size_t at0, int n, NoteHitBus& bus) {
    if (static_cast<long>(at0) <= kBeat && kBeat < static_cast<long>(at0) + n) bus.add(7, static_cast<int>(kBeat - static_cast<long>(at0)), 60, 0, NoteHitBus::Note);
  };
  const Out ono = render(nt, dc, on);
  expect(std::fabs(at(ono, kBeat - 10) - 0.5) < 0.005 && at(ono, kBeat + 200) < 0.1 && std::fabs(at(ono, kBeat * 2) - 0.5) < 0.01,
         "sidechain notes: a note ducks at its sample and the duck falls away");
  // a note from a channel (not a port) is not heard when listening to ports
  RunOpts oc;
  oc.hits = [](size_t at0, int n, NoteHitBus& bus) {
    if (static_cast<long>(at0) <= kBeat && kBeat < static_cast<long>(at0) + n) bus.add(7, static_cast<int>(kBeat - static_cast<long>(at0)), 60, -1, NoteHitBus::Note);
  };
  expect(std::fabs(at(render(nt, dc, oc), kBeat + 200) - 0.5) < 0.005, "sidechain notes: the source filter holds");
  // retrig: a note restarts the lanes (step 1 again right after the note)
  PluginState rt = gate();
  rt.params[tr::TrScRetrig] = 1;
  rt.params[tr::TrNoteSrc] = tr::NsPorts;
  editBank(rt, [](tr::Bank& b) {
    for (int k = 0; k < 16; ++k) b.val[0][0][tr::LnVol][k] = k == 0 ? 1.f : 0.f;
  });
  RunOpts orr;
  orr.hits = [](size_t at0, int n, NoteHitBus& bus) {
    const long hit = kBeat + 1500;
    if (static_cast<long>(at0) <= hit && hit < static_cast<long>(at0) + n) bus.add(0, static_cast<int>(hit - static_cast<long>(at0)), 60, 0, NoteHitBus::Note);
  };
  const Out ort = render(rt, dc, orr);
  expect(at(ort, kBeat + 1000) < 0.01 && std::fabs(at(ort, kBeat + 3000) - 0.5) < 0.01, "retrig: a note restarts the lanes");
}

// The sidechain through the project: Link makes the route the effect reads (one undo step).
void testSidechainProject() {
  Song s = makeEmptySong();
  s.tempo = kTempo;
  while (s.mixer.size() < 3) {
    MixerTrack t;
    t.id = s.allocateId();
    t.sends.push_back({s.masterId(), 1.0f, false});
    s.mixer.push_back(t);
  }
  const ObjectId keyT = s.mixer[1].id, gateT = s.mixer[2].id;
  s.mixer[1].sends.clear();  // the key is heard only through the sidechain
  s.mixer[1].name = "Key";
  s.mixer[2].name = "Gated";
  PluginState st = gate();
  st.params[tr::routeParam(0, tr::RtSource)] = tr::SrcSidechain;
  st.params[tr::routeParam(0, tr::RtTarget)] = tr::TgOutput;
  st.params[tr::routeParam(0, tr::RtAmount)] = -1;
  st.params[tr::TrScRelease] = 5;
  st.params[tr::TrScIn] = 0;  // no input yet
  s.mixer[2].slots[0].plugin = st;
  auto clipOf = [&](ObjectId ch, Tick start, Tick len) {
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch;
    c.start = start;
    c.length = len;
    c.track = 0;
    return c;
  };
  auto wav = [&](const std::string& name, size_t frames, float v) {
    AudioBuffer b(2, frames);
    for (size_t i = 0; i < frames; ++i) b.at(0, i) = b.at(1, i) = v;
    const std::string path = (std::filesystem::temp_directory_path() / ("gl_terrace_" + name)).string();
    writeWav(path, b, static_cast<int>(kSr), 24);
    return path;
  };
  const std::string padWav = wav("pad.wav", static_cast<size_t>(kBeat * 5), 0.5f), keyWav = wav("key.wav", static_cast<size_t>(kBeat), 0.9f);
  sampleCache().clear();
  Channel pad;
  pad.id = s.allocateId();
  pad.name = "Pad";
  pad.plugin = defaultPluginState("gl.sampler");
  pad.plugin.data = padWav;
  pad.audioClip = true;
  pad.mixerTrack = gateT;
  s.channels.push_back(pad);
  s.playlist.push_back(clipOf(pad.id, 0, 4 * s.ppq));
  Channel key = pad;
  key.id = s.allocateId();
  key.name = "Key";
  key.plugin.data = keyWav;
  key.mixerTrack = keyT;
  s.channels.push_back(key);
  Clip kc = clipOf(key.id, 2 * s.ppq, s.ppq);
  kc.track = 1;
  s.playlist.push_back(kc);

  Document d(s);
  const size_t h0 = d.history().position();
  expect(tr::linkLevelTrack(d, gateT, 0, keyT), "sidechain link: Key's level into the gate");
  expect(d.history().position() == h0 + 1, "sidechain link: one undo step");
  const PluginState& linked = d.song().mixer[2].slots[0].plugin;
  expect(std::lround(linked.params[tr::TrScIn]) == -1 && std::lround(linked.params[tr::TrScListen]) == tr::LsLevel, "sidechain link: input = the sidechain routes");
  RenderOptions ro;
  ro.sampleRate = static_cast<int>(kSr);
  ro.blockSize = 512;
  const AudioBuffer b = renderSong(d.song(), ro);
  auto lv = [&](long i) { return i < static_cast<long>(b.frames) ? b.at(0, static_cast<size_t>(i)) : 0.f; };
  const double before = lv(kBeat), during = lv(kBeat * 5 / 2);
  char msg[128];
  std::snprintf(msg, sizeof msg, "sidechain link: the pad ducks while the key plays (%.3f -> %.3f)", before, during);
  expect(before > 0.3 && during < before * 0.2, msg);
  d.undo();
  expect(std::lround(d.song().mixer[2].slots[0].plugin.params[tr::TrScIn]) == 0, "sidechain link: undo puts it back");
  // a channel's notes: Listen Notes, the channel kept in the state (one undo step)
  d.redo();
  Document d2(d.song());
  d2.editMixerTrack(gateT, "listen to notes", [](MixerTrack& t) { t.slots[0].plugin.params[tr::TrScListen] = tr::LsNotes; });
  const size_t h1 = d2.history().position();
  expect(tr::linkChannel(d2, gateT, 0, key.id), "sidechain link: a channel's notes");
  expect(d2.history().position() == h1 + 1, "sidechain link: the channel link is one undo step");
  const PluginState& np = d2.song().mixer[2].slots[0].plugin;
  expect(tr::noteChannel(np.data) == key.id && std::lround(np.params[tr::TrNoteSrc]) == tr::NsChannel, "sidechain link: the notes come from Key");
  Song back;
  expect(loadProjectText(projectToText(d2.song()), "", back) && tr::noteChannel(back.mixer[2].slots[0].plugin.data) == key.id,
         "sidechain link: the channel survives a save and load");
}

// ------------------------------------------------------------------ 5. routes

void testRoutes() {
  PluginState st = gate();
  st.params[tr::lfoParam(0, tr::LfShape)] = tr::ShSquare;
  st.params[tr::lfoParam(0, tr::LfRate)] = 7;  // 1/4: one cycle a beat
  st.params[tr::routeParam(3, tr::RtSource)] = tr::SrcLfo1;
  st.params[tr::routeParam(3, tr::RtTarget)] = tr::TgOutput;
  st.params[tr::routeParam(3, tr::RtAmount)] = 0.5f;  // +-12 dB
  const std::vector<float> dc(kBeat * 2, 0.5f);
  const Out o = render(st, dc);
  const double up = at(o, kBeat / 4), dn = at(o, kBeat * 3 / 4);
  char msg[96];
  std::snprintf(msg, sizeof msg, "route LFO 1 -> output: +12 / -12 dB (%.3f / %.3f)", up, dn);
  expect(std::fabs(up - 0.5 * std::pow(10.0, 0.6)) < 0.02 && std::fabs(dn - 0.5 * std::pow(10.0, -0.6)) < 0.005, msg);
  // a route onto a band's volume depth: the LFO opens and closes the low band's gate
  PluginState dp = gate();
  editBank(dp, [](tr::Bank& b) {
    for (int k = 0; k < 16; ++k) b.val[0][0][tr::LnVol][k] = 0.f;
  });
  dp.params[tr::laneParam(0, tr::LnVol, tr::LpDepth)] = 0;
  dp.params[tr::lfoParam(0, tr::LfShape)] = tr::ShSquare;
  dp.params[tr::lfoParam(0, tr::LfRate)] = 7;
  dp.params[tr::routeParam(0, tr::RtSource)] = tr::SrcLfo1;
  dp.params[tr::routeParam(0, tr::RtTarget)] = tr::bandTarget(0, tr::TbVolDepth);
  dp.params[tr::routeParam(0, tr::RtAmount)] = 1;
  const Out od = render(dp, dc);
  expect(std::fabs(at(od, kBeat / 4)) < 0.01 && std::fabs(at(od, kBeat * 3 / 4) - 0.5) < 0.01, "route LFO 1 -> low depth gates the band");
  // the follower onto a band's gain: loud input takes it down
  PluginState fo = gate();
  fo.params[tr::routeParam(0, tr::RtSource)] = tr::SrcFollower;
  fo.params[tr::routeParam(0, tr::RtTarget)] = tr::bandTarget(0, tr::TbGain);
  fo.params[tr::routeParam(0, tr::RtAmount)] = -0.5f;
  const Out of = render(fo, dc);
  // (the follower reads 0.5 of the input: 0.5 x -0.5 of 24 dB = -6 dB)
  expect(std::fabs(at(of, kBeat) - 0.5 * std::pow(10.0, -6.0 / 20)) < 0.01, "route follower -> low gain: the band ducks under its own level");
  // splits moved by an LFO: the bands still add up (level within half a dB on noise)
  PluginState sp = defaultPluginState(tr::kType);
  sp.params[tr::lfoParam(0, tr::LfShape)] = tr::ShSine;
  sp.params[tr::lfoParam(0, tr::LfRate)] = 4;
  for (int k = 0; k < 3; ++k) {
    sp.params[tr::routeParam(k, tr::RtSource)] = tr::SrcLfo1;
    sp.params[tr::routeParam(k, tr::RtTarget)] = tr::TgSplit1 + k;
    sp.params[tr::routeParam(k, tr::RtAmount)] = 0.5f;
  }
  const std::vector<float> nz = noise(kBeat * 4, 9);
  const Out os = render(sp, nz);
  double ei = 0, eo = 0;
  for (size_t i = 4800; i < nz.size(); ++i) ei += static_cast<double>(nz[i]) * nz[i], eo += static_cast<double>(os.L[i]) * os.L[i];
  std::snprintf(msg, sizeof msg, "routes onto the splits: the sum stays level (%.3f dB)", 10 * std::log10(eo / ei));
  expect(std::fabs(10 * std::log10(eo / ei)) < 0.5, msg);
  // the step size route: +1 size step (1/16 -> 1/12) moves the lane
  PluginState sz = gate();
  editBank(sz, [](tr::Bank& b) {
    for (int k = 0; k < 16; ++k) b.val[0][0][tr::LnVol][k] = k % 2 ? 0.f : 1.f;
  });
  sz.params[tr::lfoParam(0, tr::LfShape)] = tr::ShSquare;
  sz.params[tr::lfoParam(0, tr::LfRate)] = 14;  // 8 bars: the first half holds +1
  sz.params[tr::routeParam(0, tr::RtSource)] = tr::SrcLfo1;
  sz.params[tr::routeParam(0, tr::RtTarget)] = tr::bandTarget(0, tr::TbSize);
  sz.params[tr::routeParam(0, tr::RtAmount)] = 0.25f;
  const Out oz = render(sz, dc);
  expect(std::fabs(at(oz, 7000) - 0.5) < 0.01, "route onto the step size: the steps get longer");
  // a route is at rest when its amount is 0
  PluginState z = st;
  z.params[tr::routeParam(3, tr::RtAmount)] = 0;
  expect(std::fabs(at(render(z, dc), kBeat / 4) - 0.5) < 0.005, "route amount 0: nothing moves");
}

// ------------------------------------------------------------------ 6. patterns

void testPatterns() {
  PluginState st = gate();
  editBank(st, [](tr::Bank& b) {
    for (int k = 0; k < 16; ++k) b.val[1][0][tr::LnVol][k] = 0.f;  // pattern 2: the low band closed
    b.chain[0] = 0, b.chain[1] = 1;
  });
  const std::vector<float> dc(kBeat * 8, 0.5f);
  expect(std::fabs(at(render(st, dc), 20000) - 0.5) < 0.01, "pattern 1 plays by default");
  PluginState p2 = st;
  p2.params[tr::TrPattern] = 1;
  expect(std::fabs(at(render(p2, dc), 20000)) < 0.01, "the Pattern control picks pattern 2");
  // patterns share the lane settings: a 4 step lane loops pattern 2 on 4 steps too
  PluginState ch = st;
  ch.params[tr::TrChain] = 1;
  const Out oc = render(ch, dc);
  expect(std::fabs(at(oc, 20000) - 0.5) < 0.01 && std::fabs(at(oc, 96000 + 20000)) < 0.01, "the chain plays pattern 1, then 2, one bar each");
  ch.params[tr::TrLength] = 0;  // 1/2 bar
  const Out oc2 = render(ch, dc);
  expect(std::fabs(at(oc2, 20000) - 0.5) < 0.01 && std::fabs(at(oc2, 48000 + 20000)) < 0.01, "the chain follows Length");
  // notes pick patterns: note base + 1 = pattern 2 from that note on
  PluginState np = st;
  np.params[tr::TrNotePatterns] = 1;
  np.params[tr::TrNoteSrc] = tr::NsPorts;
  np.params[tr::TrNoteBase] = 48;
  RunOpts o;
  o.hits = [](size_t at0, int n, NoteHitBus& bus) {
    if (static_cast<long>(at0) <= kBeat * 2 && kBeat * 2 < static_cast<long>(at0) + n)
      bus.add(0, static_cast<int>(kBeat * 2 - static_cast<long>(at0)), 49, 0, NoteHitBus::Note);
  };
  const Out on = render(np, dc, o);
  expect(std::fabs(at(on, kBeat) - 0.5) < 0.01 && std::fabs(at(on, kBeat * 3)) < 0.01, "a note picks pattern 2");
  // a note outside the 8 pattern notes leaves the pattern
  RunOpts o2;
  o2.hits = [](size_t at0, int n, NoteHitBus& bus) {
    if (static_cast<long>(at0) <= kBeat * 2 && kBeat * 2 < static_cast<long>(at0) + n)
      bus.add(0, static_cast<int>(kBeat * 2 - static_cast<long>(at0)), 70, 0, NoteHitBus::Note);
  };
  expect(std::fabs(at(render(np, dc, o2), kBeat * 3) - 0.5) < 0.01, "a note outside the pattern notes changes nothing");
}

// ------------------------------------------------------------------ 7. state

bool sameBank(const tr::Bank& a, const tr::Bank& b) {
  for (int p = 0; p < tr::kPatterns; ++p)
    for (int bd = 0; bd < tr::kBands; ++bd)
      for (int l = 0; l < tr::kLanes; ++l)
        for (int k = 0; k < tr::kSteps; ++k)
          if (std::fabs(a.val[p][bd][l][k] - b.val[p][bd][l][k]) > 1e-4f || a.chance[p][bd][l][k] != b.chance[p][bd][l][k] ||
              a.hits[p][bd][l][k] != b.hits[p][bd][l][k])
            return false;
  for (int k = 0; k < tr::kChain; ++k)
    if (a.chain[k] != b.chain[k]) return false;
  return a.chainLen == b.chainLen;
}

void testState() {
  auto a = std::make_unique<tr::Bank>(), b = std::make_unique<tr::Bank>();
  tr::clear(*a);
  uint32_t r = 5;
  for (int t = 0; t < 600; ++t) {
    const int p = static_cast<int>(rnd(r) % tr::kPatterns), bd = static_cast<int>(rnd(r) % tr::kBands), l = static_cast<int>(rnd(r) % tr::kLanes),
              k = static_cast<int>(rnd(r) % tr::kSteps);
    a->val[p][bd][l][k] = l == tr::LnPan ? (rnd(r) % 20001) / 10000.f - 1.f : (rnd(r) % 10001) / 10000.f;
    if (t % 3 == 0) a->chance[p][bd][l][k] = static_cast<uint8_t>(rnd(r) % 101);
    if (t % 5 == 0) a->hits[p][bd][l][k] = static_cast<uint8_t>(1 + rnd(r) % 4);
  }
  a->chain[0] = 3, a->chain[1] = -1, a->chain[2] = 7, a->chain[5] = 0;
  tr::finish(*a);
  const std::string text = tr::write(*a);
  tr::parse(text.c_str(), *b);
  expect(sameBank(*a, *b), "bank text: every step, chance, repeat and the chain read back");
  expect(b->chainLen == 6, "bank text: the chain runs to its last filled slot");
  tr::clear(*b);
  expect(tr::write(*b).empty(), "bank text: an empty bank writes nothing");
  // the note source channel rides along
  const std::string withCh = tr::withNoteChannel(text, 42);
  expect(tr::noteChannel(withCh) == 42 && tr::noteChannel(tr::withBank(withCh, *a)) == 42, "bank text: the note channel stays with the steps");
  tr::parse(withCh.c_str(), *b);
  expect(sameBank(*a, *b), "bank text: the channel line does not disturb the steps");
  // garbage in: no crash, values clamped
  tr::parse("TR1\nv 9 9 9 0 5\nv 0 0 0 63 99999 7 8\nq 0 0 0 0 250\nr 0 0 0 0 9\nn 12 -3 4\nchannel=x\n", *b);
  expect(b->val[0][0][0][63] <= 1.f && b->chance[0][0][0][0] <= 100 && b->hits[0][0][0][0] <= 4 && b->hits[0][0][0][0] >= 1,
         "bank text: bad lines are clamped or skipped");

  // the whole state through a project save and load
  Song s = makeEmptySong();
  PluginState st = defaultPluginState(tr::kType);
  st.data = tr::withNoteChannel(tr::write(*a), 42);
  for (size_t k = 0; k < st.params.size(); ++k) {
    const ParamDesc& d = findPlugin(tr::kType)->params[k];
    st.params[k] = static_cast<float>(d.min + (d.max - d.min) * ((k * 37) % 11) / 10.0);
    if (d.kind == ParamKind::Choice || d.kind == ParamKind::Toggle) st.params[k] = std::round(st.params[k]);
  }
  normalizePluginState(st);
  s.mixer[0].slots[0].plugin = st;
  Song back;
  expect(loadProjectText(projectToText(s), "", back), "state: the project loads");
  const PluginState& got = back.mixer[0].slots[0].plugin;
  bool same = got.type == st.type && got.data == st.data && got.params.size() == st.params.size();
  for (size_t k = 0; same && k < st.params.size(); ++k) same = std::fabs(got.params[k] - st.params[k]) < 1e-4f;
  expect(same, "state: every parameter and the step text come back from a save");
  // factory presets: each one parses, plays finite, and the patterns of "Four floors" are there
  const PluginInfo* in = findPlugin(tr::kType);
  bool presetsOk = in && in->presets.size() >= 4;
  const std::vector<float> x = noise(kBeat * 2, 4);
  for (size_t p = 0; in && p < in->presets.size(); ++p) {
    const PluginState ps = presetState(*in, p);
    const Out o = render(ps, x);
    for (float v : o.L) presetsOk = presetsOk && std::isfinite(v) && std::fabs(v) < 8;
  }
  expect(presetsOk, "factory presets: every one plays, finite");
}

// ------------------------------------------------------------------ 8. blocks and extremes

void testBlocksAndExtremes() {
  PluginState st = gate();
  editBank(st, [](tr::Bank& b) {
    for (int bd = 0; bd < tr::kBands; ++bd)
      for (int k = 0; k < 16; ++k) b.val[0][bd][tr::LnVol][k] = ((k + bd) % 3) / 2.f, b.chance[0][bd][tr::LnVol][k] = k % 4 ? 100 : 60;
  });
  const std::vector<float> x = noise(kBeat * 4, 11);
  RunOpts a, b, c;
  a.block = 64, b.block = 1000, c.block = 17;
  const Out oa = render(st, x, a), ob = render(st, x, b), oc = render(st, x, c);
  double d = 0;
  for (size_t i = 0; i < x.size(); ++i) d = std::max({d, static_cast<double>(std::fabs(oa.L[i] - ob.L[i])), static_cast<double>(std::fabs(oa.L[i] - oc.L[i]))});
  char msg[96];
  std::snprintf(msg, sizeof msg, "block sizes 64 / 1000 / 17 play the same (%.2g)", d);
  expect(d < 1e-4, msg);
  // stopped: the free clock runs the lanes too
  RunOpts stop;
  stop.playing = false;
  const Out os = render(st, x, stop);
  double moved = 0;
  for (size_t i = 0; i < x.size(); ++i) moved = std::max(moved, static_cast<double>(std::fabs(os.L[i] - oa.L[i])));
  expect(moved < 1e-4, "stopped, the lanes run on their own clock from the start");

  const PluginInfo* in = findPlugin(tr::kType);
  uint32_t r = 3;
  bool finite = true;
  for (int t = 0; t < 30; ++t) {
    PluginState s = defaultPluginState(tr::kType);
    for (size_t k = 0; k < s.params.size(); ++k) {
      const ParamDesc& pd = in->params[k];
      const double u = (rnd(r) % 10001) / 10000.0;
      s.params[k] = static_cast<float>(pd.kind == ParamKind::Choice ? std::round(pd.min + u * (pd.max - pd.min)) : pd.min + u * (pd.max - pd.min));
    }
    s.params[tr::TrOutput] = 0;
    s.params[tr::TrSmooth] = 0.5f;
    const Out o = render(s, noise(12000, static_cast<uint32_t>(t + 20)));
    for (float v : o.L) finite = finite && std::isfinite(v) && std::fabs(v) < 100;
    for (float v : o.R) finite = finite && std::isfinite(v) && std::fabs(v) < 100;
  }
  expect(finite, "30 random states: the output stays finite");
  // splits out of order or squeezed together still play finite
  PluginState sq = defaultPluginState(tr::kType);
  sq.params[tr::TrSplit1] = 18000, sq.params[tr::TrSplit2] = 30, sq.params[tr::TrSplit3] = 20000;
  bool sqOk = true;
  for (float v : render(sq, noise(12000, 5)).L) sqOk = sqOk && std::isfinite(v) && std::fabs(v) < 4;
  expect(sqOk, "splits out of order: still finite");
  // normalize keeps a short or long state usable
  PluginState shortSt;
  shortSt.type = tr::kType;
  shortSt.params = {1, 200};
  normalizePluginState(shortSt);
  expect(static_cast<int>(shortSt.params.size()) == tr::TrCount, "normalize fills a short state");
}

}  // namespace

int main() {
  const PluginInfo* in = findPlugin(tr::kType);
  expect(in && static_cast<int>(in->params.size()) == tr::TrCount, "Terrace is registered with its parameters");
  expect(in && in->kind == PluginKind::Effect && !in->midiInput && in->sideInParam == tr::TrScIn, "an effect, no MIDI input, the sidechain input parameter");
  testCrossover();
  testEffectFlat();
  testLaneTiming();
  testChance();
  testSidechainDsp();
  testSidechainProject();
  testRoutes();
  testPatterns();
  testState();
  testBlocksAndExtremes();
  return finish();
}

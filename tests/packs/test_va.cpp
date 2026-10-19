// Gridline — J-22 "va" group tests: Twin VA (gl.twinva), Saw Swarm
// (gl.sawswarm), Finger Bass (gl.fingerbass) and the decode of FL Studio's
// shipped PoiZone / Sawer / BooBass states onto them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpGen.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/synths/va.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;
using namespace gl::packs::synths::va;

namespace {

struct Ev {
  int at;
  int kind;  // 0 on, 1 off, 2 midi
  uint32_t id;
  double pitch;
  float vel;
  uint8_t s = 0, d1 = 0, d2 = 0;
};
struct Out {
  std::vector<float> L, R;
  bool finite = true;
  float peak = 0;
  bool activeAtEnd = false;
};
Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block, double tempo = 120) {
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
  ctx.sampleRate = 44100;
  ctx.tempo = tempo;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.kind == 0) g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else if (e.kind == 1) g->noteOff(e.id, e.at - pos);
        else g->midi(e.s, e.d1, e.d2, e.at - pos);
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
double rms(const std::vector<float>& x, int a, int b) {
  double s = 0;
  for (int i = a; i < b; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / std::max(1, b - a));
}
// Brightness: mean |first difference| / mean |x|.
double bright(const std::vector<float>& x, int a, int b) {
  double d = 0, m = 0;
  for (int i = a + 1; i < b; ++i) {
    d += std::fabs(x[static_cast<size_t>(i)] - x[static_cast<size_t>(i - 1)]);
    m += std::fabs(x[static_cast<size_t>(i)]);
  }
  return m > 1e-12 ? d / m : 0;
}
double stereoDiff(const Out& o, int a, int b) {
  double s = 0;
  for (int i = a; i < b; ++i) s += std::fabs(o.L[static_cast<size_t>(i)] - o.R[static_cast<size_t>(i)]);
  return s / std::max(1, b - a);
}
double zcHz(const std::vector<float>& x, int a, int b) {
  int z = 0;
  for (int i = a + 1; i < b; ++i)
    if (x[static_cast<size_t>(i - 1)] <= 0 && x[static_cast<size_t>(i)] > 0) ++z;
  return z * 44100.0 / (b - a);
}
// Goertzel magnitude of frequency f over [a, b) (Hann window).
double tone(const std::vector<float>& x, int a, int b, double f) {
  const double w = 2 * 3.14159265358979 * f / 44100.0, c = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (int i = a; i < b; ++i) {
    const double win = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * (i - a) / (b - a));
    const double s0 = x[static_cast<size_t>(i)] * win + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (b - a);
}
// Upper-harmonic share: (3rd + 5th harmonic) / fundamental.
double harm(const std::vector<float>& x, int a, int b, double f) { return (tone(x, a, b, 3 * f) + tone(x, a, b, 5 * f)) / std::max(1e-12, tone(x, a, b, f)); }
void set(PluginState& s, int i, float v) { s.params[static_cast<size_t>(i)] = v; }
const std::vector<Ev> kNote = {{0, 0, 1, 60, 0.8f}, {22050, 1, 1, 0, 0}};

// Every parameter at its minimum / maximum, with notes, stays finite.
bool extremesFinite(const char* type) {
  const PluginInfo* info = findPlugin(type);
  bool ok = info != nullptr;
  for (int side = 0; side < 2 && info; ++side) {
    PluginState s = defaultPluginState(type);
    for (size_t k = 0; k < info->params.size(); ++k) s.params[k] = static_cast<float>(side ? info->params[k].max : info->params[k].min);
    Out o = render(s, {{0, 0, 1, 24, 1.f}, {10, 0, 2, 96, 1.f}, {50, 0, 3, 127, 1.f}, {20000, 1, 1, 0, 0}, {20000, 1, 2, 0, 0}}, 30000, 333);
    ok = ok && o.finite && o.peak < 50.f;
  }
  return ok;
}

// Shipped presets / project states of an FL plugin: decode, exactness and sound.
void corpus(const char* fl, const char* type, size_t minPresets, size_t minProjects, bool audibleAll = true) {
  if (!testfl::flInstalled()) {
    expect(true, (std::string(fl) + ": FL Studio not installed, corpus skipped").c_str());
    return;
  }
  auto pre = testfl::decodePresets(fl);
  auto prj = testfl::decodeProjects(fl);
  size_t ok = 0, exact = 0, finite = 0, audible = 0, typed = 0;
  std::vector<testfl::Decoded> all = pre;
  all.insert(all.end(), prj.begin(), prj.end());
  for (const auto& d : all) {
    testfl::maybeDump(d);
    if (!d.ok) continue;
    ++ok;
    exact += d.exact ? 1 : 0;
    typed += d.state.type == type ? 1 : 0;
    bool fin = true;
    for (float v : d.state.params) fin = fin && std::isfinite(v);
    // Two notes (C3, C4) held for 1.5 s (pads with slow attacks).
    Out o = render(d.state, {{0, 0, 1, 48, 0.85f}, {0, 0, 2, 60, 0.85f}, {66150, 1, 1, 0, 0}, {66150, 1, 2, 0, 0}}, 70560, 512);
    fin = fin && o.finite;
    finite += fin ? 1 : 0;
    audible += o.peak > 1e-3f ? 1 : 0;
    if (!d.exact) std::printf("#   not exact: %s\n", d.where.c_str());
    if (o.peak <= 1e-3f) std::printf("#   silent: %s\n", d.where.c_str());
  }
  std::printf("# %s: %zu presets, %zu project states; decoded %zu, exact (walked to end) %zu, finite %zu, audible %zu\n", fl, pre.size(),
              prj.size(), ok, exact, finite, audible);
  expect(pre.size() >= minPresets && prj.size() >= minProjects, (std::string(fl) + ": shipped states found").c_str());
  expect(ok == all.size() && typed == all.size(), (std::string(fl) + ": every state decodes to " + type).c_str());
  expect(exact == all.size(), (std::string(fl) + ": every state walks to its exact end (exact)").c_str());
  expect(finite == all.size(), (std::string(fl) + ": every decoded state renders finite").c_str());
  if (audibleAll)
    expect(audible == all.size(), (std::string(fl) + ": every decoded state is audible").c_str());
  else
    expect(audible * 100 >= all.size() * 95, (std::string(fl) + ": ≥95% of decoded states are audible").c_str());
}

// Runs a decoder on a raw state blob.
flp::PluginMapping decodeRaw(const char* fl, const std::vector<uint8_t>& state) {
  flp::File file;
  Song song;
  flp::Report rep;
  flp::Context c(file, song, rep);
  std::vector<uint8_t> name;
  for (const char* p = fl; *p; ++p) name.push_back(static_cast<uint8_t>(*p)), name.push_back(0);
  name.push_back(0), name.push_back(0);
  std::vector<flp::Event> evs(2);
  evs[0].id = 201;
  evs[0].data = name.data();
  evs[0].size = static_cast<uint32_t>(name.size());
  evs[1].id = 213;
  evs[1].data = state.data();
  evs[1].size = static_cast<uint32_t>(state.size());
  flp::PluginMapping m;
  m.flName = fl;
  const flp::PluginDecoder* dec = flp::findDecoder(fl);
  if (dec) (*dec)(c, evs, m);
  if (m.state.type.empty()) m.state.type = m.glType;
  normalizePluginState(m.state);
  return m;
}
void putF(std::vector<uint8_t>& b, size_t at, float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(u >> (8 * i));
}
void putU(std::vector<uint8_t>& b, size_t at, uint32_t u) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(u >> (8 * i));
}

}  // namespace

int main() {
  // ------------------------------------------------------------ catalogue
  for (const char* type : {"gl.twinva", "gl.sawswarm", "gl.fingerbass"}) {
    const PluginInfo* info = findPlugin(type);
    expect(info != nullptr, (std::string(type) + ": catalogue entry").c_str());
    if (!info) return finish();
    bool sane = !info->params.empty();
    for (const auto& p : info->params) sane = sane && !p.name.empty() && p.def >= p.min && p.def <= p.max;
    expect(sane, (std::string(type) + ": parameters named, defaults in range").c_str());
    expect(info->presets.size() >= 2 && info->presets.size() <= 6, (std::string(type) + ": 2-6 factory presets").c_str());
    bool presetsSound = true;
    for (const auto& pr : info->presets) {
      PluginState s = defaultPluginState(type);
      for (auto [i, v] : pr.values) s.params[static_cast<size_t>(i)] = static_cast<float>(info->params[static_cast<size_t>(i)].clamp(v));
      Out o = render(s, kNote, 30000, 256);
      presetsSound = presetsSound && o.finite && o.peak > 0.01f && o.peak < 4.f;
    }
    expect(presetsSound, (std::string(type) + ": every factory preset sounds (finite, sensible level)").c_str());
    PluginState shortState;
    shortState.type = type;
    shortState.params = {0.3f};
    normalizePluginState(shortState);
    expect(shortState.params.size() == info->params.size(), (std::string(type) + ": normalizePluginState pads short vectors").c_str());
    Out d = render(defaultPluginState(type), kNote, 66150, 256);
    expect(d.finite && d.peak > 0.02f, (std::string(type) + ": default patch sounds").c_str());
    expect(rms(d.L, 60000, 66150) < 1e-3, (std::string(type) + ": note-off releases to silence").c_str());
    expect(extremesFinite(type), (std::string(type) + ": parameter extremes stay finite").c_str());
    Out a = render(defaultPluginState(type), kNote, 30000, 64), b = render(defaultPluginState(type), kNote, 30000, 1024);
    double diff = 0;
    for (size_t i = 0; i < a.L.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a.L[i] - b.L[i])));
    expect(diff < 1e-5, (std::string(type) + ": block-size invariant").c_str());
  }

  // ------------------------------------------------------------- Twin VA
  {
    PluginState s = defaultPluginState("gl.twinva");
    Out base = render(s, kNote, 22050, 256);
    expect(std::fabs(zcHz(base.L, 4000, 20000) - 261.6) < 6, "Twin VA: plays the note pitch (saw A)");
    PluginState dark = s;
    set(dark, TwCutoff, 0.35f);
    const double hd = harm(render(dark, kNote, 22050, 256).L, 4000, 20000, 261.63), hb = harm(base.L, 4000, 20000, 261.63);
    expect(hd < 0.3 * hb, "Twin VA: cutoff darkens");
    PluginState uni = s;
    set(uni, TwUnison, 4), set(uni, TwUniPan, 1), set(uni, TwUniDetune, 0.4f);
    expect(stereoDiff(render(uni, kNote, 22050, 256), 4000, 20000) > 0.01, "Twin VA: unison spread is stereo");
    PluginState bOnly = s;
    set(bOnly, TwBalance, 1), set(bOnly, TwBPitch, 12);
    expect(std::fabs(zcHz(render(bOnly, kNote, 22050, 256).L, 4000, 20000) - 523.3) < 10, "Twin VA: osc B pitch +12 st");
    PluginState gate = s;
    set(gate, TwGateMix, 1);
    for (int k = 0; k < kTwGateSteps; ++k) set(gate, TwGate1 + k, k % 2 ? 0.f : 1.f);
    Out g = render(gate, {{0, 0, 1, 60, 0.8f}}, 44100, 256);
    // At 120 BPM a 1/16 step is 5512 samples: step 0 on, step 1 off.
    expect(rms(g.L, 4000, 5000) > 5 * rms(g.L, 7500, 10500), "Twin VA: trance gate mutes the off steps");
    PluginState arp = s;
    set(arp, TwArpMode, TaUp), set(arp, TwAmpD, 0.3f), set(arp, TwAmpS, 0);
    Out ar = render(arp, {{0, 0, 1, 60, 0.8f}, {0, 0, 2, 64, 0.8f}, {0, 0, 3, 67, 0.8f}}, 44100, 256);
    int onsets = 0;
    for (int b0 = 0; b0 + 2756 < 44100; b0 += 2756) onsets += rms(ar.L, b0, b0 + 600) > 2 * rms(ar.L, b0 + 2000, b0 + 2756) ? 1 : 0;
    expect(ar.finite && onsets >= 6, "Twin VA: arpeggiator retriggers steps");
    PluginState mono = s;
    set(mono, TwMono, 1), set(mono, TwGlide, 0.4f);
    Out mg = render(mono, {{0, 0, 1, 48, 0.8f}, {11025, 0, 2, 60, 0.8f}}, 33075, 256);
    const double mid = zcHz(mg.L, 11500, 14000);
    expect(mid > 140 && mid < 255, "Twin VA: mono glide slides between notes");
    PluginState dly = s;
    set(dly, TwDlyOn, 1), set(dly, TwDlyWet, 0.8f), set(dly, TwAmpR, 0);
    Out dl = render(dly, {{0, 0, 1, 60, 0.8f}, {4410, 1, 1, 0, 0}}, 66150, 256);
    expect(rms(dl.L, 30000, 50000) > 1e-3 && dl.activeAtEnd, "Twin VA: delay tail after release");
    PluginState lfo = s;
    set(lfo, TwLfoDest, TdPan), set(lfo, TwLfoAmt, 1), set(lfo, TwLfoShape, TlSine), set(lfo, TwLfoRate, 0.6f);
    Out lf = render(lfo, {{0, 0, 1, 60, 0.8f}}, 88200, 256);
    double lo = 1e9, hi = 0;
    for (int b0 = 2000; b0 + 1000 < 88200; b0 += 1000) lo = std::min(lo, rms(lf.L, b0, b0 + 1000)), hi = std::max(hi, rms(lf.L, b0, b0 + 1000));
    expect(hi > 3 * lo, "Twin VA: LFO → pan sweeps the left side");
    PluginState sync = s;
    set(sync, TwSync, 1), set(sync, TwBalance, 1), set(sync, TwBPitch, 7);
    expect(std::fabs(zcHz(render(sync, kNote, 22050, 256).L, 4000, 20000) - 261.6) < 60, "Twin VA: hard sync keeps the A period");
  }

  // ------------------------------------------------------------ Saw Swarm
  {
    PluginState s = defaultPluginState("gl.sawswarm");
    Out base = render(s, kNote, 22050, 256);
    expect(std::fabs(zcHz(base.L, 4000, 20000) - 261.6) < 6, "Saw Swarm: plays the note pitch");
    PluginState stack = s;
    set(stack, SwUnison, 3), set(stack, SwUniPan, 1), set(stack, SwUniDetune, 0.5f);
    expect(stereoDiff(render(stack, kNote, 22050, 256), 4000, 20000) > 0.01, "Saw Swarm: 8-voice stack spreads in stereo");
    PluginState sub = s;
    set(sub, SwSubLevel, 1), set(sub, SwSubOct, 2), set(sub, SwSubPhase, 0.5f);
    Out so = render(sub, kNote, 22050, 256);
    expect(tone(so.L, 4000, 20000, 130.81) > 5 * tone(base.L, 4000, 20000, 130.81) + 1e-4, "Saw Swarm: sub-saw an octave down adds weight");
    PluginState hp = s;
    set(hp, SwFltMode, SfHp), set(hp, SwCutoff, 0.6f);
    expect(harm(render(hp, kNote, 22050, 256).L, 4000, 20000, 261.63) > 2 * harm(base.L, 4000, 20000, 261.63), "Saw Swarm: high-pass mode thins");
    PluginState vel = s;
    set(vel, SwCutoff, 0.3f), set(vel, SwModSrc1, 1), set(vel, SwModDest1, SwCutoff), set(vel, SwModAmt1, 1);
    const double soft = harm(render(vel, {{0, 0, 1, 60, 0.2f}, {22050, 1, 1, 0, 0}}, 22050, 256).L, 4000, 20000, 261.63);
    const double hard = harm(render(vel, {{0, 0, 1, 60, 1.0f}, {22050, 1, 1, 0, 0}}, 22050, 256).L, 4000, 20000, 261.63);
    expect(hard > 1.3 * soft, "Saw Swarm: MIDI matrix velocity → cutoff");
    PluginState mw = s;
    set(mw, SwCutoff, 0.3f), set(mw, SwModSrc1, 5), set(mw, SwModDest1, SwCutoff), set(mw, SwModAmt1, 1);
    const double wheelOff = harm(render(mw, kNote, 22050, 256).L, 4000, 20000, 261.63);
    set(mw, SwModWheel, 1);
    expect(harm(render(mw, kNote, 22050, 256).L, 4000, 20000, 261.63) > 1.3 * wheelOff, "Saw Swarm: mod wheel source (CC 1) opens the filter");
    PluginState rv = s;
    set(rv, SwRevOn, 1), set(rv, SwRevMix, 1), set(rv, SwRevDecay, 0.9f);
    Out r = render(rv, {{0, 0, 1, 60, 0.8f}, {4410, 1, 1, 0, 0}}, 44100, 256);
    expect(rms(r.L, 22050, 44100) > 1e-3, "Saw Swarm: reverb tail");
    PluginState sy = s;
    set(sy, SwSyncOn, 1), set(sy, SwSyncFreq, 0.5f);
    Out syo = render(sy, kNote, 22050, 256);
    expect(syo.finite && harm(syo.L, 4000, 20000, 261.63) > 1.2 * harm(base.L, 4000, 20000, 261.63), "Saw Swarm: hard sync brightens");
    PluginState arp = s;
    set(arp, SwArpOn, 1), set(arp, SwAmpD, 0.3f), set(arp, SwAmpS, 0), set(arp, SwArpSync, 1);
    Out ar = render(arp, {{0, 0, 1, 60, 0.8f}, {0, 0, 2, 67, 0.8f}}, 44100, 256);
    int onsets = 0;
    for (int b0 = 0; b0 + 5512 < 44100; b0 += 5512) onsets += rms(ar.L, b0, b0 + 800) > 2 * rms(ar.L, b0 + 4500, b0 + 5512) ? 1 : 0;
    expect(onsets >= 5, "Saw Swarm: arpeggiator steps");
  }

  // ---------------------------------------------------------- Finger Bass
  {
    PluginState s = defaultPluginState("gl.fingerbass");
    Out o = render(s, {{0, 0, 1, 40, 0.9f}}, 88200, 256);
    const double f0 = tone(o.L, 2000, 30000, 82.41);
    expect(f0 > 8 * tone(o.L, 2000, 30000, 77.78) && f0 > 8 * tone(o.L, 2000, 30000, 87.31), "Finger Bass: plays the note pitch (E2)");
    expect(rms(o.L, 66000, 88000) < 0.7 * rms(o.L, 2000, 20000), "Finger Bass: plucked notes decay");
    PluginState tr = s;
    set(tr, BbTreble, 1);
    expect(bright(render(tr, {{0, 0, 1, 40, 0.9f}}, 22050, 256).L, 1000, 20000) > 1.2 * bright(o.L, 1000, 20000),
           "Finger Bass: treble boost brightens");
    PluginState bs = s;
    set(bs, BbBass, 0);
    expect(tone(render(bs, {{0, 0, 1, 28, 0.9f}}, 22050, 256).L, 1000, 20000, 41.2) <
               0.5 * tone(render(s, {{0, 0, 1, 28, 0.9f}}, 22050, 256).L, 1000, 20000, 41.2),
           "Finger Bass: bass cut lowers a low note's fundamental");
  }

  // ---------------------------------------------------------------- decode
  {
    // PoiZone: built state (700 bytes) with a CC link and a name.
    std::vector<uint8_t> b(700, 0);
    std::memcpy(b.data(), "SSLF", 4);
    putU(b, 4, 692);
    std::memcpy(b.data() + 8, "4reV", 4);
    for (int t = 0; t < 74; ++t) putF(b, 336 + 4 * static_cast<size_t>(t), -1.f);
    putF(b, 12 + 4 * TwAShape, 1), putF(b, 12 + 4 * TwBPitch, 0.75f), putF(b, 12 + 4 * TwCutoff, 0.4f), putF(b, 12 + 4 * TwEgAmt, 0.25f);
    putF(b, 12 + 4 * TwLfoDest, 0.4f), putF(b, 12 + 4 * TwUnison, 3), putF(b, 12 + 4 * TwPoly, 7), putF(b, 12 + 4 * TwMono, 1);
    putF(b, 12 + 4 * TwTranspose, 0.25f), putF(b, 12 + 4 * TwArpRange, 2.0f / 3.0f);
    putF(b, 336 + 4 * TwCutoff, 74);
    std::memcpy(b.data() + 636, "Test Lead", 9);
    flp::PluginMapping m = decodeRaw("PoiZone", b);
    auto P = [&](int i) { return m.state.params[static_cast<size_t>(i)]; };
    expect(m.glType == "gl.twinva" && m.exact, "PoiZone → Twin VA, exact");
    expect(P(TwAShape) == 1 && std::fabs(P(TwBPitch) - 12) < 1e-4 && std::fabs(P(TwCutoff) - 0.4f) < 1e-6, "PoiZone: shape, osc B pitch, cutoff");
    expect(std::fabs(P(TwEgAmt) + 0.5f) < 1e-6 && P(TwLfoDest) == TdFreqAB && P(TwArpRange) == 2, "PoiZone: bipolar amount, selectors");
    expect(P(TwUnison) == 3 && P(TwPoly) == 7 && P(TwMono) == 1 && std::fabs(P(TwTranspose) + 12) < 1e-4, "PoiZone: digit wheels raw, switches");
    expect(m.state.data.find("cc 18 74") != std::string::npos && m.state.data.find("name Test Lead") != std::string::npos,
           "PoiZone: CC link + preset name kept");
    expect(m.param && m.param(18) == TwCutoff && m.param(80) == TwLfoSync && m.value && std::fabs(m.value(6, 1.0) - 24) < 1e-6,
           "PoiZone: automation index + value map");
    // The CC link drives the cutoff from MIDI.
    PluginState st = m.state;
    st.params[TwArpMode] = 0, st.params[TwMono] = 0, st.params[TwGain] = 0.5f, st.params[TwAmpD] = 1, st.params[TwAmpS] = 1;
    st.params[TwAShape] = 0, st.params[TwBalance] = 0, st.params[TwTranspose] = 0, st.params[TwUnison] = 0, st.params[TwEgAmt] = 0;
    const double closed = harm(render(st, {{0, 2, 0, 0, 0, 0xB0, 74, 0}, {10, 0, 1, 60, 0.8f}}, 22050, 256).L, 4000, 20000, 261.63);
    const double open = harm(render(st, {{0, 2, 0, 0, 0, 0xB0, 74, 127}, {10, 0, 1, 60, 0.8f}}, 22050, 256).L, 4000, 20000, 261.63);
    expect(open > 1.3 * closed, "PoiZone: CC link moves the linked control");
    // Truncated / damaged.
    std::vector<uint8_t> t2(b.begin(), b.begin() + 200);
    flp::PluginMapping mt = decodeRaw("PoiZone", t2);
    bool fin = mt.glType == "gl.twinva" && !mt.exact;
    for (float v : mt.state.params) fin = fin && std::isfinite(v);
    expect(fin, "PoiZone: truncated state → approximate, finite");
    std::vector<uint8_t> nanS = b;
    putF(nanS, 12 + 4 * TwCutoff, std::nanf(""));
    flp::PluginMapping mn = decodeRaw("PoiZone", nanS);
    expect(!mn.exact && std::isfinite(mn.state.params[TwCutoff]), "PoiZone: NaN value → default, approximate");
  }
  {
    std::vector<uint8_t> b(620, 0);
    std::memcpy(b.data(), "SSLF", 4);
    putU(b, 4, 612);
    std::memcpy(b.data() + 8, "1rwS", 4);
    for (int t = 0; t < 96; ++t) b[396 + static_cast<size_t>(t)] = 0xFF;
    putF(b, 12 + 4 * SwUnison, 1), putF(b, 12 + 4 * SwSubOct, 0.25f), putF(b, 12 + 4 * SwFltMode, 2.0f / 3.0f);
    putF(b, 12 + 4 * SwPoly, 5), putF(b, 12 + 4 * SwBendRange, 11), putF(b, 12 + 4 * SwModDest1, 27), putF(b, 12 + 4 * SwModSrc1, 5);
    putF(b, 12 + 4 * SwModAmt1, 0.75f), putF(b, 12 + 4 * SwTranspose, 0.25f);
    b[396 + SwLevel] = 7;
    flp::PluginMapping m = decodeRaw("Sawer", b);
    auto P = [&](int i) { return m.state.params[static_cast<size_t>(i)]; };
    expect(m.glType == "gl.sawswarm" && m.exact, "Sawer → Saw Swarm, exact");
    expect(P(SwUnison) == 3 && P(SwSubOct) == 2 && P(SwFltMode) == SfBp, "Sawer: unison 8, sub −1 oct, BP");
    expect(P(SwPoly) == 5 && P(SwBendRange) == 11 && P(SwModDest1) == SwCutoff && P(SwModSrc1) == 5 && std::fabs(P(SwModAmt1) - 0.5f) < 1e-6,
           "Sawer: combos raw, matrix");
    expect(std::fabs(P(SwTranspose) + 12) < 1e-4 && m.state.data.find("cc 0 7") != std::string::npos, "Sawer: transpose, CC link");
    std::vector<uint8_t> nanS = b;
    putF(nanS, 12 + 4 * SwModSrc1, std::nanf(""));
    flp::PluginMapping mn = decodeRaw("Sawer", nanS);
    expect(mn.exact && mn.state.params[SwModSrc1] == 0, "Sawer: NaN matrix source = none (as the reference reads it)");
    putF(nanS, 12 + 4 * SwCutoff, std::nanf(""));
    flp::PluginMapping mk = decodeRaw("Sawer", nanS);
    expect(!mk.exact && std::isfinite(mk.state.params[SwCutoff]), "Sawer: NaN knob → default, approximate");
    flp::PluginMapping mg = decodeRaw("Sawer", {1, 2, 3, 4, 5});
    expect(mg.glType == "gl.sawswarm" && !mg.exact, "Sawer: garbage → defaults");
  }
  {
    std::vector<uint8_t> b(16, 0);
    putU(b, 0, 1), putU(b, 4, 65535), putU(b, 8, 32767), putU(b, 12, 0);
    flp::PluginMapping m = decodeRaw("BooBass", b);
    expect(m.glType == "gl.fingerbass" && m.exact, "BooBass → Finger Bass, exact");
    expect(std::fabs(m.state.params[BbBass] - 1) < 1e-6 && std::fabs(m.state.params[BbMid] - 0.5f) < 1e-4 && m.state.params[BbTreble] == 0,
           "BooBass: bass / mid / treble");
    flp::PluginMapping mt = decodeRaw("BooBass", {1, 0, 0});
    expect(mt.glType == "gl.fingerbass" && !mt.exact, "BooBass: truncated → defaults");
  }

  // ----------------------------------------------------- shipped content
  corpus("PoiZone", "gl.twinva", 380, 1);
  corpus("Sawer", "gl.sawswarm", 400, 1);
  corpus("BooBass", "gl.fingerbass", 0, 3);
  return finish();
}

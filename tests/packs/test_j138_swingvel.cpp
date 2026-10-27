// Gridline — J-138: swing and velocity controls of the instrument and effect
// packs, measured on the runtimes (step times in samples, per-hit levels).
//   Membrane Kit sequencer swing and step / note velocity
//   Plasma arpeggiator and clip player swing (song grid)
//   Hex sequencer swing
//   Mini Pulse swing
//   Seltmann step swing and velocity to level
//   every built-in instrument: level rises with velocity, nothing hangs
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/mini/mini.h"
#include "../../src/packs/models/drums.h"
#include "../../src/packs/plasma/plasma.h"
#include "../../src/packs/plasma/plasma_dsp.h"
#include "../../src/packs/seltmann/seltmann.h"
#include "../../src/packs/synths/hex.h"
#include "../Check.h"

using namespace gl;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

constexpr double kSr = 44100.0;

PluginState fresh(const std::string& type) {
  PluginState s = defaultPluginState(type);
  normalizePluginState(s);
  return s;
}

struct NoteEvt {
  int at;
  bool on;
  uint32_t id;
  double pitch;
  float vel;
};

// Renders a generator in blocks; `onBlock` sees the generator after each block.
template <class F>
std::vector<float> renderGen(const PluginState& st, const std::vector<NoteEvt>& evs, int total, int block, double tempo, F onBlock) {
  std::vector<float> L(static_cast<size_t>(total), 0.f), R(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) return L;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = tempo;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on) g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else g->noteOff(e.id, e.at - pos);
      }
    ctx.tick = pos * tempo * ctx.ppq / (60.0 * kSr);
    g->process(st, aux, ctx, L.data() + pos, R.data() + pos, n);
    onBlock(*g, pos + n);
  }
  return L;
}
std::vector<float> renderGen(const PluginState& st, const std::vector<NoteEvt>& evs, int total, double tempo = 120) {
  return renderGen(st, evs, total, 256, tempo, [](Generator&, int) {});
}

double peakIn(const std::vector<float>& v, size_t a, size_t b) {
  double p = 0;
  for (size_t i = a; i < std::min(b, v.size()); ++i) p = std::max(p, static_cast<double>(std::fabs(v[i])));
  return p;
}

// ------------------------------------------------------------- Membrane Kit
// Step start times (samples) of the kit sequencer, read from the running step
// it reports, one sample at a time.
// `swing2` (if >= 0) replaces the swing from sample `changeAt` on (a knob turned while playing).
std::vector<int> kitStepTimes(float swing, int length, int samples, float swing2 = -1, int changeAt = 0) {
  using namespace packs::models::dr;
  PluginState s = fresh("gl.membranekit");
  s.params[static_cast<size_t>(KmSwing)] = swing;
  KitData d;
  d.pattern[0].length = length;
  d.pattern[0].key = 60;
  for (int k = 0; k < length; ++k) d.pattern[0].cell[0][k] = 0xFF;
  s.data = encodeKitData(d);
  std::vector<int> t;
  int last = -1;
  renderGen(s, {{0, true, 1, 60, 1.0f}}, samples, 1, 120, [&](Generator& g, int at) {
    const int st = g.displayStep();
    if (st != last && st >= 0) t.push_back(at - 1);
    last = st;
    if (swing2 >= 0 && at == changeAt) s.params[static_cast<size_t>(KmSwing)] = swing2;
  });
  return t;
}

// Peak of each hit of a kit pattern whose steps 0, 4, 8, 12 carry the given cell velocities.
std::vector<double> kitHitPeaks(const int cellVel[4], float noteVel, bool velRoute) {
  using namespace packs::models::dr;
  PluginState s = fresh("gl.membranekit");
  if (velRoute) {  // velocity 1 -> mallet level, full amount (pad 1)
    s.params[static_cast<size_t>(kitPad(0, KpModel + MdVelSrc1))] = 1;
    s.params[static_cast<size_t>(kitPad(0, KpModel + MdVelAmt1))] = 1;
  }
  KitData d;
  d.pattern[0].length = 16;
  d.pattern[0].key = 60;
  for (int k = 0; k < 4; ++k) d.pattern[0].cell[0][k * 4] = static_cast<uint8_t>(0x80 | cellVel[k]);
  s.data = encodeKitData(d);
  const double step = kSr * 15.0 / 120.0;
  auto L = renderGen(s, {{0, true, 1, 60, noteVel}}, static_cast<int>(step * 16), 120);
  std::vector<double> p;
  for (int k = 0; k < 4; ++k) p.push_back(peakIn(L, static_cast<size_t>(k * 4 * step), static_cast<size_t>((k * 4 + 3) * step)));
  return p;
}

bool rising(const std::vector<double>& v, double ratio = 1.02) {
  for (size_t i = 1; i < v.size(); ++i)
    if (!(v[i] > v[i - 1] * ratio)) return false;
  return true;
}

}  // namespace

int main() {
  // ------------------------------------------------ 1. Membrane Kit sequencer swing
  {
    const double step = kSr * 15.0 / 120.0;  // 1/16 at 120 BPM = 5512.5 samples
    for (float sw : {0.0f, 0.5f, 1.0f}) {
      auto t = kitStepTimes(sw, 16, static_cast<int>(step * 16 * 4) + 100);  // 4 bars
      double worst = 0;
      for (size_t k = 0; k < t.size(); ++k) {
        const double want = static_cast<double>(k) * step + ((k & 1) ? sw * 0.5 * step : 0.0);
        worst = std::max(worst, std::fabs(t[k] - want));
      }
      std::printf("# kit swing %.2f: %zu steps, worst error vs grid %.2f samples, step 1 at %d (want %.1f), step 63 at %d (want %.1f)\n", sw,
                  t.size(), worst, t.size() > 1 ? t[1] : -1, step + sw * 0.5 * step, t.size() > 63 ? t[63] : -1,
                  63 * step + sw * 0.5 * step);
      expect(t.size() >= 64 && worst <= 1.0, "kit sequencer: swing " + std::to_string(sw) + " keeps every step within a sample of the grid over 4 bars");
    }
    // Odd length: the swing pairs restart with the pattern, so its first step stays on the grid.
    auto t = kitStepTimes(0.5f, 3, static_cast<int>(step * 12) + 100);
    std::printf("# kit length 3, swing 0.5: steps at");
    for (int x : t) std::printf(" %d", x);
    std::printf("\n");
    expect(t.size() >= 7 && std::fabs(t[3] - 3 * step) <= 1.0 && std::fabs(t[6] - 6 * step) <= 1.0,
           "kit sequencer: odd length 3 loops on the grid (no drift from the swing)");
    // Swing knob turned from 0 to 1 halfway through step 4 (an even step): step 5 swings fully,
    // and the downbeats after it stay on the grid.
    t = kitStepTimes(0.0f, 16, static_cast<int>(step * 16) + 100, 1.0f, static_cast<int>(step * 4.5));
    std::printf("# kit swing 0 -> 1 during step 5: steps 4..8 at %d %d %d %d %d\n", t[4], t[5], t[6], t[7], t[8]);
    expect(t.size() >= 9 && std::fabs(t[5] - 5.5 * step) <= 1.0 && std::fabs(t[6] - 6 * step) <= 1.0 && std::fabs(t[8] - 8 * step) <= 1.0,
           "kit sequencer: a swing change while playing takes effect at once and keeps the grid");
  }
  // ------------------------------------------------ 2. Membrane Kit velocity
  {
    const int cells[4] = {32, 64, 96, 127};
    auto plain = kitHitPeaks(cells, 1.0f, false);
    auto routed = kitHitPeaks(cells, 1.0f, true);
    auto soft = kitHitPeaks(cells, 0.5f, true);
    std::printf("# kit cell velocity 32/64/96/127, default pad: %.4f %.4f %.4f %.4f\n", plain[0], plain[1], plain[2], plain[3]);
    std::printf("# kit cell velocity 32/64/96/127, vel->mallet level: %.4f %.4f %.4f %.4f\n", routed[0], routed[1], routed[2], routed[3]);
    std::printf("# kit same, trigger note velocity 0.5: %.4f %.4f %.4f %.4f\n", soft[0], soft[1], soft[2], soft[3]);
    expect(rising(plain), "kit: a louder step velocity plays louder on the default pad");
    expect(rising(routed), "kit: step velocity routed to mallet level plays louder");
    expect(soft[3] < routed[3] * 0.98, "kit: the trigger note's velocity scales the pattern");
  }
  // ------------------------------------------------ 3. Plasma arpeggiator swing on the song grid
  {
    namespace pl = packs::plasma;
    const double stepBeats = pl::divisionBeats(7);
    const double stepS = stepBeats * 60.0 / 120.0 * kSr;
    auto run = [&](float swing, bool sync, double startBeat) {
      pl::Arp arp;
      pl::ArpSlot s;
      s.rate = 7;
      s.retrigLaunch = !sync;
      s.gate = 0.5f;
      arp.noteOn(60, 0.8f);
      float off[32] = {};
      std::vector<int> scale;
      std::vector<double> ons;  // beats
      const int block = 64;
      for (int pos = 0; pos < static_cast<int>(stepS * 20); pos += block) {
        pl::SeqOut out;
        const double beat = startBeat + pos * 2.0 / kSr;
        arp.run(s, off, block, kSr, 120, beat, true, swing, 2, scale, 0, out);
        for (int i = 0; i < out.n; ++i)
          if (out.ev[i].on) ons.push_back(startBeat + (pos + out.ev[i].offset) * 2.0 / kSr);
      }
      return ons;
    };
    for (float sw : {0.5f, 0.75f, 1.0f}) {
      const double late = std::clamp((sw - 0.5) * 2.0, 0.0, 1.0) / 3.0 * stepBeats;  // documented: up to a third of a step
      auto ons = run(sw, true, 0.0);
      double worst = 0;
      for (size_t k = 1; k < ons.size(); ++k) {
        const double g = std::round((ons[k] - ((std::lround(ons[k] / stepBeats) & 1) ? late : 0.0)) / stepBeats);
        const double want = g * stepBeats + ((static_cast<long long>(g) & 1) ? late : 0.0);
        worst = std::max(worst, std::fabs(ons[k] - want) * kSr / 2.0);
      }
      std::printf("# plasma arp (synced) swing %.2f: first steps at beats", sw);
      for (size_t k = 0; k < std::min<size_t>(ons.size(), 5); ++k) std::printf(" %.4f", ons[k]);
      std::printf("  worst vs grid %.1f samples\n", worst);
      expect(ons.size() >= 10 && worst <= 64.0 + 1, "plasma arp: synced steps sit on the song grid with swing " + std::to_string(sw));
    }
    // Pressed on an odd 16th: the next step is an even one, on the grid.
    {
      auto ons = run(1.0f, true, stepBeats * 1.5);
      const double late = stepBeats / 3.0;
      std::printf("# plasma arp pressed at 1.5 steps, swing 1: steps at beats");
      for (size_t k = 0; k < std::min<size_t>(ons.size(), 5); ++k) std::printf(" %.4f", ons[k]);
      std::printf(" (want 0.5000 %.4f 1.0000)\n", 0.75 + late);
      expect(ons.size() >= 3 && std::fabs(ons[0] - 2 * stepBeats) < 0.003 && std::fabs(ons[1] - (3 * stepBeats + late)) < 0.003 &&
                 std::fabs(ons[2] - 4 * stepBeats) < 0.003,
             "plasma arp: pressed between steps, the swing follows the song grid");
    }
    // The song loops back (beat 1.1 -> 0) while the key is held: the arp rejoins the song grid.
    {
      pl::Arp arp;
      pl::ArpSlot s;
      s.rate = 7;
      s.retrigLaunch = false;
      arp.noteOn(60, 0.8f);
      float off[32] = {};
      std::vector<int> scale;
      std::vector<double> ons;
      double beat = 0;
      for (int pos = 0; pos < static_cast<int>(kSr); pos += 64) {
        if (beat >= 1.1) beat -= 1.1;
        pl::SeqOut out;
        arp.run(s, off, 64, kSr, 120, beat, true, 1.0, 2, scale, 0, out);
        for (int i = 0; i < out.n; ++i)
          if (out.ev[i].on) ons.push_back(beat + out.ev[i].offset * 2.0 / kSr);
        beat += 64 * 2.0 / kSr;
      }
      bool onGrid = ons.size() >= 8;
      for (double b : ons) {
        const double g = std::round(b / stepBeats);
        const double want = g * stepBeats + ((static_cast<long long>(g) & 1) ? stepBeats / 3.0 : 0.0);
        const double alt = (g - 1) * stepBeats + stepBeats / 3.0;  // a swung odd step just before g
        onGrid = onGrid && (std::fabs(b - want) < 0.003 || std::fabs(b - alt) < 0.003);
      }
      expect(onGrid, "plasma arp: after the song loops, the steps stay on the song grid");
    }
  }
  // ------------------------------------------------ 4. Plasma clip player swing
  {
    namespace pl = packs::plasma;
    pl::ClipPlayer cp;
    pl::ClipSlot c;
    c.lengthBeats = 1;
    for (int k = 0; k < 4; ++k) {
      pl::ClipNote n;
      n.start = 0.25f * static_cast<float>(k);
      n.len = 0.1f;
      n.key = 60 + k;
      c.notes.push_back(n);
    }
    cp.noteOn(60, 0.8f);
    float off[8] = {};
    float lanes[32];
    std::vector<double> ons;
    for (int pos = 0; pos < static_cast<int>(kSr); pos += 64) {
      pl::SeqOut out;
      cp.run(c, off, 64, kSr, 120, pos * 2.0 / kSr, false, 1.0, 2, out, lanes, 32);
      for (int i = 0; i < out.n; ++i)
        if (out.ev[i].on) ons.push_back((pos + out.ev[i].offset) * 2.0 / kSr);
    }
    std::printf("# plasma clip player swing 1 (1/16): notes at beats");
    for (double b : ons) std::printf(" %.4f", b);
    std::printf("\n");
    expect(ons.size() >= 4 && std::fabs(ons[1] - (0.25 + 0.25 / 3)) < 0.003 && std::fabs(ons[2] - 0.5) < 0.003 &&
               std::fabs(ons[3] - (0.75 + 0.25 / 3)) < 0.003,
           "plasma clip player: odd 16ths start a third of a 16th late at full swing");
    bool everyPass = ons.size() >= 8;  // two whole passes of the 1-beat clip (and the third one starting)
    for (size_t k = 0; everyPass && k < 8; ++k) {
      const double want = static_cast<double>(k / 4) + 0.25 * static_cast<double>(k % 4) + ((k & 1) ? 0.25 / 3 : 0.0);
      everyPass = std::fabs(ons[k] - want) < 0.003;
    }
    expect(everyPass, "plasma clip player: each loop pass plays its first note (the loop wraps inside a block)");
  }
  // ------------------------------------------------ 5. Hex sequencer swing
  {
    namespace hx = packs::synths::hex;
    auto times = [&](bool swing) {
      PluginState s = fresh("gl.hexsynth");
      s.params[hx::HxSeqPlay] = 1;
      s.params[hx::HxSeqSwing] = swing ? 1.f : 0.f;
      for (int k = 0; k < 16; ++k) s.params[static_cast<size_t>(hx::HxSeqStep + k)] = (k & 1) ? 0.76f : 0.52f;  // root, alternating with a higher step
      s.params[hx::HxAmpA] = 0;
      s.params[hx::HxAmpD] = 0.15f;
      s.params[hx::HxAmpS] = 0;
      std::vector<int> t;
      auto L = renderGen(s, {{1024, true, 1, 60, 1.0f}}, static_cast<int>(kSr * 4), 128, 120, [](Generator&, int) {});
      // Onsets: 64-sample energy rising sharply after a dip.
      const size_t w = 64;
      std::vector<double> e;
      for (size_t i = 0; i + w <= L.size(); i += w) e.push_back(peakIn(L, i, i + w));
      for (size_t k = 2; k < e.size(); ++k)
        if (e[k] > 2.5 * e[k - 1] && e[k] > 0.02 && (t.empty() || static_cast<int>(k * w) - t.back() > 1000)) t.push_back(static_cast<int>(k * w));
      return t;
    };
    auto st = times(false), sw = times(true);
    std::printf("# hex seq straight onsets:");
    for (size_t k = 0; k < std::min<size_t>(st.size(), 6); ++k) std::printf(" %d", st[k]);
    std::printf("\n# hex seq swing onsets:   ");
    for (size_t k = 0; k < std::min<size_t>(sw.size(), 6); ++k) std::printf(" %d", sw[k]);
    std::printf("\n");
    const double step = kSr * 15.0 / 120.0;
    auto near = [](double a, double b) { return std::fabs(a - b) < 160; };  // 64-sample analysis windows + 128-sample control ticks
    expect(st.size() >= 5 && near(st[1] - st[0], step) && near(st[2] - st[0], 2 * step) && near(st[4] - st[0], 4 * step),
           "hex sequencer: straight steps every 16th");
    expect(sw.size() >= 5 && near(sw[1] - sw[0], 1.25 * step) && near(sw[2] - sw[0], 2 * step) && near(sw[3] - sw[0], 3.25 * step) &&
               near(sw[4] - sw[0], 4 * step),
           "hex sequencer: swing delays the off-beat 16ths by a quarter step, downbeats stay on the grid");
  }
  // ------------------------------------------------ 6. Mini Pulse swing (song grid)
  {
    namespace pu = packs::mini::pulse;
    for (float sw : {0.0f, 0.5f, 1.0f}) {
      PluginState s = fresh("gl.minipulse");
      for (int k = 0; k < pu::kSteps; ++k) s.params[static_cast<size_t>(pu::Step0 + k)] = 1;
      s.params[pu::Rate] = 2;  // 1/16
      s.params[pu::Swing] = sw;
      s.params[pu::Attack] = s.params[pu::Release] = static_cast<float>(findPlugin("gl.minipulse")->params[pu::Attack].min);
      auto fx = packs::createEffect("gl.minipulse");
      const int total = static_cast<int>(kSr * 2);
      std::vector<float> L(static_cast<size_t>(total), 1.f), R = L;
      ProcessContext ctx;
      ctx.sampleRate = kSr;
      ctx.tempo = 120;
      ctx.playing = true;
      for (int pos = 0; pos < total; pos += 256) {
        ctx.tick = pos * 2.0 * ctx.ppq / kSr;
        fx->process(s, ctx, L.data() + pos, R.data() + pos, std::min(256, total - pos));
      }
      std::vector<int> opens;
      for (int i = 1; i < total; ++i)
        if (L[static_cast<size_t>(i)] > 0.5f && L[static_cast<size_t>(i - 1)] <= 0.5f) opens.push_back(i);
      const double step = kSr * 15.0 / 120.0;
      double worst = 0;
      for (size_t k = 0; k < opens.size(); ++k) {
        const double g = std::round(opens[k] / step - ((k & 1) ? 0.5 * sw : 0.0));
        const double want = g * step + ((static_cast<long long>(g) & 1) ? 0.5 * sw * step : 0.0);
        worst = std::max(worst, std::fabs(opens[k] - want));
      }
      std::printf("# mini pulse swing %.2f: %zu gate openings, first at", sw, opens.size());
      for (size_t k = 0; k < std::min<size_t>(opens.size(), 4); ++k) std::printf(" %d", opens[k]);
      std::printf(", worst vs grid %.1f samples\n", worst);
      expect(opens.size() >= 15 && worst < 200, "mini pulse: swing " + std::to_string(sw) + " opens on the swung grid");
    }
  }
  // ------------------------------------------------ 7. Seltmann step swing
  {
    namespace sm = packs::seltmann;
    sm::Pattern p;
    p.unitBeats = 0.25;
    p.steps.resize(8);
    bool ok = true;
    for (int i = 0; i < 8; ++i) {
      const double want = i * 0.25 + ((i & 1) ? 0.6 * 0.5 * 0.25 : 0.0);
      ok = ok && std::fabs(sm::stepStart(p, i, 0.6) - want) < 1e-9;
    }
    std::printf("# seltmann swing 0.6: step 1 at %.4f beats, step 2 at %.4f\n", sm::stepStart(p, 1, 0.6), sm::stepStart(p, 2, 0.6));
    expect(ok, "seltmann: swing delays odd 16ths by swing x half a step");
  }
  // ------------------------------------------------ 8. Instruments: velocity sweep
  {
    int tested = 0;
    std::string flat, zero, hang;
    for (const PluginInfo& info : pluginCatalog()) {
      if (info.kind != PluginKind::Generator) continue;
      PluginState s = fresh(info.type);
      for (size_t k = 0; k < info.params.size(); ++k)  // a master limiter would hide the level change
        if (info.params[k].name == "Limiter") s.params[k] = 0;
      const int total = static_cast<int>(kSr * 1.8);
      std::vector<double> pk;
      double tail = 0;
      for (float v : {0.0f, 0.25f, 0.5f, 1.0f}) {
        auto L = renderGen(s, {{0, true, 1, 60, v}, {static_cast<int>(kSr * 0.4), false, 1, 60, 0}}, total);
        pk.push_back(peakIn(L, 0, static_cast<size_t>(kSr * 0.6)));
        if (v == 1.0f) tail = peakIn(L, static_cast<size_t>(kSr * 1.6), L.size());
      }
      if (pk[3] < 1e-4) continue;  // silent by default (needs a sample or data)
      ++tested;
      std::printf("# vel %-22s 0:%.4f 0.25:%.4f 0.5:%.4f 1:%.4f tail:%.5f\n", info.type.c_str(), pk[0], pk[1], pk[2], pk[3], tail);
      if (!(pk[1] <= pk[2] * 1.001 && pk[2] <= pk[3] * 1.001)) flat += " " + info.type + "(non-monotonic)";
      else if (pk[3] < pk[1] * 1.05) flat += " " + info.type + "(no velocity response)";
      if (pk[0] > pk[3] * 0.5) zero += " " + info.type;
      if (tail > pk[3] * 0.25) hang += " " + info.type;
    }
    std::printf("# instruments tested: %d\n# velocity flat or non-monotonic:%s\n# velocity 0 louder than half of full:%s\n# still loud 1.2 s after release:%s\n",
                tested, flat.c_str(), zero.c_str(), hang.c_str());
    expect(tested >= 30 && hang.empty(), "every built-in instrument goes quiet after its note is released");
    // Known exceptions, by design: Plasma's init patch has no velocity routing (velocity is a
    // modulation source); Step Bass plays velocity as an accent above a threshold.
    expect(flat.find("gl.drumkit") == std::string::npos, "drum model: velocity sets the level of every pad, driven ones too");
  }
  // ------------------------------------------------ 9. Drum model: every pad follows velocity
  {
    PluginState s = fresh("gl.drumkit");
    const PluginInfo* info = findPlugin("gl.drumkit");
    for (size_t k = 0; k < info->params.size(); ++k)
      if (info->params[k].name == "Limiter") s.params[k] = 0;
    std::string bad;
    for (int pad = 0; pad < 16; ++pad) {
      std::vector<double> pk;
      for (float v : {0.25f, 0.5f, 1.0f}) {
        auto L = renderGen(s, {{0, true, 1, 60.0 + pad, v}}, static_cast<int>(kSr * 0.3));
        pk.push_back(peakIn(L, 0, L.size()));
      }
      if (!rising(pk, 1.05)) bad += " " + std::to_string(pad + 1);
    }
    std::printf("# drum model pads not following velocity:%s\n", bad.c_str());
    expect(bad.empty(), "drum model: all 16 default pads get louder with velocity");
  }
  return finish();
}

// K-257 phase 2: the Shaper's lanes of up to 64 steps with their own step size (1/32 to 1 bar,
// triplets) and swing, per-step chance (seeded from the position: an export repeats exactly) and
// repeats (2 to 4 hits), 8 patterns picked by automation, the chain or notes. States that use none
// of it render bit-identical to the engine as it was (tests/shaper_legacy.inc).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "engine/Dsp.h"
#include "engine/Effects.h"
#include "engine/Generators.h"
#include "engine/ShaperSteps.h"

using namespace gl;
using namespace gl::pp;

namespace legacy {
using namespace gl::dsp;
float param(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}
int iparam(const PluginState& s, int i, int dflt = 0) { return static_cast<int>(std::lround(param(s, i, static_cast<float>(dflt)))); }
inline double coef(double seconds, double sr) { return std::exp(-1.0 / std::max(1.0, seconds * sr)); }
struct SyncClock {
  static double beat(const ProcessContext& ctx, int i) {
    double spb = 60.0 / std::max(1.0, ctx.tempo) * ctx.sampleRate;
    return ctx.tick / std::max(1, ctx.ppq) + i / spb;
  }
};
#include "shaper_legacy.inc"
}  // namespace legacy

namespace {

constexpr double kSr = 48000, kTempo = 120;  // one beat = 24000 samples; a 1/16 step = 6000

struct Out {
  std::vector<float> L, R;
  bool operator==(const Out& o) const { return L == o.L && R == o.R; }
};

// Renders `in` through `proc` in blocks of `block`, the transport playing from the song start
// (or stopped: the plugin's own free clock). `hits(at, bus)` may add the block's notes.
Out run(const std::function<void(const PluginState&, const ProcessContext&, float*, float*, int)>& proc, const PluginState& st,
        const std::vector<float>& in, bool playing = true, int block = 256, const std::function<void(size_t, NoteHitBus&)>& hits = nullptr) {
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = kTempo;
  ctx.playing = playing;
  NoteHitBus bus;
  Out o{in, in};
  for (size_t at = 0; at < in.size(); at += static_cast<size_t>(block)) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.size() - at));
    ctx.tick = static_cast<double>(at) / (kSr * 60.0 / kTempo) * ctx.ppq;
    ctx.jumped = at == 0;
    bus.clear();
    if (hits) hits(at, bus);
    ctx.hits = &bus;
    proc(st, ctx, o.L.data() + at, o.R.data() + at, n);
  }
  return o;
}

Out render(const PluginState& st, const std::vector<float>& in, bool playing = true, int block = 256,
           const std::function<void(size_t, NoteHitBus&)>& hits = nullptr) {
  auto fx = createEffect("gl.shaper");
  return run([&](const PluginState& s, const ProcessContext& c, float* l, float* r, int n) { fx->process(s, c, l, r, n); }, st, in, playing, block, hits);
}

Out renderLegacy(const PluginState& st, const std::vector<float>& in, bool playing = true, int block = 256) {
  auto fx = std::make_unique<legacy::LegacyShaper>();
  return run([&](const PluginState& s, const ProcessContext& c, float* l, float* r, int n) { fx->process(s, c, l, r, n); }, st, in, playing, block);
}

std::vector<float> noise(size_t n, uint32_t seed = 1) {
  std::vector<float> v(n);
  for (float& x : v) seed = seed * 1664525u + 1013904223u, x = static_cast<float>(static_cast<int32_t>(seed)) / 4.3e9f;
  return v;
}

uint32_t rnd(uint32_t& r) { return r = r * 1664525u + 1013904223u, r >> 8; }

// The volume gate's level at sample i (a DC input of 0.5, filter off, centre pan at unity: L = 0.5 * vol).
double gateAt(const Out& o, size_t i) { return o.L[i] / 0.5; }

PluginState gateState() {
  PluginState st = defaultPluginState("gl.shaper");
  st.params[ShSmooth] = 0.5f;  // the fastest smoothing: levels settle within a few ms
  return st;
}

void setBank(PluginState& st, const std::function<void(shsteps::Bank&)>& edit) {
  auto b = std::make_unique<shsteps::Bank>();
  shsteps::parse(st.data.c_str(), *b);
  for (int l = 0; l < shsteps::kLanes; ++l)
    for (int k = 0; k < kShaperPoints; ++k) b->val[0][l][k] = st.params[static_cast<size_t>(shsteps::laneBase(l) + k)];
  edit(*b);
  for (int l = 0; l < shsteps::kLanes; ++l)
    for (int k = 0; k < kShaperPoints; ++k) st.params[static_cast<size_t>(shsteps::laneBase(l) + k)] = b->val[0][l][k];
  st.data = shsteps::write(*b);
}

}  // namespace

int main() {
  const PluginInfo* info = findPlugin("gl.shaper");
  expect(info && static_cast<int>(info->params.size()) == ShCount, "the Shaper has its phase 2 parameters");
  expect(info->params[ShVolSteps].choices.size() == 64 && info->params[ShVolSteps].def == kShaperPoints - 1, "lanes run up to 64 steps, 16 by default");
  expect(info->params[ShVolRate].choices.size() == static_cast<size_t>(kShaperRates) && info->params[ShVolRate].choices[0] == "Auto" &&
             info->params[ShVolRate].choices.back() == "1 bar",
         "step sizes from Auto and 1/32 to 1 bar");

  // 1. Bit-identical: every factory preset and 40 random states over the controls that were there
  //    before, played and stopped, against the engine as it was.
  {
    const std::vector<float> x = noise(48000 * 3);
    int same = 0, total = 0;
    for (size_t p = 0; p < info->presets.size(); ++p) {
      const PluginState st = presetState(*info, p);
      for (bool play : {true, false}) same += render(st, x, play) == renderLegacy(st, x, play), ++total;
    }
    uint32_t r = 7;
    for (int t = 0; t < 40; ++t) {
      PluginState st = defaultPluginState("gl.shaper");
      for (int k = 0; k < ShVolRate; ++k) {  // only the controls from before phase 2
        const ParamDesc& d = info->params[static_cast<size_t>(k)];
        const double u = (rnd(r) % 10001) / 10000.0;
        st.params[static_cast<size_t>(k)] = static_cast<float>(d.kind == ParamKind::Choice ? std::round(u * d.max) : d.min + u * (d.max - d.min));
      }
      st.params[ShVolSteps] = static_cast<float>(rnd(r) % 16);  // lengths as they were (1..16)
      st.params[ShCutSteps] = static_cast<float>(rnd(r) % 16);
      st.params[ShPanSteps] = static_cast<float>(rnd(r) % 16);
      st.params[ShInput] = 0, st.params[ShOutput] = 0;
      const bool play = t % 3 != 2;
      const bool ok = render(st, x, play, 97 + t * 13) == renderLegacy(st, x, play, 97 + t * 13);
      if (!ok) std::printf("# random state %d differs\n", t);
      same += ok, ++total;
    }
    // A state saved before phase 2 (no phase 2 parameters at all) loads and plays the same.
    PluginState old = presetState(*info, 3);
    old.params.resize(static_cast<size_t>(ShVolRate));
    normalizePluginState(old);
    same += render(old, x) == renderLegacy(presetState(*info, 3), x), ++total;
    std::printf("# bit-identical renders: %d of %d\n", same, total);
    expect(same == total, "states without the phase 2 controls render bit-identical to the engine as it was");
  }

  const std::vector<float> dc(24000 * 8, 0.5f);

  // 2. A 40-step volume lane: steps past 16 come from the step bank, and the lane repeats every 40 steps.
  {
    PluginState st = gateState();
    st.params[ShVolSteps] = 39;
    setBank(st, [](shsteps::Bank& b) {
      for (int k = 0; k < 40; ++k) b.val[0][0][k] = k % 5 == 0 ? 1.f : 0.f;  // on every 5th step
    });
    st.params[ShLength] = 0;  // 1/4: a step is 1/16 beat = 1500 samples
    const Out o = render(st, std::vector<float>(1500 * 90, 0.5f));
    bool ok = true;
    for (int step = 0; step < 85; ++step) {
      const double g = gateAt(o, static_cast<size_t>(step * 1500 + 1000));
      const bool on = (step % 40) % 5 == 0;
      ok = ok && (on ? std::fabs(g - 1) < 2e-3 : std::fabs(g) < 2e-3);
    }
    expect(ok, "a 40-step lane plays its steps past 16 and repeats every 40 steps");
  }

  // 3. Step sizes: 1/16 triplets (4000 samples) and 1 bar (96000 samples).
  {
    PluginState st = gateState();
    for (int k = 0; k < kShaperPoints; ++k) st.params[static_cast<size_t>(ShVol + k)] = k % 2 ? 0.f : 1.f;
    st.params[ShVolRate] = 4;  // 1/16T
    const Out o = render(st, dc);
    bool ok = true;
    for (int step = 0; step < 20; ++step) {
      const double g = gateAt(o, static_cast<size_t>(step * 4000 + 3000));
      ok = ok && std::fabs(g - (step % 2 ? 0 : 1)) < 2e-3;
    }
    expect(ok, "1/16 triplet steps: 4000 samples each at 120 BPM");
    st.params[ShVolRate] = 11;  // 1 bar
    const Out b = render(st, std::vector<float>(96000 * 2 + 3000, 0.5f));
    expect(std::fabs(gateAt(b, 90000) - 1) < 2e-3 && std::fabs(gateAt(b, 96000 + 20000)) < 2e-3 && std::fabs(gateAt(b, 96000 * 2 + 2000) - 1) < 2e-3,
           "1-bar steps: the first bar open, the second shut, the third open");
  }

  // 4. Swing: with 1/16 steps (6000 samples) at 100% the second step of each pair starts at 9000.
  {
    PluginState st = gateState();
    for (int k = 0; k < kShaperPoints; ++k) st.params[static_cast<size_t>(ShVol + k)] = k % 2 ? 0.f : 1.f;
    st.params[ShVolRate] = 3;  // 1/16
    st.params[ShVolSwing] = 1;
    const Out o = render(st, dc);
    std::printf("# swing: 5000 %.3f, 8500 %.3f, 9500 %.3f, 11500 %.3f, 12500 %.3f\n", gateAt(o, 5000), gateAt(o, 8500), gateAt(o, 9500), gateAt(o, 11500),
                gateAt(o, 12500));
    expect(std::fabs(gateAt(o, 8500) - 1) < 2e-3 && std::fabs(gateAt(o, 9500)) < 2e-3 && std::fabs(gateAt(o, 12500) - 1) < 2e-3,
           "swing 100%: the off step starts three quarters into the pair");
    st.params[ShVolSwing] = 0.5f;  // 50%: at 7500
    const Out h = render(st, dc);
    expect(std::fabs(gateAt(h, 7000) - 1) < 2e-3 && std::fabs(gateAt(h, 8000)) < 2e-3, "swing 50%: the off step starts at 5/8 of the pair");
  }

  // 5. Chance: deterministic from the position, the same in every render and block size, about half the steps at 50%.
  {
    PluginState st = gateState();
    st.params[ShVolRate] = 3;  // 1/16: 6000 samples
    setBank(st, [](shsteps::Bank& b) {
      for (int k = 0; k < 16; ++k) b.chance[0][0][k] = 50;
    });
    const std::vector<float> x(6000 * 64, 0.5f);
    const Out a = render(st, x), b = render(st, x), c = render(st, x, true, 61);
    int on = 0;
    for (int step = 0; step < 64; ++step) on += gateAt(a, static_cast<size_t>(step * 6000 + 4000)) > 0.5;
    std::printf("# chance 50%%: %d of 64 steps played\n", on);
    double blockDiff = 0;  // another block size: the same steps play (the clock rounds an edge a sample either way)
    for (int step = 0; step < 64; ++step) {
      const size_t i = static_cast<size_t>(step * 6000 + 3000);
      blockDiff = std::max(blockDiff, std::fabs(gateAt(a, i) - gateAt(c, i)));
    }
    std::printf("# chance, block 256 against 61: largest difference mid-step %.3g\n", blockDiff);
    expect(a == b, "chance repeats exactly: two renders are the same sample for sample");
    expect(blockDiff < 1e-4, "chance does not depend on the block size");
    expect(on > 20 && on < 44, "chance 50%: about half the steps play");
    bool repeatsDiffer = false;  // the dice change from one pass of the lane to the next
    for (int step = 0; step < 16; ++step)
      repeatsDiffer = repeatsDiffer || (gateAt(a, static_cast<size_t>(step * 6000 + 4000)) > 0.5) != (gateAt(a, static_cast<size_t>((step + 16) * 6000 + 4000)) > 0.5);
    expect(repeatsDiffer, "each pass of the lane rolls its own dice");
    setBank(st, [](shsteps::Bank& bk) {
      for (int k = 0; k < 16; ++k) bk.chance[0][0][k] = 0;
    });
    const Out z = render(st, x);
    double mx = 0;
    for (size_t i = 3000; i < z.L.size(); ++i) mx = std::max(mx, std::fabs(gateAt(z, i)));
    expect(mx < 1e-3, "chance 0%: no step plays");
  }

  // 6. Repeats: four hits in a 6000-sample step: open 0..750, shut 750..1500, open 1500..2250 ...
  {
    PluginState st = gateState();
    st.params[ShVolRate] = 3;
    setBank(st, [](shsteps::Bank& b) { b.hits[0][0][0] = 4; });
    const Out o = render(st, dc);
    expect(std::fabs(gateAt(o, 500) - 1) < 2e-3 && std::fabs(gateAt(o, 1300)) < 2e-3 && std::fabs(gateAt(o, 2000) - 1) < 2e-3 && std::fabs(gateAt(o, 5800)) < 2e-3 &&
               std::fabs(gateAt(o, 6000 + 3000) - 1) < 2e-3,
           "a step of 4 repeats plays four hits, the next step plays whole");
  }

  // 7. Patterns: pattern 2 shut, picked by the Pattern control (automation).
  PluginState two = gateState();
  setBank(two, [](shsteps::Bank& b) {
    for (int k = 0; k < shsteps::kSteps; ++k) b.val[1][0][k] = 0.f;
  });
  {
    PluginState st = two;
    expect(std::fabs(gateAt(render(st, dc), 30000) - 1) < 2e-3, "pattern 1 plays by default");
    st.params[ShPattern] = 1;
    expect(std::fabs(gateAt(render(st, dc), 30000)) < 2e-3, "the Pattern control picks pattern 2");
  }

  // 8. The chain: slots 1, 2 with Length 1/4 (one beat each): beat 1 open, beat 2 shut, beat 3 open.
  {
    PluginState st = two;
    st.params[ShLength] = 0;
    st.params[ShChain] = 1;
    setBank(st, [](shsteps::Bank& b) { b.chain[0] = 0, b.chain[1] = 1; });
    const Out o = render(st, dc);
    expect(std::fabs(gateAt(o, 12000) - 1) < 2e-3 && std::fabs(gateAt(o, 36000)) < 2e-3 && std::fabs(gateAt(o, 60000) - 1) < 2e-3 &&
               std::fabs(gateAt(o, 84000)) < 2e-3,
           "the chain plays pattern 1, then 2, one Length each, and loops");
    st.params[ShChain] = 0;
    expect(std::fabs(gateAt(render(st, dc), 36000) - 1) < 2e-3, "with the chain off the chosen pattern plays");
  }

  // 9. Notes: the note above the first switches to pattern 2 at its sample, from a MIDI Out port or the MIDI input.
  {
    PluginState st = two;
    st.params[ShNoteSrc] = 1;
    st.params[ShNoteBase] = 36;
    auto at = [](size_t when, int note, NoteHitBus::Kind kind, int port) {
      return [=](size_t blockAt, NoteHitBus& bus) {
        if (when >= blockAt && when < blockAt + 256) bus.add(1, static_cast<int>(when - blockAt), note, port, kind);
      };
    };
    const Out o = render(st, dc, true, 256, at(30000, 37, NoteHitBus::Note, 0));
    expect(std::fabs(gateAt(o, 29900) - 1) < 2e-3 && std::fabs(gateAt(o, 31000)) < 2e-3 && std::fabs(gateAt(o, 90000)) < 2e-3,
           "a note picks pattern 2 at its sample and it stays");
    const Out back = render(st, dc, true, 256, [&](size_t b, NoteHitBus& bus) {
      at(30000, 37, NoteHitBus::Note, 0)(b, bus);
      at(60000, 36, NoteHitBus::Note, 0)(b, bus);
    });
    expect(std::fabs(gateAt(back, 45000)) < 2e-3 && std::fabs(gateAt(back, 70000) - 1) < 2e-3, "the first note picks pattern 1 again");
    expect(std::fabs(gateAt(render(st, dc, true, 256, at(30000, 37, NoteHitBus::Input, -1)), 40000) - 1) < 2e-3, "MIDI Out ports: the MIDI input is not heard");
    expect(std::fabs(gateAt(render(st, dc, true, 256, at(30000, 50, NoteHitBus::Note, 0)), 40000) - 1) < 2e-3, "a note outside the eight is not heard");
    st.params[ShNotePort] = 3;
    expect(std::fabs(gateAt(render(st, dc, true, 256, at(30000, 37, NoteHitBus::Note, 0)), 40000) - 1) < 2e-3, "another port is not heard");
    st.params[ShNoteSrc] = 2;
    expect(std::fabs(gateAt(render(st, dc, true, 256, at(30000, 37, NoteHitBus::Input, -1)), 40000)) < 2e-3, "MIDI input: its notes pick the pattern");
    st.params[ShNoteSrc] = 0;
    expect(std::fabs(gateAt(render(st, dc, true, 256, at(30000, 37, NoteHitBus::Input, -1)), 40000) - 1) < 2e-3, "Off: notes do nothing");
  }

  // 10. The step text: written and read back, empty when nothing differs from the defaults, saved with the state.
  {
    auto b = std::make_unique<shsteps::Bank>();
    shsteps::clear(*b);
    expect(shsteps::write(*b).empty(), "an untouched bank writes nothing (the state text stays empty)");
    b->val[0][0][20] = 0.25f, b->val[3][2][0] = -0.5f, b->chance[5][1][63] = 12, b->hits[7][0][9] = 3, b->chain[0] = 2, b->chain[4] = 7;
    const std::string text = shsteps::write(*b);
    auto c = std::make_unique<shsteps::Bank>();
    shsteps::parse(text.c_str(), *c);
    std::printf("# bank text:\n%s", text.c_str());
    expect(c->val[0][0][20] == 0.25f && c->val[3][2][0] == -0.5f && c->chance[5][1][63] == 12 && c->hits[7][0][9] == 3 && c->chain[0] == 2 &&
               c->chain[1] == -1 && c->chain[4] == 7 && c->chainLen == 5 && shsteps::write(*c) == text,
           "the step bank reads back what it wrote");
    shsteps::parse("SH1\nv 9 0 0 1\nq 0 5 0 1\nr 0 0 70 2\nc 9 x\nv 0 0 -3 5000 5000 5000 5000\n", *c);
    expect(c->val[0][0][0] == 0.5f && c->val[0][0][4] == 1.f && c->chainLen == 0 && !c->anyHits[0][0] && !c->anyChance[0][0],
           "out-of-range lines and values are ignored, the rest is read");
    PluginState st = defaultPluginState("gl.shaper");
    st.data = text;
    normalizePluginState(st);
    expect(st.data == text, "normalising keeps the step text");
  }
  return finish();
}

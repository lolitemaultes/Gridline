// Gridline - K-142 (0.7.8): an instrument that rests plays its next note exactly as it would have
// had it never rested. While it rests the engine leaves its blocks out and calls
// Generator::rested(n) instead, so every free-running part of it (an LFO, a drifting oscillator,
// a sample-and-hold clock, a random generator, a phase) must move on there just as n silent
// frames of processing would have moved it.
//
// For every built-in instrument, in its default sound and in each factory preset: one note, a
// long silence (the instrument rests in it), a second note. Played twice, realtime, the transport
// stopped: once with resting on, once with resting off (it never rests). The second note must be
// bit-identical; where an instrument's state is a sum of floats that n frames at once cannot
// reproduce exactly, it is listed below and must stay within -120 dBFS.
//   gridline_tests_k142_rested [--only <type>] [--verbose] [--no-rested]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Demo.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/Denormals.h"
#include "engine/Effects.h"
#include "engine/Program.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr int kBlock = 256;

constexpr double kNear = 1e-6;   // -120 dBFS
constexpr double kPre = 3.2e-5;  // -90 dBFS, the engine's faint level
bool gNoRested = false;          // --no-rested: leave effects out without Effect::rested (as before K-142 0.7.8)

struct Run {
  std::vector<float> L, R;
  uint64_t restBlocks = 0;
};

Run play(const PluginState& st, bool rest) {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Test";
  c.plugin = st;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setRestInstruments(rest);
  e.setProgram(b.build(s));
  e.setLooping(false);
  Run r;
  const int frames = 16 * kSr;
  r.L.assign(static_cast<size_t>(frames), 0.f);
  r.R.assign(static_cast<size_t>(frames), 0.f);
  // note 1 at 0.5 s for 0.3 s; note 2 (another pitch) at 11 s for 0.5 s
  const int on1 = kSr / 2 / kBlock * kBlock, off1 = on1 + kSr * 3 / 10 / kBlock * kBlock;
  const int on2 = 11 * kSr / kBlock * kBlock, off2 = on2 + kSr / 2 / kBlock * kBlock;
  for (int at = 0; at < frames; at += kBlock) {
    if (at == on1) e.noteOn(c.id, 60, 0.9f);
    if (at == off1) e.noteOff(c.id, 60);
    if (at == on2) e.noteOn(c.id, 67, 0.8f);
    if (at == off2) e.noteOff(c.id, 67);
    e.process(r.L.data() + at, r.R.data() + at, kBlock);
  }
  if (auto rt = b.channelRuntime(c.id)) r.restBlocks = rt->restBlocks.load();
  return r;
}

// An effect the engine may leave out, by either of its rules: GitHub #66 (it is one of the engine's
// own (effectRestsWhenStopped) or says restsWhenFaint, it is fed exact silence and its output stayed
// at or below the faint level for kRestHoldFaint, or its faintGap if longer) and K-64 (exact silence
// in and out for effectRestSeconds, playing or not). One second of sound, the silence (left out
// once a rule holds; the engine calls Effect::rested for every block it leaves out), a second sound
// at 20 s. Returns the largest difference from 20 s on against a run never left out, and whether
// it was left out at all.
struct FxRun {
  bool dozed = false, k64 = false;
  float diff = 0;
};
void fxBlock(Effect& fx, const PluginState& st, const ProcessContext& ctx, int at, float* L, float* R) {
  const ChannelAux aux{};
  for (int i = 0; i < kBlock; ++i) {
    const int t = at + i;
    float x = 0;
    if (t < kSr) x = static_cast<float>(std::sin(t * 2 * 3.14159265358979 * 220.0 / kSr) * 0.5 * std::exp(-2.0 * t / kSr));
    else if (t >= 20 * kSr && t < 21 * kSr)
      x = static_cast<float>(std::sin(t * 2 * 3.14159265358979 * 330.0 / kSr) * 0.4);
    L[i] = x;
    R[i] = x * 0.9f;
  }
  fx.processAux(st, aux, ctx, L, R, kBlock);
}
FxRun fxCompare(const std::string& type, const PluginState& st) {
  FxRun r;
  auto a = createEffect(type), b = createEffect(type);
  if (!a || !b) return r;
  ScopedFlushDenormals ftz;  // as on the audio thread
  a->prepareHost(st, kSr);
  b->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  const ChannelAux aux{};
  float aL[kBlock], aR[kBlock], bL[kBlock], bR[kBlock];
  int64_t faint = 0, quiet = 0;
  const int64_t restFrames = static_cast<int64_t>(effectRestSeconds(st) * kSr);
  const bool own = effectRestsWhenStopped(type);
  bool dozing = false, selfSound = false;
  for (int at = 0; at < 24 * kSr; at += kBlock) {
    const bool silentIn = at + kBlock <= 20 * kSr && at >= kSr;
    if (dozing && !silentIn) dozing = false;  // sound comes in: it runs again
    const bool may = own || a->restsWhenFaint();
    if (dozing || (restFrames > 0 && silentIn && quiet >= restFrames)) {
      if (!dozing) r.k64 = true;
      r.dozed = true;
      if (!gNoRested) a->rested(st, aux, ctx, kBlock);
      std::fill(aL, aL + kBlock, 0.f), std::fill(aR, aR + kBlock, 0.f);
    } else {
      fxBlock(*a, st, ctx, at, aL, aR);
      float pk = 0;
      for (int i = 0; i < kBlock; ++i) pk = std::max({pk, std::fabs(aL[i]), std::fabs(aR[i])});
      quiet = silentIn && pk == 0 ? quiet + kBlock : 0;
      const double gap = static_cast<double>(a->faintGap());
      if (!(silentIn && may)) {
        faint = 0;
      } else if (pk > AudioEngine::kRestFaint) {
        if (static_cast<double>(faint) >= std::max(AudioEngine::kRestSelfAfter * kSr, gap)) selfSound = true;
        faint = 0;
      } else {
        faint += kBlock;
        if (!selfSound && static_cast<double>(faint) >= std::max(AudioEngine::kRestHoldFaint * kSr, gap)) dozing = true;
      }
    }
    fxBlock(*b, st, ctx, at, bL, bR);
    if (at >= 20 * kSr)
      for (int i = 0; i < kBlock; ++i) r.diff = std::max({r.diff, std::fabs(aL[i] - bL[i]), std::fabs(aR[i] - bR[i])});
  }
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  gl::test::hermeticTestEnv("k142rested");
  std::string only;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--only" && i + 1 < argc) only = argv[++i];
    else if (a == "--verbose") verbose = true;
    else if (a == "--no-rested") gNoRested = true;
  }
  int sounds = 0, rested = 0, differ = 0, nearTypes = 0;
  for (const PluginInfo& pi : pluginCatalog()) {
    if (pi.kind != PluginKind::Generator || isHostedType(pi.type)) continue;
    if (!only.empty() && pi.type != only) continue;
    std::vector<std::pair<std::string, PluginState>> states{{"default", defaultPluginState(pi.type)}};
    for (size_t k = 0; k < pi.presets.size(); ++k) states.emplace_back(pi.presets[k].name, presetState(pi, k));
    int typeRested = 0;
    double worst = 0, worstPre = 0;
    std::string worstName;
    for (const auto& [name, st] : states) {
      const Run a = play(st, true), b = play(st, false);
      ++sounds;
      if (a.restBlocks == 0) continue;  // it never rested: nothing to compare
      ++rested, ++typeRested;
      // Before the second note: resting leaves out what is left below the engine's faint level
      // (that is resting itself). From the second note on: it must play as if it never rested.
      const size_t on2 = static_cast<size_t>(11 * kSr / kBlock * kBlock);
      float pre = 0, d = 0;
      size_t first = 0;
      for (size_t i = 0; i < a.L.size(); ++i) {
        const float di = std::max(std::fabs(a.L[i] - b.L[i]), std::fabs(a.R[i] - b.R[i]));
        if (i < on2) {
          pre = std::max(pre, di);
          continue;
        }
        if (di > 0 && d == 0) first = i;
        d = std::max(d, di);
      }
      worstPre = std::max(worstPre, static_cast<double>(pre));
      if (d > worst) worst = d, worstName = name;
      if (verbose)
        std::printf("  %s / %s: rested %llu blocks, before the note %.1f dBFS, the note: max difference %.1f dBFS from %.4f s\n", pi.type.c_str(), name.c_str(),
                    static_cast<unsigned long long>(a.restBlocks), pre > 0 ? 20 * std::log10(pre) : -999.0, d > 0 ? 20 * std::log10(d) : -999.0,
                    static_cast<double>(first) / kSr);
    }
    // Bit-identical, or within -120 dBFS (the faint tail a never-resting run keeps in its filters
    // is a few float steps away from the one left at rest); before the note: only what rests is
    // left out, below the engine's faint level (-90 dBFS).
    const bool same = worst == 0, near = worst <= kNear, preOk = worstPre <= kPre;
    if (!near || !preOk) ++differ;
    if (!same && near) ++nearTypes;
    std::printf("%-20s %3zu sounds, %3d rested: %s%s%s\n", pi.type.c_str(), states.size(), typeRested,
                same ? "bit-identical" : (near ? "within -120 dBFS" : "DIFFERS"),
                worst > 0 ? (" (worst " + worstName + " " + std::to_string(20 * std::log10(worst)) + " dBFS)").c_str() : "",
                preOk ? "" : (" / before the note " + std::to_string(20 * std::log10(worstPre)) + " dBFS").c_str());
  }
  std::printf("%d sounds, %d rested, %d types within -120 dBFS but not bit-identical\n", sounds, rested, nearTypes);
  expect(rested > 0, "instruments rest between the two notes");
  expect(differ == 0, (std::to_string(differ) + " instruments play differently after a rest").c_str());
  // Effects: what the engine may leave out sounds, once woken, as if it had never been.
  int fxDoze = 0, fxDiffer = 0, fxNear = 0;
  const Song demo = makeDemoTrack();  // its effects' own settings too (they are what rests in the demo)
  for (const PluginInfo& pi : pluginCatalog()) {
    if (pi.kind != PluginKind::Effect || isHostedType(pi.type)) continue;
    if (!only.empty() && pi.type != only) continue;
    std::vector<std::pair<std::string, PluginState>> states{{"default", defaultPluginState(pi.type)}};
    for (size_t k = 0; k < pi.presets.size(); ++k) states.emplace_back(pi.presets[k].name, presetState(pi, k));
    for (size_t t = 0; t < demo.mixer.size(); ++t)
      for (size_t k = 0; k < demo.mixer[t].slots.size(); ++k)
        if (demo.mixer[t].slots[k].plugin.type == pi.type)
          states.emplace_back("demo insert " + std::to_string(t) + " slot " + std::to_string(k + 1), demo.mixer[t].slots[k].plugin);
    int dozed = 0;
    double worst = 0;
    std::string worstName;
    for (const auto& [name, st] : states) {
      const FxRun f = fxCompare(pi.type, st);
      if (!f.dozed) continue;
      ++dozed;
      if (f.diff > worst) worst = f.diff, worstName = name;
      if (verbose)
        std::printf("  %s / %s: left out%s, then %.1f dBFS\n", pi.type.c_str(), name.c_str(), f.k64 ? " (K-64)" : "", f.diff > 0 ? 20 * std::log10(f.diff) : -999.0);
    }
    if (!dozed) {
      std::printf("%-20s %3zu sounds: never left out\n", pi.type.c_str(), states.size());
      continue;
    }
    ++fxDoze;
    const bool same = worst == 0, near = worst <= kNear;
    if (!near) ++fxDiffer;
    if (!same && near) ++fxNear;
    std::printf("%-20s %3zu sounds, %3d left out: %s%s\n", pi.type.c_str(), states.size(), dozed, same ? "bit-identical" : (near ? "within -120 dBFS" : "DIFFERS"),
                worst > 0 ? (" (worst " + worstName + " " + std::to_string(20 * std::log10(worst)) + " dBFS)").c_str() : "");
  }
  std::printf("%d effects left out, %d within -120 dBFS but not bit-identical\n", fxDoze, fxNear);
  expect(fxDiffer == 0, (std::to_string(fxDiffer) + " effects sound differently after being left out").c_str());
  return finish();
}

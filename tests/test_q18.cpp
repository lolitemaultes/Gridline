// Q-18: built-in instruments and effects do no allocation on the audio thread
// in their first block, and doing that work on the builder's thread leaves the
// sound exactly as it was. K-59: Junction hosts too (their built-in modules are
// warmed when the host prepares them), also when added during playback.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "packs/modulation/junction.h"

namespace {
thread_local bool gCount = false;
long gAllocs = 0;
}  // namespace
void* operator new(std::size_t n) {
  if (gCount) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCount) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11  // (GCC 11 added this warning)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 256;
constexpr int kBlocks = 112;  // about 0.6 s, the note starts in block 4

struct Take {
  std::vector<float> L, R;
  long firstBlock = 0;
};

Take render(const PluginInfo& pi, bool warmUp) {
  Document d(makeEmptySong());
  ObjectId ch;
  if (pi.kind == PluginKind::Effect) {
    ch = d.addChannel("k", "gl.kick");
    const ObjectId t1 = d.song().mixer[1].id;
    d.routeChannel(ch, t1);
    d.setSlotPlugin(t1, 0, pi.type);
  } else {
    ch = d.addChannel("g", pi.type);
  }
  ProgramBuilder b;
  b.setSampleRate(kSr);
  b.setWarmUp(warmUp);
  AudioEngine e{kSr};
  e.setProgram(b.build(d.song()));
  Take t;
  t.L.resize(kBlocks * kBlock);
  t.R.resize(kBlocks * kBlock);
  e.play();
  for (int i = 0; i < kBlocks; ++i) {
    if (i == 4) e.noteOn(ch, 60, 1.0f);
    gAllocs = 0;
    gCount = true;
    e.process(t.L.data() + i * kBlock, t.R.data() + i * kBlock, kBlock);
    gCount = false;
    if (i == 0) t.firstBlock = gAllocs;
  }
  return t;
}

namespace md = packs::modulation;

// A hand-wired graph (version 3) of built-in modules that size memory in their first block.
std::string wiredPatch(bool fx) {
  md::JGraph g;
  std::vector<uint32_t> chain;
  if (!fx) chain.push_back(md::jAddNode(g, defaultPluginState("gl.hexsynth"), 0, 0));
  for (const char* t : {"gl.chorus", "gl.delay", "gl.reverb", "gl.eq"}) chain.push_back(md::jAddNode(g, defaultPluginState(t), 0, 0));
  uint32_t from = fx ? md::kJInputs : chain[0];
  for (size_t k = fx ? 0 : 1; k < chain.size(); ++k) {
    md::JLink l;
    l.src = from, l.dst = chain[k];
    md::jConnect(g, l, fx);
    from = chain[k];
  }
  md::JLink out;
  out.src = from, out.dst = md::kJOutputs;
  md::jConnect(g, out, fx);
  if (!fx) {
    md::JLink notes;
    notes.kind = md::JKind::Notes, notes.src = md::kJInputs, notes.dst = chain[0];
    md::jConnect(g, notes, fx);
  }
  return md::serializeJunction(g);
}

// K-59: a Junction (instrument or effect; the default chain or a wired graph) added while the
// song plays: the first audio block that runs it allocates nothing.
void testPatchAddedWhilePlaying() {
  for (int wired = 0; wired < 2; ++wired)
    for (int fx = 0; fx < 2; ++fx) {
      Document d(makeEmptySong());
      const ObjectId kick = d.addChannel("k", "gl.kick");
      const ObjectId t1 = d.song().mixer[1].id;
      d.routeChannel(kick, t1);
      ProgramBuilder b;
      b.setSampleRate(kSr);
      AudioEngine e{kSr};
      e.setProgram(b.build(d.song()));
      e.play();
      std::vector<float> L(kBlock), R(kBlock);
      for (int i = 0; i < 8; ++i) e.process(L.data(), R.data(), kBlock);
      ObjectId target = kick;
      if (fx) {
        d.setSlotPlugin(t1, 0, "gl.patcherfx");
        if (wired) d.editMixerTrack(t1, "patch", [&](MixerTrack& m) { m.slots[0].plugin.data = wiredPatch(true); });
      } else {
        target = d.addChannel("j", "gl.patcher");
        if (wired) d.editChannel(target, "patch", [&](Channel& c) { c.plugin.data = wiredPatch(false); });
      }
      e.setProgram(b.build(d.song()));
      long first = -1, noted = -1;
      float peak = 0;
      for (int i = 0; i < 40; ++i) {
        if (i == 1) e.noteOn(target, 60, 1.0f);
        gAllocs = 0;
        gCount = true;
        e.process(L.data(), R.data(), kBlock);
        gCount = false;
        if (i == 0) first = gAllocs;
        if (i == 1) noted = gAllocs;
        for (int k = 0; k < kBlock; ++k) peak = std::max(peak, std::fabs(L[static_cast<size_t>(k)]));
      }
      const std::string what = std::string(fx ? "Junction FX" : "Junction") + (wired ? " (wired graph)" : " (default chain)");
      if (first != 0 || noted != 0) std::printf("# %s: %ld / %ld allocations in its first blocks\n", what.c_str(), first, noted);
      expect(first == 0, ("K-59: " + what + " added during playback: no allocation in its first block").c_str());
      expect(noted == 0, ("K-59: " + what + ": none in its first note either").c_str());
      expect(peak > 1e-4f, ("K-59: " + what + " plays").c_str());
    }
}

}  // namespace

int main() {
  int measured = 0;
  for (const PluginInfo& pi : pluginCatalog()) {
    if (pi.kind == PluginKind::NoteFx || pi.usesSample) continue;
    ++measured;
    const Take warm = render(pi, true);
    expect(warm.firstBlock == 0, (pi.type + ": no allocation in its first block on the audio thread").c_str());
    const Take cold = render(pi, false);
    if (cold.firstBlock == 0) continue;
    // Only the types the builder warms up can differ, so only they are compared.
    expect(warm.L == cold.L && warm.R == cold.R, (pi.type + ": warming it up does not change a sample").c_str());
  }
  expect(measured > 100, "the catalogue was measured");
  testPatchAddedWhilePlaying();
  return finish();
}

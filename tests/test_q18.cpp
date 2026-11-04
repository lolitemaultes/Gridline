// Q-18: built-in instruments and effects do no allocation on the audio thread
// in their first block, and doing that work on the builder's thread leaves the
// sound exactly as it was.
#include <algorithm>
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
#if defined(__GNUC__) && !defined(__clang__)
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

}  // namespace

int main() {
  int measured = 0;
  for (const PluginInfo& pi : pluginCatalog()) {
    if (pi.kind == PluginKind::NoteFx || pi.usesSample) continue;
    ++measured;
    const Take warm = render(pi, true);
    // Patch hosts may hold hosted plugins, so the builder does not run them.
    if (pi.type == "gl.patcher" || pi.type == "gl.patcherfx") continue;
    expect(warm.firstBlock == 0, (pi.type + ": no allocation in its first block on the audio thread").c_str());
    const Take cold = render(pi, false);
    if (cold.firstBlock == 0) continue;
    // Only the types the builder warms up can differ, so only they are compared.
    expect(warm.L == cold.L && warm.R == cold.R, (pi.type + ": warming it up does not change a sample").c_str());
  }
  expect(measured > 100, "the catalogue was measured");
  return finish();
}

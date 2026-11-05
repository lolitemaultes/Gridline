// Gridline: a compiled Program stays alive as long as a block is using it.
//
// The interface publishes a new Program for every edit (a drag on a send level publishes one per
// frame) and the audio thread with its workers may be anywhere inside a block at that moment. A
// block that takes long (a bridged plugin that stops answering, a busy machine) must not lose its
// Program, however many newer ones arrive meanwhile.
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "../src/core/Document.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Effects.h"
#include "Check.h"

using namespace gl;

namespace {

std::atomic<bool> gHold{false};     // the stalling effect waits while this is set
std::atomic<bool> gHolding{false};  // and says so here
std::atomic<int> gStallEvery{0};    // stress: every Nth call waits a little
std::atomic<int> gCalls{0};

// Stands in for a plugin that takes long to answer (no real work; the signal passes through).
class StallFx final : public Effect {
 public:
  void process(const PluginState&, const ProcessContext&, float*, float*, int) override {
    const int every = gStallEvery.load();
    if (every > 0 && gCalls.fetch_add(1) % every == 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    while (gHold.load()) {
      gHolding.store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void reset() override {}
};

std::unique_ptr<Effect> factory(const std::string& type) {
  if (type == "test.stall") return std::make_unique<StallFx>();
  return nullptr;
}

PluginState stallState() {
  PluginState st;
  st.type = "test.stall";
  return st;
}

// 30 instruments on tracks of their own, so a block has many tasks to hand out.
void fillSong(Document& d) {
  const ObjectId pat = d.song().patterns[0].id;
  while (d.song().mixer.size() < 32) d.addMixerTrack();
  for (int k = 0; k < 30; ++k) {
    const ObjectId c = d.addChannel("c" + std::to_string(k), "gl.kick");
    d.routeChannel(c, d.song().mixer[static_cast<size_t>(k + 1)].id);
    d.setStep(pat, c, k % 16, 100);
  }
  d.addClip(pat, 0, 0);
}

Song withStall(const Document& d) {
  Song out = d.song();
  out.mixer[1].slots[0].plugin = stallState();
  return out;
}

void startEngine(AudioEngine& e, const Song& s) {
  e.setMode(PlayMode::Pattern);
  e.setPattern(s.patterns[0].id);
  e.play();
}

// A block stuck inside an effect while far more edits arrive than the retired list keeps.
void stalledBlock() {
  Document d(makeEmptySong());
  fillSong(d);
  const Song song = withStall(d);
  ProgramBuilder builder;
  builder.setSampleRate(44100);
  AudioEngine engine(44100);
  engine.setThreads(4);
  gStallEvery.store(0);
  gHolding.store(false);
  gHold.store(true);
  std::shared_ptr<const Program> first = builder.build(song);
  std::weak_ptr<const Program> watch = first;
  engine.setProgram(std::move(first));
  startEngine(engine, song);

  std::atomic<bool> stop{false};
  std::thread audio([&] {
    std::vector<float> L(256), R(256);
    while (!stop.load()) engine.process(L.data(), R.data(), 256);
  });
  for (int i = 0; i < 5000 && !gHolding.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  expect(gHolding.load(), "the block reached the stalling effect");
  for (int i = 0; i < 300; ++i) engine.setProgram(builder.build(song));
  expect(!watch.expired(), "the Program of a running block outlives 300 newer ones");
  gHold.store(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.store(true);
  audio.join();
  engine.setProgram(builder.build(song));  // the block has ended: this collects it
  expect(watch.expired(), "and is freed once that block has ended");
}

// A long drag on a send level (one publish per edit) with tracks added and removed, blocks that
// sometimes take long, and threaded mixing on.
void routeDrag() {
  Document d(makeEmptySong());
  fillSong(d);
  const Song song = withStall(d);
  ProgramBuilder builder;
  builder.setSampleRate(44100);
  AudioEngine engine(44100);
  engine.setThreads(4);
  gHold.store(false);
  gStallEvery.store(40);
  engine.setProgram(builder.build(song));
  startEngine(engine, song);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> blocks{0};
  std::thread audio([&] {
    std::vector<float> L(128), R(128);
    while (!stop.load()) {
      engine.process(L.data(), R.data(), 128);
      blocks.fetch_add(1);
    }
  });
  const ObjectId from = song.mixer[1].id, to = song.mixer[20].id;
  std::vector<ObjectId> added;
  for (int i = 0; i < 1500; ++i) {
    d.setSend(from, to, i % 200 == 100 ? 0.f : 0.05f + static_cast<float>(i % 100) * 0.01f);
    if (i % 50 == 25) added.push_back(d.addMixerTrack());
    if (i % 50 == 45 && !added.empty()) {
      d.removeMixerTrack(added.back());
      added.pop_back();
    }
    engine.setProgram(builder.build(withStall(d)));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true);
  audio.join();
  gStallEvery.store(0);
  expect(blocks.load() > 0, "blocks ran during 1500 route edits with threaded mixing");
}

}  // namespace

int main() {
  setTestEffectFactory(&factory);
  stalledBlock();
  routeDrag();
  return finish();
}

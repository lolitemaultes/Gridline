// Gridline — J-21 QA: cross-feature checks (features that were built and
// tested separately, exercised together): recording latency + count-in +
// time-selection loop, recording + plugin delay compensation, parallel mixer
// threads + audio editor slots, automation recording + MIDI learn.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/AudioRecord.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

struct Rig {
  double sr;
  Song song;
  ProgramBuilder builder;
  AudioEngine eng;
  AudioRecorder rec;
  std::shared_ptr<const Program> prog;
  int64_t frame = 0;
  std::function<float(int64_t, int)> input = [](int64_t, int) { return 0.f; };
  std::vector<float> outL, outR;

  explicit Rig(double rate = 48000) : sr(rate), eng(rate) {
    song = makeEmptySong();
    builder.setSampleRate(rate);
    eng.setInputChannels(2);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
  }
  void build() {
    prog = builder.build(song);
    eng.setProgram(prog);
  }
  void run(int64_t frames, int block = 256) {
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block)), in(static_cast<size_t>(block) * 2);
    outL.clear();
    outR.clear();
    for (int64_t done = 0; done < frames;) {
      int n = static_cast<int>(std::min<int64_t>(block, frames - done));
      for (int i = 0; i < n; ++i) {
        in[static_cast<size_t>(2 * i)] = input(frame + i, 0);
        in[static_cast<size_t>(2 * i + 1)] = input(frame + i, 1);
      }
      eng.pushInput(in.data(), n);
      eng.process(L.data(), R.data(), n);
      outL.insert(outL.end(), L.begin(), L.begin() + n);
      outR.insert(outR.end(), R.begin(), R.begin() + n);
      frame += n;
      done += n;
      rec.pump(eng, ringsOf(*prog));
    }
  }
  double spt() const { return sr * 60.0 / (song.tempo * song.ppq); }
  void finish() {
    eng.setRecording(false);
    eng.stop();
    run(eng.recordLatency() + 1024);
  }
};

int64_t impulseAt(const RecordedTake& t) {
  for (size_t i = 0; i < t.L.size(); ++i)
    if (t.L[i] > 0.9f) return static_cast<int64_t>(i);
  return -1;
}

// Count-in + time-selection loop + input latency, all at once: the count-in
// must not shift the takes, the latency must still move captured frames
// earlier, and every loop pass must line up with the loop start.
void countInLoopLatency() {
  Rig r(48000);
  r.song.tempo = 120;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  const Tick bar = r.song.barTicks();
  const double barFrames = static_cast<double>(bar) * r.spt();
  const int64_t countIn = static_cast<int64_t>(std::llround(static_cast<double>(bar) * r.spt()));
  const int lat = 500;
  const int64_t x1 = 1000, x2 = 2000;
  const int64_t f1 = countIn + lat + x1;
  const int64_t f2 = countIn + lat + static_cast<int64_t>(std::llround(barFrames)) + x2;
  r.eng.setLooping(true);
  r.eng.setLoopRange(0, bar);
  r.eng.setRecordLatency(lat);
  r.input = [&](int64_t f, int) { return (f == f1 || f == f2) ? 1.0f : 0.0f; };
  r.rec.begin();
  r.eng.setRecording(true);
  r.eng.playWithCountIn(static_cast<double>(bar));
  r.run(countIn + lat + static_cast<int64_t>(barFrames * 2.5), 256);
  r.finish();
  auto takes = r.rec.release();
  std::printf("# count-in loop latency: %zu takes", takes.size());
  for (const auto& t : takes) std::printf(" [start %.3f, %lld frames, impulse %lld]", t.startTick, static_cast<long long>(t.frames()),
                                          static_cast<long long>(impulseAt(t)));
  std::printf("\n");
  expect(takes.size() == 3, "count-in + loop + latency: one take per pass");
  if (takes.size() < 2) return;
  expect(std::fabs(takes[0].startTick) < 1e-6 && std::fabs(takes[1].startTick) < 1e-6, "takes start at the loop start");
  expect(std::llabs(impulseAt(takes[0]) - x1) <= 1, "pass 1: impulse lands where it was played (count-in + latency removed)");
  expect(std::llabs(impulseAt(takes[1]) - x2) <= 1, "pass 2: impulse lands where it was played");
}

// The engine's recording latency follows the delay compensation of the
// program (App adds outputLatency); a latency change between takes keeps
// placement exact.
void latencyChangeBetweenTakes() {
  Rig r(44100);
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  const int64_t F = 20000;
  for (int lat : {0, 300, 4096}) {
    r.eng.setPosition(0);
    r.eng.setRecordLatency(lat);
    const int64_t base = r.frame;
    r.input = [&, base](int64_t f, int) { return f == base + F ? 1.0f : 0.0f; };
    r.rec.begin();
    r.eng.setRecording(true);
    r.eng.play();
    r.run(44100);
    r.finish();
    auto takes = r.rec.release();
    bool ok = takes.size() == 1 && std::llabs(impulseAt(takes[0]) - (F - lat)) <= 1;
    std::printf("# latency %d: %zu takes", lat, takes.size());
    for (const auto& t : takes) std::printf(" [start %.3f, %lld frames, impulse %lld]", t.startTick, static_cast<long long>(t.frames()),
                                            static_cast<long long>(impulseAt(t)));
    std::printf("\n");
    expect(ok, ("recording latency " + std::to_string(lat) + ": impulse placed latency-compensated").c_str());
  }
}

// Offline render of a song whose inserts hold every effect type (incl. the
// audio editor slot): parallel mixer threads must stay bit-identical, and no
// effect may emit NaN / inf (an Edison slot in a parallel graph shares no state).
void parallelEveryEffect() {
  Song s = makeEmptySong();
  std::vector<std::string> fx;
  for (const auto& p : pluginCatalog())
    if (p.kind == PluginKind::Effect) fx.push_back(p.type);
  // A generator that makes sound on several inserts.
  Document d(s);
  size_t slot = 0;
  ObjectId pat = d.addPattern("p", 4);
  const int inserts = static_cast<int>((fx.size() + kEffectSlots - 1) / kEffectSlots);
  for (int k = 0; k < inserts; ++k) {
    ObjectId ch = d.addChannel("c" + std::to_string(k), "gl.synth");
    ObjectId t = d.addMixerTrack();
    d.routeChannel(ch, t);
    for (int st = 0; st < 64; st += 3) d.setStep(pat, ch, st, 100);
    for (int sl = 0; sl < kEffectSlots && slot < fx.size(); ++sl) d.setSlotPlugin(t, sl, fx[slot++]);
  }
  d.addClip(pat, 0, 0);
  std::printf("# %zu of %zu effect types placed on %d inserts\n", slot, fx.size(), inserts);
  Song song = d.song();
  RenderOptions a;
  a.sampleRate = 44100;
  a.threads = 1;
  AudioBuffer one = renderSong(song, a);
  RenderOptions b = a;
  b.threads = 6;
  AudioBuffer six = renderSong(song, b);
  bool finite = true;
  for (float v : one.data) finite = finite && std::isfinite(v);
  expect(one.frames > 0 && finite, "every effect type in one song: finite output");
  expect(one.data == six.data, "every effect type: 6 mixer threads bit-identical to 1");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("qacross");
  countInLoopLatency();
  latencyChangeBetweenTakes();
  parallelEveryEffect();
  return finish();
}

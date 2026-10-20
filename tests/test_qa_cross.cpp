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
#include "engine/AudioLogger.h"
#include "engine/Program.h"
#include "engine/Render.h"
#include "packs/synths/matrixfm.h"
#include "core/Demo.h"

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

// J-36 (J-30 gap): Matrix FM allocates its delay lines in prepareHost only
// when the section is on in the built state; automation that switches the
// section on while playing (no program build) left it silent. The builder now
// prepares automated parameters at their maximum.
void automatedSectionHasMemory() {
  namespace mfm = packs::synths::mfm;
  const PluginInfo* info = findPlugin("gl.matrixfm");
  if (!info) {
    expect(false, "matrix fm registered");
    return;
  }
  auto tailEnergy = [&](bool automated) {
    Document d(makeEmptySong());
    const ObjectId ch = d.addChannel("fm", "gl.matrixfm");
    auto setP = [&](int k, double frac) {
      const auto& pd = info->params[static_cast<size_t>(k)];
      d.setParam(channelPluginParam(ch, k), pd.min + frac * (pd.max - pd.min), false);
    };
    setP(mfm::dly(0, 0), 0.0);  // delay 1 off in the saved state
    d.setParam(channelPluginParam(ch, mfm::fm(0, mfm::kColFx)), 1.0, false);  // operator 1 feeds the effects bus
    if (automated) {
      const ParamId pid = channelPluginParam(ch, mfm::dly(0, 0));
      d.editSong("auto", [&](Song& x) {
        AutomationClip a;
        a.id = x.allocateId();
        a.owner = pid.owner, a.index = pid.index;
        a.points = {AutoPoint{0, 1.f, 0, AutoShape::Curve}, AutoPoint{x.ppq * 16, 1.f, 0, AutoShape::Curve}};
        x.automations.push_back(a);
      });
    }
    ProgramBuilder b;
    b.setSampleRate(44100);
    auto prog = b.build(d.song());
    const ProgChannel* pc = nullptr;
    for (const auto& c : prog->channels)
      if (c.id == ch) pc = &c;
    if (!pc || !pc->rt || !pc->rt->gen) return -1.0;
    PluginState st = pc->plugin;  // the value the automation plays: delay 1 on
    st.params[static_cast<size_t>(mfm::dly(0, 0))] = static_cast<float>(info->params[static_cast<size_t>(mfm::dly(0, 0))].max);
    ProcessContext ctx;
    ctx.sampleRate = 44100, ctx.tempo = 120;
    std::vector<float> L(256), R(256);
    double e = 0;
    pc->rt->gen->noteOn(1, 60, 1.f, 0);
    for (int blk = 0; blk < 400; ++blk) {  // ~2.3 s
      if (blk == 20) pc->rt->gen->noteOff(1, 0);
      std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
      pc->rt->gen->process(st, pc->aux, ctx, L.data(), R.data(), 256);
      if (blk > 120)
        for (float v : L) e += static_cast<double>(v) * v;
    }
    return e;
  };
  const double plain = tailEnergy(false), automated = tailEnergy(true);
  std::printf("# matrix fm delay tail: not automated %.4g, automated %.4g\n", plain, automated);
  expect(automated > 1e-3 && automated > 10 * plain, "matrix fm: a delay switched on by automation has its delay lines");
}

// J-36: audio logger (master and armed inputs) while recording through a
// latency-compensated insert: the logger sees every processed frame, the
// take still lands where it was played, nothing non-finite.
// The first built-in effect that reports latency at its defaults.
PluginState latencyEffect() {
  for (const auto& p : pluginCatalog()) {
    if (p.kind != PluginKind::Effect) continue;
    PluginState s = defaultPluginState(p.type);
    if (auto fx = createEffect(p.type); fx && fx->latencyFor(s, 48000) > 0) return s;
  }
  return {};
}

void loggerRecordingPdc() {
  for (auto src : {AudioLogger::Source::Master, AudioLogger::Source::ArmedInputs}) {
    Rig r(48000);
    r.song.tempo = 120;
    r.song.mixer[1].input = 0;
    r.song.mixer[1].armed = true;
    r.song.mixer[1].slots[0].plugin = latencyEffect();
    r.build();
    const int lat = r.prog->outputLatency;
    r.eng.setRecordLatency(lat);
    auto lg = std::make_shared<AudioLogger>(r.sr, 2.0, src);
    r.eng.setLogger(lg);
    const int64_t x = 7000;
    r.input = [&](int64_t f, int) { return f == x + lat ? 1.0f : 0.0f; };
    r.rec.begin();
    r.eng.setRecording(true);
    r.eng.play();
    r.run(48000, 300);
    const int64_t ran = r.frame;
    r.finish();
    auto takes = r.rec.release();
    bool finite = true;
    for (float v : r.outL) finite = finite && std::isfinite(v);
    std::vector<float> L, R;
    lg->copyLast(lg->available(), L, R);
    for (float v : L) finite = finite && std::isfinite(v);
    std::printf("# logger %d + recording + pdc %d: %zu takes, logged %lld\n", static_cast<int>(src), lat, takes.size(),
                static_cast<long long>(lg->written()));
    expect(lat > 0, "pdc rig: the insert reports latency");
    expect(lg->written() >= ran && finite, "audio logger hears every frame while recording with PDC, finite");
    expect(takes.size() == 1 && std::llabs(impulseAt(takes[0]) - x) <= 1, "recording with logger + PDC: take placement exact");
  }
}

// J-36: quantized song-position jumps (J-31) with a loop and with recording:
// the jump lands on the next grid line, replaces a loop wrap that comes
// first, and recording across it stays finite and in order.
void quantizedJumps() {
  Rig r(48000);
  r.song.tempo = 120;
  r.build();
  const Tick bar = r.song.barTicks();
  r.eng.play();
  r.run(static_cast<int64_t>(static_cast<double>(bar) * 0.25 * r.spt()));
  r.eng.jumpTo(bar * 5, bar);  // quarter way into bar 1: lands at bar 5 when bar 2 starts
  double before = r.eng.position();
  r.run(static_cast<int64_t>(static_cast<double>(bar) * 0.5 * r.spt()));
  expect(r.eng.position() > before && r.eng.position() < static_cast<double>(bar), "jump waits for the grid line");
  r.run(static_cast<int64_t>(static_cast<double>(bar) * 0.5 * r.spt()));
  const double p = r.eng.position();
  expect(p >= static_cast<double>(bar * 5) && p < static_cast<double>(bar * 5) + static_cast<double>(bar) * 0.3, "jump lands on bar 5");
  // Loop shorter than the grid: the wrap comes first and goes to the jump target (inside the loop) instead of the loop start.
  r.eng.stop();
  r.run(256);
  r.eng.setLooping(true);
  r.eng.setLoopRange(0, bar);
  r.eng.play();
  r.run(static_cast<int64_t>(static_cast<double>(bar) * 0.1 * r.spt()));
  r.eng.jumpTo(bar / 2, bar * 4);
  r.run(static_cast<int64_t>(static_cast<double>(bar) * 1.0 * r.spt()));
  const double q = r.eng.position();
  expect(q >= static_cast<double>(bar) * 0.55 && q < static_cast<double>(bar) * 0.7,
         "a loop wrap before the grid line takes the pending jump");
  // Recording across a quantized jump.
  Rig w(48000);
  w.song.tempo = 120;
  w.song.mixer[1].input = 0;
  w.song.mixer[1].armed = true;
  w.build();
  w.input = [](int64_t f, int) { return f % 9000 == 0 ? 0.8f : 0.0f; };
  w.rec.begin();
  w.eng.setRecording(true);
  w.eng.play();
  w.run(10000);
  w.eng.jumpTo(bar * 8, w.song.beatTicks());
  w.run(30000);
  w.finish();
  auto takes = w.rec.release();
  bool ok = !takes.empty();
  for (const auto& t : takes) {
    ok = ok && std::isfinite(t.startTick) && t.frames() > 0;
    for (float v : t.L) ok = ok && std::isfinite(v);
  }
  std::printf("# recording across a quantized jump: %zu takes\n", takes.size());
  expect(ok, "recording across a quantized jump: finite takes");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("qacross");
  countInLoopLatency();
  latencyChangeBetweenTakes();
  parallelEveryEffect();
  automatedSectionHasMemory();
  loggerRecordingPdc();
  quantizedJumps();
  return finish();
}

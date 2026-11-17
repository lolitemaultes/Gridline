// Gridline - K-106 cp18 (K-316 plan 7.3, 8.2): the Tracker Sampler's play tap and the preview of a
// part of a sample.
//   * a played note shows in the tap: its slot, a position that moves on at the note's rate, its note;
//   * nothing is allocated while the generator renders and publishes (operator new counted);
//   * a released note leaves the tap empty once its sound has ended; a generator that goes leaves none;
//   * another instrument's tap is not read as this one's;
//   * the editor's preview of frames from .. to starts at `from` and ends by `to`.
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/TrackerData.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/tracker/tracker.h"
#include "../Check.h"

namespace {
std::atomic<long long> gNews{0};
}
void* operator new(std::size_t n) {
  gNews.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  gNews.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
namespace trk = gl::tracker;
namespace tp = gl::packs::trackerpack;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

struct Rig {
  PluginState st;
  ChannelAux aux;
  std::unique_ptr<Generator> g;
  ProcessContext ctx;
  std::vector<float> L, R;
  const tp::InstrumentAux* inst = nullptr;
  explicit Rig(int sound) {
    st = defaultPluginState(trk::kInstrumentType);
    st.params[trk::PSound] = static_cast<float>(sound);
    st.params[trk::PRamp] = 0;
    g = packs::createGenerator(st.type);
    packs::prepareAux(st, aux);
    inst = dynamic_cast<const tp::InstrumentAux*>(aux.custom.get());
    ctx.sampleRate = 44100;
    ctx.tempo = 125;
    ctx.ppq = 96;
    L.assign(4096, 0.0f);
    R.assign(4096, 0.0f);
  }
  void run(int n) {
    for (int done = 0; done < n;) {
      const int m = std::min(256, n - done);
      g->process(st, aux, ctx, L.data(), R.data(), m);
      done += m;
    }
  }
};

void testTap() {
  Rig r(1);
  expect(r.inst != nullptr, "rig: the built-in sound's instrument");
  tp::TapVoice v[tp::kTapVoices];
  r.run(512);
  expect(tp::readPlayTap(r.inst, v, tp::kTapVoices) == 0, "idle: no voice in the tap");
  const long long pub0 = tp::playTapPublished();
  r.g->noteOn(1, 60, 100.0f / 127.0f, 0);
  r.run(256);
  int n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(n == 1, "a note: one voice in the tap");
  expect(tp::playTapPublished() > pub0, "the generator published");
  const int32_t p1 = n == 1 ? v[0].pos : -1;
  expect(n == 1 && v[0].slot == 0, "the voice's slot");
  expect(n == 1 && v[0].note == 60 - trk::kPitchOffset, "the voice's note (C-4 as the tracker numbers it)");
  // C-4 of a built-in sound plays middle C; its position moves on with the frames rendered.
  const long long newsBefore = gNews.load();
  r.run(2048);
  const long long newsAfter = gNews.load();
  expect(newsAfter == newsBefore, "rendering and publishing allocate nothing (" + std::to_string(newsAfter - newsBefore) + " news)");
  n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(n == 1 && v[0].pos > p1, "the position moves on");
  // cp20: where the voice is on its envelopes (the Instrument editor's play lines): a tick on the volume
  // envelope's axis while it is on, -1 for an envelope that is off.
  {
    const tp::TapVoice before = v[0];
    r.run(4096);
    n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
    expect(n == 1 && before.volTick >= 0, "envelope tap: the volume envelope's tick (" + std::to_string(before.volTick) + ")");
    expect(n == 1 && v[0].volTick >= before.volTick, "envelope tap: it moves on or holds at the sustain point (" + std::to_string(v[0].volTick) + ")");
    expect(n == 1 && v[0].panTick >= -1, "envelope tap: the panning envelope's tick or -1");
  }
  // Another instrument is never read as this one.
  Rig other(2);
  other.g->noteOn(1, 64, 0.8f, 0);
  other.run(256);
  tp::TapVoice w[tp::kTapVoices];
  const int no = tp::readPlayTap(other.inst, w, tp::kTapVoices);
  n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(no == 1 && n == 1 && w[0].note == 64 - trk::kPitchOffset, "two instruments: each reads its own voice");
  // Released: the voice fades and the tap empties.
  r.g->noteOff(1, 0);
  r.run(44100 * 3);
  expect(tp::readPlayTap(r.inst, v, tp::kTapVoices) == 0, "released and faded: the tap is empty");
  // The generator goes: its tap with it.
  other.g.reset();
  expect(tp::readPlayTap(other.inst, w, tp::kTapVoices) == 0, "a generator gone leaves no voices");
}

void testPartPreview() {
  Rig r(3);  // (the struck tine: 1.6 s, no loop)
  const int pitch = 60;
  const int32_t len = r.inst->text.def.samples[0].length;
  expect(len > 4000, "the built-in sound has frames to play a part of");
  const int32_t from = 1000, to = 1600;
  const uint32_t id = tp::requestAudition(r.inst, 0, pitch, from, to);
  r.g->noteOn(7, pitch, 100.0f / 127.0f, 0);
  r.run(64);
  tp::TapVoice v[tp::kTapVoices];
  int n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(tp::auditionPlaying(id), "the part's preview took the note");
  expect(n == 1 && v[0].pos >= from && v[0].pos < from + 200, "the preview starts at `from` (" + std::to_string(n ? v[0].pos : -1) + ")");
  r.run(4096);
  n = tp::readPlayTap(r.inst, v, tp::kTapVoices);
  expect(n == 0, "the preview stops by `to`");
  expect(!tp::auditionSounding(id), "the preview's audition ended");
  r.g->noteOff(7, 0);
}

}  // namespace

int main() {
  testTap();
  testPartPreview();
  return finish();
}

// Gridline: J-170 Rubber Band realtime start-up (engine/TimeStretch,
// the sampler's Rubber Band voices, AudioEngine's note lookahead, clip
// voices). Rubber Band works through its whole start delay (half its longest
// window) before a stream's first frame, so a note-on cost about nine
// blocks' worth of engine work at once (seventeen at 96 kHz, twice that for
// a clip with its pre-roll). Checked here, without wall-clock timing (CI
// runners are slow): the work is counted in frames given to the engine.
//   voice     startLater() + prime() in pieces gives the same output as
//             start(), bit for bit, at 44.1 / 96 kHz, any speed, pitch,
//             pre-roll and budget; the primed stream's first block does one
//             engine step instead of all of them;
//   alloc     no heap allocation in start / prime / render across every
//             MIDI note at four sample-rate ratios, pitch bends and speed
//             changes (a few pitches allocated once before J-170);
//   sampler   notes announced ahead (Generator::noteAhead) play the same
//             output as notes that are not, chords included, also when a
//             parameter changes between the announcement and the note; the
//             note-on block does a fraction of the work;
//   engine    pattern playback announces sequenced chords (also across the
//             loop wrap): no block carries a chord's start-up work.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "../../src/core/AudioBuffer.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Song.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Sample.h"
#include "../../src/engine/TimeStretch.h"
#include "../Check.h"

// ---- allocation counter: operator new everywhere; on macOS also every malloc
// (Rubber Band allocates some buffers with malloc), through libmalloc's logger hook.
namespace {
std::atomic<bool> gCount{false};
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
  if (gCount.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCount.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
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

#if defined(__APPLE__) && !defined(__SANITIZE_ADDRESS__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GL_NO_MALLOC_HOOK 1
#endif
#endif
#ifndef GL_NO_MALLOC_HOOK
#include <pthread.h>
extern "C" {
typedef void(gl_malloc_logger_t)(uint32_t type, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t result, uint32_t skip);
extern gl_malloc_logger_t* malloc_logger;
}
namespace {
pthread_t gThread;
std::atomic<long> gMallocs{0};
// Counts allocations of the counting thread (type bit 2: an allocation). No
// allocation, no thread-local access in here (it runs inside malloc).
void countMalloc(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t) {
  if ((type & 2) && gCount.load(std::memory_order_relaxed) && pthread_equal(pthread_self(), gThread)) gMallocs.fetch_add(1, std::memory_order_relaxed);
}
void hookMalloc() {
  gThread = pthread_self();
  malloc_logger = &countMalloc;
}
long mallocs() { return gMallocs.load(); }
bool mallocHooked() { return true; }
}  // namespace
#define GL_MALLOC_HOOK 1
#endif
#endif
#ifndef GL_MALLOC_HOOK
namespace {
void hookMalloc() {}
long mallocs() { return 0; }
bool mallocHooked() { return false; }
}  // namespace
#endif

using namespace gl;
namespace fs = std::filesystem;

namespace {

using Sig = std::vector<float>;

struct CountScope {
  long a0, m0;
  CountScope() : a0(gAllocs.load()), m0(mallocs()) { gCount = true; }
  ~CountScope() { gCount = false; }
  long n() const { return gAllocs.load() - a0 + mallocs() - m0; }
};

// A bass with hits (a sawtooth at 55 Hz and noise bursts every 0.25 s).
Sig material(double sr, double sec) {
  Sig x(static_cast<size_t>(sec * sr));
  uint32_t r = 7;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    double v = 0;
    for (int k = 1; k <= 12; ++k) v += std::sin(2 * 3.14159265358979323846 * k * 55 * t) / k;
    r = r * 1664525u + 1013904223u;
    const double ph = std::fmod(t, 0.25);
    v = 0.25 * v + (ph < 0.01 ? 0.4 * ((r >> 8) / 16777216.0 - 0.5) * (1 - ph / 0.01) : 0.0);
    x[i] = static_cast<float>(v);
  }
  return x;
}

struct Src {
  const Sig* x;
  size_t pos = 0;
  static void read(void* c, float* const* d, int n) {
    auto* s = static_cast<Src*>(c);
    for (int i = 0; i < n; ++i, ++s->pos) {
      const float v = s->pos < s->x->size() ? (*s->x)[s->pos] : 0.f;
      d[0][i] = v, d[1][i] = 0.7f * v;
    }
  }
};

std::string writeTmp(const std::string& name, const Sig& x, double sr) {
  AudioBuffer b(2, x.size());
  for (size_t i = 0; i < x.size(); ++i) b.at(0, i) = x[i], b.at(1, i) = 0.8f * x[i];
  const std::string path = (fs::temp_directory_path() / name).string();
  writeWav(path, b, static_cast<int>(sr), 24);
  sampleCache().clear();
  return path;
}

// ------------------------------------------------------------ sampler runs

struct NoteAt {
  int64_t at;  // frame
  double pitch;
  int64_t len;
};
struct Run {
  Sig L, R;
  int64_t fedAtNotes = 0;  // Rubber Band frames fed in the blocks where notes start
  long allocs = 0;         // heap allocations in process() after the first block
};
// One gl.sampler (Rubber Band) playing `notes` over `frames` in blocks of
// `block`; `announce`: each note announced while it is up to 0.15 s ahead,
// as the engine does. `change`: at that frame the pitch shift moves to 30 cents.
Run samplerRun(const std::string& path, double sr, int block, const std::vector<NoteAt>& notes, int64_t frames, bool announce,
               int64_t change = -1) {
  PluginState st = defaultPluginState("gl.sampler");
  st.data = path;
  st.params[pp::SmpStretchMode] = static_cast<float>(stretch::Mode::RubberBand);
  st.params[pp::SmpOneShot] = 0.f;
  ChannelAux aux;
  prepareChannelAux(st, aux);
  auto gen = createGenerator("gl.sampler");
  gen->prepareHost(st, sr);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  Run r;
  r.L.assign(static_cast<size_t>(frames), 0.f), r.R.assign(static_cast<size_t>(frames), 0.f);
  const auto ahead = static_cast<int64_t>(std::llround(Generator::kNoteLookaheadSec * sr));
  uint32_t id = 1;
  std::vector<uint32_t> ids(notes.size(), 0);
  for (int64_t b0 = 0; b0 < frames; b0 += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, frames - b0));
    if (change >= 0 && b0 >= change) st.params[pp::SmpPitchShift] = 30.f;
    bool noteHere = false;
    for (size_t k = 0; k < notes.size(); ++k) {
      const NoteAt& nt = notes[k];
      if (nt.at >= b0 && nt.at < b0 + n) gen->noteOn(ids[k] = id++, nt.pitch, 0.9f, static_cast<int>(nt.at - b0)), noteHere = true;
      if (ids[k] && nt.at + nt.len >= b0 && nt.at + nt.len < b0 + n) gen->noteOff(ids[k], static_cast<int>(nt.at + nt.len - b0));
      if (announce && gen->wantsNoteLookahead() && nt.at >= b0 + n && nt.at < b0 + n + ahead) gen->noteAhead(nt.pitch, nt.at - b0);
    }
    const int64_t f0 = stretch::rubberBandFramesFed();
    if (b0 > 0) {
      CountScope cs;
      gen->process(st, aux, ctx, r.L.data() + b0, r.R.data() + b0, n);
      r.allocs += cs.n();
    } else {
      gen->process(st, aux, ctx, r.L.data() + b0, r.R.data() + b0, n);
    }
    if (noteHere) r.fedAtNotes += stretch::rubberBandFramesFed() - f0;
  }
  return r;
}

float maxDiff(const Sig& a, const Sig& b) {
  float d = 0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
  return d;
}
double rms(const Sig& x) {
  double e = 0;
  for (float v : x) e += static_cast<double>(v) * v;
  return x.empty() ? 0.0 : std::sqrt(e / static_cast<double>(x.size()));
}

}  // namespace

int main() {
  hookMalloc();
  std::printf("# allocation counter: operator new%s\n", mallocHooked() ? " and malloc" : "");

  // ============================================================ voice: primed == started, work moved
  int64_t coldFirst44 = 0;  // engine frames a cold note's first block needs at 44.1 kHz
  {
    int cases = 0, same = 0;
    bool movedAll = true;
    for (double sr : {44100.0, 96000.0}) {
      const Sig x = material(sr, 3.0);
      stretch::Voice a, b;
      a.prepare(2, sr, 8.0, 8.0, false, stretch::Engine::RubberBand);
      b.prepare(2, sr, 8.0, 8.0, false, stretch::Engine::RubberBand);
      for (double speed : {1.0, 0.6, 1.8}) {
        for (double pitch : {1.0, 0.75, std::pow(2.0, 7 / 12.0)}) {
          for (int pre : {0, a.prerollFor(speed)}) {
            for (int budget : {1, 300, 4096}) {
              const auto at = static_cast<size_t>(sr) - static_cast<size_t>(pre);
              Src sa{&x, at}, sb{&x, at};
              a.start(&Src::read, &sa, speed, pitch, pre);
              b.startLater(&Src::read, &sb, speed, pitch, pre);
              for (int k = 0; k < 1000 && !b.prime(&Src::read, &sb, speed, pitch, budget); ++k) {
              }
              Sig oa(256), oa2(256), ob(256), ob2(256);
              float* pa[2] = {oa.data(), oa2.data()};
              float* pb[2] = {ob.data(), ob2.data()};
              float d = 0;
              for (int blk = 0; blk < 120; ++blk) {
                const int64_t fa = a.fedFrames(), fb = b.fedFrames();
                const double p2 = pitch * (1.0 + 0.05 * std::sin(blk * 0.2));  // a bend after the start
                a.render(pa, 256, speed, p2, &Src::read, &sa);
                b.render(pb, 256, speed, p2, &Src::read, &sb);
                if (blk == 0) {
                  const int64_t cold = a.fedFrames() - fa, warm = b.fedFrames() - fb;
                  movedAll = movedAll && warm <= static_cast<int64_t>(std::ceil(2 * 256 * speed)) + 2 && cold > 4 * warm + 1000;
                  if (sr == 44100.0 && speed == 1.0 && pitch == 1.0 && pre == 0) coldFirst44 = cold;
                }
                for (int i = 0; i < 256; ++i) d = std::max({d, std::fabs(oa[static_cast<size_t>(i)] - ob[static_cast<size_t>(i)]),
                                                            std::fabs(oa2[static_cast<size_t>(i)] - ob2[static_cast<size_t>(i)])});
              }
              ++cases;
              same += d == 0.f;
            }
          }
        }
      }
    }
    std::printf("# primed against started streams: %d of %d identical; a cold first block of 256 feeds the engine %lld frames at 44.1 kHz\n", same,
                cases, static_cast<long long>(coldFirst44));
    expect(same == cases, "rubber band: a stream primed in pieces (any budget) plays bit for bit what start() plays (44.1 / 96 kHz, speeds, pitches, pre-roll)");
    expect(movedAll && coldFirst44 > 3000,
           "rubber band: the start-up work moves into prime(): a primed stream's first block feeds the engine one or two steps, a cold one all of them");
  }

  // ============================================================ no allocation on the audio path
  {
    long allocs = 0;
    int notes = 0;
    for (double sr : {44100.0, 48000.0}) {
      const Sig x = material(sr, 2.0);
      stretch::Voice v;
      v.prepare(2, sr, 8.0, 8.0, false, stretch::Engine::RubberBand);
      Sig o0(256), o1(256);
      float* o[2] = {o0.data(), o1.data()};
      uint32_t rng = 99;
      for (double base : {1.0, 44100.0 / 48000.0, 48000.0 / 44100.0, 0.5}) {
        for (int st = -24; st <= 24; ++st) {
          const double p = base * std::pow(2.0, st / 12.0);
          Src s{&x, 0};
          CountScope cs;
          if (st % 2) {
            v.start(&Src::read, &s, 1.0, p, 0);
          } else {
            v.startLater(&Src::read, &s, 1.0, p, v.prerollFor(1.0));
            while (!v.prime(&Src::read, &s, 1.0, p, 512)) {
            }
          }
          for (int k = 0; k < 6; ++k) v.render(o, 256, 1.0, p, &Src::read, &s);
          for (int k = 0; k < 6; ++k) {  // a pitch bend, +-2 semitones
            rng = rng * 1664525u + 1013904223u;
            v.render(o, 256, 1.0, p * std::pow(2.0, ((rng >> 8) / 16777216.0 * 4 - 2) / 12.0), &Src::read, &s);
          }
          for (int k = 0; k < 6; ++k) v.render(o, 256, 0.4 + 0.3 * k, p, &Src::read, &s);
          allocs += cs.n();
          ++notes;
        }
      }
    }
    std::printf("# %d notes (every semitone of 4 octaves, 4 sample-rate ratios, 44.1 / 48 kHz) with bends and speed changes: %ld allocations\n", notes,
                allocs);
    expect(allocs == 0, "rubber band realtime: start, startLater, prime and render allocate nothing at any pitch, bend or speed");
  }

  // ============================================================ sampler: announced notes
  {
    const double sr = 44100;
    const std::string path = writeTmp("gl_j170_bass.wav", material(sr, 3.0), sr);
    const auto F = static_cast<int64_t>(sr);
    // Two chords of four, then single notes.
    std::vector<NoteAt> notes;
    for (double p : {48.0, 52.0, 55.0, 59.0}) notes.push_back({F / 2 + 37, p, F / 2});
    for (double p : {50.0, 53.0, 57.0, 60.0}) notes.push_back({F + F / 4 + 11, p, F / 2});
    notes.push_back({2 * F + 5, 62.0, F / 4});
    notes.push_back({2 * F + F / 3, 43.0, F / 4});
    const int64_t frames = 3 * F;
    const Run cold = samplerRun(path, sr, 256, notes, frames, false);
    const Run warm = samplerRun(path, sr, 256, notes, frames, true);
    const float d = std::max(maxDiff(cold.L, warm.L), maxDiff(cold.R, warm.R));
    std::printf("# sampler, 2 chords of 4 and 2 notes at 44.1 kHz, block 256: difference %g, level %.3f; engine frames fed in the note-on blocks: "
                "%lld cold, %lld announced; allocations %ld / %ld\n",
                d, rms(cold.L), static_cast<long long>(cold.fedAtNotes), static_cast<long long>(warm.fedAtNotes), cold.allocs, warm.allocs);
    expect(d == 0.f && rms(cold.L) > 0.02, "sampler: notes announced ahead play bit for bit what unannounced notes play (chords included)");
    expect(warm.fedAtNotes * 4 < cold.fedAtNotes && cold.fedAtNotes > 10 * coldFirst44 / 2,
           "sampler: announced notes do their start-up work before their block (a quarter of the work, or less, left in the note-on blocks)");
    expect(cold.allocs == 0 && warm.allocs == 0, "sampler: Rubber Band notes, announced or not, allocate nothing in process()");

    // A parameter moves between the announcement and the note: the note starts afresh (the same as unannounced).
    std::vector<NoteAt> one = {{F / 2 + 100, 60.0, F / 4}};
    const Run c2 = samplerRun(path, sr, 256, one, F, false, F / 2 - 1024);
    const Run w2 = samplerRun(path, sr, 256, one, F, true, F / 2 - 1024);
    const float d2 = std::max(maxDiff(c2.L, w2.L), maxDiff(c2.R, w2.R));
    expect(d2 == 0.f && rms(c2.L) > 0.01, "sampler: a note whose sound changed after its announcement plays as an unannounced note");

    // 96 kHz, block 64: a note announced only a block ahead (it primes at once) and one announced in full time.
    const double sr2 = 96000;
    const std::string path2 = writeTmp("gl_j170_bass96.wav", material(sr2, 2.0), sr2);
    std::vector<NoteAt> two = {{100, 57.0, 20000}, {50000 + 3, 64.0, 20000}};
    const Run c3 = samplerRun(path2, sr2, 64, two, 110000, false);
    const Run w3 = samplerRun(path2, sr2, 64, two, 110000, true);
    const float d3 = std::max(maxDiff(c3.L, w3.L), maxDiff(c3.R, w3.R));
    std::printf("# sampler at 96 kHz, block 64: difference %g; note-on blocks fed %lld cold, %lld announced\n", d3,
                static_cast<long long>(c3.fedAtNotes), static_cast<long long>(w3.fedAtNotes));
    expect(d3 == 0.f && w3.fedAtNotes < c3.fedAtNotes, "sampler at 96 kHz, block 64: announced notes identical, less work at the note");
  }

  // ============================================================ engine: pattern playback announces chords
  {
    const int sr = 44100;
    const std::string path = writeTmp("gl_j170_engine.wav", material(sr, 3.0), sr);
    Song s = makeEmptySong();
    s.tempo = 120;
    s.patterns[0].bars = 1;  // 2 s at 120 BPM
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Sampler";
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.data = path;
    ch.plugin.params[pp::SmpStretchMode] = static_cast<float>(stretch::Mode::RubberBand);
    ch.plugin.params[pp::SmpOneShot] = 0.f;
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
    auto mk = [](Tick start, Tick len, int pitch) {
      gl::Note n;
      n.start = start, n.length = len, n.pitch = pitch, n.velocity = 100;
      return n;
    };
    // Chords of four on beats 1 and 3 (half a beat long): beat 1 also right after the loop wraps.
    s.patterns[0].notes[ch.id] = {mk(0, 48, 48), mk(0, 48, 52), mk(0, 48, 55), mk(0, 48, 59),
                                  mk(192, 48, 50), mk(192, 48, 53), mk(192, 48, 57), mk(192, 48, 60)};
    ProgramBuilder builder;
    builder.setSampleRate(sr);
    auto prog = builder.build(s);
    AudioEngine eng(sr);
    eng.setProgram(prog);
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(s.patterns[0].id);
    eng.setLooping(true);
    eng.play();
    const int block = 256;
    std::vector<float> L(block), R(block);
    int64_t worst = 0, worstFirst = 0;
    double level = 0;
    for (int b = 0; b < static_cast<int>(4.5 * sr / block); ++b) {
      const int64_t f0 = stretch::rubberBandFramesFed();
      eng.process(L.data(), R.data(), block);
      const int64_t fed = stretch::rubberBandFramesFed() - f0;
      if (b < 4)
        worstFirst = std::max(worstFirst, fed);  // the first chord: played from the start, nothing could be announced
      else
        worst = std::max(worst, fed);
      for (float v : L) level += static_cast<double>(v) * v;
    }
    std::printf("# pattern playback, chords of 4 (also over the loop wrap), block 256: most engine frames fed in one block %lld (the first chord, "
                "unannounced: %lld)\n",
                static_cast<long long>(worst), static_cast<long long>(worstFirst));
    expect(level > 1.0 && worstFirst > 4 * 3000 && worst * 3 < worstFirst,
           "engine: sequenced Rubber Band chords are announced and primed ahead, across the pattern's loop wrap (no block carries a chord's start-up)");
  }

  // ============================================================ clips starting together
  {
    const double sr = 44100;
    const std::string path = writeTmp("gl_j170_clip.wav", material(sr, 4.0), sr);
    SamplePtr smp = sampleCache().get(path);
    TempoMap map;
    map.setConstant(120, 96);
    // Four clips on beat 3 (1.0 s), each pitched (their realtime Rubber Band voices play: no render).
    std::vector<ProgAudioClip> clips;
    for (int k = 0; k < 4; ++k) {
      ProgAudioClip c;
      c.start = 192, c.end = 192 + 384;
      c.startSec = map.secondsAt(192.0), c.durSec = map.secondsAt(576.0) - c.startSec;
      c.sample = smp, c.srcStart = 0.1 * k, c.srcEnd = smp->seconds(), c.speed = 0.9, c.granular = true;
      c.pitch = std::pow(2.0, (k - 1) / 12.0);
      c.engine = stretch::Engine::RubberBand;
      c.rt = makeClipVoice(sr, stretch::Engine::RubberBand);
      clips.push_back(c);
    }
    const auto startS = static_cast<int64_t>(sr);  // the clips' first frame
    const auto total = static_cast<int64_t>(2.2 * sr);
    const int block = 256;
    // Played from 0.5 s (primed ahead) and from the clips' first frame (nothing to prime ahead).
    auto play = [&](int64_t from, int64_t& fedAtStart, long& allocs) {
      for (auto& c : clips) c.rt = makeClipVoice(sr, stretch::Engine::RubberBand), c.rtMix = 0, c.rtMixAt = -(int64_t{1} << 62);
      Sig L(static_cast<size_t>(total), 0.f), R(L.size(), 0.f);
      fedAtStart = 0, allocs = 0;
      for (int64_t at = from; at < total; at += block) {
        const int len = static_cast<int>(std::min<int64_t>(block, total - at));
        const int64_t f0 = stretch::rubberBandFramesFed();
        CountScope cs;
        renderAudioClips(clips, map, sr, 0, at, 0, len, L.data() + at, R.data() + at, 0.0);
        allocs += cs.n();
        if (startS >= at && startS < at + len) fedAtStart = stretch::rubberBandFramesFed() - f0;
      }
      return L;
    };
    int64_t fedWarm = 0, fedCold = 0;
    long alWarm = 0, alCold = 0;
    const Sig warm = play(static_cast<int64_t>(0.5 * sr), fedWarm, alWarm);
    const Sig cold = play(startS, fedCold, alCold);
    float d = 0;
    for (auto i = static_cast<size_t>(startS); i < warm.size(); ++i) d = std::max(d, std::fabs(warm[i] - cold[i]));
    const Sig part(warm.begin() + startS, warm.end());
    std::printf("# four Rubber Band clips starting together, block 256: engine frames fed in their first block %lld primed, %lld cold; largest "
                "difference %g (level %.3f); allocations %ld / %ld\n",
                static_cast<long long>(fedWarm), static_cast<long long>(fedCold), d, rms(part), alWarm, alCold);
    expect(d < 1e-5f && rms(part) > 0.05, "clips: a Rubber Band clip voice primed ahead plays what one started at the clip's first frame plays");
    expect(fedCold > 4 * 6000 && fedWarm * 4 < fedCold && alWarm == 0 && alCold == 0,
           "clips: clips starting together did their start-up before their first block (no allocation either way)");
  }

  return finish();
}

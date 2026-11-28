// Gridline — K-431: Audio Stretch Crash Hardening + FL-Style Stretch & BPM Grid-Lock.
//
// Part A (Crash, Fuzz, Race & Realtime Safety):
//   * stretch::process, processMapped, MappedStream, ed::stretchAudio on empty/tiny/mono/stereo
//     inputs, extreme ratios (0.05x..20x), extreme pitches, NaN/Inf/0/negative inputs, null ptrs;
//   * stretch::Voice unprepared guards, extreme/NaN prepare/startLater/prime/render, and ZERO
//     audio-thread heap allocations (Release build) across Stretch and Rubber Band;
//   * renderClip OOM/huge-frame rejection, NaN guards, mono async render concurrency (no data race);
//   * SamplerGen & SlicerGen concurrent prepareHost <-> configure/process pool-swap stress (no UAF)
//     and mid-playback sample replacement with empty/1-frame samples;
//   * AudioEngine stress: rapid BPM scrubbing (40 -> 300 -> 60), mode toggling, clip delete/undo;
//   * ClipRate & ClipOps NaN/Inf/extreme bounds guards.
//
// Part B (FL-Style Stretch & BPM Grid-Lock Across Tempo Changes):
//   * Switching a clip/channel to Stretch or Rubber Band (setClipStretchMode, Document::setParam,
//     ops::setAudioClipTimeMode) locks unlocked clips to the tempo grid so BPM changes 120->145->90
//     keep exact bar/beat ticks, waveform span, beat onsets, and pitch;
//   * Dialing sample BPM (SmpFitTempo, e.g. 128 BPM on a 1.875 s loop at 120 BPM) switches Resample
//     to Stretch, snaps to exact 1 bar (384 ticks), locks to tempo across 120->150->100 BPM with
//     pitch preserved, zero drag drift (including trimmed clips), single undo step, and clean unlock
//     back to fixed tempo when dialed to 0;
//   * fitClipToTempo switches Resample -> Stretch, syncs SmpFitTempo, locks bars & pitch;
//   * Playlist Stretch-drag to 2 bars stays exactly 2 bars across 120->155->85 BPM;
//   * Sampler TIME knob (SmpTimeStretch) stretches & locks playlist clips across BPM changes;
//   * Tempo automation ramp across a locked stretched clip tracks the TempoMap;
//   * Undo/Redo, JSON save/load + Collab clip/song serialization, and FLP export/import round-trip.

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "Check.h"
#include "Hermetic.h"
#include "core/ClipRate.h"
#include "core/Document.h"
#include "core/MenuParity.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/AudioEngine.h"
#include "engine/ClipFields.h"
#include "engine/ClipOps.h"
#include "engine/Generators.h"
#include "engine/PlaylistTools.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "engine/TimeStretch.h"
#include "packs/edison/edison.h"

// Allocation counter for audio-thread realtime-safety checks (disabled under ASan so ASan's
// own allocator & UAF/OOB detection run unimpeded).
#if !defined(__SANITIZE_ADDRESS__)
#if defined(__has_feature)
#if !__has_feature(address_sanitizer)
#define GL_K431_COUNT_ALLOCS 1
#endif
#else
#define GL_K431_COUNT_ALLOCS 1
#endif
#endif

#if defined(GL_K431_COUNT_ALLOCS)
namespace {
thread_local bool gCountAllocs = false;
std::atomic<long> gAllocCount{0};
}  // namespace
void* operator new(std::size_t n) {
  if (gCountAllocs) gAllocCount.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCountAllocs) gAllocCount.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace gl;
namespace fs = std::filesystem;
namespace ed = gl::packs::edison;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;

bool allFinite(const float* p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (!std::isfinite(p[i])) return false;
  return true;
}

std::string writeWavFile(const std::string& dir, const std::string& name, const std::vector<float>& x, int ch = 2) {
  AudioBuffer b(ch, x.size());
  for (size_t i = 0; i < x.size(); ++i)
    for (int c = 0; c < ch; ++c) b.at(c, i) = x[i];
  const std::string path = (fs::path(dir) / name).string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

std::vector<float> makeClickLoop(double bpm, int beats) {
  const double total = beats * 60.0 / bpm;
  std::vector<float> x(static_cast<size_t>(std::llround(total * kSr)), 0.0f);
  for (int k = 0; k < beats; ++k) {
    const size_t a = static_cast<size_t>(std::llround(k * 60.0 / bpm * kSr));
    for (size_t i = 0; i < static_cast<size_t>(0.004 * kSr) && a + i < x.size(); ++i)
      x[a + i] = static_cast<float>(0.9 * std::sin(2.0 * kPi * 2000.0 * static_cast<double>(i) / kSr));
  }
  return x;
}

std::vector<float> makeTone(double seconds, double hz = 440.0) {
  std::vector<float> x(static_cast<size_t>(std::llround(seconds * kSr)), 0.0f);
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr));
  return x;
}

std::vector<float> renderMono(const Song& s) {
  RenderOptions ro;
  ro.sampleRate = static_cast<int>(kSr);
  AudioBuffer b = renderSong(s, ro);
  std::vector<float> y(b.frames, 0.0f);
  for (size_t i = 0; i < b.frames; ++i) y[i] = 0.5f * (b.at(0, i) + b.at(1, i));
  return y;
}

std::vector<double> detectOnsets(const std::vector<float>& y, float thr = 0.18f) {
  std::vector<double> out;
  const size_t cool = static_cast<size_t>(0.08 * kSr);
  size_t next = 0;
  for (size_t i = 0; i < y.size(); ++i)
    if (i >= next && std::fabs(y[i]) > thr) {
      out.push_back(static_cast<double>(i) / kSr);
      next = i + cool;
    }
  return out;
}

bool beatsMatch(const std::vector<double>& got, const std::vector<double>& want, double tol) {
  if (got.size() < want.size()) return false;
  for (size_t i = 0; i < want.size(); ++i)
    if (std::fabs(got[i] - want[i]) > tol) return false;
  return true;
}

double peakHz(const std::vector<float>& y, size_t from) {
  constexpr size_t N = 16384;
  if (from + N > y.size()) return 0.0;
  std::vector<std::complex<double>> a(N);
  for (size_t i = 0; i < N; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(N));
    a[i] = {static_cast<double>(y[from + i]) * w, 0.0};
  }
  for (size_t i = 1, j = 0; i < N; ++i) {
    size_t b = N >> 1;
    for (; j & b; b >>= 1) j ^= b;
    j ^= b;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= N; len <<= 1) {
    const double ang = -2.0 * kPi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < N; i += len) {
      std::complex<double> w = 1.0;
      for (size_t j = 0; j < len / 2; ++j) {
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
  size_t best = 1;
  double bestMag = 0.0;
  for (size_t k = 1; k < N / 2; ++k) {
    const double m = std::abs(a[k]);
    if (m > bestMag) bestMag = m, best = k;
  }
  const double a0 = std::abs(a[best - 1]), a1 = std::abs(a[best]), a2 = std::abs(a[best + 1]);
  const double denom = a0 - 2.0 * a1 + a2;
  const double delta = std::fabs(denom) > 1e-12 ? 0.5 * (a0 - a2) / denom : 0.0;
  return (static_cast<double>(best) + delta) * kSr / static_cast<double>(N);
}

double centsDiff(double a, double b) {
  if (a <= 0 || b <= 0) return 1e9;
  return 1200.0 * std::log2(a / b);
}

Song makeClipSong(const std::string& wavPath, int mode, double songBpm) {
  Song s = makeEmptySong();
  s.tempo = songBpm;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Loop";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wavPath;
  ch.plugin.params[pp::SmpStretchMode] = static_cast<float>(mode);
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.length = audioClipNaturalLength(s, c);
  s.playlist.push_back(c);
  return s;
}

SamplePtr makeSynthSample(size_t frames, double sr, bool mono) {
  auto s = std::make_shared<Sample>();
  s->sampleRate = sr;
  s->left.resize(frames);
  for (size_t i = 0; i < frames; ++i)
    s->left[i] = static_cast<float>(0.4 * std::sin(2.0 * kPi * 330.0 * static_cast<double>(i) / std::max(1.0, sr)));
  if (mono) {
    s->setMono();
  } else {
    std::vector<float> r(frames);
    for (size_t i = 0; i < frames; ++i)
      r[i] = static_cast<float>(0.35 * std::cos(2.0 * kPi * 330.0 * static_cast<double>(i) / std::max(1.0, sr)));
    s->right = std::move(r);
  }
  return s;
}

void check(bool ok, const std::string& msg) { expect(ok, msg.c_str()); }

// =========================================================================
// PART A: Exhaustive Crash, Fuzz, Race & Zero-Alloc Stress Tests
// =========================================================================

void testPartA_OfflineStretchEdgeAndFuzz() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  // Null / empty / zero-length / negative-length guards.
  {
    std::vector<float> outL(128, 1.0f), outR(128, 1.0f);
    float* out[2] = {outL.data(), outR.data()};
    stretch::Settings set;
    set.mode = stretch::kStretch;
    stretch::process(nullptr, 2, 0, out, 128, kSr, set);
    expect(allFinite(outL.data(), outL.size()) && outL[0] == 0.0f, "process(null in) zeroes output safely");
    stretch::process(nullptr, 2, 100, nullptr, 128, kSr, set);
    stretch::processMapped(nullptr, 2, 0, out, 128, kSr, set, [](int64_t j) { return static_cast<double>(j); });
    expect(allFinite(outL.data(), outL.size()), "processMapped(null in) safe");
    stretch::MappedStream ms(nullptr, 2, 0, out, 128, kSr, [](int64_t j) { return static_cast<double>(j); });
    ms.renderTo(128);
    expect(allFinite(outL.data(), outL.size()), "MappedStream(null in) safe");
  }

  // Tiny (1, 2, 8, 32) and medium (2048) inputs, mono & stereo, extreme ratios & NaN/Inf.
  bool okAll = true;
  for (int nch : {1, 2}) {
    for (int64_t n : {int64_t{1}, int64_t{2}, int64_t{8}, int64_t{32}, int64_t{2048}}) {
      std::vector<float> inL(static_cast<size_t>(n), 0.0f), inR(static_cast<size_t>(n), 0.0f);
      for (int64_t i = 0; i < n; ++i) {
        inL[static_cast<size_t>(i)] = static_cast<float>(0.4 * std::sin(0.07 * static_cast<double>(i)));
        inR[static_cast<size_t>(i)] = static_cast<float>(0.4 * std::cos(0.07 * static_cast<double>(i)));
      }
      const float* in[2] = {inL.data(), nch == 2 ? inR.data() : inL.data()};
      for (stretch::Mode mode : {stretch::Mode::Resample, stretch::kStretch, stretch::Mode::RubberBand}) {
        for (int64_t outN : {int64_t{1}, int64_t{8}, int64_t{256}, std::min<int64_t>(n * 8, 16384)}) {
          for (double pitch : {0.0625, 0.25, 1.0, 4.0, 16.0, 0.0, -2.0, nan, inf}) {
            std::vector<float> oL(static_cast<size_t>(outN), 0.0f), oR(static_cast<size_t>(outN), 0.0f);
            float* out[2] = {oL.data(), oR.data()};
            stretch::Settings set;
            set.mode = mode;
            set.pitch = pitch;
            stretch::process(in, nch, n, out, outN, pitch == 0.0 ? 0.0 : (std::isnan(pitch) ? nan : kSr), set);
            if (!allFinite(oL.data(), oL.size()) || (nch == 2 && !allFinite(oR.data(), oR.size()))) okAll = false;
          }
        }
      }
    }
  }
  expect(okAll, "offline stretch::process finite across tiny/mono/stereo/extreme/NaN/Inf");

  // processMapped with adversarial srcAt (NaN, Inf, huge jumps, negative).
  {
    std::vector<float> inL(1024, 0.25f), inR(1024, -0.25f), oL(512, 0.0f), oR(512, 0.0f);
    const float* in[2] = {inL.data(), inR.data()};
    float* out[2] = {oL.data(), oR.data()};
    bool finite = true;
    for (stretch::Mode mode : {stretch::Mode::Resample, stretch::kStretch, stretch::Mode::RubberBand}) {
      stretch::Settings set;
      set.mode = mode;
      set.pitch = 1.0;
      stretch::processMapped(in, 2, 1024, out, 512, kSr, set, [&](int64_t j) {
        if (j % 17 == 0) return nan;
        if (j % 23 == 0) return inf;
        if (j % 29 == 0) return -1.0e9;
        return static_cast<double>(j) * 1.5;
      });
      if (!allFinite(oL.data(), oL.size()) || !allFinite(oR.data(), oR.size())) finite = false;
    }
    expect(finite, "processMapped survives NaN/Inf/negative srcAt in all 3 modes");
  }

  // ed::stretchAudio with empty, 1-frame, extreme ratios, NaN/Inf.
  {
    ed::Audio empty;
    empty.rate = kSr;
    empty.channels = 2;
    expect(ed::stretchAudio(empty, 2.0, 0.0, ed::Stretch::Elastic, 1).frames() == 0, "ed::stretchAudio empty safe");

    ed::Audio tiny = ed::makeAudio(kSr, 1, 16);
    for (int64_t i = 0; i < 16; ++i) tiny.L[static_cast<size_t>(i)] = tiny.R[static_cast<size_t>(i)] = 0.3f;
    bool edOk = true;
    for (ed::Stretch m : {ed::Stretch::Elastic, ed::Stretch::Tape, ed::Stretch::RubberBand}) {
      for (double tf : {0.05, 0.25, 4.0, 20.0, 0.0, -3.0, nan, inf}) {
        ed::Audio r = ed::stretchAudio(tiny, tf, std::isnan(tf) ? nan : 12.0, m, 1);
        if (!allFinite(r.L.data(), r.L.size()) || !allFinite(r.R.data(), r.R.size())) edOk = false;
      }
    }
    expect(edOk, "ed::stretchAudio survives extreme/NaN/Inf ratios and semitones");
  }
}

void testPartA_RealtimeVoiceFuzzAndZeroAlloc() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  // Unprepared voice must never dereference null or crash.
  {
    stretch::Voice v;
    float oL[64] = {}, oR[64] = {};
    float* out[2] = {oL, oR};
    v.reset();
    expect(v.lookahead(1.0, 1.0) == 0 && v.prerollFor(1.0) == 0 && v.primeLeft(1.0) == 0, "unprepared Voice queries return 0");
    v.start(nullptr, nullptr, 1.0, 1.0, 0);
    v.startLater(nullptr, nullptr, 1.0, 1.0, 0);
    expect(v.prime(nullptr, nullptr, 1.0, 1.0, 512), "unprepared Voice prime returns true");
    v.render(out, 64, 1.0, 1.0, nullptr, nullptr);
    expect(allFinite(oL, 64) && allFinite(oR, 64), "unprepared Voice render safe");
  }

  struct ReaderState {
    const std::vector<float>* src = nullptr;
    size_t idx = 0;
  };
  auto rd = [](void* ctx, float* const* d, int frames) {
    auto* rs = static_cast<ReaderState*>(ctx);
    const size_t n = rs->src->size();
    for (int i = 0; i < frames; ++i) {
      const float v = n ? (*rs->src)[rs->idx % n] : 0.0f;
      d[0][i] = v;
      d[1][i] = v;
      ++rs->idx;
    }
  };
  const std::vector<float> src = makeTone(1.0, 330.0);

  // Fuzz prepare + startLater + prime + render across Stretch (spread=false/true) & RubberBand.
  bool fuzzOk = true;
  for (stretch::Engine eng : {stretch::Engine::Stretch, stretch::Engine::RubberBand}) {
    for (int ch : {1, 2}) {
      for (bool spread : {false, true}) {
        if (eng == stretch::Engine::RubberBand && spread) continue;
        stretch::Voice v;
        v.prepare(ch, kSr, 4.0, 4.0, true, eng, 0.25, kSr, spread);
        float oL[512] = {}, oR[512] = {};
        float* out[2] = {oL, ch == 2 ? oR : nullptr};
        for (double speed : {0.0, 0.1, 0.5, 1.0, 2.0, 4.0, nan, inf, -1.0}) {
          for (double pitch : {0.25, 0.5, 1.0, 2.0, 4.0, nan, inf, 0.0, -1.0}) {
            ReaderState rs{&src, 0};
            const int pre = v.prerollFor(speed);
            v.startLater(rd, &rs, speed, pitch, pre);
            v.prime(rd, &rs, speed, pitch, 1024);
            v.render(out, 256, speed, pitch, rd, &rs);
            v.render(out, 64, 1.0, 1.0, rd, &rs);
            if (!allFinite(oL, 256) || (ch == 2 && !allFinite(oR, 256))) fuzzOk = false;
          }
        }
        // Null reader + invalid block sizes.
        v.start(nullptr, nullptr, 1.0, 1.0, 0);
        v.render(out, 0, 1.0, 1.0, nullptr, nullptr);
        v.render(out, -16, 1.0, 1.0, nullptr, nullptr);
        v.render(nullptr, 64, 1.0, 1.0, nullptr, nullptr);
      }
    }
  }
  expect(fuzzOk, "realtime Voice Stretch & RubberBand finite across extreme/NaN/Inf speeds & pitches");

  // Prepare with invalid sample rates / NaN parameters must still produce a usable voice.
  {
    stretch::Voice v;
    v.prepare(0, nan, inf, -5.0, true, stretch::Engine::Stretch, nan, -10.0, false);
    float oL[64] = {}, oR[64] = {};
    float* out[2] = {oL, oR};
    ReaderState rs{&src, 0};
    v.start(rd, &rs, 1.0, 1.0, 0);
    v.render(out, 64, 1.0, 1.0, rd, &rs);
    expect(allFinite(oL, 64), "Voice::prepare sanitizes NaN/Inf/invalid arguments");
  }

  // Zero audio-thread heap allocations during startLater / prime / render / reset.
#if defined(GL_K431_COUNT_ALLOCS)
  for (stretch::Engine eng : {stretch::Engine::Stretch, stretch::Engine::RubberBand}) {
    stretch::Voice v;
    v.prepare(2, kSr, 4.0, 4.0, true, eng, 0.25, kSr, false);
    float oL[256] = {}, oR[256] = {};
    float* out[2] = {oL, oR};
    // Warm up one cycle so any static tables (sincTable) are already initialized.
    {
      ReaderState rs{&src, 0};
      v.start(rd, &rs, 1.0, 1.0, 0);
      v.render(out, 256, 1.0, 1.0, rd, &rs);
    }
    const long before = gAllocCount.load(std::memory_order_relaxed);
    gCountAllocs = true;
    for (double sp : {0.5, 1.0, 2.0}) {
      ReaderState rs{&src, 0};
      v.startLater(rd, &rs, sp, 1.0, 0);
      v.prime(rd, &rs, sp, 1.0, 2048);
      for (int blk = 0; blk < 8; ++blk) v.render(out, 256, sp, 1.0, rd, &rs);
    }
    v.reset();
    gCountAllocs = false;
    const long allocs = gAllocCount.load(std::memory_order_relaxed) - before;
    char msg[128];
    std::snprintf(msg, sizeof msg, "zero audio-thread heap allocations in %s Voice (got %ld)",
                  eng == stretch::Engine::RubberBand ? "Rubber Band" : "Stretch", allocs);
    expect(allocs == 0, msg);
  }
#endif
}

void testPartA_RenderClipAndPoolRaceStress() {
  const double nan = std::numeric_limits<double>::quiet_NaN();

  // Huge frame count / invalid request rejected cleanly (no OOM crash).
  {
    SamplePtr smp = makeSynthSample(44100, kSr, true);
    stretch::ClipRequest rq;
    rq.sample = smp;
    rq.srcStart = 0.0;
    rq.srcEnd = 1.0;
    rq.rate = kSr;
    rq.frames = int64_t{1} << 40;
    rq.mode = stretch::kStretch;
    expect(stretch::renderClip(rq, false) == nullptr, "renderClip rejects huge frame count cleanly (no OOM crash)");

    rq.frames = 1024;
    rq.rate = nan;
    expect(stretch::renderClip(rq, false) == nullptr, "renderClip rejects NaN rate cleanly");

    // 1-frame sample rejected cleanly.
    rq.sample = makeSynthSample(1, kSr, true);
    rq.rate = kSr;
    expect(stretch::renderClip(rq, false) == nullptr, "renderClip rejects 1-frame sample cleanly");
  }

  // Mono async renderClip concurrency (verifies removed post-publish mono std::copy data race).
  {
    SamplePtr mono = makeSynthSample(static_cast<size_t>(kSr * 0.5), kSr, true);
    std::vector<stretch::RenderedClipPtr> clips;
    for (int k = 0; k < 8; ++k) {
      stretch::ClipRequest rq;
      rq.sample = mono;
      rq.srcStart = 0.0;
      rq.srcEnd = 0.5;
      rq.rate = kSr;
      rq.speed = 0.6 + 0.15 * k;
      rq.pitch = 1.0;
      rq.frames = static_cast<int64_t>(std::ceil(0.5 / rq.speed * kSr));
      rq.mode = (k % 2 == 0) ? stretch::kStretch : stretch::Mode::RubberBand;
      if (auto rc = stretch::renderClip(rq, true)) clips.push_back(rc);
    }
    bool readerFinite = true;
    for (int spin = 0; spin < 200; ++spin) {
      bool allReady = true;
      for (const auto& rc : clips) {
        const int64_t rf = rc->readyFrames.load(std::memory_order_acquire);
        for (int64_t i = 0; i < rf; i += 64) {
          if (!std::isfinite(rc->L[static_cast<size_t>(i)]) || !std::isfinite(rc->R[static_cast<size_t>(i)])) readerFinite = false;
        }
        if (!rc->ready.load(std::memory_order_acquire)) allReady = false;
      }
      if (allReady) break;
      std::this_thread::yield();
    }
    stretch::waitForPendingRenders();
    expect(readerFinite && clips.size() == 8, "mono async renderClip concurrent read finite and race-free");
    stretch::trimClipCache();
  }

  // SamplerGen & SlicerGen concurrent prepareHost <-> configure/process stress (pool swap UAF test)
  // + replacing sample with empty/1-frame sample mid-playback.
  for (const char* type : {"gl.sampler", "gl.slicer"}) {
    auto gen = createGenerator(type);
    check(gen != nullptr, std::string("created generator ") + type);
    if (!gen) continue;

    SamplePtr normal = makeSynthSample(8192, kSr, false);
    SamplePtr tiny1 = makeSynthSample(1, kSr, true);
    SamplePtr empty = makeSynthSample(0, kSr, true);

    PluginState base = defaultPluginState(type);
    std::atomic<bool> stop{false};
    std::atomic<int> modeIdx{1};

    // UI thread: rapidly toggles Stretch <-> Rubber Band (or sample rate on Slicer) and calls prepareHost().
    std::thread ui([&]() {
      PluginState st = base;
      for (int iter = 0; iter < 120 && !stop.load(std::memory_order_relaxed); ++iter) {
        const int m = (iter % 2 == 0) ? 1 : 7;
        modeIdx.store(m, std::memory_order_relaxed);
        double hostSr = kSr;
        if (std::string(type) == "gl.sampler") {
          st.params[pp::SmpStretchMode] = static_cast<float>(m);
          st.params[pp::SmpTimeStretch] = 1.5f;
          st.params[pp::SmpVoices] = 4.0f;
        } else {
          st.params[pp::SlFitTempo] = 1.0f;
          st.params[pp::SlFitKeepPitch] = 1.0f;
          hostSr = (iter % 2 == 0) ? 44100.0 : 48000.0;
        }
        gen->prepareHost(st, hostSr);
        std::this_thread::yield();
      }
      stop.store(true, std::memory_order_relaxed);
    });

    // Audio thread: triggers notes, swaps samples including empty/1-frame, processes.
    std::vector<float> L(128, 0.0f), R(128, 0.0f);
    bool finite = true;
    PluginState st = base;
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.tempo = 120.0;
    ctx.playing = true;
    int block = 0;
    uint32_t nextNoteId = 1;
    while (!stop.load(std::memory_order_relaxed) || block < 40) {
      const int m = modeIdx.load(std::memory_order_relaxed);
      if (std::string(type) == "gl.sampler") {
        st.params[pp::SmpStretchMode] = static_cast<float>(m);
        st.params[pp::SmpTimeStretch] = 1.5f;
        st.params[pp::SmpVoices] = 4.0f;
      } else {
        st.params[pp::SlFitTempo] = 1.0f;
        st.params[pp::SlFitKeepPitch] = 1.0f;
      }
      ChannelAux aux;
      if (block % 29 == 0) aux.samples = {empty};
      else if (block % 29 == 1) aux.samples = {tiny1};
      else aux.samples = {normal};
      if (std::string(type) == "gl.slicer") aux.slices = {0, 2048, 4096, 6144};
      ctx.tempo = 40.0 + static_cast<double>((block * 17) % 260);
      if (block % 4 == 0) gen->noteOn(nextNoteId++, 60.0 + (block % 7), 0.8f, 0);
      if (block % 7 == 0 && nextNoteId > 2) gen->noteOff(nextNoteId - 2, 0);
      std::fill(L.begin(), L.end(), 0.0f);
      std::fill(R.begin(), R.end(), 0.0f);
      gen->process(st, aux, ctx, L.data(), R.data(), 128);
      if (!allFinite(L.data(), L.size()) || !allFinite(R.data(), R.size())) finite = false;
      ++block;
      if (block > 800) break;
    }
    stop.store(true, std::memory_order_relaxed);
    ui.join();
    check(finite, std::string(type) + " concurrent prepareHost/process + empty/1-frame swap race-free & finite");
  }
}

void testPartA_EngineScrubDeleteUndoAndGuards(const std::string& tmpDir) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  // ClipRate NaN/Inf/extreme guards.
  {
    Channel ch;
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.params[pp::SmpStretchMode] = 1.0f;
    ch.plugin.params[pp::SmpTimeStretch] = static_cast<float>(nan);
    ch.plugin.params[pp::SmpPitchShift] = static_cast<float>(inf);
    Clip c;
    c.kind = ClipKind::Audio;
    c.stretch = nan;
    c.rateFit = -inf;
    c.pitch = static_cast<float>(nan);
    c.speedPitch = static_cast<float>(inf);
    const ClipRate r = clipRate(ch, c);
    expect(std::isfinite(r.speed) && r.speed > 0 && std::isfinite(r.pitch) && r.pitch > 0 &&
               std::isfinite(r.timeFactor) && r.timeFactor > 0,
           "clipRate produces finite positive values on NaN/Inf inputs");
  }

  // AudioEngine live playback stress: rapid BPM scrubbing + mode toggling + clip delete/undo.
  const std::string wav = writeWavFile(tmpDir, "stress_loop.wav", makeClickLoop(120.0, 4));
  Song s = makeClipSong(wav, 1, 120.0);
  const ObjectId chId = s.channels[0].id;
  const ObjectId clipId = s.playlist[0].id;
  Document doc(s);

  AudioEngine eng(kSr);
  ProgramBuilder pb;
  pb.setSampleRate(kSr);
  eng.setMode(PlayMode::Song);
  eng.setProgram(pb.build(doc.song()));
  eng.play();

  std::vector<float> outL(256, 0.0f), outR(256, 0.0f);
  bool engineFinite = true;
  const double bpms[] = {40.0, 300.0, 60.0, 185.0, 95.0, 240.0, 120.0};
  const int modes[] = {0, 1, 7, 1, 0, 7, 1};
  for (int step = 0; step < 21; ++step) {
    doc.setParam(channelPluginParam(chId, pp::SmpStretchMode), modes[step % 7], false);
    doc.setParam(songParam(SongParam::Tempo), bpms[step % 7], true);
    if (step == 10) doc.removeClip(clipId);
    if (step == 12) doc.undo();
    eng.setProgram(pb.build(doc.song()));
    for (int b = 0; b < 4; ++b) {
      std::fill(outL.begin(), outL.end(), 0.0f);
      std::fill(outR.begin(), outR.end(), 0.0f);
      eng.process(outL.data(), outR.data(), 256);
      if (!allFinite(outL.data(), outL.size()) || !allFinite(outR.data(), outR.size())) engineFinite = false;
    }
    eng.collectGarbage();
  }
  doc.endGesture();
  eng.stop();
  stretch::waitForPendingRenders();
  expect(engineFinite, "AudioEngine survives rapid BPM scrubbing, mode switches, and clip delete/undo mid-playback");

  // ClipOps NaN/Inf guards.
  {
    Song g = makeClipSong(wav, 1, 120.0);
    const ObjectId id = g.playlist[0].id;
    expect(!setClipStretchMode(g, id, nan, 1), "setClipStretchMode rejects NaN timeFactor");
    expect(!setClipStretchMode(g, id, -1.0, 1), "setClipStretchMode rejects negative timeFactor");
    expect(normalizeClip(g, id, nan) == 0.0f, "normalizeClip rejects NaN targetDb");
    expect(renderClipStretch(g, id, nan, 0.0, 0, tmpDir).empty(), "renderClipStretch rejects NaN timeFactor");
    expect(renderClipStretch(g, id, 1.5, inf, 0, tmpDir).empty(), "renderClipStretch rejects Inf semitones");
  }
}

// =========================================================================
// PART B: FL-Style Stretch & BPM Grid-Lock Behaviour Across Tempo Changes
// =========================================================================

void testPartB_StretchAndRubberBandLockGrid(const std::string& tmpDir) {
  const std::string loopWav = writeWavFile(tmpDir, "loop_120.wav", makeClickLoop(120.0, 4));
  const std::string toneWav = writeWavFile(tmpDir, "tone_120.wav", makeTone(2.0, 440.0));

  for (int mode : {1, 7}) {
    const char* mn = mode == 1 ? "Stretch" : "Rubber Band";

    // Entry point 1: Document::setParam(SmpStretchMode) (Sampler / ClipEditor / poke engine).
    {
      Document d(makeClipSong(loopWav, 0, 120.0));
      const ObjectId chId = d.song().channels[0].id;
      const ObjectId cid = d.song().playlist[0].id;
      const Tick origLen = d.song().clip(cid)->length;
      check(origLen == 384, std::string(mn) + ": 2s loop at 120 BPM starts at 1 bar (384 ticks)");

      d.setParam(channelPluginParam(chId, pp::SmpStretchMode), mode, false);
      const Clip* c1 = d.song().clip(cid);
      check(c1 && c1->tempoLock && std::fabs(c1->syncBpm - 120.0) < 1e-4 && c1->length == origLen,
            std::string(mn) + " via setParam locks clip to 120 BPM grid");
      check(std::fabs(d.song().channel(chId)->plugin.params[pp::SmpFitTempo] - 120.0f) < 1e-3f,
            std::string(mn) + " syncs channel SmpFitTempo readout");

      for (double bpm : {145.0, 90.0}) {
        d.setParam(songParam(SongParam::Tempo), bpm, false);
        const Clip* ck = d.song().clip(cid);
        TempoMap tm;
        tm.build(d.song());
        const double srcEnd = clipSourceAt(d.song(), *ck, tm, static_cast<double>(ck->start + ck->length));
        check(ck->length == origLen && std::fabs(srcEnd - 2.0) < 1e-4,
              std::string(mn) + " keeps exact 384 ticks and 2.0s waveform span at " + std::to_string(static_cast<int>(bpm)) + " BPM");
        std::vector<double> want;
        for (int b = 0; b < 4; ++b) want.push_back(b * 60.0 / bpm);
        check(beatsMatch(detectOnsets(renderMono(d.song())), want, 0.015),
              std::string(mn) + " rendered beat onsets lock to grid at " + std::to_string(static_cast<int>(bpm)) + " BPM");
      }

      // Pitch preservation check on a 440 Hz tone.
      Document dt(makeClipSong(toneWav, 0, 120.0));
      dt.setParam(channelPluginParam(dt.song().channels[0].id, pp::SmpStretchMode), mode, false);
      dt.setParam(songParam(SongParam::Tempo), 150.0, false);
      const double hz = peakHz(renderMono(dt.song()), static_cast<size_t>(0.3 * kSr));
      check(std::fabs(centsDiff(hz, 440.0)) < 6.0, std::string(mn) + " preserves 440 Hz pitch at 150 BPM");
    }

    // Entry point 2: setClipStretchMode (ClipEditor clip view & Playlist clip context menu).
    {
      Document d(makeClipSong(loopWav, 0, 120.0));
      const ObjectId cid = d.song().playlist[0].id;
      d.editSong("stretch mode", [&](Song& xs) { setClipStretchMode(xs, cid, 1.0, mode); });
      d.setParam(songParam(SongParam::Tempo), 145.0, false);
      check(d.song().clip(cid)->length == 384 && d.song().clip(cid)->tempoLock,
            std::string(mn) + " via setClipStretchMode stays locked at 145 BPM");
    }

    // Entry point 3: ops::setAudioClipTimeMode (Tools / Macros menu).
    {
      Document d(makeClipSong(loopWav, 0, 120.0));
      const ObjectId cid = d.song().playlist[0].id;
      check(ops::setAudioClipTimeMode(d, {cid}, mode) == 1, std::string(mn) + " via ops::setAudioClipTimeMode switches channel");
      d.setParam(songParam(SongParam::Tempo), 90.0, false);
      check(d.song().clip(cid)->length == 384 && d.song().clip(cid)->tempoLock,
            std::string(mn) + " via ops::setAudioClipTimeMode stays locked at 90 BPM");
    }
  }
}

void testPartB_DialSampleBpmAndFitToTempo(const std::string& tmpDir) {
  // 128 BPM 4-beat loop = 1.875 s; dropped at 120 BPM it starts as 360 ticks (3.75 beats).
  const std::string loop128 = writeWavFile(tmpDir, "loop_128.wav", makeClickLoop(128.0, 4));
  const std::string tone128 = writeWavFile(tmpDir, "tone_128.wav", makeTone(1.875, 440.0));

  {
    Document d(makeClipSong(loop128, 0, 120.0));
    const ObjectId chId = d.song().channels[0].id;
    const ObjectId cid = d.song().playlist[0].id;
    expect(d.song().clip(cid)->length == 360, "128 BPM loop at 120 BPM starts at 360 ticks (3.75 beats)");

    // User dials in 128 BPM via SmpFitTempo drag (merges into one undo step!).
    const size_t h0 = d.history().position();
    for (double v : {120.0, 124.0, 128.0})
      d.setParam(channelPluginParam(chId, pp::SmpFitTempo), v, true);
    d.endGesture();
    expect(d.history().position() == h0 + 1, "dialing BPM merges drag into a single undo step");

    const Channel* ch = d.song().channel(chId);
    const Clip* c = d.song().clip(cid);
    expect(ch && ch->plugin.params[pp::SmpStretchMode] == static_cast<float>(stretch::kStretch),
           "dialing BPM on Resample channel switches mode to Stretch");
    expect(c && c->tempoLock && std::fabs(c->syncBpm - 128.0) < 1e-4 && c->length == 384,
           "dialing 128 BPM snaps 1.875 s loop to exact 1 bar (384 ticks) and locks to tempo");

    // Change project BPM 120 -> 150 -> 100: clip stays exact 384 ticks & onsets lock to beats!
    for (double bpm : {150.0, 100.0}) {
      d.setParam(songParam(SongParam::Tempo), bpm, false);
      check(d.song().clip(cid)->length == 384, "BPM-dialed clip keeps exact 384 ticks at " + std::to_string(static_cast<int>(bpm)) + " BPM");
      std::vector<double> want;
      for (int b = 0; b < 4; ++b) want.push_back(b * 60.0 / bpm);
      check(beatsMatch(detectOnsets(renderMono(d.song())), want, 0.015),
            "BPM-dialed clip beats lock to grid at " + std::to_string(static_cast<int>(bpm)) + " BPM");
    }

    // Dialing BPM back to 0 unlocks cleanly (fixed tempo again).
    d.setParam(channelPluginParam(chId, pp::SmpFitTempo), 0.0, false);
    const Clip* cu = d.song().clip(cid);
    expect(cu && cu->syncBpm == 0.0 && !cu->tempoLock, "dialing BPM back to 0 unlocks clip");
    // At 100 BPM, 1.875 s = 3.125 beats = 300 ticks.
    expect(cu && cu->length == 300, "unlocked clip returns to natural seconds at current tempo (300 ticks at 100 BPM)");

    // Undo restores locked 384-tick clip!
    d.undo();
    expect(d.song().clip(cid)->length == 384 && d.song().clip(cid)->tempoLock &&
               std::fabs(d.song().clip(cid)->syncBpm - 128.0) < 1e-4,
           "undo restores BPM-locked clip cleanly");
  }

  // Pitch stays constant when dialing BPM on a 440 Hz tone + changing project BPM.
  {
    Document d(makeClipSong(tone128, 0, 120.0));
    d.setParam(channelPluginParam(d.song().channels[0].id, pp::SmpFitTempo), 128.0, false);
    d.setParam(songParam(SongParam::Tempo), 150.0, false);
    const double hz = peakHz(renderMono(d.song()), static_cast<size_t>(0.3 * kSr));
    expect(std::fabs(centsDiff(hz, 440.0)) < 6.0, "BPM-dialed sample preserves 440 Hz pitch at 150 BPM");
  }

  // Zero drift when dragging BPM back and forth on a trimmed (cut) clip.
  {
    Document d(makeClipSong(loop128, 0, 120.0));
    const ObjectId chId = d.song().channels[0].id;
    const ObjectId cid = d.song().playlist[0].id;
    // Trim clip to exact half (180 ticks = 0.9375 s at 120 BPM).
    d.editClip(cid, "trim", [](Clip& c) { c.length = 180; });
    for (double v : {128.0, 140.0, 100.0, 128.0})
      d.setParam(channelPluginParam(chId, pp::SmpFitTempo), v, true);
    d.endGesture();
    // 0.9375 s at 128 BPM = exact 2 beats = 192 ticks, zero drift.
    expect(d.song().clip(cid)->length == 192 && std::fabs(d.song().clip(cid)->audioEnd - 0.9375) < 1e-6,
           "dragging BPM on trimmed clip preserves exact cut with zero drift (192 ticks)");
  }

  // fitClipToTempo switches Resample -> Stretch, syncs SmpFitTempo, and keeps pitch & bars on BPM change.
  {
    Document d(makeClipSong(tone128, 0, 120.0));
    const ObjectId chId = d.song().channels[0].id;
    const ObjectId cid = d.song().playlist[0].id;
    int beats = 0;
    d.editSong("fit clip to tempo", [&](Song& xs) { beats = fitClipToTempo(xs, cid); });
    expect(beats == 4 && d.song().clip(cid)->length == 384 && d.song().clip(cid)->tempoLock,
           "fitClipToTempo snaps 128 BPM sample to 4 beats (384 ticks) and locks");
    expect(d.song().channel(chId)->plugin.params[pp::SmpStretchMode] == static_cast<float>(stretch::kStretch) &&
               std::fabs(d.song().channel(chId)->plugin.params[pp::SmpFitTempo] - 128.0f) < 1e-2f,
           "fitClipToTempo switches Resample -> Stretch and syncs SmpFitTempo");
    d.setParam(songParam(SongParam::Tempo), 150.0, false);
    const double hz = peakHz(renderMono(d.song()), static_cast<size_t>(0.3 * kSr));
    expect(d.song().clip(cid)->length == 384 && std::fabs(centsDiff(hz, 440.0)) < 6.0,
           "fitClipToTempo clip stays 1 bar & preserves pitch at 150 BPM");
  }
}

void testPartB_PlaylistDragTimeKnobAutomationRoundTrip(const std::string& tmpDir) {
  const std::string loopWav = writeWavFile(tmpDir, "loop_drag.wav", makeClickLoop(120.0, 4));

  // Playlist Stretch-drag clip from 1 bar (384 ticks) to 2 bars (768 ticks) -> stays 2 bars across BPM changes.
  {
    Document d(makeClipSong(loopWav, 1, 120.0));
    const ObjectId cid = d.song().playlist[0].id;
    d.editSong("stretch drag", [&](Song& xs) {
      TempoMap tm;
      tm.build(xs);
      xs.playlist = pltools::resizeEnds(xs, xs.playlist, {cid}, 384, true, &tm, false);
    });
    expect(d.song().clip(cid)->length == 768 && d.song().clip(cid)->tempoLock, "stretch-dragged clip is 2 bars and locked");
    for (double bpm : {155.0, 85.0, 120.0}) {
      d.setParam(songParam(SongParam::Tempo), bpm, false);
      check(d.song().clip(cid)->length == 768,
            "stretch-dragged 2-bar clip stays exactly 768 ticks at " + std::to_string(static_cast<int>(bpm)) + " BPM");
    }
  }

  // Sampler TIME knob (SmpTimeStretch = 2.0) doubles clip length to 2 bars & locks across BPM changes.
  {
    Document d(makeClipSong(loopWav, 1, 120.0));
    const ObjectId chId = d.song().channels[0].id;
    const ObjectId cid = d.song().playlist[0].id;
    d.setParam(channelPluginParam(chId, pp::SmpTimeStretch), 2.0, false);
    expect(d.song().clip(cid)->length == 768 && d.song().clip(cid)->tempoLock,
           "Sampler TIME knob x2 stretches clip to 2 bars and locks to tempo");
    d.setParam(songParam(SongParam::Tempo), 150.0, false);
    expect(d.song().clip(cid)->length == 768, "TIME-stretched clip stays exact 2 bars at 150 BPM");
    d.undo();
    d.undo();
    expect(d.song().clip(cid)->length == 384 && !d.song().clip(cid)->tempoLock &&
               std::fabs(d.song().channel(chId)->plugin.params[pp::SmpTimeStretch] - 1.0f) < 1e-5f,
           "undo restores TIME knob and clip length cleanly");
  }

  // ClipFields FollowTempo keeps SmpFitTempo in sync & supports undo.
  {
    Document d(makeClipSong(loopWav, 1, 120.0));
    const ObjectId chId = d.song().channels[0].id;
    const ObjectId cid = d.song().playlist[0].id;
    expect(clipfields::set(d, {cid}, cid, clipfields::Field::FollowTempo, 1.0, 0), "ClipFields FollowTempo on");
    expect(d.song().clip(cid)->tempoLock && d.song().channel(chId)->plugin.params[pp::SmpFitTempo] > 0.0f,
           "ClipFields FollowTempo on locks clip and syncs SmpFitTempo");
    expect(clipfields::set(d, {cid}, cid, clipfields::Field::FollowTempo, 0.0, 0), "ClipFields FollowTempo off");
    expect(!d.song().clip(cid)->tempoLock && d.song().channel(chId)->plugin.params[pp::SmpFitTempo] == 0.0f,
           "ClipFields FollowTempo off unlocks clip and clears SmpFitTempo");
  }

  // Tempo automation ramp (120 -> 150 BPM) across a Stretch-locked clip tracks TempoMap.
  {
    Document d(makeClipSong(loopWav, 0, 120.0));
    const ObjectId chId = d.song().channels[0].id;
    const ObjectId cid = d.song().playlist[0].id;
    d.setParam(channelPluginParam(chId, pp::SmpStretchMode), 1, false);
    auto td = describeParam(d.song(), songParam(SongParam::Tempo));
    d.editSong("tempo automation", [&](Song& xs) {
      AutomationClip ac;
      ac.id = xs.allocateId();
      ac.owner = kSongId;
      ac.index = static_cast<uint32_t>(SongParam::Tempo);
      ac.points = {{0, static_cast<float>(td->toNormalized(120.0)), 0, AutoShape::Hold},
                   {static_cast<Tick>(2 * xs.ppq), static_cast<float>(td->toNormalized(150.0)), 0, AutoShape::Hold}};
      xs.automations.push_back(ac);
      Clip c;
      c.id = xs.allocateId();
      c.kind = ClipKind::Automation;
      c.source = ac.id;
      c.track = 1;
      c.length = 4 * xs.ppq;
      xs.playlist.push_back(c);
    });
    TempoMap tm;
    tm.build(d.song());
    std::vector<double> want;
    for (int b = 0; b < 4; ++b) want.push_back(tm.secondsAt(static_cast<double>(b * d.song().ppq)));
    expect(d.song().clip(cid)->length == 384 && beatsMatch(detectOnsets(renderMono(d.song())), want, 0.015),
           "tempo automation 120 -> 150 BPM keeps locked Stretch clip on TempoMap beats");
  }

  // JSON Save/Load + Collab clip/song JSON + FLP Export/Import round-trip for Resample, Stretch, Rubber Band.
  for (int mode : {0, 1, 7}) {
    Document d(makeClipSong(loopWav, 0, 120.0));
    const ObjectId chId = d.song().channels[0].id;
    const ObjectId cid = d.song().playlist[0].id;
    if (mode == 0) {
      d.editSong("stretch resample", [&](Song& xs) { setClipStretch(xs, cid, 1.5, false); });
    } else {
      d.setParam(channelPluginParam(chId, pp::SmpStretchMode), mode, false);
      d.setParam(channelPluginParam(chId, pp::SmpFitTempo), 128.0, false);
    }
    const Song orig = d.song();
    // Full Song JSON round-trip (and per-element Collab clipFromJson / channelToJson).
    const Json songJ = songToJson(orig);
    Song fromJson;
    check(songFromJson(songJ, fromJson), "JSON round-trip succeeds for mode " + std::to_string(mode));
    check(fromJson.playlist.size() == orig.playlist.size() && fromJson.playlist[0] == orig.playlist[0] &&
              fromJson.channels[0].plugin.params == orig.channels[0].plugin.params,
          "JSON round-trip preserves syncBpm, tempoLock, stretch, mode & SmpFitTempo for mode " + std::to_string(mode));

    // Collab element-by-element round-trip (clipFromJson + channelToJson/channelFromJson).
    const auto plIt = songJ.object.find("playlist");
    Clip collabClip;
    collabClip.id = orig.playlist[0].id;  // caller sets id before clipFromJson (Project.cpp:2386)
    check(plIt != songJ.object.end() && !plIt->second.array.empty() &&
              clipFromJson(plIt->second.array[0], collabClip, fromJson) == 0 && collabClip == orig.playlist[0],
          "Collab clipFromJson preserves tempoLock & syncBpm for mode " + std::to_string(mode));
    Channel collabCh;
    channelFromJson(channelToJson(orig.channels[0]), collabCh, fromJson);
    check(collabCh.plugin.params == orig.channels[0].plugin.params,
          "Collab channelToJson/channelFromJson preserves stretch mode & SmpFitTempo for mode " + std::to_string(mode));

    // FLP Export -> Import round-trip preserves valid song without crashing.
    flp::ExportOptions opt;
    opt.useOrigin = false;
    opt.projectDir = tmpDir;
    std::vector<uint8_t> flpBytes;
    flp::ExportReport er;
    std::string err;
    check(flp::exportFlp(orig, flpBytes, er, opt), "FLP export succeeds for mode " + std::to_string(mode));
    Song fromFlp;
    flp::Report ir;
    check(importFlpBytes(flpBytes, fromFlp, ir, &err, "", tmpDir, false) && !fromFlp.playlist.empty(),
          "FLP import round-trip succeeds for mode " + std::to_string(mode));
  }
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k431_audio_stretch");
  const fs::path tmp = fs::temp_directory_path() / "gl_k431_audio_stretch";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);

  testPartA_OfflineStretchEdgeAndFuzz();
  testPartA_RealtimeVoiceFuzzAndZeroAlloc();
  testPartA_RenderClipAndPoolRaceStress();
  testPartA_EngineScrubDeleteUndoAndGuards(tmp.string());

  testPartB_StretchAndRubberBandLockGrid(tmp.string());
  testPartB_DialSampleBpmAndFitToTempo(tmp.string());
  testPartB_PlaylistDragTimeKnobAutomationRoundTrip(tmp.string());

  stretch::shutdownClipRenderer();
  fs::remove_all(tmp, ec);
  return finish();
}
